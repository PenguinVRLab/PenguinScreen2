// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

alignas(16) const u32 sse4_minvals[2][4] = {
	{0xff7fffff, 0xffffffff, 0xffffffff, 0xffffffff},
	{0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff},
};
alignas(16) const u32 sse4_maxvals[2][4] = {
	{0x7f7fffff, 0x7fffffff, 0x7fffffff, 0x7fffffff},
	{0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff},
};

void mVUclamp1(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw, bool bClampE = 0)
{
	if (((!clampE && CHECK_VU_OVERFLOW(mVU.index)) || (clampE && bClampE)) && mVU.regAlloc->checkVFClamp(reg.Id))
	{
		switch (xyzw)
		{
			case 1: case 2: case 4: case 8:
				xMIN.SS(reg, ptr32[mVUglob.maxvals]);
				xMAX.SS(reg, ptr32[mVUglob.minvals]);
				break;
			default:
				xMIN.PS(reg, ptr32[mVUglob.maxvals]);
				xMAX.PS(reg, ptr32[mVUglob.minvals]);
				break;
		}
	}
}

void mVUclamp2(microVU& mVU, const xmm& reg, const xmm& regT1in, int xyzw, bool bClampE = 0)
{
	if (((!clampE && CHECK_VU_SIGN_OVERFLOW(mVU.index)) || (clampE && bClampE && CHECK_VU_SIGN_OVERFLOW(mVU.index))) && mVU.regAlloc->checkVFClamp(reg.Id))
	{
		int i = (xyzw == 1 || xyzw == 2 || xyzw == 4 || xyzw == 8) ? 0 : 1;
		xPMIN.SD(reg, ptr128[&sse4_maxvals[i][0]]);
		xPMIN.UD(reg, ptr128[&sse4_minvals[i][0]]);
		return;
	}
	else
		mVUclamp1(mVU, reg, regT1in, xyzw, bClampE);
}

void mVUclamp3(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw)
{
	if (clampE && mVU.regAlloc->checkVFClamp(reg.Id))
		mVUclamp2(mVU, reg, regT1, xyzw, 1);
}

void mVUclamp4(microVU& mVU, const xmm& reg, const xmm& regT1, int xyzw)
{
	if (clampE && !CHECK_VU_SIGN_OVERFLOW(mVU.index) && mVU.regAlloc->checkVFClamp(reg.Id))
		mVUclamp1(mVU, reg, regT1, xyzw, 1);
}
