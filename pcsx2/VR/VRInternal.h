// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#ifdef ENABLE_VULKAN

#include "GS/Renderers/Vulkan/VKLoader.h"

namespace VR::Internal
{
	struct VulkanHandles
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physical_device = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		u32 queue_family = 0;

		bool IsValid() const { return device != VK_NULL_HANDLE; }
		void Clear() { *this = VulkanHandles(); }
	};

	VulkanHandles& GetVulkanHandles();
}

#endif
