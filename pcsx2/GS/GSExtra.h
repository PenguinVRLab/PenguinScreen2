// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"
#include "pcsx2/Config.h"
#include "common/BitUtils.h"

#include <utility>

template <typename T>
__forceinline bool BitEqual(const T& a, const T& b)
{
#if _M_SSE >= 0x501
	if (alignof(T) >= 32)
	{
		GSVector8i eq = GSVector8i::xffffffff();
		for (size_t i = 0; i < sizeof(T) / 32; i++)
			eq &= reinterpret_cast<const GSVector8i*>(&a)[i].eq8(reinterpret_cast<const GSVector8i*>(&b)[i]);
		return eq.alltrue();
	}
#endif
	GSVector4i eq = GSVector4i::xffffffff();
	if (alignof(T) >= 16)
	{
		for (size_t i = 0; i < sizeof(T) / 16; i++)
			eq &= reinterpret_cast<const GSVector4i*>(&a)[i].eq8(reinterpret_cast<const GSVector4i*>(&b)[i]);
		return eq.alltrue();
	}
	const char* ac = reinterpret_cast<const char*>(&a);
	const char* bc = reinterpret_cast<const char*>(&b);
	size_t i = 0;
	if (sizeof(T) >= 16)
	{
		for (; i < sizeof(T) - 15; i += 16)
			eq &= GSVector4i::load<false>(ac + i).eq8(GSVector4i::load<false>(bc + i));
	}
	if (i + 8 <= sizeof(T))
	{
		eq &= GSVector4i::loadl(ac + i).eq8(GSVector4i::loadl(bc + i));
		i += 8;
	}
	bool eqb = eq.alltrue();
	if (i + 4 <= sizeof(T))
	{
		u32 ai, bi;
		memcpy(&ai, ac + i, sizeof(ai));
		memcpy(&bi, bc + i, sizeof(bi));
		eqb = ai == bi && eqb;
		i += 4;
	}
	if (i + 2 <= sizeof(T))
	{
		u16 as, bs;
		memcpy(&as, ac + i, sizeof(as));
		memcpy(&bs, bc + i, sizeof(bs));
		eqb = as == bs && eqb;
		i += 2;
	}
	if (i != sizeof(T))
	{
		pxAssert(i + 1 == sizeof(T));
		eqb = ac[i] == bc[i] && eqb;
	}
	return eqb;
}

extern Pcsx2Config::GSOptions GSConfig;

static constexpr u32 MAXIMUM_TEXTURE_HASH_CACHE_SIZE = 10;
__fi static bool CanCacheTextureSize(u32 tw, u32 th)
{
	return (GSConfig.TexturePreloading == TexturePreloadingLevel::Full &&
			tw <= MAXIMUM_TEXTURE_HASH_CACHE_SIZE && th <= MAXIMUM_TEXTURE_HASH_CACHE_SIZE);
}

__fi static bool CanPreloadTextureSize(u32 tw, u32 th)
{
	static constexpr u32 MAXIMUM_SIZE_IN_ONE_DIRECTION = 10;
	static constexpr u32 MAXIMUM_SIZE_IN_OTHER_DIRECTION = 8;
	static constexpr u32 MAXIMUM_SIZE_IN_BOTH_DIRECTIONS = 9;

	if (GSConfig.TexturePreloading < TexturePreloadingLevel::Partial)
		return false;

	const u32 max_dimension = (tw > th) ? tw : th;
	const u32 min_dimension = (tw > th) ? th : tw;
	if (max_dimension <= MAXIMUM_SIZE_IN_BOTH_DIRECTIONS)
		return true;

	return (max_dimension <= MAXIMUM_SIZE_IN_ONE_DIRECTION &&
			min_dimension <= MAXIMUM_SIZE_IN_OTHER_DIRECTION);
}

static constexpr int MAXIMUM_TEXTURE_MIPMAP_LEVELS = 7;

static constexpr u32 MAX_SKIPPED_DUPLICATE_FRAMES = 3;

extern void* GSAllocateWrappedMemory(size_t size, size_t repeat);
extern void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat);

static constexpr u32 VECTOR_ALIGNMENT = 32;

template<typename T>
__fi static T VectorAlign(T value)
{
	return Common::AlignUpPow2(value, VECTOR_ALIGNMENT);
}

std::pair<u8, u8> GSGetRGBA8AlphaMinMax(const void* data, u32 width, u32 height, u32 stride);

// clang-format off

#ifdef _MSC_VER
	#define ALIGN_STACK(n) alignas(n) int dummy__; (void)dummy__;
#else
	#ifdef __GNUC__
		#define ALIGN_STACK(n) (void)0;
	#else
		#define ALIGN_STACK(n) alignas(n) int dummy__;
	#endif
#endif
