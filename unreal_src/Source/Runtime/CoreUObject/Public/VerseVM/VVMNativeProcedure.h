// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Containers/ArrayView.h"
#include "Containers/StringFwd.h"
#include "VVMFalse.h"
#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VVMScope.h"
#include "VVMType.h"
#include "VerseVM/VVMNames.h"

namespace Verse
{
struct FOpResult;
struct VPackage;
struct VTask;
struct VUniqueString;

using FNativeCallResult = FOpResult;

// A function that is implemented in C++
struct VNativeProcedure : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	static constexpr char DecoratorString[] = "Native";

	// Interface between VerseVM and C++
	using Args = TArrayView<VValue>;
	using FThunkFn = FNativeCallResult (*)(FRunningContext, VValue Self, Args Arguments);

	uint32 NumPositionalParameters;

	// The C++ function to call
	FThunkFn Thunk;

	TWriteBarrier<VUniqueString> Name;

	AUTORTFM_DISABLE static VNativeProcedure& New(FAllocationContext Context, uint32 NumPositionalParameters, FThunkFn Thunk, VUniqueString& InName)
	{
		return *new (Context.AllocateFastCell(sizeof(VNativeProcedure))) VNativeProcedure(Context, NumPositionalParameters, Thunk, &InName);
	}

	// Lookup a native function and set it's thunk to a C++ function
	static COREUOBJECT_API void SetThunk(Verse::VPackage* Package, FUtf8StringView VerseScopePath, FUtf8StringView DecoratedName, FThunkFn NativeFuncPtr);

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VNativeProcedure*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

private:
	AUTORTFM_DISABLE VNativeProcedure(FAllocationContext Context, uint32 InNumPositionalParameters, FThunkFn InThunk, VUniqueString* InName)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
		, NumPositionalParameters(InNumPositionalParameters)
		, Thunk(InThunk)
		, Name(Context, InName)
	{
	}
};

} // namespace Verse
#endif // WITH_VERSE_VM
