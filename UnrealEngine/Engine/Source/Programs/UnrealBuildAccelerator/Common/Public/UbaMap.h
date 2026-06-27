// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaMemory.h"
#include <map>
#include <set>

namespace uba
{
	template<typename Key, typename Value, typename Less = std::less<>>
	using Map = std::map<Key, Value, Less, Allocator<std::pair<const Key, Value>>>;

	template<typename Key, typename Less = std::less<>>
	using Set = std::set<Key, Less, Allocator<Key>>;
}
