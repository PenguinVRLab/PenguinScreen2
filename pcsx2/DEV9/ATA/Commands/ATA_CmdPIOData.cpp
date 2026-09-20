// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/ATA/ATA.h"
#include "DEV9/DEV9.h"

void ATA::DRQCmdPIODataToHost(u8* buff, int buffLen, int buffIndex, int size, bool sendIRQ)
{
	pioPtr = 0;
	pioEnd = size >> 1;

	memcpy(pioBuffer, &buff[buffIndex], size < (buffLen - buffIndex) ? size : (buffLen - buffIndex));

	regStatus &= ~ATA_STAT_BUSY;
	regStatus |= ATA_STAT_DRQ;

	if (regControlEnableIRQ && sendIRQ)
	{
		pendingInterrupt = true;
		_DEV9irq(ATA_INTR_INTRQ, 1);
	}
}
void ATA::PostCmdPIODataToHost()
{
	pioPtr = 0;
	pioEnd = 0;
	if (pioDRQEndTransferFunc != nullptr)
	{
		regStatus |= ATA_STAT_BUSY;
		regStatus &= ~ATA_STAT_DRQ;
		(this->*pioDRQEndTransferFunc)();
	}
	else
		regStatus &= ~ATA_STAT_DRQ;
}

u16 ATA::ATAreadPIO()
{
	if (pioPtr < pioEnd)
	{
		const u16 ret = *(u16*)&pioBuffer[pioPtr * 2];
		pioPtr++;
		if (pioPtr >= pioEnd)
			PostCmdPIODataToHost();

		return ret;
	}
	return 0xFF;
}

void ATA::HDD_IdentifyDevice()
{
	if (!PreCmd())
		return;
	DevCon.WriteLn("DEV9: HddidentifyDevice");

	CreateHDDinfo(hddImageSize / 512);

	pioDRQEndTransferFunc = nullptr;
	DRQCmdPIODataToHost(identifyData, 256 * 2, 0, 256 * 2, true);
}

void ATA::HDD_ReadMultiple(bool isLBA48)
{
	sectorsPerInterrupt = curMultipleSectorsSetting;
	HDD_ReadPIO(isLBA48);
}

void ATA::HDD_ReadSectors(bool isLBA48)
{
	sectorsPerInterrupt = 1;
	HDD_ReadPIO(isLBA48);
}

void ATA::HDD_ReadPIO(bool isLBA48)
{
	if (!PreCmd())
		return;

	if (sectorsPerInterrupt == 0)
	{
		CmdNoDataAbort();
		return;
	}

	IDE_CmdLBA48Transform(isLBA48);

	regStatus &= ~ATA_STAT_SEEK;
	if (!HDD_CanSeek())
	{
		regStatus |= ATA_STAT_ERR;
		regStatusSeekLock = -1;
		regError |= ATA_ERR_ID;
		PostCmdNoData();
		return;
	}
	else
		regStatus |= ATA_STAT_SEEK;

	HDD_ReadSync(&ATA::HDD_ReadPIOS2);
}

void ATA::HDD_ReadPIOS2()
{
	pioDRQEndTransferFunc = &ATA::HDD_ReadPIOEndBlock;
	DRQCmdPIODataToHost(readBuffer, readBufferLen, 0, 256 * 2, true);
}

void ATA::HDD_ReadPIOEndBlock()
{
	rdTransferred += 512;
	if (rdTransferred >= nsector * 512)
	{
		HDD_SetErrorAtTransferEnd();
		regStatus &= ~ATA_STAT_BUSY;
		pioDRQEndTransferFunc = nullptr;
		rdTransferred = 0;
	}
	else
	{
		if ((rdTransferred / 512) % sectorsPerInterrupt == 0)
			DRQCmdPIODataToHost(readBuffer, readBufferLen, rdTransferred, 256 * 2, true);
		else
			DRQCmdPIODataToHost(readBuffer, readBufferLen, rdTransferred, 256 * 2, false);
	}
}

