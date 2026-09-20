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

		float collimate_disparity = 0.0f;

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
			char label[16] = {};
		};
		static constexpr u32 MAX_COLLIMATE_RULES = 4;
		CollimateRule collimate_rules[MAX_COLLIMATE_RULES] = {};
		u32 collimate_rule_count = 0;

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
