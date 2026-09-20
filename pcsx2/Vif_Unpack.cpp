// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include "MTVU.h"

enum UnpackOffset {
	OFFSET_X = 0,
	OFFSET_Y = 1,
	OFFSET_Z = 2,
	OFFSET_W = 3
};

static __fi u32 setVifRow(vifStruct& vif, u32 reg, u32 data) {
	vif.MaskRow._u32[reg] = data;
	return data;
}

template< uint idx, uint mode, bool doMask >
static __ri void writeXYZW(u32 offnum, u32 &dest, u32 data) {
	int n = 0;

	vifStruct& vif = MTVU_VifX;

	if (doMask) {
		const VIFregisters& regs = MTVU_VifXRegs;
		switch (vif.cl) {
			case 0:  n = (regs.mask >> (offnum * 2)) & 0x3;		break;
			case 1:  n = (regs.mask >> ( 8 + (offnum * 2))) & 0x3;	break;
			case 2:  n = (regs.mask >> (16 + (offnum * 2))) & 0x3;	break;
			default: n = (regs.mask >> (24 + (offnum * 2))) & 0x3;	break;
		}
	}

	switch (n) {
		case 0:
			switch (mode) {
				case 1:  dest = data + vif.MaskRow._u32[offnum]; break;
				case 2:  dest = setVifRow(vif, offnum, vif.MaskRow._u32[offnum] + data); break;
				case 3:  dest = setVifRow(vif, offnum, data); break;
				default: dest = data; break;
			}
			break;
		case 1: dest = vif.MaskRow._u32[offnum]; break;
		case 2: dest = vif.MaskCol._u32[std::min(vif.cl,3)]; break;
		case 3: break;
	}
}
#define tParam idx,mode,doMask

template < uint idx, uint mode, bool doMask, class T >
static void UNPACK_S(u32* dest, const T* src)
{
	u32 data = *src;

	writeXYZW<tParam>(OFFSET_X, *(dest+0), data);
	writeXYZW<tParam>(OFFSET_Y, *(dest+1), data);
	writeXYZW<tParam>(OFFSET_Z, *(dest+2), data);
	writeXYZW<tParam>(OFFSET_W, *(dest+3), data);
}

template < uint idx, uint mode, bool doMask, class T >
static void UNPACK_V2(u32* dest, const T* src)
{
	writeXYZW<tParam>(OFFSET_X, *(dest+0), *(src+0));
	writeXYZW<tParam>(OFFSET_Y, *(dest+1), *(src+1));
	writeXYZW<tParam>(OFFSET_Z, *(dest+2), *(src+0));
	writeXYZW<tParam>(OFFSET_W, *(dest+3), *(src+1));
}

template < uint idx, uint mode, bool doMask, class T >
static void UNPACK_V4(u32* dest, const T* src)
{
	writeXYZW<tParam>(OFFSET_X, *(dest+0), *(src+0));
	writeXYZW<tParam>(OFFSET_Y, *(dest+1), *(src+1));
	writeXYZW<tParam>(OFFSET_Z, *(dest+2), *(src+2));
	writeXYZW<tParam>(OFFSET_W, *(dest+3), *(src+3));
}

template< uint idx, bool doMask >
static void UNPACK_V4_5(u32 *dest, const u32* src)
{
	u32 data = *src;

	writeXYZW<idx,0,doMask>(OFFSET_X, *(dest+0),	((data & 0x001f) << 3));
	writeXYZW<idx,0,doMask>(OFFSET_Y, *(dest+1),	((data & 0x03e0) >> 2));
	writeXYZW<idx,0,doMask>(OFFSET_Z, *(dest+2),	((data & 0x7c00) >> 7));
	writeXYZW<idx,0,doMask>(OFFSET_W, *(dest+3),	((data & 0x8000) >> 8));
}

static void UNPACK_INVALID(u32* dest, const u32* src)
{
	Console.Warning("Vpu/Vif: Invalid Unpack");
}

#define _upk              (UNPACKFUNCTYPE)
#define _unpk(usn, bits)  (UNPACKFUNCTYPE_##usn##bits)
#define _unpk_invalid     (UNPACKFUNCTYPE)UNPACK_INVALID

#define UnpackFuncSet( vt, idx, mode, usn, doMask ) \
	(UNPACKFUNCTYPE)_unpk(u,32)		UNPACK_##vt<idx, mode, doMask, u32>, \
	(UNPACKFUNCTYPE)_unpk(usn,16)	UNPACK_##vt<idx, mode, doMask, usn##16>, \
	(UNPACKFUNCTYPE)_unpk(usn,8)	UNPACK_##vt<idx, mode, doMask, usn##8> \

#define UnpackV4_5set(idx, doMask) \
	(UNPACKFUNCTYPE)_unpk(u,32) UNPACK_V4_5<idx, doMask> \

#define UnpackModeSet(idx, mode) \
	UnpackFuncSet( S,  idx, mode, s, 0 ), _unpk_invalid,  \
	UnpackFuncSet( V2, idx, mode, s, 0 ), _unpk_invalid,  \
	UnpackFuncSet( V4, idx, mode, s, 0 ), _unpk_invalid,  \
	UnpackFuncSet( V4, idx, mode, s, 0 ), UnpackV4_5set(idx, 0), \
 \
	UnpackFuncSet( S,  idx, mode, s, 1 ), _unpk_invalid,  \
	UnpackFuncSet( V2, idx, mode, s, 1 ), _unpk_invalid,  \
	UnpackFuncSet( V4, idx, mode, s, 1 ), _unpk_invalid,  \
	UnpackFuncSet( V4, idx, mode, s, 1 ), UnpackV4_5set(idx, 1), \
 \
	UnpackFuncSet( S,  idx, mode, u, 0 ), _unpk_invalid,  \
	UnpackFuncSet( V2, idx, mode, u, 0 ), _unpk_invalid,  \
	UnpackFuncSet( V4, idx, mode, u, 0 ), _unpk_invalid,  \
	UnpackFuncSet( V4, idx, mode, u, 0 ), UnpackV4_5set(idx, 0), \
 \
	UnpackFuncSet( S,  idx, mode, u, 1 ), _unpk_invalid,  \
	UnpackFuncSet( V2, idx, mode, u, 1 ), _unpk_invalid,  \
	UnpackFuncSet( V4, idx, mode, u, 1 ), _unpk_invalid,  \
	UnpackFuncSet( V4, idx, mode, u, 1 ), UnpackV4_5set(idx, 1)

alignas(16) const UNPACKFUNCTYPE VIFfuncTable[2][4][4 * 4 * 2 * 2] =
{
	{
		{ UnpackModeSet(0,0) },
		{ UnpackModeSet(0,1) },
		{ UnpackModeSet(0,2) },
		{ UnpackModeSet(0,3) }
	},

	{
		{ UnpackModeSet(1,0) },
		{ UnpackModeSet(1,1) },
		{ UnpackModeSet(1,2) },
		{ UnpackModeSet(1,3) }
	}
};

_vifT void vifUnpackSetup(const u32 *data) {

	vifStruct& vifX = GetVifX;

	GetVifX.unpackcalls++;

	if (GetVifX.unpackcalls > 3)
	{
		vifExecQueue(idx);
	}

	vifX.usn   = (vifXRegs.code >> 14) & 0x01;
	int vifNum = (vifXRegs.code >> 16) & 0xff;

	if (vifNum == 0) vifNum = 256;
	vifXRegs.num =  vifNum;

	const u8& gsize = nVifT[vifX.cmd & 0x0f];

	uint wl = vifXRegs.cycle.wl ? vifXRegs.cycle.wl : 256;

	if (wl <= vifXRegs.cycle.cl) {
		vifX.tag.size = ((vifNum * gsize) + 3) / 4;
	}
	else {
		int n = vifXRegs.cycle.cl * (vifNum / wl) +
		        _limit(vifNum % wl, vifXRegs.cycle.cl);

		vifX.tag.size = ((n * gsize) + 3) >> 2;
	}

	u32 addr = vifXRegs.code;
	if (idx && ((addr>>15)&1)) addr += vif1Regs.tops;
	vifX.tag.addr = (addr<<4) & (idx ? 0x3ff0 : 0xff0);

	VIF_LOG("Unpack VIF%x, QWC %x tagsize %x", idx, vifNum, vifX.tag.size);

	vifX.cl			 = 0;
	vifX.tag.cmd	 = vifX.cmd;
	GetVifX.pass	 = 1;

	vifX.start_aligned = 4-((vifX.vifpacketsize-1) & 0x3);
}

template void vifUnpackSetup<0>(const u32 *data);
template void vifUnpackSetup<1>(const u32 *data);

alignas(16) nVifStruct nVif[2];

alignas(16) nVifCall nVifUpk[(2 * 2 * 16) * 4];

alignas(16) u32 nVifMask[3][4][4] = {};

alignas(16) const u8 nVifT[16] = {
	4,
	2,
	1,
	0,
	8,
	4,
	2,
	0,
	12,
	6,
	3,
	0,
	16,
	8,
	4,
	2,
};

template <int idx, bool doMode, bool isFill>
__ri void _nVifUnpackLoop(const u8* data);

typedef void FnType_VifUnpackLoop(const u8* data);
typedef FnType_VifUnpackLoop* Fnptr_VifUnpackLoop;

alignas(16) static const Fnptr_VifUnpackLoop UnpackLoopTable[2][2][2] = {
	{
		{_nVifUnpackLoop<0, 0, 0>, _nVifUnpackLoop<0, 0, 1>},
		{_nVifUnpackLoop<0, 1, 0>, _nVifUnpackLoop<0, 1, 1>},
	},
	{
		{_nVifUnpackLoop<1, 0, 0>, _nVifUnpackLoop<1, 0, 1>},
		{_nVifUnpackLoop<1, 1, 0>, _nVifUnpackLoop<1, 1, 1>},
	},
};

void resetNewVif(int idx)
{

	nVif[idx].idx   = idx;
	nVif[idx].bSize = 0;
	std::memset(nVif[idx].buffer, 0, sizeof(nVif[idx].buffer));

	if (newVifDynaRec)
		dVifReset(idx);
}

void releaseNewVif(int idx)
{
}

static __fi u8* getVUptr(uint idx, int offset)
{
	return (u8*)(vuRegs[idx].Mem + (offset & (idx ? 0x3ff0 : 0xff0)));
}


_vifT int nVifUnpack(const u8* data)
{
	nVifStruct&   v       = nVif[idx];
	vifStruct&    vif     = GetVifX;
	VIFregisters& vifRegs = vifXRegs;

	const uint wl     = vifRegs.cycle.wl ? vifRegs.cycle.wl : 256;
	const uint ret    = std::min(vif.vifpacketsize, vif.tag.size);
	const bool isFill = (vifRegs.cycle.cl < wl);
	s32        size   = ret << 2;

	if (ret == vif.tag.size)
	{
		if (v.bSize)
		{
			memcpy(&v.buffer[v.bSize], data, size);
			v.bSize += size;
			size = v.bSize;
			data = v.buffer;

			vif.cl = 0;
			vifRegs.num = (vifXRegs.code >> 16) & 0xff;
			if (!vifRegs.num)
				vifRegs.num = 256;
		}

		if (!idx || !THREAD_VU1)
		{
			if (newVifDynaRec)
				dVifUnpack<idx>(data, isFill);
			else
				_nVifUnpack(idx, data, vifRegs.mode, isFill);
		}
		else
			vu1Thread.VifUnpack(vif, vifRegs, (u8*)data, (size + 4) & ~0x3);

		vif.pass     = 0;
		vif.tag.size = 0;
		vif.cmd      = 0;
		vifRegs.num  = 0;
		v.bSize      = 0;
	}
	else
	{
		memcpy(&v.buffer[v.bSize], data, size);
		v.bSize += size;
		vif.tag.size -= ret;

		const u8& vSize = nVifT[vif.cmd & 0x0f];

		if (!isFill)
		{
			vifRegs.num -= (size / vSize);
		}
		else
		{
			int dataSize = (size / vSize);
			vifRegs.num = vifRegs.num - (((dataSize / vifRegs.cycle.cl) * (vifRegs.cycle.wl - vifRegs.cycle.cl)) + dataSize);
		}
	}

	return ret;
}

template int nVifUnpack<0>(const u8* data);
template int nVifUnpack<1>(const u8* data);

static void setMasks(const vifStruct& vif, const VIFregisters& v)
{
	for (int i = 0; i < 16; i++)
	{
		int m = (v.mask >> (i * 2)) & 3;
		switch (m)
		{
			case 0:
				nVifMask[0][i / 4][i % 4] = 0xffffffff;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = 0;
				break;
			case 1:
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = vif.MaskRow._u32[i % 4];
				break;
			case 2:
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0;
				nVifMask[2][i / 4][i % 4] = vif.MaskCol._u32[i / 4];
				break;
			case 3:
				nVifMask[0][i / 4][i % 4] = 0;
				nVifMask[1][i / 4][i % 4] = 0xffffffff;
				nVifMask[2][i / 4][i % 4] = 0;
				break;
		}
	}
}

template <int idx, bool doMode, bool isFill>
__ri void _nVifUnpackLoop(const u8* data)
{

	vifStruct& vif = MTVU_VifX;
	VIFregisters& vifRegs = MTVU_VifXRegs;

	const int skipSize = (vifRegs.cycle.cl - vifRegs.cycle.wl) * 16;

	if (!doMode && (vif.cmd & 0x10))
		setMasks(vif, vifRegs);

	const int usn    = !!vif.usn;
	const int upkNum = vif.cmd & 0x1f;
	const u8& vSize  = nVifT[upkNum & 0x0f];

	const nVifCall* fnbase = &nVifUpk[((usn * 2 * 16) + upkNum) * (4 * 1)];
	const UNPACKFUNCTYPE ft = VIFfuncTable[idx][doMode ? vifRegs.mode : 0][((usn * 2 * 16) + upkNum)];

	pxAssume(vif.cl == 0);

	do
	{
		u8* dest = getVUptr(idx, vif.tag.addr);

		if (doMode)
		{
			ft(dest, data);
		}
		else
		{
			uint cl3 = std::min(vif.cl, 3);
			fnbase[cl3](dest, data);
		}

		vif.tag.addr += 16;
		--vifRegs.num;
		++vif.cl;

		if (isFill)
		{
			if (vif.cl <= vifRegs.cycle.cl)
				data += vSize;
			else if (vif.cl == vifRegs.cycle.wl)
				vif.cl = 0;
		}
		else
		{
			data += vSize;

			if (vif.cl >= vifRegs.cycle.wl)
			{
				vif.tag.addr += skipSize;
				vif.cl = 0;
			}
		}
	} while (vifRegs.num);
}

__fi void _nVifUnpack(int idx, const u8* data, uint mode, bool isFill)
{
	UnpackLoopTable[idx][!!mode][isFill](data);
}
