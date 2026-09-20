// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/ATA/ATA.h"
#include "DEV9/DEV9.h"

void ATA::IDE_ExecCmd(u16 value)
{
	switch (value)
	{
		case 0x00:
			HDD_Nop();
			break;
		case 0x10:
			HDD_Recalibrate();
			break;
		case 0x20:
			HDD_ReadSectors(false);
			break;
		case 0x24:
			if (lba48Supported)
				HDD_ReadSectors(true);
			else
				HDD_Unk();
			break;
		case 0x29:
			if (lba48Supported)
				HDD_ReadMultiple(true);
			else
				HDD_Unk();
			break;
		case 0x40:
			HDD_ReadVerifySectors(false);
			break;
		case 0x42:
			if (lba48Supported)
				HDD_ReadVerifySectors(true);
			else
				HDD_Unk();
			break;
		case 0x70:
			HDD_SeekCmd();
			break;
		case 0x90:
			HDD_ExecuteDeviceDiag(true);
			break;
		case 0x91:
			HDD_InitDevParameters();
			break;
		case 0xB0:
			HDD_Smart();
			break;
		case 0xC4:
			HDD_ReadMultiple(false);
			break;
		case 0xC6:
			HDD_SetMultipleMode();
			break;
		case 0xC8:
			HDD_ReadDMA(false);
			break;
		case 0xCA:
			HDD_WriteDMA(false);
			break;
		case 0x25:
			if (lba48Supported)
				HDD_ReadDMA(true);
			else
				HDD_Unk();
			break;
		case 0x35:
			if (lba48Supported)
				HDD_WriteDMA(true);
			else
				HDD_Unk();
			break;
		case 0xE1:
			HDD_IdleImmediate();
			break;
		case 0xE3:
			HDD_Idle();
			break;
		case 0xE7:
			HDD_FlushCache();
			break;
		case 0xEA:
			if (lba48Supported)
				HDD_FlushCache();
			else
				HDD_Unk();
			break;
		case 0xEC:
			HDD_IdentifyDevice();
			break;
		case 0xEF:
			HDD_SetFeatures();
			break;

		case 0x8E:
			HDD_SCE();
			break;

		default:
			HDD_Unk();
			break;
	}
}

void ATA::HDD_Unk()
{
	Console.Error("DEV9: ATA: Unknown cmd %x", regCommand);

	PreCmd();

	regError |= ATA_ERR_ABORT;
	regStatus |= ATA_STAT_ERR;
	PostCmdNoData();
}

bool ATA::PreCmd()
{
	if ((regStatus & ATA_STAT_READY) == 0)
	{
		return false;
	}
	regStatus |= ATA_STAT_BUSY;

	regStatus &= ~ATA_STAT_WRERR;
	regStatus &= ~ATA_STAT_DRQ;
	regStatus &= ~ATA_STAT_ERR;

	regError = 0;

	return true;
}

void ATA::IDE_CmdLBA48Transform(bool islba48)
{
	lba48 = islba48;
	if (!lba48)
	{
		if (regNsector == 0)
			nsector = 256;
		else
			nsector = regNsector;
	}
	else
	{
		if (regNsector == 0 && regNsectorHOB == 0)
			nsector = 65536;
		else
		{
			const int lo = regNsector;
			const int hi = regNsectorHOB;

			nsector = (hi << 8) | lo;
		}
	}
}

