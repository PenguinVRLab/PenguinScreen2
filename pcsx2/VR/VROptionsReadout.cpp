// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VR/VROptionsReadout.h"

#include "fmt/format.h"

namespace VR::OptionsReadout
{
	const char* SourceText(Source s)
	{
		switch (s)
		{
			case Source::Global:
				return "your VR settings";
			case Source::GameSettings:
				return "this game's settings";
			case Source::Profile:
				return "this game's profile";
			case Source::Default:
				return "default";
			case Source::NotAvailable:
			default:
				return "";
		}
	}

	namespace
	{
		template <typename T>
		Source From(const Setting<T>& s)
		{
			return s.from_game ? Source::GameSettings : Source::Global;
		}

		struct Rows
		{
			const Facts& f;
			std::vector<Row> out;

			void add(const char* label, std::string value, Source source)
			{
				Row r;
				r.label = label;
				r.source = source;
				r.value = std::move(value);
				const std::string body = fmt::format("{} ({})", r.value, SourceText(source));
				r.text = f.vr_on ? fmt::format("{}: {}", label, body) : fmt::format("{}: VR is off; in VR: {}", label, body);
				out.push_back(std::move(r));
			}
			void na(const char* label, const char* why)
			{
				Row r;
				r.label = label;
				r.source = Source::NotAvailable;
				r.value = fmt::format("not available for this game ({})", why);
				r.text = f.vr_on ? fmt::format("{}: {}", label, r.value) : fmt::format("{}: VR is off; in VR: {}", label, r.value);
				out.push_back(std::move(r));
			}
			void gated(const char* label, bool available, const char* why, const Setting<bool>& sw)
			{
				if (!available)
					na(label, why);
				else if (!sw.value)
					add(label, "off", From(sw));
				else
					add(label, "on", Source::Profile);
			}
			void metres(const char* label, const std::optional<float>& profile, const Setting<float>& s, int precision, const char* unit)
			{
				if (profile.has_value())
					add(label, fmt::format("{:.{}f} {}", profile.value(), precision, unit), Source::Profile);
				else
					add(label, fmt::format("{:.{}f} {}", s.value, precision, unit), From(s));
			}
		};
	}

	std::vector<Row> Describe(const Facts& f)
	{
		Rows r{f, {}};

		if (f.has_profile)
			r.add("VR profile", fmt::format("{}{}", f.profile_name.empty() ? std::string("unnamed") : f.profile_name,
									  f.profile_user ? ", your file" : ""),
				Source::Profile);
		else
			r.na("VR profile", "no VR profile");

		const bool stereo_available = !f.stereo_use_profile.value || (f.has_profile && f.p_stereo);
		if (!stereo_available)
			r.na("Stereo", "no stereo profile");
		else if (!f.stereo_mode.value)
			r.add("Stereo", "off", From(f.stereo_mode));
		else if (!f.stereo_use_profile.value)
			r.add("Stereo", "on, with your own depth", From(f.stereo_use_profile));
		else
			r.add("Stereo", "on", Source::Profile);
		if (stereo_available && f.stereo_mode.value)
		{
			if (f.stereo_use_profile.value)
				r.add("Stereo depth", fmt::format("separation {:.3f}, convergence {:.1f}", f.p_separation, f.p_convergence), Source::Profile);
			else
				r.add("Stereo depth", fmt::format("separation {:.3f}, convergence {:.1f}", f.stereo_separation.value, f.stereo_convergence.value),
					(f.stereo_separation.from_game || f.stereo_convergence.from_game) ? Source::GameSettings : Source::Global);
		}

		r.gated("Head camera", f.has_profile && f.p_camera, "no camera profile", f.head_camera);
		r.gated("Game field of view", f.has_profile && f.p_fov, "no field-of-view profile", f.game_fov);
		r.gated("World projection", f.has_profile && f.p_projection, "no world projection profile", f.world_projection);

		r.metres("Screen distance", f.has_profile ? f.p_screen_distance : std::nullopt, f.screen_distance, 1, "m");
		r.metres("Screen height", f.has_profile ? f.p_screen_height : std::nullopt, f.screen_height, 1, "m");
		r.metres("Screen curve", f.has_profile ? f.p_screen_arc_deg : std::nullopt, f.screen_arc_deg, 0, "deg");
		r.add("Screen vertical offset", fmt::format("{:.2f} m", f.screen_vertical_offset.value), From(f.screen_vertical_offset));
		if (f.has_profile && f.p_follow_head.has_value())
			r.add("Screen", f.p_follow_head.value() ? "follows your head" : "fixed in the room", Source::Profile);
		else
			r.add("Screen", "fixed in the room", Source::Default);
		if (f.has_profile && f.p_curve_fallback.has_value())
			r.add("Curve fallback", f.p_curve_fallback.value(), Source::Profile);
		else
			r.add("Curve fallback", f.curve_fallback.value, From(f.curve_fallback));

		if (f.has_profile && !f.p_hands.empty())
			r.add("Hands", f.p_hands, Source::Profile);
		else
			r.na("Hands", "no hands in its profile");
		if (!(f.has_profile && f.p_game_hands_recipe))
			r.na("Game hands", "no game-hands recipe in its profile");
		else if (f.game_hands_setting.has_value())
			r.add("Game hands", f.game_hands_setting.value() ? "on" : "off", Source::GameSettings);
		else
			r.add("Game hands", f.p_hands_game ? "on" : "off", Source::Profile);
		if (f.has_profile && f.p_controls > 0)
			r.add("VR controls", fmt::format("{} control{}", f.p_controls, f.p_controls == 1 ? "" : "s"), Source::Profile);
		else
			r.na("VR controls", "no VR controls in its profile");
		if (f.has_profile && f.p_split)
			r.add("Split screen", "when the game splits its screen", Source::Profile);
		else
			r.na("Split screen", "no split-screen profile");

		if (f.display_fit_fill.value <= 0.0f)
			r.add("Display fit", "off", From(f.display_fit_fill));
		else
			r.add("Display fit", fmt::format("{:.0f}% of a glasses display", f.display_fit_fill.value * 100.0f), From(f.display_fit_fill));
		r.add("Screen follows widescreen patches", f.follows_patch_aspect.value ? "on" : "off", From(f.follows_patch_aspect));
		return r.out;
	}
}
