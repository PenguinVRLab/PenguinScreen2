// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>

namespace VR::SeatCast
{
	static constexpr u32 kMaxWidth = 1280;

	struct Frame
	{
		u32 width = 0;
		u32 height = 0;
		u32 stride = 0;
		u64 seq = 0;
		const u8* pixels = nullptr;
	};

	void Publish(const void* pixels, u32 width, u32 height, u32 stride);

	bool ConsumeLatest(u64 last_seq, u8* dst, size_t dst_size, Frame* out);

	void Shutdown();
}
