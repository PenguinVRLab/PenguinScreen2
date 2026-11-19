// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/PadXRActions.h"

#include <algorithm>

namespace VR::PadXRActions
{
	namespace
	{
		using C = Component;

		// clang-format off
		const std::array<ActionDef, ActionCount> s_actions = {{
			{"system",        "PadXR menu (hold: recentre)", "system",        ActionType::Boolean, true},
			{"menu_select",   "Menu select",                 "menu.select",   ActionType::Boolean, false},
			{"menu_back",     "Menu back",                   "menu.back",     ActionType::Boolean, false},
			{"menu_navigate", "Menu navigate",               "menu.navigate", ActionType::Vector2, false},
			{"talk",          "Push to talk",                "talk",          ActionType::Boolean, false},
		}};
		// clang-format on

		std::string L(const char* sub) { return std::string(kLeftHand) + sub; }
		std::string R(const char* sub) { return std::string(kRightHand) + sub; }

		// clang-format off
		const std::vector<Component> s_touch_shared = {
			{"/input/squeeze/value", C::Float}, {"/input/trigger/value", C::Float}, {"/input/trigger/touch", C::Bool},
			{"/input/thumbstick", C::Vec2}, {"/input/thumbstick/x", C::Float}, {"/input/thumbstick/y", C::Float},
			{"/input/thumbstick/click", C::Bool}, {"/input/thumbstick/touch", C::Bool}, {"/input/thumbrest/touch", C::Bool},
			{"/input/grip/pose", C::Pose}, {"/input/aim/pose", C::Pose}, {"/input/grip_surface/pose", C::Pose},
			{"/output/haptic", C::Haptic},
		};
		const std::vector<Component> s_touch_left_only = {
			{"/input/x/click", C::Bool}, {"/input/x/touch", C::Bool}, {"/input/y/click", C::Bool},
			{"/input/y/touch", C::Bool}, {"/input/menu/click", C::Bool},
		};
		const std::vector<Component> s_touch_right_only = {
			{"/input/a/click", C::Bool}, {"/input/a/touch", C::Bool}, {"/input/b/click", C::Bool},
			{"/input/b/touch", C::Bool},
		};
		const std::vector<Component> s_simple = {
			{"/input/select/click", C::Bool}, {"/input/menu/click", C::Bool}, {"/input/grip/pose", C::Pose},
			{"/input/aim/pose", C::Pose}, {"/input/grip_surface/pose", C::Pose}, {"/output/haptic", C::Haptic},
		};
		const std::vector<Component> s_frame_shared = {
			{"/input/squeeze/touch", C::Bool}, {"/input/squeeze/click", C::Bool}, {"/input/squeeze/value", C::Float},
			{"/input/trigger/touch", C::Bool}, {"/input/trigger/click", C::Bool}, {"/input/trigger/value", C::Float},
			{"/input/thumbstick/touch", C::Bool}, {"/input/thumbstick/click", C::Bool},
			{"/input/thumbstick/x", C::Float}, {"/input/thumbstick/y", C::Float}, {"/input/thumbstick", C::Vec2},
			{"/input/grip/pose", C::Pose}, {"/input/aim/pose", C::Pose}, {"/output/haptic", C::Haptic},
		};
		const std::vector<Component> s_frame_left_only = {
			{"/input/dpad_up/touch", C::Bool}, {"/input/dpad_up/click", C::Bool},
			{"/input/dpad_left/touch", C::Bool}, {"/input/dpad_left/click", C::Bool},
			{"/input/dpad_down/touch", C::Bool}, {"/input/dpad_down/click", C::Bool},
			{"/input/dpad_right/touch", C::Bool}, {"/input/dpad_right/click", C::Bool},
			{"/input/view/touch", C::Bool}, {"/input/view/click", C::Bool},
		};
		const std::vector<Component> s_frame_right_only = {
			{"/input/a/touch", C::Bool}, {"/input/a/click", C::Bool}, {"/input/b/touch", C::Bool},
			{"/input/b/click", C::Bool}, {"/input/x/touch", C::Bool}, {"/input/x/click", C::Bool},
			{"/input/y/touch", C::Bool}, {"/input/y/click", C::Bool}, {"/input/menu/touch", C::Bool},
			{"/input/menu/click", C::Bool},
		};
		const std::vector<Component> s_hands = {
			{"/input/pinch_ext/pose", C::Pose}, {"/input/poke_ext/pose", C::Pose}, {"/input/grip/pose", C::Pose},
			{"/input/aim/pose", C::Pose}, {"/input/pinch_ext/value", C::Float}, {"/input/pinch_ext/ready_ext", C::Bool},
			{"/input/grasp_ext/value", C::Float}, {"/input/grasp_ext/ready_ext", C::Bool},
			{"/input/aim_activate_ext/value", C::Float}, {"/input/aim_activate_ext/ready_ext", C::Bool},
		};
#ifdef VR_PADXR_PLANTED_HANDS_BAD_PATH
		const char* const kPinchReadyPath = "/input/pinch_ext/ready";
#else
		const char* const kPinchReadyPath = "/input/pinch_ext/ready_ext";
#endif
		// clang-format on

		std::vector<Component> Join(const std::vector<Component>& a, const std::vector<Component>& b)
		{
			std::vector<Component> out = a;
			out.insert(out.end(), b.begin(), b.end());
			return out;
		}

		const std::vector<Component> s_touch_left = Join(s_touch_left_only, s_touch_shared);
		const std::vector<Component> s_touch_right = Join(s_touch_right_only, s_touch_shared);
		const std::vector<Component> s_frame_left = Join(s_frame_left_only, s_frame_shared);
		const std::vector<Component> s_frame_right = Join(s_frame_right_only, s_frame_shared);
		const std::vector<Component> s_none;
	}

	const std::array<ActionDef, ActionCount>& Actions()
	{
		return s_actions;
	}

	int FindAction(std::string_view name)
	{
		for (int i = 0; i < ActionCount; i++)
		{
			if (name == s_actions[i].name)
				return i;
		}
		return ActionCount;
	}

	std::vector<std::string> ProfilesToSuggest(bool frame_extension_enabled, bool hand_interaction_enabled)
	{
		std::vector<std::string> out = {kTouchProfile, kSimpleProfile};
		if (frame_extension_enabled)
			out.emplace_back(kFrameProfile);
#ifdef VR_PADXR_PLANTED_HANDS_WITHOUT_EXTENSION
		(void)hand_interaction_enabled;
		out.emplace_back(kHandsProfile);
#else
		if (hand_interaction_enabled)
			out.emplace_back(kHandsProfile);
#endif
		return out;
	}

	std::vector<Binding> CoreBindings(std::string_view profile)
	{
		if (profile == kTouchProfile)
		{
			return {
				{"aim_pose", L("/input/aim/pose")},
				{"aim_pose", R("/input/aim/pose")},
				{"grip_pose", L("/input/grip/pose")},
				{"grip_pose", R("/input/grip/pose")},
				{"button_a", R("/input/a/click")},
				{"button_b", R("/input/b/click")},
				{"button_x", L("/input/x/click")},
				{"button_y", L("/input/y/click")},
				{"button_menu", L("/input/menu/click")},
				{"thumbstick_click", L("/input/thumbstick/click")},
				{"thumbstick_click", R("/input/thumbstick/click")},
				{"trigger", L("/input/trigger/value")},
				{"trigger", R("/input/trigger/value")},
				{"grip", L("/input/squeeze/value")},
				{"grip", R("/input/squeeze/value")},
				{"thumbstick", L("/input/thumbstick")},
				{"thumbstick", R("/input/thumbstick")},
				{"haptic", L("/output/haptic")},
				{"haptic", R("/output/haptic")},
			};
		}
		if (profile == kSimpleProfile)
		{
			return {
				{"aim_pose", L("/input/aim/pose")},
				{"aim_pose", R("/input/aim/pose")},
				{"grip_pose", L("/input/grip/pose")},
				{"grip_pose", R("/input/grip/pose")},
				{"trigger", L("/input/select/click")},
				{"trigger", R("/input/select/click")},
				{"button_a", L("/input/select/click")},
				{"button_a", R("/input/select/click")},
				{"button_menu", L("/input/menu/click")},
				{"button_menu", R("/input/menu/click")},
				{"haptic", L("/output/haptic")},
				{"haptic", R("/output/haptic")},
			};
		}
		if (profile == kFrameProfile)
		{
			return {
				{"aim_pose", L("/input/aim/pose")},
				{"aim_pose", R("/input/aim/pose")},
				{"grip_pose", L("/input/grip/pose")},
				{"grip_pose", R("/input/grip/pose")},
				{"button_a", R("/input/a/click")},
				{"button_b", R("/input/b/click")},
				{"button_x", R("/input/x/click")},
				{"button_y", R("/input/y/click")},
				{"button_menu", L("/input/view/click")},
				{"thumbstick_click", L("/input/thumbstick/click")},
				{"thumbstick_click", R("/input/thumbstick/click")},
				{"trigger", L("/input/trigger/value")},
				{"trigger", R("/input/trigger/value")},
				{"grip", L("/input/squeeze/value")},
				{"grip", R("/input/squeeze/value")},
				{"thumbstick", L("/input/thumbstick")},
				{"thumbstick", R("/input/thumbstick")},
				{"haptic", L("/output/haptic")},
				{"haptic", R("/output/haptic")},
			};
		}
		if (profile == kHandsProfile)
		{
#ifdef VR_PADXR_PLANTED_HANDS_BIND_CORE_POSES
			return {{"aim_pose", L("/input/aim/pose")}, {"aim_pose", R("/input/aim/pose")},
				{"grip_pose", L("/input/grip/pose")}, {"grip_pose", R("/input/grip/pose")}};
#else
			return {
				{"hand_aim_pose", L("/input/aim/pose")},
				{"hand_aim_pose", R("/input/aim/pose")},
				{"pinch_value", L("/input/pinch_ext/value")},
				{"pinch_value", R("/input/pinch_ext/value")},
				{"pinch_ready", L(kPinchReadyPath)},
				{"pinch_ready", R(kPinchReadyPath)},
			};
#endif
		}
		return {};
	}

	std::vector<Binding> DefaultPadXRBindings(std::string_view profile)
	{
		if (profile == kTouchProfile)
		{
#ifdef VR_PADXR_PLANTED_BAD_PATH
			const char* touch_system = "/input/menu/clik";
#else
			const char* touch_system = "/input/menu/click";
#endif
			return {
				{"system", L(touch_system)},
				{"menu_select", L("/input/trigger/value")},
				{"menu_select", R("/input/trigger/value")},
				{"menu_select", R("/input/a/click")},
				{"menu_back", R("/input/b/click")},
				{"menu_navigate", L("/input/thumbstick")},
				{"menu_navigate", R("/input/thumbstick")},
			};
		}
		if (profile == kSimpleProfile)
		{
			return {
				{"system", L("/input/menu/click")},
				{"system", R("/input/menu/click")},
				{"menu_select", L("/input/select/click")},
				{"menu_select", R("/input/select/click")},
			};
		}
		if (profile == kFrameProfile)
		{
			return {
				{"system", L("/input/view/click")},
				{"menu_select", L("/input/trigger/value")},
				{"menu_select", R("/input/trigger/value")},
				{"menu_select", R("/input/a/click")},
				{"menu_back", R("/input/b/click")},
				{"menu_navigate", L("/input/thumbstick")},
				{"menu_navigate", R("/input/thumbstick")},
			};
		}
		return {};
	}

	std::vector<Binding> MergeSuggestions(const std::vector<Binding>& core, const std::vector<Binding>& padxr)
	{
		std::vector<Binding> out;
		out.reserve(core.size() + padxr.size());
		out.insert(out.end(), core.begin(), core.end());
#ifdef VR_PADXR_PLANTED_DROP_CORE
		if (!out.empty())
			out.pop_back();
#endif
		out.insert(out.end(), padxr.begin(), padxr.end());
		return out;
	}

	SuggestResult SuggestWithRetry(std::string_view profile, const std::vector<Binding>& core,
		const std::vector<Binding>& padxr, const SubmitFn& submit)
	{
		SuggestResult result;
		const std::vector<Binding> merged = MergeSuggestions(core, padxr);
		if (submit(merged))
		{
			result.outcome = SuggestOutcome::Merged;
			result.count = merged.size();
			return result;
		}
		if (padxr.empty() || core.empty())
			return result;

		result.log = "PadXR: bindings refused for " + std::string(profile) + "; core bindings kept";
#ifdef VR_PADXR_PLANTED_NO_RETRY
		return result;
#else
		if (submit(core))
		{
			result.outcome = SuggestOutcome::CoreOnly;
			result.count = core.size();
		}
		else
		{
			result.log = "PadXR: bindings refused for " + std::string(profile) + "; the core's own were refused too";
		}
		return result;
#endif
	}

	const std::vector<Component>& ProfileComponents(std::string_view profile, int hand)
	{
		if (hand != 0 && hand != 1)
			return s_none;
		if (profile == kTouchProfile)
			return hand == 0 ? s_touch_left : s_touch_right;
		if (profile == kSimpleProfile)
			return s_simple;
		if (profile == kFrameProfile)
			return hand == 0 ? s_frame_left : s_frame_right;
		if (profile == kHandsProfile)
			return s_hands;
		return s_none;
	}

	bool PathAllowed(std::string_view profile, std::string_view path, Component::Kind action_kind)
	{
		int hand = -1;
		std::string_view sub;
		const std::string_view left = kLeftHand, right = kRightHand;
		if (path.substr(0, left.size()) == left)
		{
			hand = 0;
			sub = path.substr(left.size());
		}
		else if (path.substr(0, right.size()) == right)
		{
			hand = 1;
			sub = path.substr(right.size());
		}
		else
		{
			return false;
		}
		for (const Component& c : ProfileComponents(profile, hand))
		{
			if (sub != c.subpath)
				continue;
			switch (action_kind)
			{
				case C::Bool:
				case C::Float:
					return c.kind == C::Bool || c.kind == C::Float;
				default:
					return c.kind == action_kind;
			}
		}
		return false;
	}

	Component::Kind KindForAction(std::string_view action)
	{
		if (action == "menu_navigate" || action == "thumbstick")
			return C::Vec2;
		if (action == "aim_pose" || action == "grip_pose" || action == "hand_aim_pose")
			return C::Pose;
		if (action == "haptic")
			return C::Haptic;
		if (action == "trigger" || action == "grip" || action == "pinch_value")
			return C::Float;
		return C::Bool;
	}

	ActionState ApplyStaleRule(const ActionState& published, std::int64_t age_ms)
	{
#ifndef VR_PADXR_PLANTED_NO_STALE
		if (published.generation != 0 && age_ms > kStaleSnapshotMs)
		{
			ActionState neutral;
			neutral.generation = published.generation;
			neutral.stale = true;
			return neutral;
		}
#endif
		return published;
	}

	VRInputSnapshot CoreSnapshotToPublish(const VRInputSnapshot& core, const ActionState& padxr)
	{
		VRInputSnapshot out = core;
#ifdef VR_PADXR_PLANTED_SYSTEM_TO_PAD
		if (padxr.SystemHeld())
		{
			out.hands[VRInputSnapshot::LEFT].thumbstick_click = true;
			out.hands[VRInputSnapshot::RIGHT].thumbstick_click = true;
		}
#else
		(void)padxr;
#endif
		return out;
	}

	const char* EventName(std::uint32_t one_event)
	{
		switch (one_event)
		{
			case EventMenuOpen:
				return "menu.open";
			case EventMenuClose:
				return "menu.close";
			case EventRecentre:
				return "recentre";
			default:
				return "none";
		}
	}

	void EventMachine::Reset()
	{
		m_phase = Phase::WaitRelease;
		m_press_ms = 0;
	}

	std::uint32_t EventMachine::Step(bool system_held, bool input_live, bool ui_open, std::int64_t now_ms)
	{
#ifdef VR_PADXR_PLANTED_ZERO_HOLD
		constexpr std::int64_t hold_ms = 0;
#else
		constexpr std::int64_t hold_ms = kRecentreHoldMs;
#endif
		if (!input_live)
		{
			m_phase = Phase::WaitRelease;
			return EventNone;
		}

		const auto elapsed = [&]() { return now_ms > m_press_ms ? now_ms - m_press_ms : 0; };

		switch (m_phase)
		{
			case Phase::WaitRelease:
				if (!system_held)
					m_phase = Phase::Idle;
				return EventNone;

			case Phase::Idle:
				if (!system_held)
					return EventNone;
				m_phase = Phase::Pressed;
				m_press_ms = now_ms;
				if (hold_ms <= 0)
				{
					m_phase = Phase::LongFired;
					return EventRecentre;
				}
				return EventNone;

			case Phase::Pressed:
				if (!system_held)
				{
					m_phase = Phase::Idle;
					if (elapsed() >= hold_ms)
						return EventRecentre;
					return ui_open ? EventMenuClose : EventMenuOpen;
				}
				if (elapsed() >= hold_ms)
				{
					m_phase = Phase::LongFired;
					return EventRecentre;
				}
				return EventNone;

			case Phase::LongFired:
				if (!system_held)
					m_phase = Phase::Idle;
				return EventNone;
		}
		return EventNone;
	}

	bool ProfileLogLatch::Changed(std::string_view left, std::string_view right, std::string* line)
	{
		if (m_any && left == m_left && right == m_right)
			return false;
		m_any = true;
		m_left = std::string(left);
		m_right = std::string(right);
		if (line)
		{
			*line = "(VR) Interaction profile: left=" + (m_left.empty() ? std::string("none") : m_left) +
					" right=" + (m_right.empty() ? std::string("none") : m_right);
		}
		return true;
	}
}
