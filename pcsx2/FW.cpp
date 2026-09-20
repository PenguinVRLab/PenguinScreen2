// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "IopDma.h"
#include "R3000A.h"
#include "FW.h"

#include "common/Console.h"

#include <cstdlib>
#include <cstring>

static u8 phyregs[16];
s8* fwregs;

s32 FWopen()
{
	memset(phyregs, 0, sizeof(phyregs));
	fwregs = (s8*)calloc(0x10000, 1);
	if (fwregs == NULL)
	{
		DevCon.WriteLn("FW: Error allocating Memory");
		return -1;
	}
	return 0;
}

void FWclose()
{
	free(fwregs);
	fwregs = NULL;
}

void PHYWrite()
{
	u8 reg = (PHYACC >> 8) & 0xf;
	u8 data = PHYACC & 0xff;

	phyregs[reg] = data;

	PHYACC &= ~0x4000ffff;
}

void PHYRead()
{
	u8 reg = (PHYACC >> 24) & 0xf;

	PHYACC &= ~0x80000000;

	PHYACC |= phyregs[reg] | (reg << 8);

	if (fwRu32(0x8424) & 0x40000000)
	{
		fwRu32(0x8420) |= 0x40000000;
		fwIrq();
	}
}

u32 FWread32(u32 addr)
{
	u32 ret = 0;

	switch (addr)
	{
		case 0x1f808400:
			ret = 0xffc00001;
			break;
		case 0x1f808410:
			ret = fwRu32(addr);
			break;
		case 0x1f808420:
			ret = fwRu32(addr);
			break;

		case 0x1f80847c:
			ret = 0x10000001;
			break;

		default:
			ret = fwRu32(addr);
			break;
	}

	DevCon.WriteLn("FW: read mem 0x%x: 0x%x", addr, ret);

	return ret;
}

void FWwrite32(u32 addr, u32 value)
{
	switch (addr)
	{

		case 0x1f808414:
			fwRu32(addr) = value;
			if (value & 0x40000000)
			{
				PHYWrite();
			}
			else if (value & 0x80000000)
			{
				PHYRead();
			}
			break;

		case 0x1f808408:
			fwRu32(addr) = value;
			fwRu32(addr) &= ~0x800000;
			break;
		case 0x1f808410:
			fwRu32(addr) = 0x8 ;
			break;
		case 0x1f808420:
		case 0x1f808428:
		case 0x1f808430:
			fwRu32(addr) &= ~value;
			break;
		case 0x1f808424:
		case 0x1f80842C:
		case 0x1f808434:
			fwRu32(addr) = value;
			break;
		case 0x1f8084B8:
			fwRu32(addr) = value;
			break;
		case 0x1f808538:
			fwRu32(addr) = value;
			break;
		default:
			fwRu32(addr) = value;
			break;
	}
	DevCon.WriteLn("FW: write mem 0x%x: 0x%x", addr, value);
}
