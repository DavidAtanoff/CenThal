// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM/Defines.h"

class FString;

namespace Verse
{
struct FOp;
struct FAllocationContext;
struct VProcedure;

AUTORTFM_DISABLE COREUOBJECT_API FString PrintProcedure(FAllocationContext, VProcedure& Function);

} // namespace Verse
#endif // WITH_VERSE_VM
