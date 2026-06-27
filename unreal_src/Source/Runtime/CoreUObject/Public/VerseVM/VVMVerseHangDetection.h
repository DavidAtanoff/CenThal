// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

#ifndef UE_TRACK_ENSURE_OVERHEAD_DURING_SCRIPTS
#define UE_TRACK_ENSURE_OVERHEAD_DURING_SCRIPTS (DO_CHECK || DO_GUARD_SLOW || DO_ENSURE)
#endif

namespace VerseHangDetection
{

COREUOBJECT_API float VerseHangThreshold();
COREUOBJECT_API bool ShouldExcludeOverhead();
COREUOBJECT_API bool IsComputationLimitExceeded(const double StartTime, double HangThreshold = VerseHangThreshold());

/**
 * Installs callbacks to be invoked when entering and exiting overhead scopes.
 *
 * @param OnOverheadBegin Callback invoked when entering an overhead scope.
 * @param OnOverheadEnd Callback invoked when exiting an overhead scope (with the time duration spent within it).
 */
COREUOBJECT_API void InstallOverheadTracking(TFunction<void()> OnOverheadBegin, TFunction<void(double Duration)> OnOverheadEnd);

/**
 * Clears installed callbacks that were being used for tracking overhead scopes.
 */
COREUOBJECT_API void UninstallOverheadTracking();

/**
 * Marks the beginning of an overhead scope.
 */
COREUOBJECT_API void BeginOverheadScope();

/**
 * Marks the end of an overhead scope.
 */
COREUOBJECT_API void EndOverheadScope();

} // namespace VerseHangDetection