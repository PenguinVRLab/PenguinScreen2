// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/PadXR.h"

#include "VR/GuestRam.h"
#include "VR/PadLook.h"

#include "common/MemoryInterface.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#ifndef PADXR_STANDALONE
#include "Config.h"
#include "Memory.h"
#include "Recording/InputRecording.h"
#include "SIO/Pad/PadTypes.h"
#include "VMManager.h"
#include "VR/SplitState.h"
#include "VR/UiPanel.h"
#include "VR/VRInput.h"

#include "common/Console.h"
#endif

namespace VR::PadXR
{
#ifndef PADXR_STANDALONE
	static_assert(kUiRestStick == Pad::ANALOG_NEUTRAL_POSITION, "the UI's rest stick is the pad's own rest");
#endif

	namespace
	{
		bool BitOk(int bit) { return bit >= 0 && bit < kButtonBits; }
		u16 Bit(int bit) { return static_cast<u16>(1u << bit); }
	}

	void Outputs::press(int bit, u32 transactions)
	{
		if (BitOk(bit) && transactions > 0)
			presses.emplace_back(bit, transactions);
	}

	void Outputs::hold(int bit)
	{
		if (BitOk(bit))
			hold_mask |= Bit(bit);
	}

	void Outputs::own(int bit, bool down)
	{
		if (!BitOk(bit))
			return;
		own_mask |= Bit(bit);
		own_down = down ? (own_down | Bit(bit)) : (own_down & ~Bit(bit));
	}

	void Outputs::stick(Axis axis, Claim mode, float value)
	{
		if (axis >= Axis::Count)
			return;
		sticks[static_cast<size_t>(axis)].Write(mode, value);
		stick_writes++;
	}

	void Outputs::stickOwnByte(Axis axis, u8 value)
	{
		if (axis >= Axis::Count)
			return;
		sticks[static_cast<size_t>(axis)].OwnByte(value);
		stick_writes++;
	}

	void Outputs::stickUnits(Axis axis, int units)
	{
		if (axis >= Axis::Count || units == 0)
			return;
		sticks[static_cast<size_t>(axis)].AddUnits(units);
		stick_writes++;
	}

	void Outputs::pressure(int bit, u8 value, bool own_cell) { pressures.Write(bit, value, own_cell); }
	void Outputs::cue(const Cue& c) { cues.push_back(c); }
	void Outputs::host(HostAction a) { hosts.push_back(a); }

	void Outputs::Clear()
	{
		hold_mask = own_mask = own_down = 0;
		for (StickCell& s : sticks)
			s.Clear();
		pressures.Clear();
		presses.clear();
		cues.clear();
		hosts.clear();
		stick_writes = 0;
	}

	bool Outputs::Empty() const
	{
		if (hold_mask || own_mask || !presses.empty() || !cues.empty() || !hosts.empty() || !pressures.Empty())
			return false;
		for (const StickCell& s : sticks)
			if (!s.Empty())
				return false;
		return true;
	}

	namespace
	{
		struct Armed
		{
			std::unique_ptr<Member> member;
			ArmRequest req;
			bool open = true;
			bool release_due = false;
			Outputs tick_out;
			Outputs tx_out;
			EdgeScheduler sched;
			MemberStatus st;
		};

		struct PortCells
		{
			WordCell word;
			std::array<StickCell, kAxisCount> sticks{};
			PressureCell pressure;
			u64 transactions = 0;
			bool any = false;
			u8 harness_sticks = 0;
			u16 harness_pressure = 0;
			u16 claimed_own = 0;
			u16 releasing = 0;
			u16 claimed_press = 0;
			PressureCell claimed_pressure;
			u16 claimed_harness_pressure = 0;

			void ClearCells()
			{
				word.Clear();
				for (StickCell& s : sticks)
					s.Clear();
				pressure.Clear();
				any = false;
				harness_sticks = 0;
				harness_pressure = 0;
				claimed_own = 0;
				releasing = 0;
				claimed_press = 0;
				claimed_pressure.Clear();
				claimed_harness_pressure = 0;
			}
		};

		struct Kind
		{
			std::string name;
			Factory factory;
		};

		std::mutex s_mutex;
		std::vector<std::unique_ptr<Armed>> s_armed;
		std::array<PortCells, kPorts> s_ports{};
		u64 s_tick = 0;
		std::atomic<bool> s_builtins{false};
		std::atomic<u64> s_refusals{0};
		std::atomic<u64> s_cues_sent{0};
		std::atomic<u64> s_cues_dropped{0};

		std::vector<Kind>& Kinds()
		{
			static std::vector<Kind> s_kinds;
			return s_kinds;
		}

		Env& CurrentEnv()
		{
			static Env s_env = DefaultEnv();
			return s_env;
		}

		void Log(const std::string& line)
		{
			if (const Env& e = CurrentEnv(); e.log)
				e.log(line.c_str());
		}

		std::string Name(const Armed& a) { return std::string(a.member->Desc().id); }

		const char* ResName(Res r)
		{
			switch (r)
			{
				case Res::Mic: return "mic";
				case Res::Layers: return "layers";
				case Res::Haptics: return "haptics";
				case Res::RightStick: return "rightStick";
				case Res::LeftStick: return "leftStick";
				case Res::Speed: return "speed";
				default: return "?";
			}
		}

		const char* ReasonName(DisarmReason r)
		{
			switch (r)
			{
				case DisarmReason::GateClosed: return "gate closed";
				case DisarmReason::NeedLost: return "need lost";
				case DisarmReason::OnlineLock: return "online";
				case DisarmReason::ProfileChanged: return "profile changed";
				case DisarmReason::SessionGone: return "session gone";
				case DisarmReason::Reset: return "reset";
				default: return "?";
			}
		}

		void Refuse(std::string_view id, const std::string& reason, std::string* why)
		{
			s_refusals.fetch_add(1, std::memory_order_relaxed);
			Log("member " + std::string(id) + " not armed: " + reason);
			if (why)
				*why = reason;
		}

		bool IsOnline()
		{
			const Env& e = CurrentEnv();
			return e.online && e.online();
		}

		bool SplitActive()
		{
#ifdef VR_PADXR_PLANTED_SPLIT
			return false;
#else
			const Env& e = CurrentEnv();
			return e.split_active && e.split_active();
#endif
		}

		bool Deliver(u32 seat, const Cue& c)
		{
			const Env& e = CurrentEnv();
			if (!CueValid(c) || seat != 0 || !e.pulse)
			{
				s_cues_dropped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
#ifndef VR_PADXR_PLANTED_CUE_DROP
			e.pulse(c.hand, std::clamp(c.amplitude, 0.0f, 1.0f), static_cast<float>(c.ms) / 1000.0f, c.locus);
#endif
			s_cues_sent.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		bool Gated(const Armed& a, const VRInputSnapshot& snap, u32 caps, MemoryInterface* ram, DisarmReason* reason)
		{
			const MemberDesc& d = a.member->Desc();
			const Env& e = CurrentEnv();
			*reason = DisarmReason::GateClosed;
			const u8 g = d.session_gates;
			if (UiOwnsInput())
				return false;
			if ((g & SessionGate::LiveSnapshot) && snap.generation == 0)
				return false;
			if ((g & SessionGate::ActionsActive) && !snap.actions_active)
				return false;
			if ((g & SessionGate::NotStale) && snap.stale)
				return false;
			if ((g & SessionGate::NotSplit) && SplitActive())
				return false;
			if ((g & SessionGate::NotReplaying) && e.replaying && e.replaying())
				return false;
			if (d.skeleton_gates_needs && !NeedsMet(d, caps))
			{
				*reason = DisarmReason::NeedLost;
				return false;
			}
			if (d.online == Online::Offline && IsOnline())
			{
				*reason = DisarmReason::OnlineLock;
				return false;
			}
			if (!a.req.gates.empty() && (!ram || !GatePass(*ram, a.req.gates)))
				return false;
			return true;
		}

		void Drain(Armed& a, Outputs& out)
		{
			for (const auto& [bit, count] : out.presses)
			{
				a.sched.Press(bit, count);
#ifndef VR_PADXR_PLANTED_STATUS_FROZEN
				a.st.presses++;
#endif
			}
			out.presses.clear();
			for (const Cue& c : out.cues)
			{
				if (Deliver(a.req.seat, c))
					a.st.cues_sent++;
				else
					a.st.cues_dropped++;
			}
			out.cues.clear();
			out.hosts.clear();
			a.st.stick_writes += out.stick_writes;
			out.stick_writes = 0;
		}

		u16 ClaimedOwnBits(const MemberDesc& d)
		{
			if (d.harness)
				return 0xFFFFu;
			u16 m = 0;
			for (size_t i = 0; i < d.buttons.size(); i++)
				if (d.buttons[i] == Claim::Own)
					m |= static_cast<u16>(1u << i);
			return m;
		}

		bool SeatClaimsCompatible(const MemberDesc& a, const MemberDesc& b, bool pad_cells, Res* clash)
		{
			if (pad_cells)
				return ClaimsCompatible(a, b, clash);
			if (a.harness || b.harness)
				return true;
			for (size_t i = 0; i < kResCount; i++)
			{
				const Res r = static_cast<Res>(i);
				if (r == Res::Layers || r == Res::RightStick || r == Res::LeftStick)
					continue;
				if (!ClaimPairCompatible(a.res[i], b.res[i]))
				{
					if (clash)
						*clash = r;
					return false;
				}
			}
			return true;
		}

		u16 ReleasingBits(const MemberDesc& d) { return d.harness ? u16{0} : ClaimedOwnBits(d); }

	}

	u16 StepHolding(EdgeScheduler& s, u16 held)
	{
#ifndef VR_PADXR_PLANTED_INFLIGHT_PRESS_REPLAYED
		if (const u16 in_flight = static_cast<u16>(held & s.down_last & ~s.fresh); in_flight != 0)
			for (int bit = 0; bit < kButtonBits; bit++)
				if (in_flight & (1u << bit))
					s.remaining[bit] = 0;
#endif
		std::array<u32, kButtonBits> keep{};
		for (int bit = 0; bit < kButtonBits; bit++)
			if (held & (1u << bit))
				keep[bit] = s.remaining[bit];
		const u16 down = s.Step();
		if (held == 0)
			return down;
		for (int bit = 0; bit < kButtonBits; bit++)
			if (held & (1u << bit))
				s.remaining[bit] = keep[bit];
		s.down_last = static_cast<u16>(s.down_last & ~held);
		return static_cast<u16>(down & ~held);
	}

	namespace
	{
		void Fold(PortCells& p, const Outputs& o, u16 sched_down, bool harness, u16 claimed)
		{
			const u16 pressed = static_cast<u16>(o.hold_mask | sched_down);
			p.claimed_press |= static_cast<u16>(pressed & claimed);
			if (const u16 cp = static_cast<u16>((o.pressures.mask | o.pressures.own_mask) & claimed); cp != 0)
			{
				PressureCell mine = o.pressures;
				mine.mask &= cp;
				mine.own_mask &= cp;
				if (harness)
					p.claimed_harness_pressure |= static_cast<u16>(mine.own_mask & ~p.claimed_pressure.own_mask);
				MergePressureCell(p.claimed_pressure, mine);
			}
#ifdef VR_PADXR_PLANTED_UNCLAIMED_OWN
			claimed = 0xFFFFu;
#endif
			if (harness)
			{
				for (size_t i = 0; i < kAxisCount; i++)
					if (o.sticks[i].has_own && !p.sticks[i].has_own)
						p.harness_sticks |= static_cast<u8>(1u << i);
				p.harness_pressure |= static_cast<u16>(o.pressures.own_mask & ~p.pressure.own_mask);
			}
			p.word.press |= pressed;
			const u16 taken = static_cast<u16>(o.own_mask & claimed & ~p.claimed_own);
			p.claimed_own |= taken;
			p.word.own_mask |= taken;
			p.word.own_down = static_cast<u16>((p.word.own_down & ~taken) | (o.own_down & taken));
			const u16 fresh_unclaimed = static_cast<u16>(o.own_mask & ~claimed & ~p.word.own_mask);
			p.word.own_mask |= fresh_unclaimed;
			p.word.own_down |= static_cast<u16>(o.own_down & fresh_unclaimed);
			for (size_t i = 0; i < kAxisCount; i++)
				p.sticks[i].Merge(o.sticks[i]);
			MergePressureCell(p.pressure, o.pressures);
		}
	}

	namespace
	{
		class PadLookMember final : public Member
		{
		public:
			const MemberDesc& Desc() const override { return PadLookDesc(); }
			void Tick(const TickContext&, Outputs& out) override
			{
				const Env& e = CurrentEnv();
				out.stickUnits(Axis::RX, e.padlook_units ? e.padlook_units() : 0);
			}
		};
	}


	const MemberDesc& PadLookDesc()
	{
		static const MemberDesc d = [] {
			MemberDesc m;
			m.id = "padLook";
			m.kind = kPadLookKind;
			m.primitives = Prim::R1Source | Prim::R7Output | Prim::R8Gate | Prim::R9Resource | Prim::R12GameState;
			m.needs = {Need::HeadPose, 0, 0, 0};
#ifdef VR_PADXR_PLANTED_MANIFEST_DRIFT
			m.res[static_cast<size_t>(Res::RightStick)] = Claim::Set;
#else
			m.res[static_cast<size_t>(Res::RightStick)] = Claim::Add;
#endif
			m.family = Family::PAD;
			m.route = Route::Merge;
			m.online = Online::Any;
			m.clock = Clock::Game;
			m.session_gates = SessionGate::NotSplit;
			m.skeleton_gates_needs = false;
			return m;
		}();
		return d;
	}

	bool RegisterKind(std::string_view kind, Factory f)
	{
		std::lock_guard lock(s_mutex);
		for (const Kind& k : Kinds())
			if (k.name == kind)
				return false;
		Kinds().push_back({std::string(kind), f});
		return true;
	}

	std::unique_ptr<Member> Create(std::string_view kind)
	{
		Factory f = nullptr;
		{
			std::lock_guard lock(s_mutex);
			for (const Kind& k : Kinds())
				if (k.name == kind)
					f = k.factory;
		}
		return f ? f() : nullptr;
	}

	bool Arm(std::unique_ptr<Member> member, const ArmRequest& req, std::string* why)
	{
		if (!member)
		{
			Refuse("?", "no member", why);
			return false;
		}
		const MemberDesc& d = member->Desc();
		if (req.seat >= kSeats || req.port >= kPorts)
		{
			Refuse(d.id, "seat or port out of range", why);
			return false;
		}
		if (d.online == Online::Offline && IsOnline())
		{
			Refuse(d.id, "online (the network adapter is on, or the online lock)", why);
			return false;
		}
		std::lock_guard lock(s_mutex);
		for (const auto& a : s_armed)
		{
			const bool same_seat = a->req.seat == req.seat;
			const bool same_port = a->req.port == req.port;
#ifndef VR_PADXR_PLANTED_SEAT_ONLY_STICKS
			if (const MemberDesc& o = a->member->Desc();
				!same_seat && a->req.port == req.port && !d.harness && !o.harness)
			{
				for (const Res r : {Res::RightStick, Res::LeftStick})
				{
					if (!ClaimPairCompatible(d.Of(r), o.Of(r)))
					{
						Refuse(d.id, std::string("claim clash on ") + ResName(r) + " with " + std::string(o.id), why);
						return false;
					}
				}
			}
#endif
#ifdef VR_PADXR_PLANTED_SEAT_ONLY_OWNERS
			const bool same_pad = false;
#else
			const bool same_pad = a->req.port == req.port;
#endif
			if (!same_seat && !same_pad)
				continue;
#ifdef VR_PADXR_PLANTED_SEAT_BLOCKS_OTHER_PAD
			const bool pad_cells = true;
#else
			const bool pad_cells = same_port;
#endif
			const MemberDesc& o = a->member->Desc();
			if (same_seat)
			{
				if (o.id == d.id && same_port)
				{
					Refuse(d.id, "already armed", why);
					return false;
				}
				Res clash = Res::Count;
				if (!SeatClaimsCompatible(d, o, pad_cells, &clash))
				{
					Refuse(d.id, std::string("claim clash on ") + ResName(clash) + " with " + std::string(o.id), why);
					return false;
				}
			}
			if (int bit = -1; (same_pad || pad_cells) && !ButtonClaimsCompatible(d, o, &bit))
			{
				Refuse(d.id, std::string("button claim clash on ") + kButtonNames[static_cast<size_t>(bit)] + " with " +
								 std::string(o.id), why);
				return false;
			}
		}
		std::string member_why;
		if (!member->Arm(&member_why))
		{
			Refuse(d.id, member_why.empty() ? std::string("refused by the member") : member_why, why);
			return false;
		}
		auto a = std::make_unique<Armed>();
		a->st.id = std::string(d.id);
		a->st.kind = std::string(d.kind);
		a->st.seat = req.seat;
		a->st.port = req.port;
		a->member = std::move(member);
		a->req = req;
		Log("armed " + a->st.id + " (" + a->st.kind + ") on seat " + std::to_string(req.seat) + " port " +
			std::to_string(req.port));
		s_armed.push_back(std::move(a));
		return true;
	}

	void DisarmAll(DisarmReason reason)
	{
		std::lock_guard lock(s_mutex);
		for (auto& a : s_armed)
		{
			a->member->Disarm(reason);
			Log("disarmed " + Name(*a) + ": " + ReasonName(reason));
		}
		s_armed.clear();
		s_builtins.store(false, std::memory_order_release);
	}

	void EnsureBuiltins()
	{
		if (s_builtins.load(std::memory_order_acquire)) [[likely]]
			return;
		{
			std::lock_guard lock(s_mutex);
			if (s_builtins.load(std::memory_order_relaxed))
				return;
			s_builtins.store(true, std::memory_order_release);
		}
		RegisterKind(kPadLookKind, [] { return std::unique_ptr<Member>(new PadLookMember()); });
		std::string why;
		if (!Arm(Create(kPadLookKind), ArmRequest{}, &why))
			Log("padLook built-in not armed: " + why);
#ifdef VR_PADXR_PLANTED_DESCRIPTOR_ARMED
		struct DescMember final : Member
		{
			explicit DescMember(const MemberDesc* d) : m_d(d) {}
			const MemberDesc& Desc() const override { return *m_d; }
			const MemberDesc* m_d;
		};
		for (const MemberDesc& dd : DescriptorOnlyMembers())
			Arm(std::make_unique<DescMember>(&dd), ArmRequest{}, &why);
#endif
	}

	void Tick()
	{
		EnsureBuiltins();
		const Env& e = CurrentEnv();
		const VRInputSnapshot snap = e.snapshot ? e.snapshot() : VRInputSnapshot{};
		const u32 caps = CapsFromSnapshot(snap);
		MemoryInterface* ram = e.ram ? e.ram() : nullptr;
		const double wall = e.wall_ms ? e.wall_ms() : 0.0;

		std::lock_guard lock(s_mutex);
		s_tick++;
#ifdef VR_PADXR_PLANTED_TICK_NEUTRAL
		s_ports[0].sticks[static_cast<size_t>(Axis::RX)].Write(Claim::Own, 0.0f);
		s_ports[0].any = true;
#endif
		for (auto& ap : s_armed)
		{
			Armed& a = *ap;
			DisarmReason reason;
			if (!Gated(a, snap, caps, ram, &reason))
			{
				if (a.open)
				{
					a.open = false;
					a.release_due = true;
					a.tick_out.Clear();
					a.sched.Reset();
					a.member->Disarm(reason);
					Log("disarmed " + Name(a) + ": " + ReasonName(reason));
				}
				continue;
			}
			if (!a.open)
			{
				std::string why;
				if (!a.member->Arm(&why))
					continue;
				a.open = true;
				Log("armed " + Name(a) + " again (its gates hold)");
			}
			a.tick_out.Clear();
			TickContext ctx{a.req.seat, a.req.port, s_tick, wall, snap, caps, ram};
			a.member->Tick(ctx, a.tick_out);
			a.st.ticks++;
			Drain(a, a.tick_out);
		}
	}

	void OnStateLoaded()
	{
		std::lock_guard lock(s_mutex);
		for (auto& a : s_armed)
		{
			a->sched.Reset();
			a->tx_out.Clear();
			a->member->OnStateLoaded();
		}
	}

	void OnPollByte(u32 port, size_t byte_index)
	{
		if (byte_index == 3)
			OnTransaction(port);
	}

	void OnTransaction(u32 port)
	{
		if (port >= kPorts)
			return;
		EnsureBuiltins();
		PortCells& p = s_ports[port];
		p.transactions++;
#ifndef VR_PADXR_PLANTED_TICK_NEUTRAL
		p.ClearCells();
#endif
		std::lock_guard lock(s_mutex);
		if (s_armed.empty())
			return;
		const bool split = SplitActive();
		u16 releasing_now = 0;
		for (const auto& ap : s_armed)
		{
			const Armed& a = *ap;
			if (a.req.port != port || !a.release_due)
				continue;
			if (a.open && split && (a.member->Desc().session_gates & SessionGate::NotSplit))
				continue;
			releasing_now |= ReleasingBits(a.member->Desc());
		}
		for (auto& ap : s_armed)
		{
			Armed& a = *ap;
			if (a.req.port != port)
				continue;
			if (!a.open)
			{
				if (a.release_due)
					p.releasing |= ReleasingBits(a.member->Desc());
				a.release_due = false;
				continue;
			}
			if (split && (a.member->Desc().session_gates & SessionGate::NotSplit))
				continue;
			if (a.release_due)
			{
				a.release_due = false;
				p.releasing |= ReleasingBits(a.member->Desc());
				for (size_t i = 0; i < kAxisCount; i++)
					p.sticks[i].Merge(a.tick_out.sticks[i]);
				a.st.transactions++;
				continue;
			}
			a.tx_out.Clear();
			TransactionContext ctx{port, p.transactions};
			a.member->Transaction(ctx, a.tx_out);
			Drain(a, a.tx_out);
			const MemberDesc& desc = a.member->Desc();
			const u16 claimed = ClaimedOwnBits(desc);
#ifdef VR_PADXR_PLANTED_TAP_DROPPED_ON_RELEASE
			const u16 down = a.sched.Step();
#else
			const u16 down = StepHolding(a.sched, static_cast<u16>(releasing_now & ~claimed));
#endif
			a.st.transactions++;
			Fold(p, a.tick_out, down, desc.harness, claimed);
			Fold(p, a.tx_out, 0, desc.harness, claimed);
		}
#ifndef VR_PADXR_PLANTED_RELEASE_EDGE_LOST
		if (const u16 drop = static_cast<u16>(p.releasing & p.word.own_mask & ~p.claimed_own); drop != 0)
		{
			p.word.own_mask &= static_cast<u16>(~drop);
			p.word.own_down &= static_cast<u16>(~drop);
		}
#endif
#ifndef VR_PADXR_PLANTED_HOLD_PRESS_KEEP_RELEASING
		if (const u16 rel = p.releasing; rel != 0)
		{
			p.word.press = static_cast<u16>((p.word.press & ~rel) | (p.claimed_press & rel));
			p.pressure.mask = static_cast<u16>((p.pressure.mask & ~rel) | (p.claimed_pressure.mask & rel));
			p.pressure.own_mask = static_cast<u16>((p.pressure.own_mask & ~rel) | (p.claimed_pressure.own_mask & rel));
			for (int bit = 0; bit < kButtonBits; bit++)
				if (rel & (1u << bit))
					p.pressure.value[bit] = p.claimed_pressure.value[bit];
#ifndef VR_PADXR_PLANTED_PROBE_REWRITES_RELEASED_HARNESS
			p.harness_pressure = static_cast<u16>((p.harness_pressure & ~rel) | (p.claimed_harness_pressure & rel));
#endif
		}
#endif
		p.any = !p.word.Empty() || !p.pressure.Empty();
		for (const StickCell& s : p.sticks)
			p.any = p.any || !s.Empty();
	}

	bool UiOwnsInput()
	{
#ifdef VR_PADXR_PLANTED_NO_UI_GATE
		return false;
#else
		const Env& e = CurrentEnv();
		return e.ui_owns_input && e.ui_owns_input();
#endif
	}

	u32 Word(u32 port, u32 raw)
	{
		if (UiOwnsInput()) [[unlikely]]
			return raw | 0xFFFFu;
		if (port >= kPorts || !s_ports[port].any) [[likely]]
			return raw;
		return MergeWord(raw, s_ports[port].word);
	}

	u8 Stick(u32 port, Axis axis, u8 real)
	{
		if (port >= kPorts || axis >= Axis::Count)
			return real;
		if (UiOwnsInput()) [[unlikely]]
			return kUiRestStick;
		if (port == 0 && axis == Axis::RX)
		{
			if (SplitActive())
				return real;
			if (const Env& e = CurrentEnv(); e.rx_lanes)
				if (u8 owned; e.rx_lanes(real, &owned))
					return owned;
		}
		const PortCells& p = s_ports[port];
		if (!p.any) [[likely]]
			return real;
		return p.sticks[static_cast<size_t>(axis)].Apply(real);
	}

	u8 PollStick(u32 port, Axis axis, u8 real)
	{
		if (UiOwnsInput()) [[unlikely]]
			return kUiRestStick;
		const Env& e = CurrentEnv();
#ifdef VR_PADXR_PLANTED_PROBE_FIRST
		return Stick(port, axis, (port == 0 && e.probe_stick) ? e.probe_stick(axis, real) : real);
#else
		const u8 merged = Stick(port, axis, real);
		const bool harness_owned = (s_ports[port < kPorts ? port : 0].harness_sticks >> static_cast<int>(axis)) & 1;
		return (port == 0 && e.probe_stick && !harness_owned) ? e.probe_stick(axis, merged) : merged;
#endif
	}

	bool PressureActive(u32 port) { return port < kPorts && !UiOwnsInput() && !s_ports[port].pressure.Empty(); }

	int Pressure(u32 port, size_t poll_byte, u8 real)
	{
		const int bit = PressureBitOfByte(poll_byte);
		if (port >= kPorts || bit < 0)
			return -1;
		if (UiOwnsInput()) [[unlikely]]
			return 0;
		const int merged = MergePressure(s_ports[port].pressure, bit, real);
		if (merged < 0)
			return -1;
		const Env& e = CurrentEnv();
		const bool harness_owned = (s_ports[port].harness_pressure >> bit) & 1;
		return (port == 0 && e.probe_pressure && !harness_owned) ? e.probe_pressure(bit, static_cast<u8>(merged)) : merged;
	}

	namespace
	{
		class ReaderMemory final : public MemoryInterface
		{
		public:
			explicit ReaderMemory(GuestReadFn read) : m_read(read) {}
			bool Failed() const { return m_failed; }

			u8 Read8(u32 a, bool* valid) override { return static_cast<u8>(Read(a, 1, valid)); }
			u16 Read16(u32 a, bool* valid) override { return static_cast<u16>(Read(a, 2, valid)); }
			u32 Read32(u32 a, bool* valid) override { return Read(a, 4, valid); }
			u64 Read64(u32 a, bool* valid) override { return Read(a, 4, valid) | (static_cast<u64>(Read(a + 4, 4, valid)) << 32); }
			u128 Read128(u32, bool* valid) override
			{
				Fail(valid);
				return u128{};
			}
			bool ReadBytes(u32 a, void* dest, u32 size) override
			{
				u8* d = static_cast<u8*>(dest);
				for (u32 i = 0; i < size; i++)
					d[i] = Read8(a + i, nullptr);
				return !m_failed;
			}
			bool Write8(u32, u8) override { return false; }
			bool Write16(u32, u16) override { return false; }
			bool Write32(u32, u32) override { return false; }
			bool Write64(u32, u64) override { return false; }
			bool Write128(u32, u128) override { return false; }
			bool WriteBytes(u32, const void*, u32) override { return false; }
			bool CompareBytes(u32, const void*, u32) override { return false; }

		private:
			void Fail(bool* valid)
			{
#ifndef VR_PADXR_PLANTED_GATE_FAILOPEN
				m_failed = true;
#endif
				if (valid)
					*valid = false;
			}

			u32 Read(u32 a, u8 width, bool* valid)
			{
				u32 v = 0;
				if (!m_read || !m_read(a, width, &v))
				{
					Fail(valid);
					return 0;
				}
				if (valid)
					*valid = true;
				return v;
			}

			GuestReadFn m_read;
			bool m_failed = false;
		};
	}

	bool GatePass(MemoryInterface& mem, const std::vector<ProfileDB::CameraGuard>& guards)
	{
		return GuestRam::GuardListPass(mem, guards);
	}

	bool GatePass(GuestReadFn read, const std::vector<ProfileDB::CameraGuard>& guards)
	{
		ReaderMemory mem(read);
		const bool pass = GuestRam::GuardListPass(mem, guards);
		return pass && !mem.Failed();
	}

	bool SendCue(u32 seat, const Cue& c) { return Deliver(seat, c); }

	CueCounts GetCueCounts()
	{
		return {s_cues_sent.load(std::memory_order_relaxed), s_cues_dropped.load(std::memory_order_relaxed)};
	}

	namespace
	{
		class AdapterReportMember final : public Member
		{
		public:
			const MemberDesc& Desc() const override { return SpatialControlsDesc(); }
		};
	}

	const MemberDesc& SpatialControlsDesc()
	{
		static const MemberDesc d = [] {
			MemberDesc m;
			m.id = "spatial-controls";
			m.kind = "spatial-device";
			m.primitives = Prim::R1Source | Prim::R2Frame | Prim::R5Solver | Prim::R7Output | Prim::R8Gate |
						   Prim::R9Device | Prim::R10Feedback | Prim::R12GameState;
			m.needs = {Need::Controllers, 0, 0, 0};
			m.res[static_cast<size_t>(Res::Haptics)] = Claim::Shared;
			m.layers = 2;
			m.family = Family::PAD;
			m.route = Route::Adapter;
			m.online = Online::Any;
			m.clock = Clock::Wall;
			m.session_gates = 0;
			m.skeleton_gates_needs = false;
			return m;
		}();
		return d;
	}

	namespace
	{
		MemberDesc DescOnly(std::string_view id, std::string_view kind, Family family, Route route, u16 primitives,
			u32 need, Online online)
		{
			MemberDesc m;
			m.id = id;
			m.kind = kind;
			m.family = family;
			m.route = route;
			m.primitives = primitives;
			m.needs = {need, 0, 0, 0};
			m.online = online;
			m.session_gates = 0;
			m.skeleton_gates_needs = false;
			return m;
		}
	}

	const std::vector<MemberDesc>& DescriptorOnlyMembers()
	{
		static const std::vector<MemberDesc> v = [] {
			constexpr u16 cam = Prim::R1Source | Prim::R7Output | Prim::R8Gate | Prim::R12GameState;
			constexpr size_t rs = static_cast<size_t>(Res::RightStick), hap = static_cast<size_t>(Res::Haptics);
			std::vector<MemberDesc> m;
			m.push_back(DescOnly("firstPerson.native", "firstPerson", Family::CAM, Route::Camera, cam, Need::HeadPose, Online::Offline));
			for (const std::string_view id : {"headLook.absolute", "headLook.anchored", "headLook.codeHook", "headLook.delta",
					 "headLook.matrix"})
				m.push_back(DescOnly(id, "headLook", Family::CAM, Route::Camera, cam, Need::HeadPose, Online::Offline));
			m.push_back(DescOnly("headLook.directLook", "headLook", Family::CAM, Route::Camera, cam, Need::HeadPose, Online::Offline));
			m.back().res[rs] = Claim::Own;
			m.push_back(DescOnly("headLook.worldProjection", "headLook", Family::CAM, Route::Camera, cam | Prim::R2Frame,
				Need::HeadPose, Online::Offline));
			m.back().layers = 1;
			m.push_back(DescOnly("hapticGloves", "haptics-backend", Family::RF, Route::None,
				Prim::R9Resource | Prim::R10Feedback | Prim::R11Seat, Need::Haptics, Online::Any));
			m.back().res[hap] = Claim::Shared;
			m.push_back(DescOnly("padChord", "chord", Family::HOST, Route::Host, Prim::R7Output | Prim::R9Port, Need::Pad, Online::Any));
			m.push_back(DescOnly("thirdPerson.followGlance", "thirdPerson", Family::PAD, Route::Merge,
				Prim::R1Source | Prim::R7Output | Prim::R8Gate, Need::HeadPose, Online::Any));
			m.back().res[rs] = Claim::Add;
			m.push_back(DescOnly("verb.look", "verb", Family::PAD, Route::Merge, Prim::R1Source | Prim::R7Output | Prim::R8Gate,
				Need::HeadPose, Online::Any));
			m.back().res[rs] = Claim::Add;
			m.push_back(DescOnly("verb.vehicle", "verb", Family::PAD, Route::Adapter,
				Prim::R1Source | Prim::R5Solver | Prim::R7Output | Prim::R8Gate | Prim::R9Device | Prim::R10Feedback |
					Prim::R12GameState,
				Need::Controllers, Online::Any));
			m.back().res[hap] = Claim::Shared;
#ifdef VR_PADXR_PLANTED_DROP_DESCRIPTOR
			m.pop_back();
#endif
			return m;
		}();
		return v;
	}

	void ReportAdapter(u32 seat, bool live)
	{
		{
			std::lock_guard lock(s_mutex);
			const auto it = std::find_if(s_armed.begin(), s_armed.end(), [&](const auto& a) {
				return a->req.seat == seat && a->member->Desc().id == SpatialControlsDesc().id;
			});
			if ((it != s_armed.end()) == live)
				return;
			if (!live)
			{
				(*it)->member->Disarm(DisarmReason::ProfileChanged);
				Log("disarmed spatial-controls: the adapter reports its controls gone");
				s_armed.erase(it);
				return;
			}
		}
		ArmRequest req;
		req.seat = seat;
		std::string why;
		if (!Arm(std::make_unique<AdapterReportMember>(), req, &why))
			Log("spatial-controls adapter report refused (" + why + "); the adapter route itself is unaffected");
	}

	std::vector<MemberStatus> Status()
	{
		std::lock_guard lock(s_mutex);
		std::vector<MemberStatus> out;
		out.reserve(s_armed.size());
		for (const auto& a : s_armed)
		{
			MemberStatus s = a->st;
			s.gated_open = a->open;
			out.push_back(std::move(s));
		}
		return out;
	}

	u64 Refusals() { return s_refusals.load(std::memory_order_relaxed); }

	namespace
	{
		std::string Join(const std::vector<std::string>& v, const char* sep)
		{
			if (v.empty())
				return "-";
			std::string s;
			for (size_t i = 0; i < v.size(); i++)
				s += (i ? sep : "") + v[i];
			return s;
		}

		const char* ClaimName(Claim c)
		{
			static constexpr const char* n[] = {"none", "shared", "tee", "exclusive", "add", "set", "own"};
			return n[static_cast<size_t>(c) < 7 ? static_cast<size_t>(c) : 0];
		}
	}

	std::string ManifestLine(const MemberDesc& d)
	{
		static constexpr const char* fam[] = {"PAD", "PER", "RF", "STREAM", "HOST", "CAM"};
		static constexpr const char* route[] = {"merge", "adapter", "device", "stream", "host", "none", "camera"};
		static constexpr const char* online[] = {"any", "private", "offline"};
		static constexpr const char* prim[] = {"R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9P", "R9D", "R9R", "R10",
			"R11", "R12"};
		static constexpr const char* need[] = {"head-pose", "head-6dof", "controllers", "analog-trigger",
			"capacitive-touch", "hands", "finger-curl", "two-hands", "haptics", "mic", "eye-gaze", "angular-velocity",
			"screen-hit", "seat-calibration", "standing", "roomscale", "pad"};

		std::vector<std::string> prims;
		for (int i = 0; i < 14; i++)
			if (d.primitives & (1u << i))
				prims.emplace_back(prim[i]);
		std::vector<std::string> needs;
		for (const u32 g : d.needs)
		{
			if (!g)
				continue;
			std::vector<std::string> any;
			for (int i = 0; i < 17; i++)
				if (g & (1u << i))
					any.emplace_back(need[i]);
			needs.push_back(Join(any, "|"));
		}
		std::vector<std::string> res;
		for (size_t i = 0; i < kResCount; i++)
			if (static_cast<Res>(i) != Res::Layers && d.res[i] != Claim::None)
				res.push_back(std::string(ResName(static_cast<Res>(i))) + ":" + ClaimName(d.res[i]));
		if (d.layers)
			res.push_back("layers:" + std::to_string(d.layers));
		for (size_t i = 0; i < d.buttons.size(); i++)
			if (d.buttons[i] != Claim::None)
				res.push_back(std::string("buttons.") + kButtonNames[i] + ":" + ClaimName(d.buttons[i]));
		std::sort(res.begin(), res.end());

		return "id=" + std::string(d.id) + " kind=" + std::string(d.kind) + " family=" + fam[static_cast<size_t>(d.family)] +
			   " route=" + route[static_cast<size_t>(d.route)] + " primitives=" + Join(prims, ",") +
			   " needs=" + Join(needs, ",") + " resources=" + Join(res, ",") + " online=" + online[static_cast<size_t>(d.online)];
	}

	std::vector<std::string> Manifest()
	{
		std::vector<std::string> out = {ManifestLine(PadLookDesc()), ManifestLine(SpatialControlsDesc())};
		std::vector<Kind> kinds;
		{
			std::lock_guard lock(s_mutex);
			kinds = Kinds();
		}
		for (const Kind& k : kinds)
			if (std::unique_ptr<Member> m = k.factory ? k.factory() : nullptr; m && !m->Desc().harness)
				out.push_back(ManifestLine(m->Desc()));
		for (const MemberDesc& d : DescriptorOnlyMembers())
			out.push_back(ManifestLine(d));
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	Env DefaultEnv()
	{
		Env e;
#ifndef PADXR_STANDALONE
		e.snapshot = [] { return VR::GetInputSnapshot(); };
		e.ram = []() -> MemoryInterface* {
			static EEMemoryInterface s_ee;
			return eeMem ? &s_ee : nullptr;
		};
		e.online = [] { return EmuConfig.DEV9.EthEnable || VMManager::IsOnlineLocked(); };
		e.split_active = [] { return VR::SplitState::Active(); };
		e.padlook_units = [] { return VR::PadLook::PublishedRxUnits(); };
		e.rx_lanes = [](u8 real, u8* out) { return VR::PadLook::RxLanes(real, out); };
		e.replaying = [] { return g_InputRecording.isActive() && g_InputRecording.getControls().isReplaying(); };
		e.wall_ms = [] {
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
		};
		e.pulse = [](int hand, float amplitude, float seconds, u8 locus) {
			VR::XRInput::QueuePulse(hand, amplitude, seconds, locus);
		};
		e.log = [](const char* line) { Console.WriteLn("(VR) PadXR: %s", line); };
		e.ui_owns_input = [] { return VR::UiPanel::OwnsInput(); };
#endif
		return e;
	}

	void ResetForTest(const Env& env)
	{
		std::lock_guard lock(s_mutex);
		s_armed.clear();
		for (PortCells& p : s_ports)
			p = PortCells{};
		s_tick = 0;
		s_builtins.store(false, std::memory_order_release);
		s_refusals.store(0, std::memory_order_relaxed);
		s_cues_sent.store(0, std::memory_order_relaxed);
		s_cues_dropped.store(0, std::memory_order_relaxed);
		CurrentEnv() = env;
	}
}
