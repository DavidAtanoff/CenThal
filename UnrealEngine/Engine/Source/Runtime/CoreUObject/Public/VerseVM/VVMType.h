// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMToJsonCallback.h"
#include "VerseVM/VVMWriteBarrier.h"

#include <new>

namespace Verse
{
struct VCppClassInfo;

template <typename T>
struct TGlobalHeapPtr;

// Represents Verse types, which may be independent of object shape, and independent of C++ type.
struct VType : VHeapValue
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VHeapValue);

protected:
	COREUOBJECT_API explicit VType(FAllocationContext Context, VEmergentType* Type);
};

struct VTrivialType : VType
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VType);

	COREUOBJECT_API static TGlobalHeapPtr<VTrivialType> Singleton;

	AUTORTFM_DISABLE static void Initialize(FAllocationContext Context);

private:
	VTrivialType(FAllocationContext Context);
};

#define DECLARE_PRIMITIVE_TYPE_BODY(Name)                                                                                                                                                                             \
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VType);                                                                                                                                                            \
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;                                                                                                              \
                                                                                                                                                                                                                      \
	COREUOBJECT_API static TGlobalHeapPtr<V##Name##Type> Singleton;                                                                                                                                                   \
                                                                                                                                                                                                                      \
	static constexpr bool SerializeIdentity = false;                                                                                                                                                                  \
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, V##Name##Type*& This, FStructuredArchiveVisitor& Visitor);                                                                               \
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);                                                                                                              \
                                                                                                                                                                                                                      \
	AUTORTFM_DISABLE static void Initialize(FAllocationContext Context);                                                                                                                                              \
                                                                                                                                                                                                                      \
	AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSONImpl(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs); \
	AUTORTFM_DISABLE COREUOBJECT_API VValue FromJSONImpl(FRunningContext, const FJsonValue&, EValueJSONFormat, FFromJsonCallback);                                                                                    \
                                                                                                                                                                                                                      \
private:                                                                                                                                                                                                              \
	AUTORTFM_DISABLE V##Name##Type(FAllocationContext Context)

#define DECLARE_PRIMITIVE_TYPE(Name)       \
	struct V##Name##Type : VType           \
	{                                      \
		DECLARE_PRIMITIVE_TYPE_BODY(Name); \
	}

#define DECLARE_STRUCTURAL_TYPE(Name, Fields)                                                                                                                                                                             \
	struct V##Name##Type : VType                                                                                                                                                                                          \
	{                                                                                                                                                                                                                     \
		DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VType);                                                                                                                                                            \
		COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;                                                                                                              \
                                                                                                                                                                                                                          \
		TWriteBarrier<VValue> Fields(DECLARE_STRUCTURAL_TYPE_FIELD);                                                                                                                                                      \
                                                                                                                                                                                                                          \
		AUTORTFM_DISABLE static V##Name##Type& New(FAllocationContext Context, Fields(DECLARE_STRUCTURAL_TYPE_PARAM))                                                                                                     \
		{                                                                                                                                                                                                                 \
			return *new (Context.AllocateFastCell(sizeof(V##Name##Type))) V##Name##Type(Context, Fields(NAME_STRUCTURAL_TYPE_PARAM));                                                                                     \
		}                                                                                                                                                                                                                 \
                                                                                                                                                                                                                          \
		static constexpr bool SerializeIdentity = false;                                                                                                                                                                  \
		AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, V##Name##Type*& This, FStructuredArchiveVisitor& Visitor);                                                                               \
		AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);                                                                                                              \
                                                                                                                                                                                                                          \
		AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSONImpl(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs); \
		AUTORTFM_DISABLE COREUOBJECT_API VValue FromJSONImpl(FRunningContext, const FJsonValue&, EValueJSONFormat, FFromJsonCallback);                                                                                    \
                                                                                                                                                                                                                          \
	protected:                                                                                                                                                                                                            \
		V##Name##Type(FAllocationContext& Context, Fields(DECLARE_STRUCTURAL_TYPE_PARAM), VEmergentType* EmergentType)                                                                                                    \
			: VType(Context, EmergentType)                                                                                                                                                                                \
			, Fields(INIT_STRUCTURAL_TYPE_FIELD)                                                                                                                                                                          \
		{                                                                                                                                                                                                                 \
		}                                                                                                                                                                                                                 \
                                                                                                                                                                                                                          \
	private:                                                                                                                                                                                                              \
		V##Name##Type(FAllocationContext& Context, Fields(DECLARE_STRUCTURAL_TYPE_PARAM))                                                                                                                                 \
			: V##Name##Type(Context, Fields(NAME_STRUCTURAL_TYPE_PARAM), &GlobalTrivialEmergentType.Get(Context))                                                                                                         \
		{                                                                                                                                                                                                                 \
		}                                                                                                                                                                                                                 \
	};

#define DECLARE_STRUCTURAL_TYPE_FIELD(Name) Name
#define DECLARE_STRUCTURAL_TYPE_PARAM(Name) VValue In##Name
#define NAME_STRUCTURAL_TYPE_PARAM(Name) In##Name
#define INIT_STRUCTURAL_TYPE_FIELD(Name) Name(Context, In##Name)

struct VAnyType : VType
{
	DECLARE_PRIMITIVE_TYPE_BODY(Any);

public:
	bool SubsumesImpl(FAllocationContext, VValue);
};

DECLARE_PRIMITIVE_TYPE(Void);
DECLARE_PRIMITIVE_TYPE(Comparable);
DECLARE_PRIMITIVE_TYPE(Logic);
DECLARE_PRIMITIVE_TYPE(Rational);
DECLARE_PRIMITIVE_TYPE(Char8);
DECLARE_PRIMITIVE_TYPE(Char32);
DECLARE_PRIMITIVE_TYPE(Range);

#define TYPE_FIELDS(Field) Field(PositiveType)
DECLARE_STRUCTURAL_TYPE(Type, TYPE_FIELDS)
#undef TYPE_FIELDS

#define TYPE_FIELDS(Field) Field(ElementType)
DECLARE_STRUCTURAL_TYPE(Array, TYPE_FIELDS)
#undef TYPE_FIELDS

#define TYPE_FIELDS(Field) Field(ElementType)
DECLARE_STRUCTURAL_TYPE(Generator, TYPE_FIELDS)
#undef TYPE_FIELDS

#define TYPE_FIELDS(Field) Field(KeyType), Field(ValueType)
DECLARE_STRUCTURAL_TYPE(WeakMap, TYPE_FIELDS)
#undef TYPE_FIELDS

#define TYPE_FIELDS(Field) Field(ValueType)
DECLARE_STRUCTURAL_TYPE(Pointer, TYPE_FIELDS)
#undef TYPE_FIELDS

DECLARE_PRIMITIVE_TYPE(Reference);

#define TYPE_FIELDS(Field) Field(ValueType)
DECLARE_STRUCTURAL_TYPE(Option, TYPE_FIELDS)
#undef TYPE_FIELDS

#define TYPE_FIELDS(Field) Field(SuperType)
DECLARE_STRUCTURAL_TYPE(Concrete, TYPE_FIELDS)
#undef TYPE_FIELDS

#define TYPE_FIELDS(Field) Field(SuperType)
DECLARE_STRUCTURAL_TYPE(Castable, TYPE_FIELDS)
#undef TYPE_FIELDS

DECLARE_PRIMITIVE_TYPE(Function);
DECLARE_PRIMITIVE_TYPE(Persistable);

#undef DECLARE_PRIMITIVE_TYPE
#undef DECLARE_STRUCTURAL_TYPE
#undef DECLARE_STRUCTURAL_TYPE_FIELD
#undef DECLARE_STRUCTURAL_TYPE_PARAM
#undef NAME_STRUCTURAL_TYPE_PARAM
#undef INIT_STRUCTURAL_TYPE_FIELD

struct VMapType : VWeakMapType
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VWeakMapType);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	static VMapType& New(FAllocationContext Context, VValue KeyType, VValue ValueType)
	{
		return *new (Context.AllocateFastCell(sizeof(VMapType))) VMapType(Context, KeyType, ValueType);
	}

	static constexpr bool SerializeIdentity = false;
	AUTORTFM_DISABLE static void SerializeLayout(FAllocationContext Context, VMapType*& This, FStructuredArchiveVisitor& Visitor);
	AUTORTFM_DISABLE void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

	AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSONImpl(FRunningContext, EValueJSONFormat, TMap<const void*, EVisitState>& VisitedObjects, FToJsonCallback, uint32 RecursionDepth, FJsonObject* Defs);
	AUTORTFM_DISABLE COREUOBJECT_API VValue FromJSONImpl(FRunningContext, const FJsonValue&, EValueJSONFormat, FFromJsonCallback);

private:
	VMapType(FAllocationContext& Context, VValue KeyType, VValue ValueType)
		: VWeakMapType(Context, KeyType, ValueType, &GlobalTrivialEmergentType.Get(Context))
	{
	}
};

} // namespace Verse
#endif // WITH_VERSE_VM
