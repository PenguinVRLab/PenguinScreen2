// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#define _PC_

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"
#include "IopHw.h"

_sif sif0;

static bool done = false;

static __fi void Sif0Init()
{
	SIF_LOG("SIF0 DMA start...");
	done = false;
	sif0.ee.cycles = 0;
	sif0.iop.cycles = 0;
}

static __fi bool WriteFifoToEE()
{
	const int readSize = std::min((s32)sif0ch.qwc, sif0.fifo.size >> 2);

	tDMA_TAG *ptag;

	SIF_LOG("Write Fifo to EE: ----------- %lX of %lX", readSize << 2, sif0ch.qwc << 2);

	ptag = sif0ch.getAddr(sif0ch.madr, DMAC_SIF0, true);
	if (ptag == NULL)
	{
		DevCon.Warning("Write Fifo to EE: ptag == NULL");
		return false;
	}

	sif0.fifo.read((u32*)ptag, readSize << 2);

	sif0ch.madr += readSize << 4;
	sif0.ee.cycles += readSize;
	sif0ch.qwc -= readSize;

	if (sif0ch.qwc == 0 && dmacRegs.ctrl.STS == STS_SIF0)
	{
		if ((sif0ch.chcr.MOD == NORMAL_MODE) || ((sif0ch.chcr.TAG >> 28) & 0x7) == TAG_CNTS)
			dmacRegs.stadr.ADDR = sif0ch.madr;
	}

	return true;
}

static __fi bool WriteIOPtoFifo()
{
	const int writeSize = std::min(sif0.iop.counter, sif0.fifo.sif_free());

	SIF_LOG("Write IOP to Fifo: +++++++++++ %lX of %lX", writeSize, sif0.iop.counter);

	sif0.fifo.write((u32*)iopPhysMem(hw_dma9.madr), writeSize);
	hw_dma9.madr += writeSize << 2;

	sif0.iop.cycles += writeSize;
	sif0.iop.counter -= writeSize;


	return true;
}

static __fi bool ProcessEETag()
{
	alignas(16) static u32 tag[4];
	tDMA_TAG& ptag(*(tDMA_TAG*)tag);

	sif0.fifo.read((u32*)&tag[0], 4);
	SIF_LOG("SIF0 EE read tag: %x %x %x %x", tag[0], tag[1], tag[2], tag[3]);

	sif0ch.unsafeTransfer(&ptag);
	sif0ch.madr = tag[1];

	SIF_LOG("SIF0 EE dest chain tag madr:%08X qwc:%04X id:%X irq:%d(%08X_%08X)",
		sif0ch.madr, sif0ch.qwc, ptag.ID, ptag.IRQ, tag[1], tag[0]);

	if (sif0ch.chcr.TIE && ptag.IRQ)
	{
		sif0.ee.end = true;
	}

	switch (ptag.ID)
	{
		case TAG_CNT:	break;

		case TAG_CNTS:
			if (dmacRegs.ctrl.STS == STS_SIF0)
					dmacRegs.stadr.ADDR = sif0ch.madr;
			break;

		case TAG_END:
			sif0.ee.end = true;
			break;
	}
	return true;
}

static __fi bool ProcessIOPTag()
{
	sif0.iop.data = *(sifData *)iopPhysMem(hw_dma9.tadr);
	sif0.iop.data.words = sif0.iop.data.words;

	sif0.fifo.write((u32*)iopPhysMem(hw_dma9.tadr + 8), 4);

	hw_dma9.tadr += 16;

	hw_dma9.madr = sif0data & 0xFFFFFF;
	if (sif0words > 0xFFFFF) DevCon.Warning("SIF0 Overrun %x", sif0words);
	sif0.iop.counter = sif0words & 0xFFFFF;

	sif0.iop.writeJunk = (sif0.iop.counter & 0x3) ? (4 - sif0.iop.counter & 0x3) : 0;

	if (sif0tag.IRQ  || (sif0tag.ID & 4)) sif0.iop.end = true;
	SIF_LOG("SIF0 IOP Tag: madr=%lx, tadr=%lx, counter=%lx (%08X_%08X) Junk %d", hw_dma9.madr, hw_dma9.tadr, sif0.iop.counter, sif0words, sif0data, sif0.iop.writeJunk);

	return true;
}

static __fi void EndEE()
{
	SIF_LOG("Sif0: End EE");
	sif0.ee.end = false;
	sif0.ee.busy = false;
	if (sif0.ee.cycles == 0)
	{
		SIF_LOG("SIF0 EE: cycles = 0");
		sif0.ee.cycles = 1;
	}
	CPU_SET_DMASTALL(DMAC_SIF0, false);
	CPU_INT(DMAC_SIF0, sif0.ee.cycles*BIAS);
}

static __fi void EndIOP()
{
	SIF_LOG("Sif0: End IOP");
	sif0data = 0;
	sif0.iop.end = false;
	sif0.iop.busy = false;

	if (sif0.iop.cycles == 0)
	{
		DevCon.Warning("SIF0 IOP: cycles = 0");
		sif0.iop.cycles = 1;
	}
	if (sif0.iop.cycles > 1000)
		sif0.iop.cycles >>= 1;

	PSX_INT(IopEvt_SIF0, sif0.iop.cycles);
}

static __fi void HandleEETransfer()
{
	if(!sif0ch.chcr.STR)
	{
		sif0.ee.end = false;
		sif0.ee.busy = false;
		return;
	}

	if (sif0ch.qwc <= 0)
	{
		if ((sif0ch.chcr.MOD == NORMAL_MODE) || sif0.ee.end)
		{
			done = true;
			EndEE();
		}
		else if (sif0.fifo.size >= 4)
		{
			ProcessEETag();
		}
	}

	if (sif0ch.qwc > 0)
	{
		if (sif0.fifo.size >= 4)
		{
			WriteFifoToEE();
		}
	}
}

static __fi void HandleIOPTransfer()
{
	if (sif0.iop.counter <= 0)
	{
		if (sif0.iop.end)
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
		if (sif0.fifo.sif_free() > 0)
		{
			WriteIOPtoFifo();
		}
	}
}

static __fi void Sif0End()
{
	psHu32(SBUS_F240) &= ~0x20;
	psHu32(SBUS_F240) &= ~0x2000;

	DMA_LOG("SIF0 DMA End");
}

__fi void SIF0Dma()
{
	int BusyCheck = 0;
	Sif0Init();

	do
	{
		BusyCheck = 0;

		if (sif0.iop.counter == 0 && sif0.iop.writeJunk && sif0.fifo.sif_free() >= sif0.iop.writeJunk)
		{
			SIF_LOG("Writing Junk %d", sif0.iop.writeJunk);
			sif0.fifo.writeJunk(sif0.iop.writeJunk);
			sif0.iop.writeJunk = 0;
		}

		if (sif0.iop.busy)
		{
			if(sif0.fifo.sif_free() > 0 || (sif0.iop.end && sif0.iop.counter == 0))
			{
				BusyCheck++;
				HandleIOPTransfer();
			}
		}
		if (sif0.ee.busy)
		{
			if(sif0.fifo.size >= 4 || (sif0.ee.end && sif0ch.qwc == 0))
			{
				BusyCheck++;
				HandleEETransfer();
			}
		}
	} while ( BusyCheck > 0);

	Sif0End();
}

__fi void  sif0Interrupt()
{
	HW_DMA9_CHCR &= ~0x01000000;
	psxDmaInterrupt2(2);
}

__fi void  EEsif0Interrupt()
{
	hwDmacIrq(DMAC_SIF0);
	sif0ch.chcr.STR = false;
}

__fi void dmaSIF0()
{
	SIF_LOG("dmaSIF0 %s", sif0ch.cmqt_to_str().c_str());

	if (sif0.fifo.readPos != sif0.fifo.writePos)
	{
		SIF_LOG("warning, sif0.fifoReadPos != sif0.fifoWritePos");
	}

	psHu32(SBUS_F240) |= 0x2000;
	sif0.ee.busy = true;

	sif0.ee.end = false;
	CPU_SET_DMASTALL(DMAC_SIF0, false);
	SIF0Dma();

}
