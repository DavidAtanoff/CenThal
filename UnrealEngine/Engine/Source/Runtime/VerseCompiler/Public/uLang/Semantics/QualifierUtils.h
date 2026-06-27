// Copyright Epic Games, Inc. All Rights Reserved.

#include "uLang/Semantics/Expression.h"

namespace uLang
{
  TArray<Verse::Vst::TNodeRef<Verse::Vst::Node>> QualifyAllAnalyzedIdentifiers(bool bVerbose, const TSRef<CSemanticProgram>& Program, Verse::Vst::TNodeRef<Verse::Vst::Node>& Root);

  TArray<const CExpressionBase*> FindUnresolvedIdentifiers(const TSRef<CSemanticProgram>& /*Program*/,
                                                           const CAstNode& RootNode);

  bool VerifyAllQualified(const TSRef<CSemanticProgram>& Program);

  TArray<const CExpressionBase*> FindResolvedIdentifiersWithoutDefinitions(const TSRef<CSemanticProgram>&,
                                                                           const CAstNode& RootNode);
}
