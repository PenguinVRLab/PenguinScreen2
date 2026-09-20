// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	enum G1Type
	{
		G1Type_ADD = 0,
		G1Type_OR,
		G1Type_ADC,
		G1Type_SBB,
		G1Type_AND,
		G1Type_SUB,
		G1Type_XOR,
		G1Type_CMP
	};

	struct xImpl_Group1
	{
		G1Type InstType;

		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;

		void operator()(const xIndirectVoid& to, const xRegisterInt& from) const;
		void operator()(const xRegisterInt& to, const xIndirectVoid& from) const;
		void operator()(const xRegisterInt& to, int imm) const;
		void operator()(const xIndirect64orLess& to, int imm) const;
	};

	struct xImpl_G1Logic : public xImpl_Group1
	{
		xImplSimd_3Arg PS;
		xImplSimd_3Arg PD;
	};

	struct xImpl_G1Arith : public xImpl_Group1
	{
		xImplSimd_3Arg PS;
		xImplSimd_3Arg PD;
		xImplSimd_3Arg SS;
		xImplSimd_3Arg SD;
	};

}
