// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/GloveHaptics.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace VR::GloveHaptics
{
	namespace
	{
		std::atomic<SinkFn> s_sink_for_test{nullptr};
	}

	void SetSinkForTest(SinkFn sink)
	{
		s_sink_for_test.store(sink, std::memory_order_release);
	}

	std::array<u8, 8> Encode(int hand, u8 locus, float amplitude, float seconds)
	{
		if (!std::isfinite(amplitude))
			amplitude = 0.f;
		if (!std::isfinite(seconds))
			seconds = 0.f;
		const int intensity = static_cast<int>(std::lround(std::clamp(amplitude, 0.f, 1.f) * 100.f));
		const int ms = std::max(1, static_cast<int>(std::lround(std::clamp(seconds, 0.f, 2.f) * 1000.f)));
#ifdef VR_GLOVES_PLANTED_V1
		return {1, static_cast<u8>(hand == 1 ? 1 : 0), 0, static_cast<u8>(intensity),
			static_cast<u8>(ms & 0xFF), static_cast<u8>(ms >> 8), 0, 0};
#else
		return {2, static_cast<u8>(hand == 1 ? 1 : 0), locus == kLocusWholeHand ? kLocusHand : locus,
			static_cast<u8>(intensity), static_cast<u8>(ms & 0xFF), static_cast<u8>(ms >> 8), 0, 0};
#endif
	}

	bool Enabled()
	{
		return false;
	}

#ifdef __linux__
	namespace
	{
		int Socket()
		{
			static int fd = -1;
			static std::once_flag once;
			std::call_once(once, [] {
				fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
				if (fd < 0)
					Console.Warning("(VR) glove haptics: no UDP socket (errno %d); the gloves stay silent this run.", errno);
			});
			return fd;
		}

		bool SendTo(u16 port, const std::array<u8, 8>& msg)
		{
			const int fd = Socket();
			if (fd < 0)
				return false;
			sockaddr_in to = {};
			to.sin_family = AF_INET;
			to.sin_port = htons(port);
			to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			return ::sendto(fd, msg.data(), msg.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&to),
					   sizeof(to)) == static_cast<ssize_t>(msg.size());
		}
	}

	void Send(int hand, u8 locus, float amplitude, float seconds)
	{
		if (SinkFn sink = s_sink_for_test.load(std::memory_order_acquire))
			sink(hand, locus, amplitude, seconds);
		if (Enabled())
			SendTo(kPort, Encode(hand, locus, amplitude, seconds));
	}

	bool SendToForTest(u16 port, int hand, u8 locus, float amplitude, float seconds)
	{
		return SendTo(port, Encode(hand, locus, amplitude, seconds));
	}
#else
	void Send(int hand, u8 locus, float amplitude, float seconds)
	{
		if (SinkFn sink = s_sink_for_test.load(std::memory_order_acquire))
			sink(hand, locus, amplitude, seconds);
	}

	bool SendToForTest(u16, int, u8, float, float)
	{
		return false;
	}
#endif
}
