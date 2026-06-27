// Copyright Epic Games, Inc. All Rights Reserved.
//
// UbaGlibcFdTable.h — fd → (path, closeId) map shared between the glibc
// detour handlers (Phase 1).
//
// PURPOSE
//   Multiple handlers need to know what path / closeId a given fd was
//   opened against:
//     - uba_glibc_close   (F1.B): needs the path + closeId to send the
//                                  CloseFile / Rpc_UpdateCloseHandle RPC
//                                  before the kernel close runs.
//     - uba_glibc_fxstat  (F1.C): would need the path to synthesize a
//                                  struct stat from Rpc_GetEntryInformation
//                                  (Phase 2/3 — Phase 1 just passes through).
//   Open-side handlers populate the map; consumers look up by fd and
//   either pop or peek depending on the call shape.
//
// CONTRACT
//   Producers:
//     - uba_glibc_open   (F1.B) inserts after a successful kernel open.
//     - uba_glibc_openat (F1.C) inserts after a successful kernel openat,
//                                only when path resolution stays under
//                                UBA's view (AT_FDCWD or absolute path).
//   Consumers:
//     - uba_glibc_close  (F1.B) takes (removes) on close.
//     - uba_glibc_fxstat (F1.C, future) peeks (no removal).
//
// IMPLEMENTATION NOTES
//   - Open-addressed linear-probed table, fixed N=256 slots.
//     Identical shape and hash to g_fdTable in UbaStaticStubCore.cpp,
//     so the wire layout is familiar. Same caveat applies: at >75% fill
//     inserts silently drop. AOSP/qemu workloads stay well under that.
//   - Path strings are allocated via uba::StubAllocatorAllocate so they
//     outlive the caller's buffer (qemu / glibc may reuse path arg
//     storage between open() and close()). The arena leaks at process
//     exit, which is fine for the stub's lifetime model.
//   - Empty-slot sentinel: fd == 0xFFFFFFFFu. Not initialised at static-
//     init time (no _init in the stub blob); GlibcFdTrack lazily zeroes
//     all slots on first call via a one-shot atomic guard.
//   - No locks. The static-Go bridge runs at most one handler at a time
//     under its alt-stack/spinlock scheme; for glibc-static qemu (real
//     pthreads) concurrent open+close on different fds is rare and the
//     race window is benign (worst case: a duplicate insert overwrites
//     a stale slot).
//
// SEE ALSO
//   - UbaStaticStubCore.cpp StubFdEntry / g_fdTable (same pattern, used
//     by the Go-static syscall bridge).

#pragma once

#include "UbaBase.h"

namespace uba
{
	// Insert (or update) the (fd, path, closeId) tuple. `path` must be
	// stable for the map's lifetime — callers should dup into the
	// StubAllocator-owned arena before invoking. Pass closeId == 0 to
	// record an fd whose close should NOT trigger an Rpc_UpdateCloseHandle
	// (e.g. an openat the glibc handler chose to skip-track).
	void GlibcFdTrack(int fd, const char* path, uba::u32 closeId);

	// Look up `fd` and write (path, closeId) into the out-params if found.
	// Returns true on hit, false on miss. Does NOT remove the entry.
	bool GlibcFdLookup(int fd, const char*& outPath, uba::u32& outCloseId);

	// Look up and remove the entry for `fd`. Returns true on hit.
	bool GlibcFdTake(int fd, const char*& outPath, uba::u32& outCloseId);
}
