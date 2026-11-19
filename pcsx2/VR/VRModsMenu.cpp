// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VR/VRModsMenu.h"
#include "VR/VRModFiles.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <algorithm>
#include <optional>

namespace VR::ModsMenu
{
	using VR::Mods::Line;
	using VR::Mods::LineState;

	namespace
	{
		std::string Named(const Line& l) { return fmt::format("{} v{}", l.name.empty() ? l.slug : l.name, l.version); }

		bool DependsOn(const Line& l, std::string_view slug)
		{
			return std::any_of(l.dependencies.begin(), l.dependencies.end(), [slug](const auto& d) { return d.first == slug; });
		}
	}

	const Line* Model::line(std::string_view slug) const
	{
		for (const Line& l : m_lines)
			if (l.slug == slug)
				return &l;
		return nullptr;
	}

	void Model::SetLines(std::vector<Line> lines)
	{
		m_lines = std::move(lines);
		m_have_lines = true;
		for (auto it = m_held.begin(); it != m_held.end();)
		{
			const Line* l = line(it->first);
			if (!l || l->enabled == it->second)
				it = m_held.erase(it);
			else
				++it;
		}
		for (auto it = m_for.begin(); it != m_for.end();)
			it = m_held.count(it->first) ? std::next(it) : m_for.erase(it);
	}

	bool Model::Shown(std::string_view slug) const
	{
		const auto h = m_held.find(slug);
		if (h != m_held.end())
			return h->second;
		const Line* l = line(slug);
		return l && l->enabled;
	}

	bool Model::Held(std::string_view slug) const { return m_held.find(slug) != m_held.end(); }

	std::string Model::depProblem(const Line& start) const
	{
		std::vector<std::string> on;
		for (const Line& l : m_lines)
			if (Shown(l.slug))
				on.push_back(l.slug);
		std::vector<const Line*> todo{&start};
		std::vector<std::string> seen{start.slug};
		for (size_t i = 0; i < todo.size(); i++)
		{
			for (const auto& [slug, range] : todo[i]->dependencies)
			{
				if (std::find(seen.begin(), seen.end(), slug) != seen.end())
					continue;
				seen.push_back(slug);
				const Line* d = line(slug);
				if (!d)
					return fmt::format("needs {} {}, not installed", slug, range);
				if (d->state == LineState::Refused)
					return fmt::format("needs {} {}, which is refused: {}", slug, range, d->reason);
				if (!Shown(slug))
				{
					const std::string other = VR::Mods::CollidesWith(m_mods, slug, on);
					if (!other.empty())
						return fmt::format("needs {} {}, which would write the same place as {}, which is on", slug, range, other);
				}
				todo.push_back(d);
			}
		}
		return {};
	}

	std::string Model::Greyed(std::string_view slug) const
	{
		const Line* l = line(slug);
		if (!l)
			return {};
		if (l->state == LineState::Refused)
			return l->reason.empty() ? std::string("refused") : l->reason;
		if (!Shown(slug))
		{
			std::vector<std::string> on;
			for (const Line& o : m_lines)
				if (o.slug != l->slug && Shown(o.slug))
					on.push_back(o.slug);
			const std::string other = VR::Mods::CollidesWith(m_mods, l->slug, on);
			if (!other.empty())
				return fmt::format("would write the same place as {}, which is on", other);
		}
		return depProblem(*l);
	}

	std::string Model::Note(std::string_view slug) const
	{
		const auto f = m_for.find(slug);
		if (f != m_for.end() && Shown(slug))
			return fmt::format("on, turned on for {}", f->second);
		const Line* l = line(slug);
		return l ? l->note : std::string();
	}

	Proposal Model::Toggle(std::string_view slug)
	{
		const Line* l = line(slug);
		if (!l)
			return {};
		Proposal p;
		p.slug = l->slug;
		p.on = !Shown(slug);
		if (p.on)
		{
			const std::string grey = Greyed(slug);
			if (!grey.empty())
			{
				p.kind = Proposal::Kind::Refused;
				p.text = grey;
				return p;
			}
			std::vector<const Line*> todo{l};
			for (size_t i = 0; i < todo.size(); i++)
			{
				for (const auto& dep : todo[i]->dependencies)
				{
					const Line* d = line(dep.first);
					if (!d || d == l || std::find(p.also.begin(), p.also.end(), d->slug) != p.also.end() || Shown(d->slug))
						continue;
					p.also.push_back(d->slug);
					todo.push_back(d);
				}
			}
		}
		else
		{
			std::vector<std::string> off{l->slug};
			for (bool grew = true; grew;)
			{
				grew = false;
				for (const Line& o : m_lines)
				{
					if (std::find(off.begin(), off.end(), o.slug) != off.end() || !Shown(o.slug))
						continue;
					if (std::any_of(off.begin(), off.end(), [&o](const std::string& s) { return DependsOn(o, s); }))
					{
						off.push_back(o.slug);
						p.also.push_back(o.slug);
						grew = true;
					}
				}
			}
		}
		if (p.also.empty())
		{
			Confirm(p);
			p.kind = Proposal::Kind::Done;
			return p;
		}
		std::string names;
		for (const std::string& s : p.also)
			names += (names.empty() ? "" : ", ") + Named(*line(s));
		p.kind = Proposal::Kind::Confirm;
		p.text = fmt::format("Switching {} {} also switches {}: {}.", p.on ? "on" : "off", Named(*l), p.on ? "on" : "off", names);
		return p;
	}

	void Model::Confirm(const Proposal& p)
	{
		const auto set = [this](const std::string& slug, bool on) {
			const Line* l = line(slug);
			if (!l)
				return;
			if (l->enabled == on)
				m_held.erase(slug);
			else
				m_held[slug] = on;
			if (!on)
				m_for.erase(slug);
		};
		set(p.slug, p.on);
		for (const std::string& a : p.also)
		{
			set(a, p.on);
			if (!p.on)
				continue;
			std::string needer = p.slug;
			const Line* first = line(p.slug);
			if (!(first && DependsOn(*first, a)))
			{
				for (const std::string& b : p.also)
				{
					const Line* bl = line(b);
					if (b != a && bl && DependsOn(*bl, a))
					{
						needer = b;
						break;
					}
				}
			}
			m_for[a] = needer;
		}
	}

	std::vector<Change> Model::Changes() const
	{
		std::vector<Change> out;
		for (const Line& l : m_lines)
		{
			const auto h = m_held.find(l.slug);
			if (h != m_held.end())
				out.push_back({l.slug, h->second});
		}
		return out;
	}

	bool Model::NeedsRestartPrompt() const
	{
		return std::any_of(m_held.begin(), m_held.end(), [this](const auto& h) {
			const Line* l = line(h.first);
			return l && l->restart_required;
		});
	}

	void Model::Discard()
	{
		m_held.clear();
		m_for.clear();
	}

	std::string StatusText(const Line& l)
	{
		switch (l.state)
		{
			case LineState::Active:
				return "active";
			case LineState::Waiting:
				return l.reason.empty() ? std::string("waiting") : "waiting: " + l.reason;
			case LineState::Off:
				return l.reason.empty() ? std::string("off") : l.reason;
			case LineState::NotHere:
				return l.reason;
			case LineState::Refused:
			default:
				return l.reason.empty() ? std::string("refused") : "refused: " + l.reason;
		}
	}

	std::string RestartPromptText(bool close_game)
	{
		return fmt::format("{}\n{}", kRestartPrompt, close_game ? kRestartPromptCloseGame : kRestartPromptLoss);
	}

	std::string HeldText(bool on, bool restart_required)
	{
		return fmt::format("changed, {} {}", on ? "on" : "off", restart_required ? "after restart" : "when you leave this page");
	}

	std::string Clean(std::string_view text)
	{
		std::string out;
		out.reserve(text.size());
		for (size_t i = 0; i < text.size();)
		{
			const u8 c = static_cast<u8>(text[i]);
			if (c < 0x80)
			{
				out += (c == '\r' || c == '\n' || c == '\t') ? ' ' : static_cast<char>(c);
				i++;
				continue;
			}
			const size_t len = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
			if (len == 0)
			{
				i++;
				continue;
			}
			size_t have = 1;
			while (have < len && i + have < text.size() && (static_cast<u8>(text[i + have]) & 0xC0) == 0x80)
				have++;
			if (have == len)
				out.append(text.substr(i, len));
			i += have;
		}
		return out;
	}

	std::string Cut(std::string_view text, size_t max_chars, std::string_view ellipsis)
	{
		std::string clean = Clean(text);
		if (VR::ModFiles::Utf8Chars(clean) <= max_chars)
			return clean;
		const size_t keep = max_chars - std::min(max_chars, VR::ModFiles::Utf8Chars(ellipsis));
		size_t chars = 0, at = 0;
		for (; at < clean.size(); at++)
		{
			if ((static_cast<u8>(clean[at]) & 0xC0) != 0x80 && chars++ == keep)
				break;
		}
		clean.resize(at);
		clean.append(ellipsis);
		return clean;
	}

	LineText TextOf(const Model& m, const Line& l, std::string_view ellipsis)
	{
		LineText t;
		t.name = Cut(l.name.empty() ? l.slug : l.name, VR::ModFiles::kNameMaxChars, ellipsis);
		t.description = Cut(l.description, VR::ModFiles::kDescriptionMaxChars, ellipsis);
		std::vector<std::string> parts;
		if (l.restart_required)
			parts.emplace_back("requires restart");
		parts.push_back(m.Held(l.slug) ? HeldText(m.Shown(l.slug), l.restart_required) : StatusText(l));
		if (std::string note = m.Note(l.slug); !note.empty())
			parts.push_back(std::move(note));
		if (const std::string grey = m.Greyed(l.slug); !grey.empty() && l.state != LineState::Refused)
			parts.push_back(fmt::format("cannot be switched on: {}", grey));
		for (const std::string& p : parts)
			t.subtext += (t.subtext.empty() ? "" : " \xC2\xB7 ") + p;
		return t;
	}

	bool SameGame(const GameId& a, const GameId& b)
	{
		return a.running && b.running && a.serial == b.serial && a.crc == b.crc;
	}

	PromptOutcome AnswerPrompt(const Model& m, bool proceed, const GameId& asked_for, const GameId& now, bool close_game)
	{
		PromptOutcome o;
		if (!SameGame(asked_for, now))
		{
			o.discard = true;
			return o;
		}
		if (!proceed)
		{
			o.keep_open = true;
			return o;
		}
		o.write = true;
		o.changes = m.Changes();
		if (close_game)
		{
			o.close_game = true;
			o.restart_after_close = m.NeedsRestartPrompt();
			return o;
		}
		o.restart = m.NeedsRestartPrompt();
		return o;
	}

	CloseDecision DecideClose(const Model& m)
	{
		CloseDecision d;
		d.prompt = m.NeedsRestartPrompt();
		d.write_now = !d.prompt && m.HasChanges();
		return d;
	}

	namespace
	{
		std::vector<std::string> OlderCopies(const std::string& dir, std::string_view slug, u32 version)
		{
			std::vector<std::string> out;
			FileSystem::FindResultsArray files;
			if (dir.empty() || !FileSystem::FindFiles(dir.c_str(), "*.yaml", FILESYSTEM_FIND_FILES, &files))
				return out;
			for (const FILESYSTEM_FIND_DATA& f : files)
			{
				const std::optional<VR::ModFiles::Name> name = VR::ModFiles::ParseName(f.FileName, nullptr);
				if (name.has_value() && name->slug == slug && name->number < version)
					out.push_back(f.FileName);
			}
			std::sort(out.begin(), out.end());
			return out;
		}
	}

	WriteOutcome WritePlayerCopies(const std::vector<Line>& lines, const std::vector<Change>& changes, const std::string& user_root)
	{
		struct Copy
		{
			std::string path, text;
		};
		std::vector<Copy> copies;
		std::vector<std::string> removals;
		std::vector<std::string> refusals;
		for (const Change& c : changes)
			if (std::none_of(lines.begin(), lines.end(), [&c](const Line& x) { return x.slug == c.slug; }))
				refusals.push_back(fmt::format("{}: not listed for this game", c.slug));
		for (const Line& l : lines)
		{
			const auto c = std::find_if(changes.begin(), changes.end(), [&l](const Change& x) { return x.slug == l.slug; });
			const bool changed = c != changes.end();
			const std::string file(Path::GetFileName(l.source));
			const std::string folder(Path::GetFileName(Path::GetDirectory(l.source)));
			const std::string user_dir = user_root.empty() ? std::string() : Path::Combine(user_root, folder);
			const std::vector<std::string> older =
				(changed || l.state != LineState::Refused) ? OlderCopies(user_dir, l.slug, l.version) : std::vector<std::string>();
			if (!changed && older.empty())
				continue;
			const std::optional<std::string> text = FileSystem::ReadFileToString(l.source.c_str());
			if (!text.has_value())
			{
				if (changed)
					refusals.push_back(fmt::format("{}: the file cannot be read", file));
				continue;
			}
			const std::optional<std::string> updated = VR::ModFiles::WithEnabled(text.value(), changed ? c->on : l.enabled);
			if (!updated.has_value())
			{
				if (changed)
					refusals.push_back(fmt::format("{}: {}", file, kCannotRewriteEnabled));
				continue;
			}
			std::string path = l.source;
			if (!l.user)
			{
				if (user_dir.empty())
				{
					if (changed)
						refusals.push_back(fmt::format("{}: no user folder to write your copy into", file));
					continue;
				}
				path = Path::Combine(user_dir, file);
			}
			for (const std::string& o : older)
				if (o != path)
					removals.push_back(o);
			copies.push_back({std::move(path), std::move(updated.value())});
		}
		WriteOutcome out;
		if (!refusals.empty())
		{
			for (const std::string& r : refusals)
				out.refusal += (out.refusal.empty() ? "" : "; ") + r;
			return out;
		}

		static constexpr const char* kNew = ".vrmods-new";
		static constexpr const char* kOld = ".vrmods-old";
		std::vector<std::string> made_dirs, temps, placed;
		std::vector<std::pair<std::string, std::string>> aside;
		const auto fail = [&](std::string why) {
			for (const std::string& p : placed)
				FileSystem::DeleteFilePath(p.c_str());
			for (auto it = aside.rbegin(); it != aside.rend(); ++it)
				FileSystem::RenamePath(it->second.c_str(), it->first.c_str());
			for (const std::string& t : temps)
				if (FileSystem::FileExists(t.c_str()))
					FileSystem::DeleteFilePath(t.c_str());
			for (auto it = made_dirs.rbegin(); it != made_dirs.rend(); ++it)
				FileSystem::DeleteDirectory(it->c_str());
			out.refusal = std::move(why);
			out.written.clear();
			out.removed.clear();
			return out;
		};
		for (const Copy& c : copies)
		{
			const std::string dir(Path::GetDirectory(c.path));
			if (!FileSystem::DirectoryExists(dir.c_str()))
			{
				if (!FileSystem::CreateDirectoryPath(dir.c_str(), true))
					return fail(fmt::format("{} cannot be made", dir));
				made_dirs.push_back(dir);
			}
			const std::string tmp = c.path + kNew;
			if (!FileSystem::WriteStringToFile(tmp.c_str(), c.text))
			{
				FileSystem::DeleteFilePath(tmp.c_str());
				return fail(fmt::format("{} cannot be written", c.path));
			}
			temps.push_back(tmp);
		}
		std::vector<std::string> away = removals;
		for (const Copy& c : copies)
			if (FileSystem::FileExists(c.path.c_str()))
				away.push_back(c.path);
		for (const std::string& a : away)
		{
			const std::string moved = a + kOld;
			if (!FileSystem::RenamePath(a.c_str(), moved.c_str()))
				return fail(fmt::format("{} cannot be replaced", a));
			aside.emplace_back(a, moved);
		}
		for (size_t i = 0; i < copies.size(); i++)
		{
			if (!FileSystem::RenamePath(temps[i].c_str(), copies[i].path.c_str()))
				return fail(fmt::format("{} cannot be written", copies[i].path));
			placed.push_back(copies[i].path);
		}
		for (const auto& [original, moved] : aside)
			FileSystem::DeleteFilePath(moved.c_str());
		out.written = placed;
		out.removed = removals;
		out.ok = true;
		return out;
	}
}
