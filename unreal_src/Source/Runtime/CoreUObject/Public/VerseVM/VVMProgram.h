// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VVMCell.h"
#include "VVMPackage.h"
#include "VVMTupleType.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMIntrinsics.h"
#include "VerseVM/VVMNameValueMap.h"

namespace Verse
{
struct VIntrinsics;
struct VMapBase;

struct VProgram : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	uint32 NumPackages() const { return PackageMap.Num(); }
	const VUniqueString& GetPackageName(uint32 Index) const { return PackageMap.GetName(Index); }
	VPackage& GetPackage(uint32 Index) const { return PackageMap.GetCell<VPackage>(Index); }
	AUTORTFM_DISABLE COREUOBJECT_API void AddPackage(FAllocationContext Context, VUniqueString& Name, VPackage& Package);
	COREUOBJECT_API void RemovePackage(FAccessContext, FUtf8StringView VersePackageName);
	VPackage* LookupPackage(FUtf8StringView VersePackageName) const { return PackageMap.LookupCell<VPackage>(VersePackageName); }
	VPackage* LookupPackageByFName(FName VersePackageName) const { return PackageMap.LookupCellByFName<VPackage>(VersePackageName); }

	// Will overwrite existing entry if exists
	AUTORTFM_DISABLE COREUOBJECT_API void AddTupleType(FAllocationContext Context, VUniqueString& MangledName, VTupleType& TupleType);
	COREUOBJECT_API VTupleType* LookupTupleType(FAccessContext Context, VUniqueString& MangledName) const;

	// Will overwrite existing entry if exists
	AUTORTFM_DISABLE COREUOBJECT_API void AddImport(FAllocationContext Context, VNamedType& TypeWithImport);
	COREUOBJECT_API VNamedType* LookupImport(FAllocationContext Context, UField* ImportedType) const;
	COREUOBJECT_API VShape* LookupShape(FAllocationContext Context, UField* ImportedType) const;
	VPackage* LookupPackage(FAllocationContext Context, UPackage* Package);

	VIntrinsics& GetIntrinsics() const { return *Intrinsics.Get(); }
	void AddIntrinsics(FAllocationContext Context, VIntrinsics& InIntrinsics) { Intrinsics.Set(Context, InIntrinsics); }

	VFunction& GetUpdatePersistentWeakMapPlayer() const { return *UpdatePersistentWeakMapPlayer.Get(); }
	void AddUpdatePersistentWeakMapPlayer(FAllocationContext Context, VFunction& InUpdatePersistentWeakMapPlayer) { UpdatePersistentWeakMapPlayer.Set(Context, InUpdatePersistentWeakMapPlayer); }

	COREUOBJECT_API void Reset(FAllocationContext Context);

	AUTORTFM_DISABLE static VProgram& New(FAllocationContext Context, uint32 Capacity)
	{
		return *new (Context.Allocate(FHeap::DestructorSpace, sizeof(VProgram))) VProgram(Context, Capacity);
	}

private:
	AUTORTFM_DISABLE VProgram(FAllocationContext Context, uint32 Capacity)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
		, PackageMap(Context, Capacity)
	{
	}

	VNameValueMap PackageMap;
	TWriteBarrier<VWeakCellMap> TupleTypeMap;
	TMap<TObjectPtr<UField>, VValue> ImportMap; // For reverse lookup UField -> VNamedType
	TMap<TObjectPtr<UField>, VValue> ShapeMap;  // For reverse lookup UStruct -> VShape
	TWriteBarrier<VIntrinsics> Intrinsics;
	TWriteBarrier<VFunction> UpdatePersistentWeakMapPlayer;
};
} // namespace Verse
#endif // WITH_VERSE_VM
