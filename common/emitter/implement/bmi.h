// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct xImplBMI_RVM
	{
		u8 Prefix;
		u8 MbPrefix;
		u8 Opcode;

		void operator()(const xRegisterInt& to, const xRegisterInt& from1, const xRegisterInt& from2) const;
		void operator()(const xRegisterInt& to, const xRegisterInt& from1, const xIndirectVoid& from2) const;

#if 0

		void operator()( const xRegisterInt& to, const xRegisterInt& from) const;
		void operator()( const xRegisterInt& to, const xIndirectVoid& from) const;

		void operator()( const xRegisterInt& to, const xRegisterInt& from, u8 imm) const;
		void operator()( const xRegisterInt& to, const xIndirectVoid& from, u8 imm) const;
#endif
	};
}
