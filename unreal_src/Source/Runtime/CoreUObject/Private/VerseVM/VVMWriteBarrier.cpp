// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMWriteBarrier.h"
#include "VerseVM/Inline/VVMWriteBarrierInline.h"

#include "AutoRTFM.h"
#include "HAL/Platform.h"
#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMLog.h"
#include <cstdint>

namespace Verse
{

void RollbackAutoRTFMWrite(void* LogicalAddress, const void* Data, size_t Size, autortfm_write_flags Flags)
{
	using Word = uintptr_t;
	static_assert(sizeof(TWriteBarrier<VValue>) == sizeof(Word));
	constexpr size_t WordSize = sizeof(Word);
	constexpr size_t WordBits = sizeof(Word) * 8;

	FAccessContextPromise Context;
	auto Rollback = [&Context, Flags](void* Target, uintptr_t Data) {
		switch (static_cast<AutoRTFM::EWriteFlags>(Flags & ~autortfm_write_no_sanitize))
		{
			case AutoRTFMWriteFlagsFor<TWriteBarrier<VValue>>:
			{
				TWriteBarrier<VValue>* ValueSlot = reinterpret_cast<TWriteBarrier<VValue>*>(Target);
				ValueSlot->Set(Context, VValue::Decode(Data));
				break;
			}
			case AutoRTFMWriteFlagsFor<TWriteBarrier<TAux<void>>>:
			{
				TWriteBarrier<TAux<void>>* AuxSlot = reinterpret_cast<TWriteBarrier<TAux<void>>*>(Target);
				AuxSlot->Set(Context, TAux<void>(BitCast<void*>(Data)));
				break;
			}
			case AutoRTFMWriteFlagsFor<TWriteBarrier<VCell>>:
			{
				TWriteBarrier<VCell>* ValueSlot = reinterpret_cast<TWriteBarrier<VCell>*>(Target);
				ValueSlot->Set(Context, BitCast<VCell*>(Data));
				break;
			}
			default:
				V_DIE("unhandled AutoRTFM::EWriteFlags for HandleAutoRTFMCustomRollback");
				break;
		}
	};

	{
		std::byte* const StartLogicalAddress = reinterpret_cast<std::byte*>(LogicalAddress);
		std::byte* const EndLogicalAddress = StartLogicalAddress + Size;
		size_t const StartMisalignment = static_cast<size_t>(reinterpret_cast<uintptr_t>(StartLogicalAddress) & (WordSize - 1));
		size_t const EndMisalignment = static_cast<size_t>(reinterpret_cast<uintptr_t>(EndLogicalAddress) & (WordSize - 1));

		if (UNLIKELY(StartMisalignment > 0 || EndMisalignment > 0))
		{
			std::byte* const AlignedStartLogicalAddress = StartLogicalAddress - StartMisalignment;
			std::byte* const AlignedEndLogicalAddress = EndLogicalAddress - EndMisalignment;
			std::byte const* const StartDataAddress = reinterpret_cast<const std::byte*>(Data);
			std::byte const* const EndDataAddress = StartDataAddress + Size;
			std::byte const* const AlignedStartData = StartDataAddress - StartMisalignment;
			std::byte const* const AlignedEndData = EndDataAddress - EndMisalignment;

			V_DIE_UNLESS_MSG(PLATFORM_LITTLE_ENDIAN, "partial word handling currently assumes little endianness");
			struct FPartialWord
			{
				std::byte* AlignedLogicalAddress = nullptr;
				Word Data = 0; // Word-aligned masked load.
			};
			static FPartialWord PartialWord;

			auto Bitmask = [](size_t StartBit, size_t EndBit) {
				uint64_t StartMask = StartBit < WordBits ? (static_cast<Word>(1) << StartBit) - 1 : ~static_cast<Word>(0);
				uint64_t EndMask = EndBit < WordBits ? (static_cast<Word>(1) << EndBit) - 1 : ~static_cast<Word>(0);
				return StartMask ^ EndMask;
			};

			if (UNLIKELY(StartLogicalAddress > AlignedEndLogicalAddress))
			{
				// This call is a has an address span that has an unaligned start,
				// and an end within the same aligned word.
				// No rollback can be performed this call.
				// Merge the data into any existing partial word, and return.
				V_DIE_UNLESS_MSG((PartialWord.Data & Bitmask(0, EndMisalignment * 8)) == 0,
					"existing partial word has unexpected bits set in its data");
				V_DIE_UNLESS(PartialWord.AlignedLogicalAddress == nullptr || PartialWord.AlignedLogicalAddress == AlignedStartLogicalAddress);
				Word const Mask = Bitmask(StartMisalignment * 8, EndMisalignment * 8);
				Word const MaskedData = FPlatformMemory::ReadUnaligned<Word>(AlignedEndData) & Mask;
				PartialWord.AlignedLogicalAddress = AlignedStartLogicalAddress;
				PartialWord.Data = MaskedData | PartialWord.Data;
				return;
			}

			if (PartialWord.AlignedLogicalAddress)
			{
				// The last call started with a partial word, and this call must complete it.
				V_DIE_UNLESS(AlignedEndLogicalAddress == PartialWord.AlignedLogicalAddress);

				Word const Mask = Bitmask(0, EndMisalignment * 8);
				V_DIE_UNLESS_MSG((PartialWord.Data & Mask) == 0,
					"existing partial word has unexpected bits set in its data");

				Word const MaskedData = FPlatformMemory::ReadUnaligned<Word>(AlignedEndData) & Mask;
				Word const WholeWord = MaskedData | PartialWord.Data;
				Rollback(AlignedEndLogicalAddress, WholeWord);

				Size -= EndMisalignment;
				PartialWord = {};
			}

			if (StartMisalignment)
			{
				// This call started with a partial word.
				// Store this partial word as it must be completed with the next call(s).
				Word const Mask = Bitmask(StartMisalignment * 8, WordBits);
				PartialWord.AlignedLogicalAddress = AlignedStartLogicalAddress;
				PartialWord.Data = FPlatformMemory::ReadUnaligned<Word>(AlignedStartData) & Mask;

				size_t const TrimBytes = WordSize - StartMisalignment;
				Size -= TrimBytes;
				LogicalAddress = StartLogicalAddress + TrimBytes;
				Data = StartDataAddress + TrimBytes;
			}
		}
	}

	V_DIE_UNLESS((reinterpret_cast<uintptr_t>(LogicalAddress) & (WordSize - 1)) == 0);
	V_DIE_UNLESS((Size & (WordSize - 1)) == 0);

	Word* const Target = reinterpret_cast<Word*>(LogicalAddress);
	size_t const Count = Size / sizeof(Word);
	for (size_t I = 0; I < Count; I++)
	{
		std::byte const* const Source = reinterpret_cast<const std::byte*>(Data) + sizeof(Word) * I;
		Rollback(&Target[I], FPlatformMemory::ReadUnaligned<Word>(Source));
	}
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
