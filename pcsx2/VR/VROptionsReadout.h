// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <optional>
#include <string>
#include <vector>

namespace VR::OptionsReadout
{
	enum class Source : u8
	{
		Global,
		GameSettings,
		Profile,
		Default,
		NotAvailable,
	};

	template <typename T>
	struct Setting
	{
		T value{};
		bool from_game = false;
	};

	struct Facts
	{
		bool vr_on = false;
		bool running = false;

		Setting<bool> stereo_mode{true}, stereo_use_profile{true}, head_camera{true}, game_fov{true}, world_projection{false},
			follows_patch_aspect{false};
		Setting<float> stereo_separation{0.02f}, stereo_convergence{20.0f}, screen_distance{2.0f}, screen_height{1.4f},
			screen_vertical_offset{0.0f}, screen_arc_deg{100.0f}, display_fit_fill{0.85f};
		Setting<std::string> curve_fallback{"flat"};
		std::optional<bool> game_hands_setting;

		bool has_profile = false;
		std::string profile_name;
		bool profile_user = false;
		bool p_stereo = false;
		float p_separation = 0.0f, p_convergence = 0.0f;
		bool p_camera = false;
		bool p_fov = false;
		bool p_projection = false;
		std::optional<float> p_screen_distance, p_screen_height, p_screen_arc_deg;
		std::optional<bool> p_follow_head;
		std::optional<std::string> p_curve_fallback;
		std::string p_hands;
		bool p_game_hands_recipe = false;
		bool p_hands_game = false;
		u32 p_controls = 0;
		bool p_split = false;
	};

	struct Row
	{
		std::string label;
		std::string value;
		Source source = Source::Global;
		std::string text;
	};

	const char* SourceText(Source s);
	std::vector<Row> Describe(const Facts& f);
}
