// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Hardware.h"
#include "MTVU.h"

#include "IPU/IPUdma.h"
#include "ps2/HwInternal.h"

bool DMACh::transfer(const char *s, tDMA_TAG* ptag)
{
	if (ptag == NULL)
	{
		throwBusError(s);
		return false;
	}
    chcrTransfer(ptag);

    qwcTransfer(ptag);
    return true;
}

void DMACh::unsafeTransfer(tDMA_TAG* ptag)
{
    chcrTransfer(ptag);
    qwcTransfer(ptag);
}

tDMA_TAG *DMACh::getAddr(u32 addr, u32 num, bool write)
{
	tDMA_TAG *ptr = dmaGetAddr(addr, write);
	if (ptr == NULL)
	{
		throwBusError("dmaGetAddr");
		setDmacStat(num);
		chcr.STR = false;
	}

	return ptr;
}

tDMA_TAG *DMACh::DMAtransfer(u32 addr, u32 num)
{
	tDMA_TAG *tag = getAddr(addr, num, false);

	if (tag == NULL) return NULL;

    chcrTransfer(tag);
    qwcTransfer(tag);
    return tag;
}

tDMA_TAG DMACh::dma_tag()
{
	return chcr.tag();
}

std::string DMACh::cmq_to_str() const
{
	return StringUtil::StdStringFromFormat("chcr = %x, madr = %x, qwc  = %x", chcr._u32, madr, qwc);
}

std::string DMACh::cmqt_to_str() const
{
	return StringUtil::StdStringFromFormat("chcr = %x, madr = %x, qwc  = %x, tadr = %1x", chcr._u32, madr, qwc, tadr);
}

__fi void throwBusError(const char *s)
{
    Console.Error("%s BUSERR", s);
    dmacRegs.stat.BEIS = true;
}

__fi void setDmacStat(u32 num)
{
	dmacRegs.stat.set_flags(1 << num);
}

__fi tDMA_TAG* SPRdmaGetAddr(u32 addr, bool write)
{

	if((addr & 0x70000000) == 0x70000000)
	{
		return (tDMA_TAG*)&eeMem->Scratch[addr & 0x3ff0];
	}

	addr &= 0x1ffffff0;

	if (addr < Ps2MemSize::ExposedRam)
	{
		return (tDMA_TAG*)&eeMem->Main[addr];
	}
	else if (addr < 0x10000000)
	{
		return (tDMA_TAG*)(write ? eeMem->ZeroWrite : eeMem->ZeroRead);
	}
	else if ((addr >= 0x11000000) && (addr < 0x11010000))
	{
		if (addr >= 0x11008000 && THREAD_VU1)
		{
			DevCon.Warning("MTVU: SPR Accessing VU1 Memory");
			vu1Thread.WaitVU();
		}

		if((addr >= 0x1100c000) && (addr < 0x11010000))
		{
			return (tDMA_TAG*)(VU1.Mem + (addr & 0x3ff0));
		}

		if((addr >= 0x11004000) && (addr < 0x11008000))
		{
			return (tDMA_TAG*)(VU0.Mem + (addr & 0xff0));
		}

		if((addr >= 0x11000000) && (addr < 0x11004000))
		{
			return (tDMA_TAG*)(VU0.Micro + (addr & 0xff0));
		}

		if((addr >= 0x11008000) && (addr < 0x1100c000))
		{
			return (tDMA_TAG*)(VU1.Micro + (addr & 0x3ff0));
		}


		return NULL;
	}
	else
	{
		Console.Error( "*PCSX2*: DMA error: %8.8x", addr);
		return NULL;
	}
}

__ri tDMA_TAG *dmaGetAddr(u32 addr, bool write)
{
	if (DMA_TAG(addr).SPR) return (tDMA_TAG*)&eeMem->Scratch[addr & 0x3ff0];

	addr &= 0x1ffffff0;

	if (addr < Ps2MemSize::ExposedRam)
	{
		return (tDMA_TAG*)&eeMem->Main[addr];
	}
	else if (addr < 0x10000000)
	{
		return (tDMA_TAG*)(write ? eeMem->ZeroWrite : eeMem->ZeroRead);
	}
	else if (addr < 0x10004000)
	{
		return (tDMA_TAG*)&eeMem->Scratch[addr & 0x3ff0];
	}
	else
	{
		Console.Error( "*PCSX2*: DMA error: %8.8x", addr);
		return NULL;
	}
}


static bool QuickDmaExec( void (*func)(), u32 mem)
{
	bool ret = false;
    DMACh& reg = (DMACh&)psHu32(mem);

	if (reg.chcr.STR && dmacRegs.ctrl.DMAE && !psHu8(DMAC_ENABLER+2))
	{
		func();
		ret = true;
	}

	return ret;
}


static tDMAC_QUEUE QueuedDMA(0);
static u32 oldvalue = 0;

static void StartQueuedDMA()
{
	if (QueuedDMA.VIF0) { DMA_LOG("Resuming DMA for VIF0"); QueuedDMA.VIF0 = !QuickDmaExec(dmaVIF0, D0_CHCR); }
	if (QueuedDMA.VIF1) { DMA_LOG("Resuming DMA for VIF1"); QueuedDMA.VIF1 = !QuickDmaExec(dmaVIF1, D1_CHCR); }
	if (QueuedDMA.GIF ) { DMA_LOG("Resuming DMA for GIF" ); QueuedDMA.GIF  = !QuickDmaExec(dmaGIF , D2_CHCR); }
	if (QueuedDMA.IPU0) { DMA_LOG("Resuming DMA for IPU0"); QueuedDMA.IPU0 = !QuickDmaExec(dmaIPU0, D3_CHCR); }
	if (QueuedDMA.IPU1) { DMA_LOG("Resuming DMA for IPU1"); QueuedDMA.IPU1 = !QuickDmaExec(dmaIPU1, D4_CHCR); }
	if (QueuedDMA.SIF0) { DMA_LOG("Resuming DMA for SIF0"); QueuedDMA.SIF0 = !QuickDmaExec(dmaSIF0, D5_CHCR); }
	if (QueuedDMA.SIF1) { DMA_LOG("Resuming DMA for SIF1"); QueuedDMA.SIF1 = !QuickDmaExec(dmaSIF1, D6_CHCR); }
	if (QueuedDMA.SIF2) { DMA_LOG("Resuming DMA for SIF2"); QueuedDMA.SIF2 = !QuickDmaExec(dmaSIF2, D7_CHCR); }
	if (QueuedDMA.SPR0) { DMA_LOG("Resuming DMA for SPR0"); QueuedDMA.SPR0 = !QuickDmaExec(dmaSPR0, D8_CHCR); }
	if (QueuedDMA.SPR1) { DMA_LOG("Resuming DMA for SPR1"); QueuedDMA.SPR1 = !QuickDmaExec(dmaSPR1, D9_CHCR); }
}

static __ri void DmaExec( void (*func)(), u32 mem, u32 value )
{
	DMACh& reg = (DMACh&)psHu32(mem);
    tDMA_CHCR chcr(value);

	if (reg.chcr.STR)
	{
		const uint channel = ChannelNumber(mem);

		if(chcr.STR == 0)
		{
			reg.chcr.STR = 0;

			if(channel == 1)
			{
				cpuClearInt( 10 );
				QueuedDMA._u16 &= ~(1 << 10);
			}
			else if (channel == 2)
			{
				cpuClearInt( 11 );
				QueuedDMA._u16 &= ~(1 << 11);
			}

			cpuClearInt( channel );
			QueuedDMA._u16 &= ~(1 << channel);
		}
		return;
	}

	reg.chcr.set(value);

	if (reg.chcr.MOD == 0x3)
	{
		static bool warned;
		if (!warned)
		{
			DevCon.Warning("%s CHCR.MOD set to 3, assuming 1 (chain)", ChcrName(mem));
			warned = true;
		}
		reg.chcr.MOD = 0x1;
	}

	if (reg.chcr.STR && !reg.chcr.MOD && reg.qwc == 0)
		reg.qwc = 0x10000;

	if (reg.chcr.STR && dmacRegs.ctrl.DMAE && !psHu8(DMAC_ENABLER+2))
	{
		func();
	}
	else if (reg.chcr.STR)
	{
		QueuedDMA._u16 |= (1 << ChannelNumber(mem));
	}
}

template< uint page >
__fi u32 dmacRead32( u32 mem )
{
	if ((CHECK_OPHFLAGHACK) && (page << 12) == (mem & (0xf << 12)) && (mem == GIF_STAT))
	{
		static unsigned counter = 1;
		if (++counter == 8)
			counter = 2;
		return (gifRegs.stat._u32 & ~(7 << 9)) | ((counter & 1) ? (counter << 9) : 0);
	}

	return psHu32(mem);
}

template< uint page >
__fi bool dmacWrite32( u32 mem, mem32_t& value )
{
	if (CHECK_DMABUSYHACK && (mem & 0xf0) && mem >= 0x10008000 && mem <= 0x1000E000)
	{
		if ((psHu32(mem & ~0xff) & 0x100) && dmacRegs.ctrl.DMAE && !psHu8(DMAC_ENABLER + 2))
		{
			while (psHu32(mem & ~0xff) & 0x100)
			{
				switch ((mem >> 8) & 0xFF)
				{
					case 0x80:
						vif0Interrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_VIF0);
						break;
					case 0x90:
						if (vif1Regs.stat.VEW)
						{
							vu1Finish(false);
							vif1VUFinish();
						}
						else
							vif1Interrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_VIF1);
						break;
					case 0xA0:
						gifInterrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_GIF);
						break;
					case 0xB0:
						[[fallthrough]];
					case 0xB4:
						if ((mem & 0xff) == 0x20)
							goto allow_write;
						else
							return false;
						break;
					case 0xD0:
						SPRFROMinterrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_FROM_SPR);
						break;
					case 0xD4:
						SPRTOinterrupt();
						cpuRegs.interrupt &= ~(1 << DMAC_TO_SPR);
						break;
					default:
						return false;
				}
			}
		}
		allow_write:;
	}

	switch(mem) {

		case (D0_QWC):
		case (D1_QWC):
		case (D2_QWC):
		case (D3_QWC):
		case (D4_QWC):
		case (D5_QWC):
		case (D6_QWC):
		case (D7_QWC):
		case (D8_QWC):
		case (D9_QWC):
		{
			psHu32(mem) = (u16)value;
			return false;
		}

		case (D0_CHCR):
		{
			DMA_LOG("VIF0dma EXECUTE, value=0x%x", value);
			DmaExec(dmaVIF0, mem, value);
			return false;
		}

		case (D1_CHCR):
		{
			DMA_LOG("VIF1dma EXECUTE, value=0x%x", value);
			DmaExec(dmaVIF1, mem, value);
			return false;
		}

		case (D2_CHCR):
		{
			DMA_LOG("GIFdma EXECUTE, value=0x%x", value);
			DmaExec(dmaGIF, mem, value);
			return false;
		}

		case (D3_CHCR):
		{
			DMA_LOG("IPU0dma EXECUTE, value=0x%x\n", value);
			DmaExec(dmaIPU0, mem, value);
			return false;
		}

		case (D4_CHCR):
		{
			DMA_LOG("IPU1dma EXECUTE, value=0x%x\n", value);
			DmaExec(dmaIPU1, mem, value);
			return false;
		}

		case (D5_CHCR):
		{
			DMA_LOG("SIF0dma EXECUTE, value=0x%x", value);
			DmaExec(dmaSIF0, mem, value);
			return false;
		}

		case (D6_CHCR):
		{
			DMA_LOG("SIF1dma EXECUTE, value=0x%x", value);
			DmaExec(dmaSIF1, mem, value);
			return false;
		}

		case (D7_CHCR):
		{
			DMA_LOG("SIF2dma EXECUTE, value=0x%x", value);
			DmaExec(dmaSIF2, mem, value);
			return false;
		}

		case (D8_CHCR):
		{
			DMA_LOG("SPR0dma EXECUTE (fromSPR), value=0x%x", value);
			DmaExec(dmaSPR0, mem, value);
			return false;
		}

		case (D9_CHCR):
		{
			DMA_LOG("SPR1dma EXECUTE (toSPR), value=0x%x", value);
			DmaExec(dmaSPR1, mem, value);
			return false;
		}

		case (fromSPR_MADR):
		case (toSPR_MADR):
		{
			psHu32(mem) = value & 0x7FFFFFFF;
			return false;
		}

		case (fromSPR_SADR):
		case (toSPR_SADR):
		{
			psHu32(mem) = value & 0x3FF0;
			return false;
		}

		case (DMAC_CTRL):
		{
			u32 oldvalue = psHu32(mem);

			HW_LOG("DMAC_CTRL Write 32bit %x", value);

			psHu32(mem) = value;
			if (((oldvalue & 0x1) == 0) && ((value & 0x1) == 1))
			{
				if (!QueuedDMA.empty()) StartQueuedDMA();
			}
#ifdef PCSX2_DEVBUILD
			if ((oldvalue & 0x30) != (value & 0x30))
			{
				std::string new_source;

				switch ((value & 0x30) >> 4)
				{
				case 1:
					new_source = "SIF0";
					break;
				case 2:
					new_source = "fromSPR";
					break;
				case 3:
					new_source = "fromIPU";
					break;
				default:
					new_source = "None";
					break;
				}
			}
			if ((oldvalue & 0xC0) != (value & 0xC0))
			{
				std::string new_dest;

				switch ((value & 0xC0) >> 6)
				{
				case 1:
					new_dest = "VIF1";
					break;
				case 2:
					new_dest = "GIF";
					break;
				case 3:
					new_dest = "SIF1";
					break;
				default:
					new_dest = "None";
					break;
				}
			}
#endif
			return false;
		}

		case (DMAC_FAKESTAT):
		case (DMAC_STAT):
		{
			if (mem == DMAC_FAKESTAT)
			{
				HW_LOG("Midways own DMAC_STAT Write 32bit %x", value);
			}
			else HW_LOG("DMAC_STAT Write 32bit %x", value);

			psHu16(0xe010) &= ~(value & 0xffff);
			psHu16(0xe012) ^= (u16)(value >> 16);

			cpuTestDMACInts();
			return false;
		}

		case (DMAC_ENABLEW):
		{
			HW_LOG("DMAC_ENABLEW Write 32bit %lx", value);
			oldvalue = psHu8(DMAC_ENABLEW + 2);
			psHu32(DMAC_ENABLEW) = value;
			psHu32(DMAC_ENABLER) = value;
			if (((oldvalue & 0x1) == 1) && (((value >> 16) & 0x1) == 0))
			{
				if (!QueuedDMA.empty()) StartQueuedDMA();
			}
			return false;
		}
		default:
			return true;
	}

	return true;
}

template u32 dmacRead32<0x03>( u32 mem );

template bool dmacWrite32<0x00>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x01>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x02>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x03>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x04>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x05>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x06>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x07>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x08>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x09>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0a>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0b>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0c>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0d>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0e>( u32 mem, mem32_t& value );
template bool dmacWrite32<0x0f>( u32 mem, mem32_t& value );
