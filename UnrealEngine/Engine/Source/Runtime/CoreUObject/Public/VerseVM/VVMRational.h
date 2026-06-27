// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "CoreTypes.h"
#include "HAL/Platform.h"
#include "Misc/AssertionMacros.h"
#include "VVMContext.h"
#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VVMInt.h"
#include "VVMValue.h"

namespace Verse
{
enum class EValueStringFormat;

struct VRational : VHeapValue
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VHeapValue);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	TWriteBarrier<VInt> Numerator;
	TWriteBarrier<VInt> Denominator;

	AUTORTFM_DISABLE COREUOBJECT_API static VRational& Add(FAllocationContext Context, VRational& Lhs, VRational& Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static VRational& Sub(FAllocationContext Context, VRational& Lhs, VRational& Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static VRational& Mul(FAllocationContext Context, VRational& Lhs, VRational& Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static VRational& Div(FAllocationContext Context, VRational& Lhs, VRational& Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static VRational& Neg(FAllocationContext Context, VRational& N);
	AUTORTFM_DISABLE COREUOBJECT_API static bool Eq(FAllocationContext Context, VRational& Lhs, VRational& Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static bool Eq(FAllocationContext Context, VRational& Lhs, VInt Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static bool Gt(FAllocationContext Context, VRational& Lhs, VRational& Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static bool Lt(FAllocationContext Context, VRational& Lhs, VRational& Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static bool Gte(FAllocationContext Context, VRational& Lhs, VRational& Rhs);
	AUTORTFM_DISABLE COREUOBJECT_API static bool Lte(FAllocationContext Context, VRational& Lhs, VRational& Rhs);

	AUTORTFM_DISABLE COREUOBJECT_API VInt Floor(FAllocationContext Context) const;
	AUTORTFM_DISABLE COREUOBJECT_API VInt Ceil(FAllocationContext Context) const;

	bool IsZero() const { return Numerator.Get().IsZero(); }

	AUTORTFM_DISABLE static VRational& New(FAllocationContext Context, VInt InNumerator, VInt InDenominator)
	{
		return *new (Context.AllocateFastCell(sizeof(VRational))) VRational(Context, InNumerator, InDenominator);
	}

	AUTORTFM_DISABLE COREUOBJECT_API ECompares EqualImpl(FAllocationContext Context, VCell* Other, const TFunction<void(::Verse::VValue, ::Verse::VValue)>& HandlePlaceholder);

	COREUOBJECT_API uint32 GetTypeHashImpl();

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);

	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VRational*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE COREUOBJECT_API void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

private:
	AUTORTFM_DISABLE COREUOBJECT_API VRational(FAllocationContext Context, VInt InNumerator, VInt InDenominator);
};

} // namespace Verse
#endif // WITH_VERSE_VM
