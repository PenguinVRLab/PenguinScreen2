// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <string>

class Error;
class ProgressCallback;

struct cdvdTrackIndex
{
	bool isPregap;
	u8 trackM;
	u8 trackS;
	u8 trackF;
	u8 discM;
	u8 discS;
	u8 discF;
};

struct cdvdTrack
{
	u32 start_lba;
	u8 type;
	u8 trackNum;
	u8 trackIndex;
	u8 trackM;
	u8 trackS;
	u8 trackF;
	u8 discM;
	u8 discS;
	u8 discF;

	cdvdTrackIndex index[2];
};

struct cdvdSubQ
{
	u8 ctrl : 4;
	u8 adr : 4;
	u8 trackNum;
	u8 trackIndex;
	u8 trackM;
	u8 trackS;
	u8 trackF;
	u8 pad;
	u8 discM;
	u8 discS;
	u8 discF;
};

struct cdvdTD
{
	u32 lsn;
	u8 type;
};

struct cdvdTN
{
	u8 strack;
	u8 etrack;
};

#define CDVD_SPINDLE_SPEED 0x7
#define CDVD_SPINDLE_NOMINAL 0x40
#define CDVD_SPINDLE_CAV 0x80

#define CDVD_MODE_2352 0
#define CDVD_MODE_2340 1
#define CDVD_MODE_2328 2
#define CDVD_MODE_2048 3
#define CDVD_MODE_2368 4

#define CDVD_TYPE_ILLEGAL 0xff
#define CDVD_TYPE_DVDV 0xfe
#define CDVD_TYPE_CDDA 0xfd
#define CDVD_TYPE_PS2DVD 0x14
#define CDVD_TYPE_PS2CDDA 0x13
#define CDVD_TYPE_PS2CD 0x12
#define CDVD_TYPE_PSCDDA 0x11
#define CDVD_TYPE_PSCD 0x10
#define CDVD_TYPE_UNKNOWN 0x05
#define CDVD_TYPE_DETCTDVDD 0x04
#define CDVD_TYPE_DETCTDVDS 0x03
#define CDVD_TYPE_DETCTCD 0x02
#define CDVD_TYPE_DETCT 0x01
#define CDVD_TYPE_NODISC 0x00

#define CDVD_CONTROL_AUDIO_PREEMPHASIS(control) ((control & (4 << 1)))
#define CDVD_CONTROL_DIGITAL_COPY_ALLOWED(control) ((control & (5 << 1)))
#define CDVD_CONTROL_IS_DATA(control) ((control & (6 << 1)))
#define CDVD_CONTROL_IS_QUADRAPHONIC_AUDIO(control) ((control & (7 << 1)))

#define CDVD_TRAY_CLOSE 0x00
#define CDVD_TRAY_OPEN 0x01

#define CDVD_AUDIO_TRACK 0x01
#define CDVD_MODE1_TRACK 0x41
#define CDVD_MODE2_TRACK 0x61

#define CDVD_AUDIO_MASK 0x00
#define CDVD_DATA_MASK 0x40

typedef bool (*_CDVDopen)(std::string filename, Error* error);
typedef bool (*_CDVDprecache)(ProgressCallback* progress, Error* error);

typedef s32 (*_CDVDreadTrack)(u32 lsn, int mode);

typedef s32 (*_CDVDgetBuffer)(u8* buffer);

typedef s32 (*_CDVDreadSubQ)(u32 lsn, cdvdSubQ* subq);
typedef s32 (*_CDVDgetTN)(cdvdTN* Buffer);
typedef s32 (*_CDVDgetTD)(u8 Track, cdvdTD* Buffer);
typedef s32 (*_CDVDgetTOC)(void* toc);
typedef s32 (*_CDVDgetDiskType)();
typedef s32 (*_CDVDgetTrayStatus)();
typedef s32 (*_CDVDctrlTrayOpen)();
typedef s32 (*_CDVDctrlTrayClose)();
typedef s32 (*_CDVDreadSector)(u8* buffer, u32 lsn, int mode);
typedef s32 (*_CDVDgetDualInfo)(s32* dualType, u32* _layer1start);

typedef void (*_CDVDnewDiskCB)(void (*callback)());

enum class CDVD_SourceType : uint8_t
{
	Iso,
	Disc,
	NoDisc,
};

struct CDVD_API
{
	void (*close)();

	_CDVDopen open;
	_CDVDprecache precache;
	_CDVDreadTrack readTrack;
	_CDVDgetBuffer getBuffer;
	_CDVDreadSubQ readSubQ;
	_CDVDgetTN getTN;
	_CDVDgetTD getTD;
	_CDVDgetTOC getTOC;
	_CDVDgetDiskType getDiskType;
	_CDVDgetTrayStatus getTrayStatus;
	_CDVDctrlTrayOpen ctrlTrayOpen;
	_CDVDctrlTrayClose ctrlTrayClose;
	_CDVDnewDiskCB newDiskCB;

	_CDVDreadSector readSector;
	_CDVDgetDualInfo getDualInfo;
};

extern const CDVD_API* CDVD;

extern const CDVD_API CDVDapi_Iso;
extern const CDVD_API CDVDapi_Disc;
extern const CDVD_API CDVDapi_NoDisc;

extern u8 strack;
extern u8 etrack;
extern std::array<cdvdTrack, 100> tracks;

extern bool cdvdLock(Error* error = nullptr);

extern void cdvdUnlock();

extern void CDVDsys_ChangeSource(CDVD_SourceType type);
extern void CDVDsys_SetFile(CDVD_SourceType srctype, std::string newfile);
extern const std::string& CDVDsys_GetFile(CDVD_SourceType srctype);
extern CDVD_SourceType CDVDsys_GetSourceType();
extern void CDVDsys_ClearFiles();

extern bool DoCDVDopen(Error* error);
extern bool DoCDVDprecache(ProgressCallback* progress, Error* error);
extern void DoCDVDclose();
extern s32 DoCDVDreadSector(u8* buffer, u32 lsn, int mode);
extern s32 DoCDVDreadTrack(u32 lsn, int mode);
extern s32 DoCDVDgetBuffer(u8* buffer);
extern s32 DoCDVDdetectDiskType();
extern void DoCDVDresetDiskTypeCache();
