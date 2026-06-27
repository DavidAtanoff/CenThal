// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaMemory.h"
#if !UBA_STUB_BUILD
#include <memory>
#endif

namespace uba
{
	// Minimal basic_string-like TString for tchar. Heap-only (no SSO yet —
	// easy to add later if profiling calls for it).
	class TString
	{
	public:
		using value_type = tchar;
		using size_type  = u64;

		TString() = default;

		TString(const tchar* s)
		{
			if (!s || !*s) return;
			size_type n = strlen_tchar(s);
			assign_raw(s, n);
		}

		TString(const tchar* s, size_type n) { assign_raw(s, n); }

		// Iterator-range ctor: [first, last). Fast path for raw tchar pointers.
		TString(const tchar* first, const tchar* last)
		{
			if (last > first) assign_raw(first, size_type(last - first));
		}

		// Generic iterator-range ctor (char iterators from std::string etc.).
		template<typename InputIt,
		         typename = std::enable_if_t<!std::is_convertible_v<InputIt, const tchar*>>>
		TString(InputIt first, InputIt last)
		{
			while (first != last) push_back(tchar(*first++));
		}

		TString(size_type n, tchar c)
		{
			reserve(n);
			for (size_type i = 0; i < n; ++i) m_data[i] = c;
			m_size = n;
			if (m_data) m_data[m_size] = 0;
		}

		TString(const TString& o) { assign_raw(o.m_data, o.m_size); }

		TString(TString&& o) noexcept { swap(o); }

		~TString()
		{
			if (m_data) m_alloc.deallocate(m_data, m_capacity);
		}

		TString& operator=(const TString& o)
		{
			if (this != &o)
			{
				clear();
				assign_raw(o.m_data, o.m_size);
			}
			return *this;
		}

		TString& operator=(TString&& o) noexcept
		{
			if (this != &o)
			{
				if (m_data) m_alloc.deallocate(m_data, m_capacity);
				m_data = nullptr; m_size = 0; m_capacity = 0;
				swap(o);
			}
			return *this;
		}

		TString& operator=(const tchar* s)
		{
			clear();
			if (s && *s) assign_raw(s, strlen_tchar(s));
			return *this;
		}

		size_type size()   const { return m_size; }
		size_type length() const { return m_size; }
		bool      empty()  const { return m_size == 0; }
		size_type capacity() const { return m_capacity ? m_capacity - 1 : 0; }

		const tchar* c_str() const { return m_data ? m_data : empty_str(); }
		const tchar* data()  const { return c_str(); }
		tchar*       data()        { return m_data ? m_data : const_cast<tchar*>(empty_str()); }

		tchar&       operator[](size_type i)       { return m_data[i]; }
		const tchar& operator[](size_type i) const { return m_data[i]; }

		// Raw-pointer iterators (tchar*).
		using iterator       = tchar*;
		using const_iterator = const tchar*;
		iterator       begin()        { return m_data ? m_data : const_cast<tchar*>(empty_str()); }
		iterator       end()          { return begin() + m_size; }
		const_iterator begin()  const { return m_data ? m_data : empty_str(); }
		const_iterator end()    const { return begin() + m_size; }
		const_iterator cbegin() const { return begin(); }
		const_iterator cend()   const { return end(); }

		tchar&       front()       { return m_data[0]; }
		const tchar& front() const { return m_data[0]; }
		tchar&       back()        { return m_data[m_size - 1]; }
		const tchar& back()  const { return m_data[m_size - 1]; }

		static constexpr size_type npos = size_type(-1);

		// Find first occurrence of `s` starting at `from`. Returns npos on miss.
		size_type find(const tchar* s, size_type from = 0) const
		{
			if (!s || !m_data) return npos;
			size_type sn = strlen_tchar(s);
			if (sn == 0) return from <= m_size ? from : npos;
			if (from > m_size || m_size - from < sn) return npos;
			for (size_type i = from; i + sn <= m_size; ++i)
			{
				size_type j = 0;
				while (j < sn && m_data[i + j] == s[j]) ++j;
				if (j == sn) return i;
			}
			return npos;
		}

		size_type find(tchar c, size_type from = 0) const
		{
			for (size_type i = from; i < m_size; ++i) if (m_data[i] == c) return i;
			return npos;
		}

		size_type find(const TString& o, size_type from = 0) const { return find(o.c_str(), from); }

		// Replace [pos, pos+count) with [s, s+slen). Returns *this.
		TString& replace(size_type pos, size_type count, const tchar* s, size_type slen)
		{
			if (pos > m_size) pos = m_size;
			if (count > m_size - pos) count = m_size - pos;
			size_type newSize = m_size - count + slen;
			if (slen != count)
			{
				reserve(newSize);
				// Shift tail.
				if (slen < count)
				{
					for (size_type i = pos + slen; i < newSize; ++i)
						m_data[i] = m_data[i - slen + count];
				}
				else
				{
					for (size_type i = newSize; i-- > pos + slen; )
						m_data[i] = m_data[i + count - slen];
				}
			}
			for (size_type i = 0; i < slen; ++i) m_data[pos + i] = s[i];
			m_size = newSize;
			if (m_data) m_data[m_size] = 0;
			return *this;
		}

		TString& replace(size_type pos, size_type count, const tchar* s)
		{
			return replace(pos, count, s, s ? strlen_tchar(s) : 0);
		}

		TString& replace(size_type pos, size_type count, const TString& o)
		{
			return replace(pos, count, o.m_data, o.m_size);
		}

		TString& assign(const tchar* s) { *this = s; return *this; }
		TString& assign(const tchar* s, size_type n) { clear(); assign_raw(s, n); return *this; }
		TString& assign(const TString& o) { *this = o; return *this; }
		TString& assign(size_type n, tchar c)
		{
			clear();
			reserve(n);
			for (size_type i = 0; i < n; ++i) m_data[i] = c;
			m_size = n;
			if (m_data) m_data[m_size] = 0;
			return *this;
		}

		// Iterator-range assign (e.g. `s.assign(p, q)` with char iterators).
		template<typename InputIt,
		         typename = std::enable_if_t<!std::is_integral_v<InputIt>>>
		TString& assign(InputIt first, InputIt last)
		{
			clear();
			while (first != last) push_back(tchar(*first++));
			return *this;
		}

		// Substring from [pos, pos+count).
		TString substr(size_type pos = 0, size_type count = npos) const
		{
			if (pos > m_size) pos = m_size;
			size_type n = (count == npos || count > m_size - pos) ? m_size - pos : count;
			return TString(m_data + pos, n);
		}

		// Erase `count` chars starting at `pos`.
		TString& erase(size_type pos, size_type count = npos)
		{
			if (pos > m_size) return *this;
			if (count > m_size - pos) count = m_size - pos;
			if (!count) return *this;
			for (size_type i = pos; i + count < m_size; ++i)
				m_data[i] = m_data[i + count];
			m_size -= count;
			if (m_data) m_data[m_size] = 0;
			return *this;
		}

		void clear()
		{
			m_size = 0;
			if (m_data) m_data[0] = 0;
		}

		void reserve(size_type n)
		{
			// capacity stored includes space for null terminator.
			size_type need = n + 1;
			if (need <= m_capacity) return;
			tchar* nd = m_alloc.allocate(need);
			if (m_data)
			{
				for (size_type i = 0; i <= m_size; ++i) nd[i] = m_data[i];
				m_alloc.deallocate(m_data, m_capacity);
			}
			else
			{
				nd[0] = 0;
			}
			m_data = nd;
			m_capacity = need;
		}

		void resize(size_type n)
		{
			reserve(n);
			if (n > m_size) for (size_type i = m_size; i < n; ++i) m_data[i] = 0;
			m_size = n;
			if (m_data) m_data[m_size] = 0;
		}

		TString& append(const tchar* s, size_type n)
		{
			if (!s || !n) return *this;
			reserve(m_size + n);
			for (size_type i = 0; i < n; ++i) m_data[m_size + i] = s[i];
			m_size += n;
			m_data[m_size] = 0;
			return *this;
		}

		TString& append(const tchar* s)
		{
			return append(s, s ? strlen_tchar(s) : 0);
		}

		TString& append(const TString& o) { return append(o.m_data, o.m_size); }

		TString& operator+=(const tchar* s)     { return append(s); }
		TString& operator+=(const TString& o)   { return append(o); }
		TString& operator+=(tchar c)
		{
			reserve(m_size + 1);
			m_data[m_size++] = c;
			m_data[m_size] = 0;
			return *this;
		}

		void push_back(tchar c) { *this += c; }

		void swap(TString& o) noexcept
		{
			tchar* td = m_data; m_data = o.m_data; o.m_data = td;
			size_type ts = m_size; m_size = o.m_size; o.m_size = ts;
			size_type tc = m_capacity; m_capacity = o.m_capacity; o.m_capacity = tc;
		}

		// Lexicographic compare.
		int compare(const TString& o) const { return compare_raw(m_data, m_size, o.m_data, o.m_size); }
		int compare(const tchar* s)   const
		{
			size_type n = s ? strlen_tchar(s) : 0;
			return compare_raw(m_data, m_size, s, n);
		}

		// Compare substring [pos, pos+count) with (s, slen) / s / other.
		int compare(size_type pos, size_type count, const tchar* s, size_type slen) const
		{
			if (pos > m_size) pos = m_size;
			if (count > m_size - pos) count = m_size - pos;
			return compare_raw(m_data + pos, count, s, slen);
		}
		int compare(size_type pos, size_type count, const tchar* s) const
		{
			return compare(pos, count, s, s ? strlen_tchar(s) : 0);
		}
		int compare(size_type pos, size_type count, const TString& o) const
		{
			return compare(pos, count, o.m_data, o.m_size);
		}

		bool operator==(const TString& o) const
		{
			if (m_size != o.m_size) return false;
			for (size_type i = 0; i < m_size; ++i) if (m_data[i] != o.m_data[i]) return false;
			return true;
		}
		bool operator!=(const TString& o) const { return !(*this == o); }
		bool operator<(const TString& o)  const { return compare(o) < 0; }

		bool operator==(const tchar* s) const
		{
			size_type n = s ? strlen_tchar(s) : 0;
			if (m_size != n) return false;
			for (size_type i = 0; i < n; ++i) if (m_data[i] != s[i]) return false;
			return true;
		}
		bool operator!=(const tchar* s) const { return !(*this == s); }

	private:
		static const tchar* empty_str() { static const tchar z[1] = { 0 }; return z; }

		static size_type strlen_tchar(const tchar* s)
		{
			size_type n = 0;
			while (s[n]) ++n;
			return n;
		}

		static int compare_raw(const tchar* a, size_type an, const tchar* b, size_type bn)
		{
			size_type m = an < bn ? an : bn;
			for (size_type i = 0; i < m; ++i)
			{
				if (a[i] < b[i]) return -1;
				if (a[i] > b[i]) return 1;
			}
			if (an < bn) return -1;
			if (an > bn) return 1;
			return 0;
		}

		void assign_raw(const tchar* s, size_type n)
		{
			if (n == 0) return;
			reserve(n);
			for (size_type i = 0; i < n; ++i) m_data[i] = s[i];
			m_size = n;
			m_data[m_size] = 0;
		}

		tchar* m_data = nullptr;
		size_type m_size = 0;
		size_type m_capacity = 0; // includes space for null terminator
		Allocator<tchar> m_alloc;
	};

	// Free comparison for (const tchar*, TString)
	inline bool operator==(const tchar* s, const TString& t) { return t == s; }
	inline bool operator!=(const tchar* s, const TString& t) { return t != s; }

	// Heterogeneous `<` against tchar* so std::less<> transparent compare works
	// (e.g. Map<TString, V> with `find(const tchar*)`).
	inline bool operator<(const TString& a, const tchar* b) { return a.compare(b) < 0; }
	inline bool operator<(const tchar* a, const TString& b) { return b.compare(a) > 0; }

	// Concatenation operators.
	inline TString operator+(const TString& a, const TString& b)
	{
		TString r; r.reserve(a.size() + b.size()); r += a; r += b; return r;
	}
	inline TString operator+(const TString& a, const tchar* b)
	{
		TString r; r += a; r += b; return r;
	}
	inline TString operator+(const tchar* a, const TString& b)
	{
		TString r; r += a; r += b; return r;
	}
	inline TString operator+(const TString& a, tchar b)
	{
		TString r; r += a; r += b; return r;
	}
	inline TString operator+(tchar a, const TString& b)
	{
		TString r; r += a; r += b; return r;
	}
}

// FNV-1a hash specialization so TString can be used as a key in UnorderedMap/Set.
#if !UBA_STUB_BUILD
namespace std
{
	template<>
	struct hash<uba::TString>
	{
		size_t operator()(const uba::TString& s) const noexcept
		{
			// FNV-1a 64-bit over the raw bytes of the string.
			uba::u64 h = 14695981039346656037ull;
			const uba::u8* p = reinterpret_cast<const uba::u8*>(s.data());
			uba::u64 n = s.size() * sizeof(uba::tchar);
			for (uba::u64 i = 0; i < n; ++i)
			{
				h ^= p[i];
				h *= 1099511628211ull;
			}
			return static_cast<size_t>(h);
		}
	};
}
#endif // !UBA_STUB_BUILD
