// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Host/AudioStreamTypes.h"

#include "common/Pcsx2Defs.h"
#include "common/FPControl.h"

#include <array>
#include <string>
#include <optional>
#include <vector>

#define BITFIELD32() \
	union \
	{ \
		u32 bitset; \
		struct \
		{
#define BITFIELD_END \
	} \
	; \
	} \
	;

class Error;
class SettingsInterface;
class SettingsWrapper;

enum class CDVD_SourceType : uint8_t;

namespace Pad
{
	enum class ControllerType : u8;
}

struct SettingInfo
{
	using GetOptionsCallback = std::vector<std::pair<std::string, std::string>> (*)();

	enum class Type
	{
		Boolean,
		Integer,
		IntegerList,
		Float,
		String,
		StringList,
		Path,
	};

	Type type;
	const char* name;
	const char* display_name;
	const char* description;
	const char* default_value;
	const char* min_value;
	const char* max_value;
	const char* step_value;
	const char* format;
	const char* const* options;
	GetOptionsCallback get_options;
	float multiplier;

	const char* StringDefaultValue() const;
	bool BooleanDefaultValue() const;
	s32 IntegerDefaultValue() const;
	s32 IntegerMinValue() const;
	s32 IntegerMaxValue() const;
	s32 IntegerStepValue() const;
	float FloatDefaultValue() const;
	float FloatMinValue() const;
	float FloatMaxValue() const;
	float FloatStepValue() const;

	void SetDefaultValue(SettingsInterface* si, const char* section, const char* key) const;
	void CopyValue(SettingsInterface* dest_si, const SettingsInterface& src_si,
		const char* section, const char* key) const;
};

enum class GenericInputBinding : u8;

struct InputBindingInfo
{
	enum class Type : u8
	{
		Unknown,
		Button,
		Axis,
		HalfAxis,
		Motor,
		Pointer,
		Keyboard,
		Device,
		Macro,
	};

	const char* name;
	const char* display_name;
	const char* icon_name;
	Type bind_type;
	u16 bind_index;
	GenericInputBinding generic_mapping;
};

enum class GenericInputBinding : u8
{
	Unknown,

	DPadUp,
	DPadRight,
	DPadLeft,
	DPadDown,

	LeftStickUp,
	LeftStickRight,
	LeftStickDown,
	LeftStickLeft,
	L3,

	RightStickUp,
	RightStickRight,
	RightStickDown,
	RightStickLeft,
	R3,

	Triangle,
	Circle,
	Cross,
	Square,

	Select,
	Start,
	System,

	L1,
	L2,
	R1,
	R2,

	SmallMotor,
	LargeMotor,

	Count,
};

enum GamefixId
{
	GamefixId_FIRST = 0,

	Fix_FpuMultiply = GamefixId_FIRST,
	Fix_GoemonTlbMiss,
	Fix_SoftwareRendererFMV,
	Fix_SkipMpeg,
	Fix_OPHFlag,
	Fix_EETiming,
	Fix_InstantDMA,
	Fix_DMABusy,
	Fix_GIFFIFO,
	Fix_VIFFIFO,
	Fix_VIF1Stall,
	Fix_VuAddSub,
	Fix_Ibit,
	Fix_VUSync,
	Fix_VUOverflow,
	Fix_XGKick,
	Fix_BlitInternalFPS,
	Fix_FullVU0Sync,

	GamefixId_COUNT
};

enum class SpeedHack
{
	MVUFlag,
	InstantVU1,
	MTVU,
	EECycleRate,
	MaxCount,
};

enum class DebugAnalysisCondition
{
	ALWAYS,
	IF_DEBUGGER_IS_OPEN,
	NEVER
};

struct DebugSymbolSource
{
	std::string Name;
	bool ClearDuringAnalysis = false;

	friend auto operator<=>(const DebugSymbolSource& lhs, const DebugSymbolSource& rhs) = default;
};

struct DebugExtraSymbolFile
{
	std::string Path;
	std::string BaseAddress;
	std::string Condition;

	friend auto operator<=>(const DebugExtraSymbolFile& lhs, const DebugExtraSymbolFile& rhs) = default;
};

enum class DebugFunctionScanMode
{
	SCAN_ELF,
	SCAN_MEMORY,
	SKIP
};

enum class AspectRatioType : u8
{
	Stretch,
	RAuto4_3_3_2,
	R4_3,
	R16_9,
	R10_7,
	MaxCount
};

enum class FMVAspectRatioSwitchType : u8
{
	Off,
	RAuto4_3_3_2,
	R4_3,
	R16_9,
	R10_7,
	MaxCount
};

enum class MemoryCardType
{
	Empty,
	File,
	Folder,
	MaxCount
};

enum class MemoryCardFileType
{
	Unknown,
	PS2_8MB,
	PS2_16MB,
	PS2_32MB,
	PS2_64MB,
	PS1,
	MaxCount
};

enum class LimiterModeType : u8
{
	Nominal,
	Turbo,
	Slomo,
	Unlimited,
};

enum class GSRendererType : s8
{
	Auto = -1,
	DX11 = 3,
	Null = 11,
	OGL = 12,
	SW = 13,
	VK = 14,
	Metal = 17,
	DX12 = 15,
};

enum class GSVSyncMode : u8
{
	Disabled,
	FIFO,
	Mailbox,
	Count
};

enum class GSInterlaceMode : u8
{
	Automatic,
	Off,
	WeaveTFF,
	WeaveBFF,
	BobTFF,
	BobBFF,
	BlendTFF,
	BlendBFF,
	AdaptiveTFF,
	AdaptiveBFF,
	Count
};

enum class GSPostBilinearMode : u8
{
	Off,
	BilinearSmooth,
	BilinearSharp,
};

enum class BiFiltering : u8
{
	Nearest,
	Forced,
	PS2,
	Forced_But_Sprite,
};

enum class TriFiltering : s8
{
	Automatic = -1,
	Off,
	PS2,
	Forced,
};

enum class AccBlendLevel : u8
{
	Minimum,
	Basic,
	Medium,
	High,
	Full,
	Maximum,
	MaxCount
};

enum class OsdOverlayPos : u8
{
	None,
	TopLeft,
	TopCenter,
	TopRight,
	CenterLeft,
	Center,
	CenterRight,
	BottomLeft,
	BottomCenter,
	BottomRight,
};

enum class TexturePreloadingLevel : u8
{
	Off,
	Partial,
	Full,
};

enum class GSScreenshotSize : u8
{
	WindowResolution,
	InternalResolution,
	InternalResolutionUncorrected,
};

enum class GSScreenshotFormat : u8
{
	PNG,
	JPEG,
	WebP,
	Count,
};

enum class GSDumpCompressionMethod : u8
{
	Uncompressed,
	LZMA,
	Zstandard,
};

enum class SavestateCompressionMethod : u8
{
	Uncompressed = 0,
	Deflate = 1,
	Zstandard = 2
};

enum class SavestateCompressionLevel : u8
{
	Low = 0,
	Medium = 1,
	High = 2,
	VeryHigh = 3,
};

enum class GSHardwareDownloadMode : u8
{
	Enabled,
	EnabledForceFull,
	NoReadbacks,
	Unsynchronized,
	Disabled
};

enum class GSCASMode : u8
{
	Disabled,
	SharpenOnly,
	SharpenAndResize,
};

enum class GSHWAutoFlushLevel : u8
{
	Disabled,
	SpritesOnly,
	Enabled,
};

enum class GSGPUTargetCLUTMode : u8
{
	Disabled,
	Enabled,
	InsideTarget,
};

enum class GSTextureInRtMode : u8
{
	Disabled,
	InsideTargets,
	MergeTargets,
};

enum class GSLimit24BitDepth : u8
{
	Disabled,
	PrioritizeUpper,
	PrioritizeLower,
};

enum class GSBilinearDirtyMode : u8
{
	Automatic,
	ForceBilinear,
	ForceNearest,
	MaxCount
};

enum class GSHalfPixelOffset : u8
{
	Off,
	Normal,
	Special,
	SpecialAggressive,
	Native,
	NativeWTexOffset,
	MaxCount
};

enum class GSNativeScaling : u8
{
	Off,
	Normal,
	Aggressive,
	NormalUpscaled,
	AggressiveUpscaled,
	MaxCount
};

enum class GSDepthFeedbackMode : u8
{
	None      = 0,
	Auto      = 1,
	Depth     = 2,
	DepthAsRT = 3,
};

enum class AchievementOverlayPosition : u8
{
	TopLeft,
	TopCenter,
	TopRight,
	CenterLeft,
	Center,
	CenterRight,
	BottomLeft,
	BottomCenter,
	BottomRight,
	MaxCount
};

struct TraceLogsEE
{
	BITFIELD32()
	bool
		bios : 1,
		memory : 1,
		giftag : 1,
		vifcode : 1,
		mskpath3 : 1,
		r5900 : 1,
		cop0 : 1,
		cop1 : 1,
		cop2 : 1,
		cache : 1,
		knownhw : 1,
		unknownhw : 1,
		dmahw : 1,
		ipu : 1,
		dmac : 1,
		counters : 1,
		spr : 1,
		vif : 1,
		gif : 1;
	BITFIELD_END

	TraceLogsEE();

	bool operator==(const TraceLogsEE& right) const;
	bool operator!=(const TraceLogsEE& right) const;
};

struct TraceLogsIOP
{
	BITFIELD32()
	bool
		bios : 1,
		memcards : 1,
		pad : 1,
		r3000a : 1,
		cop2 : 1,
		memory : 1,
		knownhw : 1,
		unknownhw : 1,
		dmahw : 1,
		dmac : 1,
		counters : 1,
		cdvd : 1,
		mdec : 1;
	BITFIELD_END

	TraceLogsIOP();

	bool operator==(const TraceLogsIOP& right) const;
	bool operator!=(const TraceLogsIOP& right) const;
};

struct TraceLogsMISC
{
	BITFIELD32()
	bool
		sif : 1;
	BITFIELD_END

	TraceLogsMISC();

	bool operator==(const TraceLogsMISC& right) const;
	bool operator!=(const TraceLogsMISC& right) const;
};

struct TraceLogFilters
{
	bool Enabled;

	TraceLogsEE EE;
	TraceLogsIOP IOP;
	TraceLogsMISC MISC;

	TraceLogFilters();

	void LoadSave(SettingsWrapper& ini);
	void SyncToConfig() const;
	bool operator==(const TraceLogFilters& right) const;
	bool operator!=(const TraceLogFilters& right) const;
};

struct Pcsx2Config
{
	struct ProfilerOptions
	{
		BITFIELD32()
		bool
			Enabled : 1,
			RecBlocks_EE : 1,
			RecBlocks_IOP : 1,
			RecBlocks_VU0 : 1,
			RecBlocks_VU1 : 1;
		BITFIELD_END

		ProfilerOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const ProfilerOptions& right) const;
		bool operator!=(const ProfilerOptions& right) const;
	};

	struct RecompilerOptions
	{
		BITFIELD32()
		bool
			EnableEE : 1,
			EnableIOP : 1,
			EnableVU0 : 1,
			EnableVU1 : 1;

		bool
			vu0Overflow : 1,
			vu0ExtraOverflow : 1,
			vu0SignOverflow : 1,
			vu0Underflow : 1;

		bool
			vu1Overflow : 1,
			vu1ExtraOverflow : 1,
			vu1SignOverflow : 1,
			vu1Underflow : 1;

		bool
			fpuOverflow : 1,
			fpuExtraOverflow : 1,
			fpuFullMode : 1;

		bool
			EnableEECache : 1;
		bool
			EnableFastmem : 1;
		bool
			PauseOnTLBMiss : 1;
		BITFIELD_END

		RecompilerOptions();
		void ApplySanityCheck();

		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const RecompilerOptions& right) const;
		bool operator!=(const RecompilerOptions& right) const;

		u32 GetEEClampMode() const;
		void SetEEClampMode(u32 value);

		u32 GetVUClampMode() const;
	};

	struct CpuOptions
	{
		BITFIELD32()
		bool
			ExtraMemory : 1;
		BITFIELD_END

		RecompilerOptions Recompiler;

		FPControlRegister FPUFPCR;
		FPControlRegister FPUDivFPCR;
		FPControlRegister VU0FPCR;
		FPControlRegister VU1FPCR;

		CpuOptions();
		void LoadSave(SettingsWrapper& wrap);
		void ApplySanityCheck();

		bool CpusChanged(const CpuOptions& right) const;

		bool operator==(const CpuOptions& right) const;
		bool operator!=(const CpuOptions& right) const;
	};

	struct GSOptions
	{
		static const char* AspectRatioNames[];
		static const char* FMVAspectRatioSwitchNames[];
		static const char* BlendingLevelNames[];
		static const char* CaptureContainers[];

		static const char* GetRendererName(GSRendererType type);

		static std::optional<bool> TriStateToOptionalBoolean(int value);

		static constexpr float DEFAULT_FRAME_RATE_NTSC = 59.94f;
		static constexpr float DEFAULT_FRAME_RATE_PAL = 50.00f;

		static constexpr GSRendererType DEFAULT_HW_RENDERER = GSRendererType::Auto;

		static constexpr AspectRatioType DEFAULT_ASPECT_RATIO = AspectRatioType::RAuto4_3_3_2;
		static constexpr GSInterlaceMode DEFAULT_INTERLACE_MODE = GSInterlaceMode::Automatic;
		static constexpr GSPostBilinearMode DEFAULT_BILINEAR_FILTERING_MODE = GSPostBilinearMode::BilinearSmooth;
		static constexpr FMVAspectRatioSwitchType DEFAULT_FMV_ASPECT_RATIO = FMVAspectRatioSwitchType::Off;
		static constexpr GSCASMode DEFAULT_CAS_MODE = GSCASMode::Disabled;

		static constexpr float DEFAULT_UPSCALE_MULTIPLIER = 1.0f;
		static constexpr AccBlendLevel DEFAULT_BLENDING_ACCURACY = AccBlendLevel::Basic;
		static constexpr BiFiltering DEFAULT_TEXTURE_FILTERING_MODE = BiFiltering::PS2;
		static constexpr TriFiltering DEFAULT_TRILINEAR_FILTERING_MODE = TriFiltering::Automatic;

		static constexpr float DEFAULT_OSD_SCALE = 100.0f;
		static constexpr float DEFAULT_OSD_MARGIN = 10.0f;
		static constexpr OsdOverlayPos DEFAULT_OSD_MESSAGE_POS = OsdOverlayPos::TopLeft;
		static constexpr OsdOverlayPos DEFAULT_OSD_PERFORMANCE_POS = OsdOverlayPos::TopRight;

		static constexpr int DEFAULT_VIDEO_CAPTURE_BITRATE = 6000;
		static constexpr int DEFAULT_VIDEO_CAPTURE_WIDTH = 640;
		static constexpr int DEFAULT_VIDEO_CAPTURE_HEIGHT = 480;
		static constexpr int DEFAULT_AUDIO_CAPTURE_BITRATE = 192;
		static const char* DEFAULT_CAPTURE_CONTAINER;

		static constexpr int DEFAULT_SHADEBOOST_BRIGHTNESS = 50;
		static constexpr int DEFAULT_SHADEBOOST_CONTRAST = 50;
		static constexpr int DEFAULT_SHADEBOOST_GAMMA = 50;
		static constexpr int DEFAULT_SHADEBOOST_SATURATION = 50;

		union
		{
			u64 bitsets[2];

			struct
			{
				bool
					SynchronousMTGS : 1,
					VsyncEnable : 1,
					DisableMailboxPresentation : 1,
					ExtendedUpscalingMultipliers : 1,
					PCRTCAntiBlur : 1,
					DisableInterlaceOffset : 1,
					PCRTCOffsets : 1,
					PCRTCOverscan : 1,
					IntegerScaling : 1,
					UseDebugDevice : 1,
					UseDebugBlend : 1,
					UseBlitSwapChain : 1,
					DisableShaderCache : 1,
					DisableFramebufferFetch : 1,
					DisableVertexShaderExpand : 1,
					SkipDuplicateFrames : 1,
					OsdShowSpeed : 1,
					OsdShowFPS : 1,
					OsdShowVPS : 1,
					OsdShowResolution : 1,
					OsdShowGSStats : 1,
					OsdShowCPU : 1,
					OsdShowGPU : 1,
					OsdShowGPUDebug : 1,
					OsdShowGPUStats : 1,
					OsdShowIndicators : 1,
					OsdShowFrameTimes : 1,
					OsdShowHardwareInfo : 1,
					OsdShowVersion : 1,
					OsdShowSettings : 1,
					OsdshowPatches : 1,
					OsdShowInputs : 1,
					OsdShowVideoCapture : 1,
					OsdShowInputRec : 1,
					OsdShowTextureReplacements : 1,
					OsdBoldText : 1,
					HWSpinGPUForReadbacks : 1,
					HWSpinCPUForReadbacks : 1,
					GPUPaletteConversion : 1,
					AutoFlushSW : 1,
					PreloadFrameWithGSData : 1,
					Mipmap : 1,
					HWMipmap : 1,
					HWAccurateAlphaTest : 1,
					HWAA1 : 1,
					HWROV : 1,
					HWROVLogging : 1,
					HWROVBarriersVK : 1,
					ManualUserHacks : 1,
					UserHacks_AlignSpriteX : 1,
					UserHacks_CPUFBConversion : 1,
					UserHacks_ReadTCOnClose : 1,
					UserHacks_DisableDepthSupport : 1,
					UserHacks_DisablePartialInvalidation : 1,
					UserHacks_DisableSafeFeatures : 1,
					UserHacks_DisableRenderFixes : 1,
					UserHacks_MergePPSprite : 1,
					UserHacks_ForceEvenSpritePosition : 1,
					UserHacks_NativePaletteDraw : 1,
					UserHacks_EstimateTextureRegion : 1,
					UserHacks_DrawBuffering : 1,
					FXAA : 1,
					ShadeBoost : 1,
					DumpGSData : 1,
					SaveRT : 1,
					SaveFrame : 1,
					SaveTexture : 1,
					SaveDepth : 1,
					SaveAlpha : 1,
					SaveInfo : 1,
					SaveTransferImages : 1,
					SaveDrawStats : 1,
					SaveFrameStats : 1,
					SaveHWConfig : 1,
					DumpReplaceableTextures : 1,
					DumpReplaceableMipmaps : 1,
					DumpTexturesWithFMVActive : 1,
					DumpDirectTextures : 1,
					DumpPaletteTextures : 1,
					LoadTextureReplacements : 1,
					LoadTextureReplacementsAsync : 1,
					PrecacheTextureReplacements : 1,
					EnableVideoCapture : 1,
					EnableVideoCaptureParameters : 1,
					VideoCaptureAutoResolution : 1,
					EnableAudioCapture : 1,
					EnableAudioCaptureParameters : 1,
					OrganizeSnapshotsByGame : 1,
					OrganizeVideoCaptureByGame : 1;
			};
		};

		int VsyncQueueSize = 2;

		float FramerateNTSC = DEFAULT_FRAME_RATE_NTSC;
		float FrameratePAL = DEFAULT_FRAME_RATE_PAL;

		AspectRatioType AspectRatio = DEFAULT_ASPECT_RATIO;
		FMVAspectRatioSwitchType FMVAspectRatioSwitch = DEFAULT_FMV_ASPECT_RATIO;
		GSInterlaceMode InterlaceMode = DEFAULT_INTERLACE_MODE;
		GSPostBilinearMode LinearPresent = DEFAULT_BILINEAR_FILTERING_MODE;

		float StretchY = 100.0f;
		int Crop[4] = {};

		float OsdScale = DEFAULT_OSD_SCALE;
		float OsdMargin = DEFAULT_OSD_MARGIN;
		std::string OsdFontPath;
		OsdOverlayPos OsdMessagesPos = DEFAULT_OSD_MESSAGE_POS;
		OsdOverlayPos OsdPerformancePos = DEFAULT_OSD_PERFORMANCE_POS;

		GSRendererType Renderer = DEFAULT_HW_RENDERER;
		float UpscaleMultiplier = DEFAULT_UPSCALE_MULTIPLIER;

		AccBlendLevel AccurateBlendingUnit = DEFAULT_BLENDING_ACCURACY;
		BiFiltering TextureFiltering = DEFAULT_TEXTURE_FILTERING_MODE;
		TexturePreloadingLevel TexturePreloading = TexturePreloadingLevel::Full;
		GSDumpCompressionMethod GSDumpCompression = GSDumpCompressionMethod::Zstandard;
		GSHardwareDownloadMode HWDownloadMode = GSHardwareDownloadMode::Enabled;
		GSCASMode CASMode = DEFAULT_CAS_MODE;
		u8 Dithering = 2;
		u8 MaxAnisotropy = 0;
		u8 TVShader = 0;
		s16 GetSkipCountFunctionId = -1;
		s16 BeforeDrawFunctionId = -1;
		s16 MoveHandlerFunctionId = -1;
		int SkipDrawStart = 0;
		int SkipDrawEnd = 0;

		GSHWAutoFlushLevel UserHacks_AutoFlush = GSHWAutoFlushLevel::Disabled;
		GSHalfPixelOffset UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
		s8 UserHacks_RoundSprite = 0;
		GSNativeScaling UserHacks_NativeScaling = GSNativeScaling::Off;
		s32 UserHacks_TCOffsetX = 0;
		s32 UserHacks_TCOffsetY = 0;
		u8 UserHacks_CPUSpriteRenderBW = 0;
		u8 UserHacks_CPUSpriteRenderLevel = 0;
		u8 UserHacks_CPUCLUTRender = 0;
		GSGPUTargetCLUTMode UserHacks_GPUTargetCLUTMode = GSGPUTargetCLUTMode::Disabled;
		GSTextureInRtMode UserHacks_TextureInsideRt = GSTextureInRtMode::Disabled;
		GSLimit24BitDepth UserHacks_Limit24BitDepth = GSLimit24BitDepth::Disabled;
		GSBilinearDirtyMode UserHacks_BilinearHack = GSBilinearDirtyMode::Automatic;
		TriFiltering TriFilter = DEFAULT_TRILINEAR_FILTERING_MODE;
		s8 OverrideTextureBarriers = -1;
		GSDepthFeedbackMode DepthFeedbackMode = GSDepthFeedbackMode::Auto;

		u8 CAS_Sharpness = 50;
		u8 ShadeBoost_Brightness = DEFAULT_SHADEBOOST_BRIGHTNESS;
		u8 ShadeBoost_Contrast = DEFAULT_SHADEBOOST_CONTRAST;
		u8 ShadeBoost_Saturation = DEFAULT_SHADEBOOST_SATURATION;
		u8 ShadeBoost_Gamma = DEFAULT_SHADEBOOST_GAMMA;
		u8 PNGCompressionLevel = 1;

		u16 SWExtraThreads = 2;
		u16 SWExtraThreadsHeight = 4;

		int SaveDrawStart = 0;
		int SaveDrawCount = 5000;
		int SaveDrawBy = 1;
		int SaveFrameStart = 0;
		int SaveFrameCount = -1;
		int SaveFrameBy = 1;

		s8 ExclusiveFullscreenControl = -1;
		GSScreenshotSize ScreenshotSize = GSScreenshotSize::WindowResolution;
		GSScreenshotFormat ScreenshotFormat = GSScreenshotFormat::PNG;
		int ScreenshotQuality = 90;

		std::string CaptureContainer = DEFAULT_CAPTURE_CONTAINER;
		std::string VideoCaptureCodec;
		std::string VideoCaptureFormat;
		std::string VideoCaptureParameters;
		std::string AudioCaptureCodec;
		std::string AudioCaptureParameters;
		int VideoCaptureBitrate = DEFAULT_VIDEO_CAPTURE_BITRATE;
		int VideoCaptureWidth = DEFAULT_VIDEO_CAPTURE_WIDTH;
		int VideoCaptureHeight = DEFAULT_VIDEO_CAPTURE_HEIGHT;
		int AudioCaptureBitrate = DEFAULT_AUDIO_CAPTURE_BITRATE;

		std::string Adapter;
		std::string HWDumpDirectory;
		std::string SWDumpDirectory;

		GSOptions();

		void LoadSave(SettingsWrapper& wrap);

		void MaskUserHacks();

		void MaskUpscalingHacks();

		bool UseHardwareRenderer() const;

		bool RestartOptionsAreEqual(const GSOptions& right) const;

		bool OptionsAreEqual(const GSOptions& right) const;

		bool operator==(const GSOptions& right) const;
		bool operator!=(const GSOptions& right) const;

		bool ShouldDump(u64 draw, int frame) const;
	};

	struct SPU2Options
	{
		enum class SPU2SyncMode : u8
		{
			Disabled,
			TimeStretch,
			Count
		};

		static constexpr s32 MAX_VOLUME = 200;
		static constexpr AudioBackend DEFAULT_BACKEND = AudioBackend::Cubeb;
		static constexpr SPU2SyncMode DEFAULT_SYNC_MODE = SPU2SyncMode::TimeStretch;

		static std::optional<SPU2SyncMode> ParseSyncMode(const char* str);
		static const char* GetSyncModeName(SPU2SyncMode backend);
		static const char* GetSyncModeDisplayName(SPU2SyncMode backend);

		BITFIELD32()
		bool
			DebugEnabled : 1,
			MsgToConsole : 1,
			MsgKeyOnOff : 1,
			MsgVoiceOff : 1,
			MsgDMA : 1,
			MsgAutoDMA : 1,
			MsgCache : 1,
			AccessLog : 1,
			DMALog : 1,
			WaveLog : 1,
			CoresDump : 1,
			MemDump : 1,
			RegDump : 1,
			VisualDebugEnabled : 1;
		BITFIELD_END

		u32 StandardVolume = 100;
		u32 FastForwardVolume = 100;
		bool OutputMuted = false;

		AudioBackend Backend = DEFAULT_BACKEND;
		SPU2SyncMode SyncMode = DEFAULT_SYNC_MODE;
		AudioStreamParameters StreamParameters;

		std::string DriverName;
		std::string DeviceName;

		SPU2Options();

		void LoadSave(SettingsWrapper& wrap);

		bool IsTimeStretchEnabled() const { return (SyncMode == SPU2SyncMode::TimeStretch); }

		bool operator==(const SPU2Options& right) const;
		bool operator!=(const SPU2Options& right) const;
	};

	struct DEV9Options
	{
		enum struct NetApi : int
		{
			Unset = 0,
			PCAP_Bridged = 1,
			PCAP_Switched = 2,
			TAP = 3,
			Sockets = 4,
		};
		static const char* NetApiNames[];

		enum struct DnsMode : int
		{
			Manual = 0,
			Auto = 1,
			Internal = 2,
		};
		static const char* DnsModeNames[];

		struct HostEntry
		{
			std::string Url;
			std::string Desc;
			u8 Address[4]{};
			bool Enabled;

			bool operator==(const HostEntry& right) const;
			bool operator!=(const HostEntry& right) const;
		};

		bool EthEnable{false};
		NetApi EthApi{NetApi::Unset};
		std::string EthDevice;
		bool EthLogDHCP{false};
		bool EthLogDNS{false};

		bool InterceptDHCP{false};
		u8 PS2IP[4]{};
		u8 Mask[4]{};
		u8 Gateway[4]{};
		u8 DNS1[4]{};
		u8 DNS2[4]{};
		bool AutoMask{true};
		bool AutoGateway{true};
		DnsMode ModeDNS1{DnsMode::Auto};
		DnsMode ModeDNS2{DnsMode::Auto};

		std::vector<HostEntry> EthHosts;

		bool HddEnable{false};
		std::string HddFile;

		DEV9Options();

		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const DEV9Options& right) const;
		bool operator!=(const DEV9Options& right) const;

	protected:
		static void LoadIPHelper(u8* field, const std::string& setting);
		static std::string SaveIPHelper(u8* field);
	};

	struct GamefixOptions
	{
		BITFIELD32()
		bool
			FpuMulHack : 1,
			GoemonTlbHack : 1,
			SoftwareRendererFMVHack : 1,
			SkipMPEGHack : 1,
			OPHFlagHack : 1,
			EETimingHack : 1,
			InstantDMAHack : 1,
			DMABusyHack : 1,
			GIFFIFOHack : 1,
			VIFFIFOHack : 1,
			VIF1StallHack : 1,
			VuAddSubHack : 1,
			IbitHack : 1,
			VUSyncHack : 1,
			VUOverflowHack : 1,
			XgKickHack : 1,
			BlitInternalFPSHack : 1,
			FullVU0SyncHack : 1;
		BITFIELD_END

		GamefixOptions();
		void LoadSave(SettingsWrapper& wrap);
		GamefixOptions& DisableAll();

		static const char* GetGameFixName(GamefixId id);

		bool Get(GamefixId id) const;
		void Set(GamefixId id, bool enabled = true);
		void Clear(GamefixId id) { Set(id, false); }

		bool operator==(const GamefixOptions& right) const;
		bool operator!=(const GamefixOptions& right) const;
	};

	struct SpeedhackOptions
	{
		static constexpr s8 MIN_EE_CYCLE_RATE = -3;
		static constexpr s8 MAX_EE_CYCLE_RATE = 3;
		static constexpr u8 MAX_EE_CYCLE_SKIP = 3;

		BITFIELD32()
		bool
			fastCDVD : 1,
			IntcStat : 1,
			WaitLoop : 1,
			vuFlagHack : 1,
			vuThread : 1,
			vu1Instant : 1;
		BITFIELD_END

		s8 EECycleRate;
		u8 EECycleSkip;

		SpeedhackOptions();
		void LoadSave(SettingsWrapper& conf);
		SpeedhackOptions& DisableAll();

		void Set(SpeedHack id, int value);

		bool operator==(const SpeedhackOptions& right) const;
		bool operator!=(const SpeedhackOptions& right) const;

		static const char* GetSpeedHackName(SpeedHack id);
		static std::optional<SpeedHack> ParseSpeedHackName(const std::string_view name);
	};

	struct DebugAnalysisOptions
	{

		static const char* RunConditionNames[];
		static const char* FunctionScanModeNames[];

		DebugAnalysisCondition RunCondition = DebugAnalysisCondition::IF_DEBUGGER_IS_OPEN;
		bool GenerateSymbolsForIRXExports = true;

		bool AutomaticallySelectSymbolsToClear = true;
		std::vector<DebugSymbolSource> SymbolSources;

		bool ImportSymbolsFromELF = true;
		bool ImportSymFileFromDefaultLocation = true;
		bool DemangleSymbols = true;
		bool DemangleParameters = true;
		std::vector<DebugExtraSymbolFile> ExtraSymbolFiles;

		DebugFunctionScanMode FunctionScanMode = DebugFunctionScanMode::SCAN_ELF;
		bool CustomFunctionScanRange = false;
		std::string FunctionScanStartAddress;
		std::string FunctionScanEndAddress;

		bool GenerateFunctionHashes = true;

		void LoadSave(SettingsWrapper& wrap);

		friend auto operator<=>(const DebugAnalysisOptions& lhs, const DebugAnalysisOptions& rhs) = default;
	};

	struct EmulationSpeedOptions
	{
		BITFIELD32()
		bool SyncToHostRefreshRate : 1;
		bool UseVSyncForTiming : 1;
		BITFIELD_END

		float NominalScalar{1.0f};
		float TurboScalar{2.0f};
		float SlomoScalar{0.5f};

		EmulationSpeedOptions();

		void LoadSave(SettingsWrapper& wrap);
		void SanityCheck();

		bool operator==(const EmulationSpeedOptions& right) const;
		bool operator!=(const EmulationSpeedOptions& right) const;
	};

	struct FilenameOptions
	{
		std::string Bios;

		FilenameOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const FilenameOptions& right) const;
		bool operator!=(const FilenameOptions& right) const;
	};

	struct USBOptions
	{
		static constexpr u32 NUM_PORTS = 2;

		struct Port
		{
			s32 DeviceType;
			u32 DeviceSubtype;

			bool operator==(const USBOptions::Port& right) const;
			bool operator!=(const USBOptions::Port& right) const;
		};

		std::array<Port, NUM_PORTS> Ports;

		USBOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const USBOptions& right) const;
		bool operator!=(const USBOptions& right) const;
	};

	struct PadOptions
	{
		static constexpr u32 NUM_PORTS = 8;

		struct Port
		{
			Pad::ControllerType Type;

			bool operator==(const PadOptions::Port& right) const;
			bool operator!=(const PadOptions::Port& right) const;
		};

		std::array<Port, NUM_PORTS> Ports;

		BITFIELD32()
		bool
			MultitapPort0_Enabled : 1,
			MultitapPort1_Enabled;
		BITFIELD_END

		PadOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool IsMultitapPortEnabled(u32 port) const
		{
			return (port == 0) ? MultitapPort0_Enabled : MultitapPort1_Enabled;
		}

		bool operator==(const PadOptions& right) const;
		bool operator!=(const PadOptions& right) const;
	};

	struct McdOptions
	{
		std::string Filename;
		bool Enabled;
		MemoryCardType Type;
	};

	struct AchievementsOptions
	{
		static constexpr u32 MINIMUM_NOTIFICATION_DURATION = 3;
		static constexpr u32 MAXIMUM_NOTIFICATION_DURATION = 30;
		static constexpr u32 DEFAULT_NOTIFICATION_DURATION = 5;
		static constexpr u32 DEFAULT_LEADERBOARD_DURATION = 10;

		static const char* OverlayPositionNames[(size_t)AchievementOverlayPosition::MaxCount + 1];

		BITFIELD32()
		bool
			Enabled : 1,
			HardcoreMode : 1,
			EncoreMode : 1,
			SpectatorMode : 1,
			UnofficialTestMode : 1,
			Notifications : 1,
			LeaderboardNotifications : 1,
			SoundEffects : 1,
			InfoSound : 1,
			UnlockSound : 1,
			LBSubmitSound : 1,
			Overlays : 1,
			LBOverlays : 1;
		BITFIELD_END

		u32 NotificationsDuration = DEFAULT_NOTIFICATION_DURATION;
		u32 LeaderboardsDuration = DEFAULT_LEADERBOARD_DURATION;
		AchievementOverlayPosition OverlayPosition = AchievementOverlayPosition::BottomRight;
		OsdOverlayPos NotificationPosition = OsdOverlayPos::TopLeft;

		std::string InfoSoundName;
		std::string UnlockSoundName;
		std::string LBSubmitSoundName;

		AchievementsOptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const AchievementsOptions& right) const;
		bool operator!=(const AchievementsOptions& right) const;
	};

	struct SavestateOptions
	{
		SavestateOptions();
		void LoadSave(SettingsWrapper& wrap);

		SavestateCompressionMethod CompressionType = SavestateCompressionMethod::Zstandard;
		SavestateCompressionLevel CompressionRatio = SavestateCompressionLevel::Medium;

		bool operator==(const SavestateOptions& right) const;
		bool operator!=(const SavestateOptions& right) const;
	};

	struct VROptions
	{
		bool Enable = true;

		float ScreenDistance = 2.0f;
		float ScreenHeight = 1.4f;

		float ScreenVerticalOffset = 0.0f;

		float ScreenArcDeg = 100.0f;

		bool StereoMode = true;
		bool StereoUseProfile = true;
		float StereoSeparation = 0.02f;
		float StereoConvergence = 20.0f;

		bool HeadCamera = true;

		VROptions();
		void LoadSave(SettingsWrapper& wrap);

		bool operator==(const VROptions& right) const;
		bool operator!=(const VROptions& right) const;
	};

	BITFIELD32()
	bool
		CdvdVerboseReads : 1,
		CdvdDumpBlocks : 1,
		CdvdPrecache : 1,
		EnablePatches : 1,
		EnableCheats : 1,
		EnablePINE : 1,
		EnableWideScreenPatches : 1,
		EnableNoInterlacingPatches : 1,
		EnableFastBoot : 1,
		EnableFastBootFastForward : 1,
		EnableThreadPinning : 1,
		EnableRecordingTools : 1,
		EnableGameFixes : 1,
		SaveStateOnShutdown : 1,
		EnableDiscordPresence : 1,
		UseSavestateSelector : 1,
		InhibitScreensaver : 1,
		BackupSavestate : 1,
		ManuallySetRealTimeClock : 1,
		UseSystemLocaleFormat : 1,

		HostFs : 1,

		WarnAboutUnsafeSettings : 1;
	BITFIELD_END

	CpuOptions Cpu;
	GSOptions GS;
	SpeedhackOptions Speedhacks;
	GamefixOptions Gamefixes;
	ProfilerOptions Profiler;
	DebugAnalysisOptions DebuggerAnalysis;
	EmulationSpeedOptions EmulationSpeed;
	SavestateOptions Savestate;
	SPU2Options SPU2;
	DEV9Options DEV9;
	USBOptions USB;
	PadOptions Pad;

	TraceLogFilters Trace;

	FilenameOptions BaseFilenames;

	AchievementsOptions Achievements;

	VROptions VR;

	McdOptions Mcd[8];
	std::string GzipIsoIndexTemplate;

	int PINESlot;

	int RtcYear;
	int RtcMonth;
	int RtcDay;
	int RtcHour;
	int RtcMinute;
	int RtcSecond;

	std::string CurrentBlockdump;
	std::string CurrentIRX;
	std::string CurrentGameArgs;
	std::string CustomDataPath;
	AspectRatioType CurrentAspectRatio = AspectRatioType::RAuto4_3_3_2;
	float CurrentCustomAspectRatio = 0.f;
	bool IsPortableMode = false;

	Pcsx2Config();
	void LoadSave(SettingsWrapper& wrap);
	void LoadSaveCore(SettingsWrapper& wrap);
	void LoadSaveMemcards(SettingsWrapper& wrap);

	void ReloadPatchAffectingOptions();

	std::string FullpathToBios() const;
	std::string FullpathToMcd(uint slot) const;

	bool operator==(const Pcsx2Config& right) const = delete;
	bool operator!=(const Pcsx2Config& right) const = delete;

	void CopyRuntimeConfig(Pcsx2Config& cfg);

	static void CopyConfiguration(SettingsInterface* dest_si, SettingsInterface& src_si);

	static void ClearConfiguration(SettingsInterface* dest_si);

	static void ClearInvalidPerGameConfiguration(SettingsInterface* si);
};

extern Pcsx2Config EmuConfig;

namespace EmuFolders
{
	extern std::string AppRoot;
	extern std::string DataRoot;
	extern std::string Settings;
	extern std::string Bios;
	extern std::string Snapshots;
	extern std::string Savestates;
	extern std::string MemoryCards;
	extern std::string Logs;
	extern std::string Cheats;
	extern std::string Patches;
	extern std::string Resources;
	extern std::string UserResources;
	extern std::string Cache;
	extern std::string Covers;
	extern std::string GameSettings;
	extern std::string Textures;
	extern std::string InputProfiles;
	extern std::string Videos;
	extern std::string DebuggerLayouts;
	extern std::string DebuggerSettings;
	extern std::string VRProfiles;

	void SetAppRoot();
	bool SetResourcesDirectory();
	bool SetDataDirectory(Error* error);

	void SetDefaults(SettingsInterface& si);
	void LoadConfig(SettingsInterface& si);
	bool EnsureFoldersExist();

	std::FILE* OpenLogFile(std::string_view name, const char* mode);

	std::string GetOverridableResourcePath(std::string_view name);
}

#ifdef _M_X86
#define REC_VU1 (EmuConfig.Cpu.Recompiler.EnableVU1)
#define THREAD_VU1 (REC_VU1 && EmuConfig.Speedhacks.vuThread)
#else
#define THREAD_VU1 false
#define REC_VU1 false
#endif
#define INSTANT_VU1 (EmuConfig.Speedhacks.vu1Instant)
#define CHECK_EEREC (EmuConfig.Cpu.Recompiler.EnableEE)
#define CHECK_CACHE (EmuConfig.Cpu.Recompiler.EnableEECache)
#define CHECK_IOPREC (EmuConfig.Cpu.Recompiler.EnableIOP)
#define CHECK_FASTMEM (EmuConfig.Cpu.Recompiler.EnableEE && EmuConfig.Cpu.Recompiler.EnableFastmem)
#define CHECK_EXTRAMEM (memGetExtraMemMode())

#define CHECK_VUADDSUBHACK (EmuConfig.Gamefixes.VuAddSubHack)
#define CHECK_FPUMULHACK (EmuConfig.Gamefixes.FpuMulHack)
#define CHECK_XGKICKHACK (EmuConfig.Gamefixes.XgKickHack)
#define CHECK_EETIMINGHACK (EmuConfig.Gamefixes.EETimingHack)
#define CHECK_INSTANTDMAHACK (EmuConfig.Gamefixes.InstantDMAHack)
#define CHECK_SKIPMPEGHACK (EmuConfig.Gamefixes.SkipMPEGHack)
#define CHECK_OPHFLAGHACK (EmuConfig.Gamefixes.OPHFlagHack)
#define CHECK_DMABUSYHACK (EmuConfig.Gamefixes.DMABusyHack)
#define CHECK_VIFFIFOHACK (EmuConfig.Gamefixes.VIFFIFOHack)
#define CHECK_VIF1STALLHACK (EmuConfig.Gamefixes.VIF1StallHack)
#define CHECK_GIFFIFOHACK (EmuConfig.Gamefixes.GIFFIFOHack)
#define CHECK_VUOVERFLOWHACK (EmuConfig.Gamefixes.VUOverflowHack)
#define CHECK_FULLVU0SYNCHACK (EmuConfig.Gamefixes.FullVU0SyncHack)

#define CHECK_VU_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0Overflow : EmuConfig.Cpu.Recompiler.vu1Overflow)
#define CHECK_VU_EXTRA_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0ExtraOverflow : EmuConfig.Cpu.Recompiler.vu1ExtraOverflow)
#define CHECK_VU_SIGN_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0SignOverflow : EmuConfig.Cpu.Recompiler.vu1SignOverflow)
#define CHECK_VU_UNDERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0Underflow : EmuConfig.Cpu.Recompiler.vu1Underflow)

#define CHECK_FPU_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuOverflow)
#define CHECK_FPU_EXTRA_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuExtraOverflow)
#define CHECK_FPU_EXTRA_FLAGS 1
#define CHECK_FPU_FULL (EmuConfig.Cpu.Recompiler.fpuFullMode)

#define SHIFT_RECOMPILE
#define BRANCH_RECOMPILE

#define ARITHMETICIMM_RECOMPILE
#define ARITHMETIC_RECOMPILE
#define MULTDIV_RECOMPILE
#define JUMP_RECOMPILE
#define LOADSTORE_RECOMPILE
#define MOVE_RECOMPILE
#define MMI_RECOMPILE
#define MMI0_RECOMPILE
#define MMI1_RECOMPILE
#define MMI2_RECOMPILE
#define MMI3_RECOMPILE
#define FPU_RECOMPILE
#define CP0_RECOMPILE
#define CP2_RECOMPILE

#ifndef ARITHMETIC_RECOMPILE
#undef ARITHMETICIMM_RECOMPILE
#endif

#define EE_CONST_PROP 1

#define PSX_EXTRALOGS 0

#undef BITFIELD32
#undef BITFIELD_END
