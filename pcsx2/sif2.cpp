// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#define _PC_

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"
#include "IopHw.h"

_sif sif2;

static bool done = false;

static __fi void Sif2Init()
{
	SIF_LOG("SIF2 DMA start... free %x iop busy %x", sif2.fifo.sif_free(), sif2.iop.busy);
	done = false;
	sif2.ee.cycles = 0;
	sif2.iop.cycles = 0;
}

__fi bool WriteFifoSingleWord()
{

	SIF_LOG("Write Single word to SIF2 Fifo");

	sif2.fifo.write((u32*)&psxHu32(HW_PS1_GPU_DATA), 1);
	if (sif2.fifo.size > 0) psxHu32(0x1000f300) &= ~0x4000000;
	return true;
}

__fi bool ReadFifoSingleWord()
{
	u32 ptag[4];

	SIF_LOG("Read Fifo SIF2 Single Word IOP Busy %x Fifo Size %x SIF2 CHCR %x", sif2.iop.busy, sif2.fifo.size, HW_DMA2_CHCR);


	sif2.fifo.read((u32*)&ptag[0], 1);
	psHu32(0x1000f3e0) = ptag[0];
	if (sif2.fifo.size == 0) psxHu32(0x1000f300) |= 0x4000000;
	if (sif2.iop.busy && sif2.fifo.size <= 8)SIF2Dma();
	return true;
}

static __fi bool WriteFifoToEE()
{
	const int readSize = std::min((s32)sif2dma.qwc, sif2.fifo.size >> 2);

	tDMA_TAG *ptag;

	SIF_LOG("Write Fifo to EE: ----------- %lX of %lX", readSize << 2, sif2dma.qwc << 2);

	ptag = sif2dma.getAddr(sif2dma.madr, DMAC_SIF2, true);
	if (ptag == NULL)
	{
		DevCon.Warning("Write Fifo to EE: ptag == NULL");
		return false;
	}

	sif2.fifo.read((u32*)ptag, readSize << 2);

	sif2dma.madr += readSize << 4;
	sif2.ee.cycles += readSize;
	sif2dma.qwc -= readSize;

	return true;
}

static __fi bool WriteIOPtoFifo()
{
	const int writeSize = std::min(sif2.iop.counter, sif2.fifo.sif_free());

	SIF_LOG("Write IOP to Fifo: +++++++++++ %lX of %lX", writeSize, sif2.iop.counter);

	sif2.fifo.write((u32*)iopPhysMem(hw_dma2.madr), writeSize);
	hw_dma2.madr += writeSize << 2;

	sif2.iop.cycles += (writeSize >> 2) ;
	sif2.iop.counter -= writeSize;
	if (sif2.iop.counter == 0) hw_dma2.madr = sif2data & 0xffffff;
	if (sif2.fifo.size > 0) psxHu32(0x1000f300) &= ~0x4000000;
	return true;
}

static __fi bool ProcessEETag()
{
	alignas(16) static u32 tag[4];
	tDMA_TAG& ptag(*(tDMA_TAG*)tag);

	sif2.fifo.read((u32*)&tag[0], 4);
	SIF_LOG("SIF2 EE read tag: %x %x %x %x", tag[0], tag[1], tag[2], tag[3]);

	sif2dma.unsafeTransfer(&ptag);
	sif2dma.madr = tag[1];

	SIF_LOG("SIF2 EE dest chain tag madr:%08X qwc:%04X id:%X irq:%d(%08X_%08X)",
		sif2dma.madr, sif2dma.qwc, ptag.ID, ptag.IRQ, tag[1], tag[0]);

	if (sif2dma.chcr.TIE && ptag.IRQ)
	{
		sif2.ee.end = true;
	}

	switch (ptag.ID)
	{
	case TAG_CNT:	break;

	case TAG_CNTS:
		break;

	case TAG_END:
		sif2.ee.end = true;
		break;
	}
	return true;
}

static __fi bool ProcessIOPTag()
{
	if (HW_DMA2_CHCR & 0x400) DevCon.Warning("First bit %x", sif2.iop.data.data);

	sif2.iop.data.words = sif2.iop.data.data >> 24;

	sif2.iop.counter =  (HW_DMA2_BCR_H16 * HW_DMA2_BCR_L16);
sif2.iop.end = true;
	DevCon.Warning("SIF2 IOP Tag: madr=%lx, counter=%lx (%08X_%08X)", hw_dma2.madr, sif2.iop.counter, sif2words, sif2data);

	return true;
}

static __fi void EndEE()
{
	SIF_LOG("Sif2: End EE");
	sif2.ee.end = false;
	sif2.ee.busy = false;
	if (sif2.ee.cycles == 0)
	{
		SIF_LOG("SIF2 EE: cycles = 0");
		sif2.ee.cycles = 1;
	}

	CPU_INT(DMAC_SIF2, sif2.ee.cycles*BIAS);
}

static __fi void EndIOP()
{
	SIF_LOG("Sif2: End IOP");
	sif2data = 0;
	sif2.iop.busy = false;

	if (sif2.iop.cycles == 0)
	{
		DevCon.Warning("SIF2 IOP: cycles = 0");
		sif2.iop.cycles = 1;
	}
	PSX_INT(IopEvt_SIF2, sif2.iop.cycles);
}

static __fi void HandleEETransfer()
{
	if (!sif2dma.chcr.STR)
	{
		sif2.ee.end = false;
		sif2.ee.busy = false;
		return;
	}

	if (sif2dma.qwc <= 0)
	{
		if ((sif2dma.chcr.MOD == NORMAL_MODE) || sif2.ee.end)
		{
			done = true;
			EndEE();
		}
		else if (sif2.fifo.size >= 4)
		{
			DevCon.Warning("SIF2 EE Chain?!");
			ProcessEETag();
		}
	}

	if (sif2dma.qwc > 0)
	{
		if (sif2.fifo.size > 0)
		{
			WriteFifoToEE();
		}
	}
}

static __fi void HandleIOPTransfer()
{
	if (sif2.iop.counter <= 0)
	{
		if (sif2.iop.end)
		{
			done = true;
			EndIOP();
		}
		else
		{
			ProcessIOPTag();
		}
	}
	else
	{
		if (sif2.fifo.sif_free() > 0)
		{
			WriteIOPtoFifo();
		}
		else DevCon.Warning("Nothing free!");
	}
}

static __fi void Sif2End()
{
	psHu32(SBUS_F240) &= ~0x80;
	psHu32(SBUS_F240) &= ~0x8000;

	DMA_LOG("SIF2 DMA End");
}

__fi void SIF2Dma()
{
	int BusyCheck = 0;
	Sif2Init();

	do
	{
		BusyCheck = 0;

		if (sif2.iop.busy)
		{
			if (sif2.fifo.sif_free() > 0 || (sif2.iop.end && sif2.iop.counter == 0))
			{
				BusyCheck++;
				HandleIOPTransfer();
			}
		}
		if (sif2.ee.busy)
		{
			if (sif2.fifo.size >= 4 || (sif2.ee.end && sif2dma.qwc == 0))
			{
				BusyCheck++;
				HandleEETransfer();
			}
		}
	} while ( BusyCheck > 0);

	Sif2End();
}

__fi void  sif2Interrupt()
{
	if (!sif2.iop.end || sif2.iop.counter > 0)
	{
		SIF2Dma();
		return;
	}

	SIF_LOG("SIF2 IOP Intr end");
	HW_DMA2_CHCR &= ~0x01000000;
	psxDmaInterrupt2(2);
}

__fi void  EEsif2Interrupt()
{
	hwDmacIrq(DMAC_SIF2);
	sif2dma.chcr.STR = false;
}

__fi void dmaSIF2()
{
	DevCon.Warning("SIF2 EE CHCR %x", sif2dma.chcr._u32);
	SIF_LOG("dmaSIF2%s", sif2dma.cmqt_to_str().c_str());

	if (sif2.fifo.readPos != sif2.fifo.writePos)
	{
		SIF_LOG("warning, sif2.fifoReadPos != sif2.fifoWritePos");
	}

	psHu32(SBUS_F240) |= 0x8000;
	sif2.ee.busy = true;

	SIF2Dma();

}
