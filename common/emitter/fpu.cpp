// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/emitter/legacy_internal.h"

emitterT void FLD32(u32 from)
{
	xWrite8(0xD9);
	ModRM(0, 0x0, DISP32);
	xWrite32(MEMADDR(from, 4));
}

emitterT void FLD(int st) { xWrite16(0xc0d9 + (st << 8)); }
emitterT void FLD1() { xWrite16(0xe8d9); }
emitterT void FLDL2E() { xWrite16(0xead9); }

emitterT void FSTP32(u32 to)
{
	xWrite8(0xD9);
	ModRM(0, 0x3, DISP32);
	xWrite32(MEMADDR(to, 4));
}

emitterT void FSTP(int st) { xWrite16(0xd8dd + (st << 8)); }

emitterT void FRNDINT() { xWrite16(0xfcd9); }
emitterT void FXCH(int st) { xWrite16(0xc8d9 + (st << 8)); }
emitterT void F2XM1() { xWrite16(0xf0d9); }
emitterT void FSCALE() { xWrite16(0xfdd9); }
emitterT void FPATAN(void) { xWrite16(0xf3d9); }
emitterT void FSIN(void) { xWrite16(0xfed9); }

emitterT void FADD320toR(x86IntRegType src)
{
	xWrite8(0xDC);
	xWrite8(0xC0 + src);
}

emitterT void FSUB32Rto0(x86IntRegType src)
{
	xWrite8(0xD8);
	xWrite8(0xE0 + src);
}

emitterT void FMUL32(u32 from)
{
	xWrite8(0xD8);
	ModRM(0, 0x1, DISP32);
	xWrite32(MEMADDR(from, 4));
}
