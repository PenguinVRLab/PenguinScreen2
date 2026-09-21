// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/StereoState.h"

#include <mutex>

namespace VR::StereoState
{
	namespace
	{
		std::mutex s_mutex;
		Params s_params;
	}

	void Publish(const Params& params)
	{
		std::lock_guard lock(s_mutex);
		s_params = params;
	}

	Params Get()
	{
		std::lock_guard lock(s_mutex);
		return s_params;
	}

	namespace
	{
		u32 s_current_eye = 0;
	}

	u32 GetCurrentEye()
	{
		return s_current_eye;
	}

	float GetCurrentEyeSign()
	{
		return (s_current_eye == 0) ? -1.0f : 1.0f;
	}

	void AdvanceEye()
	{
		s_current_eye ^= 1u;
	}
}
