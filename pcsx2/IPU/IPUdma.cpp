// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "IPU/IPU_MultiISA.h"

IPUDMAStatus IPU1Status;

void ipuDmaReset()
{
	IPU1Status.InProgress	= false;
	IPU1Status.DMAFinished	= true;
}

bool SaveStateBase::ipuDmaFreeze()
{
	if (!FreezeTag("IPUdma"))
		return false;

	Freeze(IPU1Status);
	return IsOkay();
}

static __fi int IPU1chain() {

	int totalqwc = 0;

	int qwc = ipu1ch.qwc;
	u32 *pMem;

	pMem = (u32*)dmaGetAddr(ipu1ch.madr, false);

	if (pMem == NULL)
	{
		Console.Error("ipu1dma NULL!");
		return totalqwc;
	}

	qwc = ipu_fifo.in.write(pMem, qwc);
	ipu1ch.madr += qwc << 4;
	ipu1ch.qwc -= qwc;
	totalqwc += qwc;

	hwDmacSrcTadrInc(ipu1ch);

	if (!ipu1ch.qwc)
		IPU1Status.InProgress = false;

	return totalqwc;
}

void IPU1dma()
{
	if(!ipu1ch.chcr.STR || ipu1ch.chcr.MOD == 2)
	{
		DevCon.Warning("IPU1 running when IPU1 DMA disabled! CHCR %x QWC %x", ipu1ch.chcr._u32, ipu1ch.qwc);
		CPU_SET_DMASTALL(DMAC_TO_IPU, true);
		return;
	}

	if (IPUCoreStatus.DataRequested == false)
	{
		cpuRegs.eCycle[4] = 0x9999;
		CPU_SET_DMASTALL(DMAC_TO_IPU, true);

		if (IPUCoreStatus.WaitingOnIPUTo)
		{
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(4 * BIAS);
		}
		return;
	}

	int tagcycles = 0;
	int totalqwc = 0;

	IPU_LOG("IPU1 DMA Called QWC %x Finished %d In Progress %d tadr %x", ipu1ch.qwc, IPU1Status.DMAFinished, IPU1Status.InProgress, ipu1ch.tadr);
	if (!IPU1Status.InProgress)
	{
		if (IPU1Status.DMAFinished)
			DevCon.Warning("IPU1 DMA Somehow reading tag when finished??");

		tDMA_TAG* ptag = dmaGetAddr(ipu1ch.tadr, false);

		if (!ipu1ch.transfer("IPU1", ptag))
		{
			return;
		}
		ipu1ch.madr = ptag[1]._u32;

		tagcycles += 1;

		if (ipu1ch.chcr.TTE) DevCon.Warning("TTE?");

		IPU1Status.DMAFinished = hwDmacSrcChain(ipu1ch, ptag->ID);

		IPU_LOG("dmaIPU1 dmaChain %8.8x_%8.8x size=%d, addr=%lx, fifosize=%x",
			ptag[1]._u32, ptag[0]._u32, ipu1ch.qwc, ipu1ch.madr, 8 - g_BP.IFC);

		if (ipu1ch.chcr.TIE && ptag->IRQ)
			IPU1Status.DMAFinished = true;

		if (ipu1ch.qwc)
			IPU1Status.InProgress = true;
	}

	if (IPU1Status.InProgress)
		totalqwc += IPU1chain();

	if(totalqwc == 0 || (IPU1Status.DMAFinished && !IPU1Status.InProgress))
	{
		totalqwc = std::max(4, totalqwc) + tagcycles;
		IPU_INT_TO(totalqwc * BIAS);
	}
	else
	{
			cpuRegs.eCycle[4] = 0x9999;
			CPU_SET_DMASTALL(DMAC_TO_IPU, true);
	}

	if (IPUCoreStatus.WaitingOnIPUTo && g_BP.IFC >= 1)
	{
		IPUCoreStatus.WaitingOnIPUTo = false;
		IPU_INT_PROCESS(totalqwc * BIAS);
	}

	IPU_LOG("Completed Call IPU1 DMA QWC Remaining %x Finished %d In Progress %d tadr %x", ipu1ch.qwc, IPU1Status.DMAFinished, IPU1Status.InProgress, ipu1ch.tadr);
}

void IPU0dma()
{
	if(!ipuRegs.ctrl.OFC)
	{
		if (IPUCoreStatus.WaitingOnIPUFrom)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPUProcessInterrupt();
		}
		CPU_SET_DMASTALL(DMAC_FROM_IPU, true);
		return;
	}

	int readsize;
	tDMA_TAG* pMem;

	if ((!(ipu0ch.chcr.STR) || (cpuRegs.interrupt & (1 << DMAC_FROM_IPU))) || (ipu0ch.qwc == 0))
	{
		DevCon.Warning("How??");
		if (IPUCoreStatus.WaitingOnIPUFrom)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPU_INT_PROCESS(ipuRegs.ctrl.OFC * BIAS);
		}
		return;
	}

	pxAssert(!(ipu0ch.chcr.TTE));

	IPU_LOG("dmaIPU0 chcr = %lx, madr = %lx, qwc  = %lx",
	        ipu0ch.chcr._u32, ipu0ch.madr, ipu0ch.qwc);

	pxAssert(ipu0ch.chcr.MOD == NORMAL_MODE);

	pMem = dmaGetAddr(ipu0ch.madr, true);

	readsize = std::min(ipu0ch.qwc, (u32)ipuRegs.ctrl.OFC);
	ipu_fifo.out.read(pMem, readsize);

	ipu0ch.madr += readsize << 4;
	ipu0ch.qwc -= readsize;

	if (dmacRegs.ctrl.STS == STS_fromIPU)
	{
		dmacRegs.stadr.ADDR = ipu0ch.madr;
	}

	if (!ipu0ch.qwc)
		IPU_INT_FROM(readsize * BIAS);

	CPU_SET_DMASTALL(DMAC_FROM_IPU, true);

	if (ipuRegs.ctrl.BUSY && IPUCoreStatus.WaitingOnIPUFrom)
	{
		IPUCoreStatus.WaitingOnIPUFrom = false;
		IPU_INT_PROCESS(readsize * BIAS);
	}
}

__fi void dmaIPU0()
{

	if (dmacRegs.ctrl.STS == STS_fromIPU)
		dmacRegs.stadr.ADDR = ipu0ch.madr;

	CPU_SET_DMASTALL(DMAC_FROM_IPU, false);
	IPU0dma();

	if (ipu0ch.qwc == 0x10000)
	{
		ipu0ch.qwc = 0;
		ipu0ch.chcr.STR = false;
		hwDmacIrq(DMAC_FROM_IPU);
		DMA_LOG("IPU0 DMA End");
	}
}

__fi void dmaIPU1()
{
	IPU_LOG("IPU1DMAStart QWC %x, MADR %x, CHCR %x, TADR %x", ipu1ch.qwc, ipu1ch.madr, ipu1ch.chcr._u32, ipu1ch.tadr);
	CPU_SET_DMASTALL(DMAC_TO_IPU, false);

	if (ipu1ch.chcr.MOD == CHAIN_MODE)
	{
		IPU_LOG("Setting up IPU1 Chain mode");
		if(ipu1ch.qwc == 0)
		{
			IPU1Status.InProgress = false;
			IPU1Status.DMAFinished = false;
		}
		else
		{
			IPU_LOG("Resuming DMA TAG %x", (ipu1ch.chcr.TAG >> 12));
			IPU1Status.InProgress = true;
			if ((ipu1ch.chcr.tag().ID == TAG_REFE) || (ipu1ch.chcr.tag().ID == TAG_END) || (ipu1ch.chcr.tag().IRQ && ipu1ch.chcr.TIE))
			{
				IPU1Status.DMAFinished = true;
			}
			else
			{
				IPU1Status.DMAFinished = false;
			}
		}
	}
	else
	{
			IPU_LOG("Setting up IPU1 Normal mode");
			IPU1Status.InProgress = true;
			IPU1Status.DMAFinished = true;
	}

	IPU1dma();
}

void ipuCMDProcess()
{
	IPUProcessInterrupt();
}

void ipu0Interrupt()
{
	IPU_LOG("ipu0Interrupt: %llx", cpuRegs.cycle);

	if(ipu0ch.qwc > 0)
	{
		IPU0dma();
		return;
	}

	ipu0ch.chcr.STR = false;
	hwDmacIrq(DMAC_FROM_IPU);
	CPU_SET_DMASTALL(DMAC_FROM_IPU, false);
	DMA_LOG("IPU0 DMA End");
}

__fi void ipu1Interrupt()
{
	IPU_LOG("ipu1Interrupt %llx:", cpuRegs.cycle);

	if(!IPU1Status.DMAFinished || IPU1Status.InProgress)
	{
		IPU1dma();
		return;
	}

	DMA_LOG("IPU1 DMA End");
	ipu1ch.chcr.STR = false;
	hwDmacIrq(DMAC_TO_IPU);
	CPU_SET_DMASTALL(DMAC_TO_IPU, false);
}
