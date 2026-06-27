// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMFloatStateSaveRestore.h"
#include "Misc/AssertionMacros.h"

#include "AutoRTFM.h"

struct AUTORTFM_DISABLE Helpers final
{
	static uint32 ReadFloatingPointState();
	static void WriteFloatingPointState(uint32);
	static uint32 DesiredFloatingPointState();
	static bool DoRelevantBitsMatchInFloatingPointState(uint32 A, uint32 B);
};

// The way to access the control registers, and what should go
// into these control registers, depends on the target
// architecture.

#if defined(_x86_64) || defined(__x86_64__) || defined(_M_AMD64)

#include <pmmintrin.h>

uint32 Helpers::ReadFloatingPointState()
{
	return _mm_getcsr();
}

void Helpers::WriteFloatingPointState(uint32 State)
{
	_mm_setcsr(State);
}

uint32 Helpers::DesiredFloatingPointState()
{
	// Our desired state is all floating point exceptions masked, round to nearest, no flush to zero.
	return _MM_MASK_MASK | _MM_ROUND_NEAREST | _MM_FLUSH_ZERO_OFF | _MM_DENORMALS_ZERO_OFF;
}

// Of these fields, we want to check the rounding mode, FTZ and DAZ fields, but don't care about exceptions.
static constexpr uint32 FloatingPointStateCheckMask = _MM_ROUND_MASK | _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;

#elif defined(__aarch64__) || defined(_M_ARM64)

#if defined(_MSC_VER) && !defined(__clang__)

// With VC++, there are intrinsics
#include <intrin.h>

uint32 Helpers::ReadFloatingPointState()
{
	// The system register read/write instructions use 64-bit registers,
	// but the actual register in AArch64 is defined to be 32-bit in the
	// ARMv8 ARM.
	return static_cast<uint32>(_ReadStatusReg(ARM64_FPCR));
}

void Helpers::WriteFloatingPointState(uint32_t State)
{
	_WriteStatusReg(ARM64_FPCR, State);
}

#elif defined(__GNUC__) || defined(__clang__)

uint32 Helpers::ReadFloatingPointState()
{
	uint64 Value;
	// The system register read/write instructions use 64-bit registers,
	// but the actual register in AArch64 is defined to be 32-bit in the
	// ARMv8 ARM.
	__asm__ volatile("mrs %0, fpcr"
					 : "=r"(Value));
	return static_cast<uint32>(Value);
}

void Helpers::WriteFloatingPointState(uint32 State)
{
	uint64 State64 = State; // Actual reg is 32b, instruction wants a 64b reg
	__asm__ volatile("msr fpcr, %0"
					 :
					 : "r"(State64));
}

#else

#error Unsupported compiler for AArch64!

#endif // Compiler select

uint32 Helpers::DesiredFloatingPointState()
{
	// Our desired state is all floating point exceptions masked, round to nearest, no flush to zero.
	return 0;
}

// We care about FZ (bit 24) = Flush-To-Zero enable and RMode (bits [23:22]) = rounding mode.
static constexpr uint32 FloatingPointStateCheckMask = 0x01c00000;

#else

#error Unrecognized target platform!

#endif // Target select

bool Helpers::DoRelevantBitsMatchInFloatingPointState(uint32 A, uint32 B)
{
	return (A & FloatingPointStateCheckMask) == (B & FloatingPointStateCheckMask);
}

namespace Verse
{

struct PreviousSavedStateAndWasSetPair final
{
	explicit PreviousSavedStateAndWasSetPair(uint32 PreviousSavedState, bool bWasSet)
		: PreviousSavedState(PreviousSavedState)
		, bWasSet(bWasSet) {}

	uint32 PreviousSavedState;
	bool bWasSet;

	static constexpr AutoRTFM::EReturnFromOpenMode AutoRTFMReturnFromOpenMode = AutoRTFM::EReturnFromOpenMode::CopyConstructInClosed;
};

FFloatStateSaveRestoreGuard::FFloatStateSaveRestoreGuard()
{
	const PreviousSavedStateAndWasSetPair PreviousSavedStateAndWasSet = AutoRTFM::Open([&] AUTORTFM_DISABLE {
		SavedState = Helpers::ReadFloatingPointState();

		const uint32 DesiredState = Helpers::DesiredFloatingPointState();

		bWasSet = !Helpers::DoRelevantBitsMatchInFloatingPointState(SavedState, DesiredState);

		if (bWasSet)
		{
			Helpers::WriteFloatingPointState(DesiredState);
		}

		return PreviousSavedStateAndWasSetPair(SavedState, bWasSet);
	});

	if (PreviousSavedStateAndWasSet.bWasSet)
	{
		AutoRTFM::PushOnAbortHandler(this, [PreviousSavedState = PreviousSavedStateAndWasSet.PreviousSavedState] {
			Helpers::WriteFloatingPointState(PreviousSavedState);
		});
	}
}

FFloatStateSaveRestoreGuard::~FFloatStateSaveRestoreGuard()
{
	if (bWasSet)
	{
		AutoRTFM::Open([&] AUTORTFM_DISABLE {
			// Check that the floating point state remained as we set it. If this fails
			// it is likely that someone trashed the control registers and did not set
			// them back correctly (and thus all operations in the VM would have computed
			// wrong results!).
			checkSlow(Helpers::DoRelevantBitsMatchInFloatingPointState(Helpers::DesiredFloatingPointState(), Helpers::ReadFloatingPointState()));
			Helpers::WriteFloatingPointState(SavedState);
		});

		AutoRTFM::PopOnAbortHandler(this);
	}
}

} // namespace Verse

#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
