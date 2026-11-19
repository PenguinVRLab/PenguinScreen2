// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/VRInputState.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace VR::PadXRActions
{
	inline constexpr const char* kActionSetName = "padxr";
	inline constexpr const char* kActionSetLocalizedName = "PadXR";
	inline constexpr std::uint32_t kActionSetPriority = 0;

	enum class ActionType : std::uint8_t
	{
		Boolean,
		Vector2,
	};

	enum ActionId : int
	{
		System = 0,
		MenuSelect,
		MenuBack,
		MenuNavigate,
		Talk,
		ActionCount
	};

	struct ActionDef
	{
		const char* name;
		const char* localized;
		const char* abstract_name;
		ActionType type;
		bool reserved;
	};

	const std::array<ActionDef, ActionCount>& Actions();
	int FindAction(std::string_view name);

	inline constexpr const char* kTouchProfile = "/interaction_profiles/oculus/touch_controller";
	inline constexpr const char* kSimpleProfile = "/interaction_profiles/khr/simple_controller";
	inline constexpr const char* kFrameProfile = "/interaction_profiles/valve/frame_controller_valve";
	inline constexpr const char* kFrameExtensionName = "XR_VALVE_frame_controller_interaction";
	inline constexpr const char* kHandsProfile = "/interaction_profiles/ext/hand_interaction_ext";
	inline constexpr const char* kHandInteractionExtensionName = "XR_EXT_hand_interaction";

	inline constexpr const char* kLeftHand = "/user/hand/left";
	inline constexpr const char* kRightHand = "/user/hand/right";

	std::vector<std::string> ProfilesToSuggest(bool frame_extension_enabled, bool hand_interaction_enabled = false);

	struct Binding
	{
		std::string action;
		std::string path;

		bool operator==(const Binding& o) const { return action == o.action && path == o.path; }
		bool operator!=(const Binding& o) const { return !(*this == o); }
	};

	std::vector<Binding> CoreBindings(std::string_view profile);

	std::vector<Binding> DefaultPadXRBindings(std::string_view profile);

	std::vector<Binding> MergeSuggestions(const std::vector<Binding>& core, const std::vector<Binding>& padxr);

	enum class SuggestOutcome : std::uint8_t
	{
		Merged,
		CoreOnly,
		Refused,
	};

	struct SuggestResult
	{
		SuggestOutcome outcome = SuggestOutcome::Refused;
		std::size_t count = 0;
		std::string log;
	};

	using SubmitFn = std::function<bool(const std::vector<Binding>&)>;

	SuggestResult SuggestWithRetry(std::string_view profile, const std::vector<Binding>& core,
		const std::vector<Binding>& padxr, const SubmitFn& submit);

	struct Component
	{
		const char* subpath;
		enum Kind : std::uint8_t
		{
			Bool,
			Float,
			Vec2,
			Pose,
			Haptic
		} kind;
	};
	const std::vector<Component>& ProfileComponents(std::string_view profile, int hand);

	bool PathAllowed(std::string_view profile, std::string_view path, Component::Kind action_kind);
	Component::Kind KindForAction(std::string_view action);

	struct HandActions
	{
		bool system = false;
		bool menu_select = false;
		bool menu_back = false;
		bool talk = false;
		float navigate_x = 0.f;
		float navigate_y = 0.f;

		bool operator==(const HandActions& o) const
		{
			return system == o.system && menu_select == o.menu_select && menu_back == o.menu_back && talk == o.talk &&
				   navigate_x == o.navigate_x && navigate_y == o.navigate_y;
		}
	};

	struct ActionState
	{
		std::array<HandActions, 2> hands{};
		bool active = false;
		std::uint64_t generation = 0;
		bool stale = false;

		bool SystemHeld() const { return hands[0].system || hands[1].system; }
	};

	ActionState ApplyStaleRule(const ActionState& published, std::int64_t age_ms);

	VRInputSnapshot CoreSnapshotToPublish(const VRInputSnapshot& core, const ActionState& padxr);

	inline constexpr std::int64_t kRecentreHoldMs = 800;

	enum Event : std::uint32_t
	{
		EventNone = 0,
		EventMenuOpen = 1u << 0,
		EventMenuClose = 1u << 1,
		EventRecentre = 1u << 2,
	};
	const char* EventName(std::uint32_t one_event);

	class EventMachine
	{
	public:
		std::uint32_t Step(bool system_held, bool input_live, bool ui_open, std::int64_t now_ms);
		void Reset();

	private:
		enum class Phase : std::uint8_t
		{
			WaitRelease,
			Idle,
			Pressed,
			LongFired,
		};
		Phase m_phase = Phase::WaitRelease;
		std::int64_t m_press_ms = 0;
	};

	class ProfileLogLatch
	{
	public:
		bool Changed(std::string_view left, std::string_view right, std::string* line);

	private:
		bool m_any = false;
		std::string m_left;
		std::string m_right;
	};
}
