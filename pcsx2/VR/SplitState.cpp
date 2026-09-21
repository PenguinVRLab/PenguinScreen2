// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/SplitState.h"
#include "VR/VRManager.h"
#include "VR/VRProfileDB.h"

#include "Host.h"
#include "Memory.h"
#include "VMManager.h"
#include "Config.h"

#include "common/Console.h"

#include <atomic>
#include <mutex>

namespace VR::SplitState
{
	namespace
	{
		std::mutex s_mutex;
		Snapshot s_published;
		std::atomic<bool> s_active_cheap{false};
		std::atomic<float> s_sep_scale{1.0f};

		bool s_memo_valid = false;
		int s_published_match = -1;
		int s_pending = -1;
		u32 s_pending_count = 0;

		constexpr u32 SPLIT_DEBOUNCE_VSYNCS = 30;

		void Publish(bool active, const ProfileDB::SplitParams* sp, bool announce = true)
		{
			Snapshot snap;
			snap.split_active = active;
			if (active && sp)
			{
				for (int i = 0; i < 2; i++)
				{
					snap.rect_x[i] = sp->rects[i].x;
					snap.rect_y[i] = sp->rects[i].y;
					snap.rect_w[i] = sp->rects[i].w;
					snap.rect_h[i] = sp->rects[i].h;
				}
				snap.local_view = sp->local_view;
				snap.local_pad_port = sp->local_pad_port;
				snap.mode = (sp->mode == ProfileDB::SplitParams::Mode::Duo) ? 1 :
				            (sp->mode == ProfileDB::SplitParams::Mode::Local) ? 2 : 0;
				snap.side_scale = sp->side_scale;
				snap.side_angle_deg = sp->side_angle_deg;
				snap.stereo_on = sp->stereo_on;
			}
			{
				std::lock_guard<std::mutex> lock(s_mutex);
				s_published = snap;
			}
			const bool was_active = s_active_cheap.exchange(active, std::memory_order_acq_rel);
			float scale = 1.0f;
			if (active && sp)
				scale = sp->rects[sp->local_view].w;
			s_sep_scale.store(scale > 0.0f ? scale : 1.0f, std::memory_order_release);
			if (announce)
			{
				if (active || was_active)
					Host::AddKeyedOSDMessage("VRSplitState",
						active ? "VR split-screen: SPLIT (two screens)" :
						         "VR split-screen: back to one screen",
						Host::OSD_INFO_DURATION);
				Console.WriteLn("(VR) SplitState: %s", active ? "SPLIT" : "MIRROR");
			}
		}
	}

	void Apply()
	{
		const Pcsx2Config::VROptions& cfg = EmuConfig.VR;
		if (!EffectiveVREnabled(cfg.Enable))
		{
			Publish(false, nullptr, s_memo_valid && s_published_match != -1);
			s_memo_valid = false;
			s_published_match = -1;
			s_pending = -1;
			s_pending_count = 0;
			return;
		}

		ProfileDB::EnsureLoaded();
		const std::string serial = VMManager::GetDiscSerial();
		const ProfileDB::Profile* profile =
			serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		const ProfileDB::SplitParams* sp =
			(profile && profile->split.has_value()) ? &profile->split.value() : nullptr;

		if (!sp || sp->mode == ProfileDB::SplitParams::Mode::Mirror || !sp->active.has_value())
		{
			if (s_memo_valid && s_published_match != -1)
				Publish(false, nullptr);
			else if (!s_memo_valid)
				Publish(false, nullptr, sp != nullptr);
			s_memo_valid = true;
			s_published_match = -1;
			s_pending = -1;
			s_pending_count = 0;
			return;
		}

		const ProfileDB::SplitParams::Probe& probe = sp->active.value();
		u32 value = 0;
		switch (probe.width)
		{
			case 1: value = memRead8(probe.ee_address); break;
			case 2: value = memRead16(probe.ee_address); break;
			default: value = memRead32(probe.ee_address); break;
		}
		const bool hit = probe.at_least ? (value >= probe.threshold) : (value == probe.threshold);
		const int match = hit ? 1 : -1;

		if (s_memo_valid)
		{
			if (match == s_published_match)
			{
				s_pending = match;
				s_pending_count = 0;
				return;
			}
			if (match != s_pending)
			{
				s_pending = match;
				s_pending_count = 1;
				return;
			}
			if (++s_pending_count < SPLIT_DEBOUNCE_VSYNCS)
				return;
		}

		s_memo_valid = true;
		s_published_match = match;
		s_pending = match;
		s_pending_count = 0;
		Publish(match == 1, sp);
	}

	void InvalidateMemo()
	{
		s_memo_valid = false;
		s_pending = -1;
		s_pending_count = 0;
	}

	bool Active()
	{
		return s_active_cheap.load(std::memory_order_acquire);
	}

	float SepScale()
	{
		return s_sep_scale.load(std::memory_order_acquire);
	}

	Snapshot Get()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		return s_published;
	}
}
