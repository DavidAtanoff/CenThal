// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Templates/Casts.h"
#include "VerseVM/VVMCppClassInfo.h"
#include "VerseVM/VVMType.h"
#include "VerseVM/VVMWriteBarrier.h"

class UField;
enum class ECoreRedirectFlags : uint32;

namespace Verse
{
struct VArray;
struct VPackage;

// Abstract base class for VClass and VEnumeration
struct VNamedType : VType
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VType);

	VPackage& GetPackage() const { return *Package; }
	VArray& GetRelativePath() const { return *RelativePath; }
	VArray& GetBaseName() const { return *BaseName; }

	void AppendQualifiedName(FUtf8StringBuilderBase& Builder) const;
	void AppendScopeName(FUtf8StringBuilderBase& Builder) const;
	COREUOBJECT_API void AppendMangledName(FUtf8StringBuilderBase& Builder, UTF8CHAR Separator = UTF8CHAR('-')) const;
	COREUOBJECT_API FUtf8String GetFullName(); // used by ToJSON

	bool HasUEType() const { return !!NativeType; }
	template <typename FieldType>
	FieldType* GetUEType() const { return Cast<FieldType>(NativeType.Follow().ExtractUObject()); }
	template <typename FieldType>
	FieldType* GetUETypeChecked() const { return CastChecked<FieldType>(NativeType.Follow().AsUObject()); }
	bool IsImportAsPlaceholder() const { return NativeType.Follow().IsPlaceholder(); }
	bool IsNativeBound() const { return bNativeBound; }

	void AddRedirect(ECoreRedirectFlags Kind);

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);

	void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

protected:
	AUTORTFM_DISABLE COREUOBJECT_API VNamedType(FAllocationContext Context, VEmergentType* EmergentType, VPackage* InPackage, VArray* InRelativePath, VArray* InBaseName, VArray* InAttributeIndices, VArray* InAttributes, VValue InImportType, bool bInNativeBound);

	AUTORTFM_DISABLE COREUOBJECT_API VNamedType(FAllocationContext Context, VEmergentType* EmergentType);

	// The Verse path identifying this type, split into package, path, and name.

	TWriteBarrier<VPackage> Package;
	TWriteBarrier<VArray> RelativePath;
	TWriteBarrier<VArray> BaseName;

	// Attributes holds all the attributes for this type and its members, flattened into one array.
	// Each entry of AttributeIndices is the starting index of a group in Attributes.
	// AttributeIndices has one extra element at the end, to simplify lookup.
	TWriteBarrier<VArray> AttributeIndices;
	TWriteBarrier<VArray> Attributes;

	// A UField that represents this VNamedType when interacting with UE systems.
	// Before linking this is VValue(); for imported types this is a placeholder.
	TWriteBarrier<VValue> NativeType;

	bool bNativeBound;

	friend class FInterpreter;
};
} // namespace Verse
#endif // WITH_VERSE_VM
