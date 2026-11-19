// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/VRProfileDB.h"

#include "common/MemoryInterface.h"
#include "common/Pcsx2Types.h"

#include <array>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace VR::GuestPatch
{
	inline constexpr u64 kDrainVsyncs = 2;

	inline constexpr u64 kSiteRetryVsyncs = 36000;

	struct Hook
	{
		u32 site = 0;
		u32 original = 0;
		u32 trampoline = 0;
		u32 cave = 0;
		std::array<u32, 5> body{};
		u32 scratch = 0;

		bool operator==(const Hook&) const = default;
	};

	enum class HookState : u8
	{
		Off,
		Live,
		Refused,
		Dropped,
	};

	enum class LogLevel : u8
	{
		Routine,
		Notice,
		Warning,
	};

	class Ledger
	{
	public:
		using LogFn = std::function<void(LogLevel, const std::string&)>;

		void SetLog(LogFn fn) { m_log = std::move(fn); }

		void Service(MemoryInterface& mem, u32 crc, bool running, u64 vsync);

		void Arm(MemoryInterface& mem, const std::vector<ProfileDB::CameraSilence>& silence,
			const std::vector<Hook>& hooks, u64 vsync);

		struct Lists
		{
			const std::vector<ProfileDB::CameraSilence>& silence;
			const std::vector<Hook>& hooks;
		};

		void Disarm(MemoryInterface& mem, u64 vsync, const Lists* repair_with);

		bool WriteScratch(MemoryInterface& mem, size_t i, u32 bits);

		void OnStateLoaded();

		void Forget();

		HookState HookStateAt(size_t i) const { return i < m_hook_state.size() ? m_hook_state[i] : HookState::Off; }
		bool SilenceIn() const { return m_silence_in; }
		size_t SitesRetrying() const;
		bool RepairPending() const { return m_repair_pending; }
		size_t PendingBodyWords() const { return m_drain.size(); }

	private:
		struct Word
		{
			u32 address = 0;
			u32 on = 0;
			u32 off = 0;
			bool site = false;
			bool ours = false;
			bool retry = false;
		};

		void Log(LogLevel level, const std::string& line);
		void WarnOnce(u32 address, u32 value, const std::string& line);

		void SetWords(const std::vector<ProfileDB::CameraSilence>& silence);
		bool SameWords(const std::vector<ProfileDB::CameraSilence>& silence) const;
		void PutSilenceIn(MemoryInterface& mem, const std::vector<ProfileDB::CameraSilence>& silence, u64 vsync);
		void RetryRefusedSites(MemoryInterface& mem, u64 vsync);
		void TakeSilenceOut(MemoryInterface& mem, u64 vsync, bool repair);
		void CancelDrainFor(const std::vector<ProfileDB::CameraSilence>& silence);
		void RunDrain(MemoryInterface& mem);

		void InstallHook(MemoryInterface& mem, size_t i);
		void CheckLiveHook(MemoryInterface& mem, size_t i);
		void DropHook(MemoryInterface& mem, size_t i, const std::string& why);
		void TakeHooksOut(MemoryInterface& mem, bool repair);
		bool Blocked(const Hook& h) const;

		u32 m_crc = 0;
		bool m_have_crc = false;

		std::vector<Word> m_words;
		bool m_silence_in = false;
		u64 m_refused_at = 0;
		std::vector<Word> m_drain;
		u64 m_drain_at = 0;

		std::vector<Hook> m_hooks;
		std::vector<HookState> m_hook_state;
		std::vector<u32> m_scratch_last;
		bool m_hooks_in = false;

		bool m_repair_pending = false;

		std::set<std::pair<u32, u32>> m_blocked;
		std::set<std::pair<u32, u32>> m_warned;
		LogFn m_log;
	};
}
