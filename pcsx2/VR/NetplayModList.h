// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VR::Netplay
{
	inline constexpr u16 kEtherType = 0x88B5;
	inline constexpr char kMagic[8] = {'P', 'S', 'V', 'R', 'M', 'O', 'D', 'S'};
	inline constexpr u32 kFormat = 1;
	inline constexpr u32 kMaxFrame = 1514;
	inline constexpr u64 kResendMs = 2000;
	inline constexpr u64 kAnswerWaitMs = 5000;
	inline constexpr size_t kMaxPeers = 64;
	inline constexpr size_t kMaxField = 96;

	using MAC = std::array<u8, 6>;

	struct ModLine
	{
		std::string id;
		std::string runs_in;
	};

	struct Record
	{
		std::string serial;
		u32 crc = 0;
		std::string build;
		std::string session;
		std::string seen;
		std::vector<ModLine> mods;
		u32 mods_cut = 0;
		std::vector<std::pair<std::string, std::string>> settings;
		std::vector<std::string> patches;
		std::vector<std::string> cheats;
		u32 extras_cut = 0;
	};

	std::vector<std::pair<std::string, std::string>> SettingsLines(int ee_cycle_rate, int ee_cycle_skip, float speed,
		std::string_view limiter, bool gamedb_patches);

	std::string Encode(const Record& r);
	bool Decode(std::string_view text, Record* out);

	std::vector<u8> BuildFrame(const MAC& dst, const MAC& src, const Record& r);
	bool IsRecordFrame(const u8* frame, size_t size);
	bool ParseFrame(const u8* frame, size_t size, MAC* src, Record* out);

	bool IsUnicast(const MAC& m);

	std::vector<std::string> Compare(const Record& ours, const Record& theirs);

	std::string NewSession();

	class PeerTable
	{
	public:
		void OnHeard(const MAC& src, u64 now_ms);
		std::vector<MAC> OnGuestSend(const MAC& dst, const MAC& self, const MAC& internal, const MAC& host, u64 now_ms);
		void MarkSent(const MAC& m, bool ok, u64 now_ms);

		struct Heard
		{
			bool log_full = false;
			bool owe_reply = false;
		};
		Heard OnRecord(const MAC& src, std::string_view their_session, std::string_view their_seen,
			std::string_view our_session, u64 now_ms);
		bool OnUnreadable(const MAC& src, u64 now_ms);
		std::string SeenSession(const MAC& m);
		std::vector<MAC> Overdue(u64 now_ms);
		size_t Size();
		void Clear();

	private:
		struct Peer
		{
			MAC mac{};
			bool heard = false;
			bool sent = false;
			u64 first_sent_ms = 0;
			bool resent = false;
			bool answered = false;
			bool reply_owed = false;
			bool overdue_said = false;
			bool unreadable_said = false;
			std::string their_session;
		};
		Peer* Find(const MAC& m);
		Peer* FindOrAdd(const MAC& m);
		u64 Clock(u64 now_ms);
		std::mutex m_mutex;
		std::vector<Peer> m_peers;
		u64 m_now = 0;
	};

	void SetLocalRecord(Record r);
	Record GetLocalRecord();
	void SetLocalSettings(std::vector<std::pair<std::string, std::string>> settings);
	void SetLocalPatches(std::vector<std::string> patches, std::vector<std::string> cheats);

	std::string FormatMAC(const MAC& m);
}
