// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Common.h"
#include "deci2_dcmp.h"
#include "deci2_iloadp.h"
#include "deci2_dbgp.h"
#include "deci2_netmp.h"
#include "deci2_ttyp.h"

#define PROTO_DCMP		0x0001
#define PROTO_ITTYP		0x0110
#define PROTO_IDBGP		0x0130
#define PROTO_ILOADP	0x0150
#define PROTO_ETTYP		0x0220
#define PROTO_EDBGP		0x0230
#define PROTO_NETMP		0x0400


#pragma pack(1)
struct DECI2_HEADER {
	u16		length,
			_pad,
			protocol;
	char	source,
			destination;
};

struct DECI2_DBGP_BRK{
	u32	address,
		count;
};
#pragma pack()

#define STOP	0
#define RUN		1

extern DECI2_DBGP_BRK	ebrk[32], ibrk[32];
extern s32 ebrk_count, ibrk_count;
extern s32 runCode, runCount;

extern Threading::KernelSemaphore* runEvent;

extern s32		connected;

int	writeData(const u8 *result);
void	exchangeSD(DECI2_HEADER *h);
