// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM.h"
#include "HAL/Platform.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"
#include "VVMAux.h"
#include "VVMContext.h"
#include "VVMContextImpl.h"
#include "VVMValue.h"
#include "VVMVerse.h"
#include <type_traits>

namespace Verse
{

struct FTrail;

// Enumerator used to control whether a TWriteBarrier write is transactional
// (reverts to its original value on transaction abort) or not.
enum class EWriteMode
{
	// Transactional write if called from the closed, otherwise non-transactional.
	Default,
	// Non-transactional write regardless of AutoRTFM mode.
	NonTransactional,
	// Transactional write regardless of AutoRTFM mode.
	Transactional,
};

// Returns true if WriteMode is EWriteMode::Transactional, or WriteMode is EWriteMode::Default
// and is called from the closed.
template <EWriteMode WriteMode>
inline bool IsTransactional()
{
	return WriteMode == EWriteMode::Transactional || (WriteMode == EWriteMode::Default && AutoRTFM::IsClosed());
}

// This class is a smart pointer that runs a GC write barrier whenever the stored pointer is changed
// Fundamental law of concurrent GC: when thoust mutated the heap thou shalt run the barrier template
// When a VValue of heap pointer type (VCell*) is written, a barrier is run (int32s and floats don't run a barrier)
// The barrier will inform the GC about a new edge in the heap, and GC will immediately mark the cell if a collection is ongoing
// This is necessary because GC might have already visited the previous content of Value, and might miss the updated value in that case
// No barrier is needed when _deleting_ heap references, therefore we don't care about the previous Value during mutation
// It does not matter if the barrier is run before or after mutation since we unconditionally mark only the new value
// We also run the barrier during TWriteBarrier construction since a white (= otherwise unreachable) cell might have been assigned
// New cells are allocated black (marked, reachable) so we are not worried about those here
template <typename T>
struct TWriteBarrier
{
	static constexpr bool bCanSetTransactionally = std::is_base_of_v<VValue, T> || std::is_base_of_v<TAux<void>, T> || std::is_base_of_v<VCell, T>;
	static constexpr bool bIsVValue = std::is_same_v<T, VValue> || std::is_same_v<T, VInt>;
	static constexpr bool bIsAux = IsTAux<T>;
	static constexpr bool bValueIsTPtr = !bIsVValue && !bIsAux; // TValue is T*, not T
	using TValue = typename std::conditional_t<bValueIsTPtr, T*, T>;
	using TConstValue = typename std::conditional_t<bValueIsTPtr, const T*, T>;
	using TEncodedValue = typename std::conditional<bIsVValue, uint64, T*>::type;

	V_FORCEINLINE TWriteBarrier() = default;

	/// Making use of this constructor is potentially expensive because it could involve a TLS lookup.
	V_FORCEINLINE TWriteBarrier(const TWriteBarrier& Other)
	{
		Set(FAccessContextPromise(), Other.Value);
	}

	V_FORCEINLINE TWriteBarrier& operator=(const TWriteBarrier& Other)
	{
		Set(FAccessContextPromise(), Other.Value);
		return *this;
	}

	bool Equals(const TWriteBarrier& Other) const
	{
		return Value == Other.Value;
	}

	// Needed to allow for `TWriteBarrier` to be usable with `TMap`.
	bool operator==(const TWriteBarrier& Other) const
	{
		return Equals(Other);
	}

	// Allows `TMap::RemoveByHash` to match a `VCell*` with a `TWriteBarrier<VCell>`, or a
	// `VValue` with a `TWriteBarrier<VValue>`, which lets us remove elements from a container
	// without constructing an extraneous write barrier.
	bool operator==(TConstValue Other) const
	{
		return Value == Other;
	}

	V_FORCEINLINE TWriteBarrier(FAccessContext Context, TValue Value)
	{
		Set(Context, Value);
	}

	V_FORCEINLINE TWriteBarrier(FAccessContext Context, T& Value)
		requires bValueIsTPtr
	{
		Set(Context, &Value);
	}

	AUTORTFM_OPEN void ResetTransactionally()
		requires bCanSetTransactionally;

	template <EWriteMode WriteMode = EWriteMode::Default>
	V_FORCEINLINE void Reset()
	{
		static_assert(bCanSetTransactionally || WriteMode != EWriteMode::Transactional);
		if (IsTransactional<WriteMode>())
		{
			V_DIE_UNLESS(bCanSetTransactionally);
			if constexpr (bCanSetTransactionally)
			{
				ResetTransactionally();
			}
		}
		else
		{
			UE_AUTORTFM_OPEN
			{
				Value = {};
			};
		}
	}

	AUTORTFM_OPEN void SetTransactionally(FAccessContext Context, TValue NewValue)
		requires bCanSetTransactionally;

	template <EWriteMode WriteMode = EWriteMode::Default>
	V_FORCEINLINE void Set(FAccessContext Context, TValue NewValue)
	{
		static_assert(bCanSetTransactionally || WriteMode != EWriteMode::Transactional);
		if (IsTransactional<WriteMode>())
		{
			V_DIE_UNLESS(bCanSetTransactionally);
			if constexpr (bCanSetTransactionally)
			{
				SetTransactionally(Context, NewValue);
			}
		}
		else
		{
			UE_AUTORTFM_OPEN
			{
				RunBarrier(Context, NewValue);
				Value = NewValue;
			};
		}
	}

	V_FORCEINLINE void SetTransactionally(FAccessContext Context, T& NewValue)
		requires bValueIsTPtr
	{
		SetTransactionally(Context, &NewValue);
	}

	V_FORCEINLINE void Set(FAccessContext Context, T& NewValue)
		requires bValueIsTPtr
	{
		Set(Context, &NewValue);
	}

	void SetTrailed(FAllocationContext, FTrail&, TValue);

	V_FORCEINLINE constexpr void SetNonCellNorPlaceholder(VValue NewValue)
		requires bIsVValue
	{
		checkSlow(!NewValue.IsCell());
		checkSlow(!NewValue.IsPlaceholder());
		UE_IF_CONSTEVAL
		{
			Value = NewValue;
		}
		else
		{
			if (AutoRTFM::IsClosed())
			{
				SetNonCellNorPlaceholderTransactionally(NewValue);
			}
			else
			{
				Value = NewValue;
			}
		}
	}

	AUTORTFM_OPEN void SetNonCellNorPlaceholderTransactionally(VValue)
		requires bIsVValue;

	void SetNonCellNorPlaceholderTrailed(FAllocationContext, FTrail&, VValue)
		requires bIsVValue;

	TValue Get() const { return Value; }
	template <typename TResult = TValue>
	std::enable_if_t<bIsVValue, TResult> Follow() const { return Get().Follow(); }

	// nb: operators "*" and "->" disabled for TWriteBarrier<VValue>;
	//     use Get() + VValue member functions to check/access boxed values

	T* operator->() const
		requires bValueIsTPtr
	{
		return Value;
	}

	template <typename TResult = T>
	T& operator*() const
		requires bValueIsTPtr
	{
		return *Value;
	}

	explicit operator bool() const { return !!Value; }

	friend uint32 GetTypeHash(const TWriteBarrier<T>& WriteBarrier)
	{
		using ::GetTypeHash;
		if constexpr (bIsVValue)
		{
			return GetTypeHash(WriteBarrier.Get());
		}
		else if (WriteBarrier)
		{
			return GetTypeHash(*WriteBarrier.Get());
		}
		else
		{
			return 0;
		}
	}

private:
	TValue Value{};

	template <typename ContextType>
	AUTORTFM_OPEN FORCENOINLINE static void RunBarrierSlow(ContextType Context, TValue Value)
	{
		if constexpr (bIsAux)
		{
			if (Value.GetPtr() && !FHeap::IsMarked(Value.GetPtr()))
			{
				FAccessContext(Context).RunAuxWriteBarrierNonNullDuringMarking(Value.GetPtr());
			}
		}
		else if constexpr (bIsVValue)
		{
			if (VCell* Cell = Value.ExtractCell())
			{
				if (!FHeap::IsMarked(Cell))
				{
					// Delay construction of the context (which does the expensive TLS lookup), until we actually need the mark stack to do marking.
					FAccessContext(Context).RunWriteBarrierNonNullDuringMarking(Cell);
				}
			}
			else if (UObject* Object = Value.ExtractUObject())
			{
				if (UE::GC::GIsIncrementalReachabilityPending)
				{
					Object->VerseMarkAsReachable();
				}
			}
		}
		else
		{
			VCell* Cell = reinterpret_cast<VCell*>(Value);
			if (Cell && !FHeap::IsMarked(Cell))
			{
				FAccessContext(Context).RunWriteBarrierNonNullDuringMarking(Cell);
			}
		}
	}

	template <typename ContextType>
	static void RunBarrier(ContextType Context, TValue Value)
	{
		if (FHeap::IsMarking())
		{
			RunBarrierSlow(Context, Value);
		}
	}
};

template <typename TArg>
TWriteBarrier(FAccessContext, TArg&& Arg) -> TWriteBarrier<std::decay_t<TArg>>;

namespace Detail
{

template <typename T>
struct TAutoRTFMWriteFlagsFor;

template <typename T>
	requires std::is_base_of_v<VValue, T>
struct TAutoRTFMWriteFlagsFor<TWriteBarrier<T>>
{
	static_assert(sizeof(T) == sizeof(VValue));
	static constexpr AutoRTFM::EWriteFlags Value = AutoRTFM::EWriteFlags::CustomRollback;
};

template <typename T>
	requires std::is_base_of_v<TAux<void>, T>
struct TAutoRTFMWriteFlagsFor<TWriteBarrier<T>>
{
	static_assert(sizeof(T) == sizeof(TAux<void>));
	static constexpr AutoRTFM::EWriteFlags Value = AutoRTFM::EWriteFlags::CustomRollback | AutoRTFM::EWriteFlags::CustomFlag0;
};

template <typename T>
	requires std::is_base_of_v<VCell, T>
struct TAutoRTFMWriteFlagsFor<TWriteBarrier<T>>
{
	static constexpr AutoRTFM::EWriteFlags Value = AutoRTFM::EWriteFlags::CustomRollback | AutoRTFM::EWriteFlags::CustomFlag1;
};

} // namespace Detail

// The AutoRTFM::EWriteFlags to use for a call to AutoRTFM::RecordOpenWrite()
// when recording a change to a TWriteBarrier. The flags will have the
// AutoRTFM::EWriteFlags::CustomRollback bit set, which will trigger a call to
// RollbackAutoRTFMWrite() if the transaction is aborted.
// The flags may also contain one or more AutoRTFM::EWriteFlags::CustomFlagX
// bits which are used by RollbackAutoRTFMWrite() to use the appropriate
// write-barrier method for the given templated type.
template <typename T>
static constexpr AutoRTFM::EWriteFlags AutoRTFMWriteFlagsFor = Detail::TAutoRTFMWriteFlagsFor<T>::Value;

// Called by AutoRTFM when a transaction is aborted that contains writes with
// the AutoRTFM::EWriteFlags::CustomRollback flag. This flag is used exclusively
// by TWriteBarrier so that write-barriers are performed for rollbacks of the
// TWriteBarrier value.
COREUOBJECT_API void RollbackAutoRTFMWrite(void* LogicalAddress, const void* Data, size_t Size, autortfm_write_flags Flags);

template <typename T>
inline void TWriteBarrier<T>::ResetTransactionally()
	requires bCanSetTransactionally
{
	AutoRTFM::Assign(Value, TValue{}, AutoRTFMWriteFlagsFor<TWriteBarrier<T>>);
}

template <typename T>
V_FORCEINLINE void TWriteBarrier<T>::SetTransactionally(FAccessContext Context, TValue NewValue)
	requires bCanSetTransactionally
{
	RunBarrier(Context, NewValue);

	AutoRTFM::Assign(Value, NewValue, AutoRTFMWriteFlagsFor<TWriteBarrier<T>>);
}

template <typename T>
V_FORCEINLINE void TWriteBarrier<T>::SetNonCellNorPlaceholderTransactionally(VValue NewValue)
	requires bIsVValue
{
	AutoRTFM::Assign(Value, NewValue, AutoRTFMWriteFlagsFor<TWriteBarrier<T>>);
}

} // namespace Verse

template <class VCellType>
inline void FReferenceCollector::AddReferencedVerseValue(Verse::TWriteBarrier<VCellType>& InValue, const UObject* ReferencingObject, const FProperty* ReferencingProperty)
{
	if constexpr (Verse::TWriteBarrier<VCellType>::bIsAux)
	{
		static_assert(!Verse::TWriteBarrier<VCellType>::bIsAux, "AddReferencedVerseValue: Element must be a VValue or a type derived from VCell");
	}
	else if constexpr (Verse::TWriteBarrier<VCellType>::bIsVValue)
	{
		Verse::VValue Value = InValue.Get();
		if (Verse::VCell* Cell = Value.ExtractCell())
		{
			HandleVCellReference(Cell, ReferencingObject, ReferencingProperty);
		}
		else if (UObject* Object = Value.ExtractUObject())
		{
			HandleObjectReference(Object, ReferencingObject, ReferencingProperty);
		}
	}
	else
	{
		Verse::VCell* Cell = InValue.Get();
		HandleVCellReference(Cell, ReferencingObject, ReferencingProperty);
	}
}
#endif // WITH_VERSE_VM
