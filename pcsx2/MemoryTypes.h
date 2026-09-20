// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "common/Pcsx2Defs.h"

namespace Ps2MemSize
{
	static constexpr u32 MainRam = _32mb;
	static constexpr u32 ExtraRam = _1mb * 96;
	static constexpr u32 TotalRam = _1mb * 128;
	static constexpr u32 Rom = _1mb * 4;
	static constexpr u32 Rom1 = _1mb * 4;
	static constexpr u32 Rom2 = _1mb * 4;
	static constexpr u32 Hardware = _64kb;
	static constexpr u32 Scratch = _16kb;

	static constexpr u32 IopRam = _1mb * 2;
	static constexpr u32 ExtraIopRam = _1mb * 6;
	static constexpr u32 TotalIopRam = _8mb;
	static constexpr u32 IopHardware = _64kb;

	static constexpr u32 GSregs = 0x00002000;

	extern u32 ExposedRam;
	extern u32 ExposedIopRam;
}

typedef u8 mem8_t;
typedef u16 mem16_t;
typedef u32 mem32_t;
typedef u64 mem64_t;
typedef u128 mem128_t;

struct EEVM_MemoryAllocMess
{
	u8 Main[Ps2MemSize::TotalRam];
	u8 Scratch[Ps2MemSize::Scratch];
	u8 ROM[Ps2MemSize::Rom];
	u8 ROM1[Ps2MemSize::Rom1];
	u8 ROM2[Ps2MemSize::Rom2];

	u8 ZeroRead[_1mb];
	u8 ZeroWrite[_1mb];
};

struct IopVM_MemoryAllocMess
{
	u8 Main[Ps2MemSize::TotalRam];
	u8 P[_64kb];
	u8 Sif[0x100];
};


alignas(__pagealignsize) extern u8 eeHw[Ps2MemSize::Hardware];
alignas(__pagealignsize) extern u8 iopHw[Ps2MemSize::IopHardware];


extern EEVM_MemoryAllocMess* eeMem;
extern IopVM_MemoryAllocMess* iopMem;
