// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#include "VR/SpatialControls.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace VR::SpatialControls
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDegToRad = kPi / 180.0f;

		float Clamp01(float v)
		{
			return std::clamp(v, 0.0f, 1.0f);
		}

		float Sign(float v)
		{
			return (v < 0.0f) ? -1.0f : 1.0f;
		}

		float Finite(float v)
		{
			return std::isfinite(v) ? v : 0.0f;
		}

		float Unwrap(float angle, float previous)
		{
			float d = angle - std::fmod(previous, 2.0f * kPi);
			while (d > kPi)
				d -= 2.0f * kPi;
			while (d < -kPi)
				d += 2.0f * kPi;
			return previous + d;
		}

		float SnapToDetent(float v, const Rail& rail)
		{
			if (rail.detents < 2 || !(rail.hi > rail.lo))
				return v;
			const float step = (rail.hi - rail.lo) / static_cast<float>(rail.detents - 1);
			const float k = std::round((v - rail.lo) / step);
			return std::clamp(rail.lo + k * step, rail.lo, rail.hi);
		}

		float RimAngle(const Vec3& local)
		{
			return std::atan2(local.x, local.y);
		}

		bool GrabRequested(const GrabParams& g, const HandInput& h, const Reach& r)
		{
			if (!h.valid || !r.in_reach)
				return false;
			return (g.grab_mode == GrabMode::Toggle) ? r.grip_edge : (h.squeeze > g.grab_on);
		}
		bool ReleaseRequested(const GrabParams& g, const HandInput& h, const Reach& r, bool* broke)
		{
			*broke = false;
			if (!h.valid)
				return true;
			if (g.break_away > 0.0f && Finite(r.distance) > g.break_away)
			{
				*broke = true;
				return true;
			}
			return (g.grab_mode == GrabMode::Toggle) ? r.grip_edge : (h.squeeze < g.grab_off);
		}
	}

	Vec3 operator+(const Vec3& a, const Vec3& b)
	{
		return {a.x + b.x, a.y + b.y, a.z + b.z};
	}
	Vec3 operator-(const Vec3& a, const Vec3& b)
	{
		return {a.x - b.x, a.y - b.y, a.z - b.z};
	}
	Vec3 operator*(const Vec3& a, float s)
	{
		return {a.x * s, a.y * s, a.z * s};
	}
	float Dot(const Vec3& a, const Vec3& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}
	float Length(const Vec3& a)
	{
		return std::sqrt(Dot(a, a));
	}

	Vec3 Quat::Rotate(const Vec3& v) const
	{
		const Vec3 q{x, y, z};
		const Vec3 t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x)};
		const Vec3 qxt{q.y * t.z - q.z * t.y, q.z * t.x - q.x * t.z, q.x * t.y - q.y * t.x};
		return {v.x + w * t.x + qxt.x, v.y + w * t.y + qxt.y, v.z + w * t.z + qxt.z};
	}

	Quat Quat::FromYaw(float radians)
	{
		return {0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f)};
	}

	Quat Quat::Conjugate() const
	{
		return {-x, -y, -z, w};
	}

	Quat Quat::operator*(const Quat& o) const
	{
		return {
			w * o.x + x * o.w + y * o.z - z * o.y,
			w * o.y - x * o.z + y * o.w + z * o.x,
			w * o.z + x * o.y - y * o.x + z * o.w,
			w * o.w - x * o.x - y * o.y - z * o.z,
		};
	}

	Frame PlaceControl(const Anchor& anchor, const Placement& placement)
	{
		const Quat yaw = Quat::FromYaw(anchor.yaw);
		Frame f;
		f.pivot = anchor.position + yaw.Rotate({placement.side, placement.height, -placement.forward});
		f.orientation = yaw * Quat::FromYaw(placement.yaw_deg * kDegToRad);
		return f;
	}

	Hands HandsFromSnapshot(const VRInputSnapshot& snapshot)
	{
		Hands h;
		for (int i = 0; i < 2; ++i)
		{
			const VRHandState& src = snapshot.hands[i];
			HandInput& dst = h.hand[i];
			dst.valid = snapshot.generation != 0 && snapshot.actions_active && src.grip_pose.valid;
			dst.position = {src.grip_pose.position_xyz[0], src.grip_pose.position_xyz[1], src.grip_pose.position_xyz[2]};
			dst.orientation = {src.grip_pose.orientation_xyzw[0], src.grip_pose.orientation_xyzw[1],
				src.grip_pose.orientation_xyzw[2], src.grip_pose.orientation_xyzw[3]};
			dst.squeeze = Clamp01(Finite(src.grip));
			dst.trigger = Clamp01(Finite(src.trigger));
		}
		return h;
	}

	float ApplyIdleDetent(float v, float idle_detent)
	{
		return (std::fabs(v) < idle_detent) ? 0.0f : v;
	}

	float ApplyOutputFloor(float v, float deadband, float floor)
	{
		v = Finite(v);
		deadband = std::clamp(Finite(deadband), 0.0f, 0.999f);
		floor = std::clamp(Finite(floor), 0.0f, 1.0f);
		const float mag = std::fabs(v);
		if (mag <= deadband)
			return 0.0f;
		const float t = Clamp01((mag - deadband) / (1.0f - deadband));
		return Sign(v) * (floor + (1.0f - floor) * t);
	}

	float GripDistance(const Vec3& hand, const Vec3& grip_centre, const Vec3& grip_axis, float grip_length)
	{
		const float half = std::max(Finite(grip_length), 0.0f) * 0.5f;
		const float axis_len = Length(grip_axis);
		const Vec3 axis = (axis_len > 1.0e-6f) ? grip_axis * (1.0f / axis_len) : Vec3{};
		const Vec3 d = hand - grip_centre;
		const float along = std::clamp(Dot(d, axis), -half, half);
		return Length(d - axis * along);
	}

	void MarkGripEdges(const GrabParams& grab, const Hands& hands, GripEdges& st, std::array<Reach, 2>& reach)
	{
		for (int i = 0; i < 2; ++i)
		{
			const float squeeze = Clamp01(Finite(hands.hand[i].squeeze));
			const bool was = st.on[i];
			const bool now = was ? !(squeeze < grab.grab_off) : (squeeze > grab.grab_on);
			st.on[i] = now;
			reach[i].grip_edge = now && !was;
		}
	}

	void Step1D(const GrabParams& grab, const Rail& rail, const Hands& hands, const std::array<Reach, 2>& reach,
		float dt, Axis1D& st)
	{
		const float lo = std::min(rail.lo, rail.hi);
		const float hi = std::max(rail.lo, rail.hi);
		dt = std::clamp(Finite(dt), 0.0f, 0.25f);
		st.broke_away = false;

		if (st.Held())
		{
			const HandInput& h = hands.hand[st.hand];
			bool broke = false;
			if (ReleaseRequested(grab, h, reach[st.hand], &broke))
			{
				const bool by_press = !broke && h.valid && grab.grab_mode == GrabMode::Toggle;
				st.hand = -1;
				st.broke_away = broke;
				if (grab.release == ReleaseMode::Detent)
					st.value = SnapToDetent(st.value, rail);
				if (by_press)
					return;
			}
			else
			{
				const float raw = Finite(reach[st.hand].raw);
				const float unclamped = raw - st.offset;
				const float clamped = std::clamp(unclamped, lo, hi);
				if (clamped != unclamped)
					st.offset = raw - clamped;
				st.value = clamped;
				return;
			}
		}

		int best = -1;
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			if (!GrabRequested(grab, h, reach[i]))
				continue;
			if (best < 0 || h.squeeze > hands.hand[best].squeeze)
				best = i;
		}
		if (best >= 0)
		{
			st.hand = best;
			st.offset = Finite(reach[best].raw) - st.value;
			return;
		}

		if (grab.release == ReleaseMode::Spring && st.value != 0.0f)
		{
			const float rest = std::clamp(0.0f, lo, hi);
			const float step = grab.spring_rate * dt;
			if (std::fabs(st.value - rest) <= step)
				st.value = rest;
			else
				st.value += (st.value > rest) ? -step : step;
		}
	}

	Vec3 SlideKnobPosition(const Frame& frame, const SlideParams& p, float value)
	{
		return frame.pivot + frame.orientation.Rotate(p.rail_dir) * (value * p.travel);
	}

	void StepSlide(const Frame& frame, const SlideParams& p, const GrabParams& grab, const Hands& hands, float dt,
		SlideState& st)
	{
		const Vec3 rail = frame.orientation.Rotate(p.rail_dir);
		const float travel = (p.travel > 1.0e-4f) ? p.travel : 1.0e-4f;
		const Vec3 knob = SlideKnobPosition(frame, p, st.axis.value);
		const Vec3 grip_axis = frame.orientation.Rotate(p.grip_dir);

		std::array<Reach, 2> reach{};
		MarkGripEdges(grab, hands, st.grip, reach);
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			if (!h.valid)
				continue;
			reach[i].raw = Dot(h.position - frame.pivot, rail) / travel;
			reach[i].distance = GripDistance(h.position, knob, grip_axis, grab.grab_length);
			reach[i].in_reach = reach[i].distance <= grab.grab_radius;
		}

		Rail r;
		r.lo = p.one_way ? 0.0f : -1.0f;
		r.hi = 1.0f;
		r.detents = p.detents;
		Step1D(grab, r, hands, reach, dt, st.axis);
	}

	void StepRotary(const Frame& frame, const RotaryParams& p, const GrabParams& grab, const Hands& hands, float dt,
		RotaryState& st)
	{
		const float half_lock = std::max(p.lock_to_lock_deg * 0.5f, 1.0f) * kDegToRad;

		std::array<Reach, 2> reach{};
		std::array<Vec3, 2> local{};
		MarkGripEdges(grab, hands, st.grip, reach);
		st.second_broke_away = false;
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			if (!h.valid)
			{
				st.cont_valid[i] = false;
				continue;
			}
			local[i] = frame.ToLocal(h.position);
			const float radial = std::sqrt(local[i].x * local[i].x + local[i].y * local[i].y);
			if (p.grip == RotaryGrip::Tip)
			{
				const float th = std::clamp(st.axis.value, -1.0f, 1.0f) * half_lock;
				const Vec3 tip{std::sin(th) * p.rim_radius, std::cos(th) * p.rim_radius, 0.0f};
				const Vec3 axis = p.tip_upright ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{std::sin(th), std::cos(th), 0.0f};
				reach[i].distance = GripDistance(local[i], tip, axis, grab.grab_length);
				reach[i].in_reach = reach[i].distance <= grab.grab_radius;
			}
			else
			{
				const float off_rim = radial - p.rim_radius;
				reach[i].distance = std::sqrt(off_rim * off_rim + local[i].z * local[i].z);
				reach[i].in_reach = std::fabs(off_rim) <= grab.grab_radius &&
									std::fabs(local[i].z) <= grab.grab_radius && radial > 1.0e-3f;
			}
			const float a = RimAngle(local[i]);
			st.cont_angle[i] = st.cont_valid[i] ? Unwrap(a, st.cont_angle[i]) : a;
			st.cont_valid[i] = true;
			reach[i].raw = st.cont_angle[i] / half_lock;
		}

		if (p.two_hand && st.axis.Held())
		{
			const int primary = st.axis.hand;
			const int other = 1 - primary;
			const HandInput& ho = hands.hand[other];
			bool other_broke = false;
			if (st.second_hand < 0 && GrabRequested(grab, ho, reach[other]))
			{
				const Vec3 d = local[other] - local[primary];
				const float a = std::atan2(d.x, d.y);
				st.pair_angle = st.pair_valid ? Unwrap(a, st.pair_angle) : a;
				st.pair_valid = true;
				st.second_hand = other;
				st.pair_offset = st.pair_angle / half_lock - st.axis.value;
			}
			else if (st.second_hand >= 0 && ReleaseRequested(grab, ho, reach[other], &other_broke))
			{
				st.second_hand = -1;
				st.pair_valid = false;
				st.second_broke_away = other_broke;
				st.axis.offset = reach[primary].raw - st.axis.value;
			}

			if (st.second_hand >= 0)
			{
				const HandInput& hp = hands.hand[primary];
				bool primary_broke = false;
				if (ReleaseRequested(grab, hp, reach[primary], &primary_broke))
				{
					st.axis.hand = st.second_hand;
					st.second_hand = -1;
					st.pair_valid = false;
					st.axis.broke_away = primary_broke;
					st.axis.offset = reach[st.axis.hand].raw - st.axis.value;
					return;
				}
				else
				{
					const Vec3 d = local[other] - local[primary];
					st.pair_angle = Unwrap(std::atan2(d.x, d.y), st.pair_angle);
					const float raw = st.pair_angle / half_lock;
					const float unclamped = raw - st.pair_offset;
					const float clamped = std::clamp(unclamped, -1.0f, 1.0f);
					if (clamped != unclamped)
						st.pair_offset = raw - clamped;
					st.axis.value = clamped;
					return;
				}
			}
		}
		else
		{
			st.second_hand = -1;
			st.pair_valid = false;
		}

		Rail r;
		r.lo = p.one_way ? 0.0f : -1.0f;
		r.hi = 1.0f;
		r.detents = p.detents;
		Step1D(grab, r, hands, reach, dt, st.axis);
	}

	namespace
	{
		float LeverHalfSweep(const LeverParams& p)
		{
			return std::max(p.sweep_deg * 0.5f, 1.0f) * kDegToRad;
		}
		float LeverArmLength(const LeverParams& p)
		{
			return std::max(Finite(p.arm_length), 1.0e-3f);
		}
		Vec3 LeverTipLocal(const LeverParams& p, float th)
		{
			return {std::sin(th) * LeverArmLength(p), std::cos(th) * LeverArmLength(p), 0.0f};
		}
		Vec3 LeverAxisLocal(const LeverParams& p, float th)
		{
			return p.upright ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{std::sin(th), std::cos(th), 0.0f};
		}
	}

	Frame LeverArmFrame(const Frame& control, const LeverParams& p)
	{
		Frame f;
		f.pivot = control.pivot - control.Up() * LeverArmLength(p);
		f.orientation = control.orientation * Quat::FromYaw(kPi * 0.5f);
		return f;
	}

	Vec3 LeverGripPosition(const Frame& control, const LeverParams& p, float value)
	{
		const float th = std::clamp(Finite(value), -1.0f, 1.0f) * LeverHalfSweep(p);
		return LeverArmFrame(control, p).ToWorld(LeverTipLocal(p, th));
	}

	Vec3 LeverGripAxis(const Frame& control, const LeverParams& p, float value)
	{
		const float th = std::clamp(Finite(value), -1.0f, 1.0f) * LeverHalfSweep(p);
		return LeverArmFrame(control, p).orientation.Rotate(LeverAxisLocal(p, th));
	}

	void StepLever(const Frame& control, const LeverParams& p, const GrabParams& grab, const Hands& hands, float dt,
		LeverState& st)
	{
		RotaryParams rp;
		rp.lock_to_lock_deg = std::max(p.sweep_deg, 2.0f);
		rp.rim_radius = LeverArmLength(p);
		rp.grip = RotaryGrip::Tip;
		rp.tip_upright = p.upright;
		rp.two_hand = false;
		rp.one_way = p.one_way;
		rp.detents = p.detents;
		StepRotary(LeverArmFrame(control, p), rp, grab, hands, dt, st);
	}

	Vec3 GimbalKnobPosition(const Frame& frame, const GimbalParams& p, const GimbalState& st)
	{
		return frame.pivot + frame.Right() * (st.x.value * p.travel_x) + frame.Up() * (st.y.value * p.travel_y);
	}

	void StepGimbal(const Frame& frame, const GimbalParams& p, const GrabParams& grab, const Hands& hands, float dt,
		GimbalState& st)
	{
		const Vec3 right = frame.Right();
		const Vec3 up = frame.Up();
		const float tx = (p.travel_x > 1.0e-4f) ? p.travel_x : 1.0e-4f;
		const float ty = (p.travel_y > 1.0e-4f) ? p.travel_y : 1.0e-4f;
		const Vec3 knob = GimbalKnobPosition(frame, p, st);

		std::array<Reach, 2> rx{}, ry{};
		MarkGripEdges(grab, hands, st.grip, rx);
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			ry[i].grip_edge = rx[i].grip_edge;
			if (!h.valid)
				continue;
			const Vec3 d = h.position - frame.pivot;
			rx[i].raw = Dot(d, right) / tx;
			ry[i].raw = Dot(d, up) / ty;
			rx[i].distance = ry[i].distance = GripDistance(h.position, knob, up, grab.grab_length);
			rx[i].in_reach = ry[i].in_reach = rx[i].distance <= grab.grab_radius;
		}

		const int was = st.x.hand;
		Rail r;
		Step1D(grab, r, hands, rx, dt, st.x);
		Step1D(grab, r, hands, ry, dt, st.y);
		st.hand = st.x.hand;

		if (p.twist)
		{
			const float range = std::max(p.twist_range_deg * 0.5f, 1.0f) * kDegToRad;
			if (st.hand >= 0)
			{
				const HandInput& h = hands.hand[st.hand];
				if (was < 0)
				{
					st.grab_orientation = h.orientation;
					st.cont_twist = 0.0f;
					st.cont_twist_valid = true;
					st.twist.offset = 0.0f - st.twist.value;
				}
				const Quat rel = h.orientation * st.grab_orientation.Conjugate();
				const Vec3 swung = frame.orientation.Conjugate().Rotate(rel.Rotate(right));
				const float a = std::atan2(-swung.z, swung.x);
				st.cont_twist = st.cont_twist_valid ? Unwrap(a, st.cont_twist) : a;
				st.cont_twist_valid = true;
				const float raw = st.cont_twist / range;
				const float unclamped = raw - st.twist.offset;
				const float clamped = std::clamp(unclamped, -1.0f, 1.0f);
				if (clamped != unclamped)
					st.twist.offset = raw - clamped;
				st.twist.value = clamped;
				st.twist.hand = st.hand;
			}
			else
			{
				st.twist.hand = -1;
				st.cont_twist_valid = false;
				std::array<Reach, 2> none{};
				Step1D(grab, r, hands, none, dt, st.twist);
			}
		}
	}

	namespace
	{
		void SelectorArcSpan(const SelectorParams& p, float* lo, float* step_angle)
		{
			const float half = LeverHalfSweep(p.arc);
			*lo = p.arc.one_way ? 0.0f : -half;
			*step_angle = (half - *lo) / static_cast<float>(std::max(p.positions, 2) - 1);
		}
	}

	Vec3 SelectorKnobPosition(const Frame& frame, const SelectorParams& p, int index)
	{
		const int n = std::max(p.positions, 2);
		if (p.on_arc)
		{
			float lo = 0.0f, step_angle = 1.0f;
			SelectorArcSpan(p, &lo, &step_angle);
			const float th = lo + static_cast<float>(std::clamp(index, 0, n - 1)) * step_angle;
			return LeverArmFrame(frame, p.arc).ToWorld(LeverTipLocal(p.arc, th));
		}
		const float step = p.travel / static_cast<float>(n - 1);
		return frame.pivot + frame.orientation.Rotate(p.rail_dir) * (static_cast<float>(index) * step);
	}

	void StepSelector(const Frame& frame, const SelectorParams& p, const GrabParams& grab, const Hands& hands,
		float dt, SelectorState& st)
	{
		(void)dt;
		st.shifted_up = st.shifted_down = false;
		st.broke_away = false;
		const int n = std::max(p.positions, 2);
		st.index = std::clamp(st.index, 0, n - 1);
		const Vec3 knob = SelectorKnobPosition(frame, p, st.index);

		const float step = std::max(p.travel, 1.0e-4f) / static_cast<float>(n - 1);
		const Vec3 rail = frame.orientation.Rotate(p.rail_dir);
		const Frame arm = p.on_arc ? LeverArmFrame(frame, p.arc) : Frame{};
		float arc_lo = 0.0f, arc_step = 1.0f;
		if (p.on_arc)
			SelectorArcSpan(p, &arc_lo, &arc_step);
		auto path = [&](int i) {
			if (p.on_arc)
				return (RimAngle(arm.ToLocal(hands.hand[i].position)) - arc_lo) / arc_step;
			return Dot(hands.hand[i].position - frame.pivot, rail) / step;
		};
		const Vec3 grip_axis = p.on_arc ?
								   arm.orientation.Rotate(LeverAxisLocal(p.arc, arc_lo + static_cast<float>(st.index) * arc_step)) :
								   frame.orientation.Rotate(p.grip_dir);

		std::array<Reach, 2> reach{};
		MarkGripEdges(grab, hands, st.grip, reach);
		for (int i = 0; i < 2; ++i)
		{
			if (!hands.hand[i].valid)
				continue;
			reach[i].distance = GripDistance(hands.hand[i].position, knob, grip_axis, grab.grab_length);
			reach[i].in_reach = reach[i].distance <= grab.grab_radius;
		}

		if (st.hand >= 0)
		{
			const HandInput& h = hands.hand[st.hand];
			bool broke = false;
			if (ReleaseRequested(grab, h, reach[st.hand], &broke))
			{
				st.hand = -1;
				st.broke_away = broke;
				return;
			}
			const float q = path(st.hand) - st.offset;
			const float hyst = std::clamp(p.hysteresis, 0.0f, 0.49f);
			if (q > static_cast<float>(st.index) + 0.5f + hyst && st.index < n - 1)
			{
				st.index++;
				st.shifted_up = true;
			}
			else if (q < static_cast<float>(st.index) - 0.5f - hyst && st.index > 0)
			{
				st.index--;
				st.shifted_down = true;
			}
			return;
		}

		int best = -1;
		for (int i = 0; i < 2; ++i)
		{
			const HandInput& h = hands.hand[i];
			if (!GrabRequested(grab, h, reach[i]))
				continue;
			if (best < 0 || h.squeeze > hands.hand[best].squeeze)
				best = i;
		}
		if (best >= 0)
		{
			st.hand = best;
			st.offset = path(best) - static_cast<float>(st.index);
		}
	}

	bool StepResetChord(bool left_click, bool right_click, float dt, float hold_s, ChordState& st)
	{
		if (!(left_click && right_click))
		{
			st.held_s = 0.0f;
			st.fired = false;
			return false;
		}
		st.held_s += std::clamp(Finite(dt), 0.0f, 0.25f);
		if (st.fired || st.held_s < std::max(Finite(hold_s), 0.0f))
			return false;
		st.fired = true;
		return true;
	}

	namespace
	{
		constexpr const char* kControlNames[kControlCount] = {
			"LeftLever", "RightLever", "Power", "Steer", "GrabbedLeft", "GrabbedRight",
			"Throttle", "Grabbed", "Notch1", "Notch2", "Notch3", "Notch4", "Notch5", "Notch6", "Notch7", "Notch8",
			"Steering",
			"Gear1", "Gear2", "Gear3", "Gear4", "Gear5", "Gear6", "GearR", "GearN", "ShiftUp", "ShiftDown",
			"X", "Y", "Twist", "Trigger",
			"PointerX", "PointerY", "OnScreen", "A", "B",
		};

		constexpr ControlDef kThrottleControls[] = {
			{ControlId::Throttle, "Throttle", ControlType::Axis},
			{ControlId::Grabbed, "Grabbed", ControlType::Button},
			{ControlId::Notch1, "Notch1", ControlType::Button},
			{ControlId::Notch2, "Notch2", ControlType::Button},
			{ControlId::Notch3, "Notch3", ControlType::Button},
			{ControlId::Notch4, "Notch4", ControlType::Button},
			{ControlId::Notch5, "Notch5", ControlType::Button},
			{ControlId::Notch6, "Notch6", ControlType::Button},
			{ControlId::Notch7, "Notch7", ControlType::Button},
			{ControlId::Notch8, "Notch8", ControlType::Button},
		};
		constexpr ControlDef kTwinThrottlesControls[] = {
			{ControlId::LeftLever, "LeftLever", ControlType::Axis},
			{ControlId::RightLever, "RightLever", ControlType::Axis},
			{ControlId::Power, "Power", ControlType::Axis},
			{ControlId::Steer, "Steer", ControlType::Axis},
			{ControlId::GrabbedLeft, "GrabbedLeft", ControlType::Button},
			{ControlId::GrabbedRight, "GrabbedRight", ControlType::Button},
		};
		constexpr ControlDef kWheelControls[] = {
			{ControlId::Steering, "Steering", ControlType::Axis},
			{ControlId::GrabbedLeft, "GrabbedLeft", ControlType::Button},
			{ControlId::GrabbedRight, "GrabbedRight", ControlType::Button},
		};
		constexpr ControlDef kShifterControls[] = {
			{ControlId::Gear1, "Gear1", ControlType::Button},
			{ControlId::Gear2, "Gear2", ControlType::Button},
			{ControlId::Gear3, "Gear3", ControlType::Button},
			{ControlId::Gear4, "Gear4", ControlType::Button},
			{ControlId::Gear5, "Gear5", ControlType::Button},
			{ControlId::Gear6, "Gear6", ControlType::Button},
			{ControlId::GearR, "GearR", ControlType::Button},
			{ControlId::GearN, "GearN", ControlType::Button},
			{ControlId::ShiftUp, "ShiftUp", ControlType::Button},
			{ControlId::ShiftDown, "ShiftDown", ControlType::Button},
			{ControlId::Grabbed, "Grabbed", ControlType::Button},
		};
		constexpr ControlDef kStickControls[] = {
			{ControlId::X, "X", ControlType::Axis},
			{ControlId::Y, "Y", ControlType::Axis},
			{ControlId::Twist, "Twist", ControlType::Axis},
			{ControlId::Trigger, "Trigger", ControlType::UnitAxis},
			{ControlId::Grabbed, "Grabbed", ControlType::Button},
		};
		constexpr ControlDef kLightGunControls[] = {
			{ControlId::PointerX, "PointerX", ControlType::Axis},
			{ControlId::PointerY, "PointerY", ControlType::Axis},
			{ControlId::OnScreen, "OnScreen", ControlType::Button},
			{ControlId::Trigger, "Trigger", ControlType::UnitAxis},
			{ControlId::A, "A", ControlType::Button},
			{ControlId::B, "B", ControlType::Button},
			{ControlId::Grabbed, "Grabbed", ControlType::Button},
		};

		constexpr DeviceDef kCatalogue[] = {
			{DeviceKind::Throttle, "Throttle", kThrottleControls},
			{DeviceKind::TwinThrottles, "TwinThrottles", kTwinThrottlesControls},
			{DeviceKind::Wheel, "Wheel", kWheelControls},
			{DeviceKind::Shifter, "Shifter", kShifterControls},
			{DeviceKind::Stick, "Stick", kStickControls},
			{DeviceKind::LightGun, "LightGun", kLightGunControls},
		};
		static_assert(std::size(kCatalogue) == static_cast<size_t>(DeviceKind::Count));

		float& At(ControlValues& v, ControlId id)
		{
			return v[static_cast<u32>(id)];
		}
	}

	std::span<const DeviceDef> Catalogue()
	{
		return kCatalogue;
	}

	ControlType ControlTypeOf(ControlId id)
	{
		for (const DeviceDef& d : kCatalogue)
		{
			for (const ControlDef& c : d.controls)
			{
				if (c.id == id)
					return c.type;
			}
		}
		return ControlType::Axis;
	}

	const DeviceDef* FindDevice(std::string_view name)
	{
		for (const DeviceDef& d : kCatalogue)
		{
			if (name == d.name)
				return &d;
		}
		return nullptr;
	}

	const DeviceDef* FindDevice(DeviceKind kind)
	{
		const u32 i = static_cast<u32>(kind);
		return (i < std::size(kCatalogue)) ? &kCatalogue[i] : nullptr;
	}

	const ControlDef* FindControl(const DeviceDef& device, std::string_view name)
	{
		for (const ControlDef& c : device.controls)
		{
			if (name == c.name)
				return &c;
		}
		return nullptr;
	}

	const ControlDef* FindControl(const DeviceDef& device, ControlId id)
	{
		for (const ControlDef& c : device.controls)
		{
			if (c.id == id)
				return &c;
		}
		return nullptr;
	}

	const char* ControlName(ControlId id)
	{
		const u32 i = static_cast<u32>(id);
		return (i < kControlCount) ? kControlNames[i] : "";
	}

	std::optional<ControlId> ParseControlId(std::string_view name)
	{
		for (u32 i = 0; i < kControlCount; ++i)
		{
			if (name == kControlNames[i])
				return static_cast<ControlId>(i);
		}
		return std::nullopt;
	}

	const char* DeviceName(DeviceKind kind)
	{
		const DeviceDef* d = FindDevice(kind);
		return d ? d->name : "";
	}

	Frame LeverFrame(const Frame& control, const TwinThrottlesParams& p, int side)
	{
		Frame f = control;
		const float half = p.lever_spacing * 0.5f;
		f.pivot = control.pivot + control.Right() * (side == 0 ? -half : half);
		return f;
	}

	void StepTwinThrottles(const Frame& frame, const TwinThrottlesParams& p, const Hands& hands, float dt,
		TwinThrottlesState& st)
	{
		Hands for_left = hands;
		if (st.right.axis.Held())
			for_left.hand[st.right.axis.hand].valid = false;
		StepLever(LeverFrame(frame, p, 0), p.lever, p.grab, for_left, dt, st.left);

		Hands for_right = hands;
		if (st.left.axis.Held())
			for_right.hand[st.left.axis.hand].valid = false;
		StepLever(LeverFrame(frame, p, 1), p.lever, p.grab, for_right, dt, st.right);

		const float left = ApplyIdleDetent(st.left.axis.value, p.grab.idle_detent);
		const float right = ApplyIdleDetent(st.right.axis.value, p.grab.idle_detent);
		st.engaged = TwinThrottlesEngage(TwinThrottlesPower(left, right, p), p, st.engaged);
	}

	bool TwinThrottlesEngage(float power, const TwinThrottlesParams& p, bool engaged)
	{
		if (!(p.engage > 0.0f))
			return false;
		const float on = p.engage;
		const float off = std::max(p.engage - std::max(p.engage_hysteresis, 0.0f), 0.0f);
		if (engaged)
			return !(power < off);
		return power >= on;
	}

	float TwinThrottlesPower(float left, float right, const TwinThrottlesParams& p)
	{
		const float power = (p.power == PowerMode::Max) ? std::max(left, right) : (left + right) * 0.5f;
		return std::clamp(power, 0.0f, 1.0f);
	}

	float TwinThrottlesSteer(float left, float right, const TwinThrottlesParams& p)
	{
		const float full = (std::fabs(p.steer_full_lock) > 1.0e-4f) ? p.steer_full_lock : 1.0f;
		float s = std::clamp((left - right) / full, -1.0f, 1.0f);
		const float mag = std::fabs(s);
		const float deadband = std::clamp(p.steer_deadband, 0.0f, 0.999f);
		if (mag <= deadband)
			return 0.0f;
		float t = (mag - deadband) / (1.0f - deadband);
		const float curve = std::clamp(p.steer_curve, 0.2f, 5.0f);
		t = std::pow(Clamp01(t), curve);
		const float signed_t = Sign(s) * t * ((p.steer_sign < 0.0f) ? -1.0f : 1.0f);
		return ApplyOutputFloor(signed_t, 0.0f, p.output_floor);
	}

	float TwinThrottlesLeverOutput(float lever, const TwinThrottlesParams& p)
	{
		const float s = std::clamp(Finite(lever), -1.0f, 1.0f);
		const float mag = std::fabs(s);
		const float deadband = std::clamp(p.steer_deadband, 0.0f, 0.999f);
		if (mag <= deadband)
			return 0.0f;
		float t = (mag - deadband) / (1.0f - deadband);
		const float curve = std::clamp(p.steer_curve, 0.2f, 5.0f);
		t = std::pow(Clamp01(t), curve);
		const float signed_t = Sign(s) * t * ((p.lever_sign < 0.0f) ? -1.0f : 1.0f);
		return ApplyOutputFloor(signed_t, 0.0f, p.output_floor);
	}

	ControlValues ComposeTwinThrottles(const TwinThrottlesParams& p, const TwinThrottlesState& st)
	{
		ControlValues v{};
		const float left = ApplyIdleDetent(st.left.axis.value, p.grab.idle_detent);
		const float right = ApplyIdleDetent(st.right.axis.value, p.grab.idle_detent);
		const bool direct = (p.mode == TwinMode::Direct);
		At(v, ControlId::LeftLever) = direct ? TwinThrottlesLeverOutput(left, p) : left;
		At(v, ControlId::RightLever) = direct ? TwinThrottlesLeverOutput(right, p) : right;
		At(v, ControlId::Power) = (p.engage > 0.0f) ? (st.engaged ? 1.0f : 0.0f) : TwinThrottlesPower(left, right, p);
		At(v, ControlId::Steer) = TwinThrottlesSteer(left, right, p);
		At(v, ControlId::GrabbedLeft) = st.left.axis.Held() ? 1.0f : 0.0f;
		At(v, ControlId::GrabbedRight) = st.right.axis.Held() ? 1.0f : 0.0f;
		return v;
	}

	void StepThrottle(const Frame& frame, const ThrottleParams& p, const Hands& hands, float dt, ThrottleState& st)
	{
		if (p.detented >= 2)
		{
			SelectorParams sp;
			sp.positions = std::min(p.detented, kMaxNotches);
			sp.on_arc = true;
			sp.arc = p.lever;
			StepSelector(frame, sp, p.grab, hands, dt, st.notch);
			const float t = static_cast<float>(st.notch.index) / static_cast<float>(sp.positions - 1);
			st.lever.axis.value = p.lever.one_way ? t : (t * 2.0f - 1.0f);
			st.lever.axis.hand = st.notch.hand;
			st.lever.axis.broke_away = st.notch.broke_away;
			return;
		}
		StepLever(frame, p.lever, p.grab, hands, dt, st.lever);
	}

	ControlValues ComposeThrottle(const ThrottleParams& p, const ThrottleState& st)
	{
		ControlValues v{};
		At(v, ControlId::Throttle) = ApplyIdleDetent(st.lever.axis.value, p.grab.idle_detent);
		At(v, ControlId::Grabbed) = st.lever.axis.Held() ? 1.0f : 0.0f;
		if (p.detented >= 2)
		{
			const int n = std::min(p.detented, kMaxNotches);
			for (int i = 0; i < n; ++i)
				v[static_cast<u32>(ControlId::Notch1) + i] = (st.notch.index == i) ? 1.0f : 0.0f;
		}
		return v;
	}

	void StepWheel(const Frame& frame, const WheelParams& p, const Hands& hands, float dt, WheelState& st)
	{
		StepRotary(frame, p.rim, p.grab, hands, dt, st.rotary);
	}

	ControlValues ComposeWheel(const WheelParams& p, const WheelState& st)
	{
		ControlValues v{};
		At(v, ControlId::Steering) = ApplyIdleDetent(st.rotary.axis.value, p.grab.idle_detent);
		const int h1 = st.rotary.axis.hand, h2 = st.rotary.second_hand;
		At(v, ControlId::GrabbedLeft) = (h1 == VRInputSnapshot::LEFT || h2 == VRInputSnapshot::LEFT) ? 1.0f : 0.0f;
		At(v, ControlId::GrabbedRight) = (h1 == VRInputSnapshot::RIGHT || h2 == VRInputSnapshot::RIGHT) ? 1.0f : 0.0f;
		return v;
	}

	int ShifterPositions(const ShifterParams& p)
	{
		return std::clamp(p.gears, 1, 6) + (p.has_reverse ? 1 : 0) + (p.has_neutral ? 1 : 0);
	}

	void StepShifter(const Frame& frame, const ShifterParams& p, const Hands& hands, float dt, ShifterState& st)
	{
		SelectorParams sp = p.gate;
		sp.positions = ShifterPositions(p);
		StepSelector(frame, sp, p.grab, hands, dt, st.sel);
	}

	ControlValues ComposeShifter(const ShifterParams& p, const ShifterState& st)
	{
		ControlValues v{};
		int i = 0;
		if (p.has_reverse)
			At(v, ControlId::GearR) = (st.sel.index == i++) ? 1.0f : 0.0f;
		if (p.has_neutral)
			At(v, ControlId::GearN) = (st.sel.index == i++) ? 1.0f : 0.0f;
		const int gears = std::clamp(p.gears, 1, 6);
		for (int g = 0; g < gears; ++g)
			v[static_cast<u32>(ControlId::Gear1) + g] = (st.sel.index == i + g) ? 1.0f : 0.0f;
		At(v, ControlId::ShiftUp) = st.sel.shifted_up ? 1.0f : 0.0f;
		At(v, ControlId::ShiftDown) = st.sel.shifted_down ? 1.0f : 0.0f;
		At(v, ControlId::Grabbed) = (st.sel.hand >= 0) ? 1.0f : 0.0f;
		return v;
	}

	void StepStick(const Frame& frame, const StickParams& p, const Hands& hands, float dt, StickState& st)
	{
		StepGimbal(frame, p.gimbal, p.grab, hands, dt, st.gimbal);
	}

	ControlValues ComposeStick(const StickParams& p, const StickState& st, const Hands& hands)
	{
		ControlValues v{};
		At(v, ControlId::X) = ApplyIdleDetent(st.gimbal.x.value, p.grab.idle_detent);
		At(v, ControlId::Y) = ApplyIdleDetent(st.gimbal.y.value, p.grab.idle_detent);
		At(v, ControlId::Twist) = p.gimbal.twist ? ApplyIdleDetent(st.gimbal.twist.value, p.grab.idle_detent) : 0.0f;
		const int h = st.gimbal.hand;
		At(v, ControlId::Trigger) = (h >= 0 && hands.hand[h].valid) ? hands.hand[h].trigger : 0.0f;
		At(v, ControlId::Grabbed) = (h >= 0) ? 1.0f : 0.0f;
		return v;
	}

	ControlValues ComposeLightGun(const LightGunParams& p, const Hands& hands)
	{
		ControlValues v{};
		const int h = (p.aim_hand == VRInputSnapshot::LEFT) ? VRInputSnapshot::LEFT : VRInputSnapshot::RIGHT;
		const HandInput& hand = hands.hand[h];
		At(v, ControlId::Trigger) = (hand.valid && hand.trigger > 0.5f) ? 1.0f : 0.0f;
		At(v, ControlId::A) = (hand.valid && hand.trigger > 0.5f) ? 0.0f : 0.0f;
		At(v, ControlId::Grabbed) = hand.valid ? 1.0f : 0.0f;
		return v;
	}
}
