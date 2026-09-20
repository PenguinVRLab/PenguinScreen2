// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Common.h"
#include "Gif.h"
#include "GS/GS.h"
#include "GS/GSRegs.h"

#include "common/SingleRegisterTypes.h"

extern double GetVerticalFrequency();
alignas(16) extern u8 g_RealGSMem[Ps2MemSize::GSregs];

enum CSR_FifoState
{
	CSR_FIFO_NORMAL = 0,
	CSR_FIFO_EMPTY,
	CSR_FIFO_FULL,
	CSR_FIFO_RESERVED
};

union tGS_CSR
{
	struct
	{
		u64 SIGNAL : 1;

		u64 FINISH : 1;

		u64 HSINT : 1;

		u64 VSINT : 1;

		u64 EDWINT : 1;

		u64 _zero1 : 1;
		u64 _zero2 : 1;
		u64 pad1 : 1;

		u64 FLUSH : 1;

		u64 RESET : 1;

		u64 _pad2 : 2;

		u64 NFIELD : 1;

		u64 FIELD : 1;

		u64 FIFO : 2;

		u64 REV : 8;

		u64 ID : 8;
	};

	u64 _u64;

	struct
	{
		u32 _u32;
		u32 _unused32;
	};

	void SwapField()
	{
		_u32 ^= 0x2000;
	}

	void SetField()
	{
		_u32 |= 0x2000;
	}

	void Reset()
	{
		FIFO = CSR_FIFO_EMPTY;
		REV = 0x1B;
		ID = 0x55;
	}

	bool HasAnyInterrupts() const { return (SIGNAL || FINISH || HSINT || VSINT || EDWINT); }

	u32 GetInterruptMask() const
	{
		return _u32 & 0x1f;
	}

	void SetAllInterrupts(bool value = true)
	{
		SIGNAL = FINISH = HSINT = VSINT = EDWINT = value;
	}

	tGS_CSR(u64 val) { _u64 = val; }
	tGS_CSR(u32 val) { _u32 = val; }
	tGS_CSR() { Reset(); }
};

union tGS_IMR
{
	struct
	{
		u32 _reserved1 : 8;
		u32 SIGMSK : 1;
		u32 FINISHMSK : 1;
		u32 HSMSK : 1;
		u32 VSMSK : 1;
		u32 EDWMSK : 1;
		u32 _undefined : 2;
		u32 _reserved2 : 17;
	};
	u32 _u32;

	void reset()
	{
		_u32 = 0;
		SIGMSK = FINISHMSK = HSMSK = VSMSK = EDWMSK = true;
		_undefined = 0x3;
	}
	void set(u32 value)
	{
		_u32 = (value & 0x1f00);
		_undefined = 0x3;
	}

	bool masked() const { return (SIGMSK || FINISHMSK || HSMSK || VSMSK || EDWMSK); }
};

struct GSRegSIGBLID
{
	u32 SIGID;
	u32 LBLID;
};

#define PS2MEM_GS g_RealGSMem
#define PS2GS_BASE(mem) (PS2MEM_GS + (mem & 0x13ff))

#define CSRreg ((tGS_CSR&)*(PS2MEM_GS + 0x1000))
#define GSSMODE1reg ((GSRegSMODE1&)*(PS2MEM_GS + 0x0010))
#define GSCSRr ((u32&)*(PS2MEM_GS + 0x1000))
#define GSIMR ((tGS_IMR&)*(PS2MEM_GS + 0x1010))
#define GSSIGLBLID ((GSRegSIGBLID&)*(PS2MEM_GS + 0x1080))

enum class GS_VideoMode : int
{
	Uninitialized,
	Unknown,
	NTSC,
	PAL,
	VESA,
	SDTV_480P,
	SDTV_576P,
	HDTV_720P,
	HDTV_1080I,
	HDTV_1080P,
	DVD_NTSC,
	DVD_PAL
};

extern GS_VideoMode gsVideoMode;
extern u32 lastCSRFlag;
extern bool gsIsInterlaced;

extern void gsReset();
extern void gsSetVideoMode(GS_VideoMode mode);
extern void gsPostVsyncStart();

extern void gsWrite8(u32 mem, u8 value);
extern void gsWrite16(u32 mem, u16 value);
extern void gsWrite32(u32 mem, u32 value);

extern void gsWrite64_page_00(u32 mem, u64 value);
extern void gsWrite64_page_01(u32 mem, u64 value);
extern void gsWrite64_generic(u32 mem, u64 value);

extern void TAKES_R128 gsWrite128_page_00(u32 mem, r128 value);
extern void TAKES_R128 gsWrite128_page_01(u32 mem, r128 value);
extern void TAKES_R128 gsWrite128_generic(u32 mem, r128 value);

extern u8 gsRead8(u32 mem);
extern u16 gsRead16(u32 mem);
extern u32 gsRead32(u32 mem);
extern u64 gsRead64(u32 mem);
extern u128 gsNonMirroredRead(u32 mem);

void gsIrq();

extern tGS_CSR CSRr;
