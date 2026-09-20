// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/ATA/ATA.h"
#include "DEV9/DEV9.h"

void ATA::HDD_Smart()
{
	DevCon.WriteLn("DEV9: HDD_Smart");

	if ((regStatus & ATA_STAT_READY) == 0)
		return;

	if (regHcyl != 0xC2 || regLcyl != 0x4F)
	{
		CmdNoDataAbort();
		return;
	}

	if (!fetSmartEnabled && regFeature != 0xD8)
	{
		CmdNoDataAbort();
		return;
	}

	switch (regFeature)
	{
		case 0xD9:
			SMART_EnableOps(false);
			return;
		case 0xD8:
			SMART_EnableOps(true);
			return;
		case 0xD2:
			SMART_SetAutoSaveAttribute();
			return;
		case 0xD3:
			SMART_SaveAttribute();
			return;
		case 0xDA:
			SMART_ReturnStatus();
			return;
		case 0xD1:
			Console.Error("DEV9: ATA: SMART_READ_THRESH Not Implemented");
			CmdNoDataAbort();
			return;
		case 0xD0:
			Console.Error("DEV9: ATA: SMART_READ_DATA Not Implemented");
			CmdNoDataAbort();
			return;
		case 0xD5:
			Console.Error("DEV9: ATA: SMART_READ_LOG Not Implemented");
			CmdNoDataAbort();
			return;
		case 0xD4:
			SMART_ExecuteOfflineImmediate();
			return;
		default:
			Console.Error("DEV9: ATA: Unknown SMART command %x", regFeature);
			CmdNoDataAbort();
			return;
	}
}

void ATA::SMART_SetAutoSaveAttribute()
{
	PreCmd();
	switch (regSector)
	{
		case 0x00:
			smartAutosave = false;
			break;
		case 0xF1:
			smartAutosave = true;
			break;
		default:
			Console.Error("DEV9: ATA: Unknown SMART_ATTR_AUTOSAVE command %s", regSector);
			CmdNoDataAbort();
			return;
	}
	PostCmdNoData();
}

void ATA::SMART_SaveAttribute()
{
	PreCmd();
	PostCmdNoData();
}

void ATA::SMART_ExecuteOfflineImmediate()
{
	PreCmd();
	[[maybe_unused]] int n = 0;
	switch (regSector)
	{
		case 0:
		case 1:
		case 2:
			smartSelfTestCount++;
			if (smartSelfTestCount > 21)
				smartSelfTestCount = 1;

			n = 2 + (smartSelfTestCount - 1) * 24;
			break;
		case 127:
			break;
		case 129:
		case 130:
			smartSelfTestCount++;
			if (smartSelfTestCount > 21)
			{
				smartSelfTestCount = 1;
			}
			n = 2 + (smartSelfTestCount - 1) * 24;

			SMART_ReturnStatus();
			return;
		default:
			CmdNoDataAbort();
			return;
	}
	PostCmdNoData();
}

void ATA::SMART_EnableOps(bool enable)
{
	PreCmd();
	fetSmartEnabled = enable;
	PostCmdNoData();
}

void ATA::SMART_ReturnStatus()
{
	PreCmd();
	if (!smartErrors)
	{
		regHcyl = 0xC2;
		regLcyl = 0x4F;
	}
	else
	{
		regHcyl = 0x2C;
		regLcyl = 0xF4;
	}
	PostCmdNoData();
}
