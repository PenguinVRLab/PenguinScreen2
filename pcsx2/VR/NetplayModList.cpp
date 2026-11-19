// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VR/NetplayModList.h"

#include "fmt/format.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>

namespace VR::Netplay
{
	namespace
	{
		std::mutex s_local_mutex;
		Record s_local;
		std::vector<std::pair<std::string, std::string>> s_local_settings;
		std::vector<std::string> s_local_patches;
		std::vector<std::string> s_local_cheats;

		bool Digits(const std::string& a)
		{
			return !a.empty() && a.size() <= 6 && std::all_of(a.begin(), a.end(), [](char c) { return c >= '0' && c <= '9'; });
		}

		void CompareNames(const char* kind, const std::vector<std::string>& ours, const std::vector<std::string>& theirs,
			std::vector<std::string>* d)
		{
			std::vector<std::string> names = ours;
			names.insert(names.end(), theirs.begin(), theirs.end());
			std::sort(names.begin(), names.end());
			names.erase(std::unique(names.begin(), names.end()), names.end());
			for (const std::string& n : names)
			{
				const auto a = std::count(ours.begin(), ours.end(), n), b = std::count(theirs.begin(), theirs.end(), n);
				if (a != b)
					d->push_back(b == 0 ? fmt::format("{} only ours: {}", kind, n) :
							a == 0      ? fmt::format("{} only theirs: {}", kind, n) :
										  fmt::format("{} {}: {} on our side, {} on theirs", kind, n, a, b));
			}
		}

		constexpr size_t kHeader = 14;
		constexpr size_t kMinFrame = 60;

		std::string Field(std::string_view s)
		{
			std::string out;
			for (const char c : s)
				out.push_back((c == ' ' || c == '\n' || c == '\r' || c == '\t') ? '_' : c);
			return out.empty() ? std::string("-") : out;
		}

		std::string Clean(std::string_view s)
		{
			std::string out;
			for (const char c : s.substr(0, kMaxField))
				out.push_back((c >= 0x21 && c <= 0x7E) ? c : '?');
			return out;
		}

		bool Hex32(const std::string& s, u32* out)
		{
			if (s.empty() || s.size() > 8)
				return false;
			for (const char c : s)
			{
				if (!std::isxdigit(static_cast<unsigned char>(c)))
					return false;
			}
			*out = static_cast<u32>(std::strtoul(s.c_str(), nullptr, 16));
			return true;
		}
	}

	std::string Encode(const Record& r)
	{
		std::string out = fmt::format("format {}\nserial {}\ncrc {:08X}\nbuild {}\nsession {}\nseen {}\n", kFormat,
			Field(r.serial), r.crc, Field(r.build), Field(r.session), Field(r.seen));
		for (const ModLine& m : r.mods)
			out += fmt::format("mod {} {}\n", Field(m.id), Field(m.runs_in));
		if (r.mods_cut)
			out += fmt::format("mods_cut {}\n", r.mods_cut);
		for (const auto& [k, v] : r.settings)
			out += fmt::format("setting {} {}\n", Field(k), Field(v));
		for (const std::string& p : r.patches)
			out += fmt::format("patch {}\n", Field(p));
		for (const std::string& c : r.cheats)
			out += fmt::format("cheat {}\n", Field(c));
		if (r.extras_cut)
			out += fmt::format("extras_cut {}\n", r.extras_cut);
		return out;
	}

	bool Decode(std::string_view text, Record* out)
	{
		Record r;
		bool have_serial = false, have_crc = false, have_format = false;
		std::istringstream in{std::string(text)};
		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty())
				continue;
			std::istringstream cols(line);
			std::string key, a, b, extra;
			cols >> key >> a >> b >> extra;
			if (key == "format")
			{
				have_format = (a == std::to_string(kFormat)) && b.empty();
				if (!have_format)
					return false;
			}
			else if (key == "serial")
			{
				r.serial = Clean(a);
				have_serial = !a.empty();
			}
			else if (key == "crc")
			{
				if (!Hex32(a, &r.crc) || !b.empty())
					return false;
				have_crc = true;
			}
			else if (key == "build")
				r.build = Clean(a);
			else if (key == "session")
				r.session = Clean(a);
			else if (key == "seen")
				r.seen = Clean(a);
			else if (key == "mod")
			{
				if (a.empty() || b.empty() || !extra.empty())
					return false;
				r.mods.push_back({Clean(a), Clean(b)});
			}
			else if (key == "mods_cut")
			{
				if (!Digits(a))
					return false;
				r.mods_cut = static_cast<u32>(std::strtoul(a.c_str(), nullptr, 10));
			}
			else if (key == "setting")
			{
				if (a.empty() || b.empty() || !extra.empty())
					return false;
				r.settings.emplace_back(Clean(a), Clean(b));
			}
			else if (key == "patch" || key == "cheat")
			{
				if (a.empty() || !b.empty())
					return false;
				(key == "patch" ? r.patches : r.cheats).push_back(Clean(a));
			}
			else if (key == "extras_cut")
			{
				if (!Digits(a))
					return false;
				r.extras_cut = static_cast<u32>(std::strtoul(a.c_str(), nullptr, 10));
			}
		}
		if (!have_format || !have_serial || !have_crc)
			return false;
		*out = std::move(r);
		return true;
	}

	std::vector<u8> BuildFrame(const MAC& dst, const MAC& src, const Record& r)
	{
		std::vector<u8> f;
		f.insert(f.end(), dst.begin(), dst.end());
		f.insert(f.end(), src.begin(), src.end());
		f.push_back(static_cast<u8>(kEtherType >> 8));
		f.push_back(static_cast<u8>(kEtherType & 0xFF));
		f.insert(f.end(), std::begin(kMagic), std::end(kMagic));
		const size_t room = kMaxFrame - f.size();
		Record fit = r;
		fit.serial = fit.serial.substr(0, kMaxField);
		fit.build = fit.build.substr(0, kMaxField);
		for (ModLine& m : fit.mods)
		{
			m.id = m.id.substr(0, kMaxField);
			m.runs_in = m.runs_in.substr(0, kMaxField);
		}
		for (auto& [k, v] : fit.settings)
		{
			k = k.substr(0, kMaxField);
			v = v.substr(0, kMaxField);
		}
		for (std::string& p : fit.patches)
			p = p.substr(0, kMaxField);
		for (std::string& c : fit.cheats)
			c = c.substr(0, kMaxField);
		std::string body = Encode(fit);
#ifndef VR_NETPLAY_PLANTED_CUT_MODS_FIRST
		while (body.size() > room && (!fit.cheats.empty() || !fit.patches.empty()))
		{
			(fit.cheats.empty() ? fit.patches : fit.cheats).pop_back();
			fit.extras_cut++;
			body = Encode(fit);
		}
#endif
		while (body.size() > room && !fit.mods.empty())
		{
			fit.mods.pop_back();
			fit.mods_cut++;
			body = Encode(fit);
		}
		f.insert(f.end(), body.begin(), body.begin() + std::min(body.size(), room));
		if (f.size() < kMinFrame)
			f.resize(kMinFrame, 0);
		return f;
	}

	bool IsRecordFrame(const u8* frame, size_t size)
	{
#ifdef VR_NETPLAY_PLANTED_NO_STRIP
		return false;
#endif
		if (!frame || size < kHeader + sizeof(kMagic))
			return false;
		const u16 type = static_cast<u16>((frame[12] << 8) | frame[13]);
		return type == kEtherType && std::memcmp(frame + kHeader, kMagic, sizeof(kMagic)) == 0;
	}

	bool ParseFrame(const u8* frame, size_t size, MAC* src, Record* out)
	{
		if (!IsRecordFrame(frame, size))
			return false;
		const size_t at = kHeader + sizeof(kMagic);
		std::string_view text(reinterpret_cast<const char*>(frame + at), size - at);
		const size_t nul = text.find('\0');
		if (nul != std::string_view::npos)
			text = text.substr(0, nul);
		if (src)
			std::copy(frame + 6, frame + 12, src->begin());
		return Decode(text, out);
	}

	bool IsUnicast(const MAC& m)
	{
#ifdef VR_NETPLAY_PLANTED_BROADCAST
		return true;
#endif
		return (m[0] & 0x01) == 0;
	}

	std::vector<std::string> Compare(const Record& ours, const Record& theirs)
	{
		std::vector<std::string> d;
		const auto unbound = [](const Record& r) { return r.serial.empty() || r.serial == "-"; };
		if (unbound(ours) || unbound(theirs))
			d.push_back(fmt::format("no game bound yet on {} side", unbound(ours) ? (unbound(theirs) ? "either" : "our") : "their"));
		else if (ours.serial != theirs.serial || ours.crc != theirs.crc)
			d.push_back(fmt::format("different game: ours {} CRC {:08X}, theirs {} CRC {:08X}", ours.serial, ours.crc,
				theirs.serial, theirs.crc));
		if (ours.mods_cut || theirs.mods_cut)
			d.push_back(fmt::format("a list was cut to fit one frame: ours {} left out, theirs {}", ours.mods_cut, theirs.mods_cut));
		if (ours.build != theirs.build)
			d.push_back(fmt::format("different build: ours {}, theirs {}", ours.build, theirs.build));
		std::vector<std::string> ids;
		for (const ModLine& m : ours.mods)
			ids.push_back(m.id);
		for (const ModLine& t : theirs.mods)
			ids.push_back(t.id);
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		for (const std::string& id : ids)
		{
			std::vector<const ModLine*> a, b;
			for (const ModLine& m : ours.mods)
				if (m.id == id)
					a.push_back(&m);
			for (const ModLine& t : theirs.mods)
				if (t.id == id)
					b.push_back(&t);
#ifndef VR_NETPLAY_PLANTED_BOTH_UNNAMED
			const auto both = [](const ModLine* l) { return l->runs_in.find("both-must-run") != std::string::npos; };
#else
			const auto both = [](const ModLine*) { return false; };
#endif
			if (b.empty())
				d.push_back(fmt::format("{}only ours: {} (runs_in {})",
					both(a[0]) ? "MUST RUN ON BOTH MACHINES, " : "", id, a[0]->runs_in));
			else if (a.empty())
				d.push_back(fmt::format("{}only theirs: {} (runs_in {})",
					both(b[0]) ? "MUST RUN ON BOTH MACHINES, " : "", id, b[0]->runs_in));
			else if (a.size() != b.size())
				d.push_back(fmt::format("{}: {} on our side, {} on theirs", id, a.size(), b.size()));
			else
			{
				for (size_t i = 0; i < a.size(); i++)
				{
					if (a[i]->runs_in != b[i]->runs_in)
						d.push_back(fmt::format("runs_in differs for {}: ours {}, theirs {}", id, a[i]->runs_in, b[i]->runs_in));
				}
			}
		}
		if (ours.extras_cut || theirs.extras_cut)
			d.push_back(fmt::format("patches and cheats were cut to fit one frame: ours {} left out, theirs {}",
				ours.extras_cut, theirs.extras_cut));
#ifndef VR_NETPLAY_PLANTED_NO_SETTINGS_COMPARE
		if (ours.settings.empty() != theirs.settings.empty())
			d.push_back(fmt::format("no settings from {} side (an older build): timing settings not compared",
				ours.settings.empty() ? "our" : "their"));
		else
		{
			std::vector<std::string> keys;
			for (const auto& kv : ours.settings)
				keys.push_back(kv.first);
			for (const auto& kv : theirs.settings)
				keys.push_back(kv.first);
			std::sort(keys.begin(), keys.end());
			keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
			const auto value = [](const Record& r, const std::string& k) {
				for (const auto& kv : r.settings)
					if (kv.first == k)
						return kv.second;
				return std::string("(not sent)");
			};
			for (const std::string& k : keys)
			{
				if (value(ours, k) != value(theirs, k))
					d.push_back(fmt::format("setting {} differs: ours {}, theirs {}", k, value(ours, k), value(theirs, k)));
			}
		}
#endif
		CompareNames("patch", ours.patches, theirs.patches, &d);
		CompareNames("cheat", ours.cheats, theirs.cheats, &d);
		return d;
	}

	std::string NewSession()
	{
		std::random_device rd;
		const u64 v = (static_cast<u64>(rd()) << 32) ^ rd();
		return fmt::format("{:016x}", v);
	}

	u64 PeerTable::Clock(u64 now_ms)
	{
#ifdef VR_NETPLAY_PLANTED_NO_CLOCK_CLAMP
		return now_ms;
#endif
		m_now = std::max(m_now, now_ms);
		return m_now;
	}

	PeerTable::Peer* PeerTable::Find(const MAC& m)
	{
		for (Peer& p : m_peers)
		{
			if (p.mac == m)
				return &p;
		}
		return nullptr;
	}

	PeerTable::Peer* PeerTable::FindOrAdd(const MAC& m)
	{
		if (Peer* p = Find(m))
			return p;
		if (m_peers.size() >= kMaxPeers)
			return nullptr;
		m_peers.push_back({});
		m_peers.back().mac = m;
		return &m_peers.back();
	}

	std::vector<MAC> PeerTable::OnGuestSend(const MAC& dst, const MAC& self, const MAC& internal, const MAC& host, u64 now_ms)
	{
		std::vector<MAC> out;
		std::lock_guard lock{m_mutex};
		const u64 now = Clock(now_ms);
		if (IsUnicast(dst) && dst != self && dst != internal && dst != host)
		{
			Peer* p = Find(dst);
#ifdef VR_NETPLAY_PLANTED_UNHEARD
			if (!p)
				p = FindOrAdd(dst);
			if (p && !p->sent)
#else
			if (p && p->heard && !p->sent)
#endif
				out.push_back(dst);
		}
		for (Peer& p : m_peers)
		{
			if (std::find(out.begin(), out.end(), p.mac) != out.end())
				continue;
			if (p.reply_owed)
				out.push_back(p.mac);
			else if (p.sent && !p.answered && !p.resent && now - p.first_sent_ms >= kResendMs)
			{
				p.resent = true;
				out.push_back(p.mac);
			}
		}
		return out;
	}

	void PeerTable::OnHeard(const MAC& src, u64 now_ms)
	{
		if (!IsUnicast(src))
			return;
		std::lock_guard lock{m_mutex};
		Clock(now_ms);
		if (Peer* p = FindOrAdd(src))
			p->heard = true;
	}

	void PeerTable::MarkSent(const MAC& m, bool ok, u64 now_ms)
	{
		std::lock_guard lock{m_mutex};
		const u64 now = Clock(now_ms);
		Peer* p = Find(m);
		if (!p || !ok)
			return;
		if (!p->sent)
			p->first_sent_ms = now;
		p->sent = true;
		p->reply_owed = false;
	}

	PeerTable::Heard PeerTable::OnRecord(const MAC& src, std::string_view their_session, std::string_view their_seen,
		std::string_view our_session, u64 now_ms)
	{
		Heard h;
		std::lock_guard lock{m_mutex};
		Clock(now_ms);
		Peer* p = FindOrAdd(src);
		if (!p)
			return h;
		p->answered = true;
#ifdef VR_NETPLAY_PLANTED_NO_SESSION
		if (p->their_session.empty())
#else
		if (p->their_session != their_session)
#endif
		{
			h.log_full = true;
			p->their_session = std::string(their_session);
			p->overdue_said = false;
		}
		if (their_seen != our_session)
		{
			h.owe_reply = true;
			p->reply_owed = true;
		}
		return h;
	}

	bool PeerTable::OnUnreadable(const MAC& src, u64 now_ms)
	{
		std::lock_guard lock{m_mutex};
		Clock(now_ms);
		Peer* p = FindOrAdd(src);
		if (!p)
			return false;
		p->answered = true;
		const bool first = !p->unreadable_said;
		p->unreadable_said = true;
		return first;
	}

	std::string PeerTable::SeenSession(const MAC& m)
	{
		std::lock_guard lock{m_mutex};
		const Peer* p = Find(m);
		return (p && !p->their_session.empty()) ? p->their_session : std::string("-");
	}

	std::vector<MAC> PeerTable::Overdue(u64 now_ms)
	{
		std::vector<MAC> out;
		std::lock_guard lock{m_mutex};
		const u64 now = Clock(now_ms);
		for (Peer& p : m_peers)
		{
			if (p.sent && !p.answered && !p.overdue_said && now - p.first_sent_ms >= kAnswerWaitMs)
			{
				p.overdue_said = true;
				out.push_back(p.mac);
			}
		}
		return out;
	}

	size_t PeerTable::Size()
	{
		std::lock_guard lock{m_mutex};
		return m_peers.size();
	}

	void PeerTable::Clear()
	{
		std::lock_guard lock{m_mutex};
		m_peers.clear();
	}

	void SetLocalRecord(Record r)
	{
		std::lock_guard lock{s_local_mutex};
		s_local = std::move(r);
	}

	Record GetLocalRecord()
	{
		std::lock_guard lock{s_local_mutex};
		Record r = s_local;
		if (!r.serial.empty() && r.serial != "-")
		{
			r.settings = s_local_settings;
			r.patches = s_local_patches;
			r.cheats = s_local_cheats;
		}
		return r;
	}

	void SetLocalSettings(std::vector<std::pair<std::string, std::string>> settings)
	{
		std::lock_guard lock{s_local_mutex};
		s_local_settings = std::move(settings);
	}

	void SetLocalPatches(std::vector<std::string> patches, std::vector<std::string> cheats)
	{
		std::lock_guard lock{s_local_mutex};
		s_local_patches = std::move(patches);
		s_local_cheats = std::move(cheats);
	}

	std::vector<std::pair<std::string, std::string>> SettingsLines(int ee_cycle_rate, int ee_cycle_skip, float speed,
		std::string_view limiter, bool gamedb_patches)
	{
		return {{"ee_cycle_rate", std::to_string(ee_cycle_rate)}, {"ee_cycle_skip", std::to_string(ee_cycle_skip)},
			{"speed", fmt::format("{:.2f}", speed)}, {"limiter", std::string(limiter)},
			{"gamedb_patches", gamedb_patches ? "on" : "off"}};
	}

	std::string FormatMAC(const MAC& m)
	{
		return fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", m[0], m[1], m[2], m[3], m[4], m[5]);
	}
}
