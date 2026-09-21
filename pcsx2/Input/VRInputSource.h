// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "Input/InputSource.h"

#include "VR/SpatialControls.h"
#include "VR/VRProfileDB.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class VRInputSource final : public InputSource
{
public:
	VRInputSource();
	~VRInputSource() override;

	bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	bool ReloadDevices() override;
	void Shutdown() override;
	bool IsInitialized() override;

	void PollEvents() override;
	std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
	std::vector<InputBindingKey> EnumerateMotors() override;
	bool GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping) override;
	InputLayout GetControllerLayout(u32 index) override;
	using InputSource::UpdateMotorState;
	void UpdateMotorState(InputBindingKey key, float intensity) override;

	std::optional<InputBindingKey> ParseKeyString(const std::string_view device, const std::string_view binding) override;
	TinyString ConvertKeyToString(InputBindingKey key, bool display = false, bool migration = false) override;
	TinyString ConvertKeyToIcon(InputBindingKey key) override;

	static void SetActiveControls(std::vector<VR::ProfileDB::SpatialControlSpec> specs);
	static std::vector<VR::ProfileDB::SpatialControlSpec> GetActiveControls();

	static std::vector<InputManager::VRBindingOverlayEntry> BuildBindingOverlay(
		const std::vector<VR::ProfileDB::SpatialControlSpec>& specs,
		const std::function<std::string(u32 usb_port)>& usb_device_type);

	using GuestReader = bool (*)(u32 address, u8 width, u32* value);
	static void SetGuestReader(GuestReader reader);
	static bool EvaluateGuards(const std::vector<VR::ProfileDB::CameraGuard>& guards, GuestReader reader);

	static std::optional<u32> FindDeviceIndex(std::string_view id);
	static std::optional<u32> InternDeviceId(std::string_view id);
	static std::string DeviceIdForIndex(u32 index);

	static std::string DeviceName(std::string_view id);
	static InputBindingKey MakeControlKey(u32 device_index, VR::SpatialControls::ControlId id);
	static InputBindingKey MakeMotorKey(u32 device_index);

	static u64 EventsEmittedForTest();

	static float CompensateAxisScale(float value, float axis_scale);

private:
	struct Instance
	{
		VR::ProfileDB::SpatialControlSpec spec;
		VR::SpatialControls::DeviceKind kind = VR::SpatialControls::DeviceKind::Count;
		const VR::SpatialControls::DeviceDef* def = nullptr;
		u32 device_index = 0;
		VR::SpatialControls::Placement placement;

		VR::SpatialControls::TwinThrottlesParams twin;
		VR::SpatialControls::ThrottleParams throttle;
		VR::SpatialControls::WheelParams wheel;
		VR::SpatialControls::ShifterParams shifter;
		VR::SpatialControls::StickParams stick;
		VR::SpatialControls::LightGunParams gun;

		VR::SpatialControls::TwinThrottlesState twin_state;
		VR::SpatialControls::ThrottleState throttle_state;
		VR::SpatialControls::WheelState wheel_state;
		VR::SpatialControls::ShifterState shifter_state;
		VR::SpatialControls::StickState stick_state;

		VR::SpatialControls::ControlValues last{};
		bool ever_emitted = false;
		bool announced = false;
		bool armed = false;
		bool armed_logged = false;
		std::array<bool, 2> buzzing{};
		std::array<bool, 2> at_stop{};
		std::array<float, 2> break_away_flash_s{};
		std::array<bool, VR::SpatialControls::kControlCount> stick_bound{};

		void ResetState();
		std::array<int, 2> HoldingHands() const;
		std::array<bool, 2> BrokeAway() const;
		std::array<float, 2> LeverValues() const;
	};

	void SyncSpecs();
	void BuildInstance(Instance& in) const;
	void Emit(Instance& in, const VR::SpatialControls::ControlValues& values);
	void Announce(Instance& in, bool present);
	void QueueHapticCues(Instance& in, const std::array<int, 2>& hands_before, const std::array<bool, 2>& at_stop_before);

	std::mutex m_mutex;
	std::vector<Instance> m_instances;
	u64 m_specs_seen = 0;
	bool m_initialized = false;
	bool m_session_live = false;
	u32 m_anchor_generation = 0;
	bool m_anchor_seen = false;
	std::chrono::steady_clock::time_point m_last_poll{};
	bool m_have_last_poll = false;
	VR::SpatialControls::ChordState m_chord;
};
