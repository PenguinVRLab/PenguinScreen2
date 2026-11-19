// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/YAML.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VR::Mods
{
	static constexpr u32 VU1_MICRO_BYTES = 0x4000;

	struct Guard
	{
		u32 address = 0;
		u32 equals = 0;
		u32 width = 4;
		bool relative = false;
	};

	struct Vu1Signature
	{
		u32 micro = 0;
		std::vector<u32> words;
	};

	struct Vu1Patch
	{
		std::vector<Vu1Signature> match;
		u32 micro = 0;
		std::vector<u32> words;
	};

	struct EeWord
	{
		u32 address = 0;
		u32 value = 0;
		std::optional<u32> original;
		bool relative = false;
	};

	struct Find
	{
		std::vector<u8> bytes;
		std::vector<u8> mask;
		u32 lo = 0, hi = 0;
		u32 align = 4;
	};

	enum RunsIn : u8
	{
		RUNS_OFFLINE = 1 << 0,
		RUNS_LAN = 1 << 1,
		RUNS_ONLINE = 1 << 2,
	};

	enum class NetMode
	{
		Offline,
		Networked,
	};

	enum Roles : u8
	{
		ROLE_HOST = 1 << 0,
		ROLE_JOINER = 1 << 1,
	};

	struct RoleWord
	{
		bool via_pointer = false;
		u32 address = 0;
		u32 pointer = 0;
		u32 offset = 0;
		u32 host = 0;
		u32 joiner = 0;
	};

	struct NetGate
	{
		u32 address = 0;
		u32 lan = 0;
		u32 online = 0;
		std::optional<RoleWord> role;
	};

	struct OnlineApproval
	{
		std::string service;
		std::string by;
	};

	struct Mod
	{
		std::string serial;
		std::string name;
		std::string source;
		std::string slug;
		std::string version;
		u8 runs_in = RUNS_OFFLINE;
		bool runs_in_stated = false;
		std::string effect;
		std::optional<NetGate> gate;
		u8 roles = 0;
		std::optional<bool> both_must_run;
		std::vector<OnlineApproval> online_approval;
		bool user = false;
		std::vector<u32> crcs;
		bool enabled = true;
		bool restart_required = true;
		bool own_enabled = true;
		std::vector<Guard> guards;
		std::vector<Vu1Patch> vu1;
		std::vector<EeWord> ee;
		std::optional<Find> find;
	};

	int FindSearch(const Mod& m, const u8* ram, u32 ram_size, u32* at);
	bool FindHolds(const Mod& m, const u8* ram, u32 ram_size, u32 at);

	bool CodeInMemory(const Mod& m, const u8* ram, u32 ram_size, const u8* micro, u32 micro_size);

	std::vector<Mod> ParseYAML(std::string_view text, std::string_view source, std::vector<std::string>* errors);

	class Vu1Tracker
	{
	public:
		struct Write
		{
			const Vu1Patch* patch;
		};

		void Reset();
		void SetPatches(std::vector<const Vu1Patch*> patches);
		bool HasPatches() const { return !m_patches.empty(); }

		std::vector<Write> OnUpload(u32 addr, const void* data, u32 size_bytes);

		void Commit(const Vu1Patch& patch);

		bool Matches(const Vu1Patch& patch) const;
		bool Present(const Vu1Patch& patch) const;

	private:
		void Store(u32 addr, const u8* data, u32 size_bytes);

		std::array<u8, VU1_MICRO_BYTES> m_shadow{};
		std::vector<const Vu1Patch*> m_patches;
	};

	void EnsureLoaded();
	void Reset();
	const std::vector<Mod>& All();
	const std::vector<std::string>& LoadIssues();
	const std::vector<std::string>& LoadNotes();

	enum class LineState : u8
	{
		Active,
		Waiting,
		Off,
		Refused,
		NotHere,
	};
	struct Line
	{
		std::string slug;
		u32 version = 0;
		std::string name;
		std::string description;
		u8 kind = 0;
		bool theirs = false;
		std::string by;
		bool restart_required = true;
		bool enabled = false;
		LineState state = LineState::Off;
		std::string reason;
		std::string note;
		std::string turned_on_for;
		std::vector<std::pair<std::string, std::string>> dependencies;
		bool vr_profile_binds = false;
		std::string source;
		bool user = false;
	};
	struct LiveFacts
	{
		bool refused = false;
		std::string refused_why;
		bool enabled = false;
		bool feature = false;
		bool bound = false;
		bool applies = false;
		std::string held_why;
		std::string not_here;
		bool has_find = false, anchor_found = false;
		bool has_ee = false, armed = false;
		bool has_vu1 = false, vu1_present = false;
	};
	LineState StateOf(const LiveFacts& f, std::string* why);
	std::string NotHereReason(const Mod& m, u32 crc, NetMode mode);
	std::vector<Line> Lines(std::string_view serial, u32 crc);
	std::vector<Line> Lines();
	bool IsOn(std::string_view serial, std::string_view slug);
	bool IsOn(std::string_view slug);
	struct Collision
	{
		const Mod* a = nullptr;
		const Mod* b = nullptr;
		bool vu1 = false;
		bool guard = false;
		u32 address = 0;
		u32 shared = 0;
	};
	std::vector<Collision> DropCollisions(std::vector<const Mod*>& active);
	std::string CollisionText(const Collision& c);
	std::vector<Mod> ModsOf(std::string_view serial);
	std::string CollidesWith(const std::vector<Mod>& mods, std::string_view slug, const std::vector<std::string>& on);
	std::vector<const Mod*> Select(std::string_view serial, u32 crc, NetMode mode, std::vector<Collision>* collisions = nullptr);
	std::vector<const Mod*> Select(std::string_view serial, u32 crc);
	bool NeedsGate(const Mod& m);
	bool NeedsRoleWord(const Mod& m);
	bool NeedsBothMustRun(const Mod& m);
	bool NeedsOnlineApproval(const Mod& m);
	std::string RunsInText(const Mod& m);
	inline constexpr char kOriginOurs[] = "PenguinVR";
	inline constexpr char kOriginRefusal[] =
		"origin: is 'PenguinVR' or the authors' names, or a map naming the tweak ('name') and its authors ('by')";
	inline constexpr char kOriginRepeated[] = "origin: is stated more than once";
	struct Origin
	{
		bool stated = false;
		bool ours = false;
		std::string by;
	};
	bool ParseOrigin(const ryml::ConstNodeRef& entry, Origin& out, std::string& why);
	bool ParseNetplay(const ryml::ConstNodeRef& entry, Mod& out, std::string& why);

	void OnVsync();
	void OnVU1Upload(u32 addr, const void* data, u32 size_bytes);
	void OnStateLoaded();
}
