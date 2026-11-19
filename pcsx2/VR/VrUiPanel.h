// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VR::VrUiPanel
{
	inline constexpr std::uint32_t kMaxLayers = 1;

	constexpr std::uint32_t LayerCapacity(std::uint32_t screen_layers, std::uint32_t lever_layers, std::uint32_t ui_layers)
	{
		return screen_layers + lever_layers + ui_layers + kMaxLayers;
	}

	inline constexpr std::int64_t kMaxPx = 1024;
	inline constexpr std::size_t kMaxManifestBytes = 262144;
	inline constexpr std::uint64_t kMaxAtlasFileBytes =
		2ull * static_cast<std::uint64_t>(kMaxPx) * (4ull * static_cast<std::uint64_t>(kMaxPx) + 1ull) + 65536ull;

	struct Refusal
	{
		std::string code;
		std::string where;
		std::string detail;
	};

	enum class CellClass
	{
		Lamp,
		Button,
	};

	enum class Anchor
	{
		World,
		Screen,
	};

	struct Sprite
	{
		std::int32_t x = 0, y = 0, w = 0, h = 0;
	};

	struct Cell
	{
		std::string id;
		CellClass cls = CellClass::Lamp;
		std::int32_t x = 0, y = 0, w = 0, h = 0;
		std::int32_t sprite_on = -1, sprite_off = -1;
		std::int32_t sprite_idle = -1, sprite_hover = -1, sprite_pressed = -1;
		bool default_on = false;
		std::string bind;
	};

	struct Placement
	{
		Anchor anchor = Anchor::Screen;
		float offset_m[3] = {0.0f, 0.0f, 0.0f};
		float yaw_deg = 0.0f;
		float width_m = 0.0f;
		float height_m = 0.0f;
	};

	struct Artifact
	{
		std::uint32_t raster_w = 0, raster_h = 0;
		std::uint32_t atlas_w = 0, atlas_h = 0;
		std::string atlas_file;
		std::string atlas_sha256;
		std::string atlas_pixels_sha256;
		std::string preview_file;
		std::string preview_pixels_sha256;
		std::uint32_t preview_w = 0, preview_h = 0;
		std::vector<Sprite> sprites;
		std::vector<Cell> cells;
		Placement placement;
		std::uint32_t layers = 0;
		std::vector<std::uint8_t> atlas;
	};

	bool ParseManifest(std::string_view json, Artifact& out, Refusal& why);

	bool LoadFromBytes(std::string_view manifest_json, const std::vector<std::uint8_t>& atlas_png, Artifact& out,
		Refusal& why);

	bool LoadDir(const std::string& dir, Artifact& out, Refusal& why);

	std::string Sha256Hex(const void* data, std::size_t size);

	using State = std::vector<std::pair<std::string, std::string>>;

	void Compose(const Artifact& a, const State& state, std::vector<std::uint8_t>& out);

	struct Diff
	{
		std::uint32_t max_channel = 0;
		std::uint64_t pixels = 0;
	};
	Diff MaxDiff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b);

	struct QuadPose
	{
		float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float width_m = 0.0f, height_m = 0.0f;
	};
	struct ScreenAnchor
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float yaw = 0.0f;
	};
	QuadPose Place(const Placement& p, const ScreenAnchor& anchor);

	std::uint32_t LayersFor(const Artifact* loaded);
}

#ifdef ENABLE_VULKAN
#include "VR/SwapchainWait.h"

namespace VR::VrUiPanel::Runtime
{
	struct Frame
	{
		std::int64_t swapchain_format = 0;
		bool swap_red_blue = false;
		ScreenAnchor anchor;
		XrSession session = XR_NULL_HANDLE;
		XrSpace space = XR_NULL_HANDLE;
	};

	std::uint32_t Wanted();

	std::uint32_t Build(const Frame& f, SwapchainWait::Budget& budget, XrCompositionLayerQuad* quads,
		const XrCompositionLayerBaseHeader** layers, std::uint32_t& layer_count, std::uint32_t capacity);

	void Shutdown(bool xr_calls);
}
#endif
