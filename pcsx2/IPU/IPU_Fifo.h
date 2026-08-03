// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include <string>

struct IPU_Fifo_Input
{
	alignas(16) u32 data[32];
	int readpos, writepos;

	int write(const u32* pMem, int size);
	int read(void *value);
	void clear();
	std::string desc() const;
};

struct IPU_Fifo_Output
{
	alignas(16) u32 data[32];
	int readpos, writepos;

	int write(const u32 * value, uint size);
	void read(void *value, uint size);
	void clear();
	std::string desc() const;
};

struct IPU_Fifo
{
	alignas(16) IPU_Fifo_Input in;
	alignas(16) IPU_Fifo_Output out;

	void init();
	void clear();
};

alignas(16) extern IPU_Fifo ipu_fifo;
