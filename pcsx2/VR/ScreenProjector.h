// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#ifdef ENABLE_VULKAN

#include "GS/Renderers/Vulkan/VKLoader.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

class GSTextureVK;

namespace VR::ScreenProjector
{
	inline constexpr u32 kMaxSources = 4;

	struct Source
	{
		GSTextureVK* texture = nullptr;
		u32 layer = 0;
		s32 x = 0, y = 0, w = 0, h = 0;
	};

	struct Cylinder
	{
		XrPosef pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
		float radius = 0.0f;
		float central_angle = 0.0f;
		float aspect_ratio = 0.0f;
		u32 eye_mask = 3;
		int source = 0;
	};

	struct Frame
	{
		XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		XrCompositionLayerProjectionView views[2] = {
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	};

	bool Initialize(VkFormat swapchain_format, bool srgb, float supersample, bool depth_requested);
	void Shutdown();
	bool IsInitialized();

	void Publish(const Source& src, u32 index);

	void FlushPublish();

	bool Failed();

	bool HasSource(u32 index);

	bool Present(XrSpace space, XrTime display_time, const Cylinder* cyls, u32 cyl_count, Frame& out);
}

#endif
