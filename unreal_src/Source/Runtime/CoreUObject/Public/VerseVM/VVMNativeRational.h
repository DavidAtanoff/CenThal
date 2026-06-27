// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/Platform.h"
#include "Serialization/StructuredArchive.h"
#include "Templates/TypeHash.h"
#include "UObject/StructOpsTypeTraits.h"
#include "VVMNativeRational.generated.h"

// Used to represent integer rational numbers in Verse.
USTRUCT()
struct FVerseRational
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = Rational)
	int64 Numerator{0};

	UPROPERTY(EditAnywhere, Category = Rational)
	int64 Denominator{1};

	FVerseRational ReduceAndNormalizeSigns() const
	{
		int64 A = Numerator;
		int64 B = Denominator;
		while (B)
		{
			int64 Remainder = A % B;
			A = B;
			B = Remainder;
		}

		// Avoid overflow dividing INT64_MIN / -1.
		if ((Numerator == INT64_MIN || Denominator == INT64_MIN) && A == -1)
		{
			A = 1;
		}

		FVerseRational Result;
		Result.Numerator = Numerator / A;
		Result.Denominator = Denominator / A;

		if (Result.Denominator < 0 && Result.Numerator != INT64_MIN && Result.Denominator != INT64_MIN)
		{
			Result.Numerator = -Result.Numerator;
			Result.Denominator = -Result.Denominator;
		}

		return Result;
	}

	friend bool operator==(const FVerseRational& A, const FVerseRational& B)
	{
		FVerseRational ReducedA = A.ReduceAndNormalizeSigns();
		FVerseRational ReducedB = B.ReduceAndNormalizeSigns();
		return ReducedA.Numerator == ReducedB.Numerator
			&& ReducedA.Denominator == ReducedB.Denominator;
	}

	friend uint32 GetTypeHash(const FVerseRational& R)
	{
		FVerseRational ReducedR = R.ReduceAndNormalizeSigns();
		// Make sure that integers hash the same whether represented as an int64 or a FVerseRational.
		const uint32 NumeratorHash = GetTypeHash(ReducedR.Numerator);
		return ReducedR.Denominator == 1
				 ? NumeratorHash
				 : HashCombineFast(NumeratorHash, GetTypeHash(ReducedR.Denominator));
	}

	friend void operator<<(FStructuredArchive::FSlot Slot, FVerseRational& R)
	{
		FStructuredArchive::FRecord Record = Slot.EnterRecord();
		FStructuredArchive::FSlot NumeratorField = Record.EnterField(TEXT("Numerator"));
		NumeratorField << R.Numerator;
		FStructuredArchive::FSlot DenominatorField = Record.EnterField(TEXT("Denominator"));
		DenominatorField << R.Denominator;
	}
};

template <>
struct TStructOpsTypeTraits<FVerseRational> : public TStructOpsTypeTraitsBase2<FVerseRational>
{
	enum
	{
		WithIdenticalViaEquality = true
	};
};
