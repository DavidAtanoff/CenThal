// Copyright Epic Games, Inc. All Rights Reserved.

#include "ToolModes/AutomationMode.h"

#if !UE_BUILD_SHIPPING && WITH_DEV_AUTOMATION_TESTS

#include "BuildPatchTool.h"
#include "Containers/Ticker.h"
#include "IAutomationWorkerModule.h"
#include "IAutomationControllerModule.h"
#include "Interfaces/IBuildPatchServicesModule.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ToolModes/HelpToolMode.h"
#include "ToolModes/ToolModesHelp.h"
#include "UObject/UObjectGlobals.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAutomationMode, Warning, All);
DEFINE_LOG_CATEGORY(LogAutomationMode);

using namespace BuildPatchTool;

#if WITH_GOOGLE_TEST

THIRD_PARTY_INCLUDES_START
#include "gtest/gtest.h"
THIRD_PARTY_INCLUDES_END

class FGoogleTestRunnerPrinter
	: public ::testing::EmptyTestEventListener
{
	// Called after a failed assertion or a SUCCEED() invocation.
	virtual void OnTestPartResult(const ::testing::TestPartResult& test_part_result) override
	{
		if (FAutomationTestBase* const CurrentTest = FAutomationTestFramework::GetInstance().GetCurrentTest())
		{
			if (test_part_result.passed())
			{
				CurrentTest->AddInfo(test_part_result.message());
			}
			else if (test_part_result.skipped())
			{
				CurrentTest->AddWarning(test_part_result.message());
			}
			else
			{
				CurrentTest->AddError(FString::Printf(TEXT("\n%s File:%s Line:%s"), test_part_result.message(), test_part_result.file_name(), test_part_result.line_number()));
			}

		}
		else
		{
			UE_LOGF(LogAutomationMode, Warning, "GoogleTest expectation or assert called when no automation test was actively running!");
		}
	}
};

#endif
class FAutomationToolMode : public IToolMode
{
public:
	FAutomationToolMode(IBuildPatchServicesModule& InBpsInterface, const TCHAR* InCommandLine)
		: BpsInterface(InBpsInterface)
		, CommandLine(InCommandLine)
	{
#if WITH_GOOGLE_TEST
		::testing::TestEventListeners& Listeners = ::testing::UnitTest::GetInstance()->listeners();
		Listeners.Append(new FGoogleTestRunnerPrinter());
		Listeners.Release(Listeners.default_result_printer());
#endif

	}

	virtual ~FAutomationToolMode()
	{
	}

	virtual EReturnCode Execute() override
	{
		// Parse commandline
		if (ProcessCommandline() == false)
		{
			return EReturnCode::ArgumentProcessingError;
		}

		// Print help if requested
		if (bHelp)
		{
			PrintHelp<FAutomationToolModeHelp>();
			return EReturnCode::OK;
		}

		// Main loop.
		double DeltaTime = 0.0;
		double LastTime = FPlatformTime::Seconds();

		// Setup desired frame times.
		float MainsFramerate = 500.0f;
		const float MainsFrameTime = 1.0f / MainsFramerate;

		// Required modules.
		static const FName AutomationWorkerModuleName = TEXT("AutomationWorker");
		static const FName AutomationController("AutomationController");
		IAutomationWorkerModule& AutomationWorkerModule = FModuleManager::LoadModuleChecked<IAutomationWorkerModule>(AutomationWorkerModuleName);
		IAutomationControllerModule& AutomationControllerModule = FModuleManager::LoadModuleChecked<IAutomationControllerModule>(AutomationController);
		AutomationControllerModule.Init();
		IAutomationControllerManagerRef AutomationControllerManager = AutomationControllerModule.GetAutomationController();
		AutomationControllerManager->OnTestsComplete().AddLambda([]()
		{
			FPlatformMisc::RequestExit(false);
		});
		StaticExec(NULL, *TestList);

		while (!IsEngineExitRequested())
		{
			BeginExitIfRequested();

			// Increment global frame counter once for each app tick.
			GFrameCounter++;

			// Update sub-systems.
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FTSTicker::GetCoreTicker().Tick(DeltaTime);
			AutomationWorkerModule.Tick();
			AutomationControllerModule.Tick();

			// Flush threaded logs.
			GLog->FlushThreadedLogs(EOutputDeviceRedirectorFlushOptions::Async);

			// Throttle frame rate.
			FPlatformProcess::Sleep(FMath::Max<float>(0.0f, MainsFrameTime - (FPlatformTime::Seconds() - LastTime)));

			// Calculate deltas.
			const double AppTime = FPlatformTime::Seconds();
			DeltaTime = AppTime - LastTime;
			LastTime = AppTime;
		}

		// Check for failures and exit.
		TArray<TSharedPtr<IAutomationReport>> Reports = AutomationControllerManager->GetEnabledReports();
		bool bSuccess = !GIsCriticalError && RecursiveCheckReports(Reports);
		return bSuccess ? EReturnCode::OK : EReturnCode::ToolFailure;
	}

private:
	bool RecursiveCheckReports(const TArray<TSharedPtr<IAutomationReport>>& Reports, FString ParentTestName = TEXT(""))
	{
		bool bSuccess = true;
		for (const TSharedPtr<IAutomationReport>& Report : Reports)
		{
			if (Report.IsValid())
			{
				FString ReportName = (ParentTestName + Report->GetDisplayName());
				if (Report->HasErrors())
				{
					UE_LOGF(LogBuildPatchTool, Error, "%ls: Failed", *ReportName);
					bSuccess = false;
				}
				bSuccess = RecursiveCheckReports(Report->GetChildReports(), ReportName + TEXT(" ")) & bSuccess;
			}
		}
		return bSuccess;
	}

	bool ProcessCommandline()
	{
#define PARSE_SWITCH(Switch) ParseSwitch(TEXT(#Switch "="), Switch, Switches)
		TArray<FString> Tokens, Switches;
		FCommandLine::Parse(CommandLine, Tokens, Switches);

		bHelp = ParseOption(TEXT("help"), Switches);
		if (bHelp)
		{
			return true;
		}
		if (!PARSE_SWITCH(TestList) || TestList.Contains(TEXT(";")))
		{
			TestList = TEXT("BuildPatchServices+BuildPatchTool");
		}
		TestList.InsertAt(0, TEXT("Automation RunTests "));

		return true;
#undef PARSE_SWITCH
	}

private:
	static TCHAR const* const MODE_NAME;
	IBuildPatchServicesModule& BpsInterface;
	const TCHAR* CommandLine;
	bool bHelp;
	FString TestList;
};

IMPLEMENT_BPT_MODE(AutomationTests, FAutomationToolMode);

#endif // !UE_BUILD_SHIPPING
