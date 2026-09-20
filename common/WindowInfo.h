// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "Pcsx2Defs.h"

#include <optional>

struct WindowInfo
{
	enum class Type
	{
		Surfaceless,
		Win32,
		X11,
		Wayland,
		MacOS
	};

	Type type = Type::Surfaceless;

	void* display_connection = nullptr;

	void* window_handle = nullptr;

	void* surface_handle = nullptr;

	u32 surface_width = 0;

	u32 surface_height = 0;

	float surface_scale = 1.0f;

	float surface_refresh_rate = 0.0f;

	static std::optional<float> QueryRefreshRateForWindow(const WindowInfo& wi);
};
