// Copyright Epic Games, Inc. All Rights Reserved.
// Dependency-free allocation-free single-header Verse grammar library.
//--------------------------------------------------------------------------------------------------------------------------------------------------------------

#pragma once

#include "uLang/Common/Common.h"
#include <stdint.h>
#include <utility>

namespace Verse
{
namespace Grammar
{

#ifndef VERSE_MAX_EXPR_DEPTH
#define VERSE_MAX_EXPR_DEPTH 100
#endif

#ifndef VERSE_MAX_INDCMT_DEPTH
#define VERSE_MAX_INDCMT_DEPTH 3
#endif

// Macros.
#define ULANG_GRAMMAR_RUN(e)               \
	{                                      \
		auto GrammarTemp = (e);            \
		if (!GrammarTemp)                  \
			return GrammarTemp.GetError(); \
	}
#define ULANG_GRAMMAR_SET(r, e)            \
	{                                      \
		auto GrammarTemp = (e);            \
		if (!GrammarTemp)                  \
			return GrammarTemp.GetError(); \
		r = *GrammarTemp;                  \
	}
#define ULANG_GRAMMAR_LET(r, e)   \
	auto r##Let = (e);            \
	if (!r##Let)                  \
		return r##Let.GetError(); \
	auto r = *r##Let;

// Basic functions.
template <class ElementType, uint64_t ArrayLength>
constexpr uint64_t ArraySize(ElementType (&)[ArrayLength])
{
	return ArrayLength;
}

// Trivial type.
struct SNothing
{
};

// Precedence.
enum class EPrec : uint8_t
{
	Never,
	List,
	Commas,
	Expr,
	Fun,
	Def,
	Or,
	And,
	Not,
	Eq,
	NotEq,
	Less,
	Greater,
	Choose,
	To,
	Add,
	Mul,
	Prefix,
	Call,
	Base,
	Nothing
};

// Associativity.
enum class EAssoc : uint8_t
{
	None,
	Postfix,
	InfixLeft,
	InfixRight
};

// Block form.
enum class EForm : uint8_t
{
	List,
	Commas
};

// Block punctuation.
enum class EPunctuation : uint8_t
{
	None,
	Braces,
	Parens,
	Brackets,
	AngleBrackets,
	Qualifier,
	Dot,
	Colon,
	Ind
};

// Places specializing capture generation.
enum class EPlace : uint16_t
{
	UTF8,
	Printable,
	BlockCmt,
	LineCmt,
	IndCmt,
	Space,
	String,
	Content
};

// Modes for calling: none (error if instantiated), of (failure disallowed), at
// (failure allowed), with (macro).
enum class EMode
{
	None,
	Open,
	Closed,
	With
};

// Sets a variable on construction, and restores its previous value on
// destruction.
template <typename GuardVariableType>
struct TScopedGuard
{
	TScopedGuard(GuardVariableType& InGuardVariable,
		const GuardVariableType& NewValue)
		: _GuardVariable(&InGuardVariable)
		, _OldValue(InGuardVariable)
	{
		*_GuardVariable = NewValue;
	}
	~TScopedGuard()
	{
		if (_GuardVariable)
			*_GuardVariable = _OldValue;
	}

private:
	GuardVariableType* _GuardVariable;
	GuardVariableType _OldValue;
};

// Text spans passed around by the parser.
struct SText
{
	const char8_t *Start, *Stop;
	constexpr SText()
		: Start(nullptr)
		, Stop(nullptr) {}
	constexpr SText(const char8_t* Start0, const char8_t* Stop0)
		: Start(Start0)
		, Stop(Stop0)
	{
		ULANG_ASSERT(Stop >= Start);
	}
	constexpr SText(const char8_t* Start0)
		: SText(Start0, Start0)
	{
		while (*Stop)
			++Stop;
	}
	SText(const char* Start0)
		: SText(reinterpret_cast<const char8_t*>(Start0),
			(const char8_t*)Start0)
	{
		while (*Stop)
			++Stop;
	}
	constexpr char8_t operator[](uint64_t i) const
	{
		ULANG_ASSERT(Start + i < Stop);
		return Start[i];
	}
	explicit operator bool() const { return Start != Stop; }
};
constexpr uint64_t Length(const SText& Text)
{
	return Text.Stop - Text.Start;
}
inline bool operator==(const SText& as, const SText& bs)
{
	if (Length(as) != Length(bs))
		return 0;
	for (uint64_t i = 0; i < Length(as); i++)
		if (as.Start[i] != bs.Start[i])
			return 0;
	return 1;
}
inline bool operator!=(const SText& as, const SText& bs)
{
	return !(as == bs);
}

// A snippet of text describing its location.
struct SSnippet
{
	SText Text;
	uint64_t StartLine, StopLine;
	uint64_t StartColumn, StopColumn;
	SSnippet()
		: Text(nullptr, nullptr)
		, StartLine(0)
		, StopLine(0)
		, StartColumn(0)
		, StopColumn(0) {}
	explicit operator bool() const { return bool(Text); }

private:
	// Private to ensure all non-empty snippets are within the string passed to
	// the parser.
	friend struct CParserBase;
	SSnippet(const char8_t* Start0, const char8_t* End0, uint64_t StartLine0,
		uint64_t StopLine0, uint64_t StartColumn0, uint64_t StopColumn0)
		: Text(Start0, End0)
		, StartLine(StartLine0)
		, StopLine(StopLine0)
		, StartColumn(StartColumn0)
		, StopColumn(StopColumn0) {}
};

// Verse blocks.
template <class SyntaxesType, class CaptureType>
struct SBlock
{
	SBlock(const SSnippet& BlockSnippet0 = SSnippet{},
		const SyntaxesType& Elements0 = SyntaxesType(),
		EForm Form0 = EForm::List)
		: BlockSnippet(BlockSnippet0)
		, Punctuation(EPunctuation::None)
		, Form(Form0)
		, Elements(Elements0) {}
	SSnippet BlockSnippet;           // Snippet of the whole block.
	SyntaxesType Specifiers;         // Specifiers.
	CaptureType TokenLeading;        // If Token, the Scan before it.
	SText Token;                     // Token preceding opening EPunctuation.
	CaptureType PunctuationLeading;  // After SToken, before opening EPunctuation;
									 // present only if Punctuation.
	EPunctuation Punctuation;        // Punctuation wrapping the list.
	EForm Form;                      // Commas or List.
	SyntaxesType Elements;           // Elements.
	CaptureType ElementsTrailing;    // Scan between elements and closing
									 // EPunctuation or end.
	CaptureType PunctuationTrailing; // If Punctuation, this holds Space & NewLine
									 // STrailing it.
};

// Results consisting of either a value or an error.
template <class ValueType, class ErrorType>
struct TResult
{
	template <class SourceType,
		class = decltype(ValueType(*(SourceType*)nullptr))>
	TResult(const SourceType& Value0)
		: Value(Value0)
		, Success(true) {}
	template <class SourceType,
		class = decltype(ValueType(*(SourceType*)nullptr))>
	TResult(SourceType&& Value0)
		: Value(std::move(Value0))
		, Success(true) {}
	template <class DefaultErrorType = ErrorType,
		class = decltype(DefaultErrorType())>
	TResult()
		: Error()
		, Success(false) {}
	TResult(const ErrorType& Error0)
		: Error(Error0)
		, Success(false) {}
	TResult(const TResult& Other)
		: Success(Other.Success)
	{
		if (Other.Success)
			new (&Value) ValueType(Other.Value);
		else
			new (&Error) ErrorType(Other.Error);
	}
	~TResult()
	{
		if (Success)
		{
			Value.~ValueType();
		}
		else
		{
			Error.~ErrorType();
		}
	}
	operator bool() const { return Success; }
	TResult& operator=(const TResult& R)
	{
		if (this != &R)
		{
			this->~TResult();
			new (this) TResult(R);
		}
		return *this;
	}
	const ValueType& operator*() const
	{
		ULANG_ASSERT(Success);
		return Value;
	}
	ValueType* operator->()
	{
		ULANG_ASSERT(Success);
		return &Value;
	}
	const ErrorType& GetError() const
	{
		ULANG_ASSERT(!Success);
		return Error;
	}

private:
	union
	{
		ValueType Value;
		ErrorType Error;
	};
	bool Success;
};

//--------------------------------------------------------------------------------------------------------------------------------------------------------------
// Low-level character classification.

// Verse grammar character classification functions.
constexpr bool IsSpace(char8_t c)
{
	return c == ' ' || c == '\t';
}
constexpr bool IsNewLine(char8_t c)
{
	return c == 0x0D || c == 0x0A;
}
constexpr bool IsEnding(char8_t c)
{
	return c == 0 || c == 0x0D || c == 0x0A;
}
constexpr bool IsAlpha(char8_t c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
} // Parentheses `()` required to make static analysis happy
constexpr bool IsDigit(char8_t c)
{
	return c >= '0' && c <= '9';
}
constexpr bool IsAlnum(char8_t c)
{
	return IsAlpha(c) || IsDigit(c);
}
constexpr bool IsHex(char8_t c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}
constexpr uint8_t DigitValue(char8_t c)
{
	return (c >= '0' && c <= '9') ? c - '0'
		 : (c >= 'A' && c <= 'F') ? c - 'A' + 10
		 : (c >= 'a' && c <= 'f') ? c - 'a' + 10
								  : 0;
}
constexpr bool IsIdentifierQuotable(char8_t c0, char8_t c1)
{
	return uint8_t(c0) >= 0x20 && uint8_t(c0) <= 0x7E && c0 != '{' && c0 != '}' && c0 != '"' && c0 != '\'' && c0 != '\\' && (c0 != '<' || c1 != '#') && (c0 != '#' || c1 != '>');
}
constexpr bool IsStringBackslashLiteral(char8_t c0, char8_t c1)
{
	return c0 == 'r' || c0 == 'n' || c0 == 't' || c0 == '\\' || c0 == '"' || c0 == '\'' || (c0 == '<' && c1 != '#') || c0 == '>' || (c0 == '#' && c1 != '>') || c0 == '&' || c0 == '~' || c0 == '{' || c0 == '}';
}

// Convert valid UTF-8 sequence with valid length to its Unicode Code Point.
inline char32_t EncodedChar32(const char8_t* s, uint64_t Count)
{
	switch (Count)
	{ // Extra `uint8_t` casts for when `char8_t` is signed in
	  // certain circumstances
		case 1:
			return char32_t(uint8_t(s[0]));
		case 2:
			return char32_t(
				(uint32_t(uint8_t(s[0])) * 0x40 + uint32_t(uint8_t(s[1]) & 0x3F)) & 0x7FF);
		case 3:
			return char32_t((uint32_t(uint8_t(s[0])) * 0x1000 + uint32_t(uint8_t(s[1]) & 0x3F) * 0x40 + uint32_t(uint8_t(s[2]) & 0x3F)) & 0xFFFF);
		case 4:
			return char32_t((uint32_t(uint8_t(s[0])) * 0x40000 + uint32_t(uint8_t(s[1]) & 0x3F) * 0x1000 + uint32_t(uint8_t(s[2]) & 0x3F) * 0x40 + uint32_t(uint8_t(s[3]) & 0x3F)) & 0x1FFFFF);
		default:
			ULANG_UNREACHABLE();
	}
}

// Get length of internal lexical unit recognized for Place.
// U8        := 0o80..0oBF
// UTF8      :=                                      0o00..0o7F
//           |                                       0oC2..0oDF U8
//           |  !(0oE0 0o80..0o9F | 0oED 0oA0..0oBF) 0oE0..0oEF U8 U8
//           |  !(0oF0 0o80..0o8F | 0oF4 0o90..0oBF) 0oF0..0oF4 U8 U8 U8
// Printable := 0o09 | !("<#" | "#>" | 0o0..0o1F | 0o7F | 0oC2 0o80..0o9F | 0oE2
// 0o80 0oA8..0oA9 ) UTF8 | .. Special   := '\'|'{'|'}'|'#'|'<'|'>'|'&'|'~'
// String    := .. !('\'|'{'|'}'|'"') Text ..
// Content   := .. !Special Text ..
template <EPlace Place>
uint64_t EncodedLength(const char8_t* const S)
{
	struct SJumpTable final
	{
		using JumpTableFunctionType = uint64_t (*)(const char8_t* const S);
		JumpTableFunctionType JumpTable[256];

		constexpr SJumpTable()
		{
			JumpTableFunctionType ReturnOne = [](const char8_t* const) -> uint64_t {
				return 1;
			};

			// This is the default fallback case - 0 length.
			for (int Index = 0; Index < 256; Index++)
			{
				JumpTable[Index] = [](const char8_t* const) -> uint64_t {
					return 0;
				};
			}

			if constexpr (Place == EPlace::UTF8)
			{
				for (int Index : {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x7F})
				{
					JumpTable[Index] = ReturnOne;
				}
			}

			for (int Index : {0x09, 0x20})
			{
				JumpTable[Index] = ReturnOne;
			}

			if constexpr (Place != EPlace::Space && Place != EPlace::String)
			{
				JumpTable[static_cast<int>('"')] = ReturnOne;
			}

			if constexpr (Place == EPlace::UTF8)
			{
				JumpTable[static_cast<int>('<')] = ReturnOne;
			}
			else if constexpr (Place != EPlace::Space && Place != EPlace::Content)
			{
				JumpTable[static_cast<int>('<')] = [](const char8_t* const S) -> uint64_t {
					return S[1] != '#' ? 1 : 0;
				};
			}

			if constexpr (Place == EPlace::UTF8)
			{
				JumpTable[static_cast<int>('#')] = ReturnOne;
			}
			else if constexpr (Place != EPlace::Space)
			{
				JumpTable[static_cast<int>('#')] = [](const char8_t* const S) -> uint64_t {
					return S[1] != '>' ? 1 : 0;
				};
			}

			if constexpr (Place != EPlace::Space && Place != EPlace::String && Place != EPlace::Content)
			{
				for (int Index : {'\\', '{', '}'})
				{
					JumpTable[Index] = ReturnOne;
				}
			}

			if constexpr (Place != EPlace::Space && Place != EPlace::Content)
			{
				for (int Index : {'>', '&', '~'})
				{
					JumpTable[Index] = ReturnOne;
				}
			}

			if constexpr (Place != EPlace::Space)
			{
				for (int Index : {'!', '$', '%', '\'', '(', ')', '*', '+', ',', '-', '.', '/', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '=', '?', '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '[', ']', '^', '_', '`', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '|'})
				{
					JumpTable[Index] = ReturnOne;
				}

				JumpTable[0xC2] = [](const char8_t* const S) -> uint64_t {
					return uint8_t(S[1]) >= 0x80 && uint8_t(S[1]) <= 0xBF && (Place == EPlace::UTF8 || uint8_t(S[1]) >= 0xA0)
							 ? 2
							 : 0;
				};

				for (int Index : {0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF})
				{
					JumpTable[Index] = [](const char8_t* const S) -> uint64_t {
						return uint8_t(S[1]) >= 0x80 && uint8_t(S[1]) <= 0xBF
								 ? 2
								 : 0;
					};
				}

				JumpTable[0xE0] = [](const char8_t* const S) -> uint64_t {
					return uint8_t(S[1]) >= 0xA0 && uint8_t(S[1]) <= 0xBF && uint8_t(S[2]) >= 0x80 && uint8_t(S[2]) <= 0xBF
							 ? 3
							 : 0;
				};

				JumpTable[0xE2] = [](const char8_t* const S) -> uint64_t {
					return (uint8_t(S[1]) >= 0x80) && (uint8_t(S[1]) <= 0xBF) && (uint8_t(S[2]) >= 0x80) && (uint8_t(S[2]) <= 0xBF) && ((uint8_t(S[1]) != 0x80) || ((uint8_t(S[2]) != 0xA8) && (uint8_t(S[2]) != 0xA9)))
							 ? 3
							 : 0;
				};

				for (int Index : {0xE1, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xEE, 0xEF})
				{
					JumpTable[Index] = [](const char8_t* const S) -> uint64_t {
						return uint8_t(S[1]) >= 0x80 && uint8_t(S[1]) <= 0xBF && uint8_t(S[2]) >= 0x80 && uint8_t(S[2]) <= 0xBF
								 ? 3
								 : 0;
					};
				}

				JumpTable[0xED] = [](const char8_t* const S) -> uint64_t {
					return uint8_t(S[1]) >= 0x80 && uint8_t(S[1]) <= 0x9F && uint8_t(S[2]) >= 0x80 && uint8_t(S[2]) <= 0xBF
							 ? 3
							 : 0;
				};

				JumpTable[0xF0] = [](const char8_t* const S) -> uint64_t {
					return uint8_t(S[1]) >= 0x90 && uint8_t(S[1]) <= 0xBF && uint8_t(S[2]) >= 0x80 && uint8_t(S[2]) <= 0xBF && uint8_t(S[3]) >= 0x80 && uint8_t(S[3]) <= 0xBF
							 ? 4
							 : 0;
				};

				for (int Index : {0xF1, 0xF2, 0xF3})
				{
					JumpTable[Index] = [](const char8_t* const S) -> uint64_t {
						return uint8_t(S[1]) >= 0x80 && uint8_t(S[1]) <= 0xBF && uint8_t(S[2]) >= 0x80 && uint8_t(S[2]) <= 0xBF && uint8_t(S[3]) >= 0x80 && uint8_t(S[3]) <= 0xBF
								 ? 4
								 : 0;
					};
				}

				JumpTable[0xF4] = [](const char8_t* const S) -> uint64_t {
					return uint8_t(S[1]) >= 0x80 && uint8_t(S[1]) <= 0x8F && uint8_t(S[2]) >= 0x80 && uint8_t(S[2]) <= 0xBF && uint8_t(S[3]) >= 0x80 && uint8_t(S[3]) <= 0xBF
							 ? 4
							 : 0;
				};
			}
		}
	};

	static const SJumpTable JT;
	return JT.JumpTable[*S](S);
}

//--------------------------------------------------------------------------------------------------------------------------------------------------------------
// Grammar output.
// This is not ready for use, but will later provide a Verse and VSON output
// library and pretty-printing API.

// Grammar output SEncoding.
struct SEncoding
{
	EPrec Prec;
	bool AllowIn, FollowingIn;
	SEncoding(EPrec Prec0 = EPrec::List, bool AllowIn0 = false,
		bool FollowingIn0 = false)
		: Prec(Prec0)
		, AllowIn(AllowIn0)
		, FollowingIn(FollowingIn0) {}
	SEncoding Fresh(EPrec Prec1, bool AllowIn1 = false,
		bool FollowingIn0 = false) const
	{
		return SEncoding(Prec1, AllowIn1 || Prec1 <= EPrec::Def, FollowingIn0);
	}
};
inline bool ParenthesizePrefix(const SEncoding& Encoding, EPrec StringPrec)
{
	return StringPrec < Encoding.Prec;
}
inline bool ParenthesizePostfix(const SEncoding& Encoding, EPrec StringPrec)
{
	return StringPrec < Encoding.Prec || ((Encoding.Prec == EPrec::Less) && (StringPrec == EPrec::Greater));
}

//--------------------------------------------------------------------------------------------------------------------------------------------------------------
// Token table.

// Forward declared tokens.
extern const struct STokenSet AllTokens, AllowLess, AllowNotEq;

// Token information.
struct STokenInfo
{
	const char8_t* Symbol;
	EPrec PrefixPrec;
	EMode PrefixMode;
	EPrec PostfixTokenPrec;
	EPrec PostfixPrec;
	EAssoc PostfixAssoc;
	EMode PostfixMode;
	const STokenSet& PostfixAllowMask;
	SEncoding PostfixLeftEncoding(const SEncoding& Encoding, bool Parens) const
	{
		ULANG_ASSERT(PostfixAssoc == EAssoc::Postfix || PostfixAssoc == EAssoc::InfixLeft || PostfixAssoc == EAssoc::InfixRight);
		bool AllowIn = Encoding.AllowIn || Encoding.Prec <= EPrec::Def || Parens;
		if (PostfixAssoc == EAssoc::Postfix || PostfixAssoc == EAssoc::InfixLeft)
			return Encoding.Fresh(PostfixPrec, AllowIn);
		else
			return Encoding.Fresh(EPrec(uint64_t(PostfixPrec) + 1), AllowIn);
	}
	SEncoding PostfixRightEncoding(const SEncoding& Encoding, bool Parens) const
	{
		ULANG_ASSERT(PostfixAssoc == EAssoc::InfixLeft || PostfixAssoc == EAssoc::InfixRight);
		if (PostfixAssoc == EAssoc::InfixRight)
			return Encoding.Fresh(PostfixPrec, false,
				Encoding.FollowingIn && !Parens);
		else
			return Encoding.Fresh(EPrec(uint64_t(PostfixPrec) + 1), false,
				Encoding.FollowingIn && !Parens);
	}
	EPrec PostfixRightPrec() const
	{
		ULANG_ASSERT(PostfixAssoc == EAssoc::InfixLeft || PostfixAssoc == EAssoc::InfixRight);
		return PostfixAssoc == EAssoc::InfixRight
				 ? PostfixPrec
				 : EPrec(uint64_t(PostfixPrec) + 1);
	}
};
constexpr STokenInfo Tokens[] = {
	{		u8"",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,EAssoc::None,
     EMode::None,  AllTokens                      }, // unknown
	{		u8"",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      }, // end
	{		u8"",  EPrec::Never,   EMode::None,    EPrec::Call,    EPrec::Call,       EAssoc::None,
     EMode::None,  AllTokens                      }, // NewLine
	{		u8"",   EPrec::Base,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      }, // Alpha
	{		u8"",   EPrec::Base,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      }, // Digit
	{   u8"alias",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{	 u8"and",   EPrec::Base,   EMode::None,     EPrec::And,     EPrec::And,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	  u8"at",   EPrec::Base,   EMode::None,    EPrec::Call,    EPrec::Call,       EAssoc::None,
     EMode::Closed,  AllTokens                    },
	{   u8"break",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{   u8"catch",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{   u8"const",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{u8"continue",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{	  u8"do",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{    u8"else",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{	  u8"if",   EPrec::Base,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	  u8"in",    EPrec::Def,   EMode::With,     EPrec::Def,  EPrec::Choose,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	  u8"is",  EPrec::Never,   EMode::None,     EPrec::Def,     EPrec::Def,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{    u8"live",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{ u8"mutable",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{    u8"next",   EPrec::Base,   EMode::None,     EPrec::Fun,     EPrec::Fun,
     EAssoc::InfixRight,   EMode::None,  AllTokens},
	{	 u8"not",    EPrec::Not,   EMode::With,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	  u8"of",   EPrec::Base,   EMode::None,    EPrec::Call,    EPrec::Call,       EAssoc::None,
     EMode::Open,  AllTokens                      },
	{	  u8"or",   EPrec::Base,   EMode::None,      EPrec::Or,      EPrec::Or, EAssoc::InfixRight,
     EMode::With,  AllTokens                      },
	{    u8"over",   EPrec::Base,   EMode::None,     EPrec::Fun,     EPrec::Fun,
     EAssoc::InfixLeft,   EMode::With,  AllTokens },
	{	 u8"set",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	 u8"ref",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::With,  AllTokens                      },
	{  u8"return",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{    u8"then",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{	  u8"to",   EPrec::Base,   EMode::None,      EPrec::To,      EPrec::To, EAssoc::InfixRight,
     EMode::With,  AllTokens                      },
	{   u8"until",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{    u8"when",   EPrec::Base,   EMode::None,     EPrec::Fun,     EPrec::Fun,
     EAssoc::InfixLeft,   EMode::With,  AllTokens },
	{   u8"where",  EPrec::Never,   EMode::None,     EPrec::Def,     EPrec::Def,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{   u8"while",   EPrec::Base,   EMode::None,     EPrec::Fun,     EPrec::Fun,
     EAssoc::InfixLeft,   EMode::With,  AllTokens },
	{    u8"with",  EPrec::Never,   EMode::None,    EPrec::Call,    EPrec::Call,
     EAssoc::None,   EMode::None,  AllTokens      },
	{   u8"yield",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{	 u8"var",    EPrec::Def,   EMode::With,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8",",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8";",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8"!",    EPrec::Not,   EMode::With,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	  u8"\"",   EPrec::Base,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8"&",    EPrec::Def,   EMode::None,     EPrec::Mul,     EPrec::Mul,  EAssoc::InfixLeft,
     EMode::With,  AllTokens                      },
	{	   u8"'",   EPrec::Base,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8"(",   EPrec::Base,   EMode::None,    EPrec::Call,    EPrec::Call,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8")",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8"*", EPrec::Prefix, EMode::Closed,     EPrec::Mul,     EPrec::Mul,
     EAssoc::InfixLeft, EMode::Closed,  AllTokens },
	{	  u8"*=",  EPrec::Never,   EMode::None,     EPrec::Def,     EPrec::Def,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	   u8"+", EPrec::Prefix, EMode::Closed,     EPrec::Add,     EPrec::Add,
     EAssoc::InfixLeft, EMode::Closed,  AllTokens },
	{	  u8"+=",  EPrec::Never,   EMode::None,     EPrec::Def,     EPrec::Def,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	   u8"-", EPrec::Prefix, EMode::Closed,     EPrec::Add,     EPrec::Add,
     EAssoc::InfixLeft, EMode::Closed,  AllTokens },
	{	  u8"-=",  EPrec::Never,   EMode::None,     EPrec::Def,     EPrec::Def,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	  u8"->",  EPrec::Never,   EMode::None,      EPrec::To,      EPrec::To,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	   u8".",  EPrec::Never,   EMode::None,    EPrec::Call,    EPrec::Call,
     EAssoc::InfixLeft,   EMode::With,  AllTokens },
	{	  u8"..",    EPrec::Def,   EMode::With,      EPrec::To,      EPrec::To, EAssoc::InfixRight,
     EMode::With,  AllTokens                      },
	{	   u8"/",   EPrec::Base,   EMode::None,     EPrec::Mul,     EPrec::Mul,  EAssoc::InfixLeft,
     EMode::Closed,  AllTokens                    },
	{	  u8"/=",  EPrec::Never,   EMode::None,     EPrec::Def,     EPrec::Def,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	   u8":",    EPrec::Def,   EMode::With,    EPrec::Call,  EPrec::Choose,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	  u8":=",  EPrec::Never,   EMode::None,     EPrec::Def,     EPrec::Def,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	  u8":)",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{	  u8":>",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,
     EAssoc::None,   EMode::None,  AllTokens      },
	{	   u8"<",   EPrec::Base,   EMode::None,    EPrec::Call,    EPrec::Less,
     EAssoc::InfixRight, EMode::Closed,  AllowLess},
	{	  u8"<=",  EPrec::Never,   EMode::None,    EPrec::Less,    EPrec::Less,
     EAssoc::InfixRight, EMode::Closed,  AllowLess},
	{	  u8"<>",  EPrec::Never,   EMode::None,   EPrec::NotEq,   EPrec::NotEq,
     EAssoc::InfixLeft,   EMode::With, AllowNotEq },
	{	   u8"=",  EPrec::Never,   EMode::None,      EPrec::Eq,      EPrec::Eq,  EAssoc::InfixLeft,
     EMode::With,  AllTokens                      },
	{	  u8"==",  EPrec::Never,   EMode::None,      EPrec::Eq,   EPrec::Never,       EAssoc::None,
     EMode::Closed,  AllTokens                    },
	{	  u8"=>",  EPrec::Never,   EMode::None,     EPrec::Fun,     EPrec::Fun,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	   u8">",  EPrec::Never,   EMode::None, EPrec::Greater, EPrec::Greater,
     EAssoc::InfixRight, EMode::Closed,  AllTokens},
	{	  u8">=",  EPrec::Never,   EMode::None, EPrec::Greater, EPrec::Greater,
     EAssoc::InfixRight, EMode::Closed,  AllTokens},
	{	   u8"?", EPrec::Prefix, EMode::Closed,    EPrec::Call,    EPrec::Call,
     EAssoc::Postfix,   EMode::With,  AllTokens   },
	{	   u8"@",   EPrec::Expr,   EMode::None,    EPrec::Expr,    EPrec::Expr,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8"[", EPrec::Prefix, EMode::Closed,    EPrec::Call,  EPrec::Prefix,
     EAssoc::InfixRight, EMode::Closed,  AllTokens},
	{	   u8"]",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8"^", EPrec::Prefix, EMode::Closed,    EPrec::Call,    EPrec::Call,
     EAssoc::Postfix,   EMode::With,  AllTokens   },
	{	   u8"{",  EPrec::Never,   EMode::None,    EPrec::Call,    EPrec::Call,       EAssoc::None,
     EMode::None,  AllTokens                      },
	{	   u8"|",  EPrec::Never,   EMode::None,  EPrec::Choose,  EPrec::Choose,
     EAssoc::InfixRight,   EMode::With,  AllTokens},
	{	   u8"}",  EPrec::Never,   EMode::None,   EPrec::Never,   EPrec::Never,       EAssoc::None,
     EMode::None,  AllTokens                      },
};

// Tokens.
struct SToken
{
	uint8_t Index;
	explicit constexpr SToken(uint8_t Index0)
		: Index(Index0) {}
	constexpr SToken(const char8_t* Op)
		: Index(uint8_t(ArraySize(Tokens) - 1))
	{
		for (; Index >= uint64_t(SToken::FirstParse()); Index--)
			for (uint64_t j = 0;; j++)
				if (Tokens[Index].Symbol[j] != char8_t(Op[j]))
					break;
				else if (!Op[j])
					return;
		Index = 0;
	}
	constexpr operator uint8_t() const { return Index; }
	static constexpr SToken None() { return SToken(uint8_t(0)); }
	static constexpr SToken End() { return SToken(1); }
	static constexpr SToken NewLine() { return SToken(2); }
	static constexpr SToken Alpha() { return SToken(3); }
	static constexpr SToken Digit() { return SToken(4); }
	static constexpr SToken FirstParse() { return SToken(5); }
	explicit operator bool() const { return Index != 0; }
	constexpr const STokenInfo* operator->() const { return &Tokens[Index]; }
};

// A set of tokens.
struct STokenSet
{
	constexpr STokenSet()
		: Bits{0, 0} {}
	template <class... RestTokenTypes>
	explicit constexpr STokenSet(SToken Token, RestTokenTypes... RestTokens)
		: STokenSet(RestTokens...)
	{
		Bits[uint8_t(Token) / 64] |= 1LL << (uint8_t(Token) & 63);
	}
	template <class... RestTokenTypes>
	explicit constexpr STokenSet(const char8_t* TokenStr,
		RestTokenTypes... RestTokens)
		: STokenSet(SToken(TokenStr), RestTokens...) {}
	constexpr bool Has(SToken T) const
	{
		return Bits[uint8_t(T) / 64] & (1LL << (uint8_t(T) & 63));
	}
	constexpr explicit operator bool() const { return Bits[0] || Bits[1]; }
	constexpr STokenSet operator&(const STokenSet& Other) const
	{
		return STokenSet(Bits[0] & Other.Bits[0], Bits[1] & Other.Bits[1]);
	}
	constexpr STokenSet operator|(const STokenSet& Other) const
	{
		return STokenSet(Bits[0] | Other.Bits[0], Bits[1] | Other.Bits[1]);
	}
	constexpr STokenSet operator~() const
	{
		return STokenSet(~Bits[0], ~Bits[1]);
	}

private:
	constexpr STokenSet(uint64_t Bits0, uint64_t Bits1)
		: Bits{Bits0, Bits1} {}
	uint64_t Bits[2];
};

//--------------------------------------------------------------------------------------------------------------------------------------------------------------
// Token sets.

inline const STokenSet AllTokens = ~STokenSet{};
inline const STokenSet AllowLess = ~STokenSet{u8">", u8">="};
inline const STokenSet AllowNotEq = ~STokenSet{u8">", u8">=", u8"<", u8"<="};
inline const STokenSet InPrefixes = STokenSet{u8":", u8"in"};
inline const STokenSet StopList =
	STokenSet{u8":)", u8")", u8"]", u8"}", SToken::NewLine(), SToken::End()};
inline const STokenSet StopExpr = StopList | STokenSet{u8";", u8","};
inline const STokenSet StopFun = StopExpr | STokenSet{u8"@"};
inline const STokenSet StopDef =
	StopFun | STokenSet{u8"=>", u8"next", u8"over", u8"when", u8"while"};
inline const STokenSet BracePostfixes = STokenSet{u8"{"};
inline const STokenSet BlockPostfixes = STokenSet{u8"{", u8".", u8":"};
inline const STokenSet ParenPostfixes = STokenSet{u8"("};
inline const STokenSet WithPostfixes = STokenSet{u8"with", u8"<"};
inline const STokenSet InvokePostfixes = BlockPostfixes | ParenPostfixes | WithPostfixes | STokenSet{u8"in", SToken::NewLine()};
inline const STokenSet MarkupPostfixes = STokenSet{u8",", u8";", u8">", u8":>"};
inline const STokenSet DefPostfixes =
	STokenSet{u8"=", u8":=", u8"+=", u8"-=", u8"*=", u8"/="};

//--------------------------------------------------------------------------------------------------------------------------------------------------------------
// Parser.

// Generator-independent base class of parser.
struct CParserBase
{
private:
	template <class>
	friend struct CParser;

	// A cursor tracks a parsing position in accordance with the Verse grammar,
	// and a snipping position that attributes NewLine to the Space preceding it.
	struct SCursor
	{
		const char8_t* Pos;           // Pointer to current parse position.
		const char8_t* LineStart;     // Pointer to start of line.
		const char8_t* NextLineStart; // If >Pos, indicates we've snipped the NewLine at Pos.
		SToken Token;                 // Token here.
		uint64_t TokenSize;           // Length of token.
		uint64_t Line;                // Zero-based line number.
		char8_t operator[](int64_t Offset) const { return Pos[Offset]; }
		bool SnippedNewLine() const { return NextLineStart > Pos; }
	};

	// A point for producing snippets.
	struct SPoint
	{
		const char8_t* Pos;
		uint64_t Line, Column;
		SPoint(const char8_t* Pos0, uint64_t Line0, uint64_t Column0)
			: Pos(Pos0)
			, Line(Line0)
			, Column(Column0) {}
		SPoint(const SCursor& Cursor)
			: Pos(Cursor.SnippedNewLine() ? Cursor.NextLineStart : Cursor.Pos)
			, Line(Cursor.SnippedNewLine() ? Cursor.Line + 1 : Cursor.Line)
			, Column(Cursor.SnippedNewLine()
						 ? 1
						 : uint64_t(Cursor.Pos - Cursor.LineStart + 1)) {}
		static SPoint Start(const SSnippet& Snippet)
		{
			return SPoint{Snippet.Text.Start, Snippet.StartLine, Snippet.StartColumn};
		}
		static SPoint Stop(const SSnippet& Snippet)
		{
			return SPoint{Snippet.Text.Stop, Snippet.StopLine, Snippet.StopColumn};
		}
	};

	// Grammar SContext coinciding with "push" and "pop" in the specification.
	struct SContext
	{
		const char8_t* BlockInd; // Start of the line that initiated our current
								 // indentation, or nullptr.
		const char8_t* TrimInd;  // BlockInd or a more indented block to specify
								 // further text trimming.
		bool Nest;               // Whether we accept lines with equal indentation to BlockInd.
		bool LinePrefix;         // Whether subsequent ScanKey and Commas lines should be
								 // prefixed with '&'.
		SContext()
			: BlockInd{u8""}
			, TrimInd{u8""}
			, Nest(true)
			, LinePrefix(true) {}
	};

	// Tokens.
	uint8_t FirstToken[256];              // First candidate token per leading char8_t.
	uint8_t NextToken[ArraySize(Tokens)]; // Next candidate token per token.
	SToken ParseToken(const char8_t* Start, uint64_t& Size)
	{
		if (Start[0] == 0)
			return Size = 0, SToken::End();
		for (uint8_t i = FirstToken[uint8_t(Start[0])]; i; i = NextToken[i])
		{
			if (i < SToken::FirstParse())
				return Size = 0, SToken(i);
			auto Symbol = Tokens[i].Symbol;
			uint64_t j;
			for (j = 0; Symbol[j] && Start[j] == Symbol[j]; j++)
				;
			if (Symbol[j] || (IsAlnum(Symbol[0]) && IsAlnum(Start[j])))
				continue;
			return Size = j, SToken(i);
		}
		return Size = 0, SToken::None();
	}

	// State and constructor.
	SCursor Cursor;
	SContext Context;
	uint32_t ExprDepth{0};
	uint32_t CommentDepth{0};
	const uint64_t InputLength;
	const char8_t* InputString;
	CParserBase(uint64_t InputLength0, const char8_t* InputString0,
		uint64_t Line0 = 1)
		: FirstToken{}
		, NextToken{}
		, Cursor{InputString0, InputString0, InputString0, SToken::None(), 0,
			  Line0}
		, InputLength(InputLength0)
		, InputString(InputString0)
	{
		ULANG_ASSERT(InputString != nullptr);
		ULANG_ASSERT(InputString[InputLength] == 0);
		for (uint64_t c = 0u; c < 128u; c++)
			FirstToken[c] = IsNewLine(char8_t(c)) ? SToken::NewLine()
						  : IsEnding(char8_t(c))  ? SToken::End()
						  : IsAlpha(char8_t(c))   ? SToken::Alpha()
						  : IsDigit(char8_t(c))   ? SToken::Digit()
												  : SToken::None();
		for (auto Token = uint8_t(SToken::FirstParse()); Token < ArraySize(Tokens);
			 Token++)
		{
			auto& First = FirstToken[uint64_t(Tokens[Token].Symbol[0])];
			if (First)
				NextToken[Token] = First;
			First = SToken(Token);
		}
	}

	// Consumption.
	void Next(uint64_t n)
	{
		while (n--)
		{
			ULANG_ASSERT(Cursor[0] != 0);
			Cursor.Pos++;
		}
	}
	bool Eat(const char8_t* s)
	{
		uint64_t n;
		for (n = 0; s[n]; n++)
			if (Cursor[n] != s[n])
				return false;
		return Cursor.Pos += n, true;
	}
	void EatToken() { Cursor.Pos += Cursor.TokenSize; }

	// Snippets.
	static SSnippet Snip(const SPoint& Start, const SPoint& Stop)
	{
		return SSnippet{Start.Pos, Stop.Pos, Start.Line,
			Stop.Line, Start.Column, Stop.Column};
	}
	SSnippet Snip(const SPoint& Start) const { return Snip(Start, Cursor); }
	SSnippet Snip() const { return Snip(Cursor, Cursor); }
	SText CursorQuote()
	{
		static const SText Quote[2] = {u8"", u8"\""};
		const uint8_t Cur0 = uint8_t(Cursor[0]);
		return Quote[Cur0 > 0x20 && Cur0 != '"' && Cur0 < 0x7F];
	}
	SText CursorText()
	{
		const uint8_t Cur0 = uint8_t(Cursor[0]);

		// Quoted.
		if ((Cur0 == '#' && Cursor[1] == '>') || (Cur0 == '<' && Cursor[1] == '#'))
			return SText(Cursor.Pos, Cursor.Pos + 2);
		if (IsAlpha(Cur0))
		{
			uint64_t n = 1;
			while (IsAlnum(Cursor[n]))
				n++;
			return SText(Cursor.Pos, Cursor.Pos + n);
		}
		if (Cur0 > 0x20 && Cur0 <= 0x7E)
			return SText(Cursor.Pos, Cursor.Pos + 1);

		// Not quoted.
		if (Cur0 == '"')
			return u8"'\"'";
		else if (Cur0 >= 128 && EncodedLength<EPlace::Printable>(Cursor.Pos))
			return u8"unicode character";
		else if (Cur0 >= 128)
			return u8"non-unicode character sequence";
		else if (Cur0 == '\r' || Cur0 == '\n')
			return u8"end of line";
		else if (Cur0 == '\t')
			return u8"tab";
		else if (Cur0 == ' ')
			return u8"space";
		else if (Cur0 == 0)
			return u8"end of file";
		else
			return u8"ASCII control character";
	}
};

// Generator-dependent parser.
template <class GeneratorType>
struct CParser : CParserBase
{
private:
	using SyntaxType = typename GeneratorType::SyntaxType;
	using SyntaxesType = typename GeneratorType::SyntaxesType;
	using ErrorType = typename GeneratorType::ErrorType;
	using CaptureType = typename GeneratorType::CaptureType;
	template <class ResultValueType>
	using ResultOf = TResult<ResultValueType, ErrorType>;

	// Constructor.
	const GeneratorType& Gen;
	CParser(const GeneratorType& Gen0, uint64_t n, const char8_t* Source0,
		uint64_t StartLine = 1)
		: // Accounts for null `Source0` which often occurs with empty files /
		  // etc.
		CParserBase(n, Source0 ? Source0 : u8"", StartLine)
		, Gen(Gen0)
	{
	}

	// Tracking STrailing captures across expressions and their postfixes so we
	// can assign them to the lexically outermost generator.
	struct STrailing
	{
		TResult<SCursor, SNothing> TrailingStart;
		CaptureType TrailingCapture;
		explicit operator bool() const { return bool(TrailingStart); }
		void MoveFrom(STrailing& Source)
		{
			ULANG_ASSERT(!TrailingStart);
			TrailingStart = Source.TrailingStart;
			TrailingCapture = Source.TrailingCapture;
			Source.TrailingStart = SNothing{};
		}
	};

	// Our extended internal block structure tracking block's trailing captures.
	struct SBlockInternal : public SBlock<SyntaxesType, CaptureType>
	{
		using SBlock<SyntaxesType, CaptureType>::SBlock;
		STrailing BlockTrailing;
	};

	// We track a stack of expressions and postfixes at increasing precedence so
	// that we can insert multi-precedence postfix operators like '<' and stop
	// subsequent parsing there. An SExpr is in one of three states (except
	// mid-update when these invariants don't hold):
	// - Uninitialized: no ExprSyntax, no Trailing, not Finished.
	// - Initialized:   has ExprSyntax, has Trailing, not Finished.
	// - Finished:      has ExprSyntax, no Trailing, is Finished.
	struct SExpr
	{
		SCursor Start;
		EPrec FinishPrec;
		TResult<SCursor, SNothing> Finished;
		SExpr* OuterExpr;
		STokenSet AllowPostfixes;
		TResult<SyntaxType, SNothing> ExprSyntax;
		CaptureType ExprLeading;
		STrailing Trailing; //-V730_NOINIT
		TResult<SCursor, SNothing> MarkupStart;
		bool MarkupFinished, ExprStop;
		struct SExpr* OuterMarkup;
		SText MarkupTag;
		SExpr* QualIdentTarget;
		SExpr(EPrec FinishPrec0, const SCursor& Start0, SExpr* OuterExpr0,
			STokenSet AllowPostfixes0 = STokenSet{},
			SExpr* QualIdentTarget0 = nullptr)
			: Start(Start0)
			, FinishPrec(FinishPrec0)
			, OuterExpr(OuterExpr0)
			, AllowPostfixes(AllowPostfixes0)
			, MarkupFinished(false)
			, ExprStop(false)
			, OuterMarkup(nullptr)
			, QualIdentTarget(QualIdentTarget0) {} //-V730
		SyntaxType operator*() const { return *ExprSyntax; }
		// Needs a virtual destructor since it has virtual methods or various static
		// analysis will complain
		virtual ~SExpr() = default;
		virtual ResultOf<SNothing> OnFinish(CParser& /*Parser*/)
		{
			ULANG_ASSERT(!Finished);
			ULANG_ASSERT(!OuterExpr || !OuterExpr->Finished);
			ULANG_ASSERT(Trailing);
			Finished = *Trailing.TrailingStart;
			return SNothing{};
		}
	};

	// Token management.
	void UpdateToken()
	{
		Cursor.Token = ParseToken(Cursor.Pos, Cursor.TokenSize);
		if (IsAlpha(Cursor.Token->Symbol[0]))
		{
			// Key := !Alnum Space !":="
			// When a reserved word is followed by a definition symbol, we demote it
			// to an identifier so that simple object notation supports all
			// identifiers including reserved words.
			SCursor KeyStart = Cursor;
			EatToken();
			auto SpaceResult = Space();
			auto IsIdentifier = Cursor.Token == SToken(u8":=");
			Cursor = KeyStart; // backtrack but could cache
			if (SpaceResult && IsIdentifier)
				Cursor.Token = SToken::Alpha();
		}
	}
	bool CheckToken()
	{
		auto SavedToken = Cursor.Token;
		UpdateToken();
		return Cursor.Token == SavedToken;
	}

	// Errors.
	ResultOf<SNothing> Require(const char8_t* Value,
		ErrorType (CParser::*OnError)(SText What))
	{
		if (!Eat(Value))
			return (this->*OnError)(Value);
		return SNothing{};
	}
	ResultOf<SNothing> RequireClose(SCursor Start, const char8_t* Open,
		const char8_t* Close,
		ErrorType (CParser::*OnError)(SText))
	{
		if (Eat(Close))
			return SNothing{};
		else if (!Ending())
			return (this->*OnError)(Close);
		else
			return Cursor = Start, S80(Open);
	}

	// Snippets.
	SSnippet SnipFinished(const SCursor& Start, const SExpr& End)
	{
		return Snip(Start, *End.Finished);
	}
	SSnippet SnipFinished(const SCursor& Start, const SBlockInternal& End)
	{
		return Snip(Start, *End.BlockTrailing.TrailingStart);
	}

	// Trailing capture and SSnippet management.
	ResultOf<SNothing> SpaceTrailing(STrailing& Trailing)
	{
		ULANG_ASSERT(!Trailing);
		Trailing.TrailingStart = Cursor;
		ULANG_GRAMMAR_RUN(Space(Trailing.TrailingCapture));
		return SNothing{};
	}
	ResultOf<SNothing> UpdateFrom(SExpr& Target, STrailing& Source,
		const ResultOf<SyntaxType>& SyntaxResult)
	{
		ULANG_ASSERT(Source);
		ULANG_ASSERT(!Target.Finished && !Target.Trailing);
		Target.Trailing.MoveFrom(Source);
		ULANG_GRAMMAR_SET(Target.ExprSyntax, SyntaxResult);
		return SNothing{};
	}
	ResultOf<SNothing>
	UpdateSpaceTrailing(SExpr& Target, const ResultOf<SyntaxType>& SyntaxResult)
	{
		ULANG_ASSERT(!Target.Finished);
		ULANG_ASSERT(!Target.Trailing);
		ULANG_GRAMMAR_SET(Target.ExprSyntax, SyntaxResult);
		ULANG_GRAMMAR_RUN(SpaceTrailing(Target.Trailing));
		return SNothing{};
	}
	SyntaxType ApplyTrailing(SExpr& Target, bool FinishingNow = false)
	{
		ULANG_ASSERT(!Target.Finished || FinishingNow);
		ULANG_ASSERT(Target.Trailing);
		Target.ExprSyntax = Gen.Trailing(*Target, Target.Trailing.TrailingCapture);
		Target.Trailing = STrailing{};
		return *Target;
	}
	void ApplyTrailing(SBlockInternal& Block0, const SPoint& TrailingEnd)
	{
		if (Block0.Punctuation != EPunctuation::None)
			Gen.CaptureAppend(Block0.PunctuationTrailing,
				Block0.BlockTrailing.TrailingCapture);
		else
			Gen.CaptureAppend(Block0.ElementsTrailing,
				Block0.BlockTrailing.TrailingCapture);
		Block0.BlockSnippet = Snip(SPoint::Start(Block0.BlockSnippet), TrailingEnd);
		Block0.BlockTrailing = STrailing{};
	}

	// Character set and comment and errors:
	auto S01()
	{
		return Gen.Err(Snip(), "S01",
			"Source must be ASCII or Unicode UTF-8 format");
	}
	auto S02()
	{
		return Gen.Err(Snip(), "S02", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " in block comment");
	}
	auto S03()
	{
		return Gen.Err(Snip(), "S03", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " in line comment");
	}
	auto S04()
	{
		return Gen.Err(Snip(), "S04",
			"Block comment beginning at \"<#\" never ends");
	}
	auto S05()
	{
		return Gen.Err(Snip(), "S05", "Ending \"#>\" is outside of block comment");
	}
	auto S06()
	{
		return Gen.Err(Snip(), "S06", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " in indented comment");
	}

	// Numeric and numbered character constant errors.
	auto S15()
	{
		return Gen.Err(Snip(), "S15", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " following number.");
	}
	auto S16()
	{
		return Gen.Err(Snip(), "S15", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " following character.");
	}
	auto S18()
	{
		return Gen.Err(Snip(), "S18",
			"Character code unit octet must be 1-2 digits in the range "
			"0o0 to 0oFF");
	}
	auto S19()
	{
		return Gen.Err(
			Snip(), "S19",
			"Unicode code point must be 1-6 digits in the range 0u0 to 0u10FFFF");
	}

	// Identifier errors.
	auto S20(SText What)
	{
		return Gen.Err(Snip(), "S20", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " or missing identifier following \"", What,
			"\"");
	}
	auto S23(SText What)
	{
		return Gen.Err(Snip(), "S23", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " or missing \"", What, "\" in qualifier");
	}
	auto S24(SText What)
	{
		return Gen.Err(Snip(), "S24", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " or missing \"", What,
			"\" in quoted identifier");
	}
	auto S25(SText What)
	{
		return Gen.Err(Snip(), "S25", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " or missing \"", What, "\" in path literal");
	}
	auto S26(SText What)
	{
		return Gen.Err(Snip(), "S26", "Missing label in path following \"", What,
			"\"");
	}

	// Text errors.
	auto S30()
	{
		return Gen.Err(Snip(), "S30", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " in character literal");
	}
	auto S31(SText)
	{
		return Gen.Err(Snip(), "S31", "Missing \"'\" in character literal");
	}
	auto S32(SText)
	{
		return Gen.Err(Snip(), "S32", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " or missing end quote in string literal");
	}
	auto S34()
	{
		return Gen.Err(Snip(), "S34", "Bad character escape \"\\\" followed by ",
			CursorQuote(), CursorText(), CursorQuote());
	}

	// Markup errors.
	auto S40()
	{
		return Gen.Err(Snip(), "S40", "Missing markup tag preceding ",
			CursorQuote(), CursorText(), CursorQuote());
	}
	auto S41()
	{
		return Gen.Err(Snip(), "S41", "Bad markup expression preceding ",
			CursorQuote(), CursorText(), CursorQuote());
	}
	auto S42()
	{
		return Gen.Err(Snip(), "S42",
			"Unexpected markup end tag outside of markup");
	}
	auto S43(SText Tag, SText Id)
	{
		return Gen.Err(Snip(), "S43", "Markup started with \"<", Tag,
			">\" tag but ended in mismatched \"</", Id, ">\" tag");
	}
	auto S44(SText What)
	{
		return Gen.Err(Snip(), "S44", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " or missing \"", What,
			"\" in markup end tag");
	}
	auto S46()
	{
		return Gen.Err(Snip(), "S46",
			"Expected indented markup following \":>\" but got ",
			CursorQuote(), CursorText(), CursorQuote());
	}

	// Markup content errors.
	auto S51(SText What)
	{
		return Gen.Err(Snip(), "S51", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " or missing \"", What, "\" in markup");
	}
	auto S52(SText)
	{
		return Gen.Err(Snip(), "S52", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " or missing markup end tag");
	}
	auto S54()
	{
		return Gen.Err(Snip(), "S54", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " in indented markup");
	}
	auto S57()
	{
		return Gen.Err(Snip(), "S57", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(),
			" or missing ending \";\" or newline following \"&\" markup "
			"escape expression");
	}
	auto S58()
	{
		return Gen.Err(Snip(), "S58",
			"Markup list separator \"~\" is only allowed in markup "
			"beginning with \"~\"; elsewhere escape it using \"\\~\"");
	}

	// Precedence errors.
	auto S60(SText What, SText Op)
	{
		return Gen.Err(Snip(), "S60", "Precedence doesn't allow \"", Op,
			"\" following \"", What, "\"");
	}
	auto S61(SText Op)
	{
		return Gen.Err(Snip(), "S61", "Precedence doesn't allow \"", Op, "\" here");
	}
	auto S62()
	{
		return Gen.Err(Snip(), "S62",
			"Verse uses 'and', 'or', 'not' instead of '&&', '||', '!'.");
	};
	auto S64(SText, SText Op)
	{
		return Gen.Err(Snip(), "S64", "Precedence doesn't allow \"", Op,
			"\" in markup tag expression");
	}
	auto S65()
	{
		return Gen.Err(Snip(), "S65", "Use a=b for comparison, not a==b");
	}
	auto S66(SText Op)
	{
		return Gen.Err(Snip(), "S66", "Use 'set' before \"", Op,
			"\" to update variables");
	}
	auto S67()
	{
		return Gen.Err(
			Snip(), "S67",
			"Prefix attribute must be followed by identifier declaration");
	}
	auto S68()
	{
		return Gen.Err(Snip(), "S68", "Use # for line comment, not //");
	}

	// Bad or missing expression, block, keyword errors.
	auto S70(SText)
	{
		return Gen.Err(Snip(), "S70", "Expected expression, got ", CursorQuote(),
			CursorText(), CursorQuote(), " at top level of program");
	}
	auto S71(SText What)
	{
		return Gen.Err(Snip(), "S71", "Expected expression, got ", CursorQuote(),
			CursorText(), CursorQuote(), " following \"", What, "\"");
	}
	auto S74(SText)
	{
		return Gen.Err(Snip(), "S74", "Expected markup tag expression, got ",
			CursorQuote(), CursorText(), CursorQuote());
	}
	auto S76(SText What)
	{
		return Gen.Err(Snip(), "S76", "Expected block, got ", CursorQuote(),
			CursorText(), CursorQuote(), " following \"", What, "\"");
	}
	auto S77()
	{
		return Gen.Err(Snip(), "S77", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), " following expression");
	}
	auto S78()
	{
		return Gen.Err(Snip(), "S78", "Expected <specifier> following \"with\"");
	}
	auto S79()
	{
		return Gen.Err(Snip(), "S79", "Unexpected ", CursorQuote(), CursorText(),
			CursorQuote(), "or missing \">\" following specifier");
	}

	// Expression grouping errors:
	auto S80(SText What)
	{
		return Gen.Err(Snip(), "S80", "Block starting in \"", What,
			"\" never ends");
	}
	auto S81(SText What)
	{
		return Gen.Err(Snip(), "S81", "Expected expression or \"", What, "\", got ",
			CursorQuote(), CursorText(), CursorQuote(),
			" in parenthesis");
	}
	auto S82(SText What)
	{
		return Gen.Err(Snip(), "S82", "Expected expression or \"", What, "\", got ",
			CursorQuote(), CursorText(), CursorQuote(),
			" in parenthesized parameter list");
	}
	auto S83(SText What)
	{
		return Gen.Err(Snip(), "S83", "Expected expression or \"", What, "\", got ",
			CursorQuote(), CursorText(), CursorQuote(),
			" in bracketed parameters");
	}
	auto S84(SText What)
	{
		return Gen.Err(Snip(), "S84", "Expected expression or \"", What, "\", got ",
			CursorQuote(), CursorText(), CursorQuote(),
			" in braced block");
	}
	auto S85(SText What)
	{
		return Gen.Err(Snip(), "S85", "Expected \"", What, "\", got ",
			CursorQuote(), CursorText(), CursorQuote(),
			" in prefix brackets");
	}
	auto S86(SText What)
	{
		return Gen.Err(Snip(), "S86", "Expected expression or \"", What, "\", got ",
			CursorQuote(), CursorText(), CursorQuote(),
			" in string interpolation");
	}
	auto S88(SText)
	{
		return Gen.Err(Snip(), "S88", "Expected expression, got ", CursorQuote(),
			CursorText(), CursorQuote(), " in indented block");
	}
	auto S88void()
	{
		return Gen.Err(Snip(), "S88", "Expected expression, got ", CursorQuote(),
			CursorText(), CursorQuote(), " in indented block");
	}
	auto S89()
	{
		return Gen.Err(Snip(), "S89", "Indentation mismatch: expected ",
			Context.BlockInd[SPoint(Cursor).Column] == ' ' ? "space"
														   : "tab",
			", got ", CursorQuote(), CursorText(), CursorQuote());
	}

	// Parser limitations versus spec.
	auto S97() { return Gen.Err(Snip(), "S97", "Unexpected error"); }
	auto S98()
	{
		return Gen.Err(Snip(), "S98", "Feature is not currently supported");
	}
	auto S99()
	{
		return Gen.Err(Snip(), "S99", "Exceeded maximum expression depth");
	}

	// Blank space and indentation.
	void SnipNewLine(CaptureType& Capture, EPlace Place = EPlace::Space)
	{
		// If a NewLine is ahead, incorporate it in Capture despite not consuming it
		// per grammar spec.
		if (!Cursor.SnippedNewLine() && (Cursor[0] == 0x0D || Cursor[0] == 0x0A))
		{
			auto Start = Cursor;
			Cursor.NextLineStart =
				Cursor.Pos + 1 + (Cursor[0] == 0x0D && Cursor[1] == 0x0A);
			Gen.NewLine(Capture, Snip(Start), Place);
		}
	}
	bool NewLine(CaptureType& Capture, EPlace Place = EPlace::Space)
	{
		// NewLine := 0o0D [0o0A] | 0o0A
		SnipNewLine(Capture, Place);
		if (Cursor.SnippedNewLine())
		{
			Cursor.Pos = Cursor.NextLineStart;
			Cursor.LineStart = Cursor.Pos;
			Cursor.Line++;
			return true;
		}
		return false;
	}
	bool Ending()
	{
		// Ending := &(NewLine | end)
		return Cursor.SnippedNewLine() || IsEnding(Cursor[0]);
	}
	ResultOf<SNothing> Space(CaptureType& Capture, EPlace Place = EPlace::Space,
		bool DoSnipNewLine = true)
	{
		// Space := {0o09 | 0o20 | Comment}
		ULANG_GRAMMAR_RUN(Text<EPlace::Space>(Capture, Place));
		if (DoSnipNewLine)
			SnipNewLine(Capture, Place);
		return UpdateToken(), SNothing{};
	}
	ResultOf<CaptureType> Space(EPlace Place = EPlace::Space)
	{
		CaptureType Capture;
		ULANG_GRAMMAR_RUN(Space(Capture, Place));
		return Capture;
	}
	ResultOf<SContext> Ind()
	{
		// Ind := Ending push; set Nest=false; set BlockInd=LineInd; set
		// LinePrefix=""
		ULANG_ASSERT(Ending());
		auto SavedContext = Context;
		Context.BlockInd = Cursor.LineStart;
		Context.TrimInd = Cursor.LineStart;
		Context.Nest = false;
		return SavedContext;
	}
	ResultOf<SNothing> Ded(const SContext& SavedContext,
		ErrorType (CParser::*OnError)())
	{
		// Ded := Ending pop
		Context = SavedContext;
		if (!Ending())
			return (this->*OnError)();
		return UpdateToken(), SNothing{};
	}
	ResultOf<bool> Line(CaptureType& Capture, EPlace Place)
	{
		// Line := NewLine; parse i:={0o09|0o20}; (Ending | !(0o09|0o20) Space
		//         if     (i>BlockInd | Nest and i=BlockInd) then set
		//         LineInd=ThisInd else if(not i<=BlockInd                 ) then
		//         error)
		auto SavedLineEnd = Cursor;
		if (!NewLine(Capture, Place))
			return false;
		auto SavedLineStart = Cursor;
		while (IsSpace(Cursor[0]) && Cursor[0] == Context.BlockInd[Cursor.Pos - SavedLineStart.Pos])
			Next(1);
		bool HasMoreSpace = IsSpace(Cursor[0]);
		if ((HasMoreSpace || Context.Nest) && !IsSpace(Context.BlockInd[Cursor.Pos - SavedLineStart.Pos]))
		{
			// This line falls into current indented SBlock, so consume any additional
			// optional TrimIn and note via Gen.Indent followed by potentially
			// Place-significant Space.
			while (IsSpace(Cursor[0]) && Cursor[0] == Context.TrimInd[Cursor.Pos - SavedLineStart.Pos])
				Next(1);
			Gen.Indent(Capture, Snip(SavedLineStart), Place);
			ULANG_GRAMMAR_RUN(Space(Capture, Place));
			return true;
		}
		else if (Ending())
		{
			// Blank line whose indentation isn't related to leading.
			return Gen.BlankLine(Capture, Snip(SavedLineStart), Place), true;
		}
		else if (HasMoreSpace)
		{
			// Inconsistently indented nonblank line, so error at inconsistency.
			return S89();
		}
		else
		{
			// NOTE: (yiliang.siew) For indented indcmts, such as:
			/*
			 *
			 * ```
			 * a<#>
			 *  b<#>
			 * c<#>
			 *  d<#>
			 * <#>
			 *  e<#>
			 * ```
			 *
			 * And so on, the CParser will end up treating the entirety of the
			 * contents after `a<#>` as an indcmt, and recursively do so for the
			 * contents after `b<#>` and so on. This can lead to really slow
			 * parsing/stack overflow should a malicious actor craft Verse syntax to
			 * take advantage of this. We therefore only capture indented comments up
			 * to a certain SPoint and give up; it's highly unlikely anyone would need
			 * that amount of indentation in their comments.
			 */
			const uint32_t NewCommentDepth =
				Place == EPlace::IndCmt || Place == EPlace::BlockCmt
					? CommentDepth + 1
					: CommentDepth;
			TScopedGuard CommentDepthGuard(CommentDepth, NewCommentDepth);
			if (CommentDepth > VERSE_MAX_INDCMT_DEPTH)
			{
				return Cursor = SavedLineEnd, false; // backtrack but could cache
			}

			// Line that only contains whitespace or comments, but less indented than
			// the current SBlock. If we have reached this SPoint, it means that we
			// might be at the start of a comment on the next line, but the comment
			// might not have any indentation at all.
			CaptureType SpaceCapture = {};
			ULANG_GRAMMAR_RUN(Space(SpaceCapture, Place));
			if (Cursor.SnippedNewLine())
			{
				// We need to keep eating until we hit a SToken that is
				// non-whitespace/comment and has different indentation. If so,
				// backtrack. If no backtracking, return `true` and append the capture
				// here. If this `Scan` fails to extend the current SBlock because there
				// is already a non-whitespace/comment token there, it will already have
				// backtracked to the previous line. But the capture would still be
				// empty since `Scan` would return a `SNothing{}` value.
				CaptureType ScanCapture = {};
				if (Scan(ScanCapture, Place) && Gen.CaptureIsEmpty(ScanCapture))
				{
					// NOTE: (yiliang.siew) Backtrack here so that the comment can be
					// associated with the correct capture.
					return Cursor = SavedLineEnd, false;
				}
				else
				{
					Gen.CaptureAppend(Capture, SpaceCapture);
					return true;
				}
			}
			else
			{
				// Consistent nonblank line from an earlier indented SBlock.
				return Cursor = SavedLineEnd, false; // backtrack but could cache
			}
		}
	}
	ResultOf<SNothing> Scan(CaptureType& Capture, EPlace Place = EPlace::Space)
	{
		// Scan := Space {Line}
		ULANG_GRAMMAR_RUN(Space(Capture, Place, false));
		for (;;)
		{
			CaptureType LineCapture;
			ULANG_GRAMMAR_LET(GotLine, Line(LineCapture, Place));
			if (!GotLine)
				return UpdateToken(), SNothing{};

			// In EPlace::Content, trim STrailing [NewLine Space &('~' | '</')].
			if (Place == EPlace::Content && (Cursor[0] == '~' || (Cursor[0] == '<' && Cursor[1] == '/')))
				Gen.MarkupTrim(LineCapture);
			Gen.CaptureAppend(Capture, LineCapture);
		}
	}
	ResultOf<SToken> ScanKey(CaptureType& Capture, STokenSet TokenSet)
	{
		// This function implements the grammar for Brace and ScanKey [Token1 |
		// Token2 | ..] &Key. Brace   := Scan '{' List '}' Space ScanKey := Space
		// (&NewLine Scan LinePrefix Space | !NewLine) Key     := !Alnum Space !":="
		auto ScanStart = Cursor;
		ULANG_GRAMMAR_LET(More, Space());
		bool Multiline = Ending();
		ULANG_GRAMMAR_RUN(Scan(More));
		if (Context.LinePrefix && Multiline && Cursor.Token != SToken(u8"{"))
		{
			auto LinePrefixStart = Cursor;
			if (Eat(u8"&"))
			{
				Gen.LinePrefix(More, Snip(LinePrefixStart));
				ULANG_GRAMMAR_RUN(Space(More));
				if (TokenSet.Has(Cursor.Token))
					return Gen.CaptureAppend(Capture, More), Cursor.Token;
			}
		}
		else if (TokenSet.Has(Cursor.Token))
			return Gen.CaptureAppend(Capture, More), Cursor.Token;
		return Cursor = ScanStart, SToken::None(); // backtrack but could cache
	}

	// Constants and base expressions.
	ResultOf<uint64_t> ParseHex(uint64_t MaxDigits, uint64_t MaxValue,
		ErrorType (CParser::*OnError)())
	{
		uint64_t i = 0;
		while (IsHex(Cursor[0]))
		{
			if (MaxDigits-- > 0)
			{
				auto i0 = i;
				i = i * 16 + DigitValue(Cursor[0]);
				if (i <= MaxValue && i / 16 == i0)
				{
					Next(1);
					continue;
				}
			}
			return (this->*OnError)();
		}
		return i;
	}
	ResultOf<SNothing> DisallowDotAlnum()
	{
		bool GotDot = Cursor[0] == '.';
		if (IsAlnum(Cursor[GotDot]))
			return S15();
		return SNothing{};
	}
	ResultOf<SNothing> DisallowDotNum()
	{
		bool GotDot = Cursor[0] == '.';
		if (IsDigit(Cursor[GotDot]))
			return S15();
		return SNothing{};
	}
	ResultOf<SyntaxType> Num()
	{
		// Exp    := [('e'|'E') ['+'|'-'] Digits] !(('e'|'E') ('+'|'-'|Digit))
		// Units  := [Alpha {Alpha}] !Alpha
		// Num    := !(("0b"|"0o"|"0u"|"0x") Hex) Digits ['.' Digits] Exp Units)
		// !('.' Digits)
		//        |  ("0x" Hex {Hex} !('.' Alnum)
		auto Start = Cursor;
		ULANG_ASSERT(IsDigit(Cursor[0]));
		if (Cursor[0] == '0' && Cursor[1] == 'x' && IsHex(Cursor[2]))
		{
			ULANG_ASSERT(Cursor[0] == '0' && Cursor[1] == 'x' && IsHex(Cursor[2]));
			Next(2);
			do
			{
				Next(1);
			}
			while (IsHex(Cursor[0]));
			// Could use `DisallowDotNum()` which would then permit extension function
			// on hex literals - `0xff.ShiftRight()` Can still wrap hex literals with
			// parentheses - `(0xff).ShiftRight()`
			ULANG_GRAMMAR_RUN(DisallowDotAlnum());
			return Gen.NumHex(Snip(Start), SText(Start.Pos + 2, Cursor.Pos));
		}
		while (IsDigit(Cursor[0]))
			Next(1);
		SText Digits(Start.Pos, Cursor.Pos),
			FractionalDigits(Cursor.Pos + 1, Cursor.Pos + 1);
		if (Cursor[0] == '.' && IsDigit(Cursor[1]))
		{
			Next(2);
			while (IsDigit(Cursor[0]))
				Next(1);
			FractionalDigits.Stop = Cursor.Pos;
		}
		SText ExponentSign, Exponent;
		if (Cursor[0] == 'e' || Cursor[0] == 'E')
		{
			int64_t HasExponentSign = int64_t(Cursor[1] == '+' || Cursor[1] == '-');
			if (IsDigit(Cursor[1 + HasExponentSign]))
			{
				ExponentSign = SText(Cursor.Pos + 1, Cursor.Pos + 1 + HasExponentSign);
				Next(1 + HasExponentSign);
				Exponent.Start = Cursor.Pos;
				while (IsDigit(Cursor[0]))
					Next(1);
				Exponent.Stop = Cursor.Pos;
			}
		}
		ULANG_GRAMMAR_LET(Result, Gen.Num(Snip(Start), Digits, FractionalDigits,
									  ExponentSign, Exponent));
		if (IsAlpha(Cursor[0]))
		{
			auto Pos0 = Cursor.Pos;
			do
				Next(1);
			while (IsAlnum(Cursor[0]));
			ULANG_GRAMMAR_SET(
				Result, Gen.Units(Snip(Start), Result, SText(Pos0, Cursor.Pos)));
		}
		// Disallow extra dot digit and allow dot alpha so extension functions ['.'
		// Ident] can be called on number literals
		ULANG_GRAMMAR_RUN(DisallowDotNum());
		return Result;
	}
	ResultOf<SyntaxType> CharLit()
	{
		// Special := '\'|'{'|'}'|'#'|'<'|'>'|'&'|'~'
		// CharEsc := '\' ('r'|'n'|'t'|'''|'"'|Special)
		// CharLit := ''' Printable ''' !''' | ''' CharEsc '''
		ULANG_ASSERT(Cursor[0] == '\'');
		auto Start = Cursor;
		Next(1);
		uint64_t n = EncodedLength<EPlace::Printable>(Cursor.Pos);
		if (!n)
			return S30();
		auto Char32 = EncodedChar32(Cursor.Pos, n);
		auto Backslash = Cursor[0] == '\\' && Cursor[1] && Cursor[2] == '\'';
		if (Backslash)
		{
			Next(1);
			if (IsStringBackslashLiteral(Cursor[0], Cursor[1]))
			{
				Char32 = char32_t(Cursor[0] == 'r'   ? '\r'
								  : Cursor[0] == 'n' ? '\n'
								  : Cursor[0] == 't' ? '\t'
													 : Cursor[0]);
				Backslash = 1;
				Next(n);
			}
			else
				return S34();
		}
		else
			Next(n);
		ULANG_GRAMMAR_RUN(Require(u8"'", &CParser::S31));
		return Gen.Char32(Snip(Start), Char32, false, Backslash);
	}
	ResultOf<char8_t> Char8()
	{
		// Char8 := "0o" (Hex) [Hex] !Alnum
		ULANG_ASSERT(Cursor[0] == '0' && Cursor[1] == 'o' && IsHex(Cursor[2]));
		Next(2);
		ULANG_GRAMMAR_LET(n, ParseHex(2, 0xFFULL, &CParser::S18));
		if (IsAlnum(Cursor[0]))
			return S16();
		return char8_t(n);
	}
	ResultOf<char32_t> Char32()
	{
		// Char32 := "0u" ("10" | Hex) [Hex] [Hex] [Hex] [Hex]) !Alnum
		ULANG_ASSERT(Cursor[0] == '0' && Cursor[1] == 'u' && IsHex(Cursor[2]));
		Next(2);
		ULANG_GRAMMAR_LET(n, ParseHex(6, 0x10FFFFULL, &CParser::S19));
		if (IsAlnum(Cursor[0]))
			return S16();
		return char32_t(n);
	}
	ResultOf<SText> Ident()
	{
		// Ident := Alpha {Alnum} !Alnum ["'" {!('<#'|'#>'|'\'|'{'|'}'|'"'|''')
		// 0o20-0o7E} "'"]
		ULANG_ASSERT(IsAlpha(Cursor[0]));
		auto Pos0 = Cursor.Pos;
		do
			Next(1);
		while (IsAlnum(Cursor[0]));
		if (!Eat(u8"'"))
			return SText(Pos0, Cursor.Pos);
		// Ensure not reading past string and determine if quotable
		while ((Cursor[0] != '\0') && IsIdentifierQuotable(Cursor[0], Cursor[1]))
			Next(1);
		ULANG_GRAMMAR_RUN(Require(u8"'", &CParser::S24));
		return SText(Pos0, Cursor.Pos);
	}
	ResultOf<SText> Path()
	{
		// Path := '/' Label ('@' Label | !'@')] {'/' ['(' Path ':)'] Ident} !'/'
		auto Start = Cursor;
		ULANG_GRAMMAR_RUN(Require(u8"/", &CParser::S25));
		if (Cursor[0] == '/' || (Cursor[0] == ' ' && Cursor.Pos > InputString && Cursor[-1] == '/'))
			return S68();
		ULANG_GRAMMAR_RUN(Label(u8"/"));
		if (Eat(u8"@"))
			ULANG_GRAMMAR_RUN(Label(u8"@"));
		while (Eat(u8"/"))
		{
			SText What = u8"/";
			if (Eat(u8"("))
			{
				ULANG_GRAMMAR_RUN(Path());
				ULANG_GRAMMAR_RUN(Require(u8":)", &CParser::S25));
				What = u8":)";
			}
			if (IsAlpha(Cursor[0]))
			{
				ULANG_GRAMMAR_RUN(Ident());
				continue;
			}
			return S20(What);
		}
		if (Cursor[0] != '/')
			return SText(Start.Pos, Cursor.Pos);
		return S25(u8"/");
	}
	ResultOf<SText> Label(SText What)
	{
		// Label := Alnum {Alnum|'-'|'.'} !(Alnum|'-'|'.')
		auto Pos0 = Cursor.Pos;
		if (IsAlnum(Cursor[0]))
		{
			Next(1);
			while (IsAlnum(Cursor[0]) || Cursor[0] == '-' || Cursor[0] == '.')
				Next(1);
			return SText(Pos0, Cursor.Pos);
		}
		return S26(What);
	}

	// Text processing.
	ResultOf<CaptureType> LineCmt()
	{
		// LineCmt := '#' !'>' {Text} Ending
		ULANG_ASSERT(Cursor[0] == '#');
		Next(1);
		CaptureType Capture;
		ULANG_GRAMMAR_RUN(Text<EPlace::LineCmt>(Capture));
		if (Ending())
			return Capture;
		else
			return S03();
	}
	ResultOf<CaptureType> BlockCmt()
	{
		// BlockCmt := "<#" !'>' {Text|NewLine} !'<' "#>"
		ULANG_ASSERT(Cursor[0] == '<' && Cursor[1] == '#' && Cursor[2] != '>');
		auto Start = Cursor;
		Next(2);
		CaptureType Capture;
		ULANG_GRAMMAR_RUN(Text<EPlace::BlockCmt>(Capture));
		if (Cursor[0] == '#' && Cursor[1] == '>')
			return Next(2), Capture;
		else if (Cursor[0] == 0)
			return Cursor = Start, S04();
		else
			return S02();
	}
	ResultOf<CaptureType> IndCmt()
	{
		// IndCmt := "<#>" {Text} Ind {Text|Line} Ded
		ULANG_ASSERT(Cursor[0] == '<' && Cursor[1] == '#' && Cursor[2] == '>');
		Next(3);
		CaptureType Capture;
		ULANG_GRAMMAR_RUN(Text<EPlace::LineCmt>(Capture));
		if (Ending())
		{
			ULANG_GRAMMAR_LET(SavedContext, Ind());
			ULANG_GRAMMAR_RUN(Text<EPlace::IndCmt>(Capture));
			ULANG_GRAMMAR_RUN(Ded(SavedContext, &CParser::S06));
			// NOTE: (yiliang.siew) We don't want to snip a newline here, because that
			// means that an indcmt like:
			/*
			 * <#>indcmt
			 *     indcmt_frag
			 * stub{}
			 *
			 */
			// Ends up getting an extra newline snipped as part of its comment string
			// capture, which wouldn't make sense since indcmts must always have a
			// newline after anyway. ULANG_GRAMMAR_RUN(Space(Capture,EPlace::IndCmt));
			return Capture;
		}
		else
			return S06();
	}
	template <EPlace ParsePlace>
	ResultOf<SNothing> Text(CaptureType& Capture, EPlace GenPlace = ParsePlace)
	{
		// Text      := Printable | BlockCmt | "<#>"
		// LineCmt   := '#' !'>' {Text} Ending
		// BlockCmt  := "<#" !'>' {Text|NewLine} !'<' "#>"
		// IndCmt    := "<#>" {Text} Ind {Text|Line} Ded
		// Space     := {0o09 | 0o20 | Comment}
		// String    := '"' {..                  | CharEsc |      !('\'|'{'|'}'|'"')
		// Text} '"' Content   :=     {.. | Comment | Line | CharEsc | .. | !Special
		// Text} CharEsc   := '\' ('r'|'n'|'t'|'''|'"'|Special)
		for (;;)
		{
			auto Start = Cursor;
			for (uint64_t n; (n = EncodedLength<ParsePlace>(Cursor.Pos)) != 0;)
				Next(n);
			if (Cursor.Pos != Start.Pos)
				Gen.Text(Capture, Snip(Start), GenPlace);
			auto SpecialStart = Cursor;
			switch (Cursor[0])
			{
				case '\r':
				case '\n':
					if constexpr (ParsePlace == EPlace::Content || ParsePlace == EPlace::IndCmt)
					{
						ULANG_GRAMMAR_RUN(Scan(Capture, GenPlace));
						if (Ending())
							return SNothing{};
						continue;
					}
					else if constexpr (ParsePlace == EPlace::BlockCmt)
					{
						NewLine(Capture, GenPlace);
						continue;
					}
					else
						return SNothing{};
				case '#':
					if (Cursor[1] != '>')
					{
						ULANG_GRAMMAR_LET(Commentary, LineCmt());
						Gen.LineCmt(Capture, Snip(SpecialStart), GenPlace, Commentary);
						continue;
					}
					else if constexpr (ParsePlace == EPlace::BlockCmt)
						return SNothing{};
					else
						return S05();
				case '<':
					if (Cursor[1] != '#')
					{
						return SNothing{};
					}
					else if (Cursor[2] != '>')
					{
						ULANG_GRAMMAR_LET(Commentary, BlockCmt());
						Gen.BlockCmt(Capture, Snip(SpecialStart), GenPlace, Commentary);
						continue;
					}
					else if constexpr (ParsePlace == EPlace::Space || ParsePlace == EPlace::Content || ParsePlace == EPlace::IndCmt)
					{
						ULANG_GRAMMAR_LET(Commentary, IndCmt());
						Gen.IndCmt(Capture, Snip(SpecialStart), GenPlace, Commentary);
						continue;
					}
					else
					{
						Next(3);
						Gen.Text(Capture, Snip(SpecialStart), GenPlace);
						continue;
					}
				case '\\':
					// Parse a constant escape.
					// Special  := '\'|'{'|'}'|'#'|'<'|'>'|'&'|'~'
					// CharEsc  := '\' ('r'|'n'|'t'|'''|'"'|Special)
					if constexpr (ParsePlace == EPlace::String || ParsePlace == EPlace::Content)
					{
						Next(1);
						if (Cursor[0] && IsStringBackslashLiteral(Cursor[0], Cursor[1]))
						{
							auto Backslashed = Cursor[0];
							Next(1);
							Gen.StringBackslash(Capture, Snip(SpecialStart), GenPlace,
								Backslashed);
							continue;
						}
						else
							return S34();
					}
				default:
					return SNothing{};
			}
		}
	}
	ResultOf<SBlockInternal> Interp()
	{
		// Interp := '{' List '}'
		ULANG_ASSERT(Cursor[0] == '{');
		auto Start = Cursor;
		Next(1);
		ULANG_GRAMMAR_LET(Block0, List(u8"}", &CParser::S86, Cursor, CaptureType(),
									  EPunctuation::None, Cursor));
		ULANG_GRAMMAR_RUN(RequireClose(Start, u8"{", u8"}", &CParser::S86));
		return Block0;
	}
	ResultOf<SBlockInternal> Ampersand()
	{
		// Ampersand := push; parse LinePrefix='&'; Space Def (';'|Ending); pop
		ULANG_ASSERT(Cursor[0] == '&');
		Next(1);
		auto ExprStart = Cursor;
		ULANG_GRAMMAR_LET(Leading, Space());
		auto SavedContext = Context;
		Context.LinePrefix = true;
		ULANG_GRAMMAR_LET(Block0,
			WhenExpr(
				u8"&", EPrec::Def, EPrec::Def, nullptr, Leading,
				[&](SExpr& Expr) -> ResultOf<SBlockInternal> {
					ApplyTrailing(Expr, true);
					auto SemicolonStart = Cursor;
					bool Semicolon = Eat(u8";");
					auto Block0 = SingletonBlock(ExprStart, Expr);
					if (!Ending() && !Semicolon)
						return S57();
					if (Semicolon)
						Gen.Semicolon(Block0.ElementsTrailing,
							Snip(SemicolonStart));
					ApplyTrailing(Block0, Cursor);
					return Block0;
				},
				AllTokens));
		Context = SavedContext;
		return Block0;
	}
	template <EPlace Place>
	ResultOf<SyntaxesType> String(SCursor TextStart,
		CaptureType Leading = CaptureType())
	{
		// String  := '"' {Interp | CharEsc | !('\'|'{'|'}'|'"') Text} '"'
		// Content :=     {Interp | CharEsc | Markup | Ampersand | Comment | Line |
		// !Special Text}
		SyntaxesType Splices;
		for (;;)
		{
			ULANG_GRAMMAR_RUN(Text<Place>(Leading));
			if (Cursor.Pos != TextStart.Pos)
			{
				ULANG_GRAMMAR_LET(S, Gen.StringLiteral(Snip(TextStart), Leading));
				Gen.SyntaxesAppend(Splices, S);
			}
			auto SpecialStart = Cursor;
			switch (Cursor[0])
			{
				case '{':
				{
					ULANG_GRAMMAR_LET(Block0, Interp());
					ULANG_GRAMMAR_LET(
						S, Gen.StringInterpolate(Snip(SpecialStart), Place, 1, Block0));
					Gen.SyntaxesAppend(Splices, S);
					break;
				}
				case '&':
				{
					ULANG_GRAMMAR_LET(Block0, Ampersand());
					ULANG_GRAMMAR_LET(
						S, Gen.StringInterpolate(Snip(SpecialStart), Place, 0, Block0));
					Gen.SyntaxesAppend(Splices, S);
					break;
				}
				case '<':
					// Markup := '<' Tags ..
					// Tags   := Space (!'/' ..) ..
					if (Cursor[1] != '/')
					{
						ULANG_GRAMMAR_LET(e, Markup());
						Gen.SyntaxesAppend(Splices, e);
						break;
					}
					[[fallthrough]];
				default:
					return Splices;
			}
			TextStart = Cursor;
			Leading = CaptureType();
		}
	}

	// Markup content.
	ResultOf<SyntaxType> Contents(bool TrimLeading)
	{
		// Contents := Scan (Content | '~' Content {'~' Content})
		auto Start = Cursor;
		ULANG_GRAMMAR_LET(
			Leading,
			Space(
				EPlace::Content)); // If TrimLeading, trim leading [Space NewLine].
		if (TrimLeading && Ending())
			Gen.MarkupTrim(Leading);
		ULANG_GRAMMAR_RUN(Scan(Leading, EPlace::Content));
		if (Cursor[0] != '~')
		{
			ULANG_GRAMMAR_LET(Splices, String<EPlace::Content>(Start, Leading));
			if (Cursor[0] == '~')
				return S58();
			return Gen.Content(Snip(Start), Splices);
		}
		else
		{
			Next(1);
			Gen.MarkupTrim(Leading); // Trim everything before ~.
			SyntaxesType Results;
			do
			{
				auto ElementStart = Cursor;
				ULANG_GRAMMAR_LET(Splices, String<EPlace::Content>(Cursor));
				ULANG_GRAMMAR_LET(S, Gen.Content(Snip(ElementStart), Splices));
				Gen.SyntaxesAppend(Results, S);
			}
			while (Eat(u8"~"));
			return Gen.Contents(Snip(Start), Leading, Results);
		}
	}
	ResultOf<SyntaxType> Trimmed(bool TrimLeading)
	{
		// We push and set TrimInd to LineInd so markup can precisely trim according
		// to LineStart.
		auto SavedContext = Context;
		Context.TrimInd = Cursor.LineStart;
		Context.Nest = true;
		ULANG_GRAMMAR_LET(Result, Contents(TrimLeading));
		Context = SavedContext;
		return Result;
	}

	// Blocks.
	SBlockInternal
	SingletonBlock(const SSnippet& Snippet, const SyntaxType& Syntax,
		const CaptureType& PunctuationLeading = CaptureType(),
		EPunctuation Punctuation = EPunctuation::None)
	{
		SBlockInternal Block0(Snippet);
		Block0.PunctuationLeading = PunctuationLeading;
		Block0.Punctuation = Punctuation;
		Gen.SyntaxesAppend(Block0.Elements, Syntax);
		return Block0;
	}
	SBlockInternal
	SingletonBlock(const SCursor& BlockStart, SExpr& Expr,
		const CaptureType& PunctuationLeading = CaptureType(),
		EPunctuation Punctuation = EPunctuation::None)
	{
		auto Block0 = SingletonBlock(SnipFinished(BlockStart, Expr), *Expr,
			PunctuationLeading, Punctuation);
		Block0.BlockTrailing.MoveFrom(Expr.Trailing);
		return Block0;
	}
	ResultOf<SBlockInternal> IndList(SCursor Start,
		const CaptureType& PunctuationLeading,
		EPunctuation Punctuation,
		SCursor LeadingStart,
		const CaptureType& Leading = CaptureType())
	{
		// Ind List Ded
		ULANG_GRAMMAR_LET(SavedContext, Ind());
		ULANG_GRAMMAR_LET(Block0,
			List(u8"", &CParser::S88, Start, PunctuationLeading,
				Punctuation, LeadingStart, Leading));
		ULANG_GRAMMAR_RUN(Ded(SavedContext, &CParser::S88void));
		ULANG_GRAMMAR_RUN(SpaceTrailing(Block0.BlockTrailing));
		return Block0;
	}
	ResultOf<SBlockInternal>
	BlockHelper(SText What, EPrec Prec, SExpr& OuterExpr, SCursor BlockStart,
		CaptureType PunctuationLeading, bool AllowOpen, bool AllowInd,
		bool AllowCommas, bool* Fails = nullptr)
	{
		// Brace     := Scan '{' List '}' Space
		// Block     := Brace | DotSpace Space Def Space | (DotSpace | ':') Space
		// Ind List Ded BraceInd  := Brace | Ind List Ded DotSpace  := '.' (0o09 |
		// 0o20 | Ending) Space
		switch (uint8_t(Cursor.Token))
		{
			case SToken::NewLine():
			case SToken::End():
			{
				ULANG_GRAMMAR_LET(ScanToken, ScanKey(PunctuationLeading, BracePostfixes));
				if (!ScanToken)
				{
					if (AllowInd)
						return IndList(BlockStart, PunctuationLeading, EPunctuation::Ind,
							Cursor);
					goto bad;
				}
				[[fallthrough]];
			}
			case SToken(u8"{"):
			{
				auto BraceStart = Cursor;
				EatToken();
				ULANG_GRAMMAR_LET(Block0,
					List(u8"}", &CParser::S84, Cursor, PunctuationLeading,
						EPunctuation::Braces, Cursor));
				ULANG_GRAMMAR_RUN(RequireClose(BraceStart, u8"{", u8"}", &CParser::S84));
				Block0.BlockSnippet = Snip(BlockStart);
				ULANG_GRAMMAR_RUN(SpaceTrailing(Block0.BlockTrailing));
				return Block0;
			}
			case SToken(u8"."):
			{
				if (AllowOpen && (IsSpace(Cursor[1]) /*|| IsEnding(Cursor[1])*/))
				{
					EatToken();
					// auto MiddleStart=Cursor;
					ULANG_GRAMMAR_LET(Middle, Space());
					/*if(Ending())
							return
					   IndList(BlockStart,PunctuationLeading,EPunctuation::Dot,MiddleStart,Middle);*/
					return WhenExpr(What, EPrec::Def, EPrec::Def, &OuterExpr, Middle,
						[&](SExpr& Right) -> ResultOf<SBlockInternal> {
							return SingletonBlock(BlockStart, Right,
								PunctuationLeading,
								EPunctuation::Dot);
						});
				}
				goto bad;
			}
			case SToken(u8":"):
			{
				if (AllowOpen)
				{
					auto ColonStart = Cursor;
					EatToken();
					auto MiddleStart = Cursor;
					ULANG_GRAMMAR_LET(Middle, Space());
					if (Ending())
						return IndList(BlockStart, PunctuationLeading, EPunctuation::Colon,
							MiddleStart, Middle);
					Cursor = ColonStart; // backtrack colon and space, then fall through.
				}
				[[fallthrough]];
			}
			default:
				if (Prec != EPrec::Nothing)
				{
					if (AllowCommas)
						return Commas(What, Prec, BlockStart, PunctuationLeading,
							&CParser::S71);
					else
						return WhenExpr(What, Prec, Prec, &OuterExpr, PunctuationLeading,
							[&](SExpr& Right) -> ResultOf<SBlockInternal> {
								return SingletonBlock(BlockStart, Right);
							});
				}
			bad:
				if (!Fails)
					return S71(What);
				else
					return *Fails = true, SBlockInternal{};
		}
	}
	ResultOf<SBlockInternal> Block(SText What, SExpr& OuterExpr,
		SCursor BlockStart,
		const CaptureType& PunctuationLeading,
		bool& Fails)
	{
		// Block := Brace | DotSpace Space Def Space | (DotSpace | ':') Space Ind
		// List Ded
		return BlockHelper(What, EPrec::Nothing, OuterExpr, BlockStart,
			PunctuationLeading, true, false, false, &Fails);
	}
	ResultOf<SBlockInternal> BraceInd(SText What, EPrec Prec, SExpr& OuterExpr)
	{
		// BraceInd := Brace | Ind List Ded
		auto BlockStart = Cursor;
		ULANG_GRAMMAR_LET(PunctuationLeading, Space());
		return BlockHelper(What, Prec, OuterExpr, BlockStart, PunctuationLeading,
			false, true, false);
	}
	ResultOf<SBlockInternal> KeyBlock(EPrec Prec, SExpr& OuterExpr,
		SCursor BlockStart,
		const CaptureType& TokenLeading,
		SText Token,
		const CaptureType& PunctuationLeading)
	{
		// KeyBlock := Block
		ULANG_GRAMMAR_LET(Block0,
			BlockHelper(Token, Prec, OuterExpr, BlockStart,
				PunctuationLeading, true, false, false));
		Block0.Token = Token;
		Block0.TokenLeading = TokenLeading;
		return Block0;
	}
	ResultOf<SBlockInternal> KeyBlockDefs(SExpr& OuterExpr, SCursor BlockStart,
		const CaptureType& TokenLeading,
		SText Token)
	{
		// Defs := Def {Space ',' Scan Def}
		ULANG_GRAMMAR_LET(PunctuationLeading, Space());
		ULANG_GRAMMAR_LET(Block0,
			BlockHelper(Token, EPrec::Def, OuterExpr, BlockStart,
				PunctuationLeading, true, false, true));
		Block0.Token = Token;
		Block0.TokenLeading = TokenLeading;
		return Block0;
	}
	template <class CallableType>
	ResultOf<SNothing> WhenBraceCall(const char8_t* What, EPrec Prec,
		SExpr& OuterExpr, const CallableType& Func)
	{
		// Brace  := Scan '{' List '}' Space
		// Prefix := Call | .. Space (Brace | Prefix)
		// Takes a callback because things like +a<b preemptively invoke OnFinish.
		auto BlockStart = Cursor;
		ULANG_GRAMMAR_LET(PunctuationLeading, Space());
		if (Cursor.Token == SToken(u8"{") || Cursor.Token == SToken::NewLine())
		{
			ULANG_GRAMMAR_LET(RightBlock,
				BlockHelper(What, Prec, OuterExpr, BlockStart,
					PunctuationLeading, false, false, false));
			return Func(RightBlock);
		}
		else
			return WhenExpr(What, Prec, Prec, &OuterExpr, PunctuationLeading,
				[&](SExpr& RightExpr) -> ResultOf<SNothing> {
					auto RightBlock = SingletonBlock(BlockStart, RightExpr);
					return Func(RightBlock);
				});
	}

	// Qualified identifiers.
	ResultOf<SyntaxType> QualIdentQualified(SExpr& Target, const SCursor& Start,
		SBlockInternal& Block0)
	{
		ULANG_GRAMMAR_RUN(Space(Block0.PunctuationTrailing));
		Block0.BlockSnippet = Snip(Start);
		Block0.Punctuation = EPunctuation::Qualifier;
		if (IsAlpha(Cursor[0]))
		{
			ULANG_GRAMMAR_LET(Id, Ident());
			Target.MarkupTag = Id;
			return Gen.QualIdent(Snip(Start), Block0, Id);
		}
		else
			return S23(u8":)");
	}
	ResultOf<SyntaxType> QualIdent(SText What, SExpr& Target,
		bool AllowParenthesis)
	{
		// Ident     := Alpha {Alnum} !Alnum ["'" {!('<#'|'#>'|'\'|'{'|'}'|'"'|''')
		// 0o20-0o7E} "'"] QualIdent := ['(' List ':)' Space] Ident Base      := '('
		// List ')' | .. Postfix-only non-ScanKey keywords are valid identifiers.
		auto Start = Cursor;
		if (IsAlpha(Cursor[0]))
		{
			ULANG_GRAMMAR_LET(Id, Ident());
			Target.MarkupTag = Id;
			return Gen.Ident(Snip(Start), Id, u8"", u8"");
		}
		else if (Cursor[0] == '(')
		{
			EatToken();
			ULANG_GRAMMAR_LET(Block0,
				List(u8")", &CParser::S81, Cursor, CaptureType(),
					EPunctuation::Parens, Cursor));
			if (Eat(u8":)"))
				return QualIdentQualified(Target, Start, Block0);
			else if (AllowParenthesis)
			{
				ULANG_GRAMMAR_RUN(RequireClose(Start, u8"(", u8")", &CParser::S81));
				Block0.BlockSnippet = Snip(Start);
				return Gen.Parenthesis(Block0);
			}
			else
				return S23(u8":)");
		}
		return S20(What);
	}

	// Macro invocations and constructs that lead with same syntax like '(', '<',
	// 'with'.
	struct SCall
	{
		SText CallWhat;
		SCursor CallTrailingStop;
		EMode CallMode;
		SBlockInternal& CallParameter;
		SCall* OuterCall = nullptr; // Initialized to keep static analysis happy
	};
	struct SInvoke : SExpr
	{
		SText What;
		SToken StartToken;
		STokenSet InTokens, PostTokens;
		SCall *FirstCall, *LastCall;
		SCall* Of;
		SBlockInternal* Clauses[3];
		SBlockInternal* PriorClause;
		SInvoke(SText What0, SExpr& OuterExpr0, SCursor Start0, SToken StartToken0,
			STokenSet InTokens0, STokenSet PostTokens0,
			SCall* FirstCall0 = nullptr, SCall* LastCall0 = nullptr)
			: SExpr(EPrec::Base, Start0, &OuterExpr0,
				OuterExpr0.MarkupStart ? InvokePostfixes | MarkupPostfixes
									   : InvokePostfixes)
			, What(What0)
			, StartToken(StartToken0)
			, InTokens(InTokens0)
			, PostTokens(PostTokens0)
			, FirstCall(FirstCall0)
			, LastCall(LastCall0)
			, Of(nullptr)
			, Clauses{nullptr, nullptr, nullptr}
			, PriorClause(nullptr) {}
		void UpdateLastCall(SCall* NewCall)
		{
			if (LastCall)
				LastCall->OuterCall = NewCall;
			else
				FirstCall = NewCall;
			LastCall = NewCall;
		}
		ResultOf<SNothing> OnFinish(CParser& Parser) override
		{
			Parser.CheckToken();
			this->Trailing = STrailing{
				LastCall      ? *LastCall->CallParameter.BlockTrailing.TrailingStart
				: PriorClause ? *PriorClause->BlockTrailing.TrailingStart
							  : Parser.Cursor,
				CaptureType()};
			ULANG_GRAMMAR_RUN(SExpr::OnFinish(Parser));
			if (Clauses[0])
			{
				ULANG_ASSERT(PriorClause);
				// Generate this macro invocation.
				ULANG_GRAMMAR_RUN(Parser.UpdateFrom(
					*this->OuterExpr, PriorClause->BlockTrailing,
					Parser.Gen.Invoke(Parser.SnipFinished(this->Start, *PriorClause),
						Parser.ApplyTrailing(*this->OuterExpr),
						*Clauses[0], Clauses[1], Clauses[2])));

				// Handle remaining calls on the stack now with another Invoke.
				if (!FirstCall) // Disable this to check soundness of logic below.
					return SNothing{};
				SInvoke NewTarget{
					u8"nested macro invocation",
					*this->OuterExpr,
					this->Start,
					SToken::None(),
					STokenSet{u8"do"},
					STokenSet{u8"until", u8"catch"},
					FirstCall,
					LastCall
                };
				if (!this->ExprStop)
					return Parser.Invoke(NewTarget, Parser.Cursor, CaptureType());
				else
					return NewTarget.OnFinish(Parser);
			}
			else if (!StartToken)
			{
				// Not a macro, and a macro isn't required, so flush accumulated call
				// and specifiers to the nearest outer EPrec::Call, needed for if{a}else
				// if{b}<c> associating as (if{a}else if{b})<c>.
				if (!this->OuterExpr)
				{
					return SNothing{};
				} // This should never occur - though without it some C++ semantic
				  // analysis checkers get upset when passing to FinishExpr() below.
				ULANG_GRAMMAR_LET(
					InsertCall,
					Parser.FinishExpr(SToken::None(), EPrec::Call, *this->OuterExpr));
				if (!InsertCall)
					return Parser.S61(FirstCall ? FirstCall->CallWhat : u8"macro end");
				for (auto Call = FirstCall; Call; Call = Call->OuterCall)
				{
					Call->CallParameter.BlockSnippet =
						Snip(SPoint::Start(Call->CallParameter.BlockSnippet),
							*Call->CallParameter.BlockTrailing.TrailingStart);
					ULANG_GRAMMAR_RUN(Parser.UpdateFrom(
						*InsertCall, Call->CallParameter.BlockTrailing,
						Parser.Gen.Call(
							Snip(InsertCall->Start,
								SPoint::Stop(Call->CallParameter.BlockSnippet)),
							Call->CallMode, Parser.ApplyTrailing(*InsertCall),
							Call->CallParameter)));
				}
				return SNothing{};
			}
			else
				return Parser.S76(
					What); // Error for reserved word not followed by macro.
		}
	};
	ResultOf<SNothing>
	InvokeClause(SInvoke& Target, uint64_t WhichClause, SCursor BlockStart,
		SBlockInternal& Block0, SCursor NextBlockStart,
		const CaptureType& NextTokenLeading = CaptureType())
	{
		// We've committed to producing a macro invocation, so accumulate specifiers
		// m<a> and handle any prior m(a).catch up to clauses from call m(c).
		auto Specifiers = SyntaxesType{};
		const SSnippet* FirstSpecifier = nullptr;
		while (auto Call = Target.FirstCall)
		{
			Target.FirstCall = Target.FirstCall->OuterCall;
			ApplyTrailing(Call->CallParameter, Call->CallTrailingStop);
			if (Call->CallMode == EMode::Open)
			{
				ULANG_ASSERT(!Target.Clauses[0] && !Target.Clauses[1] && !Target.Clauses[2]);
				if (FirstSpecifier)
					Call->CallParameter.BlockSnippet =
						Snip(SPoint::Start(*FirstSpecifier),
							SPoint::Stop(Call->CallParameter.BlockSnippet));
				Call->CallParameter.Specifiers = Specifiers;
				Target.Clauses[0] = &Call->CallParameter;
				Target.Of = nullptr;
				return InvokeClause(Target, WhichClause, BlockStart, Block0,
					NextBlockStart, NextTokenLeading);
			}
			else if (Call->CallMode == EMode::With)
			{
				if (!Gen.SyntaxesLength(Specifiers))
					FirstSpecifier = &Call->CallParameter.BlockSnippet;
				ULANG_GRAMMAR_LET(E, Gen.Parenthesis(Call->CallParameter));
				Gen.SyntaxesAppend(Specifiers, E);
			}
			else
				ULANG_UNREACHABLE();
		}
		if (Target.PriorClause)
			ApplyTrailing(*Target.PriorClause, FirstSpecifier
												   ? SPoint::Start(*FirstSpecifier)
												   : BlockStart);
		if (FirstSpecifier)
			Block0.BlockSnippet = Snip(SPoint::Start(*FirstSpecifier),
				SPoint::Stop(Block0.BlockSnippet));
		Block0.Specifiers = Specifiers;
		Target.LastCall = nullptr; // Catch up so subsequent accumulation works.
		Target.Clauses[WhichClause] = &Block0;
		Target.PriorClause = Block0.BlockSnippet ? &Block0 : Target.PriorClause;
		if (!Target.ExprStop)
			return Invoke(Target, NextBlockStart, NextTokenLeading);
		else
			return Target.OnFinish(*this);
	}

	ResultOf<SNothing> InvokeParens(SInvoke& Target, const SCursor& BlockStart)
	{
		// Paren := '(' List ')' Space
		SCursor PostfixStart = Cursor;
		EatToken();
		ULANG_GRAMMAR_LET(Block0, List(u8")", &CParser::S82, Cursor, CaptureType(),
									  EPunctuation::Parens, Cursor));
		if (Eat(u8":)"))
		{
			// If we're in an attribute like @a (b:)c, move the QualIdent to the Base
			// handler for '@'.
			ULANG_GRAMMAR_LET(InsertExpr,
				FinishExpr(SToken::None(), EPrec::Prefix, Target));
			if (!InsertExpr || !InsertExpr->QualIdentTarget)
				return CParser::S82(u8":)");
			InsertExpr->QualIdentTarget->Start = PostfixStart;
			ULANG_GRAMMAR_LET(Id,
				QualIdentQualified(*InsertExpr, PostfixStart, Block0));
			ULANG_GRAMMAR_RUN(UpdateSpaceTrailing(*InsertExpr->QualIdentTarget, Id));
			return SNothing{};
		}
		ULANG_GRAMMAR_RUN(RequireClose(PostfixStart, u8"(", u8")", &CParser::S82));
		Block0.BlockSnippet = Snip(BlockStart);
		auto NewCall = SCall{u8"(", Cursor, EMode::Open, Block0};
		ULANG_GRAMMAR_RUN(SpaceTrailing(NewCall.CallParameter.BlockTrailing));
		NewCall.CallTrailingStop = Cursor;
		Target.UpdateLastCall(&NewCall);
		Target.Of = &NewCall;
		Target.AllowPostfixes =
			(Target.AllowPostfixes & ~ParenPostfixes) | Target.InTokens;
		if (Target.StartToken == SToken(u8"if")) // Disallow if(a)<b>{c} to enable future if(a) b.
			Target.AllowPostfixes = Target.AllowPostfixes & ~WithPostfixes;
		return Invoke(Target, Cursor);
	}

	ResultOf<SNothing> InvokeLess(SInvoke& Target, const SCursor& BlockStart,
		const CaptureType& TokenLeading)
	{
		// Specs := [ScanKey "with" Key] '<' Scan Choose Space '>' Space (Specs |
		// !Specs)
		auto PostfixToken = Cursor.Token;
		SText CallToken;
		CaptureType PunctuationLeading;
		if (PostfixToken == SToken(u8"with"))
		{
			EatToken();
			CallToken = u8"with";
			ULANG_GRAMMAR_RUN(Space(PunctuationLeading));
			if (Cursor.Token != SToken(u8"<"))
				return S78();
		}
		EatToken();
		ULANG_GRAMMAR_LET(Leading, Space());
		// We parse specifier at EPrec::Choose, but FinishExpr at EPrec::Less to
		// right-associate nested '<'. LessExpr receives TrailingCapture so
		// specifiers can handle it and less-than can propagate it. If we parsed
		// a<b<c at just EPrec::Choose, the inner FinishExpr forces the finishes
		// EPrec::Choose, whose FinishExpr forces the outer EPrec::Choose, so the
		// outer Postfix incorrectly parses first. This is as simple as it can be;
		// other approaches add bloat.
		bool GotLess = false;
		auto LessExpr =
			SWhenExpr(EPrec::Less, &Target, AllowLess, Cursor, Leading,
				[&](SExpr& LessExpr) -> ResultOf<SNothing> {
					// We get here only if we parse a Less expression a<b, not
					// if we parse a specifier.
					ULANG_ASSERT(GotLess);
					auto& InsertExpr =
						*LessExpr.OuterExpr; // Dynamic, not necessarily Target.
					return UpdateFrom(
						InsertExpr, LessExpr.Trailing,
						Gen.InfixToken(SnipFinished(InsertExpr.Start, LessExpr),
							PostfixToken->PostfixMode,
							ApplyTrailing(InsertExpr),
							PostfixToken->Symbol, *LessExpr));
				});
		return WhenExpr(
			u8"<", EPrec::Choose, EPrec::Less, &LessExpr, CaptureType(),
			[&](SExpr& RightExpr) -> ResultOf<SNothing> {
				ULANG_GRAMMAR_RUN(
					UpdateFrom(LessExpr, RightExpr.Trailing, *RightExpr));
				if (Eat(u8">"))
				{
					// Parsed a specifier. Abandon LessExpr.
					auto RightSyntax = Gen.Leading(Leading, ApplyTrailing(LessExpr));
					auto SpecifierBlock =
						SingletonBlock(Snip(BlockStart, Cursor), RightSyntax,
							PunctuationLeading, EPunctuation::AngleBrackets);
					SpecifierBlock.Token = CallToken;
					SpecifierBlock.TokenLeading = TokenLeading;
					auto NewCall = SCall{u8"<", Cursor, EMode::With, SpecifierBlock};
					ULANG_GRAMMAR_RUN(
						SpaceTrailing(NewCall.CallParameter.BlockTrailing));
					NewCall.CallTrailingStop = Cursor;
					Target.UpdateLastCall(&NewCall);
					return Invoke(Target, Cursor);
				}
				else if (PostfixToken != SToken(u8"with"))
				{
					// We parsed a Less expression a<b so figure out where it lands and
					// finish parsing it.
					GotLess = true;
					ULANG_GRAMMAR_SET(LessExpr.OuterExpr,
						FinishExpr(SToken(u8"<"), EPrec::Less, Target));
					if (!LessExpr.OuterExpr)
						return S61(u8"<");
					return Postfix(u8"<", EPrec::Less,
						LessExpr); // Trigger's LessExpr's SWhenExpr.
				}
				else
					return S79();
			});
	}

	ResultOf<SNothing> InvokeBlock(SInvoke& Target, const SCursor& BlockStart,
		const CaptureType& TokenLeading)
	{
		// Block := Brace | DotSpace Space Def Space | (DotSpace | ':') Space Ind
		// List Ded
		bool Fails = false;
		ULANG_GRAMMAR_LET(Block0, Block(u8"macro invocation", Target, BlockStart,
									  TokenLeading, Fails));
		if (!Fails)
		{
			Target.AllowPostfixes =
				(Target.AllowPostfixes & ~ParenPostfixes & ~BlockPostfixes) | Target.InTokens | Target.PostTokens;
			if (Target.Of)
				Target.AllowPostfixes = Target.AllowPostfixes & ~Target.InTokens;
			if (Target.StartToken == SToken(u8"if")) // Disallow if{a}<b>.. so else-if never finishes
													 // before last InvokedClause.
				Target.AllowPostfixes = Target.AllowPostfixes & ~WithPostfixes;
			return InvokeClause(Target, Target.Of != 0, BlockStart, Block0, Cursor);
		}
		return Target.OnFinish(*this); // For In, '.' QualIdent.
	}

	ResultOf<SNothing> InvokeDoThen(SInvoke& Target, const SCursor& BlockStart,
		const CaptureType& TokenLeading)
	{
		// Do   := ScanKey "do"    Key (KeyBlock | Def)
		// Then := ScanKey "then"  Key (KeyBlock | Def)
		auto PostfixToken = Cursor.Token;
		EatToken();
		ULANG_GRAMMAR_LET(PunctuationLeading, Space());
		ULANG_GRAMMAR_LET(Block0,
			KeyBlock(EPrec::Def, Target, BlockStart, TokenLeading,
				PostfixToken->Symbol, PunctuationLeading));
		Target.AllowPostfixes =
			(Target.AllowPostfixes & ~Target.InTokens) | Target.PostTokens;
		return InvokeClause(Target, 1, BlockStart, Block0, Cursor);
	}

	ResultOf<SNothing> InvokeUntil(SInvoke& Target, const SCursor& BlockStart,
		const CaptureType& TokenLeading)
	{
		// Until := ScanKey "until" Key (KeyBlock | Def) | ..
		auto PostfixToken = Cursor.Token;
		EatToken();
		ULANG_GRAMMAR_LET(PunctuationLeading, Space());
		ULANG_GRAMMAR_LET(Block0,
			KeyBlock(EPrec::Def, Target, BlockStart, TokenLeading,
				PostfixToken->Symbol, PunctuationLeading));
		Target.AllowPostfixes = STokenSet{};
		return InvokeClause(Target, 2, BlockStart, Block0, Cursor);
	}

	ResultOf<SNothing> InvokeCatch(SInvoke& Target, const SCursor& BlockStart,
		const CaptureType& TokenLeading)
	{
		// Until := .. | ScanKey "catch" Key Invoke
		// Chain more catches only if !Target.FirstCall. Update AllowTokens to
		// reenable catch.
		EatToken();
		auto CatchExpr =
			SWhenExpr(EPrec::Base, &Target, AllTokens, BlockStart, TokenLeading,
				[&](SExpr& CatchExpr) -> ResultOf<SNothing> {
					auto Block0 = SingletonBlock(BlockStart, CatchExpr);
					return InvokeClause(Target, 2, BlockStart, Block0,
						*CatchExpr.Finished);
				});
		ULANG_GRAMMAR_RUN(UpdateSpaceTrailing(
			CatchExpr, Gen.Native(Snip(BlockStart), u8"catch")));
		SInvoke CatchTarget{
			u8"catch", CatchExpr,
			BlockStart, SToken(u8"catch"),
			STokenSet{u8"do"},
			STokenSet{u8"until", u8"catch"}
        };
		ULANG_GRAMMAR_RUN(Invoke(CatchTarget, Cursor));
		if (!CatchExpr.Finished)
			CatchExpr.OnFinish(*this);
		return SNothing();
	}

	ResultOf<SNothing> InvokeElse(SInvoke& Target, const SCursor& BlockStart,
		const CaptureType& TokenLeading)
	{
		// Else := ScanKey "else" Key (ScanKey If | !(ScanKey If) (KeyBlock | Def))
		auto PostfixToken = Cursor.Token;
		EatToken();
		ULANG_GRAMMAR_LET(PunctuationLeading, Space());
		Target.AllowPostfixes = STokenSet{};
		if (Cursor.Token == SToken(u8"if"))
		{
			// Grammar makes "else if" a special case so "if(a){b}else if(c){d}+1"
			// is equivalent to "(if(a){b}else if(c){d})+1", not "if(a){b}else
			// (if(c){d}+1)".
			return WhenExpr(
				u8"else if", EPrec::Base, EPrec::Base, &Target, PunctuationLeading,
				[&](SExpr& ElseExpr) -> ResultOf<SNothing> {
					Target.ExprStop = ElseExpr.ExprStop;
					auto ElseBlock = SingletonBlock(BlockStart, ElseExpr);
					ElseBlock.Token = PostfixToken->Symbol;
					ElseBlock.TokenLeading = TokenLeading;
					return InvokeClause(Target, 2, BlockStart, ElseBlock, Cursor);
				});
		}
		else
		{
			ULANG_GRAMMAR_LET(ElseBlock,
				KeyBlock(EPrec::Def, Target, BlockStart, TokenLeading,
					PostfixToken->Symbol, PunctuationLeading));
			return InvokeClause(Target, 2, BlockStart, ElseBlock, Cursor);
		}
	}

	template <SToken PostfixToken>
	ResultOf<SNothing> InvokeSeq(SInvoke& Target, const SCursor& BlockStart,
		const CaptureType& TokenLeading)
	{
		auto PostfixStart = Cursor;
		if (!Target.Clauses[0] || Target.FirstCall)
		{
			// If we have <m;c>, <m(a);c>, <m<a>;c>, <m{a}<b>;c>, introduce a new
			// block and recurse back.
			SBlockInternal Block0{Snip()};
			return InvokeClause(Target, Target.FirstCall != nullptr, BlockStart,
				Block0, BlockStart, TokenLeading);
		}
		EatToken();
		if (!Target.OuterExpr->MarkupTag)
			return S40();
		if (Target.PriorClause)
			ApplyTrailing(*Target.PriorClause,
				BlockStart); // TODO: Fix, as this is bad for <m(a)\n ;a>.
		Target.OuterExpr->MarkupFinished = true;
		CaptureType PreContent, PostContent;
		if constexpr (PostfixToken == SToken(u8","))
		{
			ULANG_GRAMMAR_LET(InnerContent,
				MarkupExpr(Target.OuterExpr, PostfixStart));
			return InvokeMarkup(Target, TokenLeading, CaptureType(), InnerContent,
				CaptureType());
		}
		if constexpr (PostfixToken == SToken(u8";"))
		{
			Gen.MarkupStart(PreContent, Snip(PostfixStart));
			ULANG_GRAMMAR_LET(Content, Trimmed(false));
			SCursor ContentsEnd = Cursor;
			ULANG_GRAMMAR_RUN(Require(u8">", &CParser::S51));
			Gen.MarkupStop(PostContent, Snip(ContentsEnd));
			return InvokeMarkup(Target, TokenLeading, PreContent, Content,
				PostContent);
		}
		if constexpr (PostfixToken == SToken(u8">"))
		{
			Gen.MarkupStart(PreContent, Snip(PostfixStart));
			ULANG_GRAMMAR_LET(Content, Trimmed(true));
			SCursor PostStart = Cursor;
			ULANG_GRAMMAR_RUN(Require(u8"<", &CParser::S52));
			Gen.MarkupStart(PostContent, Snip(PostStart));
			for (auto* ExpectMarkup = Target.OuterExpr; ExpectMarkup;
				 ExpectMarkup = ExpectMarkup->OuterMarkup)
			{
				ULANG_GRAMMAR_RUN(Require(u8"/", &CParser::S44));
				if (!IsAlpha(Cursor[0]))
					return S44(ExpectMarkup->MarkupTag);
				auto TagStart = Cursor;
				ULANG_GRAMMAR_LET(EndTag, Ident());
				if (EndTag != ExpectMarkup->MarkupTag)
					return S43(ExpectMarkup->MarkupTag, EndTag);
				auto TagSnippet = Snip(TagStart);
				Gen.MarkupTag(PostContent, TagSnippet);
				ULANG_GRAMMAR_RUN(Space(PostContent));
			}
			SCursor PostEnd = Cursor;
			ULANG_GRAMMAR_RUN(Require(u8">", &CParser::S44));
			Gen.MarkupStop(PostContent, Snip(PostEnd));
			return InvokeMarkup(Target, TokenLeading, PreContent, Content,
				PostContent);
		}
		if constexpr (PostfixToken == SToken(u8":>"))
		{
			Gen.MarkupStart(PreContent, Snip(PostfixStart));
			ULANG_GRAMMAR_RUN(Space(PreContent));
			if (!Ending())
				return S46();
			ULANG_GRAMMAR_LET(SavedContext, Ind());
			ULANG_GRAMMAR_LET(Content, Contents(true));
			ULANG_GRAMMAR_RUN(Ded(SavedContext, &CParser::S54));
			ULANG_GRAMMAR_RUN(Space(PostContent));
			return InvokeMarkup(Target, TokenLeading, PreContent, Content,
				PostContent);
		}
	}

	ResultOf<SNothing> InvokeNewLine(SInvoke& Target, const SCursor& BlockStart,
		CaptureType& TokenLeading)
	{
		ULANG_GRAMMAR_LET(
			ScanToken,
			ScanKey(TokenLeading,
				Target.AllowPostfixes & STokenSet{u8"catch", u8"do", u8"else", u8"then", u8"until", u8"with", u8"{", u8">", u8":>", u8",", u8";"}));
		if (ScanToken)
			return Invoke(Target, BlockStart, TokenLeading);
		return Target.OnFinish(*this);
	}

	ResultOf<SNothing> Invoke(SInvoke& Target, SCursor BlockStart,
		CaptureType TokenLeading = CaptureType())
	{
		// Markup  := '<' Scan Tags Scan ":>" Space Ind Contents Ded
		//         |  '<' Scan Tags Scan ';'  Scan      Contents '>'
		//         |  '<' Scan Tags Scan '>'  Scan      Contents '</' Ident Space
		//         {'/' Ident Space} '>'
		// Tags    := Space (!'/' Call ScanKey '.' | !Reserved) QualIdent Space
		// {Invoke} [',' Scan Tags] Postfix := .. | !Invoke (Paren | Specs) | ..
		// Invoke  :=          [Specs] (Paren [Specs] (Block | Do  ) | Block
		// [[Specs] Do  ]) (Until | !Until) If      := "if" Key [Specs] (Paren
		// (Block | Then) | Block [        Then]) (Else  | !Else )
		ULANG_ASSERT(CheckToken());
		if (!Target.AllowPostfixes.Has(Cursor.Token))
			return Target.OnFinish(*this);

		// Definitely starting a new potential clause.
		switch (uint8_t(Cursor.Token))
		{
			case SToken(u8"("):
			{
				return InvokeParens(Target, BlockStart);
			}
			case SToken(u8"<"):
			case SToken(u8"with"):
			{
				return InvokeLess(Target, BlockStart, TokenLeading);
			}
			case SToken(u8"{"):
			case SToken(u8"."):
			case SToken(u8":"):
			case SToken(u8"in"):
			{
				return InvokeBlock(Target, BlockStart, TokenLeading);
			}
			case SToken(u8"do"):
			case SToken(u8"then"):
			{
				return InvokeDoThen(Target, BlockStart, TokenLeading);
			}
			case SToken(u8"until"):
			{
				return InvokeUntil(Target, BlockStart, TokenLeading);
			}
			case SToken(u8"catch"):
			{
				return InvokeCatch(Target, BlockStart, TokenLeading);
			}
			case SToken(u8"else"):
			{
				return InvokeElse(Target, BlockStart, TokenLeading);
			}
			case SToken(u8","):
			{
				return InvokeSeq<SToken(u8",")>(Target, BlockStart, TokenLeading);
			}
			case SToken(u8";"):
			{
				return InvokeSeq<SToken(u8";")>(Target, BlockStart, TokenLeading);
			}
			case SToken(u8">"):
			{
				return InvokeSeq<SToken(u8">")>(Target, BlockStart, TokenLeading);
			}
			case SToken(u8":>"):
			{
				return InvokeSeq<SToken(u8":>")>(Target, BlockStart, TokenLeading);
			}
			case SToken::NewLine():
			{
				return InvokeNewLine(Target, BlockStart, TokenLeading);
			}
			default: // Ensure static analysis happy with all permutations covered
				break;
		}
		ULANG_UNREACHABLE(); // AllowPostfixes makes this unreachable.
	}

	// Markup.
	ResultOf<SNothing> InvokeMarkup(SInvoke& InvokeTarget,
		const CaptureType& TokenLeading,
		const CaptureType& PreContent,
		SyntaxType& Content,
		const CaptureType& PostContent)
	{
		auto& MarkupExpr = *InvokeTarget.OuterExpr;
		auto NoTrailing = STrailing{Cursor, CaptureType()};
		ULANG_GRAMMAR_RUN(UpdateFrom(
			MarkupExpr, NoTrailing,
			Gen.InvokeMarkup(Snip(*MarkupExpr.MarkupStart),
				!MarkupExpr.OuterMarkup ? u8"<" : u8",",
				MarkupExpr.ExprLeading, ApplyTrailing(MarkupExpr),
				InvokeTarget.Clauses[0], InvokeTarget.Clauses[1],
				TokenLeading, PreContent, Content, PostContent)));
		MarkupExpr.ExprLeading = CaptureType();
		return MarkupExpr.OnFinish(*this);
	}
	ResultOf<SyntaxType> Markup()
	{
		// Base = .. | Markup | ..
		ULANG_ASSERT(Cursor[0] == '<');
		auto Start = Cursor;
		Next(1);
		return MarkupExpr(nullptr, Start);
	}

	// Expressions.
	struct SIns
	{
		SCursor Start;
		SToken InToken;
		SCursor NextStart;
		CaptureType NextLeading;
		const SIns* NextIns;
	};
	ResultOf<SNothing> InChoose(SExpr& PostfixExpr, SCursor Start,
		const SIns* Ins = nullptr)
	{
		// In := ("in" Key | ':') Space (In | NotEq)

		TScopedGuard ExprDepthGuard(ExprDepth, ExprDepth + 1);
		if (ExprDepth > VERSE_MAX_EXPR_DEPTH)
			return S99();

		// Here, we parse the Choose into PostfixExpr without finishing it.
		auto InToken = Cursor.Token;
		if (InPrefixes.Has(Cursor.Token))
		{
			EatToken();
			auto NextStart = Cursor;
			ULANG_GRAMMAR_LET(NextLeading, Space());
			auto NextIn = SIns{Start, InToken, NextStart, NextLeading, Ins};
			return InChoose(PostfixExpr, Cursor, &NextIn);
		}
		ULANG_GRAMMAR_RUN(WhenExpr(
			InToken->Symbol, EPrec::Choose, EPrec::Choose, &PostfixExpr,
			CaptureType(), [&](SExpr& Right) -> ResultOf<SNothing> {
				auto NewRight = *Right;
				for (; Ins; Ins = Ins->NextIns)
				{
					auto RightBlock =
						SingletonBlock(SnipFinished(Ins->NextStart, Right),
							Gen.Leading(Ins->NextLeading, NewRight));
					ULANG_GRAMMAR_SET(NewRight,
						Gen.PrefixToken(SnipFinished(Ins->Start, Right),
							Ins->InToken->PrefixMode,
							Ins->InToken->Symbol, RightBlock,
							false));
				}
				return UpdateFrom(PostfixExpr, Right.Trailing, NewRight);
			}));
		return SNothing{};
	}
	ResultOf<SNothing> DefPostfix(SExpr& Target)
	{
		// Def := (.. | .. Space (('='|':='|'+='|'*='|'/=') Space (BraceInd | Def) |
		// !'=' !':=')) {&In Def | ..}
		auto DefineToken = Cursor.Token;
		if (DefPostfixes.Has(DefineToken))
		{
			EatToken();
			ULANG_GRAMMAR_LET(Right,
				BraceInd(DefineToken->Symbol, EPrec::Def, Target));
			ULANG_GRAMMAR_RUN(UpdateFrom(
				Target, Right.BlockTrailing,
				Gen.InfixBlock(SnipFinished(Target.Start, Right),
					ApplyTrailing(Target), DefineToken->Symbol, Right)));
		}
		return SNothing{};
	}

	ResultOf<SyntaxType> Digit(SExpr& Target)
	{
		if (Cursor[0] == '0' && Cursor[1] == 'o' && IsHex(Cursor[2]))
		{
			ULANG_GRAMMAR_LET(c, Char8());
			return Gen.Char8(Snip(Target.Start), c);
		}
		else if (Cursor[0] == '0' && Cursor[1] == 'u' && IsHex(Cursor[2]))
		{
			ULANG_GRAMMAR_LET(c, Char32());
			return Gen.Char32(Snip(Target.Start), c, true, false);
		}
		else
			return Num();
	}

	ResultOf<SyntaxType> Quote(SExpr& Target)
	{
		// String := '"' {Interp | CharEsc | !('\'|'{'|'}'|'"') Text} '"'
		Next(1);
		ULANG_GRAMMAR_LET(Capture, String<EPlace::String>(Cursor));
		ULANG_GRAMMAR_RUN(Require(u8"\"", &CParser::S32));
		return Gen.String(Snip(Target.Start), Capture);
	}

	ResultOf<SNothing> Attribute(SText What, EPrec Prec, SExpr& Target)
	{
		// Expr := .. | '@' Space Call Scan &('@'|QualIdent) Expr
		EatToken();
		TResult<SyntaxType, SNothing> AttrSyntax;

		// Set up for parsing RightExpr later. It may receive QualIdent early, e.g.
		// in @a (b:)c...
		auto RightExpr =
			SWhenExpr(EPrec::Expr, &Target, AllTokens, Cursor, CaptureType(),
				[&](SExpr& RightExpr) {
					return UpdateFrom(Target, RightExpr.Trailing,
						Gen.PrefixAttribute(
							SnipFinished(Target.Start, RightExpr),
							*AttrSyntax, *RightExpr));
				});

		// Parse attribute, with RightExpr as its QualIdent target for @a (b:)c.
		ULANG_GRAMMAR_LET(AttrLeading, Space());
		ULANG_GRAMMAR_RUN(WhenExpr(
			u8"@", EPrec::Call, EPrec::Prefix, nullptr, AttrLeading,
			[&](SExpr& AttrExpr) -> ResultOf<SNothing> {
				ApplyTrailing(AttrExpr, true);
				AttrSyntax = *AttrExpr;
				return SNothing{};
			},
			AllTokens, &CParser::S71, &RightExpr));

		// Parse all or the remainder of RightExpr.
		if (!RightExpr.ExprSyntax)
		{
			ULANG_GRAMMAR_RUN(Scan(RightExpr.ExprLeading));
			RightExpr.Start = Cursor;
			if (Cursor[0] != '@' && Cursor[0] != '(' && !IsAlnum(Cursor[0]))
				return S67();
			ULANG_GRAMMAR_RUN(
				Base(What, Prec, RightExpr, &CParser::S71, &CParser::S60));
		}
		ULANG_GRAMMAR_RUN(
			Postfix(What, Prec, RightExpr, &CParser::S71, &CParser::S60));
		return RightExpr.Result;
	}

	ResultOf<SyntaxType> Path(SExpr& Target)
	{
		// Base = (.. | Path | ..) Space
		ULANG_GRAMMAR_LET(P, Path());
		return Gen.Path(Snip(Target.Start), P);
	}

	ResultOf<SNothing> In(SExpr& Target)
	{
		// In  := ("in" Key | ':') Space (In | NotEq)
		// Def := (Or | (In|Var) Space (('='|':='|'+='|'*='|'/=') Space (BraceInd |
		// Def) | !'=' !':=')) {&In Def | ..}

		// Postfix definition x:t leads here, so capture any :t<v, keeping x:t
		// definition at the top.
		SToken BaseToken = Cursor.Token;
		auto PostfixExpr = SWhenExpr(
			EPrec::Def, &Target, AllTokens, Cursor, CaptureType(),
			[&](SExpr& PostfixExpr) -> ResultOf<SNothing> {
				// This runs when In or Postfix below finishes PostfixExpr.
				ULANG_GRAMMAR_RUN(
					UpdateFrom(Target, PostfixExpr.Trailing, *PostfixExpr));
				return DefPostfix(Target);
			},
			nullptr);

		// In parses Choose into PostfixExpr, then Postfix extends to NotEq.
		ULANG_GRAMMAR_RUN(InChoose(PostfixExpr, Target.Start));
		ULANG_GRAMMAR_RUN(Postfix(BaseToken->Symbol, EPrec::NotEq, PostfixExpr));
		return SNothing{};
	}

	ResultOf<SNothing> VarKeyword(SExpr& Target)
	{
		// Var := (("var" [Space '<' Space Choose Space '>'] [Space "live"])|("set"
		// [Space "live"])|"ref"|"alias"|"live") Key Space Choose Def := (Or |
		// (In|Var) Space (('='|':='|'+='|'*='|'/=') Space (BraceInd | Def) | !'='
		// !':=')) {&In Def | ..}
		SToken BaseToken = Cursor.Token;
		bool bIsVar = BaseToken == SToken(u8"var");
		bool bIsSet = BaseToken == SToken(u8"set");
		bool bLive = BaseToken == SToken(u8"live");
		EatToken();
		SyntaxesType Attributes;
		// This syntax will evolve from var<specifier> x:t=v to x:t<specifier>=v.
		if (bIsVar)
		{
			while (true)
			{
				ULANG_GRAMMAR_LET(StartingSpace, Space());
				if (Cursor.Token != SToken(u8"<"))
				{
					break;
				}

				EatToken();
				ULANG_GRAMMAR_RUN(Space());
				ULANG_GRAMMAR_RUN(WhenExpr(u8"<", EPrec::Choose, EPrec::Less, &Target,
					StartingSpace,
					[&](SExpr& Expr) -> ResultOf<SNothing> {
						ApplyTrailing(Expr, true);
						Gen.SyntaxesAppend(Attributes, *Expr);
						return SNothing{};
					}));
				ULANG_GRAMMAR_RUN(Space());
				ULANG_GRAMMAR_RUN(RequireClose(Cursor, u8"<", u8">", &CParser::S85));
			}
		}
		auto ChooseStart = Cursor;
		ULANG_GRAMMAR_LET(Middle, Space());
		if (bIsVar || bIsSet)
		{
			if (Cursor.Token == SToken(u8"live"))
			{
				EatToken();
				bLive = true;
				ChooseStart = Cursor;
				ULANG_GRAMMAR_RUN(Space());
			}
		}
		return WhenExpr(
			BaseToken->Symbol, EPrec::Choose, EPrec::Choose, &Target, Middle,
			[&](SExpr& Choose) -> ResultOf<SNothing> {
				auto ChooseBlock = SingletonBlock(ChooseStart, Choose);
				ULANG_GRAMMAR_RUN(UpdateFrom(
					Target, ChooseBlock.BlockTrailing,
					Gen.PrefixToken(SnipFinished(Target.Start, Choose),
						BaseToken->PrefixMode, BaseToken->Symbol,
						ChooseBlock, false, Attributes, bLive)));
				if (DefPostfixes.Has(
						Cursor.Token)) // Translate "set x=3" to "set{x}:=3".
					return DefPostfix(Target);
				return SNothing{};
			});
	}

	ResultOf<SNothing> Not(SExpr& Target)
	{
		// Not  := .. | ("not" Key) Space Not
		// Def  := .. | (.. | '..') Space Def
		SToken BaseToken = Cursor.Token;
		EatToken();
		auto RightStart = Cursor;
		ULANG_GRAMMAR_LET(Middle, Space());
		return WhenExpr(
			BaseToken->Symbol, BaseToken->PrefixPrec, BaseToken->PrefixPrec,
			&Target, Middle, [&](SExpr& RightExpr) {
				auto RightBlock = SingletonBlock(RightStart, RightExpr);
				return UpdateFrom(
					Target, RightBlock.BlockTrailing,
					Gen.PrefixToken(SnipFinished(Target.Start, RightExpr),
						BaseToken->PrefixMode, BaseToken->Symbol,
						RightBlock, false));
			});
	}

	ResultOf<SNothing> Ampersand(SExpr& Target)
	{
		// Def := .. | ('&' | ..) Space Def
		SToken BaseToken = Cursor.Token;
		EatToken();
		ULANG_GRAMMAR_LET(Middle, Space());
		return WhenExpr(
			u8"&", BaseToken->PrefixPrec, BaseToken->PrefixPrec, &Target, Middle,
			[&](SExpr& Right) {
				return UpdateFrom(
					Target, Right.Trailing,
					Gen.Escape(SnipFinished(Target.Start, Right), *Right));
			});
	}

	ResultOf<SNothing> Arithmetic(SExpr& Target)
	{
		// Prefix := .. | ('^' | '?' | .. | '+' | '-' | '*') Space (Brace | Prefix)
		SToken BaseToken = Cursor.Token;
		EatToken();
		return WhenBraceCall(
			BaseToken->Symbol, BaseToken->PrefixPrec, Target,
			[&](SBlockInternal& RightBlock) -> ResultOf<SNothing> {
				return UpdateFrom(
					Target, RightBlock.BlockTrailing,
					Gen.PrefixToken(SnipFinished(Target.Start, RightBlock),
						BaseToken->PrefixMode, BaseToken->Symbol,
						RightBlock,
						RightBlock.Punctuation == EPunctuation::Braces));
			});
	}

	ResultOf<SNothing> LeftBracket(SExpr& Target)
	{
		// Prefix := .. | (.. | '[' List ']' | ..) Space (Brace | Prefix)
		SToken BaseToken = Cursor.Token;
		EatToken();
		ULANG_GRAMMAR_LET(Left, List(u8"]", &CParser::S85, Cursor, CaptureType(),
									EPunctuation::None, Cursor));
		ULANG_GRAMMAR_RUN(RequireClose(Target.Start, u8"[", u8"]", &CParser::S85));
		return WhenBraceCall(
			u8"[]", BaseToken->PrefixPrec, Target,
			[&](SBlockInternal& Right) -> ResultOf<SNothing> {
				return UpdateFrom(
					Target, Right.BlockTrailing,
					Gen.PrefixBrackets(SnipFinished(Target.Start, Right), Left,
						Right));
			});
	}

	ResultOf<SNothing> If(SExpr& Target)
	{
		// If := "if" Key [Specs] (Paren (Block | Then) | Block [Then]) (Else |
		// !Else)
		Target.MarkupTag =
			u8"if"; // We propagate markup to support <if(a)>Hello</if>.
		EatToken();
		ULANG_GRAMMAR_RUN(
			UpdateSpaceTrailing(Target, Gen.Native(Snip(Target.Start), u8"if")));
		SInvoke IfTarget{u8"if",
			Target,
			Target.Start,
			SToken(u8"if"),
			STokenSet{u8"then"},
			STokenSet{u8"else"}};
		return Invoke(IfTarget, Cursor);
	}

	ResultOf<SNothing> ControlFlowKeyword(SExpr& Target)
	{
		// Def := .. | Return [KeyBlock|Def] StopDef
		SToken BaseToken = Cursor.Token;
		EatToken();
		SBlockInternal Right;
		Right.BlockTrailing.TrailingStart = Cursor;
		ULANG_GRAMMAR_SET(Right.BlockTrailing.TrailingCapture, Space());
		if (!StopDef.Has(Cursor.Token))
			ULANG_GRAMMAR_SET(Right, KeyBlock(EPrec::Def, Target,
										 *Right.BlockTrailing.TrailingStart,
										 CaptureType(), u8"",
										 Right.BlockTrailing.TrailingCapture));
		return UpdateFrom(Target, Right.BlockTrailing,
			Gen.PrefixToken(SnipFinished(Target.Start, Right),
				BaseToken->PrefixMode, BaseToken->Symbol,
				Right, false));
	}

	ResultOf<SNothing> Base(SText What, EPrec Prec, SExpr& Target,
		ErrorType (CParser::*OnTokenError)(SText),
		ErrorType (CParser::*OnPrecError)(SText, SText))
	{
		// Base := '(' List ')' | Num | Char | Path | String | Markup | If |
		// !Reserved QualIdent
		ULANG_ASSERT(CheckToken());
		if (Prec > Cursor.Token->PrefixPrec)
		{
			return (Cursor.Token->PrefixPrec == EPrec::Never)
					 ? (this->*OnTokenError)(What)
					 : (this->*OnPrecError)(What, Cursor.Token->Symbol);
		}
		switch (uint8_t(Cursor.Token))
		{
			case SToken::Digit():
			{
				return UpdateSpaceTrailing(Target, Digit(Target));
			}
			case SToken(u8"\""):
			{
				return UpdateSpaceTrailing(Target, Quote(Target));
			}
			case SToken(u8"'"):
			{
				// CharLit := ''' Printable ''' !''' | ''' CharEsc '''
				return UpdateSpaceTrailing(Target, CharLit());
			}
			case SToken::Alpha(): // Ident.
			case SToken(u8"("):   // QualIdent or Paren.
			case SToken(u8"at"):
			case SToken(
				u8"of"): // Infix operator tokens that are allowed as identifiers.
			case SToken(u8"to"):
			case SToken(u8"next"):
			case SToken(u8"over"):
			case SToken(u8"when"):
			case SToken(u8"while"):
			case SToken(u8"and"):
			case SToken(u8"or"):
			{
				return UpdateSpaceTrailing(
					Target, QualIdent(What, Target, /*AllowParenthesis*/ true));
			}
			case SToken(u8"@"):
			{
				return Attribute(What, Prec, Target);
			}
			case SToken(u8"<"):
			{
				return UpdateSpaceTrailing(Target, Markup());
			}
			case SToken(u8"/"):
			{
				return UpdateSpaceTrailing(Target, Path(Target));
			}
			case SToken(u8":"):
			case SToken(u8"in"):
			{
				return In(Target);
			}
			case SToken(u8"var"):
			case SToken(u8"set"):
			case SToken(u8"ref"):
			case SToken(u8"alias"):
			case SToken(u8"live"):
			{
				return VarKeyword(Target);
			}
			case SToken(u8".."):
			case SToken(u8"not"):
			{
				return Not(Target);
			}
			case SToken(u8"&"):
			{
				return Ampersand(Target);
			}
			case SToken(u8"^"):
			case SToken(u8"?"):
			case SToken(u8"+"):
			case SToken(u8"-"):
			case SToken(u8"*"):
			{
				return Arithmetic(Target);
			}
			case SToken(u8"["):
			{
				return LeftBracket(Target);
			}
			case SToken(u8"if"):
			{
				return If(Target);
			}
			case SToken(u8"return"):
			case SToken(u8"yield"):
			case SToken(u8"break"):
			case SToken(u8"continue"):
			{
				return ControlFlowKeyword(Target);
			}
			case SToken(u8"!"):
			{
				return S62();
			}
			default:
			{ // Static analysis warnings without default
				break;
			}
		}
		ULANG_UNREACHABLE(); // Should never occur due to structure of the
							 // precedence table.
	}

	ResultOf<SNothing> PostfixBinaryOperator(const SToken PostfixToken,
		SExpr& Target)
	{
		// Mul     := Prefix  { Space ('*' | '/' | '&'       ) Scan  Prefix  }
		// Add     := Mul     { Space ('+' | '-'             ) Scan  Mul     }
		// To      := Add     [ Space ("to" Key | ".." | "->") Scan  To      ]
		// Choose  := To      [ Space ('|'                   ) Scan  Choose  ]
		// Greater := Choose  [ Space ('>'  | ">="           ) Scan  Greater ]
		// Less    := Greater [ Space ('<'  | "<="           ) Scan  &(Choose Space
		// !'>' !'>=') Less] NotEq   := Less    { Space ('<>'                  )
		// Scan  Choose  } Eq      := NotEq   { Space ('='                   ) Scan
		// NotEq   } And     := Not     { Space ("and" Key             ) Scan  And }
		// Or      := And     { Space ("or"  Key             ) Scan  Or      }
		Target.MarkupTag = u8"";
		EatToken();
		CaptureType Leading;
		ULANG_GRAMMAR_RUN(Scan(Leading));
		return WhenExpr(
			PostfixToken->Symbol, PostfixToken->PostfixRightPrec(),
			PostfixToken->PostfixRightPrec(), &Target, Leading,
			[&](SExpr& Right) -> ResultOf<SNothing> {
				return UpdateFrom(Target, Right.Trailing,
					Gen.InfixToken(SnipFinished(Target.Start, Right),
						PostfixToken->PostfixMode,
						ApplyTrailing(Target),
						PostfixToken->Symbol, *Right));
			},
			PostfixToken->PostfixAllowMask);
	}

	ResultOf<SNothing> PostfixRef(const SToken PostfixToken, SExpr& Target)
	{
		// Call    := Base {Space Postfix}
		// Postfix := .. | ('^' | '?' | "ref" | ..)
		Target.MarkupTag = u8"";
		EatToken();
		return UpdateSpaceTrailing(
			Target, Gen.PostfixToken(Snip(Target.Start), PostfixToken->PostfixMode,
						ApplyTrailing(Target), PostfixToken->Symbol));
	}

	ResultOf<SNothing> PostfixLeftBracket(const SCursor& PostfixStart,
		SExpr& Target)
	{
		// Call    := Base {Space Postfix}
		// Postfix := .. | (.. | '[' List ']' ..)
		Target.MarkupTag = u8"";
		EatToken();
		ULANG_GRAMMAR_LET(Block0, List(u8"]", &CParser::S83, Cursor, CaptureType(),
									  EPunctuation::Brackets, Cursor));
		ULANG_GRAMMAR_RUN(RequireClose(Target.Start, u8"[", u8"]", &CParser::S83));
		Block0.BlockSnippet = Snip(PostfixStart);
		return UpdateSpaceTrailing(Target,
			Gen.Call(Snip(Target.Start), EMode::Closed,
				ApplyTrailing(Target), Block0));
	}

	ResultOf<SNothing> PostfixAttribute(const SToken PostfixToken,
		SExpr& Target)
	{
		// Expr := Fun {'@' Space Call} StopExpr | ..
		EatToken();
		ULANG_GRAMMAR_LET(Leading, Space());
		return WhenExpr(PostfixToken->Symbol, EPrec::Call, EPrec::Call, &Target,
			Leading, [&](SExpr& Right) -> ResultOf<SNothing> {
				return UpdateFrom(Target, Right.Trailing,
					Gen.PostfixAttribute(
						SnipFinished(Target.Start, Right),
						ApplyTrailing(Target), *Right));
			});
	}

	ResultOf<SNothing> PostfixAtOf(const SCursor& PostfixStart,
		const SToken PostfixToken, SExpr& Target)
	{
		// Postfix := .. | ("at"|"of") Key (KeyBlock | Fun)
		Target.MarkupTag = u8"";
		EatToken();
		ULANG_GRAMMAR_LET(PunctuationLeading, Space());
		ULANG_GRAMMAR_LET(Right,
			KeyBlock(EPrec::Fun, Target, PostfixStart, CaptureType(),
				PostfixToken->Symbol, PunctuationLeading));
		return UpdateFrom(Target, Right.BlockTrailing,
			Gen.Call(SnipFinished(Target.Start, Right),
				PostfixToken->PostfixMode, ApplyTrailing(Target),
				Right));
	}

	ResultOf<SNothing> PostfixArrow(const SToken PostfixToken, SExpr& Target)
	{
		// Def  := .. { .. | Space ":=" Space (BraceInd | Def) | ..} | ..
		// Fun  := Def {Space ("=>" Space | "next" Key) (BraceInd | Fun) } StopFun
		EatToken();
		ULANG_GRAMMAR_LET(Right,
			BraceInd(PostfixToken->Symbol,
				PostfixToken->PostfixRightPrec(), Target));
		return UpdateFrom(Target, Right.BlockTrailing,
			Gen.InfixBlock(SnipFinished(Target.Start, Right),
				ApplyTrailing(Target),
				PostfixToken->Symbol, Right));
	}

	ResultOf<SNothing> PostfixDotBlock(CaptureType& TokenLeading,
		const SToken PostfixToken, SExpr& Target)
	{
		// Postfix := .. | (.. | ScanKey '.' QualIdent)
		Target.MarkupTag = u8"";
		EatToken();
		Gen.CaptureAppend(Target.Trailing.TrailingCapture, TokenLeading);
		ULANG_GRAMMAR_LET(Id, QualIdent(u8".", Target, false));
		return UpdateSpaceTrailing(
			Target,
			Gen.InfixToken(Snip(Target.Start), PostfixToken->PostfixMode,
				ApplyTrailing(Target), PostfixToken->Symbol, Id));
	}

	ResultOf<SNothing> PostfixIs(const SCursor& PostfixStart,
		CaptureType& TokenLeading, SExpr& Target)
	{
		// Def := .. (.. | ScanKey "is" Key (KeyBlock | Def) | ..)
		EatToken();
		ULANG_GRAMMAR_LET(PunctuationLeading, Space());
		ULANG_GRAMMAR_LET(Right,
			KeyBlock(EPrec::Def, Target, PostfixStart, TokenLeading,
				u8"is", PunctuationLeading));
		return UpdateFrom(Target, Right.BlockTrailing,
			Gen.InfixBlock(SnipFinished(Target.Start, Right),
				ApplyTrailing(Target), u8"is", Right));
	}

	ResultOf<SNothing> PostfixWhere(const SCursor& PostfixStart,
		CaptureType& TokenLeading,
		const SToken PostfixToken, SExpr& Target)
	{
		// Def := .. (.. | Space "where" Key (KeyBlock | Defs) | ..)
		// Fun := Def { Space ('over' | 'upon' | 'while') Key (KeyBlock | Defs) |
		// ..) StopFun
		EatToken();
		ULANG_GRAMMAR_LET(Right, KeyBlockDefs(Target, PostfixStart, TokenLeading,
									 PostfixToken->Symbol));
		return UpdateFrom(Target, Right.BlockTrailing,
			Gen.InfixBlock(SnipFinished(Target.Start, Right),
				ApplyTrailing(Target),
				PostfixToken->Symbol, Right));
	}

	ResultOf<SNothing> PostfixMacroInvoke(const SCursor& PostfixStart,
		CaptureType& TokenLeading,
		const SToken PostfixToken, EPrec Prec,
		SExpr& Target)
	{
		// Invoke := [Specs] (Paren [Specs] (Block | Do) | Block [[Specs] Do])
		// (Until | !Until) Def    := (Or | ..) {&In Def | ..}
		SInvoke InvokeTarget{
			u8"macro invocation", Target,
			Target.Start, SToken::None(),
			STokenSet{u8"do"},
			STokenSet{u8"until", u8"catch"}
        };
		ULANG_GRAMMAR_RUN(Invoke(InvokeTarget, PostfixStart, TokenLeading));
		if (Cursor.Pos == PostfixStart.Pos && InPrefixes.Has(PostfixToken))
		{
			if (Prec > EPrec::Def)
			{
				Cursor = PostfixStart; // backtrack NewLine's ScanToken
				return Target.OnFinish(*this);
			}
			// Parse Def and TGenerate a tokenless definition of Target.
			ULANG_GRAMMAR_RUN(
				WhenExpr(PostfixToken->Symbol, EPrec::Def, EPrec::Def, &Target,
					CaptureType(), [&](SExpr& InExpr) {
						auto InBlock = SingletonBlock(InExpr.Start, InExpr);
						return UpdateFrom(
							Target, InBlock.BlockTrailing,
							Gen.InfixBlock(SnipFinished(Target.Start, InExpr),
								ApplyTrailing(Target), u8"", InBlock));
					}));
		}
		return SNothing{};
	}

	ResultOf<SNothing> Postfix(
		SText /*What*/, EPrec Prec, SExpr& Target,
		ErrorType (CParser::* /*OnTokenError*/)(SText) = &CParser::S71,
		ErrorType (CParser::* /*OnPrecError*/)(SText, SText) = &CParser::S60)
	{
		while (!Target.Finished)
		{
			SCursor PostfixStart = Cursor;
			CaptureType TokenLeading = {};
			SToken PostfixToken = Cursor.Token;
		token_leading_loop:
			ULANG_ASSERT(CheckToken());
			if (!(Prec <= PostfixToken->PostfixTokenPrec || (Target.MarkupStart && MarkupPostfixes.Has(PostfixToken))))
			{
				Cursor = PostfixStart; // backtrack NewLine's ScanToken
				return Target.OnFinish(*this);
			}
			if (!Target.AllowPostfixes.Has(
					Cursor.Token)) // Immediate error disallowing e.g. a<=b>c per
								   // grammar.
				return S61(PostfixToken->Symbol);
			switch (uint8_t(Cursor.Token))
			{
				case SToken(u8"&"):
				{
					if (Cursor[1] == '&')
						return S62();
					ULANG_GRAMMAR_RUN(PostfixBinaryOperator(PostfixToken, Target));
					continue;
				}
				case SToken(u8"|"):
				{
					if (Cursor[1] == '|')
						return S62();
					ULANG_GRAMMAR_RUN(PostfixBinaryOperator(PostfixToken, Target));
					continue;
				}
				case SToken(u8">"):
				{
					if (Target.MarkupStart)
						goto markup_postfix;
					ULANG_GRAMMAR_RUN(PostfixBinaryOperator(PostfixToken, Target));
					continue;
				}
				case SToken(u8"*"):
				case SToken(u8"/"):
				case SToken(u8"+"):
				case SToken(u8"-"):
				case SToken(u8"to"):
				case SToken(u8".."):
				case SToken(u8"->"):
				case SToken(u8">="):
				case SToken(u8"<="):
				case SToken(u8"<>"):
				case SToken(u8"="):
				case SToken(u8"and"):
				case SToken(u8"or"):
				{
					ULANG_GRAMMAR_RUN(PostfixBinaryOperator(PostfixToken, Target));
					continue;
				}
				case SToken(u8"^"):
				case SToken(u8"?"):
				case SToken(u8"ref"):
				{
					ULANG_GRAMMAR_RUN(PostfixRef(PostfixToken, Target));
					continue;
				}
				case SToken(u8"["):
				{
					ULANG_GRAMMAR_RUN(PostfixLeftBracket(PostfixStart, Target));
					continue;
				}
				case SToken(u8"@"):
				{
					ULANG_GRAMMAR_RUN(PostfixAttribute(PostfixToken, Target));
					continue;
				}
				case SToken(u8"at"):
				case SToken(u8"of"):
				{
					ULANG_GRAMMAR_RUN(PostfixAtOf(PostfixStart, PostfixToken, Target));
					continue;
				}
				case SToken(u8"=>"):
				case SToken(u8":="):
				case SToken(u8"next"):
				{
					ULANG_GRAMMAR_RUN(PostfixArrow(PostfixToken, Target));
					continue;
				}
				case SToken(u8"."):
				{
					if (!IsSpace(Cursor[1]) /*&& !IsEnding(Cursor[1])*/)
					{
						ULANG_GRAMMAR_RUN(
							PostfixDotBlock(TokenLeading, PostfixToken, Target));
						continue;
					}
					[[fallthrough]]; // Else it's a macro invocation handled below.
				}
				case SToken(u8"{"):
				case SToken(u8":"):
				case SToken(u8"<"):
				case SToken(u8"("):
				case SToken(u8"in"):
				case SToken(u8"with"):
				case SToken(u8":>"):
				case SToken(u8";"):
				case SToken(u8","):
				markup_postfix:
				{
					ULANG_GRAMMAR_RUN(PostfixMacroInvoke(PostfixStart, TokenLeading,
						PostfixToken, Prec, Target));
					continue;
				}
				case SToken(u8"is"):
				{
					ULANG_GRAMMAR_RUN(PostfixIs(PostfixStart, TokenLeading, Target));
					continue;
				}
				case SToken(u8"over"):
				case SToken(u8"when"):
				case SToken(u8"where"):
				case SToken(u8"while"):
				{
					ULANG_GRAMMAR_RUN(
						PostfixWhere(PostfixStart, TokenLeading, PostfixToken, Target));
					continue;
				}
				case SToken::NewLine():
				{
					ULANG_GRAMMAR_SET(
						PostfixToken,
						ScanKey(TokenLeading, STokenSet{u8"is", u8"with", u8"{", u8">",
												  u8":>", u8".", u8",", u8";"}));
					goto token_leading_loop;
				}
				case SToken(u8"=="):
				{
					return S65();
				}
				case SToken(u8"+="):
				case SToken(u8"-="):
				case SToken(u8"*="):
				case SToken(u8"/="):
				{
					return S66(PostfixToken->Symbol);
				}
				default:
				{
					ULANG_UNREACHABLE(); // Should be unreachable due to precedence.
				}
			}
		}
		return SNothing{};
	}
	ResultOf<SExpr*> FinishExpr(SToken Token, EPrec FinishPrec,
		SExpr& SourceExpr)
	{
		// Preemptively finish and TGenerate syntax for all expressions tighter than
		// FinishPrec, producing an error if there is no expression at or looser
		// than FinishPrec.
		ULANG_ASSERT(FinishPrec >= EPrec::Def); // Vital because EPrec.Def and looser
												// don't handle preemptive finish.
		for (auto Expr = &SourceExpr; Expr; Expr = Expr->OuterExpr)
		{
			if (Expr->FinishPrec <= FinishPrec)
				if (Token == SToken::None() || Expr->AllowPostfixes.Has(Token))
					return Expr;
			Expr->ExprStop = true;
			if (!Expr->Finished)
				ULANG_GRAMMAR_RUN(Expr->OnFinish(*this));
		}
		return nullptr;
	}
	template <class CallableType>
	struct SWhenExpr : SExpr
	{
		using ResultType = decltype((*(CallableType*)nullptr)(*(SExpr*)nullptr));
		CallableType _Func;
		ResultType Result;
		SWhenExpr(EPrec FinishPrec0, SExpr* OuterExpr0, STokenSet PostfixAllow0,
			const SCursor& Start0, const CaptureType& ExprLeading0,
			const CallableType& Callable0, SExpr* QualIdentTarget0 = nullptr)
			: SExpr(FinishPrec0, Start0, OuterExpr0, PostfixAllow0,
				QualIdentTarget0)
			, _Func(Callable0)
		{
			this->ExprLeading = ExprLeading0;
		}
		ResultOf<SNothing> Parse(CParser& Parser, SText What, EPrec ParsePrec,
			ErrorType (CParser::*OnTokenError)(SText),
			ErrorType (CParser::*OnPrecError)(SText, SText))
		{
			TScopedGuard ExprDepthGuard(Parser.ExprDepth, Parser.ExprDepth + 1);
			if (Parser.ExprDepth > VERSE_MAX_EXPR_DEPTH)
				return Parser.S99();
			ULANG_GRAMMAR_RUN(
				Parser.Base(What, ParsePrec, *this, OnTokenError, OnPrecError));
			ULANG_GRAMMAR_RUN(
				Parser.Postfix(What, ParsePrec, *this, OnTokenError, OnPrecError));
			ULANG_ASSERT(this->Finished);
			return SNothing{};
		}
		ResultOf<SNothing> OnFinish(CParser& Parser) override
		{
			ULANG_GRAMMAR_RUN(SExpr::OnFinish(Parser));
			this->ExprSyntax = Parser.Gen.Leading(this->ExprLeading, **this);
			ULANG_GRAMMAR_SET(Result, _Func(*this));
			ULANG_ASSERT(!this->Trailing);
			return SNothing{};
		}
	};
	template <class CallableType>
	SWhenExpr(EPrec, SExpr*, STokenSet, const SCursor&, const CaptureType&,
		const CallableType&, SExpr* e = nullptr, bool b = false)
		-> SWhenExpr<CallableType>;
	template <class CallableType>
	typename SWhenExpr<CallableType>::ResultType
	WhenExpr(SText What, EPrec ParsePrec, EPrec FinishPrec, SExpr* OuterExpr,
		const CaptureType& ExprLeading, const CallableType& Func,
		STokenSet AllowPostfixes = AllTokens,
		ErrorType (CParser::*OnTokenError)(SText) = &CParser::S71,
		SExpr* QualIdentTarget0 = nullptr)
	{
		// Start parsing an expression which may be finished preemptively by
		// FinishExpr.
		auto Target = SWhenExpr(FinishPrec, OuterExpr, AllowPostfixes, Cursor,
			ExprLeading, Func, QualIdentTarget0);
		ULANG_GRAMMAR_RUN(
			Target.Parse(*this, What, ParsePrec, OnTokenError, &CParser::S60));
		return Target.Result;
	}
	ResultOf<SyntaxType> MarkupExpr(SExpr* OuterMarkup, SCursor MarkupStart)
	{
		CaptureType Leading;
		ULANG_GRAMMAR_RUN(Scan(Leading));
		if (Cursor[0] == '/')
			return S42();
		auto Expr = SWhenExpr(EPrec::Call, nullptr, AllTokens, Cursor, Leading,
			[&](SExpr& Expr) -> ResultOf<SNothing> {
				Expr.Trailing = STrailing{};
				return SNothing{};
			});
		Expr.MarkupStart = MarkupStart;
		Expr.OuterMarkup = OuterMarkup;
		ULANG_GRAMMAR_RUN(Expr.Parse(*this, u8"markup", EPrec::Call, &CParser::S74,
			&CParser::S64));
		if (!Expr.MarkupFinished)
			return S41();
		return *Expr;
	}

	// Separated expressions.
	ResultOf<SBlockInternal> Commas(SText What, EPrec Prec, SCursor Start,
		CaptureType& Leading,
		ErrorType (CParser::*OnTokenError)(SText))
	{
		// Commas := Expr {',' Scan Expr}
		SBlockInternal Block0;
		for (;;)
		{
			bool More = false;
			ULANG_GRAMMAR_RUN(WhenExpr(
				What, Prec, Prec, nullptr, Leading,
				[&](SExpr& Expr) -> ResultOf<SNothing> {
					More = Eat(u8",");
					if (More)
						ApplyTrailing(Expr, true);
					else
						Block0.BlockSnippet = Snip(Start, *Expr.Trailing.TrailingStart),
						Block0.BlockTrailing.MoveFrom(Expr.Trailing);
					Leading = CaptureType();
					Gen.SyntaxesAppend(Block0.Elements, *Expr);
					return SNothing{};
				},
				AllTokens, OnTokenError));
			if (!More)
				return Block0;
			Block0.Form = EForm::Commas;
			ULANG_GRAMMAR_RUN(Scan(Leading));
		}
	}
	ResultOf<SBlockInternal> List(SText What,
		ErrorType (CParser::*OnTokenError)(SText),
		SCursor BlockStart,
		const CaptureType& PunctuationLeading,
		EPunctuation Punctuation, SCursor CommasStart,
		const CaptureType& Leading = CaptureType())
	{
		// Separator := (';'|Ending) Scan
		// List      := push; set LinePrefix=""; Scan [Commas {Separator Commas}
		// [Separator]]; pop
		auto SavedContext = Context;
		bool Some = false;
		Context.LinePrefix = false;
		auto ListBlock = SBlockInternal{};
		ListBlock.Form = EForm::List;
		ListBlock.PunctuationLeading = PunctuationLeading;
		ListBlock.Punctuation = Punctuation;
		ListBlock.ElementsTrailing = Leading;
		ULANG_GRAMMAR_RUN(Scan(ListBlock.ElementsTrailing));
		if (!StopList.Has(Cursor.Token))
			for (;;)
			{
				ULANG_GRAMMAR_LET(CommasBlock,
					Commas(What, EPrec::Expr, CommasStart,
						ListBlock.ElementsTrailing, OnTokenError));
				ApplyTrailing(CommasBlock, Cursor);
				CommasBlock.BlockSnippet = Snip(CommasStart);
				bool More = false;
				if (Cursor.Token == SToken(u8";") || Ending())
				{

					// Attribute Commas-STrailing [';'] Space &NewLine to CommasBlock,
					// following Scan to ListBlock.
					auto SemicolonStart = Cursor;
					if (Eat(u8";"))
						Gen.Semicolon(CommasBlock.ElementsTrailing, Snip(SemicolonStart));
					ULANG_GRAMMAR_LET(SemicolonTrailing, Space());
					Gen.CaptureAppend(CommasBlock.ElementsTrailing, SemicolonTrailing);

					// Start parsing next list element.
					CommasBlock.BlockSnippet = Snip(CommasStart);
					CommasStart = Cursor;
					ULANG_GRAMMAR_RUN(Scan(ListBlock.ElementsTrailing));
					More = !StopList.Has(Cursor.Token);
				}
				if (More || Some)
				{
					// Multiple Semicolon or NewLine separated elements.
					Some = true;
					ULANG_GRAMMAR_LET(CommasSyntax, Gen.Parenthesis(CommasBlock));
					Gen.SyntaxesAppend(ListBlock.Elements, CommasSyntax);
				}
				else
				{
					// Single Commas SBlock.
					Gen.CaptureAppend(CommasBlock.ElementsTrailing,
						ListBlock.ElementsTrailing);
					ListBlock.Form = CommasBlock.Form;
					ListBlock.Elements = CommasBlock.Elements;
					ListBlock.ElementsTrailing = CommasBlock.ElementsTrailing;
				}
				if (!More)
					break;
			}
		if (StopList.Has(Cursor.Token))
		{
			Context = SavedContext;
			ListBlock.BlockSnippet = Snip(BlockStart);
			return ListBlock;
		}
		return S77();
	}
	ResultOf<SyntaxType> File()
	{
		// File := [0oEF 0oBB 0oBF] set Nest=true; set BlockInd=""; set LineInd="";
		// List Scan end
		if (uint8_t(Cursor[0]) == 0xEF)
		{
			if (uint8_t(Cursor[1]) == 0xBB && uint8_t(Cursor[2]) == 0xBF)
				Next(3);
			else
				return S01();
		}
		ULANG_GRAMMAR_LET(Block0, List(u8"", &CParser::S70, Cursor, CaptureType(),
									  EPunctuation::None, Cursor));
		return Gen.File(Block0);
	}
	ResultOf<SyntaxType> CheckResult(const ResultOf<SyntaxType>& Result)
	{
		ULANG_GRAMMAR_LET(Syntax, Result);
		if (Cursor[0] != 0)
			return S70(u8"");
		if (uint64_t(Cursor.Pos - InputString) != InputLength)
			return S01();
		return Syntax;
	}

	// Friends.
	template <class FriendGeneratorType>
	friend TResult<typename FriendGeneratorType::SyntaxType,
		typename FriendGeneratorType::ErrorType>
	File(FriendGeneratorType& Gen, uint64_t n, const char8_t* s, uint64_t Line);
};

//--------------------------------------------------------------------------------------------------------------------------------------------------------------
// Default Generator Framework inheriting a user generator.

template <class GeneratorType>
struct TGenerate : GeneratorType
{
	using SyntaxType = typename GeneratorType::SyntaxType;
	using SyntaxesType = typename GeneratorType::SyntaxesType;
	using ErrorType = typename GeneratorType::ErrorType;
	using CaptureType = typename GeneratorType::CaptureType;
	using SBlockInternal = SBlock<SyntaxesType, CaptureType>;
	template <class ResultValueType>
	using ResultOf = TResult<ResultValueType, ErrorType>;

	// Passthrough constructor.
	template <class... ArgTypes>
	TGenerate(const ArgTypes&... Args)
		: GeneratorType(Args...) {}

	// Default translators from concrete syntax callbacks to abstract syntax
	// callbacks.
	TResult<SyntaxType, ErrorType>
	Units(const SSnippet& Snippet, const SyntaxType& Num, SText Units) const
	{
		ULANG_GRAMMAR_LET(UnitsIdent,
			this->Ident(SSnippet{}, u8"units'", Units, u8"'"));
		SyntaxesType Parameters;
		this->SyntaxesAppend(Parameters, Num);
		return this->Call(Snippet, EMode::Open, UnitsIdent,
			SBlockInternal{Snippet, Parameters});
	}
	ResultOf<SyntaxType> Parenthesis(const SBlockInternal& Block) const
	{
		if (this->SyntaxesLength(Block.Elements) != 1)
		{
			ULANG_GRAMMAR_LET(Macro, this->Native(SSnippet{}, u8"array"));
			return this->Invoke(Block.BlockSnippet, Macro, Block, nullptr, nullptr);
		}
		else
			return this->SyntaxesElement(Block.Elements, 0);
	}
	ResultOf<SyntaxType> StringInterpolate(const SSnippet& Snippet, EPlace Place,
		bool /*Brace*/,
		const SBlockInternal& Block) const
	{
		ULANG_ASSERT(Place == EPlace::String || Place == EPlace::Content);
		ULANG_GRAMMAR_LET(FunctionSyntax,
			this->Native(SSnippet{}, Place == EPlace::String
										 ? u8"ToString"
										 : u8"ToMarkup"));
		return this->Call(Snippet, EMode::Open, FunctionSyntax, Block);
	}
	ResultOf<SyntaxType> String(const SSnippet& Snippet,
		const SyntaxesType& Splices) const
	{
		if (this->SyntaxesLength(Splices) == 1)
			return this->SyntaxesElement(Splices, 0);
		if (this->SyntaxesLength(Splices) == 0)
			return this->Parenthesis(SBlockInternal{});
		ULANG_GRAMMAR_LET(FunctionSyntax,
			this->Native(SSnippet{}, u8"Concatenate"));
		return this->Call(Snippet, EMode::Open, FunctionSyntax,
			SBlockInternal{SSnippet{}, Splices, EForm::Commas});
	}
	ResultOf<SyntaxType> Content(const SSnippet& Snippet,
		const SyntaxesType& Splices) const
	{
		return String(Snippet, Splices);
	}
	ResultOf<SyntaxType> Contents(const SSnippet& Snippet,
		const CaptureType& /*Leading*/,
		const SyntaxesType& Splices) const
	{
		ULANG_GRAMMAR_LET(Macro, this->Native(SSnippet{}, u8"array"));
		return this->Invoke(Snippet, Macro, SBlockInternal{SSnippet{}, Splices},
			nullptr, nullptr);
	}
	ResultOf<SyntaxType>
	InvokeMarkup(const SSnippet& Snippet, SText /*StartToken*/,
		const CaptureType& /*Leading*/, const SyntaxType& Macro,
		SBlockInternal* Clause, SBlockInternal* DoClause,
		const CaptureType& /*TokenLeading*/,
		const CaptureType& /*PreContent*/, const SyntaxType& Content,
		const CaptureType& /*PostContent*/) const
	{
		ULANG_GRAMMAR_LET(DefineMacro, this->Native(SSnippet{}, u8"operator':='"));
		ULANG_GRAMMAR_LET(ContentIdent,
			this->Ident(SSnippet{}, u8"Content", u8"", u8""));
		SBlockInternal DefineClause;
		this->SyntaxesAppend(DefineClause.Elements, ContentIdent);
		SBlockInternal DefineDoClause;
		this->SyntaxesAppend(DefineDoClause.Elements, Content);
		ULANG_GRAMMAR_LET(ContentSyntax,
			this->Invoke(SSnippet{}, DefineMacro, DefineClause,
				&DefineDoClause, nullptr));
		auto LastClause = !Clause  ? SBlockInternal{}
						: DoClause ? *DoClause
								   : *Clause;
		this->SyntaxesAppend(LastClause.Elements, ContentSyntax);
		return this->Invoke(Snippet, Macro, !DoClause ? LastClause : *Clause,
			DoClause ? &LastClause : nullptr, nullptr);
	}
	ResultOf<SyntaxType>
	PrefixToken(const SSnippet& Snippet, EMode Mode, SText Symbol,
		const SBlockInternal& Block, bool Lift,
		const SyntaxesType& /*VarAttributes*/ = {}) const
	{
		if (Symbol == u8"in")
			Symbol = u8":";
		if (Lift)
			return this->Err(Snippet, "S98", "Feature is not currently supported");
		ULANG_GRAMMAR_LET(Macro, /*IsAlnum(Symbol[0])?
				 this->Ident(SSnippet{},Symbol,u8"",u8""):*/
			this->Ident(SSnippet{}, u8"prefix'", Symbol, u8"'"));
		if (Mode == EMode::Open || Mode == EMode::Closed)
			return this->Call(Snippet, Mode, Macro, Block);
		else if (Mode == EMode::With)
			return this->Invoke(Snippet, Macro, Block, nullptr, nullptr);
		else
			ULANG_UNREACHABLE();
	}
	ResultOf<SyntaxType> PrefixBrackets(const SSnippet& Snippet,
		const SBlockInternal& Left,
		const SBlockInternal& Right) const
	{
		if (Right.Punctuation == EPunctuation::Braces)
			return this->Err(Snippet, "S98", "Feature is not currently supported");
		if (this->SyntaxesLength(Left.Elements) == 0)
		{
			ULANG_GRAMMAR_LET(Macro,
				this->Ident(SSnippet{}, u8"prefix'[]'", u8"", u8""));
			return this->Call(Snippet, EMode::Closed, Macro, Right);
		}
		ULANG_GRAMMAR_LET(Macro,
			this->Ident(SSnippet{}, u8"operator'[]'", u8"", u8""));
		SBlockInternal Parameters;
		ULANG_GRAMMAR_LET(LeftSyntax, this->Parenthesis(Left));
		this->SyntaxesAppend(Parameters.Elements, LeftSyntax);
		ULANG_GRAMMAR_LET(RightSyntax, this->Parenthesis(Right));
		this->SyntaxesAppend(Parameters.Elements, RightSyntax);
		Parameters.Form = EForm::Commas;
		return this->Call(Snippet, EMode::Closed, Macro, Parameters);
	}
	ResultOf<SyntaxType> PostfixToken(const SSnippet& Snippet, EMode Mode,
		const SyntaxType& Left,
		SText Symbol) const
	{
		ULANG_GRAMMAR_LET(Macro,
			this->Ident(SSnippet{}, u8"operator'", Symbol, u8"'"));
		SBlockInternal Parameters;
		this->SyntaxesAppend(Parameters.Elements, Left);
		if (Mode == EMode::Open || Mode == EMode::Closed)
			return this->Call(Snippet, Mode, Macro, Parameters);
		else if (Mode == EMode::With)
			return this->Invoke(Snippet, Macro, Parameters, nullptr, nullptr);
		else
			ULANG_UNREACHABLE();
	}
	ResultOf<SyntaxType> InfixToken(const SSnippet& Snippet, EMode Mode,
		const SyntaxType& Left, SText Symbol,
		const SyntaxType& Right) const
	{
		if (Symbol == u8"to")
			Symbol = u8"->";
		ULANG_GRAMMAR_LET(Macro,
			this->Ident(SSnippet{}, u8"operator'", Symbol, u8"'"));
		SBlockInternal Parameters;
		this->SyntaxesAppend(Parameters.Elements, Left);
		this->SyntaxesAppend(Parameters.Elements, Right);
		if (Mode == EMode::Closed || Mode == EMode::Open)
			return Parameters.Form = EForm::Commas,
				   this->Call(Snippet, Mode, Macro, Parameters);
		else if (Mode == EMode::With)
			return this->Invoke(Snippet, Macro, Parameters, nullptr, nullptr);
		else
			ULANG_UNREACHABLE();
	}
	ResultOf<SyntaxType> InfixBlock(const SSnippet& Snippet,
		const SyntaxType& LeftSyntax, SText Symbol,
		const SBlockInternal& Right) const
	{
		if (Symbol == u8"" || Symbol == u8"is" || Symbol == u8"=")
			Symbol = u8":=";
		SBlockInternal LeftBlock;
		this->SyntaxesAppend(LeftBlock.Elements, LeftSyntax);
		ULANG_GRAMMAR_LET(Macro,
			this->Ident(SSnippet{}, u8"operator'", Symbol, u8"'"));
		return this->Invoke(Snippet, Macro, LeftBlock, &Right, nullptr);
	}
	SyntaxType Leading(const CaptureType& /*Capture*/,
		const SyntaxType& Syntax) const
	{
		return Syntax;
	}
	SyntaxType Trailing(const SyntaxType& Syntax,
		const CaptureType& /*Capture*/) const
	{
		return Syntax;
	}
	TResult<SyntaxType, ErrorType> File(const SBlockInternal& Block) const
	{
		return Parenthesis(Block);
	}

	// String callbacks that can contribute to abstract syntax.
	// In all string callbacks, every non-empty Snippet's text span is guaranteed
	// to be inside the parser's input string.
	void Text(CaptureType& Capture, const SSnippet& Snippet, EPlace Place) const
	{
		if (Place == EPlace::Content || Place == EPlace::String)
			this->GeneratorType::Text(Capture, Snippet, Place);
	}
	void NewLine(CaptureType& Capture, const SSnippet& /*Snippet*/,
		EPlace Place) const
	{
		if (Place == EPlace::Content)
		{
			char8_t Char8 = '\n'; // We normalize markup NewLine to \n.
			SSnippet NewSnippet = {};
			NewSnippet.Text = SText{&Char8, &Char8 + 1};
			this->GeneratorType::Text(Capture, NewSnippet, Place);
		}
	}
	void StringBackslash(CaptureType& Capture, const SSnippet& /*Snippet*/,
		EPlace Place, char8_t Backslashed) const
	{
		if (Place == EPlace::Content || Place == EPlace::String)
		{
			// We pass through backslashed control characters as-is.
			char8_t Char8 = Backslashed == 'n' ? '\n'
						  : Backslashed == 'r' ? '\r'
						  : Backslashed == 't' ? '\t'
											   : Backslashed;
			SSnippet NewSnippet = {};
			NewSnippet.Text = SText{&Char8, &Char8 + 1};
			this->GeneratorType::Text(Capture, NewSnippet, Place);
		}
	}

	// Optional string callbacks which don't contribute to abstract syntax.
	void LineCmt(CaptureType& /*Capture*/, const SSnippet& /*Snippet*/,
		EPlace /*Place*/, const CaptureType& /*Comments*/) const {}
	void BlockCmt(CaptureType& /*Capture*/, const SSnippet& /*Snippet*/,
		EPlace /*Place*/, const CaptureType& /*Comments*/) const {}
	void IndCmt(CaptureType& /*Capture*/, const SSnippet& /*Snippet*/,
		EPlace /*Place*/, const CaptureType& /*Comments*/) const {}
	void Indent(CaptureType& /*Capture*/, const SSnippet& /*Snippet*/,
		EPlace /*Place*/) const {}
	void BlankLine(CaptureType& /*Capture*/, const SSnippet& /*Snippet*/,
		EPlace /*Place*/) const {}
	void Semicolon(CaptureType& /*Capture*/,
		const SSnippet& /*Snippet*/) const {}
	void MarkupTrim(CaptureType& Capture) const { Capture = CaptureType(); }
	void MarkupStart(CaptureType& /*Capture*/,
		const SSnippet& /*Snippet*/) const {}
	void MarkupTag(CaptureType& /*Capture*/,
		const SSnippet& /*Snippet*/) const {}
	void MarkupStop(CaptureType& /*Capture*/,
		const SSnippet& /*Snippet*/) const {}
	void LinePrefix(CaptureType& /*Capture*/,
		const SSnippet& /*Snippet*/) const {}
};

//--------------------------------------------------------------------------------------------------------------------------------------------------------------
// Public parsing interface.

template <class GeneratorType>
TResult<typename GeneratorType::SyntaxType, typename GeneratorType::ErrorType>
File(GeneratorType& Gen, uint64_t n, const char8_t* s, uint64_t Line = 1)
{
	auto Parser = CParser(Gen, n, s, Line);
	return Parser.CheckResult(Parser.File());
}

} // namespace Grammar
} // namespace Verse
