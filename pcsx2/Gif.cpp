// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "Vif_Dma.h"

alignas(16) GIF_Fifo gif_fifo;
alignas(16) gifStruct gif;

static __fi void GifDMAInt(int cycles)
{
	if (dmacRegs.ctrl.MFD == MFD_GIF)
	{
		if (!(cpuRegs.interrupt & (1 << DMAC_MFIFO_GIF)) || cpuRegs.eCycle[DMAC_MFIFO_GIF] < (u32)cycles)
		{
			CPU_INT(DMAC_MFIFO_GIF, cycles);
		}
	}
	else if (!(cpuRegs.interrupt & (1 << DMAC_GIF)) || cpuRegs.eCycle[DMAC_GIF] < (u32)cycles)
	{
		CPU_INT(DMAC_GIF, cycles);
	}
}
__fi void clearFIFOstuff(bool full)
{
	CSRreg.FIFO = full ? CSR_FIFO_FULL : CSR_FIFO_EMPTY;
}

static __fi void CalculateFIFOCSR()
{
	if (gifRegs.stat.FQC >= 15)
	{
		CSRreg.FIFO = CSR_FIFO_FULL;
	}
	else if (gifRegs.stat.FQC == 0)
	{
		CSRreg.FIFO = CSR_FIFO_EMPTY;
	}
	else
	{
		CSRreg.FIFO = CSR_FIFO_NORMAL;
	}
}


bool CheckPaths()
{
	if (!gifUnit.CanDoPath3())
	{
		if (!gifUnit.Path3Masked())
		{
			GifDMAInt(128);
		}
		return false;
	}
	return true;
}

void GIF_Fifo::init()
{
	std::memset(data, 0, sizeof(data));
	fifoSize = 0;
	gifRegs.stat.FQC = 0;

	gif.gifstate = GIF_STATE_READY;
	gif.gspath3done = true;

	gif.gscycles = 0;
	gif.prevcycles = 0;
	gif.mfifocycles = 0;
}


int GIF_Fifo::write_fifo(u32* pMem, int size)
{
	if (fifoSize == 16)
	{
		return 0;
	}

	const int transferSize = std::min(size, 16 - (int)fifoSize);

	int writePos = fifoSize * 4;

	memcpy(&data[writePos], pMem, transferSize * 16);

	fifoSize += transferSize;

	GIF_LOG("GIF FIFO Adding %d QW to GIF FIFO at offset %d FIFO now contains %d QW", transferSize, writePos, fifoSize);

	gifRegs.stat.FQC = fifoSize;
	CalculateFIFOCSR();

	return transferSize;
}

int GIF_Fifo::read_fifo()
{
	if (!fifoSize || !gifUnit.CanDoPath3())
	{
		gifRegs.stat.FQC = fifoSize;
		CalculateFIFOCSR();
		if (fifoSize)
		{
			GIF_LOG("GIF FIFO Can't read, GIF paused/busy. Waiting");
			GifDMAInt(128);
		}
		return 0;
	}

	const int sizeRead = gifUnit.TransferGSPacketData(GIF_TRANS_DMA, (u8*)&data, fifoSize * 16) / 16;

	GIF_LOG("GIF FIFO Read %d QW from FIFO Current Size %d", sizeRead, fifoSize);

	if (sizeRead < (int)fifoSize)
	{
		if (sizeRead > 0)
		{
			const int copyAmount = fifoSize - sizeRead;
			const int readpos = sizeRead * 4;

			for (int i = 0; i < copyAmount; i++)
				CopyQWC(&data[i * 4], &data[readpos + (i * 4)]);

			fifoSize = copyAmount;

			GIF_LOG("GIF FIFO rearranged to now only contain %d QW", fifoSize);
		}
		else
		{
			GIF_LOG("GIF FIFO not read");
		}
	}
	else
	{
		GIF_LOG("GIF FIFO now empty");
		fifoSize = 0;
	}

	gifRegs.stat.FQC = fifoSize;
	CalculateFIFOCSR();

	return sizeRead;
}

void incGifChAddr(u32 qwc)
{
	if (gifch.chcr.STR)
	{
		gifch.madr += qwc * 16;
		gifch.qwc -= qwc;
		hwDmacSrcTadrInc(gifch);
	}
	else
		DevCon.Error("incGifAddr() Error!");
}

__fi void gifCheckPathStatus(bool calledFromGIF)
{
	if (calledFromGIF && gifch.chcr.STR)
	{
		if (gif_fifo.fifoSize == 16)
			GifDMAInt(16);
		return;
	}

	if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_WAIT)
		gifUnit.gifPath[GIF_PATH_3].state = GIF_PATH_IDLE;

	if (gifRegs.stat.APATH == 3)
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH = 0;

		if (!calledFromGIF && (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE || gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_WAIT))
		{
			if (gifUnit.checkPaths(1, 1, 0))
				gifUnit.Execute(false, true);
		}
	}

	if (calledFromGIF && gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE)
	{
		if (vif1Regs.stat.VGW)
		{
			if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
				CPU_INT(DMAC_VIF1, 1);

			if ((!gifUnit.Path3Masked() || gifch.qwc == 0) && (gifch.chcr.STR || gif_fifo.fifoSize))
			{
				GifDMAInt(16);
			}
		}
	}
}

__fi void gifInterrupt()
{
	GIF_LOG("gifInterrupt caught qwc=%d fifo=%d(%d) apath=%d oph=%d state=%d!", gifch.qwc, gifRegs.stat.FQC, gif_fifo.fifoSize, gifRegs.stat.APATH, gifRegs.stat.OPH, gifUnit.gifPath[GIF_PATH_3].state);
	gifCheckPathStatus(false);

	if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE)
	{
		if (vif1Regs.stat.VGW)
		{
			if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
				CPU_INT(DMAC_VIF1, 1);

			if (!gifUnit.Path3Masked() || gifch.qwc == 0)
			{
				GifDMAInt(16);
			}
			CPU_SET_DMASTALL(DMAC_GIF, gifUnit.Path3Masked() || !gifUnit.CanDoPath3());
			return;
		}
	}

	if (dmacRegs.ctrl.MFD == MFD_GIF)
	{
		gifMFIFOInterrupt();
		return;
	}

	if (gifUnit.gsSIGNAL.queued)
	{
		GIF_LOG("Path 3 Paused");
		GifDMAInt(128);
		CPU_SET_DMASTALL(DMAC_GIF, true);
		if (gif_fifo.fifoSize == 16)
			return;
	}

	if (gif_fifo.fifoSize > 0)
	{
		const int readSize = gif_fifo.read_fifo();

		if (readSize)
			GifDMAInt(readSize * BIAS);

		if ((!CheckPaths() && gif_fifo.fifoSize == 16) || readSize)
		{
			CPU_SET_DMASTALL(DMAC_GIF, gifUnit.Path3Masked() || !gifUnit.CanDoPath3());
			return;
		}
	}

	if (!(gifch.chcr.STR))
		return;

	if ((gifch.qwc > 0) || (!gif.gspath3done))
	{
		if (!dmacRegs.ctrl.DMAE)
		{
			Console.Warning("gs dma masked, re-scheduling...");
			GifDMAInt(64);
			CPU_SET_DMASTALL(DMAC_GIF, true);
			return;
		}
		GIFdma();

		return;
	}

	gif.gscycles = 0;
	gifch.chcr.STR = false;
	gifRegs.stat.FQC = gif_fifo.fifoSize;
	CalculateFIFOCSR();
	hwDmacIrq(DMAC_GIF);

	if (gif_fifo.fifoSize)
		GifDMAInt(8 * BIAS);
	GIF_LOG("GIF DMA End QWC in fifo %x (%x) APATH = %x OPH = %x state = %x", gifRegs.stat.FQC, gif_fifo.fifoSize, gifRegs.stat.APATH, gifRegs.stat.OPH, gifUnit.gifPath[GIF_PATH_3].state);
}

static u32 WRITERING_DMA(u32* pMem, u32 qwc)
{
	const u32 originalQwc = qwc;

	if (gifRegs.stat.IMT)
	{
		if (qwc > 64)
			qwc = qwc * 0.5f;
		else
			qwc = std::min(qwc, 8u);
	}
	else if (qwc > 8)
		qwc -= 8;

	uint size;

	if (CheckPaths() == false || ((qwc < 8 || gif_fifo.fifoSize > 0) && CHECK_GIFFIFOHACK))
	{
		if (gif_fifo.fifoSize < 16)
		{
			size = gif_fifo.write_fifo((u32*)pMem, originalQwc);
			incGifChAddr(size);
			return size;
		}
		return 4;
	}

	size = gifUnit.TransferGSPacketData(GIF_TRANS_DMA, (u8*)pMem, qwc * 16) / 16;
	incGifChAddr(size);
	return size;
}

static __fi void GIFchain()
{
	tDMA_TAG* pMem;

	pMem = dmaGetAddr(gifch.madr, false);
	if (pMem == NULL)
	{
		gifch.madr += gifch.qwc * 16;
		gifch.qwc = 0;
		Console.Warning("Hackfix - NULL GIFchain");
		return;
	}

	const int transferred = WRITERING_DMA((u32*)pMem, gifch.qwc);
	gif.gscycles += transferred * BIAS;

	if (!gifUnit.Path3Masked() || (gif_fifo.fifoSize < 16))
		GifDMAInt(gif.gscycles);
}

static __fi bool checkTieBit(tDMA_TAG*& ptag)
{
	if (gifch.chcr.TIE && ptag->IRQ)
	{
		GIF_LOG("dmaIrq Set");
		gif.gspath3done = true;
		return true;
	}
	return false;
}

static __fi tDMA_TAG* ReadTag()
{
	tDMA_TAG* ptag = dmaGetAddr(gifch.tadr, false);

	if (!(gifch.transfer("Gif", ptag)))
		return NULL;

	gifch.madr = ptag[1]._u32;
	gif.gscycles += 2;

	gif.gspath3done = hwDmacSrcChainWithStack(gifch, ptag->ID);
	return ptag;
}

void GIFdma()
{
	while (gifch.qwc > 0 || !gif.gspath3done)
	{
		tDMA_TAG* ptag;
		gif.gscycles = gif.prevcycles;

		if (gifRegs.ctrl.PSE)
		{
			DevCon.WriteLn("Gif dma paused by PSE bit.");
			GifDMAInt(16);
			CPU_SET_DMASTALL(DMAC_GIF, true);
			return;
		}

		if ((dmacRegs.ctrl.STD == STD_GIF) && (gif.prevcycles != 0))
		{
			if ((gifch.madr + (gifch.qwc * 16)) > dmacRegs.stadr.ADDR)
			{
				GifDMAInt(4);
				CPU_SET_DMASTALL(DMAC_GIF, true);
				gif.gscycles = 0;
				return;
			}
			gif.prevcycles = 0;
			gifch.qwc = 0;
		}

		if ((gifch.chcr.MOD == CHAIN_MODE) && (!gif.gspath3done) && gifch.qwc == 0)
		{
			ptag = ReadTag();
			if (ptag == NULL)
				return;
			GIF_LOG("gifdmaChain %8.8x_%8.8x size=%d, id=%d, addr=%lx tadr=%lx", ptag[1]._u32, ptag[0]._u32, gifch.qwc, ptag->ID, gifch.madr, gifch.tadr);
			gifRegs.stat.FQC = std::min((u32)0x10, gifch.qwc);
			CalculateFIFOCSR();

			if (dmacRegs.ctrl.STD == STD_GIF)
			{
				if ((ptag->ID == TAG_REFS) && ((gifch.madr + (gifch.qwc * 16)) > dmacRegs.stadr.ADDR))
				{
					gif.prevcycles = gif.gscycles;
					gifch.tadr -= 16;
					gifch.qwc = 0;
					hwDmacIrq(DMAC_STALL_SIS);
					GifDMAInt(128);
					gif.gscycles = 0;
					CPU_SET_DMASTALL(DMAC_GIF, true);
					return;
				}
			}

			checkTieBit(ptag);
		}
		else if (dmacRegs.ctrl.STD == STD_GIF && gifch.chcr.MOD == NORMAL_MODE)
		{
			Console.WriteLn("GIF DMA Stall in Normal mode not implemented - Report which game to PCSX2 Team");
		}

		if (gifch.qwc > 0)
		{
			GIFchain();
			CPU_SET_DMASTALL(DMAC_GIF, gifUnit.Path3Masked() || !gifUnit.CanDoPath3());
			return;
		}
	}

	gif.prevcycles = 0;
	GifDMAInt(16);
}

void dmaGIF()
{

	gif.gspath3done = false;
	CPU_SET_DMASTALL(DMAC_GIF, false);
	if (gifch.chcr.MOD == NORMAL_MODE)
	{
		gif.gspath3done = true;
	}

	if (gifch.chcr.MOD == CHAIN_MODE && gifch.qwc > 0)
	{
		if ((gifch.chcr.tag().ID == TAG_REFE) || (gifch.chcr.tag().ID == TAG_END) || (gifch.chcr.tag().IRQ && gifch.chcr.TIE))
		{
			gif.gspath3done = true;
		}
	}

	gifInterrupt();
}

static u32 QWCinGIFMFIFO(u32 DrainADDR)
{
	u32 ret;

	SPR_LOG("GIF MFIFO Requesting %x QWC from the MFIFO Base %x, SPR MADR %x Drain %x", gifch.qwc, dmacRegs.rbor.ADDR, spr0ch.madr, DrainADDR);
	if (DrainADDR <= spr0ch.madr)
	{
		ret = (spr0ch.madr - DrainADDR) >> 4;
	}
	else
	{
		u32 limit = dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16;
		ret = ((spr0ch.madr - dmacRegs.rbor.ADDR) + (limit - DrainADDR)) >> 4;
	}
	if (ret == 0)
		gif.gifstate = GIF_STATE_EMPTY;

	SPR_LOG("%x Available of the %x requested", ret, gifch.qwc);
	return ret;
}

static __fi bool mfifoGIFrbTransfer()
{
	const u32 qwc = std::min(QWCinGIFMFIFO(gifch.madr), gifch.qwc);

	if (qwc == 0)
		return true;

	u8* src = (u8*)PSM(gifch.madr);
	if (src == NULL)
		return false;

	const u32 MFIFOUntilEnd = ((dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16) - gifch.madr) >> 4;
	const bool needWrap = MFIFOUntilEnd < qwc;
	const u32 firstTransQWC = needWrap ? MFIFOUntilEnd : qwc;
	const u32 transferred = WRITERING_DMA((u32*)src, firstTransQWC);

	gifch.madr = dmacRegs.rbor.ADDR + (gifch.madr & dmacRegs.rbsr.RMSK);
	gifch.tadr = dmacRegs.rbor.ADDR + (gifch.tadr & dmacRegs.rbsr.RMSK);

	if (needWrap && transferred == MFIFOUntilEnd)
	{
		src = (u8*)PSM(dmacRegs.rbor.ADDR);
		if (src == NULL)
			return false;

		const uint secondTransQWC = qwc - MFIFOUntilEnd;
		const u32 transferred2 = WRITERING_DMA((u32*)src, secondTransQWC);

		gif.mfifocycles += (transferred2 + transferred) * 2;
	}
	else
		gif.mfifocycles += transferred * 2;

	return true;
}

static __fi void mfifoGIFchain()
{
	if (gifch.qwc == 0)
	{
		gif.mfifocycles += 4;
		return;
	}

	if ((gifch.madr & ~dmacRegs.rbsr.RMSK) == dmacRegs.rbor.ADDR)
	{
		if (QWCinGIFMFIFO(gifch.madr) == 0)
		{
			SPR_LOG("GIF FIFO EMPTY before transfer");
			gif.gifstate = GIF_STATE_EMPTY;
			gif.mfifocycles += 4;
			return;
		}

		if (!mfifoGIFrbTransfer())
		{
			gif.mfifocycles += 4;
			gifch.qwc = 0;
			gif.gspath3done = true;
			return;
		}

		gifch.madr = dmacRegs.rbor.ADDR + (gifch.madr & dmacRegs.rbsr.RMSK);
		gifch.tadr = gifch.madr;
	}
	else
	{
		SPR_LOG("Non-MFIFO Location transfer doing %x Total QWC", gifch.qwc);
		tDMA_TAG* pMem = dmaGetAddr(gifch.madr, false);
		if (pMem == NULL)
		{
			gif.mfifocycles += 4;
			gifch.qwc = 0;
			gif.gspath3done = true;
			return;
		}

		gif.mfifocycles += WRITERING_DMA((u32*)pMem, gifch.qwc) * 2;
	}

	return;
}

static u32 qwctag(u32 mask)
{
	return (dmacRegs.rbor.ADDR + (mask & dmacRegs.rbsr.RMSK));
}

void mfifoGifMaskMem(int id)
{
	switch (id)
	{
		case TAG_CNT:
		case TAG_NEXT:
		case TAG_CALL:
		case TAG_RET:
		case TAG_END:
			if (gifch.madr < dmacRegs.rbor.ADDR)
			{
				SPR_LOG("GIF MFIFO MADR below bottom of ring buffer, wrapping GIF MADR = %x Ring Bottom %x", gifch.madr, dmacRegs.rbor.ADDR);
				gifch.madr = qwctag(gifch.madr);
			}
			else if (gifch.madr > (dmacRegs.rbor.ADDR + (u32)dmacRegs.rbsr.RMSK))
			{
				SPR_LOG("GIF MFIFO MADR outside top of ring buffer, wrapping GIF MADR = %x Ring Top %x", gifch.madr, (dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK) + 16);
				gifch.madr = qwctag(gifch.madr);
			}
			break;
		default:
			break;
	}
}

void mfifoGIFtransfer()
{
	gif.mfifocycles = 0;

	if (gifRegs.ctrl.PSE)
	{
		DevCon.WriteLn("Gif MFIFO dma paused by PSE bit.");
		CPU_INT(DMAC_MFIFO_GIF, 16);
		CPU_SET_DMASTALL(DMAC_MFIFO_GIF, true);
		return;
	}

	if (gifch.qwc == 0)
	{
		gifch.tadr = qwctag(gifch.tadr);

		if (QWCinGIFMFIFO(gifch.tadr) == 0)
		{
			SPR_LOG("GIF FIFO EMPTY before tag read");
			gif.gifstate = GIF_STATE_EMPTY;
			GifDMAInt(4);
			CPU_SET_DMASTALL(DMAC_MFIFO_GIF, true);
			return;
		}

		tDMA_TAG* ptag = dmaGetAddr(gifch.tadr, false);
		gifch.unsafeTransfer(ptag);
		gifch.madr = ptag[1]._u32;

		gifRegs.stat.FQC = std::min((u32)0x10, gifch.qwc);
		CalculateFIFOCSR();

		gif.mfifocycles += 2;

		GIF_LOG("dmaChain %8.8x_%8.8x size=%d, id=%d, madr=%lx, tadr=%lx mfifo qwc = %x spr0 madr = %x",
			ptag[1]._u32, ptag[0]._u32, gifch.qwc, ptag->ID, gifch.madr, gifch.tadr, gif.gifqwc, spr0ch.madr);

		gif.gspath3done = hwDmacSrcChainWithStack(gifch, ptag->ID);

		if (dmacRegs.ctrl.STD == STD_GIF && (ptag->ID == TAG_REFS))
		{
			Console.WriteLn("GIF MFIFO DMA Stall not implemented - Report which game to PCSX2 Team");
		}
		mfifoGifMaskMem(ptag->ID);

		gifch.tadr = qwctag(gifch.tadr);

		if ((gifch.chcr.TIE) && (ptag->IRQ))
		{
			SPR_LOG("dmaIrq Set");
			gif.gspath3done = true;
		}
	}

	mfifoGIFchain();

	GifDMAInt(std::max(gif.mfifocycles, (u32)4));

	SPR_LOG("mfifoGIFtransfer end %x madr %x, tadr %x", gifch.chcr._u32, gifch.madr, gifch.tadr);
}

void gifMFIFOInterrupt()
{
	gif.mfifocycles = 0;

	if (dmacRegs.ctrl.MFD != MFD_GIF)
	{
		DevCon.WriteLn("GIF Leaving MFIFO - Report if any errors");
		gifInterrupt();
		CPU_SET_DMASTALL(DMAC_MFIFO_GIF, true);
		return;
	}

	gifCheckPathStatus(false);

	if (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE)
	{
		if (vif1Regs.stat.VGW)
		{
			if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
				CPU_INT(DMAC_VIF1, 1);

			if (!gifUnit.Path3Masked() || gifch.qwc == 0)
			{
				GifDMAInt(16);
			}
			CPU_SET_DMASTALL(DMAC_MFIFO_GIF, gifUnit.Path3Masked() || !gifUnit.CanDoPath3());
			return;
		}
	}

	if (gifUnit.gsSIGNAL.queued)
	{
		GifDMAInt(128);
		CPU_SET_DMASTALL(DMAC_MFIFO_GIF, true);
		return;
	}

	if (gif_fifo.fifoSize > 0)
	{
		const int readSize = gif_fifo.read_fifo();

		if (readSize)
			GifDMAInt(readSize * BIAS);

		if ((!CheckPaths() && gif_fifo.fifoSize == 16) || readSize)
		{
			CPU_SET_DMASTALL(DMAC_MFIFO_GIF, gifUnit.Path3Masked() || !gifUnit.CanDoPath3());
			return;
		}
	}

	if (!gifch.chcr.STR)
		return;

	if (spr0ch.madr == gifch.tadr || (gif.gifstate & GIF_STATE_EMPTY))
	{
		gif.gifstate = GIF_STATE_EMPTY;
		FireMFIFOEmpty();

		if (gifch.qwc > 0 || !gif.gspath3done)
		{
			CPU_SET_DMASTALL(DMAC_MFIFO_GIF, true);
			return;
		}
	}

	if (gifch.qwc > 0 || !gif.gspath3done)
	{
		mfifoGIFtransfer();
		CPU_SET_DMASTALL(DMAC_MFIFO_GIF, gifUnit.Path3Masked() || !gifUnit.CanDoPath3());
		return;
	}

	gif.gscycles = 0;

	gifch.chcr.STR = false;
	gif.gifstate = GIF_STATE_READY;
	gifRegs.stat.FQC = gif_fifo.fifoSize;
	CalculateFIFOCSR();
	hwDmacIrq(DMAC_GIF);
	CPU_SET_DMASTALL(DMAC_MFIFO_GIF, false);
	if (gif_fifo.fifoSize)
		GifDMAInt(8 * BIAS);
	DMA_LOG("GIF MFIFO DMA End");
}

bool SaveStateBase::gifDmaFreeze()
{
	if (!FreezeTag("GIFdma"))
		return false;

	Freeze(gif);
	Freeze(gif_fifo);

	return IsOkay();
}
