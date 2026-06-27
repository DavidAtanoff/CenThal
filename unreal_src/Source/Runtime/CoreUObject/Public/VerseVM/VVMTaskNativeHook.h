// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM/Defines.h"
#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{

struct VTask;
struct FAccessContext;
struct VEmergentType;

struct AUTORTFM_DISABLE VTaskNativeHook : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	// If desired, this can point to the hook added after this one
	TWriteBarrier<VTaskNativeHook> Next;

	// The lambda can be mutable, therefore invocation is not const
	void Invoke(FAllocationContext Context, VTask* Task)
	{
		Invoker(Context, this, Task);
	}
	void InvokeTransactionally(FAllocationContext Context, VTask* Task)
	{
		TransactionalInvoker(Context, this, Task);
	}

	// WARNING 1: The destructor of the lambda passed in must be thread safe since it can be invoked on the GC thread
	// WARNING 2: Any VCells or UObjects captured by the lambda will not be visited during GC
	template <typename FunctorType, typename FunctorTypeDecayed = std::decay_t<FunctorType>>
		requires(!TIsTFunction<FunctorTypeDecayed>::Value && std::is_invocable_r_v<void, FunctorTypeDecayed, FAllocationContext, VTask*>)
	static VTaskNativeHook& New(FAllocationContext Context, AUTORTFM_IMPLICIT_ENABLE FunctorType&& Func);

	template <typename FunctorType, typename FunctorTypeDecayed = std::decay_t<FunctorType>>
		requires(!TIsTFunction<FunctorTypeDecayed>::Value && std::is_invocable_r_v<void, FunctorTypeDecayed, FAllocationContext, VTask*>)
	static VTaskNativeHook& NewOpen(FAllocationContext Context, AUTORTFM_IMPLICIT_DISABLE FunctorType&& Func);

protected:
	using InvokerType = void (*)(FAllocationContext, VTaskNativeHook*, VTask*);
	using DestructorType = void (*)(VTaskNativeHook*);

	VTaskNativeHook(FAllocationContext Context)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
		, Invoker([](FAllocationContext, VTaskNativeHook*, VTask*) { VERSE_UNREACHABLE(); })
		, TransactionalInvoker([](FAllocationContext, VTaskNativeHook*, VTask*) { VERSE_UNREACHABLE(); })
		, Destructor([](VTaskNativeHook*) {})
	{
	}

	~VTaskNativeHook()
	{
		Destructor(this);
	}

	InvokerType Invoker;
	InvokerType TransactionalInvoker;
	DestructorType Destructor;
};

template <typename FunctorTypeDecayed, bool bTransactional>
struct TTaskNativeHook : VTaskNativeHook
{
	template <typename FunctorType>
	AUTORTFM_DISABLE TTaskNativeHook(FAllocationContext Context, FunctorType&& InFunc)
		: VTaskNativeHook(Context)
		, Func(EInPlace::InPlace, Forward<FunctorType>(InFunc))
	{
		if constexpr (bTransactional)
		{
			AutoRTFM::RecordOpenWrite(&Invoker);
			AutoRTFM::RecordOpenWrite(&TransactionalInvoker);
			AutoRTFM::RecordOpenWrite(&Destructor);
		}
		Invoker = InvokerImpl;
		TransactionalInvoker = TransactionalInvokerImpl;
		Destructor = DestructorImpl;
	}

	static void InvokerImpl(FAllocationContext Context, VTaskNativeHook* InThis, VTask* Task)
	{
		TTaskNativeHook* This = static_cast<TTaskNativeHook*>(InThis);
		This->Func.GetValue()(Context, Task);
		This->Func.Reset();
	}

	AUTORTFM_DISABLE static void TransactionalInvokerImpl(FAllocationContext Context, VTaskNativeHook* This, VTask* Task)
	{
		AutoRTFM::ETransactionStatus Status = AutoRTFM::Close([&] { InvokerImpl(Context, This, Task); });
		V_DIE_UNLESS(Status == AutoRTFM::ETransactionStatus::Executing);
	}

	static void DestructorImpl(VTaskNativeHook* This)
	{
		// Destroy only what's not already contained in the base class
		static_cast<TTaskNativeHook*>(This)->Func.~TOptional();
	}

	template <typename FunctorType>
	AUTORTFM_DISABLE static TTaskNativeHook& New(FAllocationContext Context, FunctorType&& InFunc)
	{
		std::byte* Allocation = std::is_trivially_destructible_v<FunctorTypeDecayed>
								  ? Context.AllocateFastCell(sizeof(TTaskNativeHook))
								  : Context.Allocate(FHeap::DestructorSpace, sizeof(TTaskNativeHook));

		return *new (Allocation) TTaskNativeHook(Context, Forward<FunctorType>(InFunc));
	}

	TOptional<FunctorTypeDecayed> Func;
};

template <typename FunctorType, typename FunctorTypeDecayed>
	requires(!TIsTFunction<FunctorTypeDecayed>::Value && std::is_invocable_r_v<void, FunctorTypeDecayed, FAllocationContext, VTask*>)
VTaskNativeHook& VTaskNativeHook::New(FAllocationContext Context, FunctorType&& Func)
{
#if UE_AUTORTFM_ENABLED
	static constexpr autortfm_mode FunctorTypeAutoRTFMMode = AutoRTFM::CallMode<FunctorType, FAllocationContext, VTask*>;
	static_assert(!AutoRTFM::IsDisabled(FunctorTypeAutoRTFMMode), "FunctorType must not be AutoRTFM-disabled");
#endif

	return TTaskNativeHook<FunctorTypeDecayed, /*bTransactional*/ true>::New(Context, Forward<FunctorType>(Func));
}

template <typename FunctorType, typename FunctorTypeDecayed>
	requires(!TIsTFunction<FunctorTypeDecayed>::Value && std::is_invocable_r_v<void, FunctorTypeDecayed, FAllocationContext, VTask*>)
VTaskNativeHook& VTaskNativeHook::NewOpen(FAllocationContext Context, FunctorType&& Func)
{
	struct FOpenFunctor
	{
		FunctorTypeDecayed Func;
		AUTORTFM_OPEN void operator()(FAllocationContext Context, VTask* Task)
		{
			Func(Context, Task);
		}
	};
	return TTaskNativeHook<FOpenFunctor, /*bTransactional*/ false>::New(Context, FOpenFunctor(Forward<FunctorType>(Func)));
}

} // namespace Verse

#endif
