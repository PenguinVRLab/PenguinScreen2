// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct xImplSimd_MovHL
	{
		SIMDInstructionInfo info;

		void PS(const xRegisterSSE& dst, const xIndirectVoid& src) const { PS(dst, dst, src); }
		void PS(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2) const;
		void PS(const xIndirectVoid& dst, const xRegisterSSE& src) const;

		void PD(const xRegisterSSE& dst, const xIndirectVoid& src) const { PD(dst, dst, src); }
		void PD(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2) const;
		void PD(const xIndirectVoid& dst, const xRegisterSSE& src) const;
	};

	struct xImplSimd_MovHL_RtoR
	{
		SIMDInstructionInfo info;

		void PS(const xRegisterSSE& dst, const xRegisterSSE& src) const { PS(dst, dst, src); }
		void PD(const xRegisterSSE& dst, const xRegisterSSE& src) const { PD(dst, dst, src); }
		void PS(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE& src2) const;
		void PD(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE& src2) const;
	};

	struct xImplSimd_MoveSSE
	{
		SIMDInstructionInfo aligned_load;
		SIMDInstructionInfo aligned_store;
		SIMDInstructionInfo unaligned_load;
		SIMDInstructionInfo unaligned_store;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const;
		void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const;
	};

	struct xImplSimd_Blend
	{
		xImplSimd_3ArgImm PS;

		xImplSimd_3ArgImm PD;

		xImplSimd_4ArgBlend VPS;

		xImplSimd_4ArgBlend VPD;
	};

	struct xImplSimd_PBlend
	{
		xImplSimd_3ArgImm W;
		xImplSimd_4ArgBlend VB;
	};

	struct xImplSimd_PMove
	{
		SIMDInstructionInfo info;

		void BW(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void BW(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void BD(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void BD(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void BQ(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void BQ(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void WD(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void WD(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void WQ(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void WQ(const xRegisterSSE& to, const xIndirectVoid& from) const;

		void DQ(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void DQ(const xRegisterSSE& to, const xIndirectVoid& from) const;
	};
}
