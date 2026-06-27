// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaDetoursSharedPosix.h" // Must be first

#include "UbaDetoursGoPosix.h"
#include "UbaFileMapping.h"
#include "UbaPlatform.h"
#include "UbaProcessStats.h"
#include "UbaProcessUtils.h"
#include "UbaProtocol.h"
#include "UbaStringBuffer.h"
#include "UbaTimer.h"
#include <sys/wait.h>
#include <dirent.h> 
#include <glob.h>
#include <dlfcn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>

// undefs for mimalloc
#undef realpath
#undef malloc

#if PLATFORM_LINUX
#include <sys/prctl.h>
#include <sys/sysmacros.h>
#else
#include <crt_externs.h>
extern char** environ;
#endif

#define UBA_DETOUR_DEBUG 0//UBA_DEBUG

namespace uba
{
	#include "UbaFileMapping.inl"

	constexpr bool g_logToScreen = false;

	bool g_isDetouring;
	bool g_isInitialized;
	bool g_isCancelled;
	u32 g_processId;
	pid_t g_pid;
	void Deinit();
	bool TakeLockForRpc();

	SharedMemoryAllocatorHandle g_sharedMemoryAllocator;
	u8* g_sharedMemory;

	extern int g_comFd;
	extern SharedEvent* g_cancelEvent;
	extern SharedEvent* g_readEvent;
	extern SharedEvent* g_writeEvent;
	extern u8* g_messageMappingMem;

	bool g_isUsingLocalBinary;
}

using namespace uba;

#define UBA_EXPORT __attribute__((visibility("default"))) 

#define DETOURED_FUNCTIONS \
	DETOURED_FUNCTION(chdir) \
	DETOURED_FUNCTION(fchdir) \
	DETOURED_FUNCTION(mkdir) \
	DETOURED_FUNCTION(rmdir) \
	DETOURED_FUNCTION(chroot) \
	DETOURED_FUNCTION(getcwd) \
	DETOURED_FUNCTION(getenv) \
	DETOURED_FUNCTION(setenv) \
	DETOURED_FUNCTION(unsetenv) \
	DETOURED_FUNCTION(realpath) \
	DETOURED_FUNCTION(readlink) \
	DETOURED_FUNCTION(readlinkat) \
	DETOURED_FUNCTION(read) \
	DETOURED_FUNCTION(pread) \
	DETOURED_FUNCTION(open) \
	DETOURED_FUNCTION(openat) \
	DETOURED_FUNCTION(creat) \
	DETOURED_FUNCTION(dup) \
	DETOURED_FUNCTION(dup2) \
	DETOURED_FUNCTION(close) \
	DETOURED_FUNCTION(fopen) \
	DETOURED_FUNCTION(fdopen) \
	DETOURED_FUNCTION(freopen) \
	DETOURED_FUNCTION(fchmod) \
	DETOURED_FUNCTION(fchmodat) \
	DETOURED_FUNCTION(fstat) \
	DETOURED_FUNCTION(faccessat) \
	DETOURED_FUNCTION(fstatat) \
	DETOURED_FUNCTION(futimens) \
	DETOURED_FUNCTION(fclose) \
	DETOURED_FUNCTION(mkstemp) \
	DETOURED_FUNCTION(opendir) \
	DETOURED_FUNCTION(fdopendir) \
	DETOURED_FUNCTION(dirfd) \
	DETOURED_FUNCTION(readdir) \
	DETOURED_FUNCTION(rewinddir) \
	DETOURED_FUNCTION(scandir) \
	DETOURED_FUNCTION(seekdir) \
	DETOURED_FUNCTION(telldir) \
	DETOURED_FUNCTION(closedir) \
	DETOURED_FUNCTION(stat) \
	DETOURED_FUNCTION(truncate) \
	DETOURED_FUNCTION(lstat) \
	DETOURED_FUNCTION(lseek) \
	DETOURED_FUNCTION(glob) \
	DETOURED_FUNCTION(chmod) \
	DETOURED_FUNCTION(rename) \
	DETOURED_FUNCTION(renameat) \
	DETOURED_FUNCTION(utimensat) \
	DETOURED_FUNCTION(remove) \
	DETOURED_FUNCTION(link) \
	DETOURED_FUNCTION(linkat) \
	DETOURED_FUNCTION(unlink) \
	DETOURED_FUNCTION(unlinkat) \
	DETOURED_FUNCTION(symlink) \
	DETOURED_FUNCTION(access) \
	DETOURED_FUNCTION(eaccess) \
	DETOURED_FUNCTION(posix_spawn) \
	DETOURED_FUNCTION(posix_spawnp) \
	DETOURED_FUNCTION(wait) \
	DETOURED_FUNCTION(waitpid) \
	DETOURED_FUNCTION(waitid) \
	DETOURED_FUNCTION(wait3) \
	DETOURED_FUNCTION(wait4) \
	DETOURED_FUNCTION(system) \
	DETOURED_FUNCTION(dlopen) \
	DETOURED_FUNCTION(dladdr) \
	DETOURED_FUNCTION(execv) \
	DETOURED_FUNCTION(execve) \
	DETOURED_FUNCTION(execvp) \
	DETOURED_FUNCTION(execl) \
	DETOURED_FUNCTION(execle) \
	DETOURED_FUNCTION(execlp) \
	DETOURED_FUNCTION(fork) \
	DETOURED_FUNCTION(vfork) \
	DETOURED_FUNCTION(popen) \
	DETOURED_FUNCTION(fgets) \
	DETOURED_FUNCTION(pclose) \
	DETOURED_FUNCTION(exit) \
	DETOURED_FUNCTION(_exit) \
	DETOURED_FUNCTION(_Exit) \
	DETOURED_FUNCTION_DEBUG \
	DETOURED_FUNCTION_LINUX \
	DETOURED_FUNCTION_MACOS \
	
// Used by cp and might need detour
// fadvise64
// copy_file_range

#if UBA_DETOUR_DEBUG && PLATFORM_LINUX
#define DETOURED_FUNCTION_DEBUG \
	DETOURED_FUNCTION(write) \
	DETOURED_FUNCTION(fwrite) \

#else
#define DETOURED_FUNCTION_DEBUG
#endif

#if PLATFORM_LINUX
#define DETOURED_FUNCTION_MACOS
#define DETOURED_FUNCTION_LINUX \
	DETOURED_FUNCTION(dup3) \
	DETOURED_FUNCTION(clone) \
	DETOURED_FUNCTION(fexecve) \
	DETOURED_FUNCTION(get_current_dir_name) \
	DETOURED_FUNCTION(fopen64) \
	DETOURED_FUNCTION(freopen64) \
	DETOURED_FUNCTION(secure_getenv) \
	DETOURED_FUNCTION(fcntl) \
	DETOURED_FUNCTION(fcntl64) \
	DETOURED_FUNCTION(__xstat) \
	DETOURED_FUNCTION(__xstat64) \
	DETOURED_FUNCTION(__lxstat) \
	DETOURED_FUNCTION(__lxstat64) \
	DETOURED_FUNCTION(__fxstat) \
	DETOURED_FUNCTION(__fxstat64) \
	DETOURED_FUNCTION(__fxstatat) \
	DETOURED_FUNCTION(__fxstatat64) \
	DETOURED_FUNCTION(pread64) \
	DETOURED_FUNCTION(open64) \
	DETOURED_FUNCTION(openat64) \
	DETOURED_FUNCTION(creat64) \
	DETOURED_FUNCTION(listxattr) \
	DETOURED_FUNCTION(stat64) \
	DETOURED_FUNCTION(lstat64) \
	DETOURED_FUNCTION(fstat64) \
	DETOURED_FUNCTION(scandir64) \
	DETOURED_FUNCTION(readdir64) \
	DETOURED_FUNCTION(fstatat64) \
	DETOURED_FUNCTION(fpathconf) \
	DETOURED_FUNCTION(pathconf) \
	DETOURED_FUNCTION(syscall) \
	DETOURED_FUNCTION(statx) \

#else
#define DETOURED_FUNCTION_LINUX
#define DETOURED_FUNCTION_MACOS \
	DETOURED_FUNCTION(_NSGetExecutablePath) \
	DETOURED_FUNCTION(execvP) \

#endif

#if (PLATFORM_MAC)
	// On Apple platforms when interposing, we need to have a unique name of our function
	// While on Linux we need to use original name
	// This macro will prepend "uba_" to the mac versions of the functions
	// so they can be used with the interpose macro 
	#define UBA_WRAPPER(func) uba_##func
	#define TRUE_WRAPPER(func) func

	// This magic macro that actually does the hooking, aka interposing on Apple platforms
	#ifndef DYLD_INTERPOSE
	#define DYLD_INTERPOSE(_replacement,_replacee) \
		__attribute__((used)) static struct{ const void* replacement; const void* replacee; } _interpose_##_replacee \
			__attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };
	#endif // DYLD_INTERPOSE
#else
	// Linux LD_PRELOAD path: UBA_WRAPPER(func) is `func` itself. Our exported
	// symbol shadows libc's, and True_<func> is populated via dlsym(RTLD_NEXT)
	// in PreInit().
	#define UBA_WRAPPER(func) func
	#define TRUE_WRAPPER(func) True_##func

	#define DETOURED_FUNCTION(func) \
		using Symbol_##func = decltype(func); \
		Symbol_##func* True_##func; 
	DETOURED_FUNCTIONS
	#undef DETOURED_FUNCTION
#endif

#include "UbaBinaryParser.h"
#include "UbaDetoursGoPosix.h"

#if 0
#define	EPERM		 1	/* Operation not permitted */
#define	ENOENT		 2	/* No such file or directory */
#define	ESRCH		 3	/* No such process */
#define	EINTR		 4	/* Interrupted system call */
#define	EIO			 5	/* I/O error */
#define	ENXIO		 6	/* No such device or address */
#define	E2BIG		 7	/* Argument list too long */
#define	ENOEXEC		 8	/* Exec format error */
#define	EBADF		 9	/* Bad file number */
#define	ECHILD		10	/* No child processes */
#define	EAGAIN		11	/* Try again */
#define	ENOMEM		12	/* Out of memory */
#define	EACCES		13	/* Permission denied */
#define	EFAULT		14	/* Bad address */
#define	ENOTBLK		15	/* Block device required */
#define	EBUSY		16	/* Device or resource busy */
#define	EEXIST		17	/* File exists */
#define	EXDEV		18	/* Cross-device link */
#define	ENODEV		19	/* No such device */
#define	ENOTDIR		20	/* Not a directory */
#define	EISDIR		21	/* Is a directory */
#define	EINVAL		22	/* Invalid argument */
#define	ENFILE		23	/* File table overflow */
#define	EMFILE		24	/* Too many open files */
#define	ENOTTY		25	/* Not a typewriter */
#define	ETXTBSY		26	/* Text file busy */
#define	EFBIG		27	/* File too large */
#define	ENOSPC		28	/* No space left on device */
#define	ESPIPE		29	/* Illegal seek */
#define	EROFS		30	/* Read-only file system */
#define	EMLINK		31	/* Too many links */
#define	EPIPE		32	/* Broken pipe */
#define	EDOM		33	/* Math argument out of domain of func */
#define	ERANGE		34	/* Math result not representable */
#endif

void CloseCom();

namespace uba
{
	template<typename TrueFunc>
	void InitDetour(TrueFunc*& trueFunc, const char* func)
	{
		if (trueFunc)
			return;
		trueFunc = (TrueFunc*)dlsym(RTLD_NEXT, func);
		if (trueFunc)
			return;
		printf("dlsym failed on %s: %s\n", func, dlerror());
	}

#if PLATFORM_LINUX
	// t_disallowDetour is now a TLS-free expression (expands to DisallowDetourDepth()),
	// so it is safe to call on goroutine OS threads that lack a glibc TCB.
	#define UBA_INIT_DETOUR(func, ...) \
		if (!g_isDetouring || t_disallowDetour) \
		{ \
			InitDetour(True_##func, #func); \
			return TRUE_WRAPPER(func)(__VA_ARGS__); \
		}
#elif PLATFORM_MAC
	#define UBA_INIT_DETOUR(func, ...) \
		if (!g_isDetouring || t_disallowDetour) \
		{ \
			return TRUE_WRAPPER(func)(__VA_ARGS__); \
		}
#endif

	const char* StrError(int res, int error)
	{
		if (res != -1)
			return "Success";
		return strerror(error);
	}

	u8 GetFileAccessFlags(int flags)
	{
		u8 access = 0;
		if (!flags)
			access |= AccessFlag_Read;
		else if (flags & O_RDWR)
			access |= AccessFlag_Read | AccessFlag_Write;
		else if (flags & O_RDONLY)
			access |= AccessFlag_Read;
		else if (flags & O_WRONLY)
			access |= AccessFlag_Write;
		return access;
	}

	struct DirInfo
	{
		Vector<DirTableOffset> fileTableOffsets;
		int it = -1;
		dirent ent;
	};

	bool IsDirInfo(DIR* dir) { return (u64(dir) & 0x1000'0000'0000'0000) != 0; }
	DirInfo* AsDirInfo(DIR* dir) { return (DirInfo*)(uintptr_t(dir) & ~0x1000'0000'0000'0000); }

	struct FileObject
	{
		FileInfo* fileInfo = nullptr;
		DirInfo*  dirInfo  = nullptr; // non-null only for placeholder directory fds (getdents64 source)
		u32 refCount = 1;
		u32 closeId = 0;
		u32 desiredAccess = 0;
		bool deleteOnClose = false;
		bool ownsFileInfo = false;
		bool isDirectory = false;
		TString newName;
	};

	// TODO: Change this to same style as windows implementation
	struct DetouredHandle
	{
		FileObject* fileObject = nullptr;
	};

	using FileHandles = UnorderedMap<int, DetouredHandle>;
	VARIABLE_MEM(FileHandles, g_fileHandles);
	VARIABLE_MEM(ReaderWriterLock, g_fileHandlesLock);


	StringKey ToFilenameKey(const StringBufferBase& b) { return CaseInsensitiveFs ? ToStringKeyLower(b) : ToStringKey(b); }
	StringKey ToFilenameKey(StringView b) { return CaseInsensitiveFs ? ToStringKeyLower(b) : ToStringKey(b); }

	bool CouldBeCompressedFile(const StringView& fileName) { return false; }
	bool CanDetour(StringView file)
	{
		if (t_disallowDetour)
			return false;
		return g_rules->CanDetour(file, g_runningRemote);
	}

}

bool CanDetour2(const StringView& file)
{
	if (!g_isDetouring || t_disallowDetour)
		return false;

	if (file.StartsWith("/dev/") || file.StartsWith("/etc/"))
		return false;

	#if PLATFORM_LINUX
	if (file.StartsWith("/sys/") || file.StartsWith("/run/"))
		return false;// Don't know if this is needed for macos.. but is needed for linux
	#endif

	if (g_isUsingLocalBinary)
		if (file.StartsWith("/usr/lib64") || file.StartsWith("/lib64"))
			return false;

	return true;
}

// Shared functions

#if UBA_DEBUG_LOG_ENABLED
static StringBuffer<256> OpenFlagsToString(int flags)
{
	StringBuffer<256> buf;
	auto append = [&](const char* s) { if (buf.count) buf.Append('|'); buf.Append(s); };
	int acc = flags & O_ACCMODE;
	if      (acc == O_RDONLY) append("O_RDONLY");
	else if (acc == O_WRONLY) append("O_WRONLY");
	else if (acc == O_RDWR)   append("O_RDWR");
	if (flags & O_CREAT)     append("O_CREAT");
	if (flags & O_TRUNC)     append("O_TRUNC");
	if (flags & O_APPEND)    append("O_APPEND");
	if (flags & O_NONBLOCK)  append("O_NONBLOCK");
	if (flags & O_CLOEXEC)   append("O_CLOEXEC");
	if (flags & O_EXCL)      append("O_EXCL");
	if (flags & O_NOCTTY)    append("O_NOCTTY");
	if (flags & O_NOFOLLOW)  append("O_NOFOLLOW");
	if (flags & O_SYNC)      append("O_SYNC");
	#if PLATFORM_LINUX
	if (flags & O_TMPFILE)   append("O_TMPFILE");
	#endif
	if (flags & O_DIRECTORY) append("O_DIRECTORY");
	return buf;
}
#endif

// Action dispatched by Shared_open / Shared_freopen after Shared_prepareOpen
// has normalized the path, checked dirtable existence, and (for remote runs)
// translated the path to a staging location via Rpc_CreateFileW.
enum class SharedOpenAction
{
	Error,             // errno set in prep.err; caller returns failure.
	NoDetourFallback,  // Call real syscall with original path; TrackInput on success.
	ProcFallback,      // Linux /proc/*: call real syscall, no TrackInput.
	DirectoryFd,       // Synthesize a fake fd over /dev/null with DirInfo attached.
	UnsupportedFail,   // $-prefix / memory-handle / keepInMemory: UBA_ASSERTF already fired in prep.
	NormalOpen,        // Call real syscall with prep.tempFileName / prep.trueFlags / prep.trueMode.
};

struct SharedOpenPrep
{
	SharedOpenAction action = SharedOpenAction::Error;
	StringBuffer<> fileName;                          // Normalized, devirtualized path.
	StringKey fileNameKey;                            // Key into g_mappedFileTable / g_directoryTable.
	u32 desiredAccess = 0;                            // AccessFlag_Read / AccessFlag_Write bits.
	bool isWrite = false;
	DirectoryTable::EntryInformation entryInfo{};     // Valid if fileExists.
	bool fileExists = false;
	int err = 0;                                      // For Error action.
	const char* tempFileName = nullptr;               // For NormalOpen: path to hand the real syscall.
	int trueFlags = 0;                                // For NormalOpen: possibly O_CREAT-augmented flags.
	int trueMode = 0;                                 // For NormalOpen: possibly derived creation mode.
	Vector<u8> dataToWrite;                           // For NormalOpen: preload bytes (O_APPEND / O_RDWR on remote).
	bool isDirectoryEntry = false;                    // For NormalOpen: IsDirectory(entryInfo.attributes).
	FileInfo* fileInfo = nullptr;                     // DirectoryFd: fresh alloc; NormalOpen: &g_mappedFileTable entry.
	u32 closeId = 0;                                  // RPC write-back id, if Rpc_CreateFileW returned one.
};

// Register an fd in g_fileHandles with the given prep state. Shared by the
// NormalOpen fast-path and the DirectoryFd /dev/null synthesis.
inline DetouredHandle& CreateFileTableEntry(int fd, const SharedOpenPrep& prep, bool isDirectory, const char* funcName, const char* hint)
{
	UBA_ASSERT(prep.fileInfo);
	SCOPED_WRITE_LOCK(g_fileHandlesLock, lock);
	auto insres = g_fileHandles.insert({ fd, DetouredHandle() });
	UBA_ASSERTF(insres.second, "File handle %i already added. %s (%s)", fd, funcName, hint);
	DetouredHandle& h = insres.first->second;
	auto fo = new FileObject();
	fo->closeId = prep.closeId;
	fo->fileInfo = prep.fileInfo;
	fo->desiredAccess = prep.desiredAccess;
	fo->isDirectory = isDirectory;
	h.fileObject = fo;
	return h;
}

// Synthesize a directory fd by opening /dev/null and attaching a DirInfo
// snapshot so getdents64 on the fd can emit the entries we know about.
// Mirrors the opendir flow.
inline int CreateDirFd(const SharedOpenPrep& prep, int flags, const char* funcName)
{
	StringBuffer<> forHash(prep.fileName);
	if (forHash.count == 1)
		forHash.Resize(0);
	if (CaseInsensitiveFs)
		forHash.MakeLower();
	DirHash hash(forHash);

	DirInfo* newDirInfo = new DirInfo();
	{
		SCOPED_WRITE_LOCK(g_directoryTable.m_lookupLock, lookLock);
		auto dirInsres = g_directoryTable.m_lookup.try_emplace(hash.key, g_memoryBlock);
		DirectoryTable::Directory& dir = dirInsres.first->second;
		if (dirInsres.second)
			if (g_directoryTable.EntryExistsNoLock(hash.key, forHash) != DirectoryTable::Exists_No)
				Rpc_UpdateDirectory(hash.key, prep.fileName.data, prep.fileName.count, false);
		g_directoryTable.PopulateDirectory(hash.open, dir);

		SCOPED_READ_LOCK(dir.lock, dirLock);
		newDirInfo->fileTableOffsets.resize(dir.files.size());
		u32 i = 0;
		for (auto& pair : dir.files)
			newDirInfo->fileTableOffsets[i++] = pair.second;
	}

	int fd = TRUE_WRAPPER(open)("/dev/null", O_RDONLY);

	auto& dh = CreateFileTableEntry(fd, prep, true, funcName, prep.fileName.data);
	dh.fileObject->dirInfo = newDirInfo;
	DEBUG_LOG_DETOURED(funcName, "(DIR) %s (%s) -> %i", prep.fileName.data, OpenFlagsToString(flags).data, fd);
	return fd;
}

// Forward declaration: Shared_prepareOpen's readDataFromOldFile lambda recurses
// into Shared_open to pull preload bytes for O_APPEND / O_RDWR on remote builds.
template<typename TrueOpen>
int Shared_open(const char* funcName, const char* file, int flags, int mode, const TrueOpen& trueOpen);

// Runs the full prepare phase for open()/openat()/fopen()/freopen():
// - FixPath + DevirtualizePath
// - CanDetour2 / "/" / /proc/ fallthroughs
// - Rpc_GetEntryOffset existence / attribute check
// - O_CREAT/O_EXCL/O_DIRECTORY error cases
// - g_mappedFileTable lookup + Rpc_CreateFileW for writes (remote staging path)
// - $-prefix, keepInMemory, IsMemoryHandle rejections
// - readDataFromOldFile preload for remote O_APPEND / O_RDWR
// - final trueFlags / trueMode computation
// Does NOT call the real syscall — caller dispatches on prep.action.
// The trueOpen callback is only used for the readDataFromOldFile recursion.
template<typename TrueOpen>
void Shared_prepareOpen(const char* funcName, const char* file, int flags, int mode, const TrueOpen& trueOpen, SharedOpenPrep& prep)
{
	FixPath(prep.fileName, file);
	DevirtualizePath(prep.fileName);

	prep.desiredAccess = GetFileAccessFlags(flags);
	prep.isWrite = (prep.desiredAccess & AccessFlag_Write) != 0;
	prep.trueFlags = flags;
	prep.trueMode = mode;

	if (!CanDetour2(prep.fileName) || prep.fileName.Equals("/"))
	{
		prep.action = SharedOpenAction::NoDetourFallback;
		return;
	}

	#if PLATFORM_LINUX
	if (prep.fileName.StartsWith("/proc/"))
	{
		if (prep.fileName.StartsWith("/proc/self/cmdline"))
			DEBUG_LOG("TODO!!! /proc/self/cmdline");
		prep.action = SharedOpenAction::ProcFallback;
		return;
	}
	#endif

	prep.fileNameKey = ToFilenameKey(prep.fileName);
	DirTableOffset dirTableOffset;
	bool foundEntry = Rpc_GetEntryOffset(dirTableOffset, prep.fileNameKey, prep.fileName, false);
	if (foundEntry)
		g_directoryTable.GetEntryInformation(prep.entryInfo, dirTableOffset);
	prep.fileExists = foundEntry && prep.entryInfo.attributes != 0;

	if (!(flags & O_CREAT))
	{
		if (!prep.fileExists)
		{
			prep.err = ENOENT;
			DEBUG_LOG_DETOURED(funcName, "(DIRTABLE) %s -> -1 (ENOENT)", prep.fileName.data);
			prep.action = SharedOpenAction::Error;
			return;
		}
	}
	else if (const tchar* lastSeparator = prep.fileName.Last(PathSeparator))
	{
		if (prep.fileExists)
		{
			if (flags & O_EXCL)
			{
				prep.err = EEXIST;
				DEBUG_LOG_DETOURED(funcName, "(DIRTABLE) %s -> -1 (EEXIST)", prep.fileName.data);
				prep.action = SharedOpenAction::Error;
				return;
			}
		}
		else
		{
			// Check so parent directory exist
			StringView dirName(prep.fileName.data, u32(lastSeparator - prep.fileName.data));
			DirHash hash(dirName);

			SCOPED_WRITE_LOCK(g_directoryTable.m_lookupLock, lookupLock);
			auto insres = g_directoryTable.m_lookup.try_emplace(hash.key, g_memoryBlock);
			DirectoryTable::Directory& dir = insres.first->second;
			if (insres.second)
				if (g_directoryTable.EntryExistsNoLock(hash.key, dirName) != DirectoryTable::Exists_No)
					Rpc_UpdateDirectory(hash.key, dirName.data, dirName.count, false);
			DirTableOffset dirTableOffset2;
			if (!g_directoryTable.GetLatestOffset(dirTableOffset2, dir))
			{
				prep.err = ENOENT;
				DEBUG_LOG_DETOURED(funcName, "(DIRTABLE) %s -> -1 (ENOENT)", prep.fileName.data);
				prep.action = SharedOpenAction::Error;
				return;
			}
		}
	}
	else
	{
		UBA_ASSERT(false); // Should never end up here
	}

	if (prep.fileExists)
	{
		if (IsDirectory(prep.entryInfo.attributes))
		{
			// !(flags & O_DIRECTORY) // O_DIRECTORY is actually optional

			#if PLATFORM_LINUX
			if ((flags & O_TMPFILE) && flags & (O_WRONLY|O_RDWR))
			{
				UBA_ASSERTF(false, "Tmpfile not implemented");
				prep.action = SharedOpenAction::UnsupportedFail;
				return;
			}
			#endif

			if ((flags & (O_WRONLY | O_RDWR | O_CREAT)) != 0)
			{
				prep.err = EISDIR;
				DEBUG_LOG_DETOURED(funcName, "(DIRTABLE) %s (%s) -> -1 (EISDIR)", prep.fileName.data, OpenFlagsToString(flags).data);
				prep.action = SharedOpenAction::Error;
				return;
			}

			if (g_runningRemote) // All fd for directories should probably take this path of making /dev/null handles
			{
				prep.fileInfo = new (g_memoryBlock.Allocate(sizeof(FileInfo), 8, "FileInfo")) FileInfo();
				prep.fileInfo->originalName = g_memoryBlock.Strdup(prep.fileName).data;
				prep.fileInfo->name = prep.fileInfo->originalName;
				prep.action = SharedOpenAction::DirectoryFd;
				return;
			}
		}
		else
		{
			if (flags & O_DIRECTORY)
			{
				prep.err = ENOTDIR;
				DEBUG_LOG_DETOURED(funcName, "(DIRTABLE) %s (%s) -> -1 (ENOTDIR)", prep.fileName.data, OpenFlagsToString(flags).data);
				prep.action = SharedOpenAction::Error;
				return;
			}
		}
	}

	bool keepInMemory = false;

	u64 size = InvalidValue;

	const char* realFileName = prep.fileName.data;
	const char* prevFileName = nullptr;

	SCOPED_WRITE_LOCK(g_mappedFileTable.m_lookupLock, fileTableLock);
	auto insres = g_mappedFileTable.m_lookup.try_emplace(prep.fileNameKey);
	FileInfo& info = insres.first->second;
	prep.fileInfo = &info;
	u32 lastDesiredAccess = info.lastDesiredAccess; (void)lastDesiredAccess;
	if (insres.second || info.recursiveCall)
	{
		info.originalName = g_memoryBlock.Strdup(prep.fileName).data;
		info.name = info.originalName;
		if (!keepInMemory)
		{
			char newFileName[512];
			Rpc_CreateFileW(prep.fileName, prep.fileNameKey, u8(prep.desiredAccess), newFileName, sizeof_array(newFileName), size, prep.closeId, false);
			info.name = g_memoryBlock.Strdup(newFileName);
			realFileName = info.name;
		}

		info.size = size;
		info.fileNameKey = prep.fileNameKey;
		info.lastDesiredAccess = prep.desiredAccess;
	}
	else
	{
		if (!info.originalName)
			info.originalName = g_memoryBlock.Strdup(prep.fileName).data;
		if (prep.isWrite)
		{
			prevFileName = info.name;
			//UBA_ASSERT(!info.isFileMap);
			//bool isTruncating = (flags & O_TRUNC) != 0;
			bool shouldReport = true;//!(info.lastDesiredAccess & AccessFlag_Write) || isTruncating;
			shouldReport = shouldReport && !keepInMemory;
			if (shouldReport)
			{
				char newFileName[1024];
				Rpc_CreateFileW(prep.fileName, prep.fileNameKey, u8(prep.desiredAccess), newFileName, sizeof_array(newFileName), size, prep.closeId, false);
				info.name = g_memoryBlock.Strdup(newFileName);
				realFileName = info.name;
			}
			if (prep.desiredAccess == 0 || info.lastDesiredAccess == 0)
				realFileName = info.name;
			info.lastDesiredAccess |= prep.desiredAccess;
		}
		else
		{
			size = info.size;
			realFileName = info.name;
		}
	}
	fileTableLock.Leave();

	if (realFileName[0] == '$')
	{
		if (realFileName[1] == 'd') // This is a directory and we will need to fake it locally..
		{
			prep.action = SharedOpenAction::DirectoryFd;
			return;
		}
		DEBUG_LOG_DETOURED(funcName, "FAILED %s (%s)", prep.fileName.data, realFileName);
		UBA_ASSERTF(false, "unsupported filename %s", realFileName);
		prep.action = SharedOpenAction::UnsupportedFail;
		return;
	}

	if (IsMemoryHandle(realFileName))
	{
		DEBUG_LOG_DETOURED(funcName, "FAILED %s (%s)", prep.fileName.data, realFileName);
		UBA_ASSERTF(false, "^ filenames not implemented");
		prep.action = SharedOpenAction::UnsupportedFail;
		return;
	}

	if (keepInMemory)
	{
		DEBUG_LOG_DETOURED(funcName, "FAILED %s (%s)", prep.fileName.data, realFileName);
		UBA_ASSERTF(false, "keepInMemory not implemented");
		prep.action = SharedOpenAction::UnsupportedFail;
		return;
	}

	const char* tempFileName = realFileName;
	if (tempFileName[0] == '#')
		tempFileName = prep.fileName.data;
	else
		tempFileName = info.name;

	prep.trueFlags = flags;
	int trueMode = mode;

	auto readDataFromOldFile = [&]()
		{
			DEBUG_LOG_DETOURED(funcName, "INTERNAL READ FOR MEMORYWRITE (%s) (%s)", prep.fileName.data, OpenFlagsToString(prep.trueFlags).data);
			UBA_ASSERT(!info.recursiveCall);
			FileInfo temp = info;
			info.recursiveCall = true;
			fileTableLock.Leave();

			int readFd = Shared_open(funcName, prep.fileName.data, O_RDONLY|O_NONBLOCK, mode, trueOpen);
			UBA_ASSERTF(readFd != -1, "%s", prep.fileName.data);
			prep.dataToWrite.resize(prep.entryInfo.size);
			int readRes = read(readFd, prep.dataToWrite.data(), prep.entryInfo.size);
			close(readFd);
			UBA_ASSERT(readRes == (int)prep.entryInfo.size);

			fileTableLock.Enter();
			info = temp;
		};

	// When running remote, if O_APPEND is set we need to copy the source file into the destination file since they are not the same files
	if (prep.fileExists && (flags & O_APPEND) && !prep.fileName.Equals(tempFileName))
	{
		// It might already have been created as a write file and in that case we don't need to copy old state over
		if (!prevFileName || !Equals(prevFileName, tempFileName))
		{
			readDataFromOldFile();
			// trueFlags &= ~O_APPEND; // O_APPEND actually gives different behavior on writes so we can't remove that flag
			prep.trueFlags |= O_CREAT;
			trueMode = prep.entryInfo.attributes;
		}
	}

	if (flags == O_RDWR && !prep.fileName.Equals(tempFileName))
	{
		UBA_ASSERT(prep.fileExists);
		if (!prevFileName || !Equals(prevFileName, tempFileName))
		{
			readDataFromOldFile();
			prep.trueFlags |= O_CREAT;
			trueMode = prep.entryInfo.attributes;
		}
	}

	// If we write a file that we know exists there might be a missing O_CREAT but on remote builds the output file actually doesn't exist
	// so we need to add the flag to make sure it is created. Note, this only works when O_TRUC is there, otherwise we need to download
	// the read-file, create the new file and write the read-file content into the write
	if (flags == (O_WRONLY|O_TRUNC) && !prep.fileName.Equals(tempFileName))
	{
		prep.trueFlags |= O_CREAT;
		if (trueMode == 0) // Caller did not plan on creating a file, let's create the default mode
			trueMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	}

	prep.tempFileName = tempFileName;
	prep.trueMode = trueMode;
	prep.isDirectoryEntry = IsDirectory(prep.entryInfo.attributes);
	prep.action = SharedOpenAction::NormalOpen;
}

template<typename TrueOpen>
int Shared_open(const char* funcName, const char* file, int flags, int mode, const TrueOpen& trueOpen)
{
	SharedOpenPrep prep;
	Shared_prepareOpen(funcName, file, flags, mode, trueOpen, prep);

	#if UBA_DEBUG_LOG_ENABLED
	const char* isWriteStr = prep.isWrite ? " WRITE" : ""; (void)isWriteStr;
	#endif

	switch (prep.action)
	{
	case SharedOpenAction::Error:
		errno = prep.err;
		return -1;

	case SharedOpenAction::NoDetourFallback:
	{
		TimerScope ts(g_kernelStats.getFileInfo);
		int res = trueOpen(file, flags, mode);
		ts.Leave();
		DEBUG_LOG_TRUE(funcName, "NODETOUR%s (%s) -> %i", isWriteStr, file, res);
		if (res != -1)
			TrackInput(prep.fileName);
		return res;
	}

	case SharedOpenAction::ProcFallback:
	{
		int res = trueOpen(file, flags, mode);
		DEBUG_LOG_TRUE(funcName, "NODETOUR (%s) -> %i", file, res);
		return res;
	}

	case SharedOpenAction::DirectoryFd:
		return CreateDirFd(prep, flags, funcName);

	case SharedOpenAction::UnsupportedFail:
		return -1;

	case SharedOpenAction::NormalOpen:
	{
		TimerScope ts(g_kernelStats.getFileInfo);
		int fd = trueOpen(prep.tempFileName, prep.trueFlags, prep.trueMode);
		ts.Leave();

		DEBUG_LOG_TRUE(funcName, "%s%s (%s) %s %i -> %i (%s)", file, isWriteStr, prep.tempFileName, OpenFlagsToString(prep.trueFlags).data, prep.trueMode, fd, StrError(fd, errno));
		if (fd == -1)
		{
			// TODO: Do we need to report failure if closeId is set?
			return fd;
		}

		if (auto toWrite = prep.dataToWrite.size())
		{
			int wrres = write(fd, prep.dataToWrite.data(), toWrite);
			UBA_ASSERT(wrres == (int)toWrite);
		}

		TrackInput(prep.fileName);

		CreateFileTableEntry(fd, prep, prep.isDirectoryEntry, funcName, prep.tempFileName);

		if (prep.isWrite)
			Rpc_RegisterFileForWrite(prep.fileName, prep.fileNameKey);

		return fd;
	}
	}
	return -1;
}

template<typename TrueOpen>
FILE* Shared_fopen(const char* funcName, const char* path, const char* mode, const char* trueOpenName, const TrueOpen& trueOpen)
{
	bool p = strchr(mode, '+');
	//bool b = strchr(mode, 'b');
	//bool t = strchr(mode, 't');

	int flags = 0;
	if (strchr(mode, 'r'))
	{
		if (p)
			flags = O_RDWR;
		else
			flags = O_RDONLY | O_NONBLOCK;
	}
	else if (strchr(mode, 'w'))
	{
		flags = O_CREAT | O_TRUNC;
		if (p)
			flags |= O_RDWR;
		else
			flags |= O_WRONLY;
	}
	else if (strchr(mode, 'a'))
	{
		flags = O_CREAT | O_APPEND;
		if (p)
			flags |= O_RDWR;
		else
			flags |= O_WRONLY;
	}

	int openMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	int fd = Shared_open(trueOpenName, path, flags, openMode, trueOpen);
	if (fd == -1)
	{
		DEBUG_LOG_DETOURED(funcName, "(%s) -> FAILED", path);
		return nullptr;
	}


	TimerScope ts(g_kernelStats.createFile);
	FILE* res = TRUE_WRAPPER(fdopen)(fd, mode);
	ts.Leave();

	DEBUG_LOG_TRUE(funcName, "%i (%s %s) -> %p", fd, path, mode, res);
	return res;
}

template<typename TrueClose>
int Shared_close(const char* funcName, int fd, const TrueClose& trueClose)
{
	auto runClose = [&]()
		{
			TimerScope ts(g_kernelStats.closeFile);
			int res = trueClose();
			DEBUG_LOG_TRUE(funcName, "(%i) -> %i (%s)", fd, res, StrError(res, errno));
			return res;
		};

	if (!g_isDetouring)
		return runClose();

	SCOPED_WRITE_LOCK(g_fileHandlesLock, lock);
	auto findIt = g_fileHandles.find(fd);
	if (findIt == g_fileHandles.end())
		return runClose();

	DetouredHandle& h = findIt->second;
	FileObject* fo = h.fileObject;
	UBA_ASSERTF(fo->refCount >= 1, "FileObject needs to have ref count when closed");
	g_fileHandles.erase(findIt);
	lock.Leave();

	if (--fo->refCount)
		return runClose();

	auto dg = MakeGuard([fo]() { delete fo->dirInfo; delete fo; });

	// Silent closeId=0 on a write file means the staging file we created via
	// Rpc_CreateFileW never gets finalized by the agent — the build sees a
	// truncated/stale output. If you hit this, track back to the Shared_open /
	// Shared_freopen path that produced this FileObject: either the write
	// branch in Shared_prepareOpen decided `shouldReport=false` (already-open
	// writer without O_TRUNC), or the caller used a path that never called
	// Rpc_CreateFileW at all.
	if (fo->fileInfo)
	{
		UBA_ASSERTF(!(fo->desiredAccess & AccessFlag_Write) || fo->closeId != 0,
			"%s: write-access fd=%i reached refCount=0 with closeId=0 — Rpc_UpdateCloseHandle will be skipped for %s",
			funcName, fd, fo->fileInfo->name ? fo->fileInfo->name : "<null>");
	}

	if (!fo->closeId)
		return runClose();

	SharedMemoryHandle memoryHandle;
	u64 fileSize = InvalidValue;
	u64 lastWritten = 0;
	u32 attributes = 0;

	FileInfo& fi = *fo->fileInfo;
	const tchar* path = fi.name;

	fi.created = true; // TODO: Should be set earlier

	// Dup the fd before closing so we can measure the true file size AFTER fclose
	// flushes any stdio buffers. Measuring via lseek before fclose misses any bytes
	// still sitting in the stdio buffer (e.g. when 'as' uses fdopen on top of open64).
	int dupFd = TRUE_WRAPPER(dup)(fd);
	UBA_ASSERT(dupFd != -1);
	int res = trueClose();
	int error = errno;

	struct stat attr;
	int res2 = TRUE_WRAPPER(fstat)(dupFd, &attr);
	UBA_ASSERT(res2 == 0);(void)res2; 
	fileSize = fi.size = attr.st_size;
	lastWritten = FromTimeSpec(attr.st_mtimespec);
	attributes = attr.st_mode;
	//fi.size = TRUE_WRAPPER(lseek)(dupFd, 0, SEEK_END);
	//UBA_ASSERT(fi.size != (off_t)-1);
	TRUE_WRAPPER(close)(dupFd);
	fileSize = fi.size;

	DEBUG_LOG_TRUE(funcName, "(%i) Size: %llu -> %i (%s)", fd, fileSize, res, StrError(res, error));

	Rpc_UpdateCloseHandle(path, fo->closeId, fo->deleteOnClose, fo->newName.c_str(), memoryHandle, fileSize, lastWritten, attributes, true);
	errno = error;
	return res;
}

// Shared_freopen implements the bookkeeping half of POSIX freopen.
//
// freopen preserves the caller's FILE* identity while closing the old fd and
// opening a new one — we cannot decompose it as close+fopen without mutating
// the pointer. So we reuse Shared_prepareOpen's decision tree (path
// normalization, dirtable checks, remote staging-path translation via
// Rpc_CreateFileW, preload for O_APPEND/O_RDWR) and then dispatch to the
// real freopen with prep.tempFileName — the same translated path Shared_open
// would have used. This is what makes freopen work remotely.
//
// Sequence:
//   1. Flush the stream and retire the old fd's UBA bookkeeping via
//      Shared_close with a no-op trueClose — glibc's freopen performs the
//      actual close of oldFd internally.
//   2. Run Shared_prepareOpen on (file, flags-derived-from-mode).
//   3. Dispatch on prep.action:
//        Error        -> errno + nullptr
//        NoDetour     -> trueFreopen(file, mode, stream), TrackInput
//        Proc         -> trueFreopen(file, mode, stream)
//        DirectoryFd  -> EISDIR + nullptr (freopen cannot yield a dir stream)
//        Unsupported  -> nullptr (UBA_ASSERTF already fired in prepare)
//        NormalOpen   -> trueFreopen(prep.tempFileName, mode, stream),
//                        write preload to raw fd (matching Shared_open),
//                        TrackInput, CreateFileTableEntry on fileno(result),
//                        Rpc_RegisterFileForWrite if isWrite.
template<typename TrueFreopen, typename TrueOpen>
FILE* Shared_freopen(const char* funcName, const char* file, const char* mode, FILE* stream, const TrueFreopen& trueFreopen, const TrueOpen& trueOpen)
{
	// Derive kernel-flag intent from the mode string (same mapping as Shared_fopen).
	bool p = strchr(mode, '+');
	int flags = 0;
	if (strchr(mode, 'r'))
		flags = p ? O_RDWR : (O_RDONLY | O_NONBLOCK);
	else if (strchr(mode, 'w'))
		flags = O_CREAT | O_TRUNC | (p ? O_RDWR : O_WRONLY);
	else if (strchr(mode, 'a'))
		flags = O_CREAT | O_APPEND | (p ? O_RDWR : O_WRONLY);

	int openMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	// Flush stdio buffers and retire old fd's UBA bookkeeping before the real
	// freopen closes it. Shared_close with a no-op trueClose — glibc will
	// close oldFd itself once we call trueFreopen below.
	int oldFd = fileno(stream);
	fflush(stream);
	Shared_close(funcName, oldFd, []() { return 0; });

	SharedOpenPrep prep;
	Shared_prepareOpen(funcName, file, flags, openMode, trueOpen, prep);

	#if UBA_DEBUG_LOG_ENABLED
	const char* isWriteStr = prep.isWrite ? " WRITE" : ""; (void)isWriteStr;
	#endif

	switch (prep.action)
	{
	case SharedOpenAction::Error:
		errno = prep.err;
		return nullptr;

	case SharedOpenAction::NoDetourFallback:
	{
		TimerScope ts(g_kernelStats.createFile);
		FILE* res = trueFreopen(file, mode, stream);
		ts.Leave();
		DEBUG_LOG_TRUE(funcName, "NODETOUR%s (%s, %s) -> %p", isWriteStr, file, mode, res);
		if (res)
			TrackInput(prep.fileName);
		return res;
	}

	case SharedOpenAction::ProcFallback:
	{
		FILE* res = trueFreopen(file, mode, stream);
		DEBUG_LOG_TRUE(funcName, "NODETOUR (%s, %s) -> %p", file, mode, res);
		return res;
	}

	case SharedOpenAction::DirectoryFd:
	{
		errno = EISDIR;
		DEBUG_LOG_DETOURED(funcName, "(DIR-REJECT) %s -> nullptr (EISDIR)", prep.fileName.data);
		return nullptr;
	}

	case SharedOpenAction::UnsupportedFail:
		return nullptr;

	case SharedOpenAction::NormalOpen:
	{
		// Writes MUST carry a closeId — without it Shared_close silently skips
		// Rpc_UpdateCloseHandle and the staging file never gets finalized (content
		// appears truncated or empty from the build's point of view).
		UBA_ASSERTF(!prep.isWrite || prep.closeId != 0,
			"%s: NormalOpen write path produced closeId=0 for %s (mode=%s) — Rpc_UpdateCloseHandle will be skipped",
			funcName, prep.fileName.data, mode);

		TimerScope ts(g_kernelStats.createFile);
		FILE* res = trueFreopen(prep.tempFileName, mode, stream);
		ts.Leave();

		if (!res)
		{
			DEBUG_LOG_TRUE(funcName, "%s%s (%s) %s -> nullptr (%s)", file, isWriteStr, prep.tempFileName, mode, strerror(errno));
			return nullptr;
		}

		int fd = fileno(res);
		DEBUG_LOG_TRUE(funcName, "%s%s (%s) %s -> %p fd=%i closeId=%i", file, isWriteStr, prep.tempFileName, mode, res, fd, prep.closeId);

		// glibc's freopen is specified to preserve the FILE* identity; Linux
		// typically preserves the fd number too (via dup2 internally). If glibc's
		// internal dup2/close went through our detours we'd have a ghost entry
		// at `fd` already — CreateFileTableEntry's collision assert will catch it,
		// but we verify up front so the message points at freopen.
		{
			SCOPED_READ_LOCK(g_fileHandlesLock, lock);
			UBA_ASSERTF(g_fileHandles.find(fd) == g_fileHandles.end(),
				"%s: fd=%i already has a g_fileHandles entry after trueFreopen (%s) — "
				"glibc's internal open/dup2/close went through detours and double-registered",
				funcName, fd, prep.tempFileName);
		}

		if (auto toWrite = prep.dataToWrite.size())
		{
			int wrres = write(fd, prep.dataToWrite.data(), toWrite);
			UBA_ASSERT(wrres == (int)toWrite);
		}

		TrackInput(prep.fileName);
		CreateFileTableEntry(fd, prep, false, funcName, prep.tempFileName);
		if (prep.isWrite)
			Rpc_RegisterFileForWrite(prep.fileName, prep.fileNameKey);

		return res;
	}
	}
	return nullptr;
}

template<typename True_fstat>
int Shared_fstat(const char* funcName, int fd, struct stat* attr, const True_fstat& trueFstat)
{
	//if (!g_isDetouring || t_disallowDetour)
	//	return trueFstat(fd, attr);

	SCOPED_READ_LOCK(g_fileHandlesLock, lock);
	auto findIt = g_fileHandles.find(fd);
	if (findIt == g_fileHandles.end())
	{
		TimerScope ts(g_kernelStats.getFileInfo);
		int res = trueFstat(fd, attr);
		ts.Leave();
		DEBUG_LOG_TRUE(funcName, "(%i) NODETOUR (size: %llu) -> %i (%s)", fd, attr->st_size, res, StrError(res, errno));
		return res;
	}

	FileObject& fo = *findIt->second.fileObject;
	FileInfo& fi = *fo.fileInfo;

	if (fo.desiredAccess & AccessFlag_Write)
	{
		TimerScope ts(g_kernelStats.getFileInfo);
		int res = trueFstat(fd, attr);
		ts.Leave();
		DEBUG_LOG_TRUE(funcName, "(%i) AFW (%s size: %llu) -> %i (%s)", fd, fi.originalName, attr->st_size, res, StrError(res, errno));
		return res;
	}

	FileAttributes fileAttr = {};
	const char* realName = Shared_GetFileAttributes(fileAttr, ToView(fi.originalName));

	int res = fileAttr.lastError == 0 ? 0 : -1;

	DEBUG_LOG_DETOURED(funcName, "(%i) (%s size: %llu id: %llu dev: %u) -> %i (%s)", fd, fi.originalName, fileAttr.data.st_size, fileAttr.data.st_ino, fileAttr.data.st_dev, res, StrError(res, fileAttr.lastError));

	errno = fileAttr.lastError;
	if (res == 0)
		memcpy(attr, &fileAttr.data, sizeof(struct stat));

	#if UBA_DEBUG_VALIDATE
	if (!g_runningRemote && !IsVfsEnabled())
	{
		struct stat attr2;
		int res2 = trueFstat(fd, &attr2);
		UBA_ASSERTF(res == res2, "fstat: return value differs for %s (%i vs %i)", fi.originalName, res, res2);
		if (res != -1)
		{
			bool isDir = S_ISDIR(attr->st_mode);
			UBA_ASSERTF(isDir == S_ISDIR(attr2.st_mode), "fstat: isDir not matching");
			//UBA_ASSERT(attr->st_mode == attr2.st_mode);
			//UBA_ASSERT(attr->st_dev == attr2.st_dev)
			UBA_ASSERTF(attr->st_ino == attr2.st_ino, "fstat: st_ino mismatch for %s (%llu vs %llu)", fi.originalName, attr->st_ino, attr2.st_ino);
			UBA_ASSERTF(isDir || attr->st_size == attr2.st_size, "fstat: size not matching");
			UBA_ASSERTF(isDir || FromTimeSpec(attr->st_mtimespec) == FromTimeSpec(attr2.st_mtimespec), "fstat: st_mtim mismatch for %s (%llu vs %llu)", fi.originalName, FromTimeSpec(attr->st_mtimespec), FromTimeSpec(attr2.st_mtimespec));
		}
		else
		{
			UBA_ASSERTF(fileAttr.lastError == errno, "fstat: error not matching");
		}
	}
	#endif

	return res;
}

template<typename True_stat>
int Shared_stat(const char* funcName, const char* file, struct stat* attr, const True_stat& trueStat)
{
	StringBuffer<> fixedFile;
	if (!FixPath(fixedFile, file) || fixedFile.Equals("/") || !CanDetour2(fixedFile))
	{
		TimerScope ts(g_kernelStats.getFileInfo);
		int res = trueStat(file, attr);
		return res;
	}

	UBA_ASSERTF(fixedFile.count, "FixPath failed with %s", file);

	if (g_runningRemote && fixedFile.StartsWith(g_exeDir.data))
	{
		StringBuffer<> temp;
		temp.Append(g_virtualApplicationDir).Append(fixedFile.data + g_exeDir.count);
		fixedFile.Clear().Append(temp);
	}

	DevirtualizePath(fixedFile);

	FileAttributes fileAttr;
	const char* realName = Shared_GetFileAttributes(fileAttr, fixedFile);

	int res = fileAttr.lastError == 0 ? 0 : -1;

	DEBUG_LOG_DETOURED(funcName, "%s (%s size: %llu id: %llu dev: %u mode: %u)-> %i (%s)", file, realName, fileAttr.data.st_size, fileAttr.data.st_ino, fileAttr.data.st_dev, fileAttr.data.st_mode, res, StrError(res, fileAttr.lastError));

	if (res == 0) // If success and original path had ".." in path, we need to verify that the path that leads to the ".." actually exists.
	{
		// TODO: handle multiple ".." spread out in the path
		const tchar* dotdot;
		if (Contains(file, "..", false, &dotdot))
		{
			StringBuffer<> tempPath;
			tempPath.Append(file, dotdot - file);
			struct stat tempAttr;
			int tempRes = Shared_stat("stat(dotdot)", tempPath.data, &tempAttr, trueStat);
			if (tempRes != 0)
				return tempRes;
		}

	}

	errno = fileAttr.lastError;

	if (res == 0) // This check is important.. dont write to attr unless success. (some programs rely on that)
		memcpy(attr, &fileAttr.data, sizeof(fileAttr.data));

	#if UBA_DEBUG_VALIDATE
	if (!g_runningRemote && !IsVfsEnabled() && *file)
	{
		struct stat attr2;
		int res2 = trueStat(file, &attr2);
		UBA_ASSERTF(res == res2, "%s: return value differs for %s (cached %i vs actual %i) [fixed: %s]", funcName, file, res, res2, fixedFile.data, StrError(res2, errno));
		if (res == 0)
		{
			bool isDir = S_ISDIR(attr->st_mode);
			UBA_ASSERT(isDir == S_ISDIR(attr2.st_mode));
			//UBA_ASSERT(attr->st_mode == attr2.st_mode);
			//UBA_ASSERT(attr->st_dev == attr2.st_dev)
			UBA_ASSERTF(attr->st_ino == attr2.st_ino, "stat: st_ino mismatch for %s (%llu vs %llu)", file, attr->st_ino, attr2.st_ino);
			UBA_ASSERT(isDir || attr->st_size == attr2.st_size);
			UBA_ASSERTF(isDir || FromTimeSpec(attr->st_mtimespec) == FromTimeSpec(attr2.st_mtimespec), "stat: st_mtim mismatch for %s (%llu vs %llu)", file, FromTimeSpec(attr->st_mtimespec), FromTimeSpec(attr2.st_mtimespec));
		}
		else
		{
			UBA_ASSERTF(fileAttr.lastError == errno || ((fileAttr.lastError == ENOTDIR || fileAttr.lastError == ENOENT) && (errno == ENOTDIR || errno == ENOENT))
				, "Detoured stat returned a different error. Returned %i (%s) but should return %i (%s)", fileAttr.lastError, strerror(fileAttr.lastError), errno, strerror(errno));
		}
	}
	#endif
	return res;
}

const char* ResolvePath(int dirfd, const char* pathname, StringBuffer<>& temp)
{
	if (pathname && *pathname == '/')
		return pathname;
	if (dirfd == AT_FDCWD)
		return pathname;
	SCOPED_WRITE_LOCK(g_fileHandlesLock, lock);
	auto findIt = g_fileHandles.find(dirfd);
	if (findIt == g_fileHandles.end())
		return nullptr;
	DetouredHandle& h = findIt->second;
	lock.Leave();
	FileObject* fo = h.fileObject;
	FileInfo& info = *fo->fileInfo;
	temp.Append(info.originalName);
	if (pathname)
		temp.EnsureEndsWithSlash().Append(pathname);
	return temp.data;
}

#if PLATFORM_LINUX
int Shared_statx(const char* funcName, int dirfd, const char* pathname, int flags, unsigned int mask, struct statx* statxbuf)
{
	StringBuffer<> temp;
	const char* resolvedPath = ResolvePath(dirfd, pathname, temp);
	if (!resolvedPath)
	{
		int res = (int)TRUE_WRAPPER(syscall)(SYS_statx, dirfd, pathname, flags, mask, statxbuf);
		DEBUG_LOG_TRUE("syscall.statx", "(NODETOUR) %s -> %s", pathname, res);
		return res;
	}
	bool trueStatCalled = false;
	struct stat st;
	int res = Shared_stat(funcName, resolvedPath, &st, [&](const char* path, struct stat*) -> int
		{
			trueStatCalled = true;
			return (int)TRUE_WRAPPER(syscall)(SYS_statx, AT_FDCWD, path, flags, mask, statxbuf);
		});
	if (trueStatCalled)
		return res;
	if (res != 0)
		return res;

	memset(statxbuf, 0, sizeof(*statxbuf));
	statxbuf->stx_mask       = STATX_BASIC_STATS;
	statxbuf->stx_blksize    = (u32)st.st_blksize;
	statxbuf->stx_nlink      = (u32)st.st_nlink;
	statxbuf->stx_uid        = st.st_uid;
	statxbuf->stx_gid        = st.st_gid;
	statxbuf->stx_mode       = (u16)st.st_mode;
	statxbuf->stx_ino        = st.st_ino;
	statxbuf->stx_size       = (u64)st.st_size;
	statxbuf->stx_blocks     = (u64)st.st_blocks;
	statxbuf->stx_atime      = { st.st_atim.tv_sec, (u32)st.st_atim.tv_nsec };
	statxbuf->stx_mtime      = { st.st_mtim.tv_sec, (u32)st.st_mtim.tv_nsec };
	statxbuf->stx_ctime      = { st.st_ctim.tv_sec, (u32)st.st_ctim.tv_nsec };
	statxbuf->stx_rdev_major = major(st.st_rdev);
	statxbuf->stx_rdev_minor = minor(st.st_rdev);
	statxbuf->stx_dev_major  = major(st.st_dev);
	statxbuf->stx_dev_minor  = minor(st.st_dev);
	return res;
}
#endif

//template<typename True_stat>
// strict=true honours R_OK/W_OK/X_OK against the directory-table mode bits as POSIX
// access(2)/eaccess(3) require. Used by the eaccess wrapper because callers (notably
// bash's PATH search) rely on X_OK actually rejecting non-executable files.
int Shared_access(const char* funcName, const char* pathname, int mode, bool strict = false)
{
	StringView original(ToView(pathname));

	StringBuffer<> fixedPath;
	if (!FixPath(fixedPath, original) || fixedPath.StartsWith("/proc") || !CanDetour2(fixedPath))
	{
		TimerScope ts(g_kernelStats.getFileInfo);
		auto res = TRUE_WRAPPER(access)(original.data, mode);
		ts.Leave();
		DEBUG_LOG_TRUE(funcName, "%s %i -> %i (%s)", original.data, mode, res, StrError(res, errno));
		return res;
	}

	bool checkIfDir = false;
	StringBuffer<> temp;
	if (g_runningRemote && fixedPath.StartsWith(g_exeDir.data))
	{
		temp.Append(g_virtualApplicationDir).Append(fixedPath.data + g_exeDir.count);

		if (temp.count == g_virtualApplicationDir.count) // Remove last slash from temp if this is a access call for the actual applicaiton directory
		{
			checkIfDir = true;
			temp.Resize(temp.count - 1);
		}
		fixedPath.Clear().Append(temp);
	}

	DevirtualizePath(fixedPath);

	if (!CanDetour(original))
	{
		TimerScope ts(g_kernelStats.getFileInfo);
		auto res = TRUE_WRAPPER(access)(original.data, mode);
		ts.Leave();
		DEBUG_LOG_TRUE(funcName, "%s %i -> %i (%s)", original.data, mode, res, StrError(res, errno));
		return res;
	}

	FileAttributes attr;
	const char* realName = Shared_GetFileAttributes(attr, fixedPath, checkIfDir);

	int res = attr.lastError == 0 ? 0 : -1;

	// Strict mode (eaccess): apply each requested permission bit against st_mode the way
	// POSIX access(2) does. UBA's directory table fills st_uid/st_gid with the current
	// process's uid/gid, so the owner bits are what apply for every check (matches what
	// a real eaccess would compute when the caller owns the file). Skip the check for
	// directories — caller wants traverse permission, which we always grant for entries
	// in the table.
	if (strict && res == 0 && S_ISREG(attr.data.st_mode))
	{
		const u32 m = attr.data.st_mode;
		bool deny = false;
		if ((mode & X_OK) && !(m & (S_IXUSR | S_IXGRP | S_IXOTH))) deny = true;
		if ((mode & R_OK) && !(m & (S_IRUSR | S_IRGRP | S_IROTH))) deny = true;
		if ((mode & W_OK) && !(m & (S_IWUSR | S_IWGRP | S_IWOTH))) deny = true;
		if (deny)
		{
			errno = EACCES;
			DEBUG_LOG_DETOURED(funcName, "%s %i (%s mode: %o) -> -1 (EACCES)", original.data, mode, realName, m);
			return -1;
		}
	}

	DEBUG_LOG_DETOURED(funcName, "%s %i (%s mode: %o) -> %i %s", original.data, mode, realName, attr.data.st_mode, res, StrError(res, attr.lastError));

	#if UBA_DEBUG_VALIDATE
	if (!g_runningRemote)
	{
		//errno = 0;
		auto res2 = TRUE_WRAPPER(access)(realName, mode);
		//int eo = errno;
		
		DEBUG_LOG_DETOURED(funcName, "%s %i (%s) -> %i %s", original.data, mode, realName, res, StrError(res, attr.lastError));
		
		UBA_ASSERTF(res2 == res, "MISMATCH OF RESULTS for %s - %i %i (err = %s) (exedir %s)", realName, res2, res, StrError(res, attr.lastError), g_exeDir.data);
		//UBA_ASSERTF(eo == attr.lastError, "MISMATCH OF ERRORS for %s - %i %i", realName, eo, attr.lastError);
	}
	#endif
	
	errno = attr.lastError;
	return res;
}

void DuplicateFd(int oldFd, int newFd)
{
	SCOPED_WRITE_LOCK(g_fileHandlesLock, lock);
	auto findIt = g_fileHandles.find(oldFd);
	if (findIt == g_fileHandles.end())
		return;
	DetouredHandle& h = findIt->second;
	FileObject* fo = h.fileObject;
	++fo->refCount;
	g_fileHandles[newFd].fileObject = fo;
}

// Detoured functions
#if PLATFORM_MAC
//extern "C" {
//char* realpath$DARWIN_EXTSN(const char* path, char* resolved_path);
//}
UBA_EXPORT int UBA_WRAPPER(_NSGetExecutablePath)(char* buf, uint32_t* bufsize)
{
	if (!g_isDetouring)
	{
		DEBUG_LOG_TRUE("NSGetExecutablePath", "");
		return TRUE_WRAPPER(_NSGetExecutablePath)(buf, bufsize);
	}
	if (bufsize == nullptr)
		return -1;
	const uint32_t requiredBufsize = g_virtualApplication.count + 1;
	const uint32_t initialBufsize = *bufsize;
	*bufsize = requiredBufsize;
	if (initialBufsize < requiredBufsize)
		return -1;
	if (buf != nullptr)
		memcpy(buf, g_virtualApplication.data, requiredBufsize);
	DEBUG_LOG_DETOURED("NSGetExecutablePath", "%s", buf);
	return 0;
}

//UBA_EXPORT int UBA_WRAPPER(_NSGetEnviron)(char* buf, uint32_t* bufsize)
//{
//int res = TRUE_WRAPPER(_NSGetEnviron)(buf, bufsize);
//printf("%s for %s\n", __func__, buf);
//return TRUE_WRAPPER(_NSGetEnviron)(buf, bufsize);
//}
//extern "C" char* UBA_WRAPPER(realpath$DARWIN_EXTSN)(const char* path, char* resolved_path)
//{
//printf(">>>>>> uba_realpathDARWIN: %s\n", path);
//char* res = TRUE_WRAPPER(realpath$DARWIN_EXTSN)(path, resolved_path);
//printf("<<<<< uba_realpathDARWIN: %s\n", strlen(resolved_path) > 0 ? resolved_path : "(NULL)");
//
//return res;
//}
#endif


UBA_EXPORT int UBA_WRAPPER(chdir)(const char* path)
{
	UBA_INIT_DETOUR(chdir, path);
	if (path == nullptr || *path == '\0')
	{
		errno = ENOENT;
		DEBUG_LOG_TRUE("chdir", "(BAD_PATH) -> -1");
		return -1;
	}
	size_t pathlen = strlen(path);
	if (pathlen >= g_virtualWorkingDir.capacity)
	{
		errno = ENAMETOOLONG;
		DEBUG_LOG_TRUE("chdir", "(TOO_LONG) %s -> -1", path);
		return -1;
	}
	memcpy(g_virtualWorkingDir.data, path, pathlen + 1);
	g_virtualWorkingDir.count = u32(pathlen);
	setenv("PWD", g_virtualWorkingDir.data, 1);
	g_virtualWorkingDir.EnsureEndsWithSlash();
	DEBUG_LOG_TRUE("chdir", "%s -> 0", path);
	return 0;
}

UBA_EXPORT int UBA_WRAPPER(fchdir)(int fd)
{
	UBA_INIT_DETOUR(fchdir, fd);
	UBA_ASSERTF(false, "fchdir not implemented");
	return TRUE_WRAPPER(fchdir)(fd);
}

UBA_EXPORT int UBA_WRAPPER(mkdir)(const char* path, mode_t mode)
{
	UBA_INIT_DETOUR(mkdir, path, mode);

	StringBuffer<> dirPath;
	if (!FixPath(dirPath, path) || !CanDetour2(dirPath) || (dirPath.count == 1 && dirPath[0] == '/'))
	{
		int res = TRUE_WRAPPER(mkdir)(path, mode);
		DEBUG_LOG_TRUE("mkdir", "%s -> %i", path, res);
		return res;
	}
	DevirtualizePath(dirPath);
	StringKey dirPathKey = ToFilenameKey(dirPath);

	DirTableOffset dirTableOffset;
	if (Rpc_GetEntryOffset(dirTableOffset, dirPathKey, dirPath, false))
	{
		errno = EEXIST;
		DEBUG_LOG_DETOURED("mkdir", "%s -> -1 (EEXIST)", dirPath.data);
		return -1;
	}

	const tchar* lastSeparator = dirPath.Last(PathSeparator);
	UBA_ASSERT(lastSeparator);
	StringView parentPath(dirPath.data, u32(lastSeparator - dirPath.data));
	if (!Rpc_GetEntryOffset(dirTableOffset, ToFilenameKey(parentPath), parentPath, true))
	{
		errno = ENOENT;
		DEBUG_LOG_DETOURED("mkdir", "%s -> -1 (ENOENT)", dirPath.data);
		return false;
	}

	RPC_MESSAGE(CreateDirectory, createFile);
	writer.WriteStringKey(dirPathKey);
	writer.WriteString(dirPath);
	BinaryReader reader = writer.Flush();
	DirTableSize directoryTableSize = FromU64(reader.ReadU64());
	RPC_MESSAGE_END

	g_directoryTable.ParseDirectoryTable(directoryTableSize);
	DEBUG_LOG_DETOURED("mkdir", "%s -> 0", path);
	return 0;
}

UBA_EXPORT int UBA_WRAPPER(rmdir)(const char* path)
{
	UBA_INIT_DETOUR(rmdir, path);

	StringBuffer<> pathName;
	if (!FixPath(pathName, path) || !CanDetour2(pathName))
	{
		int res = TRUE_WRAPPER(rmdir)(path);
		DEBUG_LOG_TRUE("rmdir", "%s -> %i", path, res);
		return res;
	}

	DevirtualizePath(pathName);

	StringKey pathNameKey = ToFilenameKey(pathName);

	RPC_MESSAGE(RemoveDirectory, deleteFile);
	writer.WriteStringKey(pathNameKey);
	writer.WriteString(pathName);
	BinaryReader reader = writer.Flush();
	DirTableSize directoryTableSize = FromU64(reader.ReadU64());
	RPC_MESSAGE_END

	g_directoryTable.ParseDirectoryTable(directoryTableSize);

	DEBUG_LOG_DETOURED("rmdir", "%s -> 0", path);
	return 0;
}

UBA_EXPORT int UBA_WRAPPER(chroot)(const char* path)
{
	UBA_INIT_DETOUR(chroot, path);
	UBA_ASSERTF(false, "chroot not implemented");
	return TRUE_WRAPPER(chroot)(path);
}

UBA_EXPORT char* UBA_WRAPPER(getcwd)(char* buf, size_t size)
{
	UBA_INIT_DETOUR(getcwd, buf, size);

	// g_virtualWorkingDir always ends with '/' (required by FixPath2), but
	// POSIX getcwd() must NOT return a trailing slash except for root "/".
	u32 len = g_virtualWorkingDir.count;
	if (len > 1 && g_virtualWorkingDir.data[len - 1] == '/')
		--len;

	if (size == 0)
	{
		if (buf == nullptr)
		{
			buf = (char*)malloc(len + 1);
			memcpy(buf, g_virtualWorkingDir.data, len);
			buf[len] = 0;
			DEBUG_LOG_DETOURED("getcwd", "(null) -> %s ", buf);
			return buf;
		}

		errno = EINVAL;
		DEBUG_LOG_DETOURED("getcwd", "(null) -> null (EINVAL)");
		return nullptr;
	}
	if (size < len + 1)
	{
		DEBUG_LOG_DETOURED("getcwd", "-> null (Buffer too small: %u)", u32(size));
		errno = ERANGE;
		return nullptr;
	}
	if (!buf)
		buf = (char*)malloc(size);

	memcpy(buf, g_virtualWorkingDir.data, len);
	buf[len] = 0;
	DEBUG_LOG_DETOURED("getcwd", "%s -> %p", buf, buf);
	return buf;
}

UBA_EXPORT char* UBA_WRAPPER(getenv)(const char* name)
{
	UBA_INIT_DETOUR(getenv, name);
	auto res = TRUE_WRAPPER(getenv)(name);
	DEBUG_LOG_TRUE("getenv", "(%s) -> %s", name, res ? res : "<null>");
	return res;
}

UBA_EXPORT int UBA_WRAPPER(setenv)(const char* name, const char* value, int replace)
{
	UBA_INIT_DETOUR(setenv, name, value, replace);
	auto res = TRUE_WRAPPER(setenv)(name, value, replace);
	DEBUG_LOG_TRUE("setenv", "(%s) -> %s (%i)", name, value, res);
	return res;
}

UBA_EXPORT int UBA_WRAPPER(unsetenv)(const char* name)
{
	UBA_INIT_DETOUR(unsetenv, name);
	auto res = TRUE_WRAPPER(unsetenv)(name);
	DEBUG_LOG_TRUE("unsetenv", "(%s) -> (%i)", name, res);
	return res;
}

UBA_EXPORT char* UBA_WRAPPER(realpath)(const char* path, char* resolved_path)
{
	UBA_INIT_DETOUR(realpath, path, resolved_path);

	if (!g_runningRemote && !IsVfsEnabled())
	{
		if (!resolved_path) // TODO: This is weird.. it seems like newly built uba uses old glibc?
			resolved_path  = (char*)malloc(PATH_MAX);
		auto res = TRUE_WRAPPER(realpath)(path, resolved_path);
		DEBUG_LOG_TRUE("realpath", "(%s) -> %s (%s)", path, res, StrError(res == 0 ? -1 : 0, errno));
		return res;
	}

	// TODO
	// TODO We know this might blow up... it doesn't really resolve any links
	// TODO
	StringBuffer<> fixedPath;
	FixPath(fixedPath, path);

	StringBuffer<> devirtualizedPath(fixedPath);
	DevirtualizePath(devirtualizedPath);

	FileAttributes fileAttr;
	Shared_GetFileAttributes(fileAttr, devirtualizedPath);

	if (!fileAttr.exists)
	{
		errno = ENOENT;
		DEBUG_LOG_DETOURED("realpath", "(%s) -> nullptr (ENOENT)", path);
		return nullptr;
	}

	if (!resolved_path)
		resolved_path = (char*)malloc(fixedPath.count + 1);
	memcpy(resolved_path, fixedPath.data, fixedPath.count + 1);
	DEBUG_LOG_DETOURED("realpath", "(%s) -> %s", path, resolved_path);
	return resolved_path;
}

UBA_EXPORT ssize_t UBA_WRAPPER(readlink)(const char* pathname, char* buf, size_t bufsiz)
{
	UBA_INIT_DETOUR(readlink, pathname, buf, bufsiz);

	// Beautiful hack. Some of our tools use je_malloc and dlsym do memory allocation so we end up in a deadlock when initializing detour (since detour use dlsym to figure out true function)
	if (!g_isDetouring && Equals(pathname, "/etc/je_malloc.conf"))
	{
		errno = ENOENT;
		return -1;
	}

	UBA_INIT_DETOUR(readlink, pathname, buf, bufsiz);

	if (Equals(pathname, "/proc/self/exe"))
	{
		UBA_ASSERTF(g_virtualApplication.count < bufsiz, "readLink: buffer size smaller than path not implemented");
		memcpy(buf, g_virtualApplication.data, g_virtualApplication.count + 1);
		DEBUG_LOG_DETOURED("readlink", "(%s) (%s) -> %u", pathname, buf, g_virtualApplication.count);
		return g_virtualApplication.count;
	}
	else if (StartsWith(pathname, "/proc/self/fd/"))
	{
		StringBuffer<16> fdStr;
		fdStr.Append(pathname + 14);
		u32 fd;
		if (!fdStr.Parse(fd))
			UBA_ASSERTF(false, "Failed to parse /proc/self/fd");
		SCOPED_READ_LOCK(g_fileHandlesLock, lock);
		auto findIt = g_fileHandles.find(fd);
		if (findIt != g_fileHandles.end())
		{
			DetouredHandle& h = findIt->second;
			FileObject* fo = h.fileObject;
			FileInfo& info = *fo->fileInfo;
			u32 len = TStrlen(info.originalName);
			UBA_ASSERTF(len < bufsiz, "buffer size is smaller than length of name");
			memcpy(buf, info.originalName, len + 1);
			DEBUG_LOG_DETOURED("readlink", "(%s) (%s) -> %u", pathname, buf, len);
			return len;
		}
	}
	else
	{
		UBA_ASSERTF(!StartsWith(pathname, "/UEVFS"), "Need to devirtualize %s", pathname);

		// If the path is managed by the VFS it is a regular file on the host (not a symlink).
		// Return EINVAL so callers know the file exists but is not a symlink, rather than
		// getting ENOENT from the OS (which has no physical copy of the file on the agent).
		// Without this, tools like rustc that probe their own binary path via readlink()
		// fall back to dladdr(), which returns a path with unresolved ".." components
		// (e.g. "bin/rustc/../lib/..."). That causes a wrong sysroot computation and an
		// incorrect --print target-libdir output.
		if (g_runningRemote || IsVfsEnabled())
		{
			StringBuffer<> fixedPath;
			if (FixPath(fixedPath, pathname) && CanDetour2(fixedPath))
			{
				StringBuffer<> devirtualizedPath(fixedPath);
				DevirtualizePath(devirtualizedPath);
				FileAttributes fileAttr;
				Shared_GetFileAttributes(fileAttr, devirtualizedPath);
				if (fileAttr.exists)
				{
					errno = EINVAL; // File exists in VFS as a regular file, not a symlink
					DEBUG_LOG_DETOURED("readlink", "(%s) -> -1 (EINVAL, VFS regular file not a symlink)", pathname);
					return -1;
				}
			}
		}
	}

	auto res = TRUE_WRAPPER(readlink)(pathname, buf, bufsiz);
	DEBUG_LOG_TRUE("readlink", "(%s) (%s) -> %lli (%s)", pathname, buf, res, StrError(int(res), errno));

	if (res && res < bufsiz && IsVfsEnabled())
	{
		StringBuffer<> temp(buf);
		UBA_ASSERT(!DevirtualizePath(temp));
	}

	return res;
}

UBA_EXPORT ssize_t UBA_WRAPPER(readlinkat)(int dirfd, const char* pathname, char* buf, size_t bufsiz)
{
	UBA_INIT_DETOUR(readlinkat, dirfd, pathname, buf, bufsiz);
	DEBUG_LOG_TRUE("readlinkat","(%s)", pathname);
	UBA_ASSERTF(false, "readlinkat not implemented");
	return TRUE_WRAPPER(readlinkat)(dirfd, pathname, buf, bufsiz);
}

UBA_EXPORT int UBA_WRAPPER(fcntl)(int fd, int cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	void* arg = va_arg(ap, void*);
	va_end(ap);
	UBA_INIT_DETOUR(fcntl, fd, cmd, arg);
	int res = TRUE_WRAPPER(fcntl)(fd, cmd, arg);
	if (res != -1 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC))
		DuplicateFd(fd, res);
	DEBUG_LOG_TRUE("fcntl", "(%i, %i) -> %i", fd, cmd, res);
	return res;
}

#if PLATFORM_LINUX

UBA_EXPORT int UBA_WRAPPER(fcntl64)(int fd, int cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	void* arg = va_arg(ap, void*);
	va_end(ap);
	UBA_INIT_DETOUR(fcntl64, fd, cmd, arg);
	int res = TRUE_WRAPPER(fcntl64)(fd, cmd, arg);
	if (res != -1 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC))
		DuplicateFd(fd, res);
	DEBUG_LOG_TRUE("fcntl64", "(%i, %i) -> %i", fd, cmd, res);
	return res;
}

UBA_EXPORT ssize_t UBA_WRAPPER(pread64)(int __fd, void * __buf, size_t __nbyte, off_t __offset)
{
	return TRUE_WRAPPER(pread64)(__fd, __buf, __nbyte, __offset);
}

UBA_EXPORT int UBA_WRAPPER(open64)(const char* file, int flags, ...)
{
	va_list args;
	va_start(args, flags);
	int mode = va_arg(args, int);
	va_end(args);
	UBA_INIT_DETOUR(open64, file, flags, mode);
	return Shared_open("open64", file, flags, mode, [](const char* realFile, int flags, int mode) { return TRUE_WRAPPER(open64)(realFile, flags, mode); });
}
UBA_EXPORT int UBA_WRAPPER(creat64)(const char *path, mode_t mode)
{
	UBA_INIT_DETOUR(creat64, path, mode);
	return Shared_open("creat64", path, O_WRONLY | O_CREAT | O_TRUNC, mode, [](const char* p, int f, int m) { return TRUE_WRAPPER(open64)(p, f, m); });
}
UBA_EXPORT char* UBA_WRAPPER(secure_getenv)(const char* name)
{
	UBA_INIT_DETOUR(secure_getenv, name);
	auto res = TRUE_WRAPPER(secure_getenv)(name);
	DEBUG_LOG_TRUE("secure_getenv", "(%s) -> %s", name, res ? res : "<null>");
	return res;
}

#endif

UBA_EXPORT int UBA_WRAPPER(open)(const char* file, int flags, ...)
{
	va_list args;
	va_start(args, flags);
	int mode = va_arg(args, int);
	va_end(args);
	UBA_INIT_DETOUR(open, file, flags, mode);
	return Shared_open("open", file, flags, mode, [](const char* realFile, int flags, int mode) { return TRUE_WRAPPER(open)(realFile, flags, mode); });
}

UBA_EXPORT int UBA_WRAPPER(openat)(int dirfd, const char* pathname, int flags, ...)
{
	va_list args;
	va_start(args, flags);
	int mode = va_arg(args, int);
	va_end(args);
	UBA_INIT_DETOUR(openat, dirfd, pathname, flags, mode);

	StringBuffer<> temp;
	if (const char* resolvedPath = ResolvePath(dirfd, pathname, temp))
		return Shared_open("openat", resolvedPath, flags, mode, [](const char* realFile, int flags, int mode) { return TRUE_WRAPPER(open)(realFile, flags, mode); });

	UBA_ASSERT(!g_runningRemote);
	
	int res = TRUE_WRAPPER(openat)(dirfd, pathname, flags, mode);
	DEBUG_LOG_TRUE("openat", "(NODETOUR) %s -> %i (%s)", pathname, res, StrError(res, errno));
	if (res)
		TrackInput(ToView(pathname));
	return res;
}

UBA_EXPORT int UBA_WRAPPER(creat)(const char *path, mode_t mode)
{
	UBA_INIT_DETOUR(creat, path, mode);
	return Shared_open("creat", path, O_WRONLY | O_CREAT | O_TRUNC, mode, [](const char* p, int f, int m) { return TRUE_WRAPPER(open)(p, f, m); });
}


#if UBA_DETOUR_DEBUG && PLATFORM_LINUX
UBA_EXPORT ssize_t UBA_WRAPPER(write)(int fd, const void* buf, size_t count)
{
	UBA_INIT_DETOUR(write, fd, buf, count);
	//if (isatty(fd)) // stdout and stderr
	//{
	//	Shared_WriteConsole((const char*)buf, count, fd == fileno(stderr));
	//	return count;
	//}
	DEBUG_LOG_TRUE("write", "(%i size: %llu)", fd, count);
	TimerScope ts(g_kernelStats.writeFile);
	return TRUE_WRAPPER(write)(fd, buf, count);
}

UBA_EXPORT size_t UBA_WRAPPER(fwrite)(const void* ptr, size_t size, size_t nitems, FILE* stream)
{
	UBA_INIT_DETOUR(fwrite, ptr, size, nitems, stream);
	DEBUG_LOG_TRUE("fwrite", "(size: %llu)", size*nitems);
	TimerScope ts(g_kernelStats.writeFile);
	return TRUE_WRAPPER(fwrite)(ptr, size, nitems, stream);
}
#endif

UBA_EXPORT int UBA_WRAPPER(dup)(int oldfd)
{
	UBA_INIT_DETOUR(dup, oldfd);
	auto res = TRUE_WRAPPER(dup)(oldfd);
	if (res != -1)
		DuplicateFd(oldfd, res);
	DEBUG_LOG_TRUE("dup", "(%i) -> %i", oldfd, res);
	return res;
}

UBA_EXPORT int UBA_WRAPPER(dup2)(int oldfd, int newfd)
{
	UBA_INIT_DETOUR(dup2, oldfd, newfd);
	auto res = TRUE_WRAPPER(dup2)(oldfd, newfd);
	if (res != -1)
		DuplicateFd(oldfd, newfd);
	DEBUG_LOG_TRUE("dup2", "(%i, %i) -> %i", oldfd, newfd, res);
	return res;
}

UBA_EXPORT int UBA_WRAPPER(close)(int fd)
{
	UBA_INIT_DETOUR(close, fd);
	return Shared_close("close", fd, [&]() { return TRUE_WRAPPER(close)(fd); });
}

#if PLATFORM_LINUX
UBA_EXPORT FILE* UBA_WRAPPER(fopen64)(const char* path, const char* mode)
{
	UBA_INIT_DETOUR(fopen64, path, mode);
	return Shared_fopen("fopen64", path, mode, "open64", [](const char* realFile, int flags, int openMode) { return TRUE_WRAPPER(open64)(realFile, flags, openMode); });
}
UBA_EXPORT FILE* UBA_WRAPPER(freopen64)(const char* filename, const char* mode, FILE* stream)
{
	UBA_INIT_DETOUR(freopen64, filename, mode, stream);

	// glibc extension: freopen64(NULL, mode, stream) only mutates the stream's mode.
	if (!filename)
	{
		DEBUG_LOG_TRUE("freopen64", "(null, %s, %p)", mode, stream);
		return TRUE_WRAPPER(freopen64)(filename, mode, stream);
	}

	return Shared_freopen("freopen64", filename, mode, stream,
		[](const char* p, const char* m, FILE* s) { return TRUE_WRAPPER(freopen64)(p, m, s); },
		[](const char* realFile, int flags, int openMode) { return TRUE_WRAPPER(open64)(realFile, flags, openMode); });
}
UBA_EXPORT int UBA_WRAPPER(fstat64)(int fd, struct stat64* buf)
{
	UBA_INIT_DETOUR(fstat64, fd, buf);
	return Shared_fstat("fstat64", fd, (struct stat*)buf, [](int fd, struct stat* buf) { return TRUE_WRAPPER(fstat64)(fd, (struct stat64*)buf); });
}
UBA_EXPORT int UBA_WRAPPER(__fxstat64)(int ver, int fd, struct stat64* attr)
{
	UBA_INIT_DETOUR(__fxstat64, ver, fd, attr);
	UBA_ASSERT(ver == _STAT_VER);
	return Shared_fstat("__fxstat64", fd, (struct stat*)attr, [](int fd, struct stat* buf) { return TRUE_WRAPPER(__fxstat64)(_STAT_VER, fd, (struct stat64*)buf); });
}
#endif

UBA_EXPORT FILE* UBA_WRAPPER(fopen)(const char* path, const char* mode)
{
	UBA_INIT_DETOUR(fopen, path, mode);
	return Shared_fopen("fopen", path, mode, "open", [](const char* realFile, int flags, int openMode) { return TRUE_WRAPPER(open)(realFile, flags, openMode); });
}

UBA_EXPORT FILE* UBA_WRAPPER(fdopen)(int fd, const char* mode)
{
	UBA_INIT_DETOUR(fdopen, fd, mode);
	DEBUG_LOG_TRUE("fdopen", "(%i)", fd);
	TimerScope ts(g_kernelStats.createFile);
	return TRUE_WRAPPER(fdopen)(fd, mode);
}

UBA_EXPORT FILE* UBA_WRAPPER(freopen)(const char* filename, const char* mode, FILE* stream)
{
	UBA_INIT_DETOUR(freopen, filename, mode, stream);

	// glibc extension: freopen(NULL, mode, stream) only mutates the stream's mode,
	// it does not close/reopen the fd. No tracking change required.
	if (!filename)
	{
		DEBUG_LOG_TRUE("freopen", "(null, %s, %p)", mode, stream);
		return TRUE_WRAPPER(freopen)(filename, mode, stream);
	}

	return Shared_freopen("freopen", filename, mode, stream,
		[](const char* p, const char* m, FILE* s) { return TRUE_WRAPPER(freopen)(p, m, s); },
		[](const char* realFile, int flags, int openMode) { return TRUE_WRAPPER(open)(realFile, flags, openMode); });
}

UBA_EXPORT int UBA_WRAPPER(fchmod)(int fd, mode_t mode)
{
	UBA_INIT_DETOUR(fchmod, fd, mode);
	TimerScope ts(g_kernelStats.setFileInfo);
	auto res = TRUE_WRAPPER(fchmod)(fd, mode);
	ts.Leave();
	DEBUG_LOG_TRUE("fchmod", "(%i) %i -> %i (%s)", fd, mode, res, StrError(res, errno));
	return res;
}

UBA_EXPORT int UBA_WRAPPER(fchmodat)(int dirfd, const char* pathname, mode_t mode, int flags)
{
	UBA_INIT_DETOUR(fchmodat, dirfd, pathname, mode, flags);
	UBA_ASSERT(dirfd == AT_FDCWD);
	StringBuffer<> fixedPath;
	FixPath(fixedPath, pathname);
	DevirtualizePath(fixedPath);

	TimerScope ts(g_kernelStats.setFileInfo);
	int res = TRUE_WRAPPER(fchmodat)(dirfd, fixedPath.data, mode, flags);
	ts.Leave();
	DEBUG_LOG_TRUE("fchmodat", "%i %s %i %i -> %i (%s)", dirfd, pathname, mode, flags, res, StrError(res, errno));
	return res;
}

//UBA_EXPORT size_t UBA_WRAPPER(fwrite)(const void* ptr, size_t size, size_t nitems, FILE* stream)
//{
//	UBA_INIT_DETOUR(fwrite, ptr, size, nitems, stream);
//	auto res = TRUE_WRAPPER(fwrite)(ptr, size, nitems, stream);
//	//if (stream != stdout)
//	//	DEBUG_LOG_TRUE("fwrite", "(%i) %i", fileno(stream), int(res));
//	return res;
//}

UBA_EXPORT int UBA_WRAPPER(fstat)(int fd, struct stat* buf)
{
	UBA_INIT_DETOUR(fstat, fd, buf);
	return Shared_fstat("fstat", fd, buf, [](int fd, struct stat* buf) { return TRUE_WRAPPER(fstat)(fd, buf); });
}

UBA_EXPORT int UBA_WRAPPER(fstatat)(int dirfd, const char* pathname, struct stat* statbuf, int flags)
{
	UBA_INIT_DETOUR(fstatat, dirfd, pathname, statbuf, flags);

	StringBuffer<> temp;
	if (const char* resolvedPath = ResolvePath(dirfd, pathname, temp))
		return Shared_stat("fstatat", resolvedPath, statbuf, [](const char* file, struct stat* attr) { return TRUE_WRAPPER(stat)(file, attr); });
	DEBUG_LOG_TRUE("fstatat", "%s", pathname);
	TimerScope ts(g_kernelStats.getFileInfo);
	return TRUE_WRAPPER(fstatat)(dirfd, pathname, statbuf, flags);
}

UBA_EXPORT int UBA_WRAPPER(futimens)(int fd, const struct timespec* times)
{
	UBA_INIT_DETOUR(futimens, fd, times);
	int res = TRUE_WRAPPER(futimens)(fd, times);
	DEBUG_LOG_TRUE("futimens", "(%i) -> %i (%s)", fd, res, StrError(0, errno));
	return res;
}

UBA_EXPORT int UBA_WRAPPER(fclose)(FILE* stream)
{
	UBA_INIT_DETOUR(fclose, stream);

	#define FCLOSE_STD(x) if (stream == x) { DEBUG_LOG_TRUE("fclose", "(" #x ")"); return TRUE_WRAPPER(fclose)(stream); }
	FCLOSE_STD(stdin)
	FCLOSE_STD(stdout)
	FCLOSE_STD(stderr)

	if (!stream)
	{
		int res = TRUE_WRAPPER(fclose)(stream);
		DEBUG_LOG_TRUE("fclose", "(null) -> %i (%s)", res, StrError(res, errno));
		return res;
	}

	return Shared_close("fclose", fileno(stream), [&]() { return TRUE_WRAPPER(fclose)(stream); });
}

UBA_EXPORT int UBA_WRAPPER(mkstemp)(char* templ)
{
	UBA_INIT_DETOUR(mkstemp, templ);

	static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	const uint64_t base = (sizeof(alphabet) - 1);

	DEBUG_LOG_TRUE("mkstemp", "%s", templ);

	auto len = strlen(templ);
	char* posToWrite = templ + len - 6;
	while (true)
	{
		Guid g;
		CreateGuid(g);
		u64 v = *(u64*)&g;
		for (size_t i = 0; i < 6; ++i)
		{
			posToWrite[i] = alphabet[v % base];
			v /= base;
		}

		int flags = O_RDWR | O_CREAT | O_EXCL;
		int fd = UBA_WRAPPER(open)(templ, flags, 0600);
		if (fd >= 0)
			return fd;
		if (errno != EEXIST)
			return -1;
	}
}

UBA_EXPORT DIR* UBA_WRAPPER(opendir)(const char* name)
{
	UBA_INIT_DETOUR(opendir, name);
	StringBuffer<> dirName;

	if (!FixPath(dirName, name) || !CanDetour2(dirName))
	{
		DIR* res = TRUE_WRAPPER(opendir)(dirName.data);
		DEBUG_LOG_TRUE("opendir", "(%s) -> %p", dirName.data, res);
		return res;
	}

	DevirtualizePath(dirName);

	StringBuffer<> forHash(dirName);
	if (forHash.count == 1)
		forHash.Resize(0);
	if (CaseInsensitiveFs)
		forHash.MakeLower();
	DirHash hash(forHash);

	SCOPED_WRITE_LOCK(g_directoryTable.m_lookupLock, lookLock);
	auto insres = g_directoryTable.m_lookup.try_emplace(hash.key, g_memoryBlock);
	DirectoryTable::Directory& dir = insres.first->second;
	if (insres.second)
		if (g_directoryTable.EntryExistsNoLock(hash.key, forHash) != DirectoryTable::Exists_No)
			Rpc_UpdateDirectory(hash.key, dirName.data, dirName.count, false);

	bool exists = false;
	DirTableOffset entryOffset;
	if (g_directoryTable.GetLatestOffset(entryOffset, dir))
	{
		DirectoryTable::EntryInformation entryInfo;
		g_directoryTable.GetEntryInformation(entryInfo, entryOffset);
		exists = entryInfo.attributes != 0;
	}

	if (!exists)
	{
		errno = ENOENT;
		DEBUG_LOG_DETOURED("opendir", "(%s) -> %p", dirName.data, nullptr);
		return nullptr;
	}

	g_directoryTable.PopulateDirectory(hash.open, dir);

	auto dirInfo = new DirInfo();
	
	SCOPED_READ_LOCK(dir.lock, lock);
	dirInfo->fileTableOffsets.resize(dir.files.size());
	u32 it = 0;
	for (auto& pair : dir.files)
		dirInfo->fileTableOffsets[it++] = pair.second;
	lock.Leave();
	
	DEBUG_LOG_DETOURED("opendir", "(%s) -> %p", dirName.data, dirInfo);

	return (DIR*)(u64(dirInfo) | 0x1000'0000'0000'0000);
}

UBA_EXPORT int UBA_WRAPPER(dirfd)(DIR* dirp)
{
	UBA_INIT_DETOUR(dirfd, dirp);

	if (IsDirInfo(dirp))
	{
		// Solution can be to use this and register it with file handles
		// open("/dev/null", O_RDONLY)

		// Virtual directories have no real file descriptor; return the standard error.
		// TODO: Might need to fix this
		DEBUG_LOG_DETOURED("dirfd", "(%p) -> -1 (EBADF)", dirp);
		errno = EBADF;
		return -1;
	}

	int res = TRUE_WRAPPER(dirfd)(dirp);
	DEBUG_LOG_TRUE("dirfd", "(%p) -> %i", dirp, res);
	return res;
}

dirent* Shared_readdir(const char* func, DIR* dirp)
{
	auto& dirInfo = *AsDirInfo(dirp);
	while (true)
	{
		++dirInfo.it;
		if (dirInfo.it >= dirInfo.fileTableOffsets.size())
		{
			DEBUG_LOG_DETOURED(func, "(%p) -> nullptr (found %u entries)", dirp, dirInfo.fileTableOffsets.size());
			return nullptr;
		}
		DirTableOffset fileTableOffset = dirInfo.fileTableOffsets[dirInfo.it];

		DirectoryTable::EntryInformation info;
		g_directoryTable.GetEntryInformation(info, fileTableOffset, dirInfo.ent.d_name, 256);
		if (info.attributes == 0) // File was deleted
			continue;

		auto nameLen = u16(strlen(dirInfo.ent.d_name));
		bool isDir = S_ISDIR(info.attributes);

		dirInfo.ent.d_ino = info.fileIndex;
		dirInfo.ent.d_reclen = offsetof(struct dirent, d_name) + nameLen + 1;
		dirInfo.ent.d_type = isDir ? DT_DIR : DT_REG;

		#if PLATFORM_LINUX
		dirInfo.ent.d_off = 0;
		#else
		dirInfo.ent.d_namlen = nameLen;
		#endif

		//DEBUG_LOG_DETOURED(func, "(%p) -> %p (%s)%s", dirp, &dirInfo.ent, dirInfo.ent.d_name, (isDir?" (Dir)":""));
		return &dirInfo.ent;
	}
}

UBA_EXPORT dirent* UBA_WRAPPER(readdir)(DIR* dirp)
{
	UBA_INIT_DETOUR(readdir, dirp);
	if (IsDirInfo(dirp))
		return Shared_readdir("readdir", dirp);
	auto res = TRUE_WRAPPER(readdir)(dirp);
	DEBUG_LOG_TRUE("readdir", "(%p) -> %p", dirp, res);
	return res;
}

UBA_EXPORT int UBA_WRAPPER(closedir)(DIR* dirp)
{
	UBA_INIT_DETOUR(closedir, dirp);

	if (IsDirInfo(dirp))
	{
		delete (DirInfo*)AsDirInfo(dirp);
		DEBUG_LOG_DETOURED("closedir", "(%p)", dirp);
		return 0;
	}

	DEBUG_LOG_TRUE("closedir", "(%p)", dirp);
	return TRUE_WRAPPER(closedir)(dirp);
}

template<typename Dirent, typename ReadDir>
int Shared_scandir(const char* func, const char* dirp, Dirent*** namelist, int (*filter)(const Dirent*), int (*compar)(const Dirent**, const Dirent**), ReadDir readDir)
{
	DIR* dir = UBA_WRAPPER(opendir)(dirp);
	if (!dir)
	{
		DEBUG_LOG_DETOURED(func, "(%s) -> -1 (%s)", dirp, strerror(errno));
		return -1;
	}

	Vector<Dirent*> entries;
	Dirent* ent;
	while ((ent = readDir(dir)) != nullptr)
	{
		if (filter && !filter(ent))
			continue;
		auto copy = (Dirent*)malloc(ent->d_reclen);
		memcpy(copy, ent, ent->d_reclen);
		entries.push_back(copy);
	}
	UBA_WRAPPER(closedir)(dir);

	if (compar && entries.size() > 1)
		qsort(entries.data(), entries.size(), sizeof(Dirent*), (int(*)(const void*, const void*))compar);

	*namelist = (Dirent**)malloc(entries.size() * sizeof(Dirent*));
	for (size_t i = 0; i < entries.size(); i++)
		(*namelist)[i] = entries[i];

	DEBUG_LOG_DETOURED(func, "(%s) -> %zu", dirp, entries.size());
	return (int)entries.size();
}

#if PLATFORM_LINUX

UBA_EXPORT dirent64* UBA_WRAPPER(readdir64)(DIR* dirp)
{
	UBA_INIT_DETOUR(readdir64, dirp);
	if (IsDirInfo(dirp))
		return (dirent64*)Shared_readdir("readdir64", dirp);
	auto res = TRUE_WRAPPER(readdir64)(dirp);
	DEBUG_LOG_TRUE("readdir64", "(%p) -> %p", dirp, res);
	return res;
}

UBA_EXPORT int UBA_WRAPPER(scandir64)(const char* dirp, dirent64*** namelist, int (*filter)(const dirent64*), int (*compar)(const dirent64**, const dirent64**))
{
	UBA_INIT_DETOUR(scandir64, dirp, namelist, filter, compar);
	return Shared_scandir("scandir64", dirp, namelist, filter, compar, [](DIR* d) { return UBA_WRAPPER(readdir64)(d); });
}

// Kernel's struct linux_dirent64 layout (matches man getdents64).
// Fixed header is 19 bytes; d_name is null-terminated and d_reclen is padded
// so the next entry starts at an 8-byte boundary (matching kernel behavior).
#pragma pack(push, 1)
struct UbaLinuxDirent64
{
	u64 d_ino;
	s64 d_off;
	u16 d_reclen;
	u8  d_type;
	char d_name[]; // null-terminated filename
};
#pragma pack(pop)

// Serves SYS_getdents64 for a fd produced by Shared_open's $d (placeholder dir) branch.
// Returns >0 = bytes written, 0 = end-of-stream, -1 = not a UBA-tracked dir fd (caller passes through to kernel).
// On "buffer too small for one entry", sets errno = EINVAL and returns -1 via the
// `*outErrno` out-param; caller (libc or Go) translates to its error convention.
long Shared_getdents64(const char* funcName, int fd, void* buf, unsigned int count, int* outErrno)
{
	DirInfo* dirInfo = nullptr;
	{
		SCOPED_READ_LOCK(g_fileHandlesLock, lock);
		auto it = g_fileHandles.find(fd);
		if (it == g_fileHandles.end())
			return -1; // not tracked: fall back to kernel
		FileObject* fo = it->second.fileObject;
		if (!fo || !fo->dirInfo)
			return -1;
		dirInfo = fo->dirInfo;
	}

	u8* const base = (u8*)buf;
	u8* out = base;
	u8* end = base + count;
	char nameBuf[256];

	while (dirInfo->it + 1 < (int)dirInfo->fileTableOffsets.size())
	{
		int next = dirInfo->it + 1;
		DirTableOffset off = dirInfo->fileTableOffsets[next];

		DirectoryTable::EntryInformation info;
		g_directoryTable.GetEntryInformation(info, off, nameBuf, sizeof(nameBuf));
		if (info.attributes == 0) // deleted
		{
			dirInfo->it = next;
			continue;
		}

		u16 nameLen = (u16)strlen(nameBuf);
		u16 fixed = (u16)offsetof(UbaLinuxDirent64, d_name);
		u16 reclen = (u16)((fixed + nameLen + 1 + 7) & ~7u);

		if ((u64)(out - base) + reclen > count)
		{
			if (out == base)
			{
				*outErrno = EINVAL; // buffer too small for even one entry
				DEBUG_LOG_DETOURED(funcName, "(fd=%i, count=%u) -> -1 (EINVAL: first entry needs %u bytes)", fd, count, reclen);
				return -1;
			}
			break; // return what we've filled so far; caller will re-issue
		}

		auto* e = (UbaLinuxDirent64*)out;
		e->d_ino = info.fileIndex ? info.fileIndex : 1;
		e->d_off = (s64)next + 1;
		e->d_reclen = reclen;
		e->d_type = S_ISDIR(info.attributes) ? DT_DIR : DT_REG;
		memcpy(e->d_name, nameBuf, nameLen);
		// Zero the tail (name null + alignment padding).
		memset(e->d_name + nameLen, 0, reclen - fixed - nameLen);

		out += reclen;
		dirInfo->it = next;
	}

	long written = (long)(out - base);
	*outErrno = 0;
	DEBUG_LOG_DETOURED(funcName, "(fd=%i, count=%u) -> %ld (pos=%i/%zu)", fd, count, written, dirInfo->it + 1, dirInfo->fileTableOffsets.size());
	return written;
}
#endif

UBA_EXPORT void UBA_WRAPPER(rewinddir)(DIR* dirp)
{
	UBA_INIT_DETOUR(rewinddir, dirp);
	if (IsDirInfo(dirp))
	{
		auto& dirInfo = *AsDirInfo(dirp);
		dirInfo.it = -1;
		DEBUG_LOG_DETOURED("rewinddir", "(%p)", dirp);
		return;
	}

	DEBUG_LOG_TRUE("rewinddir", "(%p)", dirp);
	TRUE_WRAPPER(rewinddir)(dirp);
}

UBA_EXPORT int UBA_WRAPPER(scandir)(const char* dirp, dirent*** namelist, int (*filter)(const dirent*), int (*compar)(const dirent**, const dirent**))
{
	UBA_INIT_DETOUR(scandir, dirp, namelist, filter, compar);
	return Shared_scandir("scandir", dirp, namelist, filter, compar, [](DIR* d) { return UBA_WRAPPER(readdir)(d); });
}
UBA_EXPORT void UBA_WRAPPER(seekdir)(DIR* dirp, long loc)
{
	UBA_INIT_DETOUR(seekdir, dirp, loc);
	UBA_ASSERTF(!IsDirInfo(dirp), "seekdir");
	DEBUG_LOG_TRUE("seekdir", "(%p)", dirp);
	return TRUE_WRAPPER(seekdir)(dirp, loc);
}

UBA_EXPORT long UBA_WRAPPER(telldir)(DIR* dirp)
{
	UBA_INIT_DETOUR(telldir, dirp);
	UBA_ASSERTF(!IsDirInfo(dirp), "telldir");
	DEBUG_LOG_TRUE("telldir", "(%p)", dirp);
	return TRUE_WRAPPER(telldir)(dirp);
}

UBA_EXPORT DIR* UBA_WRAPPER(fdopendir)(int fd)
{
	UBA_INIT_DETOUR(fdopendir, fd);

	SCOPED_READ_LOCK(g_fileHandlesLock, lock);
	auto findIt = g_fileHandles.find(fd);
	if (findIt == g_fileHandles.end())
	{
		DIR* res = TRUE_WRAPPER(fdopendir)(fd);
		DEBUG_LOG_TRUE("fdopendir", "(%i) -> %p (%s)", fd, res, StrError(res != nullptr, errno));
		return res;
	}
	DetouredHandle& h = findIt->second;
	lock.Leave();
	FileObject* fo = h.fileObject;
	FileInfo& info = *fo->fileInfo;
	if (!fo->isDirectory)
	{
		DEBUG_LOG_DETOURED("fdopendir", "(%i) -> nullptr (ENOTDIR)", fd);
		errno = ENOTDIR;
		return nullptr;
	}

	DEBUG_LOG_DETOURED("fdopendir", "(%i)", fd);
	return opendir(info.originalName);
}

UBA_EXPORT int UBA_WRAPPER(glob)(const char* pattern, int flags, int (*errfunc)(const char* epath, int eerrno), glob_t* pglob)
{
	UBA_INIT_DETOUR(glob, pattern, flags, errfunc, pglob);
	int res = TRUE_WRAPPER(glob)(pattern, flags, errfunc, pglob);
	DEBUG_LOG_TRUE("glob", "%s -> %i", pattern, res);
	return res;
}

#if UBA_DEBUG && PLATFORM_LINUX

UBA_EXPORT int UBA_WRAPPER(fstatat64)(int dirfd, const char* pathname, struct stat64* buf, int flags)
{
	UBA_INIT_DETOUR(fstatat64, dirfd, pathname, buf, flags);

	StringBuffer<> temp;
	if (const char* resolvedPath = ResolvePath(dirfd, pathname, temp))
	{
		UBA_ASSERT(!flags);
		return Shared_stat("fstatat64", resolvedPath, (struct stat*)buf, [](const char* file, struct stat* attr) { return TRUE_WRAPPER(stat64)(file, (struct stat64*)attr); });
	}
	UBA_ASSERT(!g_runningRemote);
	int res = TRUE_WRAPPER(fstatat64)(dirfd, pathname, buf, flags);
	DEBUG_LOG_TRUE("fstatat64", "%i %s -> %i (%s)", dirfd, pathname, res, StrError(res, errno));
	return res;
}

UBA_EXPORT long UBA_WRAPPER(fpathconf)(int fd, int name)
{
	UBA_INIT_DETOUR(fpathconf, fd, name);
	DEBUG_LOG_TRUE("fpathconf", "");
	UBA_ASSERT(false);
	return TRUE_WRAPPER(fpathconf)(fd, name);
}
UBA_EXPORT long UBA_WRAPPER(pathconf)(char *path, int name)
{
	UBA_INIT_DETOUR(pathconf, path, name);
	DEBUG_LOG_TRUE("pathconf", "");
	UBA_ASSERT(false);
	return TRUE_WRAPPER(pathconf)(path, name);
}

#endif

UBA_EXPORT int UBA_WRAPPER(lstat)(const char *path, struct stat *buf)
{
	UBA_INIT_DETOUR(lstat, path, buf);
	return Shared_stat("lstat", path, buf, [](const char* path, struct stat* buf) { return TRUE_WRAPPER(lstat)(path, buf); });
}

UBA_EXPORT off_t  UBA_WRAPPER(lseek)(int fd, off_t offset, int whence)
{
	UBA_INIT_DETOUR(lseek, fd, offset, whence);
	int res = TRUE_WRAPPER(lseek)(fd, offset, whence);
	DEBUG_LOG_TRUE("lseek", "%i %i %i -> %i", fd, offset, whence, res);
	return res;
}

UBA_EXPORT int UBA_WRAPPER(stat)(const char* file, struct stat* attr)
{
	UBA_INIT_DETOUR(stat, file, attr);
	return Shared_stat("stat", file, attr, [](const char* file, struct stat* attr) { return TRUE_WRAPPER(stat)(file, attr); });
}

#if PLATFORM_LINUX

UBA_EXPORT ssize_t UBA_WRAPPER(listxattr)(const char *path, char *_Nullable list, size_t size)
{
	UBA_INIT_DETOUR(listxattr, path, list, size);

	StringBuffer<> fixedPath;
	FixPath(fixedPath, path);
	DevirtualizePath(fixedPath);

	if (!CanDetour2(fixedPath))
	{
		auto res = TRUE_WRAPPER(listxattr)(path, list, size);
		DEBUG_LOG_TRUE("listxattr", "(NODETOUR) %s -> %ll (%s)", path, res, StrError(res, errno));
		return res;
	}

	StringKey fileKey = ToFilenameKey(fixedPath);

	DirectoryTable::EntryInformation entryInfo;
	if (!Rpc_GetEntryInformation(entryInfo, fileKey, fixedPath, false))
	{
		DEBUG_LOG_TRUE("listxattr", "%s -> -1 (ENOENT)", path);
		errno = ENOENT;
		return -1;
	}

	DEBUG_LOG_TRUE("listxattr", "%s -> -1 (ENOTSUP)", path);
	errno = ENOTSUP;
	return -1;
}

UBA_EXPORT int UBA_WRAPPER(stat64)(const char* file, struct stat64* attr)
{
	UBA_INIT_DETOUR(stat64, file, attr);
	return Shared_stat("stat64", file, (struct stat*)attr, [](const char* file, struct stat* attr) { return TRUE_WRAPPER(stat64)(file, (struct stat64*)attr); });
}

UBA_EXPORT int UBA_WRAPPER(__xstat64)(int ver, const char* file, struct stat64* attr)
{
	UBA_INIT_DETOUR(__xstat64, ver, file, attr);
	UBA_ASSERT(ver == _STAT_VER);
	return Shared_stat("__xstat64", file, (struct stat*)attr, [](const char* file, struct stat* attr) { return TRUE_WRAPPER(__xstat64)(_STAT_VER, file, (struct stat64*)attr); });
}

UBA_EXPORT int UBA_WRAPPER(lstat64)(const char* file, struct stat64* attr)
{
	UBA_INIT_DETOUR(lstat64, file, attr);
	return Shared_stat("lstat64", file, (struct stat*)attr, [](const char* file, struct stat* attr) { return TRUE_WRAPPER(lstat64)(file, (struct stat64*)attr); });
}
#endif

UBA_EXPORT int UBA_WRAPPER(truncate)(const char* path, off_t length)
{
	UBA_INIT_DETOUR(truncate, path, length);
	UBA_ASSERTF(!g_runningRemote, "truncate not implemented for remote execution (path: %s)", path); // TODO: Implement this if it is ever called
	return TRUE_WRAPPER(truncate)(path, length);
}

UBA_EXPORT int UBA_WRAPPER(access)(const char* pathname, int mode)
{
	UBA_INIT_DETOUR(access, pathname, mode);
	return Shared_access("access", pathname, mode);
}

UBA_EXPORT int UBA_WRAPPER(eaccess)(const char* pathname, int mode)
{
	UBA_INIT_DETOUR(eaccess, pathname, mode);
	return Shared_access("eaccess", pathname, mode, /*strict=*/true);
}

UBA_EXPORT int UBA_WRAPPER(faccessat)(int dirfd, const char *pathname, int mode, int flags)
{
	UBA_INIT_DETOUR(faccessat, dirfd, pathname, mode, flags);
	StringBuffer<> temp;
	if (const char* resolvedPath = ResolvePath(dirfd, pathname, temp))
	{
		UBA_ASSERT(flags == 0);
		return Shared_access("faccessat", resolvedPath, mode);
	}
	UBA_ASSERT(!g_runningRemote);
	return TRUE_WRAPPER(faccessat)(dirfd, pathname, mode, flags);
}

#if PLATFORM_LINUX
UBA_EXPORT int UBA_WRAPPER(__fxstatat)(int ver, int dirfd, const char* pathname, struct stat* buf, int flags)
{
	UBA_INIT_DETOUR(__fxstatat, ver, dirfd, pathname, buf, flags);
	DEBUG_LOG_TRUE("__fxstatat", "");
	return TRUE_WRAPPER(__fxstatat)(ver, dirfd, pathname, buf, flags);
}

UBA_EXPORT int UBA_WRAPPER(__fxstatat64)(int ver, int dirfd, const char* pathname, struct stat64* buf, int flags)
{
	UBA_INIT_DETOUR(__fxstatat64, ver, dirfd, pathname, buf, flags);
	DEBUG_LOG_TRUE("__fxstatat64", "");
	return TRUE_WRAPPER(__fxstatat64)(ver, dirfd, pathname, buf, flags);
}

UBA_EXPORT int UBA_WRAPPER(__xstat)(int ver, const char* file, struct stat* attr)
{
	UBA_INIT_DETOUR(__xstat, ver, file, attr);
	return Shared_stat("__xstat", file, attr, [ver](const char* file, struct stat* attr) { return TRUE_WRAPPER(__xstat)(ver, file, attr); });
}

UBA_EXPORT int UBA_WRAPPER(__lxstat)(int ver, const char* file, struct stat* attr)
{
	UBA_INIT_DETOUR(__lxstat, ver, file, attr);
	return Shared_stat("__lxstat", file, attr, [ver](const char* file, struct stat* attr) { return TRUE_WRAPPER(__lxstat)(ver, file, attr); });
}

UBA_EXPORT int UBA_WRAPPER(__fxstat)(int ver, int fd, struct stat* attr)
{
	UBA_INIT_DETOUR(__fxstat, ver, fd, attr);
	return Shared_fstat("__fxstat", fd, attr, [ver](int fd, struct stat* attr) { return TRUE_WRAPPER(__fxstat)(ver, fd, attr); });
}

UBA_EXPORT int UBA_WRAPPER(__lxstat64)(int ver, const char* file, struct stat64* attr)
{
	static_assert(sizeof(struct stat64) == sizeof(struct stat));
	UBA_INIT_DETOUR(__lxstat64, ver, file, attr);
	return Shared_stat("__lxstat64", file, (struct stat*)attr, [ver](const char* file, struct stat* attr) { return TRUE_WRAPPER(__lxstat64)(ver, file, (struct stat64*)attr); });
}

UBA_EXPORT int UBA_WRAPPER(statx)(int dirfd, const char* pathname, int flags, unsigned int mask, struct statx* statxbuf)
{
	UBA_INIT_DETOUR(statx, dirfd, pathname, flags, mask, statxbuf);
	return Shared_statx("statx", dirfd, pathname, flags, mask, statxbuf);
}
#endif

int Shared_rename(const char* func, const char* oldpath, const char* newpath)
{
	UBA_INIT_DETOUR(rename, oldpath, newpath);

	StringBuffer<> fixedOldPath;
	FixPath(fixedOldPath, oldpath);
	DevirtualizePath(fixedOldPath);
	StringKey oldKey = ToFilenameKey(fixedOldPath);

	StringBuffer<> fixedNewPath;
	FixPath(fixedNewPath, newpath);
	DevirtualizePath(fixedNewPath);
	StringKey newKey = ToFilenameKey(fixedNewPath);

	DirectoryTable::EntryInformation info;
	if (!Rpc_GetEntryInformation(info, oldKey, fixedOldPath, false))
	{
		DEBUG_LOG_TRUE(func, "(DIRTABLE) from %s to %s -> -1 (ENOENT)", fixedOldPath.data, fixedNewPath.data);
		errno = ENOENT;
		return -1;
	}

	// TODO: This might be really slow but it seems you can rename files on linux while they are open and they won't be properly renamed until closed
	// We could check if info.size == InvalidValue, this way we know if it is open
	{
		SCOPED_READ_LOCK(g_fileHandlesLock, lock);
		for (auto& kv : g_fileHandles)
		{
			FileObject& fo = *kv.second.fileObject;
			FileInfo& fi = *fo.fileInfo;
			if (fi.fileNameKey == oldKey)
			{
				UBA_ASSERTF(fo.desiredAccess & AccessFlag_Write, "Unsupported access flags");
				fo.newName = fixedNewPath.data;
				if (!fo.closeId)
				{
					char temp[1024];
					u64 size;
					Rpc_CreateFileW(fixedNewPath, newKey, AccessFlag_Write, temp, sizeof_array(temp), size, fo.closeId, true);
				}

				bool wasTempFile = fixedNewPath.StartsWith(g_systemTemp.data);
				if (g_runningRemote && !wasTempFile)
				{
					// TODO: We need to make sure this actually worked
					errno = 0;
					DEBUG_LOG_DETOURED(func, "IS_OPEN (%i) (from %s to %s) -> %i (%s)", kv.first, fixedOldPath.data, fixedNewPath.data, 0, StrError(0, errno));
					return 0;
				}
				bool isTempFile = fixedOldPath.StartsWith(g_systemTemp.data);
				UBA_ASSERTF(wasTempFile == isTempFile, "File changing from temp to not or vice versa not implemented");

				int res = TRUE_WRAPPER(rename)(fixedOldPath.data, fixedNewPath.data);
				DEBUG_LOG_DETOURED(func, "IS_OPEN (%i) (from %s to %s) -> %i (%s)", kv.first, fixedOldPath.data, fixedNewPath.data, 0, StrError(res, errno));
				return res;
			}
		}
	}

	bool canDetourOld = CanDetour2(fixedOldPath);
	bool canDetourNew = CanDetour2(fixedNewPath);
	if (!canDetourOld)
	{
		if (!canDetourNew)
		{
			int res = TRUE_WRAPPER(rename)(oldpath, newpath);
			DEBUG_LOG_TRUE(func, "(from %s to %s) -> %i (%s)", fixedOldPath.data, fixedNewPath.data, res, StrError(res, errno));
			return res;
		}
	}
	else
	{
		//UBA_ASSERT(!canDetourNew);
	}


	RPC_MESSAGE(MoveFile, moveFile);
	writer.WriteStringKey(oldKey);
	writer.WriteString(fixedOldPath);
	writer.WriteU64(info.size);
	writer.WriteU64(info.lastWrite);
	writer.WriteU32(info.attributes);
	writer.WriteStringKey(newKey);
	writer.WriteString(fixedNewPath);
	writer.WriteU32(0);// dwFlags);
	BinaryReader reader = writer.Flush();
	bool result = reader.ReadBool();
	u32 errorCode = reader.ReadU32();
	DirTableSize directoryTableSize = FromU64(reader.ReadU64());
	RPC_MESSAGE_END

	g_directoryTable.ParseDirectoryTable(directoryTableSize);
	g_mappedFileTable.SetDeleted(oldKey, fixedOldPath.data, true);
	g_mappedFileTable.SetDeleted(newKey, fixedNewPath.data, false);

	int res = result ? 0 : -1;
	DEBUG_LOG_DETOURED(func, "(from %s to %s) -> %i (%s)", fixedOldPath.data, fixedNewPath.data, res, StrError(res, errorCode));

	errno = errorCode;
	return res;
	//return TRUE_WRAPPER(rename)(oldpath, newpath);
}

UBA_EXPORT int UBA_WRAPPER(rename)(const char* oldpath, const char* newpath)
{
	UBA_INIT_DETOUR(rename, oldpath, newpath);
	return Shared_rename("rename", oldpath, newpath);
}

UBA_EXPORT int UBA_WRAPPER(chmod)(const char* pathname, mode_t mode)
{
	UBA_INIT_DETOUR(chmod, pathname, mode);

	StringBuffer<> fixedName;
	FixPath(fixedName, pathname);
	DevirtualizePath(fixedName);

	if (!CanDetour2(fixedName))
	{
		int res = TRUE_WRAPPER(chmod)(pathname, mode);
		DEBUG_LOG_TRUE("chmod", "%s %i -> %i (%s)", pathname, mode, res, StrError(res, errno));
		return res;
	}

	StringKey key = ToFilenameKey(fixedName);

	DirectoryTable::EntryInformation entryInfo;
	if (!Rpc_GetEntryInformation(entryInfo, key, fixedName, false))
	{
		errno = ENOENT;
		DEBUG_LOG_TRUE("chmod", "%s %i -> -1 (ENOENT)", pathname, mode);
		return -1;
	}

	if (entryInfo.attributes == mode)
	{
		DEBUG_LOG_DETOURED("chmod", "(NOCHANGE) %s %i -> 0", pathname, mode);
		return 0;
	}

	u32 errorCode;
	{
		RPC_MESSAGE(Chmod, chmod);
		writer.WriteStringKey(key);
		writer.WriteString(fixedName);
		writer.WriteU32(mode);
		BinaryReader reader = writer.Flush();
		errorCode = reader.ReadU32();
		//DEBUG_LOG_PIPE(L"MoveFile", L"%ls to %ls", lpExistingFileName, lpNewFileName);
	}

	int res = errorCode == 0 ? 0 : -1;
	DEBUG_LOG_DETOURED("chmod", "%s %i -> %i (%s)", pathname, mode, res, StrError(res, errorCode));

	errno = errorCode;
	return res;
}

UBA_EXPORT int UBA_WRAPPER(renameat)(int olddirfd, const char* oldpath, int newdirfd, const char* newpath)
{
	UBA_INIT_DETOUR(renameat, olddirfd, oldpath, newdirfd, newpath);

	StringBuffer<> temp1;
	const char* resolvedOldPath = ResolvePath(olddirfd, oldpath, temp1);
	UBA_ASSERT(resolvedOldPath);
	StringBuffer<> temp2;
	const char* resolvedNewPath = ResolvePath(newdirfd, newpath, temp2);
	UBA_ASSERT(resolvedNewPath);
	return Shared_rename("renameat", resolvedOldPath, resolvedNewPath);
}

UBA_EXPORT int UBA_WRAPPER(utimensat)(int dirfd, const char* pathname, const struct timespec* times, int flags)
{
	UBA_INIT_DETOUR(utimensat, dirfd, pathname, times, flags);

	StringBuffer<> temp;
	if (const char* resolvedPath = ResolvePath(dirfd, pathname, temp))
	{
		StringBuffer<> fixedPath;
		FixPath(fixedPath, resolvedPath);
		DevirtualizePath(fixedPath);
		FileAttributes fileAttr;
		Shared_GetFileAttributes(fileAttr, fixedPath);
		if (!fileAttr.exists)
		{
			DEBUG_LOG_DETOURED("utimensat", "%s -> -1 (ENOENT)", fixedPath.data);
			errno = ENOENT;
			return -1;
		}

		UBA_ASSERT(flags == 0);

		if (fileAttr.data.st_size == InvalidValue) // Means there is a file handle for it
		{
			SCOPED_READ_LOCK(g_fileHandlesLock, lock);
			for (auto& kv : g_fileHandles)
			{
				int fd = kv.first;
				DetouredHandle& h = kv.second;
				if (!fixedPath.Equals(h.fileObject->fileInfo->originalName))
					continue;
				DEBUG_LOG_TRUE("utimensat", "(futimens) %s", fixedPath.data);
				return futimens(fd, times);
			}
			UBA_ASSERTF(false, "Expected to find %s in file handles list", fixedPath.data);
		}

		// TODO: This should be added to the overlay and applied if process finish successfully
		Rpc_WriteLogf("Skipping utimensat on %s. Needs fix!", resolvedPath);
		return 0;
	}

	UBA_ASSERT(g_runningRemote);
	int res = TRUE_WRAPPER(utimensat)(dirfd, pathname, times, flags);
	DEBUG_LOG_TRUE("utimensat", "(%s) -> %i (%s)", pathname, res, StrError(res, errno));
	return res;
}

UBA_EXPORT int UBA_WRAPPER(symlink)(const char* symlinkContent, const char* symlinkFile)
{
	UBA_INIT_DETOUR(symlink, symlinkContent, symlinkFile);

	StringBuffer<> symlinkFilePath;
	FixPath(symlinkFilePath, symlinkFile);
	DevirtualizePath(symlinkFilePath);

	StringBuffer<> symlinkContentPath;
	if (IsAbsolutePath(symlinkContent))
		FixPath(symlinkContentPath, symlinkContent);
	else
		FixPath(symlinkContentPath, ToView(symlinkContent), ToView(symlinkFilePath).GetPath(true));

	DevirtualizePath(symlinkContentPath);
	if (IsAbsolutePath(symlinkContent))
		symlinkContent = symlinkContentPath.data;

	if (!CanDetour2(symlinkContentPath) && !CanDetour2(symlinkFilePath))
	{
		DEBUG_LOG_TRUE("symlink", "(file: %s content: %s)", symlinkFile, symlinkContent);
		return TRUE_WRAPPER(symlink)(symlinkContent, symlinkFile);
	}

	StringKey contentPathKey = ToFilenameKey(symlinkContentPath);
	StringKey filePathKey = ToFilenameKey(symlinkFilePath);

	// This is wrong since the symlink target (content) does not need to exist. But for now we make that assumption
	// Adding support for "real" symlinks require quite a lot of changes in directory table etc
	DirTableOffset dirTableOffset;
	if (!Rpc_GetEntryOffset(dirTableOffset, contentPathKey, symlinkContentPath, false))
	{
		errno = ENOENT;
		DEBUG_LOG_DETOURED("symlink", "(DIRTABLE) THIS IS WRONG, NEED FIX (file: %s content: %s) -> -1 (ENOENT)", symlinkFilePath.data, symlinkContentPath.data);
		return -1;
	}
	DirectoryTable::EntryInformation info;
	g_directoryTable.GetEntryInformation(info, dirTableOffset);

	if (!info.attributes)
	{
		DEBUG_LOG("symlink - TODO: This is wrong. Need to add support in dirtable for symlinks! (file: %s content: %s)", symlinkFilePath.data, symlinkContentPath.data);
		info.attributes = S_IFLNK;
	}


	RPC_MESSAGE(Symlink, copyFile)
	writer.WriteStringKey(filePathKey);
	writer.WriteString(symlinkFilePath);
	writer.WriteStringKey(contentPathKey);
	writer.WriteString(symlinkContentPath);
	writer.WriteString(symlinkContent);
	writer.WriteFileSize(info.size);
	writer.WriteFileTime(info.lastWrite);
	writer.WriteFileAttributes(info.attributes);
	BinaryReader reader = writer.Flush();
	bool success = reader.ReadBool();
	u32 error = reader.ReadU32();
	DirTableSize directoryTableSize = FromU64(reader.ReadU64());
	RPC_MESSAGE_END

	g_directoryTable.ParseDirectoryTable(directoryTableSize);

	errno = error;
	int res = success ? 0 : -1;
	DEBUG_LOG_TRUE("symlink", "(file: %s content: %s) -> %i (%s)", symlinkFilePath.data, symlinkContent, res, StrError(res, error));
	return res;
}

UBA_EXPORT ssize_t UBA_WRAPPER(pread)(int __fd, void * __buf, size_t __nbyte, off_t __offset)
{
	return TRUE_WRAPPER(pread)(__fd, __buf, __nbyte, __offset);
}

UBA_EXPORT ssize_t UBA_WRAPPER(read)(int fd, void *buf, size_t nbyte)
{
	UBA_INIT_DETOUR(read, fd, buf, nbyte)
	// char filePath[PATH_MAX];
	// if (fcntl(fd, F_GETPATH, filePath) != -1)
	// {
	// 	// printf("***** READ: %d %s bytes: %lu\n",fd, filePath, nbyte);
	// 	// do something with the file path
	// }
	ssize_t res = TRUE_WRAPPER(read)(fd, buf, nbyte);
	DEBUG_LOG_TRUE("read", "%u %llu -> %llu", fd, nbyte, res);
	return res;
}

int Shared_DeleteFile(const char* funcName, const char* pathname)
{
	StringBuffer<> fixedName;
	FixPath(fixedName, pathname);
	DevirtualizePath(fixedName);

	if (!CanDetour2(fixedName))
	{
		int res = TRUE_WRAPPER(unlink)(pathname);
		DEBUG_LOG_TRUE(funcName, "(%s) -> %i (%s)", pathname, res, StrError(res, errno));
		return res;
	}

	StringKey fileNameKey = ToFilenameKey(fixedName);

	DirTableOffset dirTableOffset;
	if (!Rpc_GetEntryOffset(dirTableOffset, fileNameKey, fixedName, false))
	{
		DEBUG_LOG_DETOURED(funcName, "(DIRTABLE) %s (%s) -> Error (ENOENT)", pathname, fixedName.data);
		errno = ENOENT;
		return -1;
	}

	u32 closeId = 0;
	RPC_MESSAGE(DeleteFile, deleteFile);
	writer.WriteString(fixedName);
	writer.WriteStringKey(fileNameKey);
	writer.WriteU32(closeId);
	BinaryReader reader = writer.Flush();
	bool result = reader.ReadBool();
	u32 errorCode = reader.ReadU32();
	DirTableSize directoryTableSize = FromU64(reader.ReadU64());
	RPC_MESSAGE_END

	g_directoryTable.ParseDirectoryTable(directoryTableSize);
	g_mappedFileTable.SetDeleted(fileNameKey, fixedName.data, true);

	int res = result != 0 ? 0 : -1;
	DEBUG_LOG_DETOURED(funcName, "%s (%s) -> %i (%s)", pathname, fixedName.data, res, StrError(res, errorCode));
	errno = errorCode;
	return res;
}

UBA_EXPORT int UBA_WRAPPER(remove)(const char* pathname)
{
	UBA_INIT_DETOUR(remove, pathname);
	// TODO: Should check if pathname is a dir, in that case call rmdir
	return Shared_DeleteFile("remove", pathname);
}

int Shared_link(const char* func, const char* oldpath, const char* newpath)
{
	StringBuffer<> fromPath;
	FixPath(fromPath, oldpath);
	DevirtualizePath(fromPath);
	StringBuffer<> toPath;
	FixPath(toPath, newpath);
	DevirtualizePath(toPath);

	if (!CanDetour2(toPath))
	{
		UBA_ASSERT(!CanDetour2(fromPath));
		int res = TRUE_WRAPPER(link)(oldpath, newpath);
		DEBUG_LOG_TRUE(func, "(NODETOUR) %s to %s -> %i (%s)", oldpath, newpath, res, StrError(res, errno));
		return res;
	}

	StringKey fromKey = ToFilenameKey(fromPath);

	DirectoryTable::EntryInformation fromEntryInfo;
	if (!Rpc_GetEntryInformation(fromEntryInfo, fromKey, fromPath, false))
	{
		DEBUG_LOG_DETOURED(func, "(DIRTABLE) %s to %s -> -1 (ENOENT)", oldpath, newpath);
		errno = ENOENT;
		return -1;
	}
	UBA_ASSERT(!IsDirectory(fromEntryInfo.attributes));

	StringKey toKey = ToFilenameKey(toPath);

	StringBuffer<> newFromPath;
	StringBuffer<> newToPath;

	RPC_MESSAGE(CopyFile, copyFile);
	writer.WriteStringKey(fromKey);
	writer.WriteString(fromPath);
	writer.WriteU64(fromEntryInfo.size);
	writer.WriteU64(fromEntryInfo.lastWrite);
	writer.WriteU32(fromEntryInfo.attributes);
	writer.WriteStringKey(toKey);
	writer.WriteString(toPath);
	BinaryReader reader = writer.Flush();
	reader.ReadString(newFromPath);
	reader.ReadString(newToPath);
	u32 closeId = reader.ReadU32();
	u32 lastError = reader.ReadU32();
	DirTableSize directoryTableSize = FromU64(reader.ReadU64());
	RPC_MESSAGE_END;

	g_directoryTable.ParseDirectoryTable(directoryTableSize);

	UBA_ASSERT(closeId != ~0u);
	UBA_ASSERT(closeId != 0);

	int res = TRUE_WRAPPER(link)(newFromPath.data, newToPath.data);
	DEBUG_LOG_TRUE(func, "%s to %s (%s to %s) -> %i (%s)", oldpath, newpath, newFromPath.data, newToPath.data, res, StrError(res, errno));

	bool deleteOnClose = res != 0; // If failing to copy we set deleteOnClose
	Rpc_UpdateCloseHandle(newToPath.data, closeId, deleteOnClose, "", {}, fromEntryInfo.size, fromEntryInfo.lastWrite, fromEntryInfo.attributes, true);

	return res;
}

UBA_EXPORT int UBA_WRAPPER(link)(const char* oldpath, const char* newpath)
{
	UBA_INIT_DETOUR(link, oldpath, newpath);
	return Shared_link("link", oldpath, newpath);
}

UBA_EXPORT int UBA_WRAPPER(linkat)(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
	UBA_INIT_DETOUR(linkat, olddirfd, oldpath, newdirfd, newpath, flags);
	StringBuffer<> temp1;
	const char* resolvedOldPath = ResolvePath(olddirfd, oldpath, temp1);
	UBA_ASSERT(resolvedOldPath);
	StringBuffer<> temp2;
	const char* resolvedNewPath = ResolvePath(newdirfd, newpath, temp2);
	UBA_ASSERT(resolvedNewPath);
	UBA_ASSERT(!flags);
	return Shared_link("linkat", resolvedOldPath, resolvedNewPath);
}

UBA_EXPORT int UBA_WRAPPER(unlink)(const char* pathname)
{
	UBA_INIT_DETOUR(unlink, pathname);
	return Shared_DeleteFile("unlink", pathname);
}

UBA_EXPORT int UBA_WRAPPER(unlinkat)(int dirfd, const char *pathname, int flags)
{
	UBA_INIT_DETOUR(unlinkat, dirfd, pathname, flags);

	StringBuffer<> temp;
	if (const char* resolvedPath = ResolvePath(dirfd, pathname, temp))
	{
		if (flags == AT_REMOVEDIR)
			return rmdir(resolvedPath);
		UBA_ASSERT(flags == 0);
		return Shared_DeleteFile("unlinkat", resolvedPath);
	}
	UBA_ASSERT(!g_runningRemote);
	DEBUG_LOG_TRUE("unlinkat", "(%i) %s", dirfd, pathname)
	return TRUE_WRAPPER(unlinkat)(dirfd, pathname, flags);
}

void FlattenArgs(StringBufferBase& out, const char* const argv[])
{
	if (!argv)
		return;
	for (u32 i = 0; argv[i]; ++i)
	{
		if (i != 0)
			out.Append(' ');
		out.Append(argv[i]);
	}
}

bool ExecuteHostRun(StringBufferBase& out, const char* const* argv, bool removeLineFeed = true)
{
	{
		StringBuffer<4096> command;
		FlattenArgs(command, argv);
		DEBUG_LOG_DETOURED("HostRun", "%s", command.data)
	}
	RPC_MESSAGE(HostRun, getFullFileName);
	u16& size = *(u16*)writer.AllocWrite(2);
	u64 pos = writer.GetPosition();
	for (u32 i = 0; argv[i]; ++i)
		writer.WriteString(argv[i]);
	size = u16(writer.GetPosition() - pos);

	BinaryReader reader = writer.Flush();
	bool success = reader.ReadBool();
	reader.ReadString(out);
	
	if (removeLineFeed && out.count && out.data[out.count-1] == '\n')
		out.Resize(out.count-1);

	if (!success)
		DEBUG_LOG("HOSTRUN FAILED: %s", out.data)
	return success;
}

int SpawnEcho(char* str, pid_t* pid, const posix_spawn_file_actions_t* file_actions, const posix_spawnattr_t* attrp, char* const envp[])
{
#if 0
	static Atomic<u32> counter;
	StringBuffer<> tempFile;
	tempFile.Append(g_systemTemp).EnsureEndsWithSlash().Append("UbaTempFile").AppendValue(getpid()).Append('_').AppendValue(counter++);
	int fd = TRUE_WRAPPER(open)(tempFile.data, O_CREAT|O_TRUNC|O_WRONLY, S_IRUSR | S_IWUSR);
	TRUE_WRAPPER(write)(fd, str, strlen(str));
	TRUE_WRAPPER(close)(fd);

	DEBUG_LOG("Created %s for cat", tempFile.data);
	char cmd[] = "/bin/cat"; 
	char* const argv2[] = { cmd, tempFile.data, nullptr };
	int res = TRUE_WRAPPER(posix_spawn)(pid, cmd, file_actions, attrp, argv2, envp);
	DEBUG_LOG_TRUE("posix_spawn", "(CAT) %s (pid: %u) -> %i", str, *pid, res);
#else
	char* const env[] = { nullptr };
	char cmd[] = "/bin/echo"; 
	char* const argv2[] = { cmd, str, nullptr };
	int res = TRUE_WRAPPER(posix_spawn)(pid, cmd, file_actions, attrp, argv2, env);
	DEBUG_LOG_TRUE("posix_spawn", "(ECHO) %s (pid: %u) -> %i", str, *pid, res);
#endif
	return res;
}

void UnsupportedHostRun(char* const argv[], const char* msg)
{
	StringBuffer<4096> command;
	FlattenArgs(command, argv);
	UbaAssert(msg, __FILE__, __LINE__, command.data, true, 1999, nullptr, 0);
}

struct ProcessCreator
{
	int SendMessage(MessageType messageType, const char* messageName, const char* path, char* const argv[], char* const envp[], bool fixupPath, bool isExec)
	{
		fixedArgv = argv;

		auto prefixArgv = [&](u32 prefixCount, u32 skipCount)
			{
				UBA_ASSERT(fixedArgv != tempArgv.data());
				u32 count = 0;
				for (int i = skipCount; argv[i]; ++i)
					++count;
				tempArgv.resize(count + prefixCount + 1);
				for (int i = skipCount; argv[i]; ++i)
					tempArgv[i - skipCount + prefixCount] = argv[i];
				tempArgv[count + prefixCount] = nullptr;
				fixedArgv = (char* const*)tempArgv.data();
			};

		if (fixupPath)
		{
			if (!path || !*path)
				FixPath(result, argv[0]);
			else
				FixPath(result, path);

			DevirtualizePath(result);
			path = result.data;

			// If the executable is a shell script, wrap it with /bin/sh so the kernel
			// does not need to parse the shebang — and so UBA's virtual FS interception
			// does not prevent the script from being opened by the kernel's exec path.
			u32 plen = result.count;
			if (plen >= 3 && result.data[plen-3] == '.' && result.data[plen-2] == 's' && result.data[plen-1] == 'h')
			{
				prefixArgv(2, 1);
				tempArgv[0] = "/bin/sh";
				tempArgv[1] = path; // the .sh script (still in result.data)
				path = "/bin/sh";
			}
		}
		else if (path && strchr(path, '/'))
		{
			// execvp-style (no fixup for bare filenames), but if the path contains a slash
			// it may still be a virtualized absolute/relative path — devirtualize it.
			result.Append(path);
			DevirtualizePath(result);
			path = result.data;
		}

		for (u32 i = 1; fixedArgv[i]; ++i)
		{
			if (i != 1)
				cmdLineWithoutApplication.append(" ");
			const char* arg = fixedArgv[i];
			// Quote arguments that contain spaces or double-quotes so ParseArguments
			// on the receiving end reconstructs the correct argv (e.g. sed -e scripts).
			bool needsQuoting = false;
			for (const char* p = arg; *p; ++p)
				if (*p == ' ' || *p == '\t' || *p == '"') { needsQuoting = true; break; }
			if (needsQuoting)
			{
				cmdLineWithoutApplication.append("\"");
				for (const char* p = arg; *p; ++p)
				{
					if (*p == '"') cmdLineWithoutApplication.append("\\\"");
					else           cmdLineWithoutApplication += *p;
				}
				cmdLineWithoutApplication.append("\"");
			}
			else
				cmdLineWithoutApplication.append(arg);
		}

		RPC_MESSAGE2(messageType, messageName, createProcess);
		writer.WriteString(path); // Application
		writer.WriteLongString(cmdLineWithoutApplication, CommunicationMemSize - 4096);
		writer.WriteString(g_virtualWorkingDir); // Current dir
		writer.WriteBool(false); // Start suspended
		writer.WriteBool(true); // Is child

		BinaryReader reader = writer.Flush();
		processId = reader.ReadU32();

		if (!processId) // Can happen if session client got disconnected from server session
		{
			errno = EINVAL; // This is not really correct but there is no errno for this failure
			return -1;
		}

		StringBuffer<1024> temp;

		rulesStr = temp.Append("UBA_RULES=").AppendValue(reader.ReadU32()).ToStringAndClear();

		StringBuffer<256> detoursLibPath;
		reader.ReadString(detoursLibPath);

		virtualApplication = reader.ReadString();

		currentDir = temp.Append("UBA_CWD=").ReadString(reader).ToStringAndClear();
		realApplication = reader.ReadString();

		#if UBA_USE_NATIVE_MAC_SEMAPHORES
		pidVar = temp.Append("UBA_PID=").AppendValue(reader.ReadU32()).ToStringAndClear();
		#endif

		comIdVar = temp.Append("UBA_COMID=").AppendValue(reader.ReadU64()).Append('+').AppendValue(reader.ReadU32()).ToStringAndClear();
		logFile = temp.Append("UBA_LOGFILE=").ReadString(reader).ToStringAndClear();
		RPC_MESSAGE_END

		// Shebang?
		if (!StringView(realApplication).GetFileName().Equals(ToView(path).GetFileName()))
		{
			DEBUG_LOG("Application is a shebang (%s -> %s)", path, virtualApplication.c_str());
			prefixArgv(1, 0);
			tempArgv[0] = virtualApplication.c_str();
		}

		#if PLATFORM_LINUX
		constexpr auto libPathName = TCV("LD_LIBRARY_PATH=");
		constexpr auto preloadName = TCV("LD_PRELOAD=");
		#else
		constexpr auto libPathName = TCV("DYLD_LIBRARY_PATH=");
		constexpr auto preloadName = TCV("DYLD_INSERT_LIBRARIES=");
		#endif

		StringView callerLibPath;
		bool hasLdPreload = false;
		bool hasTmpDir = false;

		for (u32 i = 0; envp[i]; ++i)
		{
			if (StartsWith(envp[i], "UBA_")) // Remove all  UBA_ variables. On some distros they are removed and some not.. easier to just redo
				continue;

			if (StartsWith(envp[i], libPathName.data, false))
			{
				callerLibPath = ToView(envp[i]).Skip(libPathName.count);
				continue;
			}
			hasLdPreload = hasLdPreload || StartsWith(envp[i], "LD_PRELOAD=", false);
			hasTmpDir = hasTmpDir || StartsWith(envp[i], "TMPDIR=", false);

			envvars.push_back(envp[i]);
		}

		{
			temp.Append(libPathName);
			bool isFirst = true;
			auto append = [&](StringView sv) { if (!isFirst) temp.Append(':'); temp.Append(sv); isFirst = false; };
			if (!callerLibPath.Contains(detoursLibPath))
				append(detoursLibPath);
			if (g_runningRemote && !callerLibPath.Contains(realApplication))
				append(ToView(realApplication).GetPath());
			if (callerLibPath.count) // TODO: DO WE REALLY WANT THIS? We will end up with the parent's directory path as well
				append(callerLibPath);
			ldLibraryPath = temp.ToStringAndClear();
		}

		if (g_runningRemote)
			envvars.push_back("UBA_REMOTE=1");

		envvars.push_back((sessionProcess = temp.Append("UBA_SESSION_PROCESS=").Append(getenv("UBA_SESSION_PROCESS")).ToStringAndClear()).data());

		if (!hasLdPreload)
			envvars.push_back("LD_PRELOAD=libUbaDetours.so");

		if (!hasTmpDir)
			envvars.push_back((tmpDir = temp.Append(TCV("TMPDIR=")).Append(getenv("TMPDIR")).ToStringAndClear()).data());

		UBA_ASSERT(!g_runningRemote || !ldLibraryPath.empty());

		envvars.push_back(comIdVar.data());
		envvars.push_back(currentDir.data());
		envvars.push_back(rulesStr.data());
		envvars.push_back(logFile.data());
		envvars.push_back(ldLibraryPath.data());

		if (isExec)
			envvars.push_back("UBA_EXEC=1");
		//envvars.push_back("LD_DEBUG=bindings");
		//envvars.push_back("LD_DEBUG=libs");

		#if UBA_USE_NATIVE_MAC_SEMAPHORES
		envvars.push_back(pidVar.data);
		#endif

		envvars.push_back(nullptr);

		#if 0
		DEBUG_LOG("SPAWNING %s", envvars.data()[0]);
		for (int i=1;envvars.data()[i]; ++i)
			DEBUG_LOG("env: %s", envvars.data()[i]);
		#endif

		return 0;
	}

	TString virtualApplication;
	TString realApplication;
	TString cmdLineWithoutApplication;
	u32 processId = 0;
	TString currentDir;
	TString comIdVar;
	TString rulesStr;
	TString logFile;
	TString ldLibraryPath;
	TString sessionProcess;
	TString tmpDir;
	#if UBA_USE_NATIVE_MAC_SEMAPHORES
	TString pidVar;
	#endif
	StringBuffer<> result;
	Vector<const char*> tempArgv;
	char* const* fixedArgv;
	Vector<const char*> envvars;
};

void SendStartProcess(u32 processId, u32 pid, bool success, u32 res)
{
	RPC_MESSAGE(StartProcess, createProcess);
	writer.WriteU32(processId);
	writer.WriteBool(success);
	writer.WriteU32(res);
	writer.WriteU64(1); // Process handle
	writer.WriteU32(pid);
	writer.WriteU64(0); // Thread handle
	writer.Flush();
}

int shared_posix_spawn(const char* func, pid_t* pid, const char* path, const posix_spawn_file_actions_t* file_actions, const posix_spawnattr_t* attrp, char* const argv[], char* const envp[])
{
	UBA_INIT_DETOUR(posix_spawn, pid, path, file_actions, attrp, argv, envp);

	{
		//StringBuffer<4096> command;
		//FlattenArgs(command, argv);
		//DEBUG_LOG("RUNNING: %s", command.data);
	}

	const char* tempArgv[1024];
	StringBuffer<> result;
	StringBuffer<> additionalArg;

	bool fixupPath = true;

	if (strstr(path, "xcode-select"))
	{
		if (strcmp(argv[1], "--print-path") != 0)
			UbaAssert("xcode-select only supported with --print-path", __FILE__, __LINE__, "", true, 1999, nullptr, 0);
		if (!ExecuteHostRun(result, argv))
			return -1;
		return SpawnEcho(result.data, pid,  file_actions, attrp, envp);
	}
	
	if(strstr(path, "xcrun"))
	{
		if (strcmp(argv[1], "--sdk") != 0)
			UnsupportedHostRun(argv, "xcrun unsupported first param");

		if (strcmp(argv[3], "--find") == 0)
		{
			if (!ExecuteHostRun(result, argv))
				return -1;
			return SpawnEcho(result.data, pid,  file_actions, attrp, envp);
		}
		
		if (strcmp(argv[3], "metal") != 0 && strcmp(argv[3], "metallib") != 0)
			UnsupportedHostRun(argv, "xcrun unsupported third param");

		const char* argv2[] = { argv[0], argv[1], argv[2], "--find", argv[3], 0 };
		if (!ExecuteHostRun(result, argv2))
			return -1;

#if 0 // This does not seem to be needed anymore. If we run into this again then we should instead use realpath query to host since below logic does not work with newer sdk
		// This will return the trampoline metal which we don't want
		if (const char* usrbin = strstr(result.data, "/bin/metal"))
		{
			result.Resize(usrbin - result.data);
			result.Append("/metal/");
			if (strcmp(argv[2], "macosx") == 0)
				result.Append("macos");
			else
				result.Append("ios");
			result.Append("/bin/").Append(argv[3]);
		}
#endif

		path = result.data;

		u32 argc3 = 0;
		tempArgv[argc3++] = result.data;
		for (int i=4;argv[i]; ++i)
			tempArgv[argc3++] = argv[i];
		if (Equals(argv[3], "metal"))
		{
			// This is needed because we want clang cache to be local to machine and not be under the host machine's temp (which might not match remote machine's temp)
			additionalArg.Append("-fmodules-cache-path=").Append(g_systemTemp).EnsureEndsWithSlash().Append("clangcache");
			tempArgv[argc3++] = additionalArg.data;
		}
		tempArgv[argc3] = 0;
		argv = (char*const*)tempArgv;
		fixupPath = false;
	}
	//DEBUG_LOG("LIBRARY_SEARCH_PATHS: %s", getenv("LIBRARY_SEARCH_PATHS"));
	//DEBUG_LOG("PATH: %s", getenv("PATH"));

	ProcessCreator pc;
	int res = pc.SendMessage(MessageType_CreateProcess, "CreateProcess", path, argv, envp, fixupPath, false);
	if (res != 0)
	{
		DEBUG_LOG_DETOURED(func, "SendMessage failed");
		return res;
	}

	#if UBA_DEBUG_LOG_ENABLED
	DEBUG_LOG_TRUE(func, "%s (%s)", pc.realApplication.data(), pc.logFile.data());
	for (u32 i = 0; pc.fixedArgv[i]; ++i)
		DEBUG_LOG("            %s", pc.fixedArgv[i]);
	#endif

	constexpr u32 maxRetries = 10;
	for (u32 attempt = 0; ; ++attempt)
	{
		res = TRUE_WRAPPER(posix_spawn)(pid, pc.realApplication.data(), file_actions, attrp, pc.fixedArgv, (char**)pc.envvars.data());
		if (res == 0 || errno != ETXTBSY || attempt >= maxRetries)
			break;
		Sleep(10*(attempt+1));
		DEBUG_LOG_TRUE(func, "ETXTBSY retry %u for %s", attempt + 1, pc.realApplication.data());

	}
	bool success = res == 0;

	SendStartProcess(pc.processId, *pid, success, res);
	DEBUG_LOG("         Child process started %s -> %i (pid: %i)", path, res, *pid);
	return res;
}

UBA_EXPORT int UBA_WRAPPER(posix_spawn)(pid_t* pid, const char* path, const posix_spawn_file_actions_t* file_actions, const posix_spawnattr_t* attrp, char* const argv[], char* const envp[])
{
	return shared_posix_spawn("posix_spawn", pid, path, file_actions, attrp, argv, envp);
}

UBA_EXPORT int UBA_WRAPPER(posix_spawnp)(pid_t* pid, const char* file, const posix_spawn_file_actions_t* file_actions, const posix_spawnattr_t* attrp, char* const argv[], char* const envp[])
{
	return shared_posix_spawn("posix_spawnp", pid, file, file_actions, attrp, argv, envp);
}

UBA_EXPORT pid_t UBA_WRAPPER(wait)(int* status)
{
	UBA_INIT_DETOUR(wait, status);
	pid_t res = TRUE_WRAPPER(wait)(status);
	DEBUG_LOG_TRUE("wait", "%i -> %i", status ? *status : 0, res);
	return res;
}

UBA_EXPORT pid_t UBA_WRAPPER(waitpid)(pid_t pid, int* status, int options)
{
	UBA_INIT_DETOUR(waitpid, pid, status, options);
	// TODO: Should probably report id to session
	// Always pass status to waitpid() so that we know whether to call Rpc_UpdateTables()
	int resStatus = 0;
	pid_t res = TRUE_WRAPPER(waitpid)(pid, &resStatus, options);
	DEBUG_LOG_TRUE("waitpid", "(%i) -> %i (%i)", pid, res, resStatus);
	if (WIFEXITED(resStatus))
		Rpc_UpdateTables();
	if (status)
		*status = resStatus;
	return res;
}

const char* GetResult(siginfo_t* info)
{
	if (!info)
		return "null";
	int code = info->si_code;
	if (code == CLD_EXITED)
		return "Exited";
	if (code == CLD_KILLED)
		return "Killed";
	if (code == CLD_STOPPED)
		return "Stopped";
	if (code == CLD_CONTINUED)
		return "Continued";
	if (code == CLD_TRAPPED)
		return "Trapped";
	return "Running";
}

UBA_EXPORT int UBA_WRAPPER(waitid)(idtype_t idtype, id_t id, siginfo_t* infop, int options)
{
	UBA_INIT_DETOUR(waitid, idtype, id, infop, options);
	auto res = TRUE_WRAPPER(waitid)(idtype, id, infop, options);
	DEBUG_LOG_TRUE("waitid", "%i -> %i (%s)", id, res, GetResult(infop));
	if (infop->si_code == CLD_EXITED)
		Rpc_UpdateTables();
	return res;
}

UBA_EXPORT pid_t UBA_WRAPPER(wait3)(int* status, int options, struct rusage* rusage)
{
	UBA_INIT_DETOUR(wait3, status, options, rusage);
	pid_t res =TRUE_WRAPPER(wait3)(status, options, rusage);
	DEBUG_LOG_TRUE("wait3", "-> %i (%i)", res, *status);
	if (WIFEXITED(*status))
		Rpc_UpdateTables();
	return res;
}

UBA_EXPORT pid_t UBA_WRAPPER(wait4)(pid_t pid, int* status, int options, struct rusage* rusage)
{
	UBA_INIT_DETOUR(wait4, pid, status, options, rusage);
	pid_t res = TRUE_WRAPPER(wait4)(pid, status, options, rusage);
	if (WIFEXITED(*status))
		Rpc_UpdateTables();
	DEBUG_LOG_TRUE("wait4", "(pid %i) -> %i (%i)", pid, res, *status);
	return res;
}

UnorderedSet<TString> g_handledLibraries;

void Shared_LoadLibrary(const char*& path, const char* const* loaderPaths, StringBufferBase& tempBuf)
{
	StringBuffer<512> virtualPath;
	Rpc_GetFullFileName2(path, tempBuf, virtualPath, loaderPaths);
	StringView originalPath = StringView(virtualPath).GetPath();
	StringBuffer<> error;
	BinaryInfo info;
	ParseBinary(tempBuf, originalPath, info, [&](const tchar* import, bool isKnown, const char* const* importLoaderPaths)
	{
		if (!g_handledLibraries.insert(import).second)
			return;
		StringBuffer<> temp;
		Shared_LoadLibrary(import, importLoaderPaths, temp);
	}, error);
	if (error.count)
		DEBUG_LOG(error.data);
}

UBA_EXPORT void* UBA_WRAPPER(dlopen)(const char* path, int mode)
{
	UBA_INIT_DETOUR(dlopen, path, mode);

	auto runTrue = [&]()
		{
			void* res = TRUE_WRAPPER(dlopen)(path, mode);
			DEBUG_LOG_TRUE("dlopen", "%s (%i) -> 0x%x", path, mode, res);
			return res;
		};

	StringBuffer<> tempBuf;
	if (g_isUsingLocalBinary || !path || !*path)
		return runTrue();

#if PLATFORM_MAC
	if (StartsWith(path, "@rpath/"))
	{
		path += 7;
		const char* loaderPaths[] = { "/", 0 };
		if (g_handledLibraries.insert(path).second)
			Shared_LoadLibrary(path, loaderPaths, tempBuf);
	}
	else if (!StartsWith(path, "/System") && !StartsWith(path, "/usr/lib"))
	{
		u64 nameLen = 0;
		Rpc_GetFullFileName(path, nameLen, tempBuf, false);
	}
#else

	//#if PLATFORM_LINUX
	//if (StartsWith(path, "/usr/bin/"))
	//	return runTrue();
	//#endif

	if (!IsKnownSystemFile(ToView(path).GetFileName().data))
	{
		const char* loaderPaths[] = { "/", 0 };
		if (g_handledLibraries.insert(path).second)
			Shared_LoadLibrary(path, loaderPaths, tempBuf);
		if (const char* name = TStrrchr(path, '/')) // We want to use LD_LIBRARY_PATH to find the .so file after downloading it
			path = name + 1;
	}
#endif

	return runTrue();
}

UBA_EXPORT int UBA_WRAPPER(dladdr)(const void *addr, Dl_info *info)
{
	UBA_INIT_DETOUR(dladdr, addr, info);
	int res = TRUE_WRAPPER(dladdr)(addr, info);
	if (res && info->dli_fname && *info->dli_fname && StartsWith(info->dli_fname, g_exeDir.data))
	{
		StringBuffer<PATH_MAX> newPath;
		newPath.AppendDir(g_virtualApplication).Append('/').Append(info->dli_fname + g_exeDir.count - 1); // TODO: Make g_handledLibraries a map and store the real path
		info->dli_fname = g_memoryBlock.Strdup(newPath).data;
	}
	//DEBUG_LOG_TRUE("dladdr", "%s -> %i", (info->dli_fname&&*info->dli_fname)?info->dli_fname:"", res);
	return res;
}

int Internal_exec(const char* func, const char* pathname, char* const _Nullable argv[], char* const _Nullable envp[], bool fixupPath)
{
	UBA_INIT_DETOUR(execve, pathname, argv, envp);

	// in vfork it is common that all fds are closed, and we need this one for logging
	// Also, we can leak it with O_CLOEXEC since it will go away with a successful execve
	#if UBA_DEBUG_LOG_ENABLED
	if (TRUE_WRAPPER(fcntl)(g_debugFile, F_GETFD) == -1 && errno == EBADF)
		g_debugFile = (FileHandle)TRUE_WRAPPER(open)(g_debugLogPath.data, O_APPEND | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
	#endif

	ProcessCreator pc;
	int res = pc.SendMessage(MessageType_ExecProcess, "ExecProcess", pathname, argv, envp, fixupPath, true);
	if (res != 0)
	{
		DEBUG_LOG_DETOURED(func, "SendMessage failed");
		return res;
	}

	DEBUG_LOG_TRUE(func, "%s %s", pc.realApplication.data(), pc.cmdLineWithoutApplication.data());

	constexpr u32 maxRetries = 10;
	constexpr u32 baseSleepUs = 500;   // 0.5ms
	for (u32 attempt = 0; ; ++attempt)
	{
		if (TRUE_WRAPPER(execve)(pc.realApplication.data(), pc.fixedArgv, (char**)pc.envvars.data()) == 0)
			return 0;

		int err = errno;
		if (err != ETXTBSY || attempt >= maxRetries)
		{
			DEBUG_LOG_TRUE(func, "FAILED!! errno=%d attempts=%u", err, attempt + 1);
			errno = err;
			return -1;
		}
		struct timespec ts { 0, long(baseSleepUs * (attempt + 1)) * 1000 };
		nanosleep(&ts, nullptr);
		DEBUG_LOG_TRUE(func, "ETXTBSY retry %u for %s", attempt + 1, pc.realApplication.data());
	}
}

UBA_EXPORT int UBA_WRAPPER(execv)(const char* path, char* const argv[])
{
	return Internal_exec("execv", path, argv, environ, true);
}

UBA_EXPORT int UBA_WRAPPER(execve)(const char* pathname, char* const _Nullable argv[], char* const _Nullable envp[])
{
	return Internal_exec("execve", pathname, argv, envp, true);
}

UBA_EXPORT int UBA_WRAPPER(execvp)(const char* file, char* const argv[])
{
	// This devirtualize should not be needed but fixes Mac ld for some weird reason. This should be fixed up deeper in to the callstack
	StringBuffer<> temp(file);
	DevirtualizePath(temp);
	file = temp.data;
	// Remove above after debugged

	return Internal_exec("execvp", file, argv, environ, false);
}

UBA_EXPORT int UBA_WRAPPER(execl)(const char *path, const char *arg0, ...)
{
	DEBUG_LOG_TRUE("execl", "");
	UBA_ASSERTF(false, "execl NotImplemented");
	return -1;
}

UBA_EXPORT int UBA_WRAPPER(execle)(const char *path, const char *arg0, ...)
{
	DEBUG_LOG_TRUE("execle", "");
	UBA_ASSERTF(false, "execle NotImplemented");
	return -1;
}

UBA_EXPORT int UBA_WRAPPER(execlp)(const char *file, const char *arg0, ...)
{
	DEBUG_LOG_TRUE("execlp", "");
	UBA_ASSERTF(false, "execlp NotImplemented");
	return -1;
}

#if PLATFORM_LINUX
UBA_EXPORT int UBA_WRAPPER(fexecve)(int fd, char *const argv[], char *const envp[])
{
	DEBUG_LOG_TRUE("fexecve", "");
	char path[PATH_MAX];
	char procLink[64];
	snprintf(procLink, sizeof(procLink), "/proc/self/fd/%d", fd);
	ssize_t n = readlink(procLink, path, sizeof(path) - 1);
	if (n < 0) {
		DEBUG_LOG_TRUE("fexecve", "readlink failed fd=%d", fd);
		return TRUE_WRAPPER(fexecve)(fd, argv, envp);
	}
	path[n] = 0;
	DEBUG_LOG_DETOURED("fexecve", "fd=%d -> %s", fd, path);
	return Internal_exec("fexecve", path, argv, envp, false);
}
#endif

#if PLATFORM_MAC
UBA_EXPORT int UBA_WRAPPER(execvP)(const char *file, const char *search_path, char *const argv[])
{
	DEBUG_LOG_TRUE("execvP", "");
	UBA_ASSERTF(false, "execvP NotImplemented");
	return -1;
}

UBA_EXPORT pid_t UBA_WRAPPER(fork)(void)
{
	UBA_INIT_DETOUR(fork);
	DEBUG_LOG_TRUE("fork", "");
	return TRUE_WRAPPER(fork)();
}
#endif

pid_t Internal_fork()
{
	UBA_INIT_DETOUR(fork);

	u32 childProcessId = ~0u;
	StringBuffer<256> comId;
	StringBuffer<512> logFile;
	{
		RPC_MESSAGE(ForkProcess, createProcess);
		BinaryReader reader = writer.Flush();
		childProcessId = reader.ReadU32();
		comId.AppendValue(reader.ReadU64()).Append('+').AppendValue(reader.ReadU32());
		reader.ReadString(logFile);
	}

	pid_t pid = TRUE_WRAPPER(fork)();
	if (pid == 0)
	{
		#if PLATFORM_LINUX
		prctl(PR_SET_PDEATHSIG, SIGHUP, 0, 0, 0); // We want the process to die if the parent die
		#endif

		#if UBA_DEBUG_LOG_ENABLED
		g_debugLogPath.Clear().Append(logFile);
		g_debugFile = (FileHandle)TRUE_WRAPPER(open)(logFile.data, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
		#endif

		DEBUG_LOG(TC("FORKED pid: %i"), getpid());
		StringBuffer<256> comIdName;
		const char* plusIndex = comId.First('+');
		comIdName.Append(comId.data, u64(plusIndex - comId.data));
		u64 comIdUid;
		if (!comIdName.Parse(comIdUid))
			UBA_ASSERT(false);

		u32 comIdOffset = strtoul(plusIndex + 1, nullptr, 10);
		GetMappingHandleName(comIdName.Clear(), comIdUid);

		g_comFd = shm_open(comIdName.data, O_RDWR, S_IRUSR | S_IWUSR);

		if (g_comFd == -1)
		{
			printf("UbaDetours: Failed to open shared mem: %s\n", comIdName.data);
			UBA_ASSERTF(false, "UbaDetours: Failed to open shared mem: %s", comIdName.data);
		}
		u8* rptr = (u8*)mmap(NULL, CommunicationMemSize, PROT_READ | PROT_WRITE, MAP_SHARED, g_comFd, s64(comIdOffset));
		if (rptr == MAP_FAILED)
		{
			printf("UbaDetours: Failed to mmap fd: %u\n", g_comFd);
			UBA_ASSERT(false);
		}
		TRUE_WRAPPER(fcntl)(g_comFd, F_SETFD, FD_CLOEXEC); // Don't let grandchild processes inherit this fd

		g_messageMappingMem = rptr;
		g_cancelEvent = ((SharedEvent*)rptr);
		g_readEvent = ((SharedEvent*)rptr) + 1;
		g_writeEvent = ((SharedEvent*)rptr) + 2;
		g_messageMappingMem += sizeof(SharedEvent) * 3;

		// We need to clean file handles to prevent code running between fork and execve to send messages to host when closing handles
		g_fileHandles.clear();
	}
	else
	{
		SendStartProcess(childProcessId, pid, true, 0);
		DEBUG_LOG("         Process forked (child pid: %i)", pid);
	}

	return pid;
}

#if PLATFORM_LINUX
UBA_EXPORT pid_t UBA_WRAPPER(fork)(void)
{
	DEBUG_LOG_TRUE("fork", "");
	return Internal_fork();
}
#endif

#if PLATFORM_MAC
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

UBA_EXPORT pid_t UBA_WRAPPER(vfork)(void)
{
	DEBUG_LOG_DETOURED("vfork", "");
	return Internal_fork();
}

#if PLATFORM_LINUX
UBA_EXPORT long UBA_WRAPPER(syscall)(long number, ...)
{
	va_list args;
	va_start(args, number);
	void* a1 = va_arg(args, void*);
	void* a2 = va_arg(args, void*);
	void* a3 = va_arg(args, void*);
	void* a4 = va_arg(args, void*);
	void* a5 = va_arg(args, void*);
	void* a6 = va_arg(args, void*);
	va_end(args);

	UBA_INIT_DETOUR(syscall, number, a1, a2, a3, a4, a5, a6);

	switch (number)
	{
	case SYS_write:
	{
		DEBUG_LOG_TRUE("syscall.write", "fd=%ld", (long)(uintptr_t)a1);
		return TRUE_WRAPPER(syscall)(number, a1, a2, a3, a4, a5, a6);
	}

	case SYS_close:
	{
		int fd = (int)(long)(uintptr_t)a1;
		return Shared_close("syscall.close", fd, [fd]() { return TRUE_WRAPPER(close)(fd); });
	}

	case SYS_gettid:
	case SYS_capget:
	case SYS_futex:
		break;

#if !PLATFORM_LINUX || !PLATFORM_CPU_ARM_FAMILY
	case SYS_vfork:
		{ DEBUG_LOG_DETOURED("syscall.vfork", "");
		return Internal_fork(); }
	case SYS_fork:
		{ DEBUG_LOG_DETOURED("syscall.fork", "");
		return Internal_fork(); }
#endif

	case SYS_clone:
		{ DEBUG_LOG_DETOURED("syscall.clone", "");
		return Internal_fork(); }

	case 217: // SYS_getdents64
	{
		int fd = (int)(long)(uintptr_t)a1;
		void* buf = a2;
		unsigned int cnt = (unsigned int)(uintptr_t)a3;
		int capturedErr = 0;
		long r = Shared_getdents64("syscall.getdents64", fd, buf, cnt, &capturedErr);
		if (r < 0 && capturedErr == 0)
			return TRUE_WRAPPER(syscall)(number, a1, a2, a3, a4, a5, a6); // not a UBA dir fd
		if (r < 0)
		{
			errno = capturedErr;
			return -1;
		}
		return r;
	}

	case SYS_statx:
		return Shared_statx("syscall.statx", (int)(long)(uintptr_t)a1, (const char*)a2, (int)(long)(uintptr_t)a3, (unsigned int)(uintptr_t)a4, (struct statx*)a5);

	case SYS_fcntl:
	{
		int fd  = (int)(long)(uintptr_t)a1;
		int cmd = (int)(long)(uintptr_t)a2;
		long res = TRUE_WRAPPER(syscall)(number, a1, a2, a3, a4, a5, a6);
		if (res != -1 && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC))
			DuplicateFd(fd, (int)res);
		DEBUG_LOG_TRUE("syscall.fcntl", "fd=%d cmd=%d -> %ld", fd, cmd, res);
		return res;
	}

	case 425: // SYS_io_uring_setup
		errno = ENOSYS;
		return -1;

	case 435: // SYS_clone3
		UBA_NOT_IMPLEMENTED("syscall.clone3");
		break;

	case 436: // SYS_close_range
		UBA_NOT_IMPLEMENTED("syscall.close_range")
		break;

#if UBA_DEBUG_LOG_ENABLED
	default:
		DEBUG_LOG_TRUE("syscall", "%ld", number);
		break;
#endif
	}

	return TRUE_WRAPPER(syscall)(number, a1, a2, a3, a4, a5, a6);
}
#endif

// TODO: Allow multiple popen.. hacky!
FILE* g_activePopen; 
StringBuffer<64*1024>* g_activePopenResult;
u32 g_activePopenReadPos = 0;

UBA_EXPORT FILE* UBA_WRAPPER(popen)(const char* command, const char* type)
{
	UBA_INIT_DETOUR(popen, command, type);
	DEBUG_LOG_DETOURED("popen", "%s", command);

	UBA_ASSERT(!g_activePopen);
	const char* argv[] = { command, nullptr };
	g_activePopenResult = new StringBuffer<64*1024>();

	if (!ExecuteHostRun(*g_activePopenResult, argv, false))
		return nullptr;
	g_activePopenReadPos = 0;
	g_activePopen = (FILE*)1337;
	return g_activePopen;
	//return TRUE_WRAPPER(popen)(command, type);
}

UBA_EXPORT char* UBA_WRAPPER(fgets)(char* str, int count, FILE* stream)
{
	UBA_INIT_DETOUR(fgets, str, count, stream);
	DEBUG_LOG_TRUE("fgets", "(%p)", stream);
	if (stream == g_activePopen)
	{
		u32 toWrite;
		if (const char* endl = g_activePopenResult->First('\n', g_activePopenReadPos))
		{
			u32 lineLen = u32(endl - g_activePopenResult->data) - g_activePopenReadPos + 1;
			toWrite = Min(u32(count - 2), lineLen);
		}
		else
		{
			toWrite = Min(u32(count - 2), g_activePopenResult->count - g_activePopenReadPos);
		}
		memcpy(str, g_activePopenResult->data + g_activePopenReadPos, toWrite);
		str[toWrite] = 0;
		g_activePopenReadPos += toWrite;

		//DEBUG_LOG_DETOURED("fgets", "%s", str);
		return str;
	}

	auto res = TRUE_WRAPPER(fgets)(str, count, stream);
	//DEBUG_LOG_TRUE("fgets", "%s", str);
	return res;
}

UBA_EXPORT int UBA_WRAPPER(pclose)(FILE* stream)
{
	UBA_INIT_DETOUR(pclose, stream);
	if (stream == g_activePopen)
	{
		DEBUG_LOG_DETOURED("pclose", "%p", stream);
		delete g_activePopenResult;
		g_activePopenResult = nullptr;
		g_activePopen = nullptr;
		return 0;
	}

	DEBUG_LOG_TRUE("pclose", "%p", stream);
	return TRUE_WRAPPER(pclose)(stream);
}

UBA_EXPORT void UBA_WRAPPER(exit)(int status)
{
	//UBA_INIT_DETOUR(exit, status);
	DEBUG_LOG_TRUE("exit", "(%i)", status);
	//Deinit();
	//CloseCom();
	TRUE_WRAPPER(exit)(status);
}

UBA_EXPORT void UBA_WRAPPER(_exit)(int status)
{
	DEBUG_LOG_TRUE("_exit", "(%i)", status);
	Deinit();
	CloseCom();
	TRUE_WRAPPER(_exit)(status);
}

UBA_EXPORT void UBA_WRAPPER(_Exit)(int status)
{
	DEBUG_LOG_TRUE("_Exit", "(%i)", status);
	Deinit();
	CloseCom();
	TRUE_WRAPPER(_Exit)(status);
}

UBA_EXPORT int UBA_WRAPPER(system)(const char* command)
{
	UBA_INIT_DETOUR(system, command);
	DEBUG_LOG_TRUE("system", "");
	return TRUE_WRAPPER(system)(command);
}

#if PLATFORM_LINUX

UBA_EXPORT int UBA_WRAPPER(openat64)(int dirfd, const char* pathname, int flags, ...)
{
	va_list args;
	va_start(args, flags);
	int mode = va_arg(args, int);
	va_end(args);
	UBA_INIT_DETOUR(openat64, dirfd, pathname, flags, mode);

	StringBuffer<> temp;
	if (const char* resolvedPath = ResolvePath(dirfd, pathname, temp))
		return Shared_open("openat64", resolvedPath, flags, mode, [](const char* realFile, int flags, int mode) { return TRUE_WRAPPER(open64)(realFile, flags, mode); });

	UBA_ASSERT(!g_runningRemote);

	int res = TRUE_WRAPPER(openat64)(dirfd, pathname, flags, mode);
	DEBUG_LOG_TRUE("openat64", "(NODETOUR) %s -> %i", pathname, res);
	if (res)
		TrackInput(ToView(pathname));
	return res;
}

UBA_EXPORT int UBA_WRAPPER(dup3)(int oldfd, int newfd, int flags)
{
	UBA_INIT_DETOUR(dup3, oldfd, newfd, flags);
	auto res = TRUE_WRAPPER(dup3)(oldfd, newfd, flags);
	if (res != -1)
		DuplicateFd(oldfd, newfd);
	return res;
}

UBA_EXPORT pid_t UBA_WRAPPER(clone)(int (*__fn) (void *__arg), void *__child_stack, int __flags, void *__arg, ...)
{
	DEBUG_LOG_TRUE("clone", "");
	UBA_ASSERT(false);
	return -1;//TRUE_WRAPPER(clone)();
}

UBA_EXPORT char* UBA_WRAPPER(get_current_dir_name)(void)
{
	UBA_ASSERTF(false, "get_current_dir_name");
	return TRUE_WRAPPER(get_current_dir_name)();
}

#endif

// These macros need to be at the end of the file to avoid having to forward declare
// the Apple versions of these since they are all decorated with uba_<func>.
#if PLATFORM_MAC
#define DETOURED_FUNCTION(func) \
	DYLD_INTERPOSE(UBA_WRAPPER(func), func);
DETOURED_FUNCTIONS
#undef DETOURED_FUNCTION
#endif

namespace uba
{
	int GetProcessExecutablePath(tchar* Path, u32 PathSize)
	{
#if PLATFORM_LINUX
		auto res = TRUE_WRAPPER(readlink)("/proc/self/exe", Path, PathSize);
		if (res != -1)
			Path[res] = 0;
		return res;
#elif PLATFORM_MAC
		if (_NSGetExecutablePath(Path, &PathSize) == 0)
			return strlen(Path);
		return -1;
#endif
	}

	void PreInit(const char* logFile, bool fromExec)
	{
		g_fileHandlesMem.Create();
		g_fileHandlesLockMem.Create();

		g_pid = getpid();
		g_isUsingLocalBinary = !g_runningRemote;

		SuppressDetourScope s;

		const char* tmpdir = getenv("TMPDIR");
		UBA_ASSERT(tmpdir);
		g_systemTemp.Append(tmpdir);

		#if PLATFORM_LINUX
			#define DETOURED_FUNCTION(func) \
			if (!(TRUE_WRAPPER(func) = (Symbol_##func*)dlsym(RTLD_NEXT, #func))) \
				;//printf("dlsym failed on %s: %s\n", #func, dlerror());
			DETOURED_FUNCTIONS
			#undef DETOURED_FUNCTION
		#endif

		#if UBA_DEBUG_LOG_ENABLED
		if (g_logToScreen)
			g_debugFile = (FileHandle)open("/dev/tty", O_WRONLY);
		else if (logFile)
		{
			g_debugLogPath.Clear().Append(logFile);
			g_debugFile = (FileHandle)TRUE_WRAPPER(open)(logFile, (fromExec?O_APPEND:O_CREAT|O_TRUNC) | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
		}
		#endif

		StringBuffer<> exePath;
		exePath.count = GetProcessExecutablePath(exePath.data, exePath.capacity);
		UBA_ASSERTF(exePath.count > 0, "exePath.count == 0");
		char* lastSlash = strrchr(exePath.data, '/');
		UBA_ASSERTF(lastSlash, "no slash found in %s", exePath.data);
		exePath.Resize(lastSlash - exePath.data);
		FixPath(g_exeDir, exePath.data);
		g_exeDir.EnsureEndsWithSlash();

#if UBA_SUPPORTS_GO
		if (IsGoBinary("/proc/self/exe"))
		{
			DEBUG_LOG(TC("Detected Go binary — patching internal/runtime/syscall.Syscall6"));
			PatchGoSyscalls();
			PatchGoExit();
		}
#endif
	}

#if UBA_SUPPORTS_GO

	// Handle absolute paths, and relative paths anchored at AT_FDCWD.
	// For AT_FDCWD + relative, Shared_* prepends g_virtualWorkingDir via FixPath,
	// giving Go a view of CWD that matches UBA's virtualisation.  Go's physical
	// CWD often differs (agent mode spawns in a session dir), but Go must resolve
	// "../foo"-style args against the virtual workdir for file lookups to hit.
	// Real dirfds are not translated — if Go has a fd-backed dir open, we can't
	// know its virtual path, so fall through to the raw syscall.
	static inline bool GoHandlesPath(int dirfd, const char* path)
	{
		if (!path || !*path)
			return false;
		if (path[0] == '/')
			return true;
		return dirfd == AT_FDCWD;
	}

	bool GoHandleOpenat(int dirfd, const char* path, int flags, int mode, long* result, int* errOut)
	{
		if (!GoHandlesPath(dirfd, path))
			return false;
		int capturedErr = 0;
		*result = Shared_open("openat(go)", path, flags, mode,
			[&capturedErr](const char* realFile, int flg, int mod) -> int {
				RawSyscallResult r = RawSyscallDirect(SYS_openat, AT_FDCWD, (long)realFile, flg, mod);
				if (r.r1 < 0 && r.r1 >= -4095) { capturedErr = (int)(-r.r1); return -1; }
				return (int)r.r1;
			});
		*errOut = (*result < 0) ? capturedErr : 0;
		return true;
	}

	bool GoHandleNewfstatat(int dirfd, const char* path, struct stat* buf, int flags, long* result, int* errOut)
	{
		if (!GoHandlesPath(dirfd, path))
			return false;
		// Use a raw syscall in the lambda to avoid glibc writing to errno TLS.
		// On goroutine OS threads the FS segment points at Go's M struct, not a glibc TCB,
		// so __errno_location() returns a wrong address — writing errno would corrupt the runtime.
		// We capture the kernel error code from the syscall return value instead.
		int capturedErr = 0;
		*result = Shared_stat("newfstatat(go)", path, buf,
			[&capturedErr](const char* realPath, struct stat* outBuf) -> int {
				RawSyscallResult r = RawSyscallDirect(SYS_newfstatat, AT_FDCWD, (long)realPath, (long)outBuf, 0);
				if (r.r1 < 0 && r.r1 >= -4095) { capturedErr = (int)(-r.r1); return -1; }
				return (int)r.r1;
			});
		*errOut = (*result < 0) ? capturedErr : 0;
		return true;
	}

	bool GoHandleStatx(int dirfd, const char* path, int flags, unsigned int mask, struct statx* statxbuf, long* result, int* errOut)
	{
		if (!GoHandlesPath(dirfd, path))
			return false;
		// Shared_statx currently requires dirfd == AT_FDCWD; pass AT_FDCWD so that
		// Shared_statx's FixPath resolves relative paths against g_virtualWorkingDir.
		*result = Shared_statx("statx(go)", AT_FDCWD, path, flags, mask, statxbuf);
		// Shared_statx internally uses raw syscalls already; read errno only if glibc TLS valid.
		*errOut = (*result < 0) ? errno : 0;
		return true;
	}

	bool GoHandleUnlinkat(int dirfd, const char* path, int flags, long* result, int* errOut)
	{
		if (flags & AT_REMOVEDIR)
			return false; // directory removal — not a tracked file operation
		if (!GoHandlesPath(dirfd, path))
			return false;
		*result = Shared_DeleteFile("unlinkat(go)", path);
		*errOut = (*result < 0) ? errno : 0;
		return true;
	}

	bool GoHandleClose(int fd, long* result, int* errOut)
	{
		// Only intercept fds that UBA tracks.  Go opens many fds itself (sockets,
		// pipes, os.Stdin/out/err) that were never registered with Shared_open;
		// those must pass through to the kernel unchanged so we don't disturb
		// Go's runtime.  If the fd isn't in g_fileHandles, leave it to the kernel.
		{
			SCOPED_READ_LOCK(g_fileHandlesLock, lock);
			if (g_fileHandles.find(fd) == g_fileHandles.end())
				return false;
		}
		*result = Shared_close("close(go)", fd, [fd]() {
			RawSyscallResult r = RawSyscallDirect(SYS_close, fd, 0, 0);
			if (r.r1 < 0 && r.r1 >= -4095) { errno = (int)(-r.r1); return -1; }
			return (int)r.r1;
		});
		*errOut = (*result < 0) ? errno : 0;
		return true;
	}

	bool GoHandleGetdents64(int fd, void* buf, unsigned int count, long* result, int* errOut)
	{
		// Only intercept fds UBA knows about AND that hold a DirInfo (i.e. came from
		// Shared_open's $d placeholder branch).  Regular file fds and untracked fds
		// pass through to the kernel's getdents64 — which returns ENOTDIR for files.
		int capturedErr = 0;
		long r = ::Shared_getdents64("getdents64(go)", fd, buf, count, &capturedErr);
		if (r < 0 && capturedErr == 0)
			return false; // fd not a UBA-tracked dir; caller falls through
		if (r < 0)
		{
			*result = -1;
			*errOut = capturedErr;
		}
		else
		{
			*result = r;
			*errOut = 0;
		}
		return true;
	}
#endif // PLATFORM_LINUX

	void LogHeader()
	{
		#if UBA_DEBUG_LOG_ENABLED
		static StringBuffer<128 * 1024> buf;
		buf.Clear();
		#if PLATFORM_LINUX
		int fd = TRUE_WRAPPER(open)("/proc/self/cmdline", O_RDONLY);
		if (fd != -1)
		{
			auto bufSize = read(fd, buf.data, buf.capacity);
			if (bufSize != -1)
			{
				char* it = buf.data;
				while (*it)
				{
					it += strlen(it);
					*it = ' ';
					++it;
				}
				buf.Resize(it - buf.data - 1);
			}
			TRUE_WRAPPER(close)(fd);
		}
		#else
		char** argv = *_NSGetArgv();
		for (int i=0, e=*_NSGetArgc(); i<e; ++i)
		{
			if (i != 0)
				buf.Append(' ');
			buf.Append(argv[i]);
		}
		#endif
		LogHeader(buf);
		#endif
	}

	void Init()
	{
		UBA_ASSERTF(!g_isInitialized, "Already initialized");
		g_isInitialized = true;

		bool trackInputs = false;
		FileMappingHandle directoryTableHandle;
		DirTableSize directoryTableSize;
		u32 directoryTableCount;
		FileMappingHandle mappedFileTableHandle;
		u32 mappedFileTableSize;
		u32 mappedFileTableCount;
		FileMappingHandle overlayTableHandle;

		{
			RPC_MESSAGE(Init, init);
			BinaryReader reader = writer.Flush();

			g_processId = reader.ReadU32();
			g_isChild = reader.ReadBool();
			trackInputs = reader.ReadBool();

			reader.ReadString(g_virtualApplication);
			reader.ReadString(g_virtualWorkingDir);

			g_sharedMemoryAllocator = SharedMemoryAllocatorHandle::FromU64(reader.ReadU64());
			reader.ReadU64(); // permanent files memory handle
			directoryTableHandle = FileMappingHandle::FromU64(reader.ReadU64());
			directoryTableSize = FromU64(reader.ReadU64());
			directoryTableCount = reader.ReadU32();
			mappedFileTableHandle = FileMappingHandle::FromU64(reader.ReadU64());
			mappedFileTableSize = reader.ReadU32();
			mappedFileTableCount = reader.ReadU32();
			overlayTableHandle = FileMappingHandle::FromU64(reader.ReadU64());

			if (u16 vfsSize = reader.ReadU16())
			{
				BinaryReader vfsReader(reader.GetPositionData(), 0, vfsSize);
				PopulateVfs(vfsReader);
			}

			DEBUG_LOG_PIPE(L"Init", L"");
		}

		VirtualizePath(g_virtualApplication);
		VirtualizePath(g_virtualWorkingDir);
		VirtualizePath(g_exeDir);

		UBA_ASSERTF(g_virtualApplicationDir.capacity > 0, "g_virtualApplicationDir.capacity > 0");

		const char* lastSlash = strrchr(g_virtualApplication.data, '/');
		UBA_ASSERTF(lastSlash, "Need fullpath for application (%s)", g_virtualApplication.data);
		g_virtualApplicationDir.Append(g_virtualApplication.data, lastSlash - g_virtualApplication.data + 1);

		if (g_virtualWorkingDir[g_virtualWorkingDir.count-1] == PathSeparator)
			g_virtualWorkingDir.Resize(g_virtualWorkingDir.count-1);
		setenv("PWD", g_virtualWorkingDir.data, 1);
		g_virtualWorkingDir.EnsureEndsWithSlash();

		if (g_runningRemote && g_virtualApplicationDir.StartsWith(g_exeDir)) // Check if we are using local binary (which means dlopen should also stay local)
			g_isUsingLocalBinary = true;


		LogHeader();

		TimerScope ts1(g_stats.fileTable);
		u8* mappedFileTableMem = FileMapping_MapFromHost(mappedFileTableHandle, FileMappingTableMaxSize,0, false, "FileTable");
		g_mappedFileTable.Init(mappedFileTableMem, mappedFileTableCount, mappedFileTableSize);
		ts1.Leave();

		TimerScope ts2(g_stats.dirTable);
		u8* dirTableMem = FileMapping_MapFromHost(directoryTableHandle, DirTableMaxSize,0, false, "DirTable");
		u8* overlayTableMem = FileMapping_MapFromHost(overlayTableHandle, OverlayTableMaxSize,0, false, "OverlayTable");
		g_directoryTable.Init(dirTableMem, directoryTableCount, directoryTableSize.main);
		g_directoryTable.InitOverlay(overlayTableMem , directoryTableSize.overlay);
		ts2.Leave();

		if (trackInputs)
			InitTrackInput();

		TrackInput(g_virtualApplication);

		if (g_isChild)
			Rpc_GetWrittenFiles();

		//pthread_atfork([]()
		//{
		//	DEBUG_LOG("FORKING!!");
		//},[](){},[](){});


		LogVfsInfo();

		g_isDetouring = true;
		DEBUG_LOG("Detouring enabled");

		#if 0
		char** envs = environ;
		while (char* env = *envs++)
		{
			DEBUG_LOG("  ENV %s", env);
		}
		#endif

	}

	void Deinit()
	{
		if (!g_isInitialized)
			return;
		
		g_isInitialized = false;
		g_isDetouring = false;

		if (!g_isCancelled)
		{
			// Flush every stdio stream before we fstat/close the underlying fds.
			// Programs that freopen stdout/stderr onto real output files (e.g.
			// wayland_scanner) never call fclose — they return from main() and
			// rely on libc's atexit handler to flush. If we measure and close
			// the fd before that happens, the trailing stdio buffer (up to
			// BUFSIZ bytes) is invisible to fstat and lost on close, truncating
			// the output to the last fully-flushed page.
			fflush(nullptr);

			SCOPED_WRITE_LOCK(g_fileHandlesLock, lock);
			for (auto& kv : g_fileHandles)
			{
				int fd = kv.first;
				DetouredHandle& h = kv.second;
				FileObject* fo = h.fileObject;
				if (fo->closeId)
				{
					FileInfo& fi = *fo->fileInfo;
					SharedMemoryHandle memoryHandle;
					struct stat attr;
					int res = TRUE_WRAPPER(fstat)(fd, &attr);
					UBA_ASSERTF(res == 0, "fstat failed for file %s", fi.name);(void)res;
					u64 fileSize = attr.st_size;
					u64 fileLastWriteTime = FromTimeSpec(attr.st_mtimespec);
					u32 fileAttributes = attr.st_mode;
					const tchar* path = fi.name;
					Rpc_UpdateCloseHandle(path, fo->closeId, fo->deleteOnClose, fo->newName.c_str(), memoryHandle, fileSize, fileLastWriteTime, fileAttributes, true);
				}
				TRUE_WRAPPER(close)(fd);
			}
		}

		SendInput();

		g_stats.detoursMemory = g_memoryBlock.writtenSize;

		RPC_MESSAGE(Exit, log);
		writer.WriteU32(0); // Exit code
		writer.WriteString(""); // Log name
		g_stats.Write(writer);
		g_kernelStats.Write(writer);

		// This can't wait for response since the session process might move on and reuse shared memory with someone else
		// Note, if we start using memory mapped files we need to change this to true for child processes since Exit message is writing files to disk..
		// .. and if we don't wait to exit this process until those files are written we might end up in a race condition with the parent using those files
		writer.Flush(false);

		#if UBA_DEBUG_LOG_ENABLED
		if (isLogging())
		{
			DEBUG_LOG("Finished");
			int debugFile = (int)g_debugFile;
			g_debugFile = InvalidFileHandle;
			TRUE_WRAPPER(close)(debugFile);
		}
		#endif
	}

	#if UBA_DEBUG_LOG_ENABLED
	void WriteDebug(const void* data, u32 dataLen)
	{
		int t = errno;

		#if UBA_DETOUR_DEBUG && PLATFORM_LINUX
		TRUE_WRAPPER(write)(g_debugFile, data, dataLen);
		#else
		write(g_debugFile, data, dataLen);
		#endif
		//fsync(g_debugFile);
		errno = t;
	}
	void FlushDebugLog()
	{
		if (isLogging())
			fsync(g_debugFile);
	}
	#endif

	ANALYSIS_NORETURN void UbaAssert(const tchar* text, const char* file, u32 line, const char* expr, bool allowTerminate, u32 terminateCode, void* context, u32 skipCallstackCount)
	{
		SuppressDetourScope s;
		static CriticalSection cs;
		ScopedCriticalSection scs(cs);

		#if UBA_DEBUG_LOG_ENABLED
		FlushDebugLog();
		#endif

		static auto& sb =  *new StringBuffer<8*1024>;
		WriteAssertInfo(sb.Clear(), text, file, line, expr, context);

		if (g_isInitialized)
		{
			Rpc_ResolveCallstack(sb, 3 + skipCallstackCount, context);
			Rpc_WriteLog(sb.data, sb.count, true, true);

			if (!allowTerminate)
				return;

			g_stats.detoursMemory = g_memoryBlock.writtenSize;

			if (TakeLockForRpc())
			{
				RPC_MESSAGE_NO_LOCK(Exit, log);
				writer.WriteU32(terminateCode); // Exit code
				writer.WriteString(""); // Log name
				g_stats.Write(writer);
				g_kernelStats.Write(writer);
				writer.Flush(false);
				g_communicationLock.Leave();
			}
		}
		else
		{
			#if UBA_DEBUG_LOG_ENABLED
			g_debugFile = (FileHandle)open("/dev/tty", O_WRONLY);
			sb.Append('\n');
			WriteDebug(sb.data, sb.count);
			#endif
		}

		CloseCom();
		TRUE_WRAPPER(_exit)(int(terminateCode));
	}

	LastErrorToText::LastErrorToText(u32 lastError)
	{
		Append(strerror(int(lastError)));
	}
}

extern "C"
{
	UBA_EXPORT bool UbaRequestNextProcess2(u32 prevExitCode, char* outArguments, u32 outArgumentsCapacity, u32 timeOutMs, bool* outShouldExit)
	{
		#if UBA_DEBUG_LOG_ENABLED
		FlushDebugLog();
		#endif

		*outArguments = 0;
		bool newProcess;
		DirTableSize directoryTableSize;
		{
			RPC_MESSAGE(GetNextProcess, log);
			writer.WriteU32(prevExitCode);
			writer.WriteU32(timeOutMs);
			g_stats.Write(writer);
			g_kernelStats.Write(writer);

			BinaryReader reader = writer.Flush();
			newProcess = reader.ReadBool();
			*outShouldExit = reader.ReadBool();
			if (newProcess)
			{
				reader.ReadString(outArguments, outArgumentsCapacity);
				reader.SkipString(); // workingDir
				reader.SkipString(); // description
				reader.ReadString(g_logName.Clear());
				directoryTableSize = FromU64(reader.ReadU64());
			}
		}

		g_kernelStats = {};
		g_stats = {};

		if (newProcess)
		{
			#if UBA_DEBUG_LOG_ENABLED
			SuppressDetourScope scope;
			int debugFile = g_debugFile;
			g_debugFile = InvalidFileHandle;
			close(debugFile);
			debugFile = TRUE_WRAPPER(open)(g_logName.data, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
			g_debugFile = (FileHandle)debugFile;
			LogHeader(ToView(outArguments));
			#endif

			const u8* overlayMemory = g_directoryTable.m_overlayMemory;
			g_directoryTable.DeinitOverlay();
			g_directoryTable.m_overlayMemory = overlayMemory;

			if (u32 overlaySize = directoryTableSize.overlay)
			{
				directoryTableSize.overlay = 0; // Want to just parse normal directory first
				g_directoryTable.ParseDirectoryTableNoLock(directoryTableSize);
				g_directoryTable.InitOverlay(overlayMemory, overlaySize);
			}
		}

		Rpc_UpdateTables();
		return newProcess;
	}

	UBA_EXPORT bool UbaRequestNextProcess(u32 prevExitCode, char* outArguments, u32 outArgumentsCapacity)
	{
		bool shouldExit;
		return UbaRequestNextProcess2(prevExitCode, outArguments, outArgumentsCapacity, 0, &shouldExit);
	}
}
