// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/FovDriver.h"

#include "VR/FovCore.h"
#include "VR/VRManager.h"
#include "VR/VRProfileDB.h"
#include "VR/XRCompositor.h"

#include "Config.h"
#include "GS.h"
#include "Memory.h"
#include "VMManager.h"

#include "common/Console.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <string>

namespace VR::FovDriver
{
	namespace
	{
		FovCore s_core;

		std::atomic<float> s_override_deg{std::numeric_limits<float>::quiet_NaN()};
		std::atomic<float> s_effective_deg{0.0f};
		std::atomic<float> s_held_constant{std::numeric_limits<float>::quiet_NaN()};

		std::string s_logged_label;
		int s_logged_scene = -2;
		bool s_logged_unresolved = false;
		bool s_logged_screen_unknown = false;
		bool s_logged_game_wrote = false;
		const ProfileDB::FovProfile* s_last_profile = nullptr;
		u32 s_last_crc = 0;

		void ResetLogState()
		{
			s_logged_label.clear();
			s_logged_scene = -2;
			s_logged_unresolved = false;
			s_logged_screen_unknown = false;
			s_logged_game_wrote = false;
		}

		float VsyncHz()
		{
			const float hz = static_cast<float>(GetVerticalFrequency());
			return (std::isfinite(hz) && hz > 1.0f) ? hz : 59.94f;
		}
	}

	void Apply()
	{
		FovCore::Inputs in;

		const VMState vm_state = VMManager::GetState();
		in.vm_live = (vm_state == VMState::Running || vm_state == VMState::Paused);
		in.vm_running = (vm_state == VMState::Running);
		if (!in.vm_live)
		{
			s_core.Forget();
			s_effective_deg.store(0.0f, std::memory_order_relaxed);
			s_held_constant.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
			s_last_profile = nullptr;
			s_last_crc = 0;
			ResetLogState();
			return;
		}

		in.vr_on = EffectiveVREnabled(EmuConfig.VR.Enable) && EmuConfig.VR.GameFov;
		in.vsync_hz = VsyncHz();

		const float ov = s_override_deg.load(std::memory_order_relaxed);
		if (std::isfinite(ov))
			in.override_deg = ov;

		ProfileDB::EnsureLoaded();
		const u32 crc = VMManager::GetDiscCRC();
		const ProfileDB::Profile* profile = ProfileDB::Lookup(VMManager::GetDiscSerial(), crc);
		const ProfileDB::FovProfile* fov =
			(profile && profile->fov.has_value()) ? &profile->fov.value() : nullptr;

		in.profile_changed = (fov != s_last_profile) || (crc != s_last_crc);

		EEMemoryInterface ee;

		if (!fov)
		{
			if (s_last_profile != nullptr)
			{
				static const ProfileDB::FovProfile s_empty;
				in.profile_changed = true;
				const FovCore::Status st = s_core.Step(s_empty, in, ee);
				if (st.restored)
					Console.WriteLn("(VR) FOV: the profile went away; the game has its own field of view back.");
			}
			s_last_profile = nullptr;
			s_last_crc = crc;
			s_effective_deg.store(0.0f, std::memory_order_relaxed);
			s_held_constant.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
			ResetLogState();
			return;
		}

		if (in.profile_changed)
			ResetLogState();
		s_last_profile = fov;
		s_last_crc = crc;

		in.screen_hfov_deg = XRCompositor::PresentedScreenHfovDeg();

		const FovCore::Status st = s_core.Step(*fov, in, ee);

		s_effective_deg.store(st.effective_deg, std::memory_order_relaxed);
		s_held_constant.store(st.wrote ? st.constant : std::numeric_limits<float>::quiet_NaN(),
			std::memory_order_relaxed);

		if (st.scene != s_logged_scene || st.label != s_logged_label)
		{
			s_logged_scene = st.scene;
			s_logged_label = st.label;
			s_logged_unresolved = false;
			s_logged_screen_unknown = false;
			s_logged_game_wrote = false;
			if (st.scene >= 0 && st.wrote)
			{
				Console.WriteLnFmt("(VR) FOV: scene '{}' -> lever '{}' at {:.2f}deg (constant {:.6g}).",
					st.label, fov->levers[static_cast<size_t>(st.lever)].name, st.effective_deg, st.constant);
				if (st.has_readback)
					Console.WriteLnFmt("(VR) FOV: the engine's own read-back word reads {:.6g}.", st.readback);
			}
			else if (st.scene < 0)
			{
				Console.WriteLn("(VR) FOV: no scene matches; the game's own field of view stands.");
			}
		}
		else if (st.wrote && st.scene_changed)
		{
			Console.WriteLnFmt("(VR) FOV: scene '{}' at {:.2f}deg (constant {:.6g}).",
				st.label, st.effective_deg, st.constant);
		}

		if (st.restored)
			Console.WriteLn("(VR) FOV: handed a lever back to the game.");

		if (st.base_unresolved && !s_logged_unresolved)
		{
			s_logged_unresolved = true;
			Console.WarningFmt("(VR) FOV: scene '{}' cannot resolve its lever's pointer chain; sitting out. "
							   "(A validate word that stopped matching is the usual cause.)",
				st.label);
		}
		if (st.screen_unknown && !s_logged_screen_unknown)
		{
			s_logged_screen_unknown = true;
			Console.WarningFmt("(VR) FOV: scene '{}' wants to match the screen, but no screen has been presented "
							   "yet; leaving the game's own field of view alone until one is.",
				st.label);
		}
		if (st.game_wrote && !s_logged_game_wrote)
		{
			s_logged_game_wrote = true;
			DevCon.WriteLnFmt("(VR) FOV: the game wrote lever '{}' itself; re-baselined on its value.",
				(st.lever >= 0) ? fov->levers[static_cast<size_t>(st.lever)].name : std::string("?"));
		}
	}

	void OnStateLoaded()
	{
		s_core.Forget();
		s_effective_deg.store(0.0f, std::memory_order_relaxed);
		s_held_constant.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
		ResetLogState();
	}

	float EffectiveFovDeg()
	{
		return s_effective_deg.load(std::memory_order_relaxed);
	}

	void SetOverrideDeg(std::optional<float> deg)
	{
		s_override_deg.store(deg.value_or(std::numeric_limits<float>::quiet_NaN()), std::memory_order_relaxed);
		if (deg.has_value())
			Console.WriteLnFmt("(VR) FOV: live override set to {:.2f}deg.", deg.value());
		else
			Console.WriteLn("(VR) FOV: live override cleared; the scene's own target is back.");
	}

	std::optional<float> OverrideDeg()
	{
		const float v = s_override_deg.load(std::memory_order_relaxed);
		return std::isfinite(v) ? std::optional<float>(v) : std::nullopt;
	}

	std::optional<float> HeldConstant()
	{
		const float v = s_held_constant.load(std::memory_order_relaxed);
		return std::isfinite(v) ? std::optional<float>(v) : std::nullopt;
	}
}
