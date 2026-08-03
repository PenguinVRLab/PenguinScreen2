// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/CDVDcommon.h"
#include "CDVD/IsoReader.h"
#include "CDVD/IsoFileFormats.h"
#include "Config.h"
#include "Host.h"
#include "IconsFontAwesome.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/EnumOps.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/StringUtil.h"

#include <array>
#include <ctype.h>
#include <exception>
#include <memory>
#include <mutex>
#include <time.h>

#include "fmt/format.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#define ENABLE_TIMESTAMPS

const CDVD_API* CDVD = nullptr;

static int diskTypeCached = -1;

int lastReadSize;
u32 lastLSN;

static OutputIsoFile blockDumpFile;

u8 strack;
u8 etrack;
std::array<cdvdTrack, 100> tracks;

static void CheckNullCDVD()
{
	pxAssertMsg(CDVD, "Invalid CDVD object state (null pointer exception)");
}

static int CheckDiskTypeFS(int baseType)
{
	IsoReader isor;
	if (isor.Open())
	{
		std::vector<u8> data;
		if (isor.ReadFile("SYSTEM.CNF", &data))
		{
			if (StringUtil::ContainsSubString(data, "BOOT2"))
			{
				return (baseType == CDVD_TYPE_DETCTCD) ? CDVD_TYPE_PS2CD : CDVD_TYPE_PS2DVD;
			}

			if (StringUtil::ContainsSubString(data, "BOOT"))
			{
				return CDVD_TYPE_PSCD;
			}

			return CDVD_TYPE_ILLEGAL;
		}

		if (isor.FileExists("P2L_0100.02"))
			return CDVD_TYPE_PS2DVD;

		if (isor.FileExists("PSX.EXE"))
			return CDVD_TYPE_PSCD;

		if (isor.FileExists("VIDEO_TS/VIDEO_TS.IFO"))
			return CDVD_TYPE_DVDV;
	}

#ifdef PCSX2_DEVBUILD
	return CDVD_TYPE_PS2DVD;
#endif
	return CDVD_TYPE_ILLEGAL;
}

static int FindDiskType(int mType)
{
	int dataTracks = 0;
	int audioTracks = 0;
	int iCDType = mType;
	cdvdTN tn;

	CDVD->getTN(&tn);

	if (tn.strack != tn.etrack)
	{
		iCDType = CDVD_TYPE_DETCTCD;
	}
	else if (mType < 0)
	{
		static u8 bleh[CD_FRAMESIZE_RAW];
		cdvdTD td;

		CDVD->getTD(0, &td);
		if (td.lsn > 452849)
		{
			iCDType = CDVD_TYPE_DETCTDVDS;
		}
		else
		{
			if (DoCDVDreadSector(bleh, 16, CDVD_MODE_2048) == 0)
			{

				if (*(u16*)(bleh + 166) == *(u16*)(bleh + 171))
					iCDType = CDVD_TYPE_DETCTCD;
				else
					iCDType = CDVD_TYPE_DETCTDVDS;
			}
		}
	}

	if (iCDType == CDVD_TYPE_DETCTDVDS)
	{
		s32 dlt = 0;
		u32 l1s = 0;

		if (CDVD->getDualInfo(&dlt, &l1s) == 0)
		{
			if (dlt > 0)
				iCDType = CDVD_TYPE_DETCTDVDD;
		}
	}

	switch (iCDType)
	{
		case CDVD_TYPE_DETCTCD:
			Console.WriteLn(" * CDVD Disk Open: CD, %d tracks (%d to %d):", tn.etrack - tn.strack + 1, tn.strack, tn.etrack);
			break;

		case CDVD_TYPE_DETCTDVDS:
			Console.WriteLn(" * CDVD Disk Open: DVD, Single layer or unknown:");
			break;

		case CDVD_TYPE_DETCTDVDD:
			Console.WriteLn(" * CDVD Disk Open: DVD, Double layer:");
			break;
	}

	audioTracks = dataTracks = 0;
	for (int i = tn.strack; i <= tn.etrack; i++)
	{
		cdvdTD td, td2;

		CDVD->getTD(i, &td);

		if (tn.etrack > i)
			CDVD->getTD(i + 1, &td2);
		else
			CDVD->getTD(0, &td2);

		int tlength = td2.lsn - td.lsn;

		if (td.type == CDVD_AUDIO_TRACK)
		{
			audioTracks++;
			Console.WriteLn(" * * Track %d: Audio (%d sectors)", i, tlength);
		}
		else
		{
			dataTracks++;
			Console.WriteLn(" * * Track %d: Data (Mode %d) (%d sectors)", i, ((td.type == CDVD_MODE1_TRACK) ? 1 : 2), tlength);
		}
	}

	if (dataTracks > 0)
	{
		iCDType = CheckDiskTypeFS(iCDType);
	}

	if (audioTracks > 0)
	{
		switch (iCDType)
		{
			case CDVD_TYPE_PS2CD:
				iCDType = CDVD_TYPE_PS2CDDA;
				break;
			case CDVD_TYPE_PSCD:
				iCDType = CDVD_TYPE_PSCDDA;
				break;
			default:
				iCDType = CDVD_TYPE_CDDA;
				break;
		}
	}

	return iCDType;
}

static void DetectDiskType()
{
	if (CDVD->getTrayStatus() == CDVD_TRAY_OPEN)
	{
		diskTypeCached = CDVD_TYPE_NODISC;
		return;
	}

	int baseMediaType = CDVD->getDiskType();
	int mType = -1;

	switch (baseMediaType)
	{
#if 0
		case CDVD_TYPE_CDDA:
		case CDVD_TYPE_PSCD:
		case CDVD_TYPE_PS2CD:
		case CDVD_TYPE_PSCDDA:
		case CDVD_TYPE_PS2CDDA:
			mType = CDVD_TYPE_DETCTCD;
			break;

		case CDVD_TYPE_DVDV:
		case CDVD_TYPE_PS2DVD:
			mType = CDVD_TYPE_DETCTDVDS;
			break;

		case CDVD_TYPE_DETCTDVDS:
		case CDVD_TYPE_DETCTDVDD:
		case CDVD_TYPE_DETCTCD:
			mType = baseMediaType;
			break;
#endif

		case CDVD_TYPE_NODISC:
			diskTypeCached = CDVD_TYPE_NODISC;
			return;
	}

	diskTypeCached = FindDiskType(mType);
}

static std::string m_SourceFilename[3];
static CDVD_SourceType m_CurrentSourceType = CDVD_SourceType::NoDisc;
static std::mutex s_cdvd_lock;

bool cdvdLock(Error* error)
{
	if (!s_cdvd_lock.try_lock())
	{
		Error::SetString(error, TRANSLATE_STR("CDVD", "The CDVD system is currently in use."));
		return false;
	}

	return true;
}

void cdvdUnlock()
{
	s_cdvd_lock.unlock();
}

void CDVDsys_SetFile(CDVD_SourceType srctype, std::string newfile)
{
#ifdef WIN32
	if (Path::IsAbsolute(newfile))
	{
		const auto splitPath = Path::SplitNativePath(newfile);
		const auto root = fmt::format("{}\\", splitPath.at(0));

		const auto driveType = GetDriveType(StringUtil::UTF8StringToWideString(root).c_str());
		if (driveType == DRIVE_REMOVABLE)
		{
			Host::AddIconOSDMessage("RemovableDriveWarning", ICON_FA_TRIANGLE_EXCLAMATION,
				TRANSLATE_SV("CDVD", "Game disc location is on a removable drive, performance issues such as jittering "
									 "and freezing may occur."),
				Host::OSD_WARNING_DURATION);
		}
	}
#endif

	m_SourceFilename[enum_cast(srctype)] = std::move(newfile);
}

const std::string& CDVDsys_GetFile(CDVD_SourceType srctype)
{
	return m_SourceFilename[enum_cast(srctype)];
}

CDVD_SourceType CDVDsys_GetSourceType()
{
	return m_CurrentSourceType;
}

void CDVDsys_ClearFiles()
{
	for (u32 i = 0; i < std::size(m_SourceFilename); i++)
		m_SourceFilename[i] = {};
}

void CDVDsys_ChangeSource(CDVD_SourceType type)
{
	if (CDVD)
		DoCDVDclose();

	switch (m_CurrentSourceType = type)
	{
		case CDVD_SourceType::Iso:
			CDVD = &CDVDapi_Iso;
			break;

		case CDVD_SourceType::Disc:
			CDVD = &CDVDapi_Disc;
			break;

		case CDVD_SourceType::NoDisc:
			CDVD = &CDVDapi_NoDisc;
			break;

			jNO_DEFAULT;
	}
}

bool DoCDVDopen(Error* error)
{
	CheckNullCDVD();

	CDVD->newDiskCB(cdvdNewDiskCB);

	auto CurrentSourceType = enum_cast(m_CurrentSourceType);
	if (!CDVD->open(m_SourceFilename[CurrentSourceType], error))
		return false;

	int cdtype = DoCDVDdetectDiskType();

	if (!EmuConfig.CdvdDumpBlocks || (cdtype == CDVD_TYPE_NODISC))
	{
		blockDumpFile.Close();
		return true;
	}

	std::string dump_name(Path::GetFileTitle(m_SourceFilename[CurrentSourceType]));
	if (dump_name.empty())
		dump_name = "Untitled";

	if (EmuConfig.CurrentBlockdump.empty())
		EmuConfig.CurrentBlockdump = FileSystem::GetWorkingDirectory();

	std::string temp(Path::Combine(EmuConfig.CurrentBlockdump, dump_name));

#ifdef ENABLE_TIMESTAMPS
	std::time_t curtime_t = std::time(nullptr);
	struct tm curtime = {};
#ifdef _MSC_VER
	localtime_s(&curtime, &curtime_t);
#else
	localtime_r(&curtime_t, &curtime);
#endif

	temp += StringUtil::StdStringFromFormat(" (%04d-%02d-%02d %02d-%02d-%02d)",
		curtime.tm_year + 1900, curtime.tm_mon + 1, curtime.tm_mday,
		curtime.tm_hour, curtime.tm_min, curtime.tm_sec);
#endif
	temp += ".dump";

	cdvdTD td;
	CDVD->getTD(0, &td);

	Host::AddKeyedOSDMessage("BlockDumpCreate",
		fmt::format(TRANSLATE_FS("CDVD", "Saving CDVD block dump to '{}'."), temp), Host::OSD_INFO_DURATION);

	if (blockDumpFile.Create(std::move(temp), 2))
	{
		int blockofs = 0;
		uint blocksize = CD_FRAMESIZE_RAW;
		uint blocks = td.lsn;

		switch (cdtype)
		{
			case CDVD_TYPE_PS2DVD:
			case CDVD_TYPE_DVDV:
			case CDVD_TYPE_DETCTDVDS:
			case CDVD_TYPE_DETCTDVDD:
				blocksize = 2048;
				break;
		}
		blockDumpFile.WriteHeader(blockofs, blocksize, blocks);
	}


	return true;
}

bool DoCDVDprecache(ProgressCallback* progress, Error* error)
{
	CheckNullCDVD();
	progress->SetTitle(TRANSLATE("CDVD", "Precaching CDVD"));
	return CDVD->precache(progress, error);
}

void DoCDVDclose()
{
	CheckNullCDVD();

	blockDumpFile.Close();

	CDVD->close();

	DoCDVDresetDiskTypeCache();
}

s32 DoCDVDreadSector(u8* buffer, u32 lsn, int mode)
{
	CheckNullCDVD();
	int ret = CDVD->readSector(buffer, lsn, mode);

	if (ret == 0 && blockDumpFile.IsOpened())
	{
		if (blockDumpFile.GetBlockSize() == CD_FRAMESIZE_RAW && mode != CDVD_MODE_2352)
		{
			u8 blockDumpBuffer[CD_FRAMESIZE_RAW];
			if (CDVD->readSector(blockDumpBuffer, lsn, CDVD_MODE_2352) == 0)
				blockDumpFile.WriteSector(blockDumpBuffer, lsn);
		}
		else
		{
			blockDumpFile.WriteSector(buffer, lsn);
		}
	}

	return ret;
}

s32 DoCDVDreadTrack(u32 lsn, int mode)
{
	CheckNullCDVD();

	switch (mode)
	{
		case CDVD_MODE_2352:
			lastReadSize = 2352;
			break;
		case CDVD_MODE_2340:
			lastReadSize = 2340;
			break;
		case CDVD_MODE_2328:
			lastReadSize = 2328;
			break;
		case CDVD_MODE_2048:
			lastReadSize = 2048;
			break;
	}

	lastLSN = lsn;
	return CDVD->readTrack(lsn, mode);
}

s32 DoCDVDgetBuffer(u8* buffer)
{
	CheckNullCDVD();
	const int ret = CDVD->getBuffer(buffer);

	if (ret == 0 && blockDumpFile.IsOpened())
	{
		cdvdTD td;
		CDVD->getTD(0, &td);

		if (lastLSN >= td.lsn)
			return 0;

		if (blockDumpFile.GetBlockSize() == CD_FRAMESIZE_RAW && lastReadSize != 2352)
		{
			u8 blockDumpBuffer[CD_FRAMESIZE_RAW];
			if (CDVD->readSector(blockDumpBuffer, lastLSN, CDVD_MODE_2352) == 0)
				blockDumpFile.WriteSector(blockDumpBuffer, lastLSN);
		}
		else
		{
			blockDumpFile.WriteSector(buffer, lastLSN);
		}
	}

	return ret;
}

s32 DoCDVDdetectDiskType()
{
	CheckNullCDVD();
	if (diskTypeCached < 0)
		DetectDiskType();
	return diskTypeCached;
}

void DoCDVDresetDiskTypeCache()
{
	diskTypeCached = -1;
}

static bool NODISCopen(std::string filename, Error* error)
{
	return true;
}

static bool NODISCprecache(ProgressCallback* progress, Error* error)
{
	return true;
}

static void NODISCclose()
{
}

static s32 NODISCreadTrack(u32 lsn, int mode)
{
	return -1;
}

static s32 NODISCgetBuffer(u8* buffer)
{
	return -1;
}

static s32 NODISCreadSubQ(u32 lsn, cdvdSubQ* subq)
{
	return -1;
}

static s32 NODISCgetTN(cdvdTN* Buffer)
{
	return -1;
}

static s32 NODISCgetTD(u8 Track, cdvdTD* Buffer)
{
	return -1;
}

static s32 NODISCgetTOC(void* toc)
{
	return -1;
}

static s32 NODISCgetDiskType()
{
	return CDVD_TYPE_NODISC;
}

static s32 NODISCgetTrayStatus()
{
	return CDVD_TRAY_CLOSE;
}

static s32 NODISCdummyS32()
{
	return 0;
}

static void NODISCnewDiskCB(void (* )())
{
}

static s32 NODISCreadSector(u8* tempbuffer, u32 lsn, int mode)
{
	return -1;
}

static s32 NODISCgetDualInfo(s32* dualType, u32* _layer1start)
{
	return -1;
}

const CDVD_API CDVDapi_NoDisc =
	{
		NODISCclose,
		NODISCopen,
		NODISCprecache,
		NODISCreadTrack,
		NODISCgetBuffer,
		NODISCreadSubQ,
		NODISCgetTN,
		NODISCgetTD,
		NODISCgetTOC,
		NODISCgetDiskType,
		NODISCgetTrayStatus,
		NODISCdummyS32,
		NODISCdummyS32,

		NODISCnewDiskCB,

		NODISCreadSector,
		NODISCgetDualInfo,
};
