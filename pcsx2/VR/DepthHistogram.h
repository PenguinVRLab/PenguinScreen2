// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <string>
#include <vector>

namespace VR
{

	enum class DrawClass : u8
	{

		Displaced = 0,

		FstExcluded,

		UniformQPinned,

		MonoCentre,

		StereoOff,

		AccurateStqFlagged,
	};

	struct DepthHistogram
	{

		static constexpr int kSchema = 2;
		static constexpr int kBinsPerOctave = 6;
		static constexpr int kLog2WMin = -6;
		static constexpr int kLog2WMax = 10;
		static constexpr int kBinCount = (kLog2WMax - kLog2WMin) * kBinsPerOctave;

		static constexpr double kQOverflow = 1e30;

		struct Bucket
		{
			double coverage = 0.0;
			double q_moment = 0.0;
			u64 prims = 0;
			u64 verts = 0;

			void Add(double area_fraction, double q, u32 n_prims, u32 n_verts);

			double MeanQ() const;
		};

		struct Census
		{
			u64 displaced = 0;
			u64 fst_excluded = 0;
			u64 uniform_q_pinned = 0;
			u64 mono_centre = 0;
			u64 stereo_off = 0;
			u64 accurate_stq_flagged = 0;

			u64 wide_q_displaced = 0;

			u64 Total() const;
		};

		struct Key
		{
			std::string serial;
			std::string crc;
			std::string dump;
			u32 frame = 0;
			int unscaled_w = 0;
			int unscaled_h = 0;
			bool widescreen_hack = false;
		};

		std::array<Bucket, kBinCount> bins{};
		Bucket near_overflow{};
		Bucket far_overflow{};
		Bucket non_finite{};
		Census census{};
		Key key{};

		struct Summary
		{
			double p01 = 0.0, p05 = 0.0, p10 = 0.0, p25 = 0.0, p50 = 0.0;
			double p75 = 0.0, p90 = 0.0, p95 = 0.0, p99 = 0.0;
			double octave_span_p05_p95 = 0.0;
			std::vector<double> modes;
			double valley_depth = 0.0;
			double coverage_per_prim = 0.0;
			double total_coverage = 0.0;
		};

		void Reset();

		void AddDraw(double area_fraction, double q_min, double q_max, u32 prims, u32 verts, DrawClass cls);

		void NoteTargetSize(int unscaled_w, int unscaled_h);

		static int BinIndexForW(double w);

		static double BinCentreW(int i);

		static double BinEdgeW(int i);

		Summary ComputeSummary() const;

		std::string ToJson() const;

		bool WriteJson(const std::string& path, std::string* error = nullptr) const;
	};

	namespace detail
	{

		extern bool g_qhist_armed;
	}

	void ArmDepthHistogram(bool armed);

	inline bool DepthHistogramArmed()
	{
		return detail::g_qhist_armed;
	}

	DepthHistogram& GlobalDepthHistogram();
}
