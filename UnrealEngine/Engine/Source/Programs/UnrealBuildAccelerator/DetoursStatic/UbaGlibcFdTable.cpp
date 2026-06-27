// Copyright Epic Games, Inc. All Rights Reserved.
//
// UbaGlibcFdTable.cpp — implementation of the glibc-handler-side fd→path
// map. See UbaGlibcFdTable.h for the contract and rationale.
//
// Mirrors the open-addressed fixed-size table in UbaStaticStubCore.cpp
// (StubFdEntry / g_fdTable). Kept as a separate translation unit so F1.B
// and F1.C can share the same in-blob storage without forcing a circular
// include between handler files.

#include "UbaGlibcFdTable.h"

namespace uba
{
	// StubAllocatorAllocate is defined in UbaStaticStubCore.cpp; we share
	// the same arena rather than carving our own — single allocator keeps
	// blob size and bug surface small.
	void* StubAllocatorAllocate(u64 bytes, u64 alignment);
}

namespace
{
	struct GlibcFdEntry
	{
		uba::u32   fd;       // 0xFFFFFFFF = empty slot
		uba::u32   closeId;  // 0 = "do not RPC on close"
		const char* path;    // borrowed from StubAllocator arena
	};

	constexpr uba::u32 kFdTableSize = 256;   // linear-probe; ~75% fill OK
	alignas(64) GlibcFdEntry g_table[kFdTableSize];

	// One-shot init guard. The stub blob has no _init / no static
	// constructors, so we lazy-zero on first call. Atomic to handle
	// (admittedly rare) concurrent first-touch from two threads.
	uba::u32 g_initFlag = 0;

	inline void EnsureInit()
	{
		uba::u32 expected = 0;
		if (__atomic_compare_exchange_n(&g_initFlag, &expected, 1, 0,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		{
			for (uba::u32 i = 0; i < kFdTableSize; ++i)
				g_table[i].fd = 0xFFFFFFFFu;
			__atomic_store_n(&g_initFlag, 2, __ATOMIC_RELEASE);
			return;
		}
		// Lost the CAS race — spin briefly until the winner finishes init.
		while (__atomic_load_n(&g_initFlag, __ATOMIC_ACQUIRE) != 2)
			__asm__ volatile("pause" ::: "memory");
	}

	inline uba::u32 HashFd(int fd)
	{
		uba::u32 x = (uba::u32)fd;
		x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15;
		return x & (kFdTableSize - 1);
	}
}

namespace uba
{
	void GlibcFdTrack(int fd, const char* path, u32 closeId)
	{
		if (fd < 0) return;
		EnsureInit();
		u32 h = HashFd(fd);
		for (u32 i = 0; i < kFdTableSize; ++i)
		{
			u32 slot = (h + i) & (kFdTableSize - 1);
			GlibcFdEntry& e = g_table[slot];
			if (e.fd == 0xFFFFFFFFu || e.fd == (u32)fd)
			{
				e.fd      = (u32)fd;
				e.closeId = closeId;
				e.path    = path;
				return;
			}
		}
		// Table full — silently drop. Tracking is observation-only, so
		// the worst that happens is a missed CloseFile RPC.
	}

	bool GlibcFdLookup(int fd, const char*& outPath, u32& outCloseId)
	{
		if (fd < 0) return false;
		if (__atomic_load_n(&g_initFlag, __ATOMIC_ACQUIRE) == 0) return false;
		u32 h = HashFd(fd);
		for (u32 i = 0; i < kFdTableSize; ++i)
		{
			u32 slot = (h + i) & (kFdTableSize - 1);
			GlibcFdEntry& e = g_table[slot];
			if (e.fd == 0xFFFFFFFFu) return false;
			if (e.fd == (u32)fd)
			{
				outPath    = e.path;
				outCloseId = e.closeId;
				return true;
			}
		}
		return false;
	}

	bool GlibcFdTake(int fd, const char*& outPath, u32& outCloseId)
	{
		if (fd < 0) return false;
		if (__atomic_load_n(&g_initFlag, __ATOMIC_ACQUIRE) == 0) return false;
		u32 h = HashFd(fd);
		for (u32 i = 0; i < kFdTableSize; ++i)
		{
			u32 slot = (h + i) & (kFdTableSize - 1);
			GlibcFdEntry& e = g_table[slot];
			if (e.fd == 0xFFFFFFFFu) return false;
			if (e.fd == (u32)fd)
			{
				outPath    = e.path;
				outCloseId = e.closeId;
				e.fd      = 0xFFFFFFFFu;
				return true;
			}
		}
		return false;
	}
}
