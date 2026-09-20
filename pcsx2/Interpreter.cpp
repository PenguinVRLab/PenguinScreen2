// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "Elfheader.h"
#include "Cache.h"

#include "DebugTools/Breakpoints.h"

#include "common/FastJmp.h"

#include <float.h>

using namespace R5900;

extern int vu0branch, vu1branch;

static int branch2 = 0;
static u32 cpuBlockCycles = 0;
static std::string disOut;
static bool intExitExecution = false;
static fastjmp_buf intJmpBuf;
static u32 intLastBranchTo;

void intEventTest();

void intUpdateCPUCycles()
{
	const bool lowcycles = (cpuBlockCycles <= 40);
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	u32 scale_cycles = 0;

	if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
		scale_cycles = cpuBlockCycles >> 3;

	else if (cyclerate > 1)
		scale_cycles = cpuBlockCycles >> (2 + cyclerate);

	else if (cyclerate == 1)
		scale_cycles = (cpuBlockCycles >> 3) / 1.3f;

	else if (cyclerate == -1)
		scale_cycles = (cpuBlockCycles <= 80 || cpuBlockCycles > 168 ? 5 : 7) * cpuBlockCycles / 32;

	else
		scale_cycles = ((5 + (-2 * (cyclerate + 1))) * cpuBlockCycles) >> 5;

	cpuRegs.cycle += (scale_cycles < 1) ? 1 : scale_cycles;

	if (cyclerate > 1)
	{
		cpuBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
	}
	else
	{
		cpuBlockCycles &= 0x7;
	}
}

void intBreakpoint(bool memcheck)
{
	const u32 pc = cpuRegs.pc;
 	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_EE, pc) != 0)
	{
		CBreakPoints::ClearSkipFirst(BREAKPOINT_EE);
		return;
	}

	if (!memcheck)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_EE, pc);
		if (cond && !cond->Evaluate())
			return;
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_EE);
	VMManager::SetPaused(true);
	Cpu->ExitExecution();
}

void intMemcheck(u32 op, u32 bits, bool store)
{
	u32 start = cpuRegs.GPR.r[(op >> 21) & 0x1F].UL[0];
	if (static_cast<s16>(op) != 0)
		start += static_cast<s16>(op);
	if (bits == 128)
		start &= ~0x0F;

	start = standardizeBreakpointAddress(start);
	const u32 end = start + bits/8;

	auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_EE);
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
			intBreakpoint(true);
	}
}

void intCheckMemcheck()
{
	const u32 pc = cpuRegs.pc;
	const int needed = isMemcheckNeeded(pc);
	if (needed == 0)
		return;

	const u32 op = memRead32(needed == 2 ? pc + 4 : pc);
	const OPCODE& opcode = GetInstruction(op);

	const bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
		case MEMTYPE_BYTE:
			intMemcheck(op, 8, store);
			break;
		case MEMTYPE_HALF:
			intMemcheck(op, 16, store);
			break;
		case MEMTYPE_WORD:
			intMemcheck(op, 32, store);
			break;
		case MEMTYPE_DWORD:
			intMemcheck(op, 64, store);
			break;
		case MEMTYPE_QWORD:
			intMemcheck(op, 128, store);
			break;
	}
}

static void execI()
{
#if defined(EXTRA_DEBUG) || defined(PCSX2_DEVBUILD)
	if (isBreakpointNeeded(cpuRegs.pc))
		intBreakpoint(false);

	intCheckMemcheck();

	CBreakPoints::CommitClearSkipFirst(BREAKPOINT_EE);
#endif

	const u32 pc = cpuRegs.pc;
	cpuRegs.pc += 4;

	cpuRegs.code = memRead32( pc );

	const OPCODE& opcode = GetCurrentInstruction();
#if 0
	static long int runs = 0;
	runs++;
	if (runs > 1599999999)
	{
		if (opcode.Name[0] == 'L')
		{
			Console.WriteLn ("Load %s", opcode.Name);
		}
	}
#endif

#if 0
	static long int print_me = 0;
	if (pc == 0x80000000)
	{
		print_me = 2000;
	}
	if (print_me)
	{
		print_me--;
		disOut.clear();
		disR5900Fasm(disOut, cpuRegs.code, pc);
		CPU_LOG( disOut.c_str() );
	}
#endif


	cpuBlockCycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));

	opcode.interpret();
}

static __fi void _doBranch_shared(u32 tar)
{
	branch2 = cpuRegs.branch = 1;
	execI();

	if( cpuRegs.branch != 0 )
	{
		if (Cpu == &intCpu)
		{
			if (intLastBranchTo == tar && EmuConfig.Speedhacks.WaitLoop)
			{
				intUpdateCPUCycles();
				bool can_skip = true;
				if (tar != 0x81fc0)
				{
					if ((cpuRegs.pc - tar) < (4 * 10))
					{
						for (u32 i = tar; i < cpuRegs.pc; i += 4)
						{
							if (PSM(i) != 0)
							{
								can_skip = false;
								break;
							}
						}
					}
					else
						can_skip = false;
				}

				if (can_skip)
				{
					if (static_cast<s64>(cpuRegs.nextEventCycle - cpuRegs.cycle) > 0)
						cpuRegs.cycle = cpuRegs.nextEventCycle;
					else
						cpuRegs.nextEventCycle = cpuRegs.cycle;
				}
			}
		}
		intLastBranchTo = tar;
		cpuRegs.pc = tar;
		cpuRegs.branch = 0;
	}
}

static void doBranch( u32 target )
{
	_doBranch_shared( target );
	intUpdateCPUCycles();
	intEventTest();
}

void intDoBranch(u32 target)
{
	_doBranch_shared( target );

	if( Cpu == &intCpu )
	{
		intUpdateCPUCycles();
		intEventTest();
	}
}

void intSetBranch()
{
	branch2 = 1;
}

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {

void J()
{
	doBranch(_JumpTarget_);
}

void JAL()
{
	if (EmuConfig.Gamefixes.GoemonTlbHack) {
		if (_JumpTarget_ == 0x3563b8)
			GoemonUnloadTlb(cpuRegs.GPR.n.a0.UL[0]);
	}
	_SetLink(31);
	doBranch(_JumpTarget_);
}

void BEQ()
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0])
		doBranch(_BranchTarget_);
	else
		intEventTest();
}

void BNE()
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0])
		doBranch(_BranchTarget_);
	else
		intEventTest();
}

void BGEZ()
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BGEZAL()
{
	_SetLink(31);
	if (cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BGTZ()
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] > 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLEZ()
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] <= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLTZ()
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLTZAL()
{
	_SetLink(31);
	if (cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BEQL()
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0])
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BNEL()
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0])
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLEZL()
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] <= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGTZL()
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] > 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLTZL()
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGEZL()
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLTZALL()
{
	_SetLink(31);
	if(cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGEZALL()
{
	_SetLink(31);
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void JR()
{
	if (EmuConfig.Gamefixes.GoemonTlbHack) {
		const u32 add = cpuRegs.GPR.r[_Rs_].UL[0];
		if (add == 0x33ad48 || add == 0x35060c)
			GoemonPreloadTlb();
	}
	doBranch(cpuRegs.GPR.r[_Rs_].UL[0]);
}

void JALR()
{
	const u32 temp = cpuRegs.GPR.r[_Rs_].UL[0];

	if (_Rd_)  _SetLink(_Rd_);

	doBranch(temp);
}

} } }


static void intReserve()
{
}

static void intReset()
{
	cpuRegs.branch = 0;
	branch2 = 0;
}

void intEventTest()
{
	_cpuEventTest_Shared();

	if (intExitExecution)
	{
		intExitExecution = false;
		if (CHECK_EEREC)
			writebackCache();
		fastjmp_jmp(&intJmpBuf, 1);
	}
}

static void intSafeExitExecution()
{
	if (eeEventTestIsActive)
		intExitExecution = true;
	else
	{
		if (CHECK_EEREC)
			writebackCache();
		fastjmp_jmp(&intJmpBuf, 1);
	}
}

static void intCancelInstruction()
{
	fastjmp_jmp(&intJmpBuf, 0);
}

static void intExecute()
{
	if (fastjmp_set(&intJmpBuf) != 0)
		return;

	for (;;)
	{
		if (!VMManager::Internal::HasBootedELF())
		{
			u32 elf_entry_point = VMManager::Internal::GetCurrentELFEntryPoint();
			u32 eeload_main = g_eeloadMain;
			u32 eeload_exec = g_eeloadExec;

			while (true)
			{
				execI();

				if (cpuRegs.pc == EELOAD_START)
				{
					const u32 mainjump = memRead32(EELOAD_START + 0x9c);
					if (mainjump >> 26 == 3)
						g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);

					eeload_main = g_eeloadMain;
				}
				else if (cpuRegs.pc == eeload_main)
				{
					eeloadHook();
					if (VMManager::Internal::IsFastBootInProgress())
					{
						const u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
						const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
						const u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
						const u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
						if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3))
							g_eeloadExec = EELOAD_START + 0x2B8;
						else if (typeAexecjump >> 26 == 3)
							g_eeloadExec = EELOAD_START + 0x170;
						else
							Console.WriteLn("intExecute: Could not enable launch arguments for fast boot mode; unidentified BIOS version! Please report this to the PCSX2 developers.");

						eeload_exec = g_eeloadExec;
					}

					elf_entry_point = VMManager::Internal::GetCurrentELFEntryPoint();
				}
				else if (cpuRegs.pc == eeload_exec)
				{
					eeloadHook2();
				}
				else if (cpuRegs.pc == elf_entry_point)
				{
					VMManager::Internal::EntryPointCompilingOnCPUThread();
					break;
				}
			}
		}
		else
		{
			while (true)
				execI();
		}
	}
}

static void intStep()
{
	execI();
}

static void intClear(u32 Addr, u32 Size)
{
}

static void intShutdown() {
}

R5900cpu intCpu =
{
	intReserve,
	intShutdown,

	intReset,
	intStep,
	intExecute,

	intSafeExitExecution,
	intCancelInstruction,

	intClear
};
