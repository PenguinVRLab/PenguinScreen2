// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/WristPinch.h"

#include <algorithm>
#include <cmath>

namespace VR::WristPinch
{
	namespace
	{
		using V3 = std::array<float, 3>;

		V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
		float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		V3 Cross(const V3& a, const V3& b)
		{
			return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
		}
		float Length(const V3& a) { return std::sqrt(Dot(a, a)); }

		V3 Rotate(const std::array<float, 4>& q, const V3& v)
		{
			const V3 u{q[0], q[1], q[2]};
			const V3 t = Cross(u, v);
			const V3 t2{2.f * t[0], 2.f * t[1], 2.f * t[2]};
			const V3 c = Cross(u, t2);
			return {v[0] + q[3] * t2[0] + c[0], v[1] + q[3] * t2[1] + c[1], v[2] + q[3] * t2[2] + c[2]};
		}

		float AngleDeg(const V3& a, const V3& b)
		{
			const float la = Length(a), lb = Length(b);
			if (!(la > 1e-6f) || !(lb > 1e-6f))
				return 180.f;
			const float c = std::clamp(Dot(a, b) / (la * lb), -1.f, 1.f);
			return std::acos(c) * (180.f / 3.14159265358979f);
		}
	}

	bool IsFacing(const HandJoints& hand, const VRPose& head, const Params& params)
	{
		if (!hand.wrist.valid || !head.valid)
			return false;
		const V3 palm_normal = Rotate(hand.wrist.orientation_xyzw, V3{0.f, -1.f, 0.f});
		const V3 wrist_to_head = Sub(head.position_xyz, hand.wrist.position_xyz);
		if (AngleDeg(palm_normal, wrist_to_head) > params.palm_to_head_max_deg)
			return false;
		if (!params.require_gaze)
			return true;
		const V3 gaze = Rotate(head.orientation_xyzw, V3{0.f, 0.f, -1.f});
		const V3 head_to_wrist = Sub(hand.wrist.position_xyz, head.position_xyz);
		return AngleDeg(gaze, head_to_wrist) <= params.gaze_max_deg;
	}

	void Detector::Reset()
	{
		m_pinched = true;
		m_held = false;
	}

	Output Detector::Step(const HandJoints& hand, const VRPose& head, const Params& params)
	{
		Output out;
		if (!hand.tracked || !hand.tips_valid || !hand.wrist.valid || !head.valid)
		{
			Reset();
			return out;
		}

		const float d = Length(Sub(hand.thumb_tip, hand.index_tip));
#ifdef VR_PADXR_PLANTED_PINCH_NO_HYSTERESIS
		const float open_m = params.pinch_close_m;
#else
		const float open_m = std::max(params.pinch_open_m, params.pinch_close_m);
#endif
		const bool was = m_pinched;
		if (m_pinched)
		{
			if (d > open_m)
				m_pinched = false;
		}
		else if (d < params.pinch_close_m)
		{
			m_pinched = true;
		}

		out.facing = IsFacing(hand, head, params);
		out.pinched = m_pinched;
		if (!was && m_pinched && out.facing)
		{
			m_held = true;
			out.pressed = true;
		}
		if (!m_pinched)
			m_held = false;
		out.held = m_held;
		return out;
	}
}
