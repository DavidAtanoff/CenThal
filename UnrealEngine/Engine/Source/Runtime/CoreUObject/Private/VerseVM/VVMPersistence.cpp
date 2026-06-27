// Copyright Epic Games, Inc. All Rights Reserved.

#include "VerseVM/VVMPersistence.h"
#include "Dom/JsonObject.h"

namespace Verse
{

TOptional<TMap<FJsonObject::FStringType, TSharedPtr<FJsonValue>>> MapFromPersistentJson(const FJsonObject& JsonObject)
{
	TMap<FJsonObject::FStringType, TSharedPtr<FJsonValue>> JsonValues;
	for (auto&& [FieldKey, FieldJsonValue] : JsonObject.Values)
	{
		// Ignore dummy padding field, package name field, and class name field.
		int32 Index;
		const FStringView FieldKeyView(FieldKey);
		if (FieldKeyView.FindChar('$', Index))
		{
			continue;
		}
		TOptional<FStringView> ShortName = NameToShortName(FieldKeyView);
		if (!ShortName)
		{
			return {};
		}
		JsonValues.Emplace(FString(*ShortName), FieldJsonValue);
	}
	return JsonValues;
}

} // namespace Verse
