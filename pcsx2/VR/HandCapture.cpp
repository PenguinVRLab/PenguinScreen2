// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HandCapture.h"

#include "GS/GSLocalMemory.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSVertex.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>

namespace VR::HandCapture
{
	Module g_module;

	static_assert(sizeof(GSVertex) == 32, "the hook reads 32-byte GS vertices");

	namespace
	{
		using HandsGltf::HandAsset;
		using HandsGltf::Mat4;
		using HandsGltf::Vec3;

		const char* SideName(int side) { return side == 1 ? "right" : "left"; }

		std::array<double, 3> Normalized(const std::array<double, 3>& a, bool* ok)
		{
			const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
			*ok = l > 1e-12 && std::isfinite(l);
			if (!*ok)
				return {0, 0, 0};
			return {a[0] / l, a[1] / l, a[2] / l};
		}

		struct RestFix
		{
			std::array<std::array<double, 3>, 3> rows{};
			std::array<double, 3> origin{};
			double scale = 1.0;
			Vec3 Apply(const ViewPoint& p) const
			{
				const double d[3] = {p.x - origin[0], p.y - origin[1], p.z - origin[2]};
				Vec3 o;
				o.x = static_cast<float>(scale * (rows[0][0] * d[0] + rows[0][1] * d[1] + rows[0][2] * d[2]));
				o.y = static_cast<float>(scale * (rows[1][0] * d[0] + rows[1][1] * d[1] + rows[1][2] * d[2]));
				o.z = static_cast<float>(scale * (rows[2][0] * d[0] + rows[2][1] * d[1] + rows[2][2] * d[2]));
				return o;
			}
		};

		bool MakeRestFix(const WristFrame& f, double scale, RestFix* out)
		{
			bool ok_x = false, ok_y = false, ok_z = false;
			const std::array<double, 3> x = Normalized(f.x_axis, &ok_x);
			const double dxy = x[0] * f.y_axis[0] + x[1] * f.y_axis[1] + x[2] * f.y_axis[2];
			const std::array<double, 3> y = Normalized({f.y_axis[0] - x[0] * dxy, f.y_axis[1] - x[1] * dxy, f.y_axis[2] - x[2] * dxy}, &ok_y);
			const std::array<double, 3> z = Normalized({x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]}, &ok_z);
			if (!ok_x || !ok_y || !ok_z || !(scale > 0.0) || !std::isfinite(scale))
				return false;
			out->rows = {x, y, z};
			out->origin = f.origin;
			out->scale = scale;
			return true;
		}

		using V3 = std::array<double, 3>;
		[[maybe_unused]] double Dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		V3 Cross3(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
		[[maybe_unused]] double Deg(double c) { return std::acos(std::clamp(c, -1.0, 1.0)) * (180.0 / 3.14159265358979323846); }

		[[maybe_unused]] void SymmetricEigen(std::array<std::array<double, 3>, 3> a, std::array<double, 3>* val, std::array<V3, 3>* vec)
		{
			std::array<std::array<double, 3>, 3> v{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
			for (int sweep = 0; sweep < 64; sweep++)
			{
				const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
				const double diag = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
				if (off <= 1e-30 * diag || off == 0.0)
					break;
				for (int p = 0; p < 2; p++)
					for (int q = p + 1; q < 3; q++)
					{
						if (a[p][q] == 0.0)
							continue;
						const double th = 0.5 * std::atan2(2.0 * a[p][q], a[q][q] - a[p][p]);
						const double c = std::cos(th), sn = std::sin(th);
						for (int k = 0; k < 3; k++)
						{
							const double akp = a[k][p], akq = a[k][q];
							a[k][p] = c * akp - sn * akq;
							a[k][q] = sn * akp + c * akq;
						}
						for (int k = 0; k < 3; k++)
						{
							const double apk = a[p][k], aqk = a[q][k];
							a[p][k] = c * apk - sn * aqk;
							a[q][k] = sn * apk + c * aqk;
						}
						for (int k = 0; k < 3; k++)
						{
							const double vkp = v[k][p], vkq = v[k][q];
							v[k][p] = c * vkp - sn * vkq;
							v[k][q] = sn * vkp + c * vkq;
						}
					}
			}
			std::array<int, 3> order{0, 1, 2};
			std::sort(order.begin(), order.end(), [&a](int i, int j) { return a[i][i] > a[j][j]; });
			for (int i = 0; i < 3; i++)
			{
				(*val)[i] = a[order[i]][order[i]];
				bool ok = false;
				(*vec)[i] = Normalized({v[0][order[i]], v[1][order[i]], v[2][order[i]]}, &ok);
			}
		}

		[[maybe_unused]] bool FitWrist(const WristFrame& hint, BackRank rank, double mpu, double l_ref, const std::vector<u32>& tris,
			const std::vector<ViewPoint>& view, const std::vector<u8>& view_ok, const std::vector<u32>& indices, const char* side, WristFit* out,
			std::string* why)
		{
			double area = 0.0;
			V3 m{0, 0, 0};
			std::array<std::array<double, 3>, 3> s2{};
			bool have_ref = false;
			V3 ref{0, 0, 0};
			u32 faces = 0;
			std::vector<u32> corners;
			for (u32 t : tris)
			{
				const u32 i0 = indices[t * 3], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
				if (!view_ok[i0] || !view_ok[i1] || !view_ok[i2])
					continue;
				corners.insert(corners.end(), {i0, i1, i2});
				if (!have_ref)
				{
					ref = {view[i0].x, view[i0].y, view[i0].z};
					have_ref = true;
				}
				const V3 p[3] = {{view[i0].x - ref[0], view[i0].y - ref[1], view[i0].z - ref[2]},
					{view[i1].x - ref[0], view[i1].y - ref[1], view[i1].z - ref[2]}, {view[i2].x - ref[0], view[i2].y - ref[1], view[i2].z - ref[2]}};
				const V3 u{p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
				const V3 w{p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
				const V3 n = Cross3(u, w);
				const double a = 0.5 * std::sqrt(Dot3(n, n));
				if (!(a > 0.0) || !std::isfinite(a))
					continue;
				faces++;
				area += a;
				for (int i = 0; i < 3; i++)
					m[i] += a * (p[0][i] + p[1][i] + p[2][i]) / 3.0;
				for (int i = 0; i < 3; i++)
					for (int j = 0; j < 3; j++)
						s2[i][j] += a / 12.0 *
									(p[0][i] * p[0][j] + p[1][i] * p[1][j] + p[2][i] * p[2][j] + (p[0][i] + p[1][i] + p[2][i]) * (p[0][j] + p[1][j] + p[2][j]));
			}
			if (faces < 3)
			{
				*why = fmt::format("the {} hand has no long axis to fit a wrist frame ({} kicked faces with an area)", side, faces);
				return false;
			}
			for (int i = 0; i < 3; i++)
				m[i] /= area;
			std::array<std::array<double, 3>, 3> cov{};
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					cov[i][j] = s2[i][j] / area - m[i] * m[j];
			std::array<double, 3> ev{};
			std::array<V3, 3> e{};
			SymmetricEigen(cov, &ev, &e);
			for (int i = 0; i < 3; i++)
				out->spread_m[i] = mpu * std::sqrt(std::max(0.0, ev[i]));
			const std::array<double, 3>& sd = out->spread_m;
			const std::string spreads = fmt::format("spreads {:.4f}/{:.4f}/{:.4f} m", sd[0], sd[1], sd[2]);
			if (!(sd[0] >= 1.25 * sd[1]) || !(sd[0] > 0.0))
			{
				*why = fmt::format("the {} hand has no long axis to fit a wrist frame ({})", side, spreads);
				return false;
			}
			RestFix hf;
			MakeRestFix(hint, 1.0, &hf);
			const V3& hy = hf.rows[1];
			const V3& hz = hf.rows[2];
			V3 d = e[0];
			const double dz = Dot3(d, hz);
			if (std::fabs(dz) < std::sin(15.0 * 3.14159265358979323846 / 180.0))
			{
				*why = fmt::format("the {} hint cannot tell the fingers' end (the long axis is {:.1f} deg from the hint's)", side, Deg(std::fabs(dz)));
				return false;
			}
			if (dz > 0.0)
				d = {-d[0], -d[1], -d[2]};
			double tmin = 0.0, tmax = 0.0;
			bool first = true;
			for (u32 i : corners)
			{
				const double t = (view[i].x - ref[0] - m[0]) * d[0] + (view[i].y - ref[1] - m[1]) * d[1] + (view[i].z - ref[2] - m[2]) * d[2];
				tmin = first ? t : std::min(tmin, t);
				tmax = first ? t : std::max(tmax, t);
				first = false;
			}
			out->length_m = mpu * (tmax - tmin);
			if (!(out->length_m >= 0.05))
			{
				*why = fmt::format("the {} hand is too small to fit a wrist frame ({:.4f} m along its long axis)", side, out->length_m);
				return false;
			}
			if (!(sd[1] >= 1.1 * sd[2]))
			{
				*why = fmt::format("the {} hand's cross-section is round: no palm side to fit ({})", side, spreads);
				return false;
			}
			const V3 z{-d[0], -d[1], -d[2]};
			const double hyz = Dot3(hy, z);
			bool ok = false;
			const V3 yt = Normalized({hy[0] - z[0] * hyz, hy[1] - z[1] * hyz, hy[2] - z[2] * hyz}, &ok);
			const double c2 = Dot3(yt, e[1]), c3 = Dot3(yt, e[2]);
			auto Finish = [&](int k, double c) {
				const double sign = c > 0.0 ? 1.0 : -1.0;
				const V3 y{sign * e[k][0], sign * e[k][1], sign * e[k][2]};
				const V3 x = Cross3(y, z);
				out->palm_axis = static_cast<u8>(k + 1);
				const double back = tmax - l_ref / mpu;
				out->frame.origin = {ref[0] + m[0] + back * d[0], ref[1] + m[1] + back * d[1], ref[2] + m[2] + back * d[2]};
				out->frame.x_axis = x;
				out->frame.y_axis = y;
				out->rule = WristFit::Rule::Fitted;
				return true;
			};
#ifdef VR_HANDCAP_PLANTED_BACK_NEAREST_HINT
			rank = BackRank::Nearest;
#endif
			if (rank == BackRank::Nearest)
			{
				const int k = std::fabs(c2) >= std::fabs(c3) ? 1 : 2;
				out->hint_near_deg = Deg(std::max(std::fabs(c2), std::fabs(c3)));
				out->hint_far_deg = Deg(std::min(std::fabs(c2), std::fabs(c3)));
				if (!ok || std::fabs(hyz) > std::cos(15.0 * 3.14159265358979323846 / 180.0) || out->hint_far_deg - out->hint_near_deg < 5.0)
				{
					*why = fmt::format("the {} hint cannot tell the palm side ({:.1f} deg and {:.1f} deg to the two cross-section axes)", side,
						out->hint_near_deg, out->hint_far_deg);
					return false;
				}
				return Finish(k, k == 1 ? c2 : c3);
			}
			if (!(sd[1] >= kBackRankMinSpreadRatio * sd[2]))
			{
				*why = fmt::format("the {} hand's cross-section spreads cannot tell its middle axis from its thin one (sd2/sd3 {:.3f} < {:.2f}; {})", side,
					sd[2] > 0.0 ? sd[1] / sd[2] : 0.0, kBackRankMinSpreadRatio, spreads);
				return false;
			}
			const int k = rank == BackRank::Middle ? 1 : 2;
			const double ck = k == 1 ? c2 : c3;
			out->hint_near_deg = Deg(std::fabs(ck));
			out->hint_far_deg = Deg(std::fabs(k == 1 ? c3 : c2));
			if (!ok || std::fabs(hyz) > std::cos(15.0 * 3.14159265358979323846 / 180.0) || out->hint_near_deg > kBackSignMaxDeg)
			{
				*why = fmt::format("the {} hint cannot sign the back of the hand (its y is {:.1f} deg from the {} axis)", side, out->hint_near_deg,
					k == 1 ? "middle" : "thin");
				return false;
			}
			return Finish(k, ck);
		}

		std::string WristFitText(const WristFit& f)
		{
			switch (f.rule)
			{
				case WristFit::Rule::Recipe: return "as recipe";
				case WristFit::Rule::Mirror: return "mirror";
				case WristFit::Rule::NotBuilt: return "not built";
				default: break;
			}
			const WristFrame& w = f.frame;
			const V3 z = Cross3(w.x_axis, w.y_axis);
			return fmt::format("origin ({:.6g}, {:.6g}, {:.6g}) fingers ({:.4f}, {:.4f}, {:.4f}) back ({:.4f}, {:.4f}, {:.4f}) length {:.4f} m "
							   "spreads {:.4f}/{:.4f}/{:.4f} m palm e{} hint {:.1f}/{:.1f} deg",
				w.origin[0], w.origin[1], w.origin[2], -z[0], -z[1], -z[2], w.y_axis[0], w.y_axis[1], w.y_axis[2], f.length_m, f.spread_m[0],
				f.spread_m[1], f.spread_m[2], f.palm_axis, f.hint_near_deg, f.hint_far_deg);
		}

		void ComputeNormals(HandAsset* h)
		{
			const size_t n = h->vertex_count;
			std::vector<double> acc(n * 3, 0.0);
			for (size_t t = 0; t + 2 < h->indices.size(); t += 3)
			{
				const u32 i[3] = {h->indices[t], h->indices[t + 1], h->indices[t + 2]};
				const float* a = &h->positions[i[0] * 3];
				const float* b = &h->positions[i[1] * 3];
				const float* c = &h->positions[i[2] * 3];
				const double u[3] = {static_cast<double>(b[0]) - a[0], static_cast<double>(b[1]) - a[1], static_cast<double>(b[2]) - a[2]};
				const double v[3] = {static_cast<double>(c[0]) - a[0], static_cast<double>(c[1]) - a[1], static_cast<double>(c[2]) - a[2]};
				const double fn[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
				for (int k = 0; k < 3; k++)
					for (int e = 0; e < 3; e++)
						acc[i[k] * 3 + e] += fn[e];
			}
			h->normals.assign(n * 3, 0.0f);
			for (size_t v = 0; v < n; v++)
			{
				bool ok = false;
				const std::array<double, 3> nn = Normalized({acc[v * 3], acc[v * 3 + 1], acc[v * 3 + 2]}, &ok);
				h->normals[v * 3 + 0] = ok ? static_cast<float>(nn[0]) : 0.0f;
				h->normals[v * 3 + 1] = ok ? static_cast<float>(nn[1]) : 1.0f;
				h->normals[v * 3 + 2] = ok ? static_cast<float>(nn[2]) : 0.0f;
			}
		}

		[[maybe_unused]] Orientation OrientFaces(const std::vector<u32>& faces, const std::vector<u64>& keys, const std::vector<ViewPoint>& view,
			const std::vector<u32>& indices, std::vector<u8>* flip)
		{
			Orientation o;
			const size_t n = faces.size();
			flip->assign(n, 0);
			auto key = [&](size_t f, int k) { return keys[static_cast<size_t>(faces[f]) * 3 + k]; };
			std::map<std::pair<u64, u64>, std::vector<std::pair<u32, bool>>> edges;
			for (size_t f = 0; f < n; f++)
				for (int k = 0; k < 3; k++)
				{
					const u64 a = key(f, k), b = key(f, (k + 1) % 3);
					if (a != b)
						edges[{std::min(a, b), std::max(a, b)}].push_back({static_cast<u32>(f), a < b});
				}
			std::vector<std::vector<std::pair<u32, bool>>> adj(n);
			for (const auto& [e, uses] : edges)
			{
				if (uses.size() > 2)
					o.nonmanifold++;
				else if (uses.size() == 2)
				{
					const bool same = uses[0].second == uses[1].second;
					adj[uses[0].first].push_back({uses[1].first, same});
					adj[uses[1].first].push_back({uses[0].first, same});
				}
			}
			auto corner = [&](size_t f, int k) -> const ViewPoint& { return view[indices[static_cast<size_t>(faces[f]) * 3 + k]]; };
			auto outward = [&](const std::vector<u32>& piece) {
				double c[3] = {0.0, 0.0, 0.0};
				for (u32 f : piece)
					for (int k = 0; k < 3; k++)
						c[0] += corner(f, k).x, c[1] += corner(f, k).y, c[2] += corner(f, k).z;
				const double m = 3.0 * static_cast<double>(piece.size());
				c[0] /= m, c[1] /= m, c[2] /= m;
				double vol = 0.0, scale = 0.0;
				for (u32 f : piece)
				{
					const ViewPoint& p0 = corner(f, 0);
					const ViewPoint& p1 = corner(f, (*flip)[f] ? 2 : 1);
					const ViewPoint& p2 = corner(f, (*flip)[f] ? 1 : 2);
					const double a[3] = {p0.x - c[0], p0.y - c[1], p0.z - c[2]};
					const double b[3] = {p1.x - c[0], p1.y - c[1], p1.z - c[2]};
					const double d[3] = {p2.x - c[0], p2.y - c[1], p2.z - c[2]};
					const double v = a[0] * (b[1] * d[2] - b[2] * d[1]) + a[1] * (b[2] * d[0] - b[0] * d[2]) + a[2] * (b[0] * d[1] - b[1] * d[0]);
					vol += v;
					scale += std::fabs(v);
				}
				if (vol < 0.0 && -vol > 1e-6 * scale)
					for (u32 f : piece)
						(*flip)[f] ^= 1;
			};
			if (o.nonmanifold == 0)
			{
				std::vector<s8> side(n, -1);
				std::vector<std::vector<u32>> pieces;
				std::vector<u32> stack;
				for (size_t s = 0; s < n; s++)
				{
					if (side[s] >= 0)
						continue;
					side[s] = 0;
					stack.assign(1, static_cast<u32>(s));
					pieces.emplace_back();
					while (!stack.empty())
					{
						const u32 x = stack.back();
						stack.pop_back();
						pieces.back().push_back(x);
						for (const auto& [y, same] : adj[x])
						{
							const s8 want = static_cast<s8>(side[x] ^ (same ? 1 : 0));
							if (side[y] < 0)
							{
								side[y] = want;
								stack.push_back(y);
							}
							else if (side[y] != want)
								o.conflicts++;
						}
					}
				}
				o.conflicts /= 2;
				if (o.conflicts == 0)
				{
					o.rule = OrientRule::Edges;
					o.components = static_cast<u32>(pieces.size());
					for (size_t f = 0; f < n; f++)
						(*flip)[f] = static_cast<u8>(side[f]);
					for (const std::vector<u32>& piece : pieces)
						outward(piece);
				}
			}
			if (o.rule != OrientRule::Edges)
			{
				o.rule = OrientRule::StripParity;
				u32 pos = 0;
				for (size_t f = 0; f < n; f++)
				{
					pos = (f > 0 && key(f - 1, 1) == key(f, 0) && key(f - 1, 2) == key(f, 1)) ? pos + 1 : 0;
					(*flip)[f] = static_cast<u8>(pos & 1);
				}
				std::vector<u32> all(n);
				std::iota(all.begin(), all.end(), 0u);
				outward(all);
			}
			for (u8 f : *flip)
				o.flipped += f;
			return o;
		}

		std::string OrientText(const Orientation& o, bool has)
		{
			if (!has)
				return "not built";
			return fmt::format("{} ({} pieces, {} flipped, {} edges shared by 3+ faces, {} conflicts)", OrientRuleName(o.rule), o.components,
				o.flipped, o.nonmanifold, o.conflicts);
		}

		void CopyJoints(const HandAsset& ref, HandAsset* h)
		{
			h->inverse_bind = ref.inverse_bind;
			h->rest = ref.rest;
#ifdef VR_HANDCAP_PLANTED_IBM_PERTURBED
			h->inverse_bind[HandsGltf::IndexTip].m[12] += 0.01f;
#endif
		}

		void RigidOnWrist(HandAsset* h)
		{
			h->joints.assign(static_cast<size_t>(h->vertex_count) * 4, 0);
			h->weights.assign(static_cast<size_t>(h->vertex_count) * 4, 0.0f);
			for (u32 v = 0; v < h->vertex_count; v++)
			{
				h->joints[v * 4] = static_cast<u8>(HandsGltf::Wrist);
				h->weights[v * 4] = 1.0f;
			}
		}

		HandAsset Mirror(const HandAsset& src, int to_side, const HandAsset& ref)
		{
			HandAsset h = src;
			h.right = (to_side == 1);
			for (u32 v = 0; v < h.vertex_count; v++)
			{
				h.positions[v * 3] = -h.positions[v * 3];
				h.normals[v * 3] = -h.normals[v * 3];
			}
			for (size_t t = 0; t + 2 < h.indices.size(); t += 3)
				std::swap(h.indices[t + 1], h.indices[t + 2]);
			CopyJoints(ref, &h);
			return h;
		}
	}

	bool Unproject(double X, double Y, double ofx, double ofy, double q, const Projection& p, ViewPoint* out)
	{
		if (!(q > 0.0) || !std::isfinite(q) || !(p.fx != 0.0) || !(p.fy != 0.0))
			return false;
		const double x_px = (X - ofx) / 16.0;
		const double y_px = (Y - ofy) / 16.0;
#ifdef VR_HANDCAP_PLANTED_Q_AS_W
		const double w = q;
#else
		const double w = p.k / q;
#endif
		out->x = (x_px - p.cx) * w / p.fx;
		out->y = (y_px - p.cy) * w / p.fy;
		out->z = w;
		return std::isfinite(out->x) && std::isfinite(out->y) && std::isfinite(out->z);
	}

	double SrgbToLinear(double c255)
	{
		const double c = std::clamp(c255 / 255.0, 0.0, 1.0);
		return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
	}

	u32 PartitionObjects(const std::vector<u64>& keys, std::vector<u32>* object, std::unordered_map<u64, u32>* key_object)
	{
		const size_t nt = keys.size() / 3;
		object->assign(nt, 0);
		std::vector<u32> parent(nt);
		std::iota(parent.begin(), parent.end(), 0u);
		auto find = [&parent](u32 x) {
			while (parent[x] != x)
			{
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		};
		std::unordered_map<u64, u32> first;
		first.reserve(keys.size());
		for (u32 t = 0; t < nt; t++)
		{
			for (int k = 0; k < 3; k++)
			{
				const auto [it, inserted] = first.try_emplace(keys[t * 3 + k], t);
				if (!inserted)
				{
					const u32 a = find(t), b = find(it->second);
					if (a != b)
						parent[std::max(a, b)] = std::min(a, b);
				}
			}
		}
#ifdef VR_HANDCAP_PLANTED_NO_PARTITION
		std::fill(parent.begin(), parent.end(), 0u);
#endif
		std::vector<u32> label(nt, 0xFFFFFFFFu);
		u32 n = 0;
		for (u32 t = 0; t < nt; t++)
		{
			const u32 root = find(t);
			if (label[root] == 0xFFFFFFFFu)
				label[root] = n++;
			(*object)[t] = label[root];
		}
		if (key_object)
		{
			key_object->clear();
			key_object->reserve(first.size());
			for (const auto& [key, t] : first)
				key_object->emplace(key, (*object)[t]);
		}
		return n;
	}

	u32 OpenEdges(const std::vector<u64>& keys, const std::vector<u32>& triangles)
	{
		struct EdgeHash
		{
			size_t operator()(const std::pair<u64, u64>& e) const { return std::hash<u64>()(e.first * 0x9E3779B97F4A7C15ull ^ e.second); }
		};
		std::unordered_map<std::pair<u64, u64>, u32, EdgeHash> uses;
		uses.reserve(triangles.size() * 3);
		for (u32 t : triangles)
		{
			for (int k = 0; k < 3; k++)
			{
				const u64 a = keys[t * 3 + k], b = keys[t * 3 + (k + 1) % 3];
				if (a != b)
					uses[{std::min(a, b), std::max(a, b)}]++;
			}
		}
		u32 open = 0;
		for (const auto& [e, n] : uses)
			open += (n == 1) ? 1u : 0u;
		return open;
	}

	std::array<float, 3> VertexColourFactor(const std::array<u8, 4>& cv, u32 tfx)
	{
		if (tfx == 1)
			return {1.0f, 1.0f, 1.0f};
		std::array<float, 3> out{};
		for (int c = 0; c < 3; c++)
		{
			const double f = cv[c] / 128.0;
			out[c] = static_cast<float>(f <= 1.0 ? SrgbToLinear(f * 255.0) : std::pow(f, 2.2));
		}
		return out;
	}

	bool ValidateRecipe(const Recipe& r, std::string* why)
	{
		auto fail = [why](std::string s) {
			if (why)
				*why = std::move(s);
			return false;
		};
		if (r.tw > 10 || r.th > 10)
			return fail("selector.texture tw/th are log2 sizes, at most 10");
		if (!(r.w_max > r.w_min) || !(r.w_min > 0.0))
			return fail("selector.w_range must be [lo, hi] with 0 < lo < hi");
		if (r.stable_frames == 0)
			return fail("stable_frames must be at least 1");
		if (!(r.max_wait_s > 0.0f))
			return fail("max_wait_s must be positive");
		if (!(r.max_motion_px >= 0.0) || !std::isfinite(r.max_motion_px))
			return fail("max_motion_px must be a positive number of pixels");
		if (r.has_uv_box && (!(r.uv_box[2] > r.uv_box[0]) || !(r.uv_box[3] > r.uv_box[1])))
			return fail("selector.object.uv_box must be [u0, v0, u1, v1] with u0 < u1 and v0 < v1");
		if (!(r.uv_min_coverage >= 0.0) || r.uv_min_coverage > 1.0)
			return fail("selector.object.uv_min_coverage must lie in [0, 1]");
		if (r.vertices_min > r.vertices_max || r.triangles_min > r.triangles_max || r.open_edges_min > r.open_edges_max ||
			r.vertices_max == 0 || r.triangles_max == 0)
			return fail("fingerprint ranges must be [lo, hi] with hi > 0");
		if ((r.texture_w || r.texture_h) && (r.texture_w != (1u << r.tw) || r.texture_h != (1u << r.th)))
			return fail("fingerprint.texture must be the selector's texture size");
		if (!(r.projection.fx != 0.0) || !(r.projection.fy != 0.0) || !(r.projection.k > 0.0))
			return fail("projection needs non-zero fx, fy and a positive k");
		if (!r.has_side[0] && !r.has_side[1])
			return fail("side needs a left or a right box");
		if (r.other_side == OtherSide::Capture && !(r.has_side[0] && r.has_side[1]))
			return fail("other_side: capture needs both side boxes");
		if (r.other_side == OtherSide::Mirror && r.has_side[0] && r.has_side[1])
			return fail("other_side: mirror needs exactly one side box");
		for (int s = 0; s < 2; s++)
		{
			RestFix rf;
			if (r.has_side[s] && !MakeRestFix(r.wrist[s], r.metres_per_unit, &rf))
				return fail(fmt::format("rest_fix: the {} wrist frame or metres_per_unit is degenerate", SideName(s)));
		}
		return true;
	}

	bool CheckHandAsset(const HandAsset& h, std::string* why)
	{
		auto fail = [why](std::string s) {
			if (why)
				*why = std::move(s);
			return false;
		};
		const size_t n = h.vertex_count;
		if (n == 0 || h.positions.size() != n * 3 || h.normals.size() != n * 3 || h.colors.size() != n * 3 ||
			h.joints.size() != n * 4 || h.weights.size() != n * 4)
			return fail("the vertex attributes disagree on the vertex count");
		if (!h.uvs.empty() && (h.uvs.size() != n * 2 || !h.texture.Valid()))
			return fail("texture coordinates without a whole texture");
		if (h.indices.empty() || h.indices.size() % 3 != 0 || h.indices.size() / 3 > HandsGltf::kMaxTriangles)
			return fail("the index count is not whole triangles within the budget");
		for (u32 i : h.indices)
			if (i >= n)
				return fail("an index is out of range");
		for (size_t v = 0; v < n; v++)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
			{
				if (h.joints[v * 4 + k] >= HandsGltf::kJointCount)
					return fail("a vertex names a joint outside the 26");
				const float w = h.weights[v * 4 + k];
				if (!(w >= 0.0f))
					return fail("a negative or undefined weight");
				sum += w;
			}
			if (std::fabs(sum - 1.0f) > 1e-4f)
				return fail("a vertex's weights do not sum to 1");
			for (int e = 0; e < 3; e++)
				if (!std::isfinite(h.positions[v * 3 + e]) || !std::isfinite(h.normals[v * 3 + e]) || !std::isfinite(h.colors[v * 3 + e]))
					return fail("a vertex attribute is not finite");
		}
		for (int j = 0; j < HandsGltf::kJointCount; j++)
		{
			Mat4 inv;
			if (!h.inverse_bind[j].Inverse(&inv))
				return fail(fmt::format("a singular inverse bind matrix: {}", HandsGltf::JointName(j)));
			const Vec3 t = inv.Column(3);
			const Vec3& r = h.rest[j];
			const float tol = 1e-4f * (1.0f + std::max({std::fabs(r.x), std::fabs(r.y), std::fabs(r.z)}));
			if (std::fabs(t.x - r.x) > tol || std::fabs(t.y - r.y) > tol || std::fabs(t.z - r.z) > tol)
				return fail(fmt::format("an inverse bind matrix is not its joint's rest point: {}", HandsGltf::JointName(j)));
		}
		return true;
	}

	bool IsIdentityHint(const WristFrame& f)
	{
		return f.x_axis == std::array<double, 3>{1.0, 0.0, 0.0} && f.y_axis == std::array<double, 3>{0.0, 1.0, 0.0};
	}

	const char* OrientRuleName(OrientRule r)
	{
		switch (r)
		{
			case OrientRule::Edges: return "edges";
			case OrientRule::StripParity: return "strip-parity";
			case OrientRule::Mirror: return "mirror";
			default: return "none";
		}
	}

	BuildResult BuildHands(const Recipe& recipe, const Capture& cap, const std::array<const HandAsset*, 2>& reference)
	{
		BuildResult res;
		auto refuse = [&res](std::string s) {
			res.ok = false;
			res.reason = std::move(s);
			return res;
		};
		std::string why;
		if (!ValidateRecipe(recipe, &why))
			return refuse("the recipe is inconsistent: " + why);
		const size_t nv = cap.vertices.size();
		const size_t nt = cap.indices.size() / 3;
		if (nt == 0 || cap.indices.size() % 3 != 0)
			return refuse("no triangles were captured");
		if (!cap.texture.Valid())
			return refuse("the texture was not decoded");
		if (!cap.flags.empty() && cap.flags.size() != nt)
			return refuse("the capture's triangle flags do not match its triangles");
		const auto built = [&cap](size_t t) { return cap.flags.empty() || (cap.flags[t] & kFaceDrawn) != 0; };

		std::vector<ViewPoint> view(nv);
		std::vector<u8> view_ok(nv, 0);
		std::vector<u64> keys(nt * 3);
		for (size_t i = 0; i < nv; i++)
		{
			const CapturedVertex& v = cap.vertices[i];
			view_ok[i] = Unproject(v.x, v.y, v.ofx, v.ofy, v.q, recipe.projection, &view[i]) ? 1 : 0;
		}
		for (size_t i = 0; i < nt * 3; i++)
		{
			if (cap.indices[i] >= nv)
				return refuse("a captured index is out of range");
			if (!view_ok[cap.indices[i]] && built(i / 3))
				return refuse("a vertex has no usable Q");
			const CapturedVertex& v = cap.vertices[cap.indices[i]];
			keys[i] = PositionKey(v.x, v.y, v.z);
		}

		std::vector<u32> object;
		const u32 nobj = PartitionObjects(keys, &object);
		std::vector<std::array<double, 4>> centroid(nobj, {0.0, 0.0, 0.0, 0.0});
		std::vector<std::array<double, 4>> centroid_all(nobj, {0.0, 0.0, 0.0, 0.0});
		auto tri_centroid = [&](size_t t) {
			const ViewPoint& a = view[cap.indices[t * 3]];
			const ViewPoint& b = view[cap.indices[t * 3 + 1]];
			const ViewPoint& c = view[cap.indices[t * 3 + 2]];
			return std::array<double, 3>{(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0, (a.z + b.z + c.z) / 3.0};
		};
		for (size_t t = 0; t < nt; t++)
		{
			if (!view_ok[cap.indices[t * 3]] || !view_ok[cap.indices[t * 3 + 1]] || !view_ok[cap.indices[t * 3 + 2]])
				continue;
			const std::array<double, 3> c = tri_centroid(t);
			std::array<double, 4>& a = centroid_all[object[t]];
			a = {a[0] + c[0], a[1] + c[1], a[2] + c[2], a[3] + 1.0};
			if (!built(t))
				continue;
			std::array<double, 4>& o = centroid[object[t]];
			o = {o[0] + c[0], o[1] + c[1], o[2] + c[2], o[3] + 1.0};
		}
		for (u32 o = 0; o < nobj; o++)
			if (centroid[o][3] == 0.0)
				centroid[o] = centroid_all[o];
		std::vector<int> object_side(nobj, -1);
		for (u32 o = 0; o < nobj; o++)
		{
			const double cx = centroid[o][0] / centroid[o][3], cy = centroid[o][1] / centroid[o][3], cz = centroid[o][2] / centroid[o][3];
			for (int s = 0; s < 2 && object_side[o] < 0; s++)
			{
				if (!recipe.has_side[s])
					continue;
				const bool in = (recipe.side_rule == SideRule::CentroidX) ? ((s == 0) == (cx < 0.0)) : recipe.side_box[s].Contains(cx, cy, cz);
				if (in)
					object_side[o] = s;
			}
		}

		std::vector<int> tri_side(nt, -1);
		for (size_t t = 0; t < nt; t++)
			tri_side[t] = object_side[object[t]];
#ifdef VR_HANDCAP_PLANTED_SIDE_FROM_UV
		{
			std::map<std::array<u64, 3>, int> first;
			for (size_t t = 0; t < nt; t++)
			{
				std::array<u64, 3> k;
				for (int i = 0; i < 3; i++)
				{
					const CapturedVertex& v = cap.vertices[cap.indices[t * 3 + i]];
					const float u = v.s / v.q, w = v.t / v.q;
					u32 a, b;
					std::memcpy(&a, &u, 4);
					std::memcpy(&b, &w, 4);
					k[i] = static_cast<u64>(a) | (static_cast<u64>(b) << 32);
				}
				std::sort(k.begin(), k.end());
				const auto [it, inserted] = first.try_emplace(k, tri_side[t]);
				tri_side[t] = it->second;
			}
		}
#endif
		std::array<std::vector<u32>, 2> side_tris;
		for (size_t t = 0; t < nt; t++)
			if (tri_side[t] >= 0)
				side_tris[tri_side[t]].push_back(static_cast<u32>(t));
		for (int s = 0; s < 2; s++)
		{
			if (!recipe.has_side[s])
				continue;
			std::vector<bool> used(nv, false);
			u32 sv = 0;
			for (u32 t : side_tris[s])
				for (int k = 0; k < 3; k++)
					if (!used[cap.indices[t * 3 + k]])
					{
						used[cap.indices[t * 3 + k]] = true;
						sv++;
					}
			const u32 st = static_cast<u32>(side_tris[s].size());
#ifndef VR_HANDCAP_PLANTED_NO_FINGERPRINT
			if (sv < recipe.vertices_min || sv > recipe.vertices_max)
				return refuse(fmt::format("fingerprint mismatch (vertices {}, want {}..{}) on the {} side", sv, recipe.vertices_min,
					recipe.vertices_max, SideName(s)));
			if (st < recipe.triangles_min || st > recipe.triangles_max)
				return refuse(fmt::format("fingerprint mismatch (triangles {}, want {}..{}) on the {} side", st, recipe.triangles_min,
					recipe.triangles_max, SideName(s)));
			if (recipe.texture_w && (cap.texture.width != recipe.texture_w || cap.texture.height != recipe.texture_h))
				return refuse(fmt::format("fingerprint mismatch (texture {}x{}, want {}x{})", cap.texture.width, cap.texture.height,
					recipe.texture_w, recipe.texture_h));
#endif
#ifndef VR_HANDCAP_PLANTED_NO_OPEN_EDGE_CHECK
			const u32 open = OpenEdges(keys, side_tris[s]);
			if (open < recipe.open_edges_min || open > recipe.open_edges_max)
				return refuse(fmt::format("the {} hand is not whole (open edges {}, want {}..{})", SideName(s), open, recipe.open_edges_min,
					recipe.open_edges_max));
#endif
		}

		std::array<std::vector<u32>, 2> tris;
		for (int s = 0; s < 2; s++)
		{
			for (u32 t : side_tris[s])
			{
				if (!built(t))
					continue;
#ifndef VR_HANDCAP_PLANTED_NO_TRIM
				const std::array<double, 3> c = tri_centroid(t);
				if (recipe.has_trim && recipe.trim_box.Contains(c[0], c[1], c[2]))
					continue;
#endif
				tris[s].push_back(t);
			}
		}

		for (int s = 0; s < 2; s++)
		{
			if (!recipe.has_side[s])
				continue;
			if (tris[s].empty())
				return refuse(fmt::format("the {} box holds no triangles", SideName(s)));
			if (!reference[s])
				return refuse(fmt::format("no reference hand for the {} side", SideName(s)));
			WristFit& fit = res.wrist[s];
			fit.rule = WristFit::Rule::Recipe;
			fit.frame = recipe.wrist[s];
#ifndef VR_HANDCAP_PLANTED_WRIST_RECIPE
			if (!IsIdentityHint(recipe.wrist[s]))
			{
				const HandsGltf::Vec3 tip = reference[s]->rest[HandsGltf::MiddleTip], wr = reference[s]->rest[HandsGltf::Wrist];
				const double l_ref = std::sqrt(static_cast<double>(tip.x - wr.x) * (tip.x - wr.x) + static_cast<double>(tip.y - wr.y) * (tip.y - wr.y) +
											   static_cast<double>(tip.z - wr.z) * (tip.z - wr.z));
				if (!FitWrist(recipe.wrist[s], recipe.back[s], recipe.metres_per_unit, l_ref, tris[s], view, view_ok, cap.indices, SideName(s), &fit, &why))
				{
					res.fit_refused = true;
					return refuse(why);
				}
			}
#endif
			RestFix rf;
			if (!MakeRestFix(fit.frame, recipe.metres_per_unit, &rf))
				return refuse(fmt::format("the fitted {} wrist frame is degenerate", SideName(s)));
			HandAsset& h = res.hands[s];
			h.right = (s == 1);
			h.double_sided = true;
			std::vector<u8> flip;
#ifndef VR_HANDCAP_PLANTED_NO_ORIENT
			res.orient[s] = OrientFaces(tris[s], keys, view, cap.indices, &flip);
#else
			flip.assign(tris[s].size(), 0);
#endif
			std::vector<s32> remap(nv, -1);
			for (size_t f = 0; f < tris[s].size(); f++)
			{
				const u32 t = tris[s][f];
				for (int k = 0; k < 3; k++)
				{
					const u32 src = cap.indices[t * 3 + k];
					if (remap[src] < 0)
					{
						remap[src] = static_cast<s32>(h.vertex_count++);
						const Vec3 p = rf.Apply(view[src]);
						h.positions.insert(h.positions.end(), {p.x, p.y, p.z});
						const CapturedVertex& cv = cap.vertices[src];
						const std::array<float, 3> f = VertexColourFactor(cv.rgba, cap.tfx);
						h.colors.insert(h.colors.end(), {f[0], f[1], f[2]});
#ifdef VR_HANDCAP_PLANTED_UVSWAP
						h.uvs.insert(h.uvs.end(), {cv.t / cv.q, cv.s / cv.q});
#else
						h.uvs.insert(h.uvs.end(), {cv.s / cv.q, cv.t / cv.q});
#endif
					}
				}
				const int order[3] = {0, flip[f] ? 2 : 1, flip[f] ? 1 : 2};
				for (int k : order)
					h.indices.push_back(static_cast<u32>(remap[cap.indices[t * 3 + k]]));
			}
			h.texture = cap.texture;
			ComputeNormals(&h);
			RigidOnWrist(&h);
			CopyJoints(*reference[s], &h);
			res.has[s] = true;
		}

		if (recipe.other_side == OtherSide::Mirror)
		{
			const int from = recipe.has_side[0] ? 0 : 1;
			const int to = 1 - from;
			if (!reference[to])
				return refuse(fmt::format("no reference hand for the {} side", SideName(to)));
			res.hands[to] = Mirror(res.hands[from], to, *reference[to]);
			res.has[to] = true;
			res.orient[to] = res.orient[from];
			res.orient[to].rule = OrientRule::Mirror;
			res.wrist[to].rule = WristFit::Rule::Mirror;
		}

		for (int s = 0; s < 2; s++)
		{
			if (res.has[s] && !CheckHandAsset(res.hands[s], &why))
				return refuse(fmt::format("the built {} hand breaks the loader rules: {}", SideName(s), why));
		}
		res.ok = true;
		return res;
	}

	Module::Module() = default;

	Module::~Module()
	{
		WaitForBuild();
	}

	void Module::SetState(State s)
	{
		m_state.store(s, std::memory_order_release);
	}

	void Module::Configure(const Recipe* recipe)
	{
		std::lock_guard lock(m_cfg_mutex);
		const bool same = recipe ? (m_pending && *m_pending == *recipe) : !m_pending;
		if (same)
			return;
		m_pending = recipe ? std::make_unique<Recipe>(*recipe) : nullptr;
		m_fit_retries = 0;
		m_cfg_gen.fetch_add(1, std::memory_order_acq_rel);
		{
			std::lock_guard pub(m_pub_mutex);
			m_pub = {};
		}
		m_pub_gen.fetch_add(1, std::memory_order_acq_rel);
		m_has_recipe.store(recipe != nullptr, std::memory_order_release);
		if (!recipe)
		{
			SetState(State::Off);
			m_active.store(false, std::memory_order_release);
			return;
		}
		std::string why;
		if (!ValidateRecipe(*recipe, &why) || recipe->gate == Gate::HeadlookArmed)
		{
			if (why.empty())
				why = "the recipe's gate has no provider in this build";
			Console.WarningFmt("(VR) HandCapture: refused: {}. The glove stays.", why);
			SetState(State::Refused);
			m_active.store(false, std::memory_order_release);
			return;
		}
		Console.WriteLnFmt("(VR) HandCapture: armed for {}.", recipe->serial);
		SetState(State::Watching);
		m_active.store(true, std::memory_order_release);
	}

	void Module::SetReferenceHands(const HandAsset& left, const HandAsset& right)
	{
		std::lock_guard lock(m_cfg_mutex);
		m_reference[0] = std::make_shared<const HandAsset>(left);
		m_reference[1] = std::make_shared<const HandAsset>(right);
	}

	std::shared_ptr<const HandAsset> Module::Published(int side) const
	{
		std::lock_guard lock(m_pub_mutex);
		return m_pub[side & 1];
	}

	void Module::WaitForBuild()
	{
		if (m_worker.joinable())
			m_worker.join();
	}

	void Module::SyncConfig()
	{
		const u32 g = m_cfg_gen.load(std::memory_order_acquire);
		if (g == m_seen_gen)
			return;
		std::lock_guard lock(m_cfg_mutex);
		m_seen_gen = m_cfg_gen.load(std::memory_order_acquire);
		m_have_recipe = (m_pending != nullptr);
		if (m_pending)
			m_recipe = *m_pending;
		m_watch_start = std::chrono::steady_clock::now();
		m_cur_frame = -1;
		m_cur = {};
		m_cur_matched = false;
		m_last_frame = -2;
		m_last = {};
		m_stable = 0;
		m_grab_frame = -1;
		m_capture.reset();
		m_tap.clear(), m_tap_flags.clear();
		m_tap_frame = -1;
		m_slot_drawn.clear();
		m_hide_cells.clear();
		m_hide_uv.clear();
		m_hide_tris.clear();
		const u64 resets = m_watch.resets + 1;
		m_watch = {};
		m_watch.resets = resets;
		m_cur_tex = m_cur_batch = false;
		m_watch_prev_seen = -1;
		m_watch_next_s = 5.0;
		m_run_prev_seen = -1;
		m_retry_reset.store(false, std::memory_order_release);
		m_retry_hold = 0;
		m_cur_shapes.clear();
		m_last_shapes.clear();
		m_toggle_reset.store(false, std::memory_order_release);
	}

	void Module::ResetRunForRetry(s64 hold)
	{
		m_watch_start = std::chrono::steady_clock::now();
		m_cur_frame = -1;
		m_cur = {};
		m_cur_matched = false;
		m_last_frame = -2;
		m_last = {};
		m_stable = 0;
		m_grab_frame = -1;
		m_capture.reset();
		m_cur_tex = m_cur_batch = false;
		m_watch_prev_seen = -1;
		m_watch_next_s = 5.0;
		m_run_prev_seen = -1;
		m_retry_hold = hold;
		m_cur_shapes.clear();
		m_last_shapes.clear();
	}

	void Module::ResetRunOnToggle()
	{
		if (m_grab_frame >= 0)
			m_watch.grab_aborts++;
		m_grab_frame = -1;
		m_capture.reset();
		m_cur_frame = -1;
		m_cur = {};
		m_cur_matched = false;
		m_cur_tex = m_cur_batch = false;
		m_last_frame = -2;
		m_last = {};
		m_stable = 0;
		m_run_prev_seen = -1;
		m_cur_shapes.clear();
		m_last_shapes.clear();
		m_watch.toggle_resets++;
	}

	double Module::ShapeMotion(const std::vector<GroupShape>& a, const std::vector<GroupShape>& b)
	{
		if (a.size() != b.size() || a.empty())
			return std::numeric_limits<double>::infinity();
		double m = 0.0;
		for (size_t i = 0; i < a.size(); i++)
		{
			m = std::max(m, std::hypot(a[i].cx - b[i].cx, a[i].cy - b[i].cy));
			m = std::max({m, std::abs(a[i].x0 - b[i].x0), std::abs(a[i].y0 - b[i].y0), std::abs(a[i].x1 - b[i].x1),
				std::abs(a[i].y1 - b[i].y1)});
		}
		return m;
	}

	std::string WatchStatusText(const WatchStatus& s, const char* at, double t, u32 need, bool ref, State state)
	{
		static constexpr const char* kState[] = {"off", "watching", "building", "ready", "refused", "timed_out"};
		auto gaps = [](const WatchStatus::Gaps& g) {
			return fmt::format("1:{},2:{},3:{},4+:{},back:{}", g[0], g[1], g[2], g[3], g[4]);
		};
		auto brk = [](const WatchStatus::Break& b) {
			if (!b.set)
				return std::string("none");
			return fmt::format("f{}->{},o{}->{},t{}->{},d{}->{}", b.from, b.to, b.objects_from, b.objects_to, b.triangles_from,
				b.triangles_to, b.draws_from, b.draws_to);
		};
		const int st = static_cast<int>(state);
		return fmt::format("at={} t={:.1f}s frames={} fgaps={} tex={} batch={} hands={} tap={}/{} ref={} noref={} gaps={} "
						   "streak={}/{} need={} breaks=gap:{},value:{} last_gap={} last_value={} last={}/{}/{} grabs={}/{} "
						   "resets={} state={}",
			at, t, s.frames, gaps(s.seen_gaps), s.tex_frames, s.batch_frames, s.hand_frames, s.batches_tap, s.batches, ref ? 1 : 0,
			s.stable_no_ref, gaps(s.gaps), s.streak, s.longest, need, s.breaks_gap, s.breaks_value, brk(s.last_gap),
			brk(s.last_value), s.last_objects, s.last_triangles, s.last_draws, s.grab_starts, s.grab_aborts, s.resets,
			(st >= 0 && st < 6) ? kState[st] : "?");
	}

	bool Module::HaveReference() const
	{
		std::lock_guard lock(m_cfg_mutex);
		return m_reference[0] && m_reference[1];
	}

	void Module::WatchStatusLine(const char* at, double t, bool ref)
	{
		Console.WriteLnFmt("(VR) HandCapture: watching: {}", WatchStatusText(m_watch, at, t, m_recipe.stable_frames, ref, GetState()));
	}

	bool Module::TextureMatches(u64 raw) const
	{
		GIFRegTEX0 tex0;
		tex0.U64 = raw;
#ifndef VR_HANDCAP_PLANTED_SELECTOR_NO_TBP0
		if (tex0.TBP0 != m_recipe.tbp0)
			return false;
#endif
		if (tex0.PSM != m_recipe.psm || tex0.TW != m_recipe.tw || tex0.TH != m_recipe.th)
			return false;
		if (m_recipe.has_clut && (tex0.CBP != m_recipe.cbp || tex0.CPSM != m_recipe.cpsm))
			return false;
		return true;
	}

	bool Module::BatchMatches(const GsDraw& d) const
	{
		if (d.prim < 3 || d.prim > 5 || !d.tme || d.fst || d.index_count < 3 || !d.vertices || !d.indices)
			return false;
		if (!TextureMatches(d.tex0))
			return false;
		for (u32 i = 0; i < d.index_count; i++)
			if (d.indices[i] >= d.vertex_count)
				return false;
		return true;
	}

	void Module::OnKick(const void* v0, const void* v1, const void* v2, u32 adc, u64 tex0, s64 frame)
	{
#ifndef VR_HANDCAP_PLANTED_TAP_BYPASSED
		SyncConfig();
		if (!m_have_recipe || !TextureMatches(tex0))
		{
			if (!m_tap.empty())
				m_tap.clear(), m_tap_flags.clear();
			return;
		}
		const GSVertex* a = static_cast<const GSVertex*>(v0);
		const GSVertex* b = static_cast<const GSVertex*>(v1);
		const GSVertex* c = static_cast<const GSVertex*>(v2);
		auto same_xy = [](const GSVertex* p, const GSVertex* q) { return p->XYZ.X == q->XYZ.X && p->XYZ.Y == q->XYZ.Y; };
		u8 flags = adc ? kFaceAdc : 0;
		if (same_xy(a, b) || same_xy(b, c) || same_xy(a, c))
		{
			if (!adc && (m_replay_log.load(std::memory_order_relaxed) || m_trace.load(std::memory_order_relaxed)))
				m_rlog_degenerate++;
			flags |= kFaceZeroArea;
		}
		if (frame != m_tap_frame || tex0 != m_tap_tex0)
		{
			m_tap.clear(), m_tap_flags.clear();
			m_tap_frame = frame;
			m_tap_tex0 = tex0;
		}
		m_tap.push_back(*static_cast<const GSVertex*>(v0));
		m_tap.push_back(*static_cast<const GSVertex*>(v1));
		m_tap.push_back(*static_cast<const GSVertex*>(v2));
		m_tap_flags.push_back(flags);
		if (!flags)
			m_stats.kicks_tapped++;
		m_trace_tap = (flags & kFaceZeroArea) ? 2 : 1;
#else
		(void)v0, (void)v1, (void)v2, (void)adc, (void)tex0, (void)frame;
#endif
	}

	bool Module::FindHands(const GsDraw& d, bool* from_tap)
	{
		const GSVertex* v = static_cast<const GSVertex*>(d.vertices);
		const u32 kept = d.index_count / 3;
		m_src.clear();
		*from_tap = !m_tap.empty() && m_tap_tex0 == d.tex0 && m_tap.size() / 3 >= kept && m_tap_flags.size() * 3 == m_tap.size();
		if (*from_tap)
		{
			for (size_t t = 0; t + 2 < m_tap.size(); t += 3)
				m_src.push_back({&m_tap[t], &m_tap[t + 1], &m_tap[t + 2]});
			m_src_flags = m_tap_flags;
		}
		else
		{
			for (u32 t = 0; t < kept; t++)
				m_src.push_back({&v[d.indices[t * 3]], &v[d.indices[t * 3 + 1]], &v[d.indices[t * 3 + 2]]});
			m_src_flags.assign(kept, 0);
		}
		m_keys.resize(m_src.size() * 3);
		for (size_t t = 0; t < m_src.size(); t++)
			for (int k = 0; k < 3; k++)
				m_keys[t * 3 + k] = PositionKey(m_src[t][k]->XYZ.X, m_src[t][k]->XYZ.Y, m_src[t][k]->XYZ.Z);
		const u32 nobj = PartitionObjects(m_keys, &m_obj, *from_tap ? &m_key_obj : nullptr);
		if (*from_tap)
		{
			for (u32 t = 0; t < kept; t++)
			{
				const GSVertex& a = v[d.indices[t * 3]];
				if (m_key_obj.find(PositionKey(a.XYZ.X, a.XYZ.Y, a.XYZ.Z)) == m_key_obj.end())
				{
					m_tap.clear(), m_tap_flags.clear();
					return FindHands(d, from_tap);
				}
			}
		}

		struct Acc
		{
			bool ok = true;
			bool drawn = false;
			double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
		};
		std::vector<Acc> acc(nobj);
		for (size_t t = 0; t < m_src.size(); t++)
		{
			if (m_src_flags[t])
				continue;
			Acc& o = acc[m_obj[t]];
			o.drawn = true;
			if (!o.ok)
				continue;
			for (int k = 0; k < 3; k++)
			{
				const GSVertex& g = *m_src[t][k];
				const float q = g.RGBAQ.Q;
				if (!(q > 0.0f) || !std::isfinite(q))
				{
					o.ok = false;
					break;
				}
				bool tested = true;
#ifndef VR_HANDCAP_PLANTED_W_ALL_VERTICES
				if (d.has_scissor)
				{
					const double px = (static_cast<double>(g.XYZ.X) - d.ofx) / 16.0, py = (static_cast<double>(g.XYZ.Y) - d.ofy) / 16.0;
					tested = px >= d.sc_x0 && px < d.sc_x1 + 1.0 && py >= d.sc_y0 && py < d.sc_y1 + 1.0;
				}
#endif
				const double w = m_recipe.projection.k / q;
				if (tested && (w < m_recipe.w_min || w > m_recipe.w_max))
				{
					o.ok = false;
					break;
				}
				if (m_recipe.has_uv_box)
				{
					const double u = g.ST.S / q, vv = g.ST.T / q;
					if (!(u >= m_recipe.uv_box[0] && u <= m_recipe.uv_box[2] && vv >= m_recipe.uv_box[1] && vv <= m_recipe.uv_box[3]))
					{
						o.ok = false;
						break;
					}
					o.u0 = std::min(o.u0, u);
					o.v0 = std::min(o.v0, vv);
					o.u1 = std::max(o.u1, u);
					o.v1 = std::max(o.v1, vv);
				}
			}
		}
		m_obj_hand.assign(nobj, 0);
		m_hand_objects = 0;
		for (u32 o = 0; o < nobj; o++)
		{
			bool hand = acc[o].ok && acc[o].drawn;
			if (hand && m_recipe.has_uv_box)
			{
				const double box = (m_recipe.uv_box[2] - m_recipe.uv_box[0]) * (m_recipe.uv_box[3] - m_recipe.uv_box[1]);
				const double cover = std::max(0.0, acc[o].u1 - acc[o].u0) * std::max(0.0, acc[o].v1 - acc[o].v0);
				hand = cover >= m_recipe.uv_min_coverage * box;
			}
			m_obj_hand[o] = hand ? 1 : 0;
			m_hand_objects += hand ? 1u : 0u;
		}
		m_hand_triangles = 0;
		m_hand_drawn = 0;
		for (size_t t = 0; t < m_src.size(); t++)
		{
			m_hand_triangles += m_obj_hand[m_obj[t]];
			m_hand_drawn += m_obj_hand[m_obj[t]] && !m_src_flags[t];
		}
		m_find_calls++;
		m_find_from_tap = *from_tap;
		return m_hand_objects > 0;
	}

	namespace
	{
		constexpr double kUvCell = 1e-4;
		constexpr double kUvTol = 5e-5;
		constexpr u32 kNoUv = 0xFFFFFFFFu, kUvUnset = 0xFFFFFFFEu;
		u64 UvCellKey(s64 cx, s64 cy) { return static_cast<u64>(static_cast<u32>(cx)) | (static_cast<u64>(static_cast<u32>(cy)) << 32); }
		u64 UvTriKey(u32 a, u32 b, u32 c)
		{
			if (a > b)
				std::swap(a, b);
			if (b > c)
				std::swap(b, c);
			if (a > b)
				std::swap(a, b);
			return (static_cast<u64>(a) << 42) | (static_cast<u64>(b) << 21) | static_cast<u64>(c);
		}
	}

	u32 Module::HideUvId(double u, double v) const
	{
		if (!std::isfinite(u) || !std::isfinite(v) || std::fabs(u) > 1e6 || std::fabs(v) > 1e6)
			return kNoUv;
		const s64 cx = static_cast<s64>(std::floor(u / kUvCell)), cy = static_cast<s64>(std::floor(v / kUvCell));
		static constexpr int kOrder[3] = {0, -1, 1};
		for (int dy : kOrder)
			for (int dx : kOrder)
			{
				const auto [b, e] = m_hide_cells.equal_range(UvCellKey(cx + dx, cy + dy));
				for (auto it = b; it != e; ++it)
				{
					const std::array<double, 2>& p = m_hide_uv[it->second];
					if (std::fabs(p[0] - u) <= kUvTol && std::fabs(p[1] - v) <= kUvTol)
						return it->second;
				}
			}
		return kNoUv;
	}

	void Module::MakeHideSet(const Capture& c)
	{
		m_hide_cells.clear();
		m_hide_uv.clear();
		m_hide_tris.clear();
		std::vector<u32> pid(c.vertices.size(), kNoUv);
		for (size_t i = 0; i < c.vertices.size(); i++)
		{
			const CapturedVertex& cv = c.vertices[i];
			if (!(cv.q > 0.0f) || !std::isfinite(cv.q))
				continue;
			const double u = static_cast<double>(cv.s) / cv.q, v = static_cast<double>(cv.t) / cv.q;
			u32 id = HideUvId(u, v);
			if (id == kNoUv && std::isfinite(u) && std::isfinite(v) && std::fabs(u) <= 1e6 && std::fabs(v) <= 1e6)
			{
				id = static_cast<u32>(m_hide_uv.size());
				m_hide_uv.push_back({u, v});
				m_hide_cells.emplace(UvCellKey(static_cast<s64>(std::floor(u / kUvCell)), static_cast<s64>(std::floor(v / kUvCell))), id);
			}
			pid[i] = id;
		}
		const size_t nt = c.indices.size() / 3;
		for (size_t t = 0; t < nt; t++)
		{
#ifdef VR_HANDCAP_PLANTED_HIDE_DRAWN_ONLY
			if (!c.flags.empty() && (c.flags[t] & kFaceAdc))
				continue;
#endif
			const u32 a = pid[c.indices[t * 3]], b = pid[c.indices[t * 3 + 1]], cc = pid[c.indices[t * 3 + 2]];
			if (a == kNoUv || b == kNoUv || cc == kNoUv || m_hide_uv.size() >= (1u << 21))
				continue;
			m_hide_tris.push_back(UvTriKey(a, b, cc));
		}
		std::sort(m_hide_tris.begin(), m_hide_tris.end());
		m_hide_tris.erase(std::unique(m_hide_tris.begin(), m_hide_tris.end()), m_hide_tris.end());
	}

	bool Module::Hide(const GsDraw& d)
	{
		const GSVertex* v = static_cast<const GSVertex*>(d.vertices);
		const u32 kept = d.index_count / 3;
		m_vid.assign(d.vertex_count, kUvUnset);
		auto vid = [&](u16 i) {
			u32& id = m_vid[i];
			if (id == kUvUnset)
			{
				const GSVertex& g = v[i];
				const float q = g.RGBAQ.Q;
				id = (q > 0.0f && std::isfinite(q)) ? HideUvId(static_cast<double>(g.ST.S) / q, static_cast<double>(g.ST.T) / q) : kNoUv;
			}
			return id;
		};
		m_keep.assign(kept, 1);
		u32 hands = 0;
		for (u32 t = 0; t < kept; t++)
		{
			const u32 a = vid(d.indices[t * 3]), b = vid(d.indices[t * 3 + 1]), c = vid(d.indices[t * 3 + 2]);
			if (a == kNoUv || b == kNoUv || c == kNoUv)
				continue;
			if (std::binary_search(m_hide_tris.begin(), m_hide_tris.end(), UvTriKey(a, b, c)))
			{
				m_keep[t] = 0;
				hands++;
			}
		}
		m_ready_hidden = hands;
		if (hands == 0)
			return false;
		if (m_recipe.hide_mode == HideMode::SkipDraw || hands == kept)
		{
			m_stats.draws_hidden++;
			return true;
		}
		if (!d.edit_indices || !d.edit_count)
			return false;
		u32 w = 0;
#ifdef VR_HANDCAP_PLANTED_SPLIT_DROPS_ONE
		bool dropped = false;
#endif
		for (u32 t = 0; t < kept; t++)
		{
			if (!m_keep[t])
				continue;
#ifdef VR_HANDCAP_PLANTED_SPLIT_DROPS_ONE
			if (!dropped)
			{
				dropped = true;
				continue;
			}
#endif
			const u16 i0 = d.indices[t * 3], i1 = d.indices[t * 3 + 1], i2 = d.indices[t * 3 + 2];
			d.edit_indices[w++] = i0;
			d.edit_indices[w++] = i1;
			d.edit_indices[w++] = i2;
		}
		*d.edit_count = w;
		m_stats.draws_hidden++;
		m_stats.triangles_hidden += hands;
		return false;
	}

	void Module::AccumulateDrawn()
	{
		std::vector<u32> gi(m_obj_hand.size(), kNoUv);
		u32 ng = 0;
		for (size_t o = 0; o < m_obj_hand.size(); o++)
			if (m_obj_hand[o])
				gi[o] = ng++;
		std::vector<std::vector<u8>> now(ng);
		for (size_t t = 0; t < m_src.size(); t++)
			if (gi[m_obj[t]] != kNoUv)
				now[gi[m_obj[t]]].push_back(m_src_flags[t] == 0 ? 1 : 0);
		bool same = m_slot_drawn.size() == ng;
		for (u32 g = 0; same && g < ng; g++)
			same = m_slot_drawn[g].size() == now[g].size();
		if (!same)
		{
			m_slot_drawn = std::move(now);
			return;
		}
		for (u32 g = 0; g < ng; g++)
			for (size_t i = 0; i < now[g].size(); i++)
				m_slot_drawn[g][i] |= now[g][i];
	}

	void Module::AppendGrab(const GsDraw& d, bool with_slots)
	{
		Capture& c = *m_capture;
		std::vector<u32> gi(m_obj_hand.size(), kNoUv);
		u32 ng = 0;
		for (size_t o = 0; o < m_obj_hand.size(); o++)
			if (m_obj_hand[o])
				gi[o] = ng++;
		std::vector<u32> slot(ng, 0);
		with_slots = with_slots && m_slot_drawn.size() == ng;
		struct Key
		{
			u64 a, b, c;
			bool operator==(const Key&) const = default;
		};
		struct KeyHash
		{
			size_t operator()(const Key& k) const { return std::hash<u64>()(k.a ^ (k.b * 0x9E3779B97F4A7C15ull) ^ (k.c * 0xC2B2AE3D27D4EB4Full)); }
		};
		std::unordered_map<Key, u32, KeyHash> seen;
		GIFRegTEX0 tex0;
		tex0.U64 = d.tex0;
		for (size_t t = 0; t < m_src.size(); t++)
		{
			if (!m_obj_hand[m_obj[t]])
				continue;
			{
				const u32 g = gi[m_obj[t]];
				const u32 i = slot[g]++;
				u8 f = m_src_flags[t] & (kFaceAdc | kFaceZeroArea);
				const bool drawn = m_src_flags[t] == 0 || (with_slots && i < m_slot_drawn[g].size() && m_slot_drawn[g][i]);
				if (drawn && !(f & kFaceZeroArea))
					f |= kFaceDrawn;
				c.flags.push_back(f);
			}
			for (int k = 0; k < 3; k++)
			{
				const GSVertex& g = *m_src[t][k];
				u32 s32, t32, q32;
				std::memcpy(&s32, &g.ST.S, 4);
				std::memcpy(&t32, &g.ST.T, 4);
				std::memcpy(&q32, &g.RGBAQ.Q, 4);
				const u32 rgba = static_cast<u32>(g.RGBAQ.R) | (static_cast<u32>(g.RGBAQ.G) << 8) | (static_cast<u32>(g.RGBAQ.B) << 16) |
								 (static_cast<u32>(g.RGBAQ.A) << 24);
				const Key key{PositionKey(g.XYZ.X, g.XYZ.Y, g.XYZ.Z), static_cast<u64>(s32) | (static_cast<u64>(t32) << 32),
					static_cast<u64>(q32) | (static_cast<u64>(rgba) << 32)};
				const auto [it, inserted] = seen.try_emplace(key, static_cast<u32>(c.vertices.size()));
				if (inserted)
				{
					CapturedVertex cv;
					cv.x = g.XYZ.X;
					cv.y = g.XYZ.Y;
					cv.z = g.XYZ.Z;
					cv.ofx = d.ofx;
					cv.ofy = d.ofy;
					cv.s = g.ST.S;
					cv.t = g.ST.T;
					cv.q = g.RGBAQ.Q;
					cv.rgba = {static_cast<u8>(g.RGBAQ.R), static_cast<u8>(g.RGBAQ.G), static_cast<u8>(g.RGBAQ.B), static_cast<u8>(g.RGBAQ.A)};
					c.vertices.push_back(cv);
				}
				c.indices.push_back(it->second);
			}
		}
		c.draws++;
		c.tfx = tex0.TFX;
		if (!c.texture.Valid() && d.mem)
		{
			GIFRegTEXA texa;
			texa.U64 = d.texa;
			GIFRegCLAMP clamp;
			clamp.U64 = d.clamp;
			const u32 w = 1u << tex0.TW, h = 1u << tex0.TH;
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[tex0.PSM];
			if (psm.pal > 0)
				d.mem->m_clut.Read32(tex0, texa);
			c.texture.width = w;
			c.texture.height = h;
			c.texture.clamp_u = clamp.WMS != 0;
			c.texture.clamp_v = clamp.WMT != 0;
			c.texture.rgba.resize(static_cast<size_t>(w) * h);
			for (u32 y = 0; y < h; y++)
				for (u32 x = 0; x < w; x++)
					c.texture.rgba[static_cast<size_t>(y) * w + x] = (d.mem->*psm.rt)(static_cast<int>(x), static_cast<int>(y), tex0, texa);
		}
	}

	void Module::LaunchBuild()
	{
		std::array<std::shared_ptr<const HandAsset>, 2> refs;
		{
			std::lock_guard lock(m_cfg_mutex);
			refs = m_reference;
		}
		std::shared_ptr<Capture> cap = std::move(m_capture);
		MakeHideSet(*cap);
		m_grab_frame = -1;
		m_stats.grabs++;
		Console.WriteLnFmt("(VR) HandCapture: captured {} draws, {} vertices, {} triangles.", cap->draws, cap->vertices.size(),
			cap->indices.size() / 3);
		SetState(State::Building);
		WaitForBuild();
		const Recipe recipe = m_recipe;
		const u32 gen = m_seen_gen;
		m_worker = std::thread([this, cap = std::move(cap), refs = std::move(refs), recipe, gen]() {
			BuildResult r = BuildHands(recipe, *cap, {refs[0].get(), refs[1].get()});
			std::lock_guard lock(m_cfg_mutex);
			if (m_cfg_gen.load(std::memory_order_acquire) != gen)
				return;
			if (!r.ok)
			{
				Console.WarningFmt("(VR) HandCapture: refused: {}. The glove stays.", r.reason);
#ifndef VR_HANDCAP_PLANTED_FIT_REFUSAL_FINAL
				if (r.fit_refused && m_fit_retries < kFitRetries)
				{
					m_fit_retries++;
					Console.WarningFmt("(VR) HandCapture: the wrist fit refused this capture: watching for another (retry {} of {}, after {} frames).",
						m_fit_retries, kFitRetries, kFitRetryHoldFrames);
					m_retry_reset.store(true, std::memory_order_release);
					SetState(State::Watching);
					return;
				}
#endif
				SetState(State::Refused);
				m_active.store(false, std::memory_order_release);
				return;
			}
#ifdef VR_HANDCAP_PLANTED_G01_MOVE_FIRST
			{
				std::lock_guard pub(m_pub_mutex);
				for (int s = 0; s < 2; s++)
					m_pub[s] = r.has[s] ? std::make_shared<const HandAsset>(std::move(r.hands[s])) : nullptr;
			}
#endif
			const u32 built_left = r.has[0] ? r.hands[0].TriangleCount() : 0u;
			const u32 built_right = r.has[1] ? r.hands[1].TriangleCount() : 0u;
#ifndef VR_HANDCAP_PLANTED_G01_MOVE_FIRST
			{
				std::lock_guard pub(m_pub_mutex);
				for (int s = 0; s < 2; s++)
					m_pub[s] = r.has[s] ? std::make_shared<const HandAsset>(std::move(r.hands[s])) : nullptr;
			}
#endif
			Console.WriteLnFmt("(VR) HandCapture: built the game's hands (left {} triangles, right {} triangles).", built_left, built_right);
			Console.WriteLnFmt("(VR) HandCapture: oriented the built triangles: left {}, right {}.", OrientText(r.orient[0], r.has[0]),
				OrientText(r.orient[1], r.has[1]));
			Console.WriteLnFmt("(VR) HandCapture: fitted the wrist frames: left {}, right {}.", WristFitText(r.wrist[0]), WristFitText(r.wrist[1]));
			m_pub_gen.fetch_add(1, std::memory_order_acq_rel);
			SetState(State::Ready);
		});
	}

	bool Module::RunInRow(s64 prev_seen, s64 frame) const
	{
#ifdef VR_HANDCAP_PLANTED_GRAB_FRAME_STEP
		(void)prev_seen;
		return m_last_frame + 1 == frame;
#else
#ifdef VR_HANDCAP_PLANTED_RUN_NO_STEP_LIMIT
		const bool step_ok = true;
#else
		const bool step_ok = m_recipe.max_frame_step == 0 || frame - m_last_frame <= static_cast<s64>(m_recipe.max_frame_step);
#endif
		return m_last_frame >= 0 && prev_seen == m_last_frame && frame > m_last_frame && step_ok;
#endif
	}

	void Module::EndFrame()
	{
		if (m_cur_frame < 0)
			return;
		if (m_retry_hold > 0)
			m_retry_hold--;
		auto bucket = [](WatchStatus::Gaps& g, s64 step) { g[step <= 0 ? 4 : (step >= 4 ? 3 : static_cast<size_t>(step - 1))]++; };
		m_watch.frames++;
		if (m_watch_prev_seen >= 0)
			bucket(m_watch.seen_gaps, m_cur_frame - m_watch_prev_seen);
#ifdef VR_HANDCAP_PLANTED_STATUS_GAP_FROM_SEEN
		const s64 planted_prev_seen = m_watch_prev_seen;
#endif
		m_watch_prev_seen = m_cur_frame;
		const s64 prev_seen = m_run_prev_seen;
		m_run_prev_seen = m_cur_frame;
		m_watch.tex_frames += m_cur_tex ? 1u : 0u;
		m_watch.batch_frames += m_cur_batch ? 1u : 0u;
		if (!m_cur_matched)
		{
			if (m_grab_frame == m_cur_frame)
			{
				m_watch.grab_aborts++;
				m_grab_frame = -1;
				m_capture.reset();
			}
			return;
		}
		const bool in_row = RunInRow(prev_seen, m_cur_frame);
		const bool same_totals = m_cur == m_last;
		const double motion = m_last_frame >= 0 ? ShapeMotion(m_cur_shapes, m_last_shapes) : -1.0;
#ifdef VR_HANDCAP_PLANTED_STILLNESS_OFF
		const bool still = true;
#else
		const bool still = m_recipe.max_motion_px <= 0.0 || (motion >= 0.0 && motion <= m_recipe.max_motion_px);
#endif
		const bool continues = in_row && same_totals && still;
		m_stable = continues ? m_stable + 1 : 1;
		m_watch.hand_frames++;
		m_watch.last_motion_px = motion;
		if (m_replay_log.load(std::memory_order_relaxed))
		{
			std::string g;
			for (const GroupShape& s : m_cur_shapes)
				g += fmt::format("{}{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}", g.empty() ? "" : ";", s.cx, s.cy, s.x0, s.y0, s.x1, s.y1);
			Console.WriteLnFmt("(VR) HandCapture replay motion: frame {} last {} step {} in_row {} same_totals {} motion_px {:.3f} run {} groups {} [{}]",
				m_cur_frame, m_last_frame, m_last_frame >= 0 ? m_cur_frame - m_last_frame : 0, in_row ? 1 : 0, same_totals ? 1 : 0, motion,
				m_stable, m_cur_shapes.size(), g);
		}
		if (m_last_frame >= 0)
		{
#ifdef VR_HANDCAP_PLANTED_STATUS_GAP_FROM_SEEN
			bucket(m_watch.gaps, m_cur_frame - planted_prev_seen);
#else
			bucket(m_watch.gaps, m_cur_frame - m_last_frame);
#endif
			if (!continues)
			{
				const bool gap = !in_row;
				(gap ? m_watch.breaks_gap : m_watch.breaks_value)++;
				if (!gap && same_totals)
					m_watch.breaks_motion++;
				WatchStatus::Break& b = gap ? m_watch.last_gap : m_watch.last_value;
				b = {true, m_last_frame, m_cur_frame, m_last.objects, m_cur.objects, m_last.triangles, m_cur.triangles, m_last.draws,
					m_cur.draws};
			}
		}
		m_watch.streak = m_stable;
		m_watch.longest = std::max(m_watch.longest, m_stable);
		m_watch.last_objects = m_cur.objects;
		m_watch.last_triangles = m_cur.triangles;
		m_watch.last_draws = m_cur.draws;
		if (m_grab_frame == m_cur_frame)
		{
#ifndef VR_HANDCAP_PLANTED_UNSTABLE_GRAB
			if (!continues)
			{
				m_watch.grab_aborts++;
				m_grab_frame = -1;
				m_capture.reset();
			}
			else
#endif
			{
				WatchStatusLine("grab", std::chrono::duration<double>(std::chrono::steady_clock::now() - m_watch_start).count(),
					HaveReference());
				Console.WriteLnFmt("(VR) HandCapture: grabbing frame {}: run {} frames, step {}, motion {:.2f} px.", m_cur_frame, m_stable,
					m_cur_frame - m_last_frame, motion);
				LaunchBuild();
			}
		}
		m_last_frame = m_cur_frame;
		m_last = m_cur;
		m_last_shapes.swap(m_cur_shapes);
	}

	bool Module::OnGsDraw(const GsDraw& d)
	{
		m_stats.draws_seen++;
		SyncConfig();
		const bool rlog = m_replay_log.load(std::memory_order_relaxed);
		std::FILE* const trace = m_trace.load(std::memory_order_relaxed);
		if ((rlog || trace) && m_have_recipe && TextureMatches(d.tex0))
		{
			const State before = GetState();
			const size_t tap_triangles = m_tap.size() / 3;
			const u32 degenerate = m_rlog_degenerate;
			const u64 finds = m_find_calls;
			if (trace)
				m_trace_idx.assign(d.indices, d.indices + (d.indices ? d.index_count : 0));
			m_keep.clear();
			m_ready_hidden = 0;
			const bool hide = HandleDraw(d);
			if (rlog)
				ReplayLogLine(d, before, tap_triangles, degenerate, m_find_calls != finds, hide);
			if (trace)
				TraceBatch(trace, d, before, tap_triangles, degenerate, m_find_calls != finds, hide, m_trace_idx);
			m_rlog_degenerate = 0;
			m_tap.clear(), m_tap_flags.clear();
			return hide;
		}
		const bool hide = HandleDraw(d);
		m_tap.clear(), m_tap_flags.clear();
		return hide;
	}

	void Module::ReplayLogLine(const GsDraw& d, State before, size_t tap_triangles, u32 tap_degenerate, bool found, bool hid)
	{
		static constexpr const char* kState[] = {"off", "watching", "building", "ready", "refused", "timed_out"};
		const u32 batch = d.index_count / 3;
		std::string objs, px, view;
		u32 nobj = 0, wheel_kept = 0;
		if (found)
		{
			nobj = static_cast<u32>(m_obj_hand.size());
			std::vector<u32> tris(nobj, 0);
			std::vector<std::array<double, 5>> acc(nobj, std::array<double, 5>{0, 0, 0, 0, 0});
			for (size_t t = 0; t < m_src.size(); t++)
			{
				const u32 o = m_obj[t];
				if (t < m_src_flags.size() && m_src_flags[t])
					continue;
				tris[o]++;
				for (int k = 0; k < 3; k++)
				{
					const GSVertex& g = *m_src[t][k];
					acc[o][0] += (static_cast<double>(g.XYZ.X) - d.ofx) / 16.0;
					acc[o][1] += (static_cast<double>(g.XYZ.Y) - d.ofy) / 16.0;
					ViewPoint vp;
					if (Unproject(g.XYZ.X, g.XYZ.Y, d.ofx, d.ofy, g.RGBAQ.Q, m_recipe.projection, &vp))
					{
						acc[o][2] += vp.x;
						acc[o][3] += vp.y;
						acc[o][4] += vp.z;
					}
				}
			}
			for (u32 o = 0; o < nobj; o++)
			{
				if (o < 24)
					objs += fmt::format("{}{}{}", o ? "," : "", tris[o], m_obj_hand[o] ? "h" : "");
				if (!m_obj_hand[o] || tris[o] == 0)
					continue;
				const double n = 3.0 * tris[o];
				px += fmt::format("{}({:.2f},{:.2f})", px.empty() ? "" : ",", acc[o][0] / n, acc[o][1] / n);
				view += fmt::format("{}({:.6f},{:.6f},{:.6f})", view.empty() ? "" : ",", acc[o][2] / n, acc[o][3] / n, acc[o][4] / n);
			}
			if (nobj > 24)
				objs += fmt::format(",...+{}", nobj - 24);
		}
		if (before == State::Ready && m_keep.size() == batch)
			for (u32 t = 0; t < batch; t++)
				wheel_kept += m_keep[t];
		const u64 hand_tris = found ? m_hand_drawn : (before == State::Ready ? m_ready_hidden : 0u);
		const std::string after = d.edit_count ? fmt::format("{}", *d.edit_count / 3) : std::string("none");
		Console.WriteLnFmt("(VR) HandCapture replay: frame {} state {} batch_triangles {} vertices {} from_tap {} tap_triangles {} "
						   "tap_degenerate {} objects {} hand_objects {} hand_triangles {} wheel_kept {} after_edit_triangles {} "
						   "skipped {} objs [{}] hands_px [{}] hands_view [{}]",
			d.frame, kState[static_cast<int>(before)], batch, d.vertex_count, found ? (m_find_from_tap ? 1 : 0) : -1, tap_triangles,
			tap_degenerate, nobj, found ? m_hand_objects : 0u, hand_tris, wheel_kept, after, hid ? 1 : 0, objs, px,
			view);
	}

	void Module::OnTraceKick(const void* v0, const void* v1, const void* v2, u32 prim, u32 adc, u32 invalid, u32 cull, u64 tex0, s64 frame)
	{
		const u32 tap = m_trace_tap;
		m_trace_tap = 0;
		std::FILE* const f = m_trace.load(std::memory_order_relaxed);
		if (!f || !m_have_recipe || !TextureMatches(tex0))
			return;
#ifdef VR_HANDCAP_PLANTED_TRACE_DROP
		if (m_trace_kicks_in_batch++ == 1)
			return;
#else
		m_trace_kicks_in_batch++;
#endif
		const GSVertex* g[3] = {static_cast<const GSVertex*>(v0), static_cast<const GSVertex*>(v1), static_cast<const GSVertex*>(v2)};
		std::fprintf(f, "K %" PRId64 " %" PRIu64 " %u %u %u %u %u", static_cast<int64_t>(frame), static_cast<uint64_t>(m_trace_kseq++), prim,
			adc ? 1u : 0u, invalid ? 1u : 0u, cull, tap);
		for (const GSVertex* v : g)
			std::fprintf(f, " %u %u %u %.9g %.9g %.9g", static_cast<u32>(v->XYZ.X), static_cast<u32>(v->XYZ.Y), static_cast<u32>(v->XYZ.Z),
				static_cast<double>(v->ST.S), static_cast<double>(v->ST.T), static_cast<double>(v->RGBAQ.Q));
		std::fputc('\n', f);
	}

	void Module::TraceBatch(std::FILE* f, const GsDraw& d, State before, size_t tap_triangles, u32 tap_degenerate, bool found, bool hid,
		const std::vector<u16>& idx)
	{
		static constexpr const char* kState[] = {"off", "watching", "building", "ready", "refused", "timed_out"};
		const Recipe& r = m_recipe;
		if (m_trace_cfg != m_seen_gen)
		{
			m_trace_cfg = m_seen_gen;
			std::fprintf(f, "R 1 %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %u %u %u\n", r.projection.k, r.w_min, r.w_max, r.uv_box[0], r.uv_box[1],
				r.uv_box[2], r.uv_box[3], r.uv_min_coverage, r.stable_frames, r.triangles_min, r.triangles_max);
		}
		const u32 kept = static_cast<u32>(idx.size() / 3);
		const bool from_tap = found && m_find_from_tap;
		const GSVertex* v = static_cast<const GSVertex*>(d.vertices);
		const u64 bseq = m_trace_bseq++;
		std::fprintf(f, "B %" PRId64 " %" PRIu64 " %s %u %u %u %d %zu %u %u %u %" PRIu64 " %u %s %u %u\n", static_cast<int64_t>(d.frame),
			static_cast<uint64_t>(bseq), kState[static_cast<int>(before)], d.prim, kept, d.vertex_count, found ? (from_tap ? 1 : 0) : -1,
			tap_triangles, tap_degenerate, found ? static_cast<u32>(m_obj_hand.size()) : 0u, found ? m_hand_objects : 0u,
			static_cast<uint64_t>(found ? m_hand_drawn : (before == State::Ready ? m_ready_hidden : 0u)), hid ? 1u : 0u, d.edit_count ? fmt::format("{}", *d.edit_count / 3).c_str() : "none",
			d.ofx, d.ofy);
		for (u32 i = 0; v && i < d.vertex_count; i++)
		{
			const GSVertex& g = v[i];
			std::fprintf(f, "X %u %u %u %u %.9g %.9g %.9g %02x%02x%02x%02x\n", i, static_cast<u32>(g.XYZ.X), static_cast<u32>(g.XYZ.Y),
				static_cast<u32>(g.XYZ.Z), static_cast<double>(g.ST.S), static_cast<double>(g.ST.T), static_cast<double>(g.RGBAQ.Q),
				static_cast<u32>(g.RGBAQ.R) & 0xFF, static_cast<u32>(g.RGBAQ.G) & 0xFF, static_cast<u32>(g.RGBAQ.B) & 0xFF,
				static_cast<u32>(g.RGBAQ.A) & 0xFF);
		}
		const bool keep_valid = before == State::Ready && m_keep.size() == kept;
		for (u32 t = 0; t < kept; t++)
		{
			long obj = -1;
			if (found && !from_tap && t < m_obj.size() && m_src.size() == kept)
				obj = m_obj[t];
			else if (from_tap && v && idx[t * 3] < d.vertex_count)
			{
				const GSVertex& a = v[idx[t * 3]];
				const auto it = m_key_obj.find(PositionKey(a.XYZ.X, a.XYZ.Y, a.XYZ.Z));
				if (it != m_key_obj.end())
					obj = it->second;
			}
			const u32 keep = hid ? 0u : (keep_valid ? m_keep[t] : 1u);
			std::fprintf(f, "P %u %u %u %u %ld %u\n", t, idx[t * 3], idx[t * 3 + 1], idx[t * 3 + 2], obj, keep);
		}
		size_t s_seq = 0;
		if (from_tap)
			for (size_t t = 0; t < m_src.size(); t++)
			{
				if (t < m_src_flags.size() && m_src_flags[t])
					continue;
				std::fprintf(f, "S %zu %u", s_seq++, m_obj[t]);
				for (int k = 0; k < 3; k++)
				{
					const GSVertex& g = *m_src[t][k];
					std::fprintf(f, " %u %u %u %.9g %.9g %.9g", static_cast<u32>(g.XYZ.X), static_cast<u32>(g.XYZ.Y), static_cast<u32>(g.XYZ.Z),
						static_cast<double>(g.ST.S), static_cast<double>(g.ST.T), static_cast<double>(g.RGBAQ.Q));
				}
				std::fputc('\n', f);
			}
		if (found)
		{
			const size_t nobj = m_obj_hand.size();
			struct T
			{
				u32 tris = 0;
				double wmin = 1e300, wmax = -1e300, u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300, px = 0, py = 0;
				bool in_box = true;
				int fail = 0;
			};
			std::vector<T> o(nobj);
			for (size_t t = 0; t < m_src.size() && t < m_obj.size(); t++)
			{
				if (t < m_src_flags.size() && m_src_flags[t])
					continue;
				T& a = o[m_obj[t]];
				a.tris++;
				for (int k = 0; k < 3; k++)
				{
					const GSVertex& g = *m_src[t][k];
					a.px += (static_cast<double>(g.XYZ.X) - d.ofx) / 16.0;
					a.py += (static_cast<double>(g.XYZ.Y) - d.ofy) / 16.0;
					const float q = g.RGBAQ.Q;
					if (!(q > 0.0f) || !std::isfinite(q))
					{
						a.fail = a.fail ? a.fail : 1;
						continue;
					}
					const double w = r.projection.k / q;
					a.wmin = std::min(a.wmin, w);
					a.wmax = std::max(a.wmax, w);
					const double vpx = (static_cast<double>(g.XYZ.X) - d.ofx) / 16.0, vpy = (static_cast<double>(g.XYZ.Y) - d.ofy) / 16.0;
					const bool tested = !d.has_scissor || (vpx >= d.sc_x0 && vpx < d.sc_x1 + 1.0 && vpy >= d.sc_y0 && vpy < d.sc_y1 + 1.0);
					if (!a.fail && tested && (w < r.w_min || w > r.w_max))
						a.fail = 2;
					const double u = g.ST.S / q, vv = g.ST.T / q;
					a.u0 = std::min(a.u0, u);
					a.v0 = std::min(a.v0, vv);
					a.u1 = std::max(a.u1, u);
					a.v1 = std::max(a.v1, vv);
					const bool in = u >= r.uv_box[0] && u <= r.uv_box[2] && vv >= r.uv_box[1] && vv <= r.uv_box[3];
					a.in_box = a.in_box && in;
					if (!a.fail && r.has_uv_box && !in)
						a.fail = 3;
				}
			}
			static constexpr const char* kFail[] = {"none", "q", "w", "uv_box", "coverage"};
			const double box = (r.uv_box[2] - r.uv_box[0]) * (r.uv_box[3] - r.uv_box[1]);
			for (size_t i = 0; i < nobj; i++)
			{
				T& a = o[i];
				const double share = box > 0 && a.tris ? std::max(0.0, a.u1 - a.u0) * std::max(0.0, a.v1 - a.v0) / box : 0.0;
				if (!a.fail && r.has_uv_box && !m_obj_hand[i])
					a.fail = 4;
				const double n = a.tris ? 3.0 * a.tris : 1.0;
				std::fprintf(f, "O %zu %u %.9g %.9g %.9g %.9g %.9g %.9g %u %.6f %s %u %.2f %.2f\n", i, a.tris, a.wmin, a.wmax, a.u0, a.v0, a.u1, a.v1,
					a.in_box ? 1u : 0u, share, kFail[a.fail], m_obj_hand[i] ? 1u : 0u, a.px / n, a.py / n);
			}
		}
		std::fprintf(f, "G %" PRId64 " %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRId64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %u %u %" PRId64 " %u\n",
			static_cast<int64_t>(m_cur_frame), kState[static_cast<int>(GetState())], static_cast<uint64_t>(m_cur.objects),
			static_cast<uint64_t>(m_cur.triangles), static_cast<uint64_t>(m_cur.draws), static_cast<int64_t>(m_last_frame),
			static_cast<uint64_t>(m_last.objects), static_cast<uint64_t>(m_last.triangles), static_cast<uint64_t>(m_last.draws), m_stable,
			r.stable_frames, static_cast<int64_t>(m_grab_frame), m_stats.grabs);
		std::fprintf(f, "E %" PRIu64 "\n", static_cast<uint64_t>(bseq));
		m_trace_kicks_in_batch = 0;
	}

#if defined(ENABLE_VR) && defined(VR_HANDCAP_EXPERIMENT_TAP_IN_READY)
	void Module::ExperimentPostCull(const GsDraw& d)
	{
		std::vector<GSVertex> saved;
		saved.swap(m_tap);
		bool ft = false;
		const bool any = FindHands(d, &ft);
		const u32 kept = d.index_count / 3;
		m_exp_postcull.assign(kept, 0);
		if (any)
			for (u32 t = 0; t < kept && t < m_obj.size(); t++)
				m_exp_postcull[t] = m_obj_hand[m_obj[t]];
		m_exp_postcull_valid = !ft;
		m_tap.swap(saved);
	}

	void Module::ExperimentLine(const GsDraw& d, bool found_b, bool from_tap_b)
	{
		if (!m_replay_log.load(std::memory_order_relaxed))
			return;
		const u32 kept = d.index_count / 3;
		u32 hid_a = 0, hid_b = 0, miss = 0, over = 0;
		std::string miss_px;
		const GSVertex* v = static_cast<const GSVertex*>(d.vertices);
		for (u32 t = 0; t < kept; t++)
		{
			const bool a = t < m_exp_postcull.size() && m_exp_postcull[t];
			const bool b = found_b && t < m_keep.size() && !m_keep[t];
			hid_a += a;
			hid_b += b;
			if (b && !a)
			{
				miss++;
				if (miss <= 4)
				{
					const GSVertex& g = v[d.indices[t * 3]];
					miss_px += fmt::format("{}({:.1f},{:.1f})", miss_px.empty() ? "" : ",", (static_cast<double>(g.XYZ.X) - d.ofx) / 16.0,
						(static_cast<double>(g.XYZ.Y) - d.ofy) / 16.0);
				}
			}
			over += (a && !b);
		}
		std::string refused;
		if (found_b || !m_obj_hand.empty())
		{
			const size_t nobj = m_obj_hand.size();
			std::vector<std::array<double, 7>> r(nobj, std::array<double, 7>{1e300, -1e300, 1e300, 1e300, -1e300, -1e300, 0});
			for (size_t t = 0; t < m_src.size() && t < m_obj.size(); t++)
			{
				auto& o = r[m_obj[t]];
				o[6] += 1;
				for (int k = 0; k < 3; k++)
				{
					const GSVertex& g = *m_src[t][k];
					const double q = g.RGBAQ.Q;
					const double w = q > 0 ? m_recipe.projection.k / q : -1.0;
					o[0] = std::min(o[0], w);
					o[1] = std::max(o[1], w);
					if (q > 0)
					{
						o[2] = std::min(o[2], static_cast<double>(g.ST.S) / q);
						o[3] = std::min(o[3], static_cast<double>(g.ST.T) / q);
						o[4] = std::max(o[4], static_cast<double>(g.ST.S) / q);
						o[5] = std::max(o[5], static_cast<double>(g.ST.T) / q);
					}
				}
			}
			const double box = (m_recipe.uv_box[2] - m_recipe.uv_box[0]) * (m_recipe.uv_box[3] - m_recipe.uv_box[1]);
			for (size_t o = 0; o < nobj; o++)
				if (!m_obj_hand[o] && r[o][6] >= 300)
					refused += fmt::format("{}({} tri w {:.5f}..{:.5f} uv {:.2f},{:.2f}..{:.2f},{:.2f} cov {:.2f})", refused.empty() ? "" : ",",
						static_cast<u32>(r[o][6]), r[o][0], r[o][1], r[o][2], r[o][3], r[o][4], r[o][5],
						box > 0 ? std::max(0.0, r[o][4] - r[o][2]) * std::max(0.0, r[o][5] - r[o][3]) / box : 0.0);
		}
		Console.WriteLnFmt("(VR) HandCapture experiment: frame {} kept_triangles {} from_tap {} postcull_valid {} hidden_postcull {} "
						   "hidden_tap {} miss {} overhide {} miss_px [{}] refused_big [{}]",
			d.frame, kept, from_tap_b ? 1 : 0, m_exp_postcull_valid ? 1 : 0, hid_a, hid_b, miss, over, miss_px, refused);
	}
#endif

	bool Module::HandleDraw(const GsDraw& d)
	{
		if (!m_have_recipe)
			return false;
		const State st = GetState();
		bool from_tap = false;
		if (st == State::Ready)
		{
#ifndef VR_HANDCAP_PLANTED_HIDE_IGNORES_TOGGLE
			if (!Enabled())
				return false;
#endif
#if defined(ENABLE_VR) && defined(VR_HANDCAP_EXPERIMENT_TAP_IN_READY)
			if (!BatchMatches(d))
				return false;
			ExperimentPostCull(d);
			const bool found_b = FindHands(d, &from_tap);
			if (!found_b)
			{
				m_keep.assign(d.index_count / 3, 1);
				ExperimentLine(d, false, from_tap);
				return false;
			}
			{
				const bool hid = Hide(d);
				ExperimentLine(d, true, from_tap);
				return hid;
			}
#else
			if (!BatchMatches(d))
				return false;
#endif
#ifdef VR_HANDCAP_PLANTED_HIDE_WRITES
			if (d.mem)
				d.mem->vm8()[0] += 1;
#endif
			return Hide(d);
		}
		if (st != State::Watching || !Enabled())
			return false;
		if (m_retry_reset.exchange(false, std::memory_order_acq_rel))
			ResetRunForRetry(kFitRetryHoldFrames);
		if (m_toggle_reset.exchange(false, std::memory_order_acq_rel))
			ResetRunOnToggle();

		if (d.frame != m_cur_frame)
		{
			EndFrame();
			if (GetState() != State::Watching)
				return false;
			m_cur_frame = d.frame;
			m_cur = {};
			m_cur_shapes.clear();
			m_cur_matched = false;
			m_cur_tex = m_cur_batch = false;
			double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_watch_start).count();
			if (waited > m_recipe.max_wait_s)
			{
				bool retried = false;
				{
					std::lock_guard lock(m_cfg_mutex);
					if (m_cfg_gen.load(std::memory_order_acquire) == m_seen_gen)
					{
						WatchStatusLine("timeout", waited, m_reference[0] && m_reference[1]);
#ifndef VR_HANDCAP_PLANTED_TIMEOUT_FINAL
						if (m_watch.timeout_retries < kTimeoutRetries)
						{
							m_watch.timeout_retries++;
							Console.WarningFmt("(VR) HandCapture: no stable match in {:.0f} s: watching for another (timeout retry {} of {}, after {} frames).",
								waited, m_watch.timeout_retries, kTimeoutRetries, kTimeoutRetryHoldFrames);
							retried = true;
						}
						else
#endif
						{
							Console.WarningFmt("(VR) HandCapture: timed out after {:.0f} s without a stable match. The glove stays.", waited);
							SetState(State::TimedOut);
							m_active.store(false, std::memory_order_release);
						}
					}
				}
				if (!retried)
				{
					m_capture.reset();
					return false;
				}
				ResetRunForRetry(kTimeoutRetryHoldFrames);
				m_cur_frame = d.frame;
				waited = 0.0;
			}
			if (waited >= m_watch_next_s)
			{
				WatchStatusLine("timer", waited, HaveReference());
				while (m_watch_next_s <= waited)
					m_watch_next_s += 5.0;
			}
		}
		const bool tex = TextureMatches(d.tex0);
		m_cur_tex |= tex;
		if (!tex || !BatchMatches(d))
			return false;
		m_cur_batch = true;
		const bool found = FindHands(d, &from_tap);
		m_watch.batches++;
		m_watch.batches_tap += from_tap ? 1u : 0u;
		if (!found)
			return false;
		m_stats.batches_from_tap += from_tap ? 1u : 0u;
		m_stats.draws_selected++;
		m_cur_matched = true;
		const bool first_batch = m_cur.draws == 0;
		m_cur.objects += m_hand_objects;
#ifdef VR_HANDCAP_PLANTED_GRAB_ON_DRAWN
		m_cur.triangles += m_hand_drawn;
#else
		m_cur.triangles += m_hand_triangles;
#endif
		m_cur.draws++;
		{
			const size_t first = m_cur_shapes.size();
			std::vector<s32> slot(m_obj_hand.size(), -1);
			for (size_t o = 0; o < m_obj_hand.size(); o++)
				if (m_obj_hand[o])
				{
					slot[o] = static_cast<s32>(m_cur_shapes.size());
					GroupShape s;
					s.x0 = s.y0 = std::numeric_limits<double>::infinity();
					s.x1 = s.y1 = -std::numeric_limits<double>::infinity();
					m_cur_shapes.push_back(s);
				}
			std::vector<u32> n(m_cur_shapes.size() - first, 0);
			for (size_t t = 0; t < m_src.size(); t++)
			{
				const s32 k = slot[m_obj[t]];
				if (k < 0)
					continue;
				GroupShape& s = m_cur_shapes[k];
				for (int v = 0; v < 3; v++)
				{
					const GSVertex& gv = *m_src[t][v];
					const double x = (static_cast<double>(gv.XYZ.X) - d.ofx) / 16.0, y = (static_cast<double>(gv.XYZ.Y) - d.ofy) / 16.0;
					s.cx += x, s.cy += y;
					s.x0 = std::min(s.x0, x), s.y0 = std::min(s.y0, y), s.x1 = std::max(s.x1, x), s.y1 = std::max(s.y1, y);
				}
				n[k - first] += 3;
			}
			for (size_t k = first; k < m_cur_shapes.size(); k++)
				if (n[k - first])
					m_cur_shapes[k].cx /= n[k - first], m_cur_shapes[k].cy /= n[k - first];
		}
		if (first_batch && from_tap)
			AccumulateDrawn();

		bool have_reference = false;
		{
			std::lock_guard lock(m_cfg_mutex);
			have_reference = m_reference[0] && m_reference[1];
		}
#ifdef VR_HANDCAP_PLANTED_UNSTABLE_GRAB
		const bool stable = true;
#else
		const bool stable = m_stable >= m_recipe.stable_frames && RunInRow(m_run_prev_seen, d.frame);
#endif
		if (m_grab_frame < 0 && stable && !have_reference && m_cur.draws == 1)
			m_watch.stable_no_ref++;
		if (m_grab_frame == d.frame)
			AppendGrab(d, false);
		else if (m_grab_frame < 0 && stable && have_reference && m_cur.draws == 1 && m_retry_hold <= 0)
		{
			m_watch.grab_starts++;
			m_capture = std::make_shared<Capture>();
			m_capture->vertices.reserve(static_cast<size_t>(std::max<u32>(m_recipe.vertices_max, 64)) * 2);
			m_capture->indices.reserve(static_cast<size_t>(std::max<u32>(m_recipe.triangles_max, 64)) * 6);
			m_grab_frame = d.frame;
			AppendGrab(d, from_tap);
		}
		return false;
	}
}
