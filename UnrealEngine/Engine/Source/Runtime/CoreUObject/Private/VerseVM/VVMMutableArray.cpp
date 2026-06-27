// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMMutableArray.h"

#include "VerseVM/Inline/VVMAbstractVisitorInline.h"
#include "VerseVM/Inline/VVMArrayBaseInline.h"
#include "VerseVM/Inline/VVMCellInline.h"
#include "VerseVM/Inline/VVMMarkStackVisitorInline.h"
#include "VerseVM/Inline/VVMMutableArrayInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/VVMCppClassInfo.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{
DEFINE_DERIVED_VCPPCLASSINFO(VMutableArray);
DEFINE_TRIVIAL_VISIT_REFERENCES(VMutableArray);
TGlobalTrivialEmergentTypePtr<&VMutableArray::StaticCppClassInfo> VMutableArray::GlobalTrivialEmergentType;

void VMutableArray::Reset(FAllocationContext Context)
{
	SetBufferWithStoreBarrier(Context, VBuffer());
}

void VMutableArray::ResetTransactionally(FAllocationContext Context)
{
	SetBufferWithStoreBarrier<EWriteMode::Transactional>(Context, VBuffer());
}

void VMutableArray::CopyRange(FAccessContext Context, uint32 SrcIndex, uint32 DstIndex, uint32 NumElements)
{
	// CopyRange only implements a forwards copy, so ranges are only allowed to overlap if SrcIndex >= DstIndex.
	// When this was written, the API surface of VMutableArray could never cause a backwards copy to occur.
	// (If you hit this DIE, just add a backwards copy loop to the VValue case below.)
	V_DIE_UNLESS(SrcIndex >= DstIndex || SrcIndex + NumElements <= DstIndex);

	const EArrayType ArrayType = GetArrayType();
	switch (ArrayType)
	{
		case EArrayType::VValue:
		{
			TWriteBarrier<VValue>* Data = GetData<TWriteBarrier<VValue>>();
			TWriteBarrier<VValue>* Dst = Data + DstIndex;
			TWriteBarrier<VValue>* Src = Data + SrcIndex;
			for (uint32 Index = 0; Index < NumElements; ++Index)
			{
				Dst[Index].Set<EWriteMode::NonTransactional>(Context, Src[Index].Get());
			}
			break;
		}
		case EArrayType::Int32:
		case EArrayType::Char8:
		case EArrayType::Char32:
		case EArrayType::None:
		default:
		{
			// All other types aren't of interest to GC, so just move them as we please.
			uint8* Data = GetData<uint8>();
			uint32 ElementSize = ::Verse::ByteLength(ArrayType);
			uint8* Dst = Data + DstIndex * ElementSize;
			uint8* Src = Data + SrcIndex * ElementSize;
			FMemory::Memmove(Dst, Src, NumElements * ElementSize);
			break;
		}
	}
}

void VMutableArray::Truncate(uint32 ElementsToRemove)
{
	// No fence is needed since it doesn't matter if GC sees the old or new state
	Buffer.Get().GetHeader()->NumValues -= ElementsToRemove;

	// TODO: reclaim memory if the buffer is mostly empty.
}

void VMutableArray::RemoveRange(FAccessContext Context, uint32 StartIndex, uint32 RemoveCount)
{
	checkSlow(Num() - StartIndex >= RemoveCount);

	uint32 Remainder = Num() - StartIndex - RemoveCount;
	if (Remainder > 0)
	{
		// Copy the remainder of the array to close the gap
		CopyRange(Context, StartIndex + RemoveCount, StartIndex, Remainder);
	}

	// Truncate the final elements of the array.
	Truncate(RemoveCount);
}

void VMutableArray::RemoveSwap(FAccessContext Context, uint32 RemoveIndex)
{
	uint32 Length = Num();
	checkSlow(RemoveIndex < Length);

	if (RemoveIndex < Length - 1)
	{
		// Copy the last element so that it overwrites our target element.
		CopyRange(Context, Length - 1, RemoveIndex, 1);
	}

	// Truncate the last element in the array.
	Truncate(1);
}

void VMutableArray::Append(FAllocationContext Context, VArrayBase& Array)
{
	if (!Buffer && Array.Num())
	{
		uint32 Num = 0;
		uint32 Capacity = Array.Num();
		VBuffer NewBuffer = VBuffer(Context, Num, Capacity, Array.GetArrayType());
		// We barrier because the GC needs to see the store to ArrayType/Num if
		// it sees the new buffer.
		SetBufferWithStoreBarrier(Context, NewBuffer);
	}
	else if (GetArrayType() != EArrayType::VValue && GetArrayType() != Array.GetArrayType())
	{
		ConvertDataToVValues(Context, Num() + Array.Num());
	}

	switch (GetArrayType())
	{
		case EArrayType::None:
			V_DIE_UNLESS(Array.GetArrayType() == EArrayType::None);
			// Empty-Untyped VMutableArray appending Empty-Untyped VMutableArray
			break;
		case EArrayType::VValue:
			Append<TWriteBarrier<VValue>>(Context, Array);
			break;
		case EArrayType::Int32:
			Append<int32>(Context, Array);
			break;
		case EArrayType::Char8:
			Append<UTF8CHAR>(Context, Array);
			break;
		case EArrayType::Char32:
			Append<UTF32CHAR>(Context, Array);
			break;
		default:
			V_DIE("Unhandled EArrayType encountered!");
	}
}

FOpResult VMutableArray::FreezeImpl(FAllocationContext Context, VTask* Task)
{
	EArrayType ArrayType = GetArrayType();
	VArray* FrozenArray = nullptr;

	if ((!Task || !Task->AwaitPC) && ArrayType != EArrayType::VValue)
	{
		// For simple arrays holding ints or chars, we can use memcpy to freeze everything at once.
		FrozenArray = &VArray::New(Context, Num(), ArrayType);
		if (Num())
		{
			FMemory::Memcpy(FrozenArray->GetData(), GetData(), ByteLength());
		}
	}
	else
	{
		// For arrays of VValues, we need to individually unwrap, freeze and clone each element.
		FrozenArray = &VArray::New(Context, Num(), [&](uint32 Index) -> VValue {
			VValue Element = UnwrapTransparentRef(Context, GetValue(Index), Task, [&](VValue Element) {
				AUTORTFM_SANITIZER_DISABLE_SCOPE();
				SetValue(Context, Index, Element);
			});
			FOpResult Result = VValue::Freeze(Context, Element, Task);
			V_DIE_UNLESS(Result.IsReturn()); // Values inside a Verse-native array should always be valid.
			return Result.Value;
		});
	}

	V_RETURN(*FrozenArray);
}

} // namespace Verse

#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
