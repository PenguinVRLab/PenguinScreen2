// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/FovCore.h"

#include "VR/FovMath.h"
#include "VR/GuestRam.h"

#include "MemoryTypes.h"

#include <algorithm>
#include <cmath>

namespace VR
{
	namespace
	{
		bool Finite(float v) { return std::isfinite(v); }
	}

	void FovCore::Forget()
	{
		m_holds.clear();
		m_orphans.clear();
		m_published = -1;
		m_pending = -1;
		m_pending_count = 0;
		m_effective_deg = 0.0f;
		m_ramp_left = 0;
		m_ramp_total = 0;
	}

	std::optional<u32> FovCore::ResolveLever(const ProfileDB::FovLever& lever, MemoryInterface& mem) const
	{
		if (!lever.base.has_value())
			return lever.address;

		const ProfileDB::CameraBase& base = lever.base.value();
		const std::optional<u32> obj = GuestRam::WalkChain(mem, base.pointer_addr, base.deref_offsets);
		if (!obj.has_value())
			return std::nullopt;

		if (base.has_validate)
		{
			const s64 va = static_cast<s64>(obj.value()) + base.validate_offset;
			if (va < 0 || va + 4 > static_cast<s64>(Ps2MemSize::MainRam))
				return std::nullopt;
			if (mem.Read32(static_cast<u32>(va)) != base.validate_equals)
				return std::nullopt;
		}

		const s64 addr = static_cast<s64>(obj.value()) + static_cast<s64>(lever.address) + base.base_offset;
		if (addr < 0 || addr + 4 > static_cast<s64>(Ps2MemSize::MainRam))
			return std::nullopt;
		return static_cast<u32>(addr);
	}

	void FovCore::RestoreHold(Hold& h, MemoryInterface& mem, Status& st)
	{
		const std::optional<float> guest = GuestRam::DecodeGuestValue(mem, h.address, h.encoding);
		if (guest.has_value() && guest.value() == h.last_written)
		{
			GuestRam::EncodeAndWrite(mem, h.address, h.game_value, h.encoding);
			st.restored = true;
		}
	}

	void FovCore::RestoreLever(size_t index, MemoryInterface& mem, bool vm_running, Status& st)
	{
		if (index >= m_holds.size() || !m_holds[index].has)
			return;

		Hold& h = m_holds[index];
		if (vm_running)
			RestoreHold(h, mem, st);
		else
			m_orphans.push_back(h);
		h.has = false;
	}

	void FovCore::DrainOrphans(MemoryInterface& mem, Status& st)
	{
		for (Hold& h : m_orphans)
			RestoreHold(h, mem, st);
		m_orphans.clear();
	}

	void FovCore::RestoreAll(MemoryInterface& mem, bool vm_running, Status& st)
	{
		for (size_t i = 0; i < m_holds.size(); i++)
			RestoreLever(i, mem, vm_running, st);
#ifndef VR_FOV_PLANTED_PUBLISH_NOT_CLEARED
		m_effective_deg = 0.0f;
#endif
		m_ramp_left = 0;
	}

	FovCore::Status FovCore::Step(const ProfileDB::FovProfile& fov, const Inputs& in, MemoryInterface& mem)
	{
		Status st;

		if (!in.vm_live)
		{
			Forget();
			return st;
		}

		if (in.vm_running)
			DrainOrphans(mem, st);

		if (in.profile_changed)
		{
			RestoreAll(mem, in.vm_running, st);
			m_published = -1;
			m_pending = -1;
			m_pending_count = 0;
			m_holds.clear();
		}

		if (!in.vr_on)
		{
			RestoreAll(mem, in.vm_running, st);
			m_published = -1;
			m_pending = -1;
			m_pending_count = 0;
			return st;
		}

		if (m_holds.size() != fov.levers.size())
			m_holds.assign(fov.levers.size(), Hold{});

		if (!in.vm_running)
		{
			st.scene = m_published;
			st.effective_deg = m_effective_deg;
			if (m_published >= 0 && static_cast<size_t>(m_published) < fov.scenes.size())
			{
				st.label = fov.scenes[m_published].label;
				st.lever = static_cast<int>(fov.scenes[m_published].lever);
			}
			return st;
		}

		int match = -1;
		for (size_t i = 0; i < fov.scenes.size(); i++)
		{
			if (GuestRam::GuardListPass(mem, fov.scenes[i].when))
			{
				match = static_cast<int>(i);
#ifndef VR_FOV_PLANTED_LAST_MATCH_WINS
				break;
#endif
			}
		}

		if (match == m_published)
		{
			m_pending = match;
			m_pending_count = 0;
		}
		else
		{
			if (match != m_pending)
			{
				m_pending = match;
				m_pending_count = 0;
			}
#ifdef VR_FOV_PLANTED_NO_DEBOUNCE
				if (++m_pending_count >= 1)
#else
			if (++m_pending_count >= kDebounceVsyncs)
#endif
			{
				if (m_published >= 0 && static_cast<size_t>(m_published) < fov.scenes.size())
				{
					const size_t old_lever = fov.scenes[m_published].lever;
					const bool same_lever = (match >= 0 && static_cast<size_t>(match) < fov.scenes.size() &&
											 fov.scenes[match].lever == old_lever);
#ifdef VR_FOV_PLANTED_KEEP_OLD_LEVER
						if (false)
#else
					if (!same_lever)
#endif
						RestoreLever(old_lever, mem, in.vm_running, st);
				}
				m_published = match;
				m_pending_count = 0;
				st.scene_changed = true;
				m_ramp_left = 0;
			}
		}

		st.scene = m_published;

		if (m_published < 0 || static_cast<size_t>(m_published) >= fov.scenes.size())
		{
			RestoreAll(mem, in.vm_running, st);
			st.effective_deg = 0.0f;
			return st;
		}

		const ProfileDB::FovScene& scene = fov.scenes[m_published];
		const size_t lever_index = scene.lever;
		if (lever_index >= fov.levers.size())
			return st;
		const ProfileDB::FovLever& lever = fov.levers[lever_index];
		st.label = scene.label;
		st.lever = static_cast<int>(lever_index);

#ifdef VR_FOV_PLANTED_GAME_TARGET_WRITES
		if (false)
#else
		if (scene.target.kind == ProfileDB::FovTargetKind::Game)
#endif
		{
			RestoreLever(lever_index, mem, in.vm_running, st);
#ifndef VR_FOV_PLANTED_PUBLISH_NOT_CLEARED
			m_effective_deg = 0.0f;
#endif
			m_ramp_left = 0;
			return st;
		}

		const std::optional<u32> address = ResolveLever(lever, mem);
		if (!address.has_value())
		{
			st.base_unresolved = true;
			return st;
		}

		Hold& hold = m_holds[lever_index];

		if (lever.has_readback)
		{
			const std::optional<float> rb =
				GuestRam::DecodeGuestValue(mem, lever.readback_address, ProfileDB::CameraEncoding::F32);
			if (rb.has_value())
			{
				st.has_readback = true;
				st.readback = rb.value();
			}
		}

		const std::optional<float> guest = GuestRam::DecodeGuestValue(mem, address.value(), lever.encoding);
		if (!guest.has_value())
			return st;
		if (!hold.has)
		{
			hold.has = true;
			hold.address = address.value();
			hold.encoding = lever.encoding;
			hold.game_value = guest.value();
			hold.last_written = guest.value();
		}
		else if (hold.address != address.value())
		{
			hold.address = address.value();
			hold.game_value = guest.value();
			hold.last_written = guest.value();
		}
		else if (guest.value() != hold.last_written)
		{
#ifndef VR_FOV_PLANTED_WIDEN_STALE_BASE
			hold.game_value = guest.value();
#endif
			st.game_wrote = true;
		}

		float want_constant = 0.0f;
		float want_deg = 0.0f;
#ifdef VR_FOV_PLANTED_VALUE_AS_DEGREES
		constexpr bool kRawValueAllowed = false;
#else
		constexpr bool kRawValueAllowed = true;
#endif
		const bool by_degrees = !(scene.target.kind == ProfileDB::FovTargetKind::Value && kRawValueAllowed);

		if (scene.target.kind == ProfileDB::FovTargetKind::Value && kRawValueAllowed)
		{
			want_constant = scene.target.value;
		}
		else
		{
			const ProfileDB::FovCalibration& cal = lever.calibration.value();
			switch (scene.target.kind)
			{
				case ProfileDB::FovTargetKind::Degrees:
					want_deg = scene.target.value;
					break;
				case ProfileDB::FovTargetKind::Screen:
					if (!in.screen_hfov_deg.has_value())
					{
						st.screen_unknown = true;
						RestoreLever(lever_index, mem, in.vm_running, st);
						m_effective_deg = 0.0f;
						return st;
					}
					want_deg = in.screen_hfov_deg.value();
					break;
				case ProfileDB::FovTargetKind::Widen:
				{
					const float game_deg = FovMath::DegFromConstant(cal, hold.game_value);
					want_deg = FovMath::WidenDeg(game_deg, scene.target.value);
					break;
				}
				default:
					break;
			}

			if (in.override_deg.has_value())
				want_deg = in.override_deg.value();

			if (!Finite(want_deg))
				return st;
			want_deg = std::clamp(want_deg, FovMath::kMinFovDeg, FovMath::kMaxFovDeg);
		}

		float apply_deg = want_deg;
		if (by_degrees)
		{
#ifdef VR_FOV_PLANTED_RAMP_FIXED_60HZ
			const float ramp_hz = 60.0f;
#else
			const float ramp_hz = in.vsync_hz;
#endif
			u32 ramp_vsyncs = (scene.ramp_seconds > 0.0f && ramp_hz > 0.0f)
								? static_cast<u32>(std::lround(scene.ramp_seconds * ramp_hz))
								: 0u;
#ifdef VR_FOV_PLANTED_RAMP_ALWAYS
			if (ramp_vsyncs == 0)
				ramp_vsyncs = 10;
#endif
			const bool have_current = (m_effective_deg > 0.0f);
			if (ramp_vsyncs > 0 && have_current && std::fabs(m_effective_deg - want_deg) > 0.01f &&
				(m_ramp_left == 0 || std::fabs(m_ramp_to_deg - want_deg) > 0.01f))
			{
				m_ramp_from_deg = m_effective_deg;
				m_ramp_to_deg = want_deg;
				m_ramp_total = ramp_vsyncs;
				m_ramp_left = ramp_vsyncs;
			}

			if (m_ramp_left > 0)
			{
				m_ramp_left--;
				const float t = (m_ramp_total == 0) ? 1.0f
												    : 1.0f - (static_cast<float>(m_ramp_left) / static_cast<float>(m_ramp_total));
#ifdef VR_FOV_PLANTED_RAMP_LINEAR_DEG
				apply_deg = m_ramp_from_deg + (m_ramp_to_deg - m_ramp_from_deg) * t;
#else
				apply_deg = FovMath::LerpDeg(m_ramp_from_deg, m_ramp_to_deg, t);
#endif
				st.ramping = (m_ramp_left > 0);
				if (!Finite(apply_deg))
					apply_deg = want_deg;
			}

			want_constant = FovMath::ConstantFromDeg(lever.calibration.value(), apply_deg);
			if (!Finite(want_constant))
				return st;
		}

		GuestRam::EncodeAndWrite(mem, address.value(), want_constant, lever.encoding);
		const std::optional<float> written = GuestRam::DecodeGuestValue(mem, address.value(), lever.encoding);
		hold.last_written = written.value_or(want_constant);

		m_effective_deg = by_degrees ? apply_deg : 0.0f;
		st.wrote = true;
		st.constant = want_constant;
#ifdef VR_FOV_PLANTED_PUBLISH_TARGET
		st.effective_deg = by_degrees ? want_deg : 0.0f;
#else
		st.effective_deg = m_effective_deg;
#endif
		return st;
	}
}
