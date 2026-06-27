// Copyright Epic Games, Inc. All Rights Reserved.

#include "uLang/Parser/ParserPass.h"
#include "uLang/Common/Common.h"
#include "uLang/Common/Misc/Optional.h"
#include "uLang/Common/Misc/Union.h"
#include "uLang/Common/Text/StringUtils.h"
#include "uLang/Common/Text/Unicode.h"
#include "uLang/CompilerPasses/CompilerTypes.h" // for SProgramContext, SBuildContext, etc.
#include "uLang/Parser/VerseGrammar.h"
#include "uLang/SourceProject/VerseVersion.h"

// TODO: (yiliang.siew) Should just fix these warnings in `VerseGrammar.h`.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wparentheses"
#pragma clang diagnostic ignored "-Wswitch-enum"
#pragma clang diagnostic ignored "-Wswitch-default"
#endif
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wswitch-enum"
#pragma GCC diagnostic ignored "-Wswitch-default"
#endif

#include "uLang/Syntax/VstNode.h"
#include "uLang/Syntax/vsyntax_types.h"

namespace uLang
{
using namespace Verse::Vst;
using SLocus = Verse::SLocus; // Verse::Grammar::SSnippet also stores text start
							  // and end character/index

template <typename T>
using TSRef = uLang::TSRef<T>;

//=================================================================================================
/// Storage for an accumulated capture of source text from a parse operation.
struct SGenerateVstCapture final
{
	/// The significant syntax nodes that have been created as part of the string
	/// being captured above.
	TArray<TNodeRef<Node>> Nodes; // Node::NodeArray seems to give memory leaks
								  // during Linux ASAN/UBSAN tests

	/// These are the full captures for the string that allows reconstruction of
	/// `String` from the contents here.
	TArray<TNodeRef<Node>> CaptureNodes;

	bool IsEmpty() const
	{
		return Start == Stop;
	}

	int32_t CountNumTrailingNewLines() const
	{
		CUTF8StringView View(Start, Stop);
		return uLang::CountNumTrailingNewLines(View);
	}

	int32_t CountNumLeadingNewLines() const
	{
		CUTF8StringView View(Start, Stop);
		return uLang::CountNumLeadingNewLines(View);
	}

	void Append(const SGenerateVstCapture& Other)
	{
		if (Other.IsEmpty())
		{
			return;
		}

		if (IsEmpty())
		{
			Start = Other.Start;
		}

		Stop = Other.Stop;
	}

	void Append(const Verse::Grammar::SText& Text)
	{
		const UTF8Char* const TextStart = reinterpret_cast<const UTF8Char*>(Text.Start);
		const UTF8Char* const TextStop = reinterpret_cast<const UTF8Char*>(Text.Stop);

		SGenerateVstCapture Other;
		Other.Start = TextStart;
		Other.Stop = TextStop;

		Append(Other);
	}

	void Reset()
	{
		Start = nullptr;
		Stop = nullptr;
	}

private:
	const UTF8Char* Start = nullptr;
	const UTF8Char* Stop = nullptr;
};

//=================================================================================================
struct SGenerateCommon
{
	// Common Types.
	using SyntaxType = TNodeRef<Node>;
	using SyntaxesType = Node::NodeArray;
	using ErrorType =
		TSPtr<SGlitch>; // Must use TSPtr<> rather than TSRef<> since
						// Verse::Grammar::TResult<> needs default constructor
	using CaptureType = SGenerateVstCapture;
};

//=================================================================================================
struct SGenerateVst : public Verse::Grammar::TGenerate<SGenerateCommon>
{
	enum class EParseBehaviour : int8_t
	{
		ParseAll,
		ParseNoComments // Allows for a slightly more optimized parse by skipping
						// comments.
	};

	using BlockType = Verse::Grammar::SBlock<SyntaxesType, CaptureType>;
	using ResultType = Verse::Grammar::TResult<SyntaxType, ErrorType>;
	using SToken = Verse::Grammar::SToken;
	using SSnippet = Verse::Grammar::SSnippet;
	using SText = Verse::Grammar::SText;
	using EPlace = Verse::Grammar::EPlace;
	using EMode = Verse::Grammar::EMode;

	//-------------------------------------------------------------------------------------------------
	SGenerateVst(const TSRef<uLang::CDiagnostics>& Diagnostics,
		const CUTF8String& SnippetPath,
		const EParseBehaviour ParseBehaviour,
		const uint32_t VerseVersion, const uint32_t UploadedAtFNVersion)
		: _Diagnostics(Diagnostics)
		, _SnippetPath(SnippetPath)
		, _ParseBehaviour(ParseBehaviour)
		, _VerseVersion(VerseVersion)
		, _UploadedAtFNVersion(UploadedAtFNVersion) {}

	//-------------------------------------------------------------------------------------------------
	static void SetClausePunctuation(const BlockType& InBlock, Clause& InClause)
	{
		switch (InBlock.Punctuation)
		{
			case Verse::Grammar::EPunctuation::Colon:
				InClause.SetPunctuation(Clause::EPunctuation::Colon);
				// Force a newline after the clause if it doesn't have one since otherwise
				// this is otherwise invalid syntax.
				if (!InClause.HasNewLinesAfter())
				{
					InClause.SetNewLineAfter(true);
				}
				break;
			case Verse::Grammar::EPunctuation::Braces:
				InClause.SetPunctuation(Clause::EPunctuation::Braces);
				break;
			case Verse::Grammar::EPunctuation::Ind:
				InClause.SetPunctuation(Clause::EPunctuation::Indentation);
				break;
			case Verse::Grammar::EPunctuation::Parens:
				ULANG_FALLTHROUGH;
			case Verse::Grammar::EPunctuation::Brackets:
				ULANG_FALLTHROUGH;
			case Verse::Grammar::EPunctuation::AngleBrackets:
				ULANG_FALLTHROUGH;
			case Verse::Grammar::EPunctuation::Qualifier:
				ULANG_FALLTHROUGH;
			case Verse::Grammar::EPunctuation::Dot:
				ULANG_FALLTHROUGH;
			case Verse::Grammar::EPunctuation::None:
				ULANG_FALLTHROUGH;
			default:
				InClause.SetPunctuation(Clause::EPunctuation::Unknown);
				break;
		}
	}

	//-------------------------------------------------------------------------------------------------
	static SLocus CombineLocus(const SyntaxesType& Nodes)
	{
		if (ULANG_ENSUREF(
				Nodes.IsFilled(),
				"No syntax nodes - cannot compute combined text range."))
		{
			SLocus Whence = Nodes[0]->Whence();
			for (int32_t Index = 1; Index < Nodes.Num(); ++Index)
			{
				Whence |= Nodes[Index]->Whence();
			}
			return Whence;
		}

		return SLocus();
	}

	//-------------------------------------------------------------------------------------------------
	static SLocus BlockElementsLocus(const BlockType& Block)
	{
		return Block.Elements.IsFilled() ? CombineLocus(Block.Elements)
										 : AsLocus(Block.BlockSnippet);
	}

	//-------------------------------------------------------------------------------------------------
	// Should have rest of system use `SToken` or more generic `uint8_t` since
	// that is what is used internally. Bridge between old and new parser. New
	// parser has more information that isn't reflected in old system so should
	// refactor.
	static vsyntax::res_t TokenToRes(const BlockType& Block)
	{
		// Ensures it is null terminated
		const CUTF8String TokenStr(AsStringView(Block.Token));

		switch (Token8(TokenStr))
		{
			case SToken(u8""):
				switch (Block.Punctuation)
				{
					case Verse::Grammar::EPunctuation::None:
						return vsyntax::res_none;
					case Verse::Grammar::EPunctuation::Parens:
						return vsyntax::res_of;
					case Verse::Grammar::EPunctuation::Brackets:
						return vsyntax::res_of;
					case Verse::Grammar::EPunctuation::Braces:
						return vsyntax::res_none; // `res_do` would seem more appropriate but
												  // using `res_none` for legacy code
					case Verse::Grammar::EPunctuation::Colon:
						return vsyntax::res_none; // `res_do` would seem more appropriate but
												  // using `res_none` for legacy code
					case Verse::Grammar::EPunctuation::AngleBrackets:
						return vsyntax::res_none;
					case Verse::Grammar::EPunctuation::Qualifier:
						return vsyntax::res_none;
					case Verse::Grammar::EPunctuation::Dot:
						return vsyntax::res_none;
					case Verse::Grammar::EPunctuation::Ind:
						return vsyntax::res_none;
					default:
						return vsyntax::res_none;
				}
			case SToken(u8"of"):
				return vsyntax::res_of;
			case SToken(u8"do"):
				return vsyntax::res_do;
			case SToken(u8"if"):
				return vsyntax::res_if;
			case SToken(u8"else"):
				return vsyntax::res_else;
			case SToken(u8"then"):
				return vsyntax::res_then;
			default:
				return vsyntax::res_none;
		}
	}

	//===============================================================================
	// Manipulation operations we must expose to parser.

	//-------------------------------------------------------------------------------------------------
	template <typename... MessageFragmentsType>
	ErrorType Err(const SSnippet& Location, const char* IssueIdCStr,
		MessageFragmentsType... MessageFragments) const
	{
		CUTF8StringBuilder Msg;
		Msg.Append("vErr:");
		Msg.Append(IssueIdCStr);
		Msg.Append(": ");

		// Concatenate message
		SText MessageFragmentsArray[] = {MessageFragments...};
		for (size_t FragmentIndex = 0; FragmentIndex < sizeof...(MessageFragments);
			 ++FragmentIndex)
		{
			const SText& Fragment = MessageFragmentsArray[FragmentIndex];
			Msg.Append(AsStringView(Fragment));
		}

		SGlitchLocus Locus(_SnippetPath, AsLocus(Location));
		SGlitchResult Result(uLang::EDiagnostic::ErrSyntax_InternalError,
			Msg.MoveToString());
		return TSRef<SGlitch>::New(Move(Result), Move(Locus));
	}

	//-------------------------------------------------------------------------------------------------
	static void SyntaxesAppend(SyntaxesType& As, const SyntaxType& A)
	{
		As.Push(A);
	}

	//-------------------------------------------------------------------------------------------------
	static uint64_t SyntaxesLength(const SyntaxesType& As) { return As.Num(); }

	//-------------------------------------------------------------------------------------------------
	static SyntaxType SyntaxesElement(const SyntaxesType& As, uint64_t i)
	{
		return As[static_cast<int32_t>(i)];
	}

	//-------------------------------------------------------------------------------------------------
	static void CaptureAppend(CaptureType& S, const CaptureType& T)
	{
		S.Append(T);

		for (const TNodeRef<Node>& Ref : T.Nodes)
		{
			TNodeRef<Node> NewRef = Ref;
			S.Nodes.Add(NewRef);
		}
	}

	//-------------------------------------------------------------------------------------------------
	static bool CaptureIsEmpty(const CaptureType& S)
	{
		return S.IsEmpty();
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Num(const SSnippet& Snippet, SText Digits, SText Fraction,
		SText ExponentSign, SText Exponent) const
	{
		CUTF8StringBuilder NumText;
		NumText.EnsureAllocatedExtra(static_cast<size_t>(3) // extra space
									 + Verse::Grammar::Length(Digits) + Verse::Grammar::Length(Fraction) + Verse::Grammar::Length(ExponentSign) + Verse::Grammar::Length(Exponent));

		NumText.Append(AsStringView(Digits));

		const bool bHasFraction = Verse::Grammar::Length(Fraction) > 0;
		if (bHasFraction)
		{
			NumText.Append('.');
			NumText.Append(AsStringView(Fraction));
		}

		const bool bHasExponent = Verse::Grammar::Length(Exponent) > 0;
		if (bHasExponent)
		{
			NumText.Append('e');
			NumText.Append(AsStringView(ExponentSign));
			NumText.Append(AsStringView(Exponent));
		}

		// Number literal
		if (!bHasFraction && !bHasExponent)
		{
			// It is an integer
			return TNodeRef<IntLiteral>::New(NumText, AsLocus(Snippet));
		}

		// It is a 64-bit float
		return TNodeRef<FloatLiteral>::New(NumText, FloatLiteral::EFormat::F64,
			AsLocus(Snippet));
	}

	//-------------------------------------------------------------------------------------------------
	ResultType NumHex(const SSnippet& Snippet, SText Digits) const
	{
		CUTF8StringBuilder HexString;
		HexString.Append("0x");
		HexString.Append(AsStringView(Digits));

		return TNodeRef<IntLiteral>::New(HexString, AsLocus(Snippet));
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Units(const SSnippet& Snippet, const SyntaxType& Num,
		SText Units) const
	{
		const SLocus Whence = AsLocus(Snippet);

		// Only called if Units has 1 or more characters
		switch (Units[0])
		{
			case 'f':
			{
				CUTF8StringView FloatFormatSuffix = AsStringView(Units);
				// advance the beginning past the 'f' format character
				FloatFormatSuffix._Begin++;

				// with an 'f' suffix we require Digits after the 'f'
				if (FloatFormatSuffix.IsEmpty())
				{
					return NewGlitch(Whence,
						EDiagnostic::ErrSyntax_UnrecognizedFloatBitWidth);
				}

				// is the remaining suffix all Digits?
				bool bIsAllDigits = true;
				const UTF8Char* ChU8 = FloatFormatSuffix._Begin;
				while (ChU8 != FloatFormatSuffix._End)
				{
					if (!CUnicode::IsDigitASCII(*ChU8))
					{
						bIsAllDigits = false;
						break;
					}

					ChU8++;
				}

				if (!bIsAllDigits)
				{
					return NewGlitch(
						Whence, EDiagnostic::ErrSyntax_Unimplemented,
						CUTF8String("Unrecognized suffix on number literal `%s%.*s`",
							Verse::PrettyPrintVst(Num).AsCString(),
							Verse::Grammar::Length(Units), Units.Start));
				}

				FloatLiteral::EFormat Format = FloatLiteral::EFormat::Unspecified;

				// NOTE: Currently only 64 bit-floats are supported, but there are tests
				// that test for 16/32 bit floating point literal parsing as well.
				// TODO: (yiliang.siew) Implement quick-fix support for this and other
				// trivial user-code problems in the Verse LSP.
				// https://jira.it.epicgames.com/browse/SOL-3247
				if (FloatFormatSuffix == "16")
				{
					Format = FloatLiteral::EFormat::F16;
				}
				else if (FloatFormatSuffix == "32")
				{
					Format = FloatLiteral::EFormat::F32;
				}
				else if (FloatFormatSuffix == "64")
				{
					Format = FloatLiteral::EFormat::F64;
				}
				else
				{
					return NewGlitch(
						Whence, EDiagnostic::ErrSyntax_UnrecognizedFloatBitWidth,
						CUTF8String("Unrecognized float literal bit width `%.*s` on number "
									"literal '%s'",
							FloatFormatSuffix.ByteLen(), FloatFormatSuffix._Begin,
							Verse::PrettyPrintVst(Num).AsCString()));
				}

				CUTF8StringBuilder NumStr;

				if (Num->GetElementType() == NodeType::FloatLiteral)
				{
					NumStr.Append(Num->As<FloatLiteral>().GetSourceText());
				}
				else if (Num->GetElementType() == NodeType::IntLiteral)
				{
					// A previously int literal will be converted to a float literal
					NumStr.Append(Num->As<IntLiteral>().GetSourceText());
				}
				else
				{
					return NewGlitch(
						Whence, EDiagnostic::ErrSyntax_UnrecognizedFloatBitWidth,
						CUTF8String("float suffix `%.*s` on unexpected non-number `%s`",
							FloatFormatSuffix.ByteLen(), FloatFormatSuffix._Begin,
							Verse::PrettyPrintVst(Num).AsCString()));
				}

				NumStr.Append(AsStringView(Units));
				return TNodeRef<FloatLiteral>::New(NumStr, Format, Num->Whence() | Whence);
			}

			case 'r':
				return NewGlitch(
					Whence, EDiagnostic::ErrSyntax_Unimplemented,
					CUTF8String("Rational number literal `%s%.*s` is not yet supported",
						Verse::PrettyPrintVst(Num).AsCString(),
						Verse::Grammar::Length(Units), Units.Start));

			case 'c':
				return NewGlitch(
					Whence, EDiagnostic::ErrSyntax_Unimplemented,
					CUTF8String("ASCII/UTF8 character uses `0o` as prefix followed by "
								"hexidecimal value - `%s%.*s` is not supported",
						Verse::PrettyPrintVst(Num).AsCString(),
						Verse::Grammar::Length(Units), Units.Start));

			default:
				// Units is unrecognized
				return NewGlitch(
					Whence, EDiagnostic::ErrSyntax_Unimplemented,
					CUTF8String("Unrecognized suffix on number literal `%s%.*s`",
						Verse::PrettyPrintVst(Num).AsCString(),
						Verse::Grammar::Length(Units), Units.Start));
		}
	}

	//-------------------------------------------------------------------------------------------------
	// Macro invocation m{a}, m(a){b}, etc
	//   - Macro: expr/identifier name of macro
	//   - Clause1: Usually (arguments) of macro when two+ clauses or do {body} if
	//   one clause
	//   - Clause2: Usually {body} of macro - including `then` clause in `if`
	//   - Clause3: Usually additional {body} of macro - such as `else` clause in
	//   `if`
	ResultType Invoke(const SSnippet& Snippet, const SyntaxType& MacroCommand,
		const BlockType& Clause1, const BlockType* Clause2,
		const BlockType* Clause3) const
	{
		// Each clause block has context info:
		//   - token: optional token name before opening punctuation
		//   - punctuation:
		//   {None,Braces,Parens,Brackets,AngleBrackets,Qualifier,Dot,Colon,Ind}
		//   - form: {Commas,List}

		// Invoke() / Call() specifier key:
		//
		//   macro0<spec2>()                  # Call(Call("macro0", "<spec2>), "()")
		//   macro1()<spec4>                  # Call(Call("macro1", "()"),
		//   "<spec4>") macro2<spec2>()<spec4>           # Call(Call(Call("macro2",
		//   "<spec2>), "()"), "<spec4>") macro3<spec3>{}                  #
		//   Invoke("macro3", "<spec3>{}") macro4{}<spec4>                  #
		//   Call(Invoke("macro3", "{}"), "<spec4>") macro5<spec3>{}<spec4> #
		//   Call(Invoke("macro3", "<spec3>{}"), "<spec4>") macro6<spec2>(){} #
		//   Invoke("macro3", "<spec2>()", "{}") macro7()<spec3>{}                #
		//   Invoke("macro3", "()", "<spec3>{}") macro8<spec2>()<spec3>{}         #
		//   Invoke("macro3", "<spec2>()", "<spec3>{}")
		//   macro9<spec2>()<spec3>{}<spec4>  # Call(Invoke("macro3", "<spec2>()",
		//   "<spec3>{}"), "<spec4>")
		//
		// *Notes: `Call()` with angle brackets becomes `AppendSpecifier()`
		ClauseArray Clauses;
		Clauses.Reserve(3);

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Group first clause and process their specifiers
		// Append any specifiers
		if (Clause1.Specifiers.IsFilled())
		{
			// Rather than applying the specifiers to the block itself (or the whole
			// macro invocation), they are applied to the MacroCommand expression
			// preceding it which is usually an identifier.
			AppendSpecifiers(MacroCommand, Clause1.Specifiers);
		}
		TNodeRef<Clause> ArgClauseNode = TNodeRef<Clause>::New(
			uint8_t(TokenToRes(Clause1)), AsLocus(Clause1.BlockSnippet),
			AsClauseForm(Clause1));
		SetClausePunctuation(Clause1, *ArgClauseNode);
		ArgClauseNode->AppendChildren(Clause1.Elements);
		// For the cases of empty clauses, we still want to suffix trailing
		// whitespace/comments to them.
		ProcessBlockPunctuationForClause(Clause1, ArgClauseNode);
		Clauses.Add(ArgClauseNode);

		// TODO: (yiliang.siew) This HACK is because the pretty-printer did not
		// account for newlines before. Therefore, we transfer any newlines before
		// to the clause as a line after instead so that the pretty-printer can
		// understand whether vertical forms are desired.
		TransferFirstLeadingNewLineOfClauseMember(*ArgClauseNode, *ArgClauseNode);

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Group optional second clause
		if (Clause2)
		{
			// Append any specifiers
			if (Clause2->Specifiers.IsFilled())
			{
				// Rather than applying the specifiers to the block itself (or the whole
				// macro invocation), they are applied to the clause expression
				// preceding it which is usually an identifier.
				AppendSpecifiers(ArgClauseNode, Clause2->Specifiers);
			}

			vsyntax::res_t Reserved = TokenToRes(*Clause2);
			// Should essentially be the same as Clause1 above
			TNodeRef<Clause> DoClauseNode =
				TNodeRef<Clause>::New(uint8_t(Reserved), AsLocus(Clause2->BlockSnippet),
					AsClauseForm(*Clause2));
			SetClausePunctuation(*Clause2, *DoClauseNode);
			DoClauseNode->AppendChildren(Clause2->Elements);
			ProcessBlockPunctuationForClause(*Clause2, DoClauseNode);
			// TODO: (yiliang.siew) This HACK is because the pretty-printer did not
			// account for newlines before. Therefore, we transfer any newlines before
			// to the clause as a line after instead so that the pretty-printer can
			// understand whether vertical forms are desired. Here we transfer the
			// newline to the clause directly preceding it.
			TransferFirstLeadingNewLineOfClauseMember(*DoClauseNode, *ArgClauseNode);

			Clauses.Add(DoClauseNode);
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Group optional third clause
		if (Clause3)
		{
			// Append any specifiers
			if (Clause3->Specifiers.IsFilled())
			{
				// Rather than applying the specifiers to the block itself (or the whole
				// macro invocation), they are applied to the clause expression
				// preceding it which is usually an identifier.
				AppendSpecifiers(Clauses.Last(), Clause3->Specifiers);
			}

			// Should essentially be the same as Clause1 above
			TNodeRef<Clause> PostClauseNode = TNodeRef<Clause>::New(
				uint8_t(TokenToRes(*Clause3)), AsLocus(Clause3->BlockSnippet),
				AsClauseForm(*Clause3));
			SetClausePunctuation(*Clause3, *PostClauseNode);
			PostClauseNode->AppendChildren(Clause3->Elements);
			ProcessBlockPunctuationForClause(*Clause3, PostClauseNode);

			// TODO: (yiliang.siew) This HACK is because the pretty-printer did not
			// account for newlines before. Therefore, we transfer any newlines before
			// to the clause as a line after instead so that the pretty-printer can
			// understand whether vertical forms are desired. Here we transfer the
			// newline to the clause directly preceding it.
			if (Clause2)
			{
				TransferFirstLeadingNewLineOfClauseMember(*PostClauseNode, *Clauses[1]);
			}
			else
			{
				TransferFirstLeadingNewLineOfClauseMember(*PostClauseNode,
					*ArgClauseNode);
			}

			// NOTE: (yiliang.siew) For `else` clauses, this helps to catch comments
			// that lead the `else` token, such as:
			/*
			 * ```
			 * if (1 = 1):
			 *     4
			 * <#comment#>else:
			 *     7
			 * ```
			 *
			 */
			if (Clause3->Token == "else" && !Clause3->TokenLeading.IsEmpty())
			{
				// TODO: (yiliang.siew) This is a little tricky because it's not clear
				// how to handle newlines within the leading token punctuation
				// appropriately in this case. We're just dealing with comments for now.
				for (const TNodeRef<Node>& CurNode : Clause3->TokenLeading.Nodes)
				{
					if (CurNode->IsA<Verse::Vst::Comment>())
					{
						PostClauseNode->AppendPrefixComment(CurNode);
					}
				}
			}

			Clauses.Add(PostClauseNode);
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Invocation of an expression other than an identifier
		SLocus Whence = AsLocus(Snippet);

		if (!MacroCommand->IsA<Identifier>())
		{
			return TNodeRef<Macro>::New(Whence, MacroCommand, Clauses);
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Invocation of an identifier
		const Identifier& MacroIdentifier = MacroCommand->As<Identifier>();
		const CUTF8String& MacroStr = MacroIdentifier.GetSourceText();

		if (MacroStr == CUTF8StringView("stub"))
		{
			TNodePtr<Placeholder> NewPlaceholderVstNode;

			if (Clauses.IsFilled() && Clauses[0]->GetChildren().IsFilled())
			{
				CUTF8String tx = Verse::PrettyPrintVst(Clauses[0]->GetChildren()[0]);
				NewPlaceholderVstNode = TNodeRef<Placeholder>::New(tx, Whence);
			}
			else
			{
				NewPlaceholderVstNode = TNodeRef<Placeholder>::New(Whence);
			}

			return NewPlaceholderVstNode.AsRef();
		}

		if (MacroStr == CUTF8StringView("if"))
		{
			TNodePtr<Clause> ConditionalClause;
			const TNodeRef<FlowIf> NewIfNode = TNodeRef<FlowIf>::New(Whence);
			const TNodeRef<Clause> IfIdentifierClause =
				TNodeRef<Clause>::New(MacroCommand->Whence(), Clause::EForm::Synthetic);
			NewIfNode->AddIfIdentifier(IfIdentifierClause);
			// We must also transfer the comments over from the `if` identifier to the
			// newly-created clause.
			IfIdentifierClause->AppendPrefixComments(
				MacroCommand->GetPrefixComments());
			IfIdentifierClause->AppendPostfixComments(
				MacroCommand->GetPostfixComments());
			bool bConditionClause = false;
			bool bThenClause = false;
			bool bElseClause = false;

			for (const TNodeRef<Clause>& CurrentClause : Clauses)
			{
				// Treat any clause after `else` encountered as unexpected otherwise
				// handle known clause tags
				vsyntax::res_t ClauseTag =
					bElseClause ? vsyntax::res_t::res_max
								: CurrentClause->GetTag<vsyntax::res_t>();
				// Using `if` chain rather than `switch` to avoid static compilation
				// error
				if (ClauseTag == vsyntax::res_of)
				{
					// argument block - `if (block)` or `if of: block`
					// - must be first block: error if condition block, then block or else
					// block already encountered Multiple condition clause or condition
					// clause after then block already prevented by syntax parser
					NewIfNode->AddCondition(CurrentClause);
					bConditionClause = true;
				}
				else if (ClauseTag == vsyntax::res_then)
				{
					// explicit `then` block - `if (condition[]) then: block` or without
					// initial brackets `if: condition[] then: block`
					// - error if no condition yet and no then block or else block already
					// encountered
					if (!bConditionClause)
					{
						// Missing condition [Could be moved to Semantic Analysis]
						AppendGlitch(CurrentClause->Whence(),
							EDiagnostic::ErrSyntax_ExpectedIfCondition,
							"Expected a condition block before `then` block while "
							"parsing `if`.");
					}

					if (bThenClause)
					{
						// Already present [Could be moved to Semantic Analysis]
						AppendGlitch(
							CurrentClause->Whence(),
							EDiagnostic::ErrSyntax_UnexpectedClauseTag,
							"Found more than one `then` block while parsing `if`.");
					}

					NewIfNode->AddBody(CurrentClause);
					bThenClause = true;
				}
				else if (ClauseTag == vsyntax::res_none)
				{
					// main block - after initial brackets `if (condition[]): block` or
					// without initial brackets `if: block` if no condition yet then
					// condition block otherwise then block
					// - error if then block or else block already encountered
					if (!bConditionClause)
					{
						// Treat main block as a condition block
						// Already having a `then` block should not be possible with a
						// `none` tag

						NewIfNode->AddCondition(CurrentClause);
						bConditionClause = true;
						continue;
					}

					if (!bThenClause)
					{
						// Treat main block as a `then` block
						NewIfNode->AddBody(CurrentClause);
						bThenClause = true;
						continue;
					}

					// Both condition block and then block already present [seems
					// impossible with syntax from parser]
					AppendGlitch(
						CurrentClause->Whence(),
						EDiagnostic::ErrSyntax_UnexpectedClauseTag,
						"Expected either condition block or then block to be unspecified "
						"though both are present while parsing `if`.");
				}
				else if (ClauseTag == vsyntax::res_else)
				{
					// `else` block
					// - error if no condition yet and must be last block: no else block
					// already encountered
					if (!bConditionClause)
					{
						// Missing condition [Could be moved to Semantic Analysis]
						AppendGlitch(CurrentClause->Whence(),
							EDiagnostic::ErrSyntax_ExpectedIfCondition,
							"Expected a condition block before `else` block while "
							"parsing `if`.");
					}

					// If it is an `else if` then flatten it into this `if` as a
					// multi-then clause `if`
					if ((CurrentClause->GetChildCount() == 1) && CurrentClause->GetChildren()[0]->IsA<FlowIf>())
					{
						// NOTE: (yiliang.siew) We also have to transfer any comments here
						// as part of the flattening process.
						const TNodeRef<Node> FlowIfNode = CurrentClause->GetChildren()[0];
						// NOTE: (yiliang.siew) This is the condition clause of the "else
						// if" token.
						if (FlowIfNode->GetChildCount() == 0)
						{
							AppendGlitch(
								CurrentClause->Whence(),
								EDiagnostic::ErrSyntax_ExpectedIfCondition,
								"Expected a condition block for an `else if` statement.");
						}
						else
						{
							const TNodeRef<Node> ClauseToTransferTo =
								FlowIfNode->GetChildren()[0];
							Node::TransferPrefixComments(CurrentClause, ClauseToTransferTo);
							Node::TransferPostfixComments(CurrentClause, ClauseToTransferTo);
						}
						// Note that any `if` children discovered here are already in the
						// form desired, so just append them to the current `if` node. Note
						// that the nested `if` may not have an `else` block itself so the
						// flattened multi-`if` may also have no `else` block.
						const TNodeRef<Node> NestedIf = CurrentClause->TakeChildAt(0);
						Node::TransferChildren(NestedIf, NewIfNode);
					}
					else
					{
						// standard else block
						NewIfNode->AddElseBody(CurrentClause);
					}

					bElseClause = true;
				}
				else
				{
					// Skip unexpected clause and accumulate error
					AppendGlitch(
						CurrentClause->Whence(),
						EDiagnostic::ErrSyntax_UnexpectedClauseTag,
						CUTF8String(
							"Unexpected `%s` clause while parsing `if`.",
							vsyntax::scan_reserved_t()[CurrentClause
														   ->GetTag<vsyntax::res_t>()]));
				}
			}
			return NewIfNode;
		}
		return TNodeRef<Macro>::New(Whence, MacroCommand, Clauses);
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Native(const SSnippet& Snippet, const SText& Name) const
	{
		return TNodeRef<Identifier>::New(AsStringView(Name), AsLocus(Snippet));
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Native(const SSnippet& Snippet, const char* NameCStr) const
	{
		return TNodeRef<Identifier>::New(CUTF8StringView(NameCStr), AsLocus(Snippet));
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Ident(const SSnippet& Snippet, const SText& NameA,
		const SText& NameB, const SText& NameC) const
	{
		CUTF8StringBuilder Name;
		Name.EnsureAllocatedExtra(
			(Verse::Grammar::Length(NameA) + Verse::Grammar::Length(NameB)) | Verse::Grammar::Length(NameC));
		Name.Append(AsStringView(NameA));
		Name.Append(AsStringView(NameB));
		Name.Append(AsStringView(NameC));

		return TNodeRef<Identifier>::New(Name, AsLocus(Snippet));
	}

	//-------------------------------------------------------------------------------------------------
	ResultType QualIdent(const SSnippet& Snippet, const BlockType& QualifierBlock,
		SText Name) const
	{
		if (!QualifierBlock.PunctuationLeading.IsEmpty())
		{
			const int32_t NumLeadingNewLines =
				QualifierBlock.PunctuationLeading.CountNumTrailingNewLines();
			const TNodeRef<Node>& FirstNodeInQualifier = QualifierBlock.Elements[0];
			FirstNodeInQualifier->SetNumNewLinesBefore(
				FirstNodeInQualifier->NumNewLinesBefore() + NumLeadingNewLines);
			for (const TNodeRef<Node>& CurNode :
				QualifierBlock.PunctuationLeading.Nodes)
			{
				if (CurNode->IsA<Verse::Vst::Comment>())
				{
					FirstNodeInQualifier->AppendPrefixComment(CurNode);
				}
			}
		}

		if (!QualifierBlock.ElementsTrailing.IsEmpty() && !QualifierBlock.Elements.IsEmpty())
		{
			const int32_t NumTrailingNewLines = QualifierBlock.ElementsTrailing.CountNumLeadingNewLines();
			const TNodeRef<Node>& LastNodeInQualifier = QualifierBlock.Elements.Last();
			LastNodeInQualifier->SetNumNewLinesAfter(
				LastNodeInQualifier->NumNewLinesAfter() + NumTrailingNewLines);
			for (const TNodeRef<Node>& CurNode : QualifierBlock.ElementsTrailing.Nodes)
			{
				if (CurNode->IsA<Verse::Vst::Comment>())
				{
					LastNodeInQualifier->AppendPostfixComment(CurNode);
				}
			}
		}

		if (QualifierBlock.Form == Verse::Grammar::EForm::List && QualifierBlock.Elements.Num() > 1)
		{
			return NewGlitch(AsLocus(Snippet), EDiagnostic::ErrSyntax_Unimplemented,
				"Semicolons and newlines in qualified identifiers are "
				"not yet implemented.");
		}

		// Translate qualified identifiers to an identifier with the qualifiers as
		// children.
		TNodeRef<Identifier> Result =
			TNodeRef<Identifier>::New(AsStringView(Name), AsLocus(Snippet));
		Result->AppendChildren(QualifierBlock.Elements);

		// NOTE: (yiliang.siew) Again, we're purposely re-jiggering the comments
		// here from trailing the expression to leading the identifier instead so
		// that the pretty-printer will print things in the right order.
		if (!QualifierBlock.PunctuationTrailing.IsEmpty())
		{
			const int32_t NumLeadingNewLines = QualifierBlock.ElementsTrailing.CountNumLeadingNewLines();
			Result->SetNumNewLinesAfter(NumLeadingNewLines);
			for (const TNodeRef<Node>& CurNode :
				QualifierBlock.PunctuationTrailing.Nodes)
			{
				if (CurNode->IsA<Verse::Vst::Comment>())
				{
					Result->AppendPrefixComment(CurNode);
				}
			}
		}

		return Result;
	}

	//-------------------------------------------------------------------------------------------------
	ResultType PrefixAttribute(const SSnippet& Snippet,
		const SyntaxType& Attribute,
		const SyntaxType& Base) const
	{
		PrependAttributeNode(Snippet, Attribute, Base);
		return Base;
	}

	//-------------------------------------------------------------------------------------------------
	ResultType PostfixAttribute(const SSnippet& Snippet, const SyntaxType& Base,
		const SyntaxType& Attribute) const
	{
		return NewGlitch(AsLocus(Snippet), EDiagnostic::ErrSyntax_Unimplemented,
			"Postfixed attributes are not yet supported.");
	}

	//-------------------------------------------------------------------------------------------------
	// This is near the top-level entry point for parsing the entire snippet.
	TNodeRef<Clause> File(const BlockType& Block) const
	{
		SyntaxesType Result = Block.Elements;
		// NOTE: (yiliang.siew) For any comments remaining that haven't been added,
		// just add them as block-level comments after everything else.
		// TODO: (yiliang.siew) This doesn't take newlines leading the nodes here
		// into account, nor newlines between these nodes.
		for (const TNodeRef<Node>& TrailingNode : Block.ElementsTrailing.Nodes)
		{
			Result.Add(TrailingNode);
		}

		const TNodeRef<Clause> BlockAsClause = TNodeRef<Clause>::New(
			Result, BlockElementsLocus(Block), AsClauseForm(Block));

		return BlockAsClause;
	}

	TNodeRef<Clause> MakeParameterClause(const BlockType& CallBlock) const
	{
		// This handles adding comments for things like `F(G<#Comment#>) := 0`
		Node::NodeArray FinalCallBlockElements;
		bool bUseMutatedBlockElements = false;
		if (!CallBlock.ElementsTrailing.Nodes.IsEmpty())
		{
			const int32_t NumCallBlockElements = CallBlock.Elements.Num();
			FinalCallBlockElements.Reserve(NumCallBlockElements + CallBlock.ElementsTrailing.Nodes.Num());
			FinalCallBlockElements = CallBlock.Elements;
			// If there are no elements inside the call block (i.e. `(<#Comment#>)`),
			// we'll add any trailing nodes as block-level items regardless of what
			// they are, since that is the most appropriate.
			if (NumCallBlockElements == 0)
			{
				for (const TNodeRef<Node>& CurNode : CallBlock.ElementsTrailing.Nodes)
				{
					if (CurNode->IsA<Verse::Vst::Comment>())
					{
						FinalCallBlockElements.Add(CurNode);
					}
				}
			}
			// If there _are_other elements inside the call block and these are
			// trailing it, we suffix them to the last node in the call block. For now
			// only comments are supported.
			else
			{
				const TNodeRef<Node>& LastElementInCallBlock = CallBlock.Elements.Last();
				for (const TNodeRef<Node>& CurNode : CallBlock.ElementsTrailing.Nodes)
				{
					if (CurNode->IsA<Verse::Vst::Comment>())
					{
						LastElementInCallBlock->AppendPostfixComment(CurNode);
					}
				}
			}
			bUseMutatedBlockElements = true;
		}

		// Is there a way in which CallBlock.Specifiers may be filled - and then
		// processed?
		return TNodeRef<Clause>::New(
			bUseMutatedBlockElements ? FinalCallBlockElements : CallBlock.Elements,
			AsLocus(CallBlock.BlockSnippet), AsClauseForm(CallBlock));
	}

	//-------------------------------------------------------------------------------------------------
	// EMode::Open   - Call function that cannot fail: Func(X) / Func of X
	// EMode::Closed   - Call function that may fail:    Func[X] / Func at X
	// EMode::With - Attach specifier to expression: Expr<specifier> / Expr with
	// specifier EMode::None - error if instantiated
	ResultType Call(const SSnippet& Snippet, EMode Mode,
		const SyntaxType& ReceiverSyntax,
		const BlockType& CallBlock) const
	{
		// Re categorize Mode:with `<>` as syntax element with appended specifier
		if (Mode == EMode::With)
		{
			AppendSpecifier(ReceiverSyntax, CallBlock);
			return ReceiverSyntax;
		}
		const SLocus Whence = AsLocus(Snippet);
		const bool bCanFail = (Mode == EMode::Closed); // Func[]

		TNodeRef<Clause> ParametersClause = MakeParameterClause(CallBlock);

		if (ReceiverSyntax->IsA<PrePostCall>())
		{
			// Member Access Chaining Transform
			// Convert from:
			//    call( PPC(a,'.',b,'.',c), arg1, arg2, arg3 )
			// to:
			//    PPC( a,'.', b, '.', c, clause(arg1, arg2, arg3))
			const auto& PpcChain = ReceiverSyntax.As<PrePostCall>();
			if (auto Aux = PpcChain->GetAux())
			{
				auto ChildCount = PpcChain->GetChildCount();
				ULANG_ASSERTF(ChildCount > 0, "Invalid PrePostCall");
				auto LastChild = PpcChain->GetChildren()[ChildCount - 1];
				LastChild->AppendAux(Aux->TakeChildren());
				PpcChain->RemoveAux();
			}

			// NOTE: (yiliang.siew) Because any postfix comments were originally
			// appended in `Trailing`, like for the syntax:
			/*
			 * ```
			 * A.foo<# comment #>(1)
			 * ```
			 *
			 * The `PrePostCall` node of `foo` would have the `comment` suffixed to
			 * it, since this was done before we added the clause of the argument
			 * block (i.e. `(1)`). Therefore in order to maintain this correct
			 * association for the pretty-printer, we transfer any postfix comments
			 * from the `PrePostCall` node of `foo` (which encompasses the entirety of
			 * `foo(1)`) to be suffixed to the identifier of `foo` itself.
			 */
			Node::TransferPostfixComments(ReceiverSyntax,
				ReceiverSyntax->AccessChildren().Last());
			// Now that the comments have been transferred, we can append the
			// parameters clause.
			PpcChain->AppendCallArgs(bCanFail, ParametersClause);
			PpcChain->CombineWhenceWith(Whence);

			return PpcChain;
		}
		else
		{
			const auto NewCall =
				TNodeRef<PrePostCall>::New(Whence | ReceiverSyntax->Whence());
			NewCall->AppendChild(ReceiverSyntax);
			NewCall->AppendCallArgs(bCanFail, ParametersClause);

			return NewCall;
		}
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Parenthesis(const BlockType& Block) const
	{
		return BlockAsSingleExpression(Block);
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Char8(const SSnippet& Snippet, char8_t Char8) const
	{
		return TNodeRef<CharLiteral>::New(CharLiteral::EFormat::UTF8CodeUnit,
			(char32_t)Char8, AsLocus(Snippet));
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Char32(const SSnippet& Snippet, char32_t Char32, bool bCode,
		bool bBackslash) const
	{
		// Produce an error for Unicode surrogate code points, which are not Unicode
		// scalars.
		if (Char32 >= 0xD800 && Char32 <= 0xDFFF)
		{
			return NewGlitch(
				AsLocus(Snippet),
				EDiagnostic::ErrSyntax_CharacterLiteralIsNotUnicodeScalar);
		}

		CharLiteral::EFormat Format;
		if (bBackslash)
		{
			ULANG_ASSERT(!bCode && Char32 <= 0x7F);
			Format = CharLiteral::EFormat::EscapedCode;
		}
		else if (bCode)
		{
			Format = CharLiteral::EFormat::UnicodeScalarCode;
		}
		else
		{
			Format = Char32 <= 0x7F ? CharLiteral::EFormat::ASCII
									: CharLiteral::EFormat::UnicodeScalar;
		}

		return TNodeRef<CharLiteral>::New(Format, Char32, AsLocus(Snippet));
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Path(const SSnippet& Snippet, SText Value) const
	{
		return TNodeRef<PathLiteral>::New(AsStringView(Value), AsLocus(Snippet));
	}

	//-------------------------------------------------------------------------------------------------
	ResultType Escape(const SSnippet& Snippet, const SyntaxType& Escaped) const
	{
		return Verse::Vst::TNodeRef<Verse::Vst::Escape>::New(AsLocus(Snippet), Escaped);
	}

	//-------------------------------------------------------------------------------------------------
	// Literal string span within quoted string or markup
	ResultType StringLiteral(const SSnippet& Snippet,
		const CaptureType& String) const
	{
		CUTF8StringBuilder Literal;
		// Note that Snippet is just the string - and not any surrounding double
		// quotes.
		for (const Verse::Vst::TNodeRef<Verse::Vst::Node>& CurrentNode : String.CaptureNodes)
		{
			if (CurrentNode->IsA<Verse::Vst::Comment>())
			{
				if (_VerseVersion >= Verse::Version::CommentsAreNotContentInStrings)
				{
					continue;
				}
				else
				{
					AppendGlitch(CurrentNode->Whence(),
						EDiagnostic::WarnParser_CommentsAreNotContentInStrings);
				}
			}
			if (const Verse::Vst::CAtom* SyntaxElement =
					CurrentNode->AsAtomNullable();
				SyntaxElement)
			{
				Literal.Append(SyntaxElement->GetSourceText());
			}
		}
		return Verse::Vst::TNodeRef<Verse::Vst::StringLiteral>::New(AsLocus(Snippet),
			Literal.ToStringView());
	}

	//-------------------------------------------------------------------------------------------------
	// Form string from StringLiteral, StringInterpolate
	ResultType String(const SSnippet& Snippet,
		const SyntaxesType& Splices) const
	{
		// Special case empty or literal strings without any interpolants to just
		// produce a StringLiteral node.
		if (Splices.Num() == 0)
		{
			return Verse::Vst::TNodeRef<Verse::Vst::StringLiteral>::New(AsLocus(Snippet), "");
		}
		else if (Splices.Num() == 1 && Splices[0]->IsA<Verse::Vst::StringLiteral>())
		{
			return Splices[0];
		}

		// Note that Snippet includes any surrounding double quotes so crop so that
		// it is similar to `StringLiteral()`
		SSnippet UnquotedSnippet = CropSnippet1(Snippet);
		SLocus UnquotedLocus = AsLocus(UnquotedSnippet);

		// Wrap in a InterpolatedString node so extra processing can be done on it
		const TNodeRef<InterpolatedString> InterpolatedStringNode =
			TNodeRef<InterpolatedString>::New(UnquotedLocus,
				AsStringView(UnquotedSnippet.Text));
		InterpolatedStringNode->AppendChildren(Splices);

		return InterpolatedStringNode;
	}

	//-------------------------------------------------------------------------------------------------
	// Interpolation expression within quoted string or markup
	ResultType StringInterpolate(const SSnippet& Snippet, EPlace Place,
		bool bBrace, const BlockType& Block) const
	{
		SSnippet UnquotedSnippet = CropSnippet1(Snippet);

		TNodeRef<Interpolant> InterpolantNode = TNodeRef<Interpolant>::New(
			AsLocus(Snippet), AsStringView(UnquotedSnippet.Text));
		InterpolantNode->AppendChild(MakeParameterClause(Block));
		return InterpolantNode;
	}

	//-------------------------------------------------------------------------------------------------
	// Span of text whose meaning is defined by place
	void Text(CaptureType& Capture, const SSnippet& Snippet, EPlace Place) const
	{
		// NOTE: (yiliang.siew) We capture the strings here temporarily as nodes, so
		// that we have locus information when later deciding if we are filtering
		// the contents of the string in the `StringLiteral` callback.
		switch (Place)
		{
			case Verse::Grammar::EPlace::UTF8:
			case Verse::Grammar::EPlace::Printable:
			case Verse::Grammar::EPlace::Space:
			case Verse::Grammar::EPlace::String:
			case Verse::Grammar::EPlace::Content:
				Capture.CaptureNodes.Add(Verse::Vst::TNodeRef<Verse::Vst::StringLiteral>::New(
					AsLocus(Snippet), AsStringView(Snippet.Text)));
				break;
			// We already create specific node types for these capture place types.
			case Verse::Grammar::EPlace::BlockCmt:
			case Verse::Grammar::EPlace::LineCmt:
			case Verse::Grammar::EPlace::IndCmt:
			default:
				break;
		}
		Capture.Append(Snippet.Text);
	}

	//-------------------------------------------------------------------------------------------------
	// Backslash in string or markup like \r
	void StringBackslash(CaptureType& Capture, const SSnippet& Snippet,
		EPlace Place, char8_t Backslashed) const
	{
		Capture.Append(Snippet.Text);

		if (Place == EPlace::Content || Place == EPlace::String)
		{
			// Pass through backslashed control characters as-is.
			char8_t Char8 = Backslashed == 'n' ? '\n'
						  : Backslashed == 'r' ? '\r'
						  : Backslashed == 't' ? '\t'
											   : Backslashed;

			Capture.CaptureNodes.Add(Verse::Vst::TNodeRef<Verse::Vst::StringLiteral>::New(
				AsLocus(Snippet), AsString(Char8)));
		}
	}

	//-------------------------------------------------------------------------------------------------
	// [MaxVerse] Form markup content from StringLiteral, StringInterpolate
	ResultType Content(const SSnippet& Snippet,
		const SyntaxesType& Splices) const
	{
		return NewGlitch(AsLocus(Snippet), EDiagnostic::ErrSyntax_Unimplemented,
			"Markup content from string is not yet supported.");

		// Will eventually look something like this:
		// return String(Snippet, Splices);
	}

	//-------------------------------------------------------------------------------------------------
	// [MaxVerse] Form markup content array from Content array
	ResultType Contents(const SSnippet& Snippet, const CaptureType& Leading,
		const SyntaxesType& Splices) const
	{
		return NewGlitch(AsLocus(Snippet), EDiagnostic::ErrSyntax_Unimplemented,
			"Markup from content array is not yet supported.");

		// Will eventually look something like this:
		// return Call(Snippet, EMode::Open, TNodeRef<Identifier>::New("array",
		// AsLocus0(Snippet)), BlockType{ Snippet, Splices });
	}

	//-------------------------------------------------------------------------------------------------
	// [MaxVerse] Macro invocation constructing markup from Content(s)
	ResultType InvokeMarkup(const SSnippet& Snippet, SText StartToken,
		const CaptureType& Leading, const SyntaxType& Macro,
		BlockType* Clause1, BlockType* DoClause,
		const CaptureType& TokenLeading,
		const CaptureType& PreContent,
		const SyntaxType& Content,
		const CaptureType& PostContent) const
	{
		return NewGlitch(AsLocus(Snippet), EDiagnostic::ErrSyntax_Unimplemented,
			"Markup construction is not yet supported.");
	}

	//-------------------------------------------------------------------------------------------------
	void NewLine(CaptureType& Capture, const SSnippet& Snippet,
		const EPlace Place) const
	{
		// If we are currently capturing space information, we want to know if there
		// is a newline after the current capture.
		const CUTF8StringView& SnippetStringView = AsStringView(Snippet.Text);
		// The check against `EPlace::Space` keeps this limited to only being
		// applied to comments for now.
		if (Place == EPlace::Space && Capture.Nodes.Num() != 0)
		{
			const int32_t NumTrailingNewLines =
				CountNumTrailingNewLines(SnippetStringView);
			TNodeRef<Node> LastNodeInCapture = Capture.Nodes.Last();
			LastNodeInCapture->SetNumNewLinesAfter(NumTrailingNewLines);
		}
		Capture.Append(Snippet.Text);
	}

	//-------------------------------------------------------------------------------------------------
	void Semicolon(CaptureType& Capture, const SSnippet& Snippet) const
	{
		Capture.Append(Snippet.Text);
	}

	//-------------------------------------------------------------------------------------------------
	SyntaxType Leading(const CaptureType& Capture,
		const SyntaxType& Syntax) const
	{
		// NOTE: (yiliang.siew) We capture the number of consecutive newlines here
		// and indicate in the node how many of these should be printed out.
		if (!Capture.IsEmpty())
		{
			const int32_t NumLeadingNewLines =
				Capture.CountNumLeadingNewLines();
			// TODO: (yiliang.siew) We can't prefix newlines to the comment in the
			// capture yet, because there is the assumption in the pretty-printer
			// about vertical forms are determined, and thus we transfer the first
			// leading newline from some clauses' members to the clause itself. Refer
			// to the HACK in `Invoke` for details. Once the pretty-printer gets fixed
			// and this HACK removed, this should prefix the first item in the capture
			// with the newlines, if any.
			Syntax->SetNumNewLinesBefore(NumLeadingNewLines);
		}
		if (Capture.Nodes.IsFilled())
		{
			/*
			 * Because our prefix attributes are stored on VST node definitions, we
			 * have the situation where the syntax:
			 *
			 * ```
			 * <#C0#>@<#C1#>attrib1
			 * c := class {}
			 * ```
			 *
			 * results in the VST definition of `c` getting `C0` prefixed to it, while
			 * `C1` is prefixed to the `attrib1` attribute clause. This results in
			 * ambiguity with the similar syntax:
			 *
			 * ```
			 * @<#C1#>attrib1
			 * <#C0#>c := class {}
			 * ```
			 *
			 * Which would end up with the same VST structure if we do not do this
			 * processing here.
			 */
			// NOTE: (yiliang.siew) We look at the current syntax and see if it has a
			// prepend attribute clause that we can prefix the comments to so that we
			// can distinguish the actual VST structure better as described above and
			// thus roundtrip the syntax correctly.
			TNodePtr<Node> SyntaxToAppendTo = Syntax;
			if (Syntax->HasAttributes())
			{
				const TNodePtr<Clause>& SyntaxAttributes = Syntax->GetAux();
				for (const auto& AttributeClause : SyntaxAttributes->GetChildren())
				{
					if (AttributeClause->IsA<Clause>() && AttributeClause->As<Clause>().GetForm() == Clause::EForm::IsPrependAttributeHolder)
					{
						SyntaxToAppendTo = AttributeClause;
						break;
					}
				}
			}

			// NOTE: (yiliang.siew) This is a special case for qualified identifiers,
			// since they are unlike other VST nodes in that a child identifier acts
			// as the identifier while its parent(s) are the qualifiers. If so, we
			// re-associate the comment here so that it will be roundtripped
			// appropriately in the pretty-printer.
			if (Syntax->IsA<Identifier>())
			{
				Identifier& SyntaxAsIdentifier = Syntax->As<Identifier>();
				if (SyntaxAsIdentifier.IsQualified())
				{
					for (const TNodeRef<Node>& CurrentNode : Capture.Nodes)
					{
						if (CurrentNode->IsA<Verse::Vst::Comment>())
						{
							SyntaxAsIdentifier._QualifierPreComments.Add(CurrentNode);
						}
					}

					return Syntax;
				}
			}
			for (const TNodeRef<Node>& CurrentNode : Capture.Nodes)
			{
				if (CurrentNode->IsA<Verse::Vst::Comment>())
				{
					SyntaxToAppendTo->AppendPrefixComment(CurrentNode);
				}
			}
		}
		return Syntax;
	}

	//-------------------------------------------------------------------------------------------------
	SyntaxType Trailing(const SyntaxType& Syntax,
		const CaptureType& Capture) const
	{
		// NOTE: (yiliang.siew) We capture the number of consecutive newlines here
		// and indicate in the node how many of these should be printed out.
		if (!Capture.IsEmpty())
		{
			const int32_t NumLeadingNewLines =
				Capture.CountNumLeadingNewLines();
			Syntax->SetNumNewLinesAfter(NumLeadingNewLines);
		}
		if (Capture.Nodes.IsFilled())
		{
			for (const TNodeRef<Node>& Node : Capture.Nodes)
			{
				if (Node->IsA<Verse::Vst::Comment>())
				{
					Syntax->AppendPostfixComment(Node);
				}
			}
		}

		return Syntax;
	}

	TNodeRef<Clause> MakeSpecifier(const SyntaxType& Attr) const
	{
		TNodeRef<Clause> SpecifierClause = TNodeRef<Clause>::New(
			Attr->Whence(), Clause::EForm::IsAppendAttributeHolder);
		SpecifierClause->AppendChild(Attr);
		return SpecifierClause;
	}

	//-------------------------------------------------------------------------------------------------
	ResultType PrefixToken(const SSnippet& Snippet, EMode Mode, SText Symbol,
		const BlockType& RightBlock, bool bLift,
		const SyntaxesType& Specifiers = {},
		bool bLive = false) const
	{
		const CUTF8String SymbolStr(AsStringView(Symbol));
		const SLocus Whence = AsLocus(Snippet);

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		if (bLift)
		{
			AppendGlitch(
				Whence, EDiagnostic::ErrSyntax_InternalError,
				CUTF8String(
					"%s:%u:%u: Lifting prefix operator '%s' is not yet supported.",
					_SnippetPath.AsCString(), Whence.BeginRow() + 1,
					Whence.BeginColumn() + 1, *SymbolStr));
			return TNodeRef<Placeholder>::New(Whence);
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// First check prefix tokens that allow for no RightBlock

		// Can't use Symbol directly since it is not null terminated
		uint8_t TokenId = Token8(SymbolStr);

		switch (TokenId)
		{
			case SToken(u8"return"):
			{
				const auto NewControlNode =
					TNodeRef<Control>::New(Whence, Control::EKeyword::Return);

				if (RightBlock.Elements.IsFilled())
				{
					// Number of children checked in `Desugarer.cpp` in `DesugarControl()`
					NewControlNode->AppendChildren(RightBlock.Elements);
				}

				return NewControlNode;
			}

			case SToken(u8"break"):
			{
				const auto NewControlNode =
					TNodeRef<Control>::New(Whence, Control::EKeyword::Break);

				if (RightBlock.Elements.IsFilled())
				{
					// Number of children checked in `Desugarer.cpp` in `DesugarControl()`
					NewControlNode->AppendChildren(RightBlock.Elements);
				}

				return NewControlNode;
			}

			case SToken(u8"yield"):
				AppendGlitch(Whence, EDiagnostic::ErrSyntax_Unimplemented,
					"'yield' is reserved for future use...");
				return TNodeRef<Placeholder>::New(Whence);

			case SToken(u8"continue"):
				AppendGlitch(Whence, EDiagnostic::ErrSyntax_Unimplemented,
					"'continue' is reserved for future use...");
				return TNodeRef<Placeholder>::New(Whence);

			default:
				break;
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Check prefix tokens that expect a RightBlock
		const SyntaxType RightExpr = BlockAsSingleExpression(RightBlock);
		const SLocus TokenWhence = AsTokenLocus(Snippet, Symbol);

		switch (TokenId)
		{
			case SToken(u8"-"):
			{
				const auto AddSubNode = TNodeRef<BinaryOpAddSub>::New(Whence);
				AddSubNode->AppendSubOperation(TokenWhence, RightExpr);
				return AddSubNode;
			}

			case SToken(u8"+"):
			{
				const auto AddSubNode = TNodeRef<BinaryOpAddSub>::New(Whence);
				AddSubNode->AppendAddOperation(TokenWhence, RightExpr);
				return AddSubNode;
			}

			case SToken(u8":"):
				// Note that `X:t=V` parses as `(X):=((:t)=V)` which is rearranged to
				// `(X:t):=(V)` in `DefineFromType()`
				return TNodeRef<TypeSpec>::New(Whence, RightExpr);

			case SToken(u8"?"):
			{
				const auto PpcNode = RightExpr->IsA<PrePostCall>()
									   ? RightExpr.As<PrePostCall>()
									   : TNodeRef<PrePostCall>::New(RightExpr, Whence);
				TNodeRef<Clause> QMark = PpcNode->PrependQMark(TokenWhence);
				PpcNode->CombineWhenceWith(TokenWhence);
				return PpcNode;
			}

			case SToken(u8"^"):
			{
				const auto PpcNode = RightExpr->IsA<PrePostCall>()
									   ? RightExpr.As<PrePostCall>()
									   : TNodeRef<PrePostCall>::New(RightExpr, Whence);
				TNodeRef<Clause> Hat = PpcNode->PrependHat(TokenWhence);
				PpcNode->CombineWhenceWith(TokenWhence);
				return PpcNode;
			}

			case SToken(u8"not"):
				return TNodeRef<PrefixOpLogicalNot>::New(Whence, RightExpr);

			case SToken(u8"set"):
				return TNodeRef<Mutation>::New(Whence, RightExpr, Mutation::EKeyword::Set,
					bLive);

			case SToken(u8"var"):
			{
				TNodeRef<Mutation> Result = TNodeRef<Mutation>::New(
					Whence, RightExpr, Mutation::EKeyword::Var, bLive);
				for (const SyntaxType& Specifier : Specifiers)
				{
					Result->AppendAux(MakeSpecifier(Specifier));
				}
				return Result;
			}

			case SToken(u8"live"):
				return TNodeRef<Mutation>::New(Whence, RightExpr, Mutation::EKeyword::Live,
					bLive);

			default:
				return NewGlitch(
					Whence, EDiagnostic::ErrSyntax_Unimplemented,
					CUTF8String("Prefix `%s` operator is unimplemented.", *SymbolStr));
		}
	} // PrefixToken()

	//-------------------------------------------------------------------------------------------------
	// Prefix bracket expression - usually used to specify arrays and maps:
	// [Left]Right
	ResultType PrefixBrackets(const SSnippet& Snippet, const BlockType& LeftBlock,
		const BlockType& RightBlock) const
	{
		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		const SLocus Whence = AsLocus(Snippet);

		if (RightBlock.Punctuation == Verse::Grammar::EPunctuation::Braces)
		{
			return NewGlitch(
				Whence, EDiagnostic::ErrSyntax_Unimplemented,
				CUTF8String("Braced operator'[]' is not currently supported: `%.*s`",
					Verse::Grammar::Length(Snippet.Text),
					Snippet.Text.Start));
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// array '[]element_type' or map '[key_type]value_type' specifier

		// Could alternatively create Call(Snippet, EMode::Closed, "prefix'[]'",
		// RightBlock, Leftlock)

		const SyntaxType RightExpr = BlockAsSingleExpression(RightBlock);

		// Make PrePostCall node
		const auto RhsPpc =
			RightExpr->IsA<PrePostCall>()
				? RightExpr.As<PrePostCall>()
				: TNodeRef<PrePostCall>::New(RightExpr, RightExpr->Whence());

		// Determine bracket locus
		const SLocus KeyWhence = AsLocus(LeftBlock.BlockSnippet);
		const SLocus BracketsWhence(KeyWhence.BeginRow(),
			KeyWhence.BeginColumn() - 1u,
			KeyWhence.EndRow(), KeyWhence.EndColumn() + 1u);

		const auto Args =
			LeftBlock.Elements.IsEmpty()
				// array specifier '[]element_type'
				? TNodeRef<Clause>::New(SyntaxesType(), 0, BracketsWhence,
					Clause::EForm::Synthetic)
				// map specifier '[key_type]value_type'?
				: TNodeRef<Clause>::New(LeftBlock.Elements, LeftBlock.Elements.Num(),
					BracketsWhence, AsClauseForm(LeftBlock));

		RhsPpc->PrependCallArgs(true, Args);
		RhsPpc->CombineWhenceWith(Whence);

		return RhsPpc;
	}

	//-------------------------------------------------------------------------------------------------
	ResultType InfixToken(const SSnippet& Snippet, EMode Mode,
		const SyntaxType& Left, SText Symbol,
		const SyntaxType& Right) const
	{
		// Some of these nodes previously used location of operator though now using
		// location of whole expression
		const SLocus Whence = AsLocus(Snippet);
		const SLocus SymbolWhence =
			SLocus(Left->Whence().GetEnd(), Right->Whence().GetBegin());
		const CUTF8String SymbolStr(AsStringView(Symbol));

		// Can't use `Symbol` directly since it is not null terminated
		switch (Token8(SymbolStr))
		{
			case SToken(u8"="):
				return TNodeRef<BinaryOpCompare>::New(Whence, Left, BinaryOpCompare::op::eq,
					Right);

			case SToken(u8"<>"):
				return TNodeRef<BinaryOpCompare>::New(Whence, Left,
					BinaryOpCompare::op::noteq, Right);

			case SToken(u8"<"):
				return TNodeRef<BinaryOpCompare>::New(Whence, Left, BinaryOpCompare::op::lt,
					Right);

			case SToken(u8"<="):
				return TNodeRef<BinaryOpCompare>::New(Whence, Left,
					BinaryOpCompare::op::lteq, Right);

			case SToken(u8">"):
				return TNodeRef<BinaryOpCompare>::New(Whence, Left, BinaryOpCompare::op::gt,
					Right);

			case SToken(u8">="):
				return TNodeRef<BinaryOpCompare>::New(Whence, Left,
					BinaryOpCompare::op::gteq, Right);

			case SToken(u8"+"):
			{
				if (Left->IsA<BinaryOpAddSub>())
				{
					Left->As<BinaryOpAddSub>().AppendAddOperation(SymbolWhence, Right);
					Left->CombineWhenceWith(Right->Whence());
					return Left;
				}

				const auto NewOpAdd = TNodeRef<BinaryOpAddSub>::New(Whence, Left);
				NewOpAdd->AppendAddOperation(SymbolWhence, Right);
				return NewOpAdd;
			}

			case SToken(u8"-"):
			{
				if (Left->IsA<BinaryOpAddSub>())
				{
					// NOTE: (yiliang.siew) If we do this, we must also transfer any postfix
					// comments from the `Left` node to its current rightmost leaf, since
					// otherwise the pretty-printer would treat this as a postfix comment of
					// the entire operation, which would place it in the wrong position.
					if (Left->GetPostfixComments().Num() != 0)
					{
						const TNodePtr<Node> RightmostChildOfLeft = Left->GetRightmostChild();
						if (RightmostChildOfLeft)
						{
							Node::TransferPostfixComments(Left, RightmostChildOfLeft.AsRef());
						}
					}
					Left->As<BinaryOpAddSub>().AppendSubOperation(SymbolWhence, Right);
					Left->CombineWhenceWith(Right->Whence());
					return Left;
				}

				const auto NewOpSub = TNodeRef<BinaryOpAddSub>::New(Whence, Left);
				NewOpSub->AppendSubOperation(SymbolWhence, Right);
				return NewOpSub;
			}

			case SToken(u8"*"):
			{
				if (Left->IsA<BinaryOpMulDivInfix>())
				{
					ULANG_ENSUREF(!Right->IsA<BinaryOpMulDivInfix>(),
						"RHS is a MulDiv node");
					// NOTE: (yiliang.siew) If we do this, we must also transfer any postfix
					// comments from the `Left` node to its current rightmost leaf, since
					// otherwise the pretty-printer would treat this as a postfix comment of
					// the entire operation, which would place it in the wrong position.
					if (Left->GetPostfixComments().Num() != 0)
					{
						const TNodePtr<Node> RightmostChildOfLeft = Left->GetRightmostChild();
						if (RightmostChildOfLeft)
						{
							Node::TransferPostfixComments(Left, RightmostChildOfLeft.AsRef());
						}
					}
					Left->As<BinaryOpMulDivInfix>().AppendMulOperation(SymbolWhence, Right);
					Left->CombineWhenceWith(Right->Whence());
					return Left;
				}

				const auto NewOpMul = TNodeRef<BinaryOpMulDivInfix>::New(Whence, Left);
				NewOpMul->AppendMulOperation(SymbolWhence, Right);
				return NewOpMul;
			}

			case SToken(u8"/"):
			{
				if (Left->IsA<BinaryOpMulDivInfix>())
				{
					ULANG_ENSUREF(!Right->IsA<BinaryOpMulDivInfix>(),
						"RHS is a MulDiv node");
					// NOTE: (yiliang.siew) If we do this, we must also transfer any postfix
					// comments from the `Left` node to its current rightmost leaf, since
					// otherwise the pretty-printer would treat this as a postfix comment of
					// the entire operation, which would place it in the wrong position.
					if (Left->GetPostfixComments().Num() != 0)
					{
						const TNodePtr<Node> RightmostChildOfLeft = Left->GetRightmostChild();
						if (RightmostChildOfLeft)
						{
							Node::TransferPostfixComments(Left, RightmostChildOfLeft.AsRef());
						}
					}
					Left->As<BinaryOpMulDivInfix>().AppendDivOperation(SymbolWhence, Right);
					Left->CombineWhenceWith(Right->Whence());
					return Left;
				}

				const auto NewOpDiv = TNodeRef<BinaryOpMulDivInfix>::New(Whence, Left);
				NewOpDiv->AppendDivOperation(SymbolWhence, Right);
				return NewOpDiv;
			}

			case SToken(u8"."):
			{
				ULANG_ASSERTF(
					Right->IsA<Identifier>(),
					"Illegal syntax : dot must always be followed by identifier.");
				if (Left->IsA<PrePostCall>())
				{
					// NOTE: (yiliang.siew) If we do this, we must also transfer any postfix
					// comments from the `Left` node to its current rightmost leaf, since
					// otherwise the pretty-printer would treat this as a postfix comment of
					// the entire operation, which would place it in the wrong position.
					if (Left->GetPostfixComments().Num() != 0)
					{
						const TNodePtr<Node> RightmostChildOfLeft = Left->GetRightmostChild();
						if (RightmostChildOfLeft)
						{
							Node::TransferPostfixComments(Left, RightmostChildOfLeft.AsRef());
						}
					}
					// Member Access Chaining Transform
					// Convert from:
					//    PrePostCall(PrePostCall(a 'dot' b) 'dot' c)
					// to:
					//    dot(a 'dot' b 'dot' c)
					Left->As<PrePostCall>().AppendDotIdent(Whence, Right.As<Identifier>());
					Left->CombineWhenceWith(Right->Whence());
					return Left;
				}
				else
				{
					const auto NewPpc = TNodeRef<PrePostCall>::New(Whence);
					NewPpc->AppendChild(Left);
					ULANG_ASSERTF(
						Right->IsA<Identifier>(),
						"Illegal syntax : dot must always be followed by identifier.");
					NewPpc->AppendDotIdent(Whence, Right.As<Identifier>());
					return NewPpc;
				}
			}

			case SToken(u8"and"):
				return TNodeRef<BinaryOpLogicalAnd>::New(Whence, Left, Right);

			case SToken(u8"or"):
				return TNodeRef<BinaryOpLogicalOr>::New(Whence, Left, Right);

			case SToken(u8":"):
				return TNodeRef<TypeSpec>::New(Whence, Left, Right);

			case SToken(u8".."):
				return TNodeRef<BinaryOpRange>::New(Whence, Left, Right);

			case SToken(u8"->"):
				return TNodeRef<BinaryOpArrow>::New(Whence, Left, Right);

			default:
				return NewGlitch(
					Whence, EDiagnostic::ErrSyntax_Unimplemented,
					CUTF8String("Infix `%s` operator is unimplemented.", *SymbolStr));
		}
	} // InfixToken

	//-------------------------------------------------------------------------------------------------
	ResultType DefineFromType(const SSnippet& Snippet, const SyntaxType& Left,
		const BlockType& RightBlock) const
	{
		SyntaxType Right = BlockAsSingleExpression(RightBlock);

		// For "a:b <cmp> c", the parser generates:
		//   define_from_type(a, infix_token(<cmp>, prefix_token(u8":", b), c)).
		// The case where there is no trailing comparison operator is also generated
		// as:
		//   define_from_type(a, prefix_token(u8":", b)).
		// This is to allow interpreting e.g. "a:b<c" as "a=(:b)<c". That is, a is
		// any value of the type b that is less than c. The simpler interpretation
		// of "a:Int<3" as "a:(Int<3)" suffers from a category error due to
		// comparing a type with an integer. To avoid changing the rest of the
		// compiler to consume this `a=(:b)` syntax instead of "a:b", transform this
		// simple case (without a trailing comparison operator) back to the "a:b"
		// form.

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// `X:t` parses as `X=(:t)` which is rearranged to `X:t` here
		if (Right->IsA<TypeSpec>() && Right->GetChildCount() == 1)
		{
			Right->AppendChildAt(Left, 0);
			Right->CombineWhenceWith(Left->Whence());
			return Right;
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// `X:t where t:u` parses as `X=(:t where (t:u)` which is rearranged to
		// `(X:t) where (t:u)` here
		if (Right->IsA<Where>() && !Right->IsEmpty() && Right->GetChildren()[0]->IsA<TypeSpec>() && (Right->GetChildren()[0]->GetChildCount() == 1))
		{
			SyntaxType WhereLeft = Right->GetChildren()[0];
			WhereLeft->AppendChildAt(Left, 0);
			WhereLeft->CombineWhenceWith(Left->Whence());
			Right->CombineWhenceWith(Left->Whence());
			return Right;
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// `X:t=V` parses as `(X):=((:t)=V)` which is rearranged to `(X:t):=(V)`
		// here
		if (Right->IsA<Assignment>())
		{
			Assignment& AssignOp = Right->As<Assignment>();
			SyntaxType LeftAssign = AssignOp.GetOperandLeft();
			Assignment::EOp AssignKind = AssignOp.GetTag<Assignment::EOp>();

			if ((AssignKind == Assignment::EOp::assign) && LeftAssign->IsA<TypeSpec>())
			{
				// Move Left element to typespec first child
				LeftAssign->AppendChildAt(Left, 0);
				LeftAssign->CombineWhenceWith(Left->Whence());

				// Swap Assignment node with Definition node
				NodeArray AssignOperands = AssignOp.TakeChildren();
				const int32_t NumNewLinesAfterNewlines = AssignOp.NumNewLinesAfter();
				const TNodeRef<Clause> WrappedClause = AsWrappedClause(AssignOperands[1]);
				TNodeRef<Definition> NewDefinition = TNodeRef<Definition>::New(
					Left->Whence() | AssignOp.Whence(), LeftAssign,
					// Later definition code expects definition RHS to always be wrapped
					// in a clause $Revisit - Clause wrapper seems redundant and could
					// be removed in the future]
					WrappedClause);
				NewDefinition->SetNumNewLinesAfter(NumNewLinesAfterNewlines);
				// TODO: (yiliang.siew) This is a HACK, but we will move the newline
				// before from the first child of the clause of the typespec to be a
				// line after the typespec. This is in keeping with the expectations of
				// the pretty-printer for the time being.
				// TransferFirstLeadingNewLineOfClauseMember(*WrappedClause);
				if (WrappedClause->GetChildCount() > 0 && WrappedClause->GetChildren()[0]->HasNewLinesBefore())
				{
					const int32_t CurrentNumNewLinesBefore =
						WrappedClause->GetChildren()[0]->NumNewLinesBefore();
					WrappedClause->GetChildren()[0]->SetNumNewLinesBefore(
						CurrentNumNewLinesBefore - 1);
					LeftAssign->SetNumNewLinesAfter(LeftAssign->NumNewLinesAfter() + 1);
				}

				return NewDefinition;
			}
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// `X:t:=V` parses as `(X):=((:t):=V)` which is rearranged to `(X:t):=(V)`
		// here
		if (Right->IsA<Definition>())
		{
			Definition& DefOp = Right->As<Definition>();
			SyntaxType LeftDef = DefOp.GetOperandLeft();

			if (LeftDef->IsA<TypeSpec>())
			{
				// A `:= `after a type spec should be an error and `=` should be
				// suggested instead Keeping for now while code is transitioned.

				// Move Left element to typespec first child
				LeftDef->AppendChildAt(Left, 0);
				LeftDef->CombineWhenceWith(Left->Whence());
				Right->CombineWhenceWith(LeftDef->Whence());

				return Right;
			}
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// `a:b<op>c` case is translated to: `a:type{:b<op>c}`
		const SLocus RightWhence = Right->Whence();
		TNodeRef<Clause> RightClause = TNodeRef<Clause>::New(
			Right, RightWhence, Clause::EForm::NoSemicolonOrNewline);
		TNodeRef<Macro> TypeMacro = TNodeRef<Macro>::New(
			RightWhence, TNodeRef<Identifier>::New("type", RightWhence.GetBegin()),
			ClauseArray{RightClause});
		return TNodeRef<TypeSpec>::New(AsLocus(Snippet), Left, TypeMacro);
	}

	//-------------------------------------------------------------------------------------------------
	ResultType InfixBlock(const SSnippet& Snippet, const SyntaxType& Left,
		SText Symbol, const BlockType& RightBlock) const
	{
		// TODO: (yiliang.siew) We currently do not support having effect specifiers
		// on the definition type. When we implement support for this, this check
		// should be removed.
		if (Left->IsA<Verse::Vst::TypeSpec>() && !Left->IsEmpty())
		{
			const Verse::Vst::TNodePtr<Verse::Vst::Node> TypeSpecRhs = Left->GetRightmostChild();
			if (TypeSpecRhs)
			{
				if (const Verse::Vst::TNodePtr<Verse::Vst::Clause> Aux = TypeSpecRhs->GetAux();
					Aux && !Aux->IsEmpty())
				{
					for (const Verse::Vst::TNodeRef<Verse::Vst::Node>& AuxElement : Aux->GetChildren())
					{
						const Verse::Vst::Clause* AuxChildClause =
							AuxElement->AsNullable<Verse::Vst::Clause>();
						if (!AuxChildClause)
						{
							continue;
						}
						if (AuxChildClause->GetForm() == Verse::Vst::Clause::EForm::IsAppendAttributeHolder)
						{
							AppendGlitch(
								AuxChildClause->Whence(),
								EDiagnostic::ErrSyntax_Unimplemented,
								"Open world specifiers :t<spec> are not yet supported.");
						}
					}
				}
			}
		}

		if (Verse::Grammar::Length(Symbol) == 0)
		{
			// tokenless definition
			return DefineFromType(Snippet, Left, RightBlock);
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// First test for assignments that append individual children

		bool bDanglingRHS =
			RightBlock.Elements.IsEmpty() && (RightBlock.Punctuation != Verse::Grammar::EPunctuation::Braces);

		// Can't use Symbol directly since it is not null terminated
		const CUTF8String SymbolStr(AsStringView(Symbol));
		const uint8_t SymbolId = Token8(SymbolStr);
		const SLocus Whence = AsLocus(Snippet);
		const SLocus TokenWhence =
			AsTokenLocusPrefix(RightBlock.BlockSnippet, Symbol);

		if ((SymbolId == SToken(u8":=")) || (SymbolId == SToken(u8"is")))
		{
			// Input looks like a function or a definition
			//
			// Left := RightBlock
			//
			// funcName( a1:t1, a2:t2 ) : t3 = {bodys}
			// x:Int = 123
			// Color = enum{...}

			// If there were no expressions or braces on the RHS, produce a dangling
			// equals error.
			if (bDanglingRHS)
			{
				AppendGlitch(TokenWhence, EDiagnostic::ErrSyntax_DanglingEquals);
			}

			// Wrap the RHS expression(s) in a Clause node.
			const TNodeRef<Clause> WrappedClause =
				AsWrappedClause(BlockAsSingleExpression(RightBlock));
			TNodeRef<Definition> NewDefinition =
				TNodeRef<Definition>::New(Whence, Left, WrappedClause);
			// TODO: (yiliang.siew) This is a HACK, but we will move the newline
			// before from the first child of the clause of the typespec to be a line
			// after the typespec. This is in keeping with the expectations of the
			// pretty-printer for the time being.
			// TransferFirstLeadingNewLineOfClauseMember(*WrappedClause);
			if (WrappedClause->GetChildCount() > 0 && WrappedClause->GetChildren()[0]->HasNewLinesBefore())
			{
				const int32_t CurrentNumNewLinesBefore =
					WrappedClause->GetChildren()[0]->NumNewLinesBefore();
				WrappedClause->GetChildren()[0]->SetNumNewLinesBefore(
					CurrentNumNewLinesBefore - 1);
				Left->SetNumNewLinesAfter(Left->NumNewLinesAfter() + 1);
			}
			return NewDefinition;
		}

		if (SymbolId == SToken(u8"=>"))
		{
			// #NewParser Allow dangling `=>` or prevent like `=`?
			// if (bDanglingRHS)
			//{
			//    AppendGlitch(TokenWhence, EDiagnostic::ErrSyntax_DanglingEquals,
			//    "Dangling `=>` with no expressions or empty braced block `{}` on its
			//    right hand side.");
			//}
			SyntaxType RightBlockExpr = BlockAsSingleExpression(RightBlock);
			TNodeRef<Clause> WrappedClause = AsWrappedClause(RightBlockExpr);
			SetClausePunctuation(RightBlock, *WrappedClause);

			return TNodeRef<Lambda>::New(Whence, Left, WrappedClause);
		}

		if (SymbolId == SToken(u8"where"))
		{
			// This means that there are multiple sub-expressions for the `where`
			// conditions.
			if (RightBlock.Form == Verse::Grammar::EForm::List && RightBlock.Elements.Num() > 1)
			{
				return NewGlitch(Whence, EDiagnostic::ErrSyntax_Unimplemented,
					"Semicolons and newlines in `where` clauses are not "
					"yet implemented.");
			}

			return Verse::Vst::TNodeRef<Verse::Vst::Where>::New(Whence, Left, RightBlock.Elements);
		}

		//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
		// Test for assignments that add block of expressions as single expression
		// operand
		const SyntaxType RightExpr = BlockAsSingleExpression(RightBlock);

		switch (SymbolId)
		{
			case SToken(u8"="):
				// If there were no expressions or braces on the RHS, produce a dangling
				// equals error.
				if (bDanglingRHS)
				{
					AppendGlitch(TokenWhence, EDiagnostic::ErrSyntax_DanglingEquals,
						"Dangling `=` assignment with no expressions or empty "
						"braced block `{}` on its right hand side.");
				}
				else if (Length(RightBlock.BlockSnippet.Text) > 0 && Left->GetElementType() == NodeType::Mutation)
				{
					const char8_t NextChar = RightBlock.BlockSnippet.Text.Start[0];
					switch (NextChar)
					{
						case '+':
						case '-':
						case '*':
						case '/':
							AppendGlitch(
								TokenWhence, EDiagnostic::WarnParser_SpaceBetweenEqualsAndUnary,
								CUTF8String("'=%c' is not an operator; did you mean '%c='? "
											"(Or add a space after '=' to silence this warning.)",
									NextChar, NextChar));
							break;
						default:
							break;
					}
				}
				// Note that `X:t=V` parses as `(X):=((:t)=V)` which is rearranged to
				// `(X:t):=(V)` in `DefineFromType()`
				return TNodeRef<Assignment>::New(Whence, Left, Assignment::EOp::assign,
					RightExpr);

			case SToken(u8"+="):
				// If there were no expressions or braces on the RHS, produce a dangling
				// equals error.
				if (bDanglingRHS)
				{
					AppendGlitch(TokenWhence, EDiagnostic::ErrSyntax_DanglingEquals,
						"Dangling `+=` plus assignment with no expressions or "
						"empty braced block `{}` on its right hand side.");
				}

				return TNodeRef<Assignment>::New(Whence, Left, Assignment::EOp::addAssign,
					RightExpr);

			case SToken(u8"-="):
				if (bDanglingRHS)
				{
					AppendGlitch(TokenWhence, EDiagnostic::ErrSyntax_DanglingEquals,
						"Dangling `-=` subtract assignment with no expressions or "
						"empty braced block `{}` on its right hand side.");
				}

				return TNodeRef<Assignment>::New(Whence, Left, Assignment::EOp::subAssign,
					RightExpr);

			case SToken(u8"*="):
				if (bDanglingRHS)
				{
					AppendGlitch(TokenWhence, EDiagnostic::ErrSyntax_DanglingEquals,
						"Dangling `*=` multiply assignment with no expressions or "
						"empty braced block `{}` on its right hand side.");
				}

				return TNodeRef<Assignment>::New(Whence, Left, Assignment::EOp::mulAssign,
					RightExpr);

			case SToken(u8"/="):
				if (bDanglingRHS)
				{
					AppendGlitch(TokenWhence, EDiagnostic::ErrSyntax_DanglingEquals,
						"Dangling `/=` divide assignment with no expressions or "
						"empty braced block `{}` on its right hand side.");
				}

				return TNodeRef<Assignment>::New(Whence, Left, Assignment::EOp::divAssign,
					RightExpr);

			default:
				return NewGlitch(
					Whence, EDiagnostic::ErrSyntax_Unimplemented,
					CUTF8String("Infix `%s` operator is unimplemented.", *SymbolStr));
		}
	}

	//-------------------------------------------------------------------------------------------------
	ResultType PostfixToken(const SSnippet& Snippet, EMode Mode,
		const SyntaxType& Left, SText Symbol) const
	{
		const auto PpcNode = Left->IsA<PrePostCall>()
							   ? Left.As<PrePostCall>()
							   : TNodeRef<PrePostCall>::New(Left, Left->Whence());

		const CUTF8String SymbolStr(AsStringView(Symbol));
		const SLocus Whence = AsLocus(Snippet);
		const SLocus TokenWhence = AsTokenLocusPostfix(Snippet, Symbol);

		// Can't use Symbol directly since it is not null terminated
		switch (Token8(SymbolStr))
		{
			case SToken(u8"?"):
			{
				Node::TransferPostfixComments(PpcNode, PpcNode->AccessChildren().Last());
				PpcNode->AppendQMark(TokenWhence);
				PpcNode->CombineWhenceWith(Whence);
				break;
			}

			case SToken(u8"^"):
			{
				// NOTE: (yiliang.siew) This may seem counter-intuitive, but the syntax:
				/*
				 * ```
				 * A.B<#comment#>^
				 * ```
				 *
				 * Translates to having the `PrePostCall` operation of `B^` having the
				 * `comment` suffixed to it. Because we append the `^` syntax ourselves
				 * during roundtripping, in order to have the comment appear in the right
				 * order, we transfer any suffix comments from the `PrePostCall` operation
				 * to the `B` identifier itself so that they can be pretty-printed in the
				 * right order.
				 */
				Node::TransferPostfixComments(PpcNode, PpcNode->AccessChildren().Last());
				PpcNode->AppendHat(TokenWhence);
				PpcNode->CombineWhenceWith(Whence);
				break;
			}

			case SToken(u8"ref"):
			{
				return NewGlitch(Whence, EDiagnostic::ErrSyntax_Unimplemented,
					"Postfix `ref` is unimplemented");
			}

			default:
				ULANG_ERRORF("%s:%u:%u: Unrecognized postfix operator '%s'.",
					_SnippetPath.AsCString(), Whence.BeginRow() + 1,
					Whence.BeginColumn() + 1, *SymbolStr);
				return TNodeRef<Placeholder>::New(Whence);
		}

		return PpcNode;
	}

	//===============================================================================
	// Optional string callbacks which don't contribute to abstract syntax.

	// void BlankLine(CaptureType& Capture, const SSnippet& Snippet, EPlace Place)
	// const  {} void LinePrefix(CaptureType& Capture, const SSnippet& Snippet)
	// const              {}

	//-------------------------------------------------------------------------------------------------
	void Indent(CaptureType& Capture, const SSnippet& Snippet,
		EPlace Place) const {}

	//-------------------------------------------------------------------------------------------------
	void LineCmt(CaptureType& Capture, const SSnippet& Snippet,
		const EPlace Place, const CaptureType& Comments) const
	{
		if ((_ParseBehaviour == EParseBehaviour::ParseNoComments) || !Snippet.Text)
		{
			return;
		}
		Capture.Append(Snippet.Text);
		const CUTF8StringView CommentText = AsStringView(Snippet.Text);
		Capture.Nodes.Add(Verse::Vst::TNodeRef<Verse::Vst::Comment>::New(
			Verse::Vst::Comment::EType::line, CommentText, AsLocus(Snippet)));
	}

	//-------------------------------------------------------------------------------------------------
	void BlockCmt(CaptureType& Capture, const SSnippet& Snippet,
		const EPlace Place, const CaptureType& Comments) const
	{
		if ((_ParseBehaviour == EParseBehaviour::ParseNoComments) || !Snippet.Text)
		{
			return;
		}
		Capture.Append(Snippet.Text);
		const CUTF8StringView CommentText = AsStringView(Snippet.Text);
		Capture.Nodes.Add(Verse::Vst::TNodeRef<Verse::Vst::Comment>::New(
			Verse::Vst::Comment::EType::block, CommentText, AsLocus(Snippet)));
	}

	//-------------------------------------------------------------------------------------------------
	void IndCmt(CaptureType& Capture, const SSnippet& Snippet, const EPlace Place,
		const CaptureType& Comments) const
	{
		if ((_ParseBehaviour == EParseBehaviour::ParseNoComments) || !Snippet.Text)
		{
			return;
		}
		Capture.Append(Snippet.Text);
		const CUTF8StringView CommentText = AsStringView(Snippet.Text);
		Capture.Nodes.Add(Verse::Vst::TNodeRef<Verse::Vst::Comment>::New(
			Verse::Vst::Comment::EType::ind, CommentText, AsLocus(Snippet)));
	}

	//-------------------------------------------------------------------------------------------------
	void MarkupTrim(CaptureType& Capture) const { Capture.Reset(); }

	//-------------------------------------------------------------------------------------------------
	void MarkupStart(CaptureType& Capture, const SSnippet& Snippet) const
	{
		// Capture.String.IsEmpty();
	}

	//-------------------------------------------------------------------------------------------------
	void MarkupTag(CaptureType& Capture, const SSnippet& Snippet) const
	{
		// Capture.String.IsEmpty();
	}

	//-------------------------------------------------------------------------------------------------
	void MarkupStop(CaptureType& Capture, const SSnippet& Snippet) const
	{
		// Capture.String.IsEmpty();
	}

	//-------------------------------------------------------------------------------------------------
	// Gets the snippet file path currently being parsed
	const CUTF8String& GetSnippetPath() const { return _SnippetPath; }

protected:
	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Internal methods

	//-------------------------------------------------------------------------------------------------
	void AppendSpecifier(const SyntaxType& Base,
		const BlockType& SpecifierBlock) const
	{
		// $Revisit - Note that a <specifier> is not differentiated from an
		// @attribute apart from the fact that a <specifier> may only occur after an
		// element and an @attribute may only occur before an element (currently).
		// They should be differentiated in the future.

		SyntaxType Specifier = BlockAsSingleExpression(SpecifierBlock);
		const SLocus Whence = Specifier->Whence();

		// Ensure only one specifier expression
		if (SpecifierBlock.Elements.Num() != 1)
		{
			AppendGlitch(Specifier->Whence(),
				EDiagnostic::ErrSyntax_ExpectedExpression,
				CUTF8String("%s:%u:%u: Specifier must be single identifier.",
					_SnippetPath.AsCString(), Whence.BeginRow() + 1,
					Whence.BeginColumn() + 1));
		}

		// specifier nodes need to be wrapped in a Clause to hold the attribute
		// comments
		const TNodeRef<Clause> CommentClause =
			TNodeRef<Clause>::New(Whence, Clause::EForm::IsAppendAttributeHolder);

		CommentClause->AppendChild(Specifier);
		Base->AppendAux(CommentClause);
	}

	//-------------------------------------------------------------------------------------------------
	void AppendSpecifiers(const SyntaxType& Base,
		const SyntaxesType& Specifiers) const
	{
		// $Revisit - Note that a <specifier> is not differentiated from an
		// @attribute apart from the fact that a <specifier> may only occur after an
		// element and an @attribute may only occur before an element (currently).
		// They should be differentiated in the future.

		for (const SyntaxType& Specifier : Specifiers)
		{
			// specifier nodes need to be wrapped in a Clause to hold the attribute
			// comments
			const TNodeRef<Clause> CommentClause = TNodeRef<Clause>::New(
				Specifier->Whence(), Clause::EForm::IsAppendAttributeHolder);

			CommentClause->AppendChild(Specifier);
			Base->AppendAux(CommentClause);
		}
	}

	//-------------------------------------------------------------------------------------------------
	static void AppendAttributeNode(const SSnippet& Snippet,
		const SyntaxType& Base,
		const SyntaxType& Attribute)
	{
		// $Revisit - Note that a <specifier> is not differentiated from an
		// @attribute apart from the fact that a <specifier> may only occur after an
		// element and an @attribute may only occur before an element (currently).
		// They should be differentiated in the future.

		// attribute nodes need to be wrapped in a Clause to hold the attribute
		// comments
		const TNodeRef<Clause> CommentClause = TNodeRef<Clause>::New(
			Attribute->Whence(), Clause::EForm::IsAppendAttributeHolder);

		CommentClause->AppendChild(Attribute);
		Base->AppendAux(CommentClause);
	}

	//-------------------------------------------------------------------------------------------------
	static void PrependAttributeNode(const SSnippet& Snippet,
		const SyntaxType& Attribute,
		const SyntaxType& Base)
	{
		// $Revisit - Note that a <specifier> is not differentiated from an
		// @attribute apart from the fact that a <specifier> may only occur after an
		// element and an @attribute may only occur before an element (currently).
		// They should be differentiated in the future.

		// attribute nodes need to be wrapped in a Clause to hold the attribute
		// comments
		const TNodeRef<Clause> CommentClause = TNodeRef<Clause>::New(
			Attribute->Whence(), Clause::EForm::IsPrependAttributeHolder);

		CommentClause->AppendChild(Attribute);
		Base->PrependAux(CommentClause);
	}

	//-------------------------------------------------------------------------------------------------
	SyntaxType BlockAsSingleExpression(const BlockType& Block) const
	{
		/*
		 * NOTE: (yiliang.siew) This adds trailing comments to expressions such as:
		 *
		 * ```
		 * dem():int =
		 *     # this is a comment
		 *     c:int = 5
		 *     <# leading comment #> foo() # trailing foo() invocation
		 * ```
		 *
		 * Since they seem to be only exposed in this callback from the parser.
		 *
		 * We also add leading newlines to expressions, which is important for
		 * syntax such as:
		 * ```
		 * f():void  <#hello#>     =  <#ohnoes#>
		 *     return 5
		 * ```
		 * The `TypeSpec` node does not get a newline after it, but the `Control`
		 * node would get a leading newline instead.
		 */
		// NOTE: (yiliang.siew) If the block has elements, prefix/suffix comments to
		// the first/last elements of the block and also set the leading/trailing
		// newlines to the first/last elements as well. If the block has no
		// elements, add all leading/trailing comments as block-level elements in
		// the clause and set the leading/trailing newlines as appropriate.
		const uint32_t NumBlockElements = Block.Elements.Num();
		Node::NodeArray MutableBlockElements;
		Node::NodeArray LeadingBlockElements;
		if (NumBlockElements == 0)
		{
			MutableBlockElements.Reserve(NumBlockElements + Block.ElementsTrailing.Nodes.Num());
		}
		// We check the string instead of just the nodes since currently whitespace
		// alone doesn't generate any nodes for being captured.
		if (!Block.PunctuationLeading.IsEmpty())
		{
			LeadingBlockElements.Reserve(Block.PunctuationLeading.Nodes.Num());
			// If there are no elements in the block, just add any comment nodes as
			// block-level comments.
			for (const TNodeRef<Node>& CurNode : Block.PunctuationLeading.Nodes)
			{
				if (CurNode->IsA<Verse::Vst::Comment>())
				{
					LeadingBlockElements.Add(CurNode);
				}
			}
			if (NumBlockElements == 0)
			{
				if (LeadingBlockElements.Num() != 0)
				{
					const int32_t NumLeadingNewLines =
						Block.PunctuationLeading.CountNumTrailingNewLines();
					const TNodeRef<Node>& FirstElementInBlock = LeadingBlockElements[0];
					FirstElementInBlock->SetNumNewLinesBefore(NumLeadingNewLines);
				}
			}
			else
			{
				const int32_t NumLeadingNewLines =
					Block.PunctuationLeading.CountNumTrailingNewLines();
				const TNodeRef<Node>& FirstElementInBlock = Block.Elements[0];
				FirstElementInBlock->SetNumNewLinesBefore(NumLeadingNewLines);
			}
		}
		if (!Block.ElementsTrailing.IsEmpty())
		{
			if (NumBlockElements == 0)
			{
				for (const TNodeRef<Node>& CurNode : Block.ElementsTrailing.Nodes)
				{
					if (CurNode->IsA<Verse::Vst::Comment>())
					{
						MutableBlockElements.Add(CurNode);
					}
				}
				if (MutableBlockElements.Num() != 0)
				{
					const int32_t NumTrailingNewLines =
						Block.ElementsTrailing.CountNumLeadingNewLines();
					MutableBlockElements.Last()->SetNumNewLinesAfter(NumTrailingNewLines);
				}
			}
			else
			{
				const TNodeRef<Node>& LastElementInBlock = Block.Elements.Last();
				const int32_t NumTrailingNewLines = Block.ElementsTrailing.CountNumLeadingNewLines();
				LastElementInBlock->SetNumNewLinesAfter(NumTrailingNewLines);
				for (const TNodeRef<Node>& CurNode : Block.ElementsTrailing.Nodes)
				{
					if (CurNode->IsA<Verse::Vst::Comment>())
					{
						LastElementInBlock->AppendPostfixComment(CurNode);
					}
				}
			}
		}
		TNodePtr<Node> Result = nullptr;
		if (Block.Punctuation == Verse::Grammar::EPunctuation::Parens)
		{
			Result = TNodeRef<Parens>::New(
				NumBlockElements ? BlockElementsLocus(Block)
								 : AsLocus(Block.BlockSnippet),
				AsClauseForm(
					Block), // Alternatively just use Clause::EForm::Synthetic?
				NumBlockElements == 0 ? MutableBlockElements : Block.Elements);
		}
		else if (NumBlockElements == 1 && Block.Punctuation != Verse::Grammar::EPunctuation::Braces)
		{
			// If only one element then return it
			Result = Block.Elements[0];
		}
		else
		{
			const Clause::EForm Form = AsClauseForm(Block);
			if (Form == Clause::EForm::NoSemicolonOrNewline && Block.Punctuation == Verse::Grammar::EPunctuation::None)
			{
				ULANG_ASSERT(Block.Form == Verse::Grammar::EForm::Commas);
				Result = TNodeRef<Commas>::New(
					NumBlockElements ? BlockElementsLocus(Block)
									 : AsLocus(Block.BlockSnippet),
					NumBlockElements == 0 ? MutableBlockElements : Block.Elements);
			}
			else
			{
				Result = TNodeRef<Clause>::New(
					NumBlockElements == 0 ? MutableBlockElements : Block.Elements,
					NumBlockElements ? BlockElementsLocus(Block)
									 : AsLocus(Block.BlockSnippet),
					Form);
			}
		}
		ULANG_ASSERT(Result.IsValid());
		for (const TNodeRef<Node>& CurNode : LeadingBlockElements)
		{
			ULANG_ASSERT(CurNode->IsA<Verse::Vst::Comment>());
			Result->AppendPrefixComment(CurNode);
		}
		if (Result->IsA<Clause>())
		{
			SetClausePunctuation(Block, Result->As<Clause>());
		}
		return Result.AsRef();
	}

	//-------------------------------------------------------------------------------------------------
	// Ensure that syntax element is wrapped in a clause - if it is already a
	// clause then just pass it on
	const TNodeRef<Clause> AsWrappedClause(const SyntaxType& Element) const
	{
		if (Element->IsA<Clause>())
		{
			// Already a clause node - just return it
			// return TNodeRef<Clause>(Element->As<Clause>());
			return *reinterpret_cast<const TNodeRef<Clause>*>(&Element);
		}

		// Synthetic might make sense here though it currently confuses
		// round-tripping to a string
		return TNodeRef<Clause>::New(Element, Element->Whence(),
			Clause::EForm::NoSemicolonOrNewline);
	}

	/**
	 * This function checks a `BlockType`'s leading/trailing elements/punctuation,
	 * and mutates the clause as necessary to what the final VST hierarchy should
	 * be like. This mostly handles comments and trailing/leading newlines.
	 *
	 * @param InBlock 		The block that contains the current captured
	 * elements.
	 * @param InClause 	The clause to mutate.
	 */
	static void ProcessBlockPunctuationForClause(const BlockType& InBlock,
		TNodeRef<Clause> InClause)
	{
		if (!InBlock.PunctuationLeading.IsEmpty())
		{
			for (const TNodeRef<Node>& Element : InBlock.PunctuationLeading.Nodes)
			{
				if (Element->IsA<Verse::Vst::Comment>())
				{
					InClause->AppendPrefixComment(Element);
				}
			}
			const int32_t NumTrailingNewLines =
				InBlock.PunctuationLeading.CountNumTrailingNewLines();
			if (NumTrailingNewLines > 0)
			{
				InClause->SetNumNewLinesBefore(NumTrailingNewLines);
			}
		}
		// NOTE: (yiliang.siew) Since it is possible that the trailing elements can
		// include comments that are either preceded/suffixed by whitespace, we
		// check first and associate as needed.
		if (!InBlock.ElementsTrailing.IsEmpty() || !InBlock.PunctuationTrailing.IsEmpty())
		{
			TNodeRef<Node> NodeToSuffixCommentsTo = InClause;
			const int32_t NumInClauseChildren = InClause->GetChildCount();
			// NOTE: (yiliang.siew) If there are no children in the clause at all, we
			// add each comment as a block-level element inside of the clause instead
			// of having one comment and having the rest of the comment suffixed to
			// it. The reason for this is that we do not want to assume "groups" of
			// comments for the VST (users can make a block comment for that) in terms
			// of mutating the tree.  It's more intuitive to allow deleting of
			// individual leaf nodes this way than associating them with any other VST
			// node. Plus, it's also easier to inspect the tree in a debugger.
			if (NumInClauseChildren == 0)
			{
				for (const TNodeRef<Node>& Element : InBlock.ElementsTrailing.Nodes)
				{
					if (Element->IsA<Verse::Vst::Comment>())
					{
						InClause->AppendChild(Element);
					}
				}
			}
			else
			{
				NodeToSuffixCommentsTo = InClause->GetChildren().Last();
				for (const TNodeRef<Node>& Element : InBlock.ElementsTrailing.Nodes)
				{
					if (Element->IsA<Verse::Vst::Comment>())
					{
						NodeToSuffixCommentsTo->AppendPostfixComment(Element);
					}
				}
			}
			for (const TNodeRef<Node>& PunctuationElement :
				InBlock.PunctuationTrailing.Nodes)
			{
				if (PunctuationElement->IsA<Verse::Vst::Comment>())
				{
					InClause->AppendPostfixComment(PunctuationElement);
				}
			}
			NodeArray& InClausePostfixComments = InClause->AccessPostfixComments();
			// These are newlines that immediately trail the last element in the
			// clause, before any other comments. e.g. `/n/n/n#comment`
			const int32_t NumLeadingNewLinesTrailingElements =
				InBlock.ElementsTrailing.CountNumLeadingNewLines();
			if (NumLeadingNewLinesTrailingElements > 0)
			{
				// TODO: (yiliang.siew) Technically this is wrong, but because we cannot
				// distinguish at the moment intermingled comments and newlines in terms
				// of the order, we cannot yet make this determination accurately.
				if (InClause->GetChildCount() != 0)
				{
					const TNodeRef<Node>& LastElementInClause =
						InClause->AccessChildren().Last();
					LastElementInClause->SetNumNewLinesAfter(
						NumLeadingNewLinesTrailingElements);
				}
				else
				{
					// TODO: (yiliang.siew) This is possible with the syntax `{\n\n\n }`.
					// In such cases, we can't really reproduce the newlines since we
					// cannot capture this in the current VST. The VST would need to be
					// able to capture whitespace as separate VST elements in order for
					// this to work.
				}
			}
			const int32_t NumTrailingNewLines =
				InBlock.ElementsTrailing.CountNumTrailingNewLines();
			if (NumTrailingNewLines > 0 && !InClausePostfixComments.IsEmpty()) // e.g. "#comment/n/n"
			{
				InClausePostfixComments.Last()->SetNumNewLinesAfter(
					NumTrailingNewLines);
			}
			if (!InBlock.PunctuationTrailing.IsEmpty())
			{
				const int32_t PunctuationTrailingNumLeadingNewLines =
					InBlock.PunctuationTrailing.CountNumLeadingNewLines();
				if (PunctuationTrailingNumLeadingNewLines > 0)
				{
					InClause->SetNumNewLinesAfter(InClause->NumNewLinesAfter() + PunctuationTrailingNumLeadingNewLines);
				}
			}
		}
	}

	static Clause::EForm AsClauseForm(const BlockType& Block)
	{
		// NOTE: (yiliang.siew) We process each of the clause's elements that have
		// been captured thus far, in order to attach newline information, comments,
		// and so on.
		const int32_t NumTrailingNewLines = Block.ElementsTrailing.CountNumTrailingNewLines();

		if (Block.Elements.IsFilled() && (NumTrailingNewLines > 0))
		{
			Block.Elements.Last()->SetNumNewLinesAfter(NumTrailingNewLines);
			// NOTE: (yiliang.siew) Here, we transfer any trailing newlines from
			// trailing comments over to the block's last element, since the full
			// number of trailing newlines is known here to the block, but not to
			// the comment at the time when it is added (as part of the `NewLine`
			// callback.) This avoids "doubling-up" on newlines when blockcmts end
			// an expression.
			if (!Block.ElementsTrailing.Nodes.IsEmpty())
			{
				Block.ElementsTrailing.Nodes.Last()->SetNewLineAfter(false);
			}
		}
		// $Revisit - block has additional information that is not being passed on
		// return ((Block.Punctuation == Verse::Grammar::EPunctuation::Ind) ||
		// (Block.Form == Verse::Grammar::EForm::List)) Because the parser sets
		// blocks to `List` form by default, we only consider that it could have a
		// semicolon if there were more than a single element in the block.
		if (Block.Form == Verse::Grammar::EForm::List && Block.Elements.Num() > 1)
		{
			return Clause::EForm::HasSemicolonOrNewline;
		}
		else
		{
			return Clause::EForm::NoSemicolonOrNewline;
		}
	}

	//-------------------------------------------------------------------------------------------------
	static bool TransferFirstLeadingNewLineOfClauseMember(
		Clause& InClause, Clause& ClauseToApplyTrailingNewLineTo)
	{
		if (InClause.GetChildCount() == 0 || !InClause.GetChildren()[0]->HasNewLinesBefore())
		{
			return false;
		}

		TNodeRef<Node>& InClauseFirstChild = InClause.AccessChildren()[0];
		InClauseFirstChild->SetNumNewLinesBefore(
			InClauseFirstChild->NumNewLinesBefore() - 1);
		ClauseToApplyTrailingNewLineTo.SetNumNewLinesAfter(
			ClauseToApplyTrailingNewLineTo.NumNewLinesAfter() + 1);

		return true;
	}

	//-------------------------------------------------------------------------------------------------
	static CUTF8StringView AsStringView(const SText InText)
	{
		return CUTF8StringView(reinterpret_cast<const UTF8Char*>(InText.Start),
			reinterpret_cast<const UTF8Char*>(InText.Stop));
	}

	static CUTF8String AsString(const char8_t InChar)
	{
		UTF8Char U8Char;
		memcpy(&U8Char, &InChar, sizeof(UTF8Char));
		return CUTF8String("%c", U8Char);
	}

	//-------------------------------------------------------------------------------------------------
	// Convert null terminated string to parser token and then convert to uint8_t
	// form
	static uint8_t Token8(const CUTF8String& TokenStr)
	{
		return uint8_t(SToken((const char8_t*)TokenStr.AsUTF8()));
	}

	//-------------------------------------------------------------------------------------------------
	// Crop snippet by 1 on either side
	static SSnippet CropSnippet1(const SSnippet& Snippet)
	{
		SSnippet CroppedSnippet;

		CroppedSnippet.Text = SText(Snippet.Text.Start + 1, Snippet.Text.Stop - 1);
		CroppedSnippet.StartLine = Snippet.StartLine,
		CroppedSnippet.StopLine = Snippet.StopLine,
		CroppedSnippet.StartColumn = Snippet.StartColumn + 1u,
		CroppedSnippet.StopColumn = Snippet.StopColumn - 1u;

		return CroppedSnippet;
	}

	//-------------------------------------------------------------------------------------------------
	static SLocus AsLocus(const SSnippet& Snippet)
	{
		// Converts from snippet:
		//   int64_t startline;   // inclusive, 1-based
		//   int64_t startpos;    // inclusive, 1-based
		//   int64_t endline;     // inclusive, 1-based
		//   int64_t endpos;      // exclusive, 1-based
		//
		// To SLocus:
		//    uint32_t _BeginRow;   // inclusive, 0-based
		//    uint32_t _BeginColum; // inclusive, 0-based
		//    uint32_t _EndRow;     // inclusive, 0-based
		//    uint32_t _EndColumn;  // exclusive, 0-based

		return SLocus{static_cast<uint32_t>(Snippet.StartLine) - 1u,
			static_cast<uint32_t>(Snippet.StartColumn) - 1u,
			static_cast<uint32_t>(Snippet.StopLine) - 1u,
			static_cast<uint32_t>(Snippet.StopColumn) - 1u};
	}

	//-------------------------------------------------------------------------------------------------
	// Make a locus of zero size just before the first character of the snippet
	// - used for synthetically inserted code to ensure that the locations do not
	// overlap.
	static SLocus AsLocus0(const SSnippet& Snippet)
	{
		return SLocus{
			static_cast<uint32_t>(Snippet.StartLine) - 1u,
			static_cast<uint32_t>(Snippet.StartColumn) - 1u,
			static_cast<uint32_t>(Snippet.StartLine) - 1u,
			static_cast<uint32_t>(Snippet.StartColumn) - 1u,
		};
	}

	//-------------------------------------------------------------------------------------------------
	static SLocus AsTokenLocus(const SSnippet& Snippet, const SText& TokenText)
	{
		return SLocus{static_cast<uint32_t>(Snippet.StartLine) - 1u,
			static_cast<uint32_t>(Snippet.StartColumn) - 1u,
			static_cast<uint32_t>(Snippet.StartLine) - 1u,
			static_cast<uint32_t>(Snippet.StartColumn + Verse::Grammar::Length(TokenText)) - 1u};
	}

	//-------------------------------------------------------------------------------------------------
	static SLocus AsTokenLocusPostfix(const SSnippet& Snippet,
		const SText& TokenText)
	{
		return SLocus{static_cast<uint32_t>(Snippet.StopLine) - 1u,
			static_cast<uint32_t>(Snippet.StopColumn - Verse::Grammar::Length(TokenText)) - 1u,
			static_cast<uint32_t>(Snippet.StopLine) - 1u,
			static_cast<uint32_t>(Snippet.StopColumn) - 1u};
	}

	//-------------------------------------------------------------------------------------------------
	static SLocus AsTokenLocusPrefix(const SSnippet& Snippet,
		const SText& TokenText)
	{
		return SLocus{static_cast<uint32_t>(Snippet.StartLine) - 1u,
			static_cast<uint32_t>(Snippet.StartColumn - Verse::Grammar::Length(TokenText)) - 1u,
			static_cast<uint32_t>(Snippet.StartLine) - 1u,
			static_cast<uint32_t>(Snippet.StartColumn) - 1u};
	}

	//-------------------------------------------------------------------------------------------------
	static SLocus LocusTokenPostfix(const SLocus& Locus, const SText& TokenText)
	{
		return SLocus{Locus.EndRow(), Locus.EndColumn(), Locus.EndRow(),
			Locus.EndColumn() + static_cast<uint32_t>(Verse::Grammar::Length(TokenText))};
	}

	//-------------------------------------------------------------------------------------------------
	template <typename... ResultArgsType>
	TSPtr<SGlitch> NewGlitch(const SLocus& Whence, EDiagnostic Diagnostic,
		ResultArgsType&&... ResultArgs) const noexcept
	{
		return TSPtr<SGlitch>::New(
			SGlitchResult(Diagnostic,
				uLang::ForwardArg<ResultArgsType>(ResultArgs)...),
			SGlitchLocus(_SnippetPath, Whence));
	}

	//-------------------------------------------------------------------------------------------------
	template <typename... ResultArgsType>
	void AppendGlitch(const SLocus& Whence, EDiagnostic Diagnostic,
		ResultArgsType&&... ResultArgs) const noexcept
	{
		_Diagnostics->AppendGlitch(TSRef<SGlitch>::New(
			SGlitchResult(Diagnostic,
				uLang::ForwardArg<ResultArgsType>(ResultArgs)...),
			SGlitchLocus(_SnippetPath, Whence)));
	}

	//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
	// Data members
	// Should be stateless - because the parser doesn't invoke callbacks in
	// left-to-right order
	TSRef<uLang::CDiagnostics> _Diagnostics;

	/// The path to the snippet being parsed and a VST being generated for.
	const CUTF8String& _SnippetPath;

	/// The behaviour that has been set for this current parse operation.
	EParseBehaviour _ParseBehaviour;

	/// These control backwards-compatible changes in the generator
	const uint32_t _VerseVersion;
	const uint32_t _UploadedAtFNVersion;

}; // SGenerateVst

//-------------------------------------------------------------------------------------------------
void CParserPass::ProcessSnippet(const Verse::Vst::TNodeRef<Verse::Vst::Snippet>& OutVst,
	const CUTF8String& TextSnippet,
	const SBuildContext& BuildContext,
	const uint32_t VerseVersion,
	const uint32_t UploadedAtFNVersion) const
{
	OutVst->Empty();

	NodeArray VstRoot;
	SGenerateVst VstGenerator(BuildContext._Diagnostics, OutVst->_Path,
		SGenerateVst::EParseBehaviour::ParseAll,
		VerseVersion, UploadedAtFNVersion);

	const uint64_t ByteLen = TextSnippet.ByteLen();
	const char8_t* const Data = reinterpret_cast<const char8_t*>(TextSnippet.AsUTF8());

	SGenerateVst::ResultType Result = Verse::Grammar::File(VstGenerator, ByteLen, Data);

	if (Result)
	{
		const Verse::Vst::TNodeRef<Verse::Vst::Clause> FileClause =
			Result->As<Verse::Vst::Clause>();
		Verse::Vst::Node::TransferChildren(FileClause, OutVst);
		OutVst->SetForm(FileClause->GetForm());

		if (OutVst->GetChildCount())
		{
			OutVst->SetWhence(SGenerateVst::CombineLocus(OutVst->GetChildren()));
		}
	}
	else
	{
		BuildContext._Diagnostics->AppendGlitch(Result.GetError());
	}
}

} // namespace uLang

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
