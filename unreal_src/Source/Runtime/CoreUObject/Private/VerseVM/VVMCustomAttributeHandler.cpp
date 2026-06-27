// Copyright Epic Games, Inc. All Rights Reserved.

#include "VerseVM/VVMCustomAttributeHandler.h"

namespace Verse
{

TMap<FName, ICustomAttributeHandler*> ICustomAttributeHandler::AttributeHandlers;

ICustomAttributeHandler::~ICustomAttributeHandler()
{
	TArray<FName> KeysToRemove;

	// Can't modify a TMap while iterating, so save off the keys that we handle...
	for (auto& MapIt : AttributeHandlers)
	{
		if (MapIt.Value == this)
		{
			KeysToRemove.Add(MapIt.Key);
		}
	}

	// ...and then remove them after
	for (FName KeyIt : KeysToRemove)
	{
		AttributeHandlers.Remove(KeyIt);
	}
}

ICustomAttributeHandler* ICustomAttributeHandler::FindHandlerForAttribute(const FName AttributeName)
{
	ICustomAttributeHandler** Handler = AttributeHandlers.Find(AttributeName);
	if (Handler)
	{
		return *Handler;
	}

	return nullptr;
}

bool ICustomAttributeHandler::ProcessAttribute(const CAttributeValue& Payload, const FClassDefinition& Definition, TArray<FString>& OutErrorMessages)
{
	OutErrorMessages.Add(TEXT("ProcessAttribute is unimplemented"));
	return false;
}

bool ICustomAttributeHandler::ProcessAttribute(const CAttributeValue& Payload, const FDataDefinition& Definition, TArray<FString>& OutErrorMessages)
{
	OutErrorMessages.Add(TEXT("ProcessAttribute is unimplemented"));
	return false;
}

bool ICustomAttributeHandler::ProcessAttribute(const CAttributeValue& Payload, const FFunctionDefinition& Definition, TArray<FString>& OutErrorMessages)
{
	OutErrorMessages.Add(TEXT("ProcessAttribute is unimplemented"));
	return false;
}

bool ICustomAttributeHandler::ProcessAttribute(const CAttributeValue& Payload, const FEnumDefinition& Definition, TArray<FString>& OutErrorMessages)
{
	OutErrorMessages.Add(TEXT("ProcessAttribute is unimplemented"));
	return false;
}

} // namespace Verse