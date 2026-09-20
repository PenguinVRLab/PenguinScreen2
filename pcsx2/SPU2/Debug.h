// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"

#include "SPU2/defs.h"

namespace SPU2
{
#ifdef PCSX2_DEVBUILD
	__fi static bool MsgToConsole()
	{
		return EmuConfig.SPU2.DebugEnabled;
	}

	__fi static bool MsgKeyOnOff() { return EmuConfig.SPU2.MsgKeyOnOff; }
	__fi static bool MsgVoiceOff() { return EmuConfig.SPU2.MsgVoiceOff; }
	__fi static bool MsgDMA() { return EmuConfig.SPU2.MsgDMA; }
	__fi static bool MsgAutoDMA() { return EmuConfig.SPU2.MsgAutoDMA; }
	__fi static bool MsgCache() { return EmuConfig.SPU2.MsgCache; }

	__fi static bool AccessLog() { return EmuConfig.SPU2.AccessLog; }
	__fi static bool DMALog() { return EmuConfig.SPU2.DMALog; }
	__fi static bool WaveLog() { return EmuConfig.SPU2.WaveLog; }

	__fi static bool CoresDump() { return EmuConfig.SPU2.CoresDump; }
	__fi static bool MemDump() { return EmuConfig.SPU2.MemDump; }
	__fi static bool RegDump() { return EmuConfig.SPU2.RegDump; }
	__fi static bool VisualDebug() { return EmuConfig.SPU2.VisualDebugEnabled; }

	extern void OpenFileLog();
	extern void CloseFileLog();
	extern void FileLog(const char* fmt, ...);
	extern void ConLog(const char* fmt, ...);

	extern void DoFullDump();

	extern void WriteRegLog(const char* action, u32 rmem, u16 value);

#else
	__fi static constexpr bool MsgToConsole() { return false; }

	__fi static constexpr bool MsgKeyOnOff() { return false; }
	__fi static constexpr bool MsgVoiceOff() { return false; }
	__fi static constexpr bool MsgDMA() { return false; }
	__fi static constexpr bool MsgAutoDMA() { return false; }
	__fi static constexpr bool MsgOverruns() { return false; }
	__fi static constexpr bool MsgCache() { return false; }

	__fi static constexpr bool AccessLog() { return false; }
	__fi static constexpr bool DMALog() { return false; }
	__fi static constexpr bool WaveLog() { return false; }

	__fi static constexpr bool CoresDump() { return false; }
	__fi static constexpr bool MemDump() { return false; }
	__fi static constexpr bool RegDump() { return false; }
	__fi static constexpr bool VisualDebug() { return false; }

	__fi static void FileLog(const char* fmt, ...) {}
	__fi static void ConLog(const char* fmt, ...) {}
#endif
}

#ifdef PCSX2_DEVBUILD

namespace WaveDump
{
	enum CoreSourceType
	{
		CoreSrc_Input = 0,

		CoreSrc_DryVoiceMix,

		CoreSrc_WetVoiceMix,

		CoreSrc_PreReverb,

		CoreSrc_PostReverb,

		CoreSrc_External,

		CoreSrc_Count
	};

	extern void Open();
	extern void Close();
	extern void WriteCore(uint coreidx, CoreSourceType src, s16 left, s16 right);
	extern void WriteCore(uint coreidx, CoreSourceType src, const StereoOut32& sample);
}

using WaveDump::CoreSrc_DryVoiceMix;
using WaveDump::CoreSrc_External;
using WaveDump::CoreSrc_Input;
using WaveDump::CoreSrc_PostReverb;
using WaveDump::CoreSrc_PreReverb;
using WaveDump::CoreSrc_WetVoiceMix;

#endif
