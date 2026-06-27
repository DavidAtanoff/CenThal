// Copyright Epic Games, Inc. All Rights Reserved.

#include "uLang/VerseLocalizationGen.h"

#include "uLang/Common/Text/FilePathUtils.h"
#include "uLang/Diagnostics/Diagnostics.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Syntax/VstNode.h"
#include "uLang/Toolchain/CommandLine.h"

#include <cmath>
#include <cstdio>

namespace {
	namespace Private {
        struct FImpl : uLang::SAstVisitor
		{
			const uLang::CSemanticProgram& Program;
			uLang::CDiagnostics& Diagnostics;
            uLang::TArray<uLang::FSolLocalizationInfo>& LocalizationInfo;
            uLang::TArray<uLang::FSolLocalizationInfo>& StringInfo;

			explicit FImpl(const uLang::CSemanticProgram& Program,
                           uLang::CDiagnostics& Diagnostics,
                           uLang::TArray<uLang::FSolLocalizationInfo>& LocalizationInfo,
                           uLang::TArray<uLang::FSolLocalizationInfo>& StringInfo)
				: Program(Program)
				, Diagnostics(Diagnostics)
				, LocalizationInfo(LocalizationInfo)
                , StringInfo(StringInfo)
			{
			}

			void ScrapeProgram()
			{
				Visit(*Program._AstProject);
			}

			//-------------------------------------------------------------------------------------------------
			template<typename... ResultArgsType>
			void AppendGlitch(const uLang::CAstNode& AstNode, ResultArgsType&&... ResultArgs)
			{
				uLang::SGlitchResult Glitch(uLang::ForwardArg<ResultArgsType>(ResultArgs)...);
				if (AstNode.GetNodeType() == uLang::Cases<uLang::EAstNodeType::Context_Package, uLang::EAstNodeType::Definition_Module>
					&& (!AstNode.GetMappedVstNode() || !AstNode.GetMappedVstNode()->Whence().IsValid()))
				{
					Diagnostics.AppendGlitch(uLang::Move(Glitch), uLang::SGlitchLocus());
				}
				else
				{
					ULANG_ASSERTF(
						AstNode.GetMappedVstNode() && AstNode.GetMappedVstNode()->Whence().IsValid(),
						"Expected valid whence for node used as glitch locus on %s id:%i - %s",
						AstNode.GetErrorDesc().AsCString(),
						uLang::GetDiagnosticInfo(Glitch._Id).ReferenceCode,
						Glitch._Message.AsCString());
					Diagnostics.AppendGlitch(uLang::Move(Glitch), uLang::SGlitchLocus(&AstNode));
				}
			}

            //-------------------------------------------------------------------------------------------------
            void ScrapeString(const uLang::CExprString& StringAst)
            {
                uLang::SGlitchLocus GlitchLocus(&StringAst);
                StringInfo.Emplace(StringAst._String, GlitchLocus.AsFormattedString());
            }

			//-------------------------------------------------------------------------------------------------
			void ScrapeLocalization(const uLang::CExprDefinition& DefinitionAst)
			{
				uLang::TSPtr<uLang::CExpressionBase> Value = DefinitionAst.Value();
				if (!Value)
				{
                    /* We allowed this in an earlier version of the compiler in the case of a localization in an
                    * abstract class, and will allow it now until we have support for breaking changes for
                    * to-be-published projects, without breaking already published projects (SOL-5053).
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"Value is missing for localization.");
                    */
					return;
				}

				if (Value->GetNodeType() == uLang::EAstNodeType::External)
				{ // We are in a digest, and there is nothing to see here
					return;
				}
				if (Value->GetNodeType() != uLang::EAstNodeType::Invoke_Type)
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"Expected a type invocation here.");
					return;
				}

				Value = static_cast<uLang::CExprInvokeType&>(*Value)._Argument;

				if (Value->GetNodeType() != uLang::EAstNodeType::Invoke_Invocation)
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"Expected an invocation for localization.");
					return;
				}

				uLang::CExprInvocation& Invocation = static_cast<uLang::CExprInvocation&>(*Value);

				if (!Invocation.GetArgument())
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"No arguments for localization.");
					return;
				}

				if (Invocation.GetArgument()->GetNodeType() != uLang::EAstNodeType::Invoke_MakeTuple)
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"Expected a tuple for localization,");
					return;
				}

				uLang::CExprMakeTuple& MakeTuple = static_cast<uLang::CExprMakeTuple&>(*Invocation.GetArgument());

				if (MakeTuple.SubExprNum() < 2)
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"Too few arguments for localization");
					return;
				}

				if (!MakeTuple.GetSubExprs()[0])
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"No path for localization.");
					return;
				}

				if (MakeTuple.GetSubExprs()[0]->GetNodeType() != uLang::EAstNodeType::Literal_String)
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"Localization path must be a string at this point.");
					return;
				}
				const uLang::CUTF8String& Path = static_cast<uLang::CExprString&>(*MakeTuple.GetSubExprs()[0])._String;

				if (!MakeTuple.GetSubExprs()[0])
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"No default for localization.");
					return;
				}

				if (MakeTuple.GetSubExprs()[1]->GetNodeType() != uLang::EAstNodeType::Literal_String)
				{
					AppendGlitch(
						DefinitionAst,
						uLang::EDiagnostic::ErrSyntax_InternalError,
						"Localization default must be a string.");
					return;
				}
				const uLang::CUTF8String& Default = static_cast<uLang::CExprString&>(*MakeTuple.GetSubExprs()[1])._String;

				uLang::SGlitchLocus GlitchLocus(&DefinitionAst);
                LocalizationInfo.Emplace(Path, Default, GlitchLocus.AsFormattedString());
			}

			// uLang::SAstVisitor interface 
			void Visit(uLang::CAstNode& Node)
			{
				const uLang::EAstNodeType NodeType = Node.GetNodeType();
				if (NodeType == uLang::EAstNodeType::Definition_Function)
				{
					uLang::CExprFunctionDefinition& Function = static_cast<uLang::CExprFunctionDefinition&>(Node);
					if (Function._Function->HasAttributeClass(Program._localizes, Program))
					{
						ScrapeLocalization(Function);
					}
				}
				else if (NodeType == uLang::EAstNodeType::Definition_Data)
				{
					uLang::CExprDataDefinition& DataDefAst = static_cast<uLang::CExprDataDefinition&>(Node);
					if (DataDefAst._DataMember->HasAttributeClass(Program._localizes, Program))
					{
						ScrapeLocalization(DataDefAst);
					}
				}
                else if (NodeType == uLang::EAstNodeType::Literal_String)
                {
                    ScrapeString(static_cast<uLang::CExprString&>(Node));
                }
				Node.VisitChildren(*this);
			}

			virtual void Visit(const char* FieldName, uLang::CAstNode& AstNode) override { Visit(AstNode); }
			virtual void VisitElement(uLang::CAstNode& AstNode) override { Visit(AstNode); }
		};

	}
}

namespace uLang
{
void FVerseLocalizationGen::operator()(const uLang::CSemanticProgram& Program,
    uLang::CDiagnostics& Diagnostics,
    uLang::TArray<uLang::FSolLocalizationInfo>& LocalizationInfo,
    uLang::TArray<uLang::FSolLocalizationInfo>& StringInfo) const
{
    ::Private::FImpl Impl(Program, Diagnostics, LocalizationInfo, StringInfo);
    Impl.ScrapeProgram();
}
}

