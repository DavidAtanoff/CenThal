// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "VerseVM/VVMFrame.h"
#include "Misc/StringBuilder.h"
#include "VerseVM/Inline/VVMCellInline.h"
#include "VerseVM/Inline/VVMMarkStackVisitorInline.h"
#include "VerseVM/VVMCppClassInfo.h"
#include "VerseVM/VVMLocation.h"
#include "VerseVM/VVMProcedure.h"
#include "VerseVM/VVMUniqueString.h"

namespace Verse
{

DEFINE_DERIVED_VCPPCLASSINFO(VFrame);
TGlobalTrivialEmergentTypePtr<&VFrame::StaticCppClassInfo> VFrame::GlobalTrivialEmergentType;

void VFrame::GetStackDescription(FStringBuilderBase& StringBuilder, const FOp* PC) const
{
	if (Procedure)
	{
		StringBuilder << TEXT("\t");
		if (const VUniqueString* FilePath = Procedure->FilePath.Get(); FilePath && !FilePath->AsStringView().IsEmpty())
		{
			StringBuilder << FilePath->AsStringView();
		}
		if (const VUniqueString* Name = Procedure->Name.Get(); Name && !Name->AsStringView().IsEmpty())
		{
			StringBuilder << TEXT(" ") << Name->AsStringView();
		}
		if (const FLocation* Loc = PC ? Procedure->GetLocation(*PC) : nullptr)
		{
			StringBuilder << TEXT(":") << Loc->Line;
		}
		StringBuilder << TEXT("\n");
	}
}

template <typename TVisitor>
void VFrame::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(CallerFrame, TEXT("CallerFrame"));
	Visitor.Visit(ReturnSlot, TEXT("ReturnSlot"));
	Visitor.Visit(Procedure, TEXT("Procedure"));
	Visitor.Visit(Registers, NumRegisters, TEXT("Registers"));
}

TGlobalHeapPtr<VFrame> VFrame::GlobalEmptyFrame;

void VFrame::InitializeGlobals(FAllocationContext Context)
{
	VProcedure& EmptyProcedure = VProcedure::NewUninitialized(Context, 0, 0, 0, 0, 0, 0, 0, 0);
	GlobalEmptyFrame.Set(Context, &VFrame::New(Context, nullptr, nullptr, nullptr, EmptyProcedure));
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
