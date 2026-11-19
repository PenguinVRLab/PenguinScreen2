// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VR::Relaunch
{
	struct Boot
	{
		enum class Source
		{
			Unknown,
			File,
			Disc,
			NoDisc,
			None,
		};
		Source source = Source::Unknown;
		std::string path;
		std::string elf_override;
		std::optional<bool> fast_boot;
	};

	bool RestartAfterGameClosed(bool armed, bool app_exits);
	void PendRestartAfterGameCloses(std::string game);
	void CancelRestartAfterGameCloses();
	void GameShutdownRequested(std::string_view game);
	bool TakeRestartAfterGameCloses();

	std::vector<std::string> BuildArgs(const std::vector<std::string>& original, const Boot& boot);

	std::string ChooseProgram(const std::string& self_exe, const char* appimage_env, const char* appdir_env);

	std::string CouldNotCloseText(std::string_view why);

	class Pending
	{
	public:
		std::string Settle(bool shutdown_proceeds, std::vector<std::string> args, std::string_view why);
		bool Armed() const { return m_args.has_value(); }
		const std::vector<std::string>& Args() const { return *m_args; }
		void Disarm() { m_args.reset(); }

	private:
		std::optional<std::vector<std::string>> m_args;
	};
}
