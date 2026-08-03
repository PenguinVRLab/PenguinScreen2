// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <tuple>

class PadData
{
public:
	PadData(const int port, const int slot);
	PadData(const int port, const int slot, const std::array<u8, 18> data);

	static constexpr u8 ANALOG_VECTOR_NEUTRAL = 127;

	int m_ext_port;
	int m_port;
	int m_slot;

	std::tuple<u8, u8> m_rightAnalog = {ANALOG_VECTOR_NEUTRAL, ANALOG_VECTOR_NEUTRAL};
	std::tuple<u8, u8> m_leftAnalog = {ANALOG_VECTOR_NEUTRAL, ANALOG_VECTOR_NEUTRAL};

	u8 m_compactPressFlagsGroupOne = 255;
	u8 m_compactPressFlagsGroupTwo = 255;

	std::tuple<bool, u8> m_circle = {false, 0};
	std::tuple<bool, u8> m_cross = {false, 0};
	std::tuple<bool, u8> m_square = {false, 0};
	std::tuple<bool, u8> m_triangle = {false, 0};

	std::tuple<bool, u8> m_down = {false, 0};
	std::tuple<bool, u8> m_left = {false, 0};
	std::tuple<bool, u8> m_right = {false, 0};
	std::tuple<bool, u8> m_up = {false, 0};

	std::tuple<bool, u8> m_l1 = {false, 0};
	std::tuple<bool, u8> m_l2 = {false, 0};
	std::tuple<bool, u8> m_r1 = {false, 0};
	std::tuple<bool, u8> m_r2 = {false, 0};

	bool m_start = false;
	bool m_select = false;
	bool m_l3 = false;
	bool m_r3 = false;

	void OverrideActualController() const;

	void LogPadData() const;
};

