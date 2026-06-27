// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaDirectoryTableHolder.h"
#include "UbaTest.h"

namespace uba
{
	bool TestDirectoryTable(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		DirectoryTableHolder holder(logger.m_writer, TC(""), false);
		holder.SetTreatTempDirAsEmpty();

		CHECK_TRUE(holder.RefreshDirectory(testRootDir.data, true));

		MemoryBlock dirMem(TC("Mem"));
		DirectoryTable dir(dirMem);
		dir.Init(holder.GetDirectoryTableMemory(), 0, holder.GetDirectoryTableSize(nullptr).main);
		
		CHECK_TRUE(dir.EntryExists(testRootDir, false) == DirectoryTable::Exists_Yes);
		CHECK_TRUE(dir.EntryExists(testRootDir, true) == DirectoryTable::Exists_Yes);

		StringBuffer<> fooFile;
		CHECK_TRUE(CreateTestFile(fooFile, logger, testRootDir, TCV("foo.txt"), TCV("foo")));
		CHECK_TRUE(holder.RegisterNewFile(fooFile.data));

		CHECK_TRUE(dir.EntryExists(fooFile, true) == DirectoryTable::Exists_No);
		dir.ParseDirectoryTable(holder.GetDirectoryTableSize(nullptr));
		CHECK_TRUE(dir.EntryExists(fooFile, true) == DirectoryTable::Exists_Yes);


		DirectoryTableOverlay overlay;
		holder.CreateOverlay(overlay);

		dir.InitOverlay(overlay.memory, overlay.size);

		StringBuffer<> barFile;
		CHECK_TRUE(CreateTestFile(barFile, logger, testRootDir, TCV("bar.txt"), TCV("bar")));
		CHECK_TRUE(holder.PopulateOverlayFile(overlay, barFile, [](FileInformation& out, StringKey) { out.size = sizeof(tchar)*3; out.attributes = DefaultAttributes(); return true; }));

		CHECK_TRUE(dir.EntryExists(barFile, true) == DirectoryTable::Exists_No);
		dir.ParseDirectoryTable(holder.GetDirectoryTableSize(&overlay));
		CHECK_TRUE(dir.EntryExists(barFile, true) == DirectoryTable::Exists_Yes);

		StringBuffer<> mehFile;
		CHECK_TRUE(CreateTestFile(mehFile, logger, testRootDir, TCV("meh.txt"), TCV("meh")));
		CHECK_TRUE(holder.RegisterNewFile(mehFile.data));

		CHECK_TRUE(dir.EntryExists(mehFile, true) == DirectoryTable::Exists_No);
		dir.ParseDirectoryTable(holder.GetDirectoryTableSize(&overlay));
		CHECK_TRUE(dir.EntryExists(mehFile, true) == DirectoryTable::Exists_Yes);

		u32 counter = 0;
		StringBuffer<> testRootDirForLookup(StringView(testRootDir.data, testRootDir.count - 1));
		if (CaseInsensitiveFs)
			testRootDirForLookup.MakeLower();

		auto findIt = dir.m_lookup.find(ToStringKey(testRootDirForLookup));
		CHECK_TRUE(findIt != dir.m_lookup.end());
		CHECK_TRUE(findIt->second.files.size() == 3);

		bool success = true;
		dir.TraverseFilesRecursiveNoLock(testRootDirForLookup, [&](const DirectoryTable::EntryInformation& info, StringView fileName, DirTableOffset fileOffset)
			{
				++counter;
				success = success && info.size == sizeof(tchar)*3;
				success = success && info.attributes != 0;
				success = success && fileName.GetFileName().count == 7;
			});
		CHECK_TRUE(success);
		CHECK_TRUE(counter == 3);

		// Remove file by writing entry with attributes=0
		CHECK_TRUE(holder.WriteOverlayEntry(overlay, barFile, {}));
		dir.ParseDirectoryTable(holder.GetDirectoryTableSize(&overlay));
		DirTableOffset entryOffset;
		CHECK_TRUE(dir.EntryExists(barFile, false, &entryOffset) == DirectoryTable::Exists_Yes);
		CHECK_TRUE(!dir.GetAttributes(entryOffset));

		return true;
	}
}
