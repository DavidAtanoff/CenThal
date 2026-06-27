// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "VerseVM/VVMNativeProcedure.h"
#include "VerseVM/Inline/VVMCellInline.h"
#include "VerseVM/VVMCppClassInfo.h"
#include "VerseVM/VVMNames.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMTextPrinting.h"
#include "VerseVM/VVMValuePrinting.h"

namespace Verse
{

DEFINE_DERIVED_VCPPCLASSINFO(VNativeProcedure);
TGlobalTrivialEmergentTypePtr<&VNativeProcedure::StaticCppClassInfo> VNativeProcedure::GlobalTrivialEmergentType;

template <typename TVisitor>
void VNativeProcedure::VisitReferencesImpl(TVisitor& Visitor)
{
	Visitor.Visit(Name, TEXT("Name"));
}

void VNativeProcedure::AppendToStringImpl(FAllocationContext Context, FUtf8StringBuilderBase& Builder, EValueStringFormat Format, TSet<const void*>& VisitedObjects, uint32 RecursionDepth)
{
	if (IsCellFormat(Format))
	{
		Builder.Append(TEXT("Name="));
		Name->AppendToString(Context, Builder, Format, VisitedObjects, RecursionDepth + 1);
	}
	else
	{
		Builder << Name->AsStringView();
	}
}

void VNativeProcedure::SerializeLayout(FAllocationContext Context, VNativeProcedure*& This, FStructuredArchiveVisitor& Visitor)
{
	if (Visitor.IsLoading())
	{
		This = new (Context.AllocateFastCell(sizeof(VNativeProcedure))) VNativeProcedure(Context, 0, nullptr, nullptr);
	}
}

void VNativeProcedure::SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor)
{
	Visitor.Visit(NumPositionalParameters, TEXT("NumPositionalParameters"));
	Visitor.Visit(Name, TEXT("Name"));
}

void VNativeProcedure::SetThunk(Verse::VPackage* Package, FUtf8StringView VerseScopePath, FUtf8StringView DecoratedName, FThunkFn NativeThunkPtr)
{
	// Function names are decorated twice: Once with the scope path they are defined in,
	// and once with the scope path of their base definition (usually these two are the same).
	//
	// Native functions only support a flat list of arguments. To support features like tuple unpacking
	// or named/optional parameters, they may be wrapped in a bytecode entry point. The native function
	// itself lives at a Verse path nested underneath the public entry point:
	//
	// Wrapper: (/Verse/path/to/function/definition:)(/Verse/path/to/overridden/function:)FunctionName(...)
	// Native:  (/Verse/path/to/function/definition/(/Verse/path/to/overridden/function:)FunctionName(...):)Native
	TUtf8StringBuilder<Names::DefaultNameLength> Name = Names::GetDecoratedName<UTF8CHAR>(VerseScopePath, DecoratedName, DecoratorString);
	Verse::VNativeProcedure* Procedure = Package->LookupDefinition<Verse::VNativeProcedure>(Name.ToView());
	if (!ensureMsgf(Procedure, TEXT("Could not find %s"), StringCast<TCHAR>(Name.ToString()).Get()))
	{
		return;
	}

	Procedure->Thunk = NativeThunkPtr;
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
