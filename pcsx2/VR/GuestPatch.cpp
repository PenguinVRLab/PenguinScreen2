// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/GuestPatch.h"

#include "fmt/format.h"

#include <algorithm>

namespace VR::GuestPatch
{
	void Ledger::Log(LogLevel level, const std::string& line)
	{
		if (m_log)
			m_log(level, line);
	}

	void Ledger::WarnOnce(u32 address, u32 value, const std::string& line)
	{
		if (m_warned.emplace(address, value).second)
			Log(LogLevel::Warning, line);
	}

	void Ledger::Service(MemoryInterface& mem, u32 crc, bool running, u64 vsync)
	{
		if (!m_have_crc || crc != m_crc)
		{
			Forget();
			m_warned.clear();
			m_crc = crc;
			m_have_crc = true;
			return;
		}
		if (running && !m_drain.empty() && vsync >= m_drain_at)
			RunDrain(mem);
	}

	void Ledger::Forget()
	{
		m_words.clear();
		m_silence_in = false;
		m_drain.clear();
		m_hooks.clear();
		m_hook_state.clear();
		m_scratch_last.clear();
		m_hooks_in = false;
		m_repair_pending = false;
		m_have_crc = false;
	}

	void Ledger::OnStateLoaded()
	{
#ifdef VR_GUESTPATCH_PLANTED_KEEP_ON_LOAD
		return;
#endif
		for (Word& w : m_words)
			w.ours = false;
		m_silence_in = false;
		m_drain.clear();
		for (HookState& s : m_hook_state)
		{
			if (s != HookState::Dropped)
				s = HookState::Off;
		}
		m_hooks_in = false;
		m_repair_pending = true;
	}

	void Ledger::SetWords(const std::vector<ProfileDB::CameraSilence>& silence)
	{
		m_words.clear();
		m_words.reserve(silence.size());
		for (const ProfileDB::CameraSilence& s : silence)
			m_words.push_back(Word{s.ee_address, s.value_on, s.value_off, s.value_off != 0, false});
	}

	bool Ledger::SameWords(const std::vector<ProfileDB::CameraSilence>& silence) const
	{
		if (silence.size() != m_words.size())
			return false;
		for (size_t i = 0; i < silence.size(); i++)
		{
			if (silence[i].ee_address != m_words[i].address || silence[i].value_on != m_words[i].on ||
				silence[i].value_off != m_words[i].off)
				return false;
		}
		return true;
	}

	void Ledger::PutSilenceIn(MemoryInterface& mem, const std::vector<ProfileDB::CameraSilence>& silence, u64 vsync)
	{
		SetWords(silence);
		for (Word& w : m_words)
		{
			if (w.site)
				continue;
			mem.Write32(w.address, w.on);
			w.ours = true;
		}
		size_t refused = 0;
		for (Word& w : m_words)
		{
			if (!w.site)
				continue;
			const u32 now = mem.Read32(w.address);
			if (now == w.off)
			{
				mem.Write32(w.address, w.on);
				w.ours = true;
			}
			else if (now == w.on)
			{
				w.ours = true;
			}
			else
			{
				refused++;
				w.retry = true;
				WarnOnce(w.address, now,
					fmt::format("(VR) CameraDriver: silence word at 0x{:08X} holds 0x{:08X}, neither its off 0x{:08X} nor its "
								"on 0x{:08X}: a wrong address, another build of the game, or code not loaded yet. Left alone; "
								"looked at again every vsync for {} s.",
						w.address, now, w.off, w.on, kSiteRetryVsyncs / 60));
			}
		}
		m_refused_at = vsync;
		m_silence_in = true;
		if (!m_words.empty())
			Log(LogLevel::Routine, fmt::format("(VR) CameraDriver: game camera writer silenced ({} patch(es){}).",
									   m_words.size() - refused, refused ? fmt::format(", {} refused", refused) : std::string()));
	}

	size_t Ledger::SitesRetrying() const
	{
		return static_cast<size_t>(std::count_if(m_words.begin(), m_words.end(), [](const Word& w) { return w.retry; }));
	}

	void Ledger::RetryRefusedSites(MemoryInterface& mem, u64 vsync)
	{
#ifdef VR_GUESTPATCH_PLANTED_NO_RETRY
		return;
#endif
		const bool give_up = vsync >= m_refused_at + kSiteRetryVsyncs;
		for (Word& w : m_words)
		{
			if (!w.retry)
				continue;
			const u32 now = mem.Read32(w.address);
			if (now == w.off || now == w.on)
			{
				if (now == w.off)
					mem.Write32(w.address, w.on);
				w.ours = true;
				w.retry = false;
				Log(LogLevel::Notice,
					fmt::format("(VR) CameraDriver: silence word at 0x{:08X} now holds its {} 0x{:08X}, {} vsyncs after it "
								"was refused: {}.",
						w.address, now == w.off ? "off" : "on", now, vsync - m_refused_at,
						now == w.off ? "put in" : "adopted"));
			}
			else if (give_up)
			{
				w.retry = false;
				Log(LogLevel::Warning,
					fmt::format("(VR) CameraDriver: silence word at 0x{:08X} still holds 0x{:08X} (neither its off 0x{:08X} "
								"nor its on 0x{:08X}) {} s after the arm: given up; it stays the game's until the next arm.",
						w.address, now, w.off, w.on, kSiteRetryVsyncs / 60));
			}
		}
	}

	void Ledger::CancelDrainFor(const std::vector<ProfileDB::CameraSilence>& silence)
	{
		if (m_drain.empty())
			return;
		m_drain.erase(std::remove_if(m_drain.begin(), m_drain.end(),
						  [&silence](const Word& w) {
							  return std::any_of(silence.begin(), silence.end(),
								  [&w](const ProfileDB::CameraSilence& s) { return s.ee_address == w.address; });
						  }),
			m_drain.end());
	}

	void Ledger::TakeSilenceOut(MemoryInterface& mem, u64 vsync, bool repair)
	{
		size_t sites = 0, bodies = 0;
#ifdef VR_GUESTPATCH_PLANTED_SAME_VSYNC
		for (Word& w : m_words)
		{
			if (w.ours || repair)
				mem.Write32(w.address, w.off);
			w.ours = false;
		}
		m_silence_in = false;
		return;
#endif
		for (Word& w : m_words)
		{
			if (!w.site || !(w.ours || repair) || w.on == w.off)
				continue;
#ifdef VR_GUESTPATCH_PLANTED_BLIND_RESTORE
			if (true)
#else
			if (mem.Read32(w.address) == w.on)
#endif
			{
				mem.Write32(w.address, w.off);
				sites++;
			}
		}
		for (Word& w : m_words)
		{
			if (w.site || !(w.ours || repair) || w.on == w.off)
				continue;
			const bool queued = std::any_of(m_drain.begin(), m_drain.end(), [&w](const Word& d) { return d.address == w.address; });
			if (!queued)
				m_drain.push_back(w);
			bodies++;
		}
		if (bodies > 0)
			m_drain_at = vsync + kDrainVsyncs;
		for (Word& w : m_words)
		{
			w.ours = false;
			w.retry = false;
		}
		m_silence_in = false;
		if (sites + bodies > 0)
			Log(LogLevel::Routine, fmt::format("(VR) CameraDriver: game camera writer restored ({} site word(s) now, {} body "
											   "word(s) in {} vsyncs).",
									   sites, bodies, kDrainVsyncs));
	}

	void Ledger::RunDrain(MemoryInterface& mem)
	{
		for (const Word& w : m_drain)
		{
#ifdef VR_GUESTPATCH_PLANTED_BLIND_RESTORE
			if (true)
#else
			if (mem.Read32(w.address) == w.on)
#endif
				mem.Write32(w.address, w.off);
		}
		m_drain.clear();
	}

	bool Ledger::Blocked(const Hook& h) const
	{
		return m_blocked.count({m_crc, h.cave}) != 0;
	}

	void Ledger::InstallHook(MemoryInterface& mem, size_t i)
	{
		const Hook& h = m_hooks[i];
		if (Blocked(h))
		{
			m_hook_state[i] = HookState::Dropped;
			return;
		}
		const u32 at_site = mem.Read32(h.site);
#ifdef VR_GUESTPATCH_PLANTED_ANY_JAL
		if ((at_site >> 26) == 0x03u)
			m_hooks[i].original = at_site;
#endif
		if (at_site != h.original && at_site != h.trampoline)
		{
			m_hook_state[i] = HookState::Refused;
			WarnOnce(h.site, at_site,
				fmt::format("(VR) CameraDriver: code hook at 0x{:08X} holds 0x{:08X}, not the call its cave returns to "
							"(jal tailJump = 0x{:08X}): a wrong hookAddress or tailJump, or another build. Not installed.",
					h.site, at_site, h.original));
			return;
		}
		for (u32 k = 0; k < h.body.size(); k++)
		{
			const u32 a = h.cave + k * 4u;
			const u32 now = mem.Read32(a);
#ifdef VR_GUESTPATCH_PLANTED_NO_CLOBBER
			if (false)
#else
			if (now != 0 && now != h.body[k])
#endif
			{
				if (at_site == h.trampoline)
					mem.Write32(h.site, h.original);
				m_hook_state[i] = HookState::Refused;
				WarnOnce(a, now,
					fmt::format("(VR) CameraDriver: code-hook cave word at 0x{:08X} holds 0x{:08X} (not 0, not ours): the cave "
								"is not free on this build. Move camera.codeHooks caveAddress. Not installed.",
						a, now));
				return;
			}
		}
		for (u32 k = 0; k < h.body.size(); k++)
			mem.Write32(h.cave + k * 4u, h.body[k]);
		for (u32 k = 0; k < h.body.size(); k++)
		{
			if (mem.Read32(h.cave + k * 4u) != h.body[k])
			{
				if (at_site == h.trampoline)
					mem.Write32(h.site, h.original);
				m_hook_state[i] = HookState::Refused;
				WarnOnce(h.cave + k * 4u, h.body[k],
					fmt::format("(VR) CameraDriver: code-hook cave at 0x{:08X} did not read back. Not installed.", h.cave));
				return;
			}
		}
		mem.Write32(h.scratch, 0);
		m_scratch_last[i] = 0;
		if (at_site == h.original)
			mem.Write32(h.site, h.trampoline);
		m_hook_state[i] = HookState::Live;
	}

	void Ledger::DropHook(MemoryInterface& mem, size_t i, const std::string& why)
	{
		const Hook& h = m_hooks[i];
		if (mem.Read32(h.site) == h.trampoline)
			mem.Write32(h.site, h.original);
		m_hook_state[i] = HookState::Dropped;
		m_blocked.emplace(m_crc, h.cave);
		Log(LogLevel::Warning,
			fmt::format("(VR) CameraDriver: code hook at 0x{:08X} DROPPED: {}. The game uses that memory, so the cave at "
						"0x{:08X} is not free on this build: move camera.codeHooks caveAddress/scratchAddress to memory the "
						"game never writes (prove it with a write watch). Off until the game changes.",
				h.site, why, h.cave));
	}

	void Ledger::CheckLiveHook(MemoryInterface& mem, size_t i)
	{
		const Hook& h = m_hooks[i];
		const u32 at_site = mem.Read32(h.site);
		if (at_site == h.original)
		{
			m_hook_state[i] = HookState::Off;
			InstallHook(mem, i);
			if (m_hook_state[i] == HookState::Live)
				Log(LogLevel::Routine, fmt::format("(VR) CameraDriver: code hook at 0x{:08X} reinstalled.", h.site));
			return;
		}
		if (at_site != h.trampoline)
		{
			m_hook_state[i] = HookState::Refused;
			WarnOnce(h.site, at_site,
				fmt::format("(VR) CameraDriver: code hook at 0x{:08X} changed under a live hook (now 0x{:08X}); left alone.",
					h.site, at_site));
			return;
		}
		for (u32 k = 0; k < h.body.size(); k++)
		{
			const u32 now = mem.Read32(h.cave + k * 4u);
#ifdef VR_GUESTPATCH_PLANTED_NO_CLOBBER
			if (false)
#else
			if (now != h.body[k])
#endif
			{
				DropHook(mem, i, fmt::format("its cave word at 0x{:08X} now holds 0x{:08X}", h.cave + k * 4u, now));
				return;
			}
		}
	}

	void Ledger::TakeHooksOut(MemoryInterface& mem, bool repair)
	{
		size_t restored = 0;
		for (size_t i = 0; i < m_hooks.size(); i++)
		{
			const Hook& h = m_hooks[i];
#ifdef VR_GUESTPATCH_PLANTED_BLIND_RESTORE
			if (m_hook_state[i] == HookState::Live || repair)
#else
			if ((m_hook_state[i] == HookState::Live || repair) && mem.Read32(h.site) == h.trampoline)
#endif
			{
				mem.Write32(h.site, h.original);
				restored++;
			}
			if (m_hook_state[i] != HookState::Dropped)
				m_hook_state[i] = HookState::Off;
		}
		m_hooks_in = false;
		if (restored > 0)
			Log(LogLevel::Routine, fmt::format("(VR) CameraDriver: {} camera code-hook(s) restored.", restored));
	}

	bool Ledger::WriteScratch(MemoryInterface& mem, size_t i, u32 bits)
	{
		if (i >= m_hooks.size() || m_hook_state[i] != HookState::Live)
			return false;
		const u32 scratch = m_hooks[i].scratch;
		const u32 now = mem.Read32(scratch);
#ifdef VR_GUESTPATCH_PLANTED_NO_CLOBBER
		if (false)
#else
		if (now != m_scratch_last[i])
#endif
		{
			DropHook(mem, i, fmt::format("its scratch word at 0x{:08X} holds 0x{:08X}, not the 0x{:08X} written last", scratch,
								 now, m_scratch_last[i]));
			return false;
		}
		mem.Write32(scratch, bits);
		for (size_t j = 0; j < m_hooks.size(); j++)
		{
			if (m_hooks[j].scratch == scratch)
				m_scratch_last[j] = bits;
		}
		return true;
	}

	void Ledger::Arm(MemoryInterface& mem, const std::vector<ProfileDB::CameraSilence>& silence,
		const std::vector<Hook>& hooks, u64 vsync)
	{
		m_repair_pending = false;

		if (m_silence_in && !SameWords(silence))
		{
#ifdef VR_GUESTPATCH_PLANTED_NEW_LIST
			SetWords(silence);
			for (Word& w : m_words)
				w.ours = true;
#endif
			TakeSilenceOut(mem, vsync, false);
		}
		if (!m_silence_in)
		{
			CancelDrainFor(silence);
			PutSilenceIn(mem, silence, vsync);
		}
		else
			RetryRefusedSites(mem, vsync);

		if (m_hooks_in && hooks != m_hooks)
			TakeHooksOut(mem, false);
		if (!m_hooks_in)
		{
			m_hooks = hooks;
			m_hook_state.assign(hooks.size(), HookState::Off);
			m_scratch_last.assign(hooks.size(), 0);
			size_t live = 0;
			for (size_t i = 0; i < m_hooks.size(); i++)
			{
				InstallHook(mem, i);
				live += (m_hook_state[i] == HookState::Live) ? 1 : 0;
			}
			if (live > 0)
				Log(LogLevel::Routine, fmt::format("(VR) CameraDriver: {} camera code-hook(s) installed.", live));
			m_hooks_in = true;
			return;
		}
		for (size_t i = 0; i < m_hooks.size(); i++)
		{
			if (m_hook_state[i] == HookState::Live)
				CheckLiveHook(mem, i);
		}
	}

	void Ledger::Disarm(MemoryInterface& mem, u64 vsync, const Lists* repair_with)
	{
		if (m_repair_pending)
		{
			if (!repair_with)
				return;
			SetWords(repair_with->silence);
			m_hooks = repair_with->hooks;
			m_hook_state.assign(m_hooks.size(), HookState::Off);
			m_scratch_last.assign(m_hooks.size(), 0);
			TakeHooksOut(mem, true);
			TakeSilenceOut(mem, vsync, true);
			m_repair_pending = false;
			return;
		}
		if (m_hooks_in)
			TakeHooksOut(mem, false);
		if (m_silence_in)
			TakeSilenceOut(mem, vsync, false);
	}
}
