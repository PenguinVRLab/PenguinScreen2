// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS.h"

#include "common/Threading.h"

#include <functional>

namespace MTGS
{
	using AsyncCallType = std::function<void()>;

	enum class Command : u32
	{
		GIFPath1,
		GIFPath2,
		GIFPath3,
		VSync,
		Freeze,
		Reset,
		SoftReset,
		GSPacket,
		MTVUGSPacket,
		InitAndReadFIFO,
		AsyncCall,
	};

	struct FreezeData
	{
		freezeData* fdata;
		s32 retval;
	};

	const Threading::ThreadHandle& GetThreadHandle();
	bool IsOpen();

	void StartThread();

	void ShutdownThread();

	void PresentCurrentFrame();

	void WaitGS(bool syncRegs = true, bool weakWait = false, bool isMTVU = false);
	void ResetGS(bool hardware_reset);

	bool WaitForOpen();
	void WaitForClose();
	void Freeze(FreezeAction mode, FreezeData& data);

	int GetCurrentVsyncQueueSize();
	void PostVsyncStart(bool registers_written);
	void InitAndReadFIFO(u8* mem, u32 qwc);

	void RunOnGSThread(AsyncCallType func);
	void GameChanged();
	void ApplySettings();
	void ResizeDisplayWindow(u32 width, u32 height, float scale);
	void UpdateDisplayWindow();
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle);
	void UpdateVSyncMode();
	void SetSoftwareRendering(bool software, GSInterlaceMode interlace, bool display_message = true);
	void ToggleSoftwareRendering();
	bool SaveMemorySnapshot(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
		u32* width, u32* height, std::vector<u32>* pixels);
	void SetRunIdle(bool enabled);

	static const uint RingBufferSizeFactor = 19;

	static const uint RingBufferSize = 1 << RingBufferSizeFactor;

	static const uint RingBufferMask = RingBufferSize - 1;
}
