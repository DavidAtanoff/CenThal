// Copyright Epic Games, Inc. All Rights Reserved.

#include "uLang/Semantics/DataDefinition.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticInterface.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticScope.h"

namespace uLang
{
EPersistenceExternalAccess GetPersistenceExternalAccess(const CAttributable& Attributes, const CSemanticProgram& Program)
{
    EPersistenceExternalAccess Result = EPersistenceExternalAccess::Internal;
    if (Attributes.HasAttributeClassHack(Program._expose_to_web_data_sync_service_reads, Program))
    {
        Result = static_cast<EPersistenceExternalAccess>(static_cast<std::uint8_t>(Result) | static_cast<std::uint8_t>(EPersistenceExternalAccess::Read));
    }
    if (Attributes.HasAttributeClassHack(Program._expose_to_web_data_sync_service_writes, Program))
    {
        Result = static_cast<EPersistenceExternalAccess>(static_cast<std::uint8_t>(Result) | static_cast<std::uint8_t>(EPersistenceExternalAccess::Write));
    }
    return Result;
}

void CDataDefinition::SetAstNode(CExprDefinition* AstNode)
{
    CDefinition::SetAstNode(AstNode);
}
CExprDefinition* CDataDefinition::GetAstNode() const
{
    return static_cast<CExprDefinition*>(CDefinition::GetAstNode());
}

void CDataDefinition::SetIrNode(CExprDefinition* AstNode)
{
    CDefinition::SetIrNode(AstNode);
}
CExprDefinition* CDataDefinition::GetIrNode(bool bForce) const
{
    return static_cast<CExprDefinition*>(CDefinition::GetIrNode(bForce));
}

// Only callable Phase > Deferred_Attributes
bool CDataDefinition::IsNativeRepresentation() const
{
    if (IsNative())
    {
        return true;
    }

    if (HasPredictsAttribute())
    {
        // A data member with <predicts> and non-class type uses a native representation.
        const CClassDefinition* MaybeClass = SemanticTypeUtils::RemovePointer(GetType(), ETypePolarity::Positive)->GetNormalType().AsNullable<CClassDefinition>();
        if (!MaybeClass)
        {
            return true;
        }
    }

    CSemanticProgram& Program = _EnclosingScope.GetProgram();
    uLang::CClassDefinition* ReplicatedClass = Program._replicated.Get();
    if (ReplicatedClass && HasAttributeClass(ReplicatedClass, Program))
    {
        return true;
    }

    return false;
}

void CDataDefinition::GetScopePath(uLang::CUTF8StringBuilder& OutBuilder, uLang::UTF8Char SeparatorChar, EPathMode Mode) const
{
    _EnclosingScope.GetScopePath(OutBuilder, SeparatorChar, Mode);
    if (OutBuilder.IsEmpty())
    {
        OutBuilder.AppendFormat("%s", AsNameCString());
    }
    else
    {
        // Note this seems wrong since it assumes the separator is '.' and correcting this causes weak_map tests to fail.
        // TODO: consider adjusting persitence code to pass in a EPathMode allowing this code to stop assume `.` for the separator
        OutBuilder.AppendFormat(".%s", AsNameCString());
    }
}

CUTF8String CDataDefinition::GetScopePath(uLang::UTF8Char SeparatorChar, CScope::EPathMode Mode) const
{
    CUTF8StringBuilder Path;
    GetScopePath(Path, SeparatorChar, Mode);
    return Path.MoveToString();
}

bool CDataDefinition::IsVarWritableFrom(const CScope& Scope) const
{
    const CDataDefinition& Definition = GetDefinitionVarAccessibilityRoot();
    return Scope.CanAccess(Definition, Definition.DerivedVarAccessLevel());
}

bool CDataDefinition::IsModuleScopedVar() const
{
    if (!IsVar())
    {
        return false;
    }
    if (_EnclosingScope.GetLogicalScope().GetKind() != CScope::EKind::Module)
    {
        return false;
    }
    return true;
}

EPersistenceExternalAccess CDataDefinition::GetPersistenceExternalAccess() const
{
    return uLang::GetPersistenceExternalAccess(GetPrototypeDefinition()->GetAttributes(), _EnclosingScope.GetProgram());
}

void CDataDefinition::MarkPersistenceCompatConstraint() const
{
    if (IsPersistenceCompatConstraint())
    {
        return;
    }
    _bPersistenceCompatConstraint = true;
    if (const CModule* EnclosingModule = _EnclosingScope.GetModule())
    {
        EnclosingModule->MarkPersistenceCompatConstraint();
    }
}

bool CDataDefinition::IsPersistenceCompatConstraint() const
{
    return _bPersistenceCompatConstraint;
}

bool CDataDefinition::CanHaveCustomAccessors() const
{
    return IsVar()
        && _EnclosingScope.GetLogicalScope().GetKind() == Cases<uLang::CScope::EKind::Class, uLang::CScope::EKind::Interface>
        && GetType()->GetNormalType().AsChecked<uLang::CPointerType>().NegativeValueType()->CanBeCustomAccessorDataType();
}

bool CDataDefinition::HasPredictsAttribute() const
{
    return GetPrototypeDefinition()->HasAttributeClass(_EnclosingScope.GetProgram()._predictsClass, _EnclosingScope.GetProgram());
}

bool CDataDefinition::CanBeAccessedFromPredicts() const
{
    return HasPredictsAttribute();
}

void CDataDefinition::InstantiateDefinition(CScope& InstPositiveScope, CScope& InstNegativeScope, const CNormalType& InstType, const TArray<STypeVariableSubstitution>& Substitutions) const
{
    TSRef<CDataDefinition> InstDataMember = InstPositiveScope.CreateDataDefinition(GetName());
    InstDataMember->SetPrototypeDefinition(*GetPrototypeDefinition());
    InstDataMember->SetInstantiatedOverriddenDefinition(InstType, *this);
    const CTypeBase* NegativeDataMemberType = _NegativeType;
    const CTypeBase* PositiveDataMemberType = GetType();
    NegativeDataMemberType = SemanticTypeUtils::Substitute(*NegativeDataMemberType, ETypePolarity::Negative, Substitutions);
    PositiveDataMemberType = SemanticTypeUtils::Substitute(*PositiveDataMemberType, ETypePolarity::Positive, Substitutions);
    InstDataMember->_NegativeType = NegativeDataMemberType;
    InstDataMember->SetType(PositiveDataMemberType);
    InstDataMember->CreateNegativeDefinition(InstNegativeScope);
}

void CDataDefinition::CreateNegativeDefinition(CScope& NegativeScope) const
{
    TSRef<CDataDefinition> NegativeDataDefinition = NegativeScope.CreateDataDefinition(GetName());
    NegativeDataDefinition->SetPrototypeDefinition(*GetPrototypeDefinition());
}
}    // namespace uLang
