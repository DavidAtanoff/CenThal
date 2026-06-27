// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VVMArrayBase.h"
#include "VVMEmergentTypeCreator.h"
#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VVMType.h"
#include "VVMUniqueCreator.h"

namespace Verse
{

struct VInt;

struct VArray : VArrayBase
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VArrayBase);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	AUTORTFM_DISABLE static VArray& Concat(FRunningContext Context, VArrayBase& Lhs, VArrayBase& Rhs);

	AUTORTFM_DISABLE static VArray& New(FAllocationContext Context, uint32 NumValues, EArrayType ArrayType)
	{
		return *new (Context.AllocateFastCell(sizeof(VArray))) VArray(Context, NumValues, ArrayType);
	}

	AUTORTFM_DISABLE static VArray& New(FAllocationContext Context, std::initializer_list<VValue> InitList)
	{
		return *new (Context.AllocateFastCell(sizeof(VArray))) VArray(Context, InitList);
	}

	template <typename InitIndexFunc, typename = std::enable_if_t<std::is_same_v<VValue, std::invoke_result_t<InitIndexFunc, uint32>>>>
	AUTORTFM_DISABLE static VArray& New(FAllocationContext Context, uint32 NumValues, InitIndexFunc&& InitFunc)
	{
		return *new (Context.AllocateFastCell(sizeof(VArray))) VArray(Context, NumValues, InitFunc);
	}

	AUTORTFM_DISABLE static VArray& New(FAllocationContext Context, FUtf8StringView String)
	{
		return *new (Context.AllocateFastCell(sizeof(VArray))) VArray(Context, String, &GlobalTrivialEmergentType.Get(Context));
	}

	AUTORTFM_DISABLE static VArray& New(FAllocationContext Context)
	{
		return VArray::New(Context, 0, EArrayType::None);
	}

	static constexpr bool SerializeIdentity = false;
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VArray*& This, FStructuredArchiveVisitor& Visitor) { SerializeLayoutImpl<VArray>(Context, This, Visitor); }

private:
	friend struct VMutableArray;
	AUTORTFM_DISABLE VArray(FAllocationContext Context, uint32 InNumValues, EArrayType ArrayType)
		: VArrayBase(Context, InNumValues, InNumValues, ArrayType, &GlobalTrivialEmergentType.Get(Context)) {}

	AUTORTFM_DISABLE VArray(FAllocationContext Context, std::initializer_list<VValue> InitList)
		: VArrayBase(Context, InitList, &GlobalTrivialEmergentType.Get(Context)) {}

	template <typename InitIndexFunc, typename = std::enable_if_t<std::is_same_v<VValue, std::invoke_result_t<InitIndexFunc, uint32>>>>
	AUTORTFM_DISABLE VArray(FAllocationContext Context, uint32 InNumValues, InitIndexFunc&& InitFunc)
		: VArrayBase(Context, InNumValues, InitFunc, &GlobalTrivialEmergentType.Get(Context)) {}

protected:
	AUTORTFM_DISABLE VArray(FAllocationContext Context, FUtf8StringView String, VEmergentType* Type)
		: VArrayBase(Context, String, Type) {}
};

} // namespace Verse
#endif // WITH_VERSE_VM
