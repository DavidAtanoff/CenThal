// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM/Defines.h"
#include "VerseVM/Inline/VVMWriteBarrierInline.h"
#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMDebugger.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMReturnSlot.h"
#include "VerseVM/VVMTaskNativeHook.h"
#include "VerseVM/VVMTree.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{
struct FOp;
struct VFailureContext;

struct VTask : VValueObject
	, TIntrusiveTree<VTask>
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VValueObject);
	COREUOBJECT_API static TGlobalHeapPtr<VEmergentType> EmergentType;

	// A task is "running" when it is associated with a frame on the native stack.
	// This includes a running interpreter (even if it is just on the `YieldTask` chain), and native
	// functions like `CancelChildren`.
	// Running tasks can only be resumed by falling through a sequence of yields and native returns.
	// This is independent of `Phase`, as both active and cancelling tasks may suspend.
	bool bRunning{true};

	// See the note on CancelImpl.
	enum class EPhase : int8
	{
		Active,
		CancelRequested,
		CancelStarted,
		CancelUnwind,
		Canceled,
	};
	EPhase Phase{EPhase::Active};

	bool bAwaitInitializing{false};
	// When the task is in an await, points to the start of the await body.
	// A read during an await registers the current Task/AwaitPC pair with the VRef.
	// This field is only meaningful when the effect token is concrete.
	FOp* AwaitPC{nullptr};

	// To be run on resume or unwind. May point back to the resumer.
	TWriteBarrier<VTaskNativeHook> NativeDefers;

	// Currently tracked suspended native Await() calls.
	// Invoked in FIFO order when this task completes.
	TWriteBarrier<VTaskNativeHook> NativeAwaitsHead;
	TWriteBarrier<VTaskNativeHook> NativeAwaitsTail;

	// Where execution should continue when resuming.
	FOp* ResumePC{nullptr};
	TWriteBarrier<VFrame> ResumeFrame;
	VReturnSlot ResumeSlot; // May point into ResumeFrame or one of its ancestors.

	// Where execution should continue when suspending.
	FOp* YieldPC;
	TWriteBarrier<VFrame> YieldFrame;
	TWriteBarrier<VTask> YieldTask;

	// First VFrame of this task (created with null CallerFrame by CallTask).
	// When walking VFrame::CallerFrame's we need this to determine where a task
	// boundary is via 'CurrentFrame == RootFrame' in which case we follow our tasks YieldFrame
	TWriteBarrier<VFrame> RootFrame;

	// Where execution should continue when complete.
	TWriteBarrier<VValue> Result;
	TWriteBarrier<VTask> LastAwait;
	TWriteBarrier<VTask> LastCancel;

	// Links for the containing LastCancel or LastAwait list.
	TWriteBarrier<VTask> PrevTask;
	TWriteBarrier<VTask> NextTask;

	// The task group associated with this task.
	// TODO(SOL-5927): Remove this field when moving away from content scope-based termination.
	TWriteBarrier<VTaskGroup> TaskGroup;

	AUTORTFM_DISABLE COREUOBJECT_API static void BindStruct(FAllocationContext Context, VClass& TaskClass);
	AUTORTFM_DISABLE COREUOBJECT_API static void BindStructTrivial(FAllocationContext Context);

	AUTORTFM_DISABLE static VTask& New(FAllocationContext Context, FOp* YieldPC, VFrame* YieldFrame, VTask* YieldTask, VTask* Parent, VFrame* InRootFrame = nullptr)
	{
		VEmergentType& TaskEmergentType = *EmergentType;
		uint64 NumIndexedFields = TaskEmergentType.Shape->NumIndexedFields;
		VTask& Result = *new (AllocateCell(Context, StaticCppClassInfo, NumIndexedFields)) VTask(Context, TaskEmergentType, YieldPC, YieldFrame, YieldTask, Parent, InRootFrame);
		if (FDebugger* Debugger = GetDebugger())
		{
			Debugger->AddTask(Context, Result);
		}
		return Result;
	}

	COREUOBJECT_API void AddToTaskGroup(FAllocationContext Context);
	COREUOBJECT_API bool RemoveFromTaskGroup(FAllocationContext Context);

	/**
	 * Terminates this task and all of its child sub-tasks. This immediately sets their phase to canceled.
	 * This is only safe if we are in the process of unwinding the stack, or the stack is already empty.
	 * Only a top-level task should be terminated. This is not done transactionally and cannot be rolled back.
	 */
	AUTORTFM_DISABLE COREUOBJECT_API void Terminate(FAllocationContext Context);

	/** Returns the total number of child tasks beneath this task (including children-of-children). */
	COREUOBJECT_API int64 GetNumChildrenRecursively() const;

	AUTORTFM_DISABLE COREUOBJECT_API FOpResult::EKind Resume(FRunningContext Context, VValue ResumeArgument);
	AUTORTFM_DISABLE COREUOBJECT_API FOpResult::EKind Unwind(FRunningContext Context);

	bool Active() const { return Phase < EPhase::CancelStarted && !Result; }
	bool Completed() const { return !!Result; }

	COREUOBJECT_API static FOpResult ActiveImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FOpResult CompletedImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FOpResult CancelingImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FOpResult CanceledImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FOpResult UnsettledImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FOpResult SettledImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FOpResult UninterruptedImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FOpResult InterruptedImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);

	AUTORTFM_DISABLE COREUOBJECT_API static FOpResult AwaitImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);
	AUTORTFM_DISABLE COREUOBJECT_API static FOpResult CancelImpl(FRunningContext Context, VValue Scope, VNativeProcedure::Args Arguments);

	AUTORTFM_DISABLE FOpResult Cancel(FRunningContext Context);

	AUTORTFM_DISABLE void SetPhaseTransactionally(EPhase NewPhase)
	{
		AutoRTFM::Assign(Phase, NewPhase);
	}

	AUTORTFM_DISABLE void SetResumePCTransactionally(FOp* NewResumePC)
	{
		AutoRTFM::Assign(ResumePC, NewResumePC);
	}

	AUTORTFM_DISABLE void SetYieldPCTransactionally(FOp* NewYieldPC)
	{
		AutoRTFM::Assign(YieldPC, NewYieldPC);
	}

	AUTORTFM_DISABLE bool RequestCancel(FRunningContext Context);
	AUTORTFM_DISABLE bool CancelChildren(FRunningContext Context);

	AUTORTFM_DISABLE void Suspend(FAccessContext Context)
	{
		AutoRTFM::Assign(bRunning, false);
	}

	AUTORTFM_DISABLE void Resume(FAccessContext Context)
	{
		AutoRTFM::Assign(bRunning, true);
	}

	void Park(FAllocationContext Context, TWriteBarrier<VTask>& LastTask)
	{
		V_DIE_IF(PrevTask || NextTask);
		if (LastTask)
		{
			PrevTask.SetTransactionally(Context, LastTask.Get());
			LastTask->NextTask.SetTransactionally(Context, this);
		}
		LastTask.SetTransactionally(Context, this);
	}

	void Unpark(FAllocationContext Context, TWriteBarrier<VTask>& LastTask)
	{
		if (LastTask.Get() == this)
		{
			V_DIE_IF(NextTask);
			LastTask.SetTransactionally(Context, PrevTask.Get());
		}
		if (PrevTask)
		{
			V_DIE_UNLESS(PrevTask->NextTask.Get() == this);
			PrevTask->NextTask.SetTransactionally(Context, NextTask.Get());
		}
		if (NextTask)
		{
			V_DIE_UNLESS(NextTask->PrevTask.Get() == this);
			NextTask->PrevTask.SetTransactionally(Context, PrevTask.Get());
		}
		PrevTask.ResetTransactionally();
		NextTask.ResetTransactionally();
	}

	template <typename FunctorType>
		requires(!TIsTFunction<std::decay_t<FunctorType>>::Value && std::is_invocable_r_v<void, std::decay_t<FunctorType>, FAllocationContext, VTask*>)
	AUTORTFM_DISABLE void Defer(FAllocationContext Context, AUTORTFM_IMPLICIT_ENABLE FunctorType&& Func)
	{
		VTaskNativeHook& NewDefers = VTaskNativeHook::New(Context, Forward<FunctorType>(Func));
		NewDefers.Next.SetTransactionally(Context, NativeDefers.Get());
		NativeDefers.SetTransactionally(Context, NewDefers);
	}

	template <typename FunctorType>
		requires(!TIsTFunction<std::decay_t<FunctorType>>::Value && std::is_invocable_r_v<void, std::decay_t<FunctorType>, FAllocationContext, VTask*>)
	AUTORTFM_DISABLE void DeferOpen(FAllocationContext Context, AUTORTFM_IMPLICIT_DISABLE FunctorType&& Func)
	{
		VTaskNativeHook& NewDefers = VTaskNativeHook::NewOpen(Context, Forward<FunctorType>(Func));
		NewDefers.Next.SetTransactionally(Context, NativeDefers.Get());
		NativeDefers.SetTransactionally(Context, NewDefers);
	}

	void ClearDefer()
	{
		NativeDefers.ResetTransactionally();
	}

	template <
		typename FunctorType
			UE_REQUIRES(
				!TIsTFunction<std::decay_t<FunctorType>>::Value && std::is_invocable_r_v<void, std::decay_t<FunctorType>, FAccessContext, VTask*>)>
	void Await(FRunningContext Context, AUTORTFM_IMPLICIT_ENABLE FunctorType&& Func)
	{
		Await(Context, VTaskNativeHook::New(Context, Func));
	}

	AUTORTFM_DISABLE COREUOBJECT_API void ExecNativeDefer(FAllocationContext);
	AUTORTFM_DISABLE COREUOBJECT_API void ExecNativeAwaits(FAllocationContext);

	struct FCallerSpec
	{
		FOp* PC;
		VFrame* Frame;
		VRestValue* ReturnSlot;
	};
	AUTORTFM_DISABLE COREUOBJECT_API static FCallerSpec MakeFrameForSpawn(FAllocationContext Context);

	AUTORTFM_DISABLE COREUOBJECT_API static void InitializeGlobals(FAllocationContext Context);

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth = 0);
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VTask*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

private:
	enum class EEndTaskProcedureRegister : uint32
	{
		TaskResult = FRegisterIndex::PARAMETER_START,
		__MAX
	};

	AUTORTFM_DISABLE COREUOBJECT_API VTask(FAllocationContext Context, VEmergentType& TaskEmergentType, FOp* YieldPC, VFrame* YieldFrame, VTask* YieldTask, VTask* Parent, VFrame* InRootFrame);

	AUTORTFM_DISABLE COREUOBJECT_API void Await(FAllocationContext Context, VTaskNativeHook& Hook);

	AUTORTFM_DISABLE COREUOBJECT_API void TerminateRecursively(FAllocationContext Context);
};

// A counting semaphore with room for a single waiting task. Used for structured concurrency.
struct VSemaphore : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	TWriteBarrier<VTask> Await;

	static VSemaphore& New(FAllocationContext Context)
	{
		return *new (Context.AllocateFastCell(sizeof(VSemaphore))) VSemaphore(Context);
	}

	AUTORTFM_DISABLE int32 IncrementCount(int32 Arg)
	{
		AutoRTFM::Assign(Count, Count + Arg);
		return Count;
	}

	AUTORTFM_DISABLE int32 DecrementCount(int32 Arg)
	{
		AutoRTFM::Assign(Count, Count - Arg);
		return Count;
	}

private:
	VSemaphore(FAllocationContext Context)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
	{
	}

	int32 Count{0};
};
} // namespace Verse

#endif
