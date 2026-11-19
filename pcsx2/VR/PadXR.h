// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/PadXRCore.h"
#include "VR/VRInputState.h"
#include "VR/VRProfileDB.h"

#include "common/Pcsx2Defs.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class MemoryInterface;

namespace VR::PadXR
{
	static constexpr u32 kPorts = 8;
	static constexpr u32 kSeats = 4;

	enum class DisarmReason : u8
	{
		GateClosed,
		NeedLost,
		OnlineLock,
		ProfileChanged,
		SessionGone,
		Reset,
	};

	enum class HostAction : u8
	{
		Recenter,
		Pause,
	};

	class Outputs
	{
	public:
		void press(int bit, u32 transactions = 1);
		void hold(int bit);
		void own(int bit, bool down);
		void stick(Axis axis, Claim mode, float value);
		void stickOwnByte(Axis axis, u8 value);
		void stickUnits(Axis axis, int units);
		void pressure(int bit, u8 value, bool own = false);
		void cue(const Cue& c);
		void host(HostAction a);

		void Clear();
		bool Empty() const;

		u16 hold_mask = 0;
		u16 own_mask = 0;
		u16 own_down = 0;
		std::array<StickCell, kAxisCount> sticks{};
		PressureCell pressures{};
		std::vector<std::pair<int, u32>> presses;
		std::vector<Cue> cues;
		std::vector<HostAction> hosts;
		u32 stick_writes = 0;
	};

	struct TickContext
	{
		u32 seat = 0;
		u32 port = 0;
		u64 tick = 0;
		double wall_ms = 0.0;
		const VRInputSnapshot& input;
		u32 caps = 0;
		MemoryInterface* ram = nullptr;
	};

	struct TransactionContext
	{
		u32 port = 0;
		u64 transaction = 0;
	};

	class Member
	{
	public:
		virtual ~Member() = default;
		virtual const MemberDesc& Desc() const = 0;
		virtual bool Arm(std::string* why) { return true; }
		virtual void Tick(const TickContext& ctx, Outputs& out) {}
		virtual void Transaction(const TransactionContext& ctx, Outputs& out) {}
		virtual void Disarm(DisarmReason reason) {}
		virtual void OnStateLoaded() {}
	};

	using Factory = std::unique_ptr<Member> (*)();

	bool RegisterKind(std::string_view kind, Factory f);
	std::unique_ptr<Member> Create(std::string_view kind);

	struct ArmRequest
	{
		u32 seat = 0;
		u32 port = 0;
		std::vector<ProfileDB::CameraGuard> gates;
	};

	bool Arm(std::unique_ptr<Member> member, const ArmRequest& req, std::string* why = nullptr);
	void DisarmAll(DisarmReason reason);
	void EnsureBuiltins();

	void Tick();
	void OnStateLoaded();

	void OnPollByte(u32 port, size_t byte_index);
	void OnTransaction(u32 port);
	u32 Word(u32 port, u32 raw);
	u8 Stick(u32 port, Axis axis, u8 real);
	u8 PollStick(u32 port, Axis axis, u8 real);
	int Pressure(u32 port, size_t poll_byte, u8 real);
	bool PressureActive(u32 port);

	using GuestReadFn = bool (*)(u32 address, u8 width, u32* value);
	bool GatePass(MemoryInterface& mem, const std::vector<ProfileDB::CameraGuard>& guards);
	bool GatePass(GuestReadFn read, const std::vector<ProfileDB::CameraGuard>& guards);

	bool SendCue(u32 seat, const Cue& c);
	struct CueCounts
	{
		u64 sent = 0;
		u64 dropped = 0;
	};
	CueCounts GetCueCounts();

	const MemberDesc& SpatialControlsDesc();

	const std::vector<MemberDesc>& DescriptorOnlyMembers();
	void ReportAdapter(u32 seat, bool live);

	struct MemberStatus
	{
		std::string id;
		std::string kind;
		u32 seat = 0;
		u32 port = 0;
		bool gated_open = false;
		u64 ticks = 0;
		u64 transactions = 0;
		u64 presses = 0;
		u64 stick_writes = 0;
		u64 cues_sent = 0;
		u64 cues_dropped = 0;
	};
	std::vector<MemberStatus> Status();
	u64 Refusals();

	std::vector<std::string> Manifest();
	std::string ManifestLine(const MemberDesc& d);

	struct Env
	{
		VRInputSnapshot (*snapshot)() = nullptr;
		MemoryInterface* (*ram)() = nullptr;
		bool (*online)() = nullptr;
		bool (*split_active)() = nullptr;
		bool (*replaying)() = nullptr;
		double (*wall_ms)() = nullptr;
		int (*padlook_units)() = nullptr;
		bool (*rx_lanes)(u8 real, u8* out) = nullptr;
		u8 (*probe_stick)(Axis axis, u8 real) = nullptr;
		u8 (*probe_pressure)(int bit, u8 real) = nullptr;
		void (*pulse)(int hand, float amplitude, float seconds, u8 locus) = nullptr;
		void (*log)(const char* line) = nullptr;
		bool (*ui_owns_input)() = nullptr;
	};

	static constexpr u8 kUiRestStick = 0x7F;
	bool UiOwnsInput();

	Env DefaultEnv();

	static constexpr std::string_view kPadLookKind = "padLook";
	const MemberDesc& PadLookDesc();

	u16 StepHolding(EdgeScheduler& s, u16 held);

	void ResetForTest(const Env& env);
}

namespace VR::PadLook
{
	int PublishedRxUnits();
	bool RxLanes(u8 real, u8* out);
}
