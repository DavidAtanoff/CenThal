// Copyright Epic Games, Inc. All Rights Reserved.
// uLang Compiler Public API

#pragma once

#include "AutoRTFM/Defines.h"
#include "uLang/Common/Containers/SharedPointer.h"
#include "uLang/Common/Containers/SharedPointerArray.h"
#include "uLang/Common/Text/Symbol.h"
#include "uLang/Common/Text/UTF8String.h"
#include "uLang/Semantics/DataDefinition.h"
#include "uLang/Semantics/Effects.h"
#include "uLang/Semantics/SemanticTypes.h"
#include "uLang/Semantics/Signature.h"
#include "uLang/SourceProject/PackageRole.h"
#include "uLang/Syntax/VstNode.h"

#define UE_API VERSECOMPILER_API

namespace uLang
{

// Forward declarations
class CAstNode;
class CAstCompilationUnit;
class CCaptureScope;
class CControlScope;
class CEnumeration;
class CEnumerator;
class CExpressionBase;
class CExprCodeBlock;
class CExprIdentifierBase;
class CSemanticProgram;
class CSnippet;
class CModule;
class CModulePart;
class CModuleAlias;
class CSemanticProgram;
class CSnippet;
class CTypeAlias;
class CTypeVariable;

// NOTE: (YiLiangSiew) Currently, Visual Verse relies on the numerical values of these enumerations. If you
// change this, be sure to update `BaseVisualVerseSettings.ini` as well.
#define VERSE_VISIT_AST_NODE_TYPES(v) \
    /* Helper expressions */ \
    v(Error_, CExprError)                /* An expression that could not be analyzed due to an error. Attempts to analyze should propagate the error. */ \
    v(Placeholder_, CExprPlaceholder)    /* Corresponds to a Placeholder Vst node. */ \
    v(Noop_, CNoop)                      /* Nice to have when something should be ignored. */ \
    v(External, CExprExternal)           /* An (unknown) external expression - should never reach the code generator */ \
    v(PathPlusSymbol, CExprPathPlusSymbol) /* Path of the current scope plus a symbol - /unrealengine.com/UnrealEngine@5 */ \
    \
    /* Literals */ \
    v(Literal_Logic, CExprLogic)              /* Logic literal - true/false */ \
    v(Literal_Number, CExprNumber)            /* Integer literal - 42, 0, -123, 123_456_789, 0x12fe, 0b101010 */ \
                                              /* or Float literal - 42.0, 0.0, -123.0, 123_456.0, 3.14159, .5, -.33, 4.2e1, -1e6, 7.5e-8 */ \
    v(Literal_Char, CExprChar)                /* Character literal - 'a', '\n' */ \
    v(Literal_String, CExprString)            /* String literal - "Hello, world!", "Line 1\nLine2" */ \
    v(Literal_Path, CExprPath)                /* Path literal - /unrealengine.com/UnrealEngine@5 */ \
    v(Literal_Enum, CExprEnumLiteral)         /* Enumerator - Color.Red, Size.XXL */ \
    v(Literal_Type, CExprType)                /* Type - type{<expr>} */ \
    v(Literal_Function, CExprFunctionLiteral) /* a=>b or function(a){b} */ \
    \
    /* Identifiers */ \
    v(Identifier_Unresolved, CExprIdentifierUnresolved)                 /* An existing identifier that is unresolved. It is produced by desugaring and consumed by analysis. */ \
    v(Identifier_Class, CExprIdentifierClass)                           /* Type identifier - e.g. my_type, int, color, string */ \
    v(Identifier_Module, CExprIdentifierModule)                         /* Module name */ \
    v(Identifier_ModuleAlias, CExprIdentifierModuleAlias)               /* Module alias name */ \
    v(Identifier_Enum, CExprEnumerationType)                            /* Enum name */ \
    v(Identifier_Interface, CExprInterfaceType)                         /* Interface name */ \
    v(Identifier_Data, CExprIdentifierData)                             /* Scoped data-definition (class member, local, etc.) */ \
    v(Identifier_TypeAlias, CExprIdentifierTypeAlias)                   /* Access to type alias */ \
    v(Identifier_TypeVariable, CExprIdentifierTypeVariable)             /* Access to a type variable */ \
    v(Identifier_Function, CExprIdentifierFunction)                     /* Access to functions */ \
    v(Identifier_OverloadedFunction, CExprIdentifierOverloadedFunction) /* An overloaded function identifier that hasn't been resolved to a specific overload. */ \
    v(Identifier_Self, CExprSelf)                                       /* Access to the instance the current function is being invoked on. */ \
    v(Identifier_Local, CExprLocal)                                     /* Access to the the current scope for the identifier that has this qualifier. */ \
    v(Identifier_BuiltInMacro, CExprIdentifierBuiltInMacro)             /* An intrinsic macro: option{}, logic{}, array{}, etc. */ \
    \
    /* Multi purpose syntax */ \
    v(Definition, CExprDefinition) /* represents syntactic forms elt:domain=value, elt:domain, elt=value */ \
    \
    /* Macro */ \
    v(MacroCall, CExprMacroCall) /* macro invocations should be resolved by the compiler; the code generator should never encounter this. */ \
    \
    /* Invocations */ \
    v(Invoke_Invocation, CExprInvocation)                 /* Routine call - expr1.call(expr2, expr3) */ \
    v(Invoke_UnaryArithmetic, CExprUnaryArithmetic)       /* negation */ \
    v(Invoke_BinaryArithmetic, CExprBinaryArithmetic)     /* add, sub, mul, div; two operands only */ \
    v(Invoke_ShortCircuitAnd, CExprShortCircuitAnd)       /* short-circuit evaluation of logic and */ \
    v(Invoke_ShortCircuitOr, CExprShortCircuitOr)         /* short-circuit evaluation of logic or */ \
    v(Invoke_LogicalNot, CExprLogicalNot)                 /* logical not operator */ \
    v(Invoke_Comparison, CExprComparison)                 /* comparison operators */ \
    v(Invoke_QueryValue, CExprQueryValue)                 /* Querying the value of a logic or option. */ \
    v(Invoke_MakeOption, CExprMakeOption)                 /* Making an option value. */ \
    v(Invoke_MakeArray, CExprMakeArray)                   /* Making an array value. */ \
    v(Invoke_MakeMap, CExprMakeMap)                       /* Making a map value. */ \
    v(Invoke_MakeTuple, CExprMakeTuple)                   /* Making a tuple value - (1, 2.0f, "three") */ \
    v(Invoke_TupleElement, CExprTupleElement)             /* Tuple element access `TupleExpr(Idx)` */ \
    v(Invoke_MakeRange, CExprMakeRange)                   /* Making a range value. */ \
    v(Invoke_Type, CExprInvokeType)                       /* Invoke a type as a function on a value - type(expr) or type[expr]. */ \
    v(Invoke_PointerToReference, CExprPointerToReference) /* Access the mutable reference behind the pointer. */ \
    v(Invoke_Set, CExprSet)                               /* Evaluate operand to an l-expression. */ \
    v(Invoke_NewPointer, CExprNewPointer)                 /* Create a new pointer from an initial value. */ \
    v(Invoke_ReferenceToValue, CExprReferenceToValue)     /* Evaluates the value of an expression yielding a reference type. */ \
    \
    v(Assignment, CExprAssignment) /* Assignment operation - expr1 = expr2, expr1 := expr2, expr1 += expr2, etc. */ \
    \
    /* TypeFormers */ \
    v(Invoke_ArrayFormer, CExprArrayTypeFormer)                         /* Invoke (at compile time) a formation of an array of another type */ \
    v(Invoke_GeneratorFormer, CExprGeneratorTypeFormer)                 /* Invoke (at compile time) a formation of an generator type. */ \
    v(Invoke_MapFormer, CExprMapTypeFormer)                             /* Invoke (at compile time) a formation of a map from a key and value type. */ \
    v(Invoke_OptionFormer, CExprOptionTypeFormer)                       /* Invoke (at compile time) a formation of an option of some primitive type */ \
    v(Invoke_Subtype, CExprSubtype)                                     /* Invoke (at compile time) a formation of a metaclass type. */ \
    v(Invoke_TupleType, CExprTupleType)                                 /* Get or create a tuple based on `tuple(type1, type2, ...)` */ \
    v(Invoke_Arrow, CExprArrow)                                         /* Create a function type from a parameter and return type. */ \
    \
    v(Invoke_ArchetypeInstantiation, CExprArchetypeInstantiation) /* Initializer list style instantiation - Type{expr1, id=expr2, ...} */ \
    \
    /* Flow Control */ \
    v(Flow_CodeBlock, CExprCodeBlock)       /* Code block - block {expr1; expr2} */ \
    v(Flow_Let, CExprLet)                   /* let {definition1; definition2} */ \
    v(Flow_Defer, CExprDefer)               /* defer {expr1; expr2} */ \
    v(Flow_If, CExprIf)                     /* Conditional with failable tests- if (Test[]) {clause1}, if (Test[]) {clause1} else {else_clause} */ \
    v(Flow_Iteration, CExprIteration)       /* Bounded iteration over an iterable type - for(Num:Nums, ...) {DoStuff(Num, ...)} */ \
    v(Flow_First, CExprFirst)               /* Find first over an iterable type - first(Num:Nums, ...) {DoStuff(Num, ...)} */ \
    v(Flow_Loop, CExprLoop)                 /* Simple loop - loop {DoStuff()} */ \
    v(Flow_Break, CExprBreak)               /* Control flow early exit - loop {if (IsEarlyExit[]) {break}; DoLoopStuff()} */ \
    v(Flow_Return, CExprReturn)             /* Return statement - return expr */ \
    v(Flow_ProfileBlock, CExprProfileBlock) /* profile block */\
    \
    v(Ir_AllOne, CIrAllOne)                         /* Bounded iteration over an iterable type - either for(Num:Nums) {DoStuff(Num)} or first (Num:Nums) {DoStuff(Num)} */ \
    v(Ir_For, CIrFor)                               /* Bounded iteration over an iterable type - for(Num:Nums) {DoStuff(Num)} */ \
    v(Ir_IteratedBody, CIrIteratedBody)             /* Used inside CIrFor when implementing for and first, and only around the body of the innermost CIrFor */ \
    v(Ir_ArrayAdd, CIrArrayAdd)                     /* Append an item to an array */ \
    v(Ir_MapAdd, CIrMapAdd)                         /* Add a key-value pair to a map */ \
    v(Ir_ArrayUnsafeCall, CIrArrayUnsafeCall)       /* Infallibly access an element of an array */ \
    v(Ir_ConvertToDynamic, CIrConvertToDynamic)     /* Converts a value to a dynamically typed value. */ \
    v(Ir_ConvertFromDynamic, CIrConvertFromDynamic) /* Converts a value from a dynamically typed value. */ \
    v(Ir_DataOfDynamic, CIrDataOfDynamic)           /* Access data pointer of dynamically typed value. */ \
    \
    /* Concurrency Primitives */ \
    v(Concurrent_Sync, CExprSync)                  /* sync {Coro1(); Coro2()} */ \
    v(Concurrent_Rush, CExprRush)                  /* rush {Coro1(); Coro2()} */ \
    v(Concurrent_Race, CExprRace)                  /* race {Coro1(); Coro2()} */ \
    v(Concurrent_SyncIterated, CExprSyncIterated)  /* sync(Item:Container) {Item.Coro1(); Coro2(Item)} */ \
    v(Concurrent_RushIterated, CExprRushIterated)  /* rush(Item:Container) {Item.Coro1(); Coro2(Item)} */ \
    v(Concurrent_RaceIterated, CExprRaceIterated)  /* race(Item:Container) {Item.Coro1(); Coro2(Item)} */ \
    v(Concurrent_Branch, CExprBranch)              /* branch {Coro1(); Coro2()} */ \
    v(Concurrent_Spawn, CExprSpawn)                /* spawn {Coro()} */ \
    v(Concurrent_Await, CExprAwait)                /* await {E} */ \
    v(Concurrent_Batch, CExprBatch)                /* batch {E} */ \
    v(Concurrent_Upon, CExprUpon)                  /* upon(E1) {E2} */ \
    v(Concurrent_When, CExprWhen)                  /* when(E1) {E2} */ \
    \
    /* Definitions */ \
    v(Definition_Module, CExprModuleDefinition) \
    v(Definition_Enum, CExprEnumDefinition) \
    v(Definition_Interface, CExprInterfaceDefinition) \
    v(Definition_Class, CExprClassDefinition) \
    v(Definition_Data, CExprDataDefinition) \
    v(Definition_IterationPair, CExprIterationPairDefinition) \
    v(Definition_Function, CExprFunctionDefinition) \
    v(Definition_TypeAlias, CExprTypeAliasDefinition) \
    v(Definition_Using, CExprUsing) \
    v(Definition_Import, CExprImport) \
    v(Definition_Where, CExprWhere) \
    v(Definition_Var, CExprVar) \
    v(Definition_Live, CExprLive) \
    v(Definition_ScopedAccessLevel, CExprScopedAccessLevelDefinition) \
    v(Invoke_MakeNamed, CExprMakeNamed)        /* ?ParamName:=Value pair - or default value placeholder (value not set) */ \
    \
    /* Containing Context - may contain expressions though they aren't expressions themselves */ \
    v(Context_Project, CAstProject) \
    v(Context_CompilationUnit, CAstCompilationUnit) \
    v(Context_Package, CAstPackage) \
    v(Context_Snippet, CExprSnippet)

#define FORWARD_DECLARE(_, Class) class Class;
VERSE_VISIT_AST_NODE_TYPES(FORWARD_DECLARE)
#undef FORWARD_DECLARE

/**
 * This is used to differentiate between different types of AST nodes when it is only
 * known that an instance is of type CAstNode, but not the specific subclass.
 * It is returned by the method CAstNode::GetNodeType()
 **/
enum class EAstNodeType : uint8_t
{
#define VISIT_AST_NODE_TYPE(Name, Class) Name,
    VERSE_VISIT_AST_NODE_TYPES(VISIT_AST_NODE_TYPE)
#undef VISIT_AST_NODE_TYPE
};

struct SAstNodeTypeInfo
{
    const char* _EnumeratorName;
    const char* _CppClassName;
};

/** Returns the name of an AST node type. */
VERSECOMPILER_API SAstNodeTypeInfo GetAstNodeTypeInfo(uLang::EAstNodeType Type);

//---------------------------------------------------------------------------------------
// Indicates whether an expression should return immediately - such as functions, after a
// duration (including immediately) such as coroutines or either.
enum class EInvokeTime : uint8_t
{
    Immediate  = 1 << 0, // May only call an immediate expression (such as a function call) and any async expression (such as a coroutine call) should result in an error.
    Async      = 1 << 1, // May only call an async expression (such as a coroutine call) and any immediate expression (such as a function call) should result in an error. Only true within one of the pre-defined calling contexts (`sync{}`, `race{}`, etc.).
    Any_ = Immediate | Async // Calling either immediate or async expressions is allowed.
};

inline const char* InvokeTimeAsCString(EInvokeTime InvokeTime)
{
    switch(InvokeTime)
    {
    case EInvokeTime::Immediate: return "Immediate";
    case EInvokeTime::Async: return "Async";
    case EInvokeTime::Any_: return "Any_";
    default: ULANG_UNREACHABLE();
    }
}

/**
 * Abstract base for applying some operation / iterating through AST structures.
 * @see CAstNode::VisitChildren()
 **/
struct SAstVisitor
{
    /** Called when visiting an AST node **/
    virtual void VisitImmediate(const char* FieldName, CUTF8StringView Value) {}
    virtual void VisitImmediate(const char* FieldName, int64_t Value) {}
    virtual void VisitImmediate(const char* FieldName, double Value) {}
    virtual void VisitImmediate(const char* FieldName, bool Value) {}
    virtual void VisitImmediate(const char* FieldName, const CTypeBase* Type) {}
    virtual void VisitImmediate(const char* FieldName, const CDefinition& Definition) {}
    virtual void VisitImmediate(const char* FieldName, const Verse::Vst::Node& VstNode) {}

    void VisitImmediate(const char* FieldName, const char* CString) { VisitImmediate(FieldName, CUTF8StringView(CString)); }

    virtual void Visit(const char* FieldName, CAstNode& AstNode) = 0;
    virtual void BeginArray(const char* FieldName, intptr_t Num) {}
    virtual void VisitElement(CAstNode& AstNode) = 0;
    virtual void EndArray() {}

    template<typename NodeType, bool bAllowNull, typename AllocatorType, typename... AllocatorArgTypes>
    void Visit(const char* FieldName, const TSPtrG<NodeType, bAllowNull, AllocatorType, AllocatorArgTypes...>& NodePointer)
    {
        if (!bAllowNull || NodePointer.IsValid())
        {
            Visit(FieldName, *NodePointer);
        }
    }

    template<typename NodeType, bool bAllowNull, typename AllocatorType, typename... AllocatorArgTypes>
    void VisitElement(const TSPtrG<NodeType, bAllowNull, AllocatorType, AllocatorArgTypes...>& NodePointer)
    {
        if (!bAllowNull || NodePointer.IsValid())
        {
            VisitElement(*NodePointer);
        }
    }

    template<typename NodeType, bool bAllowNull, typename NodeAllocatorType, typename ElementAllocatorType, typename... ElementAllocatorArgTypes>
    void VisitArray(const char* FieldName, const TArrayG<TSPtrG<NodeType, bAllowNull, NodeAllocatorType>, ElementAllocatorType, ElementAllocatorArgTypes...>& Array)
    {
        BeginArray(FieldName, Array.Num());
        for (const TSPtrG<NodeType, bAllowNull, NodeAllocatorType>& Element : Array)
        {
            if (!bAllowNull || Element.IsValid())
            {
                VisitElement(*Element);
            }
        }
        EndArray();
    }
    
    template<typename NodeType, bool bAllowNull, typename ElementAllocatorType, typename... RawAllocatorArgTypes>
    void VisitArray(const char* FieldName, const TSPtrArrayG<NodeType, bAllowNull, ElementAllocatorType, RawAllocatorArgTypes...>& Array)
    {
        BeginArray(FieldName, Array.Num());
        for (NodeType* Node : Array)
        {
            if (!bAllowNull || Node)
            {
                VisitElement(*Node);
            }
        }
        EndArray();
    }
};

enum class EVstMappingType
{
    Ast, AstNonReciprocal, Ir
};

/**
 * Abstract base class for AST nodes.
 */
class AUTORTFM_DISABLE CAstNode : public CSharedMix
{
public:
    CAstNode(EVstMappingType VstMappingType = EVstMappingType::Ast) : _VstMappingType(VstMappingType) {}
    
    UE_API virtual ~CAstNode();

    virtual EAstNodeType GetNodeType() const = 0;
    virtual const CExpressionBase* AsExpression() const { return nullptr; }
    virtual CExpressionBase* AsExpression() { return nullptr; }
    virtual bool MayHaveAttributes() const { return true; }
    virtual const CExprIdentifierBase* AsIdentifierBase() const { return nullptr; }

    /**
     * Iterates over this AST node's immediate fields, calling Visitor.VisitImmediate for each immediate field.
     */
    virtual void VisitImmediates(SAstVisitor& Visitor) const
    {
        const char* VstMappingTypeString;
        switch(_VstMappingType)
        {
        case EVstMappingType::Ast: VstMappingTypeString = "Ast"; break;
        case EVstMappingType::AstNonReciprocal: VstMappingTypeString = "AstNonReciprocal"; break;
        case EVstMappingType::Ir: VstMappingTypeString = "Ir"; break;
        default: ULANG_UNREACHABLE();
        };
        Visitor.VisitImmediate("VstMappingType", VstMappingTypeString);
        if (_MappedVstNode)
        {
            Visitor.VisitImmediate("MappedVstNode", *_MappedVstNode);
        }
    }

    /**
     * Iterates over this AST node's direct children, calling Visitor.Visit for each child.
     **/
    virtual void VisitChildren(SAstVisitor& Visitor) const {}

    /**
     * Wrapper for VisitChildren that takes a lambda that is called for each child.
     * The signature for the lambda should be (const SAstVisitor& RecurseVisitor, CAstNode&)
     * You can use RecurseVisitor to recursively call VisitChildren with the same lambda.
     */
    template<typename FunctionType>
    void VisitChildrenLambda(FunctionType&& Function) const;

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Text methods
    virtual CUTF8String GetErrorDesc() const = 0;

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Syntax <> Semantic Mapping
    const Verse::Vst::Node* GetMappedVstNode() const { return _MappedVstNode; }

    void SetNonReciprocalMappedVstNode(const Verse::Vst::Node* VstNode)
    {
        _VstMappingType = EVstMappingType::AstNonReciprocal;
        _MappedVstNode = VstNode; 
    }

    void SetIrMappedVstNode(const Verse::Vst::Node* VstNode)
    {
        _VstMappingType = EVstMappingType::Ir;
        _MappedVstNode = VstNode;
    }

    // True if this AstNode is used to represent an IrNode. Needed to disable some asserts and clean up code.
    // Will be removed when IrNodes have their own type.
    bool IsIrNode() const
    {
        return _VstMappingType == EVstMappingType::Ir;
    }

    bool IsVstMappingReciprocal() const
    {
        return _VstMappingType == EVstMappingType::Ast;
    }

protected:

    friend struct Verse::Vst::Node;
    // Syntax<>Semantic mapping
    mutable EVstMappingType _VstMappingType;
    mutable const Verse::Vst::Node* _MappedVstNode{nullptr};
};

/**
 * Abstract base class for AST expressions.
 * Created from abstract syntax tree (CExpressionBase and CSemanticProgram) generated by semantic analyzer.
 **/
class AUTORTFM_DISABLE CExpressionBase : public CAstNode, public CAttributable
{
public:

    explicit CExpressionBase(EVstMappingType VstMappingType = EVstMappingType::Ast) : CAstNode(VstMappingType) {}
    explicit CExpressionBase(const CTypeBase* InResultType)
    : _Report(SAnalysisResult{InResultType})
    {}

    virtual const CExpressionBase* AsExpression() const override { return this; }
    virtual CExpressionBase* AsExpression() override { return this; }
    virtual bool MayHaveAttributes() const override { return false; }

    using TMacroSymbols = TArrayG<CSymbol, TInlineElementAllocator<3>>;
    // True if this expression can be path of a segment. It works at all times, i.e., also before macro expression have been processed.
    // The MacroSymbols argument is used in the latter case. 
    virtual bool CanBePathSegment(const TMacroSymbols& MacroSymbols) const { return false; }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CAstNode::VisitImmediates(Visitor);
        if (_Report.IsSet())
        {
            Visitor.VisitImmediate("ResultType", _Report->ResultType);
        }
    }

    // Determines if expression is immediate (completes within the current update/frame) or 
    // async (completes within the current update/frame or later).
    //
    // @TODO: SOL-1423, DetermineInvokeTime() re-traverses the expression tree, which could add up time wise 
    //        (approaching on n^2) -- there should be a beter way to check this on the initial ProcessExpression()
    EInvokeTime DetermineInvokeTime(const CSemanticProgram& Program) const { return (FindFirstAsyncSubExpr(Program) == nullptr) ? EInvokeTime::Immediate : EInvokeTime::Async; }

    // Returns itself or the first async sub-expression or return nullptr if it and all its sub-expressions are immediate.
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const  { return nullptr; }

    // Returns whether the expression may fail.
    virtual bool CanFail(const CAstPackage* Package) const { return false; }

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Type methods
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const;
    UE_API void SetResultType(const CTypeBase* InResultType);
    UE_API void RefineResultType(const CTypeBase* RefinedResultType);
    
    bool IsAnalyzed() const { return _Report.IsSet(); }

    const CTypeBase* IrGetResultType() const { return _Report ? _Report->ResultType : nullptr; }
    void IrSetResultType(const CTypeBase* TypeBase)
    {
        if (TypeBase)
        {
            _Report = SAnalysisResult{ TypeBase };
        }
        else
        {
            _Report.Reset();
        }
    }

	// We begin by collecting each node's individual effects during SemanticAnalysis.
	// During AST IR generation, we do a pass to sum up the aggregate effects representing
	// each node and the code reachable within it.
	// These effects are conservative. We may say a node has some effects that it doesn't actually
	// have. For example, we'll propagate the <decides> effect outside of a failure context, even
	// though the failure context will eat the <decides> effect. It will be easy to make this mor precise
	// if we ever need it to be.
	SEffectSet Effects; 

protected:


    // Analysis Results
    struct SAnalysisResult
    {
        bool operator==(const SAnalysisResult& Other) const { return ResultType == Other.ResultType; }
        /** The type to which this node evaluates. */
        const CTypeBase* ResultType;
    };

    TOptional<SAnalysisResult> _Report;

};  // CExpressionBase


/**
 * Base for expressions that have an array of subexpressions
 * Subclasses: CExprMakeArray, CExprMakeMap, CExprMakeTuple, CExprCodeBlock, CExprConcurrentBlockBase, CExprAwait
 */
class AUTORTFM_DISABLE CExprCompoundBase : public CExpressionBase
{
public:
    // Methods

    explicit CExprCompoundBase(int32_t ReserveSubExprNum, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExpressionBase(VstMappingType)
    {
        _SubExprs.Reserve(ReserveSubExprNum);
    }

    explicit CExprCompoundBase(TArray<TSRef<CExpressionBase>>&& SubExprs, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExpressionBase(VstMappingType)
        , _SubExprs(Move(SubExprs))
    {}

    explicit CExprCompoundBase(TSRef<CExpressionBase>&& SubExpr1, TSRef<CExpressionBase>&& SubExpr2)
        : CExprCompoundBase(2)
    {
        AppendSubExpr(Move(SubExpr1));
        AppendSubExpr(Move(SubExpr2));
    }

    CExprCompoundBase() = default;

    bool IsEmpty() const                                               { return _SubExprs.IsEmpty(); }
    int32_t SubExprNum() const                                         { return _SubExprs.Num(); }
    const uLang::CExpressionBase*         GetLastSubExpr() const       { return _SubExprs.Last(); }
    const TArray<TSRef<CExpressionBase>>& GetSubExprs() const          { return _SubExprs; }
    TArray<TSRef<CExpressionBase>>&       GetSubExprs()                { return _SubExprs; }
    TArray<TSRef<CExpressionBase>>&&      TakeSubExprs()               { return Move(_SubExprs); }
    void AppendSubExpr(TSRef<CExpressionBase> SubExpr)               { _SubExprs.Add(Move(SubExpr)); }
    void PrependSubExpr(TSRef<CExpressionBase> SubExpr)              { _SubExprs.Insert(Move(SubExpr), 0); }
    void SetSubExprs(TArray<TSRef<CExpressionBase>>&& AnalyzedExprs) { _SubExprs = Move(AnalyzedExprs); }
    void ReplaceSubExpr(TSRef<CExpressionBase> SubExpr, int32_t Index)
    {
        ULANG_ASSERTF(Index >= 0 && Index < _SubExprs.Num(), "Replacing invalid subexpression index");
        _SubExprs[Index] = Move(SubExpr);
    }

    UE_API virtual bool CanFail(const CAstPackage* Package) const override;
    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;
    virtual void VisitChildren(SAstVisitor& Visitor) const override  { Visitor.VisitArray("SubExprs", _SubExprs); }

protected:

    // Internal data

    TArray<TSRef<CExpressionBase>> _SubExprs;
};


/**
 * Base class of binary operators.
 */
class AUTORTFM_DISABLE CExprBinaryOp : public CExpressionBase
{
private:
    // Operands
    TSPtr<CExpressionBase> _Lhs;
    TSPtr<CExpressionBase> _Rhs;

public:

    const TSPtr<CExpressionBase>& Lhs() const { return _Lhs; }
    const TSPtr<CExpressionBase>& Rhs() const { return _Rhs; }
    TSPtr<CExpressionBase>&& TakeRhs() { return Move(_Rhs); }
    void SetLhs(TSPtr<CExpressionBase>&& NewLhs) { _Lhs = Move(NewLhs); }
    void SetRhs(TSPtr<CExpressionBase>&& NewRhs) { _Rhs = Move(NewRhs); }

    CExprBinaryOp(TSPtr<CExpressionBase>&& Lhs, TSPtr<CExpressionBase>&& Rhs) : _Lhs(Move(Lhs)), _Rhs(Move(Rhs)) {}

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { const CExpressionBase* AsyncExpr = _Lhs->FindFirstAsyncSubExpr(Program); return AsyncExpr ? AsyncExpr : _Rhs->FindFirstAsyncSubExpr(Program); }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Lhs", _Lhs); Visitor.Visit("Rhs", _Rhs); }
};

/**
 * Nice to have when something should be ignored
 */
class AUTORTFM_DISABLE CNoop : public CExpressionBase
{
public:
    UE_API explicit CNoop(const CSemanticProgram& Program);

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Noop_; }
    virtual CUTF8String  GetErrorDesc() const override { return "noop_{}"; }
};

// Note - if these subclasses of CExpressionBase become more sophisticated they should
// probably be moved to their own files.


/**
 * Expression for external{} macro used in digests - should never reach the code generator
 */
class AUTORTFM_DISABLE CExprExternal : public CExpressionBase
{
public:
    // Methods

    UE_API explicit CExprExternal(const CSemanticProgram& Program);

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::External; }
    virtual CUTF8String  GetErrorDesc() const override { return "external{}"; }
};


/**
 * Logic literal - true/false
 **/
class AUTORTFM_DISABLE CExprLogic : public CExpressionBase
{
public:

    bool _Value;

    UE_API explicit CExprLogic(const CSemanticProgram& Program, bool Value);

    virtual EAstNodeType GetNodeType() const override   { return EAstNodeType::Literal_Logic; }
    virtual CUTF8String  GetErrorDesc() const override  { return _Value ? "true" : "false"; }

    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("Value", _Value); }
};


/**
 * Integer literal - 42, 0, -123, 123_456_789, 0x12fe, 0b101010
 */
class AUTORTFM_DISABLE CExprNumber : public CExpressionBase
{
private:
    union
    {
        Integer _IntValue;
        Float _FloatValue;
    };
    bool _bIsFloat : 1;

public:

    explicit CExprNumber()
        : _IntValue(0)
        , _bIsFloat(false)
    {}
 
    UE_API explicit CExprNumber(CSemanticProgram&, Integer);

    UE_API explicit CExprNumber(CSemanticProgram&, Float);

    bool IsFloat() const { return _bIsFloat; }
    Integer GetIntValue() const { ULANG_ASSERTF(!_bIsFloat, "Float number being treated as integer."); return _IntValue; }
    UE_API void SetIntValue(CSemanticProgram&, Integer);
    Float GetFloatValue() const { ULANG_ASSERTF( _bIsFloat, "Int number being treated as float"); return _FloatValue; }
    UE_API void SetFloatValue(CSemanticProgram&, Float);

    virtual EAstNodeType GetNodeType() const override   { return EAstNodeType::Literal_Number; }
    virtual CUTF8String  GetErrorDesc() const override  { return _bIsFloat ? "float literal" : "integer literal"; }

    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        if (_bIsFloat)
        {
            Visitor.VisitImmediate("FloatValue", _FloatValue);
        }
        else 
        {
            Visitor.VisitImmediate("IntValue", _IntValue);
        }
    }
};


/**
 * Character literal - 'H' '\n' '{0o00}' '{0u1f600}'
 **/
class AUTORTFM_DISABLE CExprChar : public CExpressionBase
{
public:
    enum class EType
    {
        UTF8CodeUnit,
        UnicodeCodePoint
    };

    uint32_t _CodePoint;
    EType _Type;

    explicit CExprChar(uint32_t CodePoint, EType Type)
        : _CodePoint(CodePoint)
        , _Type(Type)
    {
        if(_Type == EType::UTF8CodeUnit)
        {
            ULANG_ASSERTF(_CodePoint <= 0xFF, "utf8 code units must be <= 0xFF");
        }
    }

    CUTF8String AsString() const
    {
        switch (_Type)
        {
        case CExprChar::EType::UTF8CodeUnit:
        {
            const UTF8Char CodeUnit = {static_cast<UTF8Char>(_CodePoint)};
            return CUTF8StringView(&CodeUnit, &CodeUnit + 1);
        }
        case CExprChar::EType::UnicodeCodePoint:
        {
            SUTF8CodePoint Utf8 = CUnicode::EncodeUTF8(_CodePoint);
            return CUTF8StringView(Utf8.Units, Utf8.Units + Utf8.NumUnits);
        }
        default:
            ULANG_UNREACHABLE();
        }
    }

    virtual EAstNodeType GetNodeType() const override   { return EAstNodeType::Literal_Char; }
    virtual CUTF8String  GetErrorDesc() const override  { return "char literal"; }

    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("CodePoint", static_cast<int64_t>(_CodePoint)); }
};


/**
 * String literal - "Hello, world!", "Line 1\nLine2"
 **/
class AUTORTFM_DISABLE CExprString : public CExpressionBase
{
public:
    // Note that in the future this (or a related class) may include an array of substrings and sub-expressions for string interpolation.

    // Ready to use string with any escaped characters translated
    CUTF8String _String;

    explicit CExprString(CUTF8String String)
        : _String(Move(String)) {}

    virtual EAstNodeType GetNodeType() const override   { return EAstNodeType::Literal_String; }
    virtual CUTF8String  GetErrorDesc() const override  { return "string literal"; }

    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("String", _String); }
};

/**
 * Path literal - /unrealengine.com/UnrealEngine
 **/
class AUTORTFM_DISABLE CExprPath : public CExpressionBase
{
public:
    // Ready to use path with any escaped characters translated
    CUTF8String _Path;

    explicit CExprPath(CUTF8String Path)
        : _Path(Move(Path)) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Literal_Path; }
    virtual CUTF8String  GetErrorDesc() const override { return "path literal"; }

    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("Path", _Path); }
};

/**
 * Expression that evaluates to the path of the current scope, plus a given symbol, semantic analysis replaces this node with a CExprString
 **/
class AUTORTFM_DISABLE CExprPathPlusSymbol : public CExpressionBase
{
public:
    const CSymbol _Symbol;

    explicit CExprPathPlusSymbol(const CSymbol& Symbol)
        : _Symbol(Symbol) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::PathPlusSymbol; }
    virtual CUTF8String  GetErrorDesc() const override { return "path plus symbol"; }
};

/**
 * The base class of identifiers expressions.
 */
class AUTORTFM_DISABLE CExprIdentifierBase : public CExpressionBase
{
public:

    CExprIdentifierBase(TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr)
        : _Context(Move(Context)), _Qualifier(Move(Qualifier))
    {}

    const TSPtr<CExpressionBase>& Context() const { return _Context; }
    const TSPtr<CExpressionBase>& Qualifier() const { return _Qualifier; }
    
    TSPtr<CExpressionBase>&& TakeContext() { return Move(_Context); }
    TSPtr<CExpressionBase>&& TakeQualifier() { return Move(_Qualifier); }

    void SetContext(TSPtr<CExpressionBase> Context)
    {
        _Context = Move(Context);
    }

    void SetQualifier(TSPtr<CExpressionBase> Qualifier)
    {
        _Qualifier = Move(Qualifier);
    }

    // CAstNode interface.
    virtual bool MayHaveAttributes() const override { return true; }

    // CExpressionBase interface.
    virtual bool CanFail(const CAstPackage* Package) const override { return _Context.IsValid() && _Context->CanFail(Package); }
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override
    {
         const CExpressionBase* AsyncExpr = nullptr;
         if (_Context) { AsyncExpr = _Context->FindFirstAsyncSubExpr(Program); }
         if (!AsyncExpr && _Qualifier) { AsyncExpr =  _Qualifier->FindFirstAsyncSubExpr(Program); }
         return AsyncExpr;
    }

    virtual void VisitChildren(SAstVisitor& Visitor) const override
    {
        Visitor.Visit("Context", _Context);
    }

    /// This is specifically named as such to avoid shadowing other virtual methods (such as on `CNominalType`).
    virtual const CDefinition* IdentifierDefinition() const
    {
        return nullptr;
    }

    /// This node contains an identifier
    virtual const CExprIdentifierBase* AsIdentifierBase() const override { return this; }

private:
    TSPtr<CExpressionBase> _Context;
    TSPtr<CExpressionBase> _Qualifier;
};

/**
 * Enumerator - #GlobalEnum.enumeration, #.enumeration, MyType#Confirm.yes, #Confirm.yes,
 * #.yes, MyType@set_size#size.medium, #size.medium, #.medium, #position.upper, #.upper
 **/
class AUTORTFM_DISABLE CExprEnumLiteral : public CExprIdentifierBase
{
public:
    // Specific enumerator this represents - note that it also contains a pointer to its enumeration type.
    const CEnumerator* _Enumerator;

    explicit CExprEnumLiteral(const CEnumerator* Enumerator, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr)
        : CExprIdentifierBase(Move(Context), Move(Qualifier))
        , _Enumerator(Enumerator)
    {}

    virtual EAstNodeType     GetNodeType() const override   { return EAstNodeType::Literal_Enum; }
    virtual CUTF8String      GetErrorDesc() const override  { return "enumerator"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
    virtual const CDefinition* IdentifierDefinition() const override;
};

/**
 * Type expression - type{<expr>}
 */
class AUTORTFM_DISABLE CExprType : public CExpressionBase
{
public:
    
    const TSRef<CExpressionBase> _AbstractValue;

    CExprType(TSRef<CExpressionBase>&& AbstractValue, const CTypeType& TypeType)
        : _AbstractValue(Move(AbstractValue))
    {
        SetResultType(&TypeType);
    }

    const CTypeType* GetTypeType() const
    {
        return static_cast<const CTypeType*>(_Report->ResultType);
    }

    const CTypeBase* GetType() const
    {
        const CTypeType* TypeType = GetTypeType();
        return TypeType->PositiveType();
    }

    virtual EAstNodeType     GetNodeType() const override   { return EAstNodeType::Literal_Type; }
    virtual CUTF8String      GetErrorDesc() const override  { return "type"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("AbstractValue", _AbstractValue); }
};

/**
 * Function literal - a=>b or function(a){b}
 */
class AUTORTFM_DISABLE CExprFunctionLiteral : public CExpressionBase
{
public:

    CExprFunctionLiteral(TSRef<CExpressionBase>&& Domain, TSRef<CExpressionBase>&& Range)
    : _Domain(Move(Domain))
    , _Range(Move(Range))
    {}

    const TSRef<CExpressionBase>& Domain() const { return _Domain; }
    const TSRef<CExpressionBase>& Range () const { return _Range ; }

    void SetDomain(TSRef<CExpressionBase>&& NewDomain) { _Domain = Move(NewDomain); }
    void SetRange(TSRef<CExpressionBase>&& NewRange  ) { _Range  = Move(NewRange) ; }

    virtual EAstNodeType GetNodeType() const override  { return EAstNodeType::Literal_Function; }
    virtual CUTF8String  GetErrorDesc() const override { return "function literal"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Domain", _Domain); Visitor.Visit("Range", _Range); }

private:
    TSRef<CExpressionBase> _Domain;
    TSRef<CExpressionBase> _Range;
};

/**
 * Access to the instance the current function is being invoked on.
 **/
class AUTORTFM_DISABLE CExprSelf : public CExprIdentifierBase
{
public:
    CExprSelf(const CTypeBase* Type, TSPtr<CExpressionBase>&& Qualifier = nullptr)
    : CExprIdentifierBase(nullptr, Move(Qualifier))
    {
        SetResultType(Type);
    }

    virtual EAstNodeType GetNodeType() const override   { return EAstNodeType::Identifier_Self; }
    virtual CUTF8String  GetErrorDesc() const override  { return "'Self'"; }
    virtual const CDefinition* IdentifierDefinition() const override
    {
        // TODO: (yiliang.siew) Implement this.
        return nullptr;
    }
};

/// Represents the `(local:) qualifier`.
class AUTORTFM_DISABLE CExprLocal : public CExprIdentifierBase
{
public:
    CExprLocal(const CScope& Scope) : CExprIdentifierBase(), _Scope(Scope)
    {}

    virtual EAstNodeType GetNodeType() const override
    {
        return EAstNodeType::Identifier_Local;
    }

    virtual CUTF8String GetErrorDesc() const override
    {
        return "'(local:)'";
    }

    const CScope& GetScope() const
    {
        return _Scope;
    }

private:
    const CScope& _Scope;
};

/**
 * Represents a name of a a compiler built-in macro; e.g. option, array.
 * These should always be resolved by analysis and never make their way into code gen.
 **/
class AUTORTFM_DISABLE CExprIdentifierBuiltInMacro : public CExprIdentifierBase
{
public:
    const CSymbol _Symbol;

    CExprIdentifierBuiltInMacro(const CSymbol Symbol, const CTypeBase* Type)
        : CExprIdentifierBase(nullptr, nullptr)
        , _Symbol(Symbol)
    {
        SetResultType(Type);
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Identifier_BuiltInMacro; }
    virtual CUTF8String GetErrorDesc() const override { return _Symbol.AsStringView(); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("Symbol", _Symbol.AsStringView()); }
};

/**
 * An unresolved type identifier that is produced by desugaring, and consumed by analysis.
 **/
class AUTORTFM_DISABLE CExprIdentifierUnresolved : public CExprIdentifierBase
{
public:
    const CSymbol _Symbol;

    // Used for some internal compiler-generated code that is
    // allowed to lookup identifiers that are otherwise restricted
    // (private, internal, ...)
    bool _bAllowUnrestrictedAccess;
	
    const bool _bAllowReservedOperators;
   
    CExprIdentifierUnresolved(const CSymbol& Symbol, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr, bool bAllowReservedOperators = false)
        : CExprIdentifierBase( Move(Context), Move(Qualifier))
        , _Symbol(Symbol)
        , _bAllowUnrestrictedAccess(false)
        , _bAllowReservedOperators(bAllowReservedOperators)
    {}

    // Use with extreme caution! Setting this allows this identifier lookup to succeed where it 
    // would otherwise fail due to not having permission
    void GrantUnrestrictedAccess() { _bAllowUnrestrictedAccess = true; }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Identifier_Unresolved; }
    virtual bool MayHaveAttributes() const override { return true; }
    virtual CUTF8String  GetErrorDesc() const override { return _Symbol.AsString(); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("Symbol", _Symbol.AsStringView()); }
};


/**
 * Type identifier - MyType
 **/
class AUTORTFM_DISABLE CExprIdentifierClass : public CExprIdentifierBase
{
public:

    UE_API CExprIdentifierClass(const CTypeType* Type, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr);

    UE_API const CTypeType* GetTypeType(const CSemanticProgram& Program) const;
    UE_API const CClass* GetClass(const CSemanticProgram& Program) const;

    virtual EAstNodeType GetNodeType() const override   { return EAstNodeType::Identifier_Class; }
    UE_API virtual CUTF8String  GetErrorDesc() const override;
};

/**
 * Module identifier
 **/
class AUTORTFM_DISABLE CExprIdentifierModule : public CExprIdentifierBase
{
public:

    UE_API CExprIdentifierModule(const CModule* Module, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr);

    UE_API CModule const* GetModule(const CSemanticProgram& Program) const;

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Identifier_Module; }
    virtual CUTF8String  GetErrorDesc() const override { return "module identifier"; }
};

/**
 * Module alias identifier
 **/
class AUTORTFM_DISABLE CExprIdentifierModuleAlias : public CExprIdentifierBase
{
public:

    const CModuleAlias& _ModuleAlias;

    CExprIdentifierModuleAlias(const CModuleAlias& ModuleAlias, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr)
    : CExprIdentifierBase(Move(Context), Move(Qualifier))
    , _ModuleAlias(ModuleAlias)
    {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Identifier_ModuleAlias; }
    virtual CUTF8String  GetErrorDesc() const override { return "module alias identifier"; }
};

/**
 * Enum identifier
 * @jira SOL-1013 : Shouldn't this inherit from CExprIdentifierClass?
 **/
class AUTORTFM_DISABLE CExprEnumerationType : public CExprIdentifierBase
{
public:

    CExprEnumerationType(const CTypeType* TypeType, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr)
        : CExprIdentifierBase(Move(Context), Move(Qualifier))
    {
        SetResultType(TypeType);
    }

    UE_API const CTypeType* GetTypeType(const CSemanticProgram& Program) const;
    UE_API const CEnumeration* GetEnumeration(const CSemanticProgram& Program) const;

    virtual EAstNodeType GetNodeType() const override   { return EAstNodeType::Identifier_Enum; }
    virtual CUTF8String  GetErrorDesc() const override  { return "enum type identifier"; }
};

/**
 * Interface identifier
 * @jira SOL-1013 : Shouldn't this inherit from CExprIdentifierClass?
 **/
class AUTORTFM_DISABLE CExprInterfaceType : public CExprIdentifierBase
{
public:

    // Identified type

    CExprInterfaceType(const CTypeType* TypeType, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr)
        : CExprIdentifierBase(Move(Context), Move(Qualifier))
    {
        SetResultType(TypeType);
    }

    const CTypeType* GetTypeType(const CSemanticProgram& Program) const { return &GetResultType(Program)->GetNormalType().AsChecked<CTypeType>(); }
    UE_API const CInterface* GetInterface(const CSemanticProgram& Program) const;

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Identifier_Interface; }
    virtual CUTF8String GetErrorDesc() const override { return "interface type identifier"; }
};


/**
 * Local or class identifier - temp, arg, captured
 **/
class AUTORTFM_DISABLE CExprIdentifierData : public CExprIdentifierBase
{
public:

    // The variable this expression is referring to
    const CDataDefinition& _DataDefinition;

    UE_API CExprIdentifierData(const CSemanticProgram& Program, const CDataDefinition& DataDefinition, TSPtr<CExpressionBase> Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr);

    virtual const CSymbol&   GetName() const                { return _DataDefinition.GetName(); }

    virtual EAstNodeType     GetNodeType() const override   { return EAstNodeType::Identifier_Data; }
    virtual CUTF8String      GetErrorDesc() const override  { return _DataDefinition.AsNameStringView(); }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("DataDefinition", _DataDefinition); }
};

/**
 * Access to a type alias
 */
class AUTORTFM_DISABLE CExprIdentifierTypeAlias : public CExprIdentifierBase
{
public:

    // The type alias this expression is referring to
    const CTypeAlias& _TypeAlias;

    UE_API CExprIdentifierTypeAlias(const CTypeAlias& TypeAlias, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr);

    virtual EAstNodeType     GetNodeType() const override   { return EAstNodeType::Identifier_TypeAlias; }
    virtual CUTF8String      GetErrorDesc() const override  { return "type alias identifier"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
};

/**
 * Access to a type variable
 */
class AUTORTFM_DISABLE CExprIdentifierTypeVariable : public CExprIdentifierBase
{
public:

    // The type variable this expression is referring to
    const CTypeVariable& _TypeVariable;

    UE_API CExprIdentifierTypeVariable(const CTypeVariable& TypeVariable, TSPtr<CExpressionBase>&& Context = nullptr, TSPtr<CExpressionBase>&& Qualifier = nullptr);

    virtual EAstNodeType     GetNodeType() const override   { return EAstNodeType::Identifier_TypeVariable; }
    virtual CUTF8String      GetErrorDesc() const override  { return "type variable identifier"; }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
};

/**
 * Access to instance function members
 **/
class AUTORTFM_DISABLE CExprIdentifierFunction : public CExprIdentifierBase
{
public:

    const CFunction& _Function;
    // `CFlowType`s created as part of instantiating `_Function`.
    TArray<SInstantiatedTypeVariable> _InstantiatedTypeVariables;
    const CTypeBase* _ConstructorNegativeReturnType;
    CCaptureScope* _ConstructorCaptureScope = nullptr;
    const bool _bSuperQualified;

    CExprIdentifierFunction(
        const CFunction& Function,
        const CTypeBase* ResultType,
        TSPtr<CExpressionBase>&& Context = nullptr,
        TSPtr<CExpressionBase>&& Qualifier = nullptr)
        : CExprIdentifierFunction(Function, {}, ResultType, nullptr, nullptr, Move(Context), Move(Qualifier), false)
    {}

    UE_API CExprIdentifierFunction(
        const CFunction& Function,
        TArray<SInstantiatedTypeVariable> InstTypeVariables,
        const CTypeBase* ResultType,
        const CTypeBase* ConstructorNegativeReturnType,
        CCaptureScope* ConstructorCaptureScope,
        TSPtr<CExpressionBase>&& Context,
        TSPtr<CExpressionBase>&& Qualifier,
        bool bSuperQualified);

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Identifier_Function; }
    virtual CUTF8String GetErrorDesc() const override { return "function identifier"; }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
};

/**
 * An overloaded function identifier that hasn't been resolved to a specific overload.
 */
class AUTORTFM_DISABLE CExprIdentifierOverloadedFunction : public CExprIdentifierBase
{
public:

    const TArray<const CFunction*> _FunctionOverloads;
    bool _bConstructor;
    const CSymbol _Symbol;
    const CTypeBase* _TypeOverload;

    // Used for some internal compiler-generated code that is
    // allowed to lookup identifiers that are otherwise restricted
    // (private, internal, ...)
    bool _bAllowUnrestrictedAccess;

    UE_API CExprIdentifierOverloadedFunction(
        TArray<const CFunction*>&& OverloadedFunctions,
        bool bConstructor,
        const CSymbol Symbol,
        const CTypeBase* OverloadedType,
        TSPtr<CExpressionBase>&& Context,
        TSPtr<CExpressionBase>&& Qualifier,
        const CTypeBase* Type);

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Identifier_OverloadedFunction; }
    virtual CUTF8String GetErrorDesc() const override { return "overloaded function identifier"; }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
};

/**
 * Represents all definitions (and assignments) supported by Verse.
 * They are:
 *      element:valueDomain=value   e.g. x:int=5     e.g. f(x:int):int=x*x
 *      element:valueDomain         e.g. x:int       e.g. f(x:int):int
 *      element=value               e.g. x=5         e.g. f(x:int)=x*x
 */
class AUTORTFM_DISABLE CExprDefinition : public CExpressionBase
{
public:
    CExprDefinition(TSPtr<CExpressionBase>&& Element, TSPtr<CExpressionBase>&& ValueDomain, TSPtr<CExpressionBase>&& Value, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExpressionBase(VstMappingType)
    {
        if (Element) { SetElement(Move(Element.AsRef())); }
        if (ValueDomain) { SetValueDomain(Move(ValueDomain.AsRef())); }
        if (Value) { SetValue(Move(Value.AsRef())); }
    }

    explicit CExprDefinition(EVstMappingType VstMappingType = EVstMappingType::Ast) : CExpressionBase(VstMappingType)  {}

    const TSPtr<CExpressionBase>& Element() const { return _Element; }
    void SetElement(TSRef<CExpressionBase>&& Element) { _Element = Move(Element); }
    TSPtr<CExpressionBase> TakeElement() { return Move(_Element); }

    void SetName(CSymbol Name) { _Name = Name; }
    CSymbol GetName() const { return _Name; }
    bool IsNamed() const { return !_Name.IsNull(); }

    const TSPtr<CExpressionBase>& ValueDomain() const { return _ValueDomain; }
    void SetValueDomain(TSRef<CExpressionBase>&& ValueDomain) { _ValueDomain = Move(ValueDomain); }
    TSPtr<CExpressionBase> TakeValueDomain() { return Move(_ValueDomain); }

    const TSPtr<CExpressionBase>& Value() const { return _Value; }
    void SetValue(TSRef<CExpressionBase>&& Value) { _Value = Move(Value); }
    TSPtr<CExpressionBase> TakeValue() { return Move(_Value); }

    virtual bool CanBePathSegment(const TMacroSymbols& MacroSymbols) const override
    {
        return Value() && Value()->CanBePathSegment(MacroSymbols);
    }

    // CAstNode interface.
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Definition; }
    virtual CUTF8String GetErrorDesc() const override { return CUTF8String("definition"); }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Element", _Element); Visitor.Visit("ValueDomain", _ValueDomain); Visitor.Visit("Value", _Value); }
    virtual bool MayHaveAttributes() const override { return true; }

    // CExpressionBase interface.
    UE_API virtual bool CanFail(const CAstPackage* Package) const override;
    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;

protected:
    TSPtr<CExpressionBase> _Element;
    TSPtr<CExpressionBase> _ValueDomain;
    TSPtr<CExpressionBase> _Value;
    CSymbol _Name; // If non-null, then usage requires being ?named and presence of `_Value` indicates that it has a default
};


enum class EMacroClauseTag : uint32_t
{
    None = 0x1 << 0,
    Of = 0x1 << 1,
    Do = 0x1 << 2
};

constexpr EMacroClauseTag operator| (EMacroClauseTag A, EMacroClauseTag B) { return static_cast<EMacroClauseTag>(static_cast<uint32_t>(A) | static_cast<uint32_t>(B)); }
constexpr bool HasAnyTags(EMacroClauseTag A, EMacroClauseTag B) { return 0 != (static_cast<uint32_t>(A) & static_cast<uint32_t>(B)); }
constexpr bool HasAllTags(EMacroClauseTag A, EMacroClauseTag RequiredTags)
{
    return RequiredTags == static_cast<EMacroClauseTag>(static_cast<uint32_t>(A) & static_cast<uint32_t>(RequiredTags));
}
inline const char* MacroClauseTagAsCString(EMacroClauseTag Tag)
{
    switch(Tag)
    {
    case EMacroClauseTag::None: return "None";
    case EMacroClauseTag::Of: return "Of";
    case EMacroClauseTag::Do: return "Do";
    default: ULANG_UNREACHABLE();
    };
}
inline const char* MacroClauseFormAsCString(Verse::Vst::Clause::EForm Form)
{
    switch(Form)
    {
    case Verse::Vst::Clause::EForm::Synthetic: return "Synthetic";
    case Verse::Vst::Clause::EForm::NoSemicolonOrNewline: return "NoSemicolonOrNewline";
    case Verse::Vst::Clause::EForm::HasSemicolonOrNewline: return "HasSemicolonOrNewline";
    case Verse::Vst::Clause::EForm::IsAppendAttributeHolder: return "IsAppendAttributeHolder";
    case Verse::Vst::Clause::EForm::IsPrependAttributeHolder: return "IsPrependAttributeHolder";
    default: ULANG_UNREACHABLE();
    };
}

/** A macro call of the form m1{}, m2(){}, or more generally m(){}keyword_1{}keyword_2{}...keyword_N{} */
class AUTORTFM_DISABLE CExprMacroCall : public CExpressionBase
{

public:

    /** A macro is an identifier followed by any number of tagged clauses. This class represents a single clause. */
    class CClause
    {
    public:
        using EForm = Verse::Vst::Clause::EForm;

        CClause(EMacroClauseTag Tag, EForm Form, TArray<TSRef<CExpressionBase>>&& Exprs)
            : _Tag(Tag)
            , _Form(Form)
            , _Exprs(Move(Exprs))
        {}

        EMacroClauseTag Tag() const { return _Tag; }
        EForm Form() const { return _Form; }
        TArray<TSRef<CExpressionBase>> const & Exprs() const { return _Exprs; }
        TArray<TSRef<CExpressionBase>>       & Exprs()       { return _Exprs; }


    private:
        EMacroClauseTag _Tag;
        Verse::Vst::Clause::EForm _Form;
        TArray<TSRef<CExpressionBase>> _Exprs;
    };


public:
    CExprMacroCall( TSRef<CExpressionBase>&& Name, int32_t NumClauses = 0 )
        : _Name(Move(Name))
        , _Clauses()
    {
        if (NumClauses != 0)
        {
            _Clauses.Reserve(NumClauses);
        }
    }

    void AppendClause(CClause&& Clause)
    {
        _Clauses.Add(Move(Clause));
    }

    TSRef<CExpressionBase> const & Name() const { return _Name; }
    TSRef<CExpressionBase>       & Name()       { return _Name; }
    void SetName(TSRef<CExpressionBase>&& NewName) { _Name = Move(NewName); }

    TSRef<CExpressionBase>&& TakeName() { return Move(_Name); }

    TArray<CClause> const& Clauses() const { return _Clauses; }
    TArray<CClause>      & Clauses()       { return _Clauses; }

    TArray<CClause>&& TakeClauses() { return Move(_Clauses); }

    virtual bool CanBePathSegment(const TMacroSymbols& MacroSymbols) const override {
        CSymbol Symbol;
        if (_Name->GetNodeType() == EAstNodeType::Identifier_BuiltInMacro) {
            Symbol = static_cast<CExprIdentifierBuiltInMacro*>(_Name.Get())->_Symbol;
        } 
        else if (_Name->GetNodeType() == EAstNodeType::Identifier_Unresolved)
        {
            Symbol = static_cast<CExprIdentifierUnresolved*>(_Name.Get())->_Symbol;
        }
        if (!Symbol.IsNull())
        {
            for (CSymbol MacroSymbol : MacroSymbols)
            {
                if (MacroSymbol == Symbol)
                {
                    return true;
                }
            }
        }
        return false;
    }
private:
    // Inherited via CExpressionBase
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::MacroCall; }
    virtual CUTF8String GetErrorDesc() const override { return "macro invocation"; }

    virtual void VisitChildren(SAstVisitor& Visitor) const override
    {
        Visitor.Visit("Name", _Name);
        Visitor.BeginArray("Clauses", _Clauses.Num());
        for (const CClause& Clause : _Clauses)
        {
            Visitor.VisitArray("Exprs", Clause.Exprs());
        }
        Visitor.EndArray();
    }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        Visitor.BeginArray("Clauses", _Clauses.Num());
        for (const CClause& Clause : _Clauses)
        {
            Visitor.VisitImmediate("Tag", MacroClauseTagAsCString(Clause.Tag()));
            Visitor.VisitImmediate("Form", MacroClauseFormAsCString(Clause.Form()));
        }
        Visitor.EndArray();
    }

private:
    TSRef<CExpressionBase> _Name;
    TArray<CClause> _Clauses;
};


/**
 * Routine call - expr1.call(expr2, expr3)
 **/
class AUTORTFM_DISABLE CExprInvocation : public CExpressionBase
{
public:

    enum class EBracketingStyle
    {
        Undefined,
        Parentheses,
        SquareBrackets,
    };
    EBracketingStyle _CallsiteBracketStyle = EBracketingStyle::Undefined;

    template<typename TCallee, typename TArgument>
    CExprInvocation(EBracketingStyle CallsiteBracketStyle, TCallee&& Callee, TArgument&& Argument)
        : _CallsiteBracketStyle(CallsiteBracketStyle)
        , _Callee(ForwardArg<TCallee>(Callee))
        , _Argument(ForwardArg<TArgument>(Argument))
    {}

    template<typename TCallee, typename TArgument>
    CExprInvocation(EBracketingStyle CallsiteBracketStyle,
                    TCallee&& Callee,
                    TArgument&& Argument,
                    const CFunctionType* ResolvedCalleeType,
                    const CTypeBase* ResultType)
        : CExprInvocation(CallsiteBracketStyle, ForwardArg<TCallee>(Callee), ForwardArg<TArgument>(Argument))
    {
        SetResolvedCalleeType(ResolvedCalleeType);
        SetResultType(ResultType);
    }

    explicit CExprInvocation(TSRef<CExpressionBase>&& Argument)
        : _Argument(Move(Argument))
    {}

    CExprInvocation(CExprInvocation&& Rhs)
        : _Callee(Move(Rhs._Callee))
        , _Argument(Move(Rhs._Argument))
        , _ResolvedCalleeType(Rhs._ResolvedCalleeType)
    {}

    const TSPtr<CExpressionBase>& GetCallee() const { return _Callee; }
    TSPtr<CExpressionBase>&& TakeCallee()           { return Move(_Callee); }
    void SetCallee(TSPtr<CExpressionBase>&& Callee) { _Callee = Move(Callee); }

    const TSRef<CExpressionBase>& GetArgument() const { return _Argument; }
    TSRef<CExpressionBase>&& TakeArgument() { return Move(_Argument); }
    void SetArgument(TSRef<CExpressionBase>&& Arguments) { _Argument = Move(Arguments); }
    
    UE_API const CFunctionType* GetResolvedCalleeType() const;
    void SetResolvedCalleeType(const CFunctionType* ResolvedCalleeType) { _ResolvedCalleeType = ResolvedCalleeType; }

    /// Get the function that this invocation calls                     
    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;

    UE_API virtual bool CanFail(const CAstPackage* Package) const override;
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_Invocation; }
    virtual CUTF8String GetErrorDesc() const override { return "invocation"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Callee", _Callee); Visitor.Visit("Argument", _Argument); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        const char* BracketStyleString;
        switch(_CallsiteBracketStyle)
        {
        case EBracketingStyle::Undefined: BracketStyleString = "Undefined"; break;
        case EBracketingStyle::Parentheses: BracketStyleString = "Parentheses"; break;
        case EBracketingStyle::SquareBrackets: BracketStyleString = "SquareBrackets"; break;
        default: ULANG_UNREACHABLE();
        };
        Visitor.VisitImmediate("CallsiteBracketStyle", BracketStyleString);

        if (_ResolvedCalleeType)
        {
            Visitor.VisitImmediate("ResolvedCalleeType", _ResolvedCalleeType);
        }
    }

	bool bResultUsed { true }; // We conservatively say true. But some optimizations rely on us proving this is false.
private:
    // Callee subexpression - The function to call.
    TSPtr<CExpressionBase> _Callee;

    // Argument - possibly a tuple of expressions, included named expressions
    TSRef<CExpressionBase> _Argument;
    
    // More often than not, the function type could be inferred from _Callee, but in the case of generics,
    // you want to store the function type of the resolved generic.
    const CFunctionType* _ResolvedCalleeType{nullptr};
};

VERSECOMPILER_API const CExprIdentifierFunction* GetConstructorInvocationCallee(const CExprInvocation&);

VERSECOMPILER_API const CExprIdentifierFunction* GetConstructorInvocationCallee(const CExpressionBase&);

VERSECOMPILER_API bool IsConstructorInvocation(const CExprInvocation&);

VERSECOMPILER_API bool IsConstructorInvocation(const CExpressionBase&);

/**
 * Tuple element access `TupleExpr(Idx)`
 **/
class AUTORTFM_DISABLE CExprTupleElement : public CExpressionBase
{
public:

    // Expression that results in tuple to access element from
    TSPtr<CExpressionBase> _TupleExpr;

    // Index resolved from `_ElemIdxExpr`
    Integer _ElemIdx;

    // Expression used to determine index - currently must be an integer literal CExprNumber.
    // `_ElemIdx` is resolved form.
    TSPtr<CExpressionBase> _ElemIdxExpr;

    CExprTupleElement(CExprInvocation& Invocation) : _TupleExpr(Invocation.TakeCallee()), _ElemIdx(-1) { Invocation.GetMappedVstNode()->AddMapping(this); }
    CExprTupleElement(TSPtr<CExpressionBase> TupleExpr, Integer ElemIdx, const Verse::Vst::Node* MappedVstNode) : _TupleExpr(TupleExpr), _ElemIdx(ElemIdx) { _MappedVstNode = MappedVstNode; }

    UE_API virtual bool CanFail(const CAstPackage* Package) const override;
    virtual void VisitChildren(SAstVisitor& Visitor) const override           { Visitor.Visit("TupleExpr", _TupleExpr); Visitor.Visit("ElemIdxExpr", _ElemIdxExpr); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override         { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("ElemIdx", _ElemIdx); }
    virtual EAstNodeType   GetNodeType() const override                       { return EAstNodeType::Invoke_TupleElement; }
    virtual CUTF8String    GetErrorDesc() const override                      { return "tuple element access"; }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override
    {
        if (const CExpressionBase* AsyncExpr = _TupleExpr->FindFirstAsyncSubExpr(Program))
        {
            return AsyncExpr;
        }
        if (_ElemIdxExpr)
        {
            return _ElemIdxExpr->FindFirstAsyncSubExpr(Program);
        }
        return nullptr;
    }
};


/**
 * Assignment -- expr1 = expr2, expr1 := expr2, expr1 += expr2, etc.
 */
class AUTORTFM_DISABLE CExprAssignment : public CExpressionBase
{
public:
    using EOp = Verse::Vst::Assignment::EOp;

    CExprAssignment(EOp Op, TSRef<CExpressionBase>&& Lhs, TSRef<CExpressionBase>&& Rhs)
        : _Op(Op), _Lhs(Move(Lhs)), _Rhs(Move(Rhs))
    {}

    EOp Op() const { return _Op; }
    const TSRef<CExpressionBase>& Lhs() const { return _Lhs; }
    const TSRef<CExpressionBase>& Rhs() const { return _Rhs; }

    TSRef<CExpressionBase>&& TakeLhs() { return Move(_Lhs); }
    TSRef<CExpressionBase>&& TakeRhs() { return Move(_Rhs); }

    void SetLhs(TSRef<CExpressionBase>&& Lhs) { _Lhs = Move(Lhs); }
    void SetRhs(TSRef<CExpressionBase>&& Rhs) { _Rhs = Move(Rhs); }

    //~ Begin CExpressionBase interface
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { const CExpressionBase* AsyncExpr = _Lhs->FindFirstAsyncSubExpr(Program); return AsyncExpr ? AsyncExpr : _Rhs->FindFirstAsyncSubExpr(Program); }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Assignment; }
    virtual CUTF8String GetErrorDesc() const override { return "assignment"; }
    
    virtual bool CanFail(const CAstPackage* Package) const override { return _Lhs->CanFail(Package) || _Rhs->CanFail(Package); }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Lhs", _Lhs); Visitor.Visit("Rhs", _Rhs); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("Op", Verse::Vst::AssignmentOpAsCString(_Op)); }
    //~ End CExpressionBase interface

private:
    const EOp _Op;
    TSRef<CExpressionBase> _Lhs;
    TSRef<CExpressionBase> _Rhs;
};

struct SAssignmentLhsIdentifier
{
    TSPtr<CExprPointerToReference> PointerToReference;
    TSPtr<CExprIdentifierData> IdentifierData;
};
VERSECOMPILER_API TOptional<SAssignmentLhsIdentifier> IdentifierOfAssignmentLhs(const CExprAssignment* Assignment /* can be null */);
VERSECOMPILER_API bool HasImplicitClassSelf(const CExprIdentifierData* Expr /* can be null */);
VERSECOMPILER_API bool IsClassMemberAccess(const CExprIdentifierData* Expr /* can be null */);


/**
 * Base class of unary operators.
 */
class AUTORTFM_DISABLE CExprUnaryOp : public CExpressionBase
{
public:
    explicit CExprUnaryOp(TSPtr<CExpressionBase>&& Operand, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExpressionBase(VstMappingType)
    {
        SetOperand(Move(Operand));
    }

    const TSPtr<CExpressionBase>& Operand() const { return _Operand; }
    TSPtr<CExpressionBase>&& TakeOperand() { return Move(_Operand); }
    void SetOperand(TSPtr<CExpressionBase>&& Operand) { _Operand = Move(Operand); }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return _Operand.IsValid() ? _Operand->FindFirstAsyncSubExpr(Program) : nullptr; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Operand", _Operand); }

private:
    
    TSPtr<CExpressionBase> _Operand;
};


class AUTORTFM_DISABLE CExprUnaryArithmetic : public CExprInvocation
{
public:
    enum class EOp
    {
        Negate
    };

    CExprUnaryArithmetic(EOp Op, TSRef<CExpressionBase>&& Rhs)
        : CExprInvocation(Move(Rhs))
        , _Op(Op)
    {
    }

    TSRef<CExpressionBase> Operand() const { return GetArgument(); }
    void SetOperand(TSRef<CExpressionBase>&& NewOperand) { SetArgument(Move(NewOperand)); }
    TSRef<CExpressionBase>&& TakeOperand() { return TakeArgument(); }

    EOp Op() const { return _Op; }

private:
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_UnaryArithmetic; }
    virtual bool CanFail(const CAstPackage* Package) const override { return false; }
    virtual CUTF8String GetErrorDesc() const override { return "unary negation"; }

    EOp _Op;
};


class AUTORTFM_DISABLE CExprBinaryArithmetic : public CExprInvocation
{
public:
    enum class EOp
    {
        Add,
        Sub,
        Mul,
        Div
    };

    CExprBinaryArithmetic(EOp Op, TSRef<CExpressionBase>&& Argument)
        : CExprInvocation(Move(Argument))
        , _Op(Op)
    {
    }

    EOp Op() const { return _Op; }

private:
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_BinaryArithmetic; }
    virtual CUTF8String GetErrorDesc() const override
    {
        switch (_Op)
        {
        case EOp::Add: return "binary addition";
        case EOp::Sub: return "binary subtraction";
        case EOp::Mul: return "binary multiplication";
        case EOp::Div: return "binary division";
        default: ULANG_UNREACHABLE();
        };
    }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        const char* OpString;
        switch(_Op)
        {
        case EOp::Add: OpString = "Add"; break;
        case EOp::Sub: OpString = "Sub"; break;
        case EOp::Mul: OpString = "Mul"; break;
        case EOp::Div: OpString = "Div"; break;
        default: ULANG_UNREACHABLE();
        };
        Visitor.VisitImmediate("Op", OpString);
    }

    EOp _Op;
};

/**
 * Short-circuit evaluation of a Boolean and
 **/
class AUTORTFM_DISABLE CExprShortCircuitAnd : public CExprBinaryOp
{
public:

    CExprShortCircuitAnd(TSPtr<CExpressionBase>&& Lhs, TSPtr<CExpressionBase>&& Rhs) : CExprBinaryOp(Move(Lhs), Move(Rhs)) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_ShortCircuitAnd; }
    virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override { return Rhs()->GetResultType(Program); }
    virtual bool CanFail(const CAstPackage* Package) const override { return true; }
    virtual CUTF8String GetErrorDesc() const override    { return "logical '&&'"; }
};


/**
 * Short-circuit evaluation of a Boolean or
 **/
class AUTORTFM_DISABLE CExprShortCircuitOr : public CExprBinaryOp
{
public:

    CExprShortCircuitOr(TSPtr<CExpressionBase>&& Lhs, TSPtr<CExpressionBase>&& Rhs, const CTypeBase* JoinType = nullptr)
        : CExprBinaryOp(Move(Lhs), Move(Rhs))
    {
        if (JoinType != nullptr)
        {
            SetResultType(JoinType);
        }
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_ShortCircuitOr; }
    virtual bool CanFail(const CAstPackage* Package) const override { return Rhs()->CanFail(Package); }
    virtual CUTF8String GetErrorDesc() const override    { return "logical '||'"; }

private:

};


/**
 * Logical not operator
 */
class AUTORTFM_DISABLE CExprLogicalNot : public CExprUnaryOp
{
public:

    CExprLogicalNot(TSPtr<CExpressionBase>&& Operand): CExprUnaryOp(Move(Operand)) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_LogicalNot; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual bool CanFail(const CAstPackage* Package) const override { return true; }
    virtual CUTF8String GetErrorDesc() const override    { return "logical 'not'"; }
};


/**
* Comparison operators.
*/
class AUTORTFM_DISABLE CExprComparison : public CExprInvocation
{
public:
    using EOp = Verse::Vst::BinaryOpCompare::op;

    CExprComparison(EOp Op, TSRef<CExpressionBase>&& Argument)
        : CExprInvocation(Move(Argument))
        , _Op(Op)
    {
        _CallsiteBracketStyle = CExprInvocation::EBracketingStyle::SquareBrackets;
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_Comparison; }
    UE_API virtual CUTF8String GetErrorDesc() const override;
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("Op", Verse::Vst::BinaryCompareOpAsCString(_Op)); }

    EOp Op() const { return _Op; }

private:
    EOp _Op;
};


/**
 * Query the value of a boolean or option value.
 **/
class AUTORTFM_DISABLE CExprQueryValue : public CExprInvocation
{
public:

    CExprQueryValue(TSRef<CExpressionBase>&& Operand)
    : CExprInvocation(Move(Operand))
    {
        _CallsiteBracketStyle = CExprInvocation::EBracketingStyle::SquareBrackets;
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_QueryValue; }
    virtual CUTF8String GetErrorDesc() const override { return "postfix '?' operator"; }
};


/**
 * Box an option value
 */
class AUTORTFM_DISABLE CExprMakeOption : public CExprUnaryOp
{
public:

    CExprMakeOption(const CTypeBase* Type, TSPtr<CExpressionBase>&& Operand)
        : CExprUnaryOp(Move(Operand))
    {
        SetResultType(Type);
    }

    COptionType const* GetOptionType(const CSemanticProgram& Program) const
    {
        const CTypeBase* MyResultType = GetResultType(Program);
        return &(MyResultType->GetNormalType().AsChecked<COptionType>());
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_MakeOption; }
    virtual bool CanFail(const CAstPackage* Package) const override { return false; }
    virtual CUTF8String GetErrorDesc() const override { return "option value constructor"; }
};


/**
 * Create an array value
 * Can have zero or more subexpressions
 */
class AUTORTFM_DISABLE CExprMakeArray : public CExprCompoundBase
{
public:
    CExprMakeArray(int32_t ReserveSubExprNum) : CExprCompoundBase(ReserveSubExprNum) {}

    const CArrayType* GetArrayType(const CSemanticProgram& Program) const
    {
        return &GetResultType(Program)->GetNormalType().AsChecked<CArrayType>();
    }

    //~ Begin CExpressionBase interface
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_MakeArray; }
    virtual CUTF8String GetErrorDesc() const override { return "array value"; }
    //~ End CExpressionBase interface
};

/**
 * Create a map value
 * Can have zero or more subexpressions
 */
class AUTORTFM_DISABLE CExprMakeMap : public CExprCompoundBase
{
public:
    explicit CExprMakeMap(int32_t ReserveSubExprNum = 0) : CExprCompoundBase(ReserveSubExprNum) {}

    const CMapType* GetMapType(const CSemanticProgram& Program) const
    {
        return &GetResultType(Program)->GetNormalType().AsChecked<CMapType>();
    }

    //~ Begin CExpressionBase interface
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_MakeMap; }
    virtual CUTF8String GetErrorDesc() const override { return "map value"; }
    UE_API virtual bool CanFail(const CAstPackage* Package) const override;
    //~ End CExpressionBase interface
};

/**
 * Create a tuple value
 */
class AUTORTFM_DISABLE CExprMakeTuple : public CExprCompoundBase
{
public:
    using CExprCompoundBase::CExprCompoundBase;

    const CTupleType* GetTupleType(const CSemanticProgram& Program) const
    {
        return &GetResultType(Program)->GetNormalType().AsChecked<CTupleType>();
    }

    //~ Begin CExpressionBase interface
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_MakeTuple; }
    virtual CUTF8String GetErrorDesc() const override { return "tuple value"; }
    //~ End CExpressionBase interface
};


/**
* Create a range value
*/
class AUTORTFM_DISABLE CExprMakeRange : public CExpressionBase
{
public:
    TSRef<CExpressionBase> _Lhs;
    TSRef<CExpressionBase> _Rhs;

    CExprMakeRange(TSRef<CExpressionBase>&& Lhs, TSRef<CExpressionBase>&& Rhs)
        : _Lhs(Move(Lhs))
        , _Rhs(Move(Rhs))
    {}

    void SetLhs(TSRef<CExpressionBase>&& Lhs) { _Lhs = Move(Lhs); }
    void SetRhs(TSRef<CExpressionBase>&& Rhs) { _Rhs = Move(Rhs); }

    //~ Begin CExpressionBase interface
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_MakeRange; }
    virtual bool CanFail(const CAstPackage* Package) const override { return _Lhs->CanFail(Package) || _Rhs->CanFail(Package); }
    virtual CUTF8String GetErrorDesc() const override { return "range constructor"; }
    //~ End CExpressionBase interface

    virtual void VisitChildren(SAstVisitor& Visitor) const override
    {
        Visitor.Visit("Lhs", _Lhs);
        Visitor.Visit("Rhs", _Rhs);
    }
};


/**
 * Invoke a type as a function on a value - type(expr) or type[expr].
 */
class AUTORTFM_DISABLE CExprInvokeType : public CExpressionBase
{
public:
    const CTypeBase* _NegativeType;
    const bool _bIsFallible;
    const TSPtr<CExpressionBase> _TypeAst;
    const TSRef<CExpressionBase> _Argument;
    
    UE_API CExprInvokeType(const CTypeBase* NegativeType, const CTypeBase* PositiveType, bool bIsFallible, TSPtr<CExpressionBase>&& TypeAst, TSRef<CExpressionBase>&& Argument);

    //~ Begin CExpressionBase interface
    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;
    virtual bool CanFail(const CAstPackage* Package) const override { return _bIsFallible || _Argument->CanFail(Package); }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_Type; }
    virtual CUTF8String GetErrorDesc() const override { return "type invocation"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("TypeAst", _TypeAst); Visitor.Visit("Argument", _Argument); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        Visitor.VisitImmediate("NegativeType", _NegativeType);
        Visitor.VisitImmediate("bIsFallible", _bIsFallible);
    }
    //~ End CExpressionBase interface
};


/**
 * Read the value of a variable.
 */
class AUTORTFM_DISABLE CExprPointerToReference : public CExprUnaryOp
{
public:
    CExprPointerToReference(TSRef<CExpressionBase>&& Variable, bool bWritable = true)
        : CExprUnaryOp(Move(Variable))
        , _bWritable(bWritable)
    {
    }

    //~ Begin CExpressionBase interface
    virtual bool CanFail(const CAstPackage* Package) const override { return Operand()->CanFail(Package); }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_PointerToReference; }
    virtual CUTF8String GetErrorDesc() const override { return "pointer to reference"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    //~ End CExpressionBase interface

private:
    bool _bWritable;
};

/**
 * Evaluate operand to an l-expression.
 */
class AUTORTFM_DISABLE CExprSet : public CExprUnaryOp
{
public:
    CExprSet(bool bIsLive, TSPtr<CExpressionBase> Operand, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExprSet{bIsLive, nullptr, Move(Operand), VstMappingType}
    {
    }

    CExprSet(bool bIsLive, CCaptureControlScope* LiveScope, TSPtr<CExpressionBase> Operand, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExprUnaryOp{Move(Operand), VstMappingType}
        , _bIsLive{bIsLive}
        , _LiveScope{LiveScope}
    {
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_Set; }
    virtual bool CanFail(const CAstPackage* Package) const override { return Operand()->CanFail(Package); }
    virtual CUTF8String GetErrorDesc() const override { return "set"; }

    bool _bIsLive;
    CCaptureControlScope* _LiveScope = nullptr;
};

/**
 * Create a new pointer from an initial value.
 */
class AUTORTFM_DISABLE CExprNewPointer : public CExpressionBase
{
public:
    CCaptureControlScope* _LiveScope;
    const TSRef<CExpressionBase> _Value;
    
    CExprNewPointer(const CPointerType* PointerType, CCaptureControlScope* LiveScope, TSRef<CExpressionBase> Value)
        : CExpressionBase(PointerType)
        , _LiveScope(LiveScope)
        , _Value(Move(Value))
    {
    }

    //~ Begin CExpressionBase interface
    virtual bool CanFail(const CAstPackage* Package) const override { return _Value->CanFail(Package); }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_NewPointer; }
    virtual CUTF8String GetErrorDesc() const override { return "pointer new"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Value", _Value); }
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return _Value->FindFirstAsyncSubExpr(Program); }
    //~ End CExpressionBase interface
};

/**
 * Evaluates the value of an expression yielding a reference type.
 */
class AUTORTFM_DISABLE CExprReferenceToValue : public CExprUnaryOp
{
public:
    UE_API explicit CExprReferenceToValue(TSPtr<CExpressionBase> Operand);

    //~ Begin CExpressionBase interface
    virtual bool CanFail(const CAstPackage* Package) const override { return Operand()->CanFail(Package); }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_ReferenceToValue; }
    virtual CUTF8String GetErrorDesc() const override { return "convert reference to value"; }
    //~ End CExpressionBase interface
};

/**
 * Code block - `{expr1; expr2}` or `do {expr1; expr2}`
 * Can have zero or more subexpressions
 **/
class AUTORTFM_DISABLE CExprCodeBlock : public CExprCompoundBase
{
public:
    // The scope containing locals for this block
    TSPtr<CControlScope> _AssociatedScope;

    // Methods

    explicit CExprCodeBlock(int32_t ReserveSubExprNum = 0) : CExprCompoundBase(ReserveSubExprNum) {}
    explicit CExprCodeBlock(TArray<TSRef<CExpressionBase>>&& SubExprs) : CExprCompoundBase(Move(SubExprs)) {}

    virtual EAstNodeType     GetNodeType() const override  { return EAstNodeType::Flow_CodeBlock; }
    virtual CUTF8String      GetErrorDesc() const override { return "code block"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
};

class AUTORTFM_DISABLE CExprLet : public CExprCompoundBase
{
    using CExprCompoundBase::CExprCompoundBase;
    virtual EAstNodeType     GetNodeType() const override { return EAstNodeType::Flow_Let; }
    virtual CUTF8String      GetErrorDesc() const override { return "let"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
};

/**
 * Return statement - return expr
 **/
class AUTORTFM_DISABLE CExprReturn : public CExpressionBase
{
public:

    CExprReturn() {}
    CExprReturn(TSPtr<CExpressionBase>&& Result, const CFunction* Function = nullptr)
    {
        SetResult(Move(Result));
        SetFunction(Function);
    }

    const TSPtr<CExpressionBase>& Result() const { return _Result; }
    void SetResult(TSPtr<CExpressionBase>&& Result) { _Result = Move(Result); }

    const CFunction* Function() const { return _Function; }
    void SetFunction(const CFunction* Function) { _Function = Function; }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return _Result.IsValid() ? _Result->FindFirstAsyncSubExpr(Program) : nullptr; }
    virtual bool CanFail(const CAstPackage* Package) const override { return _Result.IsValid() && _Result->CanFail(Package); }
    virtual EAstNodeType     GetNodeType() const override  { return EAstNodeType::Flow_Return; }
    virtual CUTF8String      GetErrorDesc() const override { return "return"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual bool MayHaveAttributes() const override { return true; }

    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Result", _Result); }

private:
    
    // Result expression
    TSPtr<CExpressionBase> _Result;

    // The function being returned from.
    const CFunction* _Function{nullptr};
};


/**
 * Conditional with failable tests- if (test[]) {clause1}, if (test[]) {clause1} else {else_clause}
 **/
class AUTORTFM_DISABLE CExprIf : public CExpressionBase
{
public:

    CExprIf(TSRef<CExprCodeBlock>&& Condition, TSPtr<CExpressionBase>&& ThenClause, TSPtr<CExpressionBase>&& ElseClause = nullptr)
        : _Condition(Move(Condition))
        , _ThenClause(Move(ThenClause))
        , _ElseClause(Move(ElseClause))
    {}
    
    const TSRef<CExprCodeBlock>& GetCondition() const { return _Condition; }
    void SetCondition(TSRef<CExprCodeBlock>&& Condition) { _Condition = Move(Condition); }

    const TSPtr<CExpressionBase>& GetThenClause() const { return _ThenClause; }
    void SetThenClause(TSPtr<CExpressionBase>&& ThenClause) { _ThenClause = Move(ThenClause); }

    const TSPtr<CExpressionBase>& GetElseClause() const { return _ElseClause; }
    void SetElseClause(TSPtr<CExpressionBase>&& ElseClause) { _ElseClause = Move(ElseClause); }

    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;
    UE_API virtual bool CanFail(const CAstPackage* Package) const override;
    virtual EAstNodeType    GetNodeType() const override  { return EAstNodeType::Flow_If; }
    virtual CUTF8String     GetErrorDesc() const override { return "if"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Condition", _Condition); Visitor.Visit("ThenClause", _ThenClause); Visitor.Visit("ElseClause", _ElseClause); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("bIsFilter", _bIsFilter); }

    // If can be used as a filter in a for(..) { }. Code generation need to know.
    bool _bIsFilter{ false }; 
private:
    
    TSRef<CExprCodeBlock> _Condition;
    TSPtr<CExpressionBase> _ThenClause;
    TSPtr<CExpressionBase> _ElseClause;
};


/**
 * Bounded iteration
 **/
class AUTORTFM_DISABLE CExprIteration : public CExpressionBase
// Note that CExprSyncIterated, CExprRushIterated and CExprRaceIterated are subclasses
{
public:
    CExprIteration() {}
    CExprIteration(TSPtr<CExprCodeBlock>&& Filters) : _Filters(Move(Filters)) {}

    void SetBody(TSPtr<CExpressionBase>&& Body)     { _Body = Move(Body); }
    void AddFilter(TSRef<CExpressionBase>&& Filter) { 
        ULANG_ENSUREF(_Filters, "Filters must be initialized before calling AddFilter.");
        _Filters->AppendSubExpr(Filter);
    }

    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;
    virtual bool CanFail(const CAstPackage* Package) const override { return _Body && _Body->CanFail(Package); }
    virtual EAstNodeType   GetNodeType() const override  { return EAstNodeType::Flow_Iteration; }
    virtual CUTF8String    GetErrorDesc() const override { return "for"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Filters", _Filters); Visitor.Visit("Body", _Body); }

public:
    TSPtr<CExprCodeBlock> _Filters;

    // Expression to evaluate for every iteration that gets past the filters step
    TSPtr<CExpressionBase> _Body;
};

/**
 * First - first(item:collection; pred(item)) {item}
 **/
class AUTORTFM_DISABLE CExprFirst : public CExprIteration
{
public:
    virtual bool CanFail(const CAstPackage* Package) const override { return !_Filters || _Filters->CanFail(Package) || (_Body && _Body->CanFail(Package)); }
    virtual CUTF8String GetErrorDesc() const override { return "first(:){}"; }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Flow_First; }
};

/**
 * Iteration based concurrency primitives - sync(item:collection){}, rush(item:collection){} or race(item:collection){}
 **/
class AUTORTFM_DISABLE CExprConcurrentIteratedBase : public CExprIteration
{
public:
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return this; }
};


/**
 * Iterated Sync (collection form) concurrency primitive - sync(item:collection) {_coro1() _coro2()}
 **/
class AUTORTFM_DISABLE CExprSyncIterated : public CExprConcurrentIteratedBase
{
public:
    virtual CUTF8String GetErrorDesc() const override { return "sync(:){}"; }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_SyncIterated; }
};


/**
 * Iterated Rush (collection form) concurrency primitive - rush(item:collection) {_coro1() _coro2()}
 **/
class AUTORTFM_DISABLE CExprRushIterated : public CExprConcurrentIteratedBase
{
public:
    virtual CUTF8String GetErrorDesc() const override { return "rush(:){}"; }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_RushIterated; }
};


/**
 * Iterated Race (collection form) concurrency primitive - race(item:collection) {_coro1() _coro2()}
 **/
class AUTORTFM_DISABLE CExprRaceIterated : public CExprConcurrentIteratedBase
{
public:
    virtual CUTF8String GetErrorDesc() const override { return "race(:){}"; }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_RaceIterated; }
};


/// Base class for all expressions that form a type out of input type(s)
class AUTORTFM_DISABLE CExprTypeFormer : public CExpressionBase
{
public:

    // Metatype of the actual type formed.
    const CTypeType* _TypeType{nullptr};

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override  { return nullptr; }
    virtual const CTypeBase*       GetResultType(const CSemanticProgram& Program) const override          { return _TypeType; }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("TypeType", _TypeType); }
};


class AUTORTFM_DISABLE CExprUnaryTypeFormer : public CExprTypeFormer
{
public:

    CExprUnaryTypeFormer(TSRef<CExpressionBase>&& InnerTypeAst): _InnerTypeAst(Move(InnerTypeAst)) {}

    const TSRef<CExpressionBase>& GetInnerTypeAst() const { return _InnerTypeAst; }
    void SetInnerTypeAst(TSRef<CExpressionBase>&& InnerTypeAst) { _InnerTypeAst = Move(InnerTypeAst); }
    
    // CExpressionBase interface.
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return _InnerTypeAst->FindFirstAsyncSubExpr(Program); }
    
    // CAstNode interface.
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("InnerTypeAst", _InnerTypeAst); }

private:

    TSRef<CExpressionBase> _InnerTypeAst;
};


class AUTORTFM_DISABLE CExprArrayTypeFormer : public CExprUnaryTypeFormer
{
public:

    CExprArrayTypeFormer(TSRef<CExpressionBase>&& InnerTypeAst): CExprUnaryTypeFormer(Move(InnerTypeAst)) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_ArrayFormer; }
    virtual CUTF8String GetErrorDesc() const override { return "array type"; }
    
    const CArrayType* GetArrayType() const
    {
        ULANG_ASSERTF(_TypeType, "GetArrayType called on unanalyzed expression");
        return &_TypeType->PositiveType()->GetNormalType().AsChecked<CArrayType>();
    }
};

class AUTORTFM_DISABLE CExprGeneratorTypeFormer : public CExprUnaryTypeFormer
{
public:

    /// Construct an expression that takes the `ValueType` and forms an Generator of `ValueType`.
    CExprGeneratorTypeFormer(TSRef<CExpressionBase>&& InnerTypeAst) : CExprUnaryTypeFormer(Move(InnerTypeAst)) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_GeneratorFormer; }
    virtual CUTF8String GetErrorDesc() const override { return "generator(..)"; }

    const CGeneratorType* GetGeneratorType() const
    {
        ULANG_ASSERTF(_TypeType, "GetGeneratorType called on unanalyzed expression");
        return &_TypeType->PositiveType()->GetNormalType().AsChecked<CGeneratorType>();
    }
};

class AUTORTFM_DISABLE CExprMapTypeFormer : public CExprTypeFormer
{
public:
    
    CExprMapTypeFormer(TArray<TSRef<CExpressionBase>>&& KeyTypeAsts, TSRef<CExpressionBase>&& ValueTypeAst)
    : CExprTypeFormer()
    , _KeyTypeAsts(KeyTypeAsts)
    , _ValueTypeAst(ValueTypeAst)
    {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_MapFormer; }
    virtual CUTF8String GetErrorDesc() const override { return "map type"; }
    
    const TArray<TSRef<CExpressionBase>>& KeyTypeAsts() const { return _KeyTypeAsts; }
    void SetKeyTypeAst(TSRef<CExpressionBase>&& KeyTypeAst, int32_t Index) { _KeyTypeAsts[Index] = Move(KeyTypeAst); }

    const TSRef<CExpressionBase>& ValueTypeAst() const { return _ValueTypeAst; }
    void SetValueTypeAst(TSRef<CExpressionBase>&& ValueTypeAst) { _ValueTypeAst = Move(ValueTypeAst); }

    const CMapType* GetMapType() const
    {
        ULANG_ASSERTF(_TypeType, "GetMapType called on unanalyzed expression");
        return &_TypeType->PositiveType()->GetNormalType().AsChecked<CMapType>();
    }

    // CExpressionBase interface.
    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;
    
    // CAstNode interface.
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.VisitArray("KeyTypeAsts", _KeyTypeAsts); Visitor.Visit("ValueTypeAst", _ValueTypeAst); }

private:
    
    TArray<TSRef<CExpressionBase>> _KeyTypeAsts;
    TSRef<CExpressionBase> _ValueTypeAst;
};

class AUTORTFM_DISABLE CExprOptionTypeFormer : public CExprUnaryTypeFormer
{
public:

    CExprOptionTypeFormer(TSRef<CExpressionBase>&& InnerTypeAst): CExprUnaryTypeFormer(Move(InnerTypeAst)) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_OptionFormer; }
    virtual CUTF8String GetErrorDesc() const override { return "option type"; }

    const COptionType* GetOptionType() const
    {
        ULANG_ASSERTF(_TypeType, "GetOptionType called on unanalyzed expression");
        return &_TypeType->NegativeType()->GetNormalType().AsChecked<COptionType>();
    }
};

class AUTORTFM_DISABLE CExprSubtype : public CExprUnaryTypeFormer
{
public:
    /// Construct an expression that takes the `ValueType` and forms an option of `ValueType`.
    CExprSubtype(TSRef<CExpressionBase>&& InnerTypeAst): CExprUnaryTypeFormer(Move(InnerTypeAst)) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_Subtype; }
    virtual CUTF8String GetErrorDesc() const override;
    
    UE_API const CTypeType& GetSubtypeType() const;

    ETypeConstraintFlags _SubtypeConstraint = ETypeConstraintFlags::None;
};

// Get or create a tuple based on `tuple(type1, type2, ...)`
class AUTORTFM_DISABLE CExprTupleType : public CExprTypeFormer
{
public:

    CExprTupleType(int32_t ReserveTypeExprNum = 0)                           { _ElementTypeExprs.Reserve(ReserveTypeExprNum); }

    const TSPtrArray<CExpressionBase>& GetElementTypeExprs() const           { return _ElementTypeExprs; }
    TSPtrArray<CExpressionBase>&       GetElementTypeExprs()                 { return _ElementTypeExprs; }
    void ReplaceElementTypeExpr(TSPtr<CExpressionBase>&& TypeExpr, int32_t Index)
    {
        ULANG_ASSERTF(Index >= 0 && Index < _ElementTypeExprs.Num(), "Replacing invalid subexpression index");
        _ElementTypeExprs.ReplaceAt(Move(TypeExpr), Index);
    }

    virtual EAstNodeType   GetNodeType() const override                      { return EAstNodeType::Invoke_TupleType; }
    virtual CUTF8String    GetErrorDesc() const override                     { return "tuple(..)"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override          { Visitor.VisitArray("ElemTypeExprs", _ElementTypeExprs); }
    
    const CTupleType* GetTupleType() const
    {
        ULANG_ASSERTF(_TypeType, "GetTupleType called on unanalyzed expression");
        return &_TypeType->PositiveType()->GetNormalType().AsChecked<CTupleType>();
    }

protected:

    TSPtrArray<CExpressionBase> _ElementTypeExprs;
};

/**
 * Create a function type from a parameter and return type.
 * Also used on the LHS of a definition as a pattern for iteration pairs.
 */
class AUTORTFM_DISABLE CExprArrow : public CExprTypeFormer
{
public:

    CExprArrow(TSRef<CExpressionBase>&& Domain, TSRef<CExpressionBase>&& Range)
        : _Domain(Move(Domain))
        , _Range(Move(Range))
    {}

    const TSRef<CExpressionBase>& Domain() const { return _Domain; }
    const TSRef<CExpressionBase>& Range() const { return _Range; }

    void SetDomain(TSRef<CExpressionBase>&& NewDomain) { _Domain = Move(NewDomain); }
    void SetRange(TSRef<CExpressionBase>&& NewRange) { _Range = Move(NewRange); }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_Arrow; }
    virtual CUTF8String  GetErrorDesc() const override { return "function type"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Domain", _Domain); Visitor.Visit("Range", _Range); }

    const CFunctionType* GetFunctionType() const
    {
        ULANG_ASSERTF(_TypeType, "GetFunctionType called on unanalyzed expression");
        return &_TypeType->PositiveType()->GetNormalType().AsChecked<CFunctionType>();
    }

private:
    TSRef<CExpressionBase> _Domain;
    TSRef<CExpressionBase> _Range;
};

/**
 * Represents an initializer-list style construction for certain object types; 
 * i.e.: Type{expr1, id=expr2, ...}
 */
class AUTORTFM_DISABLE CExprArchetypeInstantiation : public CExpressionBase
{
public:
    UE_API CExprArchetypeInstantiation(TSRef<CExpressionBase>&& ClassAst, CExprMacroCall::CClause&& BodyAst, const CTypeBase* ResultType, const bool bIsDynamicConcreteType);

    UE_API const CNominalType* GetClassOrInterface(const CSemanticProgram& Program) const;

    const TSRefArray<CExpressionBase>& Arguments() const { return _Arguments; }

    int32_t AppendArgument(TSRef<CExpressionBase>&& Argument)
    {
        return _Arguments.Add(Move(Argument));
    }

    virtual bool CanFail(const CAstPackage* Package) const override 
    {
        for (const auto& Argument : _Arguments)
        {
            if (Argument->CanFail(Package))
            {
                return true;
            }
        }
        return false;
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Invoke_ArchetypeInstantiation; }

    virtual CUTF8String GetErrorDesc() const override { return "archetype constructor"; }

    virtual void VisitChildren(SAstVisitor& Visitor) const override
    {
        Visitor.Visit("ClassAst", _ClassAst);
        Visitor.VisitArray("BodyAstExprs", _BodyAst.Exprs());
        Visitor.BeginArray("Arguments", _Arguments.Num());
        for(const TSRef<CExpressionBase>& Argument : _Arguments)
        {
            Visitor.VisitElement(Argument);
        }
        Visitor.EndArray();
    }

    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        Visitor.VisitImmediate("BodyAstTag", MacroClauseTagAsCString(_BodyAst.Tag()));
        Visitor.VisitImmediate("BodyAstForm", MacroClauseFormAsCString(_BodyAst.Form()));
    }

    TSRef<CExpressionBase> _ClassAst;
    CExprMacroCall::CClause _BodyAst;
    const bool _bIsDynamicConcreteType;

private:
    TSRefArray<CExpressionBase> _Arguments;
};

/**
 * Block based concurrency primitives - sync{}, rush{} or race{}
 * Can have two or more async subexpressions - 0 or 1 is a warning or error.  
 **/
class AUTORTFM_DISABLE CExprConcurrentBlockBase : public CExprCompoundBase
{
public:
    explicit CExprConcurrentBlockBase(int32_t ReserveSubExprNum = 0) : CExprCompoundBase(ReserveSubExprNum) {}

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override  { return this; }
};


/**
 * Sync concurrency primitive - sync {_coro1() _coro2()}
 * 2+ Async expressions run concurrently and next expression executed when *all* expressions
 * completed. Its overall result is the result of the last expression in the block to complete
 * (using most specific common compatible result type of all expressions or no result if there is
 * no common type). Expressions may only be async (such as coroutines).
 **/
class AUTORTFM_DISABLE CExprSync : public CExprConcurrentBlockBase
{
public:
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_Sync; }
    virtual CUTF8String GetErrorDesc() const override { return "sync{}"; }
};


/**
 * Race concurrency primitive - race {_coro1() _coro2()}
 * 2+ Async expressions run concurrently and next expression executed when *fastest/first*
 * expression completed and all other expressions are aborted. Its overall result is result of
 * first expression in block to complete (using most specific common compatible result type of all
 * expressions or no result if there is no common type).
 **/
class AUTORTFM_DISABLE CExprRace : public CExprConcurrentBlockBase
{
public:
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_Race; }
    virtual CUTF8String GetErrorDesc() const override { return "race{}"; }
};


/**
 * Rush concurrency primitive - rush {_coro1() _coro2()}
 * 2+ Async expressions run concurrently and next expression executed when *fastest/first*
 * expression completed and all other expressions continue independently until each is fully
 * completed. Its overall result is the result of the first expression in the block to complete
 * (using most specific common compatible result type of all expressions or no result if there is
 * no common type).
 **/
class AUTORTFM_DISABLE CExprRush : public CExprConcurrentBlockBase
{
public:
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_Rush; }
    virtual CUTF8String GetErrorDesc() const override { return "rush{}"; }
};


/**
 * Expressions with a sub-block - branch, spawn, loop, defer
 **/
class AUTORTFM_DISABLE CExprSubBlockBase : public CExpressionBase
{
protected:
    // Sub-expression from block - either single expression or block of expressions
    TSPtr<CExpressionBase> _BlockExpr;

public:

    void SetExpr(TSPtr<CExpressionBase>&& Expr) { _BlockExpr = Move(Expr); }
    const TSPtr<CExpressionBase>& Expr() const { return _BlockExpr; }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return nullptr; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("BlockExpr", _BlockExpr); }
};


/**
 * Branch concurrency primitive - branch {Coro()}
 * One async expression started and next expression executed immediately while the started
 * expression continues independently until completion or the surrounding context (such as a
 * coroutine) completes in which case it aborts early. Result is a handle to the invoked coroutine / invoked
 * expression (also known as a future or promise) tracking the expression.
 **/
class AUTORTFM_DISABLE CExprBranch : public CExprSubBlockBase
{
public:
    virtual EAstNodeType GetNodeType() const override  { return EAstNodeType::Concurrent_Branch; }
    virtual CUTF8String  GetErrorDesc() const override { return "branch"; }

    CCaptureControlScope* _Scope{nullptr};
};


/**
 * Spawn concurrency primitive - spawn {Coro()}  [future: spawn {Expr1; Expr2}]
 * Async expression treated as a closure/lambda with next expression executed
 * immediately while the started async expression continues independently. Has no result.
 * spawn essentially makes any async expression an immediate expression. This also means that
 * it can allow async expressions / coroutines to be called within immediate functions.
 **/
class AUTORTFM_DISABLE CExprSpawn : public CExprSubBlockBase
{
public:
    virtual EAstNodeType GetNodeType() const override  { return EAstNodeType::Concurrent_Spawn; }
    virtual CUTF8String  GetErrorDesc() const override { return "spawn"; }

    CCaptureControlScope* _Scope{nullptr};
};

class AUTORTFM_DISABLE CExprAwait : public CExprCompoundBase
{
public:
    explicit CExprAwait(int32_t ReserveSubExprNum)
        : CExprCompoundBase(ReserveSubExprNum)
    {
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_Await; }

    virtual CUTF8String  GetErrorDesc() const override { return "await"; }

    virtual bool MayHaveAttributes() const override { return false; }

    virtual bool CanFail(const CAstPackage* Package) const override { return false; }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return this; }
};

class AUTORTFM_DISABLE CExprBatch : public CExprCompoundBase
{
public:
    explicit CExprBatch(int32_t ReserveSubExprNum)
        : CExprCompoundBase(ReserveSubExprNum)
    {
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_Batch; }

    virtual CUTF8String  GetErrorDesc() const override { return "batch"; }

    virtual bool MayHaveAttributes() const override { return false; }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram&) const override { return nullptr; }
};

class AUTORTFM_DISABLE CExprBinaryAwaitOp : public CExpressionBase
{
public:
    CExprBinaryAwaitOp(TSRefArray<CExpressionBase> DomainExprs, TSRefArray<CExpressionBase> RangeExprs)
        : _DomainExprs(Move(DomainExprs))
        , _RangeExprs(Move(RangeExprs))
    {
    }

    CExprBinaryAwaitOp(int32_t NumReservedDomainExprs, int32_t NumReservedRangeExprs)
    {
        _DomainExprs.Reserve(NumReservedDomainExprs);
        _RangeExprs.Reserve(NumReservedRangeExprs);
    }

    virtual bool MayHaveAttributes() const override { return false; }

    virtual bool CanFail(const CAstPackage* Package) const override { return false; }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override
    {
        for (const auto& RangeExpr : _RangeExprs)
        {
            if (const CExpressionBase* FirstAyncSubExpr = RangeExpr->FindFirstAsyncSubExpr(Program))
            {
                return FirstAyncSubExpr;
            }
        }
        return nullptr;
    }

    const TSRefArray<CExpressionBase>& GetDomainExprs() const
    {
        return _DomainExprs;
    }

    const TSRefArray<CExpressionBase>& GetRangeExprs() const
    {
        return _RangeExprs;
    }

    void AppendDomainExpr(TSRef<CExpressionBase> Expr)
    {
        _DomainExprs.Add(Move(Expr));
    }

    void AppendRangeExpr(TSRef<CExpressionBase> Expr)
    {
        _RangeExprs.Add(Move(Expr));
    }

    CCaptureControlScope* _DomainScope{nullptr};

private:
    TSRefArray<CExpressionBase> _DomainExprs;
    TSRefArray<CExpressionBase> _RangeExprs;
};

class AUTORTFM_DISABLE CExprUpon : public CExprBinaryAwaitOp
{
public:
    using CExprBinaryAwaitOp::CExprBinaryAwaitOp;

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_Upon; }

    virtual CUTF8String  GetErrorDesc() const override { return "upon"; }
};

class AUTORTFM_DISABLE CExprWhen : public CExprBinaryAwaitOp
{
public:
    using CExprBinaryAwaitOp::CExprBinaryAwaitOp;

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Concurrent_When; }

    virtual CUTF8String  GetErrorDesc() const override { return "when"; }
};

/**
 * Loop flow/concurrency primitive/macro - loop {Expr1; Expr2}
 * Loops one or more expressions.
 * For safety/security it *must* have at least one async expression, `break`, or `return`
 * so it is less likely to have an infinite loop within a single update.
 **/
class AUTORTFM_DISABLE CExprLoop : public CExprSubBlockBase
{
public:
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return _BlockExpr->FindFirstAsyncSubExpr(Program); }
    virtual EAstNodeType           GetNodeType() const override           { return EAstNodeType::Flow_Loop; }
    virtual CUTF8String            GetErrorDesc() const override          { return "loop"; }

    virtual bool CanFail(const CAstPackage* Package) const override {
        return  _BlockExpr->CanFail(Package);
    }
};


/**
 * defer macro - defer {Expr1; Expr2}
 * Defers expressions until end of current scope. Only called if encountered in flow and called in reverse order encountered.
 **/
class AUTORTFM_DISABLE CExprDefer : public CExprSubBlockBase
{
public:
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return _BlockExpr->FindFirstAsyncSubExpr(Program); }
    virtual EAstNodeType           GetNodeType() const override           { return EAstNodeType::Flow_Defer; }
    virtual CUTF8String            GetErrorDesc() const override          { return "defer"; }
};

/**
 * Control flow early exit
 *
 *   loop:
 *       if (IsEarlyExit[]):
 *           break
 *       DoLoopStuff()
 **/
class AUTORTFM_DISABLE CExprBreak : public CExpressionBase
{
public:
    const CExpressionBase* _AssociatedControlFlow{nullptr};

    virtual EAstNodeType GetNodeType() const override           { return EAstNodeType::Flow_Break; }
    virtual CUTF8String  GetErrorDesc() const override          { return "break"; }
    virtual bool MayHaveAttributes() const override { return true; }
    
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
};

/**
 * Represents members of a class/interface/module/snippet definition node
 */
class AUTORTFM_DISABLE CMemberDefinitions
{
public:
    CMemberDefinitions() {}
    CMemberDefinitions(TArray<TSRef<CExpressionBase>>&& Members) : _Members(Move(Members)) {}

    const TArray<TSRef<CExpressionBase>>& Members() const { return _Members; }
    void SetMembers(TArray<TSRef<CExpressionBase>>&& Members) { _Members = Move(Members); }
    void AppendMember(TSRef<CExpressionBase>&& Member) { _Members.Add(Move(Member)); }
    void PrependMember(TSRef<CExpressionBase>&& Member) { _Members.Insert(Move(Member), 0); }
    void SetMember(TSRef<CExpressionBase>&& Member, int32_t Index) { _Members[Index] = Move(Member); }

    void VisitMembers(SAstVisitor& Visitor) const { Visitor.VisitArray("Members", _Members); }

private:
    TArray<TSRef<CExpressionBase>> _Members;
};

/**
 * Represents a snippet in the AST.
 */
class AUTORTFM_DISABLE CExprSnippet : public CExpressionBase, public CMemberDefinitions
{
public:
    CUTF8String _Path;
    CSnippet* _SemanticSnippet{ nullptr };

    CExprSnippet(const CUTF8StringView& Path) : _Path(Path) {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Context_Snippet; }
    virtual CUTF8String  GetErrorDesc() const override { return "snippet"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual void VisitChildren(SAstVisitor& Visitor) const override { VisitMembers(Visitor); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("Path", _Path); }
};

/**
 * Represents a module definition in the AST.
 */
class AUTORTFM_DISABLE CExprModuleDefinition : public CExpressionBase, public CMemberDefinitions
{
public:
    CUTF8String _Name;
    CModulePart* _SemanticModule{ nullptr }; // You can get the CModule from this as well
    bool _bLegacyPublic = false; // To emulate legacy behavior while vmodule files are allowed

    CExprModuleDefinition(const CUTF8StringView& Name, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExpressionBase(VstMappingType)
        , _Name(Name)
    {}
    UE_API CExprModuleDefinition(CModulePart& Module, TArray<TSRef<CExpressionBase>>&& Members);
    UE_API ~CExprModuleDefinition();

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Definition_Module; }
    virtual CUTF8String  GetErrorDesc() const override { return "module definition"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual void VisitChildren(SAstVisitor& Visitor) const override { VisitMembers(Visitor); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("Name", _Name); }
    virtual bool CanBePathSegment(const TMacroSymbols& MacroSymbols) const override { return true; }
};

/**
 * Represents an enum definition in the AST.
 */
class AUTORTFM_DISABLE CExprEnumDefinition : public CExpressionBase
{
public:
    CEnumeration& _Enum;
    
    const TArray<TSRef<CExpressionBase>> _Members;

    UE_API CExprEnumDefinition(CEnumeration& Enum, TArray<TSRef<CExpressionBase>>&& Members, EVstMappingType = EVstMappingType::Ast);
    UE_API ~CExprEnumDefinition();

    virtual EAstNodeType GetNodeType() const override                    { return EAstNodeType::Definition_Enum; }
    virtual CUTF8String  GetErrorDesc() const override                   { return "enum definition"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.VisitArray("Members", _Members); }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
    virtual bool CanBePathSegment(const TMacroSymbols& MacroSymbols) const override { return true; }
};

/**
 * Represents both named and anonymous scoped access level definitions in the AST.
 * e.g. foo_scope := scoped{A,B,C}  #named
 *      foo<scoped{A,B,C}> := 42 # anonymous
 */
class AUTORTFM_DISABLE CExprScopedAccessLevelDefinition : public CExpressionBase
{
public:
    TSRef<CScopedAccessLevelDefinition> _AccessLevelDefinition;
    TArray<TSRef<CExpressionBase>> _ScopeReferenceExprs;

    UE_API CExprScopedAccessLevelDefinition(TSRef<CScopedAccessLevelDefinition>& AccessLevelDefinition, EVstMappingType VstMappingType = EVstMappingType::Ast);
    UE_API ~CExprScopedAccessLevelDefinition();

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Definition_ScopedAccessLevel; }
    virtual CUTF8String  GetErrorDesc() const override { return "scoped access level"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.VisitArray("Scopes", _ScopeReferenceExprs); }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
};

/**
 * Represents a interface definition in the AST.
 */
class AUTORTFM_DISABLE CExprInterfaceDefinition : public CExpressionBase, public CMemberDefinitions
{
public:
    CInterface& _Interface;
    
    const TArray<TSRef<CExpressionBase>>& SuperInterfaces() const { return _SuperInterfaces; }
    void SetSuperInterfaces(TArray<TSRef<CExpressionBase>>&& SuperInterfaces) { _SuperInterfaces = SuperInterfaces; }
    void SetSuperInterface(TSRef<CExpressionBase>&& SuperInterface, int32_t Index) { _SuperInterfaces[Index] = Move(SuperInterface); }

    UE_API CExprInterfaceDefinition(CInterface& Interface, TArray<TSRef<CExpressionBase>>&& SuperInterfaces, TArray<TSRef<CExpressionBase>>&& Members, EVstMappingType = EVstMappingType::Ast);

    CExprInterfaceDefinition(CInterface& Interface, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExprInterfaceDefinition(Interface, {}, {}, VstMappingType)
    {}

    UE_API ~CExprInterfaceDefinition();

    virtual EAstNodeType GetNodeType() const override                    { return EAstNodeType::Definition_Interface; }
    virtual CUTF8String  GetErrorDesc() const override                   { return "interface definition"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { VisitMembers(Visitor); }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
    virtual bool MayHaveAttributes() const override { return true; }
    virtual bool CanBePathSegment(const TMacroSymbols& MacroSymbols) const override { return true; }
private:

    TArray<TSRef<CExpressionBase>> _SuperInterfaces;
};

/**
 * Represents a class definition in the AST.
 */
class AUTORTFM_DISABLE CExprClassDefinition : public CExpressionBase, public CMemberDefinitions
{
public:
    CClass& _Class;
    
    UE_API CExprClassDefinition(CClass& Class, TArray<TSRef<CExpressionBase>>&& SuperTypes, TArray<TSRef<CExpressionBase>>&& Members, EVstMappingType = EVstMappingType::Ast);

    CExprClassDefinition(CClass& Class, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExprClassDefinition(Class, {}, {}, VstMappingType)
    {
    }

    UE_API ~CExprClassDefinition();

    const TArray<TSRef<CExpressionBase>>& SuperTypes() const { return _SuperTypes; }
    void SetSuperTypes(TArray<TSRef<CExpressionBase>>&& SuperTypes) { _SuperTypes = Move(SuperTypes); }
    void SetSuperType(TSRef<CExpressionBase>&& SuperType, int32_t Index) { _SuperTypes[Index] = Move(SuperType); }

    virtual EAstNodeType GetNodeType() const override                    { return EAstNodeType::Definition_Class; }
    virtual CUTF8String  GetErrorDesc() const override                   { return "class definition"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { VisitMembers(Visitor); }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override; 
    virtual bool CanBePathSegment(const TMacroSymbols& MacroSymbols) const override { return true; }
private:
    TArray<TSRef<CExpressionBase>> _SuperTypes;
};

/**
 * Represents a data definition in the AST.
 */
class AUTORTFM_DISABLE CExprDataDefinition : public CExprDefinition
{
public:
    const TSRef<CDataDefinition> _DataMember;

    UE_API CExprDataDefinition(const TSRef<CDataDefinition>& DataMember, TSPtr<CExpressionBase>&& Element, TSPtr<CExpressionBase>&& ValueDomain, TSPtr<CExpressionBase>&& Value, EVstMappingType = EVstMappingType::Ast);
    UE_API ~CExprDataDefinition();

    virtual EAstNodeType GetNodeType() const override                    { return EAstNodeType::Definition_Data; }
    virtual CUTF8String  GetErrorDesc() const override                   { return "data definition"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual void VisitImmediates(SAstVisitor& Visitor) const override    { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("DataMember", *_DataMember); }
};

/**
 * Represents a map pair definition in the AST: (Key=>Value):Map
 */
class AUTORTFM_DISABLE CExprIterationPairDefinition : public CExprDefinition
{
public:
    const TSRef<CDataDefinition> _KeyDefinition;
    const TSRef<CDataDefinition> _ValueDefinition;

    UE_API CExprIterationPairDefinition(
        TSRef<CDataDefinition>&& KeyDefinition,
        TSRef<CDataDefinition>&& ValueDefinition,
        TSPtr<CExpressionBase>&& Element,
        TSPtr<CExpressionBase>&& ValueDomain,
        TSPtr<CExpressionBase>&& Value,
        EVstMappingType VstMappingType = EVstMappingType::Ast);
    UE_API ~CExprIterationPairDefinition();

    virtual EAstNodeType GetNodeType() const override                    { return EAstNodeType::Definition_IterationPair; }
    virtual CUTF8String  GetErrorDesc() const override                   { return "iteration pair definition"; }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override    { CExpressionBase::VisitImmediates(Visitor); Visitor.VisitImmediate("KeyDefinition", *_KeyDefinition); Visitor.VisitImmediate("ValueDefinition", *_ValueDefinition); }
};

/**
 * Add an item to an array
 * The array itself is not included in the node,
 * instead the result destination in the code generator is used.
 * In the future this will be explicit in the IR, but for now it's inside code generation.
 **/
class AUTORTFM_DISABLE CIrArrayAdd : public CExpressionBase
{
public:
    CIrArrayAdd(TSRef<CExpressionBase>&& Source)
        : CExpressionBase(EVstMappingType::Ir)
        , _Source(Move(Source))
        {}

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Ir_ArrayAdd; }
    virtual CUTF8String  GetErrorDesc() const override { return "array add"; }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override {return _Source->FindFirstAsyncSubExpr(Program); }
    virtual bool CanFail(const CAstPackage* Package) const override { return _Source->CanFail(Package); }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Source", _Source); }

    TSRef<CExpressionBase> _Source;
};

/// @see CIrArrayAdd
class AUTORTFM_DISABLE CIrMapAdd : public CExpressionBase
{
public:
    CIrMapAdd(TSRef<CExpressionBase>&& Key, TSRef<CExpressionBase>&& Value)
        : CExpressionBase(EVstMappingType::Ir)
        , _Key(Move(Key))
        , _Value(Move(Value))
    {
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Ir_MapAdd; }
    virtual CUTF8String  GetErrorDesc() const override { return "map add"; }
    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram&) const override;
    UE_API virtual bool CanFail(const CAstPackage* Package) const override;
    UE_API virtual void VisitChildren(SAstVisitor& Visitor) const override;

    TSRef<CExpressionBase> _Key;
    TSRef<CExpressionBase> _Value;
};

class AUTORTFM_DISABLE CIrArrayUnsafeCall : public CExpressionBase
{
public:
    CIrArrayUnsafeCall(TSRef<CExpressionBase>&& Callee, TSRef<CExpressionBase>&& Arguments)
        : _Callee(Move(Callee))
        , _Argument(Move(Arguments))
    {
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Ir_ArrayUnsafeCall; }

    virtual CUTF8String  GetErrorDesc() const override { return "array unsafe call"; }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override
    {
        return _Callee->FindFirstAsyncSubExpr(Program);
    }

    virtual bool CanFail(const CAstPackage* Package) const override
    {
        return
            _Callee->CanFail(Package) ||
            _Argument->CanFail(Package);
    }

    virtual void VisitChildren(SAstVisitor& Visitor) const override
    {
        Visitor.Visit("Callee", _Callee);
        Visitor.Visit("Argument", _Argument);
    }

    TSRef<CExpressionBase> _Callee;
    TSRef<CExpressionBase> _Argument;
};

/**
 * Converts a value to a dynamically typed value. Only present in the IR, not the AST.
 */
class AUTORTFM_DISABLE CIrConvertToDynamic : public CExprUnaryOp
{
public:
    UE_API CIrConvertToDynamic(const CTypeBase* ResultType, TSRef<CExpressionBase>&& Value);

    //~ Begin CExpressionBase interface
    virtual bool CanFail(const CAstPackage* Package) const override { return Operand()->CanFail(Package); }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Ir_ConvertToDynamic; }
    virtual CUTF8String GetErrorDesc() const override { return "convert value to dynamically typed value"; }
    //~ End CExpressionBase interface
};

/**
 * Access data pointer of dynamically typed value. Only present in the IR, not the AST.
 */
class AUTORTFM_DISABLE CIrDataOfDynamic : public CExprUnaryOp
{
public:
    CIrDataOfDynamic(TSRef<CExpressionBase> Operand)
        : CExprUnaryOp(Move(Operand), EVstMappingType::Ir)
    {
    }

    //~ Begin CExpressionBase interface
    virtual bool CanFail(const CAstPackage* Package) const override { return Operand()->CanFail(Package); }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Ir_DataOfDynamic; }
    virtual CUTF8String GetErrorDesc() const override { return "access data pointer of dynamically typed value"; }
    //~ End CExpressionBase interface
};

/**
 * Converts a value to a dynamically typed value. Only present in the IR, not the AST.
 */
class AUTORTFM_DISABLE CIrConvertFromDynamic : public CExprUnaryOp
{
public:
    UE_API CIrConvertFromDynamic(const CTypeBase* ResultType, TSRef<CExpressionBase>&& Value);

    //~ Begin CExpressionBase interface
    virtual bool CanFail(const CAstPackage* Package) const override { return Operand()->CanFail(Package); }
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Ir_ConvertFromDynamic; }
    virtual CUTF8String GetErrorDesc() const override { return "convert value from dynamically typed value"; }
    //~ End CExpressionBase interface
};

/**
 * Used to represent either an all or a one.
 **/
class AUTORTFM_DISABLE CIrAllOne : public CExpressionBase
{
public:
    CIrAllOne(TSRef<CExprCodeBlock>&& Iteration, bool bInAll = true)
        : CExpressionBase(EVstMappingType::Ir)
        , _bAll(bInAll)
        , _Iteration(Move(Iteration)) {
    }

    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;
    virtual bool CanFail(const CAstPackage* Package) const override { return _bCanFail; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override {
        Visitor.Visit("Iteration", _Iteration);
    }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        Visitor.VisitImmediate("bGenerateResult", _bGenerateResult);
    }

    virtual EAstNodeType   GetNodeType() const override { return EAstNodeType::Ir_AllOne; }
    virtual CUTF8String    GetErrorDesc() const override { return _bAll ? "ir_all" : "ir_one"; }

    bool _bAll;
    bool _bCanFail{ false };
    bool _bGenerateResult{ true };
    TSRef<CExprCodeBlock>  _Iteration;
};

class AUTORTFM_DISABLE CIrFor : public CExpressionBase
{
public:
    bool _bCanFail{ false };

    const TSPtr<CDataDefinition> _KeyMember;
    const TSRef<CDataDefinition> _DataMember;
    const TSRef<CExprDefinition> _Definition;

    CIrFor(const TSRef<CDataDefinition>& DataMember, TSPtr<CExpressionBase>&& Element, TSPtr<CExpressionBase>&& ValueDomain, TSPtr<CExpressionBase>&& Value)
        : CExpressionBase(EVstMappingType::Ir)
        , _KeyMember()
        , _DataMember(DataMember)
        , _Definition(TSRef<CExprDefinition>::New(Move(Element), Move(ValueDomain), Move(Value), EVstMappingType::Ir)) {}

    CIrFor(const TSRef<CDataDefinition>& KeyMember, const TSRef<CDataDefinition>& DataMember, TSPtr<CExpressionBase>&& Element, TSPtr<CExpressionBase>&& ValueDomain, TSPtr<CExpressionBase>&& Value)
        : CExpressionBase(EVstMappingType::Ir)
        , _KeyMember(KeyMember)
        , _DataMember(DataMember)
        , _Definition(TSRef<CExprDefinition>::New(Move(Element), Move(ValueDomain), Move(Value), EVstMappingType::Ir)) {}


    void SetBody(TSPtr<CExpressionBase>&& Body) { _Body = Move(Body); }

    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;
    virtual bool CanFail(const CAstPackage* Package) const override { return _bCanFail; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Definition", _Definition); Visitor.Visit("Body", _Body); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        if (_KeyMember)
        {
            Visitor.VisitImmediate("KeyMember", *_KeyMember);
        }
        Visitor.VisitImmediate("DataMember", *_DataMember);
    }

    virtual EAstNodeType   GetNodeType() const override { return EAstNodeType::Ir_For; }
    virtual CUTF8String    GetErrorDesc() const override { return "ir_for"; }

public:
    // The scope containing the variables used for iterating
    TSPtr<CControlScope> _AssociatedScope;

    // Expression to evaluate for every iteration that gets past the filters step
    TSPtr<CExpressionBase> _Body;
};

/**
 * Wraps the innermost body of CIrFor
 * It wraps the code not inside the failure context of CIrFor.
 **/
class AUTORTFM_DISABLE CIrIteratedBody : public CExpressionBase
{
public:
    CIrIteratedBody(TSPtr<CExpressionBase>&& Body, bool bIsFor = true)
        : CExpressionBase(EVstMappingType::Ir)
        , _bIsFor(bIsFor)
        , _Body(Body) {
    }

    void SetBody(TSPtr<CExpressionBase>&& Body) { _Body = Move(Body); }

    UE_API virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override;
    virtual bool CanFail(const CAstPackage* Package) const override { return _Body && _Body->CanFail(Package); }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Body", _Body); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override {}

    virtual EAstNodeType   GetNodeType() const override { return EAstNodeType::Ir_IteratedBody; }
    virtual CUTF8String    GetErrorDesc() const override { return "ir_iterated_body"; }

public:
    // Expression to evaluate outside the failure contexts of the enclosing CIrFor
    bool _bIsFor;  // If true then part of a for, otherwise part of a first.
    TSPtr<CExpressionBase> _Body;
};

/**
 * Represents a function definition in the AST.
 */
class AUTORTFM_DISABLE CExprFunctionDefinition : public CExprDefinition
{
public:
    const TSRef<CFunction> _Function;

    UE_API CExprFunctionDefinition(const TSRef<CFunction>& Function, TSPtr<CExpressionBase>&& Element, TSPtr<CExpressionBase>&& ValueDomain, TSPtr<CExpressionBase>&& Value, EVstMappingType = EVstMappingType::Ast);
    UE_API ~CExprFunctionDefinition();

    UE_API bool HasUserAddedPredictsEffect(const CSemanticProgram& Program) const;

    virtual EAstNodeType GetNodeType() const override                    { return EAstNodeType::Definition_Function; }
    virtual CUTF8String  GetErrorDesc() const override                   { return "function definition"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
};

/**
 * Represents a type alias definition in the AST.
 */
class AUTORTFM_DISABLE CExprTypeAliasDefinition : public CExprDefinition
{
public:
    const TSRef<CTypeAlias> _TypeAlias;

    UE_API CExprTypeAliasDefinition(const TSRef<CTypeAlias>& TypeAlias, TSPtr<CExpressionBase>&& Element, TSPtr<CExpressionBase>&& ValueDomain, TSPtr<CExpressionBase>&& Value, EVstMappingType = EVstMappingType::Ast);
    UE_API ~CExprTypeAliasDefinition();

    virtual EAstNodeType GetNodeType() const override                    { return EAstNodeType::Definition_TypeAlias; }
    virtual CUTF8String  GetErrorDesc() const override                   { return "type alias definition"; }
};

/**
 * Represents a using declaration in the AST.
 */
class AUTORTFM_DISABLE CExprUsing : public CExpressionBase
{
public:
    // Note that not all `using` refer to a module
    const CModule* _Module = nullptr;
    
    TSRef<CExpressionBase> _Context;

    CExprUsing(TSRef<CExpressionBase>&& Context) : _Context(Move(Context)) {}

    virtual EAstNodeType GetNodeType() const override                    { return EAstNodeType::Definition_Using; }
    virtual CUTF8String  GetErrorDesc() const override                   { return "using"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Context", _Context); }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
};

/**
 * Represents a profile block macro invocation in the AST.
 */
class AUTORTFM_DISABLE CExprProfileBlock : public CExprSubBlockBase
{
public:
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Flow_ProfileBlock; }
    virtual CUTF8String  GetErrorDesc() const override { return "profile"; }
    virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override { return _BlockExpr->GetResultType(Program); }
    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override { return _BlockExpr->FindFirstAsyncSubExpr(Program); }

    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        Visitor.Visit("UserTag", _UserTag);
        CExprSubBlockBase::VisitImmediates(Visitor);
    }

    virtual void VisitChildren(SAstVisitor& Visitor) const override
    {
        Visitor.Visit("UserTag", _UserTag);
        CExprSubBlockBase::VisitChildren(Visitor);
    }

    TSPtr<uLang::CExpressionBase> _UserTag;         // Must resolve to a string type
    
#if WITH_VERSE_BPVM
    const uLang::CTupleType* _ProfileLocusType; 
    const uLang::CTupleType* _ProfileDataType;
#endif

};

/**
 * Represents a import declaration in the AST.
 */
class AUTORTFM_DISABLE CExprImport : public CExpressionBase
{
public:
    const TSRef<CModuleAlias> _ModuleAlias;
    const TSRef<CExpressionBase> _Path;

    UE_API CExprImport(const TSRef<CModuleAlias>& ModuleAlias, TSRef<CExpressionBase>&& Path, EVstMappingType VstMappingType = EVstMappingType::Ast);
    UE_API ~CExprImport();

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Definition_Import; }
    virtual CUTF8String  GetErrorDesc() const override { return "import"; }
    UE_API virtual const CTypeBase* GetResultType(const CSemanticProgram& Program) const override;
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.Visit("Path", _Path); }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;
};

class AUTORTFM_DISABLE CExprWhere : public CExpressionBase
{
public:
    CExprWhere(TSRef<CExpressionBase>&& Lhs, TSPtrArray<CExpressionBase>&& Rhs)
        : _Lhs(Move(Lhs))
        , _Rhs(Move(Rhs))
    {
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Definition_Where; }

    virtual CUTF8String GetErrorDesc() const override { return "where"; }

    virtual void VisitChildren(SAstVisitor& Visitor) const override
    {
        Visitor.Visit("Lhs", _Lhs);
        Visitor.VisitArray("Rhs", _Rhs);
    }

    const TSPtr<CExpressionBase>& Lhs() const
    {
        return _Lhs;
    }

    const TSPtrArray<CExpressionBase>& Rhs() const
    {
        return _Rhs;
    }

    TSPtrArray<CExpressionBase>& Rhs()
    {
        return _Rhs;
    }

    void SetLhs(TSPtr<CExpressionBase> NewLhs)
    {
        _Lhs = Move(NewLhs);
    }

    void SetRhs(TSPtrArray<CExpressionBase> NewRhs)
    {
        _Rhs = Move(NewRhs);
    }

private:
    TSPtr<CExpressionBase> _Lhs;
    TSPtrArray<CExpressionBase> _Rhs;
};

class AUTORTFM_DISABLE CExprVar : public CExprUnaryOp
{
public:
    CExprVar(bool bIsLive, TSPtr<CExpressionBase> Operand, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExprUnaryOp{Move(Operand), VstMappingType}
        , _bIsLive{bIsLive}
    {
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Definition_Var; }
    virtual bool CanFail(const CAstPackage* Package) const override { return Operand()->CanFail(Package); }
    virtual CUTF8String GetErrorDesc() const override { return "var"; }

    bool _bIsLive;
};


class AUTORTFM_DISABLE CExprLive : public CExprUnaryOp
{
public:
    explicit CExprLive(TSPtr<CExpressionBase> Operand, EVstMappingType VstMappingType = EVstMappingType::Ast)
        : CExprUnaryOp{Move(Operand), VstMappingType}
    {
    }

    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Definition_Live; }
    virtual bool CanFail(const CAstPackage* Package) const override { return Operand()->CanFail(Package); }
    virtual CUTF8String GetErrorDesc() const override { return "live"; }
};

/**
 * Represents a named value / default value placeholder in the AST.
 */
class AUTORTFM_DISABLE CExprMakeNamed : public CExpressionBase
{
public:
    explicit CExprMakeNamed(const CSymbol Name)
        : _Name(Name)
    {
    }

    CExprMakeNamed(const CSymbol Name, TSPtr<CExpressionBase>&& NameIdentifier, TSPtr<CExpressionBase>&& Argument)
        : _Name(Name)
        , _NameIdentifier(Move(NameIdentifier))
        , _Value(Move(Argument))
    {
    }

    const CSymbol& GetName() const                     { return _Name; }
    const TSPtr<CExpressionBase>& GetNameIdentifier() const      { return _NameIdentifier; }
    void SetNameIdentifier(TSPtr<CExpressionBase> NameIdentifier) { _NameIdentifier = Move(NameIdentifier); }
    const TSPtr<CExpressionBase>& GetValue() const     { return _Value; }
    void SetValue(TSPtr<CExpressionBase> Value)        { _Value = Move(Value); }
    virtual EAstNodeType GetNodeType() const override  { return EAstNodeType::Invoke_MakeNamed; }
    virtual CUTF8String  GetErrorDesc() const override { return "named"; }

    virtual void VisitChildren(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitChildren(Visitor);
        Visitor.Visit("Value", _Value);
    }

    virtual void VisitImmediates(SAstVisitor& Visitor) const override
    {
        CExpressionBase::VisitImmediates(Visitor);
        Visitor.VisitImmediate("Name", _Name.AsStringView());
    }

    virtual const CExpressionBase* FindFirstAsyncSubExpr(const CSemanticProgram& Program) const override
    {
        return _Value? _Value->FindFirstAsyncSubExpr(Program) : nullptr;
    }

    virtual bool CanFail(const CAstPackage* Package) const override
    {
        return _Value? _Value->CanFail(Package) : false;
    }

private:
    CSymbol _Name;

    /// Optional pointer to the original identifier for the name. This can be used to preserve the AST
    /// information about the original source text from where the identifier came from and provide context.
    /// (i.e. for qualifying purposes).
    TSPtr<CExpressionBase> _NameIdentifier;
    TSPtr<CExpressionBase> _Value;	
};

/**
 * Represents a package in the AST.
 **/
class AUTORTFM_DISABLE CAstPackage : public CAstNode, public CMemberDefinitions
{
public:
    CUTF8String _Name;
    
    CUTF8String _VersePath; // Verse path of the root module of this package
    CModulePart* _RootModule = nullptr; // Root module representing this package's Verse path

    TArray<const CAstPackage*> _Dependencies; // As specified in package settings
    TArray<const CAstPackage*> _UsedDependencies; // Dependencies actually used

    EVerseScope _VerseScope; // Origin/visibility of Verse code in this package
    EPackageRole _Role; // The role this package plays
    uint32_t _EffectiveVerseVersion; // The effective language version the package targets.

    /// This allows us to determine when a package was uploaded for a given Fortnite release version.
    /// It is a HACK that conditionally enables/disables behaviour in the compiler in order to
    /// support previous mistakes allowed to slip through in previous Verse langauge releases  but
    /// now need to be supported for backwards compatability.
    /// When we can confirm that all Fortnite packages that are currently uploaded are beyond this
    /// version being used in all instances of the codebase, this can then be removed.
    uint32_t _UploadedAtFNVersion = VerseFN::UploadedAtFNVersion::Latest;

    // Track the number of persistent values found per-package.
    int32_t _NumPersistentVars = 0;

    // Track the number of concrete entitlement subclasses found per-package.
    int32_t _NumConcreteEntitlementSubclasses = 0;

    bool _bAllowNative; // If the native attribute is allowed
    bool _bTreatModulesAsImplicit; // If true, module macros in this package's source and digest will be treated as implicit

    bool _bAllowExperimental; // Whether to allow the use of experimental definitions in this package.

    CAstCompilationUnit* _CompilationUnit = nullptr; // Reverse pointer to our owner

    CAstPackage(const CUTF8String& Name,
                const CUTF8String& VersePath,
                const EVerseScope VerseScope,
                const EPackageRole Role,
                const uint32_t EffectiveVerseVersion,
                const uint32_t UploadedAtFNVersion,
                const bool bAllowNative,
                const bool bTreatDefinitionsAsImplicit,
                const bool bAllowExperimental)
        : _Name(Name)
        , _VersePath(VersePath)
        , _VerseScope(VerseScope)
        , _Role(Role)
        , _EffectiveVerseVersion(EffectiveVerseVersion)
        , _UploadedAtFNVersion(UploadedAtFNVersion)
        , _bAllowNative(bAllowNative)
        , _bTreatModulesAsImplicit(bTreatDefinitionsAsImplicit)
        , _bAllowExperimental(bAllowExperimental)
    {}

    // CAstNode interface.
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Context_Package; }
    virtual CUTF8String GetErrorDesc() const override { return "package"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { VisitMembers(Visitor); }
    UE_API virtual void VisitImmediates(SAstVisitor& Visitor) const override;

    // Determine if code in this package can see definitions in a different package.
    UE_API bool CanSeePackage(const CAstPackage& DependencyPackage) const;

    // Determine if the definition originates from this package or any of our dependencies
    UE_API bool CanSeeDefinition(const CDefinition& Definition) const;
};

/**
 * A group of packages that must be compiled as a unit (= a strongly connected component (SCC) in the dependency graph)
 **/
class AUTORTFM_DISABLE CAstCompilationUnit : public CAstNode
{
public:

    const TSRefArray<CAstPackage>& Packages() const { return _Packages; }
    void ReservePackages(int32_t Num) { _Packages.Reserve(Num); }
    void AppendPackage(TSRef<CAstPackage>&& Package) { _Packages.Add(Move(Package)); }

    UE_API EPackageRole GetRole() const;
    UE_API bool IsAllowNative() const;

    // CAstNode interface.
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Context_CompilationUnit; }
    virtual CUTF8String GetErrorDesc() const override { return "compilation unit"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.VisitArray("Packages", _Packages); }

private:

    TSRefArray<CAstPackage> _Packages;
};

/**
 * Represents a project in the AST.
 **/
class AUTORTFM_DISABLE CAstProject : public CAstNode
{
public:
    
    CUTF8String _Name;

    CAstProject(const CUTF8StringView& Name) : _Name(Name) {}

    const TSRefArray<CAstCompilationUnit>& OrderedCompilationUnits() const { return _OrderedCompilationUnits; } // Guaranteed to be sorted in order of dependency
    void ReserveCompilationUnits(int32_t Num) { _OrderedCompilationUnits.Reserve(Num); }
    void AppendCompilationUnit(TSRef<CAstCompilationUnit>&& CompilationUnit) { _OrderedCompilationUnits.Add(Move(CompilationUnit)); }

    UE_API const CAstPackage* FindPackageByName(const CUTF8String& PackageName) const;
    UE_API int32_t GetNumPackages() const;
    
    // CAstNode interface.
    virtual EAstNodeType GetNodeType() const override { return EAstNodeType::Context_Project; }
    virtual CUTF8String GetErrorDesc() const override { return "project"; }
    virtual void VisitChildren(SAstVisitor& Visitor) const override { Visitor.VisitArray("CompilationUnits", _OrderedCompilationUnits); }
    virtual void VisitImmediates(SAstVisitor& Visitor) const override { CAstNode::VisitImmediates(Visitor); Visitor.VisitImmediate("Name", _Name); }

private:

    TSRefArray<CAstCompilationUnit> _OrderedCompilationUnits; // Guaranteed to be sorted in order of dependency
};

//=======================================================================================
// CAstNode Implementations
//=======================================================================================

template<typename FunctionType>
struct AUTORTFM_DISABLE TAstFunctionVisitor : public SAstVisitor
{
    FunctionType _Function;
    TAstFunctionVisitor(FunctionType&& Function) : _Function(ForwardArg<FunctionType>(Function)) {}
    virtual void Visit(const char* FieldName, CAstNode& AstNode) override { _Function(*this, AstNode); }
    virtual void VisitElement(CAstNode& AstNode) override { _Function(*this, AstNode); }
};

template<typename FunctionType>
void CAstNode::VisitChildrenLambda(FunctionType&& Function) const
{
    TAstFunctionVisitor<FunctionType> FunctionVisitor(ForwardArg<FunctionType>(Function));
    VisitChildren(FunctionVisitor);
}

//---------------------------------------------------------------------------------------
template <typename TIn, typename TOut>
struct TAsNullableTraitsOf;

template <typename TOut>
struct TAsNullableTraitsOf<TSPtr<CExpressionBase>, TOut>
{
    using TResult = TSPtr<TOut>;
    static TResult Cast(TSPtr<CExpressionBase> X) { return X.As<TOut>(); }
};

template <typename TOut>
struct TAsNullableTraitsOf<TSRef<CExpressionBase>, TOut>
{
    using TResult = TSPtr<TOut>;
    static TResult Cast(TSRef<CExpressionBase> X) { return X.As<TOut>(); }
};

template <typename TOut>
struct TAsNullableTraitsOf<CExpressionBase*, TOut>
{
    using TResult = TOut*;
    static TResult Cast(CExpressionBase* X) { return static_cast<TOut*>(X); }
};

template <typename TOut>
struct TAsNullableTraitsOf<const CExpressionBase*, TOut>
{
    using TResult = const TOut*;
    static TResult Cast(const CExpressionBase* X) { return static_cast<const TOut*>(X); }
};


template <typename TOut,
          typename TIn,
          typename Traits = TAsNullableTraitsOf<TIn, TOut>>
typename Traits::TResult AsNullable(TIn Expr)
{
#define VISIT_AST_NODE_TYPE(Name, Class)                                \
    if constexpr (std::is_same_v<TOut, Class>)                          \
    {                                                                   \
        if (Expr && Expr->GetNodeType() == EAstNodeType::Name)          \
        {                                                               \
            return Traits::Cast(Expr);                                  \
        }                                                               \
    }
    VERSE_VISIT_AST_NODE_TYPES(VISIT_AST_NODE_TYPE)
#undef VISIT_AST_NODE_TYPE
    return nullptr;
}

inline const char* AsStringLiteral(EAstNodeType Node)
{
#define VISIT_AST_NODE_TYPE(Name, Class) if (Node == EAstNodeType::Name) { return #Name; }
    VERSE_VISIT_AST_NODE_TYPES(VISIT_AST_NODE_TYPE)
#undef VISIT_AST_NODE_TYPE
        ULANG_UNREACHABLE();
}

// if Expr is a CallOp like X[A][B]...[Z] then return Expr as a CExprInvocation, otherwise return
// nullptr
UE_API const CExprInvocation* AsSubscriptCall(
    const CExpressionBase* Expr /* can be null */,
    const CSemanticProgram& Program);

// if Expr is X[A][B]...[Z] then return X, otherwise return Expr
UE_API const CExpressionBase* RemoveSubscripts(
    const CExpressionBase* Expr /* can be null */,
    const CSemanticProgram& Program);

}  // namespace uLang

#undef UE_API
