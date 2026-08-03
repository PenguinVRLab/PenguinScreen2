// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"
#include "R5900.h"
#include "R5900_Profiler.h"
#include "VU.h"
#include "iCore.h"

#include "common/emitter/x86emitter.h"

#define RFASTMEMBASE x86Emitter::rbp

extern u32 maxrecmem;
extern u32 pc;
extern int g_branch;
extern u32 target;
extern u32 s_nBlockCycles;
extern bool s_nBlockInterlocked;

#define R5900_TEXTPTR (&cpuRegs.GPR.r[9])

#define REC_FUNC(f) \
	void rec##f() \
	{ \
		recCall(Interp::f); \
	}

#define REC_FUNC_DEL(f, delreg) \
	void rec##f() \
	{ \
		if ((delreg) > 0) \
			_deleteEEreg(delreg, 1); \
		recCall(Interp::f); \
	}

#define REC_SYS(f) \
	void rec##f() \
	{ \
		recBranchCall(Interp::f); \
	}

#define REC_SYS_DEL(f, delreg) \
	void rec##f() \
	{ \
		if ((delreg) > 0) \
			_deleteEEreg(delreg, 1); \
		recBranchCall(Interp::f); \
	}

extern bool g_recompilingDelaySlot;

u8* recBeginThunk();
u8* recEndThunk();

bool TrySwapDelaySlot(u32 rs, u32 rt, u32 rd, bool allow_loadstore);
void SaveBranchState();
void LoadBranchState();

void recompileNextInstruction(bool delayslot, bool swapped_delay_slot);
void SetBranchReg();
void SetBranchImm(u32 imm);

void iFlushCall(int flushtype);
void recBranchCall(void (*func)());
void recCall(void (*func)());
u32 scaleblockcycles_clear();

namespace R5900
{
	namespace Dynarec
	{
		extern void recDoBranchImm(u32 branchTo, u32* jmpSkip, bool isLikely = false, bool swappedDelaySlot = false);
	}
}

#define GPR_IS_CONST1(reg) (EE_CONST_PROP && (reg) < 32 && (g_cpuHasConstReg & (1 << (reg))))
#define GPR_IS_CONST2(reg1, reg2) (EE_CONST_PROP && (g_cpuHasConstReg & (1 << (reg1))) && (g_cpuHasConstReg & (1 << (reg2))))
#define GPR_IS_DIRTY_CONST(reg) (EE_CONST_PROP && (reg) < 32 && (g_cpuHasConstReg & (1 << (reg))) && (!(g_cpuFlushedConstReg & (1 << (reg)))))
#define GPR_SET_CONST(reg) \
	{ \
		if ((reg) < 32) \
		{ \
			g_cpuHasConstReg |= (1 << (reg)); \
			g_cpuFlushedConstReg &= ~(1 << (reg)); \
		} \
	}

#define GPR_DEL_CONST(reg) \
	{ \
		if ((reg) < 32) \
			g_cpuHasConstReg &= ~(1 << (reg)); \
	}

alignas(16) extern GPR_reg64 g_cpuConstRegs[32];
extern u32 g_cpuHasConstReg, g_cpuFlushedConstReg;

void _eeMoveGPRtoR(const x86Emitter::xRegister32& to, int fromgpr, bool allow_preload = true);
void _eeMoveGPRtoR(const x86Emitter::xRegister64& to, int fromgpr, bool allow_preload = true);
void _eeMoveGPRtoM(uptr to, int fromgpr);

void _eeFlushAllDirty();
void _eeOnWriteReg(int reg, int signext);

void _deleteEEreg(int reg, int flush);
void _deleteEEreg128(int reg);

void _flushEEreg(int reg, bool clear = false);

int _eeTryRenameReg(int to, int from, int fromx86, int other, int xmminfo);

typedef void (*R5900FNPTR)();
typedef void (*R5900FNPTR_INFO)(int info);

#define EERECOMPILE_CODE0(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		EE::Profiler.EmitOp(eeOpcode::fn); \
		eeRecompileCode0(rec##fn##_const, rec##fn##_consts, rec##fn##_constt, rec##fn##_, (xmminfo)); \
	}
#define EERECOMPILE_CODERC0(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		EE::Profiler.EmitOp(eeOpcode::fn); \
		eeRecompileCodeRC0(rec##fn##_const, rec##fn##_consts, rec##fn##_constt, rec##fn##_, (xmminfo)); \
	}

#define EERECOMPILE_CODEX(codename, fn, xmminfo) \
	void rec##fn(void) \
	{ \
		EE::Profiler.EmitOp(eeOpcode::fn); \
		codename(rec##fn##_const, rec##fn##_, (xmminfo)); \
	}

#define EERECOMPILE_CODEI(codename, fn, xmminfo) \
	void rec##fn(void) \
	{ \
		EE::Profiler.EmitOp(eeOpcode::fn); \
		codename(rec##fn##_const, rec##fn##_, (xmminfo)); \
	}

void eeRecompileCodeRC0(R5900FNPTR constcode, R5900FNPTR_INFO constscode, R5900FNPTR_INFO consttcode, R5900FNPTR_INFO noconstcode, int xmminfo);
void eeRecompileCodeRC1(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode, int xmminfo);
void eeRecompileCodeRC2(R5900FNPTR constcode, R5900FNPTR_INFO noconstcode, int xmminfo);

#define FPURECOMPILE_CONSTCODE(fn, xmminfo) \
	void rec##fn(void) \
	{ \
		if (CHECK_FPU_FULL) \
			eeFPURecompileCode(DOUBLE::rec##fn##_xmm, R5900::Interpreter::OpcodeImpl::COP1::fn, xmminfo); \
		else \
			eeFPURecompileCode(rec##fn##_xmm, R5900::Interpreter::OpcodeImpl::COP1::fn, xmminfo); \
	}

int eeRecompileCodeXMM(int xmminfo);
void eeFPURecompileCode(R5900FNPTR_INFO xmmcode, R5900FNPTR fpucode, int xmminfo);
