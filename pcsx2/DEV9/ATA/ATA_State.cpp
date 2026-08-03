// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "common/FileSystem.h"

#include "ATA.h"
#include "DEV9/DEV9.h"

#if _WIN32
#include "pathcch.h"
#include <io.h>
#elif defined(__POSIX__)
#define INVALID_HANDLE_VALUE -1
#if defined(__APPLE__)
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
static_assert(sizeof(off_t) >= 8, "off_t is not 64bit");
#endif

ATA::ATA()
{
	ResetBegin();
	ResetEnd(true);
}

ATA::~ATA()
{
	if (hddImage)
		std::fclose(hddImage);
}

int ATA::Open(const std::string& hddPath)
{
	readBufferLen = 256 * 512;
	readBuffer = new u8[readBufferLen];
	memset(sceSec, 0, sizeof(sceSec));

	DevCon.WriteLn("DEV9: ATA: HddFile : %s", hddPath.c_str());

	if (!FileSystem::FileExists(hddPath.c_str()))
		return -1;

	hddImage = FileSystem::OpenCFile(hddPath.c_str(), "r+b");
	const s64 size = hddImage ? FileSystem::FSize64(hddImage) : -1;
	if (!hddImage || size < 0)
	{
		Console.Error("DEV9: ATA: Failed to open HDD image '%s'", hddPath.c_str());
		return -1;
	}

	std::string hddidPath = Path::ReplaceExtension(hddPath, "hddid");
	std::optional<std::vector<u8>> fileContent = FileSystem::ReadBinaryFile(hddidPath.c_str());

	if (fileContent.has_value() && fileContent.value().size() <= sizeof(sceSec))
	{
		std::copy(fileContent.value().begin(), fileContent.value().end(), sceSec);
	}
	else
	{
		memcpy(sceSec, "Sony Computer Entertainment Inc.", 32);
		memcpy(sceSec + 0x20, "SCPH-20401", 10);
		memcpy(sceSec + 0x30, "  40", 4);

		sceSec[0x40] = 0;
		sceSec[0x41] = 0;
		sceSec[0x42] = 0;
		sceSec[0x43] = 0x01;

		sceSec[0x44] = 0;
		sceSec[0x45] = 0;
		sceSec[0x46] = 0x1a;
		sceSec[0x47] = 0x01;
		sceSec[0x48] = 0x02;
		sceSec[0x49] = 0x20;
		sceSec[0x4a] = 0;
		sceSec[0x4b] = 0;
		sceSec[0x4c] = 0x01;
		sceSec[0x4d] = 0x03;
		sceSec[0x4e] = 0x11;
		sceSec[0x4f] = 0x01;
	}

	hddImageSize = static_cast<u64>(size);
	lba48Supported = (hddImageSize > ((static_cast<s64>(1) << 28) - 1) * 512);

	CreateHDDinfo(hddImageSize / 512);

	InitSparseSupport(hddPath);

	{
		std::lock_guard ioSignallock(ioMutex);
		ioRead = false;
		ioWrite = false;
	}

	ioThread = std::thread(&ATA::IO_Thread, this);
	ioRunning = true;

	return 0;
}

void ATA::InitSparseSupport(const std::string& hddPath)
{
#ifdef _WIN32
	hddSparse = false;

	const std::wstring wHddPath = FileSystem::GetWin32Path(hddPath);
	const DWORD fileAttributes = GetFileAttributes(wHddPath.c_str());
	hddSparse = fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE;

	if (!hddSparse)
		return;

	hddNativeHandle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(hddImage)));
	if (hddNativeHandle == INVALID_HANDLE_VALUE)
	{
		Console.Error("DEV9: ATA: Failed to open file for sparse");
		hddSparse = false;
		return;
	}

	hddSparseBlockSize = 4096;

	DWORD len = GetFinalPathNameByHandle(hddNativeHandle, nullptr, 0, FILE_NAME_NORMALIZED);

	if (len != 0)
	{
		std::unique_ptr<TCHAR[]> name = std::make_unique<TCHAR[]>(len);
		len = GetFinalPathNameByHandle(hddNativeHandle, name.get(), len, FILE_NAME_NORMALIZED);
		if (len != 0)
		{
			PCWSTR rootEnd;
			if (PathCchSkipRoot(name.get(), &rootEnd) == S_OK)
			{
				const size_t rootLength = rootEnd - name.get();
				std::wstring finalPath(name.get(), rootLength);

				DWORD sectorsPerCluster;
				DWORD bytesPerSector;
				DWORD temp1, temp2;
				if (GetDiskFreeSpace(finalPath.c_str(), &sectorsPerCluster, &bytesPerSector, &temp1, &temp2) == TRUE)
					hddSparseBlockSize = sectorsPerCluster * bytesPerSector;
				else
					Console.Error("DEV9: ATA: Failed to get sparse block size (GetDiskFreeSpace() returned false)");
			}
			else
				Console.Error("DEV9: ATA: Failed to get sparse block size (PathCchSkipRoot() returned false)");
		}
		else
			Console.Error("DEV9: ATA: Failed to get sparse block size (PathBuildRoot() returned 0)");
	}
	else
		Console.Error("DEV9: ATA: Failed to get sparse block size (GetFinalPathNameByHandle() returned 0)");

	WCHAR fsName[MAX_PATH + 1];
	const BOOL ret = GetVolumeInformationByHandleW(hddNativeHandle, nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH);
	if (ret == FALSE)
	{
		Console.Error("DEV9: ATA: Failed to get sparse block size (GetVolumeInformationByHandle() returned false)");
		wcscpy(fsName, L"NTFS");
	}
	if ((wcscmp(fsName, L"NTFS") == 0))
	{
		switch (hddSparseBlockSize)
		{
			case 512:
				hddSparseBlockSize = 8192;
				break;
			case 1024:
				hddSparseBlockSize = 16384;
				break;
			case 2048:
				hddSparseBlockSize = 32768;
				break;
			case 4096:
			case 8192:
			case 16384:
			case 32768:
			case 65536:
				hddSparseBlockSize = 65536;
				break;
			default:
				break;
		}
	}

#elif defined(__POSIX__)
	hddNativeHandle = fileno(hddImage);
	hddSparse = false;
	if (hddNativeHandle != -1)
	{
		hddSparse = true;

		hddSparseBlockSize = 4096;
		struct stat fileInfo;
		if (fstat(hddNativeHandle, &fileInfo) == 0)
			hddSparseBlockSize = fileInfo.st_blksize;
		else
			Console.Error("DEV9: ATA: Failed to get sparse block size (fstat returned != 0)");
	}
	else
		Console.Error("DEV9: ATA: Failed to open file for sparse");
#endif
	hddSparseBlock = std::make_unique<u8[]>(hddSparseBlockSize);
	hddSparseBlockValid = false;
}

void ATA::Close()
{
	if (ioRunning)
	{
		ioClose.store(true);
		{
			std::lock_guard ioSignallock(ioMutex);
			ioWrite = true;
		}
		ioReady.notify_all();

		ioThread.join();
		ioRunning = false;
	}

	if (!writeQueue.IsQueueEmpty())
	{
		Console.Error("DEV9: ATA: Write queue not empty, possible data loss");
		pxAssert(false);
		abort();
	}

	if (hddSparse)
	{
		hddNativeHandle = INVALID_HANDLE_VALUE;

		hddSparse = false;
		hddSparseBlock = nullptr;
		hddSparseBlockValid = false;
	}
	if (hddImage)
	{
		std::fclose(hddImage);
		hddImage = nullptr;
	}

	delete[] readBuffer;
	readBuffer = nullptr;
}

void ATA::ResetBegin()
{
	PreCmdExecuteDeviceDiag();
}
void ATA::ResetEnd(bool hard)
{
	curHeads = 16;
	curSectors = 63;
	curCylinders = 0;
	curMultipleSectorsSetting = 128;

	if (hard)
	{
		pioMode = 4;
		mdmaMode = 2;
		udmaMode = -1;
	}
	else
	{
		pioMode = 4;
		if (udmaMode == -1)
			mdmaMode = 2;
	}

	regStatus |= ATA_STAT_SEEK;
	regStatusSeekLock = 0;

	HDD_ExecuteDeviceDiag(false);
	regControlEnableIRQ = false;
}

void ATA::ATA_HardReset()
{
	ResetBegin();
	ResetEnd(true);
}

u16 ATA::Read(u32 addr, int width)
{
	switch (addr)
	{
		case ATA_R_DATA:
			if (width == 8)
				Console.Error("DEV9:ATA : ATA_R_DATA 8bit read???, Active %s", (GetSelectedDevice() == 0) ? "True" : "False");
			return ATAreadPIO();
		case ATA_R_ERROR:
			if (GetSelectedDevice() != 0)
				return 0;
			return regError;
		case ATA_R_NSECTOR:
			if (GetSelectedDevice() != 0)
				return 0;
			if (!regControlHOBRead)
				return regNsector;
			else
				return regNsectorHOB;
		case ATA_R_SECTOR:
			if (GetSelectedDevice() != 0)
				return 0;
			if (!regControlHOBRead)
				return regSector;
			else
				return regSectorHOB;
		case ATA_R_LCYL:
			if (GetSelectedDevice() != 0)
				return 0;
			if (!regControlHOBRead)
				return regLcyl;
			else
				return regLcylHOB;
		case ATA_R_HCYL:
			if (GetSelectedDevice() != 0)
				return 0;
			if (!regControlHOBRead)
				return regHcyl;
			else
				return regHcylHOB;
		case ATA_R_SELECT:
			return regSelect;
		case ATA_R_STATUS:
			pendingInterrupt = false;
			dev9.irqcause &= ~ATA_INTR_INTRQ;
			[[fallthrough]];
		case ATA_R_ALT_STATUS:

			if (!EmuConfig.DEV9.HddEnable)
				return 0xff7f;

			if (GetSelectedDevice() != 0)
				return 0;

			if (regStatusSeekLock != 0)
			{
				u8 hard = (regStatus & ~ATA_STAT_SEEK);
				hard |= (regStatusSeekLock > 0) ? ATA_STAT_SEEK : static_cast<u8>(0);
				if (addr == ATA_R_STATUS)
					regStatusSeekLock = 0;
				return hard;
			}

			return regStatus;
		default:
			Console.Error("DEV9: ATA: Unknown %dbit read at address %x", width, addr);
			return 0xff;
	}
}

void ATA::Write(u32 addr, u16 value, int width)
{
	if ((addr != ATA_R_CMD && addr != ATA_R_CONTROL) && (regStatus & (ATA_STAT_BUSY | ATA_STAT_DRQ)) != 0)
	{
		Console.Error("DEV9: ATA: DEVICE BUSY, DROPPING WRITE");
		return;
	}
	switch (addr)
	{
		case ATA_R_FEATURE:
			ClearHOB();
			regFeatureHOB = regFeature;
			regFeature = static_cast<u8>(value);
			break;
		case ATA_R_NSECTOR:
			ClearHOB();
			regNsectorHOB = regNsector;
			regNsector = static_cast<u8>(value);
			break;
		case ATA_R_SECTOR:
			ClearHOB();
			regSectorHOB = regSector;
			regSector = static_cast<u8>(value);
			break;
		case ATA_R_LCYL:
			ClearHOB();
			regLcylHOB = regLcyl;
			regLcyl = static_cast<u8>(value);
			break;
		case ATA_R_HCYL:
			ClearHOB();
			regHcylHOB = regHcyl;
			regHcyl = static_cast<u8>(value);
			break;
		case ATA_R_SELECT:
		{
			const int oldDev = GetSelectedDevice();
			const int newDev = (value >> 4) & 1;
			if (oldDev == 0 && newDev == 1)
			{
				dev9.irqcause &= ~ATA_INTR_INTRQ;
			}
			else if (oldDev == 1 && newDev == 0)
			{
				if (regControlEnableIRQ && pendingInterrupt)
					_DEV9irq(ATA_INTR_INTRQ, 1);
			}

			regSelect = static_cast<u8>(value);
			break;
		}
		case ATA_R_CONTROL:
			if ((value & 0x2) != 0)
			{
				dev9.irqcause &= ~ATA_INTR_INTRQ;
				regControlEnableIRQ = false;
			}
			else
			{
				if (GetSelectedDevice() == 0 && regControlEnableIRQ == false && pendingInterrupt)
					_DEV9irq(ATA_INTR_INTRQ, 1);
				regControlEnableIRQ = true;
			}

			if ((value & 0x4) != 0)
			{
				DevCon.WriteLn("DEV9: *ATA_R_CONTROL RESET");
				ResetBegin();
				ResetEnd(false);
			}
			if ((value & 0x80) != 0)
				regControlHOBRead = true;

			break;
		case ATA_R_CMD:
			regCommand = value;
			regControlHOBRead = false;
			pendingInterrupt = false;
			dev9.irqcause &= ~ATA_INTR_INTRQ;
			IDE_ExecCmd(value);
			break;
		default:
			Console.Error("DEV9: ATA: Unknown %dbit write at address %x, value %x", width, addr, value);
			break;
	}
}

void ATA::Async(uint cycles)
{
	if (!hddImage)
		return;

	if ((regStatus & (ATA_STAT_BUSY | ATA_STAT_DRQ)) == 0 ||
		awaitFlush || (waitingCmd != nullptr))
	{
		{
			std::lock_guard ioSignallock(ioMutex);
			if (ioRead || ioWrite)
				return;
		}

		if (waitingCmd != nullptr)
		{
			void (ATA::*cmd)() = waitingCmd;
			waitingCmd = nullptr;
			(this->*cmd)();
		}
		else if (!writeQueue.IsQueueEmpty())
		{
			{
				std::lock_guard ioSignallock(ioMutex);
				ioWrite = true;
			}
			ioReady.notify_all();
		}
		else if (awaitFlush)
		{
			awaitFlush = false;
			PostCmdNoData();
		}
	}
}

s64 ATA::HDD_GetLBA()
{
	if ((regSelect & 0x40) != 0)
	{
		if (!lba48)
		{
			return (regSector |
					(regLcyl << 8) |
					(regHcyl << 16) |
					((regSelect & 0x0f) << 24));
		}
		else
		{
			return (static_cast<s64>(regHcylHOB) << 40) |
				   (static_cast<s64>(regLcylHOB) << 32) |
				   (static_cast<s64>(regSectorHOB) << 24) |
				   (static_cast<s64>(regHcyl) << 16) |
				   (static_cast<s64>(regLcyl) << 8) |
				   regSector;
		}
	}
	else
	{
		regStatus |= static_cast<u8>(ATA_STAT_ERR);
		regError |= static_cast<u8>(ATA_ERR_ABORT);

		Console.Error("DEV9: ATA: Tried to get LBA address while LBA mode disabled");
		return -1;
	}
}

void ATA::HDD_SetLBA(s64 sectorNum)
{
	if ((regSelect & 0x40) != 0)
	{
		if (!lba48)
		{
			regSelect = static_cast<u8>((regSelect & 0xf0) | static_cast<int>((sectorNum >> 24) & 0x0f));
			regHcyl = static_cast<u8>(sectorNum >> 16);
			regLcyl = static_cast<u8>(sectorNum >> 8);
			regSector = static_cast<u8>(sectorNum);
		}
		else
		{
			regSector = static_cast<u8>(sectorNum);
			regLcyl = static_cast<u8>(sectorNum >> 8);
			regHcyl = static_cast<u8>(sectorNum >> 16);
			regSectorHOB = static_cast<u8>(sectorNum >> 24);
			regLcylHOB = static_cast<u8>(sectorNum >> 32);
			regHcylHOB = static_cast<u8>(sectorNum >> 40);
		}
	}
	else
	{
		regStatus |= ATA_STAT_ERR;
		regError |= ATA_ERR_ABORT;

		Console.Error("DEV9: ATA: Tried to set LBA address while LBA mode disabled");
	}
}

bool ATA::HDD_CanSeek()
{
	int sectors = 0;
	return HDD_CanAccess(&sectors);
}

bool ATA::HDD_CanAccess(int* sectors)
{
	s64 maxLBA = hddImageSize / 512 - 1;
	if ((regSelect & 0x40) == 0)
		maxLBA = std::min<s64>(maxLBA, curCylinders * curHeads * curSectors);

	const s64 posStart = HDD_GetLBA();
	if (posStart == -1)
		return false;

	if (posStart > maxLBA)
	{
		*sectors = -1;
		return false;
	}

	const s64 posEnd = posStart + *sectors;
	if (posEnd > maxLBA)
	{
		const s64 overshoot = posEnd - maxLBA;
		s64 space = *sectors - overshoot;
		*sectors = static_cast<int>(space);
		return false;
	}

	return true;
}

void ATA::ClearHOB()
{
	regControlHOBRead = false;
}
