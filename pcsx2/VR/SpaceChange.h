// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <openxr/openxr.h>

namespace VR
{
	struct SpaceChangeFollow
	{
		bool armed = false;
		XrTime change_time = 0;

		void OnChangePending(XrTime when)
		{
			armed = true;
			change_time = when;
		}

		bool Due(XrTime display_time)
		{
			if (!armed)
				return false;
#ifndef VR_FORKBUGS_PLANTED_RECENTER_EARLY
			if (change_time != 0 && display_time < change_time)
				return false;
#endif
			armed = false;
			return true;
		}
	};
}
