// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <vector>

const u32 ThreadListInstructions[3] =
{
	0xac420000,
	0x00000000,
	0x00000000,
};

struct BiosDebugInformation
{
	u32 eeThreadListAddr;
	u32 iopThreadListAddr;
	u32 iopModListAddr;
};

// Obained from the Open PS2SDK here https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/osd_config.h
// Only used for HLEing ConfigParam Syscalls in fast boot
typedef struct
{
	union
	{
		struct
		{
 u32 spdifMode : 1;
 u32 screenType : 2;
 u32 videoOutput : 1;
 u32 japLanguage : 1;
 u32 ps1drvConfig : 8;
 u32 version : 3;
 u32 language : 5;
 s32 timezoneOffset : 11;
 u8 timeZoneID : 7;
		};

		u8  UC[4];
		u16 US[2];
		u32 UL[1];
	};
} ConfigParam;

typedef struct
{
	union
	{
		struct
		{
 u8 format;

 u8 reserved : 4;
 u8 daylightSavings : 1;
 u8 timeFormat : 1;
 u8 dateFormat : 2;

 u8 version;
 u8 language;
		};

		u8  UC[4];
		u16 US[2];
		u32 UL[1];
	};
} Config2Param;

extern BiosDebugInformation CurrentBiosInformation;
extern u32 BiosVersion;
extern u32 BiosRegion;
extern bool NoOSD;
extern ConfigParam configParams1;
extern Config2Param configParams2;
extern bool ParamsRead;
extern bool AllowParams1;
extern bool AllowParams2;
extern u32 BiosChecksum;
extern std::string BiosDescription;
extern std::string BiosZone;

extern std::string BiosSerial;
extern std::string BiosPath;

extern std::vector<u8> BiosRom;

extern void ReadOSDConfigParames();

extern bool IsBIOS(const char* filename, u32& version, std::string& description, u32& region, std::string& zone);
extern bool IsBIOSAvailable(const std::string& full_path);

extern bool LoadBIOS();
extern void CopyBIOSToMemory();

// clang-format off
constexpr const char* TimeZoneLocations[128][2] = {
	{"Afghanistan", "Kabul"},
	{"Albania", "Tirana"},
	{"Algeria", "Algiers"},
	{"Andorra", "Andorra la Vella"},
	{"Armenia", "Yerevan"},
	{"Australia – Perth", "Perth"},
	{"Australia – Adelaide", "Adelaide"},
	{"Australia – Sydney", "Sydney"},
	{"Australia – Lord Howe Island", "Lord Howe Island"},
	{"Austria", "Vienna"},
	{"Azerbaijan", "Baku"},
	{"Bahrain", "Manama"},
	{"Bangladesh", "Dhaka"},
	{"Belarus", "Minsk"},
	{"Belgium", "Brussels"},
	{"Bosnia and Herzegovina", "Sarajevo"},
	{"Bulgaria", "Sofia"},
	{"Canada – Pacific, Yukon", "Pacific (Canada)"},
	{"Canada – Mountain", "Mountain (Canada)"},
	{"Canada – Central", "Central (Canada)"},
	{"Canada – Eastern", "Eastern (Canada)"},
	{"Canada – Atlantic", "Atlantic (Canada)"},
	{"Canada – Newfoundland", "Newfoundland"},
	{"Cape Verde", "Praia"},
	{"Chile – Santiago", "Santiago"},
	{"Chile – Easter Island", "Easter Island"},
	{"China", "Beijing"},
	{"Croatia", "Zagreb"},
	{"Cyprus", "Nicosia"},
	{"Czech Republic", "Prague"},
	{"Denmark", "Copenhagen"},
	{"Egypt", "Cairo"},
	{"Estonia", "Tallinn"},
	{"Fiji", "Suva"},
	{"Finland", "Helsinki"},
	{"France", "Paris"},
	{"Georgia", "Tbilisi"},
	{"Germany", "Berlin"},
	{"Gibraltar", "Gibraltar"},
	{"Greece", "Athens"},
	{"Greenland – Pituffik", "Northwestern Greenland"},
	{"Greenland – Greenland", "Southwestern Greenland"},
	{"Greenland – Ittoqqortoormiit", "Eastern Greenland"},
	{"Hungary", "Budapest"},
	{"Iceland", "Reykjavik"},
	{"India", "Calcutta"},
	{"Iran", "Tehran"},
	{"Iraq", "Baghdad"},
	{"Ireland", "Dublin"},
	{"Israel", "Jerusalem"},
	{"Italy", "Rome"},
	{"Japan", "Tokyo"},
	{"Jordan", "Amman"},
	{"Kazakhstan – Western", "Western Kazakhstan"},
	{"Kazakhstan – Central", "Central Kazakhstan"},
	{"Kazakhstan – Eastern", "Eastern Kazakhstan"},
	{"Kuwait", "Kuwait City"},
	{"Kyrgyzstan", "Bishkek"},
	{"Latvia", "Riga"},
	{"Lebanon", "Beirut"},
	{"Liechtenstein", "Vaduz"},
	{"Lithuania", "Vilnius"},
	{"Luxembourg", "Luxembourg"},
	{"Macedonia", "Skopje"},
	{"Malta", "Valletta"},
	{"Mexico – Tijuana", "Tijuana"},
	{"Mexico – Chihuahua", "Chihuahua"},
	{"Mexico – Mexico City", "Mexico City"},
	{"Midway Islands", "Midway Islands"},
	{"Monaco", "Monaco"},
	{"Morocco", "Casablanca"},
	{"Namibia", "Windhoek"},
	{"Nepal", "Kathmandu"},
	{"Netherlands", "Amsterdam"},
	{"New Caledonia", "New Caledonia"},
	{"New Zealand", "Wellington"},
	{"Norway", "Oslo"},
	{"Oman", "Muscat"},
	{"Pakistan", "Karachi"},
	{"Panama", "Panama City"},
	{"Poland", "Warsaw"},
	{"Portugal – Azores", "Azores"},
	{"Portugal – Lisbon", "Lisbon"},
	{"Puerto Rico", "Puerto Rico"},
	{"Reunion", "Reunion"},
	{"Romania", "Bucharest"},
	{"Russian Federation – Kaliningrad", "Kaliningrad"},
	{"Russian Federation – Moscow", "Moscow"},
	{"Russian Federation – Izhevsk", "Izhevsk"},
	{"Russian Federation – Perm", "Perm"},
	{"Russian Federation – Omsk", "Omsk"},
	{"Russian Federation – Norilsk", "Norilsk"},
	{"Russian Federation – Bratsk", "Bratsk"},
	{"Russian Federation – Yakutsk", "Yakutsk"},
	{"Russian Federation – Vladivostok", "Vladivostok"},
	{"Russian Federation – Magadan", "Magadan"},
	{"Russian Federation – Petropavlovsk-Kamchatsky", "Petropavlovsk-Kamchatsky"},
	{"Samoa", "Samoa Islands"},
	{"San Marino", "San Marino"},
	{"Saudi Arabia", "Riyadh"},
	{"Slovakia", "Bratislava"},
	{"Slovenia", "Ljubljana"},
	{"South Africa", "Johannesburg"},
	{"Spain – Canary Islands", "Canary Islands"},
	{"Spain – Madrid", "Madrid"},
	{"Sweden", "Stockholm"},
	{"Switzerland", "Bern"},
	{"Syria", "Damascus"},
	{"Tunisia", "Tunis"},
	{"Turkey", "Istanbul"},
	{"Ukraine", "Kiev"},
	{"United Arab Emirates", "Abu Dhabi"},
	{"United Kingdom", "London"},
	{"United States – Hawaii", "Hawaii"},
	{"United States – Alaska", "Alaska"},
	{"United States – Pacific", "Pacific (USA)"},
	{"United States – Mountain", "Mountain (USA)"},
	{"United States – Central", "Central (USA)"},
	{"United States – Eastern", "Eastern (USA)"},
	{"Uzbekistan", "Tashkent"},
	{"Venezuela", "Caracas"},
	{"Yugoslavia", "Belgrade"},
	{"Thailand", "Bangkok"},
	{"Hong Kong", "Hong Kong"},
	{"Malaysia", "Kuala Lumpur"},
	{"Singapore", "Singapore"},
	{"Taiwan", "Taipei"},
	{"South Korea", "Seoul"},
};
// clang-format on
