// Copyright Epic Games, Inc. All Rights Reserved.
// uLang Compiler Public API

#pragma once

#include "uLang/Common/Algo/Cases.h"
#include "uLang/Common/Containers/Array.h"
#include "uLang/Common/Containers/SharedPointer.h"
#include "uLang/Common/Containers/Function.h"
#include "uLang/Common/Text/UTF8String.h"
#include "uLang/Common/Text/UTF8StringBuilder.h"
#include "uLang/Semantics/CaptureScope.h"
#include "uLang/Semantics/FilteredDefinitionRange.h"
#include "uLang/Semantics/MemberOrigin.h"
#include "uLang/Semantics/Revision.h"
#include "uLang/Semantics/SmallDefinitionArray.h"
#include "uLang/Semantics/StructOrClass.h"
#include "uLang/Semantics/VisitSet.h"

#define UE_API VERSECOMPILER_API

namespace uLang
{
struct SAccessLevel;
struct SQualifier;
class CAstPackage;
class CAstCompilationUnit;
class CCaptureControlScope;
class CControlScope;
class CClass;
class CClassDefinition;
class CDataDefinition;
class CEnumeration;
class CEnumerator;
class CFunction;
class CInterface;
class CLogicalScope;
class CModule;
class CModulePart;
class CModuleAlias;
class CSnippet;
class CSemanticAnalyzerImpl;
class CSemanticProgram;
class CScopedAccessLevelDefinition;
class CTypeBase;
class CTypeAlias;
class CTypeScope;
class CTypeType;
class CTypeVariable;

/**
 * Stores a resolved definition and the context that it was resolved from
 */
struct SResolvedDefinition
{
    CDefinition*           _Definition;
    const CDataDefinition* _Context;

    SResolvedDefinition(CDefinition* Definition)                                 : _Definition(Definition), _Context(nullptr) {}
    SResolvedDefinition(CDefinition* Definition, const CDataDefinition* Context) : _Definition(Definition), _Context(Context) {}
};

/**
 * An array of resolved definitions and their associated contexts
 */
using SResolvedDefinitionArray = TArrayG<SResolvedDefinition, TInlineElementAllocator<1>>;

enum class EPathMode : uint8_t { Default, PrefixSeparator, PackageRelative, PackageRelativeWithRoot };

/**
 * A nested scope - program, module or class
 */
class CScope
{
public:
    enum class EKind : uint8_t
    {
        Program,
        CompatConstraintRoot,
        Module,
        ModulePart,
        Snippet,
        Class,
        Function,
        ControlScope, // A nested scope within a function body
        Interface,
        Type,
        Enumeration
    };

    static UE_API const char* KindToCString(EKind Kind);

    CScope(EKind Kind, CScope* Parent, CSemanticProgram& Program) : _Kind(Kind), _Parent(Parent), _Program(Program) {}
    UE_API virtual ~CScope();

    // Delete the generated move/copy constructors so that they don't require full definitions of the types referenced by TSRefArray.
    CScope(const CScope&) = delete;
    CScope(CScope&&) = delete;

    virtual CSymbol GetScopeName() const = 0;
    virtual const CTypeBase* ScopeAsType() const { return nullptr; }
    virtual const CDefinition* ScopeAsDefinition() const { return nullptr; }

    virtual SAccessLevel GetDefaultDefinitionAccessLevel() const { return SAccessLevel::EKind::Internal; }

    EKind GetKind() const { return _Kind; }
    CScope* GetParentScope() const { return _Parent; }
    UE_API CScope* GetScopeOfKind(EKind);
    UE_API const CScope* GetScopeOfKind(EKind) const;
    using EPathMode = uLang::EPathMode;
    UE_API void GetScopePath(uLang::CUTF8StringBuilder& OutBuilder, uLang::UTF8Char SeparatorChar = '.', EPathMode Mode = EPathMode::Default) const;
    UE_API CUTF8String GetScopePath(uLang::UTF8Char SeparatorChar = '.', EPathMode Mode = EPathMode::Default) const;
    UE_API const CModule* GetModule() const;
    UE_API CModule* GetModule();
    UE_API const CModulePart* GetModulePart() const;
    UE_API CModulePart* GetModulePart();
    UE_API CAstPackage* GetPackage() const;
    UE_API CAstCompilationUnit* GetCompilationUnit() const;
    UE_API const CSnippet* GetSnippet() const;
    UE_API CCaptureScope* GetCaptureScope();
    UE_API const CCaptureScope* GetCaptureScope() const;
    UE_API const TSPtr<CSymbolTable>& GetSymbols() const;
    CSemanticProgram& GetProgram() const { return _Program; }

    /// If this is a parametric type, get the scope of those parameters; otherwise returns this scope.
    UE_API const CScope& GetParametricTypeScope() const;

    /// Get the innermost logical scope that is or contains this scope.
    UE_API const CLogicalScope& GetLogicalScope() const;
    CLogicalScope& GetLogicalScope() { return const_cast<CLogicalScope&>(static_cast<const CScope*>(this)->GetLogicalScope()); }

    /// Iff this scope is a logical scope, return it a pointer to it. Otherwise, return null.
    virtual const CLogicalScope* AsLogicalScopeNullable() const { return nullptr; }
    virtual CLogicalScope* AsLogicalScopeNullable() { return nullptr; }

    bool IsLogicalScope() const { return AsLogicalScopeNullable() != nullptr; }

    const CLogicalScope* GetEnclosingClassOrInterface() const { return const_cast<CScope*>(this)->GetEnclosingClassOrInterface(); }
    UE_API CLogicalScope* GetEnclosingClassOrInterface();

    virtual CScope* GetNegativeScope() const { return nullptr; }

    // Check if this module is the same or a child of another
    UE_API bool IsSameOrChildOf(const CScope* Other) const;

    // Determines if this is either a function body or a nested scope within a function body
    bool IsControlScope() const { return _Kind == Cases<EKind::Function, EKind::ControlScope>; }

    // Determines if inside a type scope, ignoring control scope
    UE_API bool IsInsideTypeScope() const;

    // Determines if this is a module or snippet scope.
    bool IsModuleOrSnippet() const { return GetKind() == Cases<EKind::Module, EKind::ModulePart, EKind::Snippet>; }

    // Determines if the definitions in this scope are built-in.
    UE_API bool IsBuiltInScope() const;

    UE_API CModule& CreateModule(const CSymbol& ModuleName);
    UE_API CClassDefinition& CreateClass(const CSymbol& ClassName, CClass* Superclass = nullptr, TArray<CInterface*>&& SuperInterfaces = {}, EStructOrClass StructOrClass = EStructOrClass::Class);
    UE_API CEnumeration& CreateEnumeration(const CSymbol& EnumerationName);
    UE_API CInterface& CreateInterface(const CSymbol& InterfaceName, const TArray<CInterface*>& SuperInterfaces = {});
    UE_API TSRef<CFunction> CreateFunction(const CSymbol FunctionName);
    UE_API TSRef<CDataDefinition> CreateDataDefinition(const CSymbol VarName);
    UE_API TSRef<CDataDefinition> CreateDataDefinition(const CSymbol VarName, const CTypeBase* Type);
    UE_API TSRef<CTypeAlias> CreateTypeAlias(const CSymbol Name);
    UE_API TSRef<CTypeVariable> CreateTypeVariable(const CSymbol Name, const CTypeBase* NegativeType, const CTypeBase* PositiveType);
    UE_API TSRef<CModuleAlias> CreateModuleAlias(const CSymbol Name);
    UE_API TSRef<CScopedAccessLevelDefinition> CreateScopedAccessLevelDefinition(TOptional<CSymbol> ClassName);

    // Using declarations
    void AddUsingScope(const CLogicalScope* UsingScope) { _UsingScopes.AddUnique(UsingScope); }
    const TArray<const CLogicalScope*>& GetUsingScopes() const { return _UsingScopes; }

    // Add a local context to infer from a using declaration - return nullptr if added and conflicting context if type/value domain was already previously added
    UE_API const CDataDefinition* AddUsingInstance(const CDataDefinition* UsingContext);
    const TArray<const CDataDefinition*>& GetUsingInstances() const { return _UsingInstances; }

    static UE_API void ResolvedDefnsAppend(SResolvedDefinitionArray* ResolvedDefns, const SmallDefinitionArray& Definitions);
    static UE_API void ResolvedDefnsAppendWithContext(SResolvedDefinitionArray* ResolvedDefns, const SmallDefinitionArray& Definitions, const CDataDefinition* Context);

    /// Look for a definition in this scope and all parent scopes and aliases
    UE_API SResolvedDefinitionArray ResolveDefinition(const CSymbol& Name, const SQualifier& Qualifier = SQualifier::Unknown(), const CAstPackage* ContextPackage = nullptr) const;

    UE_API TSRef<CControlScope> CreateNestedControlScope();

    TSRef<CCaptureControlScope> CreateNestedCaptureControlScope();

    const TSRefArray<CControlScope>& GetNestedControlScopes() const { return _NestedControlScopes; }

    UE_API TSRef<CTypeScope> CreateNestedTypeScope();

    // Determines whether this scope was authored by Epic
    UE_API bool IsAuthoredByEpic() const;

    // Determines whether this scope can access Epic-internal definitions.
    // This differs from IsAuthoredByEpic by allowing packages with Scope=InternalUser to access epic-internal definitions.
    UE_API bool CanAccessEpicInternal() const;

    bool _bIsParametricTypeScope = false;

    mutable uint64_t _NextUniqueSymbolId = 0;
protected:
    friend class CSemanticAnalyzerImpl;
    friend class CDefinition;
    friend class CDataDefinition;
    // Returns whether some definition is accessible from this scope.
    // When checking accessibility, you probably want to use CDefinition::IsAccessibleFrom
    // instead of this.
    UE_API bool CanAccess(const CDefinition& Definition, const SAccessLevel& DefinitionAccessLevel) const;

    // If we are a program, module etc.
    EKind _Kind;

    // The enclosing scope for this scope
    CScope* _Parent;

    // The semantic program these types belongs to
    CSemanticProgram& _Program;

    // `using` declarations referring to other scopes / modules
    TArray<const CLogicalScope*> _UsingScopes;

    // `using` declarations referring to implied contexts / receivers
    TArray<const CDataDefinition*> _UsingInstances;

    // Nested control scopes
    TSRefArray<CControlScope> _NestedControlScopes;

    // Nested type scopes
    TSRefArray<CTypeScope> _NestedTypeScopes;

private:
    template <typename T>
    static T* GetScopeOfKind(T* This, EKind Kind);

    template <typename T, typename U>
    static T* GetCaptureScope(U* This);
};

/**
 * A scope that can contain definitions
 */
class CLogicalScope : public CScope
{
public:

    CLogicalScope(EKind Kind, CScope* Parent, CSemanticProgram& Program) : CScope(Kind, Parent, Program) {}
    UE_API virtual ~CLogicalScope();

    // Delete the generated move/copy constructors so that they don't require full definitions of the types referenced by TSRefArray.
    CLogicalScope(const CLogicalScope&) = delete;
    CLogicalScope(CLogicalScope&&) = delete;

    /// Iterates through all the logical scopes nested inside this scope
    UE_API EIterateResult IterateRecurseLogicalScopes(const TFunction<EVisitResult(const CLogicalScope&)>& Functor) const;
    UE_API EIterateResult IterateRecurseLogicalScopes(TFunction<EVisitResult(const CLogicalScope&)>&& Functor) const;

    const TArray<TSRef<CDefinition>>& GetDefinitions() const { return _Definitions; }
    TMap<CSymbol, SmallDefinitionArray>& GetDefinitionNameMap() { return _DefinitionNameMap; }

    template<typename FilterClass>
    TFilteredDefinitionRange<FilterClass> GetDefinitionsOfKind() const;

    UE_API virtual SmallDefinitionArray FindDefinitions(
        const CSymbol& Name,
        EMemberOrigin Origin,
        const SQualifier& Qualifier,
        const CAstPackage* ContextPackage,
        CVisitSet& VisitSet) const;

    SmallDefinitionArray FindDefinitions(
        const CSymbol& Name,
        EMemberOrigin Origin = EMemberOrigin::InheritedOrOriginal,
        const SQualifier& Qualifier = SQualifier::Unknown(),
        const CAstPackage* ContextPackage = nullptr) const
    {
        CVisitSet VisitSet;
        return FindDefinitions(Name, Origin, Qualifier, ContextPackage, VisitSet);
    }

    UE_API virtual void ForAllDefinitions(
        EMemberOrigin Origin,
        const TFunction<void(const CDefinition&)>& Functor,
        CVisitSet& VisitSet) const;

    void ForAllDefinitions(
        EMemberOrigin Origin,
        const TFunction<void(const CDefinition&)>& Functor) const
    {
        CVisitSet VisitSet;
        ForAllDefinitions(Origin, Functor, VisitSet);
    }

    template<typename FilterClass>
    FilterClass* FindFirstDefinitionOfKind(
        const CSymbol& Name,
        EMemberOrigin Origin,
        const SQualifier& Qualifier,
        const CAstPackage* ContextPackage,
        CVisitSet& VisitSet) const;

    template<typename FilterClass>
    FilterClass* FindFirstDefinitionOfKind(
        const CSymbol& Name,
        EMemberOrigin Origin = EMemberOrigin::InheritedOrOriginal,
        const SQualifier& Qualifier = SQualifier::Unknown(),
        const CAstPackage* ContextPackage = nullptr) const
    {
        CVisitSet VisitSet;
        return FindFirstDefinitionOfKind<FilterClass>(Name, Origin, Qualifier, ContextPackage, VisitSet);
    }

    UE_API virtual void SetRevision(SemanticRevision Revision);
    SemanticRevision GetRevision() const { return _CumulativeRevision; }

    // If this scope is in the given visit set, return false.
    // Otherwise, add this scope to the visit set and return true.
    bool TryMarkVisited(CVisitSet& VisitSet) const
    {
        return VisitSet.TryVisit(_LastVisitKey);
    }

    // Allocates an ordinal for the next definition in this scope.
    int32_t AllocateNextDefinitionOrdinal()
    {
        return _NextDefinitionOrdinal++;
    }

    /// Get the matching override definition in this class for the argument, if there is any
    UE_API const CDefinition* FindOverrideFor(const CDefinition& Definition) const;

    // CScope interface.
    virtual const CLogicalScope* AsLogicalScopeNullable() const override { return this; }
    virtual CLogicalScope* AsLogicalScopeNullable() override { return this; }

    UE_API SQualifier AsQualifier() const;

    friend class CScope;

    void AddDefinitionToLogicalScope(TSRef<CDefinition>&& NewDefinition);

    UE_API bool HasVar() const;

    UE_API void InstantiateMembers(
        CLogicalScope& InstantiatedPositiveScope,
        CLogicalScope& InstantiatedNegativeScope,
        const CNormalType& InstantiatedType,
        const TArray<STypeVariableSubstitution>& Substitutions) const;

    UE_API void SetNegativeMemberTypes(CLogicalScope& InstantiatedNegativeScope) const;

protected:
    /// When anything in this class (methods, data members etc.) or its subclasses was last modified/deleted
    SemanticRevision _CumulativeRevision = 1; // Initialize semantic revision to 1 to trigger full rebuild on first compile

    // To make sure we don't visit the same scope twice during an iteration
    CVisitKey _LastVisitKey;

private:
    // All definitions in this scope.
    TArray<TSRef<CDefinition>> _Definitions;

    // These definition references depend on the _Definitions array to manage lifetimes:
    // All the definitions mapped by name since that's how we often look them up
    TMap<CSymbol, SmallDefinitionArray> _DefinitionNameMap;
    // A collection of the _Definitions that are also logical scopes
    TArray<const CLogicalScope*> _LogicalSubScopes;

    // The next ordinal to assign to definitions within this scope.
    int32_t _NextDefinitionOrdinal{ 0 };
};

template<typename FilterClass>
TFilteredDefinitionRange<FilterClass> CLogicalScope::GetDefinitionsOfKind() const
{
    return TFilteredDefinitionRange<FilterClass>(_Definitions.begin(), _Definitions.end());
}

template<typename FilterClass>
FilterClass* CLogicalScope::FindFirstDefinitionOfKind(const CSymbol& Name, EMemberOrigin Origin, const SQualifier& Qualifier, const CAstPackage* ContextPackage, CVisitSet& VisitSet) const
{
    SmallDefinitionArray Definitions = FindDefinitions(Name, Origin, Qualifier, ContextPackage, VisitSet);
    for (CDefinition* Definition : Definitions)
    {
        if (FilterClass* Result = Definition->AsNullable<FilterClass>())
        {
            return Result;
        }
    }
    return nullptr;
}

};

#undef UE_API
