// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaNinjaParser.h"
#include "UbaCacheClient.h"
#include "UbaCacheServer.h"
#include "UbaConfig.h"
#include "UbaCoordinatorWrapper.h"
#include "UbaDefaultConstants.h"
#include "UbaFile.h"
#include "UbaLogger.h"
#include "UbaNetworkBackendMemory.h"
#include "UbaNetworkBackendTcp.h"
#include "UbaNetworkClient.h"
#include "UbaNetworkServer.h"
#include "UbaPathUtils.h"
#include "UbaScheduler.h"
#include "UbaSessionServer.h"
#include "UbaStorageServer.h"
#include "UbaNinjaBuild.h"
#include "UbaNinjaDepsLog.h"

#define UBA_USE_LOCAL_CACHE_SERVER 0
#define UBA_FORCE_REMOTE 0
#define UBA_USE_LOCAL_CLIENT 0
#define UBA_USE_HORDE 1

#if UBA_USE_LOCAL_CLIENT
#include "../../Cli/Private/UbaClient.h"
#endif

namespace uba
{
	Atomic<bool> g_shouldExit = false;
	Atomic<bool> g_ctrlBreakPressed = false;
	Event g_processStateChanged(EventResetType_Auto);

	void CtrlBreakPressed()
	{
		if (g_ctrlBreakPressed)
			FatalError(13, TC("Force terminate"));
		g_shouldExit = true;
		g_ctrlBreakPressed = true;
		g_processStateChanged.Set();
	}

	bool PrintHelp(const tchar* message)
	{
		LoggerWithWriter logger(g_consoleLogWriter, TC(""));
		if (*message)
		{
			logger.Info(TC(""));
			logger.Error(TC("%s"), message);
		}

		logger.Info(TC(""));
		logger.Info(TC("-------------------------------------------"));
		logger.Info(TC("   UbaNinja - Ninja Build Accelerator"));
		logger.Info(TC("-------------------------------------------"));
		logger.Info(TC(""));
		logger.Info(TC("  UbaNinja.exe [options...] [targets...]"));
		logger.Info(TC(""));
		logger.Info(TC("  Options:"));
		logger.Info(TC("   -C <dir>                    Change to directory before reading build.ninja"));
		logger.Info(TC("   -f <file>                   Specify input build file (default: build.ninja)"));
		logger.Info(TC("   -targets=<target1,target2>  Build specific targets (comma separated)"));
		logger.Info(TC("   -clean                      Delete all output files without building"));
		logger.Info(TC("   -dry-run                    Parse and enqueue but don't execute processes"));
		logger.Info(TC("   -rebuild                    Clean all outputs before building"));
		logger.Info(TC("   -no-up-to-date              Skip the up-to-date check (enqueue every needed edge)"));
		logger.Info(TC("   -v                          Verbose: print the fully-expanded command for each edge"));
		logger.Info(TC("   -show-includes              Do not strip /showIncludes from compiler commands"));
		logger.Info(TC("   -write-yaml=<path>          Write all build commands to a YAML file"));
		logger.Info(TC(""));
		logger.Info(TC("  Examples:"));
		logger.Info(TC("   UbaNinja.exe -C out/Default chrome"));
		logger.Info(TC(""));

		return false;
	}

	bool WrappedMain(int argc, tchar* argv[])
	{
		u64 startTime = GetTime();

		AddExceptionHandler();
		InitMemory();
		SetMemoryWorkingSet(DefaultMemoryWorkingSet);

		FilteredLogWriter logWriter(g_consoleLogWriter, LogEntryType_Info);
		LoggerWithWriter logger(logWriter, TC(""));

		TString ninjaFile;
		TString buildDir;
		bool cleanOnly = false;
		bool dryRun = false;
		TString yamlFile;
		bool rebuild = false;
		bool noUpToDate = true; // TEMPORARY OFF FOR NOW.. NOT WORKING FULLY false;
		bool verbose = false;
		bool noShowIncludes = true;
		Vector<TString> targets;

		// Parse arguments
		for (int i = 1; i < argc; ++i)
		{
			const tchar* argStr = argv[i];
			StringView arg(ToView(argStr));

			if (TStrcmp(argStr, TC("-help")) == 0 || TStrcmp(argStr, TC("--help")) == 0 ||
			    TStrcmp(argStr, TC("-h")) == 0 || TStrcmp(argStr, TC("/?")) == 0)
			{
				return PrintHelp(TC(""));
			}
			else if (TStrcmp(argStr, TC("-C")) == 0)
			{
				if (i + 1 >= argc)
					return PrintHelp(TC("-C requires a directory argument"));
				buildDir = argv[++i];
			}
			else if (TStrcmp(argStr, TC("-f")) == 0)
			{
				if (i + 1 >= argc)
					return PrintHelp(TC("-f requires a file argument"));
				ninjaFile = argv[++i];
			}
			else if (arg.StartsWith(TCV("-targets=")))
			{
				// Parse comma-separated targets
				const tchar* p = arg.data + 9;
				const tchar* end = arg.data + arg.count;
				const tchar* start = p;

				while (p <= end)
				{
					if (p == end || *p == ',')
					{
						if (start < p)
							targets.push_back(TString(start, u32(p - start)));
						if (p < end)
							++p;
						start = p;
					}
					else
						++p;
				}
			}
			else if (TStrcmp(argStr, TC("-clean")) == 0)
			{
				cleanOnly = true;
			}
			else if (TStrcmp(argStr, TC("-dry-run")) == 0)
			{
				dryRun = true;
			}
			else if (TStrcmp(argStr, TC("-rebuild")) == 0)
			{
				rebuild = true;
			}
			else if (TStrcmp(argStr, TC("-no-up-to-date")) == 0)
			{
				noUpToDate = true;
			}
			else if (TStrcmp(argStr, TC("-v")) == 0)
			{
				verbose = true;
			}
			else if (TStrcmp(argStr, TC("-show-includes")) == 0)
			{
				noShowIncludes = false;
			}
			else if (arg.StartsWith(TCV("-write-yaml=")))
			{
				yamlFile = arg.data + 12;
			}
			else if (argStr[0] != '-')
			{
				// If no ninja file specified yet and this looks like a path, treat as ninja file
				if (ninjaFile.empty() && (TStrstr(argStr, TC(".ninja")) || TStrstr(argStr, TC("\\")) || TStrstr(argStr, TC("/"))))
					ninjaFile = argStr;
				else
					targets.push_back(argStr);
			}
			else
			{
				StringBuffer<> msg;
				msg.Appendf(TC("Unknown option: %s"), arg.data);
				return PrintHelp(msg.data);
			}
		}

		// Handle -C directory change
		if (!buildDir.empty())
		{
			// Make buildDir absolute if it's relative
			if (!IsAbsolutePath(buildDir.c_str()))
			{
				StringBuffer<> cwd;
				if (!GetCurrentDirectoryW(cwd))
					return logger.Error(TC("Failed to get current directory"));

				StringBuffer<> fullPath;
				fullPath.Append(cwd).EnsureEndsWithSlash().Append(buildDir);
				buildDir = fullPath.data;
			}

			// If no ninja file specified, look for build.ninja in buildDir
			if (ninjaFile.empty())
			{
				StringBuffer<> temp;
				temp.Append(buildDir).EnsureEndsWithSlash().Append(TCV("build.ninja"));

				if (FileExists(logger, temp.data))
					ninjaFile = temp.data;
				else
					return PrintHelp(TC("build.ninja not found in specified directory"));
			}
			else
			{
				// Make ninja file relative to buildDir
				if (!IsAbsolutePath(ninjaFile.c_str()))
				{
					StringBuffer<> fullPath;
					fullPath.Append(buildDir).EnsureEndsWithSlash().Append(ninjaFile);
					ninjaFile = fullPath.data;
				}
			}
		}

		if (ninjaFile.empty())
		{
			// Check if build.ninja exists in current directory
			StringBuffer<> cwd;
			if (!GetCurrentDirectoryW(cwd))
				return PrintHelp(TC("Failed to get current directory"));
			cwd.EnsureEndsWithSlash().Append(TC("build.ninja"));

			if (FileExists(logger, cwd.data))
				ninjaFile = cwd.data;
			else
				return PrintHelp(TC("No ninja file specified and build.ninja not found in current directory"));
		}

		// Make path absolute if it's relative
		if (!IsAbsolutePath(ninjaFile.c_str()))
		{
			StringBuffer<> cwd;
			if (!GetCurrentDirectoryW(cwd))
				return logger.Error(TC("Failed to get current directory"));

			StringBuffer<> fullPath;
			fullPath.Append(cwd).EnsureEndsWithSlash().Append(ninjaFile);
			ninjaFile = fullPath.data;
		}

		// Compute ninjaDir from the (now-absolute) ninjaFile path.
		StringBuffer<512> ninjaDir(ninjaFile.c_str());
		{
			const tchar* lastSep = TStrrchr(ninjaDir.data, PathSeparator);
			if (lastSep)
				ninjaDir.Resize(u32(lastSep - ninjaDir.data + 1));
			else
				ninjaDir.EnsureEndsWithSlash();
		}

		NinjaDepsLog depsLog;
		depsLog.Init(StringView(ninjaDir.data, ninjaDir.count), GetTime());

		StringBuffer<128> traceName(TCV("UbaNinja_"));
		Guid guid;
		CreateGuid(guid);
		traceName.Append(GuidToString(guid));
		Trace trace(logWriter);
		trace.StartWriteAndThread(traceName.data, 128ull*1024*1024, true);
		auto tg = MakeGuard([&]()
			{
				trace.StopThread();
				ninjaDir.Append(TCV("UbaNinjaTrace.uba"));
				trace.Write(ninjaDir.data, true);
			});

		Scheduler* schedulerPtr = nullptr;
		u32 cacheBucketId = 0;
		bool forceRemote = UBA_FORCE_REMOTE;
		Event readyToSchedule(EventResetType_Manual);

		Atomic<bool> parserSuccess = true;

		// NinjaParseState holds primaryOutByProcessIdx which the process-finished
		// callback reads after the process completes. That can be AFTER the parser
		// thread returns, so we own the state in WrappedMain's scope (which outlives
		// the scheduler) rather than inside the parser lambda.
		NinjaParseState parseState;

		Thread parserThread([&]()
			{
				u32 taskId = 1234567;
				trace.TaskBegin(taskId, TCV("Parse"), {}, ColorWork);
				auto fail = [&](bool dummy = false) { parserSuccess = false; g_processStateChanged.Set(); return 0u; };

				// Build working dir = the directory that relative paths inside
				// the ninja file are resolved against. Ninja convention: this
				// is the cwd where the tool was invoked, or the `-C dir` if
				// specified. It is NOT necessarily the directory of the ninja
				// file (e.g. Android: `-f out/combined.ninja` from android root
				// — the file is in out/ but paths inside it are relative to
				// the android root).
				StringBuffer<> buildWorkingDir;
				if (!buildDir.empty())
				{
					buildWorkingDir.Append(buildDir);
				}
				else if (!GetCurrentDirectoryW(buildWorkingDir))
				{
					return fail(logger.Error(TC("Failed to get current directory")));
				}
				buildWorkingDir.EnsureEndsWithSlash();

				// Create shared MemoryBlock for all parsing (main + includes/subninjas)
				// Parser will be created and parsing will happen after scheduler starts
				MemoryBlock ninjaMemory(TC("NinjaParser"));
				ninjaMemory.Init(2ull * 1024 * 1024 * 1024); // 2GB reserve

				// Create parser (parsing will happen inside EnqueueCommands with callback)
				NinjaParser parser(ninjaMemory);

				u64 enqueueStartTime = GetTime();
				u64 parseStartTime = GetTime();
				if (!ParseNinjaFile(logger, parser, ninjaFile.c_str(), targets, parseState, buildWorkingDir.data))
					return fail();
				u64 parseTime = GetTime() - parseStartTime;

				trace.TaskEnd(taskId, true);

				readyToSchedule.IsSet();
				if (!schedulerPtr)
					return fail();

				u64 enqueueOnlyStart = GetTime();
				if (!EnqueueCommands(logger, parser, *schedulerPtr, parseState, buildWorkingDir, rebuild, cleanOnly, noShowIncludes, noUpToDate, verbose, dryRun, yamlFile.empty() ? nullptr : yamlFile.c_str(), cacheBucketId, forceRemote))
					return fail();
				u64 enqueueOnlyTime = GetTime() - enqueueOnlyStart;

				u64 enqueueTime = GetTime() - enqueueStartTime;
				logger.Info(TC("Parse and Enqueue completed in %s (parse=%s, enqueue=%s)"),
					TimeToText(enqueueTime).str, TimeToText(parseTime).str, TimeToText(enqueueOnlyTime).str);
				g_processStateChanged.Set();
				return 0u;

			}, TC("NinjPars"));


		Config config;
		StringBuffer<> configFile;
		if (GetCurrentDirectoryW(configFile))
		{
			configFile.EnsureEndsWithSlash().Append(TC("UbaNinja.toml"));
			if (FileExists(logger, configFile.data))
			{
				logger.Info(TC("Loading config from %s"), configFile.data);
				config.LoadFromFile(logger, configFile.data);
			}
		}

		TString coordinatorName;
		
		#if UBA_USE_HORDE
		coordinatorName = TC("Horde");
		#endif

		CoordinatorWrapper coordinator;

		NetworkBackendTcpCreateInfo nbInfo(logWriter);
		nbInfo.Apply(config);
		NetworkBackendTcp networkBackend(nbInfo);

		bool ctorSuccess = true;
		NetworkServerCreateInfo nsci(logWriter);
		nsci.workerCount = 512;
		nsci.Apply(config);
		NetworkServer networkServer(ctorSuccess, nsci);
		if (!ctorSuccess)
			return logger.Error(TC("Failed to create NetworkServer"));

		StringBuffer<> rootDir;
		if (!GetCurrentDirectoryW(rootDir))
			return logger.Error(TC("Failed to get current directory"));
		rootDir.EnsureEndsWithSlash().Append(TC(".uba"));

		u64 storageCapacity = 40ull * 1024 * 1024 * 1024;
		StorageServerCreateInfo storageInfo(networkServer, rootDir.data, logWriter);
		storageInfo.casCapacityBytes = storageCapacity;
		storageInfo.storeCompressed = true;
		storageInfo.Apply(config);
		StorageServer storageServer(storageInfo);

		SessionServerCreateInfo ssci(storageServer, networkServer, logWriter);
		ssci.rootDir = rootDir.data;
		ssci.useFillLock = false;
		ssci.traceEnabled = true;
		ssci.writeFilesBottleneck = 24;
		ssci.writeFilesFileMapMaxMb = 1;
		ssci.trace = &trace;

		//ssci.detailedTrace = true;
		//ssci.remoteTraceEnabled = true;

		#if UBA_DEBUG
		ssci.logToFile = true;
		ssci.remoteLogEnabled = true;
		#endif

		ssci.Apply(config);
		SessionServer sessionServer(ssci);

		#if UBA_DEBUG
		logger.Info(TC("Detour logging enabled. Written to %s%clog"), sessionServer.GetSessionDir(), PathSeparator);
		#endif

		#if PLATFORM_WINDOWS
		SetConsoleCtrlHandler([](DWORD signal) { CtrlBreakPressed(); return TRUE; }, TRUE);
		#else
		signal(SIGINT, [](int) { CtrlBreakPressed(); });
		signal(SIGTERM, [](int) { CtrlBreakPressed(); });
		#endif

		CacheClient* cacheClients[8];
		u32 cacheClientCount = 0;

#if UBA_USE_LOCAL_CACHE_SERVER
		NetworkBackendMemory networkMemoryBackend1(logWriter);
		NetworkBackendMemory networkMemoryBackend2(logWriter);
		NetworkBackendMemory networkMemoryBackend3(logWriter);
		NetworkBackendMemory networkMemoryBackend4(logWriter);

		logger.Info(TC(""));
		logger.Info(TC("Starting cache server..."));

		// Setup cache server/client which is used for out-of-date checking
		NetworkServerCreateInfo cnsci(logWriter);
		NetworkServer cacheNetworkServer(ctorSuccess, cnsci);
		StringBuffer<> cacheRootDir;
		//if (!GetCurrentDirectoryW(cacheRootDir))
		//	return logger.Error(TC("Failed to get current directory"));
		cacheRootDir.EnsureEndsWithSlash().Append(TC(".ubacache"));

		StorageServerCreateInfo cacheStorageInfo(cacheNetworkServer, cacheRootDir.data, logWriter);
		cacheStorageInfo.casCapacityBytes = 40ull * 1024 * 1024 * 1024;
		cacheStorageInfo.allowHintAsFallback = false;
		cacheStorageInfo.writeReceivedCasFilesToDisk = true;
		cacheStorageInfo.allowDeleteVerified = true;
		cacheStorageInfo.allowDeferredDeletes = false;
		StorageServer cacheStorageServer(cacheStorageInfo);
		CacheServerCreateInfo csInfo(cacheStorageServer, cacheRootDir.data, logWriter);
		CacheServer cacheServer(csInfo);


		if (!dryRun && !cleanOnly)
		{
			cacheNetworkServer.StartListen(networkMemoryBackend1);
			cacheNetworkServer.StartListen(networkMemoryBackend2);
			cacheNetworkServer.StartListen(networkMemoryBackend3);
			cacheNetworkServer.StartListen(networkMemoryBackend4);
			cacheServer.Load(false);
		}
#endif

		auto createCoordinator = [&](Scheduler* scheduler)
			{
				if (coordinatorName.empty())
					return true;
				if (scheduler)
					coordinator.SetScheduler(scheduler);
				if (coordinator.IsCreated())
					return true;

				StringBuffer<512> coordinatorWorkDir(rootDir);
				coordinatorWorkDir.EnsureEndsWithSlash().Append(coordinatorName);
				StringBuffer<512> binariesDir;
				if (!GetDirectoryOfCurrentModule(logger, binariesDir))
					return false;

				CoordinatorCreateInfo cinfo;
				cinfo.workDir = coordinatorWorkDir.data;
				cinfo.binariesDir = binariesDir.data;
				cinfo.maxCoreCount = 0;
				cinfo.logging = false;
				if (!coordinator.Create(logger, coordinatorName.c_str(), cinfo, networkBackend, networkServer, trace, 0))
					return false;
				return true;
			};

		NetworkClientCreateInfo nccInfo(logWriter);

#if UBA_USE_LOCAL_CACHE_SERVER
		NetworkClient cacheNetworkClient(ctorSuccess, nccInfo);
		cacheNetworkClient.Connect(networkMemoryBackend1, TC("Memory"));
		cacheNetworkClient.Connect(networkMemoryBackend2, TC("Memory"));
		cacheNetworkClient.Connect(networkMemoryBackend3, TC("Memory"));
		cacheNetworkClient.Connect(networkMemoryBackend4, TC("Memory"));
		CacheClientCreateInfo ccInfo(logWriter, storageServer, cacheNetworkClient, sessionServer);
		ccInfo.useRoots = false;
		CacheClient cacheClient(ccInfo);
		cacheClients[cacheClientCount++] = &cacheClient;
		cacheBucketId = 1;
#else

		StringBuffer<256> cacheServerEndpoint;
		Guid sessionKey;
#if 0
		if (true)
		{
			bool writeAccess;
			createCoordinator(nullptr);
			if (coordinator.RequestCacheServer(cacheServerEndpoint, sessionKey, writeAccess))
			{
				//cacheNetworkClient.Connect(networkBackend, TC("Memory"));
				nccInfo.cryptoKey128 = (u8*)&sessionKey;

				//nccInfo.useThreadedSend = true;
				cacheBucketId = 1;
			}
		}
#endif
		nccInfo.workerCount = 10;
		NetworkClient cacheNetworkClient(ctorSuccess, nccInfo);

		CacheClientCreateInfo ccInfo(logWriter, storageServer, cacheNetworkClient, sessionServer);
		ccInfo.useRoots = false;
		//ccInfo.reportMissReason = true;
		CacheClient cacheClient(ccInfo);

		if (cacheServerEndpoint.count)
		{
			u64 port = DefaultPort;
			bool success = true;
			if (const tchar* colon = cacheServerEndpoint.First(':'))
			{
				success = Parse(port, colon+1, TStrlen(colon)+1);
				cacheServerEndpoint.Resize(colon - cacheServerEndpoint.data);
			}

			if (success)
				if (cacheNetworkClient.Connect(networkBackend, cacheServerEndpoint.data, (u16)port))
				{
					IndexContainer container(31);
					networkServer.ParallelFor(30, container, [&](const WorkContext&, auto& it)
						{
							cacheNetworkClient.Connect(networkBackend, cacheServerEndpoint.data, (u16)port);
						}, TCV("Connect"));

					cacheClients[cacheClientCount++] = &cacheClient;
				}
		}
#endif

		sessionServer.RegisterNetworkTrafficProvider(u64(&cacheNetworkClient), [nc = &cacheNetworkClient](u64& outSent, u64& outReceive, u32& outConnectionCount)
			{
				outSent = nc->GetTotalSentBytes();
				outReceive = nc->GetTotalRecvBytes();
				outConnectionCount = nc->GetConnectionCount();
			});


		u32 maxLocalProcessors = Min(98u, GetLogicalProcessorCount());

#if UBA_USE_LOCAL_CLIENT
		StringBuffer<> clientRootDir;
		if (!GetCurrentDirectoryW(clientRootDir))
			return logger.Error(TC("Failed to get current directory"));
		clientRootDir.EnsureEndsWithSlash().Append(TC(".ubaclient"));
		ClientInitInfo ci { logWriter, networkBackend, clientRootDir.data, TC("localhost"), DefaultPort };
		ci.maxProcessorCount = maxLocalProcessors;
		forceRemote = true;
#endif

		storageServer.LoadCasTable();

		SchedulerCreateInfo sci(sessionServer);
		sci.maxLocalProcessors = maxLocalProcessors;
		sci.maxRacingPercent = 100;
		sci.cacheClientCount = cacheClientCount;
		sci.cacheClients = cacheClients;
		sci.writeToCache = true;
		sci.maxParallelCacheQueries = 800;//Min(GetLogicalProcessorCount(), 96u);
		sci.enableSpeculativeCacheTest = true;
		sci.forceRemote = forceRemote;
		sci.leakMemoryAtShutdown = true;
		//sci.traceDetails = true;
		//sci.writeTrackedInputs = true;
		sci.Apply(config);

		Scheduler scheduler(sci);

		auto destroyCoordinator = MakeGuard([&]() { coordinator.Destroy(); });

		Atomic<u32> failedProcessCount = 0;

		scheduler.SetProcessFinishedCallback([&](const ProcessHandle& handle)
			{
				u32 exitCode = handle.GetExitCode();
				if (exitCode != 0 && exitCode != ProcessCancelExitCode)
				{
					++failedProcessCount;
					logger.BeginScope();
					const ProcessStartInfo& startInfo = handle.GetStartInfo();
					logger.Error(TC("Process failed (Id: %u, exit code %u): %s"), handle.GetId(), exitCode, startInfo.GetDescription());

					// Output all log lines (stdout is Info, stderr is Error on POSIX)
					const auto& logLines = handle.GetLogLines();
					for (const auto& logLine : logLines)
						logger.Logf(logLine.type, TC("  %s"), logLine.text.c_str());
					logger.EndScope();
				}
				else if (exitCode == 0)
				{
					// Forward any warnings/errors printed to stderr even on success
					const auto& logLines = handle.GetLogLines();
					for (const auto& logLine : logLines)
						if (logLine.type <= LogEntryType_Warning)
							logger.Logf(logLine.type, TC("%s"), logLine.text.c_str());

					// Parse the callback payload: breadcrumbs is
					//   "<flag:1><16-hex-cmd-hash><path>"
					// where flag is 'R' for restat/phony_output edges, 'N'
					// otherwise. Hash is hex-encoded so the breadcrumbs stays a
					// plain C string and round-trips through any text tooling.
					const tchar* bc = handle.GetStartInfo().breadcrumbs;
					bool restat = false;
					const tchar* primaryOut = nullptr;
					u64 cmdHash = 0;
					if (bc && *bc)
					{
						restat = (bc[0] == TC('R'));
						// Require the full 16 hex chars; anything shorter is
						// malformed — treat as "no hash" and let RecordResult
						// fall back to hashing si.arguments.
						bool haveHash = true;
						for (u32 k = 0; k < 16 && haveHash; ++k)
						{
							tchar c = bc[1 + k];
							if      (c >= TC('0') && c <= TC('9')) cmdHash = (cmdHash << 4) | u64(c - TC('0'));
							else if (c >= TC('a') && c <= TC('f')) cmdHash = (cmdHash << 4) | u64(10 + (c - TC('a')));
							else if (c >= TC('A') && c <= TC('F')) cmdHash = (cmdHash << 4) | u64(10 + (c - TC('A')));
							else { haveHash = false; cmdHash = 0; }
						}
						primaryOut = haveHash ? (bc + 17) : (bc + 1);
					}
					depsLog.RecordResult(logger, handle,
						(primaryOut && *primaryOut) ? primaryOut : nullptr, restat, cmdHash);
				}
				g_processStateChanged.Set();
			});

		if (!dryRun && !cleanOnly)
		{
			createCoordinator(&scheduler);

			coordinator.RequestHelpers(2600);
			scheduler.Start();
		}

		networkServer.StartListen(networkBackend);


#if UBA_USE_LOCAL_CLIENT
		Client c;
		c.Init(ci);
#endif

		schedulerPtr = &scheduler;
		readyToSchedule.Set();

		u32 queued, activeLocal, activeRemote, finished;
		scheduler.GetStats(queued, activeLocal, activeRemote, finished);

		if (cleanOnly || dryRun)
		{
			// Wait for the parser thread to finish its enqueue work. Cancelling
			// the scheduler mid-enqueue trips an assert on EnqueueSuspendedProcess.
			parserThread.Wait();
			scheduler.GetStats(queued, activeLocal, activeRemote, finished);
			scheduler.Cancel();
			if (dryRun)
			{
				logger.Info(TC(""));
				logger.Info(TC("Dry-run mode: Would build %u processes"), queued);
			}
			return true;
		}

		u32 lastFinished = 0;
		while (true)
		{
			scheduler.GetStats(queued, activeLocal, activeRemote, finished);
			if (finished != lastFinished)
			{
				lastFinished = finished;
				StringBuffer<32> logEntry;
				logEntry.Appendf(TC("\r[%u/%u]"), finished, scheduler.GetTotalProcesses());
				if (u32 fc = failedProcessCount.load())
					logEntry.Appendf(TC(" Failed: %u"), fc);
				LogToConsoleNoLineFeed(LogEntryType_Info, logEntry.data, logEntry.count);
			}

			if (!parserThread.Wait(10))
				continue;

			if (finished == scheduler.GetTotalProcesses() || g_shouldExit)
				break;

			g_processStateChanged.IsSet();
		}

		if (g_shouldExit)
		{
			logger.Info(TC("Cancelling..."));
			networkServer.DisallowNewClients();
			scheduler.Cancel();
		}
		else
			while (!scheduler.IsEmpty()) // Make sure write to cache is flushed
				Sleep(10);

		cacheNetworkClient.Disconnect();

#if UBA_USE_LOCAL_CACHE_SERVER
		cacheNetworkServer.DisconnectClients();
#endif

		networkBackend.StopListen();
		networkServer.DisconnectClients(); // NOTE TO SELF, THIS ALSO DISCONNECT NETWORK VISUALIZERS.. SO LAST ACTION MIGHT LOOK CANCELLED
		destroyCoordinator.Execute();
		scheduler.Stop(); // Leak memory, we are always leaving the process here
		scheduler.GetStats(queued, activeLocal, activeRemote, finished);

		if (!g_shouldExit && !g_ctrlBreakPressed && !dryRun && !cleanOnly)
			depsLog.Save(logger, StringView(ninjaDir.data, ninjaDir.count));

		u32 failed = failedProcessCount;
		logger.Info(TC(""));
		if (g_ctrlBreakPressed)
			logger.Info(TC("Build cancelled! (Total processed: %u)"), finished);
		else if (failed == 0)
			logger.Info(TC("Build successful! (Total processed: %u in %s)"), finished, TimeToText(GetTime() - startTime, true).str);
		else
			logger.Info(TC("Build failed! (%u failed)"), failed);

#if UBA_USE_LOCAL_CACHE_SERVER
		cacheServer.RunMaintenance(false, true, []() { return false; });
#endif

		return failed == 0;
	}
}

#if PLATFORM_WINDOWS
int wmain(int argc, wchar_t* argv[]) { return uba::WrappedMain(argc, argv) ? 0 : 1; }
#else
int main(int argc, char* argv[]) { return uba::WrappedMain(argc, argv) ? 0 : 1; }
#endif
