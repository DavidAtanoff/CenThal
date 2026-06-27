// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Containers/Map.h"
#include "Templates/FunctionFwd.h"
#include "Templates/SharedPointerFwd.h"

class FJsonObject;
class FJsonValue;

namespace Verse
{
enum class EValueJSONFormat;
enum class EVisitState;
struct FRunningContext;
struct VValue;

// This callback is used by `VValue::ToJSON` and is called before any other evaluation is done to allow callers to add custom handling
// If it returns nullptr, default handling is used
// Note: be careful of calling `VCell::ToJson` directly as that bypasses this callback
using FToJsonCallback = TFunctionRef<TSharedPtr<FJsonValue>(
	FRunningContext,
	VValue,
	EValueJSONFormat,
	TMap<const void*, EVisitState>& VisitedObjects,
	uint32 RecursionDepth,
	FJsonObject* Defs)>;
} // namespace Verse

#endif
