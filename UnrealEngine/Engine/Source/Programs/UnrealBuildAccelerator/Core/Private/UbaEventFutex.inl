// Copyright Epic Games, Inc. All Rights Reserved.

#if PLATFORM_LINUX

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

namespace uba
{
	// EventFutex
	//
	// A SharedEvent implementation that uses ONLY raw Linux futex syscalls plus
	// clock_gettime(CLOCK_MONOTONIC). No pthread, no libc containers.
	//
	// The point of this implementation is to be callable from a freestanding
	// stub that has no libpthread, while still being compatible with the
	// UBA-side SharedEvent layout. Because the object lives in cross-process
	// shared memory, all futex calls use the non-private ops (FUTEX_WAIT /
	// FUTEX_WAKE). Non-private futexes look at the underlying page of the
	// mapping rather than the VMA, so a waiter in process A can be woken by
	// a Set() in process B as long as both map the same SHM page.
	//
	// State machine (single u32 futex word `m_state`):
	//
	//   Reset     = 0  (no waiters)
	//   Waiting   = 1  (at least one waiter may be parked in FUTEX_WAIT)
	//   Signaled  = 2  (manual-reset only; Set was called, waiters pass through)
	//   Destroyed = 3  (Destroy() was called; all waits return false)
	//
	// Auto-reset transitions:
	//   Set()      : CAS from {Reset,Waiting}->Reset then FUTEX_WAKE(1). This
	//                hands the "signal" to exactly one waiter: the waiter that
	//                observes a transition through Reset->(woken)-> consume.
	//                Concretely we keep a u32 counter `m_autoCount` that Set()
	//                increments and waiters decrement with CAS.
	//
	// Manual-reset transitions:
	//   Set()      : store Signaled, FUTEX_WAKE(INT_MAX).
	//   (Destroy clears and wakes; a subsequent Create re-initializes.)
	//
	// Waiter loop: load state, if it indicates a signal is available consume
	// it (auto) or return true (manual). Otherwise FUTEX_WAIT on `m_state`
	// with the expected value and loop on EAGAIN / spurious wake / EINTR.
	//
	// Cross-process notes:
	//  - Atomic ops on a u32 in shared memory are well defined on Linux x86_64
	//    and aarch64 as long as the field is naturally aligned (it is; u32 at
	//    offset after a u8 tag byte plus padding).
	//  - FUTEX_WAIT compares *uaddr == val with kernel-acquired atomicity, so
	//    the classic "lost wakeup" window between state load and futex-wait is
	//    closed by the kernel itself.
	//  - Memory ordering: Set() uses release on the store, waiters use acquire
	//    on the load. FUTEX_WAKE implies a release; FUTEX_WAIT implies an
	//    acquire on wake-up. This matches glibc's own cond-var sequencing.

	enum EventFutexState : u32
	{
		EventFutexState_Reset     = 0,
		EventFutexState_Signaled  = 2,
		EventFutexState_Destroyed = 3,
	};

	struct EventFutex : EventHeader
	{
		// Stamp tag 3 at offset 0 so cross-process consumers (notably the
		// static-detour stub's SharedEvent::Set/IsSet sanity check) can
		// identify EventFutex slots in shm without any higher-level handshake.
		// SharedEvent::Destroy zeroes this byte again when the slot is torn down.
		EventFutex() { m_resetType.padding = 3; }
		~EventFutex() { Destroy(); }

		bool Create(EventResetType rt, bool /*shared*/ = true)
		{
			static_assert(Atomic<u32>::is_always_lock_free, "atomic<u32> not lock free; can't live in shared mem");

			UBA_ASSERTF(!m_initialized.load(std::memory_order_acquire), "EventFutex already created");

			m_resetType.value = rt;
			m_state.store(EventFutexState_Reset, std::memory_order_relaxed);
			m_autoCount.store(0, std::memory_order_relaxed);
			m_initialized.store(true, std::memory_order_release);
			return true;
		}

		void Destroy()
		{
			if (!m_initialized.exchange(false, std::memory_order_acq_rel))
				return;

			// Transition to Destroyed and wake every parked waiter so they exit
			// their loops with `false`. Use non-private op: waiters may live
			// in other processes sharing the mapping.
			m_state.store(EventFutexState_Destroyed, std::memory_order_release);
			FutexWakeAll(&m_state);

			// Also kick the auto-reset counter in case a waiter is parked on it.
			m_autoCount.fetch_add(1, std::memory_order_release);
			FutexWakeAll(&m_autoCount);
		}

		void Set()
		{
			if (!m_initialized.load(std::memory_order_acquire))
				return;

			if (m_resetType.value == EventResetType_Manual)
			{
				// Manual reset: latch the signal, release everybody.
				u32 prev = m_state.exchange(EventFutexState_Signaled, std::memory_order_release);
				if (prev != EventFutexState_Signaled)
					FutexWakeAll(&m_state);
				return;
			}

			// Auto reset: publish a new token and hand it to one waiter.
			// m_autoCount serves both as the futex word and as a monotonic
			// counter of Set() calls. A waiter reads the count, parks on it,
			// and on wake tries to CAS one unit back out.
			m_autoCount.fetch_add(1, std::memory_order_release);
			FutexWakeOne(&m_autoCount);
		}

		bool IsSet(u32 timeoutMs = ~0u)
		{
			if (!m_initialized.load(std::memory_order_acquire))
				return false;

			if (m_resetType.value == EventResetType_Manual)
				return WaitManual(timeoutMs);
			else
				return WaitAuto(timeoutMs);
		}

		// Reset() is required by the local-event dispatcher (Event::Reset,
		// Event::Create's pool-reuse path). Manual-reset: clear any latched
		// signal. Auto-reset: drain outstanding tokens. No-op if Destroyed;
		// never wakes anyone (waiters only care about Signaled/Destroyed).
		void Reset()
		{
			if (!m_initialized.load(std::memory_order_acquire))
				return;

			if (m_resetType.value == EventResetType_Manual)
			{
				// CAS Signaled->Reset. If another thread is mid-Set or the
				// event is already Reset/Destroyed, leave m_state alone.
				u32 expected = EventFutexState_Signaled;
				m_state.compare_exchange_strong(expected, EventFutexState_Reset,
					std::memory_order_acq_rel, std::memory_order_relaxed);
			}
			else
			{
				// Auto-reset: drop outstanding tokens. Waiters parked in
				// WaitAuto are fine — they either haven't observed the token
				// yet (will re-load 0 and re-park) or already consumed it.
				m_autoCount.store(0, std::memory_order_release);
			}
		}

		void* GetHandle()
		{
			return nullptr;
		}

	private:
		bool WaitManual(u32 timeoutMs)
		{
			// Deadline in absolute CLOCK_MONOTONIC nanoseconds, or 0 for
			// "infinite" sentinel.
			u64 deadlineNs = 0;
			if (timeoutMs != ~0u && timeoutMs != 0)
				deadlineNs = MonotonicNs() + u64(timeoutMs) * 1'000'000ull;

			for (;;)
			{
				u32 s = m_state.load(std::memory_order_acquire);
				if (s == EventFutexState_Signaled)
					return true;
				if (s == EventFutexState_Destroyed)
					return false;

				if (timeoutMs == 0)
					return false;

				struct timespec ts;
				const struct timespec* tsPtr = nullptr;
				if (timeoutMs != ~0u)
				{
					u64 nowNs = MonotonicNs();
					if (nowNs >= deadlineNs)
						return false;
					ts = NsToTimespec(deadlineNs - nowNs);
					tsPtr = &ts;
				}

				// FUTEX_WAIT: kernel re-checks *uaddr == s atomically, which
				// closes the load-then-wait race. EAGAIN means the word
				// already changed — just loop.
				long r = syscall(SYS_futex, &m_state, FUTEX_WAIT, s, tsPtr, nullptr, 0);
				if (r == 0)
					continue;
				int e = errno;
				if (e == ETIMEDOUT)
					return false;
				if (e == EAGAIN || e == EINTR)
					continue;
				// Any other error: treat as wake and re-check state.
			}
		}

		bool WaitAuto(u32 timeoutMs)
		{
			u64 deadlineNs = 0;
			if (timeoutMs != ~0u && timeoutMs != 0)
				deadlineNs = MonotonicNs() + u64(timeoutMs) * 1'000'000ull;

			for (;;)
			{
				// Every check also has to observe Destroyed, else we might
				// sleep forever against a dead event.
				if (m_state.load(std::memory_order_acquire) == EventFutexState_Destroyed)
					return false;

				u32 c = m_autoCount.load(std::memory_order_acquire);
				if (c != 0)
				{
					// Try to consume one token.
					if (m_autoCount.compare_exchange_strong(c, c - 1,
						std::memory_order_acquire, std::memory_order_relaxed))
					{
						return true;
					}
					// Lost the race to another waiter: retry from the top.
					continue;
				}

				if (timeoutMs == 0)
					return false;

				struct timespec ts;
				const struct timespec* tsPtr = nullptr;
				if (timeoutMs != ~0u)
				{
					u64 nowNs = MonotonicNs();
					if (nowNs >= deadlineNs)
						return false;
					ts = NsToTimespec(deadlineNs - nowNs);
					tsPtr = &ts;
				}

				long r = syscall(SYS_futex, &m_autoCount, FUTEX_WAIT, 0u, tsPtr, nullptr, 0);
				if (r == 0)
					continue;
				int e = errno;
				if (e == ETIMEDOUT)
					return false;
				if (e == EAGAIN || e == EINTR)
					continue;
			}
		}

		static void FutexWakeOne(Atomic<u32>* addr)
		{
			syscall(SYS_futex, addr, FUTEX_WAKE, 1, nullptr, nullptr, 0);
		}

		static void FutexWakeAll(Atomic<u32>* addr)
		{
			syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
		}

		static u64 MonotonicNs()
		{
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			return u64(ts.tv_sec) * 1'000'000'000ull + u64(ts.tv_nsec);
		}

		static struct timespec NsToTimespec(u64 ns)
		{
			struct timespec ts;
			ts.tv_sec  = time_t(ns / 1'000'000'000ull);
			ts.tv_nsec = long(ns % 1'000'000'000ull);
			return ts;
		}

	private:
		// EventHeader occupies the first 8 bytes (tag byte + m_resetType).
		// The atomics below are plain u32s, naturally aligned, safe for
		// cross-process FUTEX use on Linux x86_64 and aarch64.
		Atomic<bool> m_initialized { false };
		// 3 bytes of padding to align the u32s (the compiler inserts these).
		Atomic<u32>  m_state       { EventFutexState_Reset };
		Atomic<u32>  m_autoCount   { 0 };
	};

	// Sanity: must fit in the shared 13-u64 slot.
	static_assert(sizeof(EventFutex) <= 13 * sizeof(u64), "EventFutex too large for SharedEvent::m_data");
}

#endif // PLATFORM_LINUX
