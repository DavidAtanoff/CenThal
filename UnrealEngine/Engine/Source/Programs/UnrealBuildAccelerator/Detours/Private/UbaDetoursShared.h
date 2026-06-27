// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaApplicationRules.h"
#include "UbaFileHandle.h"
#include "UbaStringBuffer.h"
#include "UbaPlatform.h"
#include "UbaProcessStats.h"
#include "UbaProtocol.h"
#if UBA_STUB_BUILD
// The stub build needs complete types for the shared-global macros below so
// `*reinterpret_cast<T*>(g_xxxMem.data)` expands to well-formed code at every
// call site. Include all but FileMappingTable here (that one transitively
// includes us via UbaDetoursShared.h; any TU that uses g_mappedFileTable must
// include UbaDetoursFileMappingTable.h before reaching the call site, which
// the stub and UbaDetoursShared.cpp itself already do).
#include "UbaMemory.h"
#include "UbaSynchronization.h"
#include "UbaDirectoryTable.h"
#endif

namespace uba
{
	class DirectoryTable;
	class MappedFileTable;

	#if PLATFORM_WINDOWS
	DWORD Local_GetLongPathNameW(LPCWSTR lpszShortPath, LPWSTR lpszLongPath, DWORD cchBuffer);
	#endif

	void Rpc_WriteLog(const tchar* text, u64 textCharLength, bool printInSession, bool isError);
	void Rpc_WriteLogf(const tchar* format, ...);
	void Rpc_ResolveCallstack(StringBufferBase& out, u32 skipCallstackCount, void* context);

	const tchar* GetApplicationShortName();
	bool FixPath(StringBufferBase& out, const tchar* path, u32 pathLen = ~0u);
	bool FixPath(StringBufferBase& out, const StringView path);

	void PopulateVfs(BinaryReader& vfsReader);
	bool IsVfsEnabled();
	bool DevirtualizePath(StringBufferBase& path);
	tchar DevirtualizeDrive(tchar drive);
	bool VirtualizePath(StringBufferBase& path);
	void LogHeader(StringView cmdLine);
	void LogVfsInfo();

	#if UBA_DEBUG_LOG_ENABLED
		// Use ::uba:: qualification so the macros expand correctly at any
		// scope (the stub build invokes DEBUG_LOG from extern "C" callbacks
		// outside namespace uba).
		#define DEBUG_LOG_PREFIX(Prefix, Command, fmt, ...) \
			::uba::LogScope STRING_JOIN(ls, __LINE__); \
			UBA_FMT_CHECK(fmt, ##__VA_ARGS__); \
			if (::uba::isLogging()) ::uba::WriteDebugLogWithPrefix(#Prefix, STRING_JOIN(ls, __LINE__), Command, fmt, ##__VA_ARGS__); \

		#define DEBUG_LOG_TRUE(Command, ...) DEBUG_LOG_PREFIX(T, Command, __VA_ARGS__)
		#define DEBUG_LOG_DETOURED(Command, ...) DEBUG_LOG_PREFIX(D, Command, __VA_ARGS__)
		#define DEBUG_LOG_PROXY(Command, ...) DEBUG_LOG_PREFIX(P, Command, __VA_ARGS__)
		#define DEBUG_LOG_PIPE(Command, ...) ts.Leave();
		#define DEBUG_LOG(fmt, ...) { UBA_FMT_CHECK(fmt, ##__VA_ARGS__); if (::uba::isLogging()) ::uba::WriteDebugLog(fmt, ##__VA_ARGS__); }
	#else
		#define DEBUG_LOG(...) {}
		#define DEBUG_LOG_DETOURED(Command, ...) {}
		#define DEBUG_LOG_TRUE(Command, ...) {}
		#define DEBUG_LOG_PROXY(Command, ...) {}
		#define DEBUG_LOG_PIPE(...) ts.Leave();
	#endif

#if !UBA_STUB_BUILD
	// Normal build: each global is a reference backed by a pointer slot that
	// the dynamic linker fills in at load time via R_X86_64_RELATIVE.
	extern StringBuffer<512>& g_virtualApplication;
	extern StringBuffer<512>& g_virtualApplicationDir;
	extern StringBuffer<256>& g_exeDir;
	extern StringBuffer<512>& g_logName;

	extern StringBuffer<512>& g_virtualWorkingDir;
	extern StringBuffer<256>& g_systemTemp;
	extern StringBuffer<128>& g_systemRoot;

	extern ProcessStats& g_stats;
	extern KernelStats& g_kernelStats;
	extern Futex& g_communicationLock;
	extern MemoryBlock& g_memoryBlock;
	extern DirectoryTable& g_directoryTable;
	extern MappedFileTable& g_mappedFileTable;
#endif
	// Stub-build declarations for the same globals live further down, after
	// VariableMem itself is defined (can't take `.data` on a forward decl).
	extern bool g_conEnabled[2]; // 0=stderr, 1=stdout

	// TLS-free per-thread suppression state.  Goroutine OS threads created via
	// clone() do not have a glibc TCB set up, so any thread_local access via
	// __tls_get_addr crashes on them.  To keep the LD_PRELOAD detour layer
	// usable by such threads (and by a future freestanding static-detour stub),
	// suppression depth is tracked in a tiny open-addressed array keyed by the
	// current OS thread id obtained without touching TLS.
	u32  DisallowDetourDepth();
	void DisallowDetourIncrement();
	void DisallowDetourDecrement();

	struct SuppressDetourScope
	{
		SuppressDetourScope();
		~SuppressDetourScope();
	};

	// Back-compat shim so existing `t_disallowDetour` readers (checks of the form
	// `if (t_disallowDetour)` or comparisons to 0) keep working without rewriting
	// every callsite.  `t_disallowDetour != 0` is equivalent to
	// `DisallowDetourDepth() != 0` for the current thread.
	#define t_disallowDetour uba::DisallowDetourDepth()

	#if UBA_DEBUG_LOG_ENABLED
	inline constexpr u32 LogBufSize = 16 * 1024;
	extern FileHandle g_debugFile;
	extern StringBuffer<MaxPath>& g_debugLogPath;
	inline bool isLogging() { return g_debugFile != InvalidFileHandle; }
	struct LogScope { LogScope(); ~LogScope(); void Flush(); };
	void WriteDebugLogWithPrefix(const char* prefix, LogScope& scope, const tchar* command, const tchar* format, ...);
	void WriteDebugLog(const tchar* format, ...);
	void FlushDebugLog();
	#endif
	#if UBA_DEBUG_VALIDATE
	extern bool g_validateFileAccess;
	#endif

	inline constexpr bool g_allowDirectoryCache = true;
	inline constexpr bool g_allowFileMappingDetour = true;
	inline constexpr bool g_allowFindFileDetour = true;
	inline constexpr bool g_allowListDirectoryHandle = true;

	extern u32 g_processId;
	extern u32 g_rulesIndex;
	extern ApplicationRules* g_rules;
	extern bool g_runningRemote;
	extern bool g_isChild;
	extern bool g_allowKeepFilesInMemory;
	extern bool g_allowOutputFiles;
	extern bool g_allowDiscardVirtualMemory;
	extern bool g_suppressLogging;

	#if PLATFORM_WINDOWS
	constexpr u32 ErrorSuccess = 0;
	constexpr u32 ErrorFileNotFound = ERROR_FILE_NOT_FOUND;
	using FileAttributesData = WIN32_FILE_ATTRIBUTE_DATA;
	#else
	using FileAttributesData = struct stat;
	constexpr u32 ErrorSuccess = 0;
	constexpr u32 ErrorFileNotFound = ENOENT;
	#endif


	struct FileAttributes
	{
		FileAttributesData data;
		u64 fileIndex;
		u32 volumeSerial;
		u8 exists;
		u32 lastError;
	};

	bool CouldBeCompressedFile(const StringView& fileName);

	void Shared_WriteConsole(const char* chars, u32 charCount, bool isError);
	void Shared_WriteConsole(const wchar_t* chars, u32 charCount, bool isError);

	const tchar* Shared_GetFileAttributes(FileAttributes& outAttr, StringView fileName, bool checkIfDir = false);

	void InitSharedVariables();
	void InitTrackInput();
	void TrackInput(StringView file);
	void SkipTrackInput(StringView file);
	bool IsTrackingInput();
	void SendInput();

	template<typename T> struct VariableMem { template<typename... Args> void Create(Args&&... args) { new (data) T(args...); }; u64 data[AlignUp(sizeof(T), sizeof(u64)) / sizeof(u64)]; };
#if UBA_STUB_BUILD
	// Stub build: emit storage only. The corresponding `g_name` is a macro
	// (below) expanding to an always-inline accessor that materialises a
	// reference from the storage via RIP-relative addressing — no pointer
	// slot, hence no R_X86_64_RELATIVE relocation that the objcopied blob
	// couldn't apply.
	#define VARIABLE_MEM(type, name) VariableMem<type> name##Mem;

	// Non-templated accessors — the return type is void* so the declaration
	// doesn't need any of the shared-globals' complete types in scope.
	// Callers cast through the `g_xxx` macros below, which are expanded at
	// use sites where the types are always visible.
	void* g_virtualApplicationStorage();
	void* g_virtualApplicationDirStorage();
	void* g_exeDirStorage();
	void* g_logNameStorage();
	void* g_virtualWorkingDirStorage();
	void* g_systemTempStorage();
	void* g_systemRootStorage();
	void* g_statsStorage();
	void* g_kernelStatsStorage();
	void* g_communicationLockStorage();
	void* g_memoryBlockStorage();
	void* g_directoryTableStorage();
	void* g_mappedFileTableStorage();
	#if UBA_DEBUG_LOG_ENABLED
	void* g_debugLogPathStorage();
	void* g_debugLogMutexStorage();
	#endif

	// Redirect every bare reference `g_xxx` to a cast of the storage accessor.
	// The preprocessor pass runs before name lookup, so every mention in
	// shared UBA sources becomes a cast expression that the compiler lowers
	// to a RIP-relative lea + deref — no pointer slot, no relocation.
	// Fully-qualified type names so the macros work from any namespace.
	#define g_virtualApplication     (*static_cast<::uba::StringBuffer<512>*>(::uba::g_virtualApplicationStorage()))
	#define g_virtualApplicationDir  (*static_cast<::uba::StringBuffer<512>*>(::uba::g_virtualApplicationDirStorage()))
	#define g_exeDir                 (*static_cast<::uba::StringBuffer<256>*>(::uba::g_exeDirStorage()))
	#define g_logName                (*static_cast<::uba::StringBuffer<512>*>(::uba::g_logNameStorage()))
	#define g_virtualWorkingDir      (*static_cast<::uba::StringBuffer<512>*>(::uba::g_virtualWorkingDirStorage()))
	#define g_systemTemp             (*static_cast<::uba::StringBuffer<256>*>(::uba::g_systemTempStorage()))
	#define g_systemRoot             (*static_cast<::uba::StringBuffer<128>*>(::uba::g_systemRootStorage()))
	#define g_stats                  (*static_cast<::uba::ProcessStats*>(::uba::g_statsStorage()))
	#define g_kernelStats            (*static_cast<::uba::KernelStats*>(::uba::g_kernelStatsStorage()))
	#define g_communicationLock      (*static_cast<::uba::Futex*>(::uba::g_communicationLockStorage()))
	#define g_memoryBlock            (*static_cast<::uba::MemoryBlock*>(::uba::g_memoryBlockStorage()))
	#define g_directoryTable         (*static_cast<::uba::DirectoryTable*>(::uba::g_directoryTableStorage()))
	#define g_mappedFileTable        (*static_cast<::uba::MappedFileTable*>(::uba::g_mappedFileTableStorage()))
	#if UBA_DEBUG_LOG_ENABLED
	#define g_debugLogPath           (*static_cast<::uba::StringBuffer<::uba::MaxPath>*>(::uba::g_debugLogPathStorage()))
	#define g_debugLogMutex          (*static_cast<::uba::Futex*>(::uba::g_debugLogMutexStorage()))
	#endif
#else
	#define VARIABLE_MEM(type, name) VariableMem<type> name##Mem; type& name = (type&)name##Mem.data;
#endif

	#define RPC_MESSAGE(messageName, timerName) \
		RPC_MESSAGE2(MessageType_##messageName, #messageName, timerName)

	#define RPC_MESSAGE2(messageType, messageName, timerName) \
		DEBUG_LOG(TC("RPC_MESSAGE %s"), TC(messageName)); \
		TimerScope ts(g_stats.timerName); \
		SCOPED_FUTEX(g_communicationLock, pcs); \
		BinaryWriter writer(ProcessCommunication_Write); \
		writer.WriteByte(messageType);

	#define RPC_MESSAGE_NO_LOCK(messageName, timerName) \
		DEBUG_LOG(TC("RPC_MESSAGE %s"), TC(#messageName)); \
		TimerScope ts(g_stats.timerName); \
		BinaryWriter writer(ProcessCommunication_Write); \
		writer.WriteByte(MessageType_##messageName);

	#define RPC_MESSAGE_END \
		ts.Leave(); \
		pcs.Leave();

	// Dummy code
	enum LogEntryType : u8 { LogEntryType_Error = 0, LogEntryType_Warning = 1, };
	class Logger { public: void Logf(...) {} void Info(...) {} bool Warning(...) { return false; } bool Error(...) { return false; } };
}
