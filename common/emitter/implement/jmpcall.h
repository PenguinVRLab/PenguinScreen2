// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	extern void xJccKnownTarget(JccComparisonType comparison, const void* target, bool slideForward);

	struct xImpl_JmpCall
	{
		bool isJmp;

		void operator()(const xAddressReg& absreg) const;
		void operator()(const xIndirectNative& src) const;

		void operator()(const void* func) const
		{
			if (isJmp)
				xJccKnownTarget(Jcc_Unconditional, (const void*)(uptr)func, false);
			else
			{

				sptr dest = (sptr)func - ((sptr)xGetPtr() + 5);
				pxAssertMsg(dest == (s32)dest, "Indirect jump is too far, must use a register!");
				xWrite8(0xe8);
				xWrite32(dest);
			}
		}
	};

	extern const xImpl_Mov xMOV;
	extern const xImpl_JmpCall xCALL;

	struct xImpl_FastCall
	{

		void operator()(const void* f, const xRegister32& a1 = xEmptyReg, const xRegister32& a2 = xEmptyReg) const;

		void operator()(const void* f, u32 a1, const xRegister32& a2) const;
		void operator()(const void* f, const xIndirect32& a1) const;
		void operator()(const void* f, u32 a1, u32 a2) const;
		void operator()(const void* f, void* a1) const;

		void operator()(const void* f, const xRegisterLong& a1, const xRegisterLong& a2 = xEmptyReg) const;
		void operator()(const void* f, u32 a1, const xRegisterLong& a2) const;

		template <typename T>
		__fi void operator()(T* func, u32 a1, const xRegisterLong& a2 = xEmptyReg) const
		{
			(*this)((const void*)func, a1, a2);
		}

		template <typename T>
		__fi void operator()(T* func, const xIndirect32& a1) const
		{
			(*this)((const void*)func, a1);
		}

		template <typename T>
		__fi void operator()(T* func, u32 a1, u32 a2) const
		{
			(*this)((const void*)func, a1, a2);
		}

		void operator()(const xIndirectNative& f, const xRegisterLong& a1 = xEmptyReg, const xRegisterLong& a2 = xEmptyReg) const;
	};

}
