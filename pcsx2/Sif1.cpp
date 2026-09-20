// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#define _PC_

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"
#include "IopHw.h"

_sif sif1;

static bool done = false;
static bool sif1_dma_stall = false;

static __fi void Sif1Init()
{
	SIF_LOG("SIF1 DMA start...");
	done = false;
	sif1.ee.cycles = 0;
	sif1.iop.cycles = 0;
}

static __fi bool WriteEEtoFifo()
{

	SIF_LOG("Sif 1: Write EE to Fifo");
	const int writeSize = std::min((s32)sif1ch.qwc, sif1.fifo.sif_free() >> 2);

	tDMA_TAG *ptag;

	ptag = sif1ch.getAddr(sif1ch.madr, DMAC_SIF1, false);
	if (ptag == NULL)
	{
		DevCon.Warning("Write EE to Fifo: ptag == NULL");
		return false;
	}

	sif1.fifo.write((u32*)ptag, writeSize << 2);

	sif1ch.madr += writeSize << 4;
	hwDmacSrcTadrInc(sif1ch);
	sif1.ee.cycles += writeSize;
	sif1ch.qwc -= writeSize;

	return true;
}

static __fi bool WriteFifoToIOP()
{

	SIF_LOG("Sif1: Write Fifo to IOP");
	const int readSize = std::min(sif1.iop.counter, sif1.fifo.size);

	SIF_LOG("Sif 1 IOP doing transfer %04X to %08X", readSize, HW_DMA10_MADR);

	sif1.fifo.read((u32*)iopPhysMem(hw_dma10.madr), readSize);
	psxCpu->Clear(hw_dma10.madr, readSize);
	hw_dma10.madr += readSize << 2;
	sif1.iop.cycles += readSize >> 2;
	sif1.iop.counter -= readSize;

	return true;
}

static __fi bool ProcessEETag()
{
	tDMA_TAG *ptag;
	SIF_LOG("Sif1: ProcessEETag");

	ptag = sif1ch.DMAtransfer(sif1ch.tadr, DMAC_SIF1);
	if (ptag == NULL)
	{
		Console.WriteLn("Sif1 ProcessEETag: ptag = NULL");
		return false;
	}

	if (sif1ch.chcr.TTE)
	{
		Console.WriteLn("SIF1 TTE");
		sif1.fifo.write((u32*)ptag + 2, 2);
	}

	SIF_LOG("%s", ptag->tag_to_str().c_str());
	sif1ch.madr = ptag[1]._u32;

	sif1.ee.end = hwDmacSrcChain(sif1ch, ptag->ID);

	if (sif1ch.chcr.TIE && ptag->IRQ)
	{
		sif1.ee.end = true;
	}

	return true;
}

static __fi bool SIFIOPReadTag()
{
	sif1.fifo.read((u32*)&sif1.iop.data, 4);
	SIF_LOG("SIF 1 IOP: dest chain tag madr:%08X wc:%04X id:%X irq:%d",
		sif1data & 0xffffff, sif1words, sif1tag.ID, sif1tag.IRQ);

	hw_dma10.madr = sif1data & 0xffffff;


	if (sif1words > 0xFFFFC) DevCon.Warning("SIF1 Overrun %x", sif1words);
	sif1.iop.counter = sif1words & 0xFFFFC;

	if (sif1tag.IRQ  || (sif1tag.ID & 4)) sif1.iop.end = true;

	return true;
}

static __fi void EndEE()
{
	sif1.ee.end = false;
	sif1.ee.busy = false;
	SIF_LOG("Sif 1: End EE");

	if (sif1.ee.cycles == 0)
	{
		SIF_LOG("SIF1 EE: cycles = 0");
		sif1.ee.cycles = 1;
	}

	CPU_SET_DMASTALL(DMAC_SIF1, false);
	CPU_INT(DMAC_SIF1, sif1.ee.cycles*BIAS );
}

static __fi void EndIOP()
{
	sif1data = 0;
	sif1.iop.end = false;
	sif1.iop.busy = false;
	SIF_LOG("Sif 1: End IOP");

	if (sif1.iop.cycles == 0)
	{
		DevCon.Warning("SIF1 IOP: cycles = 0");
		sif1.iop.cycles = 1;
	}
	PSX_INT(IopEvt_SIF1, sif1.iop.cycles );
}

static __fi void HandleEETransfer()
{
	if(!sif1ch.chcr.STR)
	{
		sif1.ee.end = false;
		sif1.ee.busy = false;
		return;
	}

	if (sif1ch.qwc <= 0)
	{
		if ((sif1ch.chcr.MOD == NORMAL_MODE) || sif1.ee.end)
		{
			done = true;
			EndEE();
		}
		else
		{
			done = false;
			if (!ProcessEETag()) return;
		}
	}
	else
	{
		if (dmacRegs.ctrl.STD == STD_SIF1)
		{
			if ((sif1ch.chcr.MOD == NORMAL_MODE) || ((sif1ch.chcr.TAG >> 28) & 0x7) == TAG_REFS)
			{
				const int writeSize = std::min((s32)sif1ch.qwc, sif1.fifo.sif_free() >> 2);
				if ((sif1ch.madr + (writeSize * 16)) > dmacRegs.stadr.ADDR)
				{
					hwDmacIrq(DMAC_STALL_SIS);
					sif1_dma_stall = true;
					CPU_SET_DMASTALL(DMAC_SIF1, true);
					return;
				}
			}
		}
		if (sif1.fifo.sif_free() > 0)
		{
			WriteEEtoFifo();
		}
	}
}

static __fi void HandleIOPTransfer()
{
	if (sif1.iop.counter > 0)
	{
		if (sif1.fifo.size > 0)
		{
			WriteFifoToIOP();
		}
	}

	if (sif1.iop.counter <= 0)
	{
		if (sif1.iop.end)
		{
			done = true;
			EndIOP();
		}
		else if (sif1.fifo.size >= 4)
		{

			done = false;
			SIFIOPReadTag();
		}
	}
}

static __fi void Sif1End()
{
	psHu32(SBUS_F240) &= ~0x40;
	psHu32(SBUS_F240) &= ~0x4000;

	DMA_LOG("SIF1 DMA End");
}

__fi void SIF1Dma()
{
	int BusyCheck = 0;

	if (sif1_dma_stall)
	{
		const int writeSize = std::min((s32)sif1ch.qwc, sif1.fifo.sif_free() >> 2);
		if ((sif1ch.madr + (writeSize * 16)) > dmacRegs.stadr.ADDR)
			return;
	}

	sif1_dma_stall = false;
	Sif1Init();

	do
	{
		BusyCheck = 0;

		if (sif1.ee.busy && !sif1_dma_stall)
		{
			if(sif1.fifo.sif_free() > 0 || (sif1.ee.end && sif1ch.qwc == 0))
			{
				BusyCheck++;
				HandleEETransfer();
			}
		}

		if (sif1.iop.busy)
		{
			if(sif1.fifo.size >= 4 || (sif1.iop.end && sif1.iop.counter == 0))
			{
				BusyCheck++;
				HandleIOPTransfer();
			}
		}

	} while ( BusyCheck > 0);

	Sif1End();
}

__fi void  sif1Interrupt()
{
	HW_DMA10_CHCR &= ~0x01000000;
	psxDmaInterrupt2(3);
}

__fi void  EEsif1Interrupt()
{
	hwDmacIrq(DMAC_SIF1);
	sif1ch.chcr.STR = false;
}

__fi void dmaSIF1()
{
	SIF_LOG("dmaSIF1 %s", sif1ch.cmqt_to_str().c_str());

	if (sif1.fifo.readPos != sif1.fifo.writePos)
	{
		SIF_LOG("warning, sif1.fifoReadPos != sif1.fifoWritePos");
	}

	psHu32(SBUS_F240) |= 0x4000;
	sif1.ee.busy = true;

	CPU_SET_DMASTALL(DMAC_SIF1, false);

	sif1.ee.end = false;

	if (sif1ch.chcr.MOD == CHAIN_MODE && sif1ch.qwc > 0)
	{
		if ((sif1ch.chcr.tag().ID == TAG_REFE) || (sif1ch.chcr.tag().ID == TAG_END) || (sif1ch.chcr.tag().IRQ && vif1ch.chcr.TIE))
		{
			sif1.ee.end = true;
		}
	}

	SIF1Dma();

}
