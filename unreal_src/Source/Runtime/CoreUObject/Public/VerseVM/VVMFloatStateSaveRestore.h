// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "CoreTypes.h"

namespace Verse
{

/// Scope guard that saves current FP state (rounding mode, flush-to-zero etc.)
/// and puts us into fully IEEE compliant mode for the duration of the scope.
class FFloatStateSaveRestoreGuard final
{
public:
	COREUOBJECT_API FFloatStateSaveRestoreGuard();
	COREUOBJECT_API ~FFloatStateSaveRestoreGuard();

private:
	// The relevant control register is 32-bit on all current targets.
	uint32 SavedState;

	// If we actually had to write the control register during construction.
	bool bWasSet;
};

} // namespace Verse
#endif // WITH_VERSE_VM
