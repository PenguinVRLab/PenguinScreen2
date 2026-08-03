// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SPU2/defs.h"
#include "SPU2/Debug.h"
#include "SPU2/Dma.h"
#include "SPU2/spu2.h"
#include "R3000A.h"
#include "IopHw.h"
#include "Config.h"

static constexpr int CYCLES_PER_WORD = 24;

#ifdef PCSX2_DEVBUILD

#define safe_fclose(ptr) \
	((void)((((ptr) != nullptr) && (std::fclose(ptr), !!0)), (ptr) = nullptr))

static FILE* DMA4LogFile = nullptr;
static FILE* DMA7LogFile = nullptr;
static FILE* ADMA4LogFile = nullptr;
static FILE* ADMA7LogFile = nullptr;
static FILE* ADMAOutLogFile = nullptr;

static FILE* REGWRTLogFile[2] = {0, 0};

void DMALogOpen()
{
	if (!SPU2::DMALog())
		return;
	DMA4LogFile = EmuFolders::OpenLogFile("SPU2dma4.dat", "wb");
	DMA7LogFile = EmuFolders::OpenLogFile("SPU2dma7.dat", "wb");
	ADMA4LogFile = EmuFolders::OpenLogFile("adma4.raw", "wb");
	ADMA7LogFile = EmuFolders::OpenLogFile("adma7.raw", "wb");
	ADMAOutLogFile = EmuFolders::OpenLogFile("admaOut.raw", "wb");
}

void DMA4LogWrite(void* lpData, u32 ulSize)
{
	if (!SPU2::DMALog())
		return;
	if (!DMA4LogFile)
		return;
	fwrite(lpData, ulSize, 1, DMA4LogFile);
}

void DMA7LogWrite(void* lpData, u32 ulSize)
{
	if (!SPU2::DMALog())
		return;
	if (!DMA7LogFile)
		return;
	fwrite(lpData, ulSize, 1, DMA7LogFile);
}

void ADMAOutLogWrite(void* lpData, u32 ulSize)
{
	if (!SPU2::DMALog())
		return;
	if (!ADMAOutLogFile)
		return;
	fwrite(lpData, ulSize, 1, ADMAOutLogFile);
}

void RegWriteLog(u32 core, u16 value)
{
	if (!SPU2::DMALog())
		return;
	if (!REGWRTLogFile[core])
		return;
	fwrite(&value, 2, 1, REGWRTLogFile[core]);
}

void DMALogClose()
{
	safe_fclose(DMA4LogFile);
	safe_fclose(DMA7LogFile);
	safe_fclose(REGWRTLogFile[0]);
	safe_fclose(REGWRTLogFile[1]);
	safe_fclose(ADMA4LogFile);
	safe_fclose(ADMA7LogFile);
	safe_fclose(ADMAOutLogFile);
}

#endif

void V_Core::LogAutoDMA(FILE* fp)
{
	if (!SPU2::DMALog() || !fp || !DMAPtr)
		return;
	fwrite(DMAPtr + InputDataProgress, 0x400, 1, fp);
}

void V_Core::AutoDMAReadBuffer(int mode)
{
	u32 spos = InputPosWrite & 0x100;
	bool leftbuffer = !(InputPosWrite & 0x80);

	if (InputPosWrite == 0xFFFF)
		return;

	AutoDMACtrl &= 0x3;

	int size = std::min(InputDataLeft, (u32)0x200);
	if (!leftbuffer)
		size = std::min(size, 0x100);
#ifdef PCSX2_DEVBUILD
	LogAutoDMA(Index ? ADMA7LogFile : ADMA4LogFile);
#endif
	if (DMAPtr == nullptr)
	{
		DMAPtr = (u16*)iopPhysMem(MADR);
		InputDataProgress = 0;
	}

	if (mode)
	{
		if (DMAPtr != nullptr)
			memcpy(GetMemPtr(0x2000 + (Index << 10) + spos), DMAPtr + InputDataProgress, size);
		MADR += size;
		InputDataLeft -= 0x200;
		InputDataProgress += 0x200;
	}
	else
	{
		while (size)
		{
			if (!leftbuffer)
				spos |= 0x200;
			else
				spos &= ~0x200;

			if (DMAPtr != nullptr)
				memcpy(GetMemPtr(0x2000 + (Index << 10) + spos), DMAPtr + InputDataProgress, 0x200);
			InputDataTransferred += 0x200;
			InputDataLeft -= 0x100;
			InputDataProgress += 0x100;
			leftbuffer = !leftbuffer;
			size -= 0x100;
			InputPosWrite += 0x80;
		}
	}
	if (!(InputPosWrite & 0x80))
		InputPosWrite = 0xFFFF;
}

void V_Core::StartADMAWrite(u16* pMem, u32 sz)
{
	int size = sz;

	TimeUpdate(psxRegs.cycle);

	if (SPU2::MsgAutoDMA())
	{
		SPU2::ConLog("* SPU2: DMA%c AutoDMA Transfer of %d bytes to %x (%02x %x %04x).OutPos %x\n",
			   GetDmaIndexChar(), size << 1, ActiveTSA, DMABits, AutoDMACtrl, (~Regs.ATTR) & 0xffff, OutPos);
	}

	InputDataProgress = 0;
	TADR = MADR + (size << 1);
	if ((AutoDMACtrl & (Index + 1)) == 0)
	{
		ActiveTSA = 0x2000 + (Index << 10);
		DMAICounter = size * CYCLES_PER_WORD;
		LastClock = psxRegs.cycle;
	}
	else if (size >= 256)
	{
		InputDataLeft = size;
		if (InputPosWrite != 0xFFFF)
		{
#ifdef PCM24_S1_INTERLEAVE
			if ((Index == 1) && ((PlayMode & 8) == 8))
			{
				AutoDMAReadBuffer(Index, 1);
			}
			else
			{
				AutoDMAReadBuffer(Index, 0);
			}
#else
			AutoDMAReadBuffer(0);
#endif
		}
		AdmaInProgress = 1;
	}
	else
	{
		if (SPU2::MsgToConsole())
			SPU2::ConLog("ADMA%c Error Size of %x too small\n", GetDmaIndexChar(), size);
		InputDataLeft = 0;
		DMAICounter = size * CYCLES_PER_WORD;
		LastClock = psxRegs.cycle;
	}
}

void V_Core::PlainDMAWrite(u16* pMem, u32 size)
{
	if (SPU2::MsgToConsole())
	{

		if (ActiveTSA & 7)
		{
			SPU2::ConLog("* SPU2 DMA Write > Misaligned target. Core: %d  IOP: %p  TSA: 0x%x  Size: 0x%x\n", Index, (void*)DMAPtr, ActiveTSA, ReadSize);
		}
	}

	TimeUpdate(psxRegs.cycle);

	ReadSize = size;
	IsDMARead = false;
	DMAICounter = 0;
	LastClock = psxRegs.cycle;
	Regs.STATX &= ~0x80;
	Regs.STATX |= 0x400;
	TADR = MADR + (size << 1);

	if (SPU2::MsgDMA())
	{
		SPU2::ConLog("* SPU2: DMA%c Write Transfer of %d bytes to %x (%02x %x %04x). IRQE = %d IRQA = %x \n",
			GetDmaIndexChar(), size << 1, ActiveTSA, DMABits, AutoDMACtrl, Regs.ATTR & 0xffff,
			Cores[Index].IRQEnable, Cores[Index].IRQA);
	}

	FinishDMAwrite();
}

void V_Core::FinishDMAwrite()
{
	if (DMAPtr == nullptr)
	{
		DMAPtr = (u16*)iopPhysMem(MADR);
	}

	DMAICounter = ReadSize;

#ifdef PCSX2_DEVBUILD
	if (Index == 0)
		DMA4LogWrite(DMAPtr, ReadSize << 1);
	else
		DMA7LogWrite(DMAPtr, ReadSize << 1);
#endif

	u32 buff1end = ActiveTSA + std::min(ReadSize, (u32)0x100 + std::abs(DMAICounter / CYCLES_PER_WORD));
	u32 buff2end = 0;
	if (buff1end > 0x100000)
	{
		buff2end = buff1end - 0x100000;
		buff1end = 0x100000;
	}

	const int cacheIdxStart = ActiveTSA / pcm_WordsPerBlock;
	const int cacheIdxEnd = (buff1end + pcm_WordsPerBlock - 1) / pcm_WordsPerBlock;
	PcmCacheEntry* cacheLine = &pcm_cache_data[cacheIdxStart];
	PcmCacheEntry& cacheEnd = pcm_cache_data[cacheIdxEnd];

	do
	{
		cacheLine->Validated = false;
		cacheLine++;
	} while (cacheLine != &cacheEnd);

	const u32 buff1size = (buff1end - ActiveTSA);
	memcpy(GetMemPtr(ActiveTSA), DMAPtr, buff1size * 2);

	u32 TDA;

	if (buff2end > 0)
	{

		const u32 start = ActiveTSA;
		TDA = buff1end;

		DMAPtr += TDA - ActiveTSA;
		ReadSize -= TDA - ActiveTSA;
		ActiveTSA = 0;
		memcpy(GetMemPtr(0), DMAPtr, buff2end * 2);
		TDA = (buff2end) & 0xfffff;

		for (int i = 0; i < 2; i++)
		{

			if (Cores[i].IRQEnable && (Cores[i].IRQA > start || Cores[i].IRQA < TDA))
			{
				SetIrqCallDMA(i);
			}
		}
	}
	else
	{

		TDA = buff1end;

		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > ActiveTSA && Cores[i].IRQA < TDA))
			{
				SetIrqCallDMA(i);
			}
		}
	}

	DMAPtr += TDA - ActiveTSA;
	ReadSize -= TDA - ActiveTSA;

	DMAICounter = (DMAICounter - ReadSize) * CYCLES_PER_WORD;

	CounterUpdate(DMAICounter);

	ActiveTSA = TDA;
	ActiveTSA &= 0xfffff;
	TSA = ActiveTSA;
}

void V_Core::FinishDMAread()
{
	u32 buff1end = ActiveTSA + std::min(ReadSize, (u32)0x100 + std::abs(DMAICounter / CYCLES_PER_WORD));
	u32 buff2end = 0;

	if (buff1end > 0x100000)
	{
		buff2end = buff1end - 0x100000;
		buff1end = 0x100000;
	}

	if (DMAPtr == nullptr)
	{
		DMAPtr = (u16*)iopPhysMem(MADR);
	}

	const u32 buff1size = (buff1end - ActiveTSA);
	memcpy(DMARPtr, GetMemPtr(ActiveTSA), buff1size * 2);
	u32 TDA;

	if (buff2end > 0)
	{
		const u32 start = ActiveTSA;
		TDA = buff1end;

		DMARPtr += TDA - ActiveTSA;
		ReadSize -= TDA - ActiveTSA;
		ActiveTSA = 0;

		memcpy(DMARPtr, GetMemPtr(0), buff2end * 2);

		TDA = (buff2end) & 0xfffff;

		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > start || Cores[i].IRQA < TDA))
			{
				SetIrqCallDMA(i);
			}
		}
	}
	else
	{

		TDA = buff1end;

		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA > ActiveTSA && Cores[i].IRQA < TDA))
			{
				SetIrqCallDMA(i);
			}
		}
	}

	DMARPtr += TDA - ActiveTSA;
	ReadSize -= TDA - ActiveTSA;

	if (ReadSize)
		DMAICounter = std::min(ReadSize, (u32)0x100) * CYCLES_PER_WORD;
	else
		DMAICounter = CYCLES_PER_WORD;

	CounterUpdate(DMAICounter);

	ActiveTSA = TDA;
	ActiveTSA &= 0xfffff;
	TSA = ActiveTSA;
}

void V_Core::DoDMAread(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);

	DMARPtr = pMem;
	ActiveTSA = TSA & 0xfffff;
	ReadSize = size;
	IsDMARead = true;
	LastClock = psxRegs.cycle;
	DMAICounter = (std::min(ReadSize, (u32)0x100) * CYCLES_PER_WORD);
	Regs.STATX &= ~0x80;
	Regs.STATX |= 0x400;
	TADR = MADR + (size << 1);

	CounterUpdate(DMAICounter);

	if (SPU2::MsgDMA())
	{
		SPU2::ConLog("* SPU2: DMA%c Read Transfer of %d bytes from %x (%02x %x %04x). IRQE = %d IRQA = %x \n",
			GetDmaIndexChar(), size << 1, ActiveTSA, DMABits, AutoDMACtrl, Regs.ATTR & 0xffff,
			Cores[Index].IRQEnable, Cores[Index].IRQA);
	}
}

void V_Core::DoDMAwrite(u16* pMem, u32 size)
{
	DMAPtr = pMem;

	if (size < 2)
	{
		Regs.STATX &= ~0x80;
		DMAICounter = 1 * CYCLES_PER_WORD;
		LastClock = psxRegs.cycle;
		return;
	}

	if (IsDevBuild)
	{
		DebugCores[Index].lastsize = size;
		DebugCores[Index].dmaFlag = 2;
	}

	if (SPU2::MsgToConsole())
	{
		if (TSA > 0xfffff)
		{
			SPU2::ConLog("* SPU2: Transfer Start Address out of bounds. TSA is %x\n", TSA);
		}
	}

	ActiveTSA = TSA & 0xfffff;

	const bool adma_enable = ((AutoDMACtrl & (Index + 1)) == (Index + 1));

	if (adma_enable)
	{
		StartADMAWrite(pMem, size);
	}
	else
	{
		PlainDMAWrite(pMem, size);
		Regs.STATX &= ~0x80;
		Regs.STATX |= 0x400;
	}
}
