// Copyright Epic Games, Inc. All Rights Reserved.

#include "uLang/Semantics/SemanticInterface.h"

#include "uLang/Common/Algo/FindIf.h"
#include "uLang/Semantics/MemberOrigin.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SmallDefinitionArray.h"
#include "uLang/Semantics/TypeVariable.h"
#include "uLang/Semantics/VisitSet.h"

namespace uLang
{
CInterface::CInterface(CInterface* PositiveInterface)
    : CDefinition(StaticDefinitionKind, PositiveInterface->_EnclosingScope, PositiveInterface->GetName())
    , CNominalType(StaticTypeKind, PositiveInterface->GetProgram())
    , CLogicalScope(CScope::EKind::Interface, PositiveInterface->GetParentScope(), PositiveInterface->GetProgram())
    , _SuperInterfaces(GetNegativeInterfaces(PositiveInterface->_SuperInterfaces))
    , _ConstructorEffects(PositiveInterface->_ConstructorEffects)
    , _GeneralizedInterface(PositiveInterface->_GeneralizedInterface)
    , _NegativeInterface(PositiveInterface)
{
}

CUTF8String CInterface::AsCodeRecursive(ETypeSyntaxPrecedence OuterPrecedence, TArray<const CFlowType*>& VisitedFlowTypes, bool bLinkable, ETypeStringFlag Flag) const
{
    if (GetParentScope()->GetKind() != CScope::EKind::Function)
    {
        return CNominalType::AsCodeRecursive(OuterPrecedence, VisitedFlowTypes, bLinkable, Flag);
    }
    CUTF8StringBuilder Builder;
    if (Flag == ETypeStringFlag::Qualified)
    {
        CUTF8String Name = GetQualifiedNameString(*GetParentScope()->ScopeAsDefinition());
        Builder.Append(Name.AsCString());
    }
    else
    {
        CSymbol Name = GetParentScope()->GetScopeName();
        Builder.Append(Name.AsStringView());
    }

    Builder.Append('(');

    const TArray<STypeVariableSubstitution>* InstTypeVariables;
    if (_OwnedNegativeInterface)
    {
        InstTypeVariables = &_TypeVariableSubstitutions;
    }
    else
    {
        InstTypeVariables = &_NegativeInterface->_TypeVariableSubstitutions;
    }
    const char* Separator = "";
    for (const STypeVariableSubstitution& InstTypeVariable : *InstTypeVariables)
    {
        if (!InstTypeVariable._TypeVariable->_ExplicitParam || !InstTypeVariable._TypeVariable->_NegativeTypeVariable)
        {
            continue;
        }
        Builder.Append(Separator);
        Separator = ",";
        const CTypeBase* Type;
        if (_OwnedNegativeInterface)
        {
            Type = InstTypeVariable._PositiveType;
        }
        else
        {
            Type = InstTypeVariable._NegativeType;
        }
        Builder.Append(Type->AsCodeRecursive(ETypeSyntaxPrecedence::List, VisitedFlowTypes, bLinkable, Flag));
    }

    Builder.Append(')');

    return Builder.MoveToString();
}

SmallDefinitionArray CInterface::FindDefinitions(const CSymbol& Name, EMemberOrigin Origin, const SQualifier& Qualifier, const CAstPackage* ContextPackage, CVisitSet& VisitSet) const
{
    SmallDefinitionArray Result = CLogicalScope::FindDefinitions(Name, Origin, Qualifier, ContextPackage, VisitSet);
    if (Origin != EMemberOrigin::Original)
    {
        Result.Append(FindInstanceMember(Name, EMemberOrigin::Inherited, Qualifier, ContextPackage, VisitSet));
    }
    return Result;
}

void CInterface::ForAllDefinitions(
    EMemberOrigin Origin,
    const TFunction<void(const CDefinition&)>& Functor,
    CVisitSet& VisitSet) const
{
    if (TryMarkVisited(VisitSet))
    {
        CLogicalScope::ForAllDefinitions(Origin, Functor, VisitSet);
        if (Origin != EMemberOrigin::Original)
        {
            for (CInterface* SuperInterface : _SuperInterfaces)
            {
                SuperInterface->ForAllDefinitions(EMemberOrigin::InheritedOrOriginal, Functor, VisitSet);
            }
        }
    }
}

SmallDefinitionArray CInterface::FindInstanceMember(const CSymbol& Name, EMemberOrigin Origin, const SQualifier& Qualifier, const CAstPackage* ContextPackage, CVisitSet& VisitSet) const
{
    SmallDefinitionArray Result;
    if (Origin == EMemberOrigin::Inherited || TryMarkVisited(VisitSet))
    {
        if (Origin != EMemberOrigin::Inherited)
        {
            // FindDefinition will filter on Qualifier
            Result.Append(FindDefinitions(Name, EMemberOrigin::Original, Qualifier, ContextPackage, VisitSet));
        }

        if (Origin != EMemberOrigin::Original)
        {
            for (const CInterface* SuperInterface : _SuperInterfaces)
            {
                Result.Append(SuperInterface->FindInstanceMember(Name, EMemberOrigin::InheritedOrOriginal, Qualifier, ContextPackage, VisitSet));
            }
        }
    }

    return Result;
}

EComparability CInterface::GetComparability() const
{
    CVisitSet VisitSet;
    return GetComparability(VisitSet);
}

EComparability CInterface::GetComparability(CVisitSet& VisitSet) const
{
    if (!TryMarkVisited(VisitSet))
    {
        return EComparability::Incomparable;
    }

    const CSemanticProgram& Program = _GeneralizedInterface->GetProgram();

    // Should perhaps use _GeneralizedInterface->IsUnique(), but that isn't resolved
    // until the semantic analyzer is past the Deferred_Attributes phase
    if (_GeneralizedInterface->_EffectAttributable.HasAttributeClassHack(Program._uniqueClass, Program))
    {
        return EComparability::ComparableAndHashable;
    }

    for (const CInterface* Interface : _SuperInterfaces)
    {
        if (Interface->GetComparability(VisitSet) == EComparability::ComparableAndHashable)
        {
            return EComparability::ComparableAndHashable;
        }
    }

    return EComparability::Incomparable;
}

// Only callable Phase > Deferred_Attributes
bool CInterface::IsUnique() const
{
    if (_GeneralizedInterface->_EffectAttributable.HasAttributeClass(GetProgram()._uniqueClass, GetProgram()))
    {
        return true;
    }

    // The <unique> effect is heritable
    for (const CInterface* Interface : _SuperInterfaces)
    {
        if (Interface->IsUnique())
        {
            return true;
        }
    }

    return false;
}

bool CInterface::HasOrInheritsVar(CVisitSet& VisitSet) const
{
    if (!TryMarkVisited(VisitSet))
    {
        return false;
    }
    if (VerseFN::UploadedAtFNVersion::DisallowGlobalInstancesOfInterfaceVars(GetPackage()->_UploadedAtFNVersion))
    {
        if (CLogicalScope::HasVar())
        {
            return true;
        }
    }
    for (CInterface* SuperInterface : _SuperInterfaces)
    {
        if (SuperInterface->HasOrInheritsVar(VisitSet))
        {
            return true;
        }
    }
    return false;
}

bool CInterface::HasCastableAttribute() const
{
    return _EffectAttributable.HasAttributeClass(GetProgram()._castableClass, GetProgram());
}

const CNominalType* CInterface::FindExplicitlyCastableBase() const
{
    if (HasCastableAttribute())
    {
        return this;
    }

    for (const CInterface* Interface : _SuperInterfaces)
    {
        if (const CNominalType* Result = Interface->FindExplicitlyCastableBase())
        {
            return Result;
        }
    }

    return nullptr;
}

bool CInterface::HasFinalSuperBaseAttribute() const
{
    return _EffectAttributable.HasAttributeClass(GetProgram()._finalSuperBaseClass, GetProgram());
}

bool CInterface::IsInterface(const CInterface& Interface) const
{
    if (this == &Interface)
    {
        return true;
    }
    for (const CInterface* SuperInterface : _SuperInterfaces)
    {
        if (SuperInterface->IsInterface(Interface))
        {
            return true;
        }
    }
    return false;
}

const CNormalType& CInstantiatedInterface::CreateNormalType() const
{
    if (CInterface* InstInterface = InstantiateInterface(*_Interface, GetPolarity(), GetSubstitutions()))
    {
        return *InstInterface;
    }
    return *_Interface;
}

static CInterface* FindInstantiatedInterface(const TURefArray<CInterface>& InstInterfaces, const TArray<STypeVariableSubstitution>& InstTypeVariables)
{
    auto Last = InstInterfaces.end();
    auto I = uLang::FindIf(InstInterfaces.begin(), Last, [&](CInterface* InstInterface)
    {
        return InstInterface->_TypeVariableSubstitutions == InstTypeVariables;
    });
    if (I == Last)
    {
        return nullptr;
    }
    return *I;
}

CInterface* InstantiatePositiveInterface(const CInterface& Interface, const TArray<STypeVariableSubstitution>& Substitutions)
{
    if (Interface.GetParentScope()->GetKind() != CScope::EKind::Function)
    {
        return nullptr;
    }

    TArray<STypeVariableSubstitution> InstTypeVariables = InstantiateTypeVariableSubstitutions(
        Interface._TypeVariableSubstitutions,
        Substitutions);

    CInterface* GeneralizedInterface = Interface._GeneralizedInterface;
    TURefArray<CInterface>& InstInterfaces = GeneralizedInterface->_InstantiatedInterfaces;
    if (CInterface* InstInterface = FindInstantiatedInterface(InstInterfaces, InstTypeVariables))
    {
        return InstInterface;
    }

    int32_t I = InstInterfaces.AddNew(
        *Interface.GetParentScope(),
        Interface.GetName(),
        InstantiatePositiveInterfaces(Interface._SuperInterfaces, Substitutions),
        Interface._ConstructorEffects,
        GeneralizedInterface,
        Move(InstTypeVariables),
        Interface._bHasCyclesBroken);
    CInterface* InstInterface = InstInterfaces[I];

    Interface.InstantiateMembers(*InstInterface, *InstInterface->_NegativeInterface, *InstInterface, Substitutions);

    return InstInterface;
}

TArray<STypeVariableSubstitution> InstantiateTypeVariableSubstitutions(
    const TArray<STypeVariableSubstitution>& TypeVariables,
    const TArray<STypeVariableSubstitution>& Substitutions)
{
    TArray<STypeVariableSubstitution> InstTypeVariables;
    InstTypeVariables.Reserve(TypeVariables.Num());
    for (const STypeVariableSubstitution& TypeVariable : TypeVariables)
    {
        const CTypeBase* NegativeType = TypeVariable._NegativeType;
        const CTypeBase* PositiveType = TypeVariable._PositiveType;
        NegativeType = SemanticTypeUtils::Substitute(*NegativeType, ETypePolarity::Negative, Substitutions);
        PositiveType = SemanticTypeUtils::Substitute(*PositiveType, ETypePolarity::Positive, Substitutions);
        InstTypeVariables.Add({TypeVariable._TypeVariable, NegativeType, PositiveType});
    }
    return InstTypeVariables;
}

CInterface* InstantiateInterface(const CInterface& Interface, ETypePolarity Polarity, const TArray<STypeVariableSubstitution>& Substitutions)
{
    switch (Polarity)
    {
    case ETypePolarity::Negative:
        if (CInterface* InstInterface = InstantiatePositiveInterface(*Interface._NegativeInterface, Substitutions))
        {
            return InstInterface->_NegativeInterface;
        }
        return nullptr;
    case ETypePolarity::Positive: return InstantiatePositiveInterface(Interface, Substitutions);
    default: ULANG_UNREACHABLE();
    }
}

TArray<CInterface*> InstantiatePositiveInterfaces(const TArray<CInterface*>& Interfaces, const TArray<STypeVariableSubstitution>& Substitutions)
{
    TArray<CInterface*> InstInterfaces;
    InstInterfaces.Reserve(Interfaces.Num());
    for (CInterface* Interface : Interfaces)
    {
        if (CInterface* InstInterface = InstantiatePositiveInterface(*Interface, Substitutions))
        {
            Interface = InstInterface;
        }
        InstInterfaces.Add(Interface);
    }
    return InstInterfaces;
}

TArray<CInterface*> GetNegativeInterfaces(const TArray<CInterface*>& Interfaces)
{
    TArray<CInterface*> NegativeInterfaces;
    NegativeInterfaces.Reserve(Interfaces.Num());
    for (CInterface* Interface : Interfaces)
    {
        NegativeInterfaces.Add(Interface->_NegativeInterface);
    }
    return NegativeInterfaces;
}
}
