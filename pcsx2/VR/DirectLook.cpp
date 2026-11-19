// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/CameraDriver.h"
#include "VR/VRProfileDB.h"

#include <algorithm>
#include <cmath>

namespace VR::CameraDriver
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		float ClampAngle(float angle_rad, float limit_deg)
		{
			const float limit = limit_deg * (kPi / 180.0f);
			const float a = std::isfinite(angle_rad) ? angle_rad : 0.0f;
#ifndef VR_DIRECTLOOK_PLANTED_FAULT_NOCLAMP
			return std::clamp(a, -limit, limit);
#else
			(void)limit;
			return a;
#endif
		}
	}

	DirectLookPlan PlanDirectLever(const ProfileDB::CameraDirectLever& lever, float head_angle_rad)
	{
		using Kind = ProfileDB::CameraDirectLever::Kind;
		DirectLookPlan plan;
		plan.pc = lever.hook_address;
		plan.expect = lever.expect;
		plan.is_view = (lever.kind == Kind::View);
		plan.is_aim = (lever.kind == Kind::Aim);
		plan.reg = lever.reg;
		plan.eye_reg = lever.eye_reg;
		plan.view_address = lever.view_address;
		plan.caller_sp_offset = lever.caller_sp_offset;
		plan.caller = lever.caller;
		plan.angle_rad = ClampAngle(head_angle_rad, lever.max_deg);
		if (lever.kind == Kind::Register)
			plan.units = lever.axis_sign * lever.units_per_turn / (2.0f * kPi);
		else if (plan.is_aim)
			plan.units = lever.carry_yaw ? lever.axis_sign : 0.0f;
		else
			plan.units = lever.axis_sign;
		plan.pitch_units = (plan.is_aim && lever.carry_pitch) ? lever.pitch_sign : 0.0f;
		return plan;
	}

	DirectLookAngles PlanDirectAngles(const ProfileDB::CameraDirectLook& dl, float head_yaw_rad, float head_pitch_rad)
	{
		float yaw_limit = 180.0f, pitch_limit = 180.0f;
		if (dl.yaw.has_value())
			yaw_limit = std::min(yaw_limit, dl.yaw->max_deg);
		if (dl.pitch.has_value())
			pitch_limit = std::min(pitch_limit, dl.pitch->max_deg);
		if (dl.aim.has_value() && dl.aim->carry_yaw)
			yaw_limit = std::min(yaw_limit, dl.aim->max_deg);
		if (dl.aim.has_value() && dl.aim->carry_pitch)
			pitch_limit = std::min(pitch_limit, dl.aim->max_pitch_deg);
		DirectLookAngles out;
		out.yaw_rad = ClampAngle(head_yaw_rad, yaw_limit);
		out.pitch_rad = ClampAngle(head_pitch_rad, pitch_limit);
		return out;
	}
}
