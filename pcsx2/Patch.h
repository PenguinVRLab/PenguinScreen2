// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"

#include "common/MemoryInterface.h"
#include "common/SmallString.h"

#include <string>
#include <string_view>
#include <vector>

class EEMemoryInterface;
class IOPMemoryInterface;

namespace Patch
{
	enum patch_place_type : u8
	{
		PPT_ONCE_ON_LOAD = 0,
		PPT_CONTINUOUSLY = 1,
		PPT_COMBINED_0_1 = 2,
		PPT_ON_LOAD_OR_WHEN_ENABLED = 3,

		PPT_END_MARKER
	};

	enum patch_cpu_type : u8
	{
		CPU_EE,
		CPU_IOP
	};

	enum patch_data_type : u8
	{
		BYTE_T,
		SHORT_T,
		WORD_T,
		DOUBLE_T,
		EXTENDED_T,
		SHORT_BE_T,
		WORD_BE_T,
		DOUBLE_BE_T,
		BYTES_T
	};

	static constexpr std::array<const char*, 4> s_place_to_string = {{"0", "1", "2", "3"}};
	static constexpr std::array<const char*, 2> s_cpu_to_string = {{"EE", "IOP"}};
	static constexpr std::array<const char*, 9> s_type_to_string = {
		{"byte", "short", "word", "double", "extended", "beshort", "beword", "bedouble", "bytes"}};

	struct PatchCommand
	{
		patch_place_type placetopatch;
		patch_cpu_type cpu;
		patch_data_type type;
		u32 addr;
		u64 data;
		u8* data_ptr;

		PatchCommand() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }
		PatchCommand(const PatchCommand& p) = delete;
		PatchCommand(PatchCommand&& p)
		{
			std::memcpy(static_cast<void*>(this), &p, sizeof(*this));
			p.data_ptr = nullptr;
		}
		~PatchCommand()
		{
			if (data_ptr)
				std::free(data_ptr);
		}

		PatchCommand& operator=(const PatchCommand& p) = delete;
		PatchCommand& operator=(PatchCommand&& p)
		{
			std::memcpy(static_cast<void*>(this), &p, sizeof(*this));
			p.data_ptr = nullptr;
			return *this;
		}

		bool operator==(const PatchCommand& p) const { return std::memcmp(this, &p, sizeof(*this)) == 0; }
		bool operator!=(const PatchCommand& p) const { return std::memcmp(this, &p, sizeof(*this)) != 0; }

		SmallString ToString() const
		{
			return SmallString::from_format("{},{},{},{:08x},{:x}", s_place_to_string[static_cast<u8>(placetopatch)],
				s_cpu_to_string[static_cast<u8>(cpu)], s_type_to_string[static_cast<u8>(type)], addr, data);
		}
	};
	static_assert(sizeof(PatchCommand) == 24, "PatchCommand has extra padding");

	struct DynamicPatchEntry
	{
		u32 offset;
		u32 value;
	};

	struct DynamicPatch
	{
		std::vector<DynamicPatchEntry> pattern;
		std::vector<DynamicPatchEntry> replacement;
	};

	struct PatchInfo
	{
		std::string name;
		std::string description;
		std::string author;

		std::optional<patch_place_type> place;

		std::string_view GetNamePart() const;
		std::string_view GetNameParentPart() const;
	};

	extern const char* PATCHES_CONFIG_SECTION;
	extern const char* CHEATS_CONFIG_SECTION;
	extern const char* PATCH_ENABLE_CONFIG_KEY;
	extern const char* PATCH_DISABLE_CONFIG_KEY;

	extern std::vector<PatchInfo> GetPatchInfo(const std::string_view serial, u32 crc, bool cheats, bool showAllCRCS, u32* num_unlabelled_patches);

	extern std::string GetPnachFilename(const std::string_view serial, u32 crc, bool cheats);

	extern void ReloadPatches(const std::string& serial, u32 crc, bool reload_files, bool reload_enabled_list, bool verbose, bool verbose_if_changed);

	extern void UpdateActivePatches(bool reload_enabled_list, bool verbose, bool verbose_if_changed, bool apply_new_patches);
	extern void ApplyPatchSettingOverrides();
	extern bool ReloadPatchAffectingOptions();
	extern void UnloadPatches();

	extern void LoadDynamicPatches(const std::vector<DynamicPatch>& patches);
	extern void ApplyDynamicPatches(u32 pc);

	extern void ApplyBootPatches();

	extern void ApplyVsyncPatches();

	extern void ApplyPatches(
		const std::vector<const PatchCommand*>& patches,
		patch_place_type place,
		EEMemoryInterface& ee,
		IOPMemoryInterface& iop);
	extern void ApplyPatches(
		const std::vector<const PatchCommand*>& patches,
		patch_place_type place,
		MemoryInterface& ee,
		MemoryInterface& iop);

	extern u32 GetActiveGameDBPatchesCount();
	extern u32 GetActivePatchesCount();
	extern u32 GetActiveCheatsCount();
	extern u32 GetAllActivePatchesCount();

	extern bool IsGloballyToggleablePatch(const PatchInfo& patch_info);

	extern const char* PlaceToString(std::optional<patch_place_type> place);
}
