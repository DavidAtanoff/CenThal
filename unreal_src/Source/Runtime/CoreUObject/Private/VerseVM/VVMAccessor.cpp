// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "VerseVM/VVMAccessor.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/VVMCppClassInfo.h"
#include "VerseVM/VVMStructuredArchiveVisitor.h"

namespace Verse
{

DEFINE_DERIVED_VCPPCLASSINFO(VAccessor);
TGlobalTrivialEmergentTypePtr<&VAccessor::StaticCppClassInfo> VAccessor::GlobalTrivialEmergentType;

template <typename TVisitor>
void VAccessor::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(GetGettersBegin(), GetGettersEnd(), TEXT("Getters"));
	Visitor.Visit(GetSettersBegin(), GetSettersEnd(), TEXT("Setters"));
}

void VAccessor::SerializeLayout(FAllocationContext Context, VAccessor*& This, FStructuredArchiveVisitor& Visitor)
{
	uint32 NumAccessors = 0;
	if (!Visitor.IsLoading())
	{
		NumAccessors = This->NumAccessors;
	}
	Visitor.Visit(NumAccessors, TEXT("NumAccessors"));
	if (Visitor.IsLoading())
	{
		This = &VAccessor::NewUninitialized(Context, NumAccessors);
	}
}

void VAccessor::SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor)
{
	Visitor.Visit(GetGettersBegin(), GetGettersEnd(), TEXT("Getters"));
	Visitor.Visit(GetSettersBegin(), GetSettersEnd(), TEXT("Setters"));
}

DEFINE_DERIVED_VCPPCLASSINFO(VAccessorRef);
TGlobalTrivialEmergentTypePtr<&VAccessorRef::StaticCppClassInfo> VAccessorRef::GlobalTrivialEmergentType;

TGlobalHeapPtr<VEnumerator> VAccessorRef::AccessorEnum;

template <typename TVisitor>
void VAccessorRef::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(Base, TEXT("Base"));
	Visitor.Visit(Member, TEXT("Member"));
}

VAccessorRef& VAccessorRef::Flatten(FAllocationContext Context, TArray<TWriteBarrier<VValue>>& OutArguments, int32 Depth)
{
	if (VAccessorRef* Ref = Base.Get().DynamicCast<VAccessorRef>())
	{
		VAccessorRef& Field = Ref->Flatten(Context, OutArguments, Depth + 1);
		OutArguments.Emplace(Context, Member.Get());
		return Field;
	}
	else
	{
		OutArguments.Reserve(Depth + 1);
		OutArguments.Emplace(Context, *AccessorEnum);
		return *this;
	}
}

DEFINE_DERIVED_VCPPCLASSINFO(VAccessorSuspension);
TGlobalTrivialEmergentTypePtr<&VAccessorSuspension::StaticCppClassInfo> VAccessorSuspension::GlobalTrivialEmergentType;

template <typename TVisitor>
void VAccessorSuspension::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(Next, TEXT("Next"));
	Visitor.Visit(Prev, TEXT("Prev"));
	Visitor.Visit(Ref, TEXT("Ref"));
	Visitor.Visit(Value, TEXT("Value"));
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)