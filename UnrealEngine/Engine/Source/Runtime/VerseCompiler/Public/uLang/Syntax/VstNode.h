// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "uLang/Common/Algo/Cases.h"
#include "uLang/Common/Containers/Array.h"
#include "uLang/Common/Containers/RangeView.h"
#include "uLang/Common/Containers/SharedPointer.h"
#include "uLang/Common/Containers/SharedPointerArray.h"
#include "uLang/Common/Misc/EnumUtils.h"
#include "uLang/Common/Misc/Optional.h"
#include "uLang/Common/Text/TextRange.h"
#include "uLang/Common/Text/UTF8String.h"
#include "uLang/Common/Text/UTF8StringBuilder.h"
#include "uLang/Common/Text/UTF8StringView.h"
#include "uLang/SourceProject/PackageRole.h"
#include "uLang/SourceProject/UploadedAtFNVersion.h"
#include "uLang/SourceProject/VerseScope.h"
#include "uLang/Syntax/NodeDecls.inl"

#define UE_API VERSECOMPILER_API

namespace Verse
{
namespace Vst
{
struct Node;

/**
 * Reference-counted smart pointer to a Vst::Node or any type descended from it.
 *
 * Use TNodeRef<T> (non-nullable) and TNodePtr<T> (nullable) aliases below
 * rather than instantiating TNodePtrG directly.
 */
template<class NodeType, bool AllowNull, class AllocatorType, typename... AllocatorArgsType>
struct TNodePtrG final
{
private:
    // Store as non-const internally so Deallocate works with void*.
    // Const-correctness is enforced through the public interface (operator->, operator*, Get).
    using TMutableNodeType = std::remove_const_t<NodeType>;
    static_assert(std::is_empty_v<AllocatorType>, "TNodePtrG only supports stateless allocators");

    TMutableNodeType* _Object;

    // Construct from raw pointer, incrementing its reference count.
    // Used only by New() and MakeSharedFromThis().
    explicit TNodePtrG(TMutableNodeType* InObject) : _Object(InObject)
    {
        if (InObject) { InObject->Reference(); }
    }

    void ReleaseObject()
    {
        if (_Object->Dereference())
        {
            _Object->~TMutableNodeType();
            AllocatorType::Deallocate(_Object);
        }
    }

    template<class OtherNodeType, bool OtherAllowNull, class OtherAllocatorType, typename... OtherAllocatorArgsType>
    friend struct TNodePtrG;

public:
    // Copy construction
    TNodePtrG(const TNodePtrG& Other) : _Object(Other._Object) { if (_Object) { _Object->Reference(); } }
    template<class OtherNodeType, bool OtherAllowNull>
        requires (std::is_convertible_v<OtherNodeType*, NodeType*> && (AllowNull || !OtherAllowNull))
    TNodePtrG(const TNodePtrG<OtherNodeType, OtherAllowNull, AllocatorType, AllocatorArgsType...>& Other)
        : _Object(Other._Object) { if (_Object) { _Object->Reference(); } }

    // Move construction
    TNodePtrG(TNodePtrG&& Other) : _Object(Other._Object) { Other._Object = nullptr; }
    template<class OtherNodeType, bool OtherAllowNull>
        requires (std::is_convertible_v<OtherNodeType*, NodeType*> && (AllowNull || !OtherAllowNull))
    TNodePtrG(TNodePtrG<OtherNodeType, OtherAllowNull, AllocatorType, AllocatorArgsType...>&& Other)
        : _Object(Other._Object) { Other._Object = nullptr; }

    // Default / nullptr construction (only for AllowNull=true)
    TNodePtrG() : _Object(nullptr) { static_assert(AllowNull, "Cannot default-construct a non-null TNodePtrG."); }
    TNodePtrG(uLang::NullPtrType) : _Object(nullptr) { static_assert(AllowNull, "Cannot construct a non-null TNodePtrG from nullptr."); }

    ~TNodePtrG() { if (_Object) { ReleaseObject(); } }

    // Factory methods
    template <typename... CtorArgsType>
    static TNodePtrG New(CtorArgsType&&... CtorArgs)
    {
        TMutableNodeType* Object = new(AllocatorType{}) TMutableNodeType(uLang::ForwardArg<CtorArgsType>(CtorArgs)...);
        ULANG_ASSERT(Object);
        return TNodePtrG(Object);
    }

    static TNodePtrG MakeSharedFromThis(const NodeType* const Node)
    {
        ULANG_ASSERTF(AllowNull || Node, "MakeSharedFromThis cannot create a non-null TNodePtrG from nullptr");
        ULANG_ASSERTF(!Node || Node->GetRefCount() > 0, "MakeSharedFromThis called on a Node that is not already managed by a TNodePtrG");
        return TNodePtrG(const_cast<TMutableNodeType*>(Node));
    }

    // Copy assignment
    TNodePtrG& operator=(const TNodePtrG& Other)
    {
        if (_Object != Other._Object)
        {
            if (Other._Object) { Other._Object->Reference(); }
            if (_Object) { ReleaseObject(); }
            _Object = Other._Object;
        }
        return *this;
    }
    template<class OtherNodeType, bool OtherAllowNull>
        requires (std::is_convertible_v<OtherNodeType*, NodeType*> && (AllowNull || !OtherAllowNull))
    TNodePtrG& operator=(const TNodePtrG<OtherNodeType, OtherAllowNull, AllocatorType, AllocatorArgsType...>& Other)
    {
        if (_Object != Other._Object)
        {
            if (Other._Object) { Other._Object->Reference(); }
            if (_Object) { ReleaseObject(); }
            _Object = Other._Object;
        }
        return *this;
    }

    // Move assignment
    TNodePtrG& operator=(TNodePtrG&& Other)
    {
        if (this != &Other)
        {
            if (_Object) { ReleaseObject(); }
            _Object = Other._Object;
            Other._Object = nullptr;
        }
        return *this;
    }

    template<class OtherNodeType, bool OtherAllowNull>
        requires (std::is_convertible_v<OtherNodeType*, NodeType*> && (AllowNull || !OtherAllowNull))
    TNodePtrG& operator=(TNodePtrG<OtherNodeType, OtherAllowNull, AllocatorType, AllocatorArgsType...>&& Other)
    {
        if (_Object) { ReleaseObject(); }
        _Object = Other._Object;
        Other._Object = nullptr;
        return *this;
    }

    // Nullptr assignment (only for AllowNull=true)
    TNodePtrG& operator=(uLang::NullPtrType) { static_assert(AllowNull, "Cannot assign null to non-null TNodePtrG."); if (_Object) { ReleaseObject(); _Object = nullptr; } return *this; }

    // Accessors
    NodeType* operator->() const { ULANG_ASSERT(AllowNull || _Object); return _Object; }
    NodeType& operator*() const { ULANG_ASSERT(_Object); return *static_cast<NodeType*>(_Object); }
    NodeType* Get() const { ULANG_ASSERT(AllowNull || _Object); return _Object; }
    operator NodeType* () const { return _Object; }
    void Reset() { if (_Object) { ReleaseObject(); _Object = nullptr; } }
    bool IsValid() const { return _Object != nullptr; }

    // Bool / validation
    operator bool() const { return _Object != nullptr; }
    bool operator!() const { return _Object == nullptr; }

    // Type cast (reinterpret the pointer to a different node type)
    template<class OtherNodeType>
    TNodePtrG<OtherNodeType, AllowNull, AllocatorType, AllocatorArgsType...>& As() { return reinterpret_cast<TNodePtrG<OtherNodeType, AllowNull, AllocatorType, AllocatorArgsType...>&>(*this); }
    template<class OtherNodeType>
    const TNodePtrG<OtherNodeType, AllowNull, AllocatorType, AllocatorArgsType...>& As() const { return reinterpret_cast<const TNodePtrG<OtherNodeType, AllowNull, AllocatorType, AllocatorArgsType...>&>(*this); }

    // AsRef: convert nullable TNodePtr to non-null TNodeRef
    TNodePtrG<NodeType, false, AllocatorType, AllocatorArgsType...>& AsRef()
    {
        static_assert(AllowNull, "Unnecessary AsRef on non-null TNodePtrG!");
        ULANG_ASSERTF(_Object, "Converting null TNodePtr to TNodeRef!");
        return reinterpret_cast<TNodePtrG<NodeType, false, AllocatorType, AllocatorArgsType...>&>(*this);
    }

    const TNodePtrG<NodeType, false, AllocatorType, AllocatorArgsType...>& AsRef() const
    {
        static_assert(AllowNull, "Unnecessary AsRef on non-null TNodePtrG!");
        ULANG_ASSERTF(_Object, "Converting null TNodePtr to TNodeRef!");
        return reinterpret_cast<const TNodePtrG<NodeType, false, AllocatorType, AllocatorArgsType...>&>(*this);
    }

    // Comparison operators
    bool operator==(uLang::NullPtrType) const { return _Object == nullptr; }
    bool operator!=(uLang::NullPtrType) const { return _Object != nullptr; }
    bool operator==(NodeType* Other) const { return _Object == Other; }
    bool operator!=(NodeType* Other) const { return _Object != Other; }
    bool operator==(const TNodePtrG& Other) const { return _Object == Other._Object; }
    bool operator!=(const TNodePtrG& Other) const { return _Object != Other._Object; }
    template<class OtherNodeType, bool OtherAllowNull>
    bool operator==(const TNodePtrG<OtherNodeType, OtherAllowNull, AllocatorType, AllocatorArgsType...>& Other) const { return _Object == Other._Object; }
    template<class OtherNodeType, bool OtherAllowNull>
    bool operator!=(const TNodePtrG<OtherNodeType, OtherAllowNull, AllocatorType, AllocatorArgsType...>& Other) const { return _Object != Other._Object; }

    // Swap support
    friend void Swap(TNodePtrG& A, TNodePtrG& B) { uLang::Swap(A._Object, B._Object); }
};

/// Non-nullable shared reference to a Vst node. Guarantees the pointee is always valid.
template<typename NodeType> using TNodeRef = TNodePtrG<NodeType, false, uLang::CHeapRawAllocator>;

/// Nullable shared pointer to a Vst node. May be null -- check before dereferencing.
template<typename NodeType> using TNodePtr = TNodePtrG<NodeType, true, uLang::CHeapRawAllocator>;

}
}    // namespace Verse

namespace uLang
{
class CAstNode;
class CExpressionBase;
struct SIndexedSourceText;

/// This is used as a helper struct for storing the result of a signed distance check.
struct LocusDistanceResult
{
    Verse::Vst::TNodeRef<Verse::Vst::Node> Node;
    int32_t Distance;
};
}    // namespace uLang

namespace Verse
{
class FTile;

using string = uLang::CUTF8String;
using CUTF8String = uLang::CUTF8String;
using CUTF8StringView = uLang::CUTF8StringView;
using CUTF8StringBuilder = uLang::CUTF8StringBuilder;

using SIndexedSourceText = uLang::SIndexedSourceText;

using ChType = uLang::UTF8Char;

// uLang Types
template<typename T>
using LArray = uLang::TArray<T>;

using EResult = uLang::EResult;
using CSharedMix = uLang::CSharedMix;
using SLocus = uLang::STextRange;
using SPosition = uLang::STextPosition;
using EVerseScope = uLang::EVerseScope;

inline SLocus NullWhence()
{
	return SLocus(0, 0, 0, 0);
}

namespace Vst
{
    class CAtom;
    struct Clause;
    struct Snippet;
    struct Identifier;

    enum class EChildDeletionBehavior : uint8_t
    {
        CreatePlaceholder,
        Delete,
        Default    
    };
    
    enum class ESupportsManyChildren : uint8_t
    {
        Anywhere,
        TrailingOnly,
        Nowhere
    };

#define VISIT_VSTNODE(NodeName, RequiredChildren, SupportsManyChildren, Precedence, ChildDeletionBehavior, IsCAtom) NodeName,
    enum class NodeType : uint8_t
    {
        VERSE_ENUM_VSTNODES(VISIT_VSTNODE)
    };
#undef VISIT_VSTNODE

#define VISIT_VSTNODE(NodeName, RequiredChildren, Precedence, SupportsManyChildren, ChildDeletionBehavior, IsCAtom) {#NodeName, RequiredChildren, Precedence, ESupportsManyChildren::SupportsManyChildren, EChildDeletionBehavior::ChildDeletionBehavior, IsCAtom},
    constexpr struct NodeInfo
    {
        const char* FormalName;
        int32_t RequiredChildren;
        int32_t Precedence;
        ESupportsManyChildren SupportsManyChildren;
        EChildDeletionBehavior ChildDeletionBehavior;
        bool bIsCAtom;
    } NodeInfos[] = {
        VERSE_ENUM_VSTNODES(VISIT_VSTNODE)};
#undef VISIT_VSTNODE

    static constexpr uint8_t NumNodeTypes = static_cast<uint8_t>(ULANG_COUNTOF(NodeInfos));

    static const uint8_t TagNone = 255;

    inline const char* GetNodeTypeName(const NodeType TypeOfNode)
    {
        return NodeInfos[static_cast<uint8_t>(TypeOfNode)].FormalName;
    }

    inline int32_t GetNumRequiredChildren(const NodeType TypeOfNode)
    {
        return NodeInfos[static_cast<uint8_t>(TypeOfNode)].RequiredChildren;
    }

    inline int32_t GetOperatorPrecedence(const NodeType TypeOfNode)
    {
        return NodeInfos[static_cast<uint8_t>(TypeOfNode)].Precedence;
    }

    inline ESupportsManyChildren GetSupportsManyChildren(const NodeType TypeOfNode)
    {
        return NodeInfos[static_cast<uint8_t>(TypeOfNode)].SupportsManyChildren;
    }

    inline EChildDeletionBehavior GetChildDeletionBehavior(const NodeType TypeOfNode)
    {
        return NodeInfos[static_cast<uint8_t>(TypeOfNode)].ChildDeletionBehavior;
    }

    struct Node
    {
        using NodeArray = LArray<TNodeRef<Node>>;

        Node()
            : _Children()
            , _Parent(nullptr)
            , _Aux()
            , _PreComments()
            , _PostComments()
            , _NumNewLinesBefore(0)
            , _NumNewLinesAfter(0)
            , _Tag(0)
            , _Type(static_cast<NodeType>(0))
            , _MappedAstNode(nullptr)
            , _Tile(nullptr)
        {}

        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;

        uint32_t GetRefCount() const { return _RefCount; }

        Node(NodeType in_type) : Node()
        {
            _Type = in_type;
            ULANG_ASSERTF(
                   in_type == NodeType::Project
                || in_type == NodeType::Package
                || in_type == NodeType::Module
                || in_type == NodeType::Snippet,
                "Invalid use of locus-free Node constructor for node type that requires a locus");
        }

        Node(NodeType in_type, const SLocus& whence) : Node()
        {
            _Type = in_type;
            _Whence = whence;
            ULANG_ASSERTF(_Whence.IsValid(), "Node created with invalid locus");
        }

        UE_API virtual ~Node();

        static SLocus CombineLocii(const NodeArray& Nodes)
        {
            if (Nodes.IsFilled())
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

        template<typename TNodeType>
        TNodeType& As() { ULANG_ASSERTF(IsA<TNodeType>(), "Vst Node is type `%s` not of expected type `%s` so cannot cast!", NodeInfos[GetElementTypeInt()].FormalName, NodeInfos[static_cast<int32_t>(TNodeType::StaticType)].FormalName); return *static_cast<TNodeType*>(this); }

        template<typename TNodeType>
        const TNodeType& As() const { ULANG_ASSERTF(IsA<TNodeType>(), "Vst Node is type `%s` not of expected type `%s` so cannot cast!", NodeInfos[GetElementTypeInt()].FormalName, NodeInfos[static_cast<int32_t>(TNodeType::StaticType)].FormalName); return *static_cast<const TNodeType*>(this); }

        template <typename TNodeType>
        TNodeType* AsNullable() { return IsA<TNodeType>()? static_cast<TNodeType*>(this) : nullptr; }

        template <typename TNodeType>
        const TNodeType* AsNullable() const { return IsA<TNodeType>() ? static_cast<const TNodeType*>(this) : nullptr; }

        template<typename TNodeType>
        bool IsA() const { return GetElementType() == TNodeType::StaticType; }

        int GetChildCount() const { return _Children.Num(); }
        NodeType GetElementType() const { return _Type; }
        int32_t GetElementTypeInt() const { return static_cast<int32_t>(GetElementType()); }
        template<typename OpType>
        OpType GetTag() const { return static_cast<OpType>(_Tag); }
        template<typename OpType=uint8_t>
        void SetTag(OpType in_op) { _Tag = static_cast<uint8_t>(in_op); }

        UE_API bool HasAttributes() const;
        UE_API const Identifier* GetAttributeIdentifier(const CUTF8StringView& AttributeName) const;
        UE_API bool IsAttributePresent(const CUTF8StringView& AttributeName) const;

        UE_API const Node* TryGetFirstAttributeOfType(NodeType Type) const;

        template<typename TNodeType>
        const TNodeType* TryGetFirstAttributeOfType() const
        {
            const Node* Result = TryGetFirstAttributeOfType(TNodeType::StaticType);

            if(Result != nullptr)
            {
                return &Result->As<TNodeType>();
            }

            return nullptr;
        }

        UE_API void PrependAux(const TNodeRef<Node>& AuxChild);
        UE_API void PrependAux(const NodeArray& AuxChildren);
        UE_API void AppendAux(const TNodeRef<Node>& AuxChild);
        UE_API void AppendAux(const NodeArray& AuxChildren);
        UE_API void AppendAuxAt(const TNodeRef<Node>& AuxChild, int32_t Idx);
        const TNodePtr<Clause>& GetAux() const { return _Aux; }
        UE_API void SetAux(const TNodeRef<Clause>& Aux);
        void RemoveAux() { _Aux.Reset(); }

        UE_API void AppendPrefixComment(const TNodeRef<Node>& CommentNode);
        UE_API void AppendPrefixComment(TNodeRef<Node>&& CommentNode);
        UE_API void AppendPrefixComments(const NodeArray& CommentNodes);
        UE_API void AppendPostfixComment(const TNodeRef<Node>& CommentNode);
        UE_API void AppendPostfixComment(TNodeRef<Node>&& CommentNode);
        UE_API void AppendPostfixComments(const NodeArray& CommentNodes);
        const NodeArray& GetPrefixComments() const { return _PreComments; }
        const NodeArray& GetPostfixComments() const { return _PostComments; }
        NodeArray& AccessPrefixComments() { return _PreComments; }
        NodeArray& AccessPostfixComments() { return _PostComments; }

        void SetWhence(const SLocus& Whence) { _Whence = Whence; }
        void CombineWhenceWith(const SLocus& Whence) { _Whence |= Whence; }
        const SLocus& Whence() const         { return _Whence; }
        UE_API const CUTF8String& GetSnippetPath() const;
        UE_API const Snippet* FindSnippetByFilePath(const CUTF8StringView& FilePath) const;
        UE_API const Node* FindChildByPosition(const SPosition& TextPosition) const;
        UE_API const TNodeRef<Node> FindChildClosestToPosition(const SPosition& TextPosition, const SIndexedSourceText& SourceText) const;
        template<class VisitPolicy, typename ReturnType = void >
        static void VisitWith(const TNodeRef<Node>& node, VisitPolicy& visit_policy);

        const NodeInfo& GetElementInfo() const { return NodeInfos[GetElementTypeInt()]; }
        const ChType* GetElementName() const { return (const ChType*)NodeInfos[GetElementTypeInt()].FormalName; }
        int32_t GetPrecedence() const { return GetOperatorPrecedence(GetElementType()); }
        int32_t NumRequiredChildren() const
        {
            return GetNumRequiredChildren(GetElementType());
        }
        ESupportsManyChildren IsManyChildrenSupported() const
        {
            return GetSupportsManyChildren(GetElementType());
        }

        int32_t NumNewLinesBefore() const
        {
            return _NumNewLinesBefore;
        }

        void SetNumNewLinesBefore(const int32_t Num)
        {
            _NumNewLinesBefore = Num;
        }

        int32_t NumNewLinesAfter() const
        {
            return _NumNewLinesAfter;
        }

        void SetNumNewLinesAfter(const int32_t Num)
        {
            _NumNewLinesAfter = Num;
        }

        void SetNewLineAfter(const bool bNewLineAfter)
        {
            if (bNewLineAfter)
            {
                // Don't touch the node if it already has new lines after it; we preserve the information.
                if (NumNewLinesAfter() > 0)
                {
                    return;
                }
                else
                {
                    SetNumNewLinesAfter(1);
                }
            }
            else
            {
                SetNumNewLinesAfter(0);
            }
        }

        bool HasNewLinesAfter() const
        {
            return _NumNewLinesAfter > 0;
        }

        bool HasNewLinesBefore() const
        {
            return _NumNewLinesBefore > 0;
        }

        bool IsEmpty() const { return _Children.IsEmpty(); }
        const NodeArray& GetChildren() const { return _Children; }
        NodeArray& AccessChildren() { return _Children; }
        NodeArray TakeChildren()
        {
            for (const TNodeRef<Node>& Child : _Children)
            {
                Child->_Parent = nullptr;
            }
            return Move(_Children);
        }
        TNodePtr<Node> GetRightmostChild() const
        {
            return GetChildCount() > 0
                ? TNodePtr<Node>(GetChildren()[GetChildCount() - 1])
                : TNodePtr<Node>(nullptr);
        }
        Node* AccessParent() { return _Parent; }
        TNodeRef<Node> GetSharedSelf() { return TNodeRef<Node>::MakeSharedFromThis(this); }
        TNodeRef<Node> GetSharedSelf() const { return TNodeRef<Node>::MakeSharedFromThis(this); }
        const Node* GetParent() const { return _Parent; }
        void SetParentInternal(TNodeRef<Node> InParent) { DropParent(GetSharedSelf()); _Parent = InParent; }
        bool HasParent() const
        {
            return this->GetParent() != nullptr;
        }
        template<class Type>
        const Type* GetParentOfType() const
        {
            for (const Node* CurNode = this; CurNode; CurNode = CurNode->_Parent)
            {
                if (CurNode->GetElementType() == Type::StaticType)
                {
                    return static_cast<const Type*>(CurNode);
                }
            }

            return nullptr;
        }

        bool IsElementType(NodeType InType) const { return InType == _Type; }

        UE_API const CAtom* AsAtomNullable() const;

        bool IsChildElementType(int32_t idx, NodeType InType) const { return _Children.Num() > idx && _Children[idx]->IsElementType(InType); }

        bool IsError() const { return GetElementType() == NodeType::ParseError; }

        void DebugOrphanCheck()
        {
#ifdef _DEBUG
            const Node* OrphanedNode = FindOrphanedNode(*this);
            ULANG_ASSERTF(!OrphanedNode, "An orphaned node was encountered!");
#endif
        }

        bool Contains(const Node& Target, const bool bRecursive = true) const
        {
            struct FVstContains_Visitor
            {
                static bool Contains(const TNodeRef<Node>& Root, const Node& RecursiveTarget)
                {
                    bool bRecursiveContains = false;
                    for (const TNodePtr<Node> Child : Root->AccessChildren())
                    {
                        if (Child.Get() == &RecursiveTarget)
                        {
                            bRecursiveContains = true;
                        }
                        else if (Child.IsValid())
                        {
                            bRecursiveContains = Contains(Child.AsRef(), RecursiveTarget);
                        }

                        if (bRecursiveContains)
                        {
                            break;
                        }
                    }

                    return bRecursiveContains;
                }
            };

            bool bContains = false;
            for (const TNodeRef<Node>& Child : _Children)
            {
                if (Child.Get() == &Target)
                {
                    bContains = true;
                }
                else if (bRecursive)
                {
                    bContains = FVstContains_Visitor::Contains(Child, Target);
                }

                if (bContains)
                {
                    break;
                }
            }

            return bContains;
        }

        int32_t FindPreviousSibling()
        {
            if (!this->HasParent())
            {
                return uLang::IndexNone;
            }
            const Node* Parent = this->GetParent();
            const int32_t NumChildren = Parent->GetChildCount();
            if (NumChildren == 1)
            {
                return uLang::IndexNone;
            }
            const NodeArray& Children = Parent->GetChildren();
            for (int32_t Index = 0; Index < NumChildren; ++Index)
            {
                const TNodeRef<Node>& CurNode = Children[Index];
                if (CurNode == this)
                {
                    return Index - 1;
                }
            }
            return uLang::IndexNone;
        }

        const TNodeRef<Node>& AppendChild(const TNodeRef<Node>& child)
        {
            DropParent(child);
            child->_Parent = this;
            _Children.Push(child);
            DebugOrphanCheck();
            return _Children.Last();
        }

        const TNodeRef<Node>& AppendChild(const TNodeRef<Node>&& child)
        {
            DropParent(child);
            child->_Parent = this;
            _Children.Push(child);
            DebugOrphanCheck();
            return _Children.Last();
        }

        const TNodeRef<Node>& AppendChildAt(const TNodeRef<Node>& child, int32_t idx)
        {
            DropParent(child);
            child->_Parent = this;
            _Children.Insert(child, idx);
            DebugOrphanCheck();
            return _Children[idx];
        }

        void SetChildAt(int32_t Index, TNodeRef<Node> Child)
        {
            DropParent(Child);
            Child->_Parent = this;
            _Children[Index] = Move(Child);
            DebugOrphanCheck();
        }

        TNodeRef<Node> TakeChildAt(int32_t idx, const TNodePtr<Node> Replacement = TNodePtr<Node>())
        {
            const auto ChildAtIdx = _Children[idx];
            if (Replacement.IsValid())
            {
                Replacement->_Parent = this;
                _Children[idx] = Replacement.AsRef();
            }
            else
            {
                _Children.RemoveAt(idx);
            }
            ChildAtIdx->_Parent = nullptr;
            return ChildAtIdx;
        }

        void AppendChildren(const NodeArray& Children, int32_t NumToAppend = -1)
        {
            NumToAppend = NumToAppend == -1 ? Children.Num() : NumToAppend;
            _Children.Reserve(_Children.Num() + NumToAppend);
            for (int32_t i = 0; i < NumToAppend; i += 1)
            {
                const auto& Expr = Children[i];
                AppendChild(Expr);
            }
        }

        // Prepend the given nodes to this node's child list in reverse order
        // i.e. the last node in the given list will end up as the first child of this node.
        void PrependChildren(const uLang::TArray<TNodeRef<Node>>& Children, int32_t NumToAppend = -1)
        {
            NumToAppend = NumToAppend == -1 ? Children.Num() : NumToAppend;
            _Children.Reserve(_Children.Num() + NumToAppend);
            for (int32_t i = 0; i < NumToAppend; i += 1)
            {
                const auto& Expr = Children[i];
                AppendChildAt(Expr, 0);
            }
        }

        UE_API void ReplaceSelfWith(const TNodeRef<Node>& replacement);
        
        // Supply an index if you have one
        UE_API bool RemoveFromParent(int32_t idx = uLang::IndexNone);

        void Empty()
        {
            for (const auto& Child : _Children)
            {
                if (ULANG_ENSUREF(Child->_Parent == this, "Child does not belong to me!"))
                {
                    Child->_Parent = nullptr;
                }
            }

            _Children.Reset();
        }

        const Snippet* FindSnippet() const
        {
            return GetParentOfType<Snippet>();
        }

        Node* FindRoot()
        {
            Node* root = this;
            while (root->_Parent != nullptr && root->_Parent->GetElementType() != NodeType::Snippet)
            {
                root = root->AccessParent();
            }

            return root;
        }

        static void TransferChildren(const TNodeRef<Node>& From, const TNodeRef<Node>& To, int32_t First, int32_t Last)
        {
            ULANG_ASSERTF(Last <= From->GetChildCount() - 1, "Not enough elements in source array");

            To->AccessChildren().Reserve(To->GetChildCount() + Last - First + 1);

            for (int32_t i = First; i <= Last; i += 1)
            {
                const auto & CurChild = From->GetChildren()[i];
                CurChild->_Parent = nullptr;
                To->AppendChild(CurChild);
            }

            From->AccessChildren().RemoveAt(First, Last - First + 1);
        }

        static void TransferChildren(const TNodeRef<Node>& From, const TNodeRef<Node>& To)
        {
            TransferChildren(From, To, 0, From->GetChildCount() - 1);
        }

        static void TransferPrefixComments(const TNodeRef<Node>& From, const TNodeRef<Node>& To)
        {
            NodeArray& ToPrefixComments = To->AccessPrefixComments();
            NodeArray& FromPrefixComments = From->AccessPrefixComments();
            ToPrefixComments.Reserve(ToPrefixComments.Num() + FromPrefixComments.Num());
            for (TNodeRef<Node>& FromPrefixComment : FromPrefixComments)
            {
                FromPrefixComment->_Parent = nullptr;
                To->AppendPrefixComment(FromPrefixComment);
            }
            FromPrefixComments.Empty();
        }

        static void TransferPostfixComments(const TNodeRef<Node>& From, const TNodeRef<Node>& To)
        {
            NodeArray& ToPostfixComments = To->AccessPostfixComments();
            NodeArray& FromPostfixComments = From->AccessPostfixComments();
            ToPostfixComments.Reserve(ToPostfixComments.Num() + FromPostfixComments.Num());
            for (TNodeRef<Node>& FromPostfixComment : FromPostfixComments)
            {
                FromPostfixComment->_Parent = nullptr;
                To->AppendPostfixComment(FromPostfixComment);
            }
            FromPostfixComments.Empty();
        }

        const uLang::CAstNode* GetMappedAstNode() const { return _MappedAstNode; }
        UE_API void AddMapping(uLang::CAstNode* AstNode) const;
        static UE_API void RemoveMapping( uLang::CAstNode* AstNode );

        UE_API void EnsureAuxAllocated();

        void SetTile(FTile* Tile)
        {
            _Tile = Tile;
        }

        FTile* GetTile()
        {
            return _Tile;
        }

        VERSECOMPILER_API virtual TNodeRef<Node> CloneNode() const;
        void CloneNodeFields(Node* DestOther) const;

    protected:

        friend class uLang::CAstNode;

        /**
         * Checks for any nodes that have their parent set incorrectly within the hierarchy.
         *
         * @param Root 	A pointer to the root of the tree to inspect for any orphaned nodes.
         *
         * @return 		A  pointer to the first node that was found to have an incorrect parent set.
         *					If no such node was found, returns `nullptr`.
         */
        static const Node* FindOrphanedNode(const Node& InNode)
        {
            struct SChildParent
            {
                const Node* _Child;
                const Node* _Parent;
            };
            LArray<SChildParent> Stack;
            // NOTE: (YiLiangSiew) Arbitrary stack size chosen here to try avoiding re-allocation during traversal.
            Stack.Reserve(256);
            for (const TNodeRef<Node>& CurChild : InNode.GetChildren())
            {
                Stack.Add({CurChild.Get(), &InNode});
            }
            while (!Stack.IsEmpty())
            {
                const SChildParent& CurPair = Stack.Pop();
                if (CurPair._Child->GetParent() != CurPair._Parent)
                {
                    return CurPair._Child;
                }
                for (const TNodeRef<Node>& CurChild : CurPair._Child->GetChildren())
                {
                    Stack.Add({CurChild, CurPair._Child});
                }
            }
            return nullptr;
        };

        /**
         * Validates that the child being added to this node is currently orphaned.
         * If the child has a parent, then two nodes would own the same child, which is impossible.
         */
        void DropParent(const TNodeRef<Node>& Child)
        {
            if (ULANG_ENSUREF(Child->GetParent()==nullptr, "Child already has a parent!"))
            {
            }
            else
            {
                Child->AccessParent()->AccessChildren().Remove(Child);
                Child->_Parent = nullptr;
            }
        }

        void AppendChildInternal(const TNodeRef<Node>& child)
        {
            DropParent(child);

            child->_Parent = this;
            _Children.Push(child);

            DebugOrphanCheck();
        }

        void AppendChildInternal(TNodeRef<Node>&& child)
        {
            DropParent(child);

            child->_Parent = this;
            _Children.Push(Move(child));

            DebugOrphanCheck();
        }

        NodeArray _Children;
        /** VstNodes point at their parent. We guarantee that a VstNode can only exist in one place in the Vst tree at a time, so the Parent<->Child relationship is unique. */
        Node* _Parent;
        /** Auxiliary data such as attributes associated with this VstNode. The transaction system addresses this via child index -1. */
        TNodePtr<Clause> _Aux;

        // list of comment nodes that appear before or after this node
        NodeArray _PreComments;
        NodeArray _PostComments;

        /**
         * Text location from whence this node was parsed.
         * (1) must be contained in (= not be partially outside of) parent's _Whence;
         * (2) must not overlap any sibling's _Whence;
         * (3) snippets (text files), programs, and some modules will not have a valid locus
         */
        SLocus _Whence;

        /// The number of trailing newlines that should follow this node.
        int32_t _NumNewLinesBefore;
        int32_t _NumNewLinesAfter;

        /** Describes the role of this node in the context of its parent. e.g. Children of BinaryOpAddSub are tagged as `Operator` or `Operand` */
        uint8_t _Tag;
        /** Runtime type information about this node */
        NodeType _Type;
        mutable uint32_t _RefCount = 0;
        mutable const uLang::CAstNode* _MappedAstNode;
        FTile* _Tile;

    private:
        template<class OtherNodeType, bool OtherAllowNull, class OtherAllocatorType, typename... OtherAllocatorArgsType>
        friend struct TNodePtrG;

        void Reference() const { ++_RefCount; }
        bool Dereference() const
        {
            ULANG_ASSERTF(_RefCount > 0, "Tried to dereference an object that has no references!");
            return (--_RefCount == 0);
        }
    };  // Node

    using NodeArray = Node::NodeArray;

    struct Clause : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Clause;
        enum class EForm : int8_t
        {
            Synthetic,                // The clause doesn't occur in source, but is instead used as a way to group multiple subexpressions.
            NoSemicolonOrNewline,     // The clause does not contains a semicolon or a newline: x or {x} or {x,y} but not {x;}
            HasSemicolonOrNewline,    // The clause does contain a semicolon or a newline: {x;} or {x;y,z} or \n\tx

            /// Used for clauses that have a single attribute identifier VST node within it. This means the clause should use angle brackets (i.e. `<`/`>`) instead of curly braces.
            /// This also means that the clause is before the identifier (i.e. `<@custom_attribute>identifier`).
            IsPrependAttributeHolder,
            /// Used for clauses that have a single attribute identifier VST node within it. This means the clause should use angle brackets (i.e. `<`/`>`) instead of curly braces.
            /// This also means that the clause is after the identifier (i.e. `class<pure>`).
            IsAppendAttributeHolder
        };

        enum class EPunctuation : int8_t
        {
            Unknown,
            Braces,
            Colon,
            Indentation
        };

        Clause(const SLocus& Whence, const EForm Form, const EPunctuation Punctuation) : Node(StaticType, Whence), _Form(Form), _Punctuation(Punctuation) {}

        Clause(const SLocus& Whence, const EForm Form) : Node(StaticType, Whence), _Form(Form), _Punctuation(EPunctuation::Unknown) {}

        Clause(uint8_t ClauseType, const SLocus& Whence, EForm Form) : Node(StaticType, Whence), _Form(Form), _Punctuation(EPunctuation::Unknown)
        {
            _Tag = ClauseType;
        }

        Clause(const TNodeRef<Node>& Child, const SLocus& Whence, EForm Form) : Clause(0, Whence, Form)
        {
            AppendChild(Child);
        }

        Clause(const NodeArray& Children, const SLocus& Whence, EForm Form) : Clause(0, Whence, Form)
        {
            AppendChildren(Children);
        }

        Clause(const NodeArray& Children, int32_t NumToAdd, const SLocus& Whence, EForm Form) : Clause(0, Whence, Form)
        {
            AppendChildren(Children, NumToAdd);
        }

        EForm GetForm() const { return _Form; }
        void SetForm(const EForm InForm)
        {
            this->_Form = InForm;
        }

        EPunctuation GetPunctuation() const
        {
            return _Punctuation;
        }
        void SetPunctuation(const EPunctuation InPunctuation)
        {
            this->_Punctuation = InPunctuation;
        }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;

    private:
        EForm _Form;

        /// Tells us whether the clause should be using either colon and newlines, or curly braces and semicolons to separate expressions.
        EPunctuation _Punctuation;
    };

    VERSECOMPILER_API const TNodeRef<Node> MakeStub(const SLocus& Whence);

    /**
     * Syntax element that does not need children. These nodes do not have a structure (i.e. no children), but
     * rather are leaves in the hierarchy.
     **/
    class CAtom : public Node
    {
    public:

        // Public Data Members

        CUTF8String _OriginalCode;

        // Methods

        CAtom(const uLang::CUTF8StringView& CodeStr, NodeType InType, const SLocus& Whence) : Node(InType, Whence), _OriginalCode(CodeStr) {}

        const string&      GetStringValue() const  { return _OriginalCode; }
        const CUTF8String& GetSourceText() const   { return _OriginalCode; }
        const char*        GetSourceCStr() const   { return _OriginalCode.AsCString(); }
    };

    struct Comment : public CAtom
    {
        enum class EType : uint8_t { block, line, ind, frag };

        static const Vst::NodeType StaticType = NodeType::Comment;

        Comment(EType Type, const CUTF8StringView& InText, const SLocus& Whence) : CAtom(InText, StaticType, Whence), _Type(Type) {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;

        EType _Type;
    };

    const char* CommentTypeToString(Comment::EType Type);

    // A collection of sub trees that are stored in the same source, e.g. a text file or UProperty
    struct Snippet : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Snippet;
        Snippet() : Node(StaticType) {}
        Snippet(const CUTF8StringView& Path) : Node(StaticType), _Path(Path) {}
        Snippet(const CUTF8StringView& Path, const TNodeRef<Node>& FirstChild) : Node(StaticType, FirstChild->Whence()), _Path(Path) { AppendChild(FirstChild); }
        Snippet(const TNodeRef<Node>& FirstChild) : Node(StaticType, FirstChild->Whence()) { AppendChild(FirstChild); }

        bool HasErrors() const
        {
            bool bHasErrors = false;
            for (const TNodeRef<Node>& Node : _Children)
            {
                if (Node->IsError())
                {
                    bHasErrors = true;
                    break;
                }
            }
            return bHasErrors;
        }

        Clause::EForm GetForm() const { return _Form; }
        void SetForm(Clause::EForm Form) { _Form = Form; }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
        void CloneNodeFields(Snippet* DestNode) const;

        // A place to remember what source version this snippet represents.
        uint64_t _SnippetVersion = 0;

        // Where this snippet came from - usually this is the fully qualified path of the associated text file
        CUTF8String _Path;

    private:
        Clause::EForm _Form{Clause::EForm::Synthetic};
    };

    // A collection of snippets
    struct Module : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Module;
        Module(const CUTF8StringView& Name) : Node(StaticType), _Name(Name) {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
        void CloneNodeFields(Module* DestNode) const;

        // The name of this module
        CUTF8String _Name;

        // File path of vmodule file if exists, or directory path with trailing slash
        CUTF8String _FilePath;
    };

    // A collection of Module nodes
    struct Package : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Package;

        Package(const CUTF8StringView& Name) : Node(StaticType), _Name(Name), _DependencyPackages()
        {}

        // The name of this package
        CUTF8String _Name;

        // Directory path where the source files are located
        CUTF8String _DirPath;

        // File path of vpackage file if exists, empty otherwise
        CUTF8String _FilePath;

        // Verse path of the root module of this package
        CUTF8String _VersePath;

        // Names of packages this package is dependent on
        LArray<CUTF8String> _DependencyPackages;

        // Destination directory for VNI generated C++ code (fully qualified)
        uLang::TOptional<CUTF8String> _VniDestDir;

        // The role this package plays in the project.
        uLang::EPackageRole _Role = uLang::EPackageRole::Source;

        // Origin/visibility of Verse code in this package
        EVerseScope _VerseScope = EVerseScope::PublicUser;

        // The language version targetted by the Verse code in this package.
        uLang::TOptional<uint32_t> _VerseVersion;

        /// This allows us to determine when a package was uploaded for a given Fortnite release version.
        /// It is a HACK that conditionally enables/disables behaviour in the compiler in order to
        /// support previous mistakes allowed to slip through in previous Verse langauge releases but
        /// now need to be supported for backwards compatability.
        /// When we can confirm that all Fortnite packages that are currently uploaded are beyond this
        /// version being used in all instances of the codebase, this can then be removed.
        uint32_t _UploadedAtFNVersion = VerseFN::UploadedAtFNVersion::Latest;

        // If true, module macros in this package's source and digest will be treated as implicit
        bool _bTreatModulesAsImplicit = false;

        // Whether to allow the use of experimental definitions in this package.
        bool _bAllowExperimental = false;

        // Whether Scene Graph is enabled or not. Impacts the asset digest generated.
        bool _bEnableSceneGraph = false;

        // Whether Scene Graph Animation is enabled or not. Impacts the asset digest generated.
        bool _bEnableSceneGraphAnimation = false;

        VERSECOMPILER_API TNodeRef<Module> FindOrAddModule(const CUTF8StringView& ModuleName, const CUTF8StringView& ParentModuleName = CUTF8StringView());
        VERSECOMPILER_API static uLang::TOptional<TNodeRef<Module>> FindModule(const Node& ModuleContainer, const CUTF8StringView& ModuleName);

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
        void CloneNodeFields(Package* DestNode) const;
    };

    // A collection of Package nodes
    // Packages (children) are sorted in dependency order (i.e. dependents always follow their dependencies)
    struct Project : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Project;
        Project(const CUTF8StringView& Name) : Node(StaticType), _Name(Name) {}

        // The name of this project
        CUTF8String _Name;

        // File path of vproject file if exists, empty otherwise
        CUTF8String _FilePath;

        VERSECOMPILER_API const TNodeRef<Module>& FindOrAddModule(const CUTF8StringView& ModuleName, const CUTF8StringView& FilePath, const CUTF8StringView& ParentModuleName = CUTF8StringView());
        VERSECOMPILER_API static uLang::TOptional<TNodeRef<Module>> FindModule(const Node& ModuleContainer, const CUTF8StringView& ModuleName);

        /**
         * Removes any packages from the project that have the given name.
         *
         * @param PackageName   Any package that has this name in the project will be removed.
         *
         * @return              `true` if any packages were removed, `false` if no packages with the name were found
         *                      or another error occurred.
         */
        bool RemovePackagesWithName(const CUTF8StringView& PackageName)
        {
            // NOTE: (YiLiangSiew) This assumes that packages are not stored recursively in the project,
            // or that projects do not have other projects as their descendents.
            NodeArray& Children = this->AccessChildren();
            const int32_t NumChildren = Children.Num();
            if (NumChildren == 0)
            {
                return false;
            }
            int32_t NumPackagesRemoved = 0;
            for (int32_t Index = NumChildren - 1; Index >= 0; --Index)
            {
                TNodeRef<Node> CurChild = Children[Index];
                if (!CurChild->IsA<Package>())
                {
                    continue;
                }
                if (CurChild->As<Package>()._Name == PackageName)
                {
                    Children.RemoveAt(Index);
                    ++NumPackagesRemoved;
                }
            }
            return NumPackagesRemoved > 0;
        }

        bool ReplaceSnippet(const CUTF8StringView& PathOfOldSnippetToReplace, TNodeRef<Snippet> NewSnippet, TNodePtr<Snippet>* OutOldSnippet = nullptr)
        {
            for (const TNodeRef<Node>& CurChild : this->_Children)
            {
                if (CurChild->IsA<Package>())
                {
                    Package& CurPackage = CurChild->As<Package>();
                    for (const TNodeRef<Node>& CurPkgChild : CurPackage.GetChildren())
                    {
                        if (CurPkgChild->IsA<Module>())
                        {
                            Module& CurModule = CurPkgChild->As<Module>();
                            for (const TNodeRef<Node>& CurModuleChild : CurModule.GetChildren())
                            {
                                if (CurModuleChild->IsA<Snippet>())
                                {
                                    Snippet& CurSnippet = CurModuleChild->As<Snippet>();
                                    if (CurSnippet._Path == PathOfOldSnippetToReplace)
                                    {
                                        if (OutOldSnippet != nullptr)
                                        {
                                            *OutOldSnippet = CurModuleChild.As<Snippet>();
                                        }
                                        CurSnippet.ReplaceSelfWith(NewSnippet);
                                        return true;
                                    }
                                }
                            }
                        }
                        else if (CurPkgChild->IsA<Snippet>())
                        {
                            Snippet& CurSnippet = CurPkgChild->As<Snippet>();
                            if (CurSnippet._Path == PathOfOldSnippetToReplace)
                            {
                                if (OutOldSnippet != nullptr)
                                {
                                    *OutOldSnippet = CurPkgChild.As<Snippet>();
                                }
                                CurSnippet.ReplaceSelfWith(NewSnippet);
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
        void CloneNodeFields(Project* DestNode) const;
    };

    /**
     * Corresponds to `$1:$2 = $3` syntax, where the `$2` is optional.  If and
     * only if `$2` is present, the left-hand side is a `TypeSpec`.  For example,
     * @code
     * X:int = Y
     * @endcode
     * becomes
     * @code
     * Definition
     *   TypeSpec
     *     Identifier
     *       X
     *     Identifier
     *       int
     *   Identifier
     *     Y
     * @endcode
     * whereas
     * @code
     * X := Y
     * @endcode
     * becomes
     * @code
     * Definition
     *   Identifier
     *     X
     *   Identifier
     *     Y
     * @endcode
     */
    struct Definition : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Definition;

        Definition(const SLocus& Whence, const TNodeRef<Node>& Lhs, const TNodeRef<Node>& Rhs) : Node(StaticType, Whence)
        {
            _Children.Reserve(2);
            AppendChildInternal(Lhs);
            AppendChildInternal(Rhs);
        }

        // cloning
        Definition(const SLocus& Whence) : Node(StaticType, Whence)
        {
        }

        const TNodeRef<Node>& GetOperandLeft() const { return _Children[0]; }
        const TNodeRef<Node>& GetOperandRight() const { return _Children[1]; }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    /**
     * Corresponds to `set $1 $op $2` syntax.  The left-hand side will always be
     * a `Mutation` node.  `$op` may be `=`, `+=`, `-=`, `*=`, or `/=` -
     * importantly, not `:=`.  For example,
     * @code
     * set X = Y
     * @endcode
     * becomes
     * @code
     * Assignment
     *   Mutation
     *     Set
     *     Identifier
     *       X
     *   Identifier
     *     Y
     * @endcode
     */
    struct Assignment : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Assignment;
        enum class EOp : uint8_t { assign, addAssign, subAssign, mulAssign, divAssign };

        Assignment(const SLocus& Whence, const TNodeRef<Node>& lhs, EOp InOp, const TNodeRef<Node>& rhs) : Node(StaticType, Whence)
        {
            _Children.Reserve(2);
            AppendChildInternal(lhs);

            rhs->SetTag(static_cast<uint8_t>(InOp));
            AppendChildInternal(rhs);
        }

        // cloning
        Assignment(const SLocus& Whence) : Node(StaticType, Whence)
        {
        }

        const TNodeRef<Node>& GetOperandLeft() const { return _Children[0]; }
        const TNodeRef<Node>& GetOperandRight() const { return _Children[1]; }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    inline const char* AssignmentOpAsCString(Assignment::EOp Op)
    {
        switch(Op)
        {
        case Assignment::EOp::assign: return "assign";
        case Assignment::EOp::addAssign: return "addAssign";
        case Assignment::EOp::subAssign: return "subAssign";
        case Assignment::EOp::mulAssign: return "mulAssign";
        case Assignment::EOp::divAssign: return "divAssign";
        default: ULANG_UNREACHABLE();
        };
    }

    // Conditional flow control with failure context in test condition.
    //
    // Children are Clause block nodes in the following order:
    //   - if_identifier ]
    //   - condition      |- Repeating
    //   - [then_body]   ]
    //   - [else_body]   -- Optional last node
    //
    // If Identifier blocks - must be present and is simply an empty Clause meant to
    //     hold information about comments surrounding the 'if' identifier in the
    //     original source code
    // Condition blocks - must be present (they cannot be omitted - there must be at
    //     least 1) and they must have 1 or more expressions where 1 or more of the
    //     expressions can fail. Any local variables within it's top scope are made
    //     available to any immediately following then block.
    // Then blocks - are optional (they can be omitted) and when present must follow a
    //     conditional block and may have zero (can be empty) or more expressions.
    // Else block - is optional (it can be omitted) and when present must follow a
    //     conditional or then block (it cannot be the only block), must be the last
    //     block and it may have zero (can be empty) or more expressions.
    //
    // Chained `else if` are automatically flattened into a single FlowIf node with
    // multiple condition/[then] block pairs followed by an optional else block.
    //
    // An `if` may be used as an expression with a result if all the control flow paths
    // have a common result - i.e. it will have a result if every condition is paired
    // with a then block and there is an ending else block and all the then blocks and
    // else block have a most common result type.
    struct FlowIf : public Node
    {
        static const Vst::NodeType StaticType = NodeType::FlowIf;

        // Tags for different kinds of clause block children.
        enum class ClauseTag : uint8_t { if_identifier, condition, then_body, else_body };

        FlowIf(const SLocus& Whence) : Node(StaticType, Whence)  {}

        void AddIfIdentifier(const TNodeRef<Node>& Child) { Child->SetTag(static_cast<uint8_t>(ClauseTag::if_identifier)); AppendChildInternal(Child); }
        void AddCondition(const TNodeRef<Node>& Child)    { Child->SetTag(static_cast<uint8_t>(ClauseTag::condition)); AppendChildInternal(Child); }
        void AddBody(const TNodeRef<Node>& Child)         { Child->SetTag(static_cast<uint8_t>(ClauseTag::then_body)); AppendChildInternal(Child); }
        void AddElseBody(const TNodeRef<Node>& Child)     { Child->SetTag(static_cast<uint8_t>(ClauseTag::else_body)); AppendChildInternal(Child); }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct BinaryOpLogicalOr : public Node
    {
        static const Vst::NodeType StaticType = NodeType::BinaryOpLogicalOr;
        BinaryOpLogicalOr(const SLocus& Whence, const TNodeRef<Node>& Lhs, const TNodeRef<Node>& Rhs)
            : Node(StaticType, Whence)
        {
            _Children.Reserve(2);
            AppendChild(Lhs);
            AppendChild(Rhs);
        }

        // cloning
        BinaryOpLogicalOr(const SLocus& Whence)
            : Node(StaticType, Whence)
        {
        }

        void AppendChild(const TNodeRef<Node>& Rhs) { AppendChildInternal(Rhs); }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct BinaryOpLogicalAnd : public Node
    {
        static const Vst::NodeType StaticType = NodeType::BinaryOpLogicalAnd;
        BinaryOpLogicalAnd(const SLocus& Whence, const TNodeRef<Node>& Lhs, const TNodeRef<Node>& Rhs)
            : Node(StaticType, Whence)
        {
            _Children.Reserve(2);
            AppendChild(Lhs);
            AppendChild(Rhs);
        }

        // cloning
        BinaryOpLogicalAnd(const SLocus& Whence)
            : Node(StaticType, Whence)
        {
        }

        void AppendChild(const TNodeRef<Node>& Rhs) { AppendChildInternal(Rhs); }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct PrefixOpLogicalNot : public Node
    {
        static const Vst::NodeType StaticType = NodeType::PrefixOpLogicalNot;
        PrefixOpLogicalNot( const SLocus& Whence, const TNodeRef<Node>& Rhs)
            : Node(StaticType, Whence)
        {
            AppendChildInternal(Rhs);
        }

        // cloning
        PrefixOpLogicalNot(const SLocus& Whence)
            : Node(StaticType, Whence)
        {
        }

        const TNodeRef<Node>& GetInnerNode() const { return _Children[0]; }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct BinaryOpCompare : public Node
    {
        static const Vst::NodeType StaticType = NodeType::BinaryOpCompare;

        enum class op : uint8_t { lt, lteq, gt, gteq, eq, noteq };

        BinaryOpCompare(const SLocus& Whence, const TNodeRef<Node>& lhs, op in_op, const TNodeRef<Node>& rhs)
            : Node(StaticType, Whence)
        {
            _Children.Reserve(2);
            AppendChildInternal(lhs);
            AppendChildInternal(rhs);
            rhs->SetTag(static_cast<uint8_t>(in_op));
        }

        // cloning
        BinaryOpCompare(const SLocus& Whence)
            : Node(StaticType, Whence)
        {
        }

        TNodeRef<Node> GetOperandLeft() const { return _Children[0]; }
        TNodeRef<Node> GetOperandRight() const { return _Children[1]; }
        op GetOp() const { return _Children[1]->GetTag<op>(); }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    inline const char* BinaryCompareOpAsCString(BinaryOpCompare::op Op)
    {
        switch(Op)
        {
        case BinaryOpCompare::op::lt: return "lt";
        case BinaryOpCompare::op::lteq: return "lteq";
        case BinaryOpCompare::op::gt: return "gt";
        case BinaryOpCompare::op::gteq: return "gteq";
        case BinaryOpCompare::op::eq: return "eq";
        case BinaryOpCompare::op::noteq: return "noteq";
        default: ULANG_UNREACHABLE();
        };
    }

    inline BinaryOpCompare::op BinaryCompareOpFlip(BinaryOpCompare::op Op)
    {
        switch(Op)
        {
        case BinaryOpCompare::op::lt: return BinaryOpCompare::op::gt;
        case BinaryOpCompare::op::lteq: return BinaryOpCompare::op::gteq;
        case BinaryOpCompare::op::gt: return BinaryOpCompare::op::lt;
        case BinaryOpCompare::op::gteq: return BinaryOpCompare::op::lteq;
        case BinaryOpCompare::op::eq: return BinaryOpCompare::op::noteq;
        case BinaryOpCompare::op::noteq: return BinaryOpCompare::op::eq;
        default: ULANG_UNREACHABLE();
        }
    }

    struct Where : Node
    {
        static const NodeType StaticType = NodeType::Where;

        using RhsView = uLang::TRangeView<const TNodeRef<Node>*, const TNodeRef<Node>*>;

        Where(const SLocus& Whence, const TNodeRef<Node>& Lhs, const NodeArray& RhsArray)
            : Node(StaticType, Whence)
        {
            _Children.Reserve(1 + RhsArray.Num());
            AppendChildInternal(Lhs);
            for (const TNodeRef<Node>& Rhs : RhsArray)
            {
                AppendChildInternal(Rhs);
            }
        }

        // cloning
        Where(const SLocus& Whence)
            : Node(StaticType, Whence)
        {
        }

        TNodeRef<Node> GetLhs() const
        {
            return _Children[0];
        }

        void SetLhs(TNodeRef<Node> Lhs)
        {
            SetChildAt(0, Move(Lhs));
        }

        RhsView GetRhs() const
        {
            return {_Children.begin() + 1, _Children.end()};
        }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct Mutation : Node
    {
        static const NodeType StaticType = NodeType::Mutation;

        enum class EKeyword : uint8_t
        {
            Var,
            Set,
            Live
        };

        Mutation(const SLocus& Whence, const TNodeRef<Node>& Child, EKeyword Keyword, bool bLive)
            : Node(StaticType, Whence)
            , _Keyword(Keyword)
            , _bLive(bLive)
        {
            _Children.Reserve(1);
            AppendChildInternal(Child);
        }

        // cloning
        Mutation(const SLocus& Whence, EKeyword Keyword, bool bLive)
            : Node(StaticType, Whence)
            , _Keyword(Keyword)
            , _bLive(bLive)
        {
        }

        TNodeRef<Node> Child() const
        {
            return _Children[0];
        }

        EKeyword _Keyword;

        bool _bLive;

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct TypeSpec : public Node
    {
        static const Vst::NodeType StaticType = NodeType::TypeSpec;

        // comments that go after the ':' of the typespec
        NodeArray _TypeSpecComments;

        TypeSpec(const SLocus& Whence, const TNodeRef<Node>& Lhs, const TNodeRef<Node>& Rhs)
            : Node(NodeType::TypeSpec, Whence)
            , _TypeSpecComments()
        {
            _Children.Reserve(2);
            AppendChildInternal(Lhs);
            AppendChildInternal(Rhs);
        }

        TypeSpec(const SLocus& Whence, const TNodeRef<Node>& Rhs)
            : Node(NodeType::TypeSpec, Whence)
            , _TypeSpecComments()
        {
            _Children.Reserve(1);
            AppendChildInternal(Rhs);
        }

        // cloning
        TypeSpec(const SLocus& Whence)
            : Node(NodeType::TypeSpec, Whence)
            , _TypeSpecComments()
        {

        }

        bool HasLhs() const { return _Children.Num() == 2; }
        TNodeRef<Node> GetLhs() const { ULANG_ASSERTF(HasLhs(), "Lhs assumes we have at least two children."); return _Children[0]; }
        TNodeRef<Node> TakeLhs() { ULANG_ASSERTF(HasLhs(), "Lhs assumes we have at least two children."); return TakeChildAt(0, MakeStub(Whence())); }
        TNodeRef<Node> GetRhs() const { return _Children[_Children.Num()-1]; }
        TNodeRef<Node> TakeRhs() { return TakeChildAt(_Children.Num()-1, MakeStub(Whence())); }

        void AppendTypeSpecComment(const TNodeRef<Node>& NewComment)
        {
            NewComment->SetParentInternal(GetSharedSelf());
            _TypeSpecComments.Add(NewComment);
        }

        void AppendTypeSpecComment(TNodeRef<Node>&& NewComment)
        {
            NewComment->SetParentInternal(GetSharedSelf());
            _TypeSpecComments.Emplace(NewComment);
        }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
        void CloneNodeFields(TypeSpec* DestNode) const;
    };

    struct Identifier : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::Identifier;

        /// Comments that are to be suffixed to the qualifiers of this identifier.
        NodeArray _QualifierPostComments;

        /// Comments that are to be prefixed to the qualifiers of this identifier.
        NodeArray _QualifierPreComments;

        Identifier(const CUTF8StringView& InName, const SLocus& Whence, const bool bCanBeQualified)     
            : CAtom(InName, StaticType, Whence)
            , _QualifierPostComments()
            , _QualifierPreComments()
            , _bCanBeQualified(bCanBeQualified) {}
        Identifier(const CUTF8StringView& InName, const SLocus& Whence) 
            : CAtom(InName, StaticType, Whence)
            , _QualifierPostComments()
            , _QualifierPreComments()
            , _bCanBeQualified(true) {}

        void SetCanBeQualified(const bool bCanBeQualified) { _bCanBeQualified = bCanBeQualified; }
        bool CanBeQualified() const { return _bCanBeQualified; }
        bool IsQualified() const { return _Children.Num() > 0; }
        const TNodeRef<Node>& GetQualification() const { return _Children[0]; }

        /**
         * Adds a qualifier if un-qualified.
         *
         * @param InQualifier 	The text of the qualification to add.
         *
         * @return 			`true` if the qualification succeeded, or `false` if a qualifier was already present.
         */
        bool AddQualifier(const uLang::CUTF8StringView& InQualifier);

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
        void CloneNodeFields(Identifier* DestNode) const;

    private:
        bool _bCanBeQualified;
    };
    
    struct Operator : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::Operator;

        Operator(const CUTF8StringView& InSourceText, const SLocus& Whence)
            : CAtom(InSourceText, StaticType, Whence)
        {}
        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct BinaryOp : public Node
    {
        enum class op : uint8_t { Operator, Operand };

        BinaryOp(const SLocus& Whence, Vst::NodeType NodeType) : Node(NodeType, Whence) {}
        BinaryOp(const SLocus& Whence, const TNodeRef<Node>& LhsOperand, Vst::NodeType NodeType)
            : Node(NodeType, Whence)
        {
            LhsOperand->SetTag((uint8_t)op::Operand);
            AppendChildInternal(LhsOperand);
        }

        void AppendChild(op in_op, const TNodeRef<Node> in_child)
        {
            in_child->SetTag(static_cast<uint8_t>(in_op));
            AppendChildInternal(in_child);
        }

    protected:
        void AppendOperation_Internal(const TNodeRef<Node>& InOperator, const TNodeRef<Node>& Operand)
        {
            InOperator->SetTag((uint8_t)op::Operator);
            AppendChildInternal(InOperator);

            Operand->SetTag((uint8_t)op::Operand);
            AppendChildInternal(Operand);
        }
    };

    struct BinaryOpAddSub : public BinaryOp
    {
        static const Vst::NodeType StaticType = NodeType::BinaryOpAddSub;

        BinaryOpAddSub(const SLocus& Whence) : BinaryOp(Whence, StaticType) {}
        BinaryOpAddSub(const SLocus& Whence, const TNodeRef<Node>& LhsOperand)
            : BinaryOp(Whence, LhsOperand, StaticType)
        {
        }

        VERSECOMPILER_API void AppendAddOperation(const SLocus& AddWhence, const TNodeRef<Node>& RhsOperand);
        VERSECOMPILER_API void AppendSubOperation(const SLocus& SubWhence, const TNodeRef<Node>& RhsOperand);

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct BinaryOpMulDivInfix : public BinaryOp
    {
        static const Vst::NodeType StaticType = NodeType::BinaryOpMulDivInfix;

        BinaryOpMulDivInfix(const SLocus& Whence) : BinaryOp(Whence, StaticType) {}
        BinaryOpMulDivInfix(const SLocus& Whence, const TNodeRef<Node>& LhsOperand)
            : BinaryOp(Whence, LhsOperand, StaticType)
        {
        }

        void AppendInfixOperation(const TNodeRef<Identifier>& OpIdentifier, const TNodeRef<Node>& RhsOperand)
        {
            AppendOperation_Internal(OpIdentifier, RhsOperand);
        }

        VERSECOMPILER_API void AppendMulOperation(const SLocus& MulWhence, const TNodeRef<Node>& RhsOperand);
        VERSECOMPILER_API void AppendDivOperation(const SLocus& DivWhence, const TNodeRef<Node>& RhsOperand);

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct BinaryOpRange : public Node
    {
        static const Vst::NodeType StaticType = NodeType::BinaryOpRange;
        BinaryOpRange(const SLocus& Whence, const TNodeRef<Node>& Lhs, const TNodeRef<Node>& Rhs)
            : Node(StaticType, Whence)
        {
            _Children.Reserve(2);
            AppendChild(Lhs);
            AppendChild(Rhs);
        }

        // cloning
        BinaryOpRange(const SLocus& Whence)
            : Node(StaticType, Whence)
        {
        }

        void AppendChild(const TNodeRef<Node>& Rhs) { AppendChildInternal(Rhs); }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct BinaryOpArrow : public Node
    {
        static const Vst::NodeType StaticType = NodeType::BinaryOpArrow;
        BinaryOpArrow(const SLocus& Whence, const TNodeRef<Node>& Lhs, const TNodeRef<Node>& Rhs)
            : Node(StaticType, Whence)
        {
            _Children.Reserve(2);
            AppendChild(Lhs);
            AppendChild(Rhs);
        }

        // cloning
        BinaryOpArrow(const SLocus& Whence) : Node(StaticType, Whence)
        {
        }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct PrePostCall : public Node
    {
        static const Vst::NodeType StaticType = NodeType::PrePostCall;

        enum Op { Expression, Option, Pointer, DotIdentifier, SureCall, FailCall };

        PrePostCall(const TNodeRef<Node>& FirstChild, const SLocus& Whence) : Node(StaticType, Whence)
        {
            AppendChild(FirstChild);
        }

        PrePostCall(const SLocus& Whence) : Node(StaticType, Whence)
        {
        }

        bool IsSimpleCall() const
        {
            return (GetChildCount() == 2)
                && (GetChildren()[0]->GetTag<Op>() == Expression)
                && ((GetChildren()[1]->GetTag<Op>() == SureCall) || (GetChildren()[1]->GetTag<Op>() == FailCall));
        }

        bool IsPostHat() const
        {
            return (GetChildCount() == 2)
                && (GetChildren()[0]->GetTag<Op>() == Expression)
                && (GetChildren()[1]->GetTag<Op>() == Pointer);
        }

        bool IsPreQMark() const
        {
            return (GetChildCount() == 2)
                && (GetChildren()[0]->GetTag<Op>() == Option)
                && (GetChildren()[1]->GetTag<Op>() == Expression);
        }

        VERSECOMPILER_API
        TNodeRef<Clause> PrependQMark(const SLocus& Whence);

        VERSECOMPILER_API
        TNodeRef<Clause> PrependHat(const SLocus& Whence);

        VERSECOMPILER_API
        void PrependCallArgs(bool bCanFail, const TNodeRef<Clause>& Args);

        VERSECOMPILER_API
        void AppendQMark(const SLocus& Whence);

        VERSECOMPILER_API
        void AppendHat(const SLocus& Whence);

        VERSECOMPILER_API
        void AppendCallArgs(bool bCanFail, const TNodeRef<Clause>& Args);

        VERSECOMPILER_API
        void AppendDotIdent(const SLocus& Whence, const TNodeRef<Identifier>& Ident);

        VERSECOMPILER_API
        TNodePtr<Clause> TakeLastArgs();

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct IntLiteral : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::IntLiteral;

        IntLiteral(const CUTF8StringView& InSourceText, const SLocus& Whence) : CAtom(InSourceText, StaticType, Whence) {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct FloatLiteral : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::FloatLiteral;

        enum class EFormat
        {
            Unspecified,
            F16,
            F32,
            F64
        };

        EFormat _Format;

        FloatLiteral(const CUTF8StringView& InSourceText, EFormat Format, const SLocus& Whence)
            : CAtom(InSourceText, StaticType, Whence)
            , _Format(Format)
        {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct CharLiteral : public Node
    {
        static const Vst::NodeType StaticType = NodeType::CharLiteral;

        enum class EFormat
        {
            ASCII,             // 'a'
            UnicodeScalar,     // '😀'
            UTF8CodeUnit,      // 0oXX
            UnicodeScalarCode, // 0uXXXXX
            EscapedCode,       // \t \r ...
        };

        EFormat _Format;
        uint32_t _Value;

        CharLiteral(EFormat Format, char32_t Value, const SLocus& Whence)
            : Node(StaticType, Whence)
            , _Format(Format)
            , _Value(Value)
        {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;

        VERSECOMPILER_API CUTF8String ToString() const;
    };

    struct StringLiteral : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::StringLiteral;

        StringLiteral( const SLocus& Whence, const CUTF8StringView& SyntaxSource)
            : CAtom(SyntaxSource, StaticType, Whence)
        {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct PathLiteral : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::PathLiteral;

        PathLiteral(const CUTF8StringView& InPath, const SLocus& Whence) : CAtom(InPath, StaticType, Whence) {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct Interpolant : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::Interpolant;

        Interpolant(const SLocus& Whence, const CUTF8StringView& SyntaxSource)
            : CAtom(SyntaxSource, StaticType, Whence)
        {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct InterpolatedString : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::InterpolatedString;

        InterpolatedString(const SLocus& Whence, const CUTF8StringView& SyntaxSource)
            : CAtom(SyntaxSource, StaticType, Whence)
        {}

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct Lambda : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Lambda;

        Lambda(const SLocus& Whence, const TNodeRef<Node>& Domain, const TNodeRef<Node>& Range)
            : Node(StaticType, Whence)
        {
            _Children.Reserve(2);
            AppendChild(Domain);
            AppendChild(Range);
        }

        // cloning
        Lambda(const SLocus& Whence)
            : Node(StaticType, Whence)
        {
        }

        const TNodeRef<Node>& GetDomain() const { return _Children[0]; }
        const TNodeRef<Clause>& GetRange() const { return _Children[1].As<Clause>(); }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct Control : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Control;

        enum class EKeyword : uint8_t
        {
            Return,
            Break,
            Yield,
            Continue
        };

        Control(const SLocus& Whence, Control::EKeyword Keyword) : Node(StaticType, Whence), _Keyword(Keyword)
        {
        }

        Control(const TNodeRef<Node>& Child, const SLocus& Whence, EKeyword Keyword)
            : Control(Whence, Keyword)
        {
            AppendChild(Child);
        }

        const TNodeRef<Node>& GetReturnExpression() const { return _Children[0]; }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;

        EKeyword _Keyword;
    };

    using ClauseArray = LArray<TNodeRef<Clause>>;

    struct Macro : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Macro;

        Macro(SLocus Whence, const TNodeRef<Node>& MacroName, const uLang::TArray<TNodeRef<Clause>>& InChildren) : Node(StaticType, Whence)
        {
            _Children.Empty(InChildren.Num() + 1);
            AppendChildInternal(MacroName);
            for (const auto& Child : InChildren)
            {
                AppendChildInternal(Child);
            }
        }

        // cloning
        Macro(SLocus Whence) : Node(StaticType, Whence)
        {
        }

        const TNodeRef<Node>& GetName() const { return GetChildren()[0]; }
        const TNodeRef<Clause>& GetClause(int32_t ClauseIndex) const { return GetChildren()[ClauseIndex + 1].As<Clause>(); }
        TNodeRef<Clause> TakeClause(int32_t ClauseIndex, const TNodePtr<Node> Replacement = TNodePtr<Clause>())
        {
            return TakeChildAt(ClauseIndex + 1, Replacement).As<Clause>();
        }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;

        void ReplaceWith(const TNodeRef<Node>& MacroName, const TNodeRef<Clause>& Child) {
            _Children.Empty(2);
            AppendChildInternal(MacroName);
            AppendChildInternal(Child);
        }
    };

    struct Parens : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Parens;

        Parens(SLocus Whence, Clause::EForm Form, const NodeArray& InChildren = {})
            : Node(StaticType, Whence), _Form(Form)
        {
            _Children.Reserve(InChildren.Num());
            for (const auto& Child : InChildren)
            {
                AppendChild(Child);
            }
        }

        Clause::EForm GetForm() const { return _Form; }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;

    private:
        Clause::EForm _Form;
    };

    struct Commas : Node
    {
        static const Vst::NodeType StaticType = NodeType::Commas;

        Commas(SLocus Whence, const NodeArray& InChildren = {})
            : Node(StaticType, Whence)
        {
            AppendChildren(InChildren);
        }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct Placeholder : public CAtom
    {
        static const Vst::NodeType StaticType = NodeType::Placeholder;

        Placeholder(const SLocus& Whence) : CAtom("", StaticType, Whence) {};
        Placeholder(const uLang::CUTF8StringView& CodeStr, const SLocus& Whence) : CAtom(CodeStr, StaticType, Whence) {};

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    struct ParseError : public Node
    {
    public:
        static const Vst::NodeType StaticType = NodeType::ParseError;

        ParseError(const char* Error, const SLocus& Whence)
            : Node(NodeType::ParseError, Whence)
            , _Error(Error)
        {
        }

        const char* GetError() const { return _Error; }

        void AddChild(const TNodeRef<Node>& InnerError) { AppendChildInternal(InnerError); }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;

    private:
        const char* _Error;

    };

    struct Escape : public Node
    {
        static const Vst::NodeType StaticType = NodeType::Escape;

        Escape(const SLocus& Whence, const TNodeRef<Node>& Child)
            : Node(StaticType, Whence)
        {
            AppendChild(Child);
        }

        // cloning
        Escape(const SLocus& Whence)
            : Node(StaticType, Whence)
        {
        }

        VERSECOMPILER_API TNodeRef<Node> CloneNode() const override;
    };

    /**
     * WHY THIS DESIGN ?
     * -----------------
     *
     *
     * (1) Readability. All the pretty printing code is co-located. It is easier
     *      to work on and to understand.
     * (2) Design flexibility: Policy Pattern lets pipeline stages live outside
     *      of the node class.
     *
     *  e.g. The PrettyPrinter is easier to understand because you can read and
     *      step through the code without leaving a single file.
     *      The implementation can live in a Toolchain module.
     *      You can easily swap out to a different pretty printer implementation
     *      without touching the Vst nodes.
     *
     *  If changing this design (perhaps to improve performance), make sure
     *  to preserve both properties.
     *
     *
     *  NEEDS IMPROVEMENT
     * ------------------
     *
     *  When implementing a VisitPolicy, a programmer must implement every method for every type.
     *  This is not necessarily desirable. Especially true for certain semantic compiler passes.
     **/

    template <class VisitPolicy, typename ReturnType>
    void Node::VisitWith(const TNodeRef<Node>& node, VisitPolicy& visit_policy)
    {
        const Vst::NodeType node_type = node->GetElementType();
        switch (node_type)
        {
#define VISIT_VSTNODE(NodeName, RequiredChildren, SupportsManyChildren, Precedence, ChildDeletionBehavior, IsCAtom) \
    case Vst::NodeType::NodeName:                                                                                   \
        return visit_policy.visit(node->As<Vst::NodeName>());                                                       \
        break;
            VERSE_ENUM_VSTNODES(VISIT_VSTNODE)
#undef VISIT_VSTNODE
            default:
                return ReturnType();
                break;
        }
    }

    // Verify all Vst::Node::StaticType are valid
#define VISIT_VSTNODE(NodeName, RequiredChildren, SupportsManyChildren, Precedence, ChildDeletionBehavior, IsCAtom) static_assert(NodeName::StaticType == NodeType::NodeName, #NodeName "::StaticType must be Vst::NodeType::" #NodeName);
    VERSE_ENUM_VSTNODES(VISIT_VSTNODE)
#undef VISIT_VSTNODE

}  // namespace Vst

struct SPathToNode
{
    LArray<int32_t> Path;
    LArray<int32_t> AuxPath;

    int32_t PreCommentIndex;
    int32_t PostCommentIndex;
    // @sree @nicka This should be a UID into a Map of Snippets, or something. Anything but a ptr.
};

enum class EPrettyPrintBehaviour : uint32_t
{
    Default                          = 0,
    NewlinesAfterDefinitions         = 1 << 0,
    NewlinesAfterAttributes          = 1 << 1,
    NewlinesBetweenModuleMembers     = 1 << 2,
    UseVerticalFormForEnumerations   = 1 << 3
};
ULANG_ENUM_BIT_FLAGS(EPrettyPrintBehaviour, inline);

/**
 * Appends the text code version of this syntax snippet as compilable source. 
 * @param Source string to append to
 * @see VstSnippetAsCodeSourceChain(), VstSnippetAsCodeSource(), VstAsCodeSourceAppend()
 **/
VERSECOMPILER_API void VstPrintAppend(const Vst::TNodeRef<Vst::Node>& VstNode, CUTF8StringBuilder& Source);
inline CUTF8String PrintVst(const Vst::TNodeRef<Vst::Node>& VstNode)
{
    CUTF8StringBuilder Source; VstPrintAppend(VstNode, Source); return Source.MoveToString();
}

/**
 * Appends the text code version of this syntax snippet as closely as possible to how it
 * was originally authored and if it was not authored in text originally (such as added
 * via a VPL) then in as human readable canonical form as possible.
 *
 * @param Source string to append to
 * @see VstSnippetAsCodeSourceChain(), VstSnippetAsCodeSource(), VstAsCodeSourceAppend()
 **/
VERSECOMPILER_API void VstAsCodeSourceAppend(const Vst::TNodeRef<Vst::Node>& VstNode, CUTF8StringBuilder& Source);
VERSECOMPILER_API void VstAsCodeSourceAppend(const Vst::TNodeRef<Vst::Node>& VstNode, const EPrettyPrintBehaviour Flags, CUTF8StringBuilder& Source);
VERSECOMPILER_API void VstAsCodeSourceAppend(const Vst::TNodeRef<Vst::PrePostCall>& VstNode, CUTF8StringBuilder& OutSource, int32_t FirstChildIndex, int32_t LastChildIndex);
VERSECOMPILER_API void VstAsCodeSourceAppend(const Vst::TNodeRef<Vst::Clause>& VstClause, CUTF8StringBuilder& OutSource, int32_t InitialIndent, CUTF8String const& Separator);

inline CUTF8String PrettyPrintVst(const Vst::TNodeRef<Vst::Node>& VstNode)
{
    CUTF8StringBuilder Source; VstAsCodeSourceAppend(VstNode, Source); return Source.MoveToString();
}

inline CUTF8String PrettyPrintVst(const Vst::TNodeRef<Vst::Node>& VstNode, const EPrettyPrintBehaviour Flags)
{
    CUTF8StringBuilder Source; VstAsCodeSourceAppend(VstNode, Flags, Source); return Source.MoveToString();
}

inline CUTF8String PrettyPrintVst(const Vst::TNodeRef<Vst::PrePostCall>& VstNode, int32_t FirstChildIndex, int32_t LastChildIndex)
{
    CUTF8StringBuilder Source; VstAsCodeSourceAppend(VstNode, Source, FirstChildIndex, LastChildIndex); return Source.MoveToString();
}

inline CUTF8String PrettyPrintClause(const Vst::TNodeRef<Vst::Clause>& VstClause, int32_t InitialIndent, CUTF8String const& Separator)
{
    CUTF8StringBuilder OutSource;
    VstAsCodeSourceAppend(VstClause, OutSource, InitialIndent, Separator);
    return OutSource.MoveToString();
}

VERSECOMPILER_API bool GeneratePathToNode(const Vst::TNodeRef<Vst::Node>& Node, const Vst::TNodeRef<Vst::Snippet>& VstSnippet, SPathToNode& PathToNode);
VERSECOMPILER_API Vst::TNodePtr<Vst::Node> GetNodeFromPath(const Vst::TNodeRef<Vst::Snippet>& VstSnippet, const SPathToNode& Path, bool bReturnParent=false);

/** 
 * Gets the signed distance between a locus and a row/column text position. The closest distance to the locus range is used.
 * 
 * @param A 			The locus range to compare.
 * @param B 			The text position to compare.
 * @param SourceText 	The original source text that both the locus and position refer to ranges of.
 * 
 * @return 			The signed distance between the two. If A comes before B, the result is positive and vice versa.
 */
VERSECOMPILER_API int32_t GetSignedDistanceBetweenPositionAndLocus(const SLocus& A, const SPosition& B, const SIndexedSourceText& SourceText);

}  // namespace Verse

#undef UE_API
