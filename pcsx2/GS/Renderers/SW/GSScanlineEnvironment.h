// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"
#include "GS/GSVector.h"

#include <cstdio>
#include <string>

union GSScanlineSelector
{
	struct
	{
		u32 fpsm  : 2;
		u32 zpsm  : 2;
		u32 ztst  : 2;
		u32 atst  : 3;
		u32 afail : 2;
		u32 iip   : 1;
		u32 tfx   : 3;
		u32 tcc   : 1;
		u32 fst   : 1;
		u32 ltf   : 1;
		u32 tlu   : 1;
		u32 fge   : 1;
		u32 date  : 1;
		u32 abe   : 1;
		u32 aba   : 2;
		u32 abb   : 2;
		u32 abc   : 2;
		u32 abd   : 2;
		u32 pabe  : 1;
		u32 aa1   : 1;

		u32 fwrite    : 1;
		u32 ftest     : 1;
		u32 rfb       : 1;
		u32 zwrite    : 1;
		u32 ztest     : 1;
		u32 zoverflow : 1;
		u32 zclamp    : 1;
		u32 wms       : 2;
		u32 wmt       : 2;
		u32 datm      : 1;
		u32 colclamp  : 1;
		u32 fba       : 1;
		u32 dthe      : 1;
		u32 prim      : 2;

		u32 edge   : 1;
		u32 tw     : 3;
		u32 lcm    : 1;
		u32 mmin   : 2;
		u32 notest : 1;
		u32 zequal : 1;
		u32 breakpoint : 1;
	};

	struct
	{
		u32 _pad1  : 22;
		u32 ababcd :  8;
		u32 _pad2  :  2;

		u32 fb    : 2;
		u32 _pad3 : 1;
		u32 zb    : 2;
	};

	struct
	{
		u32 lo;
		u32 hi;
	};

	u64 key;

	GSScanlineSelector() = default;
	GSScanlineSelector(u64 k)
		: key(k)
	{
	}

	operator u32() const { return lo; }
	operator u64() const { return key; }

	bool IsSolidRect() const
	{
		return prim == GS_SPRITE_CLASS && iip == 0 && tfx == TFX_NONE && abe == 0 && ztst <= 1 && atst <= 1 && date == 0 && fge == 0;
	}

	std::string to_string() const
	{
		char str[1024];
		std::snprintf(str, std::size(str),
			"fpsm:%d zpsm:%d ztst:%d ztest:%d atst:%d afail:%d iip:%d rfb:%d fb:%d zb:%d zw:%d "
			"tfx:%d tcc:%d fst:%d ltf:%d tlu:%d wms:%d wmt:%d mmin:%d lcm:%d tw:%d "
			"fba:%d cclamp:%d date:%d datm:%d "
			"prim:%d abe:%d %d%d%d%d fge:%d dthe:%d notest:%d pabe:%d aa1:%d "
			"fwrite:%d ftest:%d zoverflow:%d zequal:%d zclamp:%d edge:%d",
			fpsm, zpsm, ztst, ztest, atst, afail, iip, rfb, fb, zb, zwrite,
			tfx, tcc, fst, ltf, tlu, wms, wmt, mmin, lcm, tw,
			fba, colclamp, date, datm,
			prim, abe, aba, abb, abc, abd, fge, dthe, notest, pabe, aa1,
			fwrite, ftest, zoverflow, zequal, zclamp, edge);
		return str;
	}

	void Print() const
	{
		fprintf(stderr, "%s\n", to_string().c_str());
	}
};

struct alignas(32) GSScanlineGlobalData
{
	GSScanlineSelector sel;

	void* vm;
	const void* tex[7];
	u32* clut;
	GSVector4i* dimx;

	GSOffset fbo;
	GSOffset zbo;
	const GSVector2i* fzbr;
	const GSVector2i* fzbc;

	GSVector4i aref;
	GSVector4i afix;
	struct { GSVector4i min, max, minmax, mask, invmask; } t;

#if _M_SSE >= 0x501

	u32 fm, zm;
	u32 frb, fga;
	GSVector8 mxl;
	GSVector8 k;
	GSVector8 l;
	struct { GSVector8i i, f; } lod;

#else

	GSVector4i fm, zm;
	GSVector4i frb, fga;
	GSVector4 mxl;
	GSVector4 k;
	GSVector4 l;
	struct { GSVector4i i, f; } lod;

#endif

#ifdef ARCH_ARM64
	alignas(16) u32 const_test_128b[8][4] = {
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0xffffffff, 0x00000000},
		{0x00000000, 0xffffffff, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
	};
	alignas(16) u16 const_movemaskw_mask[8] = {0x3, 0xc, 0x30, 0xc0, 0x300, 0xc00, 0x3000, 0xc000};
	alignas(16) float const_log2_coef[4] = {
		0.204446009836232697516f,
		-1.04913055217340124191f,
		2.28330284476918490682f,
		1.0f};
#endif
};

struct alignas(32) GSScanlineLocalData
{
#if _M_SSE >= 0x501

	struct skip { GSVector8 z, s, t, q; GSVector8i rb, ga, f, _pad; } d[8];
	struct step { GSVector4 stq; struct { u32 rb, ga; } c; struct { u64 z; u32 f; } p; } d8;
	struct { u32 z, f; } p;
	struct { GSVector8i rb, ga; } c;

	struct
	{
		GSVector8 z0, z1;
		GSVector8i f;
		GSVector8 s, t, q;
		GSVector8i rb, ga;
		GSVector8i zs, zd;
		GSVector8i uf, vf;
		GSVector8i cov;

		struct { GSVector8i i, f; } lod;
		GSVector8i uv[2];
		GSVector8i uv_minmax[2];
		GSVector8i trb, tga;
		GSVector8i test;
	} temp;

#else

	struct skip { GSVector4 z, s, t, q; GSVector4i rb, ga, f, _pad; } d[4];
	struct step { GSVector4 z, stq; GSVector4i c, f; } d4;
	struct { GSVector4i rb, ga; } c;
	struct { GSVector4i z, f; } p;

	struct
	{
		GSVector4 z0, z1;
		GSVector4i f;
		GSVector4 s, t, q;
		GSVector4i rb, ga;
		GSVector4i zs, zd;
		GSVector4i uf, vf;
		GSVector4i cov;

		struct { GSVector4i i, f; } lod;
		GSVector4i uv[2];
		GSVector4i uv_minmax[2];
		GSVector4i trb, tga;
		GSVector4i test;
	} temp;

#endif

	const GSScanlineGlobalData* gd;
};

namespace GSScanlineConstantData
{
	static constexpr float log2_coef[] = {
		0.204446009836232697516f,
		-1.04913055217340124191f,
		2.28330284476918490682f,
		1.0f
	};
};

struct alignas(64) GSScanlineConstantData256B
{
	alignas(32) u8 m_test[24] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	float m_log2_coef[4] = {};
	alignas(64) float m_shift[16] = {
		8.0f, -7.0f, -6.0f, -5.0f, -4.0f, -3.0f, -2.0f, -1.0f,
		0.0f,  1.0f,  2.0f,  3.0f,  4.0f,  5.0f,  6.0f,  7.0f,
	};

	constexpr GSScanlineConstantData256B()
	{
		using namespace GSScanlineConstantData;
		for (size_t n = 0; n < std::size(log2_coef); ++n)
		{
			m_log2_coef[n] = log2_coef[n];
		}
	}
};

struct alignas(64) GSScanlineConstantData128B
{
	alignas(16) u32 m_test[8][4] = {
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0xffffffff, 0x00000000},
		{0x00000000, 0xffffffff, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
	};
	alignas(16) float m_shift[5][4] = {
		{ 4.0f  , 4.0f  , 4.0f  , 4.0f},
		{ 0.0f  , 1.0f  , 2.0f  , 3.0f},
		{ -1.0f , 0.0f  , 1.0f  , 2.0f},
		{ -2.0f , -1.0f , 0.0f  , 1.0f},
		{ -3.0f , -2.0f , -1.0f , 0.0f},
	};
	alignas(16) float m_log2_coef[4][4] = {};

	constexpr GSScanlineConstantData128B()
	{
		using namespace GSScanlineConstantData;
		for (size_t n = 0; n < std::size(log2_coef); ++n)
		{
			for (size_t i = 0; i < 4; ++i)
				m_log2_coef[n][i] = log2_coef[n];
		}
	}
};

extern const GSScanlineConstantData256B g_const_256b;
extern const GSScanlineConstantData128B g_const_128b;
