// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS.h"
#include "Gif_Unit.h"
#include "MTGS.h"
#include "MTVU.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "VMManager.h"

#include "common/FPControl.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"
#include "common/WrappedMemCopy.h"

#include <list>
#include <mutex>
#include <thread>

#if 0
#define MTGS_LOG Console.WriteLn
#else
#define MTGS_LOG(...) \
	do                \
	{                 \
	} while (0)
#endif

namespace MTGS
{
	struct BufferedData
	{
		u128 m_Ring[RingBufferSize];
		u8 Regs[Ps2MemSize::GSregs];

		u128& operator[](uint idx)
		{
			pxAssert(idx < RingBufferSize);
			return m_Ring[idx];
		}
	};

	static void ThreadEntryPoint();
	static void MainLoop();

	static void GenericStall(uint size);

	static void PrepDataPacket(Command cmd, u32 size);
	static void PrepDataPacket(GIF_PATH pathidx, u32 size);
	static void SendDataPacket();

	static void SendSimplePacket(Command type, int data0, int data1, int data2);
	static void SendSimpleGSPacket(Command type, u32 offset, u32 size, GIF_PATH path);
	static void SendPointerPacket(Command type, u32 data0, void* data1);
	static void _FinishSimplePacket();
	static u8* GetDataPacketPtr();

	static void SetEvent();

	alignas(__cachelinesize) BufferedData RingBuffer;

	alignas(__cachelinesize) static std::atomic<unsigned int> s_ReadPos;
	alignas(__cachelinesize) static std::atomic<unsigned int> s_WritePos;

	static u32 s_packet_startpos;
	static u32 s_packet_size;
	static u32 s_packet_writepos;

	static std::atomic<bool> s_SignalRingEnable;
	static std::atomic<int> s_SignalRingPosition;

	static std::atomic<int> s_QueuedFrameCount;
	static std::atomic<bool> s_VsyncSignalListener;

	static std::mutex s_mtx_RingBufferBusy2;
	static Threading::WorkSema s_sem_event;
	static Threading::UserspaceSemaphore s_sem_OnRingReset;
	static Threading::UserspaceSemaphore s_sem_Vsync;

	static int s_CopyDataTally;

#ifdef RINGBUF_DEBUG_STACK
	static std::mutex s_lock_Stack;
	static std::list<uint> ringposStack;
#endif

	static Threading::Thread s_thread;
	static std::atomic_bool s_open_flag{false};
	static std::atomic_bool s_shutdown_flag{false};
	static std::atomic_bool s_run_idle_flag{false};
	static Threading::UserspaceSemaphore s_open_or_close_done;
}

const Threading::ThreadHandle& MTGS::GetThreadHandle()
{
	return s_thread;
}

bool MTGS::IsOpen()
{
	return s_open_flag.load(std::memory_order_acquire);
}

void MTGS::StartThread()
{
	if (s_thread.Joinable())
		return;

	pxAssertRel(!s_open_flag.load(), "GS thread should not be opened when starting");
	s_sem_event.Reset();
	s_shutdown_flag.store(false, std::memory_order_release);
	s_thread.Start(&MTGS::ThreadEntryPoint);
}

void MTGS::ShutdownThread()
{
	if (!s_thread.Joinable())
		return;

	s_shutdown_flag.store(true, std::memory_order_release);
	if (IsOpen())
		WaitForClose();

	s_sem_event.NotifyOfWork();
	s_thread.Join();
}

void MTGS::ThreadEntryPoint()
{
	Threading::SetNameOfCurrentThread("GS");

	PageFaultHandler::InstallSecondaryThread();

	FPControlRegister::SetCurrent(FPControlRegister::GetDefault());

	for (;;)
	{
		while (!s_open_flag.load(std::memory_order_acquire))
		{
			if (s_shutdown_flag.load(std::memory_order_acquire))
			{
				s_sem_event.Kill();
				return;
			}

			s_sem_event.WaitForWork();
		}

		std::memcpy(RingBuffer.Regs, PS2MEM_GS, sizeof(PS2MEM_GS));
		const bool opened = GSopen(EmuConfig.GS, EmuConfig.GS.Renderer, RingBuffer.Regs,
			VMManager::GetEffectiveVSyncMode(), VMManager::ShouldAllowPresentThrottle());
		s_open_flag.store(opened, std::memory_order_release);

		s_open_or_close_done.Post();

		if (!opened)
		{
			continue;
		}

		MainLoop();

		pxAssertRel(!s_open_flag.load(std::memory_order_relaxed), "Open flag is clear on close");
		GSclose();
		s_open_or_close_done.Post();

		s_sem_event.Reset();
	}
}

void MTGS::ResetGS(bool hardware_reset)
{

	if (hardware_reset)
	{
		s_ReadPos = s_WritePos.load();
		s_QueuedFrameCount = 0;
		s_VsyncSignalListener = 0;
	}

	MTGS_LOG("MTGS: Sending Reset...");
	SendSimplePacket(Command::Reset, static_cast<int>(hardware_reset), 0, 0);

	if (hardware_reset)
		SetEvent();
}

int MTGS::GetCurrentVsyncQueueSize()
{
	return s_QueuedFrameCount.load(std::memory_order_acquire);
}

struct RingCmdPacket_Vsync
{
	u8 regset1[0x0f0];
	u32 csr;
	u32 imr;
	GSRegSIGBLID siglblid;

	u32 registers_written;
	u32 pad[3];
};

void MTGS::PostVsyncStart(bool registers_written)
{

	uint packsize = sizeof(RingCmdPacket_Vsync) / 16;
	PrepDataPacket(Command::VSync, packsize);
	MemCopy_WrappedDest((u128*)PS2MEM_GS, RingBuffer.m_Ring, s_packet_writepos, RingBufferSize, 0xf);

	u32* remainder = (u32*)GetDataPacketPtr();
	remainder[0] = GSCSRr;
	remainder[1] = GSIMR._u32;
	(GSRegSIGBLID&)remainder[2] = GSSIGLBLID;
	remainder[4] = static_cast<u32>(registers_written);
	s_packet_writepos = (s_packet_writepos + 2) & RingBufferMask;

	SendDataPacket();

	if (s_CopyDataTally != 0)
		SetEvent();

	if ((s_QueuedFrameCount.fetch_add(1) < EmuConfig.GS.VsyncQueueSize) )
		return;

	s_VsyncSignalListener.store(true, std::memory_order_release);

	s_sem_Vsync.Wait();
}

void MTGS::InitAndReadFIFO(u8* mem, u32 qwc)
{
	if (EmuConfig.GS.HWDownloadMode >= GSHardwareDownloadMode::Unsynchronized && GSIsHardwareRenderer())
	{
		if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
			GSReadLocalMemoryUnsync(mem, qwc, vif1.BITBLTBUF._u64, vif1.TRXPOS._u64, vif1.TRXREG._u64);
		else
			std::memset(mem, 0, qwc * 16);

		return;
	}

	SendPointerPacket(Command::InitAndReadFIFO, qwc, mem);
	WaitGS(false, false, false);
}

union PacketTagType
{
	struct
	{
		u32 command;
		u32 data[3];
	};
	struct
	{
		u32 _command;
		u32 _data[1];
		uptr pointer;
	};
};

void MTGS::MainLoop()
{

#ifdef RINGBUF_DEBUG_STACK
	PacketTagType prevCmd;
#endif

	std::unique_lock mtvu_lock(s_mtx_RingBufferBusy2);

	while (true)
	{
		if (s_run_idle_flag.load(std::memory_order_acquire) && VMManager::GetState() != VMState::Running && GSHasDisplayWindow())
		{
			if (!s_sem_event.CheckForWork())
			{
				GSPresentCurrentFrame();
				GSThrottlePresentation();
			}
		}
		else
		{
			mtvu_lock.unlock();
			s_sem_event.WaitForWork();
			mtvu_lock.lock();
		}

		if (!s_open_flag.load(std::memory_order_acquire))
			break;

		while (s_ReadPos.load(std::memory_order_relaxed) != s_WritePos.load(std::memory_order_acquire))
		{
			const unsigned int local_ReadPos = s_ReadPos.load(std::memory_order_relaxed);

			pxAssert(local_ReadPos < RingBufferSize);

			const PacketTagType& tag = (PacketTagType&)RingBuffer[local_ReadPos];
			u32 ringposinc = 1;

#ifdef RINGBUF_DEBUG_STACK

			s_lock_Stack.Lock();
			uptr stackpos = ringposStack.back();
			if (stackpos != local_ReadPos)
			{
				Console.Error("MTGS Ringbuffer Critical Failure ---> %x to %x (prevCmd: %x)\n", stackpos, local_ReadPos, prevCmd.command);
			}
			pxAssert(stackpos == local_ReadPos);
			prevCmd = tag;
			ringposStack.pop_back();
			s_lock_Stack.Release();
#endif

			switch (static_cast<Command>(tag.command))
			{
#if COPY_GS_PACKET_TO_MTGS == 1
				case Command::GIFPath1:
				{
					uint datapos = (local_ReadPos + 1) & RingBufferMask;
					const int qsize = tag.data[0];
					const u128* data = &RingBuffer[datapos];

					MTGS_LOG("(MTGS Packet Read) ringtype=P1, qwc=%u", qsize);

					uint endpos = datapos + qsize;
					if (endpos >= RingBufferSize)
					{
						uint firstcopylen = RingBufferSize - datapos;
						GSgifTransfer((u8*)data, firstcopylen);
						datapos = endpos & RingBufferMask;
						GSgifTransfer((u8*)RingBuffer.m_Ring, datapos);
					}
					else
					{
						GSgifTransfer((u8*)data, qsize);
					}

					ringposinc += qsize;
				}
				break;

				case Command::GIFPath2:
				{
					uint datapos = (local_ReadPos + 1) & RingBufferMask;
					const int qsize = tag.data[0];
					const u128* data = &RingBuffer[datapos];

					MTGS_LOG("(MTGS Packet Read) ringtype=P2, qwc=%u", qsize);

					uint endpos = datapos + qsize;
					if (endpos >= RingBufferSize)
					{
						uint firstcopylen = RingBufferSize - datapos;
						GSgifTransfer2((u32*)data, firstcopylen);
						datapos = endpos & RingBufferMask;
						GSgifTransfer2((u32*)RingBuffer.m_Ring, datapos);
					}
					else
					{
						GSgifTransfer2((u32*)data, qsize);
					}

					ringposinc += qsize;
				}
				break;

				case Command::GIFPath3:
				{
					uint datapos = (local_ReadPos + 1) & RingBufferMask;
					const int qsize = tag.data[0];
					const u128* data = &RingBuffer[datapos];

					MTGS_LOG("(MTGS Packet Read) ringtype=P3, qwc=%u", qsize);

					uint endpos = datapos + qsize;
					if (endpos >= RingBufferSize)
					{
						uint firstcopylen = RingBufferSize - datapos;
						GSgifTransfer3((u32*)data, firstcopylen);
						datapos = endpos & RingBufferMask;
						GSgifTransfer3((u32*)RingBuffer.m_Ring, datapos);
					}
					else
					{
						GSgifTransfer3((u32*)data, qsize);
					}

					ringposinc += qsize;
				}
				break;
#endif
				case Command::GSPacket:
				{
					Gif_Path& path = gifUnit.gifPath[tag.data[2]];
					u32 offset = tag.data[0];
					u32 size = tag.data[1];
					if (offset != ~0u)
						GSgifTransfer((u8*)&path.buffer[offset], size / 16);
					path.readAmount.fetch_sub(size, std::memory_order_acq_rel);
					break;
				}

				case Command::MTVUGSPacket:
				{
					MTVU_LOG("MTGS - Waiting on semaXGkick!");
					if (!vu1Thread.semaXGkick.TryWait())
					{
						mtvu_lock.unlock();
						vu1Thread.semaXGkick.Wait();
						mtvu_lock.lock();
					}
					Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];
					GS_Packet gsPack = path.GetGSPacketMTVU();
					if (gsPack.size)
						GSgifTransfer((u8*)&path.buffer[gsPack.offset], gsPack.size / 16);
					path.readAmount.fetch_sub(gsPack.size + gsPack.readAmount, std::memory_order_acq_rel);
					path.PopGSPacketMTVU();
					break;
				}

				default:
				{
					switch (static_cast<Command>(tag.command))
					{
						case Command::VSync:
						{
							const int qsize = tag.data[0];
							ringposinc += qsize;

							MTGS_LOG("(MTGS Packet Read) ringtype=Vsync, field=%u, skip=%s", !!(((u32&)RingBuffer.Regs[0x1000]) & 0x2000) ? 0 : 1, tag.data[1] ? "true" : "false");

							uint datapos = (local_ReadPos + 1) & RingBufferMask;
							MemCopy_WrappedSrc(RingBuffer.m_Ring, datapos, RingBufferSize, (u128*)RingBuffer.Regs, 0xf);

							u32* remainder = (u32*)&RingBuffer[datapos];
							((u32&)RingBuffer.Regs[0x1000]) = remainder[0];
							((u32&)RingBuffer.Regs[0x1010]) = remainder[1];
							((GSRegSIGBLID&)RingBuffer.Regs[0x1080]) = (GSRegSIGBLID&)remainder[2];

							GSvsync((((u32&)RingBuffer.Regs[0x1000]) & 0x2000) ? 0 : 1, remainder[4] != 0);

							s_QueuedFrameCount.fetch_sub(1);
							if (s_VsyncSignalListener.exchange(false))
								s_sem_Vsync.Post();

						}
						break;

						case Command::AsyncCall:
							{
								AsyncCallType* const func = (AsyncCallType*)tag.pointer;
								(*func)();
								delete func;
							}
							break;

						case Command::Freeze:
						{
							MTGS::FreezeData* data = (MTGS::FreezeData*)tag.pointer;
							int mode = tag.data[0];
							data->retval = GSfreeze((FreezeAction)mode, (freezeData*)data->fdata);
						}
						break;

						case Command::Reset:
							MTGS_LOG("(MTGS Packet Read) ringtype=Reset");
							GSreset(tag.data[0] != 0);
							break;

						case Command::SoftReset:
						{
							int mask = tag.data[0];
							MTGS_LOG("(MTGS Packet Read) ringtype=SoftReset");
							GSgifSoftReset(mask);
						}
						break;

						case Command::InitAndReadFIFO:
							MTGS_LOG("(MTGS Packet Read) ringtype=Fifo2, size=%d", tag.data[0]);
							GSInitAndReadFIFO((u8*)tag.pointer, tag.data[0]);
							break;

#ifdef PCSX2_DEVBUILD
						default:
							Console.Error("GSThreadProc, bad packet (%x) at m_ReadPos: %x, m_WritePos: %x", tag.command, local_ReadPos, s_WritePos.load());
							pxFail("Bad packet encountered in the MTGS Ringbuffer.");
							s_ReadPos.store(s_WritePos.load(std::memory_order_acquire), std::memory_order_release);
							continue;
#else
							jNO_DEFAULT;
#endif
					}
				}
			}

			uint newringpos = (s_ReadPos.load(std::memory_order_relaxed) + ringposinc) & RingBufferMask;

			if (IsDevBuild && EmuConfig.GS.SynchronousMTGS) [[unlikely]]
			{
				pxAssert(s_WritePos == newringpos);
			}

			s_ReadPos.store(newringpos, std::memory_order_release);

			if (s_SignalRingEnable.load(std::memory_order_acquire))
			{
				if (s_SignalRingPosition.fetch_sub(ringposinc) <= 0)
				{
					s_SignalRingEnable.store(false, std::memory_order_release);
					s_sem_OnRingReset.Post();
					continue;
				}
			}
		}

		if (s_SignalRingEnable.exchange(false))
		{
			s_SignalRingPosition.store(0, std::memory_order_release);
			s_sem_OnRingReset.Post();
		}

		if (s_VsyncSignalListener.exchange(false))
			s_sem_Vsync.Post();

	}

	s_ReadPos.store(s_WritePos.load(std::memory_order_acquire), std::memory_order_relaxed);
	s_sem_event.Kill();
}

void MTGS::WaitGS(bool syncRegs, bool weakWait, bool isMTVU)
{
	pxAssertMsg(IsOpen(), "MTGS Warning!  WaitGS issued on a closed thread.");
	if (!IsOpen()) [[unlikely]]
		return;

	Gif_Path& path = gifUnit.gifPath[GIF_PATH_1];

	SetEvent();
	if (weakWait && isMTVU)
	{
		u32 startP1Packs = path.GetPendingGSPackets();
		if (startP1Packs)
		{
			while (true)
			{
				s_mtx_RingBufferBusy2.lock();
				s_mtx_RingBufferBusy2.unlock();
				if (path.GetPendingGSPackets() != startP1Packs)
					break;
			}
		}
	}
	else
	{
		if (!s_sem_event.WaitForEmpty())
			pxFailRel("MTGS Thread Died");
	}

	pxAssert(!(weakWait && syncRegs) && "No synchronization for this!");

	if (syncRegs)
	{
		memcpy(RingBuffer.Regs, PS2MEM_GS, sizeof(RingBuffer.Regs));
	}
}

void MTGS::SetEvent()
{
	s_sem_event.NotifyOfWork();
	s_CopyDataTally = 0;
}

u8* MTGS::GetDataPacketPtr()
{
	return (u8*)&RingBuffer[s_packet_writepos & RingBufferMask];
}

void MTGS::SendDataPacket()
{
	pxAssert(s_packet_size != 0);

	uint actualSize = ((s_packet_writepos - s_packet_startpos) & RingBufferMask) - 1;
	pxAssert(actualSize <= s_packet_size);
	pxAssert(s_packet_writepos < RingBufferSize);

	PacketTagType& tag = (PacketTagType&)RingBuffer[s_packet_startpos];
	tag.data[0] = actualSize;

	s_WritePos.store(s_packet_writepos, std::memory_order_release);

	if (IsDevBuild && EmuConfig.GS.SynchronousMTGS) [[unlikely]]
	{
		WaitGS();
	}
	else
	{
		s_CopyDataTally += s_packet_size;
		if (s_CopyDataTally > 0x2000)
			SetEvent();
	}

	s_packet_size = 0;

}

void MTGS::GenericStall(uint size)
{
	const uint writepos = s_WritePos.load(std::memory_order_relaxed);

	pxAssert(size < RingBufferSize);
	pxAssert(writepos < RingBufferSize);

	uint readpos = s_ReadPos.load(std::memory_order_acquire);
	uint freeroom;

	if (writepos < readpos)
		freeroom = readpos - writepos;
	else
		freeroom = RingBufferSize - (writepos - readpos);

	if (freeroom <= size)
	{

		uint somedone = (RingBufferSize - freeroom) / 4;
		if (somedone < size + 1)
			somedone = size + 1;

		if (somedone > 0x80)
		{
			pxAssertMsg(s_SignalRingEnable == 0, "MTGS Thread Synchronization Error");
			s_SignalRingPosition.store(somedone, std::memory_order_release);

			while (true)
			{
				s_SignalRingEnable.store(true, std::memory_order_release);
				SetEvent();
				s_sem_OnRingReset.Wait();
				readpos = s_ReadPos.load(std::memory_order_acquire);

				if (writepos < readpos)
					freeroom = readpos - writepos;
				else
					freeroom = RingBufferSize - (writepos - readpos);

				if (freeroom > size)
					break;
			}

			pxAssertMsg(s_SignalRingPosition <= 0, "MTGS Thread Synchronization Error");
		}
		else
		{
			SetEvent();
			while (true)
			{
				Threading::SpinWait();
				readpos = s_ReadPos.load(std::memory_order_acquire);

				if (writepos < readpos)
					freeroom = readpos - writepos;
				else
					freeroom = RingBufferSize - (writepos - readpos);

				if (freeroom > size)
					break;
			}
		}
	}
}

void MTGS::PrepDataPacket(Command cmd, u32 size)
{
	s_packet_size = size;
	++size;
	GenericStall(size);

	const unsigned int local_WritePos = s_WritePos.load(std::memory_order_relaxed);

	PacketTagType& tag = (PacketTagType&)RingBuffer[local_WritePos];
	tag.command = static_cast<u32>(cmd);
	tag.data[0] = s_packet_size;
	s_packet_startpos = local_WritePos;
	s_packet_writepos = (local_WritePos + 1) & RingBufferMask;
}

void MTGS::PrepDataPacket(GIF_PATH pathidx, u32 size)
{

	PrepDataPacket(static_cast<Command>(pathidx), size);
}

__fi void MTGS::_FinishSimplePacket()
{
	uint future_writepos = (s_WritePos.load(std::memory_order_relaxed) + 1) & RingBufferMask;
	pxAssert(future_writepos != s_ReadPos.load(std::memory_order_acquire));
	s_WritePos.store(future_writepos, std::memory_order_release);

	if (IsDevBuild && EmuConfig.GS.SynchronousMTGS) [[unlikely]]
		WaitGS();
	else
		++s_CopyDataTally;
}

void MTGS::SendSimplePacket(Command type, int data0, int data1, int data2)
{

	GenericStall(1);
	PacketTagType& tag = (PacketTagType&)RingBuffer[s_WritePos.load(std::memory_order_relaxed)];

	tag.command = static_cast<u32>(type);
	tag.data[0] = data0;
	tag.data[1] = data1;
	tag.data[2] = data2;

	_FinishSimplePacket();
}

void MTGS::SendSimpleGSPacket(Command type, u32 offset, u32 size, GIF_PATH path)
{
	SendSimplePacket(type, (int)offset, (int)size, (int)path);

	if (!IsDevBuild || !EmuConfig.GS.SynchronousMTGS) [[likely]]
	{
		s_CopyDataTally += size / 16;
		if (s_CopyDataTally > 0x2000)
			SetEvent();
	}
}

void MTGS::SendPointerPacket(Command type, u32 data0, void* data1)
{

	GenericStall(1);
	PacketTagType& tag = (PacketTagType&)RingBuffer[s_WritePos.load(std::memory_order_relaxed)];

	tag.command = static_cast<u32>(type);
	tag.data[0] = data0;
	tag.pointer = (uptr)data1;

	_FinishSimplePacket();
}

bool MTGS::WaitForOpen()
{
	if (IsOpen())
		return true;

	StartThread();

	s_open_flag.store(true, std::memory_order_release);
	s_sem_event.NotifyOfWork();

	s_open_or_close_done.Wait();

	const bool result = s_open_flag.load(std::memory_order_acquire);
	if (!result)
		Console.Error("GS failed to open.");

	return result;
}

void MTGS::WaitForClose()
{
	if (!IsOpen())
		return;

	s_open_flag.store(false, std::memory_order_release);

	s_sem_event.NotifyOfWork();

	s_open_or_close_done.Wait();
}

void MTGS::Freeze(FreezeAction mode, MTGS::FreezeData& data)
{
	pxAssertRel(IsOpen(), "GS thread is open");

	if (mode == FreezeAction::Load)
		WaitGS(true);

	SendPointerPacket(Command::Freeze, (int)mode, &data);
	WaitGS(false);
}

void MTGS::RunOnGSThread(AsyncCallType func)
{
	SendPointerPacket(Command::AsyncCall, 0, new AsyncCallType(std::move(func)));

	SetEvent();
}

void MTGS::GameChanged()
{
	pxAssertRel(IsOpen(), "MTGS is running");
	RunOnGSThread(GSGameChanged);
}

void MTGS::ApplySettings()
{
	pxAssertRel(IsOpen(), "MTGS is running");

	RunOnGSThread([opts = EmuConfig.GS]() {
		GSUpdateConfig(opts);
	});

	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false, false);
}

void MTGS::ResizeDisplayWindow(u32 width, u32 height, float scale)
{
	pxAssertRel(IsOpen(), "MTGS is running");
	RunOnGSThread([width, height, scale]() {
		GSResizeDisplayWindow(width, height, scale);

		if (VMManager::GetState() == VMState::Paused)
			GSPresentCurrentFrame();
	});
}

void MTGS::UpdateDisplayWindow()
{
	pxAssertRel(IsOpen(), "MTGS is running");
	RunOnGSThread([]() {
		GSUpdateDisplayWindow();

		if (VMManager::GetState() == VMState::Paused)
		{
			GSPresentCurrentFrame();
			GSPresentCurrentFrame();
		}
	});
}

void MTGS::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	pxAssertRel(IsOpen(), "MTGS is running");

	RunOnGSThread([mode, allow_present_throttle]() { GSSetVSyncMode(mode, allow_present_throttle); });
}

void MTGS::UpdateVSyncMode()
{
	SetVSyncMode(VMManager::GetEffectiveVSyncMode(), VMManager::ShouldAllowPresentThrottle());
}

void MTGS::SetSoftwareRendering(bool software, GSInterlaceMode interlace, bool display_message )
{
	pxAssertRel(IsOpen(), "MTGS is running");

	if (display_message)
	{
		Host::AddIconOSDMessage("SwitchRenderer", ICON_FA_WAND_MAGIC_SPARKLES, software ?
			TRANSLATE_STR("GS", "Switching to Software Renderer...") : TRANSLATE_STR("GS", "Switching to Hardware Renderer..."),
			Host::OSD_QUICK_DURATION);
	}

	RunOnGSThread([software, interlace]() {
		GSSetSoftwareRendering(software, interlace);
	});

	if (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Unsynchronized)
		WaitGS(false, false, false);
}

void MTGS::ToggleSoftwareRendering()
{
	SetSoftwareRendering(GSIsHardwareRenderer(), EmuConfig.GS.InterlaceMode);
}

bool MTGS::SaveMemorySnapshot(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
	u32* width, u32* height, std::vector<u32>* pixels)
{
	bool result = false;
	RunOnGSThread([window_width, window_height, apply_aspect, crop_borders, width, height, pixels, &result]() {
		result = GSSaveSnapshotToMemory(window_width, window_height, apply_aspect, crop_borders, width, height, pixels);
	});
	WaitGS(false, false, false);
	return result;
}

void MTGS::PresentCurrentFrame()
{
	if (s_run_idle_flag.load(std::memory_order_relaxed))
	{
		return;
	}

	RunOnGSThread([]() {
		GSPresentCurrentFrame();
	});
}

void MTGS::SetRunIdle(bool enabled)
{
	s_run_idle_flag.store(enabled, std::memory_order_release);
}

void Gif_AddGSPacketMTVU(GS_Packet& gsPack, GIF_PATH path)
{
	MTGS::SendSimpleGSPacket(MTGS::Command::MTVUGSPacket, 0, 0, path);
}

void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
{
	if (COPY_GS_PACKET_TO_MTGS)
	{
		MTGS::PrepDataPacket(path, gsPack.size / 16);
		MemCopy_WrappedDest((u128*)&gifUnit.gifPath[path].buffer[gsPack.offset], MTGS::RingBuffer.m_Ring,
							MTGS::s_packet_writepos, MTGS::RingBufferSize, gsPack.size / 16);
		MTGS::SendDataPacket();
	}
	else
	{
		pxAssertMsg(!gsPack.readAmount, "Gif Unit - gsPack.readAmount only valid for MTVU path 1!");
		gifUnit.gifPath[path].readAmount.fetch_add(gsPack.size);
		MTGS::SendSimpleGSPacket(MTGS::Command::GSPacket, gsPack.offset, gsPack.size, path);
	}
}

void Gif_AddBlankGSPacket(u32 size, GIF_PATH path)
{
	gifUnit.gifPath[path].readAmount.fetch_add(size);
	MTGS::SendSimpleGSPacket(MTGS::Command::GSPacket, ~0u, size, path);
}

void Gif_MTGS_Wait(bool isMTVU)
{
	MTGS::WaitGS(false, true, isMTVU);
}
