// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaFile.h"
#include "UbaStringBuffer.h"

#define CHECK_TRUE(x) \
	do { \
	if (!(x)) \
		return logger.Error(TC("Failed %s (%s:%u)"), TC(#x), TC("") __FILE__, __LINE__); \
	} while (false)

#define CHECK_TRUEF(x, ...) \
	do { \
	if (!(x)) \
		return logger.Logf(LogEntryType_Error, ##__VA_ARGS__).ToFalse(); \
	} while (false)

namespace uba
{
	class Logger;
	class LoggerWithWriter;
	class ProcessHandle;
	class SessionServer;
	struct ProcessStartInfo;

	bool CreateTestFile(Logger& logger, StringView testRootDir, StringView fileName, StringView content, u32 attributes = DefaultAttributes());
	bool CreateTestFile(StringBufferBase& outFile, Logger& logger, StringView testRootDir, StringView fileName, StringView content, u32 attributes = DefaultAttributes());
	bool DeleteTestFile(Logger& logger, StringView testRootDir, StringView fileName);
	bool FileExists(Logger& logger, StringView dir, StringView fileName);
	bool FilesEqual(Logger& logger, const tchar* file1, const tchar* file2);

	using RunProcessFunction = Function<ProcessHandle(ProcessStartInfo&)>;
	using TestSessionFunction = Function<bool(LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess, uintptr_t extraData)>;
	bool RunLocal(LoggerWithWriter& logger, const StringBufferBase& testRootDir, const TestSessionFunction& testFunc, uintptr_t extraData = 0, bool enableDetour = true);
	bool RunRemote(LoggerWithWriter& logger, const StringBufferBase& testRootDir, const TestSessionFunction& testFunc, bool deleteAll = true, bool serverShouldListen = true);
	void GetTestAppPath(LoggerWithWriter& logger, StringBufferBase& out);
	const tchar* GetSystemApplication();
	const tchar* GetSystemArguments();

	struct TrackSystemUsage
	{
		TrackSystemUsage(Logger& logger);
		~TrackSystemUsage();
		Logger& logger;
		u32 startHandleCount;
	};
}