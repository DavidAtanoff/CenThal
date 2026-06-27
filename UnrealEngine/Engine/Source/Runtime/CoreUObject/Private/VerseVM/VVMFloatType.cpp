// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMFloatType.h"
#include "VerseVM/Inline/VVMCellInline.h"
#include "VerseVM/VVMAbstractVisitor.h"
#include "VerseVM/VVMFloatPrinting.h"
#include "VerseVM/VVMJson.h"
#include "VerseVM/VVMStructuredArchiveVisitor.h"
#include "VerseVM/VVMValuePrinting.h"

namespace Verse
{
DEFINE_DERIVED_VCPPCLASSINFO(VFloatType);
DEFINE_TRIVIAL_VISIT_REFERENCES(VFloatType);
TGlobalTrivialEmergentTypePtr<&VFloatType::StaticCppClassInfo> VFloatType::GlobalTrivialEmergentType;

static bool HasUnconstrainedMin(const VFloatType& Type)
{
	return Type.GetMin() == -VFloat::Infinity();
}

static bool Subsumes(const VFloatType& Type, VFloat Value)
{
	if (Value.IsNaN())
	{
		return HasUnconstrainedMin(Type) && Type.GetMax().IsNaN();
	}
	return Type.GetMin() <= Value && (Type.GetMax().IsNaN() || Type.GetMax() >= Value);
}

bool VFloatType::SubsumesImpl(FAllocationContext Context, VValue Value)
{
	if (!Value.IsFloat())
	{
		return false;
	}
	return Verse::Subsumes(*this, Value.AsFloat());
}

void VFloatType::AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth)
{
	const bool bUnconstrainedMin = HasUnconstrainedMin(*this);
	// If there is a lower bound, then NaN is already excluded, and we can treat NaN and +Infinity as equivalently unconstrained upper bounds.
	const bool bUnconstrainedMax = GetMax().IsNaN() || (GetMax().IsInfinite() && !bUnconstrainedMin);
	if (bUnconstrainedMin && bUnconstrainedMax)
	{
		Builder << UTF8TEXT("float");
	}
	else if (bUnconstrainedMin)
	{
		Builder << UTF8TEXT("type{:float<=");
		AppendDecimalToString(Builder, GetMax());
		Builder << UTF8TEXT('}');
	}
	else if (bUnconstrainedMax)
	{
		Builder << UTF8TEXT("type{:float>=");
		AppendDecimalToString(Builder, GetMin());
		Builder << UTF8TEXT('}');
	}
	else if (GetMin() == GetMax())
	{
		Builder << UTF8TEXT("type{");
		AppendDecimalToString(Builder, GetMin());
		Builder << UTF8TEXT('}');
	}
	else
	{
		Builder << UTF8TEXT("type{:float>=");
		AppendDecimalToString(Builder, GetMin());
		Builder << UTF8TEXT("<=");
		AppendDecimalToString(Builder, GetMax());
		Builder << UTF8TEXT('}');
	}
}

TSharedPtr<FJsonValue> VFloatType::ToJSONImpl(FRunningContext Context, EValueJSONFormat Format, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback Callback, uint32 RecursionDepth, FJsonObject* Defs)
{
	const bool bUnconstrainedMin = GetMin() == -VFloat::Infinity();
	// If there is a lower bound, then NaN is already excluded, and we can treat NaN and +Infinity as equivalently unconstrained upper bounds.
	const bool bUnconstrainedMax = GetMax().IsNaN() || (GetMax().IsInfinite() && !bUnconstrainedMin);
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(JSON_FIELD(Type), Persona::NumberString);
	if (!bUnconstrainedMin)
	{
		Object->SetField(JSON_FIELD(Minimum), MakeShared<FJsonValueNumber>(GetMin().AsDouble()));
	}
	if (!bUnconstrainedMax)
	{
		Object->SetField(JSON_FIELD(Maximum), MakeShared<FJsonValueNumber>(GetMax().AsDouble()));
	}
	return MakeShared<FJsonValueObject>(Object);
}

VValue VFloatType::FromJSONImpl(FRunningContext, const FJsonValue& JsonValue, EValueJSONFormat, FFromJsonCallback)
{
	double DoubleValue;
	if (!JsonValue.TryGetNumber(DoubleValue))
	{
		return {};
	}
	VFloat Result{DoubleValue};
	if (!Verse::Subsumes(*this, Result))
	{
		return {};
	}
	return Result;
}

void VFloatType::SerializeLayout(FAllocationContext Context, VFloatType*& This, FStructuredArchiveVisitor& Visitor)
{
	if (Visitor.IsLoading())
	{
		This = &VFloatType::New(Context, VFloat(), VFloat());
	}
}

void VFloatType::SerializeImpl(FAllocationContext, FStructuredArchiveVisitor& Visitor)
{
	double ScratchMin = Min.AsDouble();
	double ScratchMax = Max.AsDouble();
	Visitor.Visit(ScratchMin, TEXT("Min"));
	Visitor.Visit(ScratchMax, TEXT("Max"));
	if (Visitor.IsLoading())
	{
		Min = VFloat(ScratchMin);
		Max = VFloat(ScratchMax);
	}
}

} // namespace Verse

#endif