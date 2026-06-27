// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMCreateFieldInlineCache.h"
#include "VerseVM/VVMEmergentType.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMRestValue.h"

namespace Verse
{
struct VClass;
struct VUniqueString;
struct VNativeStruct;

/*
  This is a wrapper around a native object for us to know what fields have been initialized or not in the object. Why?
  - For pure-Verse objects, we rely on our emergent type to track field initialization and can directly store placeholders for uninitialized fields.
  - Native objects don't have an emergent type and since the data lives in the `UObject` itself we can't directly store placeholders for uninitialized fields.

  To handle this, we wrap native objects in a `VNativeConstructorWrapper` object uses its emergent types bitmap of the fields
  created and placeholders for self/fields used before they are created. When we are done with archetype construction,
  we unify any placeholders then unwrap the native object using a special opcode and return it as part of `NewObject`.
  The wrapper object then gets GC'ed during the next collection.

  By convention, non-native Verse objects are not wrapped; the unwrap opcode just no-ops and returns the
  object itself when it encounters it (this is so we can avoid allocating an extra wrapper object in the common
  non-native case).

  Therefore, we always favour emitting the unwrap instruction where a native object needs to be unwrapped, since we
  don't know during codegen whether the object in question is native or not (since we do the wrapping in `NewObject` at
  runtime). Non-native objects can also be turned into native objects at any time (we test this as well using
  `--uobject-probability` in our tests.)
 */
struct VNativeConstructorWrapper : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	AUTORTFM_DISABLE COREUOBJECT_API void AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth);

	AUTORTFM_DISABLE COREUOBJECT_API static VNativeConstructorWrapper& New(FAllocationContext Context, VClass& Class, VNativeStruct& NativeStruct);
	AUTORTFM_DISABLE COREUOBJECT_API static VNativeConstructorWrapper& New(FAllocationContext Context, VClass& Class, UObject* NativeObject);
	AUTORTFM_DISABLE COREUOBJECT_API static VNativeConstructorWrapper& New(FAllocationContext Context, VClass& Class, VShape& Shape, UObject* NativeObject);

	AUTORTFM_DISABLE bool CreateFieldCached(FAllocationContext Context, FCreateFieldCacheCase Cache);
	AUTORTFM_DISABLE bool CreateField(FAllocationContext Context, VUniqueString& FieldName, FCreateFieldCacheCase* OutCacheCase = nullptr);

	bool IsFieldCreated(uint32 FieldIndex)
	{
		return GetEmergentType()->IsFieldCreated(FieldIndex);
	}

	/// If the field is uninitialized, return a placeholder for it.
	VValue LoadField(FAllocationContext Context, uint32 FieldIndex)
	{
		return Fields[FieldIndex].Get(Context);
	}

	/// Mark a field as initialized. If it was previously loaded, return its placeholder to be unified.
	VValue UnifyField(FAllocationContext Context, uint32 FieldIndex)
	{
		VRestValue& Field = Fields[FieldIndex];
		VValue Placeholder = !Field.CanDefQuickly() ? Field.Get(Context) : VValue();
		Field.SetNonCellNorPlaceholder(VValue());
		return Placeholder;
	}

	VValue WrappedObject() const;

	/// Placeholder for self that we pass to anything attempting to unwrap this before its fully created
	VRestValue SelfPlaceholder;

private:
	friend class FInterpreter;

	AUTORTFM_DISABLE VNativeConstructorWrapper(FAllocationContext Context, VClass& Class, VNativeStruct& NativeStruct, VShape* Shape, int32 NumFields);
	AUTORTFM_DISABLE VNativeConstructorWrapper(FAllocationContext Context, VClass& Class, UObject* NativeObject, VShape* Shape, int32 NumFields);

	/// This should either be a `VNativeStruct`/`UObject` wrapped in a `VValue`.
	TWriteBarrier<VValue> NativeObject;

	int32 NumFields;

	/// These are placeholders until the corresponding native field is initialized, and then they become VValue().
	// TODO: Consider a scheme that only allocates enough entries in Fields for non-Constants.
	VRestValue Fields[];
};

} // namespace Verse
#endif // WITH_VERSE_VM
