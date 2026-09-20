// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "Vif.h"

enum VURegFlags
{
	REG_STATUS_FLAG = 16,
	REG_MAC_FLAG = 17,
	REG_CLIP_FLAG = 18,
	REG_ACC_FLAG = 19,
	REG_R = 20,
	REG_I = 21,
	REG_Q = 22,
	REG_P = 23,
	REG_VF0_FLAG = 24,
	REG_TPC = 26,
	REG_CMSAR0 = 27,
	REG_FBRST = 28,
	REG_VPU_STAT = 29,
	REG_CMSAR1 = 31
};

enum VUStatus
{
	VU_Ready = 0,
	VU_Run = 1,
	VU_Stop = 2,
};

union VECTOR
{
	struct
	{
		float x, y, z, w;
	} f;
	struct
	{
		u32 x, y, z, w;
	} i;

	float F[4];

	u128 UQ;
	s128 SQ;
	u64 UD[2];
	s64 SD[2];
	u32 UL[4];
	s32 SL[4];
	u16 US[8];
	s16 SS[8];
	u8 UC[16];
	s8 SC[16];
};

struct REG_VI
{
	union
	{
		float F;
		s32 SL;
		u32 UL;
		s16 SS[2];
		u16 US[2];
		s8 SC[4];
		u8 UC[4];
	};
	u32 padding[3];
};

#define VUFLAG_MFLAGSET 0x00000002
#define VUFLAG_INTCINTERRUPT 0x00000004
struct fdivPipe
{
	int enable;
	REG_VI reg;
	u64 sCycle;
	u32 Cycle;
	u32 statusflag;
};

struct efuPipe
{
	int enable;
	REG_VI reg;
	u64 sCycle;
	u32 Cycle;
};

struct fmacPipe
{
	u32 regupper;
	u32 reglower;
	int flagreg;
	u32 xyzwupper;
	u32 xyzwlower;
	u64 sCycle;
	u32 Cycle;
	u32 macflag;
	u32 statusflag;
	u32 clipflag;
};

struct ialuPipe
{
	int reg;
	u64 sCycle;
	u32 Cycle;
};

struct alignas(16) VURegs
{
	VECTOR VF[32];
	REG_VI VI[32];

	VECTOR ACC;
	REG_VI q;
	REG_VI p;

	uint idx;

	u64 cycle;
	u32 flags;

	u32 code;
	u32 start_pc;

	u32 branch;
	u32 branchpc;
	u32 delaybranchpc;
	bool takedelaybranch;
	u32 ebit;
	u32 pending_q;
	u32 pending_p;

	alignas(16) u32 micro_macflags[4];
	alignas(16) u32 micro_clipflags[4];
	alignas(16) u32 micro_statusflags[4];
	u32 macflag;
	u32 statusflag;
	u32 clipflag;

	s64 nextBlockCycles;

	u8* Mem;
	u8* Micro;

	u32 xgkickaddr;
	u32 xgkickdiff;
	u32 xgkicksizeremaining;
	u64 xgkicklastcycle;
	u32 xgkickcyclecount;
	u32 xgkickenable;
	u32 xgkickendpacket;

	u8 VIBackupCycles;
	u32 VIOldValue;
	u32 VIRegNumber;

	fmacPipe fmac[4];
	u32 fmacreadpos;
	u32 fmacwritepos;
	u32 fmaccount;
	fdivPipe fdiv;
	efuPipe efu;
	ialuPipe ialu[4];
	u32 ialureadpos;
	u32 ialuwritepos;
	u32 ialucount;

	VURegs()
	{
		Mem = NULL;
		Micro = NULL;
	}

	bool IsVU1() const;
	bool IsVU0() const;

	VIFregisters& GetVifRegs() const
	{
		return IsVU1() ? vif1Regs : vif0Regs;
	}
};

enum VUPipeState
{
	VUPIPE_NONE = 0,
	VUPIPE_FMAC,
	VUPIPE_FDIV,
	VUPIPE_EFU,
	VUPIPE_IALU,
	VUPIPE_BRANCH,
	VUPIPE_XGKICK
};

extern VURegs vuRegs[2];

static VURegs& VU0 = vuRegs[0];
static VURegs& VU1 = vuRegs[1];

inline bool VURegs::IsVU1() const { return this == &vuRegs[1]; }
inline bool VURegs::IsVU0() const { return this == &vuRegs[0]; }

extern void vuMemAllocate();
extern void vuMemReset();
extern void vuMemRelease();
extern u32* GET_VU_MEM(VURegs* VU, u32 addr);
