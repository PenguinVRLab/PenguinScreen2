// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::SplitState
{
	struct Snapshot
	{
		bool split_active = false;
		float rect_x[2] = {0.0f, 0.0f};
		float rect_y[2] = {0.0f, 0.0f};
		float rect_w[2] = {1.0f, 1.0f};
		float rect_h[2] = {1.0f, 1.0f};
		u8 local_view = 0;
		u8 local_pad_port = 0;
		u8 mode = 0;
		float side_scale = 0.4f;
		float side_angle_deg = 35.0f;
		bool stereo_on = true;
	};

	void Apply();

	void InvalidateMemo();

	Snapshot Get();

	bool Active();

	float SepScale();
}
