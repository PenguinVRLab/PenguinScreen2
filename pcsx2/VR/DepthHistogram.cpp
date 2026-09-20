// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#include "VR/DepthHistogram.h"

#include "common/FileSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace VR
{
	namespace detail
	{
		bool g_qhist_armed = false;
	}

	void ArmDepthHistogram(bool armed)
	{
		detail::g_qhist_armed = armed;
	}

	DepthHistogram& GlobalDepthHistogram()
	{
		static DepthHistogram s_hist;
		return s_hist;
	}

	void DepthHistogram::Bucket::Add(double area_fraction, double q, u32 n_prims, u32 n_verts)
	{
		coverage += area_fraction;
		q_moment += area_fraction * q;
		prims += n_prims;
		verts += n_verts;
	}

	double DepthHistogram::Bucket::MeanQ() const
	{
		return (coverage > 0.0) ? (q_moment / coverage) : 0.0;
	}

	u64 DepthHistogram::Census::Total() const
	{
		return displaced + fst_excluded + uniform_q_pinned + mono_centre + stereo_off + accurate_stq_flagged;
	}

	int DepthHistogram::BinIndexForW(double w)
	{
		if (!std::isfinite(w) || w <= 0.0)
			return kBinCount;
		const double octaves = std::log2(w) - static_cast<double>(kLog2WMin);
		const double fbin = std::floor(octaves * static_cast<double>(kBinsPerOctave));
		if (fbin < 0.0)
			return -1;
		if (fbin >= static_cast<double>(kBinCount))
			return kBinCount;
		return static_cast<int>(fbin);
	}

	double DepthHistogram::BinEdgeW(int i)
	{
		return std::exp2(static_cast<double>(kLog2WMin) + static_cast<double>(i) / static_cast<double>(kBinsPerOctave));
	}

	double DepthHistogram::BinCentreW(int i)
	{
		return std::exp2(static_cast<double>(kLog2WMin) +
						 (static_cast<double>(i) + 0.5) / static_cast<double>(kBinsPerOctave));
	}

	void DepthHistogram::Reset()
	{
		*this = DepthHistogram{};
	}

	void DepthHistogram::AddDraw(double area_fraction, double q_min, double q_max, u32 prims, u32 verts, DrawClass cls)
	{
		switch (cls)
		{
			case DrawClass::Displaced:
				census.displaced++;
				break;
			case DrawClass::FstExcluded:
				census.fst_excluded++;
				return;
			case DrawClass::UniformQPinned:
				census.uniform_q_pinned++;
				return;
			case DrawClass::MonoCentre:
				census.mono_centre++;
				return;
			case DrawClass::StereoOff:
				census.stereo_off++;
				return;
			case DrawClass::AccurateStqFlagged:

				census.accurate_stq_flagged++;
				return;
			default:
				return;
		}

		if (!std::isfinite(area_fraction) || area_fraction < 0.0)
			area_fraction = 0.0;

		if (q_min > q_max)
			std::swap(q_min, q_max);

		if (!std::isfinite(q_min) || !std::isfinite(q_max) || q_min <= 0.0 || q_max >= kQOverflow)
		{
			non_finite.Add(area_fraction, 0.0, prims, verts);
			return;
		}

		if (q_max > 2.0 * q_min)
			census.wide_q_displaced++;

		const double dq = q_max - q_min;

		if (!(dq > 0.0))
		{
			const double w = 1.0 / q_min;
			const int idx = BinIndexForW(w);
			Bucket& b = (idx < 0) ? near_overflow : ((idx >= kBinCount) ? far_overflow : bins[static_cast<size_t>(idx)]);
			b.Add(area_fraction, q_min, prims, verts);
			return;
		}

		const double q_far_edge = std::exp2(static_cast<double>(-kLog2WMax));
		const double q_near_edge = std::exp2(static_cast<double>(-kLog2WMin));

		const auto slice_add = [&](Bucket& b, double qa, double qb) {
			if (!(qb > qa))
				return;
			const double f = (qb - qa) / dq;
			b.Add(area_fraction * f, 0.5 * (qa + qb), 0, 0);
		};

		slice_add(far_overflow, q_min, std::min(q_max, q_far_edge));
		slice_add(near_overflow, std::max(q_min, q_near_edge), q_max);

		const double bq_lo = std::max(q_min, q_far_edge);
		const double bq_hi = std::min(q_max, q_near_edge);
		if (bq_hi > bq_lo)
		{

			int i_near = BinIndexForW(1.0 / bq_hi);
			int i_deep = BinIndexForW(1.0 / bq_lo);
			i_near = std::max(i_near, 0);
			i_deep = std::min(i_deep, kBinCount - 1);
			for (int i = i_near; i <= i_deep; i++)
			{

				const double qa = std::max(bq_lo, 1.0 / BinEdgeW(i + 1));
				const double qb = std::min(bq_hi, 1.0 / BinEdgeW(i));
				slice_add(bins[static_cast<size_t>(i)], qa, qb);
			}
		}

		const double mid_q = 0.5 * (q_min + q_max);
		const int mid_idx = BinIndexForW(1.0 / mid_q);
		Bucket& mb =
			(mid_idx < 0) ? near_overflow : ((mid_idx >= kBinCount) ? far_overflow : bins[static_cast<size_t>(mid_idx)]);
		mb.Add(0.0, 0.0, prims, verts);
	}

	void DepthHistogram::NoteTargetSize(int unscaled_w, int unscaled_h)
	{
		if (unscaled_w <= 0 || unscaled_h <= 0)
			return;
		const s64 area = static_cast<s64>(unscaled_w) * static_cast<s64>(unscaled_h);
		const s64 have = static_cast<s64>(key.unscaled_w) * static_cast<s64>(key.unscaled_h);
		if (area > have)
		{
			key.unscaled_w = unscaled_w;
			key.unscaled_h = unscaled_h;
		}
	}

	namespace
	{

		double PercentileW(const DepthHistogram& h, double p)
		{
			double total = h.near_overflow.coverage + h.far_overflow.coverage;
			for (const auto& b : h.bins)
				total += b.coverage;
			if (!(total > 0.0))
				return 0.0;

			const double target = p * total;
			double cum = h.near_overflow.coverage;
			if (target <= cum)
				return DepthHistogram::BinEdgeW(0);

			for (int i = 0; i < DepthHistogram::kBinCount; i++)
			{
				const double c = h.bins[static_cast<size_t>(i)].coverage;
				if (c > 0.0 && target <= cum + c)
				{

					const double frac = (target - cum) / c;
					const double log2w = std::log2(DepthHistogram::BinEdgeW(i)) +
										 frac / static_cast<double>(DepthHistogram::kBinsPerOctave);
					return std::exp2(log2w);
				}
				cum += c;
			}
			return DepthHistogram::BinEdgeW(DepthHistogram::kBinCount);
		}
	}

	DepthHistogram::Summary DepthHistogram::ComputeSummary() const
	{
		Summary s;

		double total_cov = near_overflow.coverage + far_overflow.coverage;
		u64 total_prims = near_overflow.prims + far_overflow.prims;
		for (const auto& b : bins)
		{
			total_cov += b.coverage;
			total_prims += b.prims;
		}
		s.total_coverage = total_cov;
		s.coverage_per_prim = (total_prims > 0) ? (total_cov / static_cast<double>(total_prims)) : 0.0;

		s.p01 = PercentileW(*this, 0.01);
		s.p05 = PercentileW(*this, 0.05);
		s.p10 = PercentileW(*this, 0.10);
		s.p25 = PercentileW(*this, 0.25);
		s.p50 = PercentileW(*this, 0.50);
		s.p75 = PercentileW(*this, 0.75);
		s.p90 = PercentileW(*this, 0.90);
		s.p95 = PercentileW(*this, 0.95);
		s.p99 = PercentileW(*this, 0.99);
		s.octave_span_p05_p95 = (s.p05 > 0.0 && s.p95 > 0.0) ? std::log2(s.p95 / s.p05) : 0.0;

		std::array<double, kBinCount> sm{};
		for (int i = 0; i < kBinCount; i++)
		{
			double acc = bins[static_cast<size_t>(i)].coverage;
			int n = 1;
			if (i > 0)
			{
				acc += bins[static_cast<size_t>(i - 1)].coverage;
				n++;
			}
			if (i + 1 < kBinCount)
			{
				acc += bins[static_cast<size_t>(i + 1)].coverage;
				n++;
			}
			sm[static_cast<size_t>(i)] = acc / static_cast<double>(n);
		}

		double peak = 0.0;
		for (const double v : sm)
			peak = std::max(peak, v);

		std::vector<std::pair<int, double>> cands;
		if (peak > 0.0)
		{
			for (int i = 0; i < kBinCount; i++)
			{
				const double v = sm[static_cast<size_t>(i)];
				if (v < 0.05 * peak)
					continue;
				const double l = (i > 0) ? sm[static_cast<size_t>(i - 1)] : -1.0;
				const double r = (i + 1 < kBinCount) ? sm[static_cast<size_t>(i + 1)] : -1.0;
				if (v >= l && v > r)
					cands.emplace_back(i, v);
			}
		}
		std::stable_sort(cands.begin(), cands.end(),
			[](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });

		std::vector<std::pair<int, double>> kept;
		for (const auto& c : cands)
		{
			bool too_close = false;
			for (const auto& k : kept)
			{
				if (std::abs(c.first - k.first) < kBinsPerOctave)
				{
					too_close = true;
					break;
				}
			}
			if (!too_close)
				kept.push_back(c);
			if (kept.size() >= 4)
				break;
		}

		s.modes.reserve(kept.size());
		for (const auto& k : kept)
			s.modes.push_back(BinCentreW(k.first));

		if (kept.size() >= 2)
		{
			const int a = std::min(kept[0].first, kept[1].first);
			const int b = std::max(kept[0].first, kept[1].first);
			double valley = sm[static_cast<size_t>(a)];
			for (int i = a; i <= b; i++)
				valley = std::min(valley, sm[static_cast<size_t>(i)]);
			const double lower_peak = std::min(kept[0].second, kept[1].second);
			s.valley_depth = (lower_peak > 0.0) ? std::clamp((lower_peak - valley) / lower_peak, 0.0, 1.0) : 0.0;
		}

		return s;
	}

	namespace
	{

		void AppendNum(std::string& out, double v)
		{
			char buf[40];
			if (!std::isfinite(v))
			{
				out += '0';
				return;
			}
			std::snprintf(buf, sizeof(buf), "%.9g", v);
			out += buf;
		}

		void AppendU64(std::string& out, u64 v)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
			out += buf;
		}

		void AppendStr(std::string& out, const std::string& s)
		{
			out += '"';
			for (const char c : s)
			{
				switch (c)
				{
					case '"':
						out += "\\\"";
						break;
					case '\\':
						out += "\\\\";
						break;
					case '\n':
						out += "\\n";
						break;
					case '\r':
						out += "\\r";
						break;
					case '\t':
						out += "\\t";
						break;
					default:
						if (static_cast<unsigned char>(c) < 0x20)
						{
							char buf[8];
							std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c) & 0xFFu);
							out += buf;
						}
						else
						{
							out += c;
						}
						break;
				}
			}
			out += '"';
		}

		void AppendNumArray(std::string& out, const char* name, const double* v, size_t n, const char* indent)
		{
			out += indent;
			out += '"';
			out += name;
			out += "\": [\n";
			for (size_t i = 0; i < n; i++)
			{
				out += indent;
				out += "  ";
				AppendNum(out, v[i]);
				if (i + 1 < n)
					out += ',';
				out += '\n';
			}
			out += indent;
			out += ']';
		}

		void AppendU64Array(std::string& out, const char* name, const u64* v, size_t n, const char* indent)
		{
			out += indent;
			out += '"';
			out += name;
			out += "\": [\n";
			for (size_t i = 0; i < n; i++)
			{
				out += indent;
				out += "  ";
				AppendU64(out, v[i]);
				if (i + 1 < n)
					out += ',';
				out += '\n';
			}
			out += indent;
			out += ']';
		}

		void AppendBucket(std::string& out, const char* name, const DepthHistogram::Bucket& b, const char* indent)
		{
			out += indent;
			out += '"';
			out += name;
			out += "\": {\"coverage\": ";
			AppendNum(out, b.coverage);
			out += ", \"prims\": ";
			AppendU64(out, b.prims);
			out += ", \"q_moment\": ";
			AppendNum(out, b.q_moment);
			out += ", \"verts\": ";
			AppendU64(out, b.verts);
			out += '}';
		}

		void AppendKV(std::string& out, const char* indent, const char* name, double v)
		{
			out += indent;
			out += '"';
			out += name;
			out += "\": ";
			AppendNum(out, v);
		}
	}

	std::string DepthHistogram::ToJson() const
	{
		const Summary s = ComputeSummary();

		std::vector<double> cov(kBinCount), qm(kBinCount);
		std::vector<u64> pr(kBinCount), ve(kBinCount);
		for (int i = 0; i < kBinCount; i++)
		{
			cov[static_cast<size_t>(i)] = bins[static_cast<size_t>(i)].coverage;
			qm[static_cast<size_t>(i)] = bins[static_cast<size_t>(i)].q_moment;
			pr[static_cast<size_t>(i)] = bins[static_cast<size_t>(i)].prims;
			ve[static_cast<size_t>(i)] = bins[static_cast<size_t>(i)].verts;
		}

		std::string o;
		o.reserve(16384);
		o += "{\n";

		o += "  \"binning\": {\n";
		o += "    \"bins_per_octave\": ";
		AppendU64(o, static_cast<u64>(kBinsPerOctave));
		o += ",\n    \"count\": ";
		AppendU64(o, static_cast<u64>(kBinCount));
		o += ",\n    \"log2w_max\": ";
		AppendU64(o, static_cast<u64>(kLog2WMax));
		o += ",\n    \"log2w_min\": -";
		AppendU64(o, static_cast<u64>(-kLog2WMin));
		o += "\n  },\n";

		o += "  \"bins\": {\n";
		AppendNumArray(o, "coverage", cov.data(), cov.size(), "    ");
		o += ",\n";
		AppendU64Array(o, "prims", pr.data(), pr.size(), "    ");
		o += ",\n";
		AppendNumArray(o, "q_moment", qm.data(), qm.size(), "    ");
		o += ",\n";
		AppendU64Array(o, "verts", ve.data(), ve.size(), "    ");
		o += "\n  },\n";

		o += "  \"census\": {\n";
		o += "    \"accurate_stq_flagged\": ";
		AppendU64(o, census.accurate_stq_flagged);
		o += ",\n    \"displaced\": ";
		AppendU64(o, census.displaced);
		o += ",\n    \"fst_excluded\": ";
		AppendU64(o, census.fst_excluded);
		o += ",\n    \"mono_centre\": ";
		AppendU64(o, census.mono_centre);
		o += ",\n    \"stereo_off\": ";
		AppendU64(o, census.stereo_off);
		o += ",\n    \"total\": ";
		AppendU64(o, census.Total());
		o += ",\n    \"uniform_q_pinned\": ";
		AppendU64(o, census.uniform_q_pinned);
		o += ",\n    \"wide_q_displaced\": ";
		AppendU64(o, census.wide_q_displaced);
		o += "\n  },\n";

		o += "  \"key\": {\n    \"crc\": ";
		AppendStr(o, key.crc);
		o += ",\n    \"dump\": ";
		AppendStr(o, key.dump);
		o += ",\n    \"frame\": ";
		AppendU64(o, key.frame);
		o += ",\n    \"serial\": ";
		AppendStr(o, key.serial);
		o += ",\n    \"unscaled_h\": ";
		AppendU64(o, static_cast<u64>(std::max(0, key.unscaled_h)));
		o += ",\n    \"unscaled_w\": ";
		AppendU64(o, static_cast<u64>(std::max(0, key.unscaled_w)));
		o += ",\n    \"widescreen_hack\": ";
		o += key.widescreen_hack ? "true" : "false";
		o += "\n  },\n";

		o += "  \"overflow\": {\n";
		AppendBucket(o, "far", far_overflow, "    ");
		o += ",\n";
		AppendBucket(o, "near", near_overflow, "    ");
		o += ",\n";
		AppendBucket(o, "non_finite", non_finite, "    ");
		o += "\n  },\n";

		o += "  \"schema\": ";
		AppendU64(o, static_cast<u64>(kSchema));
		o += ",\n";

		o += "  \"summary\": {\n";
		AppendKV(o, "    ", "coverage_per_prim", s.coverage_per_prim);
		o += ",\n";
		AppendNumArray(o, "modes", s.modes.data(), s.modes.size(), "    ");
		o += ",\n";
		AppendKV(o, "    ", "octave_span_p05_p95", s.octave_span_p05_p95);
		o += ",\n";
		AppendKV(o, "    ", "total_coverage", s.total_coverage);
		o += ",\n";
		AppendKV(o, "    ", "valley_depth", s.valley_depth);
		o += ",\n    \"w_percentiles\": {\n";
		AppendKV(o, "      ", "p01", s.p01);
		o += ",\n";
		AppendKV(o, "      ", "p05", s.p05);
		o += ",\n";
		AppendKV(o, "      ", "p10", s.p10);
		o += ",\n";
		AppendKV(o, "      ", "p25", s.p25);
		o += ",\n";
		AppendKV(o, "      ", "p50", s.p50);
		o += ",\n";
		AppendKV(o, "      ", "p75", s.p75);
		o += ",\n";
		AppendKV(o, "      ", "p90", s.p90);
		o += ",\n";
		AppendKV(o, "      ", "p95", s.p95);
		o += ",\n";
		AppendKV(o, "      ", "p99", s.p99);
		o += "\n    }\n  },\n";

		o += "  \"tool\": \"qhist\"\n";
		o += "}\n";
		return o;
	}

	bool DepthHistogram::WriteJson(const std::string& path, std::string* error) const
	{
		const std::string json = ToJson();
		std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "wb");
		if (!fp)
		{
			if (error)
				*error = "could not open '" + path + "' for writing";
			return false;
		}
		const size_t written = std::fwrite(json.data(), 1, json.size(), fp);
		const bool ok = (written == json.size());
		if (std::fclose(fp) != 0 || !ok)
		{
			if (error)
				*error = "short write to '" + path + "'";
			return false;
		}
		return true;
	}
}
