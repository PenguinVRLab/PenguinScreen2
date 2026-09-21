// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/SeatCast.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace VR::SeatCast
{
	namespace
	{
		std::mutex s_mutex;
		std::vector<u8> s_store;
		Frame s_meta;
		u64 s_seq = 0;
	}

	void Publish(const void* pixels, u32 width, u32 height, u32 stride)
	{
		if (!pixels || !width || !height)
			return;
		const size_t bytes = static_cast<size_t>(stride) * height;
		std::lock_guard<std::mutex> lock(s_mutex);
		s_store.resize(bytes);
		std::memcpy(s_store.data(), pixels, bytes);
		s_meta.width = width;
		s_meta.height = height;
		s_meta.stride = stride;
		s_meta.seq = ++s_seq;
	}

	bool ConsumeLatest(u64 last_seq, u8* dst, size_t dst_size, Frame* out)
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_meta.seq == 0 || s_meta.seq == last_seq)
			return false;
		const size_t bytes = static_cast<size_t>(s_meta.stride) * s_meta.height;
		if (bytes > dst_size)
			return false;
		std::memcpy(dst, s_store.data(), bytes);
		*out = s_meta;
		out->pixels = dst;
		return true;
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_store.clear();
		s_store.shrink_to_fit();
		s_meta = {};
		s_seq = 0;
	}
}
