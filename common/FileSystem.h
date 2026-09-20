// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Pcsx2Defs.h"

#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <sys/stat.h>

class Error;
class ProgressCallback;

#ifdef _WIN32
#define FS_OSPATH_SEPARATOR_CHARACTER '\\'
#define FS_OSPATH_SEPARATOR_STR "\\"
#else
#define FS_OSPATH_SEPARATOR_CHARACTER '/'
#define FS_OSPATH_SEPARATOR_STR "/"
#endif

enum FILESYSTEM_FILE_ATTRIBUTES
{
	FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY = 1,
	FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY = 2,
	FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED = 4,
};

enum FILESYSTEM_FIND_FLAGS
{
	FILESYSTEM_FIND_RECURSIVE = (1 << 0),
	FILESYSTEM_FIND_RELATIVE_PATHS = (1 << 1),
	FILESYSTEM_FIND_HIDDEN_FILES = (1 << 2),
	FILESYSTEM_FIND_FOLDERS = (1 << 3),
	FILESYSTEM_FIND_FILES = (1 << 4),
	FILESYSTEM_FIND_KEEP_ARRAY = (1 << 5),
	FILESYSTEM_FIND_SORT_BY_NAME = (1 << 6),
};

struct FILESYSTEM_STAT_DATA
{
	std::time_t CreationTime;
	std::time_t ModificationTime;
	s64 Size;
	u32 Attributes;
};

struct FILESYSTEM_FIND_DATA
{
	std::time_t CreationTime;
	std::time_t ModificationTime;
	std::string FileName;
	s64 Size;
	u32 Attributes;
};

namespace FileSystem
{
	using FindResultsArray = std::vector<FILESYSTEM_FIND_DATA>;

	std::vector<std::string> GetRootDirectoryList();

	bool FindFiles(const char* path, const char* pattern, u32 flags, FindResultsArray* results, ProgressCallback* cancel = nullptr);

	bool StatFile(const char* path, struct stat* st);
	bool StatFile(std::FILE* fp, struct stat* st);
	bool StatFile(const char* path, FILESYSTEM_STAT_DATA* pStatData);
	bool StatFile(std::FILE* fp, FILESYSTEM_STAT_DATA* pStatData);
	s64 GetPathFileSize(const char* path);

	std::optional<std::time_t> GetFileTimestamp(const char* path);

	bool FileExists(const char* path);

	bool DirectoryExists(const char* path);

	bool DirectoryIsEmpty(const char* path);

	bool DeleteFilePath(const char* path, Error* error = nullptr);

	bool RenamePath(const char* OldPath, const char* NewPath, Error* error = nullptr);

	struct FileDeleter
	{
		void operator()(std::FILE* fp)
		{
			std::fclose(fp);
		}
	};

	using ManagedCFilePtr = std::unique_ptr<std::FILE, FileDeleter>;
	ManagedCFilePtr OpenManagedCFile(const char* filename, const char* mode, Error* error = nullptr);
	ManagedCFilePtr OpenManagedCFileTryIgnoreCase(const char* filename, const char* mode, Error* error = nullptr);
	std::FILE* OpenCFile(const char* filename, const char* mode, Error* error = nullptr);
	std::FILE* OpenCFileTryIgnoreCase(const char* filename, const char* mode, Error* error = nullptr);

	int FSeek64(std::FILE* fp, s64 offset, int whence);
	s64 FTell64(std::FILE* fp);
	s64 FSize64(std::FILE* fp);

	int OpenFDFile(const char* filename, int flags, int mode, Error* error = nullptr);

	enum class FileShareMode
	{
		DenyReadWrite,
		DenyWrite,
		DenyRead,
		DenyNone,
	};

	ManagedCFilePtr OpenManagedSharedCFile(const char* filename, const char* mode, FileShareMode share_mode, Error* error = nullptr);
	std::FILE* OpenSharedCFile(const char* filename, const char* mode, FileShareMode share_mode, Error* error = nullptr);

	std::optional<std::vector<u8>> ReadBinaryFile(const char* filename);
	std::optional<std::vector<u8>> ReadBinaryFile(std::FILE* fp);
	std::optional<std::string> ReadFileToString(const char* filename);
	std::optional<std::string> ReadFileToString(std::FILE* fp);
	bool WriteBinaryFile(const char* filename, const void* data, size_t data_length);
	bool WriteStringToFile(const char* filename, const std::string_view sv);
	size_t ReadFileWithProgress(std::FILE* fp, void* dst, size_t length, ProgressCallback* progress,
		Error* error = nullptr, size_t chunk_size = 16 * 1024 * 1024);
	size_t ReadFileWithPartialProgress(std::FILE* fp, void* dst, size_t length, ProgressCallback* progress,
		int startPercent, int endPercent, Error* error = nullptr, size_t chunk_size = 16 * 1024 * 1024);
	std::span<const u8> MapBinaryFileForRead(const char* filename);
	std::span<const u8> MapBinaryFileForRead(std::FILE* fp);
	void UnmapFile(std::span<const u8> file);

	bool CreateDirectoryPath(const char* path, bool recursive, Error* error = nullptr);

	bool EnsureDirectoryExists(const char* path, bool recursive, Error* error = nullptr);

	bool DeleteDirectory(const char* path);

	bool RecursiveDeleteDirectory(const char* path);

	bool CopyFilePath(const char* source, const char* destination, bool replace);

	std::string GetPackagePath();

	std::string GetProgramPath();

	std::string GetWorkingDirectory();

	bool SetWorkingDirectory(const char* path);

	bool SetPathCompression(const char* path, bool enable);

	bool CreateSymLink(const char* link, const char* target);

	bool IsSymbolicLink(const char* path);

	bool DeleteSymbolicLink(const char* path, Error* error = nullptr);

#ifdef _WIN32
	bool GetWin32Path(std::wstring* dest, std::string_view str);
	std::wstring GetWin32Path(std::string_view str);
#endif

#ifndef _WIN32
	class POSIXLock
	{
	public:
		POSIXLock(int fd);
		POSIXLock(std::FILE* fp);
		~POSIXLock();

	private:
		int m_fd;
	};
#endif
};
