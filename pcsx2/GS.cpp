// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Counters.h"
#include "Common.h"
#include "Config.h"
#include "Gif_Unit.h"
#include "MTGS.h"
#include "VMManager.h"

#include <list>

alignas(16) u8 g_RealGSMem[Ps2MemSize::GSregs];
static bool s_GSRegistersWritten = false;

void gsSetVideoMode(GS_VideoMode mode)
{
	gsVideoMode = mode;
	UpdateVSyncRate(false);
}

void gsReset()
{
	MTGS::ResetGS(true);
	gsVideoMode = GS_VideoMode::Uninitialized;
	std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
	UpdateVSyncRate(true);
}

static __fi void gsCSRwrite( const tGS_CSR& csr )
{
	if (csr.RESET) {
		GUNIT_WARN("GUNIT_WARN: csr.RESET");
		gifUnit.gsSIGNAL.queued = false;
		gifUnit.gsFINISH.gsFINISHFired = true;
		gifUnit.gsFINISH.gsFINISHPending = false;
		std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
		GSIMR.reset();
		CSRreg.Reset();
		MTGS::ResetGS(false);
	}

	if(csr.FLUSH)
	{
	}

	if(csr.SIGNAL)
	{
		const bool resume = CSRreg.SIGNAL;
		GUNIT_LOG("csr.SIGNAL");
		if (gifUnit.gsSIGNAL.queued) {
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~gifUnit.gsSIGNAL.data[1])
				        | (gifUnit.gsSIGNAL.data[0]&gifUnit.gsSIGNAL.data[1]);

			if (!GSIMR.SIGMSK) gsIrq();
			CSRreg.SIGNAL  = true;
		}
		else CSRreg.SIGNAL = false;
		gifUnit.gsSIGNAL.queued = false;

		if (resume)
			gifUnit.Execute(false, true);
	}

	if (csr.FINISH)	{
		CSRreg.FINISH = false;
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if(csr.HSINT)	CSRreg.HSINT	= false;
	if(csr.VSINT)	CSRreg.VSINT	= false;
	if(csr.EDWINT)	CSRreg.EDWINT	= false;
}

static __fi void IMRwrite(u32 value)
{
	GUNIT_LOG("IMRwrite()");

	if (CSRreg.GetInterruptMask() & (~value & GSIMR._u32) >> 8)
		gsIrq();

	GSIMR._u32 = (value & 0x1f00)|0x6000;
}

__fi void gsWrite8(u32 mem, u8 value)
{
	switch (mem)
	{

		case GS_CSR:
			gsCSRwrite( tGS_CSR((u32)value) );			break;
		case GS_CSR + 1:
			gsCSRwrite( tGS_CSR(((u32)value) <<  8) );	break;
		case GS_CSR + 2:
			gsCSRwrite( tGS_CSR(((u32)value) << 16) );	break;
		case GS_CSR + 3:
			gsCSRwrite( tGS_CSR(((u32)value) << 24) );	break;

		default:
			*PS2GS_BASE(mem) = value;
		break;
	}
	GIF_LOG("GS write 8 at %8.8lx with data %8.8lx", mem, value);
}

__fi void gsWrite16(u32 mem, u16 value)
{
	GIF_LOG("GS write 16 at %8.8lx with data %8.8lx", mem, value);

	switch (mem)
	{

		case GS_CSR:
			gsCSRwrite( tGS_CSR((u32)value) );
		return;

		case GS_CSR+2:
			gsCSRwrite( tGS_CSR(((u32)value) << 16) );
		return;

		case GS_IMR:
			IMRwrite(value);
		return;
	}

	*(u16*)PS2GS_BASE(mem) = value;
}

__fi void gsWrite32(u32 mem, u32 value)
{
	pxAssume( (mem & 3) == 0 );
	GIF_LOG("GS write 32 at %8.8lx with data %8.8lx", mem, value);

	switch (mem)
	{
		case GS_CSR:
			gsCSRwrite(tGS_CSR(value));
		return;

		case GS_IMR:
			IMRwrite(value);
		return;
	}

	*(u32*)PS2GS_BASE(mem) = value;
}

void gsWrite64_generic( u32 mem, u64 value )
{
	GIF_LOG("GS Write64 at %8.8lx with data %8.8x_%8.8x", mem, (u32)(value >> 32), (u32)value);

	std::memcpy(PS2GS_BASE(mem), &value, sizeof(value));
}

void gsWrite64_page_00( u32 mem, u64 value )
{
	s_GSRegistersWritten |= (mem == GS_DISPFB1 || mem == GS_DISPFB2 || mem == GS_PMODE);
	bool reqUpdate = false;
	if (mem == GS_SMODE1 || mem == GS_SMODE2)
	{
		if (value != *(u64*)PS2GS_BASE(mem))
			reqUpdate = true;
	}

	gsWrite64_generic( mem, value );

	if (reqUpdate)
		UpdateVSyncRate(false);
}

void gsWrite64_page_01( u32 mem, u64 value )
{
	GIF_LOG("GS Write64 at %8.8lx with data %8.8x_%8.8x", mem, (u32)(value >> 32), (u32)value);

	switch( mem )
	{
		case GS_BUSDIR:

			gifUnit.stat.DIR = static_cast<u32>(value) & 1;
			if (gifUnit.stat.DIR) {
				gifUnit.stat.OPH = true;
				gifUnit.FlushToMTGS();
				GUNIT_LOG("Busdir - GS->EE Download");
			}
			else {
				GUNIT_LOG("Busdir - EE->GS Upload");
			}

			gsWrite64_generic( mem, value );
		return;

		case GS_CSR:
			gsCSRwrite(tGS_CSR(value));
		return;

		case GS_IMR:
			IMRwrite(static_cast<u32>(value));
		return;
	}

	gsWrite64_generic( mem, value );
}

void TAKES_R128 gsWrite128_page_00( u32 mem, r128 value )
{
	gsWrite128_generic( mem, value );
}

void TAKES_R128 gsWrite128_page_01( u32 mem, r128 value )
{
	switch( mem )
	{
		case GS_CSR:
			gsCSRwrite(r128_to_u32(value));
		return;

		case GS_IMR:
			IMRwrite(r128_to_u32(value));
		return;
	}

	gsWrite128_generic( mem, value );
}

void TAKES_R128 gsWrite128_generic( u32 mem, r128 value )
{
	alignas(16) const u128 uvalue = r128_to_u128(value);
	GIF_LOG("GS Write128 at %8.8lx with data %8.8x_%8.8x_%8.8x_%8.8x", mem,
		uvalue._u32[3], uvalue._u32[2], uvalue._u32[1], uvalue._u32[0]);

	r128_store(PS2GS_BASE(mem), value);
}

__fi u8 gsRead8(u32 mem)
{
	GIF_LOG("GS read 8 from %8.8lx  value: %8.8lx", mem, *(u8*)PS2GS_BASE(mem));

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u8*)PS2GS_BASE(mem);
		default:
			return *(u8*)PS2GS_BASE(GS_CSR + (mem & 0xF));
	}
}

__fi u16 gsRead16(u32 mem)
{
	GIF_LOG("GS read 16 from %8.8lx  value: %8.8lx", mem, *(u16*)PS2GS_BASE(mem));
	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u16*)PS2GS_BASE(mem);
		default:
			return *(u16*)PS2GS_BASE(GS_CSR + (mem & 0x7));
	}
}

__fi u32 gsRead32(u32 mem)
{
	GIF_LOG("GS read 32 from %8.8lx  value: %8.8lx", mem, *(u32*)PS2GS_BASE(mem));

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u32*)PS2GS_BASE(mem);
		default:
			return *(u32*)PS2GS_BASE(GS_CSR + (mem & 0xC));
	}
}

__fi u64 gsRead64(u32 mem)
{
	GIF_LOG("GS read 64 from %8.8lx  value: %8.8lx_%8.8lx", mem, *(u32*)PS2GS_BASE(mem+4), *(u32*)PS2GS_BASE(mem) );

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u64*)PS2GS_BASE(mem);
		default:
			return *(u64*)PS2GS_BASE(GS_CSR + (mem & 0x8));
	}
}

__fi u128 gsNonMirroredRead(u32 mem)
{
	return *(u128*)PS2GS_BASE(mem);
}

void gsIrq() {
	hwIntcIrq(INTC_GS);
}

void gsPostVsyncStart()
{

	const bool registers_written = s_GSRegistersWritten;
	s_GSRegistersWritten = false;
	MTGS::PostVsyncStart(registers_written);
}

bool SaveStateBase::gsFreeze()
{
	FreezeMem(PS2MEM_GS, 0x2000);
	Freeze(gsVideoMode);
	return IsOkay();
}

