// Copyright Epic Games, Inc. All Rights Reserved.

#define UBA_IS_DETOURED_INCLUDE 1

#include "UbaDetoursShared.h"
#include "UbaDetoursFileMappingTable.h"
#include "UbaDirectoryTable.h"
#include "UbaTimer.h"

#include <atomic>

#if PLATFORM_MAC
#include <locale.h>
#include <pthread.h>
#include <mach/mach_init.h>
#endif

namespace uba
{
	constexpr u32 TrackInputsMemCapacity = 512 * 1024;

	VARIABLE_MEM(StringBuffer<512>, g_virtualApplication);
	VARIABLE_MEM(StringBuffer<512>, g_virtualApplicationDir);
	VARIABLE_MEM(ProcessStats, g_stats);
	VARIABLE_MEM(KernelStats, g_kernelStats);
	VARIABLE_MEM(Futex, g_communicationLock);
	VARIABLE_MEM(StringBuffer<512>, g_logName);
	VARIABLE_MEM(StringBuffer<512>, g_virtualWorkingDir);
	VARIABLE_MEM(StringBuffer<256>, g_exeDir);
	VARIABLE_MEM(StringBuffer<128>, g_systemRoot);
	VARIABLE_MEM(StringBuffer<256>, g_systemTemp);
	VARIABLE_MEM(MemoryBlock, g_memoryBlock);
	VARIABLE_MEM(DirectoryTable, g_directoryTable);
	VARIABLE_MEM(MappedFileTable, g_mappedFileTable);
	VARIABLE_MEM(ReaderWriterLock, g_consoleStringCs);
	VARIABLE_MEM(Futex, g_trackInputsLock);

	#if UBA_DEBUG_LOG_ENABLED
	VARIABLE_MEM(StringBuffer<MaxPath>, g_debugLogPath);
	VARIABLE_MEM(Futex, g_debugLogMutex);
	#endif

	bool g_conEnabled[2];
	u32 g_rulesIndex;
	ApplicationRules* g_rules;
	bool g_runningRemote;
	bool g_isChild;
	bool g_allowKeepFilesInMemory = IsWindows;
	bool g_allowOutputFiles = IsWindows;
	bool g_allowDiscardVirtualMemory;
	bool g_suppressLogging = false;
	u32 g_vfsEntryCount;
	u32 g_vfsMatchingLength;
	u8* g_trackInputsMem;
	u32 g_trackInputsBufPos;

#if UBA_STUB_BUILD
	// See the comment above the declarations in UbaDetoursShared.h.
	// Each accessor returns a pointer to the VariableMem storage. The body is
	// `&g_xxxMem.data` — compiled as a RIP-relative lea against the stub's own
	// .text-embedded storage, no relocation needed. Callers (via the `g_xxx`
	// macros in the header) cast the void* to the underlying type.
	void* g_virtualApplicationStorage()     { return g_virtualApplicationMem.data; }
	void* g_virtualApplicationDirStorage()  { return g_virtualApplicationDirMem.data; }
	void* g_exeDirStorage()                 { return g_exeDirMem.data; }
	void* g_logNameStorage()                { return g_logNameMem.data; }
	void* g_virtualWorkingDirStorage()      { return g_virtualWorkingDirMem.data; }
	void* g_systemTempStorage()             { return g_systemTempMem.data; }
	void* g_systemRootStorage()             { return g_systemRootMem.data; }
	void* g_statsStorage()                  { return g_statsMem.data; }
	void* g_kernelStatsStorage()            { return g_kernelStatsMem.data; }
	void* g_communicationLockStorage()      { return g_communicationLockMem.data; }
	void* g_memoryBlockStorage()            { return g_memoryBlockMem.data; }
	void* g_directoryTableStorage()         { return g_directoryTableMem.data; }
	void* g_mappedFileTableStorage()        { return g_mappedFileTableMem.data; }
	#if UBA_DEBUG_LOG_ENABLED
	void* g_debugLogPathStorage()           { return g_debugLogPathMem.data; }
	void* g_debugLogMutexStorage()          { return g_debugLogMutexMem.data; }
	#endif
#endif

	void InitSharedVariables()
	{
		g_conEnabled[0] = true;
		g_conEnabled[1] = true;
		g_virtualApplicationMem.Create();
		g_virtualApplicationDirMem.Create();
		g_statsMem.Create();
		g_kernelStatsMem.Create();
		g_communicationLockMem.Create();
		g_logNameMem.Create();
		g_virtualWorkingDirMem.Create();
		g_exeDirMem.Create();
		g_systemRootMem.Create();
		g_systemTempMem.Create();

		#if UBA_DEBUG_LOG_ENABLED
		g_debugLogPathMem.Create();
		g_debugLogMutexMem.Create();
		#endif

		g_vfsEntryCount = 0;
		g_vfsMatchingLength = 0;

		u64 reserveSizeMb = IsWindows ? 256 : 1024; // The sync primitives on linux/macos is much bigger
		g_memoryBlockMem.Create(TC("GlobalDetour"), reserveSizeMb * 1024 * 1024);
		g_directoryTableMem.Create(g_memoryBlock);
		g_mappedFileTableMem.Create(g_memoryBlock);
		g_consoleStringCsMem.Create();

		g_trackInputsLockMem.Create();
	}

#if UBA_DEBUG_LOG_ENABLED
	FileHandle g_debugFile = InvalidFileHandle;
	void WriteDebug(const void* data, u32 dataLen);

	// Process-global scratch buffer used to format each DEBUG_LOG line.  Was
	// previously per-thread (thread_local t_a / t_b / t_b_size / t_logScopeCount)
	// but TLS can't be accessed from goroutine OS threads that lack a glibc TCB.
	// Serialising all log formatting through one mutex (g_debugLogMutex, declared
	// via VARIABLE_MEM alongside the other shared globals above) is fine — debug
	// logging is already the slow path, and perfectly-coherent per-thread line
	// grouping is a convenience, not a correctness requirement. Going through
	// VARIABLE_MEM + explicit Create() in InitSharedVariables() (rather than a
	// plain `static Futex`) keeps the init timing deterministic across normal,
	// LD_PRELOAD, and freestanding-stub builds; the stub has no .init_array to
	// run ReaderWriterLock's ctor.
	static char       g_debugLogBuf[LogBufSize];

	void GetPrefixExtra(StringBufferBase& out)
	{
		#if 0
		static u64 startTime = GetTime();
		u64 timeMs = TimeToMs(GetTime() - startTime);
		u64 ms = timeMs % 1000;
		u64 s = timeMs / 1000;

		out.Appendf(TC("[%5llu.%03llu]"), s, ms);
		#endif

		//out.Appendf(TC("[%i]"), getpid());
		//out.Appendf(TC("[%7u]"), GetCurrentThreadId());
	}

	void WriteDebugLogWithPrefix(const char* prefix, LogScope& scope, const tchar* command, const tchar* format, ...)
	{
		#if PLATFORM_MAC
		static locale_t safeLocale = newlocale(LC_NUMERIC_MASK, "C", duplocale(LC_GLOBAL_LOCALE));
		locale_t oldLocale = uselocale(safeLocale);
		#endif

		StringBuffer<LogBufSize> line;
		line.Append(command).Append(' ');
		if (*format)
		{
			va_list arg;
			va_start(arg, format);
			line.Append(format, arg);
			va_end(arg);
		}
		line.Append(TCV("\n"));

		StringBuffer<128> extra;
		GetPrefixExtra(extra);

		SCOPED_FUTEX(g_debugLogMutex, lock);
		#if PLATFORM_WINDOWS
		int res__ = sprintf_s(g_debugLogBuf, LogBufSize, "%s %S   %S", prefix, extra.data, line.data);
		#else
		int res__ = snprintf(g_debugLogBuf, LogBufSize, "%s %s   %s", prefix, extra.data, line.data);
		#endif
		if (res__ > 0)
			WriteDebug(g_debugLogBuf, (u32)res__);

		#if PLATFORM_MAC
		uselocale(oldLocale);
		#endif
	}

	void WriteDebugLog(const tchar* format, ...)
	{
		StringBuffer<LogBufSize> line;
		if (*format)
		{
			va_list arg;
			va_start(arg, format);
			line.Append(format, arg);
			va_end(arg);
		}
		line.Append(TCV("\n"));

		SCOPED_FUTEX(g_debugLogMutex, lock);
		#if PLATFORM_WINDOWS
		int res__ = sprintf_s(g_debugLogBuf, LogBufSize, "%S", line.data);
		if (res__ > 0)
			WriteDebug(g_debugLogBuf, (u32)res__);
		#else
		WriteDebug(line.data, line.count);
		#endif
	}

	// LogScope used to group multi-line output per thread.  With the global
	// buffer we no longer need reentrant counting or a deferred flush — each
	// WriteDebugLog* call writes its line immediately under the mutex.  Kept
	// as empty stubs so existing call sites compile unchanged.
	LogScope::LogScope() {}
	LogScope::~LogScope() {}
	void LogScope::Flush() {}
#endif

#if UBA_DEBUG_VALIDATE
	bool g_validateFileAccess = false;
#endif

	// ─────────────────────────────────────────────────────────────────────────
	// TLS-free per-thread disallow-detour depth.
	//
	// Previously `thread_local u32 t_disallowDetour`.  Removed because goroutine
	// OS threads created by Go's newosproc() via raw clone() lack a glibc TCB
	// (the FS segment base points at Go's M struct instead of a thread-control
	// block), so any access through __tls_get_addr crashes.  The replacement
	// keeps a fixed-size open-addressed array keyed by the current OS thread id,
	// obtained via GetCurrentThreadId (Windows) or a raw SYS_gettid / mach call
	// (Posix) — neither touches TLS.
	//
	// Slot count is generous enough to cover the real concurrent-thread count of
	// the detoured tools we see in practice (cl/clang/lld/msbuild hit single
	// digits; esbuild/go builds ~GOMAXPROCS).  If ever exceeded we silently drop
	// the increment — that just means we fail to suppress a reentrant detour on
	// the overflowing thread, which is a latent deadlock risk, so sizing is
	// meant to be comfortably above any plausible workload.
	// ─────────────────────────────────────────────────────────────────────────

	static constexpr u32 kDisallowSlots = 256;

	struct DisallowSlot
	{
		std::atomic<u64> tid;   // 0 = empty
		std::atomic<u32> depth;
	};
	static DisallowSlot g_disallowSlots[kDisallowSlots];

	static inline u64 GetOsThreadIdNoTls()
	{
		#if PLATFORM_WINDOWS
			return (u64)::GetCurrentThreadId();
		#elif PLATFORM_LINUX
			// Raw SYS_gettid — no glibc wrapper, no errno, no TLS.
			long tid;
			register long rax asm("rax") = 186; // SYS_gettid
			asm volatile("syscall" : "+a"(rax) : : "rcx", "r11", "memory");
			tid = rax;
			return (u64)tid;
		#elif PLATFORM_MAC
			// pthread_self reads a register; pthread_mach_thread_np does not touch TLS.
			return (u64)pthread_mach_thread_np(pthread_self());
		#else
			return 0;
		#endif
	}

	// Locate the slot for `tid`.  For increment we grab an empty slot (CAS) if
	// none matches.  For decrement we must find an existing match (or ignore).
	static DisallowSlot* FindOrAcquireSlot(u64 tid, bool allowAcquire)
	{
		// Splitmix-style mix to spread poorly-distributed tid values.
		u64 h = tid * 0x9E3779B97F4A7C15ULL;
		h ^= h >> 33;
		u32 start = (u32)(h & (kDisallowSlots - 1));
		for (u32 i = 0; i < kDisallowSlots; ++i)
		{
			u32 idx = (start + i) & (kDisallowSlots - 1);
			DisallowSlot& s = g_disallowSlots[idx];
			u64 cur = s.tid.load(std::memory_order_acquire);
			if (cur == tid)
				return &s;
			if (cur == 0 && allowAcquire)
			{
				u64 expected = 0;
				if (s.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel, std::memory_order_acquire))
					return &s;
				if (expected == tid)
					return &s;
			}
		}
		return nullptr; // overflow — silently drop
	}

	u32 DisallowDetourDepth()
	{
		u64 tid = GetOsThreadIdNoTls();
		if (DisallowSlot* s = FindOrAcquireSlot(tid, /*allowAcquire*/ false))
			return s->depth.load(std::memory_order_acquire);
		return 0;
	}

	void DisallowDetourIncrement()
	{
		u64 tid = GetOsThreadIdNoTls();
		if (DisallowSlot* s = FindOrAcquireSlot(tid, /*allowAcquire*/ true))
			s->depth.fetch_add(1, std::memory_order_acq_rel);
	}

	void DisallowDetourDecrement()
	{
		u64 tid = GetOsThreadIdNoTls();
		if (DisallowSlot* s = FindOrAcquireSlot(tid, /*allowAcquire*/ false))
		{
			// Release the slot when depth hits zero so the array does not
			// accumulate dead threads (notable when the process churns many
			// short-lived OS threads, e.g. Go's goroutine workers).
			u32 prev = s->depth.fetch_sub(1, std::memory_order_acq_rel);
			if (prev == 1)
				s->tid.store(0, std::memory_order_release);
		}
	}

	SuppressDetourScope::SuppressDetourScope()  { DisallowDetourIncrement(); }
	SuppressDetourScope::~SuppressDetourScope() { DisallowDetourDecrement(); }

#if !UBA_STUB_BUILD
	// Everything below — TrackInput(), FixPath(), VFS/virtualisation, LogHeader,
	// Rpc_WriteLog/Rpc_ResolveCallstack, Shared_WriteConsole/Shared_GetFileAttributes,
	// TakeLockForRpc — is either on a dead path for the stub or drags symbols the
	// freestanding build cannot resolve (Sleep, vsnprintf, operator new, FixPath2,
	// CouldBeCompressedFile, Rpc_GetEntryInformation, g_systemTemp/g_virtualWorkingDir
	// StringBuffer storage, etc.). The stub's only remaining dependency on this file
	// is InitSharedVariables() + DisallowDetour* + the FatalError stub already
	// provided in UbaStaticStubCore.cpp.
	void InitTrackInput()
	{
		g_trackInputsMem = (u8*)g_memoryBlock.Allocate(TrackInputsMemCapacity, 1, TC("TrackInputs"));
	}

	void SendInput()
	{
		if (!g_trackInputsMem)
			return;

		u32 left = g_trackInputsBufPos;
		u32 reserveSize = left;
		u32 pos = 0;
		while (left)
		{
			RPC_MESSAGE(InputDependencies, log)
			writer.Write7BitEncoded(reserveSize);
			reserveSize = 0;
			u32 toWrite = Min(left, u32(writer.GetCapacityLeft() - sizeof(u32)));
			writer.WriteU32(toWrite);
			writer.WriteBytes(g_trackInputsMem + pos, toWrite);
			writer.Flush();
			left -= toWrite;
			pos += toWrite;
		}
		g_trackInputsBufPos = 0;
	}


	void TrackInput(StringView file)
	{
		if (!g_trackInputsMem)
			return;

		SCOPED_FUTEX(g_trackInputsLock, l);

		if (g_trackInputsBufPos > TrackInputsMemCapacity - 2048)
			SendInput();
		UBA_ASSERTF(!file.StartsWith(TCV("\\")), TC("Error %s is not an absolute path. All tracked files must be absolute"), file.data);

		BinaryWriter w(g_trackInputsMem, g_trackInputsBufPos, TrackInputsMemCapacity);
		w.WriteString(file);
		g_trackInputsBufPos = u32(w.GetPosition());
	}

	void SkipTrackInput(StringView file)
	{
		// Just here to easily log out what we are ignoring in terms of input
	}

	bool IsTrackingInput()
	{
		return g_trackInputsMem != 0;
	}

	bool FixPath(StringBufferBase& out, const tchar* path, u32 pathLen)
	{
		return FixPath2(path, pathLen, g_virtualWorkingDir.data, g_virtualWorkingDir.count, out.data, out.capacity, &out.count);
	}

	bool FixPath(StringBufferBase& out, const StringView path)
	{
		return FixPath2(path.data, path.count, g_virtualWorkingDir.data, g_virtualWorkingDir.count, out.data, out.capacity, &out.count);
	}

	struct VfsEntry { StringView vfs; StringView local; VfsEntry() : vfs(NoInit), local(NoInit) {}; };
	VfsEntry g_vfsEntries[32];

	void PopulateVfs(BinaryReader& vfsReader)
	{
		while (vfsReader.GetLeft())
		{
			vfsReader.ReadByte(); // Index, unused
			StringBuffer<> str;
			vfsReader.ReadString(str);
			if (!str.count)
			{
				vfsReader.SkipString();
				continue;
			}

			#if PLATFORM_WINDOWS
			str.Replace('/', '\\');
			#endif

			u32 index = g_vfsEntryCount++;
			UBA_ASSERT(index < sizeof_array(g_vfsEntries));
			VfsEntry& vfsEntry = g_vfsEntries[index];
			vfsEntry.vfs = g_memoryBlock.Strdup(str);

			if (index == 0)
				g_vfsMatchingLength = vfsEntry.vfs.count;
			else
			{
				u32 shortest = Min(g_vfsMatchingLength, vfsEntry.vfs.count);
				for (u32 i=0; i!=shortest; ++i)
				{
					if (g_vfsEntries[0].vfs.data[i] == vfsEntry.vfs.data[i])
						continue;
					shortest = i;
					break;
				}
				g_vfsMatchingLength = shortest;
			}
			vfsReader.ReadString(str.Clear());
			vfsEntry.local = g_memoryBlock.Strdup(str);
		}
	}

	bool IsVfsEnabled()
	{
		return g_vfsEntryCount > 0;
	}

	bool DevirtualizePath(StringBufferBase& path)
	{
		if (!g_vfsEntryCount)
			return false;

		if (!Equals(path.data, g_vfsEntries[0].vfs.data, Min(path.count, g_vfsMatchingLength), CaseInsensitiveFs))
			return false;

		// TODO: This is not great, the dirs above the vfs root should be empty except the dir to the roots
		if (path.count < g_vfsMatchingLength)
		{
			path.Clear().Append(g_vfsEntries[0].local);
			return true;
		}

		for (u32 i=0, e=g_vfsEntryCount; i!=e; ++i)
		{
			VfsEntry& entry = g_vfsEntries[i];
			if (!path.StartsWith(entry.vfs.data))
				continue;
			StringBuffer<MaxPath> temp2(path.data + entry.vfs.count);
			path.Clear().Append(entry.local).Append(temp2);
			return true;
		}
		return false;
	}

	tchar DevirtualizeDrive(tchar drive)
	{
		if (!g_vfsEntryCount)
			return '\0';
		return g_vfsEntries[0].local[0];
	}

	bool VirtualizePath(StringBufferBase& path)
	{
		if (!g_vfsEntryCount)
			return false;
		for (u32 i=0, e=g_vfsEntryCount; i!=e; ++i)
		{
			VfsEntry& entry = g_vfsEntries[i];
			if (path.count < entry.local.count || !path.StartsWith(entry.local.data))
				continue;
			StringBuffer<MaxPath> temp2(path.data + entry.local.count);
			path.Clear().Append(entry.vfs).Append(temp2);
			return true;
		}
		return false;
	}

	void LogHeader(StringView cmdLine)
	{
		#if UBA_DEBUG_LOG_ENABLED
		if (g_debugFile == InvalidFileHandle)
			return;
		WriteDebugLog(TC("ProcessId: %u"), g_processId);
		WriteDebug("CmdLine: ", 9);

		#if PLATFORM_WINDOWS
		u32 left = cmdLine.count;
		const tchar* read = cmdLine.data;
		while (left)
		{
			char buf[1024];
			u32 written = 0;
			while (*read && written < sizeof(buf))
				buf[written++] = (char)*read++;
			WriteDebug(buf, written);
			left -= written;
		}
		#else
		WriteDebug(cmdLine.data, cmdLine.count*sizeof(tchar));
		#endif

		WriteDebug("\n", 1);
		WriteDebugLog(TC("WorkingDir: %s"), g_virtualWorkingDir.data);
		WriteDebugLog(TC("ExeDir: %s"), g_virtualApplicationDir.data);
		WriteDebugLog(TC("ExeDir (actual): %s"), g_exeDir.data);
		WriteDebugLog(TC("SystemTemp: %s"), g_systemTemp.data);
		WriteDebugLog(TC("Rules: %u (%u)"), g_rules->index, GetApplicationRules()[g_rules->index].hash);
		if (g_runningRemote)
		{
			StringBuffer<256> computerName;
			GetComputerNameW(computerName);
			WriteDebugLog(TC("Remote: %s"), computerName.data);
		}
		static u32 reuseCounter;
		if (reuseCounter)
			WriteDebugLog(TC("ProcessReuseIndex: %u"), reuseCounter);
		++reuseCounter;
		WriteDebugLog(TC(""));
		#endif
	}

	void LogVfsInfo()
	{
		for (u32 i=0; i!=g_vfsEntryCount; ++i)
		{
			DEBUG_LOG(TC("Vfs: %s -> %s"), g_vfsEntries[i].vfs.data, g_vfsEntries[i].local.data);
		}
	}

	const tchar* GetApplicationShortName()
	{
		const tchar* lastBackslash = TStrrchr(g_virtualApplication.data, '\\');
		const tchar* lastSlash = TStrrchr(g_virtualApplication.data, '/');
		if (lastBackslash || lastSlash)
			return (lastBackslash > lastSlash ? lastBackslash : lastSlash) + 1;
		return g_virtualApplication.data;
	}

	ANALYSIS_NORETURN void FatalError(u32 code, const tchar* format, ...)
	{
		StringBuffer<2048> sb;
		sb.Append(GetApplicationShortName()).Append(TCV(" ERROR: "));
		va_list arg;
		va_start(arg, format);
		sb.Append(format, arg);
		va_end(arg);
		Rpc_WriteLog(sb.data, sb.count, true, true);

		#if PLATFORM_WINDOWS // Maybe all platforms should call exit()?
		ExitProcess(code);
		#else
		exit(code);
		#endif
	}

	bool TakeLockForRpc()
	{
		for (u32 i=0; i!=5; ++i)
		{
			if (g_communicationLock.TryEnter())
				return true;
			Sleep(100);
		}
		DEBUG_LOG(TC("TakeLockForRpc failed"));
		return false;
	}

	void Rpc_WriteLog(const tchar* text, u64 textCharLength, bool printInSession, bool isError)
	{
		DEBUG_LOG(TC("LOG  %.*s"), u32(textCharLength), text); // TODO: Investigate, deadlocks on non-windows
		// DEBUG_LOG(TC("LOG [%7u] %.*s"), GetCurrentThreadId(), u32(textCharLength), text);

		if (!TakeLockForRpc())
		{
			return;
		}
		RPC_MESSAGE_NO_LOCK(Log, log)
		writer.WriteBool(printInSession);
		writer.WriteBool(isError);
		writer.WriteString(text, textCharLength);
		writer.Flush();
		g_communicationLock.Leave();
	}

	void Rpc_WriteLogf(const tchar* format, ...)
	{
		va_list arg;
		va_start(arg, format);
		StringBuffer<2048> buffer;
		buffer.Append(format, arg);
		va_end(arg);
		Rpc_WriteLog(buffer.data, buffer.count, false, false);
	}

	UBA_NOINLINE void Rpc_ResolveCallstack(StringBufferBase& out, u32 skipCallstackCount, void* context)
	{
		if (!TakeLockForRpc())
		{
			out.Append(TCV("\n  Can't fetch callstack while in rpc"));
			return;
		}

		RPC_MESSAGE_NO_LOCK(ResolveCallstack, log)
		auto written = (u32*)writer.AllocWrite(4);
		if (WriteCallstackInfo(writer, skipCallstackCount, context))
		{
			*written = u32(writer.GetPosition()) - 5;
			BinaryReader reader = writer.Flush();
			reader.ReadString(out);
		}
		else
		{
			out.Append(TCV("\n   Failed to resolve callstack\n"));
		}
		g_communicationLock.Leave();
	}

	//TODO: Implement SetConsoleTextAttribute.. clang is using it to color errors

	tchar g_consoleString[4096];
	u32 g_consoleStringIndex;

	template<typename CharType>
	void Shared_WriteConsoleT(const CharType* chars, u32 charCount, bool isError)
	{
		if (!g_conEnabled[isError?0:1] || g_suppressLogging)
			return;

		SCOPED_WRITE_LOCK(g_consoleStringCs, lock);
		const CharType* read = chars;
		tchar* write = g_consoleString + g_consoleStringIndex;
		int left = sizeof_array(g_consoleString) - g_consoleStringIndex - 1;
		int available = charCount;
		while (available)
		{
			if (*read == '\n' || !left)
			{
				*write = 0;
				u32 strLen = u32(write - g_consoleString);
				if (!g_rules->SuppressLogLine(g_consoleString, strLen))
					Rpc_WriteLog(g_consoleString, strLen, false, isError);
				write = g_consoleString;
				left = sizeof_array(g_consoleString) - 1;
			}
			else
			{
				*write = *read;
				++write;
			}
			++read;
			--left;
			--available;
		}
		g_consoleStringIndex = u32(write - g_consoleString);
	}

	void Shared_WriteConsole(const char* chars, u32 charCount, bool isError) { Shared_WriteConsoleT(chars, charCount, isError); }

	#if PLATFORM_WINDOWS
	void Shared_WriteConsole(const wchar_t* chars, u32 charCount, bool isError) { Shared_WriteConsoleT(chars, charCount, isError); }
	#endif


	const tchar* Shared_GetFileAttributes(FileAttributes& outAttr, StringView fileName, bool checkIfDir)
	{
		memset(&outAttr, 0, sizeof(FileAttributes));

		StringBuffer<MaxPath> fileNameForKey;
		fileNameForKey.Append(fileName);
		if (CaseInsensitiveFs)
			fileNameForKey.MakeLower();

		UBA_ASSERT(fileNameForKey.count);
		CHECK_PATH(fileNameForKey);
		StringKey fileNameKey = ToStringKey(fileNameForKey);

		memset(&outAttr.data, 0, sizeof(outAttr.data));

		u64 fileSize = InvalidValue;

		#if PLATFORM_WINDOWS
		if (fileName[1] == ':' && fileName[3] == 0 && (ToLower(fileName[0]) == ToLower(g_virtualWorkingDir[0]) || ToLower(fileName[0]) == g_systemRoot[0]))
		{
			// This is the root of the drive.. let's just return it as a directory
			outAttr.exists = true;
			outAttr.lastError = ErrorSuccess;
			outAttr.data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
			return fileName.data;
		}
		#endif

		// This is an optimization where we populate directory table and use that to figure out if file exists or not..
		// .. in msvc's case it doesn't matter much because these tables are already up to date when msvc use CreateFile.
		// .. clang otoh is using CreateFile with tooons of different paths trying to open files.. in remote worker case this becomes super expensive
		DirectoryTable::EntryInformation info;
		if (!Rpc_GetEntryInformation(info, fileNameKey, fileName, checkIfDir))
		{
			outAttr.exists = false;
			outAttr.lastError = ErrorFileNotFound;
			return fileName.data;
		}

		if (fileSize == InvalidValue)
			fileSize = info.size; // This can also be InvalidValue if file is open for write (Used in posix path)

		// Could be compressed and then directory table size is wrong
		if (CouldBeCompressedFile(fileName))
		{
			// If file is output file we accept wrong size because size is not supposed to be used anyway.
			// We don't want to trigger unnecessary download/decompress of file
			if (!g_rules->IsOutputFile(fileName, g_systemTemp))
			{
				StringBuffer<> temp;
				u32 closeId;
				Rpc_CreateFileW(fileName, fileNameKey, AccessFlag_Read, temp.data, temp.capacity, fileSize, closeId, true);
			}
		}

		outAttr.exists = true;
		outAttr.lastError = ErrorSuccess;

		UBA_ASSERTF(info.fileIndex, TC("No file index set for file %.*s"), fileName.count, fileName.data);
		outAttr.fileIndex = info.fileIndex;
		outAttr.volumeSerial = info.volumeSerial;

#if PLATFORM_WINDOWS
		LARGE_INTEGER li = ToLargeInteger(fileSize);
		outAttr.data.dwFileAttributes = info.attributes;
		outAttr.data.nFileSizeLow = li.LowPart;
		outAttr.data.nFileSizeHigh = li.HighPart;
		(u64&)outAttr.data.ftCreationTime = info.lastWrite;
		(u64&)outAttr.data.ftLastAccessTime = info.lastWrite;
		(u64&)outAttr.data.ftLastWriteTime = info.lastWrite;
#else
		outAttr.data.st_mtimespec = ToTimeSpec(info.lastWrite);
		outAttr.data.st_mode = (mode_t)info.attributes;
		outAttr.data.st_dev = info.volumeSerial;
		outAttr.data.st_ino = info.fileIndex;
		outAttr.data.st_size = fileSize;
		static auto uid = getuid();
		static auto gid = getgid();
		outAttr.data.st_uid = uid; // needed by git
		outAttr.data.st_gid = gid; // needed by git
#endif

#if 0//UBA_DEBUG_VALIDATE
		if (g_validateFileAccess && !keepInMemory)
		{
			WIN32_FILE_ATTRIBUTE_DATA validate;
			memset(&validate, 0, sizeof(validate));
			SuppressDetourScope _;
			BOOL res = True_GetFileAttributesExW(fileName, GetFileExInfoStandard, &validate); (void)res;
			if (outAttr.exists)
			{
				UBA_ASSERTF(res != 0, L"File %ls exists even though uba claims it is not..", fileName.data);
				if (validate.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					UBA_ASSERTF((outAttr.data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY), L"File attributes are wrong for %ls", fileName.data);
				else
				{
					validate.ftCreationTime = outAttr.data.ftCreationTime; // Creation time is not really important
					validate.ftLastAccessTime = outAttr.data.ftLastAccessTime; // Access time is not really important
					validate.ftLastWriteTime = outAttr.data.ftLastWriteTime; // Write time is important, revisit this
					UBA_ASSERTF(memcmp(&validate, &outAttr.data, sizeof(WIN32_FILE_ATTRIBUTE_DATA)) == 0, L"File %ls is not up-to-date in cache", fileName.data);
				}
			}
			else
			{
				UBA_ASSERTF(res == 0, L"Can't find file %ls but validation checked that it is there", fileName.data); // This means most likely that Uba did not update attribute table for added files.
				DWORD lastError2 = GetLastError();
				if (lastError2 == ERROR_PATH_NOT_FOUND || lastError2 == ERROR_INVALID_NAME)
					lastError2 = ERROR_FILE_NOT_FOUND;
				UBA_ASSERT(outAttr.lastError == lastError2);
			}
		}
#endif
		return fileName.data;
	}
#endif // !UBA_STUB_BUILD
}
