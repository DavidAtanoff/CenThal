// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "CoreTypes.h"
#include "Misc/AssertionMacros.h"
#include "VVMContext.h"
#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VVMType.h"
#include "VVMValue.h"

namespace Verse
{
enum class EValueStringFormat;

// Inherits from VType because true, which is option{false}, is a type.
struct VOption : VType
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VType);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	AUTORTFM_DISABLE static VOption& New(FAllocationContext Context, VValue InValue)
	{
		return *new (Context.AllocateFastCell(sizeof(VOption))) VOption(Context, InValue);
	}

	VValue GetValue() const
	{
		return Value.Get().Follow();
	}

	void SetValue(FAllocationContext Context, VValue InValue)
	{
		check(!InValue.IsUninitialized());
		Value.Set(Context, InValue);
	}

	COREUOBJECT_API uint32 GetTypeHashImpl();
	AUTORTFM_DISABLE VValue MeltImpl(FAllocationContext Context);
	AUTORTFM_DISABLE FOpResult FreezeImpl(FAllocationContext Context, VTask* Task);
	void VisitMembersImpl(FAllocationContext Context, FDebuggerVisitor& Visitor);
	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);
	AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSONImpl(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs);

	static constexpr bool SerializeIdentity = false;
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VOption*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

	static constexpr bool InstancedCell = true;

private:
	AUTORTFM_DISABLE VOption(FAllocationContext Context, VValue InValue)
		: VType(Context, &GlobalTrivialEmergentType.Get(Context))
		, Value(Context, InValue)
	{
		SetIsDeeplyMutable();
	}

	TWriteBarrier<VValue> Value;
};

} // namespace Verse
#endif // WITH_VERSE_VM
