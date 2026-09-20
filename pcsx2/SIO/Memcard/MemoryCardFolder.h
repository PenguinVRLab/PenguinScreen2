// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "Config.h"

#pragma pack(push, 1)
struct superblock
{
	char magic[28];
	char version[12];
	u16 page_len;
	u16 pages_per_cluster;
	u16 pages_per_block;
	u16 unused;
	u32 clusters_per_card;
	u32 alloc_offset;
	u32 alloc_end;
	u32 rootdir_cluster;
	u32 backup_block1;
	u32 backup_block2;
	u64 padding0x48;
	u32 ifc_list[32];
	u32 bad_block_list[32];
	u8 card_type;
	u8 card_flags;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MemoryCardFileEntryDateTime
{
	u8 unused;
	u8 second;
	u8 minute;
	u8 hour;
	u8 day;
	u8 month;
	u16 year;

	static MemoryCardFileEntryDateTime FromTime(time_t time);

	time_t ToTime() const;

	bool operator==(const MemoryCardFileEntryDateTime& other) const
	{
		return unused == other.unused && second == other.second && minute == other.minute && hour == other.hour && day == other.day && month == other.month && year == other.year;
	}
	bool operator!=(const MemoryCardFileEntryDateTime& other) const
	{
		return !(*this == other);
	}
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MemoryCardFileEntry
{
	enum MemoryCardFileModeFlags
	{
		Mode_Read = 0x0001,
		Mode_Write = 0x0002,
		Mode_Execute = 0x0004,
		Mode_CopyProtected = 0x0008,
		Mode_File = 0x0010,
		Mode_Directory = 0x0020,
		Mode_Unknown0x0040 = 0x0040,
		Mode_Unknown0x0080 = 0x0080,
		Mode_Unknown0x0100 = 0x0100,
		Mode_Unknown0x0200 = 0x0200,
		Mode_Unknown0x0400 = 0x0400,
		Mode_PocketStation = 0x0800,
		Mode_PSX = 0x1000,
		Mode_Unknown0x2000 = 0x2000,
		Mode_Unknown0x4000 = 0x4000,
		Mode_Used = 0x8000
	};

	union
	{
		struct MemoryCardFileEntryData
		{
			u32 mode;
			u32 length;
			MemoryCardFileEntryDateTime timeCreated;
			u32 cluster;
			u32 dirEntry;
			MemoryCardFileEntryDateTime timeModified;
			u32 attr;
			u8 padding[0x1C];
			u8 name[0x20];
			u8 unused[0x1A0];
		} data;
		u8 raw[0x200];
	} entry;

	bool IsFile() const { return !!(entry.data.mode & Mode_File); }
	bool IsDir() const { return !!(entry.data.mode & Mode_Directory); }
	bool IsUsed() const { return !!(entry.data.mode & Mode_Used); }
	bool IsValid() const { return entry.data.mode != 0xFFFFFFFF; }
	bool IsDotDir() const { return entry.data.name[0] == '.' && (entry.data.name[1] == '\0' || (entry.data.name[1] == '.' && entry.data.name[2] == '\0')); }

	static const u32 DefaultDirMode = Mode_Read | Mode_Write | Mode_Execute | Mode_Directory | Mode_Unknown0x0400 | Mode_Used;
	static const u32 DefaultFileMode = Mode_Read | Mode_Write | Mode_Execute | Mode_File | Mode_Unknown0x0080 | Mode_Unknown0x0400 | Mode_Used;

	static const u32 EmptyFileCluster = 0xFFFFFFFFu;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MemoryCardFileEntryCluster
{
	MemoryCardFileEntry entries[2];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MemoryCardPage
{
	static const int PageSize = 0x200;
	u8 raw[PageSize];
};
#pragma pack(pop)

struct MemoryCardFileEntryTreeNode
{
	MemoryCardFileEntry entry;
	std::vector<MemoryCardFileEntryTreeNode> subdir;

	MemoryCardFileEntryTreeNode(const MemoryCardFileEntry& entry)
		: entry(entry)
	{
	}
};

struct MemoryCardFileMetadataReference
{
	MemoryCardFileMetadataReference* parent;
	MemoryCardFileEntry* entry;
	u32 consecutiveCluster;

	bool GetPath(std::string* fileName) const;

	void GetInternalPath(std::string* fileName) const;
};

struct MemoryCardFileHandleStructure
{
	MemoryCardFileMetadataReference* fileRef;
	std::string hostFilePath;
	std::FILE* fileHandle;
};

class FileAccessHelper
{
private:
	std::map<std::string, MemoryCardFileHandleStructure> m_files;
	MemoryCardFileMetadataReference* m_lastWrittenFileRef = nullptr;

public:
	FileAccessHelper();
	~FileAccessHelper();

	std::FILE* ReOpen(const std::string_view folderName, MemoryCardFileMetadataReference* fileRef, bool writeMetadata = false);
	void CloseMatching(const std::string_view path);
	void CloseAll();
	void FlushAll();

	void ClearMetadataWriteState();

	static bool CleanMemcardFilename(char* name);

	static void WriteIndex(const std::string& baseFolderName, MemoryCardFileEntry* const entry, MemoryCardFileMetadataReference* const parent);

private:
	static bool CleanMemcardFilenameEndDotOrSpace(char* name, size_t length);

	std::FILE* Open(const std::string_view folderName, MemoryCardFileMetadataReference* fileRef, bool writeMetadata = false);
	void CloseFileHandle(std::FILE*& file, const MemoryCardFileEntry* entry = nullptr);

	void WriteMetadata(const std::string_view folderName, const MemoryCardFileMetadataReference* fileRef);
};

class FolderMemoryCard
{
public:
	static const int IndirectFatClusterCount = 1;
	static const int PageSize = MemoryCardPage::PageSize;
	static const int ClusterSize = PageSize * 2;
	static const int BlockSize = ClusterSize * 8;
	static const int EccSize = 0x10;
	static const int PageSizeRaw = PageSize + EccSize;
	static const int ClusterSizeRaw = PageSizeRaw * 2;
	static const int BlockSizeRaw = ClusterSizeRaw * 8;
	static const int TotalPages = 0x4000;
	static const int TotalClusters = TotalPages / 2;
	static const int TotalBlocks = TotalClusters / 8;
	static const int TotalSizeRaw = TotalPages * PageSizeRaw;

	static const u32 IndirectFatUnused = 0xFFFFFFFFu;
	static const u32 LastDataCluster = 0x7FFFFFFFu;
	static const u32 NextDataClusterMask = 0x7FFFFFFFu;
	static const u32 DataClusterInUseMask = 0x80000000u;

	static const int FramesAfterWriteUntilFlush = 2;

protected:
	union superBlockUnion
	{
		superblock data;
		u8 raw[BlockSize];
	} m_superBlock;
	union indirectFatUnion
	{
		u32 data[IndirectFatClusterCount][ClusterSize / 4];
		u8 raw[IndirectFatClusterCount][ClusterSize];
	} m_indirectFat;
	union fatUnion
	{
		u32 data[IndirectFatClusterCount][ClusterSize / 4][ClusterSize / 4];
		u8 raw[IndirectFatClusterCount][ClusterSize / 4][ClusterSize];
	} m_fat;
	u8 m_backupBlock1[BlockSize];
	union backupBlock2Union
	{
		u32 programmedBlock;
		u8 raw[BlockSize];
	} m_backupBlock2;

	std::map<u32, MemoryCardFileEntryCluster> m_fileEntryDict;
	std::map<u32, MemoryCardFileMetadataReference> m_fileMetadataQuickAccess;

	std::map<u32, MemoryCardPage> m_cache;
	std::map<u32, MemoryCardPage> m_oldDataCache;
	int m_framesUntilFlush;
	u64 m_timeLastWritten;

	FileAccessHelper m_lastAccessedFile;

	std::string m_folderName;

	uint m_slot;

	bool m_isEnabled;

	bool m_performFileWrites;

	bool m_filteringEnabled;
	std::string m_filteringString;

public:
	FolderMemoryCard();
	virtual ~FolderMemoryCard() = default;

	void Lock();
	void Unlock();

	void Open(const bool enableFiltering, std::string filter);
	void Open(std::string fullPath, const Pcsx2Config::McdOptions& mcdOptions, const u32 sizeInClusters, const bool enableFiltering, std::string filter, bool simulateFileWrites = false);
	void Close(bool flush = true);
	bool IsFormatted() const;

	bool ReIndex(bool enableFiltering, const std::string& filter);

	s32 IsPresent() const;
	void GetSizeInfo(McdSizeInfo& outways) const;
	bool IsPSX() const;
	s32 Read(u8* dest, u32 adr, int size);
	s32 Save(const u8* src, u32 adr, int size);
	s32 EraseBlock(u32 adr);
	u64 GetCRC() const;

	void SetSlot(uint slot);

	u32 GetSizeInClusters() const;

	void SetSizeInClusters(u32 clusters);
	void SetSizeInMB(u32 megaBytes);

	void NextFrame();

	static void CalculateECC(u8* ecc, const u8* data);

	void WriteToFile(const std::string& filename);

	const std::string& GetFolderName();

protected:
	struct EnumeratedFileEntry
	{
		std::string m_fileName;
		time_t m_timeCreated;
		time_t m_timeModified;
		bool m_isFile;
	};

	void InitializeInternalData();

	u8* GetSystemBlockPointer(const u32 adr);

	u8* GetFileEntryPointer(const u32 searchCluster, const u32 entryNumber, const u32 offset);

	MemoryCardFileEntryCluster* GetFileEntryCluster(const u32 currentCluster, const u32 searchCluster, const u32 fileCount);

	MemoryCardFileEntry* GetFileEntryFromFileDataCluster(const u32 currentCluster, const u32 searchCluster, std::string* fileName, const size_t originalDirCount, u32* outClusterNumber);


	void LoadMemoryCardData(const u32 sizeInClusters, const bool enableFiltering, const std::string& filter);

	void CreateFat();

	void CreateRootDir();


	u32 GetFreeSystemCluster() const;

	u32 GetAmountDataClusters() const;

	u32 GetFreeDataCluster() const;

	u32 GetAmountFreeDataClusters() const;

	u32 GetLastClusterOfData(const u32 cluster) const;


	MemoryCardFileEntry* AppendFileEntryToDir(const MemoryCardFileEntry* const dirEntry);

	bool AddFolder(MemoryCardFileEntry* const dirEntry, const std::string& dirPath, MemoryCardFileMetadataReference* parent = nullptr, const bool enableFiltering = false, const std::string_view filter = "");

	bool AddFile(MemoryCardFileEntry* const dirEntry, const std::string& dirPath, const EnumeratedFileEntry& fileEntry, MemoryCardFileMetadataReference* parent = nullptr);

	u32 CalculateRequiredClustersOfDirectory(const std::string& dirPath) const;


	MemoryCardFileMetadataReference* AddFileEntryToMetadataQuickAccess(MemoryCardFileEntry* const entry, MemoryCardFileMetadataReference* const parent);

	MemoryCardFileMetadataReference* AddDirEntryToMetadataQuickAccess(MemoryCardFileEntry* const entry, MemoryCardFileMetadataReference* const parent);


	void ReadDataWithoutCache(u8* const dest, const u32 adr, const u32 dataLength);


	bool ReadFromFile(u8* dest, u32 adr, u32 dataLength);
	bool WriteToFile(const u8* src, u32 adr, u32 dataLength);


	void Flush();

	bool FlushPage(const u32 page);

	bool FlushCluster(const u32 cluster);

	bool FlushBlock(const u32 block);

	void FlushSuperBlock();

	void FlushFileEntries();

	void FlushFileEntries(const u32 dirCluster, const u32 remainingFiles, const std::string& dirPath = {}, MemoryCardFileMetadataReference* parent = nullptr);

	void FlushDeletedFilesAndRemoveUnchangedDataFromCache(const std::vector<MemoryCardFileEntryTreeNode>& oldFileEntries);

	void FlushDeletedFilesAndRemoveUnchangedDataFromCache(const std::vector<MemoryCardFileEntryTreeNode>& oldFileEntries, const u32 newCluster, const u32 newFileCount, const std::string& dirPath);

	void RemoveUnchangedDataFromCache(const MemoryCardFileEntry* const oldEntry, const MemoryCardFileEntry* const newEntry);

	s32 WriteWithoutCache(const u8* src, u32 adr, int size);

	void CopyEntryDictIntoTree(std::vector<MemoryCardFileEntryTreeNode>* fileEntryTree, const u32 cluster, const u32 fileCount);

	const MemoryCardFileEntry* FindEquivalent(const MemoryCardFileEntry* searchEntry, const u32 cluster, const u32 fileCount);

	void SetTimeLastReadToNow();
	void SetTimeLastWrittenToNow();

	void AttemptToRecreateIndexFile(const std::string& directory) const;

	std::string GetDisabledMessage(uint slot) const;
	std::string GetCardFullMessage(const std::string& filePath) const;

	std::vector<EnumeratedFileEntry> GetOrderedFiles(const std::string& dirPath) const;

	void DeleteFromIndex(const std::string& filePath, const std::string_view entry) const;
};

class FolderMemoryCardAggregator
{
protected:
	static const int TotalCardSlots = 8;
	FolderMemoryCard m_cards[TotalCardSlots];

	bool m_enableFiltering = true;
	std::string m_lastKnownFilter;

public:
	FolderMemoryCardAggregator();
	virtual ~FolderMemoryCardAggregator() = default;

	void Open();
	void Close();

	void SetFiltering(const bool enableFiltering);

	s32 IsPresent(uint slot);
	void GetSizeInfo(uint slot, McdSizeInfo& outways);
	bool IsPSX(uint slot);
	s32 Read(uint slot, u8* dest, u32 adr, int size);
	s32 Save(uint slot, const u8* src, u32 adr, int size);
	s32 EraseBlock(uint slot, u32 adr);
	u64 GetCRC(uint slot);
	void NextFrame(uint slot);
	bool ReIndex(uint slot, const bool enableFiltering, const std::string& filter);
};
