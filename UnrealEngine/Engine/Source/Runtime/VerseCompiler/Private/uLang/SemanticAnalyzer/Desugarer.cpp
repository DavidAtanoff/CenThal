// Copyright Epic Games, Inc. All Rights Reserved.

#include "Desugarer.h"

#include "uLang/Common/Containers/SharedPointer.h"
#include "uLang/Common/Containers/UniquePointer.h"
#include "uLang/Diagnostics/Glitch.h"
#include "uLang/Parser/ParserPass.h"
#include "uLang/Semantics/Expression.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/UnknownType.h"
#include "uLang/SourceProject/UploadedAtFNVersion.h"
#include "uLang/SourceProject/VerseVersion.h"
#include "uLang/Syntax/VstNode.h"
#include "uLang/Syntax/vsyntax_types.h"

namespace
{
namespace Vst = Verse::Vst;

class CDesugarerImpl
{
public:

    CDesugarerImpl(uLang::CSymbolTable& Symbols, uLang::CDiagnostics& Diagnostics): _Symbols(Symbols), _Diagnostics(Diagnostics) {}

    uLang::TSRef<uLang::CAstProject> DesugarProject(const Vst::Project& VstProject)
    {
        // Desugar all the project's packages (and build vertex array for Tarjan's algorithm).
        struct SPackageVertex
        {
            uLang::TSRef<uLang::CAstPackage> Package;
            uLang::TArray<int32_t> Dependencies; // Indices of vertices this package depends on
            int32_t DepthIndex = uLang::IndexNone;
            int32_t LowLink = uLang::IndexNone;
            bool bOnStack = false;            
        };
        uLang::TArray<SPackageVertex> Vertices;
        Vertices.Reserve(VstProject.GetChildCount());
        for (const Vst::TNodeRef<Vst::Node>& VstPackage : VstProject.GetChildren())
        {
            ULANG_ASSERTF(VstPackage->GetElementType() == Vst::NodeType::Package, "Toolchain must ensure that a project only ever contains packages.");
            Vertices.Add({ DesugarPackage(static_cast<const Vst::Package&>(*VstPackage)), {} });
        }

        // Populate the dependencies for both the Tarjan vertices and the AST packages
        for (SPackageVertex& Vertex : Vertices)
        {
            const Vst::Package* VstPackage = static_cast<const Vst::Package*>(Vertex.Package->GetMappedVstNode());
            if (ULANG_ENSUREF(VstPackage && VstPackage->GetElementType() == Vst::NodeType::Package, "Node should have been properly mapped by DesugarPackage."))
            {
                Vertex.Dependencies.Reserve(VstPackage->_DependencyPackages.Num());
                Vertex.Package->_Dependencies.Reserve(VstPackage->_DependencyPackages.Num());

                for (const uLang::CUTF8String& DependencyName : VstPackage->_DependencyPackages)
                {
                    int32_t DependencyIndex = Vertices.IndexOfByPredicate([&DependencyName](const SPackageVertex& DependencyVertex) { return DependencyVertex.Package->_Name == DependencyName; });
                    if (DependencyIndex != uLang::IndexNone)
                    {
                        Vertex.Dependencies.Add(DependencyIndex);
                        Vertex.Package->_Dependencies.Add(Vertices[DependencyIndex].Package);
                    }
                    else if (Vertex.Package->_Role != uLang::ConstraintPackageRole)
                    {
                        AppendGlitch(VstPackage, uLang::EDiagnostic::ErrSemantic_UnknownPackageDependency,
                            uLang::CUTF8String("Package `%s` specifies dependency `%s` which does not exist.", *Vertex.Package->_Name, *DependencyName));
                    }
                }
            }
        }

        // Prepare new AST project
        uLang::TSRef<uLang::CAstProject> AstProject = uLang::TSRef<uLang::CAstProject>::New(VstProject._Name);
        AstProject->ReserveCompilationUnits(VstProject.GetChildCount());

        // Run Tarjan's algorithm to generate the compilation units (SCCs)
        uLang::TArray<int32_t> Stack;
        Stack.Reserve(Vertices.Num());
        int32_t CurrentDepthIndex = 0;
        auto StrongConnect = [&AstProject, &Vertices, &Stack, &CurrentDepthIndex](int32_t V, const auto& StrongConnectRef) -> void
            {
                // Set the depth index for V to the smallest unused index
                SPackageVertex& Vertex = Vertices[V];
                Vertex.DepthIndex = Vertex.LowLink = CurrentDepthIndex++;
                Stack.Push(V);
                Vertex.bOnStack = true;

                // Consider dependencies of V
                for (int32_t W : Vertex.Dependencies)
                {
                    SPackageVertex& DependencyVertex = Vertices[W];
                    if (DependencyVertex.DepthIndex == uLang::IndexNone)
                    {
                        // Dependency W has not yet been visited - recurse on it
                        StrongConnectRef(W, StrongConnectRef);
                        Vertex.LowLink = uLang::CMath::Min(Vertex.LowLink, DependencyVertex.LowLink);
                    }
                    else if (DependencyVertex.bOnStack)
                    {
                        // Dependency W is in stack and hence in the current SCC
                        // If W is not on stack, then (V, W) is an edge pointing to an SCC already found and must be ignored
                        // The next line may look odd - but is correct.
                        // It says DependencyVertex.DepthIndex not DependencyVertex.LowLink - that is deliberate and from the original paper
                        Vertex.LowLink = uLang::CMath::Min(Vertex.LowLink, DependencyVertex.DepthIndex);
                    }
                }

                // If V is a root node, pop the stack and generate an SCC
                if (Vertex.LowLink == Vertex.DepthIndex)
                {
                    // Since Tarjan's algorithm does a depth-first search, compilation units
                    // will be generated in depth-first order which is the order we desire
                    // therefore no explicit sorting of compilation units is required after this algorithm is done
                    uLang::TSRef<uLang::CAstCompilationUnit> CompilationUnit = uLang::TSRef<uLang::CAstCompilationUnit>::New();
                    int32_t W;
                    do
                    {
                        W = Stack.Pop();
                        SPackageVertex& SCCVertex = Vertices[W];
                        SCCVertex.bOnStack = false;
                        SCCVertex.Package->_CompilationUnit = CompilationUnit;
                        CompilationUnit->AppendPackage(uLang::Move(SCCVertex.Package));
                    } while (W != V);
                    AstProject->AppendCompilationUnit(uLang::Move(CompilationUnit));
                }
            };
        for (int32_t Index = 0; Index < Vertices.Num(); ++Index)
        {
            if (Vertices[Index].DepthIndex == uLang::IndexNone)
            {
                StrongConnect(Index, StrongConnect);
            }
        }

        VstProject.AddMapping(&*AstProject);
        return uLang::Move(AstProject);
    }

private:

    uLang::TSRef<uLang::CAstPackage> DesugarPackage(const Vst::Package& VstPackage)
    {
        // Turn the language version override into an effective version.
        uint32_t EffectiveVerseVersion = VstPackage._VerseVersion.Get(Verse::Version::Default);
        if (EffectiveVerseVersion < Verse::Version::Minimum
            || EffectiveVerseVersion > Verse::Version::Maximum)
        {
            AppendGlitch(
                &VstPackage,
                uLang::EDiagnostic::ErrSystem_InvalidVerseVersion,
                uLang::CUTF8String("Invalid Verse version for package %s: %u", VstPackage._Name.AsCString(), EffectiveVerseVersion));
        }

        uLang::TSRef<uLang::CAstPackage> AstPackage = uLang::TSRef<uLang::CAstPackage>::New(
            VstPackage._Name,
            VstPackage._VersePath,
            VstPackage._VerseScope,
            VstPackage._Role,
            EffectiveVerseVersion,
            VstPackage._UploadedAtFNVersion,
            VstPackage._VniDestDir.IsSet(),
            VstPackage._bTreatModulesAsImplicit,
            VstPackage._bAllowExperimental);

        uLang::TGuardValue<uLang::CAstPackage*> CurrentPackageGuard(_Package, AstPackage.Get());

        // Desugar all the package's modules or snippets.
        for (const Vst::TNodeRef<Vst::Node>& VstNode : VstPackage.GetChildren())
        {
            if (VstNode->GetElementType() == Vst::NodeType::Module)
            {
                AstPackage->AppendMember(DesugarModule(static_cast<const Vst::Module&>(*VstNode)));
            }
            else if (VstNode->GetElementType() == Vst::NodeType::Snippet)
            {
                AstPackage->AppendMember(DesugarSnippet(static_cast<const Vst::Snippet&>(*VstNode)));
            }
            else
            {
                ULANG_ERRORF("Toolchain must ensure that a package only ever contains modules or snippets.");
            }
        }
        
        VstPackage.AddMapping(&*AstPackage);
        return uLang::Move(AstPackage);
    }
    
    uLang::TSRef<uLang::CExprModuleDefinition> DesugarModule(const Vst::Module& VstModule)
    {
        uLang::TSRef<uLang::CExprModuleDefinition> AstModule = uLang::TSRef<uLang::CExprModuleDefinition>::New(VstModule._Name);

        // Is a vmodule file present?
        if (VstModule._FilePath.ToStringView().EndsWith(".vmodule"))
        {
            // Yes - mark public to mimic legacy behavior of vmodule files
            AstModule->_bLegacyPublic = true;
        }

        // Desugar the module's children, which may be either submodules or snippets.
        for (const Vst::TNodeRef<Vst::Node>& VstNode : VstModule.GetChildren())
        {
            if (VstNode->GetElementType() == Vst::NodeType::Module)
            {
                AstModule->AppendMember(DesugarModule(static_cast<const Vst::Module&>(*VstNode)));
            }
            else if (VstNode->GetElementType() == Vst::NodeType::Snippet)
            {
                AstModule->AppendMember(DesugarSnippet(static_cast<const Vst::Snippet&>(*VstNode)));
            }
            else
            {
                ULANG_ENSUREF(false, "Toolchain must ensure that a module only ever contains modules or snippets.");
            }
        }
        
        VstModule.AddMapping(&*AstModule);
        return uLang::Move(AstModule);
    }
    
    uLang::TSRef<uLang::CExprSnippet> DesugarSnippet(const Vst::Snippet& VstSnippet)
    {
        uLang::TSRef<uLang::CExprSnippet> AstSnippet = uLang::TSRef<uLang::CExprSnippet>::New(VstSnippet._Path);

        // Desugar all the snippet's top-level expressions.
        for (const Vst::TNodeRef<Vst::Node>& VstNode : VstSnippet.GetChildren())
        {
            if (!VstNode->IsA<Vst::Comment>())
            {
                AstSnippet->AppendMember(DesugarExpressionVst(*VstNode));
            }
        }
        
        VstSnippet.AddMapping(&*AstSnippet);
        return uLang::Move(AstSnippet);
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarClauseAsExpression(const Vst::Node& MaybeClauseVst)
    {
        if (MaybeClauseVst.GetElementType() != Verse::Vst::NodeType::Clause)
        {
            // If the expression isn't a clause, just desugar it directly.
            return DesugarExpressionVst(MaybeClauseVst);
        }
        else
        {
            const Verse::Vst::Clause& ClauseVst = MaybeClauseVst.As<Verse::Vst::Clause>();

            // Determine if the clause has a single non-comment child expression.
            const Vst::Node* NonCommentChild = nullptr;
            for (const Vst::Node* ChildVst : ClauseVst.GetChildren())
            {
                if (!ChildVst->IsA<Vst::Comment>())
                {
                    if (NonCommentChild == nullptr)
                    {
                        NonCommentChild = ChildVst;
                    }
                    else
                    {
                        NonCommentChild = nullptr;
                        break;
                    }
                }
            }

            if (NonCommentChild)
            {
                // If so, desugar that expression as though it occurred on its own.
                return DesugarExpressionVst(*NonCommentChild);
            }
            else
            {
                // Otherwise, desugar the clause as a code block.
                return DesugarClauseAsCodeBlock(ClauseVst);
            }
        }
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarWhere(const Vst::Where& WhereVst)
    {
        uLang::TSRef<uLang::CExpressionBase> LhsAst = DesugarExpressionVst(*WhereVst.GetLhs());
        Vst::Where::RhsView RhsVstView = WhereVst.GetRhs();
        uLang::TSPtrArray<uLang::CExpressionBase> RhsAst;
        RhsAst.Reserve(RhsVstView.Num());
        for (const Vst::TNodeRef<Vst::Node>& RhsVst : RhsVstView)
        {
            RhsAst.Add(DesugarExpressionVst(*RhsVst));
        }
        return AddMapping(WhereVst, uLang::TSRef<uLang::CExprWhere>::New(uLang::Move(LhsAst), uLang::Move(RhsAst)));
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarMutation(const Vst::Mutation& MutationVst)
    {
        switch (MutationVst._Keyword)
        {
        case Vst::Mutation::EKeyword::Var:
            return AddMapping(MutationVst, uLang::TSRef<uLang::CExprVar>::New(MutationVst._bLive, DesugarExpressionVst(*MutationVst.Child())));
        case Vst::Mutation::EKeyword::Set:
            return AddMapping(MutationVst, uLang::TSRef<uLang::CExprSet>::New(MutationVst._bLive, DesugarExpressionVst(*MutationVst.Child())));
        case Vst::Mutation::EKeyword::Live:
            return AddMapping(MutationVst, uLang::TSRef<uLang::CExprLive>::New(DesugarExpressionVst(*MutationVst.Child())));
        default:
            ULANG_UNREACHABLE();
        }
    }

    // This is old code that can't handle named parameters.
    // Only kept to compile code published before 20.30, since this code ignores some errors that the new code complains about.

    struct SNameTypeIdentifierPair
    {
        const Vst::Identifier* Name;
        const Vst::Identifier* Type;
    };

    uLang::TSRef<uLang::CExpressionBase> DesugarLocalizableOld(
        const Vst::Definition& DefinitionVst,
        const Vst::Identifier& MessageKeyVst,
        const uLang::CUTF8String& MessageDefaultText,
        const Vst::TNodeRef<Vst::Node>& MessageTypeVst,
        const uLang::TArray<SNameTypeIdentifierPair>& NameTypePairs,
        bool bIsFunction)
    {
        const uLang::CSymbol MessageKeySymbol = VerifyAddSymbol(&MessageKeyVst, MessageKeyVst._OriginalCode);

        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> MapClauseExprs;

        const uLang::CSymbol MakeLocalizableSymbol = _Symbols.AddChecked("MakeLocalizableValue");

        uLang::TSRef<uLang::CExprIdentifierUnresolved> MakeLocalizableIdentifier = uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(MakeLocalizableSymbol);
        MakeLocalizableIdentifier->SetNonReciprocalMappedVstNode(&DefinitionVst);

        // ** special exception here to allow looking up this identifier which is <epic_internal> to 
        // another module, we're doing this as a short term protection against users writing 
        // code that depends on internal details of the message type
        MakeLocalizableIdentifier->GrantUnrestrictedAccess();

        auto CreateArgDefinition = [this, &MapClauseExprs, &DefinitionVst, &MakeLocalizableIdentifier](const SNameTypeIdentifierPair& NameTypePair) -> uLang::TSRef<uLang::CExprDefinition>
        {
            const uLang::CSymbol NameSymbol = VerifyAddSymbol(NameTypePair.Name, NameTypePair.Name->_OriginalCode);
            const uLang::CSymbol TypeSymbol = VerifyAddSymbol(NameTypePair.Type, NameTypePair.Type->_OriginalCode);

            uLang::TSRef<uLang::CExpressionBase> NameIdentifier = AddMapping(*NameTypePair.Name, uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(NameSymbol));
            uLang::TSRef<uLang::CExpressionBase> TypeIdentifier = AddMapping(*NameTypePair.Type, uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(TypeSymbol));

            // create the function parameter definition
            uLang::TSRef<uLang::CExprDefinition> ArgDefinition =
                uLang::TSRef<uLang::CExprDefinition>::New(
                    NameIdentifier,
                    TypeIdentifier,
                    nullptr);
            ArgDefinition->SetNonReciprocalMappedVstNode(&DefinitionVst);

            // create an invocation passing the argument to MakeLocalizableValue
            // so it can be added to the Substitutions map
            uLang::TSRef<uLang::CExprInvocation> MakeLocalizableValueInvocation =
                uLang::TSRef<uLang::CExprInvocation>::New(
                    uLang::CExprInvocation::EBracketingStyle::Parentheses,
                    MakeLocalizableIdentifier,
                    NameIdentifier);
            MakeLocalizableValueInvocation->SetNonReciprocalMappedVstNode(&DefinitionVst);

            uLang::TSRef<uLang::CExprString> ArgNameString = uLang::TSRef<uLang::CExprString>::New(NameTypePair.Name->_OriginalCode);
            ArgNameString->SetNonReciprocalMappedVstNode(NameTypePair.Name);

            uLang::TSRef<uLang::CExprFunctionLiteral> MapClauseExpr =
                uLang::TSRef<uLang::CExprFunctionLiteral>::New(
                    uLang::Move(ArgNameString),
                    MakeLocalizableValueInvocation);
            MapClauseExpr->SetNonReciprocalMappedVstNode(&DefinitionVst);
            MapClauseExprs.Add(uLang::Move(MapClauseExpr));

            return ArgDefinition;
        };

        // in the function case, we have to differentiate between a single
        // parameter function and a multi-parameter function where for
        // multi-parameter functions the AST is expected to have the
        // list of definitions wrapped by a uLang::CExprMakeTuple node, but
        // single parameter functions should not be wrapped
        uLang::TSPtr<uLang::CExpressionBase> ElementArguments;

        if (NameTypePairs.Num() == 1)
        {
            ElementArguments = CreateArgDefinition(NameTypePairs[0]);
        }
        else
        {
            // multi-parameter functions need to wrap the 
            // definitions in a uLang::CExprMakeTuple node
            uLang::TSRef<uLang::CExprMakeTuple> ElementArgumentsTuple = uLang::TSRef<uLang::CExprMakeTuple>::New();
            ElementArgumentsTuple->SetNonReciprocalMappedVstNode(&DefinitionVst);

            for (const SNameTypeIdentifierPair& NameTypePair : NameTypePairs)
            {
                ElementArgumentsTuple->AppendSubExpr(CreateArgDefinition(NameTypePair));
            }

            ElementArguments = uLang::Move(ElementArgumentsTuple);
        }

        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> ArgumentExprs;

        {
            // Key argument

            // for the function case, the current scope will include the function name, so we pass the null symbol here
            uLang::TSRef<uLang::CExprPathPlusSymbol> KeyPath = uLang::TSRef<uLang::CExprPathPlusSymbol>::New(bIsFunction ? uLang::CSymbol() : MessageKeySymbol);
            KeyPath->SetNonReciprocalMappedVstNode(&MessageKeyVst);

            ArgumentExprs.Add(uLang::Move(KeyPath));
        }

        {
            // DefaultText argument
            uLang::TSRef<uLang::CExpressionBase> DefaultTextString = uLang::TSRef<uLang::CExprString>::New(MessageDefaultText);

            ArgumentExprs.Add(uLang::Move(DefaultTextString));
        }

        {
            // Substitutions argument
            const uLang::CSymbol MapSymbol = _Symbols.AddChecked("map");
            uLang::TSRef<uLang::CExprIdentifierUnresolved> MapIdentifier = uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(MapSymbol);
            MapIdentifier->SetNonReciprocalMappedVstNode(&DefinitionVst);
            uLang::TSRef<uLang::CExprMacroCall> MapMacroExpr = uLang::TSRef<uLang::CExprMacroCall>::New(MapIdentifier);
            MapMacroExpr->SetNonReciprocalMappedVstNode(&DefinitionVst);
            MapMacroExpr->AppendClause(uLang::CExprMacroCall::CClause(uLang::EMacroClauseTag::None, Verse::Vst::Clause::EForm::Synthetic, uLang::Move(MapClauseExprs)));

            ArgumentExprs.Add(uLang::Move(MapMacroExpr));
        }

        uLang::TSRef<uLang::CExprMakeTuple> ArgTuple = WrapExpressionListInTuple(uLang::Move(ArgumentExprs), DefinitionVst, false);

        const uLang::CSymbol MakeMessageSymbol = _Symbols.AddChecked("MakeMessageInternal");
        uLang::TSRef<uLang::CExprIdentifierUnresolved> MakeMessageIdentifier = uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(MakeMessageSymbol);

        // ** special exception here to allow looking up this identifier which is <epic_internal> to 
        // another module, we're doing this as a short term protection against users writing 
        // code that depends on internal details of the message type
        MakeMessageIdentifier->GrantUnrestrictedAccess();

        MakeMessageIdentifier->SetNonReciprocalMappedVstNode(&DefinitionVst);

        uLang::TSRef<uLang::CExprInvocation> MakeMessageInvocation =
            uLang::TSRef<uLang::CExprInvocation>::New(
                uLang::CExprInvocation::EBracketingStyle::Parentheses,
                uLang::Move(MakeMessageIdentifier),
                uLang::Move(ArgTuple));
        MakeMessageInvocation->SetNonReciprocalMappedVstNode(&DefinitionVst);

        // We explicitly desugar the identifier because the localizable definition may have an explicit qualifer.
        // i.e. `(super_class:)MyMessage<localizes><override>:message="B"`
        uLang::TSRef<uLang::CExpressionBase> MessageKeyIdentifier = DesugarIdentifier(MessageKeyVst);

        if (MessageKeyVst.HasAttributes())
        {
            MessageKeyIdentifier->_Attributes = DesugarAttributes(MessageKeyVst.GetAux()->GetChildren());
        }

        uLang::TSPtr<uLang::CExpressionBase> DefinitionElement;

        if (bIsFunction)
        {
            uLang::TSRef<uLang::CExprInvocation> ElementInvocation =
                uLang::TSRef<uLang::CExprInvocation>::New(
                    uLang::CExprInvocation::EBracketingStyle::Parentheses,
                    uLang::Move(MessageKeyIdentifier),
                    uLang::Move(ElementArguments.AsRef()));
            ElementInvocation->SetNonReciprocalMappedVstNode(&DefinitionVst);
            DefinitionElement = ElementInvocation;
        }
        else
        {
            DefinitionElement = MessageKeyIdentifier;
        }

        uLang::TSRef<uLang::CExprDefinition> Definition = uLang::TSRef<uLang::CExprDefinition>::New(DefinitionElement.AsRef(), DesugarExpressionVst(*MessageTypeVst), uLang::Move(MakeMessageInvocation));
        return Definition;
    }

    // This is the new code that improves localization, see SOL-6057

    void FillinClauseExprs(
        const Vst::Definition& DefinitionVst,
        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>>& MapClauseExprs,
        const uLang::TArray<uLang::TSRef<uLang::CExpressionBase>>& Parameters)
    {
        const uLang::CSymbol MakeLocalizableSymbol = _Symbols.AddChecked("MakeLocalizableValue");
        uLang::TSRef<uLang::CExprIdentifierUnresolved> MakeLocalizableIdentifier = uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(MakeLocalizableSymbol);
        MakeLocalizableIdentifier->SetNonReciprocalMappedVstNode(&DefinitionVst);

        // ** special exception here to allow looking up this identifier which is <epic_internal> to 
        // another module, we're doing this as a short term protection against users writing 
        // code that depends on internal details of the message type
        MakeLocalizableIdentifier->GrantUnrestrictedAccess();

        for (const uLang::TSRef<uLang::CExpressionBase>& Parameter : Parameters)
        {
            if (Parameter->GetNodeType() == uLang::EAstNodeType::Definition)
            {
                uLang::TSPtr<uLang::CExprDefinition> ParamDefinition = Parameter.As<uLang::CExprDefinition>();
                uLang::TSPtr<uLang::CExpressionBase> Element = ParamDefinition->Element();
                if (!Element || Element->GetNodeType() != uLang::EAstNodeType::Identifier_Unresolved)
                {
                    continue;
                }

                uLang::TSPtr<uLang::CExprIdentifierUnresolved> ParamUnresolved = Element.As<uLang::CExprIdentifierUnresolved>();
                uLang::CSymbol NameSymbol = ParamUnresolved->_Symbol;
                const Vst::Node* NameVstNode = ParamUnresolved->GetMappedVstNode();

                uLang::TSRef<uLang::CExprIdentifierUnresolved> NameIdentifier = uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(NameSymbol);
                NameIdentifier->SetNonReciprocalMappedVstNode(ParamUnresolved->GetMappedVstNode());

                // create an invocation passing the argument to MakeLocalizableValue
                // so it can be added to the Substitutions map
                uLang::TSRef<uLang::CExprInvocation> MakeLocalizableValueInvocation =
                    uLang::TSRef<uLang::CExprInvocation>::New(
                        uLang::CExprInvocation::EBracketingStyle::Parentheses,
                        MakeLocalizableIdentifier,
                        NameIdentifier);
                MakeLocalizableValueInvocation->SetNonReciprocalMappedVstNode(NameVstNode);

                uLang::TSRef<uLang::CExprString> ArgNameString = uLang::TSRef<uLang::CExprString>::New(NameSymbol.AsString());
                ArgNameString->SetNonReciprocalMappedVstNode(NameVstNode);

                uLang::TSRef<uLang::CExprFunctionLiteral> MapClauseExpr =
                    uLang::TSRef<uLang::CExprFunctionLiteral>::New(
                        uLang::Move(ArgNameString),
                        MakeLocalizableValueInvocation);
                MapClauseExpr->SetNonReciprocalMappedVstNode(&DefinitionVst);
                MapClauseExprs.Add(uLang::Move(MapClauseExpr));
            }
            else if (Parameter->GetNodeType() == uLang::EAstNodeType::Invoke_MakeTuple)
            {
                uLang::TSPtr<uLang::CExprMakeTuple> ParamTuple = Parameter.As<uLang::CExprMakeTuple>();
                FillinClauseExprs(DefinitionVst, MapClauseExprs, ParamTuple->GetSubExprs());
            }
        }
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarLocalizable(
        const Vst::Definition& DefinitionVst,
        const Vst::Identifier& MessageKeyVst,
        const uLang::CUTF8String& MessageDefaultText,
        const Vst::TNodeRef<Vst::Node>& MessageTypeVst,
        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>>& Parameters,
        bool bIsFunction)
    {
        const uLang::CSymbol MessageKeySymbol = VerifyAddSymbol(&MessageKeyVst, MessageKeyVst._OriginalCode);

        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> MapClauseExprs;
        FillinClauseExprs(DefinitionVst, MapClauseExprs, Parameters);

        uLang::TSPtr<uLang::CExpressionBase> ElementParameters;

        if (Parameters.Num() == 1)
        {
            ElementParameters = Parameters[0];
        }
        else
        {
            // multi-parameter functions need to wrap the 
            // definitions in a uLang::CExprMakeTuple node
            uLang::TSRef<uLang::CExprMakeTuple> ElementParametersTuple = uLang::TSRef<uLang::CExprMakeTuple>::New();
            ElementParametersTuple->SetNonReciprocalMappedVstNode(&DefinitionVst);

            for (const uLang::TSRef<uLang::CExpressionBase>& Parameter : Parameters)
            {
                ElementParametersTuple->AppendSubExpr(Parameter);
            }

            ElementParameters = uLang::Move(ElementParametersTuple);
        }

        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> ArgumentExprs;

        {
            // Key argument

            // for the function case, the current scope will include the function name, so we pass the null symbol here
            uLang::TSRef<uLang::CExprPathPlusSymbol> KeyPath = uLang::TSRef<uLang::CExprPathPlusSymbol>::New(bIsFunction ? uLang::CSymbol() : MessageKeySymbol);
            KeyPath->SetNonReciprocalMappedVstNode(&MessageKeyVst);

            ArgumentExprs.Add(uLang::Move(KeyPath));
        }

        {
            // DefaultText argument
            uLang::TSRef<uLang::CExpressionBase> DefaultTextString = uLang::TSRef<uLang::CExprString>::New(MessageDefaultText);
            DefaultTextString->SetNonReciprocalMappedVstNode(&DefinitionVst);
            ArgumentExprs.Add(uLang::Move(DefaultTextString));
        }

        {
            // Substitutions argument
            const uLang::CSymbol MapSymbol = _Symbols.AddChecked("map");
            uLang::TSRef<uLang::CExprIdentifierUnresolved> MapIdentifier = uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(MapSymbol);
            MapIdentifier->SetNonReciprocalMappedVstNode(&DefinitionVst);
            uLang::TSRef<uLang::CExprMacroCall> MapMacroExpr = uLang::TSRef<uLang::CExprMacroCall>::New(MapIdentifier);
            MapMacroExpr->SetNonReciprocalMappedVstNode(&DefinitionVst);
            MapMacroExpr->AppendClause(uLang::CExprMacroCall::CClause(uLang::EMacroClauseTag::None, Verse::Vst::Clause::EForm::Synthetic, uLang::Move(MapClauseExprs)));

            ArgumentExprs.Add(uLang::Move(MapMacroExpr));
        }

        uLang::TSRef<uLang::CExprMakeTuple> ArgTuple = WrapExpressionListInTuple(uLang::Move(ArgumentExprs), DefinitionVst, false);

        const uLang::CSymbol MakeMessageSymbol = _Symbols.AddChecked("MakeMessageInternal");
        uLang::TSRef<uLang::CExprIdentifierUnresolved> MakeMessageIdentifier = uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(MakeMessageSymbol);
        
        // ** special exception here to allow looking up this identifier which is <epic_internal> to 
        // another module, we're doing this as a short term protection against users writing 
        // code that depends on internal details of the message type
        MakeMessageIdentifier->GrantUnrestrictedAccess();

        MakeMessageIdentifier->SetNonReciprocalMappedVstNode(&DefinitionVst);

        uLang::TSRef<uLang::CExprInvocation> MakeMessageInvocation = 
            uLang::TSRef<uLang::CExprInvocation>::New(
                uLang::CExprInvocation::EBracketingStyle::Parentheses,
                uLang::Move(MakeMessageIdentifier),
                uLang::Move(ArgTuple));
        MakeMessageInvocation->SetNonReciprocalMappedVstNode(&DefinitionVst);

        // We explicitly desugar the identifier because the localizable definition may have an explicit qualifer.
        // i.e. `(super_class:)MyMessage<localizes><override>:message="B"`
        uLang::TSRef<uLang::CExpressionBase> MessageKeyIdentifier = DesugarIdentifier(MessageKeyVst);

        if (MessageKeyVst.HasAttributes())
        {
            MessageKeyIdentifier->_Attributes = DesugarAttributes(MessageKeyVst.GetAux()->GetChildren());
        }

        uLang::TSPtr<uLang::CExpressionBase> DefinitionElement;

        if (bIsFunction)
        {
            uLang::TSRef<uLang::CExprInvocation> ElementInvocation =
                uLang::TSRef<uLang::CExprInvocation>::New(
                    uLang::CExprInvocation::EBracketingStyle::Parentheses,
                    uLang::Move(MessageKeyIdentifier),
                    uLang::Move(ElementParameters.AsRef()));
            ElementInvocation->SetNonReciprocalMappedVstNode(&DefinitionVst);
            DefinitionElement = ElementInvocation;
        }
        else
        {
            DefinitionElement = MessageKeyIdentifier;
        }

        uLang::TSRef<uLang::CExprDefinition> Definition = uLang::TSRef<uLang::CExprDefinition>::New(DefinitionElement.AsRef(), DesugarExpressionVst(*MessageTypeVst), uLang::Move(MakeMessageInvocation));
        return Definition;
    }

    // Common code for new and old code for localize.
    // Selects new or old behaviour depending on UploadedAtFNVesrsion.

    uLang::TSPtr<uLang::CExpressionBase> TryDesugarLocalizable(const Vst::Definition& DefinitionVst)
    {
        Vst::TNodeRef<Vst::Node> LhsVst = DefinitionVst.GetOperandLeft();

        bool bEnableNamedParametersForLocalize = VerseFN::UploadedAtFNVersion::EnableNamedParametersForLocalize(_Package->_UploadedAtFNVersion);

        //
        // there are several valid forms for localized definitions
        // this list is mirrored in Localization.versetest
        //
        // 1) TheMsg<localizes> := "The Message"
        // 2) TheMsg<localizes> : message = "The Message"
        // 3) TheMsg<localizes>(Name:string) := "The Message"
        // 4) TheMsg<localizes>(Name:string) : message = "The Message"
        // 5) TheMsg<localizes>(Name:string) := "The Message to {Name}"
        // 6) TheMsg<localizes>(Name:string) : message = "The Message to {Name}"
        //
        // NOTE that currently we do not support any of the forms(1, 3, 5) that omit the type name,
        // but we still parse every form in order to give better error messages here
        //

        const Vst::Identifier* MaybeLocalizedIdentifier = nullptr;
        Vst::TNodePtr<Vst::Node> MaybeLocalizedType;
        const Vst::Clause* MaybeLocalizedArgs = nullptr;

        if (const Vst::Identifier* LhsIdentifier = LhsVst->AsNullable<Vst::Identifier>())
        {
            // this is only hit for case 1 where the identifier is the Lhs of the definition
            MaybeLocalizedIdentifier = LhsIdentifier;
        }
        else if (const Vst::TypeSpec* LhsTypeSpec = LhsVst->AsNullable<Vst::TypeSpec>())
        {
            // this is hit for case 2, 4, and 6, where the user explicitly stated a type

            if (LhsTypeSpec->HasLhs())
            {
                const Vst::Node& LhsNode = *LhsTypeSpec->GetLhs();

                // case 2?
                MaybeLocalizedIdentifier = LhsNode.AsNullable<Vst::Identifier>();

                if (!MaybeLocalizedIdentifier)
                {
                    const Vst::PrePostCall* PrePostCallNode = LhsNode.AsNullable<Vst::PrePostCall>();

                    if (PrePostCallNode && (PrePostCallNode->GetChildCount() >= 2))
                    {
                        Vst::TNodeRef<Vst::Node> PrePostCallFirstChild = PrePostCallNode->GetChildren()[0];
                        Vst::TNodeRef<Vst::Node> PrePostCallSecondChild = PrePostCallNode->GetChildren()[1];

                        // this is case 4 and 6
                        MaybeLocalizedIdentifier = PrePostCallFirstChild->AsNullable<Vst::Identifier>();
                        MaybeLocalizedArgs = PrePostCallSecondChild->AsNullable<Vst::Clause>();
                    }
                }
            }

            MaybeLocalizedType = LhsTypeSpec->GetRhs();
        }
        else if (const Vst::PrePostCall* PrePostCallNode = LhsVst->AsNullable<Vst::PrePostCall>())
        {
            // this is hit for case 3 and 5
            if (PrePostCallNode->GetChildCount() >= 2)
            {
                Vst::TNodeRef<Vst::Node> PrePostCallFirstChild = PrePostCallNode->GetChildren()[0];
                Vst::TNodeRef<Vst::Node> PrePostCallSecondChild = PrePostCallNode->GetChildren()[1];

                // this is case 3 and 5
                MaybeLocalizedIdentifier = PrePostCallFirstChild->AsNullable<Vst::Identifier>();
                MaybeLocalizedArgs = PrePostCallSecondChild->AsNullable<Vst::Clause>();
            }
        }

        if (MaybeLocalizedIdentifier != nullptr)
        {
            uLang::TArray<SNameTypeIdentifierPair> LocalizedArgumentNameTypePairs;
            if (!bEnableNamedParametersForLocalize)
            {
                if (MaybeLocalizedArgs)
                {
                    // this collects the pairs of function parameter name and type, (Subject,string) and (Rank,int) in the above example
                    for (const Vst::TNodeRef<Vst::Node>& ArgNode : MaybeLocalizedArgs->GetChildren())
                    {
                        if (ArgNode->GetElementType() == Vst::NodeType::TypeSpec)
                        {
                            const Vst::TypeSpec& ArgTypeSpecNode = ArgNode->As<Vst::TypeSpec>();

                            if (ArgTypeSpecNode.HasLhs())
                            {
                                const Vst::Identifier* ArgNameIdentifier = ArgTypeSpecNode.GetLhs()->AsNullable<Vst::Identifier>();
                                const Vst::Identifier* ArgTypeIdentifier = ArgTypeSpecNode.GetRhs()->AsNullable<Vst::Identifier>();

                                if (ArgNameIdentifier && ArgTypeIdentifier)
                                {
                                    LocalizedArgumentNameTypePairs.Add(SNameTypeIdentifierPair{ ArgNameIdentifier, ArgTypeIdentifier });
                                }
                            }
                        }
                    }
                }
            }

            // does this identifier have a 'localizes' attribute attached?
            if (MaybeLocalizedIdentifier->IsAttributePresent("localizes"))
            {
                // first ensure that they've specified a type (we don't currently support omitting the type)
                if (MaybeLocalizedType == nullptr)
                {
                    AppendGlitch(&DefinitionVst, uLang::EDiagnostic::ErrSemantic_LocalizesMustSpecifyType);
                    return AddMapping(DefinitionVst, uLang::TSRef<uLang::CExprError>::New());
                }

                uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> Arguments;
                if (bEnableNamedParametersForLocalize)
                {
                    if (MaybeLocalizedArgs)
                    {
                        for (const Vst::TNodeRef<Vst::Node>& ParamVst : MaybeLocalizedArgs->GetChildren())
                        {
                            Arguments.Add(DesugarExpressionVst(*ParamVst));
                        }
                    }
                }

                // Now get the RHS value
                Vst::TNodeRef<Vst::Node> RhsVst    = DefinitionVst.GetOperandRight();
                Vst::TNodePtr<Vst::Node> ValueNode = RhsVst;

                // Unwrap if wrapped in a clause
                if (RhsVst->GetElementType() == Vst::NodeType::Clause)
                {
                    if (RhsVst->GetChildCount() == 1)
                    {
                        ValueNode = RhsVst->GetChildren()[0];
                    }
                    else
                    {
                        // Bad clause - too many children
                        ValueNode.Reset();
                    }
                }

                // Only support the Rhs being a string literal or an interpolated string expression
                const Vst::Identifier* MessageKeyVst = nullptr;
                uLang::CUTF8String MessageDefaultText;
                bool bIsExternal = false;

                if (ValueNode)
                {
                    if (const Vst::StringLiteral* RhsStringLiteral = ValueNode->AsNullable<Vst::StringLiteral>())
                    {
                        MessageKeyVst = MaybeLocalizedIdentifier;
                        MessageDefaultText = RhsStringLiteral->GetSourceText();
                    }
                    else if (const Vst::InterpolatedString* RhsInterpolatedString = ValueNode->AsNullable<Vst::InterpolatedString>())
                    {
                        bool bHasNonLiteralInterpolants = false;
                        uLang::CUTF8StringBuilder DecodedString;

                        for (const Vst::TNodeRef<Vst::Node>& RhsChildNode : RhsInterpolatedString->GetChildren())
                        {
                            auto AppendInvalidInterpolantError = [&]()
                            {
                                AppendGlitch(RhsChildNode, uLang::EDiagnostic::ErrSemantic_LocalizesEscape, "Localized message strings may only contain string and character literals, and interpolated arguments.");
                            };

                            if (Vst::StringLiteral* StringLiteral = RhsChildNode->AsNullable<Vst::StringLiteral>())
                            {
                                DecodedString.Append(StringLiteral->GetSourceText());
                            }
                            else if (Vst::Interpolant* Interpolant = RhsChildNode->AsNullable<Vst::Interpolant>())
                            {
                                const Vst::Clause& InterpolantArgClause = Interpolant->GetChildren()[0]->As<Vst::Clause>();
                                uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> DesugaredInterpolantArgs = DesugarExpressionList(InterpolantArgClause.GetChildren());

                                if (DesugaredInterpolantArgs.Num() == 0)
                                {
                                    // Ignore interpolants that contained no syntax other than whitespace or comment trivia.
                                }
                                else if (DesugaredInterpolantArgs.Num() == 1)
                                {
                                    uLang::TSRef<uLang::CExpressionBase> InterpolantArg = DesugaredInterpolantArgs[0];
                                    if (uLang::CExprChar* Char = AsNullable<uLang::CExprChar>(InterpolantArg))
                                    {
                                        DecodedString.Append(Char->AsString());
                                    }
                                    else if (uLang::CExprIdentifierUnresolved* Identifier = AsNullable<uLang::CExprIdentifierUnresolved>(InterpolantArg))
                                    {
                                        DecodedString.Append('{');
                                        if (Identifier->Qualifier() || Identifier->Context())
                                        {
                                            AppendGlitch(InterpolantArg->GetMappedVstNode(), uLang::EDiagnostic::ErrSemantic_LocalizesEscape, "Localized message string interpolated arguments must not be qualified.");
                                        }
                                        // Note: this does not verify that the identifier is an argument to the <localizes> function.
                                        DecodedString.Append(Identifier->_Symbol.AsStringView());
                                        DecodedString.Append('}');
                                        bHasNonLiteralInterpolants = true;
                                    }
                                    else
                                    {
                                        AppendInvalidInterpolantError();
                                    }
                                }
                                else
                                {
                                    AppendInvalidInterpolantError();
                                }
                            }
                            else
                            {
                                AppendInvalidInterpolantError();
                            }
                        }

                        if (MaybeLocalizedArgs || !bHasNonLiteralInterpolants)
                        {
                            MessageKeyVst = MaybeLocalizedIdentifier;
                            MessageDefaultText = DecodedString.MoveToString();
                        }
                    }
                    else if (const Vst::Macro* RhsMacro = ValueNode->AsNullable<Vst::Macro>())
                    {
                        const Vst::Node* RhsMacroName = RhsMacro->GetName();

                        if (RhsMacroName)
                        {
                            const Vst::Identifier* RhsMacroNameIdentifier = RhsMacroName->AsNullable<Vst::Identifier>();

                            if (RhsMacroNameIdentifier)
                            {
                                bIsExternal = RhsMacroNameIdentifier->GetSourceText() == "external";
                            }
                        }
                    }
                }

                if (MessageKeyVst != nullptr &&
                    MaybeLocalizedType != nullptr)
                {
                    // the success case is here - we gathered the message key, default text, and any function arguments
                    const bool bIsFunction = (MaybeLocalizedArgs != nullptr);
                    if (bEnableNamedParametersForLocalize)
                    {
                        return AddMapping(DefinitionVst, DesugarLocalizable(DefinitionVst, *MessageKeyVst, MessageDefaultText, MaybeLocalizedType.AsRef(), Arguments, bIsFunction));
                    }
                    else
                    {
                        return AddMapping(DefinitionVst, DesugarLocalizableOld(DefinitionVst, *MessageKeyVst, MessageDefaultText, MaybeLocalizedType.AsRef(), LocalizedArgumentNameTypePairs, bIsFunction));
                    }
                }
                else if (bIsExternal)
                {
                    // silently allow this through, no need to desugar
                }
                else
                {
                    AppendGlitch(&DefinitionVst, uLang::EDiagnostic::ErrSemantic_LocalizesRhsMustBeString);
                    return AddMapping(DefinitionVst, uLang::TSRef<uLang::CExprError>::New());
                }
            }
        }

        return nullptr;
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarDefinition(const Vst::Definition& DefinitionVst)
    {
        Vst::TNodeRef<Vst::Node> LhsVst = DefinitionVst.GetOperandLeft();
        Vst::TNodeRef<Vst::Node> RhsVst = DefinitionVst.GetOperandRight();
        uLang::TSPtr<uLang::CExpressionBase> Element;
        uLang::TSPtr<uLang::CExpressionBase> ValueDomain;
        uLang::CSymbol               Name;

        uLang::TSPtr<uLang::CExpressionBase> LocalizableDefinition = TryDesugarLocalizable(DefinitionVst);

        if (LocalizableDefinition != nullptr)
        {
            return LocalizableDefinition.AsRef();
        }

        if (const Vst::TypeSpec* LhsTypeSpec = LhsVst->AsNullable<Vst::TypeSpec>())
        {
            if (LhsTypeSpec->HasLhs())
            {
                // Definition is `x:t = y`
                Element = DesugarMaybeNamed(*LhsTypeSpec->GetLhs(), Name);
            }
            ValueDomain = DesugarExpressionVst(*LhsTypeSpec->GetRhs());
        }
        else
        {
            // Definition is `x := y`
            Element = DesugarMaybeNamed(*LhsVst, Name);
        }
        uLang::TSRef<uLang::CExpressionBase> Value = DesugarClauseAsExpression(*RhsVst);
        if (!Name.IsNull() && !ValueDomain)
        {
            // Looks like a named argument - matched `_Parameter` will be set later in semantic analysis
            return AddMapping(DefinitionVst, uLang::TSRef<uLang::CExprMakeNamed>::New(Name, uLang::Move(Element), uLang::Move(Value)));
        }
        uLang::TSRef<uLang::CExprDefinition> DefinitionAst = uLang::TSRef<uLang::CExprDefinition>::New(uLang::Move(Element), uLang::Move(ValueDomain), uLang::Move(Value));
        if (!Name.IsNull())
        {
            // Looks like a named parameter
            DefinitionAst->SetName(Name);
        }
        return AddMapping(DefinitionVst, uLang::Move(DefinitionAst));
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarAssignment(const Vst::Assignment& AssignmentVst)
    {
        Vst::TNodeRef<Vst::Node> LhsVst = AssignmentVst.GetOperandLeft();
        Vst::TNodeRef<Vst::Node> RhsVst = AssignmentVst.GetOperandRight();
        Vst::Assignment::EOp Op = RhsVst->GetTag<Vst::Assignment::EOp>();
        // Desugar the LHS and RHS subexpressions.
        uLang::TSRef<uLang::CExpressionBase> LhsAst = DesugarExpressionVst(*LhsVst);
        uLang::TSRef<uLang::CExpressionBase> RhsAst = DesugarClauseAsExpression(*RhsVst);
        return AddMapping(AssignmentVst, uLang::TSRef<uLang::CExprAssignment>::New(Op, uLang::Move(LhsAst), uLang::Move(RhsAst)));
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarBinaryOpLogicalAndOr(const Vst::Node& VstNode)
    {
        const Vst::NodeType ThisNodeType = VstNode.GetElementType();
        const bool bIsLogicalOr = ThisNodeType == Vst::NodeType::BinaryOpLogicalOr;
        const bool bIsLogicalAnd = ThisNodeType == Vst::NodeType::BinaryOpLogicalAnd;
        const int32_t NumChildren = VstNode.GetChildCount();
        if (NumChildren == 0)
        {
            AppendGlitch(&VstNode, uLang::EDiagnostic::ErrSemantic_BinaryOpNoOperands);
            return AddMapping(VstNode, uLang::TSRef<uLang::CExprError>::New(nullptr, /*bCanFail=*/true));
        }

        // Convert the flat operand list into a right-recursive binary tree: (0 (1 (2 3)))

        // Start with the rightmost child
        const Vst::Node* RHSNode = VstNode.GetChildren().Last().Get();
        uLang::TSRef<uLang::CExpressionBase> Result = DesugarExpressionVst(*RHSNode);

        // Then loop back and build expression tree
        for (int i = NumChildren - 2; i >= 0; --i)
        {
            // Evaluate LHS expression
            const Vst::Node* LHSNode = VstNode.GetChildren()[i].Get();
            uLang::TSRef<uLang::CExpressionBase> LHS = DesugarExpressionVst(*LHSNode);

            // Build expression node
            if (bIsLogicalAnd)
            {
                Result = uLang::TSRef<uLang::CExprShortCircuitAnd>::New(uLang::Move(LHS), uLang::Move(Result));
            }
            else if (bIsLogicalOr)
            {
                Result = uLang::TSRef<uLang::CExprShortCircuitOr>::New(uLang::Move(LHS), uLang::Move(Result));
            }
            else
            {
                ULANG_UNREACHABLE();
            }
        }

        // RHS contains the final expression tree for this node
        return AddMapping(VstNode, uLang::Move(Result));
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarPrefixOpLogicalNot(const Vst::PrefixOpLogicalNot& PrefixOpLogicalNotNode)
    {
        if (PrefixOpLogicalNotNode.GetChildCount() == 0)
        {
            AppendGlitch(&PrefixOpLogicalNotNode, uLang::EDiagnostic::ErrSemantic_PrefixOpNoOperand);
            return AddMapping(PrefixOpLogicalNotNode, uLang::TSRef<uLang::CExprError>::New(nullptr, /*bCanFail=*/true));
        }
        else
        {
            const Vst::Node* OperandVst = PrefixOpLogicalNotNode.GetChildren()[0];
            uLang::TSRef<uLang::CExpressionBase> OperandAst = DesugarExpressionVst(*OperandVst);
            return AddMapping(PrefixOpLogicalNotNode, uLang::TSRef<uLang::CExprLogicalNot>::New(uLang::Move(OperandAst)));
        }
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarBinaryOpCompare(const Vst::BinaryOpCompare& BinaryOpCompareNode)
    {
        const int32_t NumChildren = BinaryOpCompareNode.GetChildCount();
        if (NumChildren != 2)
        {
            AppendGlitch(&BinaryOpCompareNode, uLang::EDiagnostic::ErrSemantic_BinaryOpExpectedTwoOperands);
            return AddMapping(BinaryOpCompareNode, uLang::TSRef<uLang::CExprError>::New(nullptr, /*bCanFail=*/true));
        }
        else
        {
            const Vst::Node* LhsNode = BinaryOpCompareNode.GetChildren()[0].Get();
            uLang::TSRef<uLang::CExpressionBase> Lhs = DesugarExpressionVst(*LhsNode);
            
            // Get RHS operand
            const Vst::Node* RhsNode = BinaryOpCompareNode.GetChildren()[1].Get();
            uLang::TSRef<uLang::CExpressionBase> Rhs = DesugarExpressionVst(*RhsNode);

            uLang::TSRef<uLang::CExprMakeTuple> Argument = uLang::TSRef<uLang::CExprMakeTuple>::New(uLang::Move(Lhs), uLang::Move(Rhs));
            Argument->SetNonReciprocalMappedVstNode(&BinaryOpCompareNode);
            uLang::TSRef<uLang::CExpressionBase> Result = AddMapping(BinaryOpCompareNode, uLang::TSRef<uLang::CExprComparison>::New(
                RhsNode->GetTag<Vst::BinaryOpCompare::op>(),
                Argument));
            return Result;
        }
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarBinaryOp(const Vst::BinaryOp& BinaryOpNode)
    {
        using EOp = Vst::BinaryOp::op;

        const int32_t NumChildren = BinaryOpNode.GetChildCount();
        if (NumChildren == 0)
        {
            AppendGlitch(&BinaryOpNode, uLang::EDiagnostic::ErrSemantic_BinaryOpNoOperands);
            return AddMapping(BinaryOpNode, uLang::TSRef<uLang::CExprError>::New());
        }

        // Get our first LHS operand
        const Vst::Node* LHSNode = BinaryOpNode.GetChildren()[0].Get();
        uLang::TSPtr<uLang::CExpressionBase> LhsPtr;
        

        bool bHasLeadingOperator = false; 
        if (LHSNode->GetElementType() == Vst::NodeType::Operator && BinaryOpNode.GetChildCount() > 1)
        {
            bHasLeadingOperator = true;

            const Vst::Node* OperatorNode = BinaryOpNode.GetChildren()[0].Get();
            const Vst::Node* OprandNode = BinaryOpNode.GetChildren()[1].Get();

            const uLang::CUTF8String& OpString = OperatorNode->As<Vst::Operator>().GetSourceText();
            uLang::TSRef<uLang::CExpressionBase> Result = DesugarExpressionVst(*OprandNode);
            if (OpString[0] == u'-')
            {
                Result = uLang::TSRef<uLang::CExprUnaryArithmetic>::New(uLang::CExprUnaryArithmetic::EOp::Negate, uLang::Move(Result));
                LhsPtr = AddMapping(BinaryOpNode, uLang::Move(Result));
            }
            else
            {
                LhsPtr = uLang::Move(Result);
            }
        }
        else
        {
            // Get our first LHS operand
            LhsPtr = DesugarExpressionVst(*LHSNode);
        }


        uLang::TSRef<uLang::CExpressionBase> Lhs = LhsPtr.AsRef();

        auto HandleMalformedVst = [&Lhs](uLang::TSRef<uLang::CExpressionBase>&& Rhs)
        {
            uLang::TSRef<uLang::CExprError> ErrorExpr = uLang::TSRef<uLang::CExprError>::New();
            ErrorExpr->AppendChild(uLang::Move(Lhs));
            ErrorExpr->AppendChild(uLang::Move(Rhs));
            Lhs = uLang::Move(ErrorExpr);
        };

        // Then loop and build expression tree
        for (int i = bHasLeadingOperator ? 2 : 1; i < NumChildren; i += 2)
        {
            const Vst::Node* OperatorNode = BinaryOpNode.GetChildren()[i].Get();

            ULANG_ENSUREF(OperatorNode->GetTag<EOp>() == EOp::Operator, "Malformed binary op node, expecting an operator. ");
            if (ULANG_ENSUREF(i + 1 < NumChildren, "Malformed binary Op node, no trailing operand."))
            {
                const Vst::Node* RhsOperandNode = BinaryOpNode.GetChildren()[i + 1].Get();
                ULANG_ENSUREF(RhsOperandNode->GetTag<EOp>() == EOp::Operand, "Malformed binary op node, expecting an operand.");
                uLang::TSRef<uLang::CExpressionBase> Rhs = DesugarExpressionVst(*RhsOperandNode);

                if (OperatorNode->GetElementType() == Vst::NodeType::Operator)
                {
                    const uLang::CUTF8String& OpString = OperatorNode->As<Vst::Operator>().GetSourceText();
                    if (OpString.ByteLen() == 1)
                    {
                        uLang::CExprBinaryArithmetic::EOp ArithmeticOp;
                        if(BinaryOpNode.GetElementType() == Vst::NodeType::BinaryOpAddSub)
                            ArithmeticOp = (OpString[0] == u'+')
                            ? uLang::CExprBinaryArithmetic::EOp::Add
                            : uLang::CExprBinaryArithmetic::EOp::Sub;
                        else if(BinaryOpNode.GetElementType() == Vst::NodeType::BinaryOpMulDivInfix)
                            ArithmeticOp = (OpString[0] == u'*')
                            ? uLang::CExprBinaryArithmetic::EOp::Mul
                            : uLang::CExprBinaryArithmetic::EOp::Div;
                        else 
                            ULANG_UNREACHABLE();
                        uLang::TSRef<uLang::CExprMakeTuple> Argument = uLang::TSRef<uLang::CExprMakeTuple>::New(uLang::Move(Lhs), uLang::Move(Rhs));
                        Argument->SetNonReciprocalMappedVstNode(OperatorNode);
                        Lhs = AddMapping(*OperatorNode, uLang::TSRef<uLang::CExprBinaryArithmetic>::New(
                            ArithmeticOp,
                            Argument));
                    }
                    else
                    {
                        HandleMalformedVst(uLang::Move(Rhs));
                    }
                }
                else if (OperatorNode->GetElementType() == Vst::NodeType::Identifier)
                {
                    uLang::TSRef<uLang::CExprMakeTuple> Argument = uLang::TSRef<uLang::CExprMakeTuple>::New(uLang::Move(Lhs), uLang::Move(Rhs));
                    Argument->SetNonReciprocalMappedVstNode(OperatorNode);
                    uLang::TSRef<uLang::CExprInvocation> Invocation = uLang::TSRef<uLang::CExprInvocation>::New(Argument);
                    const uLang::CSymbol OperatorSymbol = VerifyAddSymbol(OperatorNode, uLang::CUTF8String("operator'%s'", OperatorNode->As<Vst::Identifier>().GetSourceCStr()));
                    Invocation->SetCallee(uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(OperatorSymbol));
                    Lhs = AddMapping(*OperatorNode, uLang::Move(Invocation));
                }
                else
                {
                    HandleMalformedVst(uLang::Move(Rhs));
                }
            }
        }

        // LHS contains the final expression tree for this node
        return uLang::Move(Lhs);
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarBinaryOpRange(const Vst::BinaryOpRange& BinaryOpRange)
    {
        uLang::TSRef<uLang::CExpressionBase> Lhs = DesugarExpressionVst(*BinaryOpRange.GetChildren()[0]);
        uLang::TSRef<uLang::CExpressionBase> Rhs = DesugarExpressionVst(*BinaryOpRange.GetChildren()[1]);
        return AddMapping(BinaryOpRange, uLang::TSRef<uLang::CExprMakeRange>::New(uLang::Move(Lhs), uLang::Move(Rhs)));
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarBinaryOpArrow(const Vst::BinaryOpArrow& BinaryOpArrow)
    {
        uLang::TSRef<uLang::CExpressionBase> Lhs = DesugarExpressionVst(*BinaryOpArrow.GetChildren()[0]);
        uLang::TSRef<uLang::CExpressionBase> Rhs = DesugarExpressionVst(*BinaryOpArrow.GetChildren()[1]);
        return AddMapping(BinaryOpArrow, uLang::TSRef<uLang::CExprArrow>::New(uLang::Move(Lhs), uLang::Move(Rhs)));
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarMaybeNamed(Vst::Node& VstNode, uLang::CSymbol& Name)
    {
        if ((VstNode.GetElementType() == Vst::NodeType::PrePostCall) && (VstNode.GetChildCount() >= 2))
        {
            Vst::TNodeRef<Vst::Node> VarChild0 = VstNode.GetChildren()[0];
            Vst::Node* VarChild1 = VstNode.GetChildren()[1];

            // No qualified named parameters yet
            if ((VarChild0->GetTag<Vst::PrePostCall::Op>() == Vst::PrePostCall::Op::Option)
                && (VarChild0->GetElementType() == Vst::NodeType::Clause)
                && (VarChild1->GetTag<Vst::PrePostCall::Op>() == Vst::PrePostCall::Op::Expression)
                && (VarChild1->GetElementType() == Vst::NodeType::Identifier))
            {
                const Vst::Identifier& Identifier = VarChild1->As<Vst::Identifier>();
                if (Identifier.IsQualified())
                {
                    AppendGlitch(
                        Identifier.GetQualification(),
                        uLang::EDiagnostic::ErrSemantic_Unsupported,
                        "Qualifiers are not yet supported on named parameters.");
                }

                Name = VerifyAddSymbol(VarChild1, Identifier.GetSourceText());
                // Temporarily remove option clause so option not created and track as explicitly ?named parameter or argument
                VstNode.AccessChildren().RemoveAt(0);
                // Continue processing
                uLang::TSRef<uLang::CExpressionBase> NamedExpr = DesugarExpressionVst(VstNode);
                // Replace temporarily removed option clause so VST remains as it was originally
                VstNode.AccessChildren().Insert(VarChild0, 0);
                return NamedExpr;
            }
        }
        return DesugarExpressionVst(VstNode);
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarTypeSpec(const Vst::TypeSpec& TypeSpecVst)
    {
        uLang::TSPtr<uLang::CExpressionBase> Lhs = nullptr;
        uLang::CSymbol Name;

        if (TypeSpecVst.HasLhs())
        {
            Lhs = DesugarMaybeNamed(*TypeSpecVst.GetLhs(), Name);
        }
        uLang::TSRef<uLang::CExpressionBase> Rhs = DesugarExpressionVst(*TypeSpecVst.GetRhs());

        uLang::TSPtr<uLang::CExpressionBase> NoDefaultValue = nullptr;

        // Create a uLang::CExprDefinition AST node.
        uLang::TSRef<uLang::CExprDefinition> DefinitionAst = uLang::TSRef<uLang::CExprDefinition>::New(uLang::Move(Lhs), uLang::Move(Rhs), uLang::Move(NoDefaultValue));

        if (!Name.IsNull())
        {
            DefinitionAst->SetName(Name);
        }
        
        // Desugar the type-spec's attributes.
        if (TypeSpecVst.HasAttributes())
        {
            DefinitionAst->_Attributes = DesugarAttributes(TypeSpecVst.GetAux()->GetChildren());
        }

        return AddMapping(TypeSpecVst, uLang::Move(DefinitionAst));
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarCall(bool bCalledWithBrackets, const Vst::Clause& CallArgs, uLang::TSRef<uLang::CExpressionBase>&& Callee)
    {
        // Create an invocation AST node.
        return AddMapping(CallArgs, uLang::TSRef<uLang::CExprInvocation>::New(
            bCalledWithBrackets 
            ? uLang::CExprInvocation::EBracketingStyle::SquareBrackets 
            : uLang::CExprInvocation::EBracketingStyle::Parentheses,
            uLang::Move(Callee),
            DesugarExpressionListAsExpression(CallArgs, CallArgs.GetForm(), false)));
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarPrePostCall(const Vst::PrePostCall& Ppc)
    {
        using PpcOp = Vst::PrePostCall::Op;

        const int32_t NumPpcNodes = Ppc.GetChildCount();

        const int32_t ExpressionIndex = [&]()
        {
            for (int32_t i = 0; i < NumPpcNodes; i += 1)
            {
                const Vst::Node* PpcChildNode = Ppc.GetChildren()[i];
                if (PpcChildNode->GetTag<PpcOp>() == PpcOp::Expression)
                {
                    return i;
                }
            }

            ULANG_ERRORF("Malformed Vst : DotIndent cannot be a prefix.");
            return -1;
        }();

        //~~~~ HANDLE POSTFIXES ~~~~~~~~~~~~~~~~
        uLang::TSPtr<uLang::CExpressionBase> Lhs;
        for (int32_t i = ExpressionIndex; i < NumPpcNodes; i += 1)
        {
            const Vst::Node& PpcChildNode     = *Ppc.GetChildren()[i];
            switch (PpcChildNode.GetTag<PpcOp>())
            {
            case PpcOp::Expression:
            {
                Lhs = DesugarExpressionVst(PpcChildNode);
            }
            break;
            // Handle <expr>?
            case PpcOp::Option:
            {
                ULANG_ASSERTF(Lhs, "Expected expr on LHS of QMark");
                Lhs = AddMapping(PpcChildNode, uLang::TSRef<uLang::CExprQueryValue>::New(uLang::Move(Lhs.AsRef())));
            }
            break;
            case PpcOp::Pointer:
            {
                ULANG_ASSERTF(Lhs, "Expected expr on LHS of Hat");
                Lhs = AddMapping(PpcChildNode, uLang::TSRef<uLang::CExprPointerToReference>::New(uLang::Move(Lhs.AsRef())));
            }
            break;
            case PpcOp::DotIdentifier:
            {
                ULANG_ASSERTF(Lhs, "Expected expr on LHS of DotIdentifier");
                const Vst::Identifier& IdentifierNode = PpcChildNode.As<Vst::Identifier>();
                Lhs = DesugarIdentifier(IdentifierNode, uLang::Move(Lhs));
                if (IdentifierNode.HasAttributes())
                {
                    Lhs->_Attributes = DesugarAttributes(IdentifierNode.GetAux()->GetChildren());
                }
            }
            break;
            case PpcOp::SureCall:
            case PpcOp::FailCall:
            {
                ULANG_ASSERTF(Lhs, "Expected expr on LHS of call");
                Lhs = DesugarCall(
                    PpcChildNode.GetTag<PpcOp>() == PpcOp::FailCall,
                    PpcChildNode.As<Vst::Clause>(),   // Arguments
                    uLang::Move(Lhs.AsRef())                 // Receiver expression
                );
            }
            break;
            default: ULANG_ERRORF("Unknown PrePostCall tag!"); break;
            }
        }
        

        uLang::TSRef<uLang::CExpressionBase> Rhs = uLang::Move(Lhs.AsRef());

        //~~~~ HANDLE PREFIXES ~~~~~~~~~~~~~~~~~
        // If ExpressionIndex > 0, this expression has prefix subexpressions.
        if (ExpressionIndex > 0)
        {
            // Prefixes are handles right to left.
            // We start with the expression, and work our way to the left, applying
            // whatever modifier we might encounter.
            // e.g. Given `?[]Item` we would have the following `RhsType`
            //   1. RhsType = Item
            //   2. RhsType = []RhsType = []Item  a.k.a array of items
            //   3. RhsType = ?RhsType  = ?[]Item a.k.a. option array of items

            //@jira SOL-998 : This use-case needs to be updated
            for (int32_t i = ExpressionIndex-1; i >= 0; i -= 1)
            {
                const Vst::TNodeRef<Vst::Node>& PpcChildNode = Ppc.GetChildren()[i];
                switch (PpcChildNode->GetTag<PpcOp>())
                {
                case PpcOp::Expression: { ULANG_ERRORF("Expression should have been processed by the 'HANDLE POSTFIXES' above."); } break;
                case PpcOp::DotIdentifier: { ULANG_ERRORF("Malformed Vst : DotIndent cannot be a prefix."); } break;
                case PpcOp::Pointer:
                {
                    AppendGlitch(
                        PpcChildNode.Get(),
                        uLang::EDiagnostic::ErrSemantic_Unsupported,
                        uLang::CUTF8String("Non-unique pointers are not supported yet"));
                    
                    Rhs = AddMapping(*PpcChildNode, uLang::TSRef<uLang::CExprError>::New());
                }
                break;
                case PpcOp::Option:
                {
                    Rhs = AddMapping(*PpcChildNode, uLang::TSRef<uLang::CExprOptionTypeFormer>::New(uLang::Move(Rhs)));
                }
                break;
                case PpcOp::FailCall:
                {
                    if (PpcChildNode->GetChildCount())
                    {
                        // Desugar the key expressions.
                        ULANG_ASSERTF(PpcChildNode->IsA<Vst::Clause>(), "Expected prefix [] operand to be a clause");
                        Vst::Clause& LhsClause = PpcChildNode->As<Vst::Clause>();
                        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> LhsAsts;
                        for (const Vst::TNodeRef<Vst::Node>& LhsVst : LhsClause.GetChildren())
                        {
                            LhsAsts.Add(DesugarExpressionVst(*LhsVst));
                        }

                        Rhs = AddMapping(*PpcChildNode, uLang::TSRef<uLang::CExprMapTypeFormer>::New(uLang::Move(LhsAsts), uLang::Move(Rhs)));
                    }
                    else
                    {
                        Rhs = AddMapping(*PpcChildNode, uLang::TSRef<uLang::CExprArrayTypeFormer>::New(uLang::Move(Rhs)));
                    }
                }
                break;
                case PpcOp::SureCall:
                {
                    AppendGlitch(
                        PpcChildNode.Get(),
                        uLang::EDiagnostic::ErrSemantic_Unsupported,
                        uLang::CUTF8String("Unsupported: prefix'()' not supported yet"));
                    
                    Rhs = AddMapping(*PpcChildNode, uLang::TSRef<uLang::CExprError>::New());
                }
                break;
                default: ULANG_UNREACHABLE();
                }
            }
        }

        return uLang::Move(Rhs);
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarIdentifier(const Vst::Identifier& IdentifierNode, uLang::TSPtr<uLang::CExpressionBase>&& Context = nullptr)
    {
        if (IdentifierNode.IsQualified())
        {
            if (IdentifierNode.GetChildCount() > 1)
            {
                AppendGlitch(IdentifierNode.GetChildren()[0], uLang::EDiagnostic::ErrSemantic_ExpectedSingleExpression, "Only one qualifying expression is allowed.");
                return AddMapping(*IdentifierNode.GetChildren()[0], uLang::TSRef<uLang::CExprError>::New());
            }

            const uLang::CSymbol Symbol = VerifyAddSymbol(&IdentifierNode, IdentifierNode.GetSourceText());
            uLang::TSRef<uLang::CExpressionBase> QualifierAst = DesugarExpressionVst(*IdentifierNode.GetQualification());
            return AddMapping(IdentifierNode, uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(Symbol, uLang::Move(Context), uLang::Move(QualifierAst)));
        }
        else
        {
            const uLang::CSymbol Symbol = VerifyAddSymbol(&IdentifierNode, IdentifierNode.GetSourceText());
            return AddMapping(IdentifierNode, uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(Symbol, uLang::Move(Context)));
        }
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarFlowIf(const Vst::FlowIf& IfNode)
    {
        // All `if` nodes will have clause block children though they may be empty
        // The simplest forms that can get past the parser (though will have semantic issues) is `if:` and `if ():`
        const int32_t         NumChildren = IfNode.GetChildCount();
        const Vst::NodeArray& Clauses     = IfNode.GetChildren();

        // First, desugar the optional final else clause.
        uLang::TSPtr<uLang::CExpressionBase> Result;
        int32_t Index = NumChildren - 1;
        if (Clauses[Index]->GetTag<Vst::FlowIf::ClauseTag>() == Vst::FlowIf::ClauseTag::else_body)
        {
            Result = DesugarClauseAsCodeBlock(Clauses[Index]->As<Vst::Clause>());
            --Index;
        }
        
        // Desugar pairs of clauses into nested uLang::CExprIf nodes.
        // Must be in this order:
        //   - if identifier   ]
        //   - condition block  |- Repeating
        //   - [then block]    ]
        //   - [else block]    -- Optional last node
        // Loop in reverse order, with the first corresponding to the outermost uLang::CExprIf.
        while(Index >= 0)
        {
            switch(Clauses[Index]->GetTag<Vst::FlowIf::ClauseTag>())
            {
            case Vst::FlowIf::ClauseTag::if_identifier:
            {
                Index--;
                break;
            }
            case Vst::FlowIf::ClauseTag::then_body:
            {
                ULANG_ASSERTF(Index > 1, "Clause of FlowIf node is unexpectedly a then clause");
                if (Clauses[Index-1]->GetTag<Vst::FlowIf::ClauseTag>() != Vst::FlowIf::ClauseTag::condition)
                {
                    AppendGlitch(Clauses[Index-1], uLang::EDiagnostic::ErrSemantic_MalformedConditional, "Expected condition.");
                    uLang::TSRef<uLang::CExprError> ErrorNode = uLang::TSRef<uLang::CExprError>::New();
                    ErrorNode->AppendChild(uLang::Move(Result));
                    Result = uLang::Move(ErrorNode);
                    --Index;
                }
                else
                {
                    const Vst::Clause& Condition = Clauses[Index-1]->As<Vst::Clause>();
                    uLang::TSRef<uLang::CExprCodeBlock> ConditionCodeBlock = DesugarClauseAsCodeBlock(Condition);

                    const Vst::Clause& ThenClause = Clauses[Index]->As<Vst::Clause>();
                    uLang::TSRef<uLang::CExprCodeBlock> ThenCodeBlock = DesugarClauseAsCodeBlock(ThenClause);
            
                    Result = uLang::TSRef<uLang::CExprIf>::New(uLang::Move(ConditionCodeBlock), uLang::Move(ThenCodeBlock), uLang::Move(Result));
                    Index -= 2;
                }

                break;
            }
            case Vst::FlowIf::ClauseTag::condition:
            {
                ULANG_ASSERTF(Index > 0, "Clause of FlowIf node is unexpectedly a condition clause");
                ULANG_ASSERTF(Clauses[Index-1]->GetTag<Vst::FlowIf::ClauseTag>() == Vst::FlowIf::ClauseTag::if_identifier, "if_identifier clause of FlowIf should precede the condition clause");
                const Vst::Clause& Condition = Clauses[Index]->As<Vst::Clause>();
                uLang::TSRef<uLang::CExprCodeBlock> ConditionCodeBlock = DesugarClauseAsCodeBlock(Condition);
                --Index;

                Result = uLang::TSRef<uLang::CExprIf>::New(uLang::Move(ConditionCodeBlock), nullptr, uLang::Move(Result));

                break;
            }
            case Vst::FlowIf::ClauseTag::else_body:
            {
                AppendGlitch(
                    Clauses[Index],
                    uLang::EDiagnostic::ErrSemantic_MalformedConditional,
                    "Expected then clause or condition while parsing `if`.");
                uLang::TSRef<uLang::CExprError> ErrorNode = uLang::TSRef<uLang::CExprError>::New();
                ErrorNode->AppendChild(uLang::Move(Result));
                Result = uLang::Move(ErrorNode);
                Index -= 2;
                break;
            }
            default:
                ULANG_UNREACHABLE();
            };
        };
        
        return AddMapping(IfNode, uLang::Move(Result.AsRef()));
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarIntLiteral(const Vst::IntLiteral& IntLiteralNode)
    {
        // We look back at the mapped Vst node during analysis
        return AddMapping(IntLiteralNode, uLang::TSRef<uLang::CExprNumber>::New());
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarFloatLiteral(const Vst::FloatLiteral& FloatLiteralNode)
    {
        return AddMapping(FloatLiteralNode, uLang::TSRef<uLang::CExprNumber>::New());
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarCharLiteral(const Vst::CharLiteral& CharLiteralNode)
    {
        uLang::CExprChar::EType Type;
        switch (CharLiteralNode._Format)
        {
        case Vst::CharLiteral::EFormat::ASCII:
        case Vst::CharLiteral::EFormat::UTF8CodeUnit:
        case Vst::CharLiteral::EFormat::EscapedCode:
            Type = uLang::CExprChar::EType::UTF8CodeUnit;
            break;
        case Vst::CharLiteral::EFormat::UnicodeScalar:
        case Vst::CharLiteral::EFormat::UnicodeScalarCode:
            Type = uLang::CExprChar::EType::UnicodeCodePoint;
            break;
        default:
            ULANG_UNREACHABLE();
        }

        return AddMapping(CharLiteralNode, uLang::TSRef<uLang::CExprChar>::New(CharLiteralNode._Value, Type));
    }

    // The extra optional VstNode parameter is used when the string literal is created from a temporary StringLiteralNode. 
    uLang::TSRef<uLang::CExprString> DesugarStringLiteral(const Vst::StringLiteral& StringLiteralNode)
    {
        return AddMapping(StringLiteralNode, uLang::TSRef<uLang::CExprString>::New(StringLiteralNode.GetSourceText()));
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarPathLiteral(const Vst::PathLiteral& PathLiteralNode)
    {
        return AddMapping(PathLiteralNode, uLang::TSRef<uLang::CExprPath>::New(PathLiteralNode.GetSourceText()));
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarInterpolatedString(const Vst::InterpolatedString& InterpolatedStringNode)
    {
        const uLang::CSymbol ToStringSymbol = _Symbols.AddChecked("ToString");

        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> DesugaredChildren;
        uLang::TSPtr<uLang::CExprString> TailString;
        for (const Vst::TNodeRef<Vst::Node>& ChildNode : InterpolatedStringNode.GetChildren())
        {
            if (Vst::StringLiteral* StringLiteral = ChildNode->AsNullable<Vst::StringLiteral>())
            {
                if (TailString)
                {
                    TailString->_String += StringLiteral->GetSourceText();
                }
                else
                {
                    uLang::TSRef<uLang::CExprString> StringLiteralAst = DesugarStringLiteral(*StringLiteral);
                    TailString = StringLiteralAst;
                    DesugaredChildren.Add(uLang::Move(StringLiteralAst));
                }
            }
            else if (Vst::Interpolant* Interpolant = ChildNode->AsNullable<Vst::Interpolant>())
            {
                const Vst::Clause& InterpolantArgClause = Interpolant->GetChildren()[0]->As<Vst::Clause>();
                uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> DesugaredInterpolantArgs = DesugarExpressionList(InterpolantArgClause.GetChildren());

                // Ignore interpolants that only contained whitespace and comments.
                if (DesugaredInterpolantArgs.Num())
                {
                    if (DesugaredInterpolantArgs.Num() == 1 && DesugaredInterpolantArgs[0]->GetNodeType() == uLang::EAstNodeType::Literal_Char)
                    {
                        const uLang::CExprChar& Char = static_cast<const uLang::CExprChar&>(*DesugaredInterpolantArgs[0]);
                        if (TailString)
                        {
                            TailString->_String += Char.AsString();
                        }
                        else
                        {
                            uLang::TSRef<uLang::CExprString> StringLiteralAst = uLang::TSRef<uLang::CExprString>::New(Char.AsString());
                            TailString = StringLiteralAst;
                            DesugaredChildren.Add(uLang::Move(StringLiteralAst));
                        }
                    }
                    else
                    {
                        uLang::TSRef<uLang::CExpressionBase> ToStringArg = MakeExpressionFromExpressionList(uLang::Move(DesugaredInterpolantArgs), InterpolantArgClause.GetForm(), InterpolantArgClause, false);
                        uLang::TSRef<uLang::CExprInvocation> ToStringInvocation = uLang::TSRef<uLang::CExprInvocation>::New(
                            uLang::CExprInvocation::EBracketingStyle::Parentheses,
                            uLang::TSRef<uLang::CExprIdentifierUnresolved>::New(ToStringSymbol),
                            ToStringArg);
                        DesugaredChildren.Add(AddMapping(*Interpolant, uLang::Move(ToStringInvocation)));
                        TailString.Reset();
                    }
                }
            }
            else
            {
                AppendGlitch(ChildNode, uLang::EDiagnostic::ErrSemantic_Internal, uLang::CUTF8String("Unexpected InterpolatedString child node %s", Vst::GetNodeTypeName(ChildNode->GetElementType())));
            }
        }

        if (DesugaredChildren.Num() == 1)
        {
            return DesugaredChildren[0];
        }
        else if (DesugaredChildren.Num())
        {
            // Build a left-associated chain of `+` operations across the desugared children.
            uLang::TSRef<uLang::CExpressionBase> Result = uLang::Move(DesugaredChildren[0]);
            for (int32_t I = 1; I < DesugaredChildren.Num(); ++I)
            {
                uLang::TSRef<uLang::CExprMakeTuple> Argument = uLang::TSRef<uLang::CExprMakeTuple>::New(uLang::Move(Result), uLang::Move(DesugaredChildren[I]));
                Result = uLang::TSRef<uLang::CExprBinaryArithmetic>::New(uLang::CExprBinaryArithmetic::EOp::Add, uLang::Move(Argument));
            }
            return AddMapping(InterpolatedStringNode, uLang::Move(Result));
        }
        else
        {
            return AddMapping(InterpolatedStringNode, uLang::TSRef<uLang::CExprString>::New(""));
        }
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarLambda(const Vst::Lambda& LambdaVst)
    {
        uLang::TSRef<uLang::CExpressionBase> DomainAst = DesugarExpressionVst(*LambdaVst.GetChildren()[0]);
        uLang::TSRef<uLang::CExpressionBase> RangeAst = DesugarClauseAsExpression(*LambdaVst.GetChildren()[1]);
        return AddMapping(LambdaVst, uLang::TSRef<uLang::CExprFunctionLiteral>::New(uLang::Move(DomainAst), uLang::Move(RangeAst)));
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarControl(const Vst::Control& ControlNode)
    {
        switch (ControlNode._Keyword)
        {
        case Vst::Control::EKeyword::Return:
            {
                uLang::TSPtr<uLang::CExpressionBase> ResultAst;
                if (ControlNode.GetChildCount() == 1)
                {
                    ResultAst = DesugarExpressionVst(*ControlNode.GetReturnExpression());
                }
                else if (ControlNode.GetChildCount() > 1)
                {
                    AppendGlitch(
                        &ControlNode,
                        uLang::EDiagnostic::ErrSemantic_UnexpectedNumberOfArguments,
                        "`return` may only have a single sub-expression when returning a result.");
                    return AddMapping(ControlNode, uLang::TSRef<uLang::CExprError>::New());
                }

                return AddMapping(ControlNode, uLang::TSRef<uLang::CExprReturn>::New(uLang::Move(ResultAst)));
            }
        case Vst::Control::EKeyword::Break:
            {
                if (ControlNode.GetChildCount() > 0)
                {
                    AppendGlitch(
                        &ControlNode,
                        uLang::EDiagnostic::ErrSemantic_UnexpectedNumberOfArguments,
                        "`break` may not have any sub-expressions - it does not return a result.");
                    return AddMapping(ControlNode, uLang::TSRef<uLang::CExprError>::New());
                }
                return AddMapping(ControlNode, uLang::TSRef<uLang::CExprBreak>::New());
            }
        case Vst::Control::EKeyword::Yield:
            AppendGlitch(&ControlNode, uLang::EDiagnostic::ErrSemantic_Unimplemented);
            return AddMapping(ControlNode, uLang::TSRef<uLang::CExprError>::New());
        case Vst::Control::EKeyword::Continue:
            AppendGlitch(&ControlNode, uLang::EDiagnostic::ErrSemantic_Unimplemented);
            return AddMapping(ControlNode, uLang::TSRef<uLang::CExprError>::New());
        default: 
            return AddMapping(ControlNode, uLang::TSRef<uLang::CExprError>::New());
        }  
    }
    
    uLang::TSRef<uLang::CExpressionBase> DesugarMacro(const Vst::Macro& MacroVst)
    {
        const int32_t NumMacroChildren = MacroVst.GetChildCount();

        const Vst::Node& MacroNameVst = *MacroVst.GetName();
        uLang::TSRef<uLang::CExprMacroCall> MacroCallAst = uLang::TSRef<uLang::CExprMacroCall>::New(DesugarExpressionVst(MacroNameVst), NumMacroChildren);

        // Populate the clauses in the macro
        for (int32_t i = 1; i < NumMacroChildren; i += 1)
        {
            Vst::Node& ThisMacroChild = *MacroVst.GetChildren()[i];
            if (!ThisMacroChild.IsA<Vst::Clause>())
            {
                AppendGlitch(MacroVst.GetChildren()[i].Get(), uLang::EDiagnostic::ErrSemantic_MalformedMacro,
                    "Malformed macro: expected a macro clause");
            }
            else
            {
                // Add clause and its children to the macro

                Vst::Clause& ThisClause = ThisMacroChild.As<Vst::Clause>();

                // Don't allow attributes on macro clauses, since they'll otherwise be thrown away at this point.
                if (ThisClause.HasAttributes())
                {
                    AppendGlitch(ThisClause.GetAux()->GetChildren()[0].Get(), uLang::EDiagnostic::ErrSemantic_AttributeNotAllowed);
                }

                const uLang::EMacroClauseTag ClauseTag = [&ThisClause, this]() {
                    using res_t = vsyntax::res_t;
                    switch (ThisClause.GetTag<res_t>())
                    {
                    case res_t::res_none: return uLang::EMacroClauseTag::None;
                    case res_t::res_of: return uLang::EMacroClauseTag::Of;
                    case res_t::res_do: return uLang::EMacroClauseTag::Do;

                    case res_t::res_if:
                    case res_t::res_else:
                    case res_t::res_upon:
                    case res_t::res_where:
                    case res_t::res_catch:
                    case res_t::res_then:
                    case res_t::res_until:
                    case res_t::res_return:
                    case res_t::res_yield:
                    case res_t::res_break:
                    case res_t::res_continue:
                    case res_t::res_at:
                    case res_t::res_var:
                    case res_t::res_set:
                    case res_t::res_and:
                    case res_t::res_or:
                    case res_t::res_not:
                        AppendGlitch(&ThisClause, uLang::EDiagnostic::ErrSemantic_MalformedMacro,
                            "Malformed macro: reserved word invalid in macro clause");
                        return uLang::EMacroClauseTag::None;
                    case res_t::res_max:
                    default:
                        AppendGlitch(&ThisClause, uLang::EDiagnostic::ErrSemantic_MalformedMacro,
                            "Malformed macro: Unknown keyword");
                        return uLang::EMacroClauseTag::None;
                    }
                }();

                uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> ClauseExprs = DesugarExpressionList(ThisClause.GetChildren());

                MacroCallAst->AppendClause(uLang::CExprMacroCall::CClause(ClauseTag, ThisClause.GetForm(), uLang::Move(ClauseExprs)));

                if (ThisClause.HasAttributes())
                {
                    MacroCallAst->_Attributes += DesugarAttributes(ThisClause.GetAux()->GetChildren());
                }
            }
        }

        return AddMapping(MacroVst, uLang::Move(MacroCallAst));
    }

    uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> DesugarExpressionList(const Vst::NodeArray& Expressions)
    {
        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> DesugaredExpressions;
        for (const Vst::Node* Child : Expressions)
        {
            // Ignore comments in the subexpression list.
            if (!Child->IsA<Vst::Comment>())
            {
                DesugaredExpressions.Add(DesugarExpressionVst(*Child));
            }
        }
        return DesugaredExpressions;
    }

    uLang::TSRef<uLang::CExprMakeTuple> WrapExpressionListInTuple(uLang::TArray<uLang::TSRef<uLang::CExpressionBase>>&& Expressions, const Vst::Node& OriginNode, bool bReciprocalVstMapping)
    {
        uLang::TSRef<uLang::CExprMakeTuple> Tuple = uLang::TSRef<uLang::CExprMakeTuple>::New(Expressions.Num());
        Tuple->SetSubExprs(uLang::Move(Expressions));
        if (bReciprocalVstMapping)
        {
            OriginNode.AddMapping(Tuple.Get());
        }
        else
        {
            Tuple->SetNonReciprocalMappedVstNode(&OriginNode);
        }
        return Tuple;
    }

    uLang::TSRef<uLang::CExprCodeBlock> WrapExpressionListInCodeBlock(uLang::TArray<uLang::TSRef<uLang::CExpressionBase>>&& Expressions, const Vst::Node& OriginNode, bool bReciprocalVstMapping)
    {
        uLang::TSRef<uLang::CExprCodeBlock> Block = uLang::TSRef<uLang::CExprCodeBlock>::New(Expressions.Num());
        Block->SetSubExprs(uLang::Move(Expressions));
        if (bReciprocalVstMapping)
        {
            OriginNode.AddMapping(Block.Get());
        }
        else
        {
            Block->SetNonReciprocalMappedVstNode(&OriginNode);
        }
        return Block;
    }

    uLang::TSRef<uLang::CExpressionBase> MakeExpressionFromExpressionList(uLang::TArray<uLang::TSRef<uLang::CExpressionBase>>&& DesugaredExpressions, Vst::Clause::EForm Form, const Vst::Node& OriginNode, bool bReciprocalVstMapping = true)
    {
        if (DesugaredExpressions.Num() == 1)
        {
            // If this is a single expression, return it directly.
            return DesugaredExpressions[0];
        }
        else if (Form == Vst::Clause::EForm::NoSemicolonOrNewline)
        {
            // If this is an empty or comma separated list, create a tuple for the subexpressions.
            return WrapExpressionListInTuple(uLang::Move(DesugaredExpressions), OriginNode, bReciprocalVstMapping);
        }
        else
        {
            // Otherwise, create a code block for the subexpressions.
            return WrapExpressionListInCodeBlock(uLang::Move(DesugaredExpressions), OriginNode, bReciprocalVstMapping);
        }
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarExpressionListAsExpression(const Vst::Node& Node, Vst::Clause::EForm Form, bool bReciprocalVstMapping = true)
    {
        return MakeExpressionFromExpressionList(DesugarExpressionList(Node.GetChildren()), Form, Node, bReciprocalVstMapping);
    }

    uLang::TSRef<uLang::CExprCodeBlock> DesugarClauseAsCodeBlock(const Vst::Clause& Clause)
    {
        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> DesugaredChildren = DesugarExpressionList(Clause.GetChildren());
        if (DesugaredChildren.Num() > 1 && Clause.GetForm() == Vst::Clause::EForm::NoSemicolonOrNewline)
        {
            // If there are multiple comma separated subexpressions, wrap them in a uLang::CExprMakeTuple that is
            // the sole subexpression of the resulting code block.
            uLang::TSRef<uLang::CExprMakeTuple> Tuple = WrapExpressionListInTuple(uLang::Move(DesugaredChildren), Clause, false);
            DesugaredChildren = {Tuple};
        }

        return WrapExpressionListInCodeBlock(uLang::Move(DesugaredChildren), Clause, true);
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarParens(const Vst::Parens& Parens)
    {
        return DesugarExpressionListAsExpression(Parens, Parens.GetForm());
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarCommas(const Vst::Commas& Commas)
    {
        uLang::TArray<uLang::TSRef<uLang::CExpressionBase>> DesugaredChildren = DesugarExpressionList(Commas.GetChildren());
        ULANG_ASSERT(DesugaredChildren.Num() > 1);

        uLang::TSRef<uLang::CExprMakeTuple> Tuple = WrapExpressionListInTuple(uLang::Move(DesugaredChildren), Commas, true);

        // NOTE: (yiliang.siew) This preserves the mistake we shipped in `28.20` where mixed use of separators in
        // archetype instantiations wrapped the sub-expressions into an implicit `block`, but in other places, it
        // did not.
        if (!_Package
            || _Package->_EffectiveVerseVersion >= Verse::Version::DontMixCommaAndSemicolonInBlocks
            || VerseFN::UploadedAtFNVersion::EnforceDontMixCommaAndSemicolonInBlocks(_Package->_UploadedAtFNVersion))
        {
            return Tuple;
        }
        else
        {
            // NOTE: (yiliang.siew) This preserves the old legacy behaviour of potentially wrapping the expression in a
            // code block/tuple/returning a single expression directly.
            // This has implications on scoping (since blocks create their own scope and tuples do not) and how
            // definitions that might previously not have conflicted would conflict if we were not to do this.
            AppendGlitch(&Commas, uLang::EDiagnostic::WarnSemantic_StricterErrorCheck,
                "Mixing commas with semicolons/newlines in a clause wraps the comma-separated subexpressions in a 'block{...}' "
                "in the version of Verse you are targeting, but this behavior will change in a future version of Verse. You "
                "can preserve the current behavior in future versions of Verse by wrapping the comma-separated subexpressions "
                "in a block{...}.\n"
                "For example, instead of writing this:\n"
                "    A\n"
                "    B,\n"
                "    C\n"
                "Write this:\n"
                "    A\n"
                "    block:\n"
                "        B,\n"
                "        C");
            return WrapExpressionListInCodeBlock({ uLang::Move(Tuple) }, Commas, false);
        }
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarPlaceholder(const Vst::Placeholder& PlaceholderNode)
    {
        return AddMapping(PlaceholderNode, uLang::TSRef<uLang::CExprPlaceholder>::New());
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarEscape(const Vst::Escape& EscapeNode)
    {
        AppendGlitch(&EscapeNode, uLang::EDiagnostic::ErrSemantic_Unsupported, "Escaped syntax is not yet supported.");
        return AddMapping(EscapeNode, uLang::TSRef<uLang::CExprError>::New());
    }

    uLang::CSymbol VerifyAddSymbol(const Vst::Node* VstNode, const uLang::CUTF8StringView& Text)
    {
        uLang::TOptional<uLang::CSymbol> OptionalSymbol = _Symbols.Add(Text);
        if (!OptionalSymbol.IsSet())
        {
            AppendGlitch(VstNode, uLang::EDiagnostic::ErrSemantic_TooLongIdentifier);
            OptionalSymbol = _Symbols.Add(Text.SubViewBegin(uLang::CSymbolTable::MaxSymbolLength-1));
            ULANG_ASSERTF(OptionalSymbol.IsSet(), "Truncated name is to long");
        }
        return OptionalSymbol.GetValue();
    }

    template<typename... ResultArgsType>
    void AppendGlitch(const Vst::Node* VstNode, ResultArgsType&&... ResultArgs)
    {
        _Diagnostics.AppendGlitch(uLang::SGlitchResult(uLang::ForwardArg<ResultArgsType>(ResultArgs)...), uLang::SGlitchLocus(VstNode));
    }
    
    template<typename ExpressionType>
    uLang::TSRef<ExpressionType> AddMapping(const Vst::Node& VstNode, uLang::TSRef<ExpressionType>&& AstNode)
    {
        VstNode.AddMapping(AstNode.Get());
        return uLang::Move(AstNode);
    }
    
    uLang::TArray<uLang::SAttribute> DesugarAttributes(const uLang::TArray<Vst::TNodeRef<Vst::Node>>& AttributeVsts)
    {
        auto FilterAcceptAll = [](const Vst::Node&)->bool { return true; };

        return DesugarAttributesFiltered(AttributeVsts, FilterAcceptAll);
    }

    template<typename TPredicate>
    uLang::TArray<uLang::SAttribute> DesugarAttributesFiltered(const uLang::TArray<Vst::TNodeRef<Vst::Node>>& AttributeVsts, TPredicate FilterPredicate)
    {
        uLang::TArray<uLang::SAttribute> AttributeAsts;
        for (const Vst::TNodeRef<Vst::Node>& AttributeWrapperVst : AttributeVsts)
        {
            // the actual attribute node is wrapped in a dummy Clause (used to preserve comments
            // in the VST and tell us whether it's a prepend attribute or append specifier)
            ULANG_ASSERTF(AttributeWrapperVst->IsA<Vst::Clause>(), "attribute nodes are expected to be wrapped in a dummy Clause node with a single child");
            ULANG_ASSERTF(AttributeWrapperVst->GetChildCount() == 1, "attribute nodes are expected to be wrapped in a dummy Clause node with a single child");

            const Vst::Clause& AttributeClauseVst = AttributeWrapperVst->As<Vst::Clause>();

            uLang::SAttribute::EType AttributeType = uLang::SAttribute::EType::Attribute;
            switch(AttributeClauseVst.GetForm())
            {
            case Vst::Clause::EForm::IsPrependAttributeHolder:
                AttributeType = uLang::SAttribute::EType::Attribute;
                break;
            case Vst::Clause::EForm::IsAppendAttributeHolder:
                AttributeType = uLang::SAttribute::EType::Specifier;
                break;

            case Vst::Clause::EForm::Synthetic:
            case Vst::Clause::EForm::NoSemicolonOrNewline:
            case Vst::Clause::EForm::HasSemicolonOrNewline:
            default:
                ULANG_UNREACHABLE();
                break;
            }

            const Vst::Node& AttributeExprVst = *AttributeClauseVst.GetChildren()[0];

            if (FilterPredicate(AttributeExprVst))
            {
                uLang::TSRef<uLang::CExpressionBase> AttributeExprAst = DesugarExpressionVst(AttributeExprVst);

                uLang::SAttribute AttributeAst{ AttributeExprAst, AttributeType };
                AttributeAsts.Add(uLang::Move(AttributeAst));
            }
        }
        return AttributeAsts;
    }

    uLang::TSRef<uLang::CAstNode> DesugarVst(const Vst::Node& VstNode)
    {
        const Vst::NodeType NodeType = VstNode.GetElementType();
        switch (NodeType)
        {
        case Vst::NodeType::Project:                    return DesugarProject(static_cast<const Vst::Project&>(VstNode));
        case Vst::NodeType::Package:                    return DesugarPackage(static_cast<const Vst::Package&>(VstNode));
        case Vst::NodeType::Module:                     return DesugarModule(static_cast<const Vst::Module&>(VstNode));
        case Vst::NodeType::Snippet:                    return DesugarSnippet(static_cast<const Vst::Snippet&>(VstNode));
        case Vst::NodeType::Where:                      return DesugarWhere(static_cast<const Vst::Where&>(VstNode));
        case Vst::NodeType::Mutation:                   return DesugarMutation(static_cast<const Vst::Mutation&>(VstNode));
        case Vst::NodeType::Definition:                 return DesugarDefinition(static_cast<const Vst::Definition&>(VstNode));
        case Vst::NodeType::Assignment:                 return DesugarAssignment(static_cast<const Vst::Assignment&>(VstNode));
        case Vst::NodeType::BinaryOpLogicalOr:
        case Vst::NodeType::BinaryOpLogicalAnd:         return DesugarBinaryOpLogicalAndOr(VstNode);
        case Vst::NodeType::PrefixOpLogicalNot:         return DesugarPrefixOpLogicalNot(static_cast<const Vst::PrefixOpLogicalNot&>(VstNode));
        case Vst::NodeType::BinaryOpCompare:            return DesugarBinaryOpCompare(static_cast<const Vst::BinaryOpCompare&>(VstNode));
        case Vst::NodeType::BinaryOpAddSub:             return DesugarBinaryOp(static_cast<const Vst::BinaryOpAddSub&>(VstNode));
        case Vst::NodeType::BinaryOpMulDivInfix:        return DesugarBinaryOp(static_cast<const Vst::BinaryOpMulDivInfix&>(VstNode));
        case Vst::NodeType::BinaryOpRange:              return DesugarBinaryOpRange(static_cast<const Vst::BinaryOpRange&>(VstNode));
        case Vst::NodeType::BinaryOpArrow:              return DesugarBinaryOpArrow(static_cast<const Vst::BinaryOpArrow&>(VstNode));
        case Vst::NodeType::TypeSpec:                   return DesugarTypeSpec(static_cast<const Vst::TypeSpec&>(VstNode));
        case Vst::NodeType::PrePostCall:                return DesugarPrePostCall(static_cast<const Vst::PrePostCall&>(VstNode));
        case Vst::NodeType::Identifier:                 return DesugarIdentifier(static_cast<const Vst::Identifier&>(VstNode));
        case Vst::NodeType::Operator:                   goto unexpected_node_type; 
        case Vst::NodeType::FlowIf:                     return DesugarFlowIf(static_cast<const Vst::FlowIf&>(VstNode));
        case Vst::NodeType::IntLiteral:                 return DesugarIntLiteral(static_cast<const Vst::IntLiteral&>(VstNode));
        case Vst::NodeType::FloatLiteral:               return DesugarFloatLiteral(static_cast<const Vst::FloatLiteral&>(VstNode));
        case Vst::NodeType::CharLiteral:                return DesugarCharLiteral(static_cast<const Vst::CharLiteral&>(VstNode));
        case Vst::NodeType::StringLiteral:              return DesugarStringLiteral(static_cast<const Vst::StringLiteral&>(VstNode));
        case Vst::NodeType::PathLiteral:                return DesugarPathLiteral(static_cast<const Vst::PathLiteral&>(VstNode));
        case Vst::NodeType::Interpolant:                goto unexpected_node_type;
        case Vst::NodeType::InterpolatedString:         return DesugarInterpolatedString(static_cast<const Vst::InterpolatedString&>(VstNode));
        case Vst::NodeType::Lambda:                     return DesugarLambda(static_cast<const Vst::Lambda&>(VstNode));
        case Vst::NodeType::Control:                    return DesugarControl(static_cast<const Vst::Control&>(VstNode));
        case Vst::NodeType::Macro:                      return DesugarMacro(VstNode.As<Vst::Macro>());
        case Vst::NodeType::Clause:                     goto unexpected_node_type;
        case Vst::NodeType::Parens:                     return DesugarParens(static_cast<const Vst::Parens&>(VstNode));
        case Vst::NodeType::Commas:                     return DesugarCommas(static_cast<const Vst::Commas&>(VstNode));
        case Vst::NodeType::Placeholder:                return DesugarPlaceholder(static_cast<const Vst::Placeholder&>(VstNode));
        case Vst::NodeType::ParseError:                 goto unexpected_node_type;
        case Vst::NodeType::Escape:                     return DesugarEscape(static_cast<const Vst::Escape&>(VstNode));
        case Vst::NodeType::Comment:                    goto unexpected_node_type;
        default:
        unexpected_node_type:
            ULANG_ENSUREF(false, "Did not expect this node type (%s) in an expression context.", VstNode.GetElementName());
            return AddMapping(VstNode, uLang::TSRef<uLang::CExprError>::New()); // Return something so semantic analysis can continue
        }
    }

    uLang::TSRef<uLang::CExpressionBase> DesugarExpressionVst(const Vst::Node& VstNode)
    {
        uLang::TSRef<uLang::CAstNode> AstNode = DesugarVst(VstNode);
        if (AstNode->AsExpression())
        {
            uLang::TSRef<uLang::CExpressionBase> Expression = uLang::Move(AstNode.As<uLang::CExpressionBase>());
            if (VstNode.HasAttributes())
            {
                Expression->_Attributes = DesugarAttributes(VstNode.GetAux()->GetChildren());
            }
            return uLang::Move(Expression);
        }
        else
        {
            AppendGlitch(&VstNode, uLang::EDiagnostic::ErrSyntax_ExpectedExpression);
            uLang::TSRef<uLang::CExprError> ErrorExpr = uLang::TSRef<uLang::CExprError>::New();
            ErrorExpr->AppendChild(uLang::Move(AstNode));
            return AddMapping(VstNode, uLang::Move(ErrorExpr));
        }
    }

    uLang::CSymbolTable& _Symbols;
    uLang::CDiagnostics& _Diagnostics;
    uLang::CAstPackage* _Package = nullptr;
};
}

namespace uLang
{
uLang::TSRef<uLang::CAstProject> DesugarVstToAst(const Verse::Vst::Project& VstProject, uLang::CSymbolTable& Symbols, uLang::CDiagnostics& Diagnostics)
{
    CDesugarerImpl DesugarerImpl(Symbols, Diagnostics);
    return DesugarerImpl.DesugarProject(VstProject);
}
}
