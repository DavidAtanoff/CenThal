// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "UObject/UnrealType.h"
#include "VerseVM/VVMGlobalHeapCensusRoot.h"
#include "VerseVM/VVMWeakBarrier.h"

namespace Verse
{

#if WITH_VERSE_VM
struct FRunningContext;
struct VMutableMap;
#endif

/*
This 'session' class exists to clear weak_map session vars inbetween fortnite rounds.

Why --> References to devices, widgets, characters, entities, components (literally anything scene graph)
etc are all likely to become 'invalid' in a sense between rounds when a world is reloaded (BPVM / UE).
Thus, we can't reference them safely from weak_maps unless we make them _all_ invalidatable/disposable
which we have not done yet as it would make the APIs quite cumbersome.

So, instead we clear these maps inbetween rounds to avoid accessing bad data like SOL-4914 ran into.
*/
class FSession
#if WITH_VERSE_VM
	: public FGlobalHeapCensusRoot
#else
	: public TSharedFromThis<FSession>
#endif
{
public:
	FSession() = default;

	FSession(const FSession&) = delete;

	FSession(FSession&&) = delete;

	FSession& operator=(const FSession&) = delete;

	FSession& operator=(FSession&&) = delete;

	~FSession() = default;

	COREUOBJECT_API void ResetWeakMaps();

	COREUOBJECT_API void AddSessionMap(const FMapProperty*, UObject* Container);

#if WITH_VERSE_VM
	COREUOBJECT_API static FSession& GetSession();

	COREUOBJECT_API void AddSessionMap(FRunningContext Context, VRef& SessionMap);

	COREUOBJECT_API void ConductCensus() override;

private:
	UE::FMutex CensusMutex;
	TArray<TWeakBarrier<VRef>> SessionMapVRefs;
#else
	static FSession& GetSession()
	{
		return *Session.Get();
	}

private:
	COREUOBJECT_API static TSharedPtr<FSession> Session;
#endif

private:
	TArray<TPair<const FMapProperty*, FWeakObjectPtr>> SessionMapReferences;
};
} // namespace Verse
