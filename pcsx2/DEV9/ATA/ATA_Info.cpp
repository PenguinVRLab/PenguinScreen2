// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ATA.h"
#include "DEV9/DEV9.h"

void ATA::WriteUInt16(u8* data, int* index, u16 value)
{
	*(u16*)&data[*index] = value;
	*index += sizeof(value);
}

void ATA::WriteUInt32(u8* data, int* index, u32 value)
{
	*(u32*)&data[*index] = value;
	*index += sizeof(value);
}

void ATA::WriteUInt64(u8* data, int* index, u64 value)
{
	*(u64*)&data[*index] = value;
	*index += sizeof(value);
}

void ATA::WritePaddedString(u8* data, int* index, const std::string& value, u32 len)
{
	memset(&data[*index], (u8)' ', len);
	memcpy(&data[*index], value.c_str(), value.length() < len ? value.length() : len);
	*index += len;
}

void ATA::CreateHDDinfo(u64 sizeSectors)
{
	u64 maxSize = (1 << 28) - 1;
	const u32 nbSectors = std::min<u32>(sizeSectors, maxSize);
	if (lba48Supported)
		maxSize = (1ULL << 48) - 1;

	sizeSectors = std::min<u64>(sizeSectors, maxSize);

	constexpr u16 sectorSize = 512;
	DevCon.WriteLn("DEV9: ATA: HddSize : %i", sizeSectors * sectorSize / (1024 * 1024));
	DevCon.WriteLn("DEV9: ATA: sizeSectors : %i", sizeSectors);

	memset(&identifyData, 0, sizeof(identifyData));
	constexpr u16 defHeads = 16;
	constexpr u16 defSectors = 63;
	u64 cylinderslong = std::min<u64>(nbSectors, 16514064) / defHeads / defSectors;
	const u16 defCylinders = (u16)std::min<u64>(cylinderslong, UINT16_MAX);

	cylinderslong = std::min<u64>(nbSectors, 16514064) / curHeads / curSectors;
	curCylinders = (u16)std::min<u64>(cylinderslong, UINT16_MAX);

	const int curOldsize = curCylinders * curHeads * curSectors;

	int index = 0;
	WriteUInt16(identifyData, &index, 0x0040);
	WriteUInt16(identifyData, &index, defCylinders);
	WriteUInt16(identifyData, &index, 0xC837);
	WriteUInt16(identifyData, &index, defHeads);
	WriteUInt16(identifyData, &index, sectorSize * defSectors);
	WriteUInt16(identifyData, &index, sectorSize);
	WriteUInt16(identifyData, &index, defSectors);
	index += 2 * 2;
	index += 1 * 2;
	WritePaddedString(identifyData, &index, "PCSX2-DEV9-ATA-HDD", 20);
	WriteUInt16(identifyData, &index, 0);
	WriteUInt16(identifyData, &index, 0);
	WriteUInt16(identifyData, &index, 0);
	WritePaddedString(identifyData, &index, "FIRM100", 8);
	WritePaddedString(identifyData, &index, "PCSX2-DEV9-ATA-HDD", 40);
	WriteUInt16(identifyData, &index, 128 | (0x80 << 8));
	index += 1 * 2;
	WriteUInt16(identifyData, &index, ((1 << 11) | (1 << 9) | (1 << 8)));
	WriteUInt16(identifyData, &index, 1 << 14);
	WriteUInt16(identifyData, &index, (pioMode > 2 ? pioMode : 2) << 8);
	WriteUInt16(identifyData, &index, 0);
	WriteUInt16(identifyData, &index, (1 | (1 << 1) | (1 << 2)));
	WriteUInt16(identifyData, &index, curCylinders);
	WriteUInt16(identifyData, &index, curHeads);
	WriteUInt16(identifyData, &index, curSectors);
	WriteUInt32(identifyData, &index, curOldsize);
	WriteUInt16(identifyData, &index, curMultipleSectorsSetting | (1 << 8));
	WriteUInt32(identifyData, &index, nbSectors);
	index += 1 * 2;
	if (mdmaMode >= 0)
		WriteUInt16(identifyData, &index, 0x07 | (1 << (mdmaMode + 8)));
	else
		WriteUInt16(identifyData, &index, 0x07);
	WriteUInt16(identifyData, &index, 0x03);
	WriteUInt16(identifyData, &index, 120);
	WriteUInt16(identifyData, &index, 120);
	WriteUInt16(identifyData, &index, 120);
	WriteUInt16(identifyData, &index, 120);
	index = 80 * 2;
	WriteUInt16(identifyData, &index, 0x70);
	WriteUInt16(identifyData, &index, 0x18);
	// clang-format off
	WriteUInt16(identifyData, &index, (
		(1 << 0) |
		(1 << 5) |
		(1 << 14)
));
	WriteUInt16(identifyData, &index, (
		(lba48Supported << 10) |
		(1 << 12) |
		(1 << 13) |
		(1 << 14)
));
	WriteUInt16(identifyData, &index, (
		(1 << 0) |
		(1 << 1) |
		(1 << 14)));
	WriteUInt16(identifyData, &index, (
		(fetSmartEnabled << 0) |
		(fetSecurityEnabled << 1) |
		(fetWriteCacheEnabled << 5) |
		(fetHostProtectedAreaEnabled << 10)|
		(1 << 14)));
	WriteUInt16(identifyData, &index, (
		(lba48Supported << 10) |
		(1 << 12) |
		(1 << 13)));
	WriteUInt16(identifyData, &index, (
		(1 << 0) |
		(1 << 1) |
		(1 << 14)));
	// clang-format on
	if (udmaMode >= 0)
		WriteUInt16(identifyData, &index, 0x7f | (1 << (udmaMode + 8)));
	else
		WriteUInt16(identifyData, &index, 0x7f);
	index = 93 * 2;
	if (GetSelectedDevice())
		WriteUInt16(identifyData, &index, ((1 << 14) | (0x3 << 8)));
	else
		WriteUInt16(identifyData, &index, ((1 << 14) | (1 << 3) | 0x3));
	index = 100 * 2;
	if (lba48Supported)
		WriteUInt64(identifyData, &index, sizeSectors);
	else
		WriteUInt64(identifyData, &index, 0);
	index = 106 * 2;
	WriteUInt16(identifyData, &index, ((1 << 14) | 0));
	CreateHDDinfoCsum();
}
void ATA::CreateHDDinfoCsum()
{
	u8 counter = 0;

	for (int i = 0; i < (512 - 1); i++)
		counter += identifyData[i];

	counter += 0xA5;

	identifyData[510] = 0xA5;
	identifyData[511] = static_cast<u8>(255 - counter + 1);
	counter = 0;

	for (int i = 0; i < (512); i++)
		counter += identifyData[i];
}
