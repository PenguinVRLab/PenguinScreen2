// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "VR/VRModFiles.h"
#include "VR/VRMods.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace VR::ModsMenu
{
	inline constexpr char kRestartPrompt[] = "You have made changes that require a restart. Proceed?";
	inline constexpr char kRestartPromptLoss[] = "The game will restart. Progress since your last save will be lost.";
	inline constexpr char kRestartPromptCloseGame[] = "Your changes will be saved and will apply the next time the game starts.";
	std::string RestartPromptText(bool close_game = false);
	inline constexpr char kPageTitle[] = "Mods: changes apply after a restart";
	using VR::ModFiles::kCannotRewriteEnabled;

	struct Change
	{
		std::string slug;
		bool on = false;
	};

	struct Proposal
	{
		enum class Kind
		{
			None,
			Done,
			Confirm,
			Refused,
		};
		Kind kind = Kind::None;
		std::string slug;
		bool on = false;
		std::vector<std::string> also;
		std::string text;
	};

	class Model
	{
	public:
		void SetLines(std::vector<VR::Mods::Line> lines);
		void SetMods(std::vector<VR::Mods::Mod> mods) { m_mods = std::move(mods); }
		const std::vector<VR::Mods::Line>& Lines() const { return m_lines; }
		bool HasLines() const { return m_have_lines; }

		bool Shown(std::string_view slug) const;
		bool Held(std::string_view slug) const;
		std::string Greyed(std::string_view slug) const;
		std::string Note(std::string_view slug) const;

		Proposal Toggle(std::string_view slug);
		void Confirm(const Proposal& p);

		std::vector<Change> Changes() const;
		bool HasChanges() const { return !m_held.empty(); }
		bool NeedsRestartPrompt() const;
		void Discard();

	private:
		const VR::Mods::Line* line(std::string_view slug) const;
		std::string depProblem(const VR::Mods::Line& l) const;

		std::vector<VR::Mods::Line> m_lines;
		std::vector<VR::Mods::Mod> m_mods;
		bool m_have_lines = false;
		std::map<std::string, bool, std::less<>> m_held;
		std::map<std::string, std::string, std::less<>> m_for;
	};

	std::string StatusText(const VR::Mods::Line& l);

	struct LineText
	{
		std::string name;
		std::string description;
		std::string subtext;
	};
	LineText TextOf(const Model& m, const VR::Mods::Line& l, std::string_view ellipsis);
	std::string HeldText(bool on, bool restart_required);
	std::string Clean(std::string_view text);
	std::string Cut(std::string_view text, size_t max_chars, std::string_view ellipsis);

	struct CloseDecision
	{
		bool prompt = false;
		bool write_now = false;
	};
	CloseDecision DecideClose(const Model& m);

	struct GameId
	{
		bool running = false;
		std::string serial;
		u32 crc = 0;
	};
	bool SameGame(const GameId& a, const GameId& b);
	struct PromptOutcome
	{
		bool write = false;
		bool restart = false;
		bool keep_open = false;
		bool discard = false;
		bool close_game = false;
		bool restart_after_close = false;
		std::vector<Change> changes;
	};
	PromptOutcome AnswerPrompt(const Model& m, bool proceed, const GameId& asked_for, const GameId& now, bool close_game = false);

	struct WriteOutcome
	{
		bool ok = false;
		std::string refusal;
		std::vector<std::string> written;
		std::vector<std::string> removed;
	};
	WriteOutcome WritePlayerCopies(const std::vector<VR::Mods::Line>& lines, const std::vector<Change>& changes,
		const std::string& user_root);
}
