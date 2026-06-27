// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMSamplingProfiler.h"

#include "Async/UniqueLock.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Fork.h"
#include "VerseVM/Inline/VVMContextImplInline.h"
#include "VerseVM/VVMBytecodesAndCaptures.h"
#include "VerseVM/VVMLog.h"

#include "Trace/Detail/Channel.h"
#include "Trace/Trace.inl"

namespace Verse
{
COREUOBJECT_API extern FOpErr StopInterpreterSentry;

class AUTORTFM_DISABLE FVerseChannel : public UE::Trace::FChannel
{
public:
#if UE_TRACE_MINIMAL_ENABLED
	virtual bool OnToggle(bool bRequestedState,
		const TCHAR** OutReason) override
	{
		FSamplingProfiler* Sampler = GetSamplingProfiler();
		if (!Sampler)
		{
			Sampler = new FSamplingProfiler();
			SetSamplingProfiler(Sampler);
			UE_LOGF(LogVerseVM, Log, "Created Verse sampling profiler");
		}
		return true;
	}

	virtual void OnToggled(bool bNewState) override
	{
		FSamplingProfiler* Sampler = GetSamplingProfiler();
		if (Sampler == nullptr)
		{
			return;
		}

		if (bNewState)
		{
			UE_LOGF(LogVerseVM, Log, "Starting Verse sampling profiler");
			Sampler->Start();
		}
		else
		{
			UE_LOGF(LogVerseVM, Log, "Stopping Verse sampling profiler");
			Sampler->Stop();
		}
	}
#endif // UE_TRACE_MINIMAL_ENABLED
};

UE_TRACE_MINIMAL_CHANNEL_CUSTOM_DEFINE(VerseChannel, FVerseChannel,
	"Verse sampling profiler", false);

UE_TRACE_MINIMAL_EVENT_BEGIN(Verse, DeclareString)
UE_TRACE_MINIMAL_EVENT_FIELD(uint32, Id)
UE_TRACE_MINIMAL_EVENT_FIELD(UE::Trace::WideString, Name)
UE_TRACE_MINIMAL_EVENT_END()

UE_TRACE_MINIMAL_EVENT_BEGIN(Verse, BytecodeSample)
UE_TRACE_MINIMAL_EVENT_FIELD(uint64, Cycles)
UE_TRACE_MINIMAL_EVENT_FIELD(uint32, BytecodeOffset)
UE_TRACE_MINIMAL_EVENT_FIELD(uint32, Line)
UE_TRACE_MINIMAL_EVENT_FIELD(uint32[], Callstack)
UE_TRACE_MINIMAL_EVENT_END()

UE_TRACE_MINIMAL_EVENT_BEGIN(Verse, NativeSample)
UE_TRACE_MINIMAL_EVENT_FIELD(uint64, Cycles)
UE_TRACE_MINIMAL_EVENT_FIELD(uint32[], Callstack)
UE_TRACE_MINIMAL_EVENT_END()

static FSamplingProfiler* GSamplingProfiler = nullptr;

FSamplingProfiler* GetSamplingProfiler()
{
	return GSamplingProfiler;
}

FSamplingProfiler* GetRunningSamplingProfiler()
{
	return GSamplingProfiler && GSamplingProfiler->bIsRunning ? GSamplingProfiler
															  : nullptr;
}

void SetSamplingProfiler(FSamplingProfiler* SamplingProfiler)
{
	V_DIE_IF(GSamplingProfiler);
	V_DIE_UNLESS(SamplingProfiler);
	StoreStoreFence();
	GSamplingProfiler = SamplingProfiler;
}

// ***** Processor *****
void FSamplingProfiler::FProcessor::Start(FSamplingProfiler* InProfiler)
{
	Profiler = InProfiler;
	Thread = TUniquePtr<FRunnableThread>(FForkProcessHelper::CreateForkableThread(
		this, TEXT("Verse VM Sampling Profiler (Processor)")));
}

uint32 FSamplingProfiler::FProcessor::Run()
{
	while (true)
	{
		UE::FEventCountToken Token = Profiler->WaitEvent.PrepareWait();
		if (Profiler->bStopRequested)
		{
			return 0;
		}

		if (Profiler->bPauseRequested)
		{
			Profiler->WaitEvent.Wait(Token);
			continue;
		}

		// Wait 100ms between processing to hopefully batch up some samples
		FPlatformProcess::Sleep(100.0f / 1000.0f);

		Profiler->ProcessSamples();
	}
}

// ***** Sampler *****
FSamplingProfiler::FSamplingProfiler()
{
	Start();
}

void FSamplingProfiler::Start()
{
	if (!Thread)
	{
		// Make forkable as otherwise we don't actually run on UEFN's
		// single-threaded servers
		Thread =
			TUniquePtr<FRunnableThread>(FForkProcessHelper::CreateForkableThread(
				this, TEXT("Verse VM Sampling Profiler (Sampling)")));
		Processor.Start(this);
	}

	bPauseRequested.store(false, std::memory_order_seq_cst);
	bStopRequested.store(false, std::memory_order_seq_cst);
	WaitEvent.Notify();
}

uint32 FSamplingProfiler::Run()
{
	FIOContext::Create([this](FIOContext Context) {
		bIsRunning = true;
		while (true)
		{
			UE::FEventCountToken Token = WaitEvent.PrepareWait();
			if (bStopRequested)
			{
				break;
			}

			if (bPauseRequested)
			{
				WaitEvent.Wait(Token);
				continue;
			}

			FPlatformProcess::Sleep(1.0f / 1000.0f);

			TArray<FContextImpl*> Snapshot;
			{
				UE::TUniqueLock Lock(FContextImpl::LiveAndFreeContextsMutex);
				Snapshot = FContextImpl::LiveContexts->Array();
			}
			for (FContextImpl* Impl : Snapshot)
			{
				if (Impl->NativeFrame() != nullptr)
				{
					// TODO: This samples each context... but blocks on handshake which
					// drops our sample ~1ms rate per context We really need to request all
					// handshakes at once and then wait for them but we need to be careful
					// not to lock around 'HandshakeMutex'
					SampleRequested.store(true, std::memory_order_seq_cst);
					FRunningContext LiveContext(Impl, EIsInHandshake::No);
					Context.PairHandshake(LiveContext, [](FHandshakeContext HandshakeContext) {});
				}
			}
		}
		bIsRunning = false;
	});
	return 0;
}

void FSamplingProfiler::Exit()
{
	// Wait for the processor thread to finish
	if (Processor.Thread)
	{
		Processor.Thread->WaitForCompletion();
		Processor.Thread = nullptr;
	}

	Thread = nullptr;
	bStopRequested.store(false, std::memory_order_seq_cst);
	bPauseRequested.store(false, std::memory_order_seq_cst);
}

void FSamplingProfiler::ForceSampleCurrentContext()
{
	FContextImpl* const Impl = FContextImpl::GetCurrentImpl();
	if (!Impl || !Impl->IsLive())
	{
		return;
	}
	FRunningContext Context(Impl, EIsInHandshake::No);
	Sample(Context, nullptr, nullptr, nullptr);
}

void FSamplingProfiler::Sample(FRunningContext Context, FOp* PC, VFrame* Frame,
	VTask* Task)
{
	if (PC == &StopInterpreterSentry || Frame == VFrame::GlobalEmptyFrame.Get())
	{
		// Top-level task wrap-up: State.Frame is GlobalEmptyFrame and the only
		// native frame is the EnterVM root with Callee=null, which would produce an
		// empty callstack. Drop the sample.
		return;
	}

	TArray<TWriteBarrier<VUniqueString>> Callstack;
	Context.NativeFrame()->WalkCallstackFrames(
		PC, Frame, Task,
		[&Callstack](const FOp*, const VFrame* Frame) {
			if (Frame->Procedure && Frame->Procedure->Name && !Frame->Procedure->Name->AsStringView().IsEmpty())
			{
				Callstack.Add(Frame->Procedure->Name);
			}
		},
		[&Callstack](const FNativeFrame* NativeFrame) {
			if (const VNativeProcedure* Callee = NativeFrame->Callee;
				Callee && Callee->Name && !Callee->Name->AsStringView().IsEmpty())
			{
				Callstack.Add(Callee->Name);
			}
		});

	UE::TUniqueLock Lock(SampleMutex);
	Samples.Add(FSampledFrame{
		.Frame = {Context, Frame},
		.bIsNativeCall = !!Context.NativeFrame()->Callee,
		.Callstack = MoveTemp(Callstack),
		.BytecodeOffset = Frame ? Frame->Procedure->BytecodeOffset(*PC) : 0,
		.Cycles = FPlatformTime::Cycles64()
    });
}

void FSamplingProfiler::MarkReferencedCells(FMarkStack& MarkStack)
{
	for (auto& Pair : TracedStringIds)
	{
		MarkStack.MarkNonNull(Pair.Key);
	}

	UE::TUniqueLock ProcessingLock(
		ProcessingMutex);                    // Prevent GC collecting local copy's VCells in
											 // ProcessSamples()
	UE::TUniqueLock SampleLock(SampleMutex); // Prevent race against Sample()
	for (uint32 Index = 0; Index < Samples.Num(); ++Index)
	{
		Samples[Index].MarkReferencedCells(MarkStack);
	}
}

void FSamplingProfiler::ProcessSamples()
{
	auto GetAndTraceString = [this](VUniqueString* String) -> uint32 {
		if (uint32* Result = TracedStringIds.Find(String))
		{
			return *Result;
		}
		UE_TRACE_MINIMAL_LOG(Verse, DeclareString, VerseChannel)
			<< DeclareString.Id(++StringIdCounter)
			<< DeclareString.Name(*String->AsString());
		return TracedStringIds.Add(String, StringIdCounter);
	};

	UE::TUniqueLock GCLock(ProcessingMutex);
	TArray<FSampledFrame> LocalSamples;
	{
		UE::TUniqueLock Lock(SampleMutex);
		LocalSamples = MoveTemp(Samples);
	}

	for (uint32 Index = 0; Index < LocalSamples.Num(); ++Index)
	{
		FSampledFrame& Sample = LocalSamples[Index];
		TArray<uint32> CallstackIds;
		for (uint32 CallstackIndex = 0; CallstackIndex < Sample.Callstack.Num();
			 ++CallstackIndex)
		{
			CallstackIds.Add(
				GetAndTraceString(Sample.Callstack[CallstackIndex].Get()));
		}

		if (Sample.bIsNativeCall)
		{
			UE_TRACE_MINIMAL_LOG(Verse, NativeSample, VerseChannel)
				<< NativeSample.Cycles(Sample.Cycles)
				<< NativeSample.Callstack(CallstackIds.GetData(), CallstackIds.Num());
		}
		else if (VFrame* F = Sample.Frame.Get())
		{
			const FLocation* Location =
				F->Procedure->GetLocation(Sample.BytecodeOffset);
			UE_TRACE_MINIMAL_LOG(Verse, BytecodeSample, VerseChannel)
				<< BytecodeSample.Cycles(Sample.Cycles)
				<< BytecodeSample.BytecodeOffset(Sample.BytecodeOffset)
				<< BytecodeSample.Line(Location ? Location->Line : 0u)
				<< BytecodeSample.Callstack(CallstackIds.GetData(),
					   CallstackIds.Num());
		}
	}
}

} // namespace Verse

#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
