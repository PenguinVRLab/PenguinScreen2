// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <common/Pcsx2Types.h>

#include <QtCore/qttranslation.h>

namespace DebuggerEvents
{
	struct Event
	{
		virtual ~Event() = default;
	};

	struct Refresh : Event
	{
	};

	struct GoToAddress : Event
	{
		enum Filter
		{
			NONE,
			DISASSEMBLER,
			MEMORY_VIEW
		};

		u32 address = 0;

		Filter filter = NONE;

		bool switch_to_tab = true;

		static constexpr const char* ACTION_STRING = QT_TRANSLATE_NOOP("DebuggerEvents", "Go to in %1");
		static constexpr const char* ACTION_OVERFLOW_STRING = QT_TRANSLATE_NOOP("DebuggerEvents", "Go to in...");
	};

	struct VMUpdate : Event
	{
	};

	struct AddToSavedAddresses : Event
	{
		u32 address = 0;
		bool switch_to_tab = true;

		static constexpr const char* ACTION_STRING = QT_TRANSLATE_NOOP("DebuggerEvents", "Add to %1");
		static constexpr const char* ACTION_OVERFLOW_STRING = QT_TRANSLATE_NOOP("DebuggerEvents", "Add to...");
	};
}
