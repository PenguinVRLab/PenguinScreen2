// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/HandsGltf.h"
#include "VR/VRInputState.h"

#include "common/Pcsx2Types.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace VR::NearField
{
	enum class Hands : u8
	{
		Off = 0,
		GloveV1_1 = 1,
		Game = 2,
	};
	const char* HandsName(Hands hands);
	bool ParseHands(std::string_view text, Hands* out);
	std::string AssetRelativePath(Hands hands, bool right);

	void SetProfileHands(Hands hands);
	Hands ProfileHands();

	inline constexpr u32 kLayers = 1;
	constexpr u32 LayerCapacity(u32 screens, u32 levers, u32 ui)
	{
#ifdef VR_NEARFIELD_PLANTED_CAPACITY_SHORT
		return screens + levers + ui;
#else
		return screens + levers + ui + kLayers;
#endif
	}
	bool Fits(u32 layer_count, u32 capacity, u32 runtime_max);

	struct LeverSlot
	{
		bool active = false;
		bool grabbed = false;
		float t = 0.0f;
		float side = 0.0f, height = 0.0f, forward = 0.0f, yaw_deg = 0.0f;
	};
	struct Anchor
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
	};
	struct HandDraw
	{
		bool draw = false;
		bool on_lever = false;
		float curl = 0.0f;
		HandsGltf::Mat4 world_from_grip;
		bool from_joints = false;
		std::array<HandsGltf::Mat4, HandsGltf::kJointCount> joint_poses{};
	};
	struct JointSet
	{
		bool active = false;
		std::array<VRPose, HandsGltf::kJointCount> pose{};
	};
	inline constexpr u32 kAllJointsMask = (1u << HandsGltf::kJointCount) - 1u;
	std::array<HandDraw, 2> PlaceHands(const std::array<VRHandState, 2>& hands, bool actions_active,
		const std::array<LeverSlot, 2>& levers, const Anchor& anchor, const std::array<JointSet, 2>* joints = nullptr,
		const std::array<u32, 2>& weighted = {kAllJointsMask, kAllJointsMask},
		const std::array<bool, 2>& controller_in_hand = {true, true});
	const std::array<JointSet, 2>* JointsForModel(Hands model, const std::array<JointSet, 2>& joints);

	struct EyeView
	{
		std::array<float, 4> orientation{0, 0, 0, 1};
		std::array<float, 3> position{0, 0, 0};
		float angle_left = -0.8f, angle_right = 0.8f, angle_up = 0.8f, angle_down = -0.8f;
	};
	struct Mesh
	{
		const std::vector<float>* positions = nullptr;
		const std::vector<float>* normals = nullptr;
		const std::vector<float>* colors = nullptr;
		const std::vector<u32>* indices = nullptr;
		std::array<float, 3> tint{1, 1, 1};
		const std::vector<float>* uvs = nullptr;
		const HandsGltf::HandTexture* texture = nullptr;
	};
	std::array<float, 3> SampleHandTexture(const HandsGltf::HandTexture& tex, float u, float v);
	struct RasterStats
	{
		u32 covered = 0;
		u32 triangles = 0;
		double clear_ms = 0.0;
		double draw_ms = 0.0;
	};
	inline constexpr float kNearM = 0.05f;
	RasterStats Raster(const EyeView& eye, u32 w, u32 h, const Mesh* meshes, size_t mesh_count, std::vector<u32>* rgba,
		std::vector<float>* depth);

	struct GpuEyeConstants
	{
		std::array<float, 16> view{};
		std::array<float, 4> proj{};
	};
	static_assert(sizeof(GpuEyeConstants) == 80, "the shader's push-constant block is 80 bytes");
	bool MakeGpuEyeConstants(const EyeView& eye, GpuEyeConstants* out);
	std::array<float, 4> GpuClipFromWorld(const GpuEyeConstants& c, const HandsGltf::Vec3& world);
	inline constexpr u32 kGpuVertexFloats = 11;
	inline constexpr float kGpuNoUv = -1.0e6f;
	size_t PackGpuVertices(const Mesh& mesh, float* out);
	std::string GpuVertexGlsl();
	std::string GpuFragmentGlsl(bool encode_srgb);

	class HandsModel
	{
	public:
		bool Load(const std::string& resources_dir, Hands hands, std::string* error);
		bool Loaded() const { return m_loaded; }
		Hands Which() const { return m_hands; }
		bool RefreshGameHands();
		size_t Update(const std::array<HandDraw, 2>& draw, std::array<Mesh, 2>* out);
		const HandsGltf::HandAsset& Asset(int i) const { return m_game[i & 1] ? *m_game[i & 1] : m_asset[i & 1]; }
		bool UsingGameHand(int i) const { return m_game[i & 1] != nullptr; }

	private:
		bool m_loaded = false;
		Hands m_hands = Hands::Off;
		std::array<HandsGltf::HandAsset, 2> m_asset{};
		std::array<HandsGltf::Mat4, 2> m_asset_from_grip{};
		std::array<HandsGltf::Mat4, 2> m_glove_from_grip{};
		std::array<std::shared_ptr<const HandsGltf::HandAsset>, 2> m_game{};
		std::array<std::vector<float>, 2> m_pos{}, m_nrm{};
	};
}
