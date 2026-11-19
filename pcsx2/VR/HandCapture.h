// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/HandsGltf.h"

#include "common/Pcsx2Types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class GSLocalMemory;
struct GSVertex;

namespace VR::HandCapture
{
	struct Box
	{
		std::array<double, 3> lo{0, 0, 0};
		std::array<double, 3> hi{0, 0, 0};
		bool operator==(const Box&) const = default;
		bool Contains(double x, double y, double z) const
		{
			return x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] && z <= hi[2];
		}
	};

	enum class BackRank : u8
	{
		Nearest = 0,
		Middle = 1,
		Thin = 2,
	};

	struct WristFrame
	{
		std::array<double, 3> origin{0, 0, 0};
		std::array<double, 3> x_axis{1, 0, 0};
		std::array<double, 3> y_axis{0, 1, 0};
		bool operator==(const WristFrame&) const = default;
	};

	enum class Gate : u8
	{
		None = 0,
		HeadlookArmed = 1,
	};

	enum class OtherSide : u8
	{
		Capture = 0,
		Mirror = 1,
		Glove = 2,
	};

	enum class SideRule : u8
	{
		Boxes = 0,
		CentroidX = 1,
	};

	enum class HideMode : u8
	{
		Split = 0,
		SkipDraw = 1,
	};

	struct Projection
	{
		double fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;
		double k = 1.0;
		bool operator==(const Projection&) const = default;
	};

	struct Recipe
	{
		std::string serial;
		u32 tbp0 = 0, psm = 0;
		u32 tw = 0, th = 0;
		bool has_clut = false;
		u32 cbp = 0, cpsm = 0;
		double w_min = 0.0, w_max = 0.0;
		bool has_uv_box = false;
		std::array<double, 4> uv_box{0, 0, 0, 0};
		double uv_min_coverage = 0.0;
		u32 vu_program = 0;
		Gate gate = Gate::None;
		u32 stable_frames = 30;
		float max_wait_s = 120.0f;
		u32 max_frame_step = 0;
		double max_motion_px = 0.0;
		u32 vertices_min = 0, vertices_max = 0;
		u32 triangles_min = 0, triangles_max = 0;
		u32 open_edges_min = 0, open_edges_max = 0xFFFFFFFFu;
		u32 texture_w = 0, texture_h = 0;
		Projection projection;
		SideRule side_rule = SideRule::Boxes;
		std::array<bool, 2> has_side{false, false};
		std::array<Box, 2> side_box{};
		std::array<WristFrame, 2> wrist{};
		std::array<BackRank, 2> back{BackRank::Nearest, BackRank::Nearest};
		bool has_trim = false;
		Box trim_box{};
		double metres_per_unit = 1.0;
		OtherSide other_side = OtherSide::Capture;
		HideMode hide_mode = HideMode::Split;
		bool operator==(const Recipe&) const = default;
	};

	struct ViewPoint
	{
		double x = 0.0, y = 0.0, z = 0.0;
	};
	bool ValidateRecipe(const Recipe& r, std::string* why);

	bool Unproject(double X, double Y, double ofx, double ofy, double q, const Projection& p, ViewPoint* out);

	inline u64 PositionKey(u32 x, u32 y, u32 z) { return static_cast<u64>(x & 0xFFFF) | (static_cast<u64>(y & 0xFFFF) << 16) | (static_cast<u64>(z) << 32); }
	u32 PartitionObjects(const std::vector<u64>& keys, std::vector<u32>* object, std::unordered_map<u64, u32>* key_object = nullptr);
	u32 OpenEdges(const std::vector<u64>& keys, const std::vector<u32>& triangles);

	double SrgbToLinear(double c255);
	std::array<float, 3> VertexColourFactor(const std::array<u8, 4>& cv, u32 tfx);

	struct CapturedVertex
	{
		u16 x = 0, y = 0;
		u32 z = 0;
		u32 ofx = 0, ofy = 0;
		float s = 0.0f, t = 0.0f, q = 1.0f;
		std::array<u8, 4> rgba{128, 128, 128, 128};
	};
	enum : u8
	{
		kFaceAdc = 1,
		kFaceZeroArea = 2,
		kFaceDrawn = 4,
	};
	struct Capture
	{
		std::vector<CapturedVertex> vertices;
		std::vector<u32> indices;
		std::vector<u8> flags;
		u32 draws = 0;
		u32 tfx = 0;
		HandsGltf::HandTexture texture;
	};

	enum class OrientRule : u8
	{
		None,
		Edges,
		StripParity,
		Mirror,
	};
	const char* OrientRuleName(OrientRule r);
	struct Orientation
	{
		OrientRule rule = OrientRule::None;
		u32 components = 0;
		u32 flipped = 0;
		u32 nonmanifold = 0;
		u32 conflicts = 0;
	};
	struct WristFit
	{
		enum class Rule : u8
		{
			NotBuilt,
			Recipe,
			Fitted,
			Mirror,
		};
		Rule rule = Rule::NotBuilt;
		WristFrame frame;
		double length_m = 0.0;
		std::array<double, 3> spread_m{0, 0, 0};
		u8 palm_axis = 0;
		double hint_near_deg = 0.0, hint_far_deg = 0.0;
	};
	bool IsIdentityHint(const WristFrame& f);

	struct BuildResult
	{
		bool ok = false;
		std::string reason;
		std::array<bool, 2> has{false, false};
		std::array<HandsGltf::HandAsset, 2> hands{};
		std::array<Orientation, 2> orient{};
		std::array<WristFit, 2> wrist{};
		bool fit_refused = false;
	};
	constexpr u32 kFitRetries = 8;
	constexpr s64 kFitRetryHoldFrames = 300;
	constexpr u32 kTimeoutRetries = 4;
	constexpr s64 kTimeoutRetryHoldFrames = 60;
	constexpr double kBackRankMinSpreadRatio = 1.15;
	constexpr double kBackSignMaxDeg = 80.0;
	BuildResult BuildHands(const Recipe& recipe, const Capture& capture, const std::array<const HandsGltf::HandAsset*, 2>& reference);

	bool CheckHandAsset(const HandsGltf::HandAsset& hand, std::string* why);

	struct GsDraw
	{
		const void* vertices = nullptr;
		u32 vertex_count = 0;
		const u16* indices = nullptr;
		u32 index_count = 0;
		u16* edit_indices = nullptr;
		u32* edit_count = nullptr;
		u32 prim = 0;
		bool tme = false, fst = false;
		u64 tex0 = 0, texa = 0, clamp = 0;
		u32 ofx = 0, ofy = 0;
		bool has_scissor = false;
		s32 sc_x0 = 0, sc_x1 = 0, sc_y0 = 0, sc_y1 = 0;
		GSLocalMemory* mem = nullptr;
		s64 frame = 0;
	};

	enum class State : u8
	{
		Off = 0,
		Watching,
		Building,
		Ready,
		Refused,
		TimedOut,
	};

	struct Stats
	{
		u64 draws_seen = 0;
		u64 draws_selected = 0;
		u64 draws_hidden = 0;
		u64 triangles_hidden = 0;
		u64 kicks_tapped = 0;
		u64 batches_from_tap = 0;
		u32 grabs = 0;
	};

	struct WatchStatus
	{
		using Gaps = std::array<u64, 5>;
		struct Break
		{
			bool set = false;
			s64 from = 0, to = 0;
			u64 objects_from = 0, objects_to = 0;
			u64 triangles_from = 0, triangles_to = 0;
			u64 draws_from = 0, draws_to = 0;
		};
		u64 frames = 0;
		Gaps seen_gaps{};
		u64 tex_frames = 0;
		u64 batch_frames = 0;
		u64 hand_frames = 0;
		u64 batches = 0;
		u64 batches_tap = 0;
		Gaps gaps{};
		u32 streak = 0;
		u32 longest = 0;
		u64 breaks_gap = 0;
		u64 breaks_value = 0;
		Break last_gap{}, last_value{};
		u64 last_objects = 0, last_triangles = 0, last_draws = 0;
		u64 stable_no_ref = 0;
		u64 grab_starts = 0, grab_aborts = 0;
		u64 resets = 0;
		u64 breaks_motion = 0;
		double last_motion_px = -1.0;
		u64 toggle_resets = 0;
		u32 timeout_retries = 0;
	};
	std::string WatchStatusText(const WatchStatus& s, const char* at, double t, u32 need, bool ref, State state);

	class Module
	{
	public:
		Module();
		~Module();
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		void Configure(const Recipe* recipe);
		void SetReferenceHands(const HandsGltf::HandAsset& left, const HandsGltf::HandAsset& right);

		void SetEnabled(bool enabled)
		{
#ifndef VR_HANDCAP_PLANTED_TOGGLE_KEEPS_RUN
			if (!enabled)
				m_toggle_reset.store(true, std::memory_order_release);
#endif
			m_enabled.store(enabled, std::memory_order_release);
		}
		bool Enabled() const { return m_enabled.load(std::memory_order_acquire); }
		bool HasRecipe() const { return m_has_recipe.load(std::memory_order_acquire); }

		bool Active() const
		{
#if defined(VR_HANDCAP_PLANTED_WATCH_ALWAYS)
			return true;
#elif defined(VR_HANDCAP_PLANTED_HIDE_IGNORES_TOGGLE)
			return m_active.load(std::memory_order_relaxed);
#else
			return m_active.load(std::memory_order_relaxed) && m_enabled.load(std::memory_order_relaxed);
#endif
		}
		bool OnGsDraw(const GsDraw& draw);
#if defined(ENABLE_VR) && defined(VR_HANDCAP_EXPERIMENT_TAP_IN_READY)
		bool TapWanted() const { return Active() && (GetState() == State::Watching || GetState() == State::Ready); }
#else
		bool TapWanted() const { return Active() && GetState() == State::Watching; }
#endif
		void OnKick(const void* v0, const void* v1, const void* v2, u32 adc, u64 tex0, s64 frame);


		void SetReplayLog(bool on) { m_replay_log.store(on, std::memory_order_release); }

		void SetTrace(std::FILE* f) { m_trace.store(f, std::memory_order_release); }
		bool TraceOn() const { return m_trace.load(std::memory_order_relaxed) != nullptr; }
		bool TraceTapWanted() const
		{
			return TraceOn() && Active() && (GetState() == State::Building || GetState() == State::Ready);
		}
		void OnTraceTapOnly() { m_trace_tap = 3; }
		void OnTraceKick(const void* v0, const void* v1, const void* v2, u32 prim, u32 adc, u32 invalid, u32 cull, u64 tex0, s64 frame);

		State GetState() const { return m_state.load(std::memory_order_acquire); }
		Stats GetStats() const { return m_stats; }
		WatchStatus GetWatchStatus() const { return m_watch; }
		u32 PublishedGeneration() const { return m_pub_gen.load(std::memory_order_acquire); }
		std::shared_ptr<const HandsGltf::HandAsset> Published(int side) const;
		void WaitForBuild();

	private:
		struct FrameTotals
		{
			u64 objects = 0, triangles = 0, draws = 0;
			bool operator==(const FrameTotals& o) const { return objects == o.objects && triangles == o.triangles && draws == o.draws; }
		};
		bool BatchMatches(const GsDraw& d) const;
		bool TextureMatches(u64 tex0) const;
		bool FindHands(const GsDraw& d, bool* from_tap);
		bool Hide(const GsDraw& d);
		void MakeHideSet(const Capture& c);
		u32 HideUvId(double u, double v) const;
		void AccumulateDrawn();
		bool HandleDraw(const GsDraw& d);
		void SyncConfig();
		void EndFrame();
		void AppendGrab(const GsDraw& d, bool with_slots);
		void LaunchBuild();
		void SetState(State s);
		void ReplayLogLine(const GsDraw& d, State state_before, size_t tap_triangles, u32 tap_degenerate, bool found, bool hid);
		void TraceBatch(std::FILE* f, const GsDraw& d, State state_before, size_t tap_triangles, u32 tap_degenerate, bool found,
			bool hid, const std::vector<u16>& idx);

		mutable std::mutex m_cfg_mutex;
		std::unique_ptr<Recipe> m_pending;
		std::atomic<u32> m_cfg_gen{0};
		std::array<std::shared_ptr<const HandsGltf::HandAsset>, 2> m_reference{};

		u32 m_seen_gen = 0;
		bool m_have_recipe = false;
		Recipe m_recipe;
		std::chrono::steady_clock::time_point m_watch_start{};
		s64 m_cur_frame = -1;
		FrameTotals m_cur{};
		bool m_cur_matched = false;
		s64 m_last_frame = -2;
		FrameTotals m_last{};
		u32 m_stable = 0;
		struct GroupShape
		{
			double cx = 0.0, cy = 0.0, x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
		};
		std::vector<GroupShape> m_cur_shapes, m_last_shapes;
		static double ShapeMotion(const std::vector<GroupShape>& a, const std::vector<GroupShape>& b);
		std::atomic<bool> m_toggle_reset{false};
		void ResetRunOnToggle();
		s64 m_run_prev_seen = -1;
		bool RunInRow(s64 prev_seen, s64 frame) const;
		s64 m_grab_frame = -1;
		std::shared_ptr<Capture> m_capture;
		Stats m_stats{};
		u32 m_fit_retries = 0;
		std::atomic<bool> m_retry_reset{false};
		s64 m_retry_hold = 0;
		void ResetRunForRetry(s64 hold);
		WatchStatus m_watch{};
		bool m_cur_tex = false, m_cur_batch = false;
		s64 m_watch_prev_seen = -1;
		double m_watch_next_s = 5.0;
		void WatchStatusLine(const char* at, double t, bool ref);
		bool HaveReference() const;
		std::vector<std::array<const GSVertex*, 3>> m_src;
		std::vector<u64> m_keys;
		std::vector<u32> m_obj;
		std::unordered_map<u64, u32> m_key_obj;
		std::vector<u8> m_obj_hand;
		std::vector<u8> m_src_flags;
		u32 m_hand_objects = 0;
		u64 m_hand_triangles = 0;
		u64 m_hand_drawn = 0;
		std::vector<u8> m_keep;
		u64 m_ready_hidden = 0;
		std::vector<std::vector<u8>> m_slot_drawn;
		std::unordered_multimap<u64, u32> m_hide_cells;
		std::vector<std::array<double, 2>> m_hide_uv;
		std::vector<u64> m_hide_tris;
		std::vector<u32> m_vid;
		std::vector<GSVertex> m_tap;
		std::vector<u8> m_tap_flags;
		u64 m_tap_tex0 = 0;
		s64 m_tap_frame = -1;
		std::atomic<bool> m_replay_log{false};
		u32 m_rlog_degenerate = 0;
		u64 m_find_calls = 0;
		bool m_find_from_tap = false;
		std::atomic<std::FILE*> m_trace{nullptr};
		u32 m_trace_tap = 0;
		u64 m_trace_kseq = 0, m_trace_bseq = 0;
		u32 m_trace_kicks_in_batch = 0;
		u32 m_trace_cfg = 0xFFFFFFFFu;
		std::vector<u16> m_trace_idx;
#if defined(ENABLE_VR) && defined(VR_HANDCAP_EXPERIMENT_TAP_IN_READY)
		std::vector<u8> m_exp_postcull;
		bool m_exp_postcull_valid = false;
		void ExperimentPostCull(const GsDraw& d);
		void ExperimentLine(const GsDraw& d, bool found_b, bool from_tap_b);
#endif

		std::atomic<bool> m_active{false};
		std::atomic<bool> m_enabled{true};
		std::atomic<bool> m_has_recipe{false};
		std::atomic<State> m_state{State::Off};

		std::thread m_worker;
		mutable std::mutex m_pub_mutex;
		std::array<std::shared_ptr<const HandsGltf::HandAsset>, 2> m_pub{};
		std::atomic<u32> m_pub_gen{0};
	};

	extern Module g_module;
	inline Module& Global() { return g_module; }
}
