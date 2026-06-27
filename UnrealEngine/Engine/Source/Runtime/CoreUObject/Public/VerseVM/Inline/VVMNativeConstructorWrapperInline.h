// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMNativeConstructorWrapper.h"

#include "AutoRTFM.h"
#include "VerseVM/Inline/VVMNativeStructInline.h"
#include "VerseVM/Inline/VVMShapeInline.h"
#include "VerseVM/VVMCVars.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMUniqueString.h"
#include "VerseVM/VVMValue.h"
#include "VerseVM/VVMVerseClass.h"

namespace Verse
{

inline bool VNativeConstructorWrapper::CreateFieldCached(FAllocationContext Context, FCreateFieldCacheCase Cache)
{
	bool bNewField = EmergentTypeOffset != Cache.NextEmergentTypeOffset;
	switch (Cache.Kind)
	{
		case FCreateFieldCacheCase::EKind::NativeStruct:
		{
			VNativeStruct& NativeStruct = WrappedObject().template StaticCast<VNativeStruct>();
			VShape* Shape = NativeStruct.GetEmergentType()->Shape.Get();
			const VShape::VEntry& Field = Shape->GetField(Cache.FieldIndex);
			if (bNewField)
			{
				SetEmergentType(Context, FHeap::EmergentTypeOffsetToPtr(Cache.NextEmergentTypeOffset));
				if (Field.IsProperty())
				{
					const bool bClose = !CVarUObjectLeniency.GetValueOnAnyThread();
					FOpResult Result = Context.CloseIf(bClose, [&] {
						void* Data = NativeStruct.GetData(*NativeStruct.GetEmergentType()->CppClassInfo);
						Field.UProperty->InitializeValue_InContainer(Data);
						if (Field.Type == EFieldType::FVerseProperty)
						{
							Field.UProperty->ContainerPtrToValuePtr<VRestValue>(Data)->Reset(0);
						}
					});
					V_DIE_UNLESS(Result.IsReturn());
				}
			}
			break;
		}
		case FCreateFieldCacheCase::EKind::UObject:
		{
			UObject* UEObject = WrappedObject().ExtractUObject();
			UVerseClass* VerseClass = Cast<UVerseClass>(UEObject->GetClass());
			VShape& Shape = VerseClass->Shape.Get(Context).StaticCast<VShape>();
			const VShape::VEntry& Field = Shape.GetField(Cache.FieldIndex);
			if (bNewField)
			{
				SetEmergentType(Context, FHeap::EmergentTypeOffsetToPtr(Cache.NextEmergentTypeOffset));
				if (Field.IsProperty())
				{
					const bool bClose = !CVarUObjectLeniency.GetValueOnAnyThread();
					FOpResult Result = Context.CloseIf(bClose, [&] {
						Field.UProperty->InitializeValue_InContainer(UEObject);
						if (Field.Type == EFieldType::FVerseProperty)
						{
							Field.UProperty->ContainerPtrToValuePtr<VRestValue>(UEObject)->Reset(0);
						}
					});
					V_DIE_UNLESS(Result.IsReturn());
				}
			}
			break;
		}
	}
	return bNewField;
}

inline bool VNativeConstructorWrapper::CreateField(FAllocationContext Context, VUniqueString& FieldName, FCreateFieldCacheCase* OutCacheCase)
{
	if (VNativeStruct* NativeStruct = WrappedObject().DynamicCast<VNativeStruct>())
	{
		if (VShape* Shape = NativeStruct->GetEmergentType()->Shape.Get())
		{
			int32 FieldIndex = Shape->GetFieldIndex(FieldName);
			const VShape::VEntry& Field = Shape->GetField(FieldIndex);
			if (Field.IsProperty() || Field.IsAccessor())
			{
				if (IsFieldCreated(FieldIndex))
				{
					if (OutCacheCase)
					{
						OutCacheCase->Kind = FCreateFieldCacheCase::EKind::NativeStruct;
						OutCacheCase->FieldIndex = FieldIndex;
						OutCacheCase->NextEmergentTypeOffset = FHeap::EmergentTypePtrToOffset(GetEmergentType());
					}
					return false;
				}
				VEmergentType* NewType = GetEmergentType()->MarkFieldAsCreated(Context, FieldIndex);
				SetEmergentType(Context, NewType);
				if (OutCacheCase)
				{
					OutCacheCase->Kind = FCreateFieldCacheCase::EKind::NativeStruct;
					OutCacheCase->FieldIndex = FieldIndex;
					OutCacheCase->NextEmergentTypeOffset = FHeap::EmergentTypePtrToOffset(NewType);
				}

				if (Field.IsProperty())
				{
					const bool bClose = !CVarUObjectLeniency.GetValueOnAnyThread();
					FOpResult Result = Context.CloseIf(bClose, [&] {
						void* Data = NativeStruct->GetData(*NativeStruct->GetEmergentType()->CppClassInfo);
						Field.UProperty->InitializeValue_InContainer(Data);
						if (Field.Type == EFieldType::FVerseProperty)
						{
							Field.UProperty->ContainerPtrToValuePtr<VRestValue>(Data)->Reset(0);
						}
					});
					V_DIE_UNLESS(Result.IsReturn());
				}
				return true;
			}
		}
	}
	else if (UObject* UEObject = WrappedObject().ExtractUObject())
	{
		if (UVerseClass* VerseClass = Cast<UVerseClass>(UEObject->GetClass()))
		{
			VShape& Shape = VerseClass->Shape.Get(Context).StaticCast<VShape>();
			int32 FieldIndex = Shape.GetFieldIndex(FieldName);
			const VShape::VEntry& Field = Shape.GetField(FieldIndex);
			if (Field.IsProperty() || Field.IsAccessor())
			{
				if (IsFieldCreated(FieldIndex))
				{
					if (OutCacheCase)
					{
						OutCacheCase->Kind = FCreateFieldCacheCase::EKind::UObject;
						OutCacheCase->FieldIndex = FieldIndex;
						OutCacheCase->NextEmergentTypeOffset = FHeap::EmergentTypePtrToOffset(GetEmergentType());
					}
					return false;
				}
				VEmergentType* NewType = GetEmergentType()->MarkFieldAsCreated(Context, FieldIndex);
				SetEmergentType(Context, NewType);
				if (OutCacheCase)
				{
					OutCacheCase->Kind = FCreateFieldCacheCase::EKind::UObject;
					OutCacheCase->FieldIndex = FieldIndex;
					OutCacheCase->NextEmergentTypeOffset = FHeap::EmergentTypePtrToOffset(NewType);
				}

				if (Field.IsProperty())
				{
					const bool bClose = !CVarUObjectLeniency.GetValueOnAnyThread();
					FOpResult Result = Context.CloseIf(bClose, [&] {
						Field.UProperty->InitializeValue_InContainer(UEObject);
						if (Field.Type == EFieldType::FVerseProperty)
						{
							Field.UProperty->ContainerPtrToValuePtr<VRestValue>(UEObject)->Reset(0);
						}
					});
					V_DIE_UNLESS(Result.IsReturn());
				}
				return true;
			}
		}
	}
	V_DIE("Could not create field for imported class/struct!");
	return false;
}

inline VValue VNativeConstructorWrapper::WrappedObject() const
{
	return NativeObject.Get();
}
} // namespace Verse
#endif // WITH_VERSE_VM
