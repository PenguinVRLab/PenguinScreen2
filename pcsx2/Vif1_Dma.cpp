// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "VUmicro.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"

u32 g_vif1Cycles = 0;

__fi void vif1FLUSH()
{
	if (VU0.VI[REG_VPU_STAT].UL & 0x500)
	{
		vif1.waitforvu = true;
		vif1.vifstalled.enabled = VifStallEnable(vif1ch);
		vif1.vifstalled.value = VIF_TIMING_BREAK;
		vif1Regs.stat.VEW = true;
	}
}

void vif1TransferToMemory()
{
	u128* pMem = (u128*)dmaGetAddr(vif1ch.madr, false);

	if (pMem == nullptr)
	{
		Console.WriteLn("Vif1 Tag BUSERR");
		dmacRegs.stat.BEIS = true;
		vif1Regs.stat.FQC = 0;

		vif1ch.qwc = 0;
		vif1.done = true;
		CPU_INT(DMAC_VIF1, 0);
		return;
	}

	const u32 size = std::min(vif1.GSLastDownloadSize, (u32)vif1ch.qwc);

#ifdef PCSX2_DEVBUILD
	if (size)
	{
		Gif_Path& p1 = gifUnit.gifPath[GIF_PATH_1];
		Gif_Path& p2 = gifUnit.gifPath[GIF_PATH_2];
		Gif_Path& p3 = gifUnit.gifPath[GIF_PATH_3];
		pxAssert(p1.isDone() || !p1.gifTag.isValid);
		pxAssert(p2.isDone() || !p2.gifTag.isValid);
		pxAssert(p3.isDone() || !p3.gifTag.isValid);
	}
#endif

	MTGS::InitAndReadFIFO(reinterpret_cast<u8*>(pMem), size);

	g_vif1Cycles += size * 2;
	vif1ch.madr += size * 16;
	if (vif1.GSLastDownloadSize >= vif1ch.qwc)
	{
		vif1.GSLastDownloadSize -= vif1ch.qwc;
		vif1Regs.stat.FQC = std::min((u32)16, vif1.GSLastDownloadSize);
		vif1ch.qwc = 0;
	}
	else
	{
		vif1Regs.stat.FQC = 0;
		vif1ch.qwc -= vif1.GSLastDownloadSize;
		vif1.GSLastDownloadSize = 0;
		DevCon.Warning("QWC left on VIF FIFO Reverse");
	}
}

bool _VIF1chain()
{
	u32* pMem;

	if (vif1ch.qwc == 0)
	{
		vif1.inprogress &= ~1;
		vif1.irqoffset.value = 0;
		vif1.irqoffset.enabled = false;
		return true;
	}

	if (vif1.dmamode == VIF_NORMAL_TO_MEM_MODE)
	{
		vif1TransferToMemory();
		vif1.inprogress &= ~1;
		return true;
	}

	pMem = (u32*)dmaGetAddr(vif1ch.madr, !vif1ch.chcr.DIR);
	if (pMem == nullptr)
	{
		vif1.cmd = 0;
		vif1.tag.size = 0;
		vif1ch.qwc = 0;
		return true;
	}

	VIF_LOG("VIF1chain size=%d, madr=%lx, tadr=%lx",
		vif1ch.qwc, vif1ch.madr, vif1ch.tadr);

	if (vif1.irqoffset.enabled)
		return VIF1transfer(pMem + vif1.irqoffset.value, vif1ch.qwc * 4 - vif1.irqoffset.value, false);
	else
		return VIF1transfer(pMem, vif1ch.qwc * 4, false);
}

__fi void vif1SetupTransfer()
{
	tDMA_TAG* ptag;

	ptag = dmaGetAddr(vif1ch.tadr, false);

	if (!(vif1ch.transfer("Vif1 Tag", ptag)))
		return;

	vif1ch.madr = ptag[1]._u32;
	g_vif1Cycles += 1;
	vif1.inprogress &= ~1;

	VIF_LOG("VIF1 Tag %8.8x_%8.8x size=%d, id=%d, madr=%lx, tadr=%lx",
		ptag[1]._u32, ptag[0]._u32, vif1ch.qwc, ptag->ID, vif1ch.madr, vif1ch.tadr);

	if (!vif1.done && ((dmacRegs.ctrl.STD == STD_VIF1) && (ptag->ID == TAG_REFS)))
	{
		if ((vif1ch.madr + vif1ch.qwc * 16) > dmacRegs.stadr.ADDR)
		{
			hwDmacIrq(DMAC_STALL_SIS);
			CPU_SET_DMASTALL(DMAC_VIF1, true);
			return;
		}
	}

	if (vif1ch.chcr.TTE)
	{

		bool ret;

		alignas(16) static u128 masked_tag;

		masked_tag._u64[0] = 0;
		masked_tag._u64[1] = *((u64*)ptag + 1);

		VIF_LOG("\tVIF1 SrcChain TTE=1, data = 0x%08x.%08x", masked_tag._u32[3], masked_tag._u32[2]);

		if (vif1.irqoffset.enabled)
		{
			ret = VIF1transfer((u32*)&masked_tag + vif1.irqoffset.value, 4 - vif1.irqoffset.value, true);
		}
		else
		{
			vif1.irqoffset.value = 2;
			vif1.irqoffset.enabled = true;
			ret = VIF1transfer((u32*)&masked_tag + 2, 2, true);
		}

		if (!ret && vif1.irqoffset.enabled)
		{
			vif1.inprogress &= ~1;
			vif1ch.qwc = 0;
			return;
		}
	}
	vif1.irqoffset.value = 0;
	vif1.irqoffset.enabled = false;

	vif1.done |= hwDmacSrcChainWithStack(vif1ch, ptag->ID);

	if (vif1ch.qwc > 0)
		vif1.inprogress |= 1;

	if (vif1ch.chcr.TIE && ptag->IRQ)
	{
		VIF_LOG("dmaIrq Set");

		vif1.done = true;
		return;
	}
}

__fi void vif1VUFinish()
{
	while (!THREAD_VU1 && (VU0.VI[REG_VPU_STAT].UL & 0x100))
	{
		const s64 cycle_diff = static_cast<int>(cpuRegs.cycle - VU1.cycle);

		if ((EmuConfig.Gamefixes.VUSyncHack && cycle_diff < VU1.nextBlockCycles) || cycle_diff <= 0)
			break;

		CpuVU1->ExecuteBlock();
	}

	if (VU0.VI[REG_VPU_STAT].UL & 0x500)
	{
		vu1Thread.Get_MTVUChanges();

		if (THREAD_VU1 && !INSTANT_VU1 && (VU0.VI[REG_VPU_STAT].UL & 0x100))
			CPU_INT(VIF_VU1_FINISH, cpuGetCycles(VU_MTVU_BUSY));
		else
			CPU_INT(VIF_VU1_FINISH, 128);
		CPU_SET_DMASTALL(VIF_VU1_FINISH, true);
		return;
	}

	if (VU0.VI[REG_VPU_STAT].UL & 0x100)
	{
		u64 _cycles = VU1.cycle;
		vu1Finish(false);
		if (THREAD_VU1 && !INSTANT_VU1 && (VU0.VI[REG_VPU_STAT].UL & 0x100))
			CPU_INT(VIF_VU1_FINISH, cpuGetCycles(VU_MTVU_BUSY));
		else
			CPU_INT(VIF_VU1_FINISH, VU1.cycle - _cycles);
		CPU_SET_DMASTALL(VIF_VU1_FINISH, true);
		return;
	}

	vif1Regs.stat.VEW = false;
	VIF_LOG("VU1 finished");

	if (vif1.waitforvu)
	{
		vif1.waitforvu = false;
		if ((cpuRegs.interrupt & ((1 << DMAC_VIF1) | (1 << DMAC_MFIFO_VIF))) == 0 && vif1ch.chcr.STR && !vif1Regs.stat.test(VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			if (dmacRegs.ctrl.MFD == MFD_VIF1)
				vifMFIFOInterrupt();
			else
				vif1Interrupt();
		}
	}

}

__fi void vif1Interrupt()
{
	VIF_LOG("vif1Interrupt: %8.8llx chcr %x, done %x, qwc %x", cpuRegs.cycle, vif1ch.chcr._u32, vif1.done, vif1ch.qwc);

	g_vif1Cycles = 0;

	if (gifRegs.stat.APATH == 2 && gifUnit.gifPath[GIF_PATH_2].isDone())
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH = 0;
		vif1Regs.stat.VGW = false;

		if (gifUnit.checkPaths(1, 0, 1))
			gifUnit.Execute(false, true);
	}
	if (dmacRegs.ctrl.MFD == MFD_VIF1)
	{
		if (vif1ch.chcr.MOD == NORMAL_MODE)
			Console.WriteLn("MFIFO mode is normal (which isn't normal here)! %x", vif1ch.chcr._u32);
		vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
		vifMFIFOInterrupt();
		return;
	}

	if (vif1ch.chcr.DIR)
	{
		const bool isDirect = (vif1.cmd & 0x7f) == 0x50;
		const bool isDirectHL = (vif1.cmd & 0x7f) == 0x51;
		if ((isDirect && !gifUnit.CanDoPath2()) || (isDirectHL && !gifUnit.CanDoPath2HL()))
		{
			GUNIT_WARN("vif1Interrupt() - Waiting for Path 2 to be ready");
			CPU_INT(DMAC_VIF1, 128);
			if (gifRegs.stat.APATH == 3)
				vif1Regs.stat.VGW = 1;
			CPU_SET_DMASTALL(DMAC_VIF1, true);
			return;
		}
		vif1Regs.stat.VGW = 0;
		vif1Regs.stat.FQC = std::min(vif1ch.qwc, (u32)16);
	}

	if (vif1.waitforvu)
	{
		CPU_INT(VIF_VU1_FINISH, std::max(16, cpuGetCycles(VU_MTVU_BUSY)));
		CPU_SET_DMASTALL(DMAC_VIF1, true);
		return;
	}

	if (vif1Regs.stat.VGW)
	{
		CPU_SET_DMASTALL(DMAC_VIF1, true);
		return;
	}

	if (!vif1ch.chcr.STR)
	{
		Console.WriteLn("Vif1 running when CHCR == %x", vif1ch.chcr._u32);
		return;
	}

	if (vif1.irq && vif1.vifstalled.enabled && vif1.vifstalled.value == VIF_IRQ_STALL)
	{
		VIF_LOG("VIF IRQ Firing");
		if (!vif1Regs.stat.ER1)
			vif1Regs.stat.INT = true;

		if (((vif1Regs.code >> 24) & 0x7f) != 0x7)
		{
			vif1Regs.stat.VIS = true;
		}

		hwIntcIrq(VIF1intc);
		--vif1.irq;

		if (vif1Regs.stat.test(VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{

			vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
			if ((vif1ch.qwc > 0 || !vif1.done) && !CHECK_VIF1STALLHACK)
			{
				vif1Regs.stat.VPS = VPS_DECODING;
				VIF_LOG("VIF1 Stalled");
				CPU_SET_DMASTALL(DMAC_VIF1, true);
				return;
			}
		}
	}

	vif1.vifstalled.enabled = false;

	if (vif1.cmd)
	{
		if (vif1.done && (vif1ch.qwc == 0))
			vif1Regs.stat.VPS = VPS_WAITING;
	}
	else
	{
		vif1Regs.stat.VPS = VPS_IDLE;
	}

	if (vif1.inprogress & 0x1)
	{
		_VIF1chain();
		if (vif1ch.chcr.DIR)
			vif1Regs.stat.FQC = std::min(vif1ch.qwc, (u32)16);

		if (!(vif1Regs.stat.VGW && gifUnit.gifPath[GIF_PATH_3].state != GIF_PATH_IDLE))
		{
			if (vif1.waitforvu)
			{
				CPU_INT(DMAC_VIF1, std::max(static_cast<int>(g_vif1Cycles), cpuGetCycles(VU_MTVU_BUSY)));
			}
			else
				CPU_INT(DMAC_VIF1, g_vif1Cycles);
		}
		return;
	}

	if (!vif1.done)
	{

		if (!(dmacRegs.ctrl.DMAE) || vif1Regs.stat.VSS)
		{
			return;
		}

		if ((vif1.inprogress & 0x1) == 0)
			vif1SetupTransfer();
		if (vif1ch.chcr.DIR)
			vif1Regs.stat.FQC = std::min(vif1ch.qwc, (u32)16);

		if (!(vif1Regs.stat.VGW && gifUnit.gifPath[GIF_PATH_3].state != GIF_PATH_IDLE))
		{
			if (vif1.waitforvu)
			{
				CPU_INT(DMAC_VIF1, std::max(static_cast<int>(g_vif1Cycles), cpuGetCycles(VU_MTVU_BUSY)));
			}
			else
				CPU_INT(DMAC_VIF1, g_vif1Cycles);
		}
		return;
	}

	if (vif1.vifstalled.enabled && vif1.done)
	{
		DevCon.WriteLn("VIF1 looping on stall at end\n");
		CPU_INT(DMAC_VIF1, 0);
		CPU_SET_DMASTALL(DMAC_VIF1, true);
		return;
	}
#ifdef PCSX2_DEVBUILD
	if (vif1ch.qwc > 0)
		DevCon.WriteLn("VIF1 Ending with %x QWC left", vif1ch.qwc);
	if (vif1.cmd != 0)
		DevCon.WriteLn("vif1.cmd still set %x tag size %x", vif1.cmd, vif1.tag.size);
#endif

	if ((vif1ch.chcr.DIR == VIF_NORMAL_TO_MEM_MODE) && vif1.GSLastDownloadSize <= 16)
	{
		gifRegs.stat.OPH = false;
	}

	if (vif1ch.chcr.DIR)
		vif1Regs.stat.FQC = std::min(vif1ch.qwc, (u32)16);

	vif1ch.chcr.STR = false;
	vif1.vifstalled.enabled = false;
	vif1.irqoffset.enabled = false;
	if (vif1.queued_program)
		vifExecQueue(1);
	g_vif1Cycles = 0;
	VIF_LOG("VIF1 DMA End");
	hwDmacIrq(DMAC_VIF1);
	CPU_SET_DMASTALL(DMAC_VIF1, false);
}

void dmaVIF1()
{
	VIF_LOG("dmaVIF1 chcr = %lx, madr = %lx, qwc  = %lx\n"
			"        tadr = %lx, asr0 = %lx, asr1 = %lx",
		vif1ch.chcr._u32, vif1ch.madr, vif1ch.qwc,
		vif1ch.tadr, vif1ch.asr0, vif1ch.asr1);

	g_vif1Cycles = 0;
	vif1.inprogress = 0;
	CPU_SET_DMASTALL(DMAC_VIF1, false);

	if (vif1ch.qwc > 0)
	{

		if (vif1ch.chcr.MOD == CHAIN_MODE && vif1ch.chcr.DIR)
		{
			vif1.dmamode = VIF_CHAIN_MODE;

			if ((vif1ch.chcr.tag().ID == TAG_REFE) || (vif1ch.chcr.tag().ID == TAG_END) || (vif1ch.chcr.tag().IRQ && vif1ch.chcr.TIE))
			{
				vif1.done = true;
			}
			else
			{
				vif1.done = false;
			}
		}
		else
		{
			if (dmacRegs.ctrl.STD == STD_VIF1)
				Console.WriteLn("DMA Stall Control on VIF1 normal not implemented - Report which game to PCSX2 Team");

			if (vif1ch.chcr.DIR)
				vif1.dmamode = VIF_NORMAL_FROM_MEM_MODE;
			else
				vif1.dmamode = VIF_NORMAL_TO_MEM_MODE;

			if (vif1.irqoffset.enabled && !vif1.done)
				DevCon.Warning("Warning! VIF1 starting a Normal transfer with vif offset set (Possible force stop?)");
			vif1.done = true;
		}

		vif1.inprogress |= 1;
	}
	else
	{
		vif1.inprogress &= ~0x1;
		vif1.dmamode = VIF_CHAIN_MODE;
		vif1.done = false;
	}

	if (vif1ch.chcr.DIR)
		vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);

	if (!vif1ch.chcr.DIR || !vif1Regs.stat.test(VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		CPU_INT(DMAC_VIF1, 4);
}
