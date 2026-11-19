// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

#include <optional>

namespace VR::FovDriver
{
	void Apply();

	void OnStateLoaded();

	float EffectiveFovDeg();

	void SetOverrideDeg(std::optional<float> deg);
	std::optional<float> OverrideDeg();

	std::optional<float> HeldConstant();
}
