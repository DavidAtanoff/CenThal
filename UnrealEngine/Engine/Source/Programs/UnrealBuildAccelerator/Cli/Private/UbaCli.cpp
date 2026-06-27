// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaApplication.h"
#include "UbaBinaryParser.h"
#include "UbaCacheClient.h"
#include "UbaCacheServer.h"
#include "UbaClient.h"
#include "UbaCompressedFileHeader.h"
#include "UbaConfig.h"
#include "UbaCoordinatorWrapper.h"
#include "UbaDirectoryIterator.h"
#include "UbaFileAccessor.h"
#include "UbaNetworkBackendTcp.h"
#include "UbaPathUtils.h"
#include "UbaPlatform.h"
#include "UbaProcess.h"
#include "UbaProtocol.h"
#include "UbaRootPaths.h"
#include "UbaScheduler.h"
#include "UbaStaticPatcher.h"
#include "UbaSessionClient.h"
#include "UbaSessionServer.h"
#include "UbaStorageClient.h"
#include "UbaStorageServer.h"
#include "UbaStorageUtils.h"
#include "UbaVersion.h"

#include "UbaAWS.h"

#if PLATFORM_WINDOWS
#include <dbghelp.h>
#include <io.h>
#pragma comment (lib, "Dbghelp.lib")
#endif

#if PLATFORM_MAC
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#define CS_RUNTIME        0x00010000  // hardened runtime
#define CS_RESTRICT       0x00000800
#define CSR_ALLOW_UNRESTRICTED_DYLD (1 << 7)
#endif

namespace uba
{
	const tchar*	Version = GetVersionString();
	u32				DefaultCapacityGb = 0;
	const tchar*	DefaultRootDir = []() {
		static tchar buf[256];
		if constexpr (IsWindows)
			ExpandEnvironmentStringsW(TC("%ProgramData%\\Epic\\" UE_APP_NAME), buf, sizeof(buf));
		else
			GetFullPathNameW(TC("~/" UE_APP_NAME), sizeof_array(buf), buf, nullptr);
		return buf;
		}();
	u32				DefaultProcessorCount = []() { return GetLogicalProcessorCount(); }();

	bool PrintHelp(const tchar* message)
	{
		LoggerWithWriter logger(g_consoleLogWriter, TC(""));
		if (*message)
		{
			logger.Info(TC(""));
			logger.Error(TC("%s"), message);
		}

		const tchar* dbgStr = TC("");
		#if UBA_DEBUG
		dbgStr = TC(" (DEBUG)");
		#endif

		logger.Info(TC(""));
		logger.Info(TC("-------------------------------------------"));
		logger.Info(TC("   UbaCli v%s%s"), Version, dbgStr);
		logger.Info(TC("-------------------------------------------"));
		logger.Info(TC(""));
		logger.Info(TC("  UbaCli.exe [options...] <commandtype> <executable> [arguments...]"));
		logger.Info(TC(""));
		logger.Info(TC("  CommandTypes:"));
		logger.Info(TC("   local                   Will run executable locally using detoured paths"));
		logger.Info(TC("   remote                  Will wait for available agent and then run executable remotely"));
		logger.Info(TC("   agent                   Will run executable against agent spawned in process"));
		logger.Info(TC("   native                  Will run executable in a normal way"));
		logger.Info(TC(""));
		logger.Info(TC("  Options:"));
		#if PLATFORM_MAC
		logger.Info(TC("   -validate               Will validate binary to see if it can run dyld env needed by uba"));
		#endif
		logger.Info(TC("   -dir=<rootdir>          The directory used to store data. Defaults to \"%s\""), DefaultRootDir);
		logger.Info(TC("   -port=[<host>:]<port>   The ip/name and port (default: %u) of the machine we want to help"), DefaultPort);
		logger.Info(TC("   -log                    Log all processes detouring information to file (only works with debug builds)"));
		logger.Info(TC("   -quiet                  Does not output any logging in console except errors"));
		logger.Info(TC("   -uniqueid               Makes sure to always create session dir with a unique name"));
		logger.Info(TC("   -loop=<count>           Loop the commandline <count> number of times. Will exit when/if it fails"));
		logger.Info(TC("   -workdir=<dir>          Working directory"));
		logger.Info(TC("   -config=<file>          Config file that contains options for various systems"));
		logger.Info(TC("   -vfs=<virtual>;<local>  Will convert virtual path to local under the hood. Can have multiple -vfs."));
		logger.Info(TC("   -cross=<from>;<to>.     Will add cross architecture/platform entries. Can have multiple -cross."));
		logger.Info(TC("   -checkcas               Check so all cas entries are correct. Combine with -deleteCas to delete bad ones"));
		logger.Info(TC("   -checkfiletable         Check so file table has correct cas stored. if bad entries it is enough to delete 'casdb' file"));
		logger.Info(TC("   -checkcloud             Check if we are inside cloud and output information about cloud"));
		logger.Info(TC("   -deletecas              Deletes the casdb"));
		logger.Info(TC("   -getcas                 Will print hash of application"));
		logger.Info(TC("   -listimports            Will print explicit imports of binary"));
		logger.Info(TC("   -summary                Print summary at the end of a session"));
		logger.Info(TC("   -nocustomalloc          Disable custom allocator for processes. If you see odd crashes this can be tested"));
		logger.Info(TC("   -nostdout               Disable stdout from process."));
		logger.Info(TC("   -storeraw               Disable compression of storage. This will use more storage and might improve performance"));
		logger.Info(TC("   -maxcpu=<number>        Max number of processes that can be started. Defaults to \"%u\" on this machine"), DefaultProcessorCount);
		logger.Info(TC("   -visualizer             Spawn a visualizer that visualizes progress"));
		logger.Info(TC("   -detailedtrace          Add details to the trace"));
		logger.Info(TC("   -traceChildProcesses    Trace the child processes separately"));
		logger.Info(TC("   -crypto=<32chars>       Will enable crypto on network client/server"));
		logger.Info(TC("   -coordinator=<name>     Load a UbaCoordinator<name>.dll to instantiate a coordinator to get helpers"));
		logger.Info(TC("   -cache=<host>[:<port>]  Connect to cache server. Will fetch from cache unless -populatecache is set"));
		logger.Info(TC("   -populatecache          Populate cache server if connected to one"));
		logger.Info(TC("   -cachecommand=<cmd>     Send command to cache server. Will output result in log"));
		logger.Info(TC("   -writecachesummary      Write cache summary file about connected cache server"));
		logger.Info(TC("   -decompress=<file>      Decompress a compressed file in-place"));
		logger.Info(TC(""));
		logger.Info(TC("  CoordinatorOptions (if coordinator set):"));
		logger.Info(TC("   -uri=<address>          Uri to coordinator"));
		logger.Info(TC("   -pool=<name>            Name of helper pool inside coordinator"));
		logger.Info(TC("   -oidc=<name>            Name of oidc"));
		logger.Info(TC("   -maxcores=<number>      Max number of cores that will be asked for from coordinator"));
		logger.Info(TC(""));
		logger.Info(TC("  If <executable> is a .yaml-file UbaCli creates a scheduler to execute commands from the yaml file instead"));
		logger.Info(TC("  If <executable> is \"speedtest\" then a network speed test will be performed"));

		logger.Info(TC(""));
		return false;
	}

	StorageServer* g_storageServer;
	
	void CtrlBreakPressed()
	{
		if (g_storageServer)
		{
			g_storageServer->SaveCasTable(true);
			LoggerWithWriter(g_consoleLogWriter).Info(TC("CAS table saved..."));
		}
		abort();
	}

	#if PLATFORM_WINDOWS
	BOOL ConsoleHandler(DWORD signal)
	{
		if (signal == CTRL_C_EVENT)
			CtrlBreakPressed();
		return FALSE;
	}
	#else
	void ConsoleHandler(int sig)
	{
		CtrlBreakPressed();
	}
	#endif
	
	StringBuffer<> g_rootDir(DefaultRootDir);

	struct DirToDelete
	{
		Atomic<u32> counter;
		DirToDelete* parent = nullptr;
		TString path;
	};

	void DeleteDir(Logger& logger, WorkManagerImpl& workManager, DirToDelete* dir, Atomic<u64>& deleteCount)
	{
		StringBuffer<> fullName;
		fullName.Append(dir->path).Append(PathSeparator);
		u32 dirLen = fullName.count;

		TraverseDir(logger, dir->path, [&](const DirectoryEntry& e)
			{
				fullName.Resize(dirLen).Append(e.name);
				if (IsDirectory(e.attributes))
				{
					++dir->counter;
					auto child = new DirToDelete{1, dir, fullName.ToString()};
					workManager.AddWork([&, child](const WorkContext&)
						{
							DeleteDir(logger, workManager, child, deleteCount);
						}, 1, TC("D"));
				}
				else
				{
					DeleteFileW(fullName.data);
					++deleteCount;
					if ((deleteCount.load() % 100000) == 0)
						logger.Info(TC("Deleted: %llu"), deleteCount.load());
				}
			});

		for (auto it=dir;it;)
		{
			if (--it->counter)
				return;
			RemoveDirectoryW(it->path.c_str());
			++deleteCount;
			auto parent = it->parent;
			delete it;
			it = parent;
		}
	}

#if PLATFORM_MAC
	bool ExplainDyldEnvSupport(Logger& logger, const char* path)
	{
		bool ok = true;
		struct stat st;
		if (stat(path, &st) != 0)
			return logger.Error("FAIL: cannot stat binary %s", path);

		if (st.st_mode & (S_ISUID | S_ISGID))
			return logger.Error("FAIL: setuid/setgid binaries never allow DYLD injection");

		CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8*)path, strlen(path), true);
		if (!url)
			return logger.Error("FAIL: invalid path");

		SecStaticCodeRef code = nullptr;
		ok = SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess;
		CFRelease(url);
		if (!ok)
			return logger.Error("FAIL: cannot create SecStaticCode");

		CFDictionaryRef info = nullptr;
		ok = SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info) == errSecSuccess;
		CFRelease(code);
		if (!ok)
			return logger.Error("FAIL: cannot read code signature");

		uint32_t flags = 0;
		if (CFNumberRef flagsNum = (CFNumberRef)CFDictionaryGetValue(info, kSecCodeInfoFlags))
			CFNumberGetValue(flagsNum, kCFNumberSInt32Type, &flags);

		bool hardened = (flags & CS_RUNTIME) != 0;
		bool restricted = (flags & CS_RESTRICT) != 0;

		logger.Info("Hardened runtime: %s", hardened ? "YES" : "NO");
		logger.Info("Restricted binary: %s", restricted ? "YES" : "NO");

		bool disableLV = false;
		if (CFDictionaryRef ent = (CFDictionaryRef)CFDictionaryGetValue(info, kSecCodeInfoEntitlementsDict))
			if (CFDictionaryGetValue(ent, CFSTR("com.apple.security.cs.disable-library-validation")))
				disableLV = true;

		logger.Info("Library validation disabled: %s", disableLV ? "YES" : "NO");
		CFRelease(info);

		if (restricted)
		{
			logger.Error("FAIL: restricted binaries do not honor DYLD env vars");
			ok = false;
		}
		if (hardened && !disableLV)
		{
			logger.Error("FAIL: hardened runtime with library validation blocks DYLD injection");
			logger.Error("Re-sign binary with command line:");
			logger.Error(" codesign --force --deep --sign - %s", path);
			ok = false;
		}
#if 0 // This does not seem to ever work
		int csr = 0; size_t csrSize = sizeof(csr);
		if (sysctlbyname("kern.csr.active_config", &csr, &csrSize, nullptr, 0) == 0)
		{
			bool sipBlocksDyld = (csr & CSR_ALLOW_UNRESTRICTED_DYLD) == 0;
			logger.Info("  SIP dyld unrestricted: %s", sipBlocksDyld ? "NO" : "YES");
			if (sipBlocksDyld)
				logger.Info("  NOTE: SIP may strip DYLD_* for protected system binaries");
		}
		else
		{
			logger.Info("  SIP state: unavailable (assuming enabled)");
		}
#endif
		if (ok)
			logger.Info("PASS: DYLD_* environment variables SHOULD work for this binary");
		return ok;
	}
#endif

	bool WrappedMain(int argc, tchar* argv[])
	{
		using namespace uba;

		AddExceptionHandler();
		InitMemory();
		SetMemoryWorkingSet(DefaultMemoryWorkingSet);

		u32 storageCapacityGb = DefaultCapacityGb;
		StringBuffer<256> workDir;
		StringBuffer<128> listenIp;
		StringBuffer<128> cacheHost;
		TString crypto;
		TString coordinatorName;
		TString coordinatorPool;
		u32 coordinatorMaxCoreCount = 400;
		u16 port = DefaultPort;
		u16 cachePort = DefaultCachePort;
		u32 maxProcessCount = DefaultProcessorCount;
		u32 agentCount = 1;
		bool launchVisualizer = false;
		bool storeCompressed = true;
		bool disableCustomAllocator = false;
		bool quiet = false;
		bool useUniqueId = false;
		bool checkCas = false;
		bool checkCas2 = false;
		bool checkCloud = false;
		bool getCas = false;
		bool listImports = false;
		bool validateBinary = false;
		bool deleteCas = false;
		bool fastDelete = false;
		bool enableStdOut = true;
		bool printSummary = false;
		bool detailedTrace = false;
		bool traceChildProcesses = false;
		bool populateCache = false;
		bool writeCacheSummary = false;
		bool logToFile = false;
		bool useHackVfs = false;
		bool useScheduler = false;
		bool useMemsock = false;
		bool decompressInPlace = false;
		TString checkFileTable;
		TString cacheFilterString;
		TString cacheCommand;
		TString testCompress;
		TString fileToDecompress;
		TString testEncrypt;
		TString testDecrypt;
		TString addCas;
		TString configFile;
		TString symbolFile;
		u64 symbolOffset = 0;

		#if PLATFORM_LINUX
		TString patchBinaryIn;
		TString patchBinaryOut;
		#endif

		struct VfsEntry { TString virtualPath; TString localPath; };
		Vector<VfsEntry> vfsEntries;

		struct CrossEntry { TString from; TString to; };
		Vector<CrossEntry> crossEntries;

		u32 loopCount = 1;

		enum CommandType
		{
			CommandType_NotSet,
			CommandType_Local,
			CommandType_Remote,
			CommandType_Native,
			CommandType_Agent,
			CommandType_None,
		};

		CommandType commandType = CommandType_NotSet;

		TString application;
		TString arguments;

		auto parseOption = [&](StringView name, StringBufferBase& value)
			{
				if (IsWindows && name.Equals(TCV("-visualizer")))
				{
					launchVisualizer = true;
				}
				else if (name.Equals(TCV("-crypto")))
				{
					if (value.IsEmpty())
						value.Append(TCV("0123456789abcdef0123456789abcdef"));
					crypto = value.data;
				}
				else if (name.Equals(TCV("-coordinator")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-coordinator needs a value"));
					coordinatorName = value.data;
				}
				else if (name.Equals(TCV("-pool")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-pool needs a value"));
					coordinatorPool = value.data;
				}
				else if (name.Equals(TCV("-maxcores")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-maxcores needs a value"));
					if (!value.Parse(coordinatorMaxCoreCount))
						return PrintHelp(TC("Invalid value for -maxcores"));
				}
				else if (name.Equals(TCV("-workdir")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-workdir needs a value"));
					if ((workDir.count = GetFullPathNameW(value.data, workDir.capacity, workDir.data, nullptr)) == 0)
						return PrintHelp(StringBuffer<>().Appendf(TC("-workdir has invalid path %s"), value.data).data);
				}
				else if (name.Equals(TCV("-config")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-config needs a value"));
					if (!ExpandEnvironmentVariables(value, PrintHelp))
						return false;
					configFile = value.data;
				}
				else if (name.Equals(TCV("-vfs")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-vfs needs a value"));
					const tchar* semi = value.First(';');
					if (!semi)
						return PrintHelp(TC("-vfs needs a semicolon between virtual and local path (And don't forget quotes around value)"));
					u32 semiPos = u32(semi - value.data);
					vfsEntries.push_back({StringView(value.data, semiPos).ToString(), StringView(value).Skip(semiPos + 1).ToString()});
				}
				else if (name.Equals(TCV("-cross")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-cross needs a value"));
					const tchar* semi = value.First(';');
					if (!semi)
						return PrintHelp(TC("-cross needs a semicolon between paths (And don't forget quotes around value)"));
					u32 semiPos = u32(semi - value.data);
					crossEntries.push_back({StringView(value.data, semiPos).ToString(), StringView(value).Skip(semiPos + 1).ToString()});
				}
				else if (name.Equals(TCV("-capacity")))
				{
					if (!value.Parse(storageCapacityGb))
						return PrintHelp(TC("Invalid value for -capacity"));
				}
				else if (name.Equals(TCV("-port")))
				{
					if (const tchar* portIndex = value.First(':'))
					{
						StringBuffer<> portStr(portIndex + 1);
						if (!portStr.Parse(port))
							return PrintHelp(TC("Invalid value for port in -port"));
						listenIp.Append(value.data, portIndex - value.data);
					}
					else
					{
						if (!value.Parse(port))
							return PrintHelp(TC("Invalid value for -port"));
					}
				}
				else if (name.Equals(TCV("-log")))
				{
					logToFile = true;
				}
				else if (name.Equals(TCV("-loop")))
				{
					if (!value.Parse(loopCount))
						return PrintHelp(TC("Invalid value for -loop"));
				}
				else if (name.Equals(TCV("-quiet")))
				{
					quiet = true;
				}
				else if (name.Equals(TCV("-uniqueid")))
				{
					useUniqueId = true;
				}
				else if (name.Equals(TCV("-nocustomalloc")))
				{
					disableCustomAllocator = true;
				}
				else if (name.Equals(TCV("-maxcpu")))
				{
					if (!value.Parse(maxProcessCount))
						return PrintHelp(TC("Invalid value for -maxcpu"));
				}
				else if (name.Equals(TCV("-nostdout")))
				{
					enableStdOut = false;
				}
				else if (name.Equals(TCV("-checkcas")))
				{
					checkCas = true;
				}
				else if (name.Equals(TCV("-checkfiletable")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-checkfiletable needs a value"));
					StringBuffer<> temp;
					if ((temp.count = GetFullPathNameW(value.Replace('/', PathSeparator).data, temp.capacity, temp.data, nullptr)) == 0)
						return PrintHelp(StringBuffer<>().Appendf(TC("-checkfiletable has invalid path %s"), temp.data).data);
					checkFileTable = temp.data;
				}
				else if (name.Equals(TCV("-checkcas2")))
				{
					checkCas2 = true;
				}
				else if (name.Equals(TCV("-checkcloud")))
				{
					checkCloud = true;
				}
				else if (name.Equals(TCV("-testcompress")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-testCompress needs a value"));
					testCompress = value.data;
				}
				else if (name.Equals(TCV("-testdecompress")))
				{
					if (value.IsEmpty())
					{
						if (testCompress.empty())
							return PrintHelp(TC("-testDecompress needs a value"));
						value.Clear().Append(g_rootDir).EnsureEndsWithSlash().Append(TCV("castemp")).EnsureEndsWithSlash().Append(TCV("TestCompress.tmp"));
					}
					fileToDecompress = value.data;
				}
				else if (name.Equals(TCV("-decompress")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-decompress needs a filename"));
					fileToDecompress = value.data;
					decompressInPlace = true;
				}
				else if (name.Equals(TCV("-testencrypt")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-testEncrypt needs a value"));
					testEncrypt = value.data;
				}
				else if (name.Equals(TCV("-testdecrypt")))
				{
					if (value.IsEmpty())
					{
						if (testEncrypt.empty())
							return PrintHelp(TC("-testDecrypt needs a value"));
						value.Clear().Append(g_rootDir).EnsureEndsWithSlash().Append(TCV("TestEncrypt.tmp"));
					}
					testDecrypt = value.data;
				}
				else if (name.Equals(TCV("-symbol")))
				{
					const tchar* plus = value.First('+');
					if (!plus)
						return PrintHelp(TC("No + found. Format is -symbol=<file>+0x<offset>"));
					symbolFile = TString((const tchar*)value.data, plus);
					tchar* endPtr;
					#if PLATFORM_WINDOWS
					symbolOffset = wcstol(plus+1, &endPtr, 0);
					#else
					symbolOffset = strtol(plus+1, &endPtr, 0);
					#endif
				}
				#if PLATFORM_LINUX
				else if (name.Equals(TCV("-patchbinary")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-patchbinary needs a path"));
					patchBinaryIn = value.data;
				}
				else if (name.Equals(TCV("-out")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-out needs a path"));
					patchBinaryOut = value.data;
				}
				#endif
				else if (name.Equals(TCV("-deletecas")))
				{
					deleteCas = true;
				}
				else if (name.Equals(TCV("-addcas")))
				{
					addCas = value.data;
				}
				else if (name.Equals(TCV("-getcas")))
				{
					getCas = true;
				}
				else if (name.Equals(TCV("-listimports")))
				{
					listImports = true;
					commandType = CommandType_None;
				}
				else if (name.Equals(TCV("-validate")))
				{
					validateBinary = true;
					commandType = CommandType_None;
				}
				else if (name.Equals(TCV("-summary")))
				{
					printSummary = true;
				}
				else if (name.Equals(TCV("-detailedtrace")))
				{
					detailedTrace = true;
				}
				else if (name.Equals(TCV("-traceChildProcesses")))
				{
					traceChildProcesses = true;
				}
				else if (name.Equals(TCV("-hackvfs")))
				{
					useHackVfs = true;
				}
				else if (name.Equals(TCV("-memsock")))
				{
					useMemsock = true;
				}
				else if (name.Equals(TCV("-cache")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-cache needs a value"));
					if (const tchar* colon = value.First(':'))
					{
						value.Parse(cachePort, colon - value.data + 1);
						cacheHost.Append(value.data, colon - value.data);
					}
					else
						cacheHost.Append(value);
				}
				else if (name.Equals(TCV("-populatecache")))
				{
					populateCache = true;
				}
				else if (name.Equals(TCV("-cachecommand")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-cachecommand needs a value"));
					cacheCommand = value.data;
					commandType = CommandType_None;
					quiet = true;
				}
				else if (name.Equals(TCV("-writecachesummary")))
				{
					writeCacheSummary = true;
					cacheFilterString = value.data;
					commandType = CommandType_None;
				}
				else if (name.Equals(TCV("-storeraw")))
				{
					storeCompressed = false;
				}
				else if (name.Equals(TCV("-dir")))
				{
					if (value.IsEmpty())
						return PrintHelp(TC("-dir needs a value"));
					if ((g_rootDir.count = GetFullPathNameW(value.Replace('/', PathSeparator).data, g_rootDir.capacity, g_rootDir.data, nullptr)) == 0)
						return PrintHelp(StringBuffer<>().Appendf(TC("-dir has invalid path %s"), g_rootDir.data).data);
				}
				else if (name.Equals(TCV("-delete")))
				{
					fastDelete = true;
				}
				else if (name.Equals(TCV("-?")))
				{
					return PrintHelp(TC(""));
				}
				else if (name[0] == '-' || commandType != CommandType_None)
				{
					StringBuffer<> msg;
					msg.Appendf(TC("Unknown argument '%s'"), name.data);
					return PrintHelp(msg.data);
				}
				else if (commandType == CommandType_None)
				{
					application = name.data;
				}
				return true;
			};

		auto parseArg = [&](const tchar* arg)
		{
			StringBuffer<1024> name;
			StringBuffer<32*1024> value;

			if (const tchar* equals = TStrchr(arg,'='))
			{
				name.Append(arg, equals - arg);
				value.Append(equals+1);
			}
			else
			{
				name.Append(arg);
			}

			if (!application.empty())
			{
				if (!arguments.empty())
					arguments += ' ';
				TString argTemp;
				bool hasSpace = TStrchr(arg, ' ');
				if (hasSpace)
				{
					argTemp = arg;
					size_t index = 0;
					while (true)
					{
						index = argTemp.find('\"', index);
						if (index == std::string::npos) break;
						argTemp.replace(index, 1, TC("\\\""));
						index += 2;
					}
					arg = argTemp.c_str();
					arguments += TC("\"");
				}
				arguments += arg;
				if (hasSpace)
					arguments += TC("\"");
				return true;
			}

			if (commandType == CommandType_None)
			{
				return parseOption(name, value);
			}
			if (commandType != CommandType_NotSet)
			{
				application = arg;
			}
			else if (name.Equals(TCV("local")))
			{
				commandType = CommandType_Local;
			}
			else if (name.Equals(TCV("remote")))
			{
				commandType = CommandType_Remote;
			}
			else if (name.Equals(TCV("native")))
			{
				commandType = CommandType_Native;
			}
			else if (name.Equals(TCV("agent")))
			{
				commandType = CommandType_Agent;
			}
			else if (name.Equals(TCV("scheduler")))
			{
				commandType = CommandType_Local;
				useScheduler = true;
			}
			else
			{
				return parseOption(name, value);
			}
			return true;
		};

		for (int i=1; i!=argc; ++i)
			if (!parseArg(argv[i]))
				return false;

		auto addOption = [&](const tchar* name, const tchar* value) { StringBuffer<512> v(value); return parseOption(ToView(name), v); };(void)addOption;

		if (useHackVfs)
		{
	#if PLATFORM_WINDOWS
			addOption(TC("-vfs"), TC("Z:/UEVFS/FortniteGame;E:\\dev\\fn\\FortniteGame"));
			addOption(TC("-vfs"), TC("Z:/UEVFS/QAGame;E:\\dev\\fn\\QAGame"));
			addOption(TC("-vfs"), TC("Z:/UEVFS/Root;E:\\dev\\fn"));

			//addOption(TC("-vfs"), TC("Z:/UEVFS/Clang;c:\\sdk\\AutoSDK\\HostWin64\\Win64\\LLVM\\18.1.8"));
			//addOption(TC("-vfs"), TC("Z:/UEVFS/MSVC;c:\\sdk\\AutoSDK\\HostWin64\\Win64\\VS2022\\14.38.33130"));
			//addOption(TC("-vfs"), TC("Z:/UEVFS/WinSDK;C:\\Program Files (x86)\\Windows Kits\\10"));
			addOption(TC("-vfs"), TC("Z:/UEVFS/Clang;\\\\localhost\\c$\\sdk\\AutoSDK\\HostWin64\\Win64\\LLVM\\18.1.8"));
			addOption(TC("-vfs"), TC("Z:/UEVFS/MSVC;\\\\localhost\\c$\\sdk\\AutoSDK\\HostWin64\\Win64\\VS2022\\14.38.33130"));
			addOption(TC("-vfs"), TC("Z:/UEVFS/WinSDK;C:\\Program Files (x86)\\Windows Kits\\10"));
			addOption(TC("-vfs"), TC("Z:/UEVFS/SuperLuminal;C:\\Program Files\\Superluminal\\Performance\\API"));

			//addOption(TC("-vfs"), TC("Z:/UEVFS/Root;E:\\dev\\fn"));
			//addOption(TC("-vfs"), TC("Z:/UEVFS/Compiler;c:\\sdk\\AutoSDK\\HostWin64\\Win64\\LLVM\\18.1.8"));
			//addOption(TC("-vfs"), TC("Z:/UEVFS/Toolchain;c:\\sdk\\AutoSDK\\HostWin64\\Win64\\VS2022\\14.38.33130"));
			//addOption(TC("-vfs"), TC("Z:/UEVFS/WinSDK;c:\\sdk\\AutoSDK\\HostWin64\\Win64\\Windows Kits\\10.0.22621.0"));
	#else
			addOption(TC("-vfs"), TC("/UEVFS/Root;/home/honk/fn"));
			addOption(TC("-vfs"), TC("/UEVFS/LinuxSDK;/home/honk/AutoSDK/HostLinux/Linux_x64/v23_clang-18.1.0-rockylinux8/x86_64-unknown-linux-gnu"));
	#endif
		}

		FilteredLogWriter logWriter(g_consoleLogWriter, quiet ? LogEntryType_Warning : LogEntryType_Detail);
		LoggerWithWriter logger(logWriter, TC(""));

		Config config;
		if (!configFile.empty())
			if (!config.LoadFromFile(logger, configFile.c_str()))
				logger.Warning(TC(" Failed to read config file %s"), configFile.c_str());

		if constexpr (!IsArmBinary)
			if (IsRunningArm())
				logger.Warning(TC("  Running x64 binary on arm64 system. Use arm binaries instead"));

		bool exit = false;

		if (fastDelete)
		{
			WorkManagerImpl workManager(maxProcessCount, TC("UbaWrk/DelCas"));
			Atomic<u64> deleteCount = 0;
			DeleteDir(logger, workManager, new DirToDelete{1, nullptr, g_rootDir.ToString()}, deleteCount);
			return true;
		}

		if (!addCas.empty())
		{
			WorkManagerImpl workManager(maxProcessCount);
			StorageCreateInfo storageInfo(g_rootDir.data, logWriter, workManager);
			storageInfo.casCapacityBytes = 0;
			storageInfo.storeCompressed = storeCompressed;
			StorageImpl storage(storageInfo);
			CasKey casKey;
			if (!storage.StoreCasFile(casKey, addCas.c_str(), CasKeyZero, false))
				return false;
			exit = true;
		}

		if (checkCas)
		{
			WorkManagerImpl workManager(maxProcessCount, TC("UbaWrk/ChkCasC"));
			StorageCreateInfo storageInfo(g_rootDir.data, logWriter, workManager);
			storageInfo.casCapacityBytes = 0;
			storageInfo.storeCompressed = storeCompressed;
			StorageImpl storage(storageInfo);
			Vector<TString> badFiles;
			if (!storage.CheckCasContent(&badFiles))
				return false;

			if (deleteCas)
			{
				for (auto& badFile : badFiles)
					DeleteFileW(badFile.c_str());
				deleteCas = false;
			}

			exit = true;
		}

		if (deleteCas)
		{
			WorkManagerImpl workManager(maxProcessCount, TC("UbaWrk/DelCas"));
			StorageImpl(StorageCreateInfo(g_rootDir.data, logWriter, workManager)).DeleteAllCas();
			for (u32 i=0; i!=agentCount; ++i)
			{
				StringBuffer<> clientRootDir;
				clientRootDir.Append(g_rootDir).Append("Agent").AppendValue(i);
				StorageImpl(StorageCreateInfo(clientRootDir.data, logWriter, workManager)).DeleteAllCas();
			}
			exit = true;
		}

		if (!checkFileTable.empty())
		{
			WorkManagerImpl workManager(maxProcessCount, TC("UbaWrk/ChkFTbl"));
			StorageCreateInfo storageInfo(g_rootDir.data, logWriter, workManager);
			storageInfo.casCapacityBytes = 0;
			storageInfo.storeCompressed = storeCompressed;
			StorageImpl storage(storageInfo);
			if (!storage.LoadCasTable())
				return false;
			if (!storage.CheckFileTable(checkFileTable.data()))
				return false;
			exit = true;
		}

		if (checkCas2) // Creates a storage server and storage client and transfer _all_ cas files over network
		{
			NetworkBackendTcp networkBackend(logWriter);
			NetworkServerCreateInfo nsci(logWriter);
			bool ctorSuccess = true;
			NetworkServer server(ctorSuccess, nsci);
			StorageServerCreateInfo storageInfo(server, g_rootDir.data, logWriter);
			storageInfo.casCapacityBytes = 0;
			storageInfo.storeCompressed = storeCompressed;
			StorageServer storageServer(storageInfo);

			StringBuffer<> rootDir2(g_rootDir.data);
			rootDir2.Append("_CHECKCAS2");
			DeleteAllFiles(logger, rootDir2.data);
			Client client;

			auto g = MakeGuard([&]() { server.DisconnectClients(); });
			if (!server.StartListen(networkBackend, 1347, TC("127.0.0.1")))
				return false;
			ClientInitInfo cii { logWriter, networkBackend, rootDir2.data, TC("127.0.0.1"), 1347, TC("foo") };
			cii.createSession = false;
			cii.addDirSuffix = false;
			if (!client.Init(cii))
				return false;
			bool success = true;
			WorkManagerImpl workManager(maxProcessCount, TC("UbaWrk/ChkCas2"));
			storageServer.TraverseAllCasFiles([&](const CasKey& casKey, u64 size)
				{
					workManager.AddWork([&, casKey](const WorkContext&)
						{
							Storage::RetrieveResult res;
							storageServer.EnsureCasFile(casKey, TC("Dummy"));
							if (!client.storageClient->RetrieveCasFile(res, AsCompressed(casKey, false), {}, TC("")))
								success = false;
							if (!client.storageClient->RetrieveCasFile(res, casKey, {}, TC("")))
								success = false;

							#if 0
							StorageStats storageStats;
							FileFetcher fetcher { client.storageClient->m_bufferSlots, storageStats };
							bool destinationIsCompressed = false;
							if (!fetcher.RetrieveFile(logger, *client.networkClient, casKey, TC("e:\\temp\\foo"), destinationIsCompressed))
								success = false;
							#endif
						}, 1, TC("CheckCas2"));
				});
			workManager.FlushWork();
			if (!success)
				return false;
			exit = true;
		}

#if UBA_USE_CLOUD
		if (checkCloud)
		{
			DirectoryCache dirCache;
			dirCache.CreateDirectory(logger, g_rootDir.data);
			Cloud cloud;
			StringBuffer<> info;
			if (cloud.QueryInformation(logger, info, g_rootDir.data))
			{
				logger.Info(TC("We are inside cloud%s (%s)"), info.data, cloud.GetAvailabilityZone());
				
				StringBuffer<> reason;
				u64 terminateTime;
				if (cloud.IsTerminating(logger, reason, terminateTime))
					logger.Info(TC(".. and are being terminated: %s"), reason.data);
			}
			else
				logger.Info(TC("Seems like we are not running inside cloud."));
			exit = true;
		}
#endif
		
		u64 testCompressOriginalSize = 0;
		if (!testCompress.empty())
		{
			WorkManagerImpl workManager(maxProcessCount, TC("UbaWrk/TstComp"));

			FileAccessor fa(logger, testCompress.c_str());
			if (!fa.OpenMemoryRead())
				return logger.Error(TC("Failed to open file %s"), testCompress.c_str());
			u64 fileSize = fa.GetSize();
			u8* mem = fa.GetData();

			testCompressOriginalSize = fileSize;

			StorageCreateInfo storageInfo(g_rootDir.data, logWriter, workManager);
			storageInfo.casCapacityBytes = 0;
			storageInfo.storeCompressed = storeCompressed;
			StorageImpl storage(storageInfo);

			Storage::WriteResult res;
			CompressedFileHeader header { CalculateCasKey(mem, fileSize, true, &workManager, testCompress.c_str()) };

			StringBuffer<> dest;
			dest.Append(storage.GetTempPath()).Append(TCV("TestCompress.tmp"));
			if (!storage.WriteCompressedFile(res, TC("MemoryMap"), InvalidFileHandle, mem, fileSize, dest.data, &header, sizeof(header), 0))
				return false;
			logger.Info(TC("Compressing %s successful (Written to %s)"), testCompress.c_str(), dest.data);
			if (fileToDecompress.empty())
				return true;
			exit = true;
		}

		if (!fileToDecompress.empty())
		{
			WorkManagerImpl workManager(maxProcessCount, TC("UbaWrk/TstDecm"));

			FileAccessor faSource(logger, fileToDecompress.c_str());
			if (!faSource.OpenMemoryRead())
				return logger.Error(TC("Failed to open file %s"), fileToDecompress.c_str());
			u64 fileSize = faSource.GetSize();
			u8* mem = faSource.GetData();

			if (fileSize < 16)
				return logger.Error(TC("File %s is too small to be compressed. Requires at least 16 bytes"), fileToDecompress.c_str());

			StorageCreateInfo storageInfo(g_rootDir.data, logWriter, workManager);
			storageInfo.casCapacityBytes = 0;
			storageInfo.storeCompressed = storeCompressed;
			StorageImpl storage(storageInfo);

			BinaryReader reader(mem, 0, fileSize);

			auto& h = *(CompressedFileHeader*)mem;
			if (h.IsValid())
				reader.Skip(sizeof(CompressedFileHeader));
			else if (decompressInPlace)
			{
				logger.Info(TC("File %s has no uba header. Assumed not compressed (use -testCompress to ignore header check)"), fileToDecompress.c_str());
				return true;
			}

			u64 decompressedSize = reader.ReadU64();

			if (testCompressOriginalSize && decompressedSize != testCompressOriginalSize)
				return logger.Error(TC("Compressed file %s has wrong decompressed size. (Is it compressed?)"), fileToDecompress.c_str());

			if (decompressedSize > InvalidValue)
				return logger.Error(TC("Decompressed size of file %s is way too big (%s). (Is it compressed?)"), fileToDecompress.c_str(), BytesToText(decompressedSize).str);

			StringBuffer<> dest;
			dest.Append(storage.GetTempPath()).Append(TCV("TestDecompress.tmp"));
			FileAccessor faDest(logger, dest.data);
			if (!faDest.CreateMemoryWrite(false, DefaultAttributes(), decompressedSize))
				return false;
			u8* destMem = faDest.GetData();

			OO_SINTa decoredMemSize = OodleLZDecoder_MemorySizeNeeded(OodleLZ_Compressor_Kraken);
			void* decoderMem = malloc(decoredMemSize);
			auto mg = MakeGuard([decoderMem]() { free(decoderMem); });

			u64 decompressedLeft = decompressedSize;
			u32 blockCounter = 0;
			while (reader.GetLeft())
			{
				u32 compressedBlockSize = reader.ReadU32();
				u32 decompressedBlockSize = reader.ReadU32();

				if (decompressedLeft < decompressedBlockSize)
					return logger.Error(TC("File %s is corrupt. Destination size is %llu but block %u overflows (%llu)"), fileToDecompress.c_str(), decompressedSize, blockCounter, decompressedBlockSize);

				logger.Info(TC("Decompressing block %u. %u bytes to %u bytes"), blockCounter, compressedBlockSize, decompressedBlockSize);

				OO_SINTa decompLen = OodleLZ_Decompress(reader.GetPositionData(), (OO_SINTa)compressedBlockSize, destMem, (OO_SINTa)decompressedBlockSize, OodleLZ_FuzzSafe_Yes, OodleLZ_CheckCRC_No, OodleLZ_Verbosity_None, NULL, 0, NULL, NULL, decoderMem, decoredMemSize);
				if (decompLen != decompressedBlockSize)
					return logger.Error(TC("Failed to decompress %s (CompressedSize: %llu DecompressedSize: %llu ReadPos: %llu CompressedBlock: %u DecompressedBlock: %u)"), fileToDecompress.c_str(), fileSize, decompressedSize, reader.GetPosition(), compressedBlockSize, decompressedBlockSize);
				decompressedLeft -= decompressedBlockSize;
				destMem += decompressedBlockSize;
				reader.Skip(compressedBlockSize);
				blockCounter++;
			}

			if (!faDest.Close())
				return false;

			faSource.Close();
			
			if (decompressInPlace)
			{
				if (!CopyFileW(dest.data, fileToDecompress.c_str(), false))
					return logger.Error(TC("Failed to copy %s to %s"), dest.data, fileToDecompress.c_str());
				DeleteFileW(dest.data);
				logger.Info(TC("Decompressing %s successful"), fileToDecompress.c_str());
			}
			else
				logger.Info(TC("Decompressing %s successful (Written to %s)"), fileToDecompress.c_str(), dest.data);
			exit = true;
		}


		u8 crypto128Data[16];
		bool useCrypto = !crypto.empty() || !testEncrypt.empty() || !testDecrypt.empty();
		if (useCrypto)
		{
			if (crypto.empty())
				crypto = TC("3f58aa57466db9999213456789123445");
			if (!CryptoFromString(crypto128Data, 16, crypto.c_str()))
				return logger.Error(TC("Failed to parse crypto key %s"), crypto.c_str());
		}


		if (!testEncrypt.empty())
		{
			FileAccessor fa(logger, testEncrypt.c_str());
			if (!fa.OpenMemoryRead())
				return logger.Error(TC("Failed to open file %s"), testEncrypt.c_str());
			u64 fileSize = fa.GetSize();

			StringBuffer<> dest;
			dest.Append(g_rootDir).EnsureEndsWithSlash().Append(TCV("TestEncrypt.tmp"));
			FileAccessor destFa(logger, dest.data);
			if (!destFa.CreateMemoryWrite(false, DefaultAttributes(), fileSize))
				return false;
			memcpy(destFa.GetData(), fa.GetData(), fileSize);

			CryptoKey key = Crypto::CreateKey(logger, crypto128Data);
			if (key == InvalidCryptoKey)
				return false;

			Guid iv = *(Guid*)crypto128Data;
			if (!Crypto::Encrypt(logger, key, destFa.GetData(), u32(fileSize), iv))
				return false;

			if (!destFa.Close())
				return false;
			logger.Info(TC("Encryption of %s successful (Written to %s)"), testEncrypt.c_str(), dest.data);
			exit = true;
		}

		if (!testDecrypt.empty())
		{
			FileAccessor fa(logger, testDecrypt.c_str());
			if (!fa.OpenMemoryRead())
				return logger.Error(TC("Failed to open file %s"), testDecrypt.c_str());
			u64 fileSize = fa.GetSize();

			StringBuffer<> dest;
			dest.Append(g_rootDir).EnsureEndsWithSlash().Append(TCV("TestDecrypt.tmp"));
			FileAccessor destFa(logger, dest.data);
			if (!destFa.CreateMemoryWrite(false, DefaultAttributes(), fileSize))
				return false;
			memcpy(destFa.GetData(), fa.GetData(), fileSize);

			CryptoKey key = Crypto::CreateKey(logger, crypto128Data);
			if (key == InvalidCryptoKey)
				return false;

			Guid iv = *(Guid*)crypto128Data;
			if (!Crypto::Decrypt(logger, key, destFa.GetData(), u32(fileSize), iv))
				return false;

			if (!destFa.Close())
				return false;
			logger.Info(TC("Decryption of %s successful (Written to %s)"), testDecrypt.c_str(), dest.data);
			exit = true;
		}

		if (!symbolFile.empty())
		{
			FileInformation info;
			GetFileInformation(info, logger, symbolFile);
			u64 fakeStartAddress = 0x800000;

			StackBinaryWriter<128> writer;
			#if PLATFORM_WINDOWS
			writer.WriteBool(false);
			#endif
			writer.Write7BitEncoded(1); // Callstack count
			writer.Write7BitEncoded(0); // Module index
			writer.Write7BitEncoded(symbolOffset); // Module offset

			writer.Write7BitEncoded(1); // Module count
			writer.Write7BitEncoded(fakeStartAddress); // Start
			writer.Write7BitEncoded(info.size); // Size
			writer.WriteString(symbolFile); // Name

			BinaryReader reader(writer.GetData(), 0, writer.GetPosition());
			StringBuffer<> callstack;

			StringView search[] = { 
			#if PLATFORM_WINDOWS
				TCV("srv*C:\\Symbols*https://msdl.microsoft.com/download/symbols"),
			#endif
				{}
			};

			ParseCallstackInfo(callstack, reader, nullptr, search, false);
			logger.Log(LogEntryType_Info, callstack.data, callstack.count);
			return true;
		}

		if (exit)
			return true;

		// -patchbinary runs in utility mode (no command type required): takes a
		// statically-linked ELF and produces a detour-patched copy suitable for
		// running inside UBA.
		#if PLATFORM_LINUX
		if (!patchBinaryIn.empty())
		{
			if (patchBinaryOut.empty())
				return logger.Error(TC("-patchbinary requires -out=<output path>"));

			StringBuffer<> stubPath;
			GetDirectoryOfCurrentModule(logger, stubPath);
			stubPath.EnsureEndsWithSlash().Append(TCV("UbaStaticStub.bin"));

			FileAccessor fa(logger, patchBinaryIn.c_str());
			if (!fa.OpenMemoryRead(0, false))
				return logger.Error(TC("Failed to open file %s (%s)"), patchBinaryIn.c_str(), LastErrorToText().data);
			return PatchStaticBinary(logger, patchBinaryIn, fa.GetData(), fa.GetSize(), patchBinaryOut, stubPath);
		}
		#endif

		if (commandType == CommandType_NotSet)
		{
			const tchar* errorMsg = argc == 1 ? TC("") : TC("\nERROR: First argument must be command type. Options are 'local,remote or native'");
			StringBuffer<> msg;
			return PrintHelp(errorMsg);
		}

		bool isSpeedTest = StringView(application).Equals(TCV("speedTest"));
		if (isSpeedTest)
			if (commandType != CommandType_Agent && commandType != CommandType_Remote)
				return logger.Error(TC("Speedtest does can only run in \"remote\" or \"agent\" mode"));

		StringBuffer<512> currentDir;
		GetCurrentDirectoryW(currentDir);

		if (commandType != CommandType_None)
		{
			if (application.empty())
				return PrintHelp(TC("No executable provided"));

			if (!IsAbsolutePath(application.c_str()) && !isSpeedTest)
			{
				StringBuffer<> ubaCliPath;
				GetDirectoryOfCurrentModule(logger, ubaCliPath);
				StringBuffer<> fullApplicationName;
				if (!SearchPathForFile(logger, fullApplicationName, application, currentDir, ubaCliPath))
					return logger.Error(TC("Failed to find full path to %s"), application.c_str());
				application = fullApplicationName.data;
			}

			if (getCas)
			{
				StringKey nameKey = CaseInsensitiveFs ? ToStringKeyLower(application) : ToStringKey(application);
				FileAccessor fa(logger, application.c_str());
				if (!fa.OpenMemoryRead(0, false))
					return logger.Error(TC("Failed to open file %s (Key: %s) - %s"), application.c_str(), KeyToString(nameKey).data, LastErrorToText().data);
				u64 fileSize = fa.GetSize();
				u8* data = fa.GetData();
				bool is64Bit = false;
				bool isArm64 = false;
				bool isX64 = false;
				bool isDotnet = false;

				CasKey key = CalculateCasKey(data, fileSize, false, nullptr, application.c_str());
				CasKey uncompressedKey;
				if (fileSize > sizeof(CompressedFileHeader))
				{
					auto& hdr = *(CompressedFileHeader*)data;
					if (hdr.IsValid())
						uncompressedKey = hdr.casKey;
				}

				if (data[0] == 'M' && data[1] == 'Z')
				{
					u32 offset = *(u32*)(data + 0x3c);
					u32* signaturePos = (u32*)(data + offset);
					
					bool isPEImage = *signaturePos == 0x00004550;
					if (isPEImage)
					{
						u16 machine = *(u16*)(signaturePos + 1);
						isX64 = machine == 0x8664;
						isArm64 = machine == 0xaa64;

						u16 magic = *(u16*)(data + offset + 0x18);
						is64Bit = (magic == 0x20B);

						if (fileSize > offset + 0x18 + 0x70 + 4)
							isDotnet = *(u32*)(data + offset + 0x18 + 0x70);
					}
				}
				logger.Info(TC("%s (Key: %s)"), application.c_str(), KeyToString(nameKey).data);
				logger.Info(TC("  Is64Bit: %s"), (is64Bit ? TC("true") : TC("false")));
				logger.Info(TC("  Arch: %s"), (isX64 ? TC("x64") : (isArm64 ? TC("arm64") : (isDotnet ? TC(".net") : TC("unknown")))));
				logger.Info(TC("  Size: %llu"), fileSize);
				logger.Info(TC("  CasKey: %s"), CasKeyString(key).str);
				if (uncompressedKey != CasKeyZero)
					logger.Info(TC("  CasKey (uncompressed): %s"), CasKeyString(uncompressedKey).str);
				return true;
			}
		}

		if (listImports)
		{
			StringBuffer<> error;
			bool printImports = true;
			BinaryInfo info;
			if (!ParseBinary(application, StringView(application).GetPath(), info, [&](const tchar* import, bool isKnown, const tchar* const* loaderPaths)
			{
				if (printImports)
				{
					if (loaderPaths && *loaderPaths)
					{
						logger.Info(TC("LoaderPaths:"));
						for (auto it=loaderPaths;*it; ++it)
							if (**it)
								logger.Info(TC("  %s"), *it);
					}
					printImports = false;
					logger.Info(TC("Imports:"));
				}
				logger.Info(TC("  %s"), import);

			}, error) || error.count)
				return logger.Error(TC("%s"), error.data);
			return true;
		}

		if (validateBinary)
		{
			#if PLATFORM_MAC
			BinaryInfo info;
			StringBuffer<> error;
			if (ParseBinary(application, StringView(application).GetPath(), info, [&](const tchar* import, bool isKnown, const tchar* const* loaderPaths) {}, error))
				logger.Info(TC("MinOsVersion: %u.%u.%u"), (info.minVersion >> 16) & 0xffff, (info.minVersion >> 8) & 0xff, info.minVersion & 0xff);
			return ExplainDyldEnvSupport(logger, application.c_str());
			#else
			return true;
			#endif
		}

		const tchar* dbgStr = TC("");
		#if UBA_DEBUG
		dbgStr = TC(" (DEBUG)");
		#endif
		logger.Info(TC("UbaCli v%s%s (Rootdir: \"%s\", StoreCapacity: %uGb)\n"), Version, dbgStr, g_rootDir.data, storageCapacityGb);

		u64 storageCapacity = u64(storageCapacityGb)*1000*1000*1000;

		if (workDir.IsEmpty())
			workDir.Append(currentDir);

		// TODO: Change workdir to make it full

		#if UBA_DEBUG
		logToFile = true;
		#endif

		StringBuffer<> logFile;
		if (logToFile)
		{
			logFile.count = GetFullPathNameW(g_rootDir.data, logFile.capacity, logFile.data, nullptr);
			logFile.EnsureEndsWithSlash().Append(TCV("debuglog.log"));
			logger.Info(TC("Logging to file: %s"), logFile.data);
		}

		#if PLATFORM_WINDOWS
		SetConsoleCtrlHandler(ConsoleHandler, TRUE);
		#else
		signal(SIGINT, ConsoleHandler);
		signal(SIGTERM, ConsoleHandler);
		#endif

		NetworkBackendTcpCreateInfo nbtci(logWriter);
		NetworkBackendTcp networkBackendTcp(nbtci);
		NetworkBackendMemory networkBackendMemory(logWriter);

		NetworkBackend& networkBackend = useMemsock ? (NetworkBackend&)networkBackendMemory : (NetworkBackend&)networkBackendTcp;

		RootsHandle rootsHandle = 0;
		CacheClient* cacheClient = nullptr;
		Function<bool(Scheduler*)> createCoordinator;
		Function<void()> destroyCoordinator;

		bool isRemote = commandType == CommandType_Remote || commandType == CommandType_Agent;

		auto RunSession = [&](const Function<bool(SessionServer& sessionServer)>& func)
		{
			NetworkServerCreateInfo nsci(logWriter);
			nsci.useThreadedSend = true;
			nsci.Apply(config);

			//nsci.workerCount = 4;
			bool ctorSuccess = true;
			NetworkServer& networkServer = *new NetworkServer(ctorSuccess, nsci);
			auto destroyServer = MakeGuard([&]() { delete &networkServer; });
			if (!ctorSuccess)
				return false;

			if (useCrypto)
			{
				networkServer.RegisterCryptoKey(crypto128Data);
				logger.Info(TC("Using crypto key %s for connections"), crypto.c_str());
			}

			if (StringView(application).EndsWith(TCV(".yaml")))
				useScheduler = true;

			StorageServerCreateInfo storageInfo(networkServer, g_rootDir.data, logWriter);
			storageInfo.casCapacityBytes = storageCapacity;
			storageInfo.storeCompressed = storeCompressed;
			storageInfo.Apply(config);
			StorageServer& storageServer = *new StorageServer(storageInfo);
			auto destroyStorage = MakeGuard([&]() { delete &storageServer; });

			SessionServerCreateInfo info(storageServer, networkServer, logWriter);
			info.useUniqueId = useUniqueId || useScheduler;
			info.traceEnabled = cacheCommand.empty() || writeCacheSummary;
			info.detailedTrace = detailedTrace;
			info.traceChildProcesses = traceChildProcesses;
			info.launchVisualizer = launchVisualizer;
			info.allowCustomAllocator = !disableCustomAllocator;
			//info.shouldWriteToDisk = shouldWriteToDisk;
			info.rootDir = g_rootDir.data;
			//info.traceName.Append(TCV("TESTTRACE"));
			//info.storeIntermediateFilesCompressed = false;
			info.readIntermediateFilesCompressed = true;
			//info.treatTempDirAsEmpty = false;
			//info.extractObjFilesSymbols = true;
			info.writeFilesBottleneck = 24;
			info.writeFilesFileMapMaxMb = 1;

			#if UBA_DEBUG_LOG_ENABLED
			info.remoteLogEnabled = true;
			#endif
			//info.remoteTraceEnabled = true;

			info.deleteSessionsOlderThanSeconds = info.useUniqueId ? 0 : 1;
			info.Apply(config);

			SessionServer& sessionServer = *new SessionServer(info);
			auto destroySession = MakeGuard([&]()
				{
					#if !UBA_DEBUG
					if (info.useUniqueId)
						DeleteAllFiles(logger, sessionServer.GetSessionDir());
					#endif
					delete &sessionServer;
				});

			cacheClient = nullptr;
			auto ccg = MakeGuard([&]() { if (!cacheClient) return; auto& nc = cacheClient->GetClient(); nc.Disconnect(); delete cacheClient; delete &nc; });

			auto CreateCacheClient = [&]()
				{
					NetworkClientCreateInfo nci(logWriter);
					if (!crypto.empty())
						nci.cryptoKey128 = crypto128Data;
					auto nc = new NetworkClient(ctorSuccess, nci);
					cacheClient = new CacheClient({logWriter, storageServer, *nc, sessionServer});
				};

			if (cacheHost.count)
			{
				CreateCacheClient();
				if (!cacheClient->GetClient().Connect(networkBackend, cacheHost.data, cachePort))
					return logger.Error(TC("Failed to connect to cache server"));

				if (!storageServer.LoadCasTable(true))
					return false;

				if (!cacheCommand.empty())
				{
					LoggerWithWriter consoleLogger(g_consoleLogWriter);
					const tchar* additionalInfo = nullptr;
					return cacheClient->ExecuteCommand(consoleLogger, cacheCommand.data(), nullptr, additionalInfo);
				}

				if (writeCacheSummary)
				{
					StringBuffer<> tempFile(sessionServer.GetTempPath());
					Guid guid;
					CreateGuid(guid);
					tempFile.Append(GuidToString(guid).str).Append(TCV(".txt"));
					if (!cacheClient->ExecuteCommand(logger, TC("content"), tempFile.data, cacheFilterString.data()))
						return false;
					logger.Info(TC("Cache status summary written to %s"), tempFile.data);

					#if PLATFORM_WINDOWS
					ShellExecuteW(NULL, L"open", tempFile.data, NULL, NULL, SW_SHOW);
					#endif
					return true;
				}
			}

			// Remove empty spaces and line feeds etc at the end.. just to solve annoying copy paste command lines and accidentally getting line feed
			while (!arguments.empty())
			{
				tchar lastChar = arguments[arguments.size()-1];
				if (lastChar != '\n' && lastChar != '\r' && lastChar != '\t' && lastChar != ' ')
					break;
				arguments.resize(arguments.size() - 1);
			}

			// Vfs testing
			rootsHandle = 0;

			if (!vfsEntries.empty())
			{
				StackBinaryWriter<8*1024> writer;
				for (VfsEntry& entry : vfsEntries)
				{
					writer.WriteByte(0);
					writer.WriteString(entry.virtualPath);
					writer.WriteString(entry.localPath);
				}
				rootsHandle = sessionServer.RegisterRoots(writer.GetData(), writer.GetPosition());
				sessionServer.DevirtualizeString(application, rootsHandle, true);
			}

			for (CrossEntry& entry : crossEntries)
				if (!sessionServer.RegisterCrossArchitectureMapping(entry.from.c_str(), entry.to.c_str()))
					return false;

			if (isRemote || useScheduler)
			{
				if (!storageServer.m_casTableLoaded)
					if (!storageServer.LoadCasTable(true))
						return false;
				if (!networkServer.StartListen(networkBackend, port, listenIp.data))
					return false;
			}

			CoordinatorWrapper coordinator;
			auto cg = MakeGuard([&]() { coordinator.Destroy(); });

			createCoordinator = [&](Scheduler* scheduler)
				{
					if (coordinatorName.empty())
						return true;
					StringBuffer<512> coordinatorWorkDir(g_rootDir);
					coordinatorWorkDir.EnsureEndsWithSlash().Append(coordinatorName);
					StringBuffer<512> binariesDir;
					if (!GetDirectoryOfCurrentModule(logger, binariesDir))
						return false;

					CoordinatorCreateInfo cinfo;
					cinfo.workDir = coordinatorWorkDir.data;
					cinfo.binariesDir = binariesDir.data;

					cinfo.logging = false;
					if (!coordinator.Create(logger, coordinatorName.c_str(), cinfo, networkBackend, networkServer, sessionServer.GetTrace()))
						return false;
					coordinator.SetScheduler(scheduler);
					coordinator.RequestHelpers(1);
					return true;
				};
			destroyCoordinator = [&]() { coordinator.Destroy(); };

			auto stopServer = MakeGuard([&]() { networkServer.DisconnectClients(); });
			auto stopListen = MakeGuard([&]() { networkBackend.StopListen(); });

			bool res = func(sessionServer);

			logger.BeginScope();
			if (printSummary)
			{
				sessionServer.PrintSummary(logger);
				storageServer.PrintSummary(logger);
				networkServer.PrintSummary(logger);
				KernelStats::GetGlobal().Print(logger, true);
				PrintContentionSummary(logger);
			}
			logger.EndScope();

			return res;
		};

		auto RunLocal = [&](SessionServer& sessionServer, const TString& app, const TString& arg, bool enableDetour)
		{
			u64 start = GetTime();

			ProcessStartInfo pinfo;
			pinfo.description = app.c_str();
			pinfo.application = app.c_str();
			pinfo.arguments = arg.c_str();
			pinfo.workingDir = workDir.data;
			pinfo.rootsHandle = rootsHandle;

			u32 bucketId = 1337;
			if (cacheClient)
			{
				CacheResult cacheResult;
				cacheClient->FetchFromCache(cacheResult, RootPaths(), bucketId, pinfo);
				if (cacheResult.hit)
				{
					logger.Info(TC("Cached run took %s"), TimeToText(GetTime() - start).str);
					return true;
				}
			}

			pinfo.logFile = logFile.data;
			if (enableStdOut)
				pinfo.logLineFunc = [](void* userData, const tchar* line, u32 length, LogEntryType type)
				{
					auto s = type == LogEntryType_Error ? stderr : stdout;
					TFputs(line, s);
					TFputs(TC("\n"), s);
				};
			if (populateCache)
				pinfo.trackInputs = true;
			logger.Info(TC("Running %s %s"), app.c_str(), arg.c_str());
			ProcessHandle process = sessionServer.RunProcess(pinfo, false, enableDetour);
			if (process.GetExitCode() != 0)
				return logger.Error(TC("Error exit code: %u"), process.GetExitCode());
			logger.Info(TC("%s run took %s"), (enableDetour ? TC("Detoured") : TC("Native")), TimeToText(GetTime() - start).str);

			if (populateCache)
			{
				return logger.Error(TC("Populating cache not implemented... todo"));
				//RootPaths rootPaths;
				//cacheClient->WriteToCache(rootPaths, 0, pinfo, nullptr, 1, nullptr, 0, nullptr, 0);
			}
			return true;
		};

		auto RunRemote = [&](SessionServer& sessionServer, const TString& app, const TString& arg)
		{
			bool allowCross = true;

			u64 start = GetTime();
			ProcessStartInfo pinfo;
			pinfo.description = app.c_str();
			pinfo.application = app.c_str();
			pinfo.arguments = arg.c_str();
			pinfo.workingDir = workDir.data;
			pinfo.logFile = logFile.data;
			pinfo.logLineUserData = &logger;
			pinfo.rootsHandle = rootsHandle;
			if (enableStdOut)
				pinfo.logLineFunc = [](void* userData, const tchar* line, u32 length, LogEntryType type) { ((Logger*)userData)->Log(type, line, length); };
			logger.Info(TC("Running %s %s"), app.c_str(), arg.c_str());
			ProcessHandle process = sessionServer.RunProcessRemote(pinfo, 1.0f, nullptr, 0, allowCross, allowCross);
			process.WaitForExit(~0u);
			if (process.GetExitCode() != 0)
				return logger.Error(TC("Error exit code: %u"), process.GetExitCode());
			u64 time = GetTime() - start;
			logger.Info(TC("Remote run took %s"), TimeToText(time).str);
			return true;
		};

		const tchar* clientZone = TC("DummyZone");

		auto RunWithClient = [&](const Function<bool()>& func, u32 clientCount, Scheduler* scheduler)
			{
				List<Client> clients;
				for (u32 i=0; i!=clientCount; ++i)
				{
					u32 maxProcessor = Min(maxProcessCount/clientCount, 32u);
					ClientInitInfo cii { logWriter, networkBackend, g_rootDir.data, TC("127.0.0.1"), port, clientZone, maxProcessor, i};
					cii.useDependencyCrawler = true;
					if (!clients.emplace_back().Init(cii))
						return false;
				}

				#if 0
				Thread disconnectThread([&]()
					{
						Sleep((20 + (rand() % 5)) * 1000);
						clients.front().networkClient->Disconnect();
						if (scheduler)
							scheduler->SetMaxLocalProcessors(100);
						return true;
					});
				#endif
				return func();
			};

		auto RunAgent = [&](SessionServer& sessionServer, const TString& app, const TString& arg)
		{
			return RunWithClient([&]() { return RunRemote(sessionServer, app, arg); }, 1, nullptr);
		};


		auto RunScheduler = [&](SessionServer& sessionServer, const TString& app, const TString& arg)
		{
			#if 0
			bool ctorSuccess;
			NetworkBackendMemory nbm(logWriter);
			
			StringBuffer<> cacheRootDir(g_rootDir);
			cacheRootDir.Append("CacheServer");
			NetworkServer cacheNetworkServer(ctorSuccess);
			StorageServerCreateInfo storageInfo2(cacheNetworkServer, cacheRootDir.data, logWriter);
			storageInfo2.writeReceivedCasFilesToDisk = true;
			StorageServer cacheStorageServer(storageInfo2);
			CacheServer cacheServer(logWriter, cacheRootDir.data, cacheNetworkServer, cacheStorageServer);
			auto csg = MakeGuard([&]() { cacheServer.Save(); });

			NetworkClient cacheNetworkClient(ctorSuccess);
			CacheClient cacheClient(logWriter, storageServer, cacheNetworkClient, sessionServer);

			if (false)
			{
				cacheServer.Load();

				cacheNetworkServer.StartListen(nbm);
				cacheNetworkClient.Connect(nbm, TC("127.0.0.1"));
			}
			auto g = MakeGuard([&]() { cacheNetworkClient.Disconnect(); cacheNetworkServer.DisconnectClients(); });
			#endif


			auto g = MakeGuard([&]() { if (cacheClient) cacheClient->GetClient().Disconnect(); });

			bool success = true;
			Atomic<u32> counter = 0;
			Event finished(EventResetType_Manual);

			CacheClient* cacheClients[] = { cacheClient };
			SchedulerCreateInfo info(sessionServer);
			info.Apply(config);
			info.forceRemote = isRemote;
			info.forceNative = commandType == CommandType_Native;
			info.maxLocalProcessors = maxProcessCount;
			//info.maxRacingPercent = 50u;
			//info.memWatchdog = true;
			//info.memTracingLevel = 1;
			info.cacheClients = cacheClients;
			info.cacheClientCount = cacheClient ? 1 : 0;
			info.writeToCache = populateCache;
			info.writeTrackedInputs = true;
			Scheduler scheduler(info);

			//agentCount = 2;

			if (!createCoordinator(&scheduler))
				return false;
			auto coordinatorGuard = MakeGuard([&]() { destroyCoordinator(); });

			if (StringView(app).EndsWith(TCV(".yaml")))
			{
				if (!scheduler.EnqueueFromFile(app.c_str(), [&](EnqueueProcessInfo& epi)
					{
						if (rootsHandle)
							const_cast<ProcessStartInfo&>(epi.info).rootsHandle = rootsHandle;
					}))
					return false;
			}
			else
			{
				ProcessStartInfo pinfo;
				pinfo.description = app.c_str();
				pinfo.application = app.c_str();
				pinfo.arguments = arg.c_str();
				pinfo.workingDir = workDir.data;
				pinfo.rootsHandle = rootsHandle;
				scheduler.EnqueueProcess({pinfo});
			}

			u32 queued, activeLocal, activeRemote, outFinished;
			scheduler.GetStats(queued, activeLocal, activeRemote, outFinished);

			scheduler.SetProcessFinishedCallback([&](const ProcessHandle& ph)
				{
					auto& si = ph.GetStartInfo();
					const tchar* desc = si.description;
					if (ph.GetExitCode() != 0 && ph.GetExitCode() != ProcessCancelExitCode)
					{
						logger.Error(TC("%s - Error exit code: %u (%s %s)"), desc, ph.GetExitCode(), si.application, si.arguments);
						success = false;
					}
					u32 c = ++counter;
					logger.BeginScope();
					StringBuffer<128> extra;
					if (ph.IsRemote())
						extra.Append(TCV(" [RemoteExecutor: ")).Append(ph.GetExecutingHost()).Append(']');
					else if (ph.GetExecutionType() == ProcessExecutionType_Native)
						extra.Append(TCV(" (Not detoured)"));
					else if (ph.GetExecutionType() == ProcessExecutionType_FromCache)
						extra.Append(TCV(" (From cache)"));
					logger.Info(TC("[%u/%u] %s%s"), c, queued, desc, extra.data);
					for (auto& line : ph.GetLogLines())
						if (line.text != desc && !StartsWith(line.text.c_str(), TC("   Creating library")))
							logger.Log(line.type, line.text.c_str(), u32(line.text.size()));
					logger.EndScope();

					if (c == queued)
						finished.Set();
				});

			auto RunQueue = [&]()
				{
					logger.Info(TC("Running Scheduler with %u processes"), queued);
					u64 start = GetTime();
					scheduler.Start();
					if (!finished.IsSet())
						return false;
					u64 time = GetTime() - start;
					logger.Info(TC("Scheduler run took %s"), TimeToText(time).str);
					logger.Info(TC(""));
					sessionServer.GetServer().DisconnectClients();
					return success;
				};

			if (commandType == CommandType_Agent)
			{
				u32 clientCount = maxProcessCount == 1 ? 1 : agentCount;
				return RunWithClient([&]() { return RunQueue(); }, clientCount, &scheduler);
			}
			else
				return RunQueue();
		};

#if PLATFORM_WINDOWS	// Annoying that link.exe/lld-link.exe needs path to windows folder.. 
		if (!useScheduler)
		{
			StringBuffer<512> sdkbin;
			//sdkbin.count = GetEnvironmentVariable(TC("WindowsSdkVerBinPath"), sdkbin.data, sdkbin.capacity);
			sdkbin.Append(TCV(";C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.22621.0\\x64"));
			if (sdkbin.count)
			{
				StringBuffer<4096> temp;
				temp.count = GetEnvironmentVariableW(TC("PATH"), temp.data, temp.capacity);
				temp.Append(sdkbin);
				SetEnvironmentVariableW(TC("PATH"), temp.data);
			}
		}
#endif

		bool success = true;
		u32 procLoopCount = 1;

		for (u32 i=0; i!=loopCount; ++i)
		{
			if (i != 0)
			{
				logger.Info(TC(""));
				logger.Info(TC("RUN #%u"), i+1);
			}

			success = RunSession([&](SessionServer& sessionServer)
				{
					if (useScheduler)
						return RunScheduler(sessionServer, application, arguments);
					if (!createCoordinator(nullptr))
						return false;
					bool res = false;

					// Don't want to retry
					if (commandType == CommandType_Remote)
					{
						sessionServer.SetRemoteProcessReturnedEvent([&](Process& process)
							{
								if (sessionServer.WasEverConnected())
									process.Cancel();
							});
					}

					for (u32 procIt=0; procIt!=procLoopCount; ++procIt)
					{
						switch (commandType)
						{
						case CommandType_Native:
							res = RunLocal(sessionServer, application, arguments, false);
							break;
						case CommandType_Local:
							res = RunLocal(sessionServer, application, arguments, true);
							break;
						case CommandType_Remote:
							res = RunRemote(sessionServer, application, arguments);
							break;
						case CommandType_Agent:
							res = RunAgent(sessionServer, application, arguments);
						}
						if (!res)
							return false;
					}
					return true;
				});
			if (!success)
				break;
			//if (false)
			//	networkServer.DisconnectClients();
		}
		return success;
	}
}

#if PLATFORM_WINDOWS
int wmain(int argc, wchar_t* argv[])
{
	int res;
	//while (true)
	{
		res = uba::WrappedMain(argc, argv) ? 0 : -1;
	}
	Sleep(1); // Here to be able to put a breakpoint just before exit :-)
	return res;
}
#else
int main(int argc, char* argv[])
{
	return uba::WrappedMain(argc, argv) ? 0 : -1;
}
#endif
