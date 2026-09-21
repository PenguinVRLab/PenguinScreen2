// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::PadLook
{
	void Publish(float deflection);

	u8 ApplyRx(u8 real);

	enum class StickAxis
	{
		LX,
		RX,
		LY,
		RY,
	};

	enum class ProbeButton
	{
		CROSS,
		SQUARE,
	};

	u8 ProbeStick(StickAxis axis, u8 real);

	u32 ProbeButtons(u32 buttons);

	u8 ProbePressure(ProbeButton button, u8 real);

	void UpdateRecenterChord(bool l1, bool r1, bool l3, bool r3);
}
