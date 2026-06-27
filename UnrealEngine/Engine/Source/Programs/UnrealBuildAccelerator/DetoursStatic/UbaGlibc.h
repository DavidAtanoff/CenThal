// Copyright Epic Games, Inc. All Rights Reserved.
//
// UbaGlibc.h — glibc-static detour bridge contract.
//
// Consolidation note: this header replaces the earlier trio
//   UbaGlibcHandlers.h / UbaGlibcFdTable.h / UbaGlibcHandlerTable.cpp
// with a single header + single implementation (UbaGlibc.cpp). The older
// split was an artefact of the Phase-1 parallel-agent rollout.
//
// CONTENTS
//   1. GlibcEntryKind enum + GlibcHandlerEntry / GlibcHandlerTable layout
//      (the self-locating blob structure the patcher walks at injection
//      time to install its 5-byte JMP rel32 hooks and stamp ImportSlot
//      function pointers).
//   2. Per-handler prototypes — one C function per table entry. The
//      patcher writes a JMP over each libc wrapper's first five bytes
//      pointing to the corresponding uba_glibc_<name>.
//   3. Fd-tracking table API — GlibcFdTrack / GlibcFdLookup / GlibcFdTake
//      shared between uba_glibc_open / openat / close / fxstat so the
//      close hook can emit Rpc_UpdateCloseHandle with the original path.
//
// DESIGN
//   * All handlers use the SystemV-AMD64 ABI exactly as the libc wrapper
//     they replace does, so the 5-byte JMP at patch time does all the
//     register setup the kernel would have done. No per-handler asm shim.
//   * Unsupported operations are intentionally wired to abort handlers
//     (uba_glibc_<op> that write "[uba_stub] unsupported glibc call: <op>"
//     to stderr and exit_group). We don't know which libc calls a given
//     build script will reach; a loud abort is strictly better than a
//     silent passthrough that corrupts UBA's view of the filesystem.

#pragma once

#include "UbaBase.h"

namespace uba
{
	using ::uba::u32;
	using ::uba::u64;
}

// -----------------------------------------------------------------------------
// Handler-table layout
// -----------------------------------------------------------------------------
// The patcher locates g_glibcHandlerTable inside the embedded blob by
// scanning for kGlibcHandlerTableMagic ('UBAGLBHT' LE) at u64 alignment.
// Entries are walked in order; each dispatches on `kind`:
//
//   HandlerHook (0) — install a 5-byte JMP rel32 at the libc wrapper
//                     named by kGlibcAliasTable[tableName==`name`].
//                     Destination: blob_base + `offset`.
//   ImportSlot  (1) — resolve the target symbol whose name equals
//                     `name` and stamp its 8-byte VA at blob+`offset`
//                     so stub code can call back into target libc
//                     (currently used only for __errno_location).

static constexpr uba::u64 kGlibcHandlerTableMagic      = 0x5448424C47414255ULL; // 'UBAGLBHT' LE
static constexpr uba::u32 kGlibcHandlerTableMaxEntries = 64;                     // room for abort-handler growth

enum GlibcEntryKind : uba::u32
{
	GlibcEntry_HandlerHook = 0,
	GlibcEntry_ImportSlot  = 1,
};

struct GlibcHandlerEntry
{
	char       name[24];   // HandlerHook: stripped libc name (e.g. "open")
	                        // ImportSlot:  exact target symbol name
	uba::u32   offset;     // bytes from blob base (stamped post-link by Build.sh)
	uba::u32   kind;       // GlibcEntryKind
};

static_assert(sizeof(GlibcHandlerEntry) == 32, "GlibcHandlerEntry must be exactly 32 bytes");

struct GlibcHandlerTable
{
	uba::u64           magic;     // = kGlibcHandlerTableMagic
	uba::u32           count;     // number of valid entries
	uba::u32           reserved;
	GlibcHandlerEntry  entries[kGlibcHandlerTableMaxEntries];
};

static_assert(sizeof(GlibcHandlerTable) == 16 + 32 * kGlibcHandlerTableMaxEntries,
	"GlibcHandlerTable must be packed: 16-byte header + 32-byte entries");

// -----------------------------------------------------------------------------
// Handler prototypes (SystemV AMD64 ABI, matching the libc wrappers)
// -----------------------------------------------------------------------------
//
// Implemented (real bodies — talk to UBA's RPC / fd table / directory table):
extern "C" int   uba_glibc_open(const char* path, int flags, int mode);
extern "C" int   uba_glibc_openat(int dirfd, const char* path, int flags, int mode);
extern "C" long  uba_glibc_read(int fd, void* buf, unsigned long n);
extern "C" long  uba_glibc_write(int fd, const void* buf, unsigned long n);
extern "C" int   uba_glibc_close(int fd);
extern "C" long  uba_glibc_lseek(int fd, long off, int whence);
extern "C" int   uba_glibc_fxstat(int ver, int fd, void* statbuf);
extern "C" void* uba_glibc_mmap(void* addr, unsigned long len, int prot, int flags, int fd, long off);
extern "C" int   uba_glibc_access(const char* path, int mode);
extern "C" __attribute__((noreturn)) void uba_glibc_exit(int status);

// Abort-on-reach handlers. Each prints a one-line diagnostic to stderr
// and exit_group()s with 134 (SIGABRT-ish). The intent is to surface any
// unsupported libc call in a real workload instead of silently doing
// nothing useful. Added as HandlerHook entries — if the target lacks the
// symbol, the patcher skips the entry; if present, the wrapper is
// replaced with a JMP here.
extern "C" __attribute__((noreturn)) void uba_glibc_stat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_lstat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_fstatat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_statx(void);
extern "C" __attribute__((noreturn)) void uba_glibc_unlink(void);
extern "C" __attribute__((noreturn)) void uba_glibc_unlinkat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_rename(void);
extern "C" __attribute__((noreturn)) void uba_glibc_renameat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_renameat2(void);
extern "C" __attribute__((noreturn)) void uba_glibc_link(void);
extern "C" __attribute__((noreturn)) void uba_glibc_linkat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_symlink(void);
extern "C" __attribute__((noreturn)) void uba_glibc_symlinkat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_readlink(void);
extern "C" __attribute__((noreturn)) void uba_glibc_readlinkat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_truncate(void);
extern "C" __attribute__((noreturn)) void uba_glibc_ftruncate(void);
extern "C" __attribute__((noreturn)) void uba_glibc_mkdir(void);
extern "C" __attribute__((noreturn)) void uba_glibc_mkdirat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_rmdir(void);
extern "C" __attribute__((noreturn)) void uba_glibc_fork(void);
extern "C" __attribute__((noreturn)) void uba_glibc_vfork(void);
extern "C" __attribute__((noreturn)) void uba_glibc_clone(void);
extern "C" __attribute__((noreturn)) void uba_glibc_clone3(void);
extern "C" __attribute__((noreturn)) void uba_glibc_execve(void);
extern "C" __attribute__((noreturn)) void uba_glibc_execveat(void);
extern "C" __attribute__((noreturn)) void uba_glibc_dup(void);
extern "C" __attribute__((noreturn)) void uba_glibc_dup2(void);
extern "C" __attribute__((noreturn)) void uba_glibc_dup3(void);
extern "C" __attribute__((noreturn)) void uba_glibc_pipe(void);
extern "C" __attribute__((noreturn)) void uba_glibc_pipe2(void);
extern "C" __attribute__((noreturn)) void uba_glibc_opendir(void);
extern "C" __attribute__((noreturn)) void uba_glibc_fdopendir(void);
extern "C" __attribute__((noreturn)) void uba_glibc_readdir(void);
extern "C" __attribute__((noreturn)) void uba_glibc_closedir(void);
extern "C" __attribute__((noreturn)) void uba_glibc_getdents64(void);
extern "C" __attribute__((noreturn)) void uba_glibc_chdir(void);
extern "C" __attribute__((noreturn)) void uba_glibc_fchdir(void);

// -----------------------------------------------------------------------------
// fd → (path, closeId) map — shared between open / openat / close / fxstat
// -----------------------------------------------------------------------------
//
// Producers: uba_glibc_open (always), uba_glibc_openat (only when UBA
//            handles the path — AT_FDCWD or absolute).
// Consumers: uba_glibc_close (Take), uba_glibc_fxstat (Lookup; Phase 2
//            will use this to synthesise stat from the directory table).
//
// Open-addressed linear-probed fixed-size (256 slots). Path strings are
// dup'd into the StubAllocator arena so they outlive the caller's
// buffer. Sentinel: fd == 0xFFFFFFFFu. No locks — racers worst case see
// an old slot overwritten, which is observation-only bookkeeping.

namespace uba
{
	void GlibcFdTrack(int fd, const char* path, uba::u32 closeId);
	bool GlibcFdLookup(int fd, const char*& outPath, uba::u32& outCloseId);
	bool GlibcFdTake(int fd, const char*& outPath, uba::u32& outCloseId);
}
