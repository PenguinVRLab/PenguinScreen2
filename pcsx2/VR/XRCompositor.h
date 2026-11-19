// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"
#include "VR/ScreenCurveMode.h"
#include "VR/HeadPose.h"

#include <optional>

class GSTexture;

namespace VR::XRCompositor
{
	bool Initialize();

	void Shutdown();

	inline constexpr u32 MonoEye = 2;

	void EndOfFrame(GSTexture* current, u32 eye);

	void UpdateScreenParams(float distance_m, float height_m, float arc_deg, float vertical_offset_m,
		bool follow_head = false, float fit_fill = 0.85f);

	void UpdateScreenCurve(ScreenCurve::Fallback fallback, ScreenCurve::Force force, float projector_supersample,
		bool projector_depth);

	void UpdateScreenAspectRule(bool follow_patch_aspect);

	void UpdateScreenLag(s64 lag_ns, u8 axes);

	void NoteGameVsync(bool new_frame);

	void RequestScreenReanchor();

	inline constexpr u32 kUiPanelWidth = 1280;
	inline constexpr u32 kUiPanelHeight = 720;
	bool UiPanelWantsPixels();
	void SetUiPanelSource(GSTexture* panel);
	void RequestUiRecentre();

	bool IsReanchorRequested();
	void ClearReanchorRequestForTest();

	void ReanchorHead(HeadPose::Snapshot* head, bool* position_valid);

	struct ScreenAnchor
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float yaw = 0.0f;
		u32 generation = 0;
	};
	ScreenAnchor GetScreenAnchor();

	std::optional<float> PresentedScreenHfovDeg();

	bool RuntimeLossDetected();

	u64 GetPacedFramesConsumed();
}
