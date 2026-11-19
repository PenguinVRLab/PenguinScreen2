// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VR/VRMods.h"
#include "VR/VRModFiles.h"
#include "VR/NetplayModList.h"
#include "VR/VRProfileDB.h"

#include "BuildVersion.h"
#include "Host.h"
#include "Config.h"
#include "Memory.h"
#include "MTVU.h"
#include "VMManager.h"
#include "VU.h"
#include "VUmicro.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/YAML.h"

#include "fmt/format.h"

#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace
{
	constexpr u32 EE_MAIN_RAM_BYTES = 0x02000000u;

	std::string_view nodeVal(const ryml::ConstNodeRef& node)
	{
		return node.has_val() ? std::string_view(node.val().data(), node.val().size()) : std::string_view();
	}

	std::string_view nodeKey(const ryml::ConstNodeRef& node)
	{
		return node.has_key() ? std::string_view(node.key().data(), node.key().size()) : std::string_view();
	}

	std::optional<u32> parseHex(std::string_view s)
	{
		if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			s = s.substr(2);
		if (s.empty() || s.size() > 8)
			return std::nullopt;
		return StringUtil::FromChars<u32>(s, 16);
	}

	bool parseWords(const ryml::ConstNodeRef& seq, std::vector<u32>& out, std::string& why)
	{
		if (!seq.is_seq())
		{
			why = "'words' is not a list";
			return false;
		}
		for (const ryml::ConstNodeRef& w : seq.children())
		{
			const std::optional<u32> v = parseHex(nodeVal(w));
			if (!v.has_value())
			{
				why = fmt::format("'{}' in 'words' is not a hex word", nodeVal(w));
				return false;
			}
			out.push_back(v.value());
		}
		return true;
	}

	std::optional<u32> parseFloatBits(std::string_view s)
	{
		const std::optional<float> f = StringUtil::FromChars<float>(s);
		if (!f.has_value() || !std::isfinite(f.value()))
			return std::nullopt;
		u32 u;
		std::memcpy(&u, &f.value(), 4);
		return u;
	}

	bool parseValue(const ryml::ConstNodeRef& w, u32& value, std::optional<u32>& original, std::string& why)
	{
		const bool v = w.has_child("value"), f = w.has_child("float");
		if (v == f)
		{
			why = "a word gives exactly one of 'value' and 'float'";
			return false;
		}
		const std::optional<u32> val = v ? parseHex(nodeVal(w["value"])) : parseFloatBits(nodeVal(w["float"]));
		if (!val.has_value())
		{
			why = v ? "'value' is not hex" : "'float' is not a finite number";
			return false;
		}
		value = val.value();
		if (w.has_child("original"))
		{
			original = v ? parseHex(nodeVal(w["original"])) : parseFloatBits(nodeVal(w["original"]));
			if (!original.has_value())
			{
				why = "'original' is not in the form of the value";
				return false;
			}
		}
		return true;
	}

	bool parseAt(const ryml::ConstNodeRef& n, bool find, const char* what, u32& at, std::string& why)
	{
		const bool a = n.has_child("address"), o = n.has_child("offset");
		if (find ? (!o || a) : (!a || o))
		{
			why = fmt::format("{} {}", what,
				find ? "under find: says 'offset' (and not 'address')" : "says 'address' (and 'offset' only under find:)");
			return false;
		}
		const std::optional<u32> v = parseHex(nodeVal(n[find ? "offset" : "address"]));
		if (!v.has_value())
		{
			why = fmt::format("{}: the {} is not hex", what, find ? "offset" : "address");
			return false;
		}
		at = v.value();
		return true;
	}

	bool parseFind(const ryml::ConstNodeRef& f, VR::Mods::Find& out, std::string& why)
	{
		const int forms = f.has_child("floats") + f.has_child("words") + f.has_child("bytes");
		if (!f.is_map() || forms != 1)
		{
			why = "find: gives exactly one of 'floats', 'words' and 'bytes'";
			return false;
		}
		if (f.has_child("bytes"))
		{
			const std::string_view b = nodeVal(f["bytes"]);
			for (size_t i = 0; i < b.size();)
			{
				if (b[i] == ' ')
				{
					i++;
					continue;
				}
				const std::string_view tok = b.substr(i, 2);
				if (tok == "?" "?")
				{
					out.bytes.push_back(0);
					out.mask.push_back(0);
				}
				else
				{
					const std::optional<u8> v = (tok.size() == 2) ? StringUtil::FromChars<u8>(tok, 16) : std::nullopt;
					if (!v.has_value())
					{
						why = fmt::format("find: bytes: '{}' is not a byte", tok);
						return false;
					}
					out.bytes.push_back(v.value());
					out.mask.push_back(0xFF);
				}
				i += 2;
			}
		}
		else
		{
			const bool fl = f.has_child("floats");
			for (const ryml::ConstNodeRef& e : f[fl ? "floats" : "words"].children())
			{
				const std::optional<u32> u = fl ? parseFloatBits(nodeVal(e)) : parseHex(nodeVal(e));
				if (!u.has_value())
				{
					why = fmt::format("find: '{}' is not a {}", nodeVal(e), fl ? "number" : "hex word");
					return false;
				}
				for (int k = 0; k < 4; k++)
				{
					out.bytes.push_back(static_cast<u8>(u.value() >> (8 * k)));
					out.mask.push_back(0xFF);
				}
			}
		}
		if (out.bytes.empty() || out.bytes.size() > 256)
		{
			why = "find: a pattern is 1 to 256 bytes";
			return false;
		}
		if (f.has_child("align"))
		{
			const std::optional<u32> a = StringUtil::FromChars<u32>(nodeVal(f["align"]));
			if (!a.has_value() || (a.value() != 1 && a.value() != 2 && a.value() != 4))
			{
				why = "find: align is 1, 2 or 4";
				return false;
			}
			out.align = a.value();
		}
		if (f.has_child("range"))
		{
			const ryml::ConstNodeRef r = f["range"];
			const std::optional<u32> lo = (r.is_seq() && r.num_children() == 2) ? parseHex(nodeVal(r[0])) : std::nullopt;
			const std::optional<u32> hi = (r.is_seq() && r.num_children() == 2) ? parseHex(nodeVal(r[1])) : std::nullopt;
			if (!lo.has_value() || !hi.has_value() || hi.value() <= lo.value())
			{
				why = "find: range is [lo, hi] with lo < hi";
				return false;
			}
			out.lo = lo.value();
			out.hi = hi.value();
		}
		return true;
	}

	bool parseMod(std::string_view serial, const ryml::ConstNodeRef& m, std::string_view source, VR::Mods::Mod& out,
		std::string& why)
	{
		using namespace VR::Mods;
		if (!m.is_map())
		{
			why = "an entry is not a map";
			return false;
		}
		out.serial = StringUtil::toLower(serial);
		out.source = std::string(source);
		out.name = m.has_child("name") ? std::string(nodeVal(m["name"])) : std::string("(unnamed)");
		if (!ParseNetplay(m, out, why))
			return false;
		if (m.has_child("enabled"))
		{
			const std::string_view v = nodeVal(m["enabled"]);
			if (v != "true" && v != "false")
			{
				why = fmt::format("enabled: '{}' is not true or false", v);
				return false;
			}
			out.enabled = (v == "true");
		}
		if (m.has_child("restart_required"))
		{
			const std::string_view v = nodeVal(m["restart_required"]);
			if (v != "true" && v != "false")
			{
				why = fmt::format("restart_required: '{}' is not true or false", v);
				return false;
			}
			out.restart_required = (v == "true");
		}

		if (m.has_child("crcs"))
		{
			if (!m["crcs"].is_seq())
			{
				why = "'crcs' is not a list";
				return false;
			}
			for (const ryml::ConstNodeRef& c : m["crcs"].children())
			{
				const std::optional<u32> v = parseHex(nodeVal(c));
				if (!v.has_value())
				{
					why = fmt::format("crc '{}' is not hex", nodeVal(c));
					return false;
				}
				out.crcs.push_back(v.value());
			}
		}

		for (const char* xbox : {"ram", "xbe_versions"})
		{
			if (m.has_child(ryml::to_csubstr(xbox)))
			{
				why = fmt::format("'{}' is an Xbox field; a PS2 fix says 'ee' and 'crcs'", xbox);
				return false;
			}
		}
		{
			Origin origin;
			if (!ParseOrigin(m, origin, why))
				return false;
		}
		if (m.has_child("find"))
		{
			out.find.emplace();
			if (!parseFind(m["find"], out.find.value(), why))
				return false;
		}
		const bool find = out.find.has_value();

		if (m.has_child("guards"))
		{
			for (const ryml::ConstNodeRef& g : m["guards"].children())
			{
				Guard gd;
				gd.relative = find;
				if (!parseAt(g, find, "a guard", gd.address, why))
					return false;
				const std::optional<u32> e = g.has_child("equals") ? parseHex(nodeVal(g["equals"])) : std::nullopt;
				if (!e.has_value())
				{
					why = "a guard needs hex 'equals'";
					return false;
				}
				gd.equals = e.value();
				if (g.has_child("width"))
				{
					const std::optional<u32> w = StringUtil::FromChars<u32>(nodeVal(g["width"]));
					if (!w.has_value() || (w.value() != 1 && w.value() != 2 && w.value() != 4))
					{
						why = "a guard width is 1, 2 or 4";
						return false;
					}
					gd.width = w.value();
				}
				if ((!find && static_cast<u64>(gd.address) + gd.width > EE_MAIN_RAM_BYTES) || gd.address % gd.width)
				{
					why = fmt::format("guard {} {:#x} is not an aligned EE main-RAM {}", find ? "offset" : "address",
						gd.address, find ? "offset" : "address");
					return false;
				}
				out.guards.push_back(gd);
			}
		}

		if (m.has_child("vu1"))
		{
			for (const ryml::ConstNodeRef& p : m["vu1"].children())
			{
				Vu1Patch vp;
				const std::optional<u32> micro = p.has_child("micro") ? parseHex(nodeVal(p["micro"])) : std::nullopt;
				if (!micro.has_value() || !p.has_child("words") || !parseWords(p["words"], vp.words, why))
				{
					if (why.empty())
						why = "a vu1 patch needs hex 'micro' and 'words'";
					return false;
				}
				vp.micro = micro.value();
				if (vp.micro % 8 || vp.words.empty() || vp.words.size() % 2 ||
					vp.micro + vp.words.size() * 4 > VU1_MICRO_BYTES)
				{
					why = fmt::format("vu1 patch at {:#x}: micro must be 8-aligned and the words whole "
									  "instruction pairs inside 0x4000 bytes",
						vp.micro);
					return false;
				}
				if (!p.has_child("match") || !p["match"].is_seq() || p["match"].num_children() == 0)
				{
					why = "a vu1 patch needs at least one 'match' signature: it must never be written into a "
						  "program it was not made for";
					return false;
				}
				for (const ryml::ConstNodeRef& s : p["match"].children())
				{
					Vu1Signature sig;
					const std::optional<u32> sm = s.has_child("micro") ? parseHex(nodeVal(s["micro"])) : std::nullopt;
					if (!sm.has_value() || !s.has_child("words") || !parseWords(s["words"], sig.words, why))
					{
						if (why.empty())
							why = "a signature needs hex 'micro' and 'words'";
						return false;
					}
					sig.micro = sm.value();
					if (sig.micro % 4 || sig.words.empty() || sig.micro + sig.words.size() * 4 > VU1_MICRO_BYTES)
					{
						why = fmt::format("signature at {:#x} does not fit micro memory", sig.micro);
						return false;
					}
					const u32 lo = sig.micro, hi = sig.micro + static_cast<u32>(sig.words.size()) * 4;
					if (lo < vp.micro + vp.words.size() * 4 && vp.micro < hi)
					{
						why = fmt::format("signature at {:#x} overlaps the patch it guards", sig.micro);
						return false;
					}
					vp.match.push_back(std::move(sig));
				}
				out.vu1.push_back(std::move(vp));
			}
		}

		if (m.has_child("ee"))
		{
			for (const ryml::ConstNodeRef& w : m["ee"].children())
			{
				EeWord ew;
				ew.relative = find;
				if (!parseAt(w, find, "an ee word", ew.address, why) || !parseValue(w, ew.value, ew.original, why))
					return false;
				if (find && !ew.original.has_value())
				{
					why = "an ee word under find: carries 'original' (F3)";
					return false;
				}
				if ((!find && static_cast<u64>(ew.address) + 4 > EE_MAIN_RAM_BYTES) || ew.address % 4)
				{
					why = fmt::format("ee {} {:#x} is not an aligned EE main-RAM {}", find ? "offset" : "address",
						ew.address, find ? "offset" : "address");
					return false;
				}
				out.ee.push_back(ew);
			}
		}

		if (out.vu1.empty() && out.ee.empty())
		{
			why = "the entry changes nothing (no 'vu1' and no 'ee')";
			return false;
		}
		return true;
	}

	bool s_loaded = false;
	std::vector<VR::Mods::Mod> s_mods;
	std::vector<std::string> s_issues;
	std::vector<std::string> s_notes;

	struct Item
	{
		std::string serial;
		std::string folder;
		std::string slug;
		u32 version = 0;
		std::string name;
		std::string description;
		VR::ModFiles::Kind kind = VR::ModFiles::Kind::Fix;
		bool theirs = false;
		std::string by;
		bool restart_required = true;
		bool enabled = false;
		bool refused = false;
		std::string reason;
		std::string note;
		std::string turned_on_for;
		std::string not_saved;
		std::vector<VR::ModFiles::Dependency> deps;
		std::string source;
		bool user = false;
	};
	std::vector<Item> s_items;

	std::string s_bound_serial;
	u32 s_bound_crc = 0;
	bool s_bound = false;
	std::vector<const VR::Mods::Mod*> s_active;
	std::vector<bool> s_armed;
	std::vector<bool> s_gate_said;
	std::vector<bool> s_role_said;
	std::string s_settings_key;

	void noteSettings()
	{
		const char* limiter = "nominal";
		switch (VMManager::GetLimiterMode())
		{
			case LimiterModeType::Turbo: limiter = "turbo"; break;
			case LimiterModeType::Slomo: limiter = "slomo"; break;
			case LimiterModeType::Unlimited: limiter = "unlimited"; break;
			default: break;
		}
		const int rate = EmuConfig.Speedhacks.EECycleRate, skip = EmuConfig.Speedhacks.EECycleSkip;
		const float speed = EmuConfig.EmulationSpeed.NominalScalar;
		const bool gamedb = EmuConfig.EnablePatches;
		const std::string key = fmt::format("{}:{}:{:.2f}:{}:{}", rate, skip, speed, limiter, gamedb);
		if (key == s_settings_key)
			return;
		s_settings_key = key;
		VR::Netplay::SetLocalSettings(VR::Netplay::SettingsLines(rate, skip, speed, limiter, gamedb));
	}
	bool s_bound_eth = false;
	std::string s_notice_key;
	std::string s_overlap_key;
	struct FindState
	{
		std::optional<u32> anchor;
		int throttle = 0;
		bool refused = false;
		bool waiting_said = false;
	};
	std::vector<FindState> s_find;
	constexpr int FIND_SEARCH_PERIOD = 60;
	VR::Mods::Vu1Tracker s_tracker;
	std::vector<const VR::Mods::Vu1Patch*> s_vu1_logged;

	bool enabledByEnvironment()
	{
		static const char* env = []() -> const char* {
			if (const char* e = std::getenv("PCSX2_VR_MODS"))
				return e;
			return nullptr;
		}();
		return !(env && (std::strcmp(env, "0") == 0 || StringUtil::compareNoCase(env, "off") ||
							StringUtil::compareNoCase(env, "false")));
	}

	u32 readWidth(u32 address, u32 width)
	{
		switch (width)
		{
			case 1:
				return memRead8(address);
			case 2:
				return memRead16(address);
			default:
				return memRead32(address);
		}
	}

	bool guardsPass(const VR::Mods::Mod& m, u32 base)
	{
		for (const VR::Mods::Guard& g : m.guards)
		{
			const u64 at = static_cast<u64>(base) + g.address;
			if (at + g.width > EE_MAIN_RAM_BYTES || readWidth(static_cast<u32>(at), g.width) != g.equals)
				return false;
		}
		return true;
	}

	void writeMicro(const VR::Mods::Vu1Patch& p)
	{
		const u32 bytes = static_cast<u32>(p.words.size()) * 4;
		if (THREAD_VU1)
		{
			vu1Thread.WriteMicroMem(p.micro, p.words.data(), bytes);
		}
		else
		{
			CpuVU1->Clear(p.micro, bytes);
			std::memcpy(VU1.Micro + p.micro, p.words.data(), bytes);
		}
	}

	void unbind()
	{
		s_active.clear();
		s_armed.clear();
		s_find.clear();
		s_tracker.SetPatches({});
		s_tracker.Reset();
		s_bound = false;
		VR::Netplay::SetLocalRecord({});
	}

	void bind(const std::string& serial, u32 crc)
	{
		const bool eth_changed = s_bound_serial == serial && s_bound_crc == crc && s_bound_eth != EmuConfig.DEV9.EthEnable;
		unbind();
		s_bound_serial = serial;
		s_bound_crc = crc;
		s_bound = true;
		s_bound_eth = EmuConfig.DEV9.EthEnable;
		const VR::Mods::NetMode mode = EmuConfig.DEV9.EthEnable ? VR::Mods::NetMode::Networked : VR::Mods::NetMode::Offline;
		std::vector<VR::Mods::Collision> collisions;
		s_active = VR::Mods::Select(serial, crc, mode, &collisions);
		for (const VR::Mods::Collision& c : collisions)
			Console.WarningFmt("(VR) Mods: {}.", VR::Mods::CollisionText(c));
		const std::string overlap_key = collisions.empty() ? std::string() :
			fmt::format("{}:{:08X}:{}:{}:{}", serial, crc, s_bound_eth, collisions.size(), collisions.front().address);
		if (!collisions.empty() && overlap_key != s_overlap_key)
		{
			const VR::Mods::Collision& c = collisions.front();
			std::string text = c.guard ?
				fmt::format("vr-mods: a guard of {} reads EE word {:#x} that {} writes: neither is on.",
					Path::GetFileName(c.a->source), c.address, Path::GetFileName(c.b->source)) :
				fmt::format("vr-mods: {} and {} both write {} {:#x}: neither is on. Enable one version only.",
					Path::GetFileName(c.a->source), Path::GetFileName(c.b->source), c.vu1 ? "VU1 micro" : "EE word", c.address);
			if (collisions.size() > 1)
				text += fmt::format(" ({} more pair(s) in the log.)", collisions.size() - 1);
			Host::AddKeyedOSDMessage("VRModsOverlap", std::move(text), 12.0f);
		}
		s_overlap_key = overlap_key;
		const auto collided = [&collisions](const VR::Mods::Mod* m) {
			return std::any_of(collisions.begin(), collisions.end(), [m](const VR::Mods::Collision& c) {
				return c.a->source == m->source || c.b->source == m->source;
			});
		};
		size_t held = 0, present = 0;
		for (const VR::Mods::Mod* m : VR::Mods::Select(serial, crc, VR::Mods::NetMode::Offline))
		{
			if (std::find(s_active.begin(), s_active.end(), m) != s_active.end() || collided(m))
				continue;
			held++;
			Console.WriteLnFmt("(VR) Mods: '{}' not applied: the network adapter is on and its runs_in ({}) lists "
							   "neither lan nor online. [{}]",
				m->name, VR::Mods::RunsInText(*m), m->source);
			if (!m->find.has_value() && !m->ee.empty() &&
				std::all_of(m->ee.begin(), m->ee.end(), [](const VR::Mods::EeWord& w) {
					return static_cast<u64>(w.address) + 4 <= EE_MAIN_RAM_BYTES && memRead32(w.address) == w.value;
				}))
			{
				present++;
				Console.WarningFmt("(VR) Mods: '{}' is held off but its code is already in memory (a save state made "
								   "offline, or the adapter switched on mid-game): it runs anyway until a reboot. [{}]",
					m->name, m->source);
			}
		}
		const std::string notice_key = fmt::format("{}:{:08X}:{}:{}:{}", serial, crc, s_bound_eth, held, present);
		if ((held > 0 || present > 0) && notice_key != s_notice_key)
		{
			std::string text = fmt::format("{} offline-only mod(s) held off: the network adapter is on. Turn it off to use them.", held);
			if (eth_changed)
				text += " (switched on mid-game)";
			if (present > 0)
				text += fmt::format(" {} of them already in memory from before: running until a reboot.", present);
			Host::AddKeyedOSDMessage("VRModsHeld", std::move(text), 8.0f);
		}
		s_notice_key = notice_key;
		VR::Netplay::Record record;
		record.serial = StringUtil::toUpper(serial);
		record.crc = crc;
		record.build = BuildVersion::GitRev;
		for (const VR::Mods::Mod* m : s_active)
		{
			const std::string base = fmt::format("{}--v{}", m->slug.empty() ? m->name : m->slug, m->version);
			int n = 1;
			for (const VR::Netplay::ModLine& l : record.mods)
				n += (l.id == base || l.id.rfind(base + "#", 0) == 0) ? 1 : 0;
			record.mods.push_back({n > 1 ? fmt::format("{}#{}", base, n) : base, VR::Mods::RunsInText(*m)});
		}
		VR::Netplay::SetLocalRecord(std::move(record));
		s_armed.assign(s_active.size(), false);
		s_gate_said.assign(s_active.size(), false);
		s_role_said.assign(s_active.size(), false);
		s_find.assign(s_active.size(), FindState{});
		std::vector<const VR::Mods::Vu1Patch*> patches;
		for (const VR::Mods::Mod* m : s_active)
		{
			for (const VR::Mods::Vu1Patch& p : m->vu1)
				patches.push_back(&p);
			Console.WriteLnFmt("(VR) Mods: '{}' applies to {} (CRC {:08X}): {} vu1 patch(es), {} ee word(s). [{}]",
				m->name, serial, crc, m->vu1.size(), m->ee.size(), m->source);
			if (mode == VR::Mods::NetMode::Networked)
			{
				for (const VR::Mods::OnlineApproval& a : m->online_approval)
					Console.WriteLnFmt("(VR) Mods: '{}' is approved for online play on {} by {}. [{}]", m->name, a.service,
						a.by, m->source);
				if (m->both_must_run.value_or(false))
					Console.WarningFmt("(VR) Mods: '{}' must run on BOTH machines (netplay.both_must_run): the mod-list "
									   "exchange logs whether the other side has it; nothing checks it. [{}]",
						m->name, m->source);
			}
		}
		s_tracker.SetPatches(std::move(patches));
	}
}

namespace VR::Mods
{
	namespace
	{
		void noteNetplay(const Mod& m)
		{
			if (NeedsGate(m) && !m.gate.has_value())
			{
				const std::string what = fmt::format("'{}' lists {} but not {} and names no netplay.gate (the game's "
													 "word its code checks)",
					m.name, (m.runs_in & RUNS_LAN) ? "lan" : "online", (m.runs_in & RUNS_LAN) ? "online" : "lan");
				if (!m.user)
					s_issues.push_back(fmt::format("{}: {}", m.source, what));
				else
					Console.WarningFmt("(VR) Mods: {}. [{}]", what, m.source);
			}
			if (NeedsRoleWord(m) && !(m.gate.has_value() && m.gate->role.has_value()))
			{
				const std::string what = fmt::format("'{}' runs on the {} only and names no netplay.gate.role (the game's "
													 "word its code checks)",
					m.name, m.roles == ROLE_HOST ? "host" : "joiner");
				if (!m.user)
					s_issues.push_back(fmt::format("{}: {}", m.source, what));
				else
					Console.WarningFmt("(VR) Mods: {}. [{}]", what, m.source);
			}
			const auto issue = [&m](const std::string& what) {
				if (!m.user)
					s_issues.push_back(fmt::format("{}: {}", m.source, what));
				else
					Console.WarningFmt("(VR) Mods: {}. [{}]", what, m.source);
			};
			if (NeedsBothMustRun(m) && !m.both_must_run.has_value())
				issue(fmt::format("'{}' changes what goes on the wire (effect: changing) in a networked mode and does not "
								  "say netplay.both_must_run: true or false",
					m.name));
#ifndef VR_MODS_PLANTED_BOTH_MAY_SHIP_ON
			if (m.both_must_run.value_or(false) && m.enabled)
				issue(fmt::format("'{}' must run on both machines (netplay.both_must_run: true) and is not enabled: false: "
								  "it ships off until a server can check the other side",
					m.name));
#endif
			if (NeedsOnlineApproval(m) && m.online_approval.empty())
				issue(fmt::format("'{}' lists online and names no netplay.online_approval (the service, and its owner "
								  "who approved the mod)",
					m.name));
			if (m.runs_in_stated)
				return;
			if (!m.user)
				s_issues.push_back(fmt::format("{}: '{}' states no netplay.runs_in: offline only until it does",
					m.source, m.name));
			else
				Console.WriteLnFmt("(VR) Mods: '{}': runs_in not stated: unreviewed, offline only. [{}]", m.name, m.source);
		}

		bool matchAt(const Mod& m, const u8* ram, u32 off)
		{
			const Find& f = m.find.value();
			for (size_t i = 0; i < f.bytes.size(); i++)
			{
				const u8 b = ram[off + i];
				if (!f.mask[i] || b == f.bytes[i])
					continue;
				bool ours = false;
				for (const EeWord& w : m.ee)
				{
					const s64 k = static_cast<s64>(i) - static_cast<s64>(w.address);
					if (k >= 0 && k < 4 && b == static_cast<u8>(w.value >> (8 * k)))
					{
						ours = true;
						break;
					}
				}
				if (!ours)
					return false;
			}
			return true;
		}
	}

	int FindSearch(const Mod& m, const u8* ram, u32 ram_size, u32* at)
	{
		if (!m.find.has_value())
			return 0;
		const Find& f = m.find.value();
		const u64 len = f.bytes.size();
		const u64 hi = f.hi ? std::min<u64>(f.hi, ram_size) : ram_size;
		const u64 lo = (static_cast<u64>(f.lo) + f.align - 1) / f.align * f.align;
		if (len == 0 || hi < len || lo > hi - len)
			return 0;
		int found = 0;
		for (u64 off = lo; off <= hi - len; off += f.align)
		{
			if (matchAt(m, ram, static_cast<u32>(off)))
			{
				if (found++ == 0)
					*at = static_cast<u32>(off);
				else
					return 2;
			}
		}
		return found;
	}

	bool FindHolds(const Mod& m, const u8* ram, u32 ram_size, u32 at)
	{
		return m.find.has_value() && static_cast<u64>(at) + m.find->bytes.size() <= ram_size && matchAt(m, ram, at);
	}

	std::vector<Mod> ParseYAML(std::string_view text, std::string_view source, std::vector<std::string>* errors)
	{
		std::vector<Mod> out;
		Error error;
		const std::string buffer(text);
		std::optional<ryml::Tree> tree =
			ParseYAMLFromString(ryml::to_csubstr(buffer), ryml::csubstr(source.data(), source.size()), &error, true);
		if (!tree.has_value())
		{
			if (errors)
				errors->push_back(fmt::format("{}: {}", source, error.GetDescription()));
			return out;
		}
		const ryml::ConstNodeRef root = tree->crootref();
		if (!root.is_map())
		{
			if (errors)
				errors->push_back(fmt::format("{}: top level is not a serial-keyed map", source));
			return out;
		}
		for (const ryml::ConstNodeRef& serial_node : root.children())
		{
			const std::string_view serial = nodeKey(serial_node);
			if (!serial_node.is_seq())
			{
				if (errors)
					errors->push_back(fmt::format("{}: '{}' is not a list of mods", source, serial));
				continue;
			}
			for (const ryml::ConstNodeRef& m : serial_node.children())
			{
				Mod mod;
				std::string why;
				if (parseMod(serial, m, source, mod, why))
					out.push_back(std::move(mod));
				else if (errors)
					errors->push_back(fmt::format("{}: {}: skipped: {}", source, serial, why));
			}
		}
		return out;
	}

	void Vu1Tracker::Reset()
	{
		m_shadow.fill(0);
	}

	void Vu1Tracker::SetPatches(std::vector<const Vu1Patch*> patches)
	{
		m_patches = std::move(patches);
	}

	void Vu1Tracker::Store(u32 addr, const u8* data, u32 size_bytes)
	{
		addr &= VU1_MICRO_BYTES - 1;
		while (size_bytes > 0)
		{
			const u32 n = std::min(size_bytes, VU1_MICRO_BYTES - addr);
			std::memcpy(m_shadow.data() + addr, data, n);
			data += n;
			size_bytes -= n;
			addr = 0;
		}
	}

	bool Vu1Tracker::Matches(const Vu1Patch& patch) const
	{
		for (const Vu1Signature& s : patch.match)
		{
			if (std::memcmp(m_shadow.data() + s.micro, s.words.data(), s.words.size() * 4) != 0)
				return false;
		}
		return true;
	}

	bool Vu1Tracker::Present(const Vu1Patch& patch) const
	{
		return std::memcmp(m_shadow.data() + patch.micro, patch.words.data(), patch.words.size() * 4) == 0;
	}

	std::vector<Vu1Tracker::Write> Vu1Tracker::OnUpload(u32 addr, const void* data, u32 size_bytes)
	{
		std::vector<Write> due;
		if (m_patches.empty() || size_bytes == 0)
			return due;
		Store(addr, static_cast<const u8*>(data), size_bytes);

		const u32 lo = addr & (VU1_MICRO_BYTES - 1);
		const u32 hi = lo + std::min(size_bytes, VU1_MICRO_BYTES);
		auto touched = [&](u32 a, u32 n) {
			const u32 b = a + n;
			if (a < hi && lo < b)
				return true;
			return hi > VU1_MICRO_BYTES && a < hi - VU1_MICRO_BYTES;
		};
		for (const Vu1Patch* p : m_patches)
		{
			bool relevant = touched(p->micro, static_cast<u32>(p->words.size()) * 4);
			for (const Vu1Signature& s : p->match)
				relevant = relevant || touched(s.micro, static_cast<u32>(s.words.size()) * 4);
			if (relevant && Matches(*p) && !Present(*p))
				due.push_back({p});
		}
		return due;
	}

	void Vu1Tracker::Commit(const Vu1Patch& patch)
	{
		Store(patch.micro, reinterpret_cast<const u8*>(patch.words.data()), static_cast<u32>(patch.words.size()) * 4);
	}

	namespace
	{
		Item* itemOf(std::string_view serial, std::string_view slug)
		{
			for (Item& it : s_items)
				if (it.serial == serial && it.slug == slug)
					return &it;
			return nullptr;
		}

		void refuseItem(Item& it, std::string why)
		{
			if (it.refused)
				return;
			it.refused = true;
			it.reason = why;
			s_issues.push_back(fmt::format("{}: skipped: {}", it.source, why));
			for (Mod& m : s_mods)
				if (m.serial == it.serial && m.slug == it.slug)
					m.enabled = false;
		}

		void setEnabled(const Item& it, bool on)
		{
			for (Mod& m : s_mods)
				if (m.serial == it.serial && m.slug == it.slug)
					m.enabled = on && m.own_enabled;
		}

		std::optional<std::string> wouldCollide(const std::string& serial, const std::vector<std::string>& slugs)
		{
			std::vector<u32> crcs;
			for (const Mod& m : s_mods)
				if (m.serial == serial)
					crcs.insert(crcs.end(), m.crcs.begin(), m.crcs.end());
			u32 unlisted = 0;
			while (std::find(crcs.begin(), crcs.end(), unlisted) != crcs.end())
				unlisted++;
			crcs.push_back(unlisted);
			const auto mine = [&slugs](const Mod* m) { return std::find(slugs.begin(), slugs.end(), m->slug) != slugs.end(); };
			for (const u32 crc : crcs)
			{
				for (const NetMode mode : {NetMode::Offline, NetMode::Networked})
				{
					std::vector<Collision> found;
					Select(serial, crc, mode, &found);
					for (const Collision& c : found)
					{
						if (mine(c.a) && !mine(c.b))
							return c.b->slug;
						if (mine(c.b) && !mine(c.a))
							return c.a->slug;
					}
				}
			}
			return std::nullopt;
		}

		bool pathTo(const std::string& serial, const std::string& from, const std::string& to, std::vector<std::string>& path,
			std::vector<std::string>& seen)
		{
			const Item* it = itemOf(serial, from);
			if (!it)
				return false;
			for (const VR::ModFiles::Dependency& d : it->deps)
			{
				if (d.slug == to)
				{
					path.push_back(d.slug);
					return true;
				}
				if (std::find(seen.begin(), seen.end(), d.slug) != seen.end())
					continue;
				seen.push_back(d.slug);
				path.push_back(d.slug);
				if (pathTo(serial, d.slug, to, path, seen))
					return true;
				path.pop_back();
			}
			return false;
		}

		std::string recordOn(const Item& it)
		{
			const std::optional<std::string> text = FileSystem::ReadFileToString(it.source.c_str());
			const std::optional<std::string> on = text.has_value() ? VR::ModFiles::WithEnabled(text.value(), true) : std::nullopt;
			if (!on.has_value())
				return "not recorded: its enabled: line was not found";
			std::string path = it.source;
			if (!it.user)
			{
				if (EmuFolders::VRMods.empty())
					return "not recorded: no user folder";
				const std::string dir = Path::Combine(EmuFolders::VRMods, it.folder);
				if (!FileSystem::DirectoryExists(dir.c_str()) && !FileSystem::CreateDirectoryPath(dir.c_str(), true))
					return fmt::format("not recorded: {} cannot be made", dir);
				path = Path::Combine(dir, Path::GetFileName(it.source));
			}
			if (!FileSystem::WriteStringToFile(path.c_str(), on.value()))
				return fmt::format("not recorded: {} cannot be written", path);
			return path;
		}

		void resolveDependencies()
		{
			std::vector<std::string> serials;
			for (const Item& it : s_items)
				if (std::find(serials.begin(), serials.end(), it.serial) == serials.end())
					serials.push_back(it.serial);
			std::vector<std::string> screen;
			for (const std::string& serial : serials)
			{
				for (Item& it : s_items)
				{
					if (it.serial != serial)
						continue;
					for (const VR::ModFiles::Dependency& d : it.deps)
						if (!itemOf(serial, d.slug))
							refuseItem(it, fmt::format("needs {} {}, not installed", d.slug, d.range_text));
				}
				for (Item& it : s_items)
				{
					if (it.serial != serial)
						continue;
					std::vector<std::string> path{it.slug}, seen;
					if (pathTo(serial, it.slug, it.slug, path, seen))
					{
						std::string text;
						for (const std::string& p : path)
							text += text.empty() ? p : " -> " + p;
						refuseItem(it, fmt::format("dependency cycle: {}", text));
					}
				}
				for (bool changed = true; changed;)
				{
					changed = false;
					for (Item& it : s_items)
					{
						if (it.serial != serial || it.refused)
							continue;
						for (const VR::ModFiles::Dependency& d : it.deps)
						{
							const Item* dep = itemOf(serial, d.slug);
							if (dep->refused)
								refuseItem(it, fmt::format("needs {} {}, which is refused: {}", d.slug, d.range_text, dep->reason));
							else if (!VR::ModFiles::RangeAllows(d.range, dep->version))
								refuseItem(it, fmt::format("needs {} {}, v{} installed", d.slug, d.range_text, dep->version));
							if (it.refused)
							{
								changed = true;
								break;
							}
						}
					}
					if (changed)
						continue;
					for (Item& it : s_items)
					{
						if (it.serial != serial || it.refused || !it.enabled)
							continue;
						for (const VR::ModFiles::Dependency& d : it.deps)
						{
							std::vector<std::string> closure;
							std::vector<std::string> todo{d.slug};
							while (!todo.empty())
							{
								const std::string slug = todo.back();
								todo.pop_back();
								const Item* dep = itemOf(serial, slug);
								if (!dep || dep->enabled || std::find(closure.begin(), closure.end(), slug) != closure.end())
									continue;
								closure.push_back(slug);
								for (const VR::ModFiles::Dependency& dd : dep->deps)
									todo.push_back(dd.slug);
							}
							if (closure.empty())
								continue;
							for (const std::string& slug : closure)
								setEnabled(*itemOf(serial, slug), true);
							const std::optional<std::string> other = wouldCollide(serial, closure);
							if (other.has_value())
							{
								for (const std::string& slug : closure)
									setEnabled(*itemOf(serial, slug), false);
								refuseItem(it, fmt::format("needs {} {}, which would write the same place as {}, which is on",
									d.slug, d.range_text, other.value()));
								changed = true;
								break;
							}
							for (const std::string& slug : closure)
							{
								Item& dep = *itemOf(serial, slug);
								dep.enabled = true;
								const Item* needer = &it;
								if (slug != d.slug)
								{
									for (const std::string& c : closure)
									{
										const Item* ci = itemOf(serial, c);
										if (std::any_of(ci->deps.begin(), ci->deps.end(), [&slug](const VR::ModFiles::Dependency& x) { return x.slug == slug; }))
										{
											needer = ci;
											break;
										}
									}
								}
								const Item& why = *needer;
								dep.turned_on_for = why.slug;
								const std::string text = fmt::format("Turned on {} v{}: {} v{} needs it", dep.slug, dep.version,
									why.slug, why.version);
								const std::string where = recordOn(dep);
								Console.WriteLnFmt("(VR) Mods: {}. [{}]", text, where);
								s_notes.push_back(text);
								std::string shown = text + ".";
								if (where.rfind("not recorded: ", 0) == 0)
								{
									dep.not_saved = (where == "not recorded: its enabled: line was not found") ?
														std::string(VR::ModFiles::kCannotRewriteEnabled) :
														where.substr(std::strlen("not recorded: "));
									Console.WarningFmt("(VR) Mods: {} v{}: the player's copy not written: {}", dep.slug, dep.version,
										dep.not_saved);
									s_notes.push_back(fmt::format("{} v{}: not saved: {}", dep.slug, dep.version, dep.not_saved));
									shown += fmt::format(" Not saved: {}.", dep.not_saved);
								}
								screen.push_back(std::move(shown));
							}
						}
						if (changed)
							break;
					}
				}
			}
			if (!screen.empty())
			{
				std::string text;
				for (const std::string& t : screen)
					text += text.empty() ? t : " " + t;
				Host::AddKeyedOSDMessage("VRModsTurnedOn", std::move(text), 10.0f);
			}
		}
	}

	void EnsureLoaded()
	{
		if (s_loaded)
			return;
		s_loaded = true;
		std::vector<std::string> notes;
		const std::vector<VR::ModFiles::File> files = VR::ModFiles::Scan(&notes);
		for (const std::string& n : notes)
			Console.WriteLnFmt("(VR) Mods: {}", n);
		s_notes = notes;
		if (files.empty())
		{
			Console.WriteLnFmt("(VR) Mods: no vr-mods files in {} or {}; nothing to apply.",
				Path::Combine(EmuFolders::Resources, VR::ModFiles::kShippedDirName), EmuFolders::VRMods);
			return;
		}
		for (const VR::ModFiles::File& f : files)
		{
			std::string why;
			const std::optional<VR::ModFiles::Name> name = VR::ModFiles::ParseName(f.path, &why);
			if (!name.has_value())
			{
				s_issues.push_back(fmt::format("{}: skipped: {}", f.path, why));
				continue;
			}
			Item item;
			item.serial = StringUtil::toLower(name->serial);
			item.folder = name->serial;
			item.slug = name->slug;
			item.version = name->number;
			item.name = name->slug;
			item.source = f.path;
			item.user = f.user;
			item.note = f.note;
			const auto refuse = [&item](std::string reason) {
				item.refused = true;
				item.reason = std::move(reason);
				s_items.push_back(std::move(item));
			};
			const std::optional<std::string> text = FileSystem::ReadFileToString(f.path.c_str());
			if (!text.has_value())
			{
				s_issues.push_back(fmt::format("{}: unreadable", f.path));
				refuse("the file cannot be read");
				continue;
			}
			Error error;
			std::optional<ryml::Tree> tree = ParseYAMLFromString(ryml::to_csubstr(text.value()), ryml::to_csubstr(f.path), &error, true);
			if (!tree.has_value())
			{
				s_issues.push_back(fmt::format("{}: {}", f.path, error.GetDescription()));
				refuse(error.GetDescription());
				continue;
			}
			const ryml::ConstNodeRef root = tree->crootref();
			if (root.is_map() && root.num_children() == 1)
			{
				const ryml::ConstNodeRef top = root.first_child();
				const ryml::ConstNodeRef said = (top.is_seq() && top.num_children() == 1) ? top.first_child() : top;
				const std::string_view k = (said.is_map() && said.has_child("kind")) ? nodeVal(said["kind"]) : std::string_view();
				item.kind = k == "feature" ? VR::ModFiles::Kind::Feature :
							k == "conversion" ? VR::ModFiles::Kind::Conversion : VR::ModFiles::Kind::Fix;
			}
			const std::optional<VR::ModFiles::Entry> entry = VR::ModFiles::CheckEntry(root, name.value(), &why);
			if (!entry.has_value())
			{
				s_issues.push_back(fmt::format("{}: skipped: {}", f.path, why));
				refuse(why);
				continue;
			}
			const ryml::ConstNodeRef e = VR::ModFiles::EntryNode(root, entry.value());
			item.kind = entry->kind;
			item.enabled = f.carried_enabled.value_or(entry->enabled);
			item.restart_required = entry->restart_required;
			item.deps = entry->dependencies;
			{
				VR::Mods::Origin origin;
				if (!VR::Mods::ParseOrigin(e, origin, why))
				{
					s_issues.push_back(fmt::format("{}: skipped: {}", f.path, why));
					refuse(why);
					continue;
				}
				item.theirs = origin.stated && !origin.ours;
				item.by = origin.by;
			}
			const auto note = [](std::string text) {
				Console.WriteLnFmt("(VR) Mods: {}", text);
				s_notes.push_back(std::move(text));
			};
			const auto noteLongName = [&note, &f](const std::string& n) {
				if (VR::ModFiles::Utf8Chars(n) > VR::ModFiles::kNameMaxChars)
					note(fmt::format("{}: name is longer than {} characters", f.path, VR::ModFiles::kNameMaxChars));
			};
			if (e.has_child("description"))
			{
				const ryml::ConstNodeRef d = e["description"];
				if (d.is_map() || d.is_seq())
					note(fmt::format("{}: description: is not text; the line has no description", f.path));
				else
					item.description = std::string(nodeVal(d));
			}
			if (e.has_child("name"))
			{
				item.name = std::string(nodeVal(e["name"]));
				if (entry->kind != VR::ModFiles::Kind::Conversion)
					noteLongName(item.name);
			}
			if (entry->kind == VR::ModFiles::Kind::Feature)
			{
				s_items.push_back(std::move(item));
				continue;
			}
			if (entry->kind == VR::ModFiles::Kind::Fix)
			{
				Mod mod;
				if (parseMod(name->serial, e, f.path, mod, why))
				{
					mod.version = name->version;
					mod.slug = name->slug;
					mod.user = f.user;
					mod.enabled = item.enabled;
					mod.restart_required = item.restart_required;
					noteNetplay(mod);
					s_mods.push_back(std::move(mod));
					s_items.push_back(std::move(item));
				}
				else
				{
					s_issues.push_back(fmt::format("{}: skipped: {}", f.path, why));
					refuse(why);
				}
				continue;
			}

			const ryml::ConstNodeRef mod_node = e["mod"];
			item.name = std::string(nodeVal(mod_node["name"]));
			noteLongName(item.name);
			item.theirs = true;
			item.by = std::string(nodeVal(mod_node["by"]));
			if (!e.has_child("fixes"))
			{
				s_items.push_back(std::move(item));
				continue;
			}
			std::vector<u32> crcs;
			if (e.has_child("crcs") && e["crcs"].is_seq())
			{
				for (const ryml::ConstNodeRef& c : e["crcs"].children())
				{
					const std::optional<u32> v = parseHex(nodeVal(c));
					if (v.has_value())
						crcs.push_back(v.value());
				}
			}
			if (crcs.empty())
			{
				s_issues.push_back(fmt::format("{}: skipped: a conversion's fixes need the conversion's crcs", f.path));
				refuse("a conversion's fixes need the conversion's crcs");
				continue;
			}
			if (!e["fixes"].is_seq())
			{
				s_issues.push_back(fmt::format("{}: skipped: 'fixes' is not a list", f.path));
				refuse("'fixes' is not a list");
				continue;
			}
			const std::string mod_name(nodeVal(mod_node["name"]));
			const std::string mod_version(nodeVal(mod_node["version"]));
			for (const ryml::ConstNodeRef& fx : e["fixes"].children())
			{
				Mod mod;
				if (!parseMod(name->serial, fx, f.path, mod, why))
				{
					s_issues.push_back(fmt::format("{}: a fix skipped: {}", f.path, why));
					continue;
				}
				mod.crcs = crcs;
				mod.name = fmt::format("{} v{}: {}", mod_name, mod_version, mod.name);
				mod.version = name->version;
				mod.slug = name->slug;
				mod.user = f.user;
				mod.own_enabled = mod.enabled;
				mod.enabled = item.enabled && mod.own_enabled;
				mod.restart_required = item.restart_required;
				if (!mod.runs_in_stated && !ParseNetplay(e, mod, why))
				{
					s_issues.push_back(fmt::format("{}: a fix skipped: {}", f.path, why));
					continue;
				}
				noteNetplay(mod);
				s_mods.push_back(std::move(mod));
			}
			s_items.push_back(std::move(item));
		}
		resolveDependencies();
		s_mods.erase(std::remove_if(s_mods.begin(), s_mods.end(), [](const Mod& m) {
			const Item* it = itemOf(m.serial, m.slug);
			return it != nullptr && it->refused;
		}), s_mods.end());
		std::vector<std::pair<const Mod*, const Mod*>> said;
		for (const Mod& first : s_mods)
		{
			const Mod* const begin = s_mods.data();
			if (std::any_of(begin, &first, [&first](const Mod& m) { return m.serial == first.serial; }))
				continue;
			std::vector<u32> crcs;
			for (const Mod& m : s_mods)
				if (m.serial == first.serial)
					crcs.insert(crcs.end(), m.crcs.begin(), m.crcs.end());
			std::sort(crcs.begin(), crcs.end());
			crcs.erase(std::unique(crcs.begin(), crcs.end()), crcs.end());
			u32 unlisted = 0;
			while (std::find(crcs.begin(), crcs.end(), unlisted) != crcs.end())
				unlisted++;
			crcs.push_back(unlisted);
			for (const u32 crc : crcs)
			{
				for (const NetMode mode : {NetMode::Offline, NetMode::Networked})
				{
					std::vector<Collision> found;
					Select(first.serial, crc, mode, &found);
					for (const Collision& c : found)
					{
						if (std::find(said.begin(), said.end(), std::pair{c.a, c.b}) != said.end())
							continue;
						said.emplace_back(c.a, c.b);
						s_issues.push_back(fmt::format("{}: {} (CRC {:08X}, {})", c.a->source, CollisionText(c), crc,
							mode == NetMode::Offline ? "offline" : "network adapter on"));
					}
				}
			}
		}
		for (const std::string& issue : s_issues)
			Console.WarningFmt("(VR) Mods: {}", issue);
		Console.WriteLnFmt("(VR) Mods: {} fix(es) loaded from {} file(s).", s_mods.size(), files.size());
	}

	void Reset()
	{
		unbind();
		s_mods.clear();
		s_issues.clear();
		s_notes.clear();
		s_items.clear();
		s_loaded = false;
		s_bound_serial.clear();
		s_bound_crc = 0;
		s_vu1_logged.clear();
	}

	const std::vector<Mod>& All()
	{
		return s_mods;
	}

	const std::vector<std::string>& LoadIssues()
	{
		return s_issues;
	}

	namespace
	{
		bool signaturesAgree(const Vu1Patch& p, const Vu1Patch& q)
		{
			for (const Vu1Signature& s : p.match)
			{
				for (const Vu1Signature& t : q.match)
				{
					const u32 lo = std::max(s.micro, t.micro);
					const u32 hi = std::min(s.micro + static_cast<u32>(s.words.size()) * 4, t.micro + static_cast<u32>(t.words.size()) * 4);
					for (u32 b = lo; b < hi; b++)
					{
						if (reinterpret_cast<const u8*>(s.words.data())[b - s.micro] != reinterpret_cast<const u8*>(t.words.data())[b - t.micro])
							return false;
					}
				}
			}
			return true;
		}
	}

	std::vector<Collision> DropCollisions(std::vector<const Mod*>& active)
	{
		std::vector<Collision> out;
#ifndef VR_MODS_PLANTED_OVERLAP_IGNORED
		const auto note = [&](size_t i, size_t j, bool vu1, bool guard, u32 address, u32 n) {
			for (Collision& c : out)
			{
				if (c.a == active[i] && c.b == active[j] && c.vu1 == vu1 && c.guard == guard)
				{
					c.address = std::min(c.address, address);
					c.shared += n;
					return;
				}
			}
			out.push_back({active[i], active[j], vu1, guard, address, n});
		};
		std::unordered_map<u32, std::vector<size_t>> writers;
		for (size_t j = 0; j < active.size(); j++)
		{
			const Mod& m = *active[j];
			if (m.find.has_value())
				continue;
			for (const EeWord& w : m.ee)
			{
				std::vector<size_t>& at = writers[w.address];
				bool mine = false;
				for (const size_t i : at)
				{
					if (active[i]->source == m.source)
						mine = true;
					else
						note(i, j, false, false, w.address, 1);
				}
				if (!mine)
					at.push_back(j);
			}
		}
		for (size_t j = 0; j < active.size(); j++)
		{
			const Mod& m = *active[j];
			if (m.find.has_value())
				continue;
			for (const Guard& g : m.guards)
			{
				const auto it = writers.find(g.address & ~3u);
				if (it == writers.end())
					continue;
				for (const size_t i : it->second)
				{
					if (active[i]->source != m.source)
						note(j, i, false, true, g.address & ~3u, 1);
				}
			}
		}
		for (size_t i = 0; i < active.size(); i++)
		{
			for (size_t j = i + 1; j < active.size(); j++)
			{
				if (active[i]->source == active[j]->source)
					continue;
				for (const Vu1Patch& p : active[i]->vu1)
				{
					for (const Vu1Patch& q : active[j]->vu1)
					{
						const u32 lo = std::max(p.micro, q.micro);
						const u32 hi = std::min(p.micro + static_cast<u32>(p.words.size()) * 4, q.micro + static_cast<u32>(q.words.size()) * 4);
						if (lo < hi && signaturesAgree(p, q))
							note(i, j, true, false, lo, hi - lo);
					}
				}
			}
		}
		const auto refused = [&out](const Mod* m) {
			return std::any_of(out.begin(), out.end(), [m](const Collision& c) { return c.a->source == m->source || c.b->source == m->source; });
		};
		active.erase(std::remove_if(active.begin(), active.end(), refused), active.end());
#else
		(void)active;
#endif
		return out;
	}

	std::string CollisionText(const Collision& c)
	{
		if (c.guard)
			return fmt::format("the guard of '{}' [{}] reads EE word {:#010x} ({} guard(s) on its words) that '{}' [{}] "
							   "writes: neither applies while both are enabled (a guard must read only stock words or its own)",
				c.a->name, c.a->source, c.address, c.shared, c.b->name, c.b->source);
		const std::string what = c.vu1 ? fmt::format("VU1 micro memory at {:#06x} ({} byte(s) in common)", c.address, c.shared) :
										  fmt::format("EE word {:#010x} ({} word(s) in common)", c.address, c.shared);
		return fmt::format("'{}' [{}] and '{}' [{}] both write {}: neither applies while both are enabled (enable one "
						   "version only)",
			c.a->name, c.a->source, c.b->name, c.b->source, what);
	}

	std::vector<const Mod*> Select(std::string_view serial, u32 crc, NetMode mode, std::vector<Collision>* collisions)
	{
		std::vector<const Mod*> out;
		const std::string want = StringUtil::toLower(serial);
		const u8 need = (mode == NetMode::Offline) ? RUNS_OFFLINE : static_cast<u8>(RUNS_LAN | RUNS_ONLINE);
		for (const Mod& m : s_mods)
		{
			if (!m.enabled || m.serial != want)
				continue;
			if (!m.crcs.empty() && std::find(m.crcs.begin(), m.crcs.end(), crc) == m.crcs.end())
				continue;
#ifndef VR_MODS_PLANTED_RUNSIN_IGNORED
			if (!(m.runs_in & need))
				continue;
#else
			(void)need;
#endif
			out.push_back(&m);
		}
		std::vector<Collision> found = DropCollisions(out);
		if (collisions)
			*collisions = std::move(found);
		return out;
	}

	std::vector<const Mod*> Select(std::string_view serial, u32 crc)
	{
		return Select(serial, crc, NetMode::Offline);
	}

	std::string RunsInText(const Mod& m)
	{
		if (!m.runs_in_stated)
			return "unstated";
		std::string out;
		for (const auto& [bit, word] : {std::pair{RUNS_OFFLINE, "offline"}, std::pair{RUNS_LAN, "lan"}, std::pair{RUNS_ONLINE, "online"}})
		{
			if (m.runs_in & bit)
				out += out.empty() ? std::string(word) : fmt::format(",{}", word);
		}
		if (m.roles == ROLE_HOST)
			out += ";host-only";
		else if (m.roles == ROLE_JOINER)
			out += ";joiner-only";
		if (m.both_must_run.value_or(false))
			out += ";both-must-run";
		return out;
	}

	bool ParseOrigin(const ryml::ConstNodeRef& entry, Origin& out, std::string& why)
	{
		out = Origin();
		if (!entry.is_map() || !entry.has_child("origin"))
			return true;
		out.stated = true;
		size_t stated = 0;
		for (const ryml::ConstNodeRef& c : entry.children())
			stated += (c.has_key() && nodeKey(c) == "origin");
		if (stated > 1)
		{
			why = kOriginRepeated;
			return false;
		}
		const ryml::ConstNodeRef o = entry["origin"];
		const auto trimmed = [](std::string_view s) {
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
				s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
				s.remove_suffix(1);
			return s;
		};
		if (o.is_map())
		{
			if (!o.has_child("name") || !o.has_child("by") || trimmed(nodeVal(o["by"])).empty())
			{
				why = kOriginRefusal;
				return false;
			}
			out.by = std::string(nodeVal(o["by"]));
			return true;
		}
		const std::string_view v = o.is_seq() ? std::string_view() : trimmed(nodeVal(o));
		if (v.empty())
		{
			why = kOriginRefusal;
			return false;
		}
		out.ours = (v == kOriginOurs);
		if (!out.ours)
			out.by = std::string(v);
		return true;
	}

	bool ParseNetplay(const ryml::ConstNodeRef& entry, Mod& out, std::string& why)
	{
		if (!entry.is_map() || !entry.has_child("netplay"))
			return true;
		const ryml::ConstNodeRef n = entry["netplay"];
		if (!n.is_map())
		{
			why = "'netplay' is not a map";
			return false;
		}
		if (n.has_child("effect"))
		{
			const std::string e(nodeVal(n["effect"]));
			if (e != "local" && e != "respecting" && e != "changing")
			{
				why = fmt::format("netplay.effect '{}' is not local, respecting or changing", e);
				return false;
			}
			out.effect = e;
		}
		const auto onlyKeys = [&](const ryml::ConstNodeRef& node, std::initializer_list<std::string_view> known, const char* where) {
			for (const ryml::ConstNodeRef& c : node.children())
			{
				const std::string_view k = nodeKey(c);
				if (std::find(known.begin(), known.end(), k) == known.end())
				{
					why = fmt::format("{}: unknown key '{}'", where, k);
					return false;
				}
			}
			return true;
		};
		if (!onlyKeys(n, {"runs_in", "effect", "gate", "roles", "both_must_run", "online_approval"}, "netplay"))
			return false;
		if (n.has_child("gate") && n["gate"].is_map())
		{
			if (!onlyKeys(n["gate"], {"address", "lan", "online", "role"}, "netplay.gate"))
				return false;
			if (n["gate"].has_child("role") && n["gate"]["role"].is_map() &&
				!onlyKeys(n["gate"]["role"], {"address", "pointer", "offset", "host", "joiner"}, "netplay.gate.role"))
				return false;
		}
		if (!n.has_child("runs_in"))
		{
			if (n.has_child("gate") || n.has_child("roles") || n.has_child("both_must_run") || n.has_child("online_approval"))
			{
				why = "netplay.gate, roles, both_must_run and online_approval need netplay.runs_in";
				return false;
			}
			return true;
		}
		const ryml::ConstNodeRef r = n["runs_in"];
		if (!r.is_seq() || r.num_children() == 0)
		{
			why = "netplay.runs_in is a non-empty list of offline, lan and online";
			return false;
		}
		u8 bits = 0;
		for (const ryml::ConstNodeRef& c : r.children())
		{
			const std::string_view w = nodeVal(c);
			if (w == "offline")
				bits |= RUNS_OFFLINE;
			else if (w == "lan")
				bits |= RUNS_LAN;
			else if (w == "online")
				bits |= RUNS_ONLINE;
#ifndef VR_MODS_PLANTED_RUNSIN_LENIENT
			else
			{
				why = fmt::format("netplay.runs_in: '{}' is not offline, lan or online", w);
				return false;
			}
#endif
		}
		out.runs_in = bits;
		out.runs_in_stated = true;
		if (n.has_child("gate"))
		{
			const ryml::ConstNodeRef g = n["gate"];
			const auto hex = [&](const char* k) { return g.is_map() && g.has_child(ryml::to_csubstr(k)) ? parseHex(nodeVal(g[ryml::to_csubstr(k)])) : std::nullopt; };
			const std::optional<u32> at = hex("address"), lan = hex("lan"), online = hex("online");
			if (!at.has_value() || !lan.has_value() || !online.has_value())
			{
				why = "netplay.gate needs hex 'address', 'lan' and 'online'";
				return false;
			}
			if (at.value() % 4 || static_cast<u64>(at.value()) + 4 > EE_MAIN_RAM_BYTES || lan.value() == online.value())
			{
				why = fmt::format("netplay.gate: {:#x} is not an aligned EE main-RAM word, or lan and online are equal",
					at.value());
				return false;
			}
			out.gate = NetGate{at.value(), lan.value(), online.value(), std::nullopt};
			if (g.has_child("role"))
			{
				const ryml::ConstNodeRef rw = g["role"];
				const auto h = [&](const char* k) { return rw.is_map() && rw.has_child(ryml::to_csubstr(k)) ? parseHex(nodeVal(rw[ryml::to_csubstr(k)])) : std::nullopt; };
				RoleWord w;
				const std::optional<u32> host = h("host"), joiner = h("joiner"), addr = h("address"), ptr = h("pointer"),
										 off = h("offset");
				w.via_pointer = ptr.has_value();
				const bool placed = w.via_pointer ? (!addr.has_value() && off.has_value()) : addr.has_value();
				if (!host.has_value() || !joiner.has_value() || host.value() == joiner.value() || !placed)
				{
					why = "netplay.gate.role needs hex 'host' and 'joiner' (different) and either 'address' or "
						  "'pointer' + 'offset'";
					return false;
				}
				w.host = host.value();
				w.joiner = joiner.value();
				w.address = addr.value_or(0);
				w.pointer = ptr.value_or(0);
				w.offset = off.value_or(0);
				const u32 fixed = w.via_pointer ? w.pointer : w.address;
				if (fixed % 4 || static_cast<u64>(fixed) + 4 > EE_MAIN_RAM_BYTES || w.offset % 4)
				{
					why = fmt::format("netplay.gate.role: {:#x} is not an aligned EE main-RAM word (or the offset is not "
									  "word-aligned)",
						fixed);
					return false;
				}
				out.gate->role = w;
			}
		}
		if (n.has_child("roles"))
		{
			const ryml::ConstNodeRef r2 = n["roles"];
			if (!r2.is_seq() || r2.num_children() == 0)
			{
				why = "netplay.roles is a non-empty list of host and joiner";
				return false;
			}
			u8 rb = 0;
			for (const ryml::ConstNodeRef& c : r2.children())
			{
				const std::string_view w = nodeVal(c);
				if (w == "host")
					rb |= ROLE_HOST;
				else if (w == "joiner")
					rb |= ROLE_JOINER;
				else
				{
					why = fmt::format("netplay.roles: '{}' is not host or joiner", w);
					return false;
				}
			}
			out.roles = (rb == (ROLE_HOST | ROLE_JOINER)) ? 0 : rb;
		}
		if (n.has_child("both_must_run"))
		{
			const std::string_view v = nodeVal(n["both_must_run"]);
			if (v != "true" && v != "false")
			{
				why = fmt::format("netplay.both_must_run '{}' is not true or false", v);
				return false;
			}
			out.both_must_run = (v == "true");
		}
		if (n.has_child("online_approval"))
		{
			const ryml::ConstNodeRef a = n["online_approval"];
			if (!(out.runs_in & RUNS_ONLINE) || !a.is_seq() || a.num_children() == 0)
			{
				why = "netplay.online_approval is a non-empty list of {service, by}, for a mod whose runs_in lists online";
				return false;
			}
			for (const ryml::ConstNodeRef& c : a.children())
			{
				if (!c.is_map() || !onlyKeys(c, {"service", "by"}, "netplay.online_approval"))
				{
					if (why.empty())
						why = "netplay.online_approval: each entry is a map {service, by}";
					return false;
				}
				OnlineApproval ap;
				ap.service = c.has_child("service") ? std::string(nodeVal(c["service"])) : std::string();
				ap.by = c.has_child("by") ? std::string(nodeVal(c["by"])) : std::string();
				if (ap.service.empty() || ap.by.empty())
				{
					why = "netplay.online_approval: each entry names a 'service' and who approved it ('by')";
					return false;
				}
				out.online_approval.push_back(std::move(ap));
			}
		}
		return true;
	}

	bool NeedsBothMustRun(const Mod& m)
	{
#ifdef VR_MODS_PLANTED_BOTH_NOT_REQUIRED
		return false;
#endif
		return m.runs_in_stated && m.effect == "changing" && (m.runs_in & (RUNS_LAN | RUNS_ONLINE)) != 0;
	}

	bool NeedsOnlineApproval(const Mod& m)
	{
#ifdef VR_MODS_PLANTED_APPROVAL_NOT_REQUIRED
		return false;
#endif
		return m.runs_in_stated && (m.runs_in & RUNS_ONLINE) != 0 && m.effect != "local";
	}

	bool NeedsRoleWord(const Mod& m)
	{
#ifdef VR_MODS_PLANTED_ROLE_NOT_REQUIRED
		return false;
#endif
		return m.roles == ROLE_HOST || m.roles == ROLE_JOINER;
	}

	bool NeedsGate(const Mod& m)
	{
#ifdef VR_MODS_PLANTED_GATE_NOT_REQUIRED
		return false;
#endif
		return m.runs_in_stated && (((m.runs_in & RUNS_LAN) != 0) != ((m.runs_in & RUNS_ONLINE) != 0));
	}

	void OnVsync()
	{
		if (!enabledByEnvironment())
			return;
		EnsureLoaded();
		if (s_mods.empty())
			return;

		const u32 crc = VMManager::Internal::HasBootedELF() ? VMManager::GetCurrentCRC() : 0;
		const std::string serial = StringUtil::toLower(VMManager::GetDiscSerial());
		if (crc == 0)
		{
			if (s_bound)
				unbind();
			return;
		}
		if (!s_bound || crc != s_bound_crc || serial != s_bound_serial || EmuConfig.DEV9.EthEnable != s_bound_eth)
			bind(serial, crc);
		noteSettings();

		for (size_t i = 0; i < s_active.size(); i++)
		{
			const Mod& m = *s_active[i];
			if (m.gate.has_value() && NeedsGate(m) && !s_gate_said[i])
			{
				const u32 v = memRead32(m.gate->address);
				const bool lan_only = (m.runs_in & RUNS_LAN) != 0;
				if (v == (lan_only ? m.gate->online : m.gate->lan))
				{
					s_gate_said[i] = true;
					Console.WarningFmt("(VR) Mods: '{}': the game's word {:#010x} = {:#x} says {} play, which its runs_in ({}) "
									   "does not list; it relies on its own code to stand down. [{}]",
						m.name, m.gate->address, v, lan_only ? "online" : "LAN", RunsInText(m), m.source);
				}
			}
			if (m.gate.has_value() && m.gate->role.has_value() && NeedsRoleWord(m) && !s_role_said[i])
			{
				const RoleWord& w = m.gate->role.value();
				std::optional<u32> at;
				if (!w.via_pointer)
					at = w.address;
				else
				{
					const u32 p = memRead32(w.pointer) & 0x1FFFFFFFu;
					if (p != 0 && static_cast<u64>(p) + w.offset + 4 <= EE_MAIN_RAM_BYTES)
						at = p + w.offset;
				}
				if (at.has_value())
				{
					const u32 v = memRead32(at.value());
					const bool host_only = m.roles == ROLE_HOST;
					if (v == (host_only ? w.joiner : w.host))
					{
						s_role_said[i] = true;
						Console.WarningFmt("(VR) Mods: '{}': the game's role word {:#010x} = {:#x} says this machine is the {}, "
										   "which its netplay.roles ({}) excludes; it relies on its own code to stand down. [{}]",
							m.name, at.value(), v, host_only ? "joiner" : "host", host_only ? "host" : "joiner", m.source);
					}
				}
			}
			if (m.ee.empty())
				continue;
			u32 base = 0;
			if (m.find.has_value())
			{
				FindState& fs = s_find[i];
				const u8* ram = eeMem->Main;
				if (fs.refused)
					continue;
				if (fs.anchor.has_value() && !FindHolds(m, ram, EE_MAIN_RAM_BYTES, fs.anchor.value()))
				{
					Console.WriteLnFmt("(VR) Mods: '{}': the pattern no longer holds at {:#010x}: searching again.",
						m.name, fs.anchor.value());
					fs.anchor.reset();
					fs.throttle = 0;
					continue;
				}
				if (!fs.anchor.has_value())
				{
					if (fs.throttle > 0)
					{
						fs.throttle--;
						continue;
					}
					u32 at = 0;
					const int k = FindSearch(m, ram, EE_MAIN_RAM_BYTES, &at);
					if (k == 2)
					{
						fs.refused = true;
						Console.WarningFmt("(VR) Mods: '{}': the pattern matches more than once: refused for this "
										   "boot. [{}]",
							m.name, m.source);
						continue;
					}
					if (k == 0)
					{
						if (!fs.waiting_said)
						{
							Console.WriteLnFmt("(VR) Mods: '{}': the pattern is not in RAM yet: searching again every {} "
											   "vsyncs.",
								m.name, FIND_SEARCH_PERIOD);
							fs.waiting_said = true;
						}
						fs.throttle = FIND_SEARCH_PERIOD;
						continue;
					}
					fs.anchor = at;
					Console.WriteLnFmt("(VR) Mods: '{}': found at {:#010x}.", m.name, at);
				}
				base = fs.anchor.value();
			}
			if (!guardsPass(m, base))
				continue;
			bool writable = true;
			for (const EeWord& w : m.ee)
			{
				const u64 at = static_cast<u64>(base) + w.address;
				if (at + 4 > EE_MAIN_RAM_BYTES)
				{
					writable = false;
					break;
				}
				const u32 now = memRead32(static_cast<u32>(at));
				if (w.relative && now != w.original.value() && now != w.value)
				{
					writable = false;
					break;
				}
			}
			if (!writable)
				continue;
			if (!s_armed[i])
			{
				s_armed[i] = true;
				Console.WriteLnFmt("(VR) Mods: '{}' armed: guards hold, {} ee word(s) asserted every vsync{}.", m.name,
					m.ee.size(), m.find.has_value() ? " at the anchor" : "");
			}
			for (const EeWord& w : m.ee)
			{
				const u32 at = base + w.address;
				if (memRead32(at) != w.value)
					memWrite32(at, w.value);
			}
		}
	}

	void OnVU1Upload(u32 addr, const void* data, u32 size_bytes)
	{
		if (!s_tracker.HasPatches())
			return;
		for (const Vu1Tracker::Write& w : s_tracker.OnUpload(addr, data, size_bytes))
		{
			writeMicro(*w.patch);
			s_tracker.Commit(*w.patch);
			if (std::find(s_vu1_logged.begin(), s_vu1_logged.end(), w.patch) == s_vu1_logged.end())
			{
				s_vu1_logged.push_back(w.patch);
				Console.WriteLnFmt("(VR) Mods: vu1 patch written: {} instruction(s) at micro {:#06x} (the program's "
								   "signatures matched an upload).",
					w.patch->words.size() / 2, w.patch->micro);
			}
		}
	}

	void OnStateLoaded()
	{
		s_tracker.Reset();
		s_bound = false;

		if (!enabledByEnvironment())
			return;
		EnsureLoaded();
		const std::string serial = StringUtil::toLower(VMManager::GetDiscSerial());
		if (serial.empty() || !eeMem)
			return;
		if (THREAD_VU1)
			vu1Thread.WaitVU();
		std::string said;
		for (const Mod& m : s_mods)
		{
			if (m.enabled || m.serial != serial)
				continue;
			if (!CodeInMemory(m, eeMem->Main, EE_MAIN_RAM_BYTES, VU1.Micro, VU1_MICRO_BYTES))
				continue;
			Console.WarningFmt("(VR) Mods: this save state was made with '{}' on ({} v{}, now off): its code is in memory and "
							   "runs until the game is rebooted. [{}]",
				m.name, m.slug, m.version, m.source);
			const std::string id = fmt::format("{} v{}", m.slug, m.version);
			if (said.find(id) == std::string::npos)
				said += said.empty() ? id : ", " + id;
		}
		if (!said.empty())
			Host::AddKeyedOSDMessage("VRModsStateOff",
				fmt::format("This save state was made with {} on. It runs until you reboot the game.", said), 12.0f);
	}

	bool CodeInMemory(const Mod& m, const u8* ram, u32 ram_size, const u8* micro, u32 micro_size)
	{
		if (ram && !m.ee.empty())
		{
			u32 base = 0;
			bool placed = true;
			if (m.find.has_value())
			{
				u32 at = 0;
				placed = FindSearch(m, ram, ram_size, &at) == 1;
				base = at;
			}
			if (placed && std::all_of(m.ee.begin(), m.ee.end(), [&](const EeWord& w) {
					const u64 at = static_cast<u64>(base) + w.address;
					u32 now = 0;
					if (at + 4 > ram_size)
						return false;
					std::memcpy(&now, ram + at, 4);
					return now == w.value;
				}))
				return true;
		}
		if (micro)
		{
			for (const Vu1Patch& p : m.vu1)
			{
				const u64 bytes = static_cast<u64>(p.words.size()) * 4;
				if (p.micro + bytes <= micro_size && std::memcmp(micro + p.micro, p.words.data(), bytes) == 0)
					return true;
			}
		}
		return false;
	}

	const std::vector<std::string>& LoadNotes()
	{
		return s_notes;
	}

	LineState StateOf(const LiveFacts& f, std::string* why)
	{
		const auto with = [why](LineState s, std::string text) {
			if (why)
				*why = std::move(text);
			return s;
		};
		if (f.refused)
			return with(LineState::Refused, f.refused_why);
		if (!f.enabled)
			return with(LineState::Off, {});
		if (f.feature)
			return with(LineState::Active, {});
		if (!f.held_why.empty())
			return with(LineState::Refused, f.held_why);
		if (!f.not_here.empty())
			return with(LineState::NotHere, f.not_here);
		if (!f.bound)
			return with(LineState::Waiting, "the game is not running");
		if (!f.applies)
			return with(LineState::Waiting, "the game is not bound yet");
		if (f.has_find && !f.anchor_found)
			return with(LineState::Waiting, "its anchor is not found yet");
		if (f.has_ee && !f.armed)
			return with(LineState::Waiting, "its guards are not met");
		if (f.has_vu1 && !f.vu1_present)
			return with(LineState::Waiting, "waiting for the game to load that code");
		return with(LineState::Active, {});
	}

	std::vector<Line> Lines(std::string_view serial, u32 crc)
	{
		std::vector<Line> out;
		const std::string want = StringUtil::toLower(serial);
		const bool bound_here = s_bound && s_bound_serial == want && s_bound_crc == crc;
		const NetMode mode = (bound_here && s_bound_eth) ? NetMode::Networked : NetMode::Offline;
		std::vector<Collision> collisions;
		const std::vector<const Mod*> on = Select(want, crc, mode, &collisions);
		for (const Item& it : s_items)
		{
			if (it.serial != want)
				continue;
			Line l;
			l.slug = it.slug;
			l.version = it.version;
			l.name = it.name;
			l.description = it.description;
			l.kind = static_cast<u8>(it.kind);
			l.theirs = it.theirs;
			l.by = it.by;
			l.restart_required = it.restart_required;
			l.enabled = it.enabled;
			l.note = it.note;
			l.turned_on_for = it.turned_on_for;
			if (!it.turned_on_for.empty())
				l.note = l.note.empty() ? fmt::format("on, turned on for {}", it.turned_on_for) :
										  fmt::format("{}; on, turned on for {}", l.note, it.turned_on_for);
			if (!it.not_saved.empty())
				l.note += fmt::format("; not saved: {}", it.not_saved);
			for (const VR::ModFiles::Dependency& d : it.deps)
				l.dependencies.emplace_back(d.slug, d.range_text);
			l.source = it.source;
			l.user = it.user;

			LiveFacts f;
			f.refused = it.refused;
			f.refused_why = it.reason;
			f.enabled = it.enabled;
			f.feature = it.kind == VR::ModFiles::Kind::Feature;
			f.bound = bound_here;
			bool any = false, in_set = false, crc_ok = false;
			for (size_t k = 0; k < s_mods.size(); k++)
			{
				const Mod& m = s_mods[k];
				if (m.serial != want || m.slug != it.slug)
					continue;
				any = true;
				f.has_ee |= !m.ee.empty();
				f.has_vu1 |= !m.vu1.empty();
				f.has_find |= m.find.has_value();
				crc_ok |= m.crcs.empty() || std::find(m.crcs.begin(), m.crcs.end(), crc) != m.crcs.end();
				in_set |= std::find(on.begin(), on.end(), &m) != on.end();
				for (const Collision& c : collisions)
					if (f.held_why.empty() && (c.a == &m || c.b == &m))
						f.held_why = CollisionText(c);
				if (!bound_here)
					continue;
				const auto a = std::find(s_active.begin(), s_active.end(), &m);
				if (a == s_active.end())
					continue;
				const size_t i = static_cast<size_t>(a - s_active.begin());
				f.applies = true;
				f.armed = i < s_armed.size() && s_armed[i];
				f.anchor_found = i < s_find.size() && s_find[i].anchor.has_value();
				for (const Vu1Patch& p : m.vu1)
					f.vu1_present |= s_tracker.Present(p);
			}
			if (any && !f.refused && f.enabled && f.held_why.empty() && !in_set && (crc != 0 || bound_here))
			{
				for (const Mod& m : s_mods)
				{
					if (m.serial == want && m.slug == it.slug && f.not_here.empty())
						f.not_here = crc_ok ? NotHereReason(m, 0, mode) : std::string("Not for this disc");
				}
			}
			l.state = StateOf(f, &l.reason);
			if (it.kind == VR::ModFiles::Kind::Conversion && !it.refused && crc != 0)
			{
				const VR::ProfileDB::Profile* p = VR::ProfileDB::Lookup(want, crc);
				l.vr_profile_binds = p && p->mod.has_value() && p->source_path == it.source;
				if (l.vr_profile_binds && !it.enabled)
					l.reason = "fixes off, VR settings still apply";
			}
			out.push_back(std::move(l));
		}
		return out;
	}

	std::vector<Mod> ModsOf(std::string_view serial)
	{
		const std::string want = StringUtil::toLower(serial);
		std::vector<Mod> out;
		for (const Mod& m : s_mods)
			if (m.serial == want)
				out.push_back(m);
		return out;
	}

	std::string CollidesWith(const std::vector<Mod>& mods, std::string_view slug, const std::vector<std::string>& on)
	{
		std::vector<const Mod*> set;
		for (const Mod& m : mods)
			if (m.slug == slug || std::find(on.begin(), on.end(), m.slug) != on.end())
				set.push_back(&m);
		if (std::none_of(set.begin(), set.end(), [slug](const Mod* m) { return m->slug == slug; }))
			return {};
		std::vector<u32> crcs;
		for (const Mod* m : set)
			for (const u32 c : m->crcs)
				if (std::find(crcs.begin(), crcs.end(), c) == crcs.end())
					crcs.push_back(c);
		u32 unlisted = 1;
		while (std::find(crcs.begin(), crcs.end(), unlisted) != crcs.end())
			unlisted++;
		crcs.push_back(unlisted);
		for (const u32 crc : crcs)
		{
			for (const u8 need : {static_cast<u8>(RUNS_OFFLINE), static_cast<u8>(RUNS_LAN | RUNS_ONLINE)})
			{
				std::vector<const Mod*> active;
				for (const Mod* m : set)
					if ((m->crcs.empty() || std::find(m->crcs.begin(), m->crcs.end(), crc) != m->crcs.end()) && (m->runs_in & need))
						active.push_back(m);
				for (const Collision& c : DropCollisions(active))
				{
					if (c.a->slug == slug && c.b->slug != slug)
						return c.b->slug;
					if (c.b->slug == slug && c.a->slug != slug)
						return c.a->slug;
				}
			}
		}
		return {};
	}

	std::vector<Line> Lines()
	{
		const u32 crc = VMManager::Internal::HasBootedELF() ? VMManager::GetCurrentCRC() : 0;
		return Lines(VMManager::GetDiscSerial(), crc);
	}

	bool IsOn(std::string_view serial, std::string_view slug)
	{
		const Item* it = itemOf(StringUtil::toLower(serial), slug);
		return it != nullptr && !it->refused && it->enabled;
	}

	bool IsOn(std::string_view slug)
	{
		return IsOn(s_bound ? s_bound_serial : StringUtil::toLower(VMManager::GetDiscSerial()), slug);
	}

	std::string NotHereReason(const Mod& m, u32 crc, NetMode mode)
	{
		if (crc != 0 && !m.crcs.empty() && std::find(m.crcs.begin(), m.crcs.end(), crc) == m.crcs.end())
			return "Not for this disc";
		const u8 need = (mode == NetMode::Offline) ? RUNS_OFFLINE : static_cast<u8>(RUNS_LAN | RUNS_ONLINE);
		if (!(m.runs_in & need))
			return mode == NetMode::Offline ? "Not in offline play" : "Not in LAN or online play";
		return {};
	}
}
