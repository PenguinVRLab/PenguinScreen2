// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/UiPanel.h"
#include "VR/DisplayFit.h"

#include "VR/PadXRActions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace VR::UiPanel
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDegToRad = kPi / 180.0f;
		constexpr float kRadToDeg = 180.0f / kPi;

		float WrapPi(float a)
		{
			while (a > kPi)
				a -= 2.0f * kPi;
			while (a < -kPi)
				a += 2.0f * kPi;
			return a;
		}

		Panel Ahead(float yaw, float pitch, float distance_m, float width_m, float height_m, float ui_scale)
		{
			const DisplayFit::Placement pl = DisplayFit::PlaceAhead(yaw, pitch, distance_m);
			Panel p;
			p.pose.qx = pl.qx;
			p.pose.qy = pl.qy;
			p.pose.qz = pl.qz;
			p.pose.qw = pl.qw;
			p.pose.x = pl.dx;
			p.pose.y = pl.dy;
			p.pose.z = pl.dz;
			p.width_m = width_m;
			p.height_m = height_m;
			p.distance_m = distance_m;
			p.yaw_rad = yaw;
			p.ui_scale = ui_scale;
			return p;
		}

		std::mutex s_osd_mutex;
		OsdTracker s_osd;
		std::atomic<bool> s_owns_input{false};
		std::atomic<double> s_owns_input_at{0.0};

		static_assert(kEventMenuOpen == VR::PadXRActions::EventMenuOpen, "UiPanel's copy of the PadXR event bits");
		static_assert(kEventMenuClose == VR::PadXRActions::EventMenuClose, "UiPanel's copy of the PadXR event bits");
		static_assert(kEventRecentre == VR::PadXRActions::EventRecentre, "UiPanel's copy of the PadXR event bits");

		double NowSeconds()
		{
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}
	}

	YawPitch HeadYawPitch(const Pose& h)
	{
		const float fx = -2.0f * (h.qw * h.qy + h.qz * h.qx);
		const float fy = 2.0f * (h.qw * h.qx - h.qy * h.qz);
		const float fz = -(1.0f - 2.0f * (h.qx * h.qx + h.qy * h.qy));
		YawPitch yp;
		yp.pitch = std::asin(std::clamp(fy, -1.0f, 1.0f));
		if ((fx * fx + fz * fz) > 1.0e-4f)
		{
			yp.yaw = std::atan2(-fx, -fz);
			yp.ok = true;
		}
		return yp;
	}

	float HeightFor(float width_m)
	{
		return width_m * static_cast<float>(kTextureHeight) / static_cast<float>(kTextureWidth);
	}

	Panel PlaceAtOpen(const Head& head, const Settings& st)
	{
		const YawPitch yp = HeadYawPitch(head.pose);
		const float lim = std::max(st.max_pitch_deg, 0.0f) * kDegToRad;
		const float yaw = yp.ok ? yp.yaw : 0.0f;
		const float pitch = yp.ok ? std::clamp(yp.pitch, -lim, lim) : 0.0f;
		Panel p = Ahead(yaw, pitch, st.distance_m, st.width_m, HeightFor(st.width_m), st.ui_scale);
		p.pose.x += head.pose.x;
		p.pose.y += head.pose.y - st.drop_m;
		p.pose.z += head.pose.z;
		return p;
	}

	float HorizontalDeg(const Panel& p)
	{
		if (!(p.distance_m > 0.0f))
			return 0.0f;
		return 2.0f * std::atan(0.5f * p.width_m / p.distance_m) * kRadToDeg;
	}

	float VerticalDeg(const Panel& p)
	{
		if (!(p.distance_m > 0.0f))
			return 0.0f;
		return 2.0f * std::atan(0.5f * p.height_m / p.distance_m) * kRadToDeg;
	}

	float TextHeightDeg(const Panel& p, float font_px)
	{
		const float deg_per_px = VerticalDeg(p) / static_cast<float>(kTextureHeight);
#ifdef VR_UIPANEL_PLANTED_NOSCALE
		return font_px * deg_per_px;
#else
		return font_px * p.ui_scale * deg_per_px;
#endif
	}

	bool ComeBackTick(ComeBack& cb, const YawPitch& head, float panel_yaw_rad, float dt_s, const Settings& st)
	{
		if (!head.ok)
			return false;
		const float away = std::fabs(WrapPi(head.yaw - panel_yaw_rad));
		if (away > st.comeback_deg * kDegToRad)
		{
			cb.away_s += std::max(dt_s, 0.0f);
			if (cb.away_s >= st.comeback_hold_s)
			{
				cb.away_s = 0.0f;
				return true;
			}
			return false;
		}
		cb.away_s = 0.0f;
		return false;
	}

	Event Update(State& s, bool want_open, const Head& head, float dt_s, const Settings& st)
	{
		if (!want_open)
		{
			const bool was = s.open;
			s = State{};
			return was ? Event::Closed : Event::None;
		}
		if (!s.open)
		{
			s.open = true;
			s.placed = false;
			s.comeback = {};
		}
		if (!s.placed)
		{
			if (!head.valid)
				return Event::None;
			s.panel = PlaceAtOpen(head, st);
			s.placed = true;
			return Event::Opened;
		}
		if (!head.valid)
			return Event::None;
		if (s.replace_pending)
		{
			s.replace_pending = false;
			s.comeback = {};
			s.panel = PlaceAtOpen(head, st);
			return Event::Replaced;
		}
		if (ComeBackTick(s.comeback, HeadYawPitch(head.pose), s.panel.yaw_rad, dt_s, st))
		{
			s.panel = PlaceAtOpen(head, st);
			return Event::Replaced;
		}
		return Event::None;
	}

	void RequestReplace(State& s)
	{
#ifndef VR_UIPANEL_PLANTED_NO_REPLACE
		if (s.open && s.placed)
			s.replace_pending = true;
#else
		(void)s;
#endif
	}

	void SetOwnsInputAt(bool owns, double now_s)
	{
		s_owns_input_at.store(now_s, std::memory_order_relaxed);
		s_owns_input.store(owns, std::memory_order_release);
	}

	bool OwnsInputAt(double now_s)
	{
#ifdef VR_UIPANEL_PLANTED_NO_OWNER
		(void)now_s;
		return false;
#else
		if (!s_owns_input.load(std::memory_order_acquire))
			return false;
#ifdef VR_UIPANEL_PLANTED_MASK_NEVER_LAPSES
		(void)now_s;
		return true;
#else
		return (now_s - s_owns_input_at.load(std::memory_order_relaxed)) <= kOwnsInputStaleSeconds;
#endif
#endif
	}

	void SetOwnsInput(bool owns) { SetOwnsInputAt(owns, NowSeconds()); }
	bool OwnsInput() { return OwnsInputAt(NowSeconds()); }

	SystemRoute RouteSystemEvents(std::uint32_t events, bool ui_open)
	{
		SystemRoute r;
		if (events & kEventRecentre)
		{
			r.recentre = true;
			r.replace_panel = ui_open;
#ifdef VR_UIPANEL_PLANTED_LONGHOLD_CLOSES
			r.close_ui = ui_open;
#endif
		}
		if (events & (kEventMenuOpen | kEventMenuClose))
		{
			if (ui_open)
				r.close_ui = true;
			else
				r.open_quick_menu = true;
		}
		return r;
	}

	void NavTick(NavState& st, const NavInput& in, std::vector<NavEdge>& out)
	{
#ifdef VR_UIPANEL_PLANTED_NAV_LEVEL
		if (in.select)
			out.push_back({NavKey::Select, true});
		if (in.back)
			out.push_back({NavKey::Back, true});
		st.select = in.select;
		st.back = in.back;
#else
		if (in.select != st.select)
		{
			st.select = in.select;
			out.push_back({NavKey::Select, in.select});
		}
		if (in.back != st.back)
		{
			st.back = in.back;
			out.push_back({NavKey::Back, in.back});
		}
#endif
		const float ax = std::fabs(in.x), ay = std::fabs(in.y);
		int want = st.dir;
		if (!(ax >= kNavOff) && !(ay >= kNavOff))
			want = -1;
		else if (ax >= kNavOn || ay >= kNavOn)
		{
			if (ay >= ax)
				want = static_cast<int>(in.y > 0.0f ? NavKey::Up : NavKey::Down);
			else
				want = static_cast<int>(in.x > 0.0f ? NavKey::Right : NavKey::Left);
		}
		if (want != st.dir)
		{
			if (st.dir >= 0)
				out.push_back({static_cast<NavKey>(st.dir), false});
			if (want >= 0)
				out.push_back({static_cast<NavKey>(want), true});
			st.dir = want;
		}
	}

	void NavRelease(NavState& st, std::vector<NavEdge>& out)
	{
		if (st.select)
			out.push_back({NavKey::Select, false});
		if (st.back)
			out.push_back({NavKey::Back, false});
		if (st.dir >= 0)
			out.push_back({static_cast<NavKey>(st.dir), false});
		st = NavState{};
	}

	void OsdTracker::Post(std::string_view key, double now_s, double duration_s)
	{
		if (!key.empty())
		{
			for (auto it = m_msgs.begin(); it != m_msgs.end(); ++it)
			{
				if (it->key == key)
				{
					if (duration_s <= 0.0)
						m_msgs.erase(it);
					else
						it->end_s = now_s + duration_s;
					return;
				}
			}
		}
		if (duration_s > 0.0)
			m_msgs.push_back(Msg{std::string(key), now_s + duration_s});
	}

	bool OsdTracker::Live(double now_s)
	{
		m_msgs.erase(std::remove_if(m_msgs.begin(), m_msgs.end(), [now_s](const Msg& m) { return m.end_s <= now_s; }),
			m_msgs.end());
		return !m_msgs.empty();
	}

	bool StripVisible(bool strip_enabled, bool message_live)
	{
#ifdef VR_UIPANEL_PLANTED_STRIPALWAYS
		(void)message_live;
		return strip_enabled;
#else
		return strip_enabled && message_live;
#endif
	}

	void PostOsdMessage(std::string_view key, float duration_s)
	{
		const double now = NowSeconds();
		std::lock_guard<std::mutex> lock(s_osd_mutex);
		s_osd.Post(key, now, duration_s);
	}

	bool OsdLiveNow()
	{
		const double now = NowSeconds();
		std::lock_guard<std::mutex> lock(s_osd_mutex);
		return s_osd.Live(now);
	}

	void ResetOsd()
	{
		std::lock_guard<std::mutex> lock(s_osd_mutex);
		s_osd.Clear();
	}

	Rect StripImageRect()
	{
		return Rect{0, static_cast<std::int32_t>(kTextureHeight - kStripHeightPx), static_cast<std::int32_t>(kTextureWidth),
			static_cast<std::int32_t>(kStripHeightPx)};
	}

	Rect PanelImageRect()
	{
		return Rect{0, 0, static_cast<std::int32_t>(kTextureWidth), static_cast<std::int32_t>(kTextureHeight)};
	}

	Panel PlaceStrip(const Settings& st)
	{
		const float h = st.strip_width_m * static_cast<float>(kStripHeightPx) / static_cast<float>(kTextureWidth);
		return Ahead(0.0f, st.strip_pitch_deg * kDegToRad, st.strip_distance_m, st.strip_width_m, h, st.ui_scale);
	}

	std::uint32_t EffectiveCapacity(std::uint32_t array_capacity, std::uint32_t runtime_max_layers)
	{
		if (runtime_max_layers == 0)
			return array_capacity;
		return std::min(array_capacity, runtime_max_layers);
	}

	LayerPlan PlanLayers(const LayerRequest& rq, std::uint32_t capacity)
	{
		LayerPlan p;
#ifdef VR_UIPANEL_PLANTED_NODROP
		(void)capacity;
		p.screens = rq.screens;
		p.levers = rq.levers;
		p.member_panels = rq.member_panels;
		p.hands = rq.hands;
		p.panel = rq.panel;
		p.strip = rq.strip;
		return p;
#else
#ifdef VR_UIPANEL_PLANTED_NOLAYER
		p.panel = false;
#else
		p.panel = rq.panel;
#endif
		std::uint32_t left = capacity;
		if (p.panel)
			left = (left > 0) ? left - 1 : 0;
		if (p.panel && capacity == 0)
			p.panel = false;
		p.screens = std::min(rq.screens, left);
		p.screens_cut = rq.screens - p.screens;
		left -= p.screens;
		p.hands = std::min(rq.hands, left);
		left -= p.hands;
		p.member_panels = std::min(rq.member_panels, left);
		left -= p.member_panels;
		p.levers = std::min(rq.levers, left);
		left -= p.levers;
		p.strip = rq.strip && left > 0;
		return p;
#endif
	}

	void TestPattern(std::vector<std::uint32_t>& out)
	{
		constexpr std::uint32_t W = kTextureWidth, H = kTextureHeight;
		constexpr std::uint32_t kStripTop = kTextureHeight - kStripHeightPx;
		const std::uint32_t bg = PackRgba(0x14, 0x18, 0x22, 0xD0);
		const std::uint32_t grid = PackRgba(0x3a, 0x40, 0x50, 0xE0);
		const std::uint32_t border = PackRgba(0x5c, 0xb0, 0xff, 0xFF);
		const std::uint32_t marker = PackRgba(0xff, 0x30, 0x30, 0xFF);
		const std::uint32_t strip = PackRgba(0xff, 0xb0, 0x30, 0xE0);
		const std::uint32_t strip_edge = PackRgba(0xff, 0xe0, 0xa0, 0xFF);
		out.assign(static_cast<std::size_t>(W) * H, bg);
		for (std::uint32_t y = 0; y < H; y++)
		{
			for (std::uint32_t x = 0; x < W; x++)
			{
				std::uint32_t c = bg;
				if (y >= kStripTop)
				{
					const bool edge = (y < kStripTop + 4) || (y >= H - 4) || (x < 4) || (x >= W - 4);
					c = edge ? strip_edge : strip;
				}
				else
				{
					if ((x % 80) == 0 || (y % 80) == 0)
						c = grid;
					if (x < 8 || x >= W - 8 || y < 8)
						c = border;
					if (x >= 16 && x < 80 && y >= 16 && y < 80)
						c = marker;
					if ((x == W / 2 || x == W / 2 - 1) || (y == kStripTop / 2 || y == kStripTop / 2 - 1))
						c = border;
				}
				out[static_cast<std::size_t>(y) * W + x] = c;
			}
		}
	}
}
