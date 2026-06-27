// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMClass.h"

#include "Misc/EnumClassFlags.h"
#include "UObject/Class.h"
#include "UObject/CoreRedirects.h"
#include "UObject/GarbageCollectionSchema.h"
#include "UObject/Interface.h"
#include "UObject/ObjectInstancingGraph.h"
#include "UObject/OverridableManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/VerseValueProperty.h"
#include "VerseVM/Inline/VVMClassInline.h"
#include "VerseVM/Inline/VVMEnterVMInline.h"
#include "VerseVM/Inline/VVMMarkStackVisitorInline.h"
#include "VerseVM/Inline/VVMNativeConstructorWrapperInline.h"
#include "VerseVM/Inline/VVMNativeStructInline.h"
#include "VerseVM/Inline/VVMObjectInline.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMScopeInline.h"
#include "VerseVM/Inline/VVMShapeInline.h"
#include "VerseVM/Inline/VVMUniqueStringInline.h"
#include "VerseVM/Inline/VVMValueObjectInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMAccessor.h"
#include "VerseVM/VVMAttribute.h"
#include "VerseVM/VVMCVars.h"
#include "VerseVM/VVMEngineEnvironment.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMInstantiationContext.h"
#include "VerseVM/VVMJson.h"
#include "VerseVM/VVMLog.h"
#include "VerseVM/VVMNames.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMPersistence.h"
#include "VerseVM/VVMProcedure.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMTypeInitOrValidate.h"
#include "VerseVM/VVMValuePrinting.h"
#include "VerseVM/VVMVerse.h"
#include "VerseVM/VVMVerseClass.h"
#include "VerseVM/VVMVerseFunction.h"
#include "VerseVM/VVMVerseStruct.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{

template <typename TVisitor>
void Visit(TVisitor& Visitor, VArchetype::VEntry& Entry)
{
	Visitor.Visit(Entry.Name, TEXT("Name"));
	Visitor.Visit(Entry.Type, TEXT("Type"));
	Visitor.Visit(Entry.Value, TEXT("Value"));
	Visitor.Visit(reinterpret_cast<std::underlying_type_t<EArchetypeEntryFlags>&>(Entry.Flags), TEXT("Flags"));
}

bool VArchetype::VEntry::IsMethod() const
{
	VValue EntryValue = Value.Get();
	if (VFunction* EntryFunction = EntryValue.DynamicCast<VFunction>())
	{
		return !EntryFunction->HasSelf();
	}
	return false;
}

DEFINE_DERIVED_VCPPCLASSINFO(VArchetype);
TGlobalTrivialEmergentTypePtr<&VArchetype::StaticCppClassInfo> VArchetype::GlobalTrivialEmergentType;

template <typename TVisitor>
void VArchetype::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(Class, TEXT("Class"));
	Visitor.Visit(NextArchetype, TEXT("NextArchetype"));
	Visitor.Visit(Entries, NumEntries, TEXT("Entries"));
}

void VArchetype::SerializeLayout(FAllocationContext Context, VArchetype*& This, FStructuredArchiveVisitor& Visitor)
{
	uint32 NumEntries = 0;
	if (!Visitor.IsLoading())
	{
		NumEntries = This->NumEntries;
	}

	Visitor.Visit(NumEntries, TEXT("NumEntries"));
	if (Visitor.IsLoading())
	{
		This = &VArchetype::NewUninitialized(Context, NumEntries);
	}
}

void VArchetype::SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor)
{
	Visitor.Visit(Class, TEXT("Class"));
	Visitor.Visit(NextArchetype, TEXT("NextArchetype"));
	Visitor.Visit(Entries, NumEntries, TEXT("Entries"));
}

void VArchetype::AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth)
{
	if (IsCellFormat(Format))
	{
		Builder.Append(UTF8TEXT("\n"));
		for (uint32 Index = 0; Index < NumEntries; ++Index)
		{
			const VEntry& Entry = Entries[Index];
			Builder << UTF8TEXT("\t");
			Builder.Appendf(UTF8TEXT("UniqueString(\"%s\")"), Verse::Names::RemoveQualifier(Entry.Name->AsStringView()).GetData());
			Builder << UTF8TEXT(" : Entry(Value: ");
			Entry.Value.Get().AppendToString(Context, Builder, Format, VisitedObjects, RecursionDepth + 1);
			Builder << UTF8TEXT(", IsConstant: ");
			Builder << (Entry.IsConstant() ? UTF8TEXT("true") : UTF8TEXT("false"));
			Builder << UTF8TEXT(", HasDefaultValueExpression: ");
			Builder << (Entry.HasDefaultValueExpression() ? UTF8TEXT("true") : UTF8TEXT("false"));
			Builder << UTF8TEXT(", IsNative: ");
			Builder << (Entry.IsNativeRepresentation() ? UTF8TEXT("true") : UTF8TEXT("false"));
			Builder << UTF8TEXT("))\n");
		}
	}
}

VValue VArchetype::LoadFunction(FAllocationContext Context, VUniqueString& FieldName, VValue SelfObject)
{
	// TODO: (yiliang.siew) This should probably be improved with inline caching or a hashtable instead for constructors
	// with lots of entries.
	for (uint32 Index = 0; Index < NumEntries; ++Index)
	{
		VEntry& CurrentEntry = Entries[Index];
		if (*CurrentEntry.Name.Get() != FieldName)
		{
			continue;
		}
		// At this point `(super:)`/captures for the scope should already be filled in.
		if (VFunction* Function = Entries[Index].Value.Get().DynamicCast<VFunction>(); Function && !Function->HasSelf())
		{
			VFunction& NewFunction = Function->Bind(Context, SelfObject);
			return NewFunction;
		}
	}
	return VValue();
}

DEFINE_DERIVED_VCPPCLASSINFO(VClass)
TGlobalTrivialEmergentTypePtr<&VClass::StaticCppClassInfo> VClass::GlobalTrivialEmergentType;

template <typename TVisitor>
void VClass::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(Archetype, TEXT("Archetype"));
	Visitor.Visit(Constructor, TEXT("Constructor"));
	Visitor.Visit(Blocks, TEXT("Blocks"));
	Visitor.Visit(NativeConstructorEmergentType, TEXT("NativeConstructorEmergentType"));
	Visitor.Visit(Inherited, NumInherited, TEXT("Inherited"));

	FCellUniqueLock Lock(Mutex);
	Visitor.Visit(EmergentTypesCache, TEXT("EmergentTypesCache"));
}

void VClass::SerializeLayout(FAllocationContext Context, VClass*& This, FStructuredArchiveVisitor& Visitor)
{
	int32 NumInherited = 0;
	if (!Visitor.IsLoading())
	{
		NumInherited = This->NumInherited;
	}

	Visitor.Visit(NumInherited, TEXT("NumInherited"));
	if (Visitor.IsLoading())
	{
		size_t NumBytes = offsetof(VClass, Inherited) + NumInherited * sizeof(Inherited[0]);
		This = new (Context.Allocate(FHeap::DestructorSpace, NumBytes)) VClass(Context, NumInherited);
	}
}

void VClass::SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor)
{
	VNamedType::SerializeImpl(Context, Visitor);
	Visitor.Visit(reinterpret_cast<std::underlying_type_t<EKind>&>(Kind), TEXT("Kind"));
	Visitor.Visit(reinterpret_cast<std::underlying_type_t<EFlags>&>(Flags), TEXT("Flags"));
	Visitor.Visit(Archetype, TEXT("Archetype"));
	Visitor.Visit(Constructor, TEXT("Constructor"));
	Visitor.Visit(Blocks, TEXT("Blocks"));
	Visitor.Visit(Inherited, NumInherited, TEXT("Inherited"));
}

VClass::VClass(
	FAllocationContext Context,
	VPackage* InPackageScope,
	VArray* InPath,
	VArray* InClassName,
	VArray* InAttributeIndices,
	VArray* InAttributes,
	VValue InImportStruct,
	bool bInNativeBound,
	EKind InKind,
	EFlags InFlags,
	const TArray<VClass*>& InInherited,
	VArchetype& InArchetype,
	VProcedure& InConstructor,
	VProcedure* InBlocks)
	: VNamedType(
		Context,
		&GlobalTrivialEmergentType.Get(Context),
		InPackageScope,
		InPath,
		InClassName,
		InAttributeIndices,
		InAttributes,
		InImportStruct,
		bInNativeBound)
	, Kind(InKind)
	, Flags(InFlags)
	, NumInherited(InInherited.Num())
{
	checkSlow(!IsNativeBound() || IsNativeRepresentation());

	for (int32 Index = 0; Index < NumInherited; ++Index)
	{
		new (&Inherited[Index]) TWriteBarrier<VClass>(Context, InInherited[Index]);
	}

	// `InArchetype` is an immutable template, typically part of a module's top-level bytecode.
	// Clone it to fill out the parts that need to refer to this VClass or its superclass, which
	// generally do not exist yet during compilation when the template is produced.
	Archetype.Set(Context, VArchetype::NewUninitialized(Context, InArchetype.NumEntries));

	Archetype->Class.Set(Context, *this);

	VClass* SuperClass = nullptr;
	VValue NextArchetype = VValue();
	if (NumInherited > 0 && Inherited[0]->GetKind() == VClass::EKind::Class)
	{
		SuperClass = Inherited[0].Get();
		NextArchetype = *SuperClass->Archetype;
	}

	Archetype->NextArchetype.Set(Context, NextArchetype);

	// The class body, and methods defined within it, are bare `VProcedure`s with no `VScope` yet.
	// Give them access to the lexical scope of the class definition (currently just `(super:)`).
	// When they are eventually invoked, they will be further augmented with a `Self` value.
	VScope& ClassScope = VScope::New(Context, *Archetype);
	Constructor.Set(Context, VFunction::NewUnbound(Context, InConstructor, ClassScope));
	if (!CVarUObjectLeniency.GetValueOnAnyThread())
	{
		if (Kind == EKind::Class)
		{
			Blocks.Set(Context, VFunction::NewUnbound(Context, *InBlocks, ClassScope));
		}
	}
	for (uint32 Index = 0; Index < InArchetype.NumEntries; ++Index)
	{
		VArchetype::VEntry& CurrentEntry = *new (&Archetype->Entries[Index]) VArchetype::VEntry(InArchetype.Entries[Index]);
		if (VProcedure* CurrentProcedure = CurrentEntry.Value.Get().DynamicCast<VProcedure>())
		{
			CurrentEntry.Value.Set(Context, VFunction::NewUnbound(Context, CurrentEntry.Value.Get(), ClassScope));
		}
		else if (VNativeProcedure* CurrentNativeProcedure = CurrentEntry.Value.Get().DynamicCast<VNativeProcedure>())
		{
			CurrentEntry.Value.Set(Context, VFunction::NewUnbound(Context, CurrentEntry.Value.Get(), ClassScope));
		}
	}
}

VClass::VClass(FAllocationContext Context, int32 InNumInherited)
	: VNamedType(Context, &GlobalTrivialEmergentType.Get(Context), nullptr, nullptr, nullptr, nullptr, nullptr, VValue(), false)
	, NumInherited(InNumInherited)
{
	for (int32 Index = 0; Index < NumInherited; ++Index)
	{
		new (&Inherited[Index]) TWriteBarrier<VClass>{};
	}
}

VValueObject& VClass::NewVObject(FAllocationContext Context, VArchetype& InArchetype)
{
	V_DIE_IF(IsNativeRepresentation());

	VEmergentType& NewEmergentType = GetOrCreateEmergentTypeForVObject(Context, &VValueObject::StaticCppClassInfo, InArchetype);
	return NewVObjectOfEmergentType(Context, NewEmergentType);
}

VValueObject& VClass::NewVObjectOfEmergentType(FAllocationContext Context, VEmergentType& EmergentType)
{
	VValueObject& NewObject = VValueObject::NewUninitialized(Context, EmergentType);

	if (Kind == EKind::Struct)
	{
		NewObject.SetIsStruct();
	}

	return NewObject;
}

VNativeConstructorWrapper& VClass::NewNativeStruct(FAllocationContext Context)
{
	V_DIE_UNLESS(IsNativeStruct());

	VEmergentType& EmergentType = GetOrCreateEmergentTypeForNativeStruct(Context);
	VNativeStruct& NewObject = VNativeStruct::NewUninitialized(Context, EmergentType);
	return VNativeConstructorWrapper::New(Context, *this, NewObject);
}

namespace
{
// Generate a unique FName the like MakeUniqueObjectName, but consistent across content worker and server.
// Does not try to support all the options or behaviors of MakeUniqueObjectName- just enough to let Verse
// packages compiled on the server match Verse packages compiled in the content worker.
FName MakeUniqueObjectNameForDefaultSubObject(UObject* Parent, UClass* Class)
{
	FName BaseName = Class->GetFName();

	FName TestName;
	UObject* ExistingObject;
	do
	{
		int32 NameNumber = UpdateSuffixForNextNewObject(Parent, Class, [](int32& Index) { ++Index; });
		TestName = FName(BaseName, NameNumber);
		ExistingObject = StaticFindObjectFastInternal(nullptr, Parent, TestName);
	}
	while (ExistingObject);
	return TestName;
}
} // namespace

FOpResult VClass::NewUObject(FAllocationContext Context, VArchetype& InArchetype)
{
	V_DIE_IF(IsStruct());

	UClass* Class = GetUETypeChecked<UClass>();
	UObject* const Outer = verse::GetInstantiationOuter();
	EObjectFlags const SetFlags = FInstantiationScope::Context.Flags;

	FName Name = NAME_None;
	if (EnumHasAnyFlags(SetFlags, RF_DefaultSubObject) || FInstantiationScope::Context.bModuleTopLevel)
	{
		// Most CDO and module subobjects will be renamed to something based on their containing field,
		// to make them more robust against source changes like reordering definitions.
		// However, this renaming does not catch all subobjects, and this cannot be changed without
		// breaking assets in the wild, so the default name must also be chosen consistently.
		Name = MakeUniqueObjectNameForDefaultSubObject(Outer, Class);
	}

	VNativeConstructorWrapper* Result = nullptr;
	UObject* Object = nullptr;
	auto NewUObject = [&] AUTORTFM_ENABLE {
		Object = StaticAllocateObject(Class, Outer, Name, SetFlags);

		// Blueprint classes (like prefabs), as well as CDO subobjects, use instancing to initialize their properties.
		bool bInstanceSubobjects = false;
		if (EnumHasAnyFlags(SetFlags, RF_DefaultSubObject))
		{
			bInstanceSubobjects = true;
		}
		else if (UVerseClass* VerseClass = Cast<UVerseClass>(Class))
		{
			// Verse subclasses of Blueprint superclasses must also use instancing for their properties,
			// in order to preserve subclass-before-superclass initialization order.
			//
			// TODO: Consider switching back to the Verse constructor for Verse superclasses like entity.
			// This will require determining which properties the Blueprint subclass may have overridden.
			bInstanceSubobjects |= VerseClass->SolClassFlags & VCLASS_InheritsFromBlueprint;

			// The @field attribute currently relies on instancing to initialize OwnedEntities/Components.
			// TODO: Fix @field to work with bytecode-based construction and remove this.
			bInstanceSubobjects |= VerseClass->SolClassFlags & VCLASS_HasFieldAttribute;
		}
		else
		{
			bInstanceSubobjects = Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
		}
		if (bInstanceSubobjects)
		{
			V_DIE_UNLESS(Class->GetDefaultObject(false) != nullptr);

			// Initialize properties, and instance subobjects, but do not run blocks.
			EObjectInitializerOptions Options = EObjectInitializerOptions::InitializeProperties;
			FObjectInstancingGraph InstanceGraph(Object, EObjectInstancingGraphOptions::None);

			// Exclude properties that will be initialized by the caller.
			VShape* Shape = UVerseClass::GetShape(Context, Class);
			for (int32 Index = 0; Index < InArchetype.NumEntries; ++Index)
			{
				VShape::VEntry& Field = Shape->Fields[InArchetype.Entries[Index].Name];
				if (Field.IsProperty())
				{
					InstanceGraph.AddPropertyToSubobjectExclusionList(Field.UProperty);
				}
			}
			// To support constructor delegation for classes with instanced properties,
			// the constructor's archetype entries must also be excluded from instancing.
			V_DIE_IF(InArchetype.NextArchetype.Get(Context));

			(*Class->ClassConstructor)(FObjectInitializer(Object, nullptr, Options, &InstanceGraph));

			// Mark properties that were initialized by the engine as created and initialized.
			AutoRTFM::Open([&] AUTORTFM_DISABLE {
				VNativeConstructorWrapper& Wrapper = VNativeConstructorWrapper::New(Context, *this, Object);
				VEmergentType* EmergentType = Wrapper.GetEmergentType();
				for (auto It = Shape->CreateFieldsIterator(); It; ++It)
				{
					if (It->Value.IsProperty() && !InstanceGraph.IsPropertyInSubobjectExclusionList(It->Value.UProperty))
					{
						int32 FieldIndex = It.GetId().AsInteger();
						if (!EmergentType->IsFieldCreated(FieldIndex))
						{
							EmergentType = EmergentType->MarkFieldAsCreated(Context, FieldIndex);
							VValue Placeholder = Wrapper.UnifyField(Context, FieldIndex);
							V_DIE_IF(Placeholder);
						}
					}
				}
				Wrapper.SetEmergentType(Context, EmergentType);
				Result = &Wrapper;
			});
		}
		else
		{
			// Do not initialize properties, instance subobjects (which shouldn't exist), or run blocks.
			// The caller (typically the Verse program itself) will do this work.
			EObjectInitializerOptions Options = EObjectInitializerOptions::None;
			FObjectInstancingGraph InstanceGraph(Object, EObjectInstancingGraphOptions::DisableInstancing);

			(*Class->ClassConstructor)(FObjectInitializer(Object, nullptr, Options, &InstanceGraph));

			AutoRTFM::Open([&] AUTORTFM_DISABLE {
				VNativeConstructorWrapper& Wrapper = VNativeConstructorWrapper::New(Context, *this, Object);
				Result = &Wrapper;
			});
		}
	};

	if (CVarUObjectLeniency.GetValueOnAnyThread())
	{
		NewUObject();
	}
	else
	{
		V_REQUIRE_CONCRETE(Context.NativeFrame()->EffectToken.Follow());
		V_DIE_UNLESS(Context.NativeFrame()->IsCurrent(Context));
		AutoRTFM::ETransactionStatus Status = AutoRTFM::Close([&] {
			NewUObject();
		});
		if (Status != AutoRTFM::ETransactionStatus::Executing)
		{
			return {Context.HandleAutoRTFMFailure(Status)};
		}
	}

	V_RETURN(*Result);
}

// Iterate over the fields in a VArchetype chain, in the order they are visited by constructor bytecode.
// This is used to construct a VShape ahead of time, before or without actually executing constructors.
//
// A VArchetype chain can include an archetype expression, constructor functions, and class bodies:
// - When an expression or constructor function delegates to a constructor function, NextArchetype points
//   to that function's VArchetype.
// - Otherwise, NextArchetype points to the VArchetype of the expression or constructor's class.
// - Once the chain reaches a class body VArchetype, NextArchetype points to the superclass's VArchetype.
//
// The caller can override how a subclass constructs its superclass with the optional WalkSuper parameter.
//
// The VArchetype for an expression does not set its Class field, and only sets its NextArchetype field
// if it calls a constructor (because the class may not be known at compile time), so WalkArchetypeFields
// takes InClass and InNextArchetype parameters to be used in place of InArchetype.Class/NextArchetype.
//
// For example, in WalkArchetypeFields for this expression, InClass holds the runtime value of SomeType:
//   SomeType:concrete_subtype(t)
//   SomeType{}
void VClass::WalkArchetypeFields(
	FAllocationContext Context,
	VArchetype& InArchetype,
	VClass& InClass,
	VArchetype* InNextArchetype,
	int32& BaseIndex,
	TFunction<void(int32&)> WalkSuper,
	TFunction<void(VArchetype::VEntry&, int32, VClass&)> FieldCallbackProc)
{
	for (int32 Index = 0; Index < InArchetype.NumEntries; ++Index)
	{
		FieldCallbackProc(InArchetype.Entries[Index], BaseIndex + Index, InClass);
	}
	BaseIndex += InArchetype.NumEntries;

	if (InNextArchetype)
	{
		auto WalkNextArchetype = [Context, InNextArchetype, FieldCallbackProc](int32& BaseIndex) {
			WalkArchetypeFields(
				Context,
				*InNextArchetype,
				InNextArchetype->Class.Get(Context).StaticCast<VClass>(),
				InNextArchetype->NextArchetype.Get(Context).DynamicCast<VArchetype>(),
				BaseIndex,
				{},
				FieldCallbackProc);
		};

		// If NextArchetype corresponds to a constructor function for a base class, finish constructing the current class first.
		// This mimics the constructor bytecode's use of the InitSuper parameter, here called WalkSuper.
		// VArchetype A represents a class body if and only if A == A.Class.Archetype.
		VClass& NextArchetypeClass = InNextArchetype->Class.Get(Context).StaticCast<VClass>();
		bool bNextArchetypeForConstructor = InNextArchetype != &NextArchetypeClass.GetArchetype();
		bool bNextArchetypeForBase = &InClass != &NextArchetypeClass;
		if (bNextArchetypeForConstructor && bNextArchetypeForBase)
		{
			VArchetype& ClassArchetype = InClass.GetArchetype();
			WalkArchetypeFields(
				Context,
				ClassArchetype,
				InClass,
				ClassArchetype.NextArchetype.Get(Context).DynamicCast<VArchetype>(),
				BaseIndex,
				WalkNextArchetype,
				FieldCallbackProc);
		}
		else if (WalkSuper)
		{
			WalkSuper(BaseIndex);
		}
		else
		{
			WalkNextArchetype(BaseIndex);
		}
	}

	bool bIsClassArchetype = &InArchetype == &InClass.GetArchetype();
	if (bIsClassArchetype && InClass.NumInherited > 0)
	{
		int32 Index = InClass.Inherited[0]->GetKind() == VClass::EKind::Class;
		for (; Index < InClass.NumInherited; ++Index)
		{
			VArchetype& ClassArchetype = InClass.Inherited[Index]->GetArchetype();
			WalkArchetypeFields(
				Context,
				ClassArchetype,
				*InClass.Inherited[Index],
				ClassArchetype.NextArchetype.Get(Context).DynamicCast<VArchetype>(),
				BaseIndex,
				{},
				FieldCallbackProc);
		}
	}
}

VEmergentType& VClass::GetOrCreateEmergentTypeForVObject(FAllocationContext Context, VCppClassInfo* CppClassInfo, VArchetype& InArchetype)
{
	V_DIE_IF_MSG(IsNativeRepresentation(), "This code path for archetype instantiation should only be executed for non-native Verse objects!");

	TMap<FUtf8StringView, VValue> FunctionsByName;

	VArchetype* NextArchetype = InArchetype.NextArchetype.IsUninitialized() ? Archetype.Get() : InArchetype.NextArchetype.Get(Context).DynamicCast<VArchetype>();

	// TODO: This in the future shouldn't even require a hash table lookup when we introduce inline caching for this.
	VShape::FieldsMap ShapeFields;
	ShapeFields.Reserve(InArchetype.NumEntries);
	int32 BaseIndex = 0;
	WalkArchetypeFields(Context, InArchetype, *this, NextArchetype, BaseIndex, {},
		[this, Context, &FunctionsByName, &ShapeFields](VArchetype::VEntry& Entry, int32, VClass&) {
			// e.g. for `c := class { var X:int = 0 }`, `X`'s data is stored in the object, not the shape.
			if (!Entry.IsConstant())
			{
				ShapeFields.FindOrAdd({Context, *Entry.Name}, VShape::VEntry::Offset());
			}
			else if (Entry.Value.Get().IsCellOfType<VAccessor>())
			{
				// we want to generate the same code for archetype expressions regardless of whether a field is an accessor (for backwards compat, separate compilation, etc)
				// thus, we end up adding entries from accessors from archetype instantiation and need to overwrite them here when we find their original const entry
				ShapeFields.Add({Context, *Entry.Name}, VShape::VEntry::Constant(Context, Entry.Value.Get()));
			}
			else
			{
				VValue EntryValue = Entry.Value.Get();
				if (Entry.IsMethod() && EnumHasAllFlags(Flags, EFlags::EmulateCaseInsensitiveOverrides))
				{
					EntryValue = FunctionsByName.FindOrAdd(Entry.Name->AsStringView(), EntryValue);
				}

				ShapeFields.FindOrAdd({Context, *Entry.Name}, VShape::VEntry::Constant(Context, EntryValue));
			}
			return true;
		});

	TSet<VUniqueString*> ObjectFields;
	ObjectFields.Reserve(InArchetype.NumEntries);
	for (auto& Pair : ShapeFields)
	{
		if (Pair.Value.Type == EFieldType::Offset)
		{
			ObjectFields.FindOrAdd(Pair.Key.Get());
		}
	}

	// At this point, we have all the fields and their types, so we can now create an emergent type representing it.
	VUniqueStringSet& ObjectFieldNames = VUniqueStringSet::New(Context, ObjectFields);
	const uint32 ArcheTypeHash = GetSetVUniqueStringTypeHash(ObjectFields);
	// Note: We can look up the emergent type without locking our Mutex since this thread is the only one mutating the hash table
	if (TWriteBarrier<VEmergentType>* ExistingEmergentType = EmergentTypesCache.FindByHash(ArcheTypeHash, ObjectFieldNames))
	{
		return *ExistingEmergentType->Get();
	}

	// Compute the shape by interning the set of fields.
	VShape* NewShape = VShape::New(Context, MoveTemp(ShapeFields));
	VEmergentType* NewEmergentType = VEmergentType::New(Context, NewShape, this, CppClassInfo);
	V_DIE_IF(NewEmergentType == nullptr);

	FCellUniqueLock Lock(Mutex);

	// This new type will then be kept alive in the cache to re-vend if ever the exact same set of fields are used for
	// archetype instantiation of a different object.
	EmergentTypesCache.AddByHash(ArcheTypeHash, {Context, ObjectFieldNames}, {Context, *NewEmergentType});

	return *NewEmergentType;
}

VEmergentType& VClass::GetOrCreateEmergentTypeForNativeStruct(FAllocationContext Context)
{
	V_DIE_UNLESS(IsNativeStruct());

	// Note: We can look up the emergent type without locking our Mutex since this thread is the only one mutating the hash table
	const uint32 SingleHash = 0; // For native structs, we only ever store one emergent type, regardless of archetype
	if (TWriteBarrier<VEmergentType>* ExistingEmergentType = EmergentTypesCache.FindByHash(SingleHash, TWriteBarrier<VUniqueStringSet>{}))
	{
		return *ExistingEmergentType->Get();
	}

	UStruct* Struct = GetUETypeChecked<UStruct>();
	V_DIE_UNLESS(Struct->GetMinAlignment() <= VObject::DataAlignment);

	VShape* Shape = nullptr;
	if (UVerseStruct* VerseStruct = Cast<UVerseStruct>(Struct))
	{
		Shape = &VerseStruct->Shape.Get(Context).StaticCast<VShape>();
	}
	else
	{
		Shape = GlobalProgram->LookupShape(Context, Struct);
	}
	V_DIE_UNLESS(Shape);

	VEmergentType* NewEmergentType = VEmergentType::New(Context, Shape, this, &VNativeStruct::StaticCppClassInfo);

	FCellUniqueLock Lock(Mutex);

	// Keep alive in cache for future requests
	EmergentTypesCache.AddByHash(SingleHash, {Context, nullptr}, {Context, NewEmergentType});

	return *NewEmergentType;
}

VEmergentType& VClass::GetOrCreateEmergentTypeForNativeConstructorWrapper(FAllocationContext Context)
{
	if (NativeConstructorEmergentType)
	{
		return *NativeConstructorEmergentType;
	}

	UStruct* Struct = GetUETypeChecked<UStruct>();

	VShape* Shape = nullptr;
	if (IsStruct())
	{
		if (UVerseStruct* VerseStruct = Cast<UVerseStruct>(Struct))
		{
			Shape = &VerseStruct->Shape.Get(Context).StaticCast<VShape>();
		}
		else
		{
			Shape = GlobalProgram->LookupShape(Context, Struct);
		}
	}
	else
	{
		Shape = UVerseClass::GetShape(Context, CastChecked<UClass>(Struct));
	}
	V_DIE_UNLESS(Shape);

	VEmergentType* NewEmergentType = VEmergentType::New(Context, VTrivialType::Singleton.Get(), &VNativeConstructorWrapper::StaticCppClassInfo);
	NewEmergentType->CreatedFields.Init(Context, Shape->GetMaxFieldIndex());

	NativeConstructorEmergentType.Set(Context, NewEmergentType);
	return *NewEmergentType;
}

VShape* VClass::CreateShapeForUStruct(
	Verse::FAllocationContext Context,
	TFunction<FProperty*(VArchetype::VEntry&, int32)>&& CreateProperty,
	TFunction<UFunction*(VArchetype::VEntry&)>&& CreateFunction)
{
	UStruct* Struct = GetUETypeChecked<UStruct>();
	UVerseClass* VerseClass = Cast<UVerseClass>(Struct);

	VShape* SuperShape = nullptr;
	if (UClass* SuperClass = Cast<UClass>(Struct->GetSuperStruct()))
	{
		SuperShape = UVerseClass::GetShape(Context, SuperClass);
	}

	TMap<FUtf8StringView, VValue> FunctionsByName;

	VShape::FieldsMap ShapeFields;
	ShapeFields.Reserve(Archetype->NumEntries);
	int32 BaseIndex = 0;
	WalkArchetypeFields(Context, *Archetype.Get(), *this, Archetype->NextArchetype.Get(Context).DynamicCast<VArchetype>(), BaseIndex, {},
		[this, Context, &CreateProperty, &CreateFunction, VerseClass, SuperShape, &FunctionsByName, &ShapeFields](VArchetype::VEntry& Entry, int32 Index, VClass&) {
			if (ShapeFields.Contains({Context, *Entry.Name}))
			{
				return true;
			}

			// UObjects store data fields as properties in the object, and methods in the function map.
			if (Entry.IsMethod())
			{
				VValue EntryValue = Entry.Value.Get();
				if (EnumHasAllFlags(Flags, EFlags::EmulateCaseInsensitiveOverrides))
				{
					EntryValue = FunctionsByName.FindOrAdd(Entry.Name->AsStringView(), EntryValue);
				}

				CreateFunction(Entry);
				ShapeFields.Add({Context, *Entry.Name}, VShape::VEntry::Constant(Context, EntryValue));
			}
			else if (Entry.Value.Get().IsCellOfType<VAccessor>())
			{
				ShapeFields.Add({Context, *Entry.Name}, VShape::VEntry::Constant(Context, Entry.Value.Get()));
			}
			else
			{
				FProperty* FieldProperty = nullptr;
				if (const VShape::VEntry* SuperEntry = SuperShape ? SuperShape->GetField(*Entry.Name) : nullptr)
				{
					// If the super shape has it, recycle the same property
					V_DIE_UNLESS(SuperEntry->IsProperty());
					FieldProperty = SuperEntry->UProperty;
					ShapeFields.Add({Context, *Entry.Name}, *SuperEntry);
				}
				else
				{
					FieldProperty = CreateProperty(Entry, Index);
					V_DIE_UNLESS(FieldProperty != nullptr);
					if (Entry.IsNativeRepresentation())
					{
						VValue FieldType = Entry.Type.Follow();
						V_DIE_IF(FieldType.IsUninitialized() || FieldType.IsPlaceholder());
						if (FieldType.IsCellOfType<VPointerType>())
						{
							ShapeFields.Add({Context, *Entry.Name}, VShape::VEntry::PropertyVar(FieldProperty));
						}
						else
						{
							ShapeFields.Add({Context, *Entry.Name}, VShape::VEntry::Property(FieldProperty));
						}
					}
					else
					{
						V_DIE_UNLESS(FieldProperty->IsA<FVRestValueProperty>());
						ShapeFields.Add({Context, *Entry.Name}, VShape::VEntry::VerseProperty(FieldProperty));
					}
				}

				if (VerseClass && Entry.HasDefaultValueExpression())
				{
					VerseClass->PropertiesWrittenByInitCDO.Add(FieldProperty);
				}
			}
			return true;
		});

	return VShape::New(Context, MoveTemp(ShapeFields));
}

VShape* VClass::CreateShapeForExistingUStruct(FAllocationContext Context)
{
	UStruct* Struct = GetUETypeChecked<UStruct>();

	auto CreateProperty = [this, Struct](VArchetype::VEntry& Entry, int32) {
		FString Name = Entry.Name.Get()->AsString();
		FStringView PropName = Verse::Names::RemoveQualifier(Name);
		FStringView CrcPropName = Entry.UseCRCName() ? Name : PropName;
		FName UePropName = Verse::Names::VersePropToUEFName(PropName, CrcPropName);
		FProperty* FieldProperty = Struct->FindPropertyByName(UePropName);
		check(FieldProperty); // should have been verified at script compile time
		return FieldProperty;
	};
	auto CreateFunction = [](VArchetype::VEntry&) {
		return nullptr;
	};
	return CreateShapeForUStruct(Context, CreateProperty, CreateFunction);
}

UFunction* VClass::MaybeCreateUFunctionForCallee(Verse::FAllocationContext Context, VFunction& Callee)
{
	VUniqueString* Name = nullptr;
	uint32 NumPositionalParameters = 0;
	uint32 NumNamedParameters = 0;
	if (VProcedure* Procedure = Callee.Procedure.Get().DynamicCast<VProcedure>())
	{
		Name = Procedure->Name.Get();
		NumPositionalParameters = Procedure->NumPositionalParameters;
		NumNamedParameters = Procedure->NumNamedParameters;
	}
	else if (VNativeProcedure* NativeProcedure = Callee.Procedure.Get().DynamicCast<VNativeProcedure>())
	{
		Name = NativeProcedure->Name.Get();
		NumPositionalParameters = NativeProcedure->NumPositionalParameters;
		NumNamedParameters = 0;
	}

	// For now, we support only functions with no arguments
	const bool bShouldGenerateUFunction = (NumPositionalParameters + NumNamedParameters == 0);
	if (!bShouldGenerateUFunction)
	{
		return nullptr;
	}

	// Create a new UFunction and add it the class's field list and function map
	UClass* UeClass = GetUETypeChecked<UClass>();
	FName FunctionName = Verse::Names::VerseFuncToUEFName(FString(Name->AsStringView()));
	ensure(!StaticFindObjectFast(UVerseFunction::StaticClass(), UeClass, FunctionName));
	UVerseFunction* CalleeFunction = NewObject<UVerseFunction>(UeClass, FunctionName);
	CalleeFunction->FunctionFlags |= FUNC_Public | FUNC_Native;
	CalleeFunction->InitializeDerivedMembers();
	CalleeFunction->Callee.Set(Context, Callee);
	CalleeFunction->Bind();
	return CalleeFunction;
}

void VClass::CommonPrepare(FAllocationContext Context, FInitOrValidateUStruct& InitOrValidate, UStruct* Type)
{
	// -----------------------------------------------------------------------------------------------------
	// Keep the following code in sync with FSolClassGenerator::Prepare for structs, classes, and interfaces

#if WITH_EDITOR
	InitOrValidate.SetMetaData(true, TEXT("IsBlueprintBase"), TEXT("false"));
#endif
}

void VClass::Prepare(FAllocationContext Context, FInitOrValidateUVerseStruct& InitOrValidate, UVerseStruct* Type)
{
	// -----------------------------------------------------------------------------------------------------
	// Keep the following code in sync with FSolClassGenerator::Prepare for structs, classes, and interfaces

	CommonPrepare(Context, InitOrValidate, Type);

	if (IsNativeBound() && InitOrValidate.IsInitializing())
	{
		Type->SetNativeBound();
	}

	TUtf8StringBuilder<Names::DefaultNameLength> QualifiedName;
	AppendQualifiedName(QualifiedName);

	InitOrValidate.SetValue(Type->Guid, FGuid(FCrc::Strihash_DEPRECATED(*Type->GetName()), GetTypeHash(Type->GetPackage()->GetName()), 0, 0), TEXT("Guid"));
	//??? InitOrValidate.ForceValue(VerseStruct->ConstructorEffects, MakeEffectSet(SemanticType->_ConstructorEffects));
	InitOrValidate.SetValue(Type->QualifiedName, QualifiedName.ToString(), TEXT("QualifiedName")); // TODO - Enable move temp???
}

void VClass::Prepare(FAllocationContext Context, FInitOrValidateUVerseClass& InitOrValidate, UVerseClass* Type)
{
	// -----------------------------------------------------------------------------------------------------
	// Keep the following code in sync with FSolClassGenerator::Prepare for structs, classes, and interfaces

	CommonPrepare(Context, InitOrValidate, Type);

	if (IsNativeBound() && InitOrValidate.IsInitializing())
	{
		Type->SetNativeBound();
	}

	checkf(Type->GetDefaultObject(false) == nullptr || InitOrValidate.IsValidating(), TEXT("Class `%s` instantiated twice!"), *Type->GetName());

	// Preserve Verse identity of this class or module
	// InitOrValidate.SetValue(VerseClass->PackageRelativeVersePath, GetPackageRelativeVersePathFromDefinition(AstNode), TEXT("PackageRelativeVersePath"));

	InitOrValidate.SetValue(Type->ClassWithin, UObject::StaticClass(), TEXT("ClassWithin"));
	InitOrValidate.SetClassFlags(CLASS_EditInlineNew, true, TEXT("EditInlineNew"));
	InitOrValidate.SetClassFlags(CLASS_HasInstancedReference, true, TEXT("HasInstancedReference"));
	InitOrValidate.SetClassFlagsNoValidate(CLASS_CompiledFromBlueprint, true);

	InitOrValidate.SetClassFlags(CLASS_Interface, GetKind() == VClass::EKind::Interface, TEXT("Interface"));
	InitOrValidate.ForceVerseClassFlags(VCLASS_Concrete, EnumHasAllFlags(Flags, EFlags::Concrete));
	InitOrValidate.ForceVerseClassFlags(VCLASS_Castable, EnumHasAllFlags(Flags, EFlags::ExplicitlyCastable));
	InitOrValidate.ForceVerseClassFlags(VCLASS_FinalSuper, EnumHasAllFlags(Flags, EFlags::FinalSuper));
	InitOrValidate.ForceVerseClassFlags(VCLASS_Parametric, EnumHasAllFlags(Flags, EFlags::Parametric));
	InitOrValidate.ForceVerseClassFlags(VCLASS_UniversallyAccessible, EnumHasAllFlags(Flags, EFlags::UniversallyAccessible));
	InitOrValidate.ForceVerseClassFlags(VCLASS_EpicInternal, EnumHasAllFlags(Flags, EFlags::EpicInternal));
	if (GetKind() == VClass::EKind::Struct)
	{
		InitOrValidate.SetVerseClassFlags(VCLASS_PersonaConstructible, EnumHasAllFlags(Flags, EFlags::PersonaConstructible), TEXT("PersonaConstructible"));
	}
	else
	{
		InitOrValidate.ForceVerseClassFlags(VCLASS_PersonaConstructible, EnumHasAllFlags(Flags, EFlags::PersonaConstructible));
	}

	if (GetKind() == VClass::EKind::Class)
	{
		// const bool bIsConstructorEpicInternal = GetConstructorAccessibilityScope(*SemanticType->_Definition).IsEpicInternal();
		// InitOrValidate.ForceVerseClassFlags(VCLASS_EpicInternalConstructor, bIsConstructorEpicInternal || !bIsPublicScope);
		// InitOrValidate.ForceValue(VerseClass->ConstructorEffects, MakeEffectSet(SemanticType->_ConstructorEffects));

#if WITH_EDITOR
		//// Splitting the development status from the engine as it is not as clean as expected. We seem to be using Experimental classes in UEFN.
		// const UValkyrieMetaData* ValkrieMetaData = GetDefault<UValkyrieMetaData>();
		// InitOrValidate.SetMetaData(Definition->IsExperimental(), ValkrieMetaData->DevelopmentStatusKey, *ValkrieMetaData->DevelopmentStatusValue_Experimental);
		// InitOrValidate.SetMetaData(Definition->IsDeprecated(), ValkrieMetaData->DeprecationStatusKey, *ValkrieMetaData->DeprecationStatusValue_Deprecated);
#endif
	}

#if WITH_EDITOR
	// if constexpr (FQuery::bIsClass || FQuery::bIsInterface)
	//{
	//	const FString UnmangledClassName = FULangConversionUtils::ULangStrToFString(SemanticType->GetParametricTypeScope().GetScopeName().AsString());
	//	InitOrValidate.SetMetaData(true, TEXT("DisplayName"), *UnmangledClassName);
	// }
#endif // WITH_EDITOR

	// if constexpr (FQuery::bIsClass || FQuery::bIsModule || FQuery::bIsInterface)
	//{
	//	FName PackageVersePath = NAME_None;
	//	if (const uLang::CAstPackage* Package = SemanticType->GetPackage())
	//	{
	//		// We mangle the FName so that it remains case sensitive
	//		FString FPath = FULangConversionUtils::ULangStrToFString(Package->_VersePath);
	//		PackageVersePath = FName(Verse::Names::Private::MangleCasedName(FPath, FPath));
	//	}
	//	InitOrValidate.SetValue(VerseClass->MangledPackageVersePath, PackageVersePath, TEXT("MangledPackageVersePath"));
	// }

	// Also create UE classes for the superclass and for interfaces
	UClass* SuperUClass;
	int32 FirstInterfaceIndex;
	if (NumInherited > 0 && Inherited[0]->GetKind() == VClass::EKind::Class)
	{
		SuperUClass = CastChecked<UClass>(Inherited[0]->GetOrCreateNativeType(Context));
		FirstInterfaceIndex = 1;
	}
	else
	{
		SuperUClass = GetKind() == VClass::EKind::Interface ? UInterface::StaticClass() : UObject::StaticClass();
		FirstInterfaceIndex = 0;
	}
	InitOrValidate.SetSuperStruct(SuperUClass);
	if (UVerseClass* SuperVerseClass = Cast<UVerseClass>(SuperUClass))
	{
		if (SuperVerseClass->SolClassFlags & VCLASS_InheritsFromBlueprint)
		{
			InitOrValidate.SetVerseClassFlags(VCLASS_InheritsFromBlueprint, true, TEXT("InheritsFromBlueprint"));
		}
	}
	else
	{
		if (SuperUClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
		{
			InitOrValidate.SetVerseClassFlags(VCLASS_InheritsFromBlueprint, true, TEXT("InheritsFromBlueprint"));
		}
	}
	InitOrValidate.SetValue(Type->ClassConfigName, SuperUClass->ClassConfigName, TEXT("ClassConfigName"));

	int32 NumDirectInterfaces = NumInherited - FirstInterfaceIndex;
	Type->Interfaces.Reserve(NumDirectInterfaces);
	for (int32 Index = FirstInterfaceIndex; Index < NumInherited; ++Index)
	{
		LinkSuperInterface(Context, InitOrValidate, *Inherited[Index], EAddInterfaceType::Direct);
	}
	InitOrValidate.ValidateInterfaces();
}

void VClass::LinkSuperInterface(FAllocationContext Context, FInitOrValidateUVerseClass& InitOrValidate, VClass& SuperInterface, EAddInterfaceType InterfaceType)
{
	V_DIE_UNLESS(SuperInterface.IsInterface());
	UClass* InheritedUClass = CastChecked<UClass>(SuperInterface.GetOrCreateNativeType(Context));
	if (InitOrValidate.AddInterface(InheritedUClass, InterfaceType))
	{
		for (int32 Index = 0; Index < SuperInterface.NumInherited; ++Index)
		{
			LinkSuperInterface(Context, InitOrValidate, *SuperInterface.Inherited[Index], EAddInterfaceType::Indirect);
		}
	}
}

UStruct* VClass::GetOrCreateNativeType(FAllocationContext Context)
{
	if (NativeType)
	{
		V_DIE_IF(NativeType.Follow().IsPlaceholder());
		return GetUETypeChecked<UStruct>();
	}

	UPackage* UEPackage = Package->GetOrCreateUPackage(Context);

	EVersePackageType PackageType;
	Names::GetUPackagePath(Package->GetName().AsStringView(), &PackageType);
	UTF8CHAR Separator = PackageType == EVersePackageType::VNI ? UTF8CHAR('_') : UTF8CHAR('-');

	TUtf8StringBuilder<Names::DefaultNameLength> UEName;
	AppendMangledName(UEName, Separator);

	UStruct* Struct = nullptr;
	UVerseStruct* VerseStruct = nullptr;
	UVerseClass* VerseClass = nullptr;
	if (IsStruct())
	{
		Struct = VerseStruct = NewObject<UVerseStruct>(UEPackage, FName(UEName), RF_Public);
	}
	else
	{
		Struct = VerseClass = NewObject<UVerseClass>(UEPackage, FName(UEName), RF_Public);
	}
	NativeType.Set(Context, Struct);

	return Struct;
}

FOpResult VClass::BindNativeClass(FAllocationContext Context, bool bImported)
{
	IEngineEnvironment* Environment = VerseVM::GetEngineEnvironment();
	check(Environment);

	UStruct* NativeStruct = GetUETypeChecked<UStruct>();
	UVerseStruct* VerseStruct = Cast<UVerseStruct>(NativeStruct);
	UVerseClass* VerseClass = Cast<UVerseClass>(NativeStruct);

	if (IsStruct())
	{
		if (bImported)
		{
			GlobalProgram->AddImport(Context, *this);
			if (VerseStruct == nullptr)
			{
				// We don't currently validate imported non-Verse types.
				CastChecked<UScriptStruct>(NativeStruct);
				return {FOpResult::Return};
			}

			check(VerseStruct->IsUHTNative());
			Package->GetOrCreateUPackage(Context);
		}

		AddRedirect(ECoreRedirectFlags::Type_Struct);

		VerseStruct->Class.Set(Context, this);

		Private::FVerseVMInitOrValidate<UVerseStruct> InitOrValidate(VerseStruct);
		Prepare(Context, InitOrValidate, VerseStruct);
	}
	else
	{
		if (bImported)
		{
			GlobalProgram->AddImport(Context, *this);
			if (VerseClass == nullptr)
			{
				// We don't currently validate imported non-Verse types.
				CastChecked<UClass>(NativeStruct);
				return {FOpResult::Return};
			}

			check(VerseClass->IsUHTNative());
			Package->GetOrCreateUPackage(Context);
		}

		AddRedirect(ECoreRedirectFlags::Type_Class);

		VerseClass->Class.Set(Context, this);

		Private::FVerseVMInitOrValidate<UVerseClass> InitOrValidate(VerseClass);
		Prepare(Context, InitOrValidate, VerseClass);
	}

	TArray<TPair<int32, FClassEntryAttributeElement>> EntryAttributeElements;
	EntryAttributeElements.Reserve(Archetype->NumEntries);

	// Populate shape and class members
	VShape* Shape = nullptr;
	{
		FField::FLinkedListBuilder PropertyListBuilder(&NativeStruct->ChildProperties);
		auto CreateProperty = [this, Context, bImported, Environment, NativeStruct, &EntryAttributeElements, &PropertyListBuilder, VerseClass](VArchetype::VEntry& Entry, int32 Index) {
			bool bHasAttributes = AttributeIndices
							   && 1 + Index + 1 < AttributeIndices->Num()
							   && AttributeIndices->GetValue(1 + Index + 0).AsInt32() < AttributeIndices->GetValue(1 + Index + 1).AsInt32();
			FUtf8StringView PropName = Verse::Names::RemoveQualifier(Entry.Name.Get()->AsStringView());
			FUtf8StringView CrcPropName = Entry.UseCRCName() ? Entry.Name.Get()->AsStringView() : PropName;

			FProperty* FieldProperty = nullptr;
			if (!bImported)
			{
				FieldProperty = Environment->CreateProperty(
					Context,
					Package->GetUPackage(),
					NativeStruct,
					PropName,
					CrcPropName,
					Entry.Type.Follow(),
					Entry.IsNativeRepresentation(),
					Entry.IsInstanced());

				if (!Entry.HasDefaultValueExpression())
				{
					// For the editor: This property has no default and therefore must be specified
					FieldProperty->PropertyFlags |= EPropertyFlags::CPF_RequiredParm;
				}

				if (bHasAttributes)
				{
					EntryAttributeElements.Emplace(Index, FClassEntryAttributeElement(FieldProperty));
				}

				// Link the newly created property to the property list
				PropertyListBuilder.AppendNoTerminate(*FieldProperty);
			}
			else
			{
				FString PropNameString(PropName);
				FString CrcPropNameString(CrcPropName);
				FName UEVerseName = Verse::Names::VersePropToUEFName(PropNameString, CrcPropNameString);
				FieldProperty = NativeStruct->FindPropertyByName(UEVerseName);
				if (FieldProperty != nullptr)
				{
					VType* PropertyType = Entry.Type.Follow().DynamicCast<VType>();
					if (!VerseVM::GetEngineEnvironment()->ValidateProperty(Context, FName(*PropNameString), PropertyType, FieldProperty, Entry.IsNativeRepresentation(), Entry.IsInstanced()))
					{
						V_DIE("The imported type: `%s` does not have the required property type for the property `%s`", *GetBaseName().AsString(), *PropNameString);
					}
				}
				else
				{
					V_DIE("The imported type: `%s` does not contain the required property `%s`", *GetBaseName().AsString(), *PropNameString);
				}
			}

			if (Entry.IsPredicts())
			{
				VerseClass->PredictsVarNames.Add(FAnsiString{PropName}, FieldProperty->GetFName());
			}

			return FieldProperty;
		};

		UField::FLinkedListBuilder ChildrenBuilder(ToRawPtr(MutableView(NativeStruct->Children)));
		auto CreateFunction = [this, Context, VerseClass, &ChildrenBuilder](VArchetype::VEntry& Entry) {
			UFunction* Function = MaybeCreateUFunctionForCallee(Context, Entry.Value.Get().StaticCast<VFunction>());
			if (Function != nullptr)
			{
				ChildrenBuilder.AppendNoTerminate(*Function);
				VerseClass->AddFunctionToFunctionMap(Function, Function->GetFName());

				FName DisplayName(Names::RemoveQualifier(Entry.Name->AsStringView()));
				VerseClass->DisplayNameToUENameFunctionMap.Add(DisplayName, Function->GetFName());
			}

			if (Entry.IsPredicts())
			{
				FName Name = UVerseFunction::GetUFunctionFName(Entry.Value.Get().StaticCast<Verse::VFunction>());
				VerseClass->PredictsFunctionNames.Add(Name);
			}

			return Function;
		};

		Shape = CreateShapeForUStruct(Context, CreateProperty, CreateFunction);

		if (bImported && !IsStruct())
		{
			TUtf8StringBuilder<Names::DefaultNameLength> ScopeName;
			AppendScopeName(ScopeName);
			VerseClass->BindVerseCallableFunctions(Package.Get(), ScopeName);
		}
	}
	if (bImported)
	{
		V_RETURN(*Shape);
	}

	// UVerseClass and UVerseStruct also do native binding in Link.
	NativeStruct->Bind();
	NativeStruct->StaticLink(/*bRelinkExistingProperties =*/true);

	TArray<FString> Errors;
	if (Attributes)
	{
		V_DIE_UNLESS(Attributes->GetArrayType() == Verse::EArrayType::VValue || Attributes->GetArrayType() == Verse::EArrayType::None);
		VValue* AttributeValues = Attributes->GetData<VValue>();

		// Handle class attributes first
		{
			// Attributes at index 0 relate to the actual Verse Class definition
			const int32 Begin = AttributeIndices->GetValue(0).AsInt32();
			const int32 End = AttributeIndices->GetValue(1).AsInt32();
			for (VValue AttributeValue : TArrayView<VValue>(AttributeValues + Begin, End - Begin))
			{
				FClassAttribute::ApplyClassAttribute(Context, AttributeValue.Follow(), *this, NativeStruct, Errors);
			}
		}

		// Then handle field attributes
		for (TPair<int32, FClassEntryAttributeElement>& Element : EntryAttributeElements)
		{
			// Class attributes precede class entry attributes in the AttributeValues so we offset the attribute index to account for this
			const int32 EntryIndex = Element.Key;
			const int32 AttributeIndex = EntryIndex + 1;

			const int32 Begin = AttributeIndices->GetValue(AttributeIndex + 0).AsInt32();
			const int32 End = AttributeIndices->GetValue(AttributeIndex + 1).AsInt32();

			VArchetype::VEntry& Entry = Archetype->Entries[EntryIndex];
			for (VValue AttributeValue : TArrayView<VValue>(AttributeValues + Begin, End - Begin))
			{
				Element.Value.Apply(Context, AttributeValue.Follow(), Entry, Errors);
			}
		}

		// If a property has attributes, it might have changed replication settings.
		if (UClass* NativeClass = Cast<UClass>(NativeStruct))
		{
			NativeClass->SetUpRuntimeReplicationData();
		}
	}

	// Collect all UObjects referenced by FProperties and assemble the GC token stream
	if (IsStruct())
	{
		VerseStruct->CollectBytecodeAndPropertyReferencedObjectsRecursively();
		VerseStruct->AssembleReferenceTokenStream(/*bForce=*/true);
		if (!VerseStruct->ReferenceSchema.Get().IsEmpty())
		{
			EnumAddFlags(Flags, EFlags::NativeStructWithObjectReferences);
		}
	}
	else
	{
		VerseClass->CollectBytecodeAndPropertyReferencedObjectsRecursively();
		VerseClass->AssembleReferenceTokenStream(/*bForce=*/true);
	}

	if (Errors.Num() > 0)
	{
		TStringBuilder<128> ErrorMessages;
		for (const FString& Error : Errors)
		{
			ErrorMessages.Append(Error);
			ErrorMessages.Append("\n");
		}
		Context.RaiseVerseRuntimeError(ERuntimeDiagnostic::ErrRuntime_Internal, FText::FromString(*ErrorMessages));
		return {FOpResult::Error};
	}

	V_RETURN(*Shape);
}

namespace
{
TGlobalHeapPtr<VFunction> MarkSuperFieldsCreatedFunction;
}

FOpResult VClass::ConstructNativeDefaultObject(FRunningContext Context)
{
	V_DIE_IF(IsStruct());

	UVerseClass* VerseClass = GetUETypeChecked<UVerseClass>();

	// If the CDO already exists, any instances created from it via the instancing codepath are missing defaults.
	// When this assert fails, the compiler is likely missing a dependency somewhere.
	// UHT classes have their CDO created early, so we can't catch early instantiation for them.
	V_DIE_UNLESS(VerseClass->IsUHTNative() || VerseClass->GetDefaultObject(false) == nullptr);

	// TODO(SOL-8757): Don't require the effect token to allocate a UObject.
	UObject* CDO = nullptr;
	if (CVarUObjectLeniency.GetValueOnAnyThread())
	{
		CDO = VerseClass->GetDefaultObject();
	}
	else
	{
		V_REQUIRE_CONCRETE(Context.NativeFrame()->EffectToken.Follow());
		V_DIE_UNLESS(Context.NativeFrame()->IsCurrent(Context));
		FOpResult Result = Context.Close([&] { CDO = VerseClass->GetDefaultObject(); });
		if (!Result.IsReturn())
		{
			return Result;
		}
	}
	V_DIE_UNLESS(CDO);

	if (!MarkSuperFieldsCreatedFunction)
	{
		VNativeProcedure& NativeProcedure = VNativeProcedure::New(Context, 1, InitSuperDefaultObject, VUniqueString::New(Context, "InitSuperDefaultObject"));
		MarkSuperFieldsCreatedFunction.Set(Context, &VFunction::New(Context, NativeProcedure, VValue()));
	}

	FInstantiationScope InitCtx(FInstantiationContext(CDO, RF_Public | RF_Transactional | RF_ArchetypeObject | RF_DefaultSubObject));
	FPackageScope PackageScope = Context.SetCurrentPackage(Package.Get());

	VNativeConstructorWrapper& Self = VNativeConstructorWrapper::New(Context, *this, CDO);
	VValue CreateFieldToken = VValue::CreateFieldMarker();
	VValue SkipBlocks = GlobalTrue();
	VValue InitSuper = *MarkSuperFieldsCreatedFunction;
	FOpResult Result = Constructor->InvokeWithSelf(Context, Self, {CreateFieldToken, SkipBlocks, InitSuper});
	if (!Result.IsReturn())
	{
		V_DIE_UNLESS(Result.IsError());
		return Result;
	}

	// Ensure all default initializers have run.
	// Note that this does not ensure all placeholders are resolved, particularly in subobjects,
	// which is checked on-demand by FVRestValueProperty::CopyValuesInternal and InstanceSubobjects.
	V_DIE_UNLESS(Result.Value == VValue::CreateFieldMarker());
	VShape& Shape = VerseClass->Shape.Get(Context).StaticCast<VShape>();
	int32 BaseIndex = 0;
	WalkArchetypeFields(Context, *Archetype.Get(), *this, Archetype->NextArchetype.Get(Context).DynamicCast<VArchetype>(), BaseIndex, {},
		[&](VArchetype::VEntry& Entry, int32, VClass&) {
			if (Entry.HasDefaultValueExpression())
			{
				int32 FieldIndex = Shape.GetFieldIndex(*Entry.Name);
				V_DIE_UNLESS(Self.LoadField(Context, FieldIndex).IsUninitialized());
			}
		});

#if WITH_EDITORONLY_DATA
	VerseClass->TrackDefaultInitializedProperties(CDO);
#endif

	FOverridableManager::Get().Disable(CDO, /*bPropagateToSubObjects*/ true);

	Result = Context.Close([&] { VerseClass->PrePopulateVerseFields(CDO); });
	if (!Result.IsReturn())
	{
		return Result;
	}

	UVerseClass::RenameDefaultSubobjects(CDO);

	return {FOpResult::Return};
}

FOpResult VClass::InitSuperDefaultObject(FRunningContext Context, VValue Self, TArrayView<VValue> Arguments)
{
	checkSlow(Arguments.Num() == 1);
	V_REQUIRE_CONCRETE(Arguments[0]);
	VNativeConstructorWrapper& Wrapper = Self.StaticCast<VNativeConstructorWrapper>();
	UObject* Object = Wrapper.WrappedObject().AsUObject();
	if (UVerseClass* SuperVerseClass = Cast<UVerseClass>(Object->GetClass()->GetSuperClass()))
	{
		VEmergentType* EmergentType = Wrapper.GetEmergentType();
		VClass* SuperClass = SuperVerseClass->Class.Get();
		int32 BaseIndex = 0;
		VShape& Shape = CastChecked<UVerseClass>(Object->GetClass())->Shape.Get(Context).StaticCast<VShape>();
		WalkArchetypeFields(Context, *SuperClass->Archetype.Get(), *SuperClass, SuperClass->Archetype->NextArchetype.Get(Context).DynamicCast<VArchetype>(), BaseIndex, {},
			[&](VArchetype::VEntry& Entry, int32, VClass& Class) {
				if (Entry.HasDefaultValueExpression())
				{
					int32 FieldIndex = Shape.GetFieldIndex(*Entry.Name.Get());
					if (!EmergentType->IsFieldCreated(FieldIndex))
					{
						EmergentType = EmergentType->MarkFieldAsCreated(Context, FieldIndex);
						VValue Placeholder = Wrapper.UnifyField(Context, FieldIndex);
						V_DIE_IF(Placeholder);
					}
				}
			});
		Wrapper.SetEmergentType(Context, EmergentType);
	}
	V_RETURN(Arguments[0]);
}

bool VClass::SubsumesImpl(FAllocationContext Context, VValue Value)
{
	VClass* InputType = nullptr;
	if (VObject* Object = Value.DynamicCast<VObject>())
	{
		VCell* TypeCell = Object->GetEmergentType()->Type.Get();
		checkSlow(TypeCell->IsA<VClass>());
		InputType = static_cast<VClass*>(TypeCell);
	}
	else if (Value.IsUObject())
	{
		UClass* UEClass = Value.AsUObject()->GetClass();
		if (UVerseClass* VerseClass = Cast<UVerseClass>(UEClass))
		{
			InputType = VerseClass->Class.Get();
		}
		else
		{
			InputType = &GlobalProgram->LookupImport(Context, UEClass)->StaticCast<VClass>();
		}
	}
	else
	{
		return false;
	}

	if (InputType == this)
	{
		return true;
	}

	TArray<VClass*, TInlineAllocator<8>> ToCheck;
	auto PushInherited = [&ToCheck](VClass* Class) {
		for (uint32 I = 0; I < Class->NumInherited; ++I)
		{
			ToCheck.Push(Class->Inherited[I].Get());
		}
	};

	PushInherited(InputType);
	while (ToCheck.Num())
	{
		VClass* Class = ToCheck.Pop();
		if (Class == this)
		{
			return true;
		}
		PushInherited(Class);
	}

	return false;
}

TSharedPtr<FJsonValue> VClass::ToJSONImpl(FRunningContext Context, EValueJSONFormat Format, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback Callback, uint32 RecursionDepth, FJsonObject* Defs)
{
	VisitedObjects.Add(this, Verse::EVisitState::Visiting);
	ON_SCOPE_EXIT
	{
		VisitedObjects.Add(this, EVisitState::Visited);
	};

	if (Format == EValueJSONFormat::Persona && !EnumHasAllFlags(Flags, EFlags::PersonaConstructible))
	{
		return nullptr;
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> RequiredProperties;

	TSet<VUniqueString*> Processed;
	TSet<FString> Defaults; // this doesn't handle block initialization...
	int32 BaseIndex = 0;
	bool bFail = false;
	WalkArchetypeFields(Context, *Archetype.Get(), *this, Archetype->NextArchetype.Get(Context).DynamicCast<VArchetype>(), BaseIndex, {},
		[&](VArchetype::VEntry& Entry, int32, VClass& Class) {
			VUniqueString* Name = Entry.Name.Get();
			if (bFail || Entry.IsMethod() || Processed.Contains(Name))
			{
				return;
			}

			Processed.Add(Name);
			TSharedPtr<FJsonValue> EntryValue = Entry.Type.Get().ToJSON(Context, Format, VisitedObjects, Callback, RecursionDepth, Defs);
			if (!EntryValue.IsValid())
			{
				bFail = true;
				return;
			}

			FString PropName = Format == EValueJSONFormat::Persona ? Entry.Name->AsString() : FString(Verse::Names::RemoveQualifier(Entry.Name->AsString()));

			if (Format == EValueJSONFormat::Persona)
			{
				if (TSharedPtr<FJsonObject> FieldObject = EntryValue->AsObject())
				{
					UStruct* OwnerStruct = Class.GetUEType<UStruct>();
					const VShape::VEntry* ShapeEntry = nullptr;

					if (const UVerseClass* VerseClass = Cast<UVerseClass>(OwnerStruct))
					{
						if (VShape* Shape = UVerseClass::GetShape(Context, const_cast<UVerseClass*>(VerseClass)))
						{
							ShapeEntry = Shape->GetField(*Entry.Name);
						}
					}
					else if (const UVerseStruct* VerseStruct = Cast<UVerseStruct>(OwnerStruct))
					{
						if (VShape* Shape = const_cast<UVerseStruct*>(VerseStruct)->Shape.Get(Context).DynamicCast<VShape>())
						{
							ShapeEntry = Shape->GetField(*Entry.Name);
						}
					}

					if (ShapeEntry)
					{
						FProperty* Property = ShapeEntry->UProperty;
						if (FVRestValueProperty* RestProp = CastField<FVRestValueProperty>(Property))
						{
							Property = RestProp->GetOrCreateLegacyProperty(Context);
						}
						Verse::Persona::InjectLLMDescription(OwnerStruct, Property, *FieldObject);
					}
				}
			}

			Properties->SetField(PropName, ::MoveTemp(EntryValue));
			if (Entry.HasDefaultValueExpression())
			{
				Defaults.Add(Entry.Name->AsString());
			}
		});

	if (bFail)
	{
		return nullptr;
	}

	for (auto& [Key, Value] : Properties->Values)
	{
		const FString KeyStr(Key);
		if (IsStruct() || !Defaults.Find(KeyStr))
		{
			// we've no default or this is a struct... thus its 'required' to make a valid object
			RequiredProperties.Add(MakeShared<FJsonValueString>(KeyStr));
		}
	}

	Object->SetStringField(JSON_FIELD(Type), Persona::ObjectString);
	TUtf8StringBuilder<Names::DefaultNameLength> Description;
	AppendScopeName(Description);
	Object->SetStringField(JSON_FIELD(Description), Description.ToString());
	Object->SetObjectField(JSON_FIELD(Properties), ::MoveTemp(Properties));
	Object->SetArrayField(JSON_FIELD(Required), ::MoveTemp(RequiredProperties));

	return MakeShared<FJsonValueObject>(Object);
}

static bool Construct(
	FRunningContext Context,
	VClass& Class,
	VValue Object,
	VShape* Shape,
	EValueJSONFormat Format)
{
	bool bSkipBlocks = Format == EValueJSONFormat::Persistence;
	VValue CreateFieldToken = VValue::CreateFieldMarker();
	VValue SkipBlocks = bSkipBlocks ? GlobalTrue() : VValue();
	VValue InitSuper = VValue();
	FOpResult Result = Class.GetConstructor().InvokeWithSelf(Context, Object, {CreateFieldToken, SkipBlocks, InitSuper});
	if (!Result.IsReturn())
	{
		return false;
	}
	if (!CVarUObjectLeniency.GetValueOnAnyThread())
	{
		if (Class.GetKind() == VClass::EKind::Class && !bSkipBlocks)
		{
			Result = Class.GetBlocks().InvokeWithSelf(Context, Object, VFunction::Args{});
			if (!Result.IsReturn())
			{
				return false;
			}
		}
	}

	VEmergentType* EmergentType = Object.IsCellOfType<VValueObject>() ? Object.StaticCast<VValueObject>().GetEmergentType() : Object.StaticCast<VNativeConstructorWrapper>().GetEmergentType();
	for (auto& Field : Shape->Fields)
	{
		if (Field.Value.Type != EFieldType::Constant && !EmergentType->IsFieldCreated(Shape->GetFieldIndex(*Field.Key)))
		{
			return false;
		}
	}

	return true;
}

namespace
{
struct VField
{
	TWriteBarrier<VUniqueString> Name;
	TWriteBarrier<VValue> Value;
	TWriteBarrier<VValue> ValueDomain;
	bool bIsVar = false;
};
using VFields = TArray<VField>;
} // namespace

AUTORTFM_DISABLE static bool SetFieldsAndConstruct(
	FRunningContext Context,
	VClass& Class,
	VValueObject& Object,
	const VFields& Fields,
	EValueJSONFormat Format)
{
	VEmergentType* EmergentType = Object.GetEmergentType();
	VShape* Shape = EmergentType->Shape.Get();
	V_DIE_UNLESS(Shape != nullptr);
	for (auto&& [FieldName, FieldValue, FieldValueDomain, bIsVar] : Fields)
	{
		bool bFieldCreated = Object.CreateField(Context, *FieldName);
		V_DIE_UNLESS(bFieldCreated);
		const VShape::VEntry* Field = Shape->GetField(*FieldName);
		V_DIE_UNLESS(Field != nullptr);
		FOpResult Result;
		if (bIsVar)
		{
			V_DIE_UNLESS(Field->Type == EFieldType::Offset);
			VRef& Var = VRef::New(Context, FieldValueDomain.Get());
			Var.SetNonTransactionally(Context, FieldValue.Get());
			Result = Object.SetField(Context, *FieldName, Var);
		}
		else
		{
			Result = Object.SetField(Context, *FieldName, FieldValue.Get());
		}
		if (!Result.IsReturn())
		{
			return false;
		}
	}
	if (!Construct(Context, Class, Object, Shape, Format))
	{
		return false;
	}
	return true;
}

AUTORTFM_DISABLE static bool SetFieldsAndConstruct(
	FRunningContext Context,
	VClass& Class,
	VNativeConstructorWrapper& Wrapper,
	VNativeStruct& Object,
	const VFields& Fields,
	EValueJSONFormat Format)
{
	VEmergentType* EmergentType = Object.GetEmergentType();
	VShape* Shape = EmergentType->Shape.Get();
	V_DIE_UNLESS(Shape != nullptr);
	for (auto&& [FieldName, FieldValue, FieldValueDomain, bIsVar] : Fields)
	{
		bool bFieldCreated = Wrapper.CreateField(Context, *FieldName);
		V_DIE_UNLESS(bFieldCreated);
		const VShape::VEntry* Field = Shape->GetField(*FieldName);
		V_DIE_UNLESS(Field != nullptr);
		FOpResult Result;
		if (bIsVar && Field->Type == EFieldType::FVerseProperty)
		{
			// TODO: share this ref creation code with the interpreter using UVerseClass
			VRef& Var = VRef::New(Context, FieldValueDomain.Get());
			Var.SetNonTransactionally(Context, FieldValue.Get());
			Result = Object.SetField<EWriteMode::NonTransactional>(Context, *FieldName, Var);
		}
		else
		{
			Result = CVarUObjectLeniency.GetValueOnAnyThread()
					   ? Object.SetField<EWriteMode::NonTransactional>(Context, *FieldName, FieldValue.Get())
					   : Object.SetField<EWriteMode::Transactional>(Context, *FieldName, FieldValue.Get());
		}
		if (!Result.IsReturn())
		{
			return false;
		}
	}
	if (!Construct(Context, Class, Wrapper, Shape, Format))
	{
		return false;
	}
	return true;
}

AUTORTFM_DISABLE static bool SetFieldsAndConstruct(
	FRunningContext Context,
	VClass& Class,
	VNativeConstructorWrapper& Wrapper,
	UObject* Object,
	const VFields& Fields,
	EValueJSONFormat Format)
{
	VShape* Shape = UVerseClass::GetShape(Context, Object->GetClass());
	for (auto&& [FieldName, FieldValue, FieldValueDomain, bIsVar] : Fields)
	{
		bool bFieldCreated = Wrapper.CreateField(Context, *FieldName);
		V_DIE_UNLESS(bFieldCreated);
		const VShape::VEntry* Field = Shape->GetField(*FieldName);
		V_DIE_UNLESS(Field);
		// TODO: share this var creation code with the interpreter using uverseclass
		switch (Field->Type)
		{
			case EFieldType::FProperty:
			case EFieldType::FPropertyVar:
			{
				FOpResult Result = CVarUObjectLeniency.GetValueOnAnyThread()
									 ? VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, Object, Field->UProperty, FieldValue.Get())
									 : VNativeRef::Set<EWriteMode::Transactional>(Context, nullptr, Object, Field->UProperty, FieldValue.Get());
				if (!Result.IsReturn())
				{
					return false;
				}
				break;
			}
			case EFieldType::FVerseProperty:
				if (bIsVar)
				{
					VRef& Var = VRef::New(Context, FieldValueDomain.Get());
					Var.SetNonTransactionally(Context, FieldValue.Get());
					Field->UProperty->ContainerPtrToValuePtr<VRestValue>(Object)->Set(Context, Var);
				}
				else
				{
					Verse::VRestValue* DstValue = Field->UProperty->ContainerPtrToValuePtr<VRestValue>(Object);
					if (CVarUObjectLeniency.GetValueOnAnyThread())
					{
						DstValue->Set(Context, FieldValue.Get());
					}
					else
					{
						DstValue->SetTransactionally(Context, FieldValue.Get());
					}
				}
				break;
			case EFieldType::Offset:
			case EFieldType::Constant:
			default:
				V_DIE("Cannot convert %s from JSON", *FieldName->AsString());
				return false;
		}
	}
	if (!Construct(Context, Class, Wrapper, Shape, Format))
	{
		return false;
	}
	return true;
}

VValue VClass::FromJSONImpl(FRunningContext Context, const FJsonValue& JsonValue, EValueJSONFormat Format, FFromJsonCallback Callback)
{
	if (VValue Result = Callback(Context, JsonValue, *this))
	{
		return Result;
	}

	if (Format == EValueJSONFormat::Persona && !EnumHasAllFlags(Flags, EFlags::PersonaConstructible))
	{
		return {};
	}

	bool bPersistence = Format == EValueJSONFormat::Persistence;

	VArchetype& ClassArchetype = GetArchetype();
	if (bPersistence && !ClassArchetype.NextArchetype.IsUninitialized())
	{
		// Persistent classes may not inherit from any other class.
		return VValue();
	}

	const TSharedPtr<FJsonObject>* JsonObject;
	if (!JsonValue.TryGetObject(JsonObject))
	{
		return VValue();
	}

	TOptional<TMap<FJsonObject::FStringType, TSharedPtr<FJsonValue>>> JsonValues;
	if (bPersistence)
	{
		JsonValues = MapFromPersistentJson(**JsonObject);
	}
	else
	{
		JsonValues.Emplace();
		for (const TPair<FJsonObject::FStringType, TSharedPtr<FJsonValue>>& KV : (*JsonObject)->Values)
		{
			JsonValues->Emplace(FString(KV.Key), KV.Value);
		}
	}
	if (!JsonValues)
	{
		return VValue();
	}
	VFields Fields;
	Fields.Reserve(ClassArchetype.NumEntries);

	int32 BaseIndex = 0;
	bool bFail = false;
	WalkArchetypeFields(Context, *Archetype.Get(), *this, Archetype->NextArchetype.Get(Context).DynamicCast<VArchetype>(), BaseIndex, {},
		[&](VArchetype::VEntry& Entry, int32, VClass& Class) {
			if (bFail || Fields.FindByPredicate([&Entry](const VField& Field) { return Field.Name == Entry.Name; }))
			{
				return;
			}

			FString PropName = bPersistence ? FString(Verse::Names::RemoveQualifier(Entry.Name->AsString())) : Entry.Name->AsString();
			const TSharedPtr<FJsonValue>* FieldJsonValue = JsonValues->Find(FJsonObject::FStringType(PropName));
			if (!FieldJsonValue)
			{
				if (Format == EValueJSONFormat::Persona && IsStruct())
				{
					// does the VerseVM need this to fail? Or is that BPVM specific?
					bFail = true;
				}
				return;
			}
			if (Entry.Value.Get().IsCellOfType<VAccessor>())
			{
				UE_LOGF(LogVerseVM, Warning, "`FromJSON` does not support constructing via accessors yet! Instead, set the member the accessor accesses.");
				bFail = true;
				return;
			}
			VType* FieldType = Entry.Type.Get().Follow().DynamicCast<VType>();
			if (!FieldType)
			{
				bFail = true;
				return;
			}
			VValue FieldValue = FieldType->FromJSON(Context, **FieldJsonValue, Format, Callback);
			if (!FieldValue)
			{
				bFail = true;
				return;
			}
			Fields.Emplace(Entry.Name, TWriteBarrier<VValue>{Context, FieldValue}, TWriteBarrier<VValue>{}, Entry.IsVar());
		});

	if (bFail)
	{
		return VValue();
	}

	auto MakeObjectArchetype = [Context, &Fields]() -> VArchetype& {
		TArray<VArchetype::VEntry> Entries;
		Entries.Reserve(Fields.Num());
		for (VField& Field : Fields)
		{
			Entries.Emplace(VArchetype::VEntry::ObjectField(Context, *Field.Name));
		}
		return VArchetype::New(Context, VValue(), Entries);
	};

	if (IsNativeRepresentation())
	{
		if (IsStruct())
		{
			VNativeConstructorWrapper& Wrapper = NewNativeStruct(Context);
			VNativeStruct& Object = Wrapper.WrappedObject().StaticCast<VNativeStruct>();
			if (!SetFieldsAndConstruct(Context, *this, Wrapper, Object, Fields, Format))
			{
				return VValue();
			}
			return Object;
		}
		else
		{
			FOpResult Result = NewUObject(Context, MakeObjectArchetype());
			if (!Result.IsReturn())
			{
				return VValue();
			}
			VNativeConstructorWrapper& Wrapper = Result.Value.StaticCast<VNativeConstructorWrapper>();
			UObject* Object = Wrapper.WrappedObject().AsUObject();
			if (!SetFieldsAndConstruct(Context, *this, Wrapper, Object, Fields, Format))
			{
				return VValue();
			}
			return Object;
		}
	}

	VValueObject& Object = NewVObject(Context, MakeObjectArchetype());
	if (!SetFieldsAndConstruct(Context, *this, Object, Fields, Format))
	{
		return VValue();
	}
	return Object;
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
