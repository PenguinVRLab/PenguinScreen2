// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct xImplSimd_MinMax
	{
		const xImplSimd_3Arg PS;
		const xImplSimd_3Arg PD;
		const xImplSimd_3Arg SS;
		const xImplSimd_3Arg SD;
	};

	struct xImplSimd_Compare
	{
		SSE2_ComparisonType CType;

		void PS(const xRegisterSSE& dst, const xRegisterSSE&  src) const { PS(dst, dst, src); }
		void PS(const xRegisterSSE& dst, const xIndirectVoid& src) const { PS(dst, dst, src); }
		void PS(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE&  src2) const;
		void PS(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2) const;

		void PD(const xRegisterSSE& dst, const xRegisterSSE&  src) const { PD(dst, dst, src); }
		void PD(const xRegisterSSE& dst, const xIndirectVoid& src) const { PD(dst, dst, src); }
		void PD(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE&  src2) const;
		void PD(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2) const;

		void SS(const xRegisterSSE& dst, const xRegisterSSE&  src) const { SS(dst, dst, src); }
		void SS(const xRegisterSSE& dst, const xIndirectVoid& src) const { SS(dst, dst, src); }
		void SS(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE&  src2) const;
		void SS(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2) const;

		void SD(const xRegisterSSE& dst, const xRegisterSSE&  src) const { SD(dst, dst, src); }
		void SD(const xRegisterSSE& dst, const xIndirectVoid& src) const { SD(dst, dst, src); }
		void SD(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE&  src2) const;
		void SD(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2) const;
	};

	struct xImplSimd_COMI
	{
		const xImplSimd_2Arg SS;
		const xImplSimd_2Arg SD;
	};


	struct xImplSimd_PCompare
	{
		const xImplSimd_3Arg EQB;

		const xImplSimd_3Arg EQW;

		const xImplSimd_3Arg EQD;

		const xImplSimd_3Arg GTB;

		const xImplSimd_3Arg GTW;

		const xImplSimd_3Arg GTD;
	};

	struct xImplSimd_PMinMax
	{
		const xImplSimd_3Arg UB;

		const xImplSimd_3Arg SW;

		const xImplSimd_3Arg SB;

		const xImplSimd_3Arg SD;

		const xImplSimd_3Arg UW;

		const xImplSimd_3Arg UD;
	};

}
