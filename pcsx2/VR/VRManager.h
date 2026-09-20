// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <string>

class GSTexture;

namespace VR
{
	std::string GetRuntimeInfoReport();

	void UpdateSettings();

	void ApplySceneStereo();

	bool WantsVR();

	bool LaunchRequestedVR();

	enum class SessionStatus
	{
		DisabledInConfig,
		NotLaunchedForVR,
		Inactive,
		Active,
	};
	SessionStatus GetSessionStatus();

	bool IsSessionActive();

	void EnsureFrameSubmitted();

	void EndOfFrame(GSTexture* current);
}
