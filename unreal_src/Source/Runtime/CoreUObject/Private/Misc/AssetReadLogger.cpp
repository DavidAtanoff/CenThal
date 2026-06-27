// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AssetReadLogger.h"

// Define UE_ENABLE_ASSET_READ_LOGGER if you want to compile this in shipping builds.

#if (!UE_BUILD_SHIPPING || defined(UE_ENABLE_ASSET_READ_LOGGER))
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProperties.h"
#include "Misc/CoreDelegatesInternal.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"
#include "HAL/RunnableThread.h"
#include "IO/IoChunkId.h"
#include "IO/IoDispatcher.h"
#include "IO/IoStore.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/LinkerLoad.h"
#include "IO/IoDispatcherInternal.h"

#if CSV_PROFILER
#include "ProfilingDebugging/CsvProfiler.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogAssetReadLogger, Log, All);

static FAutoConsoleCommand GAssetReadLoggerSweepAndFlushCmd(
	TEXT("AssetReadLogger.SweepAndFlush"),
	TEXT("Sweep for missed packages and flush CSV to disk (does not stop the logger)"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FAssetReadLogger& Logger = FAssetReadLogger::Get();
		if (Logger.IsEnabled())
		{
			Logger.SweepAndFlushToFile();
		}
		else
		{
			UE_LOG(LogAssetReadLogger, Warning, TEXT("AssetReadLogger is not enabled. Use -LogAssetReads to enable."));
		}
	})
);

// Pushes asset-read aggregate stats into the csvprofiler metadata. Convenience command
// for debugging. This is automatically called on OnCSVProfileEnd delegate.
static FAutoConsoleCommand GAssetReadLoggerAppendCsvMetadataCmd(
	TEXT("AssetReadLogger.AppendCsvMetadata"),
	TEXT("Push asset-read aggregate stats (assetreads_*) to the CsvProfiler metadata trailer"),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FAssetReadLogger& Logger = FAssetReadLogger::Get();
		if (Logger.IsEnabled())
		{
			Logger.AppendCSVMetadataStats();
		}
		else
		{
			UE_LOG(LogAssetReadLogger, Warning, TEXT("AssetReadLogger is not enabled. Use -LogAssetReads to enable."));
		}
	})
);

const TCHAR* LexToString(EAssetContainerType ContainerType)
{
	switch (ContainerType)
	{
	case EAssetContainerType::Unknown:
		return TEXT("Unknown");
	case EAssetContainerType::LooseFile:
		return TEXT("Loose");
	case EAssetContainerType::Pak:
		return TEXT("Pak");
	case EAssetContainerType::IoStoreOnDemand:
		return TEXT("IAD");
	case EAssetContainerType::IoStoreStreaming:
		return TEXT("IAS");
	case EAssetContainerType::IoStoreLocal:
		return TEXT("IoStore");
	default:
		return TEXT("Unknown");
	}
}

FAssetReadInfo::FAssetReadInfo(const FString& InAssetPath, EAssetContainerType InContainerType, int32 InChunkID,
	const FString& InContainerName, int64 InSize, double InTime,
	const FString& InThreadName)
	: AssetPath(InAssetPath)
	, MountPoint(ExtractMountPoint(InAssetPath))
	, ContainerType(InContainerType)
	, ChunkID(InChunkID)
	, ContainerName(InContainerName)
	, SizeBytes(InSize)
	, LoadTimestamp(InTime)
	, ThreadName(InThreadName)
{
	PathHash = CityHash64(reinterpret_cast<const char*>(*AssetPath), AssetPath.Len() * sizeof(TCHAR));
}

FString FAssetReadInfo::ExtractMountPoint(const FString& AssetPath)
{
	// Extract the first path component as the mount point
	// e.g., "/Game/Foo/Bar" -> "/Game/"
	if (AssetPath.IsEmpty() || AssetPath[0] != TEXT('/'))
	{
		return TEXT("/Unknown/");
	}

	int32 SecondSlash = AssetPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
	if (SecondSlash != INDEX_NONE && SecondSlash > 1)
	{
		return AssetPath.Left(SecondSlash + 1);
	}

	return TEXT("/Unknown/");
}

const TCHAR* FAssetReadInfo::GetContainerTypeString() const
{
	return LexToString(ContainerType);
}

FAssetReadLogger& FAssetReadLogger::Get()
{
	static FAssetReadLogger Instance;
	return Instance;
}

FAssetReadLogger::FAssetReadLogger()
{
}

FAssetReadLogger::~FAssetReadLogger()
{
	if (bIsEnabled.load())
	{
		Shutdown();
	}
}

// Parses command line for -LogAssetReads[=path], creates output CSV file, registers
// delegate hooks for package load events, and schedules backfill of already-loaded packages.
// Returns true if logging was enabled, false if the command line flag was not present.
bool FAssetReadLogger::Initialize()
{
	FString OutputPath;
	bool bHasExplicitPath = FParse::Value(FCommandLine::Get(), TEXT("-LogAssetReads="), OutputPath);
	bool bHasFlag = bHasExplicitPath || FParse::Param(FCommandLine::Get(), TEXT("LogAssetReads"));

	if (!bHasFlag)
	{
		return false;
	}

	if (bHasExplicitPath)
	{
		OutputPath = OutputPath.TrimQuotes();
		FPaths::NormalizeFilename(OutputPath);
		if (FPaths::IsRelative(OutputPath))
		{
			OutputPath = FPaths::ProjectSavedDir() / OutputPath;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		OutputPath = FPaths::ProjectSavedDir() / TEXT("Profiling") / FString::Printf(TEXT("AssetReads_%s.csv"), *Timestamp);
	}

	FString OutputDir = FPaths::GetPath(OutputPath);
	if (!OutputDir.IsEmpty())
	{
		if (!IFileManager::Get().MakeDirectory(*OutputDir, true))
		{
			UE_LOG(LogAssetReadLogger, Error, TEXT("Failed to create output directory: %s"), *OutputDir);
			return false;
		}
	}

	OutputFilePath = OutputPath;
	StartTime = FPlatformTime::Seconds();

	OutputFileArchive = TUniquePtr<FArchive>(IFileManager::Get().CreateFileWriter(*OutputFilePath));
	if (!OutputFileArchive)
	{
		UE_LOG(LogAssetReadLogger, Error, TEXT("Failed to create output file: %s"), *OutputFilePath);
		return false;
	}

	// Metadata comment block (# prefix) is ignored by CSV tools but parseable by analysis scripts.
	WriteMetadataBlock();

	FString Header = TEXT("AssetPath,MountPoint,ContainerType,ChunkID,ContainerName,SizeBytes,LoadTimestamp,Thread\n");
	FTCHARToUTF8 HeaderUtf8(*Header);
	OutputFileArchive->Serialize(const_cast<ANSICHAR*>(HeaderUtf8.Get()), HeaderUtf8.Length());
	OutputFileArchive->Flush();

	bIsEnabled.store(true);
	RegisterAssetLoadingHooks();

	UE_LOG(LogAssetReadLogger, Log, TEXT("Asset read logging enabled. Output: %s"), *OutputFilePath);
	UE_LOG(LogAssetReadLogger, Log, TEXT("Tracking: Pak, IoStore (IAD/IAS), and Loose files"));

	// Backfill must wait until GPackageResourceManager is initialized (late in FEngineLoop::Init).
	// DetectAssetContainer() -> FPackageName::DoesPackageExist() would crash before that.
	EngineInitCompleteDelegateHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([this]()
	{
		BackfillLoadedPackages();
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(EngineInitCompleteDelegateHandle);
		EngineInitCompleteDelegateHandle.Reset();
	});

#if CSV_PROFILER
	if(FCsvProfiler* const CsvProfiler = FCsvProfiler::Get())
	{
		CsvProfiler->OnCSVProfileEnd().AddLambda([]()
		{
			FAssetReadLogger& Logger = FAssetReadLogger::Get();
			if (Logger.IsEnabled())
			{
				Logger.AppendCSVMetadataStats();
			}
		});
	}
#endif

	return true;
}

bool FAssetReadLogger::IsEnabled() const
{
	return bIsEnabled.load();
}

// Thread-safe logging of a single asset read. Deduplicates by path hash and writes
// new entries directly to the CSV file. Updates per-container-type statistics.
void FAssetReadLogger::LogAssetRead(const FString& AssetPath, EAssetContainerType ContainerType,
	int32 ChunkID, const FString& ContainerName,
	int64 SizeBytes, const FString& ThreadName)
{
	if (!bIsEnabled.load())
	{
		return;
	}

	TotalReadsAttempted.fetch_add(1);
	double CurrentTime = FPlatformTime::Seconds() - StartTime;
	FString ActualThreadName = ThreadName.IsEmpty() ? GetCurrentThreadName() : ThreadName;
	FAssetReadInfo AssetInfo(AssetPath, ContainerType, ChunkID, ContainerName, SizeBytes, CurrentTime, ActualThreadName);

	bool bIsNewAsset = false;
	{
		FRWScopeLock WriteLock(AssetReadLock, SLT_Write);

		bool bWasAlreadyInSet = false;
		UniqueAssetReads.Add(AssetInfo, &bWasAlreadyInSet);

		if (!bWasAlreadyInSet)
		{
			bIsNewAsset = true;
			UniqueReadsLogged.fetch_add(1);

			// Update container type statistics
			FAssetContainerStats& Stats = StatsByContainerType.FindOrAdd(ContainerType);
			Stats.Count++;
			Stats.TotalSize += SizeBytes;
		}
	}

	if (bIsNewAsset)
	{
		FScopeLock FileLock(&OutputFileLock);
		if (OutputFileArchive)
		{
			FString Line = FormatCSVLine(AssetInfo) + TEXT("\n");
			FTCHARToUTF8 LineUtf8(*Line);
			OutputFileArchive->Serialize(const_cast<ANSICHAR*>(LineUtf8.Get()), LineUtf8.Length());
			OutputFileArchive->Flush();
		}
	}
}

void FAssetReadLogger::Shutdown()
{
	if (!bIsEnabled.load())
	{
		return;
	}

	UE_LOG(LogAssetReadLogger, Log, TEXT("Shutting down asset read logger..."));

	// Final sweep to catch any packages missed by delegate hooks
	if (IsInGameThread())
	{
		SweepForMissedPackages();
	}

	UE_LOG(LogAssetReadLogger, Log, TEXT("Total read attempts: %lld"), TotalReadsAttempted.load());
	UE_LOG(LogAssetReadLogger, Log, TEXT("Unique assets logged: %lld"), UniqueReadsLogged.load());

	if (IsInGameThread())
	{
		int32 TrackableCount = CountLoadedPackages();
		int64 CapturedCount = UniqueReadsLogged.load();
		float CoveragePercent = TrackableCount > 0 ? (float)CapturedCount / (float)TrackableCount * 100.0f : 0.0f;
		UE_LOG(LogAssetReadLogger, Log, TEXT("Final coverage: %lld captured / %d trackable packages (%.1f%%)"),
			CapturedCount, TrackableCount, CoveragePercent);
	}

	PrintContainerStatistics();

	if (EngineInitCompleteDelegateHandle.IsValid())
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(EngineInitCompleteDelegateHandle);
		EngineInitCompleteDelegateHandle.Reset();
	}

	// Unregister before closing file to prevent new writes during teardown.
	UnregisterAssetLoadingHooks();

	{
		FScopeLock FileLock(&OutputFileLock);
		if (OutputFileArchive)
		{
			OutputFileArchive->Flush();
			OutputFileArchive->Close();
			OutputFileArchive.Reset();
			UE_LOG(LogAssetReadLogger, Log, TEXT("Asset read log saved to: %s"), *OutputFilePath);
		}
	}

	bIsEnabled.store(false);
}

// Sweeps for missed packages and flushes buffered data to disk without stopping the logger.
void FAssetReadLogger::SweepAndFlushToFile()
{
	if (!bIsEnabled.load())
	{
		return;
	}

	UE_LOG(LogAssetReadLogger, Log, TEXT("Flushing asset read logs to file..."));

	SweepForMissedPackages();

	UE_LOG(LogAssetReadLogger, Log, TEXT("Total read attempts: %lld"), TotalReadsAttempted.load());
	UE_LOG(LogAssetReadLogger, Log, TEXT("Unique assets logged: %lld"), UniqueReadsLogged.load());

	int32 TrackableCount = CountLoadedPackages();
	int64 CapturedCount = UniqueReadsLogged.load();
	float CoveragePercent = TrackableCount > 0 ? (float)CapturedCount / (float)TrackableCount * 100.0f : 0.0f;
	UE_LOG(LogAssetReadLogger, Log, TEXT("Coverage: %lld captured / %d trackable packages in memory (%.1f%%)"),
		CapturedCount, TrackableCount, CoveragePercent);

	PrintContainerStatistics();

	// Flush the output file
	{
		FScopeLock FileLock(&OutputFileLock);
		if (OutputFileArchive)
		{
			OutputFileArchive->Flush();
		}
	}

	UE_LOG(LogAssetReadLogger, Log, TEXT("Flush complete. Logger still active."));
}

int64 FAssetReadLogger::GetUniqueAssetCount() const
{
	return UniqueReadsLogged.load();
}

int64 FAssetReadLogger::GetTotalReadAttempts() const
{
	return TotalReadsAttempted.load();
}

// Aggregates the in-memory tracked reads and set per-key metadata values via csv profiler
// Surfaced stats under `assetreads_*`.
void FAssetReadLogger::AppendCSVMetadataStats() const
{
#if CSV_PROFILER
	if (!bIsEnabled.load())
	{
		return;
	}

	int64 TotalAssets = 0;
	int64 TotalBytes = 0;
	int64 LargestBytes = 0;
	FString LargestPath;
	int64 PreInitBytes = 0;
	TSet<FString> UniqueContainerNames;
	TSet<int32> UniqueChunkIDs;
	TMap<EAssetContainerType, int64> AssetsByContainer;
	TMap<EAssetContainerType, int64> BytesByContainer;
	TMap<int32, int64> BytesByChunk;

	{
		FRWScopeLock ReadLock(AssetReadLock, SLT_ReadOnly);

		for (const FAssetReadInfo& Info : UniqueAssetReads)
		{
			TotalAssets++;
			TotalBytes += Info.SizeBytes;

			AssetsByContainer.FindOrAdd(Info.ContainerType, 0)++;
			BytesByContainer.FindOrAdd(Info.ContainerType, 0) += Info.SizeBytes;

			if (!Info.ContainerName.IsEmpty())
			{
				UniqueContainerNames.Add(Info.ContainerName);
			}
			if (Info.ChunkID >= 0)
			{
				UniqueChunkIDs.Add(Info.ChunkID);
				BytesByChunk.FindOrAdd(Info.ChunkID, 0) += Info.SizeBytes;
			}

			if (Info.SizeBytes > LargestBytes)
			{
				LargestBytes = Info.SizeBytes;
				LargestPath = Info.AssetPath;
			}

			if (Info.ThreadName == TEXT("PreInit"))
			{
				PreInitBytes += Info.SizeBytes;
			}
		}
	}

	if (TotalAssets == 0)
	{
		UE_LOG(LogAssetReadLogger, Log, TEXT("AppendCSVMetadataStats: no tracked reads, skipping CsvProfiler metadata push."));
		return;
	}

	FCsvProfiler* const CsvProfiler = FCsvProfiler::Get();
	if (!CsvProfiler)
	{
		UE_LOG(LogAssetReadLogger, Warning, TEXT("AppendCSVMetadataStats: CSV Profiler is null. Skipping..."));
		return;
	}

	constexpr double BytesPerMB = 1048576.0;
	auto BytesToMB = [](int64 Bytes) -> double { return static_cast<double>(Bytes) / BytesPerMB; };

	const TCHAR* KeyPrefix = TEXT("assetreads_");
	auto MakeKey = [KeyPrefix](const FString& Suffix) -> FString
	{
		return FString::Printf(TEXT("%s%s"), KeyPrefix, *Suffix);
	};

	auto SetInt = [&MakeKey, CsvProfiler](const FString& Suffix, int64 Value)
	{
		const FString Key = MakeKey(Suffix);
		const FString Value64 = FString::Printf(TEXT("%lld"), Value);
		CsvProfiler->SetMetadata(*Key, *Value64);
	};

	auto SetMB = [&MakeKey, CsvProfiler](const FString& Suffix, double Mb)
	{
		const FString Key = MakeKey(Suffix);
		const FString Value = FString::Printf(TEXT("%.1f"), Mb);
		CsvProfiler->SetMetadata(*Key, *Value);
	};

	auto SetStr = [&MakeKey, CsvProfiler](const FString& Suffix, const FString& Value)
	{
		const FString Key = MakeKey(Suffix);
		CsvProfiler->SetMetadata(*Key, *Value);
	};

	SetInt(TEXT("totalAssets"), TotalAssets);
	SetMB(TEXT("totalMB"), BytesToMB(TotalBytes));
	SetInt(TEXT("uniqueContainers"), UniqueContainerNames.Num());
	SetInt(TEXT("uniqueChunks"), UniqueChunkIDs.Num());

	for (const TPair<EAssetContainerType, int64>& Pair : AssetsByContainer)
	{
		const FString TypeName(LexToString(Pair.Key));
		SetInt(FString::Printf(TEXT("%s_assets"), *TypeName), Pair.Value);
	}

	for (const TPair<EAssetContainerType, int64>& Pair : BytesByContainer)
	{
		const FString TypeName(LexToString(Pair.Key));
		SetMB(FString::Printf(TEXT("%s_MB"), *TypeName), BytesToMB(Pair.Value));
	}

	SetMB(TEXT("largestAssetMB"), BytesToMB(LargestBytes));
	SetStr(TEXT("largestAssetPath"), LargestPath);

	if (TotalBytes > 0)
	{
		const double PreInitPct = 100.0 * static_cast<double>(PreInitBytes) / static_cast<double>(TotalBytes);
		SetMB(TEXT("preinitPct"), PreInitPct); 
	}

	if (BytesByChunk.Num() > 0)
	{
		// Top-N chunks by byte volume, mirroring AssetReadLogProcessor.DefaultTopN = 5.
		constexpr int32 TopN = 5;
		TArray<TPair<int32, int64>> SortedChunks;
		SortedChunks.Reserve(BytesByChunk.Num());
		for (const TPair<int32, int64>& Pair : BytesByChunk)
		{
			SortedChunks.Add(Pair);
		}
		SortedChunks.Sort([](const TPair<int32, int64>& A, const TPair<int32, int64>& B)
		{
			return A.Value > B.Value;
		});

		FString TopChunks;
		const int32 Limit = FMath::Min(TopN, SortedChunks.Num());
		for (int32 i = 0; i < Limit; ++i)
		{
			if (i > 0)
			{
				TopChunks += TEXT(",");
			}
			TopChunks += FString::Printf(TEXT("%d:%.1f"), SortedChunks[i].Key, BytesToMB(SortedChunks[i].Value));
		}
		SetStr(TEXT("topChunks"), TopChunks);
	}

	UE_LOG(LogAssetReadLogger, Log,
		TEXT("AppendCSVMetadataStats: pushed assets=%lld totalMB=%.1f containers=%d chunks=%d to CsvProfiler"),
		TotalAssets, BytesToMB(TotalBytes), UniqueContainerNames.Num(), UniqueChunkIDs.Num());
#else 
	UE_LOG(LogAssetReadLogger, Warning, TEXT("AppendCSVMetadataStats: CsvProfiler disabled."));
#endif // CSV_PROFILER
}

// Registers all delegate hooks for tracking package loads:
// - OnPackageLoadCompleted: Primary hook, fires after any package finishes loading (all builds)
// - OnSyncLoadPackage: GameThread-only sync load start (for thread attribution)
// - OnAsyncLoadPackage: Async load requests (tracks requesting thread for attribution)
// - OnPakFileMounted2: Pak mount events (builds package-to-container cache for platforms without metadata)
void FAssetReadLogger::RegisterAssetLoadingHooks()
{
	PackageLoadedDelegateHandle = FCoreUObjectDelegates::OnPackageLoadCompleted.AddRaw(
		this, &FAssetReadLogger::OnPackageLoaded);

	SyncLoadPackageDelegateHandle = FCoreDelegates::OnSyncLoadPackage.AddRaw(
		this, &FAssetReadLogger::OnSyncLoadPackageStarted);

	// Track requesting thread for proper attribution when OnPackageLoadCompleted fires on GameThread.
	AsyncLoadPackageDelegateHandle = FCoreDelegates::GetOnAsyncLoadPackage().AddRaw(
		this, &FAssetReadLogger::OnAsyncLoadPackageRequested);

	// Fallback for platforms where IoDispatcher container metadata isn't available.
	PakFileMountedDelegateHandle = FCoreDelegates::GetOnPakFileMounted2().AddRaw(
		this, &FAssetReadLogger::OnPakFileMounted);

	if (FCoreInternalDelegates::GetCurrentlyMountedPaksDelegate().IsBound())
	{
		TArray<FMountedPakInfo> MountedPaks = FCoreInternalDelegates::GetCurrentlyMountedPaksDelegate().Execute();
		for (const FMountedPakInfo& PakInfo : MountedPaks)
		{
			if (PakInfo.PakFile != nullptr)
			{
				OnPakFileMounted(*PakInfo.PakFile);
			}
		}
		UE_LOG(LogAssetReadLogger, Log, TEXT("Cached package mappings from %d already-mounted paks"), MountedPaks.Num());
	}
}

void FAssetReadLogger::UnregisterAssetLoadingHooks()
{
	if (PackageLoadedDelegateHandle.IsValid())
	{
		FCoreUObjectDelegates::OnPackageLoadCompleted.Remove(PackageLoadedDelegateHandle);
		PackageLoadedDelegateHandle.Reset();
	}

	if (SyncLoadPackageDelegateHandle.IsValid())
	{
		FCoreDelegates::OnSyncLoadPackage.Remove(SyncLoadPackageDelegateHandle);
		SyncLoadPackageDelegateHandle.Reset();
	}

	if (AsyncLoadPackageDelegateHandle.IsValid())
	{
		FCoreDelegates::GetOnAsyncLoadPackage().Remove(AsyncLoadPackageDelegateHandle);
		AsyncLoadPackageDelegateHandle.Reset();
	}

	if (PakFileMountedDelegateHandle.IsValid())
	{
		FCoreDelegates::GetOnPakFileMounted2().Remove(PakFileMountedDelegateHandle);
		PakFileMountedDelegateHandle.Reset();
	}
}

// Primary hook for package load completion. Always fires on GameThread, so we look up
// the originating thread from AsyncLoadRequestThreads, or check PendingSyncLoadPackages.
void FAssetReadLogger::OnPackageLoaded(UPackage* Package)
{
	if (Package == nullptr || !bIsEnabled.load())
	{
		return;
	}

	FString AssetPath = Package->GetPathName();
	if (!IsTrackableAssetPath(AssetPath))
	{
		return;
	}

	FAssetContainerInfo ContainerInfo = DetectAssetContainer(Package);

	int64 SizeBytes = GetPackageSizeBytes(Package);
	if (SizeBytes <= 0)
	{
		return;
	}

	// Resolve thread attribution from async request map (this delegate always fires on GameThread).
	FString CurrentThreadName;
	{
		FScopeLock Lock(&AsyncLoadRequestLock);
		FString* FoundThread = AsyncLoadRequestThreads.Find(Package->GetName());
		if (FoundThread)
		{
			CurrentThreadName = MoveTemp(*FoundThread);
			AsyncLoadRequestThreads.Remove(Package->GetName());
		}
	}

	if (CurrentThreadName.IsEmpty())
	{
		bool bWasSyncLoad = false;
		{
			FScopeLock Lock(&PendingSyncLoadLock);
			if (PendingSyncLoadPackages.Contains(AssetPath))
			{
				PendingSyncLoadPackages.Remove(AssetPath);
				bWasSyncLoad = true;
			}
		}
		CurrentThreadName = bWasSyncLoad ? TEXT("GameThread_Sync") : GetCurrentThreadName();
	}

	LogAssetRead(AssetPath, ContainerInfo.Type, ContainerInfo.ChunkID,
		ContainerInfo.Name, SizeBytes, CurrentThreadName);
}

// GameThread-only sync load start hook. Adds package to PendingSyncLoadPackages so
// OnPackageLoaded can attribute it as "GameThread_Sync". Background thread sync loads
// use OnSyncLoadPackageComplete instead.
void FAssetReadLogger::OnSyncLoadPackageStarted(const FString& PackageName)
{
	if (!bIsEnabled.load())
	{
		return;
	}

	if (!IsTrackableAssetPath(PackageName))
	{
		return;
	}

	{
		FScopeLock Lock(&PendingSyncLoadLock);
		PendingSyncLoadPackages.Add(PackageName);
	}

	UE_LOG(LogAssetReadLogger, Verbose, TEXT("Sync load started: %s"), *PackageName);
}

void FAssetReadLogger::ProcessPackageByName(const FString& PackageName)
{
	if (!bIsEnabled.load())
	{
		return;
	}

	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (Package)
	{
		OnPackageLoaded(Package);
	}
	else
	{
		UE_LOG(LogAssetReadLogger, Warning, TEXT("ProcessPackageByName: Could not find package %s"), *PackageName);
	}
}

// Determines how a package was loaded: Pak, IoStore (IAD/IAS/Local), or LooseFile.
// Resolution order: GetLoadedPath() -> DoesPackageExist() -> TryConvertLongPackageNameToFilename()
// -> GetPakContainerInfo() -> IFileManager loose check -> GetIoStoreContainerInfo().
FAssetContainerInfo FAssetReadLogger::DetectAssetContainer(UPackage* Package) const
{
	FString PackageFileName;
	const FPackagePath& LoadedPath = Package->GetLoadedPath();
	if (!LoadedPath.IsEmpty())
	{
		PackageFileName = LoadedPath.GetLocalFullPath();
	}

	if (PackageFileName.IsEmpty())
	{
		FPackageName::DoesPackageExist(Package->GetName(), &PackageFileName);
	}

	// Fallback for pak-loaded packages where GetLoadedPath() returns empty.
	if (PackageFileName.IsEmpty())
	{
		FString PackageName = Package->GetName();
		if (!PackageName.IsEmpty())
		{
			FString BaseFilename;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, BaseFilename))
			{
				for (const TCHAR* Extension : { TEXT(".uasset"), TEXT(".umap") })
				{
					FString TestPath = BaseFilename + Extension;
					IPlatformFile* PakPlatformFile = FPlatformFileManager::Get().FindPlatformFile(TEXT("PakFile"));
					if (PakPlatformFile && PakPlatformFile->FileExists(*TestPath))
					{
						PackageFileName = TestPath;
						break;
					}
				}
			}
			else
			{
				PackageFileName = PackageName;
			}
		}
	}

	if (!PackageFileName.IsEmpty())
	{
		FAssetContainerInfo PakInfo = GetPakContainerInfo(PackageFileName);
		if (PakInfo.bIsValid)
		{
			return PakInfo;
		}

		if (IFileManager::Get().FileExists(*PackageFileName))
		{
			return FAssetContainerInfo{
				EAssetContainerType::LooseFile,
				-1,
				TEXT("Loose"),
				true
			};
		}
	}

	FPackageId PackageId = Package->GetPackageId();
	if (PackageId.IsValid() && FIoDispatcher::IsInitialized())
	{
		FAssetContainerInfo IoStoreInfo = GetIoStoreContainerInfo(PackageId, Package->GetName());
		if (IoStoreInfo.bIsValid)
		{
			return IoStoreInfo;
		}
	}

	// Pak-loaded packages in UE5 can also have valid PackageIds, so don't assume IoStoreLocal.
	return FAssetContainerInfo{EAssetContainerType::Unknown, -1, TEXT("Unknown"), false};
}

// Queries IoDispatcher for container metadata. Falls back to cached pak mount mapping on
// platforms where FIoDispatcherInternal::GetFilename() doesn't return container info.
FAssetContainerInfo FAssetReadLogger::GetIoStoreContainerInfo(FPackageId PackageId, const FString& PackageName) const
{
	if (!PackageId.IsValid() || !FIoDispatcher::IsInitialized())
	{
		return FAssetContainerInfo{EAssetContainerType::Unknown, -1, TEXT(""), false};
	}

	FIoChunkId ChunkId = CreatePackageDataChunkId(PackageId);
	FIoDispatcher& IoDispatcher = FIoDispatcher::Get();

	if (!IoDispatcher.DoesChunkExist(ChunkId))
	{
		return FAssetContainerInfo{EAssetContainerType::Unknown, -1, TEXT(""), false};
	}

	{
		FUtf8StringView IoContainerName;
		TUtf8StringBuilder<256> FileName;
		FIoDispatcherInternal::GetFilename(ChunkId, FileName, IoContainerName);
		if (!IoContainerName.IsEmpty())
		{
			const FUtf8String Str(IoContainerName);
			const FString PakFileName = StringCast<TCHAR>(*Str).Get();
			const EAssetContainerType Type = DetermineIoStoreType(PakFileName);
			const int32 Chunk = ExtractChunkIDFromPakFilename(PakFileName);
			if(Type == EAssetContainerType::IoStoreLocal)
			{
				return FAssetContainerInfo{EAssetContainerType::Pak, Chunk, PakFileName, true};
			}
			else
			{
				return FAssetContainerInfo{Type, Chunk, PakFileName, true};
			}
		}
	}

	// Fallback to cached mapping for platforms where IoDispatcher metadata isn't available.
	{
		FAssetContainerInfo CachedInfo = LookupCachedContainerInfo(PackageName);
		if (CachedInfo.bIsValid)
		{
			return CachedInfo;
		}
	}

	// Final fallback: infer type from package name patterns.
	EAssetContainerType ContainerType = EAssetContainerType::IoStoreLocal;
	FString ContainerName = TEXT("IoStore");

	FString LowerPackageName = PackageName.ToLower();
	if (LowerPackageName.Contains(TEXT("ondemand")) || LowerPackageName.Contains(TEXT("/oad/")))
	{
		ContainerType = EAssetContainerType::IoStoreOnDemand;
		ContainerName = TEXT("OnDemand");
	}
	else if (LowerPackageName.Contains(TEXT("streaming")) || LowerPackageName.Contains(TEXT("/str/")))
	{
		ContainerType = EAssetContainerType::IoStoreStreaming;
		ContainerName = TEXT("Streaming");
	}

	return FAssetContainerInfo{
		ContainerType,
		0,
		ContainerName,
		true
	};
}

// Checks FPakPlatformFile to determine if a package came from a .pak file.
// Tries multiple path normalizations since the pak system uses FPaths::MakeStandardFilename internally.
// Returns invalid info if the file exists on physical disk (loose file takes precedence).
FAssetContainerInfo FAssetReadLogger::GetPakContainerInfo(const FString& PackageFileName) const
{
	IPlatformFile* PakPlatformFile = FPlatformFileManager::Get().FindPlatformFile(TEXT("PakFile"));
	if (PakPlatformFile == nullptr)
	{
		return FAssetContainerInfo{EAssetContainerType::Unknown, -1, TEXT(""), false};
	}

	TArray<FString> PathsToTry;
	PathsToTry.Add(PackageFileName);

	FString NormalizedPath = PackageFileName;
	FPaths::NormalizeFilename(NormalizedPath);
	if (NormalizedPath != PackageFileName)
	{
		PathsToTry.Add(NormalizedPath);
	}

	FString StandardPath = PackageFileName;
	FPaths::MakeStandardFilename(StandardPath);
	if (StandardPath != PackageFileName && StandardPath != NormalizedPath)
	{
		PathsToTry.Add(StandardPath);
	}

	for (const FString& TestPath : PathsToTry)
	{
		if (TestPath.IsEmpty())
		{
			continue;
		}

		FString PathToUse = TestPath;

		// ConvertToAbsolutePathForExternalAppForRead returns "Pak: /path/to/pakchunk10-Windows.pak/Content/Asset.uasset"
		FString AbsolutePath = PakPlatformFile->ConvertToAbsolutePathForExternalAppForRead(*PathToUse);
		bool bExists = PakPlatformFile->FileExists(*PathToUse);
		if (!bExists)
		{
			bExists = PakPlatformFile->FileExists(*AbsolutePath);
			PathToUse = AbsolutePath;
		}

		if (!bExists)
		{
			continue;
		}

		// Loose files on physical disk take precedence over pak.
		IPlatformFile& PhysicalPlatformFile = FPlatformFileManager::Get().GetPlatformPhysical();
		if (PhysicalPlatformFile.FileExists(*PathToUse))
		{
			return FAssetContainerInfo{EAssetContainerType::Unknown, -1, TEXT(""), false};
		}

		int32 ChunkID = 0;
		FString PakFileName;

		static const FString PakPrefix = TEXT("Pak: ");
		if (AbsolutePath.StartsWith(PakPrefix))
		{
			FString PathAfterPrefix = AbsolutePath.Mid(PakPrefix.Len());
			int32 PakExtPos = PathAfterPrefix.Find(TEXT(".pak"), ESearchCase::IgnoreCase);
			if (PakExtPos != INDEX_NONE)
			{
				int32 PakEndPos = PakExtPos + 4;
				FString FullPakPath = PathAfterPrefix.Left(PakEndPos);
				PakFileName = FPaths::GetCleanFilename(FullPakPath);
				ChunkID = ExtractChunkIDFromPakFilename(PakFileName);
			}
		}

		return FAssetContainerInfo{
			EAssetContainerType::Pak,
			ChunkID,
			PakFileName.IsEmpty() ? TEXT("PakFile") : PakFileName,
			true
		};
	}

	return FAssetContainerInfo{EAssetContainerType::Unknown, -1, TEXT(""), false};
}

// Checks container flags cache first, then falls back to name-based heuristics.
EAssetContainerType FAssetReadLogger::DetermineIoStoreType(const FString& ContainerName) const
{
	uint8 CachedFlags = GetCachedContainerFlags(ContainerName);
	if (IsOnDemandContainer(CachedFlags))
	{
		return EAssetContainerType::IoStoreOnDemand;
	}

	FString LowerName = ContainerName.ToLower();

	if (LowerName.Contains(TEXT("ondemand")) || LowerName.Contains(TEXT("_oad")))
	{
		return EAssetContainerType::IoStoreOnDemand;
	}

	if (LowerName.Contains(TEXT("streaming")) || LowerName.Contains(TEXT("_str")))
	{
		return EAssetContainerType::IoStoreStreaming;
	}

	return EAssetContainerType::IoStoreLocal;
}

uint8 FAssetReadLogger::GetCachedContainerFlags(const FString& ContainerName) const
{
	{
		FRWScopeLock ReadLock(ContainerFlagsCacheLock, SLT_ReadOnly);
		if (const uint8* Found = ContainerFlagsCache.Find(ContainerName))
		{
			return *Found;
		}
	}

	FString BaseName = FPaths::GetBaseFilename(ContainerName);
	if (BaseName != ContainerName)
	{
		FRWScopeLock ReadLock(ContainerFlagsCacheLock, SLT_ReadOnly);
		if (const uint8* Found = ContainerFlagsCache.Find(BaseName))
		{
			return *Found;
		}
	}

	return 0;
}

bool FAssetReadLogger::IsOnDemandContainer(uint8 Flags) const
{
	constexpr uint8 OnDemandFlag = 0x10;  // EIoContainerFlags::OnDemand
	return (Flags & OnDemandFlag) != 0;
}

// Extracts chunk ID from pak filename. Format: "pakchunk10-WindowsClient.pak" -> 10
int32 FAssetReadLogger::ExtractChunkIDFromPakFilename(const FString& FileName) const
{
	static const FString PakChunkPrefix = TEXT("pakchunk");
	int32 StartPos = FileName.Find(PakChunkPrefix, ESearchCase::IgnoreCase);

	if (StartPos == INDEX_NONE)
	{
		return 0; // Base chunk
	}

	StartPos += PakChunkPrefix.Len();

	int32 EndPos = INDEX_NONE;
	for (int32 i = StartPos; i < FileName.Len(); ++i)
	{
		TCHAR C = FileName[i];
		if (C == TEXT('-') || C == TEXT('.'))
		{
			EndPos = i;
			break;
		}
		if (!FChar::IsDigit(C))
		{
			break;
		}
	}

	if (EndPos == INDEX_NONE || EndPos <= StartPos)
	{
		return 0;
	}

	FString ChunkString = FileName.Mid(StartPos, EndPos - StartPos);
	return FCString::Atoi(*ChunkString);
}

// Returns on-disk size: tries UPackage::GetFileSize() -> IoDispatcher -> IFileManager.
int64 FAssetReadLogger::GetPackageSizeBytes(UPackage* Package) const
{
	int64 FileSize = Package->GetFileSize();
	if (FileSize > 0)
	{
		return FileSize;
	}

	FPackageId PackageId = Package->GetPackageId();
	if (PackageId.IsValid() && FIoDispatcher::IsInitialized())
	{
		FIoChunkId ChunkId = CreatePackageDataChunkId(PackageId);
		TIoStatusOr<uint64> ChunkSize = FIoDispatcher::Get().GetSizeForChunk(ChunkId);
		if (ChunkSize.IsOk())
		{
			return static_cast<int64>(ChunkSize.ValueOrDie());
		}
	}

	FString PackageFileName;
	const FPackagePath& LoadedPath = Package->GetLoadedPath();
	if (!LoadedPath.IsEmpty())
	{
		PackageFileName = LoadedPath.GetLocalFullPath();
	}

	if (PackageFileName.IsEmpty())
	{
		FPackageName::DoesPackageExist(Package->GetName(), &PackageFileName);
	}

	if (!PackageFileName.IsEmpty())
	{
		int64 Size = IFileManager::Get().FileSize(*PackageFileName);
		if (Size >= 0)
		{
			return Size;
		}
	}

	return 0;
}

// Writes JSON metadata as a comment line at the start of the CSV. Analysis scripts
// recognise the "# METADATA:" prefix and can parse platform, engine, and build info.
void FAssetReadLogger::WriteMetadataBlock()
{
	if (!OutputFileArchive)
	{
		return;
	}

	const FString PlatformName(FPlatformProperties::PlatformName());
	FString EngineVersion = FEngineVersion::Current().ToString();

#if UE_BUILD_DEBUG
	const TCHAR* BuildConfig = TEXT("Debug");
#elif UE_BUILD_DEVELOPMENT
	const TCHAR* BuildConfig = TEXT("Development");
#elif UE_BUILD_TEST
	const TCHAR* BuildConfig = TEXT("Test");
#elif UE_BUILD_SHIPPING
	const TCHAR* BuildConfig = TEXT("Shipping");
#else
	const TCHAR* BuildConfig = TEXT("Unknown");
#endif

	FString OSVersion = FPlatformMisc::GetOSVersion();
	const FString LogTimestamp = FDateTime::UtcNow().ToString(TEXT("%Y-%m-%dT%H:%M:%S"));

	auto SanitizeForJson = [](FString& Str)
	{
		Str.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Str.ReplaceInline(TEXT("\""), TEXT("\\\""));
	};

	FString SafePlatform     = PlatformName;
	FString SafeEngineVer    = EngineVersion;
	FString SafeOSVersion    = OSVersion;
	FString SafeTimestamp    = LogTimestamp;

	SanitizeForJson(SafePlatform);
	SanitizeForJson(SafeEngineVer);
	SanitizeForJson(SafeOSVersion);
	SanitizeForJson(SafeTimestamp);

	const FString MetadataLine = FString::Printf(
		TEXT("# METADATA: {\"platform\":\"%s\",\"engine_version\":\"%s\",\"build_config\":\"%s\",\"os_version\":\"%s\",\"log_timestamp\":\"%s\"}\n"),
		*SafePlatform,
		*SafeEngineVer,
		BuildConfig,
		*SafeOSVersion,
		*SafeTimestamp
	);

	FTCHARToUTF8 MetadataUtf8(*MetadataLine);
	OutputFileArchive->Serialize(const_cast<ANSICHAR*>(MetadataUtf8.Get()), MetadataUtf8.Length());
	OutputFileArchive->Flush();

	UE_LOG(LogAssetReadLogger, Log, TEXT("Platform metadata: platform=%s engine=%s config=%s"),
		*PlatformName, *EngineVersion, BuildConfig);
}

FString FAssetReadLogger::FormatCSVLine(const FAssetReadInfo& Asset) const
{
	return FString::Printf(TEXT("%s,%s,%s,%d,%s,%lld,%.3f,%s"),
		*EscapeCSVField(Asset.AssetPath),
		*EscapeCSVField(Asset.MountPoint),
		Asset.GetContainerTypeString(),
		Asset.ChunkID,
		*EscapeCSVField(Asset.ContainerName),
		Asset.SizeBytes,
		Asset.LoadTimestamp,
		*EscapeCSVField(Asset.ThreadName));
}

// RFC 4180 CSV escaping: wraps in quotes and escapes internal quotes if needed.
FString FAssetReadLogger::EscapeCSVField(const FString& Field)
{
	if (Field.Contains(TEXT(",")) || Field.Contains(TEXT("\"")) || Field.Contains(TEXT("\n")))
	{
		FString Escaped = Field.Replace(TEXT("\""), TEXT("\"\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}
	return Field;
}

FString FAssetReadLogger::GetCurrentThreadName()
{
	if (IsInGameThread())
	{
		return TEXT("GameThread");
	}
	FRunnableThread* CurrentThread = FRunnableThread::GetRunnableThread();
	if (CurrentThread)
	{
		FString Name = CurrentThread->GetThreadName();
		if (!Name.IsEmpty())
		{
			return Name;
		}
	}

	uint32 ThreadId = FPlatformTLS::GetCurrentThreadId();
	return FString::Printf(TEXT("Thread_%u"), ThreadId);
}

void FAssetReadLogger::PrintContainerStatistics() const
{
	UE_LOG(LogAssetReadLogger, Log, TEXT(""));
	UE_LOG(LogAssetReadLogger, Log, TEXT("Assets by container type:"));

	FRWScopeLock ReadLock(AssetReadLock, SLT_ReadOnly);

	for (const auto& Pair : StatsByContainerType)
	{
		const TCHAR* TypeName = LexToString(Pair.Key);
		const FAssetContainerStats& Stats = Pair.Value;

		UE_LOG(LogAssetReadLogger, Log, TEXT("  %s: %d assets, %.2f MB"),
			TypeName,
			Stats.Count,
			(double)Stats.TotalSize / (1024.0 * 1024.0));
	}
}

// Stores requesting thread name for later attribution when OnPackageLoadCompleted fires on GameThread.
void FAssetReadLogger::OnAsyncLoadPackageRequested(FStringView PackageName)
{
	if (!bIsEnabled.load())
	{
		return;
	}

	FString PackageNameStr(PackageName);
	FString ThreadName = GetCurrentThreadName();

	{
		FScopeLock Lock(&AsyncLoadRequestLock);
		AsyncLoadRequestThreads.Add(MoveTemp(PackageNameStr), MoveTemp(ThreadName));
	}
}

bool FAssetReadLogger::IsTrackableAssetPath(const FString& AssetPath)
{
	if (AssetPath.StartsWith(TEXT("/Temp/")) ||
		AssetPath.StartsWith(TEXT("/Memory/")) ||
		AssetPath.StartsWith(TEXT("/Script/")) ||
		AssetPath.StartsWith(TEXT("/Engine/Transient")) ||
		AssetPath == TEXT("None") ||
		AssetPath.IsEmpty())
	{
		return false;
	}

	return true;
}

// Iterates all UPackage objects and logs any that were loaded before hooks were registered.
// Uses "PreInit" thread name to identify these entries in the CSV.
void FAssetReadLogger::BackfillLoadedPackages()
{
	check(IsInGameThread());

	if (!bIsEnabled.load())
	{
		return;
	}

	UE_LOG(LogAssetReadLogger, Log, TEXT("Backfilling already-loaded packages..."));

	int32 BackfilledCount = 0;
	int32 SkippedCount = 0;

	ForEachObjectOfClass(UPackage::StaticClass(), [this, &BackfilledCount, &SkippedCount](UObject* Obj)
	{
		UPackage* Package = Cast<UPackage>(Obj);
		if (!Package)
		{
			return;
		}

		FString AssetPath = Package->GetPathName();

		if (!IsTrackableAssetPath(AssetPath))
		{
			SkippedCount++;
			return;
		}

		FAssetContainerInfo ContainerInfo = DetectAssetContainer(Package);

		int64 SizeBytes = GetPackageSizeBytes(Package);
		if (SizeBytes <= 0)
		{
			SkippedCount++;
			return;
		}

		LogAssetRead(AssetPath, ContainerInfo.Type, ContainerInfo.ChunkID,
			ContainerInfo.Name, SizeBytes, TEXT("PreInit"));

		BackfilledCount++;
	});

	UE_LOG(LogAssetReadLogger, Log, TEXT("Backfill complete: %d packages captured, %d skipped (transient/script/zero-size)"),
		BackfilledCount, SkippedCount);
}

// Scans all UPackage objects for any missed by delegate hooks. Uses "Sweep" thread name.
void FAssetReadLogger::SweepForMissedPackages()
{
	check(IsInGameThread());
	if (!bIsEnabled.load())
	{
		return;
	}

	UE_LOG(LogAssetReadLogger, Log, TEXT("Sweeping for missed packages..."));

	int32 NewCount = 0;
	int32 AlreadyTrackedCount = 0;
	ForEachObjectOfClass(UPackage::StaticClass(), [this, &NewCount, &AlreadyTrackedCount](UObject* Obj)
	{
		UPackage* Package = Cast<UPackage>(Obj);
		if (!Package)
		{
			return;
		}

		FString AssetPath = Package->GetPathName();
		if (!IsTrackableAssetPath(AssetPath))
		{
			return;
		}

		uint64 PathHash = CityHash64(reinterpret_cast<const char*>(*AssetPath), AssetPath.Len() * sizeof(TCHAR));
		{
			FRWScopeLock ReadLock(AssetReadLock, SLT_ReadOnly);
			FAssetReadInfo LookupInfo;
			LookupInfo.PathHash = PathHash;
			if (UniqueAssetReads.Contains(LookupInfo))
			{
				AlreadyTrackedCount++;
				return;
			}
		}

		FAssetContainerInfo ContainerInfo = DetectAssetContainer(Package);
		int64 SizeBytes = GetPackageSizeBytes(Package);
		if (SizeBytes <= 0)
		{
			return;
		}

		LogAssetRead(AssetPath, ContainerInfo.Type, ContainerInfo.ChunkID,
			ContainerInfo.Name, SizeBytes, TEXT("Sweep"));

		NewCount++;
	});

	UE_LOG(LogAssetReadLogger, Log, TEXT("Sweep complete: %d new packages found, %d already tracked"),
		NewCount, AlreadyTrackedCount);
}

int32 FAssetReadLogger::CountLoadedPackages() const
{
	int32 TotalCount = 0;
	int32 TrackableCount = 0;

	ForEachObjectOfClass(UPackage::StaticClass(), [&TotalCount, &TrackableCount](UObject* Obj)
	{
		TotalCount++;
		UPackage* Package = Cast<UPackage>(Obj);
		if (Package)
		{
			FString AssetPath = Package->GetPathName();
			if (IsTrackableAssetPath(AssetPath))
			{
				TrackableCount++;
			}
		}
	});

	UE_LOG(LogAssetReadLogger, Log, TEXT("Total UPackage objects in memory: %d (%d trackable)"),
		TotalCount, TrackableCount);

	return TrackableCount;
}

// Builds package-to-container cache from pak mount events. Also reads .utoc header
// to cache container flags for IAD detection on platforms without IoDispatcher metadata.
void FAssetReadLogger::OnPakFileMounted(const IPakFile& PakFile)
{
	if (!bIsEnabled.load())
	{
		return;
	}

	const FString& PakFileName = PakFile.PakGetPakFilename();
	const FString PakBaseName = FPaths::GetCleanFilename(PakFileName);
	const int32 ChunkID = PakFile.PakGetPakchunkIndex();
	EAssetContainerType ContainerType = DetermineIoStoreType(PakBaseName);
	const FString& MountPoint = PakFile.PakGetMountPoint();

	FString TocPath = FPaths::ChangeExtension(PakFileName, TEXT(".utoc"));
	if (FPaths::FileExists(TocPath))
	{
		FIoStoreTocResource TocResource;
		FIoStatus Status = FIoStoreTocResource::Read(*TocPath, EIoStoreTocReadOptions::ReadTocMeta, TocResource);
		if (Status.IsOk())
		{
			EIoContainerFlags Flags = TocResource.Header.ContainerFlags;
			FString ContainerName = FPaths::GetBaseFilename(TocPath);

			{
				FRWScopeLock WriteLock(ContainerFlagsCacheLock, SLT_Write);
				ContainerFlagsCache.Add(ContainerName, static_cast<uint8>(Flags));
			}

			if (EnumHasAnyFlags(Flags, EIoContainerFlags::OnDemand))
			{
				ContainerType = EAssetContainerType::IoStoreOnDemand;
				UE_LOG(LogAssetReadLogger, Log, TEXT("Container %s has OnDemand flag - marking as IAD"), *ContainerName);
			}
		}
	}

	UE_LOG(LogAssetReadLogger, Verbose, TEXT("Caching package mappings for pak: %s (Chunk %d, Mount: %s)"),
		*PakBaseName, ChunkID, *MountPoint);

	FCachedContainerInfo CachedInfo;
	CachedInfo.ContainerName = PakBaseName;
	CachedInfo.ChunkID = ChunkID;
	CachedInfo.Type = (ContainerType == EAssetContainerType::IoStoreLocal) ? EAssetContainerType::Pak : ContainerType;

	int32 CachedCount = 0;

	class FPakVisitor : public IPlatformFile::FDirectoryVisitor
	{
	public:
		FAssetReadLogger* Owner;
		const FCachedContainerInfo* Info;
		const FString* MountPointPtr;
		int32* CountPtr;

		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
		{
			if (!bIsDirectory)
			{
				FString FullPath = *MountPointPtr / FilenameOrDirectory;
				FullPath.ToLowerInline();
				FPaths::NormalizeFilename(FullPath);

				int32 OutIndex;
				if(FullPath.FindLastChar('.', OutIndex))
				{
					FullPath = FullPath.Left(OutIndex);
				}

				{
					FRWScopeLock WriteLock(Owner->PackageContainerCacheLock, SLT_Write);
					Owner->PackageToContainerCache.Add(FullPath, *Info);
				}
				(*CountPtr)++;
			}
			return true;
		}
	};

	FPakVisitor Visitor;
	Visitor.Owner = this;
	Visitor.Info = &CachedInfo;
	Visitor.MountPointPtr = &MountPoint;
	Visitor.CountPtr = &CachedCount;

	PakFile.PakVisitPrunedFilenames(Visitor);

	UE_LOG(LogAssetReadLogger, Log, TEXT("Cached %d package mappings from pak: %s"), CachedCount, *PakBaseName);
}

FAssetContainerInfo FAssetReadLogger::LookupCachedContainerInfo(const FString& PackageName) const
{
	FString PackageFilePath;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilePath))
	{
		return FAssetContainerInfo{EAssetContainerType::Unknown, -1, TEXT(""), false};
	}

	FString FullPath = PackageFilePath;
	FullPath.ToLowerInline();
	FPaths::NormalizeFilename(FullPath);

	FRWScopeLock ReadLock(PackageContainerCacheLock, SLT_ReadOnly);
	if (const FCachedContainerInfo* CachedInfo = PackageToContainerCache.Find(FullPath))
	{
		return FAssetContainerInfo{
			CachedInfo->Type,
			CachedInfo->ChunkID,
			CachedInfo->ContainerName,
			true
		};
	}

	return FAssetContainerInfo{EAssetContainerType::Unknown, -1, TEXT(""), false};
}

#endif // (!UE_BUILD_SHIPPING || defined(UE_ENABLE_ASSET_READ_LOGGER))
