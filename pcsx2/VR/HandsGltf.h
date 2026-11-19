// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace VR::HandsGltf
{
	inline constexpr int kJointCount = 26;
	inline constexpr u32 kMaxTriangles = 10000;

	enum Joint : int
	{
		Palm = 0,
		Wrist = 1,
		ThumbMetacarpal = 2,
		ThumbProximal = 3,
		ThumbDistal = 4,
		ThumbTip = 5,
		IndexMetacarpal = 6,
		IndexProximal = 7,
		IndexIntermediate = 8,
		IndexDistal = 9,
		IndexTip = 10,
		MiddleMetacarpal = 11,
		MiddleProximal = 12,
		MiddleIntermediate = 13,
		MiddleDistal = 14,
		MiddleTip = 15,
		RingMetacarpal = 16,
		RingProximal = 17,
		RingIntermediate = 18,
		RingDistal = 19,
		RingTip = 20,
		LittleMetacarpal = 21,
		LittleProximal = 22,
		LittleIntermediate = 23,
		LittleDistal = 24,
		LittleTip = 25,
	};

	const char* JointName(int joint);
	int JointParent(int joint);

	struct Vec3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	struct Mat4
	{
		std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		static Mat4 Identity() { return Mat4{}; }
		static Mat4 Translation(const Vec3& t);
		static Mat4 Rotation(const Vec3& axis, float radians);
		static Mat4 FromBasis(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& origin);
		static Mat4 FromPose(const std::array<float, 4>& q_xyzw, const std::array<float, 3>& p);
		Mat4 operator*(const Mat4& o) const;
		Vec3 Point(const Vec3& p) const;
		Vec3 Dir(const Vec3& d) const;
		Mat4 RigidInverse() const;
		bool Inverse(Mat4* out) const;
		Vec3 Column(int c) const { return {m[c * 4 + 0], m[c * 4 + 1], m[c * 4 + 2]}; }
	};

	struct HandTexture
	{
		u32 width = 0, height = 0;
		bool clamp_u = false, clamp_v = false;
		std::vector<u32> rgba;
		bool Valid() const { return width != 0 && height != 0 && rgba.size() == static_cast<size_t>(width) * height; }
	};

	struct HandAsset
	{
		bool right = false;
		u32 vertex_count = 0;
		std::vector<float> positions;
		std::vector<float> normals;
		std::vector<float> colors;
		std::vector<u8> joints;
		std::vector<float> weights;
		std::vector<u32> indices;
		std::array<Mat4, kJointCount> inverse_bind{};
		std::array<Vec3, kJointCount> rest{};
		std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
		bool double_sided = false;
		std::vector<float> uvs;
		HandTexture texture;
		bool Textured() const { return texture.Valid() && uvs.size() == static_cast<size_t>(vertex_count) * 2; }
		u32 TriangleCount() const { return static_cast<u32>(indices.size() / 3); }
	};

	bool ParseGlb(const void* data, size_t size, HandAsset* out, std::string* error);
	bool LoadGlbFile(const std::string& path, HandAsset* out, std::string* error);

	struct JointAngles
	{
		std::array<std::array<float, 2>, kJointCount> deg{};
	};
	enum class Pose
	{
		Open,
		Fist,
		Pinch,
	};
	JointAngles PoseAngles(Pose pose, bool right);
	JointAngles Lerp(const JointAngles& a, const JointAngles& b, float t);

	std::array<Mat4, kJointCount> ForwardKinematics(const HandAsset& hand, const JointAngles& angles);
	Vec3 PosedJoint(const HandAsset& hand, const std::array<Mat4, kJointCount>& fk, int joint);

	std::array<Mat4, kJointCount> SkinMatricesFromJointPoses(const HandAsset& hand, const std::array<Mat4, kJointCount>& poses);
	std::array<Mat4, kJointCount> JointPosesFromFK(const HandAsset& hand, const std::array<Mat4, kJointCount>& fk);
	u32 WeightedJoints(const HandAsset& hand);

	void Skin(const HandAsset& hand, const std::array<Mat4, kJointCount>& fk, const Mat4& world_from_asset,
		std::vector<float>* out_positions, std::vector<float>* out_normals);

	Mat4 GripInAsset(const HandAsset& hand);
}
