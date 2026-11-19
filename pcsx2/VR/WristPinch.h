// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/VRInputState.h"

#include <array>
#include <cstdint>

namespace VR::WristPinch
{
	inline constexpr float kPalmToHeadMaxDeg = 45.f;
	inline constexpr float kGazeMaxDeg = 35.f;
	inline constexpr float kPinchCloseM = 0.020f;
	inline constexpr float kPinchOpenM = 0.035f;

	struct HandJoints
	{
		VRPose wrist;
		std::array<float, 3> thumb_tip{};
		std::array<float, 3> index_tip{};
		bool tips_valid = false;
		bool tracked = false;
	};

	struct Params
	{
		float palm_to_head_max_deg = kPalmToHeadMaxDeg;
		bool require_gaze = true;
		float gaze_max_deg = kGazeMaxDeg;
		float pinch_close_m = kPinchCloseM;
		float pinch_open_m = kPinchOpenM;
	};

	struct Output
	{
		bool pressed = false;
		bool held = false;
		bool facing = false;
		bool pinched = false;
	};

	bool IsFacing(const HandJoints& hand, const VRPose& head, const Params& params);

	class Detector
	{
	public:
		Output Step(const HandJoints& hand, const VRPose& head, const Params& params = Params{});
		void Reset();

	private:
		bool m_pinched = true;
		bool m_held = false;
	};
}
