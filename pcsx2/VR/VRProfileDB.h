// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

	struct StereoSceneRule
	{
		u32 ee_address = 0;
		u32 equals = 0;
		u8 width = 4;
		std::optional<float> separation;
		std::optional<float> convergence;
		std::string label;
	};

	struct StereoParams
	{
		float separation = 0.0f;
		float convergence = 0.0f;
		UvDrawPolicy uv_draws = UvDrawPolicy::Screen;

		bool pin_uniform_q = false;

		std::vector<StereoSceneRule> scenes;
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

	struct CameraGuard
	{
		u32 ee_address = 0;
		u32 equals = 0;
		u8 width = 4;
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
		std::vector<CameraGuard> when;
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
		std::vector<CameraGuard> when;
	};

	struct CameraFov
	{
		u32 ee_address = 0;
		CameraEncoding encoding = CameraEncoding::F32;
		float scale = 1.0f;
	};

	struct CameraBase
	{
		std::vector<u8> pattern;
		std::vector<u8> mask;
		u32 scan_start = 0x00100000;
		u32 scan_end = 0x02000000;
		bool is_pointer = false;
		u32 pointer_addr = 0;
		bool is_indexed = false;
		u32 indexed_base = 0;
		s64 array_offset = 0;
		u32 index_addr = 0;
		u8 index_width = 1;
		u32 stride = 0;
		s64 base_offset = 0;
		bool has_validate = false;
		s64 validate_offset = 0;
		u32 validate_equals = 0;
	};

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
	};

	struct CameraProfile
	{
		std::vector<CameraWriteOp> writes;
		std::vector<CameraMatrixOp> matrix_writes;
		std::optional<CameraFov> fov;
		std::optional<CameraBase> base;
		std::vector<CameraGuard> guards;
		std::vector<CameraSilence> silence;
		std::vector<CameraCodeHook> code_hooks;
		std::optional<CameraPadLook> pad_look;
	};

	struct Profile
	{
		std::string serial;
		std::string name;
		std::vector<u32> crcs;
		Tier tier = Tier::Screen;
		std::optional<StereoParams> stereo;
		std::optional<float> screen_distance;
		std::optional<float> screen_height;
		std::optional<float> screen_arc_deg;
		std::optional<CameraProfile> camera;
		std::string notes;
	};

	void EnsureLoaded();

	void ReloadIfChanged();

	struct LoadIssue
	{
		std::string file;
		std::string message;
	};

	const std::vector<LoadIssue>& ValidateAtLaunch();

	struct Summary
	{
		std::string serial;
		std::string name;
		bool has_stereo = false;
		bool has_camera = false;
		float separation = 0.0f;
		float convergence = 0.0f;
	};

	std::vector<Summary> ListProfiles();

	const Profile* Lookup(const std::string_view serial, u32 crc);

	void Reset();
}
