// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SPU2/defs.h"
#include "SPU2/spu2.h"
#include "IopMem.h"

#include <cstring>

namespace SPU2Savestate
{
	static constexpr u32 SAVE_ID = 0x1227521;

	static constexpr u32 SAVE_VERSION = 0x000e;

	static void wipe_the_cache()
	{
		memset(pcm_cache_data, 0, pcm_BlockCount * sizeof(PcmCacheEntry));
	}
}

struct SPU2Savestate::DataBlock
{
	u32 spu2id;
	u8 unkregs[0x10000];
	u8 mem[0x200000];

	u32 version;
	V_Core Cores[2];
	V_SPDIF Spdif;
	u16 OutPos;
	u16 InputPos;
	u32 Cycles;
	u64 lClocks;
	int PlayMode;
};

s32 SPU2Savestate::FreezeIt(DataBlock& spud)
{
	spud.spu2id = SAVE_ID;
	spud.version = SAVE_VERSION;

	memcpy(spud.unkregs, spu2regs, sizeof(spud.unkregs));
	memcpy(spud.mem, _spu2mem, sizeof(spud.mem));

	memcpy(spud.Cores, Cores, sizeof(Cores));
	memcpy(&spud.Spdif, &Spdif, sizeof(Spdif));

#define FIX_POINTER(x) \
	if (!(x)) \
	{ \
		x = reinterpret_cast<decltype(x)>(-1); \
	} \
	else \
	{ \
		pxAssert(reinterpret_cast<const u8*>((x)) >= iopPhysMem(0) && reinterpret_cast<const u8*>((x)) < iopPhysMem(0x1fffff)); \
		x = reinterpret_cast<decltype(x)>(reinterpret_cast<const u8*>((x)) - iopPhysMem(0)); \
	}

	for (u32 i = 0; i < 2; i++)
	{
		V_Core& core = spud.Cores[i];
		FIX_POINTER(core.DMAPtr);
		FIX_POINTER(core.DMARPtr);
	}

#undef FIX_POINTER

	spud.OutPos = OutPos;
	spud.InputPos = InputPos;
	spud.Cycles = Cycles;
	spud.lClocks = lClocks;
	spud.PlayMode = PlayMode;

	return 0;
}

s32 SPU2Savestate::ThawIt(DataBlock& spud)
{
	if (spud.spu2id != SAVE_ID || spud.version < SAVE_VERSION)
	{
		fprintf(stderr, "\n*** SPU2 Warning:\n");
		if (spud.spu2id == SAVE_ID)
			fprintf(stderr, "\tSavestate version is from an older version of PCSX2.\n");
		else
			fprintf(stderr, "\tThe savestate you are trying to load is incorrect or corrupted.\n");

		fprintf(stderr,
				"\tAudio may not recover correctly.  Save your game to memorycard, reset,\n\n"
				"\tand then continue from there.\n\n");

		wipe_the_cache();
	}
	else
	{
		memcpy(spu2regs, spud.unkregs, sizeof(spud.unkregs));
		memcpy(_spu2mem, spud.mem, sizeof(spud.mem));

		memcpy(Cores, spud.Cores, sizeof(Cores));
		memcpy(&Spdif, &spud.Spdif, sizeof(Spdif));

#define FIX_POINTER(x) \
	if ((x) == reinterpret_cast<decltype(x)>(-1)) \
	{ \
		x = nullptr; \
	} \
	else \
	{ \
		pxAssert(reinterpret_cast<size_t>((x)) <= 0x1fffff); \
		x = reinterpret_cast<decltype(x)>(iopPhysMem(0) + reinterpret_cast<size_t>((x))); \
	}

		for (u32 i = 0; i < 2; i++)
		{
			V_Core& core = Cores[i];
			FIX_POINTER(core.DMAPtr);
			FIX_POINTER(core.DMARPtr);
		}

#undef FIX_POINTER

		OutPos = spud.OutPos;
		InputPos = spud.InputPos;
		Cycles = spud.Cycles;
		lClocks = spud.lClocks;
		PlayMode = spud.PlayMode;

		wipe_the_cache();

		for (int c = 0; c < 2; c++)
		{
			for (int v = 0; v < 24; v++)
			{
				const int cacheIdx = Cores[c].Voices[v].NextA / pcm_WordsPerBlock;
				Cores[c].Voices[v].SBuffer = pcm_cache_data[cacheIdx].Sampledata;
			}
		}
	}
	return 0;
}

s32 SPU2Savestate::SizeIt()
{
	return sizeof(DataBlock);
}
