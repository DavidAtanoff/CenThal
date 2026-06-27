// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VVMGlobalTrivialEmergentTypePtr.h"
#include "VVMNativeProcedure.h"
#include "VVMType.h"

namespace Verse
{

struct VProgram;
struct VTask;

// A special heap value to store all intrinsic VFunction objects
struct AUTORTFM_DISABLE VIntrinsics : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	TWriteBarrier<VFunction> Abs;
	TWriteBarrier<VFunction> Ceil;
	TWriteBarrier<VFunction> Floor;
	TWriteBarrier<VFunction> ConcatenateMaps;
	TWriteBarrier<VFunction> WeakMapType;
	TWriteBarrier<VFunction> FitsInPlayerMap;
	TWriteBarrier<VFunction> MakePersistentMap;
	TWriteBarrier<VFunction> MakeSessionVar;
	TWriteBarrier<VFunction> NotifyPersistentMapMutation;

	static VIntrinsics& New(FAllocationContext Context, VPackage& BuiltInPackage)
	{
		return *new (Context.AllocateFastCell(sizeof(VIntrinsics))) VIntrinsics(Context, BuiltInPackage);
	}

	static void Initialize(FAllocationContext Context);

private:
	COREUOBJECT_API static FNativeCallResult AbsImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FNativeCallResult CeilImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FNativeCallResult FloorImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FNativeCallResult ConcatenateMapsImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FNativeCallResult WeakMapTypeImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FNativeCallResult FitsInPlayerMapImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FNativeCallResult MakePersistentMapImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FNativeCallResult MakeSessionVarImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);
	COREUOBJECT_API static FNativeCallResult NotifyPersistentMapMutationImpl(FRunningContext Context, VValue Self, VNativeProcedure::Args Arguments);

	VIntrinsics(FAllocationContext Context, VPackage& BuiltInPackage);
};

} // namespace Verse
#endif // WITH_VERSE_VM
