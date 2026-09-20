// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/AlignedMalloc.h"
#include "common/Console.h"
#include "common/HashCombine.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/ScopedGuard.h"
#include "common/TextureDecompress.h"

#include "Config.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "GS/GSExtra.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "VMManager.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <thread>

#define TEXTURE_FILENAME_FORMAT_STRING "%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_REGION_FORMAT_STRING "%" PRIx64 "-r%ux%u-%08x"
#define TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-r%ux%u-%08x"
#define TEXTURE_FILENAME_OLD_REGION_FORMAT_STRING "%" PRIx64 "-r%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_OLD_REGION_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-r%" PRIx64 "-%08x"
#define TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME "replacements"
#define TEXTURE_DUMP_SUBDIRECTORY_NAME "dumps"

namespace
{
	struct TextureName
	{
		u64 TEX0Hash;
		u64 CLUTHash;
		u32 region_width;
		u32 region_height;

		union
		{
			struct
			{
				u32 TEX0_PSM : 6;
				u32 TEX0_TW : 4;
				u32 TEX0_TH : 4;
				u32 unused0 : 1;
				u32 TEXA_TA0 : 8;
				u32 TEXA_AEM : 1;
				u32 TEXA_TA1 : 8;
			};
			u32 bits;
		};
		u32 miplevel;

		__fi u32 Width() const { return (region_width ? region_width : (1u << TEX0_TW)); }
		__fi u32 Height() const { return (region_height ? region_height : (1u << TEX0_TH)); }
		__fi bool HasPalette() const { return (GSLocalMemory::m_psm[TEX0_PSM].pal > 0); }
		__fi bool HasRegion() const { return (region_width != 0 || region_height != 0); }

		__fi bool operator==(const TextureName& rhs) const { return BitEqual(*this, rhs); }
		__fi bool operator!=(const TextureName& rhs) const { return !BitEqual(*this, rhs); }
		__fi bool operator<(const TextureName& rhs) const { return (std::memcmp(this, &rhs, sizeof(*this)) < 0); }

		__fi void RemoveUnusedBits()
		{
			unused0 = 0;
		}
	};
	static_assert(sizeof(TextureName) == 32, "ReplacementTextureName is expected size");
}

namespace std
{
	template <>
	struct hash<TextureName>
	{
		std::size_t operator()(const TextureName& val) const
		{
			std::size_t h = 0;
			HashCombine(h, val.TEX0Hash, val.CLUTHash,
				static_cast<u64>(val.region_width) | (static_cast<u64>(val.region_height) << 32),
				static_cast<u64>(val.bits) | (static_cast<u64>(val.miplevel) << 32));
			return h;
		}
	};
}

namespace GSTextureReplacements
{
	static TextureName CreateTextureName(const GSTextureCache::HashCacheKey& hash, u32 miplevel);
	static GSTextureCache::HashCacheKey HashCacheKeyFromTextureName(const TextureName& tn);
	static std::optional<TextureName> ParseReplacementName(const std::string& filename);
	static std::string GetGameTextureDirectory();
	static std::string GetDumpFilename(const TextureName& name, u32 level);
	template <GSTexture::Format format>
	std::pair<u8, u8> GetBCAlphaMinMax(ReplacementTexture& rtex);
	static void SetReplacementTextureAlphaMinMax(ReplacementTexture& rtex);
	static std::optional<ReplacementTexture> LoadReplacementTexture(const TextureName& name, const std::string& filename, bool only_base_image);
	static void QueueAsyncReplacementTextureLoad(const TextureName& name, const std::string& filename, bool mipmap, bool cache_only);
	static void PrecacheReplacementTextures();
	static void ClearReplacementTextures();

	static void StartWorkerThread();
	static void StopWorkerThread();
	static void QueueWorkerThreadItem(std::function<void()> fn, bool high_priority);
	static void WorkerThreadEntryPoint();
	static void SyncWorkerThread();
	static void CancelPendingLoadsAndDumps();

	static std::string s_current_serial;

	static std::unordered_set<TextureName> s_dumped_textures;
	static std::mutex s_dumped_textures_mutex;

	static std::unordered_map<TextureName, std::string> s_replacement_texture_filenames;

	static std::unordered_set<TextureName> s_replacement_textures_without_clut_hash;

	static std::unordered_map<TextureName, ReplacementTexture> s_replacement_texture_cache;
	static std::mutex s_replacement_texture_cache_mutex;

	static std::unordered_map<TextureName, bool> s_pending_async_load_textures;

	static std::vector<std::pair<TextureName, bool>> s_async_loaded_textures;

	static std::thread s_worker_thread;
	static std::mutex s_worker_thread_mutex;
	static std::condition_variable s_worker_thread_cv;
	static std::deque<std::pair<std::function<void()>, bool>> s_worker_thread_queue;
	static bool s_worker_thread_running = false;
};

TextureName GSTextureReplacements::CreateTextureName(const GSTextureCache::HashCacheKey& hash, u32 miplevel)
{
	TextureName name;
	name.bits = 0;
	name.TEX0_PSM = hash.TEX0.PSM;
	name.TEX0_TW = hash.TEX0.TW;
	name.TEX0_TH = hash.TEX0.TH;
	name.TEXA_TA0 = hash.TEXA.TA0;
	name.TEXA_AEM = hash.TEXA.AEM;
	name.TEXA_TA1 = hash.TEXA.TA1;
	name.TEX0Hash = hash.TEX0Hash;
	name.CLUTHash = name.HasPalette() ? hash.CLUTHash : 0;
	name.miplevel = miplevel;
	name.region_width = hash.region_width;
	name.region_height = hash.region_height;
	if (GSConfig.ClassicTextureNames)
		name.unused0 = hash.TEX0.TCC;
	return name;
}

GSTextureCache::HashCacheKey GSTextureReplacements::HashCacheKeyFromTextureName(const TextureName& tn)
{
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[tn.TEX0_PSM];
	GSTextureCache::HashCacheKey key = {};
	key.TEX0.PSM = tn.TEX0_PSM;
	key.TEX0.TW = tn.TEX0_TW;
	key.TEX0.TH = tn.TEX0_TH;
	if (psm_s.pal == 0 && psm_s.fmt > 0)
	{
		key.TEXA.TA0 = tn.TEXA_TA0;
		key.TEXA.AEM = tn.TEXA_AEM;
		key.TEXA.TA1 = tn.TEXA_TA1;
	}
	key.TEX0Hash = tn.TEX0Hash;
	key.CLUTHash = tn.HasPalette() ? tn.CLUTHash : 0;
	key.region_width = tn.region_width;
	key.region_height = tn.region_height;
	return key;
}

std::optional<TextureName> GSTextureReplacements::ParseReplacementName(const std::string& filename)
{
	TextureName ret;
	ret.miplevel = 0;

	GSTextureCache::SourceRegion full_region;

	char extension_dot;
	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash,
			&ret.region_width, &ret.region_height, &ret.bits, &extension_dot) == 6 &&
		extension_dot == '.')
	{
		return ret;
	}

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_REGION_FORMAT_STRING "%c", &ret.TEX0Hash,
			&ret.region_width, &ret.region_height, &ret.bits, &extension_dot) == 5 &&
		extension_dot == '.')
	{
		ret.CLUTHash = 0;
		return ret;
	}

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_OLD_REGION_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash,
			&full_region.bits, &ret.bits, &extension_dot) == 5 &&
		extension_dot == '.')
	{
		ret.region_width = static_cast<u32>(full_region.GetWidth());
		ret.region_height = static_cast<u32>(full_region.GetHeight());
		return ret;
	}

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_OLD_REGION_FORMAT_STRING "%c", &ret.TEX0Hash, &full_region.bits,
			&ret.bits, &extension_dot) == 4 &&
		extension_dot == '.')
	{
		ret.CLUTHash = 0;
		ret.region_width = static_cast<u32>(full_region.GetWidth());
		ret.region_height = static_cast<u32>(full_region.GetHeight());
		return ret;
	}

	ret.region_width = 0;
	ret.region_height = 0;

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash, &ret.bits,
			&extension_dot) == 4 &&
		extension_dot == '.')
	{
		return ret;
	}

	if (std::sscanf(filename.c_str(), TEXTURE_FILENAME_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.bits, &extension_dot) ==
			3 &&
		extension_dot == '.')
	{
		ret.CLUTHash = 0;
		return ret;
	}

	return std::nullopt;
}

std::string GSTextureReplacements::GetGameTextureDirectory()
{
	return Path::Combine(EmuFolders::Textures, s_current_serial);
}

std::string GSTextureReplacements::GetDumpFilename(const TextureName& name, u32 level)
{
	std::string ret;
	if (s_current_serial.empty())
		return ret;

	const std::string game_dir(GetGameTextureDirectory());
	const std::string game_subdir(Path::Combine(game_dir, TEXTURE_DUMP_SUBDIRECTORY_NAME));

	if (!FileSystem::DirectoryExists(game_subdir.c_str()))
	{
		if (!FileSystem::CreateDirectoryPath(game_dir.c_str(), false) ||
			!FileSystem::EnsureDirectoryExists(game_subdir.c_str(), false) ||
			!FileSystem::EnsureDirectoryExists(Path::Combine(game_dir, TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME).c_str(), false))
		{
			return ret;
		}
	}

	std::string filename;
	if (name.HasRegion())
	{
		if (name.HasPalette())
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "-mip%u.png",
					name.TEX0Hash, name.CLUTHash, name.region_width, name.region_height, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING ".png",
					name.TEX0Hash, name.CLUTHash, name.region_width, name.region_height, name.bits);
		}
		else
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_FORMAT_STRING "-mip%u.png",
					name.TEX0Hash, name.region_width, name.region_height, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_FORMAT_STRING ".png",
					name.TEX0Hash, name.region_width, name.region_height, name.bits);
		}
	}
	else
	{
		if (name.HasPalette())
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_CLUT_FORMAT_STRING "-mip%u.png",
				                                  name.TEX0Hash, name.CLUTHash, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_CLUT_FORMAT_STRING ".png",
				                                  name.TEX0Hash, name.CLUTHash, name.bits);
		}
		else
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_FORMAT_STRING "-mip%u.png",
				                                  name.TEX0Hash, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_FORMAT_STRING ".png",
				                                  name.TEX0Hash, name.bits);
		}
	}

	ret = Path::Combine(game_subdir, filename);

	return ret;
}

void GSTextureReplacements::Initialize()
{
	s_current_serial = VMManager::GetDiscSerial();

	if (GSConfig.DumpReplaceableTextures || GSConfig.LoadTextureReplacements)
		StartWorkerThread();

	ReloadReplacementMap();
}

void GSTextureReplacements::GameChanged()
{
	std::string new_serial = VMManager::GetDiscSerial();
	if (s_current_serial == new_serial)
		return;

	s_current_serial = std::move(new_serial);
	ReloadReplacementMap();
	ClearDumpedTextureList();
}

static bool GetWrongCasePath(std::string* output, const char* dir, std::string_view file, FileSystem::FindResultsArray* reuseme)
{
	if (FileSystem::FindFiles(dir, "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, reuseme))
	{
		for (const FILESYSTEM_FIND_DATA& fd : *reuseme)
		{
			std::string_view name = Path::GetFileName(fd.FileName);
			if (name.size() != file.size())
				continue;
			if (0 == strncmp(name.data(), file.data(), name.size()))
				continue;
			if (0 == StringUtil::Strncasecmp(name.data(), file.data(), name.size()))
			{
				*output = fd.FileName;
				return true;
			}
		}
	}
	return false;
}

void GSTextureReplacements::ReloadReplacementMap()
{
	SyncWorkerThread();

	{
		s_replacement_texture_filenames.clear();
		s_replacement_textures_without_clut_hash.clear();

		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		s_replacement_texture_cache.clear();
		s_pending_async_load_textures.clear();
		s_async_loaded_textures.clear();
	}

	if (s_current_serial.empty() || !GSConfig.LoadTextureReplacements)
		return;

	const std::string texture_dir = GetGameTextureDirectory();
	const std::string replacement_dir(Path::Combine(texture_dir, TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME));

	FileSystem::FindResultsArray files;

	std::string wrong_case_path;
	const std::string* right_case_path = nullptr;
	if (GetWrongCasePath(&wrong_case_path, EmuFolders::Textures.c_str(), s_current_serial, &files))
		right_case_path = &texture_dir;
	else if (GetWrongCasePath(&wrong_case_path, texture_dir.c_str(), TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME, &files))
		right_case_path = &replacement_dir;
	if (right_case_path)
	{
		Host::AddKeyedOSDMessage("TextureReplacementDirCaseMismatch",
			fmt::format(TRANSLATE_FS("TextureReplacement", "Texture replacement directory {} will not work on case sensitive filesystems.\n"
			                                               "Rename it to {} to remove this warning."),
			            wrong_case_path, *right_case_path),
			Host::OSD_WARNING_DURATION);
	}

	if (!FileSystem::FindFiles(replacement_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE, &files))
		return;

	std::string filename;
	for (FILESYSTEM_FIND_DATA& fd : files)
	{
		filename = Path::GetFileName(fd.FileName);
		if (!GetLoader(filename))
			continue;

		std::optional<TextureName> name = ParseReplacementName(filename);
		if (!name.has_value())
			continue;

		const bool tcc_alias = GSConfig.ClassicTextureNames && (name->unused0 != 0);
		TextureName canonical = name.value();
		canonical.RemoveUnusedBits();

		DbgCon.WriteLn("Found %ux%u replacement '%.*s'", canonical.Width(), canonical.Height(), static_cast<int>(filename.size()), filename.data());
		if (tcc_alias)
			s_replacement_texture_filenames.emplace(name.value(), fd.FileName);
		s_replacement_texture_filenames.emplace(canonical, std::move(fd.FileName));

		canonical.CLUTHash = 0;
		s_replacement_textures_without_clut_hash.insert(canonical);
		if (tcc_alias)
		{
			name->CLUTHash = 0;
			s_replacement_textures_without_clut_hash.insert(name.value());
		}
	}

	if (!s_replacement_texture_filenames.empty())
	{
		if (GSConfig.PrecacheTextureReplacements)
			PrecacheReplacementTextures();

		if (GSConfig.GPUPaletteConversion && GSConfig.TexturePreloading != TexturePreloadingLevel::Full)
		{
			Console.Warning("Replacement textures were found, and GPU palette conversion is enabled without full preloading.");
			Console.Warning("Palette textures will be disabled. Please enable full preloading or disable GPU palette conversion.");
		}
	}
}

void GSTextureReplacements::UpdateConfig(Pcsx2Config::GSOptions& old_config)
{
	if (s_worker_thread_running && !GSConfig.DumpReplaceableTextures && !GSConfig.LoadTextureReplacements)
		StopWorkerThread();
	if (!s_worker_thread_running && (GSConfig.DumpReplaceableTextures || GSConfig.LoadTextureReplacements))
		StartWorkerThread();

	if ((!GSConfig.DumpReplaceableTextures && old_config.DumpReplaceableTextures) ||
		(!GSConfig.LoadTextureReplacements && old_config.LoadTextureReplacements))
	{
		CancelPendingLoadsAndDumps();
	}

	if (GSConfig.LoadTextureReplacements && !old_config.LoadTextureReplacements)
		ReloadReplacementMap();
	else if (!GSConfig.LoadTextureReplacements && old_config.LoadTextureReplacements)
		ClearReplacementTextures();

	if (!GSConfig.DumpReplaceableTextures && old_config.DumpReplaceableTextures)
		ClearDumpedTextureList();

	if (GSConfig.LoadTextureReplacements && GSConfig.PrecacheTextureReplacements && !old_config.PrecacheTextureReplacements)
		PrecacheReplacementTextures();

	if (GSConfig.ClassicTextureNames != old_config.ClassicTextureNames)
	{
		CancelPendingLoadsAndDumps();
		ClearDumpedTextureList();
		if (GSConfig.LoadTextureReplacements)
			ReloadReplacementMap();
	}
}

void GSTextureReplacements::Shutdown()
{
	StopWorkerThread();

	std::string().swap(s_current_serial);
	ClearReplacementTextures();
	ClearDumpedTextureList();
}

u32 GSTextureReplacements::CalcMipmapLevelsForReplacement(u32 width, u32 height)
{
	return static_cast<u32>(std::log2(std::max(width, height))) + 1u;
}

bool GSTextureReplacements::HasAnyReplacementTextures()
{
	return !s_replacement_texture_filenames.empty();
}

bool GSTextureReplacements::HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey& hash)
{
	const TextureName name(CreateTextureName(hash.WithRemovedCLUTHash(), 0));
	return s_replacement_textures_without_clut_hash.find(name) != s_replacement_textures_without_clut_hash.end();
}

GSTexture* GSTextureReplacements::LookupReplacementTexture(const GSTextureCache::HashCacheKey& hash, bool mipmap,
	bool* pending, std::pair<u8, u8>* alpha_minmax, bool force_sync,
	GSTextureCache::SourceRegion classic_region, u32 base_width, u32 base_height)
{
	const TextureName name(CreateTextureName(hash, 0));
	*pending = false;

	const auto classic_crop = [&](const ReplacementTexture& rtex) -> GSVector4i {
		if (!classic_region.HasEither() || base_width == 0 || base_height == 0)
			return GSVector4i::zero();
		if (rtex.format != GSTexture::Format::Color)
		{
			static bool warned = false;
			if (!warned)
			{
				Console.Warning("Classic Dump: compressed replacement for a region-clamped "
								"texture cannot be region-cropped; expect atlas artifacts.");
				warned = true;
			}
			return GSVector4i::zero();
		}
		const GSVector4i rrect = classic_region.GetRect(static_cast<int>(base_width), static_cast<int>(base_height));
		const float sx = static_cast<float>(rtex.width) / static_cast<float>(base_width);
		const float sy = static_cast<float>(rtex.height) / static_cast<float>(base_height);
		GSVector4i crop(static_cast<int>(rrect.x * sx + 0.5f), static_cast<int>(rrect.y * sy + 0.5f),
			static_cast<int>(rrect.z * sx + 0.5f), static_cast<int>(rrect.w * sy + 0.5f));
		crop = crop.max_i32(GSVector4i::zero());
		crop = crop.min_i32(GSVector4i(static_cast<int>(rtex.width), static_cast<int>(rtex.height),
			static_cast<int>(rtex.width), static_cast<int>(rtex.height)));
		if (crop.rempty() || crop.eq(GSVector4i(0, 0, static_cast<int>(rtex.width), static_cast<int>(rtex.height))))
			return GSVector4i::zero();
		return crop;
	};

	auto fnit = s_replacement_texture_filenames.find(name);
	if (fnit == s_replacement_texture_filenames.end())
		return nullptr;

	{
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		auto it = s_replacement_texture_cache.find(name);
		if (it != s_replacement_texture_cache.end())
		{
			*alpha_minmax = it->second.alpha_minmax;
			return CreateReplacementTexture(it->second, mipmap, classic_crop(it->second));
		}
	}

	if (GSConfig.LoadTextureReplacementsAsync && !force_sync)
	{
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		QueueAsyncReplacementTextureLoad(name, fnit->second, mipmap, false);

		*pending = true;
		return nullptr;
	}
	else
	{
		std::optional<ReplacementTexture> replacement(LoadReplacementTexture(name, fnit->second, !mipmap));
		if (!replacement.has_value())
			return nullptr;

		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		const ReplacementTexture& rtex = s_replacement_texture_cache.emplace(name, std::move(replacement.value())).first->second;

		*alpha_minmax = rtex.alpha_minmax;
		return CreateReplacementTexture(rtex, mipmap, classic_crop(rtex));
	}
}

template <GSTexture::Format format>
std::pair<u8, u8> GSTextureReplacements::GetBCAlphaMinMax(ReplacementTexture& rtex)
{
	constexpr u32 BC_BLOCK_SIZE = 4;
	constexpr u32 BC_BLOCK_BYTES = (format == GSTexture::Format::BC1) ? 8 : 16;

	const u32 blocks_wide = (rtex.width + (BC_BLOCK_SIZE - 1)) / BC_BLOCK_SIZE;
	const u32 blocks_high = (rtex.height + (BC_BLOCK_SIZE - 1)) / BC_BLOCK_SIZE;

	GSVector4i minc = GSVector4i::xffffffff();
	GSVector4i maxc = GSVector4i::zero();

	for (u32 y = 0; y < blocks_high; y++)
	{
		const u8* block_in = rtex.data.data() + y * rtex.pitch;
		alignas(16) u8 block_pixels_out[BC_BLOCK_SIZE * BC_BLOCK_SIZE * sizeof(u32)];

		for (u32 x = 0; x < blocks_wide; x++, block_in += BC_BLOCK_BYTES)
		{
			switch (format)
			{
				case GSTexture::Format::BC1:
					DecompressBlockBC1(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;
				case GSTexture::Format::BC2:
					DecompressBlockBC2(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;
				case GSTexture::Format::BC3:
					DecompressBlockBC3(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;

				case GSTexture::Format::BC7:
					bc7decomp::unpack_bc7(block_in, reinterpret_cast<bc7decomp::color_rgba*>(block_pixels_out));
					break;
			}

			const u8* out_ptr = block_pixels_out;
			for (u32 i = 0; i < ((BC_BLOCK_SIZE * BC_BLOCK_SIZE * sizeof(u32)) / sizeof(GSVector4i)); i++)
			{
				const GSVector4i v = GSVector4i::load<true>(out_ptr);
				out_ptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}
		}
	}

	return std::make_pair<u8, u8>(static_cast<u8>(minc.minv_u32() >> 24), static_cast<u8>(maxc.maxv_u32() >> 24));
}

void GSTextureReplacements::SetReplacementTextureAlphaMinMax(ReplacementTexture& rtex)
{
	switch (rtex.format)
	{
		case GSTexture::Format::BC1:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC1>(rtex);
			break;

		case GSTexture::Format::BC2:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC2>(rtex);
			break;

		case GSTexture::Format::BC3:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC3>(rtex);
			break;

		case GSTexture::Format::BC7:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC7>(rtex);
			break;

		default:
			pxAssert(rtex.format == GSTexture::Format::Color);
			rtex.alpha_minmax = GSGetRGBA8AlphaMinMax(rtex.data.data(), rtex.width, rtex.height, rtex.pitch);
			break;
	}
}

std::optional<GSTextureReplacements::ReplacementTexture> GSTextureReplacements::LoadReplacementTexture(const TextureName& name, const std::string& filename, bool only_base_image)
{
	ReplacementTextureLoader loader = GetLoader(filename);
	if (!loader)
		return std::nullopt;

	ReplacementTexture rtex;
	if (!loader(filename.c_str(), &rtex, only_base_image))
	{
		Console.Warning("Failed to load replacement texture %s", filename.c_str());
		return std::nullopt;
	}

	SetReplacementTextureAlphaMinMax(rtex);

	return rtex;
}

void GSTextureReplacements::QueueAsyncReplacementTextureLoad(const TextureName& name, const std::string& filename, bool mipmap, bool cache_only)
{
	auto it = s_pending_async_load_textures.find(name);
	if (it != s_pending_async_load_textures.end())
	{
		if (!cache_only && it->second)
		{
			s_pending_async_load_textures.erase(it);
		}
		else
		{
			it->second &= cache_only;
			return;
		}
	}

	s_pending_async_load_textures.emplace(name, cache_only);
	QueueWorkerThreadItem([name, filename, mipmap]() {
		std::optional<ReplacementTexture> replacement(LoadReplacementTexture(name, filename, !mipmap));

		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		auto it = s_pending_async_load_textures.find(name);
		if (it == s_pending_async_load_textures.end() ||
			s_replacement_texture_cache.find(name) != s_replacement_texture_cache.end())
		{
			if (it != s_pending_async_load_textures.end())
				s_pending_async_load_textures.erase(it);

			return;
		}

		if (replacement.has_value())
		{
			s_replacement_texture_cache.emplace(name, std::move(replacement.value()));
			s_async_loaded_textures.emplace_back(name, mipmap);
		}
		else
		{
			s_pending_async_load_textures.erase(name);
		}
	}, !cache_only);
}

void GSTextureReplacements::PrecacheReplacementTextures()
{
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);

	const bool mipmap = GSConfig.HWMipmap || GSConfig.TriFilter == TriFiltering::Forced;

	for (const auto& it : s_replacement_texture_filenames)
	{
		if (s_replacement_texture_cache.find(it.first) != s_replacement_texture_cache.end())
			continue;

		QueueAsyncReplacementTextureLoad(it.first, it.second, mipmap, true);
	}
}

void GSTextureReplacements::ClearReplacementTextures()
{
	s_replacement_texture_filenames.clear();
	s_replacement_textures_without_clut_hash.clear();

	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	s_replacement_texture_cache.clear();
	s_pending_async_load_textures.clear();
	s_async_loaded_textures.clear();
}

GSTexture* GSTextureReplacements::CreateReplacementTexture(const ReplacementTexture& rtex, bool mipmap, const GSVector4i& crop)
{
	if (!crop.rempty())
	{
		const int cw = (crop.x >= 0 && crop.x < static_cast<int>(rtex.width))
		                   ? std::min(crop.width(), static_cast<int>(rtex.width) - crop.x)
		                   : 0;
		const int ch = (crop.y >= 0 && crop.y < static_cast<int>(rtex.height))
		                   ? std::min(crop.height(), static_cast<int>(rtex.height) - crop.y)
		                   : 0;
		const size_t row_bytes = static_cast<size_t>(cw) * 4;
		bool ok = cw > 0 && ch > 0 && row_bytes <= rtex.pitch;
		std::vector<u8> cropped;
		if (ok)
		{
			cropped.resize(row_bytes * static_cast<size_t>(ch));
			for (int row = 0; row < ch && ok; row++)
			{
				const size_t src_off = (static_cast<size_t>(crop.y) + static_cast<size_t>(row)) *
				                            static_cast<size_t>(rtex.pitch) +
				                        static_cast<size_t>(crop.x) * 4;
				if (src_off + row_bytes > rtex.data.size())
				{
					ok = false;
					break;
				}
				std::memcpy(cropped.data() + static_cast<size_t>(row) * row_bytes, rtex.data.data() + src_off, row_bytes);
			}
		}
		if (ok)
		{
			GSTexture* ctex = g_gs_device->CreateTexture(cw, ch, 1, rtex.format);
			if (!ctex)
				return nullptr;
			ctex->Update(GSVector4i(0, 0, cw, ch), cropped.data(), static_cast<int>(row_bytes));
			return ctex;
		}
		static bool warned = false;
		if (!warned)
		{
			Console.Warning("Classic Dump: computed crop rect (%d,%d %dx%d, clamped %dx%d) does not fit "
							"a %ux%u replacement (pitch %u) — injecting uncropped instead of risking an "
							"out-of-bounds read. Expect an atlas artifact on this texture; please "
							"report it.",
				crop.x, crop.y, crop.width(), crop.height(), cw, ch, rtex.width, rtex.height, rtex.pitch);
			warned = true;
		}
	}

	if (mipmap && GSTexture::IsCompressedFormat(rtex.format) && rtex.mips.empty())
	{
		static bool log_once = false;
		if (!log_once)
		{
			Console.Warning("Disabling autogenerated mipmaps on one or more compressed replacement textures.");
			Host::AddIconOSDMessage("DisablingReplacementAutoGeneratedMipmap", ICON_FA_CIRCLE_EXCLAMATION,
				TRANSLATE_SV("GS", "Disabling autogenerated mipmaps on one or more compressed replacement textures. "
								   "Please generate mipmaps when compressing your textures."),
				Host::OSD_WARNING_DURATION);
			log_once = true;
		}

		mipmap = false;
	}

	GSTexture* tex = g_gs_device->CreateTexture(rtex.width, rtex.height, static_cast<int>(rtex.mips.size()) + 1, rtex.format);
	if (!tex)
		return nullptr;

	tex->Update(GSVector4i(0, 0, rtex.width, rtex.height), rtex.data.data(), rtex.pitch);

	if (!rtex.mips.empty())
	{
		for (u32 i = 0; i < static_cast<u32>(rtex.mips.size()); i++)
		{
			const ReplacementTexture::MipData& mip = rtex.mips[i];
			tex->Update(GSVector4i(0, 0, static_cast<int>(mip.width), static_cast<int>(mip.height)), mip.data.data(), mip.pitch, i + 1);
		}
	}

	return tex;
}

void GSTextureReplacements::ProcessAsyncLoadedTextures()
{
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	for (const auto& [name, mipmap] : s_async_loaded_textures)
	{
		const auto pit = s_pending_async_load_textures.find(name);
		if (pit != s_pending_async_load_textures.end())
		{
			const bool cache_only = pit->second;
			s_pending_async_load_textures.erase(pit);

			if (cache_only)
				continue;
		}

		auto it = s_replacement_texture_cache.find(name);
		if (it == s_replacement_texture_cache.end())
			continue;

		GSTexture* tex = CreateReplacementTexture(it->second, mipmap);
		if (tex)
			g_texture_cache->InjectHashCacheTexture(HashCacheKeyFromTextureName(name), tex, it->second.alpha_minmax);
	}
	s_async_loaded_textures.clear();
}

void GSTextureReplacements::DumpTexture(const GSTextureCache::HashCacheKey& hash, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, GSLocalMemory& mem, u32 level)
{
	const TextureName name(CreateTextureName(hash, level));
	{
		std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
		if (s_dumped_textures.find(name) != s_dumped_textures.end() || s_replacement_texture_filenames.find(name) != s_replacement_texture_filenames.end())
			return;

		s_dumped_textures.insert(name);
	}

	std::string filename(GetDumpFilename(name, level));
	if (filename.empty() || FileSystem::FileExists(filename.c_str()))
		return;

	const std::string_view title(Path::GetFileTitle(filename));
	DevCon.WriteLn("Dumping %ux%u texture '%.*s'.", name.Width(), name.Height(), static_cast<int>(title.size()), title.data());

	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	const GSVector2i& bs = psm.bs;
	const int tw = region.HasX() ? region.GetWidth() : (1 << TEX0.TW);
	const int th = region.HasY() ? region.GetHeight() : (1 << TEX0.TH);
	const GSVector4i rect(region.GetRect(tw, th));
	const GSVector4i block_rect(rect.ralign<Align_Outside>(bs));
	const int read_width = block_rect.width();
	const int read_height = block_rect.height();
	const u32 pitch = static_cast<u32>(read_width) * sizeof(u32);

	u8* buffer = static_cast<u8*>(_aligned_malloc(pitch * static_cast<u32>(read_height), 32));
	psm.rtx(mem, mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM), block_rect, buffer, pitch, TEXA);

	const u32 buffer_offset = ((rect.top - block_rect.top) * pitch) + ((rect.left - block_rect.left) * sizeof(u32));
	QueueWorkerThreadItem([filename = std::move(filename), tw, th, pitch, buffer, buffer_offset]() {
		if (!SavePNGImage(filename.c_str(), tw, th, buffer + buffer_offset, pitch))
			Console.Error(fmt::format("Failed to dump texture to '{}'.", filename));
		_aligned_free(buffer);
	}, false);
}

void GSTextureReplacements::ClearDumpedTextureList()
{
	std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
	s_dumped_textures.clear();
}

u32 GSTextureReplacements::GetDumpedTextureCount()
{
	std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
	return static_cast<u32>(s_dumped_textures.size());
}

u32 GSTextureReplacements::GetLoadedTextureCount()
{
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	return static_cast<u32>(s_replacement_texture_cache.size());
}

void GSTextureReplacements::StartWorkerThread()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);

	if (s_worker_thread.joinable())
		return;

	s_worker_thread_running = true;
	s_worker_thread = std::thread(WorkerThreadEntryPoint);
}

void GSTextureReplacements::StopWorkerThread()
{
	{
		std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
		if (!s_worker_thread.joinable())
			return;
	}

	SyncWorkerThread();

	{
		std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
		s_worker_thread_running = false;
		s_worker_thread_cv.notify_one();
	}

	s_worker_thread.join();

	CancelPendingLoadsAndDumps();
}

void GSTextureReplacements::QueueWorkerThreadItem(std::function<void()> fn, bool high_priority)
{
	pxAssert(s_worker_thread.joinable());

	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	if (!high_priority)
	{
		s_worker_thread_queue.emplace_back(std::move(fn), false);
	}
	else
	{
		auto iter = s_worker_thread_queue.rbegin();
		for (; iter != s_worker_thread_queue.rend(); ++iter)
		{
			if (iter->second)
			{
				break;
			}
		}

		if (iter != s_worker_thread_queue.rend())
		{
			s_worker_thread_queue.insert(iter.base(), std::make_pair(std::move(fn), true));
		}
		else
		{
			s_worker_thread_queue.emplace_front(std::move(fn), true);
		}
	}

	s_worker_thread_cv.notify_one();
}

void GSTextureReplacements::WorkerThreadEntryPoint()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	while (s_worker_thread_running)
	{
		if (s_worker_thread_queue.empty())
		{
			s_worker_thread_cv.wait(lock);
			continue;
		}

		std::function<void()> fn = std::move(s_worker_thread_queue.front().first);
		s_worker_thread_queue.pop_front();
		lock.unlock();
		fn();
		lock.lock();
	}
}

void GSTextureReplacements::SyncWorkerThread()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	if (!s_worker_thread.joinable())
		return;

	for (;;)
	{
		if (s_worker_thread_queue.empty())
			break;

		lock.unlock();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		lock.lock();
	}
}

void GSTextureReplacements::CancelPendingLoadsAndDumps()
{
	SyncWorkerThread();

	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	s_async_loaded_textures.clear();
	s_pending_async_load_textures.clear();
}
