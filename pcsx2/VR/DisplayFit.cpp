// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/DisplayFit.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace VR::DisplayFit
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDegToRad = kPi / 180.0f;
		constexpr float kRadToDeg = 180.0f / kPi;
		constexpr float kPitchLimit = 89.0f * kDegToRad;

		std::atomic<float> s_yaw_scale{1.0f};
		std::atomic<float> s_pitch_scale{1.0f};
		std::atomic<bool> s_tight{false};

		bool Usable(const Screen& s)
		{
			if (!(s.aspect > 0.0f) || !std::isfinite(s.aspect))
				return false;
			if (s.curved)
				return s.arc_deg > 0.0f && std::isfinite(s.arc_deg);
			return s.height_m > 0.0f && s.distance_m > 0.0f && std::isfinite(s.height_m) && std::isfinite(s.distance_m);
		}
	}

	float VerticalDeg(const Screen& s)
	{
		if (!Usable(s))
			return 0.0f;
		if (s.curved)
			return 2.0f * std::atan((s.arc_deg * kDegToRad) / (2.0f * s.aspect)) * kRadToDeg;
		return 2.0f * std::atan(s.height_m / (2.0f * s.distance_m)) * kRadToDeg;
	}

	float HorizontalDeg(const Screen& s)
	{
		if (!Usable(s))
			return 0.0f;
		if (s.curved)
			return s.arc_deg;
		return 2.0f * std::atan((s.height_m * s.aspect) / (2.0f * s.distance_m)) * kRadToDeg;
	}

	Fit Compute(float display_vfov_deg, const Screen& reference, float fill)
	{
		Fit f;
#ifdef VR_DISPLAYFIT_PLANTED_NOCUT
		constexpr float cut = 1000.0f;
#else
		constexpr float cut = kTightVerticalFovDeg;
#endif
		if (!(fill > 0.0f) || !(display_vfov_deg > 0.0f) || !std::isfinite(display_vfov_deg) || display_vfov_deg >= cut)
			return f;
		f.tight = true;
		const float v_ref = VerticalDeg(reference);
		const float h_ref = HorizontalDeg(reference);
		const float target = std::min(fill, 1.0f) * display_vfov_deg;
		if (!(v_ref > target) || !(h_ref > 0.0f))
			return f;
		f.pitch_scale = target / v_ref;
#ifdef VR_DISPLAYFIT_PLANTED_FAULT
		f.pitch_scale = 1.0f / f.pitch_scale;
#endif
		f.yaw_scale = HorizontalDeg(Apply(reference, f)) / h_ref;
		return f;
	}

	Screen Apply(const Screen& reference, const Fit& fit)
	{
		if (fit.IsIdentity() || !Usable(reference))
			return reference;
		Screen out = reference;
		const float half = 0.5f * fit.pitch_scale * VerticalDeg(reference) * kDegToRad;
		if (reference.curved)
			out.arc_deg = 2.0f * reference.aspect * std::tan(half) * kRadToDeg;
		else
			out.height_m = 2.0f * reference.distance_m * std::tan(half);
		return out;
	}

	void HeadToReference(float& yaw_rad, float& pitch_rad, const Fit& fit)
	{
		if (fit.IsIdentity() || !(fit.yaw_scale > 0.0f) || !(fit.pitch_scale > 0.0f))
			return;
		yaw_rad = std::clamp(yaw_rad / fit.yaw_scale, -kPi, kPi);
		pitch_rad = std::clamp(pitch_rad / fit.pitch_scale, -kPitchLimit, kPitchLimit);
	}

	void HeadForOp(float& yaw_rad, float& pitch_rad, const Fit& fit, bool fitted)
	{
#ifdef VR_DISPLAYFIT_PLANTED_ANGLESFLAG
		fitted = true;
#endif
		if (fitted)
			HeadToReference(yaw_rad, pitch_rad, fit);
	}

	float RecenterPitch(float forward_y, const Fit& fit)
	{
#ifdef VR_DISPLAYFIT_PLANTED_NOPITCH
		(void)forward_y;
		(void)fit;
		return 0.0f;
#else
		if (!fit.tight || !std::isfinite(forward_y))
			return 0.0f;
		const float limit = kMaxRecenterPitchDeg * kDegToRad;
		return std::clamp(std::asin(std::clamp(forward_y, -1.0f, 1.0f)), -limit, limit);
#endif
	}

	float ForwardY(float qx, float qy, float qz, float qw)
	{
#ifdef VR_DISPLAYFIT_PLANTED_OLDSIGN
		return -2.0f * (qy * qz + qw * qx);
#else
		return 2.0f * (qw * qx - qy * qz);
#endif
	}

	Placement PlaceAhead(float yaw_rad, float pitch_rad, float distance_m)
	{
		const float sy = std::sin(yaw_rad * 0.5f), cy = std::cos(yaw_rad * 0.5f);
		const float sp = std::sin(pitch_rad * 0.5f), cp = std::cos(pitch_rad * 0.5f);
		Placement p;
		p.qx = cy * sp;
		p.qy = sy * cp;
		p.qz = -sy * sp;
		p.qw = cy * cp;
		const float cpitch = std::cos(pitch_rad);
		p.dx = -distance_m * std::sin(yaw_rad) * cpitch;
		p.dy = distance_m * std::sin(pitch_rad);
		p.dz = -distance_m * std::cos(yaw_rad) * cpitch;
		return p;
	}

	void Publish(const Fit& fit)
	{
		s_yaw_scale.store(fit.yaw_scale, std::memory_order_relaxed);
		s_pitch_scale.store(fit.pitch_scale, std::memory_order_relaxed);
		s_tight.store(fit.tight, std::memory_order_relaxed);
	}

	Fit Get()
	{
		Fit f;
		f.tight = s_tight.load(std::memory_order_relaxed);
		f.yaw_scale = s_yaw_scale.load(std::memory_order_relaxed);
		f.pitch_scale = s_pitch_scale.load(std::memory_order_relaxed);
		return f;
	}

	void Reset()
	{
		Publish(Fit{});
	}
}
