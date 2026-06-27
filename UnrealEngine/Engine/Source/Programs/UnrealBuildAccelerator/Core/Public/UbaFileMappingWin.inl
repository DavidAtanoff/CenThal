// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

namespace uba
{
	////////////////////////////////////////////////////////////////////////////////////////////////////

	HANDLE g_hostProcess;
	#include "UbaFileMapping.inl"

	////////////////////////////////////////////////////////////////////////////////////////////////////

	bool FileMapping_GetWineBackend(FileMappingBackend& out, Logger& logger, HMODULE wineDll)
	{
		#define UBA_FM_FUNC(func) \
			auto func##Func = (decltype(FileMapping_##func)*)GetProcAddress(wineDll, "FileMapping_" #func); \
			if (!func##Func) return logger.Warning(TC("FileMapping_" #func " is not exported from wine dll"));
		UBA_FM_FUNCTIONS
		#undef UBA_FM_FUNC

		#define UBA_FM_FUNC(func) out.func = func##Func;
		UBA_FM_FUNCTIONS
		#undef UBA_FM_FUNC
		out.linuxBackend = true;
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////
}
