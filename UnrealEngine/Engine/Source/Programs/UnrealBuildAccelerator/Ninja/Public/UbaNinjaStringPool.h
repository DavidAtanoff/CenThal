// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaSynchronization.h"
#include "UbaVector.h"

namespace uba
{
	// Lightweight char-based string reference with cached length (like Ninja's StringPiece)
	// Note: Different from UBA's StringView which uses tchar*
	struct CharStringView
	{
		const char* data = nullptr;
		u32 length = 0;

		CharStringView() = default;
		CharStringView(const char* str, u32 len) : data(str), length(len) {}

		bool operator==(const CharStringView& other) const
		{
			if (length != other.length)
				return false;
			return memcmp(data, other.data, length) == 0;
		}
	};

	// String pool with integer IDs for fast lookups (like Ninja's string interning)
	class StringPool
	{
	public:
		StringPool(MemoryBlock& memoryBlock);

		// Intern a string and return its unique ID (single-threaded only — call from one thread at a time)
		u32 Intern(const char* str, u32 length);
		u32 Intern(const CharStringView& view) { return Intern(view.data, view.length); }

		// Thread-safe variant — safe to call from parallel work items
		u32 InternThreadSafe(const char* str, u32 length);

		// Get string view by ID (inline for performance)
		inline CharStringView GetString(u32 id) const
		{
			if (id >= m_strings.size())
				return CharStringView{ "", 0 };
			const StringEntry& entry = m_strings[id];
			return CharStringView{ entry.data, entry.length };
		}

		// Get raw char* by ID (inline for performance)
		inline const char* GetCStr(u32 id) const
		{
			if (id >= m_strings.size())
				return "";
			return m_strings[id].data;
		}

		// Reserve capacity for expected number of unique strings
		void Reserve(u32 capacity);

		// Number of interned strings (including ID 0 = empty).
		// Useful for sizing flat arrays indexed by stringId.
		inline u32 Size() const { return u32(m_strings.size()); }

	private:
		struct StringEntry
		{
			const char* data;
			u32 length;
			u32 hash;
		};

		// Swiss Tables style: separate control bytes and IDs for cache-friendly lookups
		// Control byte: 0x80 = empty, 0xFF = deleted, [0-0x7F] = hash & 0x7F
		static constexpr u8 kEmpty = 0x80;
		static constexpr u8 kDeleted = 0xFF;

		MemoryBlock& m_memoryBlock;
		Vector<StringEntry> m_strings;  // ID -> StringEntry
		Vector<u8> m_ctrl;              // Control bytes (1 per slot, SIMD-friendly)
		Vector<u32> m_ids;              // String IDs (separate array)
		u32 m_hashMask;                 // Fast modulo: hash & m_hashMask
		u32 m_hashCount;                // Number of entries in hash table

		Futex m_mutex;  // Protects InternThreadSafe for use from parallel work items

		static u32 HashString(const char* data, u32 length);
		void RehashTable(u32 newSize);
		u32 FindSlot(u32 hash, const char* str, u32 length) const;
		u32 InternImpl(const char* str, u32 length, u32 hash);
	};

	// Empty string ID constant
	static constexpr u32 StringId_Empty = 0;
}
