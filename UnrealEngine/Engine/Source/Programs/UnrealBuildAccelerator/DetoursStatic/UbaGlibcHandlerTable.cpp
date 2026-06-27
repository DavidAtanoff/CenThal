// Copyright Epic Games, Inc. All Rights Reserved.
//
// UbaGlibcHandlerTable.cpp — Phase 0 instance of GlibcHandlerTable.
//
// LAYOUT NOTE
//   We place the table in a dedicated section ".text.uba_handlers" so the
//   linker keeps it together and it ends up early in the stub blob (right
//   after .text.entry — see UbaStaticStub.ld). The patcher resolves the
//   table inside the embedded blob by scanning for kGlibcHandlerTableMagic
//   at u64-aligned offsets — this is the same self-locating-sentinel
//   pattern used for the existing Syscall6 / runtime.exit hooks (see
//   UbaStaticPatcher.cpp). No fixed numeric offset has to agree between
//   the linker script and the patcher.
//
// OFFSET-FIELD STAMPING
//   Each entry's `offset` is intentionally 0 in this Phase 0 scaffold.
//   The proper post-link offset stamping is owned by Phase 1 (Build.sh
//   change). Phase 1.A's patcher must treat a zero `offset` as
//   "not yet stamped" and refuse to patch (clear error message) so we
//   don't silently install JMPs that point at the blob base.
//
//   TODO(phase1): Build.sh post-link step computes
//     entries[i].offset = (uba_glibc_<name> address) - (g_glibcHandlerTable
//     address) + (table offset within blob)
//   and stamps each LE-u32 in place inside the extracted blob.

#include "UbaGlibcHandlers.h"

// The "used" attribute prevents LTO/dead-strip from removing the table.
// "section" routes it to .text.uba_handlers which the linker script keeps
// adjacent to .text.entry (see UbaStaticStub.ld).
extern "C" __attribute__((section(".text.uba_handlers"), used))
const GlibcHandlerTable g_glibcHandlerTable = {
	/* magic    */ kGlibcHandlerTableMagic,
	/* count    */ 11,
	/* reserved */ 0,
	/* entries  */ {
		// name (≤ 23 chars), offset (stamped post-link), kind
		{ "open",              0, GlibcEntry_HandlerHook },
		{ "openat",            0, GlibcEntry_HandlerHook },
		{ "read",              0, GlibcEntry_HandlerHook },
		{ "write",             0, GlibcEntry_HandlerHook },
		{ "close",             0, GlibcEntry_HandlerHook },
		{ "lseek",             0, GlibcEntry_HandlerHook },
		{ "fxstat",            0, GlibcEntry_HandlerHook },
		{ "mmap",              0, GlibcEntry_HandlerHook },
		{ "access",            0, GlibcEntry_HandlerHook },
		// Hook glibc's _exit / _Exit so the stub sends MessageType_Exit
		// before SYS_exit_group runs.  Equivalent role to
		// runtime.exit.abi0 in the Go-static path.  Patcher resolves
		// "_exit" or "_Exit" via kGlibcAliasTable and stamps a 5-byte
		// JMP rel32 to uba_glibc_exit at blob+offset.
		{ "exit",              0, GlibcEntry_HandlerHook },
		// Import slot — patcher resolves `__errno_location` in the target
		// and stamps its 8-byte VA at blob+offset. Build.sh sets `offset`
		// to the file offset of `g_glibc_errno_hook_fn` in the stub blob.
		{ "__errno_location",  0, GlibcEntry_ImportSlot  },
		// Remaining (kGlibcHandlerTableMaxEntries - 11) slots zero-init.
		// Future agents may extend by bumping `count` and adding entries.
	},
};
