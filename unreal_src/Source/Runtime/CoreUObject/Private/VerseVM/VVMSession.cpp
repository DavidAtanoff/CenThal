// Copyright Epic Games, Inc. All Rights Reserved.
#include "VerseVM/VVMSession.h"
#include "Async/UniqueLock.h"
#include "VerseVM/Inline/VVMMapInline.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMMap.h"
#include "VerseVM/VVMNeverDestroyed.h"
#include "VerseVM/VVMRef.h"

namespace Verse
{
#if WITH_VERSE_VM
FSession& FSession::GetSession()
{
	static TNeverDestroyed<FSession> Session;
	return Session.Get();
}
#else
TSharedPtr<FSession> FSession::Session(new FSession());
#endif

void FSession::ResetWeakMaps()
{
	SessionMapReferences.RemoveAllSwap([](auto&& Arg) { return !Arg.Value.IsValid(); });
	for (auto& MapReference : SessionMapReferences)
	{
		const FMapProperty* Property = MapReference.Key;
		UObject* Container = MapReference.Value.Get();
		check(Container);
		Property->DestroyValue_InContainer(Container);
		Property->InitializeValue_InContainer(Container);
	}
#if WITH_VERSE_VM
	FRunningContext Context = FRunningContextPromise();
	AutoRTFM::Open([this, Context] AUTORTFM_DISABLE {
		UE::TUniqueLock Lock(CensusMutex);
		for (TWeakBarrier<VRef>& MapReference : SessionMapVRefs)
		{
			if (VRef* Ref = MapReference.Get(Context))
			{
				// set to an empty map to 'reset'. There shouldn't be meaningful initializers for session vars at the module scope to worry about...
				Ref->Set(Context, VMapBase::New<VMutableMap>(Context));
			}
		}
	});
#endif
}

void FSession::AddSessionMap(const FMapProperty* Property, UObject* Container)
{
	SessionMapReferences.Emplace(Property, Container);
}

#if WITH_VERSE_VM
void FSession::AddSessionMap(FRunningContext Context, VRef& SessionMap)
{
	UE::TUniqueLock Lock(CensusMutex);
	SessionMapVRefs.Emplace(TWeakBarrier<VRef>(SessionMap));
}

void FSession::ConductCensus()
{
	UE::TUniqueLock Lock(CensusMutex);
	for (TArray<TWeakBarrier<VRef>>::TIterator Iter = SessionMapVRefs.CreateIterator(); Iter; ++Iter)
	{
		if (Iter->ClearWeakDuringCensus())
		{
			Iter.RemoveCurrent();
		}
	}
}
#endif
} // namespace Verse