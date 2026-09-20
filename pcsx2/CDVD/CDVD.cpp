// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/CDVD.h"
#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD_internal.h"
#include "CDVD/IsoReader.h"
#include "CDVD/IsoFileFormats.h"
#include "GS.h"
#include "SIO/Sio.h"
#include "Elfheader.h"
#include "ps2/BiosTools.h"
#include "Recording/InputRecording.h"
#include "Host.h"
#include "R3000A.h"
#include "Common.h"
#include "IopBios.h"
#include "IopHw.h"
#include "IopDma.h"
#include "VMManager.h"

#include "common/BitUtils.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include <cctype>
#include <ctime>
#ifndef _WIN32
#include <time.h>
#endif
#include <memory>

cdvdStruct cdvd;

u32 PSXCLK = 36864000;

static constexpr s32 GMT9_OFFSET_SECONDS = 9 * 60 * 60;

static constexpr u8 monthmap[13] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static constexpr u8 cdvdParamLength[16] = { 0, 0, 0, 0, 0, 4, 11, 11, 11, 1, 255, 255, 7, 2, 11, 1 };

static constexpr size_t NVRAM_SIZE = 1024;
static u8 s_nvram[NVRAM_SIZE];

static constexpr u32 DEFAULT_MECHA_VERSION = 0x00020603;
static u32 s_mecha_version = 0;

static __fi void SetSCMDResultSize(u8 size) noexcept
{
	cdvd.SCMDResultCnt = size;
	cdvd.SCMDResultPos = 0;
	cdvd.sDataIn &= ~0x40;
	memset(&cdvd.SCMDResultBuff[0], 0, size);
}

static void CDVDCancelReadAhead()
{
	cdvd.nextSectorsBuffered = 0;
	psxRegs.interrupt &= ~(1 << IopEvt_CdvdSectorReady);
}

static void CDVDSECTORREADY_INT(u32 eCycle)
{
	if (psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady))
		return;

	if (EmuConfig.Speedhacks.fastCDVD)
	{
		if (eCycle < Cdvd_FullSeek_Cycles && eCycle > 1)
			eCycle *= 0.5f;
	}

	PSX_INT(IopEvt_CdvdSectorReady, eCycle);
}

static void CDVDREAD_INT(u32 eCycle)
{
	if (EmuConfig.Speedhacks.fastCDVD)
	{
		if (eCycle < Cdvd_FullSeek_Cycles && eCycle > 1)
			eCycle *= 0.5f;
	}

	PSX_INT(IopEvt_CdvdRead, eCycle);
}

static void CDVD_INT(int eCycle)
{
	if (eCycle == 0)
		cdvdActionInterrupt();
	else
		PSX_INT(IopEvt_Cdvd, eCycle);
}

static void cdvdSetIrq(uint id = (1 << Irq_CommandComplete))
{
	if (!(cdvd.IntrStat & id))
	{
		iopIntcIrq(2);
		psxSetNextBranchDelta(20);
	}
	else
		DevCon.Warning("CDVD trying to double issue IRQ %x", id);

	cdvd.IntrStat |= id;
	cdvd.AbortRequested = false;
}

static int mg_BIToffset(u8* buffer)
{
	int i, ofs = 0x20;
	for (i = 0; i < GetBufferU16(&buffer[0], 0x1A); i++)
		ofs += 0x10;

	if (GetBufferU16(&buffer[0], 0x18) & 1)
		ofs += buffer[ofs];
	if ((GetBufferU16(&buffer[0], 0x18) & 0xF000) == 0)
		ofs += 8;

	return ofs + 0x20;
}

const NVMLayout* getNvmLayout() noexcept
{
	return (nvmlayouts[1].biosVer <= BiosVersion) ? &nvmlayouts[1] : &nvmlayouts[0];
}

static void cdvdCreateNewNVM()
{
	std::memset(s_nvram, 0, sizeof(s_nvram));

	const NVMLayout* nvmLayout = getNvmLayout();
	if (((BiosVersion >> 8) == 2) && ((BiosVersion & 0xff) != 10))
		std::memcpy(&s_nvram[nvmLayout->regparams], PStwoRegionDefaults[BiosRegion], 12);

	static constexpr u8 ILinkID_Data[8] = {0x00, 0xAC, 0xFF, 0xFF, 0xFF, 0xFF, 0xB9, 0x86};
	std::memcpy(&s_nvram[nvmLayout->ilinkId], ILinkID_Data, sizeof(ILinkID_Data));
	if (nvmlayouts[1].biosVer <= BiosVersion)
	{
		static constexpr u8 ILinkID_checksum[2] = {0x00, 0x18};
		std::memcpy(&s_nvram[nvmLayout->ilinkId + 0x08], ILinkID_checksum, sizeof(ILinkID_checksum));
	}

	std::memcpy(&s_nvram[nvmLayout->config1 + 0x10], biosLangDefaults[BiosRegion], 16);
}

static std::string cdvdGetNVRAMPath()
{
	return Path::ReplaceExtension(BiosPath, "nvm");
}

void cdvdLoadNVRAM()
{
	Error error;
	const std::string nvmfile = cdvdGetNVRAMPath();
	auto fp = FileSystem::OpenManagedCFileTryIgnoreCase(nvmfile.c_str(), "rb", &error);
	if (!fp || std::fread(s_nvram, sizeof(s_nvram), 1, fp.get()) != 1)
	{
		ERROR_LOG("Failed to open or read NVRAM at {}: {}", Path::GetFileName(nvmfile), error.GetDescription());
		cdvdCreateNewNVM();
	}
	else
	{
		const NVMLayout* nvmLayout = getNvmLayout();
		constexpr u8 zero[16] = {0};

		if (std::memcmp(&s_nvram[nvmLayout->config1 + 0x10], zero, 16) == 0 ||
			(((BiosVersion >> 8) == 2) && ((BiosVersion & 0xff) != 10) &&
				(std::memcmp(&s_nvram[nvmLayout->regparams], zero, 12) == 0)))
		{
			ERROR_LOG("Language or Region Parameters missing, filling in defaults");
			cdvdCreateNewNVM();
		}
	}

	const std::string mecfile = Path::ReplaceExtension(BiosPath, "mec");
	fp = FileSystem::OpenManagedCFileTryIgnoreCase(mecfile.c_str(), "rb", &error);
	if (!fp || std::fread(&s_mecha_version, sizeof(s_mecha_version), 1, fp.get()) != 1)
	{
		s_mecha_version = DEFAULT_MECHA_VERSION;

		ERROR_LOG("Failed to open or read MEC file at {}: {}, creating default.", Path::GetFileName(nvmfile),
			error.GetDescription());
		fp.reset();
		fp = FileSystem::OpenManagedCFileTryIgnoreCase(mecfile.c_str(), "wb");
		if (!fp || std::fwrite(&s_mecha_version, sizeof(s_mecha_version), 1, fp.get()) != 1)
			Host::ReportErrorAsync("Error", "Failed to write MEC file. Check your BIOS setup/permission settings.");
	}
	DEV_LOG("Mechacon version: 0x{:08X}", s_mecha_version);
}

void cdvdSaveNVRAM()
{
	Error error;
	const std::string nvmfile = cdvdGetNVRAMPath();
	auto fp = FileSystem::OpenManagedCFileTryIgnoreCase(nvmfile.c_str(), "r+b", &error);
	if (!fp)
	{
		fp = FileSystem::OpenManagedCFileTryIgnoreCase(nvmfile.c_str(), "w+b", &error);
		if (!fp) [[unlikely]]
		{
			ERROR_LOG("Failed to open NVRAM at {} for updating: {}", Path::GetFileName(nvmfile), error.GetDescription());
			return;
		}
	}

	u8 existing_nvram[NVRAM_SIZE];
	if (std::fread(existing_nvram, sizeof(existing_nvram), 1, fp.get()) == 1 &&
		std::memcmp(existing_nvram, s_nvram, NVRAM_SIZE) == 0)
	{
		DEV_LOG("NVRAM has not changed, not writing to disk.");
		return;
	}

	if (FileSystem::FSeek64(fp.get(), 0, SEEK_SET) == 0 &&
		std::fwrite(s_nvram, NVRAM_SIZE, 1, fp.get()) == 1)
	{
		INFO_LOG("NVRAM saved to {}.", Path::GetFileName(nvmfile));
	}
	else
	{
		ERROR_LOG("Failed to save NVRAM to {}: {}", Path::GetFileName(nvmfile), Error::CreateErrno(errno).GetDescription());
	}
}

static void cdvdReadNVM(u8* dst, int offset, int bytes)
{
	int to_read = bytes;
	if (static_cast<size_t>(offset + bytes) > sizeof(s_nvram)) [[unlikely]]
	{
		WARNING_LOG("CDVD: Out of bounds NVRAM read: offset={}, bytes={}", offset, bytes);
		to_read = std::max(static_cast<int>(sizeof(s_nvram)) - offset, 0);
		pxAssert((bytes - to_read) > 0);
		std::memset(dst + to_read, 0, bytes - to_read);
	}

	if (to_read > 0) [[likely]]
		std::memcpy(dst, &s_nvram[offset], to_read);
}

static void cdvdWriteNVM(const u8* src, int offset, int bytes)
{
	int to_write = bytes;
	if (static_cast<size_t>(offset + bytes) > sizeof(s_nvram)) [[unlikely]]
	{
		WARNING_LOG("CDVD: Out of bounds NVRAM write: offset={}, bytes={}", offset, bytes);
		to_write = std::max(static_cast<int>(sizeof(s_nvram)) - offset, 0);
	}

	if (to_write > 0) [[likely]]
		std::memcpy(&s_nvram[offset], src, to_write);
}

static void cdvdReadConsoleID(u8* id)
{
	cdvdReadNVM(id, getNvmLayout()->consoleId, 8);
}
static void cdvdWriteConsoleID(const u8* id)
{
	cdvdWriteNVM(id, getNvmLayout()->consoleId, 8);
}

static void cdvdReadILinkID(u8* id)
{
	cdvdReadNVM(id, getNvmLayout()->ilinkId, 8);
}
static void cdvdWriteILinkID(const u8* id)
{
	cdvdWriteNVM(id, getNvmLayout()->ilinkId, 8);
}

static void cdvdReadModelNumber(u8* num, s32 part)
{
	cdvdReadNVM(num, getNvmLayout()->modelNum + part, 8);
}
static void cdvdWriteModelNumber(const u8* num, s32 part)
{
	cdvdWriteNVM(num, getNvmLayout()->modelNum + part, 8);
}

static void cdvdReadRegionParams(u8* num)
{
	cdvdReadNVM(num, getNvmLayout()->regparams, 8);
}
static void cdvdWriteRegionParams(const u8* num)
{
	cdvdWriteNVM(num, getNvmLayout()->regparams, 8);
}

static void cdvdReadMAC(u8* num)
{
	cdvdReadNVM(num, getNvmLayout()->mac, 8);
}
static void cdvdWriteMAC(const u8* num)
{
	cdvdWriteNVM(num, getNvmLayout()->mac, 8);
}

void cdvdReadLanguageParams(u8* config)
{
	cdvdReadNVM(config, getNvmLayout()->config1 + 0xF, 16);
}

s32 cdvdReadConfig(u8* config)
{
	if (cdvd.CReadWrite != 0)
	{
		config[0] = 0x80;
		memset(&config[1], 0x00, 15);
		return 1;
	}
	else if (cdvd.CBlockIndex >= cdvd.CNumBlocks)
		return 1;
	else if (
		((cdvd.COffset == 0) && (cdvd.CBlockIndex >= 4)) ||
		((cdvd.COffset == 1) && (cdvd.CBlockIndex >= 2)) ||
		((cdvd.COffset == 2) && (cdvd.CBlockIndex >= 7)))
	{
		memset(config, 0, 16);
		return 0;
	}

	const NVMLayout* nvmLayout = getNvmLayout();
	switch (cdvd.COffset)
	{
		case 0:
			cdvdReadNVM(config, nvmLayout->config0 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		case 2:
			cdvdReadNVM(config, nvmLayout->config2 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		default:
		{
			cdvdReadNVM(config, nvmLayout->config1 + (cdvd.CBlockIndex * 16), 16);
			if (cdvd.CBlockIndex == 1 && (NoOSD || VMManager::Internal::WasFastBooted()))
			{
				config[2] |= 0x80;
			}

			cdvd.CBlockIndex++;
		}
		break;
	}
	return 0;
}
s32 cdvdWriteConfig(const u8* config)
{
	if ((cdvd.CReadWrite != 1) || (cdvd.CBlockIndex >= cdvd.CNumBlocks))
		return 1;
	else if (
		((cdvd.COffset == 0) && (cdvd.CBlockIndex >= 4)) ||
		((cdvd.COffset == 1) && (cdvd.CBlockIndex >= 2)) ||
		((cdvd.COffset == 2) && (cdvd.CBlockIndex >= 7)))
		return 0;

	const NVMLayout* nvmLayout = getNvmLayout();
	switch (cdvd.COffset)
	{
		case 0:
			cdvdWriteNVM(config, nvmLayout->config0 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		case 2:
			cdvdWriteNVM(config, nvmLayout->config2 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
		default:
			cdvdWriteNVM(config, nvmLayout->config1 + ((cdvd.CBlockIndex++) * 16), 16);
			break;
	}
	return 0;
}

static bool cdvdUncheckedLoadDiscElf(ElfObject* elfo, IsoReader& isor, const std::string_view elfpath, bool isPSXElf, Error* error)
{
	size_t start_pos = (elfpath[5] == '0') ? 7 : 6;
	while (start_pos < elfpath.size() && (elfpath[start_pos] == '\\' || elfpath[start_pos] == '/'))
		start_pos++;

	size_t length = elfpath.length() - start_pos;
	const size_t semi_pos = elfpath.find(';', start_pos);
	if (semi_pos != std::string::npos)
		length = semi_pos - start_pos;

	std::string iso_filename(elfpath.substr(start_pos, length));
	DevCon.WriteLn(fmt::format("cdvdLoadElf(): '{}' -> '{}' in ISO.", elfpath, iso_filename));
	if (iso_filename.empty())
	{
		Error::SetString(error, "ISO filename is empty.");
		return false;
	}

	return elfo->OpenIsoFile(std::move(iso_filename), isor, isPSXElf, error);
}

bool cdvdLoadElf(ElfObject* elfo, const std::string_view elfpath, bool isPSXElf, Error* error)
{
	if (R3000A::ioman::is_host(elfpath))
	{
		const std::string_view path(elfpath.substr(elfpath.find(':') + 1));
		const std::string file_path(R3000A::ioman::host_path(path, false));
		return elfo->OpenFile(file_path, isPSXElf, error);
	}
	else if (elfpath.starts_with("cdrom:") || elfpath.starts_with("cdrom0:"))
	{
		IsoReader isor;
		if (!isor.Open(error))
			return false;

		return cdvdLoadDiscElf(elfo, isor, elfpath, isPSXElf, error);
	}
	else
	{
		Console.Error(fmt::format("cdvdLoadElf(): Unknown device in ELF path '{}'", elfpath));
		return false;
	}
}

bool cdvdLoadDiscElf(ElfObject* elfo, IsoReader& isor, const std::string_view elfpath, bool isPSXElf, Error* error)
{
	if (!elfpath.starts_with("cdrom:") && !elfpath.starts_with("cdrom0:"))
		return false;

	return cdvdUncheckedLoadDiscElf(elfo, isor, elfpath, isPSXElf, error);
}

u32 cdvdGetElfCRC(const std::string& path)
{
	ElfObject elfo;
	if (!elfo.OpenFile(path, false, nullptr))
		return 0;

	return elfo.GetCRC();
}

static CDVDDiscType GetPS2ElfName(IsoReader& isor, std::string* name, std::string* version, Error* error)
{
	CDVDDiscType retype = CDVDDiscType::Other;
	name->clear();
	version->clear();

	std::vector<u8> data;
	if (!isor.ReadFile("SYSTEM.CNF", &data, error))
		return CDVDDiscType::Other;

	const std::vector<std::string_view> lines =
		StringUtil::SplitString(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), '\n');
	for (size_t lineno = 0; lineno < lines.size(); lineno++)
	{
		const std::string_view line = StringUtil::StripWhitespace(lines[lineno]);
		std::string_view key, value;
		if (!StringUtil::ParseAssignmentString(line, &key, &value))
			continue;

		if (value.empty() && (lineno == (lines.size() - 1)))
		{
			Console.Warning("(SYSTEM.CNF) Unusual or malformed entry in SYSTEM.CNF ignored:");
			Console.WarningFmt("  {}", line);
			continue;
		}

		if (key == "BOOT2")
		{
			DevCon.WriteLn(Color_StrongBlue, fmt::format("(SYSTEM.CNF) Detected PS2 Disc = {}", value));
			*name = value;
			retype = CDVDDiscType::PS2Disc;
		}
		else if (key == "BOOT")
		{
			DevCon.WriteLn(Color_StrongBlue, fmt::format("(SYSTEM.CNF) Detected PSX/PSone Disc = {}", value));
			*name = value;
			retype = CDVDDiscType::PS1Disc;
		}
		else if (key == "VMODE")
		{
			DevCon.WriteLn(Color_Blue, fmt::format("(SYSTEM.CNF) Disc region type = {}", value));
		}
		else if (key == "VER")
		{
			DevCon.WriteLn(Color_Blue, fmt::format("(SYSTEM.CNF) Software version = {}", value));
			*version = value;
		}
	}

	Error::SetString(error, "Disc image is *not* a PlayStation or PS2 game");
	return retype;
}

static std::string ExecutablePathToSerial(const std::string& path)
{
	std::string::size_type pos = path.rfind('\\');
	std::string serial;
	if (pos != std::string::npos)
	{
		serial = path.substr(pos + 1);
	}
	else
	{
		pos = path.rfind(':');
		if (pos != std::string::npos)
			serial = path.substr(pos + 1);
		else
			serial = path;
	}

	pos = serial.rfind(';');
	if (pos != std::string::npos)
		serial.erase(pos);

	if (!StringUtil::WildcardMatch(serial.c_str(), "????_???.??*") &&
		!StringUtil::WildcardMatch(serial.c_str(), "????""-???.??*"))
	{
		serial.clear();
	}

	for (std::string::size_type pos = 0; pos < serial.size();)
	{
		if (serial[pos] == '.')
		{
			serial.erase(pos, 1);
			continue;
		}

		if (serial[pos] == '_')
			serial[pos] = '-';
		else
			serial[pos] = static_cast<char>(std::toupper(serial[pos]));

		pos++;
	}

	return serial;
}

void cdvdGetDiscInfo(std::string* out_serial, std::string* out_elf_path, std::string* out_version, u32* out_crc,
	CDVDDiscType* out_disc_type)
{
	Error error;
	IsoReader isor;

	std::string elfpath, version;
	CDVDDiscType disc_type = CDVDDiscType::Other;
	if (!isor.Open(&error) || (disc_type = GetPS2ElfName(isor, &elfpath, &version, &error)) == CDVDDiscType::Other)
		Console.Error(fmt::format("Failed to get ELF name: {}", error.GetDescription()));

	if (out_crc)
	{
		u32 crc = 0;

		if (disc_type == CDVDDiscType::PS2Disc || disc_type == CDVDDiscType::PS1Disc)
		{
			ElfObject elfo;
			const bool isPSXElf = (disc_type == CDVDDiscType::PS1Disc);
			if (!cdvdLoadDiscElf(&elfo, isor, elfpath, isPSXElf, &error))
				Console.Error(fmt::format("Failed to load ELF info for {}: {}", elfpath, error.GetDescription()));
			else
				crc = elfo.GetCRC();
		}

		*out_crc = crc;
	}

	if (out_serial)
	{
		if (disc_type != CDVDDiscType::Other)
			*out_serial = ExecutablePathToSerial(elfpath);
		else
			out_serial->clear();
	}
	if (out_elf_path)
		*out_elf_path = std::move(elfpath);
	if (out_version)
		*out_version = std::move(version);
	if (out_disc_type)
		*out_disc_type = disc_type;
}

void cdvdReadKey(u8, u16, u32 arg2, u8* key)
{
	const std::string DiscSerial = VMManager::GetDiscSerial();

	s32 numbers = 0, letters = 0;
	u32 key_0_3;
	u8 key_4, key_14;

	memset(key, 0, 16);

	if (!DiscSerial.empty())
	{
		numbers = StringUtil::FromChars<s32>(std::string_view(DiscSerial).substr(5, 5)).value_or(0);

		letters = static_cast<s32>((DiscSerial[3] & 0x7F) << 0)  |
				  static_cast<s32>((DiscSerial[2] & 0x7F) << 7)  |
				  static_cast<s32>((DiscSerial[1] & 0x7F) << 14) |
				  static_cast<s32>((DiscSerial[0] & 0x7F) << 21);
	}

	key_0_3 = ((numbers & 0x1FC00) >> 10) | ((0x01FFFFFF & letters) << 7);
	key_4 = ((numbers & 0x0001F) << 3) | ((0x0E000000 & letters) >> 25);
	key_14 = ((numbers & 0x003E0) >> 2) | 0x04;

	key[0] = (key_0_3 & 0x000000FF) >> 0;
	key[1] = (key_0_3 & 0x0000FF00) >> 8;
	key[2] = (key_0_3 & 0x00FF0000) >> 16;
	key[3] = (key_0_3 & 0xFF000000) >> 24;
	key[4] = key_4;

	switch (arg2)
	{
		case 75:
			key[14] = key_14;
			key[15] = 0x05;
			break;

		case 4246:
			key[0] = 0x07;
			key[1] = 0xF7;
			key[2] = 0xF2;
			key[3] = 0x01;
			key[4] = 0x00;
			key[15] = 0x01;
			break;

		default:
			key[15] = 0x01;
			break;
	}

	DevCon.WriteLn("CDVD.KEY = %02X,%02X,%02X,%02X,%02X,%02X,%02X",
		cdvd.Key[0], cdvd.Key[1], cdvd.Key[2], cdvd.Key[3], cdvd.Key[4], cdvd.Key[14], cdvd.Key[15]);
}

s32 cdvdGetToc(void* toc) noexcept
{
	s32 ret = CDVD->getTOC(toc);
	if (ret == -1)
		ret = 0x80;
	return ret;
}

s32 cdvdReadSubQ(s32 lsn, cdvdSubQ* subq) noexcept
{
	s32 ret = CDVD->readSubQ(lsn, subq);
	if (ret == -1)
		ret = 0x80;
	return ret;
}

static void cdvdDetectDisk()
{
	cdvd.DiscType = DoCDVDdetectDiskType();

	if (cdvd.DiscType != 0)
	{
		cdvdTD td;
		CDVD->getTD(0, &td);
		cdvd.MaxSector = td.lsn;
	}
}

static void cdvdUpdateStatus(cdvdStatus NewStatus) noexcept
{
	cdvd.Status = NewStatus;
	cdvd.StatusSticky |= NewStatus;
}

static void cdvdUpdateReady(u8 NewReadyStatus) noexcept
{
	cdvd.Ready = NewReadyStatus | (CDVD_DRIVE_MECHA_INIT | CDVD_DRIVE_DEV9CON);
}

s32 cdvdCtrlTrayOpen()
{
	if (cdvd.Status & CDVD_STATUS_TRAY_OPEN)
		return 0x80;

	DevCon.WriteLn(Color_Green, "Open virtual disk tray");

	if (CDVDsys_GetSourceType() == CDVD_SourceType::Disc)
	{
		cdvdNewDiskCB();
		return 0;
	}

	cdvdDetectDisk();
	cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
	cdvdUpdateReady(0);
	cdvd.Spinning = false;
	cdvdSetIrq(1 << Irq_Eject);

	return 0;
}

s32 cdvdCtrlTrayClose()
{
	if (!(cdvd.Status & CDVD_STATUS_TRAY_OPEN))
		return 0x80;

	DevCon.WriteLn(Color_Green, "Close virtual disk tray");

	if (VMManager::Internal::IsFastBootInProgress())
	{
		DevCon.WriteLn(Color_Green, "Media already loaded (fast boot)");
		cdvdUpdateReady(CDVD_DRIVE_READY);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.Spinning = true;
		cdvd.Tray.trayState = CDVD_DISC_ENGAGED;
		cdvd.Tray.cdvdActionSeconds = 0;
	}
	else
	{
		DevCon.WriteLn(Color_Green, "Detecting media");
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvdUpdateStatus(CDVD_STATUS_STOP);
		cdvd.Spinning = false;
		cdvd.Tray.trayState = CDVD_DISC_DETECTING;
		cdvd.Tray.cdvdActionSeconds = 3;
	}
	cdvdDetectDisk();

	return 0;
}

static s32 cdvdReadDvdDualInfo(s32* dualType, u32* layer1Start) noexcept
{
	*dualType = 0;
	*layer1Start = 0;

	return CDVD->getDualInfo(dualType, layer1Start);
}

static bool cdvdIsDVD() noexcept
{
	if (cdvd.DiscType == CDVD_TYPE_DETCTDVDS || cdvd.DiscType == CDVD_TYPE_DETCTDVDD || cdvd.DiscType == CDVD_TYPE_PS2DVD || cdvd.DiscType == CDVD_TYPE_DVDV)
		return true;
	else
		return false;
}

static int cdvdTrayStateDetecting()
{
	if (cdvd.Tray.trayState == CDVD_DISC_DETECTING)
		return CDVD_TYPE_DETCT;

	if (cdvdIsDVD())
	{
		u32 layer1Start;
		s32 dualType;

		cdvdReadDvdDualInfo(&dualType, &layer1Start);

		if (dualType > 0)
			return CDVD_TYPE_DETCTDVDD;
		else
			return CDVD_TYPE_DETCTDVDS;
	}

	if (cdvd.DiscType != CDVD_TYPE_NODISC)
		return CDVD_TYPE_DETCTCD;
	else
		return CDVD_TYPE_DETCT;
}
static u32 cdvdRotationTime(CDVD_MODE_TYPE mode)
{
	if (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV)
	{
		const float rotationPerSecond = static_cast<float>(((mode == MODE_CDROM) ? CD_MAX_ROTATION_X1 : DVD_MAX_ROTATION_X1) * cdvd.Speed) / 60.0f;
		const float msPerRotation = 1000.0f / rotationPerSecond;
		return static_cast<u32>((static_cast<float>(PSXCLK) / 1000.0f) * msPerRotation);
	}
	else
	{
		int numSectors = 0;
		int offset = 0;

		switch (cdvd.DiscType)
		{
			case CDVD_TYPE_DETCTDVDS:
			case CDVD_TYPE_PS2DVD:
			case CDVD_TYPE_DETCTDVDD:
				numSectors = 2298496;

				u32 layer1Start;
				s32 dualType;
				cdvdReadDvdDualInfo(&dualType, &layer1Start);
				if (cdvd.SeekToSector >= layer1Start)
					offset = layer1Start;
				break;
			default:
				numSectors = 360000;
				break;
		}
		const float sectorSpeed = (1.0f - ((static_cast<float>(cdvd.SeekToSector - offset) / numSectors) * 0.60f)) + 0.40f;

		const float rotationPerSecond = static_cast<float>(((mode == MODE_CDROM) ? CD_MAX_ROTATION_X1 : DVD_MAX_ROTATION_X1) * std::min(static_cast<float>(cdvd.Speed), (mode == MODE_CDROM) ? 10.3f : 1.6f) * sectorSpeed) / 60.0f;
		const float msPerRotation = 1000.0f / rotationPerSecond;
		return static_cast<u32>((static_cast<float>(PSXCLK) / 1000.0f) * msPerRotation);
	}
}

static uint cdvdBlockReadTime(CDVD_MODE_TYPE mode) noexcept
{
	if (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV)
	{
		int numSectors = 0;
		int offset = 0;

		switch (cdvd.DiscType)
		{
			case CDVD_TYPE_DETCTDVDS:
			case CDVD_TYPE_PS2DVD:
			case CDVD_TYPE_DETCTDVDD:
				numSectors = 2298496;
				u32 layer1Start;
				s32 dualType;

				cdvdReadDvdDualInfo(&dualType, &layer1Start);
				if (cdvd.SeekToSector >= layer1Start)
					offset = layer1Start;
				break;
			default:
				numSectors = 360000;
				break;
		}

		const float sectorSpeed = ((static_cast<float>(cdvd.SeekToSector - offset) / static_cast<float>(numSectors)) * 0.60f) + 0.40f;
		const float cycles = static_cast<float>(PSXCLK) / (static_cast<float>(((mode == MODE_CDROM) ? CD_SECTORS_PERSECOND : DVD_SECTORS_PERSECOND) * cdvd.Speed) * sectorSpeed);

		return static_cast<int>(cycles);
	}

	const float cycles = static_cast<float>(PSXCLK) / static_cast<float>(((mode == MODE_CDROM) ? CD_SECTORS_PERSECOND : DVD_SECTORS_PERSECOND) * std::min(static_cast<float>(cdvd.Speed), (mode == MODE_CDROM) ? 10.3f : 1.6f));

	return static_cast<int>(cycles);
}

void cdvdReset()
{
	std::memset(&cdvd, 0, sizeof(cdvd));

	cdvd.DiscType = CDVD_TYPE_NODISC;
	cdvd.Spinning = false;

	cdvd.sDataIn = 0x40;
	cdvdUpdateReady(CDVD_DRIVE_READY);
	cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
	cdvd.Speed = 4;
	cdvd.BlockSize = 2064;
	cdvd.Action = cdvdAction_None;
	cdvd.ReadTime = cdvdBlockReadTime(MODE_DVDROM);
	cdvd.RotSpeed = cdvdRotationTime(MODE_DVDROM);

	ReadOSDConfigParames();

	DevCon.WriteLn(Color_StrongGreen, configParams1.timezoneOffset < 0 ? "Time Zone Offset: GMT%03d:%02d" : "Time Zone Offset: GMT+%02d:%02d",
				   configParams1.timezoneOffset / 60, std::abs(configParams1.timezoneOffset % 60));

	if (configParams1.timeZoneID < 0x80)
	{
		const bool new_time_zone_ID_names = ((BiosVersion >> 8) == 2) || ((BiosVersion & 0xFF) >= 90);
		DevCon.WriteLn(Color_StrongGreen, "Time Zone Location: %s",
			TimeZoneLocations[configParams1.timeZoneID][new_time_zone_ID_names]);
	}
	else
	{
		DevCon.WriteLn(Color_StrongRed, "Invalid time zone configuration in BIOS (ID: %d)", configParams1.timeZoneID);
	}

	DevCon.WriteLn(Color_StrongGreen, "DST: %s Time", configParams2.daylightSavings ? "Summer" : "Winter");
	DevCon.WriteLn(Color_StrongGreen, "Time Format: %s-Hour", configParams2.timeFormat ? "12" : "24");
 	DevCon.WriteLn(Color_StrongGreen, "Date Format: %s", configParams2.dateFormat ? (configParams2.dateFormat == 2 ? "DD/MM/YYYY" : "MM/DD/YYYY") : "YYYY/MM/DD");
	DevCon.WriteLn(Color_StrongGreen, "System Time Basis: %s",
				   EmuConfig.ManuallySetRealTimeClock ? "Manual RTC" : g_InputRecording.isActive() ? "Default Input Recording Time" : "Operating System Time");

	std::tm input_tm{};
	std::tm resulting_tm{};

	const int bios_settings_offset_seconds = 60 * (configParams1.timezoneOffset + configParams2.daylightSavings * 60);

	if (EmuConfig.ManuallySetRealTimeClock)
	{
		resulting_tm.tm_sec = EmuConfig.RtcSecond;
		resulting_tm.tm_min = EmuConfig.RtcMinute;
		resulting_tm.tm_hour = EmuConfig.RtcHour;
		resulting_tm.tm_mday = EmuConfig.RtcDay;
		resulting_tm.tm_mon = EmuConfig.RtcMonth - 1;
		resulting_tm.tm_year = EmuConfig.RtcYear + 100;
		resulting_tm.tm_isdst = 0;

#if defined(_WIN32)
		const std::time_t input_time = _mkgmtime(&resulting_tm) + GMT9_OFFSET_SECONDS - bios_settings_offset_seconds;
		gmtime_s(&input_tm, &input_time);
#else
		const std::time_t input_time = timegm(&resulting_tm) + GMT9_OFFSET_SECONDS - bios_settings_offset_seconds;
		gmtime_r(&input_time, &input_tm);
#endif
	}
	else if (g_InputRecording.isActive())
	{
		input_tm.tm_sec = 0;
		input_tm.tm_min = 0;
		input_tm.tm_hour = 0;
		input_tm.tm_mday = 4;
		input_tm.tm_mon = 2;
		input_tm.tm_year = 120;
		input_tm.tm_isdst = 0;

#if defined(_WIN32)
		const std::time_t resulting_time = _mkgmtime(&input_tm) - GMT9_OFFSET_SECONDS + bios_settings_offset_seconds;
		gmtime_s(&resulting_tm, &resulting_time);
#else
		const std::time_t resulting_time = timegm(&input_tm) - GMT9_OFFSET_SECONDS + bios_settings_offset_seconds;
		gmtime_r(&resulting_time, &resulting_tm);
#endif
	}
	else
	{
		const std::time_t input_time = std::time(nullptr) + GMT9_OFFSET_SECONDS;
		const std::time_t resulting_time = input_time - GMT9_OFFSET_SECONDS + bios_settings_offset_seconds;

#ifdef _MSC_VER
		gmtime_s(&input_tm, &input_time);
		gmtime_s(&resulting_tm, &resulting_time);
#else
		gmtime_r(&input_time, &input_tm);
		gmtime_r(&resulting_time, &resulting_tm);
#endif
	}

	cdvd.RTC.second = static_cast<u8>(input_tm.tm_sec);
	cdvd.RTC.minute = static_cast<u8>(input_tm.tm_min);
	cdvd.RTC.hour = static_cast<u8>(input_tm.tm_hour);
	cdvd.RTC.day = static_cast<u8>(input_tm.tm_mday);
	cdvd.RTC.month = static_cast<u8>(input_tm.tm_mon + 1);
	cdvd.RTC.year = static_cast<u8>(input_tm.tm_year - 100);

	DevCon.WriteLn(Color_StrongGreen, "Resulting System Time: 20%02u-%02u-%02u %02u:%02u:%02u",
				   resulting_tm.tm_year - 100, resulting_tm.tm_mon + 1, resulting_tm.tm_mday,
				   resulting_tm.tm_hour, resulting_tm.tm_min, resulting_tm.tm_sec);

	cdvdCtrlTrayClose();
}

bool SaveStateBase::cdvdFreeze()
{
	if (!FreezeTag("cdvd"))
		return false;

	Freeze(cdvd);
	if (!IsOkay())
		return false;

	if (IsLoading())
	{

		if (cdvd.Reading)
			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekCompleted ? cdvd.CurrentSector : cdvd.SeekToSector, cdvd.ReadMode);
	}

	return true;
}

void cdvdNewDiskCB()
{
	DoCDVDresetDiskTypeCache();
	cdvdDetectDisk();

	if (!VMManager::Internal::IsFastBootInProgress() && cdvd.Tray.trayState != CDVD_DISC_EJECT)
	{
		DevCon.WriteLn(Color_Green, "Ejecting media");
		cdvdUpdateStatus(CDVD_STATUS_TRAY_OPEN);
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvd.Tray.trayState = CDVD_DISC_EJECT;
		cdvd.Spinning = false;
		cdvdSetIrq(1 << Irq_Eject);
		if (cdvd.DiscType > 0)
			cdvd.Tray.cdvdActionSeconds = 3;
	}
	else if (cdvd.DiscType > 0)
	{
		DevCon.WriteLn(Color_Green, "Seeking new media");
		cdvdUpdateReady(CDVD_DRIVE_BUSY);
		cdvdUpdateStatus(CDVD_STATUS_SEEK);
		cdvd.Spinning = true;
		cdvd.Tray.trayState = CDVD_DISC_DETECTING;
		cdvd.Tray.cdvdActionSeconds = 3;
	}
}

static void mechaDecryptBytes(u32 madr, int size)
{
	int shiftAmount = (cdvd.decSet >> 4) & 7;
	const int doXor = (cdvd.decSet) & 1;
	const int doShift = (cdvd.decSet) & 2;

	u8* curval = iopPhysMem(madr);
	for (int i = 0; i < size; ++i, ++curval)
	{
		if (doXor)
			*curval ^= cdvd.Key[4];
		if (doShift)
			*curval = (*curval >> shiftAmount) | (*curval << (8 - shiftAmount));
	}
}

int cdvdReadSector()
{
	s32 bcr;

	CDVD_LOG("SECTOR %d (BCR %x;%x)", cdvd.CurrentSector, HW_DMA3_BCR_H16, HW_DMA3_BCR_L16);

	bcr = (HW_DMA3_BCR_H16 * HW_DMA3_BCR_L16) * 4;
	if (bcr < cdvd.BlockSize || !(HW_DMA3_CHCR & 0x01000000))
	{
		CDVD_LOG("READBLOCK:  bcr < cdvd.BlockSize; %x < %x", bcr, cdvd.BlockSize);
		if (HW_DMA3_CHCR & 0x01000000)
		{
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
		}
		return -1;
	}

	u8* mdest = iopPhysMem(HW_DMA3_MADR);

	if (cdvd.BlockSize == 2064)
	{
		u32 layer1Start;
		s32 dualType;
		s32 layerNum;
		u32 lsn = cdvd.CurrentSector;

		cdvdReadDvdDualInfo(&dualType, &layer1Start);

		if ((dualType == 1) && (lsn >= layer1Start))
		{
			layerNum = 1;
			lsn = lsn - layer1Start + 0x30000;
		}
		else if ((dualType == 2) && (lsn >= layer1Start))
		{
			layerNum = 1;
			lsn = ~(layer1Start + 0x30000 - 1);
		}
		else
		{
			layerNum = 0;
			lsn += 0x30000;
		}

		mdest[0] = 0x20 | layerNum;
		mdest[1] = static_cast<u8>(lsn >> 16);
		mdest[2] = static_cast<u8>(lsn >> 8);
		mdest[3] = static_cast<u8>(lsn);

		mdest[4] = 0;
		mdest[5] = 0;

		mdest[6] = 0;
		mdest[7] = 0;
		mdest[8] = 0;
		mdest[9] = 0;
		mdest[10] = 0;
		mdest[11] = 0;

		memcpy(&mdest[12], &cdr.Transfer[0], 2048);

		mdest[2060] = 0;
		mdest[2061] = 0;
		mdest[2062] = 0;
		mdest[2063] = 0;
	}
	else
	{
		memcpy(mdest, &cdr.Transfer[0], cdvd.BlockSize);
	}

	if (cdvd.decSet)
		mechaDecryptBytes(HW_DMA3_MADR, cdvd.BlockSize);

	psxCpu->Clear(HW_DMA3_MADR, cdvd.BlockSize / 4);

	HW_DMA3_BCR_H16 -= (cdvd.BlockSize / (HW_DMA3_BCR_L16 * 4));
	HW_DMA3_MADR += cdvd.BlockSize;

	if (!HW_DMA3_BCR_H16)
	{
		if (HW_DMA3_CHCR & 0x01000000)
		{
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
		}
	}

	return 0;
}

__fi void cdvdActionInterrupt()
{
	u8 ready_status = CDVD_DRIVE_READY;
	if (cdvd.AbortRequested)
	{
		Console.Warning("Action Abort %d", cdvd.Action);
		cdvd.Error = 0x1;
		ready_status |= CDVD_DRIVE_ERROR;
		cdvdUpdateReady(ready_status);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.WaitingDMA = false;
		CDVDCancelReadAhead();
		psxRegs.interrupt &= ~(1 << IopEvt_Cdvd);
	}

	switch (cdvd.Action)
	{
		case cdvdAction_Seek:
			cdvd.Spinning = true;
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = cdvd.SeekToSector;
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case cdvdAction_Standby:
			DevCon.Warning("CDVD Standby Call");
			cdvd.Spinning = true;
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = cdvd.SeekToSector;
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case cdvdAction_Stop:
			cdvd.Spinning = false;
			cdvdUpdateReady(ready_status);
			cdvd.CurrentSector = 0;
			cdvdUpdateStatus(CDVD_STATUS_STOP);
			break;

		default:
			cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			break;
	}
	
	cdvd.Action = cdvdAction_None;
	cdvdSetIrq();
}

__fi void cdvdSectorReady()
{
	if (cdvd.nextSectorsBuffered < 16)
	{
		cdvd.nextSectorsBuffered++;
		CDVD_LOG("Buffering sector");
	}

	if (cdvd.nextSectorsBuffered < 16)
		CDVDSECTORREADY_INT(cdvd.ReadTime);
	else if (!cdvd.Reading)
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
}

__fi void cdvdReadInterrupt()
{

	cdvdUpdateReady(CDVD_DRIVE_BUSY);
	cdvdUpdateStatus(CDVD_STATUS_READ);
	cdvd.WaitingDMA = false;

	if (!cdvd.SeekCompleted)
	{

		cdvd.Spinning = true;
		cdvd.CurrentRetryCnt = 0;
		cdvd.Reading = 1;
		cdvd.SeekCompleted = 1;
		cdvd.CurrentSector = cdvd.SeekToSector;
		CDVD_LOG("Cdvd Seek Complete at iopcycle=%8.8x.", psxRegs.cycle);
	}

	if (cdvd.AbortRequested)
	{
		{
			Console.Warning("Read Abort");
			cdvd.Error = 0x1;
			cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.WaitingDMA = false;
			CDVDCancelReadAhead();
			cdvdSetIrq();
			return;
		}
	}

	if (cdvd.CurrentSector >= cdvd.MaxSector)
	{
		DevCon.Warning("Read past end of disc Sector %d Max Sector %d", cdvd.CurrentSector, cdvd.MaxSector);
		cdvd.Error = 0x32;
		cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
		cdvdUpdateStatus(CDVD_STATUS_PAUSE);
		cdvd.WaitingDMA = false;
		cdvdSetIrq();
		return;
	}

	if (cdvd.Reading)
	{
		if (cdvd.ReadErr == 0)
		{
			while ((cdvd.ReadErr = DoCDVDgetBuffer(&cdr.Transfer[0])), cdvd.ReadErr == -2)
			{
				Threading::Sleep(0);
				Threading::SpinWait();
			}
		}

		if (cdvd.ReadErr == -1)
		{
			cdvd.CurrentRetryCnt++;

			if (cdvd.CurrentRetryCnt <= cdvd.RetryCntMax)
			{
				ERROR_LOG("CDVD read err, retrying... (attempt {} of {})", cdvd.CurrentRetryCnt, cdvd.RetryCntMax);
				cdvd.ReadErr = DoCDVDreadTrack(cdvd.CurrentSector, cdvd.ReadMode);
				CDVDREAD_INT(cdvd.ReadTime);
			}
			else
				ERROR_LOG("CDVD READ ERROR, sector = {}", cdvd.CurrentSector);

			return;
		}

		cdvd.Reading = false;

		pxAssert(cdvd.ReadErr == 0);
	}

	if (cdvd.SectorCnt > 0 && cdvd.nextSectorsBuffered)
	{
		if (cdvdReadSector() == -1)
		{
			pxAssert((int)cdvd.ReadTime > 0);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.WaitingDMA = true;
			return;
		}

		cdvd.nextSectorsBuffered--;
		CDVDSECTORREADY_INT(cdvd.ReadTime);

		cdvd.CurrentSector++;
		cdvd.SeekToSector++;

		if (--cdvd.SectorCnt <= 0)
		{
			cdvdSetIrq();
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.Reading = 0;
			if (cdvd.nextSectorsBuffered < 16)
				cdvdUpdateStatus(CDVD_STATUS_READ);
			else
				cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			return;
		}
	}
	else
	{
		if (cdvd.SectorCnt <= 0)
		{
			cdvdSetIrq();

			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			return;
		}
		if (cdvd.nextSectorsBuffered)
			CDVDREAD_INT((cdvd.BlockSize / 4) * 12);
		else
			CDVDREAD_INT(psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12));

		return;
	}

	cdvd.CurrentRetryCnt = 0;
	cdvd.Reading = 1;
	cdvd.ReadErr = DoCDVDreadTrack(cdvd.CurrentSector, cdvd.ReadMode);
	if (cdvd.nextSectorsBuffered)
		CDVDREAD_INT((cdvd.BlockSize / 4) * 12);
	else
		CDVDREAD_INT(psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12));
}

static uint cdvdStartSeek(uint newsector, CDVD_MODE_TYPE mode, bool transition_to_CLV)
{
	cdvd.SeekToSector = newsector;

	uint delta = abs(static_cast<s32>(cdvd.SeekToSector - cdvd.CurrentSector));
	uint seektime = 0;
	bool isSeeking = false;

	cdvdUpdateReady(CDVD_DRIVE_BUSY);
	cdvd.Reading = 1;
	cdvd.SeekCompleted = 0;
	int drive_speed_change_cycles = 0;
	const int old_rotspeed = cdvd.RotSpeed;
	cdvd.RotSpeed = cdvdRotationTime(mode);

	cdvd.ReadTime = cdvdBlockReadTime(mode);

	if (cdvd.Spinning && transition_to_CLV)
	{
		const float psx_clk_cycles = static_cast<float>(PSXCLK);
		const float old_rpm = (psx_clk_cycles / static_cast<float>(old_rotspeed)) * 60.0f;
		const float new_rpm = (psx_clk_cycles / static_cast<float>(cdvd.RotSpeed)) * 60.0f;
		drive_speed_change_cycles = (psx_clk_cycles / 1000.0f) * (0.054950495049505f * std::abs(new_rpm - old_rpm));
		CDVDCancelReadAhead();
	}
	cdvdUpdateStatus(CDVD_STATUS_SEEK);

	if (!cdvd.Spinning)
	{
		CDVD_LOG("CdSpinUp > Simulating CdRom Spinup Time, and seek to sector %d", cdvd.SeekToSector);
		seektime = PSXCLK / 3;
		cdvd.Spinning = true;
		cdvd.nextSectorsBuffered = 0;
		CDVDSECTORREADY_INT(seektime + cdvd.ReadTime);
	}
	else if ((tbl_ContigiousSeekDelta[mode] == 0) || (delta >= tbl_ContigiousSeekDelta[mode]))
	{
		CDVDCancelReadAhead();

		if (delta >= tbl_FastSeekDelta[mode])
		{
			CDVD_LOG("CdSeek Begin > to sector %d, from %d - delta=%d [FULL]", cdvd.SeekToSector, cdvd.CurrentSector, delta);
			seektime = Cdvd_FullSeek_Cycles;
		}
		else
		{
			CDVD_LOG("CdSeek Begin > to sector %d, from %d - delta=%d [FAST]", cdvd.SeekToSector, cdvd.CurrentSector, delta);
			seektime = Cdvd_FastSeek_Cycles;
		}
		isSeeking = true;
	}
	else if (!drive_speed_change_cycles)
	{
		CDVD_LOG("CdSeek Begin > Contiguous block without seek - delta=%d sectors", delta);

		
		isSeeking = false;

		if (cdvd.Action != cdvdAction_Seek)
		{
			if (delta == 0)
			{
				cdvdUpdateStatus(CDVD_STATUS_READ);
				cdvd.SeekCompleted = 1;
				cdvd.Reading = 1;
				cdvd.CurrentRetryCnt = 0;

				if (!cdvd.nextSectorsBuffered)
				{
					if (psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady))
					{
						seektime = psxRemainingCycles(IopEvt_CdvdSectorReady) + ((cdvd.BlockSize / 4) * 12);
					}
					else
					{
						delta = 1;
					}
				}
				else
					return (cdvd.BlockSize / 4) * 12;
			}
			else
			{
				if (delta >= cdvd.nextSectorsBuffered)
				{
					CDVDCancelReadAhead();
				}
				else
					cdvd.nextSectorsBuffered -= delta;
			}
		}
	}

	seektime += drive_speed_change_cycles;

	if ((delta || cdvd.Action == cdvdAction_Seek) && !isSeeking && !cdvd.nextSectorsBuffered)
	{
		const u32 rotationalLatency = cdvdRotationTime(static_cast<CDVD_MODE_TYPE>(cdvdIsDVD())) / 2;
		if (cdvd.Action == cdvdAction_Seek)
		{
			seektime += rotationalLatency;
			CDVDCancelReadAhead();
		}
		else
		{
			seektime += rotationalLatency + cdvd.ReadTime;
			CDVDSECTORREADY_INT(seektime);
			seektime += (cdvd.BlockSize / 4) * 12;
		}
	}
	else if (!isSeeking)
	{
		if (!(psxRegs.interrupt & (1 << IopEvt_CdvdSectorReady)))
		{
			seektime += cdvd.ReadTime;
			CDVDSECTORREADY_INT(seektime);
		}
		seektime += (cdvd.BlockSize / 4) * 12;
	}
	else
	{
		CDVDSECTORREADY_INT(seektime);
	}

	return seektime;
}

void cdvdUpdateTrayState()
{
	if (cdvd.Tray.cdvdActionSeconds > 0)
	{
		if (--cdvd.Tray.cdvdActionSeconds == 0)
		{
			switch (cdvd.Tray.trayState)
			{
				case CDVD_DISC_OPEN:
					cdvdCtrlTrayOpen();
					if (cdvd.DiscType > 0 || CDVDsys_GetSourceType() == CDVD_SourceType::NoDisc)
					{
						cdvd.Tray.cdvdActionSeconds = 3;
						cdvd.Tray.trayState = CDVD_DISC_EJECT;
						DevCon.WriteLn(Color_Green, "Simulating ejected media");
					}

				break;
				case CDVD_DISC_EJECT:
					cdvdCtrlTrayClose();
					break;
				case CDVD_DISC_DETECTING:
					DevCon.WriteLn(Color_Green, "Seeking new disc");
					cdvd.Tray.trayState = CDVD_DISC_SEEKING;
					cdvdUpdateStatus(CDVD_STATUS_SEEK);
					cdvd.Tray.cdvdActionSeconds = 2;
					break;
				case CDVD_DISC_SEEKING:
					cdvd.Spinning = true;
					[[fallthrough]];
				case CDVD_DISC_ENGAGED:
					cdvd.Tray.trayState = CDVD_DISC_ENGAGED;
					cdvdUpdateReady(CDVD_DRIVE_READY);
					cdvdUpdateStatus(CDVD_STATUS_PAUSE);
					if (CDVDsys_GetSourceType() != CDVD_SourceType::NoDisc)
					{
						DevCon.WriteLn(Color_Green, "Media ready to use");
					}
					break;
			}
		}
	}
}

void cdvdVsync()
{
	cdvd.RTCcount++;
	const double verticalFrequency = GetVerticalFrequency();
	if (cdvd.RTCcount < verticalFrequency)
		return;

	cdvd.RTCcount -= verticalFrequency;

	cdvdUpdateTrayState();

	sioNextFrame();

	cdvd.RTC.second++;
	if (cdvd.RTC.second < 60)
		return;
	cdvd.RTC.second = 0;

	cdvd.RTC.minute++;
	if (cdvd.RTC.minute < 60)
		return;
	cdvd.RTC.minute = 0;

	cdvd.RTC.hour++;
	if (cdvd.RTC.hour < 24)
		return;
	cdvd.RTC.hour = 0;

	cdvd.RTC.day++;
	if (cdvd.RTC.day <= (cdvd.RTC.month == 2 && cdvd.RTC.year % 4 == 0 ? 29 : monthmap[cdvd.RTC.month - 1]))
		return;
	cdvd.RTC.day = 1;

	cdvd.RTC.month++;
	if (cdvd.RTC.month <= 12)
		return;
	cdvd.RTC.month = 1;

	cdvd.RTC.year++;
	if (cdvd.RTC.year < 100)
		return;
	cdvd.RTC.year = 0;
}

static __fi u8 cdvdRead18(void)
{
	u8 ret = 0;

	if (((cdvd.sDataIn & 0x40) == 0) && (cdvd.SCMDResultPos < cdvd.SCMDResultCnt))
	{
		cdvd.SCMDResultPos++;
		if (cdvd.SCMDResultPos >= cdvd.SCMDResultCnt)
			cdvd.sDataIn |= 0x40;
		ret = cdvd.SCMDResultBuff[cdvd.SCMDResultPos - 1];
	}
	CDVD_LOG("cdvdRead18(SDataOut) %x (ResultC=%d, ResultP=%d)", ret, cdvd.SCMDResultCnt, cdvd.SCMDResultPos);

	return ret;
}

u8 cdvdRead(u8 key)
{
	switch (key)
	{
		case 0x04:
			CDVD_LOG("cdvdRead04(NCMD) %x", cdvd.nCommand);
			return cdvd.nCommand;

		case 0x05:
			CDVD_LOG("cdvdRead05(NReady) %x", cdvd.Ready);
			return cdvd.Ready;

		case 0x06:
		{
			CDVD_LOG("cdvdRead06(Error) %x", cdvd.Error);
			const u8 ret = cdvd.Error;
			cdvd.Error = 0;
			return ret;
		}
		case 0x07:
			CDVD_LOG("cdvdRead07(Break) %x", 0);
			return 0;

		case 0x08:
			CDVD_LOG("cdvdRead08(IntrReason) %x", cdvd.IntrStat);
			return cdvd.IntrStat;

		case 0x0A:
			CDVD_LOG("cdvdRead0A(Status) %x", cdvd.Status);
			return cdvd.Status;

		case 0x0B:
		{
			CDVD_LOG("cdvdRead0B(Status Sticky): %x", cdvd.StatusSticky);
			return cdvd.StatusSticky;
		}
		case 0x0C:
			CDVD_LOG("cdvdRead0C(Min) %x", itob((u8)(cdvd.CurrentSector / (60 * 75))));
			return itob((u8)(cdvd.CurrentSector / (60 * 75)));

		case 0x0D:
			CDVD_LOG("cdvdRead0D(Sec) %x", itob((u8)((cdvd.CurrentSector / 75) % 60) + 2));
			return itob((u8)((cdvd.CurrentSector / 75) % 60) + 2);

		case 0x0E:
			CDVD_LOG("cdvdRead0E(Frame) %x", itob((u8)(cdvd.CurrentSector % 75)));
			return itob((u8)(cdvd.CurrentSector % 75));

		case 0x0F:
			if (cdvd.Tray.trayState == CDVD_DISC_ENGAGED)
			{
				CDVD_LOG("cdvdRead0F(Disc Type) Engaged %x", cdvd.DiscType);
				return cdvd.DiscType;
			}
			else
			{
				CDVD_LOG("cdvdRead0F(Disc Type) Detecting %x", (cdvd.Tray.trayState <= CDVD_DISC_SEEKING) ? cdvdTrayStateDetecting() : 0);
				return (cdvd.Tray.trayState <= CDVD_DISC_SEEKING) ? cdvdTrayStateDetecting() : 0;
			}

		case 0x13:
		{
			u8 speedCtrl = cdvd.SpindlCtrl & 0x3F;

			if (speedCtrl == 0)
				speedCtrl = cdvdIsDVD() ? 3 : 5;

			if (cdvdIsDVD())
				speedCtrl += 0xF;
			else
				speedCtrl--;

			if (cdvd.Tray.trayState != CDVD_DISC_ENGAGED || cdvd.Spinning == false)
				speedCtrl = 0;

			CDVD_LOG("cdvdRead13(Speed) %x", speedCtrl);
			return speedCtrl;
		}


		case 0x15:
			CDVD_LOG("cdvdRead15(RSV)");
			return 0x0;

		case 0x16:
			CDVD_LOG("cdvdRead16(SCMD) %x", cdvd.sCommand);
			return cdvd.sCommand;

		case 0x17:
			CDVD_LOG("cdvdRead17(SReady) %x", cdvd.sDataIn);
			return cdvd.sDataIn;

		case 0x18:
			return cdvdRead18();

		case 0x20:
		case 0x21:
		case 0x22:
		case 0x23:
		case 0x24:
		{
			const int temp = key - 0x20;

			CDVD_LOG("cdvdRead%d(Key%d) %x", key, temp, cdvd.Key[temp]);
			return cdvd.Key[temp];
		}
		case 0x28:
		case 0x29:
		case 0x2A:
		case 0x2B:
		case 0x2C:
		{
			const int temp = key - 0x23;

			CDVD_LOG("cdvdRead%d(Key%d) %x", key, temp, cdvd.Key[temp]);
			return cdvd.Key[temp];
		}

		case 0x30:
		case 0x31:
		case 0x32:
		case 0x33:
		case 0x34:
		{
			const int temp = key - 0x26;

			CDVD_LOG("cdvdRead%d(Key%d) %x", key, temp, cdvd.Key[temp]);
			return cdvd.Key[temp];
		}

		case 0x38:
			CDVD_LOG("cdvdRead38(KeysValid) %x", cdvd.Key[15]);

			return cdvd.Key[15];

		case 0x39:
			CDVD_LOG("cdvdRead39(KeyXor) %x", cdvd.KeyXor);

			return cdvd.KeyXor;

		case 0x3A:
			CDVD_LOG("cdvdRead3A(DecSet) %x", cdvd.decSet);

			return cdvd.decSet;

		default:
			PSXHW_LOG("*Unknown 8bit read at address 0x1f4020%x", key);
			Console.Error("IOP Unknown 8bit read from addr 0x1f4020%x", key);
			return -1;
	}
}

static bool cdvdReadErrorHandler()
{
	if (cdvd.SectorCnt <= 0)
	{
		DevCon.Warning("Bad Sector Count Error");
		cdvd.Error = 0x21;
		return false;
	}

	if (cdvd.SeekToSector >= cdvd.MaxSector)
	{
		DevCon.Warning("Error reading past end of disc");
		cdvd.Error = 0x30;
		return false;
	}

	return true;
}

static bool cdvdCommandErrorHandler()
{
	if (cdvd.nCommand > N_CD_NOP)
	{
		if ((cdvd.Status & CDVD_STATUS_TRAY_OPEN) || (cdvd.DiscType == CDVD_TYPE_NODISC))
		{
			cdvd.Error = (cdvd.DiscType == CDVD_TYPE_NODISC) ? 0x12 : 0x11;
			cdvd.Ready |= CDVD_DRIVE_ERROR;
			cdvdSetIrq();
			return false;
		}
	}

	if (cdvd.NCMDParamCnt != cdvdParamLength[cdvd.nCommand] && cdvdParamLength[cdvd.nCommand] != 255)
	{
		DevCon.Warning("CDVD: Error in command parameter length, expecting %d got %d", cdvdParamLength[cdvd.nCommand], cdvd.NCMDParamCnt);
		cdvd.Error = 0x22;
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		return false;
	}

	if (cdvd.nCommand > N_CD_CHG_SPDL_CTRL)
	{
		DevCon.Warning("CDVD: Error invalid NCMD");
		cdvd.Error = 0x10;
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		return false;
	}

	return true;
}

static void cdvdWrite04(u8 rt)
{
	CDVD_LOG("cdvdWrite04: NCMD %s (%x) (ParamP = %x)", nCmdName[rt], rt, cdvd.NCMDParamPos);

	if (!(cdvd.Ready & CDVD_DRIVE_READY))
	{
		DevCon.Warning("CDVD: Error drive not ready on command issue");
		cdvd.Error = 0x13;
		cdvd.Ready |= CDVD_DRIVE_ERROR;
		cdvdSetIrq();
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
		return;
	}

	cdvd.nCommand = rt;
	cdvd.AbortRequested = false;

	if (!cdvdCommandErrorHandler())
	{
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
		return;
	}

	switch (rt)
	{
		case N_CD_NOP:
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdSetIrq();
			break;
		case N_CD_RESET:
			Console.WriteLn("CDVD: Reset NCommand");
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.SCMDParamPos = 0;
			cdvd.SCMDParamCnt = 0;
			cdvdUpdateStatus(CDVD_STATUS_STOP);
			cdvd.Spinning = false;
			std::memset(&cdvd.SCMDResultBuff[0], 0, sizeof(cdvd.SCMDResultBuff));
			cdvdSetIrq();
			break;

		case N_CD_STANDBY:

			DevCon.Warning("CdStandby : %d", rt);
			CDVD_INT(cdvdStartSeek(0, static_cast<CDVD_MODE_TYPE>(cdvdIsDVD()), false));
			cdvdUpdateStatus(CDVD_STATUS_SEEK);
			cdvd.Action = cdvdAction_Standby;
			break;

		case N_CD_STOP:
			DevCon.Warning("CdStop : %d", rt);
			cdvdUpdateReady(CDVD_DRIVE_BUSY);
			CDVDCancelReadAhead();
			cdvdUpdateStatus(CDVD_STATUS_SPIN);
			CDVD_INT(PSXCLK / 6);
			cdvd.Action = cdvdAction_Stop;
			break;

		case N_CD_PAUSE:
			psxRegs.interrupt &= ~(1 << IopEvt_Cdvd);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdSetIrq();
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case N_CD_SEEK:
			cdvd.Action = cdvdAction_Seek;
			CDVD_INT(cdvdStartSeek(GetBufferU32(&cdvd.NCMDParamBuff[0], 0), static_cast<CDVD_MODE_TYPE>(cdvdIsDVD()), false));
			cdvdUpdateStatus(CDVD_STATUS_SEEK);
			break;

		case N_CD_READ:
		{
			cdvd.SeekToSector = GetBufferU32(&cdvd.NCMDParamBuff[0], 0);
			cdvd.SectorCnt = GetBufferU32(&cdvd.NCMDParamBuff[0], 4);
			cdvd.RetryCntMax = (cdvd.NCMDParamBuff[8] == 0) ? 0x100 : cdvd.NCMDParamBuff[8];
			const u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | (cdvdIsDVD() ? 3 : 5);

			if (cdvd.NCMDParamBuff[9] & CDVD_SPINDLE_NOMINAL)
				DevCon.Warning("CDVD: CD Read using Nominal switch from CAV to CLV, unhandled");

			bool ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1:
					cdvd.Speed = 1;
					break;
				case 2:
					cdvd.Speed = 2;
					break;
				case 3:
					cdvd.Speed = 4;
					break;
				case 4:
					if (cdvdIsDVD())
					{
						DevCon.Warning("CDVD Read invalid DVD Speed %d", cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED);
						ParamError = true;
					}
					else
						cdvd.Speed = 12;
					break;
				case 5:
					if (cdvdIsDVD())
					{
						DevCon.Warning("CDVD Read invalid DVD Speed %d", cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED);
						ParamError = true;
					}
					else
						cdvd.Speed = 24;
					break;
				default:
					Console.Error("Unknown CDVD Read Speed SpindleCtrl=%x", cdvd.SpindlCtrl);
					ParamError = true;
					break;
			}

			if (cdvdIsDVD() && cdvd.NCMDParamBuff[10] != 0)
			{
				ParamError = true;
			}
			else
			{
				switch (cdvd.NCMDParamBuff[10])
				{
					case 2:
						cdvd.ReadMode = CDVD_MODE_2340;
						cdvd.BlockSize = 2340;
						break;
					case 1:
						cdvd.ReadMode = CDVD_MODE_2328;
						cdvd.BlockSize = 2328;
						break;
					case 0:
						cdvd.ReadMode = CDVD_MODE_2048;
						cdvd.BlockSize = 2048;
						break;
					default:
						ParamError = true;
						break;
				}
			}

			if (ParamError)
			{
				DevCon.Warning("CDVD: CD Read Bad Parameter Error");
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22;
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			if (!cdvdReadErrorHandler())
			{
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvdRotationTime(static_cast<CDVD_MODE_TYPE>(cdvdIsDVD())));
				break;
			}

			CDVD_LOG("CDRead > startSector=%d, seekTo=%d nSectors=%d, RetryCnt=%x, Speed=%dx(%s), ReadMode=%x(%x) SpindleCtrl=%x",
				cdvd.CurrentSector, cdvd.SeekToSector, cdvd.SectorCnt, cdvd.RetryCntMax, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.ReadMode, cdvd.NCMDParamBuff[10], cdvd.SpindlCtrl);

			if (EmuConfig.CdvdVerboseReads)
				Console.WriteLn(Color_Gray, "CDRead: Reading Sector %07d (%03d Blocks of Size %d) at Speed=%dx(%s) Spindle=%x",
					cdvd.SeekToSector, cdvd.SectorCnt, cdvd.BlockSize, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.SpindlCtrl);

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, static_cast<CDVD_MODE_TYPE>(cdvdIsDVD()), !(cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) && (oldSpindleCtrl & CDVD_SPINDLE_CAV)));

			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			cdvd.Reading = 1;
			break;
		}
		case N_CD_READ_CDDA:
		case N_CD_READ_XCDDA:
		{
			if (cdvdIsDVD())
			{
				DevCon.Warning("CDVD: DVD Read when CD Error");
				cdvd.Error = 0x14;
				cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
				cdvdSetIrq();
				return;
			}
			cdvd.SeekToSector = GetBufferU32(&cdvd.NCMDParamBuff[0], 0);
			cdvd.SectorCnt = GetBufferU32(&cdvd.NCMDParamBuff[0], 4);
			cdvd.RetryCntMax = (cdvd.NCMDParamBuff[8] == 0) ? 0x100 : cdvd.NCMDParamBuff[8];

			const u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | 5;

			if (cdvd.NCMDParamBuff[9] & CDVD_SPINDLE_NOMINAL)
				DevCon.Warning("CDVD: CDDA Read using Nominal switch from CAV to CLV, unhandled");

			bool ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1:
					cdvd.Speed = 1;
					break;
				case 2:
					cdvd.Speed = 2;
					break;
				case 3:
					cdvd.Speed = 4;
					break;
				case 4:
					cdvd.Speed = 12;
					break;
				case 5:
					cdvd.Speed = 24;
					break;
				default:
					Console.Error("Unknown CDVD Read Speed SpindleCtrl=%x", cdvd.SpindlCtrl);
					ParamError = true;
					break;
			}

			switch (cdvd.NCMDParamBuff[10])
			{
				case 1:
					cdvd.ReadMode = CDVD_MODE_2368;
					cdvd.BlockSize = 2368;
					break;
				case 0:
					cdvd.ReadMode = CDVD_MODE_2352;
					cdvd.BlockSize = 2352;
					break;
				default:
					ParamError = true;
					break;
			}

			if (ParamError)
			{
				DevCon.Warning("CDVD: CDDA Read Bad Parameter Error");
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22;
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			CDVD_LOG("CDRead > startSector=%d, seekTo=%d, nSectors=%d, RetryCnt=%x, Speed=%dx(%s), ReadMode=%x(%x) SpindleCtrl=%x",
				cdvd.CurrentSector, cdvd.SeekToSector, cdvd.SectorCnt, cdvd.RetryCntMax, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.ReadMode, cdvd.NCMDParamBuff[10], cdvd.SpindlCtrl);

			if (EmuConfig.CdvdVerboseReads)
				Console.WriteLn(Color_Gray, "CdAudioRead: Reading Sector %07d (%03d Blocks of Size %d) at Speed=%dx(%s) Spindle=%x",
					cdvd.CurrentSector, cdvd.SectorCnt, cdvd.BlockSize, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.SpindlCtrl);

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, MODE_CDROM, !(cdvd.SpindlCtrl& CDVD_SPINDLE_CAV) && (oldSpindleCtrl& CDVD_SPINDLE_CAV)));

			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			cdvd.Reading = 1;
			break;
		}
		case N_DVD_READ:
		{
			if (!cdvdIsDVD())
			{
				DevCon.Warning("CDVD: DVD Read when CD Error");
				cdvd.Error = 0x14;
				cdvdUpdateReady(CDVD_DRIVE_READY | CDVD_DRIVE_ERROR);
				cdvdSetIrq();
				return;
			}
			cdvd.SeekToSector = GetBufferU32(&cdvd.NCMDParamBuff[0], 0);
			cdvd.SectorCnt = GetBufferU32(&cdvd.NCMDParamBuff[0], 4);

			const u32 oldSpindleCtrl = cdvd.SpindlCtrl;

			if (cdvd.NCMDParamBuff[8] == 0)
				cdvd.RetryCntMax = 0x100;
			else
				cdvd.RetryCntMax = cdvd.NCMDParamBuff[8];

			if (cdvd.NCMDParamBuff[9] & 0x3F)
				cdvd.SpindlCtrl = cdvd.NCMDParamBuff[9];
			else
				cdvd.SpindlCtrl = (cdvd.NCMDParamBuff[9] & 0x80) | 3;

			if (cdvd.NCMDParamBuff[9] & CDVD_SPINDLE_NOMINAL)
				DevCon.Warning("CDVD: DVD Read using Nominal switch from CAV to CLV, unhandled");

			bool ParamError = false;

			switch (cdvd.SpindlCtrl & CDVD_SPINDLE_SPEED)
			{
				case 1:
					cdvd.Speed = 1;
					break;
				case 2:
					cdvd.Speed = 2;
					break;
				case 3:
					cdvd.Speed = 4;
					break;
				default:
					Console.Error("Unknown CDVD Read Speed SpindleCtrl=%x", cdvd.SpindlCtrl);
					ParamError = true;
					break;
			}

			if (cdvd.NCMDParamBuff[10] != 0)
				ParamError = true;

			cdvd.ReadMode = CDVD_MODE_2048;
			cdvd.BlockSize = 2064;

			if (ParamError)
			{
				DevCon.Warning("CDVD: DVD Read Bad Parameter Error");
				cdvd.SpindlCtrl = oldSpindleCtrl;
				cdvd.Error = 0x22;
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvd.BlockSize * 12);
				break;
			}

			if (!cdvdReadErrorHandler())
			{
				cdvd.Action = cdvdAction_Error;
				cdvdUpdateStatus(CDVD_STATUS_SEEK);
				cdvdUpdateReady(CDVD_DRIVE_BUSY);
				CDVD_INT(cdvdRotationTime(static_cast<CDVD_MODE_TYPE>(cdvdIsDVD())));
				break;
			}

			CDVD_LOG("DvdRead > startSector=%d, seekTo=%d nSectors=%d, RetryCnt=%x, Speed=%dx(%s), ReadMode=%x(%x) SpindleCtrl=%x",
				cdvd.CurrentSector, cdvd.SeekToSector, cdvd.SectorCnt, cdvd.RetryCntMax, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.ReadMode, cdvd.NCMDParamBuff[10], cdvd.SpindlCtrl);

			if (EmuConfig.CdvdVerboseReads)
				Console.WriteLn(Color_Gray, "DvdRead: Reading Sector %07d (%03d Blocks of Size %d) at Speed=%dx(%s) SpindleCtrl=%x",
					cdvd.SeekToSector, cdvd.SectorCnt, cdvd.BlockSize, cdvd.Speed, (cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) ? "CAV" : "CLV", cdvd.SpindlCtrl);

			CDVDREAD_INT(cdvdStartSeek(cdvd.SeekToSector, MODE_DVDROM, !(cdvd.SpindlCtrl & CDVD_SPINDLE_CAV) && (oldSpindleCtrl& CDVD_SPINDLE_CAV)));

			cdvd.ReadErr = DoCDVDreadTrack(cdvd.SeekToSector, cdvd.ReadMode);

			cdvd.Reading = 1;
			break;
		}
		case N_CD_GET_TOC:
			DevCon.WriteLn("CDGetToc Param[0]=%d, Param[1]=%d", cdvd.NCMDParamBuff[0], cdvd.NCMDParamBuff[1]);
			cdvdGetToc(iopPhysMem(HW_DMA3_MADR));
			cdvdSetIrq();
			HW_DMA3_CHCR &= ~0x01000000;
			psxDmaInterrupt(3);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
			break;

		case N_CD_READ_KEY:
		{
			const u8 arg0 = cdvd.NCMDParamBuff[0];
			const u16 arg1 = cdvd.NCMDParamBuff[1] | (cdvd.NCMDParamBuff[2] << 8);
			const u32 arg2 = cdvd.NCMDParamBuff[3] | (cdvd.NCMDParamBuff[4] << 8) | (cdvd.NCMDParamBuff[5] << 16) | (cdvd.NCMDParamBuff[6] << 24);
			DevCon.WriteLn("cdvdReadKey(%d, %d, %d)", arg0, arg1, arg2);
			cdvdReadKey(arg0, arg1, arg2, &cdvd.Key[0]);
			cdvd.KeyXor = 0x00;
			cdvdSetIrq();
			cdvdUpdateStatus(CDVD_STATUS_PAUSE);
			cdvdUpdateReady(CDVD_DRIVE_READY);
			cdvd.nextSectorsBuffered = 0;
			CDVDSECTORREADY_INT(cdvd.ReadTime);
		}
		break;

		case N_CD_CHG_SPDL_CTRL:
			Console.WriteLn("sceCdChgSpdlCtrl(%d)", cdvd.NCMDParamBuff[0]);
			cdvdSetIrq();
			break;

		default:
			Console.Warning("NCMD Unknown %x", rt);
			cdvdSetIrq();
			break;
	}
	cdvd.NCMDParamPos = 0;
	cdvd.NCMDParamCnt = 0;
}

static __fi void cdvdWrite05(u8 rt)
{
	CDVD_LOG("cdvdWrite05(NDataIn) %x", rt);

	if (cdvd.NCMDParamPos >= 16)
	{
		DevCon.Warning("CDVD: NCMD Overflow");
		cdvd.NCMDParamPos = 0;
		cdvd.NCMDParamCnt = 0;
	}

	cdvd.NCMDParamBuff[cdvd.NCMDParamPos++] = rt;
	cdvd.NCMDParamCnt++;
}

static __fi void cdvdWrite06(u8 rt)
{
	CDVD_LOG("cdvdWrite06(HowTo) %x", rt);
	cdvd.HowTo = rt;
}

static __fi void cdvdWrite07(u8 rt)
{
	CDVD_LOG("cdvdWrite07(Break) %x", rt);

	if (!(cdvd.Ready & CDVD_DRIVE_BUSY) || cdvd.AbortRequested)
		return;

	DbgCon.WriteLn("*PCSX2*: CDVD BREAK %x", rt);

	cdvd.AbortRequested = true;
}

static __fi void cdvdWrite08(u8 rt)
{
	CDVD_LOG("cdvdWrite08(IntrReason) = ACK(%x)", rt);
	cdvd.IntrStat &= ~rt;
}

static __fi void cdvdWrite0A(u8 rt)
{
	CDVD_LOG("cdvdWrite0A(Status) %x", rt);
}

static __fi void cdvdWrite0F(u8 rt)
{
	CDVD_LOG("cdvdWrite0F(Type) %x", rt);
	DevCon.WriteLn("*PCSX2*: CDVD TYPE %x", rt);
}

static __fi void cdvdWrite14(u8 rt)
{
	if (rt == 0xFE)
		Console.Warning("*PCSX2*: Unimplemented PS1 mode DISC SPEED = FAST");
	else
		Console.Warning("*PCSX2*: Unimplemented PS1 mode DISC SPEED = STANDARD");
}

static __fi void fail_pol_cal()
{
	Console.Error("[MG] ERROR - Make sure the file is already decrypted!!!");
	cdvd.SCMDResultBuff[0] = 0x80;
}

static void cdvdWrite16(u8 rt)
{
	{
		int address;
		u8 tmp;

		CDVD_LOG("cdvdWrite16: SCMD %s (%x) (ParamP = %x)", sCmdName[rt], rt, cdvd.SCMDParamPos);

		cdvd.sCommand = rt;
		std::memset(&cdvd.SCMDResultBuff[0], 0, sizeof(cdvd.SCMDResultBuff));

		switch (rt)
		{

			case 0x02:
				SetSCMDResultSize(11);
				cdvd.SCMDResultBuff[0] = cdvdReadSubQ(cdvd.CurrentSector, (cdvdSubQ*)&cdvd.SCMDResultBuff[1]);
				break;

			case 0x03:
				switch (cdvd.SCMDParamBuff[0])
				{
					case 0x00:
						SetSCMDResultSize(4);
						std::memcpy(&cdvd.SCMDResultBuff[0], &s_mecha_version, sizeof(u32));
						break;
					case 0x30:
						SetSCMDResultSize(2);
						cdvd.SCMDResultBuff[0] = cdvd.Status;
						cdvd.SCMDResultBuff[1] = (cdvd.Status & 0x1) ? 8 : 0;
						break;
					case 0x44:
						SetSCMDResultSize(1);
						cdvdWriteConsoleID(&cdvd.SCMDParamBuff[1]);
						break;

					case 0x45:
						SetSCMDResultSize(9);
						cdvdReadConsoleID(&cdvd.SCMDResultBuff[1]);
						break;

					case 0xFD:
						SetSCMDResultSize(6);
						cdvd.SCMDResultBuff[0] = 0;
						cdvd.SCMDResultBuff[1] = 0x04;
						cdvd.SCMDResultBuff[2] = 0x12;
						cdvd.SCMDResultBuff[3] = 0x10;
						cdvd.SCMDResultBuff[4] = 0x01;
						cdvd.SCMDResultBuff[5] = 0x30;
						break;

					case 0xEF:
						SetSCMDResultSize(3);
						cdvd.SCMDResultBuff[0] = 0;
						cdvd.SCMDResultBuff[1] = 0x0F;
						cdvd.SCMDResultBuff[2] = 0x05;
						break;

					default:
						SetSCMDResultSize(1);
						cdvd.SCMDResultBuff[0] = 0x81;
						Console.Warning("*Unknown Mecacon Command param Test2 subparams - param[0]=%02X", cdvd.SCMDParamBuff[0]);
						break;
				}
				break;

			case 0x05:
				cdvd.StatusSticky = cdvd.Status & CDVD_STATUS_TRAY_OPEN;

				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x06:
				SetSCMDResultSize(1);
				if (cdvd.SCMDParamBuff[0] == 0)
					cdvd.SCMDResultBuff[0] = cdvdCtrlTrayOpen();
				else
					cdvd.SCMDResultBuff[0] = cdvdCtrlTrayClose();
				break;

			case 0x08:
				SetSCMDResultSize(8);
				cdvd.SCMDResultBuff[0] = 0;
				cdvd.SCMDResultBuff[1] = itob(cdvd.RTC.second);
				cdvd.SCMDResultBuff[2] = itob(cdvd.RTC.minute);
				cdvd.SCMDResultBuff[3] = itob(cdvd.RTC.hour);
				cdvd.SCMDResultBuff[4] = 0;
				cdvd.SCMDResultBuff[5] = itob(cdvd.RTC.day);
				cdvd.SCMDResultBuff[6] = itob(cdvd.RTC.month);
				cdvd.SCMDResultBuff[7] = itob(cdvd.RTC.year);
				break;

			case 0x09:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				cdvd.RTC.pad = 0;

				cdvd.RTC.second = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 7]);
				cdvd.RTC.minute = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 6]) % 60;
				cdvd.RTC.hour = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 5]) % 24;
				cdvd.RTC.day = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 3]);
				cdvd.RTC.month = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 2] & 0x7f);
				cdvd.RTC.year = btoi(cdvd.SCMDParamBuff[cdvd.SCMDParamPos - 1]);
				break;

			case 0x0A:
				address = (cdvd.SCMDParamBuff[0] << 8) | cdvd.SCMDParamBuff[1];

				if (address < 512)
				{
					SetSCMDResultSize(3);
					cdvdReadNVM(&cdvd.SCMDResultBuff[1], address * 2, 2);
					tmp = cdvd.SCMDResultBuff[1];
					cdvd.SCMDResultBuff[1] = cdvd.SCMDResultBuff[2];
					cdvd.SCMDResultBuff[2] = tmp;
				}
				else
				{
					SetSCMDResultSize(1);
					cdvd.SCMDResultBuff[0] = 0xff;
				}
				break;

			case 0x0B:
				SetSCMDResultSize(1);
				address = (cdvd.SCMDParamBuff[0] << 8) | cdvd.SCMDParamBuff[1];

				if (address < 512)
				{
					tmp = cdvd.SCMDParamBuff[2];
					cdvd.SCMDParamBuff[2] = cdvd.SCMDParamBuff[3];
					cdvd.SCMDParamBuff[3] = tmp;
					cdvdWriteNVM(&cdvd.SCMDParamBuff[2], address * 2, 2);
				}
				else
				{
					cdvd.SCMDResultBuff[0] = 0xff;
				}
				break;

			case 0x0F:
				Console.WriteLn(Color_StrongBlack, "sceCdPowerOff called. Shutting down VM.");
				Host::RequestVMShutdown(false, false, false);
				break;

			case 0x12:
				SetSCMDResultSize(9);
				cdvdReadILinkID(&cdvd.SCMDResultBuff[1]);
				if ((!cdvd.SCMDResultBuff[3]) && (!cdvd.SCMDResultBuff[4]))
				{
					cdvd.SCMDResultBuff[0] = 0x00;
					cdvd.SCMDResultBuff[1] = 0x00;
					cdvd.SCMDResultBuff[2] = 0xAC;
					cdvd.SCMDResultBuff[3] = 0xFF;
					cdvd.SCMDResultBuff[4] = 0xFF;
					cdvd.SCMDResultBuff[5] = 0xFF;
					cdvd.SCMDResultBuff[6] = 0xFF;
					cdvd.SCMDResultBuff[7] = 0xB9;
					cdvd.SCMDResultBuff[8] = 0x86;
				}
				break;

			case 0x13:
				SetSCMDResultSize(1);
				cdvdWriteILinkID(&cdvd.SCMDParamBuff[1]);
				break;

			case 0x14:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x15:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 5;
				break;

			case 0x16:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x17:
				SetSCMDResultSize(9);
				cdvdReadModelNumber(&cdvd.SCMDResultBuff[1], cdvd.SCMDParamBuff[0]);
				break;

			case 0x18:
				SetSCMDResultSize(1);
				cdvdWriteModelNumber(&cdvd.SCMDParamBuff[1], cdvd.SCMDParamBuff[0]);
				break;

			case 0x1A:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 1;
				break;

			case 0x1B:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x1C:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x1E:
				SetSCMDResultSize(5);
				cdvd.SCMDResultBuff[0] = 0x00;
				cdvd.SCMDResultBuff[1] = 0x14;
				cdvd.SCMDResultBuff[2] = 0x00;
				cdvd.SCMDResultBuff[3] = 0x00;
				cdvd.SCMDResultBuff[4] = 0x00;
				break;

			case 0x20:
				SetSCMDResultSize(3);
				cdvd.SCMDResultBuff[0] = 0x00;
				cdvd.SCMDResultBuff[1] = 0x01;
				cdvd.SCMDResultBuff[2] = 0x00;
				break;

			case 0x22:
				SetSCMDResultSize(10);
				cdvd.SCMDResultBuff[0] = 0;
				cdvd.SCMDResultBuff[1] = 0;
				cdvd.SCMDResultBuff[2] = 0;
				cdvd.SCMDResultBuff[3] = 0;
				cdvd.SCMDResultBuff[4] = 0;
				cdvd.SCMDResultBuff[5] = 0;
				cdvd.SCMDResultBuff[6] = 0;
				cdvd.SCMDResultBuff[7] = 0;
				cdvd.SCMDResultBuff[8] = 0;
				cdvd.SCMDResultBuff[9] = 0;
				break;

			case 0x24:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x27:
				{
					SetSCMDResultSize(13);

					const std::string DiscSerial = VMManager::GetDiscSerial();
					cdvd.SCMDResultBuff[0] = 0;
					cdvd.SCMDResultBuff[1] = DiscSerial[0];
					cdvd.SCMDResultBuff[2] = DiscSerial[1];
					cdvd.SCMDResultBuff[3] = DiscSerial[2];
					cdvd.SCMDResultBuff[4] = DiscSerial[3];
					cdvd.SCMDResultBuff[5] = DiscSerial[4];
					cdvd.SCMDResultBuff[6] = DiscSerial[5];
					cdvd.SCMDResultBuff[7] = DiscSerial[6];
					cdvd.SCMDResultBuff[8] = DiscSerial[7];
					cdvd.SCMDResultBuff[9] = DiscSerial[9];
					cdvd.SCMDResultBuff[10] = DiscSerial[10];
					cdvd.SCMDResultBuff[11] = DiscSerial[11];
					cdvd.SCMDResultBuff[12] = DiscSerial[12];
				}
				break;

			case 0x29:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x31:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x32:
				SetSCMDResultSize(2);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x36:
				SetSCMDResultSize(15);

				std::memcpy(&cdvd.SCMDResultBuff[1], &s_mecha_version, sizeof(u32));
				cdvdReadRegionParams(&cdvd.SCMDResultBuff[3]);
				DevCon.WriteLn("REGION PARAMS = %s %s", mg_zones[cdvd.SCMDResultBuff[1] & 7], &cdvd.SCMDResultBuff[3]);
				cdvd.SCMDResultBuff[1] = 1 << cdvd.SCMDResultBuff[1];
				cdvd.SCMDResultBuff[2] = 0;
				cdvd.SCMDResultBuff[11] = 0;
				cdvd.SCMDResultBuff[12] = 0;
				cdvd.SCMDResultBuff[13] = 0;
				cdvd.SCMDResultBuff[14] = 0;
				break;

			case 0x37:
				SetSCMDResultSize(9);
				cdvdReadMAC(&cdvd.SCMDResultBuff[1]);
				break;

			case 0x38:
				SetSCMDResultSize(1);
				cdvdWriteMAC(&cdvd.SCMDParamBuff[0]);
				break;

			case 0x3E:
				SetSCMDResultSize(1);
				cdvdWriteRegionParams(&cdvd.SCMDParamBuff[2]);
				break;

			case 0x40:
				SetSCMDResultSize(1);
				cdvd.CReadWrite = cdvd.SCMDParamBuff[0];
				cdvd.COffset = cdvd.SCMDParamBuff[1];
				cdvd.CNumBlocks = cdvd.SCMDParamBuff[2];
				cdvd.CBlockIndex = 0;
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x41:
				SetSCMDResultSize(16);
				cdvdReadConfig(&cdvd.SCMDResultBuff[0]);
				break;

			case 0x42:
				SetSCMDResultSize(1);
				cdvdWriteConfig(&cdvd.SCMDParamBuff[0]);
				break;

			case 0x43:
				SetSCMDResultSize(1);
				cdvd.CReadWrite = 0;
				cdvd.COffset = 0;
				cdvd.CNumBlocks = 0;
				cdvd.CBlockIndex = 0;
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x80:
				SetSCMDResultSize(1);
				cdvd.mg_datatype = 0;
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x81:
				SetSCMDResultSize(1);
				cdvd.mg_datatype = 0;
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x82:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x83:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x84:
				SetSCMDResultSize(1 + 8 + 4);
				cdvd.SCMDResultBuff[0] = 0;

				cdvd.SCMDResultBuff[1] = 0x21;
				cdvd.SCMDResultBuff[2] = 0xdc;
				cdvd.SCMDResultBuff[3] = 0x31;
				cdvd.SCMDResultBuff[4] = 0x96;
				cdvd.SCMDResultBuff[5] = 0xce;
				cdvd.SCMDResultBuff[6] = 0x72;
				cdvd.SCMDResultBuff[7] = 0xe0;
				cdvd.SCMDResultBuff[8] = 0xc8;

				cdvd.SCMDResultBuff[9] = 0x69;
				cdvd.SCMDResultBuff[10] = 0xda;
				cdvd.SCMDResultBuff[11] = 0x34;
				cdvd.SCMDResultBuff[12] = 0x9b;
				break;

			case 0x85:
				SetSCMDResultSize(1 + 4 + 8);
				cdvd.SCMDResultBuff[0] = 0;

				cdvd.SCMDResultBuff[1] = 0xeb;
				cdvd.SCMDResultBuff[2] = 0x01;
				cdvd.SCMDResultBuff[3] = 0xc7;
				cdvd.SCMDResultBuff[4] = 0xa9;

				cdvd.SCMDResultBuff[5] = 0x3f;
				cdvd.SCMDResultBuff[6] = 0x9c;
				cdvd.SCMDResultBuff[7] = 0x5b;
				cdvd.SCMDResultBuff[8] = 0x19;
				cdvd.SCMDResultBuff[9] = 0x31;
				cdvd.SCMDResultBuff[10] = 0xa0;
				cdvd.SCMDResultBuff[11] = 0xb3;
				cdvd.SCMDResultBuff[12] = 0xa3;
				break;

			case 0x86:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x87:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x8D:
				SetSCMDResultSize(1);
				if (cdvd.mg_size + cdvd.SCMDParamCnt > cdvd.mg_maxsize)
				{
					cdvd.SCMDResultBuff[0] = 0x80;
				}
				else
				{
					memcpy(&cdvd.mg_buffer[cdvd.mg_size], cdvd.SCMDParamBuff, cdvd.SCMDParamCnt);
					cdvd.mg_size += cdvd.SCMDParamCnt;
					cdvd.SCMDResultBuff[0] = 0;
				}
				break;

			case 0x8E:
				SetSCMDResultSize(std::min(16, cdvd.mg_size));
				memcpy(&cdvd.SCMDResultBuff[0], &cdvd.mg_buffer[0], cdvd.SCMDResultCnt);
				cdvd.mg_size -= cdvd.SCMDResultCnt;
				memcpy(&cdvd.mg_buffer[0], &cdvd.mg_buffer[cdvd.SCMDResultCnt], cdvd.mg_size);
				break;

			case 0x88:
			case 0x8F:
				SetSCMDResultSize(1);
				if (cdvd.mg_datatype == 1)
				{
					int bit_ofs = 0;

					if ((cdvd.mg_maxsize != cdvd.mg_size) || (cdvd.mg_size < 0x20) || (cdvd.mg_size != GetBufferU16(&cdvd.mg_buffer[0], 0x14)))
					{
						fail_pol_cal();
						break;
					}

					std::string zoneStr;
					for (int i = 0; i < 8; i++)
					{
						if (cdvd.mg_buffer[0x1C] & (1 << i))
							zoneStr += mg_zones[i];
					}

					Console.WriteLn("[MG] ELF_size=0x%X Hdr_size=0x%X unk=0x%X flags=0x%X count=%d zones=%s",
						*(u32*)&cdvd.mg_buffer[0x10], *(u16*)&cdvd.mg_buffer[0x14], *(u16*)&cdvd.mg_buffer[0x16],
						*(u16*)&cdvd.mg_buffer[0x18], *(u16*)&cdvd.mg_buffer[0x1A],
						zoneStr.c_str());

					bit_ofs = mg_BIToffset(&cdvd.mg_buffer[0]);

					const size_t buf_size = sizeof(cdvd.mg_buffer);

					if (bit_ofs < 0x20 || (size_t)bit_ofs > buf_size)
					{
						fail_pol_cal();
						break;
					}

					const size_t kbit_ofs = bit_ofs - 0x20;
					const size_t kcon_ofs = bit_ofs - 0x10;

					std::memcpy(&cdvd.mg_kbit[0], &cdvd.mg_buffer[kbit_ofs], 0x10);
					std::memcpy(&cdvd.mg_kcon[0], &cdvd.mg_buffer[kcon_ofs], 0x10);

					if ((cdvd.mg_buffer[bit_ofs + 5] || cdvd.mg_buffer[bit_ofs + 6] || cdvd.mg_buffer[bit_ofs + 7]) ||
						(GetBufferU16(&cdvd.mg_buffer[0],bit_ofs + 4) * 16 + bit_ofs + 8 + 16 != GetBufferU16(&cdvd.mg_buffer[0], 0x14)))
					{
						fail_pol_cal();
						break;
					}
				}
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x90:
				SetSCMDResultSize(1);
				cdvd.mg_size = 0;
				cdvd.mg_datatype = 1;
				Console.WriteLn("[MG] hcode=%d cnum=%d a2=%d length=0x%X",
					cdvd.SCMDParamBuff[0], cdvd.SCMDParamBuff[3], cdvd.SCMDParamBuff[4], cdvd.mg_maxsize = cdvd.SCMDParamBuff[1] | (static_cast<int>(cdvd.SCMDParamBuff[2]) << 8));

				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x91:
			{
				SetSCMDResultSize(3);
				const int bit_ofs = mg_BIToffset(&cdvd.mg_buffer[0]);

				if (bit_ofs < 0)
				{
					fail_pol_cal();
					break;
				}

				const size_t bufsize = sizeof(cdvd.mg_buffer);
				const size_t ofs = static_cast<size_t>(bit_ofs);

				if (ofs > bufsize - 5)
				{
					fail_pol_cal();
					break;
				}
				const unsigned int blocks = static_cast<unsigned int>(cdvd.mg_buffer[ofs + 4]);
				const size_t copy_len = 8 + 16 * static_cast<size_t>(blocks);

				if (copy_len > bufsize - ofs)
				{
					fail_pol_cal();
					break;
				}

				std::memmove(&cdvd.mg_buffer[0], &cdvd.mg_buffer[ofs], copy_len);

				cdvd.mg_maxsize = 0;
				cdvd.mg_size = 8 + 16 * cdvd.mg_buffer[4];
				Console.WriteLn("[MG] BIT count=%d", cdvd.mg_buffer[4]);

				cdvd.SCMDResultBuff[0] = (cdvd.mg_datatype == 1) ? 0 : 0x80;
				cdvd.SCMDResultBuff[1] = (cdvd.mg_size >> 0) & 0xFF;
				cdvd.SCMDResultBuff[2] = (cdvd.mg_size >> 8) & 0xFF;
				break;
			}
			case 0x92:
				SetSCMDResultSize(1);
				cdvd.mg_size = 0;
				cdvd.mg_datatype = 0;
				cdvd.mg_maxsize = cdvd.SCMDParamBuff[0] | (((int)cdvd.SCMDParamBuff[1]) << 8);
				cdvd.SCMDResultBuff[0] = 0;
				break;

			case 0x93:
				SetSCMDResultSize(1);
				if (((cdvd.SCMDParamBuff[0] | (static_cast<int>(cdvd.SCMDParamBuff[1]) << 8)) == cdvd.mg_size) && (cdvd.mg_datatype == 0))
				{
					cdvd.mg_maxsize = 0;
					cdvd.SCMDResultBuff[0] = 0;
				}
				else
				{
					cdvd.SCMDResultBuff[0] = 0x80;
				}
				break;

			case 0x94:
				SetSCMDResultSize(1 + 8);
				cdvd.SCMDResultBuff[0] = 0;
				memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kbit, 8);
				break;

			case 0x95:
				SetSCMDResultSize(1 + 8);
				cdvd.SCMDResultBuff[0] = 0;
				memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kbit+8, 8);
				break;

			case 0x96:
				SetSCMDResultSize(1 + 8);
				cdvd.SCMDResultBuff[0] = 0;
				memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kcon, 8);
				break;

			case 0x97:
				SetSCMDResultSize(1 + 8);
				cdvd.SCMDResultBuff[0] = 0;
				memcpy(&cdvd.SCMDResultBuff[1], cdvd.mg_kcon + 8, 8);
				break;

			default:
				SetSCMDResultSize(1);
				cdvd.SCMDResultBuff[0] = 0x80;
				Console.WriteLn("SCMD Unknown %x", rt);
				break;
		}

		cdvd.SCMDParamPos = 0;
		cdvd.SCMDParamCnt = 0;
	}
}

static __fi void cdvdWrite17(u8 rt)
{
	CDVD_LOG("cdvdWrite17(SDataIn) %x", rt);

	if (cdvd.SCMDParamPos >= 16)
	{
		DevCon.Warning("CDVD: SCMD Overflow");
		cdvd.SCMDParamPos = 0;
		cdvd.SCMDParamCnt = 0;
	}

	cdvd.SCMDParamBuff[cdvd.SCMDParamPos++] = rt;
	cdvd.SCMDParamCnt++;
}

static __fi void cdvdWrite18(u8 rt)
{
	CDVD_LOG("cdvdWrite18(SDataOut) %x", rt);
	Console.WriteLn("*PCSX2* SDATAOUT");
}

static __fi void cdvdWrite3A(u8 rt)
{
	CDVD_LOG("cdvdWrite3A(DecSet) %x", rt);
	cdvd.decSet = rt;
}

void cdvdWrite(u8 key, u8 rt)
{
	switch (key)
	{
		case 0x04:
			cdvdWrite04(rt);
			break;
		case 0x05:
			cdvdWrite05(rt);
			break;
		case 0x06:
			cdvdWrite06(rt);
			break;
		case 0x07:
			cdvdWrite07(rt);
			break;
		case 0x08:
			cdvdWrite08(rt);
			break;
		case 0x09:
			if (rt != 0)
				Console.Warning("8bit write to addr 0x1f402009 = 0x%x", rt);
			break;
		case 0x0A:
			cdvdWrite0A(rt);
			break;
		case 0x0F:
			cdvdWrite0F(rt);
			break;
		case 0x14:
			cdvdWrite14(rt);
			break;
		case 0x16:
			cdvdWrite16(rt);
			break;
		case 0x17:
			cdvdWrite17(rt);
			break;
		case 0x18:
			cdvdWrite18(rt);
			break;
		case 0x3A:
			cdvdWrite3A(rt);
			break;
		default:
			Console.Warning("IOP Unknown 8bit write to addr 0x1f4020%02x = 0x%x", key, rt);
			break;
	}
}
