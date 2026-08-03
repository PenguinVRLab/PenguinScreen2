// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/Pcsx2Defs.h"
#include "common/SmallString.h"
#include "Input/InputManager.h"

class SettingsInterface;

class InputSource
{
public:
	InputSource();
	virtual ~InputSource();

	virtual bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) = 0;
	virtual void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) = 0;
	virtual bool ReloadDevices() = 0;
	virtual void Shutdown() = 0;
	virtual bool IsInitialized() = 0;

	virtual void PollEvents() = 0;

	virtual std::optional<InputBindingKey> ParseKeyString(const std::string_view device, const std::string_view binding) = 0;
	virtual TinyString ConvertKeyToString(InputBindingKey key, bool display = false, bool migration = false) = 0;
	virtual TinyString ConvertKeyToIcon(InputBindingKey key) = 0;

	virtual std::vector<std::pair<std::string, std::string>> EnumerateDevices() = 0;

	virtual std::vector<InputBindingKey> EnumerateMotors() = 0;

	virtual bool GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping) = 0;

	virtual InputLayout GetControllerLayout(u32 index) = 0;

	virtual void UpdateMotorState(InputBindingKey key, float intensity) = 0;

	virtual void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity);

	static InputBindingKey MakeGenericControllerAxisKey(InputSourceType clazz, u32 controller_index, s32 axis_index);

	static InputBindingKey MakeGenericControllerButtonKey(InputSourceType clazz, u32 controller_index, s32 button_index);

	static InputBindingKey MakeGenericControllerHatKey(
		InputSourceType clazz, u32 controller_index, s32 hat_index, u8 hat_direction, u32 num_directions);

	static InputBindingKey MakeGenericControllerMotorKey(InputSourceType clazz, u32 controller_index, s32 motor_index);

	static std::optional<InputBindingKey> ParseGenericControllerKey(
		InputSourceType clazz, const std::string_view source, const std::string_view sub_binding);

	static std::string ConvertGenericControllerKeyToString(InputBindingKey key);

	static bool ShouldIgnoreInversion();
};
