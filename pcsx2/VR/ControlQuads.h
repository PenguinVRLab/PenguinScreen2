// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <vector>

namespace VR::ControlQuads
{
	static constexpr int kSlots = 2;
	static constexpr int kLeft = 0;
	static constexpr int kRight = 1;

	static constexpr u32 kWidth = 256;
	static constexpr u32 kHeight = 320;
	static constexpr float kQuadWidthM = 0.20f;
	static constexpr float kQuadHeightM = 0.25f;

	struct Slot
	{
		bool active = false;
		float t = 0.0f;
		bool grabbed = false;
		bool broke_away = false;
		float side = 0.0f;
		float height = 0.0f;
		float forward = 0.0f;
		float yaw_deg = 0.0f;
	};

	static constexpr float kBreakAwayFlashSeconds = 0.6f;

	void Publish(int slot, const Slot& state);
	void ClearAll();
	Slot Get(int slot);

	u32 RasterKey(float t, bool grabbed, bool broke_away = false);

	void RasterLever(std::vector<u32>& out, float t, bool grabbed, bool broke_away = false, int slot = kLeft);

	constexpr u32 PackRgba(u32 r, u32 g, u32 b, u32 a = 255)
	{
		return (a << 24) | (b << 16) | (g << 8) | r;
	}
	static constexpr u32 kColourGripIdle = PackRgba(0x9a, 0xa0, 0xac);
	static constexpr u32 kColourGripGrab = PackRgba(0x7c, 0xc0, 0xff);
	static constexpr u32 kColourHand = PackRgba(0xff, 0xd6, 0xa8);
	static constexpr u32 kColourBreakAway = PackRgba(0xff, 0xa0, 0x30);
}
