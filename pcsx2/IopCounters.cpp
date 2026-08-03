// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Note on INTC usage: All counters code is always called from inside the context of an
// event test, so instead of using the iopTestIntc we just set the 0x1070 flags directly.
// The EventText function will pick it up.

#include "IopCounters.h"
#include "R3000A.h"
#include "Common.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9.h"
#include "USB/USB.h"
#include "IopHw.h"
#include "IopDma.h"
#include "CDVD/CDVD.h"

#include <math.h>

#define PSXPIXEL 3
#define PSXSOUNDCLK ((int)(48000))

psxCounter psxCounters[NUM_COUNTERS];
s32 psxNextDeltaCounter;
u64 psxNextStartCounter;

bool hBlanking = false;
bool vBlanking = false;

#define IOPCNT_STOPPED (0x10000000ul)

#define IOPCNT_FUTURE_TARGET (0x1000000000ULL)
#define IOPCNT_MODE_WRITE_MSK 0x63FF
#define IOPCNT_MODE_FLAG_MSK 0x1C00

#define IOPCNT_GATE_CNT_LOW 0
#define IOPCNT_GATE_CLR_END 1
#define IOPCNT_GATE_CNT_HIGH_ZERO_OFF 2
#define IOPCNT_GATE_START_AT_END 3

#define PSXHBLANK 0x2001

static bool psxRcntCanCount(int cntidx)
{
	if (psxCounters[cntidx].mode.stopped)
		return false;

	if (!(psxCounters[cntidx].mode.gateEnable))
		return true;

	const u32 gateMode = psxCounters[cntidx].mode.gateMode;

	if (cntidx == 2 || cntidx == 4 || cntidx == 5)
	{
		return (gateMode & 1);
	}

	const bool blanking = cntidx == 0 ? hBlanking : vBlanking;

	if ((gateMode == IOPCNT_GATE_CNT_LOW && blanking == true) || (gateMode == IOPCNT_GATE_CNT_HIGH_ZERO_OFF && blanking == false))
		return false;

	return true;
}

static void psxRcntSync(int cntidx)
{
	if ((psxCounters[cntidx].currentIrqMode.repeatInterrupt) && !(psxCounters[cntidx].currentIrqMode.toggleInterrupt))
	{
		psxCounters[cntidx].mode.intrEnable = true;
	}

	if (psxRcntCanCount(cntidx) && psxCounters[cntidx].rate != PSXHBLANK)
	{
		const u32 change = (psxRegs.cycle - psxCounters[cntidx].startCycle) / psxCounters[cntidx].rate;
		if (change > 0)
		{
			psxCounters[cntidx].count += change;
			psxCounters[cntidx].startCycle += change * psxCounters[cntidx].rate;

			psxCounters[cntidx].startCycle &= ~((u64)psxCounters[cntidx].rate - 1);
		}
	}
	else
	{
		if (psxCounters[cntidx].mode.gateEnable && psxCounters[cntidx].mode.gateMode == IOPCNT_GATE_CNT_HIGH_ZERO_OFF)
			psxCounters[cntidx].count = 0;

		psxCounters[cntidx].startCycle = psxRegs.cycle;
	}
}

static void _rcntSet(int cntidx)
{
	u64 overflowCap = (cntidx >= 3) ? 0x100000000ULL : 0x10000;
	u64 c;

	const psxCounter& counter = psxCounters[cntidx];

	if (counter.rate == PSXHBLANK || !psxRcntCanCount(cntidx))
		return;

	if (counter.count > overflowCap || counter.count > counter.target)
	{
		psxNextDeltaCounter = 4;
		return;
	}

	c = (u64)((overflowCap - counter.count) * counter.rate) - ((u32)psxRegs.cycle - (u32)counter.startCycle);
	c += psxRegs.cycle - psxNextStartCounter;

	if (c < (u64)psxNextDeltaCounter)
	{
		psxNextDeltaCounter = (u32)c;
		psxSetNextBranch(psxNextStartCounter, psxNextDeltaCounter);
	}

	if (counter.target & IOPCNT_FUTURE_TARGET)
		return;

	c = (s64)((counter.target - counter.count) * counter.rate) - ((u32)psxRegs.cycle - (u32)counter.startCycle);
	c += psxRegs.cycle - psxNextStartCounter;

	if (c < (u64)psxNextDeltaCounter)
	{
		psxNextDeltaCounter = (u32)c;
		psxSetNextBranch(psxNextStartCounter, psxNextDeltaCounter);
	}
}


void psxRcntInit()
{
	int i;

	std::memset(psxCounters, 0, sizeof(psxCounters));

	for (i = 0; i < 3; i++)
	{
		psxCounters[i].rate = 1;
		psxCounters[i].mode.intrEnable = true;
		psxCounters[i].target = IOPCNT_FUTURE_TARGET;
		psxCounters[i].currentIrqMode.repeatInterrupt = false;
		psxCounters[i].currentIrqMode.toggleInterrupt = false;
	}
	for (i = 3; i < 6; i++)
	{
		psxCounters[i].rate = 1;
		psxCounters[i].mode.intrEnable = true;
		psxCounters[i].target = IOPCNT_FUTURE_TARGET;
		psxCounters[i].currentIrqMode.repeatInterrupt = false;
		psxCounters[i].currentIrqMode.toggleInterrupt = false;
	}

	psxCounters[0].interrupt = 0x10;
	psxCounters[1].interrupt = 0x20;
	psxCounters[2].interrupt = 0x40;

	psxCounters[3].interrupt = 0x04000;
	psxCounters[4].interrupt = 0x08000;
	psxCounters[5].interrupt = 0x10000;

	psxCounters[6].rate = 768;
	psxCounters[6].deltaCycles = psxCounters[6].rate;
	psxCounters[6].mode.modeval = 0x8;

	psxCounters[7].rate = PSXCLK / 1000;
	psxCounters[7].deltaCycles = psxCounters[7].rate;
	psxCounters[7].mode.modeval = 0x8;

	for (i = 0; i < 8; i++)
		psxCounters[i].startCycle = psxRegs.cycle;

	psxNextDeltaCounter = 1;
	psxNextStartCounter = psxRegs.cycle;
}

static void _rcntFireInterrupt(int i, bool isOverflow)
{
	bool updateIntr = psxCounters[i].currentIrqMode.repeatInterrupt;

	if (psxCounters[i].mode.intrEnable)
	{
		bool already_set = isOverflow ? psxCounters[i].mode.overflowFlag : psxCounters[i].mode.targetFlag;
		if (updateIntr || !already_set)
		{
			psxHu32(HW_ISTAT) |= psxCounters[i].interrupt;
			iopTestIntc();
		}

		updateIntr |= psxCounters[i].currentIrqMode.toggleInterrupt;
	}

	if (updateIntr)
	{
		if (psxCounters[i].currentIrqMode.toggleInterrupt)
		{
			psxCounters[i].mode.intrEnable ^= true;
		}
		else
		{
			psxCounters[i].mode.intrEnable = false;
		}
	}
}

static void _rcntTestTarget(int i)
{
	if (psxCounters[i].count < psxCounters[i].target)
		return;

	PSXCNT_LOG("IOP Counter[%d] target 0x%I64x >= 0x%I64x (mode: %x)",
			   i, psxCounters[i].count, psxCounters[i].target, psxCounters[i].mode.modeval);

	if (psxCounters[i].mode.targetIntr)
	{
		_rcntFireInterrupt(i, false);
	}

	psxCounters[i].mode.targetFlag = true;

	if (psxCounters[i].mode.zeroReturn)
	{
		psxCounters[i].count -= psxCounters[i].target;
	}
	else
		psxCounters[i].target |= IOPCNT_FUTURE_TARGET;
}


static __fi void _rcntTestOverflow(int i)
{
	u64 maxTarget = (i < 3) ? 0xffff : 0xfffffffful;
	if (psxCounters[i].count <= maxTarget)
		return;

	PSXCNT_LOG("IOP Counter[%d] overflow 0x%I64x >= 0x%I64x (mode: %x)",
			   i, psxCounters[i].count, maxTarget, psxCounters[i].mode.modeval);

	if (psxCounters[i].mode.overflIntr)
	{
		_rcntFireInterrupt(i, true);
	}

	psxCounters[i].mode.overflowFlag = true;

	psxCounters[i].count -= maxTarget + 1;
	psxCounters[i].target &= maxTarget;
}

static void _psxCheckStartGate(int i)
{
	if (!(psxCounters[i].mode.gateEnable))
		return;

	switch (psxCounters[i].mode.gateMode)
	{
		case 0x0:

			psxCounters[i].count = (i < 3) ?
									   psxRcntRcount16(i) :
									   psxRcntRcount32(i);

			psxCounters[i].startCycle = psxRegs.cycle & ~((u64)psxCounters[i].rate - 1);
			break;

		case 0x1:
			break;

		case 0x2:
			psxRcntSync(i);
			psxCounters[i].mode.stopped = false;
			psxCounters[i].count = 0;
			psxCounters[i].target &= ~IOPCNT_FUTURE_TARGET;
			break;

		case 0x3:
			break;
	}
}

static void _psxCheckEndGate(int i)
{
	if (!(psxCounters[i].mode.gateEnable))
		return;

	switch (psxCounters[i].mode.gateMode)
	{
		case 0x0:
			psxCounters[i].startCycle = psxRegs.cycle & ~((u64)psxCounters[i].rate - 1);
			break;

		case 0x1:
			psxRcntSync(i);
			psxCounters[i].count = 0;
			psxCounters[i].target &= ~IOPCNT_FUTURE_TARGET;
			break;

		case 0x2:
			psxRcntSync(i);
			psxCounters[i].count = 0;
			psxCounters[i].target &= ~IOPCNT_FUTURE_TARGET;
			break;

		case 0x3:
			if (psxCounters[i].mode.stopped)
			{
				psxCounters[i].startCycle = psxRegs.cycle & ~((u64)psxCounters[i].rate - 1);
				psxCounters[i].mode.stopped = false;
			}
			break;
	}
}

void psxHBlankStart()
{
	if ((psxCounters[1].rate == PSXHBLANK) && psxRcntCanCount(1))
	{
		psxCounters[1].count++;
		_rcntTestOverflow(1);
		_rcntTestTarget(1);
	}

	if ((psxCounters[3].rate == PSXHBLANK) && psxRcntCanCount(3))
	{
		psxCounters[3].count++;
		_rcntTestOverflow(3);
		_rcntTestTarget(3);
	}

	_psxCheckStartGate(0);

	hBlanking = true;

	_rcntSet(0);
}

void psxHBlankEnd()
{
	_psxCheckEndGate(0);

	hBlanking = false;

	_rcntSet(0);
}

void psxVBlankStart()
{
	cdvdVsync();
	iopIntcIrq(0);
	
	_psxCheckStartGate(1);
	_psxCheckStartGate(3);

	vBlanking = true;

	_rcntSet(1);
	_rcntSet(3);
}

void psxVBlankEnd()
{
	iopIntcIrq(11);
	
	_psxCheckEndGate(1);
	_psxCheckEndGate(3);

	vBlanking = false;

	_rcntSet(1);
	_rcntSet(3);
}

void psxRcntUpdate()
{
	int i;

	psxNextDeltaCounter = 0x7fffffff;
	psxNextStartCounter = psxRegs.cycle;

	for (i = 0; i < 6; i++)
	{

		psxRcntSync(i);

		if (psxCounters[i].rate == PSXHBLANK)
			continue;

		if (!psxRcntCanCount(i))
			continue;

		_rcntTestOverflow(i);
		_rcntTestTarget(i);
	}

	const u32 spu2_delta = (psxRegs.cycle - lClocks) % 768;
	psxCounters[6].startCycle = psxRegs.cycle - spu2_delta;
	psxCounters[6].deltaCycles = psxCounters[6].rate;
	SPU2async();
	psxNextDeltaCounter = psxCounters[6].deltaCycles;

	DEV9async(1);    
	const s32 diffusb = psxRegs.cycle - psxCounters[7].startCycle;
	s32 cusb = psxCounters[7].deltaCycles;

	if (diffusb >= psxCounters[7].deltaCycles)
	{
		USBasync(diffusb);
		psxCounters[7].startCycle += psxCounters[7].rate * (diffusb / psxCounters[7].rate);
		psxCounters[7].deltaCycles = psxCounters[7].rate;
	}
	else
		cusb -= diffusb;

	if (cusb < psxNextDeltaCounter)
		psxNextDeltaCounter = cusb;

	for (i = 0; i < 6; i++)
		_rcntSet(i);
}

void psxRcntWcount16(int index, u16 value)
{
	pxAssert(index < 3);
	PSXCNT_LOG("16bit IOP Counter[%d] writeCount16 = %x", index, value);

	psxRcntSync(index);

	psxCounters[index].count = value & 0xffff;

	psxCounters[index].target &= 0xffff;

	if (psxCounters[index].count > psxCounters[index].target)
	{
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;
	}

	_rcntSet(index);
}

void psxRcntWcount32(int index, u32 value)
{
	pxAssert(index >= 3 && index < 6);
	PSXCNT_LOG("32bit IOP Counter[%d] writeCount32 = %x", index, value);

	psxRcntSync(index);

	psxCounters[index].count = value;

	psxCounters[index].target &= 0xffffffff;

	if (psxCounters[index].count > psxCounters[index].target)
	{
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;
	}

	_rcntSet(index);
}

__fi void psxRcntWmode16(int index, u32 value)
{
	int irqmode = 0;
	PSXCNT_LOG("16bit IOP Counter[%d] writeMode = 0x%04X", index, value);

	pxAssume(index >= 0 && index < 3);
	psxCounter& counter = psxCounters[index];
	psxCounterMode oldMode = counter.mode;

	counter.mode.modeval = (value & IOPCNT_MODE_WRITE_MSK) | (counter.mode.modeval & IOPCNT_MODE_FLAG_MSK);

	if (!((oldMode.targetFlag || oldMode.overflowFlag) && (oldMode.targetIntr || oldMode.overflIntr)))
		psxRcntSetNewIntrMode(index);

	if (counter.mode.repeatIntr != counter.currentIrqMode.repeatInterrupt || counter.mode.toggleIntr != counter.currentIrqMode.toggleInterrupt)
		DevCon.Warning("Write to psxCounter[%d] mode old repeat %d new %d old toggle %d new %d", index, counter.currentIrqMode.repeatInterrupt, counter.mode.repeatIntr, counter.currentIrqMode.toggleInterrupt, counter.mode.toggleIntr);
	
	if (value & (1 << 4))
	{
		irqmode += 1;
	}
	if (value & (1 << 5))
	{
		irqmode += 2;
	}
	if (value & (1 << 7))
	{
		PSXCNT_LOG("16 Counter %d Toggle IRQ on %s", index, (irqmode & 3) == 1 ? "Target" : ((irqmode & 3) == 2 ? "Overflow" : "Target and Overflow"));
	}
	else
	{
		PSXCNT_LOG("16 Counter %d Pulsed IRQ on %s", index, (irqmode & 3) == 1 ? "Target" : ((irqmode & 3) == 2 ? "Overflow" : "Target and Overflow"));
	}
	if (!(value & (1 << 6)))
	{
		PSXCNT_LOG("16 Counter %d One Shot", index);
	}
	else
	{
		PSXCNT_LOG("16 Counter %d Repeat", index);
	}
	if (index == 2)
	{
		if (counter.mode.t2Prescale)
			psxCounters[2].rate = 8;
		else
			psxCounters[2].rate = 1;
	}
	else
	{
		counter.rate = 1;

		if (counter.mode.extSignal)
			counter.rate = (index == 0) ? PSXPIXEL : PSXHBLANK;

		if (counter.rate == PSXPIXEL)
			Console.Warning("PSX Pixel clock set to time 0, sync may be incorrect");

		if (counter.mode.gateEnable)
		{
			if (counter.mode.gateMode >= IOPCNT_GATE_CNT_HIGH_ZERO_OFF)
				counter.mode.stopped = true;

			PSXCNT_LOG("IOP Counter[%d] Gate Check set, value = 0x%04X", index, value);
		}
	}

	counter.count = 0;
	counter.startCycle = psxRegs.cycle & ~((u64)counter.rate - 1);
	counter.target &= 0xffff;

	_rcntSet(index);
}

__fi void psxRcntWmode32(int index, u32 value)
{
	PSXCNT_LOG("32bit IOP Counter[%d] writeMode = 0x%04x", index, value);
	int irqmode = 0;
	pxAssume(index >= 3 && index < 6);
	psxCounter& counter = psxCounters[index];
	psxCounterMode oldMode = counter.mode;

	counter.mode.modeval = (value & IOPCNT_MODE_WRITE_MSK) | (counter.mode.modeval & IOPCNT_MODE_FLAG_MSK);

	if (!((oldMode.targetFlag || oldMode.overflowFlag) && (oldMode.targetIntr || oldMode.overflIntr)))
		psxRcntSetNewIntrMode(index);

	if (counter.mode.repeatIntr != counter.currentIrqMode.repeatInterrupt || counter.mode.toggleIntr != counter.currentIrqMode.toggleInterrupt)
		DevCon.Warning("Write to psxCounter[%d] mode old repeat %d new %d old toggle %d new %d", index, counter.currentIrqMode.repeatInterrupt, counter.mode.repeatIntr, counter.currentIrqMode.toggleInterrupt, counter.mode.toggleIntr);
	
	if (value & (1 << 4))
	{
		irqmode += 1;
	}
	if (value & (1 << 5))
	{
		irqmode += 2;
	}
	if (value & (1 << 7))
	{
		PSXCNT_LOG("32 Counter %d Toggle IRQ on %s", index, (irqmode & 3) == 1 ? "Target" : ((irqmode & 3) == 2 ? "Overflow" : "Target and Overflow"));
	}
	else
	{
		PSXCNT_LOG("32 Counter %d Pulsed IRQ on %s", index, (irqmode & 3) == 1 ? "Target" : ((irqmode & 3) == 2 ? "Overflow" : "Target and Overflow"));
	}
	if (!(value & (1 << 6)))
	{
		PSXCNT_LOG("32 Counter %d One Shot", index);
	}
	else
	{
		PSXCNT_LOG("32 Counter %d Repeat", index);
	}
	if (index == 3)
	{
		counter.rate = 1;

		if (counter.mode.extSignal)
			counter.rate = PSXHBLANK;

		if (counter.mode.gateEnable)
		{
			PSXCNT_LOG("IOP Counter[3] Gate Check set, value = %x", value);
			if (counter.mode.gateMode >= IOPCNT_GATE_CNT_HIGH_ZERO_OFF)
				counter.mode.stopped = true;
		}
	}
	else
	{
		switch (counter.mode.t4_5Prescale)
		{
			case 0x1:
				counter.rate = 8;
				break;
			case 0x2:
				counter.rate = 16;
				break;
			case 0x3:
				counter.rate = 256;
				break;
			default:
				counter.rate = 1;
				break;
		}
	}

	counter.count = 0;
	counter.startCycle = psxRegs.cycle & ~(static_cast<u64>(counter.rate - 1));
	counter.target &= 0xffffffff;
	_rcntSet(index);
}

void psxRcntWtarget16(int index, u32 value)
{
	pxAssert(index < 3);
	PSXCNT_LOG("IOP Counter[%d] writeTarget16 = %lx", index, value);
	psxCounters[index].target = value & 0xffff;

	psxRcntSync(index);

	if (psxCounters[index].target <= psxCounters[index].count)
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;

	_rcntSet(index);
}

void psxRcntWtarget32(int index, u32 value)
{
	pxAssert(index >= 3 && index < 6);
	PSXCNT_LOG("IOP Counter[%d] writeTarget32 = %lx mode %x", index, value, psxCounters[index].mode.modeval);

	psxCounters[index].target = value;

	psxRcntSync(index);

	if (psxCounters[index].target <= psxCounters[index].count)
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;

	_rcntSet(index);
}

u16 psxRcntRcount16(int index)
{
	psxRcntSync(index);
	const u32 retval = (u32)psxCounters[index].count;

	pxAssert(index < 3);

	PSXCNT_LOG("IOP Counter[%d] readCount16 = %lx", index, (u16)retval);

	return (u16)retval;
}

u32 psxRcntRcount32(int index)
{
	psxRcntSync(index);
	
	const u32 retval = (u32)psxCounters[index].count;

	pxAssert(index >= 3 && index < 6);

	PSXCNT_LOG("IOP Counter[%d] readCount32 = %lx", index, retval);

	return retval;
}

void psxRcntSetNewIntrMode(int index)
{
	psxCounters[index].mode.targetFlag = false;
	psxCounters[index].mode.overflowFlag = false;
	psxCounters[index].mode.intrEnable = true;

	
	psxCounters[index].currentIrqMode.repeatInterrupt = psxCounters[index].mode.repeatIntr;
	psxCounters[index].currentIrqMode.toggleInterrupt = psxCounters[index].mode.toggleIntr;
}

bool SaveStateBase::psxRcntFreeze()
{
	if (!FreezeTag("iopCounters"))
		return false;

	Freeze(psxCounters);
	Freeze(psxNextDeltaCounter);
	Freeze(psxNextStartCounter);
	Freeze(hBlanking);
	Freeze(vBlanking);

	if (!IsOkay())
		return false;

	if (IsLoading())
		psxRcntUpdate();

	return true;
}
