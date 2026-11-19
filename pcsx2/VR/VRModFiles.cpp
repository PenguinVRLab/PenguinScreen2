// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VR/VRModFiles.h"

#include "Config.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <map>
#include <regex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
	const std::regex s_serial_re("^[A-Za-z]{4}-\\d{5}$");
	const std::regex s_stem_re("^([a-z0-9]+(?:-[a-z0-9]+)*)--v([1-9][0-9]{0,8})$");
	const std::regex s_slug_re("^[a-z0-9]+(?:-[a-z0-9]+)*$");

	std::string_view val(const ryml::ConstNodeRef& n)
	{
		if (!n.has_val())
			return {};
		return std::string_view(n.val().data(), n.val().size());
	}

	std::optional<bool> trueOrFalse(const ryml::ConstNodeRef& n)
	{
		const std::string_view v = val(n);
		if (v == "true")
			return true;
		if (v == "false")
			return false;
		return std::nullopt;
	}

	std::string relKey(std::string_view path)
	{
		const std::string_view file = Path::GetFileName(path);
		const std::string_view dir = Path::GetFileName(Path::GetDirectory(path));
		return StringUtil::toLower(fmt::format("{}/{}", dir, file));
	}

	void scanDir(const std::string& dir, bool user, std::vector<VR::ModFiles::File>& out)
	{
		if (dir.empty() || !FileSystem::DirectoryExists(dir.c_str()))
			return;
		FileSystem::FindResultsArray files;
		if (!FileSystem::FindFiles(dir.c_str(), "*.yaml",
				FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_SORT_BY_NAME, &files))
			return;
		for (const FILESYSTEM_FIND_DATA& fd : files)
			out.push_back({fd.FileName, user, static_cast<s64>(fd.ModificationTime), static_cast<s64>(fd.Size)});
	}
}

namespace VR::ModFiles
{
	std::optional<Name> ParseName(std::string_view path, std::string* why)
	{
		const std::string_view file = Path::GetFileName(path);
		const std::string stem(Path::GetFileTitle(path));
		const std::string folder(Path::GetFileName(Path::GetDirectory(path)));
		const bool folder_is_serial = std::regex_match(folder, s_serial_re);
		std::smatch m;
		const bool name_ok = std::regex_match(stem, m, s_stem_re);
		if (!folder_is_serial)
		{
			if (why)
			{
				if (std::regex_match(stem, s_serial_re))
					*why = fmt::format("'{}' is the old single-file layout; each entry now lives in {}/<name>--v<version>.yaml",
						file, StringUtil::toUpper(stem));
				else
					*why = fmt::format("'{}' is not in a folder named after a serial (<SERIAL>/<name>--v<version>.yaml)", file);
			}
			return std::nullopt;
		}
#ifdef VR_MODFILES_PLANTED_NOVERSION
		if (!name_ok)
			return Name{folder, stem, "0", 0};
#endif
		if (!name_ok)
		{
			if (why)
				*why = fmt::format("'{}' does not carry a whole-number version in its name: it must be <name>--v<N>.yaml, "
								   "lower-case words joined by hyphens, then --v and a whole number from 1 with no leading "
								   "zero (e.g. cockpit-fix--v1.yaml)", file);
			return std::nullopt;
		}
		return Name{folder, m[1].str(), m[2].str(), static_cast<u32>(std::stoul(m[2].str()))};
	}

	std::optional<Entry> CheckEntry(const ryml::ConstNodeRef& root, const Name& name, std::string* why)
	{
		const auto fail = [why](std::string s) -> std::optional<Entry> {
			if (why)
				*why = std::move(s);
			return std::nullopt;
		};
		if (!root.is_map() || root.num_children() != 1)
			return fail("the file must hold exactly one entry, under its folder's serial");
		const ryml::ConstNodeRef top = root.first_child();
		const std::string_view key = top.has_key() ? std::string_view(top.key().data(), top.key().size()) : std::string_view();
		if (!StringUtil::compareNoCase(key, name.serial))
			return fail(fmt::format("the entry is under '{}' but the file is in the folder '{}'", key, name.serial));

		Entry entry;
		ryml::ConstNodeRef e = top;
		if (top.is_seq())
		{
			if (top.num_children() != 1 || !top.first_child().is_map())
				return fail("one entry per file: a list under the serial may hold exactly one entry (split the others into their own files)");
			entry.list_form = true;
			e = top.first_child();
		}
		else if (!top.is_map())
		{
			return fail("the entry is not a map");
		}

		if (!e.has_child("kind"))
			return fail("the entry has no 'kind:' (fix or conversion)");
		const std::string_view kind = val(e["kind"]);
		if (kind == "fix")
			entry.kind = Kind::Fix;
		else if (kind == "conversion")
			entry.kind = Kind::Conversion;
		else if (kind == "feature")
			entry.kind = Kind::Feature;
		else
			return fail(fmt::format("kind '{}' is not fix or conversion", kind));

		if (!e.has_child("version"))
			return fail(fmt::format("the entry has no 'version:' (the file name says {})", name.version));
		if (val(e["version"]) != name.version)
			return fail(fmt::format("version '{}' disagrees with the file name's {}", val(e["version"]), name.version));

		if (!e.has_child("enabled"))
			return fail("the entry has no 'enabled:' (true or false)");
		const std::optional<bool> on = trueOrFalse(e["enabled"]);
		if (!on.has_value())
			return fail(fmt::format("enabled: '{}' is not true or false", val(e["enabled"])));
		entry.enabled = on.value();
		if (e.has_child("restart_required"))
		{
			const std::optional<bool> rr = trueOrFalse(e["restart_required"]);
			if (!rr.has_value())
				return fail(fmt::format("restart_required: '{}' is not true or false", val(e["restart_required"])));
			entry.restart_required = rr.value();
		}
		if (e.has_child("dependencies"))
		{
			const ryml::ConstNodeRef d = e["dependencies"];
			const bool none = !d.is_map() && !d.is_seq() && (!d.has_val() || val(d).empty() || val(d) == "~" || val(d) == "null");
			if (!d.is_map() && !none)
				return fail("'dependencies:' is a map of mod names to version ranges");
			if (d.is_map())
			{
				for (const ryml::ConstNodeRef& c : d.children())
				{
					const std::string key(c.has_key() ? std::string_view(c.key().data(), c.key().size()) : std::string_view());
					if (!std::regex_match(key, s_slug_re))
						return fail(fmt::format("dependencies: '{}' is not a mod name (lower-case words joined by hyphens)", key));
					if (!c.has_val() && !c.is_val())
						return fail(fmt::format("dependencies: '{}': the range is not a plain value", key));
					std::string rwhy;
					const std::string text(val(c));
					std::optional<Range> r = ParseRange(text, &rwhy);
					if (!r.has_value())
						return fail(fmt::format("dependencies: '{}': the range '{}' does not parse ({})", key, text, rwhy));
					entry.dependencies.push_back({key, text, std::move(r.value())});
				}
			}
		}

		if (entry.kind == Kind::Feature)
		{
			for (const char* k : {"ee", "ram", "vu1", "find", "guards", "fixes"})
			{
				if (e.has_child(ryml::to_csubstr(k)))
					return fail(fmt::format("a feature switches emulator code and carries no '{}:'", k));
			}
			const std::optional<u32> have = FeatureVersion(name.slug);
			if (!have.has_value() || have.value() != name.number)
				return fail(fmt::format("this build has no feature {} v{}", name.slug, name.version));
		}

		if (entry.kind == Kind::Conversion)
		{
			if (!e.has_child("mod") || !e["mod"].is_map())
				return fail("a conversion needs 'mod:' with its name, version and authors ('by')");
			const ryml::ConstNodeRef mod = e["mod"];
			if (!mod.has_child("name") || val(mod["name"]).empty())
				return fail("mod.name is missing");
			if (!mod.has_child("by") || val(mod["by"]).empty())
				return fail("mod.by is missing: name the mod's authors (a conversion is never ours)");
			if (!mod.has_child("version") || val(mod["version"]).empty())
				return fail("mod.version is missing: the conversion's own version, as its authors write it");
		}
		return entry;
	}

	ryml::ConstNodeRef EntryNode(const ryml::ConstNodeRef& root, const Entry& entry)
	{
		const ryml::ConstNodeRef top = root.first_child();
		return entry.list_form ? top.first_child() : top;
	}

	namespace
	{
		const std::vector<Feature> s_build_features = {};
		std::vector<Feature> s_test_features;
		bool s_test_table = false;
	}

	const std::vector<Feature>& Features()
	{
		return s_test_table ? s_test_features : s_build_features;
	}

	void SetFeaturesForTest(std::vector<Feature> features)
	{
		s_test_table = !features.empty();
		s_test_features = std::move(features);
	}

	std::optional<u32> FeatureVersion(std::string_view slug)
	{
		for (const Feature& f : Features())
		{
			if (f.slug == slug)
				return f.version;
		}
		return std::nullopt;
	}

	namespace
	{
		struct Partial
		{
			int parts = 0;
			u32 v[3] = {0, 0, 0};
		};

		bool parsePartial(std::string_view s, Partial& out, std::string& why)
		{
			if (s.empty())
			{
				why = "a version is missing";
				return false;
			}
			int n = 0;
			bool wild = false;
			size_t i = 0;
			while (true)
			{
				const size_t dot = s.find('.', i);
				const std::string_view p = s.substr(i, dot == std::string_view::npos ? std::string_view::npos : dot - i);
				if (n == 3)
				{
					why = fmt::format("'{}' has more than three numbers", s);
					return false;
				}
				if (p == "x" || p == "X" || p == "*")
					wild = true;
				else
				{
					const bool digits = !p.empty() && p.size() <= 9 && std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; });
					if (!digits || (p.size() > 1 && p[0] == '0') || wild)
					{
						why = fmt::format("'{}' is not a version (whole numbers or x, no leading zero, no tags)", s);
						return false;
					}
					out.v[n] = static_cast<u32>(std::stoul(std::string(p)));
					out.parts = n + 1;
				}
				n++;
				if (dot == std::string_view::npos)
					break;
				i = dot + 1;
			}
			return true;
		}

		using Cmp = Comparator;
		Cmp cmp(Cmp::Op op, u32 a, u32 b, u32 c) { return Cmp{op, a, b, c}; }
		const Cmp kNever = cmp(Cmp::LT, 0, 0, 0);

		void desugar(std::string_view op, const Partial& p, std::vector<Cmp>& out)
		{
			const u32 M = p.v[0], m = p.v[1], q = p.v[2];
			const int k = p.parts;
			if (op.empty() || op == "=")
			{
				if (k == 0)
					return;
				if (k == 3)
				{
					out.push_back(cmp(Cmp::GE, M, m, q));
					out.push_back(cmp(Cmp::LE, M, m, q));
				}
				else
				{
					out.push_back(cmp(Cmp::GE, M, k > 1 ? m : 0, 0));
					out.push_back(k == 1 ? cmp(Cmp::LT, M + 1, 0, 0) : cmp(Cmp::LT, M, m + 1, 0));
				}
			}
			else if (op == ">=")
			{
				if (k > 0)
					out.push_back(cmp(Cmp::GE, M, m, q));
			}
			else if (op == ">")
			{
				if (k == 0)
					out.push_back(kNever);
				else if (k == 3)
					out.push_back(cmp(Cmp::GT, M, m, q));
				else
					out.push_back(k == 1 ? cmp(Cmp::GE, M + 1, 0, 0) : cmp(Cmp::GE, M, m + 1, 0));
			}
			else if (op == "<")
			{
				out.push_back(k == 0 ? kNever : cmp(Cmp::LT, M, m, q));
			}
			else if (op == "<=")
			{
				if (k == 3)
					out.push_back(cmp(Cmp::LE, M, m, q));
				else if (k > 0)
					out.push_back(k == 1 ? cmp(Cmp::LT, M + 1, 0, 0) : cmp(Cmp::LT, M, m + 1, 0));
			}
			else if (op == "^")
			{
				if (k == 0)
					return;
				out.push_back(cmp(Cmp::GE, M, m, q));
				if (M > 0 || k == 1)
					out.push_back(cmp(Cmp::LT, M + 1, 0, 0));
				else if (m > 0 || k == 2)
					out.push_back(cmp(Cmp::LT, 0, m + 1, 0));
				else
					out.push_back(cmp(Cmp::LT, 0, 0, q + 1));
			}
			else if (op == "~")
			{
				if (k == 0)
					return;
				out.push_back(cmp(Cmp::GE, M, m, q));
				out.push_back(k == 1 ? cmp(Cmp::LT, M + 1, 0, 0) : cmp(Cmp::LT, M, m + 1, 0));
			}
		}

		bool holds(const Cmp& c, u32 M, u32 m, u32 q)
		{
			const auto a = std::tie(M, m, q);
			const auto b = std::tie(c.major, c.minor, c.patch);
			switch (c.op)
			{
				case Cmp::GE: return a >= b;
				case Cmp::GT: return a > b;
				case Cmp::LE: return a <= b;
				case Cmp::LT: return a < b;
			}
			return false;
		}

		std::vector<std::string_view> splitSpaces(std::string_view s)
		{
			std::vector<std::string_view> out;
			size_t i = 0;
			while (i < s.size())
			{
				while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
					i++;
				size_t j = i;
				while (j < s.size() && s[j] != ' ' && s[j] != '\t')
					j++;
				if (j > i)
					out.push_back(s.substr(i, j - i));
				i = j;
			}
			return out;
		}

		bool parseSet(std::string_view text, std::vector<Cmp>& out, std::string& why)
		{
			std::vector<std::string_view> tok = splitSpaces(text);
			if (tok.empty())
				return true;
			if (tok.size() == 3 && tok[1] == "-")
			{
				Partial lo, hi;
				if (!parsePartial(tok[0], lo, why) || !parsePartial(tok[2], hi, why))
					return false;
				if (lo.parts > 0)
					out.push_back(cmp(Cmp::GE, lo.v[0], lo.v[1], lo.v[2]));
				if (hi.parts == 3)
					out.push_back(cmp(Cmp::LE, hi.v[0], hi.v[1], hi.v[2]));
				else if (hi.parts > 0)
					out.push_back(hi.parts == 1 ? cmp(Cmp::LT, hi.v[0] + 1, 0, 0) : cmp(Cmp::LT, hi.v[0], hi.v[1] + 1, 0));
				return true;
			}
			for (size_t i = 0; i < tok.size(); i++)
			{
				std::string_view t = tok[i];
				std::string_view op;
				for (const std::string_view o : {">=", "<=", ">", "<", "=", "^", "~"})
				{
					if (t.substr(0, o.size()) == o)
					{
						op = o;
						break;
					}
				}
				std::string_view ver = t.substr(op.size());
				if (ver.empty() && !op.empty())
				{
					if (i + 1 >= tok.size())
					{
						why = fmt::format("'{}' has no version after it", op);
						return false;
					}
					ver = tok[++i];
				}
				if (ver == "-")
				{
					why = "a hyphen range is '<a> - <b>', alone in its set";
					return false;
				}
				Partial p;
				if (!parsePartial(ver, p, why))
					return false;
				desugar(op, p, out);
			}
			return true;
		}
	}

	std::optional<Range> ParseRange(std::string_view text, std::string* why)
	{
		Range r;
		std::string w;
		size_t i = 0;
		const bool has_or = text.find("||") != std::string_view::npos;
		while (true)
		{
			const size_t bar = text.find("||", i);
			const std::string_view set = text.substr(i, bar == std::string_view::npos ? std::string_view::npos : bar - i);
			if (has_or && splitSpaces(set).empty())
			{
				if (why)
					*why = "'||' has an empty side";
				return std::nullopt;
			}
			std::vector<Comparator> cs;
			if (!parseSet(set, cs, w))
			{
				if (why)
					*why = std::move(w);
				return std::nullopt;
			}
			r.sets.push_back(std::move(cs));
			if (bar == std::string_view::npos)
				break;
			i = bar + 2;
		}
		return r;
	}

	bool RangeAllows(const Range& range, u32 version)
	{
		for (const std::vector<Comparator>& set : range.sets)
		{
			if (std::all_of(set.begin(), set.end(), [version](const Comparator& c) { return holds(c, version, 0, 0); }))
				return true;
		}
		return false;
	}

	namespace
	{
		int keyColumn(std::string_view line)
		{
			size_t i = 0;
			while (i < line.size() && line[i] == ' ')
				i++;
			if (i == line.size() || line[i] == '#' || line[i] == '\r')
				return -1;
			while (i + 1 < line.size() && line[i] == '-' && line[i + 1] == ' ')
			{
				i += 2;
				while (i < line.size() && line[i] == ' ')
					i++;
			}
			return static_cast<int>(i);
		}

		std::vector<std::string_view> lines(std::string_view text)
		{
			std::vector<std::string_view> out;
			size_t i = 0;
			while (i < text.size())
			{
				const size_t nl = text.find('\n', i);
				const size_t end = nl == std::string_view::npos ? text.size() : nl + 1;
				out.push_back(text.substr(i, end - i));
				i = end;
			}
			return out;
		}
	}

	namespace
	{
		std::optional<std::pair<size_t, size_t>> entryEnabled(const std::vector<std::string_view>& ls)
		{
			size_t k = 0;
			for (; k < ls.size(); k++)
			{
				if (keyColumn(ls[k]) == 0 && ls[k].substr(0, 3) != "---")
					break;
			}
			int entry_col = -1;
			size_t first = k + 1;
			for (; first < ls.size() && entry_col < 0; first++)
				entry_col = keyColumn(ls[first]);
			if (entry_col <= 0)
				return std::nullopt;
			first--;
			if (ls[first].substr(static_cast<size_t>(entry_col), 8) == "enabled:")
				return std::pair{first, static_cast<size_t>(entry_col) + 8};
			for (size_t j = first + 1; j < ls.size(); j++)
			{
				const std::string_view l = ls[j];
				size_t n = 0;
				while (n < l.size() && l[n] == ' ')
					n++;
				if (static_cast<int>(n) == entry_col && l.substr(n, 8) == "enabled:")
					return std::pair{j, n + 8};
			}
			return std::nullopt;
		}

		std::string_view lineEnd(std::string_view l)
		{
			if (l.size() >= 2 && l.substr(l.size() - 2) == "\r\n")
				return l.substr(l.size() - 2);
			if (!l.empty() && l.back() == '\n')
				return l.substr(l.size() - 1);
			return {};
		}

		std::string withoutEntrySwitch(std::string_view text)
		{
			const std::vector<std::string_view> ls = lines(text);
			const auto at = entryEnabled(ls);
			if (!at.has_value())
				return std::string(text);
			std::string out;
			for (size_t i = 0; i < ls.size(); i++)
			{
				if (i != at->first)
					out.append(ls[i]);
				else
				{
					out.append(ls[i].substr(0, at->second));
					out.append(lineEnd(ls[i]));
				}
			}
			return out;
		}
	}

	std::optional<std::string> WithEnabled(std::string_view text, bool on)
	{
		const std::vector<std::string_view> ls = lines(text);
		const auto at = entryEnabled(ls);
		if (!at.has_value())
			return std::nullopt;
		static const std::regex re("^([ \\t]*)(true|false)([ \\t]*(#.*)?\\r?\\n?)$");
		const std::string rest(ls[at->first].substr(at->second));
		std::smatch m;
		if (!std::regex_match(rest, m, re))
			return std::nullopt;
		std::string out;
		for (size_t i = 0; i < ls.size(); i++)
		{
			if (i != at->first)
				out.append(ls[i]);
			else
			{
				out.append(ls[i].substr(0, at->second));
				out.append(fmt::format("{}{}{}", m[1].str(), on ? "true" : "false", m[3].str()));
			}
		}
		return out;
	}

	bool DiffersBeyondEnabled(std::string_view a, std::string_view b)
	{
		return withoutEntrySwitch(a) != withoutEntrySwitch(b);
	}

	namespace
	{
		ryml::ConstNodeRef entryOf(const ryml::ConstNodeRef& root)
		{
			if (!root.is_map() || root.num_children() == 0)
				return root;
			const ryml::ConstNodeRef top = root.first_child();
			return (top.is_seq() && top.num_children() > 0) ? top.first_child() : top;
		}

	}

	std::vector<File> ScanAll()
	{
		std::vector<File> out;
		scanDir(EmuFolders::VRMods, true, out);
		scanDir(Path::Combine(EmuFolders::Resources, kShippedDirName), false, out);
		return out;
	}

	namespace
	{
		struct Peek
		{
			std::string kind;
			std::optional<bool> enabled;
		};

		Peek peek(const std::string& path)
		{
			Peek out;
			const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
			if (!text.has_value())
				return out;
			Error error;
			std::optional<ryml::Tree> tree = ParseYAMLFromString(ryml::to_csubstr(text.value()), ryml::to_csubstr(path), &error, true);
			if (!tree.has_value())
				return out;
			const ryml::ConstNodeRef e = entryOf(tree->crootref());
			if (!e.is_map())
				return out;
			if (e.has_child("kind"))
				out.kind = std::string(val(e["kind"]));
			if (e.has_child("enabled"))
				out.enabled = trueOrFalse(e["enabled"]);
			return out;
		}
	}

	std::vector<File> Scan(std::vector<std::string>* notes)
	{
		std::vector<File> user, shipped, out;
		scanDir(EmuFolders::VRMods, true, user);
		scanDir(Path::Combine(EmuFolders::Resources, kShippedDirName), false, shipped);
		std::unordered_map<std::string, size_t> user_keys;
		for (File& f : user)
		{
			user_keys.emplace(relKey(f.path), out.size());
			out.push_back(std::move(f));
		}
		for (File& f : shipped)
		{
			const auto it = user_keys.find(relKey(f.path));
			if (it != user_keys.end())
			{
				if (notes)
					notes->push_back(fmt::format("{} is replaced by the user's file of the same name", f.path));
				File& mine = out[it->second];
				const std::optional<std::string> a = FileSystem::ReadFileToString(mine.path.c_str());
				const std::optional<std::string> b = FileSystem::ReadFileToString(f.path.c_str());
				if (a.has_value() && b.has_value() && DiffersBeyondEnabled(a.value(), b.value()))
				{
					mine.differs_from = f.path;
					mine.note = "your copy differs from the shipped file";
					if (notes)
						notes->push_back(fmt::format("{}: your copy differs from the shipped file {} in more than enabled:",
							mine.path, f.path));
				}
				continue;
			}
			out.push_back(std::move(f));
		}

		std::vector<std::optional<Name>> names;
		std::map<std::string, std::vector<size_t>> groups;
		for (size_t i = 0; i < out.size(); i++)
		{
			names.push_back(ParseName(out[i].path, nullptr));
			if (names.back().has_value())
				groups[StringUtil::toLower(names.back()->serial) + "/" + names.back()->slug].push_back(i);
		}
		std::vector<bool> keep(out.size(), true);
		for (const auto& [key, idx] : groups)
		{
			if (idx.size() < 2)
				continue;
			const auto number = [&names](size_t i) { return names[i]->number; };
			size_t newest = idx.front();
			for (const size_t i : idx)
				if (number(i) > number(newest))
					newest = i;
			const std::string& slug = names[newest]->slug;
			size_t run = newest;
			std::optional<u32> feature;
			if (peek(out[newest].path).kind == "feature")
			{
				feature = FeatureVersion(slug);
				for (const size_t i : idx)
					if (feature.has_value() && number(i) == feature.value())
						run = i;
				if (number(run) != feature.value_or(0))
					feature.reset();
			}
			std::optional<size_t> carry;
			for (const size_t i : idx)
			{
				if (i == run)
					continue;
				keep[i] = false;
				if (feature.has_value() && number(i) > feature.value())
				{
					if (notes)
						notes->push_back(out[i].user ? fmt::format("your {} v{} needs a newer build [{}]", slug, number(i), out[i].path) :
													   fmt::format("{} v{} needs a newer build [{}]", slug, number(i), out[i].path));
				}
				else if (notes)
				{
					notes->push_back(fmt::format("{} is not loaded: {} v{} is the {}", out[i].path, slug, number(run),
						feature.has_value() ? "version this build runs" : "newest installed version"));
				}
				if (out[i].user && !out[run].user && peek(out[i].path).enabled.has_value() &&
					(!carry.has_value() || number(i) > number(carry.value())))
					carry = i;
			}
			if (carry.has_value())
			{
				File& r = out[run];
				r.carried_enabled = peek(out[carry.value()].path).enabled;
				r.carried_from = number(carry.value());
				const std::string what = fmt::format("your switch from v{} now applies to v{}", r.carried_from, number(run));
				r.note = r.note.empty() ? what : fmt::format("{}; {}", r.note, what);
				if (notes)
					notes->push_back(fmt::format("{}: {} [{}]", slug, what, r.path));
			}
		}
		std::vector<File> chosen;
		for (size_t i = 0; i < out.size(); i++)
			if (keep[i])
				chosen.push_back(std::move(out[i]));
		return chosen;
	}
	size_t Utf8Chars(std::string_view text)
	{
		size_t n = 0;
		for (const char c : text)
			n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
		return n;
	}
}
