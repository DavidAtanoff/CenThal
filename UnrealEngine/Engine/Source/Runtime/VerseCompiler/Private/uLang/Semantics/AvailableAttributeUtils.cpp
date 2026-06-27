// Copyright Epic Games, Inc. All Rights Reserved.

#include "uLang/Semantics/AvailableAttributeUtils.h"

#include "uLang/Common/Common.h"
#include "uLang/Common/Text/Symbol.h"
#include "uLang/Semantics/Definition.h"
#include "uLang/Semantics/Expression.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticScope.h"
#include "uLang/Semantics/SemanticTypes.h"
#include "uLang/Syntax/VstNode.h"

#include <errno.h>

namespace uLang
{
namespace
{
TOptional<uint64_t> GetIntegerDefinitionValue(const CExprNumber& NumberExpr)
{
    if (NumberExpr.IsFloat())
    {
        return {};
    }
    Integer IntValue = NumberExpr.GetIntValue();
    if (IntValue != 0)
    {
        return IntValue;
    }
    const Verse::Vst::Node* VstNode = NumberExpr.GetMappedVstNode();
    if (!VstNode)
    {
        return {};
    }
    if (!VstNode->IsA<Verse::Vst::IntLiteral>())
    {
        return {};
    }
    const CUTF8String& String = VstNode->As<Verse::Vst::IntLiteral>().GetSourceText();
    char* Last = nullptr;
    errno = 0;
    auto Result = ::strtoull(String.AsCString(), &Last, 0);
    if (Result == ULLONG_MAX && errno == ERANGE)
    {
        return {};
    }
    return static_cast<uint64_t>(Result);
}

TOptional<uint64_t> GetIntegerDefinitionValue(const CExprDefinition& ExprDefinition, const CSemanticProgram& SemanticProgram)
{
    if (TSPtr<CExprInvokeType> ValueInvokeExpr = AsNullable<CExprInvokeType>(ExprDefinition.Value()))
    {
        if (TSPtr<CExprNumber> NumberExpr = AsNullable<CExprNumber>(ValueInvokeExpr->_Argument))
        {
            return GetIntegerDefinitionValue(*NumberExpr);
        }
        return {};
    }
    if (TSPtr<CExprNumber> NumberExpr = AsNullable<CExprNumber>(ExprDefinition.Value()))
    {
        return GetIntegerDefinitionValue(*NumberExpr);
    }
    return {};
}

CSymbol GetArgumentName(const CExprDefinition& ExprDefinition)
{
    TSPtr<CExpressionBase> ElementExpr = ExprDefinition.Element();
    if (TSPtr<CExprIdentifierData> IdentifierData = AsNullable<CExprIdentifierData>(ElementExpr))
    {
        return IdentifierData->GetName();
    }
    if (TSPtr<CExprIdentifierUnresolved> IdentifierUnresolved = AsNullable<CExprIdentifierUnresolved>(ElementExpr))
    {
        return IdentifierUnresolved->_Symbol;
    }
    return {};
}

TOptional<uint64_t> GetAvailableAttributeVersion(const CExpressionBase& Expression, const CSemanticProgram& SemanticProgram) {
    const CExprDefinition* Definition = AsNullable<CExprDefinition>(&Expression);
    if (!Definition)
    {
        return {};
    }
    CSymbol ArgumentName = GetArgumentName(*Definition);
    if (ArgumentName != SemanticProgram._IntrinsicSymbols._MinUploadedAtFNVersion)
    {
        return {};
    }
    return GetIntegerDefinitionValue(*Definition, SemanticProgram);
    };
} // namespace anonymous

TOptional<uint64_t> GetAvailableAttributeVersion(const SAttribute& AvailableAttribute, const CSemanticProgram& SemanticProgram)
{
    if (const CExprArchetypeInstantiation* AvailableArchInst = AsNullable<CExprArchetypeInstantiation>(AvailableAttribute._Expression))
    {
        for (const TSRef<CExpressionBase>& Argument : AvailableArchInst->Arguments())
        {
            if (auto Version = GetAvailableAttributeVersion(*Argument, SemanticProgram))
            {
                return Version;
            }
        }
        for (const TSRef<CExpressionBase>& Expr : AvailableArchInst->_BodyAst.Exprs())
        {
            if (auto Version = GetAvailableAttributeVersion(*Expr, SemanticProgram))
            {
                return Version;
            }
        }
    }
    else if (const CExprMacroCall* AvailableMacroCall = AsNullable<CExprMacroCall>(AvailableAttribute._Expression))
    {
        for (const CExprMacroCall::CClause& Clause : AvailableMacroCall->Clauses())
        {
            for (const TSRef<CExpressionBase>& Expr : Clause.Exprs())
            {
                if (auto Version = GetAvailableAttributeVersion(*Expr, SemanticProgram))
                {
                    return Version;
                }
            }
        }
    }
    // @available attribute is malformed
    return {};
}

TOptional<uint64_t> GetAvailableAttributeVersion(const CDefinition& Definition, const CSemanticProgram& SemanticProgram)
{
    ULANG_ASSERTF(SemanticProgram._availableClass, "Available class definition not found");

    if (const CClass* AvailableClass = SemanticProgram._availableClass)
    {
        if (TOptional<SAttribute> AvailableAttribute = Definition.GetPrototypeDefinition()->GetAttributes().FindAttributeHack(AvailableClass, SemanticProgram))
        {
            return GetAvailableAttributeVersion(*AvailableAttribute, SemanticProgram);
        }
    }

    // No @available attribute
    return TOptional<uint64_t>{};
}

// Combine the available-attribute version with any available-attributes found on the parent scopes.
// A likely case: 
//    @available{MinUploadedAtFNVersion:=3000} 
//    C := class { Value:int=42 }
// The combined available-version for Value is 3000 given it's parent context. This also applies if there
// are multiple versions at different containing scopes - the final applicable version is the most-restrictive one.
TOptional<uint64_t> CalculateCombinedAvailableAttributeVersion(const CDefinition& Definition, const CSemanticProgram& SemanticProgram)
{
    auto CombineResultsHelper = [&SemanticProgram](const CDefinition* Definition, const TOptional<uint64_t>& CurrentResult) -> TOptional<uint64_t> {
        TOptional<uint64_t> Result = CurrentResult;
        if (TOptional<uint64_t> AttributeValue = GetAvailableAttributeVersion(*Definition, SemanticProgram))
        {
            Result = CMath::Max(CurrentResult.Get(0), AttributeValue.GetValue());
        }
        return Result;
    };

    TOptional<uint64_t> CombinedResult = CombineResultsHelper(&Definition, TOptional<uint64_t>());

    // TODO: @available isn't applied to CModulePart correctly - CModuleParts cannot themselves hold attributes, so this snippet becomes a problem:
    // 
    // @available{ MinUploadedAtFNVersion: = 3000 }
    // M<public>: = module {...}
    //
    // @available{ MinUploadedAtFNVersion: = 4000 }
    // M<public>: = module {...}
    //
    // The first module-M gets an available version of 3000. The second @available attribute is processed, but isn't applied to the CModule type. 
    // This kind of attribute should be held on the CModulePart instead.
    const CScope* Scope = &Definition._EnclosingScope;
    while (Scope != nullptr)
    {
        if (const CDefinition* ScopeDefinition = Scope->ScopeAsDefinition())
        {
            CombinedResult = CombineResultsHelper(ScopeDefinition, CombinedResult.Get(0));
        }
        Scope = Scope->GetParentScope();
    }
    return CombinedResult;
}
bool IsDefinitionAvailableAtVersion(const CDefinition& Definition, uint64_t Version, const CSemanticProgram& SemanticProgram)
{
    if (TOptional<uint64_t> AttributeVersion = CalculateCombinedAvailableAttributeVersion(Definition, SemanticProgram))
    {
        return AttributeVersion.GetValue() <= Version;
    }
    // Not version-filtered
    return true;
}

} // namespace uLang
