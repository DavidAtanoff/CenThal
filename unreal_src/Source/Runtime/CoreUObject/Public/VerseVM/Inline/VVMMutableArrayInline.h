// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/Inline/VVMArrayBaseInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{

template <EWriteMode WriteMode /* = EWriteMode::NonTransactional */>
inline void VMutableArray::AddValue(FAllocationContext Context, VValue Value)
{
	static_assert(WriteMode != EWriteMode::Default, "Use EWriteMode::NonTransactional or EWriteMode::Transactional");
	if (!GetData())
	{
		uint32 Num = 0;
		uint32 Capacity = 4;
		EArrayType ArrayType = DetermineArrayType(Value);
		VBuffer NewBuffer = VBuffer(Context, Num, Capacity, ArrayType);
		if (ArrayType == EArrayType::VValue)
		{
			// If an append is rolled back, the buffer must remain legible to the GC.
			FMemory::Memzero(NewBuffer.GetData(), Capacity * Verse::ByteLength(EArrayType::VValue));
		}
		SetBufferWithStoreBarrier<WriteMode>(Context, NewBuffer);
	}
	else if (GetArrayType() != EArrayType::VValue && GetArrayType() != DetermineArrayType(Value))
	{
		uint32 Capacity = this->Capacity();
		if (Num() == Capacity) // Check our capacity before re-allocating as VValues
		{
			Capacity = Capacity * 2;
		}
		ConvertDataToVValues<WriteMode>(Context, Capacity);
	}
	else if (Num() == Capacity())
	{
		uint32 NewCapacity = Capacity() * 2;
		VBuffer NewBuffer = VBuffer(Context, Num(), NewCapacity, GetArrayType());
		if (Num())
		{
			FMemory::Memcpy(NewBuffer.GetData(), GetData(), ByteLength());
		}

		if (GetArrayType() == EArrayType::VValue)
		{
			// If an append is rolled back, the buffer must remain legible to the GC.
			uint32 AdditionalCapacity = NewCapacity - Num();
			FMemory::Memzero(NewBuffer.GetData<VValue>() + Num(), AdditionalCapacity * Verse::ByteLength(EArrayType::VValue));
		}

		// We might be copying around a VValue buffer, so we gotta barrier before we expose this buffer to the GC.
		SetBufferWithStoreBarrier<WriteMode>(Context, NewBuffer);
	}

	uint32 Index = Num();
	V_DIE_UNLESS(Index < Capacity());
	SetValue<WriteMode>(Context, Index, Value);
	// The above store needs to happen before the GC sees an updated NumValues.
	StoreStoreFence();
	uint32& NumValues = Buffer.Get().GetHeader()->NumValues;
	if (IsTransactional<WriteMode>())
	{
		AutoRTFM::Assign(NumValues, NumValues + 1);
	}
	else
	{
		++NumValues;
	}
	if (::Verse::IsNullTerminatedString(GetArrayType()))
	{
		SetNullTerminator<WriteMode>();
	}
}

template <typename T>
inline void VMutableArray::Append(FAllocationContext Context, VArrayBase& Array)
{
	checkSlow(GetArrayType() != EArrayType::None && GetArrayType() != EArrayType::VValue && GetArrayType() == Array.GetArrayType());
	if (Array.Num())
	{
		const uint32 NewNumValues = Num() + Array.Num();
		uint32 Capacity = this->Capacity();
		if (NewNumValues > Capacity)
		{
			Capacity = NewNumValues * 2;
			VBuffer NewBuffer = VBuffer(Context, Num(), Capacity, GetArrayType());
			if (Num())
			{
				FMemory::Memcpy(NewBuffer.GetData(), GetData(), ByteLength());
			}
			// We need the store of the array type in the buffer to happen
			// before the GC sees the new buffer.
			SetBufferWithStoreBarrier(Context, NewBuffer);
		}
		FMemory::Memcpy(GetData<T>() + Num(), Array.GetData<T>(), Array.ByteLength());
		// We don't need to barrier here because the GC doesn't mark primitive arrays.
		Buffer.Get().GetHeader()->NumValues = NewNumValues;
		if (::Verse::IsNullTerminatedString(GetArrayType()))
		{
			SetNullTerminator();
		}
	}
}

template <>
inline void VMutableArray::Append<TWriteBarrier<VValue>>(FAllocationContext Context, VArrayBase& Array)
{
	checkSlow(GetArrayType() == EArrayType::VValue);
	for (uint32 Index = 0, End = Array.Num(); Index < End; ++Index)
	{
		AddValue(Context, Array.GetValue(Index));
	}
}

inline VMutableArray& VMutableArray::Concat(FAllocationContext Context, VArrayBase& Lhs, VArrayBase& Rhs)
{
	VMutableArray& NewArray = VMutableArray::New(Context, 0, Lhs.Num() + Rhs.Num(), DetermineCombinedType(Lhs.GetArrayType(), Rhs.GetArrayType()));
	NewArray.Append(Context, Lhs);
	NewArray.Append(Context, Rhs);
	return NewArray;
}

inline void VMutableArray::InPlaceMakeImmutable(FAllocationContext Context)
{
	static_assert(std::is_base_of_v<VArrayBase, VArray>);
	static_assert(sizeof(VArray) == sizeof(VArrayBase));
	SetEmergentType(Context, &VArray::GlobalTrivialEmergentType.Get(Context));
}
} // namespace Verse
#endif // WITH_VERSE_VM
