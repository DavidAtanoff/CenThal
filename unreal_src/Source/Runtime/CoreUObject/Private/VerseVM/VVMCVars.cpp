// Copyright Epic Games, Inc. All Rights Reserved.

#include "VerseVM/VVMCVars.h"

namespace Verse
{
#if WITH_VERSE_VM || defined(__INTELLISENSE__)

// This is used as a FuncList of functions to trace (empty == trace all)
TArray<FString> TraceFuncList;
const TArray<FString>& GetTraceFuncList()
{
	return TraceFuncList;
}

TAutoConsoleVariable<bool> CVarTrace(TEXT("verse.Trace"), false, TEXT("When true, print a trace of Verse instructions executed to the log."), ECVF_Default);
TAutoConsoleVariable<FString> CVarTraceFuncList(TEXT("verse.Trace.FuncList"), TEXT(""), TEXT("A string of a comma separated list of function names to trace."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* Variable) {
		const FString FuncList = Variable->GetString().TrimQuotes();
		FuncList.ParseIntoArray(TraceFuncList, TEXT(","));
	}),
	ECVF_Default);
TAutoConsoleVariable<bool> CVarTraceSkipModuleTopLevel(TEXT("verse.Trace.SkipModuleTopLevel"), false, TEXT("When true, skip tracing top-level module compilation."), ECVF_Default);
TAutoConsoleVariable<bool> CVarTraceSingleStep(TEXT("verse.Trace.SingleStep"), false, TEXT("When true, require input from stdin before continuing to the next bytecode."), ECVF_Default);
TAutoConsoleVariable<bool> CVarDumpBytecode(TEXT("verse.DumpBytecode"), false, TEXT("When true, dump bytecode of all functions."), ECVF_Default);
TAutoConsoleVariable<bool> CVarDumpBytecodeAsCFG(TEXT("verse.DumpBytecodeAsCFG"), false, TEXT("When true, bytecode dumps as a CFG with liveness info.\n"), ECVF_Default);
TAutoConsoleVariable<int32> CVarHeapMinimumTrigger(TEXT("verse.HeapMinimumTrigger"), 100 * 1024 * 1024, TEXT("Minimum trigger used by FHeap"), ECVF_Default);
TAutoConsoleVariable<int32> CVarBytecodeRegisterAllocation(
	TEXT("verse.BytecodeRegisterAllocation"),
	static_cast<int32>(BytecodeRegisterAllocation::SetLiveRanges),
	TEXT("When 2, run register allocation over bytecode.  When 1, still set register live ranges based on liveness.  Otherwise, set live ranges to entire procedure.\n"),
	ECVF_Default);
TAutoConsoleVariable<float> CVarUObjectProbability(TEXT("verse.UObjectProbablity"), 0.0f, TEXT("Probability (0.0..1.0) that we substitute VObjects with UObjects upon creation (for testing only)."), ECVF_Default);
FRandomStream RandomUObjectProbability{42}; // Constant seed so sequence is deterministic
TAutoConsoleVariable<bool> CVarUObjectLeniency(TEXT("verse.UObjectLeniency"), false, TEXT("When true, UObject allocation is done without the effect token in the open (see SOL-8757)."), ECVF_Default);
TAutoConsoleVariable<bool> CVarEnableFastArrayAppend(TEXT("verse.EnableFastArrayAppend"), true, TEXT("When true, we enable an optimization to make appending to a mutable array faster."), ECVF_Default);

#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)

TAutoConsoleVariable<bool> CVarUseDynamicSubobjectInstancing(TEXT("verse.UseDynamicSubobjectInstancing"), true, TEXT("If true, Verse classes compiled with dynamic reference support will use dynamic subobject instancing."), ECVF_Default);
TAutoConsoleVariable<bool> CVarEnableAssetClassRedirectors(TEXT("verse.EnableAssetClassRedirectors"), false, TEXT("If true, asset class aliases will be added to the list of redirectors for their package."), ECVF_Default);
TAutoConsoleVariable<bool> CVarFullyQualifyModulesInManifest(TEXT("verse.FullyQualifyModulesInManifest"), true, TEXT("If true, modules in the manifest will be fully qualified."), ECVF_Default);
/** Fallback to legacy behaviour */
TAutoConsoleVariable<bool> CVarEditInlineSubobjectProperties(TEXT("verse.EditInlineSubobjectProperties"), true, TEXT("If true, ObjectProperties of Verse classes will all be tagged with EditInline."), ECVF_Default);

TAutoConsoleVariable<bool> CVarForceCompileFramework(TEXT("verse.ForceCompileFramework"), WITH_VERSE_VM, TEXT("When true, compile framework packages from source instead of loading cooked packages."), ECVF_Default);

TAutoConsoleVariable<bool> CVarBreakOnVerseRuntimeError(TEXT("verse.Diagnostics.BreakOnVerseRuntimeError"), false, TEXT("If set to `true`, will break into the debugger on encountering a Verse runtime error."));

TAutoConsoleVariable<int32> CVarTruncatedCallstackMaxDepth(TEXT("Verse.Diagnostics.TruncatedCallstackMaxDepth"), 18, TEXT("If we're taking a truncated Callstack, this is the max depth of the callstack."));

} // namespace Verse
