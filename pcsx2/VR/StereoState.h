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

		enum class Map : u32
		{
			Linear = 0,
			Bands  = 1,
			Log    = 2,
		};

		Map map = Map::Linear;
		u32 band_count = 1;

		float split_q[3] = {};
		float conv[4] = {};
		float sep[4] = {};
		float bias[4] = {};

		float log_w0 = 0.0f;
		float log_w1 = 0.0f;
		float log_dfar = 0.0f;

	};

	void Publish(const Params& params);

	Params Get();

	u32 GetCurrentEye();

	float GetCurrentEyeSign();

	void AdvanceEye();
}
