// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::HeadPose
{
	struct Snapshot
	{
		float orientation_x = 0.0f;
		float orientation_y = 0.0f;
		float orientation_z = 0.0f;
		float orientation_w = 1.0f;

		float position_x = 0.0f;
		float position_y = 0.0f;
		float position_z = 0.0f;

		bool valid = false;

		u64 frame = 0;
	};

	void Publish(const Snapshot& pose);

	void Invalidate();

	Snapshot Get();
}
