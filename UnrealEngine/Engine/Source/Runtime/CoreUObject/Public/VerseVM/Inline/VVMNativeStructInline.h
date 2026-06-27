// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM.h"
#include "VerseVM/VVMCVars.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMEmergentType.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMObject.h"

namespace Verse
{

template <class CppStructType>
inline CppStructType& VNativeStruct::GetStruct()
{
	checkSlow(GetUScriptStruct(*GetEmergentType()) == StaticStruct<CppStructType>());

	return *BitCast<CppStructType*>(GetStruct());
}

inline void* VNativeStruct::GetStruct()
{
	return VObject::GetData(*GetEmergentType()->CppClassInfo);
}

inline UScriptStruct* VNativeStruct::GetUScriptStruct(VEmergentType& EmergentType)
{
	return EmergentType.Type->StaticCast<VClass>().GetUETypeChecked<UScriptStruct>();
}

template <class CppStructType>
inline VNativeStruct& VNativeStruct::New(FAllocationContext Context, VEmergentType& InEmergentType, CppStructType&& InStruct)
{
	UScriptStruct* ScriptStruct = GetUScriptStruct(InEmergentType);
	bool bHasDestructor = (ScriptStruct->StructFlags & STRUCT_NoDestructor) == 0;
	return *new (AllocateCell(Context, ScriptStruct->GetStructureSize(), bHasDestructor)) VNativeStruct(Context, InEmergentType, Forward<CppStructType>(InStruct));
}

inline VNativeStruct& VNativeStruct::NewUninitialized(FAllocationContext Context, VEmergentType& InEmergentType)
{
	UScriptStruct* ScriptStruct = GetUScriptStruct(InEmergentType);
	bool bHasDestructor = (ScriptStruct->StructFlags & STRUCT_NoDestructor) == 0;
	return *new (AllocateCell(Context, ScriptStruct->GetStructureSize(), bHasDestructor)) VNativeStruct(Context, InEmergentType);
}

inline std::byte* VNativeStruct::AllocateCell(FAllocationContext Context, size_t Size, bool bHasDestructor)
{
	const size_t ByteSize = DataOffset(StaticCppClassInfo) + Size;
	return bHasDestructor ? Context.Allocate(FHeap::DestructorSpace, ByteSize) : Context.AllocateFastCell(ByteSize);
}

template <class CppStructType>
inline VNativeStruct::VNativeStruct(FAllocationContext Context, VEmergentType& InEmergentType, CppStructType&& InStruct)
	: VObject(Context, InEmergentType)
{
	SetIsStruct();

	using StructType = typename TDecay<CppStructType>::Type;
	checkSlow(sizeof(StructType) == GetUScriptStruct(InEmergentType)->GetStructureSize());

	void* Data = GetData(*InEmergentType.CppClassInfo);

	// If the constructor below is rolled back, the struct must remain legible to the GC.
	FMemory::Memzero(Data, sizeof(StructType));

	bool const bConstructInClosed = AutoRTFM::IsTransactional() && !CVarUObjectLeniency.GetValueOnAnyThread();
	FOpResult Result = Context.CloseIf(bConstructInClosed, [&] {
		new (Data) StructType(Forward<CppStructType>(InStruct));
	});
	V_DIE_UNLESS(Result.IsReturn());
}

inline VNativeStruct::VNativeStruct(FAllocationContext Context, VEmergentType& InEmergentType)
	: VObject(Context, InEmergentType)
{
	SetIsStruct();

	UScriptStruct* ScriptStruct = GetUScriptStruct(InEmergentType);
	void* Data = GetData(*InEmergentType.CppClassInfo);

	// If the constructor below is rolled back, the struct must remain legible to the GC.
	FMemory::Memzero(Data, ScriptStruct->GetStructureSize());

	bool const bInitializeInClosed = AutoRTFM::IsTransactional() && !CVarUObjectLeniency.GetValueOnAnyThread();
	FOpResult Result = Context.CloseIf(bInitializeInClosed, [&] {
		ScriptStruct->InitializeStruct(Data);
	});
	V_DIE_UNLESS(Result.IsReturn());
}

inline VNativeStruct::VNativeStruct(FAllocationContext Context)
	: VObject(Context, GlobalTrivialEmergentType.Get(Context))
{
	SetIsStruct();
}

inline VNativeStruct::~VNativeStruct()
{
	VEmergentType* EmergentType = GetEmergentType();
	UScriptStruct* ScriptStruct = GetUScriptStruct(*EmergentType);

	AutoRTFM::ETransactionStatus Status = AutoRTFM::CloseIfTransactional([&] {
		ScriptStruct->DestroyStruct(VObject::GetData(*EmergentType->CppClassInfo));
	});
	V_DIE_IF(Status != AutoRTFM::ETransactionStatus::Executing);
}

} // namespace Verse
#endif // WITH_VERSE_VM
