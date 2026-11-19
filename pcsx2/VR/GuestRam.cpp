// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/GuestRam.h"

#include "MemoryTypes.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace VR::GuestRam
{
	namespace
	{
		s32 SaturateRound(float v, double lo, double hi)
		{
			const double d = std::clamp(static_cast<double>(v), lo, hi);
			return static_cast<s32>(std::lround(d));
		}
	}

	void EncodeAndWrite(MemoryInterface& mem, u32 address, float value, ProfileDB::CameraEncoding enc)
	{
		if (!std::isfinite(value))
			return;

		switch (enc)
		{
			case ProfileDB::CameraEncoding::F32:
			{
				mem.Write32(address, std::bit_cast<u32>(value));
				break;
			}
			case ProfileDB::CameraEncoding::S16_12:
			{
				const s32 raw = SaturateRound(value * 4096.0f,
					std::numeric_limits<s16>::min(), std::numeric_limits<s16>::max());
				mem.Write16(address, static_cast<u16>(static_cast<s16>(raw)));
				break;
			}
			case ProfileDB::CameraEncoding::S32Angle:
			{
				const s32 raw = SaturateRound(value,
					std::numeric_limits<s32>::min(), std::numeric_limits<s32>::max());
				mem.Write32(address, static_cast<u32>(raw));
				break;
			}
			default:
				break;
		}
	}

	std::optional<float> DecodeGuestValue(MemoryInterface& mem, u32 address, ProfileDB::CameraEncoding enc)
	{
		switch (enc)
		{
			case ProfileDB::CameraEncoding::F32:
			{
				const float v = std::bit_cast<float>(mem.Read32(address));
				if (!std::isfinite(v))
					return std::nullopt;
				return v;
			}
			case ProfileDB::CameraEncoding::S16_12:
				return static_cast<float>(static_cast<s16>(mem.Read16(address))) * (1.0f / 4096.0f);
			case ProfileDB::CameraEncoding::S32Angle:
				return static_cast<float>(static_cast<s32>(mem.Read32(address)));
			default:
				return std::nullopt;
		}
	}

	std::optional<u32> WalkChain(MemoryInterface& mem, u32 root, const std::vector<s32>& hops)
	{
		u32 deref = mem.Read32(root);
		if (deref == 0 || deref >= Ps2MemSize::MainRam || (deref & 3u) != 0)
			return std::nullopt;
		for (const s32 hop : hops)
		{
			const s64 slot = static_cast<s64>(deref) + hop;
			if (slot < 0 || slot + 4 > static_cast<s64>(Ps2MemSize::MainRam))
				return std::nullopt;
			deref = mem.Read32(static_cast<u32>(slot));
			if (deref == 0 || deref >= Ps2MemSize::MainRam || (deref & 3u) != 0)
				return std::nullopt;
		}
		return deref;
	}

	bool GuardListPass(MemoryInterface& mem, const std::vector<ProfileDB::CameraGuard>& guards)
	{
		for (const ProfileDB::CameraGuard& g : guards)
		{
			u32 address = g.ee_address;
			if (g.is_chain)
			{
				const std::optional<u32> obj = WalkChain(mem, g.pointer_addr, g.deref_offsets);
				if (!obj.has_value())
					return false;
				const s64 a = static_cast<s64>(obj.value()) + g.offset;
				if (a < 0 || a + g.width > static_cast<s64>(Ps2MemSize::MainRam))
					return false;
				address = static_cast<u32>(a);
			}
			u32 v = 0;
			switch (g.width)
			{
				case 1: v = mem.Read8(address); break;
				case 2: v = mem.Read16(address); break;
				default: v = mem.Read32(address); break;
			}
			if ((v == g.equals) == g.not_equals)
				return false;
		}
		return true;
	}
}
