// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct xImpl_Test
	{
		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;
		void operator()(const xIndirect64orLess& dest, int imm) const;
		void operator()(const xRegisterInt& to, int imm) const;
	};

	enum G8Type
	{
		G8Type_BT = 4,
		G8Type_BTS,
		G8Type_BTR,
		G8Type_BTC,
	};

	struct xImpl_BitScan
	{
		u16 Opcode;

		void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const;
		void operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const;
	};

	struct xImpl_Group8
	{
		G8Type InstType;

		void operator()(const xRegister16or32or64& bitbase, const xRegister16or32or64& bitoffset) const;
		void operator()(const xRegister16or32or64& bitbase, u8 bitoffset) const;

		void operator()(const xIndirectVoid& bitbase, const xRegister16or32or64& bitoffset) const;

		void operator()(const xIndirect64& bitbase, u8 bitoffset) const;
		void operator()(const xIndirect32& bitbase, u8 bitoffset) const;
		void operator()(const xIndirect16& bitbase, u8 bitoffset) const;
	};

}
