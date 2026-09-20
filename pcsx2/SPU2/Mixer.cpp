// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/AudioStream.h"
#include "SPU2/Debug.h"
#include "SPU2/defs.h"
#include "SPU2/spu2.h"
#include "SPU2/interpolate_table.h"

#include "common/Assertions.h"

#define XAFLAG_LOOP_END (1ul << 0)
#define XAFLAG_LOOP (1ul << 1)
#define XAFLAG_LOOP_START (1ul << 2)

#if MULTI_ISA_COMPILE_ONCE
PcmCacheEntry pcm_cache_data[pcm_BlockCount];

int g_counter_cache_hits = 0;
int g_counter_cache_misses = 0;
int g_counter_cache_ignores = 0;
#endif

MULTI_ISA_UNSHARED_START

static const s32 tbl_XA_Factor[16][2] =
	{
		{0, 0},
		{60, 0},
		{115, -52},
		{98, -55},
		{122, -60}};

static void __forceinline XA_decode_block(s16* buffer, const s16* block, s32& prev1, s32& prev2)
{
	const s32 header = *block;
	const s32 shift = (header & 0xF) + 16;
	const int id = header >> 4 & 0xF;
	if (id > 4 && SPU2::MsgToConsole())
		SPU2::ConLog("* SPU2: Unknown ADPCM coefficients table id %d\n", id);
	const s32 pred1 = tbl_XA_Factor[id][0];
	const s32 pred2 = tbl_XA_Factor[id][1];

	const s8* blockbytes = (s8*)&block[1];
	const s8* blockend = &blockbytes[13];

	for (; blockbytes <= blockend; ++blockbytes)
	{
		s32 data = ((*blockbytes) << 28) & 0xF0000000;
		s32 pcm = (data >> shift) + (((pred1 * prev1) + (pred2 * prev2) + 32) >> 6);

		pcm = std::clamp<s32>(pcm, -0x8000, 0x7fff);
		*(buffer++) = pcm;

		data = ((*blockbytes) << 24) & 0xF0000000;
		s32 pcm2 = (data >> shift) + (((pred1 * pcm) + (pred2 * prev1) + 32) >> 6);

		pcm2 = std::clamp<s32>(pcm2, -0x8000, 0x7fff);
		*(buffer++) = pcm2;

		prev2 = pcm;
		prev1 = pcm2;
	}
}

static void __forceinline IncrementNextA(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	for (int i = 0; i < 2; i++)
	{
		if (Cores[i].IRQEnable && (vc.NextA == Cores[i].IRQA))
		{

			SetIrqCall(i);
		}
	}

	vc.NextA++;
	vc.NextA &= 0xFFFFF;
}

static __forceinline void GetNextDataBuffered(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	if (vc.SBuffer == nullptr)
	{
		const int cacheIdx = (vc.NextA & 0xFFFF8) / pcm_WordsPerBlock;
		PcmCacheEntry& cacheLine = pcm_cache_data[cacheIdx];
		vc.SBuffer = cacheLine.Sampledata;

		if (cacheLine.Validated && vc.Prev1 == cacheLine.Prev1 && vc.Prev2 == cacheLine.Prev2)
		{

			vc.Prev1 = vc.SBuffer[27];
			vc.Prev2 = vc.SBuffer[26];

			if (IsDevBuild)
				g_counter_cache_hits++;
		}
		else
		{
			if (vc.NextA >= SPU2_DYN_MEMLINE)
			{
				cacheLine.Validated = true;
				cacheLine.Prev1 = vc.Prev1;
				cacheLine.Prev2 = vc.Prev2;
			}

			if (IsDevBuild)
			{
				if (vc.NextA < SPU2_DYN_MEMLINE)
					g_counter_cache_ignores++;
				else
					g_counter_cache_misses++;
			}


			s16* memptr = GetMemPtr(vc.NextA & 0xFFFF8);
			XA_decode_block(vc.SBuffer, memptr, vc.Prev1, vc.Prev2);
		}
	}

	int sampleIdx = ((vc.NextA % pcm_WordsPerBlock) - 1) * 4;
	for (int i = 0; i < 4; i++)
	{
		vc.DecodeFifo[(vc.DecPosWrite + i) % 32] = vc.SBuffer[sampleIdx + i];
	}
}

static __forceinline s32 ApplyVolume(s32 data, s32 volume)
{
	return (volume * data) >> 15;
}

static __forceinline StereoOut32 ApplyVolume(const StereoOut32& data, const V_VolumeLR& volume)
{
	return StereoOut32(
		ApplyVolume(data.Left, volume.Left),
		ApplyVolume(data.Right, volume.Right));
}

static __forceinline StereoOut32 ApplyVolume(const StereoOut32& data, const V_VolumeSlideLR& volume)
{
	return StereoOut32(
		ApplyVolume(data.Left, volume.Left.Value),
		ApplyVolume(data.Right, volume.Right.Value));
}

static __forceinline void UpdateBlockHeader(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	for (int i = 0; i < 2; i++)
		if (Cores[i].IRQEnable && Cores[i].IRQA == (vc.NextA & 0xFFFF8))
			SetIrqCall(i);

	s16* memptr = GetMemPtr(vc.NextA & 0xFFFF8);
	vc.LoopFlags = *memptr >> 8;

	if ((vc.LoopFlags & XAFLAG_LOOP_START) && !vc.LoopMode)
	{
		vc.LoopStartA = vc.NextA & 0xFFFF8;
	}
}

static __forceinline void DecodeSamples(uint coreidx, uint voiceidx)
{
	V_Core& thiscore(Cores[coreidx]);
	V_Voice& vc(thiscore.Voices[voiceidx]);

	UpdateBlockHeader(thiscore, voiceidx);

	if (((int)(vc.DecPosWrite - vc.DecPosRead)) > 12) {
		return;
	}

	if (vc.ADSR.Phase > V_ADSR::PHASE_STOPPED)
	{
		GetNextDataBuffered(thiscore, voiceidx);
	}

	vc.DecPosWrite += 4;

	IncrementNextA(thiscore, voiceidx);
	if ((vc.NextA & 7) == 0)
	{
		if (vc.LoopFlags & XAFLAG_LOOP_END)
		{
			thiscore.Regs.ENDX |= (1 << voiceidx);
			vc.NextA = vc.LoopStartA;
			if (!(vc.LoopFlags & XAFLAG_LOOP))
			{
				vc.Stop();

				if (IsDevBuild)
				{
					if (SPU2::MsgVoiceOff())
						SPU2::ConLog("* SPU2: Voice Off by EndPoint: %d \n", voiceidx);
				}
			}
		}

		IncrementNextA(thiscore, voiceidx);
		vc.SBuffer = nullptr;
	}
}

static void __forceinline UpdatePitch(uint coreidx, uint voiceidx)
{
	V_Voice& vc(Cores[coreidx].Voices[voiceidx]);
	s32 pitch;

	if ((vc.Modulated == 0) || (voiceidx == 0))
		pitch = vc.Pitch;
	else
		pitch = std::clamp((vc.Pitch * (32768 + Cores[coreidx].Voices[voiceidx - 1].OutX)) >> 15, 0, 0x3fff);

	pitch = std::min(pitch, 0x3FFF);
	vc.SP += pitch;
}

static __forceinline void CalculateADSR(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	if (vc.ADSR.Phase == V_ADSR::PHASE_STOPPED)
	{
		vc.ADSR.Value = 0;
		return;
	}

	if (!vc.ADSR.Calculate(thiscore.Index | (voiceidx << 1)))
	{
		if (IsDevBuild)
		{
			if (SPU2::MsgVoiceOff())
				SPU2::ConLog("* SPU2: Voice Off by ADSR: %d \n", voiceidx);
		}
		vc.Stop();
	}

	pxAssume(vc.ADSR.Value >= 0);
}

static __forceinline void ConsumeSamples(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	int consumed = vc.SP >> 12;
	vc.SP &= 0xfff;
	vc.DecPosRead += consumed;
}

static __forceinline s32 GetVoiceValues(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	int phase = (vc.SP & 0x0ff0) >> 4;
	s32 out = 0;
	out += (interpTable[phase][0] * vc.DecodeFifo[(vc.DecPosRead + 0) % 32]) >> 15;
	out += (interpTable[phase][1] * vc.DecodeFifo[(vc.DecPosRead + 1) % 32]) >> 15;
	out += (interpTable[phase][2] * vc.DecodeFifo[(vc.DecPosRead + 2) % 32]) >> 15;
	out += (interpTable[phase][3] * vc.DecodeFifo[(vc.DecPosRead + 3) % 32]) >> 15;

	return out;
}

static __forceinline void UpdateNoise(V_Core& thiscore)
{
	static const uint8_t noise_add[64] = {
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1};

	static const uint16_t noise_freq_add[5] = {
		0, 84, 140, 180, 210};


	u32 level = 0x8000 >> (thiscore.NoiseClk >> 2);
	level <<= 16;

	thiscore.NoiseCnt += 0x10000;

	thiscore.NoiseCnt += noise_freq_add[thiscore.NoiseClk & 3];
	if ((thiscore.NoiseCnt & 0xffff) >= noise_freq_add[4])
	{
		thiscore.NoiseCnt += 0x10000;
		thiscore.NoiseCnt -= noise_freq_add[thiscore.NoiseClk & 3];
	}

	if (thiscore.NoiseCnt >= level)
	{
		while (thiscore.NoiseCnt >= level)
			thiscore.NoiseCnt -= level;

		thiscore.NoiseOut = (thiscore.NoiseOut << 1) | noise_add[(thiscore.NoiseOut >> 10) & 63];
	}
}

static __forceinline s32 GetNoiseValues(V_Core& thiscore)
{
	return (s16)thiscore.NoiseOut;
}

static __forceinline void spu2M_WriteFast(u32 addr, s16 value)
{
	for (int i = 0; i < 2; i++)
	{
		if (Cores[i].IRQEnable && Cores[i].IRQA == addr)
		{
			SetIrqCall(i);
		}
	}
#ifndef DEBUG_FAST
	pxAssume(addr < SPU2_DYN_MEMLINE);
#endif
	*GetMemPtr(addr) = value;
}


static __forceinline StereoOut32 MixVoice(uint coreidx, uint voiceidx)
{
	V_Core& thiscore(Cores[coreidx]);
	V_Voice& vc(thiscore.Voices[voiceidx]);

	vc.Volume.Update();

	DecodeSamples(coreidx, voiceidx);

	StereoOut32 voiceOut(0, 0);
	s32 Value = 0;

	if (vc.ADSR.Phase > V_ADSR::PHASE_STOPPED)
	{
		if (vc.Noise)
			Value = GetNoiseValues(thiscore);
		else
			Value = GetVoiceValues(thiscore, voiceidx);

		CalculateADSR(thiscore, voiceidx);
		Value = ApplyVolume(Value, vc.ADSR.Value);
		vc.OutX = Value;

		if (IsDevBuild)
			DebugCores[coreidx].Voices[voiceidx].displayPeak = std::max(DebugCores[coreidx].Voices[voiceidx].displayPeak, (s32)vc.OutX);

		voiceOut = ApplyVolume(StereoOut32(Value, Value), vc.Volume);
	}

	UpdatePitch(coreidx, voiceidx);

	ConsumeSamples(thiscore, voiceidx);

	if (voiceidx == 1)
		spu2M_WriteFast(((0 == coreidx) ? 0x400 : 0xc00) + OutPos, Value);
	else if (voiceidx == 3)
		spu2M_WriteFast(((0 == coreidx) ? 0x600 : 0xe00) + OutPos, Value);

	return voiceOut;
}

static __forceinline void MixCoreVoices(VoiceMixSet& dest, const uint coreidx)
{
	V_Core& thiscore(Cores[coreidx]);

	for (uint voiceidx = 0; voiceidx < V_Core::NumVoices; ++voiceidx)
	{
		StereoOut32 VVal(MixVoice(coreidx, voiceidx));

		dest.Dry.Left += VVal.Left & thiscore.VoiceGates[voiceidx].DryL;
		dest.Dry.Right += VVal.Right & thiscore.VoiceGates[voiceidx].DryR;
		dest.Wet.Left += VVal.Left & thiscore.VoiceGates[voiceidx].WetL;
		dest.Wet.Right += VVal.Right & thiscore.VoiceGates[voiceidx].WetR;
	}
}

static __forceinline StereoOut32 MixCore(const uint coreidx, const VoiceMixSet& inVoices, const StereoOut32& Input, const StereoOut32& Ext)
{
	V_Core& thiscore(Cores[coreidx]);

	thiscore.MasterVol.Update();
	UpdateNoise(thiscore);

	const VoiceMixSet Voices(clamp_mix(inVoices.Dry), clamp_mix(inVoices.Wet));

	spu2M_WriteFast(((0 == thiscore.Index) ? 0x1000 : 0x1800) + OutPos, Voices.Dry.Left);
	spu2M_WriteFast(((0 == thiscore.Index) ? 0x1200 : 0x1A00) + OutPos, Voices.Dry.Right);
	spu2M_WriteFast(((0 == thiscore.Index) ? 0x1400 : 0x1C00) + OutPos, Voices.Wet.Left);
	spu2M_WriteFast(((0 == thiscore.Index) ? 0x1600 : 0x1E00) + OutPos, Voices.Wet.Right);

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(thiscore.Index, CoreSrc_DryVoiceMix, Voices.Dry);
	WaveDump::WriteCore(thiscore.Index, CoreSrc_WetVoiceMix, Voices.Wet);
#endif

	StereoOut32 TD(
		Input.Left & thiscore.DryGate.InpL,
		Input.Right & thiscore.DryGate.InpR);

	TD.Left += Voices.Dry.Left & thiscore.DryGate.SndL;
	TD.Right += Voices.Dry.Right & thiscore.DryGate.SndR;

	TD.Left += Ext.Left & thiscore.DryGate.ExtL;
	TD.Right += Ext.Right & thiscore.DryGate.ExtR;

	StereoOut32 TW;

	TW.Left = Input.Left & thiscore.WetGate.InpL;
	TW.Right = Input.Right & thiscore.WetGate.InpR;

	TW.Left += Voices.Wet.Left & thiscore.WetGate.SndL;
	TW.Right += Voices.Wet.Right & thiscore.WetGate.SndR;
	TW.Left += Ext.Left & thiscore.WetGate.ExtL;
	TW.Right += Ext.Right & thiscore.WetGate.ExtR;

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(thiscore.Index, CoreSrc_PreReverb, TW);
#endif

	StereoOut32 RV = thiscore.DoReverb(TW);

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(thiscore.Index, CoreSrc_PostReverb, RV);
#endif

	return TD + ApplyVolume(RV, thiscore.FxVol);
}

void spu2Mix()
{
	StereoOut32 InputData[2] =
		{
 ApplyVolume(Cores[0].ReadInput(), Cores[0].InpVol),

			(PlayMode & 8) ? StereoOut32::Empty : ApplyVolume(Cores[1].ReadInput(), Cores[1].InpVol)};

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(0, CoreSrc_Input, InputData[0]);
	WaveDump::WriteCore(1, CoreSrc_Input, InputData[1]);
#endif

	VoiceMixSet VoiceData[2] = {{StereoOut32(), StereoOut32()}, {StereoOut32(), StereoOut32()}};

	MixCoreVoices(VoiceData[0], 0);
	MixCoreVoices(VoiceData[1], 1);

	StereoOut32 Ext(MixCore(0, VoiceData[0], InputData[0], StereoOut32::Empty));

	if ((PlayMode & 4) || (Cores[0].Mute != 0))
		Ext = StereoOut32::Empty;
	else
	{
		Ext = ApplyVolume(clamp_mix(Ext), Cores[0].MasterVol);
	}

	spu2M_WriteFast(0x800 + OutPos, Ext.Left);
	spu2M_WriteFast(0xA00 + OutPos, Ext.Right);

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(0, CoreSrc_External, Ext);
#endif

	Ext = ApplyVolume(Ext, Cores[1].ExtVol);
	StereoOut32 Out(MixCore(1, VoiceData[1], InputData[1], Ext));

	if (PlayMode & 8)
	{

		Out = Cores[1].ReadInput_HiFi();
	}
	else
	{
		Out = ApplyVolume(clamp_mix(Out), Cores[1].MasterVol);
	}

#ifdef PCSX2_DEVBUILD
	WaveDump::WriteCore(1, CoreSrc_External, Out);
#endif

	spu2Output(Out);

	OutPos++;
	if (OutPos >= 0x200)
		OutPos = 0;

	if constexpr (IsDevBuild)
	{
		static int p_cachestat_counter = 0;

		p_cachestat_counter++;
		if (p_cachestat_counter > (48000 * 10))
		{
			p_cachestat_counter = 0;
			if (SPU2::MsgCache())
			{
				SPU2::ConLog(" * SPU2 > CacheStats > Hits: %d  Misses: %d  Ignores: %d\n",
					g_counter_cache_hits,
					g_counter_cache_misses,
					g_counter_cache_ignores);
			}

			g_counter_cache_hits =
				g_counter_cache_misses =
					g_counter_cache_ignores = 0;
		}
	}
}

MULTI_ISA_UNSHARED_END
