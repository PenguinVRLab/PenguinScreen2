// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "IPU/IPU.h"
#include "IPU/mpeg2_vlc.h"
#include "GS/MultiISA.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef _MSC_VER
#define BigEndian(in) _byteswap_ulong(in)
#else
#define BigEndian(in) __builtin_bswap32(in)
#endif

#ifdef _MSC_VER
#define BigEndian64(in) _byteswap_uint64(in)
#else
#define BigEndian64(in) __builtin_bswap64(in)
#endif

struct macroblock_8{
	u8 Y[16][16];
	u8 Cb[8][8];
	u8 Cr[8][8];
};

struct macroblock_16{
	s16 Y[16][16];
	s16 Cb[8][8];
	s16 Cr[8][8];
};

struct macroblock_rgb32{
	struct {
		u8 r, g, b, a;
	} c[16][16];
};

struct rgb16_t{
	u16 r:5, g:5, b:5, a:1;
};

struct macroblock_rgb16{
	rgb16_t	c[16][16];
};

struct decoder_t {

	s16 DCTblock[64];

	u8 niq[64];
	u8 iq[64];

	macroblock_8 mb8;
	macroblock_16 mb16;
	macroblock_rgb32 rgb32;
	macroblock_rgb16 rgb16;

	uint ipu0_data;
	uint ipu0_idx;

	int quantizer_scale;

	int coding_type;

	s16 dc_dct_pred[3];

	int intra_dc_precision;
	int picture_structure;
	int frame_pred_frame_dct;
	int concealment_motion_vectors;
	int q_scale_type;
	int intra_vlc_format;
	int top_field_first;
	int sgn;
	int dte;
	int ofm;
	int macroblock_modes;
	int dcr;
	int coded_block_pattern;

	bool scantype;

	int mpeg1;

	template< typename T >
	void SetOutputTo( T& obj )
	{
		uint mb_offset = ((uptr)&obj - (uptr)&mb8);
		pxAssume( (mb_offset & 15) == 0 );
		ipu0_idx	= mb_offset / 16;
		ipu0_data	= sizeof(obj)/16;
	}

	u128* GetIpuDataPtr()
	{
		return ((u128*)&mb8) + ipu0_idx;
	}

	void AdvanceIpuDataBy(uint amt)
	{
		pxAssertMsg(ipu0_data>=amt, "IPU FIFO Overflow on advance!" );
		ipu0_idx  += amt;
		ipu0_data -= amt;
	}
};

alignas(16) extern decoder_t decoder;
alignas(16) extern tIPU_BP g_BP;

MULTI_ISA_DEF(
	extern void ipu_dither(const macroblock_rgb32& rgb32, macroblock_rgb16& rgb16, int dte);

	void IPUWorker();
)

extern rgb16_t g_ipu_vqclut[16];
extern u16 g_ipu_thresh[2];

alignas(16) extern u8 g_ipu_indx4[16*16/2];
alignas(16) extern const int non_linear_quantizer_scale[32];
extern int coded_block_pattern;

struct mpeg2_scan_pack
{
	u8 norm[64];
	u8 alt[64];
};

alignas(16) extern const std::array<u8, 1024> g_idct_clip_lut;
alignas(16) extern const mpeg2_scan_pack mpeg2_scan;
