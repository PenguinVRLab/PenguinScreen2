// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "CDVDcommon.h"

#include <memory>
#include <string>
#include <string_view>

class Error;
class ElfObject;
class IsoReader;

#define btoi(b) ((b) / 16 * 10 + (b) % 16)
#define itob(i) ((i) / 10 * 16 + (i) % 10)

static __fi s32 msf_to_lsn(const u8* Time) noexcept
{
	u32 lsn;

	lsn = Time[2];
	lsn += (Time[1] - 2) * 75;
	lsn += Time[0] * 75 * 60;
	return lsn;
}

static __fi s32 msf_to_lba(const u8 m, const u8 s, const u8 f) noexcept
{
	u32 lsn;
	lsn = f;
	lsn += (s - 2) * 75;
	lsn += m * 75 * 60;
	return lsn;
}

static __fi void lsn_to_msf(u8* Time, s32 lsn) noexcept
{
	u8 m, s, f;

	lsn += 150;
	m = lsn / 4500;
	lsn = lsn - m * 4500;
	s = lsn / 75;
	f = lsn - (s * 75);
	Time[0] = itob(m);
	Time[1] = itob(s);
	Time[2] = itob(f);
}

static __fi void lba_to_msf(s32 lba, u8* m, u8* s, u8* f) noexcept
{
	lba += 150;
	*m = lba / (60 * 75);
	*s = (lba / 75) % 60;
	*f = lba % 75;
}

struct cdvdRTC
{
	u8 status;
	u8 second;
	u8 minute;
	u8 hour;
	u8 pad;
	u8 day;
	u8 month;
	u8 year;
};

enum class CDVDDiscType : u8
{
	Other,
	PS1Disc,
	PS2Disc
};

enum TrayStates
{
	CDVD_DISC_ENGAGED,
	CDVD_DISC_DETECTING,
	CDVD_DISC_SEEKING,
	CDVD_DISC_EJECT,
	CDVD_DISC_OPEN
};

struct cdvdTrayTimer
{
	u32 cdvdActionSeconds;
	TrayStates trayState;
};

struct cdvdStruct
{
	u8 nCommand;
	u8 Ready;
	u8 Error;
	u8 IntrStat;
	u8 Status;
	u8 StatusSticky;
	u8 DiscType;
	u8 sCommand;
	u8 sDataIn;
	u8 sDataOut;
	u8 HowTo;

	u8 NCMDParamBuff[16];
	u8 SCMDParamBuff[16];
	u8 SCMDResultBuff[16];

	u8 NCMDParamCnt;
	u8 NCMDParamPos;
	u8 SCMDParamCnt;
	u8 SCMDParamPos;
	u8 SCMDResultCnt;
	u8 SCMDResultPos;

	u8 CBlockIndex;
	u8 COffset;
	u8 CReadWrite;
	u8 CNumBlocks;

	double RTCcount;
	cdvdRTC RTC;

	u32 CurrentSector;
	int SectorCnt;
	int SeekCompleted;
	int Reading;
	int WaitingDMA;
	int ReadMode;
	int BlockSize;
	int Speed;
	int RetryCntMax;
	int CurrentRetryCnt;
	int ReadErr;
	int SpindlCtrl;

	u8 Key[16];
	u8 KeyXor;
	u8 decSet;

	u8 mg_buffer[65536];
	int mg_size;
	int mg_maxsize;
	int mg_datatype;
	u8 mg_kbit[16];
	u8 mg_kcon[16];

	u8 TrayTimeout;
	u8 Action;
	u32 SeekToSector;
	u32 MaxSector;
	u32 ReadTime;
	u32 RotSpeed;
	bool Spinning;
	cdvdTrayTimer Tray;
	u8 nextSectorsBuffered;
	bool AbortRequested;
};

extern cdvdStruct cdvd;

extern void cdvdReadLanguageParams(u8* config);

extern void cdvdLoadNVRAM();
extern void cdvdSaveNVRAM();
extern void cdvdReset();
extern void cdvdVsync();
extern void cdvdActionInterrupt();
extern void cdvdSectorReady();
extern void cdvdReadInterrupt();

extern void cdvdNewDiskCB();
extern u8 cdvdRead(u8 key);
extern void cdvdWrite(u8 key, u8 rt);

extern void cdvdGetDiscInfo(std::string* out_serial, std::string* out_elf_path, std::string* out_version, u32* out_crc,
	CDVDDiscType* out_disc_type);
extern u32 cdvdGetElfCRC(const std::string& path);
extern bool cdvdLoadElf(ElfObject* elfo, const std::string_view elfpath, bool isPSXElf, Error* error);
extern bool cdvdLoadDiscElf(ElfObject* elfo, IsoReader& isor, const std::string_view elfpath, bool isPSXElf, Error* error);

extern s32 cdvdCtrlTrayOpen();
extern s32 cdvdCtrlTrayClose();

