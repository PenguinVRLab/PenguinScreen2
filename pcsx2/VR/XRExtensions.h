// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <openxr/openxr.h>

#include <vector>

namespace VR::XRExtensions
{
	inline constexpr const char* kVulkanEnable2 = "XR_KHR_vulkan_enable2";
	inline constexpr const char* kVulkanEnable = "XR_KHR_vulkan_enable";
	inline constexpr const char* kFrameController = "XR_VALVE_frame_controller_interaction";

	struct Offered
	{
		bool vulkan_enable2 = false;
		bool cylinder = false;
		bool hand_tracking = false;
		bool frame_controller = false;
		bool hand_interaction = false;
	};

	inline std::vector<const char*> ToEnable(const Offered& o)
	{
		std::vector<const char*> out;
		out.push_back(o.vulkan_enable2 ? kVulkanEnable2 : kVulkanEnable);
		if (o.cylinder)
			out.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
		if (o.hand_tracking)
			out.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
		if (o.frame_controller)
			out.push_back(kFrameController);
#ifndef VR_XRSESSION_PLANTED_HAND_INTERACTION_DROPPED
		if (o.hand_interaction)
			out.push_back(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
#endif
#ifdef VR_XRSESSION_PLANTED_FOUR_SLOTS
		if (out.size() > 4)
			out.resize(4);
#endif
		return out;
	}
}
