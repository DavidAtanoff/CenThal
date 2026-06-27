// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "CoreTypes.h"
#include "HAL/Platform.h"
#include "VVMContext.h"
#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMEnumerator.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMUniqueString.h"
#include "VerseVM/VVMValue.h"

namespace Verse
{

struct AUTORTFM_DISABLE VAccessor : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	static VAccessor& NewUninitialized(FAllocationContext Context, uint32 InNumAccessors)
	{
		FLayout Layout = CalcLayout(InNumAccessors);
		return *new (Context.AllocateFastCell(Layout.TotalSize)) VAccessor(Context, InNumAccessors);
	}

	static void SerializeLayout(FAllocationContext Context, VAccessor*& This, FStructuredArchiveVisitor& Visitor);
	COREUOBJECT_API void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

	// `NumParams` -1 and -2 for getters and setters respectively to align them to trailing array positions
	TWriteBarrier<VUniqueString>& FindGetter(uint32 NumParams)
	{
		return GetGettersBegin()[NumParams - 1];
	}
	TWriteBarrier<VUniqueString>& FindSetter(uint32 NumParams)
	{
		return GetSettersBegin()[NumParams - 2];
	}
	void AddGetter(FAllocationContext Context, uint32 NumParams, VUniqueString& Name)
	{
		FindGetter(NumParams).Set(Context, Name);
	}
	void AddSetter(FAllocationContext Context, uint32 NumParams, VUniqueString& Name)
	{
		FindSetter(NumParams).Set(Context, Name);
	}

private:
	TWriteBarrier<VUniqueString>* GetGettersBegin() { return BitCast<TWriteBarrier<VUniqueString>*>(BitCast<std::byte*>(this) + GetLayout().GettersOffset); }
	TWriteBarrier<VUniqueString>* GetGettersEnd() { return GetGettersBegin() + NumAccessors; }

	TWriteBarrier<VUniqueString>* GetSettersBegin() { return BitCast<TWriteBarrier<VUniqueString>*>(BitCast<std::byte*>(this) + GetLayout().SettersOffset); }
	TWriteBarrier<VUniqueString>* GetSettersEnd() { return GetSettersBegin() + NumAccessors; }

	uint32 NumAccessors;
	TWriteBarrier<VCell> Trailing[];

	struct FLayout
	{
		int32 GettersOffset;
		int32 SettersOffset;
		int32 TotalSize;
	};

	static FLayout CalcLayout(uint32 NumAccessors)
	{
		FStructBuilder StructBuilder;
		StructBuilder.AddMember(offsetof(VAccessor, Trailing), alignof(VAccessor));

		FLayout Layout;
		Layout.GettersOffset = StructBuilder.AddMember(sizeof(TWriteBarrier<VUniqueString>) * NumAccessors, alignof(TWriteBarrier<VUniqueString>));
		Layout.SettersOffset = StructBuilder.AddMember(sizeof(TWriteBarrier<VUniqueString>) * NumAccessors, alignof(TWriteBarrier<VUniqueString>));
		Layout.TotalSize = StructBuilder.GetSize();
		return Layout;
	}

	FLayout GetLayout() const
	{
		return CalcLayout(NumAccessors);
	}

	VAccessor(FAllocationContext Context, uint32 InNumAccessors)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
		, NumAccessors(InNumAccessors)
	{
		for (TWriteBarrier<VUniqueString>* Getter = GetGettersBegin(); Getter != GetGettersEnd(); ++Getter)
		{
			new (Getter) TWriteBarrier<VUniqueString>{};
		}
		for (TWriteBarrier<VUniqueString>* Setter = GetSettersBegin(); Setter != GetSettersEnd(); ++Setter)
		{
			new (Setter) TWriteBarrier<VUniqueString>{};
		}
	}
};

struct AUTORTFM_DISABLE VAccessorRef : VHeapValue
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VHeapValue);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	COREUOBJECT_API static TGlobalHeapPtr<VEnumerator> AccessorEnum;

	// The source this ref was projected from. Either an object or another VAccessorRef.
	TWriteBarrier<VValue> Base;

	// The member projected from Base. Either a VAccessor or an argument to the getter/setter.
	TWriteBarrier<VValue> Member;

	VAccessorRef& Flatten(FAllocationContext Context, TArray<TWriteBarrier<VValue>>& OutArguments, int32 Depth);

	static VAccessorRef& New(FAllocationContext Context, VValue Base, VAccessor& Member)
	{
		return *new (Context.AllocateFastCell(sizeof(VAccessorRef))) VAccessorRef(Context, Base, Member);
	}

	static VAccessorRef& New(FAllocationContext Context, VAccessorRef& Base, VValue Member)
	{
		return *new (Context.AllocateFastCell(sizeof(VAccessorRef))) VAccessorRef(Context, Base, Member);
	}

private:
	VAccessorRef(FAllocationContext Context, VValue InBase, VValue InMember)
		: VHeapValue(Context, &GlobalTrivialEmergentType.Get(Context))
		, Base(Context, InBase)
		, Member(Context, InMember)
	{
	}
};

// A postponed setter call. Used when verse.UObjectLeniency is disabled.
struct AUTORTFM_DISABLE VAccessorSuspension : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	TWriteBarrier<VAccessorSuspension> Next;
	TWriteBarrier<VAccessorSuspension> Prev;

	TWriteBarrier<VAccessorRef> Ref;
	TWriteBarrier<VValue> Value;

	static VAccessorSuspension& New(FAllocationContext Context, VAccessorRef& Ref, VValue Value)
	{
		return *new (Context.AllocateFastCell(sizeof(VAccessorSuspension))) VAccessorSuspension(Context, Ref, Value);
	}

	void Append(FAllocationContext Context, VAccessorSuspension& Other)
	{
		Prev->Next.Set(Context, Other);
		Other.Prev.Set(Context, *Prev);
		Other.Next.Set(Context, *this);
		Prev.Set(Context, Other);
	}

private:
	VAccessorSuspension(FAllocationContext Context, VAccessorRef& InRef, VValue InValue)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
		, Next(Context, this)
		, Prev(Context, this)
		, Ref(Context, InRef)
		, Value(Context, InValue)
	{
	}
};

} // namespace Verse
#endif // WITH_VERSE_VM
