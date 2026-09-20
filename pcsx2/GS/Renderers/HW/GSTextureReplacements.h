// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/HW/GSTextureCache.h"

#include <utility>

namespace GSTextureReplacements
{
	struct ReplacementTexture
	{
		u32 width;
		u32 height;
		GSTexture::Format format;
		std::pair<u8, u8> alpha_minmax;

		u32 pitch;
		std::vector<u8> data;

		struct MipData
		{
			u32 width;
			u32 height;
			u32 pitch;
			std::vector<u8> data;
		};
		std::vector<MipData> mips;
	};

	void Initialize();
	void GameChanged();
	void ReloadReplacementMap();
	void UpdateConfig(Pcsx2Config::GSOptions& old_config);
	void Shutdown();

	u32 CalcMipmapLevelsForReplacement(u32 width, u32 height);

	bool HasAnyReplacementTextures();
	bool HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey& hash);
	GSTexture* LookupReplacementTexture(const GSTextureCache::HashCacheKey& hash, bool mipmap, bool* pending, std::pair<u8, u8>* alpha_minmax, bool force_sync = false,
		GSTextureCache::SourceRegion classic_region = {}, u32 base_width = 0, u32 base_height = 0);
	GSTexture* CreateReplacementTexture(const ReplacementTexture& rtex, bool mipmap, const GSVector4i& crop = GSVector4i::zero());
	void ProcessAsyncLoadedTextures();

	void DumpTexture(const GSTextureCache::HashCacheKey& hash, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA,
		GSTextureCache::SourceRegion region, GSLocalMemory& mem, u32 level);
	void ClearDumpedTextureList();

	u32 GetDumpedTextureCount();

	u32 GetLoadedTextureCount();

	using ReplacementTextureLoader = bool (*)(const std::string& filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image);
	ReplacementTextureLoader GetLoader(const std::string_view filename);

	bool SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch);
}
