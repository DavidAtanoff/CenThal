// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM/Defines.h"
#include "VVMClass.h"
#include "VVMFalse.h"
#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VVMScope.h"
#include "VVMVerse.h"

namespace Verse
{
enum class EValueStringFormat;
struct VUniqueString;

struct VFunction : VHeapValue
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VHeapValue);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	using Args = TArray<VValue, TInlineAllocator<8>>;

	/// Either a VProcedure or a VNativeProcedure.
	TWriteBarrier<VValue> Procedure;

	/// If specified, the object instance that this function belongs to. Can either be a `VObject` or a `UObject`.
	/// When not bound, this should be an uninitialized `VValue` for methods and `VFalse` for functions. This is
	/// so we can differentiate between when we should bind `Self` lazily at runtime for calls to methods.
	TWriteBarrier<VValue> Self;

	/// The lexical scope that this function is allocated with. This includes all lexical captures, including `(super:)`.
	TWriteBarrier<VScope> ParentScope;

	AUTORTFM_DISABLE V_FORCEINLINE FOpResult Invoke(FRunningContext Context, VValue Argument, TWriteBarrier<VUniqueString>* NamedArg = nullptr)
	{
		return InvokeWithSelf(Context, Self.Get(), Argument, NamedArg);
	}
	AUTORTFM_DISABLE V_FORCEINLINE FOpResult Invoke(FRunningContext Context, Args&& Arguments, TArrayView<TWriteBarrier<VUniqueString>> NamedArgs = {}, TArrayView<VValue> NamedArgVals = {})
	{
		return InvokeWithSelf(Context, Self.Get(), MoveTemp(Arguments), NamedArgs, NamedArgVals);
	}
	AUTORTFM_DISABLE COREUOBJECT_API FOpResult InvokeWithSelf(FRunningContext Context, VValue Self, VValue Argument, TWriteBarrier<VUniqueString>* NamedArg = nullptr);
	AUTORTFM_DISABLE COREUOBJECT_API FOpResult InvokeWithSelf(FRunningContext Context, VValue Self, Args&& Arguments, TArrayView<TWriteBarrier<VUniqueString>> NamedArgs = {}, TArrayView<VValue> NamedArgVals = {}, bool bRequireConcreteEffectToken = true);
	AUTORTFM_DISABLE COREUOBJECT_API FOpResult Spawn(FRunningContext Context, Args&& Arguments, TArrayView<TWriteBarrier<VUniqueString>> NamedArgs = {}, TArrayView<VValue> NamedArgVals = {});

	AUTORTFM_DISABLE static VFunction& New(FAllocationContext Context, VValue Procedure, VValue Self, VScope* ParentScope = nullptr)
	{
		return *new (Context.AllocateFastCell(sizeof(VFunction))) VFunction(Context, Procedure, Self, ParentScope);
	}

	/// Use this when you're constructing a function where you passing captures to it for its lexical scope - i.e. a
	/// method that binds `Self` lazily at runtime, but captures `(super:)` in its current scope.
	AUTORTFM_DISABLE static VFunction& NewUnbound(FAllocationContext Context, VValue Procedure, VScope& InScope)
	{
		return *new (Context.AllocateFastCell(sizeof(VFunction))) VFunction(Context, Procedure, VValue(), &InScope);
	}

	AUTORTFM_DISABLE VFunction& Bind(FAllocationContext Context, VValue InSelf)
	{
		checkf(!HasSelf(), TEXT("Attempting to bind `Self` to a `VFunction` that already has it set; this is probably a mistake in the code generation."));
		checkf(ParentScope, TEXT("The function should already have had its scope set; this is probably a mistake in the code generation."));
		return *new (Context.AllocateFastCell(sizeof(VFunction))) VFunction(Context, Procedure.Get(), InSelf, ParentScope.Get());
	}

	/// Checks if the function is already bound.
	COREUOBJECT_API bool HasSelf() const;

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);

	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VFunction*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

	static constexpr bool InstancedCell = true;

private:
	AUTORTFM_DISABLE VFunction(FAllocationContext Context, VValue InProcedure, VValue InSelf, VScope* InParentScope)
		: VHeapValue(Context, &GlobalTrivialEmergentType.Get(Context))
		, Procedure(Context, InProcedure)
		, Self(Context, InSelf)
		, ParentScope(Context, InParentScope)
	{
	}
};
} // namespace Verse
#endif // WITH_VERSE_VM
