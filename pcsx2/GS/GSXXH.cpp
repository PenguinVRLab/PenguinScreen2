// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "MultiISA.h"

#include <cmath>
#include <cstdlib>

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
namespace CURRENT_ISA
{
#include <xxhash.h>
}

MULTI_ISA_UNSHARED_IMPL;

#include "GSXXH.h"

u64 __noinline CURRENT_ISA::GSXXH3_64_Long(const void* data, size_t len)
{
	return XXH3_hashLong_64b_internal(data, len, XXH3_kSecret, sizeof(XXH3_kSecret), XXH3_accumulate, XXH3_scrambleAcc);
}

u32 CURRENT_ISA::GSXXH3_64_Update(void* state, const void* data, size_t len)
{
	return XXH3_64bits_update(static_cast<XXH3_state_t*>(state), static_cast<const xxh_u8*>(data), len);
}

u64 CURRENT_ISA::GSXXH3_64_Digest(void* state)
{
	return XXH3_64bits_digest(static_cast<XXH3_state_t*>(state));
}
