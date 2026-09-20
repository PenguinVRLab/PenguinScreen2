// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

class GSTexture;

namespace VR::XRCompositor
{
	bool Initialize();

	void Shutdown();

	inline constexpr u32 MonoEye = 2;

	void EndOfFrame(GSTexture* current, u32 eye);

	void UpdateScreenParams(float distance_m, float height_m, float arc_deg, float vertical_offset_m,
		bool follow_head = false);

	void RequestScreenReanchor();
}
