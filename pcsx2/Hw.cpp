// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Gif_Unit.h"
#include "Hardware.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"

#include "common/WrappedMemCopy.h"

#include "fmt/format.h"

#include <deque>

using namespace R5900;

const int rdram_devices = 2;
int rdram_sdevid = 0;

std::deque<u8> ee_sio_rx_fifo;
std::deque<u8> ee_sio_tx_fifo;

void hwReset()
{
	std::memset(eeHw, 0, sizeof(eeHw));

	{
		std::deque<u8> empty_rx;
		std::swap(ee_sio_rx_fifo, empty_rx);
		std::deque<u8> empty_tx;
		std::swap(ee_sio_tx_fifo, empty_tx);
	}

	psHu32(SBUS_F260) = 0x1D000060;

	psHu32(DMAC_ENABLEW) = 0x1201;
	psHu32(DMAC_ENABLER) = 0x1201;

	rcntInit();

	SPU2::Reset(false);

	sifReset();
	gsReset();
	gifUnit.Reset();
	ipuReset();
	vif0Reset();
	vif1Reset();
	gif_fifo.init();
	USBreset();
}

__fi uint intcInterrupt()
{
	if ((psHu32(INTC_STAT)) == 0) {
		return 0;
	}
	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) == 0)
	{
		return 0;
	}

	HW_LOG("intcInterrupt %x", psHu32(INTC_STAT) & psHu32(INTC_MASK));
	if(psHu32(INTC_STAT) & 0x2){
		counters[0].hold = rcntRcount(0);
		counters[1].hold = rcntRcount(1);
	}

	return 0x400;
}

__fi uint dmacInterrupt()
{
	if( ((psHu16(DMAC_STAT + 2) & psHu16(DMAC_STAT)) == 0 ) &&
		( psHu16(DMAC_STAT) & 0x8000) == 0 )
	{
		return 0;
	}

	if (!dmacRegs.ctrl.DMAE || psHu8(DMAC_ENABLER+2) == 1)
	{
		return 0;
	}

	DMA_LOG("dmacInterrupt %x",
		((psHu16(DMAC_STAT + 2) & psHu16(DMAC_STAT)) |
		 (psHu16(DMAC_STAT) & 0x8000))
	);

	return 0x800;
}

void hwIntcIrq(int n)
{
	psHu32(INTC_STAT) |= 1<<n;
	if(psHu32(INTC_MASK) & (1<<n))cpuTestINTCInts();
}

void hwDmacIrq(int n)
{
	psHu32(DMAC_STAT) |= 1<<n;
	if(psHu16(DMAC_STAT+2) & (1<<n))cpuTestDMACInts();
}

void FireMFIFOEmpty()
{
	SPR_LOG("MFIFO Data Empty");
	hwDmacIrq(DMAC_MFIFO_EMPTY);

	if (dmacRegs.ctrl.MFD == MFD_VIF1) vif1Regs.stat.FQC = 0;
	if (dmacRegs.ctrl.MFD == MFD_GIF)  gifRegs.stat.FQC  = 0;
}

__ri bool hwMFIFOWrite(u32 addr, const u128* data, uint qwc)
{
	pxAssert((dmacRegs.rbor.ADDR & 15) == 0);
	pxAssert((addr & 15) == 0);

	if(qwc > ((dmacRegs.rbsr.RMSK + 16u) >> 4u)) DevCon.Warning("MFIFO Write bigger than MFIFO! QWC=%x FifoSize=%x", qwc, ((dmacRegs.rbsr.RMSK + 16) >> 4));

	if (u128* dst = (u128*)PSM(dmacRegs.rbor.ADDR))
	{
		const u32 ringsize = (dmacRegs.rbsr.RMSK / 16) + 1;
		pxAssertMsg( PSM(dmacRegs.rbor.ADDR+ringsize-1) != NULL, "Scratchpad/MFIFO ringbuffer spans into invalid (unmapped) physical memory!" );
		uint startpos = (addr & dmacRegs.rbsr.RMSK)/16;
		MemCopy_WrappedDest( data, dst, startpos, ringsize, qwc );
	}
	else
	{
		SPR_LOG( "Scratchpad/MFIFO: invalid base physical address: 0x%08x", dmacRegs.rbor.ADDR );
		pxFail( fmt::format( "Scratchpad/MFIFO: Invalid base physical address: 0x{:08x}", u32(dmacRegs.rbor.ADDR)).c_str() );
		return false;
	}

	return true;
}

__ri void hwMFIFOResume() {

	switch (dmacRegs.ctrl.MFD)
	{
		case MFD_VIF1:
		{
			SPR_LOG("Resuming VIF1 MFIFO, Vif CHCR %x Stalled %x done %x", vif1ch.chcr._u32, vif1.vifstalled.enabled, vif1.done);
			if (vif1.inprogress & 0x10)
			{
				vif1.inprogress &= ~0x10;
				if (vif1ch.chcr.STR && !(cpuRegs.interrupt & (1 << DMAC_MFIFO_VIF)) && !vif1Regs.stat.INT)
				{
					SPR_LOG("Data Added, Resuming");
					CPU_INT(DMAC_MFIFO_VIF, cpuRegs.eCycle[DMAC_FROM_SPR]);
				}

			}
			break;
		}
		case MFD_GIF:
		{
			SPR_LOG("Resuming GIF MFIFO, Gif CHCR %x done %x", gifch.chcr._u32, gif.gspath3done);
			if ((gif.gifstate & GIF_STATE_EMPTY)) {
				CPU_INT(DMAC_MFIFO_GIF, cpuRegs.eCycle[DMAC_FROM_SPR]);
				gif.gifstate = GIF_STATE_READY;
			}
			break;
		}
		default:
			break;
	}
}

__ri bool hwDmacSrcChainWithStack(DMACh& dma, int id) {
	switch (id) {
		case TAG_REFE:
			dma.tadr += 16;
			return true;

		case TAG_CNT:
			dma.tadr += 16;
			dma.madr = dma.tadr;
			return false;

		case TAG_NEXT:
		{
			u32 temp = dma.madr;
			dma.madr = dma.tadr + 16;
			dma.tadr = temp;
			return false;
		}
		case TAG_REF:
		case TAG_REFS:
			dma.tadr += 16;
			return false;

		case TAG_CALL:
		{
			u32 temp = dma.madr;
			dma.madr = dma.tadr + 16;

			switch(dma.chcr.ASP)
            {
                case 0:
                    dma.asr0 = dma.madr + (dma.qwc << 4);
                    dma.chcr.ASP++;
                    break;

                case 1:
                    dma.asr1 = dma.madr + (dma.qwc << 4);
                    dma.chcr.ASP++;
                    break;

                default:
                    Console.Warning("Call Stack Overflow (report if it fixes/breaks anything)");
                    return true;
			}

			dma.tadr = temp;

			return false;
		}

		case TAG_RET:
			dma.madr = dma.tadr + 16;

			switch(dma.chcr.ASP)
            {
                case 2:
                    dma.tadr = dma.asr1;
                    dma.asr1 = 0;
                    dma.chcr.ASP--;
                    break;

                case 1:
                    dma.tadr = dma.asr0;
                    dma.asr0 = 0;
                    dma.chcr.ASP--;
                    break;

                case 0:
                    return true;

                default:
                    return true;
            }
			return false;

		case TAG_END:
			dma.madr = dma.tadr + 16;
			return true;
	}

	return false;
}


void hwDmacSrcTadrInc(DMACh& dma)
{
	if (dma.chcr.STR == 0) return;
	if (dma.chcr.MOD != 1) return;

	u16 tagid = (dma.chcr.TAG >> 12) & 0x7;

	if (tagid == TAG_CNT)
	{
		dma.tadr = dma.madr;
	}
}
bool hwDmacSrcChain(DMACh& dma, int id)
{
	u32 temp;

	switch (id)
	{
		case TAG_REFE:
			dma.tadr += 16;
			return true;

		case TAG_CNT:
			dma.madr = dma.tadr + 16;
			dma.tadr = dma.madr;
			return false;

		case TAG_NEXT:
			temp = dma.madr;
			dma.madr = dma.tadr + 16;
			dma.tadr = temp;
			return false;

		case TAG_REF:
		case TAG_REFS:
			dma.tadr += 16;
			return false;

		case TAG_END:
			dma.madr = dma.tadr + 16;
			return true;
		default:
			return true;
	}

	return false;
}

