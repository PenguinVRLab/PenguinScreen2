// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"
#include "GS/MultiISA.h"

static bool mvuNeedsFPCRUpdate(mV)
{
	if (isVU1 && THREAD_VU1)
		return true;

	return EmuConfig.Cpu.FPUFPCR.bitmask != (isVU0 ? EmuConfig.Cpu.VU0FPCR.bitmask : EmuConfig.Cpu.VU1FPCR.bitmask);
}

void mVUdispatcherAB(mV)
{
	mVU.startFunct = xGetAlignedCallTarget();

	{
		xScopedStackFrame frame(false, true);

		if (!isVU1) xFastCall((void*)mVUexecuteVU0, arg1reg, arg2reg);
		else        xFastCall((void*)mVUexecuteVU1, arg1reg, arg2reg);

		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(ptr32[isVU0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);

		xMOVAPS (xmmT1, ptr128[&mVU.regs().VI[REG_P].UL]);
		xMOVAPS (xmmPQ, ptr128[&mVU.regs().VI[REG_Q].UL]);
		xMOVDZX (xmmT2, ptr32[&mVU.regs().pending_q]);
		xSHUF.PS(xmmPQ, xmmT1, 0);
		xPSHUF.D(xmmPQ, xmmPQ, 0xe1);
		xMOVSS(xmmPQ, xmmT2);
		xPSHUF.D(xmmPQ, xmmPQ, 0xe1);

		if (isVU1)
		{
			xMOVDZX(xmmT2, ptr32[&mVU.regs().pending_p]);
			xPSHUF.D(xmmPQ, xmmPQ, 0x1B);
			xMOVSS(xmmPQ, xmmT2);
			xPSHUF.D(xmmPQ, xmmPQ, 0x1B);
		}

		xMOVAPS(xmmT1, ptr128[&mVU.regs().micro_macflags]);
		xMOVAPS(ptr128[mVU.macFlag], xmmT1);


		xMOVAPS(xmmT1, ptr128[&mVU.regs().micro_clipflags]);
		xMOVAPS(ptr128[mVU.clipFlag], xmmT1);

		xMOV(gprF0, ptr32[&mVU.regs().micro_statusflags[0]]);
		xMOV(gprF1, ptr32[&mVU.regs().micro_statusflags[1]]);
		xMOV(gprF2, ptr32[&mVU.regs().micro_statusflags[2]]);
		xMOV(gprF3, ptr32[&mVU.regs().micro_statusflags[3]]);

		xJMP(rax);

		mVU.exitFunct = x86Ptr;

		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);

		if (!isVU1) xFastCall((void*)mVUcleanUpVU0);
		else        xFastCall((void*)mVUcleanUpVU1);
	}

	xRET();

	Perf::any.Register(mVU.startFunct, static_cast<u32>(xGetPtr() - mVU.startFunct),
		mVU.index ? "VU1StartFunc" : "VU0StartFunc");
}

void mVUdispatcherCD(mV)
{
	mVU.startFunctXG = xGetAlignedCallTarget();

	{
		xScopedStackFrame frame(false, true);

		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(ptr32[isVU0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask]);

		mVUrestoreRegs(mVU);
		xMOV(gprF0, ptr32[&mVU.regs().micro_statusflags[0]]);
		xMOV(gprF1, ptr32[&mVU.regs().micro_statusflags[1]]);
		xMOV(gprF2, ptr32[&mVU.regs().micro_statusflags[2]]);
		xMOV(gprF3, ptr32[&mVU.regs().micro_statusflags[3]]);

		xJMP(ptrNative[&mVU.resumePtrXG]);

		mVU.exitFunctXG = x86Ptr;

		xMOV(ptr32[&mVU.regs().micro_statusflags[0]], gprF0);
		xMOV(ptr32[&mVU.regs().micro_statusflags[1]], gprF1);
		xMOV(ptr32[&mVU.regs().micro_statusflags[2]], gprF2);
		xMOV(ptr32[&mVU.regs().micro_statusflags[3]], gprF3);

		if (mvuNeedsFPCRUpdate(mVU))
			xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);
	}

	xRET();

	Perf::any.Register(mVU.startFunctXG, static_cast<u32>(xGetPtr() - mVU.startFunctXG),
		mVU.index ? "VU1StartFuncXG" : "VU0StartFuncXG");
}

static void mVUGenerateWaitMTVU(mV)
{
	mVU.waitMTVU = xGetAlignedCallTarget();

	int num_xmms = 0, num_gprs = 0;

	for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
	{
		if (!xRegister32::IsCallerSaved(i) || i == rsp.GetId())
			continue;

		if (i == gprT2.GetId())
			continue;

		xPUSH(xRegister64(i));
		num_gprs++;
	}

	for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
	{
		if (!xRegisterSSE::IsCallerSaved(i))
			continue;

		num_xmms++;
	}

	const int stack_size = (num_xmms * sizeof(u128)) + ((~num_gprs & 1) * sizeof(u64)) + SHADOW_STACK_SIZE;
	int stack_offset = SHADOW_STACK_SIZE;

	if (stack_size > 0)
	{
		xSUB(rsp, stack_size);
		for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
			stack_offset += sizeof(u128);
		}
	}

	xFastCall((void*)mVUwaitMTVU);

	stack_offset = (num_xmms - 1) * sizeof(u128) + SHADOW_STACK_SIZE;
	for (int i = static_cast<int>(iREGCNT_XMM - 1); i >= 0; i--)
	{
		if (!xRegisterSSE::IsCallerSaved(i))
			continue;

		xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
		stack_offset -= sizeof(u128);
	}
	xADD(rsp, stack_size);

	for (int i = static_cast<int>(iREGCNT_GPR - 1); i >= 0; i--)
	{
		if (!xRegister32::IsCallerSaved(i) || i == rsp.GetId())
			continue;

		if (i == gprT2.GetId())
			continue;

		xPOP(xRegister64(i));
	}

	xRET();

	Perf::any.Register(mVU.waitMTVU, static_cast<u32>(xGetPtr() - mVU.waitMTVU),
		mVU.index ? "VU1WaitMTVU" : "VU0WaitMTVU");
}

static void mVUGenerateCopyPipelineState(mV)
{
	mVU.copyPLState = xGetAlignedCallTarget();

	xLoadFarAddr(rdx, reinterpret_cast<u8*>(&mVU.prog.lpState));

	if (g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX)
	{
		xMOVAPS(ymm0, ptr[rax]);
		xMOVAPS(ymm1, ptr[rax + 32u]);
		xMOVAPS(ymm2, ptr[rax + 64u]);

		xMOVUPS(ptr[rdx], ymm0);
		xMOVUPS(ptr[rdx + 32u], ymm1);
		xMOVUPS(ptr[rdx + 64u], ymm2);

		xVZEROUPPER();
	}
	else
	{
		xMOVAPS(xmm0, ptr[rax]);
		xMOVAPS(xmm1, ptr[rax + 16u]);
		xMOVAPS(xmm2, ptr[rax + 32u]);
		xMOVAPS(xmm3, ptr[rax + 48u]);
		xMOVAPS(xmm4, ptr[rax + 64u]);
		xMOVAPS(xmm5, ptr[rax + 80u]);

		xMOVUPS(ptr[rdx], xmm0);
		xMOVUPS(ptr[rdx + 16u], xmm1);
		xMOVUPS(ptr[rdx + 32u], xmm2);
		xMOVUPS(ptr[rdx + 48u], xmm3);
		xMOVUPS(ptr[rdx + 64u], xmm4);
		xMOVUPS(ptr[rdx + 80u], xmm5);
	}

	xRET();

	Perf::any.Register(mVU.copyPLState, static_cast<u32>(xGetPtr() - mVU.copyPLState),
		mVU.index ? "VU1CopyPLState" : "VU0CopyPLState");
}

static void mVUGenerateCompareState(mV)
{
	mVU.compareStateF = xGetAlignedCallTarget();

	if (g_cpu.vectorISA < ProcessorFeatures::VectorISA::AVX2)
	{
		xMOVAPS  (xmm0, ptr32[arg1reg]);
		xPCMP.EQD(xmm0, ptr32[arg2reg]);
		xMOVAPS  (xmm1, ptr32[arg1reg + 0x10]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x10]);
		xPAND    (xmm0, xmm1);

		xMOVMSKPS(eax, xmm0);
		xXOR     (eax, 0xf);
		xForwardJNZ8 exitPoint;

		xMOVAPS  (xmm0, ptr32[arg1reg + 0x20]);
		xPCMP.EQD(xmm0, ptr32[arg2reg + 0x20]);
		xMOVAPS  (xmm1, ptr32[arg1reg + 0x30]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x30]);
		xPAND    (xmm0, xmm1);

		xMOVAPS  (xmm1, ptr32[arg1reg + 0x40]);
		xPCMP.EQD(xmm1, ptr32[arg2reg + 0x40]);
		xMOVAPS  (xmm2, ptr32[arg1reg + 0x50]);
		xPCMP.EQD(xmm2, ptr32[arg2reg + 0x50]);
		xPAND    (xmm1, xmm2);
		xPAND    (xmm0, xmm1);

		xMOVMSKPS(eax, xmm0);
		xXOR(eax, 0xf);

		exitPoint.SetTarget();
	}
	else
	{
		xMOVUPS(ymm0, ptr[arg1reg]);
		xPCMP.EQD(ymm0, ymm0, ptr[arg2reg]);
		xPMOVMSKB(eax, ymm0);
		xXOR(eax, 0xffffffff);
		xForwardJNZ8 exitPoint;

		xMOVUPS(ymm0, ptr[arg1reg + 0x20]);
		xMOVUPS(ymm1, ptr[arg1reg + 0x40]);
		xPCMP.EQD(ymm0, ymm0, ptr[arg2reg + 0x20]);
		xPCMP.EQD(ymm1, ymm1, ptr[arg2reg + 0x40]);
		xPAND(ymm0, ymm0, ymm1);

		xPMOVMSKB(eax, ymm0);
		xNOT(eax);

		exitPoint.SetTarget();
		xVZEROUPPER();
	}

	xRET();
}


_mVUt void* mVUexecute(u32 startPC, u32 cycles)
{

	microVU& mVU = mVUx;
	u32 vuLimit = vuIndex ? 0x3ff8 : 0xff8;
	if (startPC > vuLimit + 7)
	{
		DevCon.Warning("microVU%x Warning: startPC = 0x%x, cycles = 0x%x", vuIndex, startPC, cycles);
	}

	mVU.cycles = cycles;
	mVU.totalCycles = cycles;

	xSetTextPtr(mVU.textPtr());
	xSetPtr(mVU.prog.x86ptr);
	return mVUsearchProg<vuIndex>(startPC & vuLimit, (uptr)&mVU.prog.lpState);
}

_mVUt void mVUcleanUp()
{
	microVU& mVU = mVUx;

	mVU.prog.x86ptr = x86Ptr;

	if ((xGetPtr() < mVU.prog.x86start) || (xGetPtr() >= mVU.prog.x86end))
	{
		Console.WriteLn(vuIndex ? Color_Orange : Color_Magenta, "microVU%d: Program cache limit reached.", mVU.index);
		mVUreset(mVU, false);
	}

	mVU.cycles = mVU.totalCycles - std::max(0, mVU.cycles);
	mVU.regs().cycle += mVU.cycles;

	if (!vuIndex || !THREAD_VU1)
	{
		u32 cycles_passed = std::min(mVU.cycles, 3000) * EmuConfig.Speedhacks.EECycleSkip;
		if (cycles_passed > 0)
		{
			s64 vu0_offset = VU0.cycle - cpuRegs.cycle;
			cpuRegs.cycle += cycles_passed;

			if (!vuIndex)
				VU0.cycle = cpuRegs.cycle + vu0_offset;
			else
				VU0.cycle += cycles_passed;
		}
	}
	mVU.profiler.Print();
}

void* mVUexecuteVU0(u32 startPC, u32 cycles) { return mVUexecute<0>(startPC, cycles); }
void* mVUexecuteVU1(u32 startPC, u32 cycles) { return mVUexecute<1>(startPC, cycles); }
void mVUcleanUpVU0() { mVUcleanUp<0>(); }
void mVUcleanUpVU1() { mVUcleanUp<1>(); }
