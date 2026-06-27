// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Async/EventCount.h"
#include "AutoRTFM/Defines.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Templates/UniquePtr.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMFrame.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMTask.h"

#include "Trace/Config.h"
#include "Trace/Detail/Channel.h"
#include "Trace/Trace.h"

namespace Verse
{
// Enabled via cmd-line arg `-trace=verse` passed to the program being sampled
UE_TRACE_MINIMAL_CHANNEL_CUSTOM_EXTERN(VerseChannel, FVerseChannel, COREUOBJECT_API)

struct FSampledFrame
{
	TWriteBarrier<VFrame> Frame;
	bool bIsNativeCall{false};
	TArray<TWriteBarrier<VUniqueString>> Callstack;
	uint32 BytecodeOffset;
	uint64 Cycles;
	void MarkReferencedCells(FMarkStack& MarkStack)
	{
		MarkStack.Mark(Frame.Get());
		for (uint32 Index = 0; Index < Callstack.Num(); ++Index)
		{
			MarkStack.MarkNonNull(Callstack[Index].Get());
		}
	}
};

// - TODO: Redact non-user visible callstacks
struct AUTORTFM_DISABLE FSamplingProfiler final : FRunnable
{
	struct AUTORTFM_DISABLE FProcessor final : FRunnable
	{
		COREUOBJECT_API void Start(FSamplingProfiler* InProfiler);
		COREUOBJECT_API uint32 Run() override;

		FSamplingProfiler* Profiler = nullptr;
		TUniquePtr<FRunnableThread> Thread;
	};

	COREUOBJECT_API FSamplingProfiler();

	COREUOBJECT_API void Start();
	void Pause() { bPauseRequested.store(true, std::memory_order_seq_cst); }

	// FRunnable Interface
	COREUOBJECT_API uint32 Run() override;
	void Stop() override
	{
		bStopRequested.store(true, std::memory_order_seq_cst);
		WaitEvent.Notify();
	}
	COREUOBJECT_API void Exit() override;
	// ~FRunnable Interface

	void Sample(FRunningContext Context, FOp* PC, VFrame* Frame, VTask* Task);
	COREUOBJECT_API void ForceSampleCurrentContext();
	void ProcessSamples();
	void MarkReferencedCells(FMarkStack&);

	TUniquePtr<FRunnableThread> Thread;
	FProcessor Processor;

	TArray<FSampledFrame> Samples; // Written by Sample() under SampleMutex

	// Used to trace the strings already emitted to insights this session
	// TODO: Reset these upon a new insights connection
	uint32 StringIdCounter = 0;
	TMap<VUniqueString*, uint32> TracedStringIds;

	bool bIsRunning = false;
	std::atomic<bool> SampleRequested = false;
	std::atomic<bool> bStopRequested = false;
	std::atomic<bool> bPauseRequested = false;

	UE::FMutex SampleMutex;     // Sample() (mutator) VS ProcessSamples() (processor)
	UE::FMutex ProcessingMutex; // MarkReferencedCells() VS ProcessSamples() (processor)
	UE::FEventCount WaitEvent;
};

COREUOBJECT_API FSamplingProfiler* GetSamplingProfiler();
COREUOBJECT_API FSamplingProfiler* GetRunningSamplingProfiler();
COREUOBJECT_API void SetSamplingProfiler(FSamplingProfiler*);

} // namespace Verse

#endif
