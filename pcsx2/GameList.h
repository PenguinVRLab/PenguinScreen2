// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GameDatabase.h"

#include "common/Pcsx2Defs.h"

#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ProgressCallback;

struct VMBootParameters;

namespace GameList
{
	enum class EntryType
	{
		PS2Disc,
		PS1Disc,
		ELF,
		Invalid,
		Count
	};

	enum class Region
	{
		NTSC_B,
		NTSC_C,
		NTSC_HK,
		NTSC_J,
		NTSC_K,
		NTSC_T,
		NTSC_U,
		Other,
		PAL_A,
		PAL_AU,
		PAL_AF,
		PAL_BE,
		PAL_E,
		PAL_F,
		PAL_FI,
		PAL_G,
		PAL_GR,
		PAL_I,
		PAL_IN,
		PAL_M,
		PAL_NL,
		PAL_NO,
		PAL_P,
		PAL_PL,
		PAL_R,
		PAL_S,
		PAL_SC,
		PAL_SW,
		PAL_SWI,
		PAL_UK,
		Count
	};

	using CompatibilityRating = GameDatabaseSchema::Compatibility;
	static constexpr u32 CompatibilityRatingCount = static_cast<u32>(GameDatabaseSchema::Compatibility::Perfect) + 1u;

	struct Entry
	{
		EntryType type = EntryType::PS2Disc;
		Region region = Region::Other;

		std::string path;
		std::string serial;
		std::string title;
		std::string title_sort;
		std::string title_en;
		u64 total_size = 0;
		std::time_t last_modified_time = 0;
		std::time_t last_played_time = 0;
		std::time_t total_played_time = 0;

		const std::string& GetTitle(bool force_en = false) const
		{
			return title_en.empty() || !force_en ? title : title_en;
		}
		const std::string& GetTitleSort(bool force_en = false) const
		{
			if (force_en && !title_en.empty())
				return title_en;
			return title_sort.empty() ? title : title_sort;
		}

		u32 crc = 0;

		CompatibilityRating compatibility_rating = CompatibilityRating::Unknown;

		__fi bool IsDisc() const { return (type == EntryType::PS1Disc || type == EntryType::PS2Disc); }
	};

	const char* EntryTypeToString(EntryType type, bool translate);
	const char* RegionToString(Region region, bool translate);
	const char* RegionToFlagFilename(Region region);
	const char* EntryCompatibilityRatingToString(CompatibilityRating rating, bool translate);

	void FillBootParametersForEntry(VMBootParameters* params, const Entry* entry);

	bool PopulateEntryFromPath(const std::string& path, GameList::Entry* entry);

	std::unique_lock<std::recursive_mutex> GetLock();
	const Entry* GetEntryByIndex(u32 index);
	const Entry* GetEntryForPath(const char* path);
	const Entry* GetEntryByCRC(u32 crc);
	const Entry* GetEntryBySerialAndCRC(const std::string_view serial, u32 crc);
	u32 GetEntryCount();

	void Refresh(bool invalidate_cache, bool only_cache = false, ProgressCallback* progress = nullptr);

	bool RescanPath(const std::string& path);

	bool GetSerialAndCRCForFilename(const char* filename, std::string* serial, u32* crc);

	void AddPlayedTimeForSerial(const std::string& serial, std::time_t last_time, std::time_t add_time);
	void ClearPlayedTimeForSerial(const std::string& serial);

	std::time_t GetCachedPlayedTimeForSerial(const std::string& serial);

	std::string FormatTimestamp(std::time_t timestamp);

	std::string FormatTimespan(const std::time_t timespan, const bool long_format = false);

	std::string GetCoverImagePathForEntry(const Entry* entry);
	std::string GetNewCoverImagePathForEntry(const Entry* entry, const char* new_filename, bool use_serial = false);

	bool DownloadCovers(const std::vector<std::string>& url_templates, bool use_serial = false, ProgressCallback* progress = nullptr,
		std::function<void(const Entry*, std::string)> save_callback = {});

	void CheckCustomAttributesForPath(const std::string& path, bool& has_custom_title, bool& has_custom_region);
	void SaveCustomTitleForPath(const std::string& path, const std::string& custom_title);
	void SaveCustomRegionForPath(const std::string& path, int custom_region);
	std::string GetCustomTitleForPath(const std::string& path);
}
