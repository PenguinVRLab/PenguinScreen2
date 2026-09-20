// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Hardware.h"
#include "IopHw.h"
#include "ps2/HwInternal.h"
#include "ps2/eeHwTraceLog.inl"

#include "ps2/pgif.h"

using namespace R5900;

static __fi void IntCHackCheck()
{
	s64 diff = cpuRegs.nextEventCycle - cpuRegs.cycle;
	if (diff > 0 && (cpuRegs.cycle - cpuRegs.lastEventCycle) > 8) cpuRegs.cycle = cpuRegs.nextEventCycle;
}

template< uint page > RETURNS_R128 _hwRead128(u32 mem);

template< uint page, bool intcstathack >
mem32_t _hwRead32(u32 mem)
{
	pxAssume( (mem & 0x03) == 0 );

	switch( page )
	{
		case 0x00:	return rcntRead32<0x00>( mem );
		case 0x01:	return rcntRead32<0x01>( mem );

		case 0x02:	return ipuRead32( mem );

		case 0x03:
			if (mem >= EEMemoryMap::VIF0_Start)
			{
				if(mem >= EEMemoryMap::VIF1_Start)
					return vifRead32<1>(mem);
				else
					return vifRead32<0>(mem);
			}
			return dmacRead32<0x03>( mem );

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{

			r128 out128 = _hwRead128<page>(mem & ~0x0f);
			return reinterpret_cast<u32*>(&out128)[(mem >> 2) & 0x3];
		}
		break;

		case 0x0f:
		{

			if (mem == INTC_STAT)
			{
				if (intcstathack && !(psxHu32(HW_ICFG) & (1 << 3))) IntCHackCheck();
				return psHu32(INTC_STAT);
			}

			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				return PGIFr((mem & 0x1FFFFFFF));
			}

			if ((mem & 0x1000ff00) == 0x1000f300)
			{
				int ret = 0;
				u32 sif2fifosize = std::min(sif2.fifo.size, 7);

				switch (mem & 0xf0)
				{
				case 0x00:
					ret = psxHu32(0x1f801814);
					break;
				case 0x80:
#if PSX_EXTRALOGS
					DevCon.Warning("FIFO Size %x", sif2fifosize);
#endif
					ret = psHu32(mem) | (sif2fifosize << 16);
					if (sif2.fifo.size > 0) ret |= 0x80000000;
					break;
				case 0xc0:
					ReadFifoSingleWord();
					ret = psHu32(mem);
					break;
				case 0xe0:
					if (sif2.fifo.size > 0)
					{
						ReadFifoSingleWord();
						ret = psHu32(mem);
					}
					else ret = 0;
					break;
				}
#if PSX_EXTRALOGS
				DevCon.Warning("SBUS read %x value sending %x", mem, ret);
#endif
				return ret;


			}
			switch( mem )
			{
				case SIO_ISR:
					

					

					if(!ee_sio_rx_fifo.empty())
						return 0xf00;

					return 0x0;
					break;
				case 0x1000f410:
				case MCH_RICM:
					return 0;

				case SBUS_F240:
#if PSX_EXTRALOGS
					DevCon.Warning("Read  SBUS_F240  %x ", psHu32(SBUS_F240));
#endif
					return psHu32(SBUS_F240) | 0xF0000102;
				case SBUS_F260:
#if PSX_EXTRALOGS
					DevCon.Warning("Read  SBUS_F260  %x ", psHu32(SBUS_F260));
#endif
					return psHu32(SBUS_F260);
				case MCH_DRD:
					if( !((psHu32(MCH_RICM) >> 6) & 0xF) )
					{
						switch ((psHu32(MCH_RICM)>>16) & 0xFFF)
						{

							case 0x21:
								if(rdram_sdevid < rdram_devices)
								{
									rdram_sdevid++;
									return 0x1F;
								}
							return 0;

							case 0x23:
								return 0x0D0D;

							case 0x24:
								return 0x0090;

							case 0x40:
								return psHu32(MCH_RICM) & 0x1F;
						}
					}
				return 0;
			}
		}
		break;
		default: break;
	}
	if(mem == (D1_CHCR + 0x10) && CHECK_VIFFIFOHACK)
		return psHu32(mem) + (vif1ch.qwc * 16);

	return psHu32(mem);
}

template< uint page >
mem32_t hwRead32(u32 mem)
{
	mem32_t retval = _hwRead32<page,false>(mem);
	eeHwTraceLog( mem, retval, true );
	return retval;
}

mem32_t hwRead32_page_0F_INTC_HACK(u32 mem)
{
	mem32_t retval = _hwRead32<0x0f,true>(mem);
	eeHwTraceLog( mem, retval, true );
	return retval;
}

template< uint page >
mem8_t _hwRead8(u32 mem)
{
	if(mem == SIO_RXFIFO)
	{
		if(ee_sio_rx_fifo.empty())
			return 0;
		
		const char c = ee_sio_rx_fifo.front();
		ee_sio_rx_fifo.pop_front();
		return c;
	}

	u32 ret32 = _hwRead32<page, false>(mem & ~0x03);
	return ((u8*)&ret32)[mem & 0x03];
}

template< uint page >
mem8_t hwRead8(u32 mem)
{
	mem8_t ret8 = _hwRead8<page>(mem);
	eeHwTraceLog( mem, ret8, true );
	return ret8;
}

template< uint page >
mem16_t _hwRead16(u32 mem)
{
	pxAssume( (mem & 0x01) == 0 );

	u32 ret32 = _hwRead32<page, false>(mem & ~0x03);
	return ((u16*)&ret32)[(mem>>1) & 0x01];
}

template< uint page >
mem16_t hwRead16(u32 mem)
{
	u16 ret16 = _hwRead16<page>(mem);
	eeHwTraceLog( mem, ret16, true );
	return ret16;
}

mem16_t hwRead16_page_0F_INTC_HACK(u32 mem)
{
	pxAssume( (mem & 0x01) == 0 );

	u32 ret32 = _hwRead32<0x0f, true>(mem & ~0x03);
	u16 ret16 = ((u16*)&ret32)[(mem>>1) & 0x01];

	eeHwTraceLog( mem, ret16, true );
	return ret16;
}

template< uint page >
static u64 _hwRead64(u32 mem)
{
	pxAssume( (mem & 0x07) == 0 );

	switch (page)
	{
		case 0x02:
			return ipuRead64(mem);

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{

			uint wordpart = (mem >> 3) & 0x1;
			r128 full = _hwRead128<page>(mem & ~0x0f);
			return *(reinterpret_cast<u64*>(&full) + wordpart);
		}
		case 0x0F:
			if ((mem & 0xffffff00) == 0x1000f300)
			{
				DevCon.Warning("64bit read from %x wibble", mem);
				if (mem == 0x1000f3E0)
				{

					ReadFifoSingleWord();
					u32 lo = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 hi = psHu32(0x1000f3E0);
					return static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
				}
			}
		default: break;
	}

	return static_cast<u64>(_hwRead32<page, false>(mem));
}

template< uint page >
mem64_t hwRead64(u32 mem)
{
	u64 res = _hwRead64<page>(mem);
	eeHwTraceLog(mem, res, true);
	return res;
}

template< uint page >
RETURNS_R128 _hwRead128(u32 mem)
{
	pxAssume( (mem & 0x0f) == 0 );

	alignas(16) mem128_t result;

	switch (page)
	{
		case 0x05:
			ReadFIFO_VIF1(&result);
			break;

		case 0x07:
			if (mem & 0x10)
				return r128_zero();
			else
				ReadFIFO_IPUout(&result);
			break;

		case 0x04:
		case 0x06:
			return r128_zero();
		case 0x0F:
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				PGIFrQword((mem & 0x1FFFFFFF), &result);
				break;
			}

			if ((mem & 0xffffff00) == 0x1000f300)
			{
				DevCon.Warning("128bit read from %x wibble", mem);
				if (mem == 0x1000f3E0)
				{

					ReadFifoSingleWord();
					u32 part0 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part1 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part2 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part3 = psHu32(0x1000f3E0);
					return r128_from_u32x4(part0, part1, part2, part3);
				}
			}
			break;

		default:
			return r128_from_u64_dup(_hwRead64<page>(mem));
	}
	return r128_load(&result);
}

template< uint page >
RETURNS_R128 hwRead128(u32 mem)
{
	r128 res = _hwRead128<page>(mem);
	eeHwTraceLog(mem, res, true);
	return res;
}

#define InstantizeHwRead(pageidx) \
	template mem8_t hwRead8<pageidx>(u32 mem); \
	template mem16_t hwRead16<pageidx>(u32 mem); \
	template mem32_t hwRead32<pageidx>(u32 mem); \
	template mem64_t hwRead64<pageidx>(u32 mem); \
	template RETURNS_R128 hwRead128<pageidx>(u32 mem); \
	template mem32_t _hwRead32<pageidx, false>(u32 mem);

InstantizeHwRead(0x00);	InstantizeHwRead(0x08);
InstantizeHwRead(0x01);	InstantizeHwRead(0x09);
InstantizeHwRead(0x02);	InstantizeHwRead(0x0a);
InstantizeHwRead(0x03);	InstantizeHwRead(0x0b);
InstantizeHwRead(0x04);	InstantizeHwRead(0x0c);
InstantizeHwRead(0x05);	InstantizeHwRead(0x0d);
InstantizeHwRead(0x06);	InstantizeHwRead(0x0e);
InstantizeHwRead(0x07);	InstantizeHwRead(0x0f);
