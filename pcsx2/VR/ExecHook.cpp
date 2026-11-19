// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/ExecHook.h"

#include "DebugTools/Breakpoints.h"
#include "Host.h"
#include "Memory.h"
#include "R5900.h"
#include "VMManager.h"

#include "common/Console.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>

namespace VR
{
	namespace
	{
		struct Armed
		{
			bool used = false;
			ExecHookSpec spec;
		};

		std::mutex s_lock;
		Armed s_armed[EXECHOOK_MAX];
		std::atomic<u64> s_hits[EXECHOOK_MAX];
		std::atomic<u64> s_stale[EXECHOOK_MAX];
		ExecVisitRecord s_visits[EXECHOOK_MAX];
		u64 s_visit_seq = 0;

		static constexpr int CALLERS_LOGGED = 8;
		u32 s_callers[EXECHOOK_MAX][CALLERS_LOGGED];
		int s_caller_count[EXECHOOK_MAX];

		std::atomic<float> s_yaw{0.0f};
		std::atomic<float> s_pitch{0.0f};
		std::atomic<bool> s_active{false};

	}

	int ExecHookArm(const ExecHookSpec& spec)
	{
		if (spec.pc == 0 || (spec.pc & 3) != 0)
		{
			Console.WarningFmt("(VR) exec hook refused: {:#010x} is not an instruction address", spec.pc);
			return -1;
		}
		if (spec.expect_op == 0)
		{
			Console.Warning("(VR) exec hook refused: no `expect` word (the instruction the game must hold there)");
			return -1;
		}
		if ((spec.action == ExecAction::AddGpr || spec.action == ExecAction::AddFpr) && spec.reg > 31)
		{
			Console.WarningFmt("(VR) exec hook refused: register {} is not 0..31", spec.reg);
			return -1;
		}
		if (spec.action == ExecAction::AddGpr && spec.reg == 0)
		{
			Console.Warning("(VR) exec hook refused: register 0 is the constant zero");
			return -1;
		}
		if (spec.action == ExecAction::AimLookAt && (spec.reg > 31 || spec.reg2 == 0 || spec.reg2 > 31 || spec.reg == spec.reg2))
		{
			Console.WarningFmt("(VR) exec hook refused: a direction needs a register 1..31, an eye and a target two different ones (got {} and {})", spec.reg, spec.reg2);
			return -1;
		}
		if (spec.action == ExecAction::ComposeView && (spec.address == 0 || (spec.address & 3) != 0))
		{
			Console.WarningFmt("(VR) exec hook refused: {:#010x} is not a matrix address", spec.address);
			return -1;
		}
		if (spec.action == ExecAction::Record &&
			(spec.reg == 0 || spec.reg > 31 || (spec.record_offset & 3) != 0 || (spec.record_lever & 3) != 0))
		{
			Console.WarningFmt("(VR) exec hook refused: a record needs a base register 1..31 and aligned offsets (got {}, "
							   "{:#x}, {:#010x})", spec.reg, spec.record_offset, spec.record_lever);
			return -1;
		}

		for (const ExecGate& g : spec.gates)
		{
			if (g.width == 0)
				continue;
			if ((g.width != 1 && g.width != 2 && g.width != 4) || g.address < 0x00010000u || g.address > Ps2MemSize::ExposedRam - g.width)
			{
				Console.WarningFmt("(VR) exec hook refused: a gate of width {} at {:#010x} cannot be read", g.width, g.address);
				return -1;
			}
		}

		const u32 have = memRead32(spec.pc);
		if (have != spec.expect_op)
		{
			Console.WarningFmt("(VR) exec hook refused: the game holds {:#010x} at {:#010x}, the profile expects {:#010x} "
							   "(another disc revision, or the code is not loaded yet)",
				have, spec.pc, spec.expect_op);
			return -1;
		}

		int id = -1;
		{
			std::lock_guard<std::mutex> lock(s_lock);
			for (int i = 0; i < EXECHOOK_MAX; i++)
			{
				if (s_armed[i].used && s_armed[i].spec.pc == spec.pc)
				{
					if (!ExecHookSameAddressAllowed(s_armed[i].spec, spec))
					{
						Console.WarningFmt("(VR) exec hook refused: {:#010x} is already armed by another owner", spec.pc);
						return -1;
					}
					id = i;
					break;
				}
			}
			for (int i = 0; id < 0 && i < EXECHOOK_MAX; i++)
			{
				if (!s_armed[i].used)
					id = i;
			}
			if (id < 0)
			{
				Console.WarningFmt("(VR) exec hook refused: all {} slots are in use", EXECHOOK_MAX);
				return -1;
			}
			s_armed[id].used = true;
			s_armed[id].spec = spec;
			s_visits[id] = ExecVisitRecord{};
			s_visits[id].pc = spec.pc;
		}
		s_hits[id].store(0);
		s_stale[id].store(0);
		s_caller_count[id] = 0;

		if (!CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, spec.pc))
			CBreakPoints::AddBreakPoint(BREAKPOINT_EE, spec.pc);

		Console.WriteLnFmt("(VR) exec hook {} armed at {:#010x} (action {}, reg {}, address {:#010x})", id, spec.pc,
			static_cast<int>(spec.action), spec.reg, spec.address);
		return id;
	}

	void ExecHookClearAll(ExecOwner owner)
	{
		Armed armed[EXECHOOK_MAX];
		{
			std::lock_guard<std::mutex> lock(s_lock);
			for (int i = 0; i < EXECHOOK_MAX; i++)
			{
				if (!s_armed[i].used || s_armed[i].spec.owner != owner)
					continue;
				armed[i] = s_armed[i];
				s_armed[i] = Armed{};
				s_visits[i] = ExecVisitRecord{};
			}
		}
		int n = 0;
		for (int i = 0; i < EXECHOOK_MAX; i++)
		{
			if (!armed[i].used)
				continue;
			CBreakPoints::RemoveBreakPoint(BREAKPOINT_EE, armed[i].spec.pc);
			n++;
		}
		if (n > 0)
			Console.WriteLnFmt("(VR) exec hooks cleared ({})", n);
	}

	void ExecHookSetHead(float yaw_rad, float pitch_rad, bool active)
	{
		s_yaw.store(yaw_rad, std::memory_order_relaxed);
		s_pitch.store(pitch_rad, std::memory_order_relaxed);
		s_active.store(active, std::memory_order_relaxed);
	}

	bool ExecHookOnHit(u32 pc)
	{
		ExecHookSpec spec;
		int id = -1;
		{
			std::lock_guard<std::mutex> lock(s_lock);
			for (int i = 0; i < EXECHOOK_MAX; i++)
			{
				if (s_armed[i].used && s_armed[i].spec.pc == pc)
				{
					id = i;
					spec = s_armed[i].spec;
					break;
				}
			}
		}
		if (id < 0)
			return false;

		const u64 n = s_hits[id].fetch_add(1) + 1;
		if (n == 1)
			Console.WriteLnFmt("(VR) exec hook {} reached for the first time at {:#010x}", id, pc);

		if (spec.caller_sp_offset != 0)
		{
			const u32 caller = memRead32(cpuRegs.GPR.n.sp.UL[0] + spec.caller_sp_offset);
			bool seen = false;
			for (int i = 0; i < s_caller_count[id] && !seen; i++)
				seen = (s_callers[id][i] == caller);
			if (!seen && s_caller_count[id] < CALLERS_LOGGED)
			{
				s_callers[id][s_caller_count[id]++] = caller;
				Console.WriteLnFmt("(VR) exec hook {} at {:#010x}: called from {:#010x}{}", id, pc, caller,
					(spec.caller_expect == 0) ? "" : (caller == spec.caller_expect ? " (the caller it acts for)" : " (not its caller: left alone)"));
			}
			if (spec.caller_expect != 0 && caller != spec.caller_expect)
				return true;
		}

		if (!s_active.load(std::memory_order_relaxed) && spec.action != ExecAction::Record)
			return true;

		if (memRead32(pc) != spec.expect_op)
		{
			if (s_stale[id].fetch_add(1) == 0)
				Console.WarningFmt("(VR) exec hook {} at {:#010x}: the instruction changed since arming; the hook does nothing", id, pc);
			return true;
		}

		for (const ExecGate& g : spec.gates)
		{
			if (g.width == 0)
				continue;
			const u32 v = (g.width == 1) ? memRead8(g.address) : ((g.width == 2) ? memRead16(g.address) : memRead32(g.address));
			if (!ExecGateHolds(g, v))
				return true;
		}

		const float yaw = s_yaw.load(std::memory_order_relaxed);
		const float pitch = s_pitch.load(std::memory_order_relaxed);

		switch (spec.action)
		{
			case ExecAction::Record:
			{
				ExecVisitRecord r;
				const u32 base = cpuRegs.GPR.r[spec.reg].UL[0];
				if (!ExecReadVisit([](u32 a) { return memRead32(a); }, base, spec.record_offset, spec.record_lever,
						Ps2MemSize::ExposedRam, &r))
				{
					if (s_stale[id].fetch_add(1) == 0)
						Console.WarningFmt("(VR) exec hook {} at {:#010x}: register {} holds {:#010x}, no camera to record "
										   "there; the visit is not recorded", id, pc, spec.reg, base);
					break;
				}
				std::lock_guard<std::mutex> lock(s_lock);
				r.seq = ++s_visit_seq;
				r.pc = pc;
				s_visits[id] = r;
				break;
			}
			case ExecAction::AddGpr:
			{
				const s32 add = spec.use_pitch ? ExecAngleUnits(pitch, spec.pitch_units) : ExecAngleUnits(yaw, spec.yaw_units);
				const s32 now = static_cast<s32>(cpuRegs.GPR.r[spec.reg].UL[0]);
				cpuRegs.GPR.r[spec.reg].SD[0] = static_cast<s64>(static_cast<s32>(static_cast<u32>(now) + static_cast<u32>(add)));
				break;
			}
			case ExecAction::AddFpr:
			{
				const float add = spec.use_pitch ? pitch * spec.pitch_units : yaw * spec.yaw_units;
				if (std::isfinite(add))
					fpuRegs.fpr[spec.reg].f += add;
				break;
			}
			case ExecAction::ComposeView:
			{
				float m[16];
				for (int i = 0; i < 16; i++)
				{
					const u32 w = memRead32(spec.address + 4 * i);
					std::memcpy(&m[i], &w, 4);
				}
				ExecComposeView(m, yaw * spec.yaw_units, pitch * spec.pitch_units);
				for (int i = 0; i < 16; i++)
				{
					u32 w;
					std::memcpy(&w, &m[i], 4);
					memWrite32(spec.address + 4 * i, w);
				}
				break;
			}
			case ExecAction::AimLookAt:
			{
				const bool direction = (spec.reg == 0);
				const u32 target_at = cpuRegs.GPR.r[spec.reg2].UL[0];
				const u32 eye_at = direction ? 0 : cpuRegs.GPR.r[spec.reg].UL[0];
				const auto in_ram = [](u32 a) { return a >= 0x00010000u && (a & 3) == 0 && a <= Ps2MemSize::ExposedRam - 12; };
				if (!in_ram(target_at) || (!direction && (!in_ram(eye_at) || eye_at == target_at)))
				{
					if (s_stale[id].fetch_add(1) == 0)
						Console.WarningFmt("(VR) exec hook {} at {:#010x}: the registers do not hold what the hook turns ({:#010x}, {:#010x}); the hook does nothing", id, pc, eye_at, target_at);
					break;
				}
				float eye[3] = {0.0f, 0.0f, 0.0f}, target[3];
				for (int i = 0; i < 3; i++)
				{
					const u32 wt = memRead32(target_at + 4 * i);
					std::memcpy(&target[i], &wt, 4);
					if (!direction)
					{
						const u32 we = memRead32(eye_at + 4 * i);
						std::memcpy(&eye[i], &we, 4);
					}
				}
				if (ExecAimLookAt(eye, target, yaw * spec.yaw_units, pitch * spec.pitch_units))
				{
					for (int i = 0; i < 3; i++)
					{
						u32 w;
						std::memcpy(&w, &target[i], 4);
						memWrite32(target_at + 4 * i, w);
					}
				}
				break;
			}
		}
		return true;
	}

	void ExecHookForgetVisits(ExecOwner owner)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		for (int i = 0; i < EXECHOOK_MAX; i++)
		{
			if (s_armed[i].used && s_armed[i].spec.owner == owner)
			{
				const u32 pc = s_visits[i].pc;
				s_visits[i] = ExecVisitRecord{};
				s_visits[i].pc = pc;
			}
		}
	}

	bool ExecHookLastVisit(u32 pc, ExecVisitRecord* out)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		for (int i = 0; i < EXECHOOK_MAX; i++)
		{
			if (s_armed[i].used && s_armed[i].spec.pc == pc && s_armed[i].spec.action == ExecAction::Record)
			{
				if (s_visits[i].seq == 0)
					return false;
				*out = s_visits[i];
				return true;
			}
		}
		return false;
	}

	u32 ExecHookArmedCount()
	{
		std::lock_guard<std::mutex> lock(s_lock);
		u32 n = 0;
		for (int i = 0; i < EXECHOOK_MAX; i++)
			n += s_armed[i].used ? 1 : 0;
		return n;
	}

	bool ExecHookIsArmedAt(u32 pc)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		for (int i = 0; i < EXECHOOK_MAX; i++)
		{
			if (s_armed[i].used && s_armed[i].spec.pc == pc)
				return true;
		}
		return false;
	}


	u64 ExecHookHits(int id)
	{
		if (id < 0 || id >= EXECHOOK_MAX)
			return 0;
		return s_hits[id].load();
	}

}
