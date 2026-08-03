// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef _WIN32
#include "common/RedtapeWindows.h"

extern "C" {
	_declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}

extern "C" {
	_declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#endif
