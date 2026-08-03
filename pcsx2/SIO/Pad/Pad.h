// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"
#include "Input/InputManager.h"
#include "SIO/Pad/PadTypes.h"

#include <memory>

class PadBase;

enum class GenericInputBinding : u8;
class SettingsInterface;
class StateWrapper;

namespace Pad
{
	constexpr size_t DEFAULT_EJECT_TICKS = 50;

	bool Initialize();
	void Shutdown();

	Pad::ControllerType GetDefaultPadType(u32 pad);

	void LoadConfig(const SettingsInterface& si);

	void SetDefaultControllerConfig(SettingsInterface& si);
	void SetDefaultHotkeyConfig(SettingsInterface& si);

	void ClearPortBindings(SettingsInterface& si, u32 port);

	void CopyConfiguration(SettingsInterface* dest_si, const SettingsInterface& src_si, bool copy_pad_config = true,
		bool copy_pad_bindings = true, bool copy_hotkey_bindings = true);

	const std::vector<std::pair<const char*, const char*>> GetControllerTypeNames();

	const ControllerInfo* GetControllerInfo(Pad::ControllerType type);
	const ControllerInfo* GetControllerInfoByName(const std::string_view name);

	const ControllerInfo* GetConfigControllerType(const SettingsInterface& si, const char* section, u32 port);

	bool MapController(
		SettingsInterface& si, u32 controller, const std::vector<std::pair<GenericInputBinding, std::string>>& mapping);

	std::vector<std::string> GetInputProfileNames();
	std::string GetConfigSection(u32 pad_index);

	bool HasConnectedPad(u8 unifiedSlot);

	PadBase* GetPad(u8 port, u8 slot);
	PadBase* GetPad(const u8 unifiedSlot);

	void SetControllerState(u32 controller, u32 bind, float value);

	bool Freeze(StateWrapper& sw);

	void SetMacroButtonState(InputBindingKey& key, u32 pad, u32 index, bool state);
	void UpdateMacroButtons();
};
