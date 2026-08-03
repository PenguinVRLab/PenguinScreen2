// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/// Version number for GS and other shaders. Increment whenever any of the contents of the
/// shaders change, to invalidate the cache.
static constexpr u32 SHADER_CACHE_VERSION = 110; // upstream PR 14688 (=108) + PCSX2-VR tfx VS clamp + Stage 1/D4 rt_in_array/depth_in_array per-view feedback sampling
