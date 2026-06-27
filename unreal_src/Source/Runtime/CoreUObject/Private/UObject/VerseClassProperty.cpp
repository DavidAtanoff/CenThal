// Copyright Epic Games, Inc. All Rights Reserved.

#include "UObject/VerseClassProperty.h"

#include "Templates/Casts.h"

#include "UObject/Class.h"
#include "UObject/PropertyTypeName.h"
#include "UObject/PropertyHelper.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UnrealType.h"
#include "UObject/UnrealTypePrivate.h"
#include "VerseVM/VVMVerseClass.h"
#include "Hash/Blake3.h"

IMPLEMENT_FIELD(FVerseClassProperty)

FVerseClassProperty::FVerseClassProperty(FFieldVariant InOwner, const FName& InName)
	: Super(InOwner, InName)
{
}

FVerseClassProperty::FVerseClassProperty(FFieldVariant InOwner, const UECodeGen_Private::FVerseClassPropertyParams& Prop)
	: Super{InOwner, (const UECodeGen_Private::FClassPropertyParams&)Prop}
	, bRequiresConcrete{Prop.bRequiresConcrete}
	, bRequiresCastable{Prop.bRequiresCastable}
{
}

void FVerseClassProperty::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar << bRequiresConcrete;
	Ar << bRequiresCastable;
}

const TCHAR* FVerseClassProperty::ImportText_Internal(const TCHAR* Buffer, TNotNull<void*> ContainerOrPropertyPtr, EPropertyPointerType PropertyPointerType, UObject* Parent, int32 PortFlags, FOutputDevice* ErrorText) const
{
	const TCHAR* Result = Super::ImportText_Internal(Buffer, ContainerOrPropertyPtr, PropertyPointerType, Parent, PortFlags, ErrorText);

	if (Result)
	{
		void* Data = PointerToValuePtr(ContainerOrPropertyPtr, PropertyPointerType);
		UObject* AssignedVersePropertyObject = GetObjectPropertyValue(Data);
		UClass* AssignedVersePropertyClass = Cast<UClass>(AssignedVersePropertyObject);
		// Validate constraints
		if ((bRequiresConcrete && !StaticIsClassConcrete(AssignedVersePropertyClass))
			|| (bRequiresCastable && !StaticIsClassCastable(AssignedVersePropertyClass)))
		{
			ErrorText->Logf(TEXT("Invalid object '%s' specified for property '%s'"), *GetNameSafe(AssignedVersePropertyObject), *GetName());
			UObject* NullObj = nullptr;

			if (PropertyPointerType == EPropertyPointerType::Container && HasSetter())
			{
				SetValue_InContainer(ContainerOrPropertyPtr, NullObj);
			}
			else
			{
				SetObjectPropertyValue(PointerToValuePtr(ContainerOrPropertyPtr, PropertyPointerType), NullObj);
			}
			Result = nullptr;
		}
	}

	return Result;
}

FString FVerseClassProperty::GetCPPMacroType(FString& ExtendedTypeText) const
{
	if (PropertyFlags & CPF_TObjectPtr)
	{
		ExtendedTypeText = FString::Printf(TEXT("TObjectPtr<%s%s>"), PropertyClass->GetPrefixCPP(), *PropertyClass->GetName());
		return TEXT("OBJECTPTR");
	}
	ExtendedTypeText = TEXT("UVerseClass");
	return TEXT("OBJECT");
}

void FVerseClassProperty::PostDuplicate(const FField& InField)
{
	const FVerseClassProperty* Other = static_cast<const FVerseClassProperty*>(&InField);
	bRequiresCastable = Other->bRequiresCastable;
	bRequiresConcrete = Other->bRequiresConcrete;

	Super::PostDuplicate(InField);
}

bool FVerseClassProperty::SameType(const FProperty* Other) const
{
	return Super::SameType(Other)
		&& (bRequiresConcrete == static_cast<const FVerseClassProperty*>(Other)->bRequiresConcrete)
		&& (bRequiresCastable == static_cast<const FVerseClassProperty*>(Other)->bRequiresCastable);
}

#if WITH_EDITORONLY_DATA
void FVerseClassProperty::AppendSchemaHash(FBlake3& Builder, bool bSkipEditorOnly) const
{
	Super::AppendSchemaHash(Builder, bSkipEditorOnly);

	uint8_t RequiresConcrete = bRequiresConcrete;
	Builder.Update(&RequiresConcrete, 1);
	uint8_t RequiresCastable = bRequiresCastable;
	Builder.Update(&RequiresCastable, 1);
}
#endif

const UVerseClass* FVerseClassProperty::GetVerseClass(const UClass* Class)
{
	if (Class)
	{
		for (const UStruct* TestStruct : Class->GetSuperStructIterator())
		{
			if (const UVerseClass* VerseClass = Cast<UVerseClass>(TestStruct))
			{
				return VerseClass;
			}
		}
	}

	return nullptr;
}

bool FVerseClassProperty::StaticIsClassConcrete(const UClass* Class)
{
	if (const UVerseClass* VerseClass = GetVerseClass(Class))
	{
		return VerseClass->IsConcrete();
	}
	return false;
}

bool FVerseClassProperty::StaticIsClassCastable(const UClass* Class)
{
	if (const UVerseClass* VerseClass = GetVerseClass(Class))
	{
		return VerseClass->IsExplicitlyCastable();
	}
	return false;
}
