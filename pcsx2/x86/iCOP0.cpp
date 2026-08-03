// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Important Note to Future Developers:
//   None of the COP0 instructions are really critical performance items,
//   so don't waste time converting any more them into recompiled code
//   unless it can make them nicely compact.  Calling the C versions will
//   suffice.

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "iR5900.h"
#include "iCOP0.h"

namespace Interp = R5900::Interpreter::OpcodeImpl::COP0;
using namespace x86Emitter;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace COP0 {

static void _setupBranchTest()
{
	_eeFlushAllDirty();

	xMOV(eax, ptr[(&psHu32(DMAC_PCR))]);
	xMOV(ecx, 0x3ff);
	xNOT(eax);
	xOR(eax, ptr[(&psHu32(DMAC_STAT))]);
	xAND(eax, ecx);
	xCMP(eax, ecx);
}

void recBC0F()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, false);
	_setupBranchTest();
	recDoBranchImm(branchTo, JE32(0), false, swap);
}

void recBC0T()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const bool swap = TrySwapDelaySlot(0, 0, 0, false);
	_setupBranchTest();
	recDoBranchImm(branchTo, JNE32(0), false, swap);
}

void recBC0FL()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	recDoBranchImm(branchTo, JE32(0), true, false);
}

void recBC0TL()
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	recDoBranchImm(branchTo, JNE32(0), true, false);
}

void recTLBR() { recCall(Interp::TLBR); }
void recTLBP() { recCall(Interp::TLBP); }
void recTLBWI() { recCall(Interp::TLBWI); }
void recTLBWR() { recCall(Interp::TLBWR); }

void recERET()
{
	recBranchCall(Interp::ERET);
}

void recEI()
{
	recBranchCall(Interp::EI);
}

void recDI()
{

	if (!g_recompilingDelaySlot)
		recompileNextInstruction(false, false);

	xMOV(eax, ptr[&cpuRegs.CP0.n.Status]);
	xTEST(eax, 0x20006);
	xForwardJNZ8 iHaveNoIdea;
	xTEST(eax, 0x18);
	xForwardJNZ8 inUserMode;
	iHaveNoIdea.SetTarget();
	xAND(eax, ~(u32)0x10000);
	xMOV(ptr[&cpuRegs.CP0.n.Status], eax);
	inUserMode.SetTarget();
}


#ifndef CP0_RECOMPILE

REC_SYS(MFC0);
REC_SYS(MTC0);

#else

void recMFC0()
{
	if (_Rd_ == 9)
	{
		xMOV(rcx, ptr64[&cpuRegs.cycle]);
		xADD(rcx, scaleblockcycles_clear());
		xMOV(ptr64[&cpuRegs.cycle], rcx);
		xMOV(rax, rcx);
		xSUB(rax, ptr[&cpuRegs.lastCOP0Cycle]);
		xADD(ptr[&cpuRegs.CP0.n.Count], rax);
		xMOV(ptr[&cpuRegs.lastCOP0Cycle], rcx);

		if (!_Rt_)
			return;

		const int regt = _Rt_ ? _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE) : -1;
		xMOVSX(xRegister64(regt), ptr32[&cpuRegs.CP0.r[_Rd_]]);
		return;
	}

	if (!_Rt_)
		return;

	if (_Rd_ == 25)
	{
		if (0 == (_Imm_ & 1))
		{
			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pccr]);
		}
		else if (0 == (_Imm_ & 2))
		{
			iFlushCall(FLUSH_INTERPRETER);
			xMOV(rax, ptr64[&cpuRegs.cycle]);
			xADD(rax, scaleblockcycles_clear());
			xMOV(ptr64[&cpuRegs.cycle], rax);
			xFastCall((void*)COP0_UpdatePCCR);

			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pcr0]);
		}
		else
		{
			iFlushCall(FLUSH_INTERPRETER);
			xMOV(rax, ptr64[&cpuRegs.cycle]);
			xADD(rax, scaleblockcycles_clear());
			xMOV(ptr64[&cpuRegs.cycle], rax);
			xFastCall((void*)COP0_UpdatePCCR);

			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			xMOVSX(xRegister64(regt), ptr32[&cpuRegs.PERF.n.pcr1]);
		}

		return;
	}
	else if (_Rd_ == 24)
	{
		COP0_LOG("MFC0 Breakpoint debug Registers code = %x\n", cpuRegs.code & 0x3FF);
		return;
	}

	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
	xMOVSX(xRegister64(regt), ptr32[&cpuRegs.CP0.r[_Rd_]]);
}

void recMTC0()
{
	if (GPR_IS_CONST1(_Rt_))
	{
		switch (_Rd_)
		{
			case 12:
				iFlushCall(FLUSH_INTERPRETER);
				xMOV(rax, ptr64[&cpuRegs.cycle]);
				xADD(rax, scaleblockcycles_clear());
				xMOV(ptr64[&cpuRegs.cycle], rax);
				xFastCall((void*)WriteCP0Status, g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 16:
				iFlushCall(FLUSH_INTERPRETER);
				xFastCall((void*)WriteCP0Config, g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 9:
				xMOV(rcx, ptr64[&cpuRegs.cycle]);
				xADD(rcx, scaleblockcycles_clear());
				xMOV(ptr64[&cpuRegs.cycle], rcx);
				xMOV(ptr64[&cpuRegs.lastCOP0Cycle], rcx);
				xMOV(ptr32[&cpuRegs.CP0.r[9]], g_cpuConstRegs[_Rt_].UL[0]);
				break;

			case 25:
				if (0 == (_Imm_ & 1))
				{
					if (0 != (_Imm_ & 0x3E))
						break;
					iFlushCall(FLUSH_INTERPRETER);
					xMOV(rax, ptr64[&cpuRegs.cycle]);
					xADD(rax, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rax);
					xFastCall((void*)COP0_UpdatePCCR);
					xMOV(ptr32[&cpuRegs.PERF.n.pccr], g_cpuConstRegs[_Rt_].UL[0]);
					xFastCall((void*)COP0_DiagnosticPCCR);
				}
				else if (0 == (_Imm_ & 2))
				{
					xMOV(rax, ptr64[&cpuRegs.cycle]);
					xADD(rax, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rax);
					xMOV(ptr32[&cpuRegs.PERF.n.pcr0], g_cpuConstRegs[_Rt_].UL[0]);
					xMOV(ptr64[&cpuRegs.lastPERFCycle[0]], rax);
				}
				else
				{
					xMOV(rax, ptr64[&cpuRegs.cycle]);
					xADD(rax, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rax);
					xMOV(ptr32[&cpuRegs.PERF.n.pcr1], g_cpuConstRegs[_Rt_].UL[0]);
					xMOV(ptr64[&cpuRegs.lastPERFCycle[1]], rax);
				}
				break;

			case 24:
				COP0_LOG("MTC0 Breakpoint debug Registers code = %x\n", cpuRegs.code & 0x3FF);
				break;

			default:
				xMOV(ptr32[&cpuRegs.CP0.r[_Rd_]], g_cpuConstRegs[_Rt_].UL[0]);
				break;
		}
	}
	else
	{
		switch (_Rd_)
		{
			case 12:
				_eeMoveGPRtoR(arg1reg, _Rt_);
				iFlushCall(FLUSH_INTERPRETER);
				xMOV(rax, ptr64[&cpuRegs.cycle]);
				xADD(rax, scaleblockcycles_clear());
				xMOV(ptr64[&cpuRegs.cycle], rax);
				xFastCall((void*)WriteCP0Status);
				break;

			case 16:
				_eeMoveGPRtoR(arg1reg, _Rt_);
				iFlushCall(FLUSH_INTERPRETER);
				xFastCall((void*)WriteCP0Config);
				break;

			case 9:
				xMOV(rcx, ptr64[&cpuRegs.cycle]);
				xADD(rcx, scaleblockcycles_clear());
				xMOV(ptr64[&cpuRegs.cycle], rcx);
				_eeMoveGPRtoM((uptr)&cpuRegs.CP0.r[9], _Rt_);
				xMOV(ptr64[&cpuRegs.lastCOP0Cycle], rcx);
				break;

			case 25:
				if (0 == (_Imm_ & 1))
				{
					if (0 != (_Imm_ & 0x3E))
						break;
					iFlushCall(FLUSH_INTERPRETER);
					xMOV(rax, ptr64[&cpuRegs.cycle]);
					xADD(rax, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rax);
					xFastCall((void*)COP0_UpdatePCCR);
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pccr, _Rt_);
					xFastCall((void*)COP0_DiagnosticPCCR);
				}
				else if (0 == (_Imm_ & 2))
				{
					xMOV(rcx, ptr64[&cpuRegs.cycle]);
					xADD(rcx, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rcx);
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pcr0, _Rt_);
					xMOV(ptr64[&cpuRegs.lastPERFCycle[0]], rcx);
				}
				else
				{
					xMOV(rcx, ptr64[&cpuRegs.cycle]);
					xADD(rcx, scaleblockcycles_clear());
					xMOV(ptr64[&cpuRegs.cycle], rcx);
					_eeMoveGPRtoM((uptr)&cpuRegs.PERF.n.pcr1, _Rt_);
					xMOV(ptr64[&cpuRegs.lastPERFCycle[1]], rcx);
				}
				break;

			case 24:
				COP0_LOG("MTC0 Breakpoint debug Registers code = %x\n", cpuRegs.code & 0x3FF);
				break;

			default:
				_eeMoveGPRtoM((uptr)&cpuRegs.CP0.r[_Rd_], _Rt_);
				break;
		}
	}
}
#endif


}
}
}
}
