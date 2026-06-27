// Copyright Epic Games, Inc. All Rights Reserved.

#include "VerseVM/VVMVerseHangDetection.h"
#include "AutoRTFM.h"
#include "AutoRTFM/Defines.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMath.h"
#include "HAL/PlatformTime.h"
#include "VerseVM/VVMLog.h"

// TODO: Once the new VM lands, we should attempt to reduce GVerseHangDetectionThresholdSeconds back to 3.0. #jira SOL-7622
static float GVerseHangDetectionThresholdSeconds = 9.0f;
static float GVerseHangDetectionThresholdSecondsDuringCook = 120.0f;
static bool GVerseHangDetectionDuringDebugging = false;
static bool GVerseHangDetectionExcludeOverhead = true;

static void OnVerseHangDetectionThresholdChanged()
{
	UE_LOGF(LogVerseVM, Log, "Verse hang detection threshold changed to '%f'", GVerseHangDetectionThresholdSeconds);
}

static FAutoConsoleVariableRef CVarVerseHangDetectionThreshold(
	TEXT("verse.HangDetectionThresholdSeconds"),
	GVerseHangDetectionThresholdSeconds,
	TEXT("Maximum time a Verse script is permitted to run before a runtime error is triggered.\n"),
	FConsoleVariableDelegate::CreateStatic([](IConsoleVariable*) { OnVerseHangDetectionThresholdChanged(); }),
	ECVF_Default);

static FAutoConsoleVariableRef CVarVerseHangDetectionThresholdDuringCook(
	TEXT("verse.HangDetectionThresholdSecondsDuringCook"),
	GVerseHangDetectionThresholdSecondsDuringCook,
	TEXT("Maximum time a Verse script is permitted to run before a runtime error is triggered - in the cooker.\n"),
	ECVF_Default);

static FAutoConsoleVariableRef CVarVerseHangDetectionDuringDebugging(
	TEXT("verse.HangDetectionDuringDebugging"),
	GVerseHangDetectionDuringDebugging,
	TEXT("True if verse hang detection should be enabled during debugging.\n"),
	ECVF_Default);

static FAutoConsoleVariableRef CVarVerseHangDetectionExcludeOverhead(
	TEXT("verse.HangDetectionExcludeOverhead"),
	GVerseHangDetectionExcludeOverhead,
	TEXT("True if verse hang detection should exclude overhead scopes, like Ensure stack walks.\n"),
	ECVF_Default);

namespace
{
class FOverheadTracker
{
public:
	static FOverheadTracker& Get()
	{
		thread_local FOverheadTracker TrackerInstance;
		return TrackerInstance;
	}

	void InstallOverheadTracking(TFunction<void()> InOnOverheadBegin, TFunction<void(double Duration)> InOnOverheadEnd)
	{
		if (!bInstalled)
		{
			UE_LOGF(LogVerseVM, VeryVerbose, "OverheadTracking INSTALL: HasOnBegin=%d HasOnEnd=%d ThreadId=%u", (bool)InOnOverheadBegin, (bool)InOnOverheadEnd, FPlatformTLS::GetCurrentThreadId());

			OnOverheadBegin = MoveTemp(InOnOverheadBegin);
			OnOverheadEnd = MoveTemp(InOnOverheadEnd);

#if UE_TRACK_ENSURE_OVERHEAD_DURING_SCRIPTS
			// Cache the old Ensure handler so we can call it within ours and restore it later.
			PreviousEnsureHandler = SetEnsureHandler([this](const FEnsureHandlerArgs& Args) -> bool {
				BeginOverheadScope();
				return PreviousEnsureHandler ? PreviousEnsureHandler(Args) : false;
			});
#endif // UE_TRACK_ENSURE_OVERHEAD_DURING_SCRIPTS

			bInstalled = true;
		}
		else
		{
			UE_LOGF(LogVerseVM, Warning, "OverheadTracking INSTALL SKIPPED (already installed): ThreadId=%u", FPlatformTLS::GetCurrentThreadId());
		}
	}

	void UninstallOverheadTracking()
	{
		if (bInstalled)
		{
			UE_LOGF(LogVerseVM, VeryVerbose, "OverheadTracking UNINSTALL: HasOnBegin=%d HasOnEnd=%d ThreadId=%u", (bool)OnOverheadBegin, (bool)OnOverheadEnd, FPlatformTLS::GetCurrentThreadId());

			OnOverheadBegin = nullptr;
			OnOverheadEnd = nullptr;

#if UE_TRACK_ENSURE_OVERHEAD_DURING_SCRIPTS
			// Restore the old Ensure handler.
			SetEnsureHandler(MoveTemp(PreviousEnsureHandler));
#endif // UE_TRACK_ENSURE_OVERHEAD_DURING_SCRIPTS

			bInstalled = false;
		}
	}

	void BeginOverheadScope()
	{
		if (NestingDepth == 0)
		{
			UE_LOGF(LogVerseVM, Log, "BeginOverheadScope HasOnBegin=%d", (bool)OnOverheadBegin);

			OverheadStartTime = FPlatformTime::Seconds();
			if (OnOverheadBegin)
			{
				OnOverheadBegin();
			}
		}

		NestingDepth++;
	}

	void EndOverheadScope()
	{
		if (NestingDepth > 0)
		{
			--NestingDepth;
			if (NestingDepth == 0)
			{
				const double OverheadDuration = FPlatformTime::Seconds() - OverheadStartTime;

				UE_LOGF(LogVerseVM, Log, "EndOverheadScope OverheadDuration=%.2f HasOnEnd=%d", OverheadDuration, (bool)OnOverheadBegin);

				OverheadStartTime = 0.0;
				if (OnOverheadEnd)
				{
					OnOverheadEnd(OverheadDuration);
				}
			}
		}
	}

	int32 NestingDepth = 0;
	double OverheadStartTime = 0.0;
	bool bInstalled = false;

	TFunction<void()> OnOverheadBegin;
	TFunction<void(double)> OnOverheadEnd;

#if UE_TRACK_ENSURE_OVERHEAD_DURING_SCRIPTS
	// Cached previous Ensure handler that needs to be reinstated on timer reset.
	TFunction<bool(const FEnsureHandlerArgs& Args)> PreviousEnsureHandler;
#endif // UE_TRACK_ENSURE_OVERHEAD_DURING_SCRIPTS
};
} // namespace

namespace VerseHangDetection
{

float VerseHangThreshold()
{
	float HangThreshold = GVerseHangDetectionThresholdSeconds;

#if !UE_BUILD_SHIPPING
	if (::IsRunningCommandlet())
	{
		return GVerseHangDetectionThresholdSecondsDuringCook;
	}

	constexpr float VeryLargeHangThreshold = 120.0f;

	if (VeryLargeHangThreshold > GVerseHangDetectionThresholdSeconds)
	{
// The AutoRTFM sanitizer can significantly increase the runtime of Verse
// code as we are performing additional checks that transactional code
// behaves correctly with respect to memory, and thus we bump the hang
// threshold to compensate for this additional checking.
#if AUTORTFM_SANITIZER
		HangThreshold = VeryLargeHangThreshold;
#else
		// If we are using the aborteroonie 'retry all transactions at least once to check we abort correctly' mode, we
		// bump the hang threshold as we are running every transaction nest at least *twice*.
		if (AutoRTFM::ForTheRuntime::GetRetryTransaction() != AutoRTFM::ForTheRuntime::EAutoRTFMRetryTransactionState::NoRetry)
		{
			HangThreshold = VeryLargeHangThreshold;
		}
#endif
	}
#endif // !UE_BUILD_SHIPPING

	return HangThreshold;
}

bool ShouldExcludeOverhead()
{
	return GVerseHangDetectionExcludeOverhead;
}

bool IsComputationLimitExceeded(const double StartTime, double HangThreshold)
{
	if (StartTime == 0.0f)
	{
		return false;
	}

	double RunningTime = ::FPlatformTime::Seconds() - StartTime;
	if (RunningTime < HangThreshold || (!GVerseHangDetectionDuringDebugging && ::FPlatformMisc::IsDebuggerPresent()))
	{
		return false;
	}
	return true;
}

void InstallOverheadTracking(TFunction<void()> OnOverheadBegin, TFunction<void(double Duration)> OnOverheadEnd)
{
	FOverheadTracker::Get().InstallOverheadTracking(MoveTemp(OnOverheadBegin), MoveTemp(OnOverheadEnd));
}

void UninstallOverheadTracking()
{
	FOverheadTracker::Get().UninstallOverheadTracking();
}

void BeginOverheadScope()
{
	FOverheadTracker::Get().BeginOverheadScope();
}

void EndOverheadScope()
{
	FOverheadTracker::Get().EndOverheadScope();
}

} // namespace VerseHangDetection
