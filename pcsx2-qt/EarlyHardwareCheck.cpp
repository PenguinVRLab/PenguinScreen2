// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#if defined(_WIN32) && defined(_MSC_VER)

#include "pcsx2/VMManager.h"

#include "common/RedtapeWindows.h"

#pragma optimize("", off)

struct EarlyHardwareCheckObject
{
	EarlyHardwareCheckObject()
	{
		const char* error;
		if (VMManager::PerformEarlyHardwareChecks(&error))
			return;

		const int error_len = static_cast<int>(std::strlen(error));
		int wlen = MultiByteToWideChar(CP_UTF8, 0, error, error_len, nullptr, 0);
		if (wlen > 0)
		{
			wchar_t* werror = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, sizeof(wchar_t) * (error_len + 1)));
			if (werror && (wlen = MultiByteToWideChar(CP_UTF8, 0, error, error_len, werror, wlen)) > 0)
			{
				werror[wlen] = 0;
				MessageBoxW(NULL, werror, L"Hardware Check Failed", MB_ICONERROR);
				HeapFree(GetProcessHeap(), 0, werror);
			}
		}

		TerminateProcess(GetCurrentProcess(), 0xFFFFFFFF);
	}
};
#pragma warning(disable : 4075)
#pragma init_seg(".CRT$XCT")
EarlyHardwareCheckObject s_hardware_checker;

#pragma optimize("", on)

#endif
