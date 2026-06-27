// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "UObject/VerseValueProperty.h"
#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{

struct AUTORTFM_DISABLE VNativeRef : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	enum class EType : uint8
	{
		FProperty,
		Index,
	};

	// The source this ref was projected from. Either a UObject, a VNativeStruct, or another VNativeRef.
	TWriteBarrier<VValue> Base;

	// The time this ref was projected from Base. A ref may have been invalidated if Base.Epoch < Context.GetWriteEpoch().
	uint64 Epoch;

	union
	{
		FProperty* UProperty;
		int32 Index;
	};

	EType Type;

	TPair<void*, FProperty*> GetData(FAllocationContext Context);

	FOpResult Call(FAllocationContext Context, VValue Argument, bool bSet);
	FOpResult LoadField(FAllocationContext Context, VUniqueString& FieldName);

	COREUOBJECT_API FOpResult Get(FAllocationContext Context);

	/**
	 * Returns FOpResult::Return on success, with a freshly-allocated, VValue-based copy of the property data in the result's Value.
	 * If an error occurs while accessing the native field, generates a runtime error and then returns FOpResult::Error.
	 */
	COREUOBJECT_API static FOpResult Get(FAllocationContext Context, const void* Container, FProperty* Property);

	/**
	 * Returns a freshly-allocated, VValue-based copy of the property data in the result's Value.
	 * If an error occurs while accessing the native field, returns an uninitialized VValue.
	 */
	COREUOBJECT_API static VValue Peek(FAllocationContext Context, const void* Container, FProperty* Property);

	/**
	 * Returns FOpResult::Return on success, with a freshly-allocated, VValue-based copy of the struct in the result's Value.
	 * If an error occurs while accessing the struct, generates a runtime error and then returns FOpResult::Error.
	 */
	COREUOBJECT_API static FOpResult GetStruct(FAllocationContext Context, const void* Data, UScriptStruct* Struct);

	/**
	 * Returns FOpResult::Return on success, with a freshly-allocated, VValue-based copy of the struct in the result's Value.
	 * If an error occurs while accessing the native field, returns an uninitialized VValue.
	 */
	COREUOBJECT_API static VValue PeekStruct(FAllocationContext Context, const void* Data, UScriptStruct* Struct);

	COREUOBJECT_API FOpResult Set(FAllocationContext Context, VValue Value);

	COREUOBJECT_API FOpResult SetNonTransactionally(FAllocationContext Context, VValue Value);

	template <EWriteMode WriteMode, typename BaseType>
	COREUOBJECT_API static FOpResult Set(FAllocationContext Context, BaseType Base, void* Container, FProperty* Property, VValue Value);

	static VNativeRef& New(FAllocationContext Context, UObject* Base, FProperty* Property)
	{
		return *new (Context.AllocateFastCell(sizeof(VNativeRef))) VNativeRef(Context, Base, Property);
	}

	static VNativeRef& New(FAllocationContext Context, VNativeStruct* Base, FProperty* Property)
	{
		return *new (Context.AllocateFastCell(sizeof(VNativeRef))) VNativeRef(Context, *Base, Property);
	}

	static VNativeRef& New(FAllocationContext Context, VNativeRef* Base, FProperty* Property)
	{
		return *new (Context.AllocateFastCell(sizeof(VNativeRef))) VNativeRef(Context, *Base, Property);
	}

	static VNativeRef& New(FAllocationContext Context, VNativeRef* Base, int32 Index)
	{
		return *new (Context.AllocateFastCell(sizeof(VNativeRef))) VNativeRef(Context, *Base, Index);
	}

	COREUOBJECT_API FOpResult FreezeImpl(FAllocationContext Context, VTask* Task);

private:
	VNativeRef(FAllocationContext Context, VValue InBase, FProperty* InProperty)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
		, Base(Context, InBase)
		, Epoch(Context.GetWriteEpoch())
		, UProperty(InProperty)
		, Type(EType::FProperty)
	{
		SetIsDeeplyMutable();
	}

	VNativeRef(FAllocationContext Context, VValue InBase, int32 InIndex)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
		, Base(Context, InBase)
		, Epoch(Context.GetWriteEpoch())
		, Index(InIndex)
		, Type(EType::Index)
	{
		SetIsDeeplyMutable();
	}
};

extern template FOpResult VNativeRef::Set<EWriteMode::Transactional>(FAllocationContext Context, UObject* Base, void* Container, FProperty* Property, VValue Value);
extern template FOpResult VNativeRef::Set<EWriteMode::Transactional>(FAllocationContext Context, VNativeStruct* Base, void* Container, FProperty* Property, VValue Value);
extern template FOpResult VNativeRef::Set<EWriteMode::NonTransactional>(FAllocationContext Context, std::nullptr_t Base, void* Container, FProperty* Property, VValue Value);

} // namespace Verse
#endif
