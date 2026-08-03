// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/MultiISA.h"

#include <algorithm>
#include <array>
#include <cstdio>

extern const std::array<u16*, 0x401> regtable;

#define spu2Rs16(mmem) (*(s16*)((s8*)spu2regs + ((mmem)&0x1fff)))
#define spu2Ru16(mmem) (*(u16*)((s8*)spu2regs + ((mmem)&0x1fff)))

struct StereoOut32
{
	static const StereoOut32 Empty;

	s32 Left;
	s32 Right;

	StereoOut32() = default;

	StereoOut32(s32 left, s32 right)
		: Left(left)
		, Right(right)
	{
	}

	StereoOut32 operator*(const int& factor) const
	{
		return StereoOut32(
			Left * factor,
			Right * factor);
	}

	StereoOut32& operator*=(const int& factor)
	{
		Left *= factor;
		Right *= factor;
		return *this;
	}

	StereoOut32 operator+(const StereoOut32& right) const
	{
		return StereoOut32(
			Left + right.Left,
			Right + right.Right);
	}

	StereoOut32 operator/(int src) const
	{
		return StereoOut32(Left / src, Right / src);
	}
};

extern void (*spu2Mix)();
extern s16* GetMemPtr(u32 addr);
extern s16 spu2M_Read(u32 addr);
extern void spu2M_Write(u32 addr, s16 value);
extern void spu2M_Write(u32 addr, u16 value);
MULTI_ISA_DEF(void spu2Mix();)
extern void spu2Output(StereoOut32 out);

static __forceinline s16 SignExtend16(u16 v)
{
	return (s16)v;
}

static __forceinline s32 clamp_mix(s32 x)
{
	return std::clamp(x, -0x8000, 0x7fff);
}

static __forceinline StereoOut32 clamp_mix(StereoOut32 sample)
{
	return StereoOut32(clamp_mix(sample.Left), clamp_mix(sample.Right));
}

struct V_VolumeLR
{
	static V_VolumeLR Max;

	s32 Left;
	s32 Right;

	V_VolumeLR() = default;
	V_VolumeLR(s32 both)
		: Left(both)
		, Right(both)
	{
	}

	void DebugDump(FILE* dump, const char* title);
};

struct V_VolumeSlide
{

	union
	{
		u16 Reg_VOL;
		struct
		{
			u16 Step : 2;
			u16 Shift : 5;
			u16 : 5;
			u16 Phase : 1;
			u16 Decr : 1;
			u16 Exp : 1;
			u16 Enable : 1;
		};
	};

	u32 Counter;
	s32 Value;

public:
	V_VolumeSlide() = default;
	V_VolumeSlide(s16 regval, s32 fullvol)
		: Reg_VOL(regval)
		, Value(fullvol)
	{
	}

	void Update();
	void RegSet(u16 src);

#ifdef PCSX2_DEVBUILD
	void DebugDump(FILE* dump, const char* title, const char* nameLR);
#endif
};

struct V_VolumeSlideLR
{
	static V_VolumeSlideLR Max;

	V_VolumeSlide Left;
	V_VolumeSlide Right;

public:
	V_VolumeSlideLR() = default;
	V_VolumeSlideLR(s16 regval, s32 bothval)
		: Left(regval, bothval)
		, Right(regval, bothval)
	{
	}

	void Update()
	{
		Left.Update();
		Right.Update();
	}

#ifdef PCSX2_DEVBUILD
	void DebugDump(FILE* dump, const char* title);
#endif
};

struct V_ADSR
{
	union
	{
		u32 reg32;

		struct
		{
			u16 regADSR1;
			u16 regADSR2;
		};

		struct
		{
			u32 SustainLevel : 4;
			u32 DecayShift : 4;
			u32 AttackStep : 2;
			u32 AttackShift : 5;
			u32 AttackMode : 1;
			u32 ReleaseShift : 5;
			u32 ReleaseMode : 1;
			u32 SustainStep : 2;
			u32 SustainShift : 5;
			u32 : 1;
			u32 SustainDir : 1;
			u32 SustainMode : 1;
		};
	};

	static constexpr int ADSR_PHASES = 5;

	static constexpr int PHASE_STOPPED = 0;
	static constexpr int PHASE_ATTACK = 1;
	static constexpr int PHASE_DECAY = 2;
	static constexpr int PHASE_SUSTAIN = 3;
	static constexpr int PHASE_RELEASE = 4;

	struct CachedADSR
	{
		bool Decr;
		bool Exp;
		u8 Shift;
		s8 Step;
		s32 Target;
	};

	std::array<CachedADSR, ADSR_PHASES> CachedPhases;

	u32 Counter;
	s32 Value;
	u8 Phase;

public:
	void UpdateCache();
	bool Calculate(int voiceidx);
	void Attack();
	void Release();
};


struct V_Voice
{
	V_VolumeSlideLR Volume;

	V_ADSR ADSR;
	u16 Pitch;
	u32 LoopStartA;
	u32 StartA;
	u32 NextA;
	s32 Prev1;
	s32 Prev2;

	bool Modulated;
	bool Noise;

	s8 LoopMode;
	s8 LoopFlags;

	s32 SP;

	s32 OutX;

	s16* SBuffer;

	s32 DecodeFifo[32];
	u32 DecPosWrite;
	u32 DecPosRead;

	void Start();
	void Stop();
};

struct V_VoiceDebug
{
	s8 FirstBlock;
	s32 SampleData;
	s32 PeakX;
	s32 displayPeak;
	s32 lastSetStartA;
};

struct V_CoreDebug
{
	V_VoiceDebug Voices[24];
	u32 lastsize;

	s32 admaWaveformL[0x100];
	s32 admaWaveformR[0x100];

	s32 dmaFlag;
};

extern V_CoreDebug DebugCores[2];

struct V_Reverb
{
	s16 IN_COEF_L;
	s16 IN_COEF_R;

	u32 APF1_SIZE;
	u32 APF2_SIZE;

	s16 APF1_VOL;
	s16 APF2_VOL;

	u32 SAME_L_SRC;
	u32 SAME_R_SRC;
	u32 DIFF_L_SRC;
	u32 DIFF_R_SRC;
	u32 SAME_L_DST;
	u32 SAME_R_DST;
	u32 DIFF_L_DST;
	u32 DIFF_R_DST;

	s16 IIR_VOL;
	s16 WALL_VOL;

	u32 COMB1_L_SRC;
	u32 COMB1_R_SRC;
	u32 COMB2_L_SRC;
	u32 COMB2_R_SRC;
	u32 COMB3_L_SRC;
	u32 COMB3_R_SRC;
	u32 COMB4_L_SRC;
	u32 COMB4_R_SRC;

	s16 COMB1_VOL;
	s16 COMB2_VOL;
	s16 COMB3_VOL;
	s16 COMB4_VOL;

	u32 APF1_L_DST;
	u32 APF1_R_DST;
	u32 APF2_L_DST;
	u32 APF2_R_DST;
};

struct V_SPDIF
{
	u16 Out;
	u16 Info;
	u16 Unknown1;
	u16 Mode;
	u16 Media;
	u16 Unknown2;
	u16 Protection;
};

struct V_CoreRegs
{
	u32 PMON;
	u32 NON;
	u32 VMIXL;
	u32 VMIXR;
	u32 VMIXEL;
	u32 VMIXER;
	u32 ENDX;

	u16 MMIX;
	u16 STATX;
	u16 ATTR;
	u16 _1AC;
};

struct V_VoiceGates
{
	s32 DryL;
	s32 DryR;
	s32 WetL;
	s32 WetR;
};

struct V_CoreGates
{
	s32 InpL;
	s32 InpR;
	s32 SndL;
	s32 SndR;
	s32 ExtL;
	s32 ExtR;
};

struct VoiceMixSet
{
	StereoOut32 Dry, Wet;

	VoiceMixSet() {}
	VoiceMixSet(const StereoOut32& dry, const StereoOut32& wet)
		: Dry(dry)
		, Wet(wet)
	{
	}
};

struct V_Core
{
	static const uint NumVoices = 24;

	u32 Index;

	V_VoiceGates VoiceGates[NumVoices];
	V_CoreGates DryGate;
	V_CoreGates WetGate;

	V_VolumeSlideLR MasterVol;
	V_VolumeLR ExtVol;
	V_VolumeLR InpVol;
	V_VolumeLR FxVol;

	V_Voice Voices[NumVoices];

	u32 IRQA;
	u32 TSA;
	u32 ActiveTSA;

	bool IRQEnable;
	bool FxEnable;
	bool Mute;
	bool AdmaInProgress;

	s8 DMABits;
	u8 NoiseClk;
	u32 NoiseCnt;
	u32 NoiseOut;
	u16 AutoDMACtrl;
	s32 DMAICounter;
	u64 LastClock;
	u32 InputDataLeft;
	u32 InputDataTransferred;
	u32 InputPosWrite;
	u32 InputDataProgress;

	V_Reverb Revb;

	s16 RevbDownBuf[2][64 * 2];
	s16 RevbUpBuf[2][64 * 2];
	u32 RevbSampleBufPos;
	u32 EffectsStartA;
	u32 EffectsEndA;

	V_CoreRegs Regs;

	StereoOut32 LastEffect;

	u8 CoreEnabled;

	u8 AttrBit0;
	u8 DmaMode;

	bool DmaStarted;
	u32 AutoDmaFree;

	u16* DMAPtr;
	u16* DMARPtr;
	u32 ReadSize;
	bool IsDMARead;

	u32 KeyOn;
	u32 KeyOff;

	u16 psxSoundDataTransferControl;
	u16 psxSPUSTAT;

	V_Core()
		: Index(-1)
		, DMAPtr(nullptr)
	{
	}
	V_Core(int idx) : Index(idx) {};

	void Init(int index);
	void UpdateEffectsBufferSize();
	void AnalyzeReverbPreset();

	void WriteRegPS1(u32 mem, u16 value);
	u16 ReadRegPS1(u32 mem);

	StereoOut32 DoReverb(StereoOut32 Input);
	s32 RevbGetIndexer(s32 offset);

	StereoOut32 ReadInput();
	StereoOut32 ReadInput_HiFi();

	int GetDmaIndex() const
	{
		return (Index == 0) ? 4 : 7;
	}

	char GetDmaIndexChar() const
	{
		return 0x30 + GetDmaIndex();
	}

	__forceinline u16 DmaRead()
	{
		const u16 ret = static_cast<u16>(spu2M_Read(ActiveTSA));
		++ActiveTSA;
		ActiveTSA &= 0xfffff;
		TSA = ActiveTSA;
		return ret;
	}

	__forceinline void DmaWrite(u16 value)
	{
		spu2M_Write(ActiveTSA, value);
		++ActiveTSA;
		ActiveTSA &= 0xfffff;
		TSA = ActiveTSA;
	}

	void LogAutoDMA(FILE* fp);

	void DoDMAwrite(u16* pMem, u32 size);
	void DoDMAread(u16* pMem, u32 size);
	void FinishDMAread();

	void AutoDMAReadBuffer(int mode);
	void StartADMAWrite(u16* pMem, u32 sz);
	void PlainDMAWrite(u16* pMem, u32 sz);
	void FinishDMAwrite();
};

MULTI_ISA_DEF(
	StereoOut32 ReverbUpsample(V_Core& core);
	s32 ReverbDownsample(V_Core& core, bool right);
)

extern StereoOut32 (*ReverbUpsample)(V_Core& core);
extern s32 (*ReverbDownsample)(V_Core& core, bool right);

extern V_Core Cores[2];
extern V_SPDIF Spdif;

extern u16 OutPos;
extern u16 InputPos;
extern u32 Cycles;

extern s16 spu2regs[0x010000 / sizeof(s16)];
extern s16 _spu2mem[0x200000 / sizeof(s16)];
extern int PlayMode;

extern void SetIrqCall(int core);
extern void SetIrqCallDMA(int core);
extern void StartVoices(int core, u32 value);
extern void StopVoices(int core, u32 value);
extern void CalculateADSR(V_Voice& vc);
extern void UpdateSpdifMode();

namespace SPU2Savestate
{
	struct DataBlock;

	extern s32 FreezeIt(DataBlock& spud);
	extern s32 ThawIt(DataBlock& spud);
	extern s32 SizeIt();
}

static constexpr s32 SPU2_DYN_MEMLINE = 0x2800;

static constexpr int pcm_WordsPerBlock = 8;

static constexpr int pcm_BlockCount = 0x100000 / pcm_WordsPerBlock;

static constexpr int pcm_DecodedSamplesPerBlock = 28;

struct PcmCacheEntry
{
	bool Validated;
	s16 Sampledata[pcm_DecodedSamplesPerBlock];
	s32 Prev1;
	s32 Prev2;
};

extern PcmCacheEntry pcm_cache_data[pcm_BlockCount];
extern int g_counter_cache_hits;
extern int g_counter_cache_misses;
extern int g_counter_cache_ignores;
