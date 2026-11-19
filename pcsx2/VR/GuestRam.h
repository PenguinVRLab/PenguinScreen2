// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include "VR/VRProfileDB.h"

#include "common/MemoryInterface.h"
#include "common/Pcsx2Types.h"

#include <optional>
#include <vector>

namespace VR::GuestRam
{
	void EncodeAndWrite(MemoryInterface& mem, u32 address, float value, ProfileDB::CameraEncoding enc);

	std::optional<float> DecodeGuestValue(MemoryInterface& mem, u32 address, ProfileDB::CameraEncoding enc);

	std::optional<u32> WalkChain(MemoryInterface& mem, u32 root, const std::vector<s32>& hops);

	bool GuardListPass(MemoryInterface& mem, const std::vector<ProfileDB::CameraGuard>& guards);
}
