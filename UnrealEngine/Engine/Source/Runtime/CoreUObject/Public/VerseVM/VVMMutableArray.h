// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VVMArrayBase.h"
#include "VVMEmergentTypeCreator.h"
#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VVMWriteBarrier.h"

namespace Verse
{
struct FOp;
struct FOpResult;
struct VTask;

struct VMutableArray : VArrayBase
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VArrayBase);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

public:
	COREUOBJECT_API void Reset(FAllocationContext Context);

	void ResetTransactionally(FAllocationContext Context);

	/** Adds `Value` at the end of the array. */
	AUTORTFM_DISABLE void AddValue(FAllocationContext Context, VValue Value)
	{
		AddValue<EWriteMode::NonTransactional>(Context, Value);
	}

	AUTORTFM_DISABLE void AddValueTransactionally(FAllocationContext Context, VValue Value)
	{
		AddValue<EWriteMode::Transactional>(Context, Value);
	}

	template <EWriteMode WriteMode = EWriteMode::NonTransactional>
	AUTORTFM_DISABLE void AddValue(FAllocationContext, VValue);

	/** Removes `Count` array elements, starting from `StartIndex`, keeping all other array elements in order. */
	COREUOBJECT_API void RemoveRange(FAccessContext, uint32 StartIndex, uint32 Count);

	/** Eliminates the array element at `RemoveIndex`, and moves the last array element into its place. */
	COREUOBJECT_API void RemoveSwap(FAccessContext, uint32 RemoveIndex);

	template <typename T>
	AUTORTFM_DISABLE void Append(FAllocationContext Context, VArrayBase& Array);
	AUTORTFM_DISABLE void Append(FAllocationContext Context, VArrayBase& Array);

	static VMutableArray& Concat(FAllocationContext Context, VArrayBase& Lhs, VArrayBase& Rhs);

	void InPlaceMakeImmutable(FAllocationContext);

	AUTORTFM_DISABLE static VMutableArray& New(FAllocationContext Context, uint32 NumValues, uint32 InitialCapacity, EArrayType ArrayType)
	{
		return *new (Context.AllocateFastCell(sizeof(VMutableArray))) VMutableArray(Context, NumValues, InitialCapacity, ArrayType);
	}

	AUTORTFM_DISABLE static VMutableArray& New(FAllocationContext Context, std::initializer_list<VValue> InitList)
	{
		return *new (Context.AllocateFastCell(sizeof(VMutableArray))) VMutableArray(Context, InitList);
	}

	template <typename InitIndexFunc, typename = std::enable_if_t<std::is_same_v<VValue, std::invoke_result_t<InitIndexFunc, uint32>>>>
	AUTORTFM_DISABLE static VMutableArray& New(FAllocationContext Context, uint32 InNumValues, InitIndexFunc&& InitFunc)
	{
		return *new (Context.AllocateFastCell(sizeof(VMutableArray))) VMutableArray(Context, InNumValues, InitFunc);
	}

	AUTORTFM_DISABLE static VMutableArray& New(FAllocationContext Context, FUtf8StringView String)
	{
		return *new (Context.AllocateFastCell(sizeof(VMutableArray))) VMutableArray(Context, String);
	}

	AUTORTFM_DISABLE static VMutableArray& New(FAllocationContext Context)
	{
		return VMutableArray::New(Context, 0, 0, EArrayType::None);
	}

	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VMutableArray*& This, FStructuredArchiveVisitor& Visitor) { SerializeLayoutImpl<VMutableArray>(Context, This, Visitor); }

	AUTORTFM_DISABLE COREUOBJECT_API FOpResult FreezeImpl(FAllocationContext Context, VTask* Task);

	/** Discards the last `ElementsToRemove` elements from the array's backing buffer; does not reclaim memory. */
	void Truncate(uint32 ElementsToRemove);

private:
	AUTORTFM_DISABLE VMutableArray(FAllocationContext Context, uint32 NumValues, uint32 InitialCapacity, EArrayType ArrayType)
		: VArrayBase(Context, NumValues, InitialCapacity, ArrayType, &GlobalTrivialEmergentType.Get(Context))
	{
		V_DIE_UNLESS(InitialCapacity >= NumValues);
	}

	AUTORTFM_DISABLE VMutableArray(FAllocationContext Context, std::initializer_list<VValue> InitList)
		: VArrayBase(Context, InitList, &GlobalTrivialEmergentType.Get(Context))
	{
	}

	template <typename InitIndexFunc, typename = std::enable_if_t<std::is_same_v<VValue, std::invoke_result_t<InitIndexFunc, uint32>>>>
	AUTORTFM_DISABLE VMutableArray(FAllocationContext Context, uint32 InNumValues, InitIndexFunc&& InitFunc)
		: VArrayBase(Context, InNumValues, InitFunc, &GlobalTrivialEmergentType.Get(Context))
	{
	}

	AUTORTFM_DISABLE VMutableArray(FAllocationContext Context, FUtf8StringView String)
		: VArrayBase(Context, String, &GlobalTrivialEmergentType.Get(Context))
	{
	}

	/** Copies `Count` array elements from `SrcIndex` to `DstIndex`. */
	void CopyRange(FAccessContext, uint32 SrcIndex, uint32 DstIndex, uint32 Count);
};

} // namespace Verse
#endif // WITH_VERSE_VM
