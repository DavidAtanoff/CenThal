// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "HAL/Platform.h" // IWYU pragma: keep
#include "verse_local_allocator_ue.h"
#include <cstddef>

namespace Verse
{
class FSubspace;

// This is really pas_local_allocator.
struct alignas(PAS_LOCAL_ALLOCATOR_ALIGNMENT) FLocalAllocator final
{
	FLocalAllocator() = default;
	COREUOBJECT_API ~FLocalAllocator();

	COREUOBJECT_API void Initialize(FSubspace* Subspace, size_t Size);
	COREUOBJECT_API void Stop();
	COREUOBJECT_API std::byte* Allocate();
	COREUOBJECT_API std::byte* TryAllocate();

private:
	// It so happens to be the case that we only create FLocalAllocators on the UE side when it's for sizes that qualify for the small segregated page config variant.
	// That variant may require a smaller local allocator size, because it has fewer bits, depending on system page size.
	//
	// If we ever started getting panics inside verse_local_allocator_construct() because our size was two small, we should change this to use
	// VERSE_MAX_SEGREGATED_LOCAL_ALLOCATOR_SIZE instead.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-folding-constant"
#pragma clang diagnostic ignored "-Wvla-cxx-extension"
#endif // defined(__clang__)
	char OpaqueData[VERSE_SMALL_SEGREGATED_LOCAL_ALLOCATOR_SIZE];
#if defined(__clang__)
#pragma clang diagnostic pop
#endif // defined(__clang__)
};

} // namespace Verse
#endif // WITH_VERSE_VM
