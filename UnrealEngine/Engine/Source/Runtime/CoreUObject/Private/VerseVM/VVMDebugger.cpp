// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMDebugger.h"
#include "VerseVM/Inline/VVMContextImplInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/VVMFalse.h"
#include "VerseVM/VVMFrame.h"
#include "VerseVM/VVMLog.h"
#include "VerseVM/VVMNativeProcedure.h"

static Verse::FDebugger* GDebugger = nullptr;

Verse::FDebugger* Verse::GetDebugger()
{
	return GDebugger;
}

void Verse::SetDebugger(FDebugger* Arg)
{
	StoreStoreFence();
	GDebugger = Arg;
}

namespace Verse
{

static bool IsFalse(VValue Arg)
{
	return Arg.IsCell() && &Arg.AsCell() == GlobalFalsePtr.Get();
}

void Debugger::ForEachStackFrame(
	FRunningContext Context,
	const FOp* InPC,
	VFrame* InFrame,
	VTask* InTask,
	const FNativeFrame* NativeFrame,
	TFunctionRef<void(const FLocation*, FFrame)> Callback)
{
	checkf(NativeFrame, TEXT("ForEachStackFrame requires a non-null NativeFrame"));

	// The PC, Frame and Task must travel together--all three must be either populated or null.
	check(!!InPC == !!InFrame && !!InPC == !!InTask);

	TWriteBarrier<VUniqueString> SelfName{Context, VUniqueString::New(Context, "Self")};

	auto OnVFrame = [Context, Callback, &SelfName](const FOp* CurrentOp, VFrame* Frame) {
		VUniqueString& FilePath = *Frame->Procedure->FilePath;
		if (FilePath.Num() == 0)
		{
			return;
		}
		FRegisters Registers;
		VValue SelfValue = Frame->Registers[FRegisterIndex::SELF].Get(Context);
		V_DIE_IF_MSG(
			SelfValue.IsUninitialized(),
			"`Self` should have been bound by now for methods, and set to `GlobalFalse()` for functions. "
			"This indicates either a codegen issue, or a failure in `CallWithSelf`!");
		if (IsFalse(SelfValue))
		{
			Registers.Reserve(Frame->Procedure->NumRegisterNames);
		}
		else
		{
			Registers.Reserve(Frame->Procedure->NumRegisterNames + 1);
			Registers.Emplace(SelfName, Frame->Registers[FRegisterIndex::SELF].Get(Context));
		}

		uint32 BytecodeOffset = Frame->Procedure->BytecodeOffset(*CurrentOp);
		for (FRegisterName *I = Frame->Procedure->GetRegisterNamesBegin(), *Last = Frame->Procedure->GetRegisterNamesEnd(); I != Last; ++I)
		{
			VValue RegisterValue;
			if (I->LiveRange.Contains(BytecodeOffset))
			{
				RegisterValue = Frame->Registers[I->Index.Index].Get(Context);
			}
			Registers.Emplace(I->Name, RegisterValue);
		}

		Callback(
			Frame->Procedure->GetLocation(*CurrentOp),
			FFrame{Context, *Frame->Procedure->Name, FilePath, ::MoveTemp(Registers)});
	};

	auto OnNativeFrame = [Context, Callback](const FNativeFrame* NF) {
		if (const VNativeProcedure* Callee = NF->Callee)
		{
			Callback(nullptr, FFrame{Context, *Callee->Name});
		}
	};

	NativeFrame->WalkCallstackFrames(InPC, InFrame, InTask, OnVFrame, OnNativeFrame);
}
} // namespace Verse

#endif
