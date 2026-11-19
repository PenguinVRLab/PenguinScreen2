// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0
#pragma once

namespace VR::DisplayFit
{
	inline constexpr float kTightVerticalFovDeg = 60.0f;
	inline constexpr float kDefaultFill = 0.85f;

	struct Screen
	{
		bool curved = false;
		float arc_deg = 0.0f;
		float height_m = 1.4f;
		float distance_m = 2.0f;
		float aspect = 4.0f / 3.0f;
	};

	struct Fit
	{
		bool tight = false;
		float yaw_scale = 1.0f;
		float pitch_scale = 1.0f;
		bool IsIdentity() const { return yaw_scale == 1.0f && pitch_scale == 1.0f; }
	};

	float VerticalDeg(const Screen& s);
	float HorizontalDeg(const Screen& s);

	Fit Compute(float display_vfov_deg, const Screen& reference, float fill);
	Screen Apply(const Screen& reference, const Fit& fit);

	void HeadToReference(float& yaw_rad, float& pitch_rad, const Fit& fit);

	void HeadForOp(float& yaw_rad, float& pitch_rad, const Fit& fit, bool fitted);

	inline constexpr float kMaxRecenterPitchDeg = 60.0f;
	float RecenterPitch(float forward_y, const Fit& fit);
	float ForwardY(float qx, float qy, float qz, float qw);

	struct Placement
	{
		float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
		float dx = 0.0f, dy = 0.0f, dz = 0.0f;
	};
	Placement PlaceAhead(float yaw_rad, float pitch_rad, float distance_m);

	void Publish(const Fit& fit);
	Fit Get();
	void Reset();
}
