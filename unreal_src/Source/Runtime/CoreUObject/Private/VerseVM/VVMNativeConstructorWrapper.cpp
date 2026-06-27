// Copyright Epic Games, Inc. All Rights Reserved.

#if !WITH_VERSE_BPVM || defined(__INTELLISENSE__)

#include "VerseVM/VVMNativeConstructorWrapper.h"
#include "VerseVM/Inline/VVMMapInline.h"
#include "VerseVM/Inline/VVMNativeConstructorWrapperInline.h"
#include "VerseVM/Inline/VVMShapeInline.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMValuePrinting.h"
#include "VerseVM/VVMVerseClass.h"

namespace Verse
{
DEFINE_DERIVED_VCPPCLASSINFO(VNativeConstructorWrapper);

template <typename TVisitor>
void VNativeConstructorWrapper::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(NativeObject, TEXT("NativeObject"));
	Visitor.Visit(SelfPlaceholder, TEXT("SelfPlaceholder"));
	Visitor.Visit(Fields, NumFields, TEXT("Fields"));
}

VNativeConstructorWrapper& VNativeConstructorWrapper::New(FAllocationContext Context, VClass& Class, VNativeStruct& NativeStruct)
{
	VShape* Shape = NativeStruct.GetEmergentType()->Shape.Get();
	int32 NumFields = Shape ? Shape->GetMaxFieldIndex() : 0;
	return *new (Context.AllocateFastCell(offsetof(VNativeConstructorWrapper, Fields) + sizeof(VRestValue) * NumFields)) VNativeConstructorWrapper(Context, Class, NativeStruct, Shape, NumFields);
}

VNativeConstructorWrapper& VNativeConstructorWrapper::New(FAllocationContext Context, VClass& Class, UObject* NativeObject)
{
	VShape* Shape = UVerseClass::GetShape(Context, NativeObject->GetClass());
	V_DIE_UNLESS(Shape);
	return VNativeConstructorWrapper::New(Context, Class, *Shape, NativeObject);
}

VNativeConstructorWrapper& VNativeConstructorWrapper::New(FAllocationContext Context, VClass& Class, VShape& Shape, UObject* NativeObject)
{
	int32 NumFields = Shape.GetMaxFieldIndex();
	return *new (Context.AllocateFastCell(offsetof(VNativeConstructorWrapper, Fields) + sizeof(VRestValue) * NumFields)) VNativeConstructorWrapper(Context, Class, NativeObject, &Shape, NumFields);
}

VNativeConstructorWrapper::VNativeConstructorWrapper(FAllocationContext Context, VClass& Class, VNativeStruct& NativeStruct, VShape* Shape, int32 NumFields)
	: VCell(Context, &Class.GetOrCreateEmergentTypeForNativeConstructorWrapper(Context))
	, SelfPlaceholder(0)
	, NativeObject(Context, NativeStruct)
	, NumFields(NumFields)
{
	for (int32 FieldIndex = 0; FieldIndex < NumFields; ++FieldIndex)
	{
		// TODO SOL-4222: Pipe through proper split depth here.
		new (&Fields[FieldIndex]) VRestValue(0);
	}

	if (Shape)
	{
		for (auto It = Shape->Fields.CreateIterator(); It; ++It)
		{
			if (!It->Value.IsProperty())
			{
				int32 FieldIndex = It.GetId().AsInteger();
				Fields[FieldIndex].SetNonCellNorPlaceholder(VValue());
			}
		}
	}
}

VNativeConstructorWrapper::VNativeConstructorWrapper(FAllocationContext Context, VClass& Class, UObject* NativeObject, VShape* Shape, int32 NumFields)
	: VCell(Context, &Class.GetOrCreateEmergentTypeForNativeConstructorWrapper(Context))
	, SelfPlaceholder(0)
	, NativeObject(Context, NativeObject)
	, NumFields(NumFields)
{
	for (int32 FieldIndex = 0; FieldIndex < NumFields; ++FieldIndex)
	{
		// TODO SOL-4222: Pipe through proper split depth here.
		new (&Fields[FieldIndex]) VRestValue(0);
	}

	for (auto It = Shape->Fields.CreateIterator(); It; ++It)
	{
		if (!It->Value.IsProperty())
		{
			int32 FieldIndex = It.GetId().AsInteger();
			Fields[FieldIndex].SetNonCellNorPlaceholder(VValue());
		}
	}
}
} // namespace Verse
#endif
