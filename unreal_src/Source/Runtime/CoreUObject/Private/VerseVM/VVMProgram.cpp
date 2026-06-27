// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "VerseVM/VVMProgram.h"
#include "VerseVM/Inline/VVMMarkStackVisitorInline.h"
#include "VerseVM/Inline/VVMPackageInline.h"
#include "VerseVM/VVMCppClassInfo.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMIntrinsics.h"
#include "VerseVM/VVMVerseClass.h"

namespace Verse
{

DEFINE_DERIVED_VCPPCLASSINFO(VProgram);
TGlobalTrivialEmergentTypePtr<&VProgram::StaticCppClassInfo> VProgram::GlobalTrivialEmergentType;

void VProgram::AddPackage(FAllocationContext Context, VUniqueString& Name, VPackage& Package)
{
	PackageMap.AddValue(Context, Name, VValue(Package));

	// Register tuple types used by the added package
	Package.ForEachUsedTupleType([Context, this](VTupleType* UsedTupleType) {
		AddTupleType(Context, UsedTupleType->GetMangledName(), *UsedTupleType);
	});

	// Register imports used by the added package
	Package.ForEachUsedImport([Context, this](VNamedType* TypeWithImport) {
		AddImport(Context, *TypeWithImport); // Will overwrite existing entry if exists
	});
}

void VProgram::RemovePackage(FAccessContext Context, FUtf8StringView VersePackageName)
{
	TOptional<VValue> RemovedValue = PackageMap.RemoveValue(Context, VersePackageName);
	if (RemovedValue)
	{
		VPackage& RemovedPackage = RemovedValue->StaticCast<VPackage>();
		RemovedPackage.ResetRedirects();

		// Note: The TupleTypeMap will weed out now unused tuple types during the next GC census
		// TODO(SOL-8667): The import and shape maps should be a weak maps so we don't have to iterate through every import and remove them.
		RemovedPackage.ForEachUsedImport([this](VNamedType* TypeWithImport) {
			UField* ImportedType = TypeWithImport->GetUETypeChecked<UField>();
			ImportMap.Remove(ImportedType);
			ShapeMap.Remove(ImportedType);
		});
	}
}

void VProgram::AddTupleType(FAllocationContext Context, VUniqueString& MangledName, VTupleType& TupleType)
{
	if (!TupleTypeMap)
	{
		TupleTypeMap.Set(Context, VWeakCellMap::New(Context));
	}
	TupleTypeMap->Add(Context, &MangledName, &TupleType);
}

VTupleType* VProgram::LookupTupleType(FAccessContext Context, VUniqueString& MangledName) const
{
	if (!TupleTypeMap)
	{
		return nullptr;
	}

	return static_cast<VTupleType*>(TupleTypeMap->Find(Context, &MangledName));
}

template <typename TVisitor>
void VProgram::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(PackageMap, TEXT("PackageMap"));
	Visitor.Visit(TupleTypeMap, TEXT("TupleTypeMap"));
	Visitor.Visit(ImportMap.begin(), ImportMap.end(), TEXT("ImportMap"));
	Visitor.Visit(ShapeMap.begin(), ShapeMap.end(), TEXT("ShapeMap"));
	Visitor.Visit(Intrinsics, TEXT("Intrinsics"));
	Visitor.Visit(UpdatePersistentWeakMapPlayer, TEXT("UpdatePersistentWeakMapPlayer"));
}

void VProgram::Reset(FAllocationContext Context)
{
	PackageMap.Reset(Context);
	TupleTypeMap.Reset();
	ImportMap.Empty();
	ShapeMap.Empty();
}

void VProgram::AddImport(FAllocationContext Context, VNamedType& TypeWithImport)
{
	UField* ImportedType = TypeWithImport.GetUETypeChecked<UField>();
	if (UStruct* Struct = Cast<UStruct>(ImportedType); Struct && !Cast<UVerseClass>(ImportedType))
	{
		VClass& Class = TypeWithImport.StaticCast<VClass>();
		VShape* StructShape = Class.CreateShapeForExistingUStruct(Context);
		ShapeMap.Add(ImportedType, VValue(*StructShape));
	}

	ImportMap.Add(ImportedType, VValue(TypeWithImport));
}

VNamedType* VProgram::LookupImport(FAllocationContext Context, UField* ImportedType) const
{
	const VValue* FoundImport = ImportMap.Find(ImportedType);
	if (!FoundImport)
	{
		return nullptr;
	}

	return &FoundImport->StaticCast<VNamedType>();
}

VShape* VProgram::LookupShape(FAllocationContext Context, UField* ImportedType) const
{
	const VValue* FoundShape = ShapeMap.Find(ImportedType);
	if (!FoundShape)
	{
		return nullptr;
	}

	return &FoundShape->StaticCast<VShape>();
}

VPackage* VProgram::LookupPackage(FAllocationContext Context, UPackage* Package)
{
	for (int32 Index = 0; Index < PackageMap.Num(); ++Index)
	{
		VPackage& VersePackage = GetPackage(Index);
		if (VersePackage.GetUPackage() == Package)
		{
			return &VersePackage;
		}
	}
	return nullptr;
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
