// Copyright Epic Games, Inc. All Rights Reserved.

#include "UObject/VerseValueProperty.h"
#include "AutoRTFM.h"
#include "UObject/GarbageCollectionSchema.h"
#include "UObject/InstanceDataObjectUtils.h"
#include "UObject/ObjectInstancingGraph.h"
#include "UObject/PropertyHelper.h"
#include "VerseVM/Inline/VVMEnterVMInline.h"
#include "VerseVM/Inline/VVMMapInline.h"
#include "VerseVM/Inline/VVMObjectInline.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/VVMAbstractVisitor.h"
#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMEngineEnvironment.h"
#include "VerseVM/VVMIntType.h"
#include "VerseVM/VVMJson.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMNativeType.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMStructuredArchiveVisitor.h"
#include "VerseVM/VVMValue.h"
#include "VerseVM/VVMWriteBarrier.h"
#if WITH_EDITORONLY_DATA
#include "VerseVM/VBPVMDynamicProperty.h"
#endif

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

FVCellProperty::FVCellProperty(FFieldVariant InOwner, const FName& InName)
	: Super(InOwner, InName)
{
}

FVCellProperty::FVCellProperty(FFieldVariant InOwner, const UECodeGen_Private::FVerseCellValuePropertyParams& Prop)
	: Super(InOwner, reinterpret_cast<const UECodeGen_Private::FPropertyParamsBaseWithOffset&>(Prop))
{
}

FVValueProperty::FVValueProperty(FFieldVariant InOwner, const FName& InName)
	: Super(InOwner, InName)
{
}

FVValueProperty::FVValueProperty(FFieldVariant InOwner, const UECodeGen_Private::FVerseValuePropertyParams& Prop)
	: Super(InOwner, reinterpret_cast<const UECodeGen_Private::FPropertyParamsBaseWithOffset&>(Prop))
{
}

FVRestValueProperty::FVRestValueProperty(FFieldVariant InOwner, const FName& InName)
	: Super(InOwner, InName)
{
}

FVRestValueProperty::FVRestValueProperty(FFieldVariant InOwner, const UECodeGen_Private::FVerseValuePropertyParams& Prop)
	: Super(InOwner, reinterpret_cast<const UECodeGen_Private::FPropertyParamsBaseWithOffset&>(Prop))
{
}

FVRestValueProperty::~FVRestValueProperty()
{
	if (LegacyProperty)
	{
		delete LegacyProperty;
		LegacyProperty = nullptr;
	}
}

void FVRestValueProperty::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedVerseValue(Type);
}

void FVRestValueProperty::AddCppProperty(FProperty* Property)
{
	check(Property);
	check(!LegacyProperty);
	LegacyProperty = Property;
#if WITH_EDITORONLY_DATA
	bNativeRepresentation = false;
#endif
}

FField* FVRestValueProperty::GetInnerFieldByName(const FName& InName)
{
	if (LegacyProperty && LegacyProperty->GetFName() == InName)
	{
		return LegacyProperty;
	}
	return nullptr;
}

void FVRestValueProperty::GetInnerFields(TArray<FField*>& OutFields)
{
	if (LegacyProperty)
	{
		OutFields.Add(LegacyProperty);
		LegacyProperty->GetInnerFields(OutFields);
	}
}

FProperty* FVRestValueProperty::GetOrCreateLegacyProperty(Verse::FAllocationContext Context)
{
	if (LegacyProperty == nullptr)
	{
		AutoRTFM::Open([&] AUTORTFM_DISABLE {
			Verse::IEngineEnvironment* Environment = Verse::VerseVM::GetEngineEnvironment();
			LegacyProperty = Environment->CreateLegacyProperty(Context, this);

			FArchive Ar;
			LegacyProperty->Link(Ar);
		});
	}

	return LegacyProperty;
}

// Construct a default-initialized value of legacy property type, to be deserialized into.
//
// When a property has no Verse-level default initializer, it is instead delta serialized
// against its legacy property type's default value. However, LegacyProperty is "shallow"
// in the sense that its inner properties may be VRestValues, with the wrong default value
// for delta serialization.
// 
// This function thus initializes these inner properties to *their* legacy properties'
// default values, to be used if they were left out of an asset by delta serialization.
void* FVRestValueProperty::AllocateAndInitializeLegacyValue(Verse::FAllocationContext Context)
{
	GetOrCreateLegacyProperty(Context);
	V_DIE_UNLESS(LegacyProperty);

	void* Data = LegacyProperty->AllocateAndInitializeValue();
	if (FStructProperty* StructProperty = CastField<FStructProperty>(LegacyProperty))
	{
		for (TFieldIterator<FVRestValueProperty> It(StructProperty->Struct); It; ++It)
		{
			void* InnerData = It->AllocateAndInitializeLegacyValue(Context);
			Verse::VValue Value = Verse::VNativeRef::Peek(Context, InnerData, It->LegacyProperty);
			It->LegacyProperty->DestroyAndFreeValue(InnerData);

			// Value may be uninitialized if LegacyProperty is or contains a legacy FObjectProperty.
			// In this case, a value must be provided either by a default initializer or by the asset,
			// so it is fine to leave it uninitialized here.
			It->GetPropertyValuePtr_InContainer(Data)->Set(Context, Value);
		}
	}
	return Data;
}

void FVRestValueProperty::PostDuplicate(const FField& InField)
{
	Verse::FAllocationContext Context = Verse::FAllocationContextPromise{};
	const FVRestValueProperty& Source = static_cast<const FVRestValueProperty&>(InField);
	Type.Set(Context, Source.Type.Get());
	if (Source.LegacyProperty)
	{
		LegacyProperty = CastFieldChecked<FProperty>(FField::Duplicate(Source.LegacyProperty, this));
	}
#if WITH_EDITORONLY_DATA
	bNativeRepresentation = Source.bNativeRepresentation;
#endif
	Super::PostDuplicate(InField);
}

namespace
{
AUTORTFM_DISABLE EPropertyVisitorControlFlow VisitValue(Verse::FAllocationContext Context, bool bTransactional, const FPropertyVisitorContext& VisitorContext, const TFunctionRef<EPropertyVisitorControlFlow(const FPropertyVisitorContext&)> InFunc, Verse::VValue Value)
{
	EPropertyVisitorControlFlow RetVal = EPropertyVisitorControlFlow::StepInto;

	if (UObject* Object = Value.ExtractUObject())
	{
		auto VisitObject = [&] AUTORTFM_ENABLE {
			FPropertyVisitorContext SubContext = VisitorContext.VisitPropertyData(Object);
			RetVal = Object->GetClass()->Visit(SubContext, InFunc);
		};
		if (bTransactional)
		{
			AutoRTFM::ETransactionStatus Status = AutoRTFM::Close(VisitObject);
			if (Status != AutoRTFM::ETransactionStatus::Executing)
			{
				return EPropertyVisitorControlFlow::Stop;
			}
		}
		else
		{
			VisitObject();
		}
	}
	else if (Verse::VNativeStruct* NativeStruct = Value.DynamicCast<Verse::VNativeStruct>())
	{
		UScriptStruct* ScriptStruct = Verse::VNativeStruct::GetUScriptStruct(*NativeStruct->GetEmergentType());
		void* PropertyData = NativeStruct->GetStruct();
		auto VisitStruct = [&] AUTORTFM_ENABLE {
			FPropertyVisitorContext SubContext = VisitorContext.VisitPropertyData(PropertyData);
			RetVal = ScriptStruct->Visit(SubContext, InFunc);
		};
		if (bTransactional)
		{
			AutoRTFM::ETransactionStatus Status = AutoRTFM::Close(VisitStruct);
			if (Status != AutoRTFM::ETransactionStatus::Executing)
			{
				return EPropertyVisitorControlFlow::Stop;
			}
		}
		else
		{
			VisitStruct();
		}
	}
	else if (Verse::VValueObject* Struct = Value.DynamicCast<Verse::VValueObject>(); Struct && Struct->IsStruct())
	{
		Verse::VEmergentType& EmergentType = *Struct->GetEmergentType();
		for (auto It = EmergentType.Shape->CreateFieldsIterator(); It; ++It)
		{
			Verse::FOpResult LoadResult = Struct->LoadField(Context, *It->Key);
			V_DIE_UNLESS(LoadResult.IsReturn());
			RetVal = VisitValue(Context, bTransactional, VisitorContext, InFunc, LoadResult.Value);
			if (RetVal == EPropertyVisitorControlFlow::Stop)
			{
				return EPropertyVisitorControlFlow::Stop;
			}
		}
	}
	else if (Verse::VArrayBase* Array = Value.DynamicCast<Verse::VArrayBase>())
	{
		// Non-VValue arrays cannot hold elements with properties to be visited.
		if (Array->GetArrayType() != Verse::EArrayType::VValue)
		{
			return RetVal;
		}

		uint32 ArrayNum = Array->Num();
		for (int32 Index = 0; Index < ArrayNum; ++Index)
		{
			RetVal = VisitValue(Context, bTransactional, VisitorContext, InFunc, Array->GetValue(Index));
			if (RetVal == EPropertyVisitorControlFlow::Stop)
			{
				return EPropertyVisitorControlFlow::Stop;
			}
		}
	}
	else if (Verse::VMapBase* Map = Value.DynamicCast<Verse::VMapBase>())
	{
		for (TPair<Verse::VValue, Verse::VValue> Pair : *Map)
		{
			RetVal = VisitValue(Context, bTransactional, VisitorContext, InFunc, Pair.Key);
			if (RetVal == EPropertyVisitorControlFlow::Stop)
			{
				return EPropertyVisitorControlFlow::Stop;
			}

			RetVal = VisitValue(Context, bTransactional, VisitorContext, InFunc, Pair.Value);
			if (RetVal == EPropertyVisitorControlFlow::Stop)
			{
				return EPropertyVisitorControlFlow::Stop;
			}
		}
	}
	else if (Verse::VOption* Option = Value.DynamicCast<Verse::VOption>())
	{
		RetVal = VisitValue(Context, bTransactional, VisitorContext, InFunc, Option->GetValue());
	}
	else if (Verse::VFunction* Function = Value.DynamicCast<Verse::VFunction>())
	{
		RetVal = VisitValue(Context, bTransactional, VisitorContext, InFunc, Function->Self.Get());
	}
	return RetVal;
}
}

void FVRestValueProperty::VisitProperties(const FPropertyVisitorContext& VisitorContext, const TFunctionRef<EPropertyVisitorControlFlow(const FPropertyVisitorContext&)> InFunc) const
{
	const bool bTransactional = AutoRTFM::IsClosed();
	AutoRTFM::Open([&] AUTORTFM_DISABLE {
		Verse::FAllocationContext Context = Verse::FAllocationContextPromise{};

		Verse::VRestValue& Slot = TTypeFundamentals::GetPropertyValuePtr(VisitorContext.Data.PropertyData)[0];
		Verse::VValue Value = Slot.Get(Context);
		if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
		{
			Value = Ref->Get(Context);
		}

		VisitValue(Context, bTransactional, VisitorContext, InFunc, Value);
	});
}

#if WITH_EDITORONLY_DATA

Verse::VValue FVRestValueProperty::ConvertToVValueLegacy(Verse::FAllocationContext Context, const void* Container, FProperty* Property)
{
	if (FVerseDynamicProperty* DynamicProperty = CastField<FVerseDynamicProperty>(Property))
	{
		const UE::FDynamicallyTypedValue* LegacyValue = DynamicProperty->ContainerPtrToValuePtr<UE::FDynamicallyTypedValue>(Container);
		return LegacyValue->ToVValue(Context);
	}

	// Convert type values.  `subtype` with class or interface upper bound and
	// `subtype` variants use `FVerseClassProperty`, while all other type values
	// use nullable `FObjectProperty`.  Note `FVerseClassProperty` inherits
	// (transitively) from `FObjectProperty`.
	if (FObjectProperty* TypeProperty = CastField<FObjectProperty>(Property))
	{
		if (UObject* Object = TypeProperty->GetObjectPtrPropertyValue_InContainer(Container))
		{
			Verse::FNativeType NativeType(CastChecked<UStruct>(Object));
			return *NativeType.Type;
		}
	}

	return Verse::VValue();
}

bool FVRestValueProperty::ConvertFromVValueLegacy(Verse::FAllocationContext Context, void* Container, FProperty* Property, Verse::VValue Value)
{
	if (FVerseDynamicProperty* DynamicProperty = CastField<FVerseDynamicProperty>(Property))
	{
		// Leave dynamic values default-initialized. Their deserialization code ignores the old value.
		return true;
	}

	if (FObjectProperty* TypeProperty = CastField<FObjectProperty>(Property))
	{
		// Leave non-class/struct types default-initialized. BPVM data cannot represent them anyway.
		if (Verse::VClass* Class = Value.DynamicCast<Verse::VClass>())
		{
			TypeProperty->SetObjectPropertyValue_InContainer(Container, Class->GetUETypeChecked<UObject>());
		}
		return true;
	}

	return false;
}

static EConvertFromTypeResult SerializeWithConversion(FProperty* Property, const FPropertyTag& Tag, FStructuredArchive::FSlot Slot, uint8* Data, const UStruct* DefaultsStruct, const uint8* DefaultsData)
{
	EConvertFromTypeResult Result = Property->ConvertFromType(Tag, Slot, Data, DefaultsStruct, (uint8*)DefaultsData);
	switch (Result)
	{
		case EConvertFromTypeResult::UseSerializeItem:
			if (FName PropID = Property->GetID(); Tag.Type != PropID)
			{
				UE_LOGF(LogClass, Warning, "Type mismatch in %ls of %ls - Previous (%ls) Current(%ls) in package: %ls",
					*WriteToString<32>(Tag.Name), *WriteToString<32>(Property->GetOwnerClass()->GetFName()),
					*WriteToString<32>(Tag.Type), *WriteToString<32>(PropID),
					*Slot.GetUnderlyingArchive().GetArchiveName());
				return EConvertFromTypeResult::CannotConvert;
			}
			if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			{
				BoolProperty->SetPropertyValue(Data, Tag.BoolVal != 0);
			}
			else
			{
				Property->SerializeItem(Slot, Data, DefaultsData);
			}
			return EConvertFromTypeResult::Serialized;

		case EConvertFromTypeResult::Serialized:
		case EConvertFromTypeResult::CannotConvert:
		case EConvertFromTypeResult::Converted:
			return Result;
	}
	VERSE_UNREACHABLE();
}

EConvertFromTypeResult FVRestValueProperty::ConvertFromType(const FPropertyTag& Tag, FStructuredArchive::FSlot Slot, uint8* Data, const UStruct* DefaultsStruct, const uint8* Defaults)
{
	if (Tag.Type == GetID())
	{
		return EConvertFromTypeResult::UseSerializeItem;
	}

	AutoRTFM::UnreachableIfTransactional();

	Verse::FRunningContext Context = Verse::FRunningContextPromise{};

	// Generate the corresponding BPVM property if necessary.
	if (!GetOrCreateLegacyProperty(Context))
	{
		return EConvertFromTypeResult::CannotConvert;
	}

	void* DeserializedData = AllocateAndInitializeLegacyValue(Context);

	// Convert the default value back to a legacy value in the destination buffer.
	// This is required for things like structs that delta serialize against the default.
	// When there is no default, leave the buffer default-initialized.
	Verse::VRestValue& RestValue = *GetPropertyValuePtr_InContainer(Data, Tag.ArrayIndex);
	if (!RestValue.IsUninitialized())
	{
		Verse::VValue Value = RestValue.Get(Context);
		if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
		{
			Value = Ref->Get(Context);
		}

		Verse::FOpResult Result = Verse::VNativeRef::Set<Verse::EWriteMode::NonTransactional>(Context, nullptr, DeserializedData, LegacyProperty, Value);
		V_DIE_UNLESS(Result.IsReturn());
	}

	// Serialize the legacy data. If Defaults is provided, convert it in the same way as Data above.
	EConvertFromTypeResult SerializeResult;
	if (Defaults)
	{
		void* DefaultsData = AllocateAndInitializeLegacyValue(Context);

		Verse::VRestValue& DefaultRestValue = *GetPropertyValuePtr_InContainer(const_cast<uint8*>(Defaults), Tag.ArrayIndex);
		if (!DefaultRestValue.IsUninitialized())
		{
			Verse::VValue Value = DefaultRestValue.Get(Context);
			if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
			{
				Value = Ref->Get(Context);
			}

			Verse::FOpResult Result = Verse::VNativeRef::Set<Verse::EWriteMode::NonTransactional>(Context, nullptr, DefaultsData, LegacyProperty, Value);
			V_DIE_UNLESS(Result.IsReturn());
		}

		FSerializedPropertyScope SerializedProperty(Slot.GetUnderlyingArchive(), this);
		SerializeResult = SerializeWithConversion(LegacyProperty, Tag, Slot, (uint8*)DeserializedData, DefaultsStruct, (uint8*)DefaultsData);

		LegacyProperty->DestroyAndFreeValue(DefaultsData);
	}
	else
	{
		FSerializedPropertyScope SerializedProperty(Slot.GetUnderlyingArchive(), this);
		SerializeResult = SerializeWithConversion(LegacyProperty, Tag, Slot, (uint8*)DeserializedData, DefaultsStruct, nullptr);
	}

	// Convert legacy to VValue
	Verse::VValue Result = Verse::VNativeRef::Peek(Context, DeserializedData, LegacyProperty);

	// Clean up
	LegacyProperty->DestroyAndFreeValue(DeserializedData);

	if (!Result)
	{
		UE_LOGF(LogProperty, Warning, "Invalid value for %ls", *GetFullName());
		return EConvertFromTypeResult::CannotConvert;
	}

	// Store in this property's data value
	if (!bNativeRepresentation && Type.Follow().IsCellOfType<Verse::VPointerType>())
	{
		Verse::VValue MeltedValue = Verse::VValue::Melt(Context, Result);
		Verse::VRef& NewRef = Verse::VRef::New(Context, {});
		NewRef.SetNonTransactionally(Context, MeltedValue);
		RestValue.Set(Context, NewRef);
	}
	else
	{
		RestValue.Set(Context, Result);
	}

	return SerializeResult;
}

#endif

void FVRestValueProperty::CopyValuesInternal(void* Dest, const void* Src, int32 Count) const
{
	const bool bTransactional = AutoRTFM::IsClosed();
	AutoRTFM::Open([&] AUTORTFM_DISABLE {
		Verse::FRunningContext Context = Verse::FRunningContextPromise{};
		for (int32 Index = 0; Index < Count; Index++)
		{
			Verse::VRestValue& DestSlot = TTypeFundamentals::GetPropertyValuePtr(Dest)[Index];
			Verse::VRestValue& SrcSlot = TTypeFundamentals::GetPropertyValuePtr(const_cast<void*>(Src))[Index];
			V_DIE_IF(DestSlot.CanDefQuickly() || SrcSlot.CanDefQuickly());

			Verse::VValue SrcValue = SrcSlot.Get(Context);
			V_DIE_IF(SrcValue.IsPlaceholder());
			if (bTransactional)
			{
				DestSlot.SetTransactionally(Context, SrcValue);
			}
			else
			{
				DestSlot.Set(Context, SrcValue);
			}
		}
	});
}

namespace
{
AUTORTFM_DISABLE Verse::FOpResult InstancePropertyValueTransactionally(Verse::FAllocationContext Context, FObjectInstancingGraph* InstanceGraph, UObject* SourceComponent, TNotNull<UObject*> CurrentValue, TNotNull<UObject*> Owner)
{
	return Context.Close([&] {
		V_RETURN(InstanceGraph->InstancePropertyValue(SourceComponent, CurrentValue, Owner));
	});
}

AUTORTFM_DISABLE Verse::FOpResult InstanceSubobjectTemplatesTransactionally(Verse::FAllocationContext Context, UStruct* Struct, TNotNull<void*> Data, const void* DefaultData, const UStruct* DefaultStruct, TNotNull<UObject*> Owner, FObjectInstancingGraph* InstanceGraph)
{
	return Context.Close([&] {
		Struct->InstanceSubobjectTemplates(Data, DefaultData, DefaultStruct, Owner, InstanceGraph);
	});
}

AUTORTFM_DISABLE Verse::FOpResult InstanceValue(Verse::FAllocationContext Context, bool bTransactional, bool bMutable, Verse::VValue Value, Verse::VValue DefaultValue, TNotNull<UObject*> Owner, FObjectInstancingGraph* InstanceGraph)
{
	if (Value.IsUObject())
	{
		UObject* SourceObject = DefaultValue.ExtractUObject();

		// NULL may have been delta-serialized at save/cook time as a transient reference (e.g. if the referenced object was not exported at save/cook time).
		// In that case, treat NULL as a sentinel to indicate that we should reset the value to default state and potentially instantiate a new unique object.
		UObject* NativeObject = Value.ExtractUObject();
		if (NativeObject == nullptr)
		{
			NativeObject = SourceObject;
		}

		if (bTransactional)
		{
			return InstancePropertyValueTransactionally(Context, InstanceGraph, SourceObject, NativeObject, Owner);
		}
		else
		{
			V_RETURN(InstanceGraph->InstancePropertyValue(SourceObject, NativeObject, Owner));
		}
	}
	else if (Verse::VNativeStruct* NativeStruct = Value.DynamicCast<Verse::VNativeStruct>())
	{
		Verse::VNativeStruct* DefaultStruct = DefaultValue.DynamicCast<Verse::VNativeStruct>();

		if (!bMutable)
		{
			NativeStruct = &NativeStruct->Duplicate(Context);
		}

		UScriptStruct* ScriptStruct = Verse::VNativeStruct::GetUScriptStruct(*NativeStruct->GetEmergentType());
		if (bTransactional)
		{
			Verse::FOpResult Result = InstanceSubobjectTemplatesTransactionally(Context, ScriptStruct, NativeStruct->GetStruct(), DefaultStruct ? DefaultStruct->GetStruct() : nullptr, ScriptStruct, Owner, InstanceGraph);
			if (!Result.IsReturn())
			{
				return Result;
			}
		}
		else
		{
			ScriptStruct->InstanceSubobjectTemplates(NativeStruct->GetStruct(), DefaultStruct ? DefaultStruct->GetStruct() : nullptr, ScriptStruct, Owner, InstanceGraph);
		}
		V_RETURN(*NativeStruct);
	}
	else if (Verse::VValueObject* Struct = Value.DynamicCast<Verse::VValueObject>(); Struct && Struct->IsStruct())
	{
		Verse::VValueObject* DefaultStruct = DefaultValue.DynamicCast<Verse::VValueObject>();

		Verse::VEmergentType& EmergentType = *Struct->GetEmergentType();
		Verse::VValueObject* DestStruct = Struct;
		if (!bMutable)
		{
			Verse::VEmergentType& NewEmergentType = EmergentType.GetOrCreateMeltTransition(Context);
			DestStruct = &Verse::VValueObject::NewUninitialized(Context, NewEmergentType);
			DestStruct->SetIsStruct();
		}

		for (auto It = EmergentType.Shape->CreateFieldsIterator(); It; ++It)
		{
			Verse::FOpResult LoadResult = Struct->LoadField(Context, *It->Key);
			V_DIE_UNLESS(LoadResult.IsReturn());

			Verse::VValue DefaultField;
			if (DefaultStruct)
			{
				Verse::FOpResult DefaultResult = DefaultStruct->LoadField(Context, *It->Key);
				V_DIE_UNLESS(DefaultResult.IsReturn());
				DefaultField = DefaultResult.Value;
			}

			Verse::FOpResult Result = InstanceValue(Context, bTransactional, bMutable, LoadResult.Value, DefaultField, Owner, InstanceGraph);
			if (!Result.IsReturn())
			{
				return Result;
			}

			Verse::FOpResult StoreResult = DestStruct->SetField(Context, *It->Key, Result.Value);
			V_DIE_UNLESS(StoreResult.IsReturn());
		}
		V_RETURN(*DestStruct);
	}
	else if (Verse::VArrayBase* Array = Value.DynamicCast<Verse::VArrayBase>())
	{
		Verse::VArrayBase* DefaultArray = DefaultValue.DynamicCast<Verse::VArrayBase>();

		// Non-VValue arrays cannot hold elements that need instancing.
		if (Array->GetArrayType() != Verse::EArrayType::VValue)
		{
			V_RETURN(*Array);
		}

		uint32 ArrayNum = Array->Num();
		Verse::VArrayBase* DestArray = Array;
		if (!bMutable)
		{
			DestArray = &Verse::VArray::New(Context, ArrayNum, Verse::EArrayType::VValue);
		}

		bool bUnchanged = true;
		uint32 DefaultArrayNum = DefaultArray ? DefaultArray->Num() : 0;
		for (int32 Index = 0; Index < ArrayNum; ++Index)
		{
			Verse::VValue DefaultElement = Index < DefaultArrayNum ? DefaultArray->GetValue(Index) : Verse::VValue();
			Verse::FOpResult Result = InstanceValue(Context, bTransactional, bMutable, Array->GetValue(Index), DefaultElement, Owner, InstanceGraph);
			if (!Result.IsReturn())
			{
				return Result;
			}
			DestArray->SetValue(Context, Index, Result.Value);

			bUnchanged &= Result.Value == Array->GetValue(Index);
		}
		if (bUnchanged)
		{
			V_RETURN(*Array);
		}
		V_RETURN(*DestArray);
	}
	else if (Verse::VMapBase* Map = Value.DynamicCast<Verse::VMapBase>())
	{
		Verse::VMapBase* DefaultMap = DefaultValue.DynamicCast<Verse::VMapBase>();

		Verse::VMapBase* DestMap = nullptr;
		if (bMutable)
		{
			// TODO: Mutate in-place? Can we mutate keys in place like FMapProperty does for TMap?
			DestMap = &Verse::VMapBase::New<Verse::VMutableMap>(Context, Map->Capacity);
		}
		else
		{
			DestMap = &Verse::VMapBase::New<Verse::VMap>(Context, Map->Capacity);
		}

		bool bUnchanged = true;
		for (TPair<Verse::VValue, Verse::VValue> Pair : *Map)
		{
			Verse::VMapBase::PairType* DefaultPair = nullptr;
			if (DefaultMap)
			{
				Verse::VMapBase::SequenceType Slot;
				if (DefaultMap->FindWithSlot(Context, Pair.Key, &Slot))
				{
					DefaultPair = &DefaultMap->GetPairTable()[Slot];
				}
			}

			Verse::FOpResult KeyResult = InstanceValue(Context, bTransactional, bMutable, Pair.Key, DefaultPair ? DefaultPair->Key.Follow() : Verse::VValue(), Owner, InstanceGraph);
			if (!KeyResult.IsReturn())
			{
				return KeyResult;
			}
			Verse::FOpResult ValueResult = InstanceValue(Context, bTransactional, bMutable, Pair.Value, DefaultPair ? DefaultPair->Value.Follow() : Verse::VValue(), Owner, InstanceGraph);
			if (!ValueResult.IsReturn())
			{
				return ValueResult;
			}
			DestMap->Add(Context, KeyResult.Value, ValueResult.Value);

			bUnchanged &= KeyResult.Value == Pair.Key;
			bUnchanged &= ValueResult.Value == Pair.Value;
		}
		if (bUnchanged)
		{
			V_RETURN(*Map);
		}
		V_RETURN(*DestMap);
	}
	else if (Verse::VOption* Option = Value.DynamicCast<Verse::VOption>())
	{
		Verse::VOption* DefaultOption = DefaultValue.DynamicCast<Verse::VOption>();

		// The value `true` is a singleton `option{false}`, which should never be cloned.
		// (The value `false` is a singleton VFalse, which this function already ignores.)
		if (Option == &Verse::GlobalTrue())
		{
			V_RETURN(*Option);
		}

		Verse::VOption* DestOption = Option;
		if (!bMutable)
		{
			DestOption = &Verse::VOption::New(Context, Verse::VValue());
		}

		Verse::FOpResult Result = InstanceValue(Context, bTransactional, bMutable, Option->GetValue(), DefaultOption ? DefaultOption->GetValue() : Verse::VValue(), Owner, InstanceGraph);
		if (!Result.IsReturn())
		{
			return Result;
		}
		DestOption->SetValue(Context, Result.Value);
		if (Result.Value == Option->GetValue())
		{
			V_RETURN(*Option);
		}
		V_RETURN(*DestOption);
	}
	else if (Verse::VFunction* Function = Value.DynamicCast<Verse::VFunction>())
	{
		Verse::VFunction* DefaultFunction = DefaultValue.DynamicCast<Verse::VFunction>();

		Verse::FOpResult Result = InstanceValue(Context, bTransactional, bMutable, Function->Self.Get(), DefaultFunction ? DefaultFunction->Self.Get() : Verse::VValue(), Owner, InstanceGraph);
		if (!Result.IsReturn())
		{
			return Result;
		}
		Verse::VValue Self = Result.Value;
		if (Self == Function->Self.Get())
		{
			V_RETURN(*Function);
		}
		V_RETURN(Verse::VFunction::New(Context, Function->Procedure.Get(), Self, Function->ParentScope.Get()));
	}
	else if (Value.IsPlaceholder())
	{
		return {Verse::FOpResult::Block, Value};
	}
	else
	{
		V_RETURN(Value);
	}
}
}

void FVRestValueProperty::InstanceSubobjects(void* Data, const void* DefaultData, TNotNull<UObject*> Owner, FObjectInstancingGraph* InstanceGraph)
{
	const bool bTransactional = AutoRTFM::IsClosed();
	AutoRTFM::Open([&] AUTORTFM_DISABLE {
		Verse::FRunningContext Context = Verse::FRunningContextPromise{};
		for (int32 Index = 0; Index < ArrayDim; Index++)
		{
			Verse::VRestValue& Slot = TTypeFundamentals::GetPropertyValuePtr(Data)[Index];
			Verse::VRestValue* DefaultSlot = DefaultData ? &TTypeFundamentals::GetPropertyValuePtr(const_cast<void*>(DefaultData))[Index] : nullptr;

			if (Slot.IsUninitialized())
			{
				continue;
			}

			Verse::VValue Value = Slot.Get(Context);
			Verse::VValue DefaultValue = DefaultSlot ? DefaultSlot->Get(Context) : Verse::VValue();
			if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
			{
				Verse::VRef* DefaultRef = DefaultValue.DynamicCast<Verse::VRef>();

				V_DIE_IF(Ref->GetDomain());
				Verse::VRef& DestRef = Verse::VRef::New(Context, {});

				Verse::VValue MeltedValue = Verse::VValue::Melt(Context, Ref->Get(Context));
				V_DIE_IF(MeltedValue.IsPlaceholder());

				Verse::FOpResult Result = InstanceValue(Context, bTransactional, /*bMutable*/ true, MeltedValue, DefaultRef ? DefaultRef->Get(Context) : Verse::VValue(), Owner, InstanceGraph);
				if (!Result.IsReturn())
				{
					V_DIE_UNLESS(Result.IsError());
					return;
				}
				DestRef.SetNonTransactionally(Context, Result.Value);
				Value = DestRef;
			}
			else
			{
				Verse::FOpResult Result = InstanceValue(Context, bTransactional, /*bMutable*/ false, Value, DefaultValue, Owner, InstanceGraph);
				if (!Result.IsReturn())
				{
					V_DIE_UNLESS(Result.IsError());
					return;
				}
				Value = Result.Value;
			}

			if (Value.IsCell() && GUObjectArray.IsDisregardForGC(Owner))
			{
				Value.AsCell().AddRef(Context);
			}
			if (bTransactional)
			{
				Slot.SetTransactionally(Context, Value);
			}
			else
			{
				Slot.Set(Context, Value);
			}
		}
	});
}

template <typename T>
FString TFVersePropertyBase<T>::GetCPPMacroType(FString& ExtendedTypeText) const
{
	ExtendedTypeText = FString();
	return FString();
}

template <typename T>
bool TFVersePropertyBase<T>::Identical(TNotNull<const void*> A, const void* B, uint32 PortFlags) const
{
	if (nullptr == B) // if the comparand is NULL, we just call this no-match
	{
		return false;
	}

	const TCppType* Lhs = reinterpret_cast<const TCppType*>(NotNullGet(A));
	const TCppType* Rhs = reinterpret_cast<const TCppType*>(B);
	return *Lhs == *Rhs;
}

template <typename T>
void TFVersePropertyBase<T>::SerializeItem(FStructuredArchive::FSlot Slot, TNotNull<void*> Value, void const* Defaults) const
{
	Verse::FRunningContext Context = Verse::FRunningContextPromise{};
	Verse::FStructuredArchiveVisitor Visitor(Context, Slot);
	Visitor.Visit(*static_cast<TCppType*>(NotNullGet(Value)), TEXT(""));
}

template <typename T>
void TFVersePropertyBase<T>::ExportText_Internal(FString& ValueStr, TNotNull<const void*> PropertyValueOrContainer, EPropertyPointerType PointerType, const void* DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const
{
	check(false);
	return;
}

template <typename T>
const TCHAR* TFVersePropertyBase<T>::ImportText_Internal(const TCHAR* Buffer, TNotNull<void*> ContainerOrPropertyPtr, EPropertyPointerType PropertyPointerType, UObject* OwnerObject, int32 PortFlags, FOutputDevice* ErrorText) const
{
	check(false);
	return TEXT("");
}

template <typename T>
bool TFVersePropertyBase<T>::ContainsObjectReference(TArray<const FStructProperty*>& EncounteredStructProps, EPropertyObjectReferenceType InReferenceType/* = EPropertyObjectReferenceType::Strong*/) const
{
	return true;
}

template <typename T>
void TFVersePropertyBase<T>::EmitReferenceInfo(UE::GC::FSchemaBuilder& Schema, int32 BaseOffset, TArray<const FStructProperty*>& EncounteredStructProps, UE::GC::FPropertyStack& DebugPath)
{
	for (int32 Idx = 0, Num = FProperty::ArrayDim; Idx < Num; ++Idx)
	{
		Schema.Add(UE::GC::DeclareMember(DebugPath, BaseOffset + FProperty::GetOffset_ForGC() + Idx * sizeof(TCppType), UE::GC::EMemberType::VerseValue));
	}
}

TSharedPtr<FJsonValue> VersePropertyToJSON(Verse::FRunningContext Context, const FProperty* Property, const void* InValue, TMap<const void*, Verse::EVisitState>& VisitedObjects, Verse::FToJsonCallback Callback, const uint32 RecursionDepth)
{
	if (const FVRestValueProperty* VRestValueProp = CastField<FVRestValueProperty>(Property))
	{
		const Verse::VRestValue* RestValue = static_cast<const Verse::VRestValue*>(InValue);
		return RestValue->ToJSON(Context, Verse::EValueJSONFormat::Analytics, VisitedObjects, Callback, RecursionDepth + 1);
	}
	if (const FVValueProperty* VValueProp = CastField<FVValueProperty>(Property))
	{
		const Verse::TWriteBarrier<Verse::VValue>* Value = static_cast<const Verse::TWriteBarrier<Verse::VValue>*>(InValue);
		return Value->Get().ToJSON(Context, Verse::EValueJSONFormat::Analytics, VisitedObjects, Callback, RecursionDepth + 1);
	}
	if (const FVCellProperty* VCellProp = CastField<FVCellProperty>(Property))
	{
		const Verse::TWriteBarrier<Verse::VCell>* Cell = static_cast<const Verse::TWriteBarrier<Verse::VCell>*>(InValue);
		return Cell->Get()->ToJSON(Context, Verse::EValueJSONFormat::Analytics, VisitedObjects, Callback, RecursionDepth + 1);
	}
	V_DIE("Could not convert Verse property to JSON - Unknown property type!");
}

IMPLEMENT_FIELD(FVCellProperty)
IMPLEMENT_FIELD(FVValueProperty)
IMPLEMENT_FIELD(FVRestValueProperty)

#endif // WITH_VERSE_VM
