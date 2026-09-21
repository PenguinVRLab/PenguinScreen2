// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include "VR/VRInputState.h"

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace VR::SpatialControls
{
	struct Vec3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};
	Vec3 operator+(const Vec3& a, const Vec3& b);
	Vec3 operator-(const Vec3& a, const Vec3& b);
	Vec3 operator*(const Vec3& a, float s);
	float Dot(const Vec3& a, const Vec3& b);
	float Length(const Vec3& a);

	struct Quat
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
		Vec3 Rotate(const Vec3& v) const;
		static Quat FromYaw(float radians);
		Quat Conjugate() const;
		Quat operator*(const Quat& other) const;
	};

	struct Frame
	{
		Vec3 pivot;
		Quat orientation;
		Vec3 Right() const { return orientation.Rotate({1.0f, 0.0f, 0.0f}); }
		Vec3 Up() const { return orientation.Rotate({0.0f, 1.0f, 0.0f}); }
		Vec3 Forward() const { return orientation.Rotate({0.0f, 0.0f, -1.0f}); }
		Vec3 ToWorld(const Vec3& local) const { return pivot + orientation.Rotate(local); }
		Vec3 ToLocal(const Vec3& world) const { return orientation.Conjugate().Rotate(world - pivot); }
	};

	struct Anchor
	{
		Vec3 position;
		float yaw = 0.0f;
	};

	struct Placement
	{
		float side = 0.0f;
		float height = 0.0f;
		float forward = 0.0f;
		float yaw_deg = 0.0f;
	};

	Frame PlaceControl(const Anchor& anchor, const Placement& placement);

	struct HandInput
	{
		bool valid = false;
		Vec3 position;
		Quat orientation;
		float squeeze = 0.0f;
		float trigger = 0.0f;
	};
	struct Hands
	{
		std::array<HandInput, 2> hand{};
	};
	Hands HandsFromSnapshot(const VRInputSnapshot& snapshot);

	enum class ReleaseMode
	{
		Latch,
		Spring,
		Detent,
	};

	enum class GrabMode
	{
		Hold,
		Toggle,
	};

	struct GrabParams
	{
		float grab_radius = 0.05f;
		float grab_length = 0.12f;
		float grab_on = 0.6f;
		float grab_off = 0.4f;
		GrabMode grab_mode = GrabMode::Toggle;
		float break_away = 0.25f;
		float idle_detent = 0.03f;
		ReleaseMode release = ReleaseMode::Latch;
		float spring_rate = 4.0f;
	};

	constexpr GrabParams SpringGrab()
	{
		GrabParams g;
		g.release = ReleaseMode::Spring;
		return g;
	}

	constexpr GrabParams HoldGrab()
	{
		GrabParams g;
		g.grab_mode = GrabMode::Hold;
		return g;
	}

	struct Axis1D
	{
		float value = 0.0f;
		float offset = 0.0f;
		int hand = -1;
		bool broke_away = false;
		bool Held() const { return hand >= 0; }
		void Reset()
		{
			value = 0.0f;
			offset = 0.0f;
			hand = -1;
			broke_away = false;
		}
	};

	struct GripEdges
	{
		std::array<bool, 2> on{};
	};

	struct Rail
	{
		float lo = -1.0f;
		float hi = 1.0f;
		int detents = 0;
	};

	struct Reach
	{
		float raw = 0.0f;
		bool in_reach = false;
		float distance = 0.0f;
		bool grip_edge = false;
	};

	void MarkGripEdges(const GrabParams& grab, const Hands& hands, GripEdges& st, std::array<Reach, 2>& reach);

	float GripDistance(const Vec3& hand, const Vec3& grip_centre, const Vec3& grip_axis, float grip_length);

	void Step1D(const GrabParams& grab, const Rail& rail, const Hands& hands, const std::array<Reach, 2>& reach,
		float dt, Axis1D& st);

	float ApplyIdleDetent(float v, float idle_detent);

	float ApplyOutputFloor(float v, float deadband, float floor);

	struct SlideParams
	{
		Vec3 rail_dir{0.0f, 0.0f, -1.0f};
		Vec3 grip_dir{0.0f, 1.0f, 0.0f};
		float travel = 0.12f;
		bool one_way = false;
		int detents = 0;
	};
	struct SlideState
	{
		Axis1D axis;
		GripEdges grip;
	};
	Vec3 SlideKnobPosition(const Frame& frame, const SlideParams& p, float value);
	void StepSlide(const Frame& frame, const SlideParams& p, const GrabParams& grab, const Hands& hands, float dt,
		SlideState& st);

	enum class RotaryGrip : u8
	{
		Rim,
		Tip,
	};
	struct RotaryParams
	{
		float lock_to_lock_deg = 900.0f;
		float rim_radius = 0.18f;
		RotaryGrip grip = RotaryGrip::Rim;
		bool tip_upright = false;
		bool two_hand = true;
		bool one_way = false;
		int detents = 0;
	};
	struct RotaryState
	{
		Axis1D axis;
		GripEdges grip;
		std::array<float, 2> cont_angle{};
		std::array<bool, 2> cont_valid{};
		float pair_angle = 0.0f;
		bool pair_valid = false;
		int second_hand = -1;
		float pair_offset = 0.0f;
		bool second_broke_away = false;
	};
	void StepRotary(const Frame& frame, const RotaryParams& p, const GrabParams& grab, const Hands& hands, float dt,
		RotaryState& st);

	struct LeverParams
	{
		float sweep_deg = 60.0f;
		float arm_length = 0.20f;
		bool upright = false;
		bool one_way = false;
		int detents = 0;
	};
	using LeverState = RotaryState;
	Frame LeverArmFrame(const Frame& control, const LeverParams& p);
	Vec3 LeverGripPosition(const Frame& control, const LeverParams& p, float value);
	Vec3 LeverGripAxis(const Frame& control, const LeverParams& p, float value);
	void StepLever(const Frame& control, const LeverParams& p, const GrabParams& grab, const Hands& hands, float dt,
		LeverState& st);

	struct GimbalParams
	{
		float travel_x = 0.10f;
		float travel_y = 0.10f;
		bool twist = false;
		float twist_range_deg = 60.0f;
	};
	struct GimbalState
	{
		Axis1D x, y, twist;
		GripEdges grip;
		int hand = -1;
		Quat grab_orientation;
		float cont_twist = 0.0f;
		bool cont_twist_valid = false;
	};
	Vec3 GimbalKnobPosition(const Frame& frame, const GimbalParams& p, const GimbalState& st);
	void StepGimbal(const Frame& frame, const GimbalParams& p, const GrabParams& grab, const Hands& hands, float dt,
		GimbalState& st);

	struct SelectorParams
	{
		Vec3 rail_dir{0.0f, 0.0f, -1.0f};
		Vec3 grip_dir{0.0f, 1.0f, 0.0f};
		int positions = 6;
		float travel = 0.10f;
		float hysteresis = 0.15f;
		bool on_arc = false;
		LeverParams arc;
	};
	struct SelectorState
	{
		int index = 0;
		float offset = 0.0f;
		int hand = -1;
		bool shifted_up = false;
		bool shifted_down = false;
		bool broke_away = false;
		GripEdges grip;
		void Reset()
		{
			index = 0;
			offset = 0.0f;
			hand = -1;
			shifted_up = shifted_down = false;
			broke_away = false;
			grip = {};
		}
	};
	Vec3 SelectorKnobPosition(const Frame& frame, const SelectorParams& p, int index);
	void StepSelector(const Frame& frame, const SelectorParams& p, const GrabParams& grab, const Hands& hands,
		float dt, SelectorState& st);

	static constexpr float kResetChordHoldSeconds = 0.5f;
	struct ChordState
	{
		float held_s = 0.0f;
		bool fired = false;
	};
	bool StepResetChord(bool left_click, bool right_click, float dt, float hold_s, ChordState& st);

	struct HapticCue
	{
		float amplitude;
		float seconds;
	};
	static constexpr HapticCue kHapticGrab{0.5f, 0.03f};
	static constexpr HapticCue kHapticRelease{0.3f, 0.02f};
	static constexpr HapticCue kHapticRailHit{1.0f, 0.06f};
	static constexpr HapticCue kHapticBreakAway{0.8f, 0.10f};

	enum class DeviceKind : u8
	{
		Throttle,
		TwinThrottles,
		Wheel,
		Shifter,
		Stick,
		LightGun,
		Count,
	};

	enum class ControlId : u32
	{
		LeftLever,
		RightLever,
		Power,
		Steer,
		GrabbedLeft,
		GrabbedRight,
		Throttle,
		Grabbed,
		Notch1,
		Notch2,
		Notch3,
		Notch4,
		Notch5,
		Notch6,
		Notch7,
		Notch8,
		Steering,
		Gear1,
		Gear2,
		Gear3,
		Gear4,
		Gear5,
		Gear6,
		GearR,
		GearN,
		ShiftUp,
		ShiftDown,
		X,
		Y,
		Twist,
		Trigger,
		PointerX,
		PointerY,
		OnScreen,
		A,
		B,
		Count,
	};
	static constexpr u32 kControlCount = static_cast<u32>(ControlId::Count);
	static constexpr int kMaxNotches = 8;

	enum class ControlType : u8
	{
		Axis,
		UnitAxis,
		Button,
	};

	struct ControlDef
	{
		ControlId id;
		const char* name;
		ControlType type;
	};

	struct DeviceDef
	{
		DeviceKind kind;
		const char* name;
		std::span<const ControlDef> controls;
	};

	std::span<const DeviceDef> Catalogue();
	ControlType ControlTypeOf(ControlId id);
	const DeviceDef* FindDevice(std::string_view name);
	const DeviceDef* FindDevice(DeviceKind kind);
	const ControlDef* FindControl(const DeviceDef& device, std::string_view name);
	const ControlDef* FindControl(const DeviceDef& device, ControlId id);
	const char* ControlName(ControlId id);
	std::optional<ControlId> ParseControlId(std::string_view name);
	const char* DeviceName(DeviceKind kind);

	using ControlValues = std::array<float, kControlCount>;

	enum class PowerMode : u8
	{
		Sum,
		Max,
	};

	enum class TwinMode : u8
	{
		Composed,
		Direct,
	};

	struct TwinThrottlesParams
	{
		LeverParams lever;
		GrabParams grab;
		float lever_spacing = 0.50f;
		PowerMode power = PowerMode::Sum;
		float steer_full_lock = 1.0f;
		float steer_deadband = 0.05f;
		float steer_curve = 1.0f;
		float steer_sign = 1.0f;
		float output_floor = 0.0f;
		float engage = 0.0f;
		float engage_hysteresis = 0.05f;
		TwinMode mode = TwinMode::Composed;
		float lever_sign = 1.0f;
	};
	struct TwinThrottlesState
	{
		LeverState left, right;
		bool engaged = false;
		void Reset()
		{
			left = LeverState{};
			right = LeverState{};
			engaged = false;
		}
	};
	Frame LeverFrame(const Frame& control, const TwinThrottlesParams& p, int side );
	void StepTwinThrottles(const Frame& frame, const TwinThrottlesParams& p, const Hands& hands, float dt,
		TwinThrottlesState& st);
	float TwinThrottlesPower(float left, float right, const TwinThrottlesParams& p);
	float TwinThrottlesSteer(float left, float right, const TwinThrottlesParams& p);
	float TwinThrottlesLeverOutput(float lever, const TwinThrottlesParams& p);
	bool TwinThrottlesEngage(float power, const TwinThrottlesParams& p, bool engaged);
	ControlValues ComposeTwinThrottles(const TwinThrottlesParams& p, const TwinThrottlesState& st);

	struct ThrottleParams
	{
		LeverParams lever;
		GrabParams grab;
		int detented = 0;
	};
	struct ThrottleState
	{
		LeverState lever;
		SelectorState notch;
		void Reset()
		{
			lever = LeverState{};
			notch.Reset();
		}
	};
	void StepThrottle(const Frame& frame, const ThrottleParams& p, const Hands& hands, float dt, ThrottleState& st);
	ControlValues ComposeThrottle(const ThrottleParams& p, const ThrottleState& st);

	struct WheelParams
	{
		RotaryParams rim;
		GrabParams grab = SpringGrab();
	};
	struct WheelState
	{
		RotaryState rotary;
		void Reset() { rotary = RotaryState{}; }
	};
	void StepWheel(const Frame& frame, const WheelParams& p, const Hands& hands, float dt, WheelState& st);
	ControlValues ComposeWheel(const WheelParams& p, const WheelState& st);

	struct ShifterParams
	{
		SelectorParams gate;
		GrabParams grab = HoldGrab();
		int gears = 6;
		bool has_reverse = true;
		bool has_neutral = true;
	};
	struct ShifterState
	{
		SelectorState sel;
		void Reset() { sel.Reset(); }
	};
	int ShifterPositions(const ShifterParams& p);
	void StepShifter(const Frame& frame, const ShifterParams& p, const Hands& hands, float dt, ShifterState& st);
	ControlValues ComposeShifter(const ShifterParams& p, const ShifterState& st);

	struct StickParams
	{
		GimbalParams gimbal;
		GrabParams grab = SpringGrab();
	};
	struct StickState
	{
		GimbalState gimbal;
		void Reset() { gimbal = GimbalState{}; }
	};
	void StepStick(const Frame& frame, const StickParams& p, const Hands& hands, float dt, StickState& st);
	ControlValues ComposeStick(const StickParams& p, const StickState& st, const Hands& hands);

	struct LightGunParams
	{
		int aim_hand = VRInputSnapshot::RIGHT;
	};
	ControlValues ComposeLightGun(const LightGunParams& p, const Hands& hands);
}
