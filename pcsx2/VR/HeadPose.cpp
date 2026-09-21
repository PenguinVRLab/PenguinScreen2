// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HeadPose.h"

#include <mutex>

namespace VR::HeadPose
{
	namespace
	{
		std::mutex s_mutex;
		Snapshot s_pose;

		u64 s_frame = 0;
	}

	void Publish(const Snapshot& pose)
	{
		std::lock_guard lock(s_mutex);
		s_pose = pose;
		s_pose.frame = ++s_frame;
	}

	void Invalidate()
	{
		std::lock_guard lock(s_mutex);
		s_pose = Snapshot{};
		s_pose.frame = ++s_frame;
	}

	Snapshot Get()
	{
		std::lock_guard lock(s_mutex);
		return s_pose;
	}
}
