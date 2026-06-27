// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

// No includes in this file!

////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
#define WRITE_DEBUG(format, ...) \
do { \
    char str[1024]; \
    int len = snprintf(str, sizeof(str), format "\n", ##__VA_ARGS__); \
	ssize_t ignored_result = write(STDERR_FILENO, str, (size_t)len); \
} while (0)
#endif

#if PLATFORM_WINDOWS
Bottleneck& g_createFileHandleBottleneck = *new Bottleneck(8); // Allocated and leaked just to prevent shutdown asserts in debug

HANDLE InternalCreateFileMappingW(Logger& logger, HANDLE hFile, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName, const tchar* hint)
{
	HANDLE res;
	if (flProtect == PAGE_READWRITE)
	{
		// Experiment to try to prevent lock happening on AWS servers when lots of helpers are sending back obj files.
		Timer timer;
		BottleneckScope bs(g_createFileHandleBottleneck, timer);
		res = ::CreateFileMappingW(hFile, NULL, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
	}
	else
		res = ::CreateFileMappingW(hFile, NULL, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

#if UBA_DEBUG_FILE_MAPPING
	if (res)
	{
		SCOPED_FUTEX(g_fileMappingsLock, lock);
		UBA_FOR_ASSERT(auto insres =) g_fileMappings.try_emplace(res, hint);
		UBA_ASSERT(insres.second);
		//logger.Info(TC("FILEMAPPING CREAT: %s %llu (%llu)"), hint, u64(res), g_fileMappings.size());
	}
#endif

	return res;
}
static void InternalDiscard(Logger& logger, u8* mem, u64 size, const tchar* from, const tchar* hint)
{
#if PLATFORM_WINDOWS
	u64 stepSize = 16ull*1024*1024*1024; // Extremely large.. here to test if it is faster to do multiple calls
	u64 left = size;
	while (left)
	{
		u64 toDiscard = Min(left, stepSize);
		DWORD res = DiscardVirtualMemory(mem, AlignUp(toDiscard, 4*1024));
		if (res != ERROR_SUCCESS)
			logger.Warning(TC("%s - Failed to discard memory (%s) (%u)"), from, hint, res);
		mem += toDiscard;
		left -= toDiscard;
	}
#endif
}
#else
Atomic<u64> g_mappingUidCounter = 0;
static pthread_mutex_t g_mappingUidCounterLock = PTHREAD_MUTEX_INITIALIZER;
void AppendPrefixAndValue(char* out, const char* prefix, u64 value, bool addSeparatorAfterPrefix = false)
{
	strcpy(out, prefix);
	out += strlen(prefix);
	if (addSeparatorAfterPrefix)
		*out++ = '/';
	constexpr char hexChars[] = "0123456789abcdef";
	for (int i=0;i!=8;++i)
	{
		*out++ = hexChars[(value >> 4) & 0xf];
		*out++ = hexChars[value & 0xf];
		value = value >> 8;
		if (!value)
			break;
	}
	*out = 0;
}
void GetMappingHandleName(char* out, u64 uid)
{
	#if PLATFORM_MAC
	if (!IsRunningDarling())
	{
		memcpy(out, "/tmp", 4);
		out += 4;
	}
	#endif
	AppendPrefixAndValue(out, "/uba_", uid);
}
u64 ParseMappingUid(const char* str)
{
	u64 result = 0;
	int shift = 0;
	while (str[0] && str[1] && shift < 64)
	{
		auto hexVal = [](char c) -> u8 { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
		result |= (u64)((hexVal(str[0]) << 4) | hexVal(str[1])) << shift;
		shift += 8;
		str += 2;
	}
	return result;
}

// Per-user directory for shm lock files. Avoids /tmp's sticky bit (can't unlink others'
// files), umask stripping mkdir(0777) down to 0700, and systemd PrivateTmp=yes isolating
// /tmp per service. Prefers $XDG_RUNTIME_DIR (per-user tmpfs), then $TMPDIR, then /tmp.
// Uses pthread_once rather than a function-local static so this compiles/links without
// libstdc++ (e.g. winegcc, which doesn't pull in __cxa_guard_acquire/_release).
char g_lockDirBuf[256];
pthread_once_t g_lockDirOnce = PTHREAD_ONCE_INIT;
void InitLockDir()
{
	const char* xdg = getenv("XDG_RUNTIME_DIR");
	if (xdg && *xdg)
		snprintf(g_lockDirBuf, sizeof(g_lockDirBuf), "%s/uba_shm_locks", xdg);
	else
	{
		const char* tmp = getenv("TMPDIR");
		if (!tmp || !*tmp)
			tmp = "/tmp";
		snprintf(g_lockDirBuf, sizeof(g_lockDirBuf), "%s/uba_shm_locks_%u", tmp, (unsigned)getuid());
	}
}
const char* GetLockDir()
{
	pthread_once(&g_lockDirOnce, InitLockDir);
	return g_lockDirBuf;
}

bool GarbageCollectFileMappings(Logger& logger)
{
	// Since we need to not leak file mappings we use files as a trick to know which ones are used and not
	char filePath[512];
	strcpy(filePath, GetLockDir());

	// Create dir (per-user, 0700 — also survives a strict umask unchanged)
	if (mkdir(filePath, 0700) == -1)
		if (errno != EEXIST)
		{
			logger.Error(TC("Failed to create %s for memory mapping (%s)"), filePath, strerror(errno));
			return false;
		}

	// Clear out all orphaned shm_open
	if (DIR* dir = opendir(filePath))
	{
		auto pathLen = strlen(filePath);
		filePath[pathLen++] = '/';
		while (true)
		{
			errno = 0;
			struct dirent* pDirent = readdir(dir);
			if (!pDirent)
			{
				if (errno == 0)
					break;

				// This is actually an error.. should return false?
				break;
			}

			char* fileName = pDirent->d_name;
			if (fileName[0] == '.' && (fileName[1] == 0 || (fileName[1] == '.' && fileName[2] == 0))) 
				continue;

			u32 uid = ParseMappingUid(fileName);

			strcpy(filePath+pathLen, fileName);
			int lockFd = open(filePath, O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
			if (lockFd == -1)
			{
				if (errno == EPERM)
				{
					g_mappingUidCounter = uid;
					return true;
				}
				logger.Warning("Failed to open %s for memory mapping (%s)", filePath, strerror(errno));
				UBA_ASSERTF(false, "Failed to open %s (%s)", filePath, strerror(errno));
				return false;
			}

			if (flock(lockFd, LOCK_EX | LOCK_NB) == 0)
			{
				char uidName[64];
				GetMappingHandleName(uidName, uid);
				if (shm_unlink(uidName) == 0)
					;// logger.Info("Removed old shared memory %s", uidName.data);
				remove(filePath);
			}
			else
			{
				g_mappingUidCounter = uid;
			}
			close(lockFd);
		}
		closedir(dir);
	}
	if (g_mappingUidCounter)
		logger.Info("Starting shared memory files at %u", g_mappingUidCounter.load());
	return true;
}
#endif

FileMappingHandle FileMapping_Create(Logger& logger, u32 flProtect, u64 maxSize, const fmchar* name, const fmchar* hint)
{
#if PLATFORM_WINDOWS
	ExtendedTimerScope ts(KernelStats::GetCurrent().createFileMapping);
	return u64(InternalCreateFileMappingW(logger, INVALID_HANDLE_VALUE, flProtect, (DWORD)ToHigh(maxSize), ToLow(maxSize), name, hint));
#else
	UBA_ASSERT(!name);
	UBA_ASSERT((flProtect & (~u32(PAGE_READWRITE | SEC_RESERVE))) == 0);

	pthread_mutex_lock(&g_mappingUidCounterLock);
	if (!g_mappingUidCounter)
		if (!GarbageCollectFileMappings(logger))
			return {};
	pthread_mutex_unlock(&g_mappingUidCounterLock);

	// Let's find a free shm name
	u64 uid;
	int shmFd;
	int lockFd;
	char lockFilePath[256];
	char uidName[64];

	int retryCount = 4;
	while (true)
	{
		uid = ++g_mappingUidCounter;

		AppendPrefixAndValue(lockFilePath, GetLockDir(), uid, true);

		lockFd = open(lockFilePath, O_CREAT | O_RDWR | O_NOFOLLOW | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
		if (lockFd == -1)
		{
			if (errno == EEXIST)
				continue;
			logger.Warning(TC("Failed to open/create %s (%s)"), lockFilePath, strerror(errno));
			UBA_ASSERTF(false, "Failed to open/create %s (%s)", lockFilePath, strerror(errno));
			continue;
		}

		if (flock(lockFd, LOCK_EX | LOCK_NB) != 0) // Some other process is using this one
		{
			close(lockFd);
			continue;
		}

		GetMappingHandleName(uidName, uid);

		int oflags = O_CREAT | O_RDWR | O_NOFOLLOW | O_EXCL;
		shmFd = shm_open(uidName, oflags, S_IRUSR | S_IWUSR);
		if (shmFd != -1)
			break;

		int err = errno;
		if (err == EEXIST)
		{
			// We hold the exclusive flock, so no other process owns this uid.
			// The shm is an orphan from a previous run that lost its lock file
			// (e.g. ftruncate failure). Unlink it and retry with a fresh shm.
			shm_unlink(uidName);
			shmFd = shm_open(uidName, oflags, S_IRUSR | S_IWUSR);
			if (shmFd != -1)
				break;
			err = errno;
		}

		bool retry = retryCount-- > 0;

		auto logType = retry ? LogEntryType_Warning : LogEntryType_Error;
		logger.Logf(logType, TC("Failed to create shm %s after getting lock-file %s (%s)"), uidName, lockFilePath, strerror(err));

		remove(lockFilePath);
		close(lockFd);

		if (retry)
			continue;

		SetLastError(err);
		return {};
	}

	if (maxSize != 0)
	{
		if (ftruncate(shmFd, (s64)maxSize) == -1)
		{
			SetLastError(errno);
			close(shmFd);
			shm_unlink(uidName);
			remove(lockFilePath);
			close(lockFd);
			//UBA_ASSERTF(false, "Failed to truncate file mapping '%s' to size %llu (%s)", name, maxSize, strerror(errno));
			return {};
		}
	}
	SetLastError(0);
	FileMappingHandle h(uid);
	h.shmFd = shmFd;
	h.lockFd = lockFd;
	return h;
#endif
}

FileMappingHandle FileMapping_CreateFromFile(Logger& logger, FileHandle file, u32 protect, u64 maxSize, const fmchar* hint)
{
#if PLATFORM_WINDOWS
	ExtendedTimerScope ts(KernelStats::GetCurrent().createFileMapping);
	return u64(InternalCreateFileMappingW(logger, AsHANDLE(file), protect, (DWORD)ToHigh(maxSize), ToLow(maxSize), NULL, hint));
#else
	FileMappingHandle h;
	int fd = AsFileDescriptor(file);
	if (maxSize && (protect & (~PAGE_READONLY)) != 0)
	{
#if PLATFORM_MAC // For some reason lseek+write does not work on apple silicon platform
		if (ftruncate(fd, maxSize) == -1)
		{
			logger.Error("ftruncate to %llu on fd %i failed for %s: %s\n", maxSize, fd, hint, strerror(errno));
			return h;
		}
#else
		if (lseek(fd, maxSize - 1, SEEK_SET) != maxSize - 1)
		{
			logger.Error("lseek to %llu failed for %s: %s\n", maxSize - 1, hint, strerror(errno));
			return h;
		}

		errno = 0;
		int res = write(fd, "", 1);
		if (res != 1)
		{
			logger.Error("write one byte at %llu on fd %i (%s) failed (res: %i): %s\n", maxSize - 1, fd, hint, res, strerror(errno));
			return h;
		}
#endif
	}

	h.shmFd = fd;
	h.internal = ~0ull;
	return h;
#endif
}

u8* FileMapping_Map(Logger& logger, FileMappingHandle fileMappingObject, u32 desiredAccess, u64 offset, u64 bytesToMap, bool allowDiscard)
{
#if PLATFORM_WINDOWS
	ExtendedTimerScope ts(KernelStats::GetCurrent().mapViewOfFile);
	u8* res = (u8*)::MapViewOfFile(AsHANDLE(fileMappingObject), desiredAccess, (DWORD)ToHigh(offset), ToLow(offset), bytesToMap);
	if (!res)
		return nullptr;
	if (allowDiscard)
		InternalDiscard(logger, res, bytesToMap, TC("MapViewOfFile"), TC(""));
#else
	int prot = 0;
	if (desiredAccess & FILE_MAP_READ)
		prot |= PROT_READ;
	if (desiredAccess & FILE_MAP_WRITE)
		prot |= PROT_WRITE;
	UBA_ASSERT(fileMappingObject.IsValid());
	int shmFd = fileMappingObject.shmFd;
	void* rptr = mmap(NULL, bytesToMap, prot, MAP_SHARED, shmFd, s64(offset));
	if (rptr == MAP_FAILED)
	{
		SetLastError(errno);
		return nullptr;
	}
	u8* res = (u8*)rptr;
	//UBA_ASSERTF(false, "Failed to map file with fd %i, desiredAccess %u offset %llu, bytesToMap %llu (%s)", fd, desiredAccess, offset, bytesToMap, strerror(errno));
#endif

#if UBA_DEBUG_FILE_MAPPING
	{
		SCOPED_FUTEX(g_fileMappingsLock, lock);
		auto findIt = g_fileMappings.find(fileMappingObject.mh);
		UBA_ASSERTF(findIt != g_fileMappings.end(), TC("Mapping nonexisting handle: %llu"), u64(fileMappingObject.mh));
		auto insres = g_viewMappings.try_emplace(res, fileMappingObject.mh);
		UBA_ASSERT(insres.second);
		++findIt->second.viewCount;
	}
#endif

	return res;
}

bool FileMapping_Commit(Logger& logger, void* address, u64 size, bool allowDiscard)
{
#if PLATFORM_WINDOWS
	ExtendedTimerScope ts(KernelStats::GetCurrent().virtualAlloc);
	if (!::VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE))
		return false;
	if (allowDiscard)
		InternalDiscard(logger, (u8*)address, size, TC("MapViewCommit"), TC(""));
	return true;
#else
	return true;
#endif
}

bool FileMapping_Unmap(Logger& logger, const void* address, u64 bytesToUnmap, const fmchar* hint, bool allowDiscard)
{
	if (!address)
		return true;

#if UBA_DEBUG_FILE_MAPPING
	{
		SCOPED_FUTEX(g_fileMappingsLock, lock);
		auto findIt = g_viewMappings.find(address);
		UBA_ASSERT(findIt != g_viewMappings.end());
		auto findIt2 = g_fileMappings.find(findIt->second);
		g_viewMappings.erase(findIt);
		UBA_ASSERTF(findIt2 != g_fileMappings.end(), TC("Unmap nonexisting handle: %llu"), u64(findIt->second));
		--findIt2->second.viewCount;
	}
#endif

#if PLATFORM_WINDOWS
	ExtendedTimerScope ts(KernelStats::GetCurrent().unmapViewOfFile);
	if (allowDiscard)
		InternalDiscard(logger, (u8*)address, bytesToUnmap, TC("UnmapViewOfFile"), hint);
	return ::UnmapViewOfFile(address);
#else
	UBA_ASSERTF(bytesToUnmap, TC("bytesToUnmap is zero unmapping %p (%s)"), address, hint);
	if (munmap((void*)address, bytesToUnmap) == 0)
		return true;
	UBA_ASSERT(false);
	return false;
#endif
}

bool FileMapping_Close(Logger& logger, FileMappingHandle h, const fmchar* hint)
{
	if (!h.IsValid())
		return true;

#if UBA_DEBUG_FILE_MAPPING
	SCOPED_FUTEX(g_fileMappingsLock, lock);
	auto findIt = g_fileMappings.find(h.mh);
	UBA_ASSERTF(findIt != g_fileMappings.end(), TC("Handle: %llu"), u64(h.mh));
	UBA_ASSERTF(!findIt->second.viewCount.load(), TC("Closing file mapping with %llu open views (%s)"), findIt->second.viewCount.load(), findIt->second.hint.c_str());
	logger.Info(TC("FILEMAPPING CLOSE: %s %llu (%llu)"), findIt->second.hint.c_str(), u64(h.mh), g_fileMappings.size() - 1);
	g_fileMappings.erase(findIt);
#endif

#if PLATFORM_WINDOWS
	return ::CloseHandle(AsHANDLE(h));
#else
	if (h.shmFd != -1 && h.internal != ~0ull)
		if (close(h.shmFd) != 0)
			UBA_ASSERTF(false, "Failed to close file mapping (%s)", strerror(errno));

	if (h.internal != 0 && h.internal != ~0ull)
	{
		char uidName[128];
		GetMappingHandleName(uidName, h.internal);
		if (shm_unlink(uidName) != 0)
		{
			SetLastError(errno);
			UBA_ASSERTF(false, "Failed to unlink %s (%s)", uidName, strerror(errno));
			return false;
		}

		char lockFile[256];
		AppendPrefixAndValue(lockFile, GetLockDir(), h.internal, true);
		remove(lockFile);
	}
	if (h.lockFd != -1)
		close(h.lockFd);
	return true;
#endif
}

void* FileMapping_ReservePlaceholder(void* baseAddress, u64 capacity)
{
#if PLATFORM_WINDOWS
	void* memory = VirtualAlloc2(GetCurrentProcess(), baseAddress, capacity, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
#else
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	if (baseAddress)
		flags |= MAP_FIXED;
	void* memory = mmap(baseAddress, capacity, PROT_NONE, flags, -1, 0);
	if (memory == MAP_FAILED)
		memory = nullptr;
#endif
	UBA_ASSERTF(memory, TC("VirtualAlloc2 failed (%s)"), LastErrorToText().data);
	return memory;
}

void* FileMapping_MapPlaceholder(FileMappingHandle handle, MemoryMapType mapType, void* targetAddress, u64 targetOffset, u64 targetCapacity, u64 handleOffset, u64 size)
{
#if PLATFORM_WINDOWS
	u32 protect = PAGE_READWRITE;
	if (mapType == MemoryMapType_ReadOnly)
		protect = PAGE_READONLY;
	else if (mapType == MemoryMapType_CopyOnWrite)
		protect = PAGE_WRITECOPY;

	size = AlignUp(size, 4*1024); // This is needed for VirtualFree to align properly

	if (u64 toRelease = targetCapacity - targetOffset - size)
	{
		//TimerScope ts(kernelStats.virtualFree);
		bool res = VirtualFree((u8*)targetAddress + targetOffset + size, toRelease, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
		//UBA_ASSERTF(res, TC("VirtualFree for placeholder failed releasing %llu at %llu (Capacity: %llu MappedSize: %llu) - %s (%s)"), toRelease, size, m_capacity, m_mappedSize, LastErrorToText().data, hint);
		if (!res)
			return nullptr;//false;
	}

	//kernelStats.mapViewOfFile3.bytes += size;
	//TimerScope ts(kernelStats.mapViewOfFile3);
	void* viewA = MapViewOfFile3(AsHANDLE(handle), GetCurrentProcess(), (u8*)targetAddress+targetOffset, handleOffset, size, MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0);
#else
	//UBA_ASSERT(mapType != MemoryMapType_CopyOnWrite);
	u32 protect = PROT_READ;
	u32 mapFlags = MAP_FIXED;

	if (mapType == MemoryMapType_CopyOnWrite)
	{
		mapFlags |= MAP_PRIVATE;
		protect |= PROT_WRITE;
	}
	else
	{
		mapFlags |= MAP_SHARED;
		if (mapType == MemoryMapType_ReadWrite)
			protect |= PROT_WRITE;
	}
	//TimerScope ts(kernelStats.mapViewOfFile3);
	void* viewA = mmap((u8*)targetAddress+targetOffset, size, protect, mapFlags, handle.shmFd, handleOffset);
	if (viewA == MAP_FAILED)
		viewA = nullptr;
#endif
	return viewA;
}

bool FileMapping_UnmapPlaceholder(void* memory, u64 capacity, u64 mappedSize, u64* subMappings, u64 subMappingsCount)
{
#if PLATFORM_WINDOWS
	bool res; (void)res;
	if (mappedSize)
	{
		auto& kernelStats = KernelStats::GetCurrent();
		{
			TimerScope ts(kernelStats.unmapViewOfFile2);
			res = UnmapViewOfFile2(GetCurrentProcess(), memory, MEM_PRESERVE_PLACEHOLDER);
			UBA_ASSERT(res);
		}

		for (u64 i=0;i!=subMappingsCount; ++i)
		{
			TimerScope ts(kernelStats.unmapViewOfFile2);
			res = UnmapViewOfFile2(GetCurrentProcess(), (u8*)memory + subMappings[i], MEM_PRESERVE_PLACEHOLDER);
			UBA_ASSERT(res);
		}

		if (mappedSize < capacity || subMappingsCount)
		{
			TimerScope ts(kernelStats.virtualFree);
			res = VirtualFree(memory, capacity, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
			UBA_ASSERTF(res, TC("VirtualFree failed (%u)"), GetLastError());
		}
		mappedSize = 0;
	}
	res = VirtualFree(memory, 0, MEM_RELEASE);
	UBA_ASSERTF(res, TC("VirtualFree failed (%u)"), GetLastError());
#else
	int res = munmap(memory, capacity);
	UBA_ASSERT(res == 0); (void)res;
#endif
	return true;
}

FileMappingHandle FileMapping_DuplicateFromHost(FileMappingHandle handle, const char* hint)
{
#if PLATFORM_WINDOWS
	HANDLE realHandle;
	if (!DuplicateHandle(g_hostProcess, AsHANDLE(handle), GetCurrentProcess(), &realHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
	{
		UBA_ASSERTF(false, L"Failed to duplicate %S handle (%u)", hint, GetLastError());
		return {};
	}
	return FileMappingHandle(u64(realHandle));
#else
	char uidName[128];
	GetMappingHandleName(uidName, handle.internal);
	int shmFd = shm_open(uidName, O_RDWR, S_IRUSR | S_IWUSR);
	UBA_ASSERTF(shmFd != -1, "shm_open failed for %s and handle %s (%s)", hint, uidName, strerror(errno));
	if (shmFd == -1)
		return {};
	FileMappingHandle newHandle = handle;
	newHandle.shmFd = shmFd;
	return newHandle;
#endif
}

u8* FileMapping_MapFromHost(FileMappingHandle handle, u64 size, u64 offset, bool writable, const char* hint)
{
#if PLATFORM_WINDOWS
	DWORD dwDesiredAccess = FILE_MAP_READ;
	if (writable)
		dwDesiredAccess |= FILE_MAP_WRITE;
	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)offset;
	//TimerScope ts(g_kernelStats.mapViewOfFile);
	u8* mem = (u8*)::MapViewOfFile(AsHANDLE(handle), dwDesiredAccess, li.HighPart, li.LowPart, size);
	UBA_ASSERT(mem);
	::CloseHandle(AsHANDLE(handle));
	return mem;
#else
	char uidName[128];
	GetMappingHandleName(uidName, handle.internal);
	int shmFd = shm_open(uidName, O_RDONLY, S_IRUSR | S_IWUSR);
	UBA_ASSERTF(shmFd != -1, "shm_open failed for %s and handle %s (%s)", hint, uidName, strerror(errno));
	u8* mem = (u8*)mmap(NULL, size, PROT_READ, MAP_SHARED, shmFd, 0);
	UBA_ASSERTF(mem != MAP_FAILED, "mmap failed (%s)", strerror(errno));
	close(shmFd);
	return mem;
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////
