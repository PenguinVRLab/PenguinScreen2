// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSVector.h"

#pragma pack(push, 1)

struct alignas(32) GSVertexHW9
{
	GSVector4 t;
	GSVector4 p;

	GSVertexHW9& operator=(GSVertexHW9& v)
	{
		t = v.t;
		p = v.p;
		return *this;
	}
};

#pragma pack(pop)
