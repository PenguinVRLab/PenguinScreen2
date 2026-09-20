// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Hardware.h"
#include "Gif_Unit.h"
#include "IopHw.h"
#include "IopMem.h"

#include "ps2/HwInternal.h"
#include "ps2/eeHwTraceLog.inl"

#include "ps2/pgif.h"
#include "SPU2/spu2.h"
#include "R3000A.h"

#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

#include "IopDma.h"

using namespace R5900;

#define HELPSWITCH(m) (((m)>>4) & 0xff)
#define mcase(src) case HELPSWITCH(src)

template< uint page > void _hwWrite8(u32 mem, u8 value);
template< uint page > void _hwWrite16(u32 mem, u8 value);
template< uint page > void TAKES_R128 _hwWrite128(u32 mem, r128 value);


template<uint page>
void _hwWrite32( u32 mem, u32 value )
{
	pxAssume( (mem & 0x03) == 0 );

#if PSX_EXTRALOGS
	if ((mem & 0x1000ff00) == 0x1000f300) DevCon.Warning("32bit Write to SIF Register %x value %x", mem, value);
#endif

	switch (page)
	{
		case 0x00:	if (!rcntWrite32<0x00>(mem, value)) return;	break;
		case 0x01:	if (!rcntWrite32<0x01>(mem, value)) return;	break;

		case 0x02:
			if (!ipuWrite32(mem, value)) return;
		break;

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{

			u128 zerofill = u128::From32(0);
			zerofill._u32[(mem >> 2) & 0x03] = value;

			_hwWrite128<page>(mem & ~0x0f, r128_from_u128(zerofill));
		}
		return;

		case 0x03:
			if (mem >= EEMemoryMap::VIF0_Start)
			{
				if(mem >= EEMemoryMap::VIF1_Start)
				{
					if (!vifWrite32<1>(mem, value)) return;
				}
				else
				{
					if (!vifWrite32<0>(mem, value)) return;
				}
			}
			else switch(mem)
			{
				case (GIF_CTRL):
				{
					gifRegs.ctrl.write(value & 9);
					if (gifRegs.ctrl.RST) {
						GUNIT_LOG("GIF CTRL - Reset");
						gifUnit.Reset(true);
					}
					gifRegs.stat.PSE = gifRegs.ctrl.PSE;
					return;
				}

				case (GIF_MODE):
				{
					gifRegs.mode.write(value);
					if (gifRegs.stat.M3R == 1 && gifRegs.mode.M3R == 0 && (gifch.chcr.STR || gif_fifo.fifoSize))
					{
						DevCon.Warning("GIF Mode cancelling P3 Disable");
						CPU_INT(DMAC_GIF, 8);
					}


					gifRegs.stat.M3R = gifRegs.mode.M3R;
					gifRegs.stat.IMT = gifRegs.mode.IMT;
					return;
				}
			}
		break;

		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0b:
		case 0x0c:
		case 0x0d:
		case 0x0e:
			if (!dmacWrite32<page>(mem, value)) return;
		break;

		case 0x0f:
		{
			switch( HELPSWITCH(mem) )
			{
				mcase(INTC_STAT):
					psHu32(INTC_STAT) &= ~value;
				return;

				mcase(INTC_MASK):
					psHu32(INTC_MASK) ^= (u16)value;
					cpuTestINTCInts();
				return;

				mcase(SIO_TXFIFO):
				{
					u8* woot = (u8*)&value;
					_hwWrite8<0x0f>(SIO_TXFIFO, woot[0]);
					_hwWrite8<0x0f>(SIO_TXFIFO, woot[1]);
					_hwWrite8<0x0f>(SIO_TXFIFO, woot[2]);
					_hwWrite8<0x0f>(SIO_TXFIFO, woot[3]);
				}
				return;

				mcase(SBUS_F200):
				break;

				mcase(SBUS_F220):
					psHu32(mem) |= value;
				return;

				mcase(SBUS_F230):
					psHu32(mem) &= ~value;
				return;

				mcase(SBUS_F240):
					if (value & (1 << 18))
					{
						iopIntcIrq(1);
					}
					if (value & (1 << 19))
					{
						u64 cycle = psxRegs.cycle;
						psxReset();
						PSXCLK =  33868800;
						SPU2::Reset(true);
						setPs1CDVDSpeed(cdvd.Speed);
						psxHu32(HW_ICFG) = 0x8;
						psxHu32(HW_ICTRL) = 1;
						psxRegs.cycle = cycle;
					}
					if(!(value & 0x100))
						psHu32(mem) &= ~0x100;
					else
						psHu32(mem) |= 0x100;
				return;

				mcase(SBUS_F260):
#if PSX_EXTRALOGS
					DevCon.Warning("Write  SBUS_F260  %x ", psHu32(SBUS_F260));
#endif
					psHu32(mem) = value;
				return;

#if 0
				mcase(SBUS_F300) :
					psxHu32(0x1f801814) = value;
				return;
				mcase(SBUS_F380) :
					psHu32(mem) = value;
				return;
#endif

				mcase(MCH_RICM):
					if ((((value >> 16) & 0xFFF) == 0x21) && (((value >> 6) & 0xF) == 1) && (((psHu32(0xf440) >> 7) & 1) == 0))
						rdram_sdevid = 0;
					psHu32(mem) = value & ~0x80000000;
				return;

				mcase(MCH_DRD):
				break;

				mcase(DMAC_ENABLEW):
					if (!dmacWrite32<0x0f>(DMAC_ENABLEW, value)) return;
				break;

				default:
					if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
						PGIFw((mem & 0x1FFFFFFF), value);
						return;
					}

			}
		}
		break;
	}

	psHu32(mem) = value;
}

template<uint page>
void hwWrite32( u32 mem, u32 value )
{
	eeHwTraceLog( mem, value, false );
	_hwWrite32<page>( mem, value );
}

template< uint page >
void _hwWrite8(u32 mem, u8 value)
{
#if PSX_EXTRALOGS
	if ((mem & 0x1000ff00) == 0x1000f300) DevCon.Warning("8bit Write to SIF Register %x value %x wibble", mem, value);
#endif
	if (mem == SIO_TXFIFO)
	{
		static bool last_char_was_cr = false;
		
		if(last_char_was_cr && (value == '\n'))
		{
			last_char_was_cr = false;
			return;
		}
		
		last_char_was_cr = value == '\r';
		bool should_flush_cause_newline = false;
		if (last_char_was_cr)
		{
			should_flush_cause_newline = true;
			ee_sio_tx_fifo.push_back('\n');
		}
		else
		{
			should_flush_cause_newline = value == '\n';
			ee_sio_tx_fifo.push_back(value);
		}

		if (ee_sio_tx_fifo.size() == 1024 || should_flush_cause_newline)
		{
			std::string output_string(ee_sio_tx_fifo.begin(), ee_sio_tx_fifo.end());

			eeConLog(ShiftJIS_ConvertString(output_string.c_str()));
			ee_sio_tx_fifo.clear();
		}
		return;
	}

	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			DevCon.Warning ( "8bit write mem = %x value %x", mem, value );
			_hwWrite32<page>(mem & ~3, (u32)value << (mem & 3) * 8);
			return;
	}

	u32 merged = _hwRead32<page,false>(mem & ~0x03);
	((u8*)&merged)[mem & 0x3] = value;

	_hwWrite32<page>(mem & ~0x03, merged);
}

template< uint page >
void hwWrite8(u32 mem, u8 value)
{
	eeHwTraceLog( mem, value, false );
	_hwWrite8<page>(mem, value);
}

template< uint page >
void _hwWrite16(u32 mem, u16 value)
{
	pxAssume( (mem & 0x01) == 0 );
#if PSX_EXTRALOGS
	if ((mem & 0x1000ff00) == 0x1000f300) DevCon.Warning("16bit Write to SIF Register %x wibble", mem);
#endif
	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			DevCon.Warning ( "16bit write mem = %x value %x", mem, value );
			_hwWrite32<page>(mem & ~3, (u32)value << (mem & 3) * 8);
			return;
	}

	u32 merged = _hwRead32<page,false>(mem & ~0x03);
	((u16*)&merged)[(mem>>1) & 0x1] = value;

	hwWrite32<page>(mem & ~0x03, merged);
}

template< uint page >
void hwWrite16(u32 mem, u16 value)
{
	eeHwTraceLog( mem, value, false );
	_hwWrite16<page>(mem, value);
}

template<uint page>
void _hwWrite64( u32 mem, u64 value )
{
	pxAssume( (mem & 0x07) == 0 );

#if PSX_EXTRALOGS
	if ((mem & 0x1000ff00) == 0x1000f300) DevCon.Warning("64bit Write to SIF Register %x wibble", mem);
#endif
	switch (page)
	{
		case 0x02:
			if (!ipuWrite64(mem, value)) return;
		break;

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			u128 zerofill = u128::From32(0);
			zerofill._u64[(mem >> 3) & 0x01] = value;
			hwWrite128<page>(mem & ~0x0f, r128_from_u128(zerofill));
		}
		return;

		default:
			hwWrite32<page>( mem, value );
		return;
	}

	std::memcpy(&eeHw[(mem) & 0xffff], &value, sizeof(value));
}

template<uint page>
void hwWrite64( u32 mem, mem64_t value )
{
	eeHwTraceLog( mem, value, false );
	_hwWrite64<page>(mem, value);
}

template< uint page >
void TAKES_R128 _hwWrite128(u32 mem, r128 srcval)
{
	pxAssume( (mem & 0x0f) == 0 );

#if PSX_EXTRALOGS
	if ((mem & 0x1000ff00) == 0x1000f300) DevCon.Warning("128bit Write to SIF Register %x wibble", mem);
#endif

	switch (page)
	{
		case 0x04:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_VIF0(&usrcval);
			}
		return;

		case 0x05:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_VIF1(&usrcval);
			}
		return;

		case 0x06:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_GIF(&usrcval);
			}
		return;

		case 0x07:
			if (mem & 0x10)
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_IPUin(&usrcval);
			}
			else
			{

			}

		return;

		case 0x0F:
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				PGIFwQword((mem & 0x1FFFFFFF), (void*)&usrcval);
				return;
			}

		default: break;
	}

	hwWrite64<page>(mem, r128_to_u64(srcval));

}

template< uint page >
void TAKES_R128 hwWrite128(u32 mem, r128 srcval)
{
	eeHwTraceLog( mem, srcval, false );
	_hwWrite128<page>(mem, srcval);
}

#define InstantizeHwWrite(pageidx) \
	template void hwWrite8<pageidx>(u32 mem, mem8_t value); \
	template void hwWrite16<pageidx>(u32 mem, mem16_t value); \
	template void hwWrite32<pageidx>(u32 mem, mem32_t value); \
	template void hwWrite64<pageidx>(u32 mem, mem64_t value); \
	template void TAKES_R128 hwWrite128<pageidx>(u32 mem, r128 srcval);

InstantizeHwWrite(0x00);	InstantizeHwWrite(0x08);
InstantizeHwWrite(0x01);	InstantizeHwWrite(0x09);
InstantizeHwWrite(0x02);	InstantizeHwWrite(0x0a);
InstantizeHwWrite(0x03);	InstantizeHwWrite(0x0b);
InstantizeHwWrite(0x04);	InstantizeHwWrite(0x0c);
InstantizeHwWrite(0x05);	InstantizeHwWrite(0x0d);
InstantizeHwWrite(0x06);	InstantizeHwWrite(0x0e);
InstantizeHwWrite(0x07);	InstantizeHwWrite(0x0f);
