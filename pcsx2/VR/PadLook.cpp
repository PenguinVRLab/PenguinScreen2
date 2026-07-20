// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#include "VR/PadLook.h"

#include "Host.h"
#include "VR/CameraDriver.h"
#include "VR/XRCompositor.h"
#include "VR/XRSession.h"

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace VR::PadLook
{
	namespace
	{

		std::atomic<int> s_rx_offset{0};
	}

	void Publish(float deflection)
	{
		if (!std::isfinite(deflection))
			deflection = 0.0f;
		deflection = std::clamp(deflection, -1.0f, 1.0f);

		s_rx_offset.store(-static_cast<int>(std::lround(deflection * 127.0f)),
			std::memory_order_relaxed);
	}

	u8 ApplyRx(u8 real)
	{
		const int offset = s_rx_offset.load(std::memory_order_relaxed);
		if (offset == 0)
			return real;
		return static_cast<u8>(std::clamp(static_cast<int>(real) + offset, 0, 255));
	}

	void UpdateRecenterChord(bool l1, bool r1, bool l3, bool r3)
	{

		static bool s_chord_was_held = false;
		const bool held = l1 && r1 && l3 && r3;
		if (held && !s_chord_was_held && XRSession::IsSessionRunning())
		{
			CameraDriver::RequestRecenter();
			XRCompositor::RequestScreenReanchor();
			Host::AddKeyedOSDMessage("VRRecenter", "VR recentered (head + screen).", 2.0f);
			Console.WriteLn("(VR) Recenter chord (L1+R1+L3+R3) fired: head camera + screen re-anchor.");
		}
		s_chord_was_held = held;
	}
}
