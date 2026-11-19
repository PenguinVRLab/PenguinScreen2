// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/SpatialControls.h"

#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "VR/ScreenCurveMode.h"
#include "VR/HandCapture.h"

namespace VR::NearField
{
	enum class Hands : u8;
}

namespace VR::ProfileDB
{
	enum class Tier
	{
		Screen,
		Stereo,
		Immersive,
	};

	enum class UvDrawPolicy
	{
		Screen,
		World,
	};

	enum class StereoMap : u32
	{
		Linear = 0,
		Bands = 1,
		Log = 2,
	};

	struct StereoBand
	{
		float conv = 0.0f;
		float sep = 0.0f;
	};

	struct StereoLogParams
	{
		float w0 = 1.0f;
		float w1 = 1.0f;
		float dfar = 0.0f;
	};

	struct StereoResolvedMap
	{
		StereoMap map = StereoMap::Linear;
		u32 band_count = 1;
		float split_q[3] = {};
		float conv[4] = {};
		float sep[4] = {};
		float bias[4] = {};
		float log_w0 = 0.0f;
		float log_w1 = 0.0f;
		float log_dfar = 0.0f;
		float crossed_limit = 0.0f;
		float crossed_gain = 0.0f;
	};

	u32 SelectBand(const StereoResolvedMap& map, float q);

	float EvalBand(const StereoResolvedMap& map, u32 band, float q);

	float EvalDisparity(const StereoResolvedMap& map, float separation, float convergence, float q);

	struct StereoSceneRule
	{
		u32 ee_address = 0;
		u32 equals = 0;
		u8 width = 4;
		std::optional<float> separation;
		std::optional<float> convergence;

		std::optional<StereoResolvedMap> map_override;

		std::string label;
	};

	struct CollimateRule
	{
		s8 prim = -1;
		s8 tme = 1;
		s8 abe = -1;
		s32 min_w = 0;
		s32 max_w = 0;
		s32 min_h = 0;
		s32 max_h = 0;
		float rx0 = 0.0f, ry0 = 0.0f, rx1 = 0.0f, ry1 = 0.0f;
		float tu0 = 0.0f, tv0 = 0.0f, tu1 = 0.0f, tv1 = 0.0f;
		std::string label;
	};

	static constexpr u32 kMaxCollimateRules = 4;

	struct HudCollimate
	{
		float disparity = 0.0f;
		std::vector<CollimateRule> rules;
	};

	struct StereoParams
	{
		float separation = 0.0f;
		float convergence = 0.0f;
		UvDrawPolicy uv_draws = UvDrawPolicy::Screen;

		bool pin_uniform_q = false;

		bool z_driven_depth = false;

		std::vector<StereoSceneRule> scenes;

		std::optional<HudCollimate> hud_collimate;

		StereoMap map = StereoMap::Linear;
		std::vector<float> splits;
		std::vector<StereoBand> bands;
		std::optional<StereoLogParams> log_params;

		StereoResolvedMap resolved;
	};

	enum class CameraEncoding
	{
		F32,
		S16_12,
		S32Angle,
	};

	enum class CameraSource
	{
		Constant,
		HeadYaw,
		HeadPitch,
		HeadRoll,
		HeadX,
		HeadY,
		HeadZ,
	};

	enum class CameraCompose
	{
		Absolute,
		Delta,
		Anchored,
	};

	enum class CameraWrap
	{
		None,
		Deg360,
	};

	enum class CameraClass
	{
		FullTracking,
		RotationOnly,
		TranslationOnly,
		FixedProjection,
		HybridEnvironment,
	};

	const char* CameraClassName(CameraClass c);

	struct CameraGuard
	{
		u32 ee_address = 0;
		u32 equals = 0;
		u8 width = 4;
		bool not_equals = false;
		bool is_chain = false;
		u32 pointer_addr = 0;
		std::vector<s32> deref_offsets;
		s32 offset = 0;

		bool operator==(const CameraGuard&) const = default;
	};

	struct CameraWriteOp
	{
		u32 ee_address = 0;
		bool relative = false;
		CameraEncoding encoding = CameraEncoding::F32;
		CameraSource source = CameraSource::Constant;
		CameraCompose compose = CameraCompose::Absolute;
		CameraWrap wrap = CameraWrap::None;
		float scale = 1.0f;
		float bias = 0.0f;
		float clamp_min = -std::numeric_limits<float>::infinity();
		float clamp_max = std::numeric_limits<float>::infinity();
		float axis_sign = 1.0f;
		bool anchor_head = true;
		bool fitted_angles = true;
		std::vector<CameraGuard> when;
		bool head_only_projected = false;
	};

	enum class MatrixComposeOrder
	{
		Pre,
		Post,
	};

	struct CameraMatrixOp
	{
		u32 ee_address = 0;
		bool relative = false;
		bool anchored = false;
		MatrixComposeOrder order = MatrixComposeOrder::Pre;
		bool also_transpose = false;
		u32 transpose_address = 0;
		float axis_sign_yaw = 1.0f;
		float axis_sign_pitch = 1.0f;
		float axis_sign_roll = 1.0f;
		float yaw_min_deg = -std::numeric_limits<float>::infinity();
		float yaw_max_deg = std::numeric_limits<float>::infinity();
		float pitch_min_deg = -std::numeric_limits<float>::infinity();
		float pitch_max_deg = std::numeric_limits<float>::infinity();
		float roll_min_deg = -std::numeric_limits<float>::infinity();
		float roll_max_deg = std::numeric_limits<float>::infinity();
		std::vector<CameraGuard> when;
	};

	struct CameraBase
	{
		std::vector<u8> pattern;
		std::vector<u8> mask;
		u32 scan_start = 0x00100000;
		u32 scan_end = 0x02000000;
		bool is_pointer = false;
		u32 pointer_addr = 0;
		std::vector<s32> deref_offsets;
		bool is_indexed = false;
		u32 indexed_base = 0;
		s64 array_offset = 0;
		u32 index_addr = 0;
		u8 index_width = 1;
		u32 stride = 0;
		u32 count = 0;
		s64 base_offset = 0;
		bool has_validate = false;
		s64 validate_offset = 0;
		u32 validate_equals = 0;
	};

	using IndexedRead32 = bool (*)(u32 address, u32* value);
	std::optional<u32> ResolveIndexedBase(const CameraBase& spec, u32 index, IndexedRead32 read32);

	struct CameraSilence
	{
		u32 ee_address = 0;
		u32 value_on = 0;
		u32 value_off = 0;
	};

	struct CameraCodeHook
	{
		bool enabled = false;
		u32 hook_address = 0;
		u32 cave_address = 0;
		u32 scratch_address = 0;
		u32 tail_jump_address = 0;
		CameraSource source = CameraSource::HeadYaw;
		float scale = 1.0f;
		float axis_sign = 1.0f;
		u8 target_fpr = 13;
		u8 scratch_fpr = 1;
		u8 addr_gpr = 1;
	};

	struct CameraPadLook
	{
		float max_look_deg = 90.0f;
		float engage_deg = 12.0f;
		float curve = 1.0f;
		bool latch = false;

		float release_deg = 4.0f;

		float stick_floor = 0.0f;
	};

	struct CameraDirectLever
	{
		enum class Kind : u8
		{
			Register,
			View,
			Aim,
		};
		Kind kind = Kind::Register;
		u32 hook_address = 0;
		u32 expect = 0;
		u8 reg = 0;
		u8 eye_reg = 0;
		float pitch_sign = 1.0f;
		float max_pitch_deg = 30.0f;
		bool carry_yaw = true;
		bool carry_pitch = true;
		u32 view_address = 0;
		float units_per_turn = 65536;
		float axis_sign = 1.0f;
		float max_deg = 80.0f;
		u32 caller_sp_offset = 0;
		u32 caller = 0;
	};

	struct CameraDirectLook
	{
		std::optional<CameraDirectLever> yaw;
		std::optional<CameraDirectLever> pitch;
		std::optional<CameraDirectLever> aim;
		std::vector<CameraGuard> when;
		bool fitted_angles = false;
	};

	struct CameraProfile
	{
		std::vector<CameraWriteOp> writes;
		std::vector<CameraMatrixOp> matrix_writes;
		std::optional<CameraBase> base;
		std::vector<CameraGuard> guards;
		std::vector<CameraSilence> silence;
		std::vector<CameraCodeHook> code_hooks;
		std::optional<CameraPadLook> pad_look;
		std::optional<CameraDirectLook> direct_look;
		bool requires_world_projection = false;

		bool ArmsWithWorldProjection(bool projection_on) const
		{
#if defined(VR_CAMERA_PLANTED_IGNORE_WORLDPROJ)
			(void)projection_on;
			return true;
#else
			return !requires_world_projection || projection_on;
#endif
		}
	};

	enum class FovModel
	{
		Tan,
		InvTan,
	};

	struct FovCalibration
	{
		FovModel model = FovModel::Tan;
		std::vector<std::pair<float, float>> pairs;
		float k = 0.0f;
		float fit_error_deg = 0.0f;
	};

	struct FovLever
	{
		std::string name;
		u32 address = 0;
		CameraEncoding encoding = CameraEncoding::F32;
		std::optional<CameraBase> base;
		std::optional<FovCalibration> calibration;
		bool has_readback = false;
		u32 readback_address = 0;
	};

	enum class FovTargetKind
	{
		Degrees,
		Screen,
		Game,
		Widen,
		Value,
	};

	struct FovTarget
	{
		FovTargetKind kind = FovTargetKind::Game;
		float value = 0.0f;
	};

	struct FovScene
	{
		std::string label;
		std::vector<CameraGuard> when;
		size_t lever = 0;
		FovTarget target;
		float ramp_seconds = 0.0f;
	};

	struct FovProfile
	{
		std::vector<FovLever> levers;
		std::vector<FovScene> scenes;
		FovTarget default_target;
	};

	struct SpatialControlSpec
	{
		std::string device;
		SpatialControls::DeviceKind kind = SpatialControls::DeviceKind::Count;
		std::string id;
		struct Placement
		{
			float side = 0.0f;
			float height = 0.0f;
			float forward = 0.0f;
			float yaw_deg = 0.0f;

			bool operator==(const Placement&) const = default;
		} placement;
		float travel = 0.12f;
		float sweep_deg = 60.0f;
		float arm_length = 0.20f;
		bool upright = false;
		float grab_radius = 0.05f;
		float grab_length = 0.12f;
		SpatialControls::GrabMode grab_mode = SpatialControls::GrabMode::Toggle;
		float break_away = 0.25f;
		SpatialControls::ReleaseMode release = SpatialControls::ReleaseMode::Latch;
		float spring_rate = 4.0f;
		bool one_way = false;
		int detented = 0;
		SpatialControls::PowerMode power = SpatialControls::PowerMode::Sum;
		float engage = 0.0f;
		SpatialControls::TwinMode mode = SpatialControls::TwinMode::Composed;
		float lever_sign = 1.0f;
		float steer_full_lock = 1.0f;
		float steer_deadband = 0.05f;
		float steer_curve = 1.0f;
		float steer_sign = 1.0f;
		float output_floor = 0.0f;
		float lock_to_lock_deg = 900.0f;
		float rim_radius = 0.18f;
		int gears = 6;
		bool has_reverse = true;
		bool has_neutral = true;
		float travel_x = 0.10f;
		float travel_y = 0.10f;
		bool twist = false;
		float twist_range_deg = 60.0f;
		bool rifle = false;
		bool aim_left = false;
		std::string preset;
		std::vector<CameraGuard> when;
		struct Bind
		{
			std::string control;
			std::string target;

			bool operator==(const Bind&) const = default;
		};
		std::vector<Bind> bind;
		u32 pad_port = 0;
		int usb_port = -1;

		bool operator==(const SpatialControlSpec&) const = default;
	};

	struct SplitParams
	{
		enum class Layout : u8
		{
			Horizontal,
			Vertical,
		};
		enum class Mode : u8
		{
			Focus,
			Duo,
			Mirror,
			Local,
		};
		struct Probe
		{
			u32 ee_address = 0;
			u32 threshold = 0;
			bool at_least = false;
			u8 width = 4;
			bool Hit(u32 value) const { return at_least ? (value >= threshold) : (value == threshold); }
		};
		struct Rect
		{
			float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;
		};

		Layout layout = Layout::Horizontal;
		u8 views = 2;
		u8 local_view = 0;
		u8 local_pad_port = 0;
		Rect rects[2];
		bool rects_explicit = false;
		std::optional<Probe> active;
		std::vector<Probe> also;
		template <typename Read>
		bool ActiveHit(Read&& read) const
		{
			if (!active.has_value() || !active->Hit(read(active.value())))
				return false;
#if !defined(VR_SPLIT_PLANTED_FIRST_PROBE_ONLY)
			for (const Probe& p : also)
				if (!p.Hit(read(p)))
					return false;
#endif
			return true;
		}
		Mode mode = Mode::Focus;
		float side_scale = 0.4f;
		float side_angle_deg = 35.0f;
		bool stereo_on = true;
	};

	struct ProfileRequirements
	{
		std::optional<bool> extra_memory;
		bool Any() const { return extra_memory.has_value(); }
	};

	struct ProfileRecommendations
	{
		std::string textures;
	};

	struct ModIdentity
	{
		std::string name;
		std::string version;
		std::string by;
		std::string url;
		std::string file;
	};

	struct ProfileTextures
	{
		std::string pack;
		std::optional<bool> classic_names;
	};

	struct ProjectionProfile
	{
		enum class Drawn : u8
		{
			Copy = 0,
			Matrix = 1,
			Hook = 2,
			LookAt = 3,
		};
		enum class HookBody : u8
		{
			Pre = 0,
			Rest = 1,
		};
		enum class Body : u8
		{
			Anchored = 0,
			Identity = 1,
			MinusOwn = 2,
		};
		enum class FrustumModel : u8
		{
			Degrees = 0,
			Constant = 1,
		};

		bool general = false;
		Drawn drawn = Drawn::Copy;
		u32 yaw_address = 0;
		std::optional<u32> pitch_address;
		std::optional<u32> fov_readback;
		std::vector<CameraGuard> when;
		std::optional<u32> yaw_op;
		std::optional<u32> pitch_op;
		float yaw_sign = 1.0f;
		float pitch_sign = 1.0f;
		float tan_x = 0.0f;
		float tan_y = 0.0f;
		u32 frame_age = 0;
		bool view = false;
		Body yaw_body = Body::Anchored;
		Body pitch_body = Body::Anchored;
		u32 matrix_address = 0;
		u32 look_eye_offset = 0;
		u32 look_target_offset = 0;
		u32 matrix_op = 0;
		bool roll_declared = false;
		FrustumModel frustum_model = FrustumModel::Degrees;
		u32 write_to_read = 1;
		u32 hook_pc = 0;
		u32 hook_expect = 0;
		u8 hook_reg = 0;
		u32 hook_offset = 0;
		s8 hook_basis[3] = {1, 2, 3};
		HookBody hook_body = HookBody::Pre;
		u32 lever_address = 0;
		u32 visit_max_age = 4;
		std::vector<u32> frame_ages;
		bool latch_first_draw = false;
		u32 read_to_draw = 0;
		bool latch_closing = false;
	};

	struct Profile
	{
		std::string serial;
		std::optional<ModIdentity> mod;
		std::string name;
		std::vector<u32> crcs;
		std::string based_on;
		ProfileRequirements requirements;
		ProfileRecommendations recommendations;
		ProfileTextures textures;
		Tier tier = Tier::Screen;
		std::optional<StereoParams> stereo;
		std::optional<float> screen_distance;
		std::optional<float> screen_height;
		std::optional<float> screen_arc_deg;
		std::optional<bool> screen_follow_head;
		std::optional<float> screen_lag_ms;
		std::optional<ScreenCurve::Fallback> screen_curve_fallback;
		std::optional<ScreenCurve::Force> screen_force_fallback;
		std::optional<float> screen_projector_supersample;
		std::optional<bool> screen_projector_depth;
		std::optional<CameraProfile> camera;
		std::optional<CameraClass> camera_class;
		std::optional<FovProfile> fov;
		std::optional<ProjectionProfile> projection;
		std::optional<SplitParams> split;
		std::vector<SpatialControlSpec> controls;
		VR::NearField::Hands hands{};
		std::optional<VR::HandCapture::Recipe> game_hands;
		std::string notes;
		std::string source_path;
		bool from_user_folder = false;
	};

	void EnsureLoaded();

	void ReloadIfChanged();

	struct LoadIssue
	{
		std::string file;
		std::string message;
	};

	const std::vector<LoadIssue>& ValidateAtLaunch();

	enum class StereoRail
	{
		Divergence,
		FixationGap,
	};

	struct StereoRailFinding
	{
		StereoRail rail = StereoRail::Divergence;
		std::string serial;
		std::string site;
		float arcmin = 0.0f;
		bool from_map = false;
		std::string message;
	};

	const std::vector<StereoRailFinding>& StereoRailFindings();

	struct ProfileAdvisory
	{
		std::string serial;
		std::string site;
		std::string message;
	};

	const std::vector<ProfileAdvisory>& ProfileAdvisories();

	struct Summary
	{
		std::string serial;
		std::string name;
		std::vector<u32> crcs;
		bool has_stereo = false;
		bool has_camera = false;
		float separation = 0.0f;
		float convergence = 0.0f;
		bool conversion = false;
		std::string mod_version;
		std::string source_path;
		bool from_user_folder = false;
	};

	enum class BindOutcome : u8
	{
		NoProfile,
		CrcMatch,
		AnyCrcBase,
		OnlyVariant,
		CrcMiss,
		NeedsCrc,
	};
	struct BindDecision
	{
		BindOutcome outcome = BindOutcome::NoProfile;
		size_t variant = 0;
		bool Bound() const
		{
			return outcome == BindOutcome::CrcMatch || outcome == BindOutcome::AnyCrcBase ||
			       outcome == BindOutcome::OnlyVariant;
		}
	};

	template <typename At>
	BindDecision DecideBinding(size_t count, u32 crc, At at)
	{
		BindDecision d;
		if (count == 0)
			return d;
		if (crc == 0)
		{
			size_t plain = 0, only = 0;
			for (size_t i = 0; i < count; i++)
			{
#ifdef VR_PROFILEDB_PLANTED_CONVERSION_BLIND
				if (true)
#else
				if (!at(i).second)
#endif
				{
					plain++;
					only = i;
				}
			}
			if (plain == 1)
				return BindDecision{BindOutcome::OnlyVariant, only};
			for (size_t i = 0; i < count; i++)
			{
				if (!at(i).second && at(i).first.empty())
					return BindDecision{BindOutcome::AnyCrcBase, i};
			}
			d.outcome = BindOutcome::NeedsCrc;
			return d;
		}
		std::optional<size_t> any;
		for (size_t i = 0; i < count; i++)
		{
			const std::vector<u32>& crcs = at(i).first;
			if (crcs.empty())
			{
				if (!any.has_value())
					any = i;
				continue;
			}
			for (const u32 c : crcs)
			{
				if (c == crc)
					return BindDecision{BindOutcome::CrcMatch, i};
			}
		}
		if (any.has_value())
			return BindDecision{BindOutcome::AnyCrcBase, any.value()};
		d.outcome = BindOutcome::CrcMiss;
		return d;
	}

	std::optional<size_t> BoundSummaryRow(const std::vector<Summary>& rows, std::string_view serial, u32 crc);

	std::string DescribeBinding(std::string_view serial, u32 crc, const std::vector<Profile>& variants, const BindDecision& d);

	u32 BindNoticesLogged();

	std::vector<Summary> ListProfiles();

	std::optional<std::string> ConversionTitle(std::string_view serial, u32 crc);

	const Profile* Lookup(const std::string_view serial, u32 crc);

	struct RequirementGap
	{
		enum class Kind : u8
		{
			ExtraMemory,
		};
		Kind kind = Kind::ExtraMemory;
		const char* section = "";
		const char* key = "";
		bool value = false;
		bool needs_restart = false;
	};

	std::vector<RequirementGap> FindRequirementGaps(const ProfileRequirements& req, bool extra_memory_on);

	struct RequirementOffer
	{
		std::string serial;
		u32 crc = 0;
		std::string name;
		std::string game_settings_path;
		std::vector<RequirementGap> gaps;
	};

	using RequirementOfferHandler = std::function<void(const RequirementOffer&)>;
	void SetRequirementOfferHandler(RequirementOfferHandler handler);

	enum class OfferOutcome : u8
	{
		Nothing,
		AlreadyOffered,
		Logged,
		Offered,
	};

	OfferOutcome OfferRequirements(const Profile* bound, u32 crc, bool extra_memory_on, const std::string& game_settings_path);

	void Reset();

}
