// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include <vector>
#include "common/Pcsx2Types.h"

class DebugInterface;

namespace MipsStackWalk {
	struct StackFrame {
		u32 entry;
		u32 pc;
		u32 sp;
		int stackSize;
	};

	std::vector<StackFrame> Walk(DebugInterface* cpu, u32 pc, u32 ra, u32 sp, u32 threadEntry);
};
