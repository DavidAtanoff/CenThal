// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Templates/FunctionFwd.h"

class FJsonValue;

namespace Verse
{
struct FRunningContext;
struct VType;
struct VValue;

using FFromJsonCallback = TFunctionRef<VValue(
	FRunningContext,
	const FJsonValue&,
	VType&)>;
} // namespace Verse

#endif
