// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HandsGltf.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

namespace VR::HandsGltf
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		const char* const kJointNames[kJointCount] = {
			"XR_HAND_JOINT_PALM_EXT",
			"XR_HAND_JOINT_WRIST_EXT",
			"XR_HAND_JOINT_THUMB_METACARPAL_EXT",
			"XR_HAND_JOINT_THUMB_PROXIMAL_EXT",
			"XR_HAND_JOINT_THUMB_DISTAL_EXT",
			"XR_HAND_JOINT_THUMB_TIP_EXT",
			"XR_HAND_JOINT_INDEX_METACARPAL_EXT",
			"XR_HAND_JOINT_INDEX_PROXIMAL_EXT",
			"XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT",
			"XR_HAND_JOINT_INDEX_DISTAL_EXT",
			"XR_HAND_JOINT_INDEX_TIP_EXT",
			"XR_HAND_JOINT_MIDDLE_METACARPAL_EXT",
			"XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT",
			"XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT",
			"XR_HAND_JOINT_MIDDLE_DISTAL_EXT",
			"XR_HAND_JOINT_MIDDLE_TIP_EXT",
			"XR_HAND_JOINT_RING_METACARPAL_EXT",
			"XR_HAND_JOINT_RING_PROXIMAL_EXT",
			"XR_HAND_JOINT_RING_INTERMEDIATE_EXT",
			"XR_HAND_JOINT_RING_DISTAL_EXT",
			"XR_HAND_JOINT_RING_TIP_EXT",
			"XR_HAND_JOINT_LITTLE_METACARPAL_EXT",
			"XR_HAND_JOINT_LITTLE_PROXIMAL_EXT",
			"XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT",
			"XR_HAND_JOINT_LITTLE_DISTAL_EXT",
			"XR_HAND_JOINT_LITTLE_TIP_EXT",
		};

		constexpr int kParents[kJointCount] = {
			Wrist, -1,
			Wrist, ThumbMetacarpal, ThumbProximal, ThumbDistal,
			Wrist, IndexMetacarpal, IndexProximal, IndexIntermediate, IndexDistal,
			Wrist, MiddleMetacarpal, MiddleProximal, MiddleIntermediate, MiddleDistal,
			Wrist, RingMetacarpal, RingProximal, RingIntermediate, RingDistal,
			Wrist, LittleMetacarpal, LittleProximal, LittleIntermediate, LittleDistal,
		};

		constexpr int kOrder[kJointCount] = {
			Wrist, Palm,
			ThumbMetacarpal, ThumbProximal, ThumbDistal, ThumbTip,
			IndexMetacarpal, IndexProximal, IndexIntermediate, IndexDistal, IndexTip,
			MiddleMetacarpal, MiddleProximal, MiddleIntermediate, MiddleDistal, MiddleTip,
			RingMetacarpal, RingProximal, RingIntermediate, RingDistal, RingTip,
			LittleMetacarpal, LittleProximal, LittleIntermediate, LittleDistal, LittleTip,
		};

		Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
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

		bool Fail(std::string* error, const std::string& why)
		{
			if (error)
				*error = why;
			return false;
		}

		int OnlyChild(int j)
		{
			int found = -1, count = 0;
			for (int k = 0; k < kJointCount; k++)
			{
				if (k != Palm && kParents[k] == j)
				{
					found = k;
					count++;
				}
			}
			return count == 1 ? found : -1;
		}
	}

	const char* JointName(int joint)
	{
		return (joint >= 0 && joint < kJointCount) ? kJointNames[joint] : nullptr;
	}

	int JointParent(int joint)
	{
		return (joint >= 0 && joint < kJointCount) ? kParents[joint] : -1;
	}

	Mat4 Mat4::Translation(const Vec3& t)
	{
		Mat4 r;
		r.m[12] = t.x;
		r.m[13] = t.y;
		r.m[14] = t.z;
		return r;
	}

	Mat4 Mat4::Rotation(const Vec3& axis_in, float radians)
	{
		const Vec3 a = Normalize(axis_in);
		const float c = std::cos(radians), s = std::sin(radians), t = 1.0f - c;
		Mat4 r;
		r.m[0] = t * a.x * a.x + c;
		r.m[1] = t * a.x * a.y + s * a.z;
		r.m[2] = t * a.x * a.z - s * a.y;
		r.m[4] = t * a.x * a.y - s * a.z;
		r.m[5] = t * a.y * a.y + c;
		r.m[6] = t * a.y * a.z + s * a.x;
		r.m[8] = t * a.x * a.z + s * a.y;
		r.m[9] = t * a.y * a.z - s * a.x;
		r.m[10] = t * a.z * a.z + c;
		return r;
	}

	Mat4 Mat4::FromBasis(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& o)
	{
		Mat4 r;
		r.m = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0, o.x, o.y, o.z, 1};
		return r;
	}

	Mat4 Mat4::FromPose(const std::array<float, 4>& q, const std::array<float, 3>& p)
	{
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		Mat4 r;
		r.m[0] = 1 - 2 * (y * y + z * z);
		r.m[1] = 2 * (x * y + z * w);
		r.m[2] = 2 * (x * z - y * w);
		r.m[4] = 2 * (x * y - z * w);
		r.m[5] = 1 - 2 * (x * x + z * z);
		r.m[6] = 2 * (y * z + x * w);
		r.m[8] = 2 * (x * z + y * w);
		r.m[9] = 2 * (y * z - x * w);
		r.m[10] = 1 - 2 * (x * x + y * y);
		r.m[12] = p[0];
		r.m[13] = p[1];
		r.m[14] = p[2];
		return r;
	}

	Mat4 Mat4::operator*(const Mat4& o) const
	{
		Mat4 r;
		for (int c = 0; c < 4; c++)
			for (int row = 0; row < 4; row++)
			{
				float s = 0.0f;
				for (int k = 0; k < 4; k++)
					s += m[k * 4 + row] * o.m[c * 4 + k];
				r.m[c * 4 + row] = s;
			}
		return r;
	}

	Vec3 Mat4::Point(const Vec3& p) const
	{
		return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
			m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
	}

	Vec3 Mat4::Dir(const Vec3& d) const
	{
		return {m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
			m[2] * d.x + m[6] * d.y + m[10] * d.z};
	}

	Mat4 Mat4::RigidInverse() const
	{
		Mat4 r;
		for (int c = 0; c < 3; c++)
			for (int row = 0; row < 3; row++)
				r.m[c * 4 + row] = m[row * 4 + c];
		const Vec3 t{m[12], m[13], m[14]};
		const Vec3 ti = r.Dir(t);
		r.m[12] = -ti.x;
		r.m[13] = -ti.y;
		r.m[14] = -ti.z;
		return r;
	}

	bool Mat4::Inverse(Mat4* out) const
	{
		const float* a = m.data();
		float inv[16];
		inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
		inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
		inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
		inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
		inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
		inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
		inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
		inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
		inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
		inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
		inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
		inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
		inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
		inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
		inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
		inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
		const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
		if (std::fabs(det) < 1e-20f)
		{
			*out = Mat4{};
			return false;
		}
		for (int i = 0; i < 16; i++)
			out->m[i] = inv[i] / det;
		return true;
	}

	bool ParseGlb(const void* data, size_t size, HandAsset* out, std::string* error)
	{
		*out = HandAsset{};
		cgltf_options options = {};
		options.type = cgltf_file_type_glb;
		cgltf_data* doc = nullptr;
		if (cgltf_parse(&options, data, size, &doc) != cgltf_result_success || !doc)
			return Fail(error, "not a readable glTF 2.0 binary");
		struct Free
		{
			cgltf_data* d;
			~Free() { cgltf_free(d); }
		} free_doc{doc};
		if (cgltf_load_buffers(&options, doc, nullptr) != cgltf_result_success)
			return Fail(error, "the embedded buffer could not be read (an external buffer is not part of the contract)");
		if (cgltf_validate(doc) != cgltf_result_success)
			return Fail(error, "the file fails cgltf_validate (an accessor or view out of range)");
		if (doc->extensions_required_count != 0)
			return Fail(error, "the file requires an extension (the contract has none)");
		if (doc->skins_count != 1)
			return Fail(error, "want exactly one skin, found " + std::to_string(doc->skins_count));

		const cgltf_node* mesh_node = nullptr;
		for (cgltf_size i = 0; i < doc->nodes_count; i++)
		{
			if (doc->nodes[i].mesh && doc->nodes[i].skin)
			{
				if (mesh_node)
					return Fail(error, "more than one skinned mesh node");
				mesh_node = &doc->nodes[i];
			}
		}
		if (!mesh_node || mesh_node->mesh->primitives_count != 1)
			return Fail(error, "want one skinned mesh with one primitive");
		const cgltf_primitive& prim = mesh_node->mesh->primitives[0];
		if (prim.type != cgltf_primitive_type_triangles || !prim.indices)
			return Fail(error, "the primitive is not an indexed TRIANGLES list");

		const cgltf_skin& skin = *mesh_node->skin;
		if (skin.joints_count != kJointCount)
			return Fail(error, "the skin has " + std::to_string(skin.joints_count) + " joints, want 26");
		std::array<int, kJointCount> skin_to_xr{};
		std::array<bool, kJointCount> seen{};
		for (cgltf_size i = 0; i < skin.joints_count; i++)
		{
			const char* name = skin.joints[i]->name;
			int xr = -1;
			for (int j = 0; name && j < kJointCount; j++)
				if (std::strcmp(name, kJointNames[j]) == 0)
					xr = j;
			if (xr < 0)
				return Fail(error, std::string("a skin joint is not an XR_EXT_hand_tracking joint name: ") + (name ? name : "(unnamed)"));
			if (seen[xr])
				return Fail(error, std::string("joint named twice: ") + name);
			seen[xr] = true;
			skin_to_xr[i] = xr;
		}
		if (!skin.inverse_bind_matrices || skin.inverse_bind_matrices->count != kJointCount)
			return Fail(error, "the skin has no inverse bind matrices for its 26 joints");
		for (cgltf_size i = 0; i < kJointCount; i++)
		{
			Mat4& ibm = out->inverse_bind[skin_to_xr[i]];
			if (!cgltf_accessor_read_float(skin.inverse_bind_matrices, i, ibm.m.data(), 16))
				return Fail(error, "an inverse bind matrix could not be read");
			Mat4 rest;
			if (!ibm.Inverse(&rest))
				return Fail(error, std::string("a singular inverse bind matrix: ") + kJointNames[skin_to_xr[i]]);
			out->rest[skin_to_xr[i]] = {rest.m[12], rest.m[13], rest.m[14]};
		}

		const cgltf_accessor *pos = nullptr, *nrm = nullptr, *col = nullptr, *jnt = nullptr, *wgt = nullptr;
		for (cgltf_size i = 0; i < prim.attributes_count; i++)
		{
			const cgltf_attribute& a = prim.attributes[i];
			if (a.index != 0)
				continue;
			switch (a.type)
			{
				case cgltf_attribute_type_position: pos = a.data; break;
				case cgltf_attribute_type_normal: nrm = a.data; break;
				case cgltf_attribute_type_color: col = a.data; break;
				case cgltf_attribute_type_joints: jnt = a.data; break;
				case cgltf_attribute_type_weights: wgt = a.data; break;
				default: break;
			}
		}
		if (!pos || !nrm || !col || !jnt || !wgt)
			return Fail(error, "want POSITION, NORMAL, COLOR_0, JOINTS_0 and WEIGHTS_0");
		const cgltf_size n = pos->count;
		if (n == 0 || nrm->count != n || col->count != n || jnt->count != n || wgt->count != n)
			return Fail(error, "the vertex attributes disagree on the vertex count");
		const cgltf_size col_comps = cgltf_num_components(col->type);
		if (col_comps != 3 && col_comps != 4)
			return Fail(error, "COLOR_0 is not VEC3 or VEC4");
		out->vertex_count = static_cast<u32>(n);
		out->positions.resize(n * 3);
		out->normals.resize(n * 3);
		out->colors.resize(n * 3);
		out->joints.resize(n * 4);
		out->weights.resize(n * 4);
		for (cgltf_size v = 0; v < n; v++)
		{
			float c[4] = {1, 1, 1, 1};
			cgltf_uint ji[4] = {};
			float w[4] = {};
			if (!cgltf_accessor_read_float(pos, v, &out->positions[v * 3], 3) ||
				!cgltf_accessor_read_float(nrm, v, &out->normals[v * 3], 3) ||
				!cgltf_accessor_read_float(col, v, c, col_comps) || !cgltf_accessor_read_uint(jnt, v, ji, 4) ||
				!cgltf_accessor_read_float(wgt, v, w, 4))
				return Fail(error, "a vertex attribute could not be read");
			out->colors[v * 3 + 0] = c[0];
			out->colors[v * 3 + 1] = c[1];
			out->colors[v * 3 + 2] = c[2];
			float sum = 0.0f;
			for (int k = 0; k < 4; k++)
			{
				if (ji[k] >= kJointCount)
					return Fail(error, "a vertex names a joint index outside the skin's 26");
				out->joints[v * 4 + k] = static_cast<u8>(skin_to_xr[ji[k]]);
				out->weights[v * 4 + k] = w[k];
				sum += w[k];
			}
			if (!(sum > 0.5f))
				return Fail(error, "a vertex carries no weight (every vertex must be weighted)");
			for (int k = 0; k < 4; k++)
				out->weights[v * 4 + k] /= sum;
		}
		const cgltf_size idx_count = prim.indices->count;
		if (idx_count % 3 != 0 || idx_count / 3 > kMaxTriangles)
			return Fail(error, "the index count is not whole triangles within the 10 000 budget");
		out->indices.resize(idx_count);
		for (cgltf_size i = 0; i < idx_count; i++)
		{
			const cgltf_size ix = cgltf_accessor_read_index(prim.indices, i);
			if (ix >= n)
				return Fail(error, "an index is out of range");
			out->indices[i] = static_cast<u32>(ix);
		}
		if (prim.material)
		{
			if (prim.material->has_pbr_metallic_roughness)
				std::memcpy(out->base_color.data(), prim.material->pbr_metallic_roughness.base_color_factor, sizeof(float) * 4);
			out->double_sided = prim.material->double_sided != 0;
		}

		bool side_known = false;
		if (doc->scene && doc->scene->extras.data)
		{
			const std::string_view ex(doc->scene->extras.data);
			if (ex.find("\"xr_hand\"") != std::string_view::npos)
			{
				const size_t at = ex.find("\"xr_hand\"");
				const std::string_view tail = ex.substr(at);
				if (tail.find("\"right\"") != std::string_view::npos && (tail.find("\"left\"") == std::string_view::npos ||
																			   tail.find("\"right\"") < tail.find("\"left\"")))
					out->right = true;
				side_known = true;
			}
		}
		if (!side_known)
			out->right = out->rest[ThumbTip].x < 0.0f;
		return true;
	}

	bool LoadGlbFile(const std::string& path, HandAsset* out, std::string* error)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f)
			return Fail(error, "cannot open " + path);
		const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (!ParseGlb(bytes.data(), bytes.size(), out, error))
		{
			if (error)
				*error = path + ": " + *error;
			return false;
		}
		return true;
	}

	JointAngles PoseAngles(Pose pose, bool right)
	{
		JointAngles a;
		const float side = right ? 1.0f : -1.0f;
		auto set = [&a](int j, float flex, float swing = 0.0f) { a.deg[j] = {flex, swing}; };
		switch (pose)
		{
			case Pose::Open:
				break;
			case Pose::Fist:
				set(IndexMetacarpal, 0.0f), set(IndexProximal, 80.0f), set(IndexIntermediate, 95.0f), set(IndexDistal, 45.0f);
				set(MiddleMetacarpal, 0.0f), set(MiddleProximal, 82.0f), set(MiddleIntermediate, 96.0f), set(MiddleDistal, 45.0f);
				set(RingMetacarpal, 4.0f), set(RingProximal, 84.0f), set(RingIntermediate, 96.0f), set(RingDistal, 46.0f);
				set(LittleMetacarpal, 8.0f), set(LittleProximal, 86.0f), set(LittleIntermediate, 94.0f), set(LittleDistal, 46.0f);
				set(ThumbMetacarpal, 19.0f, 20.75f * side), set(ThumbProximal, 26.5f), set(ThumbDistal, 69.5f);
				break;
			case Pose::Pinch:
				set(IndexMetacarpal, 0.0f), set(IndexProximal, 42.0f), set(IndexIntermediate, 52.0f), set(IndexDistal, 28.0f);
				set(MiddleMetacarpal, 0.0f), set(MiddleProximal, 22.0f), set(MiddleIntermediate, 30.0f), set(MiddleDistal, 16.0f);
				set(RingMetacarpal, 2.0f), set(RingProximal, 30.0f), set(RingIntermediate, 36.0f), set(RingDistal, 18.0f);
				set(LittleMetacarpal, 4.0f), set(LittleProximal, 38.0f), set(LittleIntermediate, 40.0f), set(LittleDistal, 20.0f);
				set(ThumbMetacarpal, 30.0f, 9.0f * side), set(ThumbProximal, 20.0f), set(ThumbDistal, 35.75f);
				break;
		}
		return a;
	}

	JointAngles Lerp(const JointAngles& a, const JointAngles& b, float t)
	{
		t = std::clamp(std::isfinite(t) ? t : 0.0f, 0.0f, 1.0f);
		JointAngles r;
		for (int j = 0; j < kJointCount; j++)
			for (int k = 0; k < 2; k++)
				r.deg[j][k] = a.deg[j][k] + (b.deg[j][k] - a.deg[j][k]) * t;
		return r;
	}

	std::array<Mat4, kJointCount> ForwardKinematics(const HandAsset& h, const JointAngles& angles)
	{
		const Vec3& P_ip = h.rest[IndexProximal];
		const Vec3& P_lp = h.rest[LittleProximal];
		const Vec3 across = Sub(P_ip, P_lp);
		const Vec3 along = Sub(h.rest[MiddleProximal], h.rest[Wrist]);
		const Vec3 c = Normalize(Cross(across, along));
		const Vec3 back = h.right ? Mul(c, -1.0f) : c;
		const Vec3 thumbward = Normalize(across);

		std::array<Mat4, kJointCount> T{};
		for (int oi = 0; oi < kJointCount; oi++)
		{
			const int n = kOrder[oi];
			const int p = kParents[n];
			const Mat4 base = p < 0 ? Mat4::Identity() : T[p];
			Vec3 d;
			if (n == Wrist || n == Palm)
				d = Normalize(Sub(h.rest[MiddleProximal], h.rest[n]));
			else if (const int ch = OnlyChild(n); ch >= 0)
				d = Normalize(Sub(h.rest[ch], h.rest[n]));
			else
				d = Normalize(Sub(h.rest[n], h.rest[p]));
			Vec3 b = back;
			if (n >= ThumbMetacarpal && n <= ThumbTip)
			{
				b = Add(Mul(back, 0.8f), Mul(thumbward, 0.6f));
				b = Normalize(Sub(b, Mul(d, Dot(b, d))));
			}
#ifdef VR_HANDS_PLANTED_FLEX_SIGN
			const Vec3 flex_axis = Normalize(Cross(d, b));
#else
			const Vec3 flex_axis = Normalize(Cross(d, Mul(b, -1.0f)));
#endif
			const float fl = angles.deg[n][0] * (kPi / 180.0f);
			const float sw = angles.deg[n][1] * (kPi / 180.0f);
			const Mat4 R = Mat4::Rotation(back, sw) * Mat4::Rotation(flex_axis, fl);
			const Vec3& P = h.rest[n];
			T[n] = base * Mat4::Translation(P) * R * Mat4::Translation(Mul(P, -1.0f));
		}
		return T;
	}

	Vec3 PosedJoint(const HandAsset& hand, const std::array<Mat4, kJointCount>& fk, int joint)
	{
		return fk[joint].Point(hand.rest[joint]);
	}

	void Skin(const HandAsset& h, const std::array<Mat4, kJointCount>& fk, const Mat4& world_from_asset,
		std::vector<float>* out_p, std::vector<float>* out_n)
	{
		std::array<Mat4, kJointCount> M;
		for (int j = 0; j < kJointCount; j++)
		{
#ifdef VR_HANDS_PLANTED_NO_SKIN
			M[j] = world_from_asset;
			(void)fk;
#else
			M[j] = world_from_asset * fk[j];
#endif
		}
		out_p->resize(static_cast<size_t>(h.vertex_count) * 3);
		out_n->resize(static_cast<size_t>(h.vertex_count) * 3);
		for (u32 v = 0; v < h.vertex_count; v++)
		{
			const Vec3 p{h.positions[v * 3], h.positions[v * 3 + 1], h.positions[v * 3 + 2]};
			const Vec3 nn{h.normals[v * 3], h.normals[v * 3 + 1], h.normals[v * 3 + 2]};
			Vec3 sp{}, sn{};
			for (int k = 0; k < 4; k++)
			{
				const float w = h.weights[v * 4 + k];
				if (w <= 0.0f)
					continue;
				const Mat4& m = M[h.joints[v * 4 + k]];
				sp = Add(sp, Mul(m.Point(p), w));
				sn = Add(sn, Mul(m.Dir(nn), w));
			}
			sn = Normalize(sn);
			(*out_p)[v * 3 + 0] = sp.x;
			(*out_p)[v * 3 + 1] = sp.y;
			(*out_p)[v * 3 + 2] = sp.z;
			(*out_n)[v * 3 + 0] = sn.x;
			(*out_n)[v * 3 + 1] = sn.y;
			(*out_n)[v * 3 + 2] = sn.z;
		}
	}

	std::array<Mat4, kJointCount> SkinMatricesFromJointPoses(const HandAsset& h, const std::array<Mat4, kJointCount>& poses)
	{
		std::array<Mat4, kJointCount> M;
		for (int j = 0; j < kJointCount; j++)
			M[j] = poses[j] * h.inverse_bind[j];
		return M;
	}

	u32 WeightedJoints(const HandAsset& h)
	{
		u32 mask = 0;
		for (size_t k = 0; k < h.joints.size() && k < h.weights.size(); k++)
		{
			if (h.weights[k] > 0.0f && h.joints[k] < kJointCount)
				mask |= 1u << h.joints[k];
		}
		return mask;
	}

	std::array<Mat4, kJointCount> JointPosesFromFK(const HandAsset& h, const std::array<Mat4, kJointCount>& fk)
	{
		std::array<Mat4, kJointCount> P;
		for (int j = 0; j < kJointCount; j++)
		{
			Mat4 rest;
			if (!h.inverse_bind[j].Inverse(&rest))
				rest = Mat4::Translation(h.rest[j]);
			P[j] = fk[j] * rest;
		}
		return P;
	}

	Mat4 GripInAsset(const HandAsset& h)
	{
		const std::array<Mat4, kJointCount> fk = ForwardKinematics(h, PoseAngles(Pose::Fist, h.right));
		const Vec3 across = Sub(h.rest[IndexProximal], h.rest[LittleProximal]);
		const Vec3 along = Sub(h.rest[MiddleProximal], h.rest[Wrist]);
		const Vec3 c = Normalize(Cross(across, along));
		const Vec3 back = h.right ? Mul(c, -1.0f) : c;
		const Vec3 z = Normalize(Mul(across, -1.0f));
		Vec3 x = h.right ? back : Mul(back, -1.0f);
		x = Normalize(Sub(x, Mul(z, Dot(x, z))));
		const Vec3 y = Cross(z, x);

		const int ring[4] = {MiddleProximal, MiddleIntermediate, MiddleDistal, MiddleTip};
		double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sxr = 0, syr = 0, sr = 0;
		float along_z = 0.0f;
		for (int j : ring)
		{
			const Vec3 p = PosedJoint(h, fk, j);
			const double u = Dot(p, x), v = Dot(p, y), r2 = u * u + v * v;
			sxx += u * u, sxy += u * v, syy += v * v, sx += u, sy += v;
			sxr += u * r2, syr += v * r2, sr += r2;
			along_z += Dot(p, z) / 4.0f;
		}
		const double A[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, 4.0}};
		const double B[3] = {sxr, syr, sr};
		const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
						   A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
		double cu = 0.0, cv = 0.0;
		if (std::fabs(det) > 1e-18)
		{
			const double da = B[0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (B[1] * A[2][2] - A[1][2] * B[2]) +
							  A[0][2] * (B[1] * A[2][1] - A[1][1] * B[2]);
			const double db = A[0][0] * (B[1] * A[2][2] - A[1][2] * B[2]) - B[0] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
							  A[0][2] * (A[1][0] * B[2] - B[1] * A[2][0]);
			cu = 0.5 * da / det;
			cv = 0.5 * db / det;
		}
		const Vec3 origin = Add(Add(Mul(x, static_cast<float>(cu)), Mul(y, static_cast<float>(cv))), Mul(z, along_z));
		return Mat4::FromBasis(x, y, z, origin);
	}
}
