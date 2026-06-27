// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaTestAll.h"
#include "UbaFileAccessor.h"

namespace uba
{
	bool CreateTestFile(Logger& logger, StringView testRootDir, StringView fileName, StringView content, u32 attributes)
	{
		StringBuffer<> testFileName;
		return CreateTestFile(testFileName, logger, testRootDir, fileName, content, attributes);
	}

	bool CreateTestFile(StringBufferBase& outFile, Logger& logger, StringView testRootDir, StringView fileName, StringView content, u32 attributes)
	{
		outFile.Clear().Append(testRootDir).EnsureEndsWithSlash().Append(fileName).FixPathSeparators();

		if (fileName.Contains(PathSeparator) || fileName.Contains(NonPathSeparator))
		{
			StringBuffer<> testFileDir;
			testFileDir.AppendDir(outFile);
			DirectoryCache().CreateDirectory(logger, testFileDir.data);
		}

		u64 bytes = content.count*sizeof(tchar);
		FileAccessor file(logger, outFile.data);
		CHECK_TRUEF(file.CreateMemoryWrite(false, attributes, bytes), TC("Failed to create file for write"));
		memcpy(file.GetData(), content.data, bytes);
		return file.Close();
	}

	bool DeleteTestFile(Logger& logger, StringView testRootDir, StringView fileName)
	{
		StringBuffer<> testFileName;
		testFileName.Append(testRootDir).EnsureEndsWithSlash().Append(fileName).FixPathSeparators();
		CHECK_TRUEF(DeleteFileW(testFileName.data), TC("Failed to delete test file %s"), fileName.data);
		return true;
	}

	bool FileExists(Logger& logger, StringView dir, StringView fileName)
	{
		StringBuffer<> testFileName;
		testFileName.Append(dir).EnsureEndsWithSlash().Append(fileName).FixPathSeparators();
		return FileExists(logger, testFileName.data);
	}

	bool FilesEqual(Logger& logger, const tchar* file1, const tchar* file2)
	{
		FileAccessor f1(logger, file1);
		CHECK_TRUE(f1.OpenMemoryRead());
		FileAccessor f2(logger, file2);
		CHECK_TRUE(f2.OpenMemoryRead());
		CHECK_TRUE(f1.GetSize() == f2.GetSize());
		return memcmp(f1.GetData(), f2.GetData(), f1.GetSize()) == 0;
	}

	bool WrappedMain(int argc, tchar* argv[])
	{
		AddExceptionHandler();
		return RunTests(argc, argv);
	}

	TrackSystemUsage::TrackSystemUsage(Logger& l) : logger(l)
	{
		Event::ClearCache();
		#if PLATFORM_WINDOWS
		GetProcessHandleCount(GetCurrentProcess(), (DWORD*)&startHandleCount);
		#endif
	}

	TrackSystemUsage::~TrackSystemUsage()
	{
		Event::ClearCache();
		#if PLATFORM_WINDOWS
		u32 endHandleCount = 0;
		GetProcessHandleCount(GetCurrentProcess(), (DWORD*)&endHandleCount);
		if (startHandleCount != endHandleCount)
			logger.Error(TC("Handles were leaked while running test (%u)"), endHandleCount - startHandleCount);
		#endif
	}
}

#if PLATFORM_WINDOWS
int wmain(int argc, wchar_t* argv[])
{
	return uba::WrappedMain(argc, argv) ? 0 : -1;
}
#else
int main(int argc, char* argv[])
{
	return uba::WrappedMain(argc, argv) ? 0 : -1;
}
#endif
