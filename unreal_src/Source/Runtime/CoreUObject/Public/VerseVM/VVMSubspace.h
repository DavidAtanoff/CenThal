// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "HAL/Platform.h"
#include <cstddef>

namespace Verse
{
struct FAllocationContext;

// This is really a pas_heap. Hence, we don't let you create it directly. We create a pas_heap
// and cast to this.
class FSubspace final
{
public:
	COREUOBJECT_API static FSubspace* Create(size_t MinAlign = 1, size_t ReservationSize = 0, size_t ReservationAlignment = 1);
	COREUOBJECT_API std::byte* GetBase() const;

private:
	friend struct FAllocationContext;
	friend struct FContextImpl;

	COREUOBJECT_API std::byte* TryAllocate(size_t Size);
	COREUOBJECT_API std::byte* TryAllocate(size_t Size, size_t Alignment);
	COREUOBJECT_API std::byte* Allocate(size_t Size);
	COREUOBJECT_API std::byte* Allocate(size_t Size, size_t Alignment);

	FSubspace() = delete;
	FSubspace(const FSubspace&) = delete;
};

} // namespace Verse
#endif // WITH_VERSE_VM
