// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

union GPRRegs {
	struct {
		u32 r0, at, v0, v1, a0, a1, a2, a3,
			t0, t1, t2, t3, t4, t5, t6, t7,
			s0, s1, s2, s3, s4, s5, s6, s7,
			t8, t9, k0, k1, gp, sp, s8, ra, hi, lo;
	} n;
	u32 r[34];
};

union CP0Regs {
	struct {
		u32 Index,     Random,    EntryLo0,  EntryLo1,
			Context,   PageMask,  Wired,     Reserved0,
			BadVAddr,  Count,     EntryHi,   Compare,
			Status,    Cause,     EPC,       PRid,
			Config,    LLAddr,    WatchLO,   WatchHI,
			XContext,  Reserved1, Reserved2, Reserved3,
			Reserved4, Reserved5, ECC,       CacheErr,
			TagLo,     TagHi,     ErrorEPC,  Reserved6;
	} n;
	u32 r[32];
};

struct SVector2D {
	short x, y;
};

struct SVector2Dz {
	short z, pad;
};

struct SVector3D {
	short x, y, z, pad;
};

struct LVector3D {
	short x, y, z, pad;
};

struct CBGR {
	unsigned char r, g, b, c;
};

struct SMatrix3D {
	short m11, m12, m13, m21, m22, m23, m31, m32, m33, pad;
};

union CP2Data {
	struct {
		SVector3D     v0, v1, v2;
		CBGR          rgb;
		s32          otz;
		s32          ir0, ir1, ir2, ir3;
		SVector2D     sxy0, sxy1, sxy2, sxyp;
		SVector2Dz    sz0, sz1, sz2, sz3;
		CBGR          rgb0, rgb1, rgb2;
		s32          reserved;
		s32          mac0, mac1, mac2, mac3;
		u32 irgb, orgb;
		s32          lzcs, lzcr;
	} n;
	u32 r[32];
};

union CP2Ctrl {
	struct {
		SMatrix3D rMatrix;
		s32      trX, trY, trZ;
		SMatrix3D lMatrix;
		s32      rbk, gbk, bbk;
		SMatrix3D cMatrix;
		s32      rfc, gfc, bfc;
		s32      ofx, ofy;
		s32      h;
		s32      dqa, dqb;
		s32      zsf3, zsf4;
		s32      flag;
	} n;
	u32 r[32];
};

struct psxRegisters {
	GPRRegs GPR;
	CP0Regs CP0;
	CP2Data CP2D;
	CP2Ctrl CP2C;
	u32 pc;
	u32 code;
	u64 cycle;
	u32 interrupt;
	u32 pcWriteback;

	u64 iopNextEventCycle;

	s32 iopBreak;

	s32 iopCycleEE;
	u32 iopCycleEECarry;

	u64 sCycle[32];
	s32 eCycle[32];
};

alignas(16) extern psxRegisters psxRegs;

#ifndef _PC_

#define _i32(x) (s32)x
#define _u32(x) (u32)x

#define _i16(x) (s16)x
#define _u16(x) (u16)x

#define _i8(x) (s8)x
#define _u8(x) (u8)x

#define _PC_       psxRegs.pc

#define _Funct_          ((psxRegs.code      ) & 0x3F)
#define _Rd_             ((psxRegs.code >> 11) & 0x1F)
#define _Rt_             ((psxRegs.code >> 16) & 0x1F)
#define _Rs_             ((psxRegs.code >> 21) & 0x1F)
#define _Sa_             ((psxRegs.code >>  6) & 0x1F)
#define _Im_             ((u16)psxRegs.code)
#define _InstrucTarget_  (psxRegs.code & 0x03ffffff)

#define _Imm_	((short)psxRegs.code)
#define _ImmU_	(psxRegs.code&0xffff)

#define _rRs_   psxRegs.GPR.r[_Rs_]
#define _rRt_   psxRegs.GPR.r[_Rt_]
#define _rRd_   psxRegs.GPR.r[_Rd_]
#define _rSa_   psxRegs.GPR.r[_Sa_]
#define _rFs_   psxRegs.CP0.r[_Rd_]

#define _c2dRs_ psxRegs.CP2D.r[_Rs_]
#define _c2dRt_ psxRegs.CP2D.r[_Rt_]
#define _c2dRd_ psxRegs.CP2D.r[_Rd_]
#define _c2dSa_ psxRegs.CP2D.r[_Sa_]

#define _rHi_   psxRegs.GPR.n.hi
#define _rLo_   psxRegs.GPR.n.lo

#define _JumpTarget_    ((_InstrucTarget_ << 2) + (_PC_ & 0xf0000000))
#define _BranchTarget_  (((s32)(s16)_Imm_ * 4) + _PC_)

#define _SetLink(x)     psxRegs.GPR.r[x] = _PC_ + 4;

#endif

extern s32 psxNextDeltaCounter;
extern u64 psxNextStartCounter;
extern bool iopEventAction;
extern bool iopEventTestIsActive;

extern bool iopIsDelaySlot;

struct R3000Acpu {
	void (*Reserve)();
	void (*Reset)();
	s32 (*ExecuteBlock)( s32 eeCycles );
	void (*Clear)(u32 Addr, u32 Size);
	void (*Shutdown)();
};

extern R3000Acpu *psxCpu;
extern R3000Acpu psxInt;
extern R3000Acpu psxRec;

extern void psxReset();
extern void psxException(u32 code, u32 step);
extern void iopEventTest();

int psxIsBreakpointNeeded(u32 addr);
int psxIsMemcheckNeeded(u32 pc);

extern void (*psxBSC[64])();
extern void (*psxSPC[64])();
extern void (*psxREG[32])();
extern void (*psxCP0[32])();
extern void (*psxCP2[64])();
extern void (*psxCP2BSC[32])();

extern void psxBiosReset();
extern bool psxBiosCall();
