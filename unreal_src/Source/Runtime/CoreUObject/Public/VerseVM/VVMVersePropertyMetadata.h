// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "VerseVM/VVMVerseConstraints.h"

#include "VVMVersePropertyMetadata.generated.h"

// Stores per-property metadata for a Verse property on UVerseClass/UVerseStruct.
USTRUCT()
struct FVersePropertyMetadata
{
	GENERATED_BODY()

	UPROPERTY()
	FVerseIntConstraints IntConstraints;

	UPROPERTY()
	FVerseDoubleConstraints DoubleConstraints;

	// Only populated in BPVM
	UPROPERTY()
	FString LLMDescription;
};
