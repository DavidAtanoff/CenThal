// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaNinjaParser.h"
#include "UbaEnvironment.h"
#include "UbaFile.h"
#include "UbaFileAccessor.h"
#include "UbaLogger.h"
#include "UbaStringBuffer.h"
#include "UbaTimer.h"

namespace uba
{
	// Fast delimiter lookup table - replaces 7 character comparisons with 1 array lookup
	// Used in ParseBuild hot path for scanning tokens
	alignas(64) static const bool g_isDelimiter[256] = {
		0,0,0,0,0,0,0,0,0,1,1,0,0,1,0,0,  // 0-15:  \t=9, \n=10, \r=13
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 16-31
		1,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,  // 32-47: space=32, $=36
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 48-63
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 64-79
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 80-95
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 96-111
		0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,  // 112-127: |=124
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 128-143
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 144-159
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 160-175
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 176-191
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 192-207
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 208-223
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 224-239
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0   // 240-255
	};

	// Separator lookup (Windows only - POSIX needs just '/').
#if PLATFORM_WINDOWS
	alignas(64) static const bool g_isSep[256] = {
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 0-15
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 16-31
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,  // 32-47: '/' = 47
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 48-63
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 64-79
		0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,  // 80-95: '\\' = 92
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 96-111
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  // 112-127
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	};
#define UBA_IS_SEP(c) g_isSep[(u8)(c)]
#else
#define UBA_IS_SEP(c) ((c) == '/')
#endif

	// Strip leading "./" sequences and collapse mid-path "<sep>.<sep>" from a path token.
	// Matches ninja's CanonicalizePath behavior for these specific patterns.
	// POSIX: only '/' is a separator. Windows: both '/' and '\\'.
	static u32 StripDotSlash(char* data, u32 len)
	{
		if (len < 2) return len;

		// Phase 1: strip leading "./"
		u32 src = 0;
		while (src + 2 <= len && data[src] == '.' && UBA_IS_SEP(data[src + 1]))
			src += 2;

		// Phase 2: find first mid-path "<sep>.<sep>".
		// Use memchr (SIMD-accelerated on glibc/msvcrt) to jump from '.' to '.',
		// then verify neighbors. Most tokens have no '.' at all, or '.' only in
		// filenames (like "foo.cc"), so memchr eats the span cheaply and returns
		// without a boundary match.
		u32 midSrc = len; // sentinel: not found
		{
			const char* scan = data + src;
			const char* end  = data + len;
			while (scan + 1 < end)
			{
				const char* dot = (const char*)memchr(scan, '.', size_t(end - scan) - 1);
				if (!dot) break;
				u32 pos = u32(dot - data);
				// Short-circuit: data[pos+1] is always valid (memchr length -1).
				// data[pos-1] only accessed when pos > 0. After phase 1, a match with
				// pos == 0 is impossible (phase 1 would have stripped it), so the
				// `pos > 0` check is pure defense.
				if (UBA_IS_SEP(data[pos + 1]) && pos > 0 && UBA_IS_SEP(data[pos - 1]))
				{
					midSrc = pos;
					break;
				}
				scan = dot + 1;
			}
		}

		if (midSrc == len && src == 0)
			return len; // common fast path: nothing to strip

		if (midSrc == len)
		{
			// Only leading strip needed.
			len -= src;
			memmove(data, data + src, len);
			return len;
		}

		// Copy clean prefix [src..midSrc).
		u32 dst = midSrc - src;
		if (src > 0)
			memmove(data, data + src, dst);

		// Skip the found "./" and any consecutive ones.
		u32 s = midSrc + 2;
		while (s + 1 < len && data[s] == '.' && UBA_IS_SEP(data[s + 1]))
			s += 2;

		// Bulk-copy the remainder, collapsing any further "<sep>.<sep>".
#if !PLATFORM_WINDOWS
		// POSIX: memchr to the next '/', memmove the span, peek for "./" after.
		while (s < len)
		{
			const char* nextSep = (const char*)memchr(data + s, '/', len - s);
			if (!nextSep)
			{
				memmove(data + dst, data + s, len - s);
				dst += len - s;
				break;
			}
			u32 sepPos = u32(nextSep - data);
			u32 span = sepPos - s + 1; // include the separator
			memmove(data + dst, data + s, span);
			dst += span;
			s = sepPos + 1;
			while (s + 1 < len && data[s] == '.' && data[s + 1] == '/')
				s += 2;
		}
#else
		// Windows: two-pointer byte loop; the dual-sep set makes memchr less clean.
		// Still benefits from the g_isSep lookup vs the old compound compare.
		while (s < len)
		{
			char c = data[s++];
			data[dst++] = c;
			if (UBA_IS_SEP(c) && s + 1 < len && data[s] == '.' && UBA_IS_SEP(data[s + 1]))
				s += 2;
		}
#endif

		return dst;
	}

	NinjaParser::NinjaParser(MemoryBlock& memoryBlock)
		: m_memoryBlock(memoryBlock)
		, m_stringPool(memoryBlock)
		, m_tokenArena(TC("NinjaTokens"))
		, m_commandId(StringId_Empty)
		, m_descriptionId(StringId_Empty)
		, m_depfileId(StringId_Empty)
		, m_depsId(StringId_Empty)
		, m_rspfileId(StringId_Empty)
		, m_rspfile_contentId(StringId_Empty)
		, m_poolId(StringId_Empty)
		, m_restatId(StringId_Empty)
		, m_generatorId(StringId_Empty)
		, m_depthId(StringId_Empty)
		, m_inId(StringId_Empty)
		, m_outId(StringId_Empty)
		, m_phonyOutputId(StringId_Empty)
	{
		// Reserve 1 GB of virtual address space for the token arena. Pages
		// commit on demand as edges are parsed. Android scale is ~5M u32
		// tokens = 20 MB, well inside the reserve and one actual commit walk.
		m_tokenArena.Init(1ull * 1024 * 1024 * 1024);

		// Pre-allocate capacity for large builds (e.g., Chromium: 10k rules, 180k edges, 100k vars)
		m_edges.reserve(200000);
		m_rules.reserve(12000);
		m_pools.reserve(100);
		m_globalVars.reserve(120000);
		m_ruleMap.reserve(12000);
		m_poolMap.reserve(100);
		m_stringPool.Reserve(250000);  // Expect ~250k unique strings

		// Pre-intern all common names once so hot paths skip the hash lookup entirely
		m_commandId          = m_stringPool.Intern("command", 7);
		m_descriptionId      = m_stringPool.Intern("description", 11);
		m_depfileId          = m_stringPool.Intern("depfile", 7);
		m_depsId             = m_stringPool.Intern("deps", 4);
		m_rspfileId          = m_stringPool.Intern("rspfile", 7);
		m_rspfile_contentId  = m_stringPool.Intern("rspfile_content", 15);
		m_poolId             = m_stringPool.Intern("pool", 4);
		m_restatId           = m_stringPool.Intern("restat", 6);
		m_generatorId        = m_stringPool.Intern("generator", 9);
		m_depthId            = m_stringPool.Intern("depth", 5);
		m_inId               = m_stringPool.Intern("in", 2);
		m_outId              = m_stringPool.Intern("out", 3);
		m_phonyOutputId      = m_stringPool.Intern("phony_output", 12);

		// Pre-allocate token scratch buffer so ParseBuild/SplitPaths never alloc on startup
		m_tokenBuffer.resize(2048);
	}

	bool NinjaParser::Parse(Logger& logger, const tchar* filename, const tchar* buildRoot)
	{
		u64 startTime = GetTime();

		// Set the build root BEFORE ParseFile so its first-call "default to
		// ninja file's dir" is skipped. Ensure trailing separator so
		// append-filename path concatenation just works.
		if (buildRoot && *buildRoot)
		{
			StringBuffer<> root;
			root.Append(buildRoot);
			root.EnsureEndsWithSlash();
			Vector<char> rootUtf8;
			rootUtf8.reserve(root.count);
			for (u32 i = 0; i < root.count; ++i)
				rootUtf8.push_back((char)(u8)root.data[i]);
			m_buildRootDirId = m_stringPool.Intern(rootUtf8.data(), u32(rootUtf8.size()));
		}

		bool success = ParseFile(logger, filename, 0);  // Main file uses global scope
		m_parseTimeMs = TimeToMs(GetTime() - startTime);
		return success;
	}

	bool NinjaParser::ParseFile(Logger& logger, const tchar* filename, u32 scopeId)
	{
		FileAccessor fa(logger, filename);
		if (!fa.OpenMemoryRead())
			return false;

		// Ninja files are UTF-8/ASCII - work with them as char* directly
		const char* fileData = (const char*)fa.GetData();
		u64 fileSize = fa.GetSize();

		PrefetchVirtualMemory(fileData, fileSize);

		ParseContext ctx { logger };
		ctx.data = fileData;
		ctx.size = fileSize;
		ctx.pos = 0;
		ctx.line = 1;
		ctx.scopeId = scopeId;

		// Extract directory from filename and intern it
		StringBuffer<> tempDir;
		tempDir.Append(filename);
		const tchar* lastSep = TStrrchr(tempDir.data, PathSeparator);
		if (lastSep)
		{
			tempDir.count = u32(lastSep - tempDir.data + 1);
			tempDir.data[tempDir.count] = 0;
		}
		else
		{
			if (!GetCurrentDirectoryW(tempDir))
				return false;
			tempDir.EnsureEndsWithSlash();
		}

		// Convert tchar* path to char* for string pool
		Vector<char> dirUtf8;
		dirUtf8.reserve(tempDir.count);
		for (u32 i = 0; i < tempDir.count; ++i)
			dirUtf8.push_back((char)(u8)tempDir.data[i]);

		ctx.workingDirId = m_stringPool.Intern(dirUtf8.data(), u32(dirUtf8.size()));

		// Record the build root on the first (top-level) call so subninja paths can be resolved against it
		if (m_buildRootDirId == StringId_Empty)
			m_buildRootDirId = ctx.workingDirId;

		bool success = ParseImpl(ctx);
		m_lineCount = ctx.line;
		return success;
	}

	bool NinjaParser::ParseImpl(ParseContext& ctx)
	{
		const char* line;
		u32 lineLen;

		while (ReadLine(ctx, line, lineLen))
		{
			if (lineLen == 0)
				continue;

			// Dispatch on first character to minimize comparisons.
			// 'build' is by far the most common line type.
			bool ok = true;
			switch (line[0])
			{
			case 'b':
				if (lineLen >= 6 && memcmp(line, "build ", 6) == 0)
					ok = ParseBuild(ctx, line, lineLen);
				else
					ok = ParseVariable(ctx, line, lineLen);
				break;
			case 'r':
				if (lineLen >= 5 && memcmp(line, "rule ", 5) == 0)
					ok = ParseRule(ctx, line, lineLen);
				else
					ok = ParseVariable(ctx, line, lineLen);
				break;
			case 'i':
				if (lineLen >= 8 && memcmp(line, "include ", 8) == 0)
					ok = ParseInclude(ctx, line, lineLen);
				else
					ok = ParseVariable(ctx, line, lineLen);
				break;
			case 'd':
				if (lineLen >= 8 && memcmp(line, "default ", 8) == 0)
					ok = ParseDefault(ctx, line, lineLen);
				else
					ok = ParseVariable(ctx, line, lineLen);
				break;
			case 'p':
				if (lineLen >= 5 && memcmp(line, "pool ", 5) == 0)
					ok = ParsePool(ctx, line, lineLen);
				else
					ok = ParseVariable(ctx, line, lineLen);
				break;
			case 's':
				if (lineLen >= 9 && memcmp(line, "subninja ", 9) == 0)
					ok = ParseSubninja(ctx, line, lineLen);
				else
					ok = ParseVariable(ctx, line, lineLen);
				break;
			default:
				ok = ParseVariable(ctx, line, lineLen);
				break;
			}
			if (!ok)
				return false;
		}

		return true;
	}

	bool NinjaParser::ParseRule(ParseContext& ctx, const char* line, u32 lineLen)
	{
		// Extract rule name from "rule <name>" line
		const char* nameStart = line + 5; // Skip "rule "
		const char* nameEnd = nameStart;
		while (nameEnd < line + lineLen && *nameEnd != ' ' && *nameEnd != '\t')
			++nameEnd;

		u32 nameId = m_stringPool.Intern(nameStart, u32(nameEnd - nameStart));

		NinjaRule rule;
		rule.name = nameId;

		// Read indented properties
		UnorderedMap<u32, u32> props;
		if (!ReadIndentedLines(ctx, props))
			return false;

		// Extract known properties using pre-interned IDs (no hash lookup per call)
		for (auto& kv : props)
		{
			if (kv.first == m_commandId)
				rule.command = kv.second;
			else if (kv.first == m_descriptionId)
				rule.description = kv.second;
			else if (kv.first == m_depfileId)
				rule.depfile = kv.second;
			else if (kv.first == m_depsId)
				rule.deps = kv.second;
			else if (kv.first == m_rspfileId)
				rule.rspfile = kv.second;
			else if (kv.first == m_rspfile_contentId)
				rule.rspfile_content = kv.second;
			else if (kv.first == m_poolId)
				rule.pool = kv.second;
			else if (kv.first == m_restatId)
			{
				const char* valueStr = m_stringPool.GetCStr(kv.second);
				rule.restat = (strcmp(valueStr, "1") == 0 || strcmp(valueStr, "true") == 0);
			}
			else if (kv.first == m_generatorId)
			{
				const char* valueStr = m_stringPool.GetCStr(kv.second);
				rule.generator = (strcmp(valueStr, "1") == 0 || strcmp(valueStr, "true") == 0);
			}
		}

		u32 idx = u32(m_rules.size());
		m_ruleMap[rule.name] = idx;
		m_rules.emplace_back(std::move(rule));

		return true;
	}

	bool NinjaParser::ParseBuild(ParseContext& ctx, const char* line, u32 lineLen)
	{
		// Parse "build <outputs>: <rule> <inputs> [| <implicit>] [|| <order-only>]" line
		NinjaBuildEdge edge;

		// Helpers for the token arena. Each edge's token lists (outputs,
		// implicitOutputs, inputs, implicitDeps, orderOnlyDeps) get populated
		// sequentially — so they end up as contiguous slices in the arena.
		// We never allocate per-push heap blocks; commits happen lazily as
		// writtenSize advances past the committed tail.
		auto arenaTop = [&]() -> u32* {
			return reinterpret_cast<u32*>(m_tokenArena.memory + m_tokenArena.writtenSize);
		};
		auto pushTokenToArena = [&](u32 tokenId) {
			u32* slot = (u32*)m_tokenArena.AllocateNoLock(sizeof(u32), alignof(u32), TC(""));
			*slot = tokenId;
		};
		auto makeSpan = [](u32* start, u32* stop) -> TokenSpan {
			return { start, u32(stop - start) };
		};

		const char* p = line + 6; // Skip "build "
		const char* end = line + lineLen;

		// Find ':' separating outputs from rule name.
		// Must skip $: escape sequences — escaped colons are part of output names, not the separator.
		// (e.g. "build chrome$:visual_elements_resources: phony ..." has two colons; the first is escaped)
		const char* colonPos = p;
		while (colonPos < end)
		{
			if (*colonPos == ':')
				break;
			if (*colonPos == '$' && colonPos + 1 < end)
				colonPos++;  // Skip the char following '$' (handles $:, $space, $$, $\n, etc.)
			colonPos++;
		}
		if (colonPos >= end)
			return false;

		// Check for implicit outputs: "build out | implicitout: rule ..."
		// The '|' (not '||') before the ':' separates regular from implicit outputs.
		// We don't need to escape-scan here since '$|' is not valid ninja syntax.
		const char* pipePosInOutputs = nullptr;
		for (const char* q = p; q < colonPos - 1; ++q)
		{
			if (q[0] == '|' && q[1] != '|')
			{
				pipePosInOutputs = q;
				break;
			}
		}

		// SplitPaths still pushes tokens into a Vector<u32> scratch; we then
		// copy the result into the arena as one contiguous span per list.
		// The scratch is the instance-level m_tokenBuffer recycled as a u32
		// staging area via a thread_local Vector<u32> would be messier — a
		// small local vector is fine since counts per edge are small.
		Vector<u32> stage;
		if (pipePosInOutputs)
		{
			stage.clear();
			SplitPaths(p, u32(pipePosInOutputs - p), stage);
			u32* s = arenaTop();
			for (u32 id : stage) pushTokenToArena(id);
			edge.outputs = makeSpan(s, arenaTop());

			const char* implStart = pipePosInOutputs + 1;
			stage.clear();
			SplitPaths(implStart, u32(colonPos - implStart), stage);
			s = arenaTop();
			for (u32 id : stage) pushTokenToArena(id);
			edge.implicitOutputs = makeSpan(s, arenaTop());
		}
		else
		{
			stage.clear();
			SplitPaths(p, u32(colonPos - p), stage);
			u32* s = arenaTop();
			for (u32 id : stage) pushTokenToArena(id);
			edge.outputs = makeSpan(s, arenaTop());
		}
		p = colonPos + 1;

		// Skip whitespace after ':'
		while (p < end && (*p == ' ' || *p == '\t'))
			++p;

		// Parse rule name
		const char* ruleStart = p;
		while (p < end && *p != ' ' && *p != '\t')
			++p;

		edge.ruleName = m_stringPool.Intern(ruleStart, u32(p - ruleStart));

		// Parse inputs and dependencies with escape sequence support.
		// Each section's tokens are pushed directly to the token arena; we
		// record (start, current top) per section so the resulting TokenSpans
		// point at the right slice. Within a single edge these sections are
		// filled sequentially so each slice is contiguous.
		//
		// Delimiters: `|`  -> implicit deps
		//             `||` -> order-only deps
		//             `|@` -> validations (AOSP extension; stock ninja doesn't
		//                      emit this so the section stays empty for
		//                      upstream manifests — backward compatible)
		enum { INPUTS, IMPLICIT, ORDER_ONLY, VALIDATIONS, NUM_SECTIONS } state = INPUTS;
		u32* sectionStart[NUM_SECTIONS] = { nullptr, nullptr, nullptr, nullptr };
		u32* sectionEnd  [NUM_SECTIONS] = { nullptr, nullptr, nullptr, nullptr };
		sectionStart[INPUTS] = arenaTop();

		// Use instance token buffer - pre-allocated in ctor, avoids alloc/memset per edge
		u32 tokenSize = 0;
		// Track whether the current token contains any '.'. Dot-free tokens skip StripDotSlash entirely.
		bool tokenHasDot = false;

		while (p < end)
		{
			if (*p == '$' && p + 1 < end)
			{
				// Ninja escape sequence: $<char> where char can be space, :, $, newline
				++p;
				if (*p == ' ' || *p == ':' || *p == '$')
				{
					// Escaped special character - add the literal character
					if (tokenSize >= m_tokenBuffer.size())
						m_tokenBuffer.resize(tokenSize * 2);
					m_tokenBuffer[tokenSize++] = *p;
					++p;
				}
				else if (*p == '\n' || *p == '\r')
				{
					// Line continuation - skip
					++p;
					if (p < end && p[-1] == '\r' && *p == '\n')
						++p;  // Skip CRLF
				}
				else
				{
					// Not a recognized escape, keep the $
					if (tokenSize >= m_tokenBuffer.size())
						m_tokenBuffer.resize(tokenSize * 2);
					m_tokenBuffer[tokenSize++] = '$';
				}
			}
			else if (*p == ' ' || *p == '\t')
			{
				// Space/tab separates tokens (unless escaped)
				if (tokenSize > 0)
				{
					if (tokenHasDot)
						tokenSize = StripDotSlash(m_tokenBuffer.data(), tokenSize);
					if (tokenSize > 0)
					{
						u32 tokenId = m_stringPool.Intern(m_tokenBuffer.data(), tokenSize);
						pushTokenToArena(tokenId);
					}
					tokenSize = 0;
					tokenHasDot = false;
				}
				++p;
			}
			else if (*p == '|')
			{
				if (tokenSize > 0)
				{
					if (tokenHasDot)
						tokenSize = StripDotSlash(m_tokenBuffer.data(), tokenSize);
					if (tokenSize > 0)
					{
						u32 tokenId = m_stringPool.Intern(m_tokenBuffer.data(), tokenSize);
						pushTokenToArena(tokenId);
					}
					tokenSize = 0;
					tokenHasDot = false;
				}

				// Close out the previous section and open the next, both at
				// the current arena top (contiguous — no token gaps).
				sectionEnd[state] = arenaTop();

				// Disambiguate `|`, `||`, and `|@` (AOSP validations).
				if (p + 1 < end && p[1] == '|')
				{
					state = ORDER_ONLY;
					p += 2;
				}
				else if (p + 1 < end && p[1] == '@')
				{
					state = VALIDATIONS;
					p += 2;
				}
				else
				{
					state = IMPLICIT;
					++p;
				}
				sectionStart[state] = arenaTop();

				// Skip whitespace
				while (p < end && (*p == ' ' || *p == '\t'))
					++p;
			}
			else
			{
				// Bulk scan using delimiter lookup table (replaces 7 comparisons per char with 1 lookup).
				// One branchless OR per byte to flag any '.' — lets the flush path skip StripDotSlash.
				const char* tokenStart = p;
				while (p < end && !g_isDelimiter[(u8)*p])
				{
					tokenHasDot |= (*p == '.');
					++p;
				}

				u32 len = u32(p - tokenStart);
				if (len > 0)
				{
					if (tokenSize + len > m_tokenBuffer.size())
						m_tokenBuffer.resize((tokenSize + len) * 2);
					memcpy(m_tokenBuffer.data() + tokenSize, tokenStart, len);
					tokenSize += len;
				}
				else if (p < end)
				{
					// Zero-length scan - must advance to avoid infinite loop
					++p;
				}
			}
		}

		// Final token
		if (tokenSize > 0)
		{
			if (tokenHasDot)
				tokenSize = StripDotSlash(m_tokenBuffer.data(), tokenSize);
			if (tokenSize > 0)
			{
				u32 tokenId = m_stringPool.Intern(m_tokenBuffer.data(), tokenSize);
				pushTokenToArena(tokenId);
			}
		}

		// Close the active section and build the TokenSpans.
		sectionEnd[state] = arenaTop();
		edge.inputs        = makeSpan(sectionStart[INPUTS],      sectionEnd[INPUTS]);
		edge.implicitDeps  = makeSpan(sectionStart[IMPLICIT],    sectionEnd[IMPLICIT]);
		edge.orderOnlyDeps = makeSpan(sectionStart[ORDER_ONLY],  sectionEnd[ORDER_ONLY]);
		edge.validations   = makeSpan(sectionStart[VALIDATIONS], sectionEnd[VALIDATIONS]);

		// Read indented variables
		if (!ReadIndentedLines(ctx, edge.variables))
			return false;

		// Store scope ID for variable lookup
		edge.scopeId = ctx.scopeId;

		// AOSP extension: `phony_output = true` marks virtual outputs.
		// Lookup via the pre-interned id is O(1); stock-ninja edges just miss.
		{
			auto it = edge.variables.find(m_phonyOutputId);
			if (it != edge.variables.end())
			{
				const char* v = m_stringPool.GetCStr(it->second);
				edge.phonyOutput = (strcmp(v, "1") == 0 || strcmp(v, "true") == 0);
			}
		}

		u32 edgeIndex = u32(m_edges.size());
		m_edges.emplace_back(std::move(edge));

		// Call streaming callback if set (allows processing edge immediately)
		if (m_edgeParsedCallback)
			m_edgeParsedCallback(m_edgeCallbackUserData, edgeIndex, m_edges[edgeIndex]);

		return true;
	}

	bool NinjaParser::ParseVariable(ParseContext& ctx, const char* line, u32 lineLen)
	{
		// Find equals sign
		const char* eq = line;
		const char* end = line + lineLen;

		while (eq < end && *eq != '=')
			++eq;

		if (eq >= end)
			return false;

		// Variable name
		const char* nameStart = line;
		const char* nameEnd = eq;

		// Trim trailing whitespace from name
		while (nameEnd > nameStart && (nameEnd[-1] == ' ' || nameEnd[-1] == '\t'))
			--nameEnd;

		// Value
		const char* valueStart = eq + 1;

		// Skip leading whitespace
		while (valueStart < end && (*valueStart == ' ' || *valueStart == '\t'))
			++valueStart;

		// Trim trailing whitespace from value
		while (end > valueStart && (end[-1] == ' ' || end[-1] == '\t'))
			--end;

		u32 nameId = m_stringPool.Intern(nameStart, u32(nameEnd - nameStart));
		u32 valueId = m_stringPool.Intern(valueStart, u32(end - valueStart));

		// Store in appropriate scope
		if (ctx.scopeId == 0)
			m_globalVars[nameId] = valueId;
		else
			m_scopeVars[ctx.scopeId - 1][nameId] = valueId;
		return true;
	}

	bool NinjaParser::ParseDefault(ParseContext& ctx, const char* line, u32 lineLen)
	{
		const char* p = line + 8; // Skip "default "
		SplitPaths(p, u32(line + lineLen - p), m_defaults);
		return true;
	}

	bool NinjaParser::ParsePool(ParseContext& ctx, const char* line, u32 lineLen)
	{
		// Extract pool name from "pool <name>" line
		const char* nameStart = line + 5; // Skip "pool "
		const char* nameEnd = nameStart;
		while (nameEnd < line + lineLen && *nameEnd != ' ' && *nameEnd != '\t')
			++nameEnd;

		u32 nameId = m_stringPool.Intern(nameStart, u32(nameEnd - nameStart));

		NinjaPool pool;
		pool.name = nameId;

		// Read indented properties
		UnorderedMap<u32, u32> props;
		if (!ReadIndentedLines(ctx, props))
			return false;

		// Extract depth property using pre-interned ID
		auto it = props.find(m_depthId);
		if (it != props.end())
		{
			const char* depthStr = m_stringPool.GetCStr(it->second);
			pool.depth = (u32)atoi(depthStr);
		}

		u32 idx = u32(m_pools.size());
		m_poolMap[pool.name] = idx;
		m_pools.push_back(pool);

		return true;
	}

	bool NinjaParser::ParseInclude(ParseContext& ctx, const char* line, u32 lineLen)
	{
		const char* p = line + 8; // Skip "include "
		const char* end = line + lineLen;
		while (p < end && (*p == ' ' || *p == '\t'))
			++p;

		// Make absolute path
		StringBuffer<512> fullPath;
		bool isAbsolute = (p < end && (p[0] == '/' || (p[1] == ':'))); // Unix absolute or Windows drive letter

		if (!isAbsolute)
		{
			CharStringView workingDir = m_stringPool.GetString(ctx.workingDirId);
			fullPath.Append(workingDir.data, workingDir.length);
		}
		fullPath.Append(p, u32(end - p));

		// Convert char* path to tchar* for file opening
		StringBuffer<512> fullPathW;
		for (u32 i = 0; i < fullPath.count; ++i)
			fullPathW.Append((tchar)(u8)fullPath.data[i]);

		// Parse included file with current scope (includes don't create new scopes)
		return ParseFile(ctx.logger, fullPathW.data, ctx.scopeId);
	}

	bool NinjaParser::ParseSubninja(ParseContext& ctx, const char* line, u32 lineLen)
	{
		const char* p = line + 9; // Skip "subninja "
		const char* end = line + lineLen;
		while (p < end && (*p == ' ' || *p == '\t'))
			++p;

		// Make absolute path
		StringBuffer<512> fullPath;
		bool isAbsolute = (p < end && (p[0] == '/' || (p[1] == ':')));

		if (!isAbsolute)
		{
			// Subninja paths are relative to the build root (top-level build.ninja directory),
			// not to the current file's directory. This matches real ninja and GN's generated output.
			CharStringView buildRootDir = m_stringPool.GetString(m_buildRootDirId);
			fullPath.Append(buildRootDir.data, buildRootDir.length);
		}
		fullPath.Append(p, u32(end - p));

		// Convert char* path to tchar* for file opening
		StringBuffer<512> fullPathW;
		for (u32 i = 0; i < fullPath.count; ++i)
			fullPathW.Append((tchar)(u8)fullPath.data[i]);

		// Create new scope for subninja file. Variables defined here don't
		// leak into the parent, but the subninja CAN read the parent's
		// variables — record the parent so ExpandVariables can walk up.
		u32 newScopeId = u32(m_scopeVars.size()) + 1;
		m_scopeVars.emplace_back();
		m_scopeParents.push_back(ctx.scopeId); // parent: the scope that issued the subninja

		return ParseFile(ctx.logger, fullPathW.data, newScopeId);
	}

	bool NinjaParser::ReadLine(ParseContext& ctx, const char*& outLine, u32& outLen)
	{
		// Skip empty lines and comments
		while (ctx.pos < ctx.size)
		{
			const char* lineStart = ctx.data + ctx.pos;
			const char* fileEnd = ctx.data + ctx.size;
			u64 remaining = ctx.size - ctx.pos;

			// Fast newline search using vectorized memchr (much faster than byte-by-byte)
			const char* lf = (const char*)memchr(lineStart, '\n', remaining);
			const char* lineEnd;
			if (lf)
			{
				lineEnd = lf;
				ctx.pos = u64(lf + 1 - ctx.data);  // Skip past \n
			}
			else
			{
				// No \n: check for standalone \r (rare, old Mac line endings)
				const char* cr = (const char*)memchr(lineStart, '\r', remaining);
				lineEnd = cr ? cr : fileEnd;
				ctx.pos = u64(lineEnd - ctx.data);
				if (ctx.pos < ctx.size) ++ctx.pos;
			}

			// Strip trailing \r for \r\n line endings
			if (lineEnd > lineStart && lineEnd[-1] == '\r')
				--lineEnd;

			++ctx.line;

			// Trim leading whitespace
			while (lineStart < lineEnd && (*lineStart == ' ' || *lineStart == '\t'))
				++lineStart;

			// Trim trailing whitespace
			while (lineEnd > lineStart && (lineEnd[-1] == ' ' || lineEnd[-1] == '\t'))
				--lineEnd;

			// Skip empty lines and comments
			if (lineStart >= lineEnd || *lineStart == '#')
				continue;

			// Fast path: no line continuation
			if (lineEnd[-1] != '$')
			{
				outLine = lineStart;
				outLen = u32(lineEnd - lineStart);
				return true;
			}

			// Slow path: $ at end of line means continuation onto the next physical line.
			// Merge all continuation segments into ctx.continuationBuf.
			// clear() resets size to 0 but keeps allocated capacity, so after the first
			// continuation in a file there are no further heap allocations.
			ctx.continuationBuf.clear();

			const char* segStart = lineStart;
			const char* segEnd   = lineEnd - 1; // drop the trailing '$'

			for (;;)
			{
				u32 oldSize = u32(ctx.continuationBuf.size());
				u32 segLen  = u32(segEnd - segStart);
				ctx.continuationBuf.resize(oldSize + segLen);
				memcpy(ctx.continuationBuf.data() + oldSize, segStart, segLen);

				// Are there more physical lines to consume?
				if (ctx.pos >= ctx.size)
					break;

				// Read the next physical line inline (same logic as the outer loop).
				const char* nextStart = ctx.data + ctx.pos;
				u64 nextRemaining = ctx.size - ctx.pos;

				const char* nextLf = (const char*)memchr(nextStart, '\n', nextRemaining);
				const char* nextEnd;
				if (nextLf)
				{
					nextEnd = nextLf;
					ctx.pos = u64(nextLf + 1 - ctx.data);
				}
				else
				{
					const char* cr = (const char*)memchr(nextStart, '\r', nextRemaining);
					nextEnd = cr ? cr : fileEnd;
					ctx.pos = u64(nextEnd - ctx.data);
					if (ctx.pos < ctx.size) ++ctx.pos;
				}

				if (nextEnd > nextStart && nextEnd[-1] == '\r')
					--nextEnd;

				++ctx.line;

				// Trim leading whitespace (continuation indent)
				while (nextStart < nextEnd && (*nextStart == ' ' || *nextStart == '\t'))
					++nextStart;

				// Trim trailing whitespace
				while (nextEnd > nextStart && (nextEnd[-1] == ' ' || nextEnd[-1] == '\t'))
					--nextEnd;

				if (nextStart >= nextEnd)
					break; // blank continuation line — stop merging

				if (nextEnd[-1] == '$')
				{
					segStart = nextStart;
					segEnd   = nextEnd - 1; // drop the trailing '$', keep looping
				}
				else
				{
					segStart = nextStart;
					segEnd   = nextEnd;
					// Append final segment and exit loop
					oldSize = u32(ctx.continuationBuf.size());
					segLen  = u32(segEnd - segStart);
					ctx.continuationBuf.resize(oldSize + segLen);
					memcpy(ctx.continuationBuf.data() + oldSize, segStart, segLen);
					break;
				}
			}

			outLine = ctx.continuationBuf.data();
			outLen  = u32(ctx.continuationBuf.size());
			return true;
		}

		return false;
	}

	bool NinjaParser::ReadIndentedLines(ParseContext& ctx, UnorderedMap<u32, u32>& vars)
	{
		while (ctx.pos < ctx.size)
		{
			// Peek at next character
			if (ctx.data[ctx.pos] != ' ' && ctx.data[ctx.pos] != '\t')
				break;

			const char* line;
			u32 lineLen;
			if (!ReadLine(ctx, line, lineLen))
				break;

			// Parse "name = value"
			const char* eq = line;
			const char* end = line + lineLen;

			while (eq < end && *eq != '=')
				++eq;

			if (eq >= end)
				continue;

			const char* nameStart = line;
			const char* nameEnd = eq;
			while (nameEnd > nameStart && (nameEnd[-1] == ' ' || nameEnd[-1] == '\t'))
				--nameEnd;

			const char* valueStart = eq + 1;
			while (valueStart < end && (*valueStart == ' ' || *valueStart == '\t'))
				++valueStart;

			while (end > valueStart && (end[-1] == ' ' || end[-1] == '\t'))
				--end;

			u32 nameId = m_stringPool.Intern(nameStart, u32(nameEnd - nameStart));
			u32 valueId = m_stringPool.Intern(valueStart, u32(end - valueStart));
			vars[nameId] = valueId;
		}

		return true;
	}

	void NinjaParser::SkipWhitespace(const char*& p, const char* end)
	{
		while (p < end && (*p == ' ' || *p == '\t'))
			++p;
	}

	void NinjaParser::SplitPaths(const char* str, u32 len, Vector<u32>& out)
	{
		const char* p = str;
		const char* end = str + len;
		// Use instance buffer with logical-size tracking (same pattern as ParseBuild)
		u32 tokenSize = 0;
		bool tokenHasDot = false;

		while (p < end)
		{
			if (*p == '$' && p + 1 < end)
			{
				++p;
				if (*p == ' ' || *p == ':' || *p == '$')
				{
					if (tokenSize >= m_tokenBuffer.size())
						m_tokenBuffer.resize(tokenSize * 2 + 1);
					m_tokenBuffer[tokenSize++] = *p++;
				}
				else if (*p == '\n' || *p == '\r')
				{
					++p;
					if (p < end && p[-1] == '\r' && *p == '\n')
						++p;
				}
				else
				{
					if (tokenSize >= m_tokenBuffer.size())
						m_tokenBuffer.resize(tokenSize * 2 + 1);
					m_tokenBuffer[tokenSize++] = '$';
				}
			}
			else if (*p == ' ' || *p == '\t')
			{
				if (tokenSize > 0)
				{
					if (tokenHasDot)
						tokenSize = StripDotSlash(m_tokenBuffer.data(), tokenSize);
					if (tokenSize > 0)
						out.push_back(m_stringPool.Intern(m_tokenBuffer.data(), tokenSize));
					tokenSize = 0;
					tokenHasDot = false;
				}
				++p;
			}
			else
			{
				// Bulk scan using delimiter lookup table + memcpy (same as ParseBuild hot path)
				const char* tokenStart = p;
				while (p < end && !g_isDelimiter[(u8)*p])
				{
					tokenHasDot |= (*p == '.');
					++p;
				}
				u32 scanLen = u32(p - tokenStart);
				if (scanLen > 0)
				{
					if (tokenSize + scanLen > m_tokenBuffer.size())
						m_tokenBuffer.resize((tokenSize + scanLen) * 2);
					memcpy(m_tokenBuffer.data() + tokenSize, tokenStart, scanLen);
					tokenSize += scanLen;
				}
				else if (p < end)
				{
					++p;  // Unknown delimiter - advance to avoid infinite loop
				}
			}
		}

		if (tokenSize > 0)
		{
			if (tokenHasDot)
				tokenSize = StripDotSlash(m_tokenBuffer.data(), tokenSize);
			if (tokenSize > 0)
				out.push_back(m_stringPool.Intern(m_tokenBuffer.data(), tokenSize));
		}
	}

	void NinjaParser::ExpandCommand(const NinjaBuildEdge& edge, Vector<char>& out, Vector<char>& tempBuffer)
	{
		// Find rule
		auto it = m_ruleMap.find(edge.ruleName);
		if (it == m_ruleMap.end())
			return;

		const NinjaRule& rule = m_rules[it->second];

		// Create variable map with edge-specific values
		UnorderedMap<u32, u32> vars = edge.variables;

		// Build $in - caller-provided tempBuffer is used as per-path scratch (thread-safe)
		Vector<char> inStr, outStr, rspfilePath;
		inStr.reserve(262144);
		for (u32 i = 0; i < edge.inputs.size(); ++i)
		{
			if (i > 0)
				inStr.push_back(' ');

			CharStringView input = m_stringPool.GetString(edge.inputs[i]);

			tempBuffer.clear();
			ExpandVariables(input.data, input.length, tempBuffer, nullptr, edge.scopeId);
			// Canonicalize: strip leading "./" produced by variable expansion (e.g. $root = ".")
			u32 expandedLen = u32(tempBuffer.size());
			expandedLen = StripDotSlash(tempBuffer.data(), expandedLen);
			tempBuffer.resize(expandedLen);
			bool hasSpace = memchr(tempBuffer.data(), ' ', tempBuffer.size()) != nullptr;
			if (hasSpace) inStr.push_back('"');
			inStr.insert(inStr.end(), tempBuffer.begin(), tempBuffer.end());
			if (hasSpace) inStr.push_back('"');
		}

		vars[m_inId] = m_stringPool.InternThreadSafe(inStr.data(), u32(inStr.size()));

		// Build $out
		outStr.reserve(262144);
		for (u32 i = 0; i < edge.outputs.size(); ++i)
		{
			if (i > 0)
				outStr.push_back(' ');

			CharStringView output = m_stringPool.GetString(edge.outputs[i]);

			tempBuffer.clear();
			ExpandVariables(output.data, output.length, tempBuffer, nullptr, edge.scopeId);
			// Canonicalize: strip leading "./" produced by variable expansion (e.g. $root = ".")
			u32 expandedLen = u32(tempBuffer.size());
			expandedLen = StripDotSlash(tempBuffer.data(), expandedLen);
			tempBuffer.resize(expandedLen);
			bool hasSpace = memchr(tempBuffer.data(), ' ', tempBuffer.size()) != nullptr;
			if (hasSpace) outStr.push_back('"');
			outStr.insert(outStr.end(), tempBuffer.begin(), tempBuffer.end());
			if (hasSpace) outStr.push_back('"');
		}

		vars[m_outId] = m_stringPool.InternThreadSafe(outStr.data(), u32(outStr.size()));

		// Add $rspfile variable if rule uses response file
		if (rule.rspfile != StringId_Empty)
		{
			CharStringView rspfileStr = m_stringPool.GetString(rule.rspfile);
			rspfilePath.reserve(512);
			ExpandVariables(rspfileStr.data, rspfileStr.length, rspfilePath, &vars, edge.scopeId);
			vars[m_rspfileId] = m_stringPool.InternThreadSafe(rspfilePath.data(), u32(rspfilePath.size()));
		}

		// Expand command
		CharStringView commandStr = m_stringPool.GetString(rule.command);
		ExpandVariables(commandStr.data, commandStr.length, out, &vars, edge.scopeId, /*skipVarId=*/StringId_Empty, /*shellEscapeDollarSpace=*/false);
	}

	void NinjaParser::ExpandVariables(const char* str, u32 len, Vector<char>& out, const UnorderedMap<u32, u32>* edgeVars, u32 scopeId, u32 skipVarId, bool shellEscapeDollarSpace)
	{
		// Fast check for variables using vectorized memchr
		if (!memchr(str, '$', len))
		{
			// Fast path: no variables, bulk copy
			u32 oldSize = u32(out.size());
			out.resize(oldSize + len);
			memcpy(&out[oldSize], str, len);
			return;
		}

		const char* p = str;
		const char* end = str + len;
		const char* literalStart = p;  // Track start of literal sections

		while (p < end)
		{
			if (*p == '$')
			{
				// Copy any literal section before this variable
				if (p > literalStart)
				{
					u32 literalLen = u32(p - literalStart);
					u32 oldSize = u32(out.size());
					out.resize(oldSize + literalLen);
					memcpy(&out[oldSize], literalStart, literalLen);
				}

				++p;
				if (p >= end)
					break;

				// Handle special cases
				if (*p == '$')
				{
					out.push_back('$');
					++p;
					literalStart = p;  // Reset literal start
					continue;
				}
				else if (*p == ' ')
				{
					// Ninja's dollar-space is an escaped space inside a single token.
					// When building a shell command, emit backslash-space so that
					// ParseArguments treats it as an embedded space (not a separator).
					if (shellEscapeDollarSpace)
						out.push_back('\\');
					out.push_back(' ');
					++p;
					literalStart = p;
					continue;
				}
				else if (*p == ':')
				{
					out.push_back(':');
					++p;
					literalStart = p;
					continue;
				}

				// Variable reference
				const char* varStart = p;
				const char* varEnd = p;

				if (*p == '{')
				{
					++varStart;
					++p;
					while (p < end && *p != '}')
						++p;
					varEnd = p;
					if (p < end)
						++p; // Skip closing brace
				}
				else
				{
					while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
					       (*p >= '0' && *p <= '9') || *p == '_'))
					{
						++p;
					}
					varEnd = p;
				}

				u32 varLen = u32(varEnd - varStart);
				if (varLen == 0)
					continue;

				// Look up variable (3-level: edge vars → scope vars → global vars)
				u32 varNameId = m_stringPool.Intern(varStart, varLen);
				u32 valueId = StringId_Empty;

				// 1. Check edge-specific variables first (skip if this is the variable we're expanding to avoid infinite loop)
				if (edgeVars && varNameId != skipVarId)
				{
					auto it = edgeVars->find(varNameId);
					if (it != edgeVars->end())
						valueId = it->second;
				}

				// 2. Walk up the scope chain: current subninja -> parent -> ...
				//    Ninja semantics: a subninja inherits its parent's variables.
				//    Android's Soong generates multi-level subninjas where common
				//    variables (e.g. `g.bpf.relPwd = PWD=/proc/self/cwd`) live in
				//    a middle scope and are consumed by edges deeper down the tree.
				{
					u32 s = scopeId;
					while (valueId == StringId_Empty && s > 0)
					{
						auto it = m_scopeVars[s - 1].find(varNameId);
						if (it != m_scopeVars[s - 1].end())
						{
							valueId = it->second;
							break;
						}
						s = (s - 1) < m_scopeParents.size() ? m_scopeParents[s - 1] : 0;
					}
				}

				// 3. Check global variables
				if (valueId == StringId_Empty)
				{
					auto it = m_globalVars.find(varNameId);
					if (it != m_globalVars.end())
						valueId = it->second;
				}

				// Expand value recursively, passing varNameId to prevent circular edge var references
				if (valueId != StringId_Empty)
				{
					CharStringView value = m_stringPool.GetString(valueId);
					ExpandVariables(value.data, value.length, out, edgeVars, scopeId, varNameId, shellEscapeDollarSpace);
				}

				// Reset literal start after variable expansion
				literalStart = p;
			}
			else
			{
				// Regular character - will be copied in bulk with next literal section
				++p;
			}
		}

		// Copy any remaining literal section
		if (p > literalStart)
		{
			u32 literalLen = u32(p - literalStart);
			u32 oldSize = u32(out.size());
			out.resize(oldSize + literalLen);
			memcpy(&out[oldSize], literalStart, literalLen);
		}
	}
}
