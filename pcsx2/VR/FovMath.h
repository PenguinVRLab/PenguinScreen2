// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/VRProfileDB.h"

#include <string>

namespace VR::FovMath
{
	inline constexpr float kMinFovDeg = 1.0f;
	inline constexpr float kMaxFovDeg = 170.0f;

	bool Fit(ProfileDB::FovCalibration& cal, std::string* why);

	float DegFromConstant(const ProfileDB::FovCalibration& cal, float constant);

	float ConstantFromDeg(const ProfileDB::FovCalibration& cal, float deg);

	float WidenDeg(float game_deg, float factor);

	float LerpDeg(float from_deg, float to_deg, float t);
}
