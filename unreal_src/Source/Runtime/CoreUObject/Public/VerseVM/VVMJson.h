// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Dom/JsonObject.h"

#include "Dom/JsonObject.h"
#include "VerseVM/VVMToJsonCallback.h"

namespace Verse
{
struct VCell;
struct VValue;

// constants for Persistence JSON
namespace Persistence
{
// Only needed until PDSS no longer needs this field.
constexpr TCHAR PackageNameKey[] = TEXT("$package_name");
// Only needed until PDSS no longer needs this field.
constexpr TCHAR ClassNameKey[] = TEXT("$class_name");
constexpr TCHAR KeyKey[] = TEXT("k");
constexpr TCHAR ValueKey[] = TEXT("v");
} // namespace Persistence

// constants for Persona JSON
namespace Persona
{
constexpr TCHAR StringString[] = TEXT("STRING");
constexpr TCHAR NumberString[] = TEXT("NUMBER");
constexpr TCHAR ObjectString[] = TEXT("OBJECT");
constexpr TCHAR ArrayString[] = TEXT("ARRAY");
constexpr TCHAR BooleanString[] = TEXT("BOOLEAN");
constexpr TCHAR IntegerString[] = TEXT("INTEGER");

constexpr TCHAR DescriptionString[] = TEXT("description");
constexpr TCHAR EnumString[] = TEXT("enum");
constexpr TCHAR ItemsString[] = TEXT("items");
constexpr TCHAR MaximumString[] = TEXT("maximum");
constexpr TCHAR MinimumString[] = TEXT("minimum");
constexpr TCHAR PropertiesString[] = TEXT("properties");
constexpr TCHAR RequiredString[] = TEXT("required");
constexpr TCHAR TypeString[] = TEXT("type");
constexpr TCHAR AnyOfString[] = TEXT("any_of");
constexpr TCHAR KeyString[] = TEXT("key");
constexpr TCHAR ValueString[] = TEXT("value");
constexpr TCHAR NullableString[] = TEXT("nullable");

constexpr TCHAR SchemaString[] = TEXT("$schema");
constexpr TCHAR SchemaLink[] = TEXT("https://ai.google.dev/api/caching#Schema");

inline const FName LLMDescriptionAttributeName(TEXT("llm_description_attribute"));
inline const FName LLMDescriptionFieldName(TEXT("Description"));

COREUOBJECT_API void InjectLLMDescription(const UStruct* DeclaredIn, FProperty* Property, FJsonObject& FieldObject);
} // namespace Persona

enum class EValueJSONFormat
{
	Analytics,   // Basic JSON, no schema
	Persistence, // Persistence JSON
	Persona      // Persona JSON, types implementing this should output themselves according to https://ai.google.dev/api/caching#Schema
};

enum class EVisitState
{
	Visiting,
	Visited
};

// To handle case differences for field names in JSON code
// ie: Analytics prefers PascalCase whereas Persona/Persistence use CamelCase
#define JSON_FIELD(Name) Format == EValueJSONFormat::Persona ? Persona::Name##String : TEXT(#Name)

#if WITH_VERSE_VM
struct FRunningContext;

AUTORTFM_DISABLE COREUOBJECT_API TSharedPtr<FJsonValue> ToJSON(FRunningContext, VValue, EValueJSONFormat, AUTORTFM_IMPLICIT_DISABLE FToJsonCallback, uint32 RecursionDepth = 0, FJsonObject* Defs = nullptr);
#endif

COREUOBJECT_API TSharedRef<FJsonValue> Int64ToJson(int64 Arg);

COREUOBJECT_API bool TryGetInt64(const FJsonValue& JsonValue, int64& Int64Value);

AUTORTFM_DISABLE_IF(WITH_VERSE_VM)
COREUOBJECT_API TSharedPtr<FJsonValue> Wrap(const TSharedPtr<FJsonValue>& Value, EValueJSONFormat Format);
AUTORTFM_DISABLE_IF(WITH_VERSE_VM)
COREUOBJECT_API TSharedPtr<FJsonValue> Unwrap(const TSharedPtr<FJsonValue>& Value, EValueJSONFormat Format);

AUTORTFM_DISABLE_IF(WITH_VERSE_VM)
COREUOBJECT_API TSharedPtr<FJsonValue> Wrap(const TSharedPtr<FJsonValue>& Value);
COREUOBJECT_API TSharedPtr<FJsonValue> Unwrap(const FJsonValue& Value);

} // namespace Verse
