// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/NearField.h"
#include "VR/PadXRActions.h"
#include "VR/VRInputState.h"
#include "VR/WristPinch.h"

#include <openxr/openxr.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace VR::HandTracking
{
	inline constexpr const char* kExtensionName = XR_EXT_HAND_TRACKING_EXTENSION_NAME;

	WristPinch::HandJoints JointsFromLocations(bool is_active, const XrHandJointLocationEXT* joints, std::uint32_t count);

	inline constexpr const char* kHandInteractionProfile = PadXRActions::kHandsProfile;

	bool IsControllerProfile(std::string_view profile);

	enum class ProfileCheck
	{
		Failed,
		None,
		Named,
	};

	std::string NextProfileName(std::string_view previous, ProfileCheck check, std::string_view name);

	struct ProfileProbe
	{
		std::function<XrResult(int hand, XrPath* out)> get;
		std::function<XrResult(XrPath path, std::string* out)> to_string;
	};

	struct ProfileCheckResult
	{
		std::array<std::string, 2> names;
		std::array<ProfileCheck, 2> checks{ProfileCheck::Failed, ProfileCheck::Failed};
		std::array<XrResult, 2> failure{XR_SUCCESS, XR_SUCCESS};
		bool AnyFailed() const { return checks[0] == ProfileCheck::Failed || checks[1] == ProfileCheck::Failed; }
	};

	ProfileCheckResult CheckProfiles(const std::array<std::string, 2>& previous, const ProfileProbe& probe);

	class ProfileCheckSchedule
	{
	public:
		static constexpr std::uint32_t kBackstopUpdates = 90;
		static constexpr std::uint32_t kMaxQuickRetries = 8;
		bool Due(bool event, bool focus_gain);
		void Done(bool failed);
		void Reset();
		std::uint32_t QuickRetries() const { return m_retries; }

	private:
		std::uint32_t m_since = 0;
		std::uint32_t m_retries = 0;
		bool m_retry_owed = false;
	};

	std::array<bool, 2> ControllersInHands(const VRInputSnapshot& snap, const std::array<std::string, 2>& profiles);

	std::array<std::string, 2> CurrentProfiles();

	inline constexpr const char* kProfileUnchecked = "<unchecked>";
	std::array<std::string, 2> ProfilesAtSessionStart();

	bool ControllerInHand(const VRHandState& hand, std::string_view profile);

	struct RuntimeHand
	{
		float pinch_value = 0.f;
		bool pinch_ready = false;
		VRPose aim;
	};

	class PinchSource
	{
	public:
		bool Step(const std::array<WristPinch::HandJoints, 2>& hands, const VRPose& head,
			const std::array<bool, 2>& controller_in_hand, const WristPinch::Params& params = WristPinch::Params{});
		void Reset();
		bool Held() const { return m_held; }
		const WristPinch::Output& Last(int hand) const { return m_last[hand & 1]; }

	private:
		std::array<WristPinch::Detector, 2> m_detectors{};
		std::array<WristPinch::Output, 2> m_last{};
		bool m_held = false;
	};

	bool SystemForEvents(const PadXRActions::ActionState& state, bool pinch_held);

	void SetExtensionEnabled(XrInstance instance, bool enabled);
	bool IsExtensionEnabled();

	bool CreateTrackers(XrSession session);
	void DestroyTrackers();
	void ParkHandles();
	bool HasTrackers();

	bool Update(XrTime display_time);

	bool SystemHeld();

	NearField::JointSet JointSetFromLocations(bool is_active, const XrHandJointLocationEXT* joints, std::uint32_t count);
	const std::array<NearField::JointSet, 2>& JointsForDraw();
	void SetJointsForTest(const std::array<NearField::JointSet, 2>& sets);

	bool StepForTest(const std::array<WristPinch::HandJoints, 2>& hands, const VRPose& head,
		const std::array<bool, 2>& controller_in_hand);
	void ResetForTest();
}
