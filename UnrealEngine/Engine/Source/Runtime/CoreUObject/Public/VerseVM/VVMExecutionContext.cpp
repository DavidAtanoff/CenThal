// Copyright Epic Games, Inc. All Rights Reserved.

#include "VerseVM/VVMExecutionContext.h"
#include "UObject/UObjectThreadContext.h"

namespace verse
{
#if WITH_VERSE_BPVM
thread_local TUniquePtr<FExecutionContextImpl> CurrentImpl;

FExecutionContextImpl* FExecutionContextImpl::Claim()
{
	FExecutionContextImpl* Result = CurrentImpl.Get();
	if (!Result)
	{
		Result = new FExecutionContextImpl;
		CurrentImpl.Reset(Result);
	}

	ensureMsgf(!Result->bActive, TEXT("Creating a new Verse execution context when one is already active!"));
	Result->bActive = true;

	return Result;
}

void FExecutionContextImpl::Release()
{
	ensure(bActive);
	ensure(CurrentImpl.Get() == this);
	bActive = false;
}

FExecutionContextImpl* FExecutionContextImpl::GetCurrent()
{
	return CurrentImpl.Get();
}
#endif

COREUOBJECT_API bool FExecutionContext::bBlockAllExecution = false;

#if WITH_VERSE_BPVM
thread_local bool bInGameThreadScope = false;

FEnsureGameThreadScope::FEnsureGameThreadScope()
	: bOriginalValue(bInGameThreadScope)
{
	bInGameThreadScope = true;
	if (!bOriginalValue)
	{
		// Checking for the async loading thread catches times where the game thread helps with loading work.
		// These scopes are reentrant, and disable IsRoutingPostLoad, so we only do this check at the top level.
		ensure(IsInGameThread() && (!IsInAsyncLoadingThread() || FUObjectThreadContext::Get().IsRoutingPostLoad));
	}
}

FEnsureGameThreadScope::~FEnsureGameThreadScope()
{
	bInGameThreadScope = bOriginalValue;
}
#endif
} // namespace verse
