// Copyright Epic Games, Inc. All Rights Reserved.
//
// UbaGlibcHandlers.cpp — Phase 1 implementation of the glibc-static detour
// bridge handlers. Each function is jumped to via a 5-byte JMP rel32 stamped
// over the corresponding libc wrapper's first instruction (e.g. __libc_open).
// Control never returns to the wrapper body.
//
// HANDLER OWNERSHIP (Phase 1 sub-agents):
//   - F1.B owns: uba_glibc_open, uba_glibc_read, uba_glibc_write,
//                uba_glibc_close
//   - F1.C owns: uba_glibc_openat, uba_glibc_lseek, uba_glibc_fxstat   <-- THIS
//   - F1.D owns: uba_glibc_mmap, uba_glibc_access
//
// BRIDGE PRIMITIVES
//   For glibc-static qemu (real pthreads, 8 MB+ stacks, no SIGURG hazard)
//   the alt-stack/signal-block/spinlock dance the static-Go bridge needs
//   is unnecessary. Handlers are plain SystemV-AMD64 C functions; they
//   may freely use raw syscalls, atomic loads of stub globals, and the
//   shared RPC plumbing (which has its own g_communicationLock).
//
// ERRNO TRANSLATION
//   The kernel returns -errno in rax for syscall failures (range
//   [-4095, -1]). glibc wrappers translate to `errno = -result; return -1;`.
//   Our handlers do the same — calling __errno_location() to find the
//   per-thread errno slot. (__errno_location() is in libc.so but the
//   patched binary is statically linked, so the symbol resolves to the
//   target's own __errno_location, which uses the target's pthread TLS.)

#include <stddef.h>

#include "UbaGlibcHandlers.h"
#include "UbaGlibcFdTable.h"
#include "UbaDetoursShared.h"   // DEBUG_LOG / TC
#include "UbaDirectoryTable.h"  // DirectoryTable::EntryInformation (F1.D access)
#include "UbaDetoursFileMappingTable.h"  // Rpc_GetEntryInformation (F1.D access)

// AT_FDCWD comes from the libc headers UbaBase.h pulls in under
// UBA_STUB_BUILD; the system fcntl.h has it as -100 on Linux.
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

// Linux x86-64 syscall numbers we need. (See <asm/unistd_64.h>.)
#define SYS_NR_LSEEK    8
#define SYS_NR_FSTAT    5
#define SYS_NR_OPENAT   257
#define SYS_NR_MMAP     9
#define SYS_NR_ACCESS   21

#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

// `g_initDone` is the stub-init flag set in UbaStaticStubCore.cpp once the
// session's RPC plumbing is up. Before that, handlers must passthrough to
// the kernel — the directory table is empty so any RPC path either spins on
// the comm event or short-circuits to a wrong "not found" answer.
extern "C" uba::u32 g_initDone;

// Defined in UbaStaticStubCore.cpp — sends MessageType_Exit on the existing
// RPC channel.  Used by uba_glibc_exit (the _exit hook below) so the server
// gets a clean exit message before the kernel terminates the process.
namespace uba { void SendExitRpc(uba::u32 exitCode); }

// StubAllocatorAllocate is defined in UbaStaticStubCore.cpp; we share the
// arena rather than carving our own. Declared here so ubg_dup_path below
// can dup paths into stub-blob-owned memory.
namespace uba { void* StubAllocatorAllocate(uba::u64 bytes, uba::u64 alignment); }

// ----------------------------------------------------------------------------
// Raw-syscall helpers + errno-style return translation.
//
// Phase 0's STUB_NOT_REACHED helper is preserved so handlers F1.B and F1.D
// own can keep stubbing themselves out until they land. The Phase 1 bodies
// for F1.C (openat / lseek / fxstat) below use the ubg_raw_syscall* helpers
// directly.
// ----------------------------------------------------------------------------

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

	// All Phase 1 handlers are now implemented, so STUB_NOT_REACHED isn't
	// fired from any live code path. Kept here (and __attribute__((unused))-
	// silenced) so any future scaffolded handler can use it without having
	// to re-introduce the helper.
	__attribute__((noreturn, noinline, unused))
	void ubg_not_reached(const char* what, size_t whatLen)
	{
		static const char pre[] __attribute__((section(".text"))) = "[uba_stub] glibc-handler not-reached: ";
		static const char nl[]  __attribute__((section(".text"))) = "\n";
		ubg_raw_syscall3(1 /* SYS_write */, 2, (long)pre, (long)(sizeof(pre) - 1));
		ubg_raw_syscall3(1 /* SYS_write */, 2, (long)what, (long)whatLen);
		ubg_raw_syscall3(1 /* SYS_write */, 2, (long)nl, 1);
		ubg_raw_syscall1(231 /* SYS_exit_group */, 1);
		__builtin_unreachable();
	}

	// Translate a raw kernel syscall return value (negative errno in
	// [-4095, -1]) into glibc-wrapper semantics: on error set errno and
	// return -1; otherwise return the result verbatim.
	//
	// __errno_location is provided by UbaStaticStubCore.cpp inside the
	// stub blob. That stub-side function defers to the target binary's
	// own __errno_location via a writable function-pointer slot
	// (g_glibc_errno_hook_fn) the patcher stamps at injection time —
	// which then returns the target's glibc-static per-thread errno.
	//
	// The declaration here is intentionally NOT weak. A weak ref makes
	// the compiler emit `if (&__errno_location)` as a GOT load — but the
	// stub's linker script does not capture .got into the extracted blob,
	// so the GOT entry would read as zero at runtime (it lands in the
	// PT_LOAD's BSS extension), the guard would short-circuit, and the
	// errno write would silently be skipped. With a strong ref the call
	// is RIP-relative and the address-of check is folded by the compiler.
	extern "C" int* __errno_location(void);

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

#define STUB_NOT_REACHED(label) do {                                          \
		static const char _stub_msg[] __attribute__((section(".text"))) = label; \
		ubg_not_reached(_stub_msg, sizeof(_stub_msg) - 1);                       \
	} while (0)

// ----------------------------------------------------------------------------
// Handler bodies — Phase 0 stubs. Each marks its parameters as unused (the
// scaffold compiles with -Wall) and aborts via STUB_NOT_REACHED.
//
// __attribute__((used)) keeps the symbols in the linked ELF even though
// nothing in C++ source references them — the patcher resolves them by
// name (or via the GlibcHandlerTable offsets) at patch time.
// ----------------------------------------------------------------------------

// uba_glibc_open — replaces glibc's __libc_open wrapper.
//
// SystemV-AMD64 args: rdi=path, rsi=flags, rdx=mode. Returns the new fd, or
// -1 with errno set on failure (mirroring glibc-wrapper semantics).
//
// Behaviour mirrors UBA_WRAPPER(open) → Shared_open in the LD_PRELOAD detour
// (UbaDetoursFunctionsPosix.cpp:1841) but inlined: Shared_open is a file-
// private template in that TU and not callable from the static stub.
//   1. Pre-init / null path → raw SYS_openat passthrough (no RPC plumbing
//      yet; the directory table is empty so any RPC would either spin on
//      the comm event or short-circuit to a wrong "not found" answer).
//   2. Otherwise: hash the path, fire Rpc_CreateFileW to learn the staging
//      newName + closeId, real-openat against newName (path may differ from
//      input under remote / VFS staging), then register (fd, path, closeId)
//      in the shared GlibcFdTable so close can finalise via
//      Rpc_UpdateCloseHandle.
//
// Path-decision policies handled by Shared_prepareOpen (NoDetourFallback,
// DirectoryFd, ProcFallback, ...) are NOT replicated here — qemu's I/O surface
// doesn't hit those branches. If a future glibc-static target needs them,
// port the relevant slice from Shared_prepareOpen at that point.
extern "C" __attribute__((used))
int uba_glibc_open(const char* path, int flags, int mode)
{
	using uba::u8;
	using uba::u32;
	using uba::u64;

	// Pre-init or null path: pure passthrough. AT_FDCWD + SYS_openat is what
	// modern eglibc uses internally for open(); semantics match.
	if (!path || !__atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
	{
		long r = ubg_raw_syscall4(SYS_NR_OPENAT,
			(long)AT_FDCWD, (long)path, (long)flags, (long)mode);
		int  fd = (int)ubg_translate_errno(r);
		DEBUG_LOG(TC("__libc_open: %s (flags=0x%x mode=0%o) -> %d (pre-init passthrough)"),
			path ? path : "(null)", (u32)flags, (u32)mode, fd);
		return fd;
	}

	// Compute path length (bounded — defensive against unterminated buffers).
	u64 plen = 0;
	while (path[plen] && plen < 4096) ++plen;

	// Map open() flag bits to UBA's access enum. Same mapping uba_syscall_log
	// uses for SYS_nr_openat in UbaStaticStubCore.cpp.
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

	// Server may have remapped to a staging path (remote workers, VFS).
	// Use newName for the real syscall when it differs from the original.
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

	// On success register the (fd, path, closeId) tuple so close can drive
	// Rpc_UpdateCloseHandle. The original (caller-visible) path is what UBA's
	// directory table is keyed on — even if we opened newName, close should
	// finalise against the original. Path is dup'd into stub-owned memory
	// because the caller's buffer may be reused after we return.
	if (fd >= 0)
	{
		const char* dup = ubg_dup_path(path);
		uba::GlibcFdTrack(fd, dup, outCloseId);
	}

	(void)outSize;
	return fd;
}

// uba_glibc_openat — replaces glibc's __openat wrapper.
//
// SystemV-AMD64 args: rdi=dirfd, rsi=path, rdx=flags, rcx=mode (unused
// when O_CREAT is absent, but we always pass it through to keep the
// kernel-call ABI consistent).
//
// For Phase 1 the body is a raw-syscall passthrough that ALSO populates
// the shared fd→path map (UbaGlibcFdTable) when path resolution stays
// under UBA's view (AT_FDCWD or absolute path). Tracking enables
// uba_glibc_close (F1.B) to send a CloseFile RPC at close time and
// uba_glibc_fxstat (Phase 2) to synthesize stat results from the
// directory table.
//
// Path resolution that depends on `dirfd` (a real fd + a relative path)
// is delegated unchanged to the kernel and NOT tracked: UBA's directory
// table is keyed by absolute path, and resolving a dirfd-relative path
// against UBA's virtual cwd would require fd→path lookup on the parent
// dirfd plus path joining — out of scope for Phase 1.
//
// Mirrors the LD_PRELOAD shape in UbaDetoursFunctionsPosix.cpp's
// UBA_WRAPPER(openat): same AT_FDCWD shortcut, same fall-through to a
// real openat for dirfd-relative paths.
extern "C" __attribute__((used))
int uba_glibc_openat(int dirfd, const char* path, int flags, int mode)
{
	bool absolute   = ubg_path_is_absolute(path);
	bool atFdCwd    = (dirfd == AT_FDCWD);
	bool ubaHandles = atFdCwd || absolute;

	long r;
	if (ubaHandles)
	{
		// Path resolution stays under UBA's view — issue the real openat
		// against AT_FDCWD so the kernel resolves against the (possibly
		// virtualized) cwd UBA established. Keep the original `dirfd`
		// for non-absolute / non-CWD dirfds via the else-branch below.
		r = ubg_raw_syscall4(SYS_NR_OPENAT,
			(long)AT_FDCWD, (long)path, (long)flags, (long)mode);
	}
	else
	{
		// Relative path against a real dirfd — passthrough verbatim and
		// don't touch UBA bookkeeping.
		r = ubg_raw_syscall4(SYS_NR_OPENAT,
			(long)dirfd, (long)path, (long)flags, (long)mode);
	}

	long ret = ubg_translate_errno(r);

	// Track the fd → path mapping on success so close (F1.B) and
	// fxstat (future) can correlate. Skip for kernel-resolved relative
	// paths (we don't have the resolved absolute path).
	//
	// closeId == 0 means "do not RPC on close". F1.B's open hook will
	// produce a non-zero closeId when it goes through Rpc_CreateFileW;
	// our openat passthrough doesn't talk to UBA yet (that's a Phase 2
	// follow-on), so we mark the fd as "tracked, no RPC" — the entry
	// still helps fxstat synthesize a path-based stat in Phase 2.
	if (ret >= 0 && ubaHandles && path)
	{
		const char* dup = ubg_dup_path(path);
		uba::GlibcFdTrack((int)ret, dup, /*closeId=*/0);
	}

	DEBUG_LOG(TC("__libc_openat: dirfd=%d path=%s flags=0x%x mode=0%o -> %lld"),
		dirfd, path ? path : "(null)", (unsigned)flags, (unsigned)mode, (long long)ret);

	return (int)ret;
}

// uba_glibc_read — replaces glibc's __libc_read wrapper.
//
// SystemV-AMD64 args: rdi=fd, rsi=buf, rdx=n.
//
// UBA tracks reads at the fd level via the open-time CreateFile RPC, not
// per-syscall. Pure SYS_read passthrough with errno translation — matches
// the LD_PRELOAD detour, where the read wrapper is only present under the
// optional UBA_DETOUR_DEBUG build flag and is otherwise just a TRUE_WRAPPER
// call.
extern "C" __attribute__((used))
long uba_glibc_read(int fd, void* buf, unsigned long n)
{
	long r = ubg_raw_syscall3(0 /* SYS_read */, (long)fd, (long)buf, (long)n);
	return ubg_translate_errno(r);
}

// uba_glibc_write — replaces glibc's __libc_write wrapper.
//
// SystemV-AMD64 args: rdi=fd, rsi=buf, rdx=n.
//
// Per-write tracking is unnecessary: UBA's write bookkeeping happens at
// close time via fstat-of-the-final-fd inside uba_glibc_close (see
// Rpc_UpdateCloseHandle with fileSize / mtime / mode). The LD_PRELOAD
// production build also passes write through; the wrapper there only adds
// a log line under UBA_DETOUR_DEBUG.
extern "C" __attribute__((used))
long uba_glibc_write(int fd, const void* buf, unsigned long n)
{
	long r = ubg_raw_syscall3(1 /* SYS_write */, (long)fd, (long)buf, (long)n);
	return ubg_translate_errno(r);
}

// uba_glibc_close — replaces glibc's __libc_close wrapper.
//
// SystemV-AMD64 args: rdi=fd. Returns 0 on success, -1 (errno set) on error.
//
// Behaviour mirrors UBA_WRAPPER(close) → Shared_close
// (UbaDetoursFunctionsPosix.cpp:929). The LD_PRELOAD detour is the canonical
// reference but, like Shared_open, Shared_close is a file-private template
// not visible from the static stub — so the relevant slice is inlined here:
//   1. Pre-init: pure SYS_close passthrough.
//   2. Otherwise: take the (path, closeId) from the GlibcFdTable. If the fd
//      was registered with a non-zero closeId (i.e. open went through
//      Rpc_CreateFileW), dup the fd, fstat the dup to capture the final
//      size / mtime / mode, close the dup, then real-close the caller's fd
//      and fire Rpc_UpdateCloseHandle so UBA's CAS finalises the staging
//      file.
//
// Why dup+fstat (not lseek-end + post-close stat): the dup keeps the file
// description alive across the real close so any fdopen / stdio buffers
// flushed by the close are visible in fstat. Same rationale as the
// LD_PRELOAD path (UbaDetoursFunctionsPosix.cpp:986-1003).
extern "C" __attribute__((used))
int uba_glibc_close(int fd)
{
	using uba::u32;
	using uba::u64;

	// Pre-init: pure SYS_close passthrough. No fd table to consult; even if
	// there was, the RPC plumbing isn't ready yet.
	if (!__atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
	{
		long r = ubg_raw_syscall1(3 /* SYS_close */, (long)fd);
		return (int)ubg_translate_errno(r);
	}

	const char* path    = nullptr;
	u32         closeId = 0;
	bool tracked = uba::GlibcFdTake(fd, path, closeId);

	// If the fd was opened through __libc_open's RPC path, capture the
	// final file metadata BEFORE the real close runs. dup → fstat → close
	// the dup; then real-close the caller's fd. closeId == 0 means
	// "tracked but not RPC-registered" (e.g. an openat passthrough that
	// only seeded the path) — skip the metadata round-trip in that case.
	u64 fileSize      = 0;
	u64 lastWriteTime = 0;
	u32 attributes    = 0x8000u;   // S_IFREG fallback if fstat fails

	if (tracked && closeId)
	{
		long dupFd = ubg_raw_syscall1(32 /* SYS_dup */, (long)fd);
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
				// FromTimeSpec-equivalent: 100-ns ticks since epoch.
				lastWriteTime = mtimeSec * 10000000ull + mtimeNsec / 100;
			}
			(void)ubg_raw_syscall1(3 /* SYS_close */, dupFd);
		}
	}

	long r  = ubg_raw_syscall1(3 /* SYS_close */, (long)fd);
	int  rc = (int)ubg_translate_errno(r);

	DEBUG_LOG(TC("__libc_close: fd=%d (%s) size=%llu attr=0x%x -> %d (errno=%d)"),
		fd, path ? path : "(untracked)",
		(unsigned long long)fileSize, (u32)attributes, rc,
		(rc < 0 && &__errno_location && __errno_location()) ? *__errno_location() : 0);

	if (tracked && closeId)
	{
		uba::Rpc_UpdateCloseHandle(path, closeId, /*deleteOnClose=*/false,
			/*newName=*/"", uba::SharedMemoryHandle{},
			fileSize, lastWriteTime, attributes,
			/*success=*/(rc == 0));
	}

	return rc;
}

// uba_glibc_lseek — replaces glibc's __libc_lseek wrapper.
//
// SystemV-AMD64 args: rdi=fd, rsi=offset, rdx=whence. The kernel returns
// the new file offset on success (which can legitimately be very large
// for sparse files), or -errno on failure.
//
// lseek doesn't need any UBA logic — it just moves the file offset on a
// fd that was already tracked by uba_glibc_open / uba_glibc_openat. Pure
// passthrough.
extern "C" __attribute__((used))
long uba_glibc_lseek(int fd, long off, int whence)
{
	long r = ubg_raw_syscall3(SYS_NR_LSEEK, (long)fd, off, (long)whence);
	long ret = ubg_translate_errno(r);
	DEBUG_LOG(TC("__libc_lseek: fd=%d off=%lld whence=%d -> %lld"),
		fd, (long long)off, whence, (long long)ret);
	return ret;
}

// uba_glibc_fxstat — replaces glibc's __fxstat / __fxstat64 wrappers.
//
// SystemV-AMD64 args: rdi=ver, rsi=fd, rdx=buf. On x86-64 with eglibc 2.17
// `ver` is _STAT_VER == 1; the buffer layout matches the kernel `struct
// stat`. (The 32-bit divergence between __fxstat and __fxstat64 doesn't
// apply on x86-64 — both wrappers use the same kernel layout.)
//
// PHASE 1 BEHAVIOUR: pure passthrough to SYS_fstat.
//
// Synthesizing fxstat from UBA's directory table requires an fd→path
// lookup followed by a Rpc_GetEntryInformation call (mirroring the
// newfstatat case in uba_syscall_log). The fd→path map is now in place
// via UbaGlibcFdTable, but the actual synthesis is deferred to Phase 2:
// for qemu-system-x86_64 the only fxstat targets are real files the
// kernel knows about (the ELF being executed plus a couple of /proc
// entries), so the kernel's answer is authoritative for Phase 1's
// "qemu --version works end-to-end" goal.
//
// When Phase 2 wires in synthesis, the shape will be:
//   const char* path; u32 closeId;
//   if (uba::GlibcFdLookup(fd, path, closeId) && path)
//       try Rpc_GetEntryInformation(...) → fill statbuf, return 0.
//   else fall through to SYS_fstat.
extern "C" __attribute__((used))
int uba_glibc_fxstat(int ver, int fd, void* statbuf)
{
	(void)ver;   // _STAT_VER on x86-64 eglibc 2.17 — buffer layout fixed.
	long r = ubg_raw_syscall2(SYS_NR_FSTAT, (long)fd, (long)statbuf);
	long ret = ubg_translate_errno(r);
	DEBUG_LOG(TC("__fxstat: ver=%d fd=%d -> %lld (passthrough)"),
		ver, fd, (long long)ret);
	return (int)ret;
}

// uba_glibc_mmap — replaces glibc's __mmap / __mmap64 wrappers.
//
// SystemV-AMD64 args: rdi=addr, rsi=len, rdx=prot, rcx=flags, r8=fd, r9=off.
// (NB: at the libc-syscall boundary glibc moves rcx -> r10 because SYSCALL
// clobbers rcx; our handler receives args per the C ABI and re-stages them
// inside ubg_raw_syscall6.)
//
// MVP rationale: UBA's Shared_* surface doesn't synthesize file mappings;
// file-backed reads land via the prior open → fd → CreateFile RPC the
// __libc_open / __openat handlers fire. mmap therefore needs no UBA
// involvement — pass it to the kernel and translate the result.
//
// Kernel ABI: rax holds the new VA on success, or -errno in [-4095, -1] on
// failure. glibc's __mmap returns MAP_FAILED (= (void*)-1) on failure with
// errno set; on success the new VA. We mirror that.
extern "C" __attribute__((used))
void* uba_glibc_mmap(void* addr, unsigned long len, int prot, int flags, int fd, long off)
{
	long r = ubg_raw_syscall6(SYS_NR_MMAP,
		(long)addr, (long)len, (long)prot, (long)flags, (long)fd, (long)off);

	void* result;
	if (r < 0 && r >= -4095)
	{
		// Set errno via ubg_translate_errno's __errno_location path; its
		// -1 return matches MAP_FAILED on x86-64 (both are 0xFFFF...FFFF
		// when reinterpreted as a pointer), but cast explicitly for
		// clarity.
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

// uba_glibc_access — UBA-aware existence probe; replaces glibc's __access.
//
// LD_PRELOAD reference: Shared_access in UbaDetoursFunctionsPosix.cpp:1359
// fixes the path, devirtualises it, and asks the directory table via
// Shared_GetFileAttributes whether the entry exists. Returns 0 on exists,
// -1 (with errno) on miss. mode is informational — UBA doesn't track
// per-mode permissions; existence is the relevant bit.
//
// Static-detour adaptation: Shared_GetFileAttributes is gated by
// `#if !UBA_STUB_BUILD` (its body pulls in FixPath / VFS / StringBuffer
// storage we don't link), so we use Rpc_GetEntryInformation directly —
// the same primitive uba_syscall_log's newfstatat case uses to synthesise
// stat results (UbaStaticStubCore.cpp:1245). Stub callers operate on
// already-computed paths (qemu issues kernel-facing absolute paths) and
// skip FixPath.
//
// Before init OR on directory-table miss: passthrough to SYS_access. The
// kernel's answer is authoritative for paths UBA hasn't indexed.
extern "C" __attribute__((used))
int uba_glibc_access(const char* path, int mode)
{
	if (path && __atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
	{
		uba::u64 plen = 0;
		while (path[plen] && plen < 4096)
			++plen;
		if (plen > 0)
		{
			uba::StringView view(path, (uba::u32)plen);
			uba::StringKey  key  = uba::ToStringKeyNoCheck(path, plen);
			uba::DirectoryTable::EntryInformation info;
			if (uba::Rpc_GetEntryInformation(info, key, view, /*checkIfDir=*/false))
			{
				// Rpc_GetEntryInformation returns true only when the
				// directory table has a non-zero attributes value (see
				// UbaDetoursFileMappingTable.cpp:215). MVP ignores
				// R_OK/W_OK/X_OK — qemu's __access calls are existence
				// probes, not permission checks. Phase 3 may translate
				// `mode` against info.attributes if a target needs it.
				DEBUG_LOG(TC("__libc_access: %s mode=0x%x -> 0 (UBA exists, attr=0x%x)"),
					path, (unsigned)mode, (uba::u32)info.attributes);
				return 0;
			}
			// Not in UBA's directory table — fall through to kernel.
		}
	}

	long r = ubg_raw_syscall2(SYS_NR_ACCESS, (long)path, (long)mode);
	int ret = (int)ubg_translate_errno(r);
	DEBUG_LOG(TC("__libc_access: %s mode=0x%x -> %d (passthrough)"),
		path ? path : "(null)", (unsigned)mode, ret);
	return ret;
}

// uba_glibc_exit — replaces glibc's _exit / _Exit.  Glibc's wrapper just
// issues SYS_exit_group; we send MessageType_Exit on the way past so the
// UBA server marks the process as cleanly exited (otherwise UbaCli logs
// "Process not active but did not get exit message" and synthesises a
// garbage signal code).
//
// Hook position note: `exit(int)` (the higher-level libc function that
// flushes stdio and runs atexit handlers) ultimately calls `_exit(int)`,
// so hooking the latter catches every well-behaved exit path including
// `return` from `main`.  Crashes via SIGSEGV / SIGABRT bypass this and
// remain a no-message exit on the UbaCli side — same behaviour as the
// LD_PRELOAD path.
//
// The `exit` table-name resolves through kGlibcAliasTable to "_exit" or
// "_Exit" (glibc emits both as a STT_FUNC GLOBAL alias of the same
// address).  The patcher writes a 5-byte JMP rel32 over the wrapper,
// destroying its body — control reaches us, we send the RPC, then we
// issue SYS_exit_group ourselves in an infinite loop so the compiler
// can't fall through into adjacent code.
extern "C" __attribute__((noreturn, used))
void uba_glibc_exit(int status)
{
	if (__atomic_load_n(&g_initDone, __ATOMIC_ACQUIRE))
		uba::SendExitRpc((uba::u32)status);
	for (;;)
		ubg_raw_syscall1(231 /* SYS_exit_group */, (long)status);
}
