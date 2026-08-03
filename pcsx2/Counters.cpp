// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <time.h>
#include <cmath>

#include "Common.h"
#include "R3000A.h"
#include "Counters.h"
#include "IopCounters.h"

#include "GS.h"
#include "GS/GS.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "Patch.h"
#include "ps2/HwInternal.h"
#include "SIO/Sio.h"
#include "SPU2/spu2.h"
#include "Recording/InputRecording.h"
#include "VMManager.h"
#include "VUmicro.h"

static const uint EECNT_FUTURE_TARGET = 0x10000000;

uint g_FrameCount = 0;

Counter counters[4];
SyncCounter hsyncCounter;
SyncCounter vsyncCounter;

u64 nextStartCounter;
s32 nextDeltaCounter;

static void rcntStartGate(bool mode, u64 sCycle);
static void rcntEndGate(bool mode, u64 sCycle);
static void rcntWcount(int index, u32 value);
static void rcntWmode(int index, u32 value);
static void rcntWtarget(int index, u32 value);
static void rcntWhold(int index, u32 value);

static bool IsInterlacedVideoMode()
{
	return (gsVideoMode == GS_VideoMode::PAL || gsVideoMode == GS_VideoMode::NTSC || gsVideoMode == GS_VideoMode::DVD_NTSC || gsVideoMode == GS_VideoMode::DVD_PAL || gsVideoMode == GS_VideoMode::HDTV_1080I);
}

static bool IsProgressiveVideoMode()
{
	return !(*(u32*)PS2GS_BASE(GS_SYNCV) & 0x1) || !(*(u32*)PS2GS_BASE(GS_SMODE1) & 0x6000);
}

void rcntReset(int index)
{
	counters[index].count = 0;
	counters[index].startCycle = cpuRegs.cycle;
}

static __fi void _rcntSet(int cntidx)
{
	s32 c;
	pxAssume(cntidx <= 4);

	const Counter& counter = counters[cntidx];

	if (!rcntCanCount(cntidx) || (counter.mode.ClockSource == 0x3))
		return;

	if (!counter.mode.TargetInterrupt && !counter.mode.OverflowInterrupt && !counter.mode.ZeroReturn)
		return;
	if (counter.count > 0x10000 || counter.count > counter.target)
	{
		nextDeltaCounter = 4;
		return;
	}

	c = ((0x10000 - counter.count) * counter.rate) - (cpuRegs.cycle - counter.startCycle);
	c += cpuRegs.cycle - nextStartCounter;

	if (c < nextDeltaCounter)
	{
		nextDeltaCounter = c;

		cpuSetNextEvent(nextStartCounter, nextDeltaCounter);
	}

	if (counter.target & EECNT_FUTURE_TARGET)
	{
		return;
	}
	else
	{

		c = ((counter.target - counter.count) * counter.rate) - (cpuRegs.cycle - counter.startCycle);
		c += cpuRegs.cycle - nextStartCounter;

		if (c < nextDeltaCounter)
		{
			nextDeltaCounter = c;
			cpuSetNextEvent(nextStartCounter, nextDeltaCounter);
		}
	}
}


static __fi void cpuRcntSet()
{
	int i;

	nextStartCounter = cpuRegs.cycle;
	nextDeltaCounter = vsyncCounter.deltaCycles - (cpuRegs.cycle - vsyncCounter.startCycle);

	s32 nextHsync = hsyncCounter.deltaCycles - (cpuRegs.cycle - hsyncCounter.startCycle);
	if (nextHsync < nextDeltaCounter)
		nextDeltaCounter = nextHsync;

	for (i = 0; i < 4; i++)
		_rcntSet(i);

	if (nextDeltaCounter < 0)
		nextDeltaCounter = 0;

	cpuSetNextEvent(nextStartCounter, nextDeltaCounter);
}


struct vSyncTimingInfo
{
	double Framerate;
	GS_VideoMode VideoMode;
	u32 Render;
	u32 Blank;

	u32 GSBlank;

	u32 hSyncError;
	u32 hRender;
	u32 hBlank;
	u32 hScanlinesPerFrame;
};

static vSyncTimingInfo vSyncInfo;

void rcntInit()
{
	int i;

	g_FrameCount = 0;

	std::memset(counters, 0, sizeof(counters));

	for (i = 0; i < 4; i++)
	{
		counters[i].rate = 2;
		counters[i].target = 0xffff;
	}
	counters[0].interrupt = 9;
	counters[1].interrupt = 10;
	counters[2].interrupt = 11;
	counters[3].interrupt = 12;

	std::memset(&vSyncInfo, 0, sizeof(vSyncInfo));

	gsVideoMode = GS_VideoMode::Uninitialized;
	gsIsInterlaced = VMManager::Internal::IsFastBootInProgress();

	hsyncCounter.Mode = MODE_HRENDER;
	hsyncCounter.startCycle = cpuRegs.cycle;
	hsyncCounter.deltaCycles = vSyncInfo.hRender;
	vsyncCounter.Mode = MODE_VRENDER;
	vsyncCounter.deltaCycles = vSyncInfo.Render;
	vsyncCounter.startCycle = cpuRegs.cycle;

	for (i = 0; i < 4; i++)
		rcntReset(i);
	cpuRcntSet();
}

static void vSyncInfoCalc(vSyncTimingInfo* info, double framesPerSecond, u32 scansPerFrame)
{
	constexpr double clock = static_cast<double>(PS2CLK);

	const u64 Frame = clock * 10000ULL / framesPerSecond;
	const u64 Scanline = Frame / scansPerFrame;

	const bool ntsc_hblank = gsVideoMode != GS_VideoMode::PAL && gsVideoMode != GS_VideoMode::DVD_PAL;
	const u64 HalfFrame = Frame / 2;
	const float extra_scanlines = static_cast<float>(IsProgressiveVideoMode()) * (ntsc_hblank ? 0.5f : 1.5f);
	const u64 Blank = Scanline * ((ntsc_hblank ? 22.5f : 24.5f) + extra_scanlines);
	const u64 Render = HalfFrame - Blank;
	const u64 GSBlank = Scanline * ((ntsc_hblank ? 3.5 : 3) + extra_scanlines);

	u64 hRender = Scanline * 0.8368298368298368f;
	u64 hBlank = Scanline - hRender;

	if (!IsInterlacedVideoMode())
	{
		hBlank /= 2;
		hRender /= 2;
	}

	info->Framerate = framesPerSecond;
	info->GSBlank = (u32)(GSBlank / 10000);
	info->Render = (u32)(Render / 10000);
	info->Blank = (u32)(Blank / 10000);
	const u64 accumilated_vrender = (Render % 10000) + (Blank % 10000);
	info->Render += (u32)(accumilated_vrender / 10000);

	info->hRender = (u32)(hRender / 10000);
	info->hBlank = (u32)(hBlank / 10000);
	info->hScanlinesPerFrame = scansPerFrame;

	const u64 accumilatedHRenderError = (hRender % 10000) + (hBlank % 10000);
	const u64 accumilatedHFractional = accumilatedHRenderError % 10000;
	info->hRender += (u32)(accumilatedHRenderError / 10000);
	info->hSyncError = (u32)((accumilatedHFractional * (scansPerFrame / (IsInterlacedVideoMode() ? 2 : 1))) / 10000);

}

const char* ReportVideoMode()
{
	switch (gsVideoMode)
	{
	case GS_VideoMode::PAL:          return "PAL";
	case GS_VideoMode::NTSC:         return "NTSC";
	case GS_VideoMode::DVD_NTSC:     return "DVD NTSC";
	case GS_VideoMode::DVD_PAL:      return "DVD PAL";
	case GS_VideoMode::VESA:         return "VESA";
	case GS_VideoMode::SDTV_480P:    return "SDTV 480p";
	case GS_VideoMode::SDTV_576P:    return "SDTV 576p";
	case GS_VideoMode::HDTV_720P:    return "HDTV 720p";
	case GS_VideoMode::HDTV_1080I:   return "HDTV 1080i";
	case GS_VideoMode::HDTV_1080P:   return "HDTV 1080p";
	default:                         return "Unknown";
	}
}

const char* ReportInterlaceMode()
{
	const u64& smode2 = *(u64*)PS2GS_BASE(GS_SMODE2);
	return !IsProgressiveVideoMode() ? ((smode2 & 2) ? "Interlaced (Frame)" : "Interlaced (Field)") : "Progressive";
}

double GetVerticalFrequency()
{

	switch (gsVideoMode)
	{
		case GS_VideoMode::Uninitialized:
			return 60.00;
		case GS_VideoMode::PAL:
		case GS_VideoMode::DVD_PAL:
			return (IsProgressiveVideoMode() == false) ? EmuConfig.GS.FrameratePAL : EmuConfig.GS.FrameratePAL - 0.24f;
		case GS_VideoMode::NTSC:
		case GS_VideoMode::DVD_NTSC:
			return (IsProgressiveVideoMode() == false) ? EmuConfig.GS.FramerateNTSC : EmuConfig.GS.FramerateNTSC - 0.11f;
		case GS_VideoMode::SDTV_480P:
			return 59.94;
		case GS_VideoMode::HDTV_1080P:
		case GS_VideoMode::HDTV_1080I:
		case GS_VideoMode::HDTV_720P:
		case GS_VideoMode::SDTV_576P:
		case GS_VideoMode::VESA:
			return 60.00;
		default:
			return FRAMERATE_NTSC * 2;
	}
}

void UpdateVSyncRate(bool force)
{

	const double vertical_frequency = GetVerticalFrequency();

	const double frames_per_second = vertical_frequency / 2.0;

	if (vSyncInfo.Framerate != frames_per_second || vSyncInfo.VideoMode != gsVideoMode || force)
	{
		u32 total_scanlines = 0;
		bool custom = false;

		switch (gsVideoMode)
		{
			case GS_VideoMode::Uninitialized:
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_NTSC_I;
				else
					total_scanlines = SCANLINES_TOTAL_NTSC_NI;
				break;
			case GS_VideoMode::PAL:
			case GS_VideoMode::DVD_PAL:
				custom = (EmuConfig.GS.FrameratePAL != Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_PAL);
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_PAL_I;
				else
					total_scanlines = SCANLINES_TOTAL_PAL_NI;
				break;
			case GS_VideoMode::NTSC:
			case GS_VideoMode::DVD_NTSC:
				custom = (EmuConfig.GS.FramerateNTSC != Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC);
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_NTSC_I;
				else
					total_scanlines = SCANLINES_TOTAL_NTSC_NI;
				break;
			case GS_VideoMode::SDTV_480P:
			case GS_VideoMode::SDTV_576P:
			case GS_VideoMode::HDTV_720P:
			case GS_VideoMode::VESA:
				total_scanlines = SCANLINES_TOTAL_NTSC_I;
				break;
			case GS_VideoMode::HDTV_1080P:
			case GS_VideoMode::HDTV_1080I:
				total_scanlines = SCANLINES_TOTAL_1080;
				break;
			case GS_VideoMode::Unknown:
			default:
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_NTSC_I;
				else
					total_scanlines = SCANLINES_TOTAL_NTSC_NI;
				Console.Error("PCSX2-Counters: Unknown video mode detected");
				pxAssertMsg(false, "Unknown video mode detected via SetGsCrt");
		}

		const bool video_mode_initialized = gsVideoMode != GS_VideoMode::Uninitialized;

		if (video_mode_initialized && vSyncInfo.VideoMode != gsVideoMode)
			CSRreg.FIELD = 1;

		vSyncInfo.VideoMode = gsVideoMode;

		vSyncInfoCalc(&vSyncInfo, frames_per_second, total_scanlines);

		if (video_mode_initialized)
			Console.WriteLn(Color_Green, "UpdateVSyncRate: Mode Changed to %s.", ReportVideoMode());

		if (custom && video_mode_initialized)
			Console.WriteLn(Color_StrongGreen, "  ... with user configured refresh rate: %.02f Hz", vertical_frequency);

		s32 hdiff = hsyncCounter.deltaCycles;
		s32 vdiff = vsyncCounter.deltaCycles;
		hsyncCounter.deltaCycles = (hsyncCounter.Mode == MODE_HBLANK) ? vSyncInfo.hBlank : vSyncInfo.hRender;
		vsyncCounter.deltaCycles = (vsyncCounter.Mode == MODE_GSBLANK) ?
		                               vSyncInfo.GSBlank :
		                               ((vsyncCounter.Mode == MODE_VBLANK) ? vSyncInfo.Blank : vSyncInfo.Render);

		hsyncCounter.startCycle += hdiff - hsyncCounter.deltaCycles;
		vsyncCounter.startCycle += vdiff - vsyncCounter.deltaCycles;

		cpuRcntSet();

		VMManager::Internal::FrameRateChanged();
	}
}

extern u64 eecount_on_last_vdec;
extern bool FMVstarted;
extern bool EnableFMV;

static bool s_last_fmv_state = false;

static __fi void DoFMVSwitch()
{
	bool new_fmv_state = s_last_fmv_state;
	if (EnableFMV)
	{
		DevCon.WriteLn("FMV started");
		new_fmv_state = true;
		EnableFMV = false;
	}
	else if (FMVstarted)
	{
		const int diff = cpuRegs.cycle - eecount_on_last_vdec;
		if (diff > 60000000)
		{
			DevCon.WriteLn("FMV ended");
			new_fmv_state = false;
			FMVstarted = false;
		}
	}

	if (new_fmv_state == s_last_fmv_state)
		return;

	s_last_fmv_state = new_fmv_state;

	switch (EmuConfig.GS.FMVAspectRatioSwitch)
	{
		case FMVAspectRatioSwitchType::Off:
			break;
		case FMVAspectRatioSwitchType::RAuto4_3_3_2:
			EmuConfig.CurrentAspectRatio = new_fmv_state ? AspectRatioType::RAuto4_3_3_2 : EmuConfig.GS.AspectRatio;
			break;
		case FMVAspectRatioSwitchType::R4_3:
			EmuConfig.CurrentAspectRatio = new_fmv_state ? AspectRatioType::R4_3 : EmuConfig.GS.AspectRatio;
			break;
		case FMVAspectRatioSwitchType::R16_9:
			EmuConfig.CurrentAspectRatio = new_fmv_state ? AspectRatioType::R16_9 : EmuConfig.GS.AspectRatio;
			break;
		case FMVAspectRatioSwitchType::R10_7:
			EmuConfig.CurrentAspectRatio = new_fmv_state ? AspectRatioType::R10_7 : EmuConfig.GS.AspectRatio;
			break;
		default:
			break;
	}

	if (EmuConfig.Gamefixes.SoftwareRendererFMVHack && EmuConfig.GS.UseHardwareRenderer())
	{
		DevCon.Warning("FMV Switch");
		MTGS::SetSoftwareRendering(new_fmv_state, new_fmv_state ? GSInterlaceMode::AdaptiveTFF : EmuConfig.GS.InterlaceMode, false);
	}
}

static __fi void VSyncStart(u64 sCycle)
{
	DoFMVSwitch();
	VMManager::Internal::VSyncOnCPUThread();

	if (!VMManager::Internal::IsExecutionInterrupted())
		VMManager::Internal::Throttle();

	gsPostVsyncStart();

	VMManager::Internal::PollInputOnCPUThread();

	EECNT_LOG("    ================  EE COUNTER VSYNC START (frame: %d)  ================", g_FrameCount);

	AutoEject::CountDownTicks();
	MemcardBusy::Decrement();

	if (!GSSMODE1reg.SINT)
	{
		hwIntcIrq(INTC_VBLANK_S);
		rcntStartGate(true, sCycle);
		psxVBlankStart();
	}

	if (VMManager::Internal::IsExecutionInterrupted())
		Cpu->ExitExecution();
}

static __fi void GSVSync()
{
	if (GSSMODE1reg.SINT)
		return;

	if (IsProgressiveVideoMode())
		CSRreg.SetField();
	else
		CSRreg.SwapField();

	if (!CSRreg.VSINT)
	{
		CSRreg.VSINT = true;
		if (!GSIMR.VSMSK)
			gsIrq();
	}
}

static __fi void VSyncEnd(u64 sCycle)
{
	EECNT_LOG("    ================  EE COUNTER VSYNC END (frame: %d)  ================", g_FrameCount);

	g_FrameCount++;
	if (!GSSMODE1reg.SINT)
	{
		hwIntcIrq(INTC_VBLANK_E);
		psxVBlankEnd();
		rcntEndGate(true, sCycle);
	}

}

#ifdef VSYNC_DEBUG
static u32 hsc = 0;
static int vblankinc = 0;
#endif

__fi void rcntUpdate_vSync()
{
	if (!cpuTestCycle(vsyncCounter.startCycle, vsyncCounter.deltaCycles))
		return;

	if (vsyncCounter.Mode == MODE_VBLANK)
	{
		vsyncCounter.startCycle += vSyncInfo.Blank;
		vsyncCounter.deltaCycles = vSyncInfo.Render;

		VSyncEnd(vsyncCounter.startCycle);

		vsyncCounter.Mode = MODE_VRENDER;
	}
	else if (vsyncCounter.Mode == MODE_GSBLANK)
	{
		GSVSync();

		vsyncCounter.Mode = MODE_VBLANK;
		vsyncCounter.deltaCycles = vSyncInfo.Blank;
	}
	else
	{
		vsyncCounter.startCycle += vSyncInfo.Render;
		vsyncCounter.deltaCycles = vSyncInfo.GSBlank;

		VSyncStart(vsyncCounter.startCycle);

		vsyncCounter.Mode = MODE_GSBLANK;

		hsyncCounter.deltaCycles += vSyncInfo.hSyncError;

#ifdef VSYNC_DEBUG
		vblankinc++;
		if (vblankinc > 1)
		{
			if (hsc != vSyncInfo.hScanlinesPerFrame)
				Console.WriteLn(" ** vSync > Abnormal Scanline Count: %d", hsc);
			hsc = 0;
			vblankinc = 0;
		}
#endif
	}
}

__fi void rcntUpdate_hScanline()
{
	if (!cpuTestCycle(hsyncCounter.startCycle, hsyncCounter.deltaCycles))
		return;

	if (hsyncCounter.Mode == MODE_HBLANK)
	{

		hsyncCounter.startCycle += vSyncInfo.hBlank;
		hsyncCounter.deltaCycles = vSyncInfo.hRender;
		if (!GSSMODE1reg.SINT)
		{
			rcntEndGate(false, hsyncCounter.startCycle);
			psxHBlankEnd();
		}

		hsyncCounter.Mode = MODE_HRENDER;
	}
	else
	{

		hsyncCounter.startCycle += vSyncInfo.hRender;
		hsyncCounter.deltaCycles = vSyncInfo.hBlank;
		if (!GSSMODE1reg.SINT)
		{
			if (!CSRreg.HSINT)
			{
				CSRreg.HSINT = true;
				if (!GSIMR.HSMSK)
					gsIrq();
			}

			rcntStartGate(false, hsyncCounter.startCycle);
			psxHBlankStart();
		}

		hsyncCounter.Mode = MODE_HBLANK;

#ifdef VSYNC_DEBUG
		hsc++;
#endif
	}
}

static __fi void _cpuTestTarget(int i)
{
	if (counters[i].count < counters[i].target)
		return;

	if (counters[i].mode.TargetInterrupt)
	{
		EECNT_LOG("EE Counter[%d] TARGET reached - mode=%x, count=%x, target=%x", i, counters[i].mode, counters[i].count, counters[i].target);
		if (!counters[i].mode.TargetReached)
		{
			counters[i].mode.TargetReached = 1;
			hwIntcIrq(counters[i].interrupt);
		}
	}

	if (counters[i].mode.ZeroReturn)
		counters[i].count -= counters[i].target;
	else
		counters[i].target |= EECNT_FUTURE_TARGET;
}

static __fi void _cpuTestOverflow(int i)
{
	if (counters[i].count <= 0xffff)
		return;

	if (counters[i].mode.OverflowInterrupt)
	{
		EECNT_LOG("EE Counter[%d] OVERFLOW - mode=%x, count=%x", i, counters[i].mode, counters[i].count);
		if (!counters[i].mode.OverflowReached)
		{
			counters[i].mode.OverflowReached = 1;
			hwIntcIrq(counters[i].interrupt);
		}
	}

	counters[i].count -= 0x10000;
	counters[i].target &= 0xffff;
}


__fi bool rcntCanCount(int i)
{
	if (!counters[i].mode.IsCounting)
		return false;

	if (!counters[i].mode.EnableGate)
		return true;

	return ((counters[i].mode.GateSource == 0 && counters[i].mode.ClockSource != 3 && (hsyncCounter.Mode == MODE_HRENDER || counters[i].mode.GateMode != 0)) ||
			(counters[i].mode.GateSource == 1 && (vsyncCounter.Mode == MODE_VRENDER || counters[i].mode.GateMode != 0)));
}

__fi void rcntSyncCounter(int i)
{
	if (counters[i].mode.ClockSource != 0x3)
	{
		const u32 change = (cpuRegs.cycle - counters[i].startCycle) / counters[i].rate;
		counters[i].startCycle += change * counters[i].rate;

		counters[i].startCycle &= ~((u64)counters[i].rate - 1);

		if (rcntCanCount(i))
			counters[i].count += change;
	}
	else
		counters[i].startCycle = cpuRegs.cycle;
}

__fi void rcntUpdate()
{
	rcntUpdate_vSync();
	rcntUpdate_hScanline();

	for (int i = 0; i <= 3; i++)
	{
		rcntSyncCounter(i);

		if (counters[i].mode.ClockSource == 0x3 || !rcntCanCount(i))
			continue;

		_cpuTestOverflow(i);
		_cpuTestTarget(i);
	}

	cpuRcntSet();
}

static __fi void _rcntSetGate(int index)
{
	if (counters[index].mode.EnableGate)
	{
		if (!(counters[index].mode.GateSource == 0 && counters[index].mode.ClockSource == 3))
			EECNT_LOG("EE Counter[%d] Using Gate!  Source=%s, Mode=%d.",
				index, counters[index].mode.GateSource ? "vblank" : "hblank", counters[index].mode.GateMode);
		else
			EECNT_LOG("EE Counter[%d] GATE DISABLED because of hblank source.", index);
	}
}

static __fi void rcntStartGate(bool isVblank, u64 sCycle)
{
	for (int i = 0; i < 4; i++)
	{
		if (!isVblank && (counters[i].mode.ClockSource == 3) && rcntCanCount(i))
		{
			counters[i].count += HBLANK_COUNTER_SPEED;
			_cpuTestOverflow(i);
			_cpuTestTarget(i);
		}

		if (!counters[i].mode.EnableGate)
			continue;

		if ((!!counters[i].mode.GateSource) != isVblank)
			continue;

		switch (counters[i].mode.GateMode)
		{
			case 0x0:

				rcntSyncCounter(i);
				counters[i].startCycle = sCycle & ~((u64)counters[i].rate - 1);
				EECNT_LOG("EE Counter[%d] %s StartGate Type0, count = %x", i,
					isVblank ? "vblank" : "hblank", counters[i].count);
				break;
			case 0x2:
				break;
			case 0x1:
			case 0x3:
				rcntSyncCounter(i);
				counters[i].count = 0;
				counters[i].target &= 0xffff;
				counters[i].startCycle = sCycle & ~((u64)counters[i].rate - 1);
				EECNT_LOG("EE Counter[%d] %s StartGate Type%d, count = %x", i,
					isVblank ? "vblank" : "hblank", counters[i].mode.GateMode, counters[i].count);
				break;
		}
	}

}

static __fi void rcntEndGate(bool isVblank, u64 sCycle)
{
	for (int i = 0; i < 4; i++)
	{
		if (!counters[i].mode.EnableGate)
			continue;

		if ((!!counters[i].mode.GateSource) != isVblank)
			continue;

		switch (counters[i].mode.GateMode)
		{
			case 0x0:
				counters[i].startCycle = sCycle & ~((u64)counters[i].rate - 1);

				EECNT_LOG("EE Counter[%d] %s EndGate Type0, count = %x", i,
					isVblank ? "vblank" : "hblank", counters[i].count);
				break;

			case 0x1:
				break;

			case 0x2:
			case 0x3:
				rcntSyncCounter(i);
				EECNT_LOG("EE Counter[%d]  %s EndGate Type%d, count = %x", i,
					isVblank ? "vblank" : "hblank", counters[i].mode.GateMode, counters[i].count);
				counters[i].count = 0;
				counters[i].target &= 0xffff;
				counters[i].startCycle = sCycle & ~(static_cast<u64>(counters[i].rate - 1));
				break;
		}
	}
}

static __fi void rcntWmode(int index, u32 value)
{
	rcntSyncCounter(index);

	counters[index].modeval &= ~(value & 0xc00);
	counters[index].modeval = (counters[index].modeval & 0xc00) | (value & 0x3ff);
	EECNT_LOG("EE Counter[%d] writeMode = %x passed value=%x", index, counters[index].modeval, value);

	switch (counters[index].mode.ClockSource) {
		case 0: counters[index].rate = 2; break;
		case 1: counters[index].rate = 32; break;
		case 2: counters[index].rate = 512; break;
		case 3: counters[index].rate = vSyncInfo.hBlank+vSyncInfo.hRender; break;
	}

	counters[index].startCycle = cpuRegs.cycle & ~((u64)counters[index].rate - 1);
	_rcntSetGate(index);
	_rcntSet(index);
}

static __fi void rcntWcount(int index, u32 value)
{
	EECNT_LOG("EE Counter[%d] writeCount = %x,   oldcount=%x, target=%x", index, value, counters[index].count, counters[index].target);

	rcntSyncCounter(index);

	counters[index].count = value & 0xffff;

	counters[index].target &= 0xffff;

	if (counters[index].count >= counters[index].target)
		counters[index].target |= EECNT_FUTURE_TARGET;

	_rcntSet(index);
}

static __fi void rcntWtarget(int index, u32 value)
{
	EECNT_LOG("EE Counter[%d] writeTarget = %x", index, value);

	counters[index].target = value & 0xffff;

	rcntSyncCounter(index);

	if (counters[index].target <= counters[index].count)
		counters[index].target |= EECNT_FUTURE_TARGET;

	_rcntSet(index);
}

static __fi void rcntWhold(int index, u32 value)
{
	EECNT_LOG("EE Counter[%d] Hold Write = %x", index, value);
	counters[index].hold = value;
}

__fi u32 rcntRcount(int index)
{
	u32 ret;

	rcntSyncCounter(index);

	ret = counters[index].count;

	EECNT_LOG("EE Counter[%d] readCount32 = %x", index, ret);
	return (u16)ret;
}

template <uint page>
__fi u16 rcntRead32(u32 mem)
{

	switch( mem )
	{
		case(RCNT0_COUNT):	return (u16)rcntRcount(0);
		case(RCNT0_MODE):	return (u16)counters[0].modeval;
		case(RCNT0_TARGET):	return (u16)counters[0].target;
		case(RCNT0_HOLD):	return (u16)counters[0].hold;

		case(RCNT1_COUNT):	return (u16)rcntRcount(1);
		case(RCNT1_MODE):	return (u16)counters[1].modeval;
		case(RCNT1_TARGET):	return (u16)counters[1].target;
		case(RCNT1_HOLD):	return (u16)counters[1].hold;

		case(RCNT2_COUNT):	return (u16)rcntRcount(2);
		case(RCNT2_MODE):	return (u16)counters[2].modeval;
		case(RCNT2_TARGET):	return (u16)counters[2].target;

		case(RCNT3_COUNT):	return (u16)rcntRcount(3);
		case(RCNT3_MODE):	return (u16)counters[3].modeval;
		case(RCNT3_TARGET):	return (u16)counters[3].target;
	}

	return psHu16(mem);
}

template <uint page>
__fi bool rcntWrite32(u32 mem, mem32_t& value)
{
	pxAssume(mem >= RCNT0_COUNT && mem < 0x10002000);

	switch( mem )
	{
		case(RCNT0_COUNT):	return rcntWcount(0, value),	false;
		case(RCNT0_MODE):	return rcntWmode(0, value),		false;
		case(RCNT0_TARGET):	return rcntWtarget(0, value),	false;
		case(RCNT0_HOLD):	return rcntWhold(0, value),		false;

		case(RCNT1_COUNT):	return rcntWcount(1, value),	false;
		case(RCNT1_MODE):	return rcntWmode(1, value),		false;
		case(RCNT1_TARGET):	return rcntWtarget(1, value),	false;
		case(RCNT1_HOLD):	return rcntWhold(1, value),		false;

		case(RCNT2_COUNT):	return rcntWcount(2, value),	false;
		case(RCNT2_MODE):	return rcntWmode(2, value),		false;
		case(RCNT2_TARGET):	return rcntWtarget(2, value),	false;

		case(RCNT3_COUNT):	return rcntWcount(3, value),	false;
		case(RCNT3_MODE):	return rcntWmode(3, value),		false;
		case(RCNT3_TARGET):	return rcntWtarget(3, value),	false;
	}

	return true;
}

template u16 rcntRead32<0x00>(u32 mem);
template u16 rcntRead32<0x01>(u32 mem);

template bool rcntWrite32<0x00>(u32 mem, mem32_t& value);
template bool rcntWrite32<0x01>(u32 mem, mem32_t& value);

bool SaveStateBase::rcntFreeze()
{
	Freeze(counters);
	Freeze(hsyncCounter);
	Freeze(vsyncCounter);
	Freeze(nextDeltaCounter);
	Freeze(nextStartCounter);
	Freeze(vSyncInfo);
	Freeze(gsVideoMode);
	Freeze(gsIsInterlaced);

	if (IsLoading())
		cpuRcntSet();

	return IsOkay();
}
