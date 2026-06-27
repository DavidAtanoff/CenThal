// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "VerseVM/VVMObject.h"
#include "VerseVM/Inline/VVMObjectInline.h"
#include "VerseVM/VVMAccessor.h"
#include "VerseVM/VVMCppClassInfo.h"
#include "VerseVM/VVMDebuggerVisitor.h"

namespace Verse
{
DEFINE_DERIVED_VCPPCLASSINFO(VObject);
DEFINE_TRIVIAL_VISIT_REFERENCES(VObject);

FOpResult VObject::LoadField(FAllocationContext Context, VEmergentType& EmergentType, const VShape::VEntry* Field, VValue Self, FLoadFieldCacheCase* OutCacheCase)
{
	AutoRTFM::UnreachableIfClosed("#jira SOL-8415");
	V_DIE_IF(Field == nullptr);

	const VCppClassInfo& CppClassInfo = *EmergentType.CppClassInfo;

	switch (Field->Type)
	{
		case EFieldType::Offset:
		{
			VRestValue* Value = &GetFieldData(CppClassInfo)[Field->Index];
			if (OutCacheCase)
			{
				uint64 Offset = BitCast<char*>(Value) - BitCast<char*>(this);
				*OutCacheCase = FLoadFieldCacheCase::Offset(&EmergentType, Offset);
			}
			V_RETURN(Value->Get(Context));
		}
		case EFieldType::FProperty:
			if (IsMeltedNativeStruct())
			{
				// Wrap our return in a VNativeRef as we are a melted struct
				V_RETURN(VNativeRef::New(Context, this->DynamicCast<VNativeStruct>(), Field->UProperty));
			}
			return VNativeRef::Get(Context, GetData(CppClassInfo), Field->UProperty);
		case EFieldType::FPropertyVar:
			V_RETURN(VNativeRef::New(Context, this->DynamicCast<VNativeStruct>(), Field->UProperty));
		case EFieldType::FVerseProperty:
			V_RETURN(Field->UProperty->ContainerPtrToValuePtr<VRestValue>(GetData(CppClassInfo))->Get(Context));
		case EFieldType::Constant:
		{
			VValue FieldValue = Field->Value.Follow();

			// Self is passed in as a separate parameter because it may be a placeholder for a partially-constructed
			// native object. In this case, don't populate the inline cache- subsequent runs will use the wrong Self.
			if (Self.IsUninitialized())
			{
				Self = *this;
			}
			else
			{
				OutCacheCase = nullptr;
			}

			// Bind methods and accessors to Self- they are stored without it to enable more shape sharing.
			// Ignore function which are already bound, which are just fields of function type.
			if (VFunction* Function = FieldValue.DynamicCast<VFunction>(); Function && !Function->HasSelf())
			{
				if (OutCacheCase)
				{
					*OutCacheCase = FLoadFieldCacheCase::Function(&EmergentType, Function);
				}
				V_RETURN(Function->Bind(Context, Self));
			}
			if (VAccessor* Accessor = FieldValue.DynamicCast<VAccessor>())
			{
				if (OutCacheCase)
				{
					*OutCacheCase = FLoadFieldCacheCase::Accessor(&EmergentType, Accessor);
				}
				V_RETURN(VAccessorRef::New(Context, Self, *Accessor));
			}
			if (OutCacheCase)
			{
				*OutCacheCase = FLoadFieldCacheCase::Constant(&EmergentType, FieldValue);
			}
			V_RETURN(FieldValue);
		}
		default:
			VERSE_UNREACHABLE();
			break;
	}
}

void VObject::VisitMembersImpl(FAllocationContext Context, FDebuggerVisitor& Visitor)
{
	Visitor.VisitObject([this, Context, &Visitor] {
		VEmergentType* EmergentType = GetEmergentType();
		for (auto It = EmergentType->Shape->CreateFieldsIterator(); It; ++It)
		{
			Visitor.Visit(LoadField(Context, *EmergentType, &It->Value), It->Key->AsStringView());
		}
	});
}
} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
