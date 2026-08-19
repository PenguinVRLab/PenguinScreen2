// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

namespace VR::PadLook
{

	void Publish(float deflection);

	u8 ApplyRx(u8 real);

	void UpdateRecenterChord(bool l1, bool r1, bool l3, bool r3);
}
