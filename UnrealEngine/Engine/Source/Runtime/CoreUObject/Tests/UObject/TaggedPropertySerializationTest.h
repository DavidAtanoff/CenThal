// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Containers/Array.h"
#include "UObject/Class.h"

#include "TaggedPropertySerializationTest.generated.h"

#if WITH_TESTS

USTRUCT()
struct FTestTaggedPropertySerializationBoolArray
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<bool> Array;
};

USTRUCT()
struct FTestTaggedPropertySerializationInt32Array
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<int32> Array;
};

#endif // WITH_TESTS
