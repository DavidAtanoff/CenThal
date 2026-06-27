// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaLogger.h"
#include "UbaNinjaParser.h"

namespace uba
{
	class Scheduler;

	// State produced by ParseNinjaFile and consumed by EnqueueCommands.
	// Carries the edge maps and target set across the two phases.
	struct NinjaParseState
	{
		Vector<u32>     outputToEdge;
		Vector<u8>      edgesToBuildFlags;
		Vector<u32>     edgeToProcessIndex;
		Vector<u8>      wantedOutputs;
		u32             phonyId = 0;
		bool            wantAll = false; // True when no targets specified; set by ParseNinjaFile

		// Per-processIdx "callback payload" used via ProcessStartInfo::breadcrumbs
		// to pass edge context to the process-finished callback. Encoded as:
		//   <flag><absolute-primary-output-path>
		// where <flag> = 'R' for `restat = 1` rules, 'N' otherwise. The callback
		// splits off the flag and hands both pieces to NinjaDepsLog::RecordResult.
		// We can't use ProcessStartInfo::userData for this — the scheduler
		// overwrites it with its own ExitProcessInfo pointer. breadcrumbs is
		// passed through untouched (it's a trace/diagnostic field).
		Vector<TString> primaryOutByProcessIdx;
	};

	// Phase 1: intern target IDs, register the edge-parsed callback, and parse the ninja file.
	// Can be run on a background thread while the caller does other initialization work.
	// `buildRoot` is the directory that relative paths inside the ninja file are
	// resolved against (ninja's cwd-at-invocation, possibly adjusted by `-C`).
	// Nullptr falls back to the ninja file's own directory, which is only correct
	// when they coincide (e.g. `-C out/Default` for chromium/llvm).
	bool ParseNinjaFile(LoggerWithWriter& logger, NinjaParser& parser,
	                    const tchar* ninjaFile, const Vector<TString>& specifiedTargets,
	                    NinjaParseState& outState, const tchar* buildRoot = nullptr);

	// Phase 2: BFS target selection, output cleaning, weight computation, and scheduler enqueue.
	// Must be called after ParseNinjaFile has completed.
	// If `noUpToDate` is true the mtime-based up-to-date check is skipped and
	// every needed edge is enqueued — useful when iterating on UbaNinja itself
	// and you want to force the full pipeline without wiping outputs (as `-rebuild` would).
	// If `verbose` is true, the fully-expanded command for each enqueued edge
	// is printed to stdout in `[N/total] <command>` format (matching ninja -v),
	// suitable for diffing UbaNinja's commands against real ninja's.
	bool EnqueueCommands(LoggerWithWriter& logger, NinjaParser& parser, Scheduler& scheduler,
	                     NinjaParseState& state, StringView workingDir,
	                     bool rebuild, bool cleanOnly, bool noShowIncludes, bool noUpToDate,
	                     bool verbose, bool dryRun, const tchar* yamlFile,
	                     u32 cacheBucketId, bool forceRemote);
}
