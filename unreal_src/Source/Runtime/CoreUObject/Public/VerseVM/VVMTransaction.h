// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Templates/TypeHash.h"
#include "VVMCell.h"
#include "VVMContext.h"
#include "VVMLog.h"
#include "VVMPlaceholder.h"
#include "VVMPtrVariant.h"
#include "VVMVerse.h"
#include "VVMWriteBarrier.h"

#ifndef ENABLE_VVM_TRANSACTION_ID_VALIDATION
#define ENABLE_VVM_TRANSACTION_ID_VALIDATION ((UE_BUILD_SHIPPING == 0) && (UE_BUILD_TEST == 0))
#endif

namespace Verse
{
template <typename T>
struct FWriteLog
{
	FWriteLog(const FWriteLog&) = delete;

	FWriteLog(FWriteLog&&) = delete;

	FWriteLog& operator=(const FWriteLog&) = delete;

	FWriteLog& operator=(FWriteLog&&) = delete;

	~FWriteLog() = default;

	FWriteLog()
	{
		checkSlow(IsInline());
		std::memset(Table, 0, InitialCapacity * sizeof(uint64));
	}

	void Append(FAllocationContext Context, FWriteLog& Child)
	{
		for (uint32 I = 0, Last = Child.Num; I != Last; ++I)
		{
			AddImpl(Context, Child.Log[I]);
		}
	}

	void Backtrack(FAccessContext Context)
	{
		for (uint32 I = 0, Last = Num; I != Last; ++I)
		{
			Log[I].Backtrack(Context);
		}
	}

	void Empty()
	{
		if (IsInline())
		{
			EmptyInlineHashTable();
		}
		else
		{
			EmptyHashTable();
		}
	}

protected:
	void AddImpl(FAllocationContext Context, T Entry)
	{
		checkSlow(Entry.Key() != 0);
		if (IsInline())
		{
			AddToInlineHashTable(Context, Entry);
		}
		else
		{
			AddToHashTable(Context, Entry);
		}
	}

private:
	V_FORCEINLINE bool IsInline() { return Table == InlineTable; }

	V_FORCEINLINE bool ShouldGrowTable()
	{
		return 2 * Num > TableCapacity;
	}

	static uint64& FindBucket(uint64 Entry, uint64* Table, uint32 Capacity, bool& bIsNewEntry)
	{
		checkSlow(Capacity && (Capacity & (Capacity - 1)) == 0);
		// We use a simple linear probing hash table.
		uint32 Mask = Capacity - 1;
		uint32 Index = ::GetTypeHash(Entry) & Mask;
		for (;;)
		{
			if (!Table[Index] || Table[Index] == Entry)
			{
				bIsNewEntry = !Table[Index];
				return Table[Index];
			}
			Index = (Index + 1) & Mask;
		}
	}

	FORCENOINLINE void GrowTable(FAllocationContext Context)
	{
		uint32 NewCapacity = TableCapacity * 2;
		if (TableCapacity == InitialCapacity)
		{
			NewCapacity *= 2;
		}

		std::size_t AllocationSize = sizeof(uint64) * NewCapacity;
		uint64* NewTable = BitCast<uint64*>(Context.AllocateAuxCell(AllocationSize));
		std::memset(NewTable, 0, AllocationSize);

		for (uint32 I = 0, Last = Num; I != Last; ++I)
		{
			bool bIsNewEntry;
			FindBucket(Log[I].Key(), NewTable, NewCapacity, bIsNewEntry) = Log[I].Key();
		}

		TableCapacity = NewCapacity;
		Table = NewTable;
	}

	void AddToInlineHashTable(FAllocationContext Context, T Entry)
	{
		for (uint32 I = 0, Last = InitialCapacity; I != Last; ++I)
		{
			if (!Table[I])
			{
				Table[I] = Entry.Key();
				AppendToLog(Context, Entry);
				return;
			}
			if (Entry.Key() == Table[I])
			{
				return;
			}
		}

		GrowTable(Context);
		AddToHashTable(Context, Entry);
	}

	void AddToHashTable(FAllocationContext Context, T Entry)
	{
		bool bIsNewEntry;
		uint64& Bucket = FindBucket(Entry.Key(), Table, TableCapacity, bIsNewEntry);
		if (bIsNewEntry)
		{
			Bucket = Entry.Key();
			AppendToLog(Context, Entry);
			if (ShouldGrowTable())
			{
				GrowTable(Context);
			}
		}
	}

	void EmptyInlineHashTable()
	{
		std::memset(Table, 0, InitialCapacity * sizeof(uint64));
		Num = 0;
		TableCapacity = InitialCapacity;
		LogCapacity = InitialCapacity;
	}

	void EmptyHashTable()
	{
		Table = InlineTable;
		Log = BitCast<T*>(static_cast<char*>(InlineLog));
		EmptyInlineHashTable();
	}

	void AppendToLog(FAllocationContext Context, T Entry)
	{
		if (Num == LogCapacity)
		{
			uint32 NewCapacity = LogCapacity * 2;
			T* NewLog = BitCast<T*>(Context.AllocateAuxCell(NewCapacity * sizeof(T)));
			std::memcpy(NewLog, Log, Num * sizeof(T));
			LogCapacity = NewCapacity;
			Log = NewLog;
		}

		new (Log + Num) T{::MoveTemp(Entry)};
		++Num;
	}

private:
	static constexpr uint32 InitialCapacity = 4;

	uint64* Table = InlineTable;
	T* Log = BitCast<T*>(static_cast<char*>(InlineLog));

	uint64 InlineTable[InitialCapacity];
	alignas(alignof(T)) char InlineLog[InitialCapacity * sizeof(T)];

	uint32 Num = 0;
	uint32 TableCapacity = InitialCapacity;
	// TODO: It's conceivable we could make LogCapacity a function of TableCapacity.
	// But we're just doing the simple thing for now.
	uint32 LogCapacity = InitialCapacity;
};

struct FTrailLogEntry
{
	enum class EType : uint8
	{
		Mode, // VPlaceholder::Mode
		Value,
		EmergentType
	};

	explicit FTrailLogEntry(VPlaceholder::EMode& Mode)
		: Ptr{&Mode}
		, Value{static_cast<uint64>(Mode)}
		, Type{EType::Mode}
	{
	}

	explicit FTrailLogEntry(TWriteBarrier<VValue>& InValue)
		: Ptr{&InValue}
		, Value{InValue.Get().GetEncodedBits()}
		, Type{EType::Value}
	{
	}

	// InValue == Cell who's EmergentType we want to log
	explicit FTrailLogEntry(VCell* InValue)
		: Ptr{InValue}
		, Value{InValue->EmergentTypeOffset}
		, Type{EType::EmergentType}
	{
	}

	uint64 Key() const
	{
		return reinterpret_cast<uint64>(Ptr);
	}

	void Backtrack(FAccessContext Context)
	{
		if (Type == EType::Mode)
		{
			*static_cast<EMode*>(Ptr) = static_cast<EMode>(Value);
		}
		else if (Type == EType::Value)
		{
			static_cast<TWriteBarrier<VValue>*>(Ptr)->Set(Context, VValue::Decode(Value));
		}
		else if (Type == EType::EmergentType)
		{
			static_cast<VCell*>(Ptr)->SetEmergentType(Context, static_cast<uint32>(Value));
		}
		else
		{
			V_DIE("Unknown FTrailLogEntry::EType encountered!");
		}
	}

private:
	using EMode = VPlaceholder::EMode;

	void* Ptr; // VPlaceholder::Mode() is byte-aligned, leaving no bits for TPtrVariant.
	uint64 Value;
	EType Type;
};

struct FTrailLog : FWriteLog<FTrailLogEntry>
{
	void Add(FAllocationContext Context, VPlaceholder::EMode& Mode)
	{
		AddImpl(Context, FTrailLogEntry{Mode});
	}

	void Add(FAllocationContext Context, TWriteBarrier<VValue>& Value)
	{
		AddImpl(Context, FTrailLogEntry{Value});
	}

	void Add(FAllocationContext Context, VCell* Value)
	{
		AddImpl(Context, FTrailLogEntry{Value});
	}
};

struct FTrail
{
	FTrail(FTrail* Parent)
		: Parent(Parent)
	{
	}

	FTrail* GetParent() const
	{
		return Parent;
	}

	void Exit(FAllocationContext Context)
	{
		if (Parent)
		{
			Parent->Log.Append(Context, Log);
		}
		Log.Empty();
	}

	void LogBeforeWrite(FAllocationContext Context, VPlaceholder::EMode& Mode)
	{
		Log.Add(Context, Mode);
	}

	void LogBeforeWrite(FAllocationContext Context, TWriteBarrier<VValue>& Value)
	{
		Log.Add(Context, Value);
	}

	void LogEmergentTypeBeforeWrite(FAllocationContext Context, VCell* Value)
	{
		Log.Add(Context, Value);
	}

	FTrailLog Log;
	FTrail* Parent{nullptr};
};

struct FTransactionLogEntry
{
	uintptr_t Key() { return Slot.RawPtr(); }

	using FSlot = TPtrVariant<TWriteBarrier<VValue>*, TWriteBarrier<TAux<void>>*, TWriteBarrier<VCell>*>;

	FSlot Slot;      // The memory location we write OldValue into on abort.
	uint64 OldValue; // VValue or TAux<void> depending on how Slot is encoded.
	static_assert(sizeof(OldValue) == sizeof(VValue));
	static_assert(sizeof(OldValue) == sizeof(VCell*));
	static_assert(sizeof(OldValue) == sizeof(TAux<void>));

	FTransactionLogEntry(TWriteBarrier<VValue>& InSlot, VValue OldValue)
		: Slot(&InSlot)
		, OldValue(OldValue.GetEncodedBits())
	{
	}

	template <typename T, typename = std::enable_if_t<std::is_convertible_v<T*, VCell*>>>
	FTransactionLogEntry(TWriteBarrier<T>& InSlot, T* OldValue)
		: Slot(reinterpret_cast<TWriteBarrier<VCell>*>(&InSlot))
		, OldValue(BitCast<uint64>(OldValue))
	{
	}

	FTransactionLogEntry(TWriteBarrier<TAux<void>>& InSlot, TAux<void> OldValue)
		: Slot(&InSlot)
		, OldValue(BitCast<uint64>(OldValue.GetPtr()))
	{
	}

	void Backtrack(FAccessContext Context)
	{
		if (Slot.Is<TWriteBarrier<VValue>*>())
		{
			TWriteBarrier<VValue>* ValueSlot = Slot.As<TWriteBarrier<VValue>*>();
			ValueSlot->Set(Context, VValue::Decode(OldValue));
		}
		else if (Slot.Is<TWriteBarrier<TAux<void>>*>())
		{
			TWriteBarrier<TAux<void>>* AuxSlot = Slot.As<TWriteBarrier<TAux<void>>*>();
			AuxSlot->Set(Context, TAux<void>(BitCast<void*>(OldValue)));
		}
		else
		{
			TWriteBarrier<VCell>* ValueSlot = Slot.As<TWriteBarrier<VCell>*>();
			ValueSlot->Set(Context, BitCast<VCell*>(OldValue));
		}
	}
};

struct AUTORTFM_DISABLE FTransaction
{
	enum class EState
	{
		// Initial constructed state.
		Constructed,
		// The transaction has begun, and is on transaction stack.
		// Active can only transition from the Constructed state, and can
		// only transition to the Committed or Aborted state.
		Active,
		// The transaction has completed without failure.
		// Committed can only transition from the Active state, and is a
		// terminal state.
		Committed,
		// The transaction has completed with failure.
		// Aborted can only transition from the Active state, and is a terminal
		// state.
		Aborted,
	};

	// Convenience function for returning true iff State is Active.
	inline bool IsActive() const { return State == EState::Active; }

	// The outer transaction. The outer VM transaction will have a null parent.
	FTransaction* Parent{nullptr};

	// The current VM transaction state.
	EState State = EState::Constructed;

#if ENABLE_VVM_TRANSACTION_ID_VALIDATION
	// The identifier of the AutoRTFM transaction created with Start()
	AutoRTFM::TransactionID AutoRTFMTransactionID{0};
#endif

	// Begins the transaction.
	// Creates and pushes a new AutoRTFM transaction, and sets this FTransaction
	// as the current transaction with Context.
	// Must be in EState::Constructed to call.
	void Start(FRunningContext Context)
	{
		V_DIE_UNLESS(State == EState::Constructed);
		DieUnlessTransactionIDIsZero();
		AutoRTFM::StartTransaction();
		Parent = Context.CurrentTransaction();
		Context.SetCurrentTransaction(this);
		SnapshotTransactionID();
		State = EState::Active;
	}

	// Commits the transaction.
	// Sets the parent transaction (which may be null) as the current
	// transaction with Context.
	// Commits the AutoRTFM transaction which will call on-commit handlers, and
	// pop the AutoRTFM transaction created with Start() from the AutoRTFM
	// transaction stack.
	// Must be in EState::Active to call.
	void Commit(FRunningContext Context)
	{
		V_DIE_UNLESS(State == EState::Active);
		DieUnlessTransactionIDsMatch();
		AutoRTFM::CommitTransaction();
		Context.SetCurrentTransaction(Parent);
		State = EState::Committed;
	}

	// Controls the behaviour of FTransaction::Abort()
	enum class EAbortMode
	{
		// Abort() will rollback and pop the current AutoRTFM transaction.
		Default,
		// Abort() will not rollback and pop the current AutoRTFM transaction,
		// as the AutoRTFM transaction was already aborted before calling.
		AutoRTFMTransactionAlreadyAborted,
	};

	// Aborts the transaction.
	// Sets the parent transaction (which may be null) as the current
	// transaction with Context.
	// If AbortMode is Default, then aborts the AutoRTFM transaction created
	// with Start(), which will revert any memory writes, call on-abort handlers
	// and pop the AutoRTFM transaction from the AutoRTFM transaction stack.
	// Must be in EState::Active to call.
	template <EAbortMode AbortMode = EAbortMode::Default>
	void Abort(FAccessContext Context)
	{
		V_DIE_UNLESS(State == EState::Active);
		State = EState::Aborted;
		Context.SetCurrentTransaction(Parent);
		if constexpr (AbortMode == EAbortMode::Default)
		{
			DieUnlessTransactionIDsMatch();
			AutoRTFM::AbortTransaction();
		}
		else
		{
			// Identifier should not match, as the transaction should be aborted.
			DieIfTransactionIDsMatch();
		}
		ClearTransactionID();
	}

	// Stores the current AutoRTFM transaction ID to AutoRTFMTransactionID
	// No-op if !ENABLE_VVM_TRANSACTION_ID_VALIDATION
	inline void SnapshotTransactionID()
	{
#if ENABLE_VVM_TRANSACTION_ID_VALIDATION
		AutoRTFMTransactionID = AutoRTFM::CurrentTransactionID();
#endif
	}

	// Resets the current AutoRTFM transaction ID to 0
	// No-op if !ENABLE_VVM_TRANSACTION_ID_VALIDATION
	inline void ClearTransactionID()
	{
#if ENABLE_VVM_TRANSACTION_ID_VALIDATION
		AutoRTFMTransactionID = 0;
#endif
	}

	// Dies if AutoRTFMTransactionID is not 0
	// No-op if !ENABLE_VVM_TRANSACTION_ID_VALIDATION
	inline void DieUnlessTransactionIDIsZero() const
	{
#if ENABLE_VVM_TRANSACTION_ID_VALIDATION
		V_DIE_UNLESS(AutoRTFMTransactionID == 0)
#endif
	}

	// Dies if AutoRTFMTransactionID is not equal to the current AutoRTFM transaction ID
	// No-op if !ENABLE_VVM_TRANSACTION_ID_VALIDATION
	inline void DieUnlessTransactionIDsMatch() const
	{
#if ENABLE_VVM_TRANSACTION_ID_VALIDATION
		V_DIE_UNLESS(AutoRTFMTransactionID == AutoRTFM::CurrentTransactionID())
#endif
	}

	// Dies if AutoRTFMTransactionID is equal to the current AutoRTFM transaction ID
	// No-op if !ENABLE_VVM_TRANSACTION_ID_VALIDATION
	inline void DieIfTransactionIDsMatch() const
	{
#if ENABLE_VVM_TRANSACTION_ID_VALIDATION
		V_DIE_IF(AutoRTFMTransactionID == AutoRTFM::CurrentTransactionID());
#endif
	}
};

} // namespace Verse
#endif // WITH_VERSE_VM
