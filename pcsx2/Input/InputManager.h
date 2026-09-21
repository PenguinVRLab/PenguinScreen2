// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <utility>
#include <vector>

#include "common/Pcsx2Types.h"
#include "common/SettingsInterface.h"
#include "common/WindowInfo.h"

#include "pcsx2/Config.h"

class SmallStringBase;

enum class InputSourceType : u32
{
	Keyboard,
	Pointer,
	SDL,
#ifdef _WIN32
	DInput,
	XInput,
#endif
#ifdef ENABLE_VR
	VR,
#endif
	Count,
};

enum class InputSubclass : u32
{
	None = 0,

	PointerButton = 0,
	PointerAxis = 1,

	ControllerButton = 0,
	ControllerAxis = 1,
	ControllerHat = 2,
	ControllerMotor = 3,
	ControllerHaptic = 4,
};

enum class InputLayout : u8
{
	Unknown,
	Xbox,
	Playstation,
	Nintendo
};

enum class InputModifier : u32
{
	None = 0,
	Negate,
	FullAxis,
};

union InputBindingKey
{
	struct
	{
		InputSourceType source_type : 4;
		u32 source_index : 8;
		InputSubclass source_subtype : 3;
		InputModifier modifier : 2;
		u32 invert : 1;
		u32 needs_migration : 1;
		u32 unused : 13;
		u32 data;
	};

	u64 bits;

	bool operator==(const InputBindingKey& k) const { return bits == k.bits; }
	bool operator!=(const InputBindingKey& k) const { return bits != k.bits; }

	InputBindingKey MaskDirection() const
	{
		InputBindingKey r;
		r.bits = bits;
		r.modifier = InputModifier::None;
		r.invert = 0;
		r.needs_migration = false;
		return r;
	}
};
static_assert(sizeof(InputBindingKey) == sizeof(u64), "Input binding key is 64 bits");

struct InputBindingKeyHash
{
	std::size_t operator()(const InputBindingKey& k) const { return std::hash<u64>{}(k.bits); }
};

using InputButtonEventHandler = std::function<void(s32 value)>;

using InputAxisEventHandler = std::function<void(InputBindingKey key, float value)>;

struct InputInterceptHook
{
	enum class CallbackResult
	{
		StopProcessingEvent,
		ContinueProcessingEvent,
		RemoveHookAndStopProcessingEvent,
		RemoveHookAndContinueProcessingEvent,
	};

	using Callback = std::function<CallbackResult(InputBindingKey key, float value)>;
};

struct HotkeyInfo
{
	const char* name;
	const char* category;
	const char* display_name;
	void (*handler)(s32 pressed);
};
#define DECLARE_HOTKEY_LIST(name) extern const HotkeyInfo name[]
#define BEGIN_HOTKEY_LIST(name) const HotkeyInfo name[] = {
#define DEFINE_HOTKEY(name, category, display_name, handler) {(name), (category), (display_name), (handler)},
#define END_HOTKEY_LIST() \
	{ \
		nullptr, nullptr, nullptr, nullptr \
	} \
	} \
	;

DECLARE_HOTKEY_LIST(g_common_hotkeys);
DECLARE_HOTKEY_LIST(g_gs_hotkeys);
DECLARE_HOTKEY_LIST(g_host_hotkeys);

enum class InputPointerAxis : u8
{
	X,
	Y,
	WheelX,
	WheelY,
	Count
};

class InputSource;

namespace InputManager
{
	static constexpr double VIBRATION_UPDATE_INTERVAL_SECONDS = 0.5;

	static constexpr u32 MAX_POINTER_DEVICES = 1;
	static constexpr u32 MAX_POINTER_BUTTONS = 3;

	static constexpr u32 MAX_SOFTWARE_CURSORS = MAX_POINTER_BUTTONS + 2;

	InputSource* GetInputSourceInterface(InputSourceType type);

	const char* InputSourceToString(InputSourceType clazz);

	bool GetInputSourceDefaultEnabled(InputSourceType type);

	std::optional<InputSourceType> ParseInputSourceString(const std::string_view str);

	std::optional<u32> GetIndexFromPointerBinding(const std::string_view str);

	std::string GetPointerDeviceName(u32 pointer_index);

	std::optional<u32> ConvertHostKeyboardStringToCode(const std::string_view str);

	std::optional<std::string> ConvertHostKeyboardCodeToString(u32 code);

	const char* ConvertHostKeyboardCodeToIcon(u32 code);

	InputBindingKey MakeHostKeyboardKey(u32 key_code);

	InputBindingKey MakePointerButtonKey(u32 index, u32 button_index);

	InputBindingKey MakePointerAxisKey(u32 index, InputPointerAxis axis);

	std::optional<InputBindingKey> ParseInputBindingKey(const std::string_view binding);

	std::string ConvertInputBindingKeyToString(InputBindingInfo::Type binding_type, InputBindingKey key, bool migration = false);

	std::string ConvertInputBindingKeysToString(InputBindingInfo::Type binding_type, const InputBindingKey* keys, size_t num_keys, bool migration = false);

	bool PrettifyInputBinding(SmallStringBase& binding, bool use_icons = true);

	void SetGamepadIconPreference(InputLayout layout);
	InputLayout GetGamepadIconPreference();

	std::vector<std::string_view> SplitChord(const std::string_view binding);

	std::vector<const HotkeyInfo*> GetHotkeyList();

	std::vector<std::pair<std::string, std::string>> EnumerateDevices();

	std::vector<InputBindingKey> EnumerateMotors();

	using GenericInputBindingMapping = std::vector<std::pair<GenericInputBinding, std::string>>;
	GenericInputBindingMapping GetGenericBindingMapping(const std::string_view device);

	bool IsInputSourceEnabled(SettingsInterface& si, InputSourceType type);

	void ReloadBindings(SettingsInterface& si, SettingsInterface& binding_si, SettingsInterface& hotkey_binding_si, bool is_binding_profile, bool is_hotkey_profile);

	void ReloadSources(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock);

	bool ReloadDevices();

	void CloseSources();

	void PollSources();

	bool HasAnyBindingsForKey(InputBindingKey key);

	bool HasAnyBindingsForSource(InputBindingKey key);

	bool InvokeEvents(InputBindingKey key, float value, GenericInputBinding generic_key = GenericInputBinding::Unknown,
		GenericInputBinding axis_neg_key = GenericInputBinding::Unknown,
		GenericInputBinding axis_pos_key = GenericInputBinding::Unknown);

	void ClearBindStateFromSource(InputBindingKey key);

	void SetHook(InputInterceptHook::Callback callback);

	void RemoveHook();

	bool HasHook();

	void SetUSBVibrationIntensity(u32 port, float large_or_single_motor_intensity, float small_motor_intensity);
	void SetPadVibrationIntensity(u32 pad_index, float large_or_single_motor_intensity, float small_motor_intensity);

	void PauseVibration();

	std::pair<float, float> GetPointerAbsolutePosition(u32 index);

	void UpdatePointerAbsolutePosition(u32 index, float x, float y);

	void UpdatePointerRelativeDelta(u32 index, InputPointerAxis axis, float d, bool raw_input = false);

	void UpdateHostMouseMode();

	void OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name);

	void OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier);

#ifdef ENABLE_VR
	struct VRBindingOverlayEntry
	{
		std::string section;
		std::string key;
		std::string binding;
	};
	bool SetVRBindingOverlay(std::vector<VRBindingOverlayEntry> entries);
	std::vector<std::string> GetVRBindingOverlay(const std::string_view section, const std::string_view key);
	bool HasVRBindingOverlay();
#endif
}

namespace Host
{
	std::optional<WindowInfo> GetTopLevelWindowInfo();

	void OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name);

	void OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier);

	void SetMouseMode(bool relative_mode, bool hide_cursor);

	void SetMouseLock(bool state);
}
