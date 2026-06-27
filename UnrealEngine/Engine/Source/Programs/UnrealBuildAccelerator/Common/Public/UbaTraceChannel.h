// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaEnvironment.h"
#include "UbaFileMappingHandle.h"
#include "UbaLogger.h"

#define UBA_USE_TRACE_CHANNEL_FILE !PLATFORM_WINDOWS

namespace uba
{
	////////////////////////////////////////////////////////////////////////////////////////////////////

	class TraceChannel
	{
	public:
		TraceChannel(Logger& logger);
		~TraceChannel();

		bool Init(const tchar* channelName = TC("Default"));
		void Deinit();
		bool Write(const tchar* traceName, const tchar* ifMatching = nullptr);
		bool Read(StringBufferBase& outTraceName);
		bool IsInitialized();

		Logger& m_logger;

		#if UBA_USE_TRACE_CHANNEL_FILE
		TString m_channelFile;
		struct timespec m_lastModified;
		#else
		MutexHandle m_mutex = InvalidMutexHandle;
		FileMappingHandle m_memHandle;
		void* m_mem = nullptr;
		#endif
	};

	////////////////////////////////////////////////////////////////////////////////////////////////////

}
