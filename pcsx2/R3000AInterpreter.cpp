// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "Common.h"
#include "Config.h"
#include "VMManager.h"

#include "R5900OpcodeTables.h"
#include "DebugTools/Breakpoints.h"
#include "IopBios.h"
#include "IopHw.h"

using namespace R3000A;

bool iopIsDelaySlot = false;

static bool branch2 = 0;
static u32 branchPC;

static void doBranch(s32 tar);

void psxBGEZ()
{
	if (_i32(_rRs_) >= 0) doBranch(_BranchTarget_);
}

void psxBGEZAL()
{
	_SetLink(31);
	if (_i32(_rRs_) >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void psxBGTZ()
{
	if (_i32(_rRs_) > 0) doBranch(_BranchTarget_);
}

void psxBLEZ()
{
	if (_i32(_rRs_) <= 0) doBranch(_BranchTarget_);
}
void psxBLTZ()
{
	if (_i32(_rRs_) < 0) doBranch(_BranchTarget_);
}

void psxBLTZAL()
{
	_SetLink(31);
	if (_i32(_rRs_) < 0)
		{
			doBranch(_BranchTarget_);
		}
}

void psxBEQ()
{
	if (_i32(_rRs_) == _i32(_rRt_)) doBranch(_BranchTarget_);
}

void psxBNE()
{
	if (_i32(_rRs_) != _i32(_rRt_)) doBranch(_BranchTarget_);
}

void psxJ()
{
	u32 delayslot = iopMemRead32(psxRegs.pc);
	if (delayslot >> 16 == 0x2400 && irxImportExec(irxImportTableAddr(psxRegs.pc), delayslot & 0xffff))
		return;

	doBranch(_JumpTarget_);
}

void psxJAL()
{
	_SetLink(31);
	doBranch(_JumpTarget_);
}

void psxJR()
{
	doBranch(_u32(_rRs_));
}

void psxJALR()
{
	if (_Rd_)
	{
		_SetLink(_Rd_);
	}
	doBranch(_u32(_rRs_));
}

void psxBreakpoint(bool memcheck)
{
	u32 pc = psxRegs.pc;
	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_IOP, pc) != 0)
	{
		CBreakPoints::ClearSkipFirst(BREAKPOINT_IOP);
		return;
	}

	if (!memcheck)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_IOP, pc);
		if (cond && !cond->Evaluate())
			return;
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_IOP);
	VMManager::SetPaused(true);
	Cpu->ExitExecution();
}

void psxMemcheck(u32 op, u32 bits, bool store)
{
	u32 start = psxRegs.GPR.r[(op >> 21) & 0x1F];
	if ((s16)op != 0)
		start += (s16)op;

	u32 end = start + bits / 8;

	auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_IOP);
	for (size_t i = 0; i < checks.size(); i++)
	{
		auto& check = checks[i];

		if (check.result == 0)
			continue;
		if ((check.memCond & MEMCHECK_WRITE) == 0 && store)
			continue;
		if ((check.memCond & MEMCHECK_READ) == 0 && !store)
			continue;

		if (check.hasCond)
		{
			if (!check.cond.Evaluate())
				continue;
		}

		if (start < check.end && check.start < end)
			psxBreakpoint(true);
	}
}

void psxCheckMemcheck()
{
	u32 pc = psxRegs.pc;
	int needed = psxIsMemcheckNeeded(pc);
	if (needed == 0)
		return;

	u32 op = iopMemRead32(needed == 2 ? pc + 4 : pc);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
	case MEMTYPE_BYTE:
		psxMemcheck(op, 8, store);
		break;
	case MEMTYPE_HALF:
		psxMemcheck(op, 16, store);
		break;
	case MEMTYPE_WORD:
		psxMemcheck(op, 32, store);
		break;
	case MEMTYPE_DWORD:
		psxMemcheck(op, 64, store);
		break;
	}
}

static __fi void execI()
{
#if defined(EXTRA_DEBUG) || defined(PCSX2_DEVBUILD)
	if (psxIsBreakpointNeeded(psxRegs.pc))
		psxBreakpoint(false);

	psxCheckMemcheck();
	
	CBreakPoints::CommitClearSkipFirst(BREAKPOINT_IOP);
#endif

	if (psxRegs.pc == 0x1630 && EmuConfig.CurrentIRX.length() > 3) {
		if (iopMemRead32(0x20018) == 0x1F) {
			iopMemWrite32(0x20094, 0xbffc0000);
		}
	}

	psxRegs.code = iopMemRead32(psxRegs.pc);

		PSXCPU_LOG("%s", disR3000AF(psxRegs.code, psxRegs.pc));

	psxRegs.pc+= 4;
	psxRegs.cycle++;

	psxBSC[psxRegs.code >> 26]();
}

static void doBranch(s32 tar) {
	if (tar == 0x0)
		DevCon.Warning("[R3000 Interpreter] Warning: Branch to 0x0!");

	if(tar == 0x890)
	{
		DevCon.WriteLn(Color_Gray, "R3000 Debugger: Branch to 0x890 (SYSMEM). Clearing modules.");
		R3000SymbolGuardian.ClearIrxModules();
	}

	if(tar == 0xbfc4a000) {
		psxRegs.GPR.n.a0 = Ps2MemSize::ExposedIopRam >> 20;
	}

	branch2 = iopIsDelaySlot = true;
	branchPC = tar;
	execI();
	PSXCPU_LOG( "\n" );
	iopIsDelaySlot = false;
	psxRegs.pc = branchPC;

	iopEventTest();
}

static void intReserve() {
}

static void intAlloc() {
}

static void intReset() {
	intAlloc();
}

static s32 intExecuteBlock( s32 eeCycles )
{
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;
	u64 lastIOPCycle = 0;

	while (psxRegs.iopCycleEE > 0)
	{
		lastIOPCycle = psxRegs.cycle;
		if ((psxHu32(HW_ICFG) & 8) && ((psxRegs.pc & 0x1fffffffU) == 0xa0 || (psxRegs.pc & 0x1fffffffU) == 0xb0 || (psxRegs.pc & 0x1fffffffU) == 0xc0))
			psxBiosCall();

		branch2 = 0;
		while (!branch2)
			execI();

		
		if ((psxHu32(HW_ICFG) & (1 << 3)))
		{
			const u32 cnum = 1280;
			const u32 cdenom = 147;

			const u32 t = ((cnum * (psxRegs.cycle - lastIOPCycle)) + psxRegs.iopCycleEECarry);
			psxRegs.iopCycleEE -= t / cdenom;
			psxRegs.iopCycleEECarry = t % cdenom;
		}
		else
		{ 
			psxRegs.iopCycleEE -= (psxRegs.cycle - lastIOPCycle) * 8;
		}
	}

	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

static void intClear(u32 Addr, u32 Size) {
}

static void intShutdown() {
}

R3000Acpu psxInt = {
	intReserve,
	intReset,
	intExecuteBlock,
	intClear,
	intShutdown
};
