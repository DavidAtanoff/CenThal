// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "VerseVM/VVMWriteBarrier.h"
#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "UObject/UnrealType.h" // For FProperty
#include "UObject/VerseValueProperty.h"
#include "VerseVM/Inline/VVMShapeInline.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMNativeConverter.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMObject.h"
#include "VerseVM/VVMProcedure.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMUnreachable.h"

// NOTE: (yiliang.siew) Silence these warnings for now in the cases below.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
#endif

namespace Verse
{
template <typename TBinaryFunction>
bool VObject::AllFields(FAllocationContext Context, TBinaryFunction F)
{
	VEmergentType* EmergentType = GetEmergentType();
	for (auto I = EmergentType->Shape->CreateFieldsIterator(); I; ++I)
	{
		if (!::Invoke(F, I->Key->AsStringView(), LoadField(Context, *EmergentType, &I->Value)))
		{
			return false;
		}
	}
	return true;
}

inline FOpResult VObject::LoadField(FAllocationContext Context, const VUniqueString& Name, VValue Self, FLoadFieldCacheCase* OutCacheCase)
{
	VEmergentType* EmergentType = GetEmergentType();
	return LoadField(Context, *EmergentType, EmergentType->Shape->GetField(Name), Self, OutCacheCase);
}

template <EWriteMode WriteMode>
inline FOpResult VObject::SetField(FAllocationContext Context, const VShape& Shape, const VUniqueString& Name, void* Data, VValue Value)
{
	const VShape::VEntry* Field = Shape.GetField(Name);
	V_DIE_IF(Field == nullptr);
	return SetField<WriteMode>(Context, *Field, Data, Value);
}

template <EWriteMode WriteMode>
inline FOpResult VObject::SetField(FAllocationContext Context, const VShape::VEntry& Field, void* Data, VValue Value)
{
	AutoRTFM::UnreachableIfClosed("#jira SOL-8415");

	switch (Field.Type)
	{
		case EFieldType::Offset:
			BitCast<VRestValue*>(Data)[Field.Index].Set<WriteMode>(Context, Value);
			return {FOpResult::Return};
		case EFieldType::FProperty:
			return VNativeRef::Set<WriteMode>(Context, nullptr, Data, Field.UProperty, Value);
		case EFieldType::FPropertyVar:
			return VNativeRef::Set<WriteMode>(Context, nullptr, Data, Field.UProperty, Value.StaticCast<VRef>().Get(Context));
		case EFieldType::FVerseProperty:
			Field.UProperty->ContainerPtrToValuePtr<VRestValue>(Data)->Set<WriteMode>(Context, Value);
			return {FOpResult::Return};
		case EFieldType::Constant:
		default:
			VERSE_UNREACHABLE(); // This shouldn't happen since such field's data should be on the shape, not the object.
			break;
	}
}

template <EWriteMode WriteMode /* = EWriteMode::Default */>
inline FOpResult VObject::SetField(FAllocationContext Context, const VUniqueString& Name, VValue Value)
{
	const VEmergentType* EmergentType = GetEmergentType();
	return SetField<WriteMode>(Context, *EmergentType->Shape, Name, GetData(*EmergentType->CppClassInfo), Value);
}

} // namespace Verse
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif // WITH_VERSE_VM
