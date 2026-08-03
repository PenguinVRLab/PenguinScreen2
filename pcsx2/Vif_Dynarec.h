// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Vif.h"
#include "Vif_HashBucket.h"
#include "VU.h"

typedef u32 (*nVifCall)(void*, const void*);
typedef void (*nVifrecCall)(uptr dest, uptr src);

extern void _nVifUnpack(int idx, const u8* data, uint mode, bool isFill);
extern void dVifReset(int idx);
extern void dVifRelease(int idx);
extern void VifUnpackSSE_Init();

_vifT extern void dVifUnpack(const u8* data, bool isFill);

struct nVifStruct
{
	alignas(16) u8 buffer[256*16];
	u32            bSize;

	u32                     idx;

	u8*                     recWritePtr;
	u8*                     recEndPtr;

	HashBucket              vifBlocks;


	nVifStruct() = default;
};

extern void resetNewVif(int idx);

alignas(16) extern nVifStruct nVif[2];
alignas(16) extern nVifCall nVifUpk[(2 * 2 * 16) * 4];
alignas(16) extern u32      nVifMask[3][4][4];

static constexpr bool newVifDynaRec = 1;
