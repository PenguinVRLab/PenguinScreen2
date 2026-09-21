// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

#include <openxr/openxr.h>

#include <array>
#include <atomic>

namespace VR
{
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

	private:
		static constexpr int HAND_COUNT = 2;

		bool CreateActionSetAndActions(XrInstance instance);
		bool SuggestBindings(XrInstance instance);
		bool AttachAndCreateSpaces(XrInstance instance, XrSession session);

		bool CreateAction(XrInstance instance, XrActionType type, const char* name,
			const char* localized_name, XrAction* out);

		bool ReadHand(XrSession session, int hand, XrTime time, VRHandState* out);

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

		std::array<XrSpace, HAND_COUNT> m_aim_spaces{};
		std::array<XrSpace, HAND_COUNT> m_grip_spaces{};
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

		void QueueHaptic(int hand, float amplitude);

		void QueuePulse(int hand, float amplitude, float seconds);

		bool TakePendingPulseForTest(int hand, float* amplitude, float* seconds);

		void PublishSnapshotForTest(const VRInputSnapshot& snapshot);

		void ResetSnapshotForTest();
	}
}
