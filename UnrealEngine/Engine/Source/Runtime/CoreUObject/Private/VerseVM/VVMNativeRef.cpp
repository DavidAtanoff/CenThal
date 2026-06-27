// Copyright Epic Games, Inc. All Rights Reserved.

#if !WITH_VERSE_BPVM || defined(__INTELLISENSE__)

#include "VerseVM/VVMNativeRef.h"

#include "AutoRTFM.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/VerseClassProperty.h"
#include "UObject/VerseStringProperty.h"
#include "UObject/VerseValueProperty.h"
#include "VerseVM/Inline/VVMCellInline.h"
#include "VerseVM/Inline/VVMEnumerationInline.h"
#include "VerseVM/Inline/VVMMarkStackVisitorInline.h"
#include "VerseVM/Inline/VVMObjectInline.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMShapeInline.h"
#include "VerseVM/VBPVMRuntimeType.h"
#include "VerseVM/VVMCVars.h"
#include "VerseVM/VVMNativeConverter.h"
#include "VerseVM/VVMNativeRational.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMRuntimeError.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMVerseEnum.h"
#include "VerseVM/VVMVerseStruct.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{

DEFINE_DERIVED_VCPPCLASSINFO(VNativeRef);
TGlobalTrivialEmergentTypePtr<&VNativeRef::StaticCppClassInfo> VNativeRef::GlobalTrivialEmergentType;

TPair<void*, FProperty*> VNativeRef::GetData(FAllocationContext Context)
{
	if (UObject* Object = Base.Get().ExtractUObject())
	{
		V_DIE_UNLESS(Type == EType::FProperty);
		return {Object, UProperty};
	}
	else if (VNativeStruct* Struct = Base.Get().DynamicCast<VNativeStruct>())
	{
		V_DIE_UNLESS(Type == EType::FProperty);
		return {Struct->GetStruct(), UProperty};
	}
	else if (VNativeRef* Ref = Base.Get().DynamicCast<VNativeRef>())
	{
		void* Container;
		FProperty* Property;
		Tie(Container, Property) = Ref->GetData(Context);
		if (Container == nullptr)
		{
			return {nullptr, nullptr};
		}

		if (Type == EType::FProperty)
		{
			return {Property->ContainerPtrToValuePtr<void>(Container), UProperty};
		}
		else if (Type == EType::Index)
		{
			if (Epoch != Context.GetWriteEpoch())
			{
				Context.RaiseVerseRuntimeError(ERuntimeDiagnostic::ErrRuntime_InvalidRef, FText::FromString("Potentially-invalidated native ref"));
				return {nullptr, nullptr};
			}

			if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				FScriptArrayHelper_InContainer NativeValue(ArrayProperty, Container);
				return {NativeValue.GetRawPtr(Index), ArrayProperty->Inner};
			}
			else if (FVerseStringProperty* StringProperty = CastField<FVerseStringProperty>(Property))
			{
				// Use const indexing even for mutable access to avoid an open copy-on-write.
				const FNativeString* NativeValue = StringProperty->ContainerPtrToValuePtr<FNativeString>(Container);
				const FNativeString::ElementType& Data = (*NativeValue)[Index];
				return {&const_cast<FNativeString::ElementType&>(Data), StringProperty->Inner};
			}
			else if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
			{
				FScriptMapHelper_InContainer NativeValue(MapProperty, Container);
				return {NativeValue.GetPairPtr(Index), MapProperty->ValueProp};
			}
		}
	}
	VERSE_UNREACHABLE();
}

FOpResult VNativeRef::Call(FAllocationContext Context, VValue Argument, bool bSet)
{
	void* Container;
	FProperty* Property;
	Tie(Container, Property) = VNativeRef::GetData(Context);
	if (Container == nullptr)
	{
		return {FOpResult::Error};
	}

	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper_InContainer NativeValue(ArrayProperty, Container);
		if (Argument.IsInt32() && NativeValue.IsValidIndex(Argument.AsInt32()))
		{
			V_RETURN(VNativeRef::New(Context, this, Argument.AsInt32()));
		}
		else
		{
			return {FOpResult::Fail};
		}
	}
	else if (FVerseStringProperty* StringProperty = CastField<FVerseStringProperty>(Property))
	{
		FNativeString* NativeValue = StringProperty->ContainerPtrToValuePtr<FNativeString>(Container);
		if (Argument.IsInt32() && NativeValue->IsValidIndex(Argument.AsInt32()))
		{
			V_RETURN(VNativeRef::New(Context, this, Argument.AsInt32()));
		}
		else
		{
			return {FOpResult::Fail};
		}
	}
	else if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		void* Key = MapProperty->KeyProp->AllocateAndInitializeValue();
		ON_SCOPE_EXIT
		{
			MapProperty->KeyProp->DestroyAndFreeValue(Key);
		};

		FOpResult Result = VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, Key, MapProperty->KeyProp, Argument);
		if (!Result.IsReturn())
		{
			return Result;
		}

		FScriptMapHelper_InContainer NativeValue(MapProperty, Container);
		int32 InternalIndex = NativeValue.FindMapPairIndexFromHash(Key);
		if (bSet && InternalIndex == INDEX_NONE)
		{
			// Inserting into a TMap may invalidate VNativeRefs to its elements.
			Context.BumpWriteEpoch();

			AutoRTFM::ETransactionStatus Status = AutoRTFM::Close([&NativeValue, Key] { NativeValue.FindOrAdd(Key); });
			if (Status != AutoRTFM::ETransactionStatus::Executing)
			{
				Context.RaiseVerseRuntimeError(ERuntimeDiagnostic::ErrRuntime_NativeInternal, FText::FromString("Closed write to native field did not yield AutoRTFM::ETransactionStatus::Executing"));
				return {FOpResult::Error};
			}

			// TODO: The FScriptMapHelper API could be improved to elide this double lookup.
			InternalIndex = NativeValue.FindMapPairIndexFromHash(Key);
		}

		if (InternalIndex != INDEX_NONE)
		{
			V_RETURN(VNativeRef::New(Context, this, InternalIndex));
		}
		else
		{
			return {FOpResult::Fail};
		}
	}
	else
	{
		V_DIE("Unknown callee");
	}
}

FOpResult VNativeRef::LoadField(FAllocationContext Context, VUniqueString& FieldName)
{
	void* Container;
	FProperty* Property;
	Tie(Container, Property) = GetData(Context);
	if (Container == nullptr)
	{
		return {FOpResult::Error};
	}

	if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		VShape* Shape = nullptr;
		if (UVerseStruct* VerseStruct = Cast<UVerseStruct>(StructProperty->Struct))
		{
			Shape = &VerseStruct->Shape.Get(Context).StaticCast<VShape>();
		}
		else
		{
			Shape = GlobalProgram->LookupShape(Context, StructProperty->Struct);
		}
		V_DIE_UNLESS(Shape);

		const VShape::VEntry* Field = Shape->GetField(FieldName);
		switch (Field->Type)
		{
			case EFieldType::FProperty:
				V_RETURN(VNativeRef::New(Context, this, Field->UProperty));
			case EFieldType::FVerseProperty:
			{
				void* Data = Property->ContainerPtrToValuePtr<void>(Container);
				V_RETURN(Field->UProperty->ContainerPtrToValuePtr<VRestValue>(Data)->Get(Context));
			}
		}
	}
	VERSE_UNREACHABLE();
}

FOpResult VNativeRef::Get(FAllocationContext Context)
{
	void* Container;
	FProperty* Property;
	Tie(Container, Property) = GetData(Context);
	if (Container == nullptr)
	{
		return {FOpResult::Error};
	}
	return VNativeRef::Get(Context, Container, Property);
}

FOpResult VNativeRef::Get(FAllocationContext Context, const void* Container, FProperty* Property)
{
	VValue Value = VNativeRef::Peek(Context, Container, Property);
	if (UNLIKELY(Value.IsUninitialized()))
	{
		Context.RaiseVerseRuntimeError(ERuntimeDiagnostic::ErrRuntime_NativeInternal, FText::FromString("Null UObject encountered."));
		return {FOpResult::Error};
	}
	V_RETURN(Value);
}

VValue VNativeRef::Peek(FAllocationContext Context, const void* Container, FProperty* Property)
{
	if (FVRestValueProperty* ValueProperty = CastField<FVRestValueProperty>(Property))
	{
		// Native field with VRestValue type: any, comparable, persistable, type, function
		VRestValue* NativeValue = ValueProperty->ContainerPtrToValuePtr<VRestValue>(const_cast<void*>(Container));
		return NativeValue->Get(Context);
	}
	else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		UEnum* UeEnum = EnumProperty->GetEnum();
		if (UeEnum == StaticEnum<EVerseTrue>())
		{
			// Get value of EVerseTrue even though technically not necessary as it's always zero
			const EVerseTrue* NativeValue = EnumProperty->ContainerPtrToValuePtr<EVerseTrue>(Container);
			return FNativeConverter::ToVValue(Context, *NativeValue);
		}

		// Convert integer value to corresponding VEnumerator cell
		UVerseEnum* VerseEnum = CastChecked<UVerseEnum>(UeEnum);
		VEnumeration* Enumeration = VerseEnum->Enumeration.Get();
		V_DIE_UNLESS(EnumProperty->GetUnderlyingProperty()->IsA<FByteProperty>());
		const uint8* NativeValue = EnumProperty->ContainerPtrToValuePtr<uint8>(Container);
		return Enumeration->GetEnumeratorChecked(*NativeValue);
	}
	else if (FBoolProperty* LogicProperty = CastField<FBoolProperty>(Property))
	{
		const bool* NativeValue = LogicProperty->ContainerPtrToValuePtr<bool>(Container);
		return FNativeConverter::ToVValue(Context, *NativeValue);
	}
	else if (FInt64Property* IntProperty = CastField<FInt64Property>(Property))
	{
		const int64* NativeValue = IntProperty->ContainerPtrToValuePtr<int64>(Container);
		return FNativeConverter::ToVValue(Context, *NativeValue);
	}
	else if (FDoubleProperty* FloatProperty = CastField<FDoubleProperty>(Property))
	{
		const double* NativeValue = FloatProperty->ContainerPtrToValuePtr<double>(Container);
		return FNativeConverter::ToVValue(Context, *NativeValue);
	}
	else if (FByteProperty* CharProperty = CastField<FByteProperty>(Property))
	{
		const UTF8CHAR* NativeValue = CharProperty->ContainerPtrToValuePtr<UTF8CHAR>(Container);
		return FNativeConverter::ToVValue(Context, *NativeValue);
	}
	else if (FIntProperty* Char32Property = CastField<FIntProperty>(Property))
	{
		const UTF32CHAR* NativeValue = Char32Property->ContainerPtrToValuePtr<UTF32CHAR>(Container);
		return FNativeConverter::ToVValue(Context, *NativeValue);
	}
	else if (FClassProperty* TypeProperty = CastField<FClassProperty>(Property))
	{
		// VerseVM does not use FClassProperty or FVerseClassProperty- fall through to legacy conversion.
	}
	else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
	{
		if (ObjectProperty->PropertyClass == UStruct::StaticClass())
		{
			// VerseVM does not use FObjectProperty for types- fall through to legacy conversion.
		}
		else
		{
			if (UObject* const* NativeValue = ObjectProperty->ContainerPtrToValuePtr<UObject*>(Container); *NativeValue)
			{
				return FNativeConverter::ToVValue(Context, *NativeValue);
			}
			return VValue();
		}
	}
	else if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (StructProperty->Struct == FVerseRational::StaticStruct())
		{
			const FVerseRational* NativeValue = StructProperty->ContainerPtrToValuePtr<FVerseRational>(Container);
			return FNativeConverter::ToVValue(Context, *NativeValue);
		}

		const void* NativeValue = StructProperty->ContainerPtrToValuePtr<void>(Container);
		return VNativeRef::PeekStruct(Context, NativeValue, StructProperty->Struct);
	}
	else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper_InContainer NativeValue(ArrayProperty, Container);
		int NumElements = NativeValue.Num();
		VArray& Array = VArray::New(Context, NumElements, [Context, ArrayProperty, &NativeValue](uint32 Index) -> VValue {
			// If an error occurs, Peek will return an uninitialized VValue, which will immediately halt VArray construction.
			return VNativeRef::Peek(Context, NativeValue.GetElementPtr(Index), ArrayProperty->Inner);
		});
		if (NumElements > 0 && Array.GetArrayType() == EArrayType::None)
		{
			// If the array should have elements but has no backing store, an error occurred mid-construction.
			return VValue();
		}
		return Array;
	}
	else if (FVerseStringProperty* StringProperty = CastField<FVerseStringProperty>(Property))
	{
		const FNativeString* NativeValue = StringProperty->ContainerPtrToValuePtr<FNativeString>(Container);
		return FNativeConverter::ToVValue(Context, *NativeValue);
	}
	else if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		FScriptMapHelper_InContainer NativeValue(MapProperty, Container);
		FScriptMapHelper::FIterator Iter = NativeValue.CreateIterator();
		int NumElements = NativeValue.Num();
		VMapBase& Map = VMapBase::New<VMap>(Context, NumElements, [Context, MapProperty, &NativeValue, &Iter](uint32 I) -> TPair<VValue, VValue> {
			void* Data = NativeValue.GetPairPtr(Iter);
			++Iter;
			// If an error occurs, Peek will return an uninitialized VValue, which will immediately halt VMap construction.
			VValue EntryKey = VNativeRef::Peek(Context, Data, MapProperty->KeyProp);
			VValue EntryValue = VNativeRef::Peek(Context, Data, MapProperty->ValueProp);
			return {EntryKey, EntryValue};
		});
		if (NumElements > 0 && Map.Num() == 0)
		{
			// If the map should have elements but is actually empty, an error occurred mid-construction.
			return VValue();
		}
		return Map;
	}
	else if (FOptionalProperty* OptionProperty = CastField<FOptionalProperty>(Property))
	{
		const void* NativeValue = OptionProperty->ContainerPtrToValuePtr<void>(Container);
		if (OptionProperty->IsSet(NativeValue))
		{
			VValue Inner = VNativeRef::Peek(Context, NativeValue, OptionProperty->GetValueProperty());
			if (Inner.IsUninitialized())
			{
				return Inner;
			}
			return VOption::New(Context, Inner);
		}
		else
		{
			return GlobalFalse();
		}
	}

	// We couldn't handle this type
#if WITH_EDITORONLY_DATA
	// See if it's a legacy type
	return FVRestValueProperty::ConvertToVValueLegacy(Context, Container, Property);
#else
	VERSE_UNREACHABLE();
#endif
}

FOpResult VNativeRef::GetStruct(FAllocationContext Context, const void* Data, UScriptStruct* Struct)
{
	VValue Value = VNativeRef::PeekStruct(Context, Data, Struct);
	if (Value.IsUninitialized())
	{
		Context.RaiseVerseRuntimeError(ERuntimeDiagnostic::ErrRuntime_NativeInternal, FText::FromString("Null UObject encountered."));
		return {FOpResult::Error};
	}
	V_RETURN(Value);
}

VValue VNativeRef::PeekStruct(FAllocationContext Context, const void* Data, UScriptStruct* Struct)
{
	VClass* Class = nullptr;
	VShape* Shape = nullptr;
	if (UVerseStruct* UeStruct = Cast<UVerseStruct>(Struct))
	{
		if (UeStruct->IsTuple())
		{
			uint32 NumElements = 0;
			for (TFieldIterator<FProperty> Counter(UeStruct); Counter; ++Counter)
			{
				++NumElements;
			}
			TFieldIterator<FProperty> Iterator(UeStruct);
			// We assume here that the element initializer gets invoked in ascending index order.
			VArray& Array = VArray::New(Context, NumElements, [Context, Data, &Iterator](uint32 Index) {
				// If an error occurs, Peek will return an uninitialized VValue, which will immediately halt VArray construction.
				VValue TupleElem = VNativeRef::Peek(Context, Data, *Iterator);
				++Iterator;
				return TupleElem;
			});
			if (NumElements > 0 && Array.GetArrayType() == EArrayType::None)
			{
				// If the array should have elements but has no backing store, an error occurred mid-construction.
				return VValue();
			}
			return Array;
		}

		Class = UeStruct->Class.Get();
		Shape = &UeStruct->Shape.Get(Context).StaticCast<VShape>();
	}
	else
	{
		VNamedType* ImportedType = GlobalProgram->LookupImport(Context, Struct);
		if (ImportedType)
		{
			Class = &ImportedType->StaticCast<VClass>();
		}
	}

	V_DIE_UNLESS(Class);

	if (Class->IsNativeRepresentation())
	{
		VEmergentType& EmergentType = Class->GetOrCreateEmergentTypeForNativeStruct(Context);
		VNativeStruct& NativeStruct = VNativeStruct::NewUninitialized(Context, EmergentType);
		bool const bCopyInClosed = AutoRTFM::IsTransactional() && !CVarUObjectLeniency.GetValueOnAnyThread();
		FOpResult Result = Context.CloseIf(bCopyInClosed, [&] {
			Struct->CopyScriptStruct(NativeStruct.GetStruct(), Data);
		});
		V_DIE_UNLESS(Result.IsReturn());
		return NativeStruct;
	}

	V_DIE_UNLESS(Shape); // Must have a shape at this point
	TArray<VArchetype::VEntry> ArchetypeEntries;
	ArchetypeEntries.Reserve(Shape->GetNumFields());
	for (VShape::FieldsMap::TIterator ShapeEntry = Shape->CreateFieldsIterator(); ShapeEntry; ++ShapeEntry)
	{
		ArchetypeEntries.Add(VArchetype::VEntry::ObjectField(Context, *ShapeEntry->Key));
	}
	VArchetype& Archetype = VArchetype::New(Context, VValue(), ArchetypeEntries);
	VValueObject& ValueStruct = Class->NewVObject(Context, Archetype);
	for (VShape::FieldsMap::TIterator ShapeEntry = Shape->CreateFieldsIterator(); ShapeEntry; ++ShapeEntry)
	{
		bool bCreated = ValueStruct.CreateField(Context, *ShapeEntry->Key);
		V_DIE_UNLESS(bCreated);
		V_DIE_UNLESS(ShapeEntry->Value.Type == EFieldType::FProperty || ShapeEntry->Value.Type == EFieldType::FVerseProperty); // Shapes of UStructs must have only properties
		VValue Value = VNativeRef::Peek(Context, Data, ShapeEntry->Value.UProperty);
		if (Value.IsUninitialized())
		{
			return Value;
		}
		FOpResult Result = ValueStruct.SetField(Context, *ShapeEntry->Key, Value);
		V_DIE_UNLESS(Result.IsReturn());
	}
	return ValueStruct;
}

FOpResult VNativeRef::Set(FAllocationContext Context, VValue Value)
{
	void* Container;
	FProperty* Property;
	Tie(Container, Property) = GetData(Context);
	if (Container == nullptr)
	{
		return {FOpResult::Error};
	}
	// Note: if/when we allow the GC to terminate during transactions, this must pass the base UObject or VNativeStruct.
	return Set<EWriteMode::Transactional>(Context, nullptr, Container, Property, Value);
}

FOpResult VNativeRef::SetNonTransactionally(FAllocationContext Context, VValue Value)
{
	void* Container;
	FProperty* Property;
	Tie(Container, Property) = GetData(Context);
	if (Container == nullptr)
	{
		return {FOpResult::Error};
	}
	// Note: if/when we allow the GC to terminate during transactions, this must pass the base UObject or VNativeStruct.
	return Set<EWriteMode::NonTransactional>(Context, nullptr, Container, Property, Value);
}

template FOpResult VNativeRef::Set<EWriteMode::Transactional>(FAllocationContext Context, UObject* Base, void* Container, FProperty* Property, VValue Value);
template FOpResult VNativeRef::Set<EWriteMode::Transactional>(FAllocationContext Context, VNativeStruct* Base, void* Container, FProperty* Property, VValue Value);
template FOpResult VNativeRef::Set<EWriteMode::NonTransactional>(FAllocationContext Context, std::nullptr_t Base, void* Container, FProperty* Property, VValue Value);

#define OP_RESULT_HELPER(Result) \
	if (!Result.IsReturn())      \
	{                            \
		return Result;           \
	}

namespace
{
template <EWriteMode WriteMode, typename FunctionType>
AUTORTFM_DISABLE FOpResult WriteImpl(FAllocationContext Context, AUTORTFM_IMPLICIT_ENABLE FunctionType F)
{
	if constexpr (WriteMode == EWriteMode::Transactional)
	{
		AutoRTFM::ETransactionStatus Status = AutoRTFM::Close(F);
		if (Status != AutoRTFM::ETransactionStatus::Executing)
		{
			Context.RaiseVerseRuntimeError(ERuntimeDiagnostic::ErrRuntime_NativeInternal, FText::FromString("Closed write to native field did not yield AutoRTFM::ETransactionStatus::Executing"));
			return {FOpResult::Error};
		}
	}
	else
	{
		F();
	}

	return {FOpResult::Return};
}

template <EWriteMode WriteMode, typename ValueType>
AUTORTFM_DISABLE FOpResult Assign(FAllocationContext Context, ValueType* ValuePtr, ValueType NativeValue)
{
	if constexpr (WriteMode == EWriteMode::Transactional)
	{
		AutoRTFM::Assign(*ValuePtr, NativeValue);
	}
	else
	{
		*ValuePtr = NativeValue;
	}
	return {FOpResult::Return};
}

template <EWriteMode WriteMode, typename ValueType>
AUTORTFM_DISABLE FOpResult SetImpl(FAllocationContext Context, void* Container, FProperty* Property, VValue Value)
{
	TFromVValue<ValueType> NativeValue;
	FOpResult Result = FNativeConverter::FromVValue(Context, Value, NativeValue);
	OP_RESULT_HELPER(Result);

	ValueType* ValuePtr = Property->ContainerPtrToValuePtr<ValueType>(Container);
	return Assign<WriteMode>(Context, ValuePtr, NativeValue.GetValue());
}
} // namespace

template <EWriteMode WriteMode, typename BaseType>
FOpResult VNativeRef::Set(FAllocationContext Context, BaseType Base, void* Container, FProperty* Property, VValue Value)
{
	if (FVRestValueProperty* ValueProperty = CastField<FVRestValueProperty>(Property))
	{
		// Native field with VRestValue type: any, comparable, persistable, type, function
		VRestValue* NativeValue = ValueProperty->ContainerPtrToValuePtr<VRestValue>(const_cast<void*>(Container));
		NativeValue->Set<WriteMode>(Context, Value);
		return {FOpResult::Return};
	}
	else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		UEnum* UeEnum = EnumProperty->GetEnum();
		if (UeEnum == StaticEnum<EVerseTrue>())
		{
			return SetImpl<WriteMode, EVerseTrue>(Context, Container, EnumProperty, Value);
		}

		V_REQUIRE_CONCRETE(Value);
		V_DIE_UNLESS(Value.IsCellOfType<VEnumerator>() && EnumProperty->GetUnderlyingProperty()->IsA<FByteProperty>());

		VEnumerator& Enumerator = Value.StaticCast<VEnumerator>();
		uint8 NativeValue = static_cast<uint8>(Enumerator.GetIntValue());
		if (NativeValue != Enumerator.GetIntValue())
		{
			Context.RaiseVerseRuntimeError(ERuntimeDiagnostic::ErrRuntime_NativeInternal, FText::FromString("Native enumerators must be integers between 0 and 255"));
			return {FOpResult::Error};
		}
		uint8* ValuePtr = EnumProperty->ContainerPtrToValuePtr<uint8>(Container);
		return Assign<WriteMode>(Context, ValuePtr, NativeValue);
	}
	else if (FBoolProperty* LogicProperty = CastField<FBoolProperty>(Property))
	{
		return SetImpl<WriteMode, bool>(Context, Container, LogicProperty, Value);
	}
	if (FInt64Property* IntProperty = CastField<FInt64Property>(Property))
	{
		return SetImpl<WriteMode, int64>(Context, Container, IntProperty, Value);
	}
	else if (FDoubleProperty* FloatProperty = CastField<FDoubleProperty>(Property))
	{
		return SetImpl<WriteMode, double>(Context, Container, FloatProperty, Value);
	}
	else if (FByteProperty* CharProperty = CastField<FByteProperty>(Property))
	{
		return SetImpl<WriteMode, UTF8CHAR>(Context, Container, CharProperty, Value);
	}
	else if (FIntProperty* Char32Property = CastField<FIntProperty>(Property))
	{
		return SetImpl<WriteMode, UTF32CHAR>(Context, Container, Char32Property, Value);
	}
	else if (FClassProperty* TypeProperty = CastField<FClassProperty>(Property))
	{
		// VerseVM does not use FClassProperty or FVerseClassProperty- fall through to legacy conversion.
	}
	else if (FObjectProperty* ClassProperty = CastField<FObjectProperty>(Property))
	{
		if (ClassProperty->PropertyClass == UStruct::StaticClass())
		{
			// VerseVM does not use FObjectProperty for types- fall through to legacy conversion.
		}
		else
		{
			// Convert as TNonNullPtr<UObject> but write as TNonNullPtr<TObjectPtr<UObject>> for the write barrier.

			TFromVValue<TNonNullPtr<UObject>> NativeValue;
			FOpResult Result = FNativeConverter::FromVValue(Context, Value, NativeValue);
			OP_RESULT_HELPER(Result);

			return WriteImpl<WriteMode>(Context, [ClassProperty, Container, &NativeValue] {
				TNonNullPtr<TObjectPtr<UObject>>* ValuePtr = ClassProperty->template ContainerPtrToValuePtr<TNonNullPtr<TObjectPtr<UObject>>>(Container);
				*ValuePtr = NativeValue.GetValue().Get();
			});
		}
	}
	else if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		V_REQUIRE_CONCRETE(Value);

		if (StructProperty->Struct == FVerseRational::StaticStruct())
		{
			return SetImpl<WriteMode, FVerseRational>(Context, Container, StructProperty, Value);
		}

		void* ValuePtr = StructProperty->ContainerPtrToValuePtr<void>(Container);
		auto WriteStruct = [Context, StructProperty, ValuePtr](const auto& F) -> FOpResult {
			UScriptStruct* ScriptStruct = StructProperty->Struct;

			// Write the converted struct into temporary storage.
			TArray<std::byte, TInlineAllocator<64>> TempStorage;
			TempStorage.AddUninitialized(ScriptStruct->GetStructureSize() + ScriptStruct->GetMinAlignment() - 1);
			std::byte* AlignedTempStorage = Align(TempStorage.GetData(), ScriptStruct->GetMinAlignment());
			StructProperty->InitializeValue(AlignedTempStorage);
			FOpResult InitResult = F(AlignedTempStorage);
			ON_SCOPE_EXIT
			{
				StructProperty->DestroyValue(AlignedTempStorage);
			};
			OP_RESULT_HELPER(InitResult);

			// Copy the converted struct to the final destination, transactionally if necessary.
			return WriteImpl<WriteMode>(Context, [StructProperty, ValuePtr, AlignedTempStorage] {
				StructProperty->CopyCompleteValue(ValuePtr, AlignedTempStorage);
			});
		};

		VClass* Class = nullptr;
		VShape* Shape = nullptr;
		if (UVerseStruct* VerseStruct = Cast<UVerseStruct>(StructProperty->Struct))
		{
			if (VerseStruct->IsTuple())
			{
				VArrayBase& Array = Value.StaticCast<VArrayBase>();
				return WriteStruct([Context, &Array, VerseStruct](void* Dest) -> FOpResult {
					TFieldIterator<FProperty> Iterator(VerseStruct);
					for (int32 Index = 0; Index < Array.Num(); ++Index, ++Iterator)
					{
						FOpResult ElemResult = VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, Dest, *Iterator, Array.GetValue(Index));
						OP_RESULT_HELPER(ElemResult);
					}
					return {FOpResult::Return};
				});
			}

			Class = VerseStruct->Class.Get();
			Shape = &VerseStruct->Shape.Get(Context).StaticCast<VShape>();
		}
		else
		{
			VNamedType* ImportedType = GlobalProgram->LookupImport(Context, StructProperty->Struct);
			V_DIE_UNLESS(ImportedType);
			Class = &ImportedType->StaticCast<VClass>();
		}

		if (Class->IsNativeRepresentation())
		{
			// Conservatively assume the struct may contain a TArray or TMap.
			Context.BumpWriteEpoch();

			VNativeStruct& NativeStruct = Value.StaticCast<VNativeStruct>();
			checkSlow(VNativeStruct::GetUScriptStruct(*NativeStruct.GetEmergentType()) == StructProperty->Struct);

			return WriteImpl<WriteMode>(Context, [StructProperty, ValuePtr, &NativeStruct] {
				StructProperty->CopyCompleteValue(ValuePtr, NativeStruct.GetStruct());
			});
		}

		V_DIE_UNLESS(Shape);
		VValueObject& ValueStruct = Value.StaticCast<VValueObject>();
		return WriteStruct([Context, &ValueStruct, StructProperty, Shape](void* Dest) -> FOpResult {
			for (auto ShapeEntry = Shape->CreateFieldsIterator(); ShapeEntry; ++ShapeEntry)
			{
				FOpResult LoadResult = ValueStruct.LoadField(Context, *ShapeEntry->Key);
				OP_RESULT_HELPER(LoadResult);
				V_DIE_UNLESS(ShapeEntry->Value.Type == EFieldType::FProperty || ShapeEntry->Value.Type == EFieldType::FVerseProperty);
				FOpResult WriteResult = VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, Dest, ShapeEntry->Value.UProperty, LoadResult.Value);
				OP_RESULT_HELPER(WriteResult);
			}
			return {FOpResult::Return};
		});
	}
	else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		V_REQUIRE_CONCRETE(Value);
		V_DIE_UNLESS(Value.IsCellOfType<VArrayBase>());
		VArrayBase& Array = Value.StaticCast<VArrayBase>();

		FScriptArray NativeValue;
		FScriptArrayHelper Helper(ArrayProperty, &NativeValue);
		FOpResult Result = WriteImpl<WriteMode>(Context, [&] { Helper.EmptyAndAddValues(Array.Num()); });
		OP_RESULT_HELPER(Result);
		for (int32 Index = 0; Index < Array.Num(); Index++)
		{
			FOpResult ElemResult = VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, Helper.GetElementPtr(Index), ArrayProperty->Inner, Array.GetValue(Index));
			OP_RESULT_HELPER(ElemResult);
		}

		// Overwriting a TArray may invalidate VNativeRefs to its elements.
		Context.BumpWriteEpoch();

		return WriteImpl<WriteMode>(Context, [ArrayProperty, Container, &NativeValue] {
			FScriptArrayHelper_InContainer ValuePtr(ArrayProperty, Container);
			ValuePtr.MoveAssign(&NativeValue);
		});
	}
	else if (FVerseStringProperty* StringProperty = CastField<FVerseStringProperty>(Property))
	{
		TFromVValue<FNativeString> NativeString;
		FOpResult Result = FNativeConverter::FromVValue(Context, Value, NativeString);
		OP_RESULT_HELPER(Result);

		// Overwriting an FNativeString may invalidate VNativeRefs to its elements.
		Context.BumpWriteEpoch();

		return WriteImpl<WriteMode>(Context, [StringProperty, Container, &NativeString] {
			FNativeString* ValuePtr = StringProperty->ContainerPtrToValuePtr<FNativeString>(Container);
			*ValuePtr = MoveTemp(NativeString.Value);
		});
	}
	else if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		V_REQUIRE_CONCRETE(Value);
		V_DIE_UNLESS(Value.IsCellOfType<VMapBase>());
		VMapBase& Map = Value.StaticCast<VMapBase>();

		FScriptMap NativeValue;
		FScriptMapHelper Helper(MapProperty, &NativeValue);
		FOpResult Result = WriteImpl<WriteMode>(Context, [&] { Helper.EmptyValues(Map.Num()); });
		OP_RESULT_HELPER(Result);
		for (TPair<VValue, VValue> Pair : Map)
		{
			int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
			FOpResult KeyResult = VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, Helper.GetPairPtr(Index), Helper.GetKeyProperty(), Pair.Key);
			OP_RESULT_HELPER(KeyResult);
			FOpResult ValueResult = VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, Helper.GetPairPtr(Index), Helper.GetValueProperty(), Pair.Value);
			OP_RESULT_HELPER(ValueResult);
		}
		Helper.Rehash();

		// Overwriting a TMap may invalidate VNativeRefs to its elements.
		Context.BumpWriteEpoch();

		return WriteImpl<WriteMode>(Context, [MapProperty, Container, &NativeValue] {
			FScriptMapHelper_InContainer ValuePtr(MapProperty, Container);
			ValuePtr.MoveAssign(&NativeValue);
		});
	}
	else if (FOptionalProperty* OptionProperty = CastField<FOptionalProperty>(Property))
	{
		V_REQUIRE_CONCRETE(Value);

		if (VOption* Option = Value.DynamicCast<VOption>())
		{
			void* Data;
			FOpResult Result = WriteImpl<WriteMode>(Context, [OptionProperty, Container, Value, &Data] {
				void* ValuePtr = OptionProperty->ContainerPtrToValuePtr<void>(Container);
				Data = OptionProperty->MarkSetAndGetInitializedValuePointerToReplace(ValuePtr);
			});
			OP_RESULT_HELPER(Result);

			return VNativeRef::Set<WriteMode>(Context, Base, Data, OptionProperty->GetValueProperty(), Option->GetValue());
		}
		else
		{
			V_DIE_UNLESS(Value == GlobalFalse());

			// Unsetting a TOptional may invalidate VNativeRefs to its contents.
			Context.BumpWriteEpoch();

			return WriteImpl<WriteMode>(Context, [OptionProperty, Container] {
				void* ValuePtr = OptionProperty->ContainerPtrToValuePtr<void>(Container);
				OptionProperty->MarkUnset(ValuePtr);
			});
		}
	}

	// We couldn't handle this type
#if WITH_EDITORONLY_DATA
	// See if it's a legacy type
	return WriteImpl<WriteMode>(Context, [Context, Container, Property, Value] {
		if (!FVRestValueProperty::ConvertFromVValueLegacy(Context, Container, Property, Value))
		{
			V_DIE_UNLESS(false);
		}
	});
#else
	VERSE_UNREACHABLE();
#endif
}

#undef OP_RESULT_HELPER

FOpResult VNativeRef::FreezeImpl(FAllocationContext Context, VTask* Task)
{
	return Get(Context);
}

template <typename TVisitor>
void VNativeRef::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(Base, TEXT("Base"));
}

} // namespace Verse
#endif
