// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaNinjaStringPool.h"

// SIMD for fast hash table probing (cross-platform)
#if defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
	#include <emmintrin.h>  // SSE2
	#define UBA_USE_SIMD 1
	#if defined(_MSC_VER)
		#include <intrin.h>
		inline int CountTrailingZeros(int mask) {
			unsigned long index;
			_BitScanForward(&index, mask);
			return (int)index;
		}
	#else
		inline int CountTrailingZeros(int mask) {
			return __builtin_ctz(mask);
		}
	#endif
#else
	#define UBA_USE_SIMD 0
#endif

namespace uba
{
	StringPool::StringPool(MemoryBlock& memoryBlock)
		: m_memoryBlock(memoryBlock)
		, m_hashMask(0)
		, m_hashCount(0)
	{
		// Reserve ID 0 for empty string
		m_strings.push_back(StringEntry{ "", 0, 0 });

		// Initialize with small hash table (will grow as needed)
		u32 initialSize = 1024;
		m_ctrl.resize(initialSize, kEmpty);
		m_ids.resize(initialSize, 0);
		m_hashMask = initialSize - 1;
	}

	void StringPool::Reserve(u32 capacity)
	{
		m_strings.reserve(capacity);

		// Find next power of 2 that gives us ~75% load factor
		u32 desiredSize = (capacity * 4) / 3;
		u32 tableSize = 1024;
		while (tableSize < desiredSize)
			tableSize <<= 1;

		if (tableSize > m_ctrl.size())
			RehashTable(tableSize);
	}

	// Fast MurmurHash3-inspired hash function with optimizations for short strings
	u32 StringPool::HashString(const char* data, u32 length)
	{
		const u32 seed = 0x9747b28c;

		// Fast path for very short strings (common in Ninja files)
		if (length <= 8)
		{
			u32 hash = seed ^ length;
			const u8* bytes = (const u8*)data;

			// Unrolled for lengths 1-8
			if (length >= 4)
			{
				hash ^= *(const u32*)bytes;
				hash = (hash << 13) | (hash >> 19);
				bytes += 4;
				length -= 4;
			}
			if (length >= 2)
			{
				hash ^= *(const u16*)bytes;
				bytes += 2;
				length -= 2;
			}
			if (length >= 1)
			{
				hash ^= *bytes;
			}

			// Quick finalization
			hash *= 0x85ebca6b;
			hash ^= hash >> 16;
			return hash ? hash : 1;
		}

		// Standard path for longer strings
		u32 hash = seed;

		// Process 4 bytes at a time
		const u32* blocks = (const u32*)data;
		u32 numBlocks = length / 4;

		for (u32 i = 0; i < numBlocks; ++i)
		{
			u32 k = blocks[i];
			k *= 0xcc9e2d51;
			k = (k << 15) | (k >> 17);
			k *= 0x1b873593;
			hash ^= k;
			hash = (hash << 13) | (hash >> 19);
			hash = hash * 5 + 0xe6546b64;
		}

		// Process remaining bytes
		const u8* tail = (const u8*)(data + numBlocks * 4);
		u32 k = 0;
		switch (length & 3)
		{
		case 3: k ^= tail[2] << 16; [[fallthrough]];
		case 2: k ^= tail[1] << 8;  [[fallthrough]];
		case 1: k ^= tail[0];
			k *= 0xcc9e2d51;
			k = (k << 15) | (k >> 17);
			k *= 0x1b873593;
			hash ^= k;
		}

		// Finalization
		hash ^= length;
		hash ^= hash >> 16;
		hash *= 0x85ebca6b;
		hash ^= hash >> 13;
		hash *= 0xc2b2ae35;
		hash ^= hash >> 16;

		return hash ? hash : 1;
	}

	u32 StringPool::FindSlot(u32 hash, const char* str, u32 length) const
	{
		u32 slot = hash & m_hashMask;
		u8 h2 = hash & 0x7F;  // Low 7 bits for quick matching

#if UBA_USE_SIMD
		// SIMD path: Check 16 slots at once.
		// kEmpty (0x80) and kDeleted (0xFF) both have the high bit set; h2 (0..0x7F) does
		// not. So _mm_movemask_epi8(group) directly gives the empty/deleted mask without
		// a separate cmpeq — the F14/abseil trick. Saves ~40% of SIMD ops in the inner loop.
		__m128i needle = _mm_set1_epi8(h2);
		const u32 tableSize = u32(m_ctrl.size());

		while (true)
		{
			if (slot + 16 <= tableSize)
			{
				__m128i group = _mm_loadu_si128((const __m128i*)&m_ctrl[slot]);
				int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(needle, group));
				int emptyMask = _mm_movemask_epi8(group);  // high bit set == empty/deleted

				// Check h2 matches first
				while (mask != 0)
				{
					int bitPos = CountTrailingZeros(mask);
					u32 candidateSlot = slot + bitPos;
					u32 candidateId = m_ids[candidateSlot];

					const StringEntry& entry = m_strings[candidateId];
					if (entry.length == length && memcmp(entry.data, str, length) == 0)
						return candidateSlot;  // Found it

					mask &= mask - 1;  // Clear lowest bit
				}

				// No match in this group; if any slot is empty the key doesn't exist
				if (emptyMask != 0)
					return slot + CountTrailingZeros(emptyMask);

				// All 16 slots full and no match - continue to next group
				slot = (slot + 16) & m_hashMask;
			}
			else
			{
				// Near end of array - fall back to scalar for remaining slots
				u8 ctrl = m_ctrl[slot];

				if (ctrl == kEmpty)
					return slot;

				if (ctrl == h2)
				{
					u32 candidateId = m_ids[slot];
					const StringEntry& entry = m_strings[candidateId];
					if (entry.length == length && memcmp(entry.data, str, length) == 0)
						return slot;
				}

				slot = (slot + 1) & m_hashMask;
			}
		}
#else
		// Scalar fallback for platforms without SSE2
		while (true)
		{
			u8 ctrl = m_ctrl[slot];

			if (ctrl == kEmpty)
				return slot;  // Empty slot found

			if (ctrl == h2)  // Quick match on low 7 bits
			{
				u32 candidateId = m_ids[slot];
				const StringEntry& entry = m_strings[candidateId];
				if (entry.length == length && memcmp(entry.data, str, length) == 0)
					return slot;  // Found it
			}

			slot = (slot + 1) & m_hashMask;
		}
#endif
	}

	u32 StringPool::Intern(const char* str, u32 length)
	{
		if (length == 0)
			return StringId_Empty;
		u32 hash = HashString(str, length);
		return InternImpl(str, length, hash);
	}

	u32 StringPool::InternThreadSafe(const char* str, u32 length)
	{
		if (length == 0)
			return StringId_Empty;
		u32 hash = HashString(str, length);
		SCOPED_FUTEX(m_mutex, lock);
		return InternImpl(str, length, hash);
	}

	u32 StringPool::InternImpl(const char* str, u32 length, u32 hash)
	{
		// Find slot (uses SIMD if available)
		u32 slot = FindSlot(hash, str, length);

		// Check if string already exists at this slot
		if (m_ctrl[slot] != kEmpty)
			return m_ids[slot];

		// New string - store in memory block
		char* internedData = (char*)m_memoryBlock.Allocate(length + 1, 1, TC("NinjaString"));
		memcpy(internedData, str, length);
		internedData[length] = 0;

		// Assign new ID
		u32 newId = u32(m_strings.size());
		m_strings.push_back(StringEntry{ internedData, length, hash });

		// Insert into hash table
		m_ctrl[slot] = hash & 0x7F;
		m_ids[slot] = newId;
		m_hashCount++;

		// Rehash if load factor exceeds 75%
		if (m_hashCount * 4 > m_ctrl.size() * 3)
			RehashTable(u32(m_ctrl.size()) * 2);

		return newId;
	}

	void StringPool::RehashTable(u32 newSize)
	{
		// newSize must be a power of two. Callers pick the target size explicitly:
		//   InternImpl passes m_ctrl.size() * 2 to grow on overflow;
		//   Reserve passes a capacity-derived size for a one-shot pre-sizing.
		Vector<u8> newCtrl(newSize, kEmpty);
		Vector<u32> newIds(newSize, 0);
		u32 newMask = newSize - 1;

		// Reinsert all existing entries
		for (u32 i = 0; i < m_ctrl.size(); ++i)
		{
			if (m_ctrl[i] == kEmpty || m_ctrl[i] == kDeleted)
				continue;  // Skip empty/deleted slots

			u32 id = m_ids[i];
			const StringEntry& strEntry = m_strings[id];
			u32 hash = strEntry.hash;

			// Find slot in new table using linear probing
			u32 slot = hash & newMask;
			while (newCtrl[slot] != kEmpty)
				slot = (slot + 1) & newMask;

			newCtrl[slot] = hash & 0x7F;
			newIds[slot] = id;
		}

		m_ctrl = std::move(newCtrl);
		m_ids = std::move(newIds);
		m_hashMask = newMask;
	}
}
