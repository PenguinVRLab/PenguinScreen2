// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VR/VRRelaunch.h"

#include <algorithm>
#include <mutex>
#include <iterator>
#include <string_view>

namespace VR::Relaunch
{
	namespace
	{
		constexpr std::string_view kValueOptions[] = {
			"--vr-seat", "-vr-seat", "--vr-cast", "-vr-cast", "--vr-seat-dirs", "-vr-seat-dirs", "--vr-seat-jsons",
			"-vr-seat-jsons", "-datapath", "-state", "-statefile", "-elf", "-gameargs", "-disc", "-logfile",
		};
		constexpr std::string_view kDropped[] = {"-updatecleanup", "-setupwizard", "-testconfig", "-help", "-version", "-vr-info"};

		template <size_t N>
		bool In(const std::string_view (&a)[N], std::string_view s)
		{
			return std::find(std::begin(a), std::end(a), s) != std::end(a);
		}
	}

	std::vector<std::string> BuildArgs(const std::vector<std::string>& original, const Boot& boot)
	{
		std::vector<std::string> out;
		std::vector<std::string> target_options;
		std::vector<std::string> files;
		std::optional<bool> fast_boot;
		bool no_more = false;
		for (size_t i = 0; i < original.size(); i++)
		{
			const std::string& a = original[i];
			if (no_more)
			{
				files.push_back(a);
				continue;
			}
			if (a == "--")
			{
				no_more = true;
				continue;
			}
			if (In(kValueOptions, a) && i + 1 < original.size())
			{
				const std::string& v = original[++i];
				if (a == "-state" || a == "-statefile")
					continue;
				if (a == "-elf" || a == "-disc")
				{
					target_options.push_back(a);
					target_options.push_back(v);
					continue;
				}
				out.push_back(a);
				out.push_back(v);
				continue;
			}
			if (In(kDropped, a))
				continue;
			if (a == "-fastboot" || a == "-slowboot")
			{
				fast_boot = (a == "-fastboot");
				continue;
			}
			if (a == "-bios")
			{
				target_options.push_back(a);
				continue;
			}
			if (!a.empty() && a[0] == '-')
			{
				out.push_back(a);
				continue;
			}
			files.push_back(a);
		}

		const auto choice = [&out](std::optional<bool> fast) {
			if (fast.has_value())
				out.push_back(fast.value() ? "-fastboot" : "-slowboot");
		};
		if (boot.source == Boot::Source::None)
			return out;
		const bool known = boot.source != Boot::Source::Unknown &&
						   (!boot.elf_override.empty() || boot.source == Boot::Source::Disc || boot.source == Boot::Source::NoDisc ||
							   !boot.path.empty());
		if (!known)
		{
			choice(fast_boot);
			out.insert(out.end(), target_options.begin(), target_options.end());
			if (!files.empty())
			{
				out.push_back("--");
				out.insert(out.end(), files.begin(), files.end());
			}
			return out;
		}
		choice(boot.fast_boot);
		if (!boot.elf_override.empty())
		{
			out.push_back("-elf");
			out.push_back(boot.elf_override);
		}
		switch (boot.source)
		{
			case Boot::Source::Disc:
				out.push_back("-disc");
				out.push_back(boot.path);
				break;
			case Boot::Source::NoDisc:
				out.push_back("-bios");
				break;
			case Boot::Source::File:
			default:
				if (!boot.path.empty())
				{
					out.push_back("--");
					out.push_back(boot.path);
				}
				break;
		}
		return out;
	}

	std::string ChooseProgram(const std::string& self_exe, const char* appimage_env, const char* appdir_env)
	{
		if (!appimage_env || !*appimage_env || !appdir_env || !*appdir_env)
			return self_exe;
		std::string dir(appdir_env);
		while (dir.size() > 1 && dir.back() == '/')
			dir.pop_back();
		const bool inside = self_exe.size() > dir.size() && self_exe.compare(0, dir.size(), dir) == 0 && self_exe[dir.size()] == '/';
		return inside ? std::string(appimage_env) : self_exe;
	}

	std::string CouldNotCloseText(std::string_view why)
	{
		return "The emulator could not close: " + std::string(why) + ". Your switches are saved; restart the emulator to apply them.";
	}

	std::string Pending::Settle(bool shutdown_proceeds, std::vector<std::string> args, std::string_view why)
	{
		if (shutdown_proceeds)
		{
			m_args = std::move(args);
			return {};
		}
		m_args.reset();
		return CouldNotCloseText(why);
	}
	bool RestartAfterGameClosed(bool armed, bool app_exits)
	{
		return armed && !app_exits;
	}

	namespace
	{
		std::mutex s_close_mutex;
		std::string s_close_pending;
		bool s_close_armed = false;
	}

	void PendRestartAfterGameCloses(std::string game)
	{
		std::lock_guard lock(s_close_mutex);
		s_close_pending = std::move(game);
	}

	void CancelRestartAfterGameCloses()
	{
		std::lock_guard lock(s_close_mutex);
		s_close_pending.clear();
	}

	void GameShutdownRequested(std::string_view game)
	{
		std::lock_guard lock(s_close_mutex);
		if (!s_close_pending.empty() && s_close_pending == game)
			s_close_armed = true;
		s_close_pending.clear();
	}

	bool TakeRestartAfterGameCloses()
	{
		std::lock_guard lock(s_close_mutex);
		const bool armed = s_close_armed;
		s_close_armed = false;
		s_close_pending.clear();
		return armed;
	}
}
