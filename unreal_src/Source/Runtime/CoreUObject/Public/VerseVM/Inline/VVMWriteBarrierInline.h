// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMTransaction.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{

template <typename T>
inline void TWriteBarrier<T>::SetTrailed(FAllocationContext Context, FTrail& Trail, TValue NewValue)
{
	Trail.LogBeforeWrite(Context, *this);
	Set(Context, NewValue);
}

template <typename T>
inline void TWriteBarrier<T>::SetNonCellNorPlaceholderTrailed(FAllocationContext Context, FTrail& Trail, VValue NewValue)
	requires bIsVValue
{
	Trail.LogBeforeWrite(Context, *this);
	SetNonCellNorPlaceholder(NewValue);
}

} // namespace Verse

#endif
