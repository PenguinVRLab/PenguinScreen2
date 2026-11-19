// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "Config.h"

#include <cmath>

namespace VR::ScreenAspect
{
	inline float Compute(AspectRatioType configured, u32 w, u32 h, float patch_aspect, bool follow_patch)
	{
		switch (configured)
		{
			case AspectRatioType::RAuto4_3_3_2:
#ifndef VR_SCREENASPECT_PLANTED_IGNORE_PATCH
				if (follow_patch && std::isfinite(patch_aspect) && patch_aspect > 0.0f)
					return patch_aspect;
#endif
				return 4.0f / 3.0f;
			case AspectRatioType::R4_3:
				return 4.0f / 3.0f;
			case AspectRatioType::R16_9:
				return 16.0f / 9.0f;
			default:
				return (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 4.0f / 3.0f;
		}
	}
}
