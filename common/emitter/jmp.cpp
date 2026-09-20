// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

/*
 * ix86 core v0.9.1
 *
 * Original Authors (v0.6.2 and prior):
 *		linuzappz <linuzappz@pcsx.net>
 *		alexey silinov
 *		goldfinger
 *		zerofrog(@gmail.com)
 *
 * Authors of v0.9.1:
 *		Jake.Stine(@gmail.com)
 *		cottonvibes(@gmail.com)
 *		sudonim(1@gmail.com)
 */

#include "common/emitter/internal.h"

namespace x86Emitter
{

	void xImpl_JmpCall::operator()(const xAddressReg& absreg) const
	{
		xOpWrite(0, 0xff, isJmp ? 4 : 2, absreg.GetNonWide());
	}
	void xImpl_JmpCall::operator()(const xIndirectNative& src) const
	{
		EmitRex(0, xIndirect32(src.Base, src.Index, 1, 0));
		xWrite8(0xff);
		EmitSibMagic(isJmp ? 4 : 2, src);
	}

	const xImpl_JmpCall xJMP = {true};
	const xImpl_JmpCall xCALL = {false};


	template <typename Reg1, typename Reg2>
	void prepareRegsForFastcall(const Reg1& a1, const Reg2& a2)
	{
		if (a1.IsEmpty())
			return;

		if (a2.Id != arg1reg.Id)
		{
			xMOV(Reg1(arg1reg), a1);
			if (!a2.IsEmpty())
			{
				xMOV(Reg2(arg2reg), a2);
			}
		}
		else if (a1.Id != arg2reg.Id)
		{
			xMOV(Reg2(arg2reg), a2);
			xMOV(Reg1(arg1reg), a1);
		}
		else
		{
			xPUSH(a1);
			xMOV(Reg2(arg2reg), a2);
			xPOP(Reg1(arg1reg));
		}
	}

	void xImpl_FastCall::operator()(const void* f, const xRegister32& a1, const xRegister32& a2) const
	{
		prepareRegsForFastcall(a1, a2);
		uptr disp = ((uptr)xGetPtr() + 5) - (uptr)f;
		if ((sptr)disp == (s32)disp)
		{
			xCALL(f);
		}
		else
		{
			xLEA(rax, ptr64[f]);
			xCALL(rax);
		}
	}

	void xImpl_FastCall::operator()(const void* f, const xRegisterLong& a1, const xRegisterLong& a2) const
	{
		prepareRegsForFastcall(a1, a2);
		uptr disp = ((uptr)xGetPtr() + 5) - (uptr)f;
		if ((sptr)disp == (s32)disp)
		{
			xCALL(f);
		}
		else
		{
			xLEA(rax, ptr64[f]);
			xCALL(rax);
		}
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, const xRegisterLong& a2) const
	{
		if (!a2.IsEmpty())
		{
			xMOV(arg2reg, a2);
		}
		xMOV(arg1reg, a1);
		(*this)(f, arg1reg, arg2reg);
	}

	void xImpl_FastCall::operator()(const void* f, void* a1) const
	{
		xLEA(arg1reg, ptr[a1]);
		(*this)(f, arg1reg, arg2reg);
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, const xRegister32& a2) const
	{
		if (!a2.IsEmpty())
		{
			xMOV(arg2regd, a2);
		}
		xMOV(arg1regd, a1);
		(*this)(f, arg1regd, arg2regd);
	}

	void xImpl_FastCall::operator()(const void* f, const xIndirect32& a1) const
	{
		xMOV(arg1regd, a1);
		(*this)(f, arg1regd);
	}

	void xImpl_FastCall::operator()(const void* f, u32 a1, u32 a2) const
	{
		xMOV(arg1regd, a1);
		xMOV(arg2regd, a2);
		(*this)(f, arg1regd, arg2regd);
	}

	void xImpl_FastCall::operator()(const xIndirectNative& f, const xRegisterLong& a1, const xRegisterLong& a2) const
	{
		prepareRegsForFastcall(a1, a2);
		xCALL(f);
	}

	const xImpl_FastCall xFastCall = {};

	__emitinline s32* xJcc32(JccComparisonType comparison, s32 displacement)
	{
		if (comparison == Jcc_Unconditional)
			xWrite8(0xe9);
		else
		{
			xWrite8(0x0f);
			xWrite8(0x80 | comparison);
		}
		xWrite<s32>(displacement);

		return ((s32*)xGetPtr()) - 1;
	}

	__emitinline s8* xJcc8(JccComparisonType comparison, s8 displacement)
	{
		xWrite8((comparison == Jcc_Unconditional) ? 0xeb : (0x70 | comparison));
		xWrite<s8>(displacement);
		return (s8*)xGetPtr() - 1;
	}

	__emitinline void xJccKnownTarget(JccComparisonType comparison, const void* target, bool slideForward)
	{
		sptr displacement8 = (sptr)target - (sptr)(xGetPtr() + 2);

		const int slideVal = slideForward ? ((comparison == Jcc_Unconditional) ? 3 : 4) : 0;
		displacement8 -= slideVal;

		if (slideForward)
		{
			pxAssertMsg(displacement8 >= 0, "Used slideForward on a backward jump; nothing to slide!");
		}

		if (is_s8(displacement8))
			xJcc8(comparison, displacement8);
		else
		{
			s32* bah = xJcc32(comparison);
			sptr distance = (sptr)target - (sptr)xGetPtr();

			pxAssertMsg(distance >= -0x80000000LL && distance < 0x80000000LL, "Jump target is too far away, needs an indirect register");

			*bah = (s32)distance;
		}
	}

	__emitinline void xJcc(JccComparisonType comparison, const void* target)
	{
		xJccKnownTarget(comparison, target, false);
	}

	xForwardJumpBase::xForwardJumpBase(uint opsize, JccComparisonType cctype)
	{
		pxAssert(opsize == 1 || opsize == 4);
		pxAssertMsg(cctype != Jcc_Unknown, "Invalid ForwardJump conditional type.");

		BasePtr = (s8*)xGetPtr() +
				  ((opsize == 1) ? 2 :
                                   ((cctype == Jcc_Unconditional) ? 5 : 6));

		if (opsize == 1)
			xWrite8((cctype == Jcc_Unconditional) ? 0xeb : (0x70 | cctype));
		else
		{
			if (cctype == Jcc_Unconditional)
				xWrite8(0xe9);
			else
			{
				xWrite8(0x0f);
				xWrite8(0x80 | cctype);
			}
		}

		xAdvancePtr(opsize);
	}

	void xForwardJumpBase::_setTarget(uint opsize) const
	{
		pxAssertMsg(BasePtr != NULL, "");

		sptr displacement = (sptr)xGetPtr() - (sptr)BasePtr;
		if (opsize == 1)
		{
			pxAssertMsg(is_s8(displacement), "Emitter Error: Invalid short jump displacement.");
			BasePtr[-1] = (s8)displacement;
		}
		else
		{
			((s32*)BasePtr)[-1] = displacement;
		}
	}

	__fi JccComparisonType xInvertCond(JccComparisonType src)
	{
		pxAssert(src != Jcc_Unknown);
		if (Jcc_Unconditional == src)
			return Jcc_Unconditional;

		return (JccComparisonType)((int)src ^ 1);
	}
}
