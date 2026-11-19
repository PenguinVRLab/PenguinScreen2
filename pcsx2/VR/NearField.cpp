// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/NearField.h"
#include "VR/HandCapture.h"
#include "VR/SpatialControls.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

namespace VR::NearField
{
	namespace
	{
		std::atomic<u8> s_profile_hands{static_cast<u8>(Hands::Off)};

		using HandsGltf::Mat4;
		using HandsGltf::Vec3;

		Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
		Vec3 Mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
		float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		Vec3 Cross(const Vec3& a, const Vec3& b)
		{
			return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
		}
		Vec3 Normalize(const Vec3& a)
		{
			const float l = std::sqrt(Dot(a, a));
			return l > 1e-12f ? Mul(a, 1.0f / l) : Vec3{};
		}
		Vec3 FromSC(const SpatialControls::Vec3& v) { return {v.x, v.y, v.z}; }

		bool EqualsNoCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
				const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
				if (x != y)
					return false;
			}
			return true;
		}

		constexpr float kLightX = -0.35f, kLightY = 0.80f, kLightZ = 0.50f;
		constexpr float kAmbient = 0.38f, kDiffuse = 0.80f, kSpec = 0.06f, kSpecPower = 18.0f, kRim = 0.10f, kRimPower = 3.0f;
		constexpr float kRimR = 0.80f, kRimG = 0.88f, kRimB = 1.00f;

		u8 EncodeSrgb(float linear)
		{
			linear = std::clamp(linear, 0.0f, 1.0f);
			const float s = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
			return static_cast<u8>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
		}
	}

	const char* HandsName(Hands hands)
	{
		switch (hands)
		{
			case Hands::GloveV1_1: return "glove-v1.1";
			case Hands::Game: return "game";
			case Hands::Off:
			default: return "off";
		}
	}

	bool ParseHands(std::string_view text, Hands* out)
	{
		if (EqualsNoCase(text, "off"))
			*out = Hands::Off;
		else if (EqualsNoCase(text, "glove-v1.1"))
			*out = Hands::GloveV1_1;
		else if (EqualsNoCase(text, "game"))
			*out = Hands::Game;
		else
			return false;
		return true;
	}

	std::string AssetRelativePath(Hands hands, bool right)
	{
		if (hands != Hands::GloveV1_1 && hands != Hands::Game)
			return {};
		return right ? "vr-assets/hands/glove-v1.1/glove-v1.1_right.glb" : "vr-assets/hands/glove-v1.1/glove-v1.1_left.glb";
	}

	void SetProfileHands(Hands hands)
	{
		s_profile_hands.store(static_cast<u8>(hands), std::memory_order_release);
	}

	Hands ProfileHands()
	{
		return static_cast<Hands>(s_profile_hands.load(std::memory_order_acquire));
	}

	bool Fits(u32 layer_count, u32 capacity, u32 runtime_max)
	{
		const u32 limit = runtime_max != 0 ? std::min(capacity, runtime_max) : capacity;
		return layer_count < limit;
	}

	std::array<HandDraw, 2> PlaceHands(const std::array<VRHandState, 2>& hands, bool actions_active,
		const std::array<LeverSlot, 2>& levers, const Anchor& anchor, const std::array<JointSet, 2>* joints,
		const std::array<u32, 2>& weighted, const std::array<bool, 2>& controller_in_hand)
	{
		std::array<HandDraw, 2> out{};
		for (int i = 0; i < 2; i++)
		{
			const LeverSlot& lv = levers[i];
#ifdef VR_NEARFIELD_PLANTED_GRAB_IGNORED
			const bool on_lever = false;
			(void)lv;
#else
			const bool on_lever = lv.active && lv.grabbed;
#endif
			if (on_lever)
			{
				SpatialControls::Anchor a;
				a.position = {anchor.x, anchor.y, anchor.z};
				a.yaw = anchor.yaw;
				SpatialControls::Placement pl;
				pl.side = lv.side;
				pl.height = lv.height;
				pl.forward = lv.forward;
				pl.yaw_deg = lv.yaw_deg;
				const SpatialControls::Frame f = SpatialControls::PlaceControl(a, pl);
				const SpatialControls::LeverParams lp;
				const Vec3 grip = FromSC(SpatialControls::LeverGripPosition(f, lp, lv.t));
				const Vec3 axis = Normalize(FromSC(SpatialControls::LeverGripAxis(f, lp, lv.t)));
				const Vec3 z = Mul(axis, -1.0f);
				Vec3 x = FromSC(f.Right());
				x = Normalize(Sub(x, Mul(z, Dot(x, z))));
				const Vec3 y = Cross(z, x);
				out[i].draw = true;
				out[i].on_lever = true;
				out[i].curl = 1.0f;
				out[i].world_from_grip = Mat4::FromBasis(x, y, z, grip);
				continue;
			}
			const VRHandState& h = hands[i];
			if (!actions_active)
				continue;
#ifdef VR_NEARFIELD_PLANTED_GRIP_MEANS_CONTROLLER
			const bool controller = h.grip_pose.valid;
			(void)controller_in_hand;
#else
			const bool controller = controller_in_hand[i];
#endif
			if (controller)
			{
				if (!h.grip_pose.valid)
					continue;
				out[i].draw = true;
				out[i].curl = std::clamp(std::isfinite(h.grip) ? h.grip : 0.0f, 0.0f, 1.0f);
				out[i].world_from_grip = Mat4::FromPose(h.grip_pose.orientation_xyzw, h.grip_pose.position_xyz);
				continue;
			}
			if (!joints)
				continue;
			const JointSet& js = (*joints)[i];
			bool drawable = js.active;
			for (int j = 0; j < HandsGltf::kJointCount && drawable; j++)
			{
#ifdef VR_NEARFIELD_PLANTED_JOINT_DRAW_INVALID
				(void)weighted;
#else
				if ((weighted[i] >> j) & 1u)
					drawable = js.pose[j].valid;
#endif
			}
			if (!drawable)
				continue;
			out[i].draw = true;
			out[i].from_joints = true;
			for (int j = 0; j < HandsGltf::kJointCount; j++)
				out[i].joint_poses[j] = Mat4::FromPose(js.pose[j].orientation_xyzw, js.pose[j].position_xyz);
		}
		return out;
	}

	const std::array<JointSet, 2>* JointsForModel(Hands model, const std::array<JointSet, 2>& joints)
	{
		return model == Hands::GloveV1_1 ? &joints : nullptr;
	}

	RasterStats Raster(const EyeView& eye, u32 w, u32 h, const Mesh* meshes, size_t mesh_count, std::vector<u32>* rgba,
		std::vector<float>* depth)
	{
		RasterStats stats;
		const auto t_clear = std::chrono::steady_clock::now();
		rgba->assign(static_cast<size_t>(w) * h, 0u);
		depth->assign(static_cast<size_t>(w) * h, 0.0f);
		const auto t_draw = std::chrono::steady_clock::now();
		stats.clear_ms = std::chrono::duration<double, std::milli>(t_draw - t_clear).count();
		if (w == 0 || h == 0)
			return stats;

		const Mat4 view = Mat4::FromPose(eye.orientation, eye.position).RigidInverse();
		const float tl = std::tan(eye.angle_left), tr = std::tan(eye.angle_right);
		const float tu = std::tan(eye.angle_up), td = std::tan(eye.angle_down);
		if (!(tr > tl) || !(tu > td))
			return stats;
		const float sx = static_cast<float>(w) / (tr - tl);
		const float sy = static_cast<float>(h) / (tu - td);

		const Vec3 L = Normalize({kLightX, kLightY, kLightZ});

		struct V
		{
			float px, py, iz;
			float r, g, b;
			float dr, dg, db;
			float u, v;
			bool ok;
		};
		std::vector<V> verts;

		for (size_t mi = 0; mi < mesh_count; mi++)
		{
			const Mesh& m = meshes[mi];
			if (!m.positions || !m.normals || !m.colors || !m.indices)
				continue;
			const size_t n = m.positions->size() / 3;
			const bool textured = m.uvs && m.texture && m.texture->Valid() && m.uvs->size() >= n * 2;
			verts.resize(n);
			for (size_t v = 0; v < n; v++)
			{
				const Vec3 pw{(*m.positions)[v * 3], (*m.positions)[v * 3 + 1], (*m.positions)[v * 3 + 2]};
				const Vec3 nw{(*m.normals)[v * 3], (*m.normals)[v * 3 + 1], (*m.normals)[v * 3 + 2]};
				const Vec3 p = view.Point(pw);
				Vec3 nv = Normalize(view.Dir(nw));
				V& o = verts[v];
				const float zf = -p.z;
				o.ok = zf > kNearM;
				if (!o.ok)
					continue;
				o.iz = 1.0f / zf;
				o.px = (p.x * o.iz - tl) * sx;
				o.py = (tu - p.y * o.iz) * sy;
				const Vec3 to_eye = Normalize(Mul(p, -1.0f));
				if (Dot(nv, to_eye) < 0.0f)
					nv = Mul(nv, -1.0f);
				const float ndl = std::max(0.0f, Dot(nv, L));
				const Vec3 hv = Normalize({L.x + to_eye.x, L.y + to_eye.y, L.z + to_eye.z});
				const float spec = std::pow(std::max(0.0f, Dot(nv, hv)), kSpecPower) * kSpec;
				const float rim = std::pow(1.0f - std::clamp(Dot(nv, to_eye), 0.0f, 1.0f), kRimPower) * kRim;
				const float shade = kAmbient + kDiffuse * ndl;
				o.r = (*m.colors)[v * 3 + 0] * m.tint[0] * shade + spec + rim * kRimR;
				o.g = (*m.colors)[v * 3 + 1] * m.tint[1] * shade + spec + rim * kRimG;
				o.b = (*m.colors)[v * 3 + 2] * m.tint[2] * shade + spec + rim * kRimB;
				if (textured)
				{
					o.dr = (*m.colors)[v * 3 + 0] * m.tint[0] * shade;
					o.dg = (*m.colors)[v * 3 + 1] * m.tint[1] * shade;
					o.db = (*m.colors)[v * 3 + 2] * m.tint[2] * shade;
					o.u = (*m.uvs)[v * 2];
					o.v = (*m.uvs)[v * 2 + 1];
				}
			}

			const std::vector<u32>& idx = *m.indices;
			for (size_t t = 0; t + 2 < idx.size(); t += 3)
			{
				const V& a = verts[idx[t]];
				const V& b = verts[idx[t + 1]];
				const V& c = verts[idx[t + 2]];
				if (!a.ok || !b.ok || !c.ok)
					continue;
				const float area = (b.px - a.px) * (c.py - a.py) - (b.py - a.py) * (c.px - a.px);
				if (std::fabs(area) < 1e-8f)
					continue;
				const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.px, b.px, c.px}))));
				const int x1 = std::min(static_cast<int>(w) - 1, static_cast<int>(std::ceil(std::max({a.px, b.px, c.px}))));
				const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.py, b.py, c.py}))));
				const int y1 = std::min(static_cast<int>(h) - 1, static_cast<int>(std::ceil(std::max({a.py, b.py, c.py}))));
				if (x0 > x1 || y0 > y1)
					continue;
				stats.triangles++;
				const float inv_area = 1.0f / area;
				for (int y = y0; y <= y1; y++)
				{
					const float fy = static_cast<float>(y) + 0.5f;
					for (int x = x0; x <= x1; x++)
					{
						const float fx = static_cast<float>(x) + 0.5f;
						float w0 = ((b.px - fx) * (c.py - fy) - (b.py - fy) * (c.px - fx)) * inv_area;
						float w1 = ((c.px - fx) * (a.py - fy) - (c.py - fy) * (a.px - fx)) * inv_area;
						float w2 = 1.0f - w0 - w1;
						if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
							continue;
						const float iz = w0 * a.iz + w1 * b.iz + w2 * c.iz;
						const size_t at = static_cast<size_t>(y) * w + static_cast<size_t>(x);
#ifndef VR_NEARFIELD_PLANTED_NO_DEPTH
						if (iz <= (*depth)[at])
							continue;
#endif
						(*depth)[at] = iz;
						const float pa = w0 * a.iz / iz, pb = w1 * b.iz / iz, pc = w2 * c.iz / iz;
						float r = pa * a.r + pb * b.r + pc * c.r;
						float g = pa * a.g + pb * b.g + pc * c.g;
						float bl = pa * a.b + pb * b.b + pc * c.b;
						if (textured)
						{
							const float dr = pa * a.dr + pb * b.dr + pc * c.dr;
							const float dg = pa * a.dg + pb * b.dg + pc * c.dg;
							const float db = pa * a.db + pb * b.db + pc * c.db;
							const std::array<float, 3> t =
								SampleHandTexture(*m.texture, pa * a.u + pb * b.u + pc * c.u, pa * a.v + pb * b.v + pc * c.v);
							r = dr * t[0] + (r - dr);
							g = dg * t[1] + (g - dg);
							bl = db * t[2] + (bl - db);
						}
						if ((*rgba)[at] == 0u)
							stats.covered++;
						(*rgba)[at] = (0xFFu << 24) | (static_cast<u32>(EncodeSrgb(bl)) << 16) |
									  (static_cast<u32>(EncodeSrgb(g)) << 8) | static_cast<u32>(EncodeSrgb(r));
					}
				}
			}
		}
		stats.draw_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_draw).count();
		return stats;
	}

	std::array<float, 3> SampleHandTexture(const HandsGltf::HandTexture& tex, float u, float v)
	{
		if (!tex.Valid())
			return {1.0f, 1.0f, 1.0f};
		const int W = static_cast<int>(tex.width), H = static_cast<int>(tex.height);
		const float tx = u * static_cast<float>(W) - 0.5f;
		const float ty = v * static_cast<float>(H) - 0.5f;
		const float fx0 = std::floor(tx), fy0 = std::floor(ty);
		const float ax = tx - fx0, ay = ty - fy0;
		auto wrap = [](int i, int n, bool clamp) { return clamp ? std::clamp(i, 0, n - 1) : ((i % n) + n) % n; };
		const int x0 = wrap(static_cast<int>(fx0), W, tex.clamp_u), x1 = wrap(static_cast<int>(fx0) + 1, W, tex.clamp_u);
		const int y0 = wrap(static_cast<int>(fy0), H, tex.clamp_v), y1 = wrap(static_cast<int>(fy0) + 1, H, tex.clamp_v);
		auto texel = [&tex, W](int x, int y, int ch) {
			const u32 c = tex.rgba[static_cast<size_t>(y) * W + x];
			const float s = static_cast<float>((c >> (ch * 8)) & 0xFFu) / 255.0f;
			return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
		};
		std::array<float, 3> out{};
		for (int ch = 0; ch < 3; ch++)
		{
			const float top = texel(x0, y0, ch) * (1.0f - ax) + texel(x1, y0, ch) * ax;
			const float bot = texel(x0, y1, ch) * (1.0f - ax) + texel(x1, y1, ch) * ax;
			out[ch] = top * (1.0f - ay) + bot * ay;
		}
		return out;
	}

	bool MakeGpuEyeConstants(const EyeView& eye, GpuEyeConstants* out)
	{
		const float tl = std::tan(eye.angle_left), tr = std::tan(eye.angle_right);
		const float tu = std::tan(eye.angle_up), td = std::tan(eye.angle_down);
		if (!(tr > tl) || !(tu > td))
			return false;
		out->view = Mat4::FromPose(eye.orientation, eye.position).RigidInverse().m;
#ifdef VR_NEARFIELD_PLANTED_GPU_FLIP_Y
		out->proj = {tl, -tu, 2.0f / (tr - tl), -2.0f / (tu - td)};
#else
		out->proj = {tl, tu, 2.0f / (tr - tl), 2.0f / (tu - td)};
#endif
		return true;
	}

	std::array<float, 4> GpuClipFromWorld(const GpuEyeConstants& c, const Vec3& world)
	{
		Mat4 view;
		view.m = c.view;
		const Vec3 p = view.Point(world);
		const float d = -p.z;
		return {(p.x - c.proj[0] * d) * c.proj[2] - d, (c.proj[1] * d - p.y) * c.proj[3] - d, kNearM, d};
	}

	size_t PackGpuVertices(const Mesh& mesh, float* out)
	{
		if (!mesh.positions || !mesh.normals || !mesh.colors)
			return 0;
		const size_t n = std::min({mesh.positions->size(), mesh.normals->size(), mesh.colors->size()}) / 3;
		const float* P = mesh.positions->data();
		const float* N = mesh.normals->data();
		const float* C = mesh.colors->data();
#ifdef VR_NEARFIELD_PLANTED_GPU_NO_TINT
		const std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
#else
		const std::array<float, 3> tint = mesh.tint;
#endif
		for (size_t v = 0; v < n; v++)
		{
			float* o = out + v * kGpuVertexFloats;
			o[0] = P[v * 3];
			o[1] = P[v * 3 + 1];
			o[2] = P[v * 3 + 2];
			o[3] = N[v * 3];
			o[4] = N[v * 3 + 1];
			o[5] = N[v * 3 + 2];
			o[6] = C[v * 3] * tint[0];
			o[7] = C[v * 3 + 1] * tint[1];
			o[8] = C[v * 3 + 2] * tint[2];
			const bool textured = mesh.uvs && mesh.texture && mesh.texture->Valid() && mesh.uvs->size() >= (v + 1) * 2;
			o[9] = textured ? (*mesh.uvs)[v * 2] : kGpuNoUv;
			o[10] = textured ? (*mesh.uvs)[v * 2 + 1] : kGpuNoUv;
		}
		return n;
	}

	std::string GpuVertexGlsl()
	{
		char buf[3072];
		std::snprintf(buf, sizeof(buf),
			"#version 450\n"
			"layout(location = 0) in vec3 in_pos;\n"
			"layout(location = 1) in vec3 in_nrm;\n"
			"layout(location = 2) in vec3 in_col;\n"
			"layout(location = 3) in vec2 in_uv;\n"
			"layout(push_constant, std430) uniform PC { mat4 view; vec4 proj; } pc;\n"
			"layout(location = 0) out vec3 v_col;\n"
			"layout(location = 1) out vec3 v_dif;\n"
			"layout(location = 2) out vec2 v_uv;\n"
			"vec3 safe_normalize(vec3 a) { float l = length(a); return l > 1e-12 ? a / l : vec3(0.0); }\n"
			"void main() {\n"
			"    vec3 p = (pc.view * vec4(in_pos, 1.0)).xyz;\n"
			"    vec3 nv = safe_normalize(mat3(pc.view) * in_nrm);\n"
			"    vec3 to_eye = safe_normalize(-p);\n"
			"    if (dot(nv, to_eye) < 0.0) nv = -nv;\n"
			"    vec3 L = safe_normalize(vec3(%.9g, %.9g, %.9g));\n"
			"    float ndl = max(0.0, dot(nv, L));\n"
			"    vec3 hv = safe_normalize(L + to_eye);\n"
			"    float spec = pow(max(0.0, dot(nv, hv)), %.9g) * %.9g;\n"
			"    float rim = pow(1.0 - clamp(dot(nv, to_eye), 0.0, 1.0), %.9g) * %.9g;\n"
			"    float shade = %.9g + %.9g * ndl;\n"
			"    v_col = in_col * shade + vec3(spec) + rim * vec3(%.9g, %.9g, %.9g);\n"
			"    v_dif = in_col * shade;\n"
			"    v_uv = in_uv;\n"
			"    float d = -p.z;\n"
			"    gl_Position = vec4((p.x - pc.proj.x * d) * pc.proj.z - d, (pc.proj.y * d - p.y) * pc.proj.w - d, %.9g, d);\n"
			"}\n",
			kLightX, kLightY, kLightZ, kSpecPower, kSpec, kRimPower, kRim, kAmbient, kDiffuse, kRimR, kRimG, kRimB, kNearM);
		return buf;
	}

	std::string GpuFragmentGlsl(bool encode_srgb)
	{
		std::string s =
			"#version 450\n"
			"layout(location = 0) in vec3 v_col;\n"
			"layout(location = 1) in vec3 v_dif;\n"
			"layout(location = 2) in vec2 v_uv;\n"
			"layout(set = 0, binding = 0) uniform sampler2D hand_tex;\n"
			"layout(location = 0) out vec4 o_col;\n"
			"float enc(float l) { l = clamp(l, 0.0, 1.0); return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }\n"
			"void main() {\n"
			"    vec3 c = v_col;\n"
			"    if (v_uv.x > -1.0e5) c = v_dif * texture(hand_tex, v_uv).rgb + (v_col - v_dif);\n"
			"    c = clamp(c, 0.0, 1.0);\n";
		if (encode_srgb)
			s += "    c = vec3(enc(c.r), enc(c.g), enc(c.b));\n";
		s += "    o_col = vec4(c, 1.0);\n"
			 "}\n";
		return s;
	}

	bool HandsModel::Load(const std::string& resources_dir, Hands hands, std::string* error)
	{
		m_loaded = false;
		m_hands = hands;
		if (hands == Hands::Off)
		{
			if (error)
				*error = "hands are off";
			return false;
		}
		for (int i = 0; i < 2; i++)
		{
			const std::string path = resources_dir + "/" + AssetRelativePath(hands, i == 1);
			if (!HandsGltf::LoadGlbFile(path, &m_asset[i], error))
				return false;
			if (m_asset[i].right != (i == 1))
			{
				if (error)
					*error = path + ": the file's hand is the " + (m_asset[i].right ? "right" : "left") + " one";
				return false;
			}
			m_glove_from_grip[i] = HandsGltf::GripInAsset(m_asset[i]).RigidInverse();
			m_asset_from_grip[i] = m_glove_from_grip[i];
			m_game[i].reset();
		}
		if (hands == Hands::Game)
			HandCapture::Global().SetReferenceHands(m_asset[0], m_asset[1]);
		m_loaded = true;
		return true;
	}

	bool HandsModel::RefreshGameHands()
	{
		if (!m_loaded || m_hands != Hands::Game)
			return false;
		HandCapture::Module& cap = HandCapture::Global();
		const bool on = cap.Enabled();
		bool changed = false;
		for (int i = 0; i < 2; i++)
		{
#ifdef VR_HANDCAP_PLANTED_MODEL_KEEPS_GLOVE
			std::shared_ptr<const HandsGltf::HandAsset> want;
			(void)on;
#else
			std::shared_ptr<const HandsGltf::HandAsset> want = on ? cap.Published(i) : nullptr;
#endif
			if (want && want->right != (i == 1))
				want.reset();
			if (want == m_game[i])
				continue;
			m_game[i] = std::move(want);
			m_asset_from_grip[i] = m_game[i] ? HandsGltf::GripInAsset(*m_game[i]).RigidInverse() : m_glove_from_grip[i];
			changed = true;
		}
		return changed;
	}

	size_t HandsModel::Update(const std::array<HandDraw, 2>& draw, std::array<Mesh, 2>* out)
	{
		size_t n = 0;
		if (!m_loaded)
			return 0;
		for (int i = 0; i < 2; i++)
		{
			if (!draw[i].draw)
				continue;
			const HandsGltf::HandAsset& a = Asset(i);
			if (draw[i].from_joints)
			{
#if defined(VR_NEARFIELD_PLANTED_JOINT_NOIBM)
				HandsGltf::Skin(a, draw[i].joint_poses, Mat4::Identity(), &m_pos[i], &m_nrm[i]);
#elif defined(VR_NEARFIELD_PLANTED_JOINT_PLACED_TWICE)
				HandsGltf::Skin(a, HandsGltf::SkinMatricesFromJointPoses(a, draw[i].joint_poses), m_asset_from_grip[i],
					&m_pos[i], &m_nrm[i]);
#else
				HandsGltf::Skin(a, HandsGltf::SkinMatricesFromJointPoses(a, draw[i].joint_poses), Mat4::Identity(),
					&m_pos[i], &m_nrm[i]);
#endif
			}
			else
			{
				const HandsGltf::JointAngles angles = HandsGltf::Lerp(HandsGltf::PoseAngles(HandsGltf::Pose::Open, a.right),
					HandsGltf::PoseAngles(HandsGltf::Pose::Fist, a.right), draw[i].curl);
				const auto fk = HandsGltf::ForwardKinematics(a, angles);
				HandsGltf::Skin(a, fk, draw[i].world_from_grip * m_asset_from_grip[i], &m_pos[i], &m_nrm[i]);
			}
			Mesh& m = (*out)[n++];
			m.positions = &m_pos[i];
			m.normals = &m_nrm[i];
			m.colors = &a.colors;
			m.indices = &a.indices;
			m.tint = {a.base_color[0], a.base_color[1], a.base_color[2]};
			m.uvs = a.Textured() ? &a.uvs : nullptr;
			m.texture = a.Textured() ? &a.texture : nullptr;
		}
		return n;
	}
}
