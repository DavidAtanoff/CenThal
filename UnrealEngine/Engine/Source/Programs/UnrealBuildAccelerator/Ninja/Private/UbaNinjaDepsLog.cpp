// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaNinjaDepsLog.h"
#include "UbaBinaryReaderWriter.h"
#include "UbaFile.h"
#include "UbaFileAccessor.h"
#include "UbaHash.h"
#include "UbaLogger.h"
#include "UbaMap.h"
#include "UbaPathUtils.h"
#include "UbaProcessHandle.h"
#include "UbaProcessStartInfo.h"
#include "UbaTimer.h"

namespace uba
{
	// High bit of the leading u32 distinguishes path records from deps records.
	static constexpr u32 kPathMask = 0x80000000u;

	NinjaDepsLog::NinjaDepsLog() = default;
	NinjaDepsLog::~NinjaDepsLog() = default;

	void NinjaDepsLog::Init(StringView workingDir, u64 buildStartTime)
	{
		m_workingDir.assign(workingDir.data, workingDir.count);
		if (!m_workingDir.empty() && m_workingDir.back() != PathSeparator)
			m_workingDir += PathSeparator;
		m_buildStart = buildStartTime;

		m_pathBuf.reserve(64  * 1024);
		m_depsBuf.reserve(128 * 1024);
		m_logBuf.reserve(64  * 1024);

		// Load any prior run's state so Save merges old + new rather than clobbering.
		// Without this, edges that were up-to-date in the current build (and thus
		// never called RecordResult) would vanish from the log and look "unknown"
		// on the next run.
		LoggerWithWriter quiet(g_nullLogWriter, TC(""));

		// .ninja_log: text, we just preserve everything after the header so that
		// writer Save() re-emits the same header + old tail + newly-recorded tail.
		{
			StringBuffer<512> logPath;
			logPath.Append(m_workingDir.c_str()).Append(TC(".ninja_log"));
			FileAccessor fa(quiet, logPath.data);
			if (fa.OpenMemoryRead(0, /*errorOnFail=*/false))
			{
				static const char header[] = "# ninja log v5\n";
				constexpr u64 headerLen = sizeof(header) - 1;
				if (fa.GetSize() >= headerLen && memcmp(fa.GetData(), header, headerLen) == 0)
				{
					const u8* p    = (const u8*)fa.GetData() + headerLen;
					const u8* pEnd = (const u8*)fa.GetData() + fa.GetSize();
					// Skip any stray leading null bytes — legacy files (when the header
					// was off-by-one 16 bytes) have a 0x00 byte at the start of the tail.
					while (p < pEnd && *p == 0) ++p;
					u64 tail = u64(pEnd - p);
					if (tail > 0)
					{
						m_logBuf.resize(tail);
						memcpy(m_logBuf.data(), p, tail);
					}
				}
			}
		}

		// .ninja_deps: binary, v4. Parse path records to rebuild m_pathIds +
		// m_nextPathId so newly-added paths get unique ids. Also preserve the
		// raw bytes by copying path records into m_pathBuf and deps records
		// into m_depsBuf. Save() writes m_pathBuf before m_depsBuf.
		{
			StringBuffer<512> depsPath;
			depsPath.Append(m_workingDir.c_str()).Append(TC(".ninja_deps"));
			FileAccessor fa(quiet, depsPath.data);
			if (!fa.OpenMemoryRead(0, /*errorOnFail=*/false))
				return;

			static const char header[] = "# ninjadeps\n";
			constexpr u64 headerLen = sizeof(header) - 1;
			if (fa.GetSize() < headerLen + 4 || memcmp(fa.GetData(), header, headerLen) != 0)
				return;
			u32 version;
			memcpy(&version, (const u8*)fa.GetData() + headerLen, 4);
			if (version != 4)
				return;

			const u8* p   = (const u8*)fa.GetData() + headerLen + 4;
			const u8* end = (const u8*)fa.GetData() + fa.GetSize();

			while (p + 4 <= end)
			{
				u32 sizeWithFlag;
				memcpy(&sizeWithFlag, p, 4);
				bool isPath = (sizeWithFlag & kPathMask) != 0;
				u32 recordSize = sizeWithFlag & ~kPathMask;
				if (p + 4 + recordSize > end)
					break; // truncated: stop here, keep what we have

				if (isPath)
				{
					if (recordSize < 4) break;
					u32 paddedLen = recordSize - 4;
					u32 pathLen = paddedLen;
					while (pathLen > 0 && p[4 + pathLen - 1] == 0)
						--pathLen;

					// Rebuild m_pathIds mapping. Paths are stored UTF-8 on disk;
					// we widen to tchar for the map key so new runs' GetOrAddPath
					// lookups hit these entries.
					#if PLATFORM_WINDOWS
					tchar wide[4096];
					int wLen = MultiByteToWideChar(CP_UTF8, 0, (const char*)p + 4, (int)pathLen,
						wide, (int)(sizeof_array(wide) - 1));
					if (wLen < 0) wLen = 0;
					TString key(wide, u32(wLen));
					#else
					TString key((const tchar*)(p + 4), pathLen);
					#endif
					u32 checksum;
					memcpy(&checksum, p + 4 + paddedLen, 4);
					u32 pathId = ~checksum;
					m_pathIds.emplace(std::move(key), pathId);
					if (pathId + 1 > m_nextPathId)
						m_nextPathId = pathId + 1;

					// Copy the full record (leading size+flag u32 included) to m_pathBuf.
					u64 before = m_pathBuf.size();
					m_pathBuf.resize(before + 4 + recordSize);
					memcpy(m_pathBuf.data() + before, p, 4 + recordSize);
				}
				else
				{
					u64 before = m_depsBuf.size();
					m_depsBuf.resize(before + 4 + recordSize);
					memcpy(m_depsBuf.data() + before, p, 4 + recordSize);
				}
				p += 4 + recordSize;
			}
		}
	}

	// -----------------------------------------------------------------------
	// Internal helpers
	// -----------------------------------------------------------------------

	void NinjaDepsLog::MakeRelative(StringBufferBase& out, StringView absPath) const
	{
		u32 wdLen = u32(m_workingDir.size());
		if (wdLen > 0 &&
			absPath.count > wdLen &&
			memcmp(absPath.data, m_workingDir.c_str(), wdLen * sizeof(tchar)) == 0)
		{
			out.Append(absPath.data + wdLen, absPath.count - wdLen);
		}
		else
		{
			out.Append(absPath.data, absPath.count);
		}
	}

	u32 NinjaDepsLog::GetOrAddPath(StringView absPath)
	{
		// Fast path: read lock.
		{
			SCOPED_READ_LOCK(m_pathLock, rl);
			auto it = m_pathIds.find(TString(absPath.data, absPath.count));
			if (it != m_pathIds.end())
				return it->second;
		}

		// Slow path: write lock.
		SCOPED_WRITE_LOCK(m_pathLock, wl);

		// Double-check after acquiring write lock.
		TString key(absPath.data, absPath.count);
		auto it = m_pathIds.find(key);
		if (it != m_pathIds.end())
			return it->second;

		u32 id = m_nextPathId++;

		// Build path record into m_pathBuf while still holding the write lock,
		// BEFORE inserting into m_pathIds.  Any thread that later reads the id
		// from m_pathIds is therefore guaranteed to see the path record in m_pathBuf.
		//
		// Path record layout (ninja deps v4):
		//   u32  (padded_len + 4) | kPathMask
		//   u8   path[padded_len]    — path bytes, null-padded to 4-byte boundary
		//   u32  ~id                  — checksum

		StringBuffer<512> relBuf;
		MakeRelative(relBuf, absPath);

		// Ninja deps/log files use narrow (UTF-8) paths on all platforms.
#if PLATFORM_WINDOWS
		char narrow[4096];
		int narrowLen = WideCharToMultiByte(CP_UTF8, 0, relBuf.data, (int)relBuf.count,
		                                    narrow, (int)(sizeof(narrow) - 1), nullptr, nullptr);
		if (narrowLen < 0) narrowLen = 0;
		const char* pathStr = narrow;
		u32 pathLen = u32(narrowLen);
#else
		const char* pathStr = relBuf.data;
		u32 pathLen = relBuf.count;
#endif

		// Round up to 4-byte boundary (may equal pathLen when already aligned).
		u32 paddedLen  = (pathLen + 3u) & ~3u;
		u32 recordSize = paddedLen + 4u;  // padded path + checksum u32

		u64 offset = m_pathBuf.size();
		m_pathBuf.resize(offset + 4u + paddedLen + 4u, u8(0));
		u8* p = m_pathBuf.data() + offset;

		// 1. Leading u32: record_size | kPathMask
		u32 sizeWithFlag = recordSize | kPathMask;
		memcpy(p, &sizeWithFlag, 4);  p += 4;

		// 2. Path bytes (null-padded region already zeroed by resize)
		if (pathLen) memcpy(p, pathStr, pathLen);
		p += paddedLen;

		// 3. Checksum u32: ~id
		u32 checksum = ~id;
		memcpy(p, &checksum, 4);

		// Publish the id — now safe since the path record is in the buffer.
		m_pathIds.emplace(std::move(key), id);
		return id;
	}

	// -----------------------------------------------------------------------
	// RecordResult
	// -----------------------------------------------------------------------

	// Returns current wall-clock time in the same units as a stored file mtime
	// on this platform. Windows: FILETIME (100ns since 1601, matches
	// FileBasicInformation::lastWriteTime). POSIX: 100ns since 1970 Unix epoch,
	// matching FromTimeSpec(st_mtimespec).
	static u64 NowAsFileMtime()
	{
#if PLATFORM_WINDOWS
		FILETIME ft;
		GetSystemTimeAsFileTime(&ft);
		return (u64(ft.dwHighDateTime) << 32) | u64(ft.dwLowDateTime);
#else
		timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		return u64(ts.tv_sec) * 10'000'000ull + u64(ts.tv_nsec / 100);
#endif
	}

	void NinjaDepsLog::RecordResult(Logger& logger, const ProcessHandle& handle,
		const tchar* primaryOutputAbsPath, bool restat, u64 cmdHashOverride)
	{
		const auto& trackedOutputs = handle.GetTrackedOutputs();
		const auto& trackedInputs  = handle.GetTrackedInputs();

		// Key the log off the caller-provided declared primary output when given.
		// Fall back to UBA's trackedOutputs[0] for callers that don't know the
		// edge's declared output (and accept the sidecar-first quirk).
		StringBuffer<512> firstOutput;
		if (primaryOutputAbsPath && *primaryOutputAbsPath)
		{
			firstOutput.Append(primaryOutputAbsPath);
		}
		else
		{
			if (trackedOutputs.empty())
				return;
			BinaryReader r(trackedOutputs.data(), 0, trackedOutputs.size());
			if (!r.GetLeft())
				return;
			r.ReadString(firstOutput);
		}
		if (!firstOutput.count)
			return;

		// ------------------------------------------------------------------
		// Timing (relative to build start)
		// ------------------------------------------------------------------
		u64 wallTime = handle.GetTotalWallTime();
		u64 nowTime  = GetTime();
		u64 endMs    = TimeToMs(nowTime > m_buildStart ? nowTime - m_buildStart : 0);
		u64 startMs  = endMs > TimeToMs(wallTime) ? endMs - TimeToMs(wallTime) : 0;

		// ------------------------------------------------------------------
		// Recorded mtime for log/deps entries.
		//
		// Non-restat edges: stat the output and use its lastWriteTime, as
		// ninja does. This is what consumers compare their input mtimes
		// against on subsequent builds.
		//
		// Restat edges: record "now" instead. The output may not have been
		// rewritten by the command (that's the whole point of restat) so
		// using its actual mtime can register as older than inputs that
		// were created/copied afterwards, causing the edge to look dirty on
		// every build. Using wall-clock-now as the recorded mtime makes
		// subsequent checks compare inputs against "last build" — a touched
		// input rebuilds the edge, an untouched one leaves it alone.
		// ------------------------------------------------------------------
		u64 mtime64 = 0;
		if (restat)
		{
			mtime64 = NowAsFileMtime();
		}
		else
		{
			FileBasicInformation fbi;
			if (GetFileBasicInformation(fbi, logger, firstOutput.data, /*errorOnFail=*/false))
				mtime64 = fbi.lastWriteTime;
		}

		// ------------------------------------------------------------------
		// Collect dep IDs (inputs) — no lock held while iterating.
		//
		// UBA's trackedInputs includes every file the process read during
		// execution. Some of those are the process's own outputs (e.g. a
		// linker reading its own .pdb or .ilk that it writes), which real
		// ninja never records as deps. Filter those out so mtime checks
		// don't conclude "dep (pdb) is newer than out (exe)" every run —
		// they're always newer; they were written by the same step.
		// ------------------------------------------------------------------
		Set<TString> outputSet; // full-path set for exclusion
		if (!trackedOutputs.empty())
		{
			BinaryReader r(trackedOutputs.data(), 0, trackedOutputs.size());
			StringBuffer<512> outPath;
			while (r.GetLeft())
			{
				outPath.Clear();
				r.ReadString(outPath);
				if (outPath.count)
					outputSet.emplace(outPath.data, outPath.count);
			}
		}

		Vector<u32> depIds;
		if (!trackedInputs.empty())
		{
			BinaryReader r(trackedInputs.data(), 0, trackedInputs.size());
			StringBuffer<512> depPath;
			depIds.reserve(128);
			while (r.GetLeft())
			{
				depPath.Clear();
				r.ReadString(depPath);
				if (!depPath.count)
					continue;
				// Skip if this input is also one of the process's outputs.
				if (outputSet.find(TString(depPath.data, depPath.count)) != outputSet.end())
					continue;
				depIds.push_back(GetOrAddPath(StringView(depPath.data, depPath.count)));
			}
		}

		// Primary output path ID (also added to the shared path table).
		u32 outId = GetOrAddPath(StringView(firstOutput.data, firstOutput.count));

		// ------------------------------------------------------------------
		// Deps record (ninja deps v4)
		//
		//   u32 record_size  = 4 * (3 + dep_count)  [out_id + mtime_lo + mtime_hi + dep_ids]
		//   u32 out_id
		//   u32 mtime_lo     (low  32 bits of 64-bit timestamp)
		//   u32 mtime_hi     (high 32 bits of 64-bit timestamp)
		//   u32 dep_id[dep_count]
		// ------------------------------------------------------------------
		u32 depCount   = u32(depIds.size());
		u32 recordSize = 4u * (3u + depCount);
		u32 totalBytes = 4u + recordSize;

		u32 mtimeLo = u32(mtime64 & 0xFFFFFFFFu);
		u32 mtimeHi = u32(mtime64 >> 32);

		{
			Vector<u8> rec(totalBytes);
			u8* p = rec.data();
			memcpy(p, &recordSize, 4); p += 4;
			memcpy(p, &outId,      4); p += 4;
			memcpy(p, &mtimeLo,    4); p += 4;
			memcpy(p, &mtimeHi,    4); p += 4;
			if (depCount)
				memcpy(p, depIds.data(), 4u * depCount);

			SCOPED_WRITE_LOCK(m_depsLock, wl);
			u64 off = m_depsBuf.size();
			m_depsBuf.resize(off + totalBytes);
			memcpy(m_depsBuf.data() + off, rec.data(), totalBytes);
		}

		// ------------------------------------------------------------------
		// Log record (ninja log v5)
		//
		//   start_ms\tend_ms\tmtime\toutput_path\tcommand_hash_hex\n
		// ------------------------------------------------------------------
		{
			const ProcessStartInfo& si = handle.GetStartInfo();
			// If the caller supplied the pre-Expand hash, use it verbatim.
			// Otherwise fall back to hashing the post-Expand arguments (which
			// is fine for callers that don't shell-wrap — the transformation
			// is identity then).
			u64 cmdHashVal = cmdHashOverride;
			if (!cmdHashVal)
			{
				StringKey k = ToStringKeyNoCheck(si.arguments, TStrlen(si.arguments));
				cmdHashVal = k.a;
			}
			struct { u64 a; } cmdHash { cmdHashVal };

			StringBuffer<512> relOut;
			MakeRelative(relOut, StringView(firstOutput.data, firstOutput.count));

#if PLATFORM_WINDOWS
			char narrowOut[4096];
			int narrowOutLen = WideCharToMultiByte(CP_UTF8, 0, relOut.data, (int)relOut.count,
			                                       narrowOut, (int)(sizeof(narrowOut) - 1), nullptr, nullptr);
			if (narrowOutLen < 0) narrowOutLen = 0;
			narrowOut[narrowOutLen] = '\0';
			const char* outPathStr = narrowOut;
#else
			const char* outPathStr = relOut.data;
#endif

			char logLine[4096];
			int logLineLen = snprintf(logLine, sizeof(logLine),
				"%llu\t%llu\t%llu\t%s\t%llx\n",
				(unsigned long long)startMs,
				(unsigned long long)endMs,
				(unsigned long long)mtime64,
				outPathStr,
				(unsigned long long)cmdHash.a);

			if (logLineLen > 0 && logLineLen < (int)sizeof(logLine))
			{
				SCOPED_WRITE_LOCK(m_logLock, wl);
				u64 off = m_logBuf.size();
				m_logBuf.resize(off + u64(logLineLen));
				memcpy(m_logBuf.data() + off, logLine, u64(logLineLen));
			}
		}
	}

	// -----------------------------------------------------------------------
	// Save
	// -----------------------------------------------------------------------

	bool NinjaDepsLog::Save(Logger& logger, StringView ninjaDir)
	{
		StringBuffer<512> dirBuf;
		dirBuf.Append(ninjaDir).EnsureEndsWithSlash();

		StringBuffer<512> depsPathBuf(dirBuf);
		depsPathBuf.Append(TC(".ninja_deps"));

		StringBuffer<512> logPathBuf(dirBuf);
		logPathBuf.Append(TC(".ninja_log"));

		// ----- .ninja_deps -----
		{
			FileHandle fh = CreateFileW(depsPathBuf.data, GENERIC_WRITE, 0, CREATE_ALWAYS, DefaultAttributes());
			if (fh == InvalidFileHandle)
				return logger.Error(TC("NinjaDepsLog: Failed to create %s"), depsPathBuf.data);
			auto fg = MakeGuard([&]() { CloseFile(depsPathBuf.data, fh); });

			// Header: 12 ASCII bytes + u32 version
			static const char depsHeader[] = "# ninjadeps\n";
			constexpr u32 depsHeaderLen = u32(sizeof(depsHeader) - 1);
			const u32 depsVersion = 4u;

			if (!WriteFile(logger, depsPathBuf.data, fh, depsHeader, depsHeaderLen))
				return false;
			if (!WriteFile(logger, depsPathBuf.data, fh, &depsVersion, 4))
				return false;

			// All path records first — they are always written before the deps records
			// that reference them, so the file is valid even if read sequentially.
			if (!m_pathBuf.empty())
				if (!WriteFile(logger, depsPathBuf.data, fh, m_pathBuf.data(), m_pathBuf.size()))
					return false;

			// Then all deps records.
			if (!m_depsBuf.empty())
				if (!WriteFile(logger, depsPathBuf.data, fh, m_depsBuf.data(), m_depsBuf.size()))
					return false;
		}

		// ----- .ninja_log -----
		{
			FileHandle fh = CreateFileW(logPathBuf.data, GENERIC_WRITE, 0, CREATE_ALWAYS, DefaultAttributes());
			if (fh == InvalidFileHandle)
				return logger.Error(TC("NinjaDepsLog: Failed to create %s"), logPathBuf.data);
			auto fg = MakeGuard([&]() { CloseFile(logPathBuf.data, fh); });

			static const char logHeader[] = "# ninja log v5\n";
			constexpr u32 logHeaderLen = u32(sizeof(logHeader) - 1);

			if (!WriteFile(logger, logPathBuf.data, fh, logHeader, logHeaderLen))
				return false;
			if (!m_logBuf.empty())
				if (!WriteFile(logger, logPathBuf.data, fh, m_logBuf.data(), m_logBuf.size()))
					return false;
		}

		logger.Detail(TC("Wrote %s (%u paths) and %s"),
			depsPathBuf.data, m_nextPathId, logPathBuf.data);

		return true;
	}

	// =======================================================================
	// NinjaLogIndex - reader for .ninja_log (v5) and .ninja_deps (v4)
	// =======================================================================

	namespace
	{
		// Convert a char-based relative path from the log file to an absolute-path
		// StringKey consistent with the mtime cache's keying (lowercase on Windows).
		// Normalizes separator by going through FixPath. Also returns the absolute
		// tchar path in absScratch for the caller to copy elsewhere.
		StringKey RelativePathToAbsKey(const char* data, u32 len, StringView workingDir,
			StringBufferBase& tcharScratch, StringBufferBase& absScratch)
		{
			tcharScratch.count = 0;
			if (len > tcharScratch.capacity - 1)
				len = u32(tcharScratch.capacity - 1);
			tcharScratch.count = len;
			for (u32 i = 0; i < len; ++i)
			{
				char c = data[i];
				tcharScratch.data[i] = (tchar)(u8)((c == '\\' || c == '/') ? PathSeparator : c);
			}
			tcharScratch.data[tcharScratch.count] = 0;

			FixPath(absScratch.Clear(), StringView(tcharScratch.data, tcharScratch.count), workingDir);
			return CaseInsensitiveFs
				? ToStringKeyLower(StringView(absScratch.data, absScratch.count))
				: ToStringKeyNoCheck(absScratch.data, absScratch.count);
		}

		bool LoadLogFileImpl(Logger& logger, const char* fileData, u64 fileSize,
			const tchar* fileName, StringView workingDir, NinjaLogIndex& idx)
		{
			static const char header[] = "# ninja log v5\n";
			constexpr u64 headerLen = sizeof(header) - 1;
			if (fileSize < headerLen || memcmp(fileData, header, headerLen) != 0)
				return logger.Error(TC("%s: unexpected header (not a ninja log v5)"), fileName);

			StringBuffer<512> tcharScratch, absScratch;
			const char* p   = fileData + headerLen;
			const char* end = fileData + fileSize;

			// Each record: start_ms\tend_ms\tmtime\toutput_path\tcommand_hash_hex\n
			while (p < end)
			{
				const char* lineEnd = (const char*)memchr(p, '\n', u64(end - p));
				if (!lineEnd)
					lineEnd = end;

				const char* cursor = p;
				const char* tabs[5] = {};
				u32 tabCount = 0;
				while (cursor < lineEnd && tabCount < 5)
				{
					const char* t = (const char*)memchr(cursor, '\t', u64(lineEnd - cursor));
					if (!t)
						break;
					tabs[tabCount++] = t;
					cursor = t + 1;
				}
				if (tabCount == 4)
				{
					u64 mtime = strtoull(tabs[1] + 1, nullptr, 10);
					const char* pathStart = tabs[2] + 1;
					u32 pathLen = u32(tabs[3] - pathStart);
					const char* hashStart = tabs[3] + 1;
					u32 hashLen = u32(lineEnd - hashStart);

					StringKey outKey = RelativePathToAbsKey(pathStart, pathLen, workingDir,
						tcharScratch, absScratch);

					u64 cmdHash = 0;
					for (u32 i = 0; i < hashLen; ++i)
					{
						char c = hashStart[i];
						u8 nibble;
						if      (c >= '0' && c <= '9') nibble = u8(c - '0');
						else if (c >= 'a' && c <= 'f') nibble = u8(c - 'a' + 10);
						else if (c >= 'A' && c <= 'F') nibble = u8(c - 'A' + 10);
						else break;
						cmdHash = (cmdHash << 4) | nibble;
					}

					// Last writer wins when the same output appears multiple times
					// (ninja appends; tail entry is freshest).
					NinjaLogIndex::LogEntry& e = idx.logByOutputKey[outKey];
					e.outMtime = mtime;
					e.cmdHash  = cmdHash;
				}
				// Malformed lines are silently skipped — matches ninja's tolerant reader.

				p = (lineEnd < end) ? lineEnd + 1 : end;
			}
			return true;
		}

		bool LoadDepsFileImpl(Logger& logger, const u8* fileData, u64 fileSize,
			const tchar* fileName, StringView workingDir, NinjaLogIndex& idx)
		{
			static const char header[] = "# ninjadeps\n";
			constexpr u64 headerLen = sizeof(header) - 1;
			if (fileSize < headerLen + 4 || memcmp(fileData, header, headerLen) != 0)
				return logger.Error(TC("%s: unexpected header (not a ninjadeps file)"), fileName);

			u32 version;
			memcpy(&version, fileData + headerLen, 4);
			if (version != 4)
				return logger.Error(TC("%s: unsupported deps version %u (expected 4)"), fileName, version);

			// Per-file local path id (by record order) -> absolute-path StringKey
			// and the absolute path bytes for later stat-pass parent registration.
			struct LocalPath
			{
				StringKey key;
				u32       offset; // into idx.flatDepPathBuf
				u32       len;
			};
			Vector<LocalPath> localPaths;
			localPaths.reserve(65536);

			StringBuffer<512> tcharScratch, absScratch;
			const u8* p   = fileData + headerLen + 4;
			const u8* end = fileData + fileSize;

			while (p + 4 <= end)
			{
				u32 sizeWithFlag;
				memcpy(&sizeWithFlag, p, 4);
				p += 4;

				bool isPath = (sizeWithFlag & kPathMask) != 0;
				u32 recordSize = sizeWithFlag & ~kPathMask;

				if (p + recordSize > end)
					return logger.Error(TC("%s: truncated record (size %u, %lld remaining)"),
						fileName, recordSize, s64(end - p));

				if (isPath)
				{
					if (recordSize < 4)
						return logger.Error(TC("%s: path record too short"), fileName);
					u32 paddedLen = recordSize - 4;

					// Strip trailing null padding (writer zero-fills)
					u32 pathLen = paddedLen;
					while (pathLen > 0 && p[pathLen - 1] == 0)
						--pathLen;

					u32 checksum;
					memcpy(&checksum, p + paddedLen, 4);
					u32 expectedId = ~checksum;
					if (expectedId != localPaths.size())
					{
						return logger.Error(TC("%s: path record id mismatch (got %u, expected %u)"),
							fileName, expectedId, u32(localPaths.size()));
					}

					StringKey key = RelativePathToAbsKey((const char*)p, pathLen, workingDir,
						tcharScratch, absScratch);

					// Stash the abs path in flatDepPathBuf.
					u32 bufOff = u32(idx.flatDepPathBuf.size());
					idx.flatDepPathBuf.resize(bufOff + absScratch.count);
					memcpy(idx.flatDepPathBuf.data() + bufOff, absScratch.data, absScratch.count * sizeof(tchar));

					LocalPath lp;
					lp.key    = key;
					lp.offset = bufOff;
					lp.len    = absScratch.count;
					localPaths.push_back(lp);
				}
				else
				{
					if (recordSize < 12 || (recordSize & 3) != 0)
						return logger.Error(TC("%s: malformed deps record (size %u)"), fileName, recordSize);

					u32 outLocalId;
					u32 mtimeLo;
					u32 mtimeHi;
					memcpy(&outLocalId, p +  0, 4);
					memcpy(&mtimeLo,    p +  4, 4);
					memcpy(&mtimeHi,    p +  8, 4);

					u32 depCount = (recordSize - 12) / 4;
					if (outLocalId >= localPaths.size())
						return logger.Error(TC("%s: deps record references unknown path id %u"),
							fileName, outLocalId);

					StringKey outKey = localPaths[outLocalId].key;
					u64 mtime = (u64(mtimeHi) << 32) | u64(mtimeLo);

					u32 depOffset = u32(idx.flatDepKeys.size());
					idx.flatDepKeys.resize(depOffset + depCount);
					idx.flatDepPathOffset.resize(depOffset + depCount);
					idx.flatDepPathLen.resize(depOffset + depCount);
					for (u32 i = 0; i < depCount; ++i)
					{
						u32 depLocalId;
						memcpy(&depLocalId, p + 12 + i * 4, 4);
						if (depLocalId >= localPaths.size())
							return logger.Error(TC("%s: deps record references unknown dep id %u"),
								fileName, depLocalId);
						const LocalPath& lp = localPaths[depLocalId];
						idx.flatDepKeys[depOffset + i]       = lp.key;
						idx.flatDepPathOffset[depOffset + i] = lp.offset;
						idx.flatDepPathLen[depOffset + i]    = lp.len;
					}

					NinjaLogIndex::DepsEntry& e = idx.depsByOutputKey[outKey];
					e.outMtime  = mtime;
					e.depOffset = depOffset;
					e.depCount  = depCount;
				}

				p += recordSize;
			}
			return true;
		}
	}

	bool NinjaLogIndex::Load(Logger& logger, StringView ninjaDir, StringView workingDir)
	{
		// .ninja_log
		{
			StringBuffer<512> logPath(ninjaDir);
			logPath.EnsureEndsWithSlash().Append(TC(".ninja_log"));

			FileAccessor fa(logger, logPath.data);
			if (fa.OpenMemoryRead(0, /*errorOnFail=*/false))
			{
				logByOutputKey.reserve(65536);
				if (!LoadLogFileImpl(logger, (const char*)fa.GetData(), fa.GetSize(),
					logPath.data, workingDir, *this))
					return false;
			}
		}

		// .ninja_deps
		{
			StringBuffer<512> depsPath(ninjaDir);
			depsPath.EnsureEndsWithSlash().Append(TC(".ninja_deps"));

			FileAccessor fa(logger, depsPath.data);
			if (fa.OpenMemoryRead(0, /*errorOnFail=*/false))
			{
				depsByOutputKey.reserve(65536);
				flatDepKeys.reserve(2 * 1024 * 1024);
				if (!LoadDepsFileImpl(logger, fa.GetData(), fa.GetSize(),
					depsPath.data, workingDir, *this))
					return false;
			}
		}

		return true;
	}
}
