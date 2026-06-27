// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "AutoRTFM.h"

#include "Containers/StaticArray.h"
#include "Containers/StringConv.h"
#include "Containers/Utf8String.h"

#include "HAL/Platform.h"
#include "HAL/PlatformMisc.h"

#include "UObject/OverridableManager.h"
#include "UObject/UnrealType.h"
#include "UObject/VerseValueProperty.h"

#include "VerseVM/Inline/VVMArrayBaseInline.h"
#include "VerseVM/Inline/VVMClassInline.h"
#include "VerseVM/Inline/VVMEqualInline.h"
#include "VerseVM/Inline/VVMIntInline.h"
#include "VerseVM/Inline/VVMMapInline.h"
#include "VerseVM/Inline/VVMMutableArrayInline.h"
#include "VerseVM/Inline/VVMNativeConstructorWrapperInline.h"
#include "VerseVM/Inline/VVMPlaceholderInline.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMScopeInline.h"
#include "VerseVM/Inline/VVMUniqueStringInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMValueObjectInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/Inline/VVMWriteBarrierInline.h"
#include "VerseVM/VVMAccessor.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMArrayBase.h"
#include "VerseVM/VVMBytecode.h"
#include "VerseVM/VVMBytecodeOps.h"
#include "VerseVM/VVMBytecodesAndCaptures.h"
#include "VerseVM/VVMCVars.h"
#include "VerseVM/VVMCreateFieldInlineCache.h"
#include "VerseVM/VVMDebugger.h"
#include "VerseVM/VVMEmergentType.h"
#include "VerseVM/VVMEngineEnvironment.h"
#include "VerseVM/VVMFailureContext.h"
#include "VerseVM/VVMFalse.h"
#include "VerseVM/VVMFastFailureContext.h"
#include "VerseVM/VVMFloat.h"
#include "VerseVM/VVMFrame.h"
#include "VerseVM/VVMFunction.h"
#include "VerseVM/VVMGlobalHeapPtr.h"
#include "VerseVM/VVMInstantiationContext.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMLoadFieldInlineCache.h"
#include "VerseVM/VVMLog.h"
#include "VerseVM/VVMMap.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMNames.h"
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMOption.h"
#include "VerseVM/VVMProcedure.h"
#include "VerseVM/VVMProfilingLibrary.h"
#include "VerseVM/VVMRational.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMRuntimeError.h"
#include "VerseVM/VVMSamplingProfiler.h"
#include "VerseVM/VVMSuspension.h"
#include "VerseVM/VVMTask.h"
#include "VerseVM/VVMUniqueString.h"
#include "VerseVM/VVMUnreachable.h"
#include "VerseVM/VVMValue.h"
#include "VerseVM/VVMValuePrinting.h"
#include "VerseVM/VVMVerse.h"

#include <cstdio>

#if !defined(PLATFORM_COMPILER_IWYU)
static_assert(UE_AUTORTFM, "New VM depends on AutoRTFM.");
#endif

namespace Verse
{

// The Interpreter is organized into two main execution loops: the main loop and the suspension loop.
// The main loop works like a normal interpreter loop. Control flow falls through from one bytecode
// to the next. We also have jump instructions which can divert control flow. However, since Verse
// also has failure, the bytecode has support for any bytecode that fails jumping to the current
// failure context's "on fail" bytecode destination. The way this works is that the BeginFailureContext
// and EndFailureContext bytecodes form a pair. The BeginFailureContext specifies where to jump to in
// the event of failure. Notably, if failure doesn't happen, the EndFailureContext bytecode must execute.
// This means that BeginFailureContext and EndFailureContext should be control equivalent -- we can't
// have jumps that jump over an EndFailureContext bytecode from within the failure context range.
//
// The bytecode also has builtin support for Verse's lenient execution model. This support is fundamental
// to the execution model of the bytecode. Bytecode instructions can suspend when a needed input
// operand is not concrete -- it's a placeholder -- and then resume execution when the input operand
// becomes concrete. Bytecode suspensions will capture their input operands and use the captured operands
// when they resume execution. When a placeholder becomes concrete unlocking a suspension, that suspension
// will execute in the suspension interpreter loop. The reason bytecode suspensions capture their input
// operands is so that those bytecode frame slots can be reused by the rest of the bytecode program.
// Because the operands aren't reloaded from the frame, and instead from the suspension, our bytecode
// generator can have a virtual register allocation algorithm that doesn't need to take into account
// liveness constraints dictated by leniency. This invariant has interesting implications executing a
// failure context leniently. In that scenario, we need to capture everything that's used both in the
// then/else branch. (For now, we implement this by just cloning the entire frame.) It's a goal to
// share as much code as we can between the main and suspension interpreter loops. That's why there
// are overloaded functions and interpreter-loop-specific macros that can handle both bytecode
// structs and suspension captures.
//
// Because of leniency, the interpreter needs to be careful about executing effects in program order. For
// example, if you have two effectful bytecodes one after the other, and the first one suspends, then the
// second one can't execute until the first one finishes. To handle this, we track an effect token that we
// thread through the program. Effectful operations will require the effect token to be concrete. They only
// execute after the token is concrete. Effectful operations always define a new non-concrete effect token.
// Only after the operation executes will it set the effect token to be concrete.
//
// Slots in the bytecode are all unification variables in support of Verse's general unification variable
// semantics. In our runtime, a unification variable is either a normal concrete value or a placeholder.
// A placeholder is used to support leniency. A placeholder can be used to unify two non-concrete variables.
// A placeholder can also point at a list of suspensions to fire when it becomes concrete. And finally, a
// placeholder can be mutated to point at a concrete value. When the runtime mutates a placeholder to
// point at a concrete value, it will fire its list of suspensions.
//
// Logically, a bytecode frame is initialized with empty placeholders. Every local variable in Verse is a
// unification variable. However, we really want to avoid this placeholder allocation for every local. After
// all, most locals will be defined before they're used. We optimize this by making these slots VRestValue
// instead of VPlaceholder. A VRestValue can be thought of a promise to produce a VPlaceholder if it's used
// before it has a concretely defined value. However, if we define a value in a bytecode slot before it's
// used, we can elide the allocation of the VPlaceholder altogether.

/*
  # Object archetype construction semantics
  ## Basic terminology

  A class **constructor** contains the bytecode of its body (including field initializers, `block`s, `let`s, etc.).  A
  **constructor** represents a similar thing for the body of constructor functions. These are also referred to as **body
  worker functions**.

  An **archetype** is a data structure that just represents the fields that can be initialized by a constructor/body
  worker function, along with storing the type of each field. We use this for determining the shape of an object and
  which fields' data will live in the object versus living in the shape; this tells us how to allocate the memory for
  said object.


  ## Constructors, delegating constructors, and side effects

  Constructors can forward to other constructors (this is also referred to as a _delegating constructor_).
  The semantics here are illustrated in the following examples:

  A basic example:

  ```
  c1 := class { A:int = 1 }

  MakeC1_1<constructor>():= c1:
	  block{SideEffectB()}
	  A := 3 # This doesn't unify because `MakeC1` already initializes `A`
	  block{SideEffectC()}}

  MakeC1<constructor>():= c1:
	  A := 2
	  block{SideEffectA()}
	  MakeC1_1<constructor>() # This is to a call of `c1`, which executes as per-normal.

  O:= MakeC1()
  O.A = 2
  # The order of the side effects are as indicated by the function names in lexicographical order.
  ```
  The side effects execute in the order `SideEffectA`, then `SideEffectB`, and then finally `SideEffectC`.

  However, in the case of a delegating constructor to a base class, the semantics are slightly different. In such a
  case, before the call to the delegating constructor, the current class's body is run first, along with any side
  effects that it produces, _then_ the call to the delegating constructor is made. The following example illustrates:

  ```
  c1 := class { A:int = 1 }
  c2 := class(c1):
	  block {SideEffectB()}
	  A<override>:int = 2 # This is what actually initializes `A`
	  block {SideEffectC()}

  MakeC1_1<constructor>():= c1:
	  block{SideEffectE()}
	  A := 3
	  block{SideEffectF()}}

  MakeC1<constructor>():= c1:
	  A := 4
	  block{SideEffectD()}
	  MakeC1_1<constructor>() # This is to a call of `c1`, which executes as per-normal.

  MakeC2<constructor>():= c2:
	  block{SideEffectA()}
	  # Before `MakeC1` is called, we call `c2`'s constructor.
	  # Note that we skip calling `c1`'s constructor from `c2`'s constructor in this case.
	  MakeC1<constructor>()

  O:= MakeC2()
  O.A = 2
  # The order of the side effects are as indicated by the function names in lexicographical order.
  ```

  Similarly, the side effects here execute in the order `SideEffectA`, then `SideEffectB`, then `SideEffectC`, and so on.

  In order to implement these semantics correctly, we keep track of fields that we've already initialized using the
  `CreateField` instruction, relying on the invariant that an uninitialized `VValue` represents an uninitialized field.

  In the archetypes, we set them to either point to the delegating archetype representing the nested constructor,
  or, if none exists, we set it to the class body constructor (since an archetype may not initialize all fields in the class).
  The base class body archetype will, naturally, point to nothing. When we construct a new object, we walk the archetype
  linked list and determine the entries that will be initialized in the object/shape, which is how we determine the emergent
  type to create/vend for the object.
 */

// This is used as a special PC to get the interpreter to break out of its loop.
COREUOBJECT_API FOpErr StopInterpreterSentry;
// This is used as a special PC to get the interpreter to throw a runtime error from the watchdog.
FOpErr ThrowRuntimeErrorSentry;

namespace
{

enum class EExecutionLoopKind
{
	Main,
	Suspension
};

struct AUTORTFM_DISABLE FExecutionState
{
	FOp* PC{nullptr};
	VFrame* Frame{nullptr};

	const TWriteBarrier<VValue>* Constants{nullptr};
	VRestValue* Registers{nullptr};
	FValueOperand* Operands{nullptr};
	FLabelOffset* Labels{nullptr};

	FExecutionState(FOp* PC, VFrame* Frame)
		: PC(PC)
		, Frame(Frame)
		, Constants(Frame->Procedure->GetConstantsBegin())
		, Registers(Frame->Registers)
		, Operands(Frame->Procedure->GetOperandsBegin())
		, Labels(Frame->Procedure->GetLabelsBegin())
	{
	}

	FExecutionState() = default;
	FExecutionState(const FExecutionState&) = default;
	FExecutionState(FExecutionState&&) = default;
	FExecutionState& operator=(const FExecutionState&) = default;
};

// In Verse, all functions conceptually take a single argument tuple
// To avoid unnecessary boxing and unboxing of VValues, we add an optimization where we try to avoid boxing/unboxing as much as possible
// This function reconciles the number of expected parameters with the number of provided arguments and boxes/unboxes only as needed
template <typename ArgFunction, typename StoreFunction>
AUTORTFM_DISABLE static void CopyPositionalArguments(
	FAllocationContext Context,
	uint32 NumArgs,
	ArgFunction GetArg,
	StoreFunction StoreArg)
{
	/* direct passing */
	for (uint32 Arg = 0; Arg < NumArgs; ++Arg)
	{
		StoreArg(Arg, GetArg(Arg));
	}
}

template <typename ArgFunction, typename StoreFunction>
AUTORTFM_DISABLE static void CopyPositionalArguments(
	FAllocationContext Context,
	uint32 NumParams,
	uint32 NumArgs,
	ArgFunction GetArg,
	StoreFunction StoreArg)
{
	if (NumArgs == NumParams)
	{
		CopyPositionalArguments(Context, NumArgs, GetArg, StoreArg);
	}
	else if (NumArgs == 1)
	{
		// Function wants loose arguments but a tuple is provided - unbox them
		VValue IncomingArg = GetArg(0);
		VArrayBase& Args = IncomingArg.StaticCast<VArrayBase>();

		V_DIE_UNLESS(NumParams == Args.Num());
		for (uint32 Param = 0; Param < NumParams; ++Param)
		{
			StoreArg(Param, Args.GetValue(Param));
		}
	}
	else if (NumParams == 1)
	{
		// Function wants loose arguments in a box, ie:
		// F(X:tuple(int, int)):int = X(0) + X(1)
		// F(3, 5) = 8 <-- we need to box these
		VArray& ArgArray = VArray::New(Context, NumArgs, GetArg);
		StoreArg(0, ArgArray);
	}
	else
	{
		V_DIE("Unexpected parameter/argument count mismatch");
	}
}

template <typename NamedArgFunction, typename NamedStoreFunction>
AUTORTFM_DISABLE static void CopyNamedArguments(
	FAllocationContext Context,
	uint32 NumNamedParams,
	FNamedParam* NamedParams,
	TArrayView<TWriteBarrier<VUniqueString>> NamedArgs,
	NamedArgFunction GetNamedArg,
	NamedStoreFunction StoreNamedArg)
{
	const uint32 NumNamedArgs = NamedArgs.Num();
	for (uint32 NamedParamIdx = 0; NamedParamIdx < NumNamedParams; ++NamedParamIdx)
	{
		VValue ValueToStore;
		for (uint32 NamedArgIdx = 0; NamedArgIdx < NumNamedArgs; ++NamedArgIdx)
		{
			if (NamedParams[NamedParamIdx].Name.Get() == NamedArgs[NamedArgIdx].Get())
			{
				ValueToStore = GetNamedArg(NamedArgIdx);
				break;
			}
		}
		StoreNamedArg(NamedParamIdx, ValueToStore);
	}
}

template <typename ArgFunction, typename StoreFunction, typename NamedArgFunction, typename NamedStoreFunction>
AUTORTFM_DISABLE static void CopyArguments(
	FAllocationContext Context,
	uint32 NumParams,
	uint32 NumNamedParams,
	uint32 NumArgs,
	FNamedParam* NamedParams,
	TArrayView<TWriteBarrier<VUniqueString>> NamedArgs,
	ArgFunction GetArg,
	StoreFunction StoreArg,
	NamedArgFunction GetNamedArg,
	NamedStoreFunction StoreNamedArg)
{
	CopyPositionalArguments(Context, NumParams, NumArgs, GetArg, StoreArg);
	CopyNamedArguments(Context, NumNamedParams, NamedParams, NamedArgs, GetNamedArg, StoreNamedArg);
}

template <typename ReturnSlotType>
AUTORTFM_DISABLE V_FORCEINLINE static VFrame& MakeFrameForCallee(
	FRunningContext Context,
	FOp* CallerPC,
	VFrame* CallerFrame,
	ReturnSlotType ReturnSlot,
	VProcedure& Procedure,
	VValue Self,
	VScope* Scope)
{
	VFrame& Frame = VFrame::New(Context, CallerPC, CallerFrame, ReturnSlot, Procedure);

	check(FRegisterIndex::PARAMETER_START + Procedure.NumPositionalParameters + Procedure.NumNamedParameters <= Procedure.NumRegisters);

	Frame.Registers[FRegisterIndex::SELF].Set(Context, Self);
	Frame.Registers[FRegisterIndex::SCOPE].Set(Context, Scope ? VValue(*Scope) : VValue(GlobalFalse()));

	return Frame;
}

template <typename ReturnSlotType, typename ArgFunction>
AUTORTFM_DISABLE V_FORCEINLINE static VFrame& MakeFrameForCallee(
	FRunningContext Context,
	FOp* CallerPC,
	VFrame* CallerFrame,
	ReturnSlotType ReturnSlot,
	VProcedure& Procedure,
	VValue Self,
	VScope* Scope,
	ArgFunction GetArg)
{
	VFrame& Frame = MakeFrameForCallee(Context, CallerPC, CallerFrame, ReturnSlot, Procedure, Self, Scope);

	CopyPositionalArguments(
		Context, Procedure.NumPositionalParameters,
		GetArg,
		[&](uint32 Param, VValue Value) {
			Frame.Registers[FRegisterIndex::PARAMETER_START + Param].Set(Context, Value);
		});

	return Frame;
}

template <typename ReturnSlotType, typename ArgFunction, typename NamedArgFunction>
AUTORTFM_DISABLE V_FORCEINLINE static VFrame& MakeFrameForCallee(
	FRunningContext Context,
	FOp* CallerPC,
	VFrame* CallerFrame,
	ReturnSlotType ReturnSlot,
	VProcedure& Procedure,
	VValue Self,
	VScope* Scope,
	const uint32 NumArgs,
	TArrayView<TWriteBarrier<VUniqueString>> NamedArgs,
	ArgFunction GetArg,
	NamedArgFunction GetNamedArg)
{
	VFrame& Frame = MakeFrameForCallee(Context, CallerPC, CallerFrame, ReturnSlot, Procedure, Self, Scope);

	CopyArguments(
		Context, Procedure.NumPositionalParameters, Procedure.NumNamedParameters, NumArgs, Procedure.GetNamedParamsBegin(), NamedArgs,
		GetArg,
		[&](uint32 Param, VValue Value) {
			Frame.Registers[FRegisterIndex::PARAMETER_START + Param].Set(Context, Value);
		},
		GetNamedArg,
		[&](uint32 NamedParam, VValue Value) {
			Frame.Registers[Procedure.GetNamedParamsBegin()[NamedParam].Index.Index].Set(Context, Value);
		});

	return Frame;
}
} // namespace

static constexpr bool DoStats = false;
static double NumReuses;
static double TotalNumFailureContexts;

bool ShouldTrace()
{
	return CVarTrace.GetValueOnAnyThread() && GetTraceFuncList().IsEmpty();
}

bool ShouldTrace(VProcedure& Procedure)
{
	const TArray<FString>& FuncList = GetTraceFuncList();
	if (FuncList.IsEmpty() || FuncList.Contains(Procedure.Name->AsString()))
	{
		return true;
	}
	return false;
}

class AUTORTFM_DISABLE FInterpreter
{
	FRunningContext Context;

	FExecutionState State;

	VRestValue EffectToken{0};
	VSuspension* UnblockedSuspensionQueue{nullptr};

	VFailureContext* const OutermostFailureContext;
	VTask* OutermostTask;
	// When a failure context is finished leniently, this marks the end of its `then`/`else` block.
	FOp* OutermostEndPC;

	FString ExecutionTrace;
	FExecutionState SavedStateForTracing;

	// Dynamically-scoped contexts. These are mirrored to and from FNativeFrame when passing between C++ and Verse.

	// TODO(SOL-7928): Remove these contexts. They are hacks for BPVM compatibility.
	TArray<FInstantiationScope> InstantiationContext;
	TArray<FPackageScope> PackageContext;

	VTask* Task;

	VBatchedRefs* BatchedRefs;

	static constexpr uint32 CachedFailureContextsCapacity = 32;
	uint32 NumCachedFailureContexts = 0; // This represents how many elements are in CachedFailureContexts.
	VFailureContext* CachedFailureContexts[CachedFailureContextsCapacity];

	// These fields are in service of the dynamic escape analysis we do of failure contexts.
	// At a high level, failure contexts escape during leniency and when we call into native.
	// If a failure context doesn't escape, we cache it for reuse. An unescaped failure context
	// is put back in the cache if we finish executing inside that failure context or if we fail.
	uint32 NumUnescapedFailureContexts = 0; // This represents the number of failure contexts at the top of the failure context stack that have not escaped yet.
	VFailureContext* _FailureContext = nullptr;

	void PushReusableFailureContext()
	{
		checkSlow(NumUnescapedFailureContexts > 0);
		NumUnescapedFailureContexts -= 1;

		if (NumCachedFailureContexts < CachedFailureContextsCapacity)
		{
			CachedFailureContexts[NumCachedFailureContexts] = _FailureContext;
			++NumCachedFailureContexts;
		}
	}

	VFailureContext* PopReusableFailureContext()
	{
		if (!NumCachedFailureContexts)
		{
			return nullptr;
		}

		if constexpr (DoStats)
		{
			NumReuses += 1.0;
		}

		--NumCachedFailureContexts;
		return CachedFailureContexts[NumCachedFailureContexts];
	}

	void EscapeFailureContext()
	{
		NumUnescapedFailureContexts = 0;
	}

	VFailureContext* FailureContext()
	{
		EscapeFailureContext();
		return _FailureContext;
	}

	V_FORCEINLINE VValue GetOperand(FValueOperand Operand)
	{
		if (Operand.IsRegister())
		{
			return State.Registers[Operand.AsRegister().Index].Get(Context);
		}
		else if (Operand.IsConstant())
		{
			return State.Constants[Operand.AsConstant().Index].Get().Follow();
		}
		else
		{
			return VValue();
		}
	}

	static VValue GetOperand(VValue Value)
	{
		return Value.Follow();
	}

	static VValue GetOperand(const TWriteBarrier<VValue>& Value)
	{
		return GetOperand(Value.Get());
	}

	TArrayView<FValueOperand> GetOperands(TOperandRange<FValueOperand> Operands)
	{
		return TArrayView<FValueOperand>(State.Operands + Operands.Index, Operands.Num);
	}

	template <typename CellType>
	TArrayView<TWriteBarrier<CellType>> GetOperands(TOperandRange<TWriteBarrier<CellType>> Immediates)
	{
		TWriteBarrier<CellType>* Constants = BitCast<TWriteBarrier<CellType>*>(State.Constants);
		return TArrayView<TWriteBarrier<CellType>>{Constants + Immediates.Index, Immediates.Num};
	}

	static TArrayView<TWriteBarrier<VValue>> GetOperands(TArray<TWriteBarrier<VValue>>& Operands)
	{
		return TArrayView<TWriteBarrier<VValue>>(Operands);
	}

	TArrayView<FLabelOffset> GetConstants(TOperandRange<FLabelOffset> Constants)
	{
		return TArrayView<FLabelOffset>(State.Labels + Constants.Index, Constants.Num);
	}

	template <typename OpType, typename = void>
	struct HasDest : std::false_type
	{
	};
	template <typename OpType>
	struct HasDest<OpType, std::void_t<decltype(OpType::Dest)>> : std::true_type
	{
	};

	// Construct a return slot for the "Dest" field of "Op" if it has one.
	template <typename OpType>
	auto MakeReturnSlot(OpType& Op)
	{
		return MakeReturnSlot(Op, HasDest<OpType>{});
	}

	template <typename OpType>
	VRestValue* MakeReturnSlot(OpType& Op, std::false_type)
	{
		return nullptr;
	}

	template <typename OpType>
	auto MakeReturnSlot(OpType& Op, std::true_type)
	{
		return MakeOperandReturnSlot(Op.Dest);
	}

	// Never used as an actual instruction operand- just for CallSetter to ignore a return value.
	VRestValue* MakeOperandReturnSlot(VRestValue* Dest)
	{
		return Dest;
	}

	VRestValue* MakeOperandReturnSlot(FRegisterIndex Dest)
	{
		return &State.Frame->Registers[Dest.Index];
	}

	VValue MakeOperandReturnSlot(const TWriteBarrier<VValue>& Dest)
	{
		return GetOperand(Dest);
	}

	template <typename OpType>
	void AssertMayYield(OpType& Op)
	{
		if constexpr (!OpType::bYields)
		{
			V_DIE("Unexpected yield");
		}
	}

	void AssertMayYield(FOpCall& Op) { V_DIE_UNLESS(Op.bCalleeYields); }
	void AssertMayYield(FOpCallWithSelf& Op) { V_DIE_UNLESS(Op.bCalleeYields); }

	// Include autogenerated functions to create captures
#include "VVMMakeCapturesFuncs.gen.h"

	void PrintOperandOrValue(FString& String, FRegisterIndex Operand)
	{
		if (Operand.Index == FRegisterIndex::UNINITIALIZED)
		{
			String += "(UNINITIALIZED)";
		}
		else
		{
			String += State.Frame->Registers[Operand.Index].ToString(Context, EValueStringFormat::CellsWithAddresses);
		}
	}

	void PrintOperandOrValue(FString& String, FValueOperand Operand)
	{
		if (Operand.IsRegister())
		{
			String += State.Frame->Registers[Operand.AsRegister().Index].ToString(Context, EValueStringFormat::CellsWithAddresses);
		}
		else if (Operand.IsConstant())
		{
			String += State.Constants[Operand.AsConstant().Index].Get().ToString(Context, EValueStringFormat::CellsWithAddresses);
		}
		else
		{
			String += "Empty";
		}
	}

	template <typename T>
	void PrintOperandOrValue(FString& String, TWriteBarrier<T>& Operand)
	{
		if constexpr (std::is_same_v<T, VValue>)
		{
			String += Operand.Get().ToString(Context, EValueStringFormat::CellsWithAddresses);
		}
		else
		{
			if (Operand)
			{
				String += Operand->ToString(Context, EValueStringFormat::CellsWithAddresses);
			}
			else
			{
				String += "(NULL)";
			}
		}
	}

	void PrintOperandOrValue(FString& String, TOperandRange<FValueOperand> Operands)
	{
		String += TEXT("(");
		const TCHAR* Separator = TEXT("");
		for (int32 Index = 0; Index < Operands.Num; ++Index)
		{
			String += Separator;
			Separator = TEXT(", ");
			PrintOperandOrValue(String, State.Operands[Operands.Index + Index]);
		}
		String += TEXT(")");
	}

	template <typename T>
	void PrintOperandOrValue(FString& String, TOperandRange<TWriteBarrier<T>> Operands)
	{
		TWriteBarrier<T>* Constants = BitCast<TWriteBarrier<T>*>(State.Constants);
		String += TEXT("(");
		const TCHAR* Separator = TEXT("");
		for (int32 Index = 0; Index < Operands.Num; ++Index)
		{
			String += Separator;
			Separator = TEXT(", ");
			PrintOperandOrValue(String, Constants[Operands.Index + Index]);
		}
		String += TEXT(")");
	}

	template <typename T>
	void PrintOperandOrValue(FString& String, TArray<TWriteBarrier<T>>& Operands)
	{
		String += TEXT("(");
		const TCHAR* Separator = TEXT("");
		for (TWriteBarrier<T>& Operand : Operands)
		{
			String += Separator;
			Separator = TEXT(", ");
			PrintOperandOrValue(String, Operand);
		}
		String += TEXT(")");
	}

	template <typename OpOrCaptures>
	FString TraceOperandsImpl(OpOrCaptures& Op, TArray<EOperandRole> RolesToPrint)
	{
		FString String;
		const TCHAR* Separator = TEXT("");
		Op.ForEachOperand([&](EOperandRole Role, auto& OperandOrValue, const TCHAR* Name) {
			AutoRTFM::UnreachableIfClosed("#jira SOL-8465");
			if (RolesToPrint.Find(Role) != INDEX_NONE)
			{
				String += Separator;
				Separator = TEXT(", ");
				String.Append(Name).Append("=");
				PrintOperandOrValue(String, OperandOrValue);
			}
		});
		return String;
	}

	template <typename OpOrCaptures>
	FString TraceInputs(OpOrCaptures& Op)
	{
		return TraceOperandsImpl(Op, {EOperandRole::Use, EOperandRole::Immediate});
	}

	template <typename OpOrCaptures>
	FString TraceOutputs(OpOrCaptures& Op)
	{
		return TraceOperandsImpl(Op, {EOperandRole::UnifyDef, EOperandRole::ClobberDef});
	}

	FString TracePrefix(VProcedure* Procedure, VRestValue* CurrentEffectToken, EOpcode Opcode, uint32 BytecodeOffset, bool bLenient)
	{
		FString String;
		String += FString::Printf(TEXT("%p"), Procedure);
		String += FString::Printf(TEXT("#%u|"), BytecodeOffset);
		if (CurrentEffectToken)
		{
			String += TEXT("EffectToken=");
			String += CurrentEffectToken->ToString(Context, EValueStringFormat::CellsWithAddresses);
			String += TEXT("|");
		}
		if (bLenient)
		{
			String += TEXT("Lenient|");
		}
		String += ToString(Opcode);
		return String;
	}

	void BeginTrace()
	{
		SavedStateForTracing = State;
		if (!ShouldTrace(*State.Frame->Procedure))
		{
			return;
		}

		if (CVarTraceSingleStep.GetValueOnAnyThread())
		{
			getchar();
		}

		if (State.PC == &StopInterpreterSentry)
		{
			UE_LOGF(LogVerseVM, Display, "StoppingExecution, encountered StopInterpreterSentry");
			return;
		}

		if (State.PC == &ThrowRuntimeErrorSentry)
		{
			UE_LOGF(LogVerseVM, Display, "StoppingExecution, encountered ThrowRuntimeErrorSentry");
			return;
		}

		ExecutionTrace = TEXT("StartingOpcode|");
		ExecutionTrace += TracePrefix(State.Frame->Procedure.Get(), &EffectToken, State.PC->Opcode, State.Frame->Procedure->BytecodeOffset(*State.PC), false);
		ExecutionTrace += TEXT("(");

#define VISIT_OP(Name)                                                     \
	case EOpcode::Name:                                                    \
	{                                                                      \
		ExecutionTrace += TraceInputs(*static_cast<FOp##Name*>(State.PC)); \
		break;                                                             \
	}

		switch (State.PC->Opcode)
		{
			VERSE_ENUM_OPS(VISIT_OP)
#undef VISIT_OP
		}

		ExecutionTrace += TEXT(")");
		UE_LOGF(LogVerseVM, Display, "%ls", *ExecutionTrace);
	}

	template <typename CaptureType>
	void BeginTrace(CaptureType& Captures, VBytecodeSuspension& Suspension)
	{
		if (!ShouldTrace(*State.Frame->Procedure))
		{
			return;
		}

		if (CVarTraceSingleStep.GetValueOnAnyThread())
		{
			getchar();
		}

		ExecutionTrace = TEXT("StartingOpcode|");
		ExecutionTrace += TracePrefix(Suspension.Procedure.Get(), nullptr, Suspension.Opcode, Suspension.BytecodeOffset, true);
		ExecutionTrace += TEXT("(");
		ExecutionTrace += TraceInputs(Captures);
		ExecutionTrace += TEXT(")");
		UE_LOGF(LogVerseVM, Display, "%ls", *ExecutionTrace);
	}

	void EndTrace(bool bSuspended, bool bFailed)
	{
		if (!ShouldTrace(*State.Frame->Procedure))
		{
			return;
		}

		FExecutionState CurrentState = State;
		State = SavedStateForTracing;

		FString Temp;

#define VISIT_OP(Name)                                           \
	case EOpcode::Name:                                          \
	{                                                            \
		Temp = TraceOutputs(*static_cast<FOp##Name*>(State.PC)); \
		break;                                                   \
	}

		switch (State.PC->Opcode)
		{
			VERSE_ENUM_OPS(VISIT_OP)
#undef VISIT_OP
		}

		ExecutionTrace = TEXT("EndingOpcode  |");
		ExecutionTrace += TracePrefix(State.Frame->Procedure.Get(), &EffectToken, State.PC->Opcode, State.Frame->Procedure->BytecodeOffset(*State.PC), false);

		if (!Temp.IsEmpty())
		{
			ExecutionTrace += TEXT("|");
			ExecutionTrace += Temp;
		}

		if (bSuspended)
		{
			ExecutionTrace += TEXT("|Suspending");
		}

		if (bFailed)
		{
			ExecutionTrace += TEXT("|Failed");
		}

		UE_LOGF(LogVerseVM, Display, "%ls", *ExecutionTrace);

		State = CurrentState;
	}

	template <typename CaptureType>
	void EndTraceWithCaptures(CaptureType& Captures, VBytecodeSuspension& Suspension, bool bSuspended, bool bFailed)
	{
		if (!ShouldTrace(*State.Frame->Procedure))
		{
			return;
		}

		ExecutionTrace = TEXT("EndingOpcode  |");
		ExecutionTrace += TracePrefix(Suspension.Procedure.Get(), nullptr, Suspension.Opcode, Suspension.BytecodeOffset, true);
		ExecutionTrace += TEXT("|");
		ExecutionTrace += TraceOutputs(Captures);
		if (bSuspended)
		{
			ExecutionTrace += TEXT("|Suspending");
		}

		if (bFailed)
		{
			ExecutionTrace += TEXT("|Failed");
		}
		UE_LOGF(LogVerseVM, Display, "%ls", *ExecutionTrace);
	}

	static void AddSuspension(FAllocationContext Context, VSuspension*& SuspensionQueue, VSuspension* Suspension)
	{
		if (SuspensionQueue)
		{
			SuspensionQueue->Last().Next.Set(Context, Suspension);
		}
		else
		{
			SuspensionQueue = Suspension;
		}
	}

	static ECompares Def(FRunningContext Context, FTrail& Trail, VValue ResultSlot, VValue Value, VSuspension*& SuspensionsToFire)
	{
		// The comparison returns equal if we encounter a placeholder
		return VValue::Equal(Context, ResultSlot, Value, [&](VValue Left, VValue Right) {
			// Given how the interpreter is structured, we know these must be resolved
			// to placeholders. They can't be pointing to values or we should be using
			// the value they point to.
			AutoRTFM::UnreachableIfClosed("#jira SOL-8465");
			checkSlow(!Left.IsPlaceholder() || Left.Follow().IsPlaceholder());
			checkSlow(!Right.IsPlaceholder() || Right.Follow().IsPlaceholder());

			if (Left.IsPlaceholder())
			{
				if (Right.IsPlaceholder())
				{
					Left.GetRootPlaceholder().Unify(Context, Trail, Right.GetRootPlaceholder());
				}
				else
				{
					AddSuspension(Context, SuspensionsToFire, Left.GetRootPlaceholder().SetValue(Context, Right));
				}
			}
			else
			{
				AddSuspension(Context, SuspensionsToFire, Right.GetRootPlaceholder().SetValue(Context, Left));
			}
		});
	}

	ECompares Def(FTrail& Trail, VValue ResultSlot, VValue Value)
	{
		return Def(Context, Trail, ResultSlot, Value, UnblockedSuspensionQueue);
	}

	ECompares Def(FTrail& Trail, const TWriteBarrier<VValue>& ResultSlot, VValue Value)
	{
		return Def(Trail, GetOperand(ResultSlot), Value);
	}

	V_FORCEINLINE static ECompares Def(FRunningContext Context, FTrail& Trail, VRestValue& ResultSlot, VValue Value, VSuspension*& SuspensionsToFire)
	{
		// TODO: This needs to consider split depth eventually.
		if (LIKELY(ResultSlot.CanDefQuickly()))
		{
			// Note: once `Move` no longer immediately follows `Reset`, this
			// `Set` will need to be `SetTrailed`.
			ResultSlot.Set(Context, Value);
			return ECompares::Eq;
		}
		return Def(Context, Trail, ResultSlot.Get(Context), Value, SuspensionsToFire);
	}

	V_FORCEINLINE ECompares Def(FTrail& Trail, VRestValue& ResultSlot, VValue Value)
	{
		return Def(Context, Trail, ResultSlot, Value, UnblockedSuspensionQueue);
	}

	V_FORCEINLINE static ECompares DefTrailed(FRunningContext Context, FTrail& Trail, VRestValue& ResultSlot, VValue Value, VSuspension*& SuspensionsToFire)
	{
		// TODO: This needs to consider split depth eventually.
		if (LIKELY(ResultSlot.CanDefQuickly()))
		{
			ResultSlot.SetTrailed(Context, Trail, Value);
			return ECompares::Eq;
		}
		return Def(Context, Trail, ResultSlot.Get(Context), Value, SuspensionsToFire);
	}

	V_FORCEINLINE ECompares DefTrailed(FTrail& Trail, VRestValue& ResultSlot, VValue Value)
	{
		return DefTrailed(Context, Trail, ResultSlot, Value, UnblockedSuspensionQueue);
	}

	V_FORCEINLINE ECompares Def(FTrail& Trail, FRegisterIndex ResultSlot, VValue Value)
	{
		return Def(Trail, State.Frame->Registers[ResultSlot.Index], Value);
	}

	V_FORCEINLINE ECompares DefTrailed(FTrail& Trail, FRegisterIndex ResultSlot, VValue Value)
	{
		return DefTrailed(Trail, State.Frame->Registers[ResultSlot.Index], Value);
	}

	// Never used as an actual instruction operand- just for CallSetter to ignore a return value.
	V_FORCEINLINE ECompares Def(FTrail& Trail, VRestValue* ResultSlot, VValue Value)
	{
		if (ResultSlot)
		{
			return Def(Context, Trail, *ResultSlot, Value, UnblockedSuspensionQueue);
		}
		else
		{
			return ECompares::Eq;
		}
	}

	template <bool bTrailed>
	V_FORCEINLINE static ECompares DefReturnSlotImpl(FRunningContext Context, FTrail& Trail, VReturnSlot& ReturnSlot, VValue Value, VSuspension*& SuspensionsToFire)
	{
		if (ReturnSlot.Kind == VReturnSlot::EReturnKind::RestValue)
		{
			if (ReturnSlot.RestValue)
			{
				if constexpr (bTrailed)
				{
					return DefTrailed(Context, Trail, *ReturnSlot.RestValue, Value, SuspensionsToFire);
				}
				else
				{
					return Def(Context, Trail, *ReturnSlot.RestValue, Value, SuspensionsToFire);
				}
			}
			else
			{
				return ECompares::Eq;
			}
		}
		else
		{
			checkSlow(ReturnSlot.Kind == VReturnSlot::EReturnKind::Value);
			// If this ends up doing a placeholder unification, placeholder unifications are always trailed.
			return Def(Context, Trail, ReturnSlot.Value.Get(), Value, SuspensionsToFire);
		}
	}

	ECompares DefReturnSlotTrailed(FTrail& Trail, VReturnSlot& ReturnSlot, VValue Value)
	{
		return DefReturnSlotImpl<true>(Context, Trail, ReturnSlot, Value, UnblockedSuspensionQueue);
	}

	ECompares DefReturnSlot(FTrail& Trail, VReturnSlot& ReturnSlot, VValue Value)
	{
		return DefReturnSlotImpl<false>(Context, Trail, ReturnSlot, Value, UnblockedSuspensionQueue);
	}

	// ValueForOp returns a reference for the value of the given operand.
	// This is overloaded for a `TWriteBarrier<VValue>&` or `FRegisterIndex`
	// parameter type to match the main loop and the suspension loop operand
	// types.
	V_FORCEINLINE TWriteBarrier<VValue>& ValueForOp(TWriteBarrier<VValue>& Op)
	{
		return Op;
	}
	V_FORCEINLINE VRestValue& ValueForOp(FRegisterIndex Op)
	{
		return State.Frame->Registers[Op.Index];
	}

	V_FORCEINLINE FTrail& GetTrailFor(FOp&)
	{
		return _FailureContext->Trail;
	}

	template <typename CapturesType UE_REQUIRES(std::is_base_of_v<FBytecodeSuspensionCaptures, CapturesType>)>
	V_FORCEINLINE FTrail& GetTrailFor(CapturesType& Captures)
	{
		return VBytecodeSuspension::FromCaptures(Captures).FailureContext->Trail;
	}

	void BumpEffectEpoch()
	{
		EffectToken.Reset(0);
	}

	FOpResult::EKind FinishedExecutingFailureContextLeniently(
		VFailureContext& FailureContext,
		FOp* StartPC,
		FOp* EndPC,
		VValue NextEffectToken)
	{
		VFailureContext* ParentFailure = FailureContext.Parent.Get();

		FOpResult::EKind Result = FOpResult::Return;

		if (StartPC < EndPC)
		{
			VFrame* Frame = FailureContext.Frame.Get();

			// When we cloned the frame for lenient execution, we guarantee the caller info
			// isn't set because when this is done executing, it should not return to the
			// caller at the time of creation of the failure context. It should return back here.
			V_DIE_IF(Frame->CallerFrame || Frame->CallerPC);

			V_DIE_UNLESS(ParentFailure);

			FInterpreter Interpreter(
				Context,
				FExecutionState(StartPC, Frame),
				NextEffectToken,
				ParentFailure,
				FailureContext.Task.Get(),
				FailureContext.BatchedRefs.Get(),
				EndPC);
			Result = Interpreter.Execute();
			if (Result == FOpResult::Error)
			{
				return Result;
			}
			if (Result != FOpResult::Fail)
			{
				ParentFailure->Trail.Exit(Context);
			}

			// TODO: We need to think through exactly what control flow inside
			// of the then/else of a failure context means. For example, then/else
			// can contain a break/return, but we might already be executing past
			// that then/else leniently. So we need to somehow find a way to transfer
			// control of the non-lenient execution. This likely means the below
			// def of the effect token isn't always right.

			// This can't fail.
			Def(ParentFailure->Trail, FailureContext.DoneEffectToken, Interpreter.EffectToken.Get(Context));
		}
		else
		{
			// This can't fail.
			Def(ParentFailure->Trail, FailureContext.DoneEffectToken, NextEffectToken);
		}

		if (ParentFailure && !ParentFailure->bFailed)
		{
			// We increment the suspension count for our parent failure
			// context when this failure context sees lenient execution.
			// So this is the decrement to balance that out that increment.
			return FinishedExecutingSuspensionIn(*ParentFailure);
		}
		return Result;
	}

	FOpResult::EKind FinishedExecutingSuspensionIn(VFailureContext& FailureContext)
	{
		// This is reachable because lenient instructions in a fast failure context will run this
		// after unblocking code that might cause a then/else to run which might cause an outer
		// failure context to fail. Below, the unification of X with zero causes the inner fast-failure
		// if to unblock, which causes the outer if to fail. After that chain reaction from unblocking
		// the suspension, we call this function.
		//
		// F():int =
		//     var Y:int = 0
		//     X:int
		//     if:
		//         if (X = 0) { 0 = 1 } else { Err() }
		//     then:
		//         Err()
		//     else:
		//         set Y += 1
		//     Z := Y
		//     X = 0
		//     Z
		if (FailureContext.bFailed)
		{
			return FOpResult::Fail;
		}

		V_DIE_IF(FailureContext.bFailed);

		V_DIE_UNLESS(FailureContext.SuspensionCount);
		uint32 RemainingCount = --FailureContext.SuspensionCount;
		if (RemainingCount)
		{
			return FOpResult::Return;
		}

		if (LIKELY(!FailureContext.bExecutedEndFailureContextOpcode))
		{
			return FOpResult::Return;
		}

		FailureContext.FinishedExecuting(Context);
		FOp* StartPC = FailureContext.ThenPC;
		FOp* EndPC = FailureContext.DonePC;
		// Since we finished executing all suspensions in this failure context without failure, we can now commit the transaction
		VValue NextEffectToken = FailureContext.BeforeThenEffectToken.Get(Context);
		if (NextEffectToken.IsPlaceholder())
		{
			VValue NewNextEffectToken = VValue::Placeholder(VPlaceholder::New(Context, 0));
			DoTransactionActionWhenEffectTokenIsConcrete<TransactAction::Commit>(FailureContext, *FailureContext.Task, NextEffectToken, NewNextEffectToken);
			NextEffectToken = NewNextEffectToken;
		}
		else
		{
			FailureContext.Transaction.Commit(Context);
		}

		return FinishedExecutingFailureContextLeniently(FailureContext, StartPC, EndPC, NextEffectToken);
	}

	FOpResult::EKind Fail()
	{
#if DO_GUARD_SLOW
		if (NumUnescapedFailureContexts)
		{
			V_DIE_IF(_FailureContext->SuspensionCount);
			V_DIE_IF(_FailureContext->bExecutedEndFailureContextOpcode);
		}
#endif
		// This doesn't escape the failure context.
		return Fail(*_FailureContext);
	}

	FOpResult::EKind Fail(VFailureContext& FailureContext)
	{
		if (FailureContext.bFailed)
		{
			// Fail() can be called more than once when CallNativeFunction()
			// calls back into the VM, which then triggers a failure using the
			// same FailureContext.
			// FailureContext.bFailed is set to true by VFailureContext::Fail(),
			// so we early out here if it is already set.
			return FOpResult::Return;
		}

		V_DIE_UNLESS(Task == FailureContext.Task.Get());

		FailureContext.Fail(Context);
		FailureContext.FinishedExecuting(Context);

		if (LIKELY(!FailureContext.bExecutedEndFailureContextOpcode))
		{
			return FOpResult::Return;
		}

		FOp* StartPC = FailureContext.FailurePC;
		FOp* EndPC = FailureContext.DonePC;
		VValue NextEffectToken = FailureContext.IncomingEffectToken.Get();

		return FinishedExecutingFailureContextLeniently(FailureContext, StartPC, EndPC, NextEffectToken);
	}

	// Returns true if unwinding succeeded. False if we are trying to unwind past
	// the outermost frame of this Interpreter instance.
	bool UnwindIfNeeded()
	{
		if (NumUnescapedFailureContexts)
		{
			// When we suspend in a failure context, we escape that failure context.
			// When we unblock a suspension, we also escape all unescaped failure contexts
			// at the top of the stack.
			//
			// So, if we make it here after encountering failure, it means we could only
			// have failed in a non-lenient context, so therefore, we could only have failed
			// at the top-most failure context.
#if DO_GUARD_SLOW
			{
				VFailureContext* FailureContext = _FailureContext->Parent.Get();
				for (uint32 I = 0; I < NumUnescapedFailureContexts - 1; ++I)
				{
					V_DIE_IF(FailureContext->bFailed);
					FailureContext = FailureContext->Parent.Get();
				}
			}
#endif

			if (_FailureContext->bFailed)
			{
				PushReusableFailureContext();
				State = FExecutionState(_FailureContext->FailurePC, _FailureContext->Frame.Get());
				EffectToken.Set(Context, _FailureContext->IncomingEffectToken.Get());
				_FailureContext = _FailureContext->Parent.Get();
			}

			return true;
		}

		if (!FailureContext()->bFailed)
		{
			return true;
		}

		VFailureContext* FailedContext = FailureContext();
		while (true)
		{
			if (FailedContext == OutermostFailureContext)
			{
				return false;
			}

			VFailureContext* Parent = FailedContext->Parent.Get();
			if (!Parent->bFailed)
			{
				break;
			}
			FailedContext = Parent;
		}

		State = FExecutionState(FailedContext->FailurePC, FailedContext->Frame.Get());
		_FailureContext = FailedContext->Parent.Get();
		EffectToken.Set(Context, FailedContext->IncomingEffectToken.Get());

		return true;
	}

	template <typename ReturnSlotType>
	void Suspend(VTask& SuspendingTask, ReturnSlotType ResumeSlot)
	{
		SuspendingTask.Suspend(Context);
		SuspendingTask.ResumeSlot.SetTransactionally(Context, ResumeSlot);
	}

	// Returns true if yielding succeeded. False if we are trying to yield past
	// the outermost frame of this Interpreter instance.
	bool YieldIfNeeded(FOp* NextPC)
	{
		while (true)
		{
			if (Task->bRunning)
			{
				// The task is still active or already unwinding.
				if (Task->Phase != VTask::EPhase::CancelStarted)
				{
					return true;
				}

				if (Task->CancelChildren(Context))
				{
					BeginUnwind(NextPC);
					return true;
				}

				Task->Suspend(Context);
			}
			else
			{
				if (Task->Phase == VTask::EPhase::CancelRequested)
				{
					Task->SetPhaseTransactionally(VTask::EPhase::CancelStarted);
					if (Task->CancelChildren(Context))
					{
						Task->Resume(Context);
						BeginUnwind(NextPC);
						return true;
					}
				}
			}

			VTask* SuspendedTask = Task;

			// Save the current state for when the task is resumed.
			SuspendedTask->SetResumePCTransactionally(NextPC);
			SuspendedTask->ResumeFrame.SetTransactionally(Context, State.Frame);

			// Switch back to the task that started or resumed this one.
			State = FExecutionState(SuspendedTask->YieldPC, SuspendedTask->YieldFrame.Get());
			Task = SuspendedTask->YieldTask.Get();

			// Detach the task from the stack.
			SuspendedTask->SetYieldPCTransactionally(&StopInterpreterSentry);
			SuspendedTask->YieldFrame.SetTransactionally(Context, VFrame::GlobalEmptyFrame.Get());
			SuspendedTask->YieldTask.ResetTransactionally();

			if (SuspendedTask == OutermostTask)
			{
				return false;
			}

			NextPC = State.PC;
		}
	}

	// Jump from PC to its associated unwind label, in the current function or some transitive caller.
	// There must always be some unwind label, because unwinding always terminates at EndTask.
	void BeginUnwind(FOp* PC)
	{
		V_DIE_UNLESS(Task->bRunning);

		Task->SetPhaseTransactionally(VTask::EPhase::CancelUnwind);
		Task->ExecNativeDefer(Context);

		for (VFrame* Frame = State.Frame; Frame != nullptr; PC = Frame->CallerPC, Frame = Frame->CallerFrame.Get())
		{
			VProcedure* Procedure = Frame->Procedure.Get();
			int32 Offset = Procedure->BytecodeOffset(PC);

			for (
				FUnwindEdge* UnwindEdge = Procedure->GetUnwindEdgesBegin();
				UnwindEdge != Procedure->GetUnwindEdgesEnd() && UnwindEdge->Begin < Offset;
				++UnwindEdge)
			{
				if (Offset <= UnwindEdge->End)
				{
					State = FExecutionState(UnwindEdge->OnUnwind.GetLabeledPC(), Frame);
					return;
				}
			}
		}

		VERSE_UNREACHABLE();
	}

	enum class TransactAction
	{
		Start,
		Commit
	};

	template <TransactAction Action>
	void DoTransactionActionWhenEffectTokenIsConcrete(VFailureContext& FailureContext, VTask& TaskContext, VValue IncomingEffectToken, VValue NextEffectToken)
	{
		VLambdaSuspension& Suspension = VLambdaSuspension::New(
			Context, FailureContext, TaskContext,
			[](FRunningContext TheContext, VLambdaSuspension& LambdaSuspension, VSuspension*& SuspensionsToFire) {
				if constexpr (Action == TransactAction::Start)
				{
					LambdaSuspension.FailureContext->Transaction.Start(TheContext);
				}
				else
				{
					LambdaSuspension.FailureContext->Transaction.Commit(TheContext);
				}
				VValue NextEffectToken = LambdaSuspension.Args()[0].Get();
				FInterpreter::Def(TheContext, LambdaSuspension.FailureContext->Parent->Trail, NextEffectToken, VValue::EffectDoneMarker(), SuspensionsToFire);
			},
			NextEffectToken);

		IncomingEffectToken.EnqueueSuspension(Context, Suspension);
	}

	// Macros to be used both directly in the interpreter loops and impl functions.
	// Parameterized over the implementation of ENQUEUE_SUSPENSION, FAIL, and YIELD.

#define REQUIRE_CONCRETE(Value)          \
	if (UNLIKELY(Value.IsPlaceholder())) \
	{                                    \
		ENQUEUE_SUSPENSION(Value);       \
	}

#define DEF(Result, Value) \
	DEF_RESULT_HELPER(Def(GetTrailFor(Op), Result, Value))

#define DEF_RESULT_HELPER(Result)             \
	switch (Result)                           \
	{                                         \
		case ECompares::Eq:                   \
			break;                            \
		case ECompares::Neq:                  \
			FAIL();                           \
		case ECompares::Undecidable:          \
			V_DIE("Undecidable unification"); \
		case ECompares::RuntimeError:         \
			RUNTIME_ERROR();                  \
		default:                              \
			VERSE_UNREACHABLE();              \
	}

#define OP_RESULT_HELPER(Result)                  \
	if (!Result.IsReturn())                       \
	{                                             \
		if (Result.Kind == FOpResult::Block)      \
		{                                         \
			check(Result.Value.IsPlaceholder());  \
			ENQUEUE_SUSPENSION(Result.Value);     \
		}                                         \
		else if (Result.Kind == FOpResult::Fail)  \
		{                                         \
			FAIL();                               \
		}                                         \
		else if (Result.Kind == FOpResult::Yield) \
		{                                         \
			YIELD();                              \
		}                                         \
		else if (Result.Kind == FOpResult::Error) \
		{                                         \
			RUNTIME_ERROR(Result.Value);          \
		}                                         \
		else                                      \
		{                                         \
			VERSE_UNREACHABLE();                  \
		}                                         \
	}

	// Macro definitions to be used in impl functions.

#define ENQUEUE_SUSPENSION(Value) \
	return FOpResult              \
	{                             \
		FOpResult::Block, Value   \
	}

#define FAIL()          \
	return FOpResult    \
	{                   \
		FOpResult::Fail \
	}

#define YIELD()          \
	return FOpResult     \
	{                    \
		FOpResult::Yield \
	}

#define RUNTIME_ERROR(Value)    \
	return FOpResult            \
	{                           \
		FOpResult::Error, Value \
	}

#define RAISE_RUNTIME_ERROR_CODE(Context, Diagnostic)                                           \
	const Verse::SRuntimeDiagnosticInfo& DiagnosticInfo = GetRuntimeDiagnosticInfo(Diagnostic); \
	Context.RaiseVerseRuntimeError(Diagnostic, FText::FromString(DiagnosticInfo.Description));

#define RAISE_RUNTIME_ERROR(Context, Diagnostic, Message) \
	Context.RaiseVerseRuntimeError(Diagnostic, FText::FromString(DiagnosticInfo.Description));

#define RAISE_RUNTIME_ERROR_FORMAT(Context, Diagnostic, FormatString, ...) \
	Context.RaiseVerseRuntimeError(Diagnostic, FText::FromString(FString::Printf(FormatString, ##__VA_ARGS__)));

	template <typename Token>
	V_FORCEINLINE VFastFailureContext& UpdateIndicatorForSuspend(FTrail& Trail, Token LeniencyIndicator, FOp* OnFailure)
	{
		VValue LeniencyIndicatorValue = GetOperand(LeniencyIndicator);
		if constexpr (std::is_same_v<Token, FRegisterIndex>)
		{
			VFastFailureContext* FastContext;
			if (!LeniencyIndicatorValue.IsPlaceholder())
			{
				FastContext = &LeniencyIndicatorValue.StaticCast<VFastFailureContext>();
			}
			else
			{
				FastContext = &VFastFailureContext::New(Context);
				FastContext->BatchedRefs.Set(Context, BatchedRefs);
				FastContext->IncomingEffectToken.Set(Context, EffectToken.Get(Context));
			}
			FastContext->Suspensions++;
			checkSlow(!FastContext->FailurePC || FastContext->FailurePC == OnFailure);
			FastContext->FailurePC = OnFailure;
			Def(Trail, State.Registers[LeniencyIndicator.Index], VValue(*FastContext));
			return *FastContext;
		}
		else
		{
			VFastFailureContext& FastContext = LeniencyIndicatorValue.StaticCast<VFastFailureContext>();
			FastContext.Suspensions++;
			return FastContext;
		}
	}

	template <typename OpType>
	V_FORCEINLINE FOpResult SuspendOnLeniencyIndicator(OpType& Op, VValue ToSuspendOn)
	{
		UpdateIndicatorForSuspend(GetTrailFor(Op), Op.LeniencyIndicator, Op.OnFailure.GetLabeledPC());
		ENQUEUE_SUSPENSION(ToSuspendOn);
	}

	template <typename OpType, typename Exec, typename LVal, typename RVal>
	V_FORCEINLINE FOpResult FastFailHelper(OpType& Op, FOp*& NextPC, LVal VLhs, RVal VRhs, Exec F)
	{
		VValue Lhs = GetOperand(VLhs);
		if (UNLIKELY(Lhs.IsPlaceholder()))
		{
			return SuspendOnLeniencyIndicator(Op, Lhs);
		}
		VValue Rhs = GetOperand(VRhs);
		if (UNLIKELY(Rhs.IsPlaceholder()))
		{
			return SuspendOnLeniencyIndicator(Op, Rhs);
		}

		FOpResult Ret = F(Lhs, Rhs);
		if (Ret.Kind != FOpResult::Fail)
		{
			if constexpr (HasDest<OpType>::value)
			{
				DEF(Op.Dest, Ret.Value);
			}
		}
		else
		{
			NextPC = Op.OnFailure.GetLabeledPC();
			VValue LeniencyIndicator = GetOperand(Op.LeniencyIndicator);
			if (!LeniencyIndicator.IsPlaceholder())
			{
				VFastFailureContext& FastFailureContext = LeniencyIndicator.StaticCast<VFastFailureContext>();
				FastFailureContext.bFailed = true;

				if constexpr (std::is_base_of_v<FOp, OpType>) // We're a normal, non-suspension, op.
				{
					checkSlow(!FastFailureContext.FailurePC || FastFailureContext.FailurePC == Op.OnFailure.GetLabeledPC());
					FastFailureContext.FailurePC = Op.OnFailure.GetLabeledPC();

					if (FastFailureContext.DidExecuteEndFastFailureContext())
					{
						// We can only get here, I think (I hope?), when we are resuming an inner fast failure
						// context's then/else leniently. We are in a nested interpreter instance because we're
						// running normal interpreter code but within a fast failure context that already ran its
						// EndFastFailureContext opcode. In such a case, we should run our else, and then bail out
						// of the inner interpreter loop, since its execution was limited to be within a then or else.
						FOpResult Result = ExecuteFastFailureThenOrElseLeniently(FastFailureContext);
						if (Result.Kind == FOpResult::Error)
						{
							return Result;
						}
						NextPC = &StopInterpreterSentry;
					}
				}
			}
		}
		return FOpResult{FOpResult::Return};
	}

	V_FORCEINLINE FOpResult ExecuteFastFailureThenOrElseLeniently(VFastFailureContext& FastFailureContext)
	{
		FOp* NextPC = FastFailureContext.bFailed ? FastFailureContext.FailurePC : FastFailureContext.ThenPC;
		FOpResult::EKind Result;
		{
			VFailureContext* EnclosingFailureContext = FastFailureContext.EnclosingFailureContext.Get();
			FInterpreter Interpreter(
				Context,
				FExecutionState(NextPC, FastFailureContext.Frame.Get()),
				// Because we don't allow <writes> in a fast failure context, it's OK to use this effect token both for the then/else.
				FastFailureContext.IncomingEffectToken.Get(Context),
				EnclosingFailureContext,
				Task,
				FastFailureContext.BatchedRefs.Get(),
				FastFailureContext.DonePC);
			Result = Interpreter.Execute();

			if (Result == FOpResult::Error)
			{
				return FOpResult{FOpResult::Error};
			}

			if (Result != FOpResult::Fail)
			{
				EnclosingFailureContext->Trail.Exit(Context);
			}

			// This can't fail.
			Def(EnclosingFailureContext->Trail, FastFailureContext.DoneEffectToken, Interpreter.EffectToken.Get(Context));
		}

		// Now that we have executed, unblock our parent, if any.
		// And, if we were the last thing blocking the parent, recursively run it as well.
		if (VFastFailureContext* Parent = FastFailureContext.Parent.Get();
			Parent
			&& !Parent->bFailed
			&& --Parent->Suspensions == 0
			&& Parent->DidExecuteEndFastFailureContext())
		{
			[[clang::musttail]] return ExecuteFastFailureThenOrElseLeniently(*FastFailureContext.Parent);
		}

		return FOpResult{FOpResult::Return};
	}

	template <typename OpType, typename Exec>
	V_FORCEINLINE FOpResult FastFailSuspensionHelper(OpType& Op, Exec F)
	{
		VValue LeniencyIndicator = Op.LeniencyIndicator.Get();
		checkSlow(!LeniencyIndicator.IsPlaceholder());
		VFastFailureContext& FastFailContext = LeniencyIndicator.StaticCast<VFastFailureContext>();
		if (FastFailContext.bFailed)
		{
			return FOpResult{FOpResult::Return};
		}

		FOp* NextPC = nullptr;
		FOpResult Ret = F(Op, NextPC);
		if (Ret.Kind == FOpResult::Block)
		{
			return Ret;
		}

		// If we failed, run the if's else.
		if (FastFailContext.bFailed)
		{
			return ExecuteFastFailureThenOrElseLeniently(FastFailContext);
		}

		V_DIE_UNLESS(FastFailContext.Suspensions > 0);
		// If we ran our last suspension and the failure context finished running, run the if's then.
		if (--FastFailContext.Suspensions == 0 && FastFailContext.DidExecuteEndFastFailureContext())
		{
			return ExecuteFastFailureThenOrElseLeniently(FastFailContext);
		}

		return FOpResult{FOpResult::Return};
	}

	VRational& PrepareRationalSourceHelper(VValue& Source)
	{
		if (VRational* RationalSource = Source.DynamicCast<VRational>())
		{
			return *RationalSource;
		}

		V_DIE_UNLESS_MSG(Source.IsInt(), "Unsupported operands were passed to a Rational operation!");

		return VRational::New(Context, Source.AsInt(), VInt(Context, 1));
	}

	template <typename OpType>
	FOpResult AddImpl(OpType& Op)
	{
		VValue LeftSource = GetOperand(Op.LeftSource);
		VValue RightSource = GetOperand(Op.RightSource);
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);

		if (LeftSource.IsInt() && RightSource.IsInt())
		{
			DEF(Op.Dest, VInt::Add(Context, LeftSource.AsInt(), RightSource.AsInt()));
		}
		else if (LeftSource.IsFloat() && RightSource.IsFloat())
		{
			DEF(Op.Dest, LeftSource.AsFloat() + RightSource.AsFloat());
		}
		else if (LeftSource.IsCellOfType<VRational>() || RightSource.IsCellOfType<VRational>())
		{
			VRational& LeftRational = PrepareRationalSourceHelper(LeftSource);
			VRational& RightRational = PrepareRationalSourceHelper(RightSource);

			DEF(Op.Dest, VRational::Add(Context, LeftRational, RightRational).StaticCast<VCell>());
		}
		else if (LeftSource.IsCellOfType<VArrayBase>() && RightSource.IsCellOfType<VArrayBase>())
		{
			// Array concatenation.
			VArrayBase& LeftArray = LeftSource.StaticCast<VArrayBase>();
			VArrayBase& RightArray = RightSource.StaticCast<VArrayBase>();

			DEF(Op.Dest, VArray::Concat(Context, LeftArray, RightArray));
		}
		else
		{
			auto LeftString = LeftSource.ToString(Context, EValueStringFormat::VerseSyntax);
			auto RightString = RightSource.ToString(Context, EValueStringFormat::VerseSyntax);
			V_DIE("Unsupported operands %hs and %hs were passed to a `Add` operation!", ToCStr(LeftString), ToCStr(RightString));
		}

		return {FOpResult::Return};
	}

	// TODO: Add the ability for bytecode instructions to have optional arguments so instead of having this bytecode
	//		 we can just have 'Add' which can take a boolean telling it whether the result should be mutable.
	template <typename OpType>
	FOpResult MutableAddImpl(OpType& Op)
	{
		VValue LeftSource = GetOperand(Op.LeftSource);
		VValue RightSource = GetOperand(Op.RightSource);
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);

		if (LeftSource.IsCellOfType<VArrayBase>() && RightSource.IsCellOfType<VArrayBase>())
		{
			// Array concatenation.
			VArrayBase& LeftArray = LeftSource.StaticCast<VArrayBase>();
			VArrayBase& RightArray = RightSource.StaticCast<VArrayBase>();

			DEF(Op.Dest, VMutableArray::Concat(Context, LeftArray, RightArray));
		}
		else
		{
			V_DIE("Unsupported operands were passed to a `MutableAdd` operation!");
		}

		return {FOpResult::Return};
	}

	FOpResult CanFastAppendToArrayFastFailImplHelper(VValue Ref, VValue MaybeMutableArray)
	{
		// We can do a fast += on a var array if it's a plain non-native Verse array.
		// We don't support accessors or domain functions for vars.
		if (VRef* RealRef = Ref.DynamicCast<VRef>())
		{
			if (RealRef->GetDomain())
			{
				return FOpResult{FOpResult::Fail};
			}
		}
		else
		{
			// Right now, accessors and native refs don't have callable domains. This
			// code will have to change if we grant them those capabilities.

			// Also, note that code path for appending to an array in a struct field will pass
			// in the empty VValue (VValue()) to this code path for Ref.
		}

		if (MaybeMutableArray.IsCellOfType<VMutableArray>())
		{
			return FOpResult{FOpResult::Return};
		}
		return FOpResult{FOpResult::Fail};
	}

	template <typename OpType>
	FOpResult FastAppendToArrayImpl(OpType& Op, FOp*& NextPC, VValue IncomingEffectToken)
	{
		VValue LeftSource = GetOperand(Op.LeftSource);
		VValue RightSource = GetOperand(Op.RightSource);
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);

		VArrayBase* Right = RightSource.DynamicCast<VArrayBase>();
		V_DIE_UNLESS(Right);
		VMutableArray& MutableArray = LeftSource.StaticCast<VMutableArray>();
		// This is needed for live variables to work. The very first write into an array in a transaction
		// will upgrade it to be VValue backed. This is because when you do await{Array}, we non-transactionally
		// upgrade the array to be VValue backed. Because it's non-transactional, this doesn't play well with
		// transactional updates. There are situations where we could end up with a stale VBuffer in the array
		// after the transaction aborts. Therefore, all transactional writes must see the same VValue backed
		// array that the await sees.
		//
		// This isn't a fundamental limitation, and we should lift it at some point. But for now, this is the
		// most expedient path to get to correctly working code.
		MutableArray.ConvertDataToVValues<EWriteMode::NonTransactional>(Context);
		for (uint32 Num = Right->Num(), I = 0; I < Num; ++I)
		{
			VValue MeltedValue = VValue::Melt(Context, Right->GetValue(I));
			if (MeltedValue.IsPlaceholder())
			{
				AUTORTFM_SANITIZER_DISABLE_SCOPE();
				// This doesn't need to be transactional because the first AddValueTransactionally will
				// already have already changed the length transactionally.
				MutableArray.Truncate(I);
				REQUIRE_CONCRETE(MeltedValue);
				VERSE_UNREACHABLE();
			}
			MutableArray.AddValueTransactionally(Context, MeltedValue);
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult SubImpl(OpType& Op)
	{
		VValue LeftSource = GetOperand(Op.LeftSource);
		VValue RightSource = GetOperand(Op.RightSource);
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);

		if (LeftSource.IsInt() && RightSource.IsInt())
		{
			DEF(Op.Dest, VInt::Sub(Context, LeftSource.AsInt(), RightSource.AsInt()));
		}
		else if (LeftSource.IsFloat() && RightSource.IsFloat())
		{
			DEF(Op.Dest, LeftSource.AsFloat() - RightSource.AsFloat());
		}
		else if (LeftSource.IsCellOfType<VRational>() || RightSource.IsCellOfType<VRational>())
		{
			VRational& LeftRational = PrepareRationalSourceHelper(LeftSource);
			VRational& RightRational = PrepareRationalSourceHelper(RightSource);

			DEF(Op.Dest, VRational::Sub(Context, LeftRational, RightRational).StaticCast<VCell>());
		}
		else
		{
			V_DIE("Unsupported operands were passed to a `Sub` operation!");
		}

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult MulImpl(OpType& Op)
	{
		VValue LeftSource = GetOperand(Op.LeftSource);
		VValue RightSource = GetOperand(Op.RightSource);
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);

		if (LeftSource.IsInt())
		{
			if (RightSource.IsInt())
			{
				DEF(Op.Dest, VInt::Mul(Context, LeftSource.AsInt(), RightSource.AsInt()));
				return {FOpResult::Return};
			}
			else if (RightSource.IsFloat())
			{
				DEF(Op.Dest, LeftSource.AsInt().ConvertToFloat() * RightSource.AsFloat());
				return {FOpResult::Return};
			}
		}
		else if (LeftSource.IsFloat())
		{
			if (RightSource.IsInt())
			{
				DEF(Op.Dest, LeftSource.AsFloat() * RightSource.AsInt().ConvertToFloat());
				return {FOpResult::Return};
			}
			else if (RightSource.IsFloat())
			{
				DEF(Op.Dest, LeftSource.AsFloat() * RightSource.AsFloat());
				return {FOpResult::Return};
			}
		}

		if (LeftSource.IsCellOfType<VRational>() || RightSource.IsCellOfType<VRational>())
		{
			VRational& LeftRational = PrepareRationalSourceHelper(LeftSource);
			VRational& RightRational = PrepareRationalSourceHelper(RightSource);

			DEF(Op.Dest, VRational::Mul(Context, LeftRational, RightRational).StaticCast<VCell>());
			return {FOpResult::Return};
		}

		V_DIE("Unsupported operands were passed to a `Mul` operation!");
	}

	template <typename OpType>
	FOpResult DivImpl(OpType& Op)
	{
		VValue LeftSource = GetOperand(Op.LeftSource);
		VValue RightSource = GetOperand(Op.RightSource);
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);

		if (LeftSource.IsInt() && RightSource.IsInt())
		{
			if (RightSource.AsInt().IsZero())
			{
				FAIL();
			}

			DEF(Op.Dest, VRational::New(Context, LeftSource.AsInt(), RightSource.AsInt()).StaticCast<VCell>());
		}
		else if (LeftSource.IsFloat() && RightSource.IsFloat())
		{
			DEF(Op.Dest, LeftSource.AsFloat() / RightSource.AsFloat());
		}
		else if (LeftSource.IsCellOfType<VRational>() || RightSource.IsCellOfType<VRational>())
		{
			VRational& LeftRational = PrepareRationalSourceHelper(LeftSource);
			VRational& RightRational = PrepareRationalSourceHelper(RightSource);
			if (RightRational.IsZero())
			{
				FAIL();
			}

			DEF(Op.Dest, VRational::Div(Context, LeftRational, RightRational).StaticCast<VCell>());
		}
		else
		{
			V_DIE("Unsupported operands were passed to a `Div` operation!");
		}

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult ModImpl(OpType& Op)
	{
		VValue LeftSource = GetOperand(Op.LeftSource);
		VValue RightSource = GetOperand(Op.RightSource);
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);

		if (LeftSource.IsInt() && RightSource.IsInt())
		{
			if (RightSource.AsInt().IsZero())
			{
				FAIL();
			}

			DEF(Op.Dest, VInt::Mod(Context, LeftSource.AsInt(), RightSource.AsInt()));
		}
		// TODO: VRational could support Mod in limited circumstances
		else
		{
			V_DIE("Unsupported operands were passed to a `Mod` operation!");
		}

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NegImpl(OpType& Op)
	{
		VValue Source = GetOperand(Op.Source);
		REQUIRE_CONCRETE(Source);

		if (Source.IsInt())
		{
			DEF(Op.Dest, VInt::Neg(Context, Source.AsInt()));
		}
		else if (Source.IsFloat())
		{
			DEF(Op.Dest, -(Source.AsFloat()));
		}
		else if (Source.IsCellOfType<VRational>())
		{
			DEF(Op.Dest, VRational::Neg(Context, Source.StaticCast<VRational>()));
		}
		else
		{
			V_DIE("Unimplemented type passed to VM `Neg` operation");
		}

		return {FOpResult::Return};
	}

	FOpResult QueryImplHelper(VValue Source)
	{
		if (Source.ExtractCell() == GlobalFalsePtr.Get())
		{
			return FOpResult{FOpResult::Fail};
		}
		else if (VOption* Option = Source.DynamicCast<VOption>()) // True = VOption(VFalse), which is handled by this case
		{
			return FOpResult{FOpResult::Return, Option->GetValue()};
		}
		else if (!Source.IsUObject())
		{
			V_DIE("Unimplemented type passed to VM `Query` operation");
		}

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult QueryImpl(OpType& Op)
	{
		VValue Source = GetOperand(Op.Source);
		REQUIRE_CONCRETE(Source);
		FOpResult Value = QueryImplHelper(Source);

		if (Value.IsReturn())
		{
			DEF(Op.Dest, Value.Value);
			return {FOpResult::Return};
		}

		FAIL();
	}

	template <typename OpType>
	FOpResult MapKeyImpl(OpType& Op)
	{
		VValue Map = GetOperand(Op.Map);
		VValue Index = GetOperand(Op.Index);
		REQUIRE_CONCRETE(Map);
		REQUIRE_CONCRETE(Index);

		if (Map.IsCellOfType<VMapBase>() && Index.IsInt())
		{
			DEF(Op.Dest, Map.StaticCast<VMapBase>().GetKey(Index.AsInt32()));
		}
		else
		{
			V_DIE("Unimplemented type passed to VM `MapKey` operation!");
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult MapValueImpl(OpType& Op)
	{
		VValue Map = GetOperand(Op.Map);
		VValue Index = GetOperand(Op.Index);
		REQUIRE_CONCRETE(Map);
		REQUIRE_CONCRETE(Index);

		if (Map.IsCellOfType<VMapBase>() && Index.IsInt())
		{
			DEF(Op.Dest, Map.StaticCast<VMapBase>().GetValue(Index.AsInt32()));
		}
		else
		{
			V_DIE("Unimplemented type passed to VM `MapValue` operation!");
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult LengthImpl(OpType& Op)
	{
		const VValue Container = GetOperand(Op.Container);
		// We need this to be concrete before we can attempt to get its size, even if the values in the container
		// might be placeholders.
		REQUIRE_CONCRETE(Container);
		if (const VArrayBase* Array = Container.DynamicCast<VArrayBase>())
		{
			DEF(Op.Dest, VInt{static_cast<int32>(Array->Num())});
		}
		else if (const VMapBase* Map = Container.DynamicCast<VMapBase>())
		{
			DEF(Op.Dest, VInt{static_cast<int32>(Map->Num())});
		}
		else
		{
			V_DIE("Unsupported container type passed!");
		}

		return {FOpResult::Return};
	}

	// TODO (SOL-5813) : Optimize melt to start at the value it suspended on rather
	// than re-doing the entire melt Op again which is what we do currently.
	//
	// When we implement FOX we need to examine whether this should thread effects
	// Currently we leave it un-ordered as we use it in conjunction with `InitializeVar`
	// for class construction which doesn't want to suspend on the effect token.
	template <typename OpType>
	FOpResult MeltImpl(OpType& Op)
	{
		VValue Value = GetOperand(Op.Value);
		VValue Result = VValue::Melt(Context, Value);
		REQUIRE_CONCRETE(Result);
		DEF(Op.Dest, Result);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult FreezeImpl(OpType& Op, FOp*& NextPC, VValue& IncomingEffectToken, VBatchedRefs* CurrentBatchedRefs)
	{
		VValue Value = GetOperand(Op.Value);
		REQUIRE_CONCRETE(Value);
		if (VAccessorRef* AccessorRef = Value.DynamicCast<VAccessorRef>())
		{
			return CallGetter(Op, *AccessorRef, NextPC, IncomingEffectToken, CurrentBatchedRefs);
		}
		else
		{
			REQUIRE_CONCRETE(IncomingEffectToken);
			FOpResult Result = VValue::Freeze(Context, Value, Task);
			if (Result.IsReturn())
			{
				DEF(Op.Dest, Result.Value);
			}
			return Result.Kind;
		}
	}

	template <typename OpType>
	FOpResult RefGetImpl(OpType& Op, FOp*& NextPC, VValue IncomingEffectToken)
	{
		VValue RefValue = GetOperand(Op.Ref);
		REQUIRE_CONCRETE(RefValue);
		VValue Result;
		if (VRef* Ref = RefValue.DynamicCast<VRef>())
		{
			if (Task->AwaitPC)
			{
				Ref->AddAwaitTask(Context, *Task);
			}
			Result = Ref->Get(Context);
		}
		else if (VNativeRef* NativeRef = RefValue.DynamicCast<VNativeRef>())
		{
			Result = *NativeRef;
		}
		else if (VAccessorRef* AccessorRef = RefValue.DynamicCast<VAccessorRef>())
		{
			Result = *AccessorRef;
		}
		else
		{
			VCell* Cell = RefValue.ExtractCell();
			V_DIE("Unexpected ref type %s", Cell ? *Cell->DebugName() : TEXT("VValue"));
		}
		DEF(Op.Dest, Result);
		return {FOpResult::Return};
	}

	bool EndBatch(VBatchedRefs& CurrentBatchedRefs)
	{
		CurrentBatchedRefs.EndBatch();
		if (CurrentBatchedRefs.Batched())
		{
			return true;
		}
		TSet<VTask*> AwaitTasks;
		CurrentBatchedRefs.ForEach([&](VRef& BatchedRef) {
			BatchedRef.ForEachAwaitTask([&](VTask& AwaitTask) {
				AwaitTasks.Add(&AwaitTask);
			});
		});
		for (VTask* AwaitTask : AwaitTasks)
		{
			if (!SignalAwaiter(VValue::EffectDoneMarker(), *AwaitTask, &CurrentBatchedRefs))
			{
				return false;
			}
		}
		CurrentBatchedRefs.Reset(Context);
		return true;
	}

	bool CancelLiveTask(VRef& Ref, VValue CurrentLiveTask)
	{
		VTask* LiveTask = Ref.GetLiveTask();
		if (!LiveTask)
		{
			return true;
		}
		if (CurrentLiveTask && LiveTask == &CurrentLiveTask.StaticCast<VTask>())
		{
			return true;
		}
		FOpResult Result = Spawn(Context, [&] { return LiveTask->Cancel(Context); });
		switch (Result.Kind)
		{
			case FOpResult::Return:
			case FOpResult::Block:
			case FOpResult::Yield:
				return true;
			case FOpResult::Fail:
				V_DIE("Illegal failure at `live` right-hand side");
			case FOpResult::Error:
				return false;
			default:
				VERSE_UNREACHABLE();
		}
	}

	bool SignalAwaiter(VValue IncomingEffectToken, VTask& AwaitTask, VBatchedRefs* CurrentBatchedRefs)
	{
		FOpResult::EKind Result = Context.PushNativeFrame(
			FailureContext(),
			Task,
			CurrentBatchedRefs,
			IncomingEffectToken,
			nullptr,
			nullptr,
			nullptr,
			[&] { return Resume(Context, GlobalFalse(), AwaitTask, AwaitTask.ResumePC); });
		switch (Result)
		{
			case FOpResult::Return:
			case FOpResult::Yield:
				return true;
			case FOpResult::Error:
				return false;
			case FOpResult::Block:
			case FOpResult::Fail:
			default:
				VERSE_UNREACHABLE();
		}
	}

	bool SignalAwaiters(VRef& Ref, VValue IncomingEffectToken, VBatchedRefs* CurrentBatchedRefs)
	{
		if (CurrentBatchedRefs && CurrentBatchedRefs->Batched())
		{
			CurrentBatchedRefs->Add(Context, Ref);
			return true;
		}
		return !Ref.AnyAwaitTask([&](VTask& AwaitTask) {
			return !SignalAwaiter(IncomingEffectToken, AwaitTask, CurrentBatchedRefs);
		});
	}

	FOpResult RefSetImpl(
		VRef& Ref,
		VValue Value,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs,
		VValue CurrentLiveTask)
	{
		REQUIRE_CONCRETE(IncomingEffectToken);
		if (!CancelLiveTask(Ref, CurrentLiveTask))
		{
			RUNTIME_ERROR();
		}
		if (CurrentLiveTask)
		{
			Ref.SetLiveTask(Context, &CurrentLiveTask.StaticCast<VTask>());
		}
		Ref.Set(Context, Value);
		if (!SignalAwaiters(Ref, IncomingEffectToken, CurrentBatchedRefs))
		{
			RUNTIME_ERROR();
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult RefSetImpl(
		OpType& Op,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs,
		VValue CurrentLiveTask)
	{
		VValue RefValue = GetOperand(Op.Ref);
		VValue Value = GetOperand(Op.Value);
		REQUIRE_CONCRETE(RefValue);
		if (VRef* Ref = RefValue.DynamicCast<VRef>())
		{
			return RefSetImpl(*Ref, Value, IncomingEffectToken, CurrentBatchedRefs, CurrentLiveTask);
		}
		if (VNativeRef* NativeRef = RefValue.DynamicCast<VNativeRef>())
		{
			V_DIE_IF(CurrentLiveTask);
			REQUIRE_CONCRETE(IncomingEffectToken);
			return NativeRef->Set(Context, Value);
		}
		if (VAccessorRef* AccessorRef = RefValue.DynamicCast<VAccessorRef>())
		{
			V_DIE_IF(CurrentLiveTask);
			return CallSetter(Op, *AccessorRef, Value, NextPC, IncomingEffectToken, CurrentBatchedRefs);
		}
		VCell* Cell = RefValue.ExtractCell();
		V_DIE("Unexpected ref type %s", Cell ? *Cell->DebugName() : TEXT("VValue"));
	}

	template <typename OpType>
	FOpResult CallSetImpl(
		OpType& Op,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs,
		VValue CurrentLiveTask)
	{
		const VValue Container = GetOperand(Op.Container);
		const VValue Argument = GetOperand(Op.Index);
		const VValue ValueToSet = GetOperand(Op.ValueToSet);
		REQUIRE_CONCRETE(Container);
		REQUIRE_CONCRETE(Argument);

		if (VAccessorRef* AccessorRef = Container.DynamicCast<VAccessorRef>())
		{
			V_DIE_IF(CurrentLiveTask);
			VAccessorRef& NewAccessorRef = VAccessorRef::New(Context, *AccessorRef, Argument);
			return CallSetter(Op, NewAccessorRef, ValueToSet, NextPC, IncomingEffectToken, CurrentBatchedRefs);
		}
		if (VNativeRef* NativeRef = Container.DynamicCast<VNativeRef>())
		{
			REQUIRE_CONCRETE(IncomingEffectToken);
			FOpResult Result = NativeRef->Call(Context, Argument, /*bSet*/ true);
			OP_RESULT_HELPER(Result);
			return Result.Value.StaticCast<VNativeRef>().Set(Context, ValueToSet);
		}
		if (VMutableArray* Array = Container.DynamicCast<VMutableArray>())
		{
			REQUIRE_CONCRETE(IncomingEffectToken);
			// Bounds check since this index access in Verse is fallible.
			if (!Argument.IsUint32())
			{
				FAIL();
			}
			uint32 Index = Argument.AsUint32();
			if (!Array->IsInBounds(Index))
			{
				FAIL();
			}
			Array->ConvertDataToVValues<EWriteMode::NonTransactional>(Context); // This is needed for live variables to work. See comment above in FastAppendToArrayImpl.
			if (VRef* Ref = Array->GetValue(Index).ExtractTransparentRef())
			{
				return RefSetImpl(*Ref, ValueToSet, IncomingEffectToken, CurrentBatchedRefs, CurrentLiveTask);
			}
			Array->SetValueTransactionally(Context, Index, ValueToSet);
			return {FOpResult::Return};
		}
		// NOTE: `VPersistentMap` is a subtype of `VMutableMap`, so keep it above `VMutableMap`
		if (VPersistentMap* PMap = Container.DynamicCast<VPersistentMap>())
		{
			REQUIRE_CONCRETE(IncomingEffectToken);
			if (!VerseVM::GetEngineEnvironment()->IsValidWeakMapKey(Argument))
			{
				RAISE_VERSE_RUNTIME_ERROR_CODE(ERuntimeDiagnostic::ErrRuntime_WeakMapInvalidKey);
				return {FOpResult::Error};
			}

			VValue ExistingValue = PMap->Find(Context, Argument);
			if (VRef* Ref = ExistingValue.ExtractTransparentRef())
			{
				return RefSetImpl(*Ref, ValueToSet, IncomingEffectToken, CurrentBatchedRefs, CurrentLiveTask);
			}
			PMap->AddTransactionally(Context, Argument, ValueToSet);

			if (ExistingValue.IsUninitialized())
			{
				VFunction& Function = GlobalProgram->GetUpdatePersistentWeakMapPlayer();
				VRestValue* Dest = nullptr;
				TArray<TWriteBarrier<VValue>> ArgumentArray = {TWriteBarrier<VValue>(Context, *PMap), TWriteBarrier<VValue>(Context, Argument)};
				TArrayView<TWriteBarrier<VValue>> Arguments = ArgumentArray;
				CallFunction(
					Op,
					Function,
					Function.Self.Follow(),
					Arguments,
					TArrayView<TWriteBarrier<VUniqueString>>{},
					TArrayView<TWriteBarrier<VValue>>{},
					Dest,
					NextPC,
					IncomingEffectToken,
					CurrentBatchedRefs);
			}

			return {FOpResult::Return};
		}
		if (VMutableMap* Map = Container.DynamicCast<VMutableMap>())
		{
			REQUIRE_CONCRETE(IncomingEffectToken);
			if (VRef* Ref = Map->Find(Context, Argument).ExtractTransparentRef())
			{
				return RefSetImpl(*Ref, ValueToSet, IncomingEffectToken, CurrentBatchedRefs, CurrentLiveTask);
			}
			Map->AddTransactionally(Context, Argument, ValueToSet);
			return {FOpResult::Return};
		}
		V_DIE("Unsupported container type passed!");
	}

	template <typename OpType>
	FOpResult RefCallDomainImpl(OpType& Op, FOp*& NextPC, VValue& IncomingEffectToken, VBatchedRefs* CurrentBatchedRefs)
	{
		VValue RefValue = GetOperand(Op.Ref);
		REQUIRE_CONCRETE(RefValue);
		VValue Argument = GetOperand(Op.Argument);
		if (VRef* Ref = RefValue.DynamicCast<VRef>())
		{
			TArrayView<VValue> Arguments{&Argument, 1};
			VValue Domain = Ref->GetDomain();
			if (!Domain)
			{
				DEF(Op.Dest, Argument);
				return {FOpResult::Return};
			}
			VFunction& Function = Domain.StaticCast<VFunction>();
			return CallFunction(
				Op,
				Function,
				Function.Self.Get(),
				Arguments,
				TArrayView<TWriteBarrier<VUniqueString>>{},
				TArrayView<TWriteBarrier<VValue>>{},
				Op.Dest,
				NextPC,
				IncomingEffectToken,
				CurrentBatchedRefs);
		}
		if (VNativeRef* NativeRef = RefValue.DynamicCast<VNativeRef>())
		{
			DEF(Op.Dest, Argument);
			return {FOpResult::Return};
		}
		if (VAccessorRef* Ref = RefValue.DynamicCast<VAccessorRef>())
		{
			DEF(Op.Dest, Argument);
			return {FOpResult::Return};
		}
		VCell* Cell = RefValue.ExtractCell();
		V_DIE("Unexpected ref type %s", Cell ? *Cell->DebugName() : TEXT("VValue"));
	}

	// Caution: This function may return with a different frame, so register to
	// value lookups must be performed before calling.
	template <typename OpType, typename ArgumentType, typename NamedArgumentType, typename NamedArgumentValueType, typename DestOperandType>
	FOpResult CallFunction(
		OpType& Op,
		VFunction& Function,
		VValue Self,
		TArrayView<ArgumentType> Arguments,
		TArrayView<NamedArgumentType> NamedArguments,
		TArrayView<NamedArgumentValueType> NamedArgumentValues,
		DestOperandType Dest,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs)
	{
		if (VProcedure* Procedure = Function.Procedure.Get().DynamicCast<VProcedure>())
		{
			if (NextPC)
			{
				VFrame& NewFrame = MakeFrameForCallee(
					Context, NextPC, State.Frame, MakeOperandReturnSlot(Dest), *Procedure, Self, Function.ParentScope.Get(),
					Arguments.Num(), NamedArguments,
					[&](uint32 Arg) { return GetOperand(Arguments[Arg]); },
					[&](uint32 NamedArg) { return GetOperand(NamedArgumentValues[NamedArg]); });
				auto UpdateExecutionState = [&](FOp* PC, VFrame* Frame) {
					State = FExecutionState(PC, Frame);
					NextPC = PC;
				};
				UpdateExecutionState(Procedure->GetOpsBegin(), &NewFrame);
				return {FOpResult::Return};
			}
			else
			{
				FOp* CallerPC = nullptr;
				VFrame* CallerFrame = nullptr;
				VFrame& NewFrame = MakeFrameForCallee(
					Context, CallerPC, CallerFrame, MakeOperandReturnSlot(Dest), *Procedure, Self, Function.ParentScope.Get(),
					Arguments.Num(), NamedArguments,
					[&](uint32 Arg) { return GetOperand(Arguments[Arg]); },
					[&](uint32 NamedArg) { return GetOperand(NamedArgumentValues[NamedArg]); });
				FInterpreter Interpreter(
					Context,
					FExecutionState(Procedure->GetOpsBegin(), &NewFrame),
					IncomingEffectToken,
					FailureContext(),
					Task,
					CurrentBatchedRefs);
				FOpResult::EKind Result = Interpreter.Execute();
				IncomingEffectToken = Interpreter.EffectToken.Get(Context);
				return Result;
			}
		}
		else if (VNativeProcedure* NativeProcedure = Function.Procedure.Get().DynamicCast<VNativeProcedure>())
		{
			return CallNativeFunction(
				Op,
				NativeProcedure,
				Self,
				Arguments,
				Dest,
				NextPC,
				IncomingEffectToken,
				CurrentBatchedRefs);
		}
		else
		{
			VERSE_UNREACHABLE();
		}
	}

	template <typename OpType, typename ArgumentType, typename DestOperandType>
	FOpResult CallNativeFunction(
		OpType& Op,
		VNativeProcedure* NativeProcedure,
		VValue Self,
		TArrayView<ArgumentType> Arguments,
		DestOperandType Dest,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs)
	{
		REQUIRE_CONCRETE(Self);

		VFunction::Args Args;
		Args.AddUninitialized(NativeProcedure->NumPositionalParameters);
		CopyPositionalArguments(
			Context, NativeProcedure->NumPositionalParameters, Arguments.Num(),
			[&](uint32 Arg) { return GetOperand(Arguments[Arg]); },
			[&](uint32 Param, VValue Value) { Args[Param] = Value; });

		VValue OpEffectToken = IncomingEffectToken;

		// We do not REQUIRE_CONCRETE(IncomingEffectToken) here- the native function glue does that only when necessary.
		FNativeCallResult Result = Context.PushNativeFrame(
			FailureContext(),
			Task,
			CurrentBatchedRefs,
			IncomingEffectToken,
			NativeProcedure,
			State.PC,
			State.Frame,
			[&] {
				Context.CheckForHandshake([&] {
					if (FSamplingProfiler* Sampler = GetRunningSamplingProfiler())
					{
						bool bExpected = true;
						if (Sampler->SampleRequested.compare_exchange_weak(bExpected, false, std::memory_order_relaxed))
						{
							// We have sample here to know when we are in a native func
							Sampler->Sample(Context, nullptr, nullptr, Task);
						}
					}
				});
				return (*NativeProcedure->Thunk)(Context, Self, Args);
			});

		// We do not currently support arbitrary native functions consuming the effect token.
		V_DIE_UNLESS(IncomingEffectToken == OpEffectToken);

		// TODO: <converges> calls do not have to consume the effect token when suspending.
		OP_RESULT_HELPER(Result);
		DEF(Dest, Result.Value);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult CallImpl(OpType& Op, VValue Callee)
	{
		// Handles FOpCall for all non-function calls
		check(!Callee.IsPlaceholder());

		auto Arguments = GetOperands(Op.Arguments);
		V_DIE_UNLESS(Arguments.Num() == 1);

		VValue Argument = GetOperand(Arguments[0]);
		if (VAccessorRef* AccessorRef = Callee.DynamicCast<VAccessorRef>())
		{
			VAccessorRef& NewAccessorRef = VAccessorRef::New(Context, *AccessorRef, Argument);
			DEF(Op.Dest, NewAccessorRef);
		}
		else if (VNativeRef* NativeRef = Callee.DynamicCast<VNativeRef>())
		{
			REQUIRE_CONCRETE(Argument);
			FOpResult Result = NativeRef->Call(Context, Argument, /*bSet*/ false);
			OP_RESULT_HELPER(Result);
			DEF(Op.Dest, Result.Value);
		}
		else if (VArray* Array = Callee.DynamicCast<VArray>())
		{
			REQUIRE_CONCRETE(Argument);
			// Bounds check since this index access in Verse is fallible.
			if (Argument.IsUint32() && Array->IsInBounds(Argument.AsUint32()))
			{
				DEF(Op.Dest, Array->GetValue(Argument.AsUint32()));
			}
			else
			{
				FAIL();
			}
		}
		else if (VMutableArray* MutableArray = Callee.DynamicCast<VMutableArray>())
		{
			REQUIRE_CONCRETE(Argument);
			// Bounds check since this index access in Verse is fallible.
			if (!Argument.IsUint32())
			{
				FAIL();
			}
			uint32 Index = Argument.AsUint32();
			if (!MutableArray->IsInBounds(Index))
			{
				FAIL();
			}
			VValue Element = UnwrapTransparentRef(Context, MutableArray->GetValue(Index), Task, [&](VValue Element) {
				AUTORTFM_SANITIZER_DISABLE_SCOPE();
				MutableArray->SetValue(Context, Index, Element);
			});
			DEF(Op.Dest, Element);
		}
		else if (VMap* Map = Callee.DynamicCast<VMap>())
		{
			// TODO SOL-5621: We need to ensure the entire Key structure is concrete, not just the top-level.
			REQUIRE_CONCRETE(Argument);
			if (VValue Result = Map->Find(Context, Argument))
			{
				DEF(Op.Dest, Result);
			}
			else
			{
				FAIL();
			}
		}
		else if (VMutableMap* MutableMap = Callee.DynamicCast<VMutableMap>())
		{
			// TODO SOL-5621: We need to ensure the entire Key structure is concrete, not just the top-level.
			REQUIRE_CONCRETE(Argument);

			if (Callee.IsCellOfType<VPersistentMap>() && !VerseVM::GetEngineEnvironment()->IsValidWeakMapKey(Argument))
			{
				RAISE_VERSE_RUNTIME_ERROR_CODE(ERuntimeDiagnostic::ErrRuntime_WeakMapInvalidKey);
				return {FOpResult::Error};
			}

			VMutableMap::SequenceType Slot;
			VValue WrappedValue = MutableMap->FindWithSlot(Context, Argument, &Slot);
			if (!WrappedValue)
			{
				FAIL();
			}
			VValue Value = UnwrapTransparentRef(Context, WrappedValue, Task, [&](VValue Value) {
				AUTORTFM_SANITIZER_DISABLE_SCOPE();
				MutableMap->GetPairTable()[Slot].Value.Set(Context, Value);
			});
			DEF(Op.Dest, Value);
		}
		else if (VType* Type = Callee.DynamicCast<VType>())
		{
			REQUIRE_CONCRETE(Argument);
			if (Type->Subsumes(Context, Argument))
			{
				DEF(Op.Dest, Argument);
			}
			else
			{
				FAIL();
			}
		}
		else
		{
			V_DIE("Unknown callee");
		}

		return {FOpResult::Return};
	}

	// Caution: This function may return with a different frame, so register to
	// value lookups must be performed before calling.
	template <typename OpType>
	FOpResult CallGetter(
		OpType& Op,
		VAccessorRef& AccessorRef,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs)
	{
		auto& Dest = Op.Dest;
		TArray<TWriteBarrier<VValue>> Arguments;
		VAccessorRef& Field = AccessorRef.Flatten(Context, Arguments, 0);
		VAccessor& Accessor = Field.Member.Get().StaticCast<VAccessor>();
		VUniqueString& FieldName = *Accessor.FindGetter(Arguments.Num());
		return CallAccessor(Op, Dest, Field.Base.Follow(), FieldName, Arguments, NextPC, IncomingEffectToken, CurrentBatchedRefs);
	}

	// Caution: This function may return with a different frame, so register to
	// value lookups must be performed before calling.
	template <typename OpType>
	FOpResult CallSetter(
		OpType& Op,
		VAccessorRef& AccessorRef,
		VValue Value,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs)
	{
		VRestValue* Dest = nullptr;
		TArray<TWriteBarrier<VValue>> Arguments;
		VAccessorRef& Field = AccessorRef.Flatten(Context, Arguments, 1);
		Arguments.Emplace(Context, Value);
		VAccessor& Accessor = Field.Member.Get().StaticCast<VAccessor>();
		VUniqueString& FieldName = *Accessor.FindSetter(Arguments.Num());
		return CallAccessor(Op, Dest, Field.Base.Follow(), FieldName, Arguments, NextPC, IncomingEffectToken, CurrentBatchedRefs);
	}

	// Caution: This function may return with a different frame, so register to
	// value lookups must be performed before calling.
	template <typename OpType, typename DestOperandType>
	FOpResult CallAccessor(
		OpType& Op,
		DestOperandType Dest,
		VValue Self,
		VUniqueString& FieldName,
		TArrayView<TWriteBarrier<VValue>> Arguments,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs)
	{
		REQUIRE_CONCRETE(Self);

		VValue Callee;
		if (VValueObject* Object = Self.DynamicCast<VValueObject>())
		{
			VEmergentType* EmergentType = Object->GetEmergentType();
			const VShape::VEntry* Field = EmergentType->Shape->GetField(FieldName);
			V_DIE_UNLESS(Field && Field->Type == EFieldType::Constant);
			Callee = Field->Value.Get();
		}
		else if (UObject* UEObject = Self.ExtractUObject())
		{
			VShape& Shape = UVerseClass::GetShapeForLoadField(Context, UEObject->GetClass());
			const VShape::VEntry* Field = Shape.GetField(FieldName);
			V_DIE_UNLESS(Field && Field->Type == EFieldType::Constant);
			Callee = Field->Value.Get();
		}
		V_DIE_IF(Callee.IsUninitialized());

		VFunction& Function = Callee.StaticCast<VFunction>();
		return CallFunction(
			Op,
			Function,
			Self,
			Arguments,
			TArrayView<TWriteBarrier<VUniqueString>>{},
			TArrayView<TWriteBarrier<VValue>>{},
			Dest,
			NextPC,
			IncomingEffectToken,
			CurrentBatchedRefs);
	}

	template <typename OpType>
	FOpResult NewArrayImpl(OpType& Op)
	{
		auto Values = GetOperands(Op.Values);
		const uint32 NumValues = Values.Num();
		VArray& NewArray = VArray::New(Context, NumValues, [this, &Values](uint32 Index) { return GetOperand(Values[Index]); });
		DEF(Op.Dest, NewArray);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NewMutableArrayImpl(OpType& Op)
	{
		auto Values = GetOperands(Op.Values);
		const uint32 NumValues = Values.Num();
		VMutableArray& NewArray = VMutableArray::New(Context, NumValues, [this, &Values](uint32 Index) { return GetOperand(Values[Index]); });
		DEF(Op.Dest, NewArray);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NewMutableArrayWithCapacityImpl(OpType& Op)
	{
		const VValue Size = GetOperand(Op.Size);
		REQUIRE_CONCRETE(Size); // Must be an Int32 (although UInt32 is better)
		// TODO: We should kill this opcode until we actually have a use for it.
		// Allocating this with None array type means we're not actually reserving a
		// capacity. The way to do this right in the future is to use profiling to
		// guide what array type we pick. This opcode is currently only being
		// used in our bytecode tests.
		DEF(Op.Dest, VMutableArray::New(Context, 0, static_cast<uint32>(Size.AsInt32()), EArrayType::None));

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult ArrayAddImpl(OpType& Op)
	{
		const VValue Container = GetOperand(Op.Container);
		const VValue ValueToAdd = GetOperand(Op.ValueToAdd);
		REQUIRE_CONCRETE(Container);
		if (VMutableArray* Array = Container.DynamicCast<VMutableArray>())
		{
			Array->AddValue(Context, ValueToAdd);
		}
		else
		{
			V_DIE("Unimplemented type passed to VM `ArrayAdd` operation!");
		}
		DEF(Op.Dest, Container);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult InPlaceMakeImmutableImpl(OpType& Op)
	{
		const VValue Container = GetOperand(Op.Container);
		REQUIRE_CONCRETE(Container);
		if (VMutableArray* MutableArray = Container.DynamicCast<VMutableArray>())
		{
			GetTrailFor(Op).LogEmergentTypeBeforeWrite(Context, MutableArray);
			MutableArray->InPlaceMakeImmutable(Context);
			checkSlow(Container.IsCellOfType<VArray>() && !Container.IsCellOfType<VMutableArray>());
		}
		else
		{
			V_DIE("Unimplemented type passed to VM `InPlaceMakeImmutable` operation!");
		}
		DEF(Op.Dest, Container);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NewOptionImpl(OpType& Op)
	{
		VValue Value = GetOperand(Op.Value);

		DEF(Op.Dest, VOption::New(Context, Value));

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NewMapImpl(OpType& Op)
	{
		auto Keys = GetOperands(Op.Keys);
		auto Values = GetOperands(Op.Values);

		const uint32 NumKeys = Keys.Num();
		V_DIE_UNLESS(NumKeys == static_cast<uint32>(Values.Num()));

		// TODO(SOL-5621): Keys must be deeply concrete.
		for (int32 Index = 0; Index < NumKeys; ++Index)
		{
			REQUIRE_CONCRETE(GetOperand(Keys[Index]));
		}

		VMapBase& NewMap = VMapBase::New<VMap>(Context, NumKeys, [this, &Keys, &Values](uint32 Index) {
			return TPair<VValue, VValue>(GetOperand(Keys[Index]), GetOperand(Values[Index]));
		});

		DEF(Op.Dest, NewMap);

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NewClassImpl(OpType& Op)
	{
		auto AttrIndices = GetOperands(Op.AttributeIndices);
		auto Attrs = GetOperands(Op.Attributes);
		VArray* AttrIndicesValue = nullptr;
		VArray* AttrsValue = nullptr;
		if (Attrs.Num() > 0)
		{
			AttrIndicesValue = &VArray::New(Context, AttrIndices.Num(), [this, &AttrIndices](uint32 Index) {
				return AttrIndices[Index].Get();
			});
			AttrsValue = &VArray::New(Context, Attrs.Num(), [this, &Attrs](uint32 Index) {
				return GetOperand(Attrs[Index]);
			});
		}
		VValue ImportStruct = GetOperand(Op.ImportStruct);

		bool bNativeRepresentation = Op.bNativeBound;
		bool bPredicts = false;

		auto Inherited = GetOperands(Op.Inherited);
		TArray<VClass*> InheritedClasses;
		int32 NumInherited = Inherited.Num();
		InheritedClasses.Reserve(NumInherited);
		for (int32 Index = 0; Index < NumInherited; ++Index)
		{
			const VValue CurrentArg = GetOperand(Inherited[Index]);
			REQUIRE_CONCRETE(CurrentArg);
			VClass& Class = CurrentArg.StaticCast<VClass>();
			bNativeRepresentation |= Class.IsNativeRepresentation();
			bPredicts |= Class.IsPredicts();
			InheritedClasses.Add(&Class);
		}

		for (int32 Index = 0; Index < Op.Archetype->NumEntries; ++Index)
		{
			VArchetype::VEntry& Entry = Op.Archetype->Entries[Index];
			bNativeRepresentation |= EnumHasAnyFlags(Entry.Flags, EArchetypeEntryFlags::NativeRepresentation);
			bPredicts |= EnumHasAnyFlags(Entry.Flags, EArchetypeEntryFlags::Predicts);
		}

		VClass::EFlags Flags = Op.Flags;
		if (bNativeRepresentation || bPredicts)
		{
			EnumAddFlags(Flags, VClass::EFlags::NativeRepresentation);
		}
		if (bPredicts)
		{
			EnumAddFlags(Flags, VClass::EFlags::Predicts);
		}

		// We're doing this because the placeholder during codegen time isn't yet concrete.
		REQUIRE_CONCRETE(Op.Archetype->NextArchetype.Get(Context));
		VClass& NewClass = VClass::New(
			Context,
			Op.Package.Get(),
			Op.RelativePath.Get(),
			Op.ClassName.Get(),
			AttrIndicesValue,
			AttrsValue,
			ImportStruct,
			Op.bNativeBound,
			Op.ClassKind,
			Flags,
			InheritedClasses,
			*Op.Archetype,
			*Op.ConstructorBody,
			Op.Blocks.Get());

		DEF(Op.ClassDest, NewClass);
		DEF(Op.ArchetypeDest, NewClass.GetArchetype());
		DEF(Op.ConstructorDest, NewClass.GetConstructor());
		if (!CVarUObjectLeniency.GetValueOnAnyThread())
		{
			if (Op.ClassKind == VClass::EKind::Class)
			{
				DEF(Op.BlocksDest, NewClass.GetBlocks());
			}
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult BindNativeClassImpl(OpType& Op)
	{
		VValue ClassValue = GetOperand(Op.Class);
		REQUIRE_CONCRETE(ClassValue);

		VClass& Class = ClassValue.StaticCast<VClass>();

		if (Class.Attributes)
		{
			const uint32 NumAttributes = Class.Attributes->Num();
			for (uint32 Index = 0; Index < NumAttributes; ++Index)
			{
				VValue AttributeValue = Class.Attributes->GetValue(Index);
				REQUIRE_CONCRETE(AttributeValue);
			}
		}

		FOpResult Result = RequireConcreteClassLayout(Class);
		if (!Result.IsReturn())
		{
			return Result;
		}

		// TODO: Native functions should require this shape (and by proxy the UClass) to be concrete before being called.
		REQUIRE_CONCRETE(Class.NativeType.Follow());
		UStruct* Struct = Class.GetOrCreateNativeType(Context);
		FOpResult ShapeResult = Class.BindNativeClass(Context, Op.bImported);
		if (!ShapeResult.IsReturn())
		{
			return ShapeResult;
		}

		VRestValue* StructShape = nullptr;
		if (UVerseClass* VerseClass = Cast<UVerseClass>(Struct))
		{
			StructShape = &VerseClass->Shape;
		}
		else if (UVerseStruct* VerseStruct = Cast<UVerseStruct>(Struct))
		{
			StructShape = &VerseStruct->Shape;
		}

		if (StructShape)
		{
			VShape& Shape = ShapeResult.Value.StaticCast<VShape>();
			ECompares Compares = DefTrailed(GetTrailFor(Op), *StructShape, Shape);
			V_DIE_UNLESS(Compares == ECompares::Eq);

			if (GUObjectArray.IsDisregardForGC(Struct))
			{
				// Placeholders between the field and the shape will not be traced- just bypass them now that the shape is concrete.
				StructShape->SetTrailed(Context, GetTrailFor(Op), Shape);
				Shape.AddRef(Context);
			}
		}

		return {FOpResult::Return};
	}

	FOpResult RequireConcreteClassLayout(VClass& Class)
	{
		auto Inherited = Class.GetInherited();
		if (Inherited.Num() > 0 && Inherited[0]->GetKind() == VClass::EKind::Class)
		{
			REQUIRE_CONCRETE(Inherited[0]->NativeType.Follow());
			UStruct* Super = Inherited[0]->GetOrCreateNativeType(Context);
			if (UVerseClass* VerseSuper = Cast<UVerseClass>(Super))
			{
				REQUIRE_CONCRETE(VerseSuper->Shape.Get(Context));
			}
		}

		const uint32 NumArchetypeEntries = Class.GetArchetype().NumEntries;
		for (uint32 Index = 0; Index < NumArchetypeEntries; ++Index)
		{
			VArchetype::VEntry& Entry = Class.GetArchetype().Entries[Index];
			if (VValue FieldType = Entry.Type.Follow(); !FieldType.IsUninitialized())
			{
				FOpResult Result = RequireConcreteFieldLayout(Entry.IsNativeRepresentation(), Entry.Type.Follow());
				if (!Result.IsReturn())
				{
					return Result;
				}
			}
		}

		return {FOpResult::Return};
	}

	FOpResult RequireConcreteFieldLayout(bool bNativeRepresentation, VValue Type)
	{
		REQUIRE_CONCRETE(Type);
		if (VTypeType* TypeType = Type.DynamicCast<VTypeType>())
		{
			return RequireConcreteFieldLayout(bNativeRepresentation, TypeType->PositiveType.Follow());
		}
		else if (VClass* ClassType = Type.DynamicCast<VClass>())
		{
			if (bNativeRepresentation)
			{
				REQUIRE_CONCRETE(ClassType->NativeType.Follow());
			}
			if (ClassType->IsStruct())
			{
				REQUIRE_CONCRETE(ClassType->NativeType.Follow());
				UStruct* Struct = ClassType->GetOrCreateNativeType(Context);
				if (UVerseStruct* VerseStruct = Cast<UVerseStruct>(Struct))
				{
					REQUIRE_CONCRETE(VerseStruct->Shape.Get(Context));
				}
			}
		}
		else if (VConcreteType* ConcreteType = Type.DynamicCast<VConcreteType>())
		{
			return RequireConcreteFieldLayout(bNativeRepresentation, ConcreteType->SuperType.Follow());
		}
		else if (VCastableType* CastableType = Type.DynamicCast<VCastableType>())
		{
			return RequireConcreteFieldLayout(bNativeRepresentation, CastableType->SuperType.Follow());
		}
		else if (VArrayType* ArrayType = Type.DynamicCast<VArrayType>())
		{
			return RequireConcreteFieldLayout(bNativeRepresentation, ArrayType->ElementType.Follow());
		}
		else if (VGeneratorType* GeneratorType = Type.DynamicCast<VGeneratorType>())
		{
			return RequireConcreteFieldLayout(bNativeRepresentation, GeneratorType->ElementType.Follow());
		}
		else if (VMapType* MapType = Type.DynamicCast<VMapType>())
		{
			FOpResult KeyResult = RequireConcreteFieldLayout(bNativeRepresentation, MapType->KeyType.Follow());
			if (!KeyResult.IsReturn())
			{
				return KeyResult;
			}
			FOpResult ValueResult = RequireConcreteFieldLayout(bNativeRepresentation, MapType->ValueType.Follow());
			if (!ValueResult.IsReturn())
			{
				return ValueResult;
			}
		}
		else if (VPointerType* PointerType = Type.DynamicCast<VPointerType>())
		{
			return RequireConcreteFieldLayout(bNativeRepresentation, PointerType->ValueType.Follow());
		}
		else if (VOptionType* OptionType = Type.DynamicCast<VOptionType>())
		{
			return RequireConcreteFieldLayout(bNativeRepresentation, OptionType->ValueType.Follow());
		}
		else if (VTupleType* Tuple = Type.DynamicCast<VTupleType>())
		{
			for (TWriteBarrier<VValue>& Element : Tuple->GetElements())
			{
				FOpResult Result = RequireConcreteFieldLayout(bNativeRepresentation, Element.Follow());
				if (!Result.IsReturn())
				{
					return Result;
				}
			}
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult EndModuleDataImpl(OpType& Op)
	{
		const VValue Operand = GetOperand(Op.Value);

		// TODO: this REQUIRE_CONCRETE call is too shallow; it'll miss nonconcrete subexpressions in
		//       Operand
		REQUIRE_CONCRETE(Operand);

		UObject* Module = FInstantiationScope::Context.Outer;
		VUniqueString& FieldName = *Op.FieldName.Get();

#if WITH_EDITORONLY_DATA
		UVerseClass::TrackDefaultInitializedPropertiesOfGlobalField(Context, Module, Operand);
#endif

		UVerseClass::RenameSubobjectsOfGlobalField(Context, Module, FString(FieldName.AsStringView()), Operand);

		return FOpResult{FOpResult::Return};
	}

	template <typename OpType>
	FOpResult LoadConstructorImpl(OpType& Op)
	{
		VValue ClassOperand = GetOperand(Op.Class);
		REQUIRE_CONCRETE(ClassOperand);
		VClass& Class = ClassOperand.StaticCast<VClass>();
		DEF(Op.Dest, Class.GetConstructor());
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NewObjectImpl(OpType& Op, FOp*& NextPC, VValue& IncomingEffectToken, VBatchedRefs* CurrentBatchedRefs)
	{
		VValue ArchetypeOperand = GetOperand(Op.Archetype);
		REQUIRE_CONCRETE(ArchetypeOperand);
		VArchetype& Archetype = ArchetypeOperand.StaticCast<VArchetype>();

		VValue ClassOperand = GetOperand(Op.Class);
		REQUIRE_CONCRETE(ClassOperand);
		VClass& Class = ClassOperand.StaticCast<VClass>();

		// TODO: (yiliang.siew) We also need the delegating archetype to be concrete here, but we'll get
		// into a suspension loop if we do so because the class isn't yet concrete.
		REQUIRE_CONCRETE(Archetype.NextArchetype.Get(Context));

		// UObject/VNativeStruct or VObject?
		bool bNativeRepresentation = Class.IsNativeRepresentation();
		if (!Class.IsStruct())
		{
			// CDO subobjects use traditional UE object construction, which also requires they be UObjects.
			bNativeRepresentation |= EnumHasAnyFlags(FInstantiationScope::Context.Flags, RF_DefaultSubObject);

			if (!EnumHasAnyFlags(Class.Flags, VClass::EFlags::CustomAttribute))
			{
				// Module-toplevel objects (aka. "globals") are represented as UObjects
				bNativeRepresentation |= FInstantiationScope::Context.bModuleTopLevel;

				// Debugging functionality. This lets us test that both paths work as expected and not just with the smaller
				// subset of code that uses native Verse interop.
				if (!bNativeRepresentation)
				{
					const float UObjectProbability = CVarUObjectProbability.GetValueOnAnyThread();
					bNativeRepresentation = UObjectProbability > 0.0f && (UObjectProbability > RandomUObjectProbability.FRand());
				}
			}
		}

		if constexpr (std::is_same_v<OpType, FOpNewObjectICClass>)
		{
			// If we fail, take the slow path and swap the cache (LRU).
			if (Op.CachedClass == &Class && !bNativeRepresentation)
			{
				VValue NewObject = Class.NewVObjectOfEmergentType(Context, *FHeap::EmergentTypeOffsetToPtr(Op.EmergentTypeOffset));
				DEF(Op.Dest, NewObject);
				return {FOpResult::Return};
			}
		}

		VValue NewObject;
		if (bNativeRepresentation)
		{
			if (Class.IsStruct())
			{
				REQUIRE_CONCRETE(Class.NativeType.Follow());
				if (UVerseStruct* VerseStruct = Cast<UVerseStruct>(Class.GetOrCreateNativeType(Context)))
				{
					REQUIRE_CONCRETE(VerseStruct->Shape.Get(Context));
				}

				NewObject = Class.NewNativeStruct(Context);
			}
			else
			{
				REQUIRE_CONCRETE(Class.NativeType.Follow());
				UStruct* Struct = Class.GetOrCreateNativeType(Context);
				if (UVerseClass* VerseClass = Cast<UVerseClass>(Struct))
				{
					REQUIRE_CONCRETE(VerseClass->Shape.Get(Context));
				}

				if (!verse::CanAllocateUObjects())
				{
					RAISE_RUNTIME_ERROR_FORMAT(Context, ERuntimeDiagnostic::ErrRuntime_MemoryLimitExceeded, TEXT("Ran out of memory for allocating `UObject`s while attempting to construct a Verse object of type %s!"), *Class.GetBaseName().AsString());
					return {FOpResult::Error};
				}

				// TODO: Consider eliding this FailureContext() when the object can be allocated entirely in the open.
				FOpResult Result = Context.PushNativeFrame(FailureContext(), Task, CurrentBatchedRefs, IncomingEffectToken, /*Callee*/ nullptr, State.PC, State.Frame, [&] {
					return Class.NewUObject(Context, Archetype);
				});
				if (!Result.IsReturn())
				{
					return Result;
				}
				NewObject = Result.Value;
			}
		}
		else
		{
			VEmergentType& EmergentType = Class.GetOrCreateEmergentTypeForVObject(Context, &VValueObject::StaticCppClassInfo, Archetype);
			NewObject = Class.NewVObjectOfEmergentType(Context, EmergentType);
			if constexpr (!std::is_same_v<OpType, FNewObjectSuspensionCaptures> && !std::is_same_v<OpType, FNewObjectICClassSuspensionCaptures>)
			{
				Op.EmergentTypeOffset = FHeap::EmergentTypePtrToOffset(&EmergentType);
				Op.CachedClass = &Class;
				StoreStoreFence();
				Op.Opcode = EOpcode::NewObjectICClass;
			}
		}
		DEF(Op.Dest, NewObject);

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult LoadFieldImpl(OpType& Op)
	{
		VValue ObjectOperand = GetOperand(Op.Object);
		REQUIRE_CONCRETE(ObjectOperand);
		VUniqueString& FieldName = *Op.Name.Get();

		// clang-format off
		if constexpr (
			std::is_same_v<OpType, FOpLoadFieldICOffset> ||
			std::is_same_v<OpType, FOpLoadFieldICConstant> ||
			std::is_same_v<OpType, FOpLoadFieldICFunction> ||
			std::is_same_v<OpType, FOpLoadFieldICAccessor>)
		// clang-format on
		{
			if (ObjectOperand.IsCell())
			{
				VCell& Cell = ObjectOperand.AsCell();
				if (Cell.EmergentTypeOffset == Op.EmergentTypeOffset)
				{
					VValue Result;
					if constexpr (std::is_same_v<OpType, FOpLoadFieldICOffset>)
					{
						VRestValue* RestValue = BitCast<VRestValue*>(BitCast<char*>(&Cell) + Op.ICPayload);
						Result = UnwrapTransparentRef(Context, RestValue->Get(Context), Task, [&](VValue Value) {
							AUTORTFM_SANITIZER_DISABLE_SCOPE();
							RestValue->Set(Context, Value);
						});
					}
					if constexpr (std::is_same_v<OpType, FOpLoadFieldICConstant>)
					{
						Result = VValue::Decode(Op.ICPayload);
					}
					if constexpr (std::is_same_v<OpType, FOpLoadFieldICFunction>)
					{
						Result = BitCast<VFunction*>(Op.ICPayload)->Bind(Context, Cell.StaticCast<VObject>());
					}
					if constexpr (std::is_same_v<OpType, FOpLoadFieldICAccessor>)
					{
						Result = VAccessorRef::New(Context, Cell, *BitCast<VAccessor*>(Op.ICPayload));
					}
					DEF(Op.Dest, Result);
					return {FOpResult::Return};
				}
			}
		}

		VValue Self = ObjectOperand;
		VNativeConstructorWrapper* Wrapper = ObjectOperand.DynamicCast<VNativeConstructorWrapper>();
		if (Wrapper)
		{
			ObjectOperand = Wrapper->WrappedObject();
			Self = Wrapper->SelfPlaceholder.Get(Context);
		}

		if (VAccessorRef* AccessorRef = ObjectOperand.DynamicCast<VAccessorRef>())
		{
			// for deep mutability accessors (such as an array of structs)
			//   SetAS(:accessor, J:int, Member:string, K:int, Value:int)<transacts><decides>:void
			//   set T.AS[0].AI[0] = 666 <-- we are grabbing `accessor, 0, '<verse_path_to>AI', 0, 666`
			// we must trim the verse-path from our field name
			VUniqueString& Field = VUniqueString::New(Context, Verse::Names::RemoveQualifier(FieldName.AsStringView()));
			VAccessorRef& NewAccessorRef = VAccessorRef::New(Context, *AccessorRef, Field);
			DEF(Op.Dest, NewAccessorRef);
			return {FOpResult::Return};
		}
		else if (VNativeRef* NativeRef = ObjectOperand.DynamicCast<VNativeRef>())
		{
			FOpResult Result = NativeRef->LoadField(Context, FieldName);
			OP_RESULT_HELPER(Result);
			DEF(Op.Dest, Result.Value);
			return {FOpResult::Return};
		}
		else if (VObject* Object = ObjectOperand.DynamicCast<VObject>())
		{
			VEmergentType* EmergentType = Object->GetEmergentType();
			VShape& Shape = *EmergentType->Shape;
			int32 FieldIndex = Shape.GetFieldIndex(FieldName);
			const VShape::VEntry& Field = Shape.GetField(FieldIndex);
			if (Wrapper)
			{
				if (VValue Placeholder = Wrapper->LoadField(Context, FieldIndex))
				{
					DEF(Op.Dest, Placeholder);
					return {FOpResult::Return};
				}
			}

			FLoadFieldCacheCase CacheCase;
			FOpResult FieldResult = Object->LoadField(Context, *EmergentType, &Field, Self, &CacheCase);
			if (!FieldResult.IsReturn())
			{
				V_DIE_UNLESS(FieldResult.IsError());
				return {FOpResult::Error};
			}

			if constexpr (std::is_same_v<OpType, FOpLoadField>)
			{
				if (CacheCase)
				{
					Op.EmergentTypeOffset = CacheCase.EmergentTypeOffset;
					EOpcode NewOpcode;
					switch (CacheCase.Kind)
					{
						case FLoadFieldCacheCase::EKind::Offset:
							Op.ICPayload = CacheCase.U.Offset;
							NewOpcode = EOpcode::LoadFieldICOffset;
							break;
						case FLoadFieldCacheCase::EKind::ConstantValue:
							Op.ICPayload = CacheCase.U.Value.Encode();
							NewOpcode = EOpcode::LoadFieldICConstant;
							break;
						case FLoadFieldCacheCase::EKind::ConstantFunction:
							Op.ICPayload = BitCast<uint64>(CacheCase.U.Function);
							NewOpcode = EOpcode::LoadFieldICFunction;
							break;
						case FLoadFieldCacheCase::EKind::Accessor:
							Op.ICPayload = BitCast<uint64>(CacheCase.U.Accessor);
							NewOpcode = EOpcode::LoadFieldICAccessor;
							break;
						default:
							VERSE_UNREACHABLE();
					}
					StoreStoreFence();
					Op.Opcode = NewOpcode;
				}
			}

			VValue Result = FieldResult.Value;
			if (Field.Type != EFieldType::Constant)
			{
				Result = UnwrapTransparentRef(Context, Result, Task, [&](VValue Value) {
					AUTORTFM_SANITIZER_DISABLE_SCOPE();
					Object->SetField(Context, FieldName, Value);
				});
			}

			DEF(Op.Dest, Result);
			return {FOpResult::Return};
		}
		else if (UObject* NativeObject = ObjectOperand.ExtractUObject())
		{
			if (Wrapper)
			{
				// No need to use GetShapeForLoadField here, because having a Wrapper means
				// we've constructed the instance from Verse.
				VShape* Shape = UVerseClass::GetShape(Context, NativeObject->GetClass());
				int32 FieldIndex = Shape->GetFieldIndex(FieldName);
				if (VValue Placeholder = Wrapper->LoadField(Context, FieldIndex))
				{
					DEF(Op.Dest, Placeholder);
					return {FOpResult::Return};
				}
			}

			FOpResult FieldResult = UVerseClass::LoadField(Context, NativeObject, FieldName, Self);
			if (FieldResult.IsReturn())
			{
				DEF(Op.Dest, FieldResult.Value);
				return {FOpResult::Return};
			}
			else
			{
				V_DIE_UNLESS(FieldResult.IsError());
				return {FOpResult::Error};
			}
		}

		V_DIE("Unsupported operand to a `LoadField` operation when loading: %s!", *FieldName.AsString());
	}

	template <typename OpType>
	FOpResult LoadFieldFromSuperImpl(OpType& Op)
	{
		const VValue ScopeOperand = GetOperand(Op.Scope);
		REQUIRE_CONCRETE(ScopeOperand);

		const VValue SelfOperand = GetOperand(Op.Self);
		REQUIRE_CONCRETE(SelfOperand);

		VUniqueString& FieldName = *Op.Name.Get();

		// Currently, we only allow object instances (of classes) to be referred to by `Self`.
		V_DIE_UNLESS(SelfOperand.IsCellOfType<VValueObject>() || SelfOperand.IsUObject());
		if (VValueObject* SelfValueObject = SelfOperand.DynamicCast<VValueObject>())
		{
			V_DIE_IF(SelfValueObject->IsStruct()); // Structs don't support inheritance or methods.
		}

		// NOTE: (yiliang.siew) We need to allocate a new function here for now in order to support passing methods around
		// as first-class values, since the method for each caller can't just be shared as the function from the
		// shape/constructor.
		VScope& Scope = ScopeOperand.StaticCast<VScope>().GetRootScope();
		V_DIE_UNLESS(Scope.NumCaptures == 1);

		VValue ArchetypeValue = Scope.Captures[0].Get();
		REQUIRE_CONCRETE(ArchetypeValue);
		VArchetype& Archetype = ArchetypeValue.StaticCast<VArchetype>();
		VValue ArchetypeClassValue = Archetype.Class.Get(Context);
		REQUIRE_CONCRETE(ArchetypeClassValue);
		VClass& ArchetypeClass = ArchetypeClassValue.StaticCast<VClass>();

		VValue FunctionWithSelf;
		int32 BaseIndex{0};
		VClass::WalkArchetypeFields(Context,
			Archetype,
			ArchetypeClass,
			Archetype.NextArchetype.Get(Context).DynamicCast<Verse::VArchetype>(),
			BaseIndex,
			{},
			[&](VArchetype::VEntry& Entry, int32, VClass& Class) {
				if (&Class == &ArchetypeClass)
				{
					return;
				}

				if (!FunctionWithSelf)
				{
					// TODO: we'll probably want to teach
					//       `WalkArchetypeFields` how to stop iterating
					FunctionWithSelf = Class.Archetype->LoadFunction(Context, FieldName, SelfOperand);
				}
			});

		V_DIE_UNLESS(FunctionWithSelf);
		DEF(Op.Dest, FunctionWithSelf);

		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NewScopeImpl(OpType& Op)
	{
		VValue ParentScopeValue = GetOperand(Op.ParentScope);
		VScope* ParentScope = ParentScopeValue.DynamicCast<VScope>();
		auto Captures = GetOperands(Op.Captures);
		VScope& Scope = VScope::New(Context, ParentScope, Captures.Num());
		auto J = Scope.Captures;
		for (auto I = Captures.begin(), Last = Captures.end(); I != Last; ++I, ++J)
		{
			J->Set(Context, GetOperand(*I));
		}
		DEF(Op.Dest, Scope);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult NewFunctionImpl(OpType& Op)
	{
		VValue ProcedureValue = GetOperand(Op.Procedure);
		REQUIRE_CONCRETE(ProcedureValue);
		VProcedure& Procedure = ProcedureValue.StaticCast<VProcedure>();
		VValue ParentScopeValue = GetOperand(Op.ParentScope);
		REQUIRE_CONCRETE(ParentScopeValue);
		VScope* ParentScope = ParentScopeValue.DynamicCast<VScope>();
		VFunction& Function = VFunction::New(Context, Procedure, GetOperand(Op.Self), ParentScope);
		DEF(Op.Dest, Function);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult LoadParentScopeImpl(OpType& Op)
	{
		VValue ScopeValue = GetOperand(Op.Scope);
		REQUIRE_CONCRETE(ScopeValue);
		VScope& Scope = ScopeValue.StaticCast<VScope>();
		V_DIE_UNLESS(Scope.ParentScope);
		DEF(Op.Dest, *Scope.ParentScope);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult LoadCaptureImpl(OpType& Op)
	{
		VValue ScopeValue = GetOperand(Op.Scope);
		REQUIRE_CONCRETE(ScopeValue);
		VScope& Scope = ScopeValue.StaticCast<VScope>();
		checkSlow(Op.Index < Scope.NumCaptures);
		DEF(Op.Dest, Scope.Captures[Op.Index].Get());
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult UnifyFieldImpl(
		OpType& Op,
		FOp*& NextPC,
		const VValue& IncomingEffectToken,
		EExecutionLoopKind ExecutionLoopKind)
	{
		if (ExecutionLoopKind == EExecutionLoopKind::Main && !CVarUObjectLeniency.GetValueOnAnyThread())
		{
			BumpEffectEpoch();
			REQUIRE_CONCRETE(IncomingEffectToken);
		}

		VValue ObjectOperand = GetOperand(Op.Object);
		REQUIRE_CONCRETE(ObjectOperand);
		VUniqueString& FieldName = *Op.Name.Get();
		VValue ValueOperand = GetOperand(Op.Value);

		VNativeConstructorWrapper* Wrapper = ObjectOperand.DynamicCast<VNativeConstructorWrapper>();
		if (Wrapper)
		{
			ObjectOperand = Wrapper->WrappedObject();
		}

		if (VObject* Object = ObjectOperand.DynamicCast<VObject>())
		{
			const VEmergentType* EmergentType = Object->GetEmergentType();
			VShape* Shape = EmergentType->Shape.Get();
			int32 FieldIndex = Shape->GetFieldIndex(FieldName);
			const VShape::VEntry& Field = Shape->GetField(FieldIndex);
			switch (Field.Type)
			{
				case EFieldType::Offset:
					checkSlow(Object->IsA<VValueObject>());
					DEF(Object->GetFieldData(*EmergentType->CppClassInfo)[Field.Index], ValueOperand);
					break;

				// NOTE: VNativeRef::Set only makes sense here because UnifyField is only used for initialization.
				case EFieldType::FProperty:
				{
					checkSlow(Wrapper && Object->IsA<VNativeStruct>());
					void* ObjectData = Object->GetData(*EmergentType->CppClassInfo);
					FOpResult Result = CVarUObjectLeniency.GetValueOnAnyThread()
										 ? VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, ObjectData, Field.UProperty, ValueOperand)
										 : VNativeRef::Set<EWriteMode::Transactional>(Context, nullptr, ObjectData, Field.UProperty, ValueOperand);
					OP_RESULT_HELPER(Result);
					if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
					{
						DEF(Placeholder, ValueOperand);
					}
					break;
				}

				case EFieldType::FVerseProperty:
				{
					checkSlow(Wrapper && Object->IsA<VNativeStruct>());
					VRestValue& Slot = *Field.UProperty->ContainerPtrToValuePtr<VRestValue>(Object->GetData(*EmergentType->CppClassInfo));
					if (CVarUObjectLeniency.GetValueOnAnyThread())
					{
						DEF(Slot, ValueOperand);
					}
					else
					{
						Slot.SetTransactionally(Context, ValueOperand);
					}
					if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
					{
						DEF(Placeholder, ValueOperand);
					}
					break;
				}

				case EFieldType::FPropertyVar:
				case EFieldType::Constant:
					V_DIE("Field %s has an unsupported type; cannot unify!", *FieldName.AsString());
					break;
			}
		}
		else if (UObject* NativeObject = ObjectOperand.ExtractUObject())
		{
			UVerseClass* Class = CastChecked<UVerseClass>(NativeObject->GetClass());
			VShape& Shape = Class->Shape.Get(Context).StaticCast<VShape>();
			int32 FieldIndex = Shape.GetFieldIndex(FieldName);
			const VShape::VEntry& Field = Shape.GetField(FieldIndex);
			switch (Field.Type)
			{
				// NOTE: VNativeRef::Set only makes sense here because UnifyField is only used for initialization.
				case EFieldType::FProperty:
				{
					FOpResult Result = CVarUObjectLeniency.GetValueOnAnyThread()
										 ? VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, NativeObject, Field.UProperty, ValueOperand)
										 : VNativeRef::Set<EWriteMode::Transactional>(Context, nullptr, NativeObject, Field.UProperty, ValueOperand);
					OP_RESULT_HELPER(Result);
					if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
					{
						DEF(Placeholder, ValueOperand);
					}
					break;
				}

				case EFieldType::FVerseProperty:
				{
					VRestValue& Slot = *Field.UProperty->ContainerPtrToValuePtr<VRestValue>(NativeObject);
					if (LIKELY(Slot.CanDefQuickly()) && ValueOperand.IsCell() && GUObjectArray.IsDisregardForGC(NativeObject))
					{
						ValueOperand.AsCell().AddRef(Context);
					}

					if (CVarUObjectLeniency.GetValueOnAnyThread())
					{
						DEF(Slot, ValueOperand);
					}
					else
					{
						V_DIE_UNLESS(Slot.CanDefQuickly());
						Slot.SetTransactionally(Context, ValueOperand);
					}

					if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
					{
						DEF(Placeholder, ValueOperand);
					}
					break;
				}

				case EFieldType::Offset:
				case EFieldType::FPropertyVar:
				case EFieldType::Constant:
					V_DIE("Field: %s has an unsupported type; cannot unify!", *FieldName.AsString());
					break;
			}
		}
		else
		{
			V_DIE("Unsupported object operand to UnifyField");
		}

		if (ExecutionLoopKind == EExecutionLoopKind::Main && !CVarUObjectLeniency.GetValueOnAnyThread())
		{
			DEF(EffectToken, IncomingEffectToken);
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult InitializeVarImpl(
		OpType& Op,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs,
		EExecutionLoopKind ExecutionLoopKind)
	{
		if (ExecutionLoopKind == EExecutionLoopKind::Main && !CVarUObjectLeniency.GetValueOnAnyThread())
		{
			BumpEffectEpoch();
			REQUIRE_CONCRETE(IncomingEffectToken);
		}

		VValue ObjectOperand = GetOperand(Op.Object);
		REQUIRE_CONCRETE(ObjectOperand);
		VUniqueString& FieldName = *Op.Name.Get();
		VValue ValueOperand = GetOperand(Op.Value);
		VValue ValueDomainOperand = GetOperand(Op.ValueDomain);

		VValue Self = ObjectOperand;
		VNativeConstructorWrapper* Wrapper = ObjectOperand.DynamicCast<VNativeConstructorWrapper>();
		if (Wrapper)
		{
			ObjectOperand = Wrapper->WrappedObject();
			Self = Wrapper->SelfPlaceholder.Get(Context);
		}

		if (UNLIKELY(Verse::FInstantiationScope::Context.bModuleTopLevel)
			&& Op.bCheckIfVariableAllocationIsAllowed)
		{
			RAISE_RUNTIME_ERROR_FORMAT(Context,
				ERuntimeDiagnostic::ErrRuntime_UnimplementedGlobalVariable,
				TEXT("Can't allocate mutable var field %s while initializing module."),
				*FieldName.AsString());
			return {FOpResult::Error};
		}

		// Lookup the value for Op.TokenDest early, as CallSetter() may change
		// the frame, resulting in an incorrect register lookup.
		auto& TokenDestValue = ValueForOp(Op.TokenDest);
		VValue TokenOperand = GetOperand(Op.Token);

		if (VObject* Object = ObjectOperand.DynamicCast<VObject>())
		{
			const VEmergentType* EmergentType = Object->GetEmergentType();
			VShape* Shape = EmergentType->Shape.Get();
			int32 FieldIndex = Shape->GetFieldIndex(FieldName);
			const VShape::VEntry& Field = Shape->GetField(FieldIndex);
			switch (Field.Type)
			{
				case EFieldType::Offset:
				{
					checkSlow(Object->IsA<VValueObject>());
					VRef& Var = VRef::New(Context, ValueDomainOperand);
					Var.SetNonTransactionally(Context, ValueOperand);
					DEF(Object->GetFieldData(*EmergentType->CppClassInfo)[Field.Index], Var);
					break;
				}

				case EFieldType::FVerseProperty:
				{
					checkSlow(Wrapper && Object->IsA<VNativeStruct>());
					VRef& Var = VRef::New(Context, ValueDomainOperand);
					Var.SetNonTransactionally(Context, ValueOperand);

					VRestValue& Slot = *Field.UProperty->ContainerPtrToValuePtr<VRestValue>(Object->GetData(*EmergentType->CppClassInfo));
					if (CVarUObjectLeniency.GetValueOnAnyThread())
					{
						DEF(Slot, Var);
					}
					else
					{
						Slot.SetTransactionally(Context, Var);
					}

					if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
					{
						DEF(Placeholder, Var);
					}
					break;
				}

				case EFieldType::Constant:
				{
					VAccessor& Accessor = Field.Value.Get().StaticCast<VAccessor>();
					VAccessorRef& Ref = VAccessorRef::New(Context, Self, Accessor);
					if (CVarUObjectLeniency.GetValueOnAnyThread())
					{
						FOpResult Result = CallSetter(Op, Ref, ValueOperand, NextPC, IncomingEffectToken, CurrentBatchedRefs);
						OP_RESULT_HELPER(Result);
					}
					else
					{
						VAccessorSuspension& Setter = VAccessorSuspension::New(Context, Ref, ValueOperand);
						if (TokenOperand == VValue::CreateFieldMarker())
						{
							TokenOperand = Setter;
						}
						else
						{
							TokenOperand.StaticCast<VAccessorSuspension>().Append(Context, Setter);
						}
					}
					if (Wrapper)
					{
						if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
						{
							DEF(Placeholder, Ref);
						}
					}
					break;
				}

				case EFieldType::FProperty:
				case EFieldType::FPropertyVar:
					V_DIE("Field %s has an unsupported type; cannot initialize!", *FieldName.AsString());
					break;
			}
		}
		else if (UObject* NativeObject = ObjectOperand.ExtractUObject())
		{
			UVerseClass* Class = CastChecked<UVerseClass>(NativeObject->GetClass());
			VShape& Shape = Class->Shape.Get(Context).StaticCast<VShape>();
			int32 FieldIndex = Shape.GetFieldIndex(FieldName);
			const VShape::VEntry& Field = Shape.GetField(FieldIndex);
			switch (Field.Type)
			{
				case EFieldType::FPropertyVar:
				{
					FOpResult Result = CVarUObjectLeniency.GetValueOnAnyThread()
										 ? VNativeRef::Set<EWriteMode::NonTransactional>(Context, nullptr, NativeObject, Field.UProperty, ValueOperand)
										 : VNativeRef::Set<EWriteMode::Transactional>(Context, nullptr, NativeObject, Field.UProperty, ValueOperand);
					OP_RESULT_HELPER(Result);
					if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
					{
						DEF(Placeholder, VNativeRef::New(Context, NativeObject, Field.UProperty));
					}
					break;
				}

				case EFieldType::FVerseProperty:
				{
					VRef& Var = VRef::New(Context, ValueDomainOperand);
					Var.SetNonTransactionally(Context, ValueOperand);
					VRestValue& Slot = *Field.UProperty->ContainerPtrToValuePtr<VRestValue>(NativeObject);
					if (LIKELY(Slot.CanDefQuickly()) && GUObjectArray.IsDisregardForGC(NativeObject))
					{
						Var.AddRef(Context);

						if (CVarUObjectLeniency.GetValueOnAnyThread())
						{
							Slot.Set(Context, Var);
						}
						else
						{
							Slot.SetTransactionally(Context, Var);
						}
					}
					else
					{
						if (CVarUObjectLeniency.GetValueOnAnyThread())
						{
							DEF(Slot, Var);
						}
						else
						{
							V_DIE_UNLESS(Slot.CanDefQuickly());
							Slot.SetTransactionally(Context, Var);
						}
					}
					if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
					{
						DEF(Placeholder, Var);
					}
					break;
				}

				case EFieldType::Constant:
				{
					VAccessor& Accessor = Field.Value.Get().StaticCast<VAccessor>();
					VAccessorRef& Ref = VAccessorRef::New(Context, Self, Accessor);
					if (CVarUObjectLeniency.GetValueOnAnyThread())
					{
						FOpResult Result = CallSetter(Op, Ref, ValueOperand, NextPC, IncomingEffectToken, CurrentBatchedRefs);
						OP_RESULT_HELPER(Result);
					}
					else
					{
						VAccessorSuspension& Setter = VAccessorSuspension::New(Context, Ref, ValueOperand);
						if (TokenOperand == VValue::CreateFieldMarker())
						{
							TokenOperand = Setter;
						}
						else
						{
							TokenOperand.StaticCast<VAccessorSuspension>().Append(Context, Setter);
						}
					}
					if (VValue Placeholder = Wrapper->UnifyField(Context, FieldIndex))
					{
						DEF(Placeholder, Ref);
					}
					break;
				}

				case EFieldType::Offset:
				case EFieldType::FProperty:
					V_DIE("Field %s has an unsupported type; cannot initialize!", *FieldName.AsString());
					break;
			}
		}
		else
		{
			V_DIE("Unsupported object operand to InitializeVar");
		}

		// TODO(SOL-8560): CallSetter() does not call the setter function
		// immediately. Instead, the frame is setup for the call and the PC is
		// set to the setter function. If the setter suspends, then we will
		// define Op.Dest too early, allowing instructions that should wait on
		// Op.TokenDest to run before the setter has complete. We need only define
		// Op.TokenDest once the setter has complete.
		DEF(TokenDestValue, TokenOperand);
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult SetFieldImpl(
		OpType& Op,
		FOp*& NextPC,
		VValue& IncomingEffectToken,
		VBatchedRefs* CurrentBatchedRefs,
		VValue CurrentLiveTask)
	{
		VValue ObjectOperand = GetOperand(Op.Object);
		REQUIRE_CONCRETE(ObjectOperand);
		VValue Value = GetOperand(Op.Value);
		VUniqueString& FieldName = *Op.Name.Get();

		// This is only used for setting into a deeply mutable struct.
		// However, this code should just work for setting fields var
		// fields in a class when we stop boxing those fields in a VRef.
		if (VObject* Object = ObjectOperand.DynamicCast<VObject>())
		{
			REQUIRE_CONCRETE(IncomingEffectToken);

			const VEmergentType* EmergentType = Object->GetEmergentType();
			VShape* Shape = EmergentType->Shape.Get();
			const VShape::VEntry* Field = Shape->GetField(FieldName);
			if (Field->Type == EFieldType::Offset)
			{
				VRestValue& FieldValue = Object->GetFieldData(*EmergentType->CppClassInfo)[Field->Index];
				if (VRef* Ref = FieldValue.GetRaw().ExtractTransparentRef())
				{
					return RefSetImpl(*Ref, Value, IncomingEffectToken, CurrentBatchedRefs, CurrentLiveTask);
				}
				FieldValue.SetTransactionally(Context, Value);
				return FOpResult{FOpResult::Return};
			}
			VNativeStruct& NativeStruct = ObjectOperand.StaticCast<VNativeStruct>();
			if (Field->Type == EFieldType::FProperty)
			{
				FOpResult Result = VNativeRef::Set<EWriteMode::Transactional>(Context, &NativeStruct, NativeStruct.GetData(*EmergentType->CppClassInfo), Field->UProperty, Value);
				OP_RESULT_HELPER(Result);
			}
			else if (Field->Type == EFieldType::FVerseProperty)
			{
				VRestValue* FieldValue = Field->UProperty->ContainerPtrToValuePtr<VRestValue>(Object->GetData(*EmergentType->CppClassInfo));
				if (VRef* Ref = FieldValue->GetRaw().ExtractTransparentRef())
				{
					return RefSetImpl(*Ref, Value, IncomingEffectToken, CurrentBatchedRefs, CurrentLiveTask);
				}
				FieldValue->SetTransactionally(Context, Value);
			}
			else
			{
				V_DIE("Field %s has an unsupported type; cannot set!", *FieldName.AsString());
			}
			return FOpResult{FOpResult::Return};
		}
		if (ObjectOperand.IsUObject())
		{
			REQUIRE_CONCRETE(IncomingEffectToken);

			// TODO: Implement this when we stop boxing fields in VRefs.
			VERSE_UNREACHABLE();
		}
		if (VAccessorRef* AccessorRef = ObjectOperand.DynamicCast<VAccessorRef>())
		{
			// for accessors which are expecting a 'string', ie:
			//   SetAS(:accessor, J:int, Member:string, K:int, Value:int)<transacts><decides>:void
			//   set T.AS[0].I = 666 <-- we are grabbing `accessor, 0, '<verse_path_to>I'`
			// we must trim the verse-path from our field names
			VUniqueString& Field = VUniqueString::New(Context, Verse::Names::RemoveQualifier(FieldName.AsStringView()));
			VAccessorRef& NewAccessorRef = VAccessorRef::New(Context, *AccessorRef, Field);
			return CallSetter(Op, NewAccessorRef, Value, NextPC, IncomingEffectToken, CurrentBatchedRefs);
		}
		if (VNativeRef* NativeRef = ObjectOperand.DynamicCast<VNativeRef>())
		{
			FOpResult Result = NativeRef->LoadField(Context, FieldName);
			OP_RESULT_HELPER(Result);
			return Result.Value.StaticCast<VNativeRef>().Set(Context, Value);
		}
		V_DIE("Unsupported operand to a `SetField` operation!");
	}

	template <typename OpType>
	FOpResult CreateFieldImpl(
		OpType& Op,
		FOp*& NextPC,
		const VValue& IncomingEffectToken,
		EExecutionLoopKind ExecutionLoopKind)
	{
		if (ExecutionLoopKind == EExecutionLoopKind::Main && !CVarUObjectLeniency.GetValueOnAnyThread())
		{
			BumpEffectEpoch();
			if (UNLIKELY(IncomingEffectToken.IsPlaceholder()))
			{
				return SuspendOnLeniencyIndicator(Op, IncomingEffectToken);
			}
		}

		VValue TokenOperand = GetOperand(Op.Token);
		if (UNLIKELY(TokenOperand.IsPlaceholder()))
		{
			return SuspendOnLeniencyIndicator(Op, TokenOperand);
		}
		VValue ObjectOperand = GetOperand(Op.Object);
		if (UNLIKELY(ObjectOperand.IsPlaceholder()))
		{
			return SuspendOnLeniencyIndicator(Op, ObjectOperand);
		}
		VValue LeniencyIndicator = GetOperand(Op.LeniencyIndicator);

		auto DoImpl = [&] {
			V_DIE_UNLESS_MSG(ObjectOperand.IsCell(), "Unsupported object operand to a `CreateField` operation!")
			VCell& Cell = ObjectOperand.AsCell();
			uint32 InEmergentType = Cell.EmergentTypeOffset;

			// The result of CreateField indicates whether the field has already been created, either
			// by a previous initializer or as a constant entry in the object's shape.
			//
			// For VValueObjects, this state is currently tracked in the fields themselves, using the
			// uninitialized `VValue()`. Native types don't have sentinel values like this, so they are
			// wrapped in VNativeConstructorWrapper which uses a separate map.
			//
			// Constructors and class bodies use JumpIfInitialized on this result to skip overridden
			// initializers, so the uninitialized `VValue()` indicates that the field is new.
			if constexpr (std::is_same_v<OpType, FOpCreateFieldICValueObjectConstant> || std::is_same_v<OpType, FOpCreateFieldICValueObjectField>)
			{
				if (VValueObject* Object = Cell.DynamicCast<VValueObject>();
					Object && InEmergentType == Op.SourceEmergentTypeOffset)
				{
					constexpr FCreateFieldCacheCase::EKind Kind = std::is_same_v<OpType, FOpCreateFieldICValueObjectConstant>
																	? FCreateFieldCacheCase::EKind::ValueObjectConstant
																	: FCreateFieldCacheCase::EKind::ValueObjectField;
					if (!Object->CreateFieldCached(Context, {Kind, Op.FieldIndex, Op.NextEmergentTypeOffset}))
					{
						NextPC = Op.OnFailure.GetLabeledPC();
						if (!LeniencyIndicator.IsPlaceholder())
						{
							LeniencyIndicator.StaticCast<VFastFailureContext>().bFailed = true;
						}
					}
					return FOpResult{FOpResult::Return};
				}
			}

			if constexpr (std::is_same_v<OpType, FOpCreateFieldICNativeStruct> || std::is_same_v<OpType, FOpCreateFieldICUObject>)
			{
				if (VNativeConstructorWrapper* WrappedObject = Cell.DynamicCast<VNativeConstructorWrapper>();
					WrappedObject && InEmergentType == Op.SourceEmergentTypeOffset)
				{
					constexpr FCreateFieldCacheCase::EKind Kind = std::is_same_v<OpType, FOpCreateFieldICNativeStruct>
																	? FCreateFieldCacheCase::EKind::NativeStruct
																	: FCreateFieldCacheCase::EKind::UObject;
					if (!WrappedObject->CreateFieldCached(Context, {Kind, Op.FieldIndex, Op.NextEmergentTypeOffset}))
					{
						NextPC = Op.OnFailure.GetLabeledPC();
						if (!LeniencyIndicator.IsPlaceholder())
						{
							LeniencyIndicator.StaticCast<VFastFailureContext>().bFailed = true;
						}
					}
					return FOpResult{FOpResult::Return};
				}
			}

			VUniqueString& FieldName = *Op.Name.Get();
			FCreateFieldCacheCase CacheCase;
			if (VNativeConstructorWrapper* WrappedObject = Cell.DynamicCast<VNativeConstructorWrapper>())
			{
				if (!WrappedObject->CreateField(Context, FieldName, &CacheCase))
				{
					NextPC = Op.OnFailure.GetLabeledPC();
					if (!LeniencyIndicator.IsPlaceholder())
					{
						LeniencyIndicator.StaticCast<VFastFailureContext>().bFailed = true;
					}
				}
			}
			else if (VValueObject* Object = Cell.DynamicCast<VValueObject>())
			{
				if (!Object->CreateField(Context, FieldName, &CacheCase))
				{
					NextPC = Op.OnFailure.GetLabeledPC();
					if (!LeniencyIndicator.IsPlaceholder())
					{
						LeniencyIndicator.StaticCast<VFastFailureContext>().bFailed = true;
					}
				}
			}
			else
			{
				V_DIE("Unsupported object operand to a `CreateField` operation!");
			}

			// clang-format off
			if constexpr (
				std::is_same_v<OpType, FOpCreateField> ||
				std::is_same_v<OpType, FOpCreateFieldICValueObjectConstant> ||
				std::is_same_v<OpType, FOpCreateFieldICValueObjectField> ||
				std::is_same_v<OpType, FOpCreateFieldICNativeStruct> ||
				std::is_same_v<OpType, FOpCreateFieldICUObject>)
			// clang-format on
			{
				EOpcode NewOpcode;
				switch (CacheCase.Kind)
				{
					case FCreateFieldCacheCase::EKind::ValueObjectConstant:
						NewOpcode = EOpcode::CreateFieldICValueObjectConstant;
						break;
					case FCreateFieldCacheCase::EKind::ValueObjectField:
						NewOpcode = EOpcode::CreateFieldICValueObjectField;
						break;
					case FCreateFieldCacheCase::EKind::NativeStruct:
						NewOpcode = EOpcode::CreateFieldICNativeStruct;
						break;
					case FCreateFieldCacheCase::EKind::UObject:
						NewOpcode = EOpcode::CreateFieldICUObject;
						break;
					case FCreateFieldCacheCase::EKind::Invalid:
						return FOpResult{FOpResult::Return};
				}
				Op.SourceEmergentTypeOffset = InEmergentType;
				Op.FieldIndex = CacheCase.FieldIndex;
				Op.NextEmergentTypeOffset = CacheCase.NextEmergentTypeOffset;
				StoreStoreFence();
				Op.Opcode = NewOpcode;
			}

			return FOpResult{FOpResult::Return};
		};

		FOpResult Result = DoImpl();

		if (Result.IsReturn() && ExecutionLoopKind == EExecutionLoopKind::Main && !CVarUObjectLeniency.GetValueOnAnyThread())
		{
			DEF(EffectToken, IncomingEffectToken);
		}

		return Result;
	}

	template <typename OpType>
	FOpResult CreateFieldSuspensionImpl(OpType& Op, FOp*& NextPC, const VValue& IncomingEffectToken)
	{
		return FastFailSuspensionHelper(Op, [&](auto& Op, auto NextPC) {
			return CreateFieldImpl(Op, NextPC, IncomingEffectToken, EExecutionLoopKind::Suspension);
		});
	}

	template <typename OpType>
	FOpResult UnifyNativeObjectImpl(OpType& Op, FOp*& NextPC, VValue& IncomingEffectToken, VBatchedRefs* CurrentBatchedRefs)
	{
		VValue TokenOperand = GetOperand(Op.Token);
		REQUIRE_CONCRETE(TokenOperand);

		VValue ObjectOperand = GetOperand(Op.Object);
		REQUIRE_CONCRETE(ObjectOperand);

		if (VNativeConstructorWrapper* Wrapper = ObjectOperand.DynamicCast<VNativeConstructorWrapper>())
		{
			for (int32 FieldIndex = 0; FieldIndex < Wrapper->NumFields; ++FieldIndex)
			{
				REQUIRE_CONCRETE(Wrapper->Fields[FieldIndex].Get(Context));
			}
			DEF(Wrapper->SelfPlaceholder, Wrapper->WrappedObject());
		}

		if (!CVarUObjectLeniency.GetValueOnAnyThread())
		{
			// Pass nullptr for NextPC so these always use nested interpreter instances.
			FOp* NullNextPC = nullptr;

			// Call postponed setters.
			if (VAccessorSuspension* Setters = TokenOperand.DynamicCast<VAccessorSuspension>())
			{
				VAccessorSuspension* Setter = Setters;
				do
				{
					FOpResult Result = CallSetter(Op, *Setter->Ref, Setter->Value.Get(), NullNextPC, IncomingEffectToken, CurrentBatchedRefs);
					OP_RESULT_HELPER(Result);

					Setter = Setter->Next.Get();
				}
				while (Setter != Setters);
			}

			// Call postponed blocks.
			VClass* Class = nullptr;
			if (VNativeConstructorWrapper* Wrapper = ObjectOperand.DynamicCast<VNativeConstructorWrapper>())
			{
				ObjectOperand = Wrapper->WrappedObject();
			}
			if (VObject* Object = ObjectOperand.DynamicCast<VObject>())
			{
				Class = &Object->GetEmergentType()->Type->StaticCast<VClass>();
			}
			else if (UObject* NativeObject = ObjectOperand.ExtractUObject())
			{
				UClass* NativeClass = NativeObject->GetClass();
				if (UVerseClass* VerseClass = Cast<UVerseClass>(NativeClass))
				{
					Class = VerseClass->Class.Get();
				}
				else
				{
					Class = &GlobalProgram->LookupImport(Context, NativeClass)->StaticCast<VClass>();
				}
			}
			if (Class->Kind == VClass::EKind::Class)
			{
				VRestValue* Dest = nullptr;
				TArrayView<FValueOperand> Arguments;
				TArrayView<TWriteBarrier<VUniqueString>> NamedArguments;
				TArrayView<FValueOperand> NamedArgumentValues;
				FOpResult Result = CallFunction(
					Op,
					Class->GetBlocks(),
					ObjectOperand,
					Arguments,
					NamedArguments,
					NamedArgumentValues,
					Dest,
					NullNextPC,
					IncomingEffectToken,
					CurrentBatchedRefs);
				OP_RESULT_HELPER(Result)
			}
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult UnwrapNativeConstructorWrapperImpl(OpType& Op)
	{
		// Unwrap the native object and return it, while throwing away the wrapper object.
		const VValue ObjectOperand = GetOperand(Op.Object);
		REQUIRE_CONCRETE(ObjectOperand);

		if (VNativeConstructorWrapper* Wrapper = ObjectOperand.DynamicCast<VNativeConstructorWrapper>())
		{
			DEF(Op.Dest, Wrapper->SelfPlaceholder.Get(Context));
		}
		else if (VObject* VerseObject = ObjectOperand.DynamicCast<VObject>())
		{
			DEF(Op.Dest, *VerseObject);
		}
		else if (UObject* UEObject = ObjectOperand.ExtractUObject())
		{
			DEF(Op.Dest, UEObject);
		}
		else
		{
			auto Got = ObjectOperand.ToString(Context, EValueStringFormat::VerseSyntax);
			V_DIE("The `UnwrapNativeConstructorWrapper` opcode only wrapped/unwrapped objects; unrecognized operand type (%hs) indicates a problem in the codegen!", ToCStr(Got));
		}
		// The wrapper object should naturally get GC'ed after this in the next cycle, since it's only referenced when
		// we first create the native object.
		return FOpResult{FOpResult::Return};
	}

	template <typename OpType>
	FOpResult BeginProfileBlockImpl(OpType& Op)
	{
		DEF(Op.Dest, VInt(Context, BitCast<int64>(FPlatformTime::Cycles64())));

		FVerseProfilingDelegates::RaiseBeginProfilingEvent();

		return FOpResult{FOpResult::Return};
	}

	template <typename OpType>
	FOpResult EndProfileBlockImpl(OpType& Op)
	{
		const uint64_t WallTimeEnd = FPlatformTime::Cycles64();

		const uint64_t WallTimeStart = BitCast<uint64_t>(GetOperand(Op.WallTimeStart).AsInt().AsInt64());
		const double WallTimeTotal = FPlatformTime::ToMilliseconds64(WallTimeEnd - WallTimeStart);

		// Build the locus
		const VUniqueString* SnippetPath = Op.SnippetPath.Get();
		TOptional<FUtf8String> SnippetPathStr = SnippetPath->AsOptionalUtf8String();

		const FProfileLocus Locus = {
			.BeginRow = GetOperand(Op.BeginRow).AsUint32(),
			.BeginColumn = GetOperand(Op.BeginColumn).AsUint32(),
			.EndRow = GetOperand(Op.EndRow).AsUint32(),
			.EndColumn = GetOperand(Op.EndColumn).AsUint32(),
			.SnippetPath = SnippetPathStr ? SnippetPathStr.GetValue() : "",
		};

		const VValue UserTag = GetOperand(Op.UserTag);
		const VCell& UserTagCell = UserTag.AsCell();
		const FUtf8StringView UserTagStr = UserTag.AsCell().StaticCast<VArray>().AsStringView();

		FVerseProfilingDelegates::RaiseEndProfilingEvent(UserTagStr.Len() ? reinterpret_cast<const char*>(UserTagStr.GetData()) : "", WallTimeTotal, Locus);

		return FOpResult{FOpResult::Return};
	}

	template <typename OpType>
	FOpResult BeginAwaitImpl(OpType& Op, FOp*& NextPC, VValue IncomingEffectToken)
	{
		V_DIE_IF(Task->bAwaitInitializing);
		V_DIE_IF(Task->AwaitPC);
		Task->bAwaitInitializing = true;
		Task->AwaitPC = NextPC;
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult AwaitSuccessImpl(OpType& Op, FOp*& NextPC, VValue IncomingEffectToken)
	{
		if (Task->bAwaitInitializing)
		{
			FAIL();
		}
		else
		{
			Task->AwaitPC = nullptr;
		}
		return {FOpResult::Return};
	}

	template <typename OpType>
	FOpResult EndAwaitImpl(OpType& Op, FOp*& NextPC, VValue IncomingEffectToken)
	{
		V_DIE_UNLESS(Task->AwaitPC);
		Task->bAwaitInitializing = false;
		Task->AwaitPC = nullptr;
		return {FOpResult::Return};
	}

	bool EqFastFailImplHelper(VValue Lhs, VValue Rhs)
	{
		ECompares Cmp = VValue::Equal(Context, Lhs, Rhs, [](VValue Left, VValue Right) {});
		return Cmp == ECompares::Eq;
	}

	bool NeqFastFailImplHelper(VValue Lhs, VValue Rhs)
	{
		ECompares Cmp = VValue::Equal(Context, Lhs, Rhs, [](VValue Left, VValue Right) {});
		return Cmp == ECompares::Neq;
	}

	bool LtFastFailImplHelper(VValue Lhs, VValue Rhs)
	{
		if (Lhs.IsInt() && Rhs.IsInt())
		{
			if (!VInt::Lt(Context, Lhs.AsInt(), Rhs.AsInt()))
			{
				return false;
			}
		}
		else if (Lhs.IsFloat() && Rhs.IsFloat())
		{
			if (!(Lhs.AsFloat() < Rhs.AsFloat()))
			{
				return false;
			}
		}
		else if (Lhs.IsCellOfType<VRational>() && Rhs.IsCellOfType<VRational>())
		{
			VRational& LeftRational = Lhs.StaticCast<VRational>();
			VRational& RightRational = Rhs.StaticCast<VRational>();
			if (!VRational::Lt(Context, LeftRational, RightRational))
			{
				return false;
			}
		}
		else
		{
			V_DIE("Unsupported operands were passed to a `Lt` operation!");
		}

		return true;
	}

	bool LteFastFailImplHelper(VValue Lhs, VValue Rhs)
	{
		if (Lhs.IsInt() && Rhs.IsInt())
		{
			if (!VInt::Lte(Context, Lhs.AsInt(), Rhs.AsInt()))
			{
				return false;
			}
		}
		else if (Lhs.IsFloat() && Rhs.IsFloat())
		{
			if (!(Lhs.AsFloat() <= Rhs.AsFloat()))
			{
				return false;
			}
		}
		else if (Lhs.IsCellOfType<VRational>() && Rhs.IsCellOfType<VRational>())
		{
			VRational& LeftRational = Lhs.StaticCast<VRational>();
			VRational& RightRational = Rhs.StaticCast<VRational>();
			if (!VRational::Lte(Context, LeftRational, RightRational))
			{
				return false;
			}
		}
		else
		{
			V_DIE("Unsupported operands were passed to a `Lte` operation!");
		}

		return true;
	}

	bool GtFastFailImplHelper(VValue Lhs, VValue Rhs)
	{
		if (Lhs.IsInt() && Rhs.IsInt())
		{
			if (!VInt::Gt(Context, Lhs.AsInt(), Rhs.AsInt()))
			{
				return false;
			}
		}
		else if (Lhs.IsFloat() && Rhs.IsFloat())
		{
			if (!(Lhs.AsFloat() > Rhs.AsFloat()))
			{
				return false;
			}
		}
		else if (Lhs.IsCellOfType<VRational>() && Rhs.IsCellOfType<VRational>())
		{
			VRational& LeftRational = Lhs.StaticCast<VRational>();
			VRational& RightRational = Rhs.StaticCast<VRational>();
			if (!VRational::Gt(Context, LeftRational, RightRational))
			{
				return false;
			}
		}
		else
		{
			V_DIE("Unsupported operands were passed to a `Gt` operation!");
		}

		return true;
	}

	bool GteFastFailImplHelper(VValue Lhs, VValue Rhs)
	{
		if (Lhs.IsInt() && Rhs.IsInt())
		{
			if (!VInt::Gte(Context, Lhs.AsInt(), Rhs.AsInt()))
			{
				return false;
			}
		}
		else if (Lhs.IsFloat() && Rhs.IsFloat())
		{
			if (!(Lhs.AsFloat() >= Rhs.AsFloat()))
			{
				return false;
			}
		}
		else if (Lhs.IsCellOfType<VRational>() && Rhs.IsCellOfType<VRational>())
		{
			VRational& LeftRational = Lhs.StaticCast<VRational>();
			VRational& RightRational = Rhs.StaticCast<VRational>();
			if (!VRational::Gte(Context, LeftRational, RightRational))
			{
				return false;
			}
		}
		else
		{
			V_DIE("Unsupported operands were passed to a `Gte` operation!");
		}

		return true;
	}

	FOpResult NeqImplHelper(VValue LeftSource, VValue RightSource)
	{
		VValue ToSuspendOn;
		// This returns true for placeholders, so if we see any placeholders,
		// we're not yet done checking for inequality because we need to
		// check the concrete values.
		ECompares Cmp = VValue::Equal(Context, LeftSource, RightSource, [&](VValue Left, VValue Right) {
			AutoRTFM::UnreachableIfClosed("#jira SOL-8465");
			checkSlow(Left.IsPlaceholder() || Right.IsPlaceholder());
			if (!ToSuspendOn)
			{
				ToSuspendOn = Left.IsPlaceholder() ? Left : Right;
			}
		});

		if (Cmp == ECompares::Neq)
		{
			return {FOpResult::Return};
		}
		REQUIRE_CONCRETE(ToSuspendOn);
		FAIL();
	}

	FOpResult LtImplHelper(VValue LeftSource, VValue RightSource)
	{
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);
		if (!LtFastFailImplHelper(LeftSource, RightSource))
		{
			FAIL();
		}
		return {FOpResult::Return};
	}

	FOpResult LteImplHelper(VValue LeftSource, VValue RightSource)
	{
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);
		if (!LteFastFailImplHelper(LeftSource, RightSource))
		{
			FAIL();
		}
		return {FOpResult::Return};
	}

	FOpResult GtImplHelper(VValue LeftSource, VValue RightSource)
	{
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);
		if (!GtFastFailImplHelper(LeftSource, RightSource))
		{
			FAIL();
		}
		return {FOpResult::Return};
	}

	FOpResult GteImplHelper(VValue LeftSource, VValue RightSource)
	{
		REQUIRE_CONCRETE(LeftSource);
		REQUIRE_CONCRETE(RightSource);
		if (!GteFastFailImplHelper(LeftSource, RightSource))
		{
			FAIL();
		}
		return {FOpResult::Return};
	}

	FORCENOINLINE void HandleHandshakeSlowpath()
	{
		if (Context.IsRuntimeErrorRequested())
		{
			Context.ClearRuntimeErrorRequest();
			State.PC = &ThrowRuntimeErrorSentry;
			return;
		}

		if (FDebugger* Debugger = GetDebugger(); Debugger && State.PC != &StopInterpreterSentry)
		{
			Debugger->Notify(Context, *State.PC, *State.Frame, *Task);
		}

		if (FSamplingProfiler* Sampler = GetRunningSamplingProfiler())
		{
			bool bExpected = true;
			if (Sampler->SampleRequested.compare_exchange_weak(bExpected, false, std::memory_order_relaxed))
			{
				Sampler->Sample(Context, State.PC, State.Frame, Task);
			}
		}
	}

	template <typename OpType>
	FOpResult ArrayIndexFastFailImpl(OpType& Op, FOp*& NextPC)
	{
		return FastFailHelper(Op, NextPC, Op.Array, Op.Index, [&](VValue Callee, VValue Argument) {
			if (VArray* Array = Callee.DynamicCast<VArray>())
			{
				// Bounds check since this index access in Verse is fallible.
				if (!Argument.IsUint32() || !Array->IsInBounds(Argument.AsUint32()))
				{
					FAIL();
				}
				return FOpResult{FOpResult::Return, Array->GetValue(Argument.AsUint32())};
			}
			if (VMutableArray* MutableArray = Callee.DynamicCast<VMutableArray>())
			{
				// Bounds check since this index access in Verse is fallible.
				if (!Argument.IsUint32())
				{
					FAIL();
				}
				uint32 Index = Argument.AsUint32();
				if (!MutableArray->IsInBounds(Index))
				{
					FAIL();
				}
				VValue Element = UnwrapTransparentRef(Context, MutableArray->GetValue(Index), Task, [&](VValue Element) {
					AUTORTFM_SANITIZER_DISABLE_SCOPE();
					MutableArray->SetValue(Context, Index, Element);
				});
				return FOpResult{FOpResult::Return, Element};
			}
			V_DIE("Got non-array for `FOpArrayIndexFastFail`");
		});
	}

	template <typename OpType>
	FOpResult ArrayIndexFastFailSuspensionImpl(OpType& Op)
	{
		return FastFailSuspensionHelper(
			Op,
			[&](auto& Op, auto NextPC) {
				return ArrayIndexFastFailImpl(Op, NextPC);
			});
	}

	template <typename OpType>
	FOpResult QueryFastFailImpl(OpType& Op, FOp*& NextPC)
	{
		return FastFailHelper(Op, NextPC, Op.Source, FValueOperand(), [&](VValue Source, VValue Dummy) {
			return QueryImplHelper(Source);
		});
	}

	template <typename OpType>
	FOpResult QueryFastFailSuspensionImpl(OpType& Op)
	{
		return FastFailSuspensionHelper(Op, [&](auto& Op, auto NextPC) { return QueryFastFailImpl(Op, NextPC); });
	}

	template <typename OpType>
	FOpResult CanFastAppendToArrayFastFailImpl(OpType& Op, FOp*& NextPC)
	{
		return FastFailHelper(Op, NextPC, Op.Ref, Op.MaybeMutableArray, [&](VValue Ref, VValue MaybeMutableArray) {
			return CanFastAppendToArrayFastFailImplHelper(Ref, MaybeMutableArray);
		});
	}

	template <typename OpType>
	FOpResult CanFastAppendToArrayFastFailSuspensionImpl(OpType& Op)
	{
		return FastFailSuspensionHelper(Op, [&](auto& Op, auto NextPC) { return CanFastAppendToArrayFastFailImpl(Op, NextPC); });
	}

	template <typename OpType>
	FOpResult TypeCastFastFailImpl(OpType& Op, FOp*& NextPC)
	{
		return FastFailHelper(Op, NextPC, Op.Type, Op.Value, [&](VValue Type, VValue Value) {
			if (VType* Ty = Type.DynamicCast<VType>())
			{
				if (Ty->Subsumes(Context, Value))
				{
					return FOpResult{FOpResult::Return, Value};
				}
				else
				{
					return FOpResult{FOpResult::Fail};
				}
			}
			else
			{
				V_DIE("Got non-type for `FOpTypeCastFastFail`");
			}
		});
	}

	template <typename OpType>
	FOpResult TypeCastFastFailSuspensionImpl(OpType& Op)
	{
		return FastFailSuspensionHelper(Op, [&](auto& Op, auto NextPC) { return TypeCastFastFailImpl(Op, NextPC); });
	}

	template <typename OpType>
	FOpResult EqFastFailImpl(OpType& Op, FOp*& NextPC)
	{
		return FastFailHelper(Op, NextPC, Op.Lhs, Op.Rhs, [&](VValue Lhs, VValue Rhs) {
			return FOpResult{EqFastFailImplHelper(Lhs, Rhs) ? FOpResult::Return : FOpResult::Fail, Lhs};
		});
	}

	template <typename OpType>
	FOpResult EqFastFailSuspensionImpl(OpType& Op)
	{
		return FastFailSuspensionHelper(Op, [&](auto& Op, auto NextPC) { return EqFastFailImpl(Op, NextPC); });
	}

#define DECLARE_COMPARISON_OP_IMPL(OpName)                                                                            \
	template <typename OpType>                                                                                        \
	FOpResult OpName##Impl(OpType& Op)                                                                                \
	{                                                                                                                 \
		VValue LeftSource = GetOperand(Op.LeftSource);                                                                \
		VValue RightSource = GetOperand(Op.RightSource);                                                              \
		FOpResult Result = OpName##ImplHelper(LeftSource, RightSource);                                               \
		if (Result.IsReturn())                                                                                        \
		{                                                                                                             \
			/* success returns the left - value */                                                                    \
			Def(GetTrailFor(Op), Op.Dest, LeftSource);                                                                \
		}                                                                                                             \
		return Result;                                                                                                \
	}                                                                                                                 \
                                                                                                                      \
	template <typename OpType>                                                                                        \
	FOpResult OpName##FastFailImpl(OpType& Op, FOp*& NextPC)                                                          \
	{                                                                                                                 \
		return FastFailHelper(Op, NextPC, Op.Lhs, Op.Rhs, [&](VValue Lhs, VValue Rhs) {                               \
			return FOpResult{OpName##FastFailImplHelper(Lhs, Rhs) ? FOpResult::Return : FOpResult::Fail, Lhs};        \
		});                                                                                                           \
	}                                                                                                                 \
                                                                                                                      \
	template <typename OpType>                                                                                        \
	FOpResult OpName##FastFailSuspensionImpl(OpType& Op)                                                              \
	{                                                                                                                 \
		return FastFailSuspensionHelper(Op, [&](auto& Op, auto NextPC) { return OpName##FastFailImpl(Op, NextPC); }); \
	}

	DECLARE_COMPARISON_OP_IMPL(Neq)
	DECLARE_COMPARISON_OP_IMPL(Lt)
	DECLARE_COMPARISON_OP_IMPL(Lte)
	DECLARE_COMPARISON_OP_IMPL(Gt)
	DECLARE_COMPARISON_OP_IMPL(Gte)

	// TODO SOL-4461: Return should work with lenient execution of failure contexts.
	// We can't just logically execute the first Return we encounter during lenient
	// execution if the then/else when executed would've returned.
	//
	// We also need to figure out how to properly pop a frame off if the
	// failure context we're leniently executing returns. We could continue
	// to execute the current frame and just not thread through the effect
	// token, so no effects could happen. But that's inefficient.
	template <bool bTrailed, typename OpType>
	V_FORCEINLINE ECompares ReturnImpl(OpType& Op, FOp*& NextPC)
	{
		FTrail& Trail = GetTrailFor(Op);
		VValue IncomingEffectToken = EffectToken.Get(Context);

		{
			ECompares Result;
			if constexpr (bTrailed)
			{
				Result = DefTrailed(Trail, State.Frame->ReturnSlot.EffectToken, IncomingEffectToken);
			}
			else
			{
				Result = Def(Trail, State.Frame->ReturnSlot.EffectToken, IncomingEffectToken);
			}
			checkSlow(Result == ECompares::Eq);
		}

		VValue Value = GetOperand(Op.Value);
		VFrame* Frame = State.Frame;

		if (VFrame* CallerFrame = Frame->CallerFrame.Get())
		{
			NextPC = Frame->CallerPC;
			State = FExecutionState(NextPC, CallerFrame);
		}
		else
		{
			NextPC = &StopInterpreterSentry;
		}

		// TODO: Add a test where this unification fails at the top level with no return continuation.
		ECompares Result;
		if constexpr (bTrailed)
		{
			Result = DefReturnSlotTrailed(Trail, Frame->ReturnSlot, Value);
		}
		else
		{
			Result = DefReturnSlot(Trail, Frame->ReturnSlot, Value);
		}
		return Result;
	}

#undef ENQUEUE_SUSPENSION
#undef FAIL
#undef YIELD
#undef RUNTIME_ERROR

	using FDispatchFn = V_PRESERVE_NONE FOpResult::EKind (FInterpreter::*)(const bool);
	using FDispatchArray = TStaticArray<FDispatchFn, static_cast<FOpcodeInt>(EOpcode::OpcodeCount)>;
	inline static FDispatchArray TraceDispatchArray = {};
	inline static FDispatchArray NoTraceDispatchArray = {};

	struct FDispatchInstaller
	{
		FDispatchInstaller(FDispatchArray& DispatchArray, EOpcode Opcode, FDispatchFn Handler)
		{
			DispatchArray[static_cast<FOpcodeInt>(Opcode)] = Handler;
		}
	};

	// NOTE: (yiliang.siew) We don't templat-ize `bHasOutermostPCBounds` since it would mean duplicating all the opcode handlers,
	// which bloats compile times.
	template <bool bPrintTrace>
	V_PRESERVE_NONE V_FORCEINLINE FOpResult::EKind Dispatch(bool bHasOutermostPCBounds)
	{
		if (bHasOutermostPCBounds)
		{
			if (UNLIKELY(!State.Frame->CallerFrame && State.PC >= OutermostEndPC))
			{
				State.PC = &StopInterpreterSentry;
			}
		}

		// We want the handshake to be before we load the opcode.
		Context.CheckForHandshake([this] {
			HandleHandshakeSlowpath();
		});

		if (UnblockedSuspensionQueue)
		{
			[[clang::musttail]] return DispatchSuspension<bPrintTrace>(bHasOutermostPCBounds);
		}

		EOpcode Opcode = State.PC->Opcode;
		checkSlow(Opcode < EOpcode::OpcodeCount);
		FOpcodeInt OpInt = static_cast<FOpcodeInt>(Opcode);
		FDispatchFn Fn;
		if constexpr (bPrintTrace)
		{
			Fn = TraceDispatchArray[OpInt];
		}
		else
		{
			Fn = NoTraceDispatchArray[OpInt];
		}
		checkSlow(Fn);
		[[clang::musttail]] return (this->*Fn)(bHasOutermostPCBounds);
	}

	// Macros to be used in both the interpreter loops.
	// Parameterized over the implementation of BEGIN/END_OP_CASE as well as ENQUEUE_SUSPENSION, FAIL, YIELD, and RUNTIME_ERROR

#define OP_IMPL_HELPER(OpImpl, ...)                                 \
	FOpResult Result{FOpResult::Error};                             \
	V_INLINE_UNLESS_DEBUG_BUILD Result = OpImpl(Op, ##__VA_ARGS__); \
	OP_RESULT_HELPER(Result)

// Define an opcode implementation that may suspend as part of execution.
#define OP(OpName, ...) OP_WITH_IMPL(OpName, OpName##Impl, ##__VA_ARGS__)
#define OP_WITH_IMPL(OpName, OpImpl, ...) \
	BEGIN_OP_CASE(OpName)                 \
	OP_IMPL_HELPER(OpImpl, ##__VA_ARGS__) \
	END_OP_CASE(OpName)

// Macro definitions to be used in the main interpreter loop.

// We REQUIRE_CONCRETE on the effect token first because it obviates the need to capture
// the incoming effect token. If the incoming effect token is a placeholder, we will
// suspend, and we'll only resume after it becomes concrete.
#define OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(OpName, ...) OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(OpName, OpName##Impl, ##__VA_ARGS__)
#define OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(OpName, OpImpl, ...)                            \
	BEGIN_OP_CASE(OpName)                                                                              \
	{                                                                                                  \
		VValue IncomingEffectToken = EffectToken.Get(Context);                                         \
		BumpEffectEpoch();                                                                             \
		REQUIRE_CONCRETE(IncomingEffectToken);                                                         \
		OP_IMPL_HELPER(OpImpl, NextPC, static_cast<const VValue&>(IncomingEffectToken), ##__VA_ARGS__) \
		DEF(EffectToken, IncomingEffectToken);                                                         \
	}                                                                                                  \
	END_OP_CASE(OpName)

#define OP_IMPL_CAPTURES_EFFECT_TOKENS(OpName, ...) OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(OpName, OpName##Impl, ##__VA_ARGS__)
#define OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(OpName, OpImpl, ...)                                \
	BEGIN_OP_CASE(OpName)                                                                            \
	{                                                                                                \
		VValue IncomingEffectToken = EffectToken.Get(Context);                                       \
		FOpResult Result{FOpResult::Error};                                                          \
		V_INLINE_UNLESS_DEBUG_BUILD Result = OpImpl(Op, NextPC, IncomingEffectToken, ##__VA_ARGS__); \
		EffectToken.Set(Context, IncomingEffectToken);                                               \
		OP_RESULT_HELPER(Result);                                                                    \
	}                                                                                                \
	END_OP_CASE(OpName)

#define NEXT_OP(bSuspended, bFailed)   \
	if constexpr (bPrintTrace)         \
	{                                  \
		EndTrace(bSuspended, bFailed); \
	}                                  \
                                       \
	State.PC = NextPC;                 \
	[[clang::musttail]] return Dispatch<bPrintTrace>(bHasOutermostPCBounds);

#define BEGIN_OP_CASE(Name)                                                                     \
	template <bool bPrintTrace>                                                                 \
	V_PRESERVE_NONE FOpResult::EKind Name##InternalDispatcher(const bool bHasOutermostPCBounds) \
	{                                                                                           \
		if constexpr (bPrintTrace)                                                              \
		{                                                                                       \
			BeginTrace();                                                                       \
		}                                                                                       \
		FOp##Name& Op = *static_cast<FOp##Name*>(State.PC);                                     \
		FOp* NextPC = BitCast<FOp*>(&Op + 1);

#define END_OP_CASE(Name)                                                                                     \
	NEXT_OP(false, false);                                                                                    \
	}                                                                                                         \
	inline static const FDispatchInstaller Name##TraceInstaller =                                             \
		FDispatchInstaller(TraceDispatchArray, EOpcode::Name, &FInterpreter::Name##InternalDispatcher<true>); \
	inline static const FDispatchInstaller Name##NoTraceInstaller =                                           \
		FDispatchInstaller(NoTraceDispatchArray, EOpcode::Name, &FInterpreter::Name##InternalDispatcher<false>);

#define ENQUEUE_SUSPENSION(Value)                               \
	VBytecodeSuspension& Suspension = VBytecodeSuspension::New( \
		Context,                                                \
		*FailureContext(),                                      \
		*Task,                                                  \
		*State.Frame->Procedure,                                \
		State.PC,                                               \
		MakeCaptures(Op));                                      \
	Value.EnqueueSuspension(Context, Suspension);               \
	++FailureContext()->SuspensionCount;                        \
	NEXT_OP(true, false)

#define FAIL()                                                                             \
	if (Fail() == FOpResult::Error)                                                        \
	{                                                                                      \
		return FOpResult::Error;                                                           \
	}                                                                                      \
	if (UnblockedSuspensionQueue)                                                          \
	{                                                                                      \
		[[clang::musttail]] return DispatchSuspension<bPrintTrace>(bHasOutermostPCBounds); \
	}                                                                                      \
	if (!UnwindIfNeeded())                                                                 \
	{                                                                                      \
		if constexpr (bPrintTrace)                                                         \
		{                                                                                  \
			EndTrace(false, true);                                                         \
		}                                                                                  \
		return FOpResult::Fail;                                                            \
	}                                                                                      \
	NextPC = State.PC;                                                                     \
	NEXT_OP(false, true)

#define YIELD()                         \
	AssertMayYield(Op);                 \
	Suspend(*Task, MakeReturnSlot(Op)); \
	if (!YieldIfNeeded(NextPC))         \
	{                                   \
		return FOpResult::Yield;        \
	}                                   \
	NextPC = State.PC;                  \
	NEXT_OP(false, false)

#define RUNTIME_ERROR(Value) return FOpResult::Error

#define UPDATE_EXECUTION_STATE(PC, Frame) \
	NextPC = PC;                          \
	State = FExecutionState(NextPC, Frame)

	OP(Add)
	OP(Sub)
	OP(Mul)
	OP(Div)
	OP(Mod)
	OP(Neg)

	OP(MutableAdd)

	OP(Neq)
	OP(Lt)
	OP(Lte)
	OP(Gt)
	OP(Gte)

	OP(LtFastFail, NextPC)
	OP(LteFastFail, NextPC)
	OP(GtFastFail, NextPC)
	OP(GteFastFail, NextPC)
	OP(EqFastFail, NextPC)
	OP(NeqFastFail, NextPC)
	OP(ArrayIndexFastFail, NextPC)
	OP(TypeCastFastFail, NextPC)
	OP(QueryFastFail, NextPC)

	OP(Query)

	OP(Melt)
	OP_IMPL_CAPTURES_EFFECT_TOKENS(Freeze, BatchedRefs)

	BEGIN_OP_CASE(NewRef)
	{
		if (UNLIKELY(Verse::FInstantiationScope::Context.bModuleTopLevel))
		{
			RAISE_RUNTIME_ERROR_FORMAT(Context,
				ERuntimeDiagnostic::ErrRuntime_UnimplementedGlobalVariable,
				TEXT("Can't create a var at module scope."));
			return FOpResult::Error;
		}

		DEF(Op.Dest, VRef::New(Context, GetOperand(Op.Domain)));
	}
	END_OP_CASE(NewRef)
	BEGIN_OP_CASE(NewPersistentOrSessionWeakMapRef)
	{
		DEF(Op.Dest, VRef::New(Context));
	}
	END_OP_CASE(NewPersistentOrSessionWeakMapRef)

	OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(RefGet)
	OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(RefSet, RefSetImpl, BatchedRefs, {})
	OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(RefSetLive, RefSetImpl, BatchedRefs, GetOperand(Op.Task))
	OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(SetField, SetFieldImpl, BatchedRefs, {})
	OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(SetFieldLive, SetFieldImpl, BatchedRefs, GetOperand(Op.Task))
	OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(CallSet, CallSetImpl, BatchedRefs, {})
	OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(CallSetLive, CallSetImpl, BatchedRefs, GetOperand(Op.Task))
	OP_IMPL_CAPTURES_EFFECT_TOKENS(RefCallDomain, BatchedRefs)

	OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(FastAppendToArray)
	OP(CanFastAppendToArrayFastFail, NextPC)

	OP(NewOption)
	OP(Length)
	OP(NewArray)
	OP(NewMutableArray)
	OP(NewMutableArrayWithCapacity)
	OP(ArrayAdd)
	OP(InPlaceMakeImmutable)
	OP(NewMap)
	OP(MapKey)
	OP(MapValue)

	OP(NewClass)
	OP(BindNativeClass)
	OP(EndModuleData)

	BEGIN_OP_CASE(ConstructNativeDefaultObject)
	{
		VValue ClassValue = GetOperand(Op.Class);
		VClass& Class = ClassValue.StaticCast<VClass>();
		UStruct* Struct = Class.GetOrCreateNativeType(Context);
		if (UVerseClass* VerseClass = Cast<UVerseClass>(Struct))
		{
			VValue IncomingEffectToken = EffectToken.Get(Context);
			FOpResult Result = Context.PushNativeFrame(FailureContext(), Task, BatchedRefs, IncomingEffectToken, /*Callee*/ nullptr, State.PC, State.Frame, [&] {
				return Class.ConstructNativeDefaultObject(Context);
			});
			if (Result.IsError())
			{
				return Result.Kind;
			}
			V_DIE_UNLESS(Result.IsReturn());
		}
	}
	END_OP_CASE(ConstructNativeDefaultObject)
	BEGIN_OP_CASE(LoadImport)
	{
		VValue ClassValue = GetOperand(Op.Class);
		VClass& Class = ClassValue.StaticCast<VClass>();

		Context.PushImport(Op.ImportPath);

		AssertMayYield(Op);
		Suspend(*Task, Class.NativeType.Follow());
		if (!YieldIfNeeded(NextPC))
		{
			return FOpResult::Yield;
		}
		NextPC = State.PC;
	}
	END_OP_CASE(LoadImport)

	BEGIN_OP_CASE(JumpIfDefaultSubObject)
	{
		VValue ObjectOperand = GetOperand(Op.Object);
		V_DIE_IF(ObjectOperand.IsPlaceholder());
		if (VNativeConstructorWrapper* Wrapper = ObjectOperand.DynamicCast<VNativeConstructorWrapper>())
		{
			ObjectOperand = Wrapper->WrappedObject();
		}
		if (UObject* Object = ObjectOperand.ExtractUObject())
		{
			if (Object->HasAnyFlags(RF_DefaultSubObject))
			{
				NextPC = Op.OnDefaultSubObject.GetLabeledPC();
			}
		}
	}
	END_OP_CASE(JumpIfDefaultSubObject)

	// NOTE: These two instructions must run in the same FInterpreter as each other, because they push+pop InstantiationContext.
	BEGIN_OP_CASE(BeginModule)
	{
		VValue PackageOperand = GetOperand(Op.Package);
		VPackage& Package = PackageOperand.StaticCast<VPackage>();
		UPackage* Outer = Package.GetOrCreateUPackage(Context);
		UClass* Module = nullptr;
		{
			const FString ModuleName = FString(Op.ModuleName.Get()->AsStringView());
			V_DIE_IF(ModuleName.IsEmpty());
			Module = FindObject<UClass>(Outer, ModuleName);
			if (!Module)
			{
				Module = NewObject<UClass>(Outer, *ModuleName, RF_Public);
				UStruct* Super = UObject::StaticClass();
				Module->SetSuperStruct(Super);
				Module->PropertyLink = Super->PropertyLink;
				Module->Bind();
				Module->StaticLink(/* bRelinkExistingProperties = */ true);
				Module->CollectBytecodeAndPropertyReferencedObjectsRecursively();
				Module->AssembleReferenceTokenStream(/* bForce = */ true);
			}
		}
		UObject* ModuleCDO = nullptr;
		if (CVarUObjectLeniency.GetValueOnAnyThread())
		{
			ModuleCDO = Module->GetDefaultObject();
		}
		else
		{
			V_DIE_IF(EffectToken.Get(Context).IsPlaceholder());
			V_DIE_UNLESS(Context.NativeFrame()->IsCurrent(Context));
			const AutoRTFM::ETransactionStatus Status = AutoRTFM::Close([&] {
				ModuleCDO = Module->GetDefaultObject();
			});
			if (Status != AutoRTFM::ETransactionStatus::Executing)
			{
				FOpResult Result = Context.HandleAutoRTFMFailure(Status);
				V_DIE_UNLESS(Result.IsError());
				RUNTIME_ERROR(Result.Value);
			}
		}
		V_DIE_UNLESS(ModuleCDO);

		InstantiationContext.Emplace(FInstantiationContext{.Outer = ModuleCDO, .Flags = RF_Public, .bModuleTopLevel = true});
		PackageContext.Emplace(Context.SetCurrentPackage(&Package));

		DEF(Op.Dest, Module);
	}
	END_OP_CASE(BeginModule)
	BEGIN_OP_CASE(EndModule)
	{
		PackageContext.RemoveAt(PackageContext.Num() - 1);
		InstantiationContext.RemoveAt(InstantiationContext.Num() - 1);

		VValue ModuleOperand = GetOperand(Op.Module);
		UClass* Module = CastChecked<UClass>(ModuleOperand.ExtractUObject());
		UObject* ModuleCDO = Module->GetDefaultObject();

		FOverridableManager::Get().Disable(ModuleCDO, /*bPropagateToSubObjects*/ true);
	}
	END_OP_CASE(EndModule)

	OP(LoadConstructor)
	OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(NewObject, NewObjectImpl, BatchedRefs)
	OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(NewObjectICClass, NewObjectImpl, BatchedRefs)
	OP_WITH_IMPL(CreateField, CreateFieldImpl, NextPC, EffectToken.Get(Context), EExecutionLoopKind::Main)
	OP_WITH_IMPL(CreateFieldICValueObjectConstant, CreateFieldImpl, NextPC, EffectToken.Get(Context), EExecutionLoopKind::Main)
	OP_WITH_IMPL(CreateFieldICValueObjectField, CreateFieldImpl, NextPC, EffectToken.Get(Context), EExecutionLoopKind::Main)
	OP_WITH_IMPL(CreateFieldICNativeStruct, CreateFieldImpl, NextPC, EffectToken.Get(Context), EExecutionLoopKind::Main)
	OP_WITH_IMPL(CreateFieldICUObject, CreateFieldImpl, NextPC, EffectToken.Get(Context), EExecutionLoopKind::Main)
	OP_WITH_IMPL(UnifyField, UnifyFieldImpl, NextPC, EffectToken.Get(Context), EExecutionLoopKind::Main)
	OP_IMPL_CAPTURES_EFFECT_TOKENS(InitializeVar, BatchedRefs, EExecutionLoopKind::Main)
	OP_IMPL_CAPTURES_EFFECT_TOKENS(UnifyNativeObject, BatchedRefs)
	OP(UnwrapNativeConstructorWrapper)

	OP_WITH_IMPL(LoadField, LoadFieldImpl)
	OP_WITH_IMPL(LoadFieldICOffset, LoadFieldImpl)
	OP_WITH_IMPL(LoadFieldICConstant, LoadFieldImpl)
	OP_WITH_IMPL(LoadFieldICFunction, LoadFieldImpl)
	OP_WITH_IMPL(LoadFieldICAccessor, LoadFieldImpl)
	OP(LoadFieldFromSuper)

	OP(NewScope)
	OP(NewFunction)
	OP(LoadParentScope)
	OP(LoadCapture)

	OP(BeginProfileBlock)
	OP(EndProfileBlock)

	BEGIN_OP_CASE(Err)
	{
		// If this is the stop interpreter sentry op, return.
		if (&Op == &StopInterpreterSentry)
		{
			return FOpResult::Return;
		}

		RAISE_RUNTIME_ERROR_CODE(Context, ERuntimeDiagnostic::ErrRuntime_Internal);
		return FOpResult::Error;
	}
	END_OP_CASE(Err)

	BEGIN_OP_CASE(Tracepoint)
	{
		VUniqueString& Name = *Op.Name.Get();
		UE_LOGF(LogVerseVM, Display, "Hit tracepoint: %ls", *Name.AsString());
	}
	END_OP_CASE(Tracepoint)

	BEGIN_OP_CASE(Move)
	{
		// TODO SOL-4459: This doesn't work with leniency and failure. For example,
		// if both Dest/Source are placeholders of split depth not of the current
		// context, a suspension should be enqueued on both `Dest` and `Source`.
		DEF(Op.Dest, GetOperand(Op.Source));
	}
	END_OP_CASE(Move)

	BEGIN_OP_CASE(MoveTrailed)
	{
		// TODO SOL-4459: This doesn't work with leniency and failure. For example,
		// if both Dest/Source are placeholders of split depth not of the current
		// context, a suspension should be enqueued on both `Dest` and `Source`.
		DEF_RESULT_HELPER(DefTrailed(GetTrailFor(Op), Op.Dest, GetOperand(Op.Source)));
	}
	END_OP_CASE(MoveTrailed)

	BEGIN_OP_CASE(MoveNonComparable)
	{
		// TODO SOL-4459: This doesn't work with leniency and failure. For example,
		// if both Dest/Source are placeholders of split depth not of the current
		// context, a suspension should be enqueued on both `Dest` and `Source`.
		switch (Def(GetTrailFor(Op), Op.Dest, GetOperand(Op.Source)))
		{
			case ECompares::Eq:
				break;
			case ECompares::Neq:
			case ECompares::Undecidable:
				FAIL();
			case ECompares::RuntimeError:
				RUNTIME_ERROR();
			default:
				VERSE_UNREACHABLE();
		}
	}
	END_OP_CASE(MoveNonComparable)

	BEGIN_OP_CASE(ResetNonTrailed)
	{
		State.Frame->Registers[Op.Dest.Index].Reset(0);
	}
	END_OP_CASE(ResetNonTrailed)

	BEGIN_OP_CASE(Reset)
	{
		State.Frame->Registers[Op.Dest.Index].ResetTrailed(Context, _FailureContext->Trail, 0);
	}
	END_OP_CASE(Reset)

	BEGIN_OP_CASE(Jump)
	{
		NextPC = Op.JumpOffset.GetLabeledPC();
	}
	END_OP_CASE(Jump)

	BEGIN_OP_CASE(JumpIfInitialized)
	{
		VValue Val = GetOperand(Op.Source);
		if (!Val.IsUninitialized())
		{
			NextPC = Op.JumpOffset.GetLabeledPC();
		}
	}
	END_OP_CASE(JumpIfInitialized)

	BEGIN_OP_CASE(Switch)
	{
		VValue Which = GetOperand(Op.Which);
		TArrayView<FLabelOffset> Offsets = GetConstants(Op.JumpOffsets);
		NextPC = Offsets[Which.AsInt32()].GetLabeledPC();
	}
	END_OP_CASE(Switch)

	BEGIN_OP_CASE(BeginFailureContext)
	{
		if constexpr (DoStats)
		{
			TotalNumFailureContexts += 1.0;
		}

		VValue IncomingEffectToken = EffectToken.Get(Context);

		static_assert(std::is_trivially_destructible_v<VFailureContext>);
		void* Allocation = PopReusableFailureContext();
		if (!Allocation)
		{
			Allocation = FAllocationContext(Context).AllocateFastCell(sizeof(VFailureContext));
		}
		_FailureContext = new (Allocation) VFailureContext(
			Context,
			Task,
			_FailureContext,
			*State.Frame,
			IncomingEffectToken,
			BatchedRefs,
			Op.OnFailure.GetLabeledPC());

		if (IncomingEffectToken.IsPlaceholder())
		{
			BumpEffectEpoch();
			// This purposefully escapes the failure context.
			DoTransactionActionWhenEffectTokenIsConcrete<TransactAction::Start>(*FailureContext(), *Task, IncomingEffectToken, EffectToken.Get(Context));
		}
		else
		{
			_FailureContext->Transaction.Start(Context);
			++NumUnescapedFailureContexts;
		}
	}
	END_OP_CASE(BeginFailureContext)

	BEGIN_OP_CASE(EndFailureContext)
	{
		V_DIE_IF(_FailureContext->bFailed);   // We shouldn't have failed and still made it here.
		V_DIE_UNLESS(_FailureContext->Frame); // A null Frame indicates an artificial context from task resumption.

		if (_FailureContext->SuspensionCount)
		{
			// When we suspend inside of a failure context, we escape that failure context.
			V_DIE_UNLESS(NumUnescapedFailureContexts == 0);

			_FailureContext->bExecutedEndFailureContextOpcode = true;
			_FailureContext->ThenPC = NextPC;
			_FailureContext->DonePC = Op.Done.GetLabeledPC();

			if (_FailureContext->Parent)
			{
				++_FailureContext->Parent->SuspensionCount;
			}
			_FailureContext->BeforeThenEffectToken.Set(Context, EffectToken.Get(Context));
			EffectToken.Set(Context, _FailureContext->DoneEffectToken.Get(Context));
			NextPC = Op.Done.GetLabeledPC();
			_FailureContext->Frame.Set(Context, _FailureContext->Frame->CloneWithoutCallerInfo(Context));
		}
		else
		{
			_FailureContext->FinishedExecuting(Context);

			if (VValue IncomingEffectToken = EffectToken.Get(Context); IncomingEffectToken.IsPlaceholder())
			{
				// This is the case where an effect token wasn't concrete when the failure context started.
				// We shouldn't have created an unescaped failure context to begin with in this case. See
				// code in BeginFailureContext.
				checkSlow(NumUnescapedFailureContexts == 0);
				BumpEffectEpoch();
				DoTransactionActionWhenEffectTokenIsConcrete<TransactAction::Commit>(*FailureContext(), *Task, IncomingEffectToken, EffectToken.Get(Context));
			}
			else
			{
				if (NumUnescapedFailureContexts)
				{
					// We didn't escape the current failure context: we didn't suspend and the effect token is concrete.
					// Therefore, we can put it into our cache for reuse.
					PushReusableFailureContext();
				}
				_FailureContext->Transaction.Commit(Context);
			}
		}

		// TODO (Saam) (#jira SOL-8798): Should this really go here and not in FailureContext.FinishedExecuting?
		// Right now, we're committing to the parent transaction log before we know if the
		// failure context succeeds or fails if it's lenient.
		_FailureContext->Trail.Exit(Context);

		_FailureContext = _FailureContext->Parent.Get();
	}
	END_OP_CASE(EndFailureContext)

	BEGIN_OP_CASE(EndFastFailureContext)
	{
		VValue LeniencyIndicator = GetOperand(Op.LeniencyIndicator);
		if (!LeniencyIndicator.IsPlaceholder())
		{
			VFastFailureContext& FastFailureContext = LeniencyIndicator.StaticCast<VFastFailureContext>();
			if (FastFailureContext.Suspensions)
			{
				FastFailureContext.ThenPC = NextPC;
				NextPC = Op.OnDone.GetLabeledPC();
				FastFailureContext.DonePC = NextPC;
				FastFailureContext.Frame.Set(Context, State.Frame->CloneWithoutCallerInfo(Context));
				FastFailureContext.EnclosingFailureContext.Set(Context, FailureContext());

				VFastFailureContext& Parent = UpdateIndicatorForSuspend(GetTrailFor(Op), Op.OuterLeniencyIndicator, nullptr);
				FastFailureContext.Parent.Set(Context, &Parent);

				EffectToken.Set(Context, FastFailureContext.DoneEffectToken.Get(Context));
			}
		}
	}
	END_OP_CASE(EndFastFailureContext)

	BEGIN_OP_CASE(SelfTask)
	{
		DEF(Op.Dest, *Task);
	}
	END_OP_CASE(SelfTask)

	BEGIN_OP_CASE(BeginTask)
	{
		VValue ParentValue = GetOperand(Op.Parent);
		VTask* Parent = ParentValue ? &ParentValue.StaticCast<VTask>() : nullptr;
		Task = &VTask::New(Context, Op.OnYield.GetLabeledPC(), State.Frame, Task, Parent, State.Frame);
		if (Op.bAddToTaskGroup)
		{
			Task->AddToTaskGroup(Context);
		}
		DEF(Op.Dest, *Task);
	}
	END_OP_CASE(BeginTask)

	BEGIN_OP_CASE(CallTask)
	{
		VValue ParentValue = GetOperand(Op.Parent);
		VTask* Parent = ParentValue ? &ParentValue.StaticCast<VTask>() : nullptr;

		VValue CalleeValue = GetOperand(Op.Callee);
		VFunction& Callee = CalleeValue.StaticCast<VFunction>();

		TArrayView<FValueOperand> Arguments = GetOperands(Op.Arguments);

		VProcedure& Procedure = Callee.Procedure.Get().StaticCast<VProcedure>();
		VFrame& Frame = MakeFrameForCallee(
			Context, /*CallerPC*/ nullptr, /*CallerFrame*/ nullptr, /*ReturnSlot*/ VValue{}, Procedure, Callee.Self.Follow(), Callee.ParentScope.Get(),
			[&](uint32 Arg) { return GetOperand(Arguments[Arg]); });

		Task = &VTask::New(Context, NextPC, State.Frame, Task, Parent, &Frame);
		Task->AddToTaskGroup(Context);

		DEF(Op.Dest, *Task);

		UPDATE_EXECUTION_STATE(Procedure.GetOpsBegin(), &Frame);
	}
	END_OP_CASE(CallTask)

	BEGIN_OP_CASE(EndTask)
	{
		V_DIE_UNLESS(Task->bRunning);

		if (Task->Phase == VTask::EPhase::CancelRequested)
		{
			Task->SetPhaseTransactionally(VTask::EPhase::CancelStarted);
		}

		VValue Result;
		VTask* Awaiter;
		VTask* SignaledTask = nullptr;
		if (Task->Phase == VTask::EPhase::Active)
		{
			if (!Task->CancelChildren(Context))
			{
				VTask* Child = Task->LastChild.Get();
				Task->Park(Context, Child->LastCancel);
				Task->DeferOpen(Context, [Child](FAllocationContext Context, VTask* Task) {
					Task->Unpark(Context, Child->LastCancel);
				});

				NextPC = &Op;
				YIELD();
			}

			Result = GetOperand(Op.Value);
			Task->Result.SetTransactionally(Context, Result);

			// Communicate the result to the parent task, if there is one.
			if (Op.Write.Index < FRegisterIndex::UNINITIALIZED)
			{
				if (State.Registers[Op.Write.Index].Get(Context).IsUninitialized())
				{
					State.Registers[Op.Write.Index].SetTrailed(Context, _FailureContext->Trail, Result);

					// Write the task's next jump target (for early-escape support), if there is one.
					if (Op.Switch.Index < FRegisterIndex::UNINITIALIZED)
					{
						V_DIE_UNLESS(State.Registers[Op.Switch.Index].Get(Context).IsUninitialized());
						State.Registers[Op.Switch.Index].SetTrailed(Context, _FailureContext->Trail, GetOperand(Op.Which));
					}
				}
			}
			if (Op.Signal.IsRegister())
			{
				VSemaphore& Semaphore = GetOperand(Op.Signal).StaticCast<VSemaphore>();
				if (Semaphore.IncrementCount(1) == 0)
				{
					V_DIE_UNLESS(Semaphore.Await.Get());
					SignaledTask = Semaphore.Await.Get();
					Semaphore.Await.ResetTransactionally();
				}
			}

			Awaiter = Task->LastAwait.Get();
			Task->LastAwait.ResetTransactionally();
		}
		else
		{
			V_DIE_UNLESS(VTask::EPhase::CancelStarted <= Task->Phase && Task->Phase < VTask::EPhase::Canceled);

			if (!Task->CancelChildren(Context))
			{
				V_DIE_UNLESS(Task->Phase == VTask::EPhase::CancelStarted);

				NextPC = &Op;
				YIELD();
			}

			Task->SetPhaseTransactionally(VTask::EPhase::Canceled);
			Result = GlobalFalse();

			Awaiter = Task->LastCancel.Get();
			Task->LastCancel.ResetTransactionally();

			if (VTask* Parent = Task->Parent.Get())
			{
				// A canceling parent is implicitly awaiting its last child.
				if (Parent->Phase == VTask::EPhase::CancelStarted && Parent->LastChild.Get() == Task)
				{
					SignaledTask = Parent;
				}
			}
		}

		Task->ExecNativeAwaits(Context);
		Task->Suspend(Context);
		Task->RemoveFromTaskGroup(Context);
		Task->DetachTransactionally(Context);

		VTask* CompletedTask = Task;

		// This task may be resumed to run unblocked suspensions, but nothing remains to run after them.
		CompletedTask->SetResumePCTransactionally(&StopInterpreterSentry);
		CompletedTask->ResumeFrame.SetTransactionally(Context, State.Frame);

		// Switch back to the task that started or resumed this one.
		UPDATE_EXECUTION_STATE(CompletedTask->YieldPC, CompletedTask->YieldFrame.Get());
		Task = CompletedTask->YieldTask.Get();

		// Detach the task from the stack.
		CompletedTask->SetYieldPCTransactionally(&StopInterpreterSentry);
		CompletedTask->YieldFrame.SetTransactionally(Context, *VFrame::GlobalEmptyFrame);
		CompletedTask->YieldTask.ResetTransactionally();

		auto ResumeAwaiter = [&](VTask* Awaiter) {
			Awaiter->SetYieldPCTransactionally(NextPC);
			Awaiter->YieldFrame.SetTransactionally(Context, State.Frame);
			Awaiter->YieldTask.SetTransactionally(Context, Task);
			Awaiter->Resume(Context);

			UPDATE_EXECUTION_STATE(Awaiter->ResumePC, Awaiter->ResumeFrame.Get());
			if (Task == nullptr)
			{
				OutermostTask = Awaiter;
			}
			Task = Awaiter;
		};

		// Resume any awaiting (or cancelling) tasks in the order they arrived.
		// The front of the list is the most recently-awaiting task, which should run last.
		if (SignaledTask && !SignaledTask->bRunning)
		{
			ResumeAwaiter(SignaledTask);
		}
		for (VTask* PrevTask; Awaiter != nullptr; Awaiter = PrevTask)
		{
			PrevTask = Awaiter->PrevTask.Get();

			// Normal resumption of a canceling task is a no-op.
			if (Awaiter->Phase != VTask::EPhase::Active)
			{
				continue;
			}

			ResumeAwaiter(Awaiter);
			Task->ExecNativeDefer(Context);
			if (DefReturnSlotTrailed(GetTrailFor(Op), Task->ResumeSlot, Result) != ECompares::Eq)
			{
				V_DIE("Failed unifying the result of `Await` or `Cancel`");
			}
		}

		// A resumed task may already have been re-suspended or canceled.
		if (Task == nullptr || !YieldIfNeeded(NextPC))
		{
			return FOpResult::Yield;
		}
		NextPC = State.PC;
	}
	END_OP_CASE(EndTask)

	OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(BeginAwait)
	OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(AwaitSuccess)
	OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(EndAwait)

	BEGIN_OP_CASE(BeginBatch)
	{
		if (!BatchedRefs)
		{
			// This write can't be transactional.  An AutoRTFM transaction may
			// not yet be available, so a `RecordOpenWrite` would need to be
			// after requiring the effect token.  However, later `set`s may
			// need to capture the allocated value before the effect token is
			// available.
			BatchedRefs = &VBatchedRefs::New(Context);
		}
		REQUIRE_CONCRETE(EffectToken.Get(Context));
		BatchedRefs->BeginBatch();
	}
	END_OP_CASE(BeginBatch)

	BEGIN_OP_CASE(EndBatch)
	{
		V_DIE_UNLESS(BatchedRefs);
		REQUIRE_CONCRETE(EffectToken.Get(Context));
		if (!EndBatch(*BatchedRefs))
		{
			RUNTIME_ERROR();
		}
	}
	END_OP_CASE(EndBatch)

	BEGIN_OP_CASE(Yield)
	{
		NextPC = Op.ResumeOffset.GetLabeledPC();
		YIELD();
	}
	END_OP_CASE(Yield)

	BEGIN_OP_CASE(NewSemaphore)
	{
		VSemaphore& Semaphore = VSemaphore::New(Context);
		DEF(Op.Dest, Semaphore);
	}
	END_OP_CASE(NewSemaphore)

	BEGIN_OP_CASE(WaitSemaphore)
	{
		VSemaphore& Semaphore = GetOperand(Op.Source).StaticCast<VSemaphore>();
		if (Semaphore.DecrementCount(Op.Count) < 0)
		{
			V_DIE_IF(Semaphore.Await.Get());
			Semaphore.Await.SetTransactionally(Context, Task);
			YIELD();
		}
	}
	END_OP_CASE(WaitSemaphore)

	// An indexed access (i.e. `B := A[10]`) is just the same as `Call(B, A, 10)`.
	BEGIN_OP_CASE(Call)
	{
		VValue Callee = GetOperand(Op.Callee);
		REQUIRE_CONCRETE(Callee);

		TArrayView<FValueOperand> Arguments = GetOperands(Op.Arguments);
		TArrayView<TWriteBarrier<VUniqueString>> NamedArguments = GetOperands(Op.NamedArguments);
		TArrayView<FValueOperand> NamedArgumentValues = GetOperands(Op.NamedArgumentValues);

		VValue IncomingEffectToken = EffectToken.Get(Context);

		if (VFunction* Function = Callee.DynamicCast<VFunction>())
		{
			FOpResult Result = CallFunction(
				Op,
				*Function,
				Function->Self.Follow(),
				Arguments,
				NamedArguments,
				NamedArgumentValues,
				Op.Dest,
				NextPC,
				IncomingEffectToken,
				BatchedRefs);
			OP_RESULT_HELPER(Result)
		}
		else
		{
			OP_IMPL_HELPER(CallImpl, Callee);
		}
	}
	END_OP_CASE(Call)

	BEGIN_OP_CASE(CallWithSelf)
	{
		VValue Callee = GetOperand(Op.Callee);
		REQUIRE_CONCRETE(Callee);

		VValue Self = GetOperand(Op.Self);
		REQUIRE_CONCRETE(Self);

		TArrayView<FValueOperand> Arguments = GetOperands(Op.Arguments);
		TArrayView<TWriteBarrier<VUniqueString>> NamedArguments = GetOperands(Op.NamedArguments);
		TArrayView<FValueOperand> NamedArgumentValues = GetOperands(Op.NamedArgumentValues);

		VValue IncomingEffectToken = EffectToken.Get(Context);

		if (VFunction* Function = Callee.DynamicCast<VFunction>())
		{
			V_DIE_IF(Function->HasSelf());

			FOpResult Result = CallFunction(
				Op,
				*Function,
				Self,
				Arguments,
				NamedArguments,
				NamedArgumentValues,
				Op.Dest,
				NextPC,
				IncomingEffectToken,
				BatchedRefs);
			OP_RESULT_HELPER(Result)
		}
		else if (VNativeProcedure* NativeProcedure = Callee.DynamicCast<VNativeProcedure>())
		{
			// This branch is only used when invoking a native function from its wrapper procedure, if it has one.
			// This saves an extra VFunction allocation for those native functions.
			FOpResult Result = CallNativeFunction(
				Op,
				NativeProcedure,
				Self,
				Arguments,
				Op.Dest,
				NextPC,
				IncomingEffectToken,
				BatchedRefs);
			OP_RESULT_HELPER(Result)
		}
		else
		{
			V_DIE("Unsupported callee operand type: %s passed to `CallWithSelf`!", *Callee.AsCell().GetEmergentType()->Type->DebugName());
		}
	}
	END_OP_CASE(CallWithSelf) // CallWithSelf

	BEGIN_OP_CASE(Return)
	{
		DEF_RESULT_HELPER(ReturnImpl<false>(Op, NextPC));
	}
	END_OP_CASE(Return)

	BEGIN_OP_CASE(ReturnTrailed)
	{
		DEF_RESULT_HELPER(ReturnImpl<true>(Op, NextPC));
	}
	END_OP_CASE(ReturnTrailed)

	BEGIN_OP_CASE(ResumeUnwind)
	{
		BeginUnwind(NextPC);
		NextPC = State.PC;
	}
	END_OP_CASE(ResumeUnwind)

#undef OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL
#undef OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL
#undef BEGIN_OP_CASE
#undef END_OP_CASE
#undef ENQUEUE_SUSPENSION
#undef FAIL
#undef YIELD

// Macro definitions to be used in the suspension interpreter loop.
#define OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(OpName, OpImpl, ...) \
	BEGIN_OP_CASE(OpName)                                                   \
	{                                                                       \
		FOp* NextPC = nullptr;                                              \
		const VValue IncomingEffectToken = VValue::EffectDoneMarker();      \
		OP_IMPL_HELPER(OpImpl, NextPC, IncomingEffectToken, ##__VA_ARGS__); \
		DEF(Op.ReturnEffectToken, IncomingEffectToken);                     \
	}                                                                       \
	END_OP_CASE()

#define OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(OpName, OpImpl, ...)                                \
	BEGIN_OP_CASE(OpName)                                                                            \
	{                                                                                                \
		FOp* NextPC = nullptr;                                                                       \
		VValue IncomingEffectToken = GetOperand(Op.IncomingEffectToken);                             \
		FOpResult Result{FOpResult::Error};                                                          \
		V_INLINE_UNLESS_DEBUG_BUILD Result = OpImpl(Op, NextPC, IncomingEffectToken, ##__VA_ARGS__); \
		Op.IncomingEffectToken.Set(Context, IncomingEffectToken);                                    \
		OP_RESULT_HELPER(Result)                                                                     \
		DEF(Op.ReturnEffectToken, IncomingEffectToken);                                              \
	}                                                                                                \
	END_OP_CASE(OpName)

#define BEGIN_OP_CASE(Name)                                                                              \
	case EOpcode::Name:                                                                                  \
	{                                                                                                    \
		F##Name##SuspensionCaptures& Op = BytecodeSuspension.GetCaptures<F##Name##SuspensionCaptures>(); \
		if constexpr (bPrintTrace)                                                                       \
		{                                                                                                \
			BeginTrace(Op, BytecodeSuspension);                                                          \
		}

#define END_OP_CASE(Name)                                           \
	_FailureContext->Trail.Exit(Context);                           \
	FinishedExecutingSuspensionIn(*_FailureContext);                \
	if constexpr (bPrintTrace)                                      \
	{                                                               \
		EndTraceWithCaptures(Op, BytecodeSuspension, false, false); \
	}                                                               \
	break;                                                          \
	}

#define ENQUEUE_SUSPENSION(Value)                                  \
	_FailureContext->Trail.Exit(Context);                          \
	Value.EnqueueSuspension(Context, *CurrentSuspension);          \
	if constexpr (bPrintTrace)                                     \
	{                                                              \
		EndTraceWithCaptures(Op, BytecodeSuspension, true, false); \
	}                                                              \
	break

#define FAIL()                                                     \
	if constexpr (bPrintTrace)                                     \
	{                                                              \
		EndTraceWithCaptures(Op, BytecodeSuspension, false, true); \
	}                                                              \
	if (Fail(*_FailureContext) == FOpResult::Error)                \
	{                                                              \
		return FOpResult::Error;                                   \
	}                                                              \
	break

#define YIELD()                                                     \
	_FailureContext->Trail.Exit(Context);                           \
	FinishedExecutingSuspensionIn(*_FailureContext);                \
	if constexpr (bPrintTrace)                                      \
	{                                                               \
		EndTraceWithCaptures(Op, BytecodeSuspension, false, false); \
	}                                                               \
	Suspend(*Task, MakeReturnSlot(Op));                             \
	break

	// Don't inline to keep suspension handling from bloating the icache when not needed.
	template <bool bPrintTrace>
	V_PRESERVE_NONE FORCENOINLINE FOpResult::EKind DispatchSuspension(bool bHasOutermostPCBounds)
	{
		check(!!UnblockedSuspensionQueue);

		EscapeFailureContext();

		do
		{
			// Remove the suspension from the queue first- it may re-suspend onto a placeholder's queue,
			// and other suspensions may be added to the unblocked queue. These operations would corrupt
			// the queues if we tried to leave the suspension in place while running it.
			VSuspension* CurrentSuspension = UnblockedSuspensionQueue;
			UnblockedSuspensionQueue = UnblockedSuspensionQueue->Next.Get();
			CurrentSuspension->Next.Set(Context, nullptr);

			if (CurrentSuspension->FailureContext->bFailed)
			{
				continue;
			}

#if WITH_EDITORONLY_DATA
			FPackageScope PackageScope = Context.SetCurrentPackage(CurrentSuspension->CurrentPackage.Get());
#endif
			FInstantiationScope InitCtx(FInstantiationContext(CurrentSuspension->CurrentOuter.Get().ExtractUObject(), CurrentSuspension->CurrentFlags));
			TGuardValue<VFailureContext*> GuardFailureContext(_FailureContext, CurrentSuspension->FailureContext.Get());
			TGuardValue<VTask*> GuardTask(Task, CurrentSuspension->Task.Get());

			if (VLambdaSuspension* LambdaSuspension = CurrentSuspension->DynamicCast<VLambdaSuspension>())
			{
				LambdaSuspension->Callback(Context, *LambdaSuspension, UnblockedSuspensionQueue);
				continue;
			}

			VBytecodeSuspension& BytecodeSuspension = CurrentSuspension->StaticCast<VBytecodeSuspension>();

			switch (BytecodeSuspension.Opcode)
			{
				OP(Add)
				OP(Sub)
				OP(Mul)
				OP(Div)
				OP(Mod)
				OP(Neg)

				OP(MutableAdd)

				OP(Neq)
				OP(Lt)
				OP(Lte)
				OP(Gt)
				OP(Gte)

				OP_WITH_IMPL(LtFastFail, LtFastFailSuspensionImpl)
				OP_WITH_IMPL(LteFastFail, LteFastFailSuspensionImpl)
				OP_WITH_IMPL(GtFastFail, GtFastFailSuspensionImpl)
				OP_WITH_IMPL(GteFastFail, GteFastFailSuspensionImpl)
				OP_WITH_IMPL(EqFastFail, EqFastFailSuspensionImpl)
				OP_WITH_IMPL(NeqFastFail, NeqFastFailSuspensionImpl)
				OP_WITH_IMPL(ArrayIndexFastFail, ArrayIndexFastFailSuspensionImpl)
				OP_WITH_IMPL(TypeCastFastFail, TypeCastFastFailSuspensionImpl)
				OP_WITH_IMPL(QueryFastFail, QueryFastFailSuspensionImpl)

				OP(Query)

				OP(Melt)
				OP_IMPL_CAPTURES_EFFECT_TOKENS(Freeze, Op.BatchedRefs.Get())

				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(RefGet)
				OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(RefSet, RefSetImpl, Op.BatchedRefs.Get(), {})
				OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(RefSetLive, RefSetImpl, Op.BatchedRefs.Get(), GetOperand(Op.Task))
				OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(SetField, SetFieldImpl, Op.BatchedRefs.Get(), {})
				OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(SetFieldLive, SetFieldImpl, Op.BatchedRefs.Get(), GetOperand(Op.Task))
				OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(CallSet, CallSetImpl, Op.BatchedRefs.Get(), {})
				OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(CallSetLive, CallSetImpl, Op.BatchedRefs.Get(), GetOperand(Op.Task))
				OP_IMPL_CAPTURES_EFFECT_TOKENS(RefCallDomain, Op.BatchedRefs.Get())

				OP_WITH_IMPL(CanFastAppendToArrayFastFail, CanFastAppendToArrayFastFailSuspensionImpl)
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(FastAppendToArray)

				OP(Length)
				OP(NewMutableArrayWithCapacity)
				OP(ArrayAdd)
				OP(InPlaceMakeImmutable)
				OP(NewMap)
				OP(MapKey)
				OP(MapValue)

				OP(NewClass)
				OP(BindNativeClass)
				OP(EndModuleData)

				OP(LoadConstructor)
				OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(NewObject, NewObjectImpl, Op.BatchedRefs.Get())
				OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL(NewObjectICClass, NewObjectImpl, Op.BatchedRefs.Get())
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(CreateField, CreateFieldSuspensionImpl)
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(CreateFieldICValueObjectConstant, CreateFieldSuspensionImpl)
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(CreateFieldICValueObjectField, CreateFieldSuspensionImpl)
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(CreateFieldICNativeStruct, CreateFieldSuspensionImpl)
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(CreateFieldICUObject, CreateFieldSuspensionImpl)
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL(UnifyField, UnifyFieldImpl, EExecutionLoopKind::Suspension)
				OP_IMPL_CAPTURES_EFFECT_TOKENS(InitializeVar, Op.BatchedRefs.Get(), EExecutionLoopKind::Suspension)
				OP_IMPL_CAPTURES_EFFECT_TOKENS(UnifyNativeObject, Op.BatchedRefs.Get())
				OP(UnwrapNativeConstructorWrapper)

				OP_WITH_IMPL(LoadField, LoadFieldImpl)
				OP_WITH_IMPL(LoadFieldICOffset, LoadFieldImpl)
				OP_WITH_IMPL(LoadFieldICConstant, LoadFieldImpl)
				OP_WITH_IMPL(LoadFieldICFunction, LoadFieldImpl)
				OP_WITH_IMPL(LoadFieldICAccessor, LoadFieldImpl)
				OP(LoadFieldFromSuper)

				OP(NewScope)
				OP(NewFunction)
				OP(LoadParentScope)
				OP(LoadCapture)

				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(BeginAwait)
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(AwaitSuccess)
				OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN(EndAwait)

				// An indexed access (i.e. `B := A[10]`) is just the same as `Call(B, A, 10)`.
				BEGIN_OP_CASE(Call)
				{
					VValue Callee = GetOperand(Op.Callee);
					REQUIRE_CONCRETE(Callee);

					TArrayView<TWriteBarrier<VValue>> Arguments = GetOperands(Op.Arguments);
					TArrayView<TWriteBarrier<VUniqueString>> NamedArguments(Op.NamedArguments);
					TArrayView<TWriteBarrier<VValue>> NamedArgumentValues = GetOperands(Op.NamedArgumentValues);

					VValue IncomingEffectToken = GetOperand(Op.IncomingEffectToken);

					FOp* NextPC = nullptr;
					FOpResult Result = FOpResult::Error;
					if (VFunction* Function = Callee.DynamicCast<VFunction>())
					{
						Result = CallFunction(
							Op,
							*Function,
							Function->Self.Follow(),
							Arguments,
							NamedArguments,
							NamedArgumentValues, Op.Dest,
							NextPC,
							IncomingEffectToken,
							Op.BatchedRefs.Get());
					}
					else
					{
						Result = CallImpl(Op, Callee);
					}
					switch (Result.Kind)
					{
						case FOpResult::Return:
						case FOpResult::Yield:
							DEF(Op.ReturnEffectToken, IncomingEffectToken);
							break;

						case FOpResult::Block:
						case FOpResult::Fail:
						case FOpResult::Error:
							break;
					}
					OP_RESULT_HELPER(Result)
				}
				END_OP_CASE(Call)

				BEGIN_OP_CASE(CallWithSelf)
				{
					VValue Callee = GetOperand(Op.Callee);
					REQUIRE_CONCRETE(Callee);

					VValue Self = GetOperand(Op.Self);
					REQUIRE_CONCRETE(Self);

					TArrayView<TWriteBarrier<VValue>> Arguments = GetOperands(Op.Arguments);
					TArrayView<TWriteBarrier<VUniqueString>> NamedArguments(Op.NamedArguments);
					TArrayView<TWriteBarrier<VValue>> NamedArgumentValues = GetOperands(Op.NamedArgumentValues);

					VValue IncomingEffectToken = GetOperand(Op.IncomingEffectToken);

					FOp* NextPC = nullptr;
					FOpResult Result = FOpResult::Error;
					if (VFunction* Function = Callee.DynamicCast<VFunction>())
					{
						V_DIE_IF(Function->HasSelf());

						Result = CallFunction(
							Op,
							*Function,
							Self,
							Arguments,
							NamedArguments,
							NamedArgumentValues,
							Op.Dest,
							NextPC,
							IncomingEffectToken,
							Op.BatchedRefs.Get());
					}
					else if (VNativeProcedure* NativeProcedure = Callee.DynamicCast<VNativeProcedure>())
					{
						// This branch is only used when invoking a native function from its wrapper procedure, if it has one.
						// This saves an extra VFunction allocation for those native functions.
						Result = CallNativeFunction(
							Op,
							NativeProcedure,
							Self,
							Arguments,
							Op.Dest,
							NextPC,
							IncomingEffectToken,
							Op.BatchedRefs.Get());
					}
					else
					{
						V_DIE("Unsupported operand passed to `CallWithSelf`!");
					}
					switch (Result.Kind)
					{
						case FOpResult::Return:
						case FOpResult::Yield:
							DEF(Op.ReturnEffectToken, IncomingEffectToken);
							break;

						case FOpResult::Block:
						case FOpResult::Fail:
						case FOpResult::Error:
							break;
					}
					OP_RESULT_HELPER(Result)
				}
				END_OP_CASE(CallWithSelf) // CallWithSelf

				BEGIN_OP_CASE(BeginBatch)
				{
					V_DIE_UNLESS(Op.BatchedRefs);
					Op.BatchedRefs->BeginBatch();
				}
				END_OP_CASE(BeginBatch)

				BEGIN_OP_CASE(EndBatch)
				{
					V_DIE_UNLESS(Op.BatchedRefs);
					if (!EndBatch(*Op.BatchedRefs))
					{
						RUNTIME_ERROR();
					}
				}
				END_OP_CASE(EndBatch)

				default:
					V_DIE("Invalid opcode: %u", static_cast<FOpcodeInt>(State.PC->Opcode));
			}
		}
		while (UnblockedSuspensionQueue);

		if (!UnwindIfNeeded())
		{
			return FOpResult::Fail;
		}
		if (!YieldIfNeeded(State.PC))
		{
			return FOpResult::Yield;
		}

		[[clang::musttail]] return Dispatch<bPrintTrace>(bHasOutermostPCBounds);
	}

#undef OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN
#undef OP_IMPL_CAPTURES_RETURN_EFFECT_TOKEN_WITH_IMPL
#undef OP_IMPL_CAPTURES_EFFECT_TOKENS
#undef OP_IMPL_CAPTURES_EFFECT_TOKENS_WITH_IMPL
#undef BEGIN_OP_CASE
#undef END_OP_CASE
#undef ENQUEUE_SUSPENSION
#undef FAIL
#undef YIELD
#undef RUNTIME_ERROR
#undef RAISE_RUNTIME_ERROR_CODE
#undef RAISE_RUNTIME_ERROR
#undef RAISE_RUNTIME_ERROR_FORMAT
#undef OP_RESULT_HELPER

public:
	FInterpreter(
		FRunningContext Context,
		FExecutionState State,
		VValue IncomingEffectToken,
		VFailureContext* FailureContext,
		VTask* Task,
		VBatchedRefs* BatchedRefs,
		FOp* EndPC = nullptr)
		: Context(Context)
		, State(State)
		, EffectToken(Context, IncomingEffectToken)
		, OutermostFailureContext(FailureContext)
		, OutermostTask(Task)
		, OutermostEndPC(EndPC)
		, Task(Task)
		, BatchedRefs(BatchedRefs)
		, _FailureContext(FailureContext)
	{
		V_DIE_UNLESS(OutermostFailureContext);
	}

	~FInterpreter()
	{
		V_DIE_IF(UnblockedSuspensionQueue);
	}

	FOpResult::EKind Execute()
	{
		V_DIE_UNLESS(AutoRTFM::IsTransactional());

		if (CVarTrace.GetValueOnAnyThread())
		{
			if (OutermostEndPC)
			{
				return Dispatch<true>(true);
			}
			else
			{
				return Dispatch<true>(false);
			}
		}
		else
		{
			if (OutermostEndPC)
			{
				return Dispatch<false>(true);
			}
			else
			{
				return Dispatch<false>(false);
			}
		}
	}

	static FOpResult InvokeWithSelf(FRunningContext Context, VFunction& Function, VValue Self, VFunction::Args&& IncomingArguments, TArrayView<TWriteBarrier<VUniqueString>> NamedArgs = {}, TArrayView<VValue> NamedArgVals = {}, bool bRequireConcreteEffectToken = true)
	{
		if (VNativeProcedure* NativeProcedure = Function.Procedure.Get().DynamicCast<VNativeProcedure>())
		{
			return NativeProcedure->Thunk(Context, Self, IncomingArguments);
		}
		VProcedure& Procedure = Function.Procedure.Get().StaticCast<VProcedure>();

		VRestValue ReturnSlot(0);

		// The Return instruction trails its write to ReturnSlot. If we backtrack after returning from InvokeWithSelf,
		// this will corrupt the stack. Calling Get materializes a VPlaceholder, which can be trailed safely.
		ReturnSlot.Get(Context);

		VFunction::Args Arguments = MoveTemp(IncomingArguments);

		FOp* CallerPC = &StopInterpreterSentry;
		VFrame* CallerFrame = nullptr;
		VFrame& Frame = MakeFrameForCallee(
			Context, CallerPC, CallerFrame, &ReturnSlot, Procedure, Self, Function.ParentScope.Get(),
			Arguments.Num(), NamedArgs,
			[&](uint32 Arg) { return Arguments[Arg]; },
			[&](uint32 NamedArg) { return NamedArgVals[NamedArg]; });

		// Check if we're inside native C++ code that was invoked by Verse
		FNativeFrame* NativeFrame = Context.NativeFrame();
		V_DIE_UNLESS(NativeFrame);

		FInterpreter Interpreter(
			Context,
			FExecutionState(Procedure.GetOpsBegin(), &Frame),
			NativeFrame->EffectToken,
			NativeFrame->FailureContext,
			NativeFrame->Task,
			NativeFrame->BatchedRefs);

		FOpResult::EKind Result = Interpreter.Execute();

		// We must have a concrete effect token when returning to C++, unless that C++ is the VM or never had one to begin with.
		if (bRequireConcreteEffectToken && !NativeFrame->EffectToken.IsPlaceholder())
		{
			V_DIE_IF(Interpreter.EffectToken.Get(Context).IsPlaceholder());
		}
		NativeFrame->EffectToken = Interpreter.EffectToken.Get(Context);

		if (ShouldTrace())
		{
			UE_LOGF(LogVerseVM, Display, "\n");
		}

		if constexpr (DoStats)
		{
			UE_LOGF(LogVerseVM, Display, "Num Transactions: %lf", TotalNumFailureContexts);
			UE_LOGF(LogVerseVM, Display, "Num Reuses: %lf", NumReuses);
			UE_LOGF(LogVerseVM, Display, "Hit rate: %lf", NumReuses / TotalNumFailureContexts);
		}

		return {Result, Result == FOpResult::Return ? ReturnSlot.Get(Context) : VValue()};
	}

	template <typename TFunction>
	static FOpResult Spawn(FRunningContext Context, TFunction F)
	{
		FNativeFrame* NativeFrame = Context.NativeFrame();
		V_DIE_UNLESS(NativeFrame);

		VTask* Task = &VTask::New(Context, &StopInterpreterSentry, VFrame::GlobalEmptyFrame.Get(), /*YieldTask*/ nullptr, /*Parent*/ nullptr);
		Task->AddToTaskGroup(Context);

		VTask::FCallerSpec CallerSpec = VTask::MakeFrameForSpawn(Context);

		FOpResult::EKind Result = FOpResult::Return;

		Task->Suspend(Context); // So that Call.Return invokes Task->Resume()
		Task->SetResumePCTransactionally(CallerSpec.PC);
		Task->ResumeFrame.SetTransactionally(Context, CallerSpec.Frame);
		Task->ResumeSlot.SetTransactionally(Context, CallerSpec.ReturnSlot);

		Context.PushNativeFrame(
			NativeFrame->FailureContext,
			Task,
			NativeFrame->BatchedRefs,
			NativeFrame->EffectToken,
			NativeFrame->Callee,
			CallerSpec.PC,
			CallerSpec.Frame,
			[&] { Result = F().Kind; });

		return {Result, *Task};
	}

	static FOpResult Spawn(FRunningContext Context, VFunction& Function, VFunction::Args&& IncomingArguments, TArrayView<TWriteBarrier<VUniqueString>> NamedArgs, TArrayView<VValue> NamedArgVals)
	{
		FNativeFrame* NativeFrame = Context.NativeFrame();
		V_DIE_UNLESS(NativeFrame);

		VTask* Task = &VTask::New(Context, &StopInterpreterSentry, VFrame::GlobalEmptyFrame.Get(), /*YieldTask*/ nullptr, /*Parent*/ nullptr);
		Task->AddToTaskGroup(Context);

		VTask::FCallerSpec CallerSpec = VTask::MakeFrameForSpawn(Context);

		VFunction::Args Arguments = MoveTemp(IncomingArguments);

		FOpResult::EKind Result = FOpResult::Return;
		if (VProcedure* Procedure = Function.Procedure.Get().DynamicCast<VProcedure>())
		{
			VFrame& Frame = MakeFrameForCallee(
				Context, CallerSpec.PC, CallerSpec.Frame, CallerSpec.ReturnSlot, *Procedure, Function.Self.Get(), Function.ParentScope.Get(),
				Arguments.Num(), NamedArgs,
				[&](uint32 Arg) { return Arguments[Arg]; },
				[&](uint32 NamedArg) { return NamedArgVals[NamedArg]; });

			FInterpreter Interpreter(
				Context,
				FExecutionState(Procedure->GetOpsBegin(), &Frame),
				NativeFrame->EffectToken,
				NativeFrame->FailureContext,
				Task,
				NativeFrame->BatchedRefs);

			Result = Interpreter.Execute();

			// We must have a concrete effect token when returning to C++, unless we never had one to begin with.
			if (!NativeFrame->EffectToken.IsPlaceholder())
			{
				V_DIE_IF(Interpreter.EffectToken.Get(Context).IsPlaceholder());
			}
			NativeFrame->EffectToken = Interpreter.EffectToken.Get(Context);
		}
		else if (VNativeProcedure* NativeCallee = Function.Procedure.Get().DynamicCast<VNativeProcedure>())
		{
			V_DIE_IF(!NamedArgs.IsEmpty());

			Task->Suspend(Context); // So that Call.Return invokes Task->Resume()
			Task->SetResumePCTransactionally(CallerSpec.PC);
			Task->ResumeFrame.SetTransactionally(Context, CallerSpec.Frame);
			Task->ResumeSlot.SetTransactionally(Context, CallerSpec.ReturnSlot);

			Context.PushNativeFrame(
				NativeFrame->FailureContext,
				Task,
				NativeFrame->BatchedRefs,
				NativeFrame->EffectToken,
				NativeCallee,
				CallerSpec.PC,
				CallerSpec.Frame,
				[&] { Result = NativeCallee->Thunk(Context, Function.Self.Get(), Arguments).Kind; });
		}

		if (ShouldTrace())
		{
			UE_LOGF(LogVerseVM, Display, "\n");
		}

		// TODO: `spawn->native function` calls are not filling in the 'native return value' which causes failure to be returned from the VNI glue.
		//		 This should be fixed then we can enable this check again for soundness. For now we just return the task regardless.
		//
		// We expect Result here to be either Return (the callee completed), Yield (the callee suspended), or Error (a runtime error occurred)
		// V_DIE_IF(Result == FOpResult::Fail || Result == FOpResult::Block);

		return {Result, *Task};
	}

	static FOpResult::EKind Resume(FRunningContext Context, VValue ResumeArgument, VTask& Task, FOp* AwaitPC = nullptr)
	{
		FNativeFrame* NativeFrame = Context.NativeFrame();
		V_DIE_UNLESS(NativeFrame);

		if (Task.Phase != VTask::EPhase::Active)
		{
			return FOpResult::Return;
		}

		if (ShouldTrace())
		{
			UE_LOGF(LogVerseVM, Display, "");
			UE_LOGF(LogVerseVM, Display, "Resuming:");
		}

		checkSlow(Task.YieldFrame == VFrame::GlobalEmptyFrame.Get());
		checkSlow(Task.YieldTask == nullptr);
		Task.Resume(Context);
		if (AwaitPC)
		{
			Task.AwaitPC = AwaitPC;
		}

		FInterpreter Interpreter(
			Context,
			FExecutionState{Task.ResumePC, Task.ResumeFrame.Get()},
			NativeFrame->EffectToken,
			NativeFrame->FailureContext,
			&Task,
			NativeFrame->BatchedRefs);

		Task.ExecNativeDefer(Context);

		bool bExecute = true;
		switch (Interpreter.DefReturnSlotTrailed(Interpreter.FailureContext()->Trail, Task.ResumeSlot, ResumeArgument))
		{
			case ECompares::Eq:
				break;
			case ECompares::Neq:
				Interpreter.Fail(*Interpreter.FailureContext());
				bExecute = Interpreter.UnwindIfNeeded();
				break;
			case ECompares::Undecidable:
				V_DIE("Undecidable unification");
			case ECompares::RuntimeError:
				return FOpResult::Error;
			default:
				VERSE_UNREACHABLE();
		}

		FOpResult::EKind Result = FOpResult::Return;
		if (bExecute)
		{
			Result = Interpreter.Execute();
		}

		// We must have a concrete effect token when returning to C++, unless we never had one to begin with.
		if (!NativeFrame->EffectToken.IsPlaceholder())
		{
			V_DIE_IF(Interpreter.EffectToken.Get(Context).IsPlaceholder());
		}
		NativeFrame->EffectToken = Interpreter.EffectToken.Get(Context);

		V_DIE_IF(Result == FOpResult::Fail);
		return Result;
	}

	static FOpResult::EKind Unwind(FRunningContext Context, VTask& Task)
	{
		FNativeFrame* NativeFrame = Context.NativeFrame();
		V_DIE_UNLESS(NativeFrame);

		V_DIE_UNLESS(Task.Phase == VTask::EPhase::CancelStarted && !Task.LastChild);

		if (ShouldTrace())
		{
			UE_LOGF(LogVerseVM, Display, "");
			UE_LOGF(LogVerseVM, Display, "Unwinding:");
		}

		Task.Resume(Context);

		FInterpreter Interpreter(
			Context,
			FExecutionState(Task.ResumePC, Task.ResumeFrame.Get()),
			NativeFrame->EffectToken,
			NativeFrame->FailureContext,
			&Task,
			NativeFrame->BatchedRefs);

		Interpreter.BeginUnwind(Interpreter.State.PC);
		FOpResult::EKind Result = Interpreter.Execute();

		NativeFrame->EffectToken = Interpreter.EffectToken.Get(Context);

		V_DIE_IF(Result == FOpResult::Fail);
		return Result;
	}
};

FOpResult VFunction::InvokeWithSelf(FRunningContext Context, VValue InSelf, Args&& Arguments, TArrayView<TWriteBarrier<VUniqueString>> NamedArgs, TArrayView<VValue> NamedArgVals, bool bRequireConcreteEffectToken)
{
	return FInterpreter::InvokeWithSelf(Context, *this, InSelf, MoveTemp(Arguments), NamedArgs, NamedArgVals, bRequireConcreteEffectToken);
}

FOpResult VFunction::InvokeWithSelf(FRunningContext Context, VValue InSelf, VValue Argument, TWriteBarrier<VUniqueString>* NamedArg)
{
	if (NamedArg)
	{
		TArray<TWriteBarrier<VUniqueString>> NamedArgs{*NamedArg};
		Args NamedArgVals{Argument};
		return FInterpreter::InvokeWithSelf(Context, *this, InSelf, VFunction::Args{Argument}, NamedArgs, NamedArgVals);
	}
	return FInterpreter::InvokeWithSelf(Context, *this, InSelf, VFunction::Args{Argument});
}

FOpResult VFunction::Spawn(FRunningContext Context, Args&& Arguments, TArrayView<TWriteBarrier<VUniqueString>> NamedArgs, TArrayView<VValue> NamedArgVals)
{
	return FInterpreter::Spawn(Context, *this, MoveTemp(Arguments), NamedArgs, NamedArgVals);
}

FOpResult::EKind VTask::Resume(FRunningContext Context, VValue ResumeArgument)
{
	return FInterpreter::Resume(Context, ResumeArgument, *this);
}

FOpResult::EKind VTask::Unwind(FRunningContext Context)
{
	return FInterpreter::Unwind(Context, *this);
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
