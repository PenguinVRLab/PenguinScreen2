// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "MemoryTypes.h"

struct EECNT_MODE
{
	u32 ClockSource:2;

	u32 EnableGate:1;

	u32 GateSource:1;

	u32 GateMode:2;

	u32 ZeroReturn:1;

	u32 IsCounting:1;

	u32 TargetInterrupt:1;

	u32 OverflowInterrupt:1;

	u32 TargetReached:1;

	u32 OverflowReached:1;
};

struct Counter
{
	u32 count;
	union
	{
		u32 modeval;
		EECNT_MODE mode;
	};
	u32 target, hold;
	u32 rate, interrupt;
	u64 startCycle;
};

struct SyncCounter
{
	u32 Mode;
	u64 startCycle;
	s32 deltaCycles;
};

#define HBLANK_COUNTER_SPEED	1

#define SCANLINES_TOTAL_1080	1125
#define FRAMERATE_NTSC			29.97

#define SCANLINES_TOTAL_NTSC_I	525
#define SCANLINES_TOTAL_NTSC_NI	526
#define SCANLINES_VSYNC_NTSC	3
#define SCANLINES_VRENDER_NTSC	240
#define SCANLINES_VBLANK1_NTSC	19
#define SCANLINES_VBLANK2_NTSC	20

#define FRAMERATE_PAL			25.0

#define SCANLINES_TOTAL_PAL_I	625
#define SCANLINES_TOTAL_PAL_NI	628
#define SCANLINES_VSYNC_PAL		5
#define SCANLINES_VRENDER_PAL	288
#define SCANLINES_VBLANK1_PAL	19
#define SCANLINES_VBLANK2_PAL	20

#define MODE_VRENDER	0x0
#define MODE_VBLANK		0x1
#define MODE_GSBLANK	0x2

#define MODE_HRENDER	0x0
#define MODE_HBLANK		0x1

extern const char* ReportVideoMode();
extern const char* ReportInterlaceMode();
extern Counter counters[4];
extern SyncCounter hsyncCounter;
extern SyncCounter vsyncCounter;

extern s32 nextDeltaCounter;
extern u64 nextStartCounter;
extern uint g_FrameCount;

extern void rcntUpdate_hScanline();
extern void rcntUpdate_vSync();
extern bool rcntCanCount(int i);
extern void rcntSyncCounter(int i);
extern void rcntUpdate();

extern void rcntInit();
extern u32	rcntRcount(int index);
template< uint page > extern bool rcntWrite32( u32 mem, mem32_t& value );
template< uint page > extern u16 rcntRead32( u32 mem );

extern void UpdateVSyncRate(bool force);

