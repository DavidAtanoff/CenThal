// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMTask.h"
#include "VerseVM/VVMType.h"

namespace Verse
{
enum class EValueStringFormat;
struct FRefAwaiter;

struct FRefAwaiterHeader
{
	FRefAwaiterHeader() = default;

	FSetElementId Prev;
	FSetElementId Next;

	// Non-reentrant
	template <typename UnaryFunction>
	void ForEach(const TSet<FRefAwaiter>&, UnaryFunction) const;

	// Reentrant
	template <typename UnaryFunction>
	bool AnyOf(const TSet<FRefAwaiter>&, UnaryFunction) const;
};

struct FRefAwaiter : FRefAwaiterHeader
{
	inline FRefAwaiter(FAccessContext Context, VTask& Task, const FOp& AwaitPC);

	TWriteBarrier<VTask> Task;
	const FOp* AwaitPC;

	friend bool operator==(const FRefAwaiter&, const FRefAwaiter&);

	friend uint32 GetTypeHash(const FRefAwaiter&);

	void Attach(TSet<FRefAwaiter>& Set, FRefAwaiterHeader& Header, FSetElementId ThisElementId)
	{
		Prev = Header.Prev;
		Get(Set, Header, Prev).Next = ThisElementId;
		Next = {};
		Get(Set, Header, Next).Prev = ThisElementId;
	}

	void Detach(TSet<FRefAwaiter>& Set, FRefAwaiterHeader& Header)
	{
		Get(Set, Header, Prev).Next = Next;
		Get(Set, Header, Next).Prev = Prev;
	}

private:
	static FRefAwaiterHeader& Get(TSet<FRefAwaiter>& Set, FRefAwaiterHeader& Header, FSetElementId ElementId)
	{
		if (ElementId.IsValidId())
		{
			return Set[ElementId];
		}
		return Header;
	}
};

struct VRefRareData : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> EmergentType;

	AUTORTFM_DISABLE static VRefRareData& New(FAllocationContext Context, VValue Domain)
	{
		return *new (Context.Allocate(FHeap::DestructorSpace, sizeof(VRefRareData))) VRefRareData{Context, Domain};
	}

	VValue GetDomain() const
	{
		return Domain.Get();
	}

	VTask* GetLiveTask() const;

	void SetLiveTask(FAllocationContext, VTask*);

	void AddAwaitTask(FAccessContext, VTask&);

	template <typename UnaryFunction>
	void ForEachAwaitTask(UnaryFunction) const;

	template <typename UnaryFunction>
	bool AnyAwaitTask(UnaryFunction) const;

	size_t GetAllocatedSize() const
	{
		return AwaiterBuffer.GetAllocatedSize();
	}

private:
	AUTORTFM_DISABLE explicit VRefRareData(FAllocationContext Context, VValue Domain)
		: VCell{Context, &EmergentType.Get(Context)}
		, Domain{Context, Domain}
	{
		FHeap::ReportAllocatedNativeBytes(GetAllocatedSize());
	}

	VRefRareData(const VRefRareData&) = delete;

	VRefRareData(VRefRareData&&) = delete;

	VRefRareData& operator=(const VRefRareData&) = delete;

	VRefRareData& operator=(VRefRareData&&) = delete;

	~VRefRareData()
	{
		FHeap::ReportDeallocatedNativeBytes(GetAllocatedSize());
	}

	static bool ContainsAwaitTask(const VTask&, const FOp&);

	TWriteBarrier<VValue> Domain;
	TWriteBarrier<VTask> LiveTask;
	TSet<FRefAwaiter> AwaiterBuffer;
	FRefAwaiterHeader AwaiterHeader;
};

struct VRef : VHeapValue
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VHeapValue);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	AUTORTFM_DISABLE static VRef& New(FAllocationContext Context, VValue Domain = {})
	{
		return *new (Context.AllocateFastCell(sizeof(VRef))) VRef(Context, Domain);
	}

	VValue Get(FAllocationContext Context)
	{
		return Value.Get(Context);
	}

	void Set(FAllocationContext Context, VValue NewValue);

	void SetNonTransactionally(FAccessContext Context, VValue NewValue)
	{
		return Value.Set(Context, NewValue);
	}

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);
	AUTORTFM_DISABLE TSharedPtr<FJsonValue> ToJSONImpl(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs);
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VRef*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);
	static constexpr bool InstancedCell = true;

	VValue GetDomain() const
	{
		return RareData ? RareData->GetDomain() : VValue{};
	}

	VTask* GetLiveTask() const;

	AUTORTFM_DISABLE void SetLiveTask(FAllocationContext, VTask*);

	AUTORTFM_DISABLE void AddAwaitTask(FAllocationContext, VTask&);

	template <typename UnaryFunction>
	void ForEachAwaitTask(UnaryFunction F) const
	{
		if (RareData)
		{
			RareData->ForEachAwaitTask(F);
		}
	}

	template <typename UnaryFunction>
	bool AnyAwaitTask(UnaryFunction F) const
	{
		return RareData && RareData->AnyAwaitTask(F);
	}

	AUTORTFM_DISABLE void VisitMembersImpl(FAllocationContext, FDebuggerVisitor&);

private:
	VRestValue Value;
	TWriteBarrier<VRefRareData> RareData;

	explicit VRef(FAllocationContext Context, VValue Domain)
		: VHeapValue{Context, &GlobalTrivialEmergentType.Get(Context)}
		// TODO: Figure out what split depth meets here.
		, Value{0}
		, RareData{Context, Domain ? &VRefRareData::New(Context, Domain) : nullptr}
	{
	}
};

// There are different designs going on in regards to VNativeRef and VTransparentRef in the VM currently, ie:
//
// VTransparentRef --> We call `UnwrapTransparentRef` in several OpCodes (see LoadField as example) to unwrap these
// and return the VCell it points to which allows us to directly mutate it which is our current impl of VCell 'deep mutability'.
//
// VNativeRef --> The `UnwrapTransparentRef` design does not work here, as we have no 'value' that can be directly passed around, and instead
// we create a VNativeRef when loading out of a native object via VObject::LoadField/UVerseClasss::LoadField and rely on `Freeze` OpCodes
// to unwrap them at the correct point.
//
// Long term, when we implement 'correct' deep mutability we will need to change from directly passing VCell's around to passing
// Ref's around which should let us align these two approaches. But for now they are separate and there may be holes where the VNativeRef
// returned by `LoadField` is not handled by a freeze before being passed to another OpCode and will most likely crash.
template <typename SetType>
VValue UnwrapTransparentRef(FAllocationContext Context, VValue Value, VTask* Task, SetType Set)
{
	AutoRTFM::UnreachableIfClosed("#jira SOL-8415");

	if (VRef* TransparentRef = Value.ExtractTransparentRef())
	{
		if (Task && Task->AwaitPC)
		{
			TransparentRef->AddAwaitTask(Context, *Task);
		}
		return TransparentRef->Get(Context);
	}
	if (Task && Task->AwaitPC)
	{
		VRef& NewRef = VRef::New(Context, {});
		NewRef.SetNonTransactionally(Context, Value);
		NewRef.AddAwaitTask(Context, *Task);
		Set(VValue::TransparentRef(NewRef));
	}
	return Value;
}

struct VBatchedRefs : VHeapValue
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VHeapValue);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	static VBatchedRefs& New(FAllocationContext Context)
	{
		return *new (Context.AllocateFastCell(sizeof(VBatchedRefs))) VBatchedRefs(Context);
	}

	void Add(FAllocationContext Context, VRef& BatchedRef)
	{
		V_DIE_IF(BatchNestingDepth == 0);
		BatchedRefs->AddValueTransactionally(Context, BatchedRef);
	}

	void BeginBatch()
	{
		AutoRTFM::Assign(BatchNestingDepth, BatchNestingDepth + 1);
	}

	void EndBatch()
	{
		V_DIE_IF(BatchNestingDepth == 0);
		AutoRTFM::Assign(BatchNestingDepth, BatchNestingDepth - 1);
	}

	bool Batched()
	{
		return BatchNestingDepth != 0;
	}

	template <typename UnaryFunction>
	void ForEach(UnaryFunction F) const
	{
		for (VValue RefValue : *BatchedRefs)
		{
			F(RefValue.StaticCast<VRef>());
		}
	}

	template <typename UnaryFunction>
	bool AnyOf(UnaryFunction F) const
	{
		for (VValue RefValue : *BatchedRefs)
		{
			if (F(RefValue.StaticCast<VRef>()))
			{
				return true;
			}
		}
		return false;
	}

	void Reset(FAllocationContext Context)
	{
		BatchedRefs->ResetTransactionally(Context);
	}

private:
	explicit VBatchedRefs(FAllocationContext Context)
		: VHeapValue{Context, &GlobalTrivialEmergentType.Get(Context)}
		, BatchedRefs{Context, VMutableArray::New(Context)}
	{
	}

	TWriteBarrier<VMutableArray> BatchedRefs;
	uint64 BatchNestingDepth{0};
};
} // namespace Verse
#endif // WITH_VERSE_VM
