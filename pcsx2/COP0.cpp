// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "COP0.h"

__ri void cpuUpdateOperationMode()
{

}

void WriteCP0Status(u32 value)
{
	COP0_UpdatePCCR();
	cpuRegs.CP0.n.Status.val = value;
	cpuSetNextEventDelta(4);
}

void WriteCP0Config(u32 value)
{
	cpuRegs.CP0.n.Config = value & ~0xFC0;
	cpuRegs.CP0.n.Config |= 0x440;
}

static __fi bool PERF_ShouldCountEvent(uint evt)
{
	switch (evt)
	{

		case 1:
		case 2:
		case 3:
			return true;

		case 4:
		case 5:
		case 6:
			return false;

		case 7:
		case 8:
		case 9:
		case 10:
			return false;

		case 11:
			return false;

		case 12:
		case 13:
		case 14:
		case 15:
			return true;
	}

	return false;
}

void COP0_DiagnosticPCCR()
{
	if (cpuRegs.PERF.n.pccr.b.Event0 >= 7 && cpuRegs.PERF.n.pccr.b.Event0 <= 10)
		Console.Warning("PERF/PCR0 Unsupported Update Event Mode = 0x%x", cpuRegs.PERF.n.pccr.b.Event0);

	if (cpuRegs.PERF.n.pccr.b.Event1 >= 7 && cpuRegs.PERF.n.pccr.b.Event1 <= 10)
		Console.Warning("PERF/PCR1 Unsupported Update Event Mode = 0x%x", cpuRegs.PERF.n.pccr.b.Event1);
}
extern int branch;
__fi void COP0_UpdatePCCR()
{
	if (cpuRegs.CP0.n.Status.b.ERL || !cpuRegs.PERF.n.pccr.b.CTE)
	{
		cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
		cpuRegs.lastPERFCycle[1] = cpuRegs.lastPERFCycle[0];
		return;
	}

	if (cpuRegs.PERF.n.pccr.val & ((1 << (cpuRegs.CP0.n.Status.b.KSU + 2)) | (cpuRegs.CP0.n.Status.b.EXL << 1)))
	{

		if (PERF_ShouldCountEvent(cpuRegs.PERF.n.pccr.b.Event0))
		{
			u32 incr = cpuRegs.cycle - cpuRegs.lastPERFCycle[0];
			if (incr == 0)
				incr++;

			cpuRegs.PERF.n.pcr0 += incr;
			if ((cpuRegs.PERF.n.pcr0 & 0x80000000))
			{

			}
		}
	}

	if (cpuRegs.PERF.n.pccr.val & ((1 << (cpuRegs.CP0.n.Status.b.KSU + 12)) | (cpuRegs.CP0.n.Status.b.EXL << 11)))
	{

		if (PERF_ShouldCountEvent(cpuRegs.PERF.n.pccr.b.Event1))
		{
			u32 incr = cpuRegs.cycle - cpuRegs.lastPERFCycle[1];
			if (incr == 0)
				incr++;

			cpuRegs.PERF.n.pcr1 += incr;

			if ((cpuRegs.PERF.n.pcr1 & 0x80000000))
			{

			}
		}
	}
	cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
	cpuRegs.lastPERFCycle[1] = cpuRegs.cycle;
}

void MapTLB(const tlbs& t, int i)
{
	u32 mask, addr;
	u32 saddr, eaddr;

	COP0_LOG("MAP TLB %d: 0x%08X-> [0x%08X 0x%08X] S=%d G=%d ASID=%d Mask=0x%03X EntryLo0 PFN=%x EntryLo0 Cache=%x EntryLo1 PFN=%x EntryLo1 Cache=%x VPN2=%x",
		i, t.VPN2(), t.PFN0(), t.PFN1(), t.isSPR() >> 31, t.isGlobal(), t.EntryHi.ASID,
		t.Mask(), t.EntryLo0.PFN, t.EntryLo0.C, t.EntryLo1.PFN, t.EntryLo1.C, t.VPN2());

	if (t.isSPR())
	{
		if (t.VPN2() != 0x70000000)
			Console.Warning("COP0: Mapping Scratchpad to non-default address 0x%08X", t.VPN2());

		vtlb_VMapBuffer(t.VPN2(), eeMem->Scratch, Ps2MemSize::Scratch);
	}
	else
	{
		if (t.EntryLo0.V)
		{
			mask = ((~t.Mask()) << 1) & 0xfffff;
			saddr = t.VPN2() >> 12;
			eaddr = saddr + t.Mask() + 1;

			for (addr = saddr; addr < eaddr; addr++)
			{
				if ((addr & mask) == ((t.VPN2() >> 12) & mask))
				{
					memSetPageAddr(addr << 12, t.PFN0() + ((addr - saddr) << 12));
					Cpu->Clear(addr << 12, 0x400);
				}
			}
		}

		if (t.EntryLo1.V)
		{
			mask = ((~t.Mask()) << 1) & 0xfffff;
			saddr = (t.VPN2() >> 12) + t.Mask() + 1;
			eaddr = saddr + t.Mask() + 1;

			for (addr = saddr; addr < eaddr; addr++)
			{
				if ((addr & mask) == ((t.VPN2() >> 12) & mask))
				{
					memSetPageAddr(addr << 12, t.PFN1() + ((addr - saddr) << 12));
					Cpu->Clear(addr << 12, 0x400);
				}
			}
		}
	}
}

__inline u32 ConvertPageMask(const u32 PageMask)
{
	const u32 mask = std::popcount(PageMask >> 13);

	pxAssertMsg(!((mask & 1) || mask > 12), "Invalid page mask for this TLB entry. EE cache doesn't know what to do here.");

	return (1 << (12 + mask)) - 1;
}

void UnmapTLB(const tlbs& t, int i)
{
	u32 mask, addr;
	u32 saddr, eaddr;

	if (t.isSPR())
	{
		vtlb_VMapUnmap(t.VPN2(), 0x4000);
		return;
	}

	if (t.EntryLo0.V)
	{
		mask = ((~t.Mask()) << 1) & 0xfffff;
		saddr = t.VPN2() >> 12;
		eaddr = saddr + t.Mask() + 1;
		for (addr = saddr; addr < eaddr; addr++)
		{
			if ((addr & mask) == ((t.VPN2() >> 12) & mask))
			{
				memClearPageAddr(addr << 12);
				Cpu->Clear(addr << 12, 0x400);
			}
		}
	}

	if (t.EntryLo1.V)
	{
		mask = ((~t.Mask()) << 1) & 0xfffff;
		saddr = (t.VPN2() >> 12) + t.Mask() + 1;
		eaddr = saddr + t.Mask() + 1;
		for (addr = saddr; addr < eaddr; addr++)
		{
			if ((addr & mask) == ((t.VPN2() >> 12) & mask))
			{
				memClearPageAddr(addr << 12);
				Cpu->Clear(addr << 12, 0x400);
			}
		}
	}

	for (size_t i = 0; i < cachedTlbs.count; i++)
	{
		if (cachedTlbs.PFN0s[i] == t.PFN0() && cachedTlbs.PFN1s[i] == t.PFN1() && cachedTlbs.PageMasks[i] == ConvertPageMask(t.PageMask.UL))
		{
			for (size_t j = i; j < cachedTlbs.count - 1; j++)
			{
				cachedTlbs.CacheEnabled0[j] = cachedTlbs.CacheEnabled0[j + 1];
				cachedTlbs.CacheEnabled1[j] = cachedTlbs.CacheEnabled1[j + 1];
				cachedTlbs.PFN0s[j] = cachedTlbs.PFN0s[j + 1];
				cachedTlbs.PFN1s[j] = cachedTlbs.PFN1s[j + 1];
				cachedTlbs.PageMasks[j] = cachedTlbs.PageMasks[j + 1];
			}
			cachedTlbs.count--;
			break;
		}
	}
}

void WriteTLB(int i)
{
	tlb[i].PageMask.UL = cpuRegs.CP0.n.PageMask;
	tlb[i].EntryHi.UL = cpuRegs.CP0.n.EntryHi;
	tlb[i].EntryLo0.UL = cpuRegs.CP0.n.EntryLo0;
	tlb[i].EntryLo1.UL = cpuRegs.CP0.n.EntryLo1;

	if (tlb[i].isSPR())
	{
		tlb[i].EntryLo0.C = 3;
		tlb[i].EntryLo1.C = 3;
	}
	else
	{
		if (!tlb[i].EntryLo0.isValidCacheMode())
			tlb[i].EntryLo0.C = 2;
		if (!tlb[i].EntryLo1.isValidCacheMode())
			tlb[i].EntryLo1.C = 2;
	}

	if (!tlb[i].isSPR() && ((tlb[i].EntryLo0.V && tlb[i].EntryLo0.isCached()) || (tlb[i].EntryLo1.V && tlb[i].EntryLo1.isCached())))
	{
		const size_t idx = cachedTlbs.count;
		cachedTlbs.CacheEnabled0[idx] = tlb[i].EntryLo0.isCached() ? ~0 : 0;
		cachedTlbs.CacheEnabled1[idx] = tlb[i].EntryLo1.isCached() ? ~0 : 0;
		cachedTlbs.PFN1s[idx] = tlb[i].PFN1();
		cachedTlbs.PFN0s[idx] = tlb[i].PFN0();
		cachedTlbs.PageMasks[idx] = ConvertPageMask(tlb[i].PageMask.UL);

		cachedTlbs.count++;
	}

	MapTLB(tlb[i], i);
}

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {
namespace COP0 {

	void TLBR()
	{
		COP0_LOG("COP0_TLBR %d:%x,%x,%x,%x",
			cpuRegs.CP0.n.Index, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);

		const u8 i = cpuRegs.CP0.n.Index & 0x3f;

		if (i > 47)
		{
			Console.Warning("TLBR with index > 47! (%d)", i);
			return;
		}

		cpuRegs.CP0.n.PageMask = tlb[i].PageMask.Mask << 13;
		cpuRegs.CP0.n.EntryHi = tlb[i].EntryHi.UL & ~((tlb[i].PageMask.Mask << 13) | 0x1f00);
		cpuRegs.CP0.n.EntryLo0 = tlb[i].EntryLo0.UL & ~(0xFC000000) & ~1;
		cpuRegs.CP0.n.EntryLo1 = tlb[i].EntryLo1.UL & ~(0x7C000000) & ~1;
		cpuRegs.CP0.n.EntryLo0 |= (tlb[i].EntryLo0.UL & 1) & (tlb[i].EntryLo1.UL & 1);
		cpuRegs.CP0.n.EntryLo1 |= (tlb[i].EntryLo0.UL & 1) & (tlb[i].EntryLo1.UL & 1);
	}

	void TLBWI()
	{
		const u8 j = cpuRegs.CP0.n.Index & 0x3f;

		if (j > 47)
		{
			Console.Warning("TLBWI with index > 47! (%d)", j);
			return;
		}

		COP0_LOG("COP0_TLBWI %d:%x,%x,%x,%x",
			cpuRegs.CP0.n.Index, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);

		UnmapTLB(tlb[j], j);
		WriteTLB(j);
	}

	void TLBWR()
	{
		const u8 j = cpuRegs.CP0.n.Random & 0x3f;

		if (j > 47)
		{
			Console.Warning("TLBWR with random > 47! (%d)", j);
			return;
		}

		DevCon.Warning("COP0_TLBWR %d:%x,%x,%x,%x\n",
			cpuRegs.CP0.n.Random, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);

		UnmapTLB(tlb[j], j);
		WriteTLB(j);
	}

	void TLBP()
	{
		int i;

		union
		{
			struct
			{
				u32 VPN2 : 19;
				u32 VPN2X : 2;
				u32 G : 3;
				u32 ASID : 8;
			} s;
			u32 u;
		} EntryHi32;

		EntryHi32.u = cpuRegs.CP0.n.EntryHi;

		cpuRegs.CP0.n.Index = 0xFFFFFFFF;
		for (i = 0; i < 48; i++)
		{
			if (tlb[i].VPN2() == ((~tlb[i].Mask()) & (EntryHi32.s.VPN2)) && ((tlb[i].isGlobal()) || ((tlb[i].EntryHi.ASID & 0xff) == EntryHi32.s.ASID)))
			{
				cpuRegs.CP0.n.Index = i;
				break;
			}
		}
		if (cpuRegs.CP0.n.Index == 0xFFFFFFFF)
			cpuRegs.CP0.n.Index = 0x80000000;
	}

	void MFC0()
	{
		if ((_Rd_ != 9) && !_Rt_)
			return;

		switch (_Rd_)
		{
			case 12:
				cpuRegs.GPR.r[_Rt_].SD[0] = (s32)(cpuRegs.CP0.r[_Rd_] & 0xf0c79c1f);
				break;

			case 25:
				if (0 == (_Imm_ & 1))
				{
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pccr.val;
				}
				else if (0 == (_Imm_ & 2))
				{
					COP0_UpdatePCCR();
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pcr0;
				}
				else
				{
					COP0_UpdatePCCR();
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pcr1;
				}
				break;

			case 24:
				COP0_LOG("MFC0 Breakpoint debug Registers code = %x", cpuRegs.code & 0x3FF);
				break;

			case 9:
			{
				s64 incr = cpuRegs.cycle - cpuRegs.lastCOP0Cycle;
				if (incr == 0)
					incr++;
				cpuRegs.CP0.n.Count += incr;
				cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
				if (!_Rt_)
					break;
			}
				[[fallthrough]];

			default:
				cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.CP0.r[_Rd_];
		}
	}

	void MTC0()
	{
		switch (_Rd_)
		{
			case 9:
				cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
				cpuRegs.CP0.r[9] = cpuRegs.GPR.r[_Rt_].UL[0];
				break;

			case 12:
				WriteCP0Status(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 16:
				WriteCP0Config(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 24:
				COP0_LOG("MTC0 Breakpoint debug Registers code = %x", cpuRegs.code & 0x3FF);
				break;

			case 25:
				if (0 == (_Imm_ & 1))
				{
					if (0 != (_Imm_ & 0x3E))
						break;
					COP0_UpdatePCCR();
					cpuRegs.PERF.n.pccr.val = cpuRegs.GPR.r[_Rt_].UL[0];
					COP0_DiagnosticPCCR();
				}
				else if (0 == (_Imm_ & 2))
				{
					cpuRegs.PERF.n.pcr0 = cpuRegs.GPR.r[_Rt_].UL[0];
					cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
				}
				else
				{
					cpuRegs.PERF.n.pcr1 = cpuRegs.GPR.r[_Rt_].UL[0];
					cpuRegs.lastPERFCycle[1] = cpuRegs.cycle;
				}
				break;

			default:
				cpuRegs.CP0.r[_Rd_] = cpuRegs.GPR.r[_Rt_].UL[0];
				break;
		}
	}

	int CPCOND0()
	{
		return (((dmacRegs.stat.CIS | ~dmacRegs.pcr.CPC) & 0x3FF) == 0x3ff);
	}

	void BC0F()
	{
		if (CPCOND0() == 0)
			intDoBranch(_BranchTarget_);
	}

	void BC0T()
	{
		if (CPCOND0() == 1)
			intDoBranch(_BranchTarget_);
	}

	void BC0FL()
	{
		if (CPCOND0() == 0)
			intDoBranch(_BranchTarget_);
		else
			cpuRegs.pc += 4;
	}

	void BC0TL()
	{
		if (CPCOND0() == 1)
			intDoBranch(_BranchTarget_);
		else
			cpuRegs.pc += 4;
	}

	void ERET()
	{
#ifdef ENABLE_VTUNE
		const u32 million = 1000 * 1000;
		static u32 vtune = 0;
		vtune++;

		if (vtune > 30 * million)
		{
			Console.WriteLn("VTUNE: quick_exit");
			std::quick_exit(EXIT_SUCCESS);
		}
		else if (!(vtune % million))
		{
			Console.WriteLn("VTUNE: ERET was called %uM times", vtune / million);
		}

#endif

		if (cpuRegs.CP0.n.Status.b.ERL)
		{
			cpuRegs.pc = cpuRegs.CP0.n.ErrorEPC;
			cpuRegs.CP0.n.Status.b.ERL = 0;
		}
		else
		{
			cpuRegs.pc = cpuRegs.CP0.n.EPC;
			cpuRegs.CP0.n.Status.b.EXL = 0;
		}
		cpuUpdateOperationMode();
		cpuSetNextEventDelta(4);
		intSetBranch();
	}

	void DI()
	{
		if (cpuRegs.CP0.n.Status.b._EDI || cpuRegs.CP0.n.Status.b.EXL ||
			cpuRegs.CP0.n.Status.b.ERL || (cpuRegs.CP0.n.Status.b.KSU == 0))
		{
			cpuRegs.CP0.n.Status.b.EIE = 0;
		}
	}

	void EI()
	{
		if (cpuRegs.CP0.n.Status.b._EDI || cpuRegs.CP0.n.Status.b.EXL ||
			cpuRegs.CP0.n.Status.b.ERL || (cpuRegs.CP0.n.Status.b.KSU == 0))
		{
			cpuRegs.CP0.n.Status.b.EIE = 1;
			cpuSetNextEventDelta(4);
		}
	}

}
}
}
}
