// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VVMCell.h"
#include "VVMGlobalHeapPtr.h"
#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VVMOption.h"
#include "VVMType.h"

namespace Verse
{
enum class EValueStringFormat;

struct VFalse : VType
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VType);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	AUTORTFM_DISABLE static void InitializeGlobals(FAllocationContext Context);

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);

	static constexpr bool SerializeIdentity = false;
	AUTORTFM_DISABLE COREUOBJECT_API static void SerializeLayout(FAllocationContext Context, VFalse*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE COREUOBJECT_API void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

private:
	AUTORTFM_DISABLE VFalse(FAllocationContext Context)
		: VType(Context, &GlobalTrivialEmergentType.Get(Context))
	{
	}

	AUTORTFM_DISABLE static VFalse& New(FAllocationContext Context)
	{
		return *new (Context.AllocateFastCell(sizeof(VFalse))) VFalse(Context);
	}
};

COREUOBJECT_API extern TGlobalHeapPtr<VFalse> GlobalFalsePtr;
COREUOBJECT_API extern TGlobalHeapPtr<VOption> GlobalTruePtr;

// True is represented as VOption(VFalse)
inline VOption& GlobalTrue()
{
	return *GlobalTruePtr.Get();
}

inline VFalse& GlobalFalse()
{
	return *GlobalFalsePtr.Get();
}

} // namespace Verse
#endif // WITH_VERSE_VM
