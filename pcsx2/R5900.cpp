// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "common/StringUtil.h"
#include "ps2/BiosTools.h"
#include "R5900.h"
#include "R3000A.h"
#include "ps2/pgif.h"
#include "VUmicro.h"
#include "COP0.h"
#include "MTVU.h"
#include "VMManager.h"

#include "Hardware.h"
#include "IPU/IPUdma.h"

#include "Elfheader.h"
#include "CDVD/CDVD.h"
#include "Patch.h"
#include "GameDatabase.h"
#include "GSDumpReplayer.h"

#include "DebugTools/Breakpoints.h"
#include "DebugTools/MIPSAnalyst.h"
#include "DebugTools/SymbolGuardian.h"
#include "R5900OpcodeTables.h"

#include "fmt/format.h"

using namespace R5900;

s32 EEsCycle;
u64 EEoCycle;

alignas(16) cpuRegistersPack _cpuRegistersPack;
alignas(16) tlbs tlb[48];
cachedTlbs_t cachedTlbs;

R5900cpu *Cpu = NULL;

static constexpr uint eeWaitCycles = 3072;

bool eeEventTestIsActive = false;
EE_intProcessStatus eeRunInterruptScan = INT_NOT_RUNNING;

u32 g_eeloadMain = 0, g_eeloadExec = 0, g_osdsys_str = 0;

const int kMaxArgs = 16;
uptr g_argPtrs[kMaxArgs];
#define DEBUG_LAUNCHARG 0

void cpuReset()
{
	std::memset(&cpuRegs, 0, sizeof(cpuRegs));
	std::memset(&fpuRegs, 0, sizeof(fpuRegs));
	std::memset(&tlb, 0, sizeof(tlb));
	cachedTlbs.count = 0;

	cpuRegs.pc				= 0xbfc00000;
	cpuRegs.CP0.n.Config	= 0x440;
	cpuRegs.CP0.n.Status.val= 0x70400004;
	cpuRegs.CP0.n.PRid		= 0x00002e20;
	fpuRegs.fprc[0]			= 0x00002e30;
	fpuRegs.fprc[31]		= 0x01000001;

	cpuRegs.nextEventCycle = cpuRegs.cycle + 4;
	EEsCycle = 0;
	EEoCycle = cpuRegs.cycle;

	psxReset();
	pgifInit();

	extern void Deci2Reset();
	Deci2Reset();

	AllowParams1 = !VMManager::Internal::IsFastBootInProgress();
	AllowParams2 = !VMManager::Internal::IsFastBootInProgress();
	ParamsRead = false;

	g_eeloadMain = 0;
	g_eeloadExec = 0;
	g_osdsys_str = 0;

	CBreakPoints::ClearSkipFirst();
}

__ri void cpuException(u32 code, u32 bd)
{
	bool errLevel2, checkStatus;
	u32 offset = 0;

    cpuRegs.branch = 0;
	cpuRegs.CP0.n.Cause = code & 0xffff;

	if(cpuRegs.CP0.n.Status.b.ERL == 0)
	{
		errLevel2 = false;
		checkStatus = (cpuRegs.CP0.n.Status.b.BEV == 0);

		if (((code & 0x7C) >= 0x8) && ((code & 0x7C) <= 0xC))
			offset = 0x0;
		else if ((code & 0x7C) == 0x0)
			offset = 0x200;
		else
			offset = 0x180;
	}
	else
	{
		errLevel2 = true;
		checkStatus = (cpuRegs.CP0.n.Status.b.DEV == 0);

		Console.Error("*PCSX2* FIX ME: Level 2 cpuException");
		if ((code & 0x38000) <= 0x8000 )
		{
			cpuRegs.pc = 0xBFC00000;
			Console.Warning("Reset request");
			cpuUpdateOperationMode();
			return;
		}
		else if ((code & 0x38000) == 0x10000)
			offset = 0x80;
		else if ((code & 0x38000) == 0x18000)
			offset = 0x100;
		else
			Console.Error("Unknown Level 2 Exception!! Cause %x", code);
	}

	if (cpuRegs.CP0.n.Status.b.EXL == 0)
	{
		cpuRegs.CP0.n.Status.b.EXL = 1;
		if (bd)
		{
			Console.Warning("branch delay!!");
			cpuRegs.CP0.n.EPC = cpuRegs.pc - 4;
			cpuRegs.CP0.n.Cause |= 0x80000000;
		}
		else
		{
			cpuRegs.CP0.n.EPC = cpuRegs.pc;
			cpuRegs.CP0.n.Cause &= ~0x80000000;
		}
	}
	else
	{
		offset = 0x180;
		if (errLevel2) Console.Warning("cpuException: Status.EXL = 1 cause %x", code);
	}

	if (checkStatus)
		cpuRegs.pc = 0x80000000 + offset;
	else
		cpuRegs.pc = 0xBFC00200 + offset;

	cpuUpdateOperationMode();
}

void cpuTlbMiss(u32 addr, u32 bd, u32 excode)
{
	if (Cpu != &intCpu || IsDebugBuild) {
		Console.Error("cpuTlbMiss pc:%x, cycl:%llx, addr: %x, status=%x, code=%x",
				cpuRegs.pc, cpuRegs.cycle, addr, cpuRegs.CP0.n.Status.val, excode);
	}

	cpuRegs.CP0.n.BadVAddr = addr;
	cpuRegs.CP0.n.Context &= 0xFF80000F;
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0;
	cpuRegs.CP0.n.EntryHi = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF);

	cpuRegs.pc -= 4;
	cpuException(excode, bd);
}

void cpuTlbMissR(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBL);
}

void cpuTlbMissW(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBS);
}

__fi void cpuSetNextEvent( u64 startCycle, s32 delta )
{

	if( (int)(cpuRegs.nextEventCycle - startCycle) > delta )
	{
		cpuRegs.nextEventCycle = startCycle + delta;
	}
}

__fi void cpuSetNextEventDelta( s32 delta )
{
	cpuSetNextEvent( cpuRegs.cycle, delta );
}

__fi int cpuGetCycles(int interrupt)
{
	if(interrupt == VU_MTVU_BUSY && (!THREAD_VU1 || INSTANT_VU1))
		return 1;
	else
	{
		const int cycles = (cpuRegs.sCycle[interrupt] + cpuRegs.eCycle[interrupt]) - cpuRegs.cycle;
		return std::max(1, cycles);
	}

}

__fi int cpuTestCycle( u64 startCycle, s32 delta )
{

	return (int)(cpuRegs.cycle - startCycle) >= delta;
}

__fi void cpuSetEvent()
{
	cpuRegs.nextEventCycle = cpuRegs.cycle;
}

__fi void cpuClearInt( uint i )
{
	pxAssume( i < 32 );
	cpuRegs.interrupt &= ~(1 << i);
	cpuRegs.dmastall &= ~(1 << i);
}

static __fi void TESTINT( u8 n, void (*callback)() )
{
	if( !(cpuRegs.interrupt & (1 << n)) ) return;

	if(CHECK_INSTANTDMAHACK || cpuTestCycle( cpuRegs.sCycle[n], cpuRegs.eCycle[n] ) )
	{
		cpuClearInt( n );
		callback();
	}
	else
		cpuSetNextEvent( cpuRegs.sCycle[n], cpuRegs.eCycle[n] );
}

static __fi bool _cpuTestInterrupts()
{

	if (!dmacRegs.ctrl.DMAE || (psHu8(DMAC_ENABLER+2) & 1))
	{
		return false;
	}

	eeRunInterruptScan = INT_RUNNING;

	while (eeRunInterruptScan == INT_RUNNING)
	{
		TESTINT(VU_MTVU_BUSY, MTVUInterrupt);
		TESTINT(DMAC_VIF1, vif1Interrupt);
		TESTINT(DMAC_GIF, gifInterrupt);
		TESTINT(DMAC_SIF0, EEsif0Interrupt);
		TESTINT(DMAC_SIF1, EEsif1Interrupt);

		if (cpuRegs.interrupt & ((1 << DMAC_VIF0) | (1 << DMAC_FROM_IPU) | (1 << DMAC_TO_IPU)
			| (1 << DMAC_FROM_SPR) | (1 << DMAC_TO_SPR) | (1 << DMAC_MFIFO_VIF) | (1 << DMAC_MFIFO_GIF)
			| (1 << VIF_VU0_FINISH) | (1 << VIF_VU1_FINISH) | (1 << IPU_PROCESS)))
		{
			TESTINT(DMAC_VIF0, vif0Interrupt);

			TESTINT(DMAC_FROM_IPU, ipu0Interrupt);
			TESTINT(DMAC_TO_IPU, ipu1Interrupt);
			TESTINT(IPU_PROCESS, ipuCMDProcess);

			TESTINT(DMAC_FROM_SPR, SPRFROMinterrupt);
			TESTINT(DMAC_TO_SPR, SPRTOinterrupt);

			TESTINT(DMAC_MFIFO_VIF, vifMFIFOInterrupt);
			TESTINT(DMAC_MFIFO_GIF, gifMFIFOInterrupt);

			TESTINT(VIF_VU0_FINISH, vif0VUFinish);
			TESTINT(VIF_VU1_FINISH, vif1VUFinish);
		}

		if (eeRunInterruptScan == INT_REQ_LOOP)
			eeRunInterruptScan = INT_RUNNING;
		else
			break;
	}

	eeRunInterruptScan = INT_NOT_RUNNING;

	if ((cpuRegs.interrupt & 0x1FFFF) & ~cpuRegs.dmastall)
		return true;
	else
		return false;
}

static __fi void _cpuTestTIMR()
{
	cpuRegs.CP0.n.Count += cpuRegs.cycle - cpuRegs.lastCOP0Cycle;
	cpuRegs.lastCOP0Cycle = cpuRegs.cycle;

	if ( (cpuRegs.CP0.n.Status.val & 0x8000) &&
		cpuRegs.CP0.n.Count >= cpuRegs.CP0.n.Compare && cpuRegs.CP0.n.Count < cpuRegs.CP0.n.Compare+1000 )
	{
		Console.WriteLn( Color_Magenta, "timr intr: %x, %x", cpuRegs.CP0.n.Count, cpuRegs.CP0.n.Compare);
		cpuException(0x808000, cpuRegs.branch);
	}
}

static __fi void _cpuTestPERF()
{

	COP0_UpdatePCCR();
}

static bool cpuIntsEnabled(int Interrupt)
{
	bool IntType = !!(cpuRegs.CP0.n.Status.val & Interrupt);

	return IntType && cpuRegs.CP0.n.Status.b.EIE && cpuRegs.CP0.n.Status.b.IE &&
		!cpuRegs.CP0.n.Status.b.EXL && (cpuRegs.CP0.n.Status.b.ERL == 0);
}

__fi void _cpuEventTest_Shared()
{
	eeEventTestIsActive = true;
	cpuRegs.nextEventCycle = cpuRegs.cycle + eeWaitCycles;
	cpuRegs.lastEventCycle = cpuRegs.cycle;

	uint mask = intcInterrupt() | dmacInterrupt();
	if (cpuIntsEnabled(mask))
		cpuException(mask, cpuRegs.branch);

	EEsCycle += cpuRegs.cycle - EEoCycle;
	EEoCycle = cpuRegs.cycle;

	if (EEsCycle > 0)
		iopEventAction = true;

	if (iopEventAction)
	{

		EEsCycle = psxCpu->ExecuteBlock(EEsCycle);

		iopEventAction = false;
	}

	iopEventTest();

	if (cpuTestCycle(nextStartCounter, nextDeltaCounter))
	{
		rcntUpdate();
		_cpuTestPERF();
	}

	_cpuTestTIMR();

	if (cpuRegs.interrupt)
	{
		if (CHECK_INSTANTDMAHACK && dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1) && (cpuRegs.interrupt & 0x1FFFF))
		{
			while ((cpuRegs.interrupt & 0x1FFFF) && _cpuTestInterrupts())
				;
		}
		else
			_cpuTestInterrupts();
	}

	CpuVU0->ExecuteBlock();
	CpuVU1->ExecuteBlock();

	const float mutiplier = static_cast<float>(PS2CLK) / static_cast<float>(PSXCLK);
	const int nextIopEventDeta = ((psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier);
	if (EEsCycle >= nextIopEventDeta)
	{

		cpuSetNextEventDelta(48);
	}
	else
	{
		cpuSetNextEventDelta(((psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier) - EEsCycle);
	}

	cpuSetNextEvent(nextStartCounter, nextDeltaCounter);

	eeEventTestIsActive = false;
}

__ri void cpuTestINTCInts()
{
	if (!cpuIntsEnabled(0x400))
		return;

	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) == 0)
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE;
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestDMACInts()
{
	if (!cpuIntsEnabled(0x800))
		return;

	if (((psHu16(0xe012) & psHu16(0xe010)) == 0) &&
		((psHu16(0xe010) & 0x8000) == 0))
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE;
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestTIMRInts()
{
	if ((cpuRegs.CP0.n.Status.val & 0x10007) == 0x10001)
	{
		_cpuTestPERF();
		_cpuTestTIMR();
	}
}

__fi void cpuTestHwInts()
{
	cpuTestINTCInts();
	cpuTestDMACInts();
	cpuTestTIMRInts();
}

__fi void CPU_SET_DMASTALL(EE_EventType n, bool set)
{
	if (set)
		cpuRegs.dmastall |= 1 << n;
	else
		cpuRegs.dmastall &= ~(1 << n);
}

__fi void CPU_INT( EE_EventType n, s32 ecycle)
{
	if (ecycle < 4 && !(cpuRegs.dmastall & (1 << n)) && eeRunInterruptScan != INT_NOT_RUNNING)
	{
		eeRunInterruptScan = INT_REQ_LOOP;
		cpuRegs.interrupt |= 1 << n;
		cpuRegs.sCycle[n] = cpuRegs.cycle;
		cpuRegs.eCycle[n] = 0;
		return;
	}

	if (CHECK_EETIMINGHACK && n < VIF_VU0_FINISH)
		ecycle = 8;

	cpuRegs.interrupt |= 1 << n;
	cpuRegs.sCycle[n] = cpuRegs.cycle;
	cpuRegs.eCycle[n] = ecycle;

	if (ecycle <= 28 && psxRegs.iopCycleEE > 0)
	{

		psxRegs.iopBreak += psxRegs.iopCycleEE;
		psxRegs.iopCycleEE = 0;
	}

	cpuSetNextEventDelta(cpuRegs.eCycle[n]);
}

int ParseArgumentString(u32 arg_block)
{
	if (!arg_block)
		return 0;

	int argc = 0;
	bool wasSpace = true;
	int args_len = strlen((char *)PSM(arg_block));
	for (int i = 0; i < args_len; i++)
	{
		char curchar = *(char *)PSM(arg_block + i);
		if (curchar == '\0')
			break;

		bool isSpace = (curchar == ' ');
		if (isSpace)
			memset(PSM(arg_block + i), 0, 1);
		else if (wasSpace)
		{
			if (argc < kMaxArgs)
			{
				g_argPtrs[argc] = arg_block + i;
				argc++;
			}
			else
			{
				Console.WriteLn("ParseArgumentString: Discarded additional arguments beyond the maximum of %d.", kMaxArgs);
				break;
			}
		}
		wasSpace = isSpace;
	}
#if DEBUG_LAUNCHARG
	Console.WriteLn("ParseArgumentString: Saving these strings:");
	for (int a = 0; a < argc; a++)
		Console.WriteLn("%p -> '%s'.", g_argPtrs[a], (char *)PSM(g_argPtrs[a]));
#endif
	return argc;
}

void eeloadHook()
{
	std::string elfname;
	int argc = cpuRegs.GPR.n.a0.SD[0];
	if (argc)
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook: EELOAD was called with %d arguments according to $a0 and %d according to vargs block:",
			argc, memRead32(cpuRegs.GPR.n.a1.UD[0] - 4));
		for (int a = 0; a < argc; a++)
			Console.WriteLn("argv[%d]: %p -> %p -> '%s'", a, cpuRegs.GPR.n.a1.UL[0] + (a * 4),
				memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)), (char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		if (argc > 1)
			elfname = (char*)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + 4));

		if (!EmuConfig.CurrentGameArgs.empty() && elfname == "rom0:PS2LOGO")
		{
			const char *argString = EmuConfig.CurrentGameArgs.c_str();
			Console.WriteLn("eeloadHook: Supplying launch argument(s) '%s' to module '%s'...", argString, elfname.c_str());

			u32 arg_ptr = 0;
			int arg_len = 0;
			for (int a = 0; a < argc; a++)
			{
				arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4));
				arg_len = strlen((char *)PSM(arg_ptr));
				memset(PSM(arg_ptr + arg_len), 0x20, 1);
			}
			strcpy((char *)PSM(arg_ptr + arg_len + 1), EmuConfig.CurrentGameArgs.c_str());
			u32 first_arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0]);
#if DEBUG_LAUNCHARG
			Console.WriteLn("eeloadHook: arg block is '%s'.", (char *)PSM(first_arg_ptr));
#endif
			argc = ParseArgumentString(first_arg_ptr);

			for (int a = 0; a < argc; a++)
				memWrite32(cpuRegs.GPR.n.a1.UD[0] + (a * 4), g_argPtrs[a]);
			cpuRegs.GPR.n.a0.SD[0] = argc;
#if DEBUG_LAUNCHARG
			Console.WriteLn("eeloadHook: New arguments are:");
			for (int a = 0; a < argc; a++)
				Console.WriteLn("argv[%d]: %p -> '%s'", a, memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)),
				(char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		}
	}
#if DEBUG_LAUNCHARG
	else
		Console.WriteLn("eeloadHook: EELOAD was called with no arguments.");
#endif

	if (VMManager::Internal::IsFastBootInProgress() && elfname.empty())
	{
		const std::string& elf_override = VMManager::Internal::GetELFOverride();
		if (!elf_override.empty())
		{
			elfname = fmt::format("host:{}", elf_override);
		}
		else
		{
			CDVDDiscType disc_type;
			std::string disc_elf;
			cdvdGetDiscInfo(nullptr, &disc_elf, nullptr, nullptr, &disc_type);
			if (disc_type == CDVDDiscType::PS2Disc)
			{
				elfname = std::move(disc_elf);
			}
			else
			{
				Console.Warning(fmt::format("Not allowing fast boot for non-PS2 ELF {}", disc_elf));
			}
		}

		if (!elfname.empty())
		{
			for (g_osdsys_str = EELOAD_START; g_osdsys_str < EELOAD_START + EELOAD_SIZE; g_osdsys_str += 8)
			{
				if (!strcmp((char*)PSM(g_osdsys_str), "rom0:OSDSYS"))
				{
					strcpy((char*)PSM(g_osdsys_str), elfname.c_str());
					break;
				}
			}
		}
		else
		{
			VMManager::Internal::DisableFastBoot();
			AllowParams1 = true;
			AllowParams2 = true;
		}
	}

	VMManager::Internal::ELFLoadingOnCPUThread(std::move(elfname));

	if (CHECK_EXTRAMEM)
	{
		vtlb_VMap(Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);

		vtlb_VMap(0x20000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
		vtlb_VMap(0x30000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
	}
}

void eeloadHook2()
{
	if (EmuConfig.CurrentGameArgs.empty())
		return;

	if (!g_osdsys_str)
	{
		Console.WriteLn("eeloadHook2: Called before \"rom0:OSDSYS\" was found by eeloadHook()!");
		return;
	}

	const char *argString = EmuConfig.CurrentGameArgs.c_str();
	Console.WriteLn("eeloadHook2: Supplying launch argument(s) '%s' to ELF '%s'.", argString, (char *)PSM(g_osdsys_str));

	int game_len = strlen((char *)PSM(g_osdsys_str));
	memset(PSM(g_osdsys_str + game_len), 0x20, 1);
	strcpy((char *)PSM(g_osdsys_str + game_len + 1), EmuConfig.CurrentGameArgs.c_str());
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: arg block is '%s'.", (char *)PSM(g_osdsys_str));
#endif
	int argc = ParseArgumentString(g_osdsys_str);

	uptr block_start = g_osdsys_str - (argc * 4);
	for (int a = 0; a < argc; a++)
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook2: Writing address %p to location %p.", g_argPtrs[a], block_start + (a * 4));
#endif
		memWrite32(block_start + (a * 4), g_argPtrs[a]);
	}

#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: Saving %d and %p in $a0 and $a1.", argc, block_start);
#endif
	cpuRegs.GPR.n.a0.SD[0] = argc;
	cpuRegs.GPR.n.a1.UD[0] = block_start;
}

inline bool isBranchOrJump(u32 addr)
{
	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	if ((opcode.flags == (IS_BRANCH | BRANCHTYPE_SYSCALL)) || (opcode.flags == (IS_BRANCH | BRANCHTYPE_ERET)))
		return false;

	return (opcode.flags & IS_BRANCH) != 0;
}

int isBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr))
		bpFlags += 1;

	if (isBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr+4))
		bpFlags += 2;

	return bpFlags;
}

int isMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (isBranchOrJump(addr))
		addr += 4;

	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}
