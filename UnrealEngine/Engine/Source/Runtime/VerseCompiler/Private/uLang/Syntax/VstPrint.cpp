// Copyright Epic Games, Inc. All Rights Reserved.
#include "uLang/Common/Common.h"
#include "uLang/Common/Text/Unicode.h"
#include "uLang/Common/Text/StringUtils.h"
#include "uLang/Common/Misc/EnumUtils.h"
#include "uLang/Common/Text/FilePathUtils.h"
#include "uLang/Common/Text/UTF8StringBuilder.h"
#include "uLang/Common/Text/VerseStringEscaping.h"
#include "uLang/Syntax/VstNode.h"
#include "uLang/Syntax/vsyntax_types.h"  // Alter for new parser?
#include "uLang/Semantics/Expression.h"
#include "uLang/Diagnostics/Glitch.h"
#include "uLang/SourceProject/IndexedSourceText.h"

/*
 * Based on the pretty printer in tlang with the goal to produce compilable source code. 
 * Minimal attempts to look like the original source code and drops all comments.
 */

namespace Verse
{
    class CVstPrinter
    {
    public:
        CVstPrinter(uLang::CUTF8StringBuilder& out_string, const int32_t InitialIndent = 0)
            : os(out_string)
            , indent_amount(InitialIndent)
            , Format(EFormat::Newline)
            , bNewlinesOk(true)
        {
            do_indent();
        }

        // Ensure final newline
        ~CVstPrinter()
        {
            if (os.LastByte() != '\n')
            {
                os.Append('\n');
            }
        }

        void DoNewline(bool NeedSeparator = true)
        {
            if (!bNewlinesOk)
            {
             if (NeedSeparator && Format == EFormat::Newline)
                {
                    os.Append(';');
                }
             if (os.LastByte() != ' ')
             {
                 os.Append(' ');
             }
            }
            else
            {
                NumberOfNewlines++;
                os.Append('\n');
                do_indent();
            }
        }

        enum class EFormat {
            Comma,          // comma separated 
            Newline,        // newline between items
            AtAttributes,   // newline between items, macron has '@' in front of them and no line breaks for its parts.
            Attributes      // no separator, '<' & '>' around macros
        };

        void VisitWith(const Vst::TNodeRef<Vst::Node>& Node, EFormat InFormat)
        {
            EFormat OldFormat = Format;
            Format = InFormat;
            Vst::Node::VisitWith(Node, *this);
            Format = OldFormat;
        }

        void PrintCommaSeparatedChildren(const Vst::Node& parent)
        {
            const int32_t NumChildren = parent.GetChildCount();
            for (int32_t ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
            {
                if (ChildIndex)
                {
                    os.Append(',');
                }
                PrintElementWithFormat(parent.GetChildren()[ChildIndex], EFormat::Comma);
            }
        }

        void PrintAuxAfter(const Vst::TNodePtr<Vst::Clause>& Aux)
        {
            if (!Aux)
            {
                return;
            }
            VisitClauseWithFormat(*Aux, EFormat::Attributes);
        }

        void PrintElementWithFormat(const Vst::TNodeRef<Vst::Node>& InNode, EFormat InFormat)
        {
            EFormat OldFormat = Format;
            Format = InFormat;
            PrintElement(InNode);
            Format = OldFormat;
        }

        void PrintElement(const Vst::TNodeRef<Vst::Node>& InNode)
        {
            const Vst::TNodePtr<Vst::Clause>& Aux = InNode->GetAux();
            bool bPrintAuxAfter = InNode->IsA<Vst::Identifier>() || InNode->IsA<Vst::PrePostCall>();
            bool bPrintAnyAux = Aux.IsValid() && !InNode->IsA<Vst::Mutation>();
            
            if (bPrintAnyAux && !bPrintAuxAfter && Aux)
            {
                int ByteLen = os.ByteLen();
                VisitClauseWithFormat(*Aux, EFormat::AtAttributes);
                if (ByteLen < os.ByteLen())
                {
                    DoNewline();
                }

            }

            VisitWith(InNode, Format);

            if (bPrintAnyAux && bPrintAuxAfter && Aux)
            {
                VisitClauseWithFormat(*Aux, EFormat::Attributes);
            }
        }

        const Vst::TNodeRef<Vst::Node>& StripAnyClause(const Vst::TNodeRef<Vst::Node>& Node)
        {
            ULANG_ASSERT(Node.IsValid());
            if (const Vst::Clause* Clause = Node->AsNullable<Vst::Clause>())
            {
                ULANG_ASSERTF(Clause->GetChildCount() == 1, "expected clause with a single child");
                const Vst::TNodeRef<Vst::Node>& Child = Clause->GetChildren()[0];
                ULANG_ASSERT(Child.IsValid());
                return Child;
            }
            return Node;
        }

        void VisitClause(const Vst::Clause& Clause)
        {
            int Count = CountNonComments(Clause.GetChildren());
            EFormat ClauseFormat =
                Clause.GetForm() == Vst::Clause::EForm::NoSemicolonOrNewline && Count > 1
                ? EFormat::Comma
                : EFormat::Newline;
            VisitClauseWithFormat(Clause, ClauseFormat);
        }

        void VisitClauseWithFormat(const Vst::Clause& Clause, EFormat InFormat)
        {
            EFormat OldFormat = Format;
            Format = InFormat;
            VisitExpressionList(Clause.GetChildren());
            Format = OldFormat;
        }

        void VisitExpressionListWithFormat(const Vst::NodeArray& Expressions, EFormat InFormat)
        {
            EFormat OldFormat = Format;
            Format = InFormat;
            VisitExpressionList(Expressions);
            Format = OldFormat;
        }

        void VisitExpressionList(const Vst::NodeArray& Expressions)
        {
            bool bOldNewlinesOk = bNewlinesOk;
            bool bListNewlineOk = bNewlinesOk && Format != EFormat::Comma;
            bool bExpressionNewlineOk = bListNewlineOk && Format != uLang::Cases<EFormat::AtAttributes, EFormat::Attributes>;

            bNewlinesOk = bListNewlineOk;
            const int32_t NumExpressions = Expressions.Num();
            for (int32_t Idx = 0; Idx < NumExpressions; ++Idx)
            {
                const Vst::TNodeRef<Vst::Node>& Expression = StripAnyClause(Expressions[Idx]);
                if (!Expression->IsA<Vst::Comment>())
                {
                    if (Format == uLang::Cases<EFormat::Newline>)
                    {
                        if (Idx != 0 || bNewlinesOk)
                        {
                            DoNewline();
                        }
                    }
                    else if (Format == EFormat::AtAttributes)
                    {
                        DoNewline();
                        os.Append('@');
                    }
                    else if (Format == EFormat::Attributes)
                    {
                        os.Append('<');
                    }
                    bNewlinesOk = bExpressionNewlineOk;
                    PrintElement(Expression);
                    bNewlinesOk = bListNewlineOk;

                    if (Format == EFormat::Comma && Idx < NumExpressions - 1)
                    {
                        os.Append(',');
                        DoNewline(false);
                    }
                    else if (Format == EFormat::Attributes)
                    {
                        os.Append('>');
                    }
                }
            }
            bNewlinesOk = bOldNewlinesOk;
        }

        void VisitBinaryOp(const Vst::TNodeRef<Vst::Node>& Operand1, const char* OperandCstr, const Vst::TNodeRef<Vst::Node>& Operand2, bool bRhsNewLinesOk)
        {
            // operand operator
            //      operand

            bool bOldNewlinesOk = bNewlinesOk;
            bNewlinesOk = false;
            PrintElement(Operand1);
            bNewlinesOk = bOldNewlinesOk && bRhsNewLinesOk;

            os.Append(OperandCstr);
            indent_amount += 2;
            if (Operand2->GetElementType() == Vst::NodeType::Clause)
            {
                const Vst::TNodeRef<Vst::Clause>& RhsClause = Operand2.As<Vst::Clause>();
                PrintClause(*RhsClause);
            } 
            else
            {
                PrintElementWithFormat(Operand2, EFormat::Newline);
            }
            bNewlinesOk = bOldNewlinesOk;
            indent_amount -= 2;
        }

        //  *******

        void visit(const Vst::Comment& node)
        {
            // Ignore comments
        }

        void visit(const Vst::Project& node)
        {
            for (const Vst::TNodeRef<Vst::Node>& child : node.GetChildren())
            {
                PrintElement(child);
            }
        }

        void visit(const Vst::Package& node)
        {
            for (const Vst::TNodeRef<Vst::Node>& child : node.GetChildren())
            {
                PrintElement(child);
            }
        }

        void visit(const Vst::Module& node)
        {
            for (const Vst::TNodeRef<Vst::Node>& child : node.GetChildren())
            {
                PrintElement(child);
            }
        }

        void visit(const Vst::Snippet& node)
        {
            VisitExpressionList(node.GetChildren());
        }

        void visit(const Vst::PrefixOpLogicalNot& node)
        {
            os.Append("not");
            const Vst::TNodeRef<Vst::Node>& Operand = node.GetInnerNode();
            os.Append(' ');
            Vst::Node::VisitWith(Operand, *this);
        }

        void visit(const Vst::Definition& Node)
        {
            const Vst::TNodeRef<Vst::Node>& LeftOperand = Node.GetOperandLeft();

            const char* OpCStr;
            if (LeftOperand->IsA<Vst::TypeSpec>())
            {
                OpCStr = " = ";
            }
            else
            {
                OpCStr = " := ";
            }
            const Vst::TNodeRef<Vst::Node>& RightOperand = Node.GetOperandRight();
            VisitBinaryOp(LeftOperand, OpCStr, RightOperand, true);
        }

        void visit(const Vst::Assignment& Node)
        {
            using EOp = Vst::Assignment::EOp;

            const char* OpCStr = " UnknownOp";

            switch (Node.GetOperandRight()->GetTag<EOp>())
            {
            case EOp::assign: OpCStr = " = "; break;
            case EOp::addAssign: OpCStr = " += "; break;
            case EOp::subAssign: OpCStr = " -= "; break;
            case EOp::mulAssign: OpCStr = " *= "; break;
            case EOp::divAssign: OpCStr = " /= "; break;
            default: ULANG_ENSUREF(false, "Unknown assignment operator!"); break;
            }

            VisitBinaryOp(Node.GetOperandLeft(), OpCStr, Node.GetOperandRight(), true);
        }

        void visit(const Vst::BinaryOpCompare& node)
        {
            const char* OpCStr = " UnknownOp";

            switch (node.GetOp())
            {
            case Vst::BinaryOpCompare::op::lt:    OpCStr = " < ";  break;
            case Vst::BinaryOpCompare::op::lteq:  OpCStr = " <= "; break;
            case Vst::BinaryOpCompare::op::gt:    OpCStr = " > ";  break;
            case Vst::BinaryOpCompare::op::gteq:  OpCStr = " >= "; break;
            case Vst::BinaryOpCompare::op::eq:    OpCStr = " = ";  break;
            case Vst::BinaryOpCompare::op::noteq: OpCStr = " <> "; break;
            default: ULANG_ENSUREF(false, "Unknown compare operator!"); break;
            }


            VisitBinaryOp(node.GetOperandLeft(), OpCStr, node.GetOperandRight(), false);
        }

        void visit(const Vst::BinaryOpLogicalOr& Node)
        {
            const auto& Children = Node.GetChildren();
            const auto NumChildren = Children.Num();
            if (NumChildren == 2)
            {
                PrintBinaryOp(Vst::NodeType::BinaryOpLogicalOr, "or", Children[0], Children[1]);
            }
            else
            {
                ULANG_ERRORF("BinaryOpLogicalOr must have exactly two Children, got total %s.", NumChildren);
            }
        }

        void visit(const Vst::BinaryOpLogicalAnd& Node)
        {
            const auto& Children = Node.GetChildren();
            const auto NumChildren = Children.Num();
            if (NumChildren == 2)
            {
                PrintBinaryOp(Vst::NodeType::BinaryOpLogicalAnd, "and", Children[0], Children[1]);
            }
            else
            {
                ULANG_ERRORF("BinaryOpLogicalOr must have exactly two Children, got total %s.", NumChildren);
            }
        }

        void PrintBinaryOp(Vst::NodeType NodeType, const CUTF8String& String, const Vst::TNodeRef<Vst::Node>& Left, const Vst::TNodeRef<Vst::Node>& Right)
        {
            const int32_t Op_Precedence = GetOperatorPrecedence(NodeType);

            PrintElementParen(Left, Left->GetPrecedence() <= Op_Precedence);

            os.Append(' ');
            os.Append(String);
            os.Append(' ');

            PrintElementParen(Right, Right->GetPrecedence() <= Op_Precedence);
        }

        void PrintElementParen(const Vst::TNodeRef<Vst::Node>& Node, bool bUseParen = true)
        {
            if (bUseParen) os.Append('(');
            PrintElement(Node);
            if (bUseParen) os.Append(')');
        }

        void visit(const Vst::BinaryOp& Node)
        {
            Vst::NodeType NodeType = Node.GetElementType();
            const int32_t Op_Precedence = GetOperatorPrecedence(NodeType);

            const auto& Children = Node.GetChildren();
            const auto NumChildren = Children.Num();

            if (NumChildren > 1)
            {
                int Ix = 0;

                while (Ix < NumChildren && Children[Ix]->GetElementType() == Vst::NodeType::Operator)
                {
                    os.Append(Children[Ix]->As<Vst::Operator>().GetSourceText());
                    if (Ix > 0) os.Append(' ');
                    ++Ix;
                }

                if (Ix < NumChildren)
                {
                    PrintElementParen(Children[Ix], Children[Ix]->GetPrecedence() <= Op_Precedence);
                    ++Ix;
                }

                while (Ix < NumChildren)
                {
                    while (Ix < NumChildren && Children[Ix]->GetElementType() == Vst::NodeType::Operator)
                    {
                        os.Append(Children[Ix]->As<Vst::Operator>().GetSourceText());
                        ++Ix;
                    }
                    PrintElementParen(Children[Ix], Children[Ix]->GetPrecedence() <= Op_Precedence);
                    ++Ix;
                }
            }
            else
            {
                ULANG_ERRORF("BinaryOp must have more than one child, got %s.", NumChildren);
            }
        }

        void visit(const Vst::BinaryOpRange& Node)
        {
            const auto& Children = Node.GetChildren();
            if (Children.Num() == 2)
            {
                PrintBinaryOp(Vst::NodeType::BinaryOpRange, "..", Children[0], Children[1]);
            }
            else
            {
                ULANG_ERRORF("BinaryOpRange must have exactly two Children.");
            }
        }

        void visit(const Vst::BinaryOpArrow& node)
        {
            const auto& Children = node.GetChildren();
            if (Children.Num() == 2)
            {
                PrintBinaryOp(Vst::NodeType::BinaryOpArrow, "->", Children[0], Children[1]);
            }
            else
            {
                ULANG_ERRORF("BinaryOpArrow must have exactly two children.");
            }
        }

        void visit(const Vst::Where& Node)
        {
            if (Node.GetChildCount() < 1)
            {
                ULANG_ERRORF("Where must have at least one child.");
            }
            PrintElement(Node.GetLhs());
            os.Append(" where");
            Vst::Where::RhsView Rhs = Node.GetRhs();
            if (Rhs.IsEmpty())
            {
                return;
            }
            os.Append(' ');
            auto I = Rhs.begin();
            PrintElement(*I);
            ++I;
            for (auto Last = Rhs.end(); I != Last; ++I)
            {
                os.Append(", ");
                PrintElement(*I);
            }
        }

        void visit(const Vst::Mutation& Node)
        {
            if (Node.GetChildCount() != 1)
            {
                ULANG_ERRORF("Var must have one child.");
            }
            switch (Node._Keyword)
            {
            case Vst::Mutation::EKeyword::Var:
                os.Append("var");
                PrintAuxAfter(Node.GetAux());
                os.Append(' ');
                if (Node._bLive)
                {
                    os.Append("live ");
                }
                break;
            case Vst::Mutation::EKeyword::Set:
                os.Append("set ");
                if (Node._bLive)
                {
                    os.Append("live ");
                }
                break;
            case Vst::Mutation::EKeyword::Live:
                os.Append("live ");
                break;
            default:
                ULANG_UNREACHABLE();
            }
            PrintElement(Node.Child());
        }

        void visit(const Vst::TypeSpec& node)
        {
            if (node.GetChildCount() == 2)
            {
                const auto& Lhs = node.GetLhs();
                const auto& Rhs = node.GetRhs();

                PrintElementParen(Lhs, Lhs->GetPrecedence() <= GetOperatorPrecedence(Vst::NodeType::TypeSpec));
                os.Append(':');

                for (const Vst::TNodeRef<Vst::Node>& CommentNode : node._TypeSpecComments)
                {
                    Vst::Node::VisitWith(CommentNode, *this);
                }
                PrintElementParen(Rhs, Rhs->GetPrecedence() <= GetOperatorPrecedence(Vst::NodeType::TypeSpec));
            }
            else if (node.GetChildCount() == 1)
            {
                os.Append(':');
                const auto& Type = node.GetChildren()[0];
                PrintElement(Type);
            }
            else
            {
                ULANG_ERRORF("TypeSpec must have either one or two children.");
            }
        }

        enum class EVariant { Colon, Braces, Parenthesis, Keyword};

        void visit(const Vst::FlowIf& Node)
        {
            using EOp = Vst::FlowIf::ClauseTag;

            const Vst::NodeArray& NodeChildren = Node.GetChildren();
            const int32_t NumChildren = Node.GetChildCount();

            for (int32_t Ix = 0; Ix < NumChildren; Ix += 1)
            {
                const Vst::TNodeRef<Vst::Clause>& Clause = NodeChildren[Ix].As<Vst::Clause>();

                const char* Keyword = nullptr;
                switch (Clause->GetTag<EOp>())
                {
                case EOp::if_identifier:
                    Keyword = Ix == 0 ? "if" : "else if ";
                    break;
                case EOp::condition:
                    break;
                case EOp::then_body:
                    Keyword = "then ";
                    break;
                case EOp::else_body:
                    Keyword = "else ";
                    break;
                }

                if (Keyword)
                {
                    DoNewline(false);
                    os.Append(Keyword);
                }
                int ByteLen = os.ByteLen();
                if (Clause->GetTag<EOp>() != EOp::if_identifier)
                {
                    os.Append('{');
                }
                indent_amount++;
                VisitClause(*Clause);
                --indent_amount;
                if (Clause->GetTag<EOp>() != EOp::if_identifier)
                {
                    if (ByteLen < os.ByteLen())
                    {
                        DoNewline(false);
                    }
                    os.Append('}');
                }
            }
        }

        void VisitPrePostCall(const Vst::PrePostCall& node, int32_t First, int32_t Last)
        {
            for (int i = First; i <= Last; i += 1)
            {
                const auto& child = node.GetChildren()[i];
                using Op = Verse::Vst::PrePostCall::Op;
                auto thisOp = child->GetTag<Op>();
                if (thisOp == Op::Expression)
                {
                    PrintElement(child);
                }
                else if (thisOp == Op::DotIdentifier)
                {
                    if (i > First)
                    {
                        os.Append('.');
                    }
                    PrintElement(child);
                }
                else if (thisOp == Op::FailCall || thisOp == Op::SureCall)
                {
                    ++indent_amount;
                    os.Append(thisOp == Op::SureCall ? '(' : '[');
                    VisitClause(*child.As<Vst::Clause>());
                    os.Append(thisOp == Op::SureCall ? ')' : ']');
                    --indent_amount;
                }
                else if (thisOp == Op::Pointer)
                {
                    os.Append('^');
                }
                else if (thisOp == Op::Option)
                {
                    os.Append('?');
                }
            }
        }

        void visit(const Vst::PrePostCall& node)
        {
            const int32_t NumChildren = node.GetChildCount();
            bool bOldNewlinesOk = bNewlinesOk;
            bNewlinesOk = false;
            VisitPrePostCall(node, 0, NumChildren - 1);
            bNewlinesOk = bOldNewlinesOk;
        }

        void visit(const Vst::Identifier& node)
        {
            if (node.GetChildCount())
            {
                os.Append('(');
                PrintCommaSeparatedChildren(node);
                os.Append(":)");
            }
            os.Append(node.GetStringValue());
        }
        
        void visit(const Vst::Operator& node)
        {
            os.Append(node.GetStringValue());
        }

        void visit(const Vst::IntLiteral& node)
        {
            os.Append(node.GetStringValue());
        }

        void visit(const Vst::FloatLiteral& node)
        {
            os.Append(node.GetStringValue());
        }

        void visit(const Vst::CharLiteral& node)
        {
            os.Append(node.ToString());
        }

        void visit(const Vst::StringLiteral& node)
        {
            os.Append("\"");
            os.Append(uLang::VerseStringEscaping::EscapeString(node.GetStringValue()));
            os.Append("\"");
        }

        void visit(const Vst::PathLiteral& node)
        {
            os.Append(node.GetStringValue());
        }

        int CountNonComments(const Vst::NodeArray& Nodes)
        {
            int Count = 0;
            const int32_t NumNodes = Nodes.Num();
            for (int32_t Idx = 0; Idx < NumNodes; ++Idx)
            {
                const Vst::TNodeRef<Vst::Node>& Node = StripAnyClause(Nodes[Idx]);
                if (!Node->IsA<Vst::Comment>())
                {
                    Count++;
                }
            }
            return Count;
        }

        // bHideEmptyBraces is used in string interpolation where there is "nothing" betwen the {}.
        //     "This is sometimes used for writing {
        //     } long strings on multiple lines"
        // The parser does not accept
        //     "This is sometimes used for writing {{}} long strings on multiple lines"
        void PrintClause(const Vst::Clause& Clause, bool bHideEmptyBraces = false)
        {
            int Count = CountNonComments(Clause.GetChildren());
            if (Count == 0)
            {
                if (!bHideEmptyBraces)
                {
                    os.Append("{}");
                }
            }
            else
            {
                EFormat ClauseFormat = Clause.GetForm() == Vst::Clause::EForm::NoSemicolonOrNewline && Count > 1
                    ? EFormat::Comma
                    : EFormat::Newline;
                if (ClauseFormat == EFormat::Comma) os.Append('(');
                VisitClauseWithFormat(Clause, ClauseFormat);
                if (ClauseFormat == EFormat::Comma) os.Append(')');
            }
        }

        void visit(const Vst::Interpolant& node)
        {
            ULANG_ERRORF("Unexpected Interpolant node");
        }

        void visit(const Vst::InterpolatedString& node)
        {
            os.Append("\"");
            for (const Vst::TNodeRef<Vst::Node>& Child : node.GetChildren())
            {
                if (const Vst::StringLiteral* StringLiteral = Child->AsNullable<Vst::StringLiteral>())
                {
                    os.Append(uLang::VerseStringEscaping::EscapeString(StringLiteral->GetStringValue()));
                }
                else if (const Vst::Interpolant* Interpolant = Child->AsNullable<Vst::Interpolant>())
                {
                    os.Append("{");
                    PrintClause(Interpolant->GetChildren()[0]->As<Vst::Clause>(), /*bHideEmptyBraces*/true);
                    os.Append("}");
                }
                else
                {
                    ULANG_ERRORF("Unexpected InterpolatedString VST node child %s", GetNodeTypeName(Child->GetElementType()));
                }
            }
            os.Append("\"");
        }

        void visit(const Vst::Lambda& Node)
        {
            const int32_t NumChildren = Node.GetChildCount();
            if (ULANG_ENSUREF(NumChildren >= 2, "Lambda must have at least 2 children"))
            {
                bool bOldNewlinesOk = bNewlinesOk;
                bNewlinesOk = false;
                PrintElement(Node.GetDomain());
                bNewlinesOk = bOldNewlinesOk;
                os.Append(" => ");
                indent_amount += 2;               
                PrintClause(*Node.GetRange());
                indent_amount -= 2;
            }
        }

        void visit(const Vst::Control& node)
        {
            bool bPrintReturnExpression = false;

            switch (node._Keyword)
            {
                case Vst::Control::EKeyword::Return:
                    os.Append("return");
                    bPrintReturnExpression = true;
                    break;
                case Vst::Control::EKeyword::Break:
                    os.Append("break");
                    break;
                case Vst::Control::EKeyword::Yield:
                    os.Append("yield");
                    break;
                case Vst::Control::EKeyword::Continue:
                    os.Append("continue");
                    break;
                default:
                    ULANG_UNREACHABLE();
            }

            if (node.GetChildCount() == 0)
            {
                return;
            }

            if (bPrintReturnExpression)
            {
                const Vst::TNodeRef<Vst::Node>& ReturnExpr = node.GetReturnExpression();
                if (ReturnExpr.IsValid())
                {
                    // Append a space after the `return` token.
                    os.Append(' ');
                }
                bool bOldNewlinesOk = bNewlinesOk;
                bNewlinesOk = false;
                PrintElement(ReturnExpr);
                bNewlinesOk = bOldNewlinesOk;
            }
        }

        void visit(const Vst::Macro& Node)
        {
            ULANG_ENSUREF(Node.GetChildCount() > 1, "Malformed macro");
            const Vst::TNodeRef<Vst::Node>& Name = Node.GetName();
            CUTF8String NameString = "";
            if (Vst::Identifier* Identifier = Name->AsNullable<Vst::Identifier>())
            {
                NameString = Identifier->GetStringValue();
            }

            // A workaround to make asserts and auto qualification work together.
            // If auto qualification is on then the assert processing code wraps all asserts in "assert keep" before calling the semantic analyzer the first time.
            // The "assert keep" are treated as Noop in the semantic analyzer. This is needed since asserts might contain broken code.
            // The VST is then auto qualified, and converted to text where "assert keep" prints as the wrapped assert.
            // The asserts are then processed as usual before the semantic analyzer is called the second time.
            // Note that this means that auto qualification isn't done for code inside asserts.
            if (NameString == "assert keep")
            {
                if (Vst::Clause* Clause = Node.GetChildren()[1]->AsNullable<Vst::Clause>()) 
                {
                    if (Vst::StringLiteral* String = Clause->GetChildren()[0]->AsNullable<Vst::StringLiteral>())
                    {
                        os.Append(String->GetStringValue());
                        return;
                    }
                }
            }

            bool bOldNewlinesOk = bNewlinesOk;
            bool bMacroNewlinesOk = bNewlinesOk || NameString == "module" || NameString == "class" || NameString == "interface";
            bNewlinesOk = bMacroNewlinesOk;

            PrintElement(Node.GetName());

            for (int ChildIndex = 1; ChildIndex < Node.GetChildCount(); ++ChildIndex)
            {
                const Vst::TNodeRef<Vst::Clause>& Child = Node.GetChildren()[ChildIndex].As<Vst::Clause>();
                vsyntax::res_t ChildTag = Child->GetTag<vsyntax::res_t>();
                if (ChildTag == vsyntax::res_of)
                {
                    os.Append('(');
                    bNewlinesOk = false;
                }
                else
                {
                    if (ChildTag != vsyntax::res_none)
                    {
                        DoNewline(false);
                        os.Append(vsyntax::scan_reserved_t()[ChildTag]);
                    }
                    os.Append('{');
                }

                indent_amount++;
                VisitClause(*Child);
                bNewlinesOk = bMacroNewlinesOk;
                indent_amount--;
                if (Child->GetTag<vsyntax::res_t>() == vsyntax::res_of)
                {
                    os.Append(')');
                }
                else
                {
                    DoNewline(false);
                    os.Append('}');
                }
            }
            bNewlinesOk = bOldNewlinesOk;
        }

        /// This is only necessary since we declare the `VISIT_VSTNODE` macro for all VST node types.
        void visit(const Vst::Clause& node)
        {
           ULANG_ENSUREF(false, "A clause means nothing without the context of its parent, the parent is responsible for serializing it");
        }

        void visit(const Vst::Parens& Node)
        {
            os.Append('(');
            ++indent_amount;
            VisitExpressionListWithFormat(Node.GetChildren(), EFormat::Comma);
            --indent_amount; 
            os.Append(')');
        }

        void visit(const Vst::Commas& Node)
        {
            VisitExpressionListWithFormat(Node.GetChildren(), EFormat::Comma);
        }

        void visit(const Vst::Placeholder& Placeholder)
        {
            os.Append("stub{");
            os.Append(Placeholder.GetSourceText());
            os.Append("}");
        }

        void visit(const Vst::ParseError& Error)
        {
            os.AppendFormat("Error (%d:%d) : %s", Error.Whence().BeginRow(), Error.Whence().BeginColumn(), Error.GetError());
        }

        void visit(const Vst::Escape& Escape)
        {
            os.Append('&');
            if (Escape.GetChildCount() == 1)
            {
                PrintElement(Escape.GetChildren()[0]);
            }
        }

    private:
        static constexpr char IndentationString[] = "    ";

        void do_indent()
        {
            for (int i = 0; i < indent_amount; ++i)
            {
                os.Append(IndentationString);
            }
        }

        uLang::CUTF8StringBuilder& os;
        int32_t indent_amount;
        EFormat Format;
        bool bNewlinesOk{ true };
        int NumberOfNewlines{ 0 };
    };    // CVstPrinter

    void VstPrintAppend(const Vst::TNodeRef<Vst::Node>& VstNode, uLang::CUTF8StringBuilder& Source)
    {
        CVstPrinter VstPrinter(Source);
        Vst::Node::VisitWith(VstNode, VstPrinter);
    }
}
