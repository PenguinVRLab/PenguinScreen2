// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <openxr/openxr.h>

#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/VKLoader.h"
#endif

namespace VR::XRSession
{

	bool CreateInstanceAndSystem();

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

	bool IsLost();

	const char* GetStateName();
}
