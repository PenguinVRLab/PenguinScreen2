// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::StereoState
{
	struct Params
	{
		bool  enabled     = false;
		float separation  = 0.0f;
		float convergence = 0.0f;

		enum class UvPolicy { Screen, World } uv_policy = UvPolicy::Screen;

		bool pin_uniform_q = false;
	};

	void Publish(const Params& params);

	Params Get();

	u32 GetCurrentEye();

	float GetCurrentEyeSign();

	void AdvanceEye();
}
