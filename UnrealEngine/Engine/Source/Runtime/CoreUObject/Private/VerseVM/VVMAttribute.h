// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Containers/ContainersFwd.h"
#include "VerseVM/VVMClass.h"

class UStruct;
class FProperty;
class UFunction;
class UEnum;

namespace Verse
{

struct FAllocationContext;
struct VValue;
class CAttributeValue;
class FDefinition;
class ICustomAttributeHandler;

struct AUTORTFM_DISABLE FClassAttribute
{
	static void ApplyClassAttribute(FAllocationContext Context, VValue AttributeValue, VClass& Class, UStruct* Struct, TArray<FString>& OutErrors);
};

struct AUTORTFM_DISABLE FClassEntryAttributeElement
{
	FClassEntryAttributeElement(FProperty* InUeDefinition);

	void Apply(FAllocationContext Context, VValue AttributeValue, VArchetype::VEntry& Entry, TArray<FString>& OutErrors) const;

	FProperty* UeDefinition;
};

};     // namespace Verse
#endif // WITH_VERSE_VM
