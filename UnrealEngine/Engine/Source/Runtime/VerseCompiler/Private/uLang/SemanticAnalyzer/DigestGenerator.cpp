// Copyright Epic Games, Inc. All Rights Reserved.

#include "uLang/SemanticAnalyzer/DigestGenerator.h"

#include "uLang/Common/Algo/Cases.h"
#include "uLang/Common/Algo/Sort.h"
#include "uLang/CompilerPasses/CompilerTypes.h"
#include "uLang/Semantics/Attributable.h"
#include "uLang/Semantics/AccessibilityScope.h"
#include "uLang/Semantics/Expression.h"
#include "uLang/Semantics/ModuleAlias.h"
#include "uLang/Semantics/ScopedAccessLevelType.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticEnumeration.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticInterface.h"
#include "uLang/Semantics/SmallDefinitionArray.h"
#include "uLang/Semantics/TypeAlias.h"
#include "uLang/Semantics/TypeVariable.h"
#include "uLang/SourceProject/UploadedAtFNVersion.h"
#include "uLang/Syntax/VstNode.h"
#include "uLang/Syntax/vsyntax_types.h"

#include <inttypes.h>

using Verse::NullWhence;

namespace uLang
{

/// Helper class that does the actual digest generation
class CDigestGeneratorImpl
{
    static constexpr int32_t NumNewLinesForSpacing = 2;

public:

    CDigestGeneratorImpl(
        CSemanticProgram& Program,
        const CAstPackage& Package,
        const TSRef<CDiagnostics>& Diagnostics,
        const CUTF8String* Notes,
        bool bIncludeInternalDefinitions,
        bool bIncludeEpicInternalDefinitions)
        : _Program(Program)
        , _Package(Package)
        , _Diagnostics(Diagnostics)
        , _bIncludeInternalDefinitions(bIncludeInternalDefinitions)
        , _bIncludeEpicInternalDefinitions(bIncludeEpicInternalDefinitions)
        , _CurrentModule(Package._RootModule->GetModule())
        , _CurrentScope(Package._RootModule)
        , _CurrentGlitchAst(nullptr)
        , _Underscore(Program.GetSymbols()->AddChecked("_"))
        , _Notes(Notes)
    {
    }

    bool Generate(CUTF8String& OutDigestCode, TArray<const CAstPackage*>& OutDigestPackageDependencies) const
    {
        using namespace Verse::Vst;
        TNodeRef<Snippet> DigestSnippet = TNodeRef<Snippet>::New(_Package._Name);

        TGuardValue UsingsGuard{_Usings, {}};

        // Do the actual generation work
        GenerateForScope(*_Package._RootModule->GetModule(), DigestSnippet);

        SetLastChildNewLinesAfter(*DigestSnippet);

        // Prepend a list of required using declarations
        PrependUsings(*DigestSnippet);

        // Finally, generate the code
        OutDigestCode = Verse::PrettyPrintVst(DigestSnippet);

        // If digest is empty, make it clear there was no error but there was in fact nothing to export
        if (OutDigestCode.IsEmpty())
        {
            OutDigestCode = "# This digest intentionally left blank.\n";
        }

        if (_Notes && !_Notes->IsEmpty())
        {
            OutDigestCode = *_Notes + "\n" + OutDigestCode;
        }

        for (const CAstPackage* DependencyPackage : _DependencyPackages)
        {
            OutDigestPackageDependencies.Add(DependencyPackage);
        }
        uLang::Algo::Sort(OutDigestPackageDependencies, [](const CAstPackage* A, const CAstPackage* B)
            {
                return A->_Name < B->_Name;
            });

        return !_Diagnostics->HasErrors();
    }

private:
    bool GenerateForScope(const CLogicalScope& Scope, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        TGuardValue<const CScope*> CurrentScopeGuard(_CurrentScope, &Scope);

        bool bGeneratedAnything = false;
        for (const TSRef<CDefinition>& Definition : Scope.GetDefinitions())
        {
            switch (Definition->GetKind())
            {
            case CDefinition::EKind::Class: 
            {
                const CClass* cls = static_cast<const CClass*>(&Definition->AsChecked<CClassDefinition>());
                if (cls->IsSubclassOf(*_Program._scopedClass))
                {
                    bGeneratedAnything |= GenerateForScopedAccessLevel(Definition->AsChecked<CScopedAccessLevelDefinition>(), Parent);
                }
                else
                {
                    bGeneratedAnything |= GenerateDefinitionForClass(Definition->AsChecked<CClassDefinition>(), Parent);
                }
            }
            break;
            case CDefinition::EKind::Data: bGeneratedAnything |= GenerateForDataDefinition(Definition->AsChecked<CDataDefinition>(), Parent); break;
            case CDefinition::EKind::Enumeration: bGeneratedAnything |= GenerateForEnumeration(Definition->AsChecked<CEnumeration>(), Parent); break;
            case CDefinition::EKind::Enumerator: bGeneratedAnything |= GenerateForEnumerator(Definition->AsChecked<CEnumerator>(), Parent); break;
            case CDefinition::EKind::Function: bGeneratedAnything |= GenerateForFunction(Definition->AsChecked<CFunction>(), Parent); break;
            case CDefinition::EKind::Interface: bGeneratedAnything |= GenerateDefinitionForInterface(Definition->AsChecked<CInterface>(), Parent); break;
            case CDefinition::EKind::Module: bGeneratedAnything |= GenerateForModule(Definition->AsChecked<CModule>(), Parent); break;
            case CDefinition::EKind::ModuleAlias: bGeneratedAnything |= GenerateForModuleAlias(Definition->AsChecked<CModuleAlias>(), Parent); break;
            case CDefinition::EKind::TypeAlias: bGeneratedAnything |= GenerateForTypeAlias(Definition->AsChecked<CTypeAlias>(), Parent); break;
            case CDefinition::EKind::TypeVariable: /* TODO */ break;
            default: ULANG_UNREACHABLE();
            }
        }

        return bGeneratedAnything;
    }

    bool GenerateForModule(const CModule& Module, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        if (!ShouldGenerate(Module, false))
        {
            return false;
        }

        TGuardValue<const CModule*> CurrentModuleGuard(_CurrentModule, &Module);

        TNodeRef<Clause> InnerClause = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::HasSemicolonOrNewline);

        {
            TGuardValue UsingsGuard{_Usings, {}};

            if (GenerateForScope(Module, InnerClause))
            {
                SetLastChildNewLinesAfter(*InnerClause);
            }
            else
            {
                // TODO: (yiliang.siew) This hack is so that we don't produce module definitions for modules whose clauses
                // are empty. We should probably just always prune modules that have no significant children (i.e. comments, empty modules, etc.).
                // This flag is always set for the asset manifest.
                if (_Package._bTreatModulesAsImplicit)
                {
                    return false;
                }
                const bool bModuleHasPartInThisPackage = Module.GetParts().ContainsByPredicate([this](const CModulePart* ModulePart){return ModulePart->GetIrPackage() == &_Package;});
                if (!bModuleHasPartInThisPackage)
                {
                    return false;
                }
            }

            // Prepend a list of required using declarations
            PrependUsings(*InnerClause);
        }

        // Generate definition for this module
        InnerClause->SetNewLineAfter(true);
        TNodeRef<Identifier> Name = GenerateDefinitionIdentifier(Module);
        TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(
            NullWhence(),
            Name,
            TNodeRef<Macro>::New(
                NullWhence(),
                GenerateUseOfIntrinsic("module"),
                ClauseArray{ InnerClause }));

        const CUTF8String ModuleImportPath = Module.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator).AsCString();
        const CUTF8StringView View(ModuleImportPath.ToStringView());
        const CUTF8StringView LocalHost = "/localhost";
        if (!View.StartsWith(LocalHost))
        {
            // Create a convenience full path comment for the module
            Verse::Vst::TNodeRef<Verse::Vst::Comment> ImportPathComment = Verse::Vst::TNodeRef<Verse::Vst::Comment>::New(
                Verse::Vst::Comment::EType::line,
                CUTF8String("# Module import path: %s", *ModuleImportPath),
                NullWhence());
            ImportPathComment->SetNewLineAfter(true);
            DefinitionVst->AppendPrefixComment(ImportPathComment);
        }

        DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
        GenerateForAttributes(Module, Name, DefinitionVst);
        Parent->AppendChild(DefinitionVst);

        return true;
    }

    void PrependUsings(Verse::Vst::Node& Node) const
    {
        for (const CUTF8String& UsingPath : _Usings)
        {
            Verse::Vst::TNodeRef<Verse::Vst::Macro> UsingMacro = Verse::Vst::TNodeRef<Verse::Vst::Macro>::New(
                NullWhence(),
                GenerateUseOfIntrinsic("using"),
                Verse::Vst::ClauseArray{Verse::Vst::TNodeRef<Verse::Vst::Clause>::New(
                    Verse::Vst::TNodeRef<Verse::Vst::PathLiteral>::New(UsingPath, NullWhence()).As<Verse::Vst::Node>(),
                    NullWhence(),
                    Verse::Vst::Clause::EForm::NoSemicolonOrNewline)});
            UsingMacro->SetNewLineAfter(true);
            Node.AppendChildAt(UsingMacro, 0);
        }
    }

    bool GenerateForModuleAlias(const CModuleAlias& ModuleAlias, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        if (!ShouldGenerate(ModuleAlias, true))
        {
            return false;
        }

        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, ModuleAlias.GetAstNode());

        TNodeRef<Identifier> Name = GenerateDefinitionIdentifier(ModuleAlias);
        TNodeRef<PrePostCall> Call = TNodeRef<PrePostCall>::New(NullWhence());
        TNodeRef<Identifier> ImportIdentifier = GenerateUseOfIntrinsic("import");
        ImportIdentifier->SetTag(PrePostCall::Op::Expression);
        Call->AppendChild(ImportIdentifier);
        TNodeRef<Clause> Arguments = TNodeRef<Clause>::New((uint8_t)PrePostCall::Op::SureCall, NullWhence(), Clause::EForm::NoSemicolonOrNewline);
        Call->AppendChild(Arguments);
        Arguments->AppendChild(TNodeRef<PathLiteral>::New(ModuleAlias.Module()->GetScopePath('/', CScope::EPathMode::PrefixSeparator), NullWhence()));
        TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(NullWhence(), Name, Call);
        DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
        GenerateForAttributes(ModuleAlias, Name, DefinitionVst);
        Parent->AppendChild(DefinitionVst);

        return true;
    }

    bool GenerateForTypeAlias(const CTypeAlias& TypeAlias, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        if (!ShouldGenerate(TypeAlias, true))
        {
            return false;
        }

        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, TypeAlias.GetAstNode());

        TNodeRef<Identifier> Name = GenerateDefinitionIdentifier(TypeAlias);
        TNodeRef<Node> Type = GenerateForType(TypeAlias.GetPositiveAliasedType());

        TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(NullWhence(), Name, Type);
        DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
        GenerateForAttributes(TypeAlias, Name, DefinitionVst);
        Parent->AppendChild(DefinitionVst);

        return true;
    }

    TArray<Verse::Vst::TNodePtr<Verse::Vst::Node>> GenerateForSuperType(const CExpressionBase& SuperType, TArray<const CInterface*>& VisitedPublicSuperInterfaces) const
    {
        using namespace Verse::Vst;
        // Properly generate qualifiers for a subset of syntax.
        TArray<Verse::Vst::TNodePtr<Verse::Vst::Node>> Ret;

        if (SuperType.GetNodeType() == EAstNodeType::Identifier_Class)
        {
            const CExprIdentifierClass& SuperClassIdentifier = static_cast<const CExprIdentifierClass&>(SuperType);
            const CClass* SuperClass = SuperClassIdentifier.GetClass(_Program);
            // TODO: (yiliang.siew) This has to take `scoped` into account. See: https://jira.it.epicgames.com/browse/FORT-941451
            TArray<const CNominalType*> PublicSuperTypes = PublifyType(SuperClass, VisitedPublicSuperInterfaces);

            if (PublicSuperTypes.Num())
            {
                // If the type we are trying to publify is already public we just take it
                if (SuperClass == PublicSuperTypes[0])
                {
                    TNodeRef<Identifier> SuperClassId = GenerateUseOfDefinition(*SuperClass->Definition());
                    Ret.Add(Move(SuperClassId));
                }
                else
                {
                    for (const CNominalType* PublicType : PublicSuperTypes)
                    {
                        Ret.Add(GenerateForType(PublicType));
                    }
                }
            }
        }
        else if (SuperType.GetNodeType() == EAstNodeType::Identifier_Interface)
        {
            const CExprInterfaceType& SuperInterfaceIdentifier = static_cast<const CExprInterfaceType&>(SuperType);
            const CInterface* SuperInterface = SuperInterfaceIdentifier.GetInterface(_Program);
            // TODO: (yiliang.siew) This has to take `scoped` into account. https://jira.it.epicgames.com/browse/FORT-941451
            TArray<const CNominalType*> PublicSuperTypes = PublifyType(SuperInterface, VisitedPublicSuperInterfaces);

            if (PublicSuperTypes.Num())
            {
                // If the type we are trying to publify is already public we just take it
                if (SuperInterface == PublicSuperTypes[0])
                {
                    TNodeRef<Identifier> SuperInterfaceId = GenerateUseOfDefinition(*SuperInterface);
                    Ret.Add(Move(SuperInterfaceId));
                }
                else
                {
                    for (const CNominalType* PublicType : PublicSuperTypes)
                    {
                        Ret.Add(GenerateForType(PublicType));
                    }
                }
            }
        }
        
        // Fall back to generating unqualified types.
        if (Ret.Num() == 0)
        {
            const CTypeBase* SuperResultType = SuperType.GetResultType(_Program);
            const CTypeType& SuperTypeType = SuperResultType->GetNormalType().AsChecked<CTypeType>();
            const CNormalType& SuperNormalType = SuperTypeType.PositiveType()->GetNormalType();

            if (SuperNormalType.AsNullable<CClass>() || SuperNormalType.AsNullable<CInterface>())
            {
                TArray<const CNominalType*> PublicSuperTypes = PublifyType(&SuperNormalType, VisitedPublicSuperInterfaces);

                for (const CNominalType* PublicType : PublicSuperTypes)
                {
                    Ret.Add(GenerateForType(PublicType));
                }
            }
            else
            {
                return { GenerateForType(SuperTypeType.PositiveType()) };
            }
        }

        return Ret;
    }

    Verse::Vst::TNodeRef<Verse::Vst::Macro> GenerateMacroForClassOrInterface(
        const CFunction* Function,
        const CNominalType& NominalType,
        const CUTF8StringView& MacroName,
        const CLogicalScope& MemberScope,
        const TArray<TSRef<CExpressionBase>>& SuperTypes,
        const CAttributable* EffectsAttributable,
        const TOptional<SAccessLevel>& ConstructorAccessLevel) const
    {
        using namespace Verse::Vst;

        // Create the class/interface macro.
        TNodeRef<Clause> InnerClause = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::HasSemicolonOrNewline);
        InnerClause->SetNewLineAfter(true);

        TNodeRef<Identifier> ClassId = GenerateUseOfIntrinsic(MacroName);
        TNodeRef<Identifier> ClassIdCopy = ClassId;
        TNodeRef<Macro> ClassMacro = TNodeRef<Macro>::New(
                NullWhence(),
                Move(ClassIdCopy),
                ClauseArray{InnerClause});

        if (!SuperTypes.IsEmpty())
        {
            // Create the super clause.
            TNodeRef<Clause> SuperClause = TNodeRef<Clause>::New(vsyntax::res_of, NullWhence(), Clause::EForm::NoSemicolonOrNewline);
            SuperClause->SetNewLineAfter(true);
            bool bEmptySuperClause = true;
            TArray<const CInterface*> VisitedSuperInterface;
            for (const TSRef<CExpressionBase>& SuperType : SuperTypes)
            {
                for (TNodePtr<Node> SuperNode : GenerateForSuperType(*SuperType, VisitedSuperInterface))
                {
                    SuperClause->AppendChild(Move(SuperNode.AsRef()));
                    bEmptySuperClause = false;
                }
            }
            // Append the super clause after the macro name.
            if (!bEmptySuperClause)
            {
                ClassMacro->AppendChildAt(SuperClause, 1);
            }
        }

        if (EffectsAttributable)
        {
            GenerateForAttributes(*EffectsAttributable, ConstructorAccessLevel, ClassId);
        }
        
        // Add `Variance` field as needed
        GenerateVariance(Function, NominalType, MemberScope, *InnerClause);

        // Process its members
        GenerateForScope(MemberScope, InnerClause);

        SetLastChildNewLinesAfter(*InnerClause);

        return ClassMacro;
    }

    void SetLastChildNewLinesAfter(Verse::Vst::Node& Parent) const
    {
        // NOTE: (yiliang.siew) So that things appear nicely in digests, we do not set extra newlines
        // on the last definition so that the parent definition can handle adding the newline instead
        // of "doubling up" on newlines - since we want to have consistent spacing between definitions.
        if (Parent.IsEmpty())
        {
            return;
        }
        const Verse::Vst::TNodeRef<Verse::Vst::Node>& LastChild = Parent.AccessChildren().Last();
        int32_t NumNewLinesAfter = LastChild->NumNewLinesAfter() - NumNewLinesForSpacing;
        LastChild->SetNumNewLinesAfter(NumNewLinesAfter < 0 ? 0 : NumNewLinesAfter);
    }

    bool GenerateVarianceVersion(
        const CFunction* Function,
        const CNominalType& NominalType,
        const CLogicalScope& MemberScope,
        Verse::Vst::Node& Parent,
        EMemberOrigin MemberOrigin) const
    {
        using namespace Verse;

        if (!Function)
        {
            return false;
        }

        CTupleType::ElementArray VarTypes;
        CTupleType::ElementArray NegativeTypes;
        CTupleType::ElementArray PositiveTypes;
        MemberScope.ForAllDefinitions(MemberOrigin, [&](const CDefinition& MemberDefinition)
        {
            const CTypeBase* MemberDefinitionType;
            bool bIsVar;
            if (MemberDefinition.GetKind() == CDefinition::EKind::Data)
            {
                const CDataDefinition& MemberDataDefinition = MemberDefinition.AsChecked<CDataDefinition>();
                MemberDefinitionType = MemberDataDefinition.GetType();
                bIsVar = MemberDataDefinition.IsVar();
            }
            else if (MemberDefinition.GetKind() == CDefinition::EKind::Function)
            {
                MemberDefinitionType = MemberDefinition.AsChecked<CFunction>()._Signature.GetFunctionType();
                bIsVar = false;
            }
            else
            {
                ULANG_UNREACHABLE();
            }
            SemanticTypeUtils::ForEachTypeRecursive(
                MemberDefinitionType,
                ETypePolarity::Positive,
                [&](const CTypeBase* Type, ETypePolarity TypePolarity) {
                    if (const CTypeVariable* TypeVariable = Type->GetNormalType().AsNullable<CTypeVariable>())
                    {
                        if (&TypeVariable->_EnclosingScope != Function)
                        {
                            return;
                        }
                        if (bIsVar)
                        {
                            if (TypePolarity == ETypePolarity::Positive)
                            {
                                // A var has both negative and positive uses.
                                // Record only the positive use to avoid
                                // duplicate uses.
                                VarTypes.Add(TypeVariable);
                            }
                        }
                        else if (TypePolarity == ETypePolarity::Negative)
                        {
                            NegativeTypes.Add(TypeVariable);
                        }
                        else
                        {
                            PositiveTypes.Add(TypeVariable);
                        }
                    }
                });
        });

        // Don't generate the variance proxy if it would add no constraints.
        if (!NegativeTypes.IsEmpty() || !PositiveTypes.IsEmpty())
        {
            CTupleType& PositiveTupleType = _Program.GetOrCreateTupleType(Move(PositiveTypes));
            Vst::TNodeRef<Vst::Identifier> Identifier = Vst::TNodeRef<Vst::Identifier>::New(GetVarianceName(NominalType, MemberScope, MemberOrigin, "_Variance"), NullWhence());
            CUTF8String MemberScopePath = MemberScope.GetScopePath('/', CScope::EPathMode::PrefixSeparator);
            Identifier->AppendChild(Vst::TNodeRef<Vst::PathLiteral>::New(MemberScopePath, NullWhence()));
            GenerateForAttributeGeneric(_Program._privateClass, {}, [&](const CClass*) { return Identifier; });
            Vst::TNodeRef<Vst::PrePostCall> Call = Vst::TNodeRef<Vst::PrePostCall>::New(NullWhence());
            Identifier->SetTag(Vst::PrePostCall::Expression);
            Call->AppendChild(Move(Identifier));
            Call->AppendChild(GenerateForParamTypes(CFunctionType::ParamTypes(NegativeTypes)));
            Vst::TNodeRef<Vst::TypeSpec> TypeSpec = Vst::TNodeRef<Vst::TypeSpec>::New(NullWhence(), Move(Call), GenerateForType(PositiveTupleType));
            Vst::TNodeRef<Vst::Definition> Definition = Vst::TNodeRef<Vst::Definition>::New(
                NullWhence(),
                Move(TypeSpec),
                GenerateExternalMacro());
            Definition->SetNumNewLinesAfter(NumNewLinesForSpacing);

            // Add an @available{UploadedAtFNVersion:=4000} attribute if the variance proxy includes inherited fields for backward compatibility.
            if (MemberOrigin != EMemberOrigin::Original)
            {
                Vst::TNodeRef<Vst::Identifier> AvailableIdentifier = GenerateUseOfDefinition(*_Program._availableClass->Definition());
                Vst::TNodeRef<Vst::Definition> MinUploadedAtFNVersionDefinition = Vst::TNodeRef<Vst::Definition>::New(
                    NullWhence(),
                    Vst::TNodeRef<Vst::Identifier>::New(_Program._IntrinsicSymbols._MinUploadedAtFNVersion.AsStringView(), NullWhence()),
                    Vst::TNodeRef<Vst::IntLiteral>::New(uLang::CUTF8String("%" PRIu32, VerseFN::UploadedAtFNVersion::FixSubtypingIgnoringInheritedFieldsVersion), NullWhence()));
                Vst::TNodeRef<Vst::Clause> AvailableMacroClause = Vst::TNodeRef<Vst::Clause>::New(MinUploadedAtFNVersionDefinition.As<Vst::Node>(), NullWhence(), Vst::Clause::EForm::NoSemicolonOrNewline);
                Vst::TNodeRef<Vst::Macro> AvailableMacro = Vst::TNodeRef<Vst::Macro>::New(NullWhence(), AvailableIdentifier, Vst::ClauseArray{ AvailableMacroClause });
                AvailableMacro->SetNewLineAfter(true);
                // attribute nodes must be wrapped in a Clause node (elsewhere used to preserve source comments)
                Vst::TNodeRef<Vst::Clause> WrapperClause = Vst::TNodeRef<Vst::Clause>::New(AvailableMacro.As<Vst::Node>(), NullWhence(), Vst::Clause::EForm::IsAppendAttributeHolder);
                Definition->AppendAux(WrapperClause);
            }

            Parent.AppendChild(Move(Definition));
        }
        if (!VarTypes.IsEmpty())
        {
            COptionType& VarType = _Program.GetOrCreateOptionType(&_Program.GetOrCreateTupleType(Move(VarTypes)));
            Vst::TNodeRef<Vst::Identifier> Identifier = Vst::TNodeRef<Vst::Identifier>::New(GetVarianceName(NominalType, MemberScope, MemberOrigin, "_VarVariance"), NullWhence());
            CUTF8String MemberScopePath = MemberScope.GetScopePath('/', CScope::EPathMode::PrefixSeparator);
            Identifier->AppendChild(Vst::TNodeRef<Vst::PathLiteral>::New(MemberScopePath, NullWhence()));
            GenerateForAttributeGeneric(_Program._privateClass, {}, [&](const CClass*) { return Identifier; });
            Vst::TNodeRef<Vst::Mutation> Mutation = Vst::TNodeRef<Vst::Mutation>::New(NullWhence(), Move(Identifier), Vst::Mutation::EKeyword::Var, false);
            Vst::TNodeRef<Vst::TypeSpec> TypeSpec = Vst::TNodeRef<Vst::TypeSpec>::New(NullWhence(), Move(Mutation), GenerateForType(VarType));
            Vst::TNodeRef<Vst::Definition> Definition = Vst::TNodeRef<Vst::Definition>::New(
                NullWhence(),
                Move(TypeSpec),
                GenerateExternalMacro());
            Definition->SetNumNewLinesAfter(NumNewLinesForSpacing);
            Parent.AppendChild(Move(Definition));
        }
        return true;
    }

    bool GenerateVariance(
        const CFunction* Function,
        const CNominalType& NominalType,
        const CLogicalScope& MemberScope,
        Verse::Vst::Node& Parent) const
    {
        return GenerateVarianceVersion(Function, NominalType, MemberScope, Parent, EMemberOrigin::Inherited)
            && GenerateVarianceVersion(Function, NominalType, MemberScope, Parent, EMemberOrigin::Original);
    }

    CUTF8String GetVarianceName(const CNominalType& NominalType, const CLogicalScope& LogicalScope, EMemberOrigin MemberOrigin, CUTF8StringView Suffix) const
    {
        const TSPtr<CSymbolTable>& Symbols = _Program.GetSymbols();
        // Data qualification is currently not accounted for, so instead add the name of the enclosing scope as a prefix.
        CUTF8String VarianceBaseName = LogicalScope.GetScopePath('_', EPathMode::PackageRelativeWithRoot) + Suffix;
        CUTF8String VarianceName = VarianceBaseName;
        switch (MemberOrigin)
        {
        case EMemberOrigin::Inherited: VarianceName += "_Inherited"; break;
        case EMemberOrigin::Original: break;
        case EMemberOrigin::InheritedOrOriginal:
        default: ULANG_UNREACHABLE();
        };
        for (uint32_t I = 0;; ++I)
        {
            CSymbol VarianceSymbol = Symbols->AddChecked(VarianceName);
            if (NominalType.FindInstanceMember(VarianceSymbol, EMemberOrigin::Inherited, SQualifier::Unknown()).IsEmpty() &&
                LogicalScope.ResolveDefinition(VarianceSymbol).IsEmpty())
            {
                return VarianceName;
            }
            VarianceName = CUTF8String("%s%u", *VarianceBaseName, I);
        }
    }

    bool GenerateDefinitionForClassOrInterface(
        const CNominalType& NominalType,
        const CUTF8StringView& MacroName,
        const CLogicalScope& MemberScope,
        const CDefinition& DefinitionAst,
        const TArray<TSRef<CExpressionBase>>& SuperTypes,
        const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent,
        const CAttributable* EffectsAttributable,
        const TOptional<SAccessLevel>& ConstructorAccessLevel) const
    {
        using namespace Verse::Vst;

        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, DefinitionAst.GetAstNode());

        // Create the class/interface definition.
        TNodeRef<Identifier> Name = GenerateDefinitionIdentifier(DefinitionAst);
        TNodeRef<Macro> ClassMacro = GenerateMacroForClassOrInterface(
            nullptr,
            NominalType,
            MacroName,
            MemberScope,
            SuperTypes,
            EffectsAttributable,
            ConstructorAccessLevel);
        TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(NullWhence(), Name, ClassMacro);
        DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
        GenerateForAttributes(DefinitionAst, Name, DefinitionVst);
        Parent->AppendChild(DefinitionVst);

        return true;
    }

    bool GenerateDefinitionForClass(const CClassDefinition& Class, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        // Cull inaccessible classes
        if (!ShouldGenerate(Class))
        {
            return false;
        }
        return GenerateDefinitionForClassOrInterface(
            Class,
            Class.IsStruct()? "struct" : "class",
            Class,
            Class,
            Class.GetIrNode()->SuperTypes(),
            Parent,
            &Class._EffectAttributable,
            Class._ConstructorAccessLevel);
    }

    Verse::Vst::TNodeRef<Verse::Vst::Macro> GenerateMacroForClass(const CFunction* Function, const CClassDefinition& Class) const
    {
        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, Class.GetAstNode());
        return GenerateMacroForClassOrInterface(
            // Structs have not need for, nor the ability to hide, a `Variance` field.
            Class.IsStruct() ? nullptr : Function,
            Class,
            Class.IsStruct()? "struct" : "class",
            Class,
            Class.GetIrNode()->SuperTypes(),
            &Class._EffectAttributable,
            Class._ConstructorAccessLevel);
    }

    bool GenerateDefinitionForInterface(const CInterface& Interface, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        // Cull inaccessible classes
        if (!ShouldGenerate(Interface))
        {
            return false;
        }
        return GenerateDefinitionForClassOrInterface(
            Interface,
            "interface",
            Interface,
            Interface,
            Interface.GetIrNode()->SuperInterfaces(),
            Parent,
            &Interface._EffectAttributable,
            Interface._ConstructorAccessLevel);
    }

    Verse::Vst::TNodeRef<Verse::Vst::Macro> GenerateMacroForInterface(const CFunction* Function, const CInterface& Interface) const
    {
        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, Interface.GetAstNode());
        return GenerateMacroForClassOrInterface(
            Function,
            Interface,
            "interface",
            Interface,
            Interface.GetIrNode()->SuperInterfaces(),
            &Interface._EffectAttributable,
            Interface._ConstructorAccessLevel);
    }

    bool GenerateForEnumerator(const CEnumerator& Enumerator, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        // Cull inaccessible enumerations
        if (!ShouldGenerate(Enumerator))
        {
            return false;
        }
        // We check here if there are any `@doc` attributes and convert them to comments as well.
        const TNodeRef<Identifier> EnumIdentifier = GenerateDefinitionIdentifier(Enumerator);
        GenerateForAttributes(Enumerator, EnumIdentifier, EnumIdentifier);
        EnumIdentifier->SetNewLineAfter(true);
        Parent->AppendChild(EnumIdentifier);
        return true;
    }

    bool GenerateForEnumeration(const CEnumeration& Enumeration, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        // Cull inaccessible enumerations
        if (!ShouldGenerate(Enumeration))
        {
            return false;
        }

        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, Enumeration.GetAstNode());

        // Create enum definition
        TNodeRef<Clause> InnerClause = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::HasSemicolonOrNewline);
        InnerClause->SetNewLineAfter(true); // If to use vertical format
        GenerateForScope(Enumeration, InnerClause);
        SetLastChildNewLinesAfter(*InnerClause);
        TNodeRef<Identifier> Name = GenerateDefinitionIdentifier(Enumeration);
        TNodeRef<Identifier> EnumIdentifierVst = GenerateUseOfIntrinsic("enum");
        GenerateForAttributes(
            Enumeration._EffectAttributable,
            TOptional<SAccessLevel>{},
            EnumIdentifierVst);
        TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(
            NullWhence(),
            Name,
            TNodeRef<Macro>::New(
                NullWhence(),
                EnumIdentifierVst,
                ClauseArray{ InnerClause }));
        DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
        GenerateForAttributes(Enumeration, Name, DefinitionVst);
        Parent->AppendChild(DefinitionVst);

        return true;
    }

    bool GenerateForScopedPaths(const CScopedAccessLevelDefinition& ScopedAccessLevel, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        Verse::Vst::ClauseArray NewClauses;
        for (const CScope* Scope : ScopedAccessLevel._Scopes)
        {
            CUTF8String PathString = Scope->GetScopePath('/', CScope::EPathMode::PrefixSeparator);

            TNodeRef<PathLiteral> NewPathLiteral = TNodeRef<PathLiteral>::New(Move(PathString), NullWhence());
            // The syntax should be something like `scoped {/Verse.org`}, we don't want any newlines after the
            // path literal.
            NewPathLiteral->SetNumNewLinesAfter(0);
            Parent->AppendChild(NewPathLiteral);
        }

        return true;
    }

    const Verse::Vst::TNodeRef<Verse::Vst::Macro> GenerateForScopedMacro(const CScopedAccessLevelDefinition& ScopedAccessLevel) const
    {
        using namespace Verse::Vst;

        // Create access level definition
        TNodeRef<Clause> InnerClause = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::HasSemicolonOrNewline);
        InnerClause->SetNewLineAfter(false);

        GenerateForScopedPaths(ScopedAccessLevel, InnerClause);

        return TNodeRef<Macro>::New(
                NullWhence(),
                GenerateUseOfIntrinsic("scoped"),
                ClauseArray{ InnerClause });
    }

    bool GenerateForScopedAccessLevel(const CScopedAccessLevelDefinition& ScopedAccessLevel, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        // Cull inaccessible access levels
        if (!ShouldGenerate(ScopedAccessLevel))
        {
            return false;
        }

        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, ScopedAccessLevel.GetAstNode());

        TNodeRef<Identifier> ScopedDefinitionName = GenerateDefinitionIdentifier(ScopedAccessLevel);
        TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(
            NullWhence(),
            ScopedDefinitionName,
            GenerateForScopedMacro(ScopedAccessLevel));

        DefinitionVst->SetNewLineAfter(true);
        GenerateForAttributes(ScopedAccessLevel, ScopedDefinitionName, DefinitionVst);
        Parent->AppendChild(DefinitionVst);

        return true;
    }

    bool GenerateForFunction(const CFunction& Function, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        // Cull inaccessible functions
        if (!ShouldGenerate(Function) || Function.IsCoercion())
        {
            return false;
        }

        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, Function.GetAstNode());

        // Create function definition
        TNodeRef<PrePostCall> Call = TNodeRef<PrePostCall>::New(NullWhence());
        const CUTF8StringView FunctionNameStringView = Function._ExtensionFieldAccessorKind == EExtensionFieldAccessorKind::ExtensionMethod
            ? _Program._IntrinsicSymbols.StripExtensionFieldOpName(Function.GetName())
            : Function.AsNameStringView();
        TNodeRef<Identifier> FunctionName = GenerateDefinitionIdentifier(FunctionNameStringView, Function);
        TNodeRef<Clause> ParameterList = GenerateForParameters(Function);

        switch (Function._ExtensionFieldAccessorKind)
        {
        case EExtensionFieldAccessorKind::Function:
        {
            FunctionName->SetTag(PrePostCall::Op::Expression);
            Call->AppendChild(FunctionName);
            Call->AppendChild(ParameterList);
            break;
        }
        case EExtensionFieldAccessorKind::ExtensionDataMember:
        {
            _Diagnostics->AppendGlitch(
                SGlitchResult(EDiagnostic::ErrDigest_Unimplemented, uLang::CUTF8String("Extension data members are not implemented yet.")),
                _CurrentGlitchAst);
            break;
        }
        case EExtensionFieldAccessorKind::ExtensionMethod:
        {
            FunctionName->SetTag(PrePostCall::Op::DotIdentifier);
            TNodeRef<Clause> LhsParameter = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline);
            LhsParameter->SetTag(PrePostCall::Op::SureCall);

            TNodeRef<PrePostCall> DotCall = TNodeRef<PrePostCall>::New(NullWhence());
            DotCall->AppendChild(LhsParameter);
            DotCall->AppendChild(FunctionName);
            Call->AppendChild(DotCall);

            if (ParameterList->GetChildCount() == 1)
            {
                TNodeRef<Node> Child = ParameterList->TakeChildAt(0);
                ULANG_ASSERT(Child->IsA<Where>());
                TNodeRef<Where> WhereNode = Child.As<Where>();
                LhsParameter->AppendChild(WhereNode);
                TNodeRef<Node> Lhs = WhereNode->GetLhs();
                ULANG_ASSERT(Lhs->IsA<Parens>());
                ULANG_ASSERT(Lhs->GetChildCount() == 2);
                WhereNode->SetLhs(Lhs->TakeChildAt(0));
                Call->AppendChild(AsClause(Lhs->TakeChildAt(0)));
            }
            else
            {
                ULANG_ASSERT(ParameterList->GetChildCount() == 2);
                LhsParameter->AppendChild(ParameterList->TakeChildAt(0));
                Call->AppendChild(AsClause(ParameterList->TakeChildAt(0)));
            }
            break;
        }
        default:
            ULANG_ERRORF("Missing an alternative in switch.");
        }

        // Is there an implementation?
        if (const CExpressionBase* Body = Function.GetBodyIr())
        {
            if (Body->GetNodeType() == EAstNodeType::Definition_Class)
            {
                const CClass& Class = static_cast<const CExprClassDefinition&>(*Body)._Class;
                TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(
                    NullWhence(),
                    Call,
                    GenerateMacroForClass(
                        &Function,
                        *Class._Definition));
                DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);

                TArray<SAttribute> Attributes = Function.GetAttributes()._Attributes;
                if (TOptional<SAttribute> NativeAttribute = Class.GetAttributes().FindAttribute(_Program._nativeClass, _Program))
                {
                    Attributes.Push(Move(*NativeAttribute));
                }
                GenerateForAttributes(Attributes, Function, FunctionName, DefinitionVst);
                Parent->AppendChild(DefinitionVst);
            }
            else if (Body->GetNodeType() == EAstNodeType::Definition_Interface)
            {
                TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(
                    NullWhence(),
                    Call,
                    GenerateMacroForInterface(&Function, static_cast<const CExprInterfaceDefinition&>(*Body)._Interface));
                DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
                GenerateForAttributes(Function, FunctionName, DefinitionVst);
                Parent->AppendChild(DefinitionVst);
            }
            else
            {
                const CTypeBase* ReturnType = Function._Signature.GetReturnType();
                if (const CTypeType* ReturnTypeType = ReturnType->GetNormalType().AsNullable<CTypeType>();
                    ReturnTypeType && !Function.GetReturnTypeIr() && Function._Signature.GetEffects() == EffectSets::ParametricTypeDefault)
                {
                    TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(
                        NullWhence(),
                        Call,
                        GenerateForType(ReturnTypeType->PositiveType()));
                    DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
                    GenerateForAttributes(Function, FunctionName, DefinitionVst);
                    Parent->AppendChild(DefinitionVst);
                }
                else
                {
                    TNodeRef<TypeSpec> TypedCall = TNodeRef<TypeSpec>::New(
                        NullWhence(),
                        Call,
                        GenerateForType(Function._Signature.GetReturnType()));
                    // Generate an assignment to the external{} macro
                    TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(
                        NullWhence(),
                        TypedCall,
                        GenerateExternalMacro());
                    DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
                    GenerateForAttributes(Function, FunctionName, DefinitionVst);
                    Parent->AppendChild(DefinitionVst);
                    GenerateForEffectAttributes(
                        Function._Signature.GetFunctionType()->GetEffects(),
                        EffectSets::FunctionDefault,
                        Function._Signature.GetFunctionType()->IsClosed(),
                        *Call);
                }
            }
        }
        else
        {
            TNodeRef<TypeSpec> TypedCall = TNodeRef<TypeSpec>::New(
                NullWhence(),
                Call,
                GenerateForType(Function._Signature.GetReturnType()));
            // No, just generate the function declaration by itself
            TypedCall->SetNumNewLinesAfter(NumNewLinesForSpacing);
            GenerateForAttributes(Function, FunctionName, TypedCall);
            Parent->AppendChild(TypedCall);
            GenerateForEffectAttributes(
                Function._Signature.GetFunctionType()->GetEffects(),
                EffectSets::FunctionDefault,
                Function._Signature.GetFunctionType()->IsClosed(),
                *Call);
        }

        return true;
    }

    bool GenerateForDataDefinition(const CDataDefinition& DataDefinition, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Parent) const
    {
        using namespace Verse::Vst;

        // Cull inaccessible data definitions
        if (!ShouldGenerate(DataDefinition))
        {
            return false;
        }

        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, DataDefinition.GetAstNode());

        // Create data definition
        CSymbol Name = DataDefinition.GetName();
        TNodeRef<Identifier> NameNode = GenerateDefinitionIdentifier(DataDefinition);
        TNodePtr<Node> DecoratedNode;
        const CTypeBase* Type = DataDefinition.GetType();
        if (DataDefinition.IsVar())
        {
            DecoratedNode = TNodeRef<Mutation>::New(NullWhence(), NameNode, Mutation::EKeyword::Var, DataDefinition.IsLive());
            GenerateForAttributesGeneric({}, DataDefinition.SelfVarAccessLevel(), [&DecoratedNode] (const CClass*) { return DecoratedNode.AsRef(); });
            Type = Type->GetNormalType().AsChecked<CPointerType>().PositiveValueType();
        }
        else if (DataDefinition.IsLive())
        {
            DecoratedNode = TNodeRef<Mutation>::New(NullWhence(), NameNode, Mutation::EKeyword::Live, DataDefinition.IsLive());
            GenerateForAttributesGeneric({}, DataDefinition.SelfVarAccessLevel(), [&DecoratedNode](const CClass*) { return DecoratedNode.AsRef(); });
            Type = Type->GetNormalType().AsChecked<CPointerType>().PositiveValueType();
        }
        else
        {
            DecoratedNode = NameNode;
        }

        const CExpressionBase* ValueDomain = DataDefinition.GetIrNode()->ValueDomain();
        ULANG_ASSERT(ValueDomain);
        TNodePtr<Node> ValueDomainNode;
        if (ValueDomain->GetResultType(_Program)->GetNormalType().IsA<CFunctionType>())
        {
            ValueDomainNode = GenerateForExpression(DataDefinition.GetIrNode()->ValueDomain());
        }
        else
        {
            ValueDomainNode = GenerateForType(Type);
        }
        TNodeRef<TypeSpec> TypeSpecNode = TNodeRef<TypeSpec>::New(
            NullWhence(),
            Move(DecoratedNode.AsRef()),
            Move(ValueDomainNode.AsRef()));

        // Is there a default value?
        if (DataDefinition.GetIrNode()->Value().IsValid())
        {
            // Yes, generate an assignment to the external{} macro
            TNodeRef<Definition> DefinitionVst = TNodeRef<Definition>::New(
                NullWhence(),
                TypeSpecNode,
                GenerateExternalMacro());
            DefinitionVst->SetNumNewLinesAfter(NumNewLinesForSpacing);
            GenerateForAttributes(DataDefinition, NameNode, DefinitionVst);
            Parent->AppendChild(DefinitionVst);
        }
        else
        {
            // No, just generate the type spec by itself
            TypeSpecNode->SetNumNewLinesAfter(NumNewLinesForSpacing);
            GenerateForAttributes(DataDefinition, NameNode, TypeSpecNode);
            Parent->AppendChild(TypeSpecNode);
        }

        return true;
    }

    Verse::Vst::TNodeRef<Verse::Vst::Clause> GenerateForParameters(const CFunction& Function) const
    {
        using namespace Verse::Vst;
        const CExprFunctionDefinition* FunctionDefinition = Function.GetIrNode();
        const TSPtr<CExpressionBase>& Element = FunctionDefinition->Element();
        ULANG_ASSERTF(Element, "Function definition IR node must have an element.");
        ULANG_ASSERTF(Element->GetNodeType() == EAstNodeType::Invoke_Invocation, "Function definition element IR node must be an invocation.");
        const CExprInvocation& Invocation = static_cast<const CExprInvocation&>(*Element);
        auto ParamDefinitionIterator = Function._Signature.GetParams().begin();
        TNodeRef<Node> Parameter = GenerateForParameter(*Invocation.GetArgument(), ParamDefinitionIterator);
        TArray<TNodeRef<Node>> TypeVariableDefinition;
        for (const TSRef<CTypeVariable>& TypeVariable : Function.GetDefinitionsOfKind<CTypeVariable>())
        {
            if (TypeVariable->_ExplicitParam)
            {
                continue;
            }
            TypeVariableDefinition.Add(TNodeRef<TypeSpec>::New(
                NullWhence(),
                GenerateDefinitionIdentifier(*TypeVariable->Definition()),
                GenerateForType(TypeVariable->GetPositiveType())));
        }
        return AsClause(TypeVariableDefinition.IsEmpty()?
            Move(Parameter) :
            TNodeRef<Where>::New(NullWhence(), Move(Parameter), Move(TypeVariableDefinition)));
    }

    template <typename TParamDefinitionIterator>
    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForParameter(
        const CExpressionBase& Expression,
        TParamDefinitionIterator& ParamDefinitionIterator) const
    {
        using namespace Verse::Vst;
        if (Expression.GetNodeType() == EAstNodeType::Invoke_MakeTuple)
        {
            TNodeRef<Parens> Result = TNodeRef<Parens>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline);
            const CExprMakeTuple& ExprMakeTuple = static_cast<const CExprMakeTuple&>(Expression);
            for (const TSRef<CExpressionBase>& SubExpr : ExprMakeTuple.GetSubExprs())
            {
                Result->AppendChild(GenerateForParameter(*SubExpr, ParamDefinitionIterator));
            }
            return Result;
        }
        else if (Expression.GetNodeType() == EAstNodeType::Definition_Where)
        {
            const CExprWhere& ExprWhere = static_cast<const CExprWhere&>(Expression);
            // The right-hand side of `where` is handled separately.  All
            // `where` clauses are collapsed into a single `where`.  Given
            // bindings introduced by `where` are scoped to the entire domain,
            // this shouldn't introduce any ambiguity.
            return GenerateForParameter(*ExprWhere.Lhs(), ParamDefinitionIterator);
        }
        else
        {
            ULANG_ASSERTF(
                Expression.GetNodeType() == EAstNodeType::Definition,
                "Digest generation for '%s' is unimplemented.", Expression.GetErrorDesc().AsCString());
            const CExprDefinition& ExprDefinition = static_cast<const CExprDefinition&>(Expression);
            const CDataDefinition* ParamDefinition = *ParamDefinitionIterator;
            ++ParamDefinitionIterator;
            TNodePtr<Identifier> IdentifierNode;
            const CTypeBase* Type;
            const bool bNeverQualify = ParamDefinition->_bNamed; // TODO: qualified named parameters aren't handled by the desugarer yet.
            if (const CTypeVariable* ImplicitParam = ParamDefinition->_ImplicitParam)
            {
                IdentifierNode = GenerateDefinitionIdentifier(*ImplicitParam, bNeverQualify);
                Type = ImplicitParam->GetPositiveType();
            }
            else
            {
                IdentifierNode = GenerateDefinitionIdentifier(*ParamDefinition, bNeverQualify);
                Type = ParamDefinition->GetType();
            }
            TNodePtr<Node> ElementNode;
            if (ParamDefinition->_bNamed)
            {
                TNodeRef<PrePostCall> QMarkNode = TNodeRef<PrePostCall>::New(IdentifierNode.AsRef(), NullWhence());
                QMarkNode->PrependQMark(NullWhence());
                ElementNode = Move(QMarkNode);
            }
            else
            {
                ElementNode = IdentifierNode;
            }
            TNodeRef<Node> Result = TNodeRef<TypeSpec>::New(
                NullWhence(),
                Move(ElementNode.AsRef()),
                GenerateForType(Type));
            if (ExprDefinition.Value())
            {
                Result = TNodeRef<Definition>::New(
                    NullWhence(),
                    Move(Result),
                    GenerateExternalMacro());
            }
            return Result;
        }
    }

    static Verse::Vst::TNodeRef<Verse::Vst::Clause> AsClause(Verse::Vst::TNodeRef<Verse::Vst::Node> Node)
    {
        namespace Vst = Verse::Vst;
        if (Node->IsA<Vst::Clause>())
        {
            return Node.As<Vst::Clause>();
        }
        if (const Vst::Parens* Parens = Node->AsNullable<Vst::Parens>())
        {
            Vst::TNodeRef<Vst::Clause> Clause = Vst::TNodeRef<Vst::Clause>::New(NullWhence(), Parens->GetForm());
            Clause->SetTag(Vst::PrePostCall::Op::SureCall);
            Vst::Node::TransferChildren(Node, Clause);
            return Clause;
        }
        Vst::TNodeRef<Vst::Clause> Clause = Vst::TNodeRef<Vst::Clause>::New(NullWhence(), Vst::Clause::EForm::NoSemicolonOrNewline);
        Clause->SetTag(Vst::PrePostCall::Op::SureCall);
        Clause->AppendChild(Node);
        return Clause;
    }

    Verse::Vst::TNodeRef<Verse::Vst::Clause> GenerateForParamTypes(CFunctionType::ParamTypes ParamTypes) const
    {
        using namespace Verse::Vst;

        TNodeRef<Clause> ParamList = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline);
        for (const CTypeBase* ParamType : ParamTypes)
        {
            ParamList->AppendChild(TNodeRef<TypeSpec>::New(NullWhence(), GenerateForType(&SemanticTypeUtils::AsPositive(*ParamType, {}))));
        }

        // We assume this node will be used in a PrePostCall so set it up properly for that
        ParamList->SetTag(PrePostCall::Op::SureCall);

        return ParamList;
    }

    // Create VST node representing supplied tuple type
    Verse::Vst::TNodeRef<Verse::Vst::PrePostCall> GenerateForTupleType(const CTupleType& Tuple) const
    {
        using namespace Verse::Vst;

        TNodeRef<PrePostCall> Call = TNodeRef<PrePostCall>::New(NullWhence());

        // Add `tuple` identifier
        TNodeRef<Identifier> TupleIdent = GenerateUseOfIntrinsic("tuple");
        TupleIdent->SetTag(PrePostCall::Op::Expression);
        Call->AppendChild(TupleIdent);


        // Add element types
        TNodeRef<Clause> Elements = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline);
        for (const CTypeBase* ElementType : Tuple.GetElements())
        {
            Elements->AppendChild(GenerateForType(ElementType));
        }

        Elements->SetTag(PrePostCall::Op::SureCall);
        Call->AppendChild(Elements);

        return Call;
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForIntrinsicInvocation(
        const CUTF8StringView& CalleeName,
        const Verse::Vst::TNodeRef<Verse::Vst::Node>& Argument) const
    {
        using namespace Verse::Vst;
        TNodeRef<Identifier> CalleeIdentifier = GenerateUseOfIntrinsic(CalleeName);
        CalleeIdentifier->SetTag(PrePostCall::Op::Expression);
        Argument->SetTag(PrePostCall::Op::SureCall);
        TNodeRef<PrePostCall> Call = TNodeRef<PrePostCall>::New(NullWhence());
        Call->AppendChild(CalleeIdentifier);
        Call->AppendChild(Argument);
        return Call;
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForIntrinsicTypeInvocation(
        const CUTF8StringView& CalleeName,
        const CTypeBase* TypeArgment) const
    {
        using namespace Verse::Vst;
        return GenerateForIntrinsicInvocation(
            CalleeName,
            TNodeRef<Clause>::New(
                GenerateForType(TypeArgment),
                NullWhence(),
                Clause::EForm::NoSemicolonOrNewline));
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForSubtypeType(const CTypeBase* NegativeType, ETypeConstraintFlags SubtypeConstraints) const
    {
        const char* const DescriptionText = GetConstraintTypeAsCString(SubtypeConstraints, true);

        return GenerateForIntrinsicTypeInvocation(DescriptionText, NegativeType);
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForSupertypeType(const CTypeBase* PositiveType) const
    {
        return GenerateForIntrinsicTypeInvocation("supertype", PositiveType);
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForWeakMapType(const CTypeBase* KeyType, const CTypeBase* ValueType) const
    {
        using namespace Verse::Vst;
        return GenerateForIntrinsicInvocation(
            "weak_map",
            TNodeRef<Clause>::New(
                Node::NodeArray{GenerateForType(KeyType), GenerateForType(ValueType)},
                NullWhence(),
                Clause::EForm::NoSemicolonOrNewline));
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForGeneratorType(const CTypeBase* ElementType) const
    {
        using namespace Verse::Vst;
        return GenerateForIntrinsicInvocation(
            "generator",
            TNodeRef<Clause>::New(
                Node::NodeArray{GenerateForType(ElementType)},
                NullWhence(),
                Clause::EForm::NoSemicolonOrNewline));
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForType(const CNormalType& Type) const
    {
        using namespace Verse::Vst;
        switch (Type.GetKind())
        {
        case ETypeKind::False:
        case ETypeKind::True:
        case ETypeKind::Void:
        case ETypeKind::Any:
        case ETypeKind::Comparable:
        case ETypeKind::Logic:
        case ETypeKind::Int:
        case ETypeKind::Rational:
        case ETypeKind::Float:
        case ETypeKind::Char8:
        case ETypeKind::Char32:
        case ETypeKind::Path:
        case ETypeKind::Range:
            return GenerateUseOfIntrinsic(Type.AsCode());

        case ETypeKind::Class:
        {
            const CClass& Class = Type.AsChecked<CClass>();
            if (Class.GetParentScope()->GetKind() != CScope::EKind::Function)
            {
                return GenerateUseOfDefinition(*Class.Definition());
            }
            TNodeRef<Identifier> Name = GenerateUseOfDefinition(*static_cast<const CFunction*>(Class.GetParentScope()));
            Name->SetTag(PrePostCall::Expression);
            TNodeRef<Clause> ArgumentsClause = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline);
            for (const STypeVariableSubstitution& InstTypeVariable : Class._TypeVariableSubstitutions)
            {
                if (InstTypeVariable._TypeVariable->_ExplicitParam && InstTypeVariable._TypeVariable->_NegativeTypeVariable)
                {
                    ArgumentsClause->AppendChild(GenerateForType(InstTypeVariable._PositiveType));
                }
            }
            ArgumentsClause->SetTag(PrePostCall::SureCall);
            TNodeRef<PrePostCall> Invocation = TNodeRef<PrePostCall>::New(NullWhence());
            Invocation->AppendChild(Move(Name));
            Invocation->AppendChild(Move(ArgumentsClause));
            return Move(Invocation);
        }
        case ETypeKind::Interface:
        {
            const CInterface& Interface = Type.AsChecked<CInterface>();
            if (Interface.GetParentScope()->GetKind() != CScope::EKind::Function)
            {
                return GenerateUseOfDefinition(Interface);
            }
            TNodeRef<Identifier> Name = GenerateUseOfDefinition(*static_cast<const CFunction*>(Interface.GetParentScope()));
            Name->SetTag(PrePostCall::Expression);
            TNodeRef<Clause> ArgumentsClause = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline);
            for (const STypeVariableSubstitution& InstTypeVariable : Interface._TypeVariableSubstitutions)
            {
                if (InstTypeVariable._TypeVariable->_ExplicitParam && InstTypeVariable._TypeVariable->_NegativeTypeVariable)
                {
                    ArgumentsClause->AppendChild(GenerateForType(InstTypeVariable._PositiveType));
                }
            }
            ArgumentsClause->SetTag(PrePostCall::SureCall);
            TNodeRef<PrePostCall> Invocation = TNodeRef<PrePostCall>::New(NullWhence());
            Invocation->AppendChild(Move(Name));
            Invocation->AppendChild(Move(ArgumentsClause));
            return Move(Invocation);
        }
        case ETypeKind::Tuple:
            return GenerateForTupleType(Type.AsChecked<CTupleType>());
        case ETypeKind::Enumeration:
        {
            const CEnumeration& Enumeration = Type.AsChecked<CEnumeration>();
            return GenerateUseOfDefinition(Enumeration);
        }
        case ETypeKind::Option:
        {
            TNodeRef<PrePostCall> Option = TNodeRef<PrePostCall>::New(NullWhence());
            Option->AppendChild(TNodeRef<Clause>::New((uint8_t)PrePostCall::Op::Option, NullWhence(), Clause::EForm::Synthetic));
            Option->AppendChild(GenerateForType(Type.AsChecked<COptionType>().GetValueType()));
            return Option;
        }
        case ETypeKind::Type:
        {
            const CTypeType& TypeType = Type.AsChecked<CTypeType>();
            const CNormalType& NegativeType = TypeType.NegativeType()->GetNormalType();
            const CNormalType& PositiveType = TypeType.PositiveType()->GetNormalType();
            if (NegativeType.IsA<CFalseType>())
            {
                if (PositiveType.IsA<CAnyType>())
                {
                    return GenerateUseOfIntrinsic("type");
                }
                ETypeConstraintFlags SubtypeConstraintFlags = ETypeConstraintFlags::None;
                const CTypeBase& SuperType = SemanticTypeUtils::StripCastableAndConcrete(PositiveType, SubtypeConstraintFlags);
                return GenerateForSubtypeType(&SuperType, SubtypeConstraintFlags);
            }
            if (PositiveType.IsA<CAnyType>())
            {
                return GenerateForSupertypeType(TypeType.NegativeType());
            }
			// `PositiveType` type is positive with respect to its location
			// (in `TypeType`), but not absolutely.  Convert it to an absolute
			// positive type, and similarly for `NegativeType`.
            const CTypeBase& AbsoluteType = SemanticTypeUtils::AsPositive(PositiveType, {});
            // `PositiveType` will not equal `NegativeType` if it is a non-
            // primitive type or aggregate (array, map, option, etc.),
            // transitively, of non-primitive types.
            if (&AbsoluteType != &SemanticTypeUtils::AsPositive(NegativeType, {}))
            {
                _Diagnostics->AppendGlitch(
                    SGlitchResult(
                        EDiagnostic::ErrSemantic_Unimplemented,
                        CUTF8String("Use of type `%s` in a digest is currently unsupported.", TypeType.AsCode().AsCString())),
                    _CurrentGlitchAst);
            }
            return GenerateForType(&AbsoluteType);
        }
        case ETypeKind::Function:
        {
            TNodeRef<Identifier> Name = GenerateUnderscore();
            TNodeRef<PrePostCall> PrePost = TNodeRef<PrePostCall>::New(NullWhence());
            const CFunctionType* FunctionType = &Type.AsChecked<CFunctionType>();
            PrePost->AppendChild(Name);
            PrePost->AppendChild(GenerateForParamTypes(FunctionType->GetParamTypes()));
            GenerateForEffectAttributes(FunctionType->GetEffects(), EffectSets::FunctionDefault, FunctionType->IsClosed(), *PrePost);
            return TNodeRef<Macro>::New(
                NullWhence(),
                GenerateUseOfIntrinsic("type"),
                ClauseArray{ TNodeRef<Clause>::New(TNodeRef<TypeSpec>::New(NullWhence(), PrePost, GenerateForType(&FunctionType->GetReturnType())).As<Node>(), NullWhence(), Clause::EForm::NoSemicolonOrNewline) });
        }
        case ETypeKind::Array:
        {
            TNodeRef<PrePostCall> ArrayTypeFormer = TNodeRef<PrePostCall>::New(NullWhence());
            ArrayTypeFormer->AppendChild(TNodeRef<Clause>::New((uint8_t)PrePostCall::Op::FailCall, NullWhence(), Clause::EForm::NoSemicolonOrNewline));
            ArrayTypeFormer->AppendChild(GenerateForType(Type.AsChecked<CArrayType>().GetElementType()));
            return ArrayTypeFormer;
        }
        case ETypeKind::Generator:
        {
            const CGeneratorType& GeneratorType = Type.AsChecked<CGeneratorType>();
            return GenerateForGeneratorType(GeneratorType.GetElementType());
        }
        case ETypeKind::Map:
        {
            const CMapType& MapType = Type.AsChecked<CMapType>();
            if (MapType.IsWeak())
            {
                return GenerateForWeakMapType(MapType.GetKeyType(), MapType.GetValueType());
            }
            TNodeRef<PrePostCall> MapTypeFormer = TNodeRef<PrePostCall>::New(NullWhence());
            TNodeRef<Clause> KeyTypeClause = TNodeRef<Clause>::New(GenerateForType(MapType.GetKeyType()), NullWhence(), Clause::EForm::NoSemicolonOrNewline);
            KeyTypeClause->SetTag(PrePostCall::FailCall);
            MapTypeFormer->AppendChild(Move(KeyTypeClause));
            MapTypeFormer->AppendChild(GenerateForType(MapType.GetValueType()));
            return MapTypeFormer;
        }
        case ETypeKind::Pointer:
            ULANG_ERRORF("Pointers must be handled at the type spec level.");
            break;
        case ETypeKind::Variable:
        {
            return GenerateDefinitionIdentifier(*Type.AsChecked<CTypeVariable>().Definition());
        }

        case ETypeKind::Unknown:
        case ETypeKind::Module:
        case ETypeKind::Reference:
        case ETypeKind::Named:
        case ETypeKind::Persistable:
        case ETypeKind::Castable:
        case ETypeKind::Concrete:
        default:
            ULANG_ERRORF("Digest generation for %s '%s' is unimplemented.", TypeKindAsCString(Type.GetKind()), Type.AsCode().AsCString());
        };

        return GenerateUseOfIntrinsic("<unknown>");
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForType(const CTypeBase* Type) const
    {
        // If the type is a usable type alias, generate a use of that type alias.
        if (const CAliasType* AliasType = Type->AsAliasType())
        {
            if (IsUsable(AliasType->GetDefinition()))
            {
                return GenerateUseOfDefinition(AliasType->GetDefinition());
            }
        }

        return GenerateForType(Type->GetNormalType());
    }

    void GenerateForEffectAttributes(const SEffectSet Effects, const SEffectSet DefaultEffects, bool bClosed, Verse::Vst::Node& CallAttributable) const
    {
        using namespace Verse::Vst;

        if (TOptional<TArray<const CClass*>> EffectClasses = _Program.ConvertEffectSetToEffectClasses(Effects, DefaultEffects))
        {
            for (const CClass* EffectClass : EffectClasses.GetValue())
            {
                CallAttributable.AppendAux(TNodeRef<Clause>::New(GenerateUseOfDefinition(*EffectClass->Definition()).As<Node>(), NullWhence(), Clause::EForm::IsAppendAttributeHolder));
            }
        }
        if (bClosed)
        {
            CallAttributable.AppendAux(TNodeRef<Clause>::New(GenerateUseOfDefinition(*_Program._closedClass->Definition()).As<Node>(), NullWhence(), Clause::EForm::IsAppendAttributeHolder));
        }
    }

    bool IsEpicInternalOnlyAttributeClass(const CClassDefinition& AttributeClass) const
    {
        // Allow the built-in attributes so the attribute scope attributes used to define attributes
        // that *aren't* epic_internal are preserved.
        if (AttributeClass.IsBuiltIn())
        {
            return false;
        }

        // Also make an exception for the import_as attribute until we can link against a digest without it.
        if (&AttributeClass == _Program._import_as_attribute.Get())
        {
            return false;
        }

        const CDefinition* WorkingDefinition = &AttributeClass;
        while (WorkingDefinition)
        {
            if (WorkingDefinition->DerivedAccessLevel()._Kind == SAccessLevel::EKind::EpicInternal)
            {
                return true;
            }

            for (const CScope* Scope = &WorkingDefinition->_EnclosingScope; Scope; Scope = Scope->GetParentScope())
            {
                WorkingDefinition = Scope->ScopeAsDefinition();
                if (WorkingDefinition)
                {
                    break;
                }
            }
        };

        return false;
    }

    TArray<CUTF8String> ReformatDocCommentAsComments(const CUTF8String& TextValue) const
    {
        TArray<CUTF8String> Result;
        Result.Reserve(8);    // Completely arbitrary choice of how many comments we predict to have as multiline at a time.
        const CUTF8StringView TextView = TextValue.ToStringView();
        const UTF8Char* const ViewEnd = TextView._End;
        const UTF8Char* CommentStart = TextView._Begin;

        // Skip the leading newline that is often found in multiline doc strings.
        if (*CommentStart == '\n')
        {
            ++CommentStart;
        }

        for (const UTF8Char* Ptr = CommentStart; Ptr != ViewEnd; ++Ptr)
        {
            if (*Ptr == '\n')
            {
                const CUTF8StringView CurrentCommentView = CUTF8StringView(CommentStart, Ptr);
                Result.Add(CUTF8String("# %s", CUTF8String(CurrentCommentView).AsCString()));
                CommentStart = Ptr + 1; // Next comment starts after the new line character
            }
        }

        // If the comment doesn't have any newlines at all, or doesn't end in a newline.
        if (CommentStart != ViewEnd)
        {
            const CUTF8StringView CurrentCommentView = CUTF8StringView(CommentStart, ViewEnd);
            Result.Add(CUTF8String("# %s", CUTF8String(CurrentCommentView).AsCString()));
        }
        return Result;
    }

    template<typename Func>
    void GenerateForScopedAttribute(const CScopedAccessLevelDefinition& AccessLevelDefinition, const Func& SelectAttributable) const
    {
        using namespace Verse::Vst;
            
        // Determine which attributable to put the attribute on
        Verse::Vst::TNodeRef<Verse::Vst::Node> Attributable = SelectAttributable(&AccessLevelDefinition);
        TNodeRef<Macro> newScopedMacro = GenerateForScopedMacro(AccessLevelDefinition);

        // attribute nodes must be wrapped in a Clause node (elsewhere used to preserve source comments)
        TNodeRef<Clause> WrapperClause = TNodeRef<Clause>::New(newScopedMacro.As<Node>(), NullWhence(), Clause::EForm::IsAppendAttributeHolder);
        Attributable->AppendAux(WrapperClause);
    }

    const CClass* GetClassForExpression(TSPtr<CExpressionBase> Expression) const
    {
        const CNormalType& ExpressionResultType = Expression->GetResultType(_Program)->GetNormalType();
        if (const CClass* Class = ExpressionResultType.AsNullable<CClass>())
        {
            return Class;
        }
        if (const CTypeType* TypeType = ExpressionResultType.AsNullable<CTypeType>())
        {
            return TypeType->PositiveType()->GetNormalType().AsNullable<CClass>();
        }
        return nullptr;
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForExpression(TSPtr<CExpressionBase> ExprValue) const
    {
        using namespace Verse::Vst;

        TGuardValue<const CAstNode*> GlitchAstGuard(_CurrentGlitchAst, ExprValue);

        Verse::Vst::TNodePtr<Verse::Vst::Node> ResultValue;
        const CTypeBase* TypeBase = ExprValue->GetResultType(_Program);

        switch (ExprValue->GetNodeType())
        {

        case EAstNodeType::Invoke_ArchetypeInstantiation:
        {
            // Archetypes look like:              @TypeName{ arg0 := value0, arg1 := value1 ... }
            // The VST looks like this:
            // Clause:                            @TypeName { A := 0 } breaks down into
            //     Macro:
            //         [0]Identifier:             TypeName
            //         [1]Clause:
            //             [0]Definition:         arg0 := value0
            //                 [0]Identifier:     arg0
            //                 [1]Clause:
            //                     [0]TYPE:       value0
            //             [1]Definition:         arg1 := value1
            //                 [0]Identifier:     arg1
            //                 [1]Clause:
            //                     [0]TYPE:       value1

            TSPtr<CExprArchetypeInstantiation> ArchInstValue = ExprValue.As<CExprArchetypeInstantiation>();

            const CClass* AttributeClass = GetClassForExpression(ExprValue);

            CUTF8String AttributeName;

            // special case for parametric types
            TNodePtr<Node> AttribMacro;
            if (ArchInstValue->_ClassAst->GetNodeType() == EAstNodeType::Invoke_Invocation)
            {
                TSPtr<CExprInvocation> ExprInvoke = ArchInstValue->_ClassAst.As<CExprInvocation>();
                const CFunctionType* FuncType = ExprInvoke->GetResolvedCalleeType();
                if (const CTypeType* ReturnTypeType = FuncType->GetReturnType().GetNormalType().AsNullable<CTypeType>())
                {
                    AttribMacro = GenerateForType(ReturnTypeType->PositiveType());
                }
                else
                {
                    _Diagnostics->AppendGlitch(
                        SGlitchResult(EDiagnostic::ErrDigest_Unimplemented, uLang::CUTF8String("Unsupported instance of type '%s' in attribute.", FuncType->GetReturnType().GetNormalType().AsCode().AsCString())),
                        _CurrentGlitchAst);
                }
            }
            else
            {
                AttribMacro = GenerateForType(AttributeClass);
            }

            TNodeRef<Clause> ValueClause = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::Synthetic, Clause::EPunctuation::Braces);

            // iterate each argument clause and append
            for (const TSRef<uLang::CExpressionBase>& AttribArg : ArchInstValue->Arguments())
            {
                TNodePtr<Identifier> ArgIdent;
                TNodePtr<Clause> ArgValueClause;
                if (AttribArg->GetNodeType() == EAstNodeType::Definition)
                {
                    TSRef<CExprDefinition> AttribArgDef = AttribArg.As<CExprDefinition>();

                    { // IDENT
                        TSPtr<CExprIdentifierData> AttribArgIdentData = AttribArgDef->Element().As<CExprIdentifierData>();
                        ArgIdent = TNodePtr<Identifier>::New(AttribArgIdentData->GetName().AsStringView(), NullWhence());
                    }

                    { // Value
                        TSPtr<CExprInvokeType> AttribArgValue = AttribArgDef->Value().As<CExprInvokeType>();
                        TNodePtr<Node> ArgValue = GenerateForExpression(AttribArgValue->_Argument);

                        ArgValueClause = TNodePtr<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline, Clause::EPunctuation::Unknown);
                        ArgValueClause->AppendChild(ArgValue.AsRef());
                    }
                }

                TNodeRef<Definition> Arg = TNodeRef<Definition>::New(NullWhence(), ArgIdent.AsRef(), ArgValueClause.AsRef());
                ValueClause->AppendChild(Arg);
            }

            TArray<TNodeRef<Clause>> ValueClauses = { ValueClause };

            ResultValue = TNodeRef<Macro>::New(NullWhence(), AttribMacro.AsRef(), TArray<TNodeRef<Clause>>(ValueClauses));
        }
        break;

        case EAstNodeType::Literal_Logic:
        {
            TSPtr<CExprLogic> LogicValue = ExprValue.As<CExprLogic>();
            ResultValue = GenerateUseOfIntrinsic(LogicValue->_Value ? "true" : "false");
        }
        break;

        case EAstNodeType::Literal_Number:
        {
            TSPtr<CExprNumber> Number = ExprValue.As<CExprNumber>();
            if (Number->IsFloat())
            {
                CUTF8String FloatStr = uLang::CUTF8String("%lf", Number->GetFloatValue());
                ResultValue = TNodeRef<FloatLiteral>::New(FloatStr, FloatLiteral::EFormat::F64, NullWhence()).As<Node>();
            }
            else
            {
                CUTF8String IntStr = uLang::CUTF8String("%ld", Number->GetIntValue());
                ResultValue = TNodeRef<IntLiteral>::New(IntStr, NullWhence());
            }
        }
        break;

        case EAstNodeType::Literal_Char:
        {
            TSPtr<CExprChar> CharValue = ExprValue.As<CExprChar>();
            switch (CharValue->_Type)
            {
            case CExprChar::EType::UTF8CodeUnit:
            {
                const bool bIsPrintableASCII = CharValue->_CodePoint >= 32 && CharValue->_CodePoint <= 126;
                ResultValue = TNodeRef<CharLiteral>::New(bIsPrintableASCII ? CharLiteral::EFormat::ASCII : CharLiteral::EFormat::UTF8CodeUnit, CharValue->_CodePoint, NullWhence()).As<Node>();
                break;
            }
            case CExprChar::EType::UnicodeCodePoint:
                ResultValue = TNodeRef<CharLiteral>::New(CharLiteral::EFormat::UnicodeScalarCode, CharValue->_CodePoint, NullWhence()).As<Node>();
                break;
            default:
                _Diagnostics->AppendGlitch(
                    SGlitchResult(EDiagnostic::ErrDigest_Unimplemented, uLang::CUTF8String("Unknown character format type.")),
                    _CurrentGlitchAst);
                break;
            }
        }
        break;

        case EAstNodeType::Literal_String:
        {
            TSPtr<CExprString> StringValue = ExprValue.As<CExprString>();
            ResultValue = TNodeRef<StringLiteral>::New(NullWhence(), StringValue->_String).As<Node>();
        }
        break;

        case EAstNodeType::Literal_Path:
        {
            TSPtr<CExprPath> PathValue = ExprValue.As<CExprPath>();
            ResultValue = TNodeRef<PathLiteral>::New(PathValue->_Path, NullWhence()).As<Node>();
        }
        break;

        case EAstNodeType::Literal_Enum:
        {
            TSPtr<CExprEnumLiteral> EnumValue = ExprValue.As<CExprEnumLiteral>();
            ResultValue = GenerateUseOfDefinition(*EnumValue->_Enumerator).As<Node>();
        }
        break;

        case EAstNodeType::Literal_Type:
        {
            TSPtr<CExprType> TypeValue = ExprValue.As<CExprType>();
            ResultValue = GenerateForType(TypeBase);
        }
        break;

        case EAstNodeType::Identifier_Class:
        {
            TSPtr<CExprIdentifierClass> ClassIdent = ExprValue.As<CExprIdentifierClass>();
            ResultValue = GenerateUseOfDefinition(*ClassIdent->GetClass(_Program)->Definition()).As<Node>();
        }
        break;
        case EAstNodeType::Identifier_Data:
        {
            TSPtr<CExprIdentifierData> DataIdent = ExprValue.As<CExprIdentifierData>();
            ResultValue = GenerateUseOfDefinition(DataIdent->_DataDefinition).As<Node>();
        }
        break;
        case EAstNodeType::Identifier_Function:
        {
            TSPtr<CExprIdentifierFunction> FunctionIdent = ExprValue.As<CExprIdentifierFunction>();
            ResultValue = GenerateUseOfDefinition(FunctionIdent->_Function).As<Node>();
        }
        break;

        case EAstNodeType::Invoke_MakeOption:
        {
            TSPtr<CExprMakeOption> MakeOptionExpr = ExprValue.As<CExprMakeOption>();
            if (TSPtr<CExpressionBase> SubExpr = MakeOptionExpr->Operand())
            {
                TNodePtr<Clause> OptionElementClause = TNodePtr<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline, Clause::EPunctuation::Braces);

                OptionElementClause->AppendChild(GenerateForExpression(SubExpr));

                TArray<TNodeRef<Clause>> MacroClauseArray;
                MacroClauseArray.Add(OptionElementClause.AsRef());

                ResultValue = TNodeRef<Macro>::New(NullWhence(),
                    GenerateUseOfIntrinsic("option"),
                    MacroClauseArray
                ).As<Node>();
            }
            else
            {
                // unset option
                ResultValue = GenerateUseOfIntrinsic("false");
            }
        }
        break;

        case EAstNodeType::Invoke_MakeArray:
        {
            const CArrayType& ArrayNormalType = TypeBase->GetNormalType().AsChecked<CArrayType>();
            const CTypeBase* InnerType = ArrayNormalType.GetInnerType();
            if (InnerType == &_Program._char8Type)
            {
                // string
                TSPtr<CExprString> StringValue = ExprValue.As<CExprString>();
                ResultValue = TNodeRef<StringLiteral>::New(NullWhence(), StringValue->_String).As<Node>();
            }
            else
            {
                TNodeRef<Clause> ArrayElementClause = TNodeRef<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline, Clause::EPunctuation::Braces);

                TSPtr<CExprMakeArray> MakeArrayExpr = ExprValue.As<CExprMakeArray>();
                for (TSPtr<CExpressionBase> SubExpr : MakeArrayExpr->GetSubExprs())
                {
                    ArrayElementClause->AppendChild(GenerateForExpression(SubExpr));
                }

                TArray<TNodeRef<Clause>> MacroClauseArray;
                MacroClauseArray.Add(ArrayElementClause);

                ResultValue = TNodeRef<Macro>::New(NullWhence(),
                    GenerateUseOfIntrinsic("array"),
                    MacroClauseArray
                ).As<Node>();
            }
        }
        break;

        // Currently Unsupported
        case EAstNodeType::Error_:
        case EAstNodeType::Placeholder_:
        case EAstNodeType::Noop_:
        case EAstNodeType::External:
        case EAstNodeType::PathPlusSymbol:
        case EAstNodeType::Literal_Function:
        case EAstNodeType::Identifier_Unresolved:
        case EAstNodeType::Identifier_Module:
        case EAstNodeType::Identifier_ModuleAlias:
        case EAstNodeType::Identifier_Enum:
        case EAstNodeType::Identifier_Interface:
        case EAstNodeType::Identifier_TypeAlias:
        case EAstNodeType::Identifier_TypeVariable:
        case EAstNodeType::Identifier_OverloadedFunction:
        case EAstNodeType::Identifier_Self:
        case EAstNodeType::Identifier_Local:
        case EAstNodeType::Identifier_BuiltInMacro:
        case EAstNodeType::Definition:
        case EAstNodeType::MacroCall:
        case EAstNodeType::Invoke_Invocation:
        case EAstNodeType::Invoke_UnaryArithmetic:
        case EAstNodeType::Invoke_BinaryArithmetic:
        case EAstNodeType::Invoke_ShortCircuitAnd:
        case EAstNodeType::Invoke_ShortCircuitOr:
        case EAstNodeType::Invoke_LogicalNot:
        case EAstNodeType::Invoke_Comparison:
        case EAstNodeType::Invoke_QueryValue:
        case EAstNodeType::Invoke_TupleElement:
        case EAstNodeType::Invoke_MakeMap:
        case EAstNodeType::Invoke_MakeTuple:
        case EAstNodeType::Invoke_MakeRange:
        case EAstNodeType::Invoke_Type:
        case EAstNodeType::Invoke_PointerToReference:
        case EAstNodeType::Invoke_Set:
        case EAstNodeType::Invoke_NewPointer:
        case EAstNodeType::Invoke_ReferenceToValue:
        case EAstNodeType::Assignment:
        case EAstNodeType::Invoke_ArrayFormer:
        case EAstNodeType::Invoke_GeneratorFormer:
        case EAstNodeType::Invoke_MapFormer:
        case EAstNodeType::Invoke_OptionFormer:
        case EAstNodeType::Invoke_Subtype:
        case EAstNodeType::Invoke_TupleType:
        case EAstNodeType::Invoke_Arrow:
        case EAstNodeType::Flow_CodeBlock:
        case EAstNodeType::Flow_Let:
        case EAstNodeType::Flow_Defer:
        case EAstNodeType::Flow_If:
        case EAstNodeType::Flow_First:
        case EAstNodeType::Flow_Iteration:
        case EAstNodeType::Flow_Loop:
        case EAstNodeType::Flow_Break:
        case EAstNodeType::Flow_Return:
        case EAstNodeType::Flow_ProfileBlock:
        case EAstNodeType::Ir_AllOne:
        case EAstNodeType::Ir_For:
        case EAstNodeType::Ir_IteratedBody:
        case EAstNodeType::Ir_ArrayAdd:
        case EAstNodeType::Ir_MapAdd:
        case EAstNodeType::Ir_ArrayUnsafeCall:
        case EAstNodeType::Ir_ConvertToDynamic:
        case EAstNodeType::Ir_ConvertFromDynamic:
        case EAstNodeType::Ir_DataOfDynamic:
        case EAstNodeType::Concurrent_Sync:
        case EAstNodeType::Concurrent_Rush:
        case EAstNodeType::Concurrent_Race:
        case EAstNodeType::Concurrent_SyncIterated:
        case EAstNodeType::Concurrent_RushIterated:
        case EAstNodeType::Concurrent_RaceIterated:
        case EAstNodeType::Concurrent_Branch:
        case EAstNodeType::Concurrent_Spawn:
        case EAstNodeType::Concurrent_Await:
        case EAstNodeType::Concurrent_Batch:
        case EAstNodeType::Concurrent_Upon:
        case EAstNodeType::Concurrent_When:
        case EAstNodeType::Definition_Module:
        case EAstNodeType::Definition_Enum:
        case EAstNodeType::Definition_Interface:
        case EAstNodeType::Definition_Class:
        case EAstNodeType::Definition_Data:
        case EAstNodeType::Definition_IterationPair:
        case EAstNodeType::Definition_Function:
        case EAstNodeType::Definition_TypeAlias:
        case EAstNodeType::Definition_Using:
        case EAstNodeType::Definition_Import:
        case EAstNodeType::Definition_Where:
        case EAstNodeType::Definition_Var:
        case EAstNodeType::Definition_Live:
        case EAstNodeType::Definition_ScopedAccessLevel:
        case EAstNodeType::Invoke_MakeNamed:
        case EAstNodeType::Context_Project:
        case EAstNodeType::Context_CompilationUnit:
        case EAstNodeType::Context_Package:
        case EAstNodeType::Context_Snippet:
        default:
            _Diagnostics->AppendGlitch(
                SGlitchResult(EDiagnostic::ErrDigest_Unimplemented, uLang::CUTF8String("Unsupported expression type in digest generation.")),
                _CurrentGlitchAst);
            break;
        }

        return ResultValue.AsRef();
    }

    template<typename Func>
    void GenerateForAttributeArchetype(TSPtr<uLang::CExprArchetypeInstantiation> AttributeExpr, const CClass* AttributeClass, const Func& SelectAttributable) const
    {
        using namespace Verse::Vst;

        // Determine which attributable to put the attribute on
        Verse::Vst::TNodeRef<Verse::Vst::Node> Attributable = SelectAttributable(AttributeClass);
        
        if (!_bIncludeEpicInternalDefinitions && IsEpicInternalOnlyAttributeClass(*AttributeClass->_Definition))
        {
            // Filter out Epic-internal attributes from public-only digests.
            return;
        }

        DeclareDependencyOnScope(*AttributeClass);

        TNodeRef<Node> MacroInst = GenerateForExpression(AttributeExpr);
        MacroInst->SetNewLineAfter(true);  // For digest purposes, we force newlines after so that it looks neater.

        // attribute nodes must be wrapped in a Clause node (elsewhere used to preserve source comments)
        TNodeRef<Clause> WrapperClause = TNodeRef<Clause>::New(MacroInst.As<Node>(), NullWhence(), Clause::EForm::IsAppendAttributeHolder);
        Attributable->AppendAux(WrapperClause);
    }

    template<typename Func>
    void GenerateForAttributeGeneric(const CClass* AttributeClass, const uLang::TOptional<CUTF8String>& TextValue, const Func& SelectAttributable) const
    {
        using namespace Verse::Vst;

        // Determine which attributable to put the attribute on
        Verse::Vst::TNodeRef<Verse::Vst::Node> Attributable = SelectAttributable(AttributeClass);
        if (AttributeClass == _Program._doc_attribute.Get() && TextValue.IsSet())
        {
            // Replace doc comments with line comments in the digest, regardless of whether it includes epic_internal definitions.
            for (const CUTF8String& CommentString : ReformatDocCommentAsComments(TextValue.GetValue()))
            {
                Verse::Vst::TNodeRef<Verse::Vst::Comment> NewComment = Verse::Vst::TNodeRef<Verse::Vst::Comment>::New(Verse::Vst::Comment::EType::line, CommentString, NullWhence());
                NewComment->SetNumNewLinesAfter(1);
                // NOTE: (yiliang.siew) If the identifier that has this attribute has a qualifier, we need to add
                // this to the correct specific comment storage, or else the pretty printer will make the wrong assumptions
                // about how to print the comments during roundtripping.
                if (Verse::Vst::Identifier* MaybeIdentifier = Attributable->AsNullable<Verse::Vst::Identifier>(); MaybeIdentifier && MaybeIdentifier->IsQualified())
                {
                    MaybeIdentifier->_QualifierPreComments.Add(NewComment);
                }
                else
                {
                    Attributable->AppendPrefixComment(NewComment);
                }
            }
            return;
        }
        else if (!_bIncludeEpicInternalDefinitions && IsEpicInternalOnlyAttributeClass(*AttributeClass->_Definition))
        {
            // Filter out Epic-internal attributes from public-only digests.
            return;
        }
        else if (AttributeClass == _Program._getterClass || AttributeClass == _Program._setterClass)
        {
            // getters/setters are special; we don't want them to appear in any digests, regardless
            // of access level
            return;
        }

        const CDefinition* AttributeDefinition = AttributeClass->Definition();

        DeclareDependencyOnScope(*AttributeClass);

        // SOL-972 & SOL-2577: Some attributes are implemented as a function and a class, and they can't have the same name.
        // The function has the same name as the attribute, and is what we need here.
        // The class has "_attribute" appended, and is what we have.
        // This is not done for all attributes, hence the if-statement.
        if (AttributeDefinition->AsNameStringView().EndsWith("_attribute"))
        {
            CUTF8StringView ConstructorName = AttributeDefinition->AsNameStringView().SubViewTrimEnd(static_cast<int32_t>(strlen("_attribute")));
            const CSymbol ConstructorSymbol = _Program.GetSymbols()->AddChecked(ConstructorName);
            if (const CFunction* AttributeConstructor = AttributeDefinition->_EnclosingScope.GetLogicalScope().FindFirstDefinitionOfKind<CFunction>(ConstructorSymbol))
            {
                AttributeDefinition = AttributeConstructor;
            }
        }

        if (TextValue.IsSet())
        {
            TNodeRef<StringLiteral> Value = TNodeRef<StringLiteral>::New(NullWhence(), TextValue.GetValue());
            TNodeRef<Clause> ValueClause = TNodeRef<Clause>::New(Value.As<Node>(), NullWhence(), Clause::EForm::Synthetic);
            TNodeRef<Identifier> Name = GenerateUseOfDefinition(*AttributeDefinition);
            Name->SetTag(PrePostCall::Op::Expression);
            ValueClause->SetTag(PrePostCall::Op::SureCall);
            TNodeRef<PrePostCall> Call = TNodeRef<PrePostCall>::New(NullWhence());
            Call->AppendChild(Name);
            Call->AppendChild(ValueClause);
            Call->SetNewLineAfter(true);  // For digest purposes, we force newlines after so that it looks neater.
            // attribute nodes must be wrapped in a Clause node (elsewhere used to preserve source comments)
            TNodeRef<Clause> WrapperClause = TNodeRef<Clause>::New(Call.As<Node>(), NullWhence(), Clause::EForm::IsAppendAttributeHolder);
            Attributable->AppendAux(WrapperClause);
        }
        else
        {
            // attribute nodes must be wrapped in a Clause node (elsewhere used to preserve source comments)
            TNodeRef<Identifier> AttributeIdentifier = GenerateUseOfDefinition(*AttributeDefinition);
            // NOTE: (yiliang.siew) We do this so that `<epic_internal>` and other attributes that are suffixed to the identifier
            // do not get newlines after them, only the prefix attributes on the definition itself.
            if (Attributable->IsA<Definition>() || Attributable->IsA<TypeSpec>())
            {
                AttributeIdentifier->SetNewLineAfter(true);
            }
            TNodeRef<Clause> WrapperClause = TNodeRef<Clause>::New(AttributeIdentifier.As<Node>(), NullWhence(), Clause::EForm::IsAppendAttributeHolder);
            Attributable->AppendAux(WrapperClause);
        }
    }

    template<typename Func>
    void GenerateForAttributesGeneric(const TArray<SAttribute>& Attributes, const TOptional<SAccessLevel>& AccessLevel, const Func& SelectAttributable) const
    {
        for (const SAttribute& Attribute : Attributes)
        {
            // Determine the attribute class
            const CClass* AttributeClass = GetClassForExpression(Attribute._Expression);

            if (ULANG_ENSUREF(AttributeClass, "Unrecognized attribute type."))
            {
                const bool bIsAccessLevelAttribute = 
                       AttributeClass == _Program._publicClass
                    || AttributeClass == _Program._internalClass
                    || AttributeClass == _Program._protectedClass
                    || AttributeClass == _Program._privateClass
                    || AttributeClass == _Program._epicInternalClass;

                if (AttributeClass->IsSubclassOf(*_Program._scopedClass))
                {
                    GenerateForScopedAttribute(*static_cast<const CScopedAccessLevelDefinition*>(AttributeClass), SelectAttributable);
                }
                else if (!bIsAccessLevelAttribute)
                {
                    if (Attribute._Expression->GetNodeType() == EAstNodeType::Invoke_ArchetypeInstantiation)
                    {
                        GenerateForAttributeArchetype(Attribute._Expression.As<CExprArchetypeInstantiation>(), AttributeClass, SelectAttributable);
                    }
                    else
                    {
                        const uLang::TOptional<CUTF8String> TextValue = CAttributable::GetAttributeTextValue(Attributes, AttributeClass, _Program);
                        GenerateForAttributeGeneric(AttributeClass, TextValue, SelectAttributable);
                    }
                }
            }
        }

        if (AccessLevel.IsSet())
        {
            switch(AccessLevel.GetValue()._Kind)
            {
            case SAccessLevel::EKind::Public: GenerateForAttributeGeneric(_Program._publicClass, {}, SelectAttributable); break;
            case SAccessLevel::EKind::Internal: GenerateForAttributeGeneric(_Program._internalClass, {}, SelectAttributable); break;
            case SAccessLevel::EKind::Protected: GenerateForAttributeGeneric(_Program._protectedClass, {}, SelectAttributable); break;
            case SAccessLevel::EKind::Private: GenerateForAttributeGeneric(_Program._privateClass, {}, SelectAttributable); break;
            case SAccessLevel::EKind::Scoped: /* handled above */ break;
            case SAccessLevel::EKind::EpicInternal: GenerateForAttributeGeneric(_Program._epicInternalClass, {}, SelectAttributable); break;
            default: ULANG_UNREACHABLE();
            }
        }
    }

    void GenerateForAttributes(const CAttributable& Attributes, const TOptional<SAccessLevel>& AccessLevel, const Verse::Vst::TNodeRef<Verse::Vst::Node>& Attributable) const
    {
        GenerateForAttributesGeneric(Attributes._Attributes, AccessLevel, [&Attributable] (const CClass*) { return Attributable; });
    }

    void GenerateForAttributes(const TArray<SAttribute>& Attributes, const CDefinition& Definition, const Verse::Vst::TNodeRef<Verse::Vst::Identifier>& NameAttributable, const Verse::Vst::TNodeRef<Verse::Vst::Node>& DefAttributable) const
    {
        GenerateForAttributesGeneric(
            Attributes,
            Definition.SelfAccessLevel(),
            [this, &NameAttributable, &DefAttributable] (const CClass* AttributeClass)
            {
                Verse::Vst::TNodePtr<Verse::Vst::Node> Attributable;
                if (AttributeClass->GetAttributes().HasAttributeClass(_Program._attributeScopeName, _Program))
                {
                    Attributable = NameAttributable;
                }
                else
                {
                    Attributable = DefAttributable;
                }
                return Attributable.AsRef();
            });
    }

    void GenerateForAttributes(const CDefinition& Definition, const Verse::Vst::TNodeRef<Verse::Vst::Identifier>& NameAttributable, const Verse::Vst::TNodeRef<Verse::Vst::Node>& DefAttributable) const
    {
        GenerateForAttributes(Definition.GetAttributes()._Attributes, Definition, NameAttributable, DefAttributable);
    }

    Verse::Vst::TNodeRef<Verse::Vst::Macro> GenerateExternalMacro() const
    {
        using namespace Verse::Vst;
        return TNodeRef<Macro>::New(
            NullWhence(),
            GenerateUseOfIntrinsic("external"),
            ClauseArray{TNodeRef<Clause>::New(NullWhence(), Clause::EForm::NoSemicolonOrNewline)});
    }

    bool IsUsable(const CDefinition& Definition) const
    {
        SAccessLevel AccessLevel;
        if (Definition._EnclosingScope.GetKind() == CScope::EKind::Function)
        {
            AccessLevel = static_cast<const CFunction&>(Definition._EnclosingScope).DerivedAccessLevel();
        }
        else
        {
            AccessLevel = Definition.DerivedAccessLevel();
        }

        if (AccessLevel._Kind == Cases<SAccessLevel::EKind::Private, SAccessLevel::EKind::Internal>)
        {
            return false;
        }

        bool bSpecialException = false;
        bSpecialException |= IsEpicInternalSpecialException(Definition);

        if (AccessLevel._Kind == SAccessLevel::EKind::EpicInternal
            && !_bIncludeEpicInternalDefinitions
            // Don't cull inheriting from epic_internal definitions in the intrinsically defined
            // built-in snippet (e.g.attribute).
            && !Definition.IsBuiltIn()
            && !bSpecialException)
        {
            return false;
        }

        const CAstPackage* DefinitionPackage = Definition._EnclosingScope.GetPackage();
        if (!_bIncludeEpicInternalDefinitions &&
            DefinitionPackage && DefinitionPackage->_VerseScope == EVerseScope::InternalAPI)
        {
            return false;
        }

        return true;
    }

    // For a given class, find the nearest ancestor that is public
    const CClass* PublifySuperclass(const CClass* Class) const
    {
        while (Class && !IsUsable(*Class->_Definition))
        {
            Class = Class->_Superclass;
        }

        return Class;
    }

    // For a given single interface, find the set of nearest ancestors that are all public
    TArray<const CInterface*> PublifySuperInterface(const CInterface* Interface) const
    {
        // Is it public?
        if (IsUsable(*Interface))
        {
            // Yes, return as-is
            return {Interface};
        }

        // No, find public super interfaces
        TArray<const CInterface*> Result;
        for (const CInterface* SuperInterface : Interface->_SuperInterfaces)
        {
            TArray<const CInterface*> PublicSuperInterfaces = PublifySuperInterface(SuperInterface);
            for (const CInterface* PublicSuperInterface : PublicSuperInterfaces)
            {
                Result.Add(PublicSuperInterface);
            }
        }

        return Result;
    }

    // For a given set of interfaces, find the set of nearest ancestors that are all public
    TArray<const CInterface*> PublifySuperInterfaces(const TArray<CInterface*>& Interfaces) const
    {
        TArray<const CInterface*> Result;
        for (const CInterface* Interface : Interfaces)
        {
            TArray<const CInterface*> PublicSuperInterfaces = PublifySuperInterface(Interface);
            for (const CInterface* PublicSuperInterface : PublicSuperInterfaces)
            {
                Result.Add(PublicSuperInterface);
            }
        }

        return Result;
    }

    TArray<const CNominalType*> PublifyType(const CTypeBase* TypeToPublify, TArray<const CInterface*>& VisitedPublicSuperInterfaces) const
    {
        if (const CClass* Class = TypeToPublify->GetNormalType().AsNullable<CClass>())
        {
            if (IsUsable(*Class->_Definition))
            {
                return { Class };
            }
            else
            {
                TArray<const CNominalType*> Publified;

                if (Class->_Superclass)
                {
                    Publified.Append(PublifyType(Class->_Superclass, VisitedPublicSuperInterfaces));
                }
                
                for (const CInterface* SuperInterface : PublifySuperInterfaces(Class->_SuperInterfaces))
                {
                    int32_t NumVisitedSuperInterfaces = VisitedPublicSuperInterfaces.Num();
                    if (VisitedPublicSuperInterfaces.AddUnique(SuperInterface) == NumVisitedSuperInterfaces)
                    {
                        Publified.Add(SuperInterface);
                    }
                }

                return Publified;
            }
        }
        else if (const CInterface* Interface = TypeToPublify->GetNormalType().AsNullable<CInterface>())
        {
            if (IsUsable(*Interface))
            {
                int32_t NumVisitedSuperInterfaces = VisitedPublicSuperInterfaces.Num();
                if (VisitedPublicSuperInterfaces.AddUnique(Interface) == NumVisitedSuperInterfaces)
                {
                    return { Interface };
                }
                else
                {
                    return {};
                }
            }
            
            TArray<const CNominalType*> Publified;

            for (const CInterface* SuperInterface : PublifySuperInterfaces(Interface->_SuperInterfaces))
            {
                int32_t NumVisitedSuperInterfaces = VisitedPublicSuperInterfaces.Num();
                if (VisitedPublicSuperInterfaces.AddUnique(SuperInterface) == NumVisitedSuperInterfaces)
                {
                    Publified.Add(SuperInterface);
                }
            }

            return Publified;
        }

        return {};
    }

    bool DefinitionSubjectToScopedAccess(const CDefinition& Definition) const
    {
        if (Definition.DerivedAccessLevel()._Kind == SAccessLevel::EKind::Scoped)
        {
            return true;
        }

        for (const CScope* Scope = &Definition._EnclosingScope; Scope != nullptr; Scope = Scope->GetParentScope())
        {
            if (const CDefinition* ScopeDefinition = Scope->ScopeAsDefinition())
            {
                if (ScopeDefinition->DerivedAccessLevel()._Kind == SAccessLevel::EKind::Scoped)
                {
                    return true;
                }
            }
        }

        return false;
    }

    bool ShouldGenerate(const CDefinition& Definition, const bool bCheckPackage = true) const
    {
        // We generate the digest for the current user access context - for a "general" public digest, this would be
        // equivalent to stripping out anything that isn't universally accessible.

        // NOTE: (yiliang.siew) Don't generate for definitions that are auto-generated and are not meant to be
        // user-visible.  E.g. the special data definitions we create in the IR for interface fields with default values
        // (members that get a special `_def` suffix appended to their names).
        if (Definition.GetName().IsGenerated())
        {
            return false;
        }

        // Don't generate field overrides if their base field wouldn't be generated due to being in an inaccessible
        // class/interface.
        if (&Definition.GetBaseOverriddenDefinition() != &Definition)
        {
            if (const CDefinition* BaseOverrideEnclosingDefinition =
                    Definition.GetBaseClassOverriddenDefinition().GetEnclosingDefinition())
            {
                if (!ShouldGenerate(*BaseOverrideEnclosingDefinition, false))
                {
                    return false;
                }
            }
        }

        if (Definition._EnclosingScope.GetKind() == CScope::EKind::Function)
        {
            return ShouldGenerate(static_cast<const CFunction&>(Definition._EnclosingScope), bCheckPackage);
        }

        // Don't generate digest definitions for the intrinsically defined built-in snippet, or definitions outside the current package.
        const CAstPackage* Package = Definition._EnclosingScope.GetPackage();
        if (Definition.IsBuiltIn()
            || (bCheckPackage && Package && Package != &_Package))
        {
            return false;
        }

        bool bSpecialException = false;

        // Make an exception for the import_as attribute until we can link against a digest without it.
        bSpecialException |= &Definition == _Program._import_as_attribute.Get();
        bSpecialException |= &Definition == _Program._import_as.Get();

        // Make a special exception for localization related functionality that 
        // is epic_internal and needs to be visible to user code
        bSpecialException |= IsEpicInternalSpecialException(Definition);

        // @HACK: https://jira.it.epicgames.com/browse/SOL-8201 These fields were accidentally marked <public> in 36.00 but should have been <private>.
        if (VerseFN::UploadedAtFNVersion::ForcePublicInternalTransformsOnTransformComponent(_Package._UploadedAtFNVersion))
        {
            if (Definition._EnclosingScope.GetScopePath('/', EPathMode::Default) == "Verse.org/SceneGraph/transform_component")
            {
                const CUTF8StringView DefString = Definition.GetName().AsStringView();
                if (DefString == "GlobalTransformInternal" || DefString == "LocalTransformInternal" || DefString == "OriginInternal")
                {
                    bSpecialException = true;
                }
            }
        }

        // If this definition is living under a scoped access level, then we can't be sure it's not being accessed from elsewhere
        // TODO: (yiliang.siew) This is not-quite correct; we should be emitting only these definitions based on the actual accessibility context.
        const bool bWithinAScopedScope = DefinitionSubjectToScopedAccess(Definition);

        const SAccessLevel& DefinitionAccessLevel = Definition.DerivedAccessLevel();
        return DefinitionAccessLevel._Kind == Cases<SAccessLevel::EKind::Public, SAccessLevel::EKind::Protected>
            || (_bIncludeInternalDefinitions && DefinitionAccessLevel._Kind == SAccessLevel::EKind::Internal)
            || (_bIncludeEpicInternalDefinitions && DefinitionAccessLevel._Kind == SAccessLevel::EKind::EpicInternal)
            || bSpecialException
            || (bWithinAScopedScope && DefinitionAccessLevel._Kind != SAccessLevel::EKind::EpicInternal);
    }

    void DeclareDependencyOnScope(const CScope& Scope) const
    {
        const CModule* UsingModule = Scope.GetModule();

        ULANG_ASSERTF(UsingModule != nullptr, "Definition._EnclosingScope does not have a module.");

        if (!_CurrentModule->IsSameOrChildOf(UsingModule)
            && UsingModule != _Program._VerseModule)
        {
            CUTF8String UsingVersePath;
            const TArray<const CModule*> ModuleHierarchyLimitedByAvailable = GetModuleHierarchyLimitedByAvailableAttribute(UsingModule);
            if (!ModuleHierarchyLimitedByAvailable.IsEmpty())
            {
                const CModule* ModuleInAvailableHierarchy = ModuleHierarchyLimitedByAvailable.Last();
                if (const CScope* ModuleParentScope = ModuleInAvailableHierarchy->GetParentScope())
                {
                    UsingVersePath = ModuleParentScope->GetScopePath('/', CScope::EPathMode::PrefixSeparator);
                }
                else
                {
                    _Diagnostics->AppendGlitch(
                        SGlitchResult(
                            EDiagnostic::ErrDigest_DisallowedUsing,
                            uLang::CUTF8String("Expecting `%s` to have an enclosing module since it is using the @available attribute.",
                                *ModuleInAvailableHierarchy->GetName().AsString())),
                        _CurrentGlitchAst);
                }
            }
            else
            {
                UsingVersePath = UsingModule->GetScopePath('/', CScope::EPathMode::PrefixSeparator);
            }

            if (_Usings.Find(UsingVersePath) == IndexNone)
            {
                const CAstPackage* UsingPackage = Scope.GetPackage();
                if (!_bIncludeEpicInternalDefinitions && UsingPackage->_VerseScope == EVerseScope::InternalAPI)
                {
                    _Diagnostics->AppendGlitch(
                        SGlitchResult(
                            EDiagnostic::ErrDigest_DisallowedUsing,
                            uLang::CUTF8String("Package `%s` is publicly visible but its public interface depends on `%s` from package `%s` which is not publicly visible.",
                            *_Package._Name,
                            *Scope.GetScopePath('/', CScope::EPathMode::PrefixSeparator),
                            *UsingPackage->_Name)),
                        _CurrentGlitchAst);
                }
                else
                {
                    _Usings.Add(UsingVersePath);
                }
            }
        }

        const CAstPackage* ScopePackage = Scope.GetPackage();
        if (ScopePackage && ScopePackage != &_Package && ScopePackage != _Program._BuiltInPackage)
        {
            _DependencyPackages.Insert(ScopePackage);
        }
    }

    bool GetModuleHierarchyLimitedByAvailableAttributeImpl(const CModule* CurrentModule, TArray<const CModule*>& ModulesOut) const
    {
        if (!CurrentModule)
        {
            return false;
        }
        
        ModulesOut.Add(CurrentModule);
        
        if (CurrentModule->GetAttributes().FindAttribute(_Program._availableClass, _Program))
        {
            return true;
        }

        if (const CScope* C = CurrentModule->GetParentScope())
        {
            return GetModuleHierarchyLimitedByAvailableAttributeImpl(C->GetModule(), ModulesOut);
        }

        return false;
    }

    // Returns the enclosing modules to `InModule` up to and including the closest enclosing module to `InModule` that is using the @available attribute (either explicitly or implicitly), in reverse order.
    // I.e. the first element in the array is the leaf module `InModule`. If no module in this chain of enclosing scopes is using @available the returned array is empty.
    TArray<const CModule*> GetModuleHierarchyLimitedByAvailableAttribute(const CModule* InModule) const
    {
        TArray<const CModule*> ModulesOut;
        if (GetModuleHierarchyLimitedByAvailableAttributeImpl(InModule, ModulesOut))
        {
            return ModulesOut;
        }

        return {};
    }

    Verse::Vst::TNodeRef<Verse::Vst::Node> GenerateForQualifier(Verse::Vst::TNodeRef<Verse::Vst::Identifier> Identifier, const SQualifier& Qualifier, const CScope* Scope) const
    {
        if (Qualifier._Type == SQualifier::EType::Local)
        {
            return GenerateUseOfIntrinsic("local");
        }
        const CScope* LogicalScope;
        if (Qualifier._Type == SQualifier::EType::NominalType)
        {
            ULANG_ASSERTF(Qualifier.GetNominalType(), "Invalid qualifier state encountered.");
            // TODO: (yiliang.siew) For now we are always using the full path for the qualifier in this case until we implement
            // logic to use the minimum qualification path possible.
            const CDefinition* Definition = Qualifier.GetNominalType()->Definition();
            LogicalScope = Definition->DefinitionAsLogicalScopeNullable();
            
            // Only ever use the "MyModule." way of qualifying things outside of the MyModule itself.
            const CModule* Module = LogicalScope->GetModule();
            const TArray<const CModule*> ModuleHierarchyLimitedByAvailable = GetModuleHierarchyLimitedByAvailableAttribute(Module);
            if (!ModuleHierarchyLimitedByAvailable.IsEmpty()
                && Scope 
                &&!Scope->IsSameOrChildOf(Module))
            {
                if (!LogicalScope->IsModuleOrSnippet())
                {
                    // NominalType is a class/interface — the @available module prefix applies to the type, not the member.
                    SQualifier TypeQualifier = Definition->GetImplicitQualifier();
                    if (!TypeQualifier.IsUnspecified())
                    {
                        Verse::Vst::TNodeRef<Verse::Vst::Identifier> TypeIdentifier = Verse::Vst::TNodeRef<Verse::Vst::Identifier>::New(
                            Definition->AsNameStringView(), NullWhence());
                        Verse::Vst::TNodeRef<Verse::Vst::Node> TypeQualifierNode = GenerateForQualifier(
                            TypeIdentifier, TypeQualifier, Scope);
                        TypeIdentifier->AppendChild(TypeQualifierNode);
                        return TypeIdentifier;
                    }
                }
                else
                {
                    for (const CModule* ModuleInAvailableHierarchy : ModuleHierarchyLimitedByAvailable)
                    {
                        CUTF8String IdentifierPrefix = ModuleInAvailableHierarchy->GetName().AsString();
                        Identifier->_OriginalCode = IdentifierPrefix + "." + Identifier->_OriginalCode;
                        LogicalScope = ModuleInAvailableHierarchy->GetParentScope();
                    }
                }
            }
        }
        else
        {
            ULANG_ASSERT(Qualifier._Type == SQualifier::EType::LogicalScope);
            LogicalScope = Qualifier.GetLogicalScope();
        }

        ULANG_ASSERT(LogicalScope);
        CUTF8String QualifierPath = LogicalScope->GetScopePath('/', CScope::EPathMode::PrefixSeparator);
        return Verse::Vst::TNodeRef<Verse::Vst::PathLiteral>::New(QualifierPath, NullWhence());
    }

    bool NeedsQualification(CSymbol Symbol, const CScope* Scope) const
    {
        ULANG_ASSERT(Scope); // Workaround StaticAnalyzer bug: spurious warning C28182: Dereferencing NULL pointer
        const CDefinition* UnambiguousResolution = nullptr;

        auto ProcessDefinition = [&](const CDefinition* Definition) -> bool
        {
            if (!UnambiguousResolution)
            {
                UnambiguousResolution = Definition;
            }
            else if (Definition->GetImplicitQualifier() != UnambiguousResolution->GetImplicitQualifier())
            {
                return true;
            }
            return false;
        };

        // Source 1: Global (module/snippet) definitions — these could become visible
        // through `using` directives added by digest amalgamation.
        if (const SSymbolDefinitionArray* GlobalDefinitions = _Program.GetGlobalDefinitionsBySymbol(Symbol))
        {
            for (const CDefinition* Definition : *GlobalDefinitions)
            {
                if (ProcessDefinition(Definition))
                {
                    return true;
                }
            }
        }

        // Source 2: Non-global definitions visible from Scope via scope hierarchy
        // and inheritance. Walk up from Scope, stopping at module/snippet scopes
        // (already covered by Source 1). At each logical scope, FindDefinitions
        // handles inheritance (superclasses, interfaces) automatically.
        for (const CScope* WalkScope = Scope;
             WalkScope && !WalkScope->IsModuleOrSnippet();
             WalkScope = WalkScope->GetParentScope())
        {
            if (const CLogicalScope* LogicalScope = WalkScope->AsLogicalScopeNullable())
            {
                for (CDefinition* Definition : LogicalScope->FindDefinitions(Symbol))
                {
                    if (ProcessDefinition(Definition))
                    {
                        return true;
                    }
                }
            }
        }

        if (UnambiguousResolution)
        {
            const CModule* EnclosingModule = UnambiguousResolution->_EnclosingScope.GetModule();
            const TArray<const CModule*> ModuleHierarchyLimitedByAvailable = GetModuleHierarchyLimitedByAvailableAttribute(EnclosingModule);
            if (!ModuleHierarchyLimitedByAvailable.IsEmpty()
                && UnambiguousResolution->DefinitionAsLogicalScopeNullable()
                && EnclosingModule && !Scope->IsSameOrChildOf(EnclosingModule))
            {
                return true;
            }
        }

        return false;
    }

    Verse::Vst::TNodeRef<Verse::Vst::Identifier> GenerateIdentifierWithQualifierIfNeeded(CUTF8StringView IdentifierString, CSymbol SymbolToResolve, SQualifier ImplicitQualifier, const CScope* Scope, bool bNeverQualify = false) const
    {
        using namespace Verse::Vst;

        TNodeRef<Identifier> IdentifierNode = TNodeRef<Identifier>::New(IdentifierString, NullWhence());

        TNodePtr<Node> QualifierNode;
        if (!ImplicitQualifier.IsUnspecified())
        {
            if (!bNeverQualify && NeedsQualification(SymbolToResolve, Scope))
            {
                QualifierNode = GenerateForQualifier(IdentifierNode, ImplicitQualifier, Scope);
            }
        }

        if (QualifierNode)
        {
            IdentifierNode->AppendChild(QualifierNode.AsRef());
        }

        return IdentifierNode;
    }

    Verse::Vst::TNodeRef<Verse::Vst::Identifier> GenerateUnderscore() const
    {
        return Verse::Vst::TNodeRef<Verse::Vst::Identifier>::New(_Underscore.AsStringView(), NullWhence());
    }

    Verse::Vst::TNodeRef<Verse::Vst::Identifier> GenerateDefinitionIdentifier(CUTF8StringView IdentifierString, const CDefinition& Definition, bool bNeverQualify = false) const
    {
        return GenerateIdentifierWithQualifierIfNeeded(IdentifierString, Definition.GetName(), Definition.GetImplicitQualifier(), &Definition._EnclosingScope, bNeverQualify);
    }

    Verse::Vst::TNodeRef<Verse::Vst::Identifier> GenerateDefinitionIdentifier(const CDefinition& Definition, bool bNeverQualify = false) const
    {
        return GenerateIdentifierWithQualifierIfNeeded(Definition.AsNameStringView(), Definition.GetName(), Definition.GetImplicitQualifier(), &Definition._EnclosingScope, bNeverQualify);
    }

    Verse::Vst::TNodeRef<Verse::Vst::Identifier> GenerateUseOfDefinition(const CDefinition& Definition) const
    {
        DeclareDependencyOnScope(Definition._EnclosingScope);
        return GenerateIdentifierWithQualifierIfNeeded(Definition.AsNameStringView(), Definition.GetName(), Definition.GetImplicitQualifier(), _CurrentScope);
    }

    Verse::Vst::TNodeRef<Verse::Vst::Identifier> GenerateUseOfIntrinsic(const CUTF8StringView& IntrinsicName) const
    {
        return GenerateIdentifierWithQualifierIfNeeded(IntrinsicName, _Program.GetSymbols()->AddChecked(IntrinsicName), SQualifier::NominalType(_Program._VerseModule), _CurrentScope);
    }

    // Temporary helper function for identifying definitions that are EpicInternal but are required
    // to appear in the digest for user code to see.
    bool IsEpicInternalSpecialException(const CDefinition& Definition) const
    {
        if (Definition.DerivedAccessLevel()._Kind != SAccessLevel::EKind::EpicInternal)
        {
            return false;
        }

        bool Result = false;

        // localization
        {
            const CUTF8String Path = GetQualifiedNameString(Definition);
            Result |= Path == "(/Verse.org/Verse:)MakeMessageInternal";
            Result |= Path == "(/Verse.org/Verse:)MakeLocalizableValue";
            Result |= Path == "(/Verse.org/Simulation:)MakeLocalizableValue";
            Result |= Path == "(/Verse.org/Verse:)LocalizeValue";
            // TODO: (yiliang.siew) This is a HACK since `MakeLocalizableValue` references `llm_string`, which is
            // `<epic_internal>` and thus needs to be exposed in the digests.
            Result |= Path == "(/Verse.org/Verse:)llm_string";
            Result |= Path.ToStringView().StartsWith("(/Verse.org/Verse:)localizable_");
        }

        // <predicts>
        {
            const CUTF8String Path = GetQualifiedNameString(Definition);
            Result |= Path == "(/Verse.org/Predicts:)PredictsServerRegisterObject";
            Result |= Path == "(/Verse.org/Predicts:)PredictsIsRunningOnServer";
        }

        return Result;
    }

    CSemanticProgram& _Program;
    const CAstPackage& _Package;
    TSRef<CDiagnostics> _Diagnostics;
    bool _bIncludeInternalDefinitions;
    bool _bIncludeEpicInternalDefinitions;
    mutable TArray<CUTF8String> _Usings;
    mutable TSet<const CAstPackage*> _DependencyPackages;
    mutable const CModule* _CurrentModule;
    mutable const CScope* _CurrentScope;
    mutable const CAstNode* _CurrentGlitchAst;
    CSymbol _Underscore;
    const CUTF8String* _Notes;
};

//====================================================================================
// Public API
//====================================================================================

namespace DigestGenerator
{

bool Generate(
    CSemanticProgram& Program, 
    const CAstPackage& Package,
    bool bIncludeInternalDefinitions,
    bool bIncludeEpicInternalDefinitions,
    const TSRef<CDiagnostics>& Diagnostics,
    const CUTF8String* Notes,
    CUTF8String& OutDigestCode,
    TArray<const CAstPackage*>& OutDigestPackageDependencies)
{
    CDigestGeneratorImpl Generator(Program, Package, Diagnostics, Notes, bIncludeInternalDefinitions, bIncludeEpicInternalDefinitions);
    return Generator.Generate(OutDigestCode, OutDigestPackageDependencies);
}

} // namespace DigestGenerator

} // namespace uLang
