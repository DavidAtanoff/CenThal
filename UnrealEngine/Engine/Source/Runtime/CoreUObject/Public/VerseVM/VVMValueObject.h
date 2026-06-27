// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM/Defines.h"
#include "VerseVM/VVMCreateFieldInlineCache.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMObject.h"

namespace Verse
{
enum class ECompares : uint8;

/// Specialization of VObject that stores only VValues
struct VValueObject : VObject
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VObject);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	/// Allocate a new object with the given shape, populated with placeholders
	AUTORTFM_DISABLE static VValueObject& NewUninitialized(FAllocationContext Context, VEmergentType& InEmergentType);

	bool CreateFieldCached(FAllocationContext Context, FCreateFieldCacheCase Cache);
	AUTORTFM_DISABLE bool CreateField(FAllocationContext Context, VUniqueString& Name, FCreateFieldCacheCase* OutCacheCase = nullptr);

protected:
	friend class FInterpreter;

	AUTORTFM_DISABLE static std::byte* AllocateCell(FAllocationContext Context, VCppClassInfo& CppClassInfo, uint64 NumIndexedFields);

	AUTORTFM_DISABLE VValueObject(FAllocationContext Context, VEmergentType& InEmergentType);

	AUTORTFM_DISABLE ECompares EqualImpl(FAllocationContext Context, VCell* Other, const TFunction<void(::Verse::VValue, ::Verse::VValue)>& HandlePlaceholder);
	uint32 GetTypeHashImpl();
	AUTORTFM_DISABLE VValue MeltImpl(FAllocationContext Context);
	AUTORTFM_DISABLE FOpResult FreezeImpl(FAllocationContext Context, VTask* Task);
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VValueObject*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

	// This is subtle: structs are instanced by both InstanceSubobjects and StaticDuplicateObject,
	// but classes are ignored by InstanceSubobjects as they do not have an Outer to be filtered by.
	static constexpr bool InstancedCell = true;
};

inline std::byte* VValueObject::AllocateCell(FAllocationContext Context, VCppClassInfo& CppClassInfo, uint64 NumIndexedFields)
{
	return Context.AllocateFastCell(DataOffset(CppClassInfo) + NumIndexedFields * sizeof(VRestValue));
}
} // namespace Verse

#endif // WITH_VERSE_VM
