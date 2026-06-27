// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM/Defines.h"
#include "Containers/Map.h"
#include "CoreTypes.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMHeap.h"
#include "VerseVM/VVMPersistence.h"
#include "VerseVM/VVMValue.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{
struct FOp;
struct VTask;

/*
 * The following types can be used as keys:
 * logic
 * int
 * float
 * char
 * string
 * enum
 * A class, if it’s comparable
 * An option, if the element type is comparable
 * An array, if the element type is comparable
 * A map if both the key and the value types are comparable
 * A tuple if all elements in the tuple are comparable
 */
struct VMapBase : VHeapValue
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VHeapValue);
	using KeyType = TWriteBarrier<VValue>;
	using ValType = TWriteBarrier<VValue>;
	using PairType = TPair<KeyType, ValType>;
	using SequenceType = uint32;

	struct RangedForIterator
	{
		RangedForIterator(const VMapBase* Map, uint32 Index)
			: Map(Map)
			, Index(Index) {}
		TPair<VValue, VValue> operator*() const
		{
			const PairType& Pair = Map->GetPairTable()[Map->GetSequenceTable()[Index]];
			return {Pair.Key.Get().Follow(), Pair.Value.Get().Follow()};
		}
		bool operator==(const RangedForIterator& Rhs) const { return Index == Rhs.Index; }
		bool operator!=(const RangedForIterator& Rhs) const { return Index != Rhs.Index; }
		RangedForIterator& operator++()
		{
			++Index;
			return *this;
		}

		const VMapBase* Map;
		uint32 Index;
	};

protected:
	VMapBase(FAllocationContext Context, uint32 InitialCapacity, VEmergentType* Type);
	template <typename GetEntryByIndex>
	VMapBase(FAllocationContext Context, uint32 MaxNumEntries, const GetEntryByIndex& GetEntry, VEmergentType* Type);

	// This should only be called if you already have the required mutexes or know you don't need them.
	// Returns the slot in the data table where the value was inserted and a boolean indicating if an existing entry was replaced.
	AUTORTFM_DISABLE COREUOBJECT_API TPair<uint32, bool> AddWithoutLocking(FAllocationContext Context, uint32 KeyHash, VValue Key, VValue Value, bool bTransactional = false);

public:
	uint32 Num() const
	{
		return NumElements;
	}

	AUTORTFM_DISABLE COREUOBJECT_API VValue FindByHashWithSlot(FAllocationContext Context, uint32 Hash, VValue Key, uint32* OutSlot);
	AUTORTFM_DISABLE VValue FindWithSlot(FAllocationContext Context, VValue Key, SequenceType* OutSlot)
	{
		uint32 Hash = GetTypeHash(Key);
		return FindByHashWithSlot(Context, Hash, Key, OutSlot);
	}
	AUTORTFM_DISABLE VValue FindByHash(FAllocationContext Context, uint32 Hash, VValue Key)
	{
		SequenceType Slot;
		return FindByHashWithSlot(Context, Hash, Key, &Slot);
	}
	AUTORTFM_DISABLE VValue Find(FAllocationContext Context, VValue Key)
	{
		uint32 Hash = GetTypeHash(Key);
		return FindByHash(Context, Hash, Key);
	}

	// GetKey/GetValue doesn't verify that Index is within limits and
	// only works as long as nothing is removed from the map.
	VValue GetKey(uint32 Index)
	{
		check(Index < Capacity);
		PairType* PairTable = GetPairTable();
		SequenceType* SequenceTable = GetSequenceTable();
		return PairTable[SequenceTable[Index]].Key.Follow();
	}

	VValue GetValue(uint32 Index)
	{
		check(Index < Capacity);
		PairType* PairTable = GetPairTable();
		SequenceType* SequenceTable = GetSequenceTable();
		return PairTable[SequenceTable[Index]].Value.Follow();
	}

	AUTORTFM_DISABLE void Add(FAllocationContext Context, VValue Key, VValue Value);

	void AddTransactionally(FAllocationContext Context, VValue Key, VValue Value);

	AUTORTFM_DISABLE COREUOBJECT_API void Reserve(FAllocationContext Context, uint32 InCapacity);

	size_t GetPairTableSizeForCapacity(uint32 InCapacity) const
	{
		return sizeof(PairType) * InCapacity;
	}
	size_t GetSequenceTableSizeForCapacity(uint32 InCapacity) const
	{
		return sizeof(SequenceType) * InCapacity;
	}
	size_t GetPairTableSize() const
	{
		return GetPairTableSizeForCapacity(Capacity);
	}
	size_t GetSequenceTableSize() const
	{
		return GetSequenceTableSizeForCapacity(Capacity);
	}
	size_t GetAllocatedSize() const
	{
		return GetPairTableSize() + GetSequenceTableSize();
	}
	const PairType* GetPairTable() const
	{
		return Data.Get().GetPtr();
	}
	const SequenceType* GetSequenceTable() const
	{
		return SequenceData.Get().GetPtr();
	}
	PairType* GetPairTable()
	{
		return Data.Get().GetPtr();
	}
	SequenceType* GetSequenceTable()
	{
		return SequenceData.Get().GetPtr();
	}

	// These `new` calls are templated so as to avoid boilerplate News/Ctors in VMapBase's subclasses.
	template <typename MapType>
	AUTORTFM_DISABLE static MapType& New(FAllocationContext Context, uint32 InitialCapacity = 0)
	{
		std::byte* Cell = Context.AllocateFastCell(sizeof(MapType));
		VEmergentType& EmergentType = MapType::GlobalTrivialEmergentType.Get(Context);
		return *new (Cell) MapType{Context, InitialCapacity, &EmergentType};
	}

	template <typename MapType, typename GetEntryByIndex>
	static MapType& New(FAllocationContext Context, uint32 MaxNumEntries, const GetEntryByIndex& GetEntry);

	template <typename FunctionType>
	void ForEachImpl(FunctionType);

	template <typename FunctionType>
	void ForEach(FunctionType);

	template <typename MapType, typename FunctionType>
	AUTORTFM_DISABLE MapType& TraverseImpl(FAllocationContext, FunctionType);

	template <typename MapType, typename FunctionType>
	AUTORTFM_DISABLE MapType& Traverse(FAllocationContext, FunctionType);

	AUTORTFM_DISABLE COREUOBJECT_API VValue MeltImpl(FAllocationContext Context);

	AUTORTFM_DISABLE COREUOBJECT_API ECompares EqualImpl(FAllocationContext Context, VCell* Other, const TFunction<void(::Verse::VValue, ::Verse::VValue)>& HandlePlaceholder);

	COREUOBJECT_API uint32 GetTypeHashImpl();

	void VisitMembersImpl(FAllocationContext Context, FDebuggerVisitor& Visitor);

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);
	AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSONImpl(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs);

	template <typename MapType>
	AUTORTFM_DISABLE static void SerializeLayoutImpl(FAllocationContext Context, MapType*& This, FStructuredArchiveVisitor& Visitor);

	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

	static constexpr bool InstancedCell = true;

	RangedForIterator begin() const
	{
		RangedForIterator it(this, 0);
		return it;
	}

	RangedForIterator end() const
	{
		RangedForIterator it(this, NumElements);
		return it;
	}

	TWriteBarrier<TAux<PairType>> Data;
	TWriteBarrier<TAux<SequenceType>> SequenceData; // initial insert sequence only.  Overwritten values will stay in their original sequence
	uint32 NumElements;
	uint32 Capacity;
};

struct VMap : VMapBase
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VMapBase);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	using VMapBase::VMapBase;

	static constexpr bool SerializeIdentity = false;
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VMap*& This, FStructuredArchiveVisitor& Visitor) { SerializeLayoutImpl<VMap>(Context, This, Visitor); }
};

struct VMutableMap : VMapBase
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VMapBase);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	using VMapBase::VMapBase;

	AUTORTFM_DISABLE FOpResult FreezeImpl(FAllocationContext Context, VTask* Task);

	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VMutableMap*& This, FStructuredArchiveVisitor& Visitor) { SerializeLayoutImpl<VMutableMap>(Context, This, Visitor); }
};

struct VPersistentMap : VMutableMap
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VMutableMap);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	AUTORTFM_DISABLE static VPersistentMap& Register(FAllocationContext Context, FString Path, FString RelativePath, EPersistenceExternalAccess, UObject* Outer, VValue ValueType);

	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VPersistentMap*& This, FStructuredArchiveVisitor& Visitor);

	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

	VType& GetValueType() const { return ValueType.Get().StaticCast<VType>(); }

	void NotifyKeyUpdate(FAllocationContext Context, VValue Key, VValue Value);

	FString Path;
	FString RelativePath;
	EPersistenceExternalAccess ExternalAccess;
	TWriteBarrier<VValue> ValueType;
	TMap<VValue, VValue> UpdatedPairs;

private:
	AUTORTFM_DISABLE VPersistentMap(FAllocationContext Context, FString Path, FString RelativePath, EPersistenceExternalAccess ExternalAccess, VValue ValueType)
		: VMutableMap(Context, 0, &GlobalTrivialEmergentType.Get(Context))
		, Path(::MoveTemp(Path))
		, RelativePath(::MoveTemp(RelativePath))
		, ExternalAccess(ExternalAccess)
		, ValueType(Context, ValueType)
	{
	}

	AUTORTFM_DISABLE VPersistentMap(FAllocationContext Context)
		: VMutableMap(Context, 0, &GlobalTrivialEmergentType.Get(Context))
	{
	}

	void RegisterWithSaveService(FName MountPointName);
};

} // namespace Verse
#endif // WITH_VERSE_VM
