// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GS/GSGL.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"
#include "Host.h"
#include "common/Console.h"
#include "common/BitUtils.h"
#include "common/StringUtil.h"
#include <bit>
#ifdef ENABLE_VR
#include "VR/DepthHistogram.h"
#include "VR/StereoState.h"
#endif

using PS_ATST  = GSShader::PS_ATST;
using PS_AFAIL = GSShader::PS_AFAIL;

GSRendererHW::GSRendererHW()
	: GSRenderer()
{
	MULTI_ISA_SELECT(GSRendererHWPopulateFunctions)(*this);
	m_mipmap = GSConfig.HWMipmap;
	SetTCOffset();

	pxAssert(!g_texture_cache);
	g_texture_cache = std::make_unique<GSTextureCache>();
	GSTextureReplacements::Initialize();

	m_drawlist.reserve(2048);

	memset(static_cast<void*>(&m_conf), 0, sizeof(m_conf));

	ResetStates();
}

void GSRendererHW::SetTCOffset()
{
	m_userhacks_tcoffset_x = std::max<s32>(GSConfig.UserHacks_TCOffsetX, 0) / -1000.0f;
	m_userhacks_tcoffset_y = std::max<s32>(GSConfig.UserHacks_TCOffsetY, 0) / -1000.0f;
	m_userhacks_tcoffset = m_userhacks_tcoffset_x < 0.0f || m_userhacks_tcoffset_y < 0.0f;
}

GSRendererHW::~GSRendererHW()
{
	g_texture_cache.reset();
}

void GSRendererHW::Destroy()
{
	g_texture_cache->RemoveAll(true, true, true);
	GSRenderer::Destroy();
}

void GSRendererHW::PurgeTextureCache(bool sources, bool targets, bool hash_cache)
{
	g_texture_cache->RemoveAll(sources, targets, hash_cache);
}

void GSRendererHW::ReadbackTextureCache()
{
	g_texture_cache->ReadbackAll();
}

GSTexture* GSRendererHW::LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, const GSVector2i& size)
{
	return g_texture_cache->LookupPaletteSource(CBP, CPSM, CBW, offset, scale, size);
}

bool GSRendererHW::CanUpscale()
{
	return GSConfig.UpscaleMultiplier != 1.0f;
}

float GSRendererHW::GetUpscaleMultiplier()
{
	return GSConfig.UpscaleMultiplier;
}

void GSRendererHW::Reset(bool hardware_reset)
{
	if (!hardware_reset)
		g_texture_cache->ReadbackAll();

	g_texture_cache->RemoveAll(true, true, true);

	GSRenderer::Reset(hardware_reset);
}

void GSRendererHW::UpdateSettings(const Pcsx2Config::GSOptions& old_config)
{
	GSRenderer::UpdateSettings(old_config);
	m_mipmap = GSConfig.HWMipmap;
	SetTCOffset();
}

void GSRendererHW::VSync(u32 field, bool registers_written, bool idle_frame)
{
	if (GSConfig.LoadTextureReplacements)
		GSTextureReplacements::ProcessAsyncLoadedTextures();

	if (!idle_frame)
	{
		if ((s_n - s_last_transfer_draw_n) < 5)
		{
			for (auto iter = m_draw_transfers.rbegin(); iter != m_draw_transfers.rend(); iter++)
			{
				if ((s_n - iter->draw) > 50)
				{
					m_draw_transfers.erase(m_draw_transfers.begin(), std::next(iter).base());
					break;
				}
			}
		}
		else
		{
			m_draw_transfers.clear();
		}

		g_texture_cache->IncAge();
	}
	else
	{
		GL_INS("HW: No draws or transfers, not aging TC");
	}

	if (g_texture_cache->GetHashCacheMemoryUsage() > 1024 * 1024 * 1024)
	{
		Host::AddKeyedOSDMessage("HashCacheOverflow",
			fmt::format(TRANSLATE_FS("GS", "Hash cache has used {:.2f} MB of VRAM, disabling."),
				static_cast<float>(g_texture_cache->GetHashCacheMemoryUsage()) / 1048576.0f),
			Host::OSD_ERROR_DURATION);
		g_texture_cache->RemoveAll(true, false, true);
		g_gs_device->PurgePool();
		GSConfig.TexturePreloading = TexturePreloadingLevel::Partial;
	}

	m_skip = 0;
	m_skip_offset = 0;

	GSRenderer::VSync(field, registers_written, idle_frame);
}

GSTexture* GSRendererHW::GetOutput(int i, float& scale, int& y_offset)
{
	int index = i >= 0 ? i : 1;

	GSPCRTCRegs::PCRTCDisplay& curFramebuffer = PCRTCDisplays.PCRTCDisplays[index];
	const GSVector2i framebufferSize(PCRTCDisplays.GetFramebufferSize(i));

	if (curFramebuffer.framebufferRect.rempty() || curFramebuffer.FBW == 0 || framebufferSize.x < 0 || framebufferSize.y < 0)
		return nullptr;

	PCRTCDisplays.RemoveFramebufferOffset(i);

	GSTexture* t = nullptr;

	GIFRegTEX0 TEX0 = {};
	TEX0.TBP0 = curFramebuffer.Block();
	TEX0.TBW = curFramebuffer.FBW;
	TEX0.PSM = curFramebuffer.PSM;

	if (GSTextureCache::Target* rt = g_texture_cache->LookupDisplayTarget(TEX0, framebufferSize, GetTextureScaleFactor(), false))
	{
		const u32 bp_adj = (TEX0.TBP0 < rt->m_TEX0.TBP0 && rt->UnwrappedEndBlock() > GS_MAX_BLOCKS) ? (TEX0.TBP0 + GS_MAX_BLOCKS) : TEX0.TBP0;
		rt->Update();
		t = rt->m_texture;
		scale = rt->m_scale;

		const int delta = bp_adj - rt->m_TEX0.TBP0;
		if (delta > 0 && curFramebuffer.FBW != 0)
		{
			const int pages = delta >> 5u;
			int y_pages = pages / curFramebuffer.FBW;
			y_offset = y_pages * GSLocalMemory::m_psm[curFramebuffer.PSM].pgs.y;
			GL_CACHE("HW: Frame y offset %d pixels, unit %d", y_offset, i);
		}

		if (GSConfig.SaveFrame && GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
		{
			GSTexture* save_tex = t;
#ifdef ENABLE_VR
			if (save_tex->GetArrayLayers() > 1)
			{
				static const char* layer_env = std::getenv("PCSX2_VR_SNAPSHOT_LAYER");
				if (layer_env && layer_env[0] == '1')
					save_tex = save_tex->GetLayerProxyTexture(1);
			}
#endif
			save_tex->Save(GetDrawDumpPath("%05lld_f%05lld_fr%d_%05x_%s.bmp", s_n, g_perfmon.GetFrame(), i, static_cast<int>(TEX0.TBP0), GSUtil::GetPSMName(TEX0.PSM)));
		}
	}

	return t;
}

GSTexture* GSRendererHW::GetFeedbackOutput(float& scale)
{
	const int index = m_regs->EXTBUF.FBIN & 1;
	const GSVector2i fb_size(PCRTCDisplays.GetFramebufferSize(index));

	GIFRegTEX0 TEX0 = {};
	TEX0.TBP0 = m_regs->EXTBUF.EXBP;
	TEX0.TBW = m_regs->EXTBUF.EXBW;
	TEX0.PSM = PCRTCDisplays.PCRTCDisplays[index].PSM;

	GSTextureCache::Target* rt = g_texture_cache->LookupDisplayTarget(TEX0, fb_size, GetTextureScaleFactor(), true);
	if (!rt)
		return nullptr;

	rt->Update();
	GSTexture* t = rt->m_texture;
	scale = rt->m_scale;

	if (GSConfig.SaveFrame && GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		GSTexture* save_tex = t;
#ifdef ENABLE_VR
		if (save_tex->GetArrayLayers() > 1)
		{
			static const char* layer_env = std::getenv("PCSX2_VR_SNAPSHOT_LAYER");
			if (layer_env && layer_env[0] == '1')
				save_tex = save_tex->GetLayerProxyTexture(1);
		}
#endif
		save_tex->Save(GetDrawDumpPath("%05lld_f%05lld_fr%d_%05x_%s.bmp", s_n, g_perfmon.GetFrame(), 3, static_cast<int>(TEX0.TBP0), GSUtil::GetPSMName(TEX0.PSM)));
	}

	return t;
}

void GSRendererHW::Lines2Sprites()
{
	pxAssert(m_vt.m_primclass == GS_SPRITE_CLASS);

	while (m_vertex->tail * 2 > m_vertex->maxcount)
	{
		GrowVertexBuffer();
	}

	const bool predivide_q = PRIM->TME && !PRIM->FST && m_vt.m_accurate_stq;

	if (m_vertex->next >= 2)
	{
		const u32 count = m_vertex->next;

		int i = static_cast<int>(count) * 2 - 4;
		GSVertex* s = &m_vertex->buff[count - 2];
		GSVertex* q = &m_vertex->buff[count * 2 - 4];
		u16* RESTRICT index = &m_index->buff[count * 3 - 6];

		constexpr GSVector4i indices = GSVector4i::cxpr16(0, 1, 2, 1, 2, 3, 0, 0);

		for (; i >= 0; i -= 4, s -= 2, q -= 4, index -= 6)
		{
			GSVertex v0 = s[0];
			GSVertex v1 = s[1];

			v0.RGBAQ = v1.RGBAQ;
			v0.XYZ.Z = v1.XYZ.Z;
			v0.FOG = v1.FOG;

			if (predivide_q)
			{
				const GSVector4 st0 = GSVector4::loadl(&v0.ST.U64);
				const GSVector4 st1 = GSVector4::loadl(&v1.ST.U64);
				const GSVector4 Q = GSVector4(v1.RGBAQ.Q, v1.RGBAQ.Q, v1.RGBAQ.Q, v1.RGBAQ.Q);
				const GSVector4 st = st0.upld(st1) / Q;

				GSVector4::storel(&v0.ST.U64, st);
				GSVector4::storeh(&v1.ST.U64, st);

				v0.RGBAQ.Q = 1.0f;
				v1.RGBAQ.Q = 1.0f;
			}

			q[0] = v0;
			q[3] = v1;

			const u16 x = v0.XYZ.X;
			v0.XYZ.X = v1.XYZ.X;
			v1.XYZ.X = x;

			const float v0_st_s = v0.ST.S;
			v0.ST.S = v1.ST.S;
			v1.ST.S = v0_st_s;

			const u16 u = v0.U;
			v0.U = v1.U;
			v1.U = u;

			q[1] = v0;
			q[2] = v1;

			const GSVector4i this_indices = GSVector4i::broadcast16(i).add16(indices);
			const int high = this_indices.extract32<2>();
			GSVector4i::storel(index, this_indices);
			std::memcpy(&index[4], &high, sizeof(high));
		}

		m_vertex->head = m_vertex->tail = m_vertex->next = count * 2;
		m_index->tail = count * 3;
	}
}

void GSRendererHW::ExpandLineIndices()
{
	const u32 process_count = (m_index->tail + 7) / 8 * 8;
	constexpr u32 expansion_factor = 3;
	m_index->tail *= expansion_factor;
	GSVector4i* end = reinterpret_cast<GSVector4i*>(m_index->buff);
	GSVector4i* read = reinterpret_cast<GSVector4i*>(m_index->buff + process_count);
	GSVector4i* write = reinterpret_cast<GSVector4i*>(m_index->buff + process_count * expansion_factor);

	constexpr GSVector4i mask0 = GSVector4i::cxpr8(0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 2, 3, 4, 5, 4, 5);
	constexpr GSVector4i mask1 = GSVector4i::cxpr8(6, 7, 4, 5, 6, 7, 6, 7, 8, 9, 8, 9, 10, 11, 8, 9);
	constexpr GSVector4i mask2 = GSVector4i::cxpr8(10, 11, 10, 11, 12, 13, 12, 13, 14, 15, 12, 13, 14, 15, 14, 15);

	constexpr GSVector4i low0 = GSVector4i::cxpr16(0, 1, 2, 1, 2, 3, 0, 1);
	constexpr GSVector4i low1 = GSVector4i::cxpr16(2, 1, 2, 3, 0, 1, 2, 1);
	constexpr GSVector4i low2 = GSVector4i::cxpr16(2, 3, 0, 1, 2, 1, 2, 3);

	while (read > end)
	{
		read -= 1;
		write -= expansion_factor;

		const GSVector4i in = read->sll16<2>();
		write[0] = in.shuffle8(mask0) | low0;
		write[1] = in.shuffle8(mask1) | low1;
		write[2] = in.shuffle8(mask2) | low2;
	}
}

template<u32 primclass, bool fst>
GSRendererHW::TextureShuffleInfo GSRendererHW::DetectTextureShuffleImpl()
{
	static_assert(primclass == GS_SPRITE_CLASS || primclass == GS_TRIANGLE_CLASS);

	const GIFRegFRAME& frame = m_context->FRAME;
	const GIFRegTEX0& tex0 = m_context->TEX0;
	const GIFRegCLAMP& clamp = m_context->CLAMP;
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[frame.PSM];
	const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[tex0.PSM];
	const GSVertex* RESTRICT verts = m_vertex->buff;
	const u16* RESTRICT index = m_index->buff;
	constexpr u32 verts_per_quad = primclass == GS_SPRITE_CLASS ? 2 : 6;
	const u32 num_quads = m_index->tail / verts_per_quad;

	if (!PRIM->TME ||
		(primclass == GS_SPRITE_CLASS && m_index->tail < 2) ||
		(primclass == GS_TRIANGLE_CLASS && m_index->tail < 6) ||
		(m_index->tail % verts_per_quad != 0) ||
		(frame_psm.bpp != 16))
	{
		return { TextureShuffleType::None, TextureShuffleChannels_None };
	}

	const auto GetQuadXYUV = [&](u32 i, GSVector4i& xyout, GSVector4i& uvout) {
		GSVertex v0, v1;
		if (!GetQuadCorners(verts, index + verts_per_quad * i, v0, v1))
			return false;

		GSVector4 xy, uv;
		GetQuadBBoxWindow(v0, v1, xy, uv);

		GetQuadRasterizedPoints(xy, uv);

		const GSVector4i exclusive(0, 0, 1, 1);
		xyout = GSVector4i(xy) + exclusive;

		const int uvswap = (uv.xyxy() > uv.zwzw()).mask();
		uvout = GSVector4i(uv.floor());
		uvout = uvout.xyxy().runion(uvout.zwzw()) + exclusive;
		if (uvswap & 1)
			uvout = uvout.zyxw();
		if (uvswap & 2)
			uvout = uvout.xwzy();
		
		return true;
	};

	const auto Is8PixelReversal = [](const GSVector4i& xy, const GSVector4i& uv) {
		const int x_pixels = xy.width();
		return x_pixels == 8 && (xy.xxzz() == uv.zzxx()).alltrue();
	};

	GL_PUSH("HW: Texture shuffle detection");

	int first_quad = 0;

	GSVector4i xy;
	GSVector4i uv;
	if (!GetQuadXYUV(first_quad, xy, uv))
		return { TextureShuffleType::None, TextureShuffleChannels_None };

	if (Is8PixelReversal(xy, uv))
	{
		if (!GetQuadXYUV(++first_quad, xy, uv))
			return { TextureShuffleType::None, TextureShuffleChannels_None };
	}

	GL_INS("Detecting based on quad: pos={%d, %d, %d, %d}, tex={%d, %d, %d, %d}",
		xy.x, xy.y, xy.z, xy.w, uv.x, uv.y, uv.z, uv.w);

	const int x_pixels = xy.width();
	const int y_pixels = xy.height();
	const int u_pixels = std::abs(uv.width());
	const int v_pixels = std::abs(uv.height());

	const int x_u_offset = std::abs(xy.x - uv.x) % 16;

	const bool two_pixel = x_pixels == 2 && y_pixels == 2 && u_pixels == 1 && v_pixels == 1 &&
	                       tex_psm.bpp == 16 && frame_psm.bpp == 16 && num_quads == 1;

	if (x_pixels != u_pixels && !two_pixel)
	{
		GL_INS("Not a shuffle (X pixels != U pixels)");
		return { TextureShuffleType::None, TextureShuffleChannels_None };
	}

	if ((x_pixels % 8) != 0 && !two_pixel)
	{
		GL_INS("Not a shuffle (X width not multiple of 8)");
		return { TextureShuffleType::None, TextureShuffleChannels_None };
	}

	const auto CheckSwizzleShuffle = [&]() {
		if (!(x_pixels == 8 && y_pixels == 8 && u_pixels == 8 && v_pixels == 8))
			return false;

		GSVector4i xy_1(GSVector4i::zero()), uv_1(GSVector4i::zero());
		if (!GetQuadXYUV(first_quad + 1, xy_1, uv_1))
			return false;

		const GSVector4i dxy = (xy_1.xyxy() - xy.xyxy()).abs32();
		const GSVector4i duv = (uv_1.xyxy() - uv.xyxy()).abs32();

		return (dxy == duv.yxyx()).alltrue();
	};

	const auto CheckQuadOffsetXU = [&](
		int offset, const GSVector4i& xy0, const GSVector4i& uv0,
		const GSVector4i& xy1, const GSVector4i& uv1) {
			return
				((std::abs(xy1.x - xy0.z) == offset) && (std::abs(uv1.x - uv0.z) == offset)) ||
				((std::abs(xy0.x - xy1.z) == offset) && (std::abs(uv0.x - uv1.z) == offset));
	};

	const auto CheckNextQuadOffsetXU = [&](int offset) {
		GSVector4i xy_1(GSVector4i::zero()), uv_1(GSVector4i::zero());
		if (!GetQuadXYUV(first_quad + 1, xy_1, uv_1))
			return false;

		return CheckQuadOffsetXU(offset, xy, uv, xy_1, uv_1);
	};

	const auto CheckGappedSwizzleShuffle = [&]() {
		if (!CheckSwizzleShuffle())
			return false;

		GSVector4i xy_1(GSVector4i::zero()), uv_1(GSVector4i::zero()), xy_2(GSVector4i::zero()), uv_2(GSVector4i::zero());
		if (!GetQuadXYUV(first_quad + 1, xy_1, uv_1))
			return false;
		if (!GetQuadXYUV(first_quad + 2, xy_2, uv_2))
			return false;

		return xy_2.y - xy_1.w == 16;
	};

	const auto HasLowerOnes = [](int x) { return x != 0 && (x & (x + 1)) == 0; };

	const auto RegionRepeatClears8 = [&]() {
		return !(clamp.MINU & 8) && HasLowerOnes(clamp.MINU | 8);
	};

	const auto RegionRepeatSets8 = [&]() {
		return clamp.MAXU == 8;
	};

	const auto CheckRegionRepeat8 = [&]() {
		return (clamp.WMS == CLAMP_REGION_REPEAT) && (RegionRepeatClears8() || RegionRepeatSets8());
	};

	const auto CheckRegionRepeat16 = [&]() {
		return (clamp.WMS == CLAMP_REGION_REPEAT) && clamp.MINU == 0xF && ((clamp.MAXU & 0xF) == 0);
	};

	TextureShuffleType shuffle_type = TextureShuffleType::None;
	if (tex_psm.bpp == 32)
	{

		if (CheckSwizzleShuffle() && frame.FBMSK == 0xFFFF)
		{
			shuffle_type = TextureShuffleType::SwizzleTex32;
			GL_INS("SwizzleTex32 shuffle detected.");
		}
	}
	else
	{

		if (two_pixel)
		{
			shuffle_type = TextureShuffleType::TwoPixel;
			GL_INS("TwoPixel shuffle detected.");
		}
		else if ((x_u_offset == 0) && (num_quads == 1 || CheckNextQuadOffsetXU(0) || CheckNextQuadOffsetXU(8)) &&
			HasLowerOnes(frame.FBMSK) && tex0.TBP0 != frame.Block() && IsOpaque() && !m_vt.IsRealLinear())
		{
			shuffle_type = TextureShuffleType::Copy;
			GL_INS("Copy shuffle detected.");
		}
		else if (x_u_offset == 8 && (num_quads == 1 || CheckNextQuadOffsetXU(0) || CheckNextQuadOffsetXU(8)) &&
			clamp.WMS != CLAMP_REGION_REPEAT)
		{
			shuffle_type = TextureShuffleType::Offset;
			GL_INS("Offset shuffle detected.");
		}
		else if (CheckRegionRepeat8())
		{
			shuffle_type = TextureShuffleType::RegionRepeat8;
			GL_INS("RegionRepeat8 shuffle detected: UMSK=%x UFIX=%x", clamp.MINU, clamp.MAXU);
		}
		else if (CheckRegionRepeat16() && x_pixels == 16 && clamp.MAXU == xy.x && num_quads == 1)
		{
			shuffle_type = TextureShuffleType::RegionRepeat16;
			GL_INS("RegionRepeat16 shuffle detected: UMSK=%x UFIX=%x", clamp.MINU, clamp.MAXU);
		}
		else if ((x_u_offset == 0) && (x_pixels == 16) && (u_pixels == 16) &&
			(xy.x < xy.z) != (uv.x < uv.z))
		{
			shuffle_type = TextureShuffleType::Reverse;
			GL_INS("Reverse shuffle detected.");
		}
		else if (CheckGappedSwizzleShuffle())
		{
			shuffle_type = TextureShuffleType::GappedSwizzle;
			GL_INS("GappedSwizzle shuffle detected (NFS Undercover).");
		}
		else if (CheckSwizzleShuffle())
		{
			shuffle_type = TextureShuffleType::Swizzle;
			GL_INS("Swizzle shuffle detected.");
		}
	}

	if (shuffle_type == TextureShuffleType::None)
	{
		GL_INS("No shuffle (none passed detection).");
		return { shuffle_type, TextureShuffleChannels_None };
	}

	const u32 fbmsk16 = GetEffectiveTextureShuffleFbmsk();
	const u32 rb_mask = (((fbmsk16 >> 0) & 0xFF) == 0xFF) ? 0 : 0xFFFFFF;
	const u32 ga_mask = (((fbmsk16 >> 8) & 0xFF) == 0xFF) ? 0 : 0xFFFFFF;

	u32 r_mask = rb_mask;
	u32 g_mask = ga_mask;
	u32 b_mask = rb_mask;
	u32 a_mask = ga_mask;

	if (x_pixels <= 8)
	{
		const int x_offset = std::abs(xy.x) % 16;
		if (x_offset == 0)
		{
			b_mask = 0;
			a_mask = 0;
			GL_INS("Small width (dX = 8): writing to only R, G.");
		}
		else if (x_offset == 8)
		{
			r_mask = 0;
			g_mask = 0;
			GL_INS("Small width (dX = 8): writing to only B, A.");
		}
		else
		{
			pxFail("Impossible.");
		}
	}

	const bool swap_columns = x_u_offset != 0 || shuffle_type == TextureShuffleType::Reverse;

	u32 shuffle_channels = 0;
	
	switch (shuffle_type)
	{
		case TextureShuffleType::TwoPixel:
		case TextureShuffleType::Copy:
		case TextureShuffleType::Offset:
		case TextureShuffleType::Reverse:
		case TextureShuffleType::GappedSwizzle:
		case TextureShuffleType::Swizzle:
		case TextureShuffleType::RegionRepeat16:
		{
			if (swap_columns)
			{
				shuffle_channels |= TextureShuffleChannels_RedToBlue & b_mask;
				shuffle_channels |= TextureShuffleChannels_BlueToRed & r_mask;
				shuffle_channels |= TextureShuffleChannels_GreenToAlpha & a_mask;
				shuffle_channels |= TextureShuffleChannels_AlphaToGreen & g_mask;
			}
			else
			{
				shuffle_channels |= TextureShuffleChannels_RedCopy & r_mask;
				shuffle_channels |= TextureShuffleChannels_BlueCopy & b_mask;
				shuffle_channels |= TextureShuffleChannels_GreenCopy & g_mask;
				shuffle_channels |= TextureShuffleChannels_AlphaCopy & a_mask;
			}
		}
		break;

		case TextureShuffleType::RegionRepeat8:
		{
			if (RegionRepeatClears8())
			{
				shuffle_channels |= TextureShuffleChannels_RedCopy & r_mask;
				shuffle_channels |= TextureShuffleChannels_RedToBlue & b_mask;
				shuffle_channels |= TextureShuffleChannels_GreenCopy & g_mask;
				shuffle_channels |= TextureShuffleChannels_GreenToAlpha & a_mask;
			}
			else if (RegionRepeatSets8())
			{
				shuffle_channels |= TextureShuffleChannels_BlueCopy & r_mask;
				shuffle_channels |= TextureShuffleChannels_BlueToRed & b_mask;
				shuffle_channels |= TextureShuffleChannels_AlphaCopy & g_mask;
				shuffle_channels |= TextureShuffleChannels_AlphaToGreen & a_mask;
			}
		}
		break;

		case TextureShuffleType::SwizzleTex32:
		{
			shuffle_channels = TextureShuffleChannels_BlueToAlpha;
		}
		break;

		default:
			pxFail("Unhandled shuffle type.");
			break;
	}

	if (frame.Block() == tex0.TBP0)
	{
		if (shuffle_channels & TextureShuffleChannels_RedCopy)
		{
			GL_INS("Disable R -> R, recursive draw.");
			shuffle_channels &= ~TextureShuffleChannels_RedCopy;
		}
		if (shuffle_channels & TextureShuffleChannels_GreenCopy)
		{
			GL_INS("Disable G -> G, recursive draw.");
			shuffle_channels &= ~TextureShuffleChannels_GreenCopy;
		}
		if (shuffle_channels & TextureShuffleChannels_BlueCopy)
		{
			GL_INS("Disable B -> B, recursive draw.");
			shuffle_channels &= ~TextureShuffleChannels_BlueCopy;
		}
		if (shuffle_channels & TextureShuffleChannels_AlphaCopy)
		{
			GL_INS("Disable A -> A, recursive draw.");
			shuffle_channels &= ~TextureShuffleChannels_AlphaCopy;
		}
	}

	const bool disable_clobber_write = frame.Block() != tex0.TBP0;

	const bool quad_mixes_16_pixel_groups =
		x_u_offset == 8 && x_pixels >= 16 && shuffle_type != TextureShuffleType::RegionRepeat16;

	if (disable_clobber_write && quad_mixes_16_pixel_groups)
	{
		const auto RoundPage = [](int x) { return (x + 32) & ~63; };

		const int x_page_offset = xy.x - RoundPage(xy.x);
		const int u_page_offset = uv.x - RoundPage(uv.x);

		bool r_clobber = false;
		bool g_clobber = false;
		bool b_clobber = false;
		bool a_clobber = false;

		if ((x_page_offset == 0 && u_page_offset == 8) ||
			(x_page_offset == -8 && u_page_offset == 0))
		{
			b_clobber = (shuffle_channels & TextureShuffleChannels_WriteBlue) != 0;
			a_clobber = (shuffle_channels & TextureShuffleChannels_WriteAlpha) != 0;
		}
		else if ((x_page_offset == 8 && u_page_offset == 0) ||
			(x_page_offset == 0 && u_page_offset == -8))
		{
			r_clobber = (shuffle_channels & TextureShuffleChannels_WriteRed) != 0;
			g_clobber = (shuffle_channels & TextureShuffleChannels_WriteGreen) != 0;
		}

		GL_INS("Non-recursive draw, disable writes to clobbered channels: R: %d, G: %d, B: %d, A: %d.",
			r_clobber, g_clobber, b_clobber, a_clobber);

		if (r_clobber)
			shuffle_channels &= ~TextureShuffleChannels_WriteRed;
		if (g_clobber)
			shuffle_channels &= ~TextureShuffleChannels_WriteGreen;
		if (b_clobber)
			shuffle_channels &= ~TextureShuffleChannels_WriteBlue;
		if (a_clobber)
			shuffle_channels &= ~TextureShuffleChannels_WriteAlpha;
	}

	if (shuffle_channels & TextureShuffleChannels_RedToBlue)
		GL_INS("Color shuffle: R -> B.");
	if (shuffle_channels & TextureShuffleChannels_BlueToRed)
		GL_INS("Color shuffle: B -> R.");
	if (shuffle_channels & TextureShuffleChannels_GreenToAlpha)
		GL_INS("Color shuffle: G -> A.");
	if (shuffle_channels & TextureShuffleChannels_AlphaToGreen)
		GL_INS("Color shuffle: A -> G.");
	if (shuffle_channels & TextureShuffleChannels_RedCopy)
		GL_INS("Color shuffle: R -> R.");
	if (shuffle_channels & TextureShuffleChannels_GreenCopy)
		GL_INS("Color shuffle: G -> G.");
	if (shuffle_channels & TextureShuffleChannels_BlueCopy)
		GL_INS("Color shuffle: B -> B.");
	if (shuffle_channels & TextureShuffleChannels_AlphaCopy)
		GL_INS("Color shuffle: A -> A.");
	if (shuffle_channels & TextureShuffleChannels_BlueToAlpha)
		GL_INS("Color shuffle: B -> A.");

	return { shuffle_type, static_cast<TextureShuffleChannels>(shuffle_channels) };
}

void GSRendererHW::DetectTextureShuffle()
{
	if (m_vt.m_primclass == GS_SPRITE_CLASS)
	{
		if (PRIM->FST)
			m_texture_shuffle = DetectTextureShuffleImpl<GS_SPRITE_CLASS, true>();
		else
			m_texture_shuffle = DetectTextureShuffleImpl<GS_SPRITE_CLASS, false>();
	}
	else if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
	{
		if (PRIM->FST)
			m_texture_shuffle = DetectTextureShuffleImpl<GS_TRIANGLE_CLASS, true>();
		else
			m_texture_shuffle = DetectTextureShuffleImpl<GS_TRIANGLE_CLASS, false>();
	}
	else
	{
		m_texture_shuffle = TextureShuffleInfo();
	}
}

void GSRendererHW::DetectTextureShuffleSecondPass(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	if (!m_process_texture || !rt)
	{
		GL_INS("HW: Texture shuffle detection (2): Not detected.");
		m_texture_shuffle.Disable();
		return;
	}

	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	if (frame_psm.bpp != 16)
	{
		GL_INS("HW: Texture shuffle detection (2): Failed (not 16 bit RT).");
		m_texture_shuffle.Disable();
		return;
	}

	if (m_texture_shuffle)
	{
		if (tex && tex->m_32_bits_fmt)
		{
			GL_INS("HW: Texture shuffle detection (2): Passed.");
		}
		else
		{
			const auto HasLowerOnes = [&](u32 x) { return x != 0 && (x & (x + 1)) == 0; };
			if (m_cached_ctx.TEX0.TBP0 != m_cached_ctx.FRAME.Block() &&
				rt && rt->m_32_bits_fmt == true && IsOpaque() && !m_vt.IsRealLinear() &&
				HasLowerOnes(m_cached_ctx.FRAME.FBMSK))
			{
				GL_INS("HW: Texture shuffle detection (2): Passed (real 16 bit source).");
				m_texture_shuffle.real_16_bit_source = true;
			}
			else
			{
				GL_INS("HW: Texture shuffle detection (2): Failed (not reinterpreting source as 16 bit).");
				m_texture_shuffle.Disable();
			}
		}
	}
	else
	{
		const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM];
		if (PRIM->TME &&
			((m_vt.m_primclass == GS_SPRITE_CLASS || m_vt.m_primclass == GS_TRIANGLE_CLASS) && TrianglesAreQuads(true)) &&
			(tex_psm.bpp == 16) && (frame_psm.bpp == 16) && rt && rt->m_32_bits_fmt && tex && tex->m_32_bits_fmt)
		{
			GL_INS("HW: Texture shuffle detection (2): Passed (HACK: reinterpreting both source/RT as 16 bit).");
			m_texture_shuffle.type = TextureShuffleType::HackShuffle;
		}
		else
		{
			GL_INS("HW: Texture shuffle detection (2): Failed.");
			m_texture_shuffle.Disable();
		}
	}
}

template<u32 primclass, bool fst>
void GSRendererHW::ConvertSpriteTextureShuffleImpl(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	const GSVector4i xyof = m_context->scissor.xyof.xyxy();
	const float tw = static_cast<float>(1 << m_cached_ctx.TEX0.TW);
	const float th = static_cast<float>(1 << m_cached_ctx.TEX0.TH);
	const bool bilinear = m_vt.IsRealLinear();
	
	GSVertex v_default = primclass == GS_SPRITE_CLASS ? m_vertex->buff[m_index->buff[1]] :
	                                                    m_vertex->buff[m_index->buff[2]];

	const auto SetTexCoords = [&](float u, float v, GSVertex& vtx_out) {
		if constexpr (fst)
		{
			vtx_out.U = static_cast<u32>(u * 16.0f);
			vtx_out.V = static_cast<u32>(v * 16.0f);
		}
		else
		{
			vtx_out.ST.S = u / tw;
			vtx_out.ST.T = v / th;
			vtx_out.RGBAQ.Q = 1.0f;
		}
	};

	const auto SetPosCoords = [&](float x, float y, GSVertex& vtx_out) {
		vtx_out.XYZ.X = xyof.x + static_cast<u32>(x * 16.0f);
		vtx_out.XYZ.Y = xyof.y + static_cast<u32>(y * 16.0f);
	};

	const auto WriteQuad = [&](const GSVector4i& xyi, const GSVector4i& uvi, GSVertex*& vout, u16*& iout) {
		GSVector4 xy(xyi);
		GSVector4 uv(uvi);

		if (bilinear)
		{
			GL_INS("HW: Translate to texel center for bilinear.");
			uv += GSVector4(0.5f) / rt->GetScale();
		}

		if constexpr (primclass == GS_SPRITE_CLASS)
		{
			vout[0] = v_default;
			vout[1] = v_default;

			SetPosCoords(xy.x, xy.y, vout[0]);
			SetPosCoords(xy.z, xy.w, vout[1]);

			SetTexCoords(uv.x, uv.y, vout[0]);
			SetTexCoords(uv.z, uv.w, vout[1]);

			iout[0] = 0;
			iout[1] = 1;

			vout += 2;
			iout += 2;
		}
		else
		{
			vout[0] = v_default;
			vout[1] = v_default;
			vout[2] = v_default;
			vout[3] = v_default;

			SetPosCoords(xy.x, xy.y, vout[0]);
			SetPosCoords(xy.z, xy.y, vout[1]);
			SetPosCoords(xy.x, xy.w, vout[2]);
			SetPosCoords(xy.z, xy.w, vout[3]);

			SetTexCoords(uv.x, uv.y, vout[0]);
			SetTexCoords(uv.z, uv.y, vout[1]);
			SetTexCoords(uv.x, uv.w, vout[2]);
			SetTexCoords(uv.z, uv.w, vout[3]);

			iout[0] = 0;
			iout[1] = 1;
			iout[2] = 2;
			iout[3] = 1;
			iout[4] = 2;
			iout[5] = 3;

			vout += 4;
			iout += 6;
		}
	};

	const auto ShiftAlignRect = [&](const GSVector4& rf) {
		GSVector4i ri;
		
		const float rf_w = rf.z - rf.x;
		if (m_texture_shuffle.type != TextureShuffleType::TwoPixel || rf_w >= 8.0f)
		{
			const GSVector4 rf_snapped = (rf / 8.0f).round<Round_NearestInt>() * 8.0f;
			ri = GSVector4i(rf_snapped);
			
			const GSVector4 rf_offset = (rf - rf_snapped).round<Round_Truncate>();
			ri = ri.blend32<0xA>(ri + GSVector4i(rf_offset));
		}
		else
		{
			ri = GSVector4i(rf.floor()) + GSVector4i(0, 0, 1, 1);
		}

		const int w = ri.width();
		if (w >= 8 && (w & 8))
		{
			ri = ri.ralign<Align_Outside>(GSVector2i(16, 1));
		}
		else if (ri.x & 8)
		{
			ri.x -= 8;
			ri.z -= 8;
		}
		else if (ri.z & 8)
		{
			ri.x += 8;
			ri.z += 8;
		}
		return ri;
	};

	m_r = ShiftAlignRect(m_vt.m_min.p.xyxy(m_vt.m_max.p));
	GSVector4i tex_r = ShiftAlignRect(m_vt.m_min.t.xyxy(m_vt.m_max.t));
	GSVector4i scissor_r = ShiftAlignRect(GSVector4(m_context->scissor.in));

	GL_PUSH("HW: Converting to single quad for texture shuffle.");
	
	GL_INS("HW: Before adjustment: pos={%d, %d, %d, %d}, tex={%d, %d, %d, %d}, scissor={%d, %d, %d, %d}",
		m_r.x ,m_r.y, m_r.z, m_r.w, tex_r.x, tex_r.y, tex_r.z, tex_r.w,
		scissor_r.x, scissor_r.y, scissor_r.z, scissor_r.w);

	bool half_x = true;
	bool half_y = true;
	bool half_u = true;
	bool half_v = true;

	if (m_texture_shuffle.real_16_bit_source)
	{
		GL_INS("HW: Real 16 bit source: no tex coord change.");
		half_u = false;
		half_v = false;
	}

	if (m_split_texture_shuffle_pages > 0 ||
		m_texture_shuffle.type == TextureShuffleType::TwoPixel ||
		m_texture_shuffle.type == TextureShuffleType::HackShuffle)
	{
		GL_INS("HW: Split shuffle/TwoPixel/HackShuffle: no pos or tex coord change.");
		half_x = false;
		half_y = false;
		half_u = false;
		half_v = false;
	}
	else if (m_texture_shuffle.type == TextureShuffleType::GappedSwizzle)
	{
		GL_INS("HW: GappedSwizzle (NFS Undercover): rewriting rects to use full area.");

		half_x = false;
		half_y = false;
		half_u = false;
		half_v = false;

		m_r = rt->GetUnscaledRect();
		tex_r = tex->GetUnscaledRect();
		scissor_r = m_r;
	}
	else if (m_texture_shuffle.type == TextureShuffleType::Swizzle ||
	    m_texture_shuffle.type == TextureShuffleType::SwizzleTex32)
	{
		GL_INS("HW: Swizzle/SwizzleTex32: no tex coord change.");

		if (m_cached_ctx.FRAME.FBW == rt->m_TEX0.TBW * 2)
		{
			half_y = false;
		}
		else
		{
			half_x = false;
		}

		half_u = false;
		half_v = false;
	}
	else if (m_cached_ctx.TEX0.TBP0 != m_cached_ctx.FRAME.Block())
	{
		GL_INS("HW: Non-recursive draw, complex case.");

		const bool tex_tbw_is_wrong =
			tex->m_target &&
			(static_cast<int>(tex->m_from_target_TEX0.TBW * 64) < tex->m_from_target->m_valid.z / 2);

		const u32 draw_bw = std::max(m_r.z / 64, 1);

		const bool single_direction_doubled =
			((m_r.w > rt->m_valid.w) != (m_r.z > rt->m_valid.z)) ||
			(IsSinglePageDraw() && m_r.height() > 32);

		if (tex_tbw_is_wrong || (rt->m_TEX0.TBW % draw_bw) == 0 || single_direction_doubled)
		{
			u32 max_tex_draw_width = std::min(m_r.z, 1 << m_cached_ctx.TEX0.TW);

			const u32 clamp_minu = m_context->CLAMP.MINU;
			const u32 clamp_maxu = m_context->CLAMP.MAXU;

			switch (m_context->CLAMP.WMS)
			{
				case CLAMP_REGION_CLAMP:
					max_tex_draw_width = std::min(max_tex_draw_width, clamp_maxu);
					break;
				case CLAMP_REGION_REPEAT:
					max_tex_draw_width = std::min(max_tex_draw_width, (clamp_maxu | clamp_minu));
					break;
				default:
					break;
			}

			const int width_diff =
				static_cast<int>(m_env.CTXT[m_env.PRIM.CTXT].TEX0.TBW) -
				static_cast<int>((m_cached_ctx.FRAME.FBW + 1) >> 1);
			
			if (m_env.PRIM.TME && m_env.CTXT[m_env.PRIM.CTXT].TEX0.TBP0 == m_cached_ctx.FRAME.Block() &&
				GSLocalMemory::m_psm[m_env.CTXT[m_env.PRIM.CTXT].TEX0.PSM].bpp == 32 && width_diff >= 0)
			{
				const bool keep_width = width_diff > 0 || (m_cached_ctx.FRAME.FBW == 1 && width_diff == 0);

				if ((!keep_width && max_tex_draw_width >= m_cached_ctx.FRAME.FBW * 64) ||
					(single_direction_doubled && m_r.z >= rt->m_valid.z * 2))
				{
					half_v = false;
					half_y = false;
				}
				else
				{
					half_u = false;
					half_x = false;
				}
			}
			else
			{
				const u32 tex_width =
					tex_tbw_is_wrong ? tex->m_from_target->m_valid.z :
					tex->m_target ? tex->m_from_target_TEX0.TBW * 64 :
					max_tex_draw_width;

				const u32 tex_tbw = tex->m_target ? tex->m_from_target_TEX0.TBW : tex->m_TEX0.TBW;

				if ((m_cached_ctx.TEX0.TBW * 64 >= tex_width * 2) && (tex_tbw != m_cached_ctx.TEX0.TBW))
				{
					half_v = false;
					half_y = false;
				}
				else
				{
					half_u = false;
					half_x = false;
				}
			}
		}
		else
		{
			half_v = false;
			half_y = false;
			half_u = false;
			half_x = false;
		}
	}
	else
	{
		GL_INS("HW: Recursive draw, complex case.");

		const GSVector2i& frame_pgs = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;

		const u32 draw_page_width = m_r.width() / frame_pgs.x;

		if (draw_page_width != 1 && (m_r.w <= rt->m_valid.w) &&
			((m_r.z > static_cast<int>(m_cached_ctx.FRAME.FBW * 64)) || (rt->m_TEX0.TBW < m_cached_ctx.FRAME.FBW)))
		{
			half_y = false;
			half_v = false;
		}
		else
		{
			half_x = false;
			half_u = false;
		}
	}

	GL_INS("HW: Halving: X: %d, Y: %d, U: %d, V: %d", half_x, half_y, half_u, half_v);

	if (half_x)
	{
		m_r.x /= 2;
		m_r.z /= 2;
		scissor_r.x /= 2;
		scissor_r.z /= 2;
	}

	if (half_y)
	{
		m_r.y /= 2;
		m_r.w /= 2;
		scissor_r.y /= 2;
		scissor_r.w /= 2;
	}

	if (half_u)
	{
		tex_r.x /= 2;
		tex_r.z /= 2;
	}

	if (half_v)
	{
		tex_r.y /= 2;
		tex_r.w /= 2;
	}

	if (half_v && tex->m_from_target && m_cached_ctx.TEX0.TBW == m_cached_ctx.FRAME.FBW &&
		tex->m_from_target->m_TEX0.TBW == (m_cached_ctx.TEX0.TBW * 2) &&
		static_cast<int>(m_cached_ctx.TEX0.TBW * 64) == m_r.z &&
		m_r.w > tex->m_from_target->m_valid.w)
	{
		m_r.x *= 2;
		m_r.z *= 2;
		m_r.y /= 2;
		m_r.w /= 2;

		scissor_r.x *= 2;
		scissor_r.z *= 2;
		scissor_r.y /= 2;
		scissor_r.w /= 2;

		tex_r.x *= 2;
		tex_r.z *= 2;
		tex_r.y /= 2;
		tex_r.w /= 2;

		GL_CACHE("HW: Half width/double height shuffle detected, BW changed to %d", m_cached_ctx.FRAME.FBW);
	}

	GL_INS("HW: After adjustment: pos={%d, %d, %d, %d}, tex={%d, %d, %d, %d}, scissor={%d, %d, %d, %d}",
		m_r.x, m_r.y, m_r.z, m_r.w, tex_r.x, tex_r.y, tex_r.z, tex_r.w,
		scissor_r.x, scissor_r.y, scissor_r.z, scissor_r.w);

	m_vt.m_min.p.x = static_cast<float>(m_r.x);
	m_vt.m_min.p.y = static_cast<float>(m_r.y);
	m_vt.m_max.p.x = static_cast<float>(m_r.z);
	m_vt.m_max.p.y = static_cast<float>(m_r.w);

	m_vt.m_min.t.x = static_cast<float>(tex_r.x);
	m_vt.m_min.t.y = static_cast<float>(tex_r.y);
	m_vt.m_max.t.x = static_cast<float>(tex_r.z);
	m_vt.m_max.t.y = static_cast<float>(tex_r.w);

	m_context->scissor.in = scissor_r;

	m_index->tail = 0;
	m_vertex->head = m_vertex->tail = m_vertex->next = 0;

	GSVertex* vout = m_vertex->buff;
	u16* iout = m_index->buff;

	WriteQuad(m_r, tex_r, vout, iout);

	m_index->tail = iout - m_index->buff;
	m_vertex->head = m_vertex->tail = m_vertex->next = vout - m_vertex->buff;
}

void GSRendererHW::ConvertSpriteTextureShuffle(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	if (m_vt.m_primclass == GS_SPRITE_CLASS)
	{
		if (PRIM->FST)
		{
			ConvertSpriteTextureShuffleImpl<GS_SPRITE_CLASS, true>(rt, tex);
		}
		else
		{
			ConvertSpriteTextureShuffleImpl<GS_SPRITE_CLASS, false>(rt, tex);
		}
	}
	else if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
	{
		if (PRIM->FST)
		{
			ConvertSpriteTextureShuffleImpl<GS_TRIANGLE_CLASS, true>(rt, tex);
		}
		else
		{
			ConvertSpriteTextureShuffleImpl<GS_TRIANGLE_CLASS, false>(rt, tex);
		}
	}
	else
	{
		pxFail("Wrong primclass for texture shuffle.");
	}
}

GSVector4 GSRendererHW::RealignTargetTextureCoordinate(const GSTextureCache::Source* tex)
{
	if (GSConfig.UserHacks_HalfPixelOffset <= GSHalfPixelOffset::Normal ||
		GSConfig.UserHacks_HalfPixelOffset >= GSHalfPixelOffset::Native ||
		GetUpscaleMultiplier() == 1.0f || m_downscale_source || tex->GetScale() == 1.0f ||
		m_texture_shuffle)
	{
		return GSVector4(0.0f);
	}

	const GSVertex* v = &m_vertex->buff[0];
	const float scale = tex->GetScale();
	const bool linear = m_vt.IsRealLinear();
	const int t_position = v[0].U;
	GSVector4 half_offset(0.0f);

	if (PRIM->FST)
	{
		if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::SpecialAggressive)
		{
			if (!linear && t_position == 8)
			{
				half_offset.x = 8;
				half_offset.y = 8;
			}
			else if (linear && t_position == 16)
			{
				half_offset.x = 16;
				half_offset.y = 16;
			}
			else if (m_vt.m_min.p.x == -0.5f)
			{
				half_offset.x = 8;
				half_offset.y = 8;
			}
		}
		else
		{
			if (!linear && t_position == 8)
			{
				half_offset.x = 8 - 8 / scale;
				half_offset.y = 8 - 8 / scale;
			}
			else if (linear && t_position == 16)
			{
				half_offset.x = 16 - 16 / scale;
				half_offset.y = 16 - 16 / scale;
			}
			else if (m_vt.m_min.p.x == -0.5f)
			{
				half_offset.x = 8;
				half_offset.y = 8;
			}
		}

		GL_INS("HW: offset detected %f,%f t_pos %d (linear %d, scale %f)",
			half_offset.x, half_offset.y, t_position, linear, scale);
	}
	else if (m_vt.m_eq.q)
	{
		const float tw = static_cast<float>(1 << m_cached_ctx.TEX0.TW);
		const float th = static_cast<float>(1 << m_cached_ctx.TEX0.TH);
		const float q = v[0].RGBAQ.Q;

		half_offset.x = 0.5f * q / tw;
		half_offset.y = 0.5f * q / th;

		GL_INS("HW: ST offset detected %f,%f (linear %d, scale %f)",
			half_offset.x, half_offset.y, linear, scale);
	}

	return half_offset;
}

GSVector4i GSRendererHW::ComputeBoundingBoxRT(const GSVector2i& rtsize, float rtscale)
{
	const GSVector4 offset = IsCoverageAlphaSupported() ? GSVector4(-2.0f, 2.0f) : GSVector4(-1.0f, 1.0f);
	const GSVector4 box = m_vt.m_min.p.upld(m_vt.m_max.p) + offset.xxyy();
	return GSVector4i(box * GSVector4(rtscale)).rintersect(GSVector4i(0, 0, rtsize.x, rtsize.y));
}

GSVector4i GSRendererHW::ComputeBoundingBoxTex(const GSVector2i& texsize, const GSVector4i& coverage, const GSVector4i& region, float texscale)
{
	const GSVector4 offset = GSVector4(region.xyxy()) + (IsCoverageAlphaSupported() ? GSVector4(-2.0f, -2.0f, 2.0f, 2.0f) : GSVector4(-1.0f, -1.0f, 1.0f, 1.0f));
	const GSVector4 box = GSVector4(coverage) + offset;
	return GSVector4i(box * GSVector4(texscale)).rintersect(GSVector4i(0, 0, texsize.x, texsize.y));
}

void GSRendererHW::MergeSprite(GSTextureCache::Source* tex)
{
	if (GSConfig.UserHacks_MergePPSprite && CanUpscale() && tex && tex->m_target && (m_vt.m_primclass == GS_SPRITE_CLASS))
	{
		if (PRIM->FST && GSLocalMemory::m_psm[tex->m_TEX0.PSM].fmt < 2 && ((m_vt.m_eq.value & 0xCFFFF) == 0xCFFFF))
		{
			const GSVertex* v = &m_vertex->buff[0];
			bool is_paving = true;
			bool is_paving_h = true;
			bool is_paving_v = true;
			const int first_dpX = v[1].XYZ.X - v[0].XYZ.X;
			const int first_dpU = v[1].U - v[0].U;
			const int first_dpY = v[1].XYZ.Y - v[0].XYZ.Y;
			const int first_dpV = v[1].V - v[0].V;
			for (u32 i = 0; i < m_vertex->next; i += 2)
			{
				const int dpX = v[i + 1].XYZ.X - v[i].XYZ.X;
				const int dpU = v[i + 1].U - v[i].U;

				const int dpY = v[i + 1].XYZ.Y - v[i].XYZ.Y;
				const int dpV = v[i + 1].V - v[i].V;
				if (dpX != first_dpX || dpU != first_dpU)
				{
					is_paving_h = false;
				}

				if (dpY != first_dpY || dpV != first_dpV)
				{
					is_paving_v = false;
				}

				if (!is_paving_h && !is_paving_v)
					break;
			}
			is_paving = is_paving_h || is_paving_v;
#if 0
			const GSVector4 delta_p = m_vt.m_max.p - m_vt.m_min.p;
			const GSVector4 delta_t = m_vt.m_max.t - m_vt.m_min.t;
			const bool is_blit = PrimitiveOverlap() == PRIM_OVERLAP_NO;
			GL_INS("HW: PP SAMPLER: Dp %f %f Dt %f %f. Is blit %d, is paving %d, count %d", delta_p.x, delta_p.y, delta_t.x, delta_t.y, is_blit, is_paving, m_vertex->tail);
#endif

			if (is_paving)
			{
				u32 unique_verts = 2;
				GSVertex* s = &m_vertex->buff[0];
				if (is_paving_h)
				{
					s[0].XYZ.X = static_cast<u16>((16.0f * m_vt.m_min.p.x) + m_context->XYOFFSET.OFX);
					s[1].XYZ.X = static_cast<u16>((16.0f * m_vt.m_max.p.x) + m_context->XYOFFSET.OFX);

					s[0].U = static_cast<u16>(16.0f * m_vt.m_min.t.x);
					s[1].U = static_cast<u16>(16.0f * m_vt.m_max.t.x);
				}
				else
				{
					for (u32 i = 2; i < (m_vertex->tail & ~1); i++)
					{
						bool unique_found = false;

						for (u32 j = i & 1; j < unique_verts; i += 2)
						{
							if (s[i].XYZ.X != s[j].XYZ.X)
							{
								unique_found = true;
								break;
							}
						}
						if (unique_found)
						{
							unique_verts += 2;
							s[unique_verts - 2].XYZ.X = s[i & ~1].XYZ.X;
							s[unique_verts - 1].XYZ.X = s[i | 1].XYZ.X;
							s[unique_verts - 2].U = s[i & ~1].U;
							s[unique_verts - 1].U = s[i | 1].U;

							s[unique_verts - 2].XYZ.Y = static_cast<u16>((16.0f * m_vt.m_min.p.y) + m_context->XYOFFSET.OFY);
							s[unique_verts - 1].XYZ.Y = static_cast<u16>((16.0f * m_vt.m_max.p.y) + m_context->XYOFFSET.OFY);

							s[unique_verts - 2].V = static_cast<u16>(16.0f * m_vt.m_min.t.y);
							s[unique_verts - 1].V = static_cast<u16>(16.0f * m_vt.m_max.t.y);

							i |= 1;
						}
					}
				}

				if (is_paving_v)
				{
					s[0].XYZ.Y = static_cast<u16>((16.0f * m_vt.m_min.p.y) + m_context->XYOFFSET.OFY);
					s[1].XYZ.Y = static_cast<u16>((16.0f * m_vt.m_max.p.y) + m_context->XYOFFSET.OFY);

					s[0].V = static_cast<u16>(16.0f * m_vt.m_min.t.y);
					s[1].V = static_cast<u16>(16.0f * m_vt.m_max.t.y);
				}
				else
				{
					for (u32 i = 2; i < (m_vertex->tail & ~1); i++)
					{
						bool unique_found = false;

						for (u32 j = i & 1; j < unique_verts; i+=2)
						{
							if (s[i].XYZ.Y != s[j].XYZ.Y)
							{
								unique_found = true;
								break;
							}
						}
						if (unique_found)
						{
							unique_verts += 2;
							s[unique_verts - 2].XYZ.Y = s[i & ~1].XYZ.Y;
							s[unique_verts - 1].XYZ.Y = s[i | 1].XYZ.Y;
							s[unique_verts - 2].V = s[i & ~1].V;
							s[unique_verts - 1].V = s[i | 1].V;

							s[unique_verts - 2].XYZ.X = static_cast<u16>((16.0f * m_vt.m_min.p.x) + m_context->XYOFFSET.OFX);
							s[unique_verts - 1].XYZ.X = static_cast<u16>((16.0f * m_vt.m_max.p.x) + m_context->XYOFFSET.OFX);

							s[unique_verts - 2].U = static_cast<u16>(16.0f * m_vt.m_min.t.x);
							s[unique_verts - 1].U = static_cast<u16>(16.0f * m_vt.m_max.t.x);

							i |= 1;
						}
					}
				}

				m_vertex->head = m_vertex->tail = m_vertex->next = unique_verts;
				m_index->tail = unique_verts;
			}
		}
	}
}

float GSRendererHW::GetTextureScaleFactor()
{
	return GetUpscaleMultiplier();
}

GSVector2i GSRendererHW::GetValidSize(const GSTextureCache::Source* tex, const bool is_shuffle)
{
	int height = std::min<int>(m_context->scissor.in.w, m_r.w);

	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	const int pages = ((GSLocalMemory::GetEndBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r) + 1) - m_cached_ctx.FRAME.Block()) >> 5;
	if (m_cached_ctx.FRAME.FBW > 1 && m_r.height() == frame_psm.pgs.y && (pages % m_cached_ctx.FRAME.FBW) == 0 && m_env.CTXT[m_backed_up_ctx].FRAME.FBP == (m_cached_ctx.FRAME.FBP + pages) &&
		!IsPossibleChannelShuffle() && NextDrawMatchesShuffle())
		height = std::max<int>(m_context->scissor.in.w, height);

	int width = std::min(std::max<int>(m_cached_ctx.FRAME.FBW, 1) * 64, m_context->scissor.in.z);
	if (m_cached_ctx.FRAME.FBW == 0 && m_r.w > frame_psm.pgs.y)
	{
		GL_INS("HW: FBW=0 when drawing more than 1 page in height (PSM %s, PGS %dx%d).", GSUtil::GetPSMName(m_cached_ctx.FRAME.PSM),
			frame_psm.pgs.x, frame_psm.pgs.y);
	}

	if (m_channel_shuffle || (tex && IsPageCopy()))
	{
		const int page_x = frame_psm.pgs.x - 1;
		const int page_y = frame_psm.pgs.y - 1;
		pxAssert(tex);

		int src_width = tex->m_from_target ? tex->m_from_target->m_valid.width() : tex->GetUnscaledWidth();
		int src_height = tex->m_from_target ? tex->m_from_target->m_valid.height() : tex->GetUnscaledHeight();

		if (!tex->m_from_target && GSLocalMemory::m_psm[tex->m_TEX0.PSM].bpp == 8)
		{
			src_width >>= 1;
			src_height >>= 1;
		}

		width = (std::max(src_width, width) + page_x) & ~page_x;
		height = (std::max(src_height, height) + page_y) & ~page_y;
	}

	width = Common::AlignUpPow2(width, frame_psm.pgs.x);
	height = Common::AlignUpPow2(height, frame_psm.pgs.y);

	const bool possible_texture_shuffle = tex && m_vt.m_primclass == GS_SPRITE_CLASS && frame_psm.bpp == 16 &&
			GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp == 16 &&
			(is_shuffle || (tex->m_32_bits_fmt ||
				(m_cached_ctx.TEX0.TBP0 != m_cached_ctx.FRAME.Block() && IsOpaque() && !(m_context->TEX1.MMIN & 1) &&
					m_cached_ctx.FRAME.FBMSK && g_texture_cache->Has32BitTarget(m_cached_ctx.FRAME.Block()))));
	if (possible_texture_shuffle)
	{
		const u32 tex_width_pgs = (tex->m_target ? tex->m_from_target_TEX0.TBW : tex->m_TEX0.TBW);
		const u32 half_draw_width_pgs = ((width + (frame_psm.pgs.x - 1)) / frame_psm.pgs.x) >> 1;

		if (tex_width_pgs == half_draw_width_pgs)
		{
			GL_CACHE("HW: Halving width due to texture shuffle with double width, %dx%d -> %dx%d", width, height, width / 2, height);
			const int src_width = tex ? (tex->m_from_target ? tex->m_from_target->m_valid.width() : tex->GetUnscaledWidth()) : (width / 2);
			width = std::min(width / 2, src_width);
		}
		else
		{
			GL_CACHE("HW: Halving height due to texture shuffle, %dx%d -> %dx%d", width, height, width, height / 2);
			const int src_height = tex ? (tex->m_from_target ? tex->m_from_target->m_valid.height() : tex->GetUnscaledHeight()) : (height / 2);
			height = std::min(height / 2, src_height);
		}
	}

	constexpr int valid_max_size = 2047;
	if ((width > valid_max_size) || (height > valid_max_size))
	{
		DevCon.Warning("Warning: GetValidSize out of bounds, X:%d Y:%d", width, height);
		width = std::min(width, valid_max_size);
		height = std::min(height, valid_max_size);
	}

	return GSVector2i(width, height);
}

GSVector2i GSRendererHW::GetTargetSize(const GSTextureCache::Source* tex, const bool can_expand, const bool is_shuffle)
{
	const GSVector2i valid_size = GetValidSize(tex, is_shuffle);

	return g_texture_cache->GetTargetSize(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, valid_size.x, valid_size.y, can_expand);
}

bool GSRendererHW::NextDrawColClip() const
{
	const int get_next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : m_backed_up_ctx;
	const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];

	if (m_state_flush_reason != GSFlushReason::CONTEXTCHANGE || m_env.COLCLAMP.CLAMP != 0 || m_env.PRIM.ABE == 0 ||
		(m_context->FRAME.U64 ^ next_ctx.FRAME.U64) != 0 || (m_env.PRIM.TME && next_ctx.TEX0.TBP0 == m_context->FRAME.Block()))
	{
		return false;
	}

	return true;
}

bool GSRendererHW::IsPossibleChannelShuffle() const
{
	if (!PRIM->TME || m_cached_ctx.TEX0.PSM != PSMT8 ||
		m_vt.m_primclass != GS_SPRITE_CLASS ||
		(m_vertex->tail <= 2 && (((m_vt.m_max.p - m_vt.m_min.p) <= GSVector4(8.0f)).mask() & 0x3) == 0x3))
	{
		return false;
	}

	const int mask = (((m_vt.m_max.p - m_vt.m_min.p) <= GSVector4(64.0f)).mask() & 0x3);
	if (mask == 0x3)
	{
		const GSVertex* v = &m_vertex->buff[0];

		const int draw_width = std::abs(v[1].XYZ.X - v[0].XYZ.X) >> 4;
		const int draw_height = std::abs(v[1].XYZ.Y - v[0].XYZ.Y) >> 4;

		const bool mask_clamp = (m_cached_ctx.CLAMP.WMS | m_cached_ctx.CLAMP.WMT) & 0x2;

		const bool draw_match = (draw_height == 2) || (draw_width == 8);

		if (draw_match || mask_clamp)
			return true;
		else
			return false;
	}
	else if (mask != 0x1)
		return false;

	if (m_cached_ctx.TEX0.TBW == (m_cached_ctx.FRAME.FBW * 2) &&
		GSLocalMemory::IsPageAligned(m_cached_ctx.FRAME.PSM, GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p))))
	{
		const GSVertex* v = &m_vertex->buff[0];

		const int draw_width = std::abs(v[1].XYZ.X - v[0].XYZ.X) >> 4;
		const int draw_height = std::abs(v[1].XYZ.Y - v[0].XYZ.Y) >> 4;

		const bool mask_clamp = (m_cached_ctx.CLAMP.WMS | m_cached_ctx.CLAMP.WMT) & 0x2;
		const bool draw_match = (draw_height == 2) || (draw_width == 8);

		if (draw_match || mask_clamp)
			return true;
		else
			return false;
	}

	return false;
}

bool GSRendererHW::IsPageCopy() const
{
	if (!PRIM->TME)
		return false;

	const int get_next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : m_backed_up_ctx;
	const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];

	if (next_ctx.TEX0.TBP0 != (m_cached_ctx.TEX0.TBP0 + 0x20))
		return false;

	if (next_ctx.FRAME.FBP != (m_cached_ctx.FRAME.FBP + 0x1))
		return false;

	if (!NextDrawMatchesShuffle())
		return false;

	return true;
}

bool GSRendererHW::NextDrawMatchesShuffle() const
{
	const int get_next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : m_backed_up_ctx;
	const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];
	if (((m_context->TEX0.U64 ^ next_ctx.TEX0.U64) & (~0x3FFF)) != 0 ||
		m_context->TEX1.U64 != next_ctx.TEX1.U64 ||
		m_context->CLAMP.U64 != next_ctx.CLAMP.U64 ||
		m_context->TEST.U64 != next_ctx.TEST.U64 ||
		((m_context->FRAME.U64 ^ next_ctx.FRAME.U64) & (~0x1FF)) != 0 ||
		m_context->ZBUF.ZMSK != next_ctx.ZBUF.ZMSK)
	{
		return false;
	}

	return true;
}

bool GSRendererHW::IsSplitTextureShuffle(GIFRegTEX0& rt_TEX0, GSVector4i& valid_area)
{
	if (m_dirty_gs_regs == 0)
		return false;

	if (!NextDrawMatchesShuffle())
		return false;

	if (m_vertex->buff[m_index->buff[0]].U != m_v.U)
		return false;

	const GSVector4i pos_rc = GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p + GSVector4::cxpr(0.5f)));
	const GSVector4i tex_rc = GSVector4i(m_vt.m_min.t.upld(m_vt.m_max.t));

	if (std::abs(pos_rc.width() - tex_rc.width()) > 8 || pos_rc.height() != tex_rc.height())
		return false;

	GSVector4i aligned_rc = pos_rc.min_i32(tex_rc).blend32<12>(pos_rc.max_i32(tex_rc));

	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	const GSDrawingContext& next_ctx = m_env.CTXT[m_backed_up_ctx];

	const bool in_rt_per_page = GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets && (pos_rc.width() <= frame_psm.pgs.x && pos_rc.height() <= frame_psm.pgs.y);

	if ((aligned_rc.x & 7) != 0 || aligned_rc.x > 8 || (aligned_rc.z & 7) != 0 ||
		aligned_rc.y != 0 || (aligned_rc.w & (frame_psm.pgs.y - 1)) != 0 || in_rt_per_page)
	{
		return false;
	}

	aligned_rc = aligned_rc.rintersect(m_context->scissor.in);

	const u32 pages_high = static_cast<u32>(aligned_rc.height()) / frame_psm.pgs.y;
	const u32 num_pages = m_context->FRAME.FBW * pages_high;
	const u32 rt_half = (((valid_area.height() / GSLocalMemory::m_psm[rt_TEX0.PSM].pgs.y) / 2) * rt_TEX0.TBW) + (rt_TEX0.TBP0 >> 5);
	const u32 expected_next_FBP = (m_cached_ctx.FRAME.FBP + m_split_texture_shuffle_pages) + num_pages;
	const u32 potential_expected_next_FBP = m_cached_ctx.FRAME.FBP + ((m_context->FRAME.FBW * 64) / aligned_rc.width());
	const u32 expected_next_TBP0 = (m_cached_ctx.TEX0.TBP0 + (m_split_texture_shuffle_pages + num_pages) * GS_BLOCKS_PER_PAGE);
	const u32 potential_expected_next_TBP0 = m_cached_ctx.TEX0.TBP0 + (GS_BLOCKS_PER_PAGE * ((m_context->TEX0.TBW * 64) / aligned_rc.width()));
	GL_CACHE("HW: IsSplitTextureShuffle: Draw covers %ux%u pages, next FRAME %x TEX %x",
		static_cast<u32>(aligned_rc.width()) / frame_psm.pgs.x, pages_high, expected_next_FBP * GS_BLOCKS_PER_PAGE,
		expected_next_TBP0);

	if (next_ctx.TEX0.TBP0 != expected_next_TBP0 && next_ctx.TEX0.TBP0 != potential_expected_next_TBP0 && next_ctx.TEX0.TBP0 != (rt_half << 5))
	{
		GL_CACHE("HW: IsSplitTextureShuffle: Mismatch on TBP0, expecting %x, got %x", expected_next_TBP0, next_ctx.TEX0.TBP0);
		return false;
	}

	if (next_ctx.FRAME.FBP != expected_next_FBP && next_ctx.FRAME.FBP != m_cached_ctx.FRAME.FBP && next_ctx.FRAME.FBP != potential_expected_next_FBP && next_ctx.FRAME.FBP != rt_half)
	{
		GL_CACHE("HW: IsSplitTextureShuffle: Mismatch on FBP, expecting %x, got %x", expected_next_FBP * GS_BLOCKS_PER_PAGE,
			next_ctx.FRAME.FBP * GS_BLOCKS_PER_PAGE);
		return false;
	}

	GL_CACHE("HW: IsSplitTextureShuffle: Match, buffering and skipping draw.");

	if (m_split_texture_shuffle_pages == 0)
	{
		m_split_texture_shuffle_start_FBP = m_cached_ctx.FRAME.FBP;
		m_split_texture_shuffle_start_TBP = m_cached_ctx.TEX0.TBP0;

		if (m_cached_ctx.FRAME.FBW == 1)
			m_split_texture_shuffle_fbw = rt_TEX0.TBW;
		else
			m_split_texture_shuffle_fbw = m_cached_ctx.FRAME.FBW;
	}

	u32 vertical_pages = pages_high;
	u32 total_pages = num_pages;

	if (next_ctx.FRAME.FBP == rt_half && num_pages > (rt_half - (rt_TEX0.TBP0 >> 5)))
	{
		vertical_pages = (valid_area.height() / GSLocalMemory::m_psm[rt_TEX0.PSM].pgs.y) / 2;
		total_pages = vertical_pages * rt_TEX0.TBW;
	}

	if ((m_split_texture_shuffle_pages % m_split_texture_shuffle_fbw) == 0)
		m_split_texture_shuffle_pages_high += vertical_pages;

	m_split_texture_shuffle_pages += total_pages;
	return true;
}

GSVector4i GSRendererHW::GetSplitTextureShuffleDrawRect() const
{
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	GSVector4i r = GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p + GSVector4::cxpr(0.5f))).rintersect(m_context->scissor.in);

	if (m_context->FRAME.FBP != m_split_texture_shuffle_start_FBP)
	{
		const int pages_high = (r.height() + frame_psm.pgs.y - 1) / frame_psm.pgs.y;
		r.w = (m_split_texture_shuffle_pages_high + pages_high) * frame_psm.pgs.y;
	}

	return r.insert64<0>(0).ralign<Align_Outside>(frame_psm.pgs);
}

void GSRendererHW::FixSplitTextureShuffleState()
{
	if (m_split_texture_shuffle_pages == 0)
		return;

	const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM];
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];

	const GSVector4i new_r = GetSplitTextureShuffleDrawRect();
	GL_CACHE(
		"Split texture shuffle: FBP %x -> %x, TBP0 %x -> %x, draw %d,%d => %d,%d -> %d,%d => %d,%d",
		m_cached_ctx.FRAME.Block(), m_split_texture_shuffle_start_FBP * GS_BLOCKS_PER_PAGE,
		m_cached_ctx.TEX0.TBP0, m_split_texture_shuffle_start_TBP,
		m_r.x, m_r.y, m_r.z, m_r.w,
		new_r.x, new_r.y, new_r.z, new_r.w);
	m_r = new_r;

	m_context->scissor.in = new_r;

	m_cached_ctx.TEX0.TBP0 = m_split_texture_shuffle_start_TBP;

	SetNewFRAME(m_split_texture_shuffle_start_FBP << 5, m_context->FRAME.FBW, m_cached_ctx.FRAME.PSM);

	if (m_split_texture_shuffle_pages > 1 && !NextDrawMatchesShuffle())
	{
		if (m_context->FRAME.FBW != m_split_texture_shuffle_fbw && m_cached_ctx.TEX0.TBW == 1)
		{
			if (m_context->FRAME.FBW == 1 && m_split_texture_shuffle_fbw != m_context->FRAME.FBW)
			{
				m_r.x = 0;
				m_r.z = m_split_texture_shuffle_fbw * frame_psm.pgs.x;
				m_r.y = 0;
				m_r.w = std::min(1024U, m_split_texture_shuffle_pages_high * frame_psm.pgs.y);

				m_context->scissor.in = m_r;

				SetNewFRAME(m_split_texture_shuffle_start_FBP << 5, m_split_texture_shuffle_fbw, m_cached_ctx.FRAME.PSM);
			}

			const int pages = m_split_texture_shuffle_pages + 1;
			const int width = m_split_texture_shuffle_fbw;
			const int height = (pages >= width) ? (pages / width) : 1;
			m_cached_ctx.TEX0.TW = std::ceil(std::log2(std::min(1024, width * tex_psm.pgs.x)));
			m_cached_ctx.TEX0.TH = std::ceil(std::log2(std::min(1024, height * tex_psm.pgs.y)));
			m_cached_ctx.TEX0.TBW = m_split_texture_shuffle_fbw;
		}

		m_vt.m_min.p.x = m_r.x;
		m_vt.m_min.p.y = m_r.y;
		m_vt.m_min.t.x = m_r.x;
		m_vt.m_min.t.y = m_r.y;
		m_vt.m_max.p.x = m_r.z;
		m_vt.m_max.p.y = m_r.w;
		m_vt.m_max.t.x = m_r.z;
		m_vt.m_max.t.y = m_r.w;
	}
}

u32 GSRendererHW::Convert32BitTo16BitMask(u32 m)
{
	return ((m >> 3) & 0x1F) | ((m >> 6) & 0x3E0) | ((m >> 9) & 0x7C00) | ((m >> 16) & 0x8000);
}

u32 GSRendererHW::GetEffectiveTextureShuffleFbmsk() const
{
	const u32 fbmsk16 = Convert32BitTo16BitMask(m_cached_ctx.FRAME.FBMSK);
	return fbmsk16 | (fbmsk16 << 16);
}

GSVector4i GSRendererHW::GetDrawRectForPages(u32 bw, u32 psm, u32 num_pages)
{
	const GSVector2i& pgs = GSLocalMemory::m_psm[psm].pgs;
	const GSVector2i size = GSVector2i(static_cast<int>(bw) * pgs.x, static_cast<int>(num_pages / std::max(1U, bw)) * pgs.y);
	return GSVector4i::loadh(size);
}

bool GSRendererHW::IsSinglePageDraw() const
{
	const GSVector2i& frame_pgs = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;

	if (m_r.width() <= frame_pgs.x && m_r.height() <= frame_pgs.y)
		return true;

	return false;
}

bool GSRendererHW::TryToResolveSinglePageFramebuffer(GIFRegFRAME& FRAME, bool only_next_draw)
{
	const u32 start_bp = FRAME.Block();
	u32 new_bw = FRAME.FBW;
	u32 new_psm = FRAME.PSM;
	pxAssert(new_bw <= 1);

	if (m_backed_up_ctx >= 0)
	{
		const GSDrawingContext& next_ctx = m_env.CTXT[m_backed_up_ctx];
		if (next_ctx.FRAME.FBW != new_bw)
		{
			if (start_bp == next_ctx.FRAME.Block())
			{
				GL_INS("HW: TryToResolveSinglePageWidth(): Next FBP is split clear, using FBW of %u", next_ctx.FRAME.FBW);
				new_bw = next_ctx.FRAME.FBW;
				new_psm = next_ctx.FRAME.PSM;
			}
			else if (start_bp == next_ctx.ZBUF.Block())
			{
				GL_INS("HW: TryToResolveSinglePageWidth(): Next ZBP is split clear, using FBW of %u", next_ctx.FRAME.FBW);
				new_bw = next_ctx.FRAME.FBW;
			}
		}

		if (new_bw <= 1 && next_ctx.TEX0.TBP0 == start_bp && new_bw != next_ctx.TEX0.TBW)
		{
			GL_INS("HW: TryToResolveSinglePageWidth(): Next texture is using split clear, using FBW of %u", next_ctx.TEX0.TBW);
			new_bw = next_ctx.TEX0.TBW;
			new_psm = next_ctx.TEX0.PSM;
		}
	}

	if (!only_next_draw)
	{
		if (new_bw <= 1)
		{
			GSTextureCache::Target* tgt = g_texture_cache->GetTargetWithSharedBits(start_bp, new_psm);
			if (!tgt)
			{
				tgt = g_texture_cache->GetTargetWithSharedBits(start_bp, new_psm ^ 0x30);
			}
			if (tgt && ((start_bp + (m_split_clear_pages * GS_BLOCKS_PER_PAGE)) - 1) <= tgt->m_end_block)
			{
				GL_INS("HW: TryToResolveSinglePageWidth(): Using FBW of %u and PSM %s from existing target",
					tgt->m_TEX0.PSM, GSUtil::GetPSMName(tgt->m_TEX0.PSM));
				new_bw = tgt->m_TEX0.TBW;
				new_psm = tgt->m_TEX0.PSM;
			}
		}

		if (new_bw <= 1)
		{
			const bool double_width =
				GSLocalMemory::m_psm[new_psm].bpp == 32 && PCRTCDisplays.GetFramebufferBitDepth() == 16;
			const GSVector2i fb_size = PCRTCDisplays.GetFramebufferSize(-1);
			u32 width =
				std::ceil(static_cast<float>(m_split_clear_pages * GSLocalMemory::m_psm[new_psm].pgs.y) / fb_size.y) *
				64;
			width = std::max((width * (double_width ? 2 : 1)), static_cast<u32>(fb_size.x));
			new_bw = (width + 63) / 64;
			GL_INS("HW: TryToResolveSinglePageWidth(): Fallback guess target FBW of %u", new_bw);
		}
	}

	if (new_bw <= 1)
		return false;

	FRAME.FBW = new_bw;
	FRAME.PSM = new_psm;
	return true;
}

bool GSRendererHW::IsSplitClearActive() const
{
	return (m_split_clear_pages != 0);
}

bool GSRendererHW::IsStartingSplitClear()
{
	if (m_vt.m_eq.rgba != 0xFFFF || (!m_cached_ctx.ZBUF.ZMSK && !m_vt.m_eq.z) || m_primitive_covers_without_gaps != NoGapsType::FullCover)
		return false;

	if (m_context->FRAME.FBW > 1 || m_r.height() < 1024)
		return false;

	u32 pages_covered;
	if (!CheckNextDrawForSplitClear(m_r, &pages_covered))
		return false;

	m_split_clear_start = m_cached_ctx.FRAME;
	m_split_clear_start_Z = m_cached_ctx.ZBUF;
	m_split_clear_pages = pages_covered;
	m_split_clear_color = GetConstantDirectWriteMemClearColor();

	GL_INS("HW: Starting split clear at FBP %x FBW %u PSM %s with %dx%d rect covering %u pages",
		m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, GSUtil::GetPSMName(m_cached_ctx.FRAME.PSM),
		m_r.width(), m_r.height(), pages_covered);

	if (IsDiscardingDstColor())
	{
		const u32 bp = m_cached_ctx.FRAME.Block();
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, bp, m_cached_ctx.FRAME.PSM);
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, bp, m_cached_ctx.FRAME.PSM);
	}

	return true;
}

bool GSRendererHW::ContinueSplitClear()
{
	if (!IsConstantDirectWriteMemClear())
		return false;

	if (m_vt.m_eq.rgba != 0xFFFF || (!m_cached_ctx.ZBUF.ZMSK && !m_vt.m_eq.z) || m_primitive_covers_without_gaps != NoGapsType::FullCover)
		return false;

	if (IsDiscardingDstColor())
	{
		const u32 bp = m_cached_ctx.FRAME.Block();
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, bp, m_cached_ctx.FRAME.PSM);
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, bp, m_cached_ctx.FRAME.PSM);
	}

	u32 pages_covered;
	const bool skip = CheckNextDrawForSplitClear(m_r, &pages_covered);

	m_split_clear_pages += pages_covered;
	return skip;
}

bool GSRendererHW::CheckNextDrawForSplitClear(const GSVector4i& r, u32* pages_covered_by_this_draw) const
{
	const u32 end_block = GSLocalMemory::GetEndBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, r);
	if (pages_covered_by_this_draw)
	{
		if (end_block < m_cached_ctx.FRAME.Block())
			*pages_covered_by_this_draw = (((GS_MAX_BLOCKS - end_block) + m_cached_ctx.FRAME.Block()) + (GS_BLOCKS_PER_PAGE)) / GS_BLOCKS_PER_PAGE;
		else
			*pages_covered_by_this_draw = ((end_block - m_cached_ctx.FRAME.Block()) + (GS_BLOCKS_PER_PAGE)) / GS_BLOCKS_PER_PAGE;
	}

	if (m_backed_up_ctx < 0 || (m_dirty_gs_regs & (1u << DIRTY_REG_FRAME)) == 0)
		return false;

	if (r.width() != m_cached_ctx.FRAME.FBW * 64)
		return false;

	const GSDrawingContext& next_ctx = m_env.CTXT[m_backed_up_ctx];
	if (next_ctx.FRAME.Block() != ((end_block + 1) % GS_MAX_BLOCKS) ||
		m_context->TEX0.U64 != next_ctx.TEX0.U64 ||
		m_context->TEX1.U64 != next_ctx.TEX1.U64 || m_context->CLAMP.U64 != next_ctx.CLAMP.U64 ||
		m_context->TEST.U64 != next_ctx.TEST.U64 || ((m_context->FRAME.U64 ^ next_ctx.FRAME.U64) & (~0x1FF)) != 0 ||
		((m_context->ZBUF.U64 ^ next_ctx.ZBUF.U64) & (~0x1FF)) != 0)
	{
		return false;
	}

	if (!m_cached_ctx.ZBUF.ZMSK && m_cached_ctx.FRAME.FBP != m_cached_ctx.ZBUF.ZBP)
	{
		const u32 end_z_block = GSLocalMemory::GetEndBlockAddress(
			m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM, r);
		if (next_ctx.ZBUF.Block() != ((end_z_block + 1) % GS_MAX_BLOCKS))
			return false;
	}

	return true;
}

void GSRendererHW::FinishSplitClear()
{
	GL_INS("HW: FinishSplitClear(): Start %x FBW %u PSM %s, %u pages, %08X color", m_split_clear_start.Block(),
		m_split_clear_start.FBW, GSUtil::GetPSMName(m_split_clear_start.PSM), m_split_clear_pages, m_split_clear_color);

	if (m_split_clear_start.FBW <= 1 && m_split_clear_pages >= 16)
		TryToResolveSinglePageFramebuffer(m_split_clear_start, false);

	SetNewFRAME(m_split_clear_start.Block(), m_split_clear_start.FBW, m_split_clear_start.PSM);
	SetNewZBUF(m_split_clear_start_Z.Block(), m_split_clear_start_Z.PSM);
	ReplaceVerticesWithSprite(
		GetDrawRectForPages(m_split_clear_start.FBW, m_split_clear_start.PSM, m_split_clear_pages), GSVector2i(1, 1));
	GL_INS("HW: FinishSplitClear(): New draw rect is (%d,%d=>%d,%d) with FBW %u and PSM %s", m_r.x, m_r.y, m_r.z, m_r.w,
		m_split_clear_start.FBW, GSUtil::GetPSMName(m_split_clear_start.PSM));
	m_split_clear_start.U64 = 0;
	m_split_clear_start_Z.U64 = 0;
	m_split_clear_pages = 0;
	m_split_clear_color = 0;
}

bool GSRendererHW::NeedsBlending()
{
	const u32 temp_fbmask = (GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp == 16) ? 0x00F8F8F8 : 0x00FFFFFF;
	const bool FBMASK_skip = (m_cached_ctx.FRAME.FBMSK & temp_fbmask) == temp_fbmask;
	return PRIM->ABE && !FBMASK_skip;
}

bool GSRendererHW::IsRTWritten()
{
	const GIFRegTEST TEST = m_cached_ctx.TEST;
	const bool only_z_written = (TEST.ATE && TEST.ATST == ATST_NEVER && TEST.AFAIL == AFAIL_ZB_ONLY);
	if (only_z_written)
		return false;

	const u32 written_bits = (~m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk);
	const GIFRegALPHA ALPHA = m_context->ALPHA;
	return (
	        (written_bits & 0xFF000000u) != 0) ||
	       (
	        ((written_bits & 0x00FFFFFFu) != 0) &&
	        (!PRIM->ABE ||
	         ALPHA.D != 1 ||
	         (ALPHA.A != ALPHA.B &&
	          (ALPHA.C == 1 ||
	           (ALPHA.C == 2 && ALPHA.FIX != 0) ||
	           (ALPHA.C == 0 && GetAlphaMinMax().max != 0)))));
}

bool GSRendererHW::IsDepthAlwaysPassing()
{
	const u32 max_z = (0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8));
	const int check_index = m_vt.m_primclass == GS_SPRITE_CLASS ? 1 : 0;
	return (!m_cached_ctx.TEST.ZTE || m_cached_ctx.TEST.ZTST <= ZTST_ALWAYS) ||
	       (m_cached_ctx.TEST.ZTST == ZTST_GEQUAL && m_vt.m_eq.z && std::min(m_vertex->buff[check_index].XYZ.Z, max_z) == max_z);
}

bool GSRendererHW::IsUsingCsInBlend()
{
	const GIFRegALPHA ALPHA = m_context->ALPHA;
	const bool blend_zero = (ALPHA.A == ALPHA.B || (ALPHA.C == 2 && ALPHA.FIX == 0) || (ALPHA.C == 0 && GetAlphaMinMax().max == 0));
	return (NeedsBlending() && ((ALPHA.IsUsingCs() && !blend_zero) || m_context->ALPHA.D == 0));
}

bool GSRendererHW::IsUsingAsInBlend()
{
	return (NeedsBlending() && m_context->ALPHA.IsUsingAs() && GetAlphaMinMax().max != 0);
}
bool GSRendererHW::ChannelsSharedTEX0FRAME()
{
	if (!m_cached_ctx.TEST.DATE && !IsRTWritten())
		return false;

	return GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM, m_cached_ctx.FRAME.FBMSK) & GSUtil::GetChannelMask(m_cached_ctx.TEX0.PSM);
}

bool GSRendererHW::IsTBPFrameOrZ(u32 tbp, bool frame_only)
{
	const bool is_frame = (m_cached_ctx.FRAME.Block() == tbp) && (GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM) & GSUtil::GetChannelMask(m_cached_ctx.TEX0.PSM));
	const bool is_z = (m_cached_ctx.ZBUF.Block() == tbp) && (GSUtil::GetChannelMask(m_cached_ctx.ZBUF.PSM) & GSUtil::GetChannelMask(m_cached_ctx.TEX0.PSM));
	if (!is_frame && !is_z)
		return false;

	const u32 fm = m_cached_ctx.FRAME.FBMSK;
	const u32 zm = m_cached_ctx.ZBUF.ZMSK || m_cached_ctx.TEST.ZTE == 0 ? 0xffffffff : 0;
	const u32 fm_mask = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;

	const u32 max_z = (0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8));
	const bool no_rt = (!m_cached_ctx.TEST.DATE && !IsRTWritten());
	const bool no_ds = (
	                       (zm != 0 && m_cached_ctx.TEST.ZTST <= ZTST_ALWAYS) ||
	                       (zm != 0 && m_cached_ctx.TEST.ZTST == ZTST_GEQUAL && m_vt.m_eq.z && std::min(m_vertex->buff[0].XYZ.Z, max_z) == max_z) ||
	                       (!no_rt && m_cached_ctx.FRAME.FBP == m_cached_ctx.ZBUF.ZBP && !PRIM->TME && zm == 0 && (fm & fm_mask) == 0 && m_cached_ctx.TEST.ZTE)) ||
	                   (no_rt && zm != 0);

	return (is_frame && !no_rt) || (is_z && !no_ds && !frame_only);
}

void GSRendererHW::HandleManualDeswizzle()
{
	if (!m_vt.m_eq.z)
		return;

	GSVertex* v = &m_vertex->buff[0];

	const GSVector2i page_quadrant = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs / 2;

	if (PRIM->FST)
	{
		for (u32 i = 0; i < m_index->tail; i += 2)
		{
			const u32 index_first = m_index->buff[i];
			const u32 index_last = m_index->buff[i + 1];

			if ((abs((v[index_last].U) - (v[index_first].U)) >> 4) != page_quadrant.x || (abs((v[index_last].V) - (v[index_first].V)) >> 4) != page_quadrant.y)
				return;
		}
	}
	else
	{
		for (u32 i = 0; i < m_index->tail; i += 2)
		{
			const u32 index_first = m_index->buff[i];
			const u32 index_last = m_index->buff[i + 1];
			const u32 x = abs(((v[index_last].ST.S / v[index_last].RGBAQ.Q) * (1 << m_context->TEX0.TW)) - ((v[index_first].ST.S / v[index_first].RGBAQ.Q) * (1 << m_context->TEX0.TW)));
			const u32 y = abs(((v[index_last].ST.T / v[index_last].RGBAQ.Q) * (1 << m_context->TEX0.TH)) - ((v[index_first].ST.T / v[index_first].RGBAQ.Q) * (1 << m_context->TEX0.TH)));

			if (x != static_cast<u32>(page_quadrant.x) || y != static_cast<u32>(page_quadrant.y))
				return;
		}
	}

	GSVector4i tex_rect = GSVector4i(m_vt.m_min.t.x, m_vt.m_min.t.y, m_vt.m_max.t.x, m_vt.m_max.t.y);
	ReplaceVerticesWithSprite(m_r, tex_rect, GSVector2i(1 << m_cached_ctx.TEX0.TW, 1 << m_cached_ctx.TEX0.TH), m_context->scissor.in);
}

void GSRendererHW::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{

	GSVector4i rect = r;
	bool loop_h = false;
	bool loop_w = false;
	if (r.w > 2048)
	{
		rect.w = 2048;
		loop_h = true;
	}
	if (r.z > 2048)
	{
		rect.z = 2048;
		loop_w = true;
	}
	if (loop_h || loop_w)
	{
		g_texture_cache->InvalidateVideoMem(m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM), rect);
		if (loop_h)
		{
			rect.y = 0;
			rect.w = r.w - 2048;
		}
		if (loop_w)
		{
			rect.x = 0;
			rect.z = r.z - 2048;
		}
		g_texture_cache->InvalidateVideoMem(m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM), rect);
	}
	else
		g_texture_cache->InvalidateVideoMem(m_mem.GetOffset(BITBLTBUF.DBP, BITBLTBUF.DBW, BITBLTBUF.DPSM), r);
}

void GSRendererHW::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{

	if (clut)
		return;

	auto iter = m_draw_transfers.end();
	bool skip = false;
	while (iter != m_draw_transfers.begin())
	{
		--iter;

		if (!(iter->draw == s_n && BITBLTBUF.SBP == iter->blit.DBP && iter->blit.DPSM == BITBLTBUF.SPSM && r.eq(iter->rect)))
			continue;

		g_texture_cache->InvalidateVideoMem(m_mem.GetOffset(BITBLTBUF.SBP, BITBLTBUF.SBW, BITBLTBUF.SPSM), r);
		skip = true;
		break;
	}

	if (!skip)
	{
		const bool recursive_copy = (BITBLTBUF.SBP == BITBLTBUF.DBP) && (m_env.TRXDIR.XDIR == 2);
		g_texture_cache->InvalidateLocalMem(m_mem.GetOffset(BITBLTBUF.SBP, BITBLTBUF.SBW, BITBLTBUF.SPSM), r, recursive_copy);
	}
}

void GSRendererHW::Move()
{
	if (m_mv && m_mv(*this))
	{
		return;
	}

	if (m_env.TRXDIR.XDIR == 3)
		return;

	const int sx = m_env.TRXPOS.SSAX;
	const int sy = m_env.TRXPOS.SSAY;
	const int dx = m_env.TRXPOS.DSAX;
	const int dy = m_env.TRXPOS.DSAY;

	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;
	GL_CACHE("HW: Starting Move! 0x%x W:%d F:%s => 0x%x W:%d F:%s (DIR %d%d), sPos(%d %d) dPos(%d %d) size(%d %d) draw %lld",
		m_env.BITBLTBUF.SBP, m_env.BITBLTBUF.SBW, GSUtil::GetPSMName(m_env.BITBLTBUF.SPSM),
		m_env.BITBLTBUF.DBP, m_env.BITBLTBUF.DBW, GSUtil::GetPSMName(m_env.BITBLTBUF.DPSM),
		m_env.TRXPOS.DIRX, m_env.TRXPOS.DIRY,
		sx, sy, dx, dy, w, h, s_n);
	if (g_texture_cache->Move(m_env.BITBLTBUF.SBP, m_env.BITBLTBUF.SBW, m_env.BITBLTBUF.SPSM, sx, sy,
			m_env.BITBLTBUF.DBP, m_env.BITBLTBUF.DBW, m_env.BITBLTBUF.DPSM, dx, dy, w, h))
	{
		m_env.TRXDIR.XDIR = 3;
		return;
	}

	GSRenderer::Move();
}

u16 GSRendererHW::Interpolate_UV(float alpha, int t0, int t1)
{
	const float t = (1.0f - alpha) * t0 + alpha * t1;
	return static_cast<u16>(t) & ~0xF;
}

float GSRendererHW::alpha0(int L, int X0, int X1)
{
	const int x = (X0 + 15) & ~0xF;
	return static_cast<float>(x - X0) / static_cast<float>(L);
}

float GSRendererHW::alpha1(int L, int X0, int X1)
{
	const int x = (X1 - 1) & ~0xF;
	return static_cast<float>(x - X0) / static_cast<float>(L);
}

void GSRendererHW::SwSpriteRender()
{
	pxAssert(PRIM->PRIM == GS_TRIANGLESTRIP || PRIM->PRIM == GS_SPRITE);
	pxAssert(!PRIM->FGE);
	pxAssert(!PRIM->AA1);
	pxAssert(!PRIM->FIX);

	pxAssert(!m_draw_env->DTHE.DTHE);

	pxAssert(!m_cached_ctx.TEST.ATE);
	pxAssert(!m_cached_ctx.TEST.DATE);
	pxAssert(!m_cached_ctx.DepthRead() && !m_cached_ctx.DepthWrite());

	pxAssert(!m_cached_ctx.TEX0.CSM);

	pxAssert(!m_draw_env->PABE.PABE);

	pxAssert(!PRIM->TME || m_cached_ctx.TEX0.PSM == PSMCT32);
	pxAssert(m_cached_ctx.FRAME.PSM == PSMCT32);

	pxAssert(PRIM->PRIM == GS_SPRITE
		|| ((PRIM->IIP || m_vt.m_eq.rgba == 0xffff)
			&& m_vt.m_eq.z == 0x1
			&& (!PRIM->TME || PRIM->FST || m_vt.m_eq.q == 0x1)));

	const bool texture_mapping_enabled = PRIM->TME;

	const GSVector4i r = m_r;

#ifndef NDEBUG
	const int tw = 1 << m_cached_ctx.TEX0.TW;
	const int th = 1 << m_cached_ctx.TEX0.TH;
	const float meas_tw = m_vt.m_max.t.x - m_vt.m_min.t.x;
	const float meas_th = m_vt.m_max.t.y - m_vt.m_min.t.y;
	pxAssert(!PRIM->TME || (abs(meas_tw - r.width()) <= SSR_UV_TOLERANCE && abs(meas_th - r.height()) <= SSR_UV_TOLERANCE));
	pxAssert(!PRIM->TME || (abs(m_vt.m_min.t.x) <= SSR_UV_TOLERANCE && abs(m_vt.m_min.t.y) <= SSR_UV_TOLERANCE && abs(meas_tw - tw) <= SSR_UV_TOLERANCE && abs(meas_th - th) <= SSR_UV_TOLERANCE));
#endif

	GIFRegTRXPOS trxpos = {};

	trxpos.DSAX = r.x;
	trxpos.DSAY = r.y;
	trxpos.SSAX = static_cast<int>(m_vt.m_min.t.x / 2) * 2;
	trxpos.SSAY = static_cast<int>(m_vt.m_min.t.y / 2) * 2;

	pxAssert(r.x % 2 == 0 && r.y % 2 == 0);

	GIFRegTRXREG trxreg = {};

	trxreg.RRW = r.width();
	trxreg.RRH = r.height();

	pxAssert(r.width() % 2 == 0 && r.height() % 2 == 0);

	const int sx = trxpos.SSAX;
	int sy = trxpos.SSAY;
	const int dx = trxpos.DSAX;
	int dy = trxpos.DSAY;
	const int w = trxreg.RRW;
	const int h = trxreg.RRH;

	GL_INS("HW: SwSpriteRender: Dest 0x%x W:%d F:%s, size(%d %d)", m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, GSUtil::GetPSMName(m_cached_ctx.FRAME.PSM), w, h);

	const GSOffset spo = m_mem.GetOffset(m_context->TEX0.TBP0, m_context->TEX0.TBW, m_context->TEX0.PSM);
	const GSOffset& dpo = m_context->offset.fb;

	const bool alpha_blending_enabled = NeedsBlending();

	const GSVertex& v = m_index->tail > 0 ? m_vertex->buff[m_index->buff[m_index->tail - 1]] : GSVertex();
	const GSVector4i vc = GSVector4i(v.RGBAQ.R, v.RGBAQ.G, v.RGBAQ.B, v.RGBAQ.A)
	                          .ps32();

	const GSVector4i a_mask = GSVector4i::xff000000().u8to16();

	const bool fb_mask_enabled = m_cached_ctx.FRAME.FBMSK != 0x0;
	const GSVector4i fb_mask = GSVector4i(m_cached_ctx.FRAME.FBMSK).u8to16();

	const u8 tex0_tfx = m_cached_ctx.TEX0.TFX;
	const u8 tex0_tcc = m_cached_ctx.TEX0.TCC;
	const u8 alpha_a = m_context->ALPHA.A;
	const u8 alpha_b = m_context->ALPHA.B;
	const u8 alpha_c = m_context->ALPHA.C;
	const u8 alpha_d = m_context->ALPHA.D;
	const u8 alpha_fix = m_context->ALPHA.FIX;

	if (texture_mapping_enabled)
		g_texture_cache->InvalidateLocalMem(spo, GSVector4i(sx, sy, sx + w, sy + h));
	constexpr bool invalidate_local_mem_before_fb_read = false;
	if (invalidate_local_mem_before_fb_read && (alpha_blending_enabled || fb_mask_enabled))
		g_texture_cache->InvalidateLocalMem(dpo, m_r);

	for (int y = 0; y < h; y++, ++sy, ++dy)
	{
		u32* vm = m_mem.vm32();
		const GSOffset::PAHelper spa = spo.paMulti(sx, sy);
		const GSOffset::PAHelper dpa = dpo.paMulti(dx, dy);

		pxAssert(w % 2 == 0);

		for (int x = 0; x < w; x += 2)
		{
			u32* di = &vm[dpa.value(x)];
			pxAssert(di + 1 == &vm[dpa.value(x + 1)]);

			GSVector4i sc = {};
			if (texture_mapping_enabled)
			{
				const u32* si = &vm[spa.value(x)];
				pxAssert(si + 1 == &vm[spa.value(x + 1)]);
				sc = GSVector4i::loadl(si).u8to16();

				pxAssert(tex0_tfx == 0 || tex0_tfx == 1);
				if (tex0_tfx == 0)
					sc = sc.mul16l(vc).srl16<7>().clamp8();

				if (tex0_tcc == 0)
					sc = sc.blend(vc, a_mask);
			}
			else
				sc = vc;

			GSVector4i dc0 = {};
			GSVector4i dc = {};

			if (alpha_blending_enabled || fb_mask_enabled)
			{
				dc0 = GSVector4i::loadl(di).u8to16();
			}

			if (alpha_blending_enabled)
			{
				const GSVector4i A = alpha_a == 0 ? sc : alpha_a == 1 ? dc0 : GSVector4i::zero();
				const GSVector4i B = alpha_b == 0 ? sc : alpha_b == 1 ? dc0 : GSVector4i::zero();
				const GSVector4i C = alpha_c == 2 ? GSVector4i(alpha_fix).xxxx().ps32()
				                                  : (alpha_c == 0 ? sc : dc0).yyww()
				                                                             .srl32<16>()
				                                                             .ps32()
				                                                             .xxyy();
				const GSVector4i D = alpha_d == 0 ? sc : alpha_d == 1 ? dc0 : GSVector4i::zero();
				dc = A.sub16(B).mul16l(C).sra16<7>().add16(D);
			}
			else
				dc = sc;

			if (m_draw_env->COLCLAMP.CLAMP)
				dc = dc.clamp8();
			else
				dc = dc.sll16<8>().srl16<8>();

			pxAssert(m_context->FBA.FBA == 0);
			dc = dc.blend(sc, a_mask);

			if (fb_mask_enabled)
				dc = dc.blend(dc0, fb_mask);

			dc = dc.pu16(GSVector4i::zero());
			GSVector4i::storel(di, dc);
		}
	}

	g_texture_cache->InvalidateVideoMem(dpo, m_r);
}

bool GSRendererHW::CanUseSwSpriteRender()
{
	const GSVector4i r = m_r;
	if (r.x % 2 != 0 || r.y % 2 != 0)
		return false;
	const int w = r.width();
	const int h = r.height();
	if (w % 2 != 0 || h % 2 != 0)
		return false;
	if (w > 64 || h > 64)
		return false;
	if (PRIM->PRIM != GS_SPRITE
		&& ((PRIM->IIP && m_vt.m_eq.rgba != 0xffff)
			|| (PRIM->TME && !PRIM->FST && m_vt.m_eq.q != 0x1)
			|| m_vt.m_eq.z != 0x1))
		return false;
	if (m_vt.m_primclass != GS_TRIANGLE_CLASS && m_vt.m_primclass != GS_SPRITE_CLASS)
		return false;
	if (PRIM->PRIM != GS_TRIANGLESTRIP && PRIM->PRIM != GS_SPRITE)
		return false;
	if (m_vt.m_primclass == GS_TRIANGLE_CLASS && (PRIM->PRIM != GS_TRIANGLESTRIP || m_vertex->tail != 4))
		return false;
	if (m_vt.m_primclass == GS_SPRITE_CLASS && (PRIM->PRIM != GS_SPRITE || m_vertex->tail != 2))
		return false;
	if (m_cached_ctx.DepthRead() || m_cached_ctx.DepthWrite())
		return false;
	if (m_cached_ctx.FRAME.PSM != PSMCT32)
		return false;
	if (PRIM->TME)
	{

		if (m_cached_ctx.TEX0.PSM != PSMCT32)
			return false;
		if (IsMipMapDraw())
			return false;
		const int tw = 1 << m_cached_ctx.TEX0.TW;
		const int th = 1 << m_cached_ctx.TEX0.TH;
		const float meas_tw = m_vt.m_max.t.x - m_vt.m_min.t.x;
		const float meas_th = m_vt.m_max.t.y - m_vt.m_min.t.y;
		if (abs(m_vt.m_min.t.x) > SSR_UV_TOLERANCE ||
			abs(m_vt.m_min.t.y) > SSR_UV_TOLERANCE ||
			abs(meas_tw - tw) > SSR_UV_TOLERANCE ||
			abs(meas_th - th) > SSR_UV_TOLERANCE)
			return false;
		if (abs(meas_tw - w) > SSR_UV_TOLERANCE || abs(meas_th - h) > SSR_UV_TOLERANCE)
			return false;
	}

	return true;
}

template <bool linear>
void GSRendererHW::RoundSpriteOffset()
{
#if defined(DEBUG_V) || defined(DEBUG_U)
	bool debug = linear;
#endif
	const u32 count = m_vertex->next;
	GSVertex* v = &m_vertex->buff[0];

	for (u32 i = 0; i < count; i += 2)
	{

		const int ox = m_context->XYOFFSET.OFX;
		const int X0 = v[i].XYZ.X - ox;
		const int X1 = v[i + 1].XYZ.X - ox;
		const int Lx = (v[i + 1].XYZ.X - v[i].XYZ.X);
		const float ax0 = alpha0(Lx, X0, X1);
		const float ax1 = alpha1(Lx, X0, X1);
		const u16 tx0 = Interpolate_UV(ax0, v[i].U, v[i + 1].U);
		const u16 tx1 = Interpolate_UV(ax1, v[i].U, v[i + 1].U);
#ifdef DEBUG_U
		if (debug)
		{
			fprintf(stderr, "HW: u0:%d and u1:%d\n", v[i].U, v[i + 1].U);
			fprintf(stderr, "HW: a0:%f and a1:%f\n", ax0, ax1);
			fprintf(stderr, "HW: :%d and t1:%d\n", tx0, tx1);
		}
#endif

		const int oy = m_context->XYOFFSET.OFY;
		const int Y0 = v[i].XYZ.Y - oy;
		const int Y1 = v[i + 1].XYZ.Y - oy;
		const int Ly = (v[i + 1].XYZ.Y - v[i].XYZ.Y);
		const float ay0 = alpha0(Ly, Y0, Y1);
		const float ay1 = alpha1(Ly, Y0, Y1);
		const u16 ty0 = Interpolate_UV(ay0, v[i].V, v[i + 1].V);
		const u16 ty1 = Interpolate_UV(ay1, v[i].V, v[i + 1].V);
#ifdef DEBUG_V
		if (debug)
		{
			fprintf(stderr, "HW: v0:%d and v1:%d\n", v[i].V, v[i + 1].V);
			fprintf(stderr, "HW: a0:%f and a1:%f\n", ay0, ay1);
			fprintf(stderr, "HW: t0:%d and t1:%d\n", ty0, ty1);
		}
#endif

#ifdef DEBUG_U
		if (debug)
			fprintf(stderr, "HW: GREP_BEFORE %d => %d\n", v[i].U, v[i + 1].U);
#endif
#ifdef DEBUG_V
		if (debug)
			fprintf(stderr, "HW: GREP_BEFORE %d => %d\n", v[i].V, v[i + 1].V);
#endif

#if 1
		if (linear)
		{
			const int Lu = v[i + 1].U - v[i].U;
			if ((Lu > 0) && (Lu <= (Lx + 32)))
			{
				v[i + 1].U -= 8;
			}
		}
		else
		{
			if (tx0 <= tx1)
			{
				v[i].U = tx0;
				v[i + 1].U = tx1 + 16;
			}
			else
			{
				v[i].U = tx0 + 15;
				v[i + 1].U = tx1;
			}
		}
#endif
#if 1
		if (linear)
		{
			const int Lv = v[i + 1].V - v[i].V;
			if ((Lv > 0) && (Lv <= (Ly + 32)))
			{
				v[i + 1].V -= 8;
			}
		}
		else
		{
			if (ty0 <= ty1)
			{
				v[i].V = ty0;
				v[i + 1].V = ty1 + 16;
			}
			else
			{
				v[i].V = ty0 + 15;
				v[i + 1].V = ty1;
			}
		}
#endif

#ifdef DEBUG_U
		if (debug)
			fprintf(stderr, "HW: GREP_AFTER %d => %d\n\n", v[i].U, v[i + 1].U);
#endif
#ifdef DEBUG_V
		if (debug)
			fprintf(stderr, "HW: GREP_AFTER %d => %d\n\n", v[i].V, v[i + 1].V);
#endif
	}
}

void GSRendererHW::Draw()
{
	static u32 num_skipped_channel_shuffle_draws = 0;

	const GSDrawingContext* context = m_context;
	m_cached_ctx.TEX0 = context->TEX0;
	m_cached_ctx.TEXA = m_draw_env->TEXA;
	m_cached_ctx.CLAMP = context->CLAMP;
	m_cached_ctx.TEST = context->TEST;
	m_cached_ctx.FRAME = context->FRAME;
	m_cached_ctx.ZBUF = context->ZBUF;

	if (IsBadFrame())
	{
		GL_INS("HW: Warning skipping a draw call (%lld)", s_n);
		return;
	}

	if (GSVector4i(m_vt.m_min.p.xyxy(m_vt.m_max.p)).eq(GSVector4i::zero()) && m_vt.m_eq.rgba == 0xffff && 
		m_vt.m_max.c.rgba32() == 0 && m_draw_env->PRIM.PRIM == GS_POINTLIST && m_env.PRIM.PRIM != GS_POINTLIST)
		return;

	if (m_channel_shuffle)
	{
		const bool is_hle_skip = m_conf.ps.urban_chaos_hle || m_conf.ps.tales_of_abyss_hle;
		const u32 max_skip = ((m_channel_shuffle_finish || !m_channel_shuffle_width) ? std::max(m_context->FRAME.FBW, 1U) : m_channel_shuffle_width) << 5;
		const bool shuffle_detect = IsPossibleChannelShuffle() && m_last_channel_shuffle_fbmsk == m_context->FRAME.FBMSK &&
		                            m_last_channel_shuffle_fbp <= m_context->FRAME.Block() && (m_last_channel_shuffle_fbp + max_skip) >= m_context->FRAME.Block() && 
									m_last_channel_shuffle_end_block > m_context->FRAME.Block() && m_last_channel_shuffle_tbp <= m_context->TEX0.TBP0
									&& (m_last_channel_shuffle_tbp + max_skip) >= m_context->TEX0.TBP0;

		const bool shuffle_detect_loose = IsPossibleChannelShuffle() && m_last_channel_shuffle_fbmsk == m_context->FRAME.FBMSK &&
		                            m_last_channel_shuffle_fbp <= m_context->FRAME.Block() &&
		                            m_last_channel_shuffle_end_block > m_context->FRAME.Block() && m_last_channel_shuffle_tbp <= m_context->TEX0.TBP0;

		m_channel_shuffle = !m_channel_shuffle_finish && ((!is_hle_skip && shuffle_detect) || (is_hle_skip && shuffle_detect_loose));

		if (m_channel_shuffle)
		{
			m_full_screen_shuffle |= !IsPageCopy() && NextDrawMatchesShuffle();
			if (!m_conf.ps.urban_chaos_hle && !m_conf.ps.tales_of_abyss_hle)
			{
				m_last_channel_shuffle_fbp = m_context->FRAME.Block();
				m_last_channel_shuffle_tbp = m_context->TEX0.TBP0;
			}

			num_skipped_channel_shuffle_draws++;
			return;
		}

		if (m_channel_shuffle_width)
		{
			if (m_last_rt)
			{
				const int width = std::max(static_cast<int>(m_last_rt->m_TEX0.TBW) * 64, 64);
				const int shuffle_height = (((num_skipped_channel_shuffle_draws + 1 + (std::max(1, (width / 64) - 1))) * 64) / width) * 32;
				const int shuffle_width = std::min((num_skipped_channel_shuffle_draws + 1) * 64, static_cast<u32>(width));
				GSVector4i valid_area = GSVector4i::loadh(GSVector2i(shuffle_width, shuffle_height));
				const int offset = (((m_last_channel_shuffle_fbp + 0x20) - m_last_rt->m_TEX0.TBP0) >> 5) - (num_skipped_channel_shuffle_draws + 1);

				if (offset)
				{
					int vertical_offset = (offset / std::max(1U, m_channel_shuffle_width)) * 32;
					valid_area.y += vertical_offset;
					valid_area.w += vertical_offset;
				}

				if (!m_full_screen_shuffle)
				{
					m_conf.scissor.w = m_conf.scissor.y + shuffle_height * m_conf.cb_ps.ScaleFactor.z;
					if (shuffle_width)
						m_conf.scissor.z = m_conf.scissor.x + (shuffle_width * m_conf.cb_ps.ScaleFactor.z);
					else
						m_conf.scissor.z = std::min(m_conf.scissor.z, static_cast<int>((m_channel_shuffle_width * 64) * m_conf.cb_ps.ScaleFactor.z));
				}

				m_last_rt->UpdateValidity(valid_area);

				g_gs_device->RenderHW(m_conf);

				if (GSConfig.DumpGSData)
				{
					if (GSConfig.ShouldDump(s_n - 1, g_perfmon.GetFrame()))
					{
						if (m_last_rt && GSConfig.SaveRT)
						{
							const u64 frame = g_perfmon.GetFrame();

							std::string s = GetDrawDumpPath("%05lld_f%05lld_rt1_%05x_(%05x)_%s.bmp", s_n - 1, frame, m_last_channel_shuffle_fbp, m_last_rt->m_TEX0.TBP0, GSUtil::GetPSMName(m_cached_ctx.FRAME.PSM));

							m_last_rt->m_texture->Save(s);
						}
					}
				}
				g_texture_cache->InvalidateTemporarySource();
				CleanupDraw(false);
			}
		}

		if (!shuffle_detect)
		{
			m_last_channel_shuffle_fbp = 0xffff;
			m_last_channel_shuffle_tbp = 0xffff;
			m_last_channel_shuffle_end_block = 0xffff;
		}
#ifdef ENABLE_OGL_DEBUG
		if (num_skipped_channel_shuffle_draws > 0)
			GL_CACHE("HW: Skipped %d channel shuffle draws ending at %lld", num_skipped_channel_shuffle_draws, s_n);
#endif
		num_skipped_channel_shuffle_draws = 0;
	}
	else
	{
		m_last_channel_shuffle_fbp = 0xffff;
		m_last_channel_shuffle_tbp = 0xffff;
		m_last_channel_shuffle_end_block = 0xffff;
	}

	m_last_rt = nullptr;
	m_channel_shuffle_width = 0;
	m_full_screen_shuffle = false;
	m_channel_shuffle_finish = false;
	m_channel_shuffle_src_valid = GSVector4i::zero();

	GL_PUSH("HW: Draw %lld (Context %u)", s_n, PRIM->CTXT);
	GL_INS("HW: FLUSH REASON: %s%s", GetFlushReasonString(m_state_flush_reason),
		(m_state_flush_reason != GSFlushReason::CONTEXTCHANGE && m_dirty_gs_regs) ? " AND POSSIBLE CONTEXT CHANGE" :
																					"");

	DetectTextureShuffle();

	if ((m_cached_ctx.FRAME.PSM & 0xF) == PSMCT24 && m_context->TEST.DATE)
	{
		GL_CACHE("HW: DATE on a 24bit format, Frame PSM %x", m_context->FRAME.PSM);
		return;
	}

	u32 fm = m_cached_ctx.FRAME.FBMSK;
	u32 zm = (m_cached_ctx.ZBUF.ZMSK || m_cached_ctx.TEST.ZTE == 0 ||
	             (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ZTST_NEVER && m_cached_ctx.TEST.AFAIL != AFAIL_ZB_ONLY)) ?
	             0xffffffffu :
	             0;
	const u32 fm_mask = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;

	const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM];
	if (PRIM->TME && tex_psm.pal > 0)
	{
		m_mem.m_clut.Read32(m_cached_ctx.TEX0, m_cached_ctx.TEXA);
		if (m_mem.m_clut.GetGPUTexture())
		{
			CalcAlphaMinMax(0, 255);
		}
	}

	m_cached_ctx.TEST.ATE = !!m_cached_ctx.TEST.ATE && !GSRenderer::TryAlphaTest(fm, zm);

	if (IsCoverageAlphaFixedOne() && m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST > 1)
	{
		const float aref = static_cast<float>(m_cached_ctx.TEST.AREF);
		const int old_ATST = m_cached_ctx.TEST.ATST;
		m_cached_ctx.TEST.ATST = 0;

		switch (old_ATST)
		{
			case ATST_LESS:
				if (128.0f < aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_LEQUAL:
				if (128.0f <= aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_EQUAL:
				if (128.0f == aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_GEQUAL:
				if (128.0f >= aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_GREATER:
				if (128.0f > aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			case ATST_NOTEQUAL:
				if (128.0f != aref)
					m_cached_ctx.TEST.ATE = false;
				break;
			default:
				break;
		}
	}

	m_cached_ctx.FRAME.FBMSK = fm;
	m_cached_ctx.ZBUF.ZMSK = zm != 0;

	bool no_rt = (!m_cached_ctx.TEST.DATE && !IsRTWritten());
	const bool all_depth_tests_pass = IsDepthAlwaysPassing();
	bool no_ds = (zm != 0 && all_depth_tests_pass) ||
	             (no_rt && zm != 0);

	if (no_ds || all_depth_tests_pass)
	{
		if (m_cached_ctx.TEST.ZTST != ZTST_ALWAYS)
			GL_CACHE("HW: Disabling Z tests because all tests will pass.");

		m_cached_ctx.TEST.ZTST = ZTST_ALWAYS;
	}

	if (no_rt && no_ds)
	{
		GL_CACHE("HW: Skipping draw with no color nor depth output.");
		return;
	}

	const bool has_colclip_texture = g_gs_device->GetColorClipTexture() != nullptr;
	if (!no_rt && has_colclip_texture && (m_conf.colclip_frame.FBP != m_cached_ctx.FRAME.FBP || (PRIM->TME && m_conf.colclip_frame.Block() == m_cached_ctx.TEX0.TBP0)))
	{
		GIFRegTEX0 FRAME;
		FRAME.TBP0 = m_conf.colclip_frame.Block();
		FRAME.TBW = m_conf.colclip_frame.FBW;
		FRAME.PSM = m_conf.colclip_frame.PSM;

		GSTextureCache::Target* old_rt = g_texture_cache->LookupDrawTarget(FRAME, GSVector2i(1, 1), GetTextureScaleFactor(), GSTextureCache::RenderTarget, true,
			fm, false, true, true, GSVector4i(0, 0, 1, 1), true, false, false);

		if (old_rt)
		{
			GL_CACHE("HW: Pre-draw resolve of colclip! Address: %x", FRAME.TBP0);
			GSTexture* colclip_texture = g_gs_device->GetColorClipTexture();
			const GSVector4 colclip_texture_dims = GSVector4(GSVector4i(colclip_texture->GetSize()).xyxy());
			g_gs_device->StretchRect(
				colclip_texture, GSVector4(m_conf.colclip_update_area) / colclip_texture_dims,
				old_rt->m_texture, GSVector4(m_conf.colclip_update_area),
				ShaderConvert::COLCLIP_RESOLVE, Nearest);

			g_gs_device->Recycle(colclip_texture);

			g_gs_device->SetColorClipTexture(nullptr);
		}
		else
			DevCon.Warning("HW: Error resolving colclip texture for pre-draw resolve");
	}

	const bool draw_sprite_tex = PRIM->TME && (m_vt.m_primclass == GS_SPRITE_CLASS);

	m_r = GSVector4i((m_vt.m_min.p.upld(m_vt.m_max.p) + GSVector4::cxpr(0.4f)).round<Round_NearestInt>());
	m_r = m_r.blend8(m_r + GSVector4i::cxpr(0, 0, 1, 1), (m_r.xyxy() == m_r.zwzw()));
	m_r_no_scissor = m_r;
	m_r = m_r.rintersect(context->scissor.in);

	if (m_r.rempty())
	{
		GL_INS("HW: Draw %lld skipped due to having an empty rect", s_n);
		return;
	}

	m_process_texture = PRIM->TME && !(NeedsBlending() && m_context->ALPHA.IsBlack() && !m_cached_ctx.TEX0.TCC) && !(no_rt && (!m_cached_ctx.TEST.ATE || m_cached_ctx.TEST.ATST <= ATST_ALWAYS));

	if (CanUseSwPrimRender(no_rt, no_ds, draw_sprite_tex && m_process_texture) && SwPrimRender(*this, true, true))
	{
		GL_CACHE("HW: Possible texture decompression, drawn with SwPrimRender() (BP %x BW %u TBP0 %x TBW %u)",
			m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBMSK, m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.TBW);
		return;
	}

	const ClearType is_possible_mem_clear = IsConstantDirectWriteMemClear();
	if (!GSConfig.UserHacks_DisableSafeFeatures && is_possible_mem_clear)
	{
		if (!DetectStripedDoubleClear(no_rt, no_ds))
			if (!DetectDoubleHalfClear(no_rt, no_ds))
				DetectRedundantBufferClear(no_rt, no_ds, fm_mask);
	}

	CalculatePrimitiveCoversWithoutGaps();

	const bool not_writing_to_all = (m_primitive_covers_without_gaps != NoGapsType::FullCover || AreAnyPixelsDiscarded() || !all_depth_tests_pass);
	bool preserve_depth =
		not_writing_to_all || (!no_ds && (!all_depth_tests_pass || !m_cached_ctx.DepthWrite() || m_cached_ctx.TEST.ATE));

	const u32 frame_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r);

	bool tex_is_rt = (m_process_texture && m_cached_ctx.TEX0.TBP0 >= m_cached_ctx.FRAME.Block() &&
		m_cached_ctx.TEX0.TBP0 < frame_end_bp);
	bool preserve_rt_rgb = (!no_rt && (!IsDiscardingDstRGB() || not_writing_to_all || tex_is_rt));
	bool preserve_rt_alpha =
		(!no_rt && (!IsDiscardingDstAlpha() || not_writing_to_all ||
					   (tex_is_rt && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp != 24)));
	bool preserve_rt_color = preserve_rt_rgb || preserve_rt_alpha;


	bool force_preload = GSConfig.PreloadFrameWithGSData;
	if (GSConfig.UserHacks_CPUCLUTRender > 0 || GSConfig.UserHacks_GPUTargetCLUTMode != GSGPUTargetCLUTMode::Disabled)
	{
		const CLUTDrawTestResult result = (GSConfig.UserHacks_CPUCLUTRender == 2) ? PossibleCLUTDrawAggressive() : PossibleCLUTDraw();
		m_mem.m_clut.ClearDrawInvalidity();
		if (result == CLUTDrawTestResult::CLUTDrawOnCPU && GSConfig.UserHacks_CPUCLUTRender > 0)
		{
			if (SwPrimRender(*this, true, true))
			{
				GL_CACHE("HW: Possible clut draw, drawn with SwPrimRender()");
				return;
			}
		}
		else if (result != CLUTDrawTestResult::NotCLUTDraw)
		{
			force_preload |= preserve_rt_color;
			if (preserve_rt_color)
				GL_INS("HW: Forcing preload due to partial/blended CLUT draw");
		}
	}

	if (!m_channel_shuffle && m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0 &&
		IsPossibleChannelShuffle())
	{
		GL_INS("HW: Possible channel shuffle effect detected");
		m_channel_shuffle = true;
		m_last_channel_shuffle_fbmsk = m_context->FRAME.FBMSK;
	}
	else if (IsSplitClearActive())
	{
		if (ContinueSplitClear())
		{
			GL_INS("HW: Skipping due to continued split clear, FBP %x FBW %u", m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW);
			return;
		}
		else
		{
			FinishSplitClear();
		}
	}

	m_using_temp_z = false;

	FixSplitTextureShuffleState();

	if (!GSConfig.UserHacks_DisableSafeFeatures && is_possible_mem_clear)
	{
		GL_INS("HW: WARNING: Possible mem clear.");

		if (IsStartingSplitClear())
		{
			CleanupDraw(false);
			return;
		}

		const int get_next_ctx = m_env.PRIM.CTXT;
		const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];

		bool height_invalid = m_r.w >= 1024;
		const GSVector2i& pgs = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;
		const bool width_change = next_ctx.FRAME.FBW > m_cached_ctx.FRAME.FBW && next_ctx.FRAME.FBP == m_cached_ctx.FRAME.FBP && next_ctx.FRAME.PSM == m_cached_ctx.FRAME.PSM;
		if (height_invalid && m_cached_ctx.FRAME.FBW <= 1 &&
			TryToResolveSinglePageFramebuffer(m_cached_ctx.FRAME, true))
		{
			ReplaceVerticesWithSprite(
				GetDrawRectForPages(m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, (m_r.w + (pgs.y - 1)) / pgs.y),
				GSVector2i(1, 1));
			height_invalid = false;
		}
		else if (width_change)
		{
			const int num_pages = m_cached_ctx.FRAME.FBW * ((m_r.w + (pgs.y - 1)) / pgs.y);
			m_cached_ctx.FRAME.FBW = next_ctx.FRAME.FBW;

			ReplaceVerticesWithSprite(
				GetDrawRectForPages(m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, num_pages),
				GSVector2i(1, 1));
		}

		const u32 vert_index = (m_vt.m_primclass == GS_TRIANGLE_CLASS) ? 2 : 1;
		u32 const_color = m_vertex->buff[m_index->buff[vert_index]].RGBAQ.U32[0];
		u32 fb_mask = m_cached_ctx.FRAME.FBMSK;

		GSTextureCache::Target* rt_tgt = g_texture_cache->GetExactTarget(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, GSTextureCache::RenderTarget, m_cached_ctx.FRAME.Block() + 1);
		const bool clear_16bit_likely = !(context->FRAME.PSM & 0x2) && ((rt_tgt && (rt_tgt->m_TEX0.PSM & 2)) || (!rt_tgt && ((static_cast<int>(context->FRAME.FBW) * 64) <= (PCRTCDisplays.GetResolution().x >> 1) || m_r.height() <= (PCRTCDisplays.GetResolution().y >> 1))));

		rt_tgt = nullptr;

		if (clear_16bit_likely && ((const_color != 0 && (const_color >> 16) == (const_color & 0xFFFF) && ((const_color >> 8) & 0xFF) != (const_color & 0xFF)) ||
												(fb_mask != 0 && (fb_mask >> 16) == (fb_mask & 0xFFFF) && ((fb_mask >> 8) & 0xFF) != (fb_mask & 0xFF))))
		{

			GL_CACHE("Clear 16bit with 32bit %lld", s_n);

			if (!(m_cached_ctx.FRAME.PSM & 2))
			{
				if (next_ctx.FRAME.FBW == (m_cached_ctx.FRAME.FBW * 2))
				{
					m_cached_ctx.FRAME.FBW *= 2;
					m_r.z *= 2;
				}
				else
				{
					m_r.w *= 2;
				}
			}

			const_color = ((const_color & 0x1F) << 3) | ((const_color & 0x3E0) << 6) | ((const_color & 0x7C00) << 9) | ((const_color & 0x8000) << 16);
			m_cached_ctx.FRAME.FBMSK = ((fb_mask & 0x1F) << 3) | ((fb_mask & 0x3E0) << 6) | ((fb_mask & 0x7C00) << 9) | ((fb_mask & 0x8000) << 16);
			m_cached_ctx.TEXA.AEM = 0;
			m_cached_ctx.TEXA.TA0 = 0;
			m_cached_ctx.TEXA.TA1 = 128;
			m_cached_ctx.FRAME.PSM = (m_cached_ctx.FRAME.PSM & 2) ? m_cached_ctx.FRAME.PSM : PSMCT16;
			m_vertex->buff[m_index->buff[1]].RGBAQ.U32[0] = const_color;
			ReplaceVerticesWithSprite(m_r, GSVector2i(m_r.width(), m_r.height()));
		}

		const bool page_aligned = (m_r.w % pgs.y) == (pgs.y - 1) || (m_r.w % pgs.y) == 0;
		const bool is_zero_color_clear = (GetConstantDirectWriteMemClearColor() == 0 && !preserve_rt_color && page_aligned);
		const bool is_zero_depth_clear = (GetConstantDirectWriteMemClearDepth() == 0 && !preserve_depth && page_aligned);
		bool gs_mem_cleared = false;
		if (is_zero_color_clear || is_zero_depth_clear || height_invalid)
		{
			u32 rt_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(
				m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r);
			const u32 ds_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(
				m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM, m_r);

			if (!no_ds && (rt_end_bp + 1) == m_cached_ctx.ZBUF.Block() && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp == GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].trbpp)
				rt_end_bp = ds_end_bp;

			GSTextureCache::Target* tgt;
			const bool overwriting_whole_rt =
				(no_rt || height_invalid ||
					(tgt = g_texture_cache->GetExactTarget(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW,
						 GSTextureCache::RenderTarget, rt_end_bp)) == nullptr ||
					m_r.rintersect(tgt->m_valid).eq(tgt->m_valid));
			const bool overwriting_whole_ds =
				(no_ds || height_invalid ||
					(tgt = g_texture_cache->GetExactTarget(m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW,
						 GSTextureCache::DepthStencil, ds_end_bp)) == nullptr ||
					m_r.rintersect(tgt->m_valid).eq(tgt->m_valid));

			if (g_texture_cache->GetTemporaryZ() != nullptr && ((m_cached_ctx.FRAME.FBMSK != 0xFFFFFFFF && m_cached_ctx.FRAME.Block() == g_texture_cache->GetTemporaryZInfo().ZBP) || (!m_cached_ctx.ZBUF.ZMSK && m_cached_ctx.ZBUF.Block() == g_texture_cache->GetTemporaryZInfo().ZBP)))
			{
				g_texture_cache->InvalidateTemporaryZ();
			}
			gs_mem_cleared |= overwriting_whole_rt && overwriting_whole_ds && (!no_rt || !no_ds);
			if (overwriting_whole_rt && overwriting_whole_ds &&
				TryGSMemClear(no_rt, preserve_rt_color, is_zero_color_clear, rt_end_bp,
					no_ds, preserve_depth, is_zero_depth_clear, ds_end_bp))
			{
				GL_INS("HW: Skipping (%d,%d=>%d,%d) draw at FBP %x/ZBP %x due to invalid height or zero clear.", m_r.x, m_r.y,
					m_r.z, m_r.w, m_cached_ctx.FRAME.Block(), m_cached_ctx.ZBUF.Block());

				if (!height_invalid)
				{
					const GSVector2i target_size = GetValidSize(nullptr);
					if (!no_rt && is_zero_color_clear)
					{
						g_texture_cache->GetTargetSize(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM,
							target_size.x, target_size.y);
					}
					if (!no_ds && is_zero_depth_clear)
					{
						g_texture_cache->GetTargetSize(m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM,
							target_size.x, target_size.y);
					}
				}

				CleanupDraw(false);
				return;
			}
		}

		if (!gs_mem_cleared)
		{
			const int get_next_ctx = m_env.PRIM.CTXT;
			const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];
			if ((!no_rt && next_ctx.FRAME.FBP != m_cached_ctx.FRAME.FBP) || (!no_ds && next_ctx.ZBUF.ZBP != m_cached_ctx.ZBUF.ZBP))
			{
				bool frame_masked = no_rt || (m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk) || !IsOpaque() || !IsRTWritten();
				const bool z_masked = no_ds || m_cached_ctx.ZBUF.ZMSK;

				if (frame_masked && m_cached_ctx.FRAME.PSM == PSMCT32 && m_cached_ctx.FRAME.FBMSK == 0xFF000000u)
				{
					frame_masked = no_rt || !IsOpaque() || !IsRTWritten();
				}

				TryGSMemClear(frame_masked, false, false, 0, z_masked, false, false, 0);
			}
		}
	}

	GIFRegTEX0 TEX0 = {};
	GSTextureCache::Source* src = nullptr;
	TextureMinMaxResult tmm;
	bool possible_shuffle = false;
	bool draw_uses_target = false;
	if (m_process_texture)
	{
		GIFRegCLAMP MIP_CLAMP = m_cached_ctx.CLAMP;
		GSVector2i hash_lod_range(0, 0);
		m_lod = GSVector2i(0, 0);

		if (IsMipMapActive())
		{
			const int interpolation = (context->TEX1.MMIN & 1) + 1;

			int k = (m_context->TEX1.K + 8) >> 4;
			int lcm = m_context->TEX1.LCM;
			const int mxl = std::min<int>(static_cast<int>(m_context->TEX1.MXL), 6);

			if (static_cast<int>(m_vt.m_lod.x) >= mxl)
			{
				k = mxl;
				lcm = 1;
			}

			if (PRIM->FST)
			{
				pxAssert(lcm == 1);

				lcm = 1;
			}

			if (lcm == 1)
			{
				m_lod.x = std::max<int>(k, 0);
				m_lod.y = m_lod.x;
			}
			else
			{
				if (interpolation == 2)
				{
					m_lod.x = std::max<int>(static_cast<int>(floor(m_vt.m_lod.x)), 0);
				}
				else
				{
#if 0
					m_lod.x = std::max<int>(static_cast<int>(round(m_vt.m_lod.x + 0.0625)), 0);
#else
					if (ceil(m_vt.m_lod.x) < m_vt.m_lod.y)
						m_lod.x = std::max<int>(static_cast<int>(round(m_vt.m_lod.x + 0.0625 + 0.01)), 0);
					else
						m_lod.x = std::max<int>(static_cast<int>(round(m_vt.m_lod.x + 0.0625)), 0);
#endif
				}

				m_lod.y = std::max<int>(static_cast<int>(ceil(m_vt.m_lod.y)), 0);
			}

			m_lod.x = std::min<int>(m_lod.x, mxl);
			m_lod.y = std::min<int>(m_lod.y, mxl);

			TEX0 = (m_lod.x == 0) ? m_cached_ctx.TEX0 : GetTex0Layer(m_lod.x);

			hash_lod_range = GSVector2i(m_lod.x, GSConfig.HWMipmap ? mxl : m_lod.x);

			MIP_CLAMP.MINU >>= m_lod.x;
			MIP_CLAMP.MINV >>= m_lod.x;
			MIP_CLAMP.MAXU >>= m_lod.x;
			MIP_CLAMP.MAXV >>= m_lod.x;

			for (int i = 0; i < m_lod.x; i++)
			{
				m_vt.m_min.t *= 0.5f;
				m_vt.m_max.t *= 0.5f;
			}

			GL_CACHE("HW: Mipmap LOD %d %d (%f %f) new size %dx%d (K %d L %u)", m_lod.x, m_lod.y, m_vt.m_lod.x, m_vt.m_lod.y, 1 << TEX0.TW, 1 << TEX0.TH, m_context->TEX1.K, m_context->TEX1.L);
		}
		else
		{
			TEX0 = m_cached_ctx.TEX0;
		}

		tmm = GetTextureMinMax(TEX0, MIP_CLAMP, m_vt.IsLinear(), false);

		if (GSConfig.UserHacks_EstimateTextureRegion &&
			(PRIM->FST || (MIP_CLAMP.WMS == CLAMP_CLAMP && MIP_CLAMP.WMT == CLAMP_CLAMP)) &&
			TEX0.TW >= 9 && TEX0.TH >= 9 &&
			MIP_CLAMP.WMS < CLAMP_REGION_CLAMP && MIP_CLAMP.WMT < CLAMP_REGION_CLAMP &&
			((m_vt.m_max.t >= GSVector4(512.0f)).mask() & 0x3) == 0)
		{
			const GSVector4i maxt(m_vt.m_max.t + GSVector4(m_vt.IsLinear() ? 0.5f : 0.0f));
			MIP_CLAMP.WMS = CLAMP_REGION_CLAMP;
			MIP_CLAMP.WMT = CLAMP_REGION_CLAMP;
			MIP_CLAMP.MINU = 0;
			MIP_CLAMP.MAXU = maxt.x >> m_lod.x;
			MIP_CLAMP.MINV = 0;
			MIP_CLAMP.MAXV = maxt.y >> m_lod.x;
			GL_CACHE("HW: Estimated texture region: %u,%u -> %u,%u", MIP_CLAMP.MINU, MIP_CLAMP.MINV, MIP_CLAMP.MAXU + 1,
				MIP_CLAMP.MAXV + 1);
		}

		GIFRegTEX0 FRAME_TEX0;
		bool shuffle_target = false;
		const u32 page_alignment = GSLocalMemory::IsPageAlignedMasked(m_cached_ctx.TEX0.PSM, m_r);
		const bool page_aligned = (page_alignment & 0xF0F0) != 0;
		if (!no_rt && page_aligned && m_cached_ctx.ZBUF.ZMSK && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp == 16 && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp >= 16 &&
			(m_vt.m_primclass == GS_SPRITE_CLASS || (m_vt.m_primclass == GS_TRIANGLE_CLASS && (m_index->tail % 6) == 0 && TrianglesAreQuads(true) && m_index->tail > 6)))
		{
			if (GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp == 16 &&
				(m_index->tail >= (m_cached_ctx.TEX0.TBW * 2) || m_cached_ctx.TEX0.TBP0 == m_cached_ctx.FRAME.Block() || m_cached_ctx.CLAMP.WMS > CLAMP_CLAMP || m_cached_ctx.CLAMP.WMT > CLAMP_CLAMP))
			{
				const GSVertex* v = &m_vertex->buff[0];

				const int first_x = std::clamp((static_cast<int>(((v[0].XYZ.X - m_context->XYOFFSET.OFX) + 8))) >> 4, 0, 2048);
				const bool offset_last = PRIM->FST ? (v[1].U > v[0].U) : ((v[1].ST.S / v[1].RGBAQ.Q) > (v[0].ST.S / v[1].RGBAQ.Q));
				const int first_u = PRIM->FST ? ((v[0].U + (offset_last ? 0 : 9)) >> 4) : std::clamp(static_cast<int>(((1 << m_cached_ctx.TEX0.TW) * (v[0].ST.S / v[1].RGBAQ.Q)) + (offset_last ? 0.0f : 0.6f)), 0, 2048);
				const int second_u = PRIM->FST ? ((v[1].U + (offset_last ? 9 : 0)) >> 4) : std::clamp(static_cast<int>(((1 << m_cached_ctx.TEX0.TW) * (v[1].ST.S / v[1].RGBAQ.Q)) + (offset_last ? 0.6f : 0.0f)), 0, 2048);
				const u32 minv = m_cached_ctx.CLAMP.MINV;
				const u32 minu = m_cached_ctx.CLAMP.MINU;
				const bool rgba_shuffle = ((m_cached_ctx.CLAMP.WMS == m_cached_ctx.CLAMP.WMT && m_cached_ctx.CLAMP.WMS == CLAMP_REGION_REPEAT) && (minu && minv && ((minu + 1 & minu) || (minv + 1 & minv))));
				const bool shuffle_coords = ((first_x ^ first_u) & 0xF) == 8 || rgba_shuffle;

				const int draw_width = std::abs(v[1].XYZ.X + 9 - v[0].XYZ.X) >> 4;
				const int read_width = std::abs(second_u - first_u);

				shuffle_target = shuffle_coords && (((draw_width & 7) == 0 && std::abs(draw_width - read_width) <= 1) || m_skip > 50);
			}

			if (!shuffle_target)
			{
				bool shuffle_channel_reads = !m_cached_ctx.FRAME.FBMSK;
				const u32 increment = (m_vt.m_primclass == GS_TRIANGLE_CLASS) ? 3 : 2;
				const GSVertex* v = &m_vertex->buff[0];

				if (shuffle_channel_reads)
				{
					for (u32 i = 0; i < m_index->tail; i += increment)
					{
						const int first_u = (PRIM->FST ? v[i].U : static_cast<int>(v[i].ST.S / v[(increment == 2) ? i + 1 : i].RGBAQ.Q)) >> 4;
						const int second_u = (PRIM->FST ? v[i + 1].U : static_cast<int>(v[i + 1].ST.S / v[i + 1].RGBAQ.Q)) >> 4;
						const int vector_width = std::abs(v[i + 1].XYZ.X - v[i].XYZ.X) / 16;
						const int tex_width = std::abs(second_u - first_u);
						const int first_vector = (static_cast<int>(v[i].XYZ.X + 8) - static_cast<int>(m_context->XYOFFSET.OFX)) / 16;
						if ((vector_width & 7) != 0 || (tex_width & 7) != 0 || tex_width != vector_width || first_vector == first_u)
						{
							shuffle_channel_reads = false;
							break;
						}
					}
				}
				if (m_cached_ctx.FRAME.FBMSK || shuffle_channel_reads)
				{
					FRAME_TEX0.U64 = 0;
					FRAME_TEX0.TBP0 = m_cached_ctx.FRAME.Block();
					FRAME_TEX0.TBW = m_cached_ctx.FRAME.FBW;
					FRAME_TEX0.PSM = m_cached_ctx.FRAME.PSM;

					GSTextureCache::Target* tgt = g_texture_cache->FindOverlappingTarget(FRAME_TEX0.TBP0, GSLocalMemory::GetEndBlockAddress(FRAME_TEX0.TBP0, FRAME_TEX0.TBW, FRAME_TEX0.PSM, m_r));

					if (tgt)
						shuffle_target = tgt->m_32_bits_fmt;
					else
						shuffle_target = shuffle_channel_reads;

					tgt = nullptr;
				}
			}
		}
		const bool is_possible_channel_shuffle = IsPossibleChannelShuffle();
		possible_shuffle = !no_rt && (((shuffle_target ) ) || is_possible_channel_shuffle);
		const u32 channel_shuffle_targets = is_possible_channel_shuffle ? EmulateChannelShuffle(nullptr, true) : ChannelFetch_NONE;
		const bool need_aem_color = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp <= 24 && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pal == 0 && ((NeedsBlending() && m_context->ALPHA.C == 0) || IsDiscardingDstAlpha()) && m_cached_ctx.TEXA.AEM;
		const u32 color_mask = (m_vt.m_max.c > GSVector4i::zero()).mask();
		const bool texture_function_color = m_cached_ctx.TEX0.TFX == TFX_DECAL || (color_mask & 0xFFF) || (m_cached_ctx.TEX0.TFX > TFX_DECAL && (color_mask & 0xF000));
		const bool texture_function_alpha = m_cached_ctx.TEX0.TFX != TFX_MODULATE || (color_mask & 0xF000);
		const bool req_color = (is_possible_channel_shuffle && channel_shuffle_targets != ChannelFetch_ALPHA) || (!is_possible_channel_shuffle && ((texture_function_color && (!PRIM->ABE || GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp < 16 || (NeedsBlending() && IsUsingCsInBlend())) && (possible_shuffle || (m_cached_ctx.FRAME.FBMSK & (fm_mask & 0x00FFFFFF)) != (fm_mask & 0x00FFFFFF))) || need_aem_color));
		const bool alpha_used = (GSUtil::GetChannelMask(m_context->TEX0.PSM) == 0x8 || (m_context->TEX0.TCC && texture_function_alpha)) && ((NeedsBlending() && IsUsingAsInBlend()) || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST > ATST_ALWAYS) || (possible_shuffle || (m_cached_ctx.FRAME.FBMSK & (fm_mask & 0xFF000000)) != (fm_mask & 0xFF000000)));
		const bool req_alpha = (is_possible_channel_shuffle && channel_shuffle_targets == ChannelFetch_ALPHA) || (!is_possible_channel_shuffle && (GSUtil::GetChannelMask(m_context->TEX0.PSM) & 0x8) && alpha_used);

		if (!req_color && !alpha_used)
		{
			m_process_texture = false;
			possible_shuffle = false;
		}
		else
		{
			src = tex_psm.depth ? g_texture_cache->LookupDepthSource(true, TEX0, m_cached_ctx.TEXA, MIP_CLAMP, tmm.coverage, possible_shuffle, m_vt.IsLinear(), m_cached_ctx.FRAME, req_color, req_alpha)
			                    : g_texture_cache->LookupSource(true, TEX0, m_cached_ctx.TEXA, MIP_CLAMP, tmm.coverage, (GSConfig.HWMipmap || GSConfig.TriFilter == TriFiltering::Forced) ? &hash_lod_range : nullptr,
			                         possible_shuffle, m_vt.IsLinear(), m_cached_ctx.FRAME, req_color, req_alpha);

			if (!src) [[unlikely]]
			{
				GL_INS("HW: ERROR: Source lookup failed, skipping.");
				CleanupDraw(true);
				return;
			}



			const u32 draw_end = GSLocalMemory::GetEndBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r) + 1;
			const u32 draw_start = GSLocalMemory::GetStartBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r);
			draw_uses_target = src->m_from_target && ((src->m_from_target_TEX0.TBP0 <= draw_start && src->m_from_target->UnwrappedEndBlock() > m_cached_ctx.FRAME.Block()) ||
			                                          (m_cached_ctx.FRAME.Block() < src->m_from_target_TEX0.TBP0 && draw_end > src->m_from_target_TEX0.TBP0));

			if (possible_shuffle && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp != 16)
				possible_shuffle &= draw_uses_target;

			const bool shuffle_source = possible_shuffle && src && ((src->m_from_target != nullptr && GSLocalMemory::m_psm[src->m_from_target->m_TEX0.PSM].bpp != 16) || m_skip);

			if (!shuffle_source && possible_shuffle)
			{
				const bool is_16bit_copy = m_cached_ctx.TEX0.TBP0 != m_cached_ctx.FRAME.Block() && shuffle_target && IsOpaque() && !(context->TEX1.MMIN & 1) && !src->m_32_bits_fmt && m_cached_ctx.FRAME.FBMSK;
				possible_shuffle &= is_16bit_copy || (m_cached_ctx.TEX0.TBP0 == m_cached_ctx.FRAME.Block() && shuffle_target);
			}
			if (!IsPossibleChannelShuffle() && src->m_valid_alpha_minmax)
			{
				CalcAlphaMinMax(src->m_alpha_minmax.first, src->m_alpha_minmax.second);

				u32 new_fm = m_context->FRAME.FBMSK;
				u32 new_zm = (m_cached_ctx.ZBUF.ZMSK || m_cached_ctx.TEST.ZTE == 0 ||
				             (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ZTST_NEVER && m_cached_ctx.TEST.AFAIL != AFAIL_ZB_ONLY)) ?
				             0xffffffffu :
				             0;
				if (m_cached_ctx.TEST.ATE && GSRenderer::TryAlphaTest(new_fm, new_zm))
				{
					m_cached_ctx.TEST.ATE = false;
					m_cached_ctx.FRAME.FBMSK = new_fm;
					m_cached_ctx.ZBUF.ZMSK = (new_zm != 0);
					fm = new_fm;
					zm = new_zm;
					no_rt = no_rt || (!m_cached_ctx.TEST.DATE && !IsRTWritten());
					no_ds = no_ds || (zm != 0 && all_depth_tests_pass) ||
					        (!no_rt && m_cached_ctx.FRAME.FBP == m_cached_ctx.ZBUF.ZBP && !PRIM->TME && zm == 0 && (fm & fm_mask) == 0 && m_cached_ctx.TEST.ZTE) ||
					        (no_rt && zm != 0);
				}
				else
				{
					no_rt = no_rt || (!m_cached_ctx.TEST.DATE && !IsRTWritten());
					no_ds = no_ds ||
					        (!no_rt && m_cached_ctx.FRAME.FBP == m_cached_ctx.ZBUF.ZBP && !PRIM->TME && zm == 0 && (fm & fm_mask) == 0 && m_cached_ctx.TEST.ZTE) ||
					        (no_rt && zm != 0);
				}

				if (no_rt && no_ds)
				{
					GL_INS("HW: Late draw cancel.");
					CleanupDraw(true);
					return;
				}
			}
		}
	}

	const bool output_black = NeedsBlending() && ((m_context->ALPHA.A == 1 && m_context->ALPHA.D > 1) || (m_context->ALPHA.IsBlack() && m_context->ALPHA.D != 1)) && m_draw_env->COLCLAMP.CLAMP == 1;
	const bool can_expand = !(m_cached_ctx.ZBUF.ZMSK && output_black);

	GSVector2i t_size = GetTargetSize(src, can_expand, possible_shuffle);
	const GSVector4i t_size_rect = GSVector4i::loadh(t_size);

	const GSVector4i unclamped_draw_rect = m_r;

	float target_scale = GetTextureScaleFactor();
	bool scaled_copy = false;
	int scale_draw = IsScalingDraw(src, m_primitive_covers_without_gaps != NoGapsType::GapsFound);
	m_downscale_source = false;

	if (GSConfig.UserHacks_NativeScaling != GSNativeScaling::Off)
	{
		if (target_scale > 1.0f && scale_draw > 0)
		{
			if (scale_draw == 1)
			{
				m_downscale_source = src->m_from_target ? src->m_from_target->GetScale() > 1.0f : false;
				const bool highlights_only = m_cached_ctx.TEST.ATE || (PRIM->ABE && m_context->ALPHA.C == 2 && m_context->ALPHA.FIX == 255);
				if (GSConfig.UserHacks_NativeScaling < GSNativeScaling::NormalUpscaled || highlights_only || !PRIM->ABE || (src->m_from_target && src->m_from_target->Overlaps(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r)))
				{
					target_scale = 1.0f;
				}
			}
			else
				m_downscale_source = ((GSConfig.UserHacks_NativeScaling != GSNativeScaling::Aggressive && GSConfig.UserHacks_NativeScaling != GSNativeScaling::AggressiveUpscaled) || !src->m_from_target) ? false : src->m_from_target->GetScale() > 1.0f;
		}
		else
		{
			if (scale_draw == -1 && src && (!src->m_from_target || (src->m_from_target && src->m_from_target->m_downscaled)) && ((static_cast<int>(m_cached_ctx.FRAME.FBW * 64) <= (PCRTCDisplays.GetResolution().x >> 1) &&
				(GSVector4i(m_vt.m_min.p).xyxy() == GSVector4i(m_vt.m_min.t).xyxy()).alltrue() && (GSVector4i(m_vt.m_max.p).xyxy() == GSVector4i(m_vt.m_max.t).xyxy()).alltrue()) || possible_shuffle))
			{
				target_scale = src->m_from_target ? src->m_from_target->GetScale() : 1.0f;
				scale_draw = 1;
				scaled_copy = true;
			}
		}
	}

	if (IsPossibleChannelShuffle() && src && src->m_from_target && src->m_from_target->GetScale() != target_scale)
	{
		target_scale = src->m_from_target->GetScale();
	}
	if (no_ds && src && !m_channel_shuffle && src->m_from_target && (GSConfig.UserHacks_NativePaletteDraw || (src->m_target_direct && src->m_from_target->m_downscaled && scale_draw <= 1)) &&
		src->m_scale == 1.0f && (src->m_TEX0.PSM == PSMT8 || src->m_TEX0.TBP0 == m_cached_ctx.FRAME.Block()))
	{
		GL_CACHE("HW: Using native resolution for target based on texture source");
		target_scale = 1.0f;
	}

	if (!m_process_texture && tex_is_rt)
	{
		tex_is_rt = (m_process_texture && m_cached_ctx.TEX0.TBP0 >= m_cached_ctx.FRAME.Block() &&
			m_cached_ctx.TEX0.TBP0 < frame_end_bp);
		preserve_rt_rgb = (!no_rt && (!IsDiscardingDstRGB() || not_writing_to_all || tex_is_rt));
		preserve_rt_alpha =
			(!no_rt && (!IsDiscardingDstAlpha() || not_writing_to_all ||
				(tex_is_rt && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp != 24)));
		preserve_rt_color = preserve_rt_rgb || preserve_rt_alpha;
	}

	GSTextureCache::Target* rt = nullptr;
	GIFRegTEX0 FRAME_TEX0;
	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];

	m_in_target_draw = false;
	m_target_offset = 0;

	GSTextureCache::Target* ds = nullptr;
	GIFRegTEX0 ZBUF_TEX0;
	ZBUF_TEX0.U64 = 0;

	if (!no_ds)
	{
		ZBUF_TEX0.TBP0 = m_cached_ctx.ZBUF.Block();
		ZBUF_TEX0.TBW = m_cached_ctx.FRAME.FBW;
		ZBUF_TEX0.PSM = m_cached_ctx.ZBUF.PSM;

		ds = g_texture_cache->LookupDrawTarget(ZBUF_TEX0, t_size, target_scale, GSTextureCache::DepthStencil,
			m_cached_ctx.DepthWrite(), 0, force_preload, preserve_depth, preserve_depth, unclamped_draw_rect, IsPossibleChannelShuffle(), is_possible_mem_clear && ZBUF_TEX0.TBP0 != m_cached_ctx.FRAME.Block(), !no_rt,
			src, nullptr, -1);

		ZBUF_TEX0.TBW = m_channel_shuffle ? src->m_from_target_TEX0.TBW : m_cached_ctx.FRAME.FBW;

		if (!ds && m_cached_ctx.FRAME.FBP != m_cached_ctx.ZBUF.ZBP)
		{
			ds = g_texture_cache->CreateTarget(ZBUF_TEX0, t_size, GetValidSize(src, possible_shuffle), target_scale, GSTextureCache::DepthStencil,
				true, 0, false, force_preload, preserve_depth, m_r, src);
			if (!ds) [[unlikely]]
			{
				GL_INS("HW: ERROR: Failed to create ZBUF target, skipping.");
				CleanupDraw(true);
				return;
			}
		}
		else
		{
			if (((zm && m_cached_ctx.TEST.ZTST > ZTST_ALWAYS) || (m_vt.m_eq.z && m_cached_ctx.TEST.ZTST == ZTST_GEQUAL)) && GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].trbpp == 32)
			{
				if (ds->m_alpha_max != 0)
				{
					const u32 max_z = (static_cast<u64>(ds->m_alpha_max + 1) << 24) - 1;

					switch (m_cached_ctx.TEST.ZTST)
					{
						case ZTST_GEQUAL:
							if (max_z <= m_vt.m_min.p.z)
							{
								m_cached_ctx.TEST.ZTST = ZTST_ALWAYS;
								if (zm)
								{
									ds = nullptr;
									no_ds = true;
								}
							}
							break;
						case ZTST_GREATER:
							if (max_z < m_vt.m_min.p.z)
							{
								m_cached_ctx.TEST.ZTST = ZTST_ALWAYS;
								if (zm)
								{
									ds = nullptr;
									no_ds = true;
								}
							}
							break;
						default:
							break;
					}
				}
			}
		}

		if (no_rt && ds && ds->m_TEX0.TBP0 != m_cached_ctx.ZBUF.Block())
		{
			const GSLocalMemory::psm_t& zbuf_psm = GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM];
			int vertical_offset = ((static_cast<int>(m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0) / 32) / std::max(static_cast<int>(ds->m_TEX0.TBW), 1)) * zbuf_psm.pgs.y;
			int texture_offset = 0;
			int horizontal_offset = ((static_cast<int>((m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0)) / 32) % static_cast<int>(std::max(ds->m_TEX0.TBW, 1U))) * zbuf_psm.pgs.x;
			m_target_offset = std::abs(static_cast<int>((m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0)) >> 5);

			if (vertical_offset < 0)
			{
				ds->m_TEX0.TBP0 = m_cached_ctx.ZBUF.Block();
				GSVector2i new_size = ds->m_unscaled_size;
				const int new_offset = std::abs((vertical_offset / zbuf_psm.pgs.y) * GSLocalMemory::m_psm[ds->m_TEX0.PSM].pgs.y);
				texture_offset = new_offset;

				new_size.y += new_offset;

				const GSVector4i new_drect = GSVector4i(0, new_offset * ds->m_scale, new_size.x * ds->m_scale, new_size.y * ds->m_scale);
				ds->ResizeTexture(new_size.x, new_size.y, true, true, new_drect);

				if (src && src->m_from_target && src->m_from_target == ds && src->m_target_direct)
				{
					src->m_texture = ds->m_texture;

					int max_region_y = src->m_region.GetMaxY() + new_offset;
					if (max_region_y == new_offset)
						max_region_y = new_size.y;

					src->m_region.SetY(src->m_region.GetMinY() + new_offset, max_region_y);
				}

				ds->m_valid.y += new_offset;
				ds->m_valid.w += new_offset;
				ds->m_drawn_since_read.y += new_offset;
				ds->m_drawn_since_read.w += new_offset;

				g_texture_cache->CombineAlignedInsideTargets(ds, src);

				if (ds->m_dirty.size())
				{
					for (int i = 0; i < static_cast<int>(ds->m_dirty.size()); i++)
					{
						ds->m_dirty[i].r.y += new_offset;
						ds->m_dirty[i].r.w += new_offset;
					}
				}

				t_size.y += std::abs(vertical_offset);
				vertical_offset = 0;
			}

			if (horizontal_offset < 0)
			{
				ds->m_TEX0.TBP0 += horizontal_offset;
				horizontal_offset = 0;
			}

			if (vertical_offset || horizontal_offset)
			{
				GSVertex* v = &m_vertex->buff[0];

				for (u32 i = 0; i < m_vertex->tail; i++)
				{
					v[i].XYZ.X += horizontal_offset << 4;
					v[i].XYZ.Y += vertical_offset << 4;
				}

				if (texture_offset && src && src->m_from_target && src->m_target_direct && src->m_from_target == ds)
				{
					GSVector4i src_region = src->GetRegionRect();

					if (src_region.rempty())
					{
						src_region = GSVector4i::loadh(ds->m_unscaled_size);
						src_region.y += texture_offset;
					}
					else
					{
						src_region.y += texture_offset;
						src_region.w += texture_offset;
					}
					src->m_region.SetX(src_region.x, src_region.z);
					src->m_region.SetY(src_region.y, src_region.w);
				}

				m_context->scissor.in.x += horizontal_offset;
				m_context->scissor.in.z += horizontal_offset;
				m_context->scissor.in.y += vertical_offset;
				m_context->scissor.in.w += vertical_offset;
				m_r.y += vertical_offset;
				m_r.w += vertical_offset;
				m_r.x += horizontal_offset;
				m_r.z += horizontal_offset;
				m_in_target_draw = ds->m_TEX0.TBP0 != m_cached_ctx.ZBUF.Block();
				m_vt.m_min.p.x += horizontal_offset;
				m_vt.m_max.p.x += horizontal_offset;
				m_vt.m_min.p.y += vertical_offset;
				m_vt.m_max.p.y += vertical_offset;

				t_size.y = ds->m_unscaled_size.y - vertical_offset;
				t_size.x = ds->m_unscaled_size.x - horizontal_offset;
			}

			GSVector2i new_size = GetValidSize(src, possible_shuffle);
			if (new_size.x > ds->m_unscaled_size.x || new_size.y > ds->m_unscaled_size.y)
			{
				const u32 new_width = std::max(new_size.x, ds->m_unscaled_size.x);
				const u32 new_height = std::max(new_size.y, ds->m_unscaled_size.y);

				ds->ResizeTexture(new_width, new_height);
			}
			else if ((IsPageCopy() || is_possible_mem_clear) && m_r.width() <= zbuf_psm.pgs.x && m_r.height() <= zbuf_psm.pgs.y)
			{
				const int get_next_ctx = m_env.PRIM.CTXT;
				const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];
				GSVector4i update_valid = GSVector4i::loadh(GSVector2i(horizontal_offset + GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.x, GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.y + vertical_offset));
				ds->UpdateValidity(update_valid, true);
				if (is_possible_mem_clear)
				{
					if ((horizontal_offset + GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.x) >= static_cast<int>(ds->m_TEX0.TBW * 64) && next_ctx.ZBUF.Block() == (m_cached_ctx.ZBUF.Block() + 0x20))
					{
						update_valid.x = 0;
						update_valid.z = GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.x;
						update_valid.y += GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.y;
						update_valid.w += GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.y;
						ds->UpdateValidity(update_valid, true);
					}
				}
			}
		}
	}

	if (!no_rt)
	{
		possible_shuffle |= draw_sprite_tex && m_process_texture && m_primitive_covers_without_gaps != NoGapsType::FullCover &&
		                    (((src && src->m_target && src->m_from_target && src->m_from_target->m_32_bits_fmt) &&
		                      (GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp == 16 || draw_uses_target) &&
		                      GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp == 16) ||
		                     IsPossibleChannelShuffle());

		const bool possible_horizontal_texture_shuffle = possible_shuffle && src && src->m_from_target && m_r.w <= src->m_from_target->m_valid.w && m_r.z > src->m_from_target->m_valid.z && m_cached_ctx.FRAME.FBW > src->m_from_target_TEX0.TBW;

		FRAME_TEX0.U64 = 0;
		FRAME_TEX0.TBP0 = ((m_last_channel_shuffle_end_block + 1) == m_cached_ctx.FRAME.Block() && possible_shuffle) ? m_last_channel_shuffle_fbp : m_cached_ctx.FRAME.Block();
		FRAME_TEX0.TBW = (possible_horizontal_texture_shuffle || (possible_shuffle && src && src->m_from_target && IsPossibleChannelShuffle() && m_cached_ctx.FRAME.FBW <= 2)) ? src->m_from_target_TEX0.TBW : m_cached_ctx.FRAME.FBW;
		FRAME_TEX0.PSM = m_cached_ctx.FRAME.PSM;

		if (!possible_shuffle && m_split_texture_shuffle_pages == 0)
			m_r = m_r.rintersect(t_size_rect);

		GSVector4i lookup_rect = unclamped_draw_rect;
		if (possible_shuffle && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp == 16 && GSLocalMemory ::m_psm[m_cached_ctx.FRAME.PSM].bpp == 16)
		{
			const int get_next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : m_backed_up_ctx;
			const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];

			if (next_ctx.FRAME.Block() == FRAME_TEX0.TBP0 && next_ctx.FRAME.PSM != FRAME_TEX0.PSM)
				FRAME_TEX0.PSM = next_ctx.FRAME.PSM;
			else if (next_ctx.TEX0.TBP0 == FRAME_TEX0.TBP0 && next_ctx.TEX0.PSM != FRAME_TEX0.PSM)
				FRAME_TEX0.PSM = next_ctx.TEX0.PSM;
			else
				FRAME_TEX0.PSM = PSMCT32;

			if (GSLocalMemory::m_psm[FRAME_TEX0.PSM].bpp == 32 && src && src->m_from_target)
			{
				if (std::abs((lookup_rect.width() / 2) - src->m_from_target->m_unscaled_size.x) <= 8)
				{
					lookup_rect.x /= 2;
					lookup_rect.z /= 2;
				}
				else
				{
					lookup_rect.y /= 2;
					lookup_rect.w /= 2;
				}
			}
		}

		const bool is_large_rect = (t_size.y >= t_size.x) && m_r.w >= 1023 && m_primitive_covers_without_gaps == NoGapsType::FullCover;
		const bool is_clear = is_possible_mem_clear && is_large_rect;

		const bool preserve_downscale_draw = (GSConfig.UserHacks_NativeScaling != GSNativeScaling::Off && ((std::abs(scale_draw) == 1 && !scaled_copy) || (scale_draw == 0 && src && src->m_from_target && src->m_from_target->m_downscaled))) || is_possible_mem_clear == ClearType::ClearWithDraw;

		rt = g_texture_cache->LookupDrawTarget(FRAME_TEX0, t_size, ((src && src->m_scale != 1) && (GSConfig.UserHacks_NativeScaling == GSNativeScaling::Normal || GSConfig.UserHacks_NativeScaling == GSNativeScaling::NormalUpscaled) && !possible_shuffle) ? GetTextureScaleFactor() : target_scale, GSTextureCache::RenderTarget, true,
			fm, force_preload, preserve_rt_rgb, preserve_rt_alpha, lookup_rect, possible_shuffle, is_possible_mem_clear && FRAME_TEX0.TBP0 != m_cached_ctx.ZBUF.Block(),
			GSConfig.UserHacks_NativeScaling != GSNativeScaling::Off && preserve_downscale_draw && is_possible_mem_clear != ClearType::NormalClear, src, ds, (no_ds || !ds) ? -1 : (m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0));

		if (!rt)
		{
			if (is_clear)
			{
				GL_INS("HW: Clear draw with no target, skipping.");

				const bool is_zero_color_clear = (GetConstantDirectWriteMemClearColor() == 0 && !preserve_rt_color);
				const bool is_zero_depth_clear = (GetConstantDirectWriteMemClearDepth() == 0 && !preserve_depth);
				const u32 rt_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(
					m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r);
				const u32 ds_end_bp = GSLocalMemory::GetUnwrappedEndBlockAddress(
					m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM, m_r);
				TryGSMemClear(no_rt, preserve_rt_color, is_zero_color_clear, rt_end_bp,
					no_ds, preserve_depth, is_zero_depth_clear, ds_end_bp);

				CleanupDraw(true);
				return;
			}
			else if (IsPageCopy() && src->m_from_target && m_cached_ctx.TEX0.TBP0 >= src->m_from_target->m_TEX0.TBP0 && m_cached_ctx.FRAME.FBW < ((src->m_from_target->m_TEX0.TBW + 1) >> 1))
			{
				FRAME_TEX0.TBW = src->m_from_target->m_TEX0.TBW;
			}

			if (possible_shuffle && IsSplitTextureShuffle(FRAME_TEX0, lookup_rect))
			{
				if (m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0)
					g_texture_cache->InvalidateVideoMem(context->offset.fb, m_r, false);

				CleanupDraw(true);
				return;
			}

			rt = g_texture_cache->CreateTarget(FRAME_TEX0, t_size, GetValidSize(src, possible_shuffle), (GSConfig.UserHacks_NativeScaling != GSNativeScaling::Off && scale_draw < 0 && is_possible_mem_clear != ClearType::NormalClear) ? ((src && src->m_from_target) ? src->m_from_target->GetScale() : (ds ? ds->m_scale : 1.0f)) : target_scale,
			                                   GSTextureCache::RenderTarget, true, fm, false, force_preload, preserve_rt_color || possible_shuffle, lookup_rect, src);

			if (!rt) [[unlikely]]
			{
				GL_INS("HW: ERROR: Failed to create FRAME target, skipping.");
				CleanupDraw(true);
				return;
			}

			if (IsPageCopy() && m_cached_ctx.FRAME.FBW == 1)
			{
				rt->UpdateValidity(GSVector4i::loadh(GSVector2i(GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.x, GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.y)), true);
			}

			if (src && !src->m_from_target && GSLocalMemory::m_psm[src->m_TEX0.PSM].bpp == GSLocalMemory::m_psm[m_context->FRAME.PSM].bpp &&
				(GSUtil::GetChannelMask(src->m_TEX0.PSM) & GSUtil::GetChannelMask(m_context->FRAME.PSM)) != 0)
			{
				const u32 draw_end = GSLocalMemory::GetEndBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r) + 1;
				const u32 draw_start = GSLocalMemory::GetStartBlockAddress(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r);

				if (draw_start <= src->m_TEX0.TBP0 && draw_end > src->m_TEX0.TBP0)
				{
					g_texture_cache->ReplaceSourceTexture(src, rt->GetTexture(), rt->GetScale(), rt->GetUnscaledSize(), nullptr, true);

					src->m_from_target = rt;
					src->m_from_target_TEX0 = rt->m_TEX0;
					src->m_target_direct = true;
					src->m_shared_texture = true;
					src->m_target = true;
					src->m_texture = rt->m_texture;
					src->m_32_bits_fmt = rt->m_32_bits_fmt;
					src->m_valid_rect = rt->m_valid;
					src->m_alpha_minmax.first = rt->m_alpha_min;
					src->m_alpha_minmax.second = rt->m_alpha_max;

					const int target_width = std::max(FRAME_TEX0.TBW, 1U);
					const int page_offset = (src->m_TEX0.TBP0 - rt->m_TEX0.TBP0) >> 5;
					const int vertical_page_offset = page_offset / target_width;
					const int horizontal_page_offset = page_offset - (vertical_page_offset * target_width);

					if (vertical_page_offset)
					{
						const int height = std::max(rt->m_valid.w, possible_shuffle ? (m_r.w / 2) : m_r.w);
						src->m_region.SetY(vertical_page_offset * GSLocalMemory::m_psm[rt->m_TEX0.PSM].pgs.y, height);
					}
					if (horizontal_page_offset)
						src->m_region.SetX(horizontal_page_offset * GSLocalMemory::m_psm[rt->m_TEX0.PSM].pgs.x, target_width * GSLocalMemory::m_psm[rt->m_TEX0.PSM].pgs.x);

					if (rt->m_dirty.empty())
					{
						RGBAMask rgba_mask;
						rgba_mask._u32 = GSUtil::GetChannelMask(rt->m_TEX0.PSM);
						g_texture_cache->AddDirtyRectTarget(rt, m_r, FRAME_TEX0.PSM, FRAME_TEX0.TBW, rgba_mask, GSLocalMemory::m_psm[FRAME_TEX0.PSM].trbpp >= 16);
					}
				}
			}
		}
		else if (rt->m_TEX0.TBP0 != m_cached_ctx.FRAME.Block())
		{
			int vertical_offset = ((static_cast<int>(m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) / 32) / std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * frame_psm.pgs.y;
			int texture_offset = 0;
			int horizontal_offset = ((static_cast<int>((m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0)) / 32) % static_cast<int>(std::max(rt->m_TEX0.TBW, 1U))) * frame_psm.pgs.x;
			m_target_offset = std::abs(static_cast<int>((m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0)) >> 5);

			if (vertical_offset < 0)
			{
				rt->m_TEX0.TBP0 = m_cached_ctx.FRAME.Block();
				GSVector2i new_size = rt->m_unscaled_size;
				const int new_offset = std::abs((vertical_offset / frame_psm.pgs.y) * GSLocalMemory::m_psm[rt->m_TEX0.PSM].pgs.y);
				texture_offset = new_offset;

				new_size.y += new_offset;

				const GSVector4i new_drect = GSVector4i(0, new_offset * rt->m_scale, new_size.x * rt->m_scale, new_size.y * rt->m_scale);
				rt->ResizeTexture(new_size.x, new_size.y, true, true, new_drect);

				if (src && src->m_from_target && src->m_from_target == rt && src->m_target_direct)
				{
					src->m_texture = rt->m_texture;

					int max_region_y = src->m_region.GetMaxY() + new_offset;
					if (max_region_y == new_offset)
						max_region_y = new_size.y;

					src->m_region.SetY(src->m_region.GetMinY() + new_offset, max_region_y);
				}

				rt->m_valid.y += new_offset;
				rt->m_valid.w += new_offset;
				rt->m_drawn_since_read.y += new_offset;
				rt->m_drawn_since_read.w += new_offset;

				g_texture_cache->CombineAlignedInsideTargets(rt, src);

				if (rt->m_dirty.size())
				{
					for (int i = 0; i < static_cast<int>(rt->m_dirty.size()); i++)
					{
						rt->m_dirty[i].r.y += new_offset;
						rt->m_dirty[i].r.w += new_offset;
					}
				}

				t_size.y += std::abs(vertical_offset);
				vertical_offset = 0;
			}

			if (horizontal_offset < 0)
			{
				rt->m_TEX0.TBP0 += horizontal_offset;
				horizontal_offset = 0;
			}

			if (vertical_offset || horizontal_offset)
			{
				GSVertex* v = &m_vertex->buff[0];

				for (u32 i = 0; i < m_vertex->tail; i++)
				{
					v[i].XYZ.X += horizontal_offset << 4;
					v[i].XYZ.Y += vertical_offset << 4;
				}

				if (texture_offset && src && src->m_from_target && src->m_target_direct && src->m_from_target == rt)
				{
					GSVector4i src_region = src->GetRegionRect();

					if (src_region.rempty())
					{
						src_region = GSVector4i::loadh(rt->m_unscaled_size);
						src_region.y += texture_offset;
					}
					else
					{
						src_region.y += texture_offset;
						src_region.w += texture_offset;
					}
					src->m_region.SetX(src_region.x, src_region.z);
					src->m_region.SetY(src_region.y, src_region.w);
				}

				m_context->scissor.in.x += horizontal_offset;
				m_context->scissor.in.z += horizontal_offset;
				m_context->scissor.in.y += vertical_offset;
				m_context->scissor.in.w += vertical_offset;
				m_r.y += vertical_offset;
				m_r.w += vertical_offset;
				m_r.x += horizontal_offset;
				m_r.z += horizontal_offset;
				m_in_target_draw = rt->m_TEX0.TBP0 != m_cached_ctx.FRAME.Block();
				m_vt.m_min.p.x += horizontal_offset;
				m_vt.m_max.p.x += horizontal_offset;
				m_vt.m_min.p.y += vertical_offset;
				m_vt.m_max.p.y += vertical_offset;

				t_size.x = rt->m_unscaled_size.x - horizontal_offset;
				t_size.y = rt->m_unscaled_size.y - vertical_offset;
			}

			GSVector2i new_size = GetValidSize(src, possible_shuffle);
			if (new_size.x > rt->m_unscaled_size.x || new_size.y > rt->m_unscaled_size.y)
			{
				const u32 new_width = std::max(new_size.x, rt->m_unscaled_size.x);
				const u32 new_height = std::max(new_size.y, rt->m_unscaled_size.y);

				rt->ResizeTexture(new_width, new_height);
			}
			else if ((IsPageCopy() || is_possible_mem_clear) && m_r.width() <= frame_psm.pgs.x && m_r.height() <= frame_psm.pgs.y)
			{
				const int get_next_ctx = m_env.PRIM.CTXT;
				const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];
				GSVector4i update_valid = GSVector4i::loadh(GSVector2i(horizontal_offset + GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.x, GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.y + vertical_offset));
				rt->UpdateValidity(update_valid, true);
				if (is_possible_mem_clear)
				{
					if ((horizontal_offset + GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.x) >= static_cast<int>(rt->m_TEX0.TBW * 64) && next_ctx.FRAME.Block() == (m_cached_ctx.FRAME.Block() + 0x20))
					{
						update_valid.x = 0;
						update_valid.z = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.x;
						update_valid.y += GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.y;
						update_valid.w += GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.y;
						rt->UpdateValidity(update_valid, true);
					}
				}
			}
		}
		if (ds && rt && ((m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0) != (m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) || (g_texture_cache->GetTemporaryZ() != nullptr && g_texture_cache->GetTemporaryZInfo().ZBP == ds->m_TEX0.TBP0)))
		{
			m_using_temp_z = true;
			const int page_offset = static_cast<int>(m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0) / 32;
			const int rt_page_offset = static_cast<int>(m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) / 32;
			const int z_vertical_offset = (page_offset / std::max(static_cast<int>(ds->m_TEX0.TBW), 1)) * GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.y;
			const int z_horizontal_offset = (page_offset % std::max(static_cast<int>(ds->m_TEX0.TBW), 1)) * GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.x;

			if (g_texture_cache->GetTemporaryZ() != nullptr)
			{
				GSTextureCache::TempZAddress z_address_info = g_texture_cache->GetTemporaryZInfo();

				const int old_z_vertical_offset = (z_address_info.offset / std::max(static_cast<int>(ds->m_TEX0.TBW), 1)) * GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.y;
				const int old_z_horizontal_offset = (z_address_info.offset % std::max(static_cast<int>(ds->m_TEX0.TBW), 1)) * GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.x;

				if (ds->m_TEX0.TBP0 != z_address_info.ZBP || z_address_info.offset != page_offset || z_address_info.rt_offset != rt_page_offset)
				{
					if (m_temp_z_full_copy && z_address_info.ZBP == ds->m_TEX0.TBP0)
					{
						const int vertical_offset = (z_address_info.rt_offset / std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * frame_psm.pgs.y;
						const int horizontal_offset = (z_address_info.rt_offset % std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * frame_psm.pgs.x;
						const int old_z_vertical_offset = (z_address_info.offset / std::max(static_cast<int>(ds->m_TEX0.TBW), 1)) * GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.y;
						const int old_z_horizontal_offset = (z_address_info.offset % std::max(static_cast<int>(ds->m_TEX0.TBW), 1)) * GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.x;

						const GSVector4i dRect = GSVector4i((ds->m_valid.x + old_z_vertical_offset) * ds->m_scale, (ds->m_valid.y + old_z_horizontal_offset) * ds->m_scale, (ds->m_valid.z + old_z_vertical_offset + (1.0f / ds->m_scale)) * ds->m_scale, (ds->m_valid.w + old_z_horizontal_offset + (1.0f / ds->m_scale)) * ds->m_scale);
						const GSVector4 sRect = GSVector4(((ds->m_valid.x + horizontal_offset) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetWidth()), static_cast<float>((ds->m_valid.y + vertical_offset) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetHeight()), (((ds->m_valid.z + horizontal_offset) + (1.0f / ds->m_scale)) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetWidth()),
							static_cast<float>((ds->m_valid.w + vertical_offset + (1.0f / ds->m_scale)) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetHeight()));

						GL_CACHE("HW: RT in RT Z copy back draw %lld z_vert_offset %d z_offset %d", s_n, z_vertical_offset, vertical_offset);
						g_gs_device->StretchRectAuto(g_texture_cache->GetTemporaryZ(), sRect, ds->m_texture, GSVector4(dRect), Nearest);
					}

					g_texture_cache->InvalidateTemporaryZ();
					m_temp_z_full_copy = false;
					m_using_temp_z = false;
				}
				else if (!m_r.rintersect(z_address_info.rect_since).rempty() && m_cached_ctx.TEST.ZTST > ZTST_ALWAYS)
				{
					GL_CACHE("HW: RT in RT Updating Z copy on draw %lld z_offset %d", s_n, z_address_info.offset);
					GSVector4 sRect = GSVector4(z_address_info.rect_since.x / static_cast<float>(ds->m_unscaled_size.x), z_address_info.rect_since.y / static_cast<float>(ds->m_unscaled_size.y), (z_address_info.rect_since.z + (1.0f / ds->m_scale)) / static_cast<float>(ds->m_unscaled_size.x), (z_address_info.rect_since.w + (1.0f / ds->m_scale)) / static_cast<float>(ds->m_unscaled_size.y));
					GSVector4i dRect = GSVector4i((old_z_horizontal_offset + z_address_info.rect_since.x) * ds->m_scale, (old_z_vertical_offset + z_address_info.rect_since.y) * ds->m_scale, (old_z_horizontal_offset + z_address_info.rect_since.z + (1.0f / ds->m_scale)) * ds->m_scale, (old_z_vertical_offset + z_address_info.rect_since.w + (1.0f / ds->m_scale)) * ds->m_scale);

					sRect = sRect.min(GSVector4(1.0f));
					dRect = dRect.min_u32(GSVector4i(ds->m_unscaled_size.x * ds->m_scale, ds->m_unscaled_size.y * ds->m_scale).xyxy());

					g_gs_device->StretchRectAuto(ds->m_texture, sRect, g_texture_cache->GetTemporaryZ(), GSVector4(dRect), Nearest);
					z_address_info.rect_since = GSVector4i::zero();
					g_texture_cache->SetTemporaryZInfo(z_address_info);
				}
			}

			if (g_texture_cache->GetTemporaryZ() == nullptr && (m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0) != (m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0))
			{
				ds->Update();
				
				m_using_temp_z = true;
				const int get_next_ctx = m_env.PRIM.CTXT;
				const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];
				const int vertical_page_offset = (rt_page_offset / std::max(static_cast<int>(rt->m_TEX0.TBW), 1));
				const int vertical_offset = vertical_page_offset * frame_psm.pgs.y;
				const int horizontal_offset = (rt_page_offset - (vertical_page_offset * std::max(static_cast<int>(rt->m_TEX0.TBW), 1))) * frame_psm.pgs.x;

				const u32 horizontal_size = std::max(rt->m_unscaled_size.x, ds->m_unscaled_size.x);
				const u32 vertical_size = std::max(rt->m_unscaled_size.y, ds->m_unscaled_size.y);

				GSVector4i dRect = GSVector4i(horizontal_offset * ds->m_scale, vertical_offset * ds->m_scale, (horizontal_offset + (ds->m_unscaled_size.x - z_horizontal_offset)) * ds->m_scale, (vertical_offset + (ds->m_unscaled_size.y - z_vertical_offset)) * ds->m_scale);

				const int new_height = std::min(2048, std::max(t_size.y, static_cast<int>(vertical_size))) * ds->m_scale;
				const int new_width = std::min(2048, std::max(t_size.x, static_cast<int>(horizontal_size))) * ds->m_scale;

				if (GSTexture* tex = g_gs_device->CreateDepthStencil(new_width, new_height, true))
				{
					GSVector4 sRect = GSVector4(static_cast<float>(z_horizontal_offset) / static_cast<float>(ds->m_unscaled_size.x), static_cast<float>(z_vertical_offset) / static_cast<float>(ds->m_unscaled_size.y), 1.0f , 1.0f);

					const bool restricted_copy = !(((next_ctx.ZBUF.ZBP == m_context->ZBUF.ZBP && next_ctx.FRAME.FBP == m_context->FRAME.FBP)) && !(IsPossibleChannelShuffle() && src && (!src->m_from_target || EmulateChannelShuffle(src->m_from_target, true)) && !IsPageCopy()));

					if (restricted_copy)
					{
						dRect = GSVector4i(m_r.x * ds->m_scale, m_r.y * ds->m_scale, ((1 + m_r.z) * ds->m_scale), ((1 + m_r.w) * ds->m_scale));
						sRect = GSVector4(static_cast<float>((m_r.x - horizontal_offset) + z_horizontal_offset) / static_cast<float>(ds->m_unscaled_size.x), static_cast<float>((m_r.y - vertical_offset) + z_vertical_offset) / static_cast<float>(ds->m_unscaled_size.y), (static_cast<float>((m_r.z - horizontal_offset) + z_horizontal_offset) + 1.0f) / static_cast<float>(ds->m_unscaled_size.x), (static_cast<float>((m_r.w - vertical_offset) + z_vertical_offset) + 1.0f) / static_cast<float>(ds->m_unscaled_size.y));
					}

					sRect.z = std::min(sRect.z, sRect.x + ((1.0f * ds->m_scale) + (static_cast<float>(m_cached_ctx.FRAME.FBW * 64)) / static_cast<float>(ds->m_unscaled_size.x)));
					dRect.z = std::min(dRect.z, dRect.x + static_cast<int>(1 * ds->m_scale) + static_cast<int>(static_cast<float>(m_cached_ctx.FRAME.FBW * 64) * ds->m_scale));

					GL_CACHE("HW: RT in RT Z copy on draw %lld z_vert_offset %d", s_n, page_offset);

					if (m_cached_ctx.TEST.ZTST > ZTST_ALWAYS || !dRect.rintersect(GSVector4i(GSVector4(m_r) * ds->m_scale)).eq(dRect))
					{
						g_gs_device->StretchRectAuto(ds->m_texture, sRect, tex, GSVector4(dRect), Nearest);
					}
					g_texture_cache->SetTemporaryZ(tex);
					g_texture_cache->SetTemporaryZInfo(ds->m_TEX0.TBP0, page_offset, rt_page_offset);
					t_size.y = std::max(static_cast<int>(new_height / ds->m_scale), t_size.y);
				}
				else
				{
					DevCon.Warning("HW: Temporary depth buffer creation failed.");
					m_using_temp_z = false;
				}
			}
		}

		if (src && src->m_from_target && src->m_target_direct && src->m_from_target == rt)
		{
			src->m_texture = rt->m_texture;
			src->m_scale = rt->GetScale();
			src->m_unscaled_size = rt->m_unscaled_size;
		}

		target_scale = rt->GetScale();

		if (ds && ds->m_scale != target_scale)
		{
			const GSVector2i unscaled_size(ds->m_unscaled_size.x, ds->m_unscaled_size.y);
			ds->m_scale = 1;
			ds->ResizeTexture(ds->m_unscaled_size.x * target_scale, ds->m_unscaled_size.y * target_scale, true, true, GSVector4i::loadh(ds->m_unscaled_size * target_scale));
			ds->m_scale = target_scale;
			ds->m_unscaled_size = unscaled_size;
			ds->m_downscaled = rt->m_downscaled;
		}
		preserve_rt_alpha |= (GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp == 24 && rt->HasValidAlpha());
		preserve_rt_color = preserve_rt_rgb || preserve_rt_alpha;

		if (m_channel_shuffle)
		{
			m_last_channel_shuffle_tbp = src->m_TEX0.TBP0;

			m_last_channel_shuffle_end_block = (rt->m_last_draw >= s_n) ? (GS_MAX_BLOCKS - 1) : (rt->m_end_block < rt->m_TEX0.TBP0 ? (rt->m_end_block + GS_MAX_BLOCKS) : rt->m_end_block);
		}
		else
			m_last_channel_shuffle_end_block = 0xFFFF;
	}

	if (!no_ds && !ds)
	{
		ZBUF_TEX0.U64 = 0;
		ZBUF_TEX0.TBP0 = m_cached_ctx.ZBUF.Block();
		ZBUF_TEX0.TBW = m_cached_ctx.FRAME.FBW;
		ZBUF_TEX0.PSM = m_cached_ctx.ZBUF.PSM;

		ds = g_texture_cache->LookupDrawTarget(ZBUF_TEX0, t_size, target_scale, GSTextureCache::DepthStencil,
			m_cached_ctx.DepthWrite(), 0, force_preload, preserve_depth, preserve_depth, unclamped_draw_rect, IsPossibleChannelShuffle(), is_possible_mem_clear && ZBUF_TEX0.TBP0 != m_cached_ctx.FRAME.Block(), false,
			src, nullptr, -1);

		ZBUF_TEX0.TBW = m_channel_shuffle ? src->m_from_target_TEX0.TBW : m_cached_ctx.FRAME.FBW;

		if (!ds)
		{
			ds = g_texture_cache->CreateTarget(ZBUF_TEX0, t_size, GetValidSize(src, possible_shuffle), target_scale, GSTextureCache::DepthStencil,
				true, 0, false, force_preload, preserve_depth, m_r, src);
			if (!ds) [[unlikely]]
			{
				GL_INS("HW: ERROR: Failed to create ZBUF target, skipping.");
				CleanupDraw(true);
				return;
			}
		}
		else
		{
			if (((zm && m_cached_ctx.TEST.ZTST > ZTST_ALWAYS) || (m_vt.m_eq.z && m_cached_ctx.TEST.ZTST == ZTST_GEQUAL)) && GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].trbpp == 32)
			{
				if (ds->m_alpha_max != 0)
				{
					const u32 max_z = (static_cast<u64>(ds->m_alpha_max + 1) << 24) - 1;

					switch (m_cached_ctx.TEST.ZTST)
					{
						case ZTST_GEQUAL:
							if (max_z <= m_vt.m_min.p.z)
							{
								m_cached_ctx.TEST.ZTST = ZTST_ALWAYS;
								if (zm)
								{
									ds = nullptr;
									no_ds = true;
								}
							}
							break;
						case ZTST_GREATER:
							if (max_z < m_vt.m_min.p.z)
							{
								m_cached_ctx.TEST.ZTST = ZTST_ALWAYS;
								if (zm)
								{
									ds = nullptr;
									no_ds = true;
								}
							}
							break;
						default:
							break;
					}
				}
			}
		}
	}

	DetectTextureShuffleSecondPass(rt, src);

	if (m_process_texture)
	{
		GIFRegCLAMP MIP_CLAMP = m_cached_ctx.CLAMP;

		if (rt)
		{
			if (m_texture_shuffle)
			{
				if (IsSplitTextureShuffle(rt->m_TEX0, rt->m_valid))
				{
					if (m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0)
						g_texture_cache->InvalidateVideoMem(context->offset.fb, m_r, false);

					CleanupDraw(true);
					return;
				}
			}
		}

		if ((src->m_target || (m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0)) && IsPossibleChannelShuffle())
		{
			if (!src->m_target)
			{
				g_texture_cache->ReplaceSourceTexture(src, rt->GetTexture(), rt->GetScale(), rt->GetUnscaledSize(), nullptr, true);
				src->m_from_target = rt;
				src->m_from_target_TEX0 = rt->m_TEX0;
				src->m_target = true;
				src->m_target_direct = true;
				src->m_valid_rect = rt->m_valid;
				src->m_alpha_minmax.first = rt->m_alpha_min;
				src->m_alpha_minmax.second = rt->m_alpha_max;
			}

			GL_INS("HW: Channel shuffle effect detected (2nd shot)");
			m_channel_shuffle = true;
			m_last_channel_shuffle_fbmsk = m_context->FRAME.FBMSK;
			if (rt)
			{
				m_last_channel_shuffle_tbp = src->m_TEX0.TBP0;
				if (!src->m_from_target || GSLocalMemory::m_psm[src->m_from_target_TEX0.PSM].bpp != GSLocalMemory::m_psm[rt->m_TEX0.PSM].bpp)
					m_last_channel_shuffle_end_block = rt->m_end_block;
				else
					m_last_channel_shuffle_end_block = (rt->m_TEX0.TBP0 + (src->m_from_target->m_end_block - src->m_from_target_TEX0.TBP0));

				if (m_last_channel_shuffle_end_block < rt->m_TEX0.TBP0)
					m_last_channel_shuffle_end_block += GS_MAX_BLOCKS;

				if (m_last_channel_shuffle_end_block < rt->m_end_block)
					m_last_channel_shuffle_end_block = rt->m_end_block;
			}
		}
		else
		{
			m_channel_shuffle = false;
		}
#if 0
		if (m_cached_ctx.CLAMP.WMS == CLAMP_REGION_CLAMP && MIP_CLAMP.MINU == 0 && MIP_CLAMP.MAXU == tw - 1)
			m_cached_ctx.CLAMP.WMS = CLAMP_CLAMP;
		else if (m_cached_ctx.CLAMP.WMS == CLAMP_REGION_REPEAT && MIP_CLAMP.MINU == tw - 1 && MIP_CLAMP.MAXU == 0)
			m_cached_ctx.CLAMP.WMS = CLAMP_REPEAT;
		else if ((m_cached_ctx.CLAMP.WMS & 2) && !(tmm.uses_boundary & TextureMinMaxResult::USES_BOUNDARY_U))
			m_cached_ctx.CLAMP.WMS = CLAMP_CLAMP;
		if (m_cached_ctx.CLAMP.WMT == CLAMP_REGION_CLAMP && MIP_CLAMP.MINV == 0 && MIP_CLAMP.MAXV == th - 1)
			m_cached_ctx.CLAMP.WMT = CLAMP_CLAMP;
		else if (m_cached_ctx.CLAMP.WMT == CLAMP_REGION_REPEAT && MIP_CLAMP.MINV == th - 1 && MIP_CLAMP.MAXV == 0)
			m_cached_ctx.CLAMP.WMT = CLAMP_REPEAT;
		else if ((m_cached_ctx.CLAMP.WMT & 2) && !(tmm.uses_boundary & TextureMinMaxResult::USES_BOUNDARY_V))
			m_cached_ctx.CLAMP.WMT = CLAMP_CLAMP;
#endif
		const int tw = 1 << TEX0.TW;
		const int th = 1 << TEX0.TH;
		const bool is_shuffle = m_channel_shuffle || m_texture_shuffle;

		const GSVector2i unscaled_size = src->m_target ? src->GetRegionSize() : src->GetUnscaledSize();

		if (!is_shuffle && m_cached_ctx.CLAMP.WMS == CLAMP_REPEAT && (tmm.uses_boundary & TextureMinMaxResult::USES_BOUNDARY_U) && unscaled_size.x != tw)
		{
			if (unscaled_size.x < tw && m_vt.m_min.t.x > -(tw - unscaled_size.x) && m_vt.m_max.t.x < tw)
			{
				m_cached_ctx.CLAMP.WMS = CLAMP_CLAMP;
			}
			else
			{
				m_cached_ctx.CLAMP.WMS = CLAMP_REGION_REPEAT;
				m_cached_ctx.CLAMP.MINU = (1 << m_cached_ctx.TEX0.TW) - 1;
				m_cached_ctx.CLAMP.MAXU = 0;
			}
		}
		if (!is_shuffle && m_cached_ctx.CLAMP.WMT == CLAMP_REPEAT && (tmm.uses_boundary & TextureMinMaxResult::USES_BOUNDARY_V) && unscaled_size.y != th)
		{
			if (unscaled_size.y < th && m_vt.m_min.t.y > -(th - unscaled_size.y) && m_vt.m_max.t.y < th)
			{
				m_cached_ctx.CLAMP.WMT = CLAMP_CLAMP;
			}
			else
			{
				m_cached_ctx.CLAMP.WMT = CLAMP_REGION_REPEAT;
				m_cached_ctx.CLAMP.MINV = (1 << m_cached_ctx.TEX0.TH) - 1;
				m_cached_ctx.CLAMP.MAXV = 0;
			}
		}

		if (IsMipMapActive() && GSConfig.HWMipmap && !tex_psm.depth && !src->m_from_hash_cache)
		{
			const GSVector4 tmin = m_vt.m_min.t;
			const GSVector4 tmax = m_vt.m_max.t;

			const GSVector4i coverage = tmm.coverage;

			for (int layer = m_lod.x + 1; layer <= m_lod.y; layer++)
			{
				const GIFRegTEX0 MIP_TEX0(GetTex0Layer(layer));

				MIP_CLAMP.MINU >>= 1;
				MIP_CLAMP.MINV >>= 1;
				MIP_CLAMP.MAXU >>= 1;
				MIP_CLAMP.MAXV >>= 1;

				m_vt.m_min.t *= 0.5f;
				m_vt.m_max.t *= 0.5f;

				tmm = GetTextureMinMax(MIP_TEX0, MIP_CLAMP, m_vt.IsLinear(), true);

				src->UpdateLayer(MIP_TEX0, tmm.coverage, layer - m_lod.x);
			}

			src->m_texture->ClearMipmapGenerationFlag();
			m_vt.m_min.t = tmin;
			m_vt.m_max.t = tmax;

			tmm.coverage = coverage;
		}
	}

	if (rt)
	{
		rt->m_32_bits_fmt = m_texture_shuffle || (GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp != 16);
	}

	if (ds)
		ds->m_32_bits_fmt = (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].bpp != 16);

	const bool can_update_size = !is_possible_mem_clear && !m_texture_shuffle && !m_channel_shuffle;

	if (!m_texture_shuffle && !m_channel_shuffle)
	{
		if (m_vt.m_primclass == GS_SPRITE_CLASS && m_primitive_covers_without_gaps == NoGapsType::FullCover && m_index->tail > 2 && (!PRIM->TME || TextureCoversWithoutGapsNotEqual()) && m_vt.m_eq.rgba == 0xFFFF)
		{
			const GSVector2i fb_size = PCRTCDisplays.GetFramebufferSize(-1);
			if (std::abs(fb_size.x - m_r.width()) <= 1 && std::abs(fb_size.y - m_r.height()) <= 1)
			{
				GSVertex* v = m_vertex->buff;

				v[0].XYZ.Z = v[1].XYZ.Z;
				v[0].RGBAQ = v[1].RGBAQ;
				v[0].FOG = v[1].FOG;
				m_vt.m_eq.rgba = 0xFFFF;
				m_vt.m_eq.z = true;
				m_vt.m_eq.f = true;

				v[1].XYZ.X = v[m_index->tail - 1].XYZ.X;
				v[1].XYZ.Y = v[m_index->tail - 1].XYZ.Y;

				if (PRIM->FST)
				{
					v[1].U = v[m_index->tail - 1].U;
					v[1].V = v[m_index->tail - 1].V;
				}
				else
				{
					v[1].ST.S = v[m_index->tail - 1].ST.S;
					v[1].ST.T = v[m_index->tail - 1].ST.T;
					v[1].RGBAQ.Q = v[m_index->tail - 1].RGBAQ.Q;
				}

				m_vertex->head = m_vertex->tail = m_vertex->next = 2;
				m_index->tail = 2;
			}
		}

		const bool blending_cd = NeedsBlending() && !m_context->ALPHA.IsOpaque();
		bool valid_width_change = false;
		if (rt && ((!is_possible_mem_clear || blending_cd) || rt->m_TEX0.PSM != FRAME_TEX0.PSM) && !m_in_target_draw)
		{
			const u32 frame_mask = (m_cached_ctx.FRAME.FBMSK & frame_psm.fmsk);
			valid_width_change = rt->m_TEX0.TBW != FRAME_TEX0.TBW && (frame_mask != (frame_psm.fmsk & 0x00FFFFFF) || rt->m_valid_rgb == false);
			if (valid_width_change && !m_cached_ctx.ZBUF.ZMSK && (m_cached_ctx.FRAME.FBMSK & 0xFF000000))
			{
				if (m_cached_ctx.FRAME.FBMSK & 0x0F000000)
					rt->m_valid_alpha_low = false;
				if (m_cached_ctx.FRAME.FBMSK & 0xF0000000)
					rt->m_valid_alpha_high = false;
			}
			if (FRAME_TEX0.TBW != 1 || (m_r.width() > frame_psm.pgs.x || m_r.height() > frame_psm.pgs.y) || (scale_draw == 1 && !scaled_copy))
			{
				FRAME_TEX0.TBP0 = rt->m_TEX0.TBP0;
				rt->m_TEX0 = FRAME_TEX0;
			}

			if (valid_width_change)
			{
				GSVector4i new_valid_width = rt->m_valid;
				new_valid_width.z = std::min(new_valid_width.z, static_cast<int>(rt->m_TEX0.TBW) * 64);
				rt->ResizeValidity(new_valid_width);
			}
		}

		if (ds && (!is_possible_mem_clear || ds->m_TEX0.PSM != ZBUF_TEX0.PSM || (rt && ds->m_TEX0.TBW != rt->m_TEX0.TBW)) && !m_in_target_draw)
		{
			if (ZBUF_TEX0.TBW != 1 || (m_r.width() > frame_psm.pgs.x || m_r.height() > frame_psm.pgs.y) || (scale_draw == 1 && !scaled_copy))
			{
				ZBUF_TEX0.TBP0 = ds->m_TEX0.TBP0;
				ds->m_TEX0 = ZBUF_TEX0;
			}
			if (valid_width_change)
			{
				GSVector4i new_valid_width = ds->m_valid;
				new_valid_width.z = std::min(new_valid_width.z, static_cast<int>(ds->m_TEX0.TBW) * 64);
				ds->ResizeValidity(new_valid_width);
			}
		}

		if (rt)
			g_texture_cache->CombineAlignedInsideTargets(rt, src);
		if (ds)
			g_texture_cache->CombineAlignedInsideTargets(ds, src);
	}
	else if (!m_texture_shuffle)
	{
		if (rt)
		{
			const bool update_fbw = (FRAME_TEX0.TBW != rt->m_TEX0.TBW || rt->m_TEX0.TBW == 1) && !m_in_target_draw && (m_channel_shuffle && src->m_target) && (!NeedsBlending() || IsOpaque() || m_context->ALPHA.IsBlack());
			rt->m_TEX0.TBW = update_fbw ? ((src && src->m_from_target && src->m_from_target->m_32_bits_fmt) ? src->m_from_target->m_TEX0.TBW : FRAME_TEX0.TBW) : std::max(rt->m_TEX0.TBW, FRAME_TEX0.TBW);
			rt->m_TEX0.PSM = FRAME_TEX0.PSM;
		}
		if (ds)
		{
			ds->m_TEX0.TBW = std::max(ds->m_TEX0.TBW, ZBUF_TEX0.TBW);
			ds->m_TEX0.PSM = ZBUF_TEX0.PSM;
		}
	}

	if (rt)
		rt->UpdateValidChannels(rt->m_TEX0.PSM, m_texture_shuffle ? GetEffectiveTextureShuffleFbmsk() : fm);
	if (ds)
		ds->UpdateValidChannels(ZBUF_TEX0.PSM, zm);

	const GSVector2i resolution = PCRTCDisplays.GetResolution();
	GSTextureCache::Target* old_rt = nullptr;
	GSTextureCache::Target* old_ds = nullptr;

	if (!(m_cached_ctx.TEST.DATE && m_cached_ctx.TEST.DATM))
	{
		GSVector2i new_size = t_size;
		GSVector4i update_rect = m_r;
		const GIFRegTEX0& draw_TEX0 = rt ? rt->m_TEX0 : ds->m_TEX0;
		const int buffer_width = std::max(draw_TEX0.TBW, 1U) * 64;
		if (src && m_texture_shuffle && !m_texture_shuffle.real_16_bit_source)
		{
			if ((new_size.x > src->m_valid_rect.z && m_vt.m_max.p.x == new_size.x) || (new_size.y > src->m_valid_rect.w && m_vt.m_max.p.y == new_size.y))
			{
				if (new_size.y <= src->m_valid_rect.w && (rt->m_TEX0.TBW != m_cached_ctx.FRAME.FBW))
				{
					new_size.x /= 2;
				}
				else
				{
					new_size.y /= 2;
				}
			}

			if (update_rect.z > src->m_valid_rect.z && (rt->m_TEX0.TBW != m_cached_ctx.FRAME.FBW))
			{
				if (update_rect.w > src->m_valid_rect.w)
				{
					update_rect = src->m_valid_rect;
				}
				else
				{
					update_rect.x /= 2;
					update_rect.z /= 2;
				}
			}
			else
			{
				update_rect.y /= 2;
				update_rect.w /= 2;
			}
		}
		else if (m_texture_shuffle && buffer_width > 64 && update_rect.z > buffer_width)
		{
			update_rect.w *= static_cast<float>(update_rect.z) / static_cast<float>(buffer_width);
			update_rect.z = buffer_width;
		}

		GSVector2i ds_size = m_using_temp_z ? GSVector2i(g_texture_cache->GetTemporaryZ()->GetSize() / ds->m_scale) : (ds ? ds->m_unscaled_size : GSVector2i(0,0));

		const int new_w = std::min(2048, std::max(new_size.x, std::max(rt ? rt->m_unscaled_size.x : 0, ds ? ds_size.x : 0)));
		const int new_h = std::min(2048, std::max(new_size.y, std::max(rt ? rt->m_unscaled_size.y : 0, ds ? ds_size.y : 0)));

		const bool full_cover_clear = is_possible_mem_clear && GSLocalMemory::IsPageAligned(m_cached_ctx.FRAME.PSM, m_r) && m_r.x == 0 && m_r.y == 0 && !preserve_rt_rgb &&
									  !IsPageCopy() && m_r.width() == (m_cached_ctx.FRAME.FBW * 64);

		if (rt)
		{
			const u32 old_end_block = rt->m_end_block;
			const bool new_rect = rt->m_valid.rempty();
			const bool new_height = new_h > rt->GetUnscaledHeight();
			const int old_height = rt->m_texture->GetHeight();
			bool merge_targets = false;
			pxAssert(rt->GetScale() == target_scale);
			if (rt->GetUnscaledWidth() != new_w || rt->GetUnscaledHeight() != new_h)
				GL_INS("HW: Resize RT from %dx%d to %dx%d", rt->GetUnscaledWidth(), rt->GetUnscaledHeight(), new_w, new_h);

			if ((new_w > rt->m_unscaled_size.x || new_h > rt->m_unscaled_size.y) && GSConfig.UserHacks_TextureInsideRt >= GSTextureInRtMode::InsideTargets)
				merge_targets = true;

			rt->ResizeTexture(new_w, new_h);

			if (!m_texture_shuffle && !m_channel_shuffle)
			{
				if (rt->m_drawn_since_read.rempty() && rt->m_dirty.size() > 0 && new_height && (preserve_rt_color || preserve_rt_alpha))
				{
					RGBAMask mask;
					mask._u32 = preserve_rt_color ? 0x7 : 0;
					mask.c.a |= preserve_rt_alpha;
					g_texture_cache->AddDirtyRectTarget(rt, GSVector4i(rt->m_valid.x, rt->m_valid.w, rt->m_valid.z, new_h), rt->m_TEX0.PSM, rt->m_TEX0.TBW, mask, false);
					g_texture_cache->GetTargetSize(rt->m_TEX0.TBP0, rt->m_TEX0.TBW, rt->m_TEX0.PSM, 0, new_h);
				}
				const bool rt_cover = full_cover_clear && (m_r.height() + frame_psm.pgs.y) >= rt->m_valid.height();
				rt->ResizeValidity(rt_cover ? update_rect : rt->m_valid.rintersect(rt->GetUnscaledRect()));
				rt->ResizeDrawn(rt_cover ? update_rect : rt->m_drawn_since_read.rintersect(rt->GetUnscaledRect()));
			}

			const bool rt_update = can_update_size || (is_possible_mem_clear && m_vt.m_min.c.a > 0) || (m_texture_shuffle && (src && src->m_from_target != rt));

			if (rt_update && !can_update_size)
			{
				if (src && src->m_from_target)
					update_rect = update_rect.rintersect(src->m_from_target->m_valid);

				update_rect = update_rect.rintersect(GSVector4i::loadh(GSVector2i(new_w, new_h)));
			}

			const bool frame_masked = ((m_cached_ctx.FRAME.FBMSK & frame_psm.fmsk) == frame_psm.fmsk) || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ATST_NEVER && !(m_cached_ctx.TEST.AFAIL & AFAIL_FB_ONLY));
			rt->UpdateValidity(update_rect, !frame_masked && (rt_update || (m_r.w <= (resolution.y * 2) && !m_texture_shuffle)));
			rt->UpdateDrawn(update_rect, !frame_masked && (rt_update || (m_r.w <= (resolution.y * 2) && !m_texture_shuffle)));

			if (merge_targets)
				g_texture_cache->CombineAlignedInsideTargets(rt, src);
			if (!new_rect && new_height && old_end_block != rt->m_end_block)
			{
				old_rt = g_texture_cache->FindTargetOverlap(rt, GSTextureCache::RenderTarget, m_cached_ctx.FRAME.PSM);

				if (old_rt && old_rt != rt && GSUtil::HasSharedBits(old_rt->m_TEX0.PSM, rt->m_TEX0.PSM))
				{
					const int copy_width = (old_rt->m_texture->GetWidth()) > (rt->m_texture->GetWidth()) ? (rt->m_texture->GetWidth()) : old_rt->m_texture->GetWidth();
					const int copy_height = (old_rt->m_texture->GetHeight()) > (rt->m_texture->GetHeight() - old_height) ? (rt->m_texture->GetHeight() - old_height) : old_rt->m_texture->GetHeight();
					GL_INS("HW: RT double buffer copy from FBP 0x%x, %dx%d => %d,%d", old_rt->m_TEX0.TBP0, copy_width, copy_height, 0, old_height);

					g_gs_device->CopyRect(old_rt->m_texture, rt->m_texture, GSVector4i(0, 0, copy_width, copy_height), 0, old_height);
					preserve_rt_color = true;
				}
				else
				{
					old_rt = nullptr;
				}
			}
		}
		if (ds)
		{
			const u32 old_end_block = ds->m_end_block;
			const bool new_rect = ds->m_valid.rempty();
			const bool new_height = new_h > ds->GetUnscaledHeight();
			const int old_height = ds->m_texture->GetHeight();

			pxAssert(ds->GetScale() == target_scale);
			if (ds->GetUnscaledWidth() != new_w || ds->GetUnscaledHeight() != new_h)
				GL_INS("HW: Resize DS from %dx%d to %dx%d", ds->GetUnscaledWidth(), ds->GetUnscaledHeight(), new_w, new_h);

			ds->ResizeTexture(new_w, new_h);


			if (m_using_temp_z)
			{
				const int z_width = g_texture_cache->GetTemporaryZ()->GetWidth() / ds->m_scale;
				const int z_height = g_texture_cache->GetTemporaryZ()->GetHeight() / ds->m_scale;

				if (z_width != new_w || z_height != new_h)
				{
					if (GSTexture* tex = g_gs_device->CreateDepthStencil(new_w * ds->m_scale, new_h * ds->m_scale, true))
					{
						g_gs_device->StretchRectAuto(g_texture_cache->GetTemporaryZ(), tex, Nearest);
						g_texture_cache->InvalidateTemporaryZ();
						g_texture_cache->SetTemporaryZ(tex);
					}
					else
						DevCon.Warning("HW: Temporary depth buffer creation failed.");
				}
			}
			const bool z_masked = m_cached_ctx.ZBUF.ZMSK;

			if (!m_texture_shuffle && !m_channel_shuffle)
			{
				const bool z_cover = full_cover_clear && (m_r.height() + GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.y) >= ds->m_valid.height();
				ds->ResizeValidity(z_cover ? m_r : ds->GetUnscaledRect());
				ds->ResizeDrawn(z_cover ? m_r : ds->GetUnscaledRect());
			}

			const bool z_update = (can_update_size || (is_possible_mem_clear && m_vt.m_min.p.z > 0)) && !z_masked;

			if (rt && m_using_temp_z)
			{
				const GSLocalMemory::psm_t& z_psm = GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM];
				const int vertical_offset = ((static_cast<int>(m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) / 32) / std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * frame_psm.pgs.y;
				const int z_vertical_offset = ((static_cast<int>(m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0) / 32) / std::max(static_cast<int>(ds->m_TEX0.TBW), 1)) * z_psm.pgs.y;
				const int z_horizontal_offset = ((static_cast<int>(m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0) / 32) % std::max(rt->m_TEX0.TBW, 1U)) * z_psm.pgs.x;
				const int horizontal_offset = ((static_cast<int>(m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) / 32) % std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * frame_psm.pgs.x;
				
				const GSVector4i ds_rect = m_r - GSVector4i(horizontal_offset - z_horizontal_offset, vertical_offset - z_vertical_offset).xyxy();
				ds->UpdateValidity(ds_rect, z_update && (can_update_size || (ds_rect.w <= (resolution.y * 2) && !m_texture_shuffle)));
				ds->UpdateDrawn(ds_rect, z_update && (can_update_size || (ds_rect.w <= (resolution.y * 2) && !m_texture_shuffle)));
			}
			else
			{
				ds->UpdateValidity(m_r, z_update && (can_update_size || m_r.w <= (resolution.y * 2)));
				ds->UpdateDrawn(m_r, z_update && (can_update_size || m_r.w <= (resolution.y * 2)));
			}

			if (!new_rect && new_height && old_end_block != ds->m_end_block)
			{
				old_ds = g_texture_cache->FindTargetOverlap(ds, GSTextureCache::DepthStencil, m_cached_ctx.ZBUF.PSM);

				if (old_ds && old_ds != ds && GSUtil::HasSharedBits(old_ds->m_TEX0.PSM, ds->m_TEX0.PSM))
				{
					const int copy_width = (old_ds->m_texture->GetWidth()) > (ds->m_texture->GetWidth()) ? (ds->m_texture->GetWidth()) : old_ds->m_texture->GetWidth();
					const int copy_height = (old_ds->m_texture->GetHeight()) > (ds->m_texture->GetHeight() - old_height) ? (ds->m_texture->GetHeight() - old_height) : old_ds->m_texture->GetHeight();
					GL_INS("HW: DS double buffer copy from FBP 0x%x, %dx%d => %d,%d", old_ds->m_TEX0.TBP0, copy_width, copy_height, 0, old_height);

					g_gs_device->CopyRect(old_ds->m_texture, ds->m_texture, GSVector4i(0, 0, copy_width, copy_height), 0, old_height);
					preserve_depth = true;
				}
				else
				{
					old_ds = nullptr;
				}
			}
		}
	}
	else
	{
		const int new_w = std::max(rt ? rt->m_unscaled_size.x : 0, ds ? ds->m_unscaled_size.x : 0);
		const int new_h = std::max(rt ? rt->m_unscaled_size.y : 0, ds ? ds->m_unscaled_size.y : 0);
		if (rt)
			rt->ResizeTexture(new_w, new_h);
		if (ds)
			ds->ResizeTexture(new_w, new_h);
	}

	if (!m_texture_shuffle && !m_channel_shuffle && rt && src && src->m_from_target == rt && src->m_target_direct && rt->m_texture == src->m_texture)
	{
		if (GSLocalMemory::m_psm[src->m_from_target_TEX0.PSM].bpp != (GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp))
		{
			GSVector2i new_size = src->m_from_target->m_unscaled_size;

			if (GSLocalMemory::m_psm[src->m_from_target->m_TEX0.PSM].bpp == 32)
				new_size.y *= 2;
			else
				new_size.y /= 2;

			const GSVector4i dRect = GSVector4i(GSVector4(GSVector4i(0, 0, new_size.x, new_size.y)) * rt->m_scale);
			const GSVector2i old_unscaled = rt->m_unscaled_size;
			rt->ResizeTexture(new_size.x, new_size.y, false, true, dRect, true);

			if (GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp >= 16)
			{
				GSTexture* new_tex = rt->m_texture;
				rt->m_texture = src->m_texture;
				rt->m_unscaled_size = old_unscaled;
				src->m_target_direct = false;
				src->m_shared_texture = false;
				src->m_texture = new_tex;
				src->m_unscaled_size = new_size;
				src->m_TEX0.PSM = m_cached_ctx.TEX0.PSM;
			}
		}
	}
	bool skip_draw = false;
	if (!GSConfig.UserHacks_DisableSafeFeatures && is_possible_mem_clear)
		skip_draw = TryTargetClear(rt, ds, preserve_rt_color, preserve_depth);

	if (rt)
	{
		if (rt->m_last_draw >= s_n || m_texture_shuffle || m_channel_shuffle || (!rt->m_dirty.empty() && !rt->m_dirty.GetTotalRect(rt->m_TEX0, rt->m_unscaled_size).rintersect(m_r).rempty()))
		{
			const u32 alpha = m_cached_ctx.FRAME.FBMSK >> 24;
			const u32 alpha_mask = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk >> 24;
			rt->Update(m_texture_shuffle || (alpha != 0 && (alpha & alpha_mask) != alpha_mask) || (!alpha && (GetAlphaMinMax().max | (m_context->FBA.FBA << 7)) > 128));
		}
		else
			rt->m_age = 0;
	}
	if (ds)
	{
		if (ds->m_last_draw >= s_n || m_texture_shuffle || m_channel_shuffle || (!ds->m_dirty.empty() && !ds->m_dirty.GetTotalRect(ds->m_TEX0, ds->m_unscaled_size).rintersect(m_r).rempty()))
			ds->Update();
		else
			ds->m_age = 0;
	}

	if (src && src->m_shared_texture && src->m_texture != src->m_from_target->m_texture)
	{
		src->m_texture = src->m_from_target->m_texture;
	}

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		const u64 frame = g_perfmon.GetFrame();

		std::string s;

		if (GSConfig.SaveTexture && src)
		{
			s = GetDrawDumpPath("%05lld_f%05lld_itex_%s_%05x(%05x)_%s_%d%d_%02x_%02x_%02x_%02x.dds",
				s_n, frame, (src->m_from_target ? "tgt" : "gs"), static_cast<int>(m_cached_ctx.TEX0.TBP0), (src->m_from_target ? src->m_from_target->m_TEX0.TBP0 : src->m_TEX0.TBP0), GSUtil::GetPSMName(m_cached_ctx.TEX0.PSM),
				static_cast<int>(m_cached_ctx.CLAMP.WMS), static_cast<int>(m_cached_ctx.CLAMP.WMT),
				static_cast<int>(m_cached_ctx.CLAMP.MINU), static_cast<int>(m_cached_ctx.CLAMP.MAXU),
				static_cast<int>(m_cached_ctx.CLAMP.MINV), static_cast<int>(m_cached_ctx.CLAMP.MAXV));

			src->m_texture->Save(s);

			if (src->m_palette)
			{
				s = GetDrawDumpPath("%05lld_f%05lld_itpx_%05x_%s.dds", s_n, frame, m_cached_ctx.TEX0.CBP, GSUtil::GetPSMName(m_cached_ctx.TEX0.CPSM));

				src->m_palette->Save(s);
			}
		}

		if (rt && GSConfig.SaveRT)
		{
			s = GetDrawDumpPath("%05lld_f%05lld_rt0_%05x_(%05x)_%s.bmp", s_n, frame, m_cached_ctx.FRAME.Block(), rt->m_TEX0.TBP0, GSUtil::GetPSMName(m_cached_ctx.FRAME.PSM));

			if (rt->m_texture)
			{
				GSTexture* save_tex = rt->m_texture;
#ifdef ENABLE_VR
				if (save_tex->GetArrayLayers() > 1)
				{
					static const char* layer_env = std::getenv("PCSX2_VR_SNAPSHOT_LAYER");
					if (layer_env && layer_env[0] == '1')
						save_tex = save_tex->GetLayerProxyTexture(1);
				}
#endif
				save_tex->Save(s);
			}
		}

		if (ds && GSConfig.SaveDepth)
		{
			s = GetDrawDumpPath("%05lld_f%05lld_rz0_%05x_(%05x)_%s.bmp", s_n, frame, m_cached_ctx.ZBUF.Block(), ds->m_TEX0.TBP0, GSUtil::GetPSMName(m_cached_ctx.ZBUF.PSM));

			if (m_using_temp_z)
				g_texture_cache->GetTemporaryZ()->Save(s);
			else if (ds->m_texture)
				ds->m_texture->Save(s);
		}
	}

	if (m_oi && !m_oi(*this, rt ? rt->m_texture : nullptr, ds ? ds->m_texture : nullptr, src))
	{
		GL_INS("HW: Warning skipping a draw call (%lld)", s_n);
		CleanupDraw(true);
		return;
	}

	if (!OI_BlitFMV(rt, src, m_r))
	{
		GL_INS("HW: Warning skipping a draw call (%lld)", s_n);
		CleanupDraw(true);
		return;
	}

	if (CanUpscale() && (m_vt.m_primclass == GS_SPRITE_CLASS) && rt && rt->GetScale() > 1.0f)
	{
		const u32 count = m_vertex->next;
		GSVertex* v = &m_vertex->buff[0];

		if (GSConfig.UserHacks_AlignSpriteX)
		{
			const int win_position = v[1].XYZ.X - context->XYOFFSET.OFX;
			const bool unaligned_position = ((win_position & 0xF) == 8);
			const bool unaligned_texture = ((v[1].U & 0xF) == 0) && PRIM->FST;
			const bool hole_in_vertex = (count < 4) || (v[1].XYZ.X != v[2].XYZ.X);
			if (hole_in_vertex && unaligned_position && (unaligned_texture || !PRIM->FST))
			{
				for (u32 i = 0; i < count; i += 2)
				{
					v[i + 1].XYZ.X += 8;
					if (unaligned_texture)
						v[i + 1].U += 8;
				}
			}
		}

		if (PRIM->FST && draw_sprite_tex && m_process_texture)
		{
			if ((GSConfig.UserHacks_RoundSprite > 1) || (GSConfig.UserHacks_RoundSprite == 1 && !m_vt.IsLinear()))
			{
				if (m_vt.IsLinear())
					RoundSpriteOffset<true>();
				else
					RoundSpriteOffset<false>();
			}
		}
		else
		{
			;
		}
	}

	const GSVector4i real_rect = m_r;

	if (!skip_draw)
		DrawPrims(rt, ds, src, tmm);


	g_texture_cache->InvalidateTemporarySource();

	if (old_rt)
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, old_rt->m_TEX0.TBP0);
	if (old_ds)
		g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, old_ds->m_TEX0.TBP0);

	if ((fm & fm_mask) != fm_mask && rt)
	{
		const bool frame_masked = ((m_cached_ctx.FRAME.FBMSK & frame_psm.fmsk) == frame_psm.fmsk) || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ATST_NEVER && !(m_cached_ctx.TEST.AFAIL & AFAIL_FB_ONLY));
		rt->UpdateValidity(real_rect, !frame_masked && (can_update_size || (real_rect.w <= (resolution.y * 2) && !m_texture_shuffle)));

		if (m_channel_shuffle)
		{
			m_last_channel_shuffle_fbp = rt->m_TEX0.TBP0;
		}
	}

	if (ds)
	{
		const bool z_masked = m_cached_ctx.ZBUF.ZMSK;
		const bool was_written = zm != 0xffffffff && m_cached_ctx.DepthWrite();

		if (m_using_temp_z)
		{
			const int get_next_ctx = m_env.PRIM.CTXT;
			const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];
			const int z_vertical_offset = ((static_cast<int>(m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0) / 32) / std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.y;
			const int z_horizontal_offset = ((static_cast<int>(m_cached_ctx.ZBUF.Block() - ds->m_TEX0.TBP0) / 32) % std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].pgs.x;
			const int vertical_offset = ((static_cast<int>(m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) / 32) / std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * frame_psm.pgs.y;
			const int horizontal_offset = ((static_cast<int>(m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) / 32) % std::max(static_cast<int>(rt->m_TEX0.TBW), 1)) * frame_psm.pgs.x;

			if (was_written)
			{
				const GSVector4i ds_real_rect = real_rect - GSVector4i(horizontal_offset - z_horizontal_offset, vertical_offset - z_vertical_offset).xyxy();
				ds->UpdateValidity(ds_real_rect, !z_masked && (can_update_size || (ds_real_rect.w <= (resolution.y * 2) && !m_texture_shuffle)));
			}

			if (((m_state_flush_reason != CONTEXTCHANGE) || (next_ctx.ZBUF.ZBP == m_context->ZBUF.ZBP && next_ctx.FRAME.FBP == m_context->FRAME.FBP)) && !(m_channel_shuffle && !IsPageCopy()))
			{
				m_temp_z_full_copy |= was_written;
			}
			else
			{
				if (!m_temp_z_full_copy && was_written)
				{
					GSVector4i dRect = GSVector4i((z_horizontal_offset + (real_rect.x - horizontal_offset)) * ds->m_scale, (z_vertical_offset + (real_rect.y - vertical_offset)) * ds->m_scale, ((z_horizontal_offset + real_rect.z + (1.0f / ds->m_scale)) - horizontal_offset) * ds->m_scale, (z_vertical_offset + (real_rect.w + (1.0f / ds->m_scale) - vertical_offset)) * ds->m_scale);
					GSVector4 sRect = GSVector4(
						(real_rect.x * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetWidth()),
						static_cast<float>(real_rect.y * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetHeight()),
						((real_rect.z + (1.0f / ds->m_scale)) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetWidth()),
						static_cast<float>((real_rect.w + (1.0f / ds->m_scale)) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetHeight()));

					GL_CACHE("HW: RT in RT Z copy back draw %lld z_vert_offset %d rt_vert_offset %d z_horz_offset %d rt_horz_offset %d", s_n, z_vertical_offset, vertical_offset, z_horizontal_offset, horizontal_offset);
					g_gs_device->StretchRectAuto(g_texture_cache->GetTemporaryZ(), sRect, ds->m_texture, GSVector4(dRect), Nearest);
				}
				else if (m_temp_z_full_copy)
				{
					GSVector4i dRect = GSVector4i((ds->m_valid.x + z_horizontal_offset) * ds->m_scale, (ds->m_valid.y + z_vertical_offset) * ds->m_scale, (ds->m_valid.z + z_horizontal_offset + (1.0f / ds->m_scale)) * ds->m_scale, (ds->m_valid.w + z_vertical_offset + (1.0f / ds->m_scale)) * ds->m_scale);
					GSVector4 sRect = GSVector4(
						((ds->m_valid.x + horizontal_offset) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetWidth()),
						static_cast<float>((ds->m_valid.y + vertical_offset) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetHeight()),
						(((ds->m_valid.z + horizontal_offset) + (1.0f / ds->m_scale)) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetWidth()),
						static_cast<float>((ds->m_valid.w + vertical_offset + (1.0f / ds->m_scale)) * ds->m_scale) / static_cast<float>(g_texture_cache->GetTemporaryZ()->GetHeight()));

					GL_CACHE("HW: RT in RT Z copy back draw %lld z_vert_offset %d z_offset %d", s_n, z_vertical_offset, vertical_offset);
					g_gs_device->StretchRectAuto(g_texture_cache->GetTemporaryZ(), sRect, ds->m_texture, GSVector4(dRect), Nearest);
				}

				m_temp_z_full_copy = false;
			}
		}
		else if (was_written && g_texture_cache->GetTemporaryZ() != nullptr)
		{
			ds->UpdateValidity(real_rect, !z_masked && (can_update_size || (real_rect.w <= (resolution.y * 2) && !m_texture_shuffle)));
			ds->UpdateDrawn(real_rect, !z_masked && (can_update_size || (real_rect.w <= (resolution.y * 2) && !m_texture_shuffle)));

			GSTextureCache::TempZAddress z_address_info = g_texture_cache->GetTemporaryZInfo();
			if (ds->m_TEX0.TBP0 == z_address_info.ZBP)
			{
				if (z_address_info.rect_since.rempty())
					z_address_info.rect_since = real_rect;
				else
					z_address_info.rect_since = z_address_info.rect_since.runion(real_rect);
				g_texture_cache->SetTemporaryZInfo(z_address_info);
			}
		}
	}

	if (GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		const bool writeback_colclip_texture = g_gs_device->GetColorClipTexture() != nullptr;
		if (writeback_colclip_texture)
		{
			GSTexture* colclip_texture = g_gs_device->GetColorClipTexture();
			const GSVector4 colclip_texture_dims = GSVector4(GSVector4i(colclip_texture->GetSize()).xyxy());
			g_gs_device->StretchRect(
				colclip_texture, GSVector4(m_conf.colclip_update_area) / colclip_texture_dims,
				rt->m_texture, GSVector4(m_conf.colclip_update_area),
				ShaderConvert::COLCLIP_RESOLVE, Nearest);
		}

		const u64 frame = g_perfmon.GetFrame();

		std::string s;

		if (rt && GSConfig.SaveRT && !m_last_rt)
		{
			s = GetDrawDumpPath("%05lld_f%05lld_rt1_%05x_(%05x)_%s.bmp", s_n, frame, m_cached_ctx.FRAME.Block(), rt->m_TEX0.TBP0, GSUtil::GetPSMName(m_cached_ctx.FRAME.PSM));

			GSTexture* save_tex = rt->m_texture;
#ifdef ENABLE_VR
			if (save_tex->GetArrayLayers() > 1)
			{
				static const char* layer_env = std::getenv("PCSX2_VR_SNAPSHOT_LAYER");
				if (layer_env && layer_env[0] == '1')
					save_tex = save_tex->GetLayerProxyTexture(1);
			}
#endif
			save_tex->Save(s);
		}

		if (ds && GSConfig.SaveDepth)
		{
			s = GetDrawDumpPath("%05lld_f%05lld_rz1_%05x_%s.bmp", s_n, frame, m_cached_ctx.ZBUF.Block(), GSUtil::GetPSMName(m_cached_ctx.ZBUF.PSM));

			if (m_using_temp_z)
				g_texture_cache->GetTemporaryZ()->Save(s);
			else
				ds->m_texture->Save(s);
		}
	}

	if (rt)
		rt->m_last_draw = s_n;

	if (ds)
		ds->m_last_draw = s_n;

	if ((fm & fm_mask) != fm_mask && !no_rt)
	{
		if (m_mem.m_clut.GetGPUTexture() && m_mem.m_clut.GetGPUTexture() == rt->m_texture)
			m_mem.m_clut.SetGPUTextureDirty(rt->m_last_draw, rt->m_texture);

		g_texture_cache->InvalidateVideoMem(context->offset.fb, real_rect, false);

		g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, m_cached_ctx.FRAME.Block(),
			m_cached_ctx.FRAME.PSM, m_texture_shuffle ? GetEffectiveTextureShuffleFbmsk() : fm);

		if (rt && !m_using_temp_z && g_texture_cache->GetTemporaryZ() != nullptr)
		{
			GSTextureCache::TempZAddress temp_z_info = g_texture_cache->GetTemporaryZInfo();
			if (GSLocalMemory::GetStartBlockAddress(rt->m_TEX0.TBP0, rt->m_TEX0.TBW, rt->m_TEX0.PSM, real_rect) <= temp_z_info.ZBP && GSLocalMemory::GetEndBlockAddress(rt->m_TEX0.TBP0, rt->m_TEX0.TBW, rt->m_TEX0.PSM, real_rect) > temp_z_info.ZBP)
				g_texture_cache->InvalidateTemporaryZ();
		}
	}

	if (zm != 0xffffffff && !no_ds)
	{
		g_texture_cache->InvalidateVideoMem(context->offset.zb, real_rect, false);

		g_texture_cache->InvalidateVideoMemType(
			GSTextureCache::RenderTarget, m_cached_ctx.ZBUF.Block(), m_cached_ctx.ZBUF.PSM, zm);
	}
#ifdef DISABLE_HW_TEXTURE_CACHE
	if (rt)
		g_texture_cache->Read(rt, real_rect);
#endif

	CleanupDraw(false);
}

bool GSRendererHW::VerifyIndices()
{
	switch (m_vt.m_primclass)
	{
		case GS_SPRITE_CLASS:
			if (m_index->tail % 2 != 0)
				return false;
			[[fallthrough]];
		case GS_POINT_CLASS:
			for (u32 i = 0; i < m_index->tail; i++)
			{
				if (m_index->buff[i] != i)
					return false;
			}
			break;
		case GS_LINE_CLASS:
			if (m_index->tail % 2 != 0)
				return false;
			for (u32 i = 0; i < m_index->tail; i += 2)
			{
				if (m_index->buff[i] + 1 != m_index->buff[i + 1])
					return false;
			}
			break;
		case GS_TRIANGLE_CLASS:
			if (m_index->tail % 3 != 0)
				return false;
			break;
		case GS_INVALID_CLASS:
			break;
	}
	return true;
}

void GSRendererHW::HandleFlatShadedVertices()
{
	const bool maybe_fix_vertices = !m_conf.vs.iip &&
		(!g_gs_device->Features().provoking_vertex_last || IsCoverageAlphaSupported());

	const bool dont_fix_vertices = m_vt.m_primclass == GS_POINT_CLASS || m_vt.m_primclass == GS_SPRITE_CLASS;

	if (!maybe_fix_vertices || dont_fix_vertices)
		return;

	const int n = GSUtil::GetClassVertexCount(m_vt.m_primclass);

	bool prims_flat = true;
	for (u32 i = 0; i < m_index->tail; i += n)
	{
		for (u32 j = 0; j < n - 1; j++)
		{
			if (m_vertex->buff[m_index->buff[i + j]].RGBAQ.U32[0] != m_vertex->buff[m_index->buff[i + n - 1]].RGBAQ.U32[0])
			{
				prims_flat = false;
				break;
			}
		}
		if (!prims_flat)
			break;
	}
	if (prims_flat)
		return;

	while (m_vertex->maxcount < m_index->tail)
		GrowVertexBuffer();
	for (int i = static_cast<int>(m_index->tail) - 1; i >= 0; i--)
	{
		m_vertex->buff_copy[i] = m_vertex->buff[m_index->buff[i]];
		m_index->buff[i] = static_cast<u16>(i);
	}
	std::swap(m_vertex->buff, m_vertex->buff_copy);
	m_vertex->head = m_vertex->next = m_vertex->tail = m_index->tail;

	for (u32 i = 0; i < m_index->tail; i += n)
	{
		for (u32 j = 0; j < n - 1; j++)
			m_vertex->buff[i + j].RGBAQ.U32[0] = m_vertex->buff[i + n - 1].RGBAQ.U32[0];
	}
}

void GSRendererHW::SetupIA(float target_scale, float sx, float sy, bool req_vert_backup, const bool no_rt)
{
	GL_PUSH("HW: IA");

	if (GSConfig.UserHacks_ForceEvenSpritePosition && !m_isPackedUV_HackFlag && m_process_texture && PRIM->FST &&
		!m_texture_shuffle)
	{
		for (u32 i = 0; i < m_vertex->next; i++)
			m_vertex->buff[i].UV &= 0x3FEF3FEF;
	}

	const bool unscale_pt_ln = !GSConfig.UserHacks_DisableSafeFeatures && (target_scale != 1.0f);
	const GSDevice::FeatureSupport features = g_gs_device->Features();
	const bool draw_aa1 = !no_rt && PRIM->AA1 && features.aa1;

	pxAssert(VerifyIndices());

	switch (m_vt.m_primclass)
	{
		case GS_POINT_CLASS:
			{
				m_conf.topology = GSHWDrawConfig::Topology::Point;
				m_conf.indices_per_prim = 1;
				if (unscale_pt_ln)
				{
					if (features.point_expand)
					{
						m_conf.vs.point_size = true;
						m_conf.cb_vs.point_size = GSVector2(target_scale);
					}
					else if (features.vs_expand)
					{
						m_conf.vs.expand = GSHWDrawConfig::VSExpand::Point;
						m_conf.cb_vs.point_size = GSVector2(16.0f * sx, 16.0f * sy);
						m_conf.topology = GSHWDrawConfig::Topology::Triangle;
						m_conf.verts = m_vertex->buff;
						m_conf.nverts = m_vertex->next;
						m_conf.nindices = m_index->tail * 6;
						m_conf.indices_per_prim = 6;
						return;
					}
				}
				else
				{
					m_conf.cb_vs.point_size = target_scale;

					m_conf.vs.point_size = true;
				}
			}
			break;

		case GS_LINE_CLASS:
			{
				m_conf.topology = GSHWDrawConfig::Topology::Line;
				m_conf.indices_per_prim = 2;
				if (draw_aa1)
				{
					GL_INS("HW: AA1 line expand.");
					m_conf.vs.expand = GSHWDrawConfig::VSExpand::LineAA1;
					m_conf.cb_vs.point_size = GSVector2(16.0f * sx, 16.0f * sy);

					if (target_scale == 1.0f)
					{
						m_conf.cb_vs.line_aa1_width = 1.0f;
						m_conf.cb_ps.LineCovScale = 1.0f;
					}
					else
					{
						constexpr float half_native_px = 0.5f;
						const float upscaled_px = 1.0f / target_scale;

						m_conf.cb_vs.line_aa1_width = half_native_px + upscaled_px;

						m_conf.cb_ps.LineCovScale = (half_native_px + upscaled_px) / (2 * upscaled_px); 
					}

					m_conf.topology = GSHWDrawConfig::Topology::Triangle;
					m_conf.indices_per_prim = 6;
					ExpandLineIndices();
				}
				else if (unscale_pt_ln)
				{
					if (features.line_expand)
					{
						m_conf.line_expand = true;
					}
					else if (features.vs_expand)
					{
						m_conf.vs.expand = GSHWDrawConfig::VSExpand::Line;
						m_conf.cb_vs.point_size = GSVector2(16.0f * sx, 16.0f * sy);
						m_conf.topology = GSHWDrawConfig::Topology::Triangle;
						m_conf.indices_per_prim = 6;
						ExpandLineIndices();
					}
				}
			}
			break;

		case GS_SPRITE_CLASS:
			{
				if (features.vs_expand && !m_vt.m_accurate_stq)
				{
					m_conf.topology = GSHWDrawConfig::Topology::Triangle;
					m_conf.vs.expand = GSHWDrawConfig::VSExpand::Sprite;

					if (req_vert_backup)
					{
						memcpy(m_draw_vertex.buff, m_vertex->buff, sizeof(GSVertex) * m_vertex->next);
						memcpy(m_draw_index.buff, m_index->buff, sizeof(u16) * m_index->tail);

						m_conf.verts = m_draw_vertex.buff;
						m_conf.indices = m_draw_index.buff;
					}
					else
					{
						m_conf.verts = m_vertex->buff;
						m_conf.indices = m_index->buff;
					}
					m_conf.nverts = m_vertex->next;
					m_conf.nindices = m_index->tail * 3;
					m_conf.indices_per_prim = 6;
					return;
				}
				else
				{
					Lines2Sprites();

					m_conf.topology = GSHWDrawConfig::Topology::Triangle;
					m_conf.indices_per_prim = 6;
				}
			}
			break;

		case GS_TRIANGLE_CLASS:
			{
				if (draw_aa1)
				{
					GL_INS("HW: AA1 triangle expand.");
					m_conf.vs.expand = GSHWDrawConfig::VSExpand::TriangleAA1;
					m_conf.cb_vs.point_size = GSVector2(16.0f * sx, 16.0f * sy);
					m_conf.topology = GSHWDrawConfig::Topology::Triangle;
					m_conf.indices_per_prim = 3;
				}
				else
				{
					m_conf.topology = GSHWDrawConfig::Topology::Triangle;
					m_conf.indices_per_prim = 3;
				}

				if (m_vt.m_accurate_stq && m_vt.m_eq.stq) [[unlikely]]
				{
					GSVertex* const v = m_vertex->buff;
					const GSVector4 v_q = GSVector4(v[0].RGBAQ.Q);
					for (u32 i = 0; i < m_vertex->next; i++)
					{
						GSVector4 v_st = GSVector4::load<true>(&v[i].ST);
						v_st = (v_st / v_q).insert32<2, 2>(v_st);
						GSVector4::store<true>(&v[i].ST, v_st);
					}
				}
			}
			break;

		default:
			ASSUME(0);
	}

	if (req_vert_backup)
	{
		memcpy(m_draw_vertex.buff, m_vertex->buff, sizeof(GSVertex) * m_vertex->next);
		memcpy(m_draw_index.buff, m_index->buff, sizeof(u16) * m_index->tail);

		m_conf.verts = m_draw_vertex.buff;
		m_conf.indices = m_draw_index.buff;
	}
	else
	{
		m_conf.verts = m_vertex->buff;
		m_conf.indices = m_index->buff;
	}
	m_conf.nverts = m_vertex->next;
	m_conf.nindices = m_index->tail;
}

void GSRendererHW::EmulateZbuffer(const GSTextureCache::Target* ds)
{
	if (ds && m_cached_ctx.TEST.ZTE)
	{
		m_conf.depth.ztst = m_cached_ctx.TEST.ZTST;
		if (m_cached_ctx.ZBUF.ZMSK || (PRIM->AA1 && m_vt.m_primclass == GS_LINE_CLASS))
		{
			m_conf.depth.zwe = false;
			m_cached_ctx.ZBUF.ZMSK = true;
		}
		else
		{
			m_conf.depth.zwe = true;
		}
	}
	else
	{
		m_conf.depth.ztst = ZTST_ALWAYS;
	}

	const u32 max_z = 0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8);
	const bool large_z = static_cast<u32>(GSVector4i(m_vt.m_max.p).z) > max_z;

	const bool flat_z = m_vt.m_eq.z || m_vt.m_primclass == GS_POINT_CLASS || m_vt.m_primclass == GS_SPRITE_CLASS;

	m_conf.cb_vs.max_depth = 0xFFFFFFFF;
	m_conf.cb_ps.TA_MaxDepth_Af.z = 0.0f;
	m_conf.ps.zclamp = false;

	m_conf.ps.zfloor = !flat_z &&
		(m_cached_ctx.DepthWrite() || (m_cached_ctx.DepthRead() && m_cached_ctx.TEST.ZTST == ZTST_GREATER));

	if (m_cached_ctx.DepthWrite() && large_z)
	{
		if (flat_z)
		{
			m_conf.cb_vs.max_depth = max_z;
		}
		else
		{
			m_conf.cb_ps.TA_MaxDepth_Af.z = static_cast<float>(max_z) * 0x1p-32f;
			m_conf.ps.zclamp = true;
		}
	}
}

void GSRendererHW::CalculateAlphaRange(GSTextureCache::Target* rt, GSTextureCache::Target* ds,
	DATEOptions& date_options, int& blend_alpha_min, int& blend_alpha_max, int& rt_new_alpha_min, int& rt_new_alpha_max)
{
	if (rt)
	{
		GL_INS("HW: RT alpha was %s before draw", rt->m_rt_alpha_scale ? "scaled" : "NOT scaled");

		blend_alpha_min = rt_new_alpha_min = rt->m_alpha_min;
		blend_alpha_max = rt_new_alpha_max = rt->m_alpha_max;

		const int fba_value = m_draw_env->CTXT[m_draw_env->PRIM.CTXT].FBA.FBA * 128;
		const bool is_24_bit = (GSLocalMemory::m_psm[rt->m_TEX0.PSM].trbpp == 24);
		if (is_24_bit)
		{
			blend_alpha_min = 128;
			blend_alpha_max = 128;
		}

		if (GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM) & 0x8 && !m_texture_shuffle)
		{
			const int s_alpha_max = GetAlphaMinMax().max | fba_value;
			const int s_alpha_min = GetAlphaMinMax().min | fba_value;

			const bool afail_always_fb_alpha = m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY || (m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp != 32);
			const bool always_passing_alpha = !m_cached_ctx.TEST.ATE || afail_always_fb_alpha || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ATST_ALWAYS);
			const bool full_cover = rt->m_valid.rintersect(m_r).eq(rt->m_valid) && m_primitive_covers_without_gaps == NoGapsType::FullCover &&
				!(date_options.enabled || !always_passing_alpha || !IsDepthAlwaysPassing());

			const u32 fb_mask = m_conf.colormask.wa ? (m_conf.ps.fbmask ? m_conf.cb_ps.FbMask.a : 0) : 0xFF;
			const u32 alpha_mask = (GSLocalMemory::m_psm[rt->m_TEX0.PSM].fmsk & 0xFF000000) >> 24;
			if ((fb_mask & alpha_mask) == 0)
			{
				if (full_cover)
				{
					rt_new_alpha_max = s_alpha_max;
					rt_new_alpha_min = s_alpha_min;
				}
				else
				{
					rt_new_alpha_max = std::max(s_alpha_max, rt_new_alpha_max);
					rt_new_alpha_min = std::min(s_alpha_min, rt_new_alpha_min);
				}
			}
			else if ((fb_mask & alpha_mask) != alpha_mask)
			{
				const u32 new_max_alpha = (s_alpha_max != s_alpha_min) ? (std::min(s_alpha_max, ((1 << (32 - std::countl_zero(static_cast<u32>(s_alpha_max)))) - 1)) & ~fb_mask) : (s_alpha_max & ~fb_mask);
				const u32 curr_max = (rt_new_alpha_max != rt_new_alpha_min && rt->m_alpha_range) ? (((1 << (32 - std::countl_zero(static_cast<u32>(rt_new_alpha_max)))) - 1) & fb_mask) : ((rt_new_alpha_max | rt_new_alpha_min) & fb_mask);
				if (full_cover)
					rt_new_alpha_max = new_max_alpha | curr_max;
				else
					rt_new_alpha_max = std::max(static_cast<int>(new_max_alpha | curr_max), rt_new_alpha_max);

				rt_new_alpha_min = std::min(s_alpha_min, rt_new_alpha_min);
			}

			if ((fb_mask & alpha_mask) != alpha_mask)
			{
				if (full_cover && (fb_mask & alpha_mask) == 0)
					rt->m_alpha_range = s_alpha_max != s_alpha_min;
				else
					rt->m_alpha_range |= (s_alpha_max & ~fb_mask) != (s_alpha_min & ~fb_mask);
			}
		}
		else if ((m_texture_shuffle && m_conf.colormask.wa))
		{
			const GSVector4i shuffle_rect = GSVector4i(m_vt.m_min.p.x, m_vt.m_min.p.y, m_vt.m_max.p.x, m_vt.m_max.p.y);
			if (!rt->m_valid.rintersect(shuffle_rect).eq(rt->m_valid) || (m_cached_ctx.FRAME.FBMSK & 0xFFFC0000))
			{
				rt_new_alpha_max = std::max(static_cast<int>((std::max(m_cached_ctx.TEXA.TA1, m_cached_ctx.TEXA.TA0) & 0x80) + 127), rt_new_alpha_max) | fba_value;
				rt_new_alpha_min = std::min(static_cast<int>(std::min(m_cached_ctx.TEXA.TA1, m_cached_ctx.TEXA.TA0) & 0x80), rt_new_alpha_min);
			}
			else
			{
				rt_new_alpha_max = (std::max(m_cached_ctx.TEXA.TA1, m_cached_ctx.TEXA.TA0) & 0x80) + 127 | fba_value;
				rt_new_alpha_min = (std::min(m_cached_ctx.TEXA.TA1, m_cached_ctx.TEXA.TA0) & 0x80) | fba_value;
			}
			rt->m_alpha_range = true;
		}

		GL_INS("HW: RT Alpha Range: %d-%d => %d-%d", blend_alpha_min, blend_alpha_max, rt_new_alpha_min, rt_new_alpha_max);

		if (m_prim_overlap != PRIM_OVERLAP_NO)
		{
			blend_alpha_min = std::min(blend_alpha_min, rt_new_alpha_min);
			blend_alpha_max = std::max(blend_alpha_max, rt_new_alpha_max);
		}

		if (!rt->m_32_bits_fmt)
		{
			rt_new_alpha_max &= 128;
			rt_new_alpha_min &= 128;

			if (rt_new_alpha_max == rt_new_alpha_min)
				rt->m_alpha_range = false;
		}
	}

	if (ds)
	{
		ds->m_alpha_max = std::max(static_cast<u32>(ds->m_alpha_max), static_cast<u32>(m_vt.m_max.p.z) >> 24);
		ds->m_alpha_min = std::min(static_cast<u32>(ds->m_alpha_min), static_cast<u32>(m_vt.m_min.p.z) >> 24);
		GL_INS("HW: New DS Alpha Range: %d-%d", ds->m_alpha_min, ds->m_alpha_max);

		if (GSLocalMemory::m_psm[ds->m_TEX0.PSM].bpp == 16)
		{
			ds->m_alpha_max &= 128;
			ds->m_alpha_min &= 128;
		}
	}
}

void GSRendererHW::DetermineAlphaScaling(GSTextureCache::Target* rt, GSTextureCache::Source* tex,
	bool req_src_update, int rt_new_alpha_max, bool& can_scale_rt_alpha, bool& new_scale_rt_alpha)
{
	if (rt)
	{
		const bool needs_ad = rt && m_context->ALPHA.C == 1 && rt->m_alpha_min != rt->m_alpha_max && rt->m_alpha_max > 128;

		can_scale_rt_alpha = !needs_ad && (GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM) & 0x8) && rt_new_alpha_max <= 128;

		const bool partial_fbmask = (m_conf.ps.fbmask && m_conf.cb_ps.FbMask.a != 0xFF && m_conf.cb_ps.FbMask.a != 0);
		const bool rta_decorrection = m_channel_shuffle || m_texture_shuffle || (m_conf.colormask.wa && (rt_new_alpha_max > 128 || partial_fbmask));

		if (rta_decorrection)
		{
			if (m_texture_shuffle)
			{
				if (m_conf.ps.process_ba & SHUFFLE_READ)
				{
					can_scale_rt_alpha = false;

					rt->UnscaleRTAlpha();
					m_conf.rt = rt->m_texture;

					if (req_src_update)
						tex->m_texture = rt->m_texture;
				}
				else if (m_conf.colormask.wa)
				{
					if (!(m_cached_ctx.FRAME.FBMSK & 0xFFFC0000))
					{
						can_scale_rt_alpha = false;
						rt->m_rt_alpha_scale = false;
					}
					else if (m_cached_ctx.FRAME.FBMSK & 0xFFFC0000)
					{
						can_scale_rt_alpha = false;
						rt->UnscaleRTAlpha();
						m_conf.rt = rt->m_texture;

						if (req_src_update)
							tex->m_texture = rt->m_texture;
					}
				}
			}
			else if (m_channel_shuffle)
			{
				if (m_conf.ps.tales_of_abyss_hle || (tex && tex->m_from_target && tex->m_from_target == rt && m_conf.ps.channel == ChannelFetch_ALPHA) || partial_fbmask || rt_new_alpha_max > 128)
				{
					can_scale_rt_alpha = false;
					rt->UnscaleRTAlpha();
					m_conf.rt = rt->m_texture;

					if (req_src_update)
						tex->m_texture = rt->m_texture;
				}
			}
			else if (rt->m_last_draw == s_n)
			{
				can_scale_rt_alpha = false;
				rt->m_rt_alpha_scale = false;
			}
			else
			{
				can_scale_rt_alpha = false;
				rt->UnscaleRTAlpha();
				m_conf.rt = rt->m_texture;

				if (req_src_update)
					tex->m_texture = rt->m_texture;
			}
		}

		new_scale_rt_alpha = rt->m_rt_alpha_scale;
	}
}

void GSRendererHW::EmulateAA1()
{
	pxAssert(!g_gs_device->Features().aa1 || g_gs_device->Features().feedback_loops());

	if (IsCoverageAlphaSupported())
	{
		m_conf.ps.abe = PRIM->ABE;

		if (m_vt.m_primclass == GS_LINE_CLASS)
		{
			GL_INS("HW: AA1 lines. No depth write.");

			m_conf.depth.zwe = false;
			m_cached_ctx.ZBUF.ZMSK = 1;

			m_conf.ps.aa1 = GSHWDrawConfig::PS_AA1::LINE;
		}
		else if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
		{
			if (m_cached_ctx.DepthWrite())
			{
				GL_INS("HW: AA1 triangles with depth feedback.");

				m_conf.ps.aa1 = GSHWDrawConfig::PS_AA1::TRIANGLE_SW_Z;

				ConfigureDepthFeedback();
			}
			else
			{
				GL_INS("HW: AA1 triangles with no depth write.");

				m_conf.ps.aa1 = GSHWDrawConfig::PS_AA1::TRIANGLE;
			}
		}
		else
		{
			pxFail("Unsupported primclass for AA1");
		}
	}
}

bool GSRendererHW::EmulateDATEEarlyFail(DATEOptions& date, GSTextureCache::Target* rt)
{
	if (!date.enabled)
		return false;

	const bool is_overlap_alpha = m_prim_overlap != PRIM_OVERLAP_NO && !(m_cached_ctx.FRAME.FBMSK & 0x80000000);
	if (m_cached_ctx.TEST.DATM == 0)
	{
		date.enabled = rt->m_alpha_max >= 128 || (is_overlap_alpha && rt->m_alpha_min < 128 && (GetAlphaMinMax().max >= 128 || (m_context->FBA.FBA || IsCoverageAlphaFixedOne())));

		if (date.enabled && rt->m_alpha_min >= 128)
			return true;
	}
	else
	{
		date.enabled = rt->m_alpha_min < 128 || (is_overlap_alpha && rt->m_alpha_max >= 128 && (GetAlphaMinMax().min < 128 && !(m_context->FBA.FBA || IsCoverageAlphaFixedOne())));

		if (date.enabled && rt->m_alpha_max < 128)
			return true;
	}

	return false;
}

void GSRendererHW::EmulateDATESelectMethod(DATEOptions& date_options, GSTextureCache::Target* rt, int& blend_alpha_min, int& blend_alpha_max)
{
	if (!date_options.enabled)
		return;

	const GSDevice::FeatureSupport& features = g_gs_device->Features();

	const bool complex_alpha_test = m_cached_ctx.TEST.ATE &&
	                                m_cached_ctx.TEST.ATST != ATST_ALWAYS &&
	                                m_cached_ctx.TEST.ATST != ATST_NEVER &&
	                                m_cached_ctx.TEST.AFAIL != AFAIL_KEEP &&
	                                m_prim_overlap != PRIM_OVERLAP_NO;
	if (m_cached_ctx.TEST.DATM)
	{
		blend_alpha_min = std::max(blend_alpha_min, 128);
		blend_alpha_max = std::max(blend_alpha_max, 128);
	}
	else
	{
		blend_alpha_min = std::min(blend_alpha_min, 127);
		blend_alpha_max = std::min(blend_alpha_max, 127);
	}

	if (features.framebuffer_fetch)
	{
		GL_PERF("DATE: Accurate with framebuffer fetch");
		date_options.barrier = true;
		m_conf.require_full_barrier = true;
	}
	else if (features.feedback_loops() && IsCoverageAlphaSupported())
	{
		GL_PERF("DATE: Accurate with IsCoverageAlphaSupported");
		date_options.barrier = true;
		m_conf.require_full_barrier = true;
	}
	else if ((features.texture_barrier && m_prim_overlap == PRIM_OVERLAP_NO))
	{
		GL_PERF("DATE: Accurate with no overlap");
		m_conf.require_full_barrier = true;
		date_options.barrier = true;
	}
	else if (features.feedback_loops() && m_texture_shuffle)
	{
		GL_PERF("DATE: Accurate with texture shuffle");
		m_conf.require_full_barrier = true;
		date_options.barrier = true;
	}
	else if (m_conf.colormask.wa && complex_alpha_test && features.feedback_loops())
	{
		GL_PERF("DATE: Accurate with complex alpha test.");
		m_conf.require_full_barrier = true;
		date_options.barrier = true;
	}
	else if (m_conf.colormask.wa && !complex_alpha_test && (m_context->FBA.FBA || IsCoverageAlphaFixedOne()) && features.stencil_buffer)
	{
		GL_PERF("DATE: Fast with FBA, all pixels will be >= 128");
		date_options.stencil_one = !m_cached_ctx.TEST.DATM;
	}
	else if (m_conf.colormask.wa && !complex_alpha_test && !(m_cached_ctx.FRAME.FBMSK & 0x80000000))
	{
		if (m_cached_ctx.TEST.DATM && GetAlphaMinMax().max < 128 && features.stencil_buffer)
		{
			GL_PERF("DATE: Fast with alpha %d-%d", GetAlphaMinMax().min, GetAlphaMinMax().max);
			date_options.stencil_one = true;
		}
		else if (!m_cached_ctx.TEST.DATM && GetAlphaMinMax().min >= 128 && features.stencil_buffer)
		{
			GL_PERF("DATE: Fast with alpha %d-%d", GetAlphaMinMax().min, GetAlphaMinMax().max);
			date_options.stencil_one = true;
		}
		else if (features.texture_barrier && ((m_vt.m_primclass == GS_SPRITE_CLASS && ComputeDrawlistGetSize(rt->m_scale) < 10) || (m_index->tail < 30)))
		{
			GL_PERF("DATE: Accurate with alpha %d-%d", GetAlphaMinMax().min, GetAlphaMinMax().max);
			m_conf.require_full_barrier = true;
			date_options.barrier = true;
		}
		else if (features.feedback_loops() && m_conf.require_full_barrier)
		{
			GL_PERF("DATE: Accurate with alpha %d-%d", GetAlphaMinMax().min, GetAlphaMinMax().max);
			m_conf.require_full_barrier = true;
			date_options.barrier = true;
		}
		else if (features.primitive_id)
		{
			GL_PERF("DATE: Accurate with alpha %d-%d", GetAlphaMinMax().min, GetAlphaMinMax().max);
			date_options.primid = true;
		}
		else if (features.feedback_loops())
		{
			GL_PERF("DATE: Accurate with alpha %d-%d", GetAlphaMinMax().min, GetAlphaMinMax().max);
			m_conf.require_full_barrier = true;
			date_options.barrier = true;
		}
		else if (features.stencil_buffer)
		{
			GL_PERF("DATE: Fast with alpha %d-%d", GetAlphaMinMax().min, GetAlphaMinMax().max);
			date_options.stencil_one = true;
		}
	}
	else if (features.texture_barrier && !m_conf.colormask.wa)
	{
		GL_PERF("DATE: Accurate with no alpha write");
		m_conf.require_one_barrier = true;
		date_options.barrier = true;
	}

	pxAssert(!(date_options.barrier && date_options.stencil_one));
	pxAssert(!(date_options.primid && date_options.stencil_one));
	pxAssert(!(date_options.primid && date_options.barrier));
}

void GSRendererHW::EmulateDATEGetConfig(DATEOptions& date_options, bool scale_rt_alpha, GSDevice::RecycledTexture& temp_ds)
{
	if (!date_options.enabled)
	{
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Off;
		return;
	}

	const GSDevice::FeatureSupport& features = g_gs_device->Features();

	if (!m_conf.colormask.wa && (m_conf.require_one_barrier || (m_conf.require_full_barrier && features.feedback_loops())))
		date_options.barrier = true;

	if (m_conf.ps.scanmsk & 2)
		date_options.primid = false;

	if (date_options.stencil_one)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::StencilOne;
	else if (date_options.primid)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking;
	else if (date_options.barrier)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Full;
	else if (features.stencil_buffer)
		m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;

	if (scale_rt_alpha)
		m_conf.datm = static_cast<SetDATM>(m_cached_ctx.TEST.DATM + 2);
	else
		m_conf.datm = static_cast<SetDATM>(m_cached_ctx.TEST.DATM);

	const bool date_stencil_needs_ds = !m_conf.ds &&
		(m_conf.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil || m_conf.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne);
	if (date_stencil_needs_ds)
	{
		const bool need_barrier = m_conf.require_one_barrier || (m_conf.require_full_barrier && features.feedback_loops());
		if ((temp_ds.reset(g_gs_device->CreateDepthStencil(m_conf.rt->GetWidth(), m_conf.rt->GetHeight(), false)), temp_ds))
		{
			m_conf.ds = temp_ds.get();
		}
		else if (need_barrier)
		{
			date_options.stencil_one = false;
			date_options.barrier = true;
			m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Full;
			DevCon.Warning("HW: Depth buffer creation failed for Stencil Date. Fallback to Full.");
		}
		else if (features.primitive_id && !(m_conf.ps.scanmsk & 2))
		{
			date_options.stencil_one = false;
			date_options.primid = true;
			m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::PrimIDTracking;
			DevCon.Warning("HW: Depth buffer creation failed for Stencil Date. Fallback to PrimIDTracking.");
		}
		else
		{
			date_options.enabled = false;
			date_options.stencil_one = false;
			m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Off;
			DevCon.Warning("HW: Depth buffer creation failed for Stencil Date.");
		}
	}

	if (date_options.barrier)
	{
		m_conf.ps.date = 5 + m_cached_ctx.TEST.DATM;
	}
	else if (date_options.stencil_one)
	{
		const bool multidraw_fb_copy = m_conf.require_one_barrier || (m_conf.require_full_barrier && features.multidraw_fb_copy);
		if (features.texture_barrier || multidraw_fb_copy)
		{
			m_conf.require_one_barrier = true;
			m_conf.ps.date = 5 + m_cached_ctx.TEST.DATM;
		}
		m_conf.depth.date = 1;
		m_conf.depth.date_one = 1;
	}
	else if (date_options.primid)
	{
		m_conf.ps.date = 1 + m_cached_ctx.TEST.DATM;
	}
	else if (date_options.enabled)
	{
		m_conf.depth.date = 1;
	}
}

void GSRendererHW::DetermineVSConfig(GSTextureCache::Target* rt, float rtscale, const GSVector2i& rtsize,
	const GSVector2i& unscaled_size, float& scale_x, float& scale_y)
{
	m_conf.vs.tme = m_process_texture;
	m_conf.vs.fst = PRIM->FST;

	float sx, sy, ox2, oy2;
	const float ox = static_cast<float>(static_cast<int>(m_context->XYOFFSET.OFX));
	const float oy = static_cast<float>(static_cast<int>(m_context->XYOFFSET.OFY));

	if ((GSConfig.UserHacks_HalfPixelOffset < GSHalfPixelOffset::Native || m_texture_shuffle) && rtscale > 1.0f)
	{
		sx = 2.0f * rtscale / (rtsize.x << 4);
		sy = 2.0f * rtscale / (rtsize.y << 4);
		ox2 = -1.0f / rtsize.x;
		oy2 = -1.0f / rtsize.y;
		float mod_xy = 0.0f;

		if (!rt)
			mod_xy = GetModXYOffset();
		else
			mod_xy = rt->OffsetHack_modxy;

		if (mod_xy > 1.0f)
		{
			ox2 *= mod_xy;
			oy2 *= mod_xy;
		}
	}
	else
	{
		const int unscaled_x = unscaled_size.x;
		const int unscaled_y = unscaled_size.y;
		sx = 2.0f / (unscaled_x << 4);
		sy = 2.0f / (unscaled_y << 4);

		if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::NativeWTexOffset)
		{
			ox2 = (-1.0f / (unscaled_x * rtscale));
			oy2 = (-1.0f / (unscaled_y * rtscale));

			if (m_vt.m_primclass == GS_SPRITE_CLASS && rtscale > 1.0f && (m_process_texture && PRIM->FST))
			{
				const GSVertex* v = &m_vertex->buff[0];
				const int x1_frac = ((v[1].XYZ.X - m_context->XYOFFSET.OFX) & 0xf);
				const int y1_frac = ((v[1].XYZ.Y - m_context->XYOFFSET.OFY) & 0xf);
				if (x1_frac & 8)
					ox2 *= 1.0f + ((static_cast<float>(16 - x1_frac) / 8.0f) * rtscale);

				if (y1_frac & 8)
					oy2 *= 1.0f + ((static_cast<float>(16 - y1_frac) / 8.0f) * rtscale);
			}
		}
		else
		{
			ox2 = -1.0f / unscaled_x;
			oy2 = -1.0f / unscaled_y;
		}
	}

	scale_x = sx;
	scale_y = sy;
	m_conf.cb_vs.vertex_scale = GSVector2(sx, sy);
	m_conf.cb_vs.vertex_offset = GSVector2(ox * sx + ox2 + 1, oy * sy + oy2 + 1);

#ifdef ENABLE_VR
	const VR::StereoState::Params st = VR::StereoState::Get();
	const bool vr_multiview_target = rt && rt->m_texture && (rt->m_texture->GetArrayLayers() > 1);
	bool vr_pin_screen = false;
	static const bool s_vr_pinq1_debug = (std::getenv("PCSX2_VR_PINQ1") != nullptr);
	if (st.enabled && (st.pin_uniform_q || s_vr_pinq1_debug) && PRIM->TME && !PRIM->FST && m_vt.m_eq.q)
	{
		vr_pin_screen = true;
		if (s_vr_pinq1_debug)
			DevCon.WriteLn("(VR) uniform-Q draw pinned: Q=%f verts=%u",
				m_vertex->buff[0].RGBAQ.Q, static_cast<unsigned>(m_vertex->next));
	}
	static const bool s_vr_interleave_debug = (std::getenv("PCSX2_VR_INTERLEAVE") != nullptr);
	const bool vr_mono_centre = !vr_multiview_target && !s_vr_interleave_debug;
	const bool vr_engaged = st.enabled && !vr_pin_screen && !vr_mono_centre;
	const float vr_eye_sign = vr_multiview_target ? 1.0f : VR::StereoState::GetCurrentEyeSign();
	m_conf.cb_vs.vr_stereo =
		vr_engaged ?
			GSVector2(st.separation * vr_eye_sign, st.convergence) :
			GSVector2(0.0f, 0.0f);
	if (vr_engaged && st.map != VR::StereoState::Params::Map::Linear)
	{
		m_conf.cb_vs.vr_map_mode = static_cast<u32>(st.map);
		m_conf.cb_vs.vr_band_count = st.band_count;
		m_conf.cb_vs.vr_splits = GSVector4(st.split_q[0], st.split_q[1], st.split_q[2], vr_eye_sign);
		if (st.map == VR::StereoState::Params::Map::Log)
		{
			m_conf.cb_vs.vr_band[0] = GSVector4(st.log_w0, st.log_w1, st.log_dfar, 0.0f);
			m_conf.cb_vs.vr_band[1] = GSVector4::zero();
			m_conf.cb_vs.vr_band[2] = GSVector4::zero();
			m_conf.cb_vs.vr_band[3] = GSVector4::zero();
		}
		else
		{
			for (u32 i = 0; i < 4; i++)
				m_conf.cb_vs.vr_band[i] = GSVector4(st.conv[i], st.sep[i], st.bias[i], 0.0f);
		}
	}
	else
	{
		m_conf.cb_vs.vr_map_mode = 0;
		m_conf.cb_vs.vr_band_count = 0;
		m_conf.cb_vs.vr_splits = GSVector4::zero();
		m_conf.cb_vs.vr_band[0] = GSVector4::zero();
		m_conf.cb_vs.vr_band[1] = GSVector4::zero();
		m_conf.cb_vs.vr_band[2] = GSVector4::zero();
		m_conf.cb_vs.vr_band[3] = GSVector4::zero();
	}
	float vr_collimate = 0.0f;
	if (vr_engaged && PRIM->FST && st.collimate_disparity != 0.0f)
	{
		const int rw = m_r.width();
		const int rh = m_r.height();
		int matched = -1;
		for (u32 i = 0; i < st.collimate_rule_count && matched < 0; i++)
		{
			const VR::StereoState::Params::CollimateRule& r = st.collimate_rules[i];
			if (r.prim >= 0 && static_cast<int>(m_vt.m_primclass) != static_cast<int>(r.prim))
				continue;
			if (r.tme >= 0 && (PRIM->TME ? 1 : 0) != static_cast<int>(r.tme))
				continue;
			if (r.abe >= 0 && (PRIM->ABE ? 1 : 0) != static_cast<int>(r.abe))
				continue;
			if ((r.min_w > 0 && rw < r.min_w) || (r.max_w > 0 && rw > r.max_w))
				continue;
			if ((r.min_h > 0 && rh < r.min_h) || (r.max_h > 0 && rh > r.max_h))
				continue;
			if (r.rx1 > r.rx0)
			{
				const float w = static_cast<float>(unscaled_size.x);
				if (static_cast<float>(m_r.x) < r.rx0 * w || static_cast<float>(m_r.z) > r.rx1 * w)
					continue;
			}
			if (r.ry1 > r.ry0)
			{
				const float h = static_cast<float>(unscaled_size.y);
				if (static_cast<float>(m_r.y) < r.ry0 * h || static_cast<float>(m_r.w) > r.ry1 * h)
					continue;
			}
			if (r.tu1 > r.tu0)
			{
				if (m_vt.m_min.t.x < r.tu0 || m_vt.m_max.t.x > r.tu1)
					continue;
			}
			if (r.tv1 > r.tv0)
			{
				if (m_vt.m_min.t.y < r.tv0 || m_vt.m_max.t.y > r.tv1)
					continue;
			}
			matched = static_cast<int>(i);
		}

		if (matched >= 0)
			vr_collimate = st.collimate_disparity * vr_eye_sign;

		static u64 s_coll_considered = 0;
		static u64 s_coll_matched = 0;
		static bool s_coll_dead_warned = false;
		s_coll_considered++;
		if (matched >= 0)
		{
			if (s_coll_matched++ == 0)
			{
				DevCon.WriteLn("(VR) HUD collimation: rule %d ('%s') FIRST MATCH after %llu FST draws — "
							   "prim=%d tme=%d abe=%d r=%d,%d-%d,%d (%dx%d) uv=%.1f,%.1f-%.1f,%.1f d=%+.4f",
					matched, st.collimate_rules[matched].label,
					static_cast<unsigned long long>(s_coll_considered),
					static_cast<int>(m_vt.m_primclass), PRIM->TME ? 1 : 0, PRIM->ABE ? 1 : 0,
					m_r.x, m_r.y, m_r.z, m_r.w, rw, rh,
					m_vt.m_min.t.x, m_vt.m_min.t.y, m_vt.m_max.t.x, m_vt.m_max.t.y, vr_collimate);
			}
		}
		else if (!s_coll_dead_warned && s_coll_considered > 20000)
		{
			s_coll_dead_warned = true;
			Console.WarningFmt("(VR) HUD collimation: {} rule(s) authored, but NOT ONE of {} UV/FST draws has "
							   "matched. The rule does not describe this game's symbology.",
				st.collimate_rule_count, s_coll_considered);
		}

		static const bool s_coll_census = (std::getenv("PCSX2_VR_HUDCOLL") != nullptr);
		if (s_coll_census)
		{
			DevCon.WriteLn("(VR) HUDCOLL %s prim=%d tme=%d abe=%d r=%d,%d-%d,%d (%dx%d) fb=%dx%d "
						   "pct=%.3f,%.3f-%.3f,%.3f tbp=0x%x",
				(matched >= 0) ? "MATCH  " : "nomatch",
				static_cast<int>(m_vt.m_primclass), PRIM->TME ? 1 : 0, PRIM->ABE ? 1 : 0,
				m_r.x, m_r.y, m_r.z, m_r.w, rw, rh,
				unscaled_size.x, unscaled_size.y,
				static_cast<float>(m_r.x) / static_cast<float>(std::max(unscaled_size.x, 1)),
				static_cast<float>(m_r.y) / static_cast<float>(std::max(unscaled_size.y, 1)),
				static_cast<float>(m_r.z) / static_cast<float>(std::max(unscaled_size.x, 1)),
				static_cast<float>(m_r.w) / static_cast<float>(std::max(unscaled_size.y, 1)),
				PRIM->TME ? m_cached_ctx.TEX0.TBP0 : 0);

			const GIFRegTEX0& t0 = m_cached_ctx.TEX0;
			DevCon.WriteLn("(VR) HUDCOLL2 nv=%u ni=%u uv=%.1f,%.1f-%.1f,%.1f tw=%d th=%d psm=0x%x tbw=%u "
						   "abcd=%u%u%u%u fix=%u ate=%u atst=%u aref=%u zte=%u ztst=%u sc=%u,%u-%u,%u",
				m_vertex->next, m_index->tail,
				m_vt.m_min.t.x, m_vt.m_min.t.y, m_vt.m_max.t.x, m_vt.m_max.t.y,
				1 << t0.TW, 1 << t0.TH, static_cast<u32>(t0.PSM), static_cast<u32>(t0.TBW),
				static_cast<u32>(m_context->ALPHA.A), static_cast<u32>(m_context->ALPHA.B),
				static_cast<u32>(m_context->ALPHA.C), static_cast<u32>(m_context->ALPHA.D),
				static_cast<u32>(m_context->ALPHA.FIX),
				static_cast<u32>(m_cached_ctx.TEST.ATE), static_cast<u32>(m_cached_ctx.TEST.ATST),
				static_cast<u32>(m_cached_ctx.TEST.AREF), static_cast<u32>(m_cached_ctx.TEST.ZTE),
				static_cast<u32>(m_cached_ctx.TEST.ZTST),
				static_cast<u32>(m_context->SCISSOR.SCAX0), static_cast<u32>(m_context->SCISSOR.SCAY0),
				static_cast<u32>(m_context->SCISSOR.SCAX1), static_cast<u32>(m_context->SCISSOR.SCAY1));

			if (m_vt.m_primclass == GS_SPRITE_CLASS && m_index->tail <= 128)
			{
				const int ofx = static_cast<int>(m_context->XYOFFSET.OFX);
				const int ofy = static_cast<int>(m_context->XYOFFSET.OFY);
				for (u32 vi = 0; vi + 1 < m_index->tail; vi += 2)
				{
					const GSVertex& va = m_vertex->buff[m_index->buff[vi]];
					const GSVertex& vb = m_vertex->buff[m_index->buff[vi + 1]];
					DevCon.WriteLn("(VR) HUDCOLLV s=%u xy=%d,%d-%d,%d uv=%.1f,%.1f-%.1f,%.1f rgba=%02x%02x%02x%02x",
						vi / 2,
						(static_cast<int>(va.XYZ.X) - ofx) >> 4, (static_cast<int>(va.XYZ.Y) - ofy) >> 4,
						(static_cast<int>(vb.XYZ.X) - ofx) >> 4, (static_cast<int>(vb.XYZ.Y) - ofy) >> 4,
						static_cast<float>(va.U) / 16.0f, static_cast<float>(va.V) / 16.0f,
						static_cast<float>(vb.U) / 16.0f, static_cast<float>(vb.V) / 16.0f,
						static_cast<u32>(vb.RGBAQ.R), static_cast<u32>(vb.RGBAQ.G),
						static_cast<u32>(vb.RGBAQ.B), static_cast<u32>(vb.RGBAQ.A));
				}
			}
		}
	}
	m_conf.cb_vs.vr_band[0].w = vr_collimate;

	if (vr_multiview_target)
	{
		static bool s_logged_mv_draw = false;
		if (!s_logged_mv_draw)
		{
			s_logged_mv_draw = true;
			DevCon.WriteLn("(VR) First multiview draw: vr_stereo = {%.4f, %.3f}.",
				m_conf.cb_vs.vr_stereo.x, m_conf.cb_vs.vr_stereo.y);
		}
	}
	if (VR::DepthHistogramArmed()) [[unlikely]]
	{
		const bool qh_fst_excluded = !PRIM->TME || PRIM->FST;
		const bool qh_stq_hazard = m_vt.m_accurate_stq && m_vt.m_primclass == GS_SPRITE_CLASS && !qh_fst_excluded;
		const VR::DrawClass qh_class = !st.enabled ? VR::DrawClass::StereoOff :
		                               qh_fst_excluded ? VR::DrawClass::FstExcluded :
		                               qh_stq_hazard ? VR::DrawClass::AccurateStqFlagged :
		                               vr_pin_screen ? VR::DrawClass::UniformQPinned :
		                               vr_mono_centre ? VR::DrawClass::MonoCentre :
		                                                VR::DrawClass::Displaced;
		const GSVector4i qh_r = m_r.rintersect(m_context->scissor.in);
		const double qh_target_area = static_cast<double>(unscaled_size.x) * static_cast<double>(unscaled_size.y);
		const double qh_area = qh_r.rempty() ? 0.0 :
		                                       (static_cast<double>(qh_r.width()) * static_cast<double>(qh_r.height()));
		const double qh_qmin = static_cast<double>(m_vt.m_min.t.z);
		const double qh_qmax = static_cast<double>(m_vt.m_max.t.z);
		const int qh_vpp = GSUtil::GetClassVertexCount(m_vt.m_primclass);
		VR::DepthHistogram& qh = VR::GlobalDepthHistogram();
		qh.NoteTargetSize(unscaled_size.x, unscaled_size.y);
		qh.AddDraw((qh_target_area > 0.0) ? (qh_area / qh_target_area) : 0.0, qh_qmin, qh_qmax,
			(qh_vpp > 0) ? (m_index->tail / static_cast<u32>(qh_vpp)) : 0u, m_vertex->next, qh_class);
	}
#else
	m_conf.cb_vs.vr_stereo = GSVector2(0.0f, 0.0f);
	m_conf.cb_vs.vr_map_mode = 0;
	m_conf.cb_vs.vr_band[0].w = 0.0f;
#endif

	m_conf.vs.iip = !IsFlatShaded();
}

void GSRendererHW::DetermineBarriers(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	const GSDevice::FeatureSupport& features = g_gs_device->Features();

	if (features.framebuffer_fetch)
	{
		if (m_conf.require_one_barrier || m_conf.require_full_barrier)
			pxAssert(!m_conf.blend.enable);

		const bool need_barriers_for_depth = m_conf.ps.IsFeedbackLoopDepth() && features.depth_feedback;

		if (!need_barriers_for_depth)
		{
			m_conf.require_one_barrier = false;
			m_conf.require_full_barrier = false;
		}
	}
	pxAssert(!m_conf.require_full_barrier || !m_conf.ps.colclip_hw);

	if (features.feedback_loops() && m_conf.require_full_barrier && (m_prim_overlap == PRIM_OVERLAP_NO || m_conf.ps.shuffle || m_channel_shuffle))
	{
		m_conf.require_full_barrier = false;
		m_conf.require_one_barrier = true;
	}
	else if (!features.feedback_loops())
	{
		m_conf.require_full_barrier = false;
	}

	if (m_conf.require_full_barrier && features.feedback_loops())
	{
		ComputeDrawlistGetSize(rt->m_scale);
		m_conf.drawlist = &m_drawlist;
		m_conf.drawlist_bbox = &m_drawlist_bbox;
	}
}

void GSRendererHW::EmulateDither()
{
	if (m_conf.ps.dither || m_conf.blend_multi_pass.dither)
	{
		const GIFRegDIMX& DIMX = m_draw_env->DIMX;
		GL_DBG("DITHERING mode %s (%d)", (GSConfig.Dithering == 3) ? "Force 32bit" : ((GSConfig.Dithering == 0) ? "Disabled" : "Enabled"), GSConfig.Dithering);

		if (m_conf.ps.dither || GSConfig.Dithering == 3)
			m_conf.ps.dither = GSConfig.Dithering;

		m_conf.cb_ps.DitherMatrix[0] = GSVector4(DIMX.DM00, DIMX.DM01, DIMX.DM02, DIMX.DM03);
		m_conf.cb_ps.DitherMatrix[1] = GSVector4(DIMX.DM10, DIMX.DM11, DIMX.DM12, DIMX.DM13);
		m_conf.cb_ps.DitherMatrix[2] = GSVector4(DIMX.DM20, DIMX.DM21, DIMX.DM22, DIMX.DM23);
		m_conf.cb_ps.DitherMatrix[3] = GSVector4(DIMX.DM30, DIMX.DM31, DIMX.DM32, DIMX.DM33);
	}
	else if (GSConfig.Dithering > 2)
	{
		m_conf.ps.dither = GSConfig.Dithering;
		m_conf.blend_multi_pass.dither = GSConfig.Dithering;
	}
}

void GSRendererHW::EmulateTextureShuffleAndFbmask(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{

	const bool enable_fbmask_emulation = GSConfig.AccurateBlendingUnit != AccBlendLevel::Minimum;
	const GSDevice::FeatureSupport features = g_gs_device->Features();

	if (m_texture_shuffle)
	{
		ConvertSpriteTextureShuffle(rt, tex);

		m_conf.ps.shuffle = 1;
		m_conf.ps.dst_fmt = GSLocalMemory::PSM_FMT_32;

		u32 process_rg = 0;
		u32 process_ba = 0;

		if (m_texture_shuffle.channels & TextureShuffleChannels_ReadRedGreen)
		{
			process_rg |= SHUFFLE_READ;
		}

		if (m_texture_shuffle.channels & TextureShuffleChannels_ReadBlueAlpha)
		{
			process_ba |= SHUFFLE_READ;
		}

		if (m_texture_shuffle.channels & TextureShuffleChannels_WriteRedGreen)
		{
			process_rg |= SHUFFLE_WRITE;
		}

		if (m_texture_shuffle.channels & TextureShuffleChannels_WriteBlueAlpha)
		{
			process_ba |= SHUFFLE_WRITE;
		}

		const bool shuffle_across = (m_texture_shuffle.channels & TextureShuffleChannels_ShuffleAcross) != 0;

		m_conf.ps.write_rg = !!(process_rg & SHUFFLE_WRITE) && !!m_cached_ctx.TEST.DATE;

		m_conf.ps.real16src = m_texture_shuffle.real_16_bit_source;

		m_conf.ps.shuffle_same = m_texture_shuffle.SameGroupShuffle();

		const u32 fbmask = GetEffectiveTextureShuffleFbmsk();
		u32 fbmask_r = (fbmask >> 0) & 0xFF;
		u32 fbmask_g = (fbmask >> 8) & 0xFF;
		u32 fbmask_b = (fbmask >> 16) & 0xFF;
		u32 fbmask_a = (fbmask >> 24) & 0xFF;

		if (!(m_texture_shuffle.channels & TextureShuffleChannels_WriteRed))
			fbmask_r = 0xFF;
		if (!(m_texture_shuffle.channels & TextureShuffleChannels_WriteGreen))
			fbmask_g = 0xFF;
		if (!(m_texture_shuffle.channels & TextureShuffleChannels_WriteBlue))
			fbmask_b = 0xFF;
		if (!(m_texture_shuffle.channels & TextureShuffleChannels_WriteAlpha))
			fbmask_a = 0xFF;

		m_conf.ps.process_rg = process_rg;
		m_conf.ps.process_ba = process_ba;
		m_conf.ps.shuffle_across = shuffle_across;
		
		m_conf.colormask.wr = fbmask_r != 0xFF;
		m_conf.colormask.wg = fbmask_g != 0xFF;
		m_conf.colormask.wb = fbmask_b != 0xFF;
		m_conf.colormask.wa = fbmask_a != 0xFF;

		m_conf.ps.fbmask =
			(fbmask_r != 0 && fbmask_r != 0xFF) ||
			(fbmask_g != 0 && fbmask_g != 0xFF) ||
			(fbmask_b != 0 && fbmask_b != 0xFF) ||
			(fbmask_a != 0 && fbmask_a != 0xFF);

		if (m_conf.ps.fbmask && enable_fbmask_emulation)
		{
			m_conf.cb_ps.FbMask.r = fbmask_r;
			m_conf.cb_ps.FbMask.g = fbmask_g;
			m_conf.cb_ps.FbMask.b = fbmask_b;
			m_conf.cb_ps.FbMask.a = fbmask_a;

			m_conf.require_one_barrier = true;
			GL_INS("HW: FBMASK SW emulated fbmask=%x on tex shuffle", fbmask);
		}
		else
		{
			m_conf.ps.fbmask = 0;
		}

		rt->m_valid_alpha_low |= m_conf.colormask.wa;
		rt->m_valid_alpha_high |= m_conf.colormask.wa;

		m_split_texture_shuffle_pages = 0;
		m_split_texture_shuffle_pages_high = 0;
		m_split_texture_shuffle_start_FBP = 0;
		m_split_texture_shuffle_start_TBP = 0;

		if (m_cached_ctx.CLAMP.WMS > CLAMP_CLAMP)
			m_cached_ctx.CLAMP.WMS = m_cached_ctx.CLAMP.WMS == CLAMP_REGION_CLAMP ? CLAMP_CLAMP : CLAMP_REPEAT;
		if (m_cached_ctx.CLAMP.WMT > CLAMP_CLAMP)
			m_cached_ctx.CLAMP.WMT = m_cached_ctx.CLAMP.WMT == CLAMP_REGION_CLAMP ? CLAMP_CLAMP : CLAMP_REPEAT;

		m_primitive_covers_without_gaps = rt->m_valid.rintersect(m_r).eq(rt->m_valid) ? NoGapsType::FullCover : NoGapsType::GapsFound;
	}
	else
	{
		m_conf.ps.dst_fmt = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmt;

		int fbmask = static_cast<int>(m_cached_ctx.FRAME.FBMSK);
		const int fbmask_r = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;
		fbmask &= fbmask_r;
		const GSVector4i fbmask_v = GSVector4i::load(fbmask);
		const GSVector4i fbmask_vr = GSVector4i::load(fbmask_r);
		const int ff_fbmask = fbmask_v.eq8(fbmask_vr).mask();
		const int zero_fbmask = fbmask_v.eq8(GSVector4i::zero()).mask();

		m_conf.colormask.wrgba = ~ff_fbmask;

		m_conf.ps.fbmask = enable_fbmask_emulation && (~ff_fbmask & ~zero_fbmask & 0xF);

		if (m_conf.ps.fbmask)
		{
			m_conf.cb_ps.FbMask = fbmask_v.u8to32();
			if (!PRIM->ABE || !(~ff_fbmask & ~zero_fbmask & 0x7) || !features.feedback_loops())
			{
				GL_INS("HW: FBMASK Unsafe SW emulated fb_mask:%x on %d bits format", m_cached_ctx.FRAME.FBMSK,
					(m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_16) ? 16 : 32);
				m_conf.require_one_barrier = true;
			}
			else
			{
				GL_INS("HW: FBMASK SW emulated fb_mask:%x on %d bits format", m_cached_ctx.FRAME.FBMSK,
					(m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_16) ? 16 : 32);
				m_conf.require_full_barrier = true;
			}
		}
	}
}

bool GSRendererHW::TestChannelShuffle(GSTextureCache::Target* src)
{
	const bool shuffle = m_channel_shuffle || IsPossibleChannelShuffle();

	m_channel_shuffle = (shuffle && EmulateChannelShuffle(src, true)) != 0;
	return m_channel_shuffle;
}

__ri u32 GSRendererHW::EmulateChannelShuffle(GSTextureCache::Target* src, bool test_only, GSTextureCache::Target* rt)
{
	if (src && src->m_texture->IsDepthLike() && !src->m_32_bits_fmt)
	{
		if ((m_cached_ctx.FRAME.FBMSK & 0x00FF0000) == 0x00FF0000)
		{
			GL_INS("HW: HLE Shuffle Tales Of Abyss");
			if (test_only)
				return ChannelFetch_RGB;

			m_conf.ps.tales_of_abyss_hle = 1;
		}
		else
		{
			GL_INS("HW: HLE Shuffle Urban Chaos");
			if (test_only)
				return ChannelFetch_RGB;

			m_conf.ps.urban_chaos_hle = 1;
		}
	}
	else if (m_cached_ctx.CLAMP.WMS == 3 && ((m_cached_ctx.CLAMP.MAXU & 0x8) == 8))
	{
		const ChannelFetch channel_select = ((m_cached_ctx.CLAMP.WMT != 3 && (m_vertex->buff[m_index->buff[0]].V & 0x20) == 0) || (m_cached_ctx.CLAMP.WMT == 3 && ((m_cached_ctx.CLAMP.MAXV & 0x2) == 0))) ? ChannelFetch_BLUE : ChannelFetch_ALPHA;

		if (test_only)
			return channel_select;

		GL_INS("HW: %s channel", (channel_select == ChannelFetch_BLUE) ? "blue" : "alpha");

		m_conf.ps.channel = channel_select;
	}
	else if (m_cached_ctx.CLAMP.WMS == 3 && ((m_cached_ctx.CLAMP.MINU & 0x8) == 0))
	{
		const bool green = (m_cached_ctx.CLAMP.WMT == 3 && ((m_cached_ctx.CLAMP.MAXV & 0x2) == 2)) || (PRIM->FST && (m_vertex->buff[0].V & 32));
		if (green && (m_cached_ctx.FRAME.FBMSK & 0x00FFFFFF) == 0x00FFFFFF)
		{
			const int blue_mask = m_cached_ctx.FRAME.FBMSK >> 24;
			int blue_shift = -1;

			switch (blue_mask)
			{
				case 0xFF: pxAssert(0);      break;
				case 0xFE: blue_shift = 1; break;
				case 0xFC: blue_shift = 2; break;
				case 0xF8: blue_shift = 3; break;
				case 0xF0: blue_shift = 4; break;
				case 0xE0: blue_shift = 5; break;
				case 0xC0: blue_shift = 6; break;
				case 0x80: blue_shift = 7; break;
				default:                   break;
			}

			if (blue_shift >= 0)
			{
				const int green_mask = ~blue_mask & 0xFF;
				const int green_shift = 8 - blue_shift;

				GL_INS("HW: Green/Blue channel (%d, %d)", blue_shift, green_shift);
				if (test_only)
					return ChannelFetch_GXBY;

				m_conf.cb_ps.ChannelShuffle = GSVector4i(blue_mask, blue_shift, green_mask, green_shift);
				m_conf.ps.channel = ChannelFetch_GXBY;
				m_cached_ctx.FRAME.FBMSK = 0x00FFFFFF;
			}
			else
			{
				GL_INS("HW: Green channel (wrong mask) (fbmask %x)", blue_mask);
				if (test_only)
					return ChannelFetch_GREEN;

				m_conf.ps.channel = ChannelFetch_GREEN;
			}
		}
		else if (green)
		{
			GL_INS("HW: Green channel");
			if (test_only)
				return ChannelFetch_GREEN;

			m_conf.ps.channel = ChannelFetch_GREEN;
		}
		else
		{
			GL_INS("HW: Red channel");
			if (test_only)
				return ChannelFetch_RED;

			m_conf.ps.channel = ChannelFetch_RED;
		}
	}
	else
	{
		GSVector4i min_uv = GSVector4i(m_vt.m_min.t.upld(GSVector4::zero()));
		ChannelFetch channel = ChannelFetch_NONE;
		const GSLocalMemory::psm_t& t_psm = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM];
		const GSLocalMemory::psm_t& f_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
		GSVector4i block_offset = GSVector4i(min_uv.x / t_psm.bs.x, min_uv.y / t_psm.bs.y).xyxy();
		GSVector4i m_r_block_offset = GSVector4i((m_r.x & (f_psm.pgs.x - 1)) / f_psm.bs.x, (m_r.y & (f_psm.pgs.y - 1)) / f_psm.bs.y);

		min_uv.x -= block_offset.x * t_psm.bs.x;
		min_uv.y -= block_offset.y * t_psm.bs.y;
		min_uv.y &= 2;
		min_uv.x &= 8;
		{
			if (min_uv.eq(GSVector4i::cxpr(0, 0, 0, 0)))
				channel = ChannelFetch_RED;
			else if (min_uv.eq(GSVector4i::cxpr(0, 2, 0, 0)))
				channel = ChannelFetch_GREEN;
			else if (min_uv.eq(GSVector4i::cxpr(8, 0, 0, 0)))
				channel = ChannelFetch_BLUE;
			else if (min_uv.eq(GSVector4i::cxpr(8, 2, 0, 0)))
				channel = ChannelFetch_ALPHA;
		}

		if (channel != ChannelFetch_NONE)
		{
#ifdef ENABLE_OGL_DEBUG
			static constexpr const char* channel_names[] = { "Red", "Green", "Blue", "Alpha" };
			GL_INS("HW: %s channel from min UV: r={%d,%d=>%d,%d} min uv = %d,%d", channel_names[static_cast<u32>(channel - 1)],
				m_r.x, m_r.y, m_r.z, m_r.w, min_uv.x, min_uv.y);
#endif

			if (test_only)
				return channel;

			m_conf.ps.channel = channel;
		}
		else
		{
			GL_INS("HW: Channel not supported r={%d,%d=>%d,%d} min uv = %d,%d",
				m_r.x, m_r.y, m_r.z, m_r.w, min_uv.x, min_uv.y);

			if (test_only)
				return ChannelFetch_NONE;

			m_channel_shuffle = false;
			return false;
		}
	}

	pxAssert(m_channel_shuffle);

	m_conf.tex = src->m_texture;

	const GSLocalMemory::psm_t frame_psm = GSLocalMemory::m_psm[m_context->FRAME.PSM];
	m_full_screen_shuffle = (m_r.height() > frame_psm.pgs.y) || (m_r.width() > frame_psm.pgs.x) || GSConfig.UserHacks_TextureInsideRt == GSTextureInRtMode::Disabled;
	m_channel_shuffle_src_valid = src->m_valid;
	if (GSConfig.UserHacks_TextureInsideRt == GSTextureInRtMode::Disabled || ((src->m_TEX0.TBW == rt->m_TEX0.TBW) && (!m_in_target_draw && IsPageCopy())) || m_conf.ps.urban_chaos_hle || m_conf.ps.tales_of_abyss_hle)
	{
		GSVertex* s = &m_vertex->buff[0];
		s[0].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + 0);
		s[1].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + 16384);
		s[0].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + 0);
		s[1].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + 16384);

		s[0].U = 0;
		s[1].U = 16384;
		s[0].V = 0;
		s[1].V = 16384;

		m_r = GSVector4i(0, 0, 1024, 1024);
		if (!m_full_screen_shuffle && !m_conf.ps.urban_chaos_hle && !m_conf.ps.tales_of_abyss_hle && src)
		{
			if (rt->m_last_draw >= s_n)
				rt->ResizeValidity(GSVector4i::zero());

			m_channel_shuffle_width = src->m_TEX0.TBW;
		}

		m_channel_shuffle_finish = false;

		m_vertex->head = m_vertex->tail = m_vertex->next = 2;
		m_index->tail = 2;
	}
	else
	{
		const u32 frame_page_offset = std::max(static_cast<int>(((m_r.x / frame_psm.pgs.x) + (m_r.y / frame_psm.pgs.y) * rt->m_TEX0.TBW)), 0);
		m_r = GSVector4i(m_r.x & ~(frame_psm.pgs.x - 1), m_r.y & ~(frame_psm.pgs.y - 1), (m_r.z + (frame_psm.pgs.x - 1)) & ~(frame_psm.pgs.x - 1), (m_r.w + (frame_psm.pgs.y - 1)) & ~(frame_psm.pgs.y - 1));

		if (rt && rt->m_TEX0.TBP0 == m_cached_ctx.FRAME.Block())
		{
			const bool req_offset = (m_cached_ctx.CLAMP.WMS != 3 || (m_cached_ctx.CLAMP.MAXU & ~0xF) == 0) &&
			                        (m_cached_ctx.CLAMP.WMT != 3 || (m_cached_ctx.CLAMP.MAXV & ~0x3) == 0);
			if (req_offset)
				m_cached_ctx.FRAME.FBP += frame_page_offset;
		}

		m_in_target_draw |= frame_page_offset > 0;

		if (!(m_index->tail <= 64 && !IsPageCopy() && m_cached_ctx.CLAMP.WMT == 3))
		{
			GSVertex* s = &m_vertex->buff[0];
			s[0].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + (m_r.x << 4));
			s[1].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + (m_r.z << 4));
			s[0].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + (m_r.y << 4));
			s[1].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + (m_r.w << 4));

			s[0].U = m_r.x << 4;
			s[1].U = m_r.z << 4;
			s[0].V = m_r.y << 4;
			s[1].V = m_r.w << 4;
			m_vertex->head = m_vertex->tail = m_vertex->next = 2;
			m_index->tail = 2;
		}

		const u32 frame_offset = m_cached_ctx.FRAME.Block() + (IsPageCopy() ? 0x20 : 0);
		GSVector4i new_valid = rt->m_valid;
		int offset_height = static_cast<int>((((frame_offset - rt->m_TEX0.TBP0) >> 5) / rt->m_TEX0.TBW) * frame_psm.pgs.y) + frame_psm.pgs.y;

		const int get_next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : m_backed_up_ctx;
		const GSDrawingContext& next_ctx = m_env.CTXT[get_next_ctx];
		const u32 safe_TBW = std::max(rt->m_TEX0.TBW, 1U);
		if (m_state_flush_reason == GSFlushReason::CONTEXTCHANGE && !IsPageCopy() && NextDrawMatchesShuffle() && next_ctx.FRAME.FBP > m_cached_ctx.FRAME.FBP && (next_ctx.FRAME.FBP < (m_cached_ctx.FRAME.FBP + safe_TBW)) &&
			(next_ctx.FRAME.FBP - m_cached_ctx.FRAME.FBP) < safe_TBW && (next_ctx.FRAME.FBP % safe_TBW) != ((m_cached_ctx.FRAME.FBP % safe_TBW) + 1))
		{
			offset_height += frame_psm.pgs.y;
		}

		new_valid.w = std::max(new_valid.w, offset_height);
		rt->UpdateValidity(new_valid, true);

		m_channel_shuffle_finish = true;
	}


	m_primitive_covers_without_gaps = NoGapsType::FullCover;
	m_conf.cb_ps.ChannelShuffleOffset = GSVector2(0, 0);

	return true;
}

void GSRendererHW::EmulateBlending(int rt_alpha_min, int rt_alpha_max, DATEOptions& date_options,
	GSTextureCache::Target* rt, bool can_scale_rt_alpha, bool& new_rt_alpha_scale)
{
	const GIFRegALPHA& ALPHA = m_context->ALPHA;
	{
		const bool PABE_skip = m_draw_env->PABE.PABE &&
			((GetAlphaMinMax().max < 128) || (GetAlphaMinMax().max == 128 && ALPHA.A == 0 && ALPHA.B == 1 && ALPHA.C == 0 && ALPHA.D == 1));

		if (PABE_skip || !(NeedsBlending() || IsCoverageAlpha()))
		{
			m_conf.blend = {};

			return;
		}
	}

	const GSDevice::FeatureSupport features(g_gs_device->Features());
	const GIFRegCOLCLAMP& COLCLAMP = m_draw_env->COLCLAMP;
	u8 AFIX = ALPHA.FIX;

	m_conf.ps.blend_a = ALPHA.A;
	m_conf.ps.blend_b = ALPHA.B;
	m_conf.ps.blend_c = ALPHA.C;
	m_conf.ps.blend_d = ALPHA.D;

#ifdef ENABLE_OGL_DEBUG
	static constexpr const char* col[3] = {"Cs", "Cd", "0"};
	static constexpr const char* alpha[3] = {"As", "Ad", "Af"};
	GL_INS("HW: EmulateBlending(): (%s - %s) * %s + %s", col[ALPHA.A], col[ALPHA.B], alpha[ALPHA.C], col[ALPHA.D]);
	GL_INS("HW: Draw AlphaMinMax: %d-%d, RT AlphaMinMax: %d-%d, AFIX: %u", GetAlphaMinMax().min, GetAlphaMinMax().max, rt_alpha_min, rt_alpha_max, AFIX);
#endif

	if ((!PRIM->TME || m_cached_ctx.TEX0.TFX != TFX_DECAL) && (!PRIM->FGE || m_draw_env->FOGCOL.U32[0] == 0) &&
		((m_vt.m_max.c == GSVector4i::zero()).mask() & 0xfff) == 0xfff)
	{
		if (!PRIM->TME || m_cached_ctx.TEX0.TFX == TFX_MODULATE || m_vt.m_max.c.a == 0)
		{
			if (m_conf.ps.blend_a == 0)
				m_conf.ps.blend_a = 2;

			if (m_conf.ps.blend_b == 0)
				m_conf.ps.blend_b = 2;

			if (m_conf.ps.blend_d == 0)
				m_conf.ps.blend_d = 2;
		}
	}
	if (m_conf.ps.blend_c == 1)
	{
		if (rt_alpha_min == rt_alpha_max)
		{
			AFIX = rt_alpha_min;
			m_conf.ps.blend_c = 2;
		}
		else if (m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_24)
		{
			AFIX = 128;
			m_conf.ps.blend_c = 2;
		}
	}
	else if (m_conf.ps.blend_c == 0 && GetAlphaMinMax().min == GetAlphaMinMax().max)
	{
		AFIX = GetAlphaMinMax().max;
		m_conf.ps.blend_c = 2;
	}

	const bool alpha_c0_eq_zero = (m_conf.ps.blend_c == 0 && GetAlphaMinMax().max == 0);
	const bool alpha_c0_eq_one = (m_conf.ps.blend_c == 0 && (GetAlphaMinMax().min == 128) && (GetAlphaMinMax().max == 128));
	const bool alpha_c0_high_min_one = (m_conf.ps.blend_c == 0 && GetAlphaMinMax().min > 128);
	const bool alpha_c0_high_max_one = (m_conf.ps.blend_c == 0 && GetAlphaMinMax().max > 128);
	const bool alpha_c0_eq_less_max_one = (m_conf.ps.blend_c == 0 && GetAlphaMinMax().max <= 128);
	const bool alpha_c1_high_min_one = (m_conf.ps.blend_c == 1 && rt_alpha_min > 128);
	const bool alpha_c1_high_max_one = (m_conf.ps.blend_c == 1 && rt_alpha_max > 128);
	const bool alpha_c1_eq_less_max_one = (m_conf.ps.blend_c == 1 && rt_alpha_max <= 128);
	bool alpha_c1_high_no_rta_correct = m_conf.ps.blend_c == 1 && !(new_rt_alpha_scale || can_scale_rt_alpha);
	const bool alpha_c2_eq_zero = (m_conf.ps.blend_c == 2 && AFIX == 0u);
	const bool alpha_c2_eq_one = (m_conf.ps.blend_c == 2 && AFIX == 128u);
	const bool alpha_c2_eq_less_one = (m_conf.ps.blend_c == 2 && AFIX <= 128u);
	const bool alpha_c2_high_one = (m_conf.ps.blend_c == 2 && AFIX > 128u);
	const bool alpha_eq_one = alpha_c0_eq_one || alpha_c2_eq_one;
	const bool alpha_high_one = alpha_c0_high_min_one || alpha_c2_high_one;
	const bool alpha_eq_less_one = alpha_c0_eq_less_max_one || alpha_c2_eq_less_one;

	if ((m_conf.ps.blend_a == m_conf.ps.blend_b) || ((m_conf.ps.blend_b == m_conf.ps.blend_d) && alpha_eq_one))
	{
		if (m_conf.ps.blend_a != m_conf.ps.blend_b)
			m_conf.ps.blend_d = m_conf.ps.blend_a;
		m_conf.ps.blend_a = 0;
		m_conf.ps.blend_b = 0;
		m_conf.ps.blend_c = 0;
	}
	else if (alpha_c0_eq_zero || alpha_c2_eq_zero)
	{
		m_conf.ps.blend_a = 0;
		m_conf.ps.blend_b = 0;
	}
	else if (COLCLAMP.CLAMP && m_conf.ps.blend_a == 2
		&& (m_conf.ps.blend_d == 2 || (m_conf.ps.blend_b == m_conf.ps.blend_d && (alpha_high_one || alpha_c1_high_min_one))))
	{
		m_conf.ps.blend_a = 0;
		m_conf.ps.blend_b = 0;
		m_conf.ps.blend_c = 0;
		m_conf.ps.blend_d = 2;
	}

	m_optimized_blend.A = m_conf.ps.blend_a;
	m_optimized_blend.B = m_conf.ps.blend_b;
	m_optimized_blend.C = m_conf.ps.blend_c;
	m_optimized_blend.D = m_conf.ps.blend_d;
	m_optimized_blend.FIX = AFIX;

	const bool blend_ad = m_conf.ps.blend_c == 1;
	bool blend_ad_alpha_masked = blend_ad && !m_conf.colormask.wa;
	const bool is_basic_blend = GSConfig.AccurateBlendingUnit != AccBlendLevel::Minimum;
	if (blend_ad_alpha_masked && ((is_basic_blend || (COLCLAMP.CLAMP == 0) || m_conf.require_one_barrier)))
	{
		m_conf.ps.a_masked = 1;
		m_conf.ps.blend_c = 0;
		m_conf.require_one_barrier |= true;
	}
	else
		blend_ad_alpha_masked = false;

	const u8 blend_index = static_cast<u8>(((m_conf.ps.blend_a * 3 + m_conf.ps.blend_b) * 3 + m_conf.ps.blend_c) * 3 + m_conf.ps.blend_d);
	HWBlend blend = GSDevice::GetBlend(blend_index);
	const int blend_flag = blend.flags;

	if (blend_ad_alpha_masked)
		m_conf.ps.blend_c = ALPHA.C;

	bool color_dest_blend = !!(blend_flag & BLEND_CD);

	const bool PABE = m_draw_env->PABE.PABE && GetAlphaMinMax().min < 128;

	bool color_dest_blend2 = !PABE && ((m_conf.ps.blend_a == 1 && m_conf.ps.blend_b == 2 && m_conf.ps.blend_d == 2) || (m_conf.ps.blend_a == 2 && m_conf.ps.blend_b == 1 && m_conf.ps.blend_d == 1)) &&
		(alpha_eq_less_one || (alpha_c1_eq_less_max_one && new_rt_alpha_scale));
	bool blend_zero_to_one_range = !PABE && ((m_conf.ps.blend_a == 0 && m_conf.ps.blend_b == 1 && m_conf.ps.blend_d == 1) || (blend_flag & BLEND_MIX3)) &&
		(alpha_eq_less_one || (alpha_c1_eq_less_max_one && new_rt_alpha_scale));

	bool accumulation_blend = !!(blend_flag & BLEND_ACCU);
	if (alpha_eq_one && (m_conf.ps.blend_a != m_conf.ps.blend_d) && blend.dst != GSDevice::CONST_ZERO)
		accumulation_blend = true;

	const bool blend_non_recursive = !!(blend_flag & BLEND_NO_REC);

	const bool blend_mix1 = !!(blend_flag & BLEND_MIX1) && !(m_conf.ps.blend_b == m_conf.ps.blend_d && alpha_high_one);
	const bool blend_mix2 = !!(blend_flag & BLEND_MIX2);
	const bool blend_mix3 = !!(blend_flag & BLEND_MIX3);
	bool blend_mix = (blend_mix1 || blend_mix2 || blend_mix3) && COLCLAMP.CLAMP;

	const bool no_prim_overlap = (m_prim_overlap == PRIM_OVERLAP_NO);

	const bool blend_multi_pass_support = !features.texture_barrier && no_prim_overlap && is_basic_blend && COLCLAMP.CLAMP;
	const bool bmix1_multi_pass1 = blend_multi_pass_support && blend_mix1 && (alpha_c0_high_max_one || alpha_c2_high_one) && m_conf.ps.blend_d == 2;
	const bool bmix1_multi_pass2 = blend_multi_pass_support && (blend_flag & BLEND_MIX1) && m_conf.ps.blend_b == m_conf.ps.blend_d && !m_conf.ps.dither && alpha_high_one;
	const bool bmix3_multi_pass = blend_multi_pass_support && blend_mix3 && !m_conf.ps.dither && alpha_high_one;
	blend_mix &= !(bmix1_multi_pass1 || bmix1_multi_pass2 || bmix3_multi_pass);

	const bool one_barrier = m_conf.require_one_barrier || blend_ad_alpha_masked;
	const bool prefer_sw_blend = (features.feedback_loops() && m_conf.require_full_barrier) || (m_conf.require_one_barrier && (no_prim_overlap || m_channel_shuffle)) || m_conf.ps.shuffle || (no_prim_overlap && (m_conf.tex == m_conf.rt));
	const bool free_blend = blend_non_recursive
	                        || accumulation_blend;

	bool sw_blending = false;
	const bool blend_multipass_group = blend_multi_pass_support && !features.texture_barrier &&
		(bmix1_multi_pass1 || bmix1_multi_pass2 || bmix3_multi_pass || (blend_flag & (BLEND_HW3 | BLEND_HW4 | BLEND_HW5 | BLEND_HW6 | BLEND_HW7 | BLEND_HW8 | BLEND_HW9)));

	const bool barriers_supported = features.feedback_loops();
	const bool blend_requires_barrier =
		(no_prim_overlap || barriers_supported)
		&& ((blend_flag & BLEND_A_MAX)
		|| (one_barrier && (no_prim_overlap || features.framebuffer_fetch))
		|| (!(blend_flag & BLEND_HW2) && !blend_multipass_group && (alpha_c2_high_one || alpha_c0_high_max_one) && no_prim_overlap)
		|| (blend_ad && !blend_multipass_group && no_prim_overlap && !new_rt_alpha_scale));

	switch (GSConfig.AccurateBlendingUnit)
	{
		case AccBlendLevel::Maximum:
			sw_blending |= true;
			accumulation_blend &= !barriers_supported || (GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp == 32);
			[[fallthrough]];
		case AccBlendLevel::Full:
			sw_blending |= m_conf.ps.blend_a != m_conf.ps.blend_b && alpha_c0_high_max_one;
			[[fallthrough]];
		case AccBlendLevel::High:
			sw_blending |= (alpha_c1_high_max_one || alpha_c1_high_no_rta_correct) || (m_conf.ps.blend_a != m_conf.ps.blend_b && alpha_c2_high_one);
			[[fallthrough]];
		case AccBlendLevel::Medium:
			sw_blending |= barriers_supported && m_vt.m_primclass == GS_SPRITE_CLASS && ComputeDrawlistGetSize(rt->m_scale) < 100;
			sw_blending &= (no_prim_overlap || barriers_supported);
			[[fallthrough]];
		case AccBlendLevel::Basic:
		default:
			color_dest_blend &= !m_conf.ps.dither;
			color_dest_blend2 &= !(prefer_sw_blend || m_conf.ps.dither);
			blend_zero_to_one_range &= !(prefer_sw_blend || m_conf.ps.dither);
			accumulation_blend &= !prefer_sw_blend;
			sw_blending |= blend_requires_barrier || prefer_sw_blend;
			sw_blending |= free_blend;
			blend_mix &= !sw_blending;
			sw_blending |= blend_mix;
			[[fallthrough]];
		case AccBlendLevel::Minimum:
			sw_blending |= blend_non_recursive;
			break;
	}

	const bool force_sw_blending =
		(features.framebuffer_fetch && (one_barrier || m_conf.require_full_barrier)) ||

		(m_conf.ps.IsFeedbackLoopDepth() && !features.depth_feedback) ||
		
		GSConfig.UseDebugBlend;
	
	if (force_sw_blending)
	{
		sw_blending = true;
		color_dest_blend = false;
		accumulation_blend = false;
		blend_mix = false;
		color_dest_blend2 = false;
		blend_zero_to_one_range = false;
	}

	if (COLCLAMP.CLAMP == 0)
	{
		bool has_colclip_texture = g_gs_device->GetColorClipTexture() != nullptr;

		if (has_colclip_texture)
		{
			GSTexture* colclip_texture = g_gs_device->GetColorClipTexture();

			if (colclip_texture->GetSize() != rt->m_texture->GetSize())
			{
				GL_CACHE("HW: Pre-Blend resolve of colclip due to size change! Address: %x", rt->m_TEX0.TBP0);
				const GSVector4 colclip_texture_dims = GSVector4(GSVector4i(colclip_texture->GetSize()).xyxy());
				g_gs_device->StretchRect(
					colclip_texture, GSVector4(m_conf.colclip_update_area) / colclip_texture_dims,
					rt->m_texture, GSVector4(m_conf.colclip_update_area),
					ShaderConvert::COLCLIP_RESOLVE, Nearest);

				g_gs_device->Recycle(colclip_texture);

				g_gs_device->SetColorClipTexture(nullptr);

				has_colclip_texture = false;
			}
		}

		const bool free_colclip = !has_colclip_texture && (features.framebuffer_fetch || no_prim_overlap || blend_non_recursive);
		if (color_dest_blend || color_dest_blend2 || blend_zero_to_one_range)
		{
			GL_INS("HW: COLCLIP mode DISABLED");
			sw_blending = false;
			m_conf.colclip_mode = (has_colclip_texture && !NextDrawColClip()) ? GSHWDrawConfig::ColClipMode::ResolveOnly : GSHWDrawConfig::ColClipMode::NoModify;
		}
		else if (free_colclip)
		{
			GL_INS("HW: COLCLIP Free mode ENABLED");
			m_conf.ps.colclip  = 1;
			sw_blending        = true;
			accumulation_blend = false;
			blend_mix          = false;
			m_conf.colclip_mode = (has_colclip_texture && !NextDrawColClip()) ? GSHWDrawConfig::ColClipMode::ResolveOnly : GSHWDrawConfig::ColClipMode::NoModify;
		}
		else if (accumulation_blend)
		{
			GL_INS("HW: COLCLIP ACCU HW mode ENABLED");
			m_conf.ps.colclip_hw = 1;
			sw_blending = true;

			m_conf.colclip_mode = has_colclip_texture ? (NextDrawColClip() ? GSHWDrawConfig::ColClipMode::NoModify : GSHWDrawConfig::ColClipMode::ResolveOnly) : (NextDrawColClip() ? GSHWDrawConfig::ColClipMode::ConvertOnly : GSHWDrawConfig::ColClipMode::ConvertAndResolve);
		}
		else if (sw_blending)
		{
			GL_INS("HW: COLCLIP SW mode ENABLED");
			m_conf.ps.colclip = 1;
			m_conf.colclip_mode = (has_colclip_texture && !NextDrawColClip()) ? GSHWDrawConfig::ColClipMode::ResolveOnly : GSHWDrawConfig::ColClipMode::NoModify;
		}
		else
		{
			GL_INS("HW: COLCLIP HW mode ENABLED");
			m_conf.ps.colclip_hw = 1;
			m_conf.colclip_mode = has_colclip_texture ? (NextDrawColClip() ? GSHWDrawConfig::ColClipMode::NoModify : GSHWDrawConfig::ColClipMode::ResolveOnly) : (NextDrawColClip() ? GSHWDrawConfig::ColClipMode::ConvertOnly : GSHWDrawConfig::ColClipMode::ConvertAndResolve);
		}

		m_conf.colclip_frame = m_cached_ctx.FRAME;
	}

	if (PABE)
	{

		if (sw_blending)
		{
			if (accumulation_blend && (blend.op != GSDevice::OP_REV_SUBTRACT))
			{

				m_conf.ps.pabe = 1;
			}
			else if (features.feedback_loops())
			{
				color_dest_blend   = false;
				accumulation_blend = false;
				blend_mix          = false;
				m_conf.ps.pabe     = 1;

				if (m_conf.ps.colclip_hw)
				{
					const bool has_colclip_texture = g_gs_device->GetColorClipTexture() != nullptr;
					m_conf.ps.colclip_hw = 0;
					m_conf.ps.colclip = 1;
					m_conf.colclip_mode = has_colclip_texture ? GSHWDrawConfig::ColClipMode::EarlyResolve : GSHWDrawConfig::ColClipMode::NoModify;
				}
			}
			else
			{
				m_conf.ps.pabe = !(accumulation_blend || blend_mix);
			}

			GL_INS("HW: PABE mode %s", m_conf.ps.pabe ? "ENABLED" : "DISABLED");
		}
	}

	if (color_dest_blend)
	{
		m_conf.blend = {};
		m_conf.ps.blend_a = m_conf.ps.blend_b = m_conf.ps.blend_c = m_conf.ps.blend_d = 0;
		sw_blending = false;

		m_conf.colormask.wrgba &= 0x8;

		if (can_scale_rt_alpha && !new_rt_alpha_scale && m_conf.colormask.wa)
		{
			const bool afail_fb_only = m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY;
			const bool full_cover = rt->m_valid.rintersect(m_r).eq(rt->m_valid) && m_primitive_covers_without_gaps == NoGapsType::FullCover &&
				!(date_options.enabled || !afail_fb_only || !IsDepthAlwaysPassing());

			new_rt_alpha_scale = full_cover;
		}

		return;
	}
	else if (sw_blending)
	{
		if (m_conf.ps.blend_c == 2)
			m_conf.cb_ps.TA_MaxDepth_Af.a = static_cast<float>(AFIX) / 128.0f;

		if (accumulation_blend)
		{
			m_conf.blend = {true, GSDevice::CONST_ONE, GSDevice::CONST_ONE, blend.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};

			if (m_conf.ps.blend_a == 1)
				m_conf.ps.blend_a = 2;
			if (m_conf.ps.blend_b == 1)
				m_conf.ps.blend_b = 2;
			if (m_conf.ps.blend_d == 1)
				m_conf.ps.blend_d = 2;

			if (m_conf.ps.blend_a == 2)
			{
				pxAssert(m_conf.ps.blend_d == 2 || alpha_eq_one);
				m_conf.ps.blend_a = m_conf.ps.blend_d;
				m_conf.ps.blend_d = 2;
			}

			if (blend.op == GSDevice::OP_REV_SUBTRACT)
			{
				pxAssert(m_conf.ps.blend_a == 2);
				if (m_conf.ps.colclip_hw)
				{
					m_conf.blend.op = GSDevice::OP_ADD;
				}
				else
				{
					m_conf.ps.blend_a = m_conf.ps.blend_b;
					m_conf.ps.blend_b = 2;
				}
			}
			else if (m_conf.ps.pabe)
			{
				m_conf.blend.dst_factor = GSDevice::SRC1_COLOR;
			}

			m_conf.ps.no_color1 &= (m_conf.ps.pabe == 0);
		}
		else if (blend_mix)
		{
			if (m_conf.ps.dither)
			{
				const bool can_dither = (m_conf.ps.blend_a == 0 && m_conf.ps.blend_b == 1) || (m_conf.ps.blend_a == 1 && m_conf.ps.blend_b == 0);
				m_conf.ps.dither = can_dither;
				m_conf.ps.dither_adjust = can_dither;
			}

			if (blend_mix1)
			{
				if (m_conf.ps.blend_b == m_conf.ps.blend_d && (alpha_c0_high_min_one || alpha_c1_high_min_one || alpha_c2_high_one))
				{
					blend.dst = GSDevice::SRC1_COLOR;
					blend.op = GSDevice::OP_SUBTRACT;
					m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::BMIX1_ALPHA_HIGH_ONE);
				}
				else if (m_conf.ps.blend_a == m_conf.ps.blend_d)
				{
					m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::BMIX1_SRC_HALF);
				}

				m_conf.ps.blend_a = 0;
				m_conf.ps.blend_b = 2;
				m_conf.ps.blend_d = 2;
			}
			else if (blend_mix2)
			{
				blend.dst = GSDevice::SRC1_COLOR;
				m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::BMIX2_OVERFLOW);

				m_conf.ps.blend_a = 0;
				m_conf.ps.blend_b = 2;
				m_conf.ps.blend_d = 0;
			}
			else if (blend_mix3)
			{
				m_conf.ps.blend_a = 2;
				m_conf.ps.blend_b = 0;
				m_conf.ps.blend_d = 0;
			}

			m_conf.ps.no_color1 &= !GSDevice::IsDualSourceBlendFactor(blend.dst);

			m_conf.blend = {true, GSDevice::CONST_ONE, blend.dst, blend.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, m_conf.ps.blend_c == 2, AFIX};
			m_conf.ps.blend_mix = (blend.op == GSDevice::OP_REV_SUBTRACT) ? 2 : 1;
		}
		else
		{
			m_conf.blend = {};

			const bool blend_non_recursive_one_barrier = blend_non_recursive && blend_ad_alpha_masked;
			if (blend_non_recursive_one_barrier)
				m_conf.require_one_barrier |= true;
			else if (features.feedback_loops())
				m_conf.require_full_barrier |= !blend_non_recursive;
			else
				m_conf.require_one_barrier |= !blend_non_recursive;
		}
	}
	else
	{
		m_conf.ps.blend_a = 0;
		m_conf.ps.blend_b = 0;
		m_conf.ps.blend_d = 0;

		const bool rta_correction = can_scale_rt_alpha && !blend_ad_alpha_masked && m_conf.ps.blend_c == 1 && !(blend_flag & BLEND_A_MAX);
		if (rta_correction)
		{
			const bool afail_always_fb_alpha = m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY || (m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp != 32);
			const bool always_passing_alpha = !m_cached_ctx.TEST.ATE || afail_always_fb_alpha || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ATST_ALWAYS);
			const bool full_cover = rt->m_valid.rintersect(m_r).eq(rt->m_valid) && m_primitive_covers_without_gaps == NoGapsType::FullCover &&
				!(date_options.primid || date_options.barrier || !always_passing_alpha || !IsDepthAlwaysPassing());

			if (!full_cover)
			{
				rt->ScaleRTAlpha();
				m_conf.rt = rt->m_texture;
			}

			new_rt_alpha_scale = true;
			alpha_c1_high_no_rta_correct = false;

			m_conf.ps.rta_correction = rt->m_rt_alpha_scale;
		}

		if (blend_multi_pass_support)
		{
			const HWBlend blend_multi_pass = GSDevice::GetBlend(blend_index);
			if (bmix1_multi_pass1)
			{
				blend.src = GSDevice::CONST_ONE;
				blend.dst = GSDevice::CONST_ONE;
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = static_cast<u8>(HWBlendType::SRC_ALPHA_DST_FACTOR);
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, (m_conf.ps.blend_c == 2) ? GSDevice::CONST_COLOR : GSDevice::SRC1_COLOR, GSDevice::OP_ADD, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, m_conf.ps.blend_c == 2, AFIX};
			}
			else if (bmix1_multi_pass2)
			{
				m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::SRC_INV_DST_BLEND_HALF);
				blend.src = GSDevice::CONST_ONE;
				blend.dst = GSDevice::SRC1_COLOR;
				blend.op = GSDevice::OP_SUBTRACT;
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = static_cast<u8>(HWBlendType::SRC_ONE_DST_FACTOR);
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (bmix3_multi_pass)
			{
				m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::INV_SRC_DST_BLEND_HALF);
				blend.src = GSDevice::CONST_ONE;
				blend.dst = GSDevice::SRC1_COLOR;
				blend.op = GSDevice::OP_REV_SUBTRACT;
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = static_cast<u8>(HWBlendType::SRC_ONE_DST_FACTOR);
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if ((alpha_c0_high_max_one || alpha_c1_high_no_rta_correct || alpha_c2_high_one) && (blend_flag & BLEND_HW1))
			{
				m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::SRC_HALF_ONE_DST_FACTOR);
				blend.dst = (m_conf.ps.blend_c == 1) ? GSDevice::DST_ALPHA : GSDevice::SRC1_COLOR;
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = static_cast<u8>(HWBlendType::SRC_ONE_DST_FACTOR);
				m_conf.blend_multi_pass.blend = {true, blend_multi_pass.src, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW3))
			{
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend = {true, blend_multi_pass.src, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if ((alpha_c0_high_max_one || alpha_c2_high_one) && (blend_flag & BLEND_HW4))
			{
				const u8 dither = m_conf.ps.dither;
				m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::SRC_ALPHA_DST_FACTOR);
				m_conf.ps.dither = 0;
				blend.src = GSDevice::DST_COLOR;
				blend.dst = (m_conf.ps.blend_c == 2) ? GSDevice::CONST_COLOR : GSDevice::SRC1_COLOR;
				blend.op = GSDevice::OP_ADD;
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.dither = dither * GSConfig.Dithering;
				m_conf.blend_multi_pass.blend = {true, blend_multi_pass.src, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW5))
			{
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = static_cast<u8>(HWBlendType::SRC_ONE_DST_FACTOR);
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, GSDevice::OP_ADD, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW6))
			{
				m_conf.ps.blend_c = 2;
				AFIX = 64;
				blend.src = GSDevice::CONST_COLOR;
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = static_cast<u8>(HWBlendType::SRC_ONE_DST_FACTOR);
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, GSDevice::OP_ADD, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW7))
			{
				m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::SRC_HALF_ONE_DST_FACTOR);
				blend.src = GSDevice::DST_COLOR;
				blend.dst = GSDevice::DST_ALPHA;
				blend.op = GSDevice::OP_SUBTRACT;
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = static_cast<u8>(HWBlendType::SRC_ONE_DST_FACTOR);
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_COLOR, GSDevice::CONST_ONE, GSDevice::OP_ADD, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (blend_flag & BLEND_HW8)
			{
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend_hw = static_cast<u8>(HWBlendType::SRC_DOUBLE);
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_ALPHA, GSDevice::CONST_ONE, blend_multi_pass.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}
			else if (alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW9))
			{
				m_conf.blend_multi_pass.enable = true;
				m_conf.blend_multi_pass.blend = {true, GSDevice::DST_ALPHA, GSDevice::CONST_ONE, GSDevice::OP_REV_SUBTRACT, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0};
			}

			m_conf.blend_multi_pass.no_color1 = !m_conf.blend_multi_pass.enable ||
			                                    (!GSDevice::IsDualSourceBlendFactor(m_conf.blend_multi_pass.blend.src_factor) &&
			                                     !GSDevice::IsDualSourceBlendFactor(m_conf.blend_multi_pass.blend.dst_factor));
		}

		if (!m_conf.blend_multi_pass.enable && blend_flag & BLEND_HW1)
		{
			m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::SRC_ONE_DST_FACTOR);
		}
		else if (blend_flag & BLEND_HW2)
		{
			m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::SRC_ALPHA_DST_FACTOR);
		}
		else if (!m_conf.blend_multi_pass.enable && alpha_c1_high_no_rta_correct && (blend_flag & BLEND_HW3))
		{
			m_conf.ps.blend_hw = static_cast<u8>(HWBlendType::SRC_DOUBLE);
		}

		if (m_conf.ps.blend_c == 2 && (m_conf.ps.blend_hw == static_cast<u8>(HWBlendType::SRC_ALPHA_DST_FACTOR)
			|| m_conf.ps.blend_hw == static_cast<u8>(HWBlendType::SRC_HALF_ONE_DST_FACTOR)
			|| m_conf.ps.blend_hw == static_cast<u8>(HWBlendType::SRC_INV_DST_BLEND_HALF)
			|| m_conf.ps.blend_hw == static_cast<u8>(HWBlendType::INV_SRC_DST_BLEND_HALF)
			|| m_conf.blend_multi_pass.blend_hw == static_cast<u8>(HWBlendType::SRC_ALPHA_DST_FACTOR)))
		{
			m_conf.cb_ps.TA_MaxDepth_Af.a = static_cast<float>(AFIX) / 128.0f;
		}

		const GSDevice::BlendFactor src_factor_alpha = m_conf.blend_multi_pass.enable ? GSDevice::CONST_ZERO : GSDevice::CONST_ONE;
		const GSDevice::BlendFactor dst_factor_alpha = m_conf.blend_multi_pass.enable ? GSDevice::CONST_ONE : GSDevice::CONST_ZERO;
		m_conf.blend = {true, blend.src, blend.dst, blend.op, src_factor_alpha, dst_factor_alpha, m_conf.ps.blend_c == 2, AFIX};

		m_conf.ps.no_color1 &= !GSDevice::IsDualSourceBlendFactor(m_conf.blend.src_factor) &&
		                       !GSDevice::IsDualSourceBlendFactor(m_conf.blend.dst_factor);
	}

	if (m_conf.blend.op == GSDevice::OP_REV_SUBTRACT)
		m_conf.ps.round_inv = 1;

	if (sw_blending && date_options.primid && m_conf.require_full_barrier &&
		(features.texture_barrier || (features.multidraw_fb_copy && !no_prim_overlap)))
	{
		GL_PERF("DATE: Swap DATE_PRIMID with DATE_BARRIER");
		date_options.primid = false;
		date_options.barrier = true;
	}
}

__fi void GSRendererHW::GetForcedROVUsage(bool& rov_color, bool& rov_depth)
{
	if (rov_color == rov_depth)
		return;

	if (rov_depth)
	{
		GL_INS("ROV: Depth ROV forces color ROV");
		rov_color = true;
		return;
	}

	if (m_conf.ps.IsFeedbackLoopDepth() && rov_color)
	{
		GL_INS("ROV: Feedback compatibility forces color and depth ROV");
		rov_depth = true;
		return;
	}

	
	const bool date = m_cached_ctx.TEST.DATE;

	const bool atst_needs_depth = m_cached_ctx.TEST.ATE &&
		(m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY || m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY);

	if (rov_color && (m_conf.ps.HasShaderDiscard() || m_conf.ps.HasDepthOutput() || date || atst_needs_depth))
	{
		GL_INS("ROV: Color ROV with shader discard/depth write forces depth ROV");
		rov_depth = true;
	}
}

void GSRendererHW::DetermineROVUsage(GSTextureCache::Target* rt, GSTextureCache::Target* ds)
{
	const GSDevice::FeatureSupport& features = g_gs_device->Features();

	if (!(GSConfig.HWROV && features.rov))
		return;

	GL_PUSH("HW: ROV Setup");

	if (features.framebuffer_fetch)
	{
		GL_INS("ROV: Disabled because have FB-fetch");
		return;
	}

	if (rt && rt->m_texture == m_conf.tex && !m_conf.ps.tex_is_fb)
	{
		GL_INS("ROV: Disabled because tex is RT and not sampling from current pixel");
		return;
	}

	if (ds && ds->m_texture == m_conf.tex)
	{
		GL_INS("ROV: Disabled because tex is depth");
		return;
	}

	const bool color_write = rt && m_conf.colormask.wrgba != 0;
	const bool depth_write = ds && m_cached_ctx.DepthWrite();

	bool full_barrier = m_conf.require_full_barrier;

	bool barriers_color = m_conf.require_full_barrier && m_conf.ps.IsFeedbackLoopRT();
	bool barriers_depth = m_conf.require_full_barrier && m_conf.ps.IsFeedbackLoopDepth();

	if (m_conf.alpha_second_pass.enable)
	{
		full_barrier |= m_conf.alpha_second_pass.require_full_barrier;
		barriers_color |= m_conf.alpha_second_pass.require_full_barrier && m_conf.alpha_second_pass.ps.IsFeedbackLoopRT();
		barriers_depth |= m_conf.alpha_second_pass.require_full_barrier && m_conf.alpha_second_pass.ps.IsFeedbackLoopDepth();
	}

	const bool color_is_rov = rt && rt->m_texture->IsShaderWriteMode();
	const bool depth_is_rov = ds && ds->m_texture->IsShaderWrite();

	bool use_rov_color = (color_write && barriers_color) || color_is_rov;
	bool use_rov_depth = (depth_write && barriers_depth) || depth_is_rov;

	if (rt && ds)
		GetForcedROVUsage(use_rov_color, use_rov_depth);

	u32 barriers = 1; 
	if (full_barrier)
	{
		if (m_drawlist.size() > 0)
		{
			barriers = static_cast<u32>(m_drawlist.size());
		}
		else
		{
#if PCSX2_DEVBUILD
			barriers = INT_MAX;
#else
			barriers = 2;
#endif
			GetPrimitiveOverlapDrawlist(false, false, 1.0f, &barriers);
		}
	}

	const bool activate = (use_rov_color != color_is_rov || use_rov_depth != depth_is_rov) && barriers >= 2;

	if (!color_is_rov && !depth_is_rov && !activate)
	{
		GL_ROV("No ROV usage: Draw=%05lld | C=%016p | D=%016p | BAR=%d.",
			s_n, rt ? rt->m_texture : nullptr, ds ? ds->m_texture : nullptr, barriers);
		return;
	}
	
	if (activate)
	{
		GL_ROV("ROV activated: Draw=%05lld | C=%016p | D=%016p | BAR=%d | C=[%d=>%d] | D=[%d=>%d].",
			s_n, rt ? rt->m_texture : nullptr, ds ? ds->m_texture : nullptr, barriers,
			color_is_rov, use_rov_color, depth_is_rov, use_rov_depth);
	}
	else
	{
		GL_ROV("ROV continued: Draw=%05lld | C=%016p | D=%016p | BAR=%d | C=%d | D=%d.",
			s_n, rt ? rt->m_texture : nullptr, ds ? ds->m_texture : nullptr, barriers,
			color_is_rov, depth_is_rov);
	}

	GL_INS("ROV: Color ROV %s / depth ROV %s",
		use_rov_color ? "enabled" : "disabled", use_rov_depth ? "enabled" : "disabled");

	ConfigureROV(use_rov_color, use_rov_depth);
}

void GSRendererHW::ConfigureROV(bool color_rov, bool depth_rov)
{
	if (depth_rov)
	{
		m_conf.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
		const bool depth_write = m_cached_ctx.DepthWrite();
		GL_INS("ROV: Using %s depth ROV", depth_write ? "read/write" : "read-only");
		ConfigureDepthFeedback(true);
		m_conf.ps.rov_depth = depth_write ? GSHWDrawConfig::PS_ROV_DEPTH::READ_WRITE : GSHWDrawConfig::PS_ROV_DEPTH::READ_ONLY;
	}

	if (color_rov)
	{
		if (m_conf.colormask.wrgba != 0)
		{
			const GSVector4i fbmask = GSVector4i(m_conf.colormask.wr ? 0 : 0xFF, m_conf.colormask.wg ? 0 : 0xFF,
			                                     m_conf.colormask.wb ? 0 : 0xFF, m_conf.colormask.wa ? 0 : 0xFF);
			if (!m_conf.ps.fbmask)
			{
				m_conf.cb_ps.FbMask = fbmask;
			}
			else
			{
				m_conf.cb_ps.FbMask |= fbmask;
			}
			GL_INS("ROV: FbMask={R=%x, G=%x, B=%x, A=%x}",
				m_conf.cb_ps.FbMask.r, m_conf.cb_ps.FbMask.g, m_conf.cb_ps.FbMask.b, m_conf.cb_ps.FbMask.a);
		}
		else
		{
			m_conf.ps.no_color = true;
		}

		if (m_conf.IsBlending())
		{
			GL_INS("ROV: Using SW blend%s", m_conf.blend.enable ? " and disabling HW" : "");
			m_conf.ps.blend_a = m_optimized_blend.A;
			m_conf.ps.blend_b = m_optimized_blend.B;
			m_conf.ps.blend_c = m_optimized_blend.C;
			m_conf.ps.blend_d = m_optimized_blend.D;

			if (m_conf.ps.blend_c == ALPHA_C_FIX)
				m_conf.cb_ps.TA_MaxDepth_Af.a = m_optimized_blend.FIX / 128.0f;

			m_conf.blend = {};
			m_conf.ps.blend_hw = false;
			m_conf.ps.blend_mix = false;
			m_conf.blend_multi_pass = {};

			if (!m_conf.ps.no_color1)
			{
				GL_INS("ROV: Disabling dual source blending");
				m_conf.ps.no_color1 = true;
			}

			m_conf.ps.round_inv = false;
			m_conf.ps.a_masked = false;
		}

		if (m_conf.ps.dither)
		{
			m_conf.ps.dither_adjust = false;
		}

		if (m_conf.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off)
		{
			GL_INS("ROV: Using DATE Full%s",
				(m_conf.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Full) ? " and replace current method" : "");

			if (m_conf.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Full)
			{
				m_conf.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Full;
				m_conf.depth.date = false;
				m_conf.depth.date_one = false;
				m_conf.ps.date = 5 + m_cached_ctx.TEST.DATM;
				m_conf.datm = static_cast<SetDATM>(0);
			}
		}

		if (m_conf.ps.colclip_hw)
		{
			GL_INS("ROV: Replacing colclip HW with SW");
			const bool has_colclip_texture = g_gs_device->GetColorClipTexture() != nullptr;
			m_conf.ps.colclip_hw = 0;
			m_conf.ps.colclip = true;
			m_conf.colclip_mode = has_colclip_texture ? GSHWDrawConfig::ColClipMode::EarlyResolve : GSHWDrawConfig::ColClipMode::NoModify;
		}

		const bool PABE = m_draw_env->PABE.PABE && GetAlphaMinMax().min < 128;
		if (m_conf.IsBlending() && PABE && !m_conf.ps.pabe)
		{
			GL_INS("ROV: Enabling PABE");
			m_conf.ps.pabe = true;
		}

		if (m_cached_ctx.TEST.ATE && m_conf.alpha_test != GSHWDrawConfig::AlphaTestMode::KEEP)
		{
			GL_INS("ROV: Using SW feedback alpha test%s", m_conf.alpha_second_pass.enable ?
				" and disabling alpha second pass" : "");

			m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::FEEDBACK;

			GSHWDrawConfig::PS_ATST ps_atst;
			float ps_aref;
			GetAlphaTestConfigPS(m_cached_ctx.TEST.ATST, m_cached_ctx.TEST.AREF, false, ps_atst, ps_aref);
			m_conf.ps.atst = ps_atst;
			m_conf.ps.afail = static_cast<GSHWDrawConfig::PS_AFAIL>(m_cached_ctx.TEST.AFAIL);
			if (m_cached_ctx.DepthWrite() && m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY)
			{
				pxAssert(depth_rov);
				m_conf.ps.afail = PS_AFAIL::RGB_ONLY_SW_Z;
			}
			m_conf.cb_ps.FogColor_AREF.a = ps_aref;

			GL_INS("ROV: Using ATST=%d, AFAIL=%d, AREF=%.2f", ps_atst, static_cast<u32>(m_conf.ps.afail), ps_aref);

			if (m_conf.alpha_second_pass.enable)
			{
				m_conf.alpha_second_pass = {};
			}
		}

		m_conf.ps.rov_color = true;
	}

	if (color_rov || depth_rov)
	{
		m_conf.require_full_barrier = false;
		m_conf.require_one_barrier = false;
	}
}

void GSRendererHW::ConvertTextureTypeROVSingle(GSTextureCache::Target* tgt, bool shader_write)
{
	const bool depth = (tgt->m_type == GSTextureCache::DepthStencil);

	GSTexture* old_tex = depth ? m_conf.ds : m_conf.rt;

	const u32 vr_layers = old_tex->GetArrayLayers();
	const GSTexture::Usage usage = shader_write ? GSTexture::ShaderWriteTarget : GSTexture::FeedbackTarget;
	if (GSTexture* new_tex = depth ?
		(shader_write ?
			g_gs_device->FetchSurface(usage, old_tex->GetSize(), 1, GSTexture::Format::DepthColor, false, true, vr_layers) :
			g_gs_device->CreateDepthStencil(old_tex->GetSize(), false, true, vr_layers)) :
			g_gs_device->FetchSurface(usage, old_tex->GetSize(), 1, GSTexture::Format::Color, false, true, vr_layers))
	{
		switch (old_tex->GetState())
		{
			case GSTexture::State::Cleared:
				if (depth)
					g_gs_device->ClearDepth(new_tex, old_tex->GetClearDepth());
				else
					g_gs_device->ClearRenderTarget(new_tex, old_tex->GetClearColor());
				break;
			case GSTexture::State::Invalidated:
				g_gs_device->InvalidateRenderTarget(new_tex);
				break;
			case GSTexture::State::Dirty:
				g_gs_device->StretchRectAuto(old_tex, new_tex, Nearest);

				g_perfmon.Put(GSPerfMon::TextureCopiesROV, 1.0);
				g_perfmon.Put(GSPerfMon::DrawCallsROV, 1.0);
				break;
			default:
				pxAssert(false);
				break;
		}

#if PCSX2_DEVBUILD
		new_tex->SetDebugName(tgt->m_texture->GetDebugName());
#endif

		if (tgt->m_texture == old_tex)
		{
			GL_CACHE("HW: Replaced texture for %s @ 0x%04x", depth ? "DS" : "RT", tgt->m_TEX0.TBP0);
			tgt->m_texture = new_tex;
		}
		else
		{
			pxAssert(depth && g_texture_cache->GetTemporaryZ() == old_tex);
			GL_CACHE("HW: Replaced texture for temporary Z @ 0x%04x", g_texture_cache->GetTemporaryZInfo().ZBP);
			g_texture_cache->SetTemporaryZ(new_tex);
		}

		if (depth)
			m_conf.ds = new_tex;
		else
			m_conf.rt = new_tex;

		g_gs_device->Recycle(old_tex);
	}
}

void GSRendererHW::ConvertTextureTypeROV(GSTextureCache::Target* rt, GSTextureCache::Target* ds)
{
	if (ds)
	{
		if (m_conf.ps.HasDepthROV() && !ds->m_texture->IsShaderWrite())
		{
			GL_PUSH("HW: Convert DepthStencil -> DepthColor for ROV.");
			ConvertTextureTypeROVSingle(ds, true);
		}
		else if (!m_conf.ps.HasDepthROV() && !ds->m_texture->IsDepthStencil())
		{
			GL_PUSH("HW: Convert DepthColor -> DepthStencil for non-ROV.");
			ConvertTextureTypeROVSingle(ds, false);
		}
	}

	if (rt && m_conf.ps.HasColorROV() && !rt->m_texture->IsShaderWrite())
	{
		GL_PUSH("HW: Convert RenderTarget -> RenderTarget (shader write) for ROV.");
		ConvertTextureTypeROVSingle(rt, true);
	}
}

__ri static constexpr bool IsRedundantClamp(u8 clamp, u32 clamp_min, u32 clamp_max, u32 tsize)
{
	const u32 textent = (1u << tsize) - 1u;
	if (clamp == CLAMP_REGION_CLAMP)
		return (clamp_min == 0 && clamp_max >= textent);
	else if (clamp == CLAMP_REGION_REPEAT)
		return (clamp_max == 0 && clamp_min == textent);
	else
		return false;
}

__ri static constexpr u8 EffectiveClamp(u8 clamp, bool has_region)
{
	return (clamp >= CLAMP_REGION_CLAMP && has_region) ? (clamp ^ 3) : clamp;
}

__ri void GSRendererHW::EmulateTextureSampler(const GSTextureCache::Target* rt, const GSTextureCache::Target* ds, GSTextureCache::Source* tex,
	const TextureMinMaxResult& tmm, GSDevice::RecycledTexture& src_copy)
{
	if (!m_channel_shuffle)
	{
		m_conf.cb_ps.ChannelShuffleOffset = GSVector2(0, 0);
		m_conf.tex = tex->m_texture;
	}
	m_conf.pal = tex->m_palette;

	GSTextureCache::SourceRegion source_region = tex->GetRegion();
	bool target_region = tex->IsFromTarget() && source_region.HasEither();
	GSVector2i unscaled_size = target_region ? tex->GetRegionSize() : tex->GetUnscaledSize();
	float scale = tex->GetScale();
	HandleTextureHazards(rt, ds, tex, tmm, source_region, target_region, unscaled_size, scale, src_copy);

	const float scale_factor = scale;
	const float scale_rt = rt ? rt->GetScale() : ds->GetScale();

	m_conf.cb_ps.ScaleFactor = GSVector4(scale_factor * (1.0f / 16.0f), 1.0f / scale_factor, scale_rt, 0.0f);

	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[tex->m_TEX0.PSM];
	const GSLocalMemory::psm_t& cpsm = psm.pal > 0 ? GSLocalMemory::m_psm[m_cached_ctx.TEX0.CPSM] : psm;

	[[maybe_unused]] static constexpr const char* clamp_modes[] = {"REPEAT", "CLAMP", "REGION_CLAMP", "REGION_REPEAT"};
	const bool redundant_wms = IsRedundantClamp(m_cached_ctx.CLAMP.WMS, m_cached_ctx.CLAMP.MINU,
	                                            m_cached_ctx.CLAMP.MAXU, m_cached_ctx.TEX0.TW);
	const bool redundant_wmt = IsRedundantClamp(m_cached_ctx.CLAMP.WMT, m_cached_ctx.CLAMP.MINV,
	                                            m_cached_ctx.CLAMP.MAXV, m_cached_ctx.TEX0.TH);
	const u8 wms = EffectiveClamp(m_cached_ctx.CLAMP.WMS, !tex->m_target && (source_region.HasX() || redundant_wms));
	const u8 wmt = EffectiveClamp(m_cached_ctx.CLAMP.WMT, !tex->m_target && (source_region.HasY() || redundant_wmt));
	const bool complex_wms_wmt = !!((wms | wmt) & 2) || target_region;
	GL_CACHE("HW: FST: %s WMS: %s [%s%s] WMT: %s [%s%s] Complex: %d TargetRegion: %d MINU: %d MAXU: %d MINV: %d MAXV: %d",
		PRIM->FST ? "UV" : "STQ", clamp_modes[m_cached_ctx.CLAMP.WMS], redundant_wms ? "redundant," : "",
		clamp_modes[wms], clamp_modes[m_cached_ctx.CLAMP.WMT], redundant_wmt ? "redundant," : "", clamp_modes[wmt],
		complex_wms_wmt, target_region, m_cached_ctx.CLAMP.MINU, m_cached_ctx.CLAMP.MAXU, m_cached_ctx.CLAMP.MINV,
		m_cached_ctx.CLAMP.MAXV);

	const bool need_mipmap = IsMipMapDraw();
	const bool shader_emulated_sampler = tex->m_palette || (tex->m_target && !m_conf.ps.shuffle && cpsm.fmt != 0) ||
	                                     complex_wms_wmt || psm.depth || target_region;
	const bool can_trilinear = !tex->m_palette && !tex->m_target && !m_conf.ps.shuffle;
	const bool trilinear_manual = need_mipmap && GSConfig.HWMipmap;

	bool bilinear = m_vt.IsLinear();
	int trilinear = 0;
	bool trilinear_auto = false;
	switch (GSConfig.TriFilter)
	{
		case TriFiltering::Forced:
		{
			bilinear = true;
			if (can_trilinear)
			{
				trilinear = static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Linear);
				trilinear_auto = !tex->m_target && (!need_mipmap || !GSConfig.HWMipmap);
			}
		}
		break;

		case TriFiltering::PS2:
		case TriFiltering::Automatic:
		{
			if (need_mipmap && GSConfig.HWMipmap && can_trilinear)
			{
				trilinear = m_context->TEX1.MMIN;
				trilinear_auto = !tex->m_target && !GSConfig.HWMipmap;
			}
		}
		break;

		case TriFiltering::Off:
		default:
			break;
	}

	m_conf.ps.wms = (wms & 2 || target_region) ? wms : 0;
	m_conf.ps.wmt = (wmt & 2 || target_region) ? wmt : 0;

	if (psm.depth && m_vt.IsLinear() && tex->GetTexture()->IsDepthLike())
		GL_INS("HW: WARNING: Depth + bilinear filtering not supported");

	if (m_conf.ps.shuffle)
	{
		const GIFRegTEXA& TEXA = m_cached_ctx.TEXA;

		m_conf.ps.aem = TEXA.AEM;

		if (psm.depth)
		{
			m_conf.ps.depth_fmt = !tex->m_texture->IsDepthLike() ? 3 : tex->m_32_bits_fmt ? 1 : 2;
		}

		if (m_cached_ctx.TEX0.TCC)
		{
			GSVector4 ta(TEXA & GSVector4i::x000000ff());
			ta /= 255.0f;
			m_conf.cb_ps.TA_MaxDepth_Af.x = ta.x;
			m_conf.cb_ps.TA_MaxDepth_Af.y = ta.y;
		}

		bilinear &= m_vt.IsLinear();

		const GSVector4 half_pixel = RealignTargetTextureCoordinate(tex);
		m_conf.cb_vs.texture_offset = GSVector2(half_pixel.x, half_pixel.y);

		if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::NativeWTexOffset && !m_texture_shuffle)
		{
			const u32 psm = rt ? rt->m_TEX0.PSM : ds->m_TEX0.PSM;
			const bool can_offset = m_r.width() > GSLocalMemory::m_psm[psm].pgs.x || m_r.height() > GSLocalMemory::m_psm[psm].pgs.y;

			if (can_offset && tex->m_scale > 1.0f)
			{
				const GSVertex* v = &m_vertex->buff[0];
				if (PRIM->FST)
				{
					const int x1_frac = ((v[1].XYZ.X - m_context->XYOFFSET.OFX) & 0xf);
					const int y1_frac = ((v[1].XYZ.Y - m_context->XYOFFSET.OFY) & 0xf);

					if (!(x1_frac & 8))
						m_conf.cb_vs.texture_offset.x = (1.0f - ((0.5f / (tex->m_unscaled_size.x * tex->m_scale)) * tex->m_unscaled_size.x)) * 8.0f;
					if (!(y1_frac & 8))
						m_conf.cb_vs.texture_offset.y = (1.0f - ((0.5f / (tex->m_unscaled_size.y * tex->m_scale)) * tex->m_unscaled_size.y)) * 8.0f;
				}
			}
		}
	}
	else if (tex->m_target)
	{
		const GIFRegTEXA& TEXA = m_cached_ctx.TEXA;

		m_conf.ps.aem_fmt = cpsm.fmt;
		m_conf.ps.aem = TEXA.AEM;

		if (cpsm.fmt)
		{
			GSVector4 ta(TEXA & GSVector4i::x000000ff());
			ta /= 255.0f;
			m_conf.cb_ps.TA_MaxDepth_Af.x = ta.x;
			m_conf.cb_ps.TA_MaxDepth_Af.y = ta.y;
		}

		if (tex->m_palette)
		{
			if (m_cached_ctx.TEX0.PSM == PSMT4HL)
				m_conf.ps.pal_fmt = 1;
			else if (m_cached_ctx.TEX0.PSM == PSMT4HH)
				m_conf.ps.pal_fmt = 2;
			else
				m_conf.ps.pal_fmt = 3;

			bilinear &= m_vt.IsLinear();
		}

		if (tex->m_texture->IsDepthLike())
		{
			m_conf.ps.depth_fmt = (psm.bpp == 16) ? 2 : 1;

			bilinear &= m_vt.IsLinear();
		}

		const GSVector4 half_pixel = RealignTargetTextureCoordinate(tex);
		m_conf.cb_vs.texture_offset = GSVector2(half_pixel.x, half_pixel.y);

		if (GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::NativeWTexOffset)
		{
			const u32 psm = rt ? rt->m_TEX0.PSM : ds->m_TEX0.PSM;
			const bool can_offset = m_r.width() > GSLocalMemory::m_psm[psm].pgs.x || m_r.height() > GSLocalMemory::m_psm[psm].pgs.y;

			if (can_offset && tex->m_scale > 1.0f)
			{
				const GSVertex* v = &m_vertex->buff[0];
				if (PRIM->FST)
				{
					const int x1_frac = ((v[1].XYZ.X - m_context->XYOFFSET.OFX) & 0xf);
					const int y1_frac = ((v[1].XYZ.Y - m_context->XYOFFSET.OFY) & 0xf);

					if (!(x1_frac & 8))
						m_conf.cb_vs.texture_offset.x = (1.0f - ((0.5f / (tex->m_unscaled_size.x * tex->m_scale)) * tex->m_unscaled_size.x)) * 8.0f;
					if (!(y1_frac & 8))
						m_conf.cb_vs.texture_offset.y = (1.0f - ((0.5f / (tex->m_unscaled_size.y * tex->m_scale)) * tex->m_unscaled_size.y)) * 8.0f;
				}
				else if (m_vt.m_eq.q)
				{
					const float tw = static_cast<float>(1 << m_cached_ctx.TEX0.TW);
					const float th = static_cast<float>(1 << m_cached_ctx.TEX0.TH);
					const float q = v[0].RGBAQ.Q;

					m_conf.cb_vs.texture_offset.x = 0.5f * q / tw;
					m_conf.cb_vs.texture_offset.y = 0.5f * q / th;
				}
			}
		}

		if (m_vt.m_primclass == GS_SPRITE_CLASS && m_index->tail >= 4 && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp >= 16 &&
			((tex->m_from_target_TEX0.PSM & 0x30) == 0x30 || GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pal > 0))
		{
			HandleManualDeswizzle();
		}
	}
	else if (tex->m_palette)
	{

		m_conf.ps.pal_fmt = 3;
	}
	else
	{
	}

	if (m_cached_ctx.TEX0.TFX == TFX_MODULATE && m_vt.m_eq.rgba == 0xFFFF && m_vt.m_min.c.eq(GSVector4i(128)))
	{
		m_conf.ps.tfx = TFX_DECAL;
	}
	else
	{
		m_conf.ps.tfx = m_cached_ctx.TEX0.TFX;
	}

	m_conf.ps.tcc = m_cached_ctx.TEX0.TCC;

	m_conf.ps.ltf = bilinear && shader_emulated_sampler;
	m_conf.ps.point_sampler = g_gs_device->Features().broken_point_sampler && GSConfig.GPUPaletteConversion && !target_region && (!bilinear || shader_emulated_sampler);

	const int tw = static_cast<int>(1 << m_cached_ctx.TEX0.TW);
	const int th = static_cast<int>(1 << m_cached_ctx.TEX0.TH);
	const int miptw = 1 << tex->m_TEX0.TW;
	const int mipth = 1 << tex->m_TEX0.TH;

	const GSVector4 WH(static_cast<float>(tw), static_cast<float>(th), miptw * scale, mipth * scale);

	m_conf.cb_ps.STScale = GSVector2(static_cast<float>(miptw) / static_cast<float>(unscaled_size.x),
		static_cast<float>(mipth) / static_cast<float>(unscaled_size.y));

	if (target_region)
	{
		m_conf.cb_ps.STRange = GSVector4(tex->GetRegionRect() - GSVector4i::cxpr(0, 0, 1, 1)) * GSVector4(scale);
		m_conf.ps.region_rect = true;
	}
	else if (!tex->m_target)
	{
		if (source_region.HasX())
		{
			m_conf.cb_ps.STRange.x = static_cast<float>(source_region.GetMinX()) / static_cast<float>(miptw);
			m_conf.cb_ps.STRange.z = static_cast<float>(miptw) / static_cast<float>(source_region.GetWidth());
			m_conf.ps.adjs = 1;
		}
		if (source_region.HasY())
		{
			m_conf.cb_ps.STRange.y = static_cast<float>(source_region.GetMinY()) / static_cast<float>(mipth);
			m_conf.cb_ps.STRange.w = static_cast<float>(mipth) / static_cast<float>(source_region.GetHeight());
			m_conf.ps.adjt = 1;
		}
	}

	m_conf.ps.fst = !!PRIM->FST;

	m_conf.cb_ps.WH = WH;
	m_conf.cb_ps.HalfTexel = GSVector4(-0.5f, 0.5f).xxyy() / WH.zwzw();
	if (complex_wms_wmt)
	{
		const GSVector4i clamp(m_cached_ctx.CLAMP.MINU, m_cached_ctx.CLAMP.MINV, m_cached_ctx.CLAMP.MAXU, m_cached_ctx.CLAMP.MAXV);
		const GSVector4 region_repeat = GSVector4::cast(clamp);

		const GSVector4 region_clamp_offset = ((GSConfig.UserHacks_HalfPixelOffset == GSHalfPixelOffset::Native && tex->GetScale() > 1.0f) && !m_channel_shuffle) ? 
												(GSVector4::cxpr(1.0f, 1.0f, 0.1f, 0.1f) + (GSVector4::cxpr(0.1f, 0.1f, 0.0f, 0.0f) * tex->GetScale())) :
		                                         GSVector4::cxpr(0.5f, 0.5f, 0.1f, 0.1f);

		const GSVector4 region_clamp = (GSVector4(clamp) + region_clamp_offset) / WH.xyxy();
		if (wms >= CLAMP_REGION_CLAMP)
		{
			m_conf.cb_ps.MinMax.x = (wms == CLAMP_REGION_CLAMP && !m_conf.ps.depth_fmt) ? region_clamp.x : region_repeat.x;
			m_conf.cb_ps.MinMax.z = (wms == CLAMP_REGION_CLAMP && !m_conf.ps.depth_fmt) ? region_clamp.z : region_repeat.z;
		}
		if (wmt >= CLAMP_REGION_CLAMP)
		{
			m_conf.cb_ps.MinMax.y = (wmt == CLAMP_REGION_CLAMP && !m_conf.ps.depth_fmt) ? region_clamp.y : region_repeat.y;
			m_conf.cb_ps.MinMax.w = (wmt == CLAMP_REGION_CLAMP && !m_conf.ps.depth_fmt) ? region_clamp.w : region_repeat.w;
		}
	}

	if (trilinear_manual)
	{
		m_conf.cb_ps.LODParams.x = static_cast<float>(m_context->TEX1.K) / 16.0f;
		m_conf.cb_ps.LODParams.y = static_cast<float>(1 << m_context->TEX1.L);
		m_conf.cb_ps.LODParams.z = static_cast<float>(m_lod.x);
		m_conf.cb_ps.LODParams.w = static_cast<float>(m_lod.y);
		m_conf.ps.manual_lod = 1;
	}
	else if (trilinear_auto)
	{
		tex->m_texture->GenerateMipmapsIfNeeded();
		m_conf.ps.automatic_lod = 1;
	}

	m_conf.ps.tcoffsethack = m_userhacks_tcoffset;
	const GSVector4 tc_oh_ts = GSVector4(1 / 16.0f, 1 / 16.0f, m_userhacks_tcoffset_x, m_userhacks_tcoffset_y) / WH.xyxy();
	m_conf.cb_ps.TCOffsetHack = GSVector2(tc_oh_ts.z, tc_oh_ts.w);
	m_conf.cb_vs.texture_scale = GSVector2(tc_oh_ts.x, tc_oh_ts.y);

	m_conf.sampler.tau = (wms == CLAMP_REPEAT && !target_region);
	m_conf.sampler.tav = (wmt == CLAMP_REPEAT && !target_region);
	if (shader_emulated_sampler)
	{
		m_conf.sampler.biln = 0;
		m_conf.ps.sw_aniso = 0;

		m_conf.sampler.triln = (trilinear >= static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Nearest)) ?
		                           (trilinear - static_cast<u8>(GS_MIN_FILTER::Nearest_Mipmap_Nearest)) :
		                           0;
	}
	else
	{
		m_conf.sampler.biln = bilinear;
		const bool anisotropic = m_vt.m_primclass == GS_TRIANGLE_CLASS && !trilinear_manual;
		m_conf.ps.sw_aniso = anisotropic ? GSConfig.MaxAnisotropy : 0;
		m_conf.sampler.triln = trilinear;
		if (anisotropic && !trilinear_manual)
			m_conf.ps.automatic_lod = 1;
	}

	m_conf.sampler.lodclamp = !(trilinear_manual || trilinear_auto);
}

__ri void GSRendererHW::HandleTextureHazards(const GSTextureCache::Target* rt, const GSTextureCache::Target* ds,
	const GSTextureCache::Source* tex, const TextureMinMaxResult& tmm, GSTextureCache::SourceRegion& source_region,
	bool& target_region, GSVector2i& unscaled_size, float& scale, GSDevice::RecycledTexture& src_copy)
{

	const int tex_diff = tex->m_from_target ? static_cast<int>(m_cached_ctx.TEX0.TBP0 - tex->m_from_target->m_TEX0.TBP0) : static_cast<int>(m_cached_ctx.TEX0.TBP0 - tex->m_TEX0.TBP0);
	const int frame_diff = rt ? static_cast<int>(m_cached_ctx.FRAME.Block() - rt->m_TEX0.TBP0) : 0;

	auto HandleBarrierHazard = [&](bool src_empty) -> bool {

		if (rt && m_conf.tex == m_conf.rt)
		{
			m_conf.tex_hazard = GSHWDrawConfig::TEX_HAZARD_RT;
			if (m_prim_overlap == PRIM_OVERLAP_NO || src_empty || m_channel_shuffle || !g_gs_device->Features().feedback_loops())
				m_conf.require_one_barrier = true;
			else
				m_conf.require_full_barrier = true;

			return true;
		}
		else if (ds && m_conf.tex == m_conf.ds)
		{
			const bool no_depth_write = !m_cached_ctx.DepthWrite();
			if (g_gs_device->Features().test_and_sample_depth && no_depth_write)
			{
				return true;
			}
			else if (g_gs_device->Features().feedback_loops() && no_depth_write)
			{
				m_conf.tex_hazard = GSHWDrawConfig::TEX_HAZARD_DEPTH;
				if (m_prim_overlap == PRIM_OVERLAP_NO || src_empty || m_channel_shuffle)
					m_conf.require_one_barrier = true;
				else
					m_conf.require_full_barrier = true;

				return true;
			}
			else
			{
				return false;
			}
		}

		return true;
	};

	const GSTextureCache::Target* src_target = nullptr;
	if (!m_downscale_source || !tex->m_from_target)
	{
		if (rt && m_conf.tex == m_conf.rt)
		{
			if (CanUseTexIsFB(rt, tex, tmm) && !(m_channel_shuffle && tex_diff != frame_diff))
			{
				m_conf.tex = nullptr;
				m_conf.ps.tex_is_fb = true;
				if (m_prim_overlap == PRIM_OVERLAP_NO || !g_gs_device->Features().feedback_loops())
					m_conf.require_one_barrier = true;
				else
					m_conf.require_full_barrier = true;

				unscaled_size = rt->GetUnscaledSize();
				scale = rt->GetScale();
				return;
			}

			if (!m_channel_shuffle)
			{
				const GSVector4i src_box_rect = GSVector4i(m_vt.m_min.t.x, m_vt.m_min.t.y, m_vt.m_max.t.x, m_vt.m_max.t.y);
				const GSVector4i src_rect = src_box_rect + source_region.GetRect(rt->GetUnscaledSize().x, rt->GetUnscaledSize().y).xyxy();

				if (m_r.rintersect(src_rect).rempty())
				{
					if (HandleBarrierHazard(true))
					{
						unscaled_size = rt->GetUnscaledSize();
						scale = rt->GetScale();
						return;
					}
				}
			}
			GL_CACHE("HW: Source is render target, taking copy.");
			src_target = rt;
		}
		else if (ds && m_conf.tex == m_conf.ds)
		{
			if ((!m_channel_shuffle || tex_diff == frame_diff) && !m_cached_ctx.DepthWrite())
			{
				if (HandleBarrierHazard(true))
				{
					GL_CACHE("HW: Source is depth buffer, not writing, safe to read.");
					unscaled_size = ds->GetUnscaledSize();
					scale = ds->GetScale();
					return;
				}
			}

			if (!m_channel_shuffle)
			{
				const GSVector4i src_box_rect = GSVector4i(m_vt.m_min.t.x, m_vt.m_min.t.y, m_vt.m_max.t.x, m_vt.m_max.t.y);
				const GSVector4i src_rect = src_box_rect + source_region.GetRect(rt->GetUnscaledSize().x, rt->GetUnscaledSize().y).xyxy();

				if (m_r.rintersect(src_rect).rempty())
				{
					if (HandleBarrierHazard(true))
					{
						unscaled_size = ds->GetUnscaledSize();
						scale = ds->GetScale();
						return;
					}
				}
			}

			GL_CACHE("HW: Source is depth buffer, unsafe to read, taking copy.");
			src_target = ds;
		}
		else if (m_channel_shuffle && tex->m_from_target)
		{
			const int tex_page_h = ((m_vt.m_min.t.x + (m_cached_ctx.CLAMP.WMS == CLAMP_REGION_REPEAT ? m_cached_ctx.CLAMP.MAXU : 0)) / GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pgs.x);
			const int tex_page_v = ((m_vt.m_min.t.y + (m_cached_ctx.CLAMP.WMT == CLAMP_REGION_REPEAT ? m_cached_ctx.CLAMP.MAXV : 0)) / GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pgs.y);
			const int frame_page_h = m_r.x / GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.x;
			const int frame_page_v = m_r.y / GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.y;

			if (tex_diff == frame_diff && tex_page_h == frame_page_h && tex_page_v == frame_page_v)
				return;

			src_target = tex->m_from_target;
		}
		else
		{
			return;
		}
	}
	else
		src_target = tex->m_from_target;

	const GSVector2i& src_unscaled_size = src_target->GetUnscaledSize();
	const GSVector4i src_bounds = src_target->GetUnscaledRect();
	GSVector4i copy_range = GSVector4i::zero();
	GSVector2i copy_size = GSVector2i(0);
	GSVector2i copy_dst_offset = GSVector2i(0);
	if (m_downscale_source || m_channel_shuffle || tex->m_texture->IsDepthLike())
	{
		if (m_channel_shuffle)
		{
			copy_size.x = rt->m_unscaled_size.x;
			copy_size.y = rt->m_unscaled_size.y;
			copy_range.x = copy_range.y = 0;
			copy_range.z = std::min(m_r.width() + 1, copy_size.x);
			copy_range.w = std::min(m_r.height() + 1, copy_size.y);
		}
		else
		{
			copy_range = src_bounds;
			copy_size = src_unscaled_size;
		}

		const int tex_page_h = m_vt.m_min.t.x / GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pgs.x;
		const int tex_page_v = m_vt.m_min.t.y / GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pgs.y;
		const int frame_page_h = m_r.x / GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.x;
		const int frame_page_v = m_r.y / GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.y;
		if (m_channel_shuffle && (tex_diff || frame_diff || tex_page_h != frame_page_h || tex_page_v != frame_page_v))
		{
			const int clamp_horizontal_page_offset = m_cached_ctx.CLAMP.WMS == CLAMP_REGION_REPEAT ? (m_cached_ctx.CLAMP.MAXU / GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pgs.x) : 0;
			const int clamp_vertical_page_offset = m_cached_ctx.CLAMP.WMT == CLAMP_REGION_REPEAT ? (m_cached_ctx.CLAMP.MAXV / GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pgs.y) : 0;
			const int page_offset = ((m_cached_ctx.TEX0.TBP0 - src_target->m_TEX0.TBP0) >> 5) + clamp_horizontal_page_offset + clamp_vertical_page_offset;
			const GSVector2i draw_offset = GSVector2i((static_cast<int>(m_vt.m_min.t.x) / GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pgs.x) * GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.x,
											(static_cast<int>(m_vt.m_min.t.y) / GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pgs.y) * GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs.y);
			const int horizontal_offset = ((page_offset % src_target->m_TEX0.TBW) * GSLocalMemory::m_psm[src_target->m_TEX0.PSM].pgs.x) + draw_offset.x;
			const int vertical_offset = ((page_offset / src_target->m_TEX0.TBW) * GSLocalMemory::m_psm[src_target->m_TEX0.PSM].pgs.y) + draw_offset.y;

			if (HandleBarrierHazard(false) || (rt != tex->m_from_target && ds != tex->m_from_target))
			{
				m_conf.cb_ps.ChannelShuffleOffset = GSVector2((horizontal_offset - m_r.x) * tex->GetScale(), (vertical_offset - m_r.y) * tex->GetScale());
				target_region = false;
				source_region.bits = 0;

				unscaled_size = src_target->GetUnscaledSize();
				scale = src_target->GetScale();
				return;
			}
			else
			{
				copy_range.x += horizontal_offset;
				copy_range.y += vertical_offset;
				copy_range.z += horizontal_offset;
				copy_range.w += vertical_offset;

				GSVector4i::storel(&copy_dst_offset, copy_range);

				if (!m_channel_shuffle)
				{
					copy_size.y -= vertical_offset;
					copy_size.x -= horizontal_offset;
				}
				target_region = false;
				source_region.bits = 0;
				if (m_in_target_draw && (page_offset || frame_diff))
				{
					copy_range.z = copy_range.x + m_r.width();
					copy_range.w = copy_range.y + m_r.height();

					if (tex_diff != frame_diff)
					{
						GSVector4i::storel(&copy_dst_offset, m_r);
					}
				}

				copy_range.z = std::min(copy_range.z, src_target->m_unscaled_size.x);
				copy_range.w = std::min(copy_range.w, src_target->m_unscaled_size.y);
			}
		}
		else
		{
			GSVector4i::storel(&copy_dst_offset, copy_range);
		}
	}
	else
	{
		const GSVector2i tex_size = GSVector2i(1 << m_cached_ctx.TEX0.TW, 1 << m_cached_ctx.TEX0.TH);
		copy_size.x = std::min(tex_size.x, src_unscaled_size.x);
		copy_size.y = std::min(tex_size.y, src_unscaled_size.y);

		if (m_texture_shuffle || m_channel_shuffle)
			copy_range = GSVector4i::loadh(copy_size);
		else
			copy_range = tmm.coverage;

		if (m_cached_ctx.CLAMP.WMS >= CLAMP_REGION_CLAMP && copy_range.z > copy_size.x)
			copy_size.x = src_unscaled_size.x;
		if (m_cached_ctx.CLAMP.WMT >= CLAMP_REGION_CLAMP && copy_range.w > copy_size.y)
			copy_size.y = src_unscaled_size.y;

		if (target_region)
		{
			const GSVector4i src_offset = GSVector4i(source_region.GetMinX(), source_region.GetMinY()).xyxy();
			copy_range += src_offset;
			copy_range = copy_range.rintersect(source_region.GetRect(src_unscaled_size.x, src_unscaled_size.y));
			GL_CACHE("HW: Applying target region at copy: %dx%d @ %d,%d => %d,%d", copy_range.width(), copy_range.height(),
				tmm.coverage.x, tmm.coverage.y, copy_range.x, copy_range.y);

			source_region = {};
			target_region = false;

			copy_range = copy_range.rintersect(src_bounds);

			const GSVector4i dst_range = copy_range - src_offset;
			GSVector4i::storel(&copy_dst_offset, dst_range);

			GSVector4i::storel(&copy_size, GSVector4i(copy_size).max_i32(dst_range.zwzw()));
		}
		else
		{
			copy_range = copy_range.rintersect(src_bounds);
			GSVector4i::storel(&copy_dst_offset, copy_range);
		}
	}

	if (copy_range.rempty())
	{
		GL_CACHE("HW: ERROR: Reading outside of the RT range, using null texture.");
		unscaled_size = GSVector2i(1, 1);
		scale = 1.0f;
		m_conf.tex = nullptr;
		m_conf.ps.tfx = 4;
		return;
	}

	unscaled_size = copy_size;
	scale = m_downscale_source ? 1.0f : src_target->GetScale();
	GL_CACHE("HW: Copy size: %dx%d, range: %d,%d -> %d,%d (%dx%d) @ %.1f", copy_size.x, copy_size.y, copy_range.x,
		copy_range.y, copy_range.z, copy_range.w, copy_range.width(), copy_range.height(), scale);

	const GSVector2i scaled_copy_size = GSVector2i(static_cast<int>(std::ceil(static_cast<float>(copy_size.x) * scale)),
		static_cast<int>(std::ceil(static_cast<float>(copy_size.y) * scale)));
	const bool clear = src_target->m_texture->IsRenderTarget();
	static const bool s_vr_hazard_1l = (std::getenv("PCSX2_VR_HAZARD_1L") != nullptr);
	const u32 copy_layers = s_vr_hazard_1l ? 1u : src_target->m_texture->GetArrayLayers();
	src_copy.reset(g_gs_device->FetchSurface(src_target->m_texture->GetUsage(), scaled_copy_size.x,
		scaled_copy_size.y, 1, src_target->m_texture->GetFormat(), clear, true, copy_layers));
	if (!src_copy) [[unlikely]]
	{
		Console.Error("HW: Failed to allocate %dx%d texture for hazard copy", scaled_copy_size.x, scaled_copy_size.y);
		m_conf.tex = nullptr;
		m_conf.ps.tfx = 4;
		return;
	}

	static const bool s_vr_hazard_probe = (std::getenv("PCSX2_VR_HAZARD_PROBE") != nullptr);
	if (s_vr_hazard_probe) [[unlikely]]
	{
		Console.WriteLn("(VR) PROBE hazard: src_layers=%u copy_layers=%u %dx%d fmt=%d downscale=%s depth=%s scale=%.3f",
			src_target->m_texture->GetArrayLayers(), copy_layers, scaled_copy_size.x, scaled_copy_size.y,
			static_cast<int>(src_target->m_texture->GetFormat()), m_downscale_source ? "yes" : "no",
			src_target->m_texture->IsDepthStencil() ? "yes" : "no", src_target->GetScale());
		g_gs_device->VRProbeLayers(src_target->m_texture, "src_target(before-fill)");
	}

	for (u32 copy_layer = 0; copy_layer < copy_layers; copy_layer++)
	{
		GSTexture* const copy_src = (copy_layers > 1) ?
			src_target->m_texture->GetLayerProxyTexture(copy_layer) : src_target->m_texture;
		GSTexture* const copy_dst = (copy_layers > 1) ?
			src_copy->GetLayerProxyTexture(copy_layer) : src_copy.get();

		if (m_downscale_source)
		{
			if (src_target->m_texture->IsDepthStencil() || std::floor(src_target->GetScale()) != src_target->GetScale())
			{
				GSVector4 src_rect = GSVector4(tmm.coverage) / GSVector4(GSVector4i::loadh(src_unscaled_size).zwzw());
				const GSVector4 dst_rect = GSVector4(tmm.coverage);
				if (s_vr_hazard_probe) [[unlikely]]
				{
					Console.WriteLn("(VR) PROBE fill: layer %u path=StretchRectAuto(downscale-depth/frac) "
									"src_layers=%u dst_layers=%u sRect=%.4f,%.4f-%.4f,%.4f dRect=%.1f,%.1f-%.1f,%.1f",
						copy_layer, copy_src->GetArrayLayers(), copy_dst->GetArrayLayers(), src_rect.x, src_rect.y,
						src_rect.z, src_rect.w, dst_rect.x, dst_rect.y, dst_rect.z, dst_rect.w);
				}
				g_gs_device->StretchRectAuto(copy_src, src_rect, copy_dst, dst_rect, Nearest);
			}
			else
			{
				const u32 downsample_factor = static_cast<u32>(src_target->GetScale());
				const GSVector2i clamp_min = (GSConfig.UserHacks_HalfPixelOffset != GSHalfPixelOffset::Native) ?
				                                 GSVector2i(0, 0) :
				                                 GSVector2i(downsample_factor, downsample_factor);
				GSVector4i copy_rect = tmm.coverage;
				if (target_region)
				{
					copy_rect += GSVector4i(source_region.GetMinX(), source_region.GetMinY()).xyxy();
				}
				const GSVector4 dRect = GSVector4((copy_rect + GSVector4i(-1, 1).xxyy()).rintersect(src_target->GetUnscaledRect()));
				if (s_vr_hazard_probe) [[unlikely]]
				{
					Console.WriteLn("(VR) PROBE fill: layer %u path=FilteredDownsampleTexture factor=%u "
									"src_layers=%u dst_layers=%u dRect=%.1f,%.1f-%.1f,%.1f",
						copy_layer, downsample_factor, copy_src->GetArrayLayers(), copy_dst->GetArrayLayers(),
						dRect.x, dRect.y, dRect.z, dRect.w);
				}
				g_gs_device->FilteredDownsampleTexture(copy_src, copy_dst, downsample_factor, clamp_min, dRect);
			}
		}
		else
		{
			const GSVector4i offset = copy_range - GSVector4i(copy_dst_offset).xyxy();
			GSVector4i bilinear_range = copy_range + GSVector4i(-1, -1, 1, 1);
			bilinear_range = bilinear_range.rintersect(src_bounds);

			const GSVector4 src_rect = GSVector4(bilinear_range) / GSVector4(src_unscaled_size).xyxy();
			const GSVector4 dst_rect = (GSVector4(bilinear_range) - GSVector4(offset).xyxy()) * scale;

			if (s_vr_hazard_probe) [[unlikely]]
			{
				Console.WriteLn("(VR) PROBE fill: layer %u path=StretchRectAuto(plain) src_layers=%u dst_layers=%u "
								"sRect=%.4f,%.4f-%.4f,%.4f dRect=%.1f,%.1f-%.1f,%.1f",
					copy_layer, copy_src->GetArrayLayers(), copy_dst->GetArrayLayers(), src_rect.x, src_rect.y,
					src_rect.z, src_rect.w, dst_rect.x, dst_rect.y, dst_rect.z, dst_rect.w);
			}
			g_gs_device->StretchRectAuto(copy_src, src_rect, copy_dst, dst_rect, Nearest);
		}
	}

	if (s_vr_hazard_probe) [[unlikely]]
	{
		g_gs_device->VRProbeLayers(src_copy.get(), "src_copy(after-fill)");
		g_gs_device->VRProbeLayers(src_copy.get(), "src_copy(after-fill-PROBE2)");
	}

	m_conf.tex = src_copy.get();
}

bool GSRendererHW::CanUseTexIsFB(const GSTextureCache::Target* rt, const GSTextureCache::Source* tex,
	const TextureMinMaxResult& tmm)
{
	if (GSConfig.AccurateBlendingUnit == AccBlendLevel::Minimum)
	{
		GL_CACHE("HW: Disabling tex-is-fb due to minimum blending.");
		return false;
	}

	if (tex->GetRegion().HasX() || tex->GetRegion().HasY())
	{
		if (m_cached_ctx.FRAME.Block() != m_cached_ctx.TEX0.TBP0)
			return false;
	}

	if (m_channel_shuffle)
	{
		GL_CACHE("HW: Enabling tex-is-fb for channel shuffle.");
		return true;
	}

	if (m_texture_shuffle)
	{
		if (floor(abs(m_vt.m_min.t.y) + tex->m_region.GetMinY()) != floor(abs(m_vt.m_min.p.y)))
			return false;

		if (abs(floor(abs(m_vt.m_min.t.x) + tex->m_region.GetMinX()) - floor(abs(m_vt.m_min.p.x))) > 16)
			return false;

		GL_CACHE("HW: Enabling tex-is-fb for texture shuffle.");
		return true;
	}

	if (!g_gs_device->Features().feedback_loops() && m_prim_overlap != PRIM_OVERLAP_NO)
	{
		GL_CACHE("HW: Disabling tex-is-fb due to no barriers.");
		return false;
	}

	static constexpr auto check_clamp = [](u32 clamp, u32 min, u32 max, s32 tmin, s32 tmax) {
		if (clamp == CLAMP_REGION_CLAMP)
		{
			if (tmin < static_cast<s32>(min) || tmax > static_cast<s32>(max + 1))
			{
				GL_CACHE("HW: Disabling tex-is-fb due to REGION_CLAMP [%d, %d] with TMM of [%d, %d]", min, max, tmin, tmax);
				return false;
			}
		}
		else if (clamp == CLAMP_REGION_REPEAT)
		{
			const u32 req_tbits = (tmax > 1) ? (std::bit_ceil(static_cast<u32>(tmax - 1)) - 1) : 0x1;
			if ((min & req_tbits) != req_tbits)
			{
				GL_CACHE("HW: Disabling tex-is-fb due to REGION_REPEAT [%d, %d] with TMM of [%d, %d] and tbits of %d",
					min, max, tmin, tmax, req_tbits);
				return false;
			}
		}

		return true;
	};
	if (!check_clamp(
			m_cached_ctx.CLAMP.WMS, m_cached_ctx.CLAMP.MINU, m_cached_ctx.CLAMP.MAXU, tmm.coverage.x, tmm.coverage.z) ||
		!check_clamp(
			m_cached_ctx.CLAMP.WMT, m_cached_ctx.CLAMP.MINV, m_cached_ctx.CLAMP.MAXV, tmm.coverage.y, tmm.coverage.w))
	{
		return false;
	}

	const bool is_quads = (m_vt.m_primclass == GS_SPRITE_CLASS || m_prim_overlap == PRIM_OVERLAP_NO);
	if (is_quads)
	{
		if (m_vt.IsLinear())
		{
			GL_CACHE("HW: Disabling tex-is-fb due to bilinear sampling.");
			return false;
		}

		const GSLocalMemory::psm_t& tex_psm = GSLocalMemory::m_psm[tex->m_TEX0.PSM];
		const GSLocalMemory::psm_t& rt_psm = GSLocalMemory::m_psm[rt->m_TEX0.PSM];
		if (tex_psm.pal > 0 && tex_psm.bpp < rt_psm.bpp)
		{
			GL_CACHE("HW: Enabling tex-is-fb for palette conversion.");
			return true;
		}

		const GSVector4 diff(m_vt.m_min.p.upld(m_vt.m_max.p) - m_vt.m_min.t.upld(m_vt.m_max.t));
		GL_CACHE("HW: Coord diff: %f,%f", diff.x, diff.y);
		if ((diff.abs() < GSVector4(1.0f)).alltrue())
		{
			GL_CACHE("HW: Enabling tex-is-fb for sampling from rendered texel.");
			return true;
		}

		GL_CACHE("HW: Disabling tex-is-fb due to coord diff too large.");
		return false;
	}

	if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
	{
		const GSVector4 diff(m_vt.m_min.p.upld(m_vt.m_max.p) - m_vt.m_min.t.upld(m_vt.m_max.t));
		if (m_cached_ctx.FRAME.FBMSK == 0x00FFFFFF && (diff.abs() < GSVector4(1.0f)).alltrue())
		{
			GL_CACHE("HW: Elabling tex-is-fb hack for Jak.");
			return true;
		}

		GL_CACHE("HW: Disabling tex-is-fb tue to triangle draw.");
		return false;
	}

	return false;
}

void GSRendererHW::GetAlphaTestConfigPS(const u32 atst, const u8 aref, const bool invert_test, PS_ATST& ps_atst_out, float& aref_out)
{
	static const u32 inverted_atst[] = {
		ATST_ALWAYS,
		ATST_NEVER,
		ATST_GEQUAL,
		ATST_GREATER,
		ATST_NOTEQUAL,
		ATST_LESS,
		ATST_LEQUAL,
		ATST_EQUAL
	};

	constexpr float small_val = 0x100p-23f;

	switch (invert_test ? inverted_atst[atst] : atst)
	{
		case ATST_LESS:
			aref_out = static_cast<float>(aref) - small_val;
			ps_atst_out = PS_ATST::LEQUAL;
			break;
		case ATST_LEQUAL:
			aref_out = static_cast<float>(aref) - small_val + 1.0f;
			ps_atst_out = PS_ATST::LEQUAL;
			break;
		case ATST_GEQUAL:
			aref_out = static_cast<float>(aref) - small_val;
			ps_atst_out = PS_ATST::GEQUAL;
			break;
		case ATST_GREATER:
			aref_out = static_cast<float>(aref) - small_val + 1.0f;
			ps_atst_out = PS_ATST::GEQUAL;
			break;
		case ATST_EQUAL:
			aref_out = static_cast<float>(aref);
			ps_atst_out = PS_ATST::EQUAL;
			break;
		case ATST_NOTEQUAL:
			aref_out = static_cast<float>(aref);
			ps_atst_out = PS_ATST::NOTEQUAL;
			break;
		case ATST_NEVER:
		case ATST_ALWAYS:
		default:
			ps_atst_out = PS_ATST::NONE;
			break;
	}
}

void GSRendererHW::EmulateAlphaTest(DATEOptions& date_options)
{
	const GSDevice::FeatureSupport& features = g_gs_device->Features();

	if (!m_cached_ctx.TEST.ATE)
	{
		GL_INS("HW: Alpha test disabled");
		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::NONE;
		return;
	}

	GL_PUSH("HW: Alpha test config (1)");

	PS_ATST ps_atst;
	float ps_aref;

	u32 atst = m_cached_ctx.TEST.ATST;
	u32 afail = m_cached_ctx.TEST.GetAFAIL(m_cached_ctx.FRAME.PSM);
	u8 aref = m_cached_ctx.TEST.AREF;
	const bool zwe = m_cached_ctx.DepthWrite();

	if (afail == AFAIL_RGB_ONLY && !m_conf.colormask.wa)
		afail = AFAIL_FB_ONLY;

	if (!zwe && !m_conf.colormask.wrgba)
		atst = ATST_NEVER;

	if ((afail == AFAIL_FB_ONLY && !zwe) ||
		(afail == AFAIL_RGB_ONLY && !m_conf.colormask.wa && !zwe) ||
		(afail == AFAIL_ZB_ONLY && !m_conf.colormask.wrgba))
	{
		atst = ATST_ALWAYS;
	}

	if ((afail == AFAIL_FB_ONLY && !m_conf.colormask.wrgba) ||
		(afail == AFAIL_RGB_ONLY && (!(m_conf.colormask.wrgba & 7))) ||
		(afail == AFAIL_ZB_ONLY && !zwe))
	{
		afail = AFAIL_KEEP;
	}

	m_cached_ctx.TEST.ATST = atst;
	m_cached_ctx.TEST.AFAIL = afail;
	GL_INS("Using: ATST = %s, AFAIL = %s", GSUtil::GetATSTName(atst), GSUtil::GetAFAILName(afail));

	if (atst == ATST_ALWAYS)
	{
		GL_INS("Alpha test: ALWAYS (disable)");
		m_cached_ctx.TEST.ATE = false;
		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::NONE;
		return;
	}

	if (atst == ATST_NEVER)
	{
		GL_INS("Alpha test: NEVER single pass (accurate)");
		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::NEVER;
		return;
	}

	if (afail == AFAIL_KEEP)
	{
		GL_INS("Alpha test: AFAIL discard (accurate)");
		GetAlphaTestConfigPS(atst, aref, false, ps_atst, ps_aref);
		m_conf.ps.atst = ps_atst;
		m_conf.cb_ps.FogColor_AREF.a = ps_aref;
		m_conf.ps.afail = PS_AFAIL::KEEP;
		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::KEEP;
		return;
	}

	const bool independent_z =
		(m_cached_ctx.TEST.ZTST == ZTST_GEQUAL && m_vt.m_eq.z) ||
		(m_cached_ctx.TEST.ZTST == ZTST_ALWAYS) ||
		!zwe ||
		(m_prim_overlap == PRIM_OVERLAP_NO);

	const bool independent_rgb =
		(m_context->ALPHA.C != ALPHA_C_AD) ||
		!m_conf.colormask.wa ||
		(m_prim_overlap == PRIM_OVERLAP_NO);

	const bool simple_fb_only = (afail == AFAIL_FB_ONLY) && independent_z;
	const bool simple_rgb_only = (afail == AFAIL_RGB_ONLY) && independent_z && independent_rgb;
	const bool simple_zb_only = (afail == AFAIL_ZB_ONLY) && independent_z;

	const bool afail_needs_rt = (afail == AFAIL_ZB_ONLY) || (afail == AFAIL_RGB_ONLY);
	const bool afail_needs_depth = (afail == AFAIL_FB_ONLY || afail == AFAIL_RGB_ONLY) && zwe;

	const bool feedback_one_pass = simple_fb_only || simple_rgb_only || simple_zb_only;
	
	const bool free_barrier_feedback =
		((m_conf.require_one_barrier && feedback_one_pass) || m_conf.require_full_barrier) &&
		features.feedback_loops() &&
		(!afail_needs_depth || m_conf.ps.IsFeedbackLoopDepth());

	const bool free_fbfetch_feedback = features.framebuffer_fetch && !afail_needs_depth;

	const bool depth_feedback_supported = features.feedback_loops();

	const bool avoid_feedback = afail_needs_depth && !depth_feedback_supported;

	const bool prefer_feedback = free_barrier_feedback || free_fbfetch_feedback ||
	                             GSConfig.HWAccurateAlphaTest;

	const bool prefer_two_pass = !(free_fbfetch_feedback || free_barrier_feedback) &&
	                             (simple_fb_only || simple_rgb_only || simple_zb_only);
	
	if (prefer_feedback && !prefer_two_pass && !avoid_feedback)
	{
		GL_INS("Alpha test with RT/depth feedback (accurate)");
		GetAlphaTestConfigPS(atst, aref, false, ps_atst, ps_aref);
		m_conf.ps.atst = ps_atst;
		m_conf.cb_ps.FogColor_AREF.a = ps_aref;
		m_conf.ps.afail = static_cast<PS_AFAIL>(afail);

		if ((afail_needs_rt || afail_needs_depth) && features.feedback_loops())
		{
			m_conf.require_one_barrier |= feedback_one_pass;
			m_conf.require_full_barrier |= !feedback_one_pass;
		}

		if (afail_needs_depth)
		{
			pxAssert(zwe);

			if (afail == AFAIL_RGB_ONLY)
				m_conf.ps.afail = PS_AFAIL::RGB_ONLY_SW_Z;
		}
		else
		{
			pxAssert(afail != AFAIL_FB_ONLY);
		}

		ConfigureDepthFeedback();

		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::FEEDBACK;
	}
	else if (simple_fb_only)
	{
		GL_INS("Alpha test: RGBA then Z (accurate)");

		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::SIMPLE_FB_ONLY;
	}
	else if (simple_rgb_only)
	{
		GL_INS("Alpha test: RGBA (A with dual-source blend), then Z (accurate)");

		GetAlphaTestConfigPS(atst, aref, false, ps_atst, ps_aref);
		m_conf.ps.atst = ps_atst;
		m_conf.cb_ps.FogColor_AREF.a = ps_aref;
		m_conf.ps.afail = PS_AFAIL::RGB_ONLY_DSB;
		m_conf.ps.no_color1 = false;

		if (date_options.enabled && !date_options.barrier && features.primitive_id)
		{
			if (!date_options.primid)
				GL_INS("Alpha test: Swap stencil DATE for PrimID, due to AFAIL");

			date_options.stencil_one = false;
			date_options.primid = true;
		}

		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::SIMPLE_RGB_ONLY;
	}
	else
	{
		GL_INS("Alpha test: Two pass with pass/fail");

		GetAlphaTestConfigPS(atst, aref, false, ps_atst, ps_aref);
		m_conf.ps.atst = ps_atst;
		m_conf.cb_ps.FogColor_AREF.a = ps_aref;
		m_conf.ps.afail = PS_AFAIL::KEEP;
		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::PASS_THEN_FAIL;
	}
}

void GSRendererHW::EmulateAlphaTestSecondPass()
{
	if (!GSHWDrawConfig::HasAlphaTestSecondPass(m_conf.alpha_test))
	{
		return;
	}

	GL_PUSH("HW: Alpha test config (2)");

	const u32 atst = m_cached_ctx.TEST.ATST;
	const u32 afail = m_cached_ctx.TEST.AFAIL;
	const u32 aref = m_cached_ctx.TEST.AREF;

	PS_ATST ps_atst;
	float ps_aref;

	std::memcpy(&m_conf.alpha_second_pass.ps, &m_conf.ps, sizeof(m_conf.ps));
	std::memcpy(&m_conf.alpha_second_pass.colormask, &m_conf.colormask, sizeof(m_conf.colormask));
	std::memcpy(&m_conf.alpha_second_pass.depth, &m_conf.depth, sizeof(m_conf.depth));

	if (m_conf.alpha_test == GSHWDrawConfig::AlphaTestMode::SIMPLE_FB_ONLY ||
		m_conf.alpha_test == GSHWDrawConfig::AlphaTestMode::SIMPLE_RGB_ONLY)
	{

		m_conf.depth.zwe = false;

		m_conf.alpha_second_pass.colormask.wrgba = false;

		if (m_conf.alpha_second_pass.depth.zwe)
		{
			GetAlphaTestConfigPS(atst, aref, false, ps_atst, ps_aref);
			m_conf.alpha_second_pass.enable = true;
			m_conf.alpha_second_pass.ps.atst = ps_atst;
			m_conf.alpha_second_pass.ps_aref = ps_aref;
			m_conf.alpha_second_pass.ps.afail = PS_AFAIL::KEEP;
		}

		if (m_conf.alpha_test == GSHWDrawConfig::AlphaTestMode::SIMPLE_RGB_ONLY)
		{
			pxAssert(!m_conf.ps.no_color1);
			if (!m_conf.blend.enable)
			{
				m_conf.blend = GSHWDrawConfig::BlendState(true, GSDevice::CONST_ONE, GSDevice::CONST_ZERO,
					GSDevice::OP_ADD, GSDevice::SRC1_ALPHA, GSDevice::INV_SRC1_ALPHA, false, 0);
			}
			else
			{
				if (m_conf.blend_multi_pass.enable)
				{
					m_conf.blend_multi_pass.blend.src_factor_alpha = GSDevice::SRC1_ALPHA;
					m_conf.blend_multi_pass.blend.dst_factor_alpha = GSDevice::INV_SRC1_ALPHA;
				}
				else
				{
					m_conf.blend.src_factor_alpha = GSDevice::SRC1_ALPHA;
					m_conf.blend.dst_factor_alpha = GSDevice::INV_SRC1_ALPHA;
				}
			}
		}
	}
	else
	{

		if (afail == AFAIL_FB_ONLY)
		{
			m_conf.alpha_second_pass.depth.zwe = false;
		}
		else if (afail == AFAIL_ZB_ONLY)
		{
			m_conf.alpha_second_pass.colormask.wrgba = 0;
		}
		else if (afail == AFAIL_RGB_ONLY)
		{
			m_conf.alpha_second_pass.depth.zwe = false;

			m_conf.alpha_second_pass.colormask.wrgba = m_conf.colormask.wrgba & 7;
		}

		if (m_conf.alpha_second_pass.colormask.wrgba || m_conf.alpha_second_pass.depth.zwe)
		{
			GetAlphaTestConfigPS(atst, aref, true, ps_atst, ps_aref);
			m_conf.alpha_second_pass.enable = true;
			m_conf.alpha_second_pass.ps.atst = ps_atst;
			m_conf.alpha_second_pass.ps_aref = ps_aref;
			m_conf.alpha_second_pass.ps.afail = PS_AFAIL::KEEP;
		}
	}

	if (m_conf.alpha_second_pass.enable)
	{
		pxAssertRel(m_conf.alpha_second_pass.colormask.wrgba || m_conf.alpha_second_pass.depth.zwe,
			"Alpha second pass has no color/depth write.");
	}

	if (!m_conf.depth.zwe)
	{
		m_conf.ps.DisableDepthOutput();
	}

	if (m_conf.alpha_second_pass.colormask.wrgba == 0)
	{
		m_conf.alpha_second_pass.ps.DisableColorOutput();
	}
	if (!m_conf.alpha_second_pass.depth.zwe)
	{
		m_conf.alpha_second_pass.ps.DisableDepthOutput();
	}
	if (m_conf.IsFeedbackLoopRT(m_conf.alpha_second_pass.ps) || m_conf.IsFeedbackLoopDepth(m_conf.alpha_second_pass.ps))
	{
		m_conf.alpha_second_pass.require_one_barrier = m_conf.require_one_barrier;
		m_conf.alpha_second_pass.require_full_barrier = m_conf.require_full_barrier;
	}

	if (!(m_conf.colormask.wrgba || m_conf.depth.zwe))
	{
		std::memcpy(&m_conf.ps, &m_conf.alpha_second_pass.ps, sizeof(m_conf.ps));
		std::memcpy(&m_conf.colormask, &m_conf.alpha_second_pass.colormask, sizeof(m_conf.colormask));
		std::memcpy(&m_conf.depth, &m_conf.alpha_second_pass.depth, sizeof(m_conf.depth));
		m_conf.cb_ps.FogColor_AREF.a = m_conf.alpha_second_pass.ps_aref;
		m_conf.alpha_second_pass.enable = false;
	}

	if (!(m_conf.colormask.wrgba || m_conf.depth.zwe))
		m_conf.alpha_test = GSHWDrawConfig::AlphaTestMode::ABORT_DRAW;
}

void GSRendererHW::ConfigureDepthFeedback(bool rov_depth)
{
	if (m_conf.ps.IsFeedbackLoopDepth() || rov_depth)
	{
		const GSDevice::FeatureSupport& features = g_gs_device->Features();

		if (features.feedback_loops() && !rov_depth)
		{
			m_conf.require_one_barrier |= (m_prim_overlap == PRIM_OVERLAP_NO);
			m_conf.require_full_barrier |= (m_prim_overlap != PRIM_OVERLAP_NO);
		}
		
		if (m_cached_ctx.DepthRead())
		{
			GL_INS("HW: Enable SW depth test and disable HW.");
			m_conf.ps.ztst = m_cached_ctx.TEST.ZTST;
			m_conf.depth.ztst = ZTST_ALWAYS;
		}
	}
}

void GSRendererHW::CleanupDraw(bool invalidate_temp_src)
{
	if (invalidate_temp_src)
		g_texture_cache->InvalidateTemporarySource();
	m_context->UpdateScissor();

	if ((m_context->FRAME.U32[0] ^ m_cached_ctx.FRAME.U32[0]) & 0x3f3f01ff)
		m_context->offset.fb = m_mem.GetOffset(m_context->FRAME.Block(), m_context->FRAME.FBW, m_context->FRAME.PSM);
	if ((m_context->ZBUF.U32[0] ^ m_cached_ctx.ZBUF.U32[0]) & 0x3f0001ff)
		m_context->offset.zb = m_mem.GetOffset(m_context->ZBUF.Block(), m_context->FRAME.FBW, m_context->ZBUF.PSM);
}

void GSRendererHW::ResetStates()
{
	memset(static_cast<void*>(&m_conf), 0, reinterpret_cast<const char*>(&m_conf.cb_vs) - reinterpret_cast<const char*>(&m_conf));
}

__ri void GSRendererHW::DrawPrims(GSTextureCache::Target* rt, GSTextureCache::Target* ds, GSTextureCache::Source* tex, const TextureMinMaxResult& tmm)
{
#ifdef ENABLE_OGL_DEBUG
	const GSVector4i area_out = GSVector4i(m_vt.m_min.p.upld(m_vt.m_max.p)).rintersect(m_context->scissor.in);
	const GSVector4i area_in = GSVector4i(m_vt.m_min.t.upld(m_vt.m_max.t));

	GL_PUSH("HW: GL Draw from (area %d,%d => %d,%d) in (area %d,%d => %d,%d)",
		area_in.x, area_in.y, area_in.z, area_in.w,
		area_out.x, area_out.y, area_out.z, area_out.w);
#endif

	const GSDrawingEnvironment& env = *m_draw_env;

	struct InFlightSourceScope
	{
		explicit InFlightSourceScope(GSTextureCache::Source* s) { g_texture_cache->SetDrawInFlightSource(s); }
		~InFlightSourceScope() { g_texture_cache->SetDrawInFlightSource(nullptr); }
	} inflight_source_scope(tex);

	DATEOptions date_options;
	date_options.enabled = rt && m_cached_ctx.TEST.DATE && m_cached_ctx.FRAME.PSM != PSMCT24;
	date_options.primid = false;
	date_options.barrier = false;
	date_options.stencil_one = false;

	ResetStates();

	m_conf.cb_vs.texture_offset = {};
	static const bool s_no_scanmsk = (std::getenv("PCSX2_VR_NO_SCANMSK") != nullptr);
	m_conf.ps.scanmsk = s_no_scanmsk ? 0 : env.SCANMSK.MSK;
	{
		static const bool s_census2 = (std::getenv("PCSX2_VR_CHAINLOG") != nullptr);
		if (s_census2)
		{
			Console.WriteLn("(VR) DRAWCENSUS msk=%u rtL=%u tme=%d fst=%d prim=%d abe=%d r=%d,%d-%d,%d fbp=0x%x tbp=0x%x srcT=%d srcL=%u",
				env.SCANMSK.MSK,
				(rt && rt->m_texture) ? rt->m_texture->GetArrayLayers() : 0,
				PRIM->TME ? 1 : 0, PRIM->FST ? 1 : 0,
				static_cast<int>(m_vt.m_primclass), PRIM->ABE ? 1 : 0,
				m_r.x, m_r.y, m_r.z, m_r.w,
				m_cached_ctx.FRAME.Block(), PRIM->TME ? m_cached_ctx.TEX0.TBP0 : 0,
				(tex && tex->m_from_target) ? 1 : 0,
				(tex && tex->m_texture) ? tex->m_texture->GetArrayLayers() : 0);
		}
	}
#ifdef ENABLE_VR
	if (rt && rt->m_texture && rt->m_texture->GetArrayLayers() == 1 &&
		g_gs_device->SupportsStereoTargets() && VR::StereoState::Get().enabled &&
		g_texture_cache->IsDisplayChainBP(rt->m_TEX0.TBP0))
	{
		rt->PromoteToStereo();
	}
	if (rt && rt->m_texture && rt->m_texture->GetArrayLayers() > 1 && tex && tex->m_from_target &&
		tex->m_from_target->m_texture && tex->m_from_target->m_texture->GetArrayLayers() == 1 &&
		!g_texture_cache->IsDisplayChainBP(tex->m_from_target->m_TEX0.TBP0))
	{
		g_texture_cache->NoteDisplayChainBP(tex->m_from_target->m_TEX0.TBP0);
		DevCon.WriteLn("(VR) TC: feed-edge — target 0x%x joins the display chain (sampled by a stereo draw).",
			tex->m_from_target->m_TEX0.TBP0);
	}
	if (rt && ds && rt->m_texture && ds->m_texture && !m_using_temp_z)
	{
		if (rt->m_texture->GetArrayLayers() > ds->m_texture->GetArrayLayers())
			ds->PromoteToStereo();
		else if (ds->m_texture->GetArrayLayers() > rt->m_texture->GetArrayLayers())
			rt->PromoteToStereo();
	}
	else if (m_using_temp_z && rt && rt->m_texture && rt->m_texture->GetArrayLayers() > 1)
	{
		static bool s_warned_temp_z = false;
		if (!s_warned_temp_z)
		{
			s_warned_temp_z = true;
			Console.Warning("(VR) Stereo rt with temporary-Z depth — this draw combination is "
							"not yet layer-consistent (Phase A); expect right-eye depth artifacts here.");
		}
	}

	{
		static const bool s_srcguard_selftest = (std::getenv("PCSX2_VR_SRCGUARD_SELFTEST") != nullptr);
		static bool s_selftest_done = false;
		if (s_srcguard_selftest && !s_selftest_done && tex && tex->m_from_target)
			s_selftest_done = g_texture_cache->ForceKillInFlightSourceForSelfTest();
	}
#endif
	m_conf.rt = rt ? rt->m_texture : nullptr;
	m_conf.ds = ds ? (m_using_temp_z ? g_texture_cache->GetTemporaryZ() : ds->m_texture) : nullptr;

	pxAssert(!ds || !rt || (m_conf.ds->GetSize().x == m_conf.rt->GetSize().x && m_conf.ds->GetSize().y == m_conf.rt->GetSize().y));

	EmulateZbuffer(ds);

	if (m_channel_shuffle && tex && tex->m_from_target)
		EmulateChannelShuffle(tex->m_from_target, false, rt);

	MergeSprite(tex);

	m_prim_overlap = PrimitiveOverlap(false);

	EmulateAA1();

	if (rt)
	{
		EmulateTextureShuffleAndFbmask(rt, tex);
		if (m_index->tail == 0)
		{
			GL_INS("HW: DrawPrims: Texture shuffle emulation culled all vertices; exiting.");
			return;
		}
	}

	if (EmulateDATEEarlyFail(date_options, rt))
		return;

	const int afail_type = m_cached_ctx.TEST.GetAFAIL(m_cached_ctx.FRAME.PSM);
	if (m_cached_ctx.TEST.ATE && ((afail_type != AFAIL_FB_ONLY && afail_type != AFAIL_RGB_ONLY) || !NeedsBlending() || !IsUsingAsInBlend()))
	{
		const int aref = static_cast<int>(m_cached_ctx.TEST.AREF);
		CorrectATEAlphaMinMax(m_cached_ctx.TEST.ATST, aref);
	}

	int blend_alpha_min = 0, blend_alpha_max = 255;
	int rt_new_alpha_min = 0, rt_new_alpha_max = 255;
	CalculateAlphaRange(rt, ds, date_options, blend_alpha_min, blend_alpha_max, rt_new_alpha_min, rt_new_alpha_max);

	EmulateDATESelectMethod(date_options, rt, blend_alpha_min, blend_alpha_max);

	m_conf.ps.dither = GSConfig.Dithering > 0 && m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_16 && !!env.DTHE.DTHE;

	if (m_conf.ps.dst_fmt == GSLocalMemory::PSM_FMT_24)
	{
		m_conf.colormask.wa = 0;
	}

	const bool req_src_update = tex && rt && tex->m_target && tex->m_target_direct && tex->m_texture == rt->m_texture;

	bool can_scale_rt_alpha = false;
	bool new_scale_rt_alpha = false;
	DetermineAlphaScaling(rt, tex, req_src_update, rt_new_alpha_max, can_scale_rt_alpha, new_scale_rt_alpha);

	GSDevice::RecycledTexture tex_copy;
	if (tex)
	{
		EmulateTextureSampler(rt, ds, tex, tmm, tex_copy);
	}
	else
	{
		const float scale_factor = rt ? rt->GetScale() : ds->GetScale();
		m_conf.cb_ps.ScaleFactor = GSVector4(scale_factor * (1.0f / 16.0f), 1.0f / scale_factor, scale_factor, 0.0f);

		m_conf.ps.tfx = 4;
	}

	m_conf.ps.no_color1 = true;

	EmulateAlphaTest(date_options);

	m_conf.ps.fixed_one_a = IsCoverageAlphaFixedOne();

	if ((!IsOpaque() || m_context->ALPHA.IsBlack()) && rt && ((m_conf.colormask.wrgba & 0x7) || (m_texture_shuffle && !m_texture_shuffle.real_16_bit_source && !m_texture_shuffle.SameGroupShuffle())))
	{
		EmulateBlending(blend_alpha_min, blend_alpha_max, date_options, rt, can_scale_rt_alpha, new_scale_rt_alpha);
	}
	else
	{
		m_conf.blend = {};

		if (can_scale_rt_alpha && !new_scale_rt_alpha && m_conf.colormask.wa)
		{
			const bool afail_always_fb_alpha = m_cached_ctx.TEST.AFAIL == AFAIL_FB_ONLY || (m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY && GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].trbpp != 32);
			const bool always_passing_alpha = !m_cached_ctx.TEST.ATE || afail_always_fb_alpha || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST == ATST_ALWAYS);
			const bool full_cover = rt->m_valid.rintersect(m_r).eq(rt->m_valid) && m_primitive_covers_without_gaps == NoGapsType::FullCover &&
				!(date_options.enabled || !always_passing_alpha || !IsDepthAlwaysPassing());

			new_scale_rt_alpha = full_cover || rt->m_last_draw >= s_n;
		}
	}

	const bool no_rt = !rt || m_conf.colormask.wrgba == 0;
	const bool no_ds = !ds ||
		(!no_rt && m_cached_ctx.FRAME.FBP == m_cached_ctx.ZBUF.ZBP && !PRIM->TME && m_cached_ctx.ZBUF.ZMSK == 0 &&
			(m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk) == 0 && m_cached_ctx.TEST.ZTE) ||
		(no_rt && m_cached_ctx.ZBUF.ZMSK != 0);

	if (no_rt && no_ds)
	{
		GL_INS("HW: Late draw cancel DrawPrims().");
		return;
	}

	if ((m_conf.ps.tex_is_fb && rt && rt->m_rt_alpha_scale) || (tex && tex->m_from_target && tex->m_target_direct && tex->m_from_target->m_rt_alpha_scale))
		m_conf.ps.rta_source_correction = 1;

	if (req_src_update && tex->m_texture != rt->m_texture)
		tex->m_texture = rt->m_texture;

	if (rt)
	{
		rt->m_alpha_max = rt_new_alpha_max;
		rt->m_alpha_min = rt_new_alpha_min;
	}

	if (!rt || m_conf.colormask.wrgba == 0)
	{
		m_conf.ps.DisableColorOutput();
		m_conf.colormask.wrgba = 0;
	}

	GSDevice::RecycledTexture temp_ds;
	EmulateDATEGetConfig(date_options, new_scale_rt_alpha, temp_ds);

	const GSTextureCache::Target* rt_or_ds = rt ? rt : ds;
	const float rtscale = rt_or_ds->GetScale();
	const GSVector2i rtsize = rt_or_ds->GetTexture()->GetSize();
	const GSVector2i rt_unscaled_size = rt_or_ds->GetUnscaledSize();

	const float texscale = tex ? tex->GetScale() : 0.0f;
	const GSVector2i texsize = tex ? tex->GetTexture()->GetSize() : GSVector2i(0, 0);

	float vs_scale_x, vs_scale_y;
	DetermineVSConfig(rt, rtscale, rtsize, rt_unscaled_size, vs_scale_x, vs_scale_y);

	m_conf.ps.iip = !IsFlatShaded();

	m_conf.ps.fba = m_context->FBA.FBA;

	EmulateDither();

	if (PRIM->FGE)
	{
		m_conf.ps.fog = 1;

		const GSVector4 fc = GSVector4::rgba32(m_draw_env->FOGCOL.U32[0]);
		m_conf.cb_ps.FogColor_AREF = fc.blend32<8>(m_conf.cb_ps.FogColor_AREF);
	}

	if (rt)
	{
		GL_INS("HW: RT alpha is now %s", rt->m_rt_alpha_scale ? "scaled" : "NOT scaled");
		rt->m_rt_alpha_scale = new_scale_rt_alpha;
		m_conf.ps.rta_correction = rt->m_rt_alpha_scale;
	}

	DetermineROVUsage(rt, ds);
	ConvertTextureTypeROV(rt, ds);

	DetermineBarriers(rt, tex);

	EmulateAlphaTestSecondPass();

	if (m_conf.alpha_test == GSHWDrawConfig::AlphaTestMode::ABORT_DRAW)
	{
		GL_INS("HW: Aborting draw %s due to alpha test config.", s_n);
		return;
	}

	const GSVector4i hacked_scissor = m_channel_shuffle ? GSVector4i::cxpr(0, 0, 1024, 1024) : m_context->scissor.in;
	const GSVector4i scissor(GSVector4i(GSVector4(rtscale) * GSVector4(hacked_scissor)).rintersect(GSVector4i::loadh(rtsize)));

	m_conf.drawarea = m_channel_shuffle ? scissor : scissor.rintersect(ComputeBoundingBoxRT(rtsize, rtscale));

	const GSVector4i tex_region = tex ? tex->GetRegionRect() : GSVector4i::zero();
	m_conf.samplearea = m_channel_shuffle ? scissor :
		GSVector4i::loadh(texsize).rintersect(ComputeBoundingBoxTex(texsize, tmm.coverage, tex_region, texscale));

	m_conf.scissor = (date_options.enabled && !date_options.barrier) ? m_conf.drawarea : scissor;

	HandleFlatShadedVertices();

	SetupIA(rtscale, vs_scale_x, vs_scale_y, m_channel_shuffle_width != 0, no_rt);

	if (m_conf.ds && m_conf.ps.IsFeedbackLoopDepth() && !g_gs_device->Features().depth_feedback && !m_conf.ps.HasDepthROV())
	{
		GL_PUSH("HW: Creating temporary R32 RT for depth feedback");

		pxAssert(!m_conf.blend.enable && !m_conf.blend_multi_pass.blend.enable);
		pxAssert(m_conf.depth.zwe);
		pxAssert(m_conf.depth.ztst == ZTST_ALWAYS);
		pxAssert(!m_conf.alpha_second_pass.enable);

		g_gs_device->BeginDSAsRT(m_conf.ds, m_conf.drawarea);
	}
	
	if (GSConfig.SaveHWConfig && GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()))
	{
		GSHWDrawConfig::DumpConfig(GetDrawDumpPath("%05d_hwconfig.txt", s_n), m_conf);
	}

	if (!m_channel_shuffle_width)
		g_gs_device->RenderHW(m_conf);
	else
		m_last_rt = rt;

	if (g_gs_device->IsDSInRTActive())
		g_gs_device->EndDSAsRT();
}

bool GSRendererHW::HasEEUpload(GSVector4i r)
{
	for (auto iter = m_draw_transfers.begin(); iter != m_draw_transfers.end(); ++iter)
	{
		if (iter->draw == (s_n - 1) && iter->blit.DBP == m_cached_ctx.TEX0.TBP0 && GSUtil::HasSharedBits(iter->blit.DPSM, m_cached_ctx.TEX0.PSM))
		{
			GSVector4i rect = r;

			if (!GSUtil::HasCompatibleBits(iter->blit.DPSM, m_cached_ctx.TEX0.PSM))
			{
				GSTextureCache::SurfaceOffsetKey sok;
				sok.elems[0].bp = iter->blit.DBP;
				sok.elems[0].bw = iter->blit.DBW;
				sok.elems[0].psm = iter->blit.DPSM;
				sok.elems[0].rect = iter->rect;
				sok.elems[1].bp = m_cached_ctx.TEX0.TBP0;
				sok.elems[1].bw = m_cached_ctx.TEX0.TBW;
				sok.elems[1].psm = m_cached_ctx.TEX0.PSM;
				sok.elems[1].rect = r;

				rect = g_texture_cache->ComputeSurfaceOffset(sok).b2a_offset;
			}
			if (rect.rintersect(r).eq(r))
				return true;
		}
	}
	return false;
}

GSRendererHW::CLUTDrawTestResult GSRendererHW::PossibleCLUTDraw()
{
	if (m_channel_shuffle || m_texture_shuffle)
		return CLUTDrawTestResult::NotCLUTDraw;

	const bool fb_only = m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.GetAFAIL(m_cached_ctx.FRAME.PSM) == AFAIL_FB_ONLY && m_cached_ctx.TEST.ATST == ATST_NEVER;

	if (!m_cached_ctx.ZBUF.ZMSK && !fb_only && !(m_vt.m_primclass == GS_POINT_CLASS))
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_vt.m_eq.z != 0x1)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_context->TEX1.MXL)
		return CLUTDrawTestResult::NotCLUTDraw;

	if ((m_regs->DISP[0].DISPFB.Block() == m_cached_ctx.FRAME.Block()) || (m_regs->DISP[1].DISPFB.Block() == m_cached_ctx.FRAME.Block()) ||
		(m_process_texture && ((m_regs->DISP[0].DISPFB.Block() == m_cached_ctx.TEX0.TBP0) || (m_regs->DISP[1].DISPFB.Block() == m_cached_ctx.TEX0.TBP0)) && !(m_mem.m_clut.IsInvalid() & 2)))
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_process_texture && (m_cached_ctx.FRAME.FBW != 1 && m_cached_ctx.TEX0.TBW == m_cached_ctx.FRAME.FBW))
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_process_texture && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pal > 0)
		return CLUTDrawTestResult::NotCLUTDraw;

	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];

	if (GSLocalMemory::m_psm[m_mem.m_clut.GetCLUTCPSM()].bpp != psm.bpp)
		return CLUTDrawTestResult::NotCLUTDraw;

	constexpr float min_clut_width = 7.0f;
	constexpr float min_clut_height = 1.0f;
	const float page_width = static_cast<float>(psm.pgs.x);
	const float page_height = static_cast<float>(psm.pgs.y);

	if (floor(m_vt.m_min.p.x) < 0 || floor(m_vt.m_min.p.y) < 0 || floor(m_vt.m_min.p.x) > page_width || floor(m_vt.m_min.p.y) > page_height)
		return CLUTDrawTestResult::NotCLUTDraw;

	int draw_divder_match = false;
	const int valid_sizes[] = {8, 16, 32, 64};

	for (int i = 0; i < 4; i++)
	{
		draw_divder_match = ((m_vt.m_primclass == GS_POINT_CLASS) ? ((static_cast<int>(m_vt.m_max.p.x + 1) & ~1) == valid_sizes[i]) : (static_cast<int>(m_vt.m_max.p.x) == valid_sizes[i]));

		if (draw_divder_match)
			break;
	}
	const float draw_width = (m_vt.m_max.p.x - m_vt.m_min.p.x);
	const float draw_height = (m_vt.m_max.p.y - m_vt.m_min.p.y);
	const bool valid_size = ((draw_width >= min_clut_width || draw_height >= min_clut_height))
		&& (((draw_width < page_width && draw_height <= page_height) || (draw_width == page_width)) && draw_divder_match);
	
	if (!valid_size)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_process_texture)
	{
		const GSVector4i r = GetTextureMinMax(m_cached_ctx.TEX0, m_cached_ctx.CLAMP, m_vt.IsLinear(), false).coverage;

		if (GSConfig.UserHacks_GPUTargetCLUTMode != GSGPUTargetCLUTMode::Disabled)
		{
			if (HasEEUpload(r))
				return CLUTDrawTestResult::CLUTDrawOnCPU;

			GSTextureCache::Target* tgt = g_texture_cache->FindOverlappingTarget(
				m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.TBW, m_cached_ctx.TEX0.PSM, r);
			if (tgt)
			{
				tgt->UnscaleRTAlpha();
				bool is_dirty = false;
				for (const GSDirtyRect& rc : tgt->m_dirty)
				{
					if (!rc.GetDirtyRect(m_cached_ctx.TEX0, false).rintersect(r).rempty())
					{
						is_dirty = true;
						break;
					}
				}
				if (!is_dirty)
				{
					GL_INS("HW: GPU clut is enabled and this draw would readback, leaving on GPU");
					return CLUTDrawTestResult::CLUTDrawOnGPU;
				}
			}
		}
		else
		{
			if (HasEEUpload(r))
				return CLUTDrawTestResult::CLUTDrawOnCPU;
		}

		GIFRegBITBLTBUF BITBLTBUF = {};
		BITBLTBUF.SBP = m_cached_ctx.TEX0.TBP0;
		BITBLTBUF.SBW = m_cached_ctx.TEX0.TBW;
		BITBLTBUF.SPSM = m_cached_ctx.TEX0.PSM;

		InvalidateLocalMem(BITBLTBUF, r);
	}

	return CLUTDrawTestResult::CLUTDrawOnCPU;
}

GSRendererHW::CLUTDrawTestResult GSRendererHW::PossibleCLUTDrawAggressive()
{
	if (m_channel_shuffle || m_texture_shuffle)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_cached_ctx.TEST.ATE)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (NeedsBlending())
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_context->TEX1.MXL)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_cached_ctx.FRAME.FBW != 1)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (!m_cached_ctx.ZBUF.ZMSK)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (m_vt.m_eq.z != 0x1)
		return CLUTDrawTestResult::NotCLUTDraw;

	if (!((m_vt.m_primclass == GS_POINT_CLASS || m_vt.m_primclass == GS_LINE_CLASS) || ((m_mem.m_clut.GetCLUTCBP() >> 5) >= m_cached_ctx.FRAME.FBP && (m_cached_ctx.FRAME.FBP + 1U) >= (m_mem.m_clut.GetCLUTCBP() >> 5) && m_vt.m_primclass == GS_SPRITE_CLASS)))
		return CLUTDrawTestResult::NotCLUTDraw;

	return CLUTDrawTestResult::CLUTDrawOnCPU;
}

bool GSRendererHW::CanUseSwPrimRender(bool no_rt, bool no_ds, bool draw_sprite_tex)
{
	const int bw = GSConfig.UserHacks_CPUSpriteRenderBW;
	const int level = GSConfig.UserHacks_CPUSpriteRenderLevel;

	if (bw == 0)
		return false;

	if (no_rt || !no_ds || (level == 0 && !draw_sprite_tex))
		return false;

	if (m_cached_ctx.FRAME.FBW > static_cast<u32>(bw) && m_cached_ctx.FRAME.FBW != 32)
		return false;

	if (level < 2 && (IsMipMapActive() || !IsOpaque()))
		return false;

	if (m_split_texture_shuffle_pages)
		return false;

	if (PRIM->TME)
	{
		GSTextureCache::Target* src_target = g_texture_cache->GetTargetWithSharedBits(m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.PSM);
		if (src_target)
		{
			if (!IsOpaque())
			{
				GSTextureCache::Target* dst_target = g_texture_cache->GetTargetWithSharedBits(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.PSM);

				if (dst_target && dst_target->m_dirty.empty() && ((!(GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM) & 0x7)) || dst_target->m_valid_rgb) &&
					((!(GSUtil::GetChannelMask(m_cached_ctx.FRAME.PSM) & 0x8)) || (dst_target->m_valid_alpha_low && dst_target->m_valid_alpha_high)))
					return false;
			}

			const bool need_aem_color = GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp <= 24 && GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].pal == 0 && ((NeedsBlending() && m_context->ALPHA.C == 0) || IsDiscardingDstAlpha()) && m_cached_ctx.TEXA.AEM;
			const u32 color_mask = (m_vt.m_max.c > GSVector4i::zero()).mask();
			const bool texture_function_color = m_cached_ctx.TEX0.TFX == TFX_DECAL || (color_mask & 0xFFF) || (m_cached_ctx.TEX0.TFX > TFX_DECAL && (color_mask & 0xF000));
			const bool texture_function_alpha = m_cached_ctx.TEX0.TFX != TFX_MODULATE || (color_mask & 0xF000);
			const u32 fm_mask = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk;
			const bool req_color = (texture_function_color && (!PRIM->ABE || GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp < 16 || (NeedsBlending() && IsUsingCsInBlend())) && (m_cached_ctx.FRAME.FBMSK & (fm_mask & 0x00FFFFFF)) != (fm_mask & 0x00FFFFFF)) || need_aem_color;
			const bool alpha_used = (GSUtil::GetChannelMask(m_context->TEX0.PSM) == 0x8 || (m_context->TEX0.TCC && texture_function_alpha)) && ((NeedsBlending() && IsUsingAsInBlend()) || (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.ATST > ATST_ALWAYS) || (m_cached_ctx.FRAME.FBMSK & (fm_mask & 0xFF000000)) != (fm_mask & 0xFF000000));
			const bool req_alpha = (GSUtil::GetChannelMask(m_context->TEX0.PSM) & 0x8) && alpha_used;

			if ((req_color && !src_target->m_valid_rgb) || (req_alpha && (!src_target->m_valid_alpha_low || !src_target->m_valid_alpha_high)))
				return true;

			bool req_readback = false;
			if (!src_target->m_dirty.empty())
			{
				const GSVector4i tr(GetTextureMinMax(m_cached_ctx.TEX0, m_cached_ctx.CLAMP, m_vt.IsLinear(), false).coverage);

				const u32 start_bp = GSLocalMemory::GetStartBlockAddress(m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.TBW, m_cached_ctx.TEX0.PSM, tr);
				const u32 end_bp = GSLocalMemory::GetEndBlockAddress(m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.TBW, m_cached_ctx.TEX0.PSM, tr);

				for (GSDirtyRect& rc : src_target->m_dirty)
				{
					const GSVector4i dirty_rect = rc.GetDirtyRect(src_target->m_TEX0, false);
					const u32 dirty_start_bp = GSLocalMemory::GetStartBlockAddress(src_target->m_TEX0.TBP0, src_target->m_TEX0.TBW, src_target->m_TEX0.PSM, dirty_rect);
					const u32 dirty_end_bp = GSLocalMemory::GetEndBlockAddress(src_target->m_TEX0.TBP0, src_target->m_TEX0.TBW, src_target->m_TEX0.PSM, dirty_rect);

					if (start_bp < dirty_end_bp && end_bp > dirty_start_bp)
					{
						if (dirty_start_bp <= start_bp && dirty_end_bp >= end_bp)
						{
							return true;
						}
						else if (GSUtil::HasSameSwizzleBits(m_cached_ctx.TEX0.PSM, src_target->m_TEX0.PSM) || m_primitive_covers_without_gaps == NoGapsType::FullCover)
							return false;
					}
				}
			}
			else
			{
				GSVector4i src_rect = GSVector4i(m_vt.m_min.t.x, m_vt.m_min.t.y, m_vt.m_max.t.x, m_vt.m_max.t.y);
				GSVector4i area = g_texture_cache->TranslateAlignedRectByPage(src_target, m_cached_ctx.TEX0.TBP0, m_cached_ctx.TEX0.PSM, m_cached_ctx.TEX0.TBW, src_rect, false);
				req_readback = !area.rintersect(src_target->m_drawn_since_read).eq(GSVector4i::zero());
			}
			if (!GSUtil::HasSameSwizzleBits(m_cached_ctx.TEX0.PSM, src_target->m_TEX0.PSM) &&
				(!src_target->m_32_bits_fmt || GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].bpp != 16))
			{
				if (req_readback)
					g_texture_cache->Read(src_target, src_target->m_drawn_since_read);
				return true;
			}

			return false;
		}
	}

	if (PRIM->ABE && m_vt.m_eq.rgba == 0xffff && !m_context->ALPHA.IsOpaque(GetAlphaMinMax().min, GetAlphaMinMax().max))
	{
		GSTextureCache::Target* rt = g_texture_cache->GetTargetWithSharedBits(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.PSM);

		if (!rt || (!rt->m_dirty.empty() && rt->m_dirty.GetTotalRect(rt->m_TEX0, rt->m_unscaled_size).rintersect(m_r).eq(m_r)))
			return true;

		rt = nullptr;
		return false;
	}

	return true;
}

void GSRendererHW::SetNewFRAME(u32 bp, u32 bw, u32 psm)
{
	m_cached_ctx.FRAME.FBP = bp >> 5;
	m_cached_ctx.FRAME.FBW = bw;
	m_cached_ctx.FRAME.PSM = psm;
	m_context->offset.fb = m_mem.GetOffset(bp, bw, psm);
}

void GSRendererHW::SetNewZBUF(u32 bp, u32 psm)
{
	m_cached_ctx.ZBUF.ZBP = bp >> 5;
	m_cached_ctx.ZBUF.PSM = psm;
	m_context->offset.zb = m_mem.GetOffset(bp, m_cached_ctx.FRAME.FBW, psm);
}

bool GSRendererHW::DetectStripedDoubleClear(bool& no_rt, bool& no_ds)
{
	const bool single_page_offset =
		std::abs(static_cast<int>(m_cached_ctx.FRAME.FBP) - static_cast<int>(m_cached_ctx.ZBUF.ZBP)) == 1;
	const bool z_is_frame = (m_cached_ctx.FRAME.FBP == m_cached_ctx.ZBUF.ZBP || (m_cached_ctx.FRAME.FBW > 1 && single_page_offset)) &&
	                        !m_cached_ctx.ZBUF.ZMSK &&
	                        (m_cached_ctx.FRAME.PSM & 0x30) != (m_cached_ctx.ZBUF.PSM & 0x30) &&
	                        (m_cached_ctx.FRAME.PSM & 0xF) == (m_cached_ctx.ZBUF.PSM & 0xF) && m_vt.m_eq.z == 1 &&
	                        m_vertex->buff[1].XYZ.Z == m_vertex->buff[1].RGBAQ.U32[0];

	if (!z_is_frame || m_vt.m_eq.rgba != 0xFFFF)
		return false;

	const GSVector2i page_size = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].pgs;
	const int strip_size = ((single_page_offset) ? page_size.x : (page_size.x / 2));

	int vertex_offset = 0;
	int last_vertex = m_vertex->buff[0].XYZ.X;

	for (u32 i = 1; i < m_vertex->tail; i++)
	{
		vertex_offset = std::max(static_cast<int>((m_vertex->buff[i].XYZ.X - last_vertex) >> 4), vertex_offset);
		last_vertex = m_vertex->buff[i].XYZ.X;

		if (vertex_offset > strip_size)
			break;
	}

	const bool is_strips = vertex_offset == strip_size;

	if (!is_strips)
		return false;

	if (m_cached_ctx.FRAME.FBP < m_cached_ctx.ZBUF.ZBP || m_r.x == 0)
		m_r.z += vertex_offset;
	else
		m_r.x -= vertex_offset;

	GL_INS("HW: DetectStripedDoubleClear(): %d,%d => %d,%d @ FBP %x FBW %u ZBP %x", m_r.x, m_r.y, m_r.z, m_r.w,
		m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.Block());

	ReplaceVerticesWithSprite(m_r, GSVector2i(1, 1));

	m_cached_ctx.ZBUF.ZMSK = true;
	no_rt = false;
	no_ds = true;
	return true;
}

bool GSRendererHW::DetectDoubleHalfClear(bool& no_rt, bool& no_ds)
{
	if (m_cached_ctx.TEST.ZTST != ZTST_ALWAYS || m_cached_ctx.ZBUF.ZMSK)
		return false;

	const GSLocalMemory::psm_t& frame_psm = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM];
	const GSLocalMemory::psm_t& zbuf_psm = GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM];
	if (((m_cached_ctx.FRAME.FBMSK & frame_psm.fmsk) != 0 && (m_cached_ctx.FRAME.FBMSK & zbuf_psm.fmsk) != 0))
	{
		if ((m_cached_ctx.FRAME.FBMSK & frame_psm.fmsk) == (0xFF000000 & frame_psm.fmsk))
		{
			if (frame_psm.trbpp == 32 && zbuf_psm.trbpp == 32 && m_vt.m_max.c.a == 0 && m_vt.m_max.p.z < 0x1000000)
			{
				GSTextureCache::Target* frame = g_texture_cache->GetExactTarget(m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, GSTextureCache::RenderTarget, m_cached_ctx.FRAME.Block());

				if (frame && frame->m_alpha_max > 0)
					return false;
			}
			else
				return false;
		}
		else
			return false;
	}

	if (m_vt.m_eq.rgba != 0xFFFF || !m_vt.m_eq.z || (no_ds != no_rt))
		return false;

	const u32 write_color = GetConstantDirectWriteMemClearColor();
	const u32 write_depth = GetConstantDirectWriteMemClearDepth();
	if (write_color != write_depth)
		return false;

	const bool clear_depth = (m_cached_ctx.FRAME.FBP > m_cached_ctx.ZBUF.ZBP);
	const u32 base = clear_depth ? m_cached_ctx.ZBUF.ZBP : m_cached_ctx.FRAME.FBP;
	const u32 half = clear_depth ? m_cached_ctx.FRAME.FBP : m_cached_ctx.ZBUF.ZBP;
	const bool enough_bits = (frame_psm.trbpp == zbuf_psm.trbpp);

	const u32 w_pages = (m_r.z + (frame_psm.pgs.x - 1)) / frame_psm.pgs.x;
	const u32 h_pages = (m_r.w + (frame_psm.pgs.y - 1)) / frame_psm.pgs.y;
	const u32 written_pages = w_pages * h_pages;

	if (half > (base + written_pages) || half <= base)
		return false;

	const bool req_valid_alpha = ((frame_psm.fmsk & zbuf_psm.fmsk) & 0xFF000000u) != 0;
	GSTextureCache::Target* half_point = g_texture_cache->GetExactTarget(half << 5, m_cached_ctx.FRAME.FBW, clear_depth ? GSTextureCache::RenderTarget : GSTextureCache::DepthStencil, half << 5);
	half_point = (half_point && half_point->m_valid_rgb && half_point->HasValidAlpha() == req_valid_alpha) ? half_point : nullptr;
	if (half_point && half_point->m_age <= 1)
		return false;

	if ((!enough_bits && frame_psm.fmt != zbuf_psm.fmt && m_cached_ctx.FRAME.FBMSK != ((zbuf_psm.fmt == 1) ? 0xFF000000u : 0)) ||
		!GSUtil::HasCompatibleBits(m_cached_ctx.FRAME.PSM & ~0x30, m_cached_ctx.ZBUF.PSM & ~0x30))
	{
		GL_INS("HW: DetectDoubleHalfClear(): Inconsistent FRAME [%s, %08x] and ZBUF [%s] formats, not using double-half clear.",
			GSUtil::GetPSMName(m_cached_ctx.FRAME.PSM), m_cached_ctx.FRAME.FBMSK, GSUtil::GetPSMName(m_cached_ctx.ZBUF.PSM));

		if (write_color == 0)
		{
			const GSTextureCache::Target* base_tgt = g_texture_cache->GetExactTarget(base * GS_BLOCKS_PER_PAGE,
				m_cached_ctx.FRAME.FBW, clear_depth ? GSTextureCache::DepthStencil : GSTextureCache::RenderTarget,
				GSLocalMemory::GetEndBlockAddress(half * GS_BLOCKS_PER_PAGE, m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r));
			if (base_tgt)
			{
				GL_INS("HW: DetectDoubleHalfClear(): Invalidating targets at 0x%x/0x%x due to different formats, and clear to black.",
					base * GS_BLOCKS_PER_PAGE, half * GS_BLOCKS_PER_PAGE);
				g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, base * GS_BLOCKS_PER_PAGE);
				g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, half * GS_BLOCKS_PER_PAGE);
				g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, base * GS_BLOCKS_PER_PAGE);
				g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, half * GS_BLOCKS_PER_PAGE);
			}
		}

		return false;
	}

	if (m_state_flush_reason == CLUTCHANGE && clear_depth)
		return false;

	const int next_ctx = (m_state_flush_reason == CONTEXTCHANGE) ? m_env.PRIM.CTXT : (1 - m_env.PRIM.CTXT);

	if (m_env.CTXT[next_ctx].FRAME.FBP == m_cached_ctx.FRAME.FBP && m_env.CTXT[next_ctx].FRAME.FBW == m_cached_ctx.FRAME.FBW && m_r.width() == static_cast<int>(m_cached_ctx.FRAME.FBW * 64) && m_r.height() >= static_cast<int>(m_env.CTXT[next_ctx].SCISSOR.SCAY1 + 1))
		return false;

	bool horizontal = std::abs(static_cast<int>(m_cached_ctx.FRAME.FBP) - static_cast<int>(m_cached_ctx.ZBUF.ZBP)) == (m_cached_ctx.FRAME.FBW >> 1);
	const bool possible_next_clear = !m_env.PRIM.TME && !(m_env.SCANMSK.MSK & 2) && !m_env.CTXT[next_ctx].TEST.ATE && !m_env.CTXT[next_ctx].TEST.DATE &&
		(!m_env.CTXT[next_ctx].TEST.ZTE || m_env.CTXT[next_ctx].TEST.ZTST == ZTST_ALWAYS);

	const bool next_draw_match = m_env.CTXT[next_ctx].FRAME.FBP == m_cached_ctx.FRAME.FBP && m_env.CTXT[next_ctx].ZBUF.ZBP == m_cached_ctx.ZBUF.ZBP;

	if (next_draw_match && !possible_next_clear)
	{
		return false;
	}
	else
	{
		GSTextureCache::Target* tgt = g_texture_cache->GetTargetWithSharedBits(
			base * GS_BLOCKS_PER_PAGE, clear_depth ? m_cached_ctx.ZBUF.PSM : m_cached_ctx.FRAME.PSM);
		tgt = (tgt && tgt->m_valid_rgb && tgt->HasValidAlpha() == req_valid_alpha) ? tgt : nullptr;
		if (!tgt)
		{
			tgt = g_texture_cache->GetTargetWithSharedBits(
				base * GS_BLOCKS_PER_PAGE, clear_depth ? m_cached_ctx.FRAME.PSM : m_cached_ctx.ZBUF.PSM);
			tgt = (tgt && tgt->m_valid_rgb && tgt->HasValidAlpha() == req_valid_alpha) ? tgt : nullptr;
		}

		u32 end_block = ((half + written_pages) * GS_BLOCKS_PER_PAGE) - 1;

		if (tgt && tgt->m_age <= 1)
		{
			GSVector4i target_rect = tgt->GetUnscaledRect();
			if ((target_rect.w / 2) & (frame_psm.pgs.y - 1))
			{
				target_rect.w = ((target_rect.w / 2) + (frame_psm.pgs.y - 1)) & ~(frame_psm.pgs.y - 1);
				target_rect.w *= 2;
			}
			if ((m_cached_ctx.FRAME.FBW * 2) == (tgt->m_TEX0.TBW + 1))
				end_block = GSLocalMemory::GetUnwrappedEndBlockAddress(tgt->m_TEX0.TBP0, tgt->m_TEX0.TBW + 1, tgt->m_TEX0.PSM, target_rect);
			else
				end_block = GSLocalMemory::GetUnwrappedEndBlockAddress(tgt->m_TEX0.TBP0, (m_cached_ctx.FRAME.FBW == (tgt->m_TEX0.TBW / 2)) ? tgt->m_TEX0.TBW : m_cached_ctx.FRAME.FBW, tgt->m_TEX0.PSM, target_rect);

			const GSVector4 vr = GSVector4(m_r.rintersect(tgt->m_valid)) / GSVector4(tgt->m_valid);
			horizontal = (vr.z < vr.w);
		}
		else if (((m_env.CTXT[next_ctx].FRAME.FBW + 1) & ~1) == m_cached_ctx.FRAME.FBW * 2)
		{
			horizontal = true;
		}

		if ((((half + written_pages) * GS_BLOCKS_PER_PAGE) - 1) > end_block)
		{
			return false;
		}
	}

	GL_INS("HW: DetectDoubleHalfClear(): Clearing %s %s, fbp=%x, zbp=%x, pages=%u, base=%x, half=%x, rect=(%d,%d=>%d,%d)",
		clear_depth ? "depth" : "color", horizontal ? "horizontally" : "vertically", m_cached_ctx.FRAME.Block(),
		m_cached_ctx.ZBUF.Block(), written_pages, base * GS_BLOCKS_PER_PAGE, half * GS_BLOCKS_PER_PAGE, m_r.x, m_r.y, m_r.z,
		m_r.w);

	if (horizontal)
	{
		const int width = m_r.width();
		m_r.z = (w_pages * frame_psm.pgs.x);
		m_r.z += m_r.x + width;

		const u32 new_w_pages = (m_r.z + 63) / 64;
		if (new_w_pages > m_cached_ctx.FRAME.FBW)
		{
			GL_INS("HW: DetectDoubleHalfClear(): Doubling FBW because %u pages wide is less than FBW %u", new_w_pages, m_cached_ctx.FRAME.FBW);
			m_cached_ctx.FRAME.FBW *= 2;
		}
	}
	else
	{
		const int height = m_r.height();

		const int display_height = PCRTCDisplays.GetResolution().y;
		if ((display_height != 0 && height >= (display_height - 1)) || height > 300)
			return false;

		m_r.w = ((half - base) / m_cached_ctx.FRAME.FBW) * frame_psm.pgs.y;
		m_r.w += m_r.y + height;
	}
	ReplaceVerticesWithSprite(m_r, GSVector2i(1, 1));

	if (frame_psm.trbpp >= zbuf_psm.trbpp)
	{
		SetNewFRAME(base * GS_BLOCKS_PER_PAGE, m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM);
		m_cached_ctx.ZBUF.ZMSK = true;
		no_rt = false;
		no_ds = true;
	}
	else
	{
		SetNewZBUF(base * GS_BLOCKS_PER_PAGE, m_cached_ctx.ZBUF.PSM);
		m_cached_ctx.FRAME.FBMSK = 0xFFFFFFFF;
		no_rt = true;
		no_ds = false;
	}

	g_texture_cache->InvalidateVideoMemType(GSTextureCache::RenderTarget, half * GS_BLOCKS_PER_PAGE);
	g_texture_cache->InvalidateVideoMemType(GSTextureCache::DepthStencil, half * GS_BLOCKS_PER_PAGE);
	return true;
}

bool GSRendererHW::DetectRedundantBufferClear(bool& no_rt, bool& no_ds, u32 fm_mask)
{
	if (m_cached_ctx.FRAME.FBP != m_cached_ctx.ZBUF.ZBP || m_cached_ctx.ZBUF.ZMSK)
		return false;

	if (((~m_cached_ctx.FRAME.FBMSK & fm_mask) & GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmsk) == 0)
		return false;

	const bool is_zero_color_clear = m_vt.m_eq.rgba == 0xFFFF && GetConstantDirectWriteMemClearColor() == 0;
	const bool is_zero_depth_clear = m_vt.m_eq.z && GetConstantDirectWriteMemClearDepth() == 0;
	if (GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].bpp != GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].bpp &&
		is_zero_color_clear && is_zero_depth_clear)
	{
		m_cached_ctx.ZBUF.ZMSK = true;
		no_ds = true;
		no_rt = false;
		return true;
	}

	if ((m_r.x & 63) != 0 || (m_r.z & 63) != 0)
		return false;

	const u32 frame_bits_written = 32 - std::countl_zero(~m_cached_ctx.FRAME.FBMSK & fm_mask);

	const u32 z_bits_written = GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].trbpp;
	const GSTextureCache::Target* ztgt = g_texture_cache->GetTargetWithSharedBits(m_cached_ctx.ZBUF.Block(), m_cached_ctx.ZBUF.PSM);
	const bool keep_z = (ztgt && ztgt->m_valid_rgb && z_bits_written >= frame_bits_written) || (z_bits_written > frame_bits_written);
	GL_INS("HW: FRAME and ZBUF writing page-aligned same data, discarding %s", keep_z ? "FRAME" : "ZBUF");
	if (keep_z)
	{
		m_cached_ctx.FRAME.FBMSK = 0xFFFFFFFFu;
		no_rt = true;
		no_ds = false;
	}
	else
	{
		m_cached_ctx.ZBUF.ZMSK = true;
		no_ds = true;
		no_rt = false;
	}

	return true;
}

bool GSRendererHW::TryTargetClear(GSTextureCache::Target* rt, GSTextureCache::Target* ds, bool preserve_rt_color, bool preserve_depth)
{
	if (m_vt.m_eq.rgba != 0xFFFF || !m_vt.m_eq.z)
		return false;

	bool skip = true;
	if (rt)
	{
		if (!preserve_rt_color && !IsReallyDithered() && m_r.rintersect(rt->m_valid).eq(rt->m_valid))
		{
			const u32 c = GetConstantDirectWriteMemClearColor();
			u32 clear_c = c;
			const bool has_alpha = GSLocalMemory::m_psm[rt->m_TEX0.PSM].trbpp != 24;
			const bool alpha_one_or_less = has_alpha && (c >> 24) <= 0x80;
			GL_INS("HW: TryTargetClear(): RT at %x <= %08X", rt->m_TEX0.TBP0, c);

			if (rt->m_rt_alpha_scale || alpha_one_or_less)
			{
				if (alpha_one_or_less)
				{
					const u32 new_alpha = std::min((c >> 24) * 2U, 255U);
					clear_c = (clear_c & 0xFFFFFF) | (new_alpha << 24);
					rt->m_rt_alpha_scale = true;
				}
				else
				{
					rt->m_rt_alpha_scale = false;
				}
			}

			g_gs_device->ClearRenderTarget(rt->m_texture, clear_c);
			rt->m_dirty.clear();

			if (has_alpha)
			{
				rt->m_alpha_max = c >> 24;
				rt->m_alpha_min = c >> 24;
			}

			if (!rt->m_32_bits_fmt)
			{
				rt->m_alpha_max &= 128;
				rt->m_alpha_min &= 128;
			}
			rt->m_alpha_range = false;
		}
		else
		{
			skip = false;
		}
	}

	if (ds)
	{
		if (ds && !preserve_depth && m_r.rintersect(ds->m_valid).eq(ds->m_valid))
		{
			const u32 max_z = 0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8);
			const u32 z = std::min(max_z, m_vertex->buff[1].XYZ.Z);
			const float d = static_cast<float>(z) * 0x1p-32f;
			GL_INS("HW: TryTargetClear(): DS at %x <= %f", ds->m_TEX0.TBP0, d);
			g_gs_device->ClearDepth(ds->m_texture, d);
			ds->m_dirty.clear();
			ds->m_alpha_max = z >> 24;
			ds->m_alpha_min = z >> 24;

			if (GSLocalMemory::m_psm[ds->m_TEX0.PSM].bpp == 16)
			{
				ds->m_alpha_max &= 128;
				ds->m_alpha_min &= 128;
			}
		}
		else
		{
			skip = false;
		}
	}

	return skip;
}

bool GSRendererHW::TryGSMemClear(bool no_rt, bool preserve_rt, bool invalidate_rt, u32 rt_end_bp,
	bool no_ds, bool preserve_z, bool invalidate_z, u32 ds_end_bp)
{
	if (m_primitive_covers_without_gaps == NoGapsType::GapsFound)
		return false;

	if (m_r.width() < ((static_cast<int>(m_cached_ctx.FRAME.FBW) - 1) * 64))
		return false;

	if (!no_rt && (!preserve_rt || (IsOpaque() && m_cached_ctx.FRAME.FBMSK)))
	{
		ClearGSLocalMemory(m_context->offset.fb, m_r, GetConstantDirectWriteMemClearColor());

		if (invalidate_rt && !preserve_rt)
		{
			g_texture_cache->InvalidateVideoMem(m_context->offset.fb, m_r, false);
			g_texture_cache->InvalidateContainedTargets(
				GSLocalMemory::GetStartBlockAddress(
					m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r),
				rt_end_bp, m_cached_ctx.FRAME.PSM, m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.FBMSK);

			GSUploadQueue clear_queue;
			clear_queue.transfer_type = EEGS_TransferType::Clear;
			clear_queue.draw = s_n;
			clear_queue.rect = m_r;
			clear_queue.blit.DBP = m_cached_ctx.FRAME.Block();
			clear_queue.blit.DBW = m_cached_ctx.FRAME.FBW;
			clear_queue.blit.DPSM = m_cached_ctx.FRAME.PSM;
			m_draw_transfers.push_back(clear_queue);
		}
		else
		{
			g_texture_cache->InvalidateContainedTargets(
				GSLocalMemory::GetStartBlockAddress(
					m_cached_ctx.FRAME.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.PSM, m_r),
				rt_end_bp, m_cached_ctx.FRAME.PSM, m_cached_ctx.FRAME.FBW, m_cached_ctx.FRAME.FBMSK, true);
		}
	}

	if (!no_ds && !preserve_z)
	{
		ClearGSLocalMemory(m_context->offset.zb, m_r, m_vertex->buff[1].XYZ.Z);

		if (invalidate_z)
		{
			g_texture_cache->InvalidateVideoMem(m_context->offset.zb, m_r, false);
			g_texture_cache->InvalidateContainedTargets(
				GSLocalMemory::GetStartBlockAddress(
					m_cached_ctx.ZBUF.Block(), m_cached_ctx.FRAME.FBW, m_cached_ctx.ZBUF.PSM, m_r),
				ds_end_bp, m_cached_ctx.ZBUF.PSM, m_cached_ctx.FRAME.FBW);

			GSUploadQueue clear_queue;
			clear_queue.transfer_type = EEGS_TransferType::Clear;
			clear_queue.draw = s_n;
			clear_queue.rect = m_r;
			clear_queue.blit.DBP = m_cached_ctx.ZBUF.Block();
			clear_queue.blit.DBW = m_cached_ctx.FRAME.FBW;
			clear_queue.blit.DPSM = m_cached_ctx.ZBUF.PSM;
			m_draw_transfers.push_back(clear_queue);
		}
	}

	return ((invalidate_rt || no_rt) && (invalidate_z || no_ds));
}

void GSRendererHW::ClearGSLocalMemory(const GSOffset& off, const GSVector4i& r, u32 vert_color)
{
	GL_INS("HW: ClearGSLocalMemory(): %08X %d,%d => %d,%d @ BP %x BW %u %s", vert_color, r.x, r.y, r.z, r.w, off.bp(),
		off.bw(), GSUtil::GetPSMName(off.psm()));

	const u32 psm = (off.psm() == PSMCT32 && m_cached_ctx.FRAME.FBMSK == 0xFF000000u) ? PSMCT24 : off.psm();
	const int format = GSLocalMemory::m_psm[psm].fmt;

	const int left = r.left;
	const int right = r.right;
	const int bottom = r.bottom;
	int top = r.top;
	u32 drawing_mask = GSLocalMemory::m_psm[psm].depth ? 0x0 : m_cached_ctx.FRAME.FBMSK;

	const u32 fbw = m_cached_ctx.FRAME.FBW;
	const u32 pages_wide = r.z / 64u;
	const GSVector2i& pgs = GSLocalMemory::m_psm[psm].pgs;
	if (left == 0 && top == 0 && (right & (pgs.x - 1)) == 0 && pages_wide <= fbw)
	{
		const u32 pixels_per_page = pgs.x * pgs.y;
		const int page_aligned_bottom = (bottom & ~(pgs.y - 1));

		if (format == GSLocalMemory::PSM_FMT_32)
		{
			const GSVector4i vcolor = GSVector4i(vert_color & ~drawing_mask);
			const u32 iterations_per_page = (pages_wide * pixels_per_page) / 4;
			const GSVector4i mask = GSVector4i(drawing_mask);
			pxAssert((off.bp() & (GS_BLOCKS_PER_PAGE - 1)) == 0);
			for (u32 current_page = off.bp() >> 5; top < page_aligned_bottom; top += pgs.y, current_page += fbw)
			{
				current_page &= (GS_MAX_PAGES - 1);
				GSVector4i* ptr = reinterpret_cast<GSVector4i*>(m_mem.vm8() + current_page * GS_PAGE_SIZE);
				GSVector4i* const ptr_end = ptr + iterations_per_page;
				if (drawing_mask)
				{
					while (ptr != ptr_end)
					{
						*ptr = (*ptr & mask) | vcolor;
						ptr++;
					}
				}
				else
				{
					while (ptr != ptr_end)
						*(ptr++) = vcolor;
				}
			}
		}
		else if (format == GSLocalMemory::PSM_FMT_24)
		{
			const GSVector4i mask = GSVector4i::xff000000() | GSVector4i(drawing_mask);
			const GSVector4i vcolor = GSVector4i((vert_color & 0x00ffffffu) & ~drawing_mask);
			const u32 iterations_per_page = (pages_wide * pixels_per_page) / 4;
			pxAssert((off.bp() & (GS_BLOCKS_PER_PAGE - 1)) == 0);
			for (u32 current_page = off.bp() >> 5; top < page_aligned_bottom; top += pgs.y, current_page += fbw)
			{
				current_page &= (GS_MAX_PAGES - 1);
				GSVector4i* ptr = reinterpret_cast<GSVector4i*>(m_mem.vm8() + current_page * GS_PAGE_SIZE);
				GSVector4i* const ptr_end = ptr + iterations_per_page;
				while (ptr != ptr_end)
				{
					*ptr = (*ptr & mask) | vcolor;
					ptr++;
				}
			}
		}
		else if (format == GSLocalMemory::PSM_FMT_16)
		{
			const u16 converted_color = ((vert_color >> 16) & 0x8000) | ((vert_color >> 9) & 0x7C00) |
			                            ((vert_color >> 6) & 0x7E0) | ((vert_color >> 3) & 0x1F);
			const u16 converted_mask = ((drawing_mask >> 16) & 0x8000) | ((drawing_mask >> 9) & 0x7C00) |
			                           ((drawing_mask >> 6) & 0x7E0) | ((drawing_mask >> 3) & 0x1F);
			const GSVector4i vcolor = GSVector4i::broadcast16(converted_color);
			const GSVector4i mask = GSVector4i::broadcast16(converted_mask);
			const u32 iterations_per_page = (pages_wide * pixels_per_page) / 8;
			pxAssert((off.bp() & (GS_BLOCKS_PER_PAGE - 1)) == 0);
			for (u32 current_page = off.bp() >> 5; top < page_aligned_bottom; top += pgs.y, current_page += fbw)
			{
				current_page &= (GS_MAX_PAGES - 1);
				GSVector4i* ptr = reinterpret_cast<GSVector4i*>(m_mem.vm8() + current_page * GS_PAGE_SIZE);
				GSVector4i* const ptr_end = ptr + iterations_per_page;
				if (converted_mask)
				{
					while (ptr != ptr_end)
					{
						*ptr = (*ptr & mask) | vcolor;
						ptr++;
					}
				}
				else
				{
					while (ptr != ptr_end)
						*(ptr++) = vcolor;
				}
			}
		}
	}

	if (format == GSLocalMemory::PSM_FMT_32)
	{
		const u32 mask = drawing_mask;
		const u32 vcolor = vert_color & ~mask;
		u32* vm = m_mem.vm32();
		for (int y = top; y < bottom; y++)
		{
			GSOffset::PAHelper pa = off.assertSizesMatch(GSLocalMemory::swizzle32).paMulti(0, y);

			for (int x = left; x < right; x++)
				vm[pa.value(x)] = vcolor | (vm[pa.value(x)] & mask);
		}
	}
	else if (format == GSLocalMemory::PSM_FMT_24)
	{
		u32* vm = m_mem.vm32();
		const u32 mask = drawing_mask | 0xff000000u;
		const u32 write_color = (vert_color & 0xffffffu) & ~mask;
		for (int y = top; y < bottom; y++)
		{
			GSOffset::PAHelper pa = off.assertSizesMatch(GSLocalMemory::swizzle32).paMulti(0, y);

			for (int x = left; x < right; x++)
				vm[pa.value(x)] = (vm[pa.value(x)] & mask) | write_color;
		}
	}
	else if (format == GSLocalMemory::PSM_FMT_16)
	{
		const u16 converted_mask = ((drawing_mask >> 16) & 0x8000) | ((drawing_mask >> 9) & 0x7C00) |
		                           ((drawing_mask >> 6) & 0x7E0) | ((drawing_mask >> 3) & 0x1F);
		const u16 converted_color = (((vert_color >> 16) & 0x8000) | ((vert_color >> 9) & 0x7C00) | ((vert_color >> 6) & 0x7E0) | ((vert_color >> 3) & 0x1F)) & ~converted_mask;

		u16* vm = m_mem.vm16();
		for (int y = top; y < bottom; y++)
		{
			GSOffset::PAHelper pa = off.assertSizesMatch(GSLocalMemory::swizzle16).paMulti(0, y);

			for (int x = left; x < right; x++)
				vm[pa.value(x)] = converted_color | (vm[pa.value(x)] & converted_mask);
		}
	}
}

bool GSRendererHW::OI_BlitFMV(GSTextureCache::Target* _rt, GSTextureCache::Source* tex, const GSVector4i& r_draw)
{
	if (r_draw.w > 1024 && (m_vt.m_primclass == GS_SPRITE_CLASS) && (m_vertex->next == 2) && m_process_texture && !PRIM->ABE &&
		tex && !tex->m_target && m_cached_ctx.TEX0.TBW > 0 && GSConfig.UserHacks_TextureInsideRt == GSTextureInRtMode::Disabled)
	{
		GL_PUSH("HW: OI_BlitFMV");

		GL_INS("HW: OI_BlitFMV");

		const int tw = tex->m_unscaled_size.x;
		int th = tex->m_unscaled_size.y;

		pxAssert(m_cached_ctx.TEX0.TBP0 > m_cached_ctx.FRAME.Block());
		const int offset = (m_cached_ctx.TEX0.TBP0 - m_cached_ctx.FRAME.Block()) / m_cached_ctx.TEX0.TBW;
		GSVector4i r_texture(r_draw);
		r_texture.y -= offset;
		r_texture.w -= offset;
		const int new_height = std::max(r_texture.w, th);

		if (GSTexture* temp_tex = g_gs_device->CreateTexture(tw, new_height, 1, tex->m_texture->GetFormat(), true))
		{
			if (GSTexture* rt = g_gs_device->CreateFeedbackTarget(tw, new_height, GSTexture::Format::Color))
			{
				const GSVector4 dRect = GSVector4(r_texture) + GSVector4(0.5f);
				const GSVector4i r_full(0, 0, tw, th);

				g_gs_device->CopyRect(tex->m_texture, rt, r_full, 0, 0);

				g_texture_cache->ReplaceSourceTexture(tex, temp_tex, tex->m_scale, GSVector2i(tw, new_height), nullptr, false);

				if (tex->m_region.HasY())
				{
					if (tex->m_region.GetMaxY() == th)
					{
						tex->m_region.bits &= ~(0xFFFF0000ULL << 32);
						tex->m_region.SetY(tex->m_region.GetMinY(), new_height);
					}
				}
				th = new_height;
				const GSVector4 sRect(m_vt.m_min.t.x / tw, m_vt.m_min.t.y / th, m_vt.m_max.t.x / tw, m_vt.m_max.t.y / th);
				const GSVector4i r_full_new(0, 0, tw, th);
				g_gs_device->StretchRectAuto(tex->m_texture, sRect, rt, dRect, BilnIf(m_vt.IsRealLinear()));
				g_gs_device->CopyRect(rt, tex->m_texture, r_full_new, 0, 0);
				g_gs_device->Recycle(rt);
			}

			g_texture_cache->Read(tex, r_texture.rintersect(tex->m_texture->GetRect()));

		}
		g_texture_cache->InvalidateVideoMemSubTarget(_rt);

		return false;
	}

	return true;
}

bool GSRendererHW::AreAnyPixelsDiscarded() const
{
	return ((m_draw_env->SCANMSK.MSK & 2) ||
	        (m_cached_ctx.TEST.ATE && m_cached_ctx.TEST.AFAIL != AFAIL_FB_ONLY) ||
	        m_cached_ctx.TEST.DATE);
}

bool GSRendererHW::IsDiscardingDstColor()
{
	return ((!PRIM->ABE || IsOpaque() || m_context->ALPHA.IsBlack()) &&
	        !AreAnyPixelsDiscarded() && (m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk) == 0);
}

bool GSRendererHW::IsDiscardingDstRGB()
{
	return ((!PRIM->ABE || IsOpaque() || m_context->ALPHA.IsBlack() || !m_context->ALPHA.IsCdInBlend()) &&
	        ((m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk) & 0xFFFFFFu) == 0);
}

bool GSRendererHW::IsDiscardingDstAlpha() const
{
	return ((!PRIM->ABE || m_context->ALPHA.C != 1) &&
	        ((m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk) & 0xFF000000u) == 0) &&
	       (!m_cached_ctx.TEST.ATE || !(m_cached_ctx.TEST.ATST == ATST_NEVER && m_cached_ctx.TEST.AFAIL == AFAIL_RGB_ONLY && m_cached_ctx.FRAME.PSM == PSMCT32));
}

bool GSRendererHW::TextureCoversWithoutGapsNotEqual()
{
	if (m_vt.m_primclass != GS_SPRITE_CLASS)
	{
		return false;
	}

	if (m_index->tail == 2)
	{
		return true;
	}

	const GSVertex* v = &m_vertex->buff[0];
	const int first_dpY = v[1].XYZ.Y - v[0].XYZ.Y;
	const int first_dpX = v[1].XYZ.X - v[0].XYZ.X;
	const int first_dtV = v[1].V - v[0].V;
	const int first_dtU = v[1].U - v[0].U;

	if ((first_dpX >> 4) == m_r.z)
	{
		for (u32 i = 2; i < m_vertex->next; i += 2)
		{
			const int last_tV = v[i - 1].V;
			const int dtV = v[i + 1].V - v[i].V;
			const u32 last_tV_diff = std::abs(static_cast<int>(v[i].XYZ.Y) - last_tV);
			if (std::abs(dtV - first_dtV) >= 16 || last_tV_diff >= 16 || last_tV_diff == 0)
			{
				return false;
			}
		}

		return true;
	}

	if ((first_dpY >> 4) == m_r.w)
	{
		for (u32 i = 2; i < m_vertex->next; i += 2)
		{
			const int last_tU = v[i - 1].U;
			const int this_start_U = v[i].U;
			const int last_start_U = v[i - 2].U;

			const int dtU = v[i + 1].U - v[i].U;

			if (this_start_U < last_start_U)
			{
				if (std::abs(dtU - last_start_U) >= 16 || std::abs(this_start_U) >= 16)
				{
					return false;
				}
			}
			else
			{
				const u32 last_tU_diff = std::abs(this_start_U - last_tU);
				if (std::abs(dtU - first_dtU) >= 16 || last_tU_diff >= 16 || last_tU_diff == 0)
				{
					return false;
				}
			}
		}

		return true;
	}

	return false;
}

int GSRendererHW::IsScalingDraw(GSTextureCache::Source* src, bool no_gaps)
{
	if (GSConfig.UserHacks_NativeScaling == GSNativeScaling::Off)
		return 0;

	const GSVector2i draw_size = GSVector2i(m_vt.m_max.p.x - m_vt.m_min.p.x, m_vt.m_max.p.y - m_vt.m_min.p.y);
	GSVector2i tex_size = GSVector2i(m_vt.m_max.t.x - m_vt.m_min.t.x, m_vt.m_max.t.y - m_vt.m_min.t.y);

	tex_size.x = std::min(tex_size.x, 1 << m_cached_ctx.TEX0.TW);
	tex_size.y = std::min(tex_size.y, 1 << m_cached_ctx.TEX0.TH);

	const bool is_target_src = src && src->m_from_target;

	if (tex_size.x == 0 || tex_size.y == 0 || draw_size.x == 0 || draw_size.y == 0)
		return 0;

	const bool no_resize = (std::abs(draw_size.x - tex_size.x) <= 1 && std::abs(draw_size.y - tex_size.y) <= 1);
	const bool can_maintain = no_resize || (!is_target_src && m_index->tail == 2);

	if (!src || ((!is_target_src || (src->m_from_target->m_downscaled || GSConfig.UserHacks_NativeScaling > GSNativeScaling::Aggressive)) && can_maintain))
		return -1;

	const GSDrawingContext& next_ctx = m_env.CTXT[m_env.PRIM.CTXT];
	const bool next_tex0_is_draw = m_env.PRIM.TME && next_ctx.TEX0.TBP0 == m_cached_ctx.FRAME.Block() && next_ctx.TEX1.MMAG == 1;
	if (!PRIM->TME || (m_context->TEX1.MMAG != 1 && !next_tex0_is_draw) || m_vt.m_primclass < GS_TRIANGLE_CLASS || m_cached_ctx.FRAME.Block() == m_cached_ctx.TEX0.TBP0 ||
		IsMipMapDraw() || GSLocalMemory::m_psm[m_cached_ctx.TEX0.PSM].trbpp <= 8)
		return 0;

	const bool is_downscale = m_cached_ctx.TEX0.TBW >= m_cached_ctx.FRAME.FBW && draw_size.x <= (tex_size.x * 0.75f) && draw_size.y <= (tex_size.y * 0.75f);
	const GSVector4i src_valid = src->m_from_target ? src->m_from_target->m_valid : src->m_valid_rect;
	const GSVector2i tex_size_half = GSVector2i((src->GetRegion().HasX() ? src->GetRegionSize().x : src_valid.width()) / 2, (src->GetRegion().HasY() ? src->GetRegionSize().y : src_valid.height()) / 2);
	const bool possible_downscale = m_context->TEX1.MMIN == 1 || !src->m_from_target || src->m_from_target->m_downscaled || GSConfig.UserHacks_NativeScaling > GSNativeScaling::Aggressive || tex_size.x >= tex_size_half.x || tex_size.y >= tex_size_half.y;

	if (is_downscale && (draw_size.x >= PCRTCDisplays.GetResolution().x || !possible_downscale))
		return 0;

	const bool is_upscale = m_cached_ctx.TEX0.TBW <= m_cached_ctx.FRAME.FBW && ((draw_size.x / tex_size.x) >= 4 || (draw_size.y / tex_size.y) >= 4);
	const bool no_gaps_or_single_sprite = (is_downscale || is_upscale) && (no_gaps || (m_vt.m_primclass == GS_SPRITE_CLASS && SpriteDrawWithoutGaps()));

	const bool dst_discarded = IsDiscardingDstRGB() || IsDiscardingDstAlpha();
	if (no_gaps_or_single_sprite && ((is_upscale && !m_context->ALPHA.IsOpaque()) ||
		(is_downscale && (dst_discarded || (PRIM->ABE && m_context->ALPHA.C == 2 && m_context->ALPHA.FIX == 255)))))
	{
		GL_INS("HW: %s draw detected - from %dx%d to %dx%d draw %lld", is_downscale ? "Downscale" : "Upscale", tex_size.x, tex_size.y, draw_size.x, draw_size.y, s_n);
		return is_upscale ? 2 : 1;
	}

	if (m_vt.m_primclass == GS_SPRITE_CLASS && m_index->tail > 2 && !no_gaps_or_single_sprite && m_context->TEX1.MMAG == 1 && !m_context->ALPHA.IsOpaque())
	{
		GSVertex* v = &m_vertex->buff[0];
		float tw = 1 << src->m_TEX0.TW;
		float th = 1 << src->m_TEX0.TH;

		const int first_u = (PRIM->FST) ? (v[1].U - v[0].U) >> 4 : std::floor(static_cast<int>(tw * v[1].ST.S) - static_cast<int>(tw * v[0].ST.S));
		const int first_v = (PRIM->FST) ? (v[1].V - v[0].V) >> 4 : std::floor(static_cast<int>(th * v[1].ST.T) - static_cast<int>(th * v[0].ST.T));
		const int first_x = (v[1].XYZ.X - v[0].XYZ.X) >> 4;
		const int first_y = (v[1].XYZ.Y - v[0].XYZ.Y) >> 4;

		if (first_x > first_u && first_y > first_v && !no_resize && std::abs(draw_size.x - first_x) <= 4 && std::abs(draw_size.y - first_y) <= 4)
		{
			for (u32 i = 2; i < m_index->tail; i += 2)
			{
				const int next_u = (PRIM->FST) ? (v[i + 1].U - v[i].U) >> 4 : std::floor(static_cast<int>(tw * v[i + 1].ST.S) - static_cast<int>(tw * v[i].ST.S));
				const int next_v = (PRIM->FST) ? (v[i + 1].V - v[i].V) >> 4 : std::floor(static_cast<int>(th * v[i + 1].ST.T) - static_cast<int>(th * v[i].ST.T));
				const int next_x = (v[i + 1].XYZ.X - v[i].XYZ.X) >> 4;
				const int next_y = (v[i + 1].XYZ.Y - v[i].XYZ.Y) >> 4;

				if (std::abs(draw_size.x - next_x) > 4 || std::abs(draw_size.y - next_y) > 4)
					break;

				if (next_u != first_u || next_v != first_v || next_x != first_x || next_y != first_y)
					break;

				if (i + 2 >= m_index->tail)
					return 2;
			}
		}
	}

	return 0;
}

ClearType GSRendererHW::IsConstantDirectWriteMemClear()
{
	const bool direct_draw = (m_vt.m_primclass == GS_SPRITE_CLASS) || (m_vt.m_primclass == GS_TRIANGLE_CLASS && (m_index->tail % 6) == 0 && TrianglesAreQuads());
	if (direct_draw && !PRIM->TME
		&& !(m_draw_env->SCANMSK.MSK & 2) && !m_cached_ctx.TEST.ATE
		&& !m_cached_ctx.TEST.DATE
		&& (!m_cached_ctx.TEST.ZTE || m_cached_ctx.TEST.ZTST == ZTST_ALWAYS)
		&& (m_vt.m_eq.rgba == 0xFFFF || m_vertex->next == 2)
		&& (!PRIM->FGE || m_vt.m_min.p.w == 255.0f))
	{
		if ((PRIM->ABE && !m_context->ALPHA.IsOpaque()) || (m_cached_ctx.FRAME.FBMSK & GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmsk))
			return ClearWithDraw;

		return NormalClear;
	}
	return NotClear;
}

u32 GSRendererHW::GetConstantDirectWriteMemClearColor() const
{
	const u32 vert_index = (m_vt.m_primclass == GS_TRIANGLE_CLASS) ? 2 : 1;
	u32 vert_color = m_vertex->buff[m_index->buff[vert_index]].RGBAQ.U32[0];
	if (PRIM->ABE && m_context->ALPHA.IsBlack())
		vert_color &= 0xFF000000u;

	const u32 cfmt = GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmt;
	if (cfmt == 1)
		vert_color &= 0xFFFFFFu;
	else
		vert_color |= m_context->FBA.FBA << 31;

	if (cfmt == 2)
		vert_color &= 0x80F8F8F8u;

	return vert_color;
}

u32 GSRendererHW::GetConstantDirectWriteMemClearDepth() const
{
	const u32 max_z = (0xFFFFFFFF >> (GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].fmt * 8));
	return std::min(m_vertex->buff[1].XYZ.Z, max_z);
}

bool GSRendererHW::IsReallyDithered() const
{
	const GSDrawingEnvironment* env = m_draw_env;
	if (!env->DTHE.DTHE || GSConfig.Dithering == 0 || GSLocalMemory::m_psm[m_cached_ctx.FRAME.PSM].fmt != 2)
		return false;

	if ((env->DIMX.U64 & UINT64_C(0x7777777777777777)) == 0)
		return false;

	return true;
}

void GSRendererHW::ReplaceVerticesWithSprite(const GSVector4i& unscaled_rect, const GSVector4i& unscaled_uv_rect,
	const GSVector2i& unscaled_size, const GSVector4i& scissor)
{
	const GSVector4i fpr = unscaled_rect.sll32<4>();
	const GSVector4i fpuv = unscaled_uv_rect.sll32<4>();
	GSVertex* v = m_vertex->buff;

	v[0].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + fpr.x);
	v[0].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + fpr.y);
	v[0].XYZ.Z = v[1].XYZ.Z;
	v[0].RGBAQ = v[1].RGBAQ;
	v[0].FOG = v[1].FOG;

	v[1].XYZ.X = static_cast<u16>(m_context->XYOFFSET.OFX + fpr.z);
	v[1].XYZ.Y = static_cast<u16>(m_context->XYOFFSET.OFY + fpr.w);

	if (PRIM->FST)
	{
		v[0].U = fpuv.x;
		v[0].V = fpuv.y;
		v[1].U = fpuv.z;
		v[1].V = fpuv.w;
	}
	else
	{
		const GSVector4 st = GSVector4(unscaled_uv_rect) / GSVector4(GSVector4i(unscaled_size).xyxy());
		GSVector4::storel(&v[0].ST.S, st);
		GSVector4::storeh(&v[1].ST.S, st);
	}

	m_vt.m_min.p.x = unscaled_rect.x;
	m_vt.m_min.p.y = unscaled_rect.y;
	m_vt.m_min.p.z = v[0].XYZ.Z;
	m_vt.m_max.p.x = unscaled_rect.z;
	m_vt.m_max.p.y = unscaled_rect.w;
	m_vt.m_max.p.z = v[0].XYZ.Z;
	m_vt.m_min.t.x = unscaled_uv_rect.x;
	m_vt.m_min.t.y = unscaled_uv_rect.y;
	m_vt.m_max.t.x = unscaled_uv_rect.z;
	m_vt.m_max.t.y = unscaled_uv_rect.w;
	m_vt.m_min.c = GSVector4i(v[0].RGBAQ.U32[0]).u8to32();
	m_vt.m_max.c = m_vt.m_min.c;
	m_vt.m_eq.rgba = 0xFFFF;
	m_vt.m_eq.z = true;
	m_vt.m_eq.f = true;

	m_vertex->head = m_vertex->tail = m_vertex->next = 2;
	m_index->tail = 2;

	m_r = unscaled_rect;
	m_context->scissor.in = scissor;
	m_vt.m_primclass = GS_SPRITE_CLASS;

	m_drawlist.clear();
	m_prim_overlap = PRIM_OVERLAP_NO;
}

void GSRendererHW::ReplaceVerticesWithSprite(const GSVector4i& unscaled_rect, const GSVector2i& unscaled_size)
{
	ReplaceVerticesWithSprite(unscaled_rect, unscaled_rect, unscaled_size, unscaled_rect);
}

void GSRendererHW::OffsetDraw(s32 fbp_offset, s32 zbp_offset, s32 xoffset, s32 yoffset)
{
	GL_INS("HW: Offseting render target by %d pages [%x -> %x], Z by %d pages [%x -> %x]",
		fbp_offset, m_cached_ctx.FRAME.FBP << 5, zbp_offset, (m_cached_ctx.FRAME.FBP + fbp_offset) << 5);
	GL_INS("HW: Offseting vertices by [%d, %d]", xoffset, yoffset);

	m_cached_ctx.FRAME.FBP += fbp_offset;
	m_cached_ctx.ZBUF.ZBP += zbp_offset;

	const s32 fp_xoffset = xoffset << 4;
	const s32 fp_yoffset = yoffset << 4;
	for (u32 i = 0; i < m_vertex->next; i++)
	{
		m_vertex->buff[i].XYZ.X += fp_xoffset;
		m_vertex->buff[i].XYZ.Y += fp_yoffset;
	}

	m_vt.m_min.p.x += static_cast<float>(xoffset);
	m_vt.m_min.p.y += static_cast<float>(yoffset);
	m_vt.m_max.p.x += static_cast<float>(xoffset);
	m_vt.m_max.p.y += static_cast<float>(yoffset);

	m_r.x += xoffset;
	m_r.y += yoffset;
	m_r.z += xoffset;
	m_r.w += yoffset;
}

GSHWDrawConfig& GSRendererHW::BeginHLEHardwareDraw(
	GSTexture* rt, GSTexture* ds, float rt_scale, GSTexture* tex, float tex_scale, const GSVector4i& unscaled_rect)
{
	ResetStates();

	GSHWDrawConfig& config = m_conf;
	std::memset(static_cast<void*>(&config.cb_vs), 0, sizeof(config.cb_vs));
	std::memset(static_cast<void*>(&config.cb_ps), 0, sizeof(config.cb_ps));

	static GSVertex vertices[4];
	static constexpr u16 indices[6] = {0, 1, 2, 2, 1, 3};

#define V(i, x, y, u, v) \
	do \
	{ \
		vertices[i].XYZ.X = x; \
		vertices[i].XYZ.Y = y; \
		vertices[i].U = u; \
		vertices[i].V = v; \
	} while (0)

	const GSVector4i fp_rect = unscaled_rect.sll32<4>();
	V(0, fp_rect.x, fp_rect.y, fp_rect.x, fp_rect.y);
	V(1, fp_rect.z, fp_rect.y, fp_rect.z, fp_rect.y);
	V(2, fp_rect.x, fp_rect.w, fp_rect.x, fp_rect.w);
	V(3, fp_rect.z, fp_rect.w, fp_rect.z, fp_rect.w);

#undef V

	GSTexture* rt_or_ds = rt ? rt : ds;
	config.rt = rt;
	config.ds = ds;
	config.tex = tex;
	config.pal = nullptr;
	config.indices = indices;
	config.verts = vertices;
	config.nverts = static_cast<u32>(std::size(vertices));
	config.nindices = static_cast<u32>(std::size(indices));
	config.indices_per_prim = 3;
	config.drawlist = nullptr;
	config.scissor = rt_or_ds->GetRect();
	config.drawarea = config.scissor;
	config.topology = GSHWDrawConfig::Topology::Triangle;
	config.blend = GSHWDrawConfig::BlendState();
	config.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
	config.colormask = GSHWDrawConfig::ColorMaskSelector();
	config.colormask.wrgba = 0xf;
	config.require_one_barrier = false;
	config.require_full_barrier = false;
	config.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Off;
	config.datm = SetDATM::DATM0;
	config.line_expand = false;
	config.alpha_second_pass.enable = false;
	config.vs.key = 0;
	config.vs.tme = tex != nullptr;
	config.vs.iip = true;
	config.vs.fst = true;
	config.ps.key_lo = 0;
	config.ps.key_hi = 0;
	config.ps.tfx = tex ? TFX_DECAL : TFX_NONE;
	config.ps.iip = true;
	config.ps.fst = true;

	if (tex)
	{
		const GSVector2i texsize = tex->GetSize();
		config.cb_ps.WH = GSVector4(static_cast<float>(texsize.x) / tex_scale,
			static_cast<float>(texsize.y) / tex_scale, static_cast<float>(texsize.x), static_cast<float>(texsize.y));
		config.cb_ps.STScale = GSVector2(1.0f);
		config.cb_vs.texture_scale = GSVector2((1.0f / 16.0f) / config.cb_ps.WH.x, (1.0f / 16.0f) / config.cb_ps.WH.y);
	}

	const GSVector2i rtsize = rt_or_ds->GetSize();
	config.cb_vs.vertex_scale = GSVector2(2.0f * rt_scale / (rtsize.x << 4), 2.0f * rt_scale / (rtsize.y << 4));
	config.cb_vs.vertex_offset = GSVector2(-1.0f / rtsize.x + 1.0f, -1.0f / rtsize.y + 1.0f);

	return config;
}

void GSRendererHW::EndHLEHardwareDraw(bool force_copy_on_hazard )
{
	GSHWDrawConfig& config = m_conf;

	GL_PUSH("HW: HLE hardware draw in %d,%d => %d,%d", config.drawarea.left, config.drawarea.top, config.drawarea.right,
		config.drawarea.bottom);

	GSTexture* copy = nullptr;
	if (config.tex && (config.tex == config.rt || config.tex == config.ds))
	{
		const GSDevice::FeatureSupport features = g_gs_device->Features();

		if (!force_copy_on_hazard && config.tex == config.rt)
		{
			config.tex = nullptr;
			config.ps.tex_is_fb = true;
			config.require_one_barrier = !features.framebuffer_fetch;
		}
		else if (!force_copy_on_hazard && config.tex == config.ds && !config.depth.zwe &&
				 features.test_and_sample_depth)
		{
		}
		else
		{
			GSTexture* src = (config.tex == config.rt) ? config.rt : config.ds;
			copy = g_gs_device->CreateTexture(src->GetWidth(), src->GetHeight(), 1, src->GetFormat(), true);
			if (!copy)
			{
				Console.Error("HW: Texture allocation failure in EndHLEHardwareDraw()");
				return;
			}

			const GSVector4i copy_rect = config.drawarea.rintersect(src->GetRect());
			g_gs_device->CopyRect(src, copy, copy_rect - copy_rect.xyxy(), copy_rect.x, copy_rect.y);
			config.tex = copy;
		}
	}

	config.ps.no_color = !config.rt;
	config.ps.no_color1 = !config.rt || !config.blend.enable ||
	                      (!GSDevice::IsDualSourceBlendFactor(config.blend.src_factor) &&
	                       !GSDevice::IsDualSourceBlendFactor(config.blend.dst_factor));

	g_gs_device->RenderHW(m_conf);

	if (copy)
		g_gs_device->Recycle(copy);
}

std::size_t GSRendererHW::ComputeDrawlistGetSize(float scale)
{
	if (m_drawlist.empty())
	{
		const bool save_bbox = !g_gs_device->Features().texture_barrier && g_gs_device->Features().multidraw_fb_copy;
		GetPrimitiveOverlapDrawlist(true, save_bbox, scale);
	}
	return m_drawlist.size();
}

bool GSRendererHW::IsCoverageAlphaSupported()
{
	return IsCoverageAlpha() && IsRTWritten() && g_gs_device->Features().aa1;
}
