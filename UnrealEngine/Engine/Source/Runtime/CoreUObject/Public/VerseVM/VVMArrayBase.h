// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM.h"
#include "Containers/StringView.h"
#include "Containers/Utf8String.h"
#include "Misc/Optional.h"
#include "VerseVM/VVMAtomics.h"
#include "VerseVM/VVMAux.h"
#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMEmergentTypeCreator.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMHeap.h"
#include "VerseVM/VVMLog.h"
#include "VerseVM/VVMVerse.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{

enum class ECompares : uint8;
enum class EValueStringFormat;
struct VInt;

enum class EArrayType : uint8
{
	None,
	VValue,
	Int32,
	Char8,
	Char32
};

inline const char* ArrayTypeToString(EArrayType ArrayType)
{
	switch (ArrayType)
	{
		case EArrayType::None:
			return "None";
		case EArrayType::VValue:
			return "VValue";
		case EArrayType::Int32:
			return "Int32";
		case EArrayType::Char8:
			return "Char8";
		case EArrayType::Char32:
			return "Char32";
	}
}

inline bool IsNullTerminatedString(EArrayType Type)
{
	return Type == EArrayType::Char8;
}

inline size_t ByteLength(EArrayType ArrayType)
{
	switch (ArrayType)
	{
		case EArrayType::None:
			return 0; // Empty-Untyped VMutableArray
		case EArrayType::VValue:
			return sizeof(TWriteBarrier<VValue>);
		case EArrayType::Int32:
			return sizeof(int32);
		case EArrayType::Char8:
			return sizeof(UTF8CHAR);
		case EArrayType::Char32:
			return sizeof(UTF32CHAR);
		default:
			V_DIE("Unhandled EArrayType encountered!");
	}
}

struct VBuffer : TAux<void>
{
	// Note: We don't need to align char/char32 arrays this way. However, that will
	// make accessing the data branch on Type. So it might never be worth doing.
	struct alignas(sizeof(VValue)) Header
	{
		uint32 NumValues;
		// These are immutable per VBuffer. This means that if the GC sees a buffer with
		// a particular type, it's type can't change while it's scanning the buffer.
		const uint32 Capacity;
		const EArrayType Type;
	};

	VBuffer() = default;

	AUTORTFM_DISABLE VBuffer(FAllocationContext Context, uint32 NumValues, uint32 Capacity, EArrayType Type)
	{
		V_DIE_IF(Type == EArrayType::None);
		V_DIE_UNLESS(Capacity >= NumValues);

		// If we are UTF8, add +1 to capacity and set a null-terminator
		uint32 AllocationCapacity = Capacity + (IsNullTerminatedString(Type) ? 1 : 0);
		V_DIE_UNLESS(AllocationCapacity > 0);

		Ptr = Context.AllocateAuxCell(sizeof(Header) + ::Verse::ByteLength(Type) * AllocationCapacity);
		new (GetHeader()) Header{NumValues, Capacity, Type};

		if (IsNullTerminatedString(Type))
		{
			SetNullTerminator();
		}
	}

	VBuffer(FAllocationContext Context, uint32 NumValues, EArrayType Type)
		: VBuffer(Context, NumValues, NumValues, Type)
	{
	}

	Header* GetHeader() const
	{
		return static_cast<Header*>(Ptr);
	}

	void* GetDataStart()
	{
		return Ptr ? static_cast<char*>(Ptr) + sizeof(Header) : nullptr;
	}

	const void* GetDataStart() const
	{
		return const_cast<VBuffer*>(this)->GetDataStart();
	}

	EArrayType GetArrayType() const
	{
		if (Header* Header = GetHeader())
		{
			checkSlow(Header->Type != EArrayType::None);
			return Header->Type;
		}
		return EArrayType::None;
	}

	uint32 Num() const
	{
		if (Header* Header = GetHeader())
		{
			return Header->NumValues;
		}
		return 0;
	}

	uint32 Capacity() const
	{
		if (Header* Header = GetHeader())
		{
			return Header->Capacity;
		}
		return 0;
	}

	size_t ByteLength() const
	{
		return Num() * ::Verse::ByteLength(GetArrayType());
	}

	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetNullTerminator()
	{
		SetChar<WriteMode>(Num(), static_cast<UTF8CHAR>(0));
	}

	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetVValue(FAllocationContext, uint32 Index, VValue);

	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetInt32(uint32 Index, int32 Value)
	{
		checkSlow(GetArrayType() == EArrayType::Int32);
		int32& Element = GetData<int32>()[Index];
		if (IsTransactional<WriteMode>())
		{
			AutoRTFM::UnreachableIfClosed("#jira SOL-8415");
			AutoRTFM::Assign(Element, Value);
		}
		else
		{
			Element = Value;
		}
	}

	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetChar(uint32 Index, UTF8CHAR Value)
	{
		checkSlow(GetArrayType() == EArrayType::Char8);
		UTF8CHAR& Element = GetData<UTF8CHAR>()[Index];
		if (IsTransactional<WriteMode>())
		{
			AutoRTFM::UnreachableIfClosed("#jira SOL-8415");
			AutoRTFM::Assign(Element, Value);
		}
		else
		{
			Element = Value;
		}
	}

	template <EWriteMode WriteMode>
	void SetChar32(uint32 Index, UTF32CHAR Value)
	{
		checkSlow(GetArrayType() == EArrayType::Char32);
		UTF32CHAR& Element = GetData<UTF32CHAR>()[Index];
		if (IsTransactional<WriteMode>())
		{
			AutoRTFM::UnreachableIfClosed("#jira SOL-8415");
			AutoRTFM::Assign(Element, Value);
		}
		else
		{
			Element = Value;
		}
	}

	template <typename T = void>
	T* GetData() { return BitCast<T*>(GetDataStart()); }

	template <typename T = void>
	const T* GetData() const { return BitCast<T*>(GetDataStart()); }
};

static_assert(IsTAux<VBuffer>);

struct VArrayBase : VHeapValue
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VHeapValue);

protected:
	TWriteBarrier<VBuffer> Buffer;

	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetBufferWithoutStoreBarrier(FAllocationContext Context, VBuffer NewBuffer)
	{
		Buffer.Set<WriteMode>(Context, NewBuffer);
	}

	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetBufferWithStoreBarrier(FAllocationContext Context, VBuffer NewBuffer)
	{
		StoreStoreFence();
		SetBufferWithoutStoreBarrier<WriteMode>(Context, NewBuffer);
	}

	static EArrayType DetermineArrayType(VValue Value)
	{
		if (Value.IsInt32())
		{
			return EArrayType::Int32;
		}
		if (Value.IsChar())
		{
			return EArrayType::Char8;
		}
		if (Value.IsChar32())
		{
			return EArrayType::Char32;
		}
		return EArrayType::VValue;
	}

	static EArrayType DetermineCombinedType(EArrayType A, EArrayType B)
	{
		if (B == EArrayType::None)
		{
			return A;
		}
		else if (A == EArrayType::None)
		{
			return B;
		}
		else if (A == B)
		{
			return A;
		}
		else
		{
			return EArrayType::VValue;
		}
	}

	VArrayBase(FAllocationContext Context, uint32 NumValues, uint32 Capacity, EArrayType ArrayType, VEmergentType* Type)
		: VHeapValue(Context, Type)
	{
		SetIsDeeplyMutable();

		V_DIE_UNLESS(Capacity >= NumValues);
		if (ArrayType != EArrayType::None && Capacity)
		{
			SetBufferWithoutStoreBarrier(Context, VBuffer(Context, NumValues, Capacity, ArrayType));

			if (ArrayType == EArrayType::VValue)
			{
				// If the caller gives up before initializing all NumValues elements, the buffer must remain legible to the GC.
				// If this becomes a performance problem, we could allow callers to opt into handling the cleanup themselves.
				FMemory::Memzero(GetData(), ByteLength());
			}
		}
		else
		{
			V_DIE_IF(NumValues);
		}
	}

	VArrayBase(FAllocationContext Context, std::initializer_list<VValue> InitList, VEmergentType* Type)
		: VHeapValue(Context, Type)
	{
		SetIsDeeplyMutable();

		if (InitList.size())
		{
			SetBufferWithoutStoreBarrier(Context, VBuffer(Context, InitList.size(), DetermineArrayType(*InitList.begin())));
			uint32 Index = 0;
			for (VValue Value : InitList)
			{
				V_DIE_UNLESS(Value);
				SetValue(Context, Index++, Value);
			}
		}
	}

	template <typename InitIndexFunc, typename = std::enable_if_t<std::is_same_v<VValue, std::invoke_result_t<InitIndexFunc, uint32>>>>
	VArrayBase(FAllocationContext Context, uint32 NumValues, InitIndexFunc&& InitFunc, VEmergentType* Type)
		: VHeapValue(Context, Type)
	{
		AutoRTFM::UnreachableIfClosed("#jira SOL-8415");

		SetIsDeeplyMutable();

		if (NumValues)
		{
			// If an uninitialized VValue is returned from the InitFunc, the caller is requesting an immediate halt.
			// Stop processing and leave the array with an empty buffer.
			if (VValue Elem0 = InitFunc(0); Elem0)
			{
				// Initialize the VBuffer based on the type of the initial VValue.
				SetBufferWithoutStoreBarrier(Context, VBuffer(Context, NumValues, DetermineArrayType(Elem0)));
				SetValue(Context, 0, Elem0);
			}
			else
			{
				return;
			}

			// Populate the rest of the buffer. As before, stop immediately (and jettison the buffer) if an
			// uninitialized VValue is returned.
			for (uint32 Index = 1; Index < NumValues; ++Index)
			{
				if (VValue ElemN = InitFunc(Index); ElemN)
				{
					SetValue(Context, Index, ElemN);
				}
				else
				{
					SetBufferWithoutStoreBarrier(Context, VBuffer());
					return;
				}
			}
		}
	}

	VArrayBase(FAllocationContext Context, FUtf8StringView String, VEmergentType* Type)
		: VHeapValue(Context, Type)
		, Buffer(Context, VBuffer(Context, String.Len(), EArrayType::Char8))
	{
		SetIsDeeplyMutable();
		if (String.Len())
		{
			FMemory::Memcpy(GetData(), String.GetData(), String.Len());
		}
	}

	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetNullTerminator()
	{
		Buffer.Get().SetNullTerminator<WriteMode>();
	}

public:
	uint32 Num() const { return Buffer.Get().Num(); }
	uint32 Capacity() const { return Buffer.Get().Capacity(); }
	bool IsInBounds(uint32 Index) const;
	bool IsInBounds(const VInt& Index, const uint32 Bounds) const;
	VValue GetValue(uint32 Index);
	const VValue GetValue(uint32 Index) const;

	template <EWriteMode WriteMode = EWriteMode::Default>
	AUTORTFM_DISABLE void ConvertDataToVValues(FAllocationContext Context, uint32 NewCapacity);

	template <EWriteMode WriteMode = EWriteMode::Default>
	AUTORTFM_DISABLE void ConvertDataToVValues(FAllocationContext Context)
	{
		ConvertDataToVValues<WriteMode>(Context, Capacity() ? Capacity() : 4);
	}

	template <EWriteMode WriteMode = EWriteMode::NonTransactional>
	AUTORTFM_DISABLE void SetValue(FAllocationContext, uint32 Index, VValue);
	AUTORTFM_DISABLE void SetValue(FAllocationContext Context, uint32 Index, VValue Value);
	AUTORTFM_DISABLE void SetValueTransactionally(FAllocationContext Context, uint32 Index, VValue Value);
	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetVValue(FAllocationContext Context, uint32 Index, VValue Value)
	{
		Buffer.Get().SetVValue<WriteMode>(Context, Index, Value);
	}
	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetInt32(uint32 Index, int32 Value)
	{
		Buffer.Get().SetInt32<WriteMode>(Index, Value);
	}
	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetChar(uint32 Index, UTF8CHAR Value)
	{
		Buffer.Get().SetChar<WriteMode>(Index, Value);
	}
	template <EWriteMode WriteMode = EWriteMode::Default>
	void SetChar32(uint32 Index, UTF32CHAR Value)
	{
		Buffer.Get().SetChar32<WriteMode>(Index, Value);
	}

	void* GetData() { return Buffer.Get().GetData(); };
	const void* GetData() const { return Buffer.Get().GetData(); };

	template <typename T>
	T* GetData() { return Buffer.Get().GetData<T>(); }
	template <typename T>
	const T* GetData() const { return Buffer.Get().GetData<T>(); }

	EArrayType GetArrayType() const { return Buffer.Get().GetArrayType(); }

	size_t ByteLength() const { return Buffer.Get().ByteLength(); }

	TOptional<FUtf8String> AsOptionalUtf8String() const
	{
		if (GetArrayType() == EArrayType::None || GetArrayType() == EArrayType::VValue)
		{
			uint32 Len = Num();
			FUtf8String String;
			String.Reserve(Len);
			for (uint32 Index = 0; Index < Len; ++Index)
			{
				if (VValue Value = GetValue(Index); LIKELY(Value.IsChar()))
				{
					String.AppendChar(Value.AsChar());
				}
				else
				{
					return {};
				}
			}
			return String;
		}
		if (::Verse::IsNullTerminatedString(GetArrayType()))
		{
			return FUtf8String(FUtf8StringView(GetData<UTF8CHAR>(), Num()));
		}
		return {};
	}

	FString AsString() const
	{
		if (TOptional<FUtf8String> Utf8String = AsOptionalUtf8String())
		{
			return FString(*Utf8String);
		}
		V_DIE("Couldn't convert Array to String!");
	}

	TOptional<FUtf8StringView> AsOptionalStringView() const
	{
		if (GetArrayType() == EArrayType::None)
		{
			return FUtf8StringView();
		}
		if (::Verse::IsNullTerminatedString(GetArrayType()))
		{
			return FUtf8StringView(GetData<UTF8CHAR>(), Num());
		}
		return {};
	}

	FUtf8StringView AsStringView() const
	{
		if (TOptional<FUtf8StringView> View = AsOptionalStringView())
		{
			return *View;
		}
		V_DIE("Couldn't convert Array to StringView!");
	}

	bool Equals(const FUtf8StringView String) const
	{
		if (GetArrayType() == EArrayType::VValue)
		{
			for (uint32 Index = 0, End = Num(); Index < End; ++Index)
			{
				VValue Val = GetValue(Index);
				if (!Val.IsChar() || Val.AsChar() != String[Index])
				{
					return false;
				}
			}
			return true;
		}
		else if (::Verse::IsNullTerminatedString(GetArrayType()))
		{
			return AsStringView().Equals(String);
		}
		return false;
	}

	AUTORTFM_DISABLE COREUOBJECT_API ECompares EqualImpl(FAllocationContext Context, VCell* Other, const TFunction<void(::Verse::VValue, ::Verse::VValue)>& HandlePlaceholder);

	COREUOBJECT_API uint32 GetTypeHashImpl();

	AUTORTFM_DISABLE COREUOBJECT_API VValue MeltImpl(FAllocationContext Context);

	void VisitMembersImpl(FAllocationContext Context, FDebuggerVisitor& Visitor);

	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);
	AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSONImpl(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs);

	template <typename ArrayType>
	AUTORTFM_DISABLE static void SerializeLayoutImpl(FAllocationContext Context, ArrayType*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

	static constexpr bool InstancedCell = true;

	// C++ ranged-based iteration
	class FConstIterator
	{
		union
		{
			const TWriteBarrier<VValue>* Barrier;
			const int32* Int32;
			const UTF8CHAR* Char;
			const UTF32CHAR* Char32;
			const void* None;
		};
		EArrayType ArrayType;

	public:
		inline VValue operator*() const
		{
			switch (ArrayType)
			{
				case EArrayType::VValue:
					return Barrier->Get().Follow();
				case EArrayType::Int32:
					return VValue::FromInt32(*Int32);
				case EArrayType::Char8:
					return VValue::Char(*Char);
				case EArrayType::Char32:
					return VValue::Char32(*Char32);
				case EArrayType::None:
				default:
					V_DIE("Unhandled EArrayType (%u) encountered!", static_cast<uint32>(ArrayType));
			}
		}

		// Don't need to worry about the data-type here as we are just doing pointer comparison
		V_FORCEINLINE bool operator==(const FConstIterator& Rhs) const { return Barrier == Rhs.Barrier; }
		V_FORCEINLINE bool operator!=(const FConstIterator& Rhs) const { return Barrier != Rhs.Barrier; }

		inline FConstIterator& operator++()
		{
			switch (ArrayType)
			{
				case EArrayType::VValue:
					++Barrier;
					break;
				case EArrayType::Int32:
					++Int32;
					break;
				case EArrayType::Char8:
					++Char;
					break;
				case EArrayType::Char32:
					++Char32;
					break;
				case EArrayType::None:
				default:
					V_DIE("Unhandled EArrayType (%u) encountered!", static_cast<uint32>(ArrayType));
			}
			return *this;
		}

	private:
		friend struct VArrayBase;
		inline FConstIterator(const TWriteBarrier<VValue>* InCurrentValue)
			: Barrier(InCurrentValue)
			, ArrayType(EArrayType::VValue) {}
		inline FConstIterator(const int32* InCurrentValue)
			: Int32(InCurrentValue)
			, ArrayType(EArrayType::Int32) {}
		inline FConstIterator(const UTF8CHAR* InCurrentValue)
			: Char(InCurrentValue)
			, ArrayType(EArrayType::Char8) {}
		inline FConstIterator(const UTF32CHAR* InCurrentValue)
			: Char32(InCurrentValue)
			, ArrayType(EArrayType::Char32) {}
		inline FConstIterator(const void* InCurrentValue)
			: None(InCurrentValue)
			, ArrayType(EArrayType::None) {}
	};

	COREUOBJECT_API FConstIterator begin() const;
	COREUOBJECT_API FConstIterator end() const;
};

} // namespace Verse
#endif // WITH_VERSE_VM
