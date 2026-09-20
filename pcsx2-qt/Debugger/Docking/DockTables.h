// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "DockUtils.h"

#include "DebugTools/DebugInterface.h"

#include <kddockwidgets/KDDockWidgets.h>

class MD5Digest;

class DebuggerView;
struct DebuggerViewParameters;

namespace DockTables
{
	struct DebuggerViewDescription
	{
		DebuggerView* (*create_widget)(const DebuggerViewParameters& parameters);

		const char* display_name;

		DockUtils::PreferredLocation preferred_location;
	};

	extern const std::map<std::string, DebuggerViewDescription> DEBUGGER_VIEWS;

	using DockGroup = s32;

	namespace DefaultDockGroup
	{
		enum
		{
			ROOT = -1,
			TOP_RIGHT = 0,
			BOTTOM = 1,
			TOP_LEFT = 2
		};
	}

	struct DefaultDockGroupDescription
	{
		KDDockWidgets::Location location;
		DockGroup parent;
	};

	extern const std::vector<DefaultDockGroupDescription> DEFAULT_DOCK_GROUPS;

	struct DefaultDockWidgetDescription
	{
		std::string type;
		DockGroup group;
	};

	struct DefaultDockLayout
	{
		std::string name;
		BreakPointCpu cpu;
		std::vector<DefaultDockGroupDescription> groups;
		std::vector<DefaultDockWidgetDescription> widgets;
		std::set<std::string> toolbars;
	};

	extern const std::vector<DefaultDockLayout> DEFAULT_DOCK_LAYOUTS;

	const DefaultDockLayout* defaultLayout(const std::string& name);

	u32 hashDefaultLayouts();
}
