// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Dma.h"
#include "IopDma.h"
#include "IopHw.h"
#include "SPU2/Debug.h"
#include "SPU2/defs.h"
#include "SPU2/spu2.h"

StereoOut32 V_Core::ReadInput_HiFi()
{
	if (SPU2::IsRunningPSXMode() && SPU2::MsgToConsole())
		SPU2::ConLog("ReadInput_HiFi!!!!!\n");

	const u16 ReadIndex = (OutPos * 2) & 0x1FF;

	StereoOut32 retval(
		(s32&)(*GetMemPtr(0x2000 + (Index << 10) + ReadIndex)),
		(s32&)(*GetMemPtr(0x2200 + (Index << 10) + ReadIndex)));

	if (Index == 1)
	{
		retval.Left >>= 16;
		retval.Right >>= 16;
	}

	if (InputDataTransferred)
	{
		u32 amount = std::min(InputDataTransferred, (u32)0x180);

		InputDataTransferred -= amount;
		MADR += amount;
		if (!InputDataTransferred && !InputDataLeft)
		{
			if (Index == 0)
				spu2DMA4Irq();
			else
				spu2DMA7Irq();
		}
	}

	if (ReadIndex == 0x100 || ReadIndex == 0x0 || ReadIndex == 0x80 || ReadIndex == 0x180)
	{
		if (ReadIndex == 0x100)
			InputPosWrite = 0;
		else if (ReadIndex == 0)
			InputPosWrite = 0x100;

		if (InputDataLeft >= 0x100)
		{
			AutoDMAReadBuffer(0);
			AdmaInProgress = 1;
			if (InputDataLeft < 0x100)
			{
				if (IsDevBuild)
				{
					SPU2::FileLog("[%10d] AutoDMA%c block end.\n", Cycles, GetDmaIndexChar());
					if (InputDataLeft > 0)
					{
						if (SPU2::MsgAutoDMA())
							SPU2::ConLog("WARNING: adma buffer didn't finish with a whole block!!\n");
					}
				}

				InputDataLeft = 0;
			}
		}
		else if ((AutoDMACtrl & (Index + 1)))
			AutoDMACtrl |= ~3;
	}
	return retval;
}

StereoOut32 V_Core::ReadInput()
{
	StereoOut32 retval;
	u16 ReadIndex = OutPos;

	for (int i = 0; i < 2; i++)
		if (Cores[i].IRQEnable && (0x2000 + (Index << 10) + ReadIndex) == (Cores[i].IRQA & 0xfffffdff))
			SetIrqCall(i);

	if ((Index == 1) || !(Index == 0 && (PlayMode & 2) != 0))
	{
		retval = StereoOut32(
			(s32)(*GetMemPtr(0x2000 + (Index << 10) + ReadIndex)),
			(s32)(*GetMemPtr(0x2200 + (Index << 10) + ReadIndex)));
	}

#ifdef PCSX2_DEVBUILD
	DebugCores[Index].admaWaveformL[OutPos % 0x100] = retval.Left;
	DebugCores[Index].admaWaveformR[OutPos % 0x100] = retval.Right;
#endif

	if (InputDataTransferred)
	{
		u32 amount = std::min(InputDataTransferred, (u32)0x180);

		InputDataTransferred -= amount;
		MADR += amount;
		if (!InputDataTransferred && !InputDataLeft)
		{
			if (Index == 0)
				spu2DMA4Irq();
			else
				spu2DMA7Irq();
		}
	}

	if (PlayMode == 2 && Index == 0)
		ReadIndex = (ReadIndex * 2) & 0x1FF;

	if (ReadIndex == 0x100 || ReadIndex == 0x0 || ReadIndex == 0x80 || ReadIndex == 0x180)
	{
		if (ReadIndex == 0x100)
			InputPosWrite = 0;
		else if (ReadIndex == 0)
			InputPosWrite = 0x100;

		if (InputDataLeft >= 0x100)
		{
			AutoDMAReadBuffer(0);
			AdmaInProgress = 1;
			if (InputDataLeft < 0x100)
			{
				if (IsDevBuild)
				{
					SPU2::FileLog("[%10d] AutoDMA%c block end.\n", Cycles, GetDmaIndexChar());
					if (InputDataLeft > 0)
					{
						if (SPU2::MsgAutoDMA())
							SPU2::ConLog("WARNING: adma buffer didn't finish with a whole block!!\n");
					}
				}

				InputDataLeft = 0;
			}
		}
		else if ((AutoDMACtrl & (Index + 1)))
			AutoDMACtrl |= ~3;
	}
	return retval;
}
