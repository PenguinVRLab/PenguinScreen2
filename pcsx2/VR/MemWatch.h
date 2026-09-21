// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Types.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

namespace VR
{
	struct MemWatchHit
	{
		u32 pc;
		u32 op;
		u32 hits;
		u32 frame;
		u8 watch;
		u8 is_write;
		u16 reserved;
		u32 reserved2;
		u64 gpr[32];
	};

	static_assert(sizeof(MemWatchHit) == 280, "MemWatchHit is a wire format");
	static_assert(offsetof(MemWatchHit, gpr) == 24, "MemWatchHit is a wire format");

	class MemWatchTable
	{
	public:
		static constexpr u32 MAX_ROWS = 64;

		void Record(u8 watch, bool is_write, u32 pc, u32 op, u32 frame, const u64* gpr)
		{
			std::lock_guard<std::mutex> lock(m_lock);
			for (MemWatchHit& row : m_rows)
			{
				if (row.pc == pc && row.watch == watch && row.is_write == (is_write ? 1 : 0))
				{
					if (row.hits != 0xFFFFFFFFu)
						row.hits++;
					return;
				}
			}

			if (m_rows.size() >= MAX_ROWS)
			{
				if (m_dropped != 0xFFFFFFFFu)
					m_dropped++;
				return;
			}

			MemWatchHit row = {};
			row.pc = pc;
			row.op = op;
			row.hits = 1;
			row.frame = frame;
			row.watch = watch;
			row.is_write = is_write ? 1 : 0;
			if (gpr)
				std::memcpy(row.gpr, gpr, sizeof(row.gpr));
			m_rows.push_back(row);
		}

		std::vector<MemWatchHit> Drain(u32* dropped_out = nullptr)
		{
			std::lock_guard<std::mutex> lock(m_lock);
			if (dropped_out)
				*dropped_out = m_dropped;
			std::vector<MemWatchHit> rows;
			rows.swap(m_rows);
			m_dropped = 0;
			return rows;
		}

		u32 Rows() const
		{
			std::lock_guard<std::mutex> lock(m_lock);
			return static_cast<u32>(m_rows.size());
		}

		u32 Dropped() const
		{
			std::lock_guard<std::mutex> lock(m_lock);
			return m_dropped;
		}

		void Clear()
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_rows.clear();
			m_dropped = 0;
		}

		static std::vector<u8> Serialize(const std::vector<MemWatchHit>& rows, u32 dropped)
		{
			std::vector<u8> out(8 + rows.size() * sizeof(MemWatchHit));
			const u32 count = static_cast<u32>(rows.size());
			std::memcpy(out.data() + 0, &count, sizeof(count));
			std::memcpy(out.data() + 4, &dropped, sizeof(dropped));
			if (!rows.empty())
				std::memcpy(out.data() + 8, rows.data(), rows.size() * sizeof(MemWatchHit));
			return out;
		}

	private:
		mutable std::mutex m_lock;
		std::vector<MemWatchHit> m_rows;
		u32 m_dropped = 0;
	};

	static constexpr int MEMWATCH_MAX = 8;

	int MemWatchArm(u32 address, u32 size, u8 cond, bool stop);

	void MemWatchClearAll();

	bool MemWatchOnHit(u32 start, u32 end, bool is_write, u32 pc, u32 op);

	std::vector<MemWatchHit> MemWatchDrain(u32* dropped_out = nullptr);

	u32 MemWatchArmedCount();
}
