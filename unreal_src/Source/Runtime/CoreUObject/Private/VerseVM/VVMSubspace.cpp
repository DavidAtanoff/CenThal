// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "VerseVM/VVMSubspace.h"
#include "verse_heap_ue.h"

namespace Verse
{

FSubspace* FSubspace::Create(size_t MinAlign, size_t ReservationSize, size_t ReservationAlignment)
{
	return reinterpret_cast<FSubspace*>(verse_heap_create(MinAlign, ReservationSize, ReservationAlignment));
}

std::byte* FSubspace::GetBase() const
{
	return static_cast<std::byte*>(verse_heap_get_base(const_cast<pas_heap*>(reinterpret_cast<const pas_heap*>(this))));
}

std::byte* FSubspace::TryAllocate(size_t Size)
{
	return static_cast<std::byte*>(verse_heap_try_allocate(reinterpret_cast<pas_heap*>(this), Size));
}

std::byte* FSubspace::TryAllocate(size_t Size, size_t Alignment)
{
	return static_cast<std::byte*>(verse_heap_try_allocate_with_alignment(reinterpret_cast<pas_heap*>(this), Size, Alignment));
}

std::byte* FSubspace::Allocate(size_t Size)
{
	return static_cast<std::byte*>(verse_heap_allocate(reinterpret_cast<pas_heap*>(this), Size));
}

std::byte* FSubspace::Allocate(size_t Size, size_t Alignment)
{
	return static_cast<std::byte*>(verse_heap_allocate_with_alignment(reinterpret_cast<pas_heap*>(this), Size, Alignment));
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
