// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMWriteBarrierInline.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMArrayBase.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMStructuredArchiveVisitor.h"
#include "VerseVM/VVMTransaction.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{
template <EWriteMode WriteMode>
void VBuffer::SetVValue(FAllocationContext Context, uint32 Index, VValue Value)
{
	checkSlow(GetArrayType() == EArrayType::VValue);
	if (IsTransactional<WriteMode>())
	{
		GetData<TWriteBarrier<VValue>>()[Index].SetTransactionally(Context, Value);
	}
	else
	{
		new (&GetData<TWriteBarrier<VValue>>()[Index]) TWriteBarrier<VValue>(Context, Value);
	}
}

inline bool VArrayBase::IsInBounds(uint32 Index) const
{
	return Index < Num();
}

inline bool VArrayBase::IsInBounds(const VInt& Index, const uint32 Bounds) const
{
	if (Index.IsInt64())
	{
		const int64 IndexInt64 = Index.AsInt64();
		return (IndexInt64 >= 0) && (IndexInt64 < Bounds);
	}
	else
	{
		// Array maximum size is limited to the maximum size of a unsigned 32-bit integer.
		// So even if it's a `VHeapInt`, if it fails the `IsInt64` check, it is definitely out-of-bounds.
		return false;
	}
}

inline VValue VArrayBase::GetValue(uint32 Index)
{
	checkSlow(IsInBounds(Index));
	switch (GetArrayType())
	{
		case EArrayType::VValue:
			return GetData<VValue>()[Index].Follow();
		case EArrayType::Int32:
			return VValue::FromInt32(GetData<int32>()[Index]);
		case EArrayType::Char8:
			return VValue::Char(GetData<UTF8CHAR>()[Index]);
		case EArrayType::Char32:
			return VValue::Char32(GetData<UTF32CHAR>()[Index]);
		case EArrayType::None:
		default:
			V_DIE("Unhandled EArrayType (%u) encountered!", static_cast<uint32>(GetArrayType()));
	}
}

inline const VValue VArrayBase::GetValue(uint32 Index) const
{
	checkSlow(IsInBounds(Index));
	switch (GetArrayType())
	{
		case EArrayType::VValue:
			return GetData<VValue>()[Index].Follow();
		case EArrayType::Int32:
			return VValue::FromInt32(GetData<int32>()[Index]);
		case EArrayType::Char8:
			return VValue::Char(GetData<UTF8CHAR>()[Index]);
		case EArrayType::Char32:
			return VValue::Char32(GetData<UTF32CHAR>()[Index]);
		case EArrayType::None:
		default:
			V_DIE("Unhandled EArrayType (%u) encountered!", static_cast<uint32>(GetArrayType()));
	}
}

template <EWriteMode WriteMode /* = EWriteMode::Default */>
inline void VArrayBase::ConvertDataToVValues(FAllocationContext Context, uint32 NewCapacity)
{
	if (GetArrayType() != EArrayType::VValue)
	{
		uint32 Num = this->Num();
		VBuffer NewBuffer = VBuffer(Context, Num, NewCapacity, EArrayType::VValue);
		for (uint32 Index = 0; Index < Num; ++Index)
		{
			new (&NewBuffer.GetData<TWriteBarrier<VValue>>()[Index]) TWriteBarrier<VValue>(Context, GetValue(Index));
		}

		// If an append is rolled back, the buffer must remain legible to the GC.
		FMemory::Memzero(NewBuffer.GetData<VValue>() + Num, (NewCapacity - Num) * Verse::ByteLength(EArrayType::VValue));

		// We need to see the store to ArrayType/Num/all the VValues before the GC
		// sees the buffer itself.
		SetBufferWithStoreBarrier<WriteMode>(Context, NewBuffer);
	}
}

template <EWriteMode WriteMode /* = EWriteMode::NonTransactional */>
inline void VArrayBase::SetValue(FAllocationContext Context, uint32 Index, VValue Value)
{
	static_assert(WriteMode != EWriteMode::Default, "Use EWriteMode::NonTransactional or EWriteMode::Transactional");
	checkSlow(Index < Capacity());
	EArrayType ArrayType = GetArrayType();
	if (ArrayType == EArrayType::VValue)
	{
		SetVValue<WriteMode>(Context, Index, Value);
	}
	else if (ArrayType != DetermineArrayType(Value))
	{
		ConvertDataToVValues<WriteMode>(Context, Capacity());
		SetVValue<WriteMode>(Context, Index, Value);
	}
	else
	{
		switch (ArrayType)
		{
			case EArrayType::Int32:
				SetInt32<WriteMode>(Index, Value.AsInt32());
				break;
			case EArrayType::Char8:
				SetChar<WriteMode>(Index, Value.AsChar());
				break;
			case EArrayType::Char32:
				SetChar32<WriteMode>(Index, Value.AsChar32());
				break;
			case EArrayType::VValue:
			case EArrayType::None:
			default:
				V_DIE("Unhandled EArrayType (%u) encountered!", static_cast<uint32>(ArrayType));
		}
	}
}

inline void VArrayBase::SetValue(FAllocationContext Context, uint32 Index, VValue Value)
{
	SetValue<EWriteMode::NonTransactional>(Context, Index, Value);
}

inline void VArrayBase::SetValueTransactionally(FAllocationContext Context, uint32 Index, VValue Value)
{
	SetValue<EWriteMode::Transactional>(Context, Index, Value);
}

template <typename TArray>
void VArrayBase::SerializeLayoutImpl(FAllocationContext Context, TArray*& This, FStructuredArchiveVisitor& Visitor)
{
	if (Visitor.IsLoading())
	{
		This = &TArray::New(Context);
	}
}

} // namespace Verse
#endif // WITH_VERSE_VM
