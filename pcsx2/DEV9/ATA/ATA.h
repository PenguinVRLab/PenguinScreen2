// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include "common/RedtapeWindows.h"
#include "common/Path.h"

#include "DEV9/SimpleQueue.h"

class ATA
{
public:
	bool dmaReady = false;
	int nsector = 0;
	int nsectorLeft = 0;
private:
	bool lba48Supported = false;

	std::FILE* hddImage = nullptr;
	u64 hddImageSize;

	bool hddSparse = false;
	u64 hddSparseBlockSize;
	u64 HddSparseStart;
	std::unique_ptr<u8[]> hddSparseBlock;
	bool hddSparseBlockValid = false;

#ifdef _WIN32
	HANDLE hddNativeHandle = INVALID_HANDLE_VALUE;
#elif defined(__POSIX__)
	int hddNativeHandle = -1;
#endif

	int pioMode;
	int mdmaMode;
	int udmaMode;

	u8 curHeads = 16;
	u8 curSectors = 63;
	u16 curCylinders = 0;

	u8 curMultipleSectorsSetting = 128;

	u8 identifyData[512] = {0};

	bool lba48 = false;

	bool fetSmartEnabled = true;
	bool fetSecurityEnabled = false;
	bool fetWriteCacheEnabled = true;
	bool fetHostProtectedAreaEnabled = false;

	u16 regCommand;
	bool regControlEnableIRQ = false;
	bool regControlHOBRead = false;
	u8 regError;

	u8 regSelect;
	u8 regFeature;
	u8 regFeatureHOB;

	u8 regSector;
	u8 regSectorHOB;
	u8 regLcyl;
	u8 regLcylHOB;
	u8 regHcyl;
	u8 regHcylHOB;
	u8 regNsector;
	u8 regNsectorHOB;

	u8 regStatus;
	s8 regStatusSeekLock; 

	bool pendingInterrupt = false;

	bool awaitFlush = false;
	u8* currentWrite;
	u32 currentWriteLength;
	u64 currentWriteSectors;

	struct WriteQueueEntry
	{
		u8* data;
		u32 length;
		u64 sector;
	};
	SimpleQueue<WriteQueueEntry> writeQueue;

	std::thread ioThread;
	bool ioRunning = false;
	std::mutex ioMutex;

	std::condition_variable ioThreadIdle_cv;
	bool ioThreadIdle_bool = false;

	std::condition_variable ioReady;
	std::atomic_bool ioClose{false};
	bool ioWrite;
	bool ioRead;
	void (ATA::*waitingCmd)() = nullptr;

	int rdTransferred = 0;
	int wrTransferred = 0;
	int readBufferLen;
	u8* readBuffer = nullptr;

	int pioPtr;
	int pioEnd;
	u8 pioBuffer[512];

	int sectorsPerInterrupt;
	void (ATA::*pioDRQEndTransferFunc)() = nullptr;

	bool smartAutosave = true;
	bool smartErrors = false;
	u8 smartSelfTestCount = 0;

	u8 sceSec[256 * 2] = {0};

public:
	ATA();
	~ATA();

	int Open(const std::string& hddPath);
	void Close();

	void ATA_HardReset();

	u16 Read(u32 addr, int width);
	void Write(u32 addr, u16 value, int width);

	void Async(u32 cycles);

	int ReadDMAToFIFO(u8* buffer, int space);
	int WriteDMAFromFIFO(u8* buffer, int available);

	u16 ATAreadPIO();

private:
	void InitSparseSupport(const std::string& hddPath);

	void CreateHDDinfo(u64 sizeSectors);
	void CreateHDDinfoCsum();

	void ResetBegin();
	void ResetEnd(bool hard);

	u8 GetSelectedDevice()
	{
		return (regSelect >> 4) & 1;
	}
	void SetSelectedDevice(u8 value)
	{
		if (value == 1)
			regSelect |= (1 << 4);
		else
			regSelect &= ~(1 << 4);
	}

	s64 HDD_GetLBA();
	void HDD_SetLBA(s64 sectorNum);

	bool HDD_CanSeek();
	bool HDD_CanAccess(int* sectors);

	void ClearHOB();

	void IO_Thread();
	void IO_Read();
	bool IO_Write();
	bool IO_SparseZero(u64 byteOffset, u64 byteSize);
	void IO_SparseCacheUpdateLocation(u64 Offset);
	void IO_SparseCacheLoad();
#if defined(PCSX2_DEBUG) || defined(PCSX2_DEVBUILD)
	void IO_SparseCacheAssertFileZeros(u64 hddSparseBlockSizeReadable);
#endif
	bool IsAllZero(const void* data, size_t len);
	void HDD_ReadAsync(void (ATA::*drqCMD)());
	void HDD_ReadSync(void (ATA::*drqCMD)());
	bool HDD_CanAssessOrSetError();
	void HDD_SetErrorAtTransferEnd();

	void IDE_ExecCmd(u16 value);

	bool PreCmd();
	void HDD_Unk();

	void IDE_CmdLBA48Transform(bool islba48);

	void DRQCmdDMADataToHost();
	void PostCmdDMADataToHost();
	void DRQCmdDMADataFromHost();
	void PostCmdDMADataFromHost();
	void HDD_ReadDMA(bool isLBA48);
	void HDD_WriteDMA(bool isLBA48);

	void PreCmdExecuteDeviceDiag();
	void PostCmdExecuteDeviceDiag(bool sendIRQ);
	void HDD_ExecuteDeviceDiag(bool sendIRQ);

	void PostCmdNoData();
	void CmdNoDataAbort();
	void HDD_FlushCache();
	void HDD_InitDevParameters();
	void HDD_ReadVerifySectors(bool isLBA48);
	void HDD_Recalibrate();
	void HDD_SeekCmd();
	void HDD_SetFeatures();
	void HDD_SetMultipleMode();
	void HDD_Nop();
	void HDD_Idle();
	void HDD_IdleImmediate();

	void DRQCmdPIODataToHost(u8* buff, int buffLen, int buffIndex, int size, bool sendIRQ);
	void PostCmdPIODataToHost();
	void HDD_IdentifyDevice();

	void HDD_ReadMultiple(bool isLBA48);
	void HDD_ReadSectors(bool isLBA48);
	void HDD_ReadPIO(bool isLBA48);
	void HDD_ReadPIOS2();
	void HDD_ReadPIOEndBlock();

	void HDD_Smart();
	void SMART_SetAutoSaveAttribute();
	void SMART_SaveAttribute();
	void SMART_ExecuteOfflineImmediate();
	void SMART_EnableOps(bool enable);
	void SMART_ReturnStatus();

	void HDD_SCE();
	void SCE_IDENTIFY_DRIVE();

	static void WriteUInt16(u8* data, int* index, u16 value);
	static void WriteUInt32(u8* data, int* index, u32 value);
	static void WriteUInt64(u8* data, int* index, u64 value);
	static void WritePaddedString(u8* data, int* index, const std::string& value, u32 len);
};
