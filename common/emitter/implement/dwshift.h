// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct xImpl_DwordShift
	{
		u16 OpcodeBase;

		void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from, const xRegisterCL& clreg) const;

		void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from, u8 shiftcnt) const;

		void operator()(const xIndirectVoid& dest, const xRegister16or32or64& from, const xRegisterCL& clreg) const;
		void operator()(const xIndirectVoid& dest, const xRegister16or32or64& from, u8 shiftcnt) const;
	};

}
