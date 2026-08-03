// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Pcsx2Defs.h"

#include <vector>

class ReadbackSpinManager
{
public:
	struct Event
	{
		s64 size;
		u32 begin;
		u32 end;
	};

private:
	double m_spins_per_unit_time = 0;
	double m_total_spin_time = 0;
	double m_total_spin_cycles = 0;
	std::vector<Event> m_frames[3];
	u32 m_current_frame = 0;
	u32 m_reference_frame = 0;
	u32 m_reference_frame_idx = 0;

public:
	struct DrawSubmittedReturn
	{
		u32 id;
		u32 recommended_spin;
	};

	void ReadbackRequested();
	void NextFrame();
	DrawSubmittedReturn DrawSubmitted(u64 size);
	void DrawCompleted(u32 id, u32 begin_time, u32 end_time);
	void SpinCompleted(u32 cycles, u32 begin_time, u32 end_time);
	double SpinsPerUnitTime() const { return m_spins_per_unit_time; }
};
