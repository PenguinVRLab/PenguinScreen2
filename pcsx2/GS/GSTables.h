// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Assertions.h"

struct alignas(64) GSBlockSwizzleTable
{
	u8 value[8][8];

	constexpr u8 lookup(int x, int y) const
	{
		return value[y & 7][x & 7];
	}
};

template <int Height, int Width>
struct GSSizedBlockSwizzleTable : public GSBlockSwizzleTable
{
};

template <int Height>
struct alignas(128) GSPixelColOffsetTable
{
	int value[Height] = {};

	int operator[](int y) const
	{
		return value[y % Height];
	}
};

struct alignas(128) GSPixelRowOffsetTable
{
	int value[4096] = {};

	int operator[](size_t x) const
	{
		pxAssert(x < 4096);
		return value[x];
	}
};

template <int PageWidth>
struct GSSizedPixelRowOffsetTable : public GSPixelRowOffsetTable
{
};

template <int PageWidth, int Mask>
struct alignas(sizeof(void*) * 8) GSPixelRowOffsetTableList
{
	const GSPixelRowOffsetTable* rows[8];

	const GSPixelRowOffsetTable& operator[](int y) const
	{
		return *rows[y & Mask];
	}
};

template <int PageHeight, int PageWidth, int BlockHeight, int BlockWidth, int RowMask>
struct GSSwizzleTableList
{
	const GSSizedBlockSwizzleTable<BlockHeight, BlockWidth>& block;
	const GSPixelColOffsetTable<PageHeight>& col;
	const GSPixelRowOffsetTableList<PageWidth, RowMask>& row;
};

template <int PageHeight, int PageWidth, int BlockHeight, int BlockWidth, int RowMask>
constexpr GSSwizzleTableList<PageHeight, PageWidth, BlockHeight, BlockWidth, RowMask>
makeSwizzleTableList(
	const GSSizedBlockSwizzleTable<BlockHeight, BlockWidth>& block,
	const GSPixelColOffsetTable<PageHeight>& col,
	const GSPixelRowOffsetTableList<PageWidth, RowMask>& row)
{
	return {block, col, row};
}

extern const GSSizedBlockSwizzleTable<4, 8> blockTable32;
extern const GSSizedBlockSwizzleTable<8, 4> blockTable16;
extern const GSSizedBlockSwizzleTable<8, 4> blockTable16S;
extern const GSSizedBlockSwizzleTable<4, 8> blockTable8;
extern const GSSizedBlockSwizzleTable<8, 4> blockTable4;
extern const u8 columnTable32[8][8];
extern const u8 columnTable16[8][16];
extern const u8 columnTable8[16][16];
extern const u16 columnTable4[16][32];
extern const u8 clutTableT32I8[128];
extern const u8 clutTableT32I4[16];
extern const u8 clutTableT16I8[32];
extern const u8 clutTableT16I4[16];
extern const GSPixelColOffsetTable< 32> pixelColOffset32;
extern const GSPixelColOffsetTable< 64> pixelColOffset16;
extern const GSPixelColOffsetTable< 64> pixelColOffset16S;
extern const GSPixelColOffsetTable< 64> pixelColOffset8;
extern const GSPixelColOffsetTable<128> pixelColOffset4;

template <int PageWidth>
constexpr GSPixelRowOffsetTableList<PageWidth, 0> makeRowOffsetTableList(
	const GSSizedPixelRowOffsetTable<PageWidth>* a)
{
	return {{a, a, a, a, a, a, a, a}};
}

template <int PageWidth>
constexpr GSPixelRowOffsetTableList<PageWidth, 7> makeRowOffsetTableList(
	const GSSizedPixelRowOffsetTable<PageWidth>* a,
	const GSSizedPixelRowOffsetTable<PageWidth>* b)
{
	return {{a, a, b, b, b, b, a, a}};
}

struct GSTables
{
	static const GSSizedPixelRowOffsetTable< 64> _pixelRowOffset32;
	static const GSSizedPixelRowOffsetTable< 64> _pixelRowOffset16;
	static const GSSizedPixelRowOffsetTable< 64> _pixelRowOffset16S;
	static const GSSizedPixelRowOffsetTable<128> _pixelRowOffset8[2];
	static const GSSizedPixelRowOffsetTable<128> _pixelRowOffset4[2];

	static constexpr auto pixelRowOffset32   = makeRowOffsetTableList(&_pixelRowOffset32);
	static constexpr auto pixelRowOffset16   = makeRowOffsetTableList(&_pixelRowOffset16);
	static constexpr auto pixelRowOffset16S  = makeRowOffsetTableList(&_pixelRowOffset16S);
	static constexpr auto pixelRowOffset8 = makeRowOffsetTableList(&_pixelRowOffset8[0], &_pixelRowOffset8[1]);
	static constexpr auto pixelRowOffset4 = makeRowOffsetTableList(&_pixelRowOffset4[0], &_pixelRowOffset4[1]);
};

constexpr auto swizzleTables32   = makeSwizzleTableList(blockTable32,   pixelColOffset32,   GSTables::pixelRowOffset32  );
constexpr auto swizzleTables16   = makeSwizzleTableList(blockTable16,   pixelColOffset16,   GSTables::pixelRowOffset16  );
constexpr auto swizzleTables16S  = makeSwizzleTableList(blockTable16S,  pixelColOffset16S,  GSTables::pixelRowOffset16S );
constexpr auto swizzleTables8    = makeSwizzleTableList(blockTable8,    pixelColOffset8,    GSTables::pixelRowOffset8   );
constexpr auto swizzleTables4    = makeSwizzleTableList(blockTable4,    pixelColOffset4,    GSTables::pixelRowOffset4   );
