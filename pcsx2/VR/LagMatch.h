// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/VRProfileDB.h"

#include "common/Pcsx2Types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace VR::LagMatch
{
	struct Quat
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
	};

	struct Euler
	{
		float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
	};

	enum Axis : u8
	{
		AxisYaw = 1,
		AxisPitch = 2,
		AxisRoll = 4,
	};

	inline Quat Mul(const Quat& a, const Quat& b)
	{
		return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
	}

	inline Quat Conj(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

	inline Quat Normalize(const Quat& q)
	{
		const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		if (!(n > 0.0f) || !std::isfinite(n))
			return Quat{};
		return Quat{q.x / n, q.y / n, q.z / n, q.w / n};
	}

	inline Quat Nlerp(const Quat& a, Quat b, float t)
	{
		if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f)
			b = Quat{-b.x, -b.y, -b.z, -b.w};
		return Normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
	}

	inline void Rotate(const Quat& q, float& vx, float& vy, float& vz)
	{
		const Quat r = Mul(Mul(q, Quat{vx, vy, vz, 0.0f}), Conj(q));
		vx = r.x;
		vy = r.y;
		vz = r.z;
	}

	inline Quat FromEulerYXZ(const Euler& e)
	{
		const Quat qy{0.0f, std::sin(e.yaw * 0.5f), 0.0f, std::cos(e.yaw * 0.5f)};
		const Quat qx{std::sin(e.pitch * 0.5f), 0.0f, 0.0f, std::cos(e.pitch * 0.5f)};
		const Quat qz{0.0f, 0.0f, std::sin(e.roll * 0.5f), std::cos(e.roll * 0.5f)};
		return Mul(Mul(qy, qx), qz);
	}

	inline Euler ToEulerYXZ(const Quat& q)
	{
		Euler e;
		e.pitch = std::asin(std::clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f));
		e.yaw = std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
		e.roll = std::atan2(2.0f * (q.x * q.y + q.w * q.z), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
		return e;
	}

	inline Quat LayerOrientation(const Quat& now, const Quat& lagged, u8 driven_axes)
	{
#ifdef VR_LAGMATCH_PLANTED_ALL_AXES
		driven_axes = AxisYaw | AxisPitch | AxisRoll;
#endif
		if ((driven_axes & (AxisYaw | AxisPitch | AxisRoll)) == 0)
			return Quat{};
		const Euler en = ToEulerYXZ(now);
		const Euler el = ToEulerYXZ(lagged);
		Euler mix = en;
		if (driven_axes & AxisYaw)
			mix.yaw = el.yaw;
		if (driven_axes & AxisPitch)
			mix.pitch = el.pitch;
		if (driven_axes & AxisRoll)
			mix.roll = el.roll;
		return Normalize(Mul(Conj(now), FromEulerYXZ(mix)));
	}

	class PoseHistory
	{
	public:
		static constexpr size_t kCapacity = 128;

		void Clear() { m_count = 0; }
		size_t Size() const { return m_count; }

		void Record(s64 t, const Quat& q)
		{
			if (m_count > 0 && t <= At_(m_count - 1).t)
				return;
			m_items[(m_head + m_count) % kCapacity] = Item{t, q};
			if (m_count < kCapacity)
				m_count++;
			else
				m_head = (m_head + 1) % kCapacity;
		}

		std::optional<Quat> At(s64 t) const
		{
			if (m_count == 0)
				return std::nullopt;
			if (t <= At_(0).t)
				return At_(0).q;
			if (t >= At_(m_count - 1).t)
				return At_(m_count - 1).q;
			size_t hi = 1;
			while (hi < m_count && At_(hi).t < t)
				hi++;
			const Item& a = At_(hi - 1);
			const Item& b = At_(hi);
#ifdef VR_LAGMATCH_PLANTED_NEAREST
			return (t - a.t < b.t - t) ? a.q : b.q;
#endif
			const float f = static_cast<float>(static_cast<double>(t - a.t) / static_cast<double>(b.t - a.t));
			return Nlerp(a.q, b.q, f);
		}

	private:
		struct Item
		{
			s64 t = 0;
			Quat q;
		};
		const Item& At_(size_t i) const { return m_items[(m_head + i) % kCapacity]; }

		std::array<Item, kCapacity> m_items{};
		size_t m_head = 0;
		size_t m_count = 0;
	};

	class Follower
	{
	public:
		static constexpr u32 kIdleVsyncs = 8;

		void Configure(s64 lag_ns, u8 axes)
		{
			if (lag_ns == m_lag_ns && axes == m_axes)
				return;
			m_lag_ns = lag_ns;
			m_axes = axes;
			m_held.reset();
			m_pending = true;
		}

		bool On() const { return m_lag_ns > 0 && (m_axes & (AxisYaw | AxisPitch | AxisRoll)) != 0; }
		s64 LagNs() const { return m_lag_ns; }
		u8 Axes() const { return m_axes; }

		void OnHead(s64 display_time, const Quat& q)
		{
			m_history.Record(display_time, q);
			m_now = q;
		}

		void OnHeadLost()
		{
			m_now.reset();
			m_held.reset();
			m_history.Clear();
			m_pending = true;
		}

		void OnGameVsync(bool new_frame)
		{
			if (new_frame)
			{
				m_pending = true;
				m_idle = 0;
			}
			else if (++m_idle >= kIdleVsyncs)
			{
				m_pending = true;
			}
		}

		std::optional<Quat> LayerOrientation(s64 display_time)
		{
			if (!On() || !m_now.has_value())
				return std::nullopt;
#ifdef VR_LAGMATCH_PLANTED_NO_HOLD
			m_pending = true;
#endif
			if (m_pending || !m_held.has_value())
			{
				const std::optional<Quat> q = m_history.At(display_time - m_lag_ns);
				if (q.has_value())
				{
					m_held = q;
					m_pending = false;
				}
			}
			if (!m_held.has_value())
				return std::nullopt;
			return LagMatch::LayerOrientation(m_now.value(), m_held.value(), m_axes);
		}

	private:
		s64 m_lag_ns = 0;
		u8 m_axes = 0;
		PoseHistory m_history;
		std::optional<Quat> m_now;
		std::optional<Quat> m_held;
		bool m_pending = true;
		u32 m_idle = 0;
	};

	inline u8 DrivenAxes(const ProfileDB::CameraProfile& cam)
	{
		u8 axes = 0;
		const auto from_source = [&axes](ProfileDB::CameraSource s) {
			if (s == ProfileDB::CameraSource::HeadYaw)
				axes |= AxisYaw;
			else if (s == ProfileDB::CameraSource::HeadPitch)
				axes |= AxisPitch;
			else if (s == ProfileDB::CameraSource::HeadRoll)
				axes |= AxisRoll;
		};
		for (const ProfileDB::CameraWriteOp& w : cam.writes)
			from_source(w.source);
		for (const ProfileDB::CameraMatrixOp& m : cam.matrix_writes)
		{
			if (m.axis_sign_yaw != 0.0f)
				axes |= AxisYaw;
			if (m.axis_sign_pitch != 0.0f)
				axes |= AxisPitch;
			if (m.axis_sign_roll != 0.0f)
				axes |= AxisRoll;
		}
		for (const ProfileDB::CameraCodeHook& h : cam.code_hooks)
		{
			if (h.enabled)
				from_source(h.source);
		}
		if (cam.pad_look.has_value())
			axes |= AxisYaw;
		if (cam.direct_look.has_value())
		{
			const ProfileDB::CameraDirectLook& d = cam.direct_look.value();
			if (d.yaw.has_value())
				axes |= AxisYaw;
			if (d.pitch.has_value())
				axes |= AxisPitch;
			if (d.aim.has_value())
				axes |= AxisYaw | AxisPitch;
		}
		return axes;
	}

	struct Lag
	{
		s64 ns = 0;
		bool from_frame_age = false;
		u32 frame_age = 0;
		float ms = 0.0f;
	};

	inline Lag ResolveLag(const ProfileDB::Profile& p, double vsync_hz)
	{
		Lag lag;
		if (!p.screen_lag_ms.has_value() || !p.screen_follow_head.value_or(false))
			return lag;
		if (p.projection.has_value() && p.projection->frame_age > 0 && vsync_hz > 1.0)
		{
			lag.from_frame_age = true;
			lag.frame_age = p.projection->frame_age;
			lag.ms = static_cast<float>(1000.0 * lag.frame_age / vsync_hz);
		}
		else
		{
			lag.ms = p.screen_lag_ms.value();
		}
		lag.ns = static_cast<s64>(static_cast<double>(lag.ms) * 1.0e6);
		return lag;
	}
}
