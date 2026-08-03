// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/HW/GSHwHack.h"
#include "GS/GSGL.h"
#include "GS/GSUtil.h"

#include <cmath>

static bool s_nativeres;

#define RPRIM r.PRIM
#define RCONTEXT r.m_context

#define RTEX0 r.m_cached_ctx.TEX0
#define RTEST r.m_cached_ctx.TEST
#define RFRAME r.m_cached_ctx.FRAME
#define RZBUF r.m_cached_ctx.ZBUF
#define RCLAMP r.m_cached_ctx.CLAMP

#define RTME r.PRIM->TME
#define RTBP0 r.m_cached_ctx.TEX0.TBP0
#define RTBW r.m_cached_ctx.TEX0.TBW
#define RTPSM r.m_cached_ctx.TEX0.PSM
#define RFBP r.m_cached_ctx.FRAME.Block()
#define RFBW r.m_cached_ctx.FRAME.FBW
#define RFPSM r.m_cached_ctx.FRAME.PSM
#define RFBMSK r.m_cached_ctx.FRAME.FBMSK
#define RZBP r.m_cached_ctx.ZBUF.Block()
#define RZPSM r.m_cached_ctx.ZBUF.PSM
#define RZMSK r.m_cached_ctx.ZBUF.ZMSK
#define RZTST r.m_cached_ctx.TEST.ZTST


bool GSHwHack::GSC_IRem(GSRendererHW& r, int& skip)
{
	static bool first_shuffle = false;

	if (skip > 0)
	{
		if (skip == 1 && first_shuffle)
		{
			first_shuffle = false;

			GIFRegTEX0 RTLookup = GIFRegTEX0::Create(RTBP0, RFBW, RFPSM);
			GSTextureCache::Source* src = g_texture_cache->LookupSource(true, RTLookup, r.m_cached_ctx.TEXA, r.m_cached_ctx.CLAMP, GSVector4i(0, 0, 1, 1), nullptr, true, false, r.m_cached_ctx.FRAME, true, true);

			GSTextureCache::Target* rt = g_texture_cache->LookupDrawTarget(GIFRegTEX0::Create(RTBP0, RFBW, RFPSM),
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget, true, 0, false, true, true, GSVector4i(0, 0, 1, 1), true, false, true, src);

			if (!rt)
				return false;

			GSLocalMemory::psm_t rt_psm = GSLocalMemory::m_psm[RFPSM];
			int page_offset = (RTBP0 - rt->m_TEX0.TBP0) >> 5;
			int vertical_offset = page_offset / std::max(rt->m_TEX0.TBW, 1U) * rt_psm.pgs.y;
			int horizontal_offset = page_offset % std::max(rt->m_TEX0.TBW, 1U) * rt_psm.pgs.x;

			GSVector4i draw_size = GSVector4i(0, 0, 64, 32) + GSVector4i(horizontal_offset, vertical_offset, horizontal_offset, vertical_offset);

			GSHWDrawConfig& config = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), rt->GetTexture(), rt->GetScale(), draw_size);
			config.ps.shuffle = 1;
			config.ps.dst_fmt = GSLocalMemory::PSM_FMT_32;
			config.ps.write_rg = 0;
			config.ps.shuffle_same = 0;
			config.ps.real16src = 0;
			config.ps.shuffle_across = 1;
			config.ps.process_rg = r.SHUFFLE_READWRITE;
			config.ps.process_ba = r.SHUFFLE_READWRITE;
			config.colormask.wrgba = 0;
			config.colormask.wr = 1;
			config.colormask.wb = 1;
			config.ps.rta_correction = 0;
			config.ps.rta_source_correction = 0;
			config.ps.tfx = TFX_DECAL;
			config.ps.tcc = true;
			r.EndHLEHardwareDraw(false);

			rt = nullptr;
			src = nullptr;
		}
		else
		{
			skip--;
			return !first_shuffle;
		}
	}

	if (skip == 0)
	{
		const int get_next_ctx = r.m_env.PRIM.CTXT;
		const GSDrawingContext& next_ctx = r.m_env.CTXT[get_next_ctx];

		r.m_env.SCANMSK.MSK = 0;
		r.m_prev_env.SCANMSK.MSK = 0;

		if (RTME && RTPSM == PSMT8 && (RTBP0 + 0x20) == next_ctx.TEX0.TBP0 && RFBP == next_ctx.FRAME.Block())
		{
			skip = 2;
			return false;
		}
		if (RTME && RFBP != RTBP0 && RFPSM == PSMCT16S && RTPSM == PSMCT16S)
		{
			if (r.m_vt.m_max.p.x == 64 && r.m_vt.m_max.p.y == 64 && r.m_index->tail == 128)
			{
				const GSVector4i draw_size(r.m_vt.m_min.p.x, r.m_vt.m_min.p.y/2, r.m_vt.m_max.p.x, r.m_vt.m_max.p.y/2);
				const GSVector4i read_size(r.m_vt.m_min.t.x, r.m_vt.m_min.t.y/2, r.m_vt.m_max.t.x, r.m_vt.m_max.t.y/2);
				r.m_cached_ctx.TEX0.PSM = PSMCT32;
				r.m_cached_ctx.FRAME.PSM = PSMCT32;
				r.ReplaceVerticesWithSprite(draw_size, read_size, GSVector2i(read_size.width(), read_size.height()), draw_size);
			}
		}

		if (RTBP0 == (RFBP - 0x20) && r.m_vt.m_max.p.x == 64 && r.m_vt.m_max.p.y == 34 && r.m_index->tail == 2)
		{
			GSVector4i draw_size(r.m_vt.m_min.p.x, r.m_vt.m_min.p.y - 2.0f, r.m_vt.m_max.p.x, r.m_vt.m_max.p.y - 2.0f);
			GSVector4i read_size(r.m_vt.m_min.t.x, r.m_vt.m_min.t.y, r.m_vt.m_max.t.x, r.m_vt.m_max.t.y);
			r.ReplaceVerticesWithSprite(draw_size, read_size, GSVector2i(read_size.width(), read_size.height()), draw_size);


			{
				GIFRegTEX0 RTLookup = GIFRegTEX0::Create(RTBP0, RFBW, RFPSM);
				GSTextureCache::Source* src = g_texture_cache->LookupSource(true, RTLookup, r.m_cached_ctx.TEXA, r.m_cached_ctx.CLAMP, GSVector4i(0,0,1,1), nullptr, true, false, r.m_cached_ctx.FRAME, true, true);

				GSTextureCache::Target* rt = g_texture_cache->LookupDrawTarget(GIFRegTEX0::Create(RTBP0, RFBW, RFPSM),
					GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget, true, 0, false, true, true, GSVector4i(0,0,1,1), true, false, true, src);

				if (!rt)
					return false;

				GSLocalMemory::psm_t rt_psm = GSLocalMemory::m_psm[RFPSM];
				int page_offset = (RTBP0 - rt->m_TEX0.TBP0) >> 5;
				int vertical_offset = page_offset / std::max(rt->m_TEX0.TBW, 1U) * rt_psm.pgs.y;
				int horizontal_offset = page_offset % std::max(rt->m_TEX0.TBW, 1U) * rt_psm.pgs.x;

				draw_size = draw_size + GSVector4i(horizontal_offset, vertical_offset, horizontal_offset, vertical_offset);

				GSHWDrawConfig& config = r.BeginHLEHardwareDraw(
					rt->GetTexture(), nullptr, rt->GetScale(), rt->GetTexture(), rt->GetScale(), draw_size);
				config.ps.shuffle = 1;
				config.ps.dst_fmt = GSLocalMemory::PSM_FMT_32;
				config.ps.write_rg = 0;
				config.ps.shuffle_same = 0;
				config.ps.real16src = 0;
				config.ps.shuffle_across = 1;
				config.ps.process_rg = r.SHUFFLE_READWRITE;
				config.ps.process_ba = r.SHUFFLE_READWRITE;
				config.colormask.wrgba = 0;
				config.colormask.wr = 1;
				config.colormask.wb = 1;
				config.ps.rta_correction = 0;
				config.ps.rta_source_correction = 0;
				config.ps.tfx = TFX_DECAL;
				config.ps.tcc = true;
				r.EndHLEHardwareDraw(false);

				rt = nullptr;
				src = nullptr;
				first_shuffle = true;
			}
		}
	}

	return true;
}

bool GSHwHack::GSC_Manhunt2(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && RFBP == 0x03c20 && RFPSM == PSMCT32 && RTBP0 == 0x01400 && RTPSM == PSMT8)
		{
			skip = 640;
		}
	}

	return true;
}

bool GSHwHack::GSC_SacredBlaze(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if ((RFBP == 0x2680 || RFBP == 0x26c0 || RFBP == 0x2780 || RFBP == 0x2880 || RFBP == 0x2a80) && RTPSM == PSMCT32 && RFBW <= 2 &&
			(!RTME || (RTBP0 == 0x0 || RTBP0 == 0xe00 || RTBP0 == 0x3e00)))
		{
			r.SwPrimRender(r, RTBP0 > 0x1000, false);
			skip = 1;
		}
	}

	return true;
}

bool GSHwHack::GSC_GuitarHero(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTBW <= 4 && RTME && RFBW <= 4 && (r.m_context->TEX1.MMIN & 1) == 0)
		{
			r.ClearGSLocalMemory(r.m_context->offset.zb, r.m_r, 0);
			r.SwPrimRender(r, RFBP != 0x2DC0, false);
			skip = 1;
		}
	}

	return true;
}

bool GSHwHack::GSC_SFEX3(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && RFBP == 0x00500 && RFPSM == PSMCT16 && RTBP0 == 0x00f00 && RTPSM == PSMCT16)
		{

			r.m_vertex->buff[1].XYZ.Y += r.m_vertex->buff[r.m_vertex->tail - 1].XYZ.Y - r.m_context->XYOFFSET.OFY;
			r.m_vertex->buff[1].V = r.m_vertex->buff[r.m_vertex->tail - 1].V;
			r.m_vertex->tail = 2;
			r.m_index->tail = 2;
		}
	}

	return true;
}

bool GSHwHack::GSC_DTGames(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && RFPSM == PSMCT32 && RTBP0 == RFBP && RTPSM == PSMCT16 && RTEST.ATE && RTEST.ATST == ATST_NEVER && RTEST.AFAIL == AFAIL_FB_ONLY && RFBMSK == 0xFFFFFF)
		{
			GSTextureCache::Target* rt = g_texture_cache->LookupDrawTarget(GIFRegTEX0::Create(RTBP0, RFBW, RFPSM),
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget);

			if (!rt)
				return false;

			GSHWDrawConfig& clear = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), nullptr, rt->GetScale(), rt->GetUnscaledRect());
			clear.colormask.wrgba = 0;
			clear.colormask.wa = 1;
			r.EndHLEHardwareDraw(false);

			GSHWDrawConfig& config = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), rt->GetTexture(), rt->GetScale(), rt->GetUnscaledRect());
			config.ps.shuffle = 1;
			config.ps.dst_fmt = GSLocalMemory::PSM_FMT_32;
			config.ps.write_rg = 0;
			config.ps.shuffle_same = 0;
			config.ps.real16src = 0;
			config.ps.shuffle_across = 1;
			config.ps.process_rg = r.SHUFFLE_READ;
			config.ps.process_ba = r.SHUFFLE_WRITE;
			config.colormask.wrgba = 0;
			config.colormask.wa = 1;
			config.ps.rta_correction = 1;
			config.ps.tfx = TFX_DECAL;
			config.ps.tcc = true;
			r.EndHLEHardwareDraw(true);

			rt->m_alpha_min = 0;
			rt->m_alpha_max = 255;
			skip = 69;
		}
	}
	else
	{
		if (RTPSM != PSMCT16)
			skip = 0;
	}

	return true;
}

bool GSHwHack::GSC_NamcoGames(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (!s_nativeres && r.PRIM->PRIM == GS_SPRITE && RTME && RTEX0.TFX == 1 && RFPSM == RTPSM && RTPSM == PSMCT32 && RFBMSK == 0xFF000000 && r.m_index->tail > 2)
		{
			GSVertex* v = &r.m_vertex->buff[0];
			if (v[0].XYZ.X & 0xF)
			{
				const GSVector4i draw_size(r.m_vt.m_min.p.x, r.m_vt.m_min.p.y, r.m_vt.m_max.p.x + 1.0f, r.m_vt.m_max.p.y + 1.0f);
				const GSVector4i read_size(r.m_vt.m_min.t.x, r.m_vt.m_min.t.y, r.m_vt.m_max.t.x + 0.5f, r.m_vt.m_max.t.y + 0.5f);
				r.ReplaceVerticesWithSprite(draw_size, read_size, GSVector2i(read_size.width(), read_size.height()), draw_size);
			}
			else
			{
				for (u32 i = 0; i < r.m_index->tail; i+=2)
				{
					v[i].XYZ.Y -= 0x8;
				}
			}
		}
	}

	return true;
}

bool GSHwHack::GSC_SandGrainGames(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		const int get_next_ctx = r.m_env.PRIM.CTXT;
		const GSDrawingContext& next_ctx = r.m_env.CTXT[get_next_ctx];

		if (r.PRIM->PRIM == GS_SPRITE && RTME && RFPSM == PSMCT16S && RTPSM == PSMZ16S && next_ctx.TEX0.TBP0 == RFBP && next_ctx.TEX0.PSM == PSMT8H)
		{
			GSTextureCache::Target* texsrc = g_texture_cache->LookupDrawTarget(GIFRegTEX0::Create(RTBP0, RTBW, RTPSM),
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::DepthStencil);

			if (!texsrc)
				return false;

			GSTextureCache::Target* rt = g_texture_cache->LookupDrawTarget(GIFRegTEX0::Create(next_ctx.FRAME.Block(), next_ctx.FRAME.FBW, next_ctx.FRAME.PSM),
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget);

			if (!rt)
				return false;

			r.m_mem.m_clut.Read32(next_ctx.TEX0, r.m_env.TEXA);
			std::shared_ptr<GSTextureCache::Palette> palette =
				g_texture_cache->LookupPaletteObject(r.m_mem.m_clut, GSLocalMemory::m_psm[next_ctx.TEX0.PSM].pal, true);

			if (!palette)
				return false;

			GSHWDrawConfig& config = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), texsrc->GetTexture(), texsrc->GetScale(), texsrc->GetUnscaledRect());
			config.ps.channel = ChannelFetch_GXBY;
			config.cb_ps.ChannelShuffle = GSVector4i(0, 0, 0xFF, 0);
			config.ps.depth_fmt = 2;
			config.colormask.wrgba = 8;
			config.ps.tfx = TFX_DECAL;
			config.ps.tcc = true;
			r.EndHLEHardwareDraw(true);

			GSHWDrawConfig& modulate_config = r.BeginHLEHardwareDraw(
				rt->GetTexture(), nullptr, rt->GetScale(), rt->GetTexture(), rt->GetScale(), rt->GetUnscaledRect());

			modulate_config.pal = palette->GetPaletteGSTexture();
			modulate_config.ps.aem_fmt = 0;
			modulate_config.ps.aem = 0;
			modulate_config.ps.pal_fmt = 3;
			modulate_config.colormask.wrgba = 8;
			modulate_config.ps.tfx = TFX_DECAL;
			modulate_config.ps.tcc = true;
			r.EndHLEHardwareDraw(true);

			rt->m_alpha_min = 0;
			rt->m_alpha_max = 128;
			rt->m_rt_alpha_scale = false;
			rt->ScaleRTAlpha();

			const int pages = (rt->m_valid.w / 32) * rt->m_TEX0.TBW;
			skip = pages;
		}
	}

	return true;
}

bool GSHwHack::GSC_BurnoutGames(GSRendererHW& r, int& skip)
{

	static u32 state = 0;
	static GIFRegTEX0 main_fb;
	static GSVector2i main_fb_size;
	static GIFRegTEX0 downsample_fb;
	static GIFRegTEX0 bloom_fb;
	switch (state)
	{
		case 0:
		{
			if (RFBW != 2 || RFBP != RZBP || RTME)
				break;

			if (r.m_backed_up_ctx < 0)
				break;

			GSTextureCache::Target* tgt = g_texture_cache->LookupDrawTarget(r.m_env.CTXT[r.m_backed_up_ctx].TEX0,
				GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget);
			if (!tgt)
				break;

			main_fb = tgt->m_TEX0;
			main_fb_size = tgt->GetUnscaledSize();
			r.m_cached_ctx.FRAME.FBW = tgt->m_TEX0.TBW;
			r.m_cached_ctx.ZBUF.ZMSK = true;
			r.ReplaceVerticesWithSprite(GSVector4i::loadh(main_fb_size), main_fb_size);
			bloom_fb = GIFRegTEX0::Create(RFBP, RFBW, RFPSM);
			state = 1;
			GL_INS("GSC_BurnoutGames(): Initial double-striped clear.");
			return true;
		}

		case 1:
		{
			r.ReplaceVerticesWithSprite(GSVector4i::loadh(main_fb_size), main_fb_size);
			r.m_cached_ctx.ZBUF.ZMSK = true;
			state = 2;
			GL_INS("GSC_BurnoutGames(): Extract Bright Pixels.");
			return true;
		}

		case 2:
		{
			const GSVector4i downsample_rect = GSVector4i(0, 0, ((main_fb_size.x / 2)), ((main_fb_size.y / 2)));
			const GSVector4i uv_rect = GSVector4i(0, 0, main_fb_size.x, main_fb_size.y);
			r.ReplaceVerticesWithSprite(downsample_rect, uv_rect, main_fb_size, downsample_rect);
			downsample_fb = GIFRegTEX0::Create(RFBP, RFBW, RFPSM);
			state = 3;
			GL_INS("GSC_BurnoutGames(): Downsampling.");
			RTBW = RFBW * 2;
			return true;
		}

		case 3:
		{
			g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, bloom_fb.TBP0);
			state = 4;
			[[fallthrough]];
		}

		case 4:
		{
			if (!RTME || RTBP0 != downsample_fb.TBP0)
			{
				GL_INS("GSC_BurnoutGames(): Skipping extra pass.");
				skip = 1;
				return true;
			}

			GL_INS("GSC_BurnoutGames(): Bloom effect done.");
			skip = 0;
			state = 0;
			return true;
		}
	}

	return GSC_BlackAndBurnoutSky(r, skip);
}

bool GSHwHack::GSC_BlackAndBurnoutSky(GSRendererHW& r, int& skip)
{
	if (skip != 0)
		return true;

	const GIFRegTEX0& TEX0 = RTEX0;
	const GIFRegFRAME& FRAME = RFRAME;
	const GIFRegALPHA& ALPHA = RCONTEXT->ALPHA;

	if (RPRIM->PRIM == GS_SPRITE && !RPRIM->IIP && RPRIM->TME && !RPRIM->FGE && RPRIM->ABE && !RPRIM->AA1 && !RPRIM->FST && !RPRIM->FIX &&
		ALPHA.A == ALPHA.B && ALPHA.D == 0 && FRAME.PSM == PSMCT32 && TEX0.CPSM == PSMCT32 && TEX0.TCC && !TEX0.TFX && !TEX0.CSM)
	{
		if (TEX0.TBW == 16 && TEX0.TW == 10 && TEX0.PSM == PSMT8 && TEX0.TH >= 7 && FRAME.FBW == 16)
		{
			GL_INS("OO_BurnoutGames - Readback clouds renderered from TEX0.TBP0 = 0x%04x (TEX0.CBP = 0x%04x) to FBP = 0x%04x", TEX0.TBP0, TEX0.CBP, FRAME.Block());
			r.SwPrimRender(r, true, false);
			skip = 1;
		}
		if (TEX0.TBW == 2 && TEX0.TW == 7 && ((TEX0.PSM == PSMT4 && FRAME.FBW == 3) || (TEX0.PSM == PSMT8 && FRAME.FBW == 2)) && TEX0.TH == 6 && (FRAME.FBMSK & 0xFFFFFF) == 0xFFFFFF)
		{
			GL_INS("OO_BurnoutGames - Render glass smash from TEX0.TBP0 = 0x%04x (TEX0.CBP = 0x%04x) to FBP = 0x%04x", TEX0.TBP0, TEX0.CBP, FRAME.Block());
			r.SwPrimRender(r, true, false);
			skip = 1;
		}
	}
	return true;
}

bool GSHwHack::GSC_MidnightClub3(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && (RFBP > 0x01d00 && RFBP <= 0x02a00) && RFPSM == PSMCT32 && (RFBP >= 0x01600 && RFBP < 0x03260) && RTPSM == PSMT8H)
		{
			skip = 1;
		}
	}

	return true;
}

bool GSHwHack::GSC_TalesOfLegendia(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && (RFBP == 0x3f80 || RFBP == 0x03fa0) && RFPSM == PSMCT32 && RTPSM == PSMT8)
		{
			skip = 3;
		}
		if (RTME && RFBP == 0x3800 && RFPSM == PSMCT32 && RTPSM == PSMZ32)
		{
			skip = 2;
		}
		if (RTME && RFBP && RFPSM == PSMCT32 && RTBP0 == 0x3d80)
		{
			skip = 1;
		}
		if (RTME && RFBP == 0x1c00 && (RTBP0 == 0x2e80 || RTBP0 == 0x2d80) && RTPSM == 0 && RFBMSK == 0xff000000)
		{
			skip = 1;
		}
		if (!RTME && RFBP == 0x2a00 && (RTBP0 == 0x1C00) && RTPSM == 0 && RFBMSK == 0x00FFFFFF)
		{
			skip = 1;
		}
	}

	return true;
}

bool GSHwHack::GSC_UltramanFightingEvolution(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (!s_nativeres && RTME && RFBP == 0x2a00 && RFPSM == PSMZ24 && RTBP0 == 0x1c00 && RTPSM == PSMZ24)
		{
			skip = 5;
		}
	}

	return true;
}

bool GSHwHack::GSC_TalesofSymphonia(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && RFPSM == PSMCT32 && (RTBP0 == 0x2bc0 || RTBP0 <= 0x0200) && (RFBMSK == 0xFF000000 || RFBMSK == 0x00FFFFFF))
		{
			skip = 1;
		}
		if (RTME && (RTBP0 == 0x1180 || RTBP0 == 0x1a40 || RTBP0 == 0x2300) && RFBMSK >= 0xFF000000)
		{
			skip = 1;
		}
	}

	return true;
}

bool GSHwHack::GSC_UrbanReign(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RTME && RTBP0 != RFBP && RFPSM == PSMCT32 && RTPSM == PSMCT32 &&
			RFRAME.FBW == (RTEX0.TBW / 2) && RCLAMP.WMS == CLAMP_REGION_CLAMP &&
			RCLAMP.WMT == CLAMP_REGION_CLAMP && ((r.m_vt.m_max.t == GSVector4(64.0f, 448.0f)).mask() == 0x3))
		{
			GL_CACHE("GSC_UrbanReign: Fix region clamp to 64 wide");
			RCLAMP.MAXU = 63;
		}
	}

	return true;
}

bool GSHwHack::GSC_NFSUndercover(GSRendererHW& r, int& skip)
{
	const GIFRegTEX0& Texture = RTEX0;
	const GIFRegFRAME& Frame = RFRAME;

	if (RPRIM->TME && Frame.PSM == PSMCT16S && Frame.FBMSK != 0 && Frame.FBW == 10 && Texture.TBW == 1 && Texture.TBP0 == 0x02800 && Texture.PSM == PSMZ16S)
	{
		skip = 79;
		return false;
	}
	else
	{
		return skip > 0;
	}
}

bool GSHwHack::GSC_PolyphonyDigitalGames(GSRendererHW& r, int& skip)
{

	static bool shuffle_hle_active = false;
	static u32 shuffle_fbmsk = 0;

	const bool is_cs = r.IsPossibleChannelShuffle();
	if (shuffle_hle_active && is_cs)
	{
		if (RFBMSK == shuffle_fbmsk)
		{
			skip = 1;
			return true;
		}
	}
	else if (!is_cs)
	{
		shuffle_hle_active = false;
		return false;
	}

	GSTextureCache::Target* src = g_texture_cache->LookupDrawTarget(RTEX0, GSVector2i(1, 1), r.GetTextureScaleFactor(),
		GSTextureCache::RenderTarget, true, 0, false, true, true, GSVector4i::zero(), true);
	if (!src)
		return false;

	r.m_mem.m_clut.Read32(RTEX0, r.m_draw_env->TEXA);
	std::shared_ptr<GSTextureCache::Palette> palette =
		g_texture_cache->LookupPaletteObject(r.m_mem.m_clut, GSLocalMemory::m_psm[RTEX0.PSM].pal, true);
	if (!palette)
		return false;

	shuffle_hle_active = true;
	shuffle_fbmsk = RFBMSK;
	skip = 1;

	const u32 fbmsk = RFBMSK;
	if (RFBMSK != 0x00FFFFFFu)
	{
		GL_PUSH("GSC_PolyphonyDigitalGames(): HLE Gran Turismo RGB channel shuffle");
		GSHWDrawConfig& config = r.BeginHLEHardwareDraw(
			src->GetTexture(), nullptr, src->GetScale(), src->GetTexture(), src->GetScale(), src->GetUnscaledRect());
		config.pal = palette->GetPaletteGSTexture();
		config.ps.channel = ChannelFetch_RGB;
		config.colormask.wrgba = 1 | 2 | 4;
		r.EndHLEHardwareDraw(false);
		src->m_last_draw = r.s_n;
		return true;
	}
	else
	{

		const GSVector2i resolution = r.PCRTCDisplays.GetResolution();
		const GSVector2i size = GSVector2i(resolution.x, resolution.y / 2);
		const u32 page_offset = ((size.y + 31) / 32) * src->m_TEX0.TBW * GS_BLOCKS_PER_PAGE;
		constexpr u32 base = 0;

		GL_PUSH("GSC_PolyphonyDigitalGames(): HLE Gran Turismo A channel shuffle");
		GL_INS("Src: %x %s TBW %u, Dst: %x, %x, %x", src->m_TEX0.TBP0, GSUtil::GetPSMName(src->m_TEX0.PSM), src->m_TEX0.TBW,
			base, base + page_offset, base + page_offset * 2);
		GL_INS("Rect: %d,%d => %d,%d", src->m_drawn_since_read.x, src->m_drawn_since_read.y,
			src->m_drawn_since_read.z, src->m_drawn_since_read.w);

		for (u32 channel = 0; channel < 3; channel++)
		{
			const GIFRegTEX0 TEX0 = GIFRegTEX0::Create(base + channel * page_offset, 10, PSMCT32);
			GSTextureCache::Target* dst = g_texture_cache->LookupDrawTarget(TEX0, src->GetUnscaledSize(), src->GetScale(), GSTextureCache::RenderTarget, true, fbmsk);
			if (!dst)
			{
				dst = g_texture_cache->CreateTarget(TEX0, size, size, src->GetScale(), GSTextureCache::RenderTarget, true, fbmsk);
				if (!dst)
					continue;
			}

			dst->m_TEX0.PSM = PSMCT32;
			dst->m_rt_alpha_scale = false;
			dst->m_alpha_min = 0;
			dst->m_alpha_max = 255;
			dst->m_alpha_range = true;
			dst->UpdateValidChannels(PSMCT32, fbmsk);
			dst->UpdateValidity(GSVector4i::loadh(size));

			GSHWDrawConfig& config = r.BeginHLEHardwareDraw(
				dst->GetTexture(), nullptr, dst->GetScale(), src->GetTexture(), src->GetScale(), src->GetUnscaledRect());
			config.pal = palette->GetPaletteGSTexture();
			config.ps.tfx = TFX_DECAL;
			config.ps.tcc = true;
			config.ps.channel = ChannelFetch_RED + channel;
			config.colormask.wrgba = 8;
			r.EndHLEHardwareDraw(false);
			dst->m_last_draw = r.s_n;
		}

		return true;
	}
}


bool GSHwHack::GSC_Battlefield2(GSRendererHW& r, int& skip)
{
	if (skip == 0)
	{
		if (RZBP >= RFBP && RFBP >= 0x2000 && RZBP >= 0x2700 && ((RZBP - RFBP) == 0x700))
		{
			skip = 7;

			GIFRegTEX0 TEX0 = {};
			TEX0.TBP0 = RFBP;
			TEX0.TBW = 8;
			GSTextureCache::Target* dst = g_texture_cache->LookupDrawTarget(TEX0, r.GetTargetSize(), r.GetTextureScaleFactor(), GSTextureCache::DepthStencil);

			if (!dst)
				dst = g_texture_cache->CreateTarget(TEX0, r.GetTargetSize(), r.GetValidSize(nullptr), r.GetTextureScaleFactor(), GSTextureCache::DepthStencil,
					true, 0, false, false, false, GSVector4i(0,0,1,1), nullptr);

			if (dst)
			{
				float dc = r.m_vertex->buff[1].XYZ.Z;
				g_gs_device->ClearDepth(dst->m_texture, dc * std::exp2(-32.0f));
			}
		}
	}

	return true;
}

bool GSHwHack::GSC_BlueTongueGames(GSRendererHW& r, int& skip)
{
	GSDrawingContext* context = r.m_context;

	if (RPRIM->TME && RTEX0.TW == 3 && RTEX0.TH == 3 && RTEX0.PSM == 0 && RFRAME.FBMSK == 0x00FFFFFF && RFRAME.FBW == 8 && r.PCRTCDisplays.GetResolution().x > 512)
	{
		for (u32 i = 1; i < r.m_vertex->tail; i+=2)
		{
			int value = (((r.m_vertex->buff[i].XYZ.X - r.m_vertex->buff[i - 1].XYZ.X) + 8) >> 4);
			if (value != 32)
				return false;
		}

		r.m_r.x = r.m_vt.m_min.p.x;
		r.m_r.y = r.m_vt.m_min.p.y;
		r.m_r.z = r.PCRTCDisplays.GetResolution().x;
		r.m_r.w = r.PCRTCDisplays.GetResolution().y;

		for (int vert = 32; vert < 40; vert+=2)
		{
			r.m_vertex->buff[vert].XYZ.X = context->XYOFFSET.OFX + (((vert * 16) << 4) - 8);
			r.m_vertex->buff[vert].XYZ.Y = context->XYOFFSET.OFY;
			r.m_vertex->buff[vert].U = (vert * 16) << 4;
			r.m_vertex->buff[vert].V = 0;
			r.m_vertex->buff[vert+1].XYZ.X = context->XYOFFSET.OFX + ((((vert * 16) + 32) << 4) - 8);
			r.m_vertex->buff[vert+1].XYZ.Y = context->XYOFFSET.OFY + (r.PCRTCDisplays.GetResolution().y << 4) + 8;
			r.m_vertex->buff[vert+1].U = ((vert * 16) + 32) << 4;
			r.m_vertex->buff[vert+1].V = r.PCRTCDisplays.GetResolution().y << 4;
		}

		r.m_vt.m_max.p.x = r.m_r.z;
		r.m_vt.m_max.p.y = r.m_r.w;
		r.m_vt.m_max.t.x = r.m_r.z;
		r.m_vt.m_max.t.y = r.m_r.w;
		context->scissor.in.z = r.m_r.z;
		context->scissor.in.w = r.m_r.w;

		RFRAME.FBW = 10;
	}

	if ((context->FRAME.PSM == PSMCT16S || context->FRAME.PSM <= PSMCT24) && context->FRAME.FBW <= 5)
	{
		r.SwPrimRender(r, true, false);
		skip = 1;
		return true;
	}

	if (context->FRAME.FBW == 8 && r.m_index->tail == 32 && r.PRIM->TME && context->TEX0.TBW == 1)
	{
		r.SwPrimRender(r, false, false);
		return false;
	}

	return false;
}

bool GSHwHack::GSC_MetalGearSolid3(GSRendererHW& r, int& skip)
{

	if (RFPSM != PSMZ24 || RTPSM != PSMZ24 || !RTME)
		return false;

	if (!RZMSK)
	{
		u32 fm = 0, zm = 0;
		if (!r.m_cached_ctx.TEST.ATE || !r.TryAlphaTest(fm, zm) || zm == 0)
			return false;
	}

	const int w_sub = (RFBW / 2) * 64;
	const u32 w_sub_fp = w_sub << 4;
	r.m_cached_ctx.FRAME.FBP += RFBW / 2;

	GL_INS("OI_MetalGearSolid3(): %x -> %x, %dx%d, subtract %d", RFBP, RFBP + (RFBW / 2), r.m_r.width(), r.m_r.height(),
		w_sub);

	for (u32 i = 0; i < r.m_vertex->next; i++)
		r.m_vertex->buff[i].XYZ.X -= w_sub_fp;

	r.m_r -= GSVector4i(w_sub);
	return true;
}

bool GSHwHack::GSC_Turok(GSRendererHW& r, int& skip)
{

	if (r.m_index->tail == 6 && RPRIM->PRIM == 4 && !RTME && RFBMSK == 0x00FFFFFF && floor(r.m_vt.m_max.p.x) == 512 && r.m_env.CTXT[r.m_backed_up_ctx].FRAME.FBW == 10 && RFRAME.FBW == 8 && RFPSM == PSMCT32 && RTEST.ATE && RTEST.ATST == ATST_GEQUAL)
	{
		int num_pages = r.m_cached_ctx.FRAME.FBW * ((floor(r.m_vt.m_max.p.y) + 31) / 32);
		r.m_cached_ctx.FRAME.FBW = 10;
		num_pages = ((num_pages + 9) / 10) * 10;

		r.ReplaceVerticesWithSprite(
			r.GetDrawRectForPages(r.m_cached_ctx.FRAME.FBW, r.m_cached_ctx.FRAME.PSM, num_pages),
			GSVector2i(1, 1));
	}

	return true;
}

bool GSHwHack::OI_PointListPalette(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	const u32 n_vertices = r.m_vertex->next;
	const int w = r.m_r.width();
	const int h = r.m_r.height();
	const bool is_copy = !r.PRIM->ABE || (
		r.m_context->ALPHA.A == r.m_context->ALPHA.B
		&& r.m_context->ALPHA.D == 0
	);
	if (r.m_vt.m_primclass == GS_POINT_CLASS && w <= 64
		&& h <= 64
		&& n_vertices <= 256
		&& is_copy
		&& !r.PRIM->TME
		&& r.m_context->FRAME.PSM == PSMCT32
		&& !r.PRIM->FGE
		&& !r.PRIM->AA1
		&& !r.PRIM->FIX
		&& !r.m_draw_env->DTHE.DTHE
		&& !r.m_cached_ctx.TEST.ATE
		&& !r.m_cached_ctx.TEST.DATE
		&& (!r.m_cached_ctx.DepthRead() && !r.m_cached_ctx.DepthWrite())
		&& !RTEX0.CSM
		&& !r.m_draw_env->PABE.PABE
		&& r.m_context->FBA.FBA == 0
		&& r.m_cached_ctx.FRAME.FBMSK == 0
	)
	{
		const int mask = (r.m_vt.m_max.p.xyxy() == r.m_vt.m_min.p.xyxy()).mask();
		if (mask == 0xf)
			return true;

		const u32 FBP = r.m_cached_ctx.FRAME.Block();
		const u32 FBW = r.m_cached_ctx.FRAME.FBW;
		GL_INS("PointListPalette - m_r = <%d, %d => %d, %d>, n_vertices = %u, FBP = 0x%x, FBW = %u", r.m_r.x, r.m_r.y, r.m_r.z, r.m_r.w, n_vertices, FBP, FBW);
		const GSVertex* RESTRICT v = r.m_vertex->buff;
		const int ox(r.m_context->XYOFFSET.OFX);
		const int oy(r.m_context->XYOFFSET.OFY);
		for (size_t i = 0; i < n_vertices; ++i)
		{
			const GSVertex& vi = v[i];
			const GIFRegXYZ& xyz = vi.XYZ;
			const int x = (int(xyz.X) - ox) / 16;
			const int y = (int(xyz.Y) - oy) / 16;
			if (x < r.m_r.x || x > r.m_r.z)
				continue;
			if (y < r.m_r.y || y > r.m_r.w)
				continue;
			const u32 c = vi.RGBAQ.U32[0];
			r.m_mem.WritePixel32(x, y, c, FBP, FBW);
		}
		g_texture_cache->InvalidateVideoMem(r.m_context->offset.fb, r.m_r);
		return false;
	}
	return true;
}

bool GSHwHack::OI_DBZBTGames(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	if (t && t->m_from_target)
		return true;

	if (!((r.m_r == GSVector4i(0, 0, 16, 16)).alltrue() || (r.m_r == GSVector4i(0, 0, 64, 64)).alltrue()))
		return true;

	if (!r.CanUseSwSpriteRender())
		return true;

	r.SwSpriteRender();

	return false;
}

bool GSHwHack::OI_RozenMaidenGebetGarden(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	if (!RPRIM->TME)
	{
		const u32 FBP = RFRAME.Block();
		const u32 ZBP = RZBUF.Block();

		if (FBP == 0x008c0 && ZBP == 0x01a40)
		{

			GIFRegTEX0 TEX0 = {};

			TEX0.TBP0 = ZBP;
			TEX0.TBW = RFRAME.FBW;
			TEX0.PSM = RFRAME.PSM;

			if (GSTextureCache::Target* tmp_rt = g_texture_cache->LookupDrawTarget(TEX0, r.GetTargetSize(), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget))
			{
				GL_INS("OI_RozenMaidenGebetGarden FB clear");
				g_gs_device->ClearRenderTarget(tmp_rt->m_texture, 0);
				tmp_rt->UpdateDrawn(tmp_rt->m_valid);
				tmp_rt->m_alpha_max = 0;
				tmp_rt->m_alpha_min = 0;
				tmp_rt->m_alpha_range = false;
			}

			return false;
		}
		else if (FBP == 0x00000 && RZBUF.Block() == 0x01180)
		{

			GIFRegTEX0 TEX0 = {};

			TEX0.TBP0 = FBP;
			TEX0.TBW = RFRAME.FBW;
			TEX0.PSM = RZBUF.PSM;

			if (GSTextureCache::Target* tmp_ds = g_texture_cache->LookupDrawTarget(TEX0, r.GetTargetSize(), r.GetTextureScaleFactor(), GSTextureCache::DepthStencil))
			{
				GL_INS("OI_RozenMaidenGebetGarden ZB clear");
				g_gs_device->ClearDepth(tmp_ds->m_texture, 0.0f);
			}

			return false;
		}
	}

	return true;
}

bool GSHwHack::OI_SonicUnleashed(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	const GIFRegTEX0& Texture = RTEX0;

	GIFRegTEX0 Frame = {};
	Frame.TBW = RFRAME.FBW;
	Frame.TBP0 = RFRAME.Block();
	Frame.PSM = RFRAME.PSM;

	if ((!rt) || (!RPRIM->TME) || (GSLocalMemory::m_psm[Texture.PSM].bpp != 16) || (GSLocalMemory::m_psm[Frame.PSM].bpp != 16) || (Texture.TBP0 == Frame.TBP0) || (Frame.TBW != 16 && Texture.TBW != 16))
		return true;

	GL_INS("OI_SonicUnleashed replace draw by a copy draw %lld", r.s_n);

	GSTextureCache::Target* src = g_texture_cache->LookupDrawTarget(Texture, GSVector2i(1, 1), r.GetTextureScaleFactor(), GSTextureCache::RenderTarget, true, 0, false, true, true, GSVector4i::zero(), true);

	if (!src)
		return true;

	const GSVector2i src_size(src->m_texture->GetSize());

	GSTextureCache::Target* rt_again = g_texture_cache->LookupDrawTarget(Frame, src_size, src->m_scale, GSTextureCache::RenderTarget);
	if ((rt_again->m_TEX0.PSM & 0x3) == PSMCT16)
	{
		GSVector4 dRect;

		GSVector4 source_rect = GSVector4(static_cast<float>(rt_again->m_valid.x) / static_cast<float>(rt_again->m_unscaled_size.x), static_cast<float>(rt_again->m_valid.y) / static_cast<float>(rt_again->m_unscaled_size.y),
			static_cast<float>(rt_again->m_valid.z) / static_cast<float>(rt_again->m_unscaled_size.x), static_cast<float>(rt_again->m_valid.w) / static_cast<float>(rt_again->m_unscaled_size.y));

		dRect = GSVector4(rt_again->m_valid) * rt_again->m_scale;
		dRect.y /= 2;
		dRect.w /= 2;
		rt_again->m_valid.y /= 2;
		rt_again->m_valid.w /= 2;
		rt_again->m_TEX0.PSM = PSMCT32;
		GSTexture* tex = g_gs_device->CreateCompatible(rt_again->m_texture,
			static_cast<int>(rt_again->m_unscaled_size.x * rt_again->m_scale),
			static_cast<int>(rt_again->m_unscaled_size.y * rt_again->m_scale), false);

		if (!tex)
			return false;

		g_gs_device->StretchRectAuto(rt_again->m_texture, source_rect, tex, dRect, Nearest);

		g_gs_device->Recycle(rt_again->m_texture);
		rt_again->m_texture = tex;
		rt = tex;
	}

	GSVector2i rt_size(rt->GetSize());

	if (rt_size.x < src_size.x || rt_size.y < src_size.y)
	{
		if (rt_again->m_unscaled_size.x < src->m_unscaled_size.x || rt_again->m_unscaled_size.y < src->m_unscaled_size.y)
		{
			GSVector2i new_size = GSVector2i(std::max(rt_again->m_unscaled_size.x, src->m_unscaled_size.x),
				std::max(rt_again->m_unscaled_size.y, src->m_unscaled_size.y));
			rt_again->ResizeTexture(new_size.x, new_size.y);
			rt = rt_again->m_texture;
			rt_size = new_size * GSVector2i(src->GetScale());
			rt_again->UpdateDrawn(GSVector4i::loadh(new_size));
		}
	}


	const GSVector2i copy_size(std::min(rt_size.x, src_size.x), std::min(rt_size.y, src_size.y));

	const GSVector4 sRect(0.0f, 0.0f, static_cast<float>(copy_size.x) / static_cast<float>(src_size.x), static_cast<float>(copy_size.y) / static_cast<float>(src_size.y));
	const GSVector4 dRect(0, 0, copy_size.x, copy_size.y);

	g_gs_device->StretchRectAutoMask(src->m_texture, sRect, rt, dRect, true, true, true, false);

	return false;
}


bool GSHwHack::OI_ArTonelico2(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{

	const GSVertex* v = &r.m_vertex->buff[0];

	if (ds && r.m_vertex->next == 2 && !RPRIM->TME && RFRAME.FBW == 10 && v->XYZ.Z == 0 && RTEST.ZTST == ZTST_ALWAYS)
	{
		GL_INS("OI_ArTonelico2");
		g_gs_device->ClearDepth(ds, 0.0f);
	}

	return true;
}

bool GSHwHack::OI_BurnoutGames(GSRendererHW& r, GSTexture* rt, GSTexture* ds, GSTextureCache::Source* t)
{
	if (!OI_PointListPalette(r, rt, ds, t))
		return false;

	if (t && t->m_from_target)
		return true;

	if (!r.CanUseSwSpriteRender())
		return true;

	if (!r.PRIM->TME)
		return true;
	r.SwSpriteRender();

	return false;
}

#undef RPRIM
#undef RCONTEXT

#undef RTEX0
#undef RTEST
#undef RFRAME
#undef RZBUF
#undef RCLAMP

#undef RTME
#undef RTBP0
#undef RTBW
#undef RTPSM
#undef RFBP
#undef RFBW
#undef RFPSM
#undef RFBMSK
#undef RZBP
#undef RZPSM
#undef RZMSK
#undef RZTST

#define RBITBLTBUF r.m_env.BITBLTBUF
#define RSBP r.m_env.BITBLTBUF.SBP
#define RSBW r.m_env.BITBLTBUF.SBW
#define RSPSM r.m_env.BITBLTBUF.SPSM
#define RDBP r.m_env.BITBLTBUF.DBP
#define RDBW r.m_env.BITBLTBUF.DBW
#define RDPSM r.m_env.BITBLTBUF.DPSM
#define RWIDTH r.m_env.TRXREG.RRW
#define RHEIGHT r.m_env.TRXREG.RRH
#define RSX r.m_env.TRXPOS.SSAX
#define RSY r.m_env.TRXPOS.SSAY
#define RDX r.m_env.TRXPOS.DSAX
#define RDY r.m_env.TRXPOS.DSAY

static bool GetMoveTargetPair(GSRendererHW& r, GSTextureCache::Target** src, GIFRegTEX0 src_desc,
	GSTextureCache::Target** dst, GIFRegTEX0 dst_desc, bool req_target, bool preserve_target)
{
	const int src_type =
		GSLocalMemory::m_psm[src_desc.PSM].depth ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget;
	GSTextureCache::Target* tsrc =
		g_texture_cache->LookupDrawTarget(src_desc, GSVector2i(1, 1), r.GetTextureScaleFactor(), src_type);
	if (!tsrc)
		return false;

	const int dst_type =
		GSLocalMemory::m_psm[dst_desc.PSM].depth ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget;
	GSTextureCache::Target* tdst = g_texture_cache->LookupDrawTarget(dst_desc, tsrc->GetUnscaledSize(), tsrc->GetScale(),
		dst_type, true, 0, false, preserve_target, preserve_target, tsrc->GetUnscaledRect());
	if (!tdst)
	{
		if (req_target)
			return false;

		tdst = g_texture_cache->CreateTarget(dst_desc, tsrc->GetUnscaledSize(), tsrc->GetUnscaledSize(), tsrc->GetScale(), dst_type, true, 0,
			false, false, true, tsrc->GetUnscaledRect());
		if (!tdst)
			return false;
	}

	if (!preserve_target)
	{
		g_texture_cache->InvalidateVideoMemType(
			(dst_type == GSTextureCache::RenderTarget) ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget,
			dst_desc.TBP0);

		GL_INS("GetMoveTargetPair(): Clearing dirty list.");
		tdst->m_dirty.clear();
	}
	else
	{
		tdst->Update();
	}

	*src = tsrc;
	*dst = tdst;

	tdst->UpdateDrawn(tdst->m_valid);

	return true;
}

static bool GetMoveTargetPair(GSRendererHW& r, GSTextureCache::Target** src, GSTextureCache::Target** dst,
	bool req_target = false, bool preserve_target = false)
{
	return GetMoveTargetPair(r, src, GIFRegTEX0::Create(RSBP, RSBW, RSPSM), dst, GIFRegTEX0::Create(RDBP, RDBW, RDPSM),
		req_target, preserve_target);
}

static u64 s_last_hacked_move_n = 0;

bool GSHwHack::MV_Growlanser(GSRendererHW& r)
{

	if (RWIDTH != 32 || RHEIGHT != 16 || RSPSM != PSMCT32 || RDPSM != PSMCT32)
		return false;

	if (r.s_n == s_last_hacked_move_n)
		return true;

	GSTextureCache::Target *src, *dst;
	if (!GetMoveTargetPair(
			r, &src, GIFRegTEX0::Create(RSBP, RSBW, RSPSM), &dst, GIFRegTEX0::Create(RDBP, RDBW, PSMZ32), false, false))
	{
		return false;
	}

	const GSVector4i rc = src->GetUnscaledRect().rintersect(dst->GetUnscaledRect());
	dst->m_TEX0.TBW = src->m_TEX0.TBW;
	dst->UpdateValidity(rc);

	GL_INS("MV_Growlanser: %x -> %x %dx%d", RSBP, RDBP, src->GetUnscaledWidth(), src->GetUnscaledHeight());

	g_gs_device->StretchRectAuto(
		src->GetTexture(), GSVector4(rc) / GSVector4(src->GetUnscaledSize()).xyxy(),
		dst->GetTexture(), GSVector4(rc) * GSVector4(dst->GetScale()),
		Nearest);

	s_last_hacked_move_n = r.s_n;
	return true;
}

bool GSHwHack::MV_Ico(GSRendererHW& r)
{

	if (r.s_n == s_last_hacked_move_n && RSPSM == PSMT4 && RDPSM == PSMT4)
		return true;

	if (RSPSM != PSMZ32 || RDPSM != PSMCT32 || RWIDTH < 512 || RHEIGHT < 448)
		return false;

	GL_PUSH("MV_Ico: %x -> %x %dx%d", RSBP, RDBP, RWIDTH, RHEIGHT);

	GSTextureCache::Target *src, *dst;
	if (!GetMoveTargetPair(r, &src, &dst, false, false))
		return false;

	u32 pal[256];
	for (u32 i = 0; i < std::size(pal); i++)
		pal[i] = i << 24;
	std::shared_ptr<GSTextureCache::Palette> palette = g_texture_cache->LookupPaletteObject(pal, 256, true);
	if (!palette)
		return false;

	if (dst->GetUnscaledWidth() < static_cast<int>(RWIDTH) || dst->GetUnscaledHeight() < static_cast<int>(RHEIGHT))
	{
		if (!dst->ResizeTexture(std::max(dst->GetUnscaledWidth(), static_cast<int>(RWIDTH)),
				std::max(dst->GetUnscaledHeight(), static_cast<int>(RHEIGHT))))
		{
			return false;
		}
	}

	const GSVector4i draw_rc = GSVector4i(0, 0, RWIDTH, RHEIGHT).rintersect(dst->GetUnscaledRect());
	dst->UpdateValidChannels(PSMCT32, 0);
	dst->UpdateValidity(draw_rc);
	dst->UnscaleRTAlpha();
	dst->m_alpha_min = 0;
	dst->m_alpha_max = 255;

	GSHWDrawConfig& config = GSRendererHW::GetInstance()->BeginHLEHardwareDraw(
		dst->GetTexture(), nullptr, dst->GetScale(), src->GetTexture(), src->GetScale(), draw_rc);
	config.pal = palette->GetPaletteGSTexture();
	config.ps.channel = ChannelFetch_BLUE;
	config.ps.depth_fmt = 1;
	config.ps.tfx = TFX_DECAL;
	config.ps.tcc = true;
	GSRendererHW::GetInstance()->EndHLEHardwareDraw(false);

	s_last_hacked_move_n = r.s_n;
	return true;
}

#undef RBITBLTBUF
#undef RSBP
#undef RSBW
#undef RSPSM
#undef RDBP
#undef RDBW
#undef RDPSM
#undef RWIDTH
#undef RHEIGHT
#undef RSX
#undef RSY
#undef RDX
#undef RDY

#define CRC_F(name) { #name, &GSHwHack::name }

const GSHwHack::Entry<GSRendererHW::GSC_Ptr> GSHwHack::s_get_skip_count_functions[] = {
	CRC_F(GSC_IRem),
	CRC_F(GSC_Manhunt2),
	CRC_F(GSC_MidnightClub3),
	CRC_F(GSC_SacredBlaze),
	CRC_F(GSC_GuitarHero),
	CRC_F(GSC_SFEX3),
	CRC_F(GSC_DTGames),
	CRC_F(GSC_TalesOfLegendia),
	CRC_F(GSC_TalesofSymphonia),
	CRC_F(GSC_UrbanReign),
	CRC_F(GSC_BlackAndBurnoutSky),
	CRC_F(GSC_BlueTongueGames),
	CRC_F(GSC_NFSUndercover),
	CRC_F(GSC_PolyphonyDigitalGames),
	CRC_F(GSC_MetalGearSolid3),
	CRC_F(GSC_Battlefield2),
	CRC_F(GSC_Turok),

	CRC_F(GSC_NamcoGames),
	CRC_F(GSC_SandGrainGames),

	CRC_F(GSC_BurnoutGames),

	CRC_F(GSC_UltramanFightingEvolution),
};

const GSHwHack::Entry<GSRendererHW::OI_Ptr> GSHwHack::s_before_draw_functions[] = {
	CRC_F(OI_PointListPalette),
	CRC_F(OI_DBZBTGames),
	CRC_F(OI_RozenMaidenGebetGarden),
	CRC_F(OI_SonicUnleashed),
	CRC_F(OI_ArTonelico2),
	CRC_F(OI_BurnoutGames),
};

const GSHwHack::Entry<GSRendererHW::MV_Ptr> GSHwHack::s_move_handler_functions[] = {
	CRC_F(MV_Growlanser),
	CRC_F(MV_Ico),
};

#undef CRC_F

s16 GSLookupGetSkipCountFunctionId(const std::string_view name)
{
	for (u32 i = 0; i < std::size(GSHwHack::s_get_skip_count_functions); i++)
	{
		if (name == GSHwHack::s_get_skip_count_functions[i].name)
			return static_cast<s16>(i);
	}

	return -1;
}

s16 GSLookupBeforeDrawFunctionId(const std::string_view name)
{
	for (u32 i = 0; i < std::size(GSHwHack::s_before_draw_functions); i++)
	{
		if (name == GSHwHack::s_before_draw_functions[i].name)
			return static_cast<s16>(i);
	}

	return -1;
}

s16 GSLookupMoveHandlerFunctionId(const std::string_view name)
{
	for (u32 i = 0; i < std::size(GSHwHack::s_move_handler_functions); i++)
	{
		if (name == GSHwHack::s_move_handler_functions[i].name)
			return static_cast<s16>(i);
	}

	return -1;
}

void GSRendererHW::UpdateRenderFixes()
{
	GSRenderer::UpdateRenderFixes();

	m_nativeres = (GSConfig.UpscaleMultiplier == 1.0f);
	s_nativeres = m_nativeres;

	m_gsc = nullptr;
	m_oi = nullptr;
	m_mv = nullptr;

	if (!GSConfig.UserHacks_DisableRenderFixes)
	{
		if (GSConfig.GetSkipCountFunctionId >= 0 &&
			static_cast<size_t>(GSConfig.GetSkipCountFunctionId) < std::size(GSHwHack::s_get_skip_count_functions))
		{
			m_gsc = GSHwHack::s_get_skip_count_functions[GSConfig.GetSkipCountFunctionId].ptr;
		}

		if (GSConfig.BeforeDrawFunctionId >= 0 &&
			static_cast<size_t>(GSConfig.BeforeDrawFunctionId) < std::size(GSHwHack::s_before_draw_functions))
		{
			m_oi = GSHwHack::s_before_draw_functions[GSConfig.BeforeDrawFunctionId].ptr;
		}

		if (GSConfig.MoveHandlerFunctionId >= 0 &&
			static_cast<size_t>(GSConfig.MoveHandlerFunctionId) < std::size(GSHwHack::s_move_handler_functions))
		{
			m_mv = GSHwHack::s_move_handler_functions[GSConfig.MoveHandlerFunctionId].ptr;
		}
	}
}

bool GSRendererHW::IsBadFrame()
{
	if (m_gsc)
	{
		if (!m_gsc(*this, m_skip))
			return false;
	}

	if (m_skip == 0 && GSConfig.SkipDrawEnd > 0)
	{
		if (PRIM->TME)
		{
			if (GSLocalMemory::m_psm[m_context->TEX0.PSM].depth ||
				GSUtil::HasSharedBits(m_context->FRAME.Block(), m_context->FRAME.PSM, m_context->TEX0.TBP0, m_context->TEX0.PSM))
			{
				m_skip_offset = GSConfig.SkipDrawStart;
				m_skip = GSConfig.SkipDrawEnd;
			}
		}
	}

	if (m_skip > 0)
	{
		m_skip--;

		if (m_skip_offset > 1)
			m_skip_offset--;
		else
			return true;
	}

	return false;
}
