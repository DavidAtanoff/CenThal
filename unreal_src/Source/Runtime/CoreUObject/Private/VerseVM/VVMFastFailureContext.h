// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMFrame.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{

struct VFailureContext;

struct VFastFailureContext : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	TWriteBarrier<VFastFailureContext> Parent;
	TWriteBarrier<VFrame> Frame;
	TWriteBarrier<VBatchedRefs> BatchedRefs;
	TWriteBarrier<VFailureContext> EnclosingFailureContext;

	uint32 Suspensions = 0;
	bool bFailed = false;

	FOp* ThenPC = nullptr;
	FOp* FailurePC = nullptr;
	FOp* DonePC = nullptr;

	// The incoming effect token here is subtle. We allow things like <reads> in a fast failure
	// context. They bump the effect token. If they suspend, they don't register with
	// the fast failure context. So it won't even block the then/else of the fast failure context
	// from running. The reason this is sound is we emit codegen in such a way that things like
	// the RefGet unification will never fail. This isn't elegant, but it works.
	// To have the then/else see a concrete effect token at *some point*, it means we are relying
	// on things like the RefGet in a fast failure context to eventually run, even if the fast failure
	// context has already failed. The reason they will continue to run today is because a RefGet
	// suspension won't register with a fast failure context, only certain bytecodes do. So when it's
	// leniently resumed, it doesn't ask if the fast failure context has already failed, it will just
	// unconditionally run. If the MaxVerse checker proved that something like a RefGet didn't
	// have to run for the program to be un-stuck, then this strategy wouldn't work, and we'd
	// need to think of a way to capture the effect token incoming to the actual start of the if
	// condition. We could probably do that by capturing that true incoming effect token as our
	// initial leniency indicator until leniency is actually encountered.
	// Given the above, this acts both as the incoming effect token to the then and the else.
	VRestValue IncomingEffectToken{0};
	// This is the effect token we unify with after running the then/else.
	VRestValue DoneEffectToken{0};

	VFastFailureContext(FAllocationContext Context)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
	{
	}

	bool DidExecuteEndFastFailureContext()
	{
		return !!DonePC;
	}

	static VFastFailureContext& New(FAllocationContext Context)
	{
		return *new (Context.AllocateFastCell(sizeof(VFastFailureContext))) VFastFailureContext(Context);
	}
};

} // namespace Verse
#endif // WITH_VERSE_VM
