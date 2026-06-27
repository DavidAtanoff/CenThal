// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/IConsoleManager.h"

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "Math/RandomStream.h"
#endif // WITH_VERSE_VM

namespace Verse
{
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
const TArray<FString>& GetTraceFuncList();

extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarTrace;
extern COREUOBJECT_API TAutoConsoleVariable<FString> CVarTraceFuncList;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarTraceSkipModuleTopLevel;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarTraceSingleStep;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarDumpBytecode;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarDumpBytecodeAsCFG;
extern COREUOBJECT_API TAutoConsoleVariable<int32> CVarHeapMinimumTrigger;
namespace BytecodeRegisterAllocation
{
enum EBytecodeRegisterAllocation : int32
{
	Off = 0,
	SetLiveRanges = 1,
	AllocateRegisters = 2
};
} // namespace BytecodeRegisterAllocation
extern COREUOBJECT_API TAutoConsoleVariable<int32> CVarBytecodeRegisterAllocation;
extern COREUOBJECT_API TAutoConsoleVariable<float> CVarUObjectProbability;
extern COREUOBJECT_API FRandomStream RandomUObjectProbability;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarUObjectLeniency;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarEnableFastArrayAppend;
#endif // WITH_VERSE_VM

extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarUseDynamicSubobjectInstancing;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarEnableAssetClassRedirectors;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarFullyQualifyModulesInManifest;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarForceCompileFramework;
extern COREUOBJECT_API TAutoConsoleVariable<bool> CVarBreakOnVerseRuntimeError;
extern COREUOBJECT_API TAutoConsoleVariable<int32> CVarTruncatedCallstackMaxDepth;
} // namespace Verse
