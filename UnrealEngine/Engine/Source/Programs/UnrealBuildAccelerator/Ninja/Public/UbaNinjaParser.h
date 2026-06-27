// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaNinjaStringPool.h"
#include "UbaUnorderedMap.h"

namespace uba
{
	class Logger;

	// Fast ninja file parser optimized for large build files
	// Uses integer string IDs instead of raw pointers for performance (like Ninja)
	struct NinjaPool
	{
		u32 name = StringId_Empty;
		u32 depth = 0;
	};

	struct NinjaRule
	{
		u32 name = StringId_Empty;
		u32 command = StringId_Empty;
		u32 description = StringId_Empty;
		u32 depfile = StringId_Empty;
		u32 deps = StringId_Empty; // "gcc", "msvc", or empty
		u32 rspfile = StringId_Empty;
		u32 rspfile_content = StringId_Empty;
		u32 pool = StringId_Empty;
		bool restat = false;
		bool generator = false;
	};

	// Lightweight span into a bump-allocated arena of u32 tokens owned by
	// NinjaParser. The arena never moves or reallocates, so storing raw
	// pointers is safe for the lifetime of the parser. Drop-in for the
	// previous Vector<u32> fields: empty / size / operator[] / begin / end
	// / data all work, and range-for iterates tokens directly.
	struct TokenSpan
	{
		const u32* tokens = nullptr;
		u32        count  = 0;

		bool          empty() const { return count == 0; }
		u32           size()  const { return count; }
		const u32&    operator[](u32 i) const { return tokens[i]; }
		const u32*    data()  const { return tokens; }
		const u32*    begin() const { return tokens; }
		const u32*    end()   const { return tokens + count; }
	};

	struct NinjaBuildEdge
	{
		NinjaBuildEdge() = default;
		NinjaBuildEdge(NinjaBuildEdge&&) = default;
		// All five token lists are backed by NinjaParser's token arena.
		// Within one ParseBuild call the lists are populated sequentially
		// (outputs, implicitOutputs, inputs, implicitDeps, orderOnlyDeps)
		// so each list's span is a contiguous range in the arena — no
		// scratch buffer needed and zero per-edge allocations.
		TokenSpan outputs;
		TokenSpan implicitOutputs; // After | on output side (e.g. build out | foo.lib: link ...)
		TokenSpan inputs;
		TokenSpan implicitDeps;    // After | on input side
		TokenSpan orderOnlyDeps;   // After || on input side
		// AOSP ninja extension: `|@ <paths>` on the build line declares
		// "validation" edges — they're scheduled as side effects of this
		// edge but don't block its consumers. Ninja-upstream doesn't have
		// this; stock ninja files never contain `|@`, so the code path
		// below just leaves this span empty for them.
		TokenSpan validations;
		u32 ruleName = StringId_Empty;
		UnorderedMap<u32, u32> variables; // Edge-specific variable overrides (name ID -> value ID)
		u32 edgeIndex = ~0u; // Index in parsed order for dependency tracking
		u32 scopeId = 0; // File scope ID for variable lookup (0 = main/global scope)
		// AOSP ninja extension: `phony_output = true` marks an edge whose
		// outputs are virtual markers, not files on disk. Stock ninja files
		// don't set this variable, so edges parsed from upstream-generated
		// manifests keep it false. Treated like restat for freshness — log
		// recorded mtime is the reference, and the stat-existence check is
		// skipped (there's nothing on disk to stat).
		bool phonyOutput = false;
	};

	// Callback for streaming edge processing during parsing
	using EdgeParsedCallback = void(*)(void* userData, u32 edgeIndex, const NinjaBuildEdge& edge);

	class NinjaParser
	{
	public:
		NinjaParser(MemoryBlock& memoryBlock);

		// Parse a ninja file. `buildRoot` (if non-null) is the anchor directory for
		// subninja/include path resolution — matches ninja's convention that paths
		// in the build file are relative to the cwd where ninja was invoked
		// (including any `-C dir` adjustment). When null, the directory containing
		// `filename` is used as the anchor, which works when cwd == file dir
		// (e.g. `-C out/Default chrome` for chromium/llvm) but is wrong when they
		// differ (e.g. Android: `-f out/combined.ninja` from the android root).
		bool Parse(Logger& logger, const tchar* filename, const tchar* buildRoot = nullptr);

		// Set callback to be called for each edge as it's parsed (for streaming)
		void SetEdgeParsedCallback(EdgeParsedCallback callback, void* userData)
		{
			m_edgeParsedCallback = callback;
			m_edgeCallbackUserData = userData;
		}

		const Vector<NinjaRule>& GetRules() const { return m_rules; }
		const Vector<NinjaBuildEdge>& GetEdges() const { return m_edges; }
		const UnorderedMap<u32, u32>& GetVariables() const { return m_globalVars; }
		const Vector<u32>& GetDefaults() const { return m_defaults; }
		const UnorderedMap<u32, u32>& GetRuleMap() const { return m_ruleMap; }
		const Vector<NinjaPool>& GetPools() const { return m_pools; }
		const StringPool& GetStringPool() const { return m_stringPool; }

		// Optimized expansion - write directly to output buffer to avoid allocations
		// Expands to tchar* for UBA (converts char* to wchar on Windows)
		void ExpandCommand(const NinjaBuildEdge& edge, Vector<char>& out, Vector<char>& tempBuffer);
		void ExpandVariables(const char* str, u32 len, Vector<char>& out, const UnorderedMap<u32, u32>* edgeVars = nullptr, u32 scopeId = 0, u32 skipVarId = StringId_Empty, bool shellEscapeDollarSpace = false);

		u64 GetParseTimeMs() const { return m_parseTimeMs; }
		u32 GetLineCount() const { return m_lineCount; }

	private:
		struct ParseContext
		{
			Logger& logger;
			const char* data;
			u64 size;
			u64 pos;
			u32 line;
			u32 workingDirId;  // String ID of working directory
			u32 scopeId;       // Current scope ID (0 = global, >0 = subninja)
			Vector<char> continuationBuf; // Scratch buffer for $ line-continuation merging; reused across calls (clear() keeps capacity)
		};

		bool ParseFile(Logger& logger, const tchar* filename, u32 scopeId = 0);
		bool ParseImpl(ParseContext& ctx);
		bool ParseRule(ParseContext& ctx, const char* line, u32 lineLen);
		bool ParseBuild(ParseContext& ctx, const char* line, u32 lineLen);
		bool ParseVariable(ParseContext& ctx, const char* line, u32 lineLen);
		bool ParseDefault(ParseContext& ctx, const char* line, u32 lineLen);
		bool ParsePool(ParseContext& ctx, const char* line, u32 lineLen);
		bool ParseInclude(ParseContext& ctx, const char* line, u32 lineLen);
		bool ParseSubninja(ParseContext& ctx, const char* line, u32 lineLen);

		bool ReadLine(ParseContext& ctx, const char*& outLine, u32& outLen);
		bool ReadIndentedLines(ParseContext& ctx, UnorderedMap<u32, u32>& vars);
		void SkipWhitespace(const char*& p, const char* end);
		void SplitPaths(const char* str, u32 len, Vector<u32>& out);

		// Reference to shared MemoryBlock (owned by caller)
		MemoryBlock& m_memoryBlock;

		// String pool for interning all strings with integer IDs
		StringPool m_stringPool;

		// Linear bump arena for all edge-token spans (outputs/inputs/etc.).
		// Reserved to 1 GB of virtual address space, commits pages lazily
		// as tokens are appended. Never reallocates; TokenSpan pointers
		// remain stable for the lifetime of the parser. One arena suffices
		// because within a single ParseBuild the token lists are filled
		// sequentially — they never interleave.
		MemoryBlock m_tokenArena;

		Vector<NinjaRule> m_rules;
		Vector<NinjaBuildEdge> m_edges;
		Vector<NinjaPool> m_pools;
		UnorderedMap<u32, u32> m_globalVars;  // global variables (scope 0)
		// File-scoped variables per subninja, indexed by scopeId-1. Ninja
		// semantics: a subninja creates a new scope that can *read* its
		// parent's variables but doesn't *write* to them. m_scopeParents[i]
		// holds the parent scopeId of scope i+1 (0 == top/global), which the
		// lookup in ExpandVariables walks up until a match or we exhaust the
		// chain. Without this, a var defined in a middle-of-chain scope is
		// invisible to edges in any of its descendant subninjas — which is
		// how Android's `${g.bpf.relPwd}` etc. resolved to empty.
		Vector<UnorderedMap<u32, u32>> m_scopeVars;
		Vector<u32>            m_scopeParents;
		Vector<u32> m_defaults;               // default target IDs
		UnorderedMap<u32, u32> m_ruleMap;     // rule name ID -> index
		UnorderedMap<u32, u32> m_poolMap;     // pool name ID -> index

		// Pre-interned string IDs for common names — initialized once in ctor,
		// avoiding repeated Intern() hash lookups in hot paths.
		u32 m_commandId;
		u32 m_descriptionId;
		u32 m_depfileId;
		u32 m_depsId;
		u32 m_rspfileId;
		u32 m_rspfile_contentId;
		u32 m_poolId;
		u32 m_restatId;
		u32 m_generatorId;
		u32 m_depthId;
		u32 m_inId;
		u32 m_outId;
		u32 m_phonyOutputId; // AOSP ninja edge variable: `phony_output = true`

		// Reusable scratch buffer — avoids repeated alloc/free in hot paths.
		// m_tokenBuffer: shared by ParseBuild and SplitPaths (never called concurrently).
		Vector<char> m_tokenBuffer;

		// Streaming callback (called for each edge as it's parsed)
		EdgeParsedCallback m_edgeParsedCallback = nullptr;
		void* m_edgeCallbackUserData = nullptr;

		u64 m_parseTimeMs = 0;
		u32 m_lineCount = 0;
		u32 m_buildRootDirId = StringId_Empty;  // Directory of top-level build.ninja; subninja paths are relative to this
	};
}
