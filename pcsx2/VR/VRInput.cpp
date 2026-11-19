// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VRInput.h"
#include "VR/GloveHaptics.h"
#include "VR/HandTracking.h"
#include "VR/PadXRActions.h"
#include "VR/PadXRBindings.h"
#include "VR/XRSession.h"

#include "common/Console.h"
#include "Host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace VR
{
	namespace
	{
		constexpr const char* HAND_PATH[2] = {"/user/hand/left", "/user/hand/right"};

		namespace PA = PadXRActions;

		std::mutex s_snapshot_mutex;
		VRInputSnapshot s_snapshot;
		std::chrono::steady_clock::time_point s_snapshot_time{};
		PA::ActionState s_padxr_state;
		std::atomic<s64> s_test_clock_offset_ms{0};

		std::chrono::steady_clock::time_point SnapshotNow()
		{
			return std::chrono::steady_clock::now() +
				   std::chrono::milliseconds(s_test_clock_offset_ms.load(std::memory_order_relaxed));
		}

		void PublishSnapshot(VRInputSnapshot snapshot, PA::ActionState padxr = PA::ActionState{})
		{
			std::lock_guard lock{s_snapshot_mutex};
			snapshot.generation = s_snapshot.generation + 1;
			snapshot.stale = false;
			s_snapshot = snapshot;
			padxr.generation = snapshot.generation;
			padxr.stale = false;
			s_padxr_state = padxr;
			s_snapshot_time = SnapshotNow();
		}

		void PublishNeutralIfActive()
		{
#ifdef VR_FORKBUGS_PLANTED_B1
			return;
#endif
			{
				std::lock_guard lock{s_snapshot_mutex};
				if (s_snapshot.generation == 0 || !s_snapshot.actions_active)
					return;
			}
			PublishSnapshot(VRInputSnapshot{});
		}

		void ResetSnapshot()
		{
			std::lock_guard lock{s_snapshot_mutex};
			s_snapshot = VRInputSnapshot{};
			s_padxr_state = PA::ActionState{};
		}

		VRInput* s_publisher = nullptr;

		std::atomic<int> s_haptic_request[2] = {-1, -1};
		int s_haptic_applied[2] = {0, 0};

		std::atomic<long long> s_pulse_request[2] = {-1, -1};

		bool s_frame_extension_enabled = false;
		bool s_hand_interaction_enabled = false;
		std::atomic<bool> s_profile_changed{true};
		std::atomic<std::uint32_t> s_padxr_events{0};
		std::atomic<bool> s_ui_open{false};
		std::atomic<bool> s_ui_open_reported{false};
		static_assert(std::atomic<long long>::is_always_lock_free, "the pulse queue must stay lock-free");

		constexpr int PulseAmp(long long p) { return static_cast<int>(p & 0xFFFF); }
		constexpr int PulseMs(long long p) { return static_cast<int>((p >> 16) & 0xFFFF); }
		constexpr u8 PulseLocus(long long p) { return static_cast<u8>((p >> 32) & 0xFF); }

		long long PackPulse(float amplitude, float seconds, u8 locus)
		{
			if (!std::isfinite(amplitude))
				amplitude = 0.f;
			if (!std::isfinite(seconds))
				seconds = 0.f;
			const int permille = static_cast<int>(std::lround(std::clamp(amplitude, 0.f, 1.f) * 1000.f));
			const int ms = static_cast<int>(std::lround(std::clamp(seconds, 0.f, 10.f) * 1000.f));
#ifdef VR_GLOVES_PLANTED_LOCUS_DROP
			locus = 0;
#endif
			return static_cast<long long>(permille) | (static_cast<long long>(ms) << 16) |
				   (static_cast<long long>(locus) << 32);
		}

		bool CheckXR(XrResult res, const char* what)
		{
			if (XR_SUCCEEDED(res))
				return true;

			char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
			if (XRSession::HasInstance())
				xrResultToString(XRSession::GetInstance(), res, buf);
			Console.Error("(VR) %s failed: %s (%d)", what, buf, static_cast<int>(res));
			return false;
		}

		void CopyPose(const XrPosef& in, VRPose* out)
		{
			out->orientation_xyzw = {in.orientation.x, in.orientation.y, in.orientation.z, in.orientation.w};
			out->position_xyz = {in.position.x, in.position.y, in.position.z};
		}

		bool ReadBool(XrSession session, XrAction action, XrPath subaction_path)
		{
			if (action == XR_NULL_HANDLE)
				return false;

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
			if (XR_FAILED(xrGetActionStateBoolean(session, &get_info, &state)))
				return false;

			return state.isActive == XR_TRUE && state.currentState == XR_TRUE;
		}

		float ReadFloat(XrSession session, XrAction action, XrPath subaction_path)
		{
			if (action == XR_NULL_HANDLE)
				return 0.f;

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
			if (XR_FAILED(xrGetActionStateFloat(session, &get_info, &state)) || state.isActive != XR_TRUE)
				return 0.f;

			return std::isfinite(state.currentState) ? state.currentState : 0.f;
		}

		XrVector2f ReadVector2(XrSession session, XrAction action, XrPath subaction_path)
		{
			if (action == XR_NULL_HANDLE)
				return {0.f, 0.f};

			XrActionStateGetInfo get_info = {XR_TYPE_ACTION_STATE_GET_INFO};
			get_info.action = action;
			get_info.subactionPath = subaction_path;

			XrActionStateVector2f state = {XR_TYPE_ACTION_STATE_VECTOR2F};
			if (XR_FAILED(xrGetActionStateVector2f(session, &get_info, &state)) || state.isActive != XR_TRUE)
				return {0.f, 0.f};

			if (!std::isfinite(state.currentState.x) || !std::isfinite(state.currentState.y))
				return {0.f, 0.f};

			return state.currentState;
		}
	}

	VRInputSnapshot GetInputSnapshot()
	{
		std::lock_guard lock{s_snapshot_mutex};
#ifndef VR_FORKBUGS_PLANTED_B1
		if (s_snapshot.generation != 0 && SnapshotNow() - s_snapshot_time > std::chrono::milliseconds(kStaleSnapshotMs))
		{
			VRInputSnapshot neutral;
			neutral.generation = s_snapshot.generation;
			neutral.stale = true;
			return neutral;
		}
#endif
		return s_snapshot;
	}

	PadXRActions::ActionState GetPadXRActionState()
	{
		std::lock_guard lock{s_snapshot_mutex};
		const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(SnapshotNow() - s_snapshot_time).count();
		return PA::ApplyStaleRule(s_padxr_state, static_cast<std::int64_t>(age));
	}

	VRScreenTransform GetScreenTransform()
	{
		return VRScreenTransform{};
	}

	VRInput::VRInput() = default;

	VRInput::~VRInput()
	{
		Shutdown();
	}

	bool VRInput::CreateAction(XrActionSet set, XrActionType type, const char* name, const char* localized_name,
		XrAction* out)
	{
		XrActionCreateInfo create_info = {XR_TYPE_ACTION_CREATE_INFO};
		create_info.actionType = type;
		std::strncpy(create_info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(create_info.localizedActionName, localized_name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		create_info.countSubactionPaths = static_cast<uint32_t>(m_hand_paths.size());
		create_info.subactionPaths = m_hand_paths.data();

		const XrResult res = xrCreateAction(set, &create_info, out);
		if (XR_FAILED(res))
		{
			char what[64];
			std::snprintf(what, sizeof(what), "xrCreateAction(%s)", name);
			return CheckXR(res, what);
		}
		return true;
	}

	bool VRInput::CreateActionSetAndActions(XrInstance instance)
	{
		XrActionSetCreateInfo set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strncpy(set_info.actionSetName, "pcsx2_vr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
		std::strncpy(set_info.localizedActionSetName, "PCSX2 VR", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		set_info.priority = 0;
		if (!CheckXR(xrCreateActionSet(instance, &set_info, &m_action_set), "xrCreateActionSet"))
		{
			m_action_set = XR_NULL_HANDLE;
			return false;
		}

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			if (!CheckXR(xrStringToPath(instance, HAND_PATH[hand], &m_hand_paths[hand]), "xrStringToPath(hand)"))
				return false;
		}

		// clang-format off
		return CreateAction(m_action_set, XR_ACTION_TYPE_POSE_INPUT,       "aim_pose",         "Aim Pose",         &m_aim_pose) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_POSE_INPUT,       "grip_pose",        "Grip Pose",        &m_grip_pose) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_a",         "A Button",         &m_button_a) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_b",         "B Button",         &m_button_b) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_x",         "X Button",         &m_button_x) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_y",         "Y Button",         &m_button_y) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_menu",      "Menu Button",      &m_button_menu) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,    "thumbstick_click", "Thumbstick Click", &m_thumbstick_click) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT,      "trigger",          "Trigger",          &m_trigger) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT,      "grip",             "Grip",             &m_grip) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_VECTOR2F_INPUT,   "thumbstick",       "Thumbstick",       &m_thumbstick) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic",           "Haptic Feedback",  &m_haptic) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_POSE_INPUT,       "hand_aim_pose",    "Hand Aim Pose",    &m_hand_aim_pose) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_FLOAT_INPUT,      "pinch_value",      "Pinch",            &m_pinch_value) &&
		       CreateAction(m_action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,    "pinch_ready",      "Pinch Ready",      &m_pinch_ready);
		// clang-format on
	}

	void VRInput::CreatePadXRSet(XrInstance instance)
	{
		XrActionSetCreateInfo set_info = {XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strncpy(set_info.actionSetName, PA::kActionSetName, XR_MAX_ACTION_SET_NAME_SIZE - 1);
		std::strncpy(set_info.localizedActionSetName, PA::kActionSetLocalizedName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		set_info.priority = PA::kActionSetPriority;
		if (!CheckXR(xrCreateActionSet(instance, &set_info, &m_padxr_set), "xrCreateActionSet(padxr)"))
		{
			m_padxr_set = XR_NULL_HANDLE;
			Console.Warning("(VR) PadXR: no padxr action set; the controllers run on the core set alone.");
			return;
		}
		for (int i = 0; i < PA::ActionCount; i++)
		{
			const PA::ActionDef& def = PA::Actions()[i];
			const XrActionType type =
				def.type == PA::ActionType::Vector2 ? XR_ACTION_TYPE_VECTOR2F_INPUT : XR_ACTION_TYPE_BOOLEAN_INPUT;
			if (!CreateAction(m_padxr_set, type, def.name, def.localized, &m_padxr_actions[i]))
			{
				Console.Warning("(VR) PadXR: action %s could not be created; the padxr set is dropped.", def.name);
				DestroyPadXRSet(true);
				return;
			}
		}
	}

	void VRInput::DestroyPadXRSet(bool instance_alive)
	{
		for (XrAction& action : m_padxr_actions)
		{
			if (action != XR_NULL_HANDLE && instance_alive)
				xrDestroyAction(action);
			action = XR_NULL_HANDLE;
		}
		if (m_padxr_set != XR_NULL_HANDLE)
		{
			if (instance_alive)
				xrDestroyActionSet(m_padxr_set);
			m_padxr_set = XR_NULL_HANDLE;
		}
	}

	XrAction VRInput::CoreAction(const char* name) const
	{
		// clang-format off
		const struct { const char* name; XrAction action; } table[] = {
			{"aim_pose", m_aim_pose}, {"grip_pose", m_grip_pose}, {"button_a", m_button_a}, {"button_b", m_button_b},
			{"button_x", m_button_x}, {"button_y", m_button_y}, {"button_menu", m_button_menu},
			{"thumbstick_click", m_thumbstick_click}, {"trigger", m_trigger}, {"grip", m_grip},
			{"thumbstick", m_thumbstick}, {"haptic", m_haptic},
			{"hand_aim_pose", m_hand_aim_pose}, {"pinch_value", m_pinch_value}, {"pinch_ready", m_pinch_ready},
		};
		// clang-format on
		for (const auto& e : table)
		{
			if (std::strcmp(e.name, name) == 0)
				return e.action;
		}
		return XR_NULL_HANDLE;
	}

	XrAction VRInput::PadXRAction(const char* name) const
	{
		const int id = PA::FindAction(name);
		return id < PA::ActionCount ? m_padxr_actions[id] : XR_NULL_HANDLE;
	}

	bool VRInput::SuggestBindings(XrInstance instance)
	{
		const auto submit_for = [&](const std::string& profile) -> PA::SubmitFn {
			return [&, profile](const std::vector<PA::Binding>& list) {
				std::vector<XrActionSuggestedBinding> bindings;
				bindings.reserve(list.size());
				for (const PA::Binding& b : list)
				{
					XrAction action = CoreAction(b.action.c_str());
					if (action == XR_NULL_HANDLE)
						action = PadXRAction(b.action.c_str());
					XrPath path = XR_NULL_PATH;
					if (action == XR_NULL_HANDLE || XR_FAILED(xrStringToPath(instance, b.path.c_str(), &path)))
					{
						Console.Warning("(VR) xrStringToPath failed for %s", b.path.c_str());
						continue;
					}
					bindings.push_back({action, path});
				}

				XrPath profile_path = XR_NULL_PATH;
				if (!CheckXR(xrStringToPath(instance, profile.c_str(), &profile_path), "xrStringToPath(profile)"))
					return false;

				XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
				suggested.interactionProfile = profile_path;
				suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
				suggested.suggestedBindings = bindings.data();

				if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &suggested)))
				{
					Console.Warning("(VR) Runtime rejected the suggested bindings for %s", profile.c_str());
					return false;
				}

				Console.WriteLn("(VR) Suggested %u controller bindings for %s", static_cast<unsigned>(bindings.size()),
					profile.c_str());
				return true;
			};
		};

		PadXRBindings::Model model;
		if (m_padxr_set != XR_NULL_HANDLE)
		{
			const PadXRBindings::LoadReport report = model.Load([](const std::string& key) -> std::optional<std::string> {
				std::string v = Host::GetBaseStringSettingValue(PadXRBindings::kSettingsSection, key.c_str(), "");
				if (v.empty())
					return std::nullopt;
				return v;
			});
			for (const std::string& r : report.rejected)
				Console.Warning("(VR) PadXR: ignored [%s] %s (the default is kept)", PadXRBindings::kSettingsSection, r.c_str());
			if (report.overridden > 0)
				Console.WriteLn("(VR) PadXR: %d binding(s) from the settings.", report.overridden);
		}

		bool any_ok = false;
		for (const std::string& profile : PA::ProfilesToSuggest(s_frame_extension_enabled, s_hand_interaction_enabled))
		{
			const std::vector<PA::Binding> padxr =
				m_padxr_set != XR_NULL_HANDLE ? model.BindingsFor(profile) : std::vector<PA::Binding>{};
			const PA::SuggestResult r = PA::SuggestWithRetry(profile, PA::CoreBindings(profile), padxr, submit_for(profile));
			if (!r.log.empty())
				Console.Warning("(VR) %s", r.log.c_str());
			any_ok = any_ok || r.outcome != PA::SuggestOutcome::Refused;
		}

		if (!any_ok)
		{
			Console.Error("(VR) No interaction profile bindings were accepted; controllers will produce nothing.");
			return false;
		}

		return true;
	}

	bool VRInput::AttachAndCreateSpaces(XrInstance instance, XrSession session)
	{
		const XrActionSet sets[2] = {m_action_set, m_padxr_set};
		XrSessionActionSetsAttachInfo attach_info = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
		attach_info.countActionSets = m_padxr_set != XR_NULL_HANDLE ? 2u : 1u;
		attach_info.actionSets = sets;
		if (!CheckXR(xrAttachSessionActionSets(session, &attach_info), "xrAttachSessionActionSets"))
			return false;

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			XrActionSpaceCreateInfo space_info = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
			space_info.subactionPath = m_hand_paths[hand];
			space_info.poseInActionSpace.orientation.w = 1.0f;

			space_info.action = m_aim_pose;
			if (!CheckXR(xrCreateActionSpace(session, &space_info, &m_aim_spaces[hand]), "xrCreateActionSpace(aim)"))
				return false;

			space_info.action = m_grip_pose;
			if (!CheckXR(xrCreateActionSpace(session, &space_info, &m_grip_spaces[hand]), "xrCreateActionSpace(grip)"))
				return false;

			space_info.action = m_hand_aim_pose;
			if (XR_FAILED(xrCreateActionSpace(session, &space_info, &m_hand_aim_spaces[hand])))
				m_hand_aim_spaces[hand] = XR_NULL_HANDLE;
		}

		XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		rsci.poseInReferenceSpace.orientation.w = 1.0f;
		if (XR_FAILED(xrCreateReferenceSpace(session, &rsci, &m_view_space)))
		{
			m_view_space = XR_NULL_HANDLE;
			Console.Warning("(VR) VIEW space for the input snapshot failed; head_pose stays invalid.");
		}

		return true;
	}

	bool VRInput::Initialize()
	{
		if (m_action_set != XR_NULL_HANDLE)
		{
			Console.Error("(VR) VRInput::Initialize called twice.");
			return false;
		}
		if (!XRSession::HasInstance() || !XRSession::HasSession())
		{
			Console.Error("(VR) VRInput::Initialize needs a live instance and session.");
			return false;
		}

		const XrInstance instance = XRSession::GetInstance();
		const bool core_ok = CreateActionSetAndActions(instance);
		if (core_ok)
			CreatePadXRSet(instance);
		if (!core_ok || !SuggestBindings(instance) ||
			!AttachAndCreateSpaces(instance, XRSession::GetSession()))
		{
			Shutdown();
			return false;
		}

		m_attached = true;
		m_locate_warned = false;
		m_last_actions_active = -1;
		m_profile_latch = PA::ProfileLogLatch{};
		m_profile_names = HandTracking::ProfilesAtSessionStart();
		m_profile_schedule.Reset();
		m_profile_fail_warned = false;
		s_profile_changed.store(true, std::memory_order_relaxed);
		if (m_padxr_set != XR_NULL_HANDLE)
			Console.WriteLn("(VR) PadXR action set attached beside the core set.");
		s_publisher = this;
		ResetSnapshot();
		Console.WriteLn("(VR) Controller action set attached (spatial controls can read the hands).");
		return true;
	}

	void VRInput::Shutdown()
	{
		const bool session_alive = m_attached && XRSession::HasSession();
		const bool instance_alive = m_attached && XRSession::HasInstance();

		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			if (m_aim_spaces[hand] != XR_NULL_HANDLE)
			{
				if (session_alive)
					xrDestroySpace(m_aim_spaces[hand]);
				m_aim_spaces[hand] = XR_NULL_HANDLE;
			}
			if (m_grip_spaces[hand] != XR_NULL_HANDLE)
			{
				if (session_alive)
					xrDestroySpace(m_grip_spaces[hand]);
				m_grip_spaces[hand] = XR_NULL_HANDLE;
			}
			if (m_hand_aim_spaces[hand] != XR_NULL_HANDLE)
			{
				if (session_alive)
					xrDestroySpace(m_hand_aim_spaces[hand]);
				m_hand_aim_spaces[hand] = XR_NULL_HANDLE;
			}
		}
		if (m_view_space != XR_NULL_HANDLE)
		{
			if (session_alive)
				xrDestroySpace(m_view_space);
			m_view_space = XR_NULL_HANDLE;
		}

		for (XrAction* action : {&m_aim_pose, &m_grip_pose, &m_button_a, &m_button_b, &m_button_x, &m_button_y,
				 &m_button_menu, &m_thumbstick_click, &m_trigger, &m_grip, &m_thumbstick, &m_haptic,
				 &m_hand_aim_pose, &m_pinch_value, &m_pinch_ready})
		{
			if (*action != XR_NULL_HANDLE)
			{
				if (instance_alive)
					xrDestroyAction(*action);
				*action = XR_NULL_HANDLE;
			}
		}

		DestroyPadXRSet(instance_alive);

		if (m_action_set != XR_NULL_HANDLE)
		{
			if (instance_alive)
				xrDestroyActionSet(m_action_set);
			m_action_set = XR_NULL_HANDLE;
			Console.WriteLn("(VR) Controller action set destroyed.");
		}

		m_hand_paths = {};
		m_attached = false;
		if (s_publisher == this)
		{
			s_publisher = nullptr;
			ResetSnapshot();
		}
	}

	bool VRInput::ReadHand(XrSession session, int hand, XrTime time, VRHandState* out)
	{
		const XrPath hand_path = m_hand_paths[hand];

		out->a = ReadBool(session, m_button_a, hand_path);
		out->b = ReadBool(session, m_button_b, hand_path);
		out->x = ReadBool(session, m_button_x, hand_path);
		out->y = ReadBool(session, m_button_y, hand_path);
		out->menu = ReadBool(session, m_button_menu, hand_path);
		out->thumbstick_click = ReadBool(session, m_thumbstick_click, hand_path);
		out->trigger = ReadFloat(session, m_trigger, hand_path);
		out->grip = ReadFloat(session, m_grip, hand_path);

		const XrVector2f thumbstick = ReadVector2(session, m_thumbstick, hand_path);
		out->thumbstick_x = thumbstick.x;
		out->thumbstick_y = thumbstick.y;

		if (time == 0)
			return false;

		const XrSpace base_space = XRSession::GetSpace();
		constexpr XrSpaceLocationFlags POSE_VALID =
			XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

		bool ok = true;

		XrSpaceLocation aim_location = {XR_TYPE_SPACE_LOCATION};
		if (XR_SUCCEEDED(xrLocateSpace(m_aim_spaces[hand], base_space, time, &aim_location)))
		{
			if ((aim_location.locationFlags & POSE_VALID) == POSE_VALID)
			{
				CopyPose(aim_location.pose, &out->aim_pose);
				out->aim_pose.valid = true;
			}
		}
		else
		{
			ok = false;
		}

		XrSpaceVelocity grip_velocity = {XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation grip_location = {XR_TYPE_SPACE_LOCATION, &grip_velocity};
		if (XR_SUCCEEDED(xrLocateSpace(m_grip_spaces[hand], base_space, time, &grip_location)))
		{
			if ((grip_location.locationFlags & POSE_VALID) == POSE_VALID)
			{
				CopyPose(grip_location.pose, &out->grip_pose);
				out->grip_pose.valid = true;
			}
			if ((grip_velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0)
			{
				out->grip_linear_velocity = {grip_velocity.linearVelocity.x, grip_velocity.linearVelocity.y,
					grip_velocity.linearVelocity.z};
			}
		}
		else
		{
			ok = false;
		}

		return ok;
	}

	void VRInput::ReadRuntimeHand(XrSession session, int hand, XrTime time, HandTracking::RuntimeHand* out)
	{
		*out = HandTracking::RuntimeHand{};
		const XrPath hand_path = m_hand_paths[hand];
		out->pinch_value = ReadFloat(session, m_pinch_value, hand_path);
		out->pinch_ready = ReadBool(session, m_pinch_ready, hand_path);
		if (time == 0 || m_hand_aim_spaces[hand] == XR_NULL_HANDLE)
			return;
		constexpr XrSpaceLocationFlags POSE_VALID =
			XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
		if (XR_SUCCEEDED(xrLocateSpace(m_hand_aim_spaces[hand], XRSession::GetSpace(), time, &loc)) &&
			(loc.locationFlags & POSE_VALID) == POSE_VALID)
		{
			CopyPose(loc.pose, &out->aim);
			out->aim.valid = true;
		}
	}

	void VRInput::ReadPadXR(XrSession session, PA::ActionState* out)
	{
		if (m_padxr_set == XR_NULL_HANDLE)
			return;
		for (int hand = 0; hand < HAND_COUNT; ++hand)
		{
			const XrPath hand_path = m_hand_paths[hand];
			PA::HandActions& h = out->hands[hand];
			h.system = ReadBool(session, m_padxr_actions[PA::System], hand_path);
			h.menu_select = ReadBool(session, m_padxr_actions[PA::MenuSelect], hand_path);
			h.menu_back = ReadBool(session, m_padxr_actions[PA::MenuBack], hand_path);
			h.talk = ReadBool(session, m_padxr_actions[PA::Talk], hand_path);
			const XrVector2f nav = ReadVector2(session, m_padxr_actions[PA::MenuNavigate], hand_path);
			h.navigate_x = nav.x;
			h.navigate_y = nav.y;
		}
	}

	void VRInput::LogInteractionProfileIfChanged(XrInstance instance, XrSession session)
	{
		HandTracking::ProfileProbe probe;
		probe.get = [&](int hand, XrPath* out) {
			XrInteractionProfileState state = {XR_TYPE_INTERACTION_PROFILE_STATE};
			const XrResult r = xrGetCurrentInteractionProfile(session, m_hand_paths[hand], &state);
			*out = XR_SUCCEEDED(r) ? state.interactionProfile : XR_NULL_PATH;
			return r;
		};
		probe.to_string = [&](XrPath path, std::string* out) {
			char buf[XR_MAX_PATH_LENGTH] = {};
			u32 len = 0;
			const XrResult r = xrPathToString(instance, path, sizeof(buf), &len, buf);
			if (XR_SUCCEEDED(r))
				out->assign(buf, len > 0 ? len - 1 : 0);
			return r;
		};
		const HandTracking::ProfileCheckResult r = HandTracking::CheckProfiles(m_profile_names, probe);
		m_profile_names = r.names;
		m_profile_schedule.Done(r.AnyFailed());
		if (r.AnyFailed() && !m_profile_fail_warned)
		{
			m_profile_fail_warned = true;
			Console.Warning("(VR) Interaction profile check failed (left %d, right %d); keeping the last profile, "
							"retrying (next frame, then about once a second). Logged once.", static_cast<int>(r.failure[0]), static_cast<int>(r.failure[1]));
		}
		const std::array<std::string, 2>& names = m_profile_names;
		std::string line;
		if (m_profile_latch.Changed(names[0], names[1], &line))
			Console.WriteLn("%s", line.c_str());
	}

	bool VRInput::Update(XrTime display_time)
	{
		if (m_action_set == XR_NULL_HANDLE || !m_attached)
			return false;

		const XrSession session = XRSession::GetSession();
		if (session == XR_NULL_HANDLE || !XRSession::IsSessionRunning())
		{
			PublishNeutralIfActive();
			return false;
		}

		const XrActiveActionSet active_sets[2] = {{m_action_set, XR_NULL_PATH}, {m_padxr_set, XR_NULL_PATH}};
		XrActionsSyncInfo sync_info = {XR_TYPE_ACTIONS_SYNC_INFO};
		sync_info.countActiveActionSets = m_padxr_set != XR_NULL_HANDLE ? 2u : 1u;
		sync_info.activeActionSets = active_sets;
		const XrResult sync_result = xrSyncActions(session, &sync_info);
		if (!CheckXR(sync_result, "xrSyncActions"))
		{
			PublishNeutralIfActive();
			return false;
		}
		const bool actions_active = sync_result == XR_SUCCESS;
		if (m_last_actions_active != static_cast<int>(actions_active))
		{
			m_last_actions_active = static_cast<int>(actions_active);
			Console.WriteLn("(VR) Controller actions %s (session %sfocused).",
				actions_active ? "active" : "inactive", actions_active ? "" : "not ");
		}

		if (m_profile_schedule.Due(s_profile_changed.exchange(false, std::memory_order_acq_rel),
				actions_active && m_last_actions_active_for_profile != 1))
			LogInteractionProfileIfChanged(XRSession::GetInstance(), session);
		m_last_actions_active_for_profile = actions_active ? 1 : 0;

		const XrTime time = display_time;

		VRInputSnapshot snapshot;
		snapshot.actions_active = actions_active;
		bool located = true;
		for (int hand = 0; hand < HAND_COUNT; ++hand)
			located = ReadHand(session, hand, time, &snapshot.hands[hand]) && located;
		for (int hand = 0; hand < HAND_COUNT; ++hand)
			ReadRuntimeHand(session, hand, time, &m_runtime_hands[hand]);

		if (time != 0 && m_view_space != XR_NULL_HANDLE)
		{
			XrSpaceLocation head_location = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(xrLocateSpace(m_view_space, XRSession::GetSpace(), time, &head_location)))
			{
				constexpr XrSpaceLocationFlags POSE_VALID =
					XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
				if ((head_location.locationFlags & POSE_VALID) == POSE_VALID)
				{
					CopyPose(head_location.pose, &snapshot.head_pose);
					snapshot.head_pose.valid = true;
				}
			}
			else
			{
				located = false;
			}
		}

		if (!located && !m_locate_warned)
		{
			m_locate_warned = true;
			if (time == 0)
			{
				Console.Warning("(VR) Input update without a display time; poses are published invalid, "
								"buttons/axes stay live. Logged once.");
			}
			else
			{
				Console.Warning("(VR) xrLocateSpace failed (time %lld); poses will be published invalid "
								"until it recovers. Logged once.",
					static_cast<long long>(time));
			}
		}

		PA::ActionState padxr;
		padxr.active = actions_active && m_padxr_set != XR_NULL_HANDLE;
		ReadPadXR(session, &padxr);
		PublishSnapshot(PA::CoreSnapshotToPublish(snapshot, padxr), padxr);
		return true;
	}

	void VRInput::TriggerHaptic(int hand, float duration_seconds, float frequency_hz, float amplitude)
	{
		if (m_haptic == XR_NULL_HANDLE || !m_attached || !XRSession::IsSessionRunning())
			return;
		if (hand < 0 || hand >= HAND_COUNT)
			return;

		XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
		vibration.duration = duration_seconds > 0.f ?
								 static_cast<XrDuration>(duration_seconds * 1'000'000'000.0) :
								 XR_MIN_HAPTIC_DURATION;
		vibration.frequency = frequency_hz > 0.f ? frequency_hz : XR_FREQUENCY_UNSPECIFIED;
		vibration.amplitude = std::clamp(amplitude, 0.f, 1.f);

		XrHapticActionInfo action_info = {XR_TYPE_HAPTIC_ACTION_INFO};
		action_info.action = m_haptic;
		action_info.subactionPath = m_hand_paths[hand];

		xrApplyHapticFeedback(XRSession::GetSession(), &action_info,
			reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
	}

	void VRInput::StopHaptic(int hand)
	{
		if (m_haptic == XR_NULL_HANDLE || !m_attached || !XRSession::IsSessionRunning())
			return;
		if (hand < 0 || hand >= HAND_COUNT)
			return;

		XrHapticActionInfo action_info = {XR_TYPE_HAPTIC_ACTION_INFO};
		action_info.action = m_haptic;
		action_info.subactionPath = m_hand_paths[hand];

		xrStopHapticFeedback(XRSession::GetSession(), &action_info);
	}

	namespace XRInput
	{
		namespace
		{
			VRInput s_input;
			constexpr float kHapticPulseSeconds = 0.05f;

			PA::EventMachine s_events;

			std::optional<std::array<std::string, 2>> s_profiles_for_test;

			void StepPadXREvents()
			{
				const PA::ActionState state = GetPadXRActionState();
				const bool live = state.generation != 0 && !state.stale && state.active;
				const std::int64_t now_ms =
					std::chrono::duration_cast<std::chrono::milliseconds>(SnapshotNow().time_since_epoch()).count();
				const std::uint32_t events = s_events.Step(HandTracking::SystemForEvents(state, HandTracking::SystemHeld()), live, s_ui_open.load(std::memory_order_acquire), now_ms);
				if (events == PA::EventNone)
					return;
				for (const std::uint32_t e : {PA::EventMenuOpen, PA::EventMenuClose, PA::EventRecentre})
				{
					if (!(events & e))
						continue;
					Console.WriteLn("(VR) PadXR: %s", PA::EventName(e));
					if (!s_ui_open_reported.load(std::memory_order_acquire) && e != PA::EventRecentre)
						s_ui_open.store(e == PA::EventMenuOpen, std::memory_order_release);
				}
				s_padxr_events.fetch_or(events, std::memory_order_acq_rel);
			}
		}

		static void FirePulse(int hand, long long pulse)
		{
			const float seconds = static_cast<float>(PulseMs(pulse)) / 1000.f;
			const float amplitude = static_cast<float>(PulseAmp(pulse)) / 1000.f;
			s_input.TriggerHaptic(hand, seconds, 0.f, amplitude);
			GloveHaptics::Send(hand, PulseLocus(pulse), amplitude, seconds);
		}

		bool Initialize()
		{
			for (std::atomic<int>& r : s_haptic_request)
				r.store(-1, std::memory_order_relaxed);
			for (std::atomic<long long>& r : s_pulse_request)
				r.store(-1, std::memory_order_relaxed);
			s_haptic_applied[0] = s_haptic_applied[1] = 0;
			s_events.Reset();
			s_padxr_events.store(0, std::memory_order_relaxed);
			return s_input.Initialize();
		}

		void Update(XrTime display_time)
		{
			if (!s_input.IsInitialized())
				return;
			s_input.Update(display_time);
			HandTracking::Update(display_time);
			StepPadXREvents();

			for (int hand = 0; hand < 2; ++hand)
			{
				const int requested = s_haptic_request[hand].exchange(-1, std::memory_order_acq_rel);
				if (requested >= 0)
					s_haptic_applied[hand] = requested;
				if (s_haptic_applied[hand] > 0)
					s_input.TriggerHaptic(hand, kHapticPulseSeconds, 0.f, static_cast<float>(s_haptic_applied[hand]) / 1000.f);
				else if (requested == 0)
					s_input.StopHaptic(hand);

				const long long pulse = s_pulse_request[hand].exchange(-1, std::memory_order_acq_rel);
				if (pulse >= 0)
					FirePulse(hand, pulse);
			}
		}

		void Shutdown()
		{
			s_input.Shutdown();
		}

		bool IsInitialized()
		{
			return s_input.IsInitialized();
		}

		std::string CurrentInteractionProfile(int hand)
		{
			if (s_profiles_for_test)
				return (*s_profiles_for_test)[hand & 1];
			return s_input.IsInitialized() ? s_input.ProfileName(hand) : std::string();
		}

		void SetProfilesForTest(const std::array<std::string, 2>* profiles)
		{
			if (profiles)
				s_profiles_for_test = *profiles;
			else
				s_profiles_for_test.reset();
		}

		HandTracking::RuntimeHand RuntimeHandState(int hand)
		{
			return s_input.IsInitialized() ? s_input.RuntimeHandAt(hand) : HandTracking::RuntimeHand{};
		}

		void QueueHaptic(int hand, float amplitude)
		{
			if (hand < 0 || hand >= 2)
				return;
			if (!std::isfinite(amplitude))
				amplitude = 0.f;
			const int permille = static_cast<int>(std::lround(std::clamp(amplitude, 0.f, 1.f) * 1000.f));
			s_haptic_request[hand].store(permille, std::memory_order_release);
		}

		void QueuePulse(int hand, float amplitude, float seconds, u8 locus)
		{
			if (hand < 0 || hand >= 2)
				return;
			const long long packed = PackPulse(amplitude, seconds, locus);
#ifdef VR_FORKBUGS_PLANTED_B3
			s_pulse_request[hand].store(packed, std::memory_order_release);
#else
			long long current = s_pulse_request[hand].load(std::memory_order_acquire);
			for (;;)
			{
				if (current >= 0)
				{
					const int cur_amp = PulseAmp(current), new_amp = PulseAmp(packed);
					if (cur_amp > new_amp || (cur_amp == new_amp && PulseMs(current) >= PulseMs(packed)))
						return;
				}
				if (s_pulse_request[hand].compare_exchange_weak(current, packed, std::memory_order_acq_rel,
						std::memory_order_acquire))
					return;
			}
#endif
		}

		void DrainPulsesForTest()
		{
			for (int hand = 0; hand < 2; ++hand)
			{
				const long long pulse = s_pulse_request[hand].exchange(-1, std::memory_order_acq_rel);
				if (pulse >= 0)
					FirePulse(hand, pulse);
			}
		}

		bool TakePendingPulseForTest(int hand, float* amplitude, float* seconds, u8* locus)
		{
			if (hand < 0 || hand >= 2)
				return false;
			const long long pulse = s_pulse_request[hand].exchange(-1, std::memory_order_acq_rel);
			if (pulse < 0)
				return false;
			if (amplitude)
				*amplitude = static_cast<float>(PulseAmp(pulse)) / 1000.f;
			if (seconds)
				*seconds = static_cast<float>(PulseMs(pulse)) / 1000.f;
			if (locus)
				*locus = PulseLocus(pulse);
			return true;
		}

		void PublishSnapshotForTest(const VRInputSnapshot& snapshot)
		{
			PublishSnapshot(snapshot);
		}

		void PublishSnapshotForTest(const VRInputSnapshot& snapshot, const PadXRActions::ActionState& padxr)
		{
			PublishSnapshot(PA::CoreSnapshotToPublish(snapshot, padxr), padxr);
		}

		void SetFrameControllerExtensionEnabled(bool enabled)
		{
			s_frame_extension_enabled = enabled;
		}

		void SetHandInteractionExtensionEnabled(bool enabled)
		{
			s_hand_interaction_enabled = enabled;
		}

		void NoteInteractionProfileChanged()
		{
			s_profile_changed.store(true, std::memory_order_release);
		}

		std::uint32_t TakePadXREvents()
		{
			return s_padxr_events.exchange(0, std::memory_order_acq_rel);
		}

		void NoteUiOpen(bool open)
		{
			s_ui_open_reported.store(true, std::memory_order_release);
			s_ui_open.store(open, std::memory_order_release);
		}

		void ResetSnapshotForTest()
		{
			ResetSnapshot();
			s_test_clock_offset_ms.store(0, std::memory_order_relaxed);
		}

		void PublishNeutralIfActiveForTest()
		{
			PublishNeutralIfActive();
		}

		void AdvanceClockForTest(s64 milliseconds)
		{
			s_test_clock_offset_ms.fetch_add(milliseconds, std::memory_order_relaxed);
		}
	}
}
