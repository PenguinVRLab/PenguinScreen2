// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct xImplSimd_Shuffle
	{
		inline void _selector_assertion_check(u8 selector) const;

		void PS(const xRegisterSSE& dst, const xRegisterSSE&  src, u8 selector) const { PS(dst, dst, src, selector); }
		void PS(const xRegisterSSE& dst, const xIndirectVoid& src, u8 selector) const { PS(dst, dst, src, selector); }
		void PS(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE&  src2, u8 selector) const;
		void PS(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2, u8 selector) const;

		void PD(const xRegisterSSE& dst, const xRegisterSSE&  src, u8 selector) const { PD(dst, dst, src, selector); }
		void PD(const xRegisterSSE& dst, const xIndirectVoid& src, u8 selector) const { PD(dst, dst, src, selector); }
		void PD(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE&  src2, u8 selector) const;
		void PD(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2, u8 selector) const;
	};

	struct xImplSimd_PShuffle
	{
		const xImplSimd_2ArgImm D;

		const xImplSimd_2ArgImm LW;

		const xImplSimd_2ArgImm HW;

		const xImplSimd_3Arg B;
	};

	struct SimdImpl_PUnpack
	{
		const xImplSimd_3Arg LBW;
		const xImplSimd_3Arg LWD;
		const xImplSimd_3Arg LDQ;
		const xImplSimd_3Arg LQDQ;

		const xImplSimd_3Arg HBW;
		const xImplSimd_3Arg HWD;
		const xImplSimd_3Arg HDQ;
		const xImplSimd_3Arg HQDQ;
	};

	struct SimdImpl_Pack
	{
		const xImplSimd_3Arg SSWB;

		const xImplSimd_3Arg SSDW;

		const xImplSimd_3Arg USWB;

		const xImplSimd_3Arg USDW;
	};

	struct xImplSimd_Unpack
	{
		const xImplSimd_3Arg HPS;

		const xImplSimd_3Arg HPD;

		const xImplSimd_3Arg LPS;

		const xImplSimd_3Arg LPD;
	};


	struct xImplSimd_PInsert
	{
		void B(const xRegisterSSE& dst, const xRegister32& src, u8 imm8) const { B(dst, dst, src, imm8); }
		void B(const xRegisterSSE& dst, const xIndirect8&  src, u8 imm8) const { B(dst, dst, src, imm8); }
		void B(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegister32& src2, u8 imm8) const;
		void B(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirect8&  src2, u8 imm8) const;

		void W(const xRegisterSSE& dst, const xRegister32& src, u8 imm8) const { W(dst, dst, src, imm8); }
		void W(const xRegisterSSE& dst, const xIndirect16& src, u8 imm8) const { W(dst, dst, src, imm8); }
		void W(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegister32& src2, u8 imm8) const;
		void W(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirect16& src2, u8 imm8) const;

		void D(const xRegisterSSE& dst, const xRegister32& src, u8 imm8) const { D(dst, dst, src, imm8); }
		void D(const xRegisterSSE& dst, const xIndirect32& src, u8 imm8) const { D(dst, dst, src, imm8); }
		void D(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegister32& src2, u8 imm8) const;
		void D(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirect32& src2, u8 imm8) const;

		void Q(const xRegisterSSE& dst, const xRegister64& src, u8 imm8) const { Q(dst, dst, src, imm8); }
		void Q(const xRegisterSSE& dst, const xIndirect64& src, u8 imm8) const { Q(dst, dst, src, imm8); }
		void Q(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegister64& src2, u8 imm8) const;
		void Q(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirect64& src2, u8 imm8) const;
	};

	struct SimdImpl_PExtract
	{
		void B(const xRegister32& dst, const xRegisterSSE& src, u8 imm8) const;
		void B(const xIndirect8&  dst, const xRegisterSSE& src, u8 imm8) const;

		void W(const xRegister32& dst, const xRegisterSSE& src, u8 imm8) const;
		void W(const xIndirect16& dst, const xRegisterSSE& src, u8 imm8) const;

		void D(const xRegister32& dst, const xRegisterSSE& src, u8 imm8) const;
		void D(const xIndirect32& dst, const xRegisterSSE& src, u8 imm8) const;

		void Q(const xRegister64& dst, const xRegisterSSE& src, u8 imm8) const;
		void Q(const xIndirect64& dst, const xRegisterSSE& src, u8 imm8) const;
	};
}
