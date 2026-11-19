// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/XRSwapchainFormat.h"

#include <cstdio>
#include <cstring>

namespace VR::XRSwapchainFormat
{
	namespace
	{
		bool Offered(const int64_t* offered, size_t count, int64_t format)
		{
			for (size_t i = 0; i < count; i++)
			{
				if (offered[i] == format)
					return true;
			}
			return false;
		}

		struct Candidate
		{
			int64_t format;
			Rung rung;
			bool srgb;
			bool needs_little_endian;
		};

		constexpr Candidate kSrgb[] = {
			{kR8G8B8A8_SRGB, Rung::RawCopy, true, false},
			{kA8B8G8R8_SRGB_PACK32, Rung::RawCopy, true, true},
		};
		constexpr Candidate kUnorm[] = {
			{kR8G8B8A8_UNORM, Rung::RawCopy, false, false},
			{kA8B8G8R8_UNORM_PACK32, Rung::RawCopy, false, true},
			{kB8G8R8A8_UNORM, Rung::BlitSwizzle, false, false},
		};

		bool TryClass(const Candidate* cands, size_t n, const int64_t* offered, size_t count, bool little_endian,
			Choice& out)
		{
			for (size_t i = 0; i < n; i++)
			{
				if (cands[i].needs_little_endian && !little_endian)
					continue;
				if (!Offered(offered, count, cands[i].format))
					continue;
				out.format = cands[i].format;
				out.rung = cands[i].rung;
				out.srgb = cands[i].srgb;
				return true;
			}
			return false;
		}
	}

	bool HostIsLittleEndian()
	{
		const uint32_t probe = 1;
		uint8_t first = 0;
		std::memcpy(&first, &probe, 1);
		return first == 1;
	}

	Choice Choose(const int64_t* offered, size_t count, bool prefer_unorm, bool little_endian)
	{
		Choice c;
		if (!offered || count == 0)
			return c;

		c.shader_encode_would_help = Offered(offered, count, kB8G8R8A8_SRGB);

		const size_t n_srgb = sizeof(kSrgb) / sizeof(kSrgb[0]);
		const size_t n_unorm = sizeof(kUnorm) / sizeof(kUnorm[0]);
		if (prefer_unorm)
		{
			if (TryClass(kUnorm, n_unorm, offered, count, little_endian, c))
				return c;
			if (TryClass(kSrgb, n_srgb, offered, count, little_endian, c))
				return c;
		}
		else
		{
			if (TryClass(kSrgb, n_srgb, offered, count, little_endian, c))
				return c;
			if (TryClass(kUnorm, n_unorm, offered, count, little_endian, c))
				return c;
		}

		c.format = 0;
		c.rung = Rung::Unsupported;
		c.srgb = false;
		return c;
	}

	const char* FormatName(int64_t format, char* buffer, size_t buffer_size)
	{
		switch (format)
		{
			case kR8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
			case kR8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
			case kB8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
			case kB8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
			case kA8B8G8R8_UNORM_PACK32: return "VK_FORMAT_A8B8G8R8_UNORM_PACK32";
			case kA8B8G8R8_SRGB_PACK32: return "VK_FORMAT_A8B8G8R8_SRGB_PACK32";
			default:
				break;
		}
		if (!buffer || buffer_size == 0)
			return "VkFormat ?";
		std::snprintf(buffer, buffer_size, "VkFormat %lld", static_cast<long long>(format));
		return buffer;
	}

	const char* RungName(Rung rung)
	{
		switch (rung)
		{
			case Rung::RawCopy: return "RawCopy";
			case Rung::BlitSwizzle: return "BlitSwizzle";
			case Rung::ShaderEncode: return "ShaderEncode";
			case Rung::Unsupported: return "Unsupported";
		}
		return "?";
	}
}
