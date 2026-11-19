// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

#include <array>

namespace VR::GloveHaptics
{
	inline constexpr u8 kLocusWholeHand = 0x00;
	inline constexpr u8 kLocusFingertips = 0x1F;
	inline constexpr u8 kLocusPalm = 0xC0;
	inline constexpr u8 kLocusHeavy = 0xE0;
	inline constexpr u8 kLocusPinch = 0x03;
	inline constexpr u8 kLocusGrip = 0xDF;
	inline constexpr u8 kLocusHand = 0xFF;

	inline constexpr u16 kPort = 27460;

	std::array<u8, 8> Encode(int hand, u8 locus, float amplitude, float seconds);

	bool Enabled();

	void Send(int hand, u8 locus, float amplitude, float seconds);

	using SinkFn = void (*)(int hand, u8 locus, float amplitude, float seconds);
	void SetSinkForTest(SinkFn sink);

	bool SendToForTest(u16 port, int hand, u8 locus, float amplitude, float seconds);
}
