// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifndef u32
typedef unsigned int u32;
#endif

typedef struct _subframe
{
	u32 preamble : 4;
	u32 aux_data : 4;
	u32 snd_data : 20;
	u32 validity : 1;
	u32 subcode : 1;
	u32 chstatus : 1;
	u32 parity : 1;
} subframe;

typedef struct _chstatus
{
	u8 ctrlbits : 4;
	u8 reservd1 : 4;
	u8 category;
	u8 reservd2[22];
} chstatus;
