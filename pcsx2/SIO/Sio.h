// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Huge thanks to PSI for his work reversing the PS2, his documentation on SIO2 pretty much saved
// this entire implementation. https://psi-rockin.github.io/ps2tek/#sio2registers

#pragma once

#include "SIO/Memcard/MemoryCardFile.h"

struct _mcd
{
	u8 currentCommand;
	u8 term;

	bool goodSector;
	u8 msb;
	u8 lsb;
	u32 sectorAddr;
	u32 transferAddr;

	std::vector<u8> buf;

	u8 FLAG;

	u8 port;
	u8 slot;

	size_t autoEjectTicks;

	void GetSizeInfo(McdSizeInfo &info)
	{
		FileMcd_GetSizeInfo(port, slot, &info);
	}

	bool IsPSX()
	{
		return FileMcd_IsPSX(port, slot);
	}

	void EraseBlock()
	{
		FileMcd_EraseBlock(port, slot, transferAddr);
	}

	void Read(u8 *dest, int size)
	{
		FileMcd_Read(port, slot, dest, transferAddr, size);
	}

	void Write(u8 *src, int size)
	{
		FileMcd_Save(port, slot, src,transferAddr, size);
	}

	bool IsPresent()
	{
		return FileMcd_IsPresent(port, slot);
	}

	u8 DoXor()
	{
		u8 ret = msb ^ lsb;

		for (const u8 byte : buf)
		{
			ret ^= byte;
		}

		return ret;
	}

	u64 GetChecksum()
	{
		return FileMcd_GetCRC(port, slot);
	}

	void NextFrame() {
		FileMcd_NextFrame( port, slot );
	}

	bool ReIndex(const std::string& filter) {
		return FileMcd_ReIndex(port, slot, filter);
	}
};

extern _mcd mcds[2][4];
extern _mcd *mcd;

extern void sioNextFrame();

extern std::tuple<u32, u32> sioConvertPadToPortAndSlot(u32 index);

extern u32 sioConvertPortAndSlotToPad(u32 port, u32 slot);

extern bool sioPadIsMultitapSlot(u32 index);
extern bool sioPortAndSlotIsMultitap(u32 port, u32 slot);
extern void sioSetGameSerial(const std::string& serial);

namespace AutoEject
{
	extern void CountDownTicks();
	extern void Set(size_t port, size_t slot);
	extern void Clear(size_t port, size_t slot);
	extern void SetAll();
	extern void ClearAll();
}

constexpr u32 NUM_FRAMES_BEFORE_SAVESTATE_DEPENDENCY_WARNING = 60 * 60 * 60 * 2;

extern uint32_t sioLastFrameMcdBusy;

namespace MemcardBusy
{
	extern void Decrement();
	extern void SetBusy();
	extern bool IsBusy();
	extern void ClearBusy();
	extern void CheckSaveStateDependency();
}
