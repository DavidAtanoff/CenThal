// Copyright Epic Games, Inc. All Rights Reserved.

#include "VerseVM/VVMDuplication.h"
#include "VerseVM/Inline/VVMCellInline.h"
#include "VerseVM/VVMObject.h"
#include "VerseVM/VVMRef.h"

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
namespace Verse
{

void FDuplicationContext::WriteCellReference(VCell* Cell)
{
	if (Cell && Cell->GetCppClassInfo()->InstancedCell)
	{
		// Functions only need to be duplicated if their Self is going to be duplicated.
		if (VFunction* Function = Cell->DynamicCast<VFunction>())
		{
			VCell* Self = Function->Self.Get().ExtractCell();
			if (!Self || !Self->GetCppClassInfo()->InstancedCell)
			{
				return;
			}
		}

		if (!DuplicatedCells.Contains(Cell))
		{
			DuplicatedCells.Add(Cell, FDuplicatedCell());
			UnserializedCells.Add(Cell);
		}

		ReferencedCells.Add(Cell);
	}
}

void FDuplicationContext::WriteUnserializedCells(FAllocationContext Context, FArchive& Ar)
{
	while (UnserializedCells.Num())
	{
		AutoRTFM::UnreachableIfClosed("VerseVM does not support serialization in the closed");

		VCell* Cell = UnserializedCells.Pop();

		int32 ReferencesBegin = ReferencedCells.Num();
		const VCppClassInfo* CppClassInfo = Cell->GetCppClassInfo();
		int64 LayoutOffset = Ar.Tell();
		VCell::SerializeLayout(Context, CppClassInfo, Cell, Ar);
		int64 CellOffset = Ar.Tell();
		Cell->Serialize(Context, Ar);
		int32 ReferencesEnd = ReferencedCells.Num();

		int32 SerializedIndex = SerializedCells.Emplace(Cell, LayoutOffset, CellOffset, ReferencesBegin, ReferencesEnd);
		DuplicatedCells[Cell].SerializedIndex = SerializedIndex;
	}
}

void FDuplicationContext::CreateSerializedCells(FAllocationContext Context, FArchive& Ar)
{
	for (const FSerializedCell& Entry : SerializedCells)
	{
		AutoRTFM::UnreachableIfClosed("VerseVM does not support serialization in the closed");

		VCell* SerializedCell = Entry.Cell;
		Ar.Seek(Entry.LayoutOffset);

		VCell*& DuplicatedCell = DuplicatedCells[SerializedCell].DuplicatedCell;
		VCell::SerializeLayout(Context, SerializedCell->GetCppClassInfo(), DuplicatedCell, Ar);
	}
}

VCell* FDuplicationContext::ReadCellReference(VCell* SourceCell)
{
	if (FDuplicatedCell* CellInfo = DuplicatedCells.Find(SourceCell))
	{
		return CellInfo->DuplicatedCell;
	}
	return SourceCell;
}

void FDuplicationContext::ReadSerializedCells(FAllocationContext Context, FArchive& Ar)
{
	TArray<int32> SerializedCellOrder = TopologicalSortCells();
	for (int32 Index : SerializedCellOrder)
	{
		AutoRTFM::UnreachableIfClosed("VerseVM does not support serialization in the closed");
		const FSerializedCell& Entry = SerializedCells[Index];
		VCell* SerializedCell = Entry.Cell;
		Ar.Seek(Entry.Offset);

		VCell* DuplicatedCell = DuplicatedCells[SerializedCell].DuplicatedCell;
		DuplicatedCell->Serialize(Context, Ar);
	}
}

// Compute the order in which to read duplicated VCells back out of the buffer.
// Some VCells must be serialized before others- primarily VMap keys before their containing VMap.
// This function approximates these dependencies using a postorder traversal of cells that are compared by value.
TArray<int32> FDuplicationContext::TopologicalSortCells()
{
	TArray<int32> Order;
	Order.Reserve(SerializedCells.Num());

	TBitArray<> Visited(false, SerializedCells.Num());
	for (int32 Index = 0; Index < SerializedCells.Num(); ++Index)
	{
		GatherCellDependencies(Index, Visited, Order);
	}

	return Order;
}

// TODO: Can this be shared with the SerializationBeforeSerialization logic in SavePreloadDependencies?
void FDuplicationContext::GatherCellDependencies(int32 SerializedIndex, TBitArray<>& Visited, TArray<int32>& OutOrder)
{
	if (!Visited[SerializedIndex])
	{
		Visited[SerializedIndex] = true;

		const FSerializedCell& Entry = SerializedCells[SerializedIndex];
		for (int32 Index = Entry.ReferencesBegin; Index < Entry.ReferencesEnd; ++Index)
		{
			VCell* ReferencedCell = ReferencedCells[Index];

			bool bComparedByValue = true;
			if (VObject* Object = ReferencedCell->DynamicCast<VObject>())
			{
				bComparedByValue = Object->IsStruct();
			}
			else if (VRef* Ref = ReferencedCell->DynamicCast<VRef>())
			{
				bComparedByValue = false;
			}
			if (bComparedByValue)
			{
				int32 ReferencedIndex = DuplicatedCells[ReferencedCell].SerializedIndex;
				GatherCellDependencies(ReferencedIndex, Visited, OutOrder);
			}
		}

		OutOrder.Add(SerializedIndex);
	}
}

} // namespace Verse
#endif
