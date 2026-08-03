// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "SIO/Sio2.h"
#include "SIO/Sio0.h"
#include "CDVD/CDVD.h"
#include "CDVD/Ps1CD.h"
#include "IopCounters.h"
#include "IopDma.h"
#include "IopHw.h"
#include "Mdec.h"
#include "R3000A.h"

void psxHwReset() {

	memset(iopHw, 0, 0x10000);

	mdecInit();
	cdrReset();
	cdvdReset();
	psxRcntInit();
}

__fi u8 psxHw4Read8(u32 add)
{
	u16 mem = add & 0xFF;
	u8 ret = cdvdRead(mem);
	PSXHW_LOG("HwRead8 from Cdvd [segment 0x1f40], addr 0x%02x = 0x%02x", mem, ret);
	return ret;
}

__fi void psxHw4Write8(u32 add, u8 value)
{
	u8 mem = (u8)add;
	cdvdWrite(mem, value);
	PSXHW_LOG("HwWrite8 to Cdvd [segment 0x1f40], addr 0x%02x = 0x%02x", mem, value);
}

void psxDmaInterrupt(int n)
{
	if(n == 33) {
		for (int i = 0; i < 6; i++) {
			if (HW_DMA_ICR & (1 << (16 + i))) {
				if (HW_DMA_ICR & (1 << (24 + i))) {
					if (HW_DMA_ICR & (1 << 23)) {
						HW_DMA_ICR |= 0x80000000;
					}
					psxRegs.CP0.n.Cause &= ~0x7C;
					iopIntcIrq(3);
					break;
				}
			}
		}
	} else if (HW_DMA_ICR & (1 << (16 + n)))
	{
		HW_DMA_ICR |= (1 << (24 + n));
		if (HW_DMA_ICR & (1 << 23)) {
			HW_DMA_ICR |= 0x80000000;
		}
		iopIntcIrq(3);
	}
}

void psxDmaInterrupt2(int n)
{
	bool fire_interrupt = n == 2 || n == 3;

	if (n == 33) {
		for (int i = 0; i < 6; i++) {
			if (HW_DMA_ICR2 & (1 << (24 + i))) {
				if (HW_DMA_ICR2 & (1 << (16 + i)) || i == 2 || i == 3) {
					fire_interrupt = true;
					break;
				}
			}
		}
	}
	else if (HW_DMA_ICR2 & (1 << (16 + n)))
	{
		fire_interrupt = true;
	}

	if (fire_interrupt)
	{
		if(n != 33)
			HW_DMA_ICR2 |= (1 << (24 + n));

		if (HW_DMA_ICR2 & (1 << 23)) {
			HW_DMA_ICR2 |= 0x80000000;
		}
		iopIntcIrq(3);
	}
}
