// Copyright Epic Games, Inc. All Rights Reserved.
//
// UbaGlibcHandlers.h — Phase 0 contract for the glibc-static detour bridge.
//
// PURPOSE
//   The glibc-static detour patches a 5-byte JMP rel32 over each libc
//   wrapper symbol's first instruction (e.g. __libc_open). Control jumps
//   into one of the handlers declared below, which run in the context of
//   the patched binary, perform the UBA-mediated work, and return via the
//   SystemV-AMD64 ABI back to the wrapper's caller — the wrapper body
//   itself never executes.
//
// CONTRACT
//   Each handler takes the same SystemV-ABI signature as the libc wrapper
//   it stands in for, so the 5-byte JMP at the wrapper does all the ABI
//   work the kernel would have done. No per-handler asm shim needed.
//
// SCOPE
//   Phase 0 declares the 9 MVP symbols (the strace dominators for
//   qemu-system-x86_64 — see Claude/memory/glibc_static_path_forward.md
//   §7). Bodies live in UbaGlibcHandlers.cpp. Phase 1 replaces the empty
//   stubs with real bodies that call into the existing static-detour
//   bridge primitives.
//
// LAYOUT
//   The GlibcHandlerTable struct below is materialised once at
//   .text.uba_handlers in the stub blob (via UbaGlibcHandlerTable.cpp).
//   The patcher locates it inside the blob by scanning for the magic
//   value, reads each entry's offset, and uses
//     handler_addr_in_target = blob_base_in_target + entries[i].offset
//   to resolve the JMP destination at patch time.
//
// SEE ALSO
//   - UbaStaticPatcher.h / .cpp        — patcher entry point
//   - UbaGlibcHandlerTable.cpp         — table definition
//   - DetoursStatic/UbaStaticStub.S    — bridge primitives shared with
//                                        the Go-static path

#pragma once

#include "UbaBase.h"

namespace uba
{
	using ::uba::u32;
	using ::uba::u64;
}

// -----------------------------------------------------------------------------
// Handler prototypes (SystemV AMD64 ABI, signatures identical to the libc
// wrappers they replace).
//
// Phase 1 must implement these exactly — Phase 0 only stubs them out.
// -----------------------------------------------------------------------------

extern "C" int   uba_glibc_open(const char* path, int flags, int mode);
extern "C" int   uba_glibc_openat(int dirfd, const char* path, int flags, int mode);
extern "C" long  uba_glibc_read(int fd, void* buf, unsigned long n);
extern "C" long  uba_glibc_write(int fd, const void* buf, unsigned long n);
extern "C" int   uba_glibc_close(int fd);
extern "C" long  uba_glibc_lseek(int fd, long off, int whence);
extern "C" int   uba_glibc_fxstat(int ver, int fd, void* statbuf);
extern "C" void* uba_glibc_mmap(void* addr, unsigned long len, int prot, int flags, int fd, long off);
extern "C" int   uba_glibc_access(const char* path, int mode);
// Glibc's _exit / _Exit terminates the process via SYS_exit_group. Hooking
// it lets the stub send MessageType_Exit before the kernel tears the
// process down — equivalent role to runtime.exit.abi0 for the Go path.
// Marked noreturn so the patcher's 5-byte JMP rel32 destination matches
// glibc's own signature (the wrapper body is dead after the patch).
extern "C" __attribute__((noreturn))
              void uba_glibc_exit(int status);

// -----------------------------------------------------------------------------
// Handler table.
//
// Magic = 'UBAGLBHT' packed little-endian. Verify with hexdump:
//   bytes 'U','B','A','G','L','B','H','T' = 0x55 0x42 0x41 0x47 0x4C 0x42 0x48 0x54
//   little-endian u64 = 0x5448424C47414255
// -----------------------------------------------------------------------------

static constexpr uba::u64 kGlibcHandlerTableMagic   = 0x5448424C47414255ULL; // 'UBAGLBHT' LE
static constexpr uba::u32 kGlibcHandlerTableMaxEntries = 16;                 // room to grow

// Entry kind selects what the patcher does with `offset` and `name`.
//   HandlerHook : `offset` is the blob-relative VA of a stub handler. The
//                 patcher writes a 5-byte JMP rel32 at the target's libc
//                 wrapper symbol named by an entry in kGlibcAliasTable
//                 whose tableName == this entry's `name`. (Existing path.)
//   ImportSlot  : `offset` is the blob-relative VA of an 8-byte writable
//                 slot inside the stub blob. The patcher resolves the
//                 target symbol whose name is exactly `name`, and writes
//                 its 8-byte VA into blob[offset .. offset+8). Used to
//                 stamp the target's __errno_location into the stub so
//                 our handlers can set glibc's per-thread errno.
enum GlibcEntryKind : uba::u32
{
	GlibcEntry_HandlerHook = 0,
	GlibcEntry_ImportSlot  = 1,
};

struct GlibcHandlerEntry
{
	char       name[24];   // HandlerHook: stripped libc name (e.g. "open").
	                        // ImportSlot:  exact target symbol (e.g. "__errno_location").
	uba::u32   offset;     // bytes from blob base
	uba::u32   kind;       // GlibcEntryKind
};                          // 32 bytes, alignment-friendly

static_assert(sizeof(GlibcHandlerEntry) == 32, "GlibcHandlerEntry must be exactly 32 bytes");

struct GlibcHandlerTable
{
	uba::u64           magic;     // = kGlibcHandlerTableMagic
	uba::u32           count;     // number of valid entries (<= kGlibcHandlerTableMaxEntries)
	uba::u32           reserved;
	GlibcHandlerEntry  entries[kGlibcHandlerTableMaxEntries];
};

static_assert(sizeof(GlibcHandlerTable) == 16 + 32 * kGlibcHandlerTableMaxEntries,
	"GlibcHandlerTable layout must be packed: 16-byte header + 32-byte entries");

// The handler table instance lives in UbaGlibcHandlerTable.cpp, in its
// own ".text.uba_handlers" section. We deliberately do NOT publish a
// forward `extern` declaration here: nothing inside the stub blob
// references the table by symbol; the patcher resolves it inside the
// embedded blob bytes by scanning for kGlibcHandlerTableMagic at u64-
// aligned offsets (same self-locating-sentinel pattern as the existing
// Syscall6 / runtime.exit hooks).
