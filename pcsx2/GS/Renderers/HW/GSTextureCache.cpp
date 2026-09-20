// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSTextureCache.h"
#include "GSTextureReplacements.h"
#include "GSRendererHW.h"
#include "GS/GSState.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "GS/GSXXH.h"

#ifdef ENABLE_VR
#include "VR/StereoState.h"
#endif

#include "common/Console.h"
#include "common/BitUtils.h"
#include "common/HashCombine.h"
#include "common/SmallString.h"

#include "fmt/format.h"

#include <cinttypes>
#include <math.h>

#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#include <cstdlib>
#endif

std::unique_ptr<GSTextureCache> g_texture_cache;

static u8* s_unswizzle_buffer;

static std::vector<std::pair<GSTextureCache::HashCacheMap::iterator, s32>> s_hash_cache_purge_list;

#ifdef PCSX2_DEVBUILD
__fi static bool PreferReusedLabelledTexture()
{
	return !GSConfig.UseDebugDevice;
}
#else
__fi static constexpr bool PreferReusedLabelledTexture()
{
	return true;
}
#endif

GSTextureCache::GSTextureCache()
{
	s_unswizzle_buffer = (u8*)_aligned_malloc(9 * 1024 * 1024, VECTOR_ALIGNMENT);
	pxAssertRel(s_unswizzle_buffer, "Failed to allocate unswizzle buffer");

	m_surface_offset_cache.reserve(S_SURFACE_OFFSET_CACHE_MAX_SIZE);
}

GSTextureCache::~GSTextureCache()
{
	RemoveAll(true, true, true);

	s_hash_cache_purge_list = {};
	_aligned_free(s_unswizzle_buffer);
}

void GSTextureCache::ReadbackAll()
{
	for (int type = 0; type < 2; type++)
	{
		for (auto t : m_dst[type])
			Read(t, t->m_drawn_since_read);
	}
}

void GSTextureCache::RemoveAll(bool sources, bool targets, bool hash_cache)
{
	InvalidateTemporaryZ();

#ifdef ENABLE_VR
	if (targets)
		m_vr_display_bps.clear();
#endif

	if (sources || targets)
	{
		m_src.RemoveAll();
		m_palette_map.Clear();
		m_source_memory_usage = 0;
	}

	if (targets)
	{
		for (int type = 0; type < 2; type++)
		{
			for (auto t : m_dst[type])
				delete t;

			m_dst[type].clear();
		}

		m_target_heights.clear();
		m_surface_offset_cache.clear();
		m_target_memory_usage = 0;
	}

	if (hash_cache)
	{
		for (auto it : m_hash_cache)
			g_gs_device->Recycle(it.second.texture);

		m_hash_cache.clear();
		m_hash_cache_memory_usage = 0;
		m_hash_cache_replacement_memory_usage = 0;
	}
}

bool GSTextureCache::FullRectDirty(Target* target, u32 rgba_mask)
{
	if (target->m_dirty.size() == 1 && (rgba_mask & target->m_dirty[0].rgba._u32) == rgba_mask && target->m_valid.rintersect(target->m_dirty[0].r).eq(target->m_valid))
	{
		if (rgba_mask == 0x8 && target->m_TEX0.PSM == PSMCT32)
		{
			target->m_valid_alpha_high = false;
			target->m_valid_alpha_low = false;
			return false;
		}
		return true;
	}

	if (target->m_was_dst_matched && target->m_dirty.size() == 1 && target->m_drawn_since_read.rintersect(target->m_dirty[0].r).eq(target->m_drawn_since_read))
	{
		return true;
	}

	return false;
}

bool GSTextureCache::FullRectDirty(Target* target)
{
	return FullRectDirty(target, GSUtil::GetChannelMask(target->m_TEX0.PSM));
}

void GSTextureCache::AddDirtyRectTarget(Target* target, GSVector4i rect, u32 psm, u32 bw, RGBAMask rgba, bool req_linear)
{
	bool skipdirty = false;
	bool canskip = true;

	if (rect.rempty())
		return;

	std::vector<GSDirtyRect>::iterator it = target->m_dirty.end();
	while (it != target->m_dirty.begin())
	{
		--it;
		if (it[0].bw == bw && it[0].psm == psm && it[0].rgba._u32 == rgba._u32)
		{
			if (it[0].r.rintersect(rect).eq(rect) && canskip)
			{
				skipdirty = true;
				break;
			}
			const GSVector4i new_dirty_rect = rect.rintersect(target->m_valid);
			const GSVector4i existing_dirty_rect = it[0].r.rintersect(target->m_valid);
			const int dirty_overlap_top = existing_dirty_rect.wwww().ge32(new_dirty_rect.yyyy()).mask() & existing_dirty_rect.yyyy().lt32(new_dirty_rect.yyyy()).mask();
			const int dirty_overlap_bottom = existing_dirty_rect.yyyy().le32(new_dirty_rect.wwww()).mask() & existing_dirty_rect.wwww().gt32(new_dirty_rect.wwww()).mask();
			const int dirty_overlap_left = existing_dirty_rect.zzzz().ge32(new_dirty_rect.xxxx()).mask() & existing_dirty_rect.xxxx().lt32(new_dirty_rect.xxxx()).mask();
			const int dirty_overlap_right = existing_dirty_rect.xxxx().le32(new_dirty_rect.zzzz()).mask() & existing_dirty_rect.zzzz().gt32(new_dirty_rect.zzzz()).mask();
			if ((existing_dirty_rect.xzxz().eq(new_dirty_rect.xzxz()) && (dirty_overlap_top == 0xFFFF || dirty_overlap_bottom == 0xFFFF)) ||
				(existing_dirty_rect.ywyw().eq(new_dirty_rect.ywyw()) && (dirty_overlap_left == 0xFFFF || dirty_overlap_right == 0xFFFF)) ||
				existing_dirty_rect.rintersect(new_dirty_rect).eq(existing_dirty_rect))
			{
				rect = rect.runion(it[0].r);
				it = target->m_dirty.erase(it);
				canskip = false;
				continue;
			}
		}
	}

	if (!skipdirty)
	{
		GL_INS("TC: Dirty rect added for BP %x rect %d,%d->%d->%d", target->m_TEX0.TBP0, rect.x, rect.y, rect.z, rect.w);
		target->m_dirty.push_back(GSDirtyRect(rect, psm, bw, rgba, req_linear));

		if (!target->m_drawn_since_read.rempty())
		{
			if (target->m_dirty[0].rgba._u32 == GSUtil::GetChannelMask(target->m_TEX0.PSM) && target->m_drawn_since_read.rintersect(target->m_dirty.GetTotalRect(target->m_TEX0, target->m_unscaled_size)).eq(target->m_drawn_since_read))
			{
				if (target->m_dirty.size() > 1 && target->m_age == 0)
					return;

				target->m_drawn_since_read = GSVector4i::zero();
			}
		}
	}
}

void GSTextureCache::ResizeTarget(Target* t, GSVector4i rect, u32 tbp, u32 psm, u32 tbw)
{
	if (t->m_valid.z < t->m_unscaled_size.x || t->m_valid.w < t->m_unscaled_size.y)
		return;

	const GSVector2i size_delta = {std::max(0, (rect.z - t->m_valid.z)), std::max(0, (rect.w - t->m_valid.w))};
	if (size_delta.x > 1 || size_delta.y > 1)
	{
		RGBAMask rgba;
		rgba._u32 = GSUtil::GetChannelMask(t->m_TEX0.PSM);
		AddDirtyRectTarget(t, GSVector4i(t->m_valid.x, t->m_valid.w, t->m_valid.z + std::max(0, size_delta.x), t->m_valid.w + std::max(0, size_delta.y)), t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);
		AddDirtyRectTarget(t, GSVector4i(t->m_valid.z, t->m_valid.y, t->m_valid.z + std::max(0, size_delta.x), t->m_valid.w), t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);
		const GSVector4i valid_rect = {t->m_valid.x, t->m_valid.y, t->m_valid.z + std::max(0, size_delta.x), t->m_valid.w + std::max(0, size_delta.y)};
		t->UpdateValidity(valid_rect, size_delta.x > 2 || size_delta.y > 2);
		GetTargetSize(tbp, tbw, psm, valid_rect.z, valid_rect.w);
		const int new_w = std::max(t->m_unscaled_size.x, valid_rect.z);
		const int new_h = std::max(t->m_unscaled_size.y, valid_rect.w);
		t->ResizeTexture(new_w, new_h);
	}
}


bool GSTextureCache::CanTranslate(u32 bp, u32 bw, u32 spsm, GSVector4i r, u32 dbp, u32 dpsm, u32 dbw)
{
	const GSVector2i src_page_size = GSLocalMemory::m_psm[spsm].pgs;
	const GSVector2i dst_page_size = GSLocalMemory::m_psm[dpsm].pgs;
	const u32 src_bw = std::max(1U, bw);
	const u32 dst_bw = std::max(1U, dbw);
	const bool block_layout_match = GSLocalMemory::m_psm[spsm].bpp == GSLocalMemory::m_psm[dpsm].bpp;
	const bool bp_page_aligned_bp = ((bp & ~((1 << 5) - 1)) == bp) || bp == dbp || (block_layout_match && src_bw == dst_bw);
	const GSVector4i page_mask(GSVector4i((src_page_size.x - 1), (src_page_size.y - 1)).xyxy());
	const GSVector4i masked_rect(r & ~page_mask);
	const int src_pixel_width = src_bw * 64;
	const int dst_pixel_width = dst_bw * 64;
	const bool page_aligned_rect = masked_rect.xyxy().eq(r.xyxy());
	const bool width_match = (src_pixel_width / src_page_size.x) == (dst_pixel_width / dst_page_size.x);
	const bool sequential_pages = page_aligned_rect && r.x == 0 && r.z == src_pixel_width;
	const bool single_row = ((src_pixel_width / src_page_size.x) <= (dst_pixel_width / dst_page_size.x)) && r.width() <= src_pixel_width && r.height() <= src_page_size.y;
	const bool single_page_aligned = page_aligned_rect && r.z <= src_page_size.x && r.w <= src_page_size.y;
	if (block_layout_match)
	{
		return bp_page_aligned_bp && (width_match || single_row || single_page_aligned || sequential_pages);
	}
	else
	{
		return bp_page_aligned_bp && page_aligned_rect && (single_row || single_page_aligned || width_match || sequential_pages);
	}
}

GSVector4i GSTextureCache::TranslateAlignedRectByPage(u32 tbp, u32 tebp, u32 tbw, u32 tpsm, GSVector4i t_r, u32 sbp, u32 spsm, u32 sbw, GSVector4i src_r, bool is_invalidation)
{
	const GSVector2i src_page_size = GSLocalMemory::m_psm[spsm].pgs;
	const GSVector2i dst_page_size = GSLocalMemory::m_psm[tpsm].pgs;
	const int clamped_sbw = static_cast<int>(std::max(1U, sbw));
	const int clamped_tbw = static_cast<int>(std::max(1U, tbw));
	const int src_bw = clamped_sbw * 64;
	const int dst_bw = clamped_tbw * 64;
	const GSLocalMemory::psm_t& s_psm = GSLocalMemory::m_psm[spsm];
	const GSLocalMemory::psm_t& t_psm = GSLocalMemory::m_psm[tpsm];
	const int src_pgw = std::max(1, src_bw / src_page_size.x);
	const int dst_pgw = std::max(1, dst_bw / dst_page_size.x);
	GSVector4i in_rect = src_r;

	if (sbp < tebp && tebp < tbp)
		sbp += 0x4000;
	int page_offset = (static_cast<int>(sbp) - static_cast<int>(tbp)) >> 5;
	const int block_offset = (static_cast<int>(sbp) - static_cast<int>(tbp)) & 0x1F;

	if (!(s_psm.bpp == t_psm.bpp) || block_offset)
	{
		if (block_offset)
			in_rect = in_rect.ralign<Align_Outside>(s_psm.bs);
		else
			in_rect = in_rect.ralign<Align_Outside>(s_psm.pgs);

		const GSVector4i in_pages = GSVector4i(in_rect.x / s_psm.pgs.x, in_rect.y / s_psm.pgs.y, in_rect.z / s_psm.pgs.x, in_rect.w / s_psm.pgs.y);
		in_rect -= GSVector4i(in_pages.x * s_psm.pgs.x, in_pages.y * s_psm.pgs.y, in_pages.z * s_psm.pgs.x, in_pages.w * s_psm.pgs.y);
		const GSVector4i in_blocks = GSVector4i(in_rect.x / s_psm.bs.x, in_rect.y / s_psm.bs.y, (in_rect.z + (s_psm.bs.x - 1)) / s_psm.bs.x, (in_rect.w + (s_psm.bs.y - 1)) / s_psm.bs.y);

		in_rect = GSVector4i(in_pages.x * t_psm.pgs.x, in_pages.y * t_psm.pgs.y, in_pages.z * t_psm.pgs.x, in_pages.w * t_psm.pgs.y);
		in_rect += GSVector4i(in_blocks.x * t_psm.bs.x, in_blocks.y * t_psm.bs.y, in_blocks.z * t_psm.bs.x, in_blocks.w * t_psm.bs.y);

		if (in_rect.rempty())
		{
			DevCon.Warning("Error translating rect");
			return GSVector4i::zero();
		}

		const bool block_matched_format = s_psm.bpp == t_psm.bpp && (clamped_sbw == clamped_tbw || clamped_sbw == 1);
		if (block_matched_format)
		{
			GSVector4i b2a_offset = GSVector4i::zero();

			SurfaceOffsetKey sok;
			sok.elems[0].bp = sbp;
			sok.elems[0].bw = sbw;
			sok.elems[0].psm = spsm;
			sok.elems[0].rect = src_r;
			sok.elems[1].bp = tbp;
			sok.elems[1].bw = tbw;
			sok.elems[1].psm = tpsm;
			sok.elems[1].rect = t_r;

			const auto it = m_surface_offset_cache.find(sok);
			if (it != m_surface_offset_cache.end())
			{
				b2a_offset = it->second.b2a_offset;
				in_rect = (in_rect + b2a_offset.xyxy()).max_i32(GSVector4i(0));
			}
			else
			{
				const int y_page_offset = (page_offset / (src_bw / s_psm.pgs.x)) * s_psm.pgs.y;
				const GSVector4i target_rect = GSVector4i(0, y_page_offset, src_bw, 2048);
				bool new_b2a_offset_found = false;
				for (b2a_offset.y = target_rect.y; b2a_offset.y < target_rect.w; b2a_offset.y += s_psm.bs.y)
				{
					for (b2a_offset.x = target_rect.x; b2a_offset.x < target_rect.z; b2a_offset.x += s_psm.bs.x)
					{
						const u32 a_candidate_bp = s_psm.info.bn(b2a_offset.x, b2a_offset.y, tbp, sbw);
						if (sbp == a_candidate_bp)
						{
							new_b2a_offset_found = true;
							break;
						}
					}
					if (new_b2a_offset_found)
						break;
				}

				if (new_b2a_offset_found)
				{
					SurfaceOffset so;
					so.is_valid = true;
					so.b2a_offset = b2a_offset;

					if (m_surface_offset_cache.size() + 1 > S_SURFACE_OFFSET_CACHE_MAX_SIZE)
						m_surface_offset_cache.clear();

					m_surface_offset_cache.emplace(std::make_pair(sok, so));
					in_rect = (in_rect + b2a_offset.xyxy()).max_i32(GSVector4i(0));
				}
			}
		}
	}

	GSVector4i new_rect = GSVector4i::zero();

	if (src_pgw != dst_pgw)
	{
		const int horizontal_dst_page_offset = page_offset % clamped_tbw;
		const bool single_row = ((src_pgw + horizontal_dst_page_offset) <= clamped_tbw) && (in_rect.height() <= dst_page_size.y);
		const bool single_page = (in_rect.width() <= t_psm.pgs.x) && (in_rect.height() <= t_psm.pgs.y);
		const int vertical_offset = in_rect.y / t_psm.pgs.y;
		const int horizontal_offset = in_rect.x / t_psm.pgs.x;
		const int rect_offset = horizontal_offset + (vertical_offset * src_pgw);
		const int rect_pages = std::max(((in_rect.width() / t_psm.pgs.x) % src_pgw) + ((in_rect.height() / t_psm.pgs.y) * src_pgw), 1);
		page_offset += rect_offset;
		in_rect -= GSVector4i(horizontal_offset * t_psm.pgs.x, vertical_offset * t_psm.pgs.y).xyxy();

		if (sbw == 0)
		{
			if (in_rect.z > dst_page_size.x)
			{
				new_rect.x = 0;
				new_rect.z = (dst_page_size.x);
			}
			else
			{
				new_rect.x = in_rect.x;
				new_rect.z = in_rect.z;
			}
			if (in_rect.w > dst_page_size.y)
			{
				new_rect.y = 0;
				new_rect.w = dst_page_size.y;
			}
			else
			{
				new_rect.y = in_rect.y;
				new_rect.w = in_rect.w;
			}
		}
		else if (src_pgw == 1 && (horizontal_dst_page_offset + rect_pages) <= clamped_tbw)
		{
			new_rect.x = (horizontal_dst_page_offset * t_psm.pgs.x) + in_rect.x;
			new_rect.z = new_rect.x + (rect_pages * t_psm.pgs.x);
			new_rect.y = (page_offset / dst_pgw) * t_psm.pgs.y;
			new_rect.w = new_rect.y + t_psm.pgs.y;
		}
		else if (single_row || single_page)
		{
			const GSVector2i start_page = GSVector2i(page_offset % dst_pgw, page_offset / dst_pgw);
			new_rect.x = (start_page.x * t_psm.pgs.x) + in_rect.x;
			new_rect.z = (start_page.x * t_psm.pgs.x) + in_rect.z;
			new_rect.y = (start_page.y * t_psm.pgs.y) + in_rect.y;
			new_rect.w = (start_page.y * t_psm.pgs.y) + in_rect.w;
		}
		else
		{


			if ((in_rect.width() / dst_page_size.x) == src_pgw)
			{
				if (!is_invalidation && GSConfig.UserHacks_TextureInsideRt < GSTextureInRtMode::MergeTargets)
				{
					DevCon.Warning("Uneven pages mess up sbp %x dbp %x spgw %d dpgw %d src fmt %d dst fmt %d src_rect %d, %d, %d, %d draw %lld", sbp, tbp, src_pgw, dst_pgw, spsm, tpsm, in_rect.x, in_rect.y, in_rect.z, in_rect.w, GSState::s_n);
					return GSVector4i::zero();
				}

				const GSVector2i start_page = GSVector2i(page_offset % dst_pgw, page_offset / dst_pgw);
				int page_count = (in_rect.height() / dst_page_size.y) * src_pgw;

				const int horizontal_offset = (page_count % dst_pgw);
				if (horizontal_offset)
					page_count += dst_pgw - horizontal_offset;

				const int new_height = (page_count / dst_pgw) * dst_page_size.y;
				new_rect.x = 0;
				new_rect.z = dst_pgw * dst_page_size.x;
				new_rect.y = start_page.y * dst_page_size.y;
				new_rect.w = new_rect.y + new_height;
			}
			else
			{
				const GSVector2i start_page = GSVector2i((page_offset + rect_offset) % dst_pgw, page_offset / dst_pgw);
				new_rect.x = start_page.x * dst_page_size.x;
				new_rect.z = new_rect.x + in_rect.z;
				new_rect.y = start_page.y * dst_page_size.y;
				new_rect.w = new_rect.y + in_rect.w;
			}
		}
	}
	else if (!block_offset)
	{
		const int horizontal_dst_page_offset = page_offset % clamped_tbw;
		const int vertical_dst_page_offset = page_offset / clamped_tbw;
		GSVector4i offset_rect(horizontal_dst_page_offset * t_psm.pgs.x, vertical_dst_page_offset * t_psm.pgs.y);
		new_rect = in_rect + offset_rect.xyxy();
	}
	else
		new_rect = in_rect;

	if (new_rect.z > dst_bw)
	{
		if (new_rect.x >= dst_bw)
		{
			new_rect.x -= dst_bw;
			new_rect.z -= dst_bw;
			new_rect.y += t_psm.pgs.y;
			new_rect.w += t_psm.pgs.y;
		}
		else
		{
			new_rect.z = (dst_pgw * dst_page_size.x);
			new_rect.w += dst_page_size.y;
		}
	}
	return new_rect;
}

GSVector4i GSTextureCache::TranslateAlignedRectByPage(Target* t, u32 sbp, u32 spsm, u32 sbw, GSVector4i src_r, bool is_invalidation)
{
	return TranslateAlignedRectByPage(t->m_TEX0.TBP0, t->m_end_block, t->m_TEX0.TBW, t->m_TEX0.PSM, t->m_valid, sbp, spsm, sbw, src_r, is_invalidation);
}

void GSTextureCache::DirtyRectByPage(u32 sbp, u32 spsm, u32 sbw, Target* t, GSVector4i src_r)
{
	if (src_r.rempty())
		return;

	const u32 start_bp = GSLocalMemory::GetStartBlockAddress(sbp, sbw, spsm, src_r);
	const u32 end_bp = GSLocalMemory::GetEndBlockAddress(sbp, sbw, spsm, src_r);
	GL_INS("TC: Invalidating BP: 0x%x (%x -> %x) BW: %d PSM %s Target BP: 0x%x BW %x PSM %s", sbp, start_bp, end_bp, sbw, GSUtil::GetPSMName(spsm), t->m_TEX0.TBP0, t->m_TEX0.TBW, GSUtil::GetPSMName(t->m_TEX0.PSM));

	if (start_bp <= t->m_TEX0.TBP0 && end_bp >= t->UnwrappedEndBlock())
	{
		RGBAMask rgba;
		rgba._u32 = GSUtil::GetChannelMask(spsm);
		AddDirtyRectTarget(t, t->m_valid, t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);
		return;
	}

	GSLocalMemory::psm_t* src_info = &GSLocalMemory::m_psm[spsm];
	const GSLocalMemory::psm_t* dst_info = &GSLocalMemory::m_psm[t->m_TEX0.PSM];
	const int dst_width = std::max(static_cast<int>(t->m_TEX0.TBW * 64), 64);

	int src_width = std::max(static_cast<int>(sbw * 64), 64);
	int src_psm = spsm;

	GSVector4i in_rect = src_r;
	u32 target_bp = t->m_TEX0.TBP0;
	int block_offset = static_cast<int>(sbp) - static_cast<int>(target_bp);
	if (!(src_info->bpp == dst_info->bpp) || block_offset)
	{
		const int src_bpp = src_info->bpp;
		const bool column_align = !block_offset && src_r.z <= src_info->cs.x && src_r.w <= src_info->cs.y && src_info->depth == dst_info->depth;

		if (block_offset && src_info->bpp == dst_info->bpp)
			in_rect = in_rect.ralign<Align_Outside>(src_info->bs);
		else if (column_align)
			in_rect = in_rect.ralign<Align_Outside>(src_info->cs);
		else
			in_rect = in_rect.ralign<Align_Outside>(src_info->pgs);

		const GSVector4i in_pages = GSVector4i(in_rect.x / src_info->pgs.x, in_rect.y / src_info->pgs.y, in_rect.z / src_info->pgs.x, in_rect.w / src_info->pgs.y);
		in_rect -= GSVector4i(in_pages.x * src_info->pgs.x, in_pages.y * src_info->pgs.y, in_pages.z * src_info->pgs.x, in_pages.w * src_info->pgs.y);
		const GSVector4i in_blocks = GSVector4i(in_rect.x / src_info->bs.x, in_rect.y / src_info->bs.y, (in_rect.z + (src_info->bs.x - 1)) / src_info->bs.x, (in_rect.w + (src_info->bs.y - 1)) / src_info->bs.y);

		src_psm = t->m_TEX0.PSM;
		src_info = &GSLocalMemory::m_psm[src_psm];

		in_rect = GSVector4i(in_pages.x * src_info->pgs.x, in_pages.y * src_info->pgs.y, in_pages.z * src_info->pgs.x, in_pages.w * src_info->pgs.y);
		if (column_align)
			in_rect += GSVector4i(in_blocks.x * src_info->cs.x, in_blocks.y * src_info->cs.y, in_blocks.z * src_info->cs.x, in_blocks.w * src_info->cs.y);
		else
			in_rect += GSVector4i(in_blocks.x * src_info->bs.x, in_blocks.y * src_info->bs.y, in_blocks.z * src_info->bs.x, in_blocks.w * src_info->bs.y);

		if (in_rect.rempty())
			return;

		if (src_bpp < 16 || dst_info->bpp < 16)
		{
			if (dst_info->bpp >= 16 && !(src_width & 127))
				src_width = std::max(src_width / 2, 64);
			else if (dst_info->bpp < 16)
				src_width *= 2;
		}
	}

	const int src_pg_width = std::max((src_width + (src_info->pgs.x - 1)) / src_info->pgs.x, 1);
	const int dst_pg_width = std::max((dst_width + (dst_info->pgs.x - 1)) / dst_info->pgs.x, 1);

	int page_offset = (block_offset) >> 5;
	int start_page = page_offset + (in_rect.x / src_info->pgs.x) + ((in_rect.y / src_info->pgs.y) * std::max(static_cast<int>(src_width / 64), 1));
	const int horizontal_pages = (start_page % src_pg_width);
	start_page -= horizontal_pages;

	const GSVector4i page_mask(GSVector4i((src_info->pgs.x - 1), (src_info->pgs.y - 1)).xyxy());
	const GSVector4i page_masked_rect(in_rect & ~page_mask);

	const bool page_aligned_rect = page_masked_rect.eq(in_rect);

	const GSVector4i block_mask(GSVector4i((src_info->bs.x - 1), (src_info->bs.y - 1)).xyxy());
	const GSVector4i block_masked_rect(in_rect & ~block_mask);

	const bool block_aligned_rect = block_masked_rect.eq(in_rect);

	int x_offset = 0;
	int y_offset = 0;

	if (page_offset)
	{
		const int inc_vertical_offset = (page_offset / src_pg_width) * src_info->pgs.y;
		const int inc_horizontal_offset = (page_offset % src_pg_width) * src_info->pgs.x;
		in_rect = (in_rect + GSVector4i(0, inc_vertical_offset).xyxy()).max_i32(GSVector4i(0));
		if (inc_horizontal_offset)
		{
			if (inc_horizontal_offset > 0)
			{
				const int max_horizontal_adjust = std::min(src_width - in_rect.z, inc_horizontal_offset);
				in_rect = (in_rect + GSVector4i(max_horizontal_adjust, 0).xyxy()).max_i32(GSVector4i(0));
				const int h_page_offset = (inc_horizontal_offset / src_info->pgs.x);
				page_offset -= h_page_offset;
				sbp -= h_page_offset << 5;

				if (max_horizontal_adjust == 0)
					x_offset = inc_horizontal_offset;
			}
			else if (inc_horizontal_offset < 0)
			{
				const int max_horizontal_adjust = std::min(in_rect.x, std::abs(inc_horizontal_offset));
				in_rect = (in_rect - GSVector4i(max_horizontal_adjust, 0).xyxy()).max_i32(GSVector4i(0));
				const int h_page_offset = (max_horizontal_adjust / src_info->pgs.x);
				page_offset += h_page_offset;
				sbp += h_page_offset << 5;
			}
		}
		const int vertical_offset = (inc_vertical_offset / src_info->pgs.y) * src_pg_width;
		page_offset -= vertical_offset;

		if (start_page < 0 && in_rect.x == 0 && in_rect.y == 0)
			start_page -= vertical_offset;

		sbp -= vertical_offset << 5;
		block_offset = static_cast<int>(sbp) - static_cast<int>(target_bp);
	}

	if (!x_offset)
		start_page += horizontal_pages;

	const bool matched_format = (src_info->bpp == dst_info->bpp);
	const bool block_matched_format = matched_format && block_aligned_rect;
	const bool req_depth_offset = (src_info->depth > 0 && !page_aligned_rect);
	bool address_offset = block_offset > 0;
	if (block_matched_format && (block_offset || req_depth_offset))
	{
		if (block_offset > 0 || req_depth_offset)
		{
			const int xblocks = in_rect.width() / src_info->bs.x;
			const int yblocks = in_rect.height() / src_info->bs.y;
			if ((xblocks <= (src_info->pgs.x / src_info->bs.x) && yblocks <= (src_info->pgs.y / src_info->bs.y)) || req_depth_offset)
			{
				GSVector4i b2a_offset = GSVector4i::zero();
				const GSVector4i target_rect = GSVector4i(0, 0, src_info->pgs.x, src_info->pgs.y);
				bool new_b2a_offset_found = false;
				bool cache_b2a_offset_found = false;

				SurfaceOffsetKey sok;
				sok.elems[0].bp = sbp;
				sok.elems[0].bw = sbw;
				sok.elems[0].psm = spsm;
				sok.elems[0].rect = src_r;
				sok.elems[1].bp = t->m_TEX0.TBP0;
				sok.elems[1].bw = t->m_TEX0.TBW;
				sok.elems[1].psm = t->m_TEX0.PSM;
				sok.elems[1].rect = t->m_valid;

				const auto it = m_surface_offset_cache.find(sok);
				if (it != m_surface_offset_cache.end())
				{
					b2a_offset = it->second.b2a_offset;
					cache_b2a_offset_found = true;
				}
				else
				{
					for (b2a_offset.y = target_rect.y; b2a_offset.y < target_rect.w; b2a_offset.y += src_info->bs.y)
					{
						for (b2a_offset.x = target_rect.x; b2a_offset.x < target_rect.z; b2a_offset.x += src_info->bs.x)
						{
							const u32 a_candidate_bp = src_info->info.bn(b2a_offset.x, b2a_offset.y, target_bp, src_pg_width);
							if (sbp == a_candidate_bp)
							{
								new_b2a_offset_found = true;
								break;
							}
						}
						if (new_b2a_offset_found)
							break;
					}
				}

				if (cache_b2a_offset_found || new_b2a_offset_found)
				{
					if (!cache_b2a_offset_found)
					{
						SurfaceOffset so;
						so.is_valid = true;
						so.b2a_offset = b2a_offset;

						if (m_surface_offset_cache.size() + 1 > S_SURFACE_OFFSET_CACHE_MAX_SIZE)
							m_surface_offset_cache.clear();

						m_surface_offset_cache.emplace(std::make_pair(sok, so));
					}

					if (b2a_offset.x && (b2a_offset.x + in_rect.z) > src_width)
					{
						if ((b2a_offset.x + in_rect.z) <= dst_width)
						{
							x_offset += b2a_offset.x;
						}

						b2a_offset.x = 0;
					}
					address_offset = false;
					in_rect = (in_rect + b2a_offset.xyxy()).max_i32(GSVector4i(0));

					if (block_offset > 0 && !req_depth_offset)
					{
						sbp = dst_info->info.bn(b2a_offset.x + x_offset, b2a_offset.y, target_bp, t->m_TEX0.TBW);
						target_bp = dst_info->info.bn(b2a_offset.x, b2a_offset.y, target_bp, t->m_TEX0.TBW);
					}
				}
			}
			else
			{
				in_rect.x = in_rect.x & ~(src_info->pgs.x - 1);
				in_rect.z = (in_rect.z + (src_info->pgs.x - 1)) & ~(src_info->pgs.x - 1);
				in_rect.y = in_rect.y & ~(src_info->pgs.y - 1);
				in_rect.w = (in_rect.w + (src_info->pgs.y - 1)) & ~(src_info->pgs.y - 1);
			}
		}
		else
		{
			in_rect.x = in_rect.x & ~(src_info->pgs.x - 1);
			in_rect.z = (in_rect.z + (src_info->pgs.x - 1)) & ~(src_info->pgs.x - 1);
			in_rect.y = in_rect.y & ~(src_info->pgs.y - 1);
			in_rect.w = (in_rect.w + (src_info->pgs.y - 1)) & ~(src_info->pgs.y - 1);
		}
	}

	const int adjusted_src_width = (src_info->bpp >= 16) ? src_width * 2 : src_width;
	const int adjusted_dst_width = (dst_info->bpp >= 16) ? dst_width * 2 : dst_width;

	const bool width_okay = (adjusted_src_width == adjusted_dst_width) || (in_rect.z <= dst_width && in_rect.w <= src_info->pgs.y);

	RGBAMask rgba;
	rgba._u32 = GSUtil::GetChannelMask(spsm);

	if ((matched_format || page_aligned_rect || block_matched_format) && width_okay && sbp == target_bp)
	{
		in_rect = in_rect.rintersect(t->m_valid);

		if (!in_rect.rempty())
			AddDirtyRectTarget(t, in_rect, t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);
	}
	else
	{
		const int page_draw = (std::max(src_info->pgs.x, in_rect.width()) + (src_info->pgs.x - 1)) / src_info->pgs.x;
		const int page_skip = src_pg_width - page_draw;
		const int vertical_pages = (std::max(src_info->pgs.y, in_rect.height()) / src_info->pgs.y);
		const int horisontal_pages = (std::max(src_info->pgs.x, in_rect.width()) / src_info->pgs.x);
		const int totalpages = vertical_pages * horisontal_pages + (page_skip * (vertical_pages - 1));
		const bool single_width = page_draw == 1;

		if (block_offset || req_depth_offset)
		{
			const int block_x = in_rect.x & (src_info->pgs.x - 1);
			const int block_y = in_rect.y & (src_info->pgs.y - 1);
			x_offset += block_x;
			y_offset += block_y;

			if (address_offset)
			{
				const int blocks_wide = src_info->pgs.x / src_info->bs.x;
				const int block_x_offset = (block_offset % blocks_wide) * src_info->bs.x;
				const int block_y_offset = (block_offset / blocks_wide) * src_info->bs.y;

				x_offset += block_x_offset;
				y_offset += block_y_offset;
			}

			if (block_x)
				in_rect = GSVector4i(in_rect.x - block_x, in_rect.y, in_rect.z - block_x, in_rect.w);
			if (block_y)
				in_rect = GSVector4i(in_rect.x, in_rect.y - block_y, in_rect.z, in_rect.w - block_y);
		}
		if ((!(sbp & 31) || sbp == target_bp) && !x_offset && (in_rect.z <= src_width || (in_rect.z % src_width) == 0) &&
			(!(src_width & (src_info->pgs.x - 1)) || totalpages == 1) && !(in_rect.x & (src_info->pgs.x - 1)))
		{
			const int end_page = start_page + totalpages;
			const int width = ((totalpages == 1 || single_width) && matched_format) ? in_rect.width() : dst_info->pgs.x;
			const int height = (totalpages == 1 && matched_format) ? in_rect.height() : dst_info->pgs.y;
			GSVector4i new_rect = GSVector4i::zero();

			int drawn = 0;
			for (int page = start_page; page < end_page; page++)
			{
				new_rect.x = x_offset + (page % dst_pg_width) * dst_info->pgs.x;
				new_rect.z = new_rect.x + width;
				new_rect.y = y_offset + (page / dst_pg_width) * dst_info->pgs.y;
				new_rect.w = new_rect.y + height;
				new_rect = new_rect.rintersect(t->m_valid);

				if (!new_rect.rempty())
					AddDirtyRectTarget(t, new_rect, t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);

				drawn++;
				if (drawn == page_draw)
				{
					drawn = 0;
					page += page_skip;
				}
			}
		}
		else
		{
			if (in_rect.x & (dst_info->pgs.x - 1))
			{
				const u32 rect_off = (in_rect.x & (dst_info->pgs.x - 1));
				in_rect.x -= rect_off;
				in_rect.z -= rect_off;
				x_offset += rect_off;
			}
			if (in_rect.y & (dst_info->pgs.y - 1))
			{
				const u32 rect_off = (in_rect.y & (dst_info->pgs.y - 1));
				in_rect.y -= rect_off;
				in_rect.w -= rect_off;
				y_offset += rect_off;
			}

			const int offset_pages = (x_offset / dst_info->pgs.x);
			const int new_start_page = offset_pages + start_page;
			const int end_page = new_start_page + totalpages;
			const int width = ((totalpages == 1 || single_width) && matched_format) ? in_rect.width() : dst_info->pgs.x;
			const int height = (totalpages == 1 && matched_format) ? in_rect.height() : dst_info->pgs.y;

			GSVector4i new_rect = GSVector4i::zero();
			x_offset -= offset_pages * dst_info->pgs.x;

			int drawn = 0;
			for (int page = new_start_page; page < end_page; page++)
			{
				int overflow = 0;
				new_rect.x = x_offset + (page % dst_pg_width) * dst_info->pgs.x;

				int x_end_pos = new_rect.x + width;
				if (x_end_pos > dst_width)
				{
					overflow = x_end_pos - dst_width;
					x_end_pos = dst_width;
				}
				new_rect.z = std::min(x_end_pos, dst_width);
				new_rect.y = y_offset + (page / dst_pg_width) * dst_info->pgs.y;
				new_rect.w = new_rect.y + height;
				new_rect = new_rect.rintersect(t->m_valid);

				if (!new_rect.rempty())
					AddDirtyRectTarget(t, new_rect, t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);

				if (overflow)
				{
					new_rect.x = 0;
					new_rect.z = overflow;
					new_rect.y = ((page + 1) / dst_pg_width) * dst_info->pgs.y;
					new_rect.w = new_rect.y + height;
					new_rect = new_rect.rintersect(t->m_valid);

					if (!new_rect.rempty())
						AddDirtyRectTarget(t, new_rect, t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);
				}

				drawn++;
				if (drawn == page_draw)
				{
					drawn = 0;
					page += page_skip;
				}
			}
		}
	}
}

__ri static GSTextureCache::Source* FindSourceInMap(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA,
	const GSLocalMemory::psm_t& psm_s, const u32* clut, const GSTexture* gpu_clut, const GSVector2i& compare_lod,
	const GSTextureCache::SourceRegion& region, u32 fixed_tex0, FastList<GSTextureCache::Source*>& map, const GIFRegCLAMP& CLAMP, GSVector4i read_area)
{
	GSTextureCache::Source* best_s = nullptr;

	for (auto i = map.begin(); i != map.end(); ++i)
	{
		GSTextureCache::Source* s = *i;
		if (((TEX0.U32[0] ^ s->m_TEX0.U32[0]) | ((TEX0.U32[1] ^ s->m_TEX0.U32[1]) & 3)) != 0)
			continue;

		if (!s->m_target)
		{
			if (psm_s.pal > 0)
			{
				if (gpu_clut && !s->m_palette)
					continue;

				if (!s->m_palette && !s->ClutMatch({clut, psm_s.pal}))
					continue;
			}
			else
			{
				if (psm_s.fmt > 0 && s->m_TEXA.U64 != TEXA.U64)
					continue;
			}

			
			if (((s->m_region.bits | fixed_tex0) != 0))
			{
				const bool is_clamped = CLAMP.WMS == CLAMP_CLAMP && CLAMP.WMT == CLAMP_CLAMP;
				if (is_clamped && s->m_region.GetMinX() == 0 && s->m_region.GetMinY() == 0)
				{
					const GSVector4i read_region = GSVector4i(0, 0, read_area.z, read_area.w);
					const GSVector4i region_area = GSVector4i(s->m_region.GetMinX(), s->m_region.GetMinY(), s->m_region.HasX() ? s->m_region.GetMaxX() : (1 << s->m_TEX0.TW), s->m_region.HasY() ? s->m_region.GetMaxY() : (1 << s->m_TEX0.TW));

					if (!region_area.rintersect(read_region).eq(read_region))
						continue;
				}
				else if (((s->m_region.bits | fixed_tex0) != 0) && s->m_region.bits != region.bits)
				{
					continue;
				}
			}

			if (s->m_lod != compare_lod)
				continue;
		}

		map.MoveFront(i.Index());
		return s;
	}

	if (best_s != nullptr)
		return best_s;
	else
		return nullptr;
}

GSTextureCache::Source* GSTextureCache::LookupDepthSource(const bool is_depth, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const GIFRegCLAMP& CLAMP, const GSVector4i& r, const bool possible_shuffle, const bool linear, const GIFRegFRAME& frame, bool req_color, bool req_alpha, bool palette)
{
	if (GSConfig.UserHacks_DisableDepthSupport)
	{
		GL_CACHE("TC: LookupDepthSource not supported (0x%x, F:0x%x)", TEX0.TBP0, TEX0.PSM);
		return nullptr;
	}

	GL_CACHE("TC: Lookup Depth Source <%d,%d => %d,%d> (0x%x, %s, BW: %u, CBP: 0x%x, TW: %d, TH: %d)", r.x, r.y, r.z,
		r.w, TEX0.TBP0, GSUtil::GetPSMName(TEX0.PSM), TEX0.TBW, TEX0.CBP, 1 << TEX0.TW, 1 << TEX0.TH);

	const SourceRegion region = SourceRegion::Create(TEX0, CLAMP);
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[TEX0.PSM];
	const u32* const clut = g_gs_renderer->m_mem.m_clut;
	GSTexture* const gpu_clut = (psm_s.pal > 0) ? g_gs_renderer->m_mem.m_clut.GetGPUTexture() : nullptr;
	Source* src = FindSourceInMap(TEX0, TEXA, psm_s, clut, gpu_clut, GSVector2i(0, 0), region,
		region.IsFixedTEX0(TEX0), m_src.m_map[TEX0.TBP0 >> 5], CLAMP, r);
	if (src)
	{
		GL_CACHE("TC: src hit: (0x%x, %s)", TEX0.TBP0, GSUtil::GetPSMName(TEX0.PSM));
		src->Update(r);
		return src;
	}

	Target* dst = nullptr;

	const u32 bp = TEX0.TBP0;
	const u32 psm = TEX0.PSM;
	bool inside_target = false;
	GSVector4i target_rc(r);

	GSVector4i block_boundary_rect = target_rc;
	block_boundary_rect.x = block_boundary_rect.x & ~(psm_s.bs.x - 1);
	block_boundary_rect.y = block_boundary_rect.y & ~(psm_s.bs.y - 1);
	block_boundary_rect.z = std::max(target_rc.x + 1, (block_boundary_rect.z + (psm_s.bs.x / 2)) & ~(psm_s.bs.x - 1));
	block_boundary_rect.w = std::max(target_rc.y + 1, (block_boundary_rect.w + (psm_s.bs.y / 2)) & ~(psm_s.bs.y - 1));

	for (auto t : m_dst[DepthStencil])
	{
		if (!t->m_used || (!t->m_dirty.empty() && !is_depth))
			continue;

		if (GSUtil::HasSharedBits(bp, psm, t->m_TEX0.TBP0, t->m_TEX0.PSM))
		{
			GL_INS("TC: Found target in Depth list BP: %x but is RenderTarget", t->m_TEX0.TBP0);
			if (t->m_age == 0)
			{
				dst = t;
				inside_target = false;
				break;
			}
			else if (t->m_age == 1)
			{
				dst = t;
				inside_target = false;
			}
		}
		else if (!dst && bp >= t->m_TEX0.TBP0 && bp < t->m_end_block)
		{
			const bool can_translate = CanTranslate(bp, TEX0.TBW, psm, r, t->m_TEX0.TBP0, t->m_TEX0.PSM, t->m_TEX0.TBW);
			const bool swizzle_match = psm_s.depth == GSLocalMemory::m_psm[t->m_TEX0.PSM].depth;
			GSVector4i new_rect = block_boundary_rect;

			if (can_translate)
			{
				if (swizzle_match)
				{
					block_boundary_rect = TranslateAlignedRectByPage(t, bp, psm, TEX0.TBW, new_rect);
				}
				else
				{
					const GSVector2i src_page_size = psm_s.pgs;
					new_rect.x &= ~(src_page_size.x - 1);
					new_rect.y &= ~(src_page_size.y - 1);
					new_rect.z = (new_rect.z + (src_page_size.x - 1)) & ~(src_page_size.x - 1);
					new_rect.w = (new_rect.w + (src_page_size.y - 1)) & ~(src_page_size.y - 1);
					block_boundary_rect = TranslateAlignedRectByPage(t, bp & ~((1 << 5) - 1), psm, TEX0.TBW, new_rect);
				}

				if (!block_boundary_rect.rempty())
				{
					dst = t;
					inside_target = true;
				}
			}
		}
	}

	if (dst && psm_s.trbpp != 24 && !dst->HasValidAlpha())
	{
		for (Target* t : m_dst[RenderTarget])
		{
			if (t->m_age <= 1 && t->m_TEX0.TBP0 == bp && t->m_TEX0.TBW == TEX0.TBW && t->HasValidAlpha())
			{
				GL_CACHE("TC: depth: Using RT %x instead of depth because of missing alpha", t->m_TEX0.TBP0);

				if (FullRectDirty(t, 0x7))
					t->Update();
				else if (!t->m_valid_rgb || dst->m_unscaled_size != t->m_unscaled_size)
				{
					if (dst->m_unscaled_size != t->m_unscaled_size)
					{
						t->ResizeTexture(t->m_unscaled_size.x, t->m_unscaled_size.y);
					}

					CopyRGBFromDepthToColor(t, dst);
				}

				t->m_valid = t->m_valid.runion(dst->m_valid);
				dst = t;

				inside_target = false;
				break;
			}
		}
	}

	if (!dst && is_depth)
	{
		for (auto t : m_dst[RenderTarget])
		{
			if (t->m_age <= 1 && t->m_used && t->m_dirty.empty() && GSUtil::HasSharedBits(bp, psm, t->m_TEX0.TBP0, t->m_TEX0.PSM))
			{
				dst = t;
				inside_target = false;
				break;
			}
		}
	}

	if (dst)
	{
		GL_CACHE("TC: depth: dst %s hit: (0x%x, %s)", to_string(dst->m_type),
			TEX0.TBP0, GSUtil::GetPSMName(psm));

		src = new Source(TEX0, TEXA);
		src->m_texture = dst->m_texture;
		src->m_scale = dst->m_scale;
		src->m_unscaled_size = dst->m_unscaled_size;
		src->m_shared_texture = true;
		src->m_target = true;
		src->m_target_direct = true;
		src->m_from_target = dst;
		src->m_from_target_TEX0 = dst->m_TEX0;
		src->m_32_bits_fmt = dst->m_32_bits_fmt;
		src->m_valid_rect = dst->m_valid;
		src->m_end_block = dst->m_end_block;

		if (inside_target)
		{
			src->m_region.SetX(block_boundary_rect.x, std::max(src->m_from_target->m_valid.z, block_boundary_rect.z));
			src->m_region.SetY(block_boundary_rect.y, std::max(src->m_from_target->m_valid.w, block_boundary_rect.w));
		}

		if (GSRendererHW::GetInstance()->IsTBPFrameOrZ(dst->m_TEX0.TBP0))
		{
			m_temporary_source = src;
		}
		else
		{
			src->SetPages();
			m_src.Add(src, TEX0);
		}

		if (palette)
		{
			AttachPaletteToSource(src, psm_s.pal, true, true);
		}
	}
	else
	{
		return is_depth ? LookupSource(false, TEX0, TEXA, CLAMP, r, nullptr, possible_shuffle, linear, frame, req_color, req_alpha) : nullptr;
	}

	pxAssert(src->m_texture);
	pxAssert(src->m_scale == (dst ? dst->m_scale : 1.0f));

	return src;
}

GSTextureCache::Source* GSTextureCache::LookupSource(const bool is_color, const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const GIFRegCLAMP& CLAMP, const GSVector4i& r, const GSVector2i* lod, const bool possible_shuffle, const bool linear, const GIFRegFRAME& frame, bool req_color, bool req_alpha)
{
	GL_CACHE("TC: Lookup Source <%d,%d => %d,%d> (0x%x, %s, BW: %u, CBP: 0x%x, TW: %d, TH: %d)", r.x, r.y, r.z, r.w, TEX0.TBP0, GSUtil::GetPSMName(TEX0.PSM), TEX0.TBW, TEX0.CBP, 1 << TEX0.TW, 1 << TEX0.TH);

	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[TEX0.PSM];

	const u32* const clut = g_gs_renderer->m_mem.m_clut;
	GSTexture* const gpu_clut = (psm_s.pal > 0) ? g_gs_renderer->m_mem.m_clut.GetGPUTexture() : nullptr;

	SourceRegion region = SourceRegion::Create(TEX0, CLAMP);

	if ((TEX0.TW > 10 && !region.HasX()) || (TEX0.TH > 10 && !region.HasY()))
	{
		GL_CACHE("TC: Invalid TEX0 size %ux%u without region, aborting draw.", TEX0.TW, TEX0.TH);
		return nullptr;
	}

	if (TEX0.TBW > 32 && !GSUtil::IsValidPSM(static_cast<int>(TEX0.PSM)))
	{
		GL_CACHE("TC: Invalid TEX0 (TBW=%d, PSM=%x), using temporary source.", TEX0.TBW, TEX0.PSM);
		return CreateSource(TEX0, TEXA, CLAMP, nullptr, 0, 0, nullptr, nullptr, nullptr, SourceRegion(), true);
	}

	const GSVector2i compare_lod(lod ? *lod : GSVector2i(0, 0));
	Source* src = nullptr;

	const u32 lookup_page = TEX0.TBP0 >> 5;
	const bool is_fixed_tex0 = region.IsFixedTEX0(1 << TEX0.TW, 1 << TEX0.TH);
	if (region.GetMinX() != 0 || region.GetMinY() != 0)
	{
		const GSOffset offset(psm_s.info, TEX0.TBP0, TEX0.TBW, TEX0.PSM);
		const u32 region_page = offset.bn(region.GetMinX(), region.GetMinY()) >> 5;
		if (lookup_page != region_page)
			src = FindSourceInMap(TEX0, TEXA, psm_s, clut, gpu_clut, compare_lod, region, is_fixed_tex0, m_src.m_map[region_page], CLAMP, r);
	}
	if (!src)
		src = FindSourceInMap(TEX0, TEXA, psm_s, clut, gpu_clut, compare_lod, region, is_fixed_tex0, m_src.m_map[lookup_page], CLAMP, r);

	if (src && src->m_from_target && GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::MergeTargets && GSLocalMemory::GetUnwrappedEndBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, r) > src->m_from_target->m_end_block)
	{
		m_src.RemoveAt(src);
		src = nullptr;
	}

	Target* dst = nullptr;
	int x_offset = 0;
	int y_offset = 0;

	bool target_bp_hit_outside_valid_area = false;

#ifdef DISABLE_HW_TEXTURE_CACHE
	if (0)
#else
	if (!src && (GSConfig.UserHacks_CPUFBConversion || TEX0.PSM != PSMT4))
#endif
	{
		const u32 bp = TEX0.TBP0;
		const u32 psm = TEX0.PSM;
		const u32 bw = TEX0.TBW;

		GSVector4i req_rect = r;

		req_rect.x = region.HasX() ? region.GetMinX() : 0;
		req_rect.y = region.HasY() ? region.GetMinY() : 0;

		GSVector4i block_boundary_rect = req_rect;
		block_boundary_rect.x = block_boundary_rect.x & ~(psm_s.bs.x - 1);
		block_boundary_rect.y = block_boundary_rect.y & ~(psm_s.bs.y - 1);
		block_boundary_rect.z = std::max(req_rect.x + 1, (block_boundary_rect.z + (psm_s.bs.x / 2)) & ~(psm_s.bs.x - 1));
		block_boundary_rect.w = std::max(req_rect.y + 1, (block_boundary_rect.w + (psm_s.bs.y / 2)) & ~(psm_s.bs.y - 1));

		bool found_t = false;
		bool tex_merge_rt = false;
		auto& list = m_dst[RenderTarget];
		for (auto i = list.begin(); i != list.end(); ++i)
		{
			Target* t = *i;
			if (t->m_used)
			{
				const bool overlaps = t->Overlaps(bp, bw, psm, block_boundary_rect);
				if (!overlaps || (found_t && (GSState::s_n - dst->m_last_draw) < (GSState::s_n - t->m_last_draw)))
					continue;

				constexpr u32 addr_mask = GS_BLOCKS_PER_PAGE - 1;
				if (((bp & addr_mask) != (t->m_TEX0.TBP0 & addr_mask)) && (bp & addr_mask))
					continue;

				const bool width_match = (std::max(64U, bw * 64U) >> GSLocalMemory::m_psm[psm].info.pageShiftX()) ==
				                         (std::max(64U, t->m_TEX0.TBW * 64U) >> GSLocalMemory::m_psm[t->m_TEX0.PSM].info.pageShiftX());

				const bool t_maybe_dirty = !t->m_dirty.empty() || !t->OverlapsValid(bp, bw, psm, req_rect);

				if (bp == t->m_TEX0.TBP0 && t_maybe_dirty && GSUtil::GetChannelMask(psm) == GSUtil::GetChannelMask(t->m_TEX0.PSM) && GSRendererHW::GetInstance()->m_draw_transfers.size() > 0)
				{
					bool can_use = true;

					if (!possible_shuffle && !(GSLocalMemory::m_psm[TEX0.PSM].bpp == 16 && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 32) && !width_match && t_maybe_dirty && !t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size).eq(t->m_valid))
					{
						std::vector<GSState::GSUploadQueue>::reverse_iterator iter;

						const int start_draw = GSRendererHW::GetInstance()->m_draw_transfers.back().draw;

						for (iter = GSRendererHW::GetInstance()->m_draw_transfers.rbegin(); iter != GSRendererHW::GetInstance()->m_draw_transfers.rend();)
						{
							if (TEX0.TBP0 == iter->blit.DBP && GSUtil::HasCompatibleBits(iter->blit.DPSM, TEX0.PSM) && req_rect.rintersect(iter->rect).eq(req_rect))
							{
								can_use = false;
								break;
							}

							if (start_draw - iter->draw > 0)
								break;

							++iter;
						}
					}

					if (!can_use)
					{
						InvalidateSourcesFromTarget(t);
						i = list.erase(i);
						delete t;
						continue;
					}
				}

				const u32 t_psm = t->HasValidAlpha() ? t->m_TEX0.PSM & ~0x1 : ((t->m_TEX0.PSM == PSMCT32) ? PSMCT24 : t->m_TEX0.PSM);
				bool rect_clean = GSUtil::HasSameSwizzleBits(psm, t_psm) || (t_psm == PSMCT32 && GSLocalMemory::m_psm[psm].bpp == 16 && possible_shuffle);
				const bool tex_overlaps = bp >= t->m_TEX0.TBP0 && bp < t->UnwrappedEndBlock();
				const bool real_fmt_match = (GSLocalMemory::m_psm[psm].trbpp == 16) == (t->m_32_bits_fmt == false);
				if (rect_clean && tex_overlaps && !t->m_dirty.empty() && width_match)
				{
					GSVector4i new_rect = req_rect;

					if (linear)
					{
						new_rect.z -= 1;
						new_rect.w -= 1;
					}

					bool partial = false;
					if (bp > t->m_TEX0.TBP0)
					{
						const GSVector2i page_size = GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs;
						const bool can_translate = CanTranslate(bp, bw, psm, new_rect, t->m_TEX0.TBP0, t->m_TEX0.PSM, t->m_TEX0.TBW);
						const bool swizzle_match = GSLocalMemory::m_psm[psm].depth == GSLocalMemory::m_psm[t->m_TEX0.PSM].depth;

						if (can_translate)
						{
							if (swizzle_match)
							{
								new_rect = TranslateAlignedRectByPage(t, bp, psm, bw, new_rect);
							}
							else
							{
								if (GSLocalMemory::m_psm[psm].bpp != GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp)
								{
									const GSVector2i dst_page_size = GSLocalMemory::m_psm[psm].pgs;
									new_rect = GSVector4i(new_rect.x / page_size.x, new_rect.y / page_size.y, (new_rect.z + (page_size.x - 1)) / page_size.x, (new_rect.w + (page_size.y - 1)) / page_size.y);
									new_rect = GSVector4i(new_rect.x * dst_page_size.x, new_rect.y * dst_page_size.y, new_rect.z * dst_page_size.x, new_rect.w * dst_page_size.y);
								}
								else
								{
									new_rect.x &= ~(page_size.x - 1);
									new_rect.y &= ~(page_size.y - 1);
									new_rect.z = (new_rect.z + (page_size.x - 1)) & ~(page_size.x - 1);
									new_rect.w = (new_rect.w + (page_size.y - 1)) & ~(page_size.y - 1);
								}
								new_rect = TranslateAlignedRectByPage(t, bp & ~((1 << 5) - 1), psm, bw, new_rect);
							}

							rect_clean = !new_rect.eq(GSVector4i::zero());
						}
						else
						{
							SurfaceOffsetKey sok;
							sok.elems[0].bp = bp;
							sok.elems[0].bw = bw;
							sok.elems[0].psm = psm;
							sok.elems[0].rect = new_rect;
							sok.elems[1].bp = t->m_TEX0.TBP0;
							sok.elems[1].bw = t->m_TEX0.TBW;
							sok.elems[1].psm = t->m_TEX0.PSM;
							sok.elems[1].rect = t->m_valid;

							const SurfaceOffset so = ComputeSurfaceOffset(sok);
							if (so.is_valid)
							{
								new_rect = so.b2a_offset;
							}
						}
					}

					if (rect_clean)
					{
						bool can_use = true;
						for (auto& dirty : t->m_dirty)
						{
							const GSVector4i dirty_rect = dirty.GetDirtyRect(t->m_TEX0, t->m_TEX0.PSM != dirty.psm);
							if (!dirty_rect.rintersect(new_rect).rempty())
							{
								rect_clean = false;

								if (!dirty_rect.rintersect(t->m_valid).eq(t->m_valid) || GSUtil::GetChannelMask(t->m_TEX0.PSM) != t->m_dirty.GetDirtyChannels())
									partial |= !new_rect.rintersect(dirty_rect).eq(new_rect) || dirty_rect.eq(new_rect);
								else
								{
									can_use = false;
								}
								break;
							}
						}

						if (!can_use)
						{
							InvalidateSourcesFromTarget(t);
							i = list.erase(i);
							delete t;
							continue;
						}
					}

					const u32 channel_mask = GSUtil::GetChannelMask(psm);
					const u32 channels = t->m_dirty.GetDirtyChannels() & channel_mask;
					const bool dirty_overlap = !t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size).rintersect(new_rect).rempty();
					if (!possible_shuffle && t && GSUtil::HasCompatibleBits(psm, t->m_TEX0.PSM) && real_fmt_match)
					{
						GSVector4i dirty_rect = (t->m_dirty.size() > 0 && bw == t->m_TEX0.TBW) ? t->m_dirty.GetTotalRect(t->m_TEX0, GSVector2i(new_rect.z, new_rect.w)) : GSVector4i(GSVector4(t->m_valid) * GSVector4(2));
						GSVector4i resize_rect = new_rect;
						if (CLAMP.WMS == 0 || CLAMP.WMS == 3)
							resize_rect.z = std::min(resize_rect.z, static_cast<int>(t->m_TEX0.TBW) * 64);
						if ((CLAMP.WMT == 0 || CLAMP.WMT == 3) && resize_rect.w > (t->m_valid.w * 2))
							resize_rect.w = std::min(resize_rect.w, std::max(t->m_valid.w * 2, dirty_rect.w));

						if (t->Overlaps(bp, bw, psm, new_rect))
							ResizeTarget(t, resize_rect, bp, psm, bw);
					}
					if (dirty_overlap && ((channels & channel_mask) != channel_mask || partial))
					{
						t->Update();
						rect_clean = true;
					}

					if (linear)
					{
						new_rect.z += 1;
						new_rect.w += 1;
					}
				}
				else
				{
					rect_clean = t->m_dirty.empty();
					if (!possible_shuffle && frame.Block() != t->m_TEX0.TBP0 && rect_clean && bp == t->m_TEX0.TBP0 && t && GSUtil::HasCompatibleBits(psm, t->m_TEX0.PSM) && width_match && real_fmt_match)
					{
						if (!tex_merge_rt && t->Overlaps(bp, bw, psm, req_rect))
						{
							if (psm_s.bpp == GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp && !block_boundary_rect.rintersect(t->m_valid).eq(block_boundary_rect))
							{
								RGBAMask rgba_mask;
								rgba_mask.c.a = req_alpha;
								rgba_mask.c.r = rgba_mask.c.g = rgba_mask.c.b = req_color;
								if (block_boundary_rect.z > t->m_valid.z)
									AddDirtyRectTarget(t, GSVector4i(t->m_valid.z, t->m_valid.y, block_boundary_rect.z, std::max(block_boundary_rect.w, t->m_valid.w)), t->m_TEX0.PSM, t->m_TEX0.TBW, rgba_mask);
								if (block_boundary_rect.w > t->m_valid.w)
									AddDirtyRectTarget(t, GSVector4i(t->m_valid.x, t->m_valid.w, std::max(block_boundary_rect.z, t->m_valid.z), block_boundary_rect.w), t->m_TEX0.PSM, t->m_TEX0.TBW, rgba_mask);
							}

							GSVector4i resize_rect = req_rect;
							if (CLAMP.WMS == 0 || CLAMP.WMS == 3)
								resize_rect.z = std::min(resize_rect.z, static_cast<int>(t->m_TEX0.TBW) * 64);
							if ((CLAMP.WMT == 0 || CLAMP.WMT == 3) && resize_rect.w > (t->m_valid.w * 2))
								resize_rect.w = std::min(resize_rect.w, t->m_valid.w * 2);

							ResizeTarget(t, resize_rect, bp, psm, bw);
						}
					}
				}

				if (t->m_TEX0.TBP0 != frame.Block() && !possible_shuffle && bp > t->m_TEX0.TBP0 && t->Overlaps(bp, bw, psm, req_rect) && GSUtil::GetChannelMask(psm) == GSUtil::GetChannelMask(t->m_TEX0.PSM) && !width_match)
				{
					GSVector4i new_rect = req_rect;

					if (linear)
					{
						new_rect.z -= 1;
						new_rect.w -= 1;
					}

					const GSLocalMemory::psm_t* src_info = &GSLocalMemory::m_psm[psm];
					const int block_offset = static_cast<int>(bp) - static_cast<int>(t->m_TEX0.TBP0);
					const int page_offset = (block_offset) >> 5;
					const int start_page = page_offset + (new_rect.x / src_info->pgs.x) + ((new_rect.y / src_info->pgs.y) * std::max(static_cast<int>(bw), 1));
					const int src_page_width = std::max(static_cast<int>((bw * 64) / src_info->pgs.x), 1);
					const int dst_page_width = std::max(static_cast<int>((t->m_TEX0.TBW * 64) / GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs.x), 1);
					if (((start_page % dst_page_width) + src_page_width) > dst_page_width)
					{
						const u32 read_start = GSLocalMemory::GetStartBlockAddress(bp, bw, psm, new_rect);
						const u32 read_end = GSLocalMemory::GetEndBlockAddress(bp, bw, psm, new_rect);

						if (read_start > t->m_TEX0.TBP0 && read_end < t->m_end_block)
						{
							InvalidateSourcesFromTarget(t);
							i = list.erase(i);
							delete t;
							continue;
						}
					}
				}

				bool overlapping_dirty = true;

				if (!rect_clean)
				{
					const u32 read_start = GSLocalMemory::GetStartBlockAddress(bp, bw, psm, block_boundary_rect);
					const u32 read_end = GSLocalMemory::GetUnwrappedEndBlockAddress(bp, bw, psm, block_boundary_rect);
					const GSVector4i dirty_rect = t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size);
					const u32 dirty_start = GSLocalMemory::GetStartBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, dirty_rect);
					const u32 dirty_end = GSLocalMemory::GetUnwrappedEndBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, dirty_rect);

					overlapping_dirty = read_start <= dirty_end && read_end >= dirty_start;

					if (overlapping_dirty && (psm == PSMT8 || psm == PSMT4))
						continue;
				}

				const bool t_clean = ((t->m_dirty.GetDirtyChannels() & GSUtil::GetChannelMask(psm)) == 0) || rect_clean;
				const u32 color_psm = ((psm & 0x30) == 0x30) ? (psm & ~0x30) : psm;
				const u32 tex_color_psm = ((t->m_TEX0.PSM & 0x30) == 0x30) ? (t->m_TEX0.PSM & ~0x30) : t->m_TEX0.PSM;
				const bool can_convert = (GSUtil::HasCompatibleBits(psm, t_psm) && ((bw == t->m_TEX0.TBW) || (bw <= 1 && req_rect.w < GSLocalMemory::m_psm[psm].pgs.y))) ||
				                         (possible_shuffle && ((bw == t->m_TEX0.TBW) || (bw == (t->m_TEX0.TBW * 2) || bw <= 2)) && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 32);

				if (t->m_was_dst_matched)
				{
					const bool indexed_format = psm_s.trbpp < 16;
					const bool alpha_ok = (!indexed_format && (!req_alpha || req_alpha == (t->m_valid_alpha_low || t->m_valid_alpha_high))) || (indexed_format && (t->m_valid_alpha_low || t->m_valid_alpha_high));
					if (((!indexed_format && req_color) || (psm == PSMT8 || psm == PSMT4)) && alpha_ok && !t->m_valid_rgb)
					{
						GL_CACHE("TC: Attempt to repopulate RGB for target [%x] on source lookup", t->m_TEX0.TBP0);
						for (Target* dst_match : m_dst[DepthStencil])
						{
							if (dst_match->m_TEX0.TBP0 != t->m_TEX0.TBP0 || !dst_match->m_valid_rgb || (!dst_match->m_dirty.empty() && !dst_match->m_dirty.GetTotalRect(dst_match->m_TEX0, dst_match->m_unscaled_size).rintersect(block_boundary_rect).rempty()))
								continue;

							if (!CopyRGBFromDepthToColor(t, dst_match))
							{
								DevCon.Warning("Failed to update dst matched texture");
							}
							t->m_valid_rgb = true;
							t->m_TEX0 = dst_match->m_TEX0;
							break;
						}
					}
				}

				if (((!t_clean && can_convert) || t_clean || !overlapping_dirty) && GSUtil::HasSharedBits(bp, psm, t->m_TEX0.TBP0, t_psm))
				{
					bool match = true;
					if (found_t && (bw != t->m_TEX0.TBW || t->m_TEX0.PSM != psm))
						match = false;

					if (match)
					{
						if (psm == PSMT4 || (psm == PSMT8H && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 16))
						{
							if (!t->m_drawn_since_read.rempty())
							{
								t->UnscaleRTAlpha();

								Read(t, t->m_drawn_since_read);

								t->m_drawn_since_read = GSVector4i::zero();
							}
						}
						else
						{
							const bool outside_target = !t->OverlapsValid(bp, bw, psm, r);

							if (!possible_shuffle && outside_target)
							{
								target_bp_hit_outside_valid_area = true;
								GL_CACHE(
									"TC: LookupSource: Target BP match but outside valid area;"
									" Source=(BP=%04x, BW=%d, PSM=%s, req=(%x,%x - %x,%x));"
									" Target=(BP=%04x, BW=%d, PSM=%s, valid_area=(%x,%x - %x,%x))",
									bp, bw, GSUtil::GetPSMName(psm), r.x, r.y, r.z, r.w,
									t->m_TEX0.TBP0, t->m_TEX0.TBW, GSUtil::GetPSMName(t->m_TEX0.PSM),
									t->m_valid.x, t->m_valid.y, t->m_valid.z, t->m_valid.w);
								continue;
							}
							else
							{
								if (!t->HasValidBitsForFormat(psm, req_color, req_alpha, t->m_TEX0.TBW == TEX0.TBW) && !(possible_shuffle && GSLocalMemory::m_psm[psm].bpp == 16 && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 32))
									continue;

								dst = t;

								found_t = true;
								x_offset = 0;
								y_offset = 0;

								if (GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::MergeTargets && GSLocalMemory::GetUnwrappedEndBlockAddress(bp, bw, psm, req_rect) > dst->m_end_block)
									continue;
								else
								{
									tex_merge_rt = false;
									break;
								}
							}
						}
					}
				}
				else if (GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets &&
				         (GSLocalMemory::m_psm[color_psm].bpp >= 16 || ( GSLocalMemory::m_psm[color_psm].bpp == 8 && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp >= 16)) &&
				         t->m_age <= 1 && (!found_t || t->m_last_draw > dst->m_last_draw) )
				{
					const u32 end_block = GSLocalMemory::GetEndBlockAddress(bp, TEX0.TBW, TEX0.PSM, r);
					const u32 adj_bp = (end_block < t->m_TEX0.TBP0 && t->UnwrappedEndBlock() > GS_MAX_BLOCKS) ? (bp + GS_MAX_BLOCKS) : bp;
					u32 rt_tbw = std::max(1U, t->m_TEX0.TBW);
					u32 horz_page_offset = ((adj_bp - t->m_TEX0.TBP0) >> 5) % rt_tbw;

					if (GSLocalMemory::m_psm[psm].bpp == GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp && bw != rt_tbw && block_boundary_rect.height() > GSLocalMemory::m_psm[psm].pgs.y)
						continue;

					if (!possible_shuffle && std::abs(GSLocalMemory::m_psm[psm].bpp - GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp) == 16)
						continue;

					if (GSLocalMemory::m_psm[color_psm].bpp == 16 && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 32 && bw != 1 && 
					    ((t->m_TEX0.TBW < (horz_page_offset + ((block_boundary_rect.z + GSLocalMemory::m_psm[psm].pgs.x - 1) / GSLocalMemory::m_psm[psm].pgs.x)) ||
					      (t->m_TEX0.TBW != bw && block_boundary_rect.w > GSLocalMemory::m_psm[psm].pgs.y))))
					{
						DbgCon.Warning("BP %x - 16bit bad match for target bp %x bw %d src %d format %d", adj_bp, t->m_TEX0.TBP0, t->m_TEX0.TBW, bw, t->m_TEX0.PSM);
						continue;
					}
					else if (!possible_shuffle && GSLocalMemory::m_psm[psm].trbpp <= 8 &&
					         (GSUtil::GetChannelMask(t->m_TEX0.PSM) != 0xF ||
					          ((GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp != 16 || GSLocalMemory::m_psm[psm].bpp < 16) &&
					           (!(block_boundary_rect.w <= GSLocalMemory::m_psm[psm].pgs.y &&
					              ((GSLocalMemory::m_psm[psm].bpp == 32) ? bw : ((bw + 1) / 2)) <= t->m_TEX0.TBW) &&
					            !(((GSLocalMemory::m_psm[psm].bpp == 32) ? bw : ((bw + 1) / 2)) == rt_tbw)))))
					{
						DbgCon.Warning("BP %x - 8bit bad match for target bp %x bw %d src %d format %d", adj_bp, t->m_TEX0.TBP0, t->m_TEX0.TBW, bw, t->m_TEX0.PSM);
						continue;
					}
					else if (!possible_shuffle && GSLocalMemory::m_psm[psm].bpp <= 8 && TEX0.TBW == 1)
					{
						DbgCon.Warning("Too small for relocation, skipping");
						continue;
					}

					GSVector4i rect = block_boundary_rect;
					u32 src_bw = bw;
					u32 src_psm = psm;

					if ((tex_color_psm & 0xF) <= PSMCT24 && (psm & 0x7) == PSMCT16)
					{
						if (possible_shuffle)
						{
							src_psm = t->m_TEX0.PSM;
							if (src_bw == (rt_tbw * 2))
							{
								src_bw = rt_tbw;

								rect.x /= 2;
								rect.z /= 2;
							}
							else
							{
								rect.y /= 2;
								rect.w /= 2;
							}
						}
						else
							continue;
					}
					if (adj_bp > t->m_TEX0.TBP0)
					{
						if (!region.HasEither() && GSLocalMemory::m_psm[psm].bpp == 32 && (t->m_TEX0.TBW - (((adj_bp - t->m_TEX0.TBP0) >> 5) % rt_tbw)) < static_cast<u32>((block_boundary_rect.width() + 63) / 64))
						{
							DbgCon.Warning("Bad alignmenet");
							continue;
						}

						if (!possible_shuffle && !t->Inside(adj_bp, bw, psm, block_boundary_rect))
							continue;

						GSVector4i new_rect = (GSLocalMemory::m_psm[color_psm].bpp != GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp && (psm & 0x7) != PSMCT16) ? block_boundary_rect : rect;

						const u32 rt_tbw = (possible_shuffle || bw == 1 || GSUtil::GetChannelMask(psm) != 0x8 || frame.FBW <= bw || frame.FBW == t->m_TEX0.TBW || bw == t->m_TEX0.TBW) ? t->m_TEX0.TBW : frame.FBW;

						const bool can_translate = CanTranslate(adj_bp, bw, src_psm, new_rect, t->m_TEX0.TBP0, t->m_TEX0.PSM, rt_tbw);
						if (can_translate)
						{
							const bool swizzle_match = GSLocalMemory::m_psm[src_psm].depth == GSLocalMemory::m_psm[t->m_TEX0.PSM].depth;
							const GSVector2i& page_size = GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs;
							const GSVector4i page_mask(GSVector4i((page_size.x - 1), (page_size.y - 1)).xyxy());
							rect = new_rect & ~page_mask;

							if (swizzle_match)
							{
								rect = TranslateAlignedRectByPage(t->m_TEX0.TBP0, t->m_end_block, rt_tbw, t->m_TEX0.PSM, t->m_valid, adj_bp, src_psm, bw, new_rect);
								rect.x -= new_rect.x;
								rect.y -= new_rect.y;
							}
							else
							{
								if (GSLocalMemory::m_psm[psm].bpp != GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp)
								{
									const GSVector2i& dst_page_size = GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs;
									new_rect = GSVector4i(new_rect.x / page_size.x, new_rect.y / page_size.y, (new_rect.z + (page_size.x - 1)) / page_size.x, (new_rect.w + (page_size.y - 1)) / page_size.y);
									new_rect = GSVector4i(new_rect.x * dst_page_size.x, new_rect.y * dst_page_size.y, new_rect.z * dst_page_size.x, new_rect.w * dst_page_size.y);
								}
								else
								{
									new_rect.x &= ~(page_size.x - 1);
									new_rect.y &= ~(page_size.y - 1);
									new_rect.z = (new_rect.z + (page_size.x - 1)) & ~(page_size.x - 1);
									new_rect.w = (new_rect.w + (page_size.y - 1)) & ~(page_size.y - 1);
								}
								rect = TranslateAlignedRectByPage(t, adj_bp & ~((1 << 5) - 1), src_psm, bw, new_rect);
								rect.x -= new_rect.x & ~(page_size.x - 1);
								rect.y -= new_rect.y & ~(page_size.y - 1);
							}

							if (rect.rintersect(t->m_valid - GSVector4i(0, 1).xyxy()).rempty())
								continue;

							if (!t->m_dirty.empty())
							{
								const GSVector4i dirty_rect = t->m_dirty.GetTotalRect(t->m_TEX0, GSVector2i(rect.z, rect.w)).rintersect(rect);
								if (dirty_rect.eq(rect))
									continue;
							}

							if (!t->HasValidBitsForFormat(psm, req_color, req_alpha, rt_tbw == TEX0.TBW) && !(possible_shuffle && GSLocalMemory::m_psm[psm].bpp == 16 && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 32))
								continue;

							x_offset = rect.x;
							y_offset = rect.y;
							dst = t;
							tex_merge_rt = false;
							found_t = true;
							if (dst->m_TEX0.TBP0 == frame.Block() && possible_shuffle)
								break;
							else
								continue;
						}
						else
						{
							SurfaceOffset so = ComputeSurfaceOffset(adj_bp, bw, src_psm, new_rect, t);
							if (!so.is_valid && t->Wraps())
							{
								const u32 bp_unwrap = bp + GSTextureCache::MAX_BP + 0x1;
								so = ComputeSurfaceOffset(bp_unwrap, bw, src_psm, new_rect, t);
							}
							if (so.is_valid)
							{
								if (!t->HasValidBitsForFormat(psm, req_color, req_alpha, rt_tbw == TEX0.TBW) && !(possible_shuffle && GSLocalMemory::m_psm[psm].bpp == 16 && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 32))
									continue;

								dst = t;
								x_offset = so.b2a_offset.x;
								y_offset = so.b2a_offset.y;
								tex_merge_rt = false;
								found_t = true;
								if (dst->m_TEX0.TBP0 == frame.Block() && possible_shuffle)
									break;
								else
									continue;
							}
						}
					}
					else
					{

						GSVector4i inside_block_boundary_rect = r;
						inside_block_boundary_rect.x = (inside_block_boundary_rect.x + (psm_s.bs.x - 1)) & ~(psm_s.bs.x - 1);
						inside_block_boundary_rect.y = (inside_block_boundary_rect.y + (psm_s.bs.y - 1)) & ~(psm_s.bs.y - 1);
						inside_block_boundary_rect.z = inside_block_boundary_rect.z & ~(psm_s.bs.x - 1);
						inside_block_boundary_rect.w = inside_block_boundary_rect.w & ~(psm_s.bs.y - 1);
						const GSVector2i& page_size = GSLocalMemory::m_psm[src_psm].pgs;
						const GSOffset offset(GSLocalMemory::m_psm[src_psm].info, bp, bw, psm);
						const u32 offset_bp = offset.bn(region.GetMinX(), region.GetMinY());
						if (bp < t->m_TEX0.TBP0 && region.HasX() && region.HasY() &&
						    (region.GetMinX() & (page_size.x - 1)) == 0 && (region.GetMinY() & (page_size.y - 1)) == 0 &&
						    (offset.bn(region.GetMinX(), region.GetMinY()) == t->m_TEX0.TBP0 ||
						     ((offset_bp >= t->m_TEX0.TBP0) && ((((offset_bp - t->m_TEX0.TBP0) >> 5) % bw) + (rect.width() / page_size.x)) <= bw)))
						{
							GL_CACHE("TC: Target 0x%x detected in front of TBP 0x%x with %d,%d offset (%d pages)",
								t->m_TEX0.TBP0, TEX0.TBP0, region.GetMinX(), region.GetMinY(),
								(region.GetMinY() / page_size.y) * TEX0.TBW + (region.GetMinX() / page_size.x));

							if (!t->HasValidBitsForFormat(psm, req_color, req_alpha, t->m_TEX0.TBW == TEX0.TBW) && !(possible_shuffle && GSLocalMemory::m_psm[psm].bpp == 16 && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 32))
								continue;

							x_offset = ((((offset_bp - t->m_TEX0.TBP0) >> 5) % bw) * page_size.x) - region.GetMinX();
							y_offset = ((((offset_bp - t->m_TEX0.TBP0) >> 5) / bw) * page_size.y) - region.GetMinY();
							dst = t;
							tex_merge_rt = false;
							found_t = true;

							if (GSRendererHW::GetInstance()->GetCachedCtx()->FRAME.Block() == TEX0.TBP0)
							{
								pxAssert(((t->m_TEX0.TBP0 - TEX0.TBP0) % 32) == 0);
								const s32 page_offset = (t->m_TEX0.TBP0 - TEX0.TBP0) >> 5;
								GL_CACHE("TC: RT also in front of TBP, offsetting draw by %d pages and [%d,%d].", page_offset, x_offset, y_offset);
								GSRendererHW::GetInstance()->OffsetDraw(page_offset, page_offset, x_offset, y_offset);
								break;
							}

							if (dst->m_TEX0.TBP0 == frame.Block() && possible_shuffle)
								break;
							else
								continue;
						}

						else if (GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::MergeTargets && !tex_merge_rt)
						{
							dst = t;
							x_offset = 0;
							y_offset = 0;
							tex_merge_rt = true;

							found_t = false;
							if (dst->m_TEX0.TBP0 == frame.Block() && possible_shuffle)
								break;
							else
								continue;
						}
						else if (bw == t->m_TEX0.TBW && GSLocalMemory::m_psm[psm].bpp == GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp && t->Inside(bp, bw, psm, inside_block_boundary_rect))
						{
							if (!t->HasValidBitsForFormat(psm, req_color, req_alpha, true))
								continue;

							GIFRegCLAMP fake_CLAMP;
							fake_CLAMP.WMS = CLAMP_REGION_CLAMP;
							fake_CLAMP.WMT = CLAMP_REGION_CLAMP;
							fake_CLAMP.MINU = 0;
							fake_CLAMP.MINV = 0;
							fake_CLAMP.MAXV = std::min(static_cast<u32>(1u << TEX0.TH), 1022u);
							fake_CLAMP.MAXU = std::min(static_cast<u32>(1u << TEX0.TW), 1022u);
							region = SourceRegion::Create(TEX0, fake_CLAMP);

							const GSVector4i custom_offset_rect = TranslateAlignedRectByPage(t, bp, psm, bw, block_boundary_rect);
							x_offset = custom_offset_rect.x;
							y_offset = custom_offset_rect.y;
							dst = t;
							tex_merge_rt = false;
							found_t = true;
						}
					}
				}
			}
		}

		if (!found_t && !dst && !GSConfig.UserHacks_DisableDepthSupport && !target_bp_hit_outside_valid_area)
		{
			if (is_color)
			{
				for (auto t : m_dst[DepthStencil])
				{
					if (t->m_age <= 1 && t->m_used && t->m_dirty.empty() && GSUtil::HasSharedBits(psm, t->m_TEX0.PSM) && t->Inside(bp, bw, psm, block_boundary_rect))
					{
						GL_INS("TC: Warning depth format read as color format. Pixels will be scrambled");
						if (psm_s.bpp > 8)
						{
							GIFRegTEX0 depth_TEX0;
							depth_TEX0.U32[0] = TEX0.U32[0] | (0x30u << 20u);
							depth_TEX0.U32[1] = TEX0.U32[1];
							src = LookupDepthSource(false, depth_TEX0, TEXA, CLAMP, block_boundary_rect, possible_shuffle, linear, frame, req_color, req_alpha);

							if (src != nullptr)
							{

								if (TEX0.PSM == PSMT8H)
								{
									AttachPaletteToSource(src, psm_s.pal, true, true);
								}

								return src;
							}
						}
						else
						{
							if (!possible_shuffle && TEX0.PSM == PSMT8 && (GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp != 32 || !(t->m_valid_alpha_high && t->m_valid_alpha_low && t->m_valid_rgb)))
							{
								continue;
							}
							else
							{
								src = LookupDepthSource(false, TEX0, TEXA, CLAMP, block_boundary_rect, possible_shuffle, linear, frame, req_color, req_alpha, true);

								if (src != nullptr)
								{
									if (TEX0.PSM == PSMT8H)
									{
										AttachPaletteToSource(src, psm_s.pal, true, true);
									}

									return src;
								}
							}
						}
					}
				}
			}
		}

		if (tex_merge_rt)
			src = CreateMergedSource(TEX0, TEXA, region, dst->m_scale);
	}

	GSVector4i rect = r;

	if (!src)
	{
#ifdef ENABLE_OGL_DEBUG
		if (dst)
		{
			GL_CACHE("TC: dst %s hit (OFF <%d,%d>): (0x%x, %s)",
				to_string(dst->m_type),
				x_offset,
				y_offset,
				TEX0.TBP0,
				GSUtil::GetPSMName(TEX0.PSM));
		}
		else
		{
			GL_CACHE("TC: src miss (0x%x, 0x%x, %s)", TEX0.TBP0, psm_s.pal > 0 ? TEX0.CBP : 0, GSUtil::GetPSMName(TEX0.PSM));
		}
#endif
		GIFRegTEX0 src_TEX0 = TEX0;
		if (possible_shuffle && !dst && psm_s.bpp == 16)
		{
			if (frame.FBW == src_TEX0.TBW && frame.FBW <= 14)
			{
				rect.y /= 2;
				rect.w /= 2;

				if (region.HasY())
				{
					const u32 min_y = region.GetMinY() / 2;
					const u32 max_y = region.GetMaxY() / 2;

					region.ClearY();
					region.SetY(min_y, max_y);
				}
			}
			else
			{
				rect.x /= 2;
				rect.z /= 2;

				if (region.HasX())
				{
					const u32 min_x = region.GetMinX() / 2;
					const u32 max_x = region.GetMaxX() / 2;

					region.ClearX();
					region.SetX(min_x, max_x);
				}
			}
			if (TEX0.TBP0 == frame.Block())
			{
				GIFRegTEX0 target_TEX0;
				target_TEX0.TBP0 = frame.Block();
				target_TEX0.PSM = PSMCT32;
				target_TEX0.TBW = frame.FBW;

				if (target_TEX0.TBW > 14)
					target_TEX0.TBW /= 2;

				dst = g_texture_cache->CreateTarget(target_TEX0, GSVector2i(rect.z, rect.w), GSVector2i(rect.z, rect.w), GSRendererHW::GetInstance()->GetUpscaleMultiplier(),
					GSLocalMemory::m_psm[TEX0.PSM].depth ? DepthStencil : RenderTarget, true, 0, false, true, possible_shuffle, rect, nullptr);
			}
			else
			{
				src_TEX0.PSM = PSMCT32;
			}
		}

		src = CreateSource(src_TEX0, TEXA, CLAMP, dst, x_offset, y_offset, lod, &rect, gpu_clut, region, target_bp_hit_outside_valid_area && TEX0.PSM != PSMT8);
		if (!src) [[unlikely]]
			return nullptr;
	}
	else
	{
		GL_CACHE("TC: src hit: (0x%x, 0x%x, %s)",
			TEX0.TBP0, psm_s.pal > 0 ? TEX0.CBP : 0,
			GSUtil::GetPSMName(TEX0.PSM));

		if (!src->m_palette && src->m_target && src->m_from_target)
		{
			src->m_valid_alpha_minmax = true;
			if (src->m_target_direct)
				src->m_scale = src->m_from_target->GetScale();

			if ((src->m_TEX0.PSM & 0xf) == PSMCT24)
			{
				src->m_alpha_minmax.first = TEXA.AEM ? 0 : TEXA.TA0;
				src->m_alpha_minmax.second = TEXA.TA0;
			}
			else
			{
				src->m_alpha_minmax.first = src->m_from_target->m_alpha_min;
				src->m_alpha_minmax.second = src->m_from_target->m_alpha_max;

				if (!src->m_32_bits_fmt)
				{
					const bool using_both = (src->m_alpha_minmax.first ^ src->m_alpha_minmax.second) & 128;
					const bool using_ta1 = (src->m_alpha_minmax.second & 128);

					src->m_alpha_minmax.first = TEXA.AEM ? 0 : (using_both ? std::min(TEXA.TA1, TEXA.TA0) : (using_ta1 ? TEXA.TA1 : TEXA.TA0));
					src->m_alpha_minmax.second = (using_both ? std::max(TEXA.TA1, TEXA.TA0) : (using_ta1 ? TEXA.TA1 : TEXA.TA0));
				}
			}
		}

		if (src->m_from_target && src->m_target_direct)
		{
			GSVector4i req_rect = r;

			req_rect.x = region.HasX() ? region.GetMinX() : 0;
			req_rect.y = region.HasY() ? region.GetMinY() : 0;

			const bool dirty_overlap = !src->m_from_target->m_dirty.GetTotalRect(src->m_from_target->m_TEX0, src->m_from_target->m_unscaled_size).rintersect(req_rect).rempty();

			if (dirty_overlap)
				src->m_from_target->Update();

			if (src->m_region.HasEither())
			{
				if (src->m_from_target->m_TEX0.TBP0 == src->m_TEX0.TBP0)
				{
					src->m_region.bits = 0;
					src->m_region.SetX(0, region.HasX() ? region.GetMaxX() : (1 << TEX0.TW));
					src->m_region.SetY(0, region.HasY() ? region.GetMaxY() : (1 << TEX0.TH));
				}
				else if (src->m_TEX0.TBP0 > src->m_from_target->m_TEX0.TBP0)
				{
					GSVector4i dst_offset = TranslateAlignedRectByPage(src->m_from_target, src->m_TEX0.TBP0, src->m_TEX0.PSM, src->m_TEX0.TBW, GSVector4i(0, 0, 1, 1), false);
					src->m_region.bits = 0;
					src->m_region.SetX(dst_offset.x, dst_offset.x + (region.HasX() ? std::min(region.GetMaxX(), (1 << TEX0.TW)) : (1 << TEX0.TW)));
					src->m_region.SetY(dst_offset.y, dst_offset.y + (region.HasY() ? std::min(region.GetMaxY(), (1 << TEX0.TH)) : (1 << TEX0.TH)));
				}
			}
		}

		if (gpu_clut)
			AttachPaletteToSource(src, gpu_clut);
		else if (src->m_palette && (!src->m_palette_obj || !src->ClutMatch({clut, psm_s.pal})))
			AttachPaletteToSource(src, psm_s.pal, true, true);
	}

	src->Update(rect);
	return src;
}

GSTextureCache::Target* GSTextureCache::FindTargetOverlap(Target* target, int type, int psm)
{
	for (auto t : m_dst[type])
	{
		if (t != target && t->m_TEX0.TBW == target->m_TEX0.TBW && t->m_TEX0.TBP0 >= target->m_TEX0.TBP0 &&
			t->UnwrappedEndBlock() <= target->UnwrappedEndBlock() && GSUtil::HasCompatibleBits(t->m_TEX0.PSM, psm))
			return t;
	}
	return nullptr;
}

GSVector2i GSTextureCache::ScaleRenderTargetSize(const GSVector2i& sz, float scale)
{
	return GSVector2i(static_cast<int>(std::ceil(static_cast<float>(sz.x) * scale)),
		static_cast<int>(std::ceil(static_cast<float>(sz.y) * scale)));
}

void GSTextureCache::CombineAlignedInsideTargets(Target* target, GSTextureCache::Source* src)
{
	if (GSConfig.UserHacks_TextureInsideRt < GSTextureInRtMode::InsideTargets)
		return;

	auto& list = m_dst[target->m_type];

	for (auto i = list.begin(); i != list.end();)
	{
		Target* t = *i;

		if (t != target)
		{
			if (t->m_TEX0.TBP0 < target->m_TEX0.TBP0 || t->UnwrappedEndBlock() > target->UnwrappedEndBlock())
			{
				i++;
				continue;
			}
			if ((t->m_TEX0.TBW == target->m_TEX0.TBW || t->m_TEX0.TBW == 1) && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[target->m_TEX0.PSM].bpp)
			{
				const GSLocalMemory::psm_t& t_psm = GSLocalMemory::m_psm[target->m_TEX0.PSM];
				const u32 page_offset = ((t->m_TEX0.TBP0 - target->m_TEX0.TBP0) >> 5) % std::max(1U, target->m_TEX0.TBW);
				const u32 page_width = (t->m_valid.z + (t_psm.pgs.x - 1)) / t_psm.pgs.x;

				if ((page_offset + page_width) <= target->m_TEX0.TBW)
				{
					if (t->m_last_draw > target->m_last_draw || t->m_valid.rintersect(target->m_valid).rempty())
					{
						if (!t->m_drawn_since_read.rempty())
						{
							t->Update();

							const u32 vertical_offset = (((t->m_TEX0.TBP0 - target->m_TEX0.TBP0) >> 5) / std::max(1U, target->m_TEX0.TBW)) * t_psm.pgs.y;
							const u32 horizontal_offset = page_offset * t_psm.pgs.x;
							const GSVector4i target_drect_unscaled = t->m_drawn_since_read + GSVector4i(horizontal_offset, vertical_offset).xyxy();

							const GSVector4 source_rect = GSVector4(t->m_drawn_since_read) / GSVector4(t->m_unscaled_size).xyxy();
							const GSVector4 target_drect = GSVector4(target_drect_unscaled) * target->m_scale;

							const bool valid_color = t->m_valid_rgb;
							const bool valid_alpha = (t->m_valid_alpha_high || t->m_valid_alpha_low) && (GSUtil::GetChannelMask(t->m_TEX0.PSM) & 0x8);

							GL_CACHE("Combining %x-%x in to %x-%x draw %lld", t->m_TEX0.TBP0, t->m_end_block, target->m_TEX0.TBP0, target->m_end_block, GSState::s_n);

							if (target->m_type == RenderTarget)
							{
								g_gs_device->StretchRectAutoMask(t->m_texture, source_rect, target->m_texture,
									target_drect, valid_color, valid_color, valid_color, valid_alpha);
							}
							else
							{
								if (!valid_color || (!valid_alpha && (GSUtil::GetChannelMask(t->m_TEX0.PSM) & 0x8)))
									GL_CACHE("Warning: CombineAlignedInsideTargets: Depth copy with invalid lower 24 bits or invalid upper 8 bits.");
								g_gs_device->StretchRectAuto(t->m_texture, source_rect, target->m_texture, target_drect, Nearest);
							}

							target->m_valid_rgb |= t->m_valid_rgb;
							target->m_valid_alpha_high |= t->m_valid_alpha_high;
							target->m_valid_alpha_low |= t->m_valid_alpha_low;

							target->UpdateValidity(target_drect_unscaled);
							target->UpdateDrawn(target_drect_unscaled);
						}
					}

					if (src && src->m_from_target == t)
					{
						src->m_texture = t->m_texture;
						src->m_from_target = target;
						src->m_shared_texture = false;
						src->m_target_direct = false;
						t->m_texture = nullptr;
					}

					InvalidateSourcesFromTarget(t);
					i = list.erase(i);
					delete t;

					continue;
				}
			}
		}
		i++;
	}
}

GSTextureCache::RescaleHelper::RescaleHelper(const GSVector2i& size, float scale)
	: m_size(size)
	, m_scale(scale)
{
}

void GSTextureCache::RescaleHelper::CalcRescale(const Target* tgt)
{
	m_clear = (m_size.x > tgt->m_unscaled_size.x || m_size.y > tgt->m_unscaled_size.y);
	SetNewSize(m_size.max(tgt->m_unscaled_size), m_scale);
	m_dRect = (GSVector4(GSVector4i::loadh(tgt->m_unscaled_size)) * GSVector4(m_scale)).ceil();
	GL_INS("TC: Rescale: %dx%d: %dx%d @ %f -> %dx%d @ %f",
		tgt->m_unscaled_size.x, tgt->m_unscaled_size.y,
		tgt->m_texture->GetWidth(), tgt->m_texture->GetHeight(), tgt->m_scale,
		m_new_scaled_size.x, m_new_scaled_size.y, m_scale);
}

void GSTextureCache::RescaleHelper::SetNewSize(const GSVector2i& new_size, float scale)
{
	m_new_size = new_size;
	m_new_scaled_size = ScaleRenderTargetSize(new_size, scale);
}

GSTextureCache::Target* GSTextureCache::LookupDrawTarget(GIFRegTEX0 TEX0, const GSVector2i& size, float scale, int type,
	bool used, u32 fbmask, bool preload, bool preserve_rgb, bool preserve_alpha, const GSVector4i draw_rect,
	bool is_shuffle, bool possible_clear, bool preserve_scale, GSTextureCache::Source* src, GSTextureCache::Target* ds, int offset)
{
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[TEX0.PSM];
	const u32 bp = TEX0.TBP0;
	
	RescaleHelper rescaler(size, scale);

	Target* dst = nullptr;
	Target* dst_match = nullptr;
	auto* list = &m_dst[type];

	const GSVector4i min_rect = draw_rect.max_u32(GSVector4i(0, 0, draw_rect.x, draw_rect.y));

	for (int iteration = 0; iteration < 2; iteration++)
	{
		if (dst != nullptr)
			break;

		auto& new_dst = iteration == 0 ? dst : dst_match;
		list = &m_dst[iteration == 0 ? type : (1 - type)];
		for (auto i = list->begin(); i != list->end();)
		{
			Target* t = *i;
			if (bp == t->m_TEX0.TBP0)
			{
				bool can_use = true;

				if (new_dst && ((GSState::s_n - new_dst->m_last_draw) < (GSState::s_n - t->m_last_draw) && new_dst->m_TEX0.TBP0 <= bp))
				{
					DevCon.Warning("Ignoring target at %x as one at %x is newer", t->m_TEX0.TBP0, new_dst->m_TEX0.TBP0);
					i++;
					continue;
				}
					
				if (iteration == 1)
				{
					const u32 valid_mask = (t->m_valid_rgb ? 0x7 : 0x0) | ((t->m_valid_alpha_low || t->m_valid_alpha_high) ? 0x8 : 0x0);
					if ((!(valid_mask & GSUtil::GetChannelMask(TEX0.PSM)) || (!is_shuffle && TEX0.TBW < (t->m_TEX0.TBW / 2))) || 
						(!is_shuffle && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp != GSLocalMemory::m_psm[TEX0.PSM].bpp))
					{
						if (!preserve_rgb && !preserve_alpha && (!src || src->m_from_target != t) && (valid_mask & GSUtil::GetChannelMask(TEX0.PSM)))
						{
							InvalidateSourcesFromTarget(t);
							i = list->erase(i);
							delete t;
						}
						else
							i++;
						continue;
					}
				}

				if (((!preserve_rgb && !preserve_alpha) || (t->m_was_dst_matched && fbmask == 0xffffff)) && TEX0.TBW != t->m_TEX0.TBW)
				{
					if (TEX0.TBW > 1 && (t->m_age >= 1 || (type == RenderTarget && draw_rect.w > GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs.y && TEX0.TBW < t->m_TEX0.TBW)))
					{
						can_use = false;
					}
					else if (!t->m_dirty.empty())
					{
						const GSVector4i size_rect = GSVector4i::loadh(size);
						can_use = !t->m_dirty.GetTotalRect(TEX0, size).rintersect(size_rect).eq(size_rect);
					}
				}
				else if (type == RenderTarget && (fbmask == 0xffffff && !t->m_was_dst_matched && TEX0.TBW != t->m_TEX0.TBW))
				{
					auto& rev_list = m_dst[1 - type];
					for (auto j = rev_list.begin(); j != rev_list.end(); ++j)
					{
						Target* ds = *j;

						if (t->m_TEX0.TBP0 != ds->m_TEX0.TBP0 || !ds->m_valid_rgb || TEX0.TBW != ds->m_TEX0.TBW)
							continue;

						t->m_was_dst_matched = true;
						t->m_valid_rgb = false;
						break;
					}
				}
				bool dirtied_area = t->m_dirty.size() >= 1;

				if (!is_shuffle && dirtied_area)
				{
					const u32 draw_start = GSLocalMemory::GetStartBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, draw_rect);
					const u32 draw_end = GSLocalMemory::GetEndBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, draw_rect);

					const GSVector4i dirty_rect = t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size);
					const u32 dirty_start = GSLocalMemory::GetStartBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, dirty_rect);
					const u32 dirty_end = GSLocalMemory::GetEndBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, dirty_rect);

					if (dirty_end < draw_end || dirty_start > draw_start)
						dirtied_area = false;
				}

				if (can_use && ((!is_shuffle && dirtied_area) || (is_shuffle && src && GSLocalMemory::m_psm[src->m_TEX0.PSM].bpp == 8 && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == 16)) && ((preserve_alpha && preserve_rgb) || (draw_rect.w > GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs.y && !possible_clear)) && TEX0.TBW != t->m_TEX0.TBW)
				{
					can_use = false;
				}

				if (can_use)
				{
					if (used)
						list->MoveFront(i.Index());

					new_dst = t;
					new_dst->m_32_bits_fmt |= (psm_s.bpp != 16);

					break;
				}
				else if (!(src && src->m_from_target == t))
				{
					GL_INS("TC: Deleting RT BP 0x%x BW %d PSM %s due to change in target", t->m_TEX0.TBP0, t->m_TEX0.TBW, GSUtil::GetPSMName(t->m_TEX0.PSM));
					InvalidateSourcesFromTarget(t);
					i = list->erase(i);
					delete t;

					continue;
				}
			}
			else if (!min_rect.rempty() && GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets)
			{
				const GSVector4i adjusted_valid = GSVector4i(t->m_valid.x, t->m_valid.y, std::min(t->m_valid.z, static_cast<int>(t->m_TEX0.TBW) * 64), t->m_valid.w - 1);
				const u32 adjusted_endblock = GSLocalMemory::GetEndBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, adjusted_valid);
				if (adjusted_endblock <= bp)
				{
					i++;
					continue;
				}
				const GSLocalMemory::psm_t& s_psm = GSLocalMemory::m_psm[TEX0.PSM];
				const u32 widthpage_offset = (std::abs(static_cast<int>(bp - t->m_TEX0.TBP0)) >> 5) % std::max(t->m_TEX0.TBW, 1U);
				const bool is_aligned_ok = widthpage_offset == 0 || ((min_rect.width() <= static_cast<int>((t->m_TEX0.TBW - widthpage_offset) * 64) && (t->m_TEX0.TBW == TEX0.TBW || TEX0.TBW == 1)) && bp >= t->m_TEX0.TBP0);
				const bool no_target_or_newer = (!new_dst || ((GSState::s_n - new_dst->m_last_draw) < (GSState::s_n - t->m_last_draw)));
				const bool width_match = (t->m_TEX0.TBW == TEX0.TBW || (TEX0.TBW == 1 && draw_rect.w <= GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs.y));
				const bool ds_offset = !ds || offset != 0;
				const bool is_double_buffer = TEX0.TBP0 == ((((t->m_end_block + 1) - t->m_TEX0.TBP0) / 2) + t->m_TEX0.TBP0);
				const bool source_match = src && src->m_TEX0.TBP0 <= bp && src->m_end_block > bp && src->m_TEX0.TBW == TEX0.TBW && src->m_from_target && src->m_from_target == t && t->Inside(bp, TEX0.TBW, TEX0.PSM, min_rect);
				const bool was_used_last_draw = t->m_last_draw == (GSState::s_n - 1);
				const bool overlaps = t->Overlaps(bp, TEX0.TBW, TEX0.PSM, min_rect) || (is_shuffle && src && GSLocalMemory::m_psm[src->m_TEX0.PSM].bpp == 8 && t->Overlaps(bp, TEX0.TBW, TEX0.PSM, min_rect + GSVector4i(0, 0, 0, s_psm.pgs.y - (min_rect.w & (s_psm.pgs.y - 1)))));
				if (source_match || (no_target_or_newer && is_aligned_ok && width_match && overlaps && (is_shuffle || ds_offset || is_double_buffer || was_used_last_draw)))
				{
					if (!is_shuffle && (ds && offset == 0 && (t->m_valid.w >= 896) && ((((t->m_end_block + 1) - t->m_TEX0.TBP0) >> 1) + t->m_TEX0.TBP0) <= bp))
					{
						const u32 local_offset = (((bp - t->m_TEX0.TBP0) >> 5) / std::max(t->m_TEX0.TBW, 1U)) * s_psm.pgs.y;
						if ((new_dst = CreateTarget(TEX0, GSVector2i(t->m_valid.z, t->m_valid.w - local_offset), GSVector2i(t->m_valid.z, t->m_valid.w - local_offset), rescaler.m_scale, type, true, fbmask, false, false, preserve_rgb || preserve_alpha, GSVector4i::zero(), src)))
							new_dst->m_32_bits_fmt |= (psm_s.bpp != 16);

						break;
					}

					if (is_shuffle && src && src->m_TEX0.PSM == PSMT8 && GSRendererHW::GetInstance()->m_context->FRAME.FBW == 1 && t->m_last_draw != (GSState::s_n - 1) && src->m_from_target && (src->m_from_target->m_TEX0.TBP0 == src->m_TEX0.TBP0 || (((src->m_TEX0.TBP0 - src->m_from_target->m_TEX0.TBP0) >> 5) % std::max(src->m_from_target->m_TEX0.TBW, 1U) == 0)) && widthpage_offset && src->m_from_target != t)
					{
						if (iteration == 0)
						{
							GL_INS("TC: Deleting RT BP 0x%x BW %d PSM %s offset overwrite shuffle", t->m_TEX0.TBP0, t->m_TEX0.TBW, GSUtil::GetPSMName(t->m_TEX0.PSM));
							InvalidateSourcesFromTarget(t);
							i = list->erase(i);
							delete t;
						}
						else
							i++;
						continue;
					}

					if (!is_shuffle && (!GSUtil::HasSameSwizzleBits(t->m_TEX0.PSM, TEX0.PSM) ||
											((widthpage_offset % std::max(t->m_TEX0.TBW, 1U)) != 0 && ((widthpage_offset + (min_rect.width() + (s_psm.pgs.x - 1)) / s_psm.pgs.x)) > t->m_TEX0.TBW)))
					{
						const int page_offset = TEX0.TBP0 - t->m_TEX0.TBP0;
						const int number_pages = page_offset / 32;
						const u32 tbw = std::max(t->m_TEX0.TBW, 1u);
						const int row_offset = number_pages / tbw;
						const int page_height = GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs.y;
						const int vertical_position = row_offset * page_height;

						if (src && src->m_from_target == t && src->m_target_direct && vertical_position >= t->m_valid.w / 2)
						{
							src->m_valid_rect.w = std::min(vertical_position, src->m_valid_rect.w);
							t->m_valid.w = std::min(vertical_position, t->m_valid.w);
							t->ResizeValidity(t->m_valid);
							t->ResizeDrawn(t->m_valid);
							++i;
						}
						else
						{
							if (iteration == 0)
							{
								GL_INS("TC: Deleting RT BP 0x%x BW %d PSM %s due to change in target", t->m_TEX0.TBP0, t->m_TEX0.TBW, GSUtil::GetPSMName(t->m_TEX0.PSM));
								InvalidateSourcesFromTarget(t);
								i = list->erase(i);
								delete t;
							}
							else
								i++;
						}

						continue;
					}

					GSVector4i lookup_rect = min_rect;

					if (is_shuffle)
						lookup_rect = lookup_rect & GSVector4i(~8);

					const GSVector4i translated_rect = GSVector4i(0, 0, 0, 0).max_i32(TranslateAlignedRectByPage(t, TEX0.TBP0, TEX0.PSM, TEX0.TBW, lookup_rect));
					const GSVector4i dirty_rect = t->m_dirty.empty() ? GSVector4i::zero() : t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size);
					const bool all_dirty = dirty_rect.eq(t->m_valid);


					if (!is_shuffle && !dirty_rect.rempty() && (!preserve_alpha && !preserve_rgb) && (GSState::s_n - 3) > t->m_last_draw)
					{
						GL_INS("TC: Deleting RT BP 0x%x BW %d PSM %s due to dirty areas not preserved (Likely change in target)", t->m_TEX0.TBP0, t->m_TEX0.TBW, GSUtil::GetPSMName(t->m_TEX0.PSM));
						InvalidateSourcesFromTarget(t);
						i = list->erase(i);
						delete t;

						continue;
					}

					if (!all_dirty && ((translated_rect.w <= t->m_valid.w) || widthpage_offset == 0 || (GSState::s_n - 3) <= t->m_last_draw))
					{
						if (TEX0.TBW == t->m_TEX0.TBW && !is_shuffle && widthpage_offset == 0 && ((min_rect.w + 63) / 64) > 1)
						{
							u32 real_start_address = GSLocalMemory::GetStartBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, t->m_drawn_since_read);
							u32 new_end_address = GSLocalMemory::GetEndBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, min_rect);

							if (real_start_address > new_end_address)
							{
								i++;
								continue;
							}
						}

						new_dst = t;
						new_dst->m_32_bits_fmt |= (psm_s.bpp != 16);

						if (used)
							list->MoveFront(i.Index());
						if (t->m_TEX0.TBP0 <= bp || GSLocalMemory::GetStartBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, min_rect) >= bp)
							break;
						else
							continue;
					}
				}
			}

			i++;
		}
	}

	if (dst)
	{
		dst = ProcessTargetAfterLookup(rescaler, dst, TEX0, size, type, used, fbmask, false, preload, preserve_rgb,
			preserve_alpha, draw_rect, is_shuffle, possible_clear, preserve_scale, src, ds, offset);
	}
	else if (!GSConfig.UserHacks_DisableDepthSupport)
	{
		if (!dst_match)
		{
			const int rev_type = (type == DepthStencil) ? RenderTarget : DepthStencil;
			auto& rev_list = m_dst[rev_type];
			for (auto i = rev_list.begin(); i != rev_list.end(); ++i)
			{
				Target* t = *i;
				if (bp != t->m_TEX0.TBP0 || (!t->m_valid_rgb && (!(GSUtil::GetChannelMask(TEX0.PSM) & 0x8) || !(t->m_valid_alpha_low || t->m_valid_alpha_high))) ||
					(!is_shuffle && t->m_TEX0.TBW != TEX0.TBW && (possible_clear || ((~GSLocalMemory::m_psm[t->m_TEX0.PSM].fmsk | fbmask) == 0xffffffff))))
				{
					continue;
				}
				if (!is_shuffle && (!ds || (ds != t)) &&
					t->m_TEX0.TBW != TEX0.TBW && TEX0.TBW != 1 && !preserve_rgb && min_rect.w > GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs.y)
				{
					if (src && src->m_target && src->m_from_target == t && src->m_target_direct)
					{
						src->m_target_direct = false;
						src->m_shared_texture = false;
						t->m_texture = nullptr;
					}

					GL_CACHE("TC: Deleting Z draw %d", GSState::s_n);
					InvalidateSourcesFromTarget(t);
					i = rev_list.erase(i);
					delete t;
					continue;
				}
				const GSLocalMemory::psm_t& t_psm_s = GSLocalMemory::m_psm[t->m_TEX0.PSM];
				if (t_psm_s.bpp != psm_s.bpp)
				{
					bool remove_target = possible_clear || (used && !is_shuffle);

					const u32 shuffle_bw = (psm_s.bpp > t_psm_s.bpp) ? (TEX0.TBW / 2u) : (TEX0.TBW * 2u);
					if (t->m_TEX0.TBW != TEX0.TBW && (t->m_TEX0.TBW != shuffle_bw && !is_shuffle))
					{
						const u32 end_block = GSLocalMemory::GetUnwrappedEndBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, draw_rect);
						if (end_block >= t->UnwrappedEndBlock())
						{
							GL_CACHE("TC: Not converting %s at %x TBW %u with end block of %x when we're drawing through %x",
								to_string(rev_type), t->m_TEX0.TBP0, t->m_TEX0.TBW, t->UnwrappedEndBlock(), end_block);
							remove_target = true;
						}
					}

					if (remove_target)
					{
						if (src && src->m_target && src->m_from_target == t && src->m_target_direct)
						{
							src->m_target_direct = false;
							src->m_shared_texture = false;
							t->m_texture = nullptr;
						}

						InvalidateSourcesFromTarget(t);
						i = rev_list.erase(i);
						delete t;
						continue;
					}
				}

				if (t->m_age == 0)
				{
					dst_match = t;
					break;
				}
				else if (t->m_age == 1 && (preserve_rgb || (preserve_alpha && (t->m_valid_alpha_low || t->m_valid_alpha_high))))
				{
					dst_match = t;
				}
			}
		}

		if (dst_match)
		{

			rescaler.CalcRescale(dst_match);

			const bool has_alpha = dst_match->HasValidAlpha();
			const bool preserve_target = (preserve_rgb || (preserve_alpha && has_alpha)) ||
			                             !draw_rect.rintersect(dst_match->m_valid).eq(dst_match->m_valid);

			bool half_width = false;

			if (GSLocalMemory::m_psm[TEX0.PSM].bpp == 32 && GSLocalMemory::m_psm[dst_match->m_TEX0.PSM].bpp == 16)
				if (dst_match->m_valid.z == ((TEX0.TBW * 64) * 2))
					half_width = true;

			rescaler.m_clear |= (!preserve_target && fbmask != 0);
			GIFRegTEX0 new_TEX0;
			new_TEX0.TBP0 = GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets ? dst_match->m_TEX0.TBP0 : TEX0.TBP0;
			new_TEX0.TBW = (!half_width) ? dst_match->m_TEX0.TBW : TEX0.TBW;
			new_TEX0.PSM = is_shuffle ? dst_match->m_TEX0.PSM : TEX0.PSM;

			dst = Target::Create(new_TEX0, rescaler.m_new_size.x, rescaler.m_new_size.y, rescaler.m_scale, type, rescaler.m_clear);
			if (!dst)
				return nullptr;

			dst->m_32_bits_fmt = dst_match->m_32_bits_fmt;
			dst->OffsetHack_modxy = dst_match->OffsetHack_modxy;
			dst->m_end_block = dst_match->m_end_block;
			dst->m_valid = dst_match->m_valid;
			dst->m_valid_alpha_low = dst_match->m_valid_alpha_low;
			dst->m_valid_alpha_high = dst_match->m_valid_alpha_high;
			dst->m_valid_rgb = dst_match->m_valid_rgb && (dst->m_TEX0.TBW == TEX0.TBW || min_rect.w <= GSLocalMemory::m_psm[dst_match->m_TEX0.PSM].pgs.y);
			dst->m_was_dst_matched = !dst_match->m_was_dst_matched;
			dst_match->m_valid_rgb = preserve_rgb;

			if (GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp == 16 && GSLocalMemory::m_psm[dst_match->m_TEX0.PSM].bpp > 16)
				dst->m_TEX0.TBW = dst_match->m_TEX0.TBW;
			else if (GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp == 32 && GSLocalMemory::m_psm[dst_match->m_TEX0.PSM].bpp == 16)
			{
				if (half_width)
					dst->m_valid.z /= 2;
				else
					dst->m_valid.w /= 2;

				dst->UpdateValidity(dst->m_valid);
			}

			const bool fmt_16_bits = (psm_s.bpp == 16 && GSLocalMemory::m_psm[dst_match->m_TEX0.PSM].bpp == 16 && !dst->m_32_bits_fmt);

			u32 src_bpp = fmt_16_bits ? 16 : 32;

			u32 dst_bpp;

			if (type == DepthStencil)
			{
				GL_CACHE("TC: Lookup Target(Depth) %dx%d, hit Color (0x%x, TBW %d, %s was %s)",
					rescaler.m_new_size.x, rescaler.m_new_size.y,
					bp, TEX0.TBW, GSUtil::GetPSMName(TEX0.PSM), GSUtil::GetPSMName(dst_match->m_TEX0.PSM));
				dst_bpp = fmt_16_bits ? 16 : psm_s.trbpp;
			}
			else
			{
				GL_CACHE("TC: Lookup Target(Color) %dx%d, hit Depth (0x%x, TBW %d, FBMSK %0x, %s was %s)",
					rescaler.m_new_size.x, rescaler.m_new_size.y, bp, TEX0.TBW, fbmask,
					GSUtil::GetPSMName(TEX0.PSM), GSUtil::GetPSMName(dst_match->m_TEX0.PSM));
				dst_bpp = fmt_16_bits ? 16 : 32;
			}

			if (!preserve_target)
			{
				GL_INS("TC: Not converting existing %s[%x] because it's fully overwritten.", to_string(!type), dst->m_TEX0.TBP0);
			}
			else
			{
				if (dst->m_TEX0.PSM != dst_match->m_TEX0.PSM)
					dst_match->Update();

				dst->m_dirty = std::move(dst_match->m_dirty);
				dst_match->m_dirty = {};
				dst->m_alpha_max = dst_match->m_alpha_max;
				dst->m_alpha_min = dst_match->m_alpha_min;

				if (dst->m_dirty.empty() || (~dst->m_dirty.GetDirtyChannels() & GSUtil::GetChannelMask(TEX0.PSM)) != 0 ||
					!dst->m_dirty.GetDirtyRect(0, TEX0, dst->GetUnscaledRect(), false).eq(dst->GetUnscaledRect()))
				{
					if (dst_match->m_texture->GetState() == GSTexture::State::Cleared)
					{
						if (type == DepthStencil)
						{
							const u32 cc = dst_match->m_texture->GetClearColor();
							const float cd = ConvertColorToDepth(cc, src_bpp, dst_bpp);
							GL_INS("TC: Convert clear color[%08X] to depth[%f]", cc, cd);
							g_gs_device->ClearDepth(dst->m_texture, cd);
						}
						else
						{
							const float cd = dst_match->m_texture->GetClearDepth();
							const u32 cc = ConvertDepthToColor(cd, dst_bpp);
							GL_INS("TC: Convert clear depth[%f] to color[%08X]", cd, cc);
							g_gs_device->ClearRenderTarget(dst->m_texture, cc);
						}
					}
					else if (dst_match->m_texture->GetState() == GSTexture::State::Dirty)
					{
						dst_match->UnscaleRTAlpha();
						g_gs_device->StretchRectAuto(dst_match->m_texture, FullSrcRect, dst->m_texture,
							rescaler.m_dRect, Nearest, src_bpp, dst_bpp);
					}
				}

				dst->Update();
			}
		}
	}

	if (dst)
	{
		dst->m_used |= used;
		dst->readbacks_since_draw = 0;

		pxAssert(dst->m_texture && dst->m_scale == rescaler.m_scale);
	}

	return dst;
}

GSTextureCache::Target* GSTextureCache::ProcessTargetAfterLookup(RescaleHelper& rescaler, Target* dst, GIFRegTEX0 TEX0, const GSVector2i& size,
	int type, bool used, u32 fbmask, bool is_frame, bool preload, bool preserve_rgb, bool preserve_alpha,
	const GSVector4i draw_rect, bool is_shuffle, bool possible_clear, bool preserve_scale,
	GSTextureCache::Source* src, GSTextureCache::Target* ds, int offset)
{
	pxAssert(dst);

	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[TEX0.PSM];
	const u32 bp = TEX0.TBP0;

	if (type == DepthStencil)
	{
		GL_CACHE("TC: Lookup Target(Depth) %dx%d (0x%x, BW:%u, %s) hit (0x%x, BW:%d, %s)", size.x, size.y, bp,
			TEX0.TBW, GSUtil::GetPSMName(TEX0.PSM), dst->m_TEX0.TBP0, dst->m_TEX0.TBW, GSUtil::GetPSMName(dst->m_TEX0.PSM));
	}
	else
	{
		GL_CACHE("TC: Lookup %s(Color) %dx%d (0x%x, BW:%u, FBMSK %08x, %s) hit (0x%x, BW:%d, %s)",
			is_frame ? "Frame" : "Target", size.x, size.y, bp, TEX0.TBW, fbmask, GSUtil::GetPSMName(TEX0.PSM),
			dst->m_TEX0.TBP0, dst->m_TEX0.TBW, GSUtil::GetPSMName(dst->m_TEX0.PSM));
	}

	if (dst->m_scale != rescaler.m_scale && (!preserve_scale || (type == RenderTarget && (is_shuffle || !dst->m_downscaled || TEX0.TBW != dst->m_TEX0.TBW))))
	{
		rescaler.CalcRescale(dst);
		GSTexture* tex = g_gs_device->CreateCompatible(dst->m_texture, rescaler.m_new_scaled_size, rescaler.m_clear);
		if (!tex)
			return nullptr;

		if (rescaler.m_scale == 1.0f && type == RenderTarget)
		{
			const u32 downsample_factor = static_cast<u32>(dst->GetScale());
			const GSVector2i clamp_min = (GSConfig.UserHacks_HalfPixelOffset != GSHalfPixelOffset::Native) ?
				GSVector2i(0, 0) :
				GSVector2i(downsample_factor, downsample_factor);

			const GSVector4 dRect = GSVector4(dst->GetUnscaledRect());

			g_gs_device->FilteredDownsampleTexture(dst->m_texture, tex, downsample_factor, clamp_min, dRect);
		}
		else
		{
			g_gs_device->StretchRectAuto(dst->m_texture, FullSrcRect, tex, rescaler.m_dRect,
				BilnIf(type == RenderTarget && !preserve_scale));
		}

		m_target_memory_usage = (m_target_memory_usage - dst->m_texture->GetMemUsage()) + tex->GetMemUsage();

		if ((!GSConfig.UserHacks_NativePaletteDraw && !dst->m_downscaled && rescaler.m_scale != 1.0f) ||
			(dst->m_scale != 1.0f && rescaler.m_scale != 1.0f))
			delete dst->m_texture;
		else
			g_gs_device->Recycle(dst->m_texture);

		dst->m_texture = tex;
		dst->m_scale = rescaler.m_scale;
		dst->m_unscaled_size = rescaler.m_new_size;
		dst->m_downscaled = rescaler.m_scale == 1.0f && g_gs_renderer->GetUpscaleMultiplier() > 1.0f;

		if (src && src->m_target && src->m_from_target == dst && src->m_shared_texture)
		{
			src->m_texture = dst->m_texture;
			src->m_scale = dst->m_scale;
		}
	}
	else if (dst->m_scale != rescaler.m_scale)
		rescaler.m_scale = dst->m_scale;

	if (type == DepthStencil && dst->m_type == DepthStencil && GSLocalMemory::m_psm[dst->m_TEX0.PSM].trbpp == 32 && GSLocalMemory::m_psm[TEX0.PSM].trbpp == 24 && dst->m_alpha_max > 0)
	{
		rescaler.CalcRescale(dst);
		GSTexture* tex = g_gs_device->CreateCompatible(dst->m_texture, rescaler.m_new_scaled_size, false);
		if (!tex)
			return nullptr;
		g_gs_device->StretchRectAuto(dst->m_texture, FullSrcRect, tex, rescaler.m_dRect, Nearest, 32, 24);
		g_gs_device->Recycle(dst->m_texture);

		dst->m_texture = tex;
		dst->m_alpha_min = 0;
		dst->m_alpha_max = 0;
	}
	else if ((used || type == GSTextureCache::DepthStencil) && (std::abs(static_cast<s16>(GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp - GSLocalMemory::m_psm[TEX0.PSM].bpp)) == 16))
	{
		dst->Update(dst->m_alpha_max <= 128);

		const bool scale_down = GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp > GSLocalMemory::m_psm[TEX0.PSM].bpp;
		bool req_copy = true;
		rescaler.SetNewSize(dst->m_unscaled_size, dst->m_scale);
		rescaler.m_dRect = (GSVector4(dst->m_valid) * GSVector4(dst->m_scale)).ceil();
		GSVector4 source_rect = GSVector4(
			static_cast<float>(dst->m_valid.x) / static_cast<float>(dst->m_unscaled_size.x),
			static_cast<float>(dst->m_valid.y) / static_cast<float>(dst->m_unscaled_size.y),
			static_cast<float>(dst->m_valid.z) / static_cast<float>(dst->m_unscaled_size.x),
			static_cast<float>(dst->m_valid.w) / static_cast<float>(dst->m_unscaled_size.y));
		if (!is_shuffle || GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp == 16)
		{
			if (scale_down)
			{
				dst->m_valid.y *= 2;
				dst->m_valid.w *= 2;
				rescaler.m_dRect.y *= 2;
				rescaler.m_dRect.w *= 2;

				if (rescaler.m_new_size.y < dst->m_valid.w)
				{
					rescaler.m_new_size.y = dst->m_valid.w;
					rescaler.SetNewSize(rescaler.m_new_size, dst->m_scale);
					req_copy = source_rect.w != 1.0f;
				}
			}
			else
			{
				rescaler.m_dRect.y /= 2;
				rescaler.m_dRect.w /= 2;
				dst->m_valid.y /= 2;
				dst->m_valid.w /= 2;
				req_copy = true;
			}
		}
		if (!is_shuffle)
		{
			GL_INS("TC: Convert to 16bit: %dx%d: %dx%d @ %f -> %dx%d @ %f",
				dst->m_unscaled_size.x, dst->m_unscaled_size.y,
				dst->m_texture->GetWidth(), dst->m_texture->GetHeight(), dst->m_scale,
				rescaler.m_new_scaled_size.x, rescaler.m_new_scaled_size.y, rescaler.m_scale);

			if (src && src->m_from_target && src->m_from_target == dst)
			{
				src->m_texture = dst->m_texture;
				src->m_target_direct = false;
				src->m_shared_texture = false;

				if (!req_copy)
					dst->ResizeTexture(rescaler.m_new_size.x, rescaler.m_new_size.y, true, true, GSVector4i(rescaler.m_dRect), true);
				else
				{
					GSTexture* tex = g_gs_device->CreateCompatible(dst->m_texture, rescaler.m_new_scaled_size, rescaler.m_clear);
					if (!tex)
						return nullptr;

					g_gs_device->StretchRectAuto(dst->m_texture, source_rect, tex, rescaler.m_dRect, Nearest);

					m_target_memory_usage = m_target_memory_usage + tex->GetMemUsage();

					dst->m_texture = tex;
					dst->m_unscaled_size = rescaler.m_new_size;
				}
			}
			else
			{
				if (!req_copy)
					dst->ResizeTexture(rescaler.m_new_size.x, rescaler.m_new_size.y, true, true, GSVector4i(rescaler.m_dRect));
				else
				{
					GSTexture* tex = g_gs_device->CreateCompatible(dst->m_texture, rescaler.m_new_scaled_size, rescaler.m_clear);
					if (!tex)
						return nullptr;

					g_gs_device->StretchRectAuto(dst->m_texture, source_rect, tex, rescaler.m_dRect, Nearest);

					m_target_memory_usage = (m_target_memory_usage - dst->m_texture->GetMemUsage()) + tex->GetMemUsage();

					g_gs_device->Recycle(dst->m_texture);

					dst->m_texture = tex;
					dst->m_unscaled_size = rescaler.m_new_size;
				}
			}
		}

		if ((!is_shuffle && (GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp != GSLocalMemory::m_psm[TEX0.PSM].bpp || GSLocalMemory::m_psm[dst->m_TEX0.PSM].depth != GSLocalMemory::m_psm[TEX0.PSM].depth)) ||
			(is_shuffle && GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp == 16))
		{
			if (GSLocalMemory::m_psm[dst->m_TEX0.PSM].depth != GSLocalMemory::m_psm[TEX0.PSM].depth || dst->m_TEX0.TBW != TEX0.TBW)
				dst->m_32_bits_fmt = GSLocalMemory::m_psm[TEX0.PSM].bpp != 16;

			if (!is_shuffle || (is_shuffle && GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp == 16))
			{
				dst->m_TEX0.PSM = TEX0.PSM;
				dst->m_TEX0.TBW = TEX0.TBW;
			}
		}
		else if (dst->m_TEX0.PSM == PSMT8H)
		{
			dst->m_TEX0.PSM = PSMCT32;
			dst->m_valid_rgb = false;
		}
	}

	const bool preserve_target = preserve_rgb || preserve_alpha;
	const u32 mask = GSLocalMemory::m_psm[TEX0.PSM].fmsk;

	if ((preserve_target || !dst->m_valid.rintersect(draw_rect).eq(dst->m_valid)) &&
		!dst->m_valid_rgb && !FullRectDirty(dst, 0x7) &&
		(GSLocalMemory::m_psm[TEX0.PSM].trbpp < 24 || ((fbmask & 0x00FFFFFFu) != 0x00FFFFFFu)))
	{
		if (!is_frame)
		{
			GL_CACHE("TC: Attempt to repopulate RGB for %s[%x]", to_string(type), dst->m_TEX0.TBP0);
			for (Target* dst_match : m_dst[1 - type])
			{
				if (dst_match->m_TEX0.TBP0 != dst->m_TEX0.TBP0 || !dst_match->m_valid_rgb)
					continue;

				dst->m_TEX0.TBW = dst_match->m_TEX0.TBW;
				dst->m_valid = dst_match->m_valid;
				dst->UpdateValidity(dst_match->m_valid);

				if (type == RenderTarget)
				{
					dst_match->m_valid_rgb = (fbmask & mask) == (mask & 0x00FFFFFFu);
					dst->m_was_dst_matched = true;
					if (!CopyRGBFromDepthToColor(dst, dst_match))
					{
						return nullptr;
					}
				}
				else
				{
					dst_match->m_valid_rgb &= (fbmask & mask) == (mask & 0x00FFFFFFu);
					dst->Update();

					if (!dst->ResizeTexture(dst_match->m_unscaled_size.x, dst_match->m_unscaled_size.y))
					{
						return nullptr;
					}

					g_gs_device->StretchRectAuto(dst_match->m_texture, GSVector4(0, 0, 1, 1),
						dst->m_texture, GSVector4(dst->GetUnscaledRect()) * GSVector4(dst->GetScale()),
						Nearest, 32, GSLocalMemory::m_psm[dst->m_TEX0.PSM].trbpp);

					dst_match->m_valid_rgb = !used;
					dst_match->m_was_dst_matched = true;
					dst->m_valid_rgb = true;
					dst->m_32_bits_fmt = dst_match->m_32_bits_fmt;
				}
				break;
			}
		}

		if (!dst->m_valid_rgb && ((fbmask & 0x00FFFFFF) & mask) != (mask & 0x00FFFFFF))
		{
			GL_CACHE("TC: Cannot find RGB target for %s[%x], clearing.", to_string(type), dst->m_TEX0.TBP0);

			GSTexture* tex = g_gs_device->CreateCompatible(dst->m_texture, true);
			if (!tex)
				return nullptr;

			std::swap(dst->m_texture, tex);
			PreloadTarget(TEX0, size, GSVector2i(dst->m_valid.z, dst->m_valid.w), is_frame, preload,
				preserve_target, draw_rect, dst, src);
			g_gs_device->StretchRectAutoMask(tex, GSVector4::cxpr(0.0f, 0.0f, 1.0f, 1.0f), dst->m_texture,
				GSVector4(dst->m_texture->GetRect()), false, false, false, true);
			g_gs_device->Recycle(tex);
			dst->m_valid_rgb = true;
		}
	}

	if (!preserve_target && draw_rect.rintersect(dst->m_valid).eq(dst->m_valid))
	{
		const bool dont_invalidate_alpha = (dst->HasValidAlpha() && (psm_s.fmt == GSLocalMemory::PSM_FMT_24 || (fbmask & 0xFF000000u) != 0));
		if (dont_invalidate_alpha)
		{
			GL_INS("TC: Preserving alpha on 24-bit/masked %s[%x] because it was previously valid.", to_string(type), dst->m_TEX0.TBP0);

			if (!dst->m_dirty.empty())
			{
				GL_INS("TC: Clearing RGB dirty list for %s[%x] because we're overwriting the whole target.", to_string(type), dst->m_TEX0.TBP0);
				for (s32 i = static_cast<s32>(dst->m_dirty.size()) - 1; i >= 0; i--)
				{
					if (!dst->m_dirty[i].rgba.c.a)
						dst->m_dirty.erase(dst->m_dirty.begin() + static_cast<size_t>(i));
				}
			}
		}
		else
		{
			if (!dst->m_dirty.empty() && bp == dst->m_TEX0.TBP0)
			{
				GL_INS("TC: Clearing dirty list for %s[%x] because we're overwriting the whole target.", to_string(type), dst->m_TEX0.TBP0);
				dst->m_dirty.clear();
			}

			GL_INS("TC: Invalidating%s target %s[%x] because it's completely overwritten.", to_string(type),
				(rescaler.m_scale > 1.0f && GSConfig.UserHacks_HalfPixelOffset >= GSHalfPixelOffset::Native) ? "[clearing] " : "", dst->m_TEX0.TBP0);
			if (rescaler.m_scale > 1.0f && GSConfig.UserHacks_HalfPixelOffset < GSHalfPixelOffset::Native)
			{
				if (dst->m_type == RenderTarget)
					g_gs_device->ClearRenderTarget(dst->m_texture, 0);
				else
					g_gs_device->ClearDepth(dst->m_texture, 0.0f);
			}
			else
			{
				g_gs_device->InvalidateRenderTarget(dst->m_texture);
			}
		}
	}

	return dst;
}

GSTextureCache::Target* GSTextureCache::CreateTarget(GIFRegTEX0 TEX0, const GSVector2i& size, const GSVector2i& valid_size, float scale, int type,
	bool used, u32 fbmask, bool is_frame, bool preload, bool preserve_target, const GSVector4i draw_rect, GSTextureCache::Source* src)
{
	{
		static const bool s_tcensus = (std::getenv("PCSX2_VR_CHAINLOG") != nullptr);
		if (s_tcensus)
			Console.WriteLn("(VR) CHAINLOG newtarget bp=0x%x type=%d %dx%d preload=%d preserve=%d frame=%d",
				TEX0.TBP0, type, size.x, size.y, preload ? 1 : 0, preserve_target ? 1 : 0, is_frame ? 1 : 0);
	}
	if (type == DepthStencil)
	{
		GL_CACHE("TC: Lookup Target(Depth) %dx%d, miss (0x%x, TBW %d, %s) draw %lld", size.x, size.y, TEX0.TBP0,
			TEX0.TBW, GSUtil::GetPSMName(TEX0.PSM), g_gs_renderer->s_n);
	}
	else
	{
		GL_CACHE("TC: Lookup %s(Color) %dx%d FBMSK %08x, miss (0x%x, TBW %d, %s) draw %lld", is_frame ? "Frame" : "Target",
			size.x, size.y, fbmask, TEX0.TBP0, TEX0.TBW, GSUtil::GetPSMName(TEX0.PSM), g_gs_renderer->s_n);
	}
	if (GSVector4i::loadh(size).rempty())
		return nullptr;

	Target* dst = Target::Create(TEX0, size.x, size.y, scale, type, true);
	if (!dst) [[unlikely]]
		return nullptr;

	const bool was_clear = PreloadTarget(TEX0, size, valid_size, is_frame, preload, preserve_target, draw_rect, dst, src);

	dst->m_is_frame = is_frame;

	dst->m_used |= used;

	if (!is_frame)
	{
		const u32 mask = GSLocalMemory::m_psm[TEX0.PSM].fmsk;
		dst->m_valid_rgb |= GSLocalMemory::m_psm[TEX0.PSM].depth || ((fbmask & 0x00FFFFFF) & mask) != (mask & 0x00FFFFFF) || (dst->m_dirty.GetDirtyChannels() & 0x7);

		auto& rev_list = m_dst[1 - type];
		for (auto j = rev_list.begin(); j != rev_list.end();)
		{
			Target* const rev_t = *j;
			if (rev_t->m_TEX0.TBP0 == dst->m_TEX0.TBP0 && GSLocalMemory::m_psm[rev_t->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp)
			{
				if (!rev_t->m_valid_rgb && dst->m_valid_rgb)
					rev_t->m_was_dst_matched = true;

				break;
			}
			++j;
		}

		const int bpp = GSLocalMemory::m_psm[TEX0.PSM].trbpp;

		if (((fbmask & 0xFF000000) & mask) != (mask & 0xFF000000) && bpp != 24)
		{
			dst->m_valid_alpha_high |= ((~(fbmask & mask) & 0xf0000000) & mask) != 0;
			dst->m_valid_alpha_low |= ((~(fbmask & mask) & 0x0f000000) & mask) != 0;

			if (bpp == 16)
				dst->m_valid_alpha_low = dst->m_valid_alpha_high;
		}
		else if ((dst->m_dirty.GetDirtyChannels() & 0x8) && bpp != 24)
		{
			if (!preload || was_clear)
			{
				dst->m_valid_alpha_high = true;
				dst->m_valid_alpha_low = true;
			}
			else
			{
				std::vector<GSState::GSUploadQueue>::reverse_iterator iter;
				const int start_transfer = g_gs_renderer->s_transfer_n;
				const u32 tex_end = GSLocalMemory::GetUnwrappedEndBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, GSVector4i::loadh(size));
				for (iter = GSRendererHW::GetInstance()->m_draw_transfers.rbegin(); iter != GSRendererHW::GetInstance()->m_draw_transfers.rend();)
				{
					const u32 transfer_end = GSLocalMemory::GetUnwrappedEndBlockAddress(iter->blit.DBP, iter->blit.DBW, iter->blit.DPSM, iter->rect);
					if (TEX0.TBP0 == iter->blit.DBP && GSUtil::HasCompatibleBits(iter->blit.DPSM, TEX0.PSM) && (iter->blit.DBW == dst->m_TEX0.TBW || (transfer_end >= tex_end && (iter->blit.DBW * 64) == iter->rect.z)))
					{
						dst->m_valid_alpha_high |= iter->blit.DPSM != PSMT4HL;
						dst->m_valid_alpha_low |= iter->blit.DPSM != PSMT4HH;
						break;
					}

					if ((start_transfer - iter->draw) > 100)
						break;

					++iter;
				}
			}
		}
		if (was_clear)
		{
			GL_INS("TC: Clear dirty list on new target %x, because it was a zero clear", TEX0.TBP0);
			dst->m_dirty.clear();
		}
	}

	dst->readbacks_since_draw = 0;

	dst->m_last_draw = GSState::s_n;

	if (dst->m_dirty.empty() && GSLocalMemory::m_psm[TEX0.PSM].depth == 0 && (GSUtil::GetChannelMask(TEX0.PSM) & 0x8))
		dst->m_rt_alpha_scale = true;
	else
		dst->m_last_draw += 1;

	CombineAlignedInsideTargets(dst, src);

	pxAssert(dst && dst->m_texture && dst->m_scale == scale);
	return dst;
}

bool GSTextureCache::PreloadTarget(GIFRegTEX0 TEX0, const GSVector2i& size, const GSVector2i& valid_size, bool is_frame,
	bool preload, bool preserve_target, const GSVector4i draw_rect, Target* dst, GSTextureCache::Source* src)
{
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[TEX0.PSM];
	const bool supported_fmt = !GSConfig.UserHacks_DisableDepthSupport || psm_s.depth == 0;
	std::optional<bool> hw_clear;
	const bool valid_draw_size = TEX0.TBW > 0 || (psm_s.pgs.x >= size.x && psm_s.pgs.y >= size.y);

	if (valid_draw_size && supported_fmt)
	{
		const GSVector4i newrect = GSVector4i::loadh(size);
		const u32 rect_end = GSLocalMemory::GetUnwrappedEndBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, newrect);

		RGBAMask rgba;
		rgba._u32 = GSUtil::GetChannelMask(TEX0.PSM);

		dst->UpdateValidity(GSVector4i::loadh(valid_size));

		if (!is_frame && !preload )
		{
			if ((preserve_target || !draw_rect.eq(GSVector4i::loadh(valid_size))) && GSRendererHW::GetInstance()->m_draw_transfers.size() > 0)
			{
				auto& transfers = GSRendererHW::GetInstance()->m_draw_transfers;
				const int last_draw = transfers.back().draw;
				GSVector4i eerect = GSVector4i::zero();

				for (auto iter = transfers.rbegin(); iter != transfers.rend(); ++iter)
				{
					if (last_draw - iter->draw > 500)
						break;

					const u32 transfer_end = GSLocalMemory::GetUnwrappedEndBlockAddress(iter->blit.DBP, iter->blit.DBW, iter->blit.DPSM, iter->rect);
					const u32 transfer_start = GSLocalMemory::GetStartBlockAddress(iter->blit.DBP, iter->blit.DBW, iter->blit.DPSM, iter->rect);
					if (transfer_end >= TEX0.TBP0 && transfer_start <= rect_end && GSUtil::HasCompatibleBits(iter->blit.DPSM, TEX0.PSM))
					{
						GSVector4i targetr = {};
						const bool can_translate = CanTranslate(iter->blit.DBP, iter->blit.DBW, iter->blit.DPSM, iter->rect, TEX0.TBP0, TEX0.PSM, TEX0.TBW);
						const bool swizzle_match = GSLocalMemory::m_psm[iter->blit.DPSM].depth == GSLocalMemory::m_psm[TEX0.PSM].depth;
						if (can_translate)
						{
							if (swizzle_match)
							{
								targetr = TranslateAlignedRectByPage(dst, iter->blit.DBP, iter->blit.DPSM, iter->blit.DBW, iter->rect, true);
							}
							else
							{
								GSVector4i new_rect = iter->rect;
								const GSVector2i page_size = GSLocalMemory::m_psm[iter->blit.DPSM].pgs;

								if (GSLocalMemory::m_psm[iter->blit.DPSM].bpp != GSLocalMemory::m_psm[TEX0.PSM].bpp)
								{
									const GSVector2i dst_page_size = GSLocalMemory::m_psm[iter->blit.DPSM].pgs;
									new_rect = GSVector4i(new_rect.x / page_size.x, new_rect.y / page_size.y, (new_rect.z + (page_size.x - 1)) / page_size.x, (new_rect.w + (page_size.y - 1)) / page_size.y);
									new_rect = GSVector4i(new_rect.x * dst_page_size.x, new_rect.y * dst_page_size.y, new_rect.z * dst_page_size.x, new_rect.w * dst_page_size.y);
								}
								else
								{
									new_rect.x &= ~(page_size.x - 1);
									new_rect.y &= ~(page_size.y - 1);
									new_rect.z = (new_rect.z + (page_size.x - 1)) & ~(page_size.x - 1);
									new_rect.w = (new_rect.w + (page_size.y - 1)) & ~(page_size.y - 1);
								}
								targetr = TranslateAlignedRectByPage(dst, iter->blit.DBP & ~((1 << 5) - 1), iter->blit.DPSM, iter->blit.DBW, new_rect, true);
							}
						}
						else
						{
							GSTextureCache::SurfaceOffsetKey sok;
							sok.elems[0].bp = iter->blit.DBP;
							sok.elems[0].bw = iter->blit.DBW;
							sok.elems[0].psm = iter->blit.DPSM;
							sok.elems[0].rect = iter->rect;
							sok.elems[1].bp = TEX0.TBP0;
							sok.elems[1].bw = TEX0.TBW;
							sok.elems[1].psm = TEX0.PSM;
							sok.elems[1].rect = newrect;

							targetr = (iter->blit.DBP == TEX0.TBP0) ? iter->rect : ComputeSurfaceOffset(sok).b2a_offset;
						}

						if (eerect.rempty())
							eerect = targetr;
						else
							eerect = eerect.runion(targetr);

						const bool transfer_is_clear = iter->transfer_type == GSRendererHW::GetInstance()->EEGS_TransferType::Clear;
						hw_clear = hw_clear.has_value() ? (hw_clear.value() && transfer_is_clear) : transfer_is_clear;

						if (iter->blit.DBP <= TEX0.TBP0 && transfer_end >= rect_end)
						{
							if (iter->transfer_type == GSRendererHW::GetInstance()->EEGS_TransferType::Clear && iter->blit.DBP == TEX0.TBP0 && iter->blit.DPSM == TEX0.PSM)
								dst->UpdateValidity(iter->rect);

							if (iter->blit.DBP == TEX0.TBP0 && transfer_end == rect_end)
								transfers.erase(iter.base() - 1);

							break;
						}

						if (eerect.rintersect(newrect).eq(newrect))
							break;
					}
				}

				eerect = eerect.rintersect(newrect);

				if (!eerect.rempty())
				{
					const GSVector4i save_rect = preserve_target ? newrect : eerect;

					GL_INS("TC: Preloading the RT DATA from updated GS Memory");

					AddDirtyRectTarget(dst, save_rect, TEX0.PSM, TEX0.TBW, rgba, GSLocalMemory::m_psm[TEX0.PSM].trbpp >= 16);
				}
			}
		}
		else
		{
			if (GSRendererHW::GetInstance()->m_draw_transfers.size() > 0)
			{
				auto& transfers = GSRendererHW::GetInstance()->m_draw_transfers;

				for (auto iter = transfers.rbegin(); iter != transfers.rend(); ++iter)
				{
					if (iter->draw < (GSState::s_n - 1))
						break;

					if (iter->transfer_type == GSRendererHW::GetInstance()->EEGS_TransferType::Clear && iter->blit.DBP == TEX0.TBP0 && iter->blit.DBW == TEX0.TBW)
					{
						hw_clear = true;
						transfers.erase(std::next(iter).base());
						break;
					}
				}
			}
			GL_INS("TC: Preloading the RT DATA");

			AddDirtyRectTarget(dst, newrect, TEX0.PSM, TEX0.TBW, rgba, GSLocalMemory::m_psm[TEX0.PSM].trbpp >= 16);
		}
	}

	const GSVector4i dst_valid = dst->m_valid.rempty() ? GSVector4i::loadh(valid_size) : dst->m_valid;
	u32 dst_end_block = GSLocalMemory::GetEndBlockAddress(dst->m_TEX0.TBP0, dst->m_TEX0.TBW, dst->m_TEX0.PSM, dst_valid);
	if (dst_end_block < dst->m_TEX0.TBP0)
		dst_end_block += GS_MAX_BLOCKS;

	if (psm_s.depth == 0)
	{
		for (int type = 0; type < 2; type++)
		{
			auto& list = m_dst[type];
			for (auto i = list.begin(); i != list.end();)
			{
				auto j = i;
				Target* old_dst = *j;

				if (dst == old_dst)
				{
					i++;
					continue;
				}

				if (old_dst->m_TEX0.PSM != dst->m_TEX0.PSM ||
					!old_dst->Overlaps(dst->m_TEX0.TBP0, dst->m_TEX0.TBW, dst->m_TEX0.PSM, dst_valid))
				{
					i++;
					continue;
				}

				const int page_diff = std::abs(static_cast<int>(old_dst->m_TEX0.TBP0 - dst->m_TEX0.TBP0)) >> 5;
				const u32 new_buffer_width = std::max(1U, dst->m_TEX0.TBW);
				const u32 old_buffer_width = std::max(1U, old_dst->m_TEX0.TBW);

				if (new_buffer_width != old_buffer_width)
				{
					i++;

					if (src && src->m_target_direct && src->m_from_target == old_dst)
						continue;

					if (dst->m_TEX0.TBP0 > old_dst->m_TEX0.TBP0 && dst_end_block >= old_dst->UnwrappedEndBlock())
					{
						const int block_diff = dst->m_TEX0.TBP0 - old_dst->m_TEX0.TBP0;
						const u32 old_pages_wide = old_buffer_width * 64 / psm_s.pgs.x;
						if ((block_diff % (32 * old_pages_wide)) == 0)
						{
							const int new_height = block_diff / (32 * old_pages_wide) * psm_s.pgs.y;
							old_dst->m_valid = old_dst->m_valid.rintersect(GSVector4i(0, 0, old_dst->GetUnscaledWidth(), new_height));
							if (old_dst->m_valid.rempty())
							{
								InvalidateSourcesFromTarget(old_dst);
								i = list.erase(j);
								delete old_dst;
							}
						}
						continue;
					}

					if (!preserve_target && old_dst->m_age > 0)
					{
						InvalidateSourcesFromTarget(old_dst);
						i = list.erase(j);
						delete old_dst;
					}

					continue;
				}

				const u32 horizontal_page_offset_start = page_diff % new_buffer_width;
				const u32 horizontal_page_offset_end = horizontal_page_offset_start + (draw_rect.width() / psm_s.pgs.x);
				if (horizontal_page_offset_start > 0 && horizontal_page_offset_end > new_buffer_width && !old_dst->m_dirty.empty())
				{
					InvalidateSourcesFromTarget(old_dst);
					i = list.erase(j);
					delete old_dst;

					continue;
				}

				const u32 old_midpoint = (((old_dst->UnwrappedEndBlock() + 1) - old_dst->m_TEX0.TBP0) >> 1) + old_dst->m_TEX0.TBP0;
				if (dst->m_TEX0.TBP0 == old_midpoint && dst->m_TEX0.TBW == old_dst->m_TEX0.TBW && dst->m_TEX0.TBW != 0)
				{
					GSVector4i new_valid = old_dst->m_valid;
					new_valid.w /= 2;
					if (preserve_target && old_dst->m_scale == dst->m_scale && dst->m_type == old_dst->m_type && !new_valid.rcontains(old_dst->m_drawn_since_read))
					{
						const GSVector4i copy_rect = GSVector4i(GSVector4((new_valid + GSVector4i(0, new_valid.w).xyxy()).rintersect(old_dst->m_drawn_since_read).rintersect(GSVector4i(0, 0, dst->m_unscaled_size.x, new_valid.w + dst->m_unscaled_size.y))) * dst->m_scale);
						bool copy_target = true;

						if (!old_dst->m_dirty.empty())
						{
							const GSVector4i old_dirty = old_dst->m_dirty.GetTotalRect(old_dst->m_TEX0, old_dst->m_unscaled_size);
							if (copy_rect.rintersect(old_dirty).eq(copy_rect))
							{
								copy_target = false;
								RGBAMask rgba;
								rgba._u32 = GSUtil::GetChannelMask(dst->m_TEX0.PSM);
								AddDirtyRectTarget(dst, copy_rect - GSVector4i(0, new_valid.w).xyxy(), dst->m_TEX0.PSM, dst->m_TEX0.TBW, rgba, GSLocalMemory::m_psm[dst->m_TEX0.PSM].trbpp >= 16);
							}
						}

						if (copy_target)
						{
							old_dst->Update();

							if (dst->m_valid.rintersect(copy_rect - GSVector4i(0, new_valid.w).xyxy()).eq(dst->m_valid))
								dst->m_dirty.clear();
							else
								dst->Update();

							dst->m_valid_rgb = old_dst->m_valid_rgb;
							dst->m_valid_alpha_low = old_dst->m_valid_alpha_low;
							dst->m_valid_alpha_high = old_dst->m_valid_alpha_high;
							dst->m_alpha_max = old_dst->m_alpha_max;
							dst->m_alpha_min = old_dst->m_alpha_min;
							dst->m_rt_alpha_scale = old_dst->m_rt_alpha_scale;

							g_gs_device->CopyRect(old_dst->m_texture, dst->m_texture, copy_rect, 0, 0);
						}
					}
					GL_INS("TC: RT resize buffer for FBP 0x%x, %dx%d => %d,%d", old_dst->m_TEX0.TBP0, old_dst->m_valid.width(), old_dst->m_valid.height(), new_valid.width(), new_valid.height());
					old_dst->ResizeValidity(new_valid);
					return hw_clear.value_or(false);
				}
				
				if (dst->m_TEX0.TBP0 < old_dst->m_TEX0.TBP0 && dst_end_block > old_dst->m_TEX0.TBP0)
				{
					const int old_pages = ((old_dst->UnwrappedEndBlock() + 1) - old_dst->m_TEX0.TBP0) >> 5;
					const int overlapping_pages = std::min(old_pages, static_cast<int>(dst_end_block - old_dst->m_TEX0.TBP0) >> 5);
					const int overlapping_pages_height = ((overlapping_pages + (new_buffer_width - 1)) / new_buffer_width) * GSLocalMemory::m_psm[old_dst->m_TEX0.PSM].pgs.y;

					if (overlapping_pages_height == 0 || (overlapping_pages % new_buffer_width))
					{
						i++;
						continue;
					}

					const int dst_offset_height = ((page_diff / new_buffer_width) * GSLocalMemory::m_psm[old_dst->m_TEX0.PSM].pgs.y);
					const int texture_height = (dst->m_TEX0.TBW == old_dst->m_TEX0.TBW) ? (dst_offset_height + old_dst->m_valid.w) : (dst_offset_height + overlapping_pages_height);

					if (texture_height > dst->m_unscaled_size.y && !dst->ResizeTexture(dst->m_unscaled_size.x, texture_height, true))
					{
						DevCon.Warning("Failed to resize target on preload? Draw %lld", GSState::s_n);
						i++;
						continue;
					}

					const int dst_offset_width = (page_diff % new_buffer_width) * GSLocalMemory::m_psm[old_dst->m_TEX0.PSM].pgs.x;
					const int dst_offset_scaled_width = dst_offset_width * dst->m_scale;
					const int dst_offset_scaled_height = dst_offset_height * dst->m_scale;
					const GSVector4i dst_rect_scale = GSVector4i(old_dst->m_valid.x, dst_offset_height, old_dst->m_valid.z, texture_height);

					if (((!hw_clear && (preserve_target || preload)) || dst_rect_scale.rintersect(draw_rect).rempty()) && dst->GetScale() == old_dst->GetScale())
					{
						int copy_width = ((old_dst->m_texture->GetWidth()) > (dst->m_texture->GetWidth()) ? (dst->m_texture->GetWidth()) : old_dst->m_texture->GetWidth()) - dst_offset_scaled_width;
						int copy_height = (texture_height - dst_offset_height) * old_dst->m_scale;

						GL_INS("TC: RT double buffer copy from FBP 0x%x, %dx%d => %d,%d", old_dst->m_TEX0.TBP0, copy_width, copy_height, 0, dst_offset_scaled_height);

						old_dst->Update();
						dst->Update();

						dst->m_valid_rgb |= old_dst->m_valid_rgb;
						dst->m_valid_alpha_low |= old_dst->m_valid_alpha_low;
						dst->m_valid_alpha_high |= old_dst->m_valid_alpha_high;

						if (copy_width < 0)
						{
							copy_width = 0;
						}

						if (!old_dst->m_valid_rgb || !(old_dst->m_valid_alpha_high || old_dst->m_valid_alpha_low) || old_dst->m_scale != dst->m_scale)
						{
							const GSVector4 src_rect = GSVector4(0, 0, copy_width, copy_height) / (GSVector4(old_dst->m_texture->GetSize()).xyxy());
							const GSVector4 dst_rect = GSVector4(dst_offset_scaled_width, dst_offset_scaled_height, dst_offset_scaled_width + copy_width, dst_offset_scaled_height + copy_height);
							g_gs_device->StretchRectAutoMask(old_dst->m_texture, src_rect, dst->m_texture, dst_rect, old_dst->m_valid_rgb, old_dst->m_valid_rgb, old_dst->m_valid_rgb, old_dst->m_valid_alpha_high || old_dst->m_valid_alpha_low);
						}
						else
						{
							if ((copy_width + dst_offset_scaled_width) > (dst->m_unscaled_size.x * dst->m_scale) || (copy_height + dst_offset_scaled_height) > (dst->m_unscaled_size.y * dst->m_scale))
							{
								copy_width = std::min(copy_width, static_cast<int>((dst->m_unscaled_size.x * dst->m_scale) - dst_offset_scaled_width));
								copy_height = std::min(copy_height, static_cast<int>((dst->m_unscaled_size.y * dst->m_scale) - dst_offset_scaled_height));
							}

							g_gs_device->CopyRect(old_dst->m_texture, dst->m_texture, GSVector4i(0, 0, copy_width, copy_height), dst_offset_scaled_width, dst_offset_scaled_height);
						}
					}

					if (src && src->m_target && src->m_from_target == old_dst)
					{
						src->m_from_target = dst;
						src->m_texture = dst->m_texture;
						src->m_region.SetY(src->m_region.GetMinY() + dst_offset_height, src->m_region.GetMaxY() + dst_offset_height);
						src->m_region.SetX(src->m_region.GetMinX() + dst_offset_width, src->m_region.GetMaxX() + dst_offset_width);
					}

					InvalidateSourcesFromTarget(old_dst);
					i = list.erase(j);
					delete old_dst;
					continue;
				}

				i++;
			}
		}
	}
	else
	{
		for (int type = 0; type < 2; type++)
		{
			auto& list = m_dst[type];
			for (auto i = list.begin(); i != list.end();)
			{
				auto j = i;
				Target* old_dst = *j;
				if (old_dst != dst && old_dst->Overlaps(dst->m_TEX0.TBP0, dst->m_TEX0.TBW, dst->m_TEX0.PSM, dst_valid) && GSUtil::HasSharedBits(dst->m_TEX0.PSM, old_dst->m_TEX0.PSM))
				{
					if (dst->m_TEX0.TBP0 > old_dst->m_TEX0.TBP0 && dst->m_TEX0.TBW == old_dst->m_TEX0.TBW &&
						((((dst->m_TEX0.TBP0 - old_dst->m_TEX0.TBP0) >> 5) % std::max(old_dst->m_TEX0.TBW, 1U)) + (dst_valid.z / 64)) <= dst->m_TEX0.TBW)
					{
						if (GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets && old_dst->Inside(dst->m_TEX0.TBP0, dst->m_TEX0.TBW, dst->m_TEX0.PSM, dst->m_valid) &&
							GSLocalMemory::m_psm[old_dst->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp)
						{
							dst->m_TEX0.TBP0 = old_dst->m_TEX0.TBP0;
							dst->m_valid = old_dst->m_valid;
							dst->m_drawn_since_read = old_dst->m_drawn_since_read;
							dst->m_end_block = old_dst->m_end_block;
							dst->m_valid_rgb = true;
							old_dst->m_valid_rgb = false;
							old_dst->m_was_dst_matched = true;

							dst->ResizeTexture(old_dst->m_unscaled_size.x, old_dst->m_unscaled_size.y);

							const u32 dst_bpp = GSLocalMemory::m_psm[dst->m_TEX0.PSM].trbpp;
							const u32 src_bpp = (dst_bpp == 16) ? 16 : 32;
							g_gs_device->StretchRectAuto(old_dst->m_texture, GSVector4(0, 0, 1, 1),
								dst->m_texture, GSVector4(old_dst->GetUnscaledRect()) * GSVector4(dst->GetScale()), Nearest, src_bpp, dst_bpp);

							break;
						}
						else
						{
							const int height_adjust = (((dst->m_TEX0.TBP0 - old_dst->m_TEX0.TBP0) >> 5) / std::max(old_dst->m_TEX0.TBW, 1U)) * GSLocalMemory::m_psm[old_dst->m_TEX0.PSM].pgs.y;

							old_dst->m_valid.w = std::min(height_adjust, old_dst->m_valid.w);
							old_dst->ResizeValidity(old_dst->m_valid);
						}
					}
					else if (dst->m_TEX0.TBP0 < old_dst->m_TEX0.TBP0 && (((old_dst->m_TEX0.TBP0 - dst->m_TEX0.TBP0) >> 5) % std::max(old_dst->m_TEX0.TBW, 1U)) == 0)
					{
						if (GSUtil::GetChannelMask(dst->m_TEX0.PSM) == 0x7 && (old_dst->m_valid_alpha_high || old_dst->m_valid_alpha_low))
						{
							if (GSLocalMemory::m_psm[dst->m_TEX0.PSM].depth == GSLocalMemory::m_psm[old_dst->m_TEX0.PSM].depth)
								old_dst->m_valid_rgb = false;

							i++;
							continue;
						}

						const int height_adjust = ((((dst_end_block + 31) - old_dst->m_TEX0.TBP0) >> 5) / std::max(old_dst->m_TEX0.TBW, 1U)) * GSLocalMemory::m_psm[old_dst->m_TEX0.PSM].pgs.y;
						bool delete_target = true;

						if (height_adjust < old_dst->m_unscaled_size.y)
						{
							old_dst->m_TEX0.TBP0 = GSLocalMemory::GetStartBlockAddress(old_dst->m_TEX0.TBP0, old_dst->m_TEX0.TBW, old_dst->m_TEX0.PSM, GSVector4i(0, height_adjust, old_dst->m_valid.z, old_dst->m_valid.w));
							old_dst->m_valid.w -= height_adjust;
							old_dst->ResizeValidity(old_dst->m_valid);

							if (!old_dst->m_valid.rempty())
							{
								delete_target = false;
								if (GSTexture* tex = g_gs_device->CreateCompatible(old_dst->m_texture, true))
								{
									g_gs_device->CopyRect(old_dst->m_texture, tex, GSVector4i(0, height_adjust * old_dst->m_scale, old_dst->m_texture->GetWidth(), old_dst->m_texture->GetHeight()), 0, 0);
									if (src && src->m_target && src->m_from_target == old_dst)
									{
										src->m_from_target = old_dst;
										src->m_texture = old_dst->m_texture;
										src->m_target_direct = false;
										src->m_shared_texture = false;
									}
									else
									{
										g_gs_device->Recycle(old_dst->m_texture);
									}
									old_dst->m_texture = tex;
								}
							}
						}
						
						if (delete_target)
						{
							if (src && src->m_target && src->m_from_target == old_dst)
							{
								src->m_from_target = nullptr;
								src->m_texture = old_dst->m_texture;
								src->m_target_direct = false;
								src->m_shared_texture = false;

								old_dst->m_texture = nullptr;
								i = list.erase(j);
								delete old_dst;
							}
							else
							{
								InvalidateSourcesFromTarget(old_dst);
								i = list.erase(j);
								delete old_dst;
							}

							continue;
						}
					}
				}
				i++;
			}
		}
	}

	return hw_clear.value_or(false);
}

GSTextureCache::Target* GSTextureCache::LookupDisplayTarget(GIFRegTEX0 TEX0, const GSVector2i& size, float scale, bool is_feedback)
{
	const u32 bp = TEX0.TBP0;

	Target* dst = nullptr;
	auto* list = &m_dst[RenderTarget];

	for (auto i = list->begin(); i != list->end(); ++i)
	{
		Target* t = *i;

		if (bp == t->m_TEX0.TBP0 && t->m_end_block >= bp)
		{
			if (TEX0.TBW != t->m_TEX0.TBW && t->m_TEX0.TBW > 1 && t->m_age > 0)
			{
				if (!t->m_dirty.empty())
				{
					const GSVector4i dirty_rect = t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size);
					if (t->m_dirty.size() == 1 && t->m_dirty[0].bw == TEX0.TBW)
					{
						t->m_TEX0.TBW = TEX0.TBW;
						t->m_valid = dirty_rect;
						t->m_end_block = GSLocalMemory::GetEndBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, t->m_valid);
						t->m_drawn_since_read = GSVector4i::zero();
					}
					else
					{
						DevCon.Warning("Wanted %x psm %x bw %x, got %x psm %x bw %x, deleting", TEX0.TBP0, TEX0.PSM, TEX0.TBW, t->m_TEX0.TBP0, t->m_TEX0.PSM, t->m_TEX0.TBW);
						InvalidateSourcesFromTarget(t);
						i = list->erase(i);
						delete t;
						continue;
					}
				}
			}
			dst = t;
			GL_CACHE("TC: Lookup Frame %dx%d, perfect hit: (0x%x -> 0x%x %s)", size.x, size.y, bp, t->m_end_block, GSUtil::GetPSMName(TEX0.PSM));
			if (size.x > 0 || size.y > 0)
				ScaleTargetForDisplay(dst, TEX0, size.x, size.y);

			break;
		}
	}

	if (!dst)
	{
		for (auto i = list->begin(); i != list->end(); ++i)
		{
			Target* t = *i;
			const u32 end_block = GSLocalMemory::GetEndBlockAddress(bp, TEX0.TBW, TEX0.PSM, GSVector4i(0, size.y, size.x, size.y + 1));
			const u32 bp_adj = (end_block < t->m_TEX0.TBP0 && t->UnwrappedEndBlock() > GS_MAX_BLOCKS) ? (bp + GS_MAX_BLOCKS) : bp;
			const bool half_buffer_match = GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets && TEX0.TBW == t->m_TEX0.TBW && TEX0.PSM == t->m_TEX0.PSM &&
				bp == GSLocalMemory::GetStartBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, GSVector4i(0, size.y, size.x, size.y + 1));
			if (t->m_TEX0.TBP0 <= bp_adj && bp_adj <= t->UnwrappedEndBlock() && (half_buffer_match || t->Inside(bp_adj, TEX0.TBW, TEX0.PSM, GSVector4i::loadh(size))))
			{
				if (dst && (GSState::s_n - dst->m_last_draw) < (GSState::s_n - t->m_last_draw))
					continue;

				if (TEX0.TBW != t->m_TEX0.TBW && t->m_TEX0.TBW > 1 && t->m_age > 0)
				{
					if (!t->m_dirty.empty())
					{
						DevCon.Warning("2 Wanted %x psm %x bw %x, got %x psm %x bw %x, deleting", TEX0.TBP0, TEX0.PSM, TEX0.TBW, t->m_TEX0.TBP0, t->m_TEX0.PSM, t->m_TEX0.TBW);
						InvalidateSourcesFromTarget(t);
						i = list->erase(i);
						delete t;
						continue;
					}
				}

				dst = t;
				GL_CACHE("TC: Lookup Frame %dx%d, inclusive hit: (0x%x, took 0x%x -> 0x%x %s)", size.x, size.y, bp, t->m_TEX0.TBP0, t->m_end_block, GSUtil::GetPSMName(TEX0.PSM));

				if (size.x > 0 || size.y > 0)
					ScaleTargetForDisplay(dst, TEX0, size.x, size.y);

				continue;
			}
		}
	}

	if (!dst)
	{
		for (auto i = list->begin(); i != list->end(); ++i)
		{
			Target* t = *i;
			if (bp == t->m_TEX0.TBP0 && TEX0.TBW == t->m_TEX0.TBW)
			{
				if (TEX0.TBW != t->m_TEX0.TBW && t->m_TEX0.TBW > 1 && t->m_age > 0)
				{
					if (!t->m_dirty.empty())
					{
						DevCon.Warning("3 Wanted %x psm %x bw %x, got %x psm %x bw %x, deleting", TEX0.TBP0, TEX0.PSM, TEX0.TBW, t->m_TEX0.TBP0, t->m_TEX0.PSM, t->m_TEX0.TBW);
						InvalidateSourcesFromTarget(t);
						i = list->erase(i);
						delete t;
						continue;
					}
				}

				dst = t;
				GL_CACHE("TC: Lookup Frame %dx%d, empty hit: (0x%x -> 0x%x %s)", size.x, size.y, bp, t->m_end_block, GSUtil::GetPSMName(TEX0.PSM));
				break;
			}
		}
	}

	if (dst)
	{
		RescaleHelper rescaler(size, scale);
		dst = ProcessTargetAfterLookup(rescaler, dst, TEX0, size, RenderTarget, true, 0, true, GSConfig.PreloadFrameWithGSData);
	}
	
	if (dst)
	{
#ifdef ENABLE_VR
		if (VR::StereoState::Get().enabled && g_gs_device->SupportsStereoTargets())
		{
			NoteDisplayChainBP(dst->m_TEX0.TBP0);
			dst->PromoteToStereo();
		}
#endif
		return dst;
	}

	bool can_create = is_feedback;
	bool exact_match = false;
	u32 exact_match_psm = 0;
	GSVector2i new_size = size;

	if (!is_feedback && GSRendererHW::GetInstance()->m_draw_transfers.size() > 0)
	{
		const GSVector4i newrect = GSVector4i::loadh(size);
		const u32 rect_end = GSLocalMemory::GetUnwrappedEndBlockAddress(TEX0.TBP0, TEX0.TBW, TEX0.PSM, newrect);

		std::vector<GSState::GSUploadQueue>::reverse_iterator iter;
		GSVector4i eerect = GSVector4i::zero();
		const u64 last_draw = GSRendererHW::GetInstance()->m_draw_transfers.back().draw;

		for (iter = GSRendererHW::GetInstance()->m_draw_transfers.rbegin(); iter != GSRendererHW::GetInstance()->m_draw_transfers.rend();)
		{
			if (last_draw - iter->draw > 500)
				break;

			const u32 transfer_end = GSLocalMemory::GetUnwrappedEndBlockAddress(iter->blit.DBP, iter->blit.DBW, iter->blit.DPSM, iter->rect);

			if (transfer_end >= TEX0.TBP0 && iter->blit.DBP <= rect_end && GSUtil::HasCompatibleBits(iter->blit.DPSM, TEX0.PSM))
			{
				const GSVector4i targetr = iter->rect;

				if (eerect.rempty())
					eerect = targetr;
				else
					eerect = eerect.runion(targetr);

				if (iter->transfer_type == GSRendererHW::GetInstance()->EEGS_TransferType::Clear && iter->draw == last_draw)
				{
					can_create = false;
					break;
				}

				if (iter->blit.DBP == TEX0.TBP0 && transfer_end == rect_end)
				{
					exact_match = true;
					exact_match_psm = iter->blit.DPSM;
					iter = std::vector<GSState::GSUploadQueue>::reverse_iterator(GSRendererHW::GetInstance()->m_draw_transfers.erase(iter.base() - 1));
				}
				else if (GSLocalMemory::GetStartBlockAddress(iter->blit.DBP, iter->blit.DBW, iter->blit.DPSM, iter->rect) <= TEX0.TBP0 && transfer_end >= rect_end && iter->rect.width() == size.x)
				{
					GSTextureCache::Target* tgt = g_texture_cache->GetExactTarget(iter->blit.DBP, iter->blit.DBW, GSTextureCache::RenderTarget, iter->blit.DBP + 1);

					if (tgt)
					{
						RGBAMask mask;
						mask._u32 = GSUtil::GetChannelMask(iter->blit.DPSM);
						tgt->UpdateValidity(iter->rect, true);
						new_size.y = iter->rect.w;
						tgt->ResizeTexture(new_size.x, new_size.y);
						AddDirtyRectTarget(tgt, iter->rect, iter->blit.DPSM, iter->blit.DBW, mask, false);
						tgt->Update();

						return tgt;
					}
				}

				can_create = true;
				break;
			}
			else
				++iter;
		}
	}

	if (can_create)
	{
		Target* tgt = CreateTarget(TEX0, new_size, new_size, scale, RenderTarget, true, 0, true);
		if (tgt && exact_match)
		{
			const u32 channel_mask = GSUtil::GetChannelMask(exact_match_psm);
			if (channel_mask & 7)
			{
				tgt->m_valid_rgb = true;
			}
			if (channel_mask & 8)
			{
				tgt->m_valid_alpha_low = true;
				tgt->m_valid_alpha_high = true;
			}
		}
		return tgt;
	}
	else
	{
		return nullptr;
	}

	if (dst)
	{
		dst->m_used = true;
		dst->readbacks_since_draw = 0;

		pxAssert(dst->m_texture && dst->m_scale == scale);
	}

	return dst;
}

void GSTextureCache::Target::ScaleRTAlpha()
{
	if (!m_rt_alpha_scale && m_type == RenderTarget)
	{
		if (m_alpha_max > 0)
		{
			const GSVector2i rtsize(m_texture->GetSize());
			const GSVector4i valid_rect = GSVector4i(GSVector4(m_valid) * GSVector4(m_scale));
			GL_PUSH("TC: ScaleRTAlpha(valid=(%dx%d %d,%d=>%d,%d))", m_valid.width(), m_valid.height(), m_valid.x, m_valid.y, m_valid.z, m_valid.w);

			if (GSTexture* temp_rt = g_gs_device->CreateCompatible(m_texture, rtsize, !GSVector4i::loadh(rtsize).eq(valid_rect)))
			{
				const GSVector4 dRect(m_texture->GetRect().rintersect(valid_rect));
				const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
				g_gs_device->StretchRect(m_texture, sRect, temp_rt, dRect, ShaderConvert::RTA_CORRECTION, Nearest);
				g_gs_device->Recycle(m_texture);
				m_texture = temp_rt;
			}
		}

		m_rt_alpha_scale = true;
	}
}

void GSTextureCache::Target::UnscaleRTAlpha()
{
	if (m_rt_alpha_scale && m_type == RenderTarget)
	{
		if (m_alpha_max > 0)
		{
			const GSVector2i rtsize(m_texture->GetSize());
			const GSVector4i valid_rect = GSVector4i(GSVector4(m_valid) * GSVector4(m_scale));
			GL_PUSH("TC: UnscaleRTAlpha(valid=(%dx%d %d,%d=>%d,%d))", valid_rect.width(), valid_rect.height(), valid_rect.x, valid_rect.y, valid_rect.z, valid_rect.w);

			if (GSTexture* temp_rt = g_gs_device->CreateCompatible(m_texture, rtsize, !GSVector4i::loadh(rtsize).eq(valid_rect)))
			{
				const GSVector4 dRect(m_texture->GetRect().rintersect(valid_rect));
				const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
				g_gs_device->StretchRect(m_texture, sRect, temp_rt, dRect, ShaderConvert::RTA_DECORRECTION, Nearest);
				g_gs_device->Recycle(m_texture);
				m_texture = temp_rt;
			}
		}

		m_rt_alpha_scale = false;
	}
}

void GSTextureCache::ScaleTargetForDisplay(Target* t, const GIFRegTEX0& dispfb, int real_w, int real_h)
{

	const int delta = dispfb.TBP0 - t->m_TEX0.TBP0;
	int y_offset = 0;
	if (delta > 0 && t->m_TEX0.TBW != 0)
	{
		const int pages = delta >> 5u;
		const int y_pages = pages / t->m_TEX0.TBW;
		y_offset = y_pages * GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs.y;
	}

	GSTexture* old_texture = t->m_texture;
	const float scale = t->m_scale;
	const int old_width = t->m_unscaled_size.x;
	const int old_height = t->m_unscaled_size.y;
	const int needed_height = std::min(real_h + y_offset, GSRendererHW::MAX_FRAMEBUFFER_HEIGHT);
	const int needed_width = std::min(real_w, static_cast<int>(dispfb.TBW * 64));
	if (needed_height <= t->m_unscaled_size.y && needed_width <= t->m_unscaled_size.x)
		return;
	const int new_height = std::max(t->m_unscaled_size.y, needed_height);
	const int new_width = std::max(t->m_unscaled_size.x, needed_width);
	const int scaled_new_height = static_cast<int>(std::ceil(static_cast<float>(new_height) * scale));
	const int scaled_new_width = static_cast<int>(std::ceil(static_cast<float>(new_width) * scale));
	GSTexture* new_texture = g_gs_device->CreateCompatible(old_texture, scaled_new_width, scaled_new_height, false);
	if (!new_texture)
	{
		return;
	}

	GL_CACHE("TC: Expanding target for display output, target height %d @ 0x%X, display %d @ 0x%X offset %d needed %d",
		t->m_unscaled_size.y, t->m_TEX0.TBP0, real_h, dispfb.TBP0, y_offset, needed_height);

	g_gs_device->StretchRectAuto(old_texture, new_texture, GSVector4(old_texture->GetSize()).zwxy(), Nearest);
	m_target_memory_usage = (m_target_memory_usage - old_texture->GetMemUsage()) + new_texture->GetMemUsage();
	g_gs_device->Recycle(old_texture);
	t->m_texture = new_texture;
	t->m_unscaled_size = GSVector2i(new_width, new_height);

	RGBAMask rgba;
	rgba._u32 = GSUtil::GetChannelMask(t->m_TEX0.PSM);
	const int preload_width = t->m_TEX0.TBW * 64;
	if (old_width < preload_width && old_height < needed_height)
	{
		const GSVector4i right(old_width, 0, preload_width, needed_height);
		const GSVector4i bottom(0, old_height, old_width, needed_height);

		t->UpdateValidity(right.rintersect(bottom));

		AddDirtyRectTarget(t, right, t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);
		AddDirtyRectTarget(t, bottom, t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);
	}
	else
	{
		const GSVector4i newrect = GSVector4i((old_height < new_height) ? 0 : old_width,
			(old_width < preload_width) ? 0 : old_height,
			preload_width, needed_height);

		t->UpdateValidity(newrect);

		AddDirtyRectTarget(t, newrect, t->m_TEX0.PSM, t->m_TEX0.TBW, rgba);
	}

	GetTargetSize(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, new_width, static_cast<u32>(needed_height));
}

float GSTextureCache::ConvertColorToDepth(u32 c, u32 src_bpp, u32 dst_bpp)
{
	const float mult = std::exp2(-32.0f);
	switch (dst_bpp)
	{
		case 32:
		default:
			pxAssert(src_bpp == 32);
			return static_cast<float>(c) * mult;

		case 24:
			pxAssert(src_bpp == 32);
			return static_cast<float>(c & 0x00FFFFFF) * mult;

		case 16:
			pxAssert(src_bpp == 16 || src_bpp == 32);
			if (src_bpp == 16)
			{
				const u32 r = (c >>  3) & 0x001Fu;
				const u32 g = (c >>  6) & 0x03E0u;
				const u32 b = (c >>  9) & 0x7C00u;
				const u32 a = (c >> 16) & 0x8000u;
				return static_cast<float>(r | g | b | a) * mult;
			}
			else
			{
				return static_cast<float>(c & 0x0000ffff) * mult;
			}
	}
}

u32 GSTextureCache::ConvertDepthToColor(float d, u32 dst_bpp)
{
	pxAssert(dst_bpp == 32 || dst_bpp == 16);
	const u32 cc = static_cast<u32>(d * 0x1p32);
	if (dst_bpp == 16)
	{
		const u32 r = (cc <<  3) & 0x000000FF;
		const u32 g = (cc <<  6) & 0x0000F800;
		const u32 b = (cc <<  9) & 0x00F80000;
		const u32 a = (cc << 16) & 0x80000000;
		return r | g | b | a;
	}
	else
	{
		return cc;
	}
}

bool GSTextureCache::CopyRGBFromDepthToColor(Target* dst, Target* depth_src)
{
	GL_CACHE("TC: Copy RGB from %dx%d %s[%x, %s] to %dx%d %s[%x, %s]", depth_src->GetUnscaledWidth(),
		depth_src->GetUnscaledHeight(), to_string(depth_src->m_type), depth_src->m_TEX0.TBP0,
		GSUtil::GetPSMName(depth_src->m_TEX0.PSM), dst->GetUnscaledWidth(), dst->GetUnscaledHeight(), to_string(dst->m_type),
		dst->m_TEX0.TBP0, GSUtil::GetPSMName(dst->m_TEX0.PSM));

	const GSVector2i new_size = dst->GetUnscaledSize().max(GSVector2i(depth_src->m_valid.z, depth_src->m_valid.w));
	const GSVector2i new_scaled_size = ScaleRenderTargetSize(new_size, dst->GetScale());
	const bool needs_new_tex = (new_size != dst->m_unscaled_size);
	GSTexture* tex = dst->m_texture;
	if (needs_new_tex)
	{
		tex = g_gs_device->CreateCompatible(dst->m_texture, new_scaled_size,
			new_size != dst->m_unscaled_size || new_size != depth_src->m_unscaled_size);
		if (!tex)
			return false;

		m_target_memory_usage = (m_target_memory_usage - dst->m_texture->GetMemUsage()) + tex->GetMemUsage();

		GetTargetSize(dst->m_TEX0.TBP0, dst->m_TEX0.TBW, dst->m_TEX0.PSM, new_size.x, new_size.y);
	}

	const GSVector4i clear_dirty_rc = GSVector4i::loadh(depth_src->GetUnscaledSize());
	for (u32 i = 0; i < dst->m_dirty.size(); i++)
	{
		GSDirtyRect& dr = dst->m_dirty[i];
		const GSVector4i drc = dr.GetDirtyRect(dst->m_TEX0, false);
		if (!drc.rintersect(clear_dirty_rc).rempty())
		{
			if ((dr.rgba._u32 &= ~0x7) == 0)
			{
				GL_CACHE("TC: Remove dirty rect (%d,%d=>%d,%d) from %s[%x, %s] due to incoming depth.", drc.left,
					drc.top, drc.right, drc.bottom, to_string(dst->m_type), dst->m_TEX0.TBP0, GSUtil::GetPSMName(dst->m_TEX0.PSM));
				dst->m_dirty.erase(dst->m_dirty.begin() + i);
			}
		}
	}

	depth_src->Update();

	if (depth_src->m_texture->GetState() == GSTexture::State::Cleared)
	{
		g_gs_device->ClearRenderTarget(tex, ConvertDepthToColor(depth_src->m_texture->GetClearDepth(), 32));
	}
	else if (depth_src->m_texture->GetState() != GSTexture::State::Invalidated)
	{
		const GSVector4 convert_rect = GSVector4(depth_src->GetUnscaledRect().rintersect(GSVector4i::loadh(new_size)));
		g_gs_device->StretchRectAuto(
			depth_src->m_texture, convert_rect / GSVector4(depth_src->GetUnscaledSize()).xyxy(),
			tex, convert_rect * GSVector4(dst->GetScale()), Nearest, 32, 24);
	}

	if (needs_new_tex)
	{
		if (dst->m_valid_alpha_low || dst->m_valid_alpha_high)
		{
			const GSVector4 copy_rect = GSVector4(tex->GetRect().rintersect(dst->m_texture->GetRect()));
			const GSVector4 dst_dims = GSVector4(GSVector4i(dst->m_texture->GetSize()).xyxy());
			g_gs_device->StretchRectAutoMask(dst->m_texture, copy_rect / dst_dims,
				tex, copy_rect, false, false, false, true);
		}

		g_gs_device->Recycle(dst->m_texture);
		dst->m_texture = tex;
	}

	dst->m_unscaled_size = new_size;
	dst->m_valid_rgb = true;
	dst->m_32_bits_fmt = true;
	return true;
}

bool GSTextureCache::PrepareDownloadTexture(u32 width, u32 height, GSTexture::Format format, std::unique_ptr<GSDownloadTexture>* tex)
{
	GSDownloadTexture* ctex = tex->get();
	if (ctex && ctex->GetWidth() >= width && ctex->GetHeight() >= height)
		return true;

	const u32 new_width = ctex ? std::max(ctex->GetWidth(), width) : width;
	const u32 new_height = ctex ? std::max(ctex->GetHeight(), height) : height;
	tex->reset();
	*tex = g_gs_device->CreateDownloadTexture(new_width, new_height, format);
	if (!tex)
	{
		Console.WriteLn("TC: Failed to create %ux%u download texture", new_width, new_height);
		return false;
	}

#ifdef PCSX2_DEVBUILD
	(*tex)->SetDebugName(TinyString::from_format("Texture Cache {}x{} {} Readback",
		new_width, new_height, GSTexture::GetFormatName(format)));
#endif

	return true;
}

void GSTextureCache::InvalidateContainedTargets(u32 start_bp, u32 end_bp, u32 write_psm, u32 write_bw, u32 fb_mask, bool ignore_exact)
{
	const bool preserve_alpha = (GSLocalMemory::m_psm[write_psm].trbpp == 24) || (fb_mask & 0xFF000000);
	for (int type = 0; type < (ignore_exact ? 1 : 2); type++)
	{
		auto& list = m_dst[type];
		for (auto i = list.begin(); i != list.end();)
		{
			Target* const t = *i;

			if ((ignore_exact && start_bp == t->m_TEX0.TBP0) || (start_bp != t->m_TEX0.TBP0 && (t->m_TEX0.TBP0 > end_bp || t->UnwrappedEndBlock() < start_bp)))
			{
				++i;
				continue;
			}

			const bool compatible_fmt = GSUtil::HasCompatibleBits(t->m_TEX0.PSM, write_psm);
			const bool compatible_width = std::max(t->m_TEX0.TBW, 1U) == std::max(write_bw, 1U);

			if ((type != DepthStencil || !compatible_fmt || !compatible_width) && start_bp != t->m_TEX0.TBP0 && (t->m_TEX0.TBP0 < start_bp || t->UnwrappedEndBlock() > end_bp))
			{
				const u32 offset = (std::abs(static_cast<int>(start_bp - t->m_TEX0.TBP0)) >> 5) % std::max(1U, t->m_TEX0.TBW);
				const GSVector4i dirty_rect = t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size).rintersect(t->m_valid);
				const u32 end_page_offset = ((end_bp - start_bp) >> 5);
				const u32 end_width = write_bw * 64;
				const u32 end_height = ((end_page_offset / std::max(write_bw, 1U)) * GSLocalMemory::m_psm[write_psm].pgs.y) + GSLocalMemory::m_psm[write_psm].pgs.y;
				const GSVector4i r = GSVector4i(0, 0, end_width, end_height);
				const GSVector4i invalidate_r = TranslateAlignedRectByPage(t, start_bp, write_psm, write_bw, r, false).rintersect(t->m_valid);

				if (offset == 0 || dirty_rect.rempty() || !dirty_rect.rintersect(invalidate_r).rempty())
				{
					if (write_bw == t->m_TEX0.TBW && GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[write_psm].bpp)
					{

						RGBAMask mask;
						mask._u32 = GSUtil::GetChannelMask(write_psm, fb_mask);
						AddDirtyRectTarget(t, invalidate_r, t->m_TEX0.PSM, t->m_TEX0.TBW, mask, false);
					}

					++i;
					continue;
				}
			}

			if (type == DepthStencil && t->m_TEX0.TBP0 < start_bp && t->m_end_block > start_bp)
			{
				const GSVector4i masked_valid = GSVector4i(t->m_valid.x, t->m_valid.y, t->m_valid.z & ~1, t->m_valid.w & ~1);
				const u32 reduced_endblock = GSLocalMemory::GetEndBlockAddress(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM, masked_valid);

				if (reduced_endblock <= start_bp)
				{
					t->ResizeValidity(masked_valid);
					t->ResizeDrawn(masked_valid);
					++i;
					continue;
				}
			}

			InvalidateSourcesFromTarget(t);

			t->m_valid_alpha_low &= preserve_alpha;
			t->m_valid_alpha_high &= preserve_alpha;
			t->m_valid_rgb &= (fb_mask & 0x00FFFFFF) != 0;
			t->m_was_dst_matched = false;

			if ((!t->m_valid_alpha_low && !t->m_valid_alpha_high && !t->m_valid_rgb) || type == DepthStencil)
			{
				auto& rev_list = m_dst[1 - type];
				for (auto j = rev_list.begin(); j != rev_list.end();)
				{
					Target* const rev_t = *j;
					if (rev_t->m_TEX0.TBP0 == t->m_TEX0.TBP0 && GSLocalMemory::m_psm[rev_t->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp)
					{
						if (t->m_last_draw > rev_t->m_last_draw && GSUtil::GetChannelMask(t->m_TEX0.PSM) == GSUtil::GetChannelMask(rev_t->m_TEX0.PSM))
						{
							if (type == DepthStencil && GSUtil::GetChannelMask(t->m_TEX0.PSM) == 0x7)
							{
								rev_t->m_valid_rgb = false;
								rev_t->m_was_dst_matched = false;
							}
							else
							{
								GL_CACHE("TC: InvalidateContainedTargets: Remove Target %s[%x, %s]", to_string(1 - type), rev_t->m_TEX0.TBP0, GSUtil::GetPSMName(rev_t->m_TEX0.PSM));
								rev_list.erase(j);
								delete rev_t;
							}
						}
						else
							rev_t->m_was_dst_matched = false;
						break;
					}
					++j;
				}

				GL_CACHE("TC: InvalidateContainedTargets: Remove Target %s[%x, %s]", to_string(type), t->m_TEX0.TBP0, GSUtil::GetPSMName(t->m_TEX0.PSM));
				i = list.erase(i);
				delete t;
				continue;
			}
			else if (ignore_exact && GSUtil::HasCompatibleBits(t->m_TEX0.PSM, write_psm))
			{
				RGBAMask mask;
				mask._u32 = GSUtil::GetChannelMask(write_psm, fb_mask);

				AddDirtyRectTarget(t, t->m_valid, t->m_TEX0.PSM, t->m_TEX0.TBW, mask, false);
				t->m_valid_rgb |= !!(mask._u32 & 0x7);
				t->m_valid_alpha_low |= mask.c.a;
				t->m_valid_alpha_high |= mask.c.a;
			}

			GL_CACHE("TC: InvalidateContainedTargets: Clear RGB valid on %s[%x, %s]", to_string(type), t->m_TEX0.TBP0, GSUtil::GetPSMName(t->m_TEX0.PSM));
			++i;
		}
	}
}

void GSTextureCache::InvalidateVideoMemType(int type, u32 bp, u32 write_psm, u32 write_fbmsk, bool dirty_only)
{
	auto& list = m_dst[type];
	for (auto i = list.begin(); i != list.end(); ++i)
	{
		Target* const t = *i;
		if (bp != t->m_TEX0.TBP0 || (dirty_only && t->m_dirty.empty()))
			continue;

		const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[write_psm];
		const bool new_valid_alpha_low = t->m_valid_alpha_low && (psm_s.trbpp == 24 || (psm_s.trbpp == 32 && (write_fbmsk & 0x0F000000) == 0x0F000000));
		const bool new_valid_alpha_high = t->m_valid_alpha_high && (psm_s.trbpp == 24 || (psm_s.trbpp == 32 && (write_fbmsk & 0xF0000000) == 0xF0000000));
		const bool new_valid_rgb = t->m_valid_rgb && (psm_s.trbpp >= 24 && (write_fbmsk & 0x00FFFFFF) == 0x00FFFFFF);

		if ((new_valid_alpha_low || new_valid_alpha_high || new_valid_rgb) &&
			(type != DepthStencil || (GSLocalMemory::m_psm[t->m_TEX0.PSM].trbpp == 24 && new_valid_rgb)))
		{
			if (t->m_valid_alpha_low != new_valid_alpha_low || t->m_valid_alpha_high != new_valid_alpha_high || t->m_valid_rgb != new_valid_rgb)
			{
				GL_CACHE("TC: InvalidateVideoMemType: Partial Remove Target(%s) (0x%x) RGB: %s->%s, Alow: %s->%s Ahigh: %s->%s",
					to_string(type), t->m_TEX0.TBP0, t->m_valid_rgb ? "valid" : "invalid", new_valid_rgb ? "valid" : "invalid",
					t->m_valid_alpha_low ? "valid" : "invalid", new_valid_alpha_low ? "valid" : "invalid",
					t->m_valid_alpha_high ? "valid" : "invalid", new_valid_alpha_high ? "valid" : "invalid");
			}

			t->m_valid_alpha_low = new_valid_alpha_low;
			t->m_valid_alpha_high = new_valid_alpha_high;
			t->m_valid_rgb = new_valid_rgb;
			break;
		}


		GL_CACHE("TC: InvalidateVideoMemType: Remove Target(%s) (0x%x)", to_string(type),
			t->m_TEX0.TBP0);

		InvalidateSourcesFromTarget(t);

		auto& rev_list = m_dst[1 - type];
		for (auto j = rev_list.begin(); j != rev_list.end();)
		{
			Target* const rev_t = *j;
			if (rev_t->m_TEX0.TBP0 == t->m_TEX0.TBP0 && GSLocalMemory::m_psm[rev_t->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp)
			{
				rev_t->m_was_dst_matched = false;
				break;
			}
			++j;
		}

		list.erase(i);
		delete t;
		break;
	}
}

void GSTextureCache::InvalidateVideoMem(const GSOffset& off, const GSVector4i& rect, bool target)
{
	const u32 bp = off.bp();
	const u32 bw = off.bw();
	const u32 psm = off.psm();

	const u32 start_bp = GSLocalMemory::GetStartBlockAddress(off.bp(), off.bw(), off.psm(), rect);
	const u32 end_bp = rect.rempty() ? start_bp : GSLocalMemory::GetUnwrappedEndBlockAddress(off.bp(), off.bw(), off.psm(), rect);

	if (!target)
	{
		const int pages = (end_bp + ((1<<5)-1) - start_bp) >> 5;
		for (int pgs = 0; pgs < pages; pgs++)
		{
			auto& list = m_src.m_map[((bp >> 5) + pgs) & 0x1ff];
			for (auto i = list.begin(); i != list.end();)
			{
				Source* s = *i;
				++i;

				if ((GSUtil::HasSharedBits(psm, s->m_TEX0.PSM) && (end_bp > s->m_TEX0.TBP0 && start_bp < s->UnwrappedEndBlock()) && !s->m_target) ||
					(GSUtil::HasSharedBits(bp, psm, s->m_from_target_TEX0.TBP0, s->m_TEX0.PSM) && s->m_target))
				{
					m_src.RemoveAt(s);
				}
			}

			const u32 bbp = bp + bw * 0x10;
			if (bw >= 16 && bbp < 16384)
			{
				auto& list = m_src.m_map[bbp >> 5];
				for (auto i = list.begin(); i != list.end();)
				{
					Source* s = *i;
					++i;

					if (GSUtil::HasSharedBits(bbp, psm, s->m_TEX0.TBP0, s->m_TEX0.PSM))
					{
						m_src.RemoveAt(s);
					}
				}
			}
		}
	}

	bool found = false;
	GSVector4i r = rect;

	off.loopPages(rect, [this, &rect, bp, bw, psm, &found](u32 page) {
		auto& list = m_src.m_map[page];
		for (auto i = list.begin(); i != list.end();)
		{
			Source* s = *i;
			++i;

			if (GSUtil::HasSharedBits(psm, s->m_TEX0.PSM))
			{
				bool b = bp == s->m_TEX0.TBP0;

				if (!s->m_target)
				{
					found |= b;

					if (s->m_from_hash_cache || (GSConfig.UserHacks_DisablePartialInvalidation && s->m_repeating))
					{
						m_src.RemoveAt(s);
					}
					else
					{
						u32* RESTRICT valid = s->m_valid.get();

						if (valid && !s->CanPreload())
						{
							if (s->m_repeating)
							{
								for (const GSVector2i& k : s->m_p2t[page])
								{
									valid[k.x] &= k.y;
								}
							}
							else
							{
								valid[page] = 0;
							}
						}

						s->m_complete_layers = 0;
					}
				}
				else
				{
					b |= bp == s->m_from_target_TEX0.TBP0;

					if (!b)
						b = s->Overlaps(bp, bw, psm, rect);

					if (b)
					{
						m_src.RemoveAt(s);
					}
				}
			}
		}
	});

	if (!target)
		return;


	RGBAMask rgba;
	rgba._u32 = GSUtil::GetChannelMask(psm);

	for (int type = 0; type < 2; type++)
	{
		auto& list = m_dst[type];
		for (auto i = list.begin(); i != list.end();)
		{
			auto j = i;
			Target* t = *j;

			if ((bp < t->m_TEX0.TBP0 && end_bp < t->m_TEX0.TBP0) || bp > t->UnwrappedEndBlock())
			{
				++i;
				continue;
			}

			++i;

			if (GSUtil::HasSharedBits(psm, t->m_TEX0.PSM))
			{
				if (bp == t->m_TEX0.TBP0)
				{
					if (GSUtil::HasCompatibleBits(psm, t->m_TEX0.PSM) && bw == std::max(t->m_TEX0.TBW, 1U))
					{
						GL_CACHE("TC: Dirty Target(%s) (0x%x) r(%d,%d,%d,%d)", to_string(type),
							t->m_TEX0.TBP0, r.x, r.y, r.z, r.w);

						if (t->m_type == DepthStencil && GetTemporaryZ() != nullptr)
						{
							if (GetTemporaryZInfo().ZBP == t->m_TEX0.TBP0)
								InvalidateTemporaryZ();
						}

						if (GSLocalMemory::m_psm[psm].depth &&
							r.width() <= (GSLocalMemory::m_psm[psm].pgs.x >> 1) && r.height() <= (GSLocalMemory::m_psm[psm].pgs.y >> 1))
							DirtyRectByPage(bp, psm, bw, t, r);
						else
							AddDirtyRectTarget(t, r, psm, bw, rgba);

						if (FullRectDirty(t))
						{
							InvalidateSourcesFromTarget(t);
							i = list.erase(j);
							GL_CACHE("TC: Remove Target(%s) (0x%x)", to_string(type),
								t->m_TEX0.TBP0);
							delete t;
						}

						continue;
					}
				}

				if (t->Overlaps(bp, bw, psm, r))
				{
					DirtyRectByPage(bp, psm, bw, t, r);

					if (FullRectDirty(t, rgba._u32))
					{
						InvalidateSourcesFromTarget(t);
						i = list.erase(j);
						GL_CACHE("TC: Remove Target(%s) (0x%x)", to_string(type),
							t->m_TEX0.TBP0);
						delete t;
					}
				}
			}
			else if (GSUtil::GetChannelMask(psm) == 0x8 && GSUtil::GetChannelMask(t->m_TEX0.PSM) == 0x7 && t->Overlaps(bp, bw, psm, r))
			{
				t->m_valid_alpha_high &= !(psm == PSMT8H || psm == PSMT4HH);
				t->m_valid_alpha_low &= !(psm == PSMT8H || psm == PSMT4HL);
			}
		}
	}
}

void GSTextureCache::InvalidateLocalMem(const GSOffset& off, const GSVector4i& r, bool full_flush)
{
	const u32 bp = off.bp();
	const u32 psm = off.psm();
	[[maybe_unused]] const u32 bw = off.bw();
	const u32 read_start = GSLocalMemory::m_psm[psm].info.bn(r.x, r.y, bp, bw);
	const u32 read_end = GSLocalMemory::m_psm[psm].info.bn(r.z - 1, r.w - 1, bp, bw);

	GL_CACHE("TC: InvalidateLocalMem off(0x%x, %u, %s) r(%d, %d => %d, %d)",
		bp,
		bw,
		GSUtil::GetPSMName(psm),
		r.x,
		r.y,
		r.z,
		r.w);

	if (GSLocalMemory::m_psm[psm].bpp >= 16)
	{
		if (GSConfig.HWDownloadMode > GSHardwareDownloadMode::EnabledForceFull)
		{
			DevCon.Error("TC: Skipping depth readback of %ux%u @ %u,%u", r.width(), r.height(), r.left, r.top);
			return;
		}

		bool z_found = false;

		if (!GSConfig.UserHacks_DisableDepthSupport)
		{
			auto& dss = m_dst[DepthStencil];
			for (auto it = dss.rbegin(); it != dss.rend(); ++it)
			{
				Target* t = *it;

				if (t->m_32_bits_fmt && t->m_TEX0.PSM > PSMZ24)
					t->m_TEX0.PSM = PSMZ32;

				const bool bpp_match = GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[psm].bpp && GSUtil::GetChannelMask(psm) & GSUtil::GetChannelMask(t->m_TEX0.PSM);
				const u32 page_mask = ((1 << 5) - 1);
				const bool expecting_this_tex = bpp_match && (((read_start & ~page_mask) == t->m_TEX0.TBP0) || (bp >= t->m_TEX0.TBP0 && ((read_end + page_mask) & ~page_mask) <= ((t->m_end_block + page_mask) & ~page_mask)));
				if (!expecting_this_tex)
					continue;

				t->readbacks_since_draw++;

				GSVector2i page_size = GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs;
				const bool can_translate = CanTranslate(bp, bw, psm, r, t->m_TEX0.TBP0, t->m_TEX0.PSM, t->m_TEX0.TBW);
				const bool swizzle_match = GSLocalMemory::m_psm[psm].depth == GSLocalMemory::m_psm[t->m_TEX0.PSM].depth;
				GSVector4i targetr = {};
				if (full_flush || GSConfig.HWDownloadMode == GSHardwareDownloadMode::EnabledForceFull ||  t->readbacks_since_draw > 1)
				{
					targetr = t->m_drawn_since_read;
				}
				else if (can_translate)
				{
					if (swizzle_match)
					{
						targetr = TranslateAlignedRectByPage(t, bp, psm, bw, r, true);
					}
					else
					{
						GSVector4i new_rect = r;

						if (GSLocalMemory::m_psm[psm].bpp != GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp)
						{
							const GSVector2i dst_page_size = GSLocalMemory::m_psm[psm].pgs;
							new_rect = GSVector4i(new_rect.x / page_size.x, new_rect.y / page_size.y, (new_rect.z + (page_size.x - 1)) / page_size.x, (new_rect.w + (page_size.y - 1)) / page_size.y);
							new_rect = GSVector4i(new_rect.x * dst_page_size.x, new_rect.y * dst_page_size.y, new_rect.z * dst_page_size.x, new_rect.w * dst_page_size.y);
						}
						else
						{
							new_rect.x &= ~(page_size.x - 1);
							new_rect.y &= ~(page_size.y - 1);
							new_rect.z = (r.z + (page_size.x - 1)) & ~(page_size.x - 1);
							new_rect.w = (r.w + (page_size.y - 1)) & ~(page_size.y - 1);
						}
						if (new_rect.eq(GSVector4i::zero()))
						{

							SurfaceOffsetKey sok;
							sok.elems[0].bp = bp;
							sok.elems[0].bw = bw;
							sok.elems[0].psm = psm;
							sok.elems[0].rect = r;
							sok.elems[1].bp = t->m_TEX0.TBP0;
							sok.elems[1].bw = t->m_TEX0.TBW;
							sok.elems[1].psm = t->m_TEX0.PSM;
							sok.elems[1].rect = t->m_valid;

							new_rect = ComputeSurfaceOffset(sok).b2a_offset;
						}
						targetr = TranslateAlignedRectByPage(t, bp & ~((1 << 5) - 1), psm, bw, new_rect, true);
					}
				}
				else
				{
					targetr = t->m_drawn_since_read;
				}

				const GSVector4i draw_rect = (t->readbacks_since_draw > 1) ? t->m_drawn_since_read : targetr.rintersect(t->m_drawn_since_read);
				const GSVector4i dirty_rect = t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size);

				z_found = read_start >= t->m_TEX0.TBP0 && read_end <= t->m_end_block && GSLocalMemory::m_psm[psm].depth == GSLocalMemory::m_psm[t->m_TEX0.PSM].depth;

				if (z_found && draw_rect.rintersect(dirty_rect).eq(draw_rect))
					return;

				if (t->m_drawn_since_read.eq(GSVector4i::zero()))
				{
					if (z_found && draw_rect.rintersect(t->m_valid).eq(draw_rect))
						return;
					else
						continue;
				}

				if (!draw_rect.rempty())
				{
					if (t->m_TEX0.TBP0 == bp && !dirty_rect.rintersect(targetr).rempty())
						t->Update();

					Read(t, draw_rect);

					if (draw_rect.rintersect(t->m_drawn_since_read).eq(t->m_drawn_since_read))
						t->m_drawn_since_read = GSVector4i::zero();
				}
			}
		}
		if (z_found)
			return;
	}

	auto& rts = m_dst[RenderTarget];

	for (int pass = 0; pass < 2; pass++)
	{
		for (auto it = rts.rbegin(); it != rts.rend(); it++)
		{
			Target* t = *it;

			const bool exact_bp = t->m_TEX0.TBP0 == bp;
			if (pass == 0)
			{
				const bool bpp_match = GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[psm].bpp && GSUtil::GetChannelMask(psm) & GSUtil::GetChannelMask(t->m_TEX0.PSM);
				const u32 page_mask = ((1 << 5) - 1);
				const bool exact_mem_match = (read_start & ~page_mask) == (t->m_TEX0.TBP0 & ~page_mask) && ((read_end + (page_mask - 1)) & ~page_mask) <= t->m_end_block;
				const bool expecting_this_tex = exact_mem_match || (bpp_match && bw == t->m_TEX0.TBW && (((read_start & ~page_mask) == t->m_TEX0.TBP0) || (bp >= t->m_TEX0.TBP0 && ((read_end + page_mask) & ~page_mask) <= ((t->m_end_block + page_mask) & ~page_mask))));

				if (!expecting_this_tex)
					continue;
			}
			else
			{
				const bool expecting_this_tex = t->Overlaps(bp, bw, psm, r);

				if (!expecting_this_tex || !GSUtil::HasSharedBits(psm, t->m_TEX0.PSM))
					continue;
			}

			t->readbacks_since_draw++;

			GSVector2i page_size = GSLocalMemory::m_psm[t->m_TEX0.PSM].pgs;
			const bool can_translate = CanTranslate(bp, bw, psm, r, t->m_TEX0.TBP0, t->m_TEX0.PSM, t->m_TEX0.TBW);
			const bool swizzle_match = GSLocalMemory::m_psm[psm].depth == GSLocalMemory::m_psm[t->m_TEX0.PSM].depth;
			GSVector4i targetr = {};
			if (full_flush || t->readbacks_since_draw > 1)
			{
				targetr = t->m_drawn_since_read;
			}
			else if (can_translate)
			{
				if (swizzle_match)
				{
					if (exact_bp && GSUtil::HasSameSwizzleBits(psm, t->m_TEX0.PSM) && bw == t->m_TEX0.TBW)
						targetr = r;
					else
						targetr = TranslateAlignedRectByPage(t, bp, psm, bw, r, true);
				}
				else
				{
					GSVector4i new_rect = r;

					if (GSLocalMemory::m_psm[psm].bpp != GSLocalMemory::m_psm[t->m_TEX0.PSM].bpp)
					{
						const GSVector2i dst_page_size = GSLocalMemory::m_psm[psm].pgs;
						new_rect = GSVector4i(new_rect.x / page_size.x, new_rect.y / page_size.y, (new_rect.z + (page_size.x - 1)) / page_size.x, (new_rect.w + (page_size.y - 1)) / page_size.y);
						new_rect = GSVector4i(new_rect.x * dst_page_size.x, new_rect.y * dst_page_size.y, new_rect.z * dst_page_size.x, new_rect.w * dst_page_size.y);
					}
					else
					{
						new_rect.x &= ~(page_size.x - 1);
						new_rect.y &= ~(page_size.y - 1);
						new_rect.z = (r.z + (page_size.x - 1)) & ~(page_size.x - 1);
						new_rect.w = (r.w + (page_size.y - 1)) & ~(page_size.y - 1);
					}
					if (new_rect.eq(GSVector4i::zero()))
					{

						SurfaceOffsetKey sok;
						sok.elems[0].bp = bp;
						sok.elems[0].bw = bw;
						sok.elems[0].psm = psm;
						sok.elems[0].rect = r;
						sok.elems[1].bp = t->m_TEX0.TBP0;
						sok.elems[1].bw = t->m_TEX0.TBW;
						sok.elems[1].psm = t->m_TEX0.PSM;
						sok.elems[1].rect = t->m_valid;

						new_rect = ComputeSurfaceOffset(sok).b2a_offset;
					}
					targetr = TranslateAlignedRectByPage(t, bp & ~((1 << 5) - 1), psm, bw, new_rect, true);
				}
			}
			else
			{
				targetr = t->m_drawn_since_read;
			}

			if (t->m_drawn_since_read.eq(GSVector4i::zero()))
			{
				if (targetr.rintersect(t->m_valid).eq(targetr) && exact_bp)
					return;
				else
					continue;
			}

			const GSVector4i dirty_rect = t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size);
			if (targetr.rintersect(dirty_rect).eq(targetr))
			{
				if (exact_bp)
					return;
				else
					continue;
			}

			targetr = targetr.rintersect(t->m_drawn_since_read);

			if (!targetr.rempty())
			{
				if (!t->m_dirty.empty())
				{
					bool only_in_dirty_area = true;
					off.pageLooperForRect(r).loopPagesWithBreak([t, &dirty_rect, &only_in_dirty_area](u32 page) {
						const GSVector4i page_rect = GSLocalMemory::GetRectForPageOffset(t->m_TEX0.TBP0,
							page * GS_BLOCKS_PER_PAGE, t->m_TEX0.TBW, t->m_TEX0.PSM);
						if (!dirty_rect.rintersect(page_rect).eq(page_rect))
						{
							only_in_dirty_area = false;
							return false;
						}
						return true;
					});

					if (only_in_dirty_area)
					{
						if (exact_bp)
							return;
						else
							continue;
					}
				}

				if (GSConfig.HWDownloadMode > GSHardwareDownloadMode::EnabledForceFull)
				{
					DevCon.Error("TC: Skipping depth readback of %ux%u @ %u,%u", targetr.width(), targetr.height(), targetr.left, targetr.top);
					continue;
				}

				if (exact_bp && !dirty_rect.rintersect(targetr).rempty())
					t->Update();

				Read(t, targetr);

				if (t->m_drawn_since_read.rintersect(targetr).eq(t->m_drawn_since_read))
				{
					t->m_drawn_since_read = GSVector4i::zero();
				}
				else if (targetr.xzxz().eq(t->m_drawn_since_read.xzxz()) && targetr.w >= t->m_drawn_since_read.y)
				{
					if (targetr.y <= t->m_drawn_since_read.y)
						t->m_drawn_since_read.y = targetr.w;
					else if (targetr.w >= t->m_drawn_since_read.w)
						t->m_drawn_since_read.w = targetr.y;
				}
				else if (targetr.ywyw().eq(t->m_drawn_since_read.ywyw()) && targetr.z >= t->m_drawn_since_read.x)
				{
					if (targetr.x <= t->m_drawn_since_read.x)
						t->m_drawn_since_read.x = targetr.z;
					else if (targetr.z >= t->m_drawn_since_read.z)
						t->m_drawn_since_read.z = targetr.x;
				}

				if (exact_bp && read_end <= t->m_end_block)
					return;
			}
		}
	}
}

bool GSTextureCache::Move(u32 SBP, u32 SBW, u32 SPSM, int sx, int sy, u32 DBP, u32 DBW, u32 DPSM, int dx, int dy, int w, int h)
{
	if (SBP == DBP && SPSM == DPSM && !GSLocalMemory::m_psm[SPSM].depth && ShuffleMove(SBP, SBW, SPSM, sx, sy, dx, dy, w, h))
		return true;

	if (SPSM == DPSM && SBW == 1 && SBW == DBW && PageMove(SBP, DBP, SBW, SPSM, sx, sy, dx, dy, w, h))
		return true;

	bool alpha_only = false;

	if (SPSM == PSMT8H && SPSM == DPSM)
		alpha_only = true;

	const GSLocalMemory::psm_t& spsm_s = GSLocalMemory::m_psm[SPSM];
	const GSLocalMemory::psm_t& dpsm_s = GSLocalMemory::m_psm[DPSM];
	if (GSLocalMemory::m_psm[SPSM].bpp != GSLocalMemory::m_psm[DPSM].bpp || ((spsm_s.pal + dpsm_s.pal) != 0 && !alpha_only))
	{
		GL_CACHE("TC: Skipping HW move from 0x%X to 0x%X with SPSM=%s DPSM=%s", SBP, DBP, GSUtil::GetPSMName(SPSM), GSUtil::GetPSMName(DPSM));
		return false;
	}

	bool req_resize = false;

	const u32 start_SBP = SBP;
	const u32 start_DBP = DBP;

	if (m_expected_src_bp == static_cast<int>(SBP) && m_expected_dst_bp == static_cast<int>(DBP))
	{
		GSVector4i rect_offset = TranslateAlignedRectByPage(m_remembered_src_bp, m_remembered_src_bp + 1, SBW, SPSM, GSVector4i(0, 0, SBW * 64, dy + h), SBP, SPSM, SBW, GSVector4i(sx, sy, sx + w, sy + h), false);
		rect_offset.x = rect_offset.x - sx;
		rect_offset.y = rect_offset.y - sy;
		sx += rect_offset.x;
		sy += rect_offset.y;
		dx += rect_offset.x;
		dy += rect_offset.y;
		req_resize = true;
		GL_INS("TC: Detected striped move, realigning from SBP %x->%x DBP %x->%x", SBP, m_remembered_src_bp, DBP, m_remembered_dst_bp);

		SBP = m_remembered_src_bp;
		DBP = m_remembered_dst_bp;
	}

	m_expected_src_bp = -1;
	m_remembered_src_bp = -1;
	m_expected_dst_bp = -1;
	m_remembered_dst_bp = -1;

	GSTextureCache::Target* src = GetExactTarget(SBP, SBW, spsm_s.depth ? DepthStencil : RenderTarget, SBP);
	GSTextureCache::Target* dst = GetExactTarget(DBP, DBW, dpsm_s.depth ? DepthStencil : RenderTarget, DBP);

	if (alpha_only && (!dst || GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp != 32))
		return false;

	if (dst && DBP == SBP && dy > dst->m_unscaled_size.y)
	{
		const u32 new_DBP = DBP + (((dy / GSLocalMemory::m_psm[dst->m_TEX0.PSM].pgs.y) * DBW) << 5);

		dst = nullptr;

		DBP = new_DBP;
		dy = 0;

		dst = GetExactTarget(DBP, DBW, dpsm_s.depth ? DepthStencil : RenderTarget, DBP);
	}

	if (src && !dst && ((((dx == 0 && dy == 0) || (dx == sx && dy == sy && DBW == src->m_TEX0.TBW)) && ((static_cast<u32>(w) + 63) / 64) <= DBW) || HasTargetInHeightCache(DBP, DBW, DPSM, 10)))
	{
		GIFRegTEX0 new_TEX0 = {};
		new_TEX0.TBP0 = DBP;
		new_TEX0.TBW = DBW;
		new_TEX0.PSM = DPSM;

		const GSVector2i target_size = (dx == 0 && dy == 0) ? GetTargetSize(DBP, DBW, DPSM, Common::AlignUpPow2(w, 64), h) : GSVector2i(src->m_valid.z, src->m_valid.w);
		dst = LookupDrawTarget(new_TEX0, target_size, src->m_scale, src->m_type);
		if (!dst)
		{
			dst = Target::Create(new_TEX0, target_size.x, target_size.y, src->m_scale, GSLocalMemory::m_psm[DPSM].depth, true);
			if (!dst) [[unlikely]]
				return false;
		}
		else
		{
			req_resize = true;

			if (dst->m_was_dst_matched || static_cast<u32>(Common::AlignUpPow2(w, 64) / GSLocalMemory::m_psm[new_TEX0.PSM].pgs.x) > dst->m_TEX0.TBW)
			{
				dst->m_TEX0 = new_TEX0;
			}
		}

		if (!dst)
			return false;

		dst->UpdateValidity(GSVector4i(dx, dy, dx + w, dy + h));
		dst->OffsetHack_modxy = src->OffsetHack_modxy;
	}

	if (!src || !dst || src->m_scale != dst->m_scale)
		return false;

	if (src->m_TEX0.TBP0 != SBP)
	{
		const GSVector4i offset = TranslateAlignedRectByPage(src, SBP, SPSM, SBW, GSVector4i(0, 1).xxyy(), false);

		sx += offset.x;
		sy += offset.y;
	}

	if (dst->m_TEX0.TBP0 != DBP)
	{
		const GSVector4i offset = TranslateAlignedRectByPage(dst, DBP, DPSM, DBW, GSVector4i(0, 1).xxyy(), false);

		dx += offset.x;
		dy += offset.y;
		req_resize = true;
	}

	const float scale = src->m_scale;
	const int scaled_sx = static_cast<int>(sx * scale);
	const int scaled_sy = static_cast<int>(sy * scale);
	const int scaled_dx = static_cast<int>(dx * scale);
	const int scaled_dy = static_cast<int>(dy * scale);
	const int scaled_w = static_cast<int>(w * scale);
	const int scaled_h = static_cast<int>(h * scale);

	if ((scaled_sx + scaled_w) > src->m_texture->GetWidth() || (scaled_sy + scaled_h) > src->m_texture->GetHeight())
		return false;

	if (req_resize)
	{
		const GSVector2i target_size = GetTargetSize(DBP, DBW, DPSM, Common::AlignUpPow2(dx + w, 64), dx + h);
		dst->ResizeTexture(std::max(dst->m_unscaled_size.x, target_size.x), std::max(dst->m_unscaled_size.y, target_size.y));
	}
	src->UpdateIfDirtyIntersects(GSVector4i(sx, sy, sx + w, sy + h));

	dst->Update();

	g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil - dst->m_type, dst->m_TEX0.TBP0);

	const int required_dh = scaled_dy + scaled_h;
	if ((scaled_dx + scaled_w) <= dst->m_texture->GetWidth() && required_dh > dst->m_texture->GetHeight())
	{
		int new_height = dy + h;
		if (new_height > GSRendererHW::MAX_FRAMEBUFFER_HEIGHT)
			return false;

		new_height = Common::AlignUpPow2(new_height, static_cast<unsigned>(GSLocalMemory::m_psm[DPSM].bs.y));

		GL_INS("TC: Resize %dx%d target to %dx%d for move", dst->m_unscaled_size.x, dst->m_unscaled_size.y, dst->m_unscaled_size.x, new_height);
		GetTargetSize(DBP, DBW, DPSM, 0, new_height);

		if (!dst->ResizeTexture(dst->m_unscaled_size.x, new_height, false))
		{
			return false;
		}
	}

	if ((scaled_dx + scaled_w) > dst->m_texture->GetWidth() || (scaled_dy + scaled_h) > dst->m_texture->GetHeight())
		return false;
	GL_CACHE("TC: HW Move after draw %lld 0x%x[BW:%u PSM:%s] to 0x%x[BW:%u PSM:%s] <%d,%d->%d,%d> -> <%d,%d->%d,%d>", GSState::s_n, SBP, SBW,
		GSUtil::GetPSMName(SPSM), DBP, DBW, GSUtil::GetPSMName(DPSM), sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h);

	const bool cover_whole_target = dst->m_type == RenderTarget && GSVector4i(dx, dy, dx + w, dy + h).rintersect(dst->m_valid).eq(dst->m_valid);
	if (!cover_whole_target)
	{
		src->UnscaleRTAlpha();
		dst->UnscaleRTAlpha();
	}

	const GSRendererType renderer = GSGetCurrentRenderer();
	const bool renderer_is_directx = (renderer == GSRendererType::DX11 || renderer == GSRendererType::DX12);
	if (SBP == DBP && (!(GSVector4i(sx, sy, sx + w, sy + h).rintersect(GSVector4i(dx, dy, dx + w, dy + h))).rempty() || renderer_is_directx))
	{
		GSTexture* tmp_texture = g_gs_device->CreateCompatible(src->m_texture, false);
		if (!tmp_texture)
		{
			Console.Error("(HW Move) Failed to allocate temporary %dx%d texture on HW move", w, h);
			return false;
		}

		if (tmp_texture->IsDepthLike())
		{
			const GSVector4 src_rect = GSVector4(scaled_sx, scaled_sy, scaled_sx + scaled_w, scaled_sy + scaled_h);
			const GSVector4 tmp_rect = src_rect / (GSVector4(tmp_texture->GetSize()).xyxy());
			const GSVector4 dst_rect = GSVector4(scaled_dx, scaled_dy, (scaled_dx + scaled_w), (scaled_dy + scaled_h));
			g_gs_device->StretchRectAuto(src->m_texture, tmp_rect, tmp_texture, src_rect, Nearest);
			g_gs_device->StretchRectAuto(tmp_texture, tmp_rect, dst->m_texture, dst_rect, Nearest);
		}
		else
		{
			if (SPSM == PSMT8H && SPSM == DPSM)
			{
				const GSVector4 src_rect = GSVector4(scaled_sx, scaled_sy, scaled_sx + scaled_w, scaled_sy + scaled_h);
				const GSVector4 tmp_rect = src_rect / (GSVector4(tmp_texture->GetSize()).xyxy());
				const GSVector4 dst_rect = GSVector4(scaled_dx, scaled_dy, (scaled_dx + scaled_w), (scaled_dy + scaled_h));
				g_gs_device->StretchRectAutoMask(src->m_texture, tmp_rect, tmp_texture, src_rect, false, false, false, true);
				g_gs_device->StretchRectAutoMask(tmp_texture, tmp_rect, dst->m_texture, dst_rect, false, false, false, true);
			}
			else
			{
				const GSVector4i src_rect = GSVector4i(scaled_sx, scaled_sy, scaled_sx + scaled_w, scaled_sy + scaled_h);
				g_gs_device->CopyRect(src->m_texture, tmp_texture, src_rect, scaled_sx, scaled_sy);
				g_gs_device->CopyRect(tmp_texture, dst->m_texture, src_rect, scaled_dx, scaled_dy);
			}
		}

		g_gs_device->Recycle(tmp_texture);
	}
	else
	{
		const GSVector4 scaled_src_rect = GSVector4(scaled_sx, scaled_sy, scaled_sx + scaled_w, scaled_sy + scaled_h);
		const GSVector4 src_rect = scaled_src_rect / (GSVector4(src->m_texture->GetSize()).xyxy());
		if (SPSM == PSMT8H && SPSM == DPSM)
		{
			const GSVector4 dst_rect = GSVector4(scaled_dx, scaled_dy, (scaled_dx + scaled_w), (scaled_dy + scaled_h));
			g_gs_device->StretchRectAutoMask(src->m_texture, src_rect, dst->m_texture, dst_rect, false, false, false, true);
		}
		else if (src->m_type != dst->m_type)
		{
			g_gs_device->StretchRectAuto(src->m_texture, src_rect, dst->m_texture, scaled_src_rect,
				Nearest, dpsm_s.trbpp == 16 ? 16 : 32, dpsm_s.trbpp);
		}
		else if (src->m_texture->IsDepthLike())
		{
			const GSVector4 dst_rect = GSVector4(scaled_dx, scaled_dy, (scaled_dx + scaled_w), (scaled_dy + scaled_h));
			g_gs_device->StretchRectAuto(src->m_texture, src_rect, dst->m_texture, dst_rect, Nearest);
		}
		else
		{
			g_gs_device->CopyRect(src->m_texture, dst->m_texture, static_cast<GSVector4i>(scaled_src_rect), scaled_dx, scaled_dy);
		}
	}

	if (GSUtil::GetChannelMask(DPSM) & 0x7)
		dst->m_valid_rgb |= src->m_valid_rgb;

	if (GSUtil::GetChannelMask(DPSM) & 0x8)
	{
		if (DPSM != PSMT4HH)
			dst->m_valid_alpha_low |= src->m_valid_alpha_low;
		if (DPSM != PSMT4HL)
			dst->m_valid_alpha_high |= src->m_valid_alpha_high;
		dst->m_alpha_max = src->m_alpha_max;
		dst->m_alpha_min = src->m_alpha_min;
		dst->m_alpha_range |= src->m_alpha_range;
	}

	u32 page_mask = GSLocalMemory::IsPageAlignedMasked(src->m_TEX0.PSM, GSVector4i(sx, sy, sx + w, sy + h));
	if (((page_mask & 0x0f0f) == 0x0f0f || (page_mask & 0xf0f0) == 0xf0f0) && (w == GSLocalMemory::m_psm[src->m_TEX0.PSM].pgs.x || h == GSLocalMemory::m_psm[src->m_TEX0.PSM].pgs.y))
	{
		if (w == GSLocalMemory::m_psm[src->m_TEX0.PSM].pgs.x && h == GSLocalMemory::m_psm[src->m_TEX0.PSM].pgs.y)
		{
			m_expected_src_bp = start_SBP + GS_BLOCKS_PER_PAGE;
			m_expected_dst_bp = start_DBP + GS_BLOCKS_PER_PAGE;
		}
		else if (w == GSLocalMemory::m_psm[src->m_TEX0.PSM].pgs.x)
		{
			m_expected_src_bp = GSLocalMemory::GetStartBlockAddress(src->m_TEX0.TBP0, src->m_TEX0.TBW, src->m_TEX0.PSM, GSVector4i(sx + w, 0, sx + w + w, h));
			m_expected_dst_bp = GSLocalMemory::GetStartBlockAddress(dst->m_TEX0.TBP0, dst->m_TEX0.TBW, dst->m_TEX0.PSM, GSVector4i(dx + w, 0, dx + w + w, h));
		}
		else
		{
			m_expected_src_bp = GSLocalMemory::GetStartBlockAddress(src->m_TEX0.TBP0, src->m_TEX0.TBW, src->m_TEX0.PSM, GSVector4i(0, sy + h, w, sy + h + h));
			m_expected_dst_bp = GSLocalMemory::GetStartBlockAddress(dst->m_TEX0.TBP0, dst->m_TEX0.TBW, dst->m_TEX0.PSM, GSVector4i(0, dy + h, w, dy + h + h));
		}

		if (static_cast<u32>(m_expected_src_bp) < src->UnwrappedEndBlock() && static_cast<u32>(m_expected_src_bp) >= src->m_TEX0.TBP0)
		{
			m_remembered_src_bp = src->m_TEX0.TBP0;
			m_remembered_dst_bp = dst->m_TEX0.TBP0;
		}
		else
		{
			m_expected_src_bp = -1;
			m_remembered_src_bp = -1;
			m_expected_dst_bp = -1;
			m_remembered_dst_bp = -1;
		}
	}
	dst->UpdateValidity(GSVector4i(dx, dy, dx + w, dy + h));
	dst->UpdateDrawn(GSVector4i(dx, dy, dx + w, dy + h));

	if (cover_whole_target)
		dst->m_rt_alpha_scale = src->m_rt_alpha_scale;

	InvalidateVideoMem(g_gs_renderer->m_mem.GetOffset(DBP, DBW, DPSM), GSVector4i(dx, dy, dx + w, dy + h), false);
	CombineAlignedInsideTargets(dst);
	return true;
}

bool GSTextureCache::ShuffleMove(u32 BP, u32 BW, u32 PSM, int sx, int sy, int dx, int dy, int w, int h)
{
	if (PSM != PSMCT16)
		return false;

	GL_CACHE("TC: Trying ShuffleMove: BP=%04X BW=%u PSM=%u SX=%d SY=%d DX=%d DY=%d W=%d H=%d", BP, BW, PSM, sx, sy, dx, dy, w, h);

	GSTextureCache::Target* tgt = nullptr;
	for (auto t : m_dst[RenderTarget])
	{
		if (t->m_TEX0.PSM == PSMCT32 && BP >= t->m_TEX0.TBP0 && BP <= t->m_end_block)
		{
			const SurfaceOffset so(ComputeSurfaceOffset(BP, BW, PSM, GSVector4i(sx, sy, sx + w, sy + h), t));
			if (so.is_valid)
			{
				tgt = t;
				GL_CACHE("TC: ShuffleMove: Surface offset %d,%d from BP %04X - %04X", so.b2a_offset.x, so.b2a_offset.y, BP, t->m_TEX0.TBP0);
				sx += so.b2a_offset.x;
				sy += so.b2a_offset.y;
				dx += so.b2a_offset.x;
				dy += so.b2a_offset.y;
				break;
			}
		}
	}
	if (!tgt)
	{
		GL_CACHE("TC: ShuffleMove: No target found");
		return false;
	}

	const s32 diff_x = (dx - sx);
	if (std::abs(diff_x) != 8 || sy != dy)
	{
		GL_CACHE("TC: ShuffleMove: Difference is not 8 pixels");
		return false;
	}

	const bool read_ba = (diff_x < 0);
	const bool write_rg = (diff_x < 0);

	const GSVector4i bbox = write_rg ? GSVector4i(dx, dy, dx + w, dy + h) : GSVector4i(sx, sy, sx + w, sy + h);

	if (read_ba || !write_rg)
		tgt->UnscaleRTAlpha();

	GSHWDrawConfig& config = GSRendererHW::GetInstance()->BeginHLEHardwareDraw(tgt->m_texture, nullptr, tgt->m_scale, tgt->m_texture, tgt->m_scale, bbox);
	config.colormask.wrgba = (write_rg ? (1 | 2) : (4 | 8));
	config.ps.process_ba = read_ba ? 1 : 0;
	config.ps.process_rg = !read_ba ? 1 : 0;
	config.ps.process_ba = !write_rg ? 2 : 0;
	config.ps.process_rg = write_rg ? 2 : 0;
	config.ps.shuffle_across = true;
	config.ps.write_rg = write_rg;
	config.ps.shuffle = true;
	GSRendererHW::GetInstance()->EndHLEHardwareDraw(false);

	if (!write_rg)
	{
		tgt->m_alpha_min = 0;
		tgt->m_alpha_max = 255;
		tgt->m_alpha_range = true;
	}

	return true;
}

bool GSTextureCache::PageMove(u32 SBP, u32 DBP, u32 BW, u32 PSM, int sx, int sy, int dx, int dy, int w, int h)
{
	pxAssert(BW == 1);

	const GSVector4i src = GSVector4i(sx, sy, sx + w, sy + h);
	const GSVector4i drc = GSVector4i(dx, dy, dx + w, dy + h);
	if (!GSLocalMemory::IsPageAligned(PSM, src) || !GSLocalMemory::IsPageAligned(PSM, drc))
		return false;

	const GSVector2i& pgs = GSLocalMemory::m_psm[PSM].pgs;
	const u32 num_pages = (h / pgs.y) * BW;
	const u32 src_page_offset = ((sy / pgs.y) * BW) + (sx / pgs.x);
	const u32 src_block_end = SBP + (((src_page_offset + num_pages) * GS_BLOCKS_PER_PAGE) - 1);
	const u32 dst_page_offset = ((dy / pgs.y) * BW) + (dx / pgs.x);
	const u32 dst_block_end = DBP + (((dst_page_offset + num_pages) * GS_BLOCKS_PER_PAGE) - 1);
	pxAssert(num_pages > 0);
	GL_PUSH("TC: GSTextureCache::PageMove(): %u pages, with offset of %u src %u dst", num_pages, src_page_offset,
		dst_page_offset);

	Target* stgt = nullptr;
	Target* dtgt = nullptr;
	for (int type = 0; type < 2; type++)
	{
		for (Target* tgt : m_dst[type])
		{
			if (tgt->m_TEX0.PSM != PSM)
				continue;

			const u32 tgt_end = tgt->UnwrappedEndBlock();
			if (tgt->m_TEX0.TBP0 <= SBP && src_block_end <= tgt_end)
				stgt = tgt;
			if (tgt->m_TEX0.TBP0 <= DBP && dst_block_end <= tgt_end)
				dtgt = tgt;

			if (stgt && dtgt)
				break;
		}

		if (stgt && dtgt)
			break;
	}
	if (!stgt || !dtgt)
	{
		GL_INS("TC: Targets not found.");
		return false;
	}

	if (((SBP - stgt->m_TEX0.TBP0) % GS_BLOCKS_PER_PAGE) != 0 || ((DBP - dtgt->m_TEX0.TBP0) % GS_BLOCKS_PER_PAGE) != 0)
	{
		GL_INS("TC: Effective SBP of %x or DBP of %x is not page aligned.", SBP - stgt->m_TEX0.TBP0, DBP - dtgt->m_TEX0.TBP0);
		return false;
	}

	const u32 real_src_offset = ((SBP - stgt->m_TEX0.TBP0) / GS_BLOCKS_PER_PAGE) + src_page_offset;
	const u32 real_dst_offset = ((DBP - dtgt->m_TEX0.TBP0) / GS_BLOCKS_PER_PAGE) + dst_page_offset;
	CopyPages(stgt, stgt->m_TEX0.TBW, real_src_offset, dtgt, dtgt->m_TEX0.TBW, real_dst_offset, num_pages);
	return true;
}

void GSTextureCache::CopyPages(Target* src, u32 sbw, u32 src_offset, Target* dst, u32 dbw, u32 dst_offset, u32 num_pages, ShaderConvert shader)
{
	GL_PUSH("TC: GSTextureCache::CopyPages(): %u pages at %x[eff %x] BW %u to %x[eff %x] BW %u", num_pages,
		src->m_TEX0.TBP0, src->m_TEX0.TBP0 + src_offset, sbw, dst->m_TEX0.TBP0, dst->m_TEX0.TBP0 + dst_offset, dbw);

	const GSVector2i& pgs = GSLocalMemory::m_psm[dst->m_TEX0.PSM].pgs;
	const GSVector4i page_rc = GSVector4i::loadh(pgs);
	const GSVector4 src_size = GSVector4(src->GetUnscaledSize()).xyxy();
	const GSVector4 dst_scale = GSVector4(dst->GetScale());
	GSDevice::MultiStretchRect* rects = static_cast<GSDevice::MultiStretchRect*>(alloca(sizeof(GSDevice::MultiStretchRect) * num_pages));
	for (u32 i = 0; i < num_pages; i++)
	{
		const u32 src_page_num = src_offset + i;
		const GSVector2i src_offset = GSVector2i((src_page_num % sbw) * pgs.x, (src_page_num / sbw) * pgs.y);
		const u32 dst_page_num = dst_offset + i;
		const GSVector2i dst_offset = GSVector2i((dst_page_num % dbw) * pgs.x, (dst_page_num / dbw) * pgs.y);

		const GSVector4i src_rect = page_rc + GSVector4i(src_offset).xyxy();
		const GSVector4i dst_rect = page_rc + GSVector4i(dst_offset).xyxy();

		GL_INS("TC: Copy page %u @ <%d,%d=>%d,%d> to %u @ <%d,%d=>%d,%d>", src_page_num, src_rect.x, src_rect.y, src_rect.z,
			src_rect.w, dst_page_num, dst_rect.x, dst_rect.y, dst_rect.z, dst_rect.w);

		GSDevice::MultiStretchRect& rc = rects[i];
		rc.src = src->m_texture;
		rc.src_rect = GSVector4(src_rect) / src_size;
		rc.dst_rect = GSVector4(dst_rect) * dst_scale;
		rc.filter = Nearest;
		rc.wmask.wrgba = 0xf;
	}

	g_gs_device->DrawMultiStretchRects(rects, num_pages, dst->m_texture, shader);
}

GSTextureCache::Target* GSTextureCache::GetExactTarget(u32 BP, u32 BW, int type, u32 end_bp)
{
	auto& rts = m_dst[type];
	for (auto it = rts.begin(); it != rts.end(); ++it)
	{
		Target* t = *it;
		const u32 tgt_bw = std::max(t->m_TEX0.TBW, 1U);
		if ((t->m_TEX0.TBP0 == BP || (GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets && t->m_TEX0.TBP0 < BP && !(BP & 0x1f) && (((BP - t->m_TEX0.TBP0) >> 5) % tgt_bw) == 0)) && tgt_bw == BW && t->UnwrappedEndBlock() >= end_bp)
		{
			rts.MoveFront(it.Index());
			return t;
		}
	}

	return nullptr;
}

GSTextureCache::Target* GSTextureCache::GetTargetWithSharedBits(u32 BP, u32 PSM) const
{
	auto& rts = m_dst[GSLocalMemory::m_psm[PSM].depth ? DepthStencil : RenderTarget];
	for (auto it = rts.begin(); it != rts.end(); ++it)
	{
		Target* t = *it;
		const u32 t_psm = (t->HasValidAlpha()) ? t->m_TEX0.PSM & ~0x1 : t->m_TEX0.PSM;
		if (GSUtil::HasSharedBits(PSM, t_psm) && (t->m_TEX0.TBP0 == BP || (GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets && t->m_TEX0.TBP0 < BP && t->UnwrappedEndBlock() > BP)))
			return t;
	}

	return nullptr;
}

GSTextureCache::Target* GSTextureCache::FindOverlappingTarget(GSTextureCache::Target* target) const
{
	for (int i = 0; i < 2; i++)
	{
		for (Target* tgt : m_dst[i])
		{
			if (tgt == target)
				continue;

			if (CheckOverlap(tgt->m_TEX0.TBP0, tgt->m_end_block, target->m_TEX0.TBP0, target->m_end_block))
				return tgt;
		}
	}

	return nullptr;
}

GSTextureCache::Target* GSTextureCache::FindOverlappingTarget(u32 BP, u32 end_bp) const
{
	for (int i = 0; i < 2; i++)
	{
		for (Target* tgt : m_dst[i])
		{
			if (CheckOverlap(tgt->m_TEX0.TBP0, tgt->m_end_block, BP, end_bp))
				return tgt;
		}
	}

	return nullptr;
}

GSTextureCache::Target* GSTextureCache::FindOverlappingTarget(u32 BP, u32 BW, u32 PSM, GSVector4i rc) const
{
	const u32 end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(BP, BW, PSM, rc);
	return FindOverlappingTarget(BP, end_bp);
}

GSVector2i GSTextureCache::GetTargetSize(u32 bp, u32 fbw, u32 psm, s32 min_width, s32 min_height, bool can_expand)
{
	TargetHeightElem search = {};
	search.bp = bp;
	search.fbw = fbw;
	search.psm = psm;
	search.width = min_width;
	search.height = min_height;

	for (auto it = m_target_heights.begin(); it != m_target_heights.end(); ++it)
	{
		TargetHeightElem& elem = const_cast<TargetHeightElem&>(*it);
		if (elem.bits == search.bits)
		{
			if (can_expand)
			{
				if (elem.width < min_width || elem.height < min_height)
				{
					DbgCon.WriteLn("TC: Expand size at %x %u %u from %ux%u to %ux%u", bp, fbw, psm, elem.width, elem.height,
						min_width, min_height);
				}

				elem.width = std::max(elem.width, min_width);
				elem.height = std::max(elem.height, min_height);
			}

			m_target_heights.MoveFront(it.Index());
			elem.age = 0;
			return GSVector2i(elem.width, elem.height);
		}
	}

	DbgCon.WriteLn("TC: New size at %x %u %u: %ux%u draw %lld", bp, fbw, psm, min_width, min_height, GSState::s_n);
	m_target_heights.push_front(search);
	return GSVector2i(min_width, min_height);
}

bool GSTextureCache::HasTargetInHeightCache(u32 bp, u32 fbw, u32 psm, u32 max_age, bool move_front)
{
	TargetHeightElem search = {};
	search.bp = bp;
	search.fbw = fbw;
	search.psm = psm;

	for (auto it = m_target_heights.begin(); it != m_target_heights.end(); ++it)
	{
		TargetHeightElem& elem = const_cast<TargetHeightElem&>(*it);
		if (elem.bits == search.bits)
		{
			if (elem.age > max_age)
				return false;

			if (move_front)
				m_target_heights.MoveFront(it.Index());

			return true;
		}
	}

	return false;
}

bool GSTextureCache::Has32BitTarget(u32 bp)
{
	for (auto i = m_dst[RenderTarget].begin(); i != m_dst[RenderTarget].end(); ++i)
	{
		const Target* const t = *i;
		if (bp == t->m_TEX0.TBP0 && t->m_32_bits_fmt)
		{
			m_dst[RenderTarget].MoveFront(i.Index());
			return true;
		}
	}

	for (auto i = m_dst[DepthStencil].begin(); i != m_dst[DepthStencil].end(); ++i)
	{
		const Target* const t = *i;
		if (bp == t->m_TEX0.TBP0 && t->m_32_bits_fmt)
		{
			m_dst[DepthStencil].MoveFront(i.Index());
			return true;
		}
	}

	return false;
}

void GSTextureCache::InvalidateVideoMemSubTarget(GSTextureCache::Target* rt)
{
	if (!rt)
		return;

	auto& list = m_dst[RenderTarget];

	for (auto i = list.begin(); i != list.end();)
	{
		Target* t = *i;

		if ((t->m_TEX0.TBP0 > rt->m_TEX0.TBP0) && (t->m_end_block < rt->m_end_block) && (t->m_TEX0.TBW == rt->m_TEX0.TBW) && (t->m_TEX0.TBP0 < t->m_end_block))
		{
			GL_INS("TC: InvalidateVideoMemSubTarget: rt 0x%x -> 0x%x, sub rt 0x%x -> 0x%x",
				rt->m_TEX0.TBP0, rt->m_end_block, t->m_TEX0.TBP0, t->m_end_block);

			InvalidateSourcesFromTarget(t);

			i = list.erase(i);
			delete t;
		}
		else
		{
			++i;
		}
	}
}

void GSTextureCache::InvalidateSourcesFromTarget(const Target* t)
{
	u32 removed = 0;
	for (auto it = m_src.m_surfaces.begin(); it != m_src.m_surfaces.end();)
	{
		Source* src = *it++;
		if (src->m_from_target == t)
		{
			GL_CACHE("TC: Removing source at %x referencing target", src->m_TEX0.TBP0);
			m_src.RemoveAt(src);
			removed++;
		}
	}

	static const bool s_srcguard = (std::getenv("PCSX2_VR_SRCGUARD") != nullptr);
	if (s_srcguard && removed > 0)
	{
		Console.WriteLn("(VR) SRCGUARD invalidate: target 0x%x removed=%u promotion=%d inflight=%p inflight_from=0x%x",
			t->m_TEX0.TBP0, removed, m_in_stereo_promotion ? 1 : 0,
			static_cast<void*>(m_draw_inflight_source),
			(m_draw_inflight_source && m_draw_inflight_source->m_from_target) ?
				m_draw_inflight_source->m_from_target->m_TEX0.TBP0 : 0u);
	}
}

void GSTextureCache::RetargetSourcesAfterPromotion(const Target* t, GSTexture* old_tex, GSTexture* new_tex)
{
	static const bool s_srcguard = (std::getenv("PCSX2_VR_SRCGUARD") != nullptr);

	m_promo_total++;
	if (m_draw_inflight_source)
	{
		m_promo_with_inflight++;
		if (m_draw_inflight_source->m_texture == old_tex || m_draw_inflight_source->m_from_target == t)
		{
			m_promo_inflight_dangerous++;
			if (s_srcguard)
			{
				Console.Error("(VR) SRCGUARD DANGEROUS #%u: promotion of 0x%x with the draw holding "
							  "src bp=0x%x (%s, aliases_old=%d, from_this=%d)",
					m_promo_inflight_dangerous, t->m_TEX0.TBP0, m_draw_inflight_source->m_TEX0.TBP0,
					m_draw_inflight_source->m_shared_texture ? "DIRECT(shared)" : "COPY(owned)",
					(m_draw_inflight_source->m_texture == old_tex) ? 1 : 0,
					(m_draw_inflight_source->m_from_target == t) ? 1 : 0);
			}
		}
	}

	for (Source* src : m_src.m_surfaces)
	{
		if (src->m_texture != old_tex)
			continue;

		pxAssert(src->m_shared_texture);
		if (!src->m_shared_texture)
			continue;

		src->m_texture = new_tex;
		m_promo_retargeted++;
		if (s_srcguard)
		{
			Console.WriteLn("(VR) SRCGUARD RETARGET #%u: map-resident source bp=0x%x re-pointed from %p to %p "
							"(target 0x%x, in_flight=%d)",
				m_promo_retargeted, src->m_TEX0.TBP0, static_cast<void*>(old_tex),
				static_cast<void*>(new_tex), t->m_TEX0.TBP0, (src == m_draw_inflight_source) ? 1 : 0);
		}
	}

	if (m_temporary_source && m_temporary_source->m_texture == old_tex && m_temporary_source->m_shared_texture)
	{
		m_temporary_source->m_texture = new_tex;
		m_promo_retargeted++;
		if (s_srcguard)
		{
			Console.WriteLn("(VR) SRCGUARD RETARGET #%u: TEMPORARY source bp=0x%x re-pointed from %p to %p "
							"(target 0x%x, in_flight=%d)",
				m_promo_retargeted, m_temporary_source->m_TEX0.TBP0, static_cast<void*>(old_tex),
				static_cast<void*>(new_tex), t->m_TEX0.TBP0,
				(m_temporary_source == m_draw_inflight_source) ? 1 : 0);
		}
	}

	if (s_srcguard && (m_promo_total % 4096) == 0)
	{
		Console.WriteLn("(VR) SRCGUARD summary: promotions=%u with_inflight_draw=%u dangerous=%u "
						"retargeted=%u inflight_source_frees=%u",
			m_promo_total, m_promo_with_inflight, m_promo_inflight_dangerous, m_promo_retargeted,
			m_draw_inflight_source_kills);
	}
}

bool GSTextureCache::ForceKillInFlightSourceForSelfTest()
{
	if (!m_draw_inflight_source)
		return false;
	if (m_src.m_surfaces.find(m_draw_inflight_source) == m_src.m_surfaces.end())
		return false;

	Console.Error("(VR) SRCGUARD SELFTEST: deliberately freeing the in-flight draw source %p — "
				  "the guard line below is the proof it fires; this draw is now a real UAF.",
		static_cast<void*>(m_draw_inflight_source));
	m_src.RemoveAt(m_draw_inflight_source);
	return true;
}

void GSTextureCache::ReplaceSourceTexture(Source* s, GSTexture* new_texture, float new_scale,
	const GSVector2i& new_unscaled_size, HashCacheEntry* hc_entry, bool new_texture_is_shared)
{
	pxAssert(!hc_entry || !new_texture_is_shared);

	if (s->m_from_hash_cache)
	{
		pxAssert(s->m_from_hash_cache->refcount > 0);
		if ((--s->m_from_hash_cache->refcount) == 0)
			s->m_from_hash_cache->age = 0;
	}
	else if (!s->m_shared_texture)
	{
		m_source_memory_usage -= s->m_texture->GetMemUsage();
		g_gs_device->Recycle(s->m_texture);
	}

	s->m_texture = new_texture;
	s->m_shared_texture = new_texture_is_shared;
	s->m_from_hash_cache = hc_entry;
	s->m_unscaled_size = new_unscaled_size;
	s->m_scale = new_scale;

	if (s->m_from_hash_cache)
		s->m_from_hash_cache->refcount++;
	else if (!s->m_shared_texture)
		m_source_memory_usage += s->m_texture->GetMemUsage();
}

void GSTextureCache::IncAge()
{
	static constexpr int max_age = 3;
	static constexpr int max_preload_age = 30;

	for (auto i = m_src.m_surfaces.begin(); i != m_src.m_surfaces.end();)
	{
		Source* s = *i;

		++i;
		if (++s->m_age > ((!s->m_from_hash_cache && s->CanPreload()) ? max_preload_age : max_age))
			m_src.RemoveAt(s);
	}

	AgeHashCache();

	static constexpr int max_rt_age = 60;

	static constexpr int max_size_age = 120;

	for (int type = 0; type < 2; type++)
	{
		auto& list = m_dst[type];
		for (auto i = list.begin(); i != list.end();)
		{
			Target* t = *i;

			if (++t->m_age > max_rt_age)
			{
				const Target* overlapping_tgt = FindOverlappingTarget(t);

				if (overlapping_tgt != nullptr || t->m_dirty.GetTotalRect(t->m_TEX0, GSVector2i(t->m_valid.width(), t->m_valid.height())).rintersect(t->m_valid).eq(t->m_valid))
				{
					i = list.erase(i);
					GL_CACHE("TC: Remove Target(%s): (0x%x) due to age", to_string(type),
						t->m_TEX0.TBP0);

					delete t;
				}
				else
				{
					GL_CACHE("TC: Extending life of target for %x", t->m_TEX0.TBP0);
					t->m_age = 10;
					++i;
				}
			}
			else
			{
				++i;
			}
		}
	}

	for (auto it = m_target_heights.begin(); it != m_target_heights.end();)
	{
		TargetHeightElem& elem = const_cast<TargetHeightElem&>(*it);
		if (elem.age >= max_size_age)
		{
			it = m_target_heights.erase(it);
		}
		else
		{
			elem.age++;
			++it;
		}
	}
}

GSTextureCache::Source* GSTextureCache::CreateSource(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const GIFRegCLAMP& CLAMP, Target* dst, int x_offset, int y_offset, const GSVector2i* lod, const GSVector4i* src_range, GSTexture* gpu_clut, SourceRegion region, bool force_temp)
{
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	Source* src = new Source(TEX0, TEXA);

	static constexpr bool force_target_copy = false;

	int tw = std::max(region.IsFixedTEX0W(1 << TEX0.TW) ? static_cast<int>(region.GetWidth()) : (1 << TEX0.TW), dst ? (1 << TEX0.TW) : 0);
	int th = std::max(region.IsFixedTEX0H(1 << TEX0.TH) ? static_cast<int>(region.GetHeight()) : (1 << TEX0.TH), dst ? (1 << TEX0.TH) : 0);

	int tlevels = 1;
	if (lod)
	{
		tlevels = GSConfig.HWMipmap ? std::min(lod->y - lod->x + 1, GSDevice::GetMipmapLevelsForSize(tw, th)) : -1;
		src->m_lod = *lod;
	}

	bool hack = false;
	bool channel_shuffle = dst && (TEX0.PSM == PSMT8) && (GSRendererHW::GetInstance()->TestChannelShuffle(dst));

	if (dst && (x_offset != 0 || y_offset != 0) && (TEX0.PSM != PSMT8 || channel_shuffle))
	{
		const float scale = dst->m_scale;
		const int x = static_cast<int>(scale * x_offset);
		const int y = static_cast<int>(scale * y_offset);
		const int w = static_cast<int>(std::ceil(scale * tw));
		const int h = static_cast<int>(std::ceil(scale * th));

		const GSVector4i read_rect = GSVector4i(x_offset, y_offset, x_offset + tw, y_offset + th);
		if (!dst->m_dirty.empty() && !read_rect.rintersect(dst->m_dirty.GetTotalRect(dst->m_TEX0, dst->m_unscaled_size)).rempty())
			dst->Update();

		if constexpr (force_target_copy)
		{
			const bool outside_target = ((x + w) > dst->m_texture->GetWidth() || (y + h) > dst->m_texture->GetHeight());
			GSTexture::Usage usage = outside_target ? dst->m_texture->GetUsage() : GSTexture::Texture;
			GSTexture* sTex = dst->m_texture;
			GSTexture* dTex = g_gs_device->FetchSurface(usage, w, h, outside_target ? 1 : tlevels, sTex->GetFormat(), true, PreferReusedLabelledTexture(), sTex->GetArrayLayers());
			if (!dTex) [[unlikely]]
			{
				Console.Error("Failed to allocate %dx%d texture for offset source", w, h);
				delete src;
				return nullptr;
			}

			m_target_memory_usage += dTex->GetMemUsage();

			const GSVector4i area(GSVector4i(x, y, x + w, y + h).rintersect(GSVector4i(sTex->GetSize()).zwxy()));
			if (!area.rempty())
			{
				if (dst->m_rt_alpha_scale)
				{
					const GSVector4 sRectF = GSVector4(area) / GSVector4(1, 1, sTex->GetWidth(), sTex->GetHeight());
					g_gs_device->StretchRect(sTex, sRectF, dTex, GSVector4(area), ShaderConvert::RTA_DECORRECTION, Nearest);
				}
				else
					g_gs_device->CopyRect(sTex, dTex, area, 0, 0);
			}

			src->m_texture = dTex;
			src->m_unscaled_size = GSVector2i(tw, th);
		}
		else
		{
			GL_CACHE("TC: Sample offset (%d,%d) reduced region directly from target: %dx%d -> %dx%d @ %d,%d",
				dst->m_texture->GetWidth(), x_offset, y_offset, dst->m_texture->GetHeight(), w, h, x_offset, y_offset);

			if (!GSRendererHW::GetInstance()->IsTBPFrameOrZ(TEX0.TBP0) || !channel_shuffle)
			{
				if (x_offset < 0)
					src->m_region.SetX(x_offset, region.GetMaxX() + x_offset);
				else
					src->m_region.SetX(x_offset, x_offset + tw);
				if (y_offset < 0)
					src->m_region.SetY(y_offset, region.GetMaxY() + y_offset);
				else
					src->m_region.SetY(y_offset, y_offset + th);
			}

			src->m_target_direct = true;
			src->m_texture = dst->m_texture;
			src->m_unscaled_size = dst->m_unscaled_size;
			src->m_shared_texture = true;

			if (channel_shuffle)
				m_temporary_source = src;
		}

		if (GSRendererHW::GetInstance()->IsTBPFrameOrZ(dst->m_TEX0.TBP0))
			m_temporary_source = src;

		src->m_scale = scale;
		src->m_end_block = dst->m_end_block;
		src->m_target = true;
		src->m_from_target = dst;
		src->m_from_target_TEX0 = dst->m_TEX0;

		src->m_valid_alpha_minmax = true;
		if ((src->m_TEX0.PSM & 0xf) == PSMCT24)
		{
			src->m_alpha_minmax.first = TEXA.AEM ? 0 : TEXA.TA0;
			src->m_alpha_minmax.second = TEXA.TA0;
		}
		else
		{
			src->m_alpha_minmax.first = dst->m_alpha_min;
			src->m_alpha_minmax.second = dst->m_alpha_max;

			if (!dst->m_32_bits_fmt)
			{
				const bool using_both = (src->m_alpha_minmax.first ^ src->m_alpha_minmax.second) & 128;
				const bool using_ta1 = (src->m_alpha_minmax.second & 128);

				src->m_alpha_minmax.first = TEXA.AEM ? 0 : (using_both ? std::min(TEXA.TA1, TEXA.TA0) : (using_ta1 ? TEXA.TA1 : TEXA.TA0));
				src->m_alpha_minmax.second = (using_both ? std::max(TEXA.TA1, TEXA.TA0) : (using_ta1 ? TEXA.TA1 : TEXA.TA0));
			}
		}
		src->m_32_bits_fmt = dst->m_32_bits_fmt;

		if (psm.pal > 0)
		{
			AttachPaletteToSource(src, psm.pal, true, true);
		}

#ifdef PCSX2_DEVBUILD
		if (GSConfig.UseDebugDevice)
		{
			if (psm.pal > 0)
			{
				src->m_texture->SetDebugName(TinyString::from_format("Offset {},{} from 0x{:X} {} CBP 0x{:X}", x_offset, y_offset,
					static_cast<u32>(TEX0.TBP0), GSUtil::GetPSMName(TEX0.PSM), static_cast<u32>(TEX0.CBP)));
			}
			else
			{
				src->m_texture->SetDebugName(TinyString::from_format("Offset {},{} from 0x{:X} {} ", x_offset, y_offset,
					static_cast<u32>(TEX0.TBP0), GSUtil::GetPSMName(TEX0.PSM), static_cast<u32>(TEX0.CBP)));
			}
		}
#endif
	}
	else if (dst)
	{

		ShaderConvertSelector shader = GetConvertShader(dst->m_texture->GetFormat(), GSTexture::Format::Color);
		channel_shuffle = GSRendererHW::GetInstance()->TestChannelShuffle(dst);

		const bool is_8bits = TEX0.PSM == PSMT8 && !channel_shuffle;
		if (is_8bits)
		{
			GL_INS("TC: Reading RT as a packed-indexed 8 bits format");
			shader = GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp == 16 ? ShaderConvert::RGB5A1_TO_8I : ShaderConvert::RGBA_TO_8I;
		}

#ifdef ENABLE_OGL_DEBUG
		if (TEX0.PSM == PSMT4)
		{
			GL_INS("TC: ERROR: Reading RT as a packed-indexed 4 bits format is not supported");
		}
#endif

		if (GSLocalMemory::m_psm[TEX0.PSM].bpp > 8)
		{
			src->m_32_bits_fmt = dst->m_32_bits_fmt;
		}

		src->m_target = true;
		src->m_unscaled_size = GSVector2i(std::min(dst->m_unscaled_size.x, tw), std::min(dst->m_unscaled_size.y, th));
		src->m_from_target = dst;
		src->m_from_target_TEX0 = dst->m_TEX0;
		src->m_valid_rect = dst->m_valid;
		src->m_end_block = dst->m_end_block;

		if (!dst->m_dirty.empty() && !src_range->rintersect(dst->m_dirty.GetTotalRect(dst->m_TEX0, dst->m_unscaled_size)).rempty())
			dst->Update();

		src->m_valid_alpha_minmax = true;
		if ((src->m_TEX0.PSM & 0xf) == PSMCT24)
		{
			src->m_alpha_minmax.first = TEXA.AEM ? 0 : TEXA.TA0;
			src->m_alpha_minmax.second = TEXA.TA0;
		}
		else
		{
			src->m_alpha_minmax.first = dst->m_alpha_min;
			src->m_alpha_minmax.second = dst->m_alpha_max;

			if (!dst->m_32_bits_fmt)
			{
				const bool using_both = (src->m_alpha_minmax.first ^ src->m_alpha_minmax.second) & 128;
				const bool using_ta1 = (src->m_alpha_minmax.second & 128);

				src->m_alpha_minmax.first = TEXA.AEM ? 0 : (using_both ? std::min(TEXA.TA1, TEXA.TA0) : (using_ta1 ? TEXA.TA1 : TEXA.TA0));
				src->m_alpha_minmax.second = (using_both ? std::max(TEXA.TA1, TEXA.TA0) : (using_ta1 ? TEXA.TA1 : TEXA.TA0));
			}
		}
		src->m_32_bits_fmt = dst->m_32_bits_fmt;

		GSVector2i new_size(
			std::min(static_cast<int>(std::ceil(static_cast<float>(src->m_unscaled_size.x) * dst->m_scale)),
				dst->m_texture->GetWidth()),
			std::min(static_cast<int>(std::ceil(static_cast<float>(src->m_unscaled_size.y) * dst->m_scale)),
				dst->m_texture->GetHeight()));

		if (is_8bits)
		{
			if (dst->m_TEX0.TBP0 == TEX0.TBP0)
			{
				src->m_unscaled_size.x = tw;
				src->m_unscaled_size.y = th;
				new_size.x = tw;
				new_size.y = th;
			}
			else
			{
				src->m_unscaled_size.x = dst->m_unscaled_size.x * 2;
				if (GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp == 32)
					src->m_unscaled_size.y = dst->m_unscaled_size.y * 2;
				else
					src->m_unscaled_size.y = dst->m_unscaled_size.y;

				new_size.x = src->m_unscaled_size.x;
				new_size.y = src->m_unscaled_size.y;
			}
		}

		if (dst->m_TEX0.TBW != TEX0.TBW)
		{

		}
		else if (tw < 1024)
		{
			hack = true;
		}

		const float scale = is_8bits ? 1.0f : dst->m_scale;
		src->m_scale = scale;

		GSVector4i sRect = GSVector4i::loadh(new_size);
		int destX = 0;
		int destY = 0;

		const bool source_rect_empty = sRect.rempty();
		const bool use_texture = (shader.Shader() == ShaderConvert::COPY && !source_rect_empty);
		GSVector4i region_rect = GSVector4i(0, 0, tw, th);

		const GSVector2i dst_texture_size = dst->m_texture->GetSize();
		if (use_texture &&
			!force_target_copy)
		{
			src->m_texture = dst->m_texture;
			src->m_target_direct = true;
			src->m_scale = dst->m_scale;
			src->m_unscaled_size = dst->m_unscaled_size;
			src->m_shared_texture = true;
			src->m_32_bits_fmt = dst->m_32_bits_fmt;

			if (new_size != dst_texture_size)
			{
				GL_CACHE("TC: Sample reduced region directly from target: %dx%d -> %dx%d", dst_texture_size.x,
					dst_texture_size.y, new_size.x, new_size.y);

				if (new_size.x != dst_texture_size.x)
					src->m_region.SetX(region_rect.x, region_rect.z);
				if (new_size.y != dst_texture_size.y)
					src->m_region.SetY(region_rect.y, region_rect.w);
			}

			if (GSRendererHW::GetInstance()->IsTBPFrameOrZ(dst->m_TEX0.TBP0) || channel_shuffle)
			{
				GL_CACHE("TC: Source is RT or ZBUF, invalidating after draw.");
				m_temporary_source = src;
			}
		}
		else
		{
			GSTexture::Usage usage = use_texture ? GSTexture::Texture : dst->m_texture->GetUsage();
			GSTexture* sTex = dst->m_texture;
			const u32 vr_src_layers = is_8bits ? 1u : sTex->GetArrayLayers();
			GSTexture* dTex = g_gs_device->FetchSurface(usage, new_size, 1, sTex->GetFormat(), source_rect_empty || destX != 0 || destY != 0, PreferReusedLabelledTexture(), vr_src_layers);
			if (!dTex) [[unlikely]]
			{
				Console.Error("Failed to allocate %dx%d texture for target copy to source", new_size.x, new_size.y);
				delete src;
				return nullptr;
			}

			src->m_shared_texture = false;
			src->m_target_direct = false;
			m_target_memory_usage += dTex->GetMemUsage();
			src->m_texture = dTex;

			if (use_texture)
			{
				if (dst->m_rt_alpha_scale)
				{
					const GSVector4 sRectF = GSVector4(sRect) / GSVector4(1, 1, sTex->GetWidth(), sTex->GetHeight());
					g_gs_device->StretchRect(
						sTex, sRectF, dTex, GSVector4(destX, destY, sRect.width(), sRect.height()), ShaderConvert::RTA_DECORRECTION, Nearest);
				}
				else
					g_gs_device->CopyRect(sTex, dTex, sRect, destX, destY);

#ifdef PCSX2_DEVBUILD
				if (GSConfig.UseDebugDevice)
				{
					src->m_texture->SetDebugName(TinyString::from_format("{}x{} copy of 0x{:X} {}", new_size.x, new_size.y,
						static_cast<u32>(TEX0.TBP0), GSUtil::GetPSMName(TEX0.PSM)));
				}
#endif
			}
			else if (!source_rect_empty)
			{
				if (is_8bits)
				{
					if (dst->m_rt_alpha_scale)
					{
						dst->UnscaleRTAlpha();
						sTex = dst->m_texture;
					}

					const u32 destination_tbw = (dst->m_TEX0.TBP0 == TEX0.TBP0) ? (std::max<u32>(TEX0.TBW, 1u) * 64) : std::max<u32>(dst->m_TEX0.TBW, 1u) * 128;
					if (!GSConfig.UserHacks_NativePaletteDraw && dst->GetScale() > 1.0f)
					{
						GSTexture::Usage usage = use_texture ? GSTexture::Texture : dst->m_texture->GetUsage();
						GSTexture* tmpTex = g_gs_device->FetchSurface(usage, dst->m_unscaled_size, 1, dst->m_texture->GetFormat(), false, PreferReusedLabelledTexture());

						const GSVector4 dRect = GSVector4(GSVector4i::loadh(dst->m_unscaled_size));

						if (GSConfig.UserHacks_NativeScaling != GSNativeScaling::Off)
						{
							const u32 downsample_factor = static_cast<u32>(dst->GetScale());
							
							g_gs_device->FilteredDownsampleTexture(dst->m_texture, tmpTex, downsample_factor, GSVector2i(0, 0), dRect);
						}
						else
							g_gs_device->StretchRectAuto(sTex, tmpTex, dRect, Nearest);

						g_gs_device->ConvertToIndexedTexture(tmpTex, 1, x_offset, y_offset,
							std::max<u32>(dst->m_TEX0.TBW, 1u) * 64, dst->m_TEX0.PSM, dTex,
							destination_tbw, TEX0.PSM);

						g_gs_device->Recycle(tmpTex);
					}
					else
						g_gs_device->ConvertToIndexedTexture(sTex, dst->GetScale(), x_offset, y_offset,
							std::max<u32>(dst->m_TEX0.TBW, 1u) * 64, dst->m_TEX0.PSM, dTex,
							destination_tbw, TEX0.PSM);

					x_offset *= 2;
					if (GSLocalMemory::m_psm[dst->m_TEX0.PSM].bpp == 32)
						y_offset *= 2;

					src->m_region.SetX(x_offset, x_offset + tw);
					src->m_region.SetY(y_offset, y_offset + th);

					if (!GSConfig.UserHacks_NativePaletteDraw && tlevels > 1 && ((tw >> 1) > dst->m_valid.z || (th >> 1) > dst->m_valid.w))
						m_temporary_source = src;
				}
				else
				{
					if (dst->m_rt_alpha_scale && shader.Shader() == ShaderConvert::COPY)
						shader = ShaderConvert::RTA_DECORRECTION;

					const GSVector4 sRectF = GSVector4(sRect) / GSVector4(1, 1, sTex->GetWidth(), sTex->GetHeight());
					g_gs_device->StretchRect(sTex, sRectF, dTex, GSVector4(destX, destY, new_size.x, new_size.y), shader, Nearest);
				}

#ifdef PCSX2_DEVBUILD
				if (GSConfig.UseDebugDevice)
				{
					if (psm.pal > 0)
					{
						src->m_texture->SetDebugName(TinyString::from_format("Reinterpret 0x{:X} from {} to {} CBP 0x{:X}",
							static_cast<u32>(TEX0.TBP0), GSUtil::GetPSMName(dst->m_TEX0.PSM), GSUtil::GetPSMName(TEX0.PSM), static_cast<u32>(TEX0.CBP)));
					}
					else
					{
						src->m_texture->SetDebugName(TinyString::from_format("Reinterpret 0x{:X} from {} to {}",
							static_cast<u32>(TEX0.TBP0), GSUtil::GetPSMName(dst->m_TEX0.PSM), GSUtil::GetPSMName(TEX0.PSM)));
					}
				}
#endif
			}
		}

		if (psm.pal > 0)
		{
			AttachPaletteToSource(src, psm.pal, true, true);
		}

		dst->OffsetHack_modxy = hack ? g_gs_renderer->GetModXYOffset() : 0.0f;
	}
	else
	{
		if (GSUtil::GetChannelMask(TEX0.PSM) == 0xf && TEX0.TBP0 != GSRendererHW::GetInstance()->GetCachedCtx()->FRAME.Block() && TEX0.TBP0 != GSRendererHW::GetInstance()->GetCachedCtx()->ZBUF.Block() && !force_temp)
		{
			g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, TEX0.TBP0, TEX0.PSM, GSRendererHW::GetInstance()->GetCachedCtx()->FRAME.FBMSK, true);
			g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, TEX0.TBP0, TEX0.PSM, GSRendererHW::GetInstance()->GetCachedCtx()->FRAME.FBMSK, true);
		}

		if (force_temp || (GSRendererHW::GetInstance()->IsTBPFrameOrZ(TEX0.TBP0, true) && GSRendererHW::GetInstance()->ChannelsSharedTEX0FRAME()))
		{
			GL_CACHE("TC: Source == RT before RT creation, invalidating after draw.");
			m_temporary_source = src;
		}

		if (!region.HasEither() && m_temporary_source == nullptr && CLAMP.WMS == CLAMP_CLAMP && CLAMP.WMT == CLAMP_CLAMP)
		{
			if (src_range->z < tw && src_range->w < th)
			{
				auto& transfers = GSRendererHW::GetInstance()->m_draw_transfers;

				if (transfers.size() > 0)
				{
					const int last_draw = transfers.back().draw;

					for (auto iter = transfers.rbegin(); iter != transfers.rend(); ++iter)
					{
						if (last_draw - iter->draw > 10)
							break;

						if (iter->blit.DBP == TEX0.TBP0 && iter->blit.DBW == TEX0.TBW && iter->blit.DPSM == TEX0.PSM)
						{
							const int new_max_x = std::min(tw, std::max(iter->rect.z, Common::AlignUpPow2(src_range->z, psm.pgs.x)));
							const int new_max_y = std::min(th, std::max(iter->rect.w, Common::AlignUpPow2(src_range->w, psm.pgs.y)));

							if (new_max_x != tw && new_max_y != th)
							{
								region.SetX(0, new_max_x);
								region.SetY(0, new_max_y);
							}

							break;
						}
					}
				}
			}
		}

		bool paltex = (GSConfig.GPUPaletteConversion && psm.pal > 0) || gpu_clut;
		const u32* clut = (psm.pal > 0) ? static_cast<const u32*>(g_gs_renderer->m_mem.m_clut) : nullptr;

		tw = region.HasX() ? region.GetWidth() : tw;
		th = region.HasY() ? region.GetHeight() : th;
		src->m_region = region;
		src->m_unscaled_size = GSVector2i(tw, th);
		src->m_scale = 1.0f;

		if ((src->m_from_hash_cache = LookupHashCache(TEX0, TEXA, paltex, clut, lod, region)) != nullptr)
		{
			src->m_texture = src->m_from_hash_cache->texture;
			src->m_alpha_minmax = src->m_from_hash_cache->alpha_minmax;
			src->m_valid_alpha_minmax = src->m_from_hash_cache->valid_alpha_minmax;

			if (gpu_clut)
				AttachPaletteToSource(src, gpu_clut);
			else if (psm.pal > 0)
				AttachPaletteToSource(src, psm.pal, paltex, false);
		}
		else if (paltex)
		{
			src->m_texture = g_gs_device->CreateTexture(tw, th, tlevels, GSTexture::Format::UNorm8);
			if (!src->m_texture) [[unlikely]]
			{
				Console.Error("Failed to allocate %dx%d paltex texture", tw, th);
				delete src;
				return nullptr;
			}

			m_source_memory_usage += src->m_texture->GetMemUsage();
			if (gpu_clut)
				AttachPaletteToSource(src, gpu_clut);
			else
				AttachPaletteToSource(src, psm.pal, true, true);
		}
		else
		{
			src->m_texture = g_gs_device->CreateTexture(tw, th, tlevels, GSTexture::Format::Color);
			if (!src->m_texture) [[unlikely]]
			{
				Console.Error("Failed to allocate %dx%d source texture", tw, th);
				delete src;
				return nullptr;
			}

			m_source_memory_usage += src->m_texture->GetMemUsage();
			if (gpu_clut)
				AttachPaletteToSource(src, gpu_clut);
			else if (psm.pal > 0)
				AttachPaletteToSource(src, psm.pal, false, true);
		}

#ifdef PCSX2_DEVBUILD
		if (GSConfig.UseDebugDevice)
		{
			if (psm.pal > 0)
			{
				src->m_texture->SetDebugName(TinyString::from_format("{}x{} {} @ 0x{:X} TBW={} CBP=0x{:X}",
					tw, th, GSUtil::GetPSMName(TEX0.PSM), static_cast<u32>(TEX0.TBP0), static_cast<u32>(TEX0.TBW),
					static_cast<u32>(TEX0.CBP)));
			}
			else
			{
				src->m_texture->SetDebugName(TinyString::from_format("{}x{} {} @ 0x{:X} TBW={}",
					tw, th, GSUtil::GetPSMName(TEX0.PSM), static_cast<u32>(TEX0.TBP0), static_cast<u32>(TEX0.TBW)));
			}
		}
#endif
	}

	pxAssert(src->m_texture);
	pxAssert(src->m_target == (dst != nullptr));
	pxAssert(src->m_from_target == dst);
	pxAssert(src->m_scale == ((!dst || (TEX0.PSM == PSMT8 && !channel_shuffle)) ? 1.0f : dst->m_scale));

	if (src != m_temporary_source)
	{
		src->SetPages();
		m_src.Add(src, TEX0);
	}

	return src;
}

GSTextureCache::Source* GSTextureCache::CreateMergedSource(GIFRegTEX0 TEX0, GIFRegTEXA TEXA, SourceRegion region, float scale)
{
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[TEX0.PSM];
	const int tex_width = (std::max<int>(64 * TEX0.TBW, region.GetMaxX()) + (psm_s.bs.x - 1)) & ~(psm_s.bs.x - 1);
	const int tex_height = ((region.HasY() ? region.GetHeight() : (1 << TEX0.TH)) + (psm_s.bs.y - 1)) & ~(psm_s.bs.y - 1);
	const int scaled_width = static_cast<int>(static_cast<float>(tex_width) * scale);
	const int scaled_height = static_cast<int>(static_cast<float>(tex_height) * scale);

	const u32 end_block = GSLocalMemory::m_psm[TEX0.PSM].info.bn(tex_width - 1, tex_height - 1, TEX0.TBP0, TEX0.TBW);
	GL_PUSH("TC: Merging targets from %x through %x", TEX0.TBP0, end_block);

	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	const int page_width = psm.pgs.x;
	const int page_height = psm.pgs.y;
	constexpr int page_blocks = 32;

	const int row_page_increment = TEX0.TBW;
	const int page_block_increment = row_page_increment * page_blocks;
	const int width_in_pages = (tex_width + (page_width - 1)) / page_width;
	const int height_in_pages = (tex_height + (page_height - 1)) / page_height;
	const int num_pages = width_in_pages * height_in_pages;
	const GSVector4i page_rect(GSVector4i(psm.pgs).zwxy());

	const GSOffset lm_off(psm.info, TEX0.TBP0, TEX0.TBW, TEX0.PSM);
	GSTexture* lmtex = nullptr;
	GSTexture::GSMap lmtex_map;
	bool lmtex_mapped = false;

	u8* pages_done = static_cast<u8*>(alloca((num_pages + 7) / 8));
	std::memset(pages_done, 0, (num_pages + 7) / 8);

	GSDevice::MultiStretchRect* copy_queue =
		static_cast<GSDevice::MultiStretchRect*>(alloca(sizeof(GSDevice::MultiStretchRect) * num_pages * 2));
	u32 copy_count = 0;

	u32 start_TBP0 = TEX0.TBP0;
	u32 current_TBP0 = start_TBP0;
	int page_x = 0;
	int page_y = 0;

	auto preload_page = [&TEXA, scale, &psm, &lm_off, &lmtex, &lmtex_map, &lmtex_mapped,
							page_width, page_height, tex_width, tex_height, copy_queue, &copy_count](int dst_x, int dst_y) {
		if (!lmtex)
		{
			lmtex = g_gs_device->CreateTexture(tex_width, tex_height, 1, GSTexture::Format::Color, false);
			if (!lmtex) [[unlikely]]
			{
				Console.Error("Failed to allocate %dx%d texture for page preloading", tex_width, tex_height);
				return;
			}

			lmtex_mapped = lmtex->Map(lmtex_map);
		}

		const GSVector4i rect(
			dst_x, dst_y, std::min(dst_x + page_width, tex_width), std::min(dst_y + page_height, tex_height));

		if (lmtex_mapped)
		{
			psm.rtx(g_gs_renderer->m_mem, lm_off, rect, lmtex_map.bits + dst_y * lmtex_map.pitch + dst_x * sizeof(u32),
				lmtex_map.pitch, TEXA);
		}
		else
		{
			const int pitch = page_width * sizeof(u32);
			psm.rtx(g_gs_renderer->m_mem, lm_off, rect, s_unswizzle_buffer, pitch, TEXA);
			lmtex->Update(rect, s_unswizzle_buffer, pitch);
		}

		const bool linear = (scale != 1.0f);
		copy_queue[copy_count++] = {GSVector4(rect) / GSVector4(lmtex->GetSize()).xyxy(),
			GSVector4(rect) * GSVector4(scale).xyxy(), lmtex, BilnIf(linear), 0xf};
	};

	for (int page_num = 0; page_num < num_pages; page_num++)
	{
		const u32 this_start_block = current_TBP0;
		const u32 this_end_block = (current_TBP0 + page_blocks) - 1;
		bool found = false;

		if (pages_done[page_num / 8] & (1u << (page_num % 8)))
			goto next_page;

		GL_INS("TC: Searching for block range %x - %x for (%u,%u)", this_start_block, this_end_block, page_x * page_width,
			page_y * page_height);

		for (auto i = m_dst[RenderTarget].begin(); i != m_dst[RenderTarget].end(); ++i)
		{
			Target* const t = *i;
			if (this_start_block >= t->m_TEX0.TBP0 && this_end_block <= t->m_end_block && GSUtil::HasCompatibleBits(t->m_TEX0.PSM, TEX0.PSM))
			{
				GL_INS("TC: Candidate at BP %x BW %d PSM %d", t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM);

				GSVector4i src_rect(page_rect);
				bool copy_multiple_pages = (t->m_TEX0.TBW == TEX0.TBW && page_x < row_page_increment);
				int available_pages_x = 1;
				int available_pages_y = 1;
				if (copy_multiple_pages)
				{
					available_pages_x = (row_page_increment - page_x);
					available_pages_y = (height_in_pages - page_y);
					src_rect.z = available_pages_x * page_width;
					src_rect.w = available_pages_y * page_height;
				}

				const GSVector4i t_rect(t->m_valid.insert64<0>(0));

				SurfaceOffset so;
				if (this_start_block > t->m_TEX0.TBP0)
				{
					SurfaceOffsetKey sok;
					sok.elems[0].bp = this_start_block;
					sok.elems[0].bw = TEX0.TBW;
					sok.elems[0].psm = TEX0.PSM;
					sok.elems[0].rect = src_rect;
					sok.elems[1].bp = t->m_TEX0.TBP0;
					sok.elems[1].bw = t->m_TEX0.TBW;
					sok.elems[1].psm = t->m_TEX0.PSM;
					sok.elems[1].rect = t_rect;
					so = ComputeSurfaceOffset(sok);
					if (!so.is_valid)
						goto next_page;
				}
				else
				{
					so.is_valid = true;
					so.b2a_offset = src_rect.rintersect(t_rect);
				}

				if (copy_multiple_pages)
				{
					available_pages_x = std::min(available_pages_x, (so.b2a_offset.width() + (psm.pgs.x - 1)) / psm.pgs.x);
					available_pages_y = std::min(available_pages_y, (so.b2a_offset.height() + (psm.pgs.y - 1)) / psm.pgs.y);
				}

				const bool linear = (scale != t->m_scale);
				const int src_x_end = so.b2a_offset.z;
				const int src_y_end = so.b2a_offset.w;
				int src_y = so.b2a_offset.y;
				int dst_y = page_y * page_height;
				int current_copy_page = page_num;

				for (int copy_page_y = 0; copy_page_y < available_pages_y; copy_page_y++)
				{
					if (src_y >= src_y_end)
						break;

					const int wanted_height = std::min(tex_height - dst_y, page_height);
					const int copy_height = std::min(src_y_end - src_y, wanted_height);
					int src_x = so.b2a_offset.x;
					int dst_x = page_x * page_width;
					int row_page = current_copy_page;
					pxAssert(dst_y < tex_height && copy_height > 0);

					for (int copy_page_x = 0; copy_page_x < available_pages_x; copy_page_x++)
					{
						if (src_x >= src_x_end)
							break;

						if ((pages_done[row_page / 8] & (1u << (row_page % 8))) == 0)
						{
							pages_done[row_page / 8] |= (1u << (row_page % 8));

							const int wanted_width = std::min(tex_width - dst_x, page_width);
							const int copy_width = std::min(src_x_end - src_x, wanted_width);
							pxAssert(dst_x < tex_width && copy_width > 0);

							if (GSConfig.PreloadFrameWithGSData &&
								(copy_width < wanted_width || copy_height < wanted_height))
							{
								preload_page(dst_x, dst_y);
							}

							GL_INS("TC:  Copy from %d,%d -> %d,%d (%dx%d)", src_x, src_y, dst_x, dst_y, copy_width, copy_height);
							copy_queue[copy_count++] = {
								(GSVector4(src_x, src_y, src_x + copy_width, src_y + copy_height) *
									GSVector4(t->m_scale).xyxy()) /
									GSVector4(t->m_texture->GetSize()).xyxy(),
								GSVector4(dst_x, dst_y, dst_x + copy_width, dst_y + copy_height) *
									GSVector4(scale).xyxy(),
								t->m_texture, BilnIf(linear), 0xf};
						}

						row_page++;
						src_x += page_width;
						dst_x += page_width;
					}

					current_copy_page += width_in_pages;
					src_y += page_height;
					dst_y += page_height;
				}

				found = true;
				break;
			}
		}

		if (!found)
		{
			if (GSConfig.PreloadFrameWithGSData)
			{
				pages_done[page_num / 8] |= (1u << (page_num % 8));

				GL_INS("TC:  *** NOT FOUND, preloading from local memory");
				const int dst_x = page_x * page_width;
				const int dst_y = page_y * page_height;
				preload_page(dst_x, dst_y);
			}
			else
			{
				GL_INS("TC:  *** NOT FOUND");
			}
		}

	next_page:
		current_TBP0 += page_blocks;
		page_x++;
		if (page_x == width_in_pages)
		{
			start_TBP0 += page_block_increment;
			current_TBP0 = start_TBP0;
			page_x = 0;
			page_y++;
		}
	}

	if (copy_count == 0)
	{
		GL_INS("TC: No sources found.");
		return nullptr;
	}

	if (lmtex_mapped)
		lmtex->Unmap();

	GSTexture* dtex = g_gs_device->CreateFeedbackTarget(scaled_width, scaled_height, GSTexture::Format::Color, true);
	if (!dtex) [[unlikely]]
	{
		Console.Error("Failed to allocate %dx%d merged dest texture", scaled_width, scaled_height);
		return nullptr;
	}
	DevCon.Warning("Merged %d", m_source_memory_usage);
	m_source_memory_usage += dtex->GetMemUsage();

	g_gs_device->SortMultiStretchRects(copy_queue, copy_count);
	g_gs_device->DrawMultiStretchRects(copy_queue, copy_count, dtex);

	if (lmtex)
		g_gs_device->Recycle(lmtex);

	Source* src = new Source(TEX0, TEXA);
	src->m_texture = dtex;
	src->m_scale = scale;
	src->m_unscaled_size = GSVector2i(tex_width, tex_height);
	src->m_end_block = end_block;
	src->m_target = true;

	const GSOffset offset = g_gs_renderer->m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
	src->m_pages = offset.pageLooperForRect(GSVector4i(0, 0, tex_width, tex_height));
	m_src.Add(src, TEX0);

	return src;
}

extern bool FMVstarted;

GSTextureCache::HashCacheEntry* GSTextureCache::LookupHashCache(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, bool& paltex, const u32* clut, const GSVector2i* lod, SourceRegion region)
{
	const bool dump = GSConfig.DumpReplaceableTextures && (!FMVstarted || GSConfig.DumpTexturesWithFMVActive) &&
	                  (clut ? GSConfig.DumpPaletteTextures : GSConfig.DumpDirectTextures);
	const bool replace = GSConfig.LoadTextureReplacements && GSTextureReplacements::HasAnyReplacementTextures();
	bool can_cache = (TEX0.PSM >= PSMT8H && TEX0.PSM <= PSMT4HH) ? CanPreloadTextureSize(TEX0.TW, TEX0.TH) : CanCacheTextureSize(TEX0.TW, TEX0.TH);
	if (!dump && !replace && !can_cache)
		return nullptr;

	HashCacheKey key{HashCacheKey::Create(TEX0, TEXA, (dump || replace || !paltex) ? clut : nullptr, lod, region)};

	if (dump)
	{
		GSTextureReplacements::DumpTexture(key, TEX0, TEXA, region, g_gs_renderer->m_mem, 0);

		if (lod && GSConfig.DumpReplaceableMipmaps)
		{
			const int basemip = lod->x;
			const int nmips = lod->y - lod->x + 1;
			for (int mip = 1; mip < nmips; mip++)
			{
				const GIFRegTEX0 MIP_TEX0{g_gs_renderer->GetTex0Layer(basemip + mip)};
				GSTextureReplacements::DumpTexture(key, MIP_TEX0, TEXA, region, g_gs_renderer->m_mem, mip);
			}
		}
	}

	auto it = m_hash_cache.find(key);

	const bool needs_second_lookup = paltex && (dump || replace);
	if (needs_second_lookup && it == m_hash_cache.end())
		it = m_hash_cache.find(key.WithRemovedCLUTHash());

	if (it != m_hash_cache.end())
	{
		GL_CACHE("TC: HC Hit: %" PRIx64 " %" PRIx64 " R-%ux%u", key.TEX0Hash, key.CLUTHash, key.region_width, key.region_height);
		HashCacheEntry* entry = &it->second;
		paltex &= (entry->texture->GetFormat() == GSTexture::Format::UNorm8);
		entry->refcount++;
		return entry;
	}

	GL_CACHE("TC: HC Miss: %" PRIx64 " %" PRIx64 " R-%ux%u", key.TEX0Hash, key.CLUTHash, key.region_width, key.region_height);

	if (replace)
	{
		bool replacement_texture_pending = false;
		std::pair<u8, u8> alpha_minmax;
		GSTexture* replacement_tex = GSTextureReplacements::LookupReplacementTexture(key, lod != nullptr,
			&replacement_texture_pending, &alpha_minmax);
		if (replacement_tex)
		{
			paltex = false;
			const HashCacheEntry entry{replacement_tex, 1u, 0u, alpha_minmax, true, true};
			m_hash_cache_replacement_memory_usage += entry.texture->GetMemUsage();
			return &m_hash_cache.emplace(key, entry).first->second;
		}
		else if (
			replacement_texture_pending ||

			(paltex && GSTextureReplacements::HasReplacementTextureWithOtherPalette(key)))
		{
			paltex = false;

			can_cache = true;
		}
	}

	if (paltex && !can_cache && TEX0.TW <= MAXIMUM_TEXTURE_HASH_CACHE_SIZE && TEX0.TH <= MAXIMUM_TEXTURE_HASH_CACHE_SIZE)
	{
		paltex &= !dump;

		can_cache = true;
	}

	if (!can_cache)
		return nullptr;

	const int tw = region.HasX() ? region.GetWidth() : (1 << TEX0.TW);
	const int th = region.HasY() ? region.GetHeight() : (1 << TEX0.TH);
	const int tlevels = lod ? (GSConfig.HWMipmap ? std::min(lod->y - lod->x + 1, GSDevice::GetMipmapLevelsForSize(tw, th)) : -1) : 1;
	GSTexture* tex = g_gs_device->CreateTexture(tw, th, tlevels, paltex ? GSTexture::Format::UNorm8 : GSTexture::Format::Color);
	if (!tex)
	{
		return nullptr;
	}

	const bool compute_alpha_minmax = !paltex;
	std::pair<u8, u8> alpha_minmax = {0u, 255u};

	PreloadTexture(TEX0, TEXA, region, g_gs_renderer->m_mem, paltex, tex, 0, compute_alpha_minmax ? &alpha_minmax : nullptr);

	if (lod)
	{
		const int basemip = lod->x;
		for (int mip = 1; mip < tlevels; mip++)
		{
			const GIFRegTEX0 MIP_TEX0{g_gs_renderer->GetTex0Layer(basemip + mip)};
			std::pair<u8, u8> mip_alpha_minmax;
			PreloadTexture(MIP_TEX0, TEXA, region.AdjustForMipmap(mip), g_gs_renderer->m_mem, paltex, tex, mip,
				compute_alpha_minmax ? &mip_alpha_minmax : nullptr);
			if (compute_alpha_minmax)
			{
				alpha_minmax.first = std::min(alpha_minmax.first, mip_alpha_minmax.first);
				alpha_minmax.second = std::max(alpha_minmax.second, mip_alpha_minmax.second);
			}
		}

		tex->ClearMipmapGenerationFlag();
	}

	if (paltex)
		key.RemoveCLUTHash();

	const HashCacheEntry entry{tex, 1u, 0u, alpha_minmax, compute_alpha_minmax, false};
	m_hash_cache_memory_usage += tex->GetMemUsage();
	return &m_hash_cache.emplace(key, entry).first->second;
}

GSTextureCache::HashCacheMap::iterator GSTextureCache::RemoveFromHashCache(HashCacheMap::iterator it)
{
	HashCacheEntry& e = it->second;
	const u32 mem_usage = e.texture->GetMemUsage();
	if (e.is_replacement)
		m_hash_cache_replacement_memory_usage -= mem_usage;
	else
		m_hash_cache_memory_usage -= mem_usage;
	g_gs_device->Recycle(e.texture);
	return m_hash_cache.erase(it);
}

void GSTextureCache::AgeHashCache()
{
	constexpr u32 MAX_HASH_CACHE_SIZE = 800;
	constexpr u32 MAX_HASH_CACHE_AGE = 30;

	bool might_need_cache_purge = (m_hash_cache.size() > MAX_HASH_CACHE_SIZE);
	if (might_need_cache_purge)
		s_hash_cache_purge_list.clear();

	for (auto it = m_hash_cache.begin(); it != m_hash_cache.end();)
	{
		HashCacheEntry& e = it->second;
		if (e.refcount > 0)
		{
			++it;
			continue;
		}

		if (++e.age > MAX_HASH_CACHE_AGE)
		{
			it = RemoveFromHashCache(it);
			continue;
		}

		if (might_need_cache_purge)
		{
			might_need_cache_purge = (m_hash_cache.size() > MAX_HASH_CACHE_SIZE);
			if (might_need_cache_purge)
				s_hash_cache_purge_list.emplace_back(it, static_cast<s32>(e.age));
		}

		++it;
	}

	if (might_need_cache_purge)
	{
		std::sort(s_hash_cache_purge_list.begin(), s_hash_cache_purge_list.end(),
			[](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

		const u32 entries_to_purge = std::min(static_cast<u32>(m_hash_cache.size() - MAX_HASH_CACHE_SIZE),
			static_cast<u32>(s_hash_cache_purge_list.size()));
		for (u32 i = 0; i < entries_to_purge; i++)
			RemoveFromHashCache(s_hash_cache_purge_list[i].first);
	}
}

GSTextureCache::Target* GSTextureCache::Target::Create(GIFRegTEX0 TEX0, int w, int h, float scale, int type, bool clear)
{
	pxAssert(type == RenderTarget || type == DepthStencil);

	const int scaled_w = static_cast<int>(std::ceil(static_cast<float>(w) * scale));
	const int scaled_h = static_cast<int>(std::ceil(static_cast<float>(h) * scale));
	GSTexture::Usage usage = type == RenderTarget ? GSTexture::FeedbackTarget :
	                         (g_gs_device->Features().depth_feedback ? GSTexture::FeedbackDepth : GSTexture::DepthStencil);
	GSTexture::Format format = type == RenderTarget ? GSTexture::Format::Color : GSTexture::Format::DepthStencil;
	u32 layers = 1;
#ifdef ENABLE_VR
	if (type == RenderTarget && g_gs_device->SupportsStereoTargets() &&
		VR::StereoState::Get().enabled && g_texture_cache->IsDisplayChainBP(TEX0.TBP0))
	{
		layers = 2;
	}
#endif
	GSTexture* texture = g_gs_device->FetchSurface(usage, scaled_w, scaled_h, 1, format, clear, PreferReusedLabelledTexture(), layers);
	if (!texture)
		return nullptr;

	Target* t = new Target(TEX0, type, GSVector2i(w, h), scale, texture);

	g_texture_cache->m_target_memory_usage += t->m_texture->GetMemUsage();

	g_texture_cache->m_dst[type].push_front(t);

	t->UpdateTextureDebugName();

	return t;
}

GSTexture* GSTextureCache::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	for (auto t : m_dst[RenderTarget])
	{
		if (!t->m_used)
			continue;

		GSVector2i this_offset;
		if (t->m_TEX0.TBP0 == CBP)
		{
			if (t->m_TEX0.PSM != CPSM || (CBW != 0 && t->m_TEX0.TBW != CBW))
				continue;

			GL_INS("TC: Exact match on BP 0x%04x BW %u", t->m_TEX0.TBP0, t->m_TEX0.TBW);
			this_offset.x = 0;
			this_offset.y = 0;

			if (GSConfig.UserHacks_NativeScaling != GSNativeScaling::Off && t->m_scale > 1.0f)
			{
				GSTexture* tex = g_gs_device->CreateCompatible(t->m_texture, t->GetUnscaledSize(), false);
				if (!tex)
					return nullptr;

				g_gs_device->StretchRectAuto(t->m_texture, tex, Nearest);
				m_target_memory_usage = (m_target_memory_usage - t->m_texture->GetMemUsage()) + tex->GetMemUsage();
				g_gs_device->Recycle(t->m_texture);
			
				t->m_texture = tex;
				t->m_scale = 1.0f;
				t->m_downscaled = true;
			}
		}
		else if (GSConfig.UserHacks_GPUTargetCLUTMode == GSGPUTargetCLUTMode::InsideTarget &&
				 t->m_TEX0.TBP0 < CBP && t->m_end_block >= CBP)
		{
			const GSVector4i rc(0, 0, size.x, size.y);
			SurfaceOffset so = ComputeSurfaceOffset(CBP, std::max<u32>(CBW, 0), CPSM, rc, t);
			if (!so.is_valid)
				continue;

			GL_INS("TC: Match inside RT at BP 0x%04X-0x%04X BW %u", t->m_TEX0.TBP0, t->m_end_block, t->m_TEX0.TBW);
			this_offset.x = so.b2a_offset.left;
			this_offset.y = so.b2a_offset.top;
		}
		else
		{
			continue;
		}

		if (!t->m_dirty.empty())
		{
			GL_INS("TC: Candidate is dirty, checking");

			const GSVector4i clut_rc(this_offset.x, this_offset.y, this_offset.x + size.x, this_offset.y + size.y);
			bool is_dirty = false;
			for (GSDirtyRect& dirty : t->m_dirty)
			{
				if (!dirty.GetDirtyRect(t->m_TEX0, false).rintersect(clut_rc).rempty())
				{
					GL_INS("TC: Dirty rectangle overlaps CLUT rectangle, skipping");
					is_dirty = true;
					break;
				}
			}
			if (is_dirty)
				continue;
		}

		GSRendererHW::GetInstance()->m_mem.m_clut.SetGPUTextureDirty(t->m_last_draw, t->m_texture);

		offset = this_offset;
		*scale = t->m_scale;

		t->UnscaleRTAlpha();

		return t->m_texture;
	}

	return nullptr;
}

std::shared_ptr<GSTextureCache::Palette> GSTextureCache::LookupPaletteObject(const u32* clut, u16 pal, bool need_gs_texture)
{
	return m_palette_map.LookupPalette(clut, pal, need_gs_texture);
}

void GSTextureCache::Read(Target* t, const GSVector4i& r)
{
	if ((!t->m_dirty.empty() && !t->m_dirty.GetTotalRect(t->m_TEX0, t->m_unscaled_size).rintersect(r).rempty()) || r.width() == 0 || r.height() == 0)
		return;

	const GIFRegTEX0& TEX0 = t->m_TEX0;
	const bool is_depth = (t->m_type == DepthStencil);

	GSTexture::Format fmt;
	ShaderConvert ps_shader;
	std::unique_ptr<GSDownloadTexture>* dltex;
	switch (TEX0.PSM)
	{
		case PSMCT32:
		case PSMCT24:
		case PSMZ32:
		case PSMZ24:
		{
			if (is_depth)
			{
				fmt = GSTexture::Format::UInt32;
				ps_shader = ShaderConvert::DEPTH32_TO_32_BITS;
				dltex = &m_uint32_download_texture;
			}
			else
			{
				fmt = GSTexture::Format::Color;
				if (t->m_rt_alpha_scale)
					ps_shader = ShaderConvert::RTA_DECORRECTION;
				else
					ps_shader = ShaderConvert::COPY;

				dltex = &m_color_download_texture;
			}
		}
		break;

		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S:
		{
			fmt = GSTexture::Format::UInt16;
			ps_shader = is_depth ? ShaderConvert::DEPTH32_TO_16_BITS : ShaderConvert::RGB5A1_TO_16_BITS;
			dltex = &m_uint16_download_texture;
		}
		break;

		default:
			return;
	}

	const u32 write_mask = (t->m_valid_rgb ? 0x00FFFFFFu : 0) | (t->m_valid_alpha_low ? 0x0F000000u : 0) | (t->m_valid_alpha_high ? 0xF0000000u : 0);
	if (write_mask == 0)
	{
		DbgCon.Warning("Not reading back target %x PSM %s due to no write mask", TEX0.TBP0, GSUtil::GetPSMName(TEX0.PSM));
		return;
	}

	GL_PERF("TC: Read Back Target: (0x%x)[fmt: 0x%x]. Size %dx%d", TEX0.TBP0, TEX0.PSM, r.width(), r.height());

	const GSVector4 src(GSVector4(r) * GSVector4(t->m_scale) / GSVector4(t->m_texture->GetSize()).xyxy());
	const GSVector4i drc(0, 0, r.width(), r.height());
	const bool direct_read = t->m_type == RenderTarget && t->m_scale == 1.0f && ps_shader == ShaderConvert::COPY;

	if (!PrepareDownloadTexture(drc.z, drc.w, fmt, dltex))
		return;

	if (direct_read)
	{
		dltex->get()->CopyFromTexture(drc, t->m_texture, r, 0, true);
	}
	else
	{
		GSTexture* tmp = g_gs_device->CreateRenderTarget(drc.z, drc.w, fmt, false);
		if (tmp)
		{
			g_gs_device->StretchRect(t->m_texture, src, tmp, GSVector4(drc), ps_shader, Nearest);
			dltex->get()->CopyFromTexture(drc, tmp, drc, 0, true);
			g_gs_device->Recycle(tmp);
		}
		else
		{
			Console.Error("Failed to allocate temporary %dx%d target for read.", drc.z, drc.w);
			return;
		}
	}

	dltex->get()->Flush();
	if (!dltex->get()->Map(drc))
		return;

	const GSOffset off = g_gs_renderer->m_mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);
	u8* bits = const_cast<u8*>(dltex->get()->GetMapPointer());
	const u32 pitch = dltex->get()->GetMapPitch();

	switch (TEX0.PSM)
	{
		case PSMCT32:
		case PSMZ32:
		case PSMCT24:
		case PSMZ24:
			g_gs_renderer->m_mem.WritePixel32(bits, pitch, off, r, write_mask);
			break;
		case PSMCT16:
		case PSMCT16S:
		case PSMZ16:
		case PSMZ16S:
			g_gs_renderer->m_mem.WritePixel16(bits, pitch, off, r);
			break;

		default:
			Console.Error("Unknown PSM %u on Read", TEX0.PSM);
			break;
	}

	dltex->get()->Unmap();
}

void GSTextureCache::Read(Source* t, const GSVector4i& r)
{
	if (r.rempty())
		return;

	const GSVector4i drc(0, 0, r.width(), r.height());

	if (!PrepareDownloadTexture(drc.z, drc.w, GSTexture::Format::Color, &m_color_download_texture))
		return;

	m_color_download_texture->CopyFromTexture(drc, t->m_texture, r, 0, true);
	m_color_download_texture->Flush();

	if (m_color_download_texture->Map(drc))
	{
		const GSOffset off = g_gs_renderer->m_mem.GetOffset(t->m_TEX0.TBP0, t->m_TEX0.TBW, t->m_TEX0.PSM);
		g_gs_renderer->m_mem.WritePixel32(
			const_cast<u8*>(m_color_download_texture->GetMapPointer()), m_color_download_texture->GetMapPitch(), off, r);
		m_color_download_texture->Unmap();
	}
}

GSTextureCache::Surface::Surface() = default;

GSTextureCache::Surface::~Surface() = default;

bool GSTextureCache::Surface::Inside(u32 bp, u32 bw, u32 psm, const GSVector4i& rect) const
{
	const GSOffset off(GSLocalMemory::m_psm[psm].info, bp, bw, psm);
	const u32 start_block = off.bnNoWrap(rect.x, rect.y);
	const u32 end_block = off.bnNoWrap(rect.z - 1, rect.w - 1);
	return start_block >= m_TEX0.TBP0 && end_block <= UnwrappedEndBlock();
}

bool GSTextureCache::Surface::OverlapsHelper(u32 start_block0, u32 end_block0, u32 bp, u32 bw, u32 psm, const GSVector4i& rect) const
{
	const GSOffset off(GSLocalMemory::m_psm[psm].info, bp, bw, psm);

	u32 end_block = off.bnNoWrap(rect.z - 1, rect.w - 1);
	u32 start_block = off.bnNoWrap(rect.x, rect.y);

	const GSVector2i page_size = GSLocalMemory::m_psm[psm].pgs;
	if ((rect.z & (page_size.x - 1)) == 0 && (rect.w & (page_size.y - 1)) == 0)
	{
		constexpr u32 page_mask = (1 << 5) - 1;
		end_block = (((end_block + page_mask) & ~page_mask)) - 1;
	}
	else if (end_block < start_block && ((start_block - end_block) < (1 << 5)))
	{
		std::swap(start_block, end_block);
	}

	if (end_block > GS_MAX_BLOCKS && bp < m_end_block && m_end_block < m_TEX0.TBP0)
		bp += GS_MAX_BLOCKS;

	return GSTextureCache::CheckOverlap(start_block0, end_block0, start_block, end_block);
}

bool GSTextureCache::Surface::Overlaps(u32 bp, u32 bw, u32 psm, const GSVector4i& rect) const
{
	return OverlapsHelper(m_TEX0.TBP0, UnwrappedEndBlock(), bp, bw, psm, rect);
}

GSTextureCache::Source::Source(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA)
{
	m_TEX0 = TEX0;
	m_TEXA = TEXA;
}

GSTextureCache::Source::~Source()
{
	_aligned_free(m_write.rect);

	if (!m_shared_texture && !m_from_hash_cache && m_texture)
	{
		if (m_from_target)
			g_texture_cache->m_target_memory_usage -= m_texture->GetMemUsage();
		else
			g_texture_cache->m_source_memory_usage -= m_texture->GetMemUsage();
		g_gs_device->Recycle(m_texture);
	}
}

bool GSTextureCache::Source::IsPaletteFormat() const
{
	return (GSLocalMemory::m_psm[m_TEX0.PSM].pal > 0);
}

void GSTextureCache::Source::SetPages()
{
	const int tw = 1 << m_TEX0.TW;
	const int th = 1 << m_TEX0.TH;

	m_repeating = !m_from_hash_cache && m_TEX0.IsRepeating() && !m_region.IsFixedTEX0(tw, th);

	if (m_repeating && !m_target && !CanPreload())
	{
		m_p2t = g_gs_renderer->m_mem.GetPage2TileMap(m_TEX0);
	}

	const GSOffset offset =
		(m_target && m_region.HasEither()) ?
			g_gs_renderer->m_mem.GetOffset(m_from_target_TEX0.TBP0, m_from_target_TEX0.TBW, m_from_target_TEX0.PSM) :
			g_gs_renderer->m_mem.GetOffset(m_TEX0.TBP0, m_TEX0.TBW, m_TEX0.PSM);
	const GSVector4i rect = m_region.GetRect(tw, th);
	m_pages = offset.pageLooperForRect(rect);
}

void GSTextureCache::Source::Update(const GSVector4i& rect, int level)
{
	m_age = 0;
	if (m_from_target)
		m_from_target->m_age = 0;

	if (m_target || m_from_hash_cache || (m_complete_layers & (1u << level)))
		return;

	if (CanPreload())
	{
		PreloadLevel(level);
		return;
	}

	const GSVector2i& bs = GSLocalMemory::m_psm[m_TEX0.PSM].bs;
	const int tw = 1 << m_TEX0.TW;
	const int th = 1 << m_TEX0.TH;

	GSVector4i r(rect);
	const GSVector4i region_rect(m_region.GetRect(tw, th));

	if (m_region.HasEither())
		r = r.rintersect(region_rect);

	r = r.ralign<Align_Outside>(bs);

	if (region_rect.eq(r.rintersect(region_rect)))
		m_complete_layers |= (1u << level);

	const GSOffset off = g_gs_renderer->m_mem.GetOffset(m_TEX0.TBP0, m_TEX0.TBW, m_TEX0.PSM);
	GSOffset::BNHelper bn = off.bnMulti(r.left, r.top);

	u32 blocks = 0;

	if (!m_valid)
		m_valid = std::make_unique<u32[]>(GS_MAX_PAGES);

	if (m_repeating)
	{
		for (int y = r.top; y < r.bottom; y += bs.y, bn.nextBlockY())
		{
			for (int x = r.left; x < r.right; bn.nextBlockX(), x += bs.x)
			{
				const int i = (bn.blkY() << 7) + bn.blkX();
				const u32 addr = i % GS_MAX_BLOCKS;

				const u32 row = addr >> 5u;
				const u32 col = 1 << (addr & 31u);

				if ((m_valid[row] & col) == 0)
				{
					m_valid[row] |= col;

					Write(GSVector4i(x, y, x + bs.x, y + bs.y), level, off);

					blocks++;
				}
			}
		}
	}
	else
	{
		for (int y = r.top; y < r.bottom; y += bs.y, bn.nextBlockY())
		{
			for (int x = r.left; x < r.right; x += bs.x, bn.nextBlockX())
			{
				const u32 block = bn.value();
				const u32 row = block >> 5u;
				const u32 col = 1 << (block & 31u);

				if ((m_valid[row] & col) == 0)
				{
					m_valid[row] |= col;

					Write(GSVector4i(x, y, x + bs.x, y + bs.y), level, off);

					blocks++;
				}
			}
		}
	}

	if (blocks > 0)
	{
		g_perfmon.Put(GSPerfMon::Unswizzle, bs.x * bs.y * blocks << (m_palette ? 2 : 0));
		Flush(m_write.count, level, off);
	}
}

void GSTextureCache::Source::UpdateLayer(const GIFRegTEX0& TEX0, const GSVector4i& rect, int layer)
{
	if (layer > 6)
		return;

	if (m_target)
		return;

	if (TEX0 == m_layer_TEX0[layer])
		return;

	const GIFRegTEX0 old_TEX0 = m_TEX0;

	m_layer_TEX0[layer] = TEX0;
	m_TEX0 = TEX0;

	Update(rect, layer);

	m_TEX0 = old_TEX0;
}

void GSTextureCache::Source::Write(const GSVector4i& r, int layer, const GSOffset& off)
{
	if (!m_write.rect)
		m_write.rect = static_cast<GSVector4i*>(_aligned_malloc(3 * sizeof(GSVector4i), 16));

	m_write.rect[m_write.count++] = r;

	while (m_write.count >= 2)
	{
		GSVector4i& a = m_write.rect[m_write.count - 2];
		GSVector4i& b = m_write.rect[m_write.count - 1];

		if ((a == b.zyxw()).mask() == 0xfff0)
		{
			a.right = b.right;

			m_write.count--;
		}
		else if ((a == b.xwzy()).mask() == 0xff0f)
		{
			a.bottom = b.bottom;

			m_write.count--;
		}
		else
		{
			break;
		}
	}

	if (m_write.count > 2)
	{
		Flush(1, layer, off);
	}
}

void GSTextureCache::Source::Flush(u32 count, int layer, const GSOffset& off)
{
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[m_TEX0.PSM];
	const SourceRegion region((layer == 0) ? m_region : m_region.AdjustForMipmap(layer));

	const GSVector2i tex0_tw_th = GSVector2i(std::min(static_cast<int>(m_TEX0.TW), 11), std::min(static_cast<int>(m_TEX0.TH), 11));
	const int tw = std::max(region.GetWidth(), 1 << tex0_tw_th.x);
	const int th = std::max(region.GetHeight(), 1 << tex0_tw_th.y);
	const GSVector4i tex_r(region.GetRect(tw, th));

	int pitch = std::max(tw, psm.bs.x) * sizeof(u32);

	GSLocalMemory& mem = g_gs_renderer->m_mem;

	GSLocalMemory::readTexture rtx = psm.rtx;

	if (m_palette)
	{
		pitch >>= 2;
		rtx = psm.rtxP;
	}

	pitch = VectorAlign(pitch);

	for (u32 i = 0; i < count; i++)
	{
		const GSVector4i r(m_write.rect[i]);

		if (((r > tex_r).mask() & 0xff00) == 0 && ((tex_r > r).mask() & 0x00ff) == 0)
		{
			GSTexture::GSMap m;
			const GSVector4i map_r(r - tex_r.xyxy());
			if (m_texture->Map(m, &map_r, layer))
			{
				rtx(mem, off, r, m.bits, m.pitch, m_TEXA);
				m_texture->Unmap();
				continue;
			}
		}

		const GSVector4i rint(r.rintersect(tex_r));
		if (rint.width() == 0 || rint.height() == 0)
			continue;

		rtx(mem, off, r, s_unswizzle_buffer, pitch, m_TEXA);

		const u8* src = s_unswizzle_buffer + (pitch * static_cast<u32>(std::max(tex_r.top - r.top, 0))) +
		                (static_cast<u32>(std::max(tex_r.left - r.left, 0)) << (m_palette ? 0 : 2));
		m_texture->Update(rint - tex_r.xyxy(), src, pitch, layer);
	}

	if (count < m_write.count)
	{
		memmove(&m_write.rect[0], &m_write.rect[count], (m_write.count - count) * sizeof(m_write.rect[0]));
	}

	m_write.count -= count;
}

void GSTextureCache::Source::PreloadLevel(int level)
{
	const HashType hash = HashTexture(m_TEX0, m_TEXA, m_region);

	const u8 layer_bit = static_cast<u8>(1) << level;
	m_complete_layers |= layer_bit;

	if ((m_valid_hashes & layer_bit) && m_layer_hash[level] == hash)
		return;

	m_valid_hashes |= layer_bit;
	m_layer_hash[level] = hash;

	if (!m_valid_alpha_minmax)
	{
		PreloadTexture(m_TEX0, m_TEXA, m_region.AdjustForMipmap(level), g_gs_renderer->m_mem, m_palette != nullptr,
			m_texture, level, nullptr);
	}
	else
	{
		std::pair<u8, u8> mip_alpha_minmax;
		PreloadTexture(m_TEX0, m_TEXA, m_region.AdjustForMipmap(level), g_gs_renderer->m_mem, m_palette != nullptr,
			m_texture, level, &mip_alpha_minmax);
		m_alpha_minmax.first = std::min(m_alpha_minmax.first, mip_alpha_minmax.first);
		m_alpha_minmax.second = std::max(m_alpha_minmax.second, mip_alpha_minmax.second);
	}
}

bool GSTextureCache::Source::ClutMatch(const PaletteKey& palette_key)
{
	return PaletteKeyEqual()(palette_key, m_palette_obj->GetPaletteKey());
}

GSTextureCache::Target::Target(GIFRegTEX0 TEX0, int type, const GSVector2i& unscaled_size, float scale, GSTexture* texture)
	: m_type(type)
	, m_used(type == RenderTarget)
	, m_valid(GSVector4i::zero())
{
	m_TEX0 = TEX0;
	m_end_block = m_TEX0.TBP0;
	m_unscaled_size = unscaled_size;
	m_scale = scale;
	m_texture = texture;
	m_downscaled = scale == 1.0f && g_gs_renderer->GetUpscaleMultiplier() > 1.0f;

	if ((m_TEX0.PSM & 0xf) == PSMCT24)
	{
		m_alpha_min = 128;
		m_alpha_max = 128;
	}
	else
	{
		m_alpha_min = 0;
		m_alpha_max = 0;
	}
	m_32_bits_fmt |= (GSLocalMemory::m_psm[TEX0.PSM].trbpp != 16);
}

GSTextureCache::Target::~Target()
{
	pxAssert(!m_shared_texture);

	if (m_texture)
	{
		g_texture_cache->m_target_memory_usage -= m_texture->GetMemUsage();
		g_gs_device->Recycle(m_texture);
	}

#ifdef PCSX2_DEVBUILD
	for (GSTextureCache::Source* src : g_texture_cache->m_src.m_surfaces)
	{
		if (src->m_from_target == this)
		{
			pxFail(fmt::format("Source at TBP {:x} for target at TBP {:x} on target invalidation",
				static_cast<u32>(src->m_TEX0.TBP0), static_cast<u32>(m_TEX0.TBP0)
			).c_str());
			break;
		}
	}
#endif
}

bool GSTextureCache::Target::OverlapsValid(u32 bp, u32 bw, u32 psm, const GSVector4i& rect) const
{
	const u32 valid_start_block = GSLocalMemory::GetStartBlockAddress(m_TEX0.TBP0, m_TEX0.TBW, m_TEX0.PSM, m_valid);
	return OverlapsHelper(valid_start_block, UnwrappedEndBlock(), bp, bw, psm, rect);
}

void GSTextureCache::Target::Update(bool cannot_scale)
{
	if (m_texture && m_texture->GetArrayLayers() >= 2 && !m_dirty.empty())
	{
		static const bool s_census = (std::getenv("PCSX2_VR_CHAINLOG") != nullptr);
		static const bool s_skip = (std::getenv("PCSX2_VR_SKIP_LAYERED_UPDATE") != nullptr);
		if (s_census)
		{
			for (u32 di = 0; di < static_cast<u32>(m_dirty.size()); di++)
			{
				const GSVector4i dr = m_dirty.GetDirtyRect(di, m_TEX0, GSVector4i::loadh(m_unscaled_size), false);
				Console.WriteLn("(VR) CHAINLOG layered-update bp=0x%x rect=%d,%d-%d,%d skip=%d",
					m_TEX0.TBP0, dr.x, dr.y, dr.z, dr.w, s_skip ? 1 : 0);
			}
		}
		if (s_skip)
		{
			m_dirty.clear();
			return;
		}
	}
	m_age = 0;

	if (m_dirty.empty())
		return;

	if (m_type == DepthStencil && GSConfig.UserHacks_DisableDepthSupport)
	{
		GL_INS("TC: ERROR: Update DepthStencil dummy");
		m_dirty.clear();
		return;
	}

	const GSVector4i total_rect = m_dirty.GetTotalRect(m_TEX0, m_unscaled_size);
	if (total_rect.rempty())
	{
		GL_INS("TC: ERROR: Nothing to update?");
		m_dirty.clear();
		return;
	}

	const GSVector4i t_offset(total_rect.xyxy());
	const GSVector4i t_size(total_rect - t_offset);
	const GSVector4 t_sizef(t_size.zwzw());

	GSTexture* const t = g_gs_device->CreateTexture(t_size.z, t_size.w, 1, GSTexture::Format::Color);
	if (!t) [[unlikely]]
	{
		Console.Error("Failed to allocate %dx%d for update source", t_size.z, t_size.w);
		return;
	}

	GSTexture::GSMap m;
	const bool mapped = t->Map(m);

	GIFRegTEXA TEXA = {};
	TEXA.AEM = 0;
	TEXA.TA0 = 0;
	TEXA.TA1 = 0x80;

	const bool upscaled = (m_scale != 1.0f);
	const bool override_linear = (upscaled && GSConfig.UserHacks_BilinearHack == GSBilinearDirtyMode::ForceBilinear);
	const bool linear = (m_type == RenderTarget && upscaled && GSConfig.UserHacks_BilinearHack != GSBilinearDirtyMode::ForceNearest);

	GSDevice::MultiStretchRect* drects = static_cast<GSDevice::MultiStretchRect*>(
		alloca(sizeof(GSDevice::MultiStretchRect) * static_cast<u32>(m_dirty.size())));
	u32 ndrects = 0;

	const GSOffset off(g_gs_renderer->m_mem.GetOffset(m_TEX0.TBP0, m_TEX0.TBW, m_TEX0.PSM));
	const u32 bpp = GSLocalMemory::m_psm[m_TEX0.PSM].bpp;

	std::pair<u8, u8> alpha_minmax = {255, 0};
	bool transferring_alpha = false;

	for (size_t i = 0; i < m_dirty.size(); i++)
	{
		const GSVector4i update_r = m_dirty.GetDirtyRect(i, m_TEX0, total_rect, false);
		if (update_r.rempty())
			continue;

		transferring_alpha |= m_dirty[i].rgba.c.a;

		const GSVector4i read_r = m_dirty.GetDirtyRect(i, m_TEX0, total_rect, true);
		const GSVector4i t_r(read_r - t_offset);
		if (mapped)
		{
			if ((m_TEX0.PSM & 0xf) != PSMCT24 && m_dirty[i].rgba.c.a && bpp >= 16)
			{
				const int pitch = VectorAlign(read_r.width() * sizeof(u32));
				g_gs_renderer->m_mem.ReadTexture(off, read_r, s_unswizzle_buffer, pitch, TEXA);

				std::pair<u8, u8> new_alpha_minmax = GSGetRGBA8AlphaMinMax(s_unswizzle_buffer, read_r.width(), read_r.height(), pitch);
				alpha_minmax.first = std::min(alpha_minmax.first, new_alpha_minmax.first);
				alpha_minmax.second = std::max(alpha_minmax.second, new_alpha_minmax.second);
			}

			g_gs_renderer->m_mem.ReadTexture(
				off, read_r, m.bits + t_r.y * static_cast<u32>(m.pitch) + (t_r.x * sizeof(u32)), m.pitch, TEXA);
		}
		else
		{
			const int pitch = VectorAlign(read_r.width() * sizeof(u32));
			g_gs_renderer->m_mem.ReadTexture(off, read_r, s_unswizzle_buffer, pitch, TEXA);

			if ((m_TEX0.PSM & 0xf) != PSMCT24 && m_dirty[i].rgba.c.a && bpp >= 16)
			{
				std::pair<u8, u8> new_alpha_minmax = GSGetRGBA8AlphaMinMax(s_unswizzle_buffer, read_r.width(), read_r.height(), pitch);
				alpha_minmax.first = std::min(alpha_minmax.first, new_alpha_minmax.first);
				alpha_minmax.second = std::max(alpha_minmax.second, new_alpha_minmax.second);
			}

			t->Update(t_r, s_unswizzle_buffer, pitch);
		}

		GSDevice::MultiStretchRect& drect = drects[ndrects++];
		drect.src = t;
		drect.src_rect = GSVector4(update_r - t_offset) / t_sizef;
		drect.dst_rect = GSVector4(update_r) * GSVector4(m_scale);
		drect.filter = BilnIf(linear && (m_dirty[i].req_linear || override_linear));

		if (m_type == RenderTarget)
		{
			GL_INS("TC: ERROR: Update RenderTarget 0x%x bw:%d (%d,%d => %d,%d)", m_TEX0.TBP0, m_TEX0.TBW,
				update_r.x, update_r.y, update_r.z, update_r.w);
			drect.wmask = static_cast<u8>(m_dirty[i].rgba._u32);
		}
		else if (m_type == DepthStencil)
		{
			GL_INS("TC: ERROR: Update DepthStencil 0x%x", m_TEX0.TBP0);
			drect.wmask = 0xF;
		}
	}

	if (mapped)
		t->Unmap();

	if (ndrects > 0)
	{
		if (m_type == RenderTarget && transferring_alpha && bpp >= 16)
		{
			if (alpha_minmax.second > 128 || (m_TEX0.PSM & 0xf) == PSMCT24)
				UnscaleRTAlpha();
			else if (!cannot_scale && total_rect.rintersect(m_valid).eq(m_valid))
				m_rt_alpha_scale = true;
		}

		const bool linear = upscaled && GSConfig.UserHacks_BilinearHack != GSBilinearDirtyMode::ForceNearest;

		const ShaderConvertSelector shader = (m_type == RenderTarget) ?
			(m_rt_alpha_scale ? ShaderConvert::RTA_CORRECTION : ShaderConvert::COPY) :
			GetConvertShader(GSTexture::Format::Color, m_texture->GetFormat(), bpp, bpp, linear);

		g_gs_device->DrawMultiStretchRects(drects, ndrects, m_texture, shader);
	}

	if (transferring_alpha && bpp >= 16)
	{
		if (m_dirty.size() != 1 || !total_rect.eq(m_valid))
		{
			m_alpha_min = std::min(static_cast<int>(alpha_minmax.first), m_alpha_min);
			m_alpha_max = std::max(static_cast<int>(alpha_minmax.second), m_alpha_max);
		}
		else
		{
			m_alpha_min = alpha_minmax.first;
			m_alpha_max = alpha_minmax.second;
		}

		m_alpha_range |= alpha_minmax.first != alpha_minmax.second;
	}
	g_gs_device->Recycle(t);

	if (m_type == DepthStencil && g_texture_cache->GetTemporaryZ() != nullptr)
	{
		if (g_texture_cache->GetTemporaryZInfo().ZBP == m_TEX0.TBP0)
		{
			const GSTextureCache::TempZAddress z_address_info = g_texture_cache->GetTemporaryZInfo();
			if (m_TEX0.TBP0 == z_address_info.ZBP)
			{
				const GSVector4i dRect = GSVector4i(
					total_rect.x * m_scale,
					(z_address_info.offset + total_rect.y) * m_scale,
					(total_rect.z + (1.0f / m_scale)) * m_scale,
					(z_address_info.offset + total_rect.w + (1.0f / m_scale)) * m_scale);
				g_gs_device->StretchRectAuto(m_texture,
					GSVector4(total_rect.x / static_cast<float>(m_unscaled_size.x),
					          total_rect.y / static_cast<float>(m_unscaled_size.y),
					          (total_rect.z + (1.0f / m_scale)) / static_cast<float>(m_unscaled_size.x),
					          (total_rect.w + (1.0f / m_scale)) / static_cast<float>(m_unscaled_size.y)),
					g_texture_cache->GetTemporaryZ(), GSVector4(dRect), Nearest);
			}
		}
	}

	m_dirty.clear();
}

void GSTextureCache::Target::UpdateIfDirtyIntersects(const GSVector4i& rc)
{
	m_age = 0;

	for (auto& dirty : m_dirty)
	{
		const GSVector4i dirty_rc(dirty.GetDirtyRect(m_TEX0, false));
		if (dirty_rc.rintersect(rc).rempty())
			continue;

		GL_CACHE("TC: Update dirty rectangle [%d,%d,%d,%d] due to intersection with [%d,%d,%d,%d]",
			dirty_rc.x, dirty_rc.y, dirty_rc.z, dirty_rc.w, rc.x, rc.y, rc.z, rc.w);
		Update();
		break;
	}
}

void GSTextureCache::Target::UpdateValidChannels(u32 psm, u32 fbmsk)
{
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[psm];
	m_valid_alpha_low |= (psm_s.trbpp == 32 && (fbmsk & 0x0F000000) != 0x0F000000) || (psm_s.trbpp == 16);
	m_valid_alpha_high |= (psm_s.trbpp == 32 && (fbmsk & 0xF0000000) != 0xF0000000) || (psm_s.trbpp == 16);
	m_valid_rgb |= (psm_s.trbpp >= 24 && (fbmsk & 0x00FFFFFF) != 0x00FFFFFF) || (psm_s.trbpp == 16);
}

bool GSTextureCache::Target::HasValidBitsForFormat(u32 psm, bool req_color, bool req_alpha, bool width_match)
{
	bool alpha_valid = false;
	bool color_valid = false;

	switch (psm)
	{
		case PSMT4:
			return (m_valid_rgb && m_valid_alpha_low && m_valid_alpha_high);
		case PSMT8H:
			return m_valid_alpha_low || m_valid_alpha_high;
		case PSMT4HL:
			return m_valid_alpha_low;
		case PSMT4HH:
			return m_valid_alpha_high;
		case PSMT8:
		default:
			alpha_valid = m_valid_alpha_low || m_valid_alpha_high;
			color_valid = m_valid_rgb;

			if (req_alpha && !alpha_valid && color_valid && (m_TEX0.PSM & 0xF) <= PSMCT24 && (psm & 0xF) == PSMCT32)
			{
				RGBAMask mask;
				mask._u32 = 0x8;
				m_TEX0.PSM &= ~PSMCT24;

				if (!(m_dirty.GetDirtyChannels() & 0x8))
					AddDirtyRectTarget(this, m_valid, m_TEX0.PSM, m_TEX0.TBW, mask, false);

				alpha_valid = true;
			}
			break;
	}

	return (!req_color || color_valid) && (!req_alpha || alpha_valid);
}

void GSTextureCache::Target::ResizeDrawn(const GSVector4i& rect)
{
	m_drawn_since_read = m_drawn_since_read.rintersect(rect);
}


void GSTextureCache::Target::UpdateDrawn(const GSVector4i& rect, bool can_update_size)
{
	if (m_drawn_since_read.rempty())
	{
		m_drawn_since_read = rect.rintersect(m_valid);
	}
	else if (can_update_size)
	{
		m_drawn_since_read = m_drawn_since_read.runion(rect);
	}
}

void GSTextureCache::Target::ResizeValidity(const GSVector4i& rect)
{
	if (!m_valid.eq(GSVector4i::zero()))
	{
		m_valid = m_valid.rintersect(rect);
		m_drawn_since_read = m_drawn_since_read.rintersect(rect);
		m_end_block = GSLocalMemory::GetEndBlockAddress(m_TEX0.TBP0, m_TEX0.TBW, m_TEX0.PSM, m_valid);
	}

}

void GSTextureCache::Target::UpdateValidity(const GSVector4i& rect, bool can_resize)
{
	if (m_valid.eq(GSVector4i::zero()))
	{
		m_valid = rect;

		m_end_block = GSLocalMemory::GetEndBlockAddress(m_TEX0.TBP0, m_TEX0.TBW, m_TEX0.PSM, m_valid);
	}
	else if (can_resize)
	{
		m_valid = m_valid.runion(rect);

		m_end_block = GSLocalMemory::GetEndBlockAddress(m_TEX0.TBP0, m_TEX0.TBW, m_TEX0.PSM, m_valid);
	}
}

bool GSTextureCache::Target::PromoteToStereo()
{
	GSTexture* old_tex = m_texture;
	if (!old_tex || old_tex->GetArrayLayers() >= 2)
		return true;

	const bool depth = old_tex->IsDepthStencil();
	const GSVector2i size = old_tex->GetSize();
	GSTexture* tex = g_gs_device->FetchSurface(old_tex->GetUsage(), size.x, size.y, 1,
		old_tex->GetFormat(), false, PreferReusedLabelledTexture(), 2);
	if (!tex)
	{
		Console.Error("TC: Failed to allocate %dx%d stereo target for promotion.", size.x, size.y);
		return false;
	}

	static const bool s_vr_chainlog = (std::getenv("PCSX2_VR_CHAINLOG") != nullptr);
	if (s_vr_chainlog)
	{
		Console.WriteLn("(VR) CHAINLOG promote bp=0x%x state=%s depth=%d %dx%d",
			m_TEX0.TBP0,
			old_tex->GetState() == GSTexture::State::Dirty ? "dirty-copy" :
			old_tex->GetState() == GSTexture::State::Cleared ? "cleared-clear" : "invalidate",
			depth ? 1 : 0, size.x, size.y);
	}
	if (old_tex->GetState() == GSTexture::State::Dirty)
	{
		g_gs_device->CopyRect(old_tex, tex, GSVector4i::loadh(size), 0, 0);
	}
	else if (old_tex->GetState() == GSTexture::State::Cleared)
	{
		if (depth)
			g_gs_device->ClearDepth(tex, old_tex->GetClearDepth());
		else
			g_gs_device->ClearRenderTarget(tex, old_tex->GetClearColor());
	}
	else
	{
		g_gs_device->InvalidateRenderTarget(tex);
	}

	static const bool s_iss039_oldpath = (std::getenv("PCSX2_VR_ISS039_OLDPATH") != nullptr);
	if (s_iss039_oldpath) [[unlikely]]
	{
		g_texture_cache->m_in_stereo_promotion = true;
		g_texture_cache->InvalidateSourcesFromTarget(this);
		g_texture_cache->m_in_stereo_promotion = false;
	}
	else
	{
		g_texture_cache->RetargetSourcesAfterPromotion(this, old_tex, tex);
	}

	g_texture_cache->m_target_memory_usage =
		(g_texture_cache->m_target_memory_usage - old_tex->GetMemUsage()) + tex->GetMemUsage();
	g_gs_device->Recycle(old_tex);
	m_texture = tex;
	UpdateTextureDebugName();

	DevCon.WriteLn("(VR) TC: promoted %s 0x%x to a 2-layer stereo target (%dx%d).",
		depth ? "DS" : "RT", m_TEX0.TBP0, size.x, size.y);
	return true;
}

bool GSTextureCache::Target::ResizeTexture(int new_unscaled_width, int new_unscaled_height, bool recycle_old, bool require_new_rect, GSVector4i new_rect, bool keep_old)
{
	const GSVector2i size = m_texture->GetSize();
	const GSVector2i new_unscaled_size = GSVector2i(new_unscaled_width, new_unscaled_height);
	const GSVector2i new_size = ScaleRenderTargetSize(new_unscaled_size, m_scale);

	if (size.x == new_size.x && size.y == new_size.y && !require_new_rect)
		return true;

	const bool clear = (new_size.x > size.x || new_size.y > size.y);

	GSTexture* tex = g_gs_device->CreateCompatible(m_texture, new_size, clear, PreferReusedLabelledTexture());
	if (!tex)
	{
		Console.Error("(ResizeTexture) Failed to allocate %dx%d texture from %dx%d texture", size.x, size.y, new_size.x, new_size.y);
		return false;
	}

	if (m_texture->GetState() == GSTexture::State::Dirty)
	{
		const GSVector4i rc = require_new_rect ? new_rect : GSVector4i::loadh(size.min(new_size));
		if (tex->IsDepthLike())
		{
			const u32 copy_layers = std::min(m_texture->GetArrayLayers(), tex->GetArrayLayers());
			for (u32 l = 0; l < copy_layers; l++)
				g_gs_device->StretchRectAuto(m_texture->GetLayerProxyTexture(l), tex->GetLayerProxyTexture(l), GSVector4(rc), Biln);
		}
		else
		{
			if (require_new_rect)
			{
				const u32 copy_layers = std::min(m_texture->GetArrayLayers(), tex->GetArrayLayers());
				if (copy_layers > 1)
				{
					for (u32 l = 0; l < copy_layers; l++)
						g_gs_device->StretchRectAuto(m_texture->GetLayerProxyTexture(l), tex->GetLayerProxyTexture(l), GSVector4(rc), Nearest);
				}
				else
				{
					g_gs_device->StretchRectAuto(m_texture, tex, GSVector4(rc), Nearest);
				}
			}
			else
			{
				g_gs_device->CopyRect(m_texture, tex, rc, 0, 0);
			}
		}
	}
	else if (m_texture->GetState() == GSTexture::State::Cleared)
	{
		if (tex->IsDepthLike())
			g_gs_device->ClearDepth(tex, m_texture->GetClearDepth());
		else
			g_gs_device->ClearRenderTarget(tex, m_texture->GetClearColor());
	}
	else
	{
		g_gs_device->InvalidateRenderTarget(tex);
	}


	if (!keep_old)
	{
		g_texture_cache->m_target_memory_usage = (g_texture_cache->m_target_memory_usage - m_texture->GetMemUsage()) + tex->GetMemUsage();

		if (recycle_old)
			g_gs_device->Recycle(m_texture);
		else
			delete m_texture;
	}
	else
		g_texture_cache->m_target_memory_usage += tex->GetMemUsage();

	m_texture = tex;
	m_unscaled_size = new_unscaled_size;

	UpdateTextureDebugName();

	return true;
}

void GSTextureCache::Target::UpdateTextureDebugName()
{
#ifdef PCSX2_DEVBUILD
	if (GSConfig.UseDebugDevice)
	{
		m_texture->SetDebugName(SmallString::from_format("{} 0x{:X} {} BW={} {}x{}",
			m_type ? "DS" : "RT", static_cast<u32>(m_TEX0.TBP0), GSUtil::GetPSMName(m_TEX0.PSM), static_cast<u32>(m_TEX0.TBW),
			m_unscaled_size.x, m_unscaled_size.y));
	}
#endif
}

void GSTextureCache::SourceMap::Add(Source* s, const GIFRegTEX0& TEX0)
{
	m_surfaces.insert(s);

	s->m_pages.loopPages([this, s](u32 page) {
		s->m_erase_it[page] = m_map[page].InsertFront(s);
	});
}

void GSTextureCache::SourceMap::SwapTexture(GSTexture* old_tex, GSTexture* new_tex)
{
	for (auto s : m_surfaces)
	{
		if (s->m_texture == old_tex)
			s->m_texture = new_tex;
	}
}

void GSTextureCache::SourceMap::RemoveAll()
{
	for (auto s : m_surfaces)
	{
		if (s->m_from_hash_cache)
		{
			pxAssert(s->m_from_hash_cache->refcount > 0);
			if ((--s->m_from_hash_cache->refcount) == 0)
				s->m_from_hash_cache->age = 0;
		}

		delete s;
	}

	m_surfaces.clear();

	for (FastList<Source*>& item : m_map)
	{
		item.clear();
	}
}

void GSTextureCache::SourceMap::RemoveAt(Source* s)
{
	if (g_texture_cache->m_draw_inflight_source == s)
	{
		g_texture_cache->m_draw_inflight_source_kills++;
		if (g_texture_cache->m_in_stereo_promotion)
			g_texture_cache->m_draw_inflight_source_kills_promo++;

		static const bool s_srcguard = (std::getenv("PCSX2_VR_SRCGUARD") != nullptr);
		if (s_srcguard)
		{
			Console.Error("(VR) SRCGUARD kill #%u (%s): in-flight draw source bp=0x%x from_target=0x%x %s tex=%p layers=%u",
				g_texture_cache->m_draw_inflight_source_kills,
				g_texture_cache->m_in_stereo_promotion ? "stereo-promotion" : "other",
				s->m_TEX0.TBP0,
				s->m_from_target ? s->m_from_target->m_TEX0.TBP0 : 0u,
				s->m_shared_texture ? "DIRECT(shared)" : "COPY(owned)",
				static_cast<void*>(s->m_texture),
				s->m_texture ? s->m_texture->GetArrayLayers() : 0u);
		}
		pxAssertMsg(false, "TC: freeing the Source the current draw is holding (use-after-free window)");
	}

	m_surfaces.erase(s);

	GL_CACHE("TC: Remove Src Texture: 0x%x TBW %u PSM %s",
		s->m_TEX0.TBP0, s->m_TEX0.TBW, GSUtil::GetPSMName(s->m_TEX0.PSM));

	s->m_pages.loopPages([this, s](u32 page) {
		m_map[page].EraseIndex(s->m_erase_it[page]);
	});

	if (s->m_from_hash_cache)
	{
		pxAssert(s->m_from_hash_cache->refcount > 0);
		if ((--s->m_from_hash_cache->refcount) == 0)
			s->m_from_hash_cache->age = 0;
	}

	delete s;
}

void GSTextureCache::AttachPaletteToSource(Source* s, u16 pal, bool need_gs_texture, bool update_alpha_minmax)
{
	s->m_palette_obj = m_palette_map.LookupPalette(pal, need_gs_texture);
	s->m_palette = need_gs_texture ? s->m_palette_obj->GetPaletteGSTexture() : nullptr;
	if (update_alpha_minmax)
	{
		s->m_alpha_minmax = s->m_palette_obj->GetAlphaMinMax();
		s->m_valid_alpha_minmax = true;

		if (s->m_TEX0.PSM == PSMT8H && s->m_from_target && s->m_from_target->HasValidAlpha())
		{
			s->m_alpha_minmax = s->m_palette_obj->GetAlphaMinMax(static_cast<u8>(s->m_from_target->m_alpha_min), static_cast<u8>(s->m_from_target->m_alpha_max));
		}
	}
}

void GSTextureCache::AttachPaletteToSource(Source* s, GSTexture* gpu_clut)
{
	s->m_palette_obj = nullptr;
	s->m_palette = gpu_clut;

	s->m_valid_alpha_minmax = false;
	s->m_alpha_minmax.first = 0;
	s->m_alpha_minmax.second = 255;
}

GSTextureCache::SurfaceOffset GSTextureCache::ComputeSurfaceOffset(const GSOffset& off, const GSVector4i& r, const Target* t)
{
	if (!t)
		return {false};
	const SurfaceOffset so = ComputeSurfaceOffset(off.bp(), off.bw(), off.psm(), r, t);
	return so;
}

GSTextureCache::SurfaceOffset GSTextureCache::ComputeSurfaceOffset(const uint32_t bp, const uint32_t bw, const uint32_t psm, const GSVector4i& r, const Target* t)
{
	if (!t)
		return {false};
	SurfaceOffsetKey sok;
	sok.elems[0].bp = bp;
	sok.elems[0].bw = bw;
	sok.elems[0].psm = psm;
	sok.elems[0].rect = r;
	sok.elems[1].bp = t->m_TEX0.TBP0;
	sok.elems[1].bw = t->m_TEX0.TBW;
	sok.elems[1].psm = t->m_TEX0.PSM;
	sok.elems[1].rect = t->m_valid;
	const SurfaceOffset so = ComputeSurfaceOffset(sok);
	if (so.is_valid && !t->m_dirty.empty())
	{
		const SurfaceOffsetKeyElem& t_sok = sok.elems[1];
		const GSLocalMemory::psm_t& t_psm_s = GSLocalMemory::m_psm[t_sok.psm];
		const u32 so_bp = t_psm_s.info.bn(so.b2a_offset.x, so.b2a_offset.y, t_sok.bp, t_sok.bw);
		const u32 so_bp_end = t_psm_s.info.bn(so.b2a_offset.z - 1, so.b2a_offset.w - 1, t_sok.bp, t_sok.bw);
		for (const auto& dr : t->m_dirty)
		{
			const GSLocalMemory::psm_t& dr_psm_s = GSLocalMemory::m_psm[dr.psm];
			const u32 dr_bp = dr_psm_s.info.bn(dr.r.x, dr.r.y, t_sok.bp, dr.bw);
			const u32 dr_bp_end = dr_psm_s.info.bn(dr.r.z - 1, dr.r.w - 1, t_sok.bp, dr.bw);
			const bool overlap = GSTextureCache::CheckOverlap(dr_bp, dr_bp_end, so_bp, so_bp_end);
			if (overlap)
			{
				return {false};
			}
		}
	}
	return so;
}

GSTextureCache::SurfaceOffset GSTextureCache::ComputeSurfaceOffset(const SurfaceOffsetKey& sok)
{
	const SurfaceOffsetKeyElem& a_el = sok.elems[0];
	const SurfaceOffsetKeyElem& b_el = sok.elems[1];
	const GSLocalMemory::psm_t& a_psm_s = GSLocalMemory::m_psm[a_el.psm];
	const GSLocalMemory::psm_t& b_psm_s = GSLocalMemory::m_psm[b_el.psm];
	const GSVector4i a_rect = a_el.rect.ralign<Align_Outside>(a_psm_s.bs);
	const GSVector4i b_rect = b_el.rect.ralign<Align_Outside>(b_psm_s.bs);
	if (a_rect.width() <= 0 || a_rect.height() <= 0 || a_rect.x < 0 || a_rect.y < 0)
		return {false};
	if (b_rect.width() <= 0 || b_rect.height() <= 0 || b_rect.x < 0 || b_rect.y < 0)
		return {false};
	const u32 a_bp_end = a_psm_s.info.bn(a_rect.z - 1, a_rect.w - 1, a_el.bp, a_el.bw);
	const u32 b_bp_end = b_psm_s.info.bn(b_rect.z - 1, b_rect.w - 1, b_el.bp, b_el.bw);
	const bool overlap = GSTextureCache::CheckOverlap(a_el.bp, a_bp_end, b_el.bp, b_bp_end);
	if (!overlap)
		return {false};

	const auto it = m_surface_offset_cache.find(sok);
	if (it != m_surface_offset_cache.end())
		return it->second;

	SurfaceOffset so;
	so.is_valid = false;
	const int dx = b_psm_s.bs.x;
	const int dy = b_psm_s.bs.y;
	GSVector4i b2a_offset = GSVector4i::zero();
	if (a_el.bp >= b_el.bp)
	{
		const u32 b_bw = b_psm_s.trbpp > 8 ? std::max(1U, b_el.bw) : std::max(1U, b_el.bw / 2);
		const int y_page_offset = std::max(b_rect.y, static_cast<int>((((a_el.bp >= b_el.bp) >> 5) / b_bw) * b_psm_s.pgs.y));
		for (b2a_offset.y = y_page_offset; b2a_offset.y < b_rect.w; b2a_offset.y += dy)
		{
			for (b2a_offset.x = b_rect.x; b2a_offset.x < b_rect.z; b2a_offset.x += dx)
			{
				const u32 a_candidate_bp = b_psm_s.info.bn(b2a_offset.x, b2a_offset.y, b_el.bp, b_el.bw);
				if (a_el.bp == a_candidate_bp)
				{
					so.is_valid = true;
					break;
				}
			}
			if (so.is_valid)
				break;
		}
	}
	else
	{
		so.is_valid = true;
		b2a_offset.x = b_rect.x;
		b2a_offset.y = b_rect.y;
	}

	pxAssert(!so.is_valid || b2a_offset.x >= b_rect.x);
	pxAssert(!so.is_valid || b2a_offset.x < b_rect.z);
	pxAssert(!so.is_valid || b2a_offset.y >= b_rect.y);
	pxAssert(!so.is_valid || b2a_offset.y < b_rect.w);

	if (so.is_valid)
	{
		if (a_bp_end >= b_bp_end)
		{
			b2a_offset.z = b_rect.z;
			b2a_offset.w = b_rect.w;
		}
		else
		{
			so.is_valid = false;
			for (b2a_offset.w = b2a_offset.y; b2a_offset.w <= b_rect.w; b2a_offset.w += dy)
			{
				for (b2a_offset.z = b2a_offset.x; b2a_offset.z <= b_rect.z; b2a_offset.z += dx)
				{
					const u32 a_candidate_bp_end = b_psm_s.info.bn(b2a_offset.z - 1, b2a_offset.w - 1, b_el.bp, b_el.bw);
					if (a_bp_end == a_candidate_bp_end)
					{
						if (b2a_offset.z == b2a_offset.x)
							++b2a_offset.z;
						if (b2a_offset.w == b2a_offset.y)
							++b2a_offset.w;
						b2a_offset = b2a_offset.ralign<Align_Outside>(b_psm_s.bs);
						so.is_valid = true;
						break;
					}
				}
				if (so.is_valid)
					break;
			}
			if (!so.is_valid)
			{
				GL_CACHE("TC: ComputeSurfaceOffset - Could not find <z,w> offset.");
				b2a_offset.z = b_rect.z;
				b2a_offset.w = b_rect.w;
			}
		}
	}

	pxAssert(!so.is_valid || b2a_offset.z > b2a_offset.x);
	pxAssert(!so.is_valid || b2a_offset.z <= b_rect.z);
	pxAssert(!so.is_valid || b2a_offset.w > b_rect.y);
	pxAssert(!so.is_valid || b2a_offset.w <= b_rect.w);

	so.b2a_offset = b2a_offset;

	const GSVector4i& r1 = so.b2a_offset;
	const GSVector4i& r2 = b_rect;
	[[maybe_unused]] const GSVector4i ri = r1.rintersect(r2);
	pxAssert(!so.is_valid || (r1.eq(ri) && r1.x >= 0 && r1.y >= 0 && r1.z > 0 && r1.w > 0));

	if (m_surface_offset_cache.size() + 1 > S_SURFACE_OFFSET_CACHE_MAX_SIZE)
	{
		GL_PERF("TC: ComputeSurfaceOffset - Size of cache %d too big, clearing it.", m_surface_offset_cache.size());
		m_surface_offset_cache.clear();
	}
	m_surface_offset_cache.emplace(std::make_pair(sok, so));
	if (so.is_valid)
	{
		GL_CACHE("TC: ComputeSurfaceOffset - Cached HIT element (size %d), [B] BW %d, PSM %s, BP 0x%x (END 0x%x) + OFF <%d,%d => %d,%d> ---> [A] BP 0x%x (END: 0x%x).",
			m_surface_offset_cache.size(), b_el.bw, GSUtil::GetPSMName(b_el.psm), b_el.bp, b_bp_end,
			so.b2a_offset.x, so.b2a_offset.y, so.b2a_offset.z, so.b2a_offset.w,
			a_el.bp, a_bp_end);
	}
	else
	{
		GL_CACHE("TC: ComputeSurfaceOffset - Cached MISS element (size %d), [B] BW %d, PSM %s, BP 0x%x (END 0x%x) -/-> [A] BP 0x%x (END: 0x%x).",
			m_surface_offset_cache.size(), b_el.bw, GSUtil::GetPSMName(b_el.psm), b_el.bp, b_bp_end,
			a_el.bp, a_bp_end);
	}
	return so;
}

void GSTextureCache::InvalidateTemporarySource()
{
	if (!m_temporary_source)
		return;

	delete m_temporary_source;
	m_temporary_source = nullptr;
}

GSTextureCache::TempZAddress GSTextureCache::GetTemporaryZInfo()
{
	return m_temporary_z_info;
}

void GSTextureCache::SetTemporaryZInfo(u32 address, u32 offset, u32 rt_offset)
{
	m_temporary_z_info.ZBP = address;
	m_temporary_z_info.offset = offset;
	m_temporary_z_info.rt_offset = rt_offset;
	m_temporary_z_info.rect_since = GSVector4i::zero();
}
void GSTextureCache::SetTemporaryZInfo(TempZAddress address_info)
{
	m_temporary_z_info = address_info;
}

void GSTextureCache::SetTemporaryZ(GSTexture* temp_z)
{
	m_temporary_z = temp_z;
}

GSTexture* GSTextureCache::GetTemporaryZ()
{
	if (!m_temporary_z)
		return nullptr;

	return m_temporary_z;
}


void GSTextureCache::InvalidateTemporaryZ()
{
	if (!m_temporary_z)
		return;

	g_gs_device->Recycle(m_temporary_z);
	m_temporary_z = nullptr;
}

void GSTextureCache::InjectHashCacheTexture(const HashCacheKey& key, GSTexture* tex, const std::pair<u8, u8>& alpha_minmax)
{
	m_hash_cache_replacement_memory_usage += tex->GetMemUsage();

	auto it = m_hash_cache.find(key);
	if (it == m_hash_cache.end())
	{
		const HashCacheEntry entry{tex, 1u, 0u, alpha_minmax, true, true};
		m_hash_cache.emplace(key, entry);
		return;
	}

	it->second.age = 0;
	it->second.alpha_minmax = alpha_minmax;
	it->second.valid_alpha_minmax = true;

	if (!it->second.is_replacement)
		m_hash_cache_memory_usage -= it->second.texture->GetMemUsage();
	else
		m_hash_cache_replacement_memory_usage -= it->second.texture->GetMemUsage();

	it->second.is_replacement = true;
	m_src.SwapTexture(it->second.texture, tex);
	g_gs_device->Recycle(it->second.texture);
	it->second.texture = tex;
}

GSTextureCache::Palette::Palette(const u32* clut, u16 pal, bool need_gs_texture)
	: m_tex_palette(nullptr)
	, m_pal(pal)
{
	const u16 palette_size = pal * sizeof(u32);
	m_clut = (u32*)_aligned_malloc(palette_size, 64);
	memcpy(m_clut, clut, palette_size);
	if (need_gs_texture)
	{
		InitializeTexture();
	}

	m_alpha_minmax = GSGetRGBA8AlphaMinMax(m_clut, pal, 1, 0);
}

GSTextureCache::Palette::~Palette()
{
	if (m_tex_palette)
	{
		g_texture_cache->m_source_memory_usage -= m_tex_palette->GetMemUsage();
		g_gs_device->Recycle(m_tex_palette);
	}

	_aligned_free(m_clut);
}

std::pair<u8, u8> GSTextureCache::Palette::GetAlphaMinMax(u8 min_index, u8 max_index) const
{
	pxAssert(min_index <= max_index);
	return GSGetRGBA8AlphaMinMax(m_clut + min_index, max_index - min_index + 1, 1, 0);
}

GSTexture* GSTextureCache::Palette::GetPaletteGSTexture()
{
	return m_tex_palette;
}

GSTextureCache::PaletteKey GSTextureCache::Palette::GetPaletteKey()
{
	return {m_clut, m_pal};
}

void GSTextureCache::Palette::InitializeTexture()
{
	if (!m_tex_palette)
	{
		m_tex_palette = g_gs_device->CreateTexture(m_pal, 1, 1, GSTexture::Format::Color);
		if (!m_tex_palette) [[unlikely]]
		{
			Console.Error("Failed to allocate %ux1 texture for palette", m_pal);
			return;
		}

		m_tex_palette->Update(GSVector4i(0, 0, m_pal, 1), m_clut, m_pal * sizeof(m_clut[0]));

		g_texture_cache->m_source_memory_usage += m_tex_palette->GetMemUsage();
	}
}

u64 GSTextureCache::PaletteKeyHash::operator()(const PaletteKey& key) const
{
	pxAssert(key.pal == 16 || key.pal == 256);
	return key.pal == 16 ?
	           GSXXH3_64bits(key.clut, sizeof(key.clut[0]) * 16) :
	           GSXXH3_64bits(key.clut, sizeof(key.clut[0]) * 256);
};

bool GSTextureCache::PaletteKeyEqual::operator()(const PaletteKey& lhs, const PaletteKey& rhs) const
{
	if (lhs.pal != rhs.pal)
	{
		return false;
	}

	return GSVector4i::compare64(lhs.clut, rhs.clut, lhs.pal * sizeof(lhs.clut[0]));
};

GSTextureCache::PaletteMap::PaletteMap()
{
	for (auto& map : m_maps)
	{
		map.reserve(MAX_CACHED_PALETES);
	}
}

std::shared_ptr<GSTextureCache::Palette> GSTextureCache::PaletteMap::LookupPalette(u16 pal, bool need_gs_texture)
{
	return LookupPalette(g_gs_renderer->m_mem.m_clut, pal, need_gs_texture);
}

std::shared_ptr<GSTextureCache::Palette> GSTextureCache::PaletteMap::LookupPalette(const u32* clut, u16 pal, bool need_gs_texture)
{
	pxAssert(pal == 16 || pal == 256);

	auto& map = m_maps[pal == 16 ? 0 : 1];

	const PaletteKey palette_key = {clut, pal};

	const auto& it1 = map.find(palette_key);

	if (it1 != map.end())
	{
		if (need_gs_texture && !it1->second->GetPaletteGSTexture())
		{
			it1->second->InitializeTexture();
		}
		return it1->second;
	}

	if (map.size() > MAX_CACHED_PALETES)
	{
		GL_INS("TC: WARNING, %u-bit PaletteMap (Size %u): Max size %u exceeded, clearing unused palettes.", pal * sizeof(u32), map.size(), MAX_CACHED_PALETES);

		const u32 current_size = map.size();

		for (auto it = map.begin(); it != map.end();)
		{
			if (it->second.use_count() <= 1)
			{
				it = map.erase(it);
			}
			else
			{
				++it;
			}
		}

		const u32 cleared_palette_count = current_size - static_cast<u32>(map.size());

		if (cleared_palette_count == 0)
		{
			GL_INS("TC: ERROR, %u-bit PaletteMap (Size %u): Max size %u exceeded, could not clear any palette, negative performance impact.", pal * sizeof(u32), map.size(), MAX_CACHED_PALETES);
		}
		else
		{
			map.reserve(MAX_CACHED_PALETES);
			GL_INS("TC: INFO, %u-bit PaletteMap (Size %u): Cleared %u palettes.", pal * sizeof(u32), map.size(), cleared_palette_count);
		}
	}

	std::shared_ptr<Palette> palette = std::make_shared<Palette>(clut, pal, need_gs_texture);

	map.emplace(palette->GetPaletteKey(), palette);

	GL_CACHE("TC: , %u-bit PaletteMap (Size %u): Added new palette.", pal * sizeof(u32), map.size());

	return palette;
}

void GSTextureCache::PaletteMap::Clear()
{
	for (auto& map : m_maps)
	{
		map.clear();
		map.reserve(MAX_CACHED_PALETES);
	}
}

std::size_t GSTextureCache::SurfaceOffsetKeyHash::operator()(const GSTextureCache::SurfaceOffsetKey& key) const
{
	std::hash<u32> hash_fn_u32;
	std::hash<int> hash_fn_int;
	std::hash<size_t> hash_fn_szt;
	size_t hash = 0x9e3779b9;
	for (const SurfaceOffsetKeyElem& elem : key.elems)
	{
		hash = hash ^ hash_fn_u32(elem.bp) << 1;
		hash = hash ^ hash_fn_u32(elem.bw) << 1;
		hash = hash ^ hash_fn_u32(elem.psm) << 1;
		hash = hash ^ hash_fn_int(elem.rect.x) << 1;
		hash = hash ^ hash_fn_int(elem.rect.y) << 1;
		hash = hash ^ hash_fn_int(elem.rect.z) << 1;
		hash = hash ^ hash_fn_int(elem.rect.w) << 1;
	}
	return hash_fn_szt(hash);
}

bool GSTextureCache::SurfaceOffsetKeyEqual::operator()(const GSTextureCache::SurfaceOffsetKey& lhs, const GSTextureCache::SurfaceOffsetKey& rhs) const
{
	for (size_t i = 0; i < lhs.elems.size(); ++i)
	{
		const SurfaceOffsetKeyElem& lhs_elem = lhs.elems[i];
		const SurfaceOffsetKeyElem& rhs_elem = rhs.elems[i];
		if (lhs_elem.bp != rhs_elem.bp || lhs_elem.bw != rhs_elem.bw || lhs_elem.psm != rhs_elem.psm || !lhs_elem.rect.eq(rhs_elem.rect))
			return false;
	}
	return true;
}

bool GSTextureCache::SourceRegion::IsFixedTEX0(GIFRegTEX0 TEX0) const
{
	return IsFixedTEX0(1 << TEX0.TW, 1 << TEX0.TH);
}

bool GSTextureCache::SourceRegion::IsFixedTEX0(int tw, int th) const
{
	return IsFixedTEX0W(tw) || IsFixedTEX0H(th);
}

bool GSTextureCache::SourceRegion::IsFixedTEX0W(int tw) const
{
	return (GetMaxX() > tw);
}

bool GSTextureCache::SourceRegion::IsFixedTEX0H(int th) const
{
	return (GetMaxY() > th);
}

GSVector2i GSTextureCache::SourceRegion::GetSize(int tw, int th) const
{
	return GSVector2i(HasX() ? GetWidth() : tw, HasY() ? GetHeight() : th);
}

GSVector4i GSTextureCache::SourceRegion::GetRect(int tw, int th) const
{
	return GSVector4i(HasX() ? GetMinX() : 0, HasY() ? GetMinY() : 0, HasX() ? GetMaxX() : tw, HasY() ? GetMaxY() : th);
}

GSVector4i GSTextureCache::SourceRegion::GetOffset(int tw, int th) const
{
	const int xoffs = (GetMaxX() > tw) ? GetMinX() : 0;
	const int yoffs = (GetMaxY() > th) ? GetMinY() : 0;
	return GSVector4i(xoffs, yoffs, xoffs, yoffs);
}

GSTextureCache::SourceRegion GSTextureCache::SourceRegion::AdjustForMipmap(u32 level) const
{
	SourceRegion ret = {};
	if (HasX())
	{
		const s32 new_minx = GetMinX() >> level;
		const s32 new_maxx = new_minx + std::max(GetWidth() >> level, 1);
		ret.SetX(new_minx, new_maxx);
	}
	if (HasY())
	{
		const s32 new_miny = GetMinY() >> level;
		const s32 new_maxy = new_miny + std::max(GetHeight() >> level, 1);
		ret.SetY(new_miny, new_maxy);
	}
	return ret;
}

void GSTextureCache::SourceRegion::AdjustTEX0(GIFRegTEX0* TEX0) const
{
	const GSOffset offset(GSLocalMemory::m_psm[TEX0->PSM].info, TEX0->TBP0, TEX0->TBW, TEX0->PSM);
	TEX0->TBP0 += offset.bn(GetMinX(), GetMinY());
}

GSTextureCache::SourceRegion GSTextureCache::SourceRegion::Create(GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP)
{
	SourceRegion region = {};

	if (CLAMP.WMS == CLAMP_REGION_CLAMP && CLAMP.MAXU >= CLAMP.MINU)
	{
		const u32 rw = CLAMP.MAXU - CLAMP.MINU + 1;
		if (rw < (1u << TEX0.TW) || CLAMP.MAXU >= (1u << TEX0.TW))
		{
			region.SetX(CLAMP.MINU, CLAMP.MAXU + 1);
			GL_CACHE("TC: Region clamp optimization: %d width -> %d", 1 << TEX0.TW, region.GetWidth());
		}
	}
	else if (CLAMP.WMS == CLAMP_REGION_REPEAT && CLAMP.MINU != 0)
	{
		const u32 rw = ((CLAMP.MINU | CLAMP.MAXU) - CLAMP.MAXU) + 1;
		if (rw < (1u << TEX0.TW) || (CLAMP.MAXU != 0 && (rw <= (1u << TEX0.TW))))
		{
			region.SetX(CLAMP.MAXU, (CLAMP.MINU | CLAMP.MAXU) + 1);
			GL_CACHE("TC: Region repeat optimization: %d width -> %d", 1 << TEX0.TW, region.GetWidth());
		}
	}

	if (CLAMP.WMT == CLAMP_REGION_CLAMP && CLAMP.MAXV >= CLAMP.MINV)
	{
		const u32 rh = CLAMP.MAXV - CLAMP.MINV + 1;
		if (rh < (1u << TEX0.TH) || CLAMP.MAXV >= (1u << TEX0.TH))
		{
			region.SetY(CLAMP.MINV, CLAMP.MAXV + 1);
			GL_CACHE("TC: Region clamp optimization: %d height -> %d", 1 << TEX0.TW, region.GetHeight());
		}
	}
	else if (CLAMP.WMT == CLAMP_REGION_REPEAT && CLAMP.MINV != 0)
	{
		const u32 rh = ((CLAMP.MINV | CLAMP.MAXV) - CLAMP.MAXV) + 1;
		if (rh < (1u << TEX0.TH) || (CLAMP.MAXV != 0 && (rh <= (1u << TEX0.TH))))
		{
			region.SetY(CLAMP.MAXV, (CLAMP.MINV | CLAMP.MAXV) + 1);
			GL_CACHE("TC: Region repeat optimization: %d height -> %d", 1 << TEX0.TW, region.GetHeight());
		}
	}

	return region;
}

using BlockHashState = XXH3_state_t;

__fi static void BlockHashReset(BlockHashState& st)
{
	XXH3_64bits_reset(&st);
}

__fi static void BlockHashAccumulate(BlockHashState& st, const u8* bp)
{
	GSXXH3_64bits_update(&st, bp, GS_BLOCK_SIZE);
}

__fi static void BlockHashAccumulate(BlockHashState& st, const u8* bp, u32 size)
{
	GSXXH3_64bits_update(&st, bp, size);
}

__fi static GSTextureCache::HashType FinishBlockHash(BlockHashState& st)
{
	return GSXXH3_64bits_digest(&st);
}

static void HashTextureLevel(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, BlockHashState& hash_st, u8* temp)
{
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	const GSVector2i& bs = psm.bs;
	const int tw = region.HasX() ? region.GetWidth() : (1 << TEX0.TW);
	const int th = region.HasY() ? region.GetHeight() : (1 << TEX0.TH);

	const GSVector4i rect(region.GetRect(tw, th));
	const GSVector4i block_rect(rect.ralign<Align_Outside>(bs));
	GSLocalMemory& mem = g_gs_renderer->m_mem;
	const GSOffset off = mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM);

	if (tw < bs.x || th < bs.y || psm.fmsk != 0xFFFFFFFFu || region.GetMaxX() > 0 || region.GetMinY() > 0)
	{
		const bool palette = (psm.pal > 0);
		const u32 pitch = VectorAlign(static_cast<u32>(block_rect.z) << (palette ? 0 : 2));
		const u32 row_size = static_cast<u32>(tw) << (palette ? 0 : 2);
		const GSLocalMemory::readTexture rtx = palette ? psm.rtxP : psm.rtx;

		rtx(mem, off, block_rect, temp, pitch, TEXA);

		u8* ptr = temp + (pitch * static_cast<u32>(rect.top - block_rect.top)) +
		          static_cast<u32>(rect.left - block_rect.left);
		if (pitch == row_size)
		{
			BlockHashAccumulate(hash_st, ptr, pitch * static_cast<u32>(th));
		}
		else
		{
			for (int y = 0; y < th; y++, ptr += pitch)
				BlockHashAccumulate(hash_st, ptr, row_size);
		}
	}
	else
	{
		GSOffset::BNHelper bn = off.bnMulti(block_rect.left, block_rect.top);
		const int right = block_rect.right >> off.blockShiftX();
		const int bottom = block_rect.bottom >> off.blockShiftY();
		const int xAdd = (1 << off.blockShiftX()) * (psm.bpp / 8);

		for (; bn.blkY() < bottom; bn.nextBlockY())
		{
			for (int x = 0; bn.blkX() < right; bn.nextBlockX(), x += xAdd)
			{
				BlockHashAccumulate(hash_st, mem.BlockPtr(bn.value()));
			}
		}
	}
}

GSTextureCache::HashType GSTextureCache::HashTexture(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, SourceRegion region)
{
	BlockHashState hash_st;
	BlockHashReset(hash_st);
	HashTextureLevel(TEX0, TEXA, region, hash_st, s_unswizzle_buffer);
	return FinishBlockHash(hash_st);
}

void GSTextureCache::PreloadTexture(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, SourceRegion region, GSLocalMemory& mem,
	bool paltex, GSTexture* tex, u32 level, std::pair<u8, u8>* alpha_minmax)
{
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	const GSVector2i& bs = psm.bs;
	const int tw = region.HasX() ? region.GetWidth() : (1 << TEX0.TW);
	const int th = region.HasY() ? region.GetHeight() : (1 << TEX0.TH);

	const GSVector4i rect(region.GetRect(tw, th));
	const GSVector4i block_rect(rect.ralign<Align_Outside>(bs));
	const GSOffset off(mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM));
	const int read_width = block_rect.width();
	u32 pitch = static_cast<u32>(read_width) * sizeof(u32);
	GSLocalMemory::readTexture rtx = psm.rtx;
	if (paltex)
	{
		pitch >>= 2;
		rtx = psm.rtxP;
	}

	const GSVector4i unoffset_rect(0, 0, tw, th);
	GSTexture::GSMap map;
	if (rect.eq(block_rect) && !alpha_minmax && tex->Map(map, &unoffset_rect, level))
	{
		rtx(mem, off, block_rect, map.bits, map.pitch, TEXA);
		tex->Unmap();

		if (alpha_minmax)
			*alpha_minmax = std::make_pair<u8, u8>(0, 255);
	}
	else
	{
		pitch = VectorAlign(pitch);

		u8* buff = s_unswizzle_buffer;
		rtx(mem, off, block_rect, buff, pitch, TEXA);

		const u8* ptr = buff + (pitch * static_cast<u32>(rect.top - block_rect.top)) +
		                (static_cast<u32>(rect.left - block_rect.left) << (paltex ? 0 : 2));

		if (alpha_minmax)
			*alpha_minmax = GSGetRGBA8AlphaMinMax(ptr, unoffset_rect.width(), unoffset_rect.height(), pitch);

		tex->Update(unoffset_rect, ptr, pitch, level);
	}
}

GSTextureCache::HashCacheKey::HashCacheKey()
	: TEX0Hash(0)
	, CLUTHash(0)
	, region_width(0)
	, region_height(0)
{
	TEX0.U64 = 0;
	TEXA.U64 = 0;
}

GSTextureCache::HashCacheKey GSTextureCache::HashCacheKey::Create(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, const u32* clut, const GSVector2i* lod, SourceRegion region)
{
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];

	HashCacheKey ret;
	ret.TEX0.U64 = TEX0.U64 & 0x00000003FFF00000ULL;
	ret.TEXA.U64 = (psm.pal == 0 && psm.fmt > 0) ? (TEXA.U64 & 0x000000FF000080FFULL) : 0;
	ret.CLUTHash = clut ? GSTextureCache::PaletteKeyHash{}({clut, psm.pal}) : 0;
	ret.region_width = static_cast<u16>(region.GetWidth());
	ret.region_height = static_cast<u16>(region.GetHeight());

	BlockHashState hash_st;
	BlockHashReset(hash_st);

	HashTextureLevel(TEX0, TEXA, region, hash_st, s_unswizzle_buffer);

	if (lod)
	{
		const int basemip = lod->x;
		const int nmips = lod->y - lod->x + 1;
		for (int i = 1; i < nmips; i++)
		{
			const GIFRegTEX0 MIP_TEX0{g_gs_renderer->GetTex0Layer(basemip + i)};
			HashTextureLevel(MIP_TEX0, TEXA, region.AdjustForMipmap(i), hash_st, s_unswizzle_buffer);
		}
	}

	ret.TEX0Hash = FinishBlockHash(hash_st);

	return ret;
}

GSTextureCache::HashCacheKey GSTextureCache::HashCacheKey::WithRemovedCLUTHash() const
{
	HashCacheKey ret{*this};
	ret.CLUTHash = 0;
	return ret;
}

void GSTextureCache::HashCacheKey::RemoveCLUTHash()
{
	CLUTHash = 0;
}

u64 GSTextureCache::HashCacheKeyHash::operator()(const HashCacheKey& key) const
{
	std::size_t h = 0;
	HashCombine(h, key.TEX0Hash, key.CLUTHash, key.TEX0.U64, key.TEXA.U64,
		static_cast<u64>(key.region_width) | (static_cast<u64>(key.region_height) << 16));
	return h;
}
