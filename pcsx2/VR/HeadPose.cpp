// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HeadPose.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace VR::HeadPose
{
	namespace
	{
		std::mutex s_mutex;
		Snapshot s_pose;

		u64 s_frame = 0;

		std::chrono::steady_clock::time_point s_time{};
		std::atomic<s64> s_test_clock_offset_ms{0};


		std::chrono::steady_clock::time_point Now()
		{
			return std::chrono::steady_clock::now() +
				   std::chrono::milliseconds(s_test_clock_offset_ms.load(std::memory_order_relaxed));
		}
	}

	void Publish(const Snapshot& pose)
	{
		std::lock_guard lock(s_mutex);
		s_pose = pose;
		s_pose.frame = ++s_frame;
		s_time = Now();
	}

	void Invalidate()
	{
		std::lock_guard lock(s_mutex);
		s_pose = Snapshot{};
		s_pose.frame = ++s_frame;
		s_time = Now();
	}

	Snapshot Get()
	{
		std::lock_guard lock(s_mutex);
#ifndef VR_FORKBUGS_PLANTED_B5
		if (s_pose.valid && Now() - s_time > std::chrono::milliseconds(kStalePoseMs))
		{
			Snapshot stale;
			stale.frame = s_pose.frame;
			return stale;
		}
#endif
		return s_pose;
	}

	void AdvanceClockForTest(s64 milliseconds)
	{
		s_test_clock_offset_ms.fetch_add(milliseconds, std::memory_order_relaxed);
	}


	void ResetForTest()
	{
		std::lock_guard lock(s_mutex);
		s_pose = Snapshot{};
		s_test_clock_offset_ms.store(0, std::memory_order_relaxed);
		s_time = Now();
	}
}
