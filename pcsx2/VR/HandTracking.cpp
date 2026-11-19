// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/HandTracking.h"

#include "VR/VRInput.h"
#include "VR/XRSession.h"

#include "common/Console.h"

#include <atomic>

namespace VR::HandTracking
{
	namespace
	{
		constexpr XrSpaceLocationFlags kPoseValid =
			XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

		void CopyPose(const XrPosef& p, VRPose* out)
		{
			out->orientation_xyzw = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
			out->position_xyz = {p.position.x, p.position.y, p.position.z};
		}

		bool PositionValid(const XrHandJointLocationEXT& j)
		{
#ifdef VR_HANDTRACK_PLANTED_IGNORE_VALID_BITS
			(void)j;
			return true;
#else
			return (j.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
#endif
		}

		bool PoseValid(const XrHandJointLocationEXT& j)
		{
#ifdef VR_HANDTRACK_PLANTED_IGNORE_VALID_BITS
			(void)j;
			return true;
#else
			return (j.locationFlags & kPoseValid) == kPoseValid;
#endif
		}

		XrInstance s_instance = XR_NULL_HANDLE;
		bool s_enabled = false;
		PFN_xrCreateHandTrackerEXT s_xrCreateHandTrackerEXT = nullptr;
		PFN_xrDestroyHandTrackerEXT s_xrDestroyHandTrackerEXT = nullptr;
		PFN_xrLocateHandJointsEXT s_xrLocateHandJointsEXT = nullptr;
		std::array<XrHandTrackerEXT, 2> s_trackers{XR_NULL_HANDLE, XR_NULL_HANDLE};
		PinchSource s_source;
		bool s_locate_warned = false;
		std::atomic<bool> s_system_held{false};
		std::array<NearField::JointSet, 2> s_joints{};

		void Publish(bool held) { s_system_held.store(held, std::memory_order_release); }

		bool HaveTrackers() { return s_trackers[0] != XR_NULL_HANDLE || s_trackers[1] != XR_NULL_HANDLE; }
	}

	WristPinch::HandJoints JointsFromLocations(bool is_active, const XrHandJointLocationEXT* joints, std::uint32_t count)
	{
		WristPinch::HandJoints out;
		if (!is_active || !joints || count < XR_HAND_JOINT_COUNT_EXT)
			return out;
		out.tracked = true;
		const XrHandJointLocationEXT& wrist = joints[XR_HAND_JOINT_WRIST_EXT];
		const XrHandJointLocationEXT& thumb = joints[XR_HAND_JOINT_THUMB_TIP_EXT];
		const XrHandJointLocationEXT& index = joints[XR_HAND_JOINT_INDEX_TIP_EXT];
		if (PoseValid(wrist))
		{
			CopyPose(wrist.pose, &out.wrist);
			out.wrist.valid = true;
		}
		out.tips_valid = PositionValid(thumb) && PositionValid(index);
		out.thumb_tip = {thumb.pose.position.x, thumb.pose.position.y, thumb.pose.position.z};
		out.index_tip = {index.pose.position.x, index.pose.position.y, index.pose.position.z};
		return out;
	}

	bool IsControllerProfile(std::string_view profile)
	{
		return !profile.empty() && profile != kHandInteractionProfile;
	}

	std::string NextProfileName(std::string_view previous, ProfileCheck check, std::string_view name)
	{
		switch (check)
		{
			case ProfileCheck::Named:
				return std::string(name);
			case ProfileCheck::None:
				return std::string();
			case ProfileCheck::Failed:
			default:
#ifdef VR_HANDTRACK_PLANTED_FAILED_CHECK_CLEARS
				return std::string();
#else
				return std::string(previous);
#endif
		}
	}

	ProfileCheckResult CheckProfiles(const std::array<std::string, 2>& previous, const ProfileProbe& probe)
	{
		ProfileCheckResult r;
		for (int hand = 0; hand < 2; ++hand)
		{
			XrPath path = XR_NULL_PATH;
			std::string name;
			ProfileCheck check = ProfileCheck::Failed;
			const XrResult got = probe.get(hand, &path);
			if (XR_FAILED(got))
				r.failure[hand] = got;
			else if (path == XR_NULL_PATH)
				check = ProfileCheck::None;
			else
			{
				std::string s;
				const XrResult str = probe.to_string(path, &s);
#ifdef VR_HANDTRACK_PLANTED_STRING_FAIL_IS_NAMED
				check = ProfileCheck::Named;
				name = s;
				(void)str;
#else
				if (XR_SUCCEEDED(str))
				{
					check = ProfileCheck::Named;
					name = std::move(s);
				}
				else
					r.failure[hand] = str;
#endif
			}
			r.checks[hand] = check;
			r.names[hand] = NextProfileName(previous[hand], check, name);
		}
		return r;
	}

	bool ProfileCheckSchedule::Due(bool event, bool focus_gain)
	{
		const bool backstop = ++m_since >= kBackstopUpdates;
		return event || backstop || focus_gain || m_retry_owed;
	}

	void ProfileCheckSchedule::Done(bool failed)
	{
		m_since = 0;
#ifdef VR_HANDTRACK_PLANTED_FAILED_CHECK_NO_RETRY
		(void)failed;
		m_retry_owed = false;
#else
		if (failed && m_retries < kMaxQuickRetries)
		{
			++m_retries;
			m_retry_owed = true;
		}
		else
		{
			m_retry_owed = false;
			if (!failed)
				m_retries = 0;
		}
#endif
	}

	void ProfileCheckSchedule::Reset()
	{
		m_since = 0;
		m_retries = 0;
		m_retry_owed = false;
	}

	std::array<bool, 2> ControllersInHands(const VRInputSnapshot& snap, const std::array<std::string, 2>& profiles)
	{
#ifdef VR_HANDTRACK_PLANTED_PROFILES_SWAPPED
		return {ControllerInHand(snap.hands[0], profiles[1]), ControllerInHand(snap.hands[1], profiles[0])};
#else
		return {ControllerInHand(snap.hands[0], profiles[0]), ControllerInHand(snap.hands[1], profiles[1])};
#endif
	}

	std::array<std::string, 2> CurrentProfiles()
	{
		return {XRInput::CurrentInteractionProfile(0), XRInput::CurrentInteractionProfile(1)};
	}

	std::array<std::string, 2> ProfilesAtSessionStart()
	{
#ifdef VR_HANDTRACK_PLANTED_SESSION_STARTS_BARE
		return {};
#else
		return {kProfileUnchecked, kProfileUnchecked};
#endif
	}

	bool ControllerInHand([[maybe_unused]] const VRHandState& hand, [[maybe_unused]] std::string_view profile)
	{
#ifdef VR_HANDTRACK_PLANTED_POSE_MEANS_CONTROLLER
		return hand.aim_pose.valid || hand.grip_pose.valid;
#else
		return IsControllerProfile(profile);
#endif
	}

	bool PinchSource::Step(const std::array<WristPinch::HandJoints, 2>& hands, const VRPose& head,
		const std::array<bool, 2>& controller_in_hand, const WristPinch::Params& params)
	{
		bool held = false;
		for (int h = 0; h < 2; ++h)
		{
			WristPinch::HandJoints joints = hands[h];
#ifndef VR_HANDTRACK_PLANTED_NO_CONTROLLER_GATE
			if (controller_in_hand[h])
				joints.tracked = false;
#endif
			m_last[h] = m_detectors[h].Step(joints, head, params);
			held = held || m_last[h].held;
		}
		m_held = held;
		return held;
	}

	void PinchSource::Reset()
	{
		for (WristPinch::Detector& d : m_detectors)
			d.Reset();
		m_last = {};
		m_held = false;
	}

	bool SystemForEvents(const PadXRActions::ActionState& state, bool pinch_held)
	{
#ifdef VR_HANDTRACK_PLANTED_PINCH_NOT_MERGED
		(void)pinch_held;
		return state.SystemHeld();
#else
		return state.SystemHeld() || pinch_held;
#endif
	}

	void SetExtensionEnabled(XrInstance instance, bool enabled)
	{
		s_instance = instance;
		s_enabled = false;
		s_xrCreateHandTrackerEXT = nullptr;
		s_xrDestroyHandTrackerEXT = nullptr;
		s_xrLocateHandJointsEXT = nullptr;
		if (instance == XR_NULL_HANDLE)
			return;
		if (!enabled)
		{
			Console.WriteLn("(VR) Hand tracking: %s not offered by the runtime; the wrist pinch is off "
							"(controllers unchanged).", kExtensionName);
			return;
		}
		const bool ok =
			XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT",
				reinterpret_cast<PFN_xrVoidFunction*>(&s_xrCreateHandTrackerEXT))) &&
			XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT",
				reinterpret_cast<PFN_xrVoidFunction*>(&s_xrDestroyHandTrackerEXT))) &&
			XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT",
				reinterpret_cast<PFN_xrVoidFunction*>(&s_xrLocateHandJointsEXT))) &&
			s_xrCreateHandTrackerEXT && s_xrDestroyHandTrackerEXT && s_xrLocateHandJointsEXT;
		if (!ok)
		{
			s_xrCreateHandTrackerEXT = nullptr;
			s_xrDestroyHandTrackerEXT = nullptr;
			s_xrLocateHandJointsEXT = nullptr;
			Console.Warning("(VR) Hand tracking: %s enabled but its entry points did not load; the wrist pinch is off.",
				kExtensionName);
			return;
		}
		s_enabled = true;
		Console.WriteLn("(VR) Hand tracking: %s enabled (the wrist pinch reads joints).",
			kExtensionName);
	}

	bool IsExtensionEnabled()
	{
		return s_enabled;
	}

	bool CreateTrackers(XrSession session)
	{
		DestroyTrackers();
		if (!s_enabled || session == XR_NULL_HANDLE || s_instance == XR_NULL_HANDLE)
			return false;

		XrSystemHandTrackingPropertiesEXT htp = {XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		sp.next = &htp;
		if (XR_FAILED(xrGetSystemProperties(s_instance, XRSession::GetSystemId(), &sp)) || htp.supportsHandTracking != XR_TRUE)
		{
			Console.WriteLn("(VR) Hand tracking: the system does not support it; the wrist pinch is off.");
			return false;
		}

		for (int h = 0; h < 2; ++h)
		{
			XrHandTrackerCreateInfoEXT ci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
			ci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
			ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
			const XrResult res = s_xrCreateHandTrackerEXT(session, &ci, &s_trackers[h]);
			if (XR_FAILED(res))
			{
				s_trackers[h] = XR_NULL_HANDLE;
				Console.Warning("(VR) Hand tracking: xrCreateHandTrackerEXT (%s) failed (%d); the wrist pinch is off.",
					h == 0 ? "left" : "right", static_cast<int>(res));
				DestroyTrackers();
				return false;
			}
		}
		s_source.Reset();
		s_locate_warned = false;
		s_joints = {};
		Publish(false);
		Console.WriteLn("(VR) Hand tracking: one tracker per hand created.");
		return true;
	}

	void DestroyTrackers()
	{
		for (XrHandTrackerEXT& t : s_trackers)
		{
			if (t != XR_NULL_HANDLE && s_xrDestroyHandTrackerEXT)
			{
				try
				{
					s_xrDestroyHandTrackerEXT(t);
				}
				catch (...)
				{
					Console.Warning("(VR) xrDestroyHandTrackerEXT threw during teardown; continuing.");
				}
			}
			t = XR_NULL_HANDLE;
		}
		s_source.Reset();
		s_joints = {};
		Publish(false);
	}

	void ParkHandles()
	{
		s_trackers = {XR_NULL_HANDLE, XR_NULL_HANDLE};
		s_instance = XR_NULL_HANDLE;
		s_enabled = false;
		s_xrCreateHandTrackerEXT = nullptr;
		s_xrDestroyHandTrackerEXT = nullptr;
		s_xrLocateHandJointsEXT = nullptr;
		s_source.Reset();
		s_joints = {};
		Publish(false);
	}

	bool HasTrackers()
	{
		return HaveTrackers();
	}

	bool Update(XrTime display_time)
	{
		if (!HaveTrackers() || !s_xrLocateHandJointsEXT || display_time == 0)
		{
			s_joints = {};
#ifndef VR_HANDTRACK_PLANTED_HELD_SURVIVES
			s_source.Reset();
			Publish(false);
#endif
			return false;
		}

		const XrSpace base = XRSession::GetSpace();
		const VRInputSnapshot snap = GetInputSnapshot();
		std::array<WristPinch::HandJoints, 2> hands{};
		s_joints = {};
		for (int h = 0; h < 2; ++h)
		{
			if (s_trackers[h] == XR_NULL_HANDLE || base == XR_NULL_HANDLE)
				continue;
			std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
			XrHandJointLocationsEXT locations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
			locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
			locations.jointLocations = joints.data();
			XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
			li.baseSpace = base;
			li.time = display_time;
			const XrResult res = s_xrLocateHandJointsEXT(s_trackers[h], &li, &locations);
			if (XR_FAILED(res))
			{
				if (!s_locate_warned)
				{
					s_locate_warned = true;
					Console.Warning("(VR) Hand tracking: xrLocateHandJointsEXT failed (%d); that hand reads untracked. "
									"Logged once.", static_cast<int>(res));
				}
				continue;
			}
			hands[h] = JointsFromLocations(locations.isActive == XR_TRUE, joints.data(), locations.jointCount);
			s_joints[h] = JointSetFromLocations(locations.isActive == XR_TRUE, joints.data(), locations.jointCount);
		}

		VRPose head = snap.head_pose;
		if (snap.generation == 0 || snap.stale)
			head.valid = false;
		const std::array<bool, 2> controllers = ControllersInHands(snap, CurrentProfiles());
		Publish(s_source.Step(hands, head, controllers));
		return true;
	}

	bool SystemHeld()
	{
		return s_system_held.load(std::memory_order_acquire);
	}

	NearField::JointSet JointSetFromLocations(bool is_active, const XrHandJointLocationEXT* joints, std::uint32_t count)
	{
		NearField::JointSet out;
		if (!is_active || !joints || count < XR_HAND_JOINT_COUNT_EXT)
			return out;
		static_assert(XR_HAND_JOINT_COUNT_EXT == HandsGltf::kJointCount, "the glove's skin is the 26 XR joints");
		out.active = true;
		for (int j = 0; j < HandsGltf::kJointCount; j++)
		{
			CopyPose(joints[j].pose, &out.pose[j]);
			out.pose[j].valid = PoseValid(joints[j]);
		}
		return out;
	}

	const std::array<NearField::JointSet, 2>& JointsForDraw()
	{
		return s_joints;
	}

	void SetJointsForTest(const std::array<NearField::JointSet, 2>& sets)
	{
		s_joints = sets;
	}

	bool StepForTest(const std::array<WristPinch::HandJoints, 2>& hands, const VRPose& head,
		const std::array<bool, 2>& controller_in_hand)
	{
		const bool held = s_source.Step(hands, head, controller_in_hand);
		Publish(held);
		return held;
	}

	void ResetForTest()
	{
		s_trackers = {XR_NULL_HANDLE, XR_NULL_HANDLE};
		s_source.Reset();
		s_locate_warned = false;
		s_joints = {};
		Publish(false);
	}
}
