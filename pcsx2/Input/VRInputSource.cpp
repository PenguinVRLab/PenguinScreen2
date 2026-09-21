// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "Input/VRInputSource.h"

#include "Input/InputManager.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "USB/USB.h"
#include "VMManager.h"
#include "Memory.h"
#include "MemoryTypes.h"

#include "VR/CameraDriver.h"
#include "VR/ControlQuads.h"
#include "VR/SplitState.h"
#include "VR/VRInput.h"
#include "VR/XRCompositor.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace SC = VR::SpatialControls;

namespace
{
	std::mutex s_ids_mutex;
	std::vector<std::string> s_ids;

	bool ValidId(std::string_view id)
	{
		if (id.empty() || id.size() > 32)
			return false;
		for (const char c : id)
		{
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
			if (!ok)
				return false;
		}
		return true;
	}

	std::mutex s_specs_mutex;
	std::vector<VR::ProfileDB::SpatialControlSpec> s_specs;
	u64 s_specs_generation = 1;

	bool DefaultGuestReader(u32 address, u8 width, u32* value)
	{
		if (!VMManager::HasValidVM())
			return false;
		if (static_cast<u64>(address) + width > Ps2MemSize::MainRam)
			return false;
		switch (width)
		{
			case 1: *value = memRead8(address); break;
			case 2: *value = memRead16(address); break;
			default: *value = memRead32(address); break;
		}
		return true;
	}
	std::atomic<VRInputSource::GuestReader> s_guest_reader{nullptr};

	std::atomic<u64> s_events_emitted{0};

	constexpr const char* kMotorName = "Rumble";

	const char* ModifierPrefix(InputModifier m)
	{
		switch (m)
		{
			case InputModifier::Negate: return "-";
			case InputModifier::FullAxis: return "";
			case InputModifier::None:
			default: return "+";
		}
	}
}

std::optional<u32> VRInputSource::FindDeviceIndex(std::string_view id)
{
	std::lock_guard lock(s_ids_mutex);
	for (size_t i = 0; i < s_ids.size(); ++i)
	{
		if (s_ids[i] == id)
			return static_cast<u32>(i);
	}
	return std::nullopt;
}

std::optional<u32> VRInputSource::InternDeviceId(std::string_view id)
{
	if (!ValidId(id))
		return std::nullopt;
	std::lock_guard lock(s_ids_mutex);
	for (size_t i = 0; i < s_ids.size(); ++i)
	{
		if (s_ids[i] == id)
			return static_cast<u32>(i);
	}
	if (s_ids.size() >= 255)
	{
		Console.Error("(VR) VRInputSource: more than 255 distinct control ids seen; '%.*s' cannot be bound.",
			static_cast<int>(id.size()), id.data());
		return std::nullopt;
	}
	s_ids.emplace_back(id);
	return static_cast<u32>(s_ids.size() - 1);
}

std::string VRInputSource::DeviceIdForIndex(u32 index)
{
	std::lock_guard lock(s_ids_mutex);
	return (index < s_ids.size()) ? s_ids[index] : std::string("?");
}

std::string VRInputSource::DeviceName(std::string_view id)
{
	return fmt::format("VR-{}", id);
}

InputBindingKey VRInputSource::MakeControlKey(u32 device_index, SC::ControlId id)
{
	InputBindingKey key = {};
	key.source_type = InputSourceType::VR;
	key.source_index = device_index;
	key.source_subtype = (SC::ControlTypeOf(id) == SC::ControlType::Button) ? InputSubclass::ControllerButton :
																			  InputSubclass::ControllerAxis;
	key.data = static_cast<u32>(id);
	return key;
}

InputBindingKey VRInputSource::MakeMotorKey(u32 device_index)
{
	InputBindingKey key = {};
	key.source_type = InputSourceType::VR;
	key.source_index = device_index;
	key.source_subtype = InputSubclass::ControllerMotor;
	key.data = 0;
	return key;
}

void VRInputSource::SetActiveControls(std::vector<VR::ProfileDB::SpatialControlSpec> specs)
{
	std::lock_guard lock(s_specs_mutex);
	s_specs = std::move(specs);
	s_specs_generation++;
}

std::vector<VR::ProfileDB::SpatialControlSpec> VRInputSource::GetActiveControls()
{
	std::lock_guard lock(s_specs_mutex);
	return s_specs;
}

void VRInputSource::SetGuestReader(GuestReader reader)
{
	s_guest_reader.store(reader, std::memory_order_release);
}

u64 VRInputSource::EventsEmittedForTest()
{
	return s_events_emitted.load(std::memory_order_relaxed);
}

float VRInputSource::CompensateAxisScale(float value, float axis_scale)
{
	if (!std::isfinite(value))
		return 0.0f;
	if (!std::isfinite(axis_scale) || axis_scale <= 0.0f || axis_scale == 1.0f)
		return value;
	float c = value / axis_scale;
	const float back = c * axis_scale;
	if (std::fabs(back) < std::fabs(value))
		c = std::nextafter(c, (c < 0.0f) ? -2.0f : 2.0f);
	return c;
}

bool VRInputSource::EvaluateGuards(const std::vector<VR::ProfileDB::CameraGuard>& guards, GuestReader reader)
{
	if (!reader)
		reader = DefaultGuestReader;
	for (const VR::ProfileDB::CameraGuard& g : guards)
	{
		u32 address = g.ee_address;
		if (g.is_chain)
		{
			u32 deref = 0;
			if (!reader(g.pointer_addr, 4, &deref) || deref == 0 || deref >= Ps2MemSize::MainRam || (deref & 3u) != 0)
				return false;
			for (const s32 hop : g.deref_offsets)
			{
				const s64 slot = static_cast<s64>(deref) + hop;
				if (slot < 0 || slot + 4 > static_cast<s64>(Ps2MemSize::MainRam))
					return false;
				if (!reader(static_cast<u32>(slot), 4, &deref) || deref == 0 || deref >= Ps2MemSize::MainRam ||
					(deref & 3u) != 0)
					return false;
			}
			const s64 a = static_cast<s64>(deref) + g.offset;
			if (a < 0 || a + g.width > static_cast<s64>(Ps2MemSize::MainRam))
				return false;
			address = static_cast<u32>(a);
		}
		u32 v = 0;
		if (!reader(address, g.width, &v))
			return false;
		if ((v == g.equals) == g.not_equals)
			return false;
	}
	return true;
}

std::vector<InputManager::VRBindingOverlayEntry> VRInputSource::BuildBindingOverlay(
	const std::vector<VR::ProfileDB::SpatialControlSpec>& specs,
	const std::function<std::string(u32 usb_port)>& usb_device_type)
{
	std::vector<InputManager::VRBindingOverlayEntry> out;

	const Pad::ControllerInfo* ds2 = Pad::GetControllerInfo(Pad::ControllerType::DualShock2);
	const auto is_pad_target = [&](std::string_view name) {
		if (!ds2)
			return false;
		for (const InputBindingInfo& bi : ds2->bindings)
		{
			if (bi.bind_type == InputBindingInfo::Type::Motor || bi.bind_type == InputBindingInfo::Type::Macro)
				continue;
			if (name == bi.name)
				return true;
		}
		return false;
	};

	struct StickAlias
	{
		const char* alias;
		const char* neg_key;
		const char* pos_key;
	};
	static constexpr StickAlias kStickAliases[] = {
		{"LeftStickX", "LLeft", "LRight"},
		{"LeftStickY", "LDown", "LUp"},
		{"RightStickX", "RLeft", "RRight"},
		{"RightStickY", "RDown", "RUp"},
	};

	for (const VR::ProfileDB::SpatialControlSpec& spec : specs)
	{
		const SC::DeviceDef* def = SC::FindDevice(spec.device);
		if (!def)
			continue;
		const std::string src = DeviceName(spec.id) + "/";

		for (const VR::ProfileDB::SpatialControlSpec::Bind& b : spec.bind)
		{
			const SC::ControlDef* c = SC::FindControl(*def, b.control);
			if (!c)
				continue;
			const bool is_axis = SC::ControlTypeOf(c->id) != SC::ControlType::Button;

			if (spec.usb_port < 0)
			{
				const std::string section = Pad::GetConfigSection(spec.pad_port);
				const StickAlias* alias = nullptr;
				for (const StickAlias& a : kStickAliases)
				{
					if (b.target == a.alias)
						alias = &a;
				}
				if (alias)
				{
					if (!is_axis)
					{
						Console.Error("(VR) Spatial controls: VR-%s binds button '%s' to the stick alias '%s'; a button has no negative half — skipped.",
							spec.id.c_str(), b.control.c_str(), b.target.c_str());
						continue;
					}
					out.push_back({section, alias->neg_key, src + "-" + b.control});
					out.push_back({section, alias->pos_key, src + "+" + b.control});
					continue;
				}
				if (!is_pad_target(b.target))
				{
					Console.Error("(VR) Spatial controls: VR-%s binds '%s' to '%s', which is not a DualShock 2 control "
								  "(nor LeftStickX/Y, RightStickX/Y) — skipped, not guessed.",
						spec.id.c_str(), b.control.c_str(), b.target.c_str());
					continue;
				}
				out.push_back({section, b.target, src + (is_axis ? "+" : "") + b.control});
			}
			else
			{
				const std::string type = usb_device_type ? usb_device_type(static_cast<u32>(spec.usb_port)) : std::string();
				if (type.empty() || type == "None")
				{
					Console.Error("(VR) Spatial controls: VR-%s binds '%s' to USB port %d '%s', but no USB device is configured on that port — skipped.",
						spec.id.c_str(), b.control.c_str(), spec.usb_port + 1, b.target.c_str());
					continue;
				}
				out.push_back({USB::GetConfigSection(spec.usb_port), USB::GetConfigSubKey(type, b.target),
					src + (is_axis ? "+" : "") + b.control});
			}
		}
	}
	return out;
}

void VRInputSource::Instance::ResetState()
{
	twin_state.Reset();
	throttle_state.Reset();
	wheel_state.Reset();
	shifter_state.Reset();
	stick_state.Reset();
	at_stop = {};
	break_away_flash_s = {};
}

std::array<int, 2> VRInputSource::Instance::HoldingHands() const
{
	std::array<int, 2> hands{-1, -1};
	switch (kind)
	{
		case SC::DeviceKind::TwinThrottles:
			hands[0] = twin_state.left.axis.hand;
			hands[1] = twin_state.right.axis.hand;
			break;
		case SC::DeviceKind::Throttle:
			hands[0] = throttle_state.lever.axis.hand;
			break;
		case SC::DeviceKind::Wheel:
			hands[0] = wheel_state.rotary.axis.hand;
			hands[1] = wheel_state.rotary.second_hand;
			break;
		case SC::DeviceKind::Shifter:
			hands[0] = shifter_state.sel.hand;
			break;
		case SC::DeviceKind::Stick:
			hands[0] = stick_state.gimbal.hand;
			break;
		case SC::DeviceKind::LightGun:
			hands[0] = gun.aim_hand;
			break;
		default:
			break;
	}
	return hands;
}

std::array<bool, 2> VRInputSource::Instance::BrokeAway() const
{
	switch (kind)
	{
		case SC::DeviceKind::TwinThrottles:
			return {twin_state.left.axis.broke_away, twin_state.right.axis.broke_away};
		case SC::DeviceKind::Throttle:
			return {throttle_state.lever.axis.broke_away || throttle_state.notch.broke_away, false};
		case SC::DeviceKind::Wheel:
			return {wheel_state.rotary.axis.broke_away, wheel_state.rotary.second_broke_away};
		case SC::DeviceKind::Shifter:
			return {shifter_state.sel.broke_away, false};
		case SC::DeviceKind::Stick:
			return {stick_state.gimbal.x.broke_away, false};
		default:
			return {false, false};
	}
}

std::array<float, 2> VRInputSource::Instance::LeverValues() const
{
	const float none = std::numeric_limits<float>::quiet_NaN();
	switch (kind)
	{
		case SC::DeviceKind::TwinThrottles:
			return {twin_state.left.axis.value, twin_state.right.axis.value};
		case SC::DeviceKind::Throttle:
			return {throttle_state.lever.axis.value, none};
		default:
			return {none, none};
	}
}

void VRInputSource::QueueHapticCues(Instance& in, const std::array<int, 2>& hands_before, const std::array<bool, 2>& at_stop_before)
{
	const std::array<int, 2> hands_after = in.HoldingHands();
	const std::array<bool, 2> broke = in.BrokeAway();
	const auto holds = [](const std::array<int, 2>& hs, int hand) { return hs[0] == hand || hs[1] == hand; };
	for (int hand = 0; hand < 2; ++hand)
	{
		const bool was = holds(hands_before, hand), now = holds(hands_after, hand);
		if (now && !was)
		{
			VR::XRInput::QueuePulse(hand, SC::kHapticGrab.amplitude, SC::kHapticGrab.seconds);
		}
		else if (was && !now)
		{
			const bool by_break_away = (hands_before[0] == hand && broke[0]) || (hands_before[1] == hand && broke[1]);
			const SC::HapticCue& cue = by_break_away ? SC::kHapticBreakAway : SC::kHapticRelease;
			VR::XRInput::QueuePulse(hand, cue.amplitude, cue.seconds);
		}
	}
	for (int k = 0; k < 2; ++k)
	{
		if (broke[k])
			in.break_away_flash_s[k] = VR::ControlQuads::kBreakAwayFlashSeconds;
	}
	const std::array<float, 2> levers = in.LeverValues();
	for (int i = 0; i < 2; ++i)
	{
		if (std::isnan(levers[i]))
		{
			in.at_stop[i] = false;
			continue;
		}
		const bool at_stop = std::fabs(levers[i]) >= 1.0f;
		const int holder = (in.kind == SC::DeviceKind::TwinThrottles) ?
							   (i == 0 ? in.twin_state.left.axis.hand : in.twin_state.right.axis.hand) :
							   in.throttle_state.lever.axis.hand;
		if (at_stop && !at_stop_before[i] && holder >= 0)
			VR::XRInput::QueuePulse(holder, SC::kHapticRailHit.amplitude, SC::kHapticRailHit.seconds);
		in.at_stop[i] = at_stop;
	}
}

void VRInputSource::BuildInstance(Instance& in) const
{
	const VR::ProfileDB::SpatialControlSpec& s = in.spec;
	in.def = SC::FindDevice(s.device);
	in.kind = in.def ? in.def->kind : SC::DeviceKind::Count;

	in.placement.side = s.placement.side;
	in.placement.height = s.placement.height;
	in.placement.forward = s.placement.forward;
	in.placement.yaw_deg = s.placement.yaw_deg;

	SC::GrabParams grab;
	grab.grab_radius = s.grab_radius;
	grab.grab_length = s.grab_length;
	grab.grab_mode = s.grab_mode;
	grab.break_away = s.break_away;
	grab.release = s.release;
	grab.spring_rate = s.spring_rate;

	switch (in.kind)
	{
		case SC::DeviceKind::TwinThrottles:
			in.placement.side = 0.0f;
			in.twin.lever.sweep_deg = s.sweep_deg;
			in.twin.lever.arm_length = s.arm_length;
			in.twin.lever.upright = s.upright;
			in.twin.lever.one_way = false;
			in.twin.grab = grab;
			in.twin.lever_spacing = 2.0f * std::fabs(s.placement.side);
			in.twin.power = s.power;
			in.twin.steer_full_lock = s.steer_full_lock;
			in.twin.steer_deadband = s.steer_deadband;
			in.twin.steer_curve = s.steer_curve;
			in.twin.steer_sign = s.steer_sign;
			in.twin.output_floor = s.output_floor;
			in.twin.engage = s.engage;
			in.twin.mode = s.mode;
			in.twin.lever_sign = s.lever_sign;
			break;
		case SC::DeviceKind::Throttle:
			in.throttle.lever.sweep_deg = s.sweep_deg;
			in.throttle.lever.arm_length = s.arm_length;
			in.throttle.lever.upright = s.upright;
			in.throttle.lever.one_way = s.one_way;
			in.throttle.grab = grab;
			in.throttle.detented = s.detented;
			break;
		case SC::DeviceKind::Wheel:
			in.wheel.rim.lock_to_lock_deg = s.lock_to_lock_deg;
			in.wheel.rim.rim_radius = s.rim_radius;
			in.wheel.grab = grab;
			break;
		case SC::DeviceKind::Shifter:
			in.shifter.gate.travel = s.travel;
			in.shifter.grab = grab;
			in.shifter.gears = s.gears;
			in.shifter.has_reverse = s.has_reverse;
			in.shifter.has_neutral = s.has_neutral;
			break;
		case SC::DeviceKind::Stick:
			in.stick.gimbal.travel_x = s.travel_x;
			in.stick.gimbal.travel_y = s.travel_y;
			in.stick.gimbal.twist = s.twist;
			in.stick.gimbal.twist_range_deg = s.twist_range_deg;
			in.stick.grab = grab;
			break;
		case SC::DeviceKind::LightGun:
			in.gun.aim_hand = s.aim_left ? VR::VRInputSnapshot::LEFT : VR::VRInputSnapshot::RIGHT;
			break;
		default:
			break;
	}
	in.stick_bound = {};
	if (s.usb_port < 0 && in.def)
	{
		static constexpr const char* kStickTargets[] = {"LeftStickX", "LeftStickY", "RightStickX", "RightStickY",
			"LLeft", "LRight", "LUp", "LDown", "RLeft", "RRight", "RUp", "RDown"};
		for (const VR::ProfileDB::SpatialControlSpec::Bind& b : s.bind)
		{
			const SC::ControlDef* c = SC::FindControl(*in.def, b.control);
			if (!c || SC::ControlTypeOf(c->id) == SC::ControlType::Button)
				continue;
			for (const char* t : kStickTargets)
			{
				if (b.target == t)
					in.stick_bound[static_cast<u32>(c->id)] = true;
			}
		}
	}

	in.ResetState();
	in.last = {};
	in.ever_emitted = false;
}

VRInputSource::VRInputSource() = default;

VRInputSource::~VRInputSource()
{
	Shutdown();
}

bool VRInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	m_initialized = true;
	m_specs_seen = 0;
	return true;
}

void VRInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

bool VRInputSource::ReloadDevices()
{
	return false;
}

void VRInputSource::Shutdown()
{
	std::lock_guard lock(m_mutex);
	for (Instance& in : m_instances)
	{
		if (in.announced)
			Announce(in, false);
	}
	m_instances.clear();
	VR::ControlQuads::ClearAll();
	m_initialized = false;
	m_session_live = false;
	m_have_last_poll = false;
}

bool VRInputSource::IsInitialized()
{
	return m_initialized;
}

void VRInputSource::SyncSpecs()
{
	u64 generation;
	std::vector<VR::ProfileDB::SpatialControlSpec> specs;
	{
		std::lock_guard lock(s_specs_mutex);
		generation = s_specs_generation;
		if (generation == m_specs_seen)
			return;
		specs = s_specs;
	}
	m_specs_seen = generation;

	const bool had_instances = !m_instances.empty();
	for (Instance& in : m_instances)
	{
		if (in.announced)
			Announce(in, false);
	}
	m_instances.clear();
	VR::ControlQuads::ClearAll();
	for (const VR::ProfileDB::SpatialControlSpec& s : specs)
	{
		const std::optional<u32> index = InternDeviceId(s.id);
		if (!index.has_value())
			continue;
		Instance in;
		in.spec = s;
		in.device_index = index.value();
		BuildInstance(in);
		if (in.kind == SC::DeviceKind::Count)
			continue;
		m_instances.push_back(std::move(in));
	}
	if (had_instances || !m_instances.empty())
		Console.WriteLn("(VR) VRInputSource: %zu virtual control(s) declared by the profile.", m_instances.size());
}

void VRInputSource::Announce(Instance& in, bool present)
{
	const std::string name = DeviceName(in.spec.id);
	if (present && !in.announced)
	{
		in.announced = true;
		Console.WriteLn("(VR) VRInputSource: device %s connected (%s).", name.c_str(), in.spec.device.c_str());
		InputManager::OnInputDeviceConnected(name, fmt::format("VR {} ({})", in.spec.device, in.spec.id));
	}
	else if (!present && in.announced)
	{
		in.announced = false;
		Console.WriteLn("(VR) VRInputSource: device %s disconnected.", name.c_str());
		InputManager::OnInputDeviceDisconnected(MakeControlKey(in.device_index, in.def->controls[0].id), name);
	}
}

void VRInputSource::Emit(Instance& in, const SC::ControlValues& values)
{
	float axis_scale = 1.0f;
	{
		const PadBase* pad = Pad::GetPad(static_cast<u8>(in.spec.pad_port));
		if (pad)
			axis_scale = pad->GetAxisScale();
	}

	for (const SC::ControlDef& c : in.def->controls)
	{
		const u32 i = static_cast<u32>(c.id);
		if (values[i] == in.last[i])
			continue;
		in.last[i] = values[i];
		in.ever_emitted = true;
		s_events_emitted.fetch_add(1, std::memory_order_relaxed);
		const float handed = in.stick_bound[i] ? CompensateAxisScale(values[i], axis_scale) : values[i];
		InputManager::InvokeEvents(MakeControlKey(in.device_index, c.id), handed);
	}
}

void VRInputSource::PollEvents()
{
	std::lock_guard lock(m_mutex);
	SyncSpecs();

	const auto now = std::chrono::steady_clock::now();
	float dt = 1.0f / 60.0f;
	if (m_have_last_poll)
		dt = std::clamp(std::chrono::duration<float>(now - m_last_poll).count(), 0.0f, 0.25f);
	m_last_poll = now;
	m_have_last_poll = true;

	const VR::VRInputSnapshot snap = VR::GetInputSnapshot();
	const bool live = snap.generation != 0;
	bool chord_fired = false;
	if (live)
	{
		chord_fired = SC::StepResetChord(snap.hands[VR::VRInputSnapshot::LEFT].thumbstick_click,
			snap.hands[VR::VRInputSnapshot::RIGHT].thumbstick_click, dt, SC::kResetChordHoldSeconds, m_chord);
		if (chord_fired)
		{
			Console.WriteLn("(VR) VRInputSource: reset chord (both thumbsticks held %.1f s) — %zu control(s) to idle, full recenter requested.",
				SC::kResetChordHoldSeconds, m_instances.size());
			VR::CameraDriver::RequestRecenter();
			VR::XRCompositor::RequestScreenReanchor();
		}
	}
	else
	{
		m_chord = {};
	}

	if (m_instances.empty())
		return;

	if (live != m_session_live)
	{
		m_session_live = live;
		Console.WriteLn("(VR) VRInputSource: VR session %s; virtual devices %s.", live ? "live" : "gone",
			live ? "appear" : "vanish");
	}

	const VR::XRCompositor::ScreenAnchor anchor = VR::XRCompositor::GetScreenAnchor();
	bool recentered = false;
	if (!m_anchor_seen || anchor.generation != m_anchor_generation)
	{
		recentered = m_anchor_seen;
		m_anchor_seen = true;
		m_anchor_generation = anchor.generation;
	}
	SC::Anchor a;
	a.position = {anchor.x, anchor.y, anchor.z};
	a.yaw = anchor.yaw;

	const SC::Hands hands = SC::HandsFromSnapshot(snap);
	const bool split = VR::SplitState::Active();
	const GuestReader reader = s_guest_reader.load(std::memory_order_acquire);

	for (Instance& in : m_instances)
	{
		Announce(in, live);

		for (float& f : in.break_away_flash_s)
			f = std::max(f - dt, 0.0f);

		const bool gate = live && snap.actions_active && !split && EvaluateGuards(in.spec.when, reader);
		if (gate != in.armed || !in.armed_logged)
		{
			in.armed = gate;
			in.armed_logged = true;
			Console.WriteLn("(VR) VRInputSource: %s (%s) %s%s.", DeviceName(in.spec.id).c_str(), in.spec.device.c_str(),
				gate ? "ARMED" : "at rest",
				gate ? "" : (!live ? " — no session" : (!snap.actions_active ? " — session not focused" : (split ? " — split-screen" : " — gate failed"))));
		}

		if (recentered || chord_fired)
			in.ResetState();

		const auto publish_cards = [&](const SC::ControlValues& values, bool active) {
			if (in.kind != SC::DeviceKind::TwinThrottles)
				return;
			const SC::Frame control = SC::PlaceControl(a, in.placement);
			const SC::Quat unyaw = SC::Quat::FromYaw(-a.yaw);
			for (int side = 0; side < 2; ++side)
			{
				const SC::Vec3 pivot = SC::LeverFrame(control, in.twin, side).pivot;
				const SC::Vec3 local = unyaw.Rotate(pivot - a.position);
				VR::ControlQuads::Slot q;
				q.active = active;
				q.t = values[static_cast<u32>(side == 0 ? SC::ControlId::LeftLever : SC::ControlId::RightLever)];
				q.grabbed = values[static_cast<u32>(side == 0 ? SC::ControlId::GrabbedLeft : SC::ControlId::GrabbedRight)] > 0.5f;
				q.broke_away = active && in.break_away_flash_s[side] > 0.0f;
				q.side = local.x;
				q.height = local.y;
				q.forward = -local.z;
				q.yaw_deg = 0.0f;
				VR::ControlQuads::Publish(side == 0 ? VR::ControlQuads::kLeft : VR::ControlQuads::kRight, q);
			}
		};

		if (!gate)
		{
			in.ResetState();
			if (in.ever_emitted)
				Emit(in, SC::ControlValues{});
			publish_cards(SC::ControlValues{}, false);
			continue;
		}

		const SC::Frame frame = SC::PlaceControl(a, in.placement);
		const std::array<int, 2> hands_before = in.HoldingHands();
		const std::array<bool, 2> at_stop_before = in.at_stop;
		SC::ControlValues values{};
		switch (in.kind)
		{
			case SC::DeviceKind::TwinThrottles:
				SC::StepTwinThrottles(frame, in.twin, hands, dt, in.twin_state);
				values = SC::ComposeTwinThrottles(in.twin, in.twin_state);
				break;
			case SC::DeviceKind::Throttle:
				SC::StepThrottle(frame, in.throttle, hands, dt, in.throttle_state);
				values = SC::ComposeThrottle(in.throttle, in.throttle_state);
				break;
			case SC::DeviceKind::Wheel:
				SC::StepWheel(frame, in.wheel, hands, dt, in.wheel_state);
				values = SC::ComposeWheel(in.wheel, in.wheel_state);
				break;
			case SC::DeviceKind::Shifter:
				SC::StepShifter(frame, in.shifter, hands, dt, in.shifter_state);
				values = SC::ComposeShifter(in.shifter, in.shifter_state);
				break;
			case SC::DeviceKind::Stick:
				SC::StepStick(frame, in.stick, hands, dt, in.stick_state);
				values = SC::ComposeStick(in.stick, in.stick_state, hands);
				break;
			case SC::DeviceKind::LightGun:
				values = SC::ComposeLightGun(in.gun, hands);
				break;
			default:
				break;
		}
		QueueHapticCues(in, hands_before, at_stop_before);
		Emit(in, values);
		publish_cards(values, true);
	}
}

std::vector<std::pair<std::string, std::string>> VRInputSource::EnumerateDevices()
{
	std::lock_guard lock(m_mutex);
	std::vector<std::pair<std::string, std::string>> ret;
	for (const Instance& in : m_instances)
	{
		if (!in.announced)
			continue;
		ret.emplace_back(DeviceName(in.spec.id), fmt::format("VR {} ({})", in.spec.device, in.spec.id));
	}
	return ret;
}

std::vector<InputBindingKey> VRInputSource::EnumerateMotors()
{
	std::lock_guard lock(m_mutex);
	std::vector<InputBindingKey> ret;
	for (const Instance& in : m_instances)
	{
		if (in.announced)
			ret.push_back(MakeMotorKey(in.device_index));
	}
	return ret;
}

bool VRInputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	return false;
}

InputLayout VRInputSource::GetControllerLayout(u32 index)
{
	return InputLayout::Unknown;
}

void VRInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
	if (key.source_type != InputSourceType::VR || key.source_subtype != InputSubclass::ControllerMotor)
		return;
	std::lock_guard lock(m_mutex);
	for (Instance& in : m_instances)
	{
		if (in.device_index != key.source_index)
			continue;
		const std::array<int, 2> holding = in.HoldingHands();
		for (int hand = 0; hand < 2; ++hand)
		{
			const bool on = (holding[0] == hand || holding[1] == hand) && intensity > 0.0f;
			if (on)
			{
				VR::XRInput::QueueHaptic(hand, intensity);
				in.buzzing[hand] = true;
			}
			else if (in.buzzing[hand])
			{
				VR::XRInput::QueueHaptic(hand, 0.0f);
				in.buzzing[hand] = false;
			}
		}
	}
}

std::optional<InputBindingKey> VRInputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
	if (!device.starts_with("VR-") || binding.empty())
		return std::nullopt;
	const std::string_view id = device.substr(3);
	const std::optional<u32> index = InternDeviceId(id);
	if (!index.has_value())
		return std::nullopt;

	if (binding == kMotorName)
		return MakeMotorKey(index.value());

	std::string_view name = binding;
	InputModifier modifier = InputModifier::FullAxis;
	bool prefixed = false;
	if (name[0] == '+')
	{
		modifier = InputModifier::None;
		name = name.substr(1);
		prefixed = true;
	}
	else if (name[0] == '-')
	{
		modifier = InputModifier::Negate;
		name = name.substr(1);
		prefixed = true;
	}
	else if (name.starts_with("Full") && SC::ParseControlId(name.substr(4)).has_value())
	{
		name = name.substr(4);
	}

	const std::optional<SC::ControlId> id_opt = SC::ParseControlId(name);
	if (!id_opt.has_value())
		return std::nullopt;

	InputBindingKey key = MakeControlKey(index.value(), id_opt.value());
	if (key.source_subtype == InputSubclass::ControllerButton)
	{
		if (prefixed)
			return std::nullopt;
		return key;
	}
	key.modifier = modifier;
	return key;
}

TinyString VRInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	TinyString ret;
	if (key.source_type != InputSourceType::VR)
		return ret;

	const std::string id = DeviceIdForIndex(key.source_index);
	const char* sep = display ? " " : "/";
	if (key.source_subtype == InputSubclass::ControllerMotor)
	{
		ret.format("VR-{}{}{}", id, sep, kMotorName);
	}
	else if (key.source_subtype == InputSubclass::ControllerAxis)
	{
		const char* name = SC::ControlName(static_cast<SC::ControlId>(key.data));
		if (display && key.modifier == InputModifier::FullAxis)
			ret.format("VR-{} Full {}", id, name);
		else
			ret.format("VR-{}{}{}{}", id, sep, ModifierPrefix(key.modifier), name);
	}
	else if (key.source_subtype == InputSubclass::ControllerButton)
	{
		ret.format("VR-{}{}{}", id, sep, SC::ControlName(static_cast<SC::ControlId>(key.data)));
	}
	return ret;
}

TinyString VRInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	return TinyString();
}
