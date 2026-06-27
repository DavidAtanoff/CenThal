// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaNinjaBuild.h"
#include "UbaCacheClient.h"
#include "UbaDirectoryIterator.h"
#include "UbaDirectoryTableHolder.h" // for GetDirKey helper
#include "UbaFile.h"
#include "UbaHash.h"
#include "UbaLogger.h"
#include "UbaNinjaChromiumWorkarounds.h"
#include "UbaNinjaDepsLog.h"
#include "UbaNinjaParser.h"
#include "UbaPathUtils.h"
#include "UbaScheduler.h"
#include "UbaSessionServer.h"
#include "UbaSynchronization.h"
#include "UbaWorkManager.h"
#include <algorithm>
#include <atomic>

namespace uba
{
	// Forward declare

	// ============================================================
	// Up-to-date check
	// ============================================================
	// Reads .ninja_log + .ninja_deps from the prior build and suppresses
	// edgesToBuildFlags for edges that are confirmed up-to-date. An edge
	// is clean when:
	//   1. All declared + implicit outputs exist on disk
	//   2. The log's recorded command hash matches the current command
	//      (so changing a compiler flag triggers rebuild)
	//   3. No declared or discovered input (from .ninja_deps) has mtime
	//      newer than the oldest output mtime
	//   4. No upstream producer is itself dirty (propagated via topoOrder)
	//
	// All directory scans run in parallel through the WorkManager. File
	// paths stay char-based (matching ninja's own representation); only
	// the tchar conversion needed by UBA's DirectoryTable is per-lookup.
	// ============================================================
	// Strip "/showIncludes" (optionally followed by a space and on through) from
	// a command buffer in place. Matches the same transform EnqueueCommands does
	// before handing arguments to UBA, so the command hash we compare against
	// .ninja_log lines up with what the writer recorded.
	static void StripShowIncludesInPlace(Vector<char>& buf)
	{
		const char* pattern = "/showIncludes";
		u32 patternLen = 13;
		u32 writePos = 0;
		for (u32 readPos = 0; readPos < buf.size(); )
		{
			if (readPos + patternLen <= buf.size() &&
				memcmp(&buf[readPos], pattern, patternLen) == 0)
			{
				while (readPos < buf.size() && buf[readPos] != ' ')
					++readPos;
				if (readPos < buf.size() && buf[readPos] == ' ')
					++readPos;
			}
			else
			{
				buf[writePos++] = buf[readPos++];
			}
		}
		buf.resize(writePos);
	}

	static u32 SuppressUpToDateEdges(
		LoggerWithWriter& logger,
		NinjaParser& parser,
		StringView workingDir,
		WorkManager& workManager,
		const Vector<u32>& topoOrder,
		NinjaParseState& state,
		bool noShowIncludes)
	{
		u64 t0 = GetTime();

		const auto& edges         = parser.GetEdges();
		const u32   edgeCount     = u32(edges.size());
		auto&       stringPool    = const_cast<StringPool&>(parser.GetStringPool());
		auto&       outputToEdge  = state.outputToEdge;
		auto&       edgesToBuildFlags = state.edgesToBuildFlags;
		const u32   phonyId       = state.phonyId;

		// -----------------------------------------------------------
		// Load previous run's .ninja_log and .ninja_deps.
		// Missing files are fine — means "first build, everything dirty".
		// -----------------------------------------------------------
		NinjaLogIndex logIndex;
		if (!logIndex.Load(logger, workingDir, workingDir))
			return 0; // parse error already reported; play it safe and rebuild all

		if (logIndex.logByOutputKey.empty() && logIndex.depsByOutputKey.empty())
		{
			logger.Info(TC("Up-to-date check: no prior build log, rebuilding everything"));
			return 0;
		}

		u64 tAfterLoad = GetTime();

		// -----------------------------------------------------------
		// Phase 1: collect the set of unique directories we need to stat.
		// We gather them from every path referenced by needed edges
		// (outputs, implicit outputs, declared inputs, implicit deps) plus
		// every discovered header path from .ninja_deps for those edges.
		// orderOnlyDeps are excluded — ninja semantics say they don't
		// affect freshness, only ordering.
		// -----------------------------------------------------------
		// Directory list for the parallel stat pass. We store absolute paths as
		// byte offsets into a shared tchar buffer so the vector itself is small
		// and stays cache-friendly even with many thousands of entries.
		struct DirEntry
		{
			StringKey key;        // hash of lowercased dir path (matches mtime-cache key scheme)
			u32       pathOffset; // into dirPathBuf (trailing separator included)
			u32       pathLen;
		};
		Vector<tchar>    dirPathBuf;
		Vector<DirEntry> uniqueDirs;
		UnorderedMap<StringKey, u32> dirKeyToIndex; // dedup
		dirPathBuf.reserve(1 * 1024 * 1024);
		uniqueDirs.reserve(16384);

		// Per-edge path key lists. For each edge we precompute, once, the
		// StringKeys for all output / implicit-output / input / implicit-dep
		// paths (with ninja variables expanded against the edge's scope).
		// Phase 3 uses these directly to query the mtime cache and the
		// logIndex — no reparsing, no re-expansion.
		struct EdgePaths
		{
			u32 outStart   = 0, outEnd   = 0; // outputs
			u32 implOStart = 0, implOEnd = 0; // implicitOutputs
			u32 inStart    = 0, inEnd    = 0; // inputs
			u32 implIStart = 0, implIEnd = 0; // implicitDeps
			StringKey primaryOutKey = {};     // hash of outputs[0] (for log lookup)
		};
		Vector<EdgePaths> edgePaths(edgeCount);
		Vector<StringKey> pathKeys; // concatenated per-edge key ranges
		pathKeys.reserve(1 << 20);

		// Scratch buffers reused across the single-threaded phase 1 loop.
		Vector<char>       expandBuf;
		StringBuffer<512>  tcharBuf;
		StringBuffer<512>  absBuf;
		expandBuf.reserve(1024);

		// Helper: register a directory by absolute tchar path (deduped). Ensures
		// the stored path always ends with a separator so Phase 2 can blindly
		// concatenate with filenames.
		auto registerDir = [&](StringView absFilePath)
		{
			StringBuffer<512> dirPath;
			const tchar*      lastSlash;
			StringKey         dirKey;
			if (!GetDirKey(dirKey, dirPath, lastSlash, absFilePath))
				return;
			auto ins = dirKeyToIndex.emplace(dirKey, u32(uniqueDirs.size()));
			if (ins.second)
			{
				dirPath.EnsureEndsWithSlash();
				u32 dirOff = u32(dirPathBuf.size());
				dirPathBuf.resize(dirOff + dirPath.count);
				memcpy(dirPathBuf.data() + dirOff, dirPath.data, dirPath.count * sizeof(tchar));
				DirEntry de;
				de.key        = dirKey;
				de.pathOffset = dirOff;
				de.pathLen    = dirPath.count;
				uniqueDirs.push_back(de);
			}
		};

		// Expand $vars against edge scope / global scope, canonicalize to abs path,
		// hash → StringKey, push into pathKeys, also register parent dir.
		auto pushPathKey = [&](u32 stringId, u32 scopeId) -> StringKey
		{
			StringKey empty{};
			CharStringView rel = stringPool.GetString(stringId);
			if (rel.length == 0)
			{
				pathKeys.push_back(empty);
				return empty;
			}

			expandBuf.clear();
			parser.ExpandVariables(rel.data, rel.length, expandBuf, nullptr, scopeId);
			if (expandBuf.empty())
			{
				pathKeys.push_back(empty);
				return empty;
			}

			u32 n = u32(expandBuf.size());
			if (n + 1 > tcharBuf.capacity) n = tcharBuf.capacity - 1;
			tcharBuf.count = n;
			for (u32 i = 0; i < n; ++i)
			{
				char c = expandBuf[i];
				tcharBuf.data[i] = (tchar)(u8)((c == '\\' || c == '/') ? PathSeparator : c);
			}
			tcharBuf.data[tcharBuf.count] = 0;

			FixPath(absBuf.Clear(), StringView(tcharBuf.data, tcharBuf.count), workingDir);
			StringKey fileKey = CaseInsensitiveFs
				? ToStringKeyLower(StringView(absBuf.data, absBuf.count))
				: ToStringKeyNoCheck(absBuf.data, absBuf.count);

			registerDir(StringView(absBuf.data, absBuf.count));

			pathKeys.push_back(fileKey);
			return fileKey;
		};

		for (u32 i = 0; i < edgeCount; ++i)
		{
			if (!edgesToBuildFlags[i]) continue;
			const auto& e = edges[i];
			if (e.ruleName == phonyId) continue;

			EdgePaths& ep = edgePaths[i];

			ep.outStart = u32(pathKeys.size());
			for (u32 j = 0; j < e.outputs.size(); ++j)
			{
				StringKey k = pushPathKey(e.outputs[j], e.scopeId);
				if (j == 0)
					ep.primaryOutKey = k;
			}
			ep.outEnd = u32(pathKeys.size());

			ep.implOStart = u32(pathKeys.size());
			for (u32 id : e.implicitOutputs) pushPathKey(id, e.scopeId);
			ep.implOEnd = u32(pathKeys.size());

			ep.inStart = u32(pathKeys.size());
			for (u32 id : e.inputs) pushPathKey(id, e.scopeId);
			ep.inEnd = u32(pathKeys.size());

			ep.implIStart = u32(pathKeys.size());
			for (u32 id : e.implicitDeps) pushPathKey(id, e.scopeId);
			ep.implIEnd = u32(pathKeys.size());

			// Register parent dirs of discovered-deps too, so their mtimes are in cache.
			if (ep.primaryOutKey.a != 0 || ep.primaryOutKey.b != 0)
			{
				auto it = logIndex.depsByOutputKey.find(ep.primaryOutKey);
				if (it != logIndex.depsByOutputKey.end())
				{
					const auto& de = it->second;
					for (u32 k = 0; k < de.depCount; ++k)
					{
						u32 off = logIndex.flatDepPathOffset[de.depOffset + k];
						u32 len = logIndex.flatDepPathLen[de.depOffset + k];
						registerDir(StringView(logIndex.flatDepPathBuf.data() + off, len));
					}
				}
			}
		}

		u64 tAfterCollect = GetTime();

		// -----------------------------------------------------------
		// Phase 2: parallel directory stat. For each unique parent
		// directory we walk it once via TraverseDir and stash the mtime
		// of every file into a shared map keyed by hash(lowercased full
		// path). Missing files are absent from the map.
		//
		// This replaces DirectoryTableHolder, which is shaped around
		// UBA's session-level bookkeeping and was over-populating caches
		// for our read-only use.
		// -----------------------------------------------------------
		UnorderedMap<StringKey, u64> mtimeByFileKey;
		mtimeByFileKey.reserve(256 * 1024);
		Futex mtimeLock;

		if (!uniqueDirs.empty())
		{
			IndexContainer container(uniqueDirs.size());
			workManager.ParallelFor(0, container, [&](const WorkContext&, auto& it)
				{
					const DirEntry& de = uniqueDirs[it.index];
					StringView dirPath(dirPathBuf.data() + de.pathOffset, de.pathLen);

					// Each worker batches its dir's entries into a local vector,
					// then holds the map lock once to insert them all.
					Vector<std::pair<StringKey, u64>> local;
					local.reserve(128);

					TraverseDir(logger, dirPath, [&](const DirectoryEntry& entry)
						{
							if (IsDirectory(entry.attributes))
								return;

							StringBuffer<512> full;
							full.Append(dirPath);
							full.Append(entry.name, entry.nameLen);

							StringKey fileKey = CaseInsensitiveFs
								? ToStringKeyLower(StringView(full.data, full.count))
								: ToStringKeyNoCheck(full.data, full.count);
							local.emplace_back(fileKey, entry.lastWritten);
						});

					if (!local.empty())
					{
						SCOPED_FUTEX(mtimeLock, lock);
						for (auto& kv : local)
							mtimeByFileKey.emplace(kv.first, kv.second);
					}
				}, TCV("UpToDateStatDir"));
		}

		u64 tAfterStat = GetTime();

		// -----------------------------------------------------------
		// Phase 3: per-edge freshness check (Pass A). Fully parallel —
		// each edge's check is an independent set of O(1) hash lookups
		// on mtimeByFileKey.
		// -----------------------------------------------------------
		Vector<u8> clean(edgeCount, 0);

		auto getMtime = [&](StringKey key) -> u64
		{
			auto it = mtimeByFileKey.find(key);
			return it == mtimeByFileKey.end() ? 0 : it->second;
		};

		// DBG: dirty-reason counters (indexed by reason id)
		std::atomic<u32> dbgReason[8]{};
		auto dbgFirstOutName = [&](u32 i) -> CharStringView {
			const auto& e = edges[i];
			return e.outputs.empty() ? CharStringView{"<noout>", 7} : stringPool.GetString(e.outputs[0]);
		};

		IndexContainer edgeContainer(edgeCount);
		workManager.ParallelFor(0, edgeContainer, [&](const WorkContext&, auto& it)
			{
				u32 i = it.index;
				if (!edgesToBuildFlags[i]) return;
				const auto& edge = edges[i];
				if (edge.ruleName == phonyId) { clean[i] = 1; return; }

				if (edge.outputs.empty() && edge.implicitOutputs.empty())
				{
					++dbgReason[0];
					return;
				}

				const EdgePaths& ep = edgePaths[i];

				// Look up the rule to check the `restat` flag. Rules with restat
				// opt into ninja semantics where the command may not update all
				// outputs on every run — e.g. `lld-link` doesn't rewrite the
				// import .lib when its exports haven't changed, so the .lib
				// mtime can be much older than the .dll. Treating these edges
				// as dirty every build defeats the point of the up-to-date
				// check. For restat rules, use MAX(output mtimes) as the
				// reference (we consider the edge "produced at its newest
				// output"). For non-restat rules, stick with MIN as ninja does.
				bool restatRule = false;
				{
					auto rmIt = parser.GetRuleMap().find(edge.ruleName);
					if (rmIt != parser.GetRuleMap().end())
						restatRule = parser.GetRules()[rmIt->second].restat;
				}
				// AOSP extension: phony_output=true edges have virtual outputs
				// (not on disk). Treat them the same as restat for freshness —
				// skip the stat-existence check and fall back on the log's
				// recorded mtime as the reference.
				const bool useLogMtime = restatRule || edge.phonyOutput;

				// --- 1. Outputs exist on disk (skipped for phony_output). ---
				bool anyMissing = false;
				u64 minOutStat = ~u64(0);
				if (!edge.phonyOutput)
				{
					for (u32 k = ep.outStart; k < ep.outEnd; ++k)
					{
						u64 t = getMtime(pathKeys[k]);
						if (!t) { anyMissing = true; break; }
						if (t < minOutStat) minOutStat = t;
					}
					if (!anyMissing)
						for (u32 k = ep.implOStart; k < ep.implOEnd; ++k)
						{
							u64 t = getMtime(pathKeys[k]);
							if (!t) { anyMissing = true; break; }
							if (t < minOutStat) minOutStat = t;
						}
					if (anyMissing || minOutStat == ~u64(0))
					{
						++dbgReason[1];
						if (dbgReason[1] <= 3)
						{
							auto n = dbgFirstOutName(i);
							logger.Detail(TC("UTD[missing-out]: %.*hs"), n.length, n.data);
						}
						return;
					}
				}

				// --- 2. Command hash check against .ninja_log ---
				auto logIt = logIndex.logByOutputKey.find(ep.primaryOutKey);
				if (logIt == logIndex.logByOutputKey.end())
				{
					++dbgReason[2];
					if (dbgReason[2] <= 3)
					{
						auto n = dbgFirstOutName(i);
						logger.Detail(TC("UTD[no-log-entry]: %.*hs"), n.length, n.data);
					}
					return;
				}

				// Reference mtime for input-freshness comparisons.
				//
				// For `restat = 1` edges: use the mtime recorded in the log.
				// Ninja semantics — the command may not rewrite every output
				// every run (e.g. `lld-link` doesn't touch the import .lib if
				// exports didn't change; proto generators stabilize on
				// identical output). The writer records "now" for restat
				// edges, so this reference reflects "when we last confirmed
				// the edge was up-to-date" rather than the possibly-stale
				// file mtime. Without this, the edge shows dirty every run.
				//
				// For non-restat edges: use the min of current output mtimes
				// (classic ninja rule).
				u64 minOut = useLogMtime ? logIt->second.outMtime : minOutStat;

				// Recompute the command hash the same way the dispatch side did:
				// hash the full ninja-level expanded command (tchar-form, after
				// /showIncludes stripping if the CLI said so). Dispatch stashes
				// this hash in breadcrumbs and the writer records it verbatim.
				Vector<char> fullCmd, temp;
				parser.ExpandCommand(edge, fullCmd, temp);
				if (fullCmd.empty())
					return;
				if (noShowIncludes)
					StripShowIncludesInPlace(fullCmd);

				TString fullT;
				fullT.resize(fullCmd.size());
				for (u32 k = 0, kn = u32(fullCmd.size()); k < kn; ++k)
					fullT[k] = (tchar)(u8)fullCmd[k];

				StringKey curCmdHash = ToStringKeyNoCheck(fullT.c_str(), u32(fullT.length()));
				if (curCmdHash.a != logIt->second.cmdHash)
				{
					++dbgReason[3];
					if (dbgReason[3] <= 3)
					{
						auto n = dbgFirstOutName(i);
						logger.Detail(TC("UTD[cmd-hash]: %.*hs cur=%llx log=%llx"), n.length, n.data,
							(unsigned long long)curCmdHash.a, (unsigned long long)logIt->second.cmdHash);
					}
					return;
				}

				// --- 3. Declared input freshness ---
				bool inputNewer = false;
				u32 whichInput = ~0u;
				u32 whichInputIdKind = 0; // 0=input, 1=implicitDep
				u32 whichInputEdgeIdx = 0;
				u64 whichInputMtime = 0;
				for (u32 k = ep.inStart; k < ep.inEnd; ++k)
				{
					u64 t = getMtime(pathKeys[k]);
					if (t > minOut) { inputNewer = true; whichInput = k - ep.inStart; whichInputIdKind = 0; whichInputEdgeIdx = k; whichInputMtime = t; break; }
				}
				if (!inputNewer)
					for (u32 k = ep.implIStart; k < ep.implIEnd; ++k)
					{
						u64 t = getMtime(pathKeys[k]);
						if (t > minOut) { inputNewer = true; whichInput = k - ep.implIStart; whichInputIdKind = 1; whichInputEdgeIdx = k; whichInputMtime = t; break; }
					}
				if (inputNewer)
				{
					++dbgReason[4];
					if (dbgReason[4] <= 3)
					{
						auto n = dbgFirstOutName(i);
						// Find the input's stringId
						u32 inputStringId = 0;
						if (whichInputIdKind == 0 && whichInput < edge.inputs.size())
							inputStringId = edge.inputs[whichInput];
						else if (whichInputIdKind == 1 && whichInput < edge.implicitDeps.size())
							inputStringId = edge.implicitDeps[whichInput];
						auto in = stringPool.GetString(inputStringId);
						logger.Detail(TC("UTD[input-newer]: %.*hs  in=\"%.*hs\"(%s) inT=%llu outT=%llu delta=%lld"),
							n.length, n.data, in.length, in.data,
							whichInputIdKind == 0 ? TC("declared") : TC("implicit"),
							(unsigned long long)whichInputMtime, (unsigned long long)minOut,
							(long long)(whichInputMtime - minOut));
						(void)whichInputEdgeIdx;
					}
					return;
				}

				// --- 4. Discovered dep (.ninja_deps) freshness ---
				auto depIt = logIndex.depsByOutputKey.find(ep.primaryOutKey);
				if (depIt != logIndex.depsByOutputKey.end())
				{
					const auto& de = depIt->second;
					u32 whichDep = ~0u;
					u64 whichDepT = 0;
					for (u32 k = 0; k < de.depCount; ++k)
					{
						u64 t = getMtime(logIndex.flatDepKeys[de.depOffset + k]);
						if (!t) continue;
						if (t > minOut) { inputNewer = true; whichDep = k; whichDepT = t; break; }
					}
					if (inputNewer)
					{
						++dbgReason[5];
						if (dbgReason[5] <= 3 && whichDep != ~0u)
						{
							auto n = dbgFirstOutName(i);
							u32 off = logIndex.flatDepPathOffset[de.depOffset + whichDep];
							u32 len = logIndex.flatDepPathLen[de.depOffset + whichDep];
							logger.Detail(TC("UTD[dep-newer]: %.*hs  dep=\"%.*s\" depT=%llu outT=%llu delta=%lld"),
								n.length, n.data,
								len, logIndex.flatDepPathBuf.data() + off,
								(unsigned long long)whichDepT, (unsigned long long)minOut,
								(long long)(whichDepT - minOut));
						}
						return;
					}
				}

				clean[i] = 1;
			}, TCV("UpToDateEdge"));

		u64 tAfterCheck = GetTime();

		// -----------------------------------------------------------
		// Phase 4: dirty propagation (Pass B). Reverse topoOrder visits
		// producers before consumers; if any producer of an edge is
		// dirty the consumer is too.
		// -----------------------------------------------------------
		for (auto it = topoOrder.rbegin(); it != topoOrder.rend(); ++it)
		{
			u32 i = *it;
			if (!edgesToBuildFlags[i] || !clean[i]) continue;

			const auto& edge = edges[i];
			auto hasDirtyProducer = [&](u32 inputId) -> bool
			{
				if (inputId >= outputToEdge.size()) return false;
				u32 p = outputToEdge[inputId];
				if (p == ~0u || p >= edgeCount || !edgesToBuildFlags[p]) return false;
				return !clean[p];
			};
			for (u32 id : edge.inputs)
				if (hasDirtyProducer(id)) { clean[i] = 0; break; }
			if (clean[i])
				for (u32 id : edge.implicitDeps)
					if (hasDirtyProducer(id)) { clean[i] = 0; break; }
		}

		// -----------------------------------------------------------
		// Phase 5: apply. Suppress build for confirmed-clean edges.
		// -----------------------------------------------------------
		u32 suppressed = 0;
		for (u32 i = 0; i < edgeCount; ++i)
		{
			if (edgesToBuildFlags[i] && clean[i])
			{
				edgesToBuildFlags[i] = 0;
				++suppressed;
			}
		}

		u32 dirty = 0;
		for (u32 i = 0; i < edgeCount; ++i)
			if (edgesToBuildFlags[i] && edges[i].ruleName != phonyId)
				++dirty;

		logger.Info(TC("Up-to-date: %u dirty, %u skipped  [%u dirs, load %s, collect %s, stat %s, check %s, total %s]"),
			dirty, suppressed, u32(uniqueDirs.size()),
			TimeToText(tAfterLoad   - t0).str,
			TimeToText(tAfterCollect - tAfterLoad).str,
			TimeToText(tAfterStat    - tAfterCollect).str,
			TimeToText(tAfterCheck   - tAfterStat).str,
			TimeToText(GetTime()     - t0).str);

		return suppressed;
	}

	bool ParseNinjaFile(LoggerWithWriter& logger, NinjaParser& parser,
	                    const tchar* ninjaFile, const Vector<TString>& specifiedTargets,
	                    NinjaParseState& outState, const tchar* buildRoot)
	{
		const auto& stringPool = parser.GetStringPool();

		outState.phonyId = const_cast<StringPool&>(stringPool).Intern("phony", 5);

		// Intern target names before parsing so their IDs exist in the string pool
		Vector<char> targetUtf8;
		if (!specifiedTargets.empty())
		{
			logger.Detail(TC("Building %u specified targets"), u32(specifiedTargets.size()));
			for (const auto& target : specifiedTargets)
			{
				targetUtf8.clear();
				for (u32 i = 0; i < target.length(); ++i)
					targetUtf8.push_back((char)(u8)target[i]);
				u32 targetId = const_cast<StringPool&>(stringPool).Intern(targetUtf8.data(), u32(targetUtf8.size()));
				if (targetId >= outState.wantedOutputs.size())
					outState.wantedOutputs.resize(targetId + 1, 0);
				outState.wantedOutputs[targetId] = 1;
			}
		}
		// else: no targets specified — resolve after parsing using the file's default statement

		outState.outputToEdge.reserve(131072);
		outState.edgesToBuildFlags.reserve(65536);
		outState.edgeToProcessIndex.reserve(65536);

		parser.SetEdgeParsedCallback([](void* userData, u32 edgeIndex, const NinjaBuildEdge& edge)
		{
			auto& state = *static_cast<NinjaParseState*>(userData);
			if (edgeIndex >= state.edgesToBuildFlags.size())
			{
				state.edgesToBuildFlags.resize(edgeIndex + 1, 0);
				state.edgeToProcessIndex.resize(edgeIndex + 1, ~0u);
			}
			for (u32 outputId : edge.outputs)
			{
				if (outputId >= state.outputToEdge.size())
				{
					u32 newSize = outputId + 1;
					state.outputToEdge.resize(newSize, ~0u);
					if (state.wantedOutputs.size() < newSize)
						state.wantedOutputs.resize(newSize, 0);
				}
				state.outputToEdge[outputId] = edgeIndex;
			}
		}, &outState);

		logger.Detail(TC(""));
		logger.Detail(TC("Parsing ninja file: %s"), ninjaFile);

		u64 parseStartTime = GetTime();
		if (!parser.Parse(logger, ninjaFile, buildRoot))
		{
			logger.Error(TC("Failed to parse ninja file"));
			return false;
		}
		u64 parseTime = GetTime() - parseStartTime;
		logger.Detail(TC("Parse completed in %s"), TimeToText(parseTime).str);
		logger.Detail(TC("  Lines: %u"), parser.GetLineCount());
		logger.Detail(TC("  Edges: %u"), u32(parser.GetEdges().size()));

		// Warn about generator edges (e.g. "build build.ninja: configure" with generator = 1).
		// Real ninja re-runs these to regenerate the build file before proceeding; UbaNinja skips them.
		// Fast path: collect generator rule name IDs from the small rules list first (typically ~100
		// entries). Only if any exist do we scan the much larger edges list — and even then with a
		// plain integer comparison per edge, not a hash lookup. In practice UE builds have zero
		// generator rules so the edge scan is skipped entirely.
		{
			const auto& rules = parser.GetRules();
			// Collect generator rule name IDs — usually empty.
			u32 generatorRuleIds[16];
			u32 generatorRuleCount = 0;
			for (const auto& rule : rules)
				if (rule.generator && generatorRuleCount < 16)
					generatorRuleIds[generatorRuleCount++] = rule.name;

			if (generatorRuleCount > 0)
			{
				for (const auto& edge : parser.GetEdges())
				{
					bool found = false;
					for (u32 i = 0; i < generatorRuleCount; ++i)
						if (edge.ruleName == generatorRuleIds[i]) { found = true; break; }
					if (!found)
						continue;

					// Find the matching rule to get its name and command for the message.
					const NinjaRule* matchedRule = nullptr;
					for (const auto& rule : rules)
						if (rule.name == edge.ruleName) { matchedRule = &rule; break; }
					if (!matchedRule) continue;

					StringBuffer<256> ruleName;
					ruleName.Append(stringPool.GetCStr(matchedRule->name));
					StringBuffer<256> command;
					command.Append(stringPool.GetCStr(matchedRule->command));
					StringBuffer<512> output;
					if (!edge.outputs.empty())
						output.Append(stringPool.GetCStr(edge.outputs[0]));
					logger.Warning(TC("Skipping generator rule '%s' (output: '%s', command: '%s'). Ninja would re-run this to regenerate the build file."),
						ruleName.data, output.data, command.data);
				}
			}
		}

		// Resolve default targets now that the file is parsed.
		// Real ninja uses the 'default' statement when no targets are specified;
		// only fall back to building everything if the file has no default statement.
		if (specifiedTargets.empty())
		{
			const auto& defaults = parser.GetDefaults();
			if (!defaults.empty())
			{
				logger.Detail(TC("No targets specified, using %u default target(s) from build.ninja"), u32(defaults.size()));
				for (u32 defaultId : defaults)
				{
					if (defaultId >= outState.wantedOutputs.size())
						outState.wantedOutputs.resize(defaultId + 1, 0);
					outState.wantedOutputs[defaultId] = 1;
				}
			}
			else
			{
				logger.Detail(TC("No targets specified and no default statement, building everything"));
				outState.wantAll = true;
			}
		}

		return true;
	}

	bool EnqueueCommands(LoggerWithWriter& logger, NinjaParser& parser, Scheduler& scheduler,
	                     NinjaParseState& state, StringView workingDir,
	                     bool rebuild, bool cleanOnly, bool noShowIncludes, bool noUpToDate,
	                     bool verbose, bool dryRun, const tchar* yamlFile,
	                     u32 cacheBucketId, bool forceRemote)
	{
		// Aliases so the rest of the function body is unchanged
		auto& outputToEdge       = state.outputToEdge;
		auto& edgesToBuildFlags  = state.edgesToBuildFlags;
		auto& edgeToProcessIndex = state.edgeToProcessIndex;
		auto& wantedOutputs      = state.wantedOutputs;
		const u32 phonyId        = state.phonyId;

		const auto& stringPool = parser.GetStringPool();
		const auto& edges      = parser.GetEdges();

		// After parsing the string pool is frozen (reads only).
		// ExpandCommand/ExpandVariables are thread-safe: StringPool::Intern is Futex-protected
		// and ExpandVariables no longer writes to any shared state.

		bool wantAll = state.wantAll;

		u64 totalStartTime = GetTime();
		u64 queueStartTime = GetTime();

		// Per-phase timing. Each tPhase is the time spent in that phase; phases
		// that don't run (e.g. clean only under -rebuild/-clean) stay 0.
		u64 tBfs = 0, tClean = 0, tTopo = 0, tUpToDate = 0;
		u64 tPass1 = 0, tPass2Dispatch = 0, tPass2Wait = 0, tYaml = 0;
		u64 tPhase = GetTime();

		// Mark which edges need to be built
		if (wantAll)
		{
			for (u32 i = 0; i < u32(edges.size()); ++i)
				edgesToBuildFlags[i] = 1;
		}
		else
		{
			// BFS backward from target outputs to find all needed edges
			Vector<u32> edgesToVisit;
			edgesToVisit.reserve(edges.size() / 4);

			for (u32 outputId = 0; outputId < wantedOutputs.size(); ++outputId)
			{
				if (!wantedOutputs[outputId] || outputId >= outputToEdge.size())
					continue;
				u32 edgeIdx = outputToEdge[outputId];
				if (edgeIdx != ~0u && edgeIdx < edgesToBuildFlags.size() && !edgesToBuildFlags[edgeIdx])
				{
					edgesToBuildFlags[edgeIdx] = 1;
					edgesToVisit.push_back(edgeIdx);
				}
			}

			logger.Detail(TC("Finding needed edges (BFS from %u targets)..."), u32(edgesToVisit.size()));

			for (u32 visitIdx = 0; visitIdx < edgesToVisit.size(); ++visitIdx)
			{
				u32 edgeIdx = edgesToVisit[visitIdx];
				const auto& edge = edges[edgeIdx];
				auto markProducer = [&](u32 inputId)
				{
					if (inputId < outputToEdge.size())
					{
						u32 producerIdx = outputToEdge[inputId];
						if (producerIdx != ~0u && producerIdx < edgesToBuildFlags.size() && !edgesToBuildFlags[producerIdx])
						{
							edgesToBuildFlags[producerIdx] = 1;
							edgesToVisit.push_back(producerIdx);
						}
					}
				};
				for (u32 id : edge.inputs)       markProducer(id);
				for (u32 id : edge.implicitDeps)  markProducer(id);
				for (u32 id : edge.orderOnlyDeps) markProducer(id);
			}

			logger.Detail(TC("Marked %u needed edges"), u32(edgesToVisit.size()));
		}

		tBfs = GetTime() - tPhase;
		tPhase = GetTime();

		WorkManager& workManager = scheduler.GetSession().GetWorkManager();

		// Clean outputs for -rebuild and -clean modes
		if (rebuild || cleanOnly)
		{
			logger.Detail(TC("Deleting output files"));
			u64 cleanStart = GetTime();

			// Delete a single declared output. Paths in edge.outputs come from
			// build.ninja verbatim — they may contain unexpanded $vars (e.g.
			// "$builddir\foo.obj"), so we must ExpandVariables before combining
			// with workingDir.
			auto deleteOutput = [&](u32 outputId, u32 scopeId, Vector<char>& scratch)
			{
				CharStringView rel = stringPool.GetString(outputId);
				if (rel.length == 0)
					return;

				scratch.clear();
				parser.ExpandVariables(rel.data, rel.length, scratch, nullptr, scopeId);
				if (scratch.empty())
					return;

				StringBuffer<> combined;
				combined.Append(workingDir).EnsureEndsWithSlash();
				for (u32 j = 0, jn = u32(scratch.size()); j < jn; ++j)
				{
					char c = scratch[j];
					combined.Append((tchar)(u8)(c == '/' ? PathSeparator : c));
				}

				tchar fullPath[4096];
				if (!GetFullPathNameW(combined.data, sizeof_array(fullPath), fullPath, nullptr))
					return;

				u32 attr = GetFileAttributesW(fullPath);
				if (attr == INVALID_FILE_ATTRIBUTES || IsDirectory(attr))
					return;

				DeleteFileW(fullPath);
			};

			Atomic<u32> deletedEdges = 0;

			for (u32 i = 0; i < u32(edges.size()); ++i)
			{
				if (!edgesToBuildFlags[i] || edges[i].ruleName == phonyId)
					continue;
				workManager.AddWork([&, i](const WorkContext&)
					{
						Vector<char> scratch;
						scratch.reserve(512);
						for (u32 id : edges[i].outputs)
							deleteOutput(id, edges[i].scopeId, scratch);
						for (u32 id : edges[i].implicitOutputs)
							deleteOutput(id, edges[i].scopeId, scratch);
						++deletedEdges;
					}, 1, TC("NinjaFlush"));
			}
			workManager.DoWork(1000000);
			workManager.FlushWork();

			logger.Detail(TC("Cleaned outputs for %u edges in %s"), deletedEdges.load(), TimeToText(GetTime() - cleanStart).str);

			tClean = GetTime() - tPhase;
			tPhase = GetTime();

			if (cleanOnly)
				return true;
		}

		// ============================================================
		// Priority weights (computed before Pass 1 so we can sort)
		// ============================================================
		// Each non-phony edge gets a weight = 1 + sum of weights of all
		// direct consumers. This means weight[e] approximates the total
		// number of edges that transitively depend on e. Pass 1 enqueues
		// edges in descending weight order so the scheduler always
		// unblocks the most downstream work first.
		//
		// Algorithm: propagate in reverse topological order (consumers
		// before producers). We cannot rely on ninja file order since GN
		// does not guarantee topological ordering of rules in the file.
		// Instead we do an explicit topological sort via DFS.
		//
		//   weight[e]        = 1 + remainingWork[e]
		//   remainingWork[p] += weight[e]   (sum over all consumers of p)
		// Phony edges are transparent: weight propagation follows through them.
		// ============================================================
		u32 edgeCount = u32(edges.size());

		// topoOrder is populated by the Kahn's traversal below in consumer-first order
		// (final outputs first, leaf source-only edges last). The out-of-date check uses
		// this reversed (producer-first) to propagate dirty status top-down.
		Vector<u32> topoOrder;
		topoOrder.reserve(edgeCount);

		Vector<float> edgeWeights(edgeCount, 1.0f);
		{
			// Full-graph Kahn's topological sort including phony nodes as transparent weight
			// passers. Eliminates all per-consumer phony DFS overhead. Phony nodes accumulate
			// and forward weight to their producers without adding +1 themselves.
			// Weights may overcount when multiple paths exist from consumer to producer,
			// but ordering quality is still good since overcounted edges rank even higher.

			Vector<u32> inDegree(edgeCount, 0);
			for (u32 i = 0; i < edgeCount; ++i)
			{
				if (!edgesToBuildFlags[i]) continue;
				auto addDep = [&](u32 inputId)
				{
					if (inputId >= outputToEdge.size()) return;
					u32 p = outputToEdge[inputId];
					if (p == ~0u || p >= edgeCount || !edgesToBuildFlags[p]) return;
					inDegree[p]++;
				};
				for (u32 id : edges[i].inputs)        addDep(id);
				for (u32 id : edges[i].implicitDeps)  addDep(id);
				for (u32 id : edges[i].orderOnlyDeps) addDep(id);
			}

			Vector<u32> queue;
			queue.reserve(edgeCount);
			for (u32 i = 0; i < edgeCount; ++i)
				if (edgesToBuildFlags[i] && inDegree[i] == 0)
					queue.push_back(i);

			Vector<float> accumulated(edgeCount, 0.0f);
			while (!queue.empty())
			{
				u32 cur = queue.back(); queue.pop_back();
				topoOrder.push_back(cur);
				bool isPhony = edges[cur].ruleName == phonyId;
				float w = isPhony ? accumulated[cur] : (1.0f + accumulated[cur]);
				if (!isPhony) edgeWeights[cur] = w;

				auto propagate = [&](u32 inputId)
				{
					if (inputId >= outputToEdge.size()) return;
					u32 p = outputToEdge[inputId];
					if (p == ~0u || p >= edgeCount || !edgesToBuildFlags[p]) return;
					accumulated[p] += w;
					if (--inDegree[p] == 0)
						queue.push_back(p);
				};
				for (u32 id : edges[cur].inputs)        propagate(id);
				for (u32 id : edges[cur].implicitDeps)  propagate(id);
				for (u32 id : edges[cur].orderOnlyDeps) propagate(id);
			}
		}

		tTopo = GetTime() - tPhase;
		tPhase = GetTime();

		// ============================================================
		// Out-of-date check — see SuppressUpToDateEdges above for details.
		// Skipped under -rebuild / -clean / -dry-run.
		// ============================================================
		if (!rebuild && !dryRun && !cleanOnly && !noUpToDate)
		{
			SuppressUpToDateEdges(logger, parser, workingDir, workManager, topoOrder, state, noShowIncludes);
		}
		else if (noUpToDate)
		{
			logger.Info(TC("Up-to-date check skipped (-no-up-to-date)"));
		}

		tUpToDate = GetTime() - tPhase;
		tPhase = GetTime();

		#if 0 // Old disabled block preserved below until the new path has baked in.
		u32 skippedAsUpToDate = 0;
		if (!rebuild && !dryRun && !cleanOnly)
		{
			u64 dirtyStart = GetTime();

			DirectoryTableHolder dirTable(logger.m_writer);

			// Per-edge clean flag (1 = tentatively up-to-date, 0 = must rebuild)
			Vector<u8> clean(edgeCount, 0);

			// Shared path buffers for the sequential dirty-check loop.
			StringBuffer<512> relBuf, absBuf;
			StringBuffer<512> dirBuf;

			// Returns the last-write time of the file identified by stringId,
			// or 0 if the file does not exist or has no valid mtime.
			// The containing directory is populated into dirTable on first access
			// for each unique directory; subsequent lookups are O(1).
			auto getMtime = [&](u32 stringId) -> u64
			{
				CharStringView rel = stringPool.GetString(stringId);
				relBuf.Clear().Append(rel.data, rel.length);
				FixPath(absBuf.Clear(), StringView(relBuf.data, relBuf.count), workingDir);

				const tchar* lastSlash;
				StringKey dirKey;
				if (!GetDirKey(dirKey, dirBuf.Clear(), lastSlash, StringView(absBuf.data, absBuf.count)))
					return 0;

				dirTable.WriteDirectoryEntries(dirKey, StringView(dirBuf.data, dirBuf.count));

				DirTableOffset offset;
				if (dirTable.m_directoryTable.EntryExists(
						StringView(absBuf.data, absBuf.count), false, &offset) != DirectoryTable::Exists_Yes)
					return 0;

				DirectoryTable::EntryInformation info;
				dirTable.m_directoryTable.GetEntryInformation(info, offset);
				return info.lastWrite;
			};

			// Pass A: independent per-edge mtime check.
			for (u32 edgeIdx = 0; edgeIdx < edgeCount; ++edgeIdx)
			{
				if (!edgesToBuildFlags[edgeIdx]) continue;
				const auto& edge = edges[edgeIdx];
				if (edge.ruleName == phonyId) { clean[edgeIdx] = 1; continue; }

				// If no outputs are declared (unusual for non-phony), always rebuild.
				if (edge.outputs.empty() && edge.implicitOutputs.empty()) continue;

				// Find the minimum mtime across all outputs. Set to 0 if any is absent.
				// minOutMtime stays ~u64(0) only until the first output is examined.
				u64 minOutMtime = ~u64(0);
				for (u32 id : edge.outputs)
				{
					u64 t = getMtime(id);
					if (!t) { minOutMtime = 0; break; }
					if (t < minOutMtime) minOutMtime = t;
				}
				if (minOutMtime) // skip if an explicit output was already missing (minOutMtime==0)
					for (u32 id : edge.implicitOutputs)
					{
						u64 t = getMtime(id);
						if (!t) { minOutMtime = 0; break; }
						if (t < minOutMtime) minOutMtime = t;
					}

				// minOutMtime == 0  → at least one output is missing → dirty.
				// minOutMtime == ~u64(0) cannot happen here because we checked hasOutputs above
				// and the loops above would have updated it to a real timestamp.
				if (!minOutMtime) continue; // Missing output → dirty

				// Dirty if any declared input is newer than the oldest output.
				bool inputNewer = false;
				for (u32 id : edge.inputs)
				{
					u64 t = getMtime(id);
					if (t > minOutMtime) { inputNewer = true; break; }
				}
				if (!inputNewer)
					for (u32 id : edge.implicitDeps)
					{
						u64 t = getMtime(id);
						if (t > minOutMtime) { inputNewer = true; break; }
					}

				if (!inputNewer) clean[edgeIdx] = 1;
			}

			// Pass B: propagate dirty status from producers to consumers.
			// topoOrder is consumer-first (Kahn's traversal). Iterating in reverse
			// gives producer-first order, guaranteeing each edge's producers are
			// evaluated before the edge itself.
			for (auto it = topoOrder.rbegin(); it != topoOrder.rend(); ++it)
			{
				u32 edgeIdx = *it;
				if (!edgesToBuildFlags[edgeIdx] || !clean[edgeIdx]) continue;

				const auto& edge = edges[edgeIdx];
				auto hasDirtyProducer = [&](u32 inputId) -> bool
				{
					if (inputId >= outputToEdge.size()) return false;
					u32 p = outputToEdge[inputId];
					if (p == ~0u || p >= edgeCount || !edgesToBuildFlags[p]) return false;
					return !clean[p];
				};

				for (u32 id : edge.inputs)
					if (hasDirtyProducer(id)) { clean[edgeIdx] = 0; break; }
				if (clean[edgeIdx])
					for (u32 id : edge.implicitDeps)
						if (hasDirtyProducer(id)) { clean[edgeIdx] = 0; break; }
			}

			// Suppress build for edges confirmed up-to-date.
			for (u32 i = 0; i < edgeCount; ++i)
			{
				if (edgesToBuildFlags[i] && clean[i])
				{
					edgesToBuildFlags[i] = 0;
					++skippedAsUpToDate;
				}
			}

			u32 dirtyCount = 0;
			for (u32 i = 0; i < edgeCount; ++i)
				if (edgesToBuildFlags[i] && edges[i].ruleName != phonyId)
					++dirtyCount;

			logger.Detail(TC("Up-to-date check: %u dirty, %u skipped (%s)"),
				dirtyCount, skippedAsUpToDate, TimeToText(GetTime() - dirtyStart).str);
		}
		#endif

		// YAML export support
		struct YamlEntry
		{
			// No separate `executable` field: the full command lives in
			// `arguments`. ProcessStartInfoHolder::Expand() decides at spawn
			// time whether to tokenize or shell-wrap.
			TString arguments;
			TString workingDir;
			TString description;
			float   weight             = 1.0f;
			bool    canExecuteRemotely = true;
			bool    canDetour          = true;
			Vector<u32> dependencies; // processIdx values
		};
		Vector<YamlEntry> yamlEntries;
		u32 baseProcessIdx = ~0u;

		// Pass 1 - reserve a scheduler slot for every needed non-phony edge.
		// Enqueue in descending critical-path-weight order so the scheduler
		// (which scans from the lowest process index) picks the most critical
		// work first. All edgeToProcessIndex entries are set before Pass 2.
		{
			// Build sorted index list: highest weight first.
			Vector<u32> sortedEdges;
			sortedEdges.reserve(u32(edges.size()));
			for (u32 i = 0; i < u32(edges.size()); ++i)
				if (edgesToBuildFlags[i] && edges[i].ruleName != phonyId)
					sortedEdges.push_back(i);
			std::sort(sortedEdges.begin(), sortedEdges.end(), [&](u32 a, u32 b) {
				return edgeWeights[a] > edgeWeights[b];
			});
			if (yamlFile && !sortedEdges.empty())
				yamlEntries.resize(sortedEdges.size());
			u32 minProcessIdx = ~0u;
			u32 maxProcessIdx = 0;
			for (u32 i : sortedEdges)
			{
				u32 processIdx = scheduler.EnqueueSuspendedProcess();
				if (yamlFile && baseProcessIdx == ~0u)
					baseProcessIdx = processIdx;
				edgeToProcessIndex[i] = processIdx;
				if (processIdx < minProcessIdx) minProcessIdx = processIdx;
				if (processIdx > maxProcessIdx) maxProcessIdx = processIdx;
			}
			// Pre-size the primary-output lookup so Pass 2 workers can write in parallel.
			if (!sortedEdges.empty())
				state.primaryOutByProcessIdx.resize(maxProcessIdx + 1);
			logger.Detail(TC("Reserved %u process slots"), u32(sortedEdges.size()));
		}

		tPass1 = GetTime() - tPhase;
		tPhase = GetTime();

		// Total number of non-phony edges we'll enqueue (captured for `-v` output's
		// ninja-compatible "[N/total]" prefix).
		u32 verboseTotal = 0;
		for (u32 i = 0; i < u32(edges.size()); ++i)
			if (edgesToBuildFlags[i] && edges[i].ruleName != phonyId)
				++verboseTotal;

		// Verbose-print lock so each `[N/total] cmd` line is emitted atomically.
		Futex verboseLock;
		std::atomic<u32> verboseCounter{0};

		// Intern the special variable names used in rsp expansion.
		// Must not be static: a second parser in the same process would reuse these IDs
		// against a different StringPool. Intern() on existing entries is O(1) anyway.
		u32 phonyIdLocal      = phonyId;
		u32 inIdLocal         = const_cast<StringPool&>(stringPool).Intern("in", 2);
		u32 in_newlineIdLocal = const_cast<StringPool&>(stringPool).Intern("in_newline", 10);
		u32 outIdLocal        = const_cast<StringPool&>(stringPool).Intern("out", 3);

		// Pass 2 - dispatch a work item for each needed non-phony edge.
		// ALL per-edge work (ExpandCommand, strip, rsp expansion, description,
		// dep walk, ResumeQueuedProcess) runs inside the work item on WorkManager threads.
		// Thread safety: StringPool::Intern is Futex-protected; ExpandVariables is read-only.
		std::atomic<u32> processedCount{0};
		std::atomic<u32> skippedCount{0};

		DirectoryCache dirCache;

		// Bundle all the per-edge-work-item context into one struct. The Pass 2
		// lambda captures just [edgeIdx, processIdx, &pass2] and touches the rest
		// through `pass2.<field>`. References are used (not pointers) so member
		// access reads naturally inside the lambda body.
		struct Pass2Context
		{
			// Scalars / CLI flags
			StringView   workingDir;
			bool         noShowIncludes;
			bool         dryRun;
			bool         verbose;
			u32          verboseTotal;
			u32          phonyId;
			u32          inId;
			u32          in_newlineId;
			u32          outId;
			const tchar* yamlFile;
			u32          baseProcessIdx;
			u32          cacheBucketId;
			bool         forceRemote;

			// Build graph (read-only post-parse)
			const Vector<NinjaBuildEdge>& edges;
			const Vector<u32>&            outputToEdge;
			const Vector<u8>&             edgesToBuildFlags;
			const Vector<u32>&            edgeToProcessIndex;

			// Core services
			NinjaParser&      parser;
			const StringPool& stringPool;
			Scheduler&        scheduler;
			LoggerWithWriter& logger;
			DirectoryCache&   dirCache;
			NinjaParseState&  state;

			// Counters / shared side output
			std::atomic<u32>& processedCount;
			std::atomic<u32>& skippedCount;

			// -v output
			Futex&            verboseLock;
			std::atomic<u32>& verboseCounter;

			// YAML output
			Vector<YamlEntry>& yamlEntries;
		};

		Pass2Context pass2 {
			workingDir,
			noShowIncludes, dryRun, verbose, verboseTotal,
			phonyId, inIdLocal, in_newlineIdLocal, outIdLocal,
			yamlFile, baseProcessIdx, cacheBucketId, forceRemote,
			edges, outputToEdge, edgesToBuildFlags, edgeToProcessIndex,
			parser, stringPool, scheduler, logger, dirCache, state,
			processedCount, skippedCount,
			verboseLock, verboseCounter,
			yamlEntries,
		};

		for (u32 edgeIdx = 0; edgeIdx < u32(edges.size()); ++edgeIdx)
		{
			if (!edgesToBuildFlags[edgeIdx])
				continue;

			if (edges[edgeIdx].ruleName == phonyIdLocal)
			{
				skippedCount++;
				continue;
			}

			u32 processIdx = edgeToProcessIndex[edgeIdx];

			workManager.AddWork([edgeIdx, processIdx, &pass2,
				visitedEdgesFlags = Vector<u8>(), // If we put them in the lambda they will be reused for every loop on same worker
				addedProcessesFlags = Vector<u8>(),
				processDependencies = Vector<u32>()
				](const WorkContext&) mutable
			{
				// Local aliases so the body reads the same as before the refactor.
				auto&       edges             = pass2.edges;
				auto&       outputToEdge      = pass2.outputToEdge;
				auto&       edgesToBuildFlags = pass2.edgesToBuildFlags;
				auto&       edgeToProcessIndex= pass2.edgeToProcessIndex;
				auto&       parser            = pass2.parser;
				auto&       stringPool        = pass2.stringPool;
				auto&       scheduler         = pass2.scheduler;
				auto&       logger            = pass2.logger;
				auto&       dirCache          = pass2.dirCache;
				auto&       state             = pass2.state;
				auto&       processedCount    = pass2.processedCount;
				auto&       skippedCount      = pass2.skippedCount;
				auto&       verboseLock       = pass2.verboseLock;
				auto&       verboseCounter    = pass2.verboseCounter;
				auto&       yamlEntries       = pass2.yamlEntries;
				const auto  workingDir        = pass2.workingDir;
				const bool  noShowIncludes    = pass2.noShowIncludes;
				const bool  dryRun            = pass2.dryRun;
				const bool  verbose           = pass2.verbose;
				const u32   verboseTotal      = pass2.verboseTotal;
				const u32   phonyId           = pass2.phonyId;
				const u32   inId              = pass2.inId;
				const u32   in_newlineId      = pass2.in_newlineId;
				const u32   outId             = pass2.outId;
				const tchar* yamlFile         = pass2.yamlFile;
				const u32   baseProcessIdx    = pass2.baseProcessIdx;
				const u32   cacheBucketId     = pass2.cacheBucketId;
				[[maybe_unused]] const bool forceRemote = pass2.forceRemote;

				const auto& edge = edges[edgeIdx];

				// Expand command (StringPool::Intern is Futex-protected -> thread-safe)
				Vector<char> fullCommandBuffer, tempBuffer;
				parser.ExpandCommand(edge, fullCommandBuffer, tempBuffer);

				if (fullCommandBuffer.empty())
				{
					skippedCount++;
					return;
				}

				// Strip /showIncludes if requested
				if (noShowIncludes)
				{
					const char* pattern = "/showIncludes";
					u32 patternLen = 13;
					u32 writePos = 0;
					for (u32 readPos = 0; readPos < fullCommandBuffer.size(); )
					{
						if (readPos + patternLen <= fullCommandBuffer.size() &&
							memcmp(&fullCommandBuffer[readPos], pattern, patternLen) == 0)
						{
							while (readPos < fullCommandBuffer.size() && fullCommandBuffer[readPos] != ' ')
								++readPos;
							if (readPos < fullCommandBuffer.size() && fullCommandBuffer[readPos] == ' ')
								++readPos;
						}
						else
						{
							fullCommandBuffer[writePos++] = fullCommandBuffer[readPos++];
						}
					}
					fullCommandBuffer.resize(writePos);
				}

				// Widen to tchar
				TString fullCommand;
				fullCommand.resize(fullCommandBuffer.size());
				for (u32 ci = 0, cn = u32(fullCommandBuffer.size()); ci < cn; ++ci)
					fullCommand[ci] = (tchar)(u8)fullCommandBuffer[ci];

				// -v: emit the fully-expanded command in ninja -v format.
				// Serialize across worker threads so each line is atomic; use
				// monotonically-assigned N so output order matches the dispatch
				// order (which is what ninja -v shows too).
				if (verbose)
				{
					u32 n = verboseCounter.fetch_add(1) + 1;
					SCOPED_FUTEX(verboseLock, vl);
					logger.Info(TC("[%u/%u] %s"), n, verboseTotal, fullCommand.c_str());
				}

				if (fullCommand.empty())
				{
					skippedCount++;
					return;
				}

				// We no longer split exe/args here. The full command line is
				// handed to the scheduler as `arguments` with an empty
				// `application`; ProcessStartInfoHolder::Expand() centralizes
				// shell-wrap detection (env-var prefix, shell keywords, $( ),
				// etc.) and bare-name resolution. Response-file expansion
				// below still needs the edge.rule's rspfile template, not the
				// command split, so it's unaffected.
				TString arguments = fullCommand;

				// Expand response file
				TString      rspFullPath;
				Vector<char> rspContent;
				if (!dryRun)
				{
					auto ruleIt = parser.GetRuleMap().find(edge.ruleName);
					if (ruleIt != parser.GetRuleMap().end())
					{
						const auto& rule = parser.GetRules()[ruleIt->second];
						if (rule.rspfile != StringId_Empty && rule.rspfile_content != StringId_Empty)
						{
							UnorderedMap<u32, u32> rspVars = edge.variables;

							Vector<char> inputsStr, inputsNewlineStr, outputsStr, rspfilePathBuffer;
							for (u32 i = 0; i < edge.inputs.size(); ++i)
							{
								CharStringView input = stringPool.GetString(edge.inputs[i]);
								if (i > 0) { inputsStr.push_back(' '); inputsNewlineStr.push_back('\n'); }
								inputsStr.insert(inputsStr.end(), input.data, input.data + input.length);
								inputsNewlineStr.insert(inputsNewlineStr.end(), input.data, input.data + input.length);
							}
							if (!inputsStr.empty())
								rspVars[inId] = const_cast<StringPool&>(stringPool).InternThreadSafe(inputsStr.data(), u32(inputsStr.size()));
							if (!inputsNewlineStr.empty())
								rspVars[in_newlineId] = const_cast<StringPool&>(stringPool).InternThreadSafe(inputsNewlineStr.data(), u32(inputsNewlineStr.size()));

							for (u32 i = 0; i < edge.outputs.size(); ++i)
							{
								if (i > 0) outputsStr.push_back(' ');
								CharStringView output = stringPool.GetString(edge.outputs[i]);
								outputsStr.insert(outputsStr.end(), output.data, output.data + output.length);
							}
							if (!outputsStr.empty())
								rspVars[outId] = const_cast<StringPool&>(stringPool).InternThreadSafe(outputsStr.data(), u32(outputsStr.size()));

							CharStringView rspfileStr = stringPool.GetString(rule.rspfile);
							parser.ExpandVariables(rspfileStr.data, rspfileStr.length, rspfilePathBuffer, &rspVars, edge.scopeId);

							CharStringView rspfileContentStr = stringPool.GetString(rule.rspfile_content);
							parser.ExpandVariables(rspfileContentStr.data, rspfileContentStr.length, rspContent, &rspVars, edge.scopeId);

							StringBuffer<> fullRspPath;
							fullRspPath.Append(workingDir);
							fullRspPath.EnsureEndsWithSlash();
							u32 charIdx = 0;
							if (rspfilePathBuffer.size() >= 2 && rspfilePathBuffer[0] == '.' &&
								(rspfilePathBuffer[1] == '/' || rspfilePathBuffer[1] == '\\'))
								charIdx = 2;
							for (; charIdx < rspfilePathBuffer.size(); ++charIdx)
								fullRspPath.Append((tchar)(u8)rspfilePathBuffer[charIdx]);

							rspFullPath = fullRspPath.data;
						}
					}
				}

				// Build description: friendly rule type + concise output path
				CharStringView ruleNameStr = stringPool.GetString(edge.ruleName);

				// Find prefix before '__' (Chromium: CXX_COMPILER__target_name)
				u32 prefixLen = 0;
				while (prefixLen + 1 < ruleNameStr.length &&
				       !(ruleNameStr.data[prefixLen] == '_' && ruleNameStr.data[prefixLen + 1] == '_'))
					++prefixLen;
				if (prefixLen == ruleNameStr.length)
					prefixLen = ruleNameStr.length < 20 ? ruleNameStr.length : 20;

				// Map common rule prefixes to short human-readable labels
				const tchar* label = nullptr;
				auto prefixIs = [&](const char* s, u32 n) {
					return prefixLen == n && memcmp(ruleNameStr.data, s, n) == 0;
				};
				if      (prefixIs("CXX_COMPILER",  12)) label = TC("C++");
				else if (prefixIs("C_COMPILER",    10)) label = TC("C");
				else if (prefixIs("LINK",           4)) label = TC("Link");
				else if (prefixIs("SOLINK_MODULE", 13)) label = TC("Link.so");
				else if (prefixIs("SOLINK",         6)) label = TC("Link.so");
				else if (prefixIs("ALINK",          5)) label = TC("Lib");
				else if (prefixIs("ACTION",         6)) label = TC("Action");
				else if (prefixIs("STAMP",          5)) label = TC("Stamp");
				else if (prefixIs("COPY",           4)) label = TC("Copy");

				TString description;
				if (label)
					description = label;
				else
					for (u32 i = 0; i < prefixLen; ++i)
						description += (tchar)(u8)ruleNameStr.data[i];

				if (!edge.outputs.empty())
				{
					CharStringView out = stringPool.GetString(edge.outputs[0]);

					// Find last two path separators
					const char* lastSep = nullptr;
					const char* prevSep = nullptr;
					for (u32 i = 0; i < out.length; ++i)
						if (out.data[i] == '/' || out.data[i] == '\\')
						{ prevSep = lastSep; lastSep = out.data + i; }

					// Find extension within filename (stop at separator)
					const char* ext = nullptr;
					for (u32 i = out.length; i > 0; --i)
					{
						if (out.data[i - 1] == '.' ) { ext = out.data + i - 1; break; }
						if (out.data[i - 1] == '/' || out.data[i - 1] == '\\') break;
					}

					// Show parent/file.o for object files (many share the same filename)
					bool isObj = ext && (strcmp(ext, ".o") == 0 || strcmp(ext, ".obj") == 0);
					const char* nameStart = (isObj && prevSep) ? prevSep + 1
					                      : lastSep            ? lastSep + 1
					                      :                      out.data;
					u32 nameLen = u32(out.data + out.length - nameStart);

					if (!description.empty())
						description += TC(" ");
					for (u32 i = 0; i < nameLen; ++i)
						description += (tchar)(u8)nameStart[i];
				}

				if (description.size() > 60) { description.resize(57); description += TC("..."); }

				// Rsp file I/O - dirCache is Futex-protected (thread-safe)
				if (!rspFullPath.empty())
				{
					const tchar* lastSep = nullptr;
					for (const tchar* p = rspFullPath.c_str(); *p; ++p)
						if (*p == '/' || *p == '\\') lastSep = p;
					if (lastSep && lastSep > rspFullPath.c_str())
					{
						TString dirPath(rspFullPath.c_str(), u32(lastSep - rspFullPath.c_str()));
						//logger.Info(TC("CREATEDIR: %s"), dirPath.c_str());
						dirCache.CreateDirectory(logger, dirPath.c_str());
					}
					FileHandle rspHandle = CreateFileW(rspFullPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, DefaultAttributes());
					if (rspHandle == InvalidFileHandle)
						logger.Error(TC("Failed to create rsp file %s (%s)"), rspFullPath.c_str(), LastErrorToText().data);
					else
					{
						WriteFile(logger, rspFullPath.c_str(), rspHandle, rspContent.data(), u32(rspContent.size()));
						CloseFile(rspFullPath.c_str(), rspHandle);
					}
				}

				// Create output directories (like ninja does before every edge)
				{
					StringBuffer<512> tmp1, tmp2;
					Vector<char> expandBuf;
					auto createOutputDir = [&](u32 id)
					{
						CharStringView rel = stringPool.GetString(id);
						// Output paths may contain variables (e.g. $builddir); expand before use
						expandBuf.clear();
						parser.ExpandVariables(rel.data, rel.length, expandBuf, nullptr, edge.scopeId);
						tmp1.Clear().Append(expandBuf.data(), u32(expandBuf.size()));
						FixPath(tmp2.Clear(), tmp1, workingDir);
						while (tmp2.count > 0 && tmp2.data[tmp2.count - 1] != PathSeparator)
							--tmp2.count;
						if (tmp2.count > 0)
						{
							tmp2.data[tmp2.count] = 0;
							dirCache.CreateDirectory(logger, tmp2.data);
						}
					};
					for (u32 id : edge.outputs)        createOutputDir(id);
					for (u32 id : edge.implicitOutputs) createOutputDir(id);
				}

				// Dep walk - fresh scratch vectors per work item
				visitedEdgesFlags.clear();
				addedProcessesFlags.clear();
				processDependencies.clear();

				auto addDependency = [&](u32 depTargetId, auto& self) -> void
				{
					if (depTargetId >= outputToEdge.size() || outputToEdge[depTargetId] == ~0u)
						return;
					u32 depEdgeIdx = outputToEdge[depTargetId];
					if (!edgesToBuildFlags[depEdgeIdx])
						return;
					if (depEdgeIdx >= visitedEdgesFlags.size())
						visitedEdgesFlags.resize(depEdgeIdx + 1, 0);
					if (visitedEdgesFlags[depEdgeIdx] || depEdgeIdx == edgeIdx)
						return;
					visitedEdgesFlags[depEdgeIdx] = 1;

					const auto& depEdge = edges[depEdgeIdx];
					if (depEdge.ruleName == phonyId)
					{
						for (u32 id : depEdge.inputs) self(id, self);
						for (u32 id : depEdge.implicitDeps) self(id, self);
						for (u32 id : depEdge.orderOnlyDeps) self(id, self);
					}
					else
					{
						u32 depProcessIdx = edgeToProcessIndex[depEdgeIdx];
						if (depProcessIdx != ~0u)
						{
							if (depProcessIdx >= addedProcessesFlags.size())
								addedProcessesFlags.resize(depProcessIdx + 1, 0);
							if (!addedProcessesFlags[depProcessIdx])
							{
								processDependencies.push_back(depProcessIdx);
								addedProcessesFlags[depProcessIdx] = 1;
							}
						}
					}
				};

				for (u32 id : edge.inputs) addDependency(id, addDependency);
				for (u32 id : edge.implicitDeps) addDependency(id, addDependency);
				for (u32 id : edge.orderOnlyDeps) addDependency(id, addDependency);


				struct CacheInfo
				{
					Scheduler* scheduler;
					const StringPool* stringPool;
					StringView workingDir;
					Vector<u32> inputs;
					Vector<u32> outputs;
				};

				// Empty application → Expand() in UbaProcessStartInfoHolder
				// tokenizes arguments (or wraps in `sh -c` when the command
				// uses shell syntax) and Session::PrepareProcess resolves
				// bare names via the cached SearchPathForFile.
				ProcessStartInfo pinfo;
				pinfo.application = TC("");
				pinfo.arguments   = arguments.c_str();
				pinfo.workingDir  = workingDir.data;
				pinfo.description = description.c_str();
				pinfo.trackInputs = true;
				// `userData` is overwritten by the scheduler for its own exitedFunc
				// bookkeeping, so we can't stash UbaNinja state there. `breadcrumbs`
				// is passed through untouched — use it to carry the declared primary
				// output path to the process-finished callback. The pointer is
				// backed by state.primaryOutByProcessIdx which lives in WrappedMain's
				// scope and is pre-sized before Pass 2 so the TString address is stable.

				EnqueueProcessInfo epi(pinfo);
				epi.weight             = 1.0f;
				epi.canDetour          = true;
				epi.canExecuteRemotely = true;
				epi.dependencies       = processDependencies.data();
				epi.dependencyCount    = u32(processDependencies.size());
				epi.cacheBucketId      = cacheBucketId;

				Vector<tchar> knownInputsBuffer;
				u32 knownInputsCount = 0;
				{
					knownInputsBuffer.reserve((edge.inputs.size() + edge.implicitDeps.size()) * (workingDir.count + 10));
					StringBuffer<512> tmp1;
					StringBuffer<512> tmp2;
					auto addInput = [&](u32 id)
						{
							auto view = stringPool.GetString(id);
							tmp1.Clear().Append(view.data, view.length);
							FixPath(tmp2.Clear(), tmp1, workingDir);
							u32 oldSize = u32(knownInputsBuffer.size());
							knownInputsBuffer.resize(oldSize + tmp2.count + 1);
							memcpy(knownInputsBuffer.data() + oldSize, tmp2.data, tmp2.count * sizeof(tchar));
							knownInputsBuffer[oldSize + tmp2.count] = 0;
							++knownInputsCount;
						};
					for (u32 id : edge.inputs) addInput(id);
					for (u32 id : edge.implicitDeps) addInput(id);
					if (knownInputsCount)
					{
						knownInputsBuffer.push_back(0); // final empty null terminator
						epi.knownInputs      = knownInputsBuffer.data();
						epi.knownInputsBytes = u32(knownInputsBuffer.size() * sizeof(tchar));
						epi.knownInputsCount = knownInputsCount;
					}
				}

				Vector<tchar> knownOutputsBuffer;
				u32 knownOutputsCount = 0;
				{
					knownOutputsBuffer.reserve((edge.outputs.size() + edge.implicitOutputs.size()) * (workingDir.count + 10));
					StringBuffer<512> tmp1;
					StringBuffer<512> tmp2;
					auto addOutput = [&](u32 id)
					{
						auto view = stringPool.GetString(id);
						tmp1.Clear().Append(view.data, view.length);
						FixPath(tmp2.Clear(), tmp1, workingDir);
						u32 oldSize = u32(knownOutputsBuffer.size());
						knownOutputsBuffer.resize(oldSize + tmp2.count + 1);
						memcpy(knownOutputsBuffer.data() + oldSize, tmp2.data, tmp2.count * sizeof(tchar));
						knownOutputsBuffer[oldSize + tmp2.count] = 0;
						++knownOutputsCount;
					};
					for (u32 id : edge.outputs)
						addOutput(id);
					for (u32 id : edge.implicitOutputs)
						addOutput(id);
					if (knownOutputsCount)
					{
						knownOutputsBuffer.push_back(0); // final empty null terminator
						epi.knownOutputs      = knownOutputsBuffer.data();
						epi.knownOutputsBytes = u32(knownOutputsBuffer.size() * sizeof(tchar));
						epi.knownOutputsCount = knownOutputsCount;
					}
				}

				//if (!forceRemote)
				{
					if (ShouldForceRunLocally(description))
						epi.canExecuteRemotely = false;

					// Linking is slow since there is no application rule to preparse obj files so they will be transferred sequentially
					epi.canExecuteRemotely = epi.canExecuteRemotely
						&& description.compare(0, 6, TC("solin ")) != 0
						&& description.compare(0, 4, TC("lin ")) != 0
						&& description.compare(0, 5, TC("alin ")) != 0;
				}

				// Build the callback payload for breadcrumbs:
				//   <flag:1><16-hex-cmd-hash><absolute-primary-output-path>
				// where <flag> = 'R' if the rule has restat=1 (or phony_output),
				// 'N' otherwise. The 16-hex command hash is the low 64 bits of
				// ToStringKeyNoCheck on the ninja-level full command; we pre-
				// compute it here and RecordResult uses it verbatim, so the log
				// format doesn't couple to post-Expand argument layout (which
				// may be `sh -c "..."` for shell-wrapped edges).
				if (!edge.outputs.empty() && processIdx < state.primaryOutByProcessIdx.size())
				{
					Vector<char> expandPath;
					CharStringView primaryRel = stringPool.GetString(edge.outputs[0]);
					parser.ExpandVariables(primaryRel.data, primaryRel.length, expandPath, nullptr, edge.scopeId);
					if (!expandPath.empty())
					{
						StringBuffer<512> wide;
						wide.count = u32(expandPath.size());
						if (wide.count + 1 > wide.capacity) wide.count = wide.capacity - 1;
						for (u32 k = 0; k < wide.count; ++k)
						{
							char c = expandPath[k];
							wide.data[k] = (tchar)(u8)((c == '\\' || c == '/') ? PathSeparator : c);
						}
						wide.data[wide.count] = 0;
						StringBuffer<512> abs;
						FixPath(abs, StringView(wide.data, wide.count), workingDir);

						bool restatRule = false;
						{
							auto rmIt = parser.GetRuleMap().find(edge.ruleName);
							if (rmIt != parser.GetRuleMap().end())
								restatRule = parser.GetRules()[rmIt->second].restat;
						}
						const bool useWallClockMtime = restatRule || edge.phonyOutput;

						// Hash the ninja-level full command. Writer and reader
						// both see this identical string so the hashes match.
						StringKey cmdKey = ToStringKeyNoCheck(arguments.c_str(), u32(arguments.length()));

						tchar hashHex[17];
						for (u32 k = 0; k < 16; ++k)
						{
							u32 nibble = u32((cmdKey.a >> ((15 - k) * 4)) & 0xF);
							hashHex[k] = (tchar)(nibble < 10 ? (TC('0') + nibble) : (TC('a') + (nibble - 10)));
						}
						hashHex[16] = 0;

						TString& slot = state.primaryOutByProcessIdx[processIdx];
						slot.assign(useWallClockMtime ? TC("R") : TC("N"));
						slot.append(hashHex, 16);
						slot.append(abs.data, abs.count);
						pinfo.breadcrumbs = slot.c_str();
					}
				}

				scheduler.ResumeQueuedProcess(processIdx, epi);

				if (yamlFile && baseProcessIdx != ~0u)
				{
					u32 slot = processIdx - baseProcessIdx;
					auto& entry = yamlEntries[slot];
					// `executable` stays empty — the full command line lives in
					// `arguments`. ProcessStartInfoHolder::Expand() decides the
					// actual binary at spawn time.
					entry.arguments          = arguments;
					entry.workingDir         = TString(workingDir.data, workingDir.count);
					entry.description        = description;
					entry.weight             = epi.weight;
					entry.canExecuteRemotely = epi.canExecuteRemotely;
					entry.canDetour          = epi.canDetour;
					entry.dependencies       = processDependencies;
				}

				processedCount++;
			}, 1, TC("NinjaResume"));
		}

		tPass2Dispatch = GetTime() - tPhase;
		tPhase = GetTime();

		// Help drain work items on this thread while waiting
		workManager.DoWork(1000000);
		workManager.FlushWork();

		tPass2Wait = GetTime() - tPhase;
		tPhase = GetTime();

		// Write YAML file if requested (all work items are done so yamlEntries is fully populated)
		if (yamlFile && !yamlEntries.empty())
		{
			Vector<char> buf;
			buf.reserve(4 * 1024 * 1024);

			auto appendStr = [&](const char* s) {
				buf.insert(buf.end(), s, s + strlen(s));
			};
			auto appendTStr = [&](const TString& s) {
				for (tchar c : s) buf.push_back((char)(u8)c);
			};
			auto appendU32 = [&](u32 v) {
				char tmp[16];
				#if PLATFORM_WINDOWS
				_itoa_s(v, tmp, sizeof(tmp), 10);
				#else
				snprintf(tmp, 16, "%d", v);
				#endif
				appendStr(tmp);
			};

			appendStr("processes:\n");

			for (u32 slot = 0; slot < u32(yamlEntries.size()); ++slot)
			{
				const auto& e = yamlEntries[slot];
				if (e.arguments.empty())
					continue;
				u32 id = baseProcessIdx + slot;

				appendStr("  - id: "); appendU32(id); appendStr("\n");
				appendStr("    arg: "); appendTStr(e.arguments);  appendStr("\n");
				appendStr("    dir: "); appendTStr(e.workingDir); appendStr("\n");
				appendStr("    desc: "); appendTStr(e.description); appendStr("\n");

				if (e.weight != 1.0f)
				{
					char tmp[32];
					snprintf(tmp, sizeof(tmp), "    weight: %.2f\n", e.weight);
					appendStr(tmp);
				}
				if (!e.canExecuteRemotely)
					appendStr("    remote: false\n");
				if (!e.canDetour)
					appendStr("    detour: false\n");

				if (!e.dependencies.empty())
				{
					appendStr("    dep: [");
					for (u32 di = 0; di < u32(e.dependencies.size()); ++di)
					{
						if (di > 0) appendStr(", ");
						appendU32(e.dependencies[di]);
					}
					appendStr("]\n");
				}
			}

			FileHandle fh = CreateFileW(yamlFile, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, DefaultAttributes());
			if (fh != InvalidFileHandle)
			{
				WriteFile(logger, yamlFile, fh, buf.data(), u32(buf.size()));
				CloseFile(yamlFile, fh);
				logger.Info(TC("Wrote YAML file: %s (%u entries)"), yamlFile, u32(yamlEntries.size()));
			}
			else
				logger.Error(TC("Failed to create YAML file: %s"), yamlFile);
		}

		tYaml = GetTime() - tPhase;

		u64 totalTime = GetTime() - totalStartTime;
		u64 queueTime = GetTime() - queueStartTime;

		u32 totalBuilt = 0;
		for (u32 i = 0; i < u32(edges.size()); ++i)
			if (edgesToBuildFlags[i]) ++totalBuilt;

		logger.Detail(TC("Enqueued %u commands, skipped %u (of %u needed edges)"), processedCount.load(), skippedCount.load(), totalBuilt);
		logger.Detail(TC("  Queue time: %s"), TimeToText(queueTime).str);
		logger.Detail(TC("  Total time: %s"), TimeToText(totalTime).str);

		// Per-phase breakdown. Emits at Info so it's visible without -detail.
		// Skipped phases (clean / up-to-date / yaml) stay at zero; we print
		// them unconditionally so the line is stable and easy to grep.
		logger.Info(TC("Enqueue phases: bfs=%s clean=%s topo=%s uptodate=%s pass1=%s dispatch=%s wait=%s yaml=%s"),
			TimeToText(tBfs).str,
			TimeToText(tClean).str,
			TimeToText(tTopo).str,
			TimeToText(tUpToDate).str,
			TimeToText(tPass1).str,
			TimeToText(tPass2Dispatch).str,
			TimeToText(tPass2Wait).str,
			TimeToText(tYaml).str);
		return true;
	}

}
