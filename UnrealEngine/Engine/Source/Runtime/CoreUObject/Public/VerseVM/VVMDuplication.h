// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "VerseVM/VVMContext.h"

namespace Verse
{
struct VCell;

/// Utility for cloning VCells in the context of a UObject graph.
///
/// UObject graphs can be cloned by operations like duplication and reinstancing.
/// VCells can participate in these graphs, with arbitrary references in both directions.
///
/// This class holds VCell-related state shared across the whole cloning operation,
/// so that the shape of the reference graph can be reproduced correctly.
struct FDuplicationContext
{
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	/// Record a VCell's presence and dependency information in the old object graph.
	COREUOBJECT_API void WriteCellReference(VCell* Cell);
	/// Write out the contents of discovered VCells. May discover more UObject references.
	COREUOBJECT_API void WriteUnserializedCells(FAllocationContext Context, FArchive& Writer);

	/// Populate the new object graph's VCells. Call after the whole graph has been written.
	COREUOBJECT_API void CreateSerializedCells(FAllocationContext Context, FArchive& Reader);

	/// Remap a VCell reference to its equivalent in the new object graph.
	COREUOBJECT_API VCell* ReadCellReference(VCell* SourceCell);
	/// Read in the contents of the new object graph's VCells. Must be prepared to remap UObject references.
	COREUOBJECT_API void ReadSerializedCells(FAllocationContext Context, FArchive& Reader);

private:
	TArray<int32> TopologicalSortCells();
	void GatherCellDependencies(int32 SerializedIndex, TBitArray<>& Visited, TArray<int32>& OutOrder);

	struct FDuplicatedCell
	{
		VCell* DuplicatedCell = nullptr;
		int32 SerializedIndex = INDEX_NONE;
	};

	struct FSerializedCell
	{
		VCell* Cell;
		int64 LayoutOffset;
		int64 Offset;
		int32 ReferencesBegin;
		int32 ReferencesEnd;
	};

	/// Map of VCells in the old object graph to their equivalent in the new object graph.
	TMap<VCell*, FDuplicatedCell> DuplicatedCells;
	/// Pending stack of VCells that have been discovered but not yet written.
	TArray<VCell*> UnserializedCells;

	/// Index of serialized VCell contents and dependencies.
	TArray<FSerializedCell> SerializedCells;
	TArray<VCell*> ReferencedCells;
#endif
};

} // namespace Verse
