// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaHash.h"
#include "UbaSynchronization.h"
#include "UbaStringBuffer.h"
#include "UbaUnorderedMap.h"
#include "UbaVector.h"

namespace uba
{
	class Logger;
	class ProcessHandle;

	// Parsed view of a previous run's .ninja_log (v5) and .ninja_deps (v4) files.
	//
	// All lookups are keyed by a StringKey = hash of the *absolute* path (lowercased
	// on case-insensitive filesystems). That matches the keying used for the mtime
	// cache, so the up-to-date check is a single hash-table find per query.
	//
	// Both ninja log files use char-based paths and so does this reader; we widen
	// to tchar only when calling FixPath / ToStringKey.
	//
	// Load once on a single thread during startup; read freely after.
	struct NinjaLogIndex
	{
		struct LogEntry
		{
			u64 outMtime = 0; // mtime recorded in .ninja_log at end of last successful build
			u64 cmdHash  = 0; // hash of the command used last time (low 64 bits of StringKey)
		};
		struct DepsEntry
		{
			u64 outMtime  = 0; // mtime of the output when these deps were captured
			u32 depOffset = 0; // index into flatDepKeys
			u32 depCount  = 0;
		};

		UnorderedMap<StringKey, LogEntry>  logByOutputKey;
		UnorderedMap<StringKey, DepsEntry> depsByOutputKey;
		// referenced by DepsEntry.depOffset/depCount
		Vector<StringKey>                  flatDepKeys;
		// tchar buffer holding absolute paths for each entry in flatDepKeys.
		// flatDepPathOffsets[i]..[i]+len covers bytes in flatDepPathBuf for dep i.
		// We keep the paths because phase 1 of the up-to-date check needs them
		// to register parent directories for the stat pass.
		Vector<tchar>                      flatDepPathBuf;
		Vector<u32>                        flatDepPathOffset;
		Vector<u32>                        flatDepPathLen;

		// Parse .ninja_log and .ninja_deps from ninjaDir. Missing files are treated
		// as "no prior build" and return true with empty maps — not an error.
		// Paths in the log are stored relative to workingDir; we resolve them
		// against workingDir to get absolute paths before hashing.
		bool Load(Logger& logger, StringView ninjaDir, StringView workingDir);
	};

	// Writes .ninja_deps (v4) and .ninja_log (v5) files during a UbaNinja build.
	// All public methods are thread-safe.
	//
	// Usage:
	//   NinjaDepsLog log;
	//   log.Init(workingDir, buildStartTime);
	//   // In process-finished callback (for each successful process):
	//   log.RecordResult(logger, handle);
	//   // After build completes:
	//   log.Save(logger, ninjaDir);
	class NinjaDepsLog
	{
	public:
		NinjaDepsLog();
		~NinjaDepsLog();

		// Call before any RecordResult calls.
		// workingDir must end with a path separator.
		void Init(StringView workingDir, u64 buildStartTime);

		// Record deps and timing for a finished process.
		// Call only for processes that succeeded (exit code == 0).
		//
		// If primaryOutputAbsPath is non-null it is used as the edge's declared
		// primary output (the key in .ninja_log / .ninja_deps) rather than
		// whatever file UBA's tracking happened to see first. Pass it to avoid
		// keying log entries off a sidecar like `.rc.res.d` or `.pdb`.
		//
		// If `restat` is true the edge's rule has `restat = 1` — meaning the
		// command may not rewrite all outputs every time (e.g. `lld-link` leaves
		// the import .lib alone when exports didn't change, proto generators
		// that stabilize on identical output, etc.). For these we record the
		// mtime as "now" (wall-clock time) instead of the output file's own
		// mtime. That matches ninja's semantics: the output is "logically
		// up-to-date as of this build". Subsequent up-to-date checks compare
		// current input mtimes against this recorded "now" rather than the
		// file's possibly-stale or UBA-sync-bumped actual mtime, so a restat
		// edge that ran-with-no-changes doesn't get re-marked dirty every run.
		// `cmdHashOverride`, when non-zero, is stored verbatim as the log entry's
		// command hash rather than re-hashing si.arguments. The caller knows the
		// pre-Expand ninja command; si.arguments by the time RecordResult runs
		// may be the post-Expand form (e.g. `sh -c "..."`), so having callers
		// pass the deterministic upstream hash avoids coupling the log format to
		// Expand()'s transformation rules.
		void RecordResult(Logger& logger, const ProcessHandle& handle,
			const tchar* primaryOutputAbsPath = nullptr,
			bool restat = false,
			u64 cmdHashOverride = 0);

		// Write .ninja_deps and .ninja_log into ninjaDir.
		// ninjaDir must end with a path separator.
		bool Save(Logger& logger, StringView ninjaDir);

	private:
		// Returns the path ID, writing a path record to m_pathBuf if this is a new path.
		// absPath must be a non-empty absolute path (or any unique path string).
		u32 GetOrAddPath(StringView absPath);

		// Convert an absolute path to be relative to m_workingDir.
		// Writes into out and returns true if the path is under m_workingDir; otherwise copies absPath as-is.
		void MakeRelative(StringBufferBase& out, StringView absPath) const;

		TString           m_workingDir;      // includes trailing separator

		// --- path table ---
		// Guards m_pathIds and m_pathBuf.
		// Path records are appended to m_pathBuf while holding the write lock,
		// BEFORE the id is inserted into m_pathIds.  Any thread that later reads
		// an id from m_pathIds is guaranteed to find the record already in m_pathBuf.
		ReaderWriterLock  m_pathLock;
		UnorderedMap<TString, u32> m_pathIds;
		Vector<u8>        m_pathBuf;         // all path records (binary)
		u32               m_nextPathId = 0;

		// --- deps records ---
		ReaderWriterLock  m_depsLock;
		Vector<u8>        m_depsBuf;         // all deps records (binary)

		// --- log records ---
		ReaderWriterLock  m_logLock;
		Vector<u8>        m_logBuf;          // text lines (not including the header)

		u64               m_buildStart = 0;  // GetTime() at build start
	};
}
