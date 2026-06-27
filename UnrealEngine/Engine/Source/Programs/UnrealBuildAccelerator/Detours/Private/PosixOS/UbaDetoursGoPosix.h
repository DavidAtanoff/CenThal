// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaDefinitions.h"
#include "UbaPlatform.h"
#include <sys/stat.h>

namespace uba
{
#if UBA_SUPPORTS_GO

	bool IsGoBinary(const char* path);
	void PatchGoSyscalls();
	void PatchGoExit();

	// Raw Linux syscall with no glibc involvement (no errno TLS, no wrappers).
	// Safe to call from any thread, including goroutine OS threads that lack a
	// proper glibc TCB (the original motivation for this helper).
	// On error: r1 is -errno (negative), r2 is 0.
	// On success: r1 is the return value, r2 is rdx after syscall (usually 0).
	struct RawSyscallResult { long r1; long r2; };
	inline RawSyscallResult RawSyscallDirect(long nr, long a1, long a2, long a3, long a4 = 0, long a5 = 0, long a6 = 0)
	{
	    register long rax __asm__("rax") = nr;
	    register long rdx __asm__("rdx") = a3;
	    register long r10 __asm__("r10") = a4;
	    register long r8  __asm__("r8")  = a5;
	    register long r9  __asm__("r9")  = a6;
	    __asm__ volatile (
	        "syscall"
	        : "+a" (rax), "+d" (rdx)
	        : "D" (a1), "S" (a2), "r" (r10), "r" (r8), "r" (r9)
	        : "memory", "rcx", "r11"
	    );
	    return { rax, rdx };
	}

	// Each function returns true if the syscall was intercepted (caller should use *result/*errOut), or false to pass the syscall through to the kernel.
	bool GoHandleOpenat(int dirfd, const char* path, int flags, int mode, long* result, int* errOut);
	bool GoHandleNewfstatat(int dirfd, const char* path, struct stat* buf, int flags, long* result, int* errOut);
	bool GoHandleStatx(int dirfd, const char* path, int flags, unsigned int mask, struct statx* buf, long* result, int* errOut);
	bool GoHandleUnlinkat(int dirfd, const char* path, int flags, long* result, int* errOut);
	bool GoHandleClose(int fd, long* result, int* errOut);
	bool GoHandleGetdents64(int fd, void* buf, unsigned int count, long* result, int* errOut);

#endif
}
