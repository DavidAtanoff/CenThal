// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Containers/StringFwd.h"
#include "HAL/Platform.h"
#include "Templates/Function.h"
#include "UObject/ReferenceToken.h"
#include "VerseVM/VVMFromJsonCallback.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMToJsonCallback.h"
#include "VerseVM/VVMVerse.h"

#include <type_traits>

class FJsonValue;

namespace Verse
{
enum class ECompares : uint8;
enum class EValueStringFormat;
enum class EValueJSONFormat;
enum class EVisitState;
struct FAbstractVisitor;
struct FAllocationContext;
struct FDebuggerVisitor;
struct FMarkStackVisitor;
struct FOp;
struct FRunningContext;
struct FStructuredArchiveVisitor;
struct VCell;
struct VTask;
struct VType;
struct VValue;

// MSVC and clang-cl have a non-portable __super that can be used to validate the user's super-class declaration.
#if defined(_MSC_VER)
#define VCPPCLASSINFO_PORTABLE_MSC_SUPER         \
	/* disable non-standard extension warning */ \
	__pragma(warning(push)) __pragma(warning(disable : 4495)) __super __pragma(warning(pop))
#else
#define VCPPCLASSINFO_PORTABLE_MSC_SUPER Super
#endif

#define DECLARE_BASE_VCPPCLASSINFO(API)                                                    \
protected:                                                                                 \
	auto CheckSuperClass()                                                                 \
	{                                                                                      \
		return this;                                                                       \
	}                                                                                      \
                                                                                           \
	API void VisitInheritedAndNonInheritedReferences(::Verse::FAbstractVisitor& Visitor);  \
	API void VisitInheritedAndNonInheritedReferences(::Verse::FMarkStackVisitor& Visitor); \
                                                                                           \
public:                                                                                    \
	API static ::Verse::VCppClassInfo StaticCppClassInfo;

#define DECLARE_DERIVED_VCPPCLASSINFO(API, SuperClass)                                     \
private:                                                                                   \
	template <typename TVisitor>                                                           \
	void VisitReferencesImpl(TVisitor&); /* to be implemented by the user. */              \
                                                                                           \
protected:                                                                                 \
	auto CheckSuperClass()                                                                 \
	{                                                                                      \
		auto SuperThis = VCPPCLASSINFO_PORTABLE_MSC_SUPER::CheckSuperClass();              \
		static_assert(std::is_same_v<decltype(SuperThis), SuperClass*>,                    \
			"Declared super-class " #SuperClass " does not match actual super-class.");    \
		return this;                                                                       \
	}                                                                                      \
                                                                                           \
	API void VisitInheritedAndNonInheritedReferences(::Verse::FAbstractVisitor& Visitor);  \
	API void VisitInheritedAndNonInheritedReferences(::Verse::FMarkStackVisitor& Visitor); \
                                                                                           \
public:                                                                                    \
	using Super = SuperClass;                                                              \
	API static ::Verse::VCppClassInfo StaticCppClassInfo;

#define DEFINE_BASE_OR_DERIVED_VCPPCLASSINFO(CellType, SuperClassInfoPtr)                                                                                                                                                                                                               \
	::Verse::VCppClassInfo CellType::StaticCppClassInfo = {                                                                                                                                                                                                                             \
		TEXT(#CellType),                                                                                                                                                                                                                                                                \
		(SuperClassInfoPtr),                                                                                                                                                                                                                                                            \
		sizeof(CellType),                                                                                                                                                                                                                                                               \
		[] AUTORTFM_DISABLE(::Verse::VCell * This, ::Verse::FMarkStackVisitor & Visitor) -> void {                                                                                                                                                                                      \
			This->StaticCast<CellType>().VisitInheritedAndNonInheritedReferences(Visitor);                                                                                                                                                                                              \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::VCell* This, ::Verse::FAbstractVisitor& Visitor) -> void {                                                                                                                                                                                         \
			::Verse::FAbstractVisitor::FReferrerContext Context(Visitor, FReferenceToken(This));                                                                                                                                                                                        \
			This->StaticCast<CellType>().VisitInheritedAndNonInheritedReferences(Visitor);                                                                                                                                                                                              \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::VCell* This) -> void {                                                                                                                                                                                                                             \
			This->StaticCast<CellType>().ConductCensusImpl();                                                                                                                                                                                                                           \
		},                                                                                                                                                                                                                                                                              \
		std::is_trivially_destructible_v<CellType> ? nullptr : [] AUTORTFM_DISABLE(::Verse::VCell* This) -> void {                                                                                                                                                                      \
			This->StaticCast<CellType>().~CellType();                                                                                                                                                                                                                                   \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FAllocationContext Context, ::Verse::VCell* This, ::Verse::VCell* Other, const TFunction<void(::Verse::VValue, ::Verse::VValue)>& HandlePlaceholder) -> ::Verse::ECompares {                                                                       \
			return This->StaticCast<CellType>().EqualImpl(Context, Other, HandlePlaceholder);                                                                                                                                                                                           \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::VCell* This) -> uint32 {                                                                                                                                                                                                                           \
			return This->StaticCast<CellType>().GetTypeHashImpl();                                                                                                                                                                                                                      \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FAllocationContext Context, ::Verse::VCell* This) -> ::Verse::VValue {                                                                                                                                                                             \
			return This->StaticCast<CellType>().MeltImpl(Context);                                                                                                                                                                                                                      \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FAllocationContext Context, ::Verse::VCell* This, ::Verse::VTask* Task) -> ::Verse::FOpResult {                                                                                                                                                    \
			return This->StaticCast<CellType>().FreezeImpl(Context, Task);                                                                                                                                                                                                              \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FAllocationContext Context, ::Verse::VCell* This, ::Verse::VValue InputValue) -> bool {                                                                                                                                                            \
			return This->StaticCast<CellType>().SubsumesImpl(Context, InputValue);                                                                                                                                                                                                      \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FAllocationContext Context, ::Verse::VCell* This, ::Verse::FDebuggerVisitor& Visitor) -> void {                                                                                                                                                    \
			This->StaticCast<CellType>().VisitMembersImpl(Context, Visitor);                                                                                                                                                                                                            \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FAllocationContext Context, ::Verse::VCell* This, FUtf8StringBuilderBase& Builder, ::Verse::EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth) -> void {                                                         \
			This->StaticCast<CellType>().AppendToStringImpl(Context, Builder, Format, VisitedObjects, RecursionDepth);                                                                                                                                                                  \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FRunningContext Context, ::Verse::VCell* This, ::Verse::EValueJSONFormat Format, TMap<const void*, ::Verse::EVisitState>& VisitedObjects, ::Verse::FToJsonCallback Callback, uint32 RecursionDepth, FJsonObject* Defs) -> TSharedPtr<FJsonValue> { \
			return This->StaticCast<CellType>().ToJSONImpl(Context, Format, VisitedObjects, Callback, RecursionDepth, Defs);                                                                                                                                                            \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FRunningContext Context, ::Verse::VCell* This, const FJsonValue& JsonValue, ::Verse::EValueJSONFormat Format, ::Verse::FFromJsonCallback Callback) -> ::Verse::VValue {                                                                            \
			return This->StaticCast<CellType>().FromJSONImpl(Context, JsonValue, Format, Callback);                                                                                                                                                                                     \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FAllocationContext Context, ::Verse::VCell*& This, ::Verse::FStructuredArchiveVisitor& Visitor) -> void {                                                                                                                                          \
			CellType* Scratch = This != nullptr ? &This->StaticCast<CellType>() : nullptr;                                                                                                                                                                                              \
			CellType::SerializeLayout(Context, Scratch, Visitor);                                                                                                                                                                                                                       \
			This = Scratch;                                                                                                                                                                                                                                                             \
		},                                                                                                                                                                                                                                                                              \
		[] AUTORTFM_DISABLE(::Verse::FAllocationContext Context, ::Verse::VCell* This, ::Verse::FStructuredArchiveVisitor& Visitor) -> void {                                                                                                                                           \
			This->StaticCast<CellType>().SerializeImpl(Context, Visitor);                                                                                                                                                                                                               \
		},                                                                                                                                                                                                                                                                              \
		CellType::SerializeIdentity,                                                                                                                                                                                                                                                    \
		CellType::InstancedCell};                                                                                                                                                                                                                                                       \
	::Verse::VCppClassInfoRegister CellType##_Register(&CellType::StaticCppClassInfo);

#define DEFINE_BASE_VCPPCLASSINFO(CellType)                                                     \
	DEFINE_BASE_OR_DERIVED_VCPPCLASSINFO(CellType, nullptr)                                     \
	void CellType::VisitInheritedAndNonInheritedReferences(::Verse::FAbstractVisitor& Visitor)  \
	{                                                                                           \
		AutoRTFM::UnreachableIfClosed("#jira SOL-8416");                                        \
		VisitReferencesImpl(Visitor);                                                           \
	}                                                                                           \
                                                                                                \
	void CellType::VisitInheritedAndNonInheritedReferences(::Verse::FMarkStackVisitor& Visitor) \
	{                                                                                           \
		AutoRTFM::UnreachableIfClosed("#jira SOL-8416");                                        \
		VisitReferencesImpl(Visitor);                                                           \
	}

#define DEFINE_DERIVED_VCPPCLASSINFO(CellType)                                                                                                                              \
	static_assert(!std::is_same_v<CellType::Super, CellType>, #CellType " declares itself as its super-class in DECLARE_DERIVED_VCPPCLASSINFO.");                           \
	static_assert(std::is_base_of_v<CellType::Super, CellType>, #CellType " doesn't derive from the super-class declared by DECLARE_DERIVED_VCPPCLASSINFO.");               \
	static_assert(std::is_base_of_v<::Verse::VCell, CellType::Super>, #CellType "'s super-class as declared by DECLARE_DERIVED_VCPPCLASSINFO does not derive from VCell."); \
	static_assert(!std::is_polymorphic_v<CellType>, "VCell-derived C++ classes must not have virtual methods.");                                                            \
	DEFINE_BASE_OR_DERIVED_VCPPCLASSINFO(CellType, &CellType::Super::StaticCppClassInfo)                                                                                    \
	void CellType::VisitInheritedAndNonInheritedReferences(::Verse::FAbstractVisitor& Visitor)                                                                              \
	{                                                                                                                                                                       \
		AutoRTFM::UnreachableIfClosed("#jira SOL-8416");                                                                                                                    \
		Super::VisitInheritedAndNonInheritedReferences(Visitor);                                                                                                            \
		VisitReferencesImpl(Visitor);                                                                                                                                       \
	}                                                                                                                                                                       \
                                                                                                                                                                            \
	void CellType::VisitInheritedAndNonInheritedReferences(::Verse::FMarkStackVisitor& Visitor)                                                                             \
	{                                                                                                                                                                       \
		AutoRTFM::UnreachableIfClosed("#jira SOL-8416");                                                                                                                    \
		Super::VisitInheritedAndNonInheritedReferences(Visitor);                                                                                                            \
		VisitReferencesImpl(Visitor);                                                                                                                                       \
	}

#define DEFINE_TRIVIAL_VISIT_REFERENCES(CellType)        \
	template <typename TVisitor>                         \
	void CellType::VisitReferencesImpl(TVisitor&)        \
	{                                                    \
		AutoRTFM::UnreachableIfClosed("#jira SOL-8416"); \
	}

// C++ information; this is where the "vtable" goes.
struct VCppClassInfo
{
	const TCHAR* Name;
	VCppClassInfo* SuperClass;
	size_t SizeWithoutFields;
	void (*MarkReferencesImpl)(VCell* This, FMarkStackVisitor&);
	void (*VisitReferencesImpl)(VCell* This, FAbstractVisitor&);
	void (*ConductCensus)(VCell* This);
	void (*RunDestructor)(VCell* This);
	ECompares (*Equal)(FAllocationContext Context, VCell* This, VCell* Other, const TFunction<void(::Verse::VValue, ::Verse::VValue)>& HandlePlaceholder);
	uint32 (*GetTypeHash)(VCell* This);
	VValue (*Melt)(FAllocationContext Context, VCell* This);
	FOpResult (*Freeze)(FAllocationContext Context, VCell* This, VTask*);
	bool (*Subsumes)(FAllocationContext Context, VCell* This, VValue);
	void (*VisitMembers)(FAllocationContext Context, VCell* This, FDebuggerVisitor& Visitor);
	void (*AppendToString)(FAllocationContext Context, VCell* This, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);
	TSharedPtr<FJsonValue> (*ToJSON)(FRunningContext, VCell* This, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs);
	VValue (*FromJSON)(FRunningContext, VCell* This, const FJsonValue&, EValueJSONFormat, FFromJsonCallback);
	void (*SerializeLayout)(FAllocationContext Context, VCell*& This, FStructuredArchiveVisitor& Visitor);
	void (*Serialize)(FAllocationContext Context, VCell* This, FStructuredArchiveVisitor& Visitor);
	bool SerializeIdentity;
	bool InstancedCell;

	bool IsA(const VCppClassInfo* Other) const
	{
		for (const VCppClassInfo* Current = this; Current; Current = Current->SuperClass)
		{
			if (Current == Other)
			{
				return true;
			}
		}
		return false;
	}

	V_FORCEINLINE void VisitReferences(VCell* This, FMarkStackVisitor& Visitor)
	{
		MarkReferencesImpl(This, Visitor);
	}

	V_FORCEINLINE void VisitReferences(VCell* This, FAbstractVisitor& Visitor)
	{
		VisitReferencesImpl(This, Visitor);
	}

	COREUOBJECT_API FString DebugName() const;
};

struct VCppClassInfoRegister
{
	VCppClassInfo* CppClassInfo;
	VCppClassInfoRegister* Next;

	COREUOBJECT_API VCppClassInfoRegister(VCppClassInfo* InCppClassInfo);
	COREUOBJECT_API ~VCppClassInfoRegister();
};

struct VCppClassInfoRegistry
{
	COREUOBJECT_API static VCppClassInfo* GetCppClassInfo(FStringView Name);
};

// Extension point for VisitReferencesImpl and F*Visitor method instantiations
template <typename VisitorType, typename ValueType>
void Visit(VisitorType& Visitor, ValueType& Value) = delete;

template <typename VisitorType, typename KeyType, typename ValueType>
void Visit(VisitorType& Visitor, TPair<KeyType, ValueType>& Value)
{
	Visitor.Visit(Value.Key, TEXT("Key"));
	Visitor.Visit(Value.Value, TEXT("Value"));
}

} // namespace Verse
#endif // WITH_VERSE_VM
