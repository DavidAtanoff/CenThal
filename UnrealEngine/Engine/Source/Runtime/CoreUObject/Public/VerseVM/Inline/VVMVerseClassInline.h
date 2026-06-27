// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMVerseClass.h"

inline Verse::VClass* UVerseClass::GetVerseClass(Verse::FAllocationContext Context, UClass* Class)
{
	using namespace Verse;
	if (UVerseClass* VerseClass = Cast<UVerseClass>(Class))
	{
		return VerseClass->Class.Get();
	}
	else if (VNamedType* Type = GlobalProgram->LookupImport(Context, Class))
	{
		return &Type->StaticCast<VClass>();
	}
	else
	{
		return nullptr;
	}
}

inline Verse::VShape* UVerseClass::GetShape(Verse::FAllocationContext Context, UClass* Class)
{
	using namespace Verse;
	if (UVerseClass* VerseClass = Cast<UVerseClass>(Class))
	{
		return &VerseClass->Shape.Get(Context).StaticCast<VShape>();
	}
	else
	{
		return GlobalProgram->LookupShape(Context, Class);
	}
}

inline Verse::VShape& UVerseClass::GetShapeForLoadField(Verse::FAllocationContext Context, UClass* Class)
{
	using namespace Verse;
	for (;;)
	{
		if (VShape* Shape = GetShape(Context, Class))
		{
			return *Shape;
		}

		// TODO: Once we can maintain a weak map of UObjects to VShapes, we can cache superclass results instead
		// of walking up the superclass chain each time a field access is performed. #jira SOL-8667
		Class = Class->GetSuperClass();
		V_DIE_UNLESS(Class);
	}
}

inline Verse::FOpResult UVerseClass::LoadField(Verse::FAllocationContext Context, UObject* Object, Verse::VUniqueString& FieldName, Verse::VValue Self)
{
	using namespace Verse;

	AutoRTFM::UnreachableIfClosed("#jira SOL-8415");

	VShape& Shape = GetShapeForLoadField(Context, Object->GetClass());
	return LoadField(Context, Object, Shape.GetField(FieldName), Self);
}

#endif // WITH_VERSE_VM
