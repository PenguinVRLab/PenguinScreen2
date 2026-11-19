// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/HeadPose.h"

#include "common/Pcsx2Types.h"

#include <utility>
#include <vector>

namespace VR::ProfileDB
{
	struct CameraPadLook;
	struct CameraDirectLever;
	struct CameraDirectLook;
	struct CameraMatrixOp;
}

namespace VR::CameraDriver
{
	struct PadLookState
	{
		int gate = 0;
		int latch = 0;
	};

	float PadLookDeflection(const ProfileDB::CameraPadLook& pl, float yaw_deg, PadLookState& st);

	struct DirectLookPlan
	{
		u32 pc = 0;
		u32 expect = 0;
		bool is_view = false;
		bool is_aim = false;
		u8 reg = 0;
		u8 eye_reg = 0;
		u32 view_address = 0;
		float units = 0.0f;
		float pitch_units = 0.0f;
		float angle_rad = 0.0f;
		u32 caller_sp_offset = 0;
		u32 caller = 0;
	};
	DirectLookPlan PlanDirectLever(const ProfileDB::CameraDirectLever& lever, float head_angle_rad);

	struct DirectLookAngles
	{
		float yaw_rad = 0.0f;
		float pitch_rad = 0.0f;
	};
	DirectLookAngles PlanDirectAngles(const ProfileDB::CameraDirectLook& dl, float head_yaw_rad, float head_pitch_rad);

	struct MatrixHeadBaseline
	{
		float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
		bool has = false;
		int held[3] = {};
		float last_view[3] = {};
		bool view_has[3] = {};
		void Reset() { *this = MatrixHeadBaseline{}; }
		void OnGap() { Reset(); }
	};

	bool MatrixOpAngles(const ProfileDB::CameraMatrixOp& op, MatrixHeadBaseline& prev,
		float head_yaw_rad, float head_pitch_rad, float head_roll_rad, float out[3]);

	struct DeltaStep
	{
		float value = 0.0f;
		float applied = 0.0f;
	};
	DeltaStep ComposeDelta(float game, const float* hold, float d, bool wrap360, float clamp_min, float clamp_max);

	void HeadEulerYXZ(const float q[4], float out[3]);

	void Apply();


	void RequestRecenter();

	HeadPose::Snapshot VsyncHead();

	bool AnchoredBase(size_t op_index, float* base);

	bool DeltaTotal(size_t op_index, float* total);

	bool ResolvedBaseNow(u32* base);

	bool Reference(float orientation[4], float position[3]);

	bool HeadRelative(float orientation[4], u64* sample);

	void OnStateLoaded();

}
