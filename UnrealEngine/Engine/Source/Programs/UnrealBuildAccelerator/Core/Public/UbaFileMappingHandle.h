// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaBase.h"

namespace uba
{
	////////////////////////////////////////////////////////////////////////////////////////////////////

	struct FileMappingHandle
	{
		FileMappingHandle() = default;
		bool operator==(const FileMappingHandle& o) const { return internal == o.internal; }
		bool IsValid() const { return internal != 0; }
		u64 ToU64() const { return internal; }
		static FileMappingHandle FromU64(u64 v) { return { v }; }

		FileMappingHandle(u64 v) { internal = v; }
		u64 internal = 0; // HANDLE on windows, uid on posix

		int shmFd = -1; // Unused on windows
		int lockFd = -1; // Unused on windows
	};

	////////////////////////////////////////////////////////////////////////////////////////////////////

	#if PLATFORM_WINDOWS
	inline HANDLE AsHANDLE(FileMappingHandle h) { return (HANDLE)(uintptr_t)u32(h.internal); }
	#endif

	////////////////////////////////////////////////////////////////////////////////////////////////////
}
