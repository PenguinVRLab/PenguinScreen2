// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/Threading.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "VUmicro.h"

#include <thread>

#define MTVU_LOG(...) do{} while(0)

class VU_Thread final {
	static const s32 buffer_size = (_1mb * 16) / sizeof(s32);

	u32 buffer[buffer_size];
	alignas(__cachelinesize) std::atomic<int> m_ato_read_pos;
	alignas(__cachelinesize) std::atomic<int> m_ato_write_pos;
	alignas(__cachelinesize) int  m_read_pos;
	int  m_write_pos;
	Threading::WorkSema semaEvent;
	std::atomic_bool m_shutdown_flag{false};

	Threading::Thread m_thread;

public:
	alignas(16)  vifStruct        vif;
	alignas(16)  VIFregisters     vifRegs;
	Threading::UserspaceSemaphore semaXGkick;
	std::atomic<unsigned int> vuCycles[4];
	u32 vuCycleIdx;
	u32 vuFBRST;

	enum InterruptFlag {
		InterruptFlagFinish = 1 << 0,
		InterruptFlagSignal = 1 << 1,
		InterruptFlagLabel  = 1 << 2,
		InterruptFlagVUEBit = 1 << 3,
		InterruptFlagVUTBit = 1 << 4,
	};

	std::atomic<u32> mtvuInterrupts;
	std::atomic<u64> gsLabel;
	std::atomic<u64> gsSignal;

	VU_Thread();
	~VU_Thread();

	__fi const Threading::ThreadHandle& GetThreadHandle() const { return m_thread; }

	__fi bool IsOpen() const { return m_thread.Joinable(); }

	void Open();

	void Close();

	void Reset();

	void KickStart();

	bool IsDone();

	void WaitVU();

	void Get_MTVUChanges();

	void ExecuteVU(u32 vu_addr, u32 vif_top, u32 vif_itop, u32 fbrst);

	void VifUnpack(vifStruct& _vif, VIFregisters& _vifRegs, const u8* data, u32 size);

	void WriteMicroMem(u32 vu_micro_addr, const void* data, u32 size);

	void WriteDataMem(u32 vu_data_addr, const void* data, u32 size);

	void WriteVIRegs(REG_VI* viRegs);

	void WriteVFRegs(VECTOR* vfRegs);

	void WriteCol(vifStruct& _vif);

	void WriteRow(vifStruct& _vif);

private:
	void ExecuteRingBuffer();

	void WaitOnSize(s32 size);
	void ReserveSpace(s32 size);

	s32 GetReadPos();
	s32 GetWritePos();

	u32* GetWritePtr();

	void CommitWritePos();
	void CommitReadPos();

	u32 Read();
	void Read(void* dest, u32 size);
	void ReadRegs(VIFregisters* dest);

	void Write(u32 val);
	void Write(const void* src, u32 size);
	void WriteRegs(VIFregisters* src);

	u32 Get_vuCycles();
};

extern VU_Thread vu1Thread;
