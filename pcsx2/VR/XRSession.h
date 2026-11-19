// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <openxr/openxr.h>

#include <string>
#include <vector>

#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/VKLoader.h"
#endif

namespace VR::XRSession
{
	bool CreateInstanceAndSystem();

	bool UsesVulkanEnable1();

	void DestroyInstance();

	bool HasInstance();
	XrInstance GetInstance();
	XrSystemId GetSystemId();

#ifdef ENABLE_VULKAN
	bool QueryVulkanGraphicsRequirements();

	bool CreateVulkanInstanceThroughXR(const VkInstanceCreateInfo* ci, VkInstance* out);
	VkPhysicalDevice GetVulkanGraphicsDevice(VkInstance instance);
	bool CreateVulkanDeviceThroughXR(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* ci, VkDevice* out);

	bool CreateSessionVK(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
		u32 queue_family, u32 queue_index);
#endif

	void DestroySession();

	bool HasSession();
	XrSession GetSession();
	XrSpace GetSpace();

	void PumpEvents();

	bool IsSessionRunning();

	bool HasCylinderLayer();

	struct RuntimeLimits
	{
		u32 max_swapchain_width = 0;
		u32 max_swapchain_height = 0;
		u32 max_layer_count = 0;
		bool fov_mutable = false;
		bool fov_mutable_known = false;
	};
	const RuntimeLimits& GetRuntimeLimits();

	bool IsLost();

	bool ConsumeLocalSpaceChange(XrTime display_time);

	void MarkLost();

	void ParkHandlesForAbandonedPacer();

	const char* GetStateName();

	const std::vector<std::string>& EnabledExtensions();
}
