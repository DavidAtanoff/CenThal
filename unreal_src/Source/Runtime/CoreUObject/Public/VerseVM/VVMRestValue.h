// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VVMCell.h"
#include "VVMValue.h"
#include "VVMWriteBarrier.h"

namespace Verse
{
struct FTrail;
struct VFrame;

/// A Verse value "at rest" in a local variable, object field, array element, etc.
///
/// Logically these locations hold unification variables, which are initialized to unbound placeholders.
/// Most of these placeholders are immediately unified with a concrete value before they are used, so we
/// want to avoid the cost of allocating them in the first place.
///
/// Similarly, the VM generally follows placeholders when reading them from these "at rest" locations,
/// rather than proliferating redundant calls to Follow everywhere that handles VValues.
///
/// VRestValue thus represents either a fresh unbound placeholder encoded as a VValue::Root, which it will
/// convert into an allocated VPlaceholder on-demand, or some other VValue, which it will follow on-demand.
struct VRestValue
{
	VRestValue(const VRestValue&) = default;
	VRestValue& operator=(const VRestValue&) = default;

	explicit constexpr VRestValue(uint16 SplitDepth)
	{
		Reset(SplitDepth);
	}

	VRestValue(FAccessContext Context, VValue Value)
		: Value(Context, Value)
	{
	}

	constexpr void Reset(uint16 SplitDepth)
	{
		SetNonCellNorPlaceholder(VValue::Root(SplitDepth));
	}

	void ResetTransactionally(uint16 SplitDepth)
	{
		SetNonCellNorPlaceholderTransactionally(VValue::Root(SplitDepth));
	}

	void ResetTrailed(FAllocationContext Context, FTrail& Trail, uint16 SplitDepth)
	{
		SetNonCellNorPlaceholderTrailed(Context, Trail, VValue::Root(SplitDepth));
	}

	template <EWriteMode WriteMode>
	AUTORTFM_DISABLE void Set(FAccessContext Context, VValue NewValue)
	{
		if constexpr (WriteMode == EWriteMode::Transactional)
		{
			SetTransactionally(Context, NewValue);
		}
		else
		{
			Set(Context, NewValue);
		}
	}

	void Set(FAccessContext Context, VValue NewValue)
	{
		checkSlow(!NewValue.IsRoot());
		Value.Set(Context, NewValue);
	}

	void SetTransactionally(FAccessContext, VValue);

	void SetTrailed(FAllocationContext, FTrail&, VValue);

	constexpr void SetNonCellNorPlaceholder(VValue NewValue)
	{
		Value.SetNonCellNorPlaceholder(NewValue);
	}

	void SetNonCellNorPlaceholderTransactionally(VValue NewValue)
	{
		Value.SetNonCellNorPlaceholderTransactionally(NewValue);
	}

	void SetNonCellNorPlaceholderTrailed(FAllocationContext Context, FTrail& Trail, VValue NewValue)
	{
		Value.SetNonCellNorPlaceholderTrailed(Context, Trail, NewValue);
	}

	bool IsRoot() const
	{
		return Value.Get().IsRoot();
	}

	bool CanDefQuickly() const
	{
		return IsRoot();
	}

	VValue Get(FAllocationContext Context);

	VValue GetRaw()
	{
		return Value.Get();
	}

	AUTORTFM_DISABLE COREUOBJECT_API VValue GetSlow(FAllocationContext Context);

	VValue GetTransactionally(FAllocationContext Context);

	AUTORTFM_DISABLE COREUOBJECT_API VValue GetSlowTransactionally(FAllocationContext Context);

	bool IsUninitialized() const
	{
		return Value.Get().IsUninitialized();
	}

	bool operator==(const VRestValue& Other) const;

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToString(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth = 0) const;
	COREUOBJECT_API FUtf8String ToString(FAllocationContext Context, EValueStringFormat Format, uint32 RecursionDepth = 0) const;
	AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSON(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth = 0, FJsonObject* Defs = nullptr) const;

	template <typename TVisitor>
	friend void Visit(TVisitor& Visitor, VRestValue& InValue)
	{
		Visitor.Visit(InValue.Value, TEXT(""));
	}

	friend uint32 GetTypeHash(VRestValue RestValue);

private:
	VRestValue() = default;
	TWriteBarrier<VValue> Value;

	friend struct VArray;
	friend struct VFrame;
	friend struct VObject;
	friend struct VValue;
	friend struct VValueObject;
	friend struct FStructuredArchiveVisitor;
	friend ::FReferenceCollector;
};
} // namespace Verse

inline void FReferenceCollector::AddReferencedVerseValue(Verse::VRestValue& InValue, const UObject* ReferencingObject, const FProperty* ReferencingProperty)
{
	AddReferencedVerseValue(InValue.Value, ReferencingObject, ReferencingProperty);
}
#endif // WITH_VERSE_VM
