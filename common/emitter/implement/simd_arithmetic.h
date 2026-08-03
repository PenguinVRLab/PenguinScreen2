// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct _SimdShiftHelper
	{
		SIMDInstructionInfo info;
		SIMDInstructionInfo infoImm;

		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src)  const { (*this)(dst, dst, src); }
		void operator()(const xRegisterSSE& dst, const xIndirectVoid& src) const { (*this)(dst, dst, src); }
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE& src2)  const;
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2) const;

		void operator()(const xRegisterSSE& dst, u8 imm8) const { (*this)(dst, dst, imm8); }
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src, u8 imm8) const;
	};

	struct xImplSimd_ShiftWithoutQ
	{
		const _SimdShiftHelper W;
		const _SimdShiftHelper D;
	};

	struct xImplSimd_Shift
	{
		const _SimdShiftHelper W;
		const _SimdShiftHelper D;
		const _SimdShiftHelper Q;

		void DQ(const xRegisterSSE& dst, u8 imm8) const { DQ(dst, dst, imm8); }
		void DQ(const xRegisterSSE& dst, const xRegisterSSE& src, u8 imm8) const;
	};

	struct xImplSimd_AddSub
	{
		const xImplSimd_3Arg B;
		const xImplSimd_3Arg W;
		const xImplSimd_3Arg D;
		const xImplSimd_3Arg Q;

		const xImplSimd_3Arg SB;

		const xImplSimd_3Arg SW;

		const xImplSimd_3Arg USB;

		const xImplSimd_3Arg USW;
	};

	struct xImplSimd_PMul
	{
		const xImplSimd_3Arg LW;
		const xImplSimd_3Arg HW;
		const xImplSimd_3Arg HUW;
		const xImplSimd_3Arg UDQ;

		const xImplSimd_3Arg HRSW;

		const xImplSimd_3Arg LD;

		const xImplSimd_3Arg DQ;
	};

	struct xImplSimd_rSqrt
	{
		const xImplSimd_2Arg PS;
		const xImplSimd_3Arg SS;
	};

	struct xImplSimd_Sqrt
	{
		const xImplSimd_2Arg PS;
		const xImplSimd_3Arg SS;
		const xImplSimd_2Arg PD;
		const xImplSimd_3Arg SD;
	};

	struct xImplSimd_AndNot
	{
		const xImplSimd_3Arg PS;
		const xImplSimd_3Arg PD;
	};

	struct xImplSimd_PAbsolute
	{
		const xImplSimd_2Arg B;

		const xImplSimd_2Arg W;

		const xImplSimd_2Arg D;
	};

	struct xImplSimd_PSign
	{
		const xImplSimd_3Arg B;

		const xImplSimd_3Arg W;

		const xImplSimd_3Arg D;
	};

	struct xImplSimd_PMultAdd
	{
		const xImplSimd_3Arg WD;

		const xImplSimd_3Arg UBSW;
	};

	struct xImplSimd_HorizAdd
	{
		const xImplSimd_3Arg PS;

		const xImplSimd_3Arg PD;
	};

	struct xImplSimd_DotProduct
	{
		xImplSimd_3ArgImm PS;

		xImplSimd_3ArgImm PD;
	};

	struct xImplSimd_Round
	{
		const xImplSimd_2ArgImm PS;

		const xImplSimd_2ArgImm PD;

		const xImplSimd_3ArgImm SS;

		const xImplSimd_3ArgImm SD;
	};

}
