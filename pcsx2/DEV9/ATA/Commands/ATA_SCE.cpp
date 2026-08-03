// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/ATA/ATA.h"
#include "DEV9/DEV9.h"

void ATA::HDD_SCE()
{
	DevCon.WriteLn("DEV9: HDD_SCE SONY-SPECIFIC SECURITY CONTROL COMMAND %x", regFeature);

	switch (regFeature)
	{
		case 0xF1:
		case 0xF2:
		case 0xF3:
		case 0xF4:
		case 0xF5:
		case 0x20:
		case 0x30:
			Console.Error("DEV9: ATA: SCE command %x not implemented", regFeature);
			CmdNoDataAbort();
			break;
		case 0xEC:
			SCE_IDENTIFY_DRIVE();
			break;
		default:
			Console.Error("DEV9: ATA: Unknown SCE command %x", regFeature);
			CmdNoDataAbort();
			return;
	}
}

void ATA::SCE_IDENTIFY_DRIVE()
{
	PreCmd();

	pioDRQEndTransferFunc = nullptr;
	DRQCmdPIODataToHost(sceSec, 256 * 2, 0, 256 * 2, true);
}
