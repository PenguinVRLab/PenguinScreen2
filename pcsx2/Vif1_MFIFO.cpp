// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Vif.h"
#include "Gif_Unit.h"
#include "Vif_Dma.h"

static u32 qwctag(u32 mask)
{
	return (dmacRegs.rbor.ADDR + (mask & dmacRegs.rbsr.RMSK));
}

static u32 QWCinVIFMFIFO(u32 DrainADDR, u32 qwc)
{
	u32 ret;

	if (DrainADDR <= spr0ch.madr)
	{
		ret = (spr0ch.madr - DrainADDR) >> 4;
	}
	else
	{
		const u32 limit = dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16;
		ret = ((spr0ch.madr - dmacRegs.rbor.ADDR) + (limit - DrainADDR)) >> 4;
	}

	VIF_LOG("VIF MFIFO Requesting %x QWC of %x Available from the MFIFO Base %x MFIFO Top %x, SPR MADR %x Drain %x", qwc, ret, dmacRegs.rbor.ADDR, dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16, spr0ch.madr, DrainADDR);

	return ret;
}
static __fi bool mfifoVIF1rbTransfer()
{
	const u32 msize = dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16;
	const u32 mfifoqwc = std::min(QWCinVIFMFIFO(vif1ch.madr, vif1ch.qwc), vif1ch.qwc);
	u32* src;
	bool ret;

	if (mfifoqwc == 0)
	{
		DevCon.Warning("VIF MFIFO no QWC before transfer (in transfer function, bit late really)");
		return true;
	}

	if ((vif1ch.madr + (mfifoqwc << 4)) > (msize))
	{
		const int s1 = ((msize)-vif1ch.madr) >> 2;

		VIF_LOG("Split MFIFO");

		vif1ch.madr = qwctag(vif1ch.madr);

		src = (u32*)PSM(vif1ch.madr);
		if (src == nullptr)
			return false;

		if (vif1.irqoffset.enabled)
			ret = VIF1transfer(src + vif1.irqoffset.value, s1 - vif1.irqoffset.value);
		else
			ret = VIF1transfer(src, s1);

		if (ret)
		{
			if (vif1.irqoffset.value != 0)
				DevCon.Warning("VIF1 MFIFO Offest != 0! vifoffset=%x", vif1.irqoffset.value);
			vif1ch.tadr = qwctag(vif1ch.tadr);
			vif1ch.madr = qwctag(vif1ch.madr);

			src = (u32*)PSM(vif1ch.madr);
			if (src == nullptr)
				return false;
			VIF1transfer(src, ((mfifoqwc << 2) - s1));
		}
	}
	else
	{
		VIF_LOG("Direct MFIFO");

		src = (u32*)PSM(vif1ch.madr);
		if (src == nullptr)
			return false;

		if (vif1.irqoffset.enabled)
			ret = VIF1transfer(src + vif1.irqoffset.value, mfifoqwc * 4 - vif1.irqoffset.value);
		else
			ret = VIF1transfer(src, mfifoqwc << 2);
	}
	return ret;
}

static __fi void mfifo_VIF1chain()
{
	if (vif1ch.qwc == 0)
	{
		vif1.inprogress &= ~1;
		return;
	}

	if (vif1ch.madr >= dmacRegs.rbor.ADDR &&
		vif1ch.madr < (dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16u))
	{
		if (QWCinVIFMFIFO(vif1ch.madr, vif1ch.qwc) == 0)
		{
			VIF_LOG("VIF MFIFO Empty before transfer");
			vif1.inprogress |= 0x10;
			g_vif1Cycles += 4;
			return;
		}

		mfifoVIF1rbTransfer();
		vif1ch.madr = qwctag(vif1ch.madr);
		vif1ch.tadr = vif1ch.madr;
	}
	else
	{
		tDMA_TAG* pMem = dmaGetAddr(vif1ch.madr, !vif1ch.chcr.DIR);
		VIF_LOG("Non-MFIFO Location");

		if (pMem == nullptr)
			return;

		if (vif1.irqoffset.enabled)
			VIF1transfer((u32*)pMem + vif1.irqoffset.value, vif1ch.qwc * 4 - vif1.irqoffset.value);
		else
			VIF1transfer((u32*)pMem, vif1ch.qwc << 2);
	}
}

void mfifoVifMaskMem(int id)
{
	switch (id)
	{
		case TAG_CNT:
		case TAG_NEXT:
		case TAG_CALL:
		case TAG_RET:
		case TAG_END:
			if (vif1ch.madr < dmacRegs.rbor.ADDR)
			{
				vif1ch.madr = qwctag(vif1ch.madr);
			}
			if (vif1ch.madr > (dmacRegs.rbor.ADDR + static_cast<u32>(dmacRegs.rbsr.RMSK)))
			{
				vif1ch.madr = qwctag(vif1ch.madr);
			}
			break;
		default:
			break;
	}
}

void mfifoVIF1transfer()
{
	tDMA_TAG* ptag;

	g_vif1Cycles = 0;

	if (vif1ch.qwc == 0)
	{
		if (QWCinVIFMFIFO(vif1ch.tadr, 1) == 0)
		{
			VIF_LOG("VIF MFIFO Empty before tag");
			vif1.inprogress |= 0x10;
			g_vif1Cycles += 4;
			return;
		}

		vif1ch.tadr = qwctag(vif1ch.tadr);
		ptag = dmaGetAddr(vif1ch.tadr, false);

		if (dmacRegs.ctrl.STD == STD_VIF1 && (ptag->ID == TAG_REFS))
		{
			Console.WriteLn("VIF MFIFO DMA Stall not implemented - Report which game to PCSX2 Team");
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
				return;
			}
			g_vif1Cycles += 2;
		}

		vif1.irqoffset.value = 0;
		vif1.irqoffset.enabled = false;

		vif1ch.unsafeTransfer(ptag);

		vif1ch.madr = ptag[1]._u32;

		VIF_LOG("dmaChain %8.8x_%8.8x size=%d, id=%d, madr=%lx, tadr=%lx spr0 madr = %x",
			ptag[1]._u32, ptag[0]._u32, vif1ch.qwc, ptag->ID, vif1ch.madr, vif1ch.tadr, spr0ch.madr);

		vif1.done |= hwDmacSrcChainWithStack(vif1ch, ptag->ID);

		mfifoVifMaskMem(ptag->ID);

		if (vif1ch.chcr.TIE && ptag->IRQ)
		{
			VIF_LOG("dmaIrq Set");
			vif1.done = true;
		}

		vif1ch.tadr = qwctag(vif1ch.tadr);

		if (vif1ch.qwc > 0)
			vif1.inprogress |= 1;
	}
	else
	{
		DevCon.Warning("Vif MFIFO QWC not 0 on tag");
	}


	VIF_LOG("mfifoVIF1transfer end %x madr %x, tadr %x", vif1ch.chcr._u32, vif1ch.madr, vif1ch.tadr);
}

void vifMFIFOInterrupt()
{
	g_vif1Cycles = 0;
	VIF_LOG("vif mfifo interrupt");

	if (dmacRegs.ctrl.MFD != MFD_VIF1)
	{
		vif1Interrupt();
		return;
	}

	if (gifRegs.stat.APATH == 2 && gifUnit.gifPath[1].isDone())
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH = 0;

		if (gifUnit.checkPaths(1, 0, 1))
			gifUnit.Execute(false, true);
	}

	if (vif1ch.chcr.DIR)
	{
		const bool isDirect = (vif1.cmd & 0x7f) == 0x50;
		const bool isDirectHL = (vif1.cmd & 0x7f) == 0x51;
		if ((isDirect && !gifUnit.CanDoPath2()) || (isDirectHL && !gifUnit.CanDoPath2HL()))
		{
			GUNIT_WARN("vifMFIFOInterrupt() - Waiting for Path 2 to be ready");
			CPU_INT(DMAC_MFIFO_VIF, 128);
			CPU_SET_DMASTALL(DMAC_MFIFO_VIF, true);
			return;
		}
	}
	if (vif1.waitforvu)
	{
		CPU_INT(VIF_VU1_FINISH, std::max(16, cpuGetCycles(VU_MTVU_BUSY)));
		CPU_SET_DMASTALL(DMAC_MFIFO_VIF, true);
		return;
	}

	if (vif1.irq && vif1.vifstalled.enabled && vif1.vifstalled.value == VIF_IRQ_STALL)
	{
		VIF_LOG("VIF MFIFO Code Interrupt detected");
		vif1Regs.stat.INT = true;

		if (((vif1Regs.code >> 24) & 0x7f) != 0x7)
		{
			vif1Regs.stat.VIS = true;
		}

		hwIntcIrq(INTC_VIF1);
		--vif1.irq;

		if (vif1Regs.stat.test(VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
			VIF_LOG("VIF1 MFIFO Stalled qwc = %x done = %x inprogress = %x", vif1ch.qwc, vif1.done, vif1.inprogress & 0x10);
			if ((vif1ch.qwc > 0 || !vif1.done))
			{
				vif1Regs.stat.VPS = VPS_DECODING;
				VIF_LOG("VIF1 MFIFO Stalled");
				CPU_SET_DMASTALL(DMAC_MFIFO_VIF, true);
				return;
			}
		}
	}

	if (vif1.cmd)
	{
		if (vif1.done && vif1ch.qwc == 0)
			vif1Regs.stat.VPS = VPS_WAITING;
	}
	else
	{
		vif1Regs.stat.VPS = VPS_IDLE;
	}

	if (vif1.inprogress & 0x10)
	{
		FireMFIFOEmpty();
		CPU_SET_DMASTALL(DMAC_MFIFO_VIF, true);
		return;
	}

	vif1.vifstalled.enabled = false;

	if (!vif1.done || vif1ch.qwc)
	{
		switch (vif1.inprogress & 1)
		{
			case 0:
				mfifoVIF1transfer();
				vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
				[[fallthrough]];

			case 1:
				if (vif1.inprogress & 0x1)
					mfifo_VIF1chain();
				if (!(vif1Regs.stat.VGW && gifUnit.gifPath[GIF_PATH_3].state != GIF_PATH_IDLE))
				{
					if (vif1.waitforvu)
					{
						CPU_INT(DMAC_MFIFO_VIF, std::max(static_cast<int>((g_vif1Cycles == 0 ? 4 : g_vif1Cycles)), cpuGetCycles(VU_MTVU_BUSY)));
					}
					else
						CPU_INT(DMAC_MFIFO_VIF, (g_vif1Cycles == 0 ? 4 : g_vif1Cycles));
				}

				vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
				return;
		}
		return;
	}

	vif1.vifstalled.enabled = false;
	vif1.irqoffset.enabled = false;
	vif1.done = 1;

	if (spr0ch.madr == vif1ch.tadr)
	{
		FireMFIFOEmpty();
	}

	g_vif1Cycles = 0;
	vif1Regs.stat.FQC = std::min((u32)0x10, vif1ch.qwc);
	vif1ch.chcr.STR = false;
	hwDmacIrq(DMAC_VIF1);
	DMA_LOG("VIF1 MFIFO DMA End");
	CPU_SET_DMASTALL(DMAC_MFIFO_VIF, false);
	vif1Regs.stat.FQC = 0;
}
