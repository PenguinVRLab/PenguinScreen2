// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GSLocalMemory.h"

#include <string>

class alignas(32) GSDrawingContext
{
public:
	GIFRegXYOFFSET XYOFFSET;
	GIFRegTEX0     TEX0;
	GIFRegTEX1     TEX1;
	GIFRegCLAMP    CLAMP;
	GIFRegMIPTBP1  MIPTBP1;
	GIFRegMIPTBP2  MIPTBP2;
	GIFRegSCISSOR  SCISSOR;
	GIFRegALPHA    ALPHA;
	GIFRegTEST     TEST;
	GIFRegFBA      FBA;
	GIFRegFRAME    FRAME;
	GIFRegZBUF     ZBUF;

	struct
	{
		GSVector4i in;
		GSVector4i cull;
		GSVector4i xyof;
	} scissor;

	struct
	{
		GSOffset fb;
		GSOffset zb;
		GSPixelOffset4* fzb4;
	} offset;

	void Reset();

	void UpdateScissor();

	GIFRegTEX0 GetSizeFixedTEX0(const GSVector4& st, bool linear, bool mipmap = false) const;

	void Dump(const std::string& filename);
};
