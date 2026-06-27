// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaDirectoryTableHolder.h"
#include "UbaDirectoryIterator.h"
#include "UbaEnvironment.h"
#include "UbaFileMapping.h"
#include "UbaProtocol.h"

#define UBA_DEBUG_TRACK_DIR 0 // UBA_DEBUG_LOGGER

namespace uba
{
	////////////////////////////////////////////////////////////////////////////////////////////////////

	bool GetDirKey(StringKey& outDirKey, StringBufferBase& outDirPath, const tchar*& outLastSlash, StringView filePath)
	{
		StringView dirPath = filePath.GetPath();
		outLastSlash = dirPath.data + dirPath.count;
		UBA_ASSERTF(dirPath.count, TC("Can't get dir key for path %s"), filePath.data);
		if (!dirPath.count)
			return false;
		outDirPath.Append(dirPath);
		outDirKey = CaseInsensitiveFs ? ToStringKeyLower(outDirPath) : ToStringKey(outDirPath);
		return true;
	}

	StringKey GetKeyAndFixedName2(StringBuffer<>& outFixedFilePath, StringKeyHasher& outHasher, StringView filePath, StringKey* outDirKey, StringView* outFileName)
	{
		StringBuffer<> workingDir;
		if (!IsAbsolutePath(filePath))
		{
			GetCurrentDirectoryW(workingDir);
			workingDir.EnsureEndsWithSlash();
		}
		FixPath(outFixedFilePath, filePath, workingDir);

		StringKey dirKey;
		StringBuffer<> dirNameForHash;
		const tchar* lastSlash = nullptr;
		GetDirKey(dirKey, dirNameForHash, lastSlash, outFixedFilePath);
		if (outDirKey)
			*outDirKey = dirKey;
		if (outFileName)
			*outFileName = StringView(lastSlash + 1, outFixedFilePath.count - u32(lastSlash - outFixedFilePath.data) - 1);

		if (CaseInsensitiveFs)
			dirNameForHash.MakeLower();

		outHasher.Update(dirNameForHash);

		if (lastSlash)
		{
			StringBuffer<256> baseFileNameForHash;
			baseFileNameForHash.Append(lastSlash);
			if (CaseInsensitiveFs)
				baseFileNameForHash.MakeLower();

			outHasher.Update(baseFileNameForHash);
		}

		StringKey result = ToStringKey(outHasher);

#if UBA_DEBUG
		StringBuffer<> testPath(outFixedFilePath);
		if (CaseInsensitiveFs)
			testPath.MakeLower();
		StringKey testKey = ToStringKey(testPath);
		UBA_ASSERTF(testKey == result, TC("Key mismatch for %s"), outFixedFilePath.data);
#endif

		return result;
	}

	StringKey GetKeyAndFixedName(StringBuffer<>& outFixedFilePath, const tchar* filePath)
	{
		StringKeyHasher hasher;
		return GetKeyAndFixedName2(outFixedFilePath, hasher, ToView(filePath), nullptr, nullptr);
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////

	DirectoryTableHolder::DirectoryTableHolder(LogWriter& logWriter, const tchar* logPrefix, bool runningRemote)
	:	m_logger(logWriter, logPrefix)
	,	m_runningRemote(runningRemote)
	,	m_directoryTableMemory(TC("SessionDirectoryTable"))
	,	m_directoryTable(m_directoryTableMemory)
	{
		m_directoryTableHandle = FileMapping_Create(m_logger, PAGE_READWRITE|SEC_RESERVE, DirTableMaxSize, nullptr, TC("DirMappings"));
		UBA_ASSERT(m_directoryTableHandle.IsValid());
		m_directoryTableMem = FileMapping_Map(m_logger, m_directoryTableHandle, FILE_MAP_WRITE, 0, DirTableMaxSize);
		UBA_ASSERT(m_directoryTableMem);

		m_directoryTable.m_memory = m_directoryTableMem;
		m_directoryTable.m_lookup.reserve(30000);
	}

	DirectoryTableHolder::~DirectoryTableHolder()
	{
		FileMapping_Unmap(m_logger, m_directoryTableMem, DirTableMaxSize, TC("DirectoryTable"), false); // Can't discard since only parts might be mapped
		FileMapping_Close(m_logger, m_directoryTableHandle, TC("DirectoryTable"));
	}

	void DirectoryTableHolder::SetTreatTempDirAsEmpty()
	{
#if PLATFORM_WINDOWS
		wchar_t systemTemp[256];
		GetEnvironmentVariableW(L"TEMP", systemTemp, 256);
		StringBuffer<> temp;
		FixPath(systemTemp, nullptr, 0, temp);
		temp.MakeLower();
		m_directoryForcedEmpty = ToStringKey(temp);
#endif
	}

	bool DirectoryTableHolder::RefreshDirectory(const tchar* dirPath, bool forceRegister)
	{
		UBA_ASSERT(!m_runningRemote);

		StringBuffer<> fixedDirPath;
		StringKeyHasher hasher;
		GetKeyAndFixedName2(fixedDirPath, hasher, ToView(dirPath), nullptr, nullptr);

		StringKey dirKey = ToStringKey(hasher);

		auto& dirTable = m_directoryTable;
		SCOPED_READ_LOCK(dirTable.m_lookupLock, lookupLock);
		auto res = dirTable.m_lookup.find(dirKey);
		if (res == dirTable.m_lookup.end())
		{
			lookupLock.Leave();
			if (forceRegister)
				WriteDirectoryEntries(dirKey, fixedDirPath);
			return true;
		}
		DirectoryTable::Directory& dir = res->second;
		lookupLock.Leave();
		SCOPED_WRITE_LOCK(dir.lock, dirLock);

		while (dir.populatedOffset == 0)
		{
			dirLock.Leave();
			Sleep(1);
			dirLock.Enter();
		}
		UBA_ASSERT(dir.populatedOffset == 1);

		u32 tableOffset;
		return WriteDirectoryEntriesInternal(dir, dirKey, fixedDirPath, true, tableOffset);
	}

	bool DirectoryTableHolder::RegisterNewFile(const tchar* filePath)
	{
		UBA_ASSERT(!m_runningRemote);
		StringBuffer<> fixedFilePath;
		auto key = GetKeyAndFixedName(fixedFilePath, filePath);
		return RegisterCreateFileForWrite(key, fixedFilePath, 0, 0, DefaultAttributes(), true, true);
	}

	void DirectoryTableHolder::RegisterDeleteFile(const tchar* filePath)
	{
		UBA_ASSERT(!m_runningRemote);
		StringBuffer<> fixedFilePath;
		auto key = GetKeyAndFixedName(fixedFilePath, filePath);
		RegisterDeleteFile(key, fixedFilePath);
	}

	bool DirectoryTableHolder::RegisterNewDirectory(const tchar* directoryPath)
	{
		UBA_ASSERT(!m_runningRemote);
		StringBuffer<> fixedDirPath;
		auto dirKey = GetKeyAndFixedName(fixedDirPath, directoryPath);
		if (!RegisterCreateFileForWrite(dirKey, fixedDirPath, 0, 0, DefaultDirAttributes(), true, true))
			return false;
		WriteDirectoryEntries(dirKey, fixedDirPath);
		return true;
	}

	const u8* DirectoryTableHolder::GetDirectoryTableMemory()
	{
		return m_directoryTableMem;
	}

	DirTableSize DirectoryTableHolder::GetDirectoryTableSize(DirectoryTableOverlay* overlay)
	{
		u32 overlaySize = 0u;
		if (overlay)
		{
			SCOPED_READ_LOCK(overlay->lock, lock);
			overlaySize = overlay->size;
		}
		SCOPED_READ_LOCK(m_directoryTable.m_memoryLock, lock);
		return {m_directoryTable.m_memorySize, overlaySize};
	}

	void DirectoryTableHolder::CreateOverlay(DirectoryTableOverlay& overlay)
	{
		UBA_ASSERT(!overlay.handle.IsValid());
		SCOPED_WRITE_LOCK(overlay.lock, lock);
		overlay.handle = FileMapping_Create(m_logger, PAGE_READWRITE|SEC_RESERVE, OverlayTableMaxSize, nullptr, TC("Overlay"));
		UBA_ASSERTF(overlay.handle.IsValid(), TC("Failed to create overlay handle (size %u)"), OverlayTableMaxSize);
		overlay.memory = FileMapping_Map(m_logger, overlay.handle, FILE_MAP_WRITE, 0, OverlayTableMaxSize);
		UBA_ASSERT(overlay.memory);
	}

	void DirectoryTableHolder::ResetOverlay(DirectoryTableOverlay& overlay, void* data, u64 dataSize)
	{
		overlay.size = 0;
		overlay.lookup.clear();
		overlay.roots.clear();
		overlay.createdDirectories.clear();
		overlay.removedDirectories.clear();
		overlay.deletedFiles.clear();
		overlay.symlinks.clear();
		if (!dataSize)
			return;
		EnsureOverlayMemory(overlay, dataSize);
		memcpy(overlay.memory, data, dataSize);
		overlay.size = u32(dataSize);
	}

	bool DirectoryTableHolder::EnsureOverlayMemory(DirectoryTableOverlay& overlay, u64 neededSize)
	{
		u64 totalSize = overlay.size + neededSize;
		if (totalSize <= overlay.committed)
			return true;
		u64 newSize = AlignUp(totalSize, 32ull*1024);
		if (newSize > OverlayTableMaxSize)
		{
			static bool called = m_logger.Error(TC("Overlay table overflow. OverlayTableMaxSize need to be increased (Size: %llu)"), overlay.size);
			return false;
		}

		u64 toCommit = newSize - overlay.committed;
		u8* address = overlay.memory + overlay.committed;
		if (!FileMapping_Commit(m_logger, address, toCommit, false))
			m_logger.Error(TC("Failed to commit memory for directory table (Committed: %llu, ToCommit: %llu) (%s)"), m_directoryTableMemCommitted, toCommit, LastErrorToText().data);
		overlay.committed += u32(toCommit);
		return true;
	}

	void DirectoryTableHolder::DestroyOverlay(DirectoryTableOverlay& overlay)
	{
		if (!overlay.handle.IsValid())
			return;
		SCOPED_WRITE_LOCK(overlay.lock, lock);
		FileMapping_Unmap(m_logger, overlay.memory, OverlayTableMaxSize, TC("Overlay"), false); // Can't discard since only parts might be mapped
		overlay.memory = nullptr;
		FileMapping_Close(m_logger, overlay.handle, TC("Overlay"));
		overlay.handle = {};
		overlay.committed = 0;
		overlay.size = 0;
		overlay.lookup.clear();
	}

	void DirectoryTableHolder::PrintOverlay(DirectoryTableOverlay& overlay)
	{
		StringBuffer<> path;
		for (auto& root : overlay.roots)
		{
			auto findIt = overlay.lookup.find(root);
			UBA_ASSERT(findIt != overlay.lookup.end());
			PrintOverlay(overlay, findIt->second, path);
		}
		m_logger.Info(TC("OVERLAY MEMORY SIZE: %llu"), overlay.size);
	}

	void DirectoryTableHolder::PrintOverlay(DirectoryTableOverlay& overlay, DirectoryTableOverlay::Dir& dir, StringBufferBase& path)
	{
		for (auto& file : dir.files)
		{
			u32 pathLen = path.count;
			BinaryReader reader(overlay.memory, file.second);
			reader.ReadString(path);

			auto findIt = overlay.lookup.find(file.first);
			if (findIt != overlay.lookup.end())
			{
				path.Append(PathSeparator);
				PrintOverlay(overlay, findIt->second, path);
			}
			else
			{
				//if (reader.ReadFileAttributes())
					m_logger.Info(TC("OVERLAY FILE: %s"), path.data);
			}
			path.Resize(pathLen);
		}
	}

	bool DirectoryTableHolder::RegisterOverlayCreateOrWrite(DirectoryTableOverlay& overlay, StringKey filePathKey, StringView filePath, u64 fileSize, u64 lastWriteTime, u32 attributes)
	{
		UBA_ASSERTF(attributes != 0, TC("Registering file %.*s with 0 attributes"), filePath.count, filePath.data);
		FileInformation info;
		info.attributes = attributes;
		info.index = ++m_fileIndexCounter;
		info.volumeSerialNumber = 1;
		info.size = fileSize;
		info.lastWriteTime = lastWriteTime;
		return WriteOverlayEntry(overlay, filePath, info);
	}

	bool DirectoryTableHolder::RegisterOverlayDelete(DirectoryTableOverlay& overlay, StringKey filePathKey, StringView filePath)
	{
		StringKey dirKey;
		const tchar* lastSlash;
		StringBuffer<> dirName;
		if (!GetDirKey(dirKey, dirName, lastSlash, filePath))
			return false;
		FileInformation fileInfo;
		return WriteOverlayEntry(overlay, dirKey, filePath, ToView(lastSlash+1), fileInfo, !m_runningRemote);
	}

	bool DirectoryTableHolder::RegisterNonRootedDir(DirectoryTableOverlay& overlay, StringView dirPath)
	{
		UBA_ASSERT(dirPath.Last() != PathSeparator);
		if (!WriteOverlayDirInLookupNoLock(overlay, dirPath.GetPath())) // Create parent dir without any roots
			return false;
		FileInformation fileInfo;
		fileInfo.attributes = DefaultDirAttributes();
		fileInfo.index = ++m_fileIndexCounter;
		fileInfo.volumeSerialNumber = 1;
		return WriteOverlayEntry(overlay, dirPath, fileInfo); // Add non rooted dir
	}

	bool DirectoryTableHolder::PopulateOverlayFile(DirectoryTableOverlay& overlay, StringView filePath, const Function<bool(FileInformation&, StringKey)>& getFileInformation)
	{
		StringKey dirKey;
		StringView fileName;
		StringBuffer<> fixedFilePath;
		StringKeyHasher hasher;
		StringKey filePathKey = GetKeyAndFixedName2(fixedFilePath, hasher, filePath, &dirKey, &fileName);
		FileInformation fileInfo;
		if (!getFileInformation(fileInfo, filePathKey))
			return false;
		return WriteOverlayEntry(overlay, dirKey, fixedFilePath, fileName, fileInfo, !m_runningRemote);
	}

	bool DirectoryTableHolder::AddOverlayFile(DirectoryTableOverlay& overlay, StringView filePath)
	{
		FileInformation fi;
		fi.size = InvalidValue; // TODO: This is very wrong.. but can be used to know if file is actually in open state
		fi.attributes = DefaultAttributes();
		fi.index = ++m_fileIndexCounter;
		fi.volumeSerialNumber = 1;
		return WriteOverlayEntry(overlay, filePath, fi);
	}

	bool DirectoryTableHolder::WriteOverlayEntry(DirectoryTableOverlay& overlay, StringView filePath, const FileInformation& fileInfo)
	{
		StringKey dirKey;
		StringView fileName;
		StringBuffer<> fixedFilePath;
		StringKeyHasher hasher;
		StringKey filePathKey = GetKeyAndFixedName2(fixedFilePath, hasher, filePath, &dirKey, &fileName);
		if (!WriteOverlayEntry(overlay, dirKey, fixedFilePath, fileName, fileInfo, !m_runningRemote))
			return false;
		if (IsDirectory(fileInfo.attributes))
		{
			SCOPED_WRITE_LOCK(overlay.lock, lock);
			return WriteOverlayDirInLookupNoLock(overlay, filePath);
		}
		return true;
	}

	bool DirectoryTableHolder::WriteOverlayEntry(DirectoryTableOverlay& overlay, StringKey dirKey, StringView filePath, StringView fileName, const FileInformation& fileInfo, bool populateMainTableParentDir)
	{
		SCOPED_WRITE_LOCK(overlay.lock, lock);

		auto dirOffsetIns = overlay.lookup.try_emplace(dirKey);
		auto& dir = dirOffsetIns.first->second;

		if (!dir.offset)
		{
			StringView dirPath(filePath.data, u32(fileName.data - filePath.data) - 1);

			// I have a feeling there is a bug here somehow but can't create it in unit tests.
			// Essentially the theory is that if a remote helper runs in a directory tree and gets the attribute of a overlay file (prepopulated)
			// That will create the entire hierarchy to the overlay file.
			// It then queries a sibling directory for a file. Now the dir hierarchy exists but sibling directory will not exist since the directory table
			// does not know that it has not queried the directory for the main table.
			if (populateMainTableParentDir)
			{
				UBA_ASSERT(!m_runningRemote);
				WriteDirectoryEntries(dirKey, dirPath);
			}

			WriteOverlayDirectoryEntriesRecursiveNoLock(overlay, dirKey, dirPath);
		}

		auto& fileOffset = dir.files[CaseInsensitiveFs ? ToStringKeyLower(fileName) : ToStringKey(fileName)];
		return WriteOverlayEntryNoLock(overlay, dirKey, dir, fileName, fileInfo, fileOffset);
	}

	bool DirectoryTableHolder::WriteOverlayEntryNoLock(DirectoryTableOverlay& overlay, StringKey dirKey, DirectoryTableOverlay::Dir& dir, StringView fileName, const FileInformation& fileInfo, u32& outFileOffset)
	{
		u8 temp[256];
		BinaryWriter writer(temp, 0, sizeof(temp));
		writer.WriteStringKey(dirKey);
		writer.Write7BitEncoded(dir.offset); // Previous entry for same directory
		if (!dir.offset)
		{
			writer.WriteFileAttributes(DefaultDirAttributes());
			writer.WriteVolumeSerial(0);
			writer.WriteFileIndex(++m_fileIndexCounter);
		}
		writer.Write7BitEncoded(1); // Count
		u32 relativePosition = u32(writer.GetPosition());
		writer.WriteString(fileName);
		writer.WriteFileAttributes(fileInfo.attributes);
		writer.WriteVolumeSerial(fileInfo.volumeSerialNumber);
		writer.WriteFileIndex(fileInfo.index);
		writer.WriteFileTime(fileInfo.lastWriteTime);
		writer.WriteFileSize(fileInfo.size);
		u64 written = writer.GetPosition();
		if (!EnsureOverlayMemory(overlay, written + 8))
			return false;
		BinaryWriter memWriter(overlay.memory, overlay.size, overlay.committed);
		memWriter.Write7BitEncoded(written); // Storage size
		dir.offset = u32(memWriter.GetPosition()) + sizeof(StringKey);
		outFileOffset = u32(memWriter.GetPosition()) + relativePosition;

		memWriter.WriteBytes(writer.GetData(), written);
		overlay.size = u32(memWriter.GetPosition());
		return true;
	}

	bool DirectoryTableHolder::WriteOverlayDirInLookupNoLock(DirectoryTableOverlay& overlay, StringView dirPath)
	{
		UBA_ASSERT(dirPath.Last() != PathSeparator);
		StringKey dirKey = CaseInsensitiveFs ? ToStringKeyLower(dirPath) : ToStringKey(dirPath);
		auto insres = overlay.lookup.try_emplace(dirKey);
		if (!insres.second)
			return true;
		auto& dir = insres.first->second;
		u8 temp[256];
		BinaryWriter writer(temp, 0, sizeof(temp));
		writer.WriteStringKey(dirKey);
		writer.Write7BitEncoded(0); // Previous entry for same directory
		writer.WriteFileAttributes(DefaultDirAttributes());
		writer.WriteVolumeSerial(0);
		writer.WriteFileIndex(++m_fileIndexCounter);
		writer.Write7BitEncoded(0); // File count
		u64 written = writer.GetPosition();
		if (!EnsureOverlayMemory(overlay, written + 8))
			return false;
		BinaryWriter memWriter(overlay.memory, overlay.size, overlay.committed);
		memWriter.Write7BitEncoded(writer.GetPosition()); // Storage size
		dir.offset = u32(memWriter.GetPosition()) + sizeof(StringKey);
		memWriter.WriteBytes(writer.GetData(), written);
		overlay.size = u32(memWriter.GetPosition());
		return true;
	}

	bool DirectoryTableHolder::WriteOverlayDirectoryEntriesRecursiveNoLock(DirectoryTableOverlay& overlay, StringKey filePathKey, StringView filePath)
	{
		StringView dirPath = filePath.GetPath();
		if (!dirPath.count)
		{
			overlay.roots.push_back(filePathKey);
			return true;
		}
		StringKey dirKey = CaseInsensitiveFs ? ToStringKeyLower(dirPath) : ToStringKey(dirPath);
		auto& dir = overlay.lookup.try_emplace(dirKey).first->second;

		u8 temp[256];
		BinaryWriter writer(temp, 0, sizeof(temp));

		if (!dir.offset)
			if (!WriteOverlayDirectoryEntriesRecursiveNoLock(overlay, dirKey, dirPath))
				return false;

		auto res = dir.files.try_emplace(filePathKey);
		if (!res.second)
			return true;

		FileInformation fi;
		fi.attributes = DefaultDirAttributes();
		fi.index = ++m_fileIndexCounter;
		fi.volumeSerialNumber = 1;
		return WriteOverlayEntryNoLock(overlay, dirKey, dir, filePath.GetFileName(), fi, res.first->second);
	}

	bool DirectoryTableHolder::RegisterCreateFileForWrite(StringKey filePathKey, StringView filePath, u64 fileSize, u64 lastWriteTime, u32 attributes, bool registerRealFile, bool invalidateStorage)
	{
		auto& dirTable = m_directoryTable;

		StringKey dirKey;
		const tchar* lastSlash;
		StringBuffer<> dirName;
		if (!GetDirKey(dirKey, dirName, lastSlash, filePath))
			return true;

		// Remote is not updating its own directory table
		UBA_ASSERT(!m_runningRemote);
		UBA_ASSERTF(!IsInTemp(filePath), TC("Registering temp files to main table not allowed (%.*s)"), filePath.count, filePath.data);

		#if 0//_DEBUG  // Bring this back, turned off right now because a few lines above the call to this method we add a mapping
		{
			SCOPED_FUTEX(m_fileMappingTableLookupLock, lookupLock);
			auto findIt = m_fileMappingTableLookup.find(filePathKey);
			if (findIt != m_fileMappingTableLookup.end())
			{
				FileMappingEntry& entry = findIt->second;
				lookupLock.Leave();
				SCOPED_FUTEX(entry.lock, entryCs);
				UBA_ASSERT(!entry.mapping);
			}
		}
		#endif

		bool shouldWriteToDisk = registerRealFile && ShouldWriteToDisk(filePath);

		// When not writing to disk we need to populate lookup before adding non-written files.. otherwise they will be lost once lookup is actually populated
		if (!shouldWriteToDisk)
		{
			UBA_FOR_ASSERT(u32 res =) WriteDirectoryEntries(dirKey, dirName);
			UBA_ASSERTF(res, TC("Failed to write directory entries for %.*s"), dirName.count, dirName.data);
		}

		SCOPED_READ_LOCK(dirTable.m_lookupLock, lookupCs);
		auto findIt = dirTable.m_lookup.find(dirKey);
		if (findIt == dirTable.m_lookup.end())
			return true;

		DirectoryTable::Directory& dir = findIt->second;
		lookupCs.Leave();

		SCOPED_WRITE_LOCK(dir.lock, dirLock);

		// To prevent race where code creating dir manage to add to lookup but then got here later than this thread.
		while (dir.populatedOffset == 0)
		{
			dirLock.Leave();
			Sleep(1);
			dirLock.Enter();
		}

		// Directory was attempted to be added when it didn't exist. It is still added to dirtable lookup but we set populatedOffset to 2.
		// If adding a file, clearly it does exist.. so let's reparse it.
		if (dir.populatedOffset == 2)
		{
			dirLock.Leave();
			UBA_FOR_ASSERT(u32 res =) WriteDirectoryEntries(dirKey, dirName);
			UBA_ASSERT(res);
			dirLock.Enter();
		}
		UBA_ASSERTF(dir.populatedOffset == 1 || (!ShouldWriteToDisk(filePath) && dir.populatedOffset != 3), TC("Registering create file for write %s with unexpect dir.populatedOffset %u "), filePath.data, dir.populatedOffset);

		if (filePathKey == StringKeyZero)
		{
			StringBuffer<> forKey;
			forKey.Append(filePath);
			if (CaseInsensitiveFs)
				forKey.MakeLower();
			filePathKey = ToStringKey(forKey);
		}
		auto insres = dir.files.try_emplace(filePathKey, ~u32(0));

		u64 fileIndex = InvalidValue;
		u32 volumeSerial = 0;
		bool isDirectory = false;

		if (shouldWriteToDisk)
		{
			FileInformation info;
			if (!GetFileInformation(info, m_logger, filePath))
				return m_logger.Error(TC("Failed to get file information for %.*s while checking file added for write. This should not happen! (%s)"), filePath.count, filePath.data, LastErrorToText().data);

			attributes = info.attributes;
			volumeSerial = info.volumeSerialNumber;
			lastWriteTime = info.lastWriteTime;
			isDirectory = IsDirectory(attributes);
			if (isDirectory)
				fileSize = 0;
			else
				fileSize = info.size;
			fileIndex = info.index;
		}
		else
		{
			// TODO: Do we need more code here?
			UBA_ASSERTF(attributes != 0, TC("Registering file %.*s with no attributes set (Needs to be set from the outside when not writing to disk)"), filePath.count, filePath.data);
			volumeSerial = 1;
			fileIndex = ++m_fileIndexCounter;
		}

		// Check if new write is actually a write. The file might just have been open with write permissions and then actually never written to.
		// We check this by using lastWriteTime. If it hasn't change, directory table is already up-to-date
		if (!insres.second && IsValidEntry(insres.first->second))
		{
			BinaryReader reader(m_directoryTableMem + insres.first->second.internal);
			reader.SkipString();

			u32 oldAttr = reader.ReadFileAttributes();

			if (IsDirectory(oldAttr))  // Ignore updating directories.. they should always be the same regardless
			{
				UBA_ASSERT(isDirectory || !shouldWriteToDisk);
				return true;
			}
			reader.ReadVolumeSerial();
			u64 oldFileIndex = reader.ReadFileIndex();(void)oldFileIndex;

			u64 oldLastWriteTime = reader.ReadFileTime();
			if (lastWriteTime == oldLastWriteTime)
			{
#if !PLATFORM_WINDOWS
				if (oldFileIndex == fileIndex)
				//UBA_ASSERTF(oldFileIndex == fileIndex, TC("Same write time but Not same file? %llu vs %llu (%.*s)"), oldFileIndex, fileIndex, filePath.count, filePath.data); // Checking so it is really the same file
#endif
				{
					u64 oldSize = reader.ReadFileSize();
					if (oldSize == fileSize && oldAttr == attributes) // Only attributes could change from a chmod
						return true;
					// TODO: Somehow this can happen and I have no idea how. last written time should be set on close file so it shouldnt be possible.
					//else
					//	m_logger.Warning(TC("Somehow file %s has same last written time at two points in time but different size (old %llu new %llu)"), filePath.data, oldSize, fileSize);
				}
			}
		}

		// There are directory crawlers happening in parallel so we need to really make sure to invalidate this one since a crawler can actually
		// hit this file with information from a query before it was written.. and then it will turn it back to "verified" using old info
		if (registerRealFile && invalidateStorage)
			InvalidateCachedFileInfo(filePathKey);

		FileEntryAdded(filePathKey, lastWriteTime, fileSize);

		u32 volumeIndex = m_volumeCache.GetSerialIndex(volumeSerial);
		u8 temp[1024];
		u64 written;
		u64 entryPos;
		{
			BinaryWriter writer(temp, 0, sizeof(temp));
			writer.WriteStringKey(dirKey);
			writer.Write7BitEncoded(dir.latestOffset); // Previous entry for same directory
			writer.Write7BitEncoded(1); // Count
			entryPos = writer.GetPosition();
			writer.WriteString(lastSlash + 1);
			writer.WriteFileAttributes(attributes);
			writer.WriteVolumeSerial(volumeIndex);
			writer.WriteFileIndex(fileIndex);
			if (!isDirectory)
			{
				writer.WriteFileTime(lastWriteTime);
				writer.WriteFileSize(fileSize);
			}
			written = writer.GetPosition();
		}

#if UBA_DEBUG_TRACK_DIR
		m_debugLogger->Info(TC("TRACKADD    %s (Size: %llu, Attr: %u, Key: %s, Id: %llu)"), filePath.data, fileSize, attributes, KeyToString(filePathKey).data, fileIndex);
#endif

		SCOPED_WRITE_LOCK(dirTable.m_memoryLock, memoryLock);
		u32 writePos = dirTable.m_memorySize;
		EnsureDirectoryTableMemory(writePos + 8 + written);
		BinaryWriter writer(m_directoryTableMem + writePos);
		writer.Write7BitEncoded(written); // Storage size
		insres.first->second = {dirTable.m_memorySize + u32(writer.GetPosition() + entryPos)}; // Storing position to last write time
		u32 tableOffset = u32(writer.GetPosition()) + sizeof(StringKey);
		writer.WriteBytes(temp, written);
		dir.latestOffset = dirTable.m_memorySize + tableOffset;
		dirTable.m_memorySize += u32(writer.GetPosition());
		return true;
	}

	bool DirectoryTableHolder::RegisterDeleteFile(StringKey filePathKey, StringView filePath)
	{
		StringKey dirKey;
		const tchar* lastSlash;
		StringBuffer<> dirName;
		if (!GetDirKey(dirKey, dirName, lastSlash, filePath))
			return false;

		UBA_ASSERT(!m_runningRemote);
		UBA_ASSERT(!IsInTemp(filePath));

		auto& dirTable = m_directoryTable;
		SCOPED_READ_LOCK(dirTable.m_lookupLock, lookupCs);
		auto res = dirTable.m_lookup.find(dirKey);
		if (res == dirTable.m_lookup.end())
			return true;
		DirectoryTable::Directory& dir = res->second;
		lookupCs.Leave();
		SCOPED_WRITE_LOCK(dir.lock, dirLock);

		while (dir.populatedOffset == 0)
		{
			dirLock.Leave();
			Sleep(1);
			dirLock.Enter();
		}

		if (dir.populatedOffset == 2) // Invalid directory, don't need to register this
			return true;

		UBA_ASSERTF(dir.populatedOffset == 1, TC("Registering deleted file %s with unexpect dir.populatedOffset %u "), filePath.data, dir.populatedOffset);

		if (filePathKey == StringKeyZero)
		{
			StringBuffer<> forKey;
			forKey.Append(filePath);
			if (CaseInsensitiveFs)
				forKey.MakeLower();
			filePathKey = ToStringKey(forKey);
		}

		// Does not exist, no need adding to file table
		if (dir.files.erase(filePathKey) == 0)
			return true;

		u8 temp[1024];
		u64 written;
		{
			BinaryWriter writer(temp, 0, sizeof(temp));
			writer.WriteStringKey(dirKey);
			writer.Write7BitEncoded(dir.latestOffset); // Previous entry for same directory
			writer.Write7BitEncoded(1); // Count
			writer.WriteString(lastSlash + 1);
			writer.WriteFileAttributes(0);
			writer.WriteVolumeSerial(0);
			writer.WriteFileIndex(0);
			if (true) // !IsDirectory()
			{
				writer.WriteFileTime(0);
				writer.WriteFileSize(0);
			}
			written = writer.GetPosition();
		}

#if UBA_DEBUG_TRACK_DIR
		m_debugLogger->Info(TC("TRACKDEL    %s (Key: %s)"), filePath.data, KeyToString(filePathKey).data);
#endif

		{
			SCOPED_WRITE_LOCK(dirTable.m_memoryLock, memoryLock);
			u32 writePos = dirTable.m_memorySize;
			EnsureDirectoryTableMemory(writePos + 8 + written);
			BinaryWriter writer(m_directoryTableMem + writePos);
			writer.Write7BitEncoded(written); // Storage size
			u32 tableOffset = u32(writer.GetPosition()) + sizeof(StringKey);
			writer.WriteBytes(temp, written);
			dir.latestOffset = dirTable.m_memorySize + tableOffset;
			dirTable.m_memorySize += u32(writer.GetPosition());
		}
		return true;
	}

	u32 DirectoryTableHolder::WriteDirectoryEntries(const StringKey& dirKey, StringView dirPath, u32* outTableOffset)
	{
		UBA_ASSERT(!m_runningRemote);
		auto& dirTable = m_directoryTable;
		u32 temp;
		if (!outTableOffset)
			outTableOffset = &temp;
		WriteDirectoryEntriesRecursive(dirKey, dirPath, *outTableOffset);
		SCOPED_READ_LOCK(dirTable.m_memoryLock, memoryLock);
		return dirTable.m_memorySize;
	}

	void DirectoryTableHolder::WriteDirectoryEntriesRecursive(const StringKey& dirKey, StringView dirPath, u32& outTableOffset)
	{
		auto& dirTable = m_directoryTable;
		SCOPED_WRITE_LOCK(dirTable.m_lookupLock, lookupLock);
		auto res = dirTable.m_lookup.try_emplace(dirKey, dirTable.m_memoryBlock);
		DirectoryTable::Directory& dir = res.first->second;
		lookupLock.Leave();

		SCOPED_WRITE_LOCK(dir.lock, dirLock);

		if (dir.populatedOffset == 1)
		{
			outTableOffset = dir.latestOffset;
			return;
		}

		if (!WriteDirectoryEntriesInternal(dir, dirKey, dirPath, false, outTableOffset))
		{
			outTableOffset = InvalidTableOffset;
			dir.populatedOffset = 2;
		}
		else
		{
			dir.populatedOffset = 1;
		}

		u64 dirlen = dirPath.count;

		if (!dirlen) // This is for non-windows.. '/' is actually empty to get hashes correct
			return;

		// scan backwards first
		const tchar* rit = (tchar*)dirPath.data + dirlen - 2;
		while (rit > dirPath.data)
		{
			if (*rit != PathSeparator)
			{
				--rit;
				continue;
			}
			break;
		}

		if (IsWindows && rit <= dirPath.data) // There are no path separators left, this is the drive
		{
			dirPath.count = 0;
			return;
		}

		StringView parentDirPath(dirPath.data, u32(rit - dirPath.data));

		StringBuffer<> parentDirForHash;
		parentDirForHash.Append(parentDirPath);
		if (CaseInsensitiveFs)
			parentDirForHash.MakeLower();
		StringKey parentKey = ToStringKey(parentDirForHash);

		// Traverse through ancestors and populate them, this is an optimization
		u32 parentOffset;
		WriteDirectoryEntriesRecursive(parentKey, parentDirPath, parentOffset);

		dirLock.Leave();

		if (parentKey == m_directoryForcedEmpty) // We need to manually add this dir to the forced empty dir
			RegisterCreateFileForWrite(parentKey, dirPath, 0, 0, DefaultDirAttributes(), true, false);
	}

	bool DirectoryTableHolder::WriteDirectoryEntriesInternal(DirectoryTable::Directory& dir, const StringKey& dirKey, StringView dirPath, bool isRefresh, u32& outTableOffset)
	{
		UBA_ASSERT(dir.populatedOffset != 3);

		if (dir.latestOffset != InvalidTableOffset && !isRefresh)
		{
			isRefresh = true;
		}

		auto& dirTable = m_directoryTable;

		u32 volumeSerial = 0;
		u32 volumeSerialIndex = 0;
		u32 dirAttributes = 0;
		u64 fileIndex = 0;

		u64 written = 0;
		u32 itemCount = 0;

		StringKeyHasher hasher;
		if (dirPath.count)
		{
			StringBuffer<> forHash;
			forHash.Append(dirPath);
			if (CaseInsensitiveFs)
				forHash.MakeLower();
			hasher.Update(forHash.data, forHash.count);
		}

#if UBA_DEBUG_TRACK_DIR
		m_debugLogger->BeginScope();
		auto dg = MakeGuard([&]() { m_debugLogger->EndScope(); });
		StringBuffer<> str;
		str.Append(TCV("TRACKDIR "));
		if (isRefresh)
			str.Append(TCV("(Refresh) "));
		str.Append(dirPath).Append('\n');
		m_debugLogger->Log(LogEntryType_Info, str);
#endif


		Vector<u8> memoryBlock;
		memoryBlock.resize(4096);
		BinaryWriter memoryWriter(memoryBlock.data(), 0, memoryBlock.size());

		if (dirKey != m_directoryForcedEmpty)
		{
			StringBuffer<4> realPath;
			if constexpr (IsWindows)
			{
				if (dirPath.count == 2)
					dirPath = realPath.Append(dirPath).Append(PathSeparator);
			}
			else
			{
				if (!dirPath.count)
					dirPath = realPath.Append(PathSeparator);
			}

			bool res = TraverseDir(m_logger, dirPath,
				[&](const DirectoryEntry& e)
				{
					StringBuffer<256> fileNameForHash;
					fileNameForHash.Append(PathSeparator).Append(e.name, e.nameLen);
					if (CaseInsensitiveFs)
						fileNameForHash.MakeLower();

					StringKey fileKey = ToStringKey(hasher, fileNameForHash.data, fileNameForHash.count);
					auto res = dir.files.try_emplace(fileKey, ~0u);
					if (!res.second)
						return;
					UBA_ASSERT(e.attributes);
					UBA_ASSERT(e.nameLen < 512);

					res.first->second = {u32(memoryWriter.GetPosition())}; // Temporary offset that will be used further down to calculate the real offset

					memoryWriter.WriteString(e.name, e.nameLen);

#if UBA_DEBUG_TRACK_DIR
					m_debugLogger->Info(TC("    %s (Size: %llu, Attr: %u, Key: %s, Id: %llu)"), e.name, e.size, e.attributes, KeyToString(fileKey).data, e.id);
#endif

					u64 id = e.id;
					if (id == 0xffffffffffffffffllu) // When using projfs we might not have the file yet and in that case we need to make this up.
						id = ++m_fileIndexCounter;

					memoryWriter.WriteFileAttributes(e.attributes);
					memoryWriter.WriteVolumeSerial(e.volumeSerial == volumeSerial ? volumeSerialIndex : m_volumeCache.GetSerialIndex(e.volumeSerial));
					memoryWriter.WriteFileIndex(id);
					if (!IsDirectory(e.attributes))
					{
						memoryWriter.WriteFileTime(e.lastWritten);
						memoryWriter.WriteFileSize(e.size);
					}

					FileEntryAdded(res.first->first, e.lastWritten, e.size);


					++itemCount;
					if (memoryWriter.GetPosition() > memoryBlock.size() - MaxPath)
					{
						memoryBlock.resize(memoryBlock.size() * 2);
						memoryWriter.ChangeData(memoryBlock.data(), memoryBlock.size());
					}
				}, true,
				[&](const DirectoryInfo& e)
					{
						volumeSerial = e.volumeSerial;
						volumeSerialIndex = m_volumeCache.GetSerialIndex(volumeSerial);
						dirAttributes = e.attributes;
						fileIndex = e.id;
					});
				if (!res)
				{
#if UBA_DEBUG_TRACK_DIR
					m_debugLogger->Info(TC("    FAILED (not existing?)"));
#endif
					return false;
				}
		}
		else
		{
#if UBA_DEBUG_TRACK_DIR
			m_debugLogger->Info(TC("    FORCED EMPTY"));
#endif

			dirAttributes = DefaultDirAttributes();
		}

		written = memoryWriter.GetPosition();

		u64 storageSize = sizeof(StringKey) + Get7BitEncodedCount(dir.latestOffset) + Get7BitEncodedCount(itemCount) + written;

		u32 tableOffset;

		SCOPED_WRITE_LOCK(dirTable.m_memoryLock, memoryLock);
		u32 writePos = dirTable.m_memorySize;
		EnsureDirectoryTableMemory(writePos + 128 + storageSize);
		BinaryWriter tableWriter(m_directoryTableMem + dirTable.m_memorySize);

		if (isRefresh)
		{
			tableWriter.Write7BitEncoded(storageSize);
			tableWriter.WriteStringKey(dirKey);
			tableOffset = writePos + u32(tableWriter.GetPosition());
			tableWriter.Write7BitEncoded(dir.latestOffset);
		}
		else
		{
			storageSize += Get7BitEncodedCount(dirAttributes) + Get7BitEncodedCount(volumeSerialIndex) + sizeof(fileIndex);
			tableWriter.Write7BitEncoded(storageSize);
			tableWriter.WriteStringKey(dirKey);
			tableOffset = writePos + u32(tableWriter.GetPosition());
			tableWriter.Write7BitEncoded(dir.latestOffset);
			tableWriter.WriteFileAttributes(dirAttributes);
			tableWriter.WriteVolumeSerial(volumeSerialIndex);
			tableWriter.WriteFileIndex(fileIndex);
		}

		tableWriter.Write7BitEncoded(itemCount);
		u32 filesOffset = writePos + u32(tableWriter.GetPosition());
		tableWriter.WriteBytes(memoryBlock.data(), written);
		dirTable.m_memorySize += u32(tableWriter.GetPosition());

		memoryLock.Leave();

		// Update offsets to be relative to full memory
		for (auto& kv : dir.files)
			kv.second.internal += filesOffset;


		outTableOffset = tableOffset;
		dir.latestOffset = tableOffset;
		return true;
	}

	void DirectoryTableHolder::EnsureDirectoryTableMemory(u64 neededSize)
	{
		auto& dirTable = m_directoryTable;
		if (neededSize <= m_directoryTableMemCommitted)
			return;

		u64 newSize = AlignUp(neededSize, u64(1024*1024));
		if (newSize > DirTableMaxSize)
		{
			static bool called = m_logger.Error(TC("Directory table overflow. DirTableMaxSize need to be increased (Size: %llu)"), dirTable.m_memorySize);
		}

		u64 toCommit = newSize - m_directoryTableMemCommitted;
		u8* address = m_directoryTableMem + m_directoryTableMemCommitted;
		if (!FileMapping_Commit(m_logger, address, toCommit, false))
			m_logger.Error(TC("Failed to commit memory for directory table (Committed: %llu, ToCommit: %llu) (%s)"), m_directoryTableMemCommitted, toCommit, LastErrorToText().data);
		m_directoryTableMemCommitted += toCommit;
		LockMemory(m_logger, address, toCommit, TC("DirectoryTable"));
	}

	bool DirectoryTableHolder::IsInTemp(StringView path)
	{
		return false;
	}

	void DirectoryTableHolder::FileEntryAdded(StringKey filePathKey, u64 lastWritten, u64 size)
	{
	}

	bool DirectoryTableHolder::ShouldWriteToDisk(StringView fileName)
	{
		return true;
	}

	void DirectoryTableHolder::InvalidateCachedFileInfo(const StringKey& filePathKey)
	{
	}
}