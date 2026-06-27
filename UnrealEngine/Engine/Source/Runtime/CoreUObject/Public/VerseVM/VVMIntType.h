// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Containers/StringFwd.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMType.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{
enum class EValueStringFormat;

// An int type with constraints. A uninitialized min/max means no constraint.
struct VIntType : VType
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VType);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	AUTORTFM_DISABLE static VIntType& New(FAllocationContext Context, VInt InMin, VInt InMax)
	{
		return *new (Context.AllocateFastCell(sizeof(VIntType))) VIntType(Context, InMin, InMax);
	}

	const VInt GetMin() const
	{
		return Min.Get();
	}

	const VInt GetMax() const
	{
		return Max.Get();
	}

	AUTORTFM_DISABLE bool SubsumesImpl(FAllocationContext Context, VValue);

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);
	AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSONImpl(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs);
	AUTORTFM_DISABLE COREUOBJECT_API VValue FromJSONImpl(FRunningContext, const FJsonValue&, EValueJSONFormat, FFromJsonCallback);

	static constexpr bool SerializeIdentity = false;
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VIntType*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext, FStructuredArchiveVisitor& Visitor);

private:
	explicit VIntType(FAllocationContext& Context, VInt InMin, VInt InMax)
		: VType(Context, &GlobalTrivialEmergentType.Get(Context))
		, Min(Context, InMin)
		, Max(Context, InMax)
	{
	}

	TWriteBarrier<VInt> Min;
	TWriteBarrier<VInt> Max;
};

} // namespace Verse
#endif // WITH_VERSE_VM
