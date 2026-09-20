// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"

#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct McdSizeInfo
{
	u16 SectorSize;
	u16 EraseBlockSizeInSectors;
	u32 McdSizeInSectors;
	u8 Xor;
};

struct AvailableMcdInfo
{
	std::string name;
	std::string path;
	std::time_t modified_time;
	MemoryCardType type;
	MemoryCardFileType file_type;
	u32 size;
	bool formatted;
};

extern uint FileMcd_GetMtapPort(uint slot);
extern uint FileMcd_GetMtapSlot(uint slot);
extern bool FileMcd_IsMultitapSlot(uint slot);
extern std::string FileMcd_GetDefaultName(uint slot);

uint FileMcd_ConvertToSlot(uint port, uint slot);
void FileMcd_SetType();
void FileMcd_EmuOpen();
void FileMcd_EmuClose();
void FileMcd_CancelEject();
void FileMcd_Reopen(std::string new_serial);
void FileMcd_Swap();
s32 FileMcd_IsPresent(uint port, uint slot);
void FileMcd_GetSizeInfo(uint port, uint slot, McdSizeInfo* outways);
bool FileMcd_IsPSX(uint port, uint slot);
s32 FileMcd_Read(uint port, uint slot, u8* dest, u32 adr, int size);
s32 FileMcd_Save(uint port, uint slot, const u8* src, u32 adr, int size);
s32 FileMcd_EraseBlock(uint port, uint slot, u32 adr);
u64 FileMcd_GetCRC(uint port, uint slot);
void FileMcd_NextFrame(uint port, uint slot);
int FileMcd_ReIndex(uint port, uint slot, const std::string& filter);

std::vector<AvailableMcdInfo> FileMcd_GetAvailableCards(bool include_in_use_cards);
std::optional<AvailableMcdInfo> FileMcd_GetCardInfo(const std::string_view name);
bool FileMcd_IsMemoryCardFormatted(const std::string& path);
bool FileMcd_IsMemoryCardFormatted(std::FILE* fp);
bool FileMcd_CreateNewCard(const std::string_view name, MemoryCardType type, MemoryCardFileType file_type);
bool FileMcd_RenameCard(const std::string_view name, const std::string_view new_name);
bool FileMcd_DeleteCard(const std::string_view name);