// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "VR/DisplayFit.h"
#include "VR/FrameStamp.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace VR::WorldProjection
{
	struct Quat
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
	};

	inline Quat QuatMul(const Quat& a, const Quat& b)
	{
		return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
	}

	inline Quat QuatAboutX(float rad) { return Quat{std::sin(rad * 0.5f), 0.0f, 0.0f, std::cos(rad * 0.5f)}; }
	inline Quat QuatAboutY(float rad) { return Quat{0.0f, std::sin(rad * 0.5f), 0.0f, std::cos(rad * 0.5f)}; }
	inline Quat QuatAboutZ(float rad) { return Quat{0.0f, 0.0f, std::sin(rad * 0.5f), std::cos(rad * 0.5f)}; }
	inline Quat QuatConj(const Quat& q) { return Quat{-q.x, -q.y, -q.z, q.w}; }

	inline float QuatAngleDeg(const Quat& a, const Quat& b)
	{
		const Quat d = QuatMul(QuatConj(a), b);
		const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		return 2.0f * std::atan2(n, std::fabs(d.w)) * (180.0f / 3.14159265358979f);
	}

	struct Mat3
	{
		float m[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
	};

	inline Mat3 Mat3FromGuestRows(const float raw16[16])
	{
		Mat3 r;
		for (int row = 0; row < 3; row++)
			for (int col = 0; col < 3; col++)
#if defined(VR_WORLDPROJ_PLANTED_TRANSPOSE)
				r.m[row * 3 + col] = raw16[col * 4 + row];
#else
				r.m[row * 3 + col] = raw16[row * 4 + col];
#endif
		return r;
	}

	inline bool IsRotation(const Mat3& a)
	{
		for (float v : a.m)
			if (!std::isfinite(v))
				return false;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
			{
				const float d = a.m[i * 3] * a.m[j * 3] + a.m[i * 3 + 1] * a.m[j * 3 + 1] + a.m[i * 3 + 2] * a.m[j * 3 + 2];
				if (std::fabs(d - (i == j ? 1.0f : 0.0f)) > 5e-3f)
					return false;
			}
		const float det = a.m[0] * (a.m[4] * a.m[8] - a.m[5] * a.m[7]) - a.m[1] * (a.m[3] * a.m[8] - a.m[5] * a.m[6]) +
			a.m[2] * (a.m[3] * a.m[7] - a.m[4] * a.m[6]);
		return std::fabs(det - 1.0f) <= 5e-3f;
	}

	inline Quat QuatFromMat3(const Mat3& a)
	{
		const float* m = a.m;
		const float tr = m[0] + m[4] + m[8];
		Quat q;
		if (tr > 0.0f)
		{
			const float s = std::sqrt(tr + 1.0f) * 2.0f;
			q = Quat{(m[7] - m[5]) / s, (m[2] - m[6]) / s, (m[3] - m[1]) / s, 0.25f * s};
		}
		else if (m[0] > m[4] && m[0] > m[8])
		{
			const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
			q = Quat{0.25f * s, (m[1] + m[3]) / s, (m[2] + m[6]) / s, (m[7] - m[5]) / s};
		}
		else if (m[4] > m[8])
		{
			const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
			q = Quat{(m[1] + m[3]) / s, 0.25f * s, (m[5] + m[7]) / s, (m[2] - m[6]) / s};
		}
		else
		{
			const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
			q = Quat{(m[2] + m[6]) / s, (m[5] + m[7]) / s, 0.25f * s, (m[3] - m[1]) / s};
		}
		const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		return Quat{q.x / n, q.y / n, q.z / n, q.w / n};
	}

	inline Mat3 Mat3Mul(const Mat3& a, const Mat3& b)
	{
		Mat3 r;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
		return r;
	}

	inline Mat3 Mat3Transpose(const Mat3& a)
	{
		Mat3 r;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				r.m[i * 3 + j] = a.m[j * 3 + i];
		return r;
	}

	inline Mat3 Mat3FromQuat(const Quat& q)
	{
		const float x = q.x, y = q.y, z = q.z, w = q.w;
		Mat3 r;
		r.m[0] = 1 - 2 * (y * y + z * z), r.m[1] = 2 * (x * y - z * w), r.m[2] = 2 * (x * z + y * w);
		r.m[3] = 2 * (x * y + z * w), r.m[4] = 1 - 2 * (x * x + z * z), r.m[5] = 2 * (y * z - x * w);
		r.m[6] = 2 * (x * z - y * w), r.m[7] = 2 * (y * z + x * w), r.m[8] = 1 - 2 * (x * x + y * y);
		return r;
	}

	struct Euler
	{
		float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
	};
	inline Euler EulerFromMat3(const Mat3& a)
	{
		constexpr float kDeg = 180.0f / 3.14159265358979f;
		const float sp = std::fmax(-1.0f, std::fmin(1.0f, -a.m[5]));
		return Euler{std::atan2(a.m[2], a.m[8]) * kDeg, std::asin(sp) * kDeg, std::atan2(a.m[3], a.m[4]) * kDeg};
	}

	inline Quat QuatFromEuler(const Euler& e)
	{
		constexpr float kRad = 3.14159265358979f / 180.0f;
		return QuatMul(QuatMul(QuatAboutY(e.yaw * kRad), QuatAboutX(e.pitch * kRad)), QuatAboutZ(e.roll * kRad));
	}

	enum class FrustumModel : std::uint8_t
	{
		Degrees = 0,
		Constant = 1,
	};

	struct ViewModel
	{
		float yaw_sign = 1.0f;
		float pitch_sign = 1.0f;
		float tan_x = 0.0f;
		float tan_y = 0.0f;
		FrustumModel model = FrustumModel::Degrees;
	};

	struct DeclaredView
	{
		Quat orientation;
		float tan_left = 0.0f, tan_right = 0.0f, tan_up = 0.0f, tan_down = 0.0f;
	};

	inline bool Frustum(float fov_word_deg, const ViewModel& m, DeclaredView* out)
	{
		constexpr float kRad = 3.14159265358979f / 180.0f;
		if (!(m.tan_x > 0.0f) || !(m.tan_y > 0.0f))
			return false;
		float t = 1.0f;
		if (m.model == FrustumModel::Degrees
#if defined(VR_WORLDPROJ_PLANTED_CONSTWORD)
			|| true
#endif
		)
		{
			if (!std::isfinite(fov_word_deg) || fov_word_deg <= 1.0f || fov_word_deg >= 179.0f)
				return false;
			t = std::tan(fov_word_deg * kRad * 0.5f);
		}
		out->tan_right = m.tan_x * t;
		out->tan_left = -out->tan_right;
		out->tan_up = m.tan_y * t;
		out->tan_down = -out->tan_up;
#if defined(VR_WORLDPROJ_PLANTED_UPDOWN)
		std::swap(out->tan_up, out->tan_down);
#endif
		return true;
	}

	inline bool Declare(float yaw_deg, float pitch_deg, float yaw_rest_deg, float pitch_rest_deg, float fov_word_deg,
		const ViewModel& m, const Quat& reference, DeclaredView* out)
	{
		constexpr float kRad = 3.14159265358979f / 180.0f;
		if (!std::isfinite(yaw_deg) || !std::isfinite(pitch_deg) || !std::isfinite(yaw_rest_deg) ||
			!std::isfinite(pitch_rest_deg))
			return false;
		if (!Frustum(fov_word_deg, m, out))
			return false;
		const float y = m.yaw_sign * yaw_deg * kRad, y0 = m.yaw_sign * yaw_rest_deg * kRad;
		const float p = m.pitch_sign * pitch_deg * kRad, p0 = m.pitch_sign * pitch_rest_deg * kRad;
#if defined(VR_WORLDPROJ_PLANTED_PITCHFIRST)
		const Quat rel = QuatMul(QuatAboutX(p - p0), QuatAboutY(y - y0));
#else
		const Quat rel = QuatMul(QuatMul(QuatAboutX(-p0), QuatAboutY(y - y0)), QuatAboutX(p));
#endif
#if defined(VR_WORLDPROJ_PLANTED_NOREF)
		(void)reference;
		out->orientation = rel;
#else
		out->orientation = QuatMul(reference, rel);
#endif
#if defined(VR_WORLDPROJ_PLANTED_CONJUGATE)
		out->orientation = Quat{-out->orientation.x, -out->orientation.y, -out->orientation.z, out->orientation.w};
#endif
		return true;
	}

	inline constexpr float kRollOffToleranceDeg = 0.01f;

	inline bool DeclareRotation(const Mat3& drawn, bool roll_declared, float fov_word_deg, const ViewModel& m,
		const Quat& reference, DeclaredView* out, Euler* euler = nullptr)
	{
		if (!IsRotation(drawn))
			return false;
		const Euler e = EulerFromMat3(drawn);
		if (euler)
			*euler = e;
#if !defined(VR_WORLDPROJ_PLANTED_ROLLLEAK)
		if (!roll_declared && std::fabs(e.roll) > kRollOffToleranceDeg)
			return false;
#else
		(void)roll_declared;
#endif
		if (!Frustum(fov_word_deg, m, out))
			return false;
		out->orientation = QuatMul(reference, QuatFromMat3(drawn));
		return true;
	}

	inline constexpr std::uint32_t kNoFbp = 0xFFFFFFFFu;

	inline std::uint32_t ShownFbp(bool en1, bool en2, std::uint32_t fbp1, std::uint32_t fbp2)
	{
		if (en1 && en2)
		{
#if !defined(VR_WORLDPROJ_PLANTED_ONECIRCUIT)
			if (fbp1 == fbp2)
				return fbp1;
#endif
			return kNoFbp;
		}
		if (!en1 && !en2)
			return kNoFbp;
		return en2 ? fbp2 : fbp1;
	}

	struct Basis
	{
		std::int8_t axis[3] = {1, 2, 3};
	};

	inline bool BasisValid(const Basis& b)
	{
		int seen = 0;
		int perm[3];
		int sign = 1;
		for (int i = 0; i < 3; i++)
		{
			const int a = b.axis[i] < 0 ? -b.axis[i] : b.axis[i];
			if (a < 1 || a > 3 || (seen & (1 << a)))
				return false;
			seen |= 1 << a;
			perm[i] = a - 1;
			sign *= (b.axis[i] < 0) ? -1 : 1;
		}
		int inv = 0;
		for (int i = 0; i < 3; i++)
			for (int j = i + 1; j < 3; j++)
				inv += perm[i] > perm[j] ? 1 : 0;
		return sign * ((inv % 2) ? -1 : 1) == 1;
	}

	inline Mat3 CameraFromRows(const float rows[9], const Basis& b)
	{
		Mat3 r;
		for (int i = 0; i < 3; i++)
		{
#if defined(VR_WORLDPROJ_PLANTED_BASIS)
			const int row = i;
			const float s = 1.0f;
			(void)b;
#else
			const int row = (b.axis[i] < 0 ? -b.axis[i] : b.axis[i]) - 1;
			const float s = b.axis[i] < 0 ? -1.0f : 1.0f;
#endif
			for (int c = 0; c < 3; c++)
				r.m[c * 3 + i] = s * rows[row * 3 + c];
		}
		return r;
	}

	enum class HookBody : std::uint8_t
	{
		Pre = 0,
		Rest = 1,
	};

	inline constexpr float kRestAxisToleranceDeg = 0.25f;
	inline constexpr float kRestMaxRollDeg = 15.0f;

	inline bool HookRestConsistent(const Mat3& k)
	{
#if defined(VR_WORLDPROJ_PLANTED_NOKCHECK)
		(void)k;
		return true;
#else
		constexpr float kRad = 3.14159265358979f / 180.0f;
		if (!IsRotation(k) || !(k.m[8] >= std::cos(kRestAxisToleranceDeg * kRad)))
			return false;
		return std::fabs(std::atan2(k.m[3], k.m[0])) <= kRestMaxRollDeg * kRad;
#endif
	}

	inline bool HookRotation(const Mat3& pre, const Mat3& drawn, const Mat3* lever, HookBody body, Mat3* out)
	{
		if (!IsRotation(pre) || !IsRotation(drawn))
			return false;
		const Mat3 a = Mat3Mul(Mat3Transpose(pre), drawn);
		if (body == HookBody::Pre)
		{
			*out = a;
			return true;
		}
		if (!lever || !IsRotation(*lever))
			return false;
		if (!HookRestConsistent(Mat3Mul(Mat3Transpose(*lever), a)))
			return false;
#if defined(VR_WORLDPROJ_PLANTED_RESTSIDE)
		*out = Mat3Mul(Mat3Mul(a, *lever), Mat3Transpose(a));
#else
		*out = Mat3Mul(Mat3Mul(Mat3Transpose(a), *lever), a);
#endif
		return true;
	}

	struct Visit
	{
		std::uint64_t seq = 0;
		std::uint32_t base = 0;
		float pre[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		float lever[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		bool has_lever = false;
	};

	struct Pose
	{
		float yaw_deg = 0.0f;
		float pitch_deg = 0.0f;
		float fov_word = std::numeric_limits<float>::quiet_NaN();
		bool declarable = false;
		std::uint32_t ee_vsync = 0;
		DeclaredView view{};
		float position[3] = {0.0f, 0.0f, 0.0f};
		bool has_view = false;
		float roll_deg = 0.0f;
		bool matrix = false;
		Quat lever{};
		Quat head{};
		bool has_head = false;
		float yaw_rest = 0.0f, pitch_rest = 0.0f;
		bool yaw_anchored = false, pitch_anchored = false;
		std::uint32_t visit_vsync = 0;

		bool operator==(const Pose& o) const
		{
			return yaw_deg == o.yaw_deg && pitch_deg == o.pitch_deg && roll_deg == o.roll_deg &&
			       declarable == o.declarable &&
			       (fov_word == o.fov_word || (std::isnan(fov_word) && std::isnan(o.fov_word)));
		}
	};

	struct Presented
	{
		PresentDecision<Pose> decision{};
		std::uint32_t display_fbp = 0;
		std::uint32_t vsync_age = 0;
		bool declares = false;
		float far_shift_ndc = 0.0f;
	};

	struct Timing
	{
		static constexpr int kMaxAges = 4;
		std::uint32_t frame_ages[kMaxAges] = {2, 0, 0, 0};
		bool latch_first_draw = false;
		std::uint32_t read_to_draw = 0;
		bool latch_closing = false;

		std::uint32_t LargestAge() const
		{
			std::uint32_t m = 0;
			for (std::uint32_t a : frame_ages)
				m = a > m ? a : m;
			return m;
		}
		bool AgeDeclared(std::uint32_t vsyncs) const
		{
			for (std::uint32_t a : frame_ages)
				if (a != 0 && a == vsyncs)
					return true;
			return false;
		}
		static Timing OneAge(std::uint32_t age)
		{
			Timing t;
			t.frame_ages[0] = age;
			return t;
		}
	};

	class Stamps
	{
	public:
		static constexpr std::uint32_t kNoBuffer = kNoFbp;

		explicit Stamps(std::uint64_t max_age = FrameStampMachine<Pose>::kDefaultMaxAge, std::uint32_t frame_age = 2)
			: m_machine(max_age)
			, m_timing(Timing::OneAge(frame_age))
		{
		}

		void SetFrameAge(std::uint32_t frame_age) { SetTiming(Timing::OneAge(frame_age)); }

		void SetTiming(const Timing& t)
		{
			m_timing = t;
#if !defined(VR_WORLDPROJ_PLANTED_MAXAGE3)
			const std::uint64_t largest = t.LargestAge();
			m_machine.SetMaxAge(largest > FrameStampMachine<Pose>::kDefaultMaxAge ? largest :
			                                                                        FrameStampMachine<Pose>::kDefaultMaxAge);
#endif
		}
		const Timing& GetTiming() const { return m_timing; }

		void OnDraw(std::uint32_t frame_fbp, std::uint32_t display_fbp)
		{
			if (m_timing.latch_closing)
			{
				ClosingDraw(frame_fbp, display_fbp);
				return;
			}
#ifndef VR_WORLDPROJ_PLANTED_ANYDRAW
			if (display_fbp == kNoBuffer || frame_fbp != display_fbp)
				return;
#else
			(void)frame_fbp;
			(void)display_fbp;
#endif
#if !defined(VR_WORLDPROJ_PLANTED_STALEFAR)
			if (!m_drew_display)
				m_interval_far = far_shift ? far_shift() : 0.0f;
#endif
			m_drew_display = true;
			if (m_timing.latch_first_draw && m_have_mark && !m_machine.IsLatchOpen())
				LatchFromRing();
		}

		std::function<float()> far_shift;

		void OnMark(const Pose& pose, std::uint32_t display_fbp)
		{
			if (m_timing.latch_closing)
			{
				ClosingMark(pose, display_fbp);
				return;
			}
			if (m_drew_display && display_fbp != kNoBuffer)
			{
				if (m_machine.IsLatchOpen())
				{
					m_far[m_far_next % kRing] = {m_machine.CurrentLatch().latch_seq, m_interval_far};
					m_far_next++;
				}
				m_machine.OnClose(display_fbp);
			}
			m_drew_display = false;
#if defined(VR_WORLDPROJ_PLANTED_STALEFAR)
			m_interval_far = far_shift ? far_shift() : 0.0f;
#endif
			m_last_mark = pose.ee_vsync;
			m_have_mark = true;
			if (!m_timing.latch_first_draw)
			{
				m_machine.OnLatch(pose, 0, false);
				return;
			}
#if !defined(VR_WORLDPROJ_PLANTED_KEEPLATCH)
			m_machine.DropLatch();
#endif
			m_ring[m_ring_next % kRing] = pose;
			m_ring_used[m_ring_next % kRing] = true;
			m_ring_next++;
		}

		Presented OnPresent(std::uint32_t display_fbp)
		{
			Presented out;
			out.display_fbp = display_fbp;
			if (display_fbp == kNoBuffer)
				return out;
			if (m_timing.latch_closing)
				RememberDisplay(display_fbp);
			out.decision = m_machine.OnPresent(display_fbp);
			if (out.decision.source != StampSource::None && m_have_mark)
				out.vsync_age = m_last_mark - out.decision.stamp.pose.ee_vsync;
#ifndef VR_WORLDPROJ_PLANTED_ANYAGE
			const bool on_time = m_have_mark && out.decision.source != StampSource::None &&
				m_timing.AgeDeclared(out.vsync_age);
#else
			const bool on_time = true;
#endif
#ifndef VR_WORLDPROJ_PLANTED_UNGATED
			out.declares = out.decision.Declares() && out.decision.stamp.pose.declarable && on_time;
#else
			out.declares = out.decision.Declares() && on_time;
#endif
			if (out.decision.source != StampSource::None)
				for (const FarRecord& f : m_far)
					if (f.latch_seq != 0 && f.latch_seq == out.decision.stamp.latch_seq)
						out.far_shift_ndc = f.far;
			return out;
		}

		void Invalidate(XrFrameOnInvalidate xr = XrFrameOnInvalidate::Keep)
		{
			m_machine.Invalidate(xr);
			m_drew_display = false;
			ClearRing();
			ClearClosing();
		}

		void SetEnabled(bool on)
		{
			m_machine.SetEnabled(on);
			m_drew_display = false;
			ClearRing();
			ClearClosing();
		}
		bool IsEnabled() const { return m_machine.IsEnabled(); }

		const FrameStampMachine<Pose>& Machine() const { return m_machine; }

	private:
		static constexpr std::uint32_t kRing = 16;

		void ClearRing()
		{
			for (bool& u : m_ring_used)
				u = false;
		}

		void LatchFromRing()
		{
#if defined(VR_WORLDPROJ_PLANTED_NOREADDELAY)
			const std::uint32_t delay = 0;
#else
			const std::uint32_t delay = m_timing.read_to_draw;
#endif
			Pose pose;
			pose.declarable = false;
			pose.ee_vsync = m_last_mark >= delay ? m_last_mark - delay : 0;
			if (m_last_mark >= delay)
			{
				for (std::uint32_t i = 0; i < kRing; i++)
				{
					if (m_ring_used[i] && m_ring[i].ee_vsync == m_last_mark - delay)
					{
						pose = m_ring[i];
						break;
					}
				}
			}
			m_machine.OnLatch(pose, 0, false);
		}

		static constexpr int kKnown = 4;
		static constexpr int kCounted = 8;
		static constexpr std::uint32_t kClosingMinDraws = 2;

		void ClearClosing()
		{
			m_counted_n = 0;
			m_prev_main = kNoBuffer;
			m_have_prev_pose = false;
			m_pending_main = kNoBuffer;
		}

		void RememberDisplay(std::uint32_t fbp)
		{
			if (fbp == kNoBuffer)
				return;
			for (int i = 0; i < m_known_n; i++)
				if (m_known[i] == fbp)
					return;
			if (m_known_n < kKnown)
				m_known[m_known_n++] = fbp;
			else
				m_known[(m_known_next++) % kKnown] = fbp;
		}
		bool IsDisplay(std::uint32_t fbp) const
		{
			for (int i = 0; i < m_known_n; i++)
				if (m_known[i] == fbp)
					return true;
			return false;
		}

		void ClosingDraw(std::uint32_t frame_fbp, std::uint32_t display_fbp)
		{
			RememberDisplay(display_fbp);
#if defined(VR_WORLDPROJ_PLANTED_SHOWNCLOSE)
			if (display_fbp == kNoBuffer || frame_fbp != display_fbp)
				return;
#endif
			for (int i = 0; i < m_counted_n; i++)
			{
				if (m_counted[i].fbp == frame_fbp)
				{
					m_counted[i].draws++;
					return;
				}
			}
			if (m_counted_n < kCounted)
				m_counted[m_counted_n++] = DrawCount{frame_fbp, 1};
		}

		std::uint32_t IntervalMain() const
		{
			std::uint32_t best = kNoBuffer, best_n = 0, second_n = 0;
			for (int i = 0; i < m_counted_n; i++)
			{
				if (!IsDisplay(m_counted[i].fbp))
					continue;
				if (m_counted[i].draws > best_n)
				{
					second_n = best_n;
					best_n = m_counted[i].draws;
					best = m_counted[i].fbp;
				}
				else if (m_counted[i].draws > second_n)
					second_n = m_counted[i].draws;
			}
			return (best_n >= kClosingMinDraws && best_n > second_n) ? best : kNoBuffer;
		}

		void LatchAndClose(const Pose& pose, std::uint32_t fbp)
		{
			m_machine.DropLatch();
			m_machine.OnLatch(pose, 0, false);
			m_machine.OnClose(fbp);
		}

		void ClosingMark(const Pose& pose, std::uint32_t display_fbp)
		{
			RememberDisplay(display_fbp);
			m_last_mark = pose.ee_vsync;
			m_have_mark = true;
			const std::uint32_t main = IntervalMain();
			m_counted_n = 0;
#if defined(VR_WORLDPROJ_PLANTED_CLOSINGLATE)
			if (m_pending_main != kNoBuffer)
				LatchAndClose(pose, m_pending_main);
			m_pending_main = kNoBuffer;
#endif
			if (main != kNoBuffer && main != m_prev_main)
			{
#if defined(VR_WORLDPROJ_PLANTED_CLOSINGEARLY)
				Pose early = m_prev_pose;
				if (!m_have_prev_pose)
					early.declarable = false;
				LatchAndClose(early, main);
#elif defined(VR_WORLDPROJ_PLANTED_CLOSINGLATE)
				m_pending_main = main;
#else
				LatchAndClose(pose, main);
#endif
			}
#if defined(VR_WORLDPROJ_PLANTED_NOSPILL)
			else if (main != kNoBuffer)
				LatchAndClose(pose, main);
#endif
			m_prev_main = main;
			m_prev_pose = pose;
			m_have_prev_pose = true;
		}

		struct DrawCount
		{
			std::uint32_t fbp = 0;
			std::uint32_t draws = 0;
		};
		std::uint32_t m_known[kKnown]{};
		int m_known_n = 0;
		int m_known_next = 0;
		DrawCount m_counted[kCounted]{};
		int m_counted_n = 0;
		std::uint32_t m_prev_main = kNoBuffer;
		Pose m_prev_pose{};
		bool m_have_prev_pose = false;
		std::uint32_t m_pending_main = kNoBuffer;

		FrameStampMachine<Pose> m_machine;
		Timing m_timing{};
		std::uint32_t m_last_mark = 0;
		bool m_have_mark = false;
		bool m_drew_display = false;
		Pose m_ring[kRing]{};
		bool m_ring_used[kRing]{};
		std::uint32_t m_ring_next = 0;
		struct FarRecord
		{
			std::uint64_t latch_seq = 0;
			float far = 0.0f;
		};
		float m_interval_far = 0.0f;
		FarRecord m_far[kRing]{};
		std::uint32_t m_far_next = 0;
	};

	struct LayerInputs
	{
		bool present_declares = false;
		bool split = false;
		bool projector = false;
		bool chain_released = false;
		bool copied_this_visit = false;
		float display_vfov_deg = 0.0f;
		DisplayFit::Fit fit{};
		float far_shift_ndc = 0.0f;
		bool layered = false;
		bool right_copied_this_visit = false;
		bool stereo_killed = false;
	};

	inline bool DisplayTakesProjection(float display_vfov_deg, const DisplayFit::Fit& fit)
	{
#if defined(VR_WORLDPROJ_PLANTED_IDENTITYGUARD)
		(void)display_vfov_deg;
		return fit.IsIdentity();
#else
		(void)fit;
		return std::isfinite(display_vfov_deg) && display_vfov_deg >= DisplayFit::kTightVerticalFovDeg;
#endif
	}

	inline bool LayerDeclares(const LayerInputs& in)
	{
		if (!in.present_declares || in.split || in.projector || !in.chain_released)
			return false;
#if !defined(VR_WORLDPROJ_PLANTED_STALECOPY)
		if (!in.copied_this_visit)
			return false;
#endif
		return DisplayTakesProjection(in.display_vfov_deg, in.fit);
	}

	inline int LayerEyes(const LayerInputs& in)
	{
		const float s = in.far_shift_ndc;
		return (in.layered && !in.stereo_killed && std::isfinite(s) && s > 0.0f && s < 1.0f) ? 2 : 1;
	}

	inline bool LayerDeclaresStereo(const LayerInputs& in)
	{
		if (!LayerDeclares(in))
			return false;
#if !defined(VR_WORLDPROJ_PLANTED_ONEEYE)
		if (LayerEyes(in) == 2 && !in.right_copied_this_visit)
			return false;
#endif
		return true;
	}

	inline std::uint32_t EyeChain(std::uint32_t view)
	{
#if defined(VR_WORLDPROJ_PLANTED_EYESWAP)
		return 1u - view;
#else
		return view;
#endif
	}

	struct EyeView
	{
		DeclaredView view{};
		float position[3] = {0.0f, 0.0f, 0.0f};
	};

	inline bool DeclareEyes(const DeclaredView& mono, const float head_position[3], float far_shift_ndc,
		const float eye_offset[2][3], EyeView out[2])
	{
		if (!std::isfinite(far_shift_ndc) || far_shift_ndc < 0.0f || far_shift_ndc >= 1.0f)
			return false;
		const float half = 0.5f * (mono.tan_right - mono.tan_left);
		const Mat3 o = Mat3FromQuat(mono.orientation);
		for (int e = 0; e < 2; e++)
		{
#if defined(VR_WORLDPROJ_PLANTED_SHIFTSIGN)
			const float nose = (e == 0) ? -1.0f : 1.0f;
#else
			const float nose = (e == 0) ? 1.0f : -1.0f;
#endif
#if defined(VR_WORLDPROJ_PLANTED_SAMEFOV)
			const float shift = 0.0f * nose * far_shift_ndc * half;
#else
			const float shift = nose * far_shift_ndc * half;
#endif
			out[e].view = mono;
			out[e].view.tan_left = mono.tan_left + shift;
			out[e].view.tan_right = mono.tan_right + shift;
			for (int i = 0; i < 3; i++)
			{
				const float off = o.m[i * 3 + 0] * eye_offset[e][0] + o.m[i * 3 + 1] * eye_offset[e][1] +
					o.m[i * 3 + 2] * eye_offset[e][2];
				if (!std::isfinite(off))
					return false;
				out[e].position[i] = head_position[i] + off;
			}
		}
		return true;
	}

	inline float ConvergenceDistanceM(float ipd_m, float far_shift_ndc, float half_tan)
	{
		const float k = 2.0f * far_shift_ndc * half_tan;
		return (k > 0.0f && std::isfinite(k) && std::isfinite(ipd_m)) ? ipd_m / k : 0.0f;
	}

	struct PatternBar
	{
		std::int32_t x0 = 0, x1 = 0, y0 = 0, y1 = 0;
		float ndc_x = 0.0f;
		float d = 0.0f;
	};
	inline constexpr int kPatternBars = 3;

	inline bool StereoPatternBars(std::uint32_t layer, float far_shift_ndc, std::uint32_t w, std::uint32_t h,
		PatternBar out[kPatternBars])
	{
		if (w < 16 || h < 16 || !std::isfinite(far_shift_ndc) || far_shift_ndc < 0.0f || far_shift_ndc >= 1.0f)
			return false;
#if defined(VR_WORLDPROJ_PLANTED_PATTERNSIGN)
		const float sign = (layer == 0) ? 1.0f : -1.0f;
#else
		const float sign = (layer == 0) ? -1.0f : 1.0f;
#endif
		const float ds[kPatternBars] = {far_shift_ndc, 0.5f * far_shift_ndc, 0.0f};
		const std::int32_t bw = static_cast<std::int32_t>(w / 160 > 2 ? w / 160 : 2);
		const std::int32_t bh = static_cast<std::int32_t>(h / 6);
		for (int k = 0; k < kPatternBars; k++)
		{
			PatternBar& b = out[k];
			b.d = ds[k];
			b.ndc_x = sign * ds[k];
			const float cx = (b.ndc_x + 1.0f) * 0.5f * static_cast<float>(w);
			b.x0 = static_cast<std::int32_t>(std::lround(cx - 0.5f * static_cast<float>(bw)));
			b.x1 = b.x0 + bw;
			const std::int32_t cy = static_cast<std::int32_t>(h) * (k + 1) / 4;
			b.y0 = cy - bh / 2;
			b.y1 = b.y0 + bh;
		}
		return true;
	}

	inline constexpr float kHeadCheckToleranceDeg = 0.01f;

	inline Quat HeadOnAxes(const Quat& head_rel, bool pitch, bool roll)
	{
		Euler e = EulerFromMat3(Mat3FromQuat(head_rel));
#if defined(VR_WORLDPROJ_PLANTED_HEADSIGN)
		e.yaw = -e.yaw;
#endif
		if (!pitch)
			e.pitch = 0.0f;
		if (!roll)
			e.roll = 0.0f;
		return QuatFromEuler(e);
	}

	inline float HeadResidualDeg(const Pose& p)
	{
		return p.has_head ? QuatAngleDeg(p.lever, p.head) : -1.0f;
	}

	struct HeadTally
	{
		std::uint32_t checked = 0;
		std::uint32_t within = 0;
		float worst = 0.0f;
		void Add(const Pose& p)
		{
			const float r = HeadResidualDeg(p);
			if (r < 0.0f)
				return;
			checked++;
			if (r <= kHeadCheckToleranceDeg)
				within++;
			worst = std::fmax(worst, r);
		}
		void Reset() { *this = HeadTally{}; }
	};

	struct Source
	{
		enum class Drawn : std::uint8_t
		{
			Copy = 0,
			Matrix = 1,
			Hook = 2,
			LookAt = 3,
		};
		enum class Body : std::uint8_t
		{
			Anchored = 0,
			Identity = 1,
			Latched = 2,
			MinusOwn = 3,
		};
		Drawn drawn = Drawn::Copy;
		std::uint32_t yaw_address = 0;
		bool has_pitch = false;
		std::uint32_t pitch_address = 0;
		bool has_fov_readback = false;
		std::uint32_t fov_readback = 0;
		Body yaw_body = Body::Anchored;
		std::uint32_t yaw_op = 0;
		Body pitch_body = Body::Latched;
		std::uint32_t pitch_op = 0;
		std::uint32_t matrix_address = 0;
		std::uint32_t look_eye_offset = 0;
		std::uint32_t look_target_offset = 0;
		bool roll_declared = false;
		bool has_view = false;
		ViewModel model{};
		std::uint32_t frame_age = 0;
		std::uint32_t write_to_read = 1;
		std::uint32_t frame_ages[Timing::kMaxAges] = {0, 0, 0, 0};
		bool latch_first_draw = false;
		std::uint32_t read_to_draw = 0;
		bool latch_closing = false;
		std::uint32_t hook_offset = 0;
		Basis basis{};
		HookBody hook_body = HookBody::Pre;
		std::uint32_t visit_max_age = 4;

		Timing StampTiming() const
		{
			Timing t;
			if (frame_ages[0] != 0)
				for (int i = 0; i < Timing::kMaxAges; i++)
					t.frame_ages[i] = frame_ages[i];
			else
				t = Timing::OneAge(frame_age);
			t.latch_first_draw = latch_first_draw;
			t.read_to_draw = read_to_draw;
			t.latch_closing = latch_closing;
			return t;
		}
	};

	class EeSources
	{
	public:
		virtual ~EeSources() = default;
		virtual std::optional<float> F32(std::uint32_t address) = 0;
		virtual bool GatesPass() = 0;
		virtual bool AnchoredBase(std::uint32_t op, float* base) = 0;
		virtual bool Reference(Quat* orientation, float position[3]) = 0;
		virtual bool Head(Quat* head_rel) = 0;
		virtual bool LastVisit(Visit* visit)
		{
			(void)visit;
			return false;
		}
		virtual bool Base(std::uint32_t* base)
		{
			(void)base;
			return false;
		}
		virtual bool DeltaTotal(std::uint32_t op, float* total)
		{
			(void)op;
			(void)total;
			return false;
		}
	};

	struct OwnTotals
	{
		bool has_yaw = false;
		bool has_pitch = false;
		float yaw = 0.0f;
		float pitch = 0.0f;
	};

	inline bool LookAtAngles(const float eye[3], const float target[3], float* yaw_deg, float* pitch_deg)
	{
		constexpr float kDeg = 180.0f / 3.14159265358979f;
		const float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
		const float h = std::sqrt(dx * dx + dz * dz);
		if (!std::isfinite(h) || !std::isfinite(dy) || (h == 0.0f && dy == 0.0f))
			return false;
#if defined(VR_WORLDPROJ_PLANTED_LOOKATSWAP)
		*yaw_deg = std::atan2(dz, dx) * kDeg;
#else
		*yaw_deg = std::atan2(dx, dz) * kDeg;
#endif
		*pitch_deg = std::atan2(dy, h) * kDeg;
		return true;
	}

	struct HookRead
	{
		bool has_visit = false;
		Visit visit{};
		std::uint32_t visit_vsync = 0;
		bool before_recenter = false;
	};

	inline Pose ReadHookPose(const Source& src, EeSources& in, std::uint32_t seq, const HookRead& hr, const Quat* head)
	{
		Pose pose;
		pose.ee_vsync = seq;
		pose.matrix = true;
		if (!hr.has_visit)
			return pose;
		pose.visit_vsync = hr.visit_vsync;
		float rows[9];
		bool ok = hr.visit.base != 0;
		for (int r = 0; r < 3 && ok; r++)
			for (int c = 0; c < 3 && ok; c++)
			{
				const std::optional<float> v =
					in.F32(hr.visit.base + src.hook_offset + static_cast<std::uint32_t>((r * 4 + c) * 4));
				ok = v.has_value();
				rows[r * 3 + c] = v.value_or(0.0f);
			}
		if (!ok)
			return pose;
		const Mat3 pre = CameraFromRows(hr.visit.pre, src.basis);
		const Mat3 drawn = CameraFromRows(rows, src.basis);
		Mat3 lever;
		for (int i = 0; i < 9; i++)
			lever.m[i] = hr.visit.lever[i];
		Mat3 rel;
		if (!HookRotation(pre, drawn, hr.visit.has_lever ? &lever : nullptr, src.hook_body, &rel))
			return pose;
		const bool fresh = seq >= hr.visit_vsync && seq - hr.visit_vsync <= src.visit_max_age;
#if defined(VR_WORLDPROJ_PLANTED_HOOKRECENTER)
		const bool after_recenter = true;
#else
		const bool after_recenter = !hr.before_recenter;
#endif
		bool lever_ok = true;
		if (hr.visit.has_lever && !src.roll_declared && IsRotation(lever))
			lever_ok = std::fabs(EulerFromMat3(lever).roll) <= kRollOffToleranceDeg;
		Quat ref_q;
		float ref_p[3] = {0.0f, 0.0f, 0.0f};
		const Euler e = EulerFromMat3(rel);
		pose.yaw_deg = e.yaw;
		pose.pitch_deg = e.pitch;
		pose.roll_deg = e.roll;
		if (src.has_view && in.Reference(&ref_q, ref_p) && Frustum(pose.fov_word, src.model, &pose.view))
		{
			pose.view.orientation = QuatMul(ref_q, QuatFromMat3(rel));
			pose.has_view = true;
			pose.position[0] = ref_p[0];
			pose.position[1] = ref_p[1];
			pose.position[2] = ref_p[2];
		}
		if (head && hr.visit.has_lever && IsRotation(lever))
		{
			pose.lever = QuatFromMat3(lever);
			pose.head = HeadOnAxes(*head, true, src.roll_declared);
			pose.has_head = true;
		}
		pose.declarable = fresh && after_recenter && lever_ok && pose.has_view && in.GatesPass();
		return pose;
	}

	inline Pose ReadPose(const Source& src, EeSources& in, std::uint32_t seq, const Quat* head,
		const OwnTotals* own = nullptr)
	{
		Pose pose;
		pose.ee_vsync = seq;
		if (src.has_fov_readback)
			pose.fov_word = in.F32(src.fov_readback).value_or(std::numeric_limits<float>::quiet_NaN());
		Quat ref_q;
		float ref_p[3] = {0.0f, 0.0f, 0.0f};
		if (src.drawn == Source::Drawn::Matrix)
		{
			float raw[16];
			bool ok = true;
			for (int r = 0; r < 3 && ok; r++)
				for (int c = 0; c < 3 && ok; c++)
				{
					const std::optional<float> v = in.F32(src.matrix_address + static_cast<std::uint32_t>((r * 4 + c) * 4));
					ok = v.has_value();
					raw[r * 4 + c] = v.value_or(0.0f);
				}
			pose.matrix = true;
			pose.declarable = ok && in.GatesPass();
			if (ok)
			{
				const Mat3 drawn = Mat3FromGuestRows(raw);
				Euler e;
				pose.has_view = src.has_view && in.Reference(&ref_q, ref_p) &&
					DeclareRotation(drawn, src.roll_declared, pose.fov_word, src.model, ref_q, &pose.view, &e);
				if (!pose.has_view)
					e = EulerFromMat3(drawn);
				pose.yaw_deg = e.yaw;
				pose.pitch_deg = e.pitch;
				pose.roll_deg = e.roll;
				if (head && IsRotation(drawn))
				{
					pose.lever = QuatFromMat3(drawn);
					pose.head = HeadOnAxes(*head, true, src.roll_declared);
					pose.has_head = true;
				}
			}
		}
		else
		{
			bool read_ok = false;
			if (src.drawn == Source::Drawn::LookAt)
			{
				std::uint32_t base = 0;
				float p[6];
				read_ok = in.Base(&base) && base != 0;
				for (int i = 0; i < 6 && read_ok; i++)
				{
					const std::uint32_t off = (i < 3 ? src.look_eye_offset : src.look_target_offset) + static_cast<std::uint32_t>((i % 3) * 4);
					const std::optional<float> v = in.F32(base + off);
					read_ok = v.has_value();
					p[i] = v.value_or(0.0f);
				}
				read_ok = read_ok && LookAtAngles(p, p + 3, &pose.yaw_deg, &pose.pitch_deg);
				if (!src.has_pitch)
					pose.pitch_deg = 0.0f;
			}
			else
			{
				const std::optional<float> yaw = in.F32(src.yaw_address);
				const std::optional<float> pitch = src.has_pitch ? in.F32(src.pitch_address) : std::optional<float>(0.0f);
				pose.yaw_deg = yaw.value_or(0.0f);
				pose.pitch_deg = pitch.value_or(0.0f);
				read_ok = yaw.has_value() && pitch.has_value();
			}
			pose.declarable = read_ok && in.GatesPass();

			float yaw_rest = 0.0f, pitch_rest = pose.pitch_deg;
			bool rests = src.has_view && read_ok;
			if (rests && src.yaw_body == Source::Body::Anchored)
				pose.yaw_anchored = rests = in.AnchoredBase(src.yaw_op, &yaw_rest);
			else if (rests && src.yaw_body == Source::Body::MinusOwn)
			{
				rests = own && own->has_yaw;
				yaw_rest = rests ? pose.yaw_deg - own->yaw : 0.0f;
			}
			if (rests && src.pitch_body == Source::Body::Anchored)
				pose.pitch_anchored = rests = in.AnchoredBase(src.pitch_op, &pitch_rest);
			else if (src.pitch_body == Source::Body::Identity)
				pitch_rest = 0.0f;
			else if (rests && src.pitch_body == Source::Body::MinusOwn)
			{
				rests = own && own->has_pitch;
				pitch_rest = rests ? pose.pitch_deg - own->pitch : 0.0f;
			}
			pose.yaw_rest = yaw_rest;
			pose.pitch_rest = pitch_rest;
			if (rests && in.Reference(&ref_q, ref_p))
				pose.has_view = Declare(pose.yaw_deg, pose.pitch_deg, yaw_rest, pitch_rest, pose.fov_word, src.model,
					ref_q, &pose.view);
			if (rests && head)
			{
				constexpr float kRad = 3.14159265358979f / 180.0f;
				const float ly = src.model.yaw_sign * (pose.yaw_deg - yaw_rest);
				const float lp = src.has_pitch ? src.model.pitch_sign * (pose.pitch_deg - pitch_rest) : 0.0f;
				pose.lever = QuatMul(QuatAboutY(ly * kRad), QuatAboutX(lp * kRad));
				pose.head = HeadOnAxes(*head, src.has_pitch && src.pitch_body != Source::Body::Latched, false);
				pose.has_head = true;
			}
		}
		if (pose.has_view)
		{
			pose.position[0] = ref_p[0];
			pose.position[1] = ref_p[1];
			pose.position[2] = ref_p[2];
		}
		pose.declarable = pose.declarable && pose.has_view;
		return pose;
	}

	class GsSide
	{
	public:
		void SetActive(bool on, std::uint32_t frame_age) { SetActive(on, Timing::OneAge(frame_age)); }
		void SetActive(bool on, const Timing& timing)
		{
			m_active = on;
			m_stamps.SetTiming(timing);
			m_stamps.SetEnabled(on);
			m_last = Presented{};
		}
		bool Active() const { return m_active; }

		void Invalidate(bool clear_last)
		{
			m_stamps.Invalidate(XrFrameOnInvalidate::Keep);
			if (clear_last)
				m_last = Presented{};
		}

		void RendererSwitch()
		{
#if !defined(VR_WORLDPROJ_PLANTED_NOSWITCH)
			m_stamps.Invalidate(XrFrameOnInvalidate::Drop);
			m_last = Presented{};
#endif
		}

		std::function<std::uint32_t()> display_fbp;

		void SetFarShift(std::function<float()> f) { m_stamps.far_shift = std::move(f); }

		void Mark(const Pose& pose)
		{
			m_stamps.OnMark(pose, display_fbp ? display_fbp() : Stamps::kNoBuffer);
			m_last_mark = pose.ee_vsync;
		}
		void Draw(std::uint32_t frame_fbp, std::uint32_t display_fbp) { m_stamps.OnDraw(frame_fbp, display_fbp); }
		Presented Present(std::uint32_t display_fbp)
		{
			m_last = m_stamps.OnPresent(display_fbp);
			return m_last;
		}
		const Presented& Last() const { return m_last; }
		std::uint32_t LastMark() const { return m_last_mark; }
		const Stamps& StampsRef() const { return m_stamps; }

	private:
		Stamps m_stamps;
		bool m_active = false;
		std::uint32_t m_last_mark = 0;
		Presented m_last{};
	};

	class EeSide
	{
	public:
		using GsWork = std::function<void(GsSide&)>;
		using Post = std::function<void(GsWork)>;

		std::optional<Pose> Vsync(const Source* src, bool wanted, EeSources& in, const Post& post)
		{
			const std::uint32_t seq = m_seq++;
			Quat head;
			const bool has_head = in.Head(&head);
			m_heads[seq % kHeadRing] = head;
			m_head_ok[seq % kHeadRing] = has_head;
			m_head_seq[seq % kHeadRing] = seq;

			const bool want = (src != nullptr) && wanted;
			if (want != m_enabled)
			{
				m_enabled = want;
				m_toggles++;
				const Timing timing = want ? src->StampTiming() : Timing::OneAge(0);
				post([want, timing](GsSide& gs) { gs.SetActive(want, timing); });
			}
			if (!want)
				return std::nullopt;

			Quat ref_q;
			float ref_p[3];
			bool recentered = false;
			if (in.Reference(&ref_q, ref_p) &&
				(ref_q.x != m_ref.x || ref_q.y != m_ref.y || ref_q.z != m_ref.z || ref_q.w != m_ref.w))
			{
				m_ref = ref_q;
				recentered = true;
				m_last_recenter = seq;
				post([](GsSide& gs) { gs.Invalidate(false); });
			}

			const auto head_at = [this](std::uint32_t w) -> const Quat* {
				if (m_head_ok[w % kHeadRing] && m_head_seq[w % kHeadRing] == w)
					return &m_heads[w % kHeadRing];
				return nullptr;
			};
			Pose pose;
			if (src->drawn == Source::Drawn::Hook)
			{
				Visit v;
				if (in.LastVisit(&v) && (!m_have_visit || v.seq != m_visit.seq) && VisitAfterLoad(v.seq))
				{
					m_visit = v;
					m_visit_vsync = seq;
					m_have_visit = true;
				}
				HookRead hr;
				hr.has_visit = m_have_visit;
				hr.visit = m_visit;
				hr.visit_vsync = m_visit_vsync;
				hr.before_recenter = m_visit_vsync == 0 || m_visit_vsync - 1 < m_last_recenter;
				pose = ReadHookPose(*src, in, seq, hr, m_visit_vsync > 0 ? head_at(m_visit_vsync - 1) : nullptr);
			}
			else
			{
				OwnTotals now;
				if (src->yaw_body == Source::Body::MinusOwn)
					now.has_yaw = in.DeltaTotal(src->yaw_op, &now.yaw);
				if (src->has_pitch && src->pitch_body == Source::Body::MinusOwn)
					now.has_pitch = in.DeltaTotal(src->pitch_op, &now.pitch);
				m_own[seq % kHeadRing] = now;
				m_own_seq[seq % kHeadRing] = seq;
				m_own_ok[seq % kHeadRing] = true;
#if defined(VR_WORLDPROJ_PLANTED_OWNNOW)
				const std::uint32_t w = seq;
#else
				const std::uint32_t w = seq - src->write_to_read;
#endif
				const OwnTotals* own = (seq >= src->write_to_read && m_own_ok[w % kHeadRing] && m_own_seq[w % kHeadRing] == w) ?
					&m_own[w % kHeadRing] : nullptr;
				pose = ReadPose(*src, in, seq, seq >= src->write_to_read ? head_at(seq - src->write_to_read) : nullptr, own);
			}
			bool rebased = false;
			if (pose.yaw_anchored)
			{
				rebased = m_have_yaw_rest && pose.yaw_rest != m_yaw_rest;
				m_yaw_rest = pose.yaw_rest;
				m_have_yaw_rest = true;
			}
			if (pose.pitch_anchored)
			{
				rebased = rebased || (m_have_pitch_rest && pose.pitch_rest != m_pitch_rest);
				m_pitch_rest = pose.pitch_rest;
				m_have_pitch_rest = true;
			}
#if defined(VR_WORLDPROJ_PLANTED_NOREBASE)
			rebased = false;
#endif
			if (recentered || rebased)
			{
#if defined(VR_WORLDPROJ_PLANTED_ONEREAD)
				m_refuse_reads = 1;
#else
				m_refuse_reads = src->write_to_read > 1 ? src->write_to_read : 1;
#endif
			}
#if !defined(VR_WORLDPROJ_PLANTED_RECENTERREAD)
			if (m_refuse_reads > 0)
			{
				pose.declarable = false;
				m_refuse_reads--;
			}
#endif
			post([pose](GsSide& gs) { gs.Mark(pose); });
			return pose;
		}

		void Skip() { m_seq++; }

		void StateLoaded(const Post& post)
		{
			if (m_enabled)
				post([](GsSide& gs) { gs.Invalidate(true); });
#if !defined(VR_WORLDPROJ_PLANTED_LOADKEEPSVISIT)
			if (m_have_visit && m_visit.seq > m_load_floor)
				m_load_floor = m_visit.seq;
			m_load_pending = true;
			m_have_visit = false;
			m_visit = Visit{};
			m_visit_vsync = 0;
#endif
			for (bool& ok : m_own_ok)
				ok = false;
		}

		bool Enabled() const { return m_enabled; }
		std::uint32_t Toggles() const { return m_toggles; }

	private:
		static constexpr std::uint32_t kHeadRing = 16;
		std::uint32_t m_seq = 0;
		OwnTotals m_own[kHeadRing]{};
		std::uint32_t m_own_seq[kHeadRing]{};
		bool m_own_ok[kHeadRing]{};
		bool m_enabled = false;
		std::uint32_t m_toggles = 0;
		Quat m_ref{0.0f, 0.0f, 0.0f, 1.0f};
		std::uint32_t m_refuse_reads = 0;
		bool VisitAfterLoad(std::uint64_t seq)
		{
			if (m_load_pending)
			{
				m_load_pending = false;
				if (seq > m_load_floor)
					m_load_floor = seq;
				return false;
			}
			return seq > m_load_floor;
		}

		std::uint32_t m_last_recenter = 0;
		std::uint64_t m_load_floor = 0;
		bool m_load_pending = false;
		Visit m_visit{};
		std::uint32_t m_visit_vsync = 0;
		bool m_have_visit = false;
		float m_yaw_rest = 0.0f, m_pitch_rest = 0.0f;
		bool m_have_yaw_rest = false, m_have_pitch_rest = false;
		Quat m_heads[kHeadRing]{};
		bool m_head_ok[kHeadRing]{};
		std::uint32_t m_head_seq[kHeadRing]{};
	};
}
