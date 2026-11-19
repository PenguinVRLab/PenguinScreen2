// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/PadXRGestures.h"

#include <algorithm>
#include <cmath>

namespace VR::PadXRGestures
{
	namespace
	{
		using SpatialControls::Length;

		Vec3 PosOf(const VRPose& p) { return {p.position_xyz[0], p.position_xyz[1], p.position_xyz[2]}; }
		Quat QuatOf(const VRPose& p)
		{
			return {p.orientation_xyzw[0], p.orientation_xyzw[1], p.orientation_xyzw[2], p.orientation_xyzw[3]};
		}

		float Finite01(float v) { return std::isfinite(v) ? v : 0.0f; }

		void AddCue(Result* r, const Cue& c, int hand)
		{
			if (r->cue_count >= r->cues.size())
				return;
			Cue k = c;
			k.hand = static_cast<uint8_t>(hand);
			r->cues[r->cue_count++] = k;
		}

		bool Plan(Result* r, Gesture g, uint32_t* seq, double now_ms, int hand, const TitlePress& tp, float start_ms,
			float hold_ms)
		{
			PressPlan p;
			p.gesture = g;
			p.decided_ms = now_ms;
			p.dated_ms = now_ms;
			p.hand = hand;
			Refusal why = Refusal::None;
			if (!AddPress(&p, tp, start_ms, hold_ms, &why))
			{
				r->refusal = why;
				return false;
			}
			p.seq = ++*seq;
			r->fired = true;
			r->plan = p;
			return true;
		}

		bool Inv3(const float m[9], float out[9])
		{
			const float c00 = m[4] * m[8] - m[5] * m[7];
			const float c01 = m[5] * m[6] - m[3] * m[8];
			const float c02 = m[3] * m[7] - m[4] * m[6];
			const float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
			if (!std::isfinite(det) || std::fabs(det) < 1e-12f)
				return false;
			const float id = 1.0f / det;
			out[0] = c00 * id;
			out[1] = (m[2] * m[7] - m[1] * m[8]) * id;
			out[2] = (m[1] * m[5] - m[2] * m[4]) * id;
			out[3] = c01 * id;
			out[4] = (m[0] * m[8] - m[2] * m[6]) * id;
			out[5] = (m[2] * m[3] - m[0] * m[5]) * id;
			out[6] = c02 * id;
			out[7] = (m[1] * m[6] - m[0] * m[7]) * id;
			out[8] = (m[0] * m[4] - m[1] * m[3]) * id;
			return true;
		}

		Vec3 Mul3(const float m[9], const Vec3& v)
		{
			return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
				m[6] * v.x + m[7] * v.y + m[8] * v.z};
		}

		uint32_t Mix(uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x7feb352dU;
			x ^= x >> 15;
			x *= 0x846ca68bU;
			x ^= x >> 16;
			return x;
		}
	}

	uint32_t TitleCount(const TitlePress& tp)
	{
#ifdef VR_GESTURES_PLANTED_DEFAULT_COUNT
		return tp.transactions ? tp.transactions : 1;
#else
		return tp.transactions;
#endif
	}

	bool AddPress(PressPlan* plan, const TitlePress& tp, float start_ms, float hold_ms, Refusal* why)
	{
		const uint32_t count = TitleCount(tp);
		if (tp.bit < 0 || tp.bit > 15)
		{
			*why = Refusal::NoButton;
			return false;
		}
		if (count == 0)
		{
			*why = Refusal::NoTitleCount;
			return false;
		}
		const bool hold_first = tp.hold_bit >= 0 && tp.hold_bit <= 15;
		if (plan->count + (hold_first ? 2u : 1u) > kMaxSteps)
		{
			*why = Refusal::NoButton;
			return false;
		}
		const int press_index = plan->count + (hold_first ? 1 : 0);
		if (hold_first)
		{
			PressStep& h = plan->steps[plan->count++];
			h = PressStep{};
			h.bit = tp.hold_bit;
			h.min_tx = tp.hold_lead + count;
			h.through_step = press_index;
		}
		PressStep& s = plan->steps[plan->count++];
		s = PressStep{};
		s.bit = tp.bit;
		s.start_tx = hold_first ? tp.hold_lead : 0;
		s.start_ms = std::max(0.0f, start_ms);
		s.min_tx = count;
		s.hold_ms = std::max(0.0f, hold_ms);
		s.pressure = tp.pressure;
		return true;
	}

	size_t ToTransactions(const PressPlan& plan, float tx_per_second, std::array<TxStep, kMaxSteps>* out,
		bool* incomplete)
	{
		if (incomplete)
			*incomplete = false;
		const bool rate_known = std::isfinite(tx_per_second) && tx_per_second > 0.0f;
		auto tx_of = [&](float ms) -> uint32_t {
			if (!(ms > 0.0f))
				return 0;
			if (!rate_known)
			{
				if (incomplete)
					*incomplete = true;
				return 0;
			}
			return static_cast<uint32_t>(std::ceil(ms * tx_per_second / 1000.0f - 1e-4f));
		};
		std::array<uint32_t, kMaxSteps> start{}, count{};
		const size_t n = std::min<size_t>(plan.count, kMaxSteps);
		for (size_t i = 0; i < n; i++)
		{
			const PressStep& s = plan.steps[i];
			start[i] = s.start_tx + tx_of(s.start_ms);
			count[i] = std::max(s.min_tx, tx_of(s.hold_ms));
		}
		for (size_t i = 0; i < n; i++)
		{
			const int j = plan.steps[i].through_step;
			if (j >= 0 && static_cast<size_t>(j) < n && start[j] + count[j] > start[i])
				count[i] = std::max(count[i], start[j] + count[j] - start[i]);
		}
		size_t w = 0;
		for (size_t i = 0; i < n; i++)
		{
			if (plan.steps[i].bit < 0 || count[i] == 0)
				continue;
			(*out)[w++] = TxStep{plan.steps[i].bit, start[i], count[i], plan.steps[i].pressure};
		}
		return w;
	}

	BodyFrame UpdateBodyFrame(const BodyFrame& last, const VRPose& head)
	{
		BodyFrame b = last;
		if (!head.valid)
		{
			b.valid = false;
			return b;
		}
		b.origin = PosOf(head);
		const Vec3 f = QuatOf(head).Rotate({0.0f, 0.0f, -1.0f});
		const float horizontal = std::sqrt(f.x * f.x + f.z * f.z);
		if (std::isfinite(horizontal) && horizontal >= kYawHoldHorizontal)
			b.yaw = std::atan2(-f.x, -f.z);
		b.valid = true;
		return b;
	}

	Vec3 ZoneCentre(const BodyFrame& body, const Zone& z)
	{
		return body.origin + Quat::FromYaw(body.yaw).Rotate({z.side, z.height, -z.forward});
	}

	float Motion::Update(const Vec3& pos, double now_ms, float max_gap_ms)
	{
		if (have && pos.x == last.x && pos.y == last.y && pos.z == last.z)
		{
#ifndef VR_GESTURES_PLANTED_NO_REPEAT_SKIP
			if (now_ms - last_ms <= max_gap_ms)
				return kRepeat;
#else
			const double dt0 = now_ms - last_ms;
			last_ms = now_ms;
			return (dt0 > 0.0 && dt0 <= max_gap_ms) ? 0.0f : kGap;
#endif
		}
		const double dt = now_ms - last_ms;
		const bool usable = have && dt > 0.0 && dt <= max_gap_ms;
		const float speed = usable ? Length(pos - last) / static_cast<float>(dt / 1000.0) : kGap;
		have = true;
		last = pos;
		last_ms = now_ms;
		return speed;
	}

	bool GripEdge::Step(float grip, float on, float off)
	{
		grip = Finite01(grip);
		if (!closed && grip >= on)
		{
			closed = true;
			return true;
		}
		if (closed && grip <= off)
			closed = false;
		return false;
	}

	void ReachDetector::Reset()
	{
		m_hand = {};
		m_body = {};
		m_entries = 0;
	}

	Result ReachDetector::Step(const VRInputSnapshot& s, double now_ms, Gate when)
	{
		Result r;
		m_body = UpdateBodyFrame(m_body, s.head_pose);
		const size_t nz = std::min(zone_count, kMaxZones);
		for (int h = 0; h < 2; h++)
		{
			HandState& hs = m_hand[h];
			const VRHandState& hand = s.hands[h];
			const bool tracked = hand.grip_pose.valid && m_body.valid && nz > 0;
			const bool edge = tracked && hs.grip.Step(hand.grip, params.gripOn, params.gripOff);
			if (!tracked)
			{
				hs.zone = -1;
				hs.open_seen = false;
				continue;
			}
			const Vec3 pos = PosOf(hand.grip_pose);
			if (hs.zone >= 0)
			{
				const Zone& z = zones[hs.zone];
				if (Length(pos - ZoneCentre(m_body, z)) > z.radius + params.exitMarginM)
					hs.zone = -1;
			}
			bool entered = false;
			if (hs.zone < 0)
			{
				float best = 0.0f;
				for (size_t i = 0; i < nz; i++)
				{
					const float d = Length(pos - ZoneCentre(m_body, zones[i]));
					if (d < zones[i].radius && (hs.zone < 0 || d < best))
					{
						hs.zone = static_cast<int>(i);
						best = d;
					}
				}
				if (hs.zone >= 0)
				{
					entered = true;
					m_entries++;
					hs.open_seen = !hs.grip.closed;
				}
			}
			if (hs.zone < 0)
				continue;
			bool fire = edge && hs.open_seen;
#ifdef VR_GESTURES_PLANTED_REACH_CLOSED_ENTRY
			fire = fire || (entered && hs.grip.closed);
#else
			(void)entered;
#endif
			if (fire)
			{
				hs.open_seen = false;
				if (!when.parsed)
					r.refusal = Refusal::GateUnparsed;
				else if (when.open && !r.fired)
				{
					const Zone& z = zones[hs.zone];
					const float delay = params.releaseMoveStick ? params.pressDelayMs : 0.0f;
					if (Plan(&r, Gesture::Reach, &m_seq, now_ms, h, z.press, delay, 0.0f))
					{
						r.plan.neutral_left_stick = params.releaseMoveStick;
						r.plan.target = hs.zone;
					}
				}
			}
			if (!hs.grip.closed)
				hs.open_seen = true;
		}
		return r;
	}

	bool HandToGameWorld(const Vec3& hand, const VRPose& head, const GameCamera& cam, Vec3* world)
	{
		if (!head.valid || !(cam.metres_per_unit > 0.0f) || !std::isfinite(cam.metres_per_unit))
			return false;
		const Vec3 view = QuatOf(head).Conjugate().Rotate(hand - PosOf(head));
		const Vec3 game_cam = Mul3(cam.axes.data(), view) * (1.0f / cam.metres_per_unit);
		float r[9] = {cam.view[0], cam.view[1], cam.view[2], cam.view[4], cam.view[5], cam.view[6], cam.view[8],
			cam.view[9], cam.view[10]};
		float ri[9];
		float axes_inv[9];
		if (!Inv3(r, ri) || !Inv3(cam.axes.data(), axes_inv))
			return false;
		const Vec3 t{cam.view[3], cam.view[7], cam.view[11]};
		*world = Mul3(ri, game_cam - t);
		return std::isfinite(world->x) && std::isfinite(world->y) && std::isfinite(world->z);
	}

	float GameDistanceM(const Vec3& a, const Vec3& b, const GameCamera& cam)
	{
		return Length(a - b) * cam.metres_per_unit;
	}

	void ContactDetector::Reset() { m_hand = {}; }

	Result ContactDetector::Step(const VRInputSnapshot& s, double now_ms, Gate gameplay)
	{
		Result r;
		const size_t no = std::min(object_count, kMaxObjects);
		for (int h = 0; h < 2; h++)
		{
			HandState& hs = m_hand[h];
			const VRHandState& hand = s.hands[h];
			Vec3 world;
			const bool tracked =
				hand.grip_pose.valid && HandToGameWorld(PosOf(hand.grip_pose), s.head_pose, camera, &world);
			const bool edge = tracked && hs.grip.Step(hand.grip, params.gripOn, params.gripOff);
			if (!tracked)
			{
				hs.object = -1;
				hs.held = 0;
				hs.open_seen = false;
				continue;
			}
			int nearest = -1;
			float best = 0.0f;
			for (size_t i = 0; i < no; i++)
			{
				if (!objects[i].valid)
					continue;
				const float d = GameDistanceM(world, objects[i].position, camera);
#ifdef VR_GESTURES_PLANTED_CONTACT_NO_DISTANCE
				const bool touching = true;
#else
				const bool touching = d < params.radiusM;
#endif
				if (touching && (nearest < 0 || d < best))
				{
					nearest = static_cast<int>(i);
					best = d;
				}
			}
			if (nearest < 0)
			{
				hs.object = -1;
				hs.held = 0;
				continue;
			}
			if (nearest != hs.object)
			{
				hs.object = nearest;
				hs.held = 0;
				hs.open_seen = !hs.grip.closed;
			}
			hs.held++;
			if (edge && hs.open_seen && hs.held >= params.heldSamples)
			{
				hs.open_seen = false;
				if (!gameplay.parsed)
					r.refusal = Refusal::GateUnparsed;
				else if (gameplay.open && !r.fired)
				{
					if (Plan(&r, Gesture::Contact, &m_seq, now_ms, h, objects[nearest].press, 0.0f, 0.0f))
						r.plan.target = nearest;
				}
			}
			if (!hs.grip.closed)
				hs.open_seen = true;
		}
		return r;
	}

	void SwingDetector::Reset()
	{
		const SwingParams p = params;
		const TitlePress a = attack;
		*this = SwingDetector{};
		params = p;
		attack = a;
	}

	Result SwingDetector::Step(const VRInputSnapshot& s, double now_ms, bool hand_free, Gate when)
	{
		Result r;
		const int h = (params.hand == VRInputSnapshot::LEFT) ? VRInputSnapshot::LEFT : VRInputSnapshot::RIGHT;
		const VRHandState& hand = s.hands[h];
		if (!hand.grip_pose.valid)
		{
			m_motion.Reset();
			m_in_stroke = false;
			return r;
		}
		const Vec3 tip = PosOf(hand.grip_pose) + QuatOf(hand.grip_pose).Rotate({0.0f, 0.0f, -params.tipM});
		const Vec3 prev = m_motion.last;
		const float speed = m_motion.Update(tip, now_ms, params.maxGapMs);
		if (speed == Motion::kRepeat)
			return r;
		if (speed < 0.0f)
		{
			m_in_stroke = false;
		}
		else
		{
			if (!m_in_stroke && speed > params.speedOff)
			{
				m_in_stroke = true;
				m_stroke_fired = false;
				m_start = prev;
				m_path = 0.0f;
			}
			if (m_in_stroke)
			{
				m_path += Length(tip - prev);
				if (!m_stroke_fired && speed >= params.speedOn)
				{
					const float disp = Length(tip - m_start);
#ifdef VR_GESTURES_PLANTED_SWING_NO_STRAIGHT
					const bool straight = true;
#else
					const bool straight = m_path > 0.0f && disp / m_path >= params.straightness;
#endif
					if (disp >= params.travelM && straight)
					{
						m_stroke_fired = true;
						const bool cooling = now_ms - m_last_fire_ms < params.cooldownMs;
						if (!when.parsed)
							r.refusal = Refusal::GateUnparsed;
						else if (!cooling && hand_free && when.open)
						{
							if (Plan(&r, Gesture::Swing, &m_seq, now_ms, h, attack, 0.0f, params.pressMs))
								m_last_fire_ms = now_ms;
						}
					}
				}
				if (speed < params.speedOff)
					m_in_stroke = false;
			}
		}
		return r;
	}

	float AssistPressStartMs(const AssistSchedule& a, uint32_t k, uint32_t seed)
	{
		if (!(a.press_hz > 0.0f))
			return 0.0f;
		const float period = 1000.0f / a.press_hz;
		const float j = std::clamp(a.jitter, 0.0f, 0.45f);
		const uint32_t hsh = Mix(seed * 0x9E3779B9U ^ Mix(k + 1));
		const float u = static_cast<float>(hsh) / 4294967295.0f * 2.0f - 1.0f;
		return std::max(0.0f, static_cast<float>(k) * period + u * j * period);
	}

	void ShakeDetector::Reset()
	{
		const ShakeParams p = params;
		const auto b = buttons;
		*this = ShakeDetector{};
		params = p;
		buttons = b;
	}

	Result ShakeDetector::Step(const VRInputSnapshot& s, double now_ms, Gate grabbed, uint16_t player_down, bool online)
	{
		Result r;
		bool gate_ok = grabbed.parsed;
#ifdef VR_GESTURES_PLANTED_SHAKE_BAD_GATE_ARMS
		gate_ok = true;
#endif
		if (!gate_ok)
		{
			r.refusal = Refusal::GateUnparsed;
			r.cancel = m_running;
			m_running = false;
			m_assist = {};
			return r;
		}
		if (params.mode == ShakeMode::Assist && online)
		{
			r.refusal = Refusal::AssistOnline;
			r.cancel = m_running;
			m_running = false;
			m_assist = {};
			return r;
		}

		uint32_t new_reversals = 0;
		float max_speed = 0.0f;
		for (int h = 0; h < 2; h++)
		{
			HandState& hs = m_hand[h];
			const VRHandState& hand = s.hands[h];
			if (!hand.grip_pose.valid)
			{
				hs = HandState{};
				continue;
			}
			const Vec3 pos = PosOf(hand.grip_pose);
			const Vec3 prev = hs.motion.last;
			const float speed = hs.motion.Update(pos, now_ms, params.maxGapMs);
			if (speed == Motion::kRepeat)
				continue;
			if (speed >= 0.0f)
			{
				max_speed = std::max(max_speed, speed);
				if (speed >= params.reversalMinSpeed)
				{
					const Vec3 dir = (pos - prev) * (1.0f / Length(pos - prev));
					if (hs.have_dir && SpatialControls::Dot(dir, hs.dir) < 0.0f)
					{
						new_reversals++;
						m_reversal_ms[m_reversal_head] = now_ms;
						m_reversal_head = (m_reversal_head + 1) % m_reversal_ms.size();
						m_reversal_count = std::min(m_reversal_count + 1, m_reversal_ms.size());
					}
					hs.dir = dir;
					hs.have_dir = true;
				}
			}
			else
				hs.have_dir = false;
		}
		uint32_t in_window = 0;
		for (size_t i = 0; i < m_reversal_count; i++)
			if (now_ms - m_reversal_ms[i] <= params.reversalWindowMs)
				in_window++;
		const bool trigger = max_speed >= params.speed || in_window >= params.reversals;
		if (trigger)
			m_active_until = now_ms + params.holdMs;
		m_active = now_ms <= m_active_until;

		bool gate_open = grabbed.open;
#ifdef VR_GESTURES_PLANTED_SHAKE_NO_GATE
		gate_open = true;
#endif
		if (!gate_open || !m_active)
		{
			r.cancel = m_running && !gate_open;
			if (!gate_open)
				m_running = false;
			m_assist = {};
			return r;
		}

		auto free_bit = [&](const TitlePress& tp) {
			return tp.bit >= 0 && tp.bit <= 15 && !(player_down & (1u << tp.bit));
		};
		if (params.mode == ShakeMode::Assist)
		{
			AssistSchedule a;
			a.press_hz = params.assistPressHz;
			a.press_ms = params.assistPressMs;
			a.stick_hz = params.assistStick ? params.assistStickHz : 0.0f;
			a.jitter = params.assistJitter;
			a.bits = {free_bit(buttons[0]) ? buttons[0].bit : Btn::None, free_bit(buttons[1]) ? buttons[1].bit : Btn::None};
			a.transactions = TitleCount(buttons[0]);
			a.on = (a.bits[0] != Btn::None || a.bits[1] != Btn::None) && a.transactions > 0;
			if (!a.on && a.transactions == 0)
				r.refusal = Refusal::NoTitleCount;
			m_assist = a;
			m_running = m_running || a.on;
			return r;
		}
		m_assist = {};
		if (new_reversals == 0)
			return r;
		for (int tries = 0; tries < 2; tries++)
		{
			const TitlePress& tp = buttons[m_next_button % 2];
			m_next_button++;
			if (!free_bit(tp))
				continue;
			if (Plan(&r, Gesture::Shake, &m_seq, now_ms, -1, tp, 0.0f, 0.0f))
				m_running = true;
			break;
		}
		return r;
	}

	void CarryDetector::Reset()
	{
		m_hand = {};
		m_body = {};
	}

	Result CarryDetector::Step(const VRInputSnapshot& s, double now_ms, Gate ready)
	{
		Result r;
		m_body = UpdateBodyFrame(m_body, s.head_pose);
		for (int c = 0; c < 2; c++)
		{
			HandState& hs = m_hand[c];
			const VRHandState& hand = s.hands[c];
			const VRHandState& other = s.hands[1 - c];
			const bool tracked = hand.grip_pose.valid && m_body.valid;
			const bool edge = tracked ? hs.grip.Step(hand.grip, params.gripOn, params.gripOff) : false;
			if (!(source[c].radius > 0.0f))
			{
				hs = HandState{};
				continue;
			}
			if (hs.carrying)
			{
				if (!tracked)
				{
					if (hs.lost_since < 0.0)
						hs.lost_since = now_ms;
#ifdef VR_GESTURES_PLANTED_CARRY_NO_GRACE
					const bool lapsed = true;
#else
					const bool lapsed = now_ms - hs.lost_since > params.trackingGraceMs;
#endif
					if (lapsed)
					{
						hs.carrying = false;
						hs.open_seen = false;
					}
					continue;
				}
				hs.lost_since = -1.0;
				if (now_ms - hs.grabbed_ms > params.withinMs)
				{
					hs.carrying = false;
					hs.open_seen = false;
				}
				else if (!hs.grip.closed)
				{
					hs.carrying = false;
					hs.open_seen = false;
					const bool near = other.grip_pose.valid &&
									  Length(PosOf(hand.grip_pose) - PosOf(other.grip_pose)) <= params.toRadiusM;
					if (near)
					{
						if (!ready.parsed)
							r.refusal = Refusal::GateUnparsed;
						else if (ready.open && !r.fired)
						{
							if (Plan(&r, Gesture::Carry, &m_seq, now_ms, c, reload, 0.0f, params.pressMs) &&
								fire_bit >= 0 && fire_bit <= 15)
								r.plan.withhold_mask = static_cast<uint16_t>(1u << fire_bit);
						}
					}
				}
				continue;
			}
			if (!tracked)
			{
				hs.in_source = false;
				hs.open_seen = false;
				continue;
			}
			const bool inside = Length(PosOf(hand.grip_pose) - ZoneCentre(m_body, source[c])) < source[c].radius;
			if (inside && !hs.in_source)
				hs.open_seen = !hs.grip.closed;
			hs.in_source = inside;
			if (inside && edge && hs.open_seen)
			{
				hs.carrying = true;
				hs.grabbed_ms = now_ms;
				hs.lost_since = -1.0;
				hs.open_seen = false;
				continue;
			}
			if (inside && !hs.grip.closed)
				hs.open_seen = true;
		}
		return r;
	}

	void ThrowDetector::Reset()
	{
		m_hand = {};
		m_last_throw_ms = -1e18;
	}

	Result ThrowDetector::Step(const VRInputSnapshot& s, double now_ms, const TitlePress& receiver, Gate possession)
	{
		Result r;
		constexpr float kMaxGapMs = 100.0f;
		for (int h = 0; h < 2; h++)
		{
			HandState& hs = m_hand[h];
			const VRHandState& hand = s.hands[h];
			if (!hand.grip_pose.valid)
			{
				if (hs.holding)
					hs.lost = true;
				hs.motion.Reset();
				hs.hist_count = 0;
				continue;
			}
			const float speed = hs.motion.Update(PosOf(hand.grip_pose), now_ms, kMaxGapMs);
			if (speed >= 0.0f)
			{
				hs.hist[hs.hist_head] = Sample{now_ms, speed};
				hs.hist_head = (hs.hist_head + 1) % hs.hist.size();
				hs.hist_count = std::min(hs.hist_count + 1, hs.hist.size());
			}

			const bool edge = hs.grip.Step(hand.grip, params.gripOn, params.gripOff);
			if (edge)
			{
				hs.holding = true;
				hs.lost = false;
				continue;
			}
			if (!hs.holding || hs.grip.closed)
				continue;
			hs.holding = false;
			if (hs.lost)
			{
				hs.lost = false;
				continue;
			}
			float peak = -1.0f;
			double peak_ms = now_ms;
#ifdef VR_GESTURES_PLANTED_THROW_NO_BACKDATE
			if (hs.hist_count > 0)
				peak = hs.hist[(hs.hist_head + hs.hist.size() - 1) % hs.hist.size()].speed;
#else
			for (size_t i = 0; i < hs.hist_count; i++)
			{
				const Sample& smp = hs.hist[i];
				if (now_ms - smp.ms <= params.peakWindowMs && smp.ms <= now_ms && smp.speed > peak)
				{
					peak = smp.speed;
					peak_ms = smp.ms;
				}
			}
#endif
			if (peak < 0.0f)
				continue;
			const float latency = params.source == PoseSource::Hands ? params.backDateHandsMs : params.backDateControllersMs;
			const double dated = peak_ms - latency;
			if (!(params.bulletSpeedMps > 0.0f) || !(params.minThrowSpeedMps > 0.0f))
			{
				r.refusal = Refusal::NoClassThreshold;
				continue;
			}
			if (peak < params.minThrowSpeedMps)
				continue;
			if (!possession.parsed)
			{
				r.refusal = Refusal::GateUnparsed;
				continue;
			}
			if (!possession.open || now_ms - m_last_throw_ms < params.rethrowBlockMs || r.fired)
				continue;
			const bool bullet = peak >= params.bulletSpeedMps;
			if (Plan(&r, Gesture::Throw, &m_seq, now_ms, h, receiver, 0.0f, bullet ? params.bulletHoldMs : params.lobTapMs))
			{
				r.plan.dated_ms = dated;
				r.plan.throw_class = bullet ? 2 : 1;
				m_last_throw_ms = now_ms;
			}
		}
		return r;
	}

	void MeshHandoffDetector::Reset() { m_hand = {}; }

	Result MeshHandoffDetector::Step(const VRInputSnapshot& s, double now_ms, const Vec3& target, bool target_valid,
		Gate handoff_play)
	{
		Result r;
#ifdef VR_GESTURES_PLANTED_MESH_WIDE
		const float radius = 1.0f;
#else
		const float radius = params.targetRadiusM;
#endif
		for (int h = 0; h < 2; h++)
		{
			HandState& hs = m_hand[h];
			const VRHandState& hand = s.hands[h];
			if (!hand.grip_pose.valid || !target_valid)
			{
				hs.motion.Reset();
				hs.speed = -1.0f;
				hs.in_target = false;
				continue;
			}
			const bool edge = hs.commit.Step(hand.trigger, params.commitOn, params.commitOff);
			const Vec3 pos = PosOf(hand.grip_pose);
			const float measured = hs.motion.Update(pos, now_ms, params.maxGapMs);
			if (measured != Motion::kRepeat)
				hs.speed = measured;
			const float speed = hs.speed;
			const float d = Length(pos - target);
			if (hs.in_target && d > radius)
				hs.in_target = false;
			if (!hs.in_target && d <= radius && speed >= 0.0f && speed < params.handSpeedGate)
			{
				hs.in_target = true;
				hs.entered_ms = now_ms;
				hs.done = false;
				AddCue(&r, params.entryCue, h);
			}
			if (!hs.in_target || !edge || hs.done || now_ms - hs.entered_ms > params.completeWindowMs)
				continue;
			hs.done = true;
			if (!handoff_play.parsed)
			{
				r.refusal = Refusal::GateUnparsed;
				continue;
			}
			if (!handoff_play.open || r.fired)
				continue;
			AddCue(&r, params.completeCue, h);
			if (landing.bit == Btn::None)
			{
				r.fired = true;
				r.plan = PressPlan{};
				r.plan.gesture = Gesture::MeshHandoff;
				r.plan.seq = ++m_seq;
				r.plan.decided_ms = r.plan.dated_ms = now_ms;
				r.plan.hand = h;
			}
			else
				Plan(&r, Gesture::MeshHandoff, &m_seq, now_ms, h, landing, 0.0f, 0.0f);
		}
		return r;
	}
}

#ifdef VR_PADXR_GESTURES_HAVE_CORE
namespace VR::PadXRGestures
{
	PadXR::MemberDesc Describe(Gesture g)
	{
		using namespace PadXR;
		MemberDesc d;
		d.family = Family::PAD;
		d.route = Route::Merge;
		d.online = Online::Any;
		d.clock = Clock::Game;
		d.res[static_cast<size_t>(Res::Haptics)] = Claim::Shared;
		constexpr u16 base = Prim::R1Source | Prim::R6Detector | Prim::R7Output | Prim::R8Gate | Prim::R9Port |
							 Prim::R12GameState;
		switch (g)
		{
			case Gesture::Reach:
			case Gesture::Contact:
				d.id = "padReach";
				d.kind = "padReach";
				d.primitives = base | Prim::R2Frame | Prim::R3Zone | Prim::R10Feedback | Prim::R11Seat;
				d.needs = {Need::Controllers, Need::SeatCalibration, 0, 0};
				break;
			case Gesture::Swing:
				d.id = "padGesture.swing";
				d.kind = "padGesture";
				d.primitives = base;
				d.needs = {Need::Controllers, 0, 0, 0};
				break;
			case Gesture::Shake:
				d.id = "padGesture.shake";
				d.kind = "padGesture";
				d.primitives = base | Prim::R10Feedback;
				d.needs = {Need::Controllers, 0, 0, 0};
				break;
			case Gesture::Carry:
				d.id = "padGesture.carry";
				d.kind = "padGesture";
				d.primitives = base | Prim::R2Frame | Prim::R3Zone;
				d.needs = {Need::Controllers, 0, 0, 0};
				break;
			case Gesture::Throw:
				d.id = "throw";
				d.kind = "padGesture";
				d.primitives = base | Prim::R10Feedback;
				d.needs = {Need::Controllers | Need::Hands, 0, 0, 0};
				break;
			case Gesture::MeshHandoff:
				d.id = "meshHandoff";
				d.kind = "card";
				d.primitives = (base & ~Prim::R9Port) | Prim::R2Frame | Prim::R3Zone | Prim::R10Feedback;
				d.needs = {Need::Controllers | Need::Hands, Need::Haptics, 0, 0};
				d.layers = 1;
				break;
		}
		return d;
	}

	void PlanRunner::Load(const PressPlan& plan, float tx_per_second)
	{
		bool incomplete = false;
		m_count = ToTransactions(plan, tx_per_second, &m_steps, &incomplete);
		m_end = 0;
		for (size_t i = 0; i < m_count; i++)
			m_end = std::max(m_end, m_steps[i].start + m_steps[i].count);
		m_tx = 0;
		m_withhold = plan.withhold_mask;
		m_neutral_stick = plan.neutral_left_stick;
		m_running = m_end > 0;
	}

	void PlanRunner::Cancel() { m_running = false; }

	void PlanRunner::Step(PadXR::EdgeScheduler* sched, PadXR::WordCell* word, PadXR::StickCell* lx, PadXR::StickCell* ly)
	{
		if (!m_running)
			return;
		for (size_t i = 0; i < m_count; i++)
			if (m_steps[i].start == m_tx && sched)
				sched->Press(m_steps[i].bit, m_steps[i].count);
		if (word && m_withhold)
		{
			word->own_mask |= m_withhold;
			word->own_down &= static_cast<u16>(~m_withhold);
		}
		if (m_neutral_stick)
		{
			if (lx)
				lx->Write(PadXR::Claim::Own, 0.0f);
			if (ly)
				ly->Write(PadXR::Claim::Own, 0.0f);
		}
		if (++m_tx >= m_end)
			m_running = false;
	}
}
#endif
