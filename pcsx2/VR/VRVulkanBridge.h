// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#ifdef ENABLE_VULKAN

#include "GS/Renderers/Vulkan/VKLoader.h"

#include "common/Pcsx2Defs.h"

namespace VR
{
	bool BeginVulkanBootstrap();
	bool VulkanBootstrapActive();

	bool CreateVulkanInstanceThroughXR(const VkInstanceCreateInfo* ci, VkInstance* out);

	VkPhysicalDevice GetXrVulkanPhysicalDevice(VkInstance instance);

	bool CreateVulkanDeviceThroughXR(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* ci, VkDevice* out);

	bool OnVulkanDeviceCreated(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
		u32 queue_family, VkQueue queue);

	void OnGSDeviceDestroyed();
}

#endif
