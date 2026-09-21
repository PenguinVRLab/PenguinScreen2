// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#include "VR/MemWatch.h"

#include "Counters.h"
#include "DebugTools/Breakpoints.h"
#include "R5900.h"

#include "common/Console.h"

namespace VR
{
	namespace
	{
		struct ArmedWatch
		{
			bool used = false;
			bool stop = false;
			u32 start = 0;
			u32 end = 0;
		};

		std::mutex s_armed_lock;
		ArmedWatch s_armed[MEMWATCH_MAX];
		MemWatchTable s_table;
	}

	int MemWatchArm(u32 address, u32 size, u8 cond, bool stop)
	{
		if (size == 0 || size > 16)
		{
			Console.WarningFmt("(VR) MemWatchArm: size {} is not 1..16", size);
			return -1;
		}
		if (cond == 0 || cond > MEMCHECK_READWRITE)
		{
			Console.WarningFmt("(VR) MemWatchArm: cond {} is not 1 (read), 2 (write) or 3 (both)", cond);
			return -1;
		}

		const u32 start = address;
		const u32 end = address + size;

		int id = -1;
		{
			std::lock_guard<std::mutex> lock(s_armed_lock);
			for (int i = 0; i < MEMWATCH_MAX; i++)
			{
				if (s_armed[i].used && s_armed[i].start == start && s_armed[i].end == end)
				{
					id = i;
					break;
				}
			}
			if (id < 0)
			{
				for (int i = 0; i < MEMWATCH_MAX; i++)
				{
					if (!s_armed[i].used)
					{
						id = i;
						break;
					}
				}
			}
			if (id < 0)
			{
				Console.WarningFmt("(VR) MemWatchArm: all {} watch slots are in use", MEMWATCH_MAX);
				return -1;
			}
			s_armed[id].used = true;
			s_armed[id].stop = stop;
			s_armed[id].start = start;
			s_armed[id].end = end;
		}

		CBreakPoints::AddMemCheck(BREAKPOINT_EE, start, end, static_cast<MemCheckCondition>(cond), MEMCHECK_BREAK);
		CBreakPoints::ChangeMemCheckDescription(BREAKPOINT_EE, start, end, "vr-memwatch");

		Console.WriteLnFmt("(VR) memwatch {} armed: {:#010x}..{:#010x} cond={} stop={}",
			id, start, end, cond, stop ? "yes" : "no");
		return id;
	}

	void MemWatchClearAll()
	{
		ArmedWatch armed[MEMWATCH_MAX];
		{
			std::lock_guard<std::mutex> lock(s_armed_lock);
			for (int i = 0; i < MEMWATCH_MAX; i++)
			{
				armed[i] = s_armed[i];
				s_armed[i] = ArmedWatch{};
			}
		}

		for (int i = 0; i < MEMWATCH_MAX; i++)
		{
			if (armed[i].used)
				CBreakPoints::RemoveMemCheck(BREAKPOINT_EE, armed[i].start, armed[i].end);
		}

		s_table.Clear();
		Console.WriteLn("(VR) memwatch: all watches cleared");
	}

	bool MemWatchOnHit(u32 start, u32 end, bool is_write, u32 pc, u32 op)
	{
		const u32 s = standardizeBreakpointAddress(start);
		const u32 e = standardizeBreakpointAddress(end);

		int id = -1;
		bool stop = false;
		{
			std::lock_guard<std::mutex> lock(s_armed_lock);
			for (int i = 0; i < MEMWATCH_MAX; i++)
			{
				if (!s_armed[i].used)
					continue;
				if (standardizeBreakpointAddress(s_armed[i].start) != s)
					continue;
				if (standardizeBreakpointAddress(s_armed[i].end) != e)
					continue;
				id = i;
				stop = s_armed[i].stop;
				break;
			}
		}

		if (id < 0)
			return false;

		u64 gpr[32];
		for (int r = 0; r < 32; r++)
			gpr[r] = cpuRegs.GPR.r[r].UD[0];

		s_table.Record(static_cast<u8>(id), is_write, pc, op, g_FrameCount, gpr);

		return !stop;
	}

	std::vector<MemWatchHit> MemWatchDrain(u32* dropped_out)
	{
		return s_table.Drain(dropped_out);
	}

	u32 MemWatchArmedCount()
	{
		std::lock_guard<std::mutex> lock(s_armed_lock);
		u32 n = 0;
		for (int i = 0; i < MEMWATCH_MAX; i++)
			n += s_armed[i].used ? 1 : 0;
		return n;
	}
}
