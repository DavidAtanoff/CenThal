// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMObject.h"

namespace Verse
{
enum class ECompares : uint8;
struct VClass;

template <typename CppStructType>
VClass& StaticVClass();

/// A variant of Verse object that boxes a native (C++ defined) struct
struct VNativeStruct : VObject
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VObject);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	template <class CppStructType>
	CppStructType& GetStruct();
	void* GetStruct();

	static UScriptStruct* GetUScriptStruct(VEmergentType& EmergentType);

	/// Allocate a new VNativeStruct and move an existing struct into it
	template <class CppStructType>
	AUTORTFM_DISABLE static VNativeStruct& New(FAllocationContext Context, VEmergentType& InEmergentType, CppStructType&& InStruct);

	/// Allocate a new blank VNativeStruct
	AUTORTFM_DISABLE static VNativeStruct& NewUninitialized(FAllocationContext Context, VEmergentType& InEmergentType);

	AUTORTFM_DISABLE VNativeStruct& Duplicate(FAllocationContext Context);

protected:
	friend class FInterpreter;

	AUTORTFM_DISABLE static std::byte* AllocateCell(FAllocationContext Context, size_t Size, bool bHasDestructor);

	template <class CppStructType>
	AUTORTFM_DISABLE VNativeStruct(FAllocationContext Context, VEmergentType& InEmergentType, CppStructType&& InStruct);
	AUTORTFM_DISABLE VNativeStruct(FAllocationContext Context, VEmergentType& InEmergentType);
	AUTORTFM_DISABLE VNativeStruct(FAllocationContext Context);
	~VNativeStruct();

	ECompares EqualImpl(FAllocationContext Context, VCell* Other, const TFunction<void(::Verse::VValue, ::Verse::VValue)>& HandlePlaceholder);
	uint32 GetTypeHashImpl();
	AUTORTFM_DISABLE VValue MeltImpl(FAllocationContext Context);
	AUTORTFM_DISABLE FOpResult FreezeImpl(FAllocationContext Context, VTask* Task);
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext, VNativeStruct*&, FStructuredArchiveVisitor&);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext, FStructuredArchiveVisitor&);
	static constexpr bool InstancedCell = true;
};

} // namespace Verse
#endif // WITH_VERSE_VM
