// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/PadXRActions.h"
#include "VR/HandTracking.h"
#include "VR/VRInputState.h"

#include <openxr/openxr.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace VR
{
	PadXRActions::ActionState GetPadXRActionState();

	class VRInput final
	{
	public:
		VRInput();
		~VRInput();

		VRInput(const VRInput&) = delete;
		VRInput& operator=(const VRInput&) = delete;
		VRInput(VRInput&&) = delete;
		VRInput& operator=(VRInput&&) = delete;

		bool Initialize();

		void Shutdown();

		bool IsInitialized() const { return m_action_set != XR_NULL_HANDLE; }

		bool Update(XrTime display_time);

		void TriggerHaptic(int hand, float duration_seconds = 0.f, float frequency_hz = 0.f, float amplitude = 1.f);
		void StopHaptic(int hand);

		const std::string& ProfileName(int hand) const { return m_profile_names[hand & 1]; }
		const HandTracking::RuntimeHand& RuntimeHandAt(int hand) const { return m_runtime_hands[hand & 1]; }

	private:
		static constexpr int HAND_COUNT = 2;

		bool CreateActionSetAndActions(XrInstance instance);
		bool SuggestBindings(XrInstance instance);
		bool AttachAndCreateSpaces(XrInstance instance, XrSession session);

		bool CreateAction(XrActionSet set, XrActionType type, const char* name, const char* localized_name, XrAction* out);
		void CreatePadXRSet(XrInstance instance);
		void DestroyPadXRSet(bool instance_alive);
		XrAction CoreAction(const char* name) const;
		XrAction PadXRAction(const char* name) const;

		bool ReadHand(XrSession session, int hand, XrTime time, VRHandState* out);
		void ReadRuntimeHand(XrSession session, int hand, XrTime time, HandTracking::RuntimeHand* out);
		void ReadPadXR(XrSession session, PadXRActions::ActionState* out);
		void LogInteractionProfileIfChanged(XrInstance instance, XrSession session);

		bool m_attached = false;
		XrActionSet m_action_set = XR_NULL_HANDLE;
		std::array<XrPath, HAND_COUNT> m_hand_paths{};

		XrAction m_aim_pose = XR_NULL_HANDLE;
		XrAction m_grip_pose = XR_NULL_HANDLE;
		XrAction m_button_a = XR_NULL_HANDLE;
		XrAction m_button_b = XR_NULL_HANDLE;
		XrAction m_button_x = XR_NULL_HANDLE;
		XrAction m_button_y = XR_NULL_HANDLE;
		XrAction m_button_menu = XR_NULL_HANDLE;
		XrAction m_thumbstick_click = XR_NULL_HANDLE;
		XrAction m_trigger = XR_NULL_HANDLE;
		XrAction m_grip = XR_NULL_HANDLE;
		XrAction m_thumbstick = XR_NULL_HANDLE;
		XrAction m_haptic = XR_NULL_HANDLE;
		XrAction m_hand_aim_pose = XR_NULL_HANDLE;
		XrAction m_pinch_value = XR_NULL_HANDLE;
		XrAction m_pinch_ready = XR_NULL_HANDLE;

		XrActionSet m_padxr_set = XR_NULL_HANDLE;
		std::array<XrAction, PadXRActions::ActionCount> m_padxr_actions{};
		PadXRActions::ProfileLogLatch m_profile_latch;
		std::array<std::string, 2> m_profile_names;
		HandTracking::ProfileCheckSchedule m_profile_schedule;
		bool m_profile_fail_warned = false;
		int m_last_actions_active_for_profile = -1;

		std::array<XrSpace, HAND_COUNT> m_aim_spaces{};
		std::array<XrSpace, HAND_COUNT> m_grip_spaces{};
		std::array<XrSpace, HAND_COUNT> m_hand_aim_spaces{};
		std::array<HandTracking::RuntimeHand, HAND_COUNT> m_runtime_hands{};
		XrSpace m_view_space = XR_NULL_HANDLE;

		int m_last_actions_active = -1;
		bool m_locate_warned = false;
	};

	namespace XRInput
	{
		bool Initialize();

		void Update(XrTime display_time);

		void Shutdown();

		bool IsInitialized();

		std::string CurrentInteractionProfile(int hand);

		void SetProfilesForTest(const std::array<std::string, 2>* profiles);

		HandTracking::RuntimeHand RuntimeHandState(int hand);

		void QueueHaptic(int hand, float amplitude);

		void QueuePulse(int hand, float amplitude, float seconds, u8 locus = 0);

		bool TakePendingPulseForTest(int hand, float* amplitude, float* seconds, u8* locus = nullptr);

		void DrainPulsesForTest();

		void PublishSnapshotForTest(const VRInputSnapshot& snapshot);

		void ResetSnapshotForTest();

		void PublishNeutralIfActiveForTest();

		void PublishSnapshotForTest(const VRInputSnapshot& snapshot, const PadXRActions::ActionState& padxr);

		void SetFrameControllerExtensionEnabled(bool enabled);

		void SetHandInteractionExtensionEnabled(bool enabled);

		void NoteInteractionProfileChanged();

		std::uint32_t TakePadXREvents();

		void NoteUiOpen(bool open);

		void AdvanceClockForTest(s64 milliseconds);
	}
}
