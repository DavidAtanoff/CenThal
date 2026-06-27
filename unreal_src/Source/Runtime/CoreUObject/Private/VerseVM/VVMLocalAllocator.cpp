// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
#include "VerseVM/VVMLocalAllocator.h"
#include "VerseVM/VVMLog.h"
#include "VerseVM/VVMTrue.h"

namespace Verse
{

FLocalAllocator::~FLocalAllocator()
{
	V_DIE_IF_MSG(bGTrue, "Should never destruct Verse local allocators");
}

void FLocalAllocator::Initialize(FSubspace* Subspace, size_t Size)
{
	verse_local_allocator_construct(reinterpret_cast<pas_local_allocator*>(this), reinterpret_cast<pas_heap*>(Subspace), Size, sizeof(FLocalAllocator));
}

void FLocalAllocator::Stop()
{
	verse_local_allocator_stop(reinterpret_cast<pas_local_allocator*>(this));
}

std::byte* FLocalAllocator::Allocate()
{
	return static_cast<std::byte*>(verse_local_allocator_allocate(reinterpret_cast<pas_local_allocator*>(this)));
}

std::byte* FLocalAllocator::TryAllocate()
{
	return static_cast<std::byte*>(verse_local_allocator_try_allocate(reinterpret_cast<pas_local_allocator*>(this)));
}

} // namespace Verse
#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)