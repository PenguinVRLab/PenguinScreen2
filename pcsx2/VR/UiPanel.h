// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace VR::UiPanel
{
	inline constexpr std::uint32_t kTextureWidth = 1280;
	inline constexpr std::uint32_t kTextureHeight = 720;
	inline constexpr std::uint32_t kStripHeightPx = 120;

	inline constexpr std::uint32_t kUiLayers = 2;

	struct Settings
	{
		float distance_m = 1.2f;
		float width_m = 1.0f;
		float ui_scale = 2.0f;
		float drop_m = 0.05f;
		float max_pitch_deg = 15.0f;
		float comeback_deg = 40.0f;
		float comeback_hold_s = 1.0f;
		float fade_ms = 150.0f;
		float strip_distance_m = 1.2f;
		float strip_pitch_deg = -20.0f;
		float strip_width_m = 1.0f;
	};

	struct Pose
	{
		float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	struct Head
	{
		Pose pose;
		bool valid = false;
	};

	struct Panel
	{
		Pose pose;
		float width_m = 0.0f;
		float height_m = 0.0f;
		float distance_m = 0.0f;
		float yaw_rad = 0.0f;
		float ui_scale = 1.0f;
	};

	struct YawPitch
	{
		float yaw = 0.0f;
		float pitch = 0.0f;
		bool ok = false;
	};
	YawPitch HeadYawPitch(const Pose& head);

	float HeightFor(float width_m);

	Panel PlaceAtOpen(const Head& head, const Settings& st);

	float HorizontalDeg(const Panel& p);
	float VerticalDeg(const Panel& p);
	float TextHeightDeg(const Panel& p, float font_px);

	struct ComeBack
	{
		float away_s = 0.0f;
	};
	bool ComeBackTick(ComeBack& cb, const YawPitch& head, float panel_yaw_rad, float dt_s, const Settings& st);

	struct State
	{
		bool open = false;
		bool placed = false;
		bool replace_pending = false;
		Panel panel;
		ComeBack comeback;
	};
	enum class Event
	{
		None,
		Opened,
		Replaced,
		Closed,
	};
	Event Update(State& s, bool want_open, const Head& head, float dt_s, const Settings& st);
	inline bool Visible(const State& s) { return s.open && s.placed; }
	void RequestReplace(State& s);

	inline constexpr double kOwnsInputStaleSeconds = 0.5;
	void SetOwnsInput(bool owns);
	bool OwnsInput();
	void SetOwnsInputAt(bool owns, double now_s);
	bool OwnsInputAt(double now_s);

	inline constexpr std::uint32_t kEventMenuOpen = 1u << 0;
	inline constexpr std::uint32_t kEventMenuClose = 1u << 1;
	inline constexpr std::uint32_t kEventRecentre = 1u << 2;
	struct SystemRoute
	{
		bool open_quick_menu = false;
		bool close_ui = false;
		bool recentre = false;
		bool replace_panel = false;
	};
	SystemRoute RouteSystemEvents(std::uint32_t events, bool ui_open);

	enum class NavKey : std::uint8_t
	{
		Up,
		Down,
		Left,
		Right,
		Select,
		Back,
		Count,
	};
	struct NavInput
	{
		bool select = false;
		bool back = false;
		float x = 0.0f;
		float y = 0.0f;
	};
	struct NavEdge
	{
		NavKey key;
		bool down;
	};
	inline constexpr float kNavOn = 0.5f;
	inline constexpr float kNavOff = 0.2f;
	struct NavState
	{
		bool select = false;
		bool back = false;
		int dir = -1;
	};
	void NavTick(NavState& st, const NavInput& in, std::vector<NavEdge>& out);
	void NavRelease(NavState& st, std::vector<NavEdge>& out);

	class OsdTracker
	{
	public:
		void Post(std::string_view key, double now_s, double duration_s);
		bool Live(double now_s);
		void Clear() { m_msgs.clear(); }
		std::size_t Count() const { return m_msgs.size(); }

	private:
		struct Msg
		{
			std::string key;
			double end_s;
		};
		std::vector<Msg> m_msgs;
	};

	bool StripVisible(bool strip_enabled, bool message_live);

	void PostOsdMessage(std::string_view key, float duration_s);
	bool OsdLiveNow();
	void ResetOsd();

	struct Rect
	{
		std::int32_t x = 0, y = 0, w = 0, h = 0;
	};
	Rect StripImageRect();
	Rect PanelImageRect();
	Panel PlaceStrip(const Settings& st);

	struct LayerRequest
	{
		std::uint32_t screens = 0;
		std::uint32_t levers = 0;
		std::uint32_t member_panels = 0;
		std::uint32_t hands = 0;
		bool panel = false;
		bool strip = false;
	};
	struct LayerPlan
	{
		std::uint32_t screens = 0;
		std::uint32_t levers = 0;
		std::uint32_t member_panels = 0;
		std::uint32_t hands = 0;
		bool panel = false;
		bool strip = false;
		std::uint32_t screens_cut = 0;
		std::uint32_t Total() const
		{
			return screens + levers + member_panels + hands + (panel ? 1u : 0u) + (strip ? 1u : 0u);
		}
	};
	std::uint32_t EffectiveCapacity(std::uint32_t array_capacity, std::uint32_t runtime_max_layers);
	LayerPlan PlanLayers(const LayerRequest& rq, std::uint32_t capacity);

	void TestPattern(std::vector<std::uint32_t>& out);
	inline constexpr std::uint32_t PackRgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a = 0xFF)
	{
		return r | (g << 8) | (b << 16) | (a << 24);
	}
}
