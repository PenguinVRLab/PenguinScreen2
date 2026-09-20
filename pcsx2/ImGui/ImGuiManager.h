// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <span>
#include <string>
#include <vector>

struct ImFont;

union InputBindingKey;
enum class GenericInputBinding : u8;
enum class InputLayout : u8;

namespace ImGuiManager
{
	struct FontInfo
	{
		std::span<const u8> data;
		std::span<const u32> exclude_ranges;
		const char* face_name;
		bool is_emoji_font;
	};

	void SetFonts(std::vector<FontInfo> info);

	bool Initialize();

	bool InitializeFullscreenUI();

	void Shutdown(bool clear_state);

	float GetWindowWidth();
	float GetWindowHeight();

	void WindowResized();

	void RequestScaleUpdate();

	void ReloadFonts();

	void NewFrame();

	void SkipFrame();

	void RenderOSD();

	float GetGlobalScale();

	ImFont* GetStandardFont();

	ImFont* GetFixedFont();

	ImFont* GetOSDFont();

	float GetFontSizeStandard();

	float GetFontSizeMedium();

	float GetFontSizeLarge();

	bool WantsTextInput();

	bool WantsMouseInput();

	void AddTextInput(std::string str);

	void UpdateMousePosition(float x, float y);

	bool ProcessPointerButtonEvent(InputBindingKey key, float value);

	bool ProcessPointerAxisEvent(InputBindingKey key, float value);

	bool ProcessHostKeyEvent(InputBindingKey key, float value);

	bool ProcessGenericInputEvent(GenericInputBinding key, InputLayout layout, float value, u32 controller_id = 0);

	void ProcessGenericAxisEvent(GenericInputBinding negative_key, GenericInputBinding positive_key, InputLayout layout, float value, u32 controller_id = 0);

	void SwapGamepadNorthWest(bool value);

	bool IsGamepadNorthWestSwapped();

	void SetSoftwareCursor(u32 index, std::string image_path, float image_scale, u32 multiply_color = 0xFFFFFF);
	bool HasSoftwareCursor(u32 index);
	void ClearSoftwareCursor(u32 index);

	void SetSoftwareCursorPosition(u32 index, float pos_x, float pos_y);

	std::string StripIconCharacters(std::string_view str);
}

namespace Host
{
	void BeginTextInput();

	void EndTextInput();
}
