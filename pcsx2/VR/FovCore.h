// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/VRProfileDB.h"

#include "common/MemoryInterface.h"
#include "common/Pcsx2Types.h"

#include <optional>
#include <string>
#include <vector>

namespace VR
{
	class FovCore
	{
	public:
		static constexpr u32 kDebounceVsyncs = 3;

		struct Inputs
		{
			bool vr_on = false;
			bool vm_live = false;
			bool vm_running = false;
			bool profile_changed = false;
			std::optional<float> screen_hfov_deg;
			std::optional<float> override_deg;
			float vsync_hz = 59.94f;
		};

		struct Status
		{
			int scene = -1;
			std::string label;
			int lever = -1;
			float effective_deg = 0.0f;
			float constant = 0.0f;
			bool wrote = false;
			bool restored = false;
			bool ramping = false;
			bool scene_changed = false;
			bool game_wrote = false;
			bool base_unresolved = false;
			bool screen_unknown = false;
			float readback = 0.0f;
			bool has_readback = false;
		};

		Status Step(const ProfileDB::FovProfile& fov, const Inputs& in, MemoryInterface& mem);

		void Forget();

		float EffectiveDeg() const { return m_effective_deg; }

	private:
		struct Hold
		{
			bool has = false;
			u32 address = 0;
			ProfileDB::CameraEncoding encoding = ProfileDB::CameraEncoding::F32;
			float game_value = 0.0f;
			float last_written = 0.0f;
		};

		std::optional<u32> ResolveLever(const ProfileDB::FovLever& lever, MemoryInterface& mem) const;
		void RestoreLever(size_t index, MemoryInterface& mem, bool vm_running, Status& st);
		void RestoreAll(MemoryInterface& mem, bool vm_running, Status& st);
		void RestoreHold(Hold& h, MemoryInterface& mem, Status& st);
		void DrainOrphans(MemoryInterface& mem, Status& st);

		std::vector<Hold> m_holds;
		std::vector<Hold> m_orphans;
		int m_published = -1;
		int m_pending = -1;
		u32 m_pending_count = 0;
		float m_effective_deg = 0.0f;
		float m_ramp_from_deg = 0.0f;
		float m_ramp_to_deg = 0.0f;
		u32 m_ramp_left = 0;
		u32 m_ramp_total = 0;
	};
}
