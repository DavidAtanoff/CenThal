// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaDirectoryTable.h"
#include "UbaFile.h"
#include "UbaFileMappingHandle.h"
#include "UbaMap.h"

namespace uba
{
	class Logger;

	////////////////////////////////////////////////////////////////////////////////////////////////////

	struct DirectoryTableOverlay
	{
		ReaderWriterLock lock;
		FileMappingHandle handle;
		u8* memory = nullptr;
		u32 committed = 0;
		u32 size = 0;
		struct Dir { u32 offset = 0; UnorderedMap<StringKey, u32> files; };
		UnorderedMap<StringKey, Dir> lookup;
		Vector<StringKey> roots;
		Set<TString> createdDirectories;
		Set<TString> removedDirectories;
		Set<TString> deletedFiles;
		Map<TString, TString> symlinks;
	};

	////////////////////////////////////////////////////////////////////////////////////////////////////

	class DirectoryTableHolder
	{
	public:
		DirectoryTableHolder(LogWriter& logWriter, const tchar* logPrefix = TC(""), bool runningRemote = false);
		virtual ~DirectoryTableHolder();
		void SetTreatTempDirAsEmpty(); // We never want to populate files in Temp

		bool RefreshDirectory(const tchar* dirPath, bool forceRegister = false); // Tell uba a directory on disk has been changed by some other system while session is running
		bool RegisterNewFile(const tchar* filePath); // Tell uba a new file on disk has been added by some other system while session is running
		void RegisterDeleteFile(const tchar* filePath); // Tell uba a file on disk has been deleted by some other system while session is running
		bool RegisterNewDirectory(const tchar* directoryPath); // Tell uba a directory on disk has been added by some other system while session is running

		const u8* GetDirectoryTableMemory();
		DirTableSize GetDirectoryTableSize(DirectoryTableOverlay* overlay);

		void CreateOverlay(DirectoryTableOverlay& overlay);
		void ResetOverlay(DirectoryTableOverlay& overlay, void* data, u64 dataSize);
		bool EnsureOverlayMemory(DirectoryTableOverlay& overlay, u64 neededSize);
		void DestroyOverlay(DirectoryTableOverlay& overlay);

		void PrintOverlay(DirectoryTableOverlay& overlay);
		void PrintOverlay(DirectoryTableOverlay& overlay, DirectoryTableOverlay::Dir& dir, StringBufferBase& path);

		MutableLogger m_logger;

	//protected:

		bool RegisterOverlayCreateOrWrite(DirectoryTableOverlay& overlay, StringKey filePathKey, StringView filePath, u64 fileSize, u64 lastWriteTime, u32 attributes);
		bool RegisterOverlayDelete(DirectoryTableOverlay& overlay, StringKey filePathKey, StringView filePath);
		bool RegisterNonRootedDir(DirectoryTableOverlay& overlay, StringView dirPath); // Will not register parent dirs recursively

		bool PopulateOverlayFile(DirectoryTableOverlay& overlay, StringView filePath, const Function<bool(FileInformation& out, StringKey)>& getFileInformation);
		bool AddOverlayFile(DirectoryTableOverlay& overlay, StringView filePath);
		bool WriteOverlayEntry(DirectoryTableOverlay& overlay, StringView filePath, const FileInformation& fileInfo);
		bool WriteOverlayEntry(DirectoryTableOverlay& overlay, StringKey dirKey, StringView filePath, StringView fileName, const FileInformation& fileInfo, bool populateMainTableParentDir);
		bool WriteOverlayEntryNoLock(DirectoryTableOverlay& overlay, StringKey dirKey, DirectoryTableOverlay::Dir& dir, StringView fileName, const FileInformation& fileInfo, u32& outFileOffset);
		bool WriteOverlayDirInLookupNoLock(DirectoryTableOverlay& overlay, StringView tempDir);
		bool WriteOverlayDirectoryEntriesRecursiveNoLock(DirectoryTableOverlay& overlay, StringKey filePathKey, StringView filePath);

		bool RegisterCreateFileForWrite(StringKey filePathKey, StringView filePath, u64 fileSize, u64 lastWriteTime, u32 attributes, bool registerRealFile, bool invalidateStorage);
		bool RegisterDeleteFile(StringKey filePathKey, StringView filePath);

		u32 WriteDirectoryEntries(const StringKey& dirKey, StringView dirPath, u32* outTableOffset = nullptr);
		void WriteDirectoryEntriesRecursive(const StringKey& dirKey, StringView dirPath, u32& outTableOffset);
		bool WriteDirectoryEntriesInternal(DirectoryTable::Directory& dir, const StringKey& dirKey, StringView dirPath, bool isRefresh, u32& outTableOffset);
		void EnsureDirectoryTableMemory(u64 neededSize);

		virtual bool IsInTemp(StringView path);
		virtual void FileEntryAdded(StringKey filePathKey, u64 lastWritten, u64 size);
		virtual bool ShouldWriteToDisk(StringView filePath);
		virtual void InvalidateCachedFileInfo(const StringKey& filePathKey);

		bool m_runningRemote;

		Atomic<u64> m_fileIndexCounter = 8000000000; // Try not to collide with ntfs
		MemoryBlock m_directoryTableMemory;
		FileMappingHandle m_directoryTableHandle;
		u8* m_directoryTableMem;
		u64 m_directoryTableMemCommitted = 0;
		DirectoryTable m_directoryTable;
		StringKey m_directoryForcedEmpty;
		VolumeCache m_volumeCache;
	};

	////////////////////////////////////////////////////////////////////////////////////////////////////

	bool GetDirKey(StringKey& outDirKey, StringBufferBase& outDirPath, const tchar*& outLastSlash, StringView filePath);
	StringKey GetKeyAndFixedName(StringBuffer<>& outFixedFilePath, const tchar* filePath);

	////////////////////////////////////////////////////////////////////////////////////////////////////
}
