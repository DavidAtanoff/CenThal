// Copyright Epic Games, Inc. All Rights Reserved.

#include "VerseVM/VVMVerseClass.h"
#include "AutoRTFM.h"
#include "Containers/VersePath.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/CoreRedirects.h"
#include "UObject/ObjectInstancingGraph.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/UnrealType.h"
#include "VerseVM/VVMCVars.h"
#include "VerseVM/VVMEngineEnvironment.h"
#include "VerseVM/VVMExecutionContext.h"
#include "VerseVM/VVMInstantiationContext.h"
#include "VerseVM/VVMNames.h"
#include "VerseVM/VVMPackageName.h"
#include "VerseVM/VVMRuntimeError.h"
#include "VerseVM/VVMVerse.h"
#include "VerseVM/VVMVerseClassFlags.h"
#include "VerseVM/VVMVerseFunction.h"
#include "VerseVM/VVMVerseStruct.h"

#if WITH_EDITOR
#include "Interfaces/ITargetPlatform.h"
#include "UObject/CookedMetaData.h"
#include "UObject/PropertyBagRepository.h"
#endif

#if WITH_VERSE_BPVM
#include "VerseVM/VBPVMDynamicProperty.h"
#endif

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "UObject/VerseValueProperty.h"
#include "VerseVM/Inline/VVMClassInline.h"
#include "VerseVM/Inline/VVMEnterVMInline.h"
#include "VerseVM/Inline/VVMNativeConstructorWrapperInline.h"
#include "VerseVM/Inline/VVMObjectInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMAccessor.h"
#include "VerseVM/VVMDebuggerVisitor.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMMap.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMNativeRef.h"
#endif

#if WITH_EDITORONLY_DATA
#include "UObject/PropertyStateTracking.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(VVMVerseClass)

DEFINE_LOG_CATEGORY_STATIC(LogSolGeneratedClass, Log, All);

bool CVar_UseAuthoredNameNonEditor = true;
static FAutoConsoleVariableRef CVarUseAuthoredNameNonEditor(TEXT("Verse.UseAuthoredNameNonEditor"), CVar_UseAuthoredNameNonEditor, TEXT(""));

const FName UVerseClass::NativeParentClassTagName("NativeParentClass");
const FName UVerseClass::PackageVersePathTagName("PackageVersePath");
const FName UVerseClass::PackageRelativeVersePathTagName("PackageRelativeVersePath");
const FName UVerseClass::InitCDOFunctionName(TEXT("$InitCDO"));
const FName UVerseClass::StructPaddingDummyName(TEXT("$StructPaddingDummy"));
const FTopLevelAssetPath UVerseClass::VerseClassTopLevelAssetPath(FName("/Script/CoreUObject"), FName("VerseClass"));

UVerseClass::UVerseClass(
	EStaticConstructor,
	FName InName,
	uint32 InSize,
	uint32 InAlignment,
	EClassFlags InClassFlags,
	EClassCastFlags InClassCastFlags,
	const TCHAR* InClassConfigName,
	EObjectFlags InFlags,
	ClassConstructorType InClassConstructor,
	ClassVTableHelperCtorCallerType InClassVTableHelperCtorCaller,
	FUObjectCppClassStaticFunctions&& InCppClassStaticFunctions)
	: Super(
		EC_StaticConstructor,
		InName,
		InSize,
		InAlignment,
		InClassFlags,
		InClassCastFlags,
		InClassConfigName,
		InFlags,
		InClassConstructor,
		InClassVTableHelperCtorCaller,
		MoveTemp(InCppClassStaticFunctions))
{
}

UVerseClass::UVerseClass(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UE::Core::FVersePath UVerseClass::GetVersePath() const
{
#if WITH_VERSE_BPVM
	if (MangledPackageVersePath.IsNone())
	{
		return {};
	}

	FString PackageVersePath = Verse::Names::Private::UnmangleCasedName(MangledPackageVersePath);
	FString VersePath = PackageRelativeVersePath.IsEmpty() ? PackageVersePath : PackageVersePath / PackageRelativeVersePath;
	UE::Core::FVersePath Result;
	ensure(UE::Core::FVersePath::TryMake(Result, MoveTemp(VersePath)));
	return Result;
#endif
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	TUtf8StringBuilder<Verse::Names::DefaultNameLength> ScopeName;
	Class->AppendScopeName(ScopeName);

	UE::Core::FVersePath Result;
	ensure(UE::Core::FVersePath::TryMake(Result, FString(ScopeName)));
	return Result;
#endif
}

EStructPropertyLinkFlags UVerseClass::GetPropertyLinkFlags(UStruct* ContainerStruct, FProperty* Property) const
{
	EStructPropertyLinkFlags OutFlags = Super::GetPropertyLinkFlags(ContainerStruct, Property);
	if (IsNativeBound() && ContainerStruct->IsA<UClass>())
	{
		OutFlags |= EStructPropertyLinkFlags::NeverLinkDestructor_Internal;
	}
	return OutFlags;
}

void UVerseClass::Link(FArchive& Ar, bool bRelinkExistingProperties)
{
	Super::Link(Ar, bRelinkExistingProperties);

// TODO: Move this to compiled package registry.  See https://jira.it.epicgames.com/browse/SOL-7734.
#if WITH_SERVER_CODE
	UPackage* Package = GetPackage();
	EVersePackageType PackageType;
	(void)Verse::FPackageName::GetVersePackageNameFromUPackagePath(Package->GetFName(), &PackageType);
	if (PackageType != EVersePackageType::VNI)
	{
		TArray<FCoreRedirect> Redirects;

		const FString& Name = GetName();

		FString OldName{Name};
		OldName.ReplaceCharInline('-', '_', ESearchCase::CaseSensitive);

		int32 Index;
		FString OldShortName = Name.FindLastChar('-', Index) ? Name.RightChop(Index + 1) : Name;

		TStringBuilder<Verse::Names::DefaultNameLength> OldPackageName(InPlace, Package->GetName(), '/', OldName);
		TStringBuilder<Verse::Names::DefaultNameLength> PackageName(InPlace, Package->GetName());
		TStringBuilder<Verse::Names::DefaultNameLength> OldFullName(InPlace, OldPackageName, '.', OldShortName);
		TStringBuilder<Verse::Names::DefaultNameLength> FullName(InPlace, PackageName, '.', Name);
		Redirects.Emplace(ECoreRedirectFlags::Type_Class, OldFullName.ToString(), FullName.ToString());

		FCoreRedirects::AddRedirectList(Redirects, FullName.ToString());
	}
#endif

#if WITH_VERSE_BPVM
	if (Ar.IsLoading())
	{
		// Make sure coroutine task classes have been loaded before native binding
		if (!FPlatformProperties::RequiresCookedData())
		{
			for (UVerseClass* TaskClass : TaskClasses)
			{
				if (TaskClass)
				{
					Ar.Preload(TaskClass);
				}
			}
		}
	}
#endif

	// For native classes, we need to bind them explicitly here -- we need to do it
	// after Super::Link() (so it can find named properties/functions), but before
	// CDO creation (since binding can affect property offsets and class size).
	if (IsNativeBound())
	{
		Verse::IEngineEnvironment* Environment = Verse::VerseVM::GetEngineEnvironment();
		ensure(Environment);
#if WITH_VERSE_BPVM
		Environment->TryBindVniType(this);
#endif
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
		Environment->TryBindVniType(&Class->GetPackage(), this);
#endif
	}

#if WITH_VERSE_BPVM
	if (!IsUHTNative())
	{
		// Connect native function thunks
		for (const FNativeFunctionLookup& NativeFunctionLookup : NativeFunctionLookupTable)
		{
			UFunction* Function = FindFunctionByName(NativeFunctionLookup.Name);
			if (ensureMsgf(Function, TEXT("The function: %s could not be found, even though it should have been available!"), *NativeFunctionLookup.Name.ToString()))
			{
				Function->SetNativeFunc(NativeFunctionLookup.Pointer);
				Function->FunctionFlags |= FUNC_Native;
			}
		}
	}
#endif

	// Manually build token stream for Solaris classes but only when linking cooked classes or
	// when linking a duplicated class during class reinstancing.
	// However, when classes are first created (from script source) this happens in
	// FAssembleClassOrStructTask as we want to make sure all dependencies are properly set up first
	if (Ar.IsLoading() || HasAnyClassFlags(CLASS_NewerVersionExists))
	{
		AssembleReferenceTokenStream(bRelinkExistingProperties);
	}

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	if (Ar.IsLoading())
	{
		Verse::FAllocationContext Context = Verse::FAllocationContextPromise{};

		V_DIE_UNLESS_MSG(Class, "Missing VClass for %s. This class should have been created in-memory from a VClass, not loaded from a cooked package.", *GetFullName());
		SetShape(Context, Class->CreateShapeForExistingUStruct(Context));
	}
#endif

	// Default to the CVar setting for whether or not this class should use dynamic instancing.
	// Note that for backwards compatibility with existing live projects, we may need to disable
	// this below, depending on whether the class was compiled using instanced reference semantics.
	bNeedsDynamicSubobjectInstancing = Verse::CVarUseDynamicSubobjectInstancing.GetValueOnAnyThread();

	// This trait is set if the class is using explicit instanced reference semantics on its
	// generated object properties. Note that this differs from 'CLASS_HasInstancedReference'
	// which is used by engine code to signal the class *may* reference an instanced subobject.
	//
	// If this type was generated using explicit instanced reference semantics, disallow dynamic
	// subobject instancing at runtime to ensure backwards compatibility with legacy script code.
	const bool bHasInstancedPropertySemantics = HasInstancedSemantics();
	if (bHasInstancedPropertySemantics
		&& bNeedsDynamicSubobjectInstancing)
	{
		bNeedsDynamicSubobjectInstancing = false;
	}

	// If a class is compiled w/ support for dynamic references, but dynamic subobject instancing is disabled
	// for Verse types at runtime, fall back to forcing explicit instancing flags on all reference properties.
	// This makes it possible to patch dynamic instancing off at link time to avoid re-cooking engine content.
	if (Ar.IsLoading())
	{
		const bool bHasDynamicInstancedReferenceSupport = !bHasInstancedPropertySemantics;
		if (bHasDynamicInstancedReferenceSupport
			&& !bNeedsDynamicSubobjectInstancing)
		{
			DisableDynamicInstancedReferenceSupport();
		}
#if WITH_EDITOR && !UE_BUILD_SHIPPING
		else
		{
			// In this case, dynamic subobject instancing is enabled, but the (cooked) class may have been packaged
			// with it disabled. We enable support in this case since it is an inheritable class trait (e.g. prefabs).
			// Note: This is restricted to the editor context, because we only need it to support testing/iteration,
			// where we might be running the editor against engine data (e.g. VNI types) cooked w/ the CVar turned off.
			if (!bHasDynamicInstancedReferenceSupport
				&& bNeedsDynamicSubobjectInstancing)
			{
				EnableDynamicInstancedReferenceSupport();
			}
		}
#endif
		// When set, objects of this type have references to archetype instantiations that require construction on load.
		if (HasTransientSubobjects())
		{
			bNeedsPostLoadSubobjectInstancing = true;
		}
	}
}

void UVerseClass::PreloadChildren(FArchive& Ar)
{
#if WITH_VERSE_BPVM
	// Preloading functions for UVerseClass may end up with circular dependencies regardless of EDL being enabled or not
	// Since UVerseClass is not a UBlueprintGeneratedClass it does not use the deferred dependency loading path in FLinkerLoad
	// so we don't want to deal with circular dependencies here. They will be resolved by the linker eventually though.
	for (UField* Field = Children; Field; Field = Field->Next)
	{
		if (!Cast<UFunction>(Field))
		{
			Ar.Preload(Field);
		}
	}
#endif
}

FString UVerseClass::GetAuthoredNameForField(const FField* Field) const
{
	if (Field)
	{
#if WITH_EDITORONLY_DATA
		static const FName NAME_DisplayName("DisplayName");
		if (const FString* NativeDisplayName = Field->FindMetaData(NAME_DisplayName))
		{
			return *NativeDisplayName;
		}
#else
		if (CVar_UseAuthoredNameNonEditor)
		{
			return Verse::Names::UEPropToVerseName(Field->GetName());
		}
#endif
	}

	return Super::GetAuthoredNameForField(Field);
}

bool UVerseClass::IsAsset() const
{
#if WITH_EDITOR
	// Don't include placeholder types that were created for missing type imports on load.
	// These allow exports to be serialized to avoid data loss, but should not be an asset.
	if (UE::FPropertyBagRepository::IsPropertyBagPlaceholderType(this))
	{
		return false;
	}
#endif

	return true;
}

void UVerseClass::GetPreloadDependencies(TArray<UObject*>& OutDeps)
{
	Super::GetPreloadDependencies(OutDeps);

	// UClass::Serialize() will instantiate this class's CDO, but that means we need the
	// Super's CDO serialized before this class serializes
	OutDeps.Add(GetSuperClass()->GetDefaultObject());

	// For natively-bound classes, we need their coroutine objects serialized first,
	// because we bind on Link() (called during Serialize()) and native binding
	// for a class will binds its coroutine task objects at the same time.
	if (IsNativeBound())
	{
		for (UVerseClass* TaskClass : TaskClasses)
		{
			OutDeps.Add(TaskClass);
		}
	}
}

void UVerseClass::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	Super::GetAssetRegistryTags(Context);

#if WITH_EDITOR
	// NativeParentClass
	{
		FString NativeParentClassName;
		if (UClass* ParentClass = GetSuperClass())
		{
			// Walk up until we find a native class
			UClass* NativeParentClass = ParentClass;
			while (!NativeParentClass->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic))
			{
				NativeParentClass = NativeParentClass->GetSuperClass();
			}
			NativeParentClassName = FObjectPropertyBase::GetExportPath(NativeParentClass);
		}
		else
		{
			NativeParentClassName = TEXT("None");
		}

		Context.AddTag(FAssetRegistryTag(NativeParentClassTagName, MoveTemp(NativeParentClassName), FAssetRegistryTag::TT_Alphabetical));
	}
	// PackageVersePath
	if (!MangledPackageVersePath.IsNone())
	{
		Context.AddTag(FAssetRegistryTag(PackageVersePathTagName, Verse::Names::Private::UnmangleCasedName(MangledPackageVersePath), FAssetRegistryTag::TT_Alphabetical));
	}
	// PackageRelativeVersePath
	{
		Context.AddTag(FAssetRegistryTag(PackageRelativeVersePathTagName, PackageRelativeVersePath, FAssetRegistryTag::TT_Alphabetical));
	}
	// NOTE: (yiliang.siew) If this is an "internal compiler detail" class instance, just add the AR tag so that we don't need to load
	// the whole asset in order to determine this.
	if (SolClassFlags & VCLASS_InternalCodegen)
	{
		Context.AddTag(FAssetRegistryTag(Verse::InternalCodegenAssetRegistryTagName, TEXT("True"), FAssetRegistryTag::TT_Hidden));
	}
#endif
}

#if WITH_EDITOR
void UVerseClass::GetAssetRegistryTagMetadata(TMap<FName, FAssetRegistryTagMetadata>& OutMetadata) const
{
	Super::GetAssetRegistryTagMetadata(OutMetadata);

	{
		FAssetRegistryTagMetadata Metadata;
		Metadata.DisplayName = NSLOCTEXT("UVerseClass", "NativeParentClass", "Native Parent Class");
		Metadata.TooltipText = NSLOCTEXT("UVerseClass", "NativeParentClassTooltip", "The native parent class of this type.");
		OutMetadata.Emplace(NativeParentClassTagName, MoveTemp(Metadata));
	}
	{
		FAssetRegistryTagMetadata Metadata;
		Metadata.DisplayName = NSLOCTEXT("UVerseClass", "PackageVersePath", "Package Verse Path");
		Metadata.TooltipText = NSLOCTEXT("UVerseClass", "PackageVersePathTooltip", "The verse path of the package of this type.");
		OutMetadata.Emplace(PackageVersePathTagName, MoveTemp(Metadata));
	}
	{
		FAssetRegistryTagMetadata Metadata;
		Metadata.DisplayName = NSLOCTEXT("UVerseClass", "PackageRelativeVersePath", "Relative Package Verse Path");
		Metadata.TooltipText = NSLOCTEXT("UVerseClass", "PackageRelativeVersePathTooltip", "The relative verse path of the package of this type.");
		OutMetadata.Emplace(PackageRelativeVersePathTagName, MoveTemp(Metadata));
	}
}
#endif

static bool NeedsInit(UObject* InObj)
{
	if (InObj->HasAnyFlags(RF_NeedPostLoad))
	{
		return false;
	}
	if (InObj->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		if (InObj->GetClass()->HasAnyFlags(RF_NeedPostLoad))
		{
			return false;
		}
	}
	return true;
}

void UVerseClass::PostInitInstance(UObject* InObj, FObjectInstancingGraph* InstanceGraph)
{
	Super::PostInitInstance(InObj, InstanceGraph);

	if ((SolClassFlags & EVerseClassFlags::VCLASS_HasVar)
		&& Verse::FInstantiationScope::Context.bModuleTopLevel)
	{
		RAISE_VERSE_RUNTIME_ERROR_FORMAT(::Verse::ERuntimeDiagnostic::ErrRuntime_UnimplementedGlobalVariable, TEXT("Allocating a variable (via instantiation of %s) at global scope is not yet implemented."), *GetVersePath().ToString());
		return;
	}

	if (NeedsInit(InObj))
	{
		CallInitInstanceFunctions(InObj, InstanceGraph);
		AddSessionVars(InObj);
	}

#if WITH_VERSE_BPVM
	AddPersistentVars(InObj);
#endif
}

void UVerseClass::PostLoadInstance(UObject* InObj)
{
	Super::PostLoadInstance(InObj);

	// For VerseVM: The loaded object should already contain everything it needs
	// and additionally calling the constructor should not be necessary
#if WITH_VERSE_BPVM
	CallInitInstanceFunctions(InObj, nullptr);
#endif
	AddSessionVars(InObj);
}

void UVerseClass::SetupObjectInitializer(FObjectInitializer& ObjectInitializer) const
{
	Super::SetupObjectInitializer(ObjectInitializer);

	UObject* Obj = ObjectInitializer.GetObj();

	// Previous versions of the editor added subobjects to prefabs without propagating these flags.
	// Fix them up here on load, so e.g. CallInitInstanceFunctions can check RF_ArchetypeObject.
	if (Obj->HasAllFlags(RF_NeedLoad))
	{
		Obj->SetFlags(Obj->GetOuter()->GetMaskedFlags(RF_PropagateToSubObjects));
	}

	// In Verse, a subclass initializer can override a member type/value if the member is defined by a parent class. Unlike
	// native UClass types though, a parent class constructor is NOT run on the CDO, and the initializer for the CDO will
	// either be run AFTER native UObject initialization (in the compiled class case), or will not be run at all (in the
	// serialize/cooked class case). Both mean that overrides are not set until after the CDO has been natively constructed.
	//
	// Since a Verse class is non-native, when constructing its CDO, all properties will be initialized from the parent CDO.
	// This also means any object properties that are initialized with a reference to a unique subobject scoped to the parent
	// CDO will also be cloned to the new CDO by the UObject initializer through native subobject instancing (by convention).
	//
	// Members set to a default value expression in the Verse initializer *can* be excluded from instancing - they will already
	// be (a) instanced by the Verse initializer in the compiled case, or (b) instanced by the loader in the serialized case.
	// Exclusion is not required, but it will eliminate unnecessary overhead due to redundant instancing in the compiled case.
	// @see UVerseClass::RenameDefaultSubobjects() for how this is otherwise handled (it may move aside the first instance).
	//
	// Inherited members set to a new class default value with an <override> expression *must* be excluded from instancing to
	// avoid data loss in the cooked/serialized case. This will work fine in the compiled case, since the Verse initializer
	// gets called after the compiler natively constructs the CDO, and will always evaluate the Verse initializer's expression.
	//
	// However, it will fail in the cooked/serialized case, as the Verse initializer will not get called, and the loader will
	// fail to match the inherited type (that will otherwise get created here through subobject instancing) to the export.
	// In that case, the exported instance will fail to be loaded, and the CDO is left with a default-initialized cloned copy
	// of the inherited member's default value type. This is a use case that otherwise leads to data loss. @see UE-218685
	//
	// By excluding object properties that fit into either scenario from subobject instancing on the CDO, we can avoid this.
	if (Obj == GetDefaultObject(/*bCreateIfNeeded =*/false))
	{
#if WITH_VERSE_BPVM
		// There's no reason to exclude anything if we don't have an archetype to initialize from that would need instancing.
		const UObject* ObjArchetype = ObjectInitializer.GetArchetype();
		if (ObjArchetype)
		{
			// When constructing a CDO, the archetype is also expected to be a CDO (default data for the parent class).
			checkf(ObjArchetype->HasAnyFlags(RF_ClassDefaultObject) && ObjArchetype->GetClass() == Obj->GetClass()->GetSuperClass(),
				TEXT("SetupObjectInitializer for CDO (%s): The object archetype (%s) is not the parent class default object."),
				*Obj->GetPathName(),
				*ObjArchetype->GetPathName());

			// When constructing a CDO, the archetype is expected to have already been compiled or serialized on load as a dependency.
			checkf(!ObjArchetype->HasAnyFlags(RF_NeedLoad),
				TEXT("SetupObjectInitializer for CDO (%s): The object archetype (%s) is being loaded, but has not yet been fully serialized."),
				*Obj->GetPathName(),
				*ObjArchetype->GetPathName());

			// Note: This array is compiled to/cooked out along with the UVerseClass object. The Verse compiler injects any members that
			// are known to be set by the initializer. This may or may not be an <override> expression, but we need to exclude both types.
			for (const TFieldPath<FProperty>& FieldPath : PropertiesWrittenByInitCDO)
			{
				TArray<const FStructProperty*> EncounteredStructProps;

				// We only need to consider object ptrs for exclusion from subobject instancing, including optionals and containers.
				const FProperty* Property = FieldPath.Get();
				if (!Property
					|| !Property->ContainsObjectReference(EncounteredStructProps))
				{
					continue;
				}

				ObjectInitializer.AddPropertyToSubobjectExclusionList(Property);
			}
		}
#elif WITH_VERSE_VM || defined(__INTELLISENSE__)
		// TODO: Class is not set yet for UHT Verse classes.
		// If/when they can override initializers, they will need to add these same exclusions somehow.
		if (Class)
		{
			// Don't instance overridden fields. These will be overwritten by this class's default initializers.
			Verse::FAllocationContext Context = Verse::FAllocationContextPromise{};
			Verse::VArchetype& Archetype = *Class->Archetype;
			Verse::VShape& VerseShape = const_cast<Verse::VRestValue&>(Shape).Get(Context).StaticCast<Verse::VShape>();
			for (uint32 Index = 0; Index < Archetype.NumEntries; ++Index)
			{
				Verse::VArchetype::VEntry& ArchetypeEntry = Archetype.Entries[Index];
				const Verse::VShape::VEntry* Field = VerseShape.GetField(*ArchetypeEntry.Name);
				if (ArchetypeEntry.HasDefaultValueExpression() && Field->IsProperty())
				{
					ObjectInitializer.AddPropertyToSubobjectExclusionList(Field->UProperty);
				}
			}
		}
#endif
		// Exclude any additional properties that were bound by Verse field semantics. This allows us to bypass instancing
		// when constructing class default object exports on load in a cooked build, where the class is not recompiled on load.
		//
		// For the same reasons as noted above, this is required for <override> to work with @field semantics in a cooked class.
		//
		// Note that this will not currently support "partial" exclusion of e.g. a subset of values in a container property.
		// In practice, this would only be an issue if we default-initialized the field as part of the native C++ class ctor.
		// However, the current implementation replaces any native ctor default-initialized value (see PrePopulateVerseFields).
		for (FProperty* RefProp = RefLink; RefProp; RefProp = RefProp->NextRef)
		{
			const UVerseClass* OwnerAsVerseClass = RefProp->GetOwner<UVerseClass>();
			if (!OwnerAsVerseClass)
			{
				continue;
			}

			// Note: Field mappings are always scoped to the owning class for <override>
			const TFieldPath<FProperty>* VerseFieldPtr = OwnerAsVerseClass->VerseFields.Find(RefProp);
			if (!VerseFieldPtr)
			{
				continue;
			}

			const FProperty* VerseField = VerseFieldPtr->Get();
			if (VerseField
				&& !ObjectInitializer.IsPropertyInSubobjectExclusionList(VerseField))
			{
				ObjectInitializer.AddPropertyToSubobjectExclusionList(VerseField);
			}
		}
	}
}

namespace VerseClassPrivate
{
enum ETraverseSubobjectsFlag : uint32
{
	None = 0,
	NoNameGeneration = (1 << 0)
};

void TraverseSubobjectsInternal(UObject* InObject, void* ContainerPtr, FProperty* RefProperty, const FString& Prefix, const TFunctionRef<void(UObject* /*Subobject*/, const FString& /*CanonicalSubobjectName*/)> Operation, ETraverseSubobjectsFlag Flags);

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
AUTORTFM_DISABLE bool TraverseValueInternal(
	Verse::FAllocationContext Context, bool bTransactional,
	UObject* InObject, Verse::VValue Value, const FString& PropName, const FString& Prefix, int32 Index,
	ETraverseSubobjectsFlag Flags, const TFunctionRef<void(UObject* /*Subobject*/, const FString& /*CanonicalSubobjectName*/)> Operation);
#endif
} // namespace VerseClassPrivate

#if WITH_EDITORONLY_DATA
bool UVerseClass::CanCreateInstanceDataObject() const
{
	return true;
}

void UVerseClass::SerializeDefaultObject(UObject* Object, FStructuredArchive::FSlot Slot)
{
	Super::SerializeDefaultObject(Object, Slot);

	TrackDefaultInitializedProperties(Object);
}

static void MaybeSetHasTransientSubobjectsTrait(UObject* CDO, const FProperty* Property, const UObject* Subobject)
{
	UVerseClass* VerseClass = Cast<UVerseClass>(CDO->GetClass());
	if (!ensure(VerseClass))
	{
		return;
	}

	// Already set; we can early exit.
	if (VerseClass->HasTransientSubobjects())
	{
		return;
	}

	// This might be the inner property of an optional/container field.
	const FProperty* OwnerProperty = Property->GetOwnerProperty();

	// We only need to consider non-native transient properties - these require instancing on load if they are not
	// serialized and if they reference a default subobject instantiation within the CDO, which we determine below.
	// Native transient properties are expected to be natively instanced and should not require instancing on load.
	const bool bIsTransientReference = OwnerProperty->HasAnyPropertyFlags(CPF_Transient);
	if (bIsTransientReference && OwnerProperty->IsNative())
	{
		return;
	}

	// Look for references to default subobject instantiations for which (a) the reference is transient or (b) is of a
	// transient class type. The referenced object will not be serialized/exported for instances of this class on save,
	// and requires instancing on load to reconstruct default subobjects that are not serialized/exported at cook time.
	// This is necessary because, unlike for Blueprints that inherit from native C++ types for example, serialized Verse
	// types are default-initialized from the CDO as their archetype, and do not get initialized by a UClass constructor.
	//
	// To facilitate this, we set a class trait to direct cooked builds to always instance objects of this type on load.
	//
	// Notes:
	//  - CPF_Transient reference properties will always be serialized on save for Verse CDOs at cook time.
	//  - CPF_Transient reference properties will not be serialized on save for all other instances of this type.
	//	- RF_ArchetypeObject allocations of CLASS_Transient types are not marked RF_Transient and will be serialized/exported on save.
	//	- Non-RF_ArchetypeObject allocations of CLASS_Transient types are marked RF_Transient and will not be serialized/exported on save.
	const bool bIsInstancedReference = OwnerProperty->HasAnyPropertyFlags(CPF_InstancedReference) || Subobject->IsInOuter(CDO);
	if (bIsInstancedReference && (bIsTransientReference || Subobject->GetClass()->HasAnyClassFlags(CLASS_Transient)))
	{
		VerseClass->SetHasTransientSubobjects();
	}
}

struct FInitializedPropertyVisitor
{
	// Keep track of visited property-owner pairs to avoid referencing cycles.
	TSet<TTuple<const FProperty*, void*>> VisitedPropOwners;
	const UObject* CDO;

	FInitializedPropertyVisitor(const UObject* InCDO)
		: CDO(InCDO)
	{
	}

	EPropertyVisitorControlFlow operator()(const FPropertyVisitorContext& Context)
	{
		const FPropertyVisitorPath& PropertyPath = Context.Path;
		const FPropertyVisitorData& Data = Context.Data;
		const FProperty* Property = PropertyPath.Top().Property;
		void* Owner = Data.ParentStructData;
		TTuple<const FProperty*, void*> PropOwner(Property, Owner);

		if (!Property || VisitedPropOwners.Contains(PropOwner))
		{
			return EPropertyVisitorControlFlow::StepOver;
		}

		bool bIsInCDO = true;
		bool bIsCreatedByCDO = true;

		const UStruct* OwnerType = PropertyPath.Top().ParentStructType;

		if (OwnerType && OwnerType->IsChildOf<UObject>())
		{
			const UObject* OwnerObject = (const UObject*)Owner;

			if (OwnerObject)
			{
				bIsInCDO = OwnerObject->IsInOuter(CDO);

				// When this property has a default, then the property itself is already considered initialized.
				// However, if the outer CDO has overridden that default, it may contain additional subobjects
				// that need to be visited, in case they have any required properties.
				bIsCreatedByCDO = Property->HasAnyPropertyFlags(CPF_RequiredParm);
				if (!bIsCreatedByCDO)
				{
					const UClass* OwnerClass = CastChecked<UClass>(OwnerType);
					bIsCreatedByCDO = OwnerObject->GetArchetype() == OwnerClass->GetDefaultObject(false);
				}
			}
		}

		// It is possible for the property and owner types to differ during re-instancing
		// when a new CDO is created. Skip tracking in this case.
		if (bIsInCDO && OwnerType && Property->HasAnyPropertyFlags(CPF_RequiredParm) && OwnerType->IsChildOf(Property->GetOwnerStruct()))
		{
			UE::FInitializedPropertyValueState(OwnerType, Owner).Set(Property);
		}

		VisitedPropOwners.Add(PropOwner);

		if (bIsInCDO && bIsCreatedByCDO)
		{
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
			if (const FVRestValueProperty* VerseProperty = CastField<FVRestValueProperty>(Property))
			{
				VerseProperty->VisitProperties(Context, *this);
			}
#endif
			return EPropertyVisitorControlFlow::StepInto;
		}
		else
		{
			return EPropertyVisitorControlFlow::StepOver;
		}
	}
};

static void TrackDefaultInitializedPropertiesInSubobject(UObject* Subobject, const UObject* CDO)
{
	FInitializedPropertyVisitor Visitor(CDO);
	Subobject->GetClass()->Visit(Subobject, Visitor);
}

void UVerseClass::TrackDefaultInitializedProperties(void* DefaultData) const
{
	if (HasAnyClassFlags(CLASS_NewerVersionExists))
	{
		return;
	}

	UObject* CDO = (UObject*)DefaultData;

	// PropertiesWrittenByInitCDO will not contain the properties initialized in the super-class so
	// we need to traverse the class hierarchy upwards until we no longer have a Verse class.
	if (const UVerseClass* VerseSuperClass = Cast<UVerseClass>(GetSuperClass()))
	{
		VerseSuperClass->TrackDefaultInitializedProperties(CDO);
	}

	for (const TFieldPath<FProperty>& FieldPath : PropertiesWrittenByInitCDO)
	{
		// Recursively mark every sub-object in the property as initialized.
		FProperty* Property = FieldPath.Get();

		// Track <override> properties initialized by this class.
		if (Property->HasAnyPropertyFlags(CPF_RequiredParm))
		{
			UE::FInitializedPropertyValueState(CDO).Set(Property);
		}

		VerseClassPrivate::TraverseSubobjectsInternal(
			CDO, CDO, Property, /*Prefix*/ FString(), [CDO, Property](UObject* Subobject, const FString& CanonicalSubobjectName) {
				TrackDefaultInitializedPropertiesInSubobject(Subobject, CDO);
				MaybeSetHasTransientSubobjectsTrait(CDO, Property, Subobject);
			},
			VerseClassPrivate::ETraverseSubobjectsFlag::NoNameGeneration);
	}
}

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
void UVerseClass::TrackDefaultInitializedPropertiesOfGlobalField(
	Verse::FAllocationContext Context,
	UObject* Module,
	Verse::VValue Value)
{
	VerseClassPrivate::TraverseValueInternal(
		Context,
		false,
		Module,
		Value,
		FString(),
		FString(),
		0,
		VerseClassPrivate::ETraverseSubobjectsFlag::NoNameGeneration,
		[Module](UObject* Obj, const FString& Name) { TrackDefaultInitializedPropertiesInSubobject(Obj, Module); });
}
#endif
#endif // WITH_EDITORONLY_DATA

#if WITH_EDITOR
FTopLevelAssetPath UVerseClass::GetReinstancedClassPathName_Impl() const
{
#if WITH_VERSE_COMPILER
	return FTopLevelAssetPath(PreviousPathName);
#else
	return nullptr;
#endif
}
#endif

const TCHAR* UVerseClass::GetPrefixCPP() const
{
	return TEXT("");
}

#if WITH_VERSE_BPVM
void UVerseClass::AddPersistentVars(UObject* InObj)
{
	// UHT generated types will need to be constructed prior to the engine environment.  So only call if we have these vars
	if (!PersistentVars.IsEmpty())
	{
		Verse::IEngineEnvironment* Environment = Verse::VerseVM::GetEngineEnvironment();
		ensure(Environment);
		Environment->AddPersistentVars(InObj, PersistentVars);
	}
}
#endif

void UVerseClass::AddSessionVars(UObject* InObj)
{
	// UHT generated types will need to be constructed prior to the engine environment.  So only call if we have these vars
	if (!SessionVars.IsEmpty())
	{
		Verse::IEngineEnvironment* Environment = Verse::VerseVM::GetEngineEnvironment();
		ensure(Environment);
		Environment->AddSessionVars(InObj, SessionVars);
	}
}

void UVerseClass::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	Super::PreSave(ObjectSaveContext);

#if WITH_EDITOR
	// Hack: if cooking for clients, clear the InitInstanceFunction to make sure clients don't try to run it.
	if (ObjectSaveContext.IsCooking()
		&& ensure(ObjectSaveContext.GetTargetPlatform())
		&& !ObjectSaveContext.GetTargetPlatform()->IsServerOnly())
	{
		InitInstanceFunction = nullptr;
	}

	// Note: We do this in PreSave rather than PreSaveRoot since Verse stores multiple generated types in the same package, and PreSaveRoot is only called for the main "asset" within each package
	if (ObjectSaveContext.IsCooking() && (ObjectSaveContext.GetSaveFlags() & SAVE_Optional))
	{
		if (!CachedCookedMetaDataPtr)
		{
			CachedCookedMetaDataPtr = CookedMetaDataUtil::NewCookedMetaData<UClassCookedMetaData>(this, "CookedClassMetaData");
		}

		CachedCookedMetaDataPtr->CacheMetaData(this);

		if (!CachedCookedMetaDataPtr->HasMetaData())
		{
			CookedMetaDataUtil::PurgeCookedMetaData<UClassCookedMetaData>(CachedCookedMetaDataPtr);
		}
	}
	else if (CachedCookedMetaDataPtr)
	{
		CookedMetaDataUtil::PurgeCookedMetaData<UClassCookedMetaData>(CachedCookedMetaDataPtr);
	}
#endif
}

void UVerseClass::CallInitInstanceFunctions(UObject* InObj, FObjectInstancingGraph* InstanceGraph)
{
	if (!Verse::FInstantiationScope::Context.bCallInitInstanceFunctions)
	{
		return;
	}

#if WITH_EDITOR
	InObj->SetFlags(RF_Transactional);
#endif

	if (InObj->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		// The construction of the CDO should not invoke class blocks.
		return;
	}
	if (InstanceGraph && InObj == InstanceGraph->GetDestinationRoot())
	{
		// The root's class blocks will be invoked by the archetype instantiation.
		return;
	}

	if (GIsClient && !GIsEditor && !WITH_VERSE_COMPILER)
	{
		// SOL-4610: Don't run the InitInstance function on clients.
		return;
	}

#if WITH_VERSE_BPVM
	verse::FEnsureGameThreadScope EnsureGameThreadScope;

	if (InitInstanceFunction)
	{
		// Make sure the function has been loaded and PostLoaded
		checkf(!InitInstanceFunction->HasAnyFlags(RF_NeedLoad), TEXT("Trying to call \"%s\" on \"%s\" but the function has not yet been loaded."), *InitInstanceFunction->GetPathName(), *InObj->GetFullName());
		InitInstanceFunction->ConditionalPostLoad();

		// DANGER ZONE: We're allowing VM code to potentially run during post load so fingers crossed it has no side effects
		TGuardValue<bool> GuardIsRoutingPostLoad(FUObjectThreadContext::Get().IsRoutingPostLoad, false);
		if (AutoRTFM::IsClosed())
		{
			InObj->ProcessEvent(InitInstanceFunction, nullptr);
		}
		else
		{
			// Ideally we would assert !AutoRTFM::IsTransactional() here, but some systems (predicts, persistence/session tests)
			// create UObjects in the open during transactions. Starting a new transaction for those particular cases works.

			// #jira SOL-6303: What should we do with a failing transaction?
			UE_AUTORTFM_TRANSACT
			{
				InObj->ProcessEvent(InitInstanceFunction, nullptr);
			};
		}
	}

	CallPropertyInitInstanceFunctions(InObj, InstanceGraph);
#endif
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	Verse::FOpResult OpResult{Verse::FOpResult::Error};
	AutoRTFM::Open([&] AUTORTFM_DISABLE {
		Verse::FRunningContext Context = Verse::FRunningContextPromise{};
		Context.EnterVM([&] {
			// We may be running on an object of some prefab subclass that was never reflected to Verse.
			// To ensure we have at least *some* shape, call GetShapeForLoadField instead of GetShape.
			Verse::VShape& ClassShape = UVerseClass::GetShapeForLoadField(Context, InObj->GetClass());

			Verse::VNativeConstructorWrapper& Wrapper = Verse::VNativeConstructorWrapper::New(Context, *Class, ClassShape, InObj);

			// Mark all properties as initialized, including those without default initializers.
			// TODO(SOL-8957): This is too permissive- some non-default properties may be
			// initialized via the object's archetype, or by the caller of NewObject, but we should
			// catch those that are still uninitialized.
			Verse::VEmergentType* EmergentType = Wrapper.GetEmergentType();
			for (auto It = ClassShape.CreateFieldsIterator(); It; ++It)
			{
				if (It->Value.IsProperty())
				{
					int32 FieldIndex = It.GetId().AsInteger();
					if (!EmergentType->IsFieldCreated(FieldIndex))
					{
						EmergentType = EmergentType->MarkFieldAsCreated(Context, FieldIndex);
						Verse::VValue Placeholder = Wrapper.UnifyField(Context, FieldIndex);
						V_DIE_IF(Placeholder);
					}
				}
			}
			Wrapper.SetEmergentType(Context, EmergentType);

			// TODO: Consider wrapping the constructor/blocks procedure below with a stub that runs UnifyNativeObject?
			Wrapper.SelfPlaceholder.Set(Context, InObj);

			if (Verse::CVarUObjectLeniency.GetValueOnAnyThread())
			{
				Verse::VValue CreateFieldToken = Verse::VValue::CreateFieldMarker();
				Verse::VValue SkipBlocks = Verse::VValue();
				Verse::VValue InitSuper = Verse::VValue();
				OpResult = Class->GetConstructor().InvokeWithSelf(Context, Wrapper, {CreateFieldToken, SkipBlocks, InitSuper});
			}
			else
			{
				OpResult = Class->GetBlocks().InvokeWithSelf(Context, Wrapper, Verse::VFunction::Args{});
			}
		});
	});
	ensure(OpResult.IsReturn());
#endif
}

void UVerseClass::CallPropertyInitInstanceFunctions(UObject* InObj, FObjectInstancingGraph* InstanceGraph)
{
	checkf(!GIsClient || GIsEditor || WITH_VERSE_COMPILER, TEXT("SOL-4610: UEFN clients are not supposed to run Verse code."));

	for (FProperty* Property = (FProperty*)ChildProperties; Property; Property = (FProperty*)Property->Next)
	{
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			UVerseStruct* SolarisStruct = Cast<UVerseStruct>(StructProperty->Struct);
			if (SolarisStruct && SolarisStruct->InitFunction && SolarisStruct->ModuleClass && (!InstanceGraph || !InstanceGraph->IsPropertyInSubobjectExclusionList(Property)))
			{
				UObject* ModuleCDO = SolarisStruct->ModuleClass->GetDefaultObject();
				void* Data = StructProperty->ContainerPtrToValuePtr<void>(InObj);
				if (AutoRTFM::IsClosed())
				{
					ModuleCDO->ProcessEvent(SolarisStruct->InitFunction, Data);
				}
				else
				{
					// #jira SOL-6303: What should we do with a failing transaction?
					check(!AutoRTFM::IsTransactional());
					UE_AUTORTFM_TRANSACT
					{
						ModuleCDO->ProcessEvent(SolarisStruct->InitFunction, Data);
					};
				}
			}
		}
	}
}

namespace VerseClassPrivate
{

void GenerateSubobjectName(FString& OutName, const FString& InPrefix, const FString& PropName, int32 Index)
{
	if (InPrefix.Len())
	{
		OutName = InPrefix;
		OutName += TEXT("_");
	}
	OutName += PropName;
	if (Index > 0)
	{
		OutName += FString::Printf(TEXT("_%d"), Index);
	}
}

void RenameSubobject(UObject* Subobject, const FString& InName)
{
	const ERenameFlags RenameFlags = REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional;
	UObject* ExistingSubobject = StaticFindObjectFast(UObject::StaticClass(), Subobject->GetOuter(), *InName, EFindObjectFlags::None);
	if (ExistingSubobject && ExistingSubobject != Subobject)
	{
		// ExistingSubobject is an object with the same name and outer as the subobject currently assigned to the property we're traversing
		// The engine does not allow renaming on top of existing objects so we need to rename the old object first
		ExistingSubobject->Rename(*MakeUniqueObjectName(ExistingSubobject->GetOuter(), ExistingSubobject->GetClass()).ToString(), nullptr, RenameFlags);
	}
	Subobject->Rename(*InName, nullptr, RenameFlags);
}

void TraverseSubobjectsInternal(UObject* InObject, void* ContainerPtr, UStruct* Struct, const FString& Prefix, const TFunctionRef<void(UObject* /*Subobject*/, const FString& /*CanonicalSubobjectName*/)> Operation, ETraverseSubobjectsFlag Flags = ETraverseSubobjectsFlag::None);

void TraverseSubobjectsInternal(UObject* InObject, void* ContainerPtr, FProperty* RefProperty, const FString& Prefix, const TFunctionRef<void(UObject* /*Subobject*/, const FString& /*CanonicalSubobjectName*/)> Operation, ETraverseSubobjectsFlag Flags)
{
	{
		UStruct* OwnerStruct = RefProperty ? RefProperty->GetOwner<UStruct>() : nullptr;

		// If the direct owner of RefProperty is not a UStruct then we're traversing an inner property of a property that has already passed this test (FArray/FMap/FSetProperty)
		if (OwnerStruct && !OwnerStruct->IsA<UVerseClass>() && !OwnerStruct->IsA<UVerseStruct>())
		{
			// Skip non-verse properties
			return;
		}
	}

	bool bShouldGenerateSubobjectName = !(Flags & ETraverseSubobjectsFlag::NoNameGeneration);

	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(RefProperty))
	{
		// Traverse all subobjects referenced by this property (potentially in a C-style array)
		for (int32 ObjectIndex = 0; ObjectIndex < ObjProp->ArrayDim; ++ObjectIndex)
		{
			void* Address = ObjProp->ContainerPtrToValuePtr<void>(ContainerPtr, ObjectIndex);
			UObject* Subobject = ObjProp->GetObjectPropertyValue(Address);
			if (Subobject && Subobject->GetOuter() == InObject)
			{
				FString CanonicalSubobjectName;

				if (bShouldGenerateSubobjectName)
				{
					GenerateSubobjectName(CanonicalSubobjectName, Prefix, ObjProp->GetName(), ObjectIndex);
				}

				Operation(Subobject, CanonicalSubobjectName);
			}
		}
	}
	else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(RefProperty))
	{
		// Traverse all subobjects referenced by this array property (potentially in a C-style array)
		for (int32 Index = 0; Index < ArrayProp->ArrayDim; ++Index)
		{
			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(ContainerPtr, Index));

			// When traversing from an Optional property we could be dealing with an 'unset' (or invalid)
			// array here. For this reason use the unchecked variant.
			int32 ArrayNum = ArrayHelper.NumUnchecked();

			for (int32 ElementIndex = 0; ElementIndex < ArrayNum; ++ElementIndex)
			{
				FString NewPrefix;

				if (bShouldGenerateSubobjectName)
				{
					GenerateSubobjectName(NewPrefix, Prefix, ArrayProp->GetName(), ElementIndex);
				}

				void* ElementAddress = ArrayHelper.GetRawPtr(ElementIndex);
				TraverseSubobjectsInternal(InObject, ElementAddress, ArrayProp->Inner, NewPrefix, Operation, Flags);
			}
		}
	}
	else if (FSetProperty* SetProp = CastField<FSetProperty>(RefProperty))
	{
		for (int32 Index = 0; Index < SetProp->ArrayDim; ++Index)
		{
			FScriptSetHelper SetHelper(SetProp, SetProp->ContainerPtrToValuePtr<void>(ContainerPtr, Index));

			// See comment for Array properties.
			int32 SetNum = SetHelper.NumUnchecked();

			for (int32 ElementIndex = 0, Count = SetNum; Count; ++ElementIndex)
			{
				if (SetHelper.IsValidIndex(ElementIndex))
				{
					FString NewPrefix;

					if (bShouldGenerateSubobjectName)
					{
						GenerateSubobjectName(NewPrefix, Prefix, SetProp->GetName(), ElementIndex);
					}

					void* ElementAddress = SetHelper.GetElementPtr(ElementIndex);
					TraverseSubobjectsInternal(InObject, ElementAddress, SetProp->ElementProp, NewPrefix, Operation, Flags);
					--Count;
				}
			}
		}
	}
	else if (FMapProperty* MapProp = CastField<FMapProperty>(RefProperty))
	{
		for (int32 Index = 0; Index < MapProp->ArrayDim; ++Index)
		{
			FScriptMapHelper MapHelper(MapProp, MapProp->ContainerPtrToValuePtr<void>(ContainerPtr, Index));

			// See comment for Array properties.
			int32 MapNum = MapHelper.NumUnchecked();

			for (int32 ElementIndex = 0, Count = MapNum; Count; ++ElementIndex)
			{
				if (MapHelper.IsValidIndex(ElementIndex))
				{
					FString NewPrefix;

					if (bShouldGenerateSubobjectName)
					{
						GenerateSubobjectName(NewPrefix, Prefix, MapProp->GetName(), ElementIndex);
					}

					uint8* ValuePairPtr = MapHelper.GetPairPtr(ElementIndex);

					TraverseSubobjectsInternal(InObject, ValuePairPtr, MapProp->KeyProp, NewPrefix + TEXT("_Key"), Operation, Flags);
					TraverseSubobjectsInternal(InObject, ValuePairPtr, MapProp->ValueProp, NewPrefix + TEXT("_Value"), Operation, Flags);

					--Count;
				}
			}
		}
	}
	else if (FStructProperty* StructProp = CastField<FStructProperty>(RefProperty))
	{
		for (int32 Index = 0; Index < StructProp->ArrayDim; ++Index)
		{
			FString NewPrefix;

			if (bShouldGenerateSubobjectName)
			{
				GenerateSubobjectName(NewPrefix, Prefix, StructProp->GetName(), Index);
			}

			void* StructAddress = StructProp->ContainerPtrToValuePtr<void>(ContainerPtr, Index);
			TraverseSubobjectsInternal(InObject, StructAddress, StructProp->Struct, NewPrefix, Operation, Flags);
		}
	}
	else if (FOptionalProperty* OptionProp = CastField<FOptionalProperty>(RefProperty))
	{
		FProperty* ValueProp = OptionProp->GetValueProperty();
		checkf(ValueProp->GetOffset_ForInternal() == 0, TEXT("Expected offset of value property of option property \"%s\" to be 0, got %d"), *OptionProp->GetFullName(), ValueProp->GetOffset_ForInternal());
		FString NewPrefix(Prefix);
		for (int32 Index = 0; Index < OptionProp->ArrayDim; ++Index)
		{
			// If for some reason the offset of ValueProp is not 0 then we may need to adjust how we calculate the ValueAddress
			void* ValueAddress = OptionProp->ContainerPtrToValuePtr<void>(ContainerPtr, Index);
			// Update the prefix only if this is an actual C-style array
			if (OptionProp->ArrayDim > 1)
			{
				if (bShouldGenerateSubobjectName)
				{
					GenerateSubobjectName(NewPrefix, Prefix, OptionProp->GetName(), Index);
				}
			}
			TraverseSubobjectsInternal(InObject, ValueAddress, ValueProp, NewPrefix, Operation, Flags);
		}
	}
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	else if (FVRestValueProperty* ValueProp = CastField<FVRestValueProperty>(RefProperty))
	{
		const bool bTransactional = AutoRTFM::IsClosed();
		AutoRTFM::Open([&] AUTORTFM_DISABLE {
			Verse::FRunningContext Context = Verse::FRunningContextPromise{};

			for (int32 Index = 0; Index < ValueProp->ArrayDim; ++Index)
			{
				void* Address = ValueProp->ContainerPtrToValuePtr<void>(ContainerPtr, Index);
				Verse::VRestValue RestValue = ValueProp->GetPropertyValue(Address);
				Verse::VValue Value = RestValue.Get(Context);
				if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
				{
					Value = Ref->Get(Context).Follow();
				}

				if (!TraverseValueInternal(Context, bTransactional, InObject, Value, ValueProp->GetName(), Prefix, Index, Flags, Operation))
				{
					return;
				}
			}
		});
	}
#endif
}

void TraverseSubobjectsInternal(UObject* InObject, void* ContainerPtr, UStruct* Struct, const FString& Prefix, const TFunctionRef<void(UObject* /*Subobject*/, const FString& /*CanonicalSubobjectName*/)> Operation, ETraverseSubobjectsFlag Flags)
{
	for (FProperty* RefProperty = Struct->RefLink; RefProperty; RefProperty = RefProperty->NextRef)
	{
		TraverseSubobjectsInternal(InObject, ContainerPtr, RefProperty, Prefix, Operation, Flags);
	}
}

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
bool TraverseValueInternal(
	Verse::FAllocationContext Context, bool bTransactional,
	UObject* InObject, Verse::VValue Value, const FString& PropName, const FString& Prefix, int32 Index,
	ETraverseSubobjectsFlag Flags, const TFunctionRef<void(UObject* /*Subobject*/, const FString& /*CanonicalSubobjectName*/)> Operation)
{
	bool bShouldGenerateSubobjectName = !(Flags & ETraverseSubobjectsFlag::NoNameGeneration);

	if (UObject* Subobject = Value.ExtractUObject())
	{
		if (Subobject->GetOuter() == InObject)
		{
			FString CanonicalSubobjectName;

			if (bShouldGenerateSubobjectName)
			{
				GenerateSubobjectName(CanonicalSubobjectName, Prefix, PropName, Index);
			}

			if (bTransactional)
			{
				AutoRTFM::ETransactionStatus Status = AutoRTFM::Close([&] { Operation(Subobject, CanonicalSubobjectName); });
				if (Status != AutoRTFM::ETransactionStatus::Executing)
				{
					return false;
				}
			}
			else
			{
				Operation(Subobject, CanonicalSubobjectName);
			}
		}
	}
	else if (Verse::VArrayBase* Array = Value.DynamicCast<Verse::VArrayBase>())
	{
		uint32 ArrayNum = Array->Num();
		for (uint32 ElementIndex = 0; ElementIndex < ArrayNum; ++ElementIndex)
		{
			FString NewPrefix;

			if (bShouldGenerateSubobjectName)
			{
				GenerateSubobjectName(NewPrefix, Prefix, PropName, ElementIndex);
			}

			TraverseValueInternal(Context, bTransactional, InObject, Array->GetValue(ElementIndex), PropName, NewPrefix, 0, Flags, Operation);
		}
	}
	else if (Verse::VMapBase* Map = Value.DynamicCast<Verse::VMapBase>())
	{
		int32 ElementIndex = 0;
		for (TPair<Verse::VValue, Verse::VValue> Pair : *Map)
		{
			FString NewPrefix;

			if (bShouldGenerateSubobjectName)
			{
				GenerateSubobjectName(NewPrefix, Prefix, PropName, ElementIndex);
			}

			TraverseValueInternal(Context, bTransactional, InObject, Pair.Key, PropName, NewPrefix + TEXT("_Key"), 0, Flags, Operation);
			TraverseValueInternal(Context, bTransactional, InObject, Pair.Value, PropName, NewPrefix + TEXT("_Value"), 0, Flags, Operation);

			++ElementIndex;
		}
	}
	else if (Verse::VNativeStruct* NativeStruct = Value.DynamicCast<Verse::VNativeStruct>())
	{
		FString NewPrefix;

		if (bShouldGenerateSubobjectName)
		{
			GenerateSubobjectName(NewPrefix, Prefix, PropName, Index);
		}

		void* StructAddress = NativeStruct->GetStruct();
		UScriptStruct* ScriptStruct = Verse::VNativeStruct::GetUScriptStruct(*NativeStruct->GetEmergentType());
		if (bTransactional)
		{
			AutoRTFM::ETransactionStatus Status = AutoRTFM::Close([&] { TraverseSubobjectsInternal(InObject, StructAddress, ScriptStruct, NewPrefix, Operation, Flags); });
			if (Status != AutoRTFM::ETransactionStatus::Executing)
			{
				return false;
			}
		}
		else
		{
			TraverseSubobjectsInternal(InObject, StructAddress, ScriptStruct, NewPrefix, Operation, Flags);
		}
	}
	else if (Verse::VValueObject* Struct = Value.DynamicCast<Verse::VValueObject>(); Struct && Struct->IsStruct())
	{
		FString NewPrefix;

		if (bShouldGenerateSubobjectName)
		{
			GenerateSubobjectName(NewPrefix, Prefix, PropName, Index);
		}

		Verse::VEmergentType& EmergentType = *Struct->GetEmergentType();
		for (auto It = EmergentType.Shape->CreateFieldsIterator(); It; ++It)
		{
			Verse::FOpResult LoadResult = Struct->LoadField(Context, *It->Key);
			V_DIE_UNLESS(LoadResult.IsReturn());

			// TODO: Compute the property name for It->Key and incorporate it into NewPrefix.
			TraverseValueInternal(Context, bTransactional, InObject, LoadResult.Value, PropName, NewPrefix, 0, Flags, Operation);
		}
	}
	else if (Verse::VOption* Option = Value.DynamicCast<Verse::VOption>())
	{
		FString NewPrefix;

		if (bShouldGenerateSubobjectName)
		{
			GenerateSubobjectName(NewPrefix, Prefix, PropName, Index);
		}

		TraverseValueInternal(Context, bTransactional, InObject, Option->GetValue(), PropName, NewPrefix, 0, Flags, Operation);
	}
	return true;
}
#endif

} // namespace VerseClassPrivate

void UVerseClass::RenameDefaultSubobjects(UObject* InObject)
{
	VerseClassPrivate::TraverseSubobjectsInternal(InObject, InObject, InObject->GetClass(), /*Prefix*/ FString(), [](UObject* Subobject, const FString& CanonicalSubobjectName) {
		VerseClassPrivate::RenameSubobject(Subobject, CanonicalSubobjectName);
	});
}

void UVerseClass::PrePopulateVerseFields(UObject* CDO)
{
	TSet<FProperty*> SetupDestinationProperty;
	for (FProperty* Property = RefLink; Property; Property = Property->NextRef)
	{
		FProperty* FieldProperty = nullptr;
		if (UVerseClass* VerseClass = Property->GetOwner<UVerseClass>())
		{
			if (TFieldPath<FProperty>* Value = VerseClass->VerseFields.Find(Property))
			{
				FieldProperty = Value->Get();
			}
		}
		if (!FieldProperty)
		{
			continue;
		}

		// Record any uses of @field in the class hierarchy.
		SolClassFlags |= VCLASS_HasFieldAttribute;

		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		const FOptionalProperty* OptionalProperty = nullptr;
		if (!ObjectProperty)
		{
			OptionalProperty = CastField<FOptionalProperty>(Property);
			ObjectProperty = OptionalProperty ? CastField<FObjectPropertyBase>(OptionalProperty->GetValueProperty()) : nullptr;
		}

		if (!ObjectProperty)
		{
			UE_LOGF(LogSolGeneratedClass, Error, "The verse field source property(%ls) of class(%ls) only works on object property types.", *Property->GetName(), *GetName());
			continue;
		}

		int32 Num = Property->ArrayDim;
		for (int32 StaticIndex = 0; StaticIndex < Num; ++StaticIndex)
		{
			// Retrieve the value of the property
			UObject* Object = nullptr;
			UClass* ObjectClass = nullptr;

			if (ObjectProperty)
			{
				const void* PropertyValue = Property->ContainerPtrToValuePtr<void>(CDO, StaticIndex);
				if (OptionalProperty)
				{
					PropertyValue = OptionalProperty->GetValuePointerForReadIfSet(PropertyValue);
				}
				Object = PropertyValue ? ObjectProperty->GetObjectPropertyValue(PropertyValue) : nullptr;
				ObjectClass = ObjectProperty->PropertyClass;
			}

			if (Object)
			{
				if (FArrayProperty* FieldArrayProperty = CastField<FArrayProperty>(FieldProperty))
				{
					FScriptArrayHelper ArrayHelper(FieldArrayProperty, FieldArrayProperty->ContainerPtrToValuePtr<void>(CDO));
					if (const FObjectPropertyBase* InnerObjectProperty = CastField<FObjectPropertyBase>(FieldArrayProperty->Inner))
					{
						// Check for class compatibility
						if (ObjectClass == InnerObjectProperty->PropertyClass || ObjectClass->IsChildOf(InnerObjectProperty->PropertyClass))
						{
							// Clear anything that comes from super classes and rebuild it here once
							if (!SetupDestinationProperty.Contains(FieldArrayProperty))
							{
								if (ArrayHelper.Num() > 0)
								{
									// Clear any values previous values
									ArrayHelper.EmptyValues(1);
								}

								if (!FieldArrayProperty->HasAnyPropertyFlags(CPF_ForcePostConstructLink))
								{
									UE_LOGF(LogSolGeneratedClass, Error, "The verse field destination property(%ls) of class(%ls) need to be setup with CPF_ForcePostConstructLink for it to work", *FieldProperty->GetName(), *GetName());
								}

								SetupDestinationProperty.Add(FieldArrayProperty);
							}

							// Inserting at 0 because the RefLink are in reverse order of declaration.
							ArrayHelper.InsertValues(0);
							InnerObjectProperty->SetObjectPropertyValue(ArrayHelper.GetRawPtr(0), Object);
						}
						else
						{
							UE_LOGF(LogSolGeneratedClass, Error, "The verse field source(%ls) and destination property(%ls) of class(%ls) class type are not compatible", *Property->GetName(), *FieldProperty->GetName(), *GetName());
						}
					}
					else
					{
						UE_LOGF(LogSolGeneratedClass, Error, "The verse field destination property(%ls) of class(%ls) only works on an array of objects", *FieldProperty->GetName(), *GetName());
					}
				}
				else
				{
					UE_LOGF(LogSolGeneratedClass, Error, "The verse field destination property(%ls) of class(%ls) only works on an array of objects", *FieldProperty->GetName(), *GetName());
				}
			}
		}
	}
}

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
void UVerseClass::RenameSubobjectsOfGlobalField(
	Verse::FAllocationContext Context,
	UObject* Module,
	const FString& GlobalFieldName,
	Verse::VValue Value)
{
	check(Module);
	check(!GlobalFieldName.IsEmpty());

	// since BPVM-verse (ie. the editor) addresses cross-package Verse globals using their "UE
	// name", we have to normalize `GlobalFieldName` to its "UE" representation here:
	const FString UEFieldName = Verse::Names::VersePropToUEName(GlobalFieldName);

	VerseClassPrivate::TraverseValueInternal(
		Context,
		false,
		Module,
		Value,
		UEFieldName,
		FString{},
		0,
		VerseClassPrivate::ETraverseSubobjectsFlag::None,
		[](UObject* UEObject, const FString& Name) { VerseClassPrivate::RenameSubobject(UEObject, Name); });
}
#endif

bool UVerseClass::ValidateSubobjectArchetypes(UObject* InObject, UObject* InArchetype)
{
	bool bIsValid = true;

	check(InObject);

	if (InArchetype)
	{
		VerseClassPrivate::TraverseSubobjectsInternal(InObject, InObject, InObject->GetClass(), /*Prefix*/ FString(), [InArchetype, &bIsValid](UObject* Subobject, const FString& CanonicalSubobjectName) {
			if (!CanonicalSubobjectName.Equals(Subobject->GetName()))
			{
				UObject* SubArchetypeInOwnerArchetype = static_cast<UObject*>(FindObjectWithOuter(InArchetype, Subobject->GetClass(), Subobject->GetFName()));

				if (!SubArchetypeInOwnerArchetype)
				{
					const TCHAR* CanonicalSubobjectNameCStr = CanonicalSubobjectName.GetCharArray().GetData();

					UObject* ExpectedSubArchetype = static_cast<UObject*>(FindObjectWithOuter(InArchetype, Subobject->GetClass(), CanonicalSubobjectNameCStr));

					if (ExpectedSubArchetype)
					{
						UObject* SubArchetype = Subobject->GetArchetype();
						FString SubArchetypePath = SubArchetype ? SubArchetype->GetPathName() : FString();
						FString ExpectedSubArchetypePath = ExpectedSubArchetype->GetPathName();

						UE_LOGF(LogSolGeneratedClass, Display, "Incorrectly named Verse sub-object: '%ls', expected name: '%ls' (path: '%ls', archetype path: '%ls', expected archetype path: '%ls')",
							*Subobject->GetName(), CanonicalSubobjectNameCStr, *Subobject->GetPathName(), *SubArchetypePath, *ExpectedSubArchetypePath);

						bIsValid = false;
					}
				}
			}
		});
	}

	return bIsValid;
}

int32 UVerseClass::GetVerseFunctionParameterCount(UFunction* Func)
{
	int32 ParameterCount = 0;
	if (FStructProperty* TupleProperty = CastField<FStructProperty>(Func->ChildProperties))
	{
		if (UStruct* TupleStruct = TupleProperty->Struct)
		{
			for (TFieldIterator<FProperty> It(TupleProperty->Struct); It; ++It)
			{
				if (It->GetFName() != UVerseClass::StructPaddingDummyName)
				{
					ParameterCount++;
				}
			}
		}
	}
	else
	{
		for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_OutParm))
			{
				ParameterCount++;
			}
		}
	}
	return ParameterCount;
}

bool UVerseClass::CanMemberFunctionBeCalledFromGameplaySystems(FName FuncName)
{
	bool bResult = false;
#if WITH_VERSE_BPVM
	ForEachClassInHierarchy([&bResult, &FuncName](const UVerseClass* Class) {
		if (const FName* MangledFuncName = Class->DisplayNameToUENameFunctionMap.Find(FuncName))
		{
			if (UVerseFunction* VMFunc = Cast<UVerseFunction>(Class->FindFunctionByName(*MangledFuncName)))
			{
				// Function must be public and be accessible from gameplay systems.
				// For now, only parameterless and void-returning functions can be called.
				// Later, gameplay systems may be able to specify a signature to match.
				if (EnumHasAllFlags(VMFunc->FunctionFlags, EFunctionFlags::FUNC_Public)
					&& VMFunc->IsAccessibleFromEngineGameplay()
					&& GetVerseFunctionParameterCount(VMFunc) == 0
					&& !VMFunc->GetReturnProperty())
				{
					bResult = true;
				}
				return false; // Found the function, stop searching regardless of result
			}
		}
		return true;
	});
#endif // WITH_VERSE_BPVM
	return bResult;
}

void UVerseClass::ForEachVerseFunction(TFunctionRef<bool(FVerseFunctionDescriptor)> Operation, EFieldIterationFlags IterationFlags)
{
#if WITH_VERSE_BPVM
	ForEachClassInHierarchy([&Operation](const UVerseClass* Class) {
		for (const TPair<FName, FName>& NamePair : Class->DisplayNameToUENameFunctionMap)
		{
			if (UFunction* VMFunc = Class->FindFunctionByName(NamePair.Value))
			{
				FVerseFunctionDescriptor Descriptor(VMFunc, NamePair.Key, NamePair.Value);
				if (!Operation(Descriptor))
				{
					return false;
				}
			}
		}
		return true;
	},
		IterationFlags);
#endif // WITH_VERSE_BPVM
}

#if WITH_VERSE_BPVM
FVerseFunctionDescriptor UVerseClass::FindVerseFunctionByDisplayName(UObject* Object, const FString& DisplayName, EFieldIterationFlags SearchFlags)
{
	checkf(Object, TEXT("Object instance must be provided when searching for Verse functions"));
	if (UVerseClass* Class = Cast<UVerseClass>(Object->GetClass()))
	{
		return Class->FindVerseFunctionByDisplayName(DisplayName, SearchFlags);
	}
	return FVerseFunctionDescriptor();
}

FVerseFunctionDescriptor UVerseClass::FindVerseFunctionByDisplayName(const FString& DisplayName, EFieldIterationFlags SearchFlags)
{
	FName DisplayFName(DisplayName);
	FVerseFunctionDescriptor Result = FVerseFunctionDescriptor();
	ForEachClassInHierarchy([&Result, &DisplayFName](const UVerseClass* Class) {
		if (const FName* UEName = Class->DisplayNameToUENameFunctionMap.Find(DisplayFName))
		{
			Result = FVerseFunctionDescriptor(nullptr, DisplayFName, *UEName);
			return false;
		}
		return true;
	},
		SearchFlags);
	return Result;
}
#endif // WITH_VERSE_BPVM

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
void UVerseClass::SetShape(Verse::FAllocationContext Context, Verse::VShape* InShape)
{
	V_DIE_UNLESS(Shape.CanDefQuickly());
	if (GUObjectArray.IsDisregardForGC(this))
	{
		InShape->AddRef(Context);
	}
	Shape.Set(Context, *InShape);
}

Verse::FOpResult UVerseClass::LoadField(Verse::FAllocationContext Context, UObject* Object, const Verse::VShape::VEntry* Field, Verse::VValue Self)
{
	using namespace Verse;

	switch (Field->Type)
	{
		case EFieldType::FProperty:
			return VNativeRef::Get(Context, Object, Field->UProperty);
		case EFieldType::FPropertyVar:
			V_RETURN(VNativeRef::New(Context, Object, Field->UProperty));
		case EFieldType::FVerseProperty:
		{
			VRestValue& Slot = *Field->UProperty->ContainerPtrToValuePtr<VRestValue>(Object);

			if (LIKELY(Slot.CanDefQuickly()) && GUObjectArray.IsDisregardForGC(Object))
			{
				VValue Placeholder = Slot.Get(Context);
				Placeholder.AsPlaceholder().AddRef(Context);
				V_RETURN(Placeholder);
			}

			V_RETURN(Slot.Get(Context));
		}
		case EFieldType::Constant:
		{
			VValue FieldValue = Field->Value.Get();

			// Self is passed in as a separate parameter because it may be a placeholder for a partially-constructed
			// native object.
			if (Self.IsUninitialized())
			{
				Self = Object;
			}

			// Bind methods and accessors to Self- they are stored without it to enable more shape sharing.
			// Ignore function which are already bound, which are just fields of function type.
			if (VFunction* Function = FieldValue.DynamicCast<VFunction>(); Function && !Function->HasSelf())
			{
				V_RETURN(Function->Bind(Context, Self));
			}
			else if (VAccessor* Accessor = FieldValue.DynamicCast<VAccessor>())
			{
				V_RETURN(VAccessorRef::New(Context, Self, *Accessor));
			}
			V_RETURN(FieldValue);
		}
		case EFieldType::Offset:
		default:
			VERSE_UNREACHABLE();
	}
}

void UVerseClass::VisitMembers(Verse::FAllocationContext Context, UObject* Object, Verse::FDebuggerVisitor& Visitor)
{
	using namespace Verse;
	Visitor.VisitObject([Context, Object, &Visitor] {
		VShape& Shape = UVerseClass::GetShapeForLoadField(Context, Object->GetClass());
		for (auto It = Shape.CreateFieldsIterator(); It; ++It)
		{
			Visitor.Visit(UVerseClass::LoadField(Context, Object, &It->Value), It->Key->AsStringView());
		}
	});
}

void UVerseClass::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	Super::AddReferencedObjects(InThis, Collector);
	UVerseClass* This = static_cast<UVerseClass*>(InThis);
	Collector.AddReferencedVerseValue(This->Shape);
}
#endif

namespace VerseClassPrivate
{

#if WITH_EDITOR
// Property attributes used by the editor implementation. Set here to avoid requiring a recompile on cooked class types.
static const FName MD_EditInline("EditInline");
static const FName MD_SupportsDynamicInstance("SupportsDynamicInstance");
#endif // WITH_EDITOR

// Determines if the given property can be treated as an instanced reference.
bool CanTreatAsInstancedProperty(FProperty* RefProp)
{
	// The 'self' member of a task class must be handled as a special case, since it is implicitly bound at compile time.
	static const FName ContextSelfName("_Self");
	const bool bHasTaskClassNamePrefix = RefProp->GetOwnerClass()->GetName().StartsWith(Verse::FPackageName::TaskUClassPrefix);
	if (bHasTaskClassNamePrefix
		&& RefProp->HasAnyPropertyFlags(CPF_Parm)
		&& RefProp->GetFName() == ContextSelfName)
	{
		return false;
	}

	return true;
}

// Used to recursively apply instanced class property flags to an object property when dynamic subobject instancing is disabled.
void ApplyInstancedObjectPropertyFlags(FProperty* RefProp)
{
	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(RefProp))
	{
		ApplyInstancedObjectPropertyFlags(ArrayProperty->Inner);
		if (ArrayProperty->Inner->ContainsInstancedObjectProperty())
		{
			ArrayProperty->SetPropertyFlags(CPF_ContainsInstancedReference);
		}
	}
	else if (FSetProperty* SetProperty = CastField<FSetProperty>(RefProp))
	{
		ApplyInstancedObjectPropertyFlags(SetProperty->ElementProp);
		if (SetProperty->ElementProp->ContainsInstancedObjectProperty())
		{
			SetProperty->SetPropertyFlags(CPF_ContainsInstancedReference);
		}
	}
	else if (FMapProperty* MapProperty = CastField<FMapProperty>(RefProp))
	{
		ApplyInstancedObjectPropertyFlags(MapProperty->KeyProp);
		ApplyInstancedObjectPropertyFlags(MapProperty->ValueProp);
		if (MapProperty->KeyProp->ContainsInstancedObjectProperty()
			|| MapProperty->ValueProp->ContainsInstancedObjectProperty())
		{
			MapProperty->SetPropertyFlags(CPF_ContainsInstancedReference);
		}
	}
	else if (FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(RefProp))
	{
		ApplyInstancedObjectPropertyFlags(OptionalProperty->GetValueProperty());
		if (OptionalProperty->GetValueProperty()->ContainsInstancedObjectProperty())
		{
			OptionalProperty->SetPropertyFlags(CPF_ContainsInstancedReference);
		}
	}
	else if (FStructProperty* StructProperty = CastField<FStructProperty>(RefProp))
	{
		// Note: When instanced reference semantics are used, the Verse compiler always applies this to struct properties,
		// regardless of whether or not the struct has any instanced reference fields. I am choosing to emulate that here.
		StructProperty->SetPropertyFlags(CPF_ContainsInstancedReference);
	}
	else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(RefProp))
	{
		ObjectProperty->SetPropertyFlags(CPF_PersistentInstance | CPF_ExportObject | CPF_InstancedReference);
#if WITH_EDITOR
		// This is imposed by the @editable attribute when instanced reference semantics are enabled in the absence of
		// "editinline" meta. See ProcessEditableUeProperty() / "verse.EditInlineSubobjectProperties" for more context.
		if (!ObjectProperty->HasMetaData(MD_EditInline))
		{
			ObjectProperty->SetMetaData(MD_SupportsDynamicInstance, TEXT("true"));
		}
#endif // WITH_EDITOR
	}
#if WITH_VERSE_BPVM
	else if (FVerseDynamicProperty* DynamicProperty = CastField<FVerseDynamicProperty>(RefProp))
	{
		DynamicProperty->SetPropertyFlags(CPF_InstancedReference);
	}
#endif // WITH_VERSE_BPVM
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	else if (FVRestValueProperty* RestValueProperty = CastField<FVRestValueProperty>(RefProp))
	{
		RestValueProperty->SetPropertyFlags(CPF_InstancedReference);
	}
#endif
}

// Used to recursively clear instanced class property flags from an object property when dynamic subobject instancing is enabled.
void ClearInstancedObjectPropertyFlags(FProperty* RefProp)
{
	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(RefProp))
	{
		if (ArrayProperty->Inner->ContainsInstancedObjectProperty())
		{
			ArrayProperty->ClearPropertyFlags(CPF_ContainsInstancedReference);
		}

		ClearInstancedObjectPropertyFlags(ArrayProperty->Inner);
	}
	else if (FSetProperty* SetProperty = CastField<FSetProperty>(RefProp))
	{
		if (SetProperty->ElementProp->ContainsInstancedObjectProperty())
		{
			SetProperty->ClearPropertyFlags(CPF_ContainsInstancedReference);
		}

		ClearInstancedObjectPropertyFlags(SetProperty->ElementProp);
	}
	else if (FMapProperty* MapProperty = CastField<FMapProperty>(RefProp))
	{
		if (MapProperty->KeyProp->ContainsInstancedObjectProperty()
			|| MapProperty->ValueProp->ContainsInstancedObjectProperty())
		{
			MapProperty->ClearPropertyFlags(CPF_ContainsInstancedReference);
		}

		ClearInstancedObjectPropertyFlags(MapProperty->KeyProp);
		ClearInstancedObjectPropertyFlags(MapProperty->ValueProp);
	}
	else if (FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(RefProp))
	{
		if (OptionalProperty->GetValueProperty()->ContainsInstancedObjectProperty())
		{
			OptionalProperty->ClearPropertyFlags(CPF_ContainsInstancedReference);
		}

		ClearInstancedObjectPropertyFlags(OptionalProperty->GetValueProperty());
	}
	else if (FStructProperty* StructProperty = CastField<FStructProperty>(RefProp))
	{
		// Note: When instanced reference semantics are used, the Verse compiler always applies this to struct properties,
		// regardless of whether or not the struct has any instanced reference fields. I am choosing to emulate that here.
		StructProperty->ClearPropertyFlags(CPF_ContainsInstancedReference);
	}
	else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(RefProp))
	{
		ObjectProperty->ClearPropertyFlags(CPF_PersistentInstance | CPF_ExportObject | CPF_InstancedReference);
#if WITH_EDITOR
		// This is imposed by the @editable attribute when instanced reference semantics are enabled in the absence of
		// "editinline" meta. See ProcessEditableUeProperty() / "verse.EditInlineSubobjectProperties" for more context.
		if (!ObjectProperty->HasMetaData(MD_EditInline))
		{
			ObjectProperty->RemoveMetaData(MD_SupportsDynamicInstance);
		}
#endif // WITH_EDITOR
	}
#if WITH_VERSE_BPVM
	else if (FVerseDynamicProperty* DynamicProperty = CastField<FVerseDynamicProperty>(RefProp))
	{
		DynamicProperty->ClearPropertyFlags(CPF_InstancedReference);
	}
#endif // WITH_VERSE_BPVM
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	else if (FVRestValueProperty* RestValueProperty = CastField<FVRestValueProperty>(RefProp))
	{
		RestValueProperty->ClearPropertyFlags(CPF_InstancedReference);
	}
#endif
}

} // namespace VerseClassPrivate

void UVerseClass::EnableDynamicInstancedReferenceSupport()
{
	// Nothing to do if already enabled.
	if (!HasInstancedSemantics())
	{
		return;
	}

	// Clear instanced property flags to simulate being compiled with instanced reference semantics disabled.
	for (FProperty* RefProp = RefLink; RefProp && RefProp->GetOwnerClass() == this; RefProp = RefProp->NextRef)
	{
		if (VerseClassPrivate::CanTreatAsInstancedProperty(RefProp))
		{
			VerseClassPrivate::ClearInstancedObjectPropertyFlags(RefProp);
		}
	}

	// Signal that this class no longer has instanced semantics.
	SolClassFlags &= ~VCLASS_HasInstancedSemantics;

	// This class now requires dynamic instancing.
	bNeedsDynamicSubobjectInstancing = true;
}

void UVerseClass::DisableDynamicInstancedReferenceSupport()
{
	// Nothing to do if already disabled.
	if (HasInstancedSemantics())
	{
		return;
	}

	// Apply instanced property flags to allow instancing to work without dynamic references (legacy mode).
	for (FProperty* RefProp = RefLink; RefProp && RefProp->GetOwnerClass() == this; RefProp = RefProp->NextRef)
	{
		if (VerseClassPrivate::CanTreatAsInstancedProperty(RefProp))
		{
			VerseClassPrivate::ApplyInstancedObjectPropertyFlags(RefProp);
		}
	}

	// Signal that this class now has explicitly-instanced properties.
	SolClassFlags |= VCLASS_HasInstancedSemantics;

	// This class no longer requires dynamic instancing.
	bNeedsDynamicSubobjectInstancing = false;
}

UVerseClass::FStaleClassInfo UVerseClass::ResetUHTNative()
{
	check(IsUHTNative());

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	Shape.Reset(0);
#endif

	FStaleClassInfo StaleState;
	StaleState.SourceClass = this;
	Swap(StaleState.DisplayNameToUENameFunctionMap, DisplayNameToUENameFunctionMap);
	Swap(StaleState.FunctionMangledNames, FunctionMangledNames);
	Swap(StaleState.TaskClasses, TaskClasses);
	StripVerseGeneratedFunctions(&StaleState.Children);
	return StaleState;
}

void UVerseClass::StripVerseGeneratedFunctions(TArray<TKeyValuePair<FName, TObjectPtr<UField>>>* StrippedFields)
{
	UField* Current = Children;
	Children = nullptr;
	UField::FLinkedListBuilder KeepBuilder(ToRawPtr(MutableView(Children)));
	while (Current != nullptr)
	{
		UField* NextField = Current->Next;
		Current->Next = nullptr;
		if (UVerseFunction::IsVerseGeneratedFunction(Current))
		{
			if (UFunction* AsFunction = Cast<UFunction>(Current))
			{
				RemoveFunctionFromFunctionMap(AsFunction);
				FName OriginalName = AsFunction->GetFName();
				Verse::Names::MakeTypeDead(AsFunction, AsFunction->GetOuter());
				if (StrippedFields != nullptr)
				{
					StrippedFields->Emplace(OriginalName, AsFunction);
				}
			}
		}
		else
		{
			KeepBuilder.AppendNoTerminate(*Current);
		}
		Current = NextField;
	}
	ClearFunctionMapsCaches();
}

#if WITH_VERSE_BPVM
void UVerseClass::BindVerseFunction(const char* DecoratedFunctionName, FNativeFuncPtr NativeThunkPtr)
{
	FString UEName = Verse::Names::VerseFuncToUEName(FString(DecoratedFunctionName));
	FName UEFName = FName(UEName);

	// Register this native call in the NativeFunctionLookupTable
	FNativeFunctionLookup* FuncMapping = NativeFunctionLookupTable.FindByPredicate([UEFName](const FNativeFunctionLookup& NativeFunctionLookup) {
		return UEFName == NativeFunctionLookup.Name;
	});
	if (FuncMapping == nullptr)
	{
		NativeFunctionLookupTable.Emplace(UEFName, NativeThunkPtr);
	}
	else
	{
		FuncMapping->Pointer = NativeThunkPtr;
	}
}
#endif

#if WITH_VERSE_BPVM
void UVerseClass::BindVerseCoroClass(const char* DecoratedFunctionName, FNativeFuncPtr NativeThunkPtr)
{
	FString UEName = Verse::Names::VerseFuncToUEName(FString(DecoratedFunctionName));

	const FString TaskClassName = Verse::FPackageName::GetTaskUClassName(*this, *UEName);
	UVerseClass* TaskClass = FindObject<UVerseClass>(GetOutermost(), *TaskClassName);
	if (ensureAlwaysMsgf(TaskClass, TEXT("Failed to find coroutine task class: `%s`"), *TaskClassName))
	{
		TaskClass->BindVerseFunction("Update", NativeThunkPtr);
	}
}
#endif

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
void UVerseClass::SetVerseCallableThunks(const FVerseCallableThunk* InThunks, uint32 NumThunks)
{
	VerseCallableThunks = TConstArrayView<FVerseCallableThunk>(InThunks, NumThunks);
}
#endif

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
void UVerseClass::BindVerseCallableFunctions(Verse::VPackage* VersePackage, FUtf8StringView VerseScopePath)
{
	for (const FVerseCallableThunk& Thunk : VerseCallableThunks)
	{
		Verse::VNativeProcedure::SetThunk(VersePackage, VerseScopePath, Thunk.NameUTF8, Thunk.Pointer);
	}
}
#endif

namespace Verse
{
bool IsInternalCodegenAsset(const UObject& Object)
{
	// This is for everything else that we generate in the compiler during codegen.
	if (const UVerseClass* MaybeVerseClass = Cast<UVerseClass>(&Object); MaybeVerseClass && MaybeVerseClass->SolClassFlags & EVerseClassFlags::VCLASS_InternalCodegen)
	{
		return true;
	}
	if (const UVerseStruct* MaybeVerseStruct = Cast<UVerseStruct>(&Object); MaybeVerseStruct && MaybeVerseStruct->VerseClassFlags & EVerseClassFlags::VCLASS_InternalCodegen)
	{
		return true;
	}
	return false;
}

} // namespace Verse
