// Copyright Epic Games, Inc. All Rights Reserved.
// uLang Compiler Public API

#pragma once

#include "uLang/Common/Containers/SharedPointerSet.h"
#include "uLang/Common/Containers/UniquePointerSet.h"
#include "uLang/Semantics/Attributable.h"
#include "uLang/Semantics/Definition.h"
#include "uLang/Semantics/DataDefinition.h"
#include "uLang/Semantics/MemberOrigin.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticInterface.h"
#include "uLang/Semantics/SemanticScope.h"
#include "uLang/Semantics/SmallDefinitionArray.h"
#include "uLang/Semantics/StructOrClass.h"
#include "uLang/Semantics/TypeVariable.h"
#include "uLang/Semantics/VisitSet.h"

#define UE_API VERSECOMPILER_API

namespace uLang
{

// Forward declarations
class CClassDefinition;

/**
 *  Class defining a class instance / object
 *  [Might break off CStructType to differentiate stack based types.]
 **/
class CClass : public CNominalType, public CLogicalScope
{
public:
    static const ETypeKind StaticTypeKind = ETypeKind::Class;
    static const CDefinition::EKind StaticDefinitionKind = CDefinition::EKind::Class;

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Public data
    CClassDefinition* const _Definition;
    const EStructOrClass _StructOrClass;
    CClass* _Superclass;

    TArray<CInterface*> _SuperInterfaces;
    // Flattened array of all interfaces this class inherits (including interfaces from its super-class).
    // Not initially filled out -- cached after we've fully constructed the whole type hierarchy.
    TArray<CInterface*> _AllInheritedInterfaces;

    SEffectSet _ConstructorEffects;

    // Kept alive via _Definition's IrNode's (CExprClassDefinition) Members field.
    // We don't hold a shared reference to this because the Ir tree has to be 
    // destroyed before the AST.
    TArray<CExprCodeBlock*> _IrBlockClauses;

    CClass* _GeneralizedClass{this};

    TArray<STypeVariableSubstitution> _TypeVariableSubstitutions;

    struct Key final
    {
        const TArray<STypeVariableSubstitution>& _TypeVariableSubstitutions;

        friend bool operator==(const Key& Left, const Key& Right)
        {
            return
                Left._TypeVariableSubstitutions == Right._TypeVariableSubstitutions;
        }

        friend bool operator!=(const Key& Left, const Key& Right)
        {
            return
                Left._TypeVariableSubstitutions != Right._TypeVariableSubstitutions;
        }

        friend bool operator<(const Key& Left, const Key& Right)
        {
            return Left._TypeVariableSubstitutions < Right._TypeVariableSubstitutions;
        }
    };

    operator Key() const
    {
        return { _TypeVariableSubstitutions };
    }

    TURefSet<CClass, CClass::Key> _InstantiatedClasses;

    TUPtr<CClass> _OwnedNegativeClass;

    CClass* _NegativeClass;

    bool _bHasCyclesBroken{false};

    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Methods

    // Construct a generalized positive class
    UE_API CClass(
        CClassDefinition*,
        CScope& EnclosingScope,
        CClass* Superclass = nullptr,
        TArray<CInterface*>&& SuperInterfaces = {},
        EStructOrClass = EStructOrClass::Class,
        SEffectSet ConstructorEffects = EffectSets::ClassAndInterfaceDefault);

    // Construct a positive class instantiation
    UE_API CClass(
        CScope* ParentScope,
        CClassDefinition*,
        EStructOrClass,
        CClass* Superclass,
        TArray<CInterface*>&& SuperInterfaces,
        SEffectSet ConstructorEffects,
        CClass* GeneralizedClass,
        TArray<STypeVariableSubstitution>);

    // Construct a negative class from a positive class
    UE_API explicit CClass(CClass* PositiveClass);

    using CTypeBase::GetProgram;

    UE_API const CTypeType* GetTypeType() const;

    UE_API void SetSuperclass(CClass* SuperClass);

    /// Determine if current class is the same class or a subclass of the specified `Class`
    bool IsClass(const CClass& Class) const;

    /// Determine if current class is a subclass / descendant / child of the specified `Class` (and not the same class!)
    bool IsSubclassOf(const CClass& Superclass) const;

    /// Determine if current class is a superclass / ancestor / parent of the specified `Class` (and not the same class!)
    bool IsSuperclassOf(const CClass& Subclass) const;

    /// Determine if current class implements `Interface`
    UE_API bool ImplementsInterface(const CInterface& Interface) const;

    /// Is this class a struct?
    bool IsStruct() const { return _StructOrClass == EStructOrClass::Struct; }

    bool IsNative() const { return Definition()->IsNative(); }

    bool IsNativeRepresentation() const;

    UE_API bool IsAbstract() const;

    UE_API bool IsPersistent() const;

    UE_API bool IsModuleScopedVarWeakMapKey() const;

    UE_API bool IsUnique() const;

    UE_API bool HasOrInheritsVar(CVisitSet& VisitSet) const;

    bool HasOrInheritsVar() const
    {
        CVisitSet VisitSet; 
        return HasOrInheritsVar(VisitSet);
    }

    /// Does this class hold a concrete attribute?
    UE_API bool HasConcreteAttribute() const;

    /// Return first class in the inheritance chain that contains the concrete attribute or null
    UE_API const CClass* FindConcreteBase() const;

    /// Return topmost class in the inheritance chain that contains the concrete attribute or null
    UE_API const CClass* FindInitialConcreteBase() const;

    /// Is this class concrete either by having a concrete attribute or inheriting one
    bool IsExplicitlyConcrete() const override { return FindConcreteBase() != nullptr; }

    /// Does this class hold a castable attribute?
    UE_API bool HasCastableAttribute() const;

    /// Return first class in the inheritance chain that contains the castable attribute. Otherwise null
    UE_API const CNominalType* FindExplicitlyCastableBase() const;

    /// Is this class castable either by having a castable attribute or inheriting one
    bool IsExplicitlyCastable() const override { return FindExplicitlyCastableBase() != nullptr; }

    /// Does this class hold a <final_super_base> attribute?
    UE_API bool HasFinalSuperBaseAttribute() const;
    
    /// Does this class hold a <final_super> attribute?
    UE_API bool HasFinalSuperAttribute() const;

    // CScope interface
    virtual CSymbol GetScopeName() const override { return Definition()->GetName(); }
    virtual const CTypeBase* ScopeAsType() const override { return this; }
    virtual const CDefinition* ScopeAsDefinition() const override { return Definition(); }
    UE_API virtual SAccessLevel GetDefaultDefinitionAccessLevel() const override;
    virtual CScope* GetNegativeScope() const override { return _NegativeClass; }

    // CLogicalScope interface.
    UE_API virtual SmallDefinitionArray FindDefinitions(
        const CSymbol& Name,
        EMemberOrigin Origin,
        const SQualifier& Qualifier,
        const CAstPackage* ContextPackage,
        CVisitSet& VisitSet) const override;

    UE_API virtual void ForAllDefinitions(
        EMemberOrigin Origin,
        const TFunction<void(const CDefinition&)>& Functor,
        CVisitSet& VisitSet) const override;

    // CTypeBase interface.
    UE_API virtual CUTF8String AsCodeRecursive(ETypeSyntaxPrecedence OuterPrecedence, TArray<const CFlowType*>& VisitedFlowTypes, bool bLinkable, ETypeStringFlag Flag) const override;
    UE_API virtual SmallDefinitionArray FindInstanceMember(const CSymbol& Name, EMemberOrigin Origin, const SQualifier& Qualifier, const CAstPackage* ContextPackage, CVisitSet& VisitSet) const override;
    UE_API virtual EComparability GetComparability() const override;
    UE_API EComparability GetComparability(CVisitSet&) const;
    UE_API bool IsPersistable() const override;
    virtual void SetRevision(SemanticRevision Revision) override;
    virtual bool CanBeCustomAccessorDataType() const override { return true; }
    virtual bool CanBePredictsVarDataType() const override { return !IsStruct(); }

    // CNominalType interface.
    virtual const CDefinition* Definition() const override;

    template<typename TFunc>
    void ForEachAncestorClassOrInterface(const TFunc& Func)
    {
        for (CInterface* Interface : _AllInheritedInterfaces)
        {
            Func(Interface, nullptr, Interface);
        }
        for (CClass* Class = _Superclass; Class; Class = Class->_Superclass)
        {
            Func(Class, Class, nullptr);
        }
    }

    const CAttributable& GetAttributes() const;
    CAttributable& GetAttributes();

    bool HasCyclesBroken() const;
    
    bool IsParametric() const
    {
        return !!(_OwnedNegativeClass ? _TypeVariableSubstitutions : _NegativeClass->_TypeVariableSubstitutions).Num();
    }

    UE_API bool IsPersonaConstructible() const;

private:
    // The slow-path for `IsClass`, used when `HasCyclesBroken` is false.
    UE_API bool IsClassSlowPath(const CClass& Class) const;
};  // CClass

class CClassDefinition : public CDefinition, public CClass
{
public:
    CAttributable _EffectAttributable;
    TOptional<SAccessLevel> _ConstructorAccessLevel;

    CClassDefinition(const CSymbol& ClassName,
                 CScope& EnclosingScope,
                 CClass* Superclass = nullptr,
                 TArray<CInterface*>&& SuperInterfaces = {},
                 EStructOrClass StructOrClass = EStructOrClass::Class)
        : CDefinition(StaticDefinitionKind, EnclosingScope, ClassName)
        , CClass(this, EnclosingScope, Superclass, Move(SuperInterfaces), StructOrClass)
    {}

    using CDefinition::GetAttributes;

    SAccessLevel DerivedConstructorAccessLevel() const
    {
        return _ConstructorAccessLevel.Get(SAccessLevel::EKind::Public);
    }

    // CDefinition interface.
    UE_API void SetAstNode(CExprClassDefinition* AstNode);
    UE_API CExprClassDefinition* GetAstNode() const;

    UE_API void SetIrNode(CExprClassDefinition* AstNode);
    UE_API CExprClassDefinition* GetIrNode(bool bForce = false) const;

    virtual const CLogicalScope* DefinitionAsLogicalScopeNullable() const override { return this; }

    UE_API TArray<TSRef<CExpressionBase>> FindMembersWithPredictsAttribute(bool bIncludeSupers = false) const;

    bool IsPersistenceCompatConstraint() const override { return _bPersistenceCompatConstraint; }

    UE_API void MarkPersistenceCompatConstraint() const;

    bool IsEntitlementCompatConstraint() const override { return _bEntitlementCompatConstraint; }

    UE_API void MarkEntitlementCompatConstraint() const;

private:
    mutable bool _bPersistenceCompatConstraint{false};
    mutable bool _bEntitlementCompatConstraint{false};
};

class CInstantiatedClass : public CInstantiatedType
{
public:
    CInstantiatedClass(CSemanticProgram& Program, const CClass& Class, ETypePolarity Polarity, TArray<STypeVariableSubstitution> Arguments)
        : CInstantiatedType(Program, Polarity, Move(Arguments))
        , _Class(&Class)
    {
    }

    virtual bool CanBeCustomAccessorDataType() const override { return true; };

protected:
    UE_API virtual const CNormalType& CreateNormalType() const override;

private:
    const CClass* _Class;
};

// Eagerly instantiate a class.
VERSECOMPILER_API CClass* InstantiateClass(const CClass&, ETypePolarity, const TArray<STypeVariableSubstitution>&);

//=======================================================================================
// CClass Inline Methods
//=======================================================================================

//---------------------------------------------------------------------------------------
inline const CDefinition* CClass::Definition() const
{
    return _Definition;
}

//---------------------------------------------------------------------------------------
inline void CClass::SetSuperclass(CClass* Superclass)
{
    // TODO-Verse: add _Subclasses
    _Superclass = Superclass;
}

//---------------------------------------------------------------------------------------
inline bool CClass::IsClass(const CClass& Class) const
{
    if (ULANG_LIKELY(HasCyclesBroken()))
    {
        return (this == &Class) || this->IsSubclassOf(Class);
    }
    else
    {
        return IsClassSlowPath(Class);
    }
}

//---------------------------------------------------------------------------------------
inline bool CClass::IsSubclassOf(const CClass& Superclass) const
{
    const CClass* RelatedClass = _Superclass;

    while (RelatedClass)
    {
        if (RelatedClass == &Superclass)
        {
            return true;
        }

        RelatedClass = RelatedClass->_Superclass;
    }

    return false;
}

//---------------------------------------------------------------------------------------
inline bool CClass::IsSuperclassOf(const CClass& Subclass) const
{
    const CClass* RelatedClass = Subclass._Superclass;

    while (RelatedClass)
    {
        if (RelatedClass == this)
        {
            return true;
        }

        RelatedClass = RelatedClass->_Superclass;
    }

    return false;
}

//---------------------------------------------------------------------------------------
inline void CClass::SetRevision(SemanticRevision Revision)
{
    CClass* Class = this;
    do
    {
        ULANG_ENSUREF(Revision >= Class->GetRevision(), "Revision to be set must not be smaller than existing revisions.");
        if (Class->GetRevision() == Revision)
        {
            break;
        }

        Class->CLogicalScope::SetRevision(Revision);
        Class = Class->_Superclass;
    } while (Class);
}

inline const CAttributable& CClass::GetAttributes() const
{
    return _Definition->GetAttributes();
}

inline CAttributable& CClass::GetAttributes()
{
    return _Definition->GetAttributes();
}

inline bool CClass::HasCyclesBroken() const
{
    return _Definition->_bHasCyclesBroken;
}

}  // namespace uLang

#undef UE_API
