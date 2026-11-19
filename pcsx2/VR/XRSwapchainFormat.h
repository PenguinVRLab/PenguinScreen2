// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace VR::XRSwapchainFormat
{
	inline constexpr int64_t kR8G8B8A8_UNORM = 37;
	inline constexpr int64_t kR8G8B8A8_SRGB = 43;
	inline constexpr int64_t kB8G8R8A8_UNORM = 44;
	inline constexpr int64_t kB8G8R8A8_SRGB = 50;
	inline constexpr int64_t kA8B8G8R8_UNORM_PACK32 = 51;
	inline constexpr int64_t kA8B8G8R8_SRGB_PACK32 = 57;

	enum class Rung
	{
		RawCopy,
		BlitSwizzle,
		ShaderEncode,
		Unsupported
	};

	struct Choice
	{
		int64_t format = 0;
		Rung rung = Rung::Unsupported;
		bool srgb = false;
		bool shader_encode_would_help = false;
	};

	bool HostIsLittleEndian();

	Choice Choose(const int64_t* offered, size_t count, bool prefer_unorm, bool little_endian = HostIsLittleEndian());

	const char* FormatName(int64_t format, char* buffer, size_t buffer_size);

	const char* RungName(Rung rung);

	constexpr uint32_t SwapRedBlue(uint32_t rgba)
	{
		return (rgba & 0xFF00FF00u) | ((rgba & 0x000000FFu) << 16) | ((rgba >> 16) & 0x000000FFu);
	}
}
