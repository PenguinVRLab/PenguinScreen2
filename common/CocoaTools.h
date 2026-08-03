// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef __APPLE__

#include <string>
#include <optional>

struct WindowInfo;

namespace CocoaTools
{
	bool CreateMetalLayer(WindowInfo* wi);
	void DestroyMetalLayer(WindowInfo* wi);
	std::optional<float> GetViewRefreshRate(const WindowInfo& wi);

	void MarkHelpMenu(void* menu);
	std::optional<std::string> GetBundlePath();
	std::optional<std::string> GetNonTranslocatedBundlePath();
	std::optional<std::string> MoveToTrash(std::string_view file);
	bool DelayedLaunch(std::string_view file);
	bool ShowInFinder(std::string_view file);
	std::optional<std::string> GetResourcePath();

	void* CreateWindow(std::string_view title, uint32_t width, uint32_t height);
	void DestroyWindow(void* window);
	void GetWindowInfoFromWindow(WindowInfo* wi, void* window);
	void RunCocoaEventLoop(bool wait_forever = false);
	void StopMainThreadEventLoop();
}

#endif
