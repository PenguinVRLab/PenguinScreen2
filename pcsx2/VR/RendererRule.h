// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "Config.h"

namespace VR::RendererRule
{
	inline constexpr bool kForceVulkanUnderVR = true;

	struct Choice
	{
		GSRendererType renderer;
		bool overridden = false;
		const char* why = nullptr;
	};

	inline Choice ForSession(GSRendererType requested, bool wants_vr, bool vulkan_built)
	{
#ifdef VR_RENDERER_RULE_PLANTED_NONE
		return Choice{requested};
#endif
		if (!wants_vr)
			return Choice{requested};
		switch (requested)
		{
			case GSRendererType::VK:
			case GSRendererType::SW:
			case GSRendererType::Null:
				return Choice{requested};
			default:
				break;
		}
		if (!vulkan_built)
			return Choice{requested, false, "this build has no Vulkan renderer, and VR starts only from Vulkan: VR will not start."};
		if (requested == GSRendererType::Auto)
			return Choice{GSRendererType::VK};
		if (kForceVulkanUnderVR)
			return Choice{GSRendererType::VK, true,
				"the launch asked for VR (--vr), and VR starts only from the Vulkan renderer: using Vulkan instead of the "
				"renderer chosen in Graphics settings for this session (the setting itself is unchanged)."};
		return Choice{requested, false,
			"the launch asked for VR (--vr), but the renderer chosen in Graphics settings is not Vulkan, and VR starts only "
			"from Vulkan: VR will not start. Choose Vulkan in Graphics settings."};
	}

	inline bool VulkanBuilt()
	{
#ifdef ENABLE_VULKAN
		return true;
#else
		return false;
#endif
	}
}
