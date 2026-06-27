// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Containers/AnsiString.h"
#include "Misc/NotNull.h"
#include "UObject/Class.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"
#include "UObject/ScriptMacros.h"
#include "VerseVM/VVMPersistence.h"
#include "VerseVM/VVMVerseClassFlags.h"
#include "VerseVM/VVMVerseEffectSet.h"
#include "VerseVM/VVMVersePropertyMetadata.h"
#include "VerseVM/VVMVerseStruct.h"
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMShape.h"
#endif

#include "VVMVerseClass.generated.h"

class UClassCookedMetaData;
struct FVniTypeDesc;

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
namespace Verse
{
struct VPackage;
} // namespace Verse
#endif

USTRUCT()
struct FVersePersistentVar
{
	GENERATED_BODY()

	FVersePersistentVar(FString Path, FString RelativePath, Verse::EPersistenceExternalAccess ExternalAccess, TFieldPath<FMapProperty> Property)
		: Path(::MoveTemp(Path))
		, RelativePath(::MoveTemp(RelativePath))
		, ExternalAccess(ExternalAccess)
		, Property(::MoveTemp(Property))
	{
	}

	FVersePersistentVar() = default;

	UPROPERTY()
	FString Path;
	UPROPERTY()
	FString RelativePath;
	UPROPERTY()
	Verse::EPersistenceExternalAccess ExternalAccess{Verse::EPersistenceExternalAccess::Internal};
	UPROPERTY()
	TFieldPath<FMapProperty> Property;
};

USTRUCT()
struct FVerseSessionVar
{
	GENERATED_BODY()

	explicit FVerseSessionVar(TFieldPath<FMapProperty> Property)
		: Property(::MoveTemp(Property))
	{
	}

	FVerseSessionVar() = default;

	UPROPERTY()
	TFieldPath<FMapProperty> Property;
};

USTRUCT()
struct FVerseClassVarAccessor
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UFunction> Func{};

	UPROPERTY()
	bool bIsInstanceMember{false};

	UPROPERTY()
	bool bIsFallible{false};
};

USTRUCT()
struct FVerseClassVarAccessors
{
	GENERATED_BODY()

	UPROPERTY()
	TMap<int, FVerseClassVarAccessor> Getters;

	UPROPERTY()
	TMap<int, FVerseClassVarAccessor> Setters;
};

struct FVerseFunctionDescriptor
{
	UFunction* Function = nullptr; // May be nullptr even when valid
	FName DisplayName = NAME_None;
	FName UEName = NAME_None;

	FVerseFunctionDescriptor() = default;

	FVerseFunctionDescriptor(
		UFunction* InFunction,
		FName InDisplayName,
		FName InUEName)
		: Function(InFunction)
		, DisplayName(InDisplayName)
		, UEName(InUEName)
	{
	}
};

// This class is deliberately simple (i.e. POD) to keep generated code size down.
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
struct FVerseCallableThunk
{
	const char* NameUTF8;
	Verse::VNativeProcedure::FThunkFn Pointer;
};
#endif

UCLASS(MinimalAPI, within = Package, Config = Engine)
class UVerseClass : public UClass
{
	GENERATED_BODY()

public:
	UVerseClass() = default;
	explicit UVerseClass(
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
		FUObjectCppClassStaticFunctions&& InCppClassStaticFunctions);
	explicit UVerseClass(const FObjectInitializer& ObjectInitializer);

#if UE_WITH_CONSTINIT_UOBJECT
	consteval UVerseClass(
		UE::CodeGen::ConstInit::FObjectParams ObjectParams,
		UE::CodeGen::ConstInit::FUFieldParams UFieldParams,
		UE::CodeGen::ConstInit::FStructParams StructParams,
		UE::CodeGen::ConstInit::FClassParams ClassParams,
		UE::CodeGen::ConstInit::FVerseClassParams InVerseParams)
		: Super(ObjectParams, UFieldParams, StructParams, ClassParams)
		, CompiledInMangledPackageVersePath(InVerseParams.MangledPackageVersePath)
		, SolClassFlags(VCLASS_UHTNative | InVerseParams.VerseClassFlags)
		, TaskClasses(ConstEval)
		, PersistentVars(ConstEval)
		, SessionVars(ConstEval)
		, VarAccessors(ConstEval)
		, CompiledInPackageRelativeVersePath(InVerseParams.PackageRelativeVersePath)
		, PackageRelativeVersePath(ConstEval)
		, DisplayNameToUENameFunctionMap(ConstEval)
		, DirectInterfaces(ConstEval)
		, PropertiesWrittenByInitCDO(ConstEval)
		, FunctionMangledNames(ConstEval)
		, PredictsFunctionNames(ConstEval)
		, PredictsVarNames(ConstEval)
		, PredictsCoercedFunctions(ConstEval)
		, PropertyMetadata(ConstEval)
#if WITH_VERSE_COMPILER && WITH_EDITORONLY_DATA
		, PreviousPathName(ConstEval)
#endif // WITH_VERSE_COMPILER && WITH_EDITORONLY_DATA
#if WITH_VERSE_VM
		, VerseCallableThunks(InVerseParams.VerseCallableThunks)
#endif
		, NativeTypeDesc(InVerseParams.NativeTypeDesc)
	{
	}
#endif

public:
	//~ Begin UObjectBaseUtility interface
	COREUOBJECT_API virtual UE::Core::FVersePath GetVersePath() const override;
	//~ End UObjectBaseUtility interface

#if WITH_EDITORONLY_DATA
	UE_INTERNAL COREUOBJECT_API void TrackDefaultInitializedProperties(void* DefaultData) const;

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	AUTORTFM_DISABLE COREUOBJECT_API static void TrackDefaultInitializedPropertiesOfGlobalField(
		Verse::FAllocationContext Context,
		UObject* Module,
		Verse::VValue Value);
#endif
#endif

private:
	//~ Begin UObject interface
	virtual bool IsAsset() const override;
	COREUOBJECT_API virtual void GetPreloadDependencies(TArray<UObject*>& OutDeps) override;
	COREUOBJECT_API virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
	COREUOBJECT_API virtual void PreSave(FObjectPreSaveContext ObjectSaveContext) override;
#if WITH_EDITOR
	COREUOBJECT_API virtual void GetAssetRegistryTagMetadata(TMap<FName, FAssetRegistryTagMetadata>& OutMetadata) const override;
#endif
	//~ End UObject interface

	//~ Begin UStruct interface
	AUTORTFM_DISABLE COREUOBJECT_API virtual void Link(FArchive& Ar, bool bRelinkExistingProperties) override;
	COREUOBJECT_API virtual void PreloadChildren(FArchive& Ar) override;
	COREUOBJECT_API virtual FString GetAuthoredNameForField(const FField* Field) const override;
	UE_INTERNAL COREUOBJECT_API virtual EStructPropertyLinkFlags GetPropertyLinkFlags(UStruct* ContainerStruct, FProperty* Property) const override;
	//~ End UStruct interface

	//~ Begin UClass interface
	COREUOBJECT_API virtual void PostInitInstance(UObject* InObj, FObjectInstancingGraph* InstanceGraph) override;
	COREUOBJECT_API virtual void PostLoadInstance(UObject* InObj) override;
	COREUOBJECT_API virtual void SetupObjectInitializer(FObjectInitializer& ObjectInitializer) const override;
	virtual bool CanCreateAssetOfClass() const override
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	COREUOBJECT_API virtual bool CanCreateInstanceDataObject() const override;
	COREUOBJECT_API virtual void SerializeDefaultObject(UObject* Object, FStructuredArchive::FSlot Slot) override;
#endif
#if WITH_EDITOR
	COREUOBJECT_API virtual FTopLevelAssetPath GetReinstancedClassPathName_Impl() const;
#endif
	//~ End UClass interface

	// UField interface.
	COREUOBJECT_API virtual const TCHAR* GetPrefixCPP() const override;
	// End of UField interface.

public:
#if UE_WITH_CONSTINIT_UOBJECT
	// CONSTINIT_UOBJECT_TODO: Size increase
	const UTF8CHAR* CompiledInMangledPackageVersePath = nullptr;
#endif // UE_WITH_CONSTINIT_UOBJECT

	UPROPERTY()
	uint32 SolClassFlags = VCLASS_None;

	// All coroutine task classes belonging to this class (one for each coroutine in this class)
	UPROPERTY()
	TArray<TObjectPtr<UVerseClass>> TaskClasses;

	/** Initialization function */
	UPROPERTY()
	TObjectPtr<UFunction> InitInstanceFunction;

	UPROPERTY()
	TArray<FVersePersistentVar> PersistentVars;

	UPROPERTY()
	TArray<FVerseSessionVar> SessionVars;

	UPROPERTY()
	TMap<FName, FVerseClassVarAccessors> VarAccessors;

	UPROPERTY()
	EVerseEffectSet ConstructorEffects = EVerseEffectSet::None;

	UPROPERTY()
	FName MangledPackageVersePath; // Storing as FName since it's shared between classes

#if UE_WITH_CONSTINIT_UOBJECT
	// CONSTINIT_UOBJECT_TODO: Size increase
	const UTF8CHAR* CompiledInPackageRelativeVersePath = nullptr;
#endif // UE_WITH_CONSTINIT_UOBJECT

	UPROPERTY()
	FString PackageRelativeVersePath;

	//~ This map is technically wrong since the FName is caseless...
	UPROPERTY()
	TMap<FName, FName> DisplayNameToUENameFunctionMap;

	// All interface class types that this class implements
	UPROPERTY()
	TArray<TObjectPtr<UVerseClass>> DirectInterfaces;

	UPROPERTY()
	TArray<TFieldPath<FProperty>> PropertiesWrittenByInitCDO;

	// Store a mapping from all previous function mangled names used by the
	// code generator to the current version of name mangling.  Store
	// NAME_None if there are multiple possible current versions for any
	// previous version.  If a previous function mangled name matches the
	// current mangled name, nothing is stored.
	UPROPERTY()
	TMap<FName, FName> FunctionMangledNames;

	UPROPERTY()
	TArray<FName> PredictsFunctionNames;

	UPROPERTY()
	TMap<FAnsiString, FName> PredictsVarNames;

	UPROPERTY()
	TMap<FName, FName> PredictsCoercedFunctions;

	UPROPERTY()
	TMap<TFieldPath<FProperty>, FVersePropertyMetadata> PropertyMetadata;

	UPROPERTY()
	TMap<TFieldPath<FProperty>, TFieldPath<FProperty>> VerseFields;

#if WITH_VERSE_COMPILER && WITH_EDITORONLY_DATA
	/** Path name this class had before it was marked as DEAD */
	FString PreviousPathName;
#endif // WITH_VERSE_COMPILER && WITH_EDITORONLY_DATA

	COREUOBJECT_API static const FName NativeParentClassTagName;
	COREUOBJECT_API static const FName PackageVersePathTagName;
	COREUOBJECT_API static const FName PackageRelativeVersePathTagName;

	// Name of the CDO init function
	COREUOBJECT_API static const FName InitCDOFunctionName;
	COREUOBJECT_API static const FName StructPaddingDummyName;

	// This is the asset path that all `UVerseClass` get when generated (we use it to identify assets as Verse classes)
	COREUOBJECT_API static const FTopLevelAssetPath VerseClassTopLevelAssetPath;

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	UPROPERTY()
	Verse::TWriteBarrier<Verse::VClass> Class;

	// The VShape representing this class's layout. This is a placeholder before linking.
	Verse::VRestValue Shape{0};

	void SetShape(Verse::FAllocationContext Context, Verse::VShape* InShape);

	static Verse::VClass* GetVerseClass(Verse::FAllocationContext Context, UClass* Class);

	// Locate a shape for Class, which may be a UVerseClass or some other kind of imported UClass.
	static Verse::VShape* GetShape(Verse::FAllocationContext Context, UClass* Class);
	// Locate a shape for Class or a superclass. Useful when loading from prefabs that were not reflected to Verse.
	static Verse::VShape& GetShapeForLoadField(Verse::FAllocationContext Context, UClass* Class);

	static Verse::FOpResult LoadField(Verse::FAllocationContext Context, UObject* Object, Verse::VUniqueString& FieldName, Verse::VValue Self = Verse::VValue());
	AUTORTFM_DISABLE COREUOBJECT_API static Verse::FOpResult LoadField(Verse::FAllocationContext Context, UObject* Object, const Verse::VShape::VEntry* Field, Verse::VValue Self = Verse::VValue());

	AUTORTFM_DISABLE COREUOBJECT_API static void VisitMembers(Verse::FAllocationContext, UObject* Object, Verse::FDebuggerVisitor& Visitor);

	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
#endif

	/**
	 * Renames default sub-objects on a CDO so that they're unique (named after properties they are assigned to)
	 * @param  InObject Object (usually a CDO) whose default sub-objects are to be renamed
	 */
	COREUOBJECT_API static void RenameDefaultSubobjects(UObject* InObject);

	/**
	 * Pre-populates verse fields on a CDO by reading properties tagged with the @field attribute
	 * and inserting their values into the corresponding array properties on the CDO.
	 * @param  CDO   The class default object to populate
	 */
	COREUOBJECT_API void PrePopulateVerseFields(UObject* CDO);

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	// used to rename "subobjects" of module-scope globals so that the serialization system across
	// the server/cooker/editor refers to these objects using consistent names. (this is the
	// module-scope vvm analogue of RenameDefaultSubobjects.)
	AUTORTFM_DISABLE COREUOBJECT_API static void RenameSubobjectsOfGlobalField(
		Verse::FAllocationContext Context,
		UObject* Module,
		const FString& GlobalFieldName,
		Verse::VValue Value);
#endif

	/**
	 * Checks that the sub-objects of a given Verse object are using the correct sub-archetype.
	 * @param  InObject Object whose default sub-objects we are validating
	 * @param  InArchetype The archetype of InObject
	 */
	COREUOBJECT_API static bool ValidateSubobjectArchetypes(UObject* InObject, UObject* InArchetype);

	void SetNeedsSubobjectInstancingForLoadedInstances(bool bNeedsInstancing)
	{
		bNeedsPostLoadSubobjectInstancing = RefLink && bNeedsInstancing;
	}

	// Allows dynamic instanced reference support to be toggled on/off for this class.
	COREUOBJECT_API void EnableDynamicInstancedReferenceSupport();
	COREUOBJECT_API void DisableDynamicInstancedReferenceSupport();

	bool IsNativeBound() const { return (SolClassFlags & VCLASS_NativeBound) != VCLASS_None; }
	bool IsUniversallyAccessible() const { return (SolClassFlags & VCLASS_UniversallyAccessible) != VCLASS_None; }
	bool IsConcrete() const { return (SolClassFlags & VCLASS_Concrete) != VCLASS_None; }
	bool IsVerseModule() const { return (SolClassFlags & VCLASS_Module) != VCLASS_None; }
	bool IsUHTNative() const { return (SolClassFlags & VCLASS_UHTNative) != VCLASS_None; }
	bool IsEpicInternal() const { return (SolClassFlags & VCLASS_EpicInternal) != VCLASS_None; }
	bool HasInstancedSemantics() const { return (SolClassFlags & VCLASS_HasInstancedSemantics) != VCLASS_None; }
	bool HasTransientSubobjects() const { return (SolClassFlags & VCLASS_HasTransientSubobjects) != VCLASS_None; }
	bool IsFinalSuper() const { return (SolClassFlags & VCLASS_FinalSuper) != VCLASS_None; }
	bool IsExplicitlyCastable() const { return (SolClassFlags & VCLASS_Castable) != VCLASS_None; }
	bool IsConstructorEpicInternal() const { return (SolClassFlags & VCLASS_EpicInternalConstructor) != VCLASS_None; }
	bool IsPersistable() const { return (SolClassFlags & VCLASS_Persistable) != VCLASS_None; }
	bool IsParametric() const { return (SolClassFlags & VCLASS_Parametric) != VCLASS_None; }
	bool IsPersonaConstructible() const { return (SolClassFlags & VCLASS_PersonaConstructible) != VCLASS_None; }
	bool IsErrIncomplete() const { return (SolClassFlags & VCLASS_Err_Incomplete) != VCLASS_None; }

	void SetNativeBound() { SolClassFlags |= VCLASS_NativeBound; }
	void SetHasTransientSubobjects() { SolClassFlags |= VCLASS_HasTransientSubobjects; }

	/**
	 * Walks the full UVerseClass hierarchy: this class, its super classes, and all DirectInterfaces (recursively).
	 * @param Func Callback invoked for each UVerseClass in the hierarchy. Return true to continue, false to stop.
	 * @param Flags Controls which axes to walk. Defaults to IncludeAll (supers + interfaces).
	 * @return true if the full walk completed, false if the callback stopped it early.
	 */
	template <typename TFunc>
	bool ForEachClassInHierarchy(TFunc&& Func, EFieldIterationFlags Flags = EFieldIterationFlags::IncludeAll) const
	{
		if (!Func(this))
		{
			return false;
		}

		if (EnumHasAnyFlags(Flags, EFieldIterationFlags::IncludeSuper))
		{
			if (const UVerseClass* Super = Cast<UVerseClass>(GetSuperClass()))
			{
				if (!Super->ForEachClassInHierarchy(Func, Flags))
				{
					return false;
				}
			}
		}

		if (EnumHasAnyFlags(Flags, EFieldIterationFlags::IncludeInterfaces))
		{
			for (const TObjectPtr<UVerseClass>& Interface : DirectInterfaces)
			{
				if (!Interface->ForEachClassInHierarchy(Func, Flags))
				{
					return false;
				}
			}
		}

		return true;
	}

	const FName* FindPredictsVarPropertyName(const FAnsiString& VarName) const
	{
		const FName* Result = nullptr;
		ForEachClassInHierarchy([&Result, &VarName](const UVerseClass* Class) {
			Result = Class->PredictsVarNames.Find(VarName);
			return Result == nullptr;
		});
		return Result;
	}

	const FVerseClassVarAccessors* FindAccessors(FName VarName) const
	{
		const FVerseClassVarAccessors* Result = nullptr;
		ForEachClassInHierarchy([&Result, &VarName](const UVerseClass* Class) {
			Result = Class->VarAccessors.Find(VarName);
			return Result == nullptr;
		});
		return Result;
	}

	bool CanMemberFunctionBeCalledFromPredicts(FName FuncName)
	{
		bool bFound = false;
		ForEachClassInHierarchy([&bFound, &FuncName](const UVerseClass* Class) {
			bFound = Class->PredictsFunctionNames.Contains(FuncName);
			return !bFound;
		});
		return bFound;
	}

	COREUOBJECT_API bool CanMemberFunctionBeCalledFromGameplaySystems(FName FuncName);

	/**
	 * Iterates over Verse Function Properties on the current Verse class and executes a callback with VerseFunction value and its Verse name.
	 * @param Operation callback for each of the found Verse Functions. When the callback returns false, iteration is stopped.
	 * @param IterationFlags Additional options used when iterating over Verse Function properties
	 */
	COREUOBJECT_API void ForEachVerseFunction(TFunctionRef<bool(FVerseFunctionDescriptor)> Operation, EFieldIterationFlags IterationFlags = EFieldIterationFlags::None);

	FName GetFunctionMangledName(FName MangledName)
	{
		if (FName* NewMangledName = FindFunctionMangledName(MangledName))
		{
			return *NewMangledName;
		}
		return MangledName;
	}

	FName* FindFunctionMangledName(FName MangledName)
	{
		if (FName* NewMangledName = FindClassFunctionMangledName(MangledName))
		{
			return NewMangledName;
		}
		if (FName* NewMangledName = FindInterfaceFunctionMangledName(MangledName))
		{
			return NewMangledName;
		}
		return nullptr;
	}

	FName* FindInterfaceFunctionMangledName(FName MangledName)
	{
		for (const FImplementedInterface& Interface : Interfaces)
		{
			if (UVerseClass* SuperVerseClass = Cast<UVerseClass>(Interface.Class))
			{
				if (FName* NewMangledName = SuperVerseClass->FunctionMangledNames.Find(MangledName))
				{
					// @note there may not be two interface methods where one does not override
					// the other that share the same old mangled name, as the function name is
					// based on the base overridden definition.
					return NewMangledName;
				}
			}
		}
		return nullptr;
	}

	FName* FindClassFunctionMangledName(FName MangledName)
	{
		if (FName* NewMangledName = FunctionMangledNames.Find(MangledName))
		{
			return NewMangledName;
		}
		if (UVerseClass* SuperVerseClass = Cast<UVerseClass>(GetSuperClass()))
		{
			return SuperVerseClass->FindClassFunctionMangledName(MangledName);
		}
		return nullptr;
	}

	void AddFunctionMangledNames(FName OldMangledName, FName NewMangledName)
	{
		if (OldMangledName != NewMangledName)
		{
			if (FName* OtherNewMangledName = FindFunctionMangledName(OldMangledName))
			{
				if (*OtherNewMangledName != NewMangledName)
				{
					FunctionMangledNames.Add(OldMangledName, NAME_None);
				}
			}
			else
			{
				FunctionMangledNames.Add(OldMangledName, NewMangledName);
			}
		}
	}

	/**
	 * Returns a VerseFunction value given its display name
	 * @param Object Object instance to iterate Verse Functions for
	 * @param VerseName Display name of the function
	 * @param SearchFlags Additional options used when iterating over Verse Function properties
	 * @return VerseFunction value acquired from the provided Object instance or invalid function value if none was found.
	 */
#if WITH_VERSE_BPVM
	COREUOBJECT_API FVerseFunctionDescriptor FindVerseFunctionByDisplayName(UObject* Object, const FString& DisplayName, EFieldIterationFlags SearchFlags = EFieldIterationFlags::None);
	COREUOBJECT_API FVerseFunctionDescriptor FindVerseFunctionByDisplayName(const FString& DisplayName, EFieldIterationFlags SearchFlags = EFieldIterationFlags::None);
#endif // WITH_VERSE_BPVM

	/**
	 * Returns the number of parameters a verse function takes
	 */
	COREUOBJECT_API static int32 GetVerseFunctionParameterCount(UFunction* Func);

	struct FStaleClassInfo
	{
		TObjectPtr<UVerseClass> SourceClass;
		TMap<FName, FName> DisplayNameToUENameFunctionMap;
		TMap<FName, FName> FunctionMangledNames;
		TArray<TObjectPtr<UVerseClass>> TaskClasses;
		TArray<TKeyValuePair<FName, TObjectPtr<UField>>> Children;
	};

	// Reset the contents of the UHT class and return the reset information so it can be restored if the compiled failed.
	// Being able to restore will probably not be needed once BPVM is removed
	COREUOBJECT_API FStaleClassInfo ResetUHTNative();

	// Strip verse generated functions from the function list and place into the output container for later restoring
	COREUOBJECT_API void StripVerseGeneratedFunctions(TArray<TKeyValuePair<FName, TObjectPtr<UField>>>* StrippedFields);

#if WITH_VERSE_BPVM
	COREUOBJECT_API void BindVerseFunction(const char* DecoratedFunctionName, FNativeFuncPtr NativeThunkPtr);
	COREUOBJECT_API void BindVerseCoroClass(const char* DecoratedFunctionName, FNativeFuncPtr NativeThunkPtr);
#endif

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	// Set a table of thunks for native functions callable from Verse. Parameter should be a static array as it will not be copied.
	COREUOBJECT_API void SetVerseCallableThunks(const FVerseCallableThunk* InThunks, uint32 NumThunks);
	COREUOBJECT_API void BindVerseCallableFunctions(Verse::VPackage* VersePackage, FUtf8StringView VerseScopePath);
#endif

	const FVniTypeDesc* GetNativeTypeDesc()
	{
		return NativeTypeDesc;
	}
	void SetNativeTypeDesc(const FVniTypeDesc* InNativeTypeDesc) { NativeTypeDesc = InNativeTypeDesc; }

private:
	COREUOBJECT_API void CallInitInstanceFunctions(UObject* InObj, FObjectInstancingGraph* InstanceGraph);
	COREUOBJECT_API void CallPropertyInitInstanceFunctions(UObject* InObj, FObjectInstancingGraph* InstanceGraph);

#if WITH_VERSE_BPVM
	COREUOBJECT_API void AddPersistentVars(UObject*);
#endif

	COREUOBJECT_API void AddSessionVars(UObject*);

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UClassCookedMetaData> CachedCookedMetaDataPtr;
#endif // WITH_EDITORONLY_DATA

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	TConstArrayView<FVerseCallableThunk> VerseCallableThunks;
#endif

	const FVniTypeDesc* NativeTypeDesc{nullptr};
};

namespace Verse
{
COREUOBJECT_API bool IsInternalCodegenAsset(const UObject& Object);
} // namespace Verse
