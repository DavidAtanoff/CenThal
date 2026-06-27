// Copyright Epic Games, Inc. All Rights Reserved.
//
// UbaGlibc.cpp — implementation of the glibc-static detour bridge.
//
// Consolidation note: this TU replaces UbaGlibcHandlers.cpp,
// UbaGlibcHandlerTable.cpp, and UbaGlibcFdTable.cpp with a single
// implementation. See UbaGlibc.h for the public contract.
//
// Sections (in order):
//   1.  Raw syscall helpers + errno translation (internal linkage).
//   2.  ubg_not_reached / STUB_NOT_REACHED / STUB_ABORT_UNSUPPORTED —
//       shared abort path used by the unsupported-op handlers.
//   3.  fd → (path, closeId) map shared between open/openat/close.
//   4.  Real handler bodies: open, openat, read, write, close, lseek,
//       fxstat, mmap, access, exit.
//   5.  Abort-on-reach handlers for unsupported glibc calls — loud
//       failure on any libc op we don't model. The patcher only
//       installs the hook when the target exports the symbol, so
//       binaries that don't touch those ops stay unaffected.
//   6.  g_glibcHandlerTable instance — the self-locating blob of
//       (name, offset, kind) rows the patcher scans for.

#include <stddef.h>

#include "UbaGlibc.h"
#include "UbaDetoursShared.h"              // DEBUG_LOG / TC
#include "UbaDirectoryTable.h"             // DirectoryTable::EntryInformation
#include "UbaDetoursFileMappingTable.h"    // Rpc_GetEntryInformation

// AT_FDCWD comes from the libc headers UbaBase.h pulls in under
// UBA_STUB_BUILD; the system fcntl.h has it as -100 on Linux.
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

// Linux x86-64 syscall numbers we use directly.
#define SYS_NR_READ      0
#define SYS_NR_WRITE     1
#define SYS_NR_CLOSE     3
#define SYS_NR_FSTAT     5
#define SYS_NR_MMAP      9
#define SYS_NR_ACCESS    21
#define SYS_NR_DUP       32
#define SYS_NR_OPENAT    257
#define SYS_NR_LSEEK     8
#define SYS_NR_WRITE_FD2 1   // stderr fd for abort messages
#define SYS_NR_EXIT_GROUP 231

#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

// g_initDone is the stub-init flag set in UbaStaticStubCore.cpp once the
// session's RPC plumbing is up. Before that, handlers must passthrough
// to the kernel — the directory table is empty and RPC would spin.
extern "C" uba::u32 g_initDone;

// Defined in UbaStaticStubCore.cpp. The exit hook fires it on the way to
// SYS_exit_group so the server marks the process as cleanly exited.
namespace uba { void SendExitRpc(uba::u32 exitCode); }

// Defined in UbaStaticStubCore.cpp — shared arena used for path dups so
// borrowed strings outlive the caller's buffer across open→close.
namespace uba { void* StubAllocatorAllocate(uba::u64 bytes, uba::u64 alignment); }

// __errno_location is provided by UbaStaticStubCore.cpp inside the stub
// blob. That stub-side function defers to the target binary's own
// __errno_location via g_glibc_errno_hook_fn (stamped by the patcher at
// injection time) and falls back to a shared int for targets without
// a libc errno.
//
// Intentionally NOT weak: a weak ref would make the compiler emit
// `if (&__errno_location)` as a GOT load, but the stub linker script
// does not capture .got into the extracted blob — the GOT entry would
// read as zero at runtime (it lands in the PT_LOAD's BSS extension),
// the guard would short-circuit, and the errno write would silently be
// skipped. (See Claude memory: feedback_stub_blob_no_got.)
extern "C" int* __errno_location(void);

// ─────────────────────────────────────────────────────────────────────────────
// 1. Raw syscall helpers + errno translation
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	inline long ubg_raw_syscall1(long num, long a)
	{
		long r;
		__asm__ volatile("syscall"
			: "=a"(r)
			: "0"(num), "D"(a)
			: "rcx", "r11", "memory");
		return r;
	}

	inline long ubg_raw_syscall2(long num, long a, long b)
	{
		long r;
		__asm__ volatile("syscall"
			: "=a"(r)
			: "0"(num), "D"(a), "S"(b)
			: "rcx", "r11", "memory");
		return r;
	}

	inline long ubg_raw_syscall3(long num, long a, long b, long c)
	{
		long r;
		__asm__ volatile("syscall"
			: "=a"(r)
			: "0"(num), "D"(a), "S"(b), "d"(c)
			: "rcx", "r11", "memory");
		return r;
	}

	inline long ubg_raw_syscall4(long num, long a, long b, long c, long d)
	{
		long r;
		register long r10 __asm__("r10") = d;
		__asm__ volatile("syscall"
			: "=a"(r)
			: "0"(num), "D"(a), "S"(b), "d"(c), "r"(r10)
			: "rcx", "r11", "memory");
		return r;
	}

	__attribute__((unused))
	inline long ubg_raw_syscall6(long num, long a, long b, long c, long d, long e, long f)
	{
		long r;
		register long r10 __asm__("r10") = d;
		register long r8  __asm__("r8")  = e;
		register long r9  __asm__("r9")  = f;
		__asm__ volatile("syscall"
			: "=a"(r)
			: "0"(num), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
			: "rcx", "r11", "memory");
		return r;
	}

	// Translate a raw kernel syscall return value (negative errno in
	// [-4095, -1]) into glibc-wrapper semantics: on error set errno and
	// return -1; otherwise return the result verbatim.
	inline long ubg_translate_errno(long r)
	{
		if (r < 0 && r >= -4095)
		{
			int* errp = __errno_location();
			if (errp) *errp = (int)(-r);
			return -1;
		}
		return r;
	}

	inline bool ubg_path_is_absolute(const char* p)
	{
		return p && p[0] == '/';
	}

	// Duplicate a NUL-terminated path into the StubAllocator-owned arena
	// so it outlives the caller's buffer (qemu / glibc may reuse the
	// argument storage between open() and a later close() / fxstat()).
	const char* ubg_dup_path(const char* path)
	{
		if (!path) return nullptr;
		size_t len = 0;
		while (path[len] && len < 4096) ++len;
		char* dup = (char*)uba::StubAllocatorAllocate(len + 1, 1);
		for (size_t i = 0; i < len; ++i) dup[i] = path[i];
		dup[len] = 0;
		return dup;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Abort helpers — shared by the unsupported-op handlers below.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	__attribute__((noreturn, noinline, unused))
	void ubg_not_reached(const char* what, size_t whatLen)
	{
		static const char pre[] __attribute__((section(".text"))) = "[uba_stub] glibc-handler not-reached: ";
		static const char nl[]  __attribute__((section(".text"))) = "\n";
		ubg_raw_syscall3(SYS_NR_WRITE_FD2, 2, (long)pre,  sizeof(pre) - 1);
		ubg_raw_syscall3(SYS_NR_WRITE_FD2, 2, (long)what, (long)whatLen);
		ubg_raw_syscall3(SYS_NR_WRITE_FD2, 2, (long)nl,   sizeof(nl) - 1);
		ubg_raw_syscall1(SYS_NR_EXIT_GROUP, 134);   // SIGABRT-ish
		__builtin_unreachable();
	}

	__attribute__((noreturn, noinline, unused))
	void ubg_abort_unsupported(const char* op, size_t opLen)
	{
		static const char pre[] __attribute__((section(".text"))) =
			"[uba_stub] unsupported glibc call: ";
		static const char post[] __attribute__((section(".text"))) =
			" — add a handler or teach the patcher to skip it\n";
		ubg_raw_syscall3(SYS_NR_WRITE_FD2, 2, (long)pre,  sizeof(pre) - 1);
		ubg_raw_syscall3(SYS_NR_WRITE_FD2, 2, (long)op,   (long)opLen);
		ubg_raw_syscall3(SYS_NR_WRITE_FD2, 2, (long)post, sizeof(post) - 1);
		ubg_raw_syscall1(SYS_NR_EXIT_GROUP, 134);
		__builtin_unreachable();
	}
}

#define STUB_NOT_REACHED(label) do {                                           \
		static const char _stub_msg[] __attribute__((section(".text"))) = label; \
		ubg_not_reached(_stub_msg, sizeof(_stub_msg) - 1);                        \
	} while (0)

#define STUB_ABORT_UNSUPPORTED(op) do {                                        \
		static const char _stub_op[] __attribute__((section(".text"))) = #op;    \
		ubg_abort_unsupported(_stub_op, sizeof(_stub_op) - 1);                    \
	} while (0)

// ─────────────────────────────────────────────────────────────────────────────
// 3. fd → (path, closeId) map
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	struct GlibcFdEntry
	{
		uba::u32    fd;       // 0xFFFFFFFF = empty slot
		uba::u32    closeId;  // 0 = "do not RPC on close"
		const char* path;     // borrowed from StubAllocator arena
	};

	constexpr uba::u32 kFdTableSize = 256;   // linear-probe; ~75% fill OK
	alignas(64) GlibcFdEntry g_fdTable[kFdTableSize];

	// One-shot init guard. The stub blob has no static constructors, so we
	// lazy-zero on first call. Atomic to handle concurrent first-touch.
	uba::u32 g_fdInitFlag = 0;

	inline void EnsureFdInit()
	{
		uba::u32 expected = 0;
		if (__atomic_compare_exchange_n(&g_fdInitFlag, &expected, 1, 0,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		{
			for (uba::u32 i = 0; i < kFdTableSize; ++i)
				g_fdTable[i].fd = 0xFFFFFFFFu;
			__atomic_store_n(&g_fdInitFlag, 2, __ATOMIC_RELEASE);
			return;
		}
		while (__atomic_load_n(&g_fdInitFlag, __ATOMIC_ACQUIRE) != 2)
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
		EnsureFdInit();
		u32 h = HashFd(fd);
		for (u32 i = 0; i < kFdTableSize; ++i)
		{
			u32 slot = (h + i) & (kFdTableSize - 1);
			GlibcFdEntry& e = g_fdTable[slot];
			if (e.fd == 0xFFFFFFFFu || e.fd == (u32)fd)
			{
				e.fd      = (u32)fd;
				e.closeId = closeId;
				e.path    = path;
				return;
			}
		}
		// Table full — silently drop. Tracking is observation-only.
	}

	bool GlibcFdLookup(int fd, const char*& outPath, u32& outCloseId)
	{
		if (fd < 0) return false;
		if (__atomic_load_n(&g_fdInitFlag, __ATOMIC_ACQUIRE) == 0) return false;
		u32 h = HashFd(fd);
		for (u32 i = 0; i < kFdTableSize; ++i)
		{
			u32 slot = (h + i) & (kFdTableSize - 1);
			GlibcFdEntry& e = g_fdTable[slot];
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
		if (__atomic_load_n(&g_fdInitFlag, __ATOMIC_ACQUIRE) == 0) return false;
		u32 h = HashFd(fd);
		for (u32 i = 0; i < kFdTableSize; ++i)
		{
			u32 slot = (h + i) & (kFdTableSize - 1);
			GlibcFdEntry& e = g_fdTable[slot];
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

// ─────────────────────────────────────────────────────────────────────────────
// 4. Real handler bodies
// ─────────────────────────────────────────────────────────────────────────────

// uba_glibc_open — replaces glibc's __libc_open wrapper.
//
// Pre-init / null path: raw SYS_openat passthrough (RPC plumbing isn't
// ready yet). Otherwise: hash, Rpc_CreateFileW to learn the staging
// newName + closeId, real-openat against newName, then register
// (fd, path, closeId) in the fd table so close can finalise via
// Rpc_UpdateCloseHandle.
extern "C" __attribute__((used))
int uba_glibc_open(const char* path, int flags, int mode)
{
	using uba::u8;
	using uba::u32;
	using uba::u64;

	if (!path || !__atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
	{
		long r = ubg_raw_syscall4(SYS_NR_OPENAT,
			(long)AT_FDCWD, (long)path, (long)flags, (long)mode);
		int  fd = (int)ubg_translate_errno(r);
		DEBUG_LOG(TC("__libc_open: %s (flags=0x%x mode=0%o) -> %d (pre-init passthrough)"),
			path ? path : "(null)", (u32)flags, (u32)mode, fd);
		return fd;
	}

	u64 plen = 0;
	while (path[plen] && plen < 4096) ++plen;

	u64 lower = (u64)(flags & 0x3);
	u8  access = (lower == 0) ? uba::AccessFlag_Read
	           : (lower == 1) ? uba::AccessFlag_Write
	                          : (uba::AccessFlag_Read | uba::AccessFlag_Write);

	uba::StringView view(path, (u32)plen);
	uba::StringKey  key = uba::ToStringKeyNoCheck(path, plen);
	char    newName[256] = {0};
	u64     outSize = 0;
	u32     outCloseId = 0;

	uba::Rpc_CreateFileW(view, key, access, newName, sizeof(newName),
		outSize, outCloseId, /*lock=*/true);

	u64 nlen = 0;
	while (nlen < sizeof(newName) && newName[nlen]) ++nlen;
	bool differs = (nlen != plen);
	if (!differs)
	{
		for (u64 i = 0; i < nlen; ++i)
			if (newName[i] != path[i]) { differs = true; break; }
	}
	const char* realPath = (differs && nlen > 0) ? newName : path;

	long r = ubg_raw_syscall4(SYS_NR_OPENAT,
		(long)AT_FDCWD, (long)realPath, (long)flags, (long)mode);
	int  fd = (int)ubg_translate_errno(r);

	int* errp_now = __errno_location();
	int post_errno = errp_now ? *errp_now : 0;
	DEBUG_LOG(TC("__libc_open: %s (flags=0x%x mode=0%o) -> %d (errno=%d closeId=%u)"),
		path, (u32)flags, (u32)mode, fd, post_errno, (u32)outCloseId);

	if (fd >= 0)
	{
		const char* dup = ubg_dup_path(path);
		uba::GlibcFdTrack(fd, dup, outCloseId);
	}

	(void)outSize;
	return fd;
}

// uba_glibc_openat — replaces glibc's __openat wrapper.
extern "C" __attribute__((used))
int uba_glibc_openat(int dirfd, const char* path, int flags, int mode)
{
	bool absolute   = ubg_path_is_absolute(path);
	bool atFdCwd    = (dirfd == AT_FDCWD);
	bool ubaHandles = atFdCwd || absolute;

	long r;
	if (ubaHandles)
	{
		r = ubg_raw_syscall4(SYS_NR_OPENAT,
			(long)AT_FDCWD, (long)path, (long)flags, (long)mode);
	}
	else
	{
		// Relative path against a real dirfd — UBA's directory table is
		// keyed by absolute path; we can't resolve dirfd-relative paths
		// into UBA's view without parent-fd bookkeeping. Passthrough.
		r = ubg_raw_syscall4(SYS_NR_OPENAT,
			(long)dirfd, (long)path, (long)flags, (long)mode);
	}

	long ret = ubg_translate_errno(r);

	if (ret >= 0 && ubaHandles && path)
	{
		const char* dup = ubg_dup_path(path);
		uba::GlibcFdTrack((int)ret, dup, /*closeId=*/0);
	}

	DEBUG_LOG(TC("__libc_openat: dirfd=%d path=%s flags=0x%x mode=0%o -> %lld"),
		dirfd, path ? path : "(null)", (unsigned)flags, (unsigned)mode, (long long)ret);

	return (int)ret;
}

// uba_glibc_read / write — pure passthrough with errno translation.
extern "C" __attribute__((used))
long uba_glibc_read(int fd, void* buf, unsigned long n)
{
	long r = ubg_raw_syscall3(SYS_NR_READ, (long)fd, (long)buf, (long)n);
	return ubg_translate_errno(r);
}

extern "C" __attribute__((used))
long uba_glibc_write(int fd, const void* buf, unsigned long n)
{
	long r = ubg_raw_syscall3(SYS_NR_WRITE, (long)fd, (long)buf, (long)n);
	return ubg_translate_errno(r);
}

// uba_glibc_close — on RPC-registered fds, dup+fstat to capture final
// size/mtime/mode, real-close, then Rpc_UpdateCloseHandle.
extern "C" __attribute__((used))
int uba_glibc_close(int fd)
{
	using uba::u32;
	using uba::u64;

	if (!__atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
	{
		long r = ubg_raw_syscall1(SYS_NR_CLOSE, (long)fd);
		return (int)ubg_translate_errno(r);
	}

	const char* path    = nullptr;
	u32         closeId = 0;
	bool tracked = uba::GlibcFdTake(fd, path, closeId);

	u64 fileSize      = 0;
	u64 lastWriteTime = 0;
	u32 attributes    = 0x8000u;   // S_IFREG fallback if fstat fails

	if (tracked && closeId)
	{
		long dupFd = ubg_raw_syscall1(SYS_NR_DUP, (long)fd);
		if (dupFd >= 0)
		{
			alignas(8) unsigned char sb[144] = {0};
			long r = ubg_raw_syscall2(SYS_NR_FSTAT, dupFd, (long)sb);
			if (r == 0)
			{
				attributes    = *(const u32*)(sb + 24);  // st_mode
				fileSize      = *(const u64*)(sb + 48);  // st_size
				u64 mtimeSec  = *(const u64*)(sb + 88);  // st_mtime
				u64 mtimeNsec = *(const u64*)(sb + 96);  // st_mtime_nsec
				lastWriteTime = mtimeSec * 10000000ull + mtimeNsec / 100;
			}
			(void)ubg_raw_syscall1(SYS_NR_CLOSE, dupFd);
		}
	}

	long r  = ubg_raw_syscall1(SYS_NR_CLOSE, (long)fd);
	int  rc = (int)ubg_translate_errno(r);

	int* errp = __errno_location();
	DEBUG_LOG(TC("__libc_close: fd=%d (%s) size=%llu attr=0x%x -> %d (errno=%d)"),
		fd, path ? path : "(untracked)",
		(unsigned long long)fileSize, (u32)attributes, rc,
		(rc < 0 && errp) ? *errp : 0);

	if (tracked && closeId)
	{
		uba::Rpc_UpdateCloseHandle(path, closeId, /*deleteOnClose=*/false,
			/*newName=*/"", uba::SharedMemoryHandle{},
			fileSize, lastWriteTime, attributes,
			/*success=*/(rc == 0));
	}

	return rc;
}

// uba_glibc_lseek — pure passthrough.
extern "C" __attribute__((used))
long uba_glibc_lseek(int fd, long off, int whence)
{
	long r = ubg_raw_syscall3(SYS_NR_LSEEK, (long)fd, off, (long)whence);
	long ret = ubg_translate_errno(r);
	DEBUG_LOG(TC("__libc_lseek: fd=%d off=%lld whence=%d -> %lld"),
		fd, (long long)off, whence, (long long)ret);
	return ret;
}

// uba_glibc_fxstat — passthrough to SYS_fstat (Phase 2 may synthesise
// from the directory table using the fd→path map).
extern "C" __attribute__((used))
int uba_glibc_fxstat(int ver, int fd, void* statbuf)
{
	(void)ver;
	long r = ubg_raw_syscall2(SYS_NR_FSTAT, (long)fd, (long)statbuf);
	long ret = ubg_translate_errno(r);
	DEBUG_LOG(TC("__fxstat: ver=%d fd=%d -> %lld (passthrough)"),
		ver, fd, (long long)ret);
	return (int)ret;
}

// uba_glibc_mmap — passthrough with MAP_FAILED translation.
extern "C" __attribute__((used))
void* uba_glibc_mmap(void* addr, unsigned long len, int prot, int flags, int fd, long off)
{
	long r = ubg_raw_syscall6(SYS_NR_MMAP,
		(long)addr, (long)len, (long)prot, (long)flags, (long)fd, (long)off);

	void* result;
	if (r < 0 && r >= -4095)
	{
		(void)ubg_translate_errno(r);
		result = MAP_FAILED;
	}
	else
	{
		result = (void*)r;
	}

	DEBUG_LOG(TC("__libc_mmap: addr=%p len=%lu prot=0x%x flags=0x%x fd=%d off=%lld -> %p"),
		addr, (unsigned long)len, (unsigned)prot, (unsigned)flags, (int)fd, (long long)off, result);
	return result;
}

// uba_glibc_access — UBA-aware existence probe; falls back to kernel.
extern "C" __attribute__((used))
int uba_glibc_access(const char* path, int mode)
{
	if (path && __atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
	{
		uba::u64 plen = 0;
		while (path[plen] && plen < 4096) ++plen;
		if (plen > 0)
		{
			uba::StringView view(path, (uba::u32)plen);
			uba::StringKey  key  = uba::ToStringKeyNoCheck(path, plen);
			uba::DirectoryTable::EntryInformation info;
			if (uba::Rpc_GetEntryInformation(info, key, view, /*checkIfDir=*/false))
			{
				DEBUG_LOG(TC("__libc_access: %s mode=0x%x -> 0 (UBA exists, attr=0x%x)"),
					path, (unsigned)mode, (uba::u32)info.attributes);
				return 0;
			}
		}
	}

	long r = ubg_raw_syscall2(SYS_NR_ACCESS, (long)path, (long)mode);
	int ret = (int)ubg_translate_errno(r);
	DEBUG_LOG(TC("__libc_access: %s mode=0x%x -> %d (passthrough)"),
		path ? path : "(null)", (unsigned)mode, ret);
	return ret;
}

// uba_glibc_exit — sends MessageType_Exit then SYS_exit_group in an
// infinite loop so the compiler can't fall through to adjacent code.
extern "C" __attribute__((noreturn, used))
void uba_glibc_exit(int status)
{
	if (__atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
		uba::SendExitRpc((uba::u32)status);
	for (;;)
		ubg_raw_syscall1(SYS_NR_EXIT_GROUP, (long)status);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Abort-on-reach handlers for unsupported glibc calls
// ─────────────────────────────────────────────────────────────────────────────
//
// If a build tool hits any of these, UBA's view would silently diverge
// from reality (we wouldn't see the mutation/fork/stat-for-side-effects).
// A loud abort is strictly better than silent corruption: the user gets
// a clear message pinpointing what needs to be added to the handler set
// to support their workload.
//
// Each handler is prototyped as `void(void)` because we don't need the
// caller's args — we abort before they matter. The 5-byte JMP at the
// wrapper destroys its prologue; control never returns.

#define UBA_GLIBC_ABORT(name)                                                  \
	extern "C" __attribute__((noreturn, used))                                   \
	void uba_glibc_##name(void) { STUB_ABORT_UNSUPPORTED(name); }

UBA_GLIBC_ABORT(stat)
UBA_GLIBC_ABORT(lstat)
UBA_GLIBC_ABORT(fstatat)
UBA_GLIBC_ABORT(statx)
UBA_GLIBC_ABORT(unlink)
UBA_GLIBC_ABORT(unlinkat)
UBA_GLIBC_ABORT(rename)
UBA_GLIBC_ABORT(renameat)
UBA_GLIBC_ABORT(renameat2)
UBA_GLIBC_ABORT(link)
UBA_GLIBC_ABORT(linkat)
UBA_GLIBC_ABORT(symlink)
UBA_GLIBC_ABORT(symlinkat)
UBA_GLIBC_ABORT(readlink)
UBA_GLIBC_ABORT(readlinkat)
UBA_GLIBC_ABORT(truncate)
UBA_GLIBC_ABORT(ftruncate)
UBA_GLIBC_ABORT(mkdir)
UBA_GLIBC_ABORT(mkdirat)
UBA_GLIBC_ABORT(rmdir)
UBA_GLIBC_ABORT(fork)
UBA_GLIBC_ABORT(vfork)
UBA_GLIBC_ABORT(clone)
UBA_GLIBC_ABORT(clone3)
UBA_GLIBC_ABORT(execve)
UBA_GLIBC_ABORT(execveat)
UBA_GLIBC_ABORT(dup)
UBA_GLIBC_ABORT(dup2)
UBA_GLIBC_ABORT(dup3)
UBA_GLIBC_ABORT(pipe)
UBA_GLIBC_ABORT(pipe2)
UBA_GLIBC_ABORT(opendir)
UBA_GLIBC_ABORT(fdopendir)
UBA_GLIBC_ABORT(readdir)
UBA_GLIBC_ABORT(closedir)
UBA_GLIBC_ABORT(getdents64)
UBA_GLIBC_ABORT(chdir)
UBA_GLIBC_ABORT(fchdir)

#undef UBA_GLIBC_ABORT

// ─────────────────────────────────────────────────────────────────────────────
// 6. Handler table instance
// ─────────────────────────────────────────────────────────────────────────────
//
// Lives in its own ".text.uba_handlers" section so the linker keeps it
// together early in the blob. The patcher resolves it by scanning for
// kGlibcHandlerTableMagic at u64 alignment. `offset` is stamped
// post-link by DetoursStatic/Build.sh; `count` covers every valid row.

extern "C" __attribute__((section(".text.uba_handlers"), used))
const GlibcHandlerTable g_glibcHandlerTable = {
	/* magic    */ kGlibcHandlerTableMagic,
	/* count    */ 49,
	/* reserved */ 0,
	/* entries  */ {
		// --- Real handlers (mapped via kGlibcAliasTable in the patcher) ---
		{ "open",              0, GlibcEntry_HandlerHook },
		{ "openat",            0, GlibcEntry_HandlerHook },
		{ "read",              0, GlibcEntry_HandlerHook },
		{ "write",             0, GlibcEntry_HandlerHook },
		{ "close",             0, GlibcEntry_HandlerHook },
		{ "lseek",             0, GlibcEntry_HandlerHook },
		{ "fxstat",            0, GlibcEntry_HandlerHook },
		{ "mmap",              0, GlibcEntry_HandlerHook },
		{ "access",            0, GlibcEntry_HandlerHook },
		{ "exit",              0, GlibcEntry_HandlerHook },

		// --- Abort handlers for unsupported operations ---
		{ "stat",              0, GlibcEntry_HandlerHook },
		{ "lstat",             0, GlibcEntry_HandlerHook },
		{ "fstatat",           0, GlibcEntry_HandlerHook },
		{ "statx",             0, GlibcEntry_HandlerHook },
		{ "unlink",            0, GlibcEntry_HandlerHook },
		{ "unlinkat",          0, GlibcEntry_HandlerHook },
		{ "rename",            0, GlibcEntry_HandlerHook },
		{ "renameat",          0, GlibcEntry_HandlerHook },
		{ "renameat2",         0, GlibcEntry_HandlerHook },
		{ "link",              0, GlibcEntry_HandlerHook },
		{ "linkat",            0, GlibcEntry_HandlerHook },
		{ "symlink",           0, GlibcEntry_HandlerHook },
		{ "symlinkat",         0, GlibcEntry_HandlerHook },
		{ "readlink",          0, GlibcEntry_HandlerHook },
		{ "readlinkat",        0, GlibcEntry_HandlerHook },
		{ "truncate",          0, GlibcEntry_HandlerHook },
		{ "ftruncate",         0, GlibcEntry_HandlerHook },
		{ "mkdir",             0, GlibcEntry_HandlerHook },
		{ "mkdirat",           0, GlibcEntry_HandlerHook },
		{ "rmdir",             0, GlibcEntry_HandlerHook },
		{ "fork",              0, GlibcEntry_HandlerHook },
		{ "vfork",             0, GlibcEntry_HandlerHook },
		{ "clone",             0, GlibcEntry_HandlerHook },
		{ "clone3",            0, GlibcEntry_HandlerHook },
		{ "execve",            0, GlibcEntry_HandlerHook },
		{ "execveat",          0, GlibcEntry_HandlerHook },
		{ "dup",               0, GlibcEntry_HandlerHook },
		{ "dup2",              0, GlibcEntry_HandlerHook },
		{ "dup3",              0, GlibcEntry_HandlerHook },
		{ "pipe",              0, GlibcEntry_HandlerHook },
		{ "pipe2",             0, GlibcEntry_HandlerHook },
		{ "opendir",           0, GlibcEntry_HandlerHook },
		{ "fdopendir",         0, GlibcEntry_HandlerHook },
		{ "readdir",           0, GlibcEntry_HandlerHook },
		{ "closedir",          0, GlibcEntry_HandlerHook },
		{ "getdents64",        0, GlibcEntry_HandlerHook },
		{ "chdir",             0, GlibcEntry_HandlerHook },
		{ "fchdir",            0, GlibcEntry_HandlerHook },

		// --- Import slot (patcher stamps target's VA at blob+offset) ---
		{ "__errno_location",  0, GlibcEntry_ImportSlot  },
	},
};
