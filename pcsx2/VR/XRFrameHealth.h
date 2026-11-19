// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <openxr/openxr.h>

namespace VR::XRFrameHealth
{
	constexpr u32 kMaxConsecutiveFrameFailures = 300;

	constexpr bool IsTerminal(XrResult res)
	{
		return res == XR_ERROR_SESSION_LOST || res == XR_ERROR_INSTANCE_LOST || res == XR_ERROR_HANDLE_INVALID;
	}

	struct Streak
	{
		u32 failures = 0;

		bool Record(XrResult res)
		{
			if (XR_SUCCEEDED(res) || res == XR_ERROR_SESSION_NOT_RUNNING)
			{
				failures = 0;
				return false;
			}
#ifdef VR_FORKBUGS_PLANTED_FRAMELOSS
			return false;
#else
			if (IsTerminal(res))
				return true;
			return ++failures >= kMaxConsecutiveFrameFailures;
#endif
		}
	};
}
