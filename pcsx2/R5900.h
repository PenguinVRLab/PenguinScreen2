// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>

namespace R5900 {
extern const char* const bios[256];
}

extern s32 EEsCycle;
extern u64 EEoCycle;

union GPR_reg {
	u128 UQ;
	s128 SQ;
	u64 UD[2];
	s64 SD[2];
	u32 UL[4];
	s32 SL[4];
	u16 US[8];
	s16 SS[8];
	u8  UC[16];
	s8  SC[16];
};

union GPRregs {
	struct {
		GPR_reg r0, at, v0, v1, a0, a1, a2, a3,
				t0, t1, t2, t3, t4, t5, t6, t7,
				s0, s1, s2, s3, s4, s5, s6, s7,
				t8, t9, k0, k1, gp, sp, s8, ra;
	} n;
	GPR_reg r[32];
};

union PERFregs {
	struct
	{
		union
		{
			struct
			{
				u32 pad0:1;
				u32 EXL0:1;
				u32 K0:1;
				u32 S0:1;
				u32 U0:1;
				u32 Event0:5;

				u32 pad1:1;

				u32 EXL1:1;
				u32 K1:1;
				u32 S1:1;
				u32 U1:1;
				u32 Event1:5;

				u32 Reserved:11;
				u32 CTE:1;
			} b;

			u32 val;
		} pccr;

		u32 pcr0, pcr1, pad;
	} n;
	u32 r[4];
};

union CP0regs {
	struct {
		u32	Index,    Random,    EntryLo0,  EntryLo1,
			Context,  PageMask,  Wired,     Reserved0,
			BadVAddr, Count,     EntryHi,   Compare;
		union {
			struct {
				u32 IE:1;
				u32 EXL:1;
				u32 ERL:1;
				u32 KSU:2;
				u32 unused0:3;
				u32 IM:8;
				u32 EIE:1;
				u32 _EDI:1;
				u32 CH:1;
				u32 unused1:3;
				u32 BEV:1;
				u32 DEV:1;
				u32 unused2:2;
				u32 FR:1;
				u32 unused3:1;
				u32 CU:4;
			} b;
			u32 val;
		} Status;
		u32   Cause,    EPC,       PRid,
			Config,   LLAddr,    WatchLO,   WatchHI,
			XContext, Reserved1, Reserved2, Debug,
			DEPC,     PerfCnt,   ErrCtl,    CacheErr,
			TagLo,    TagHi,     ErrorEPC,  DESAVE;
	} n;
	u32 r[32];
};

struct cpuRegisters {
	GPRregs GPR;
	GPR_reg HI;
	GPR_reg LO;
	CP0regs CP0;
	u32 sa;
	u32 IsDelaySlot;
	u32 pc;
	u32 code;
	PERFregs PERF;
	u32 eCycle[32];
	u64 sCycle[32];
	u64 cycle;
	u32 interrupt;
	int branch;
	int opmode;
	u32 tempcycles;
	u32 dmastall;
	u32 pcWriteback;

	u64 nextEventCycle;
	u64 lastEventCycle;
	u64 lastCOP0Cycle;
	u64 lastPERFCycle[2];
};

union GPR_reg64 {
	u64 UD[1];
	s64 SD[1];
	u32 UL[2];
	s32 SL[2];
	u16 US[4];
	s16 SS[4];
	u8  UC[8];
	s8  SC[8];
};

union FPRreg {
	float f;
	u32 UL;
	s32 SL;
};

struct fpuRegisters {
	FPRreg fpr[32];
	u32 fprc[32];
	FPRreg ACC;
	u32 ACCflag;
};

union PageMask_t
{
	struct
	{
		u32 : 13;
		u32 Mask : 12;
		u32 : 7;
	};
	u32 UL;
};

union EntryHi_t
{
	struct
	{
		u32 ASID:8;
		u32 : 5;
		u32 VPN2:19;
	};
	u32 UL;
};

union EntryLo_t
{
	struct
	{
		u32 G:1;
		u32 V:1;
		u32 D:1;
		u32 C:3;
		u32 PFN:20;
		u32 : 5;
		u32 S : 1;
	};
	u32 UL;

	constexpr bool isCached() const { return C == 0x3; }
	constexpr bool isValidCacheMode() const { return C == 0x2 || C == 0x3 || C == 0x7; }
};

struct tlbs
{
	PageMask_t PageMask;
	EntryHi_t EntryHi;
	EntryLo_t EntryLo0;
	EntryLo_t EntryLo1;

	constexpr u32 PFN0() const { return (EntryLo0.PFN & ~Mask()) << 12; }
	constexpr u32 PFN1() const { return (EntryLo1.PFN & ~Mask()) << 12; }
	constexpr u32 VPN2() const {return ((EntryHi.VPN2) & (~Mask())) << 13; }
	constexpr u32 Mask() const { return PageMask.Mask; }
	constexpr bool isGlobal() const { return EntryLo0.G && EntryLo1.G; }
	constexpr bool isSPR() const { return EntryLo0.S; }

	constexpr bool operator==(const tlbs& other) const
	{
		return PageMask.UL == other.PageMask.UL &&
			   EntryHi.UL == other.EntryHi.UL &&
			   EntryLo0.UL == other.EntryLo0.UL &&
			   EntryLo1.UL == other.EntryLo1.UL;
	}
};

#ifndef _PC_

#define _PC_       cpuRegs.pc

#define _Funct_          ((cpuRegs.code      ) & 0x3F)
#define _Rd_             ((cpuRegs.code >> 11) & 0x1F)
#define _Rt_             ((cpuRegs.code >> 16) & 0x1F)
#define _Rs_             ((cpuRegs.code >> 21) & 0x1F)
#define _Sa_             ((cpuRegs.code >>  6) & 0x1F)
#define _Im_             ((u16)cpuRegs.code)
#define _InstrucTarget_  (cpuRegs.code & 0x03ffffff)

#define _Imm_	((s16)cpuRegs.code)
#define _ImmU_	(cpuRegs.code&0xffff)
#define _ImmSB_	(cpuRegs.code&0x8000)

#define _Opcode_ (cpuRegs.code >> 26 )

#define _JumpTarget_     ((_InstrucTarget_ << 2) + (_PC_ & 0xf0000000))
#define _BranchTarget_   (((s32)(s16)_Im_ * 4) + _PC_)
#define _TrapCode_       ((u16)cpuRegs.code >> 6)

#define _SetLink(x)     (cpuRegs.GPR.r[x].UD[0] = _PC_ + 4)

#endif

struct cpuRegistersPack
{
	alignas(16) cpuRegisters cpuRegs;
	alignas(16) fpuRegisters fpuRegs;
};

alignas(16) extern cpuRegistersPack _cpuRegistersPack;
alignas(16) extern tlbs tlb[48];

struct cachedTlbs_t
{
	u32 count;

	alignas(16) std::array<u32, 48> PageMasks;
	alignas(16) std::array<u32, 48> PFN1s;
	alignas(16) std::array<u32, 48> CacheEnabled1;
	alignas(16) std::array<u32, 48> PFN0s;
	alignas(16) std::array<u32, 48> CacheEnabled0;
};

extern cachedTlbs_t cachedTlbs;

static cpuRegisters& cpuRegs = _cpuRegistersPack.cpuRegs;
static fpuRegisters& fpuRegs = _cpuRegistersPack.fpuRegs;

extern bool eeEventTestIsActive;

void intUpdateCPUCycles();
void intEventTest();
void intSetBranch();

void intDoBranch(u32 target);

const u32 EEKERNEL_START	= 0;
const u32 EENULL_START		= 0x81FC0;
const u32 EELOAD_START		= 0x82000;
const u32 EELOAD_SIZE		= 0x20000;
extern u32 g_eeloadMain, g_eeloadExec;

extern void eeloadHook();
extern void eeloadHook2();

struct R5900cpu
{
	void (*Reserve)();

	void (*Shutdown)();

	void (*Reset)();

	void (*Step)();

	void (*Execute)();

	void (*ExitExecution)();

	void (*CancelInstruction)();

	void (*Clear)(u32 Addr, u32 Size);
};

extern R5900cpu *Cpu;
extern R5900cpu intCpu;
extern R5900cpu recCpu;

enum EE_intProcessStatus
{
	INT_NOT_RUNNING = 0,
	INT_RUNNING,
	INT_REQ_LOOP
};

enum EE_EventType
{
	DMAC_VIF0	= 0,
	DMAC_VIF1,
	DMAC_GIF,
	DMAC_FROM_IPU,
	DMAC_TO_IPU,
	DMAC_SIF0,
	DMAC_SIF1,
	DMAC_SIF2,
	DMAC_FROM_SPR,
	DMAC_TO_SPR,

	DMAC_MFIFO_VIF,
	DMAC_MFIFO_GIF,

	DMAC_STALL_SIS		= 13,
	DMAC_MFIFO_EMPTY	= 14,
	DMAC_BUS_ERROR	= 15,

	DMAC_GIF_UNIT,
	VIF_VU0_FINISH,
	VIF_VU1_FINISH,
	IPU_PROCESS,
	VU_MTVU_BUSY
};

extern void CPU_INT( EE_EventType n, s32 ecycle );
extern void CPU_SET_DMASTALL(EE_EventType n, bool set);
extern uint intcInterrupt();
extern uint dmacInterrupt();

extern void cpuReset();
extern void cpuException(u32 code, u32 bd);
extern void cpuTlbMissR(u32 addr, u32 bd);
extern void cpuTlbMissW(u32 addr, u32 bd);
extern void cpuTestHwInts();
extern void cpuClearInt(uint n);
extern void GoemonPreloadTlb();
extern void GoemonUnloadTlb(u32 key);

extern void cpuSetNextEvent( u64 startCycle, s32 delta );
extern void cpuSetNextEventDelta( s32 delta );
extern int  cpuTestCycle( u64 startCycle, s32 delta );
extern void cpuSetEvent();
extern int cpuGetCycles(int interrupt);

extern void _cpuEventTest_Shared();

extern void cpuTestINTCInts();
extern void cpuTestDMACInts();
extern void cpuTestTIMRInts();

int isMemcheckNeeded(u32 pc);
int isBreakpointNeeded(u32 addr);

#define EXC_CODE(x)     ((x)<<2)

#define EXC_CODE_Int    EXC_CODE(0)
#define EXC_CODE_Mod    EXC_CODE(1)
#define EXC_CODE_TLBL   EXC_CODE(2)
#define EXC_CODE_TLBS   EXC_CODE(3)
#define EXC_CODE_AdEL   EXC_CODE(4)
#define EXC_CODE_AdES   EXC_CODE(5)
#define EXC_CODE_IBE    EXC_CODE(6)
#define EXC_CODE_DBE    EXC_CODE(7)
#define EXC_CODE_Sys    EXC_CODE(8)
#define EXC_CODE_Bp     EXC_CODE(9)
#define EXC_CODE_Ri     EXC_CODE(10)
#define EXC_CODE_CpU    EXC_CODE(11)
#define EXC_CODE_Ov     EXC_CODE(12)
#define EXC_CODE_Tr     EXC_CODE(13)
#define EXC_CODE_FPE    EXC_CODE(15)
#define EXC_CODE_WATCH  EXC_CODE(23)
#define EXC_CODE__MASK  0x0000007c
#define EXC_CODE__SHIFT 2
