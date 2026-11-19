// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/XRSwapchainFormat.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace VR::SeatSession
{
#ifdef ENABLE_VR_SEATS
	void Start(int seat);

	void Stop();

	constexpr std::chrono::milliseconds kStopJoinTimeout{3000};

	bool JoinOrDetach(std::thread& t, const std::atomic<bool>& exited, std::chrono::milliseconds timeout);

	bool Running();

	std::string RuntimeLibFromManifest(const std::string& json_path);

	const char* ChooseVulkanBinding(const char* const* offered, size_t count);

	XRSwapchainFormat::Choice ChooseSwapchainFormat(const int64_t* offered, size_t count, bool prefer_unorm);

	enum class State : int
	{
		Idle = 0,
		Connecting,
		Waiting,
		Running,
		Failed,
	};
	State GetState();
	const char* StateName(State s);

	struct Observation
	{
		int32_t poll = 0;
		int32_t session_state = 0;
		bool instance_loss = false;
		int32_t get_system = 0;
		bool socket_seen = false;
		bool socket_now = true;
	};
	const char* RuntimeFailure(const Observation& o);
#else
	inline void Start(int) {}
	inline void Stop() {}
	inline bool Running() { return false; }
#endif
}
