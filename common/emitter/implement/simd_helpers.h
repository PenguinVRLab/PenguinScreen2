// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct SIMDInstructionInfo {
		enum class Prefix : u32 {
			None = 0,
			P66  = 1,
			PF3  = 2,
			PF2  = 3,
		};
		enum class Map : u32 {
			M0F   = 1,
			M0F38 = 2,
			M0F3A = 3,
		};
		enum class Type : u32 {
			Float, Integer, Double
		};

		u32 opcode : 8;
		Prefix prefix : 2;
		Map map : 5;
		Type type : 2;
		u32 ext : 3;
		u32 is_commutative : 1;
		u32 is_mov : 1;
		u32 dst_w : 1;
		u32 src_w : 1;
		u32 w_bit : 1;

		constexpr SIMDInstructionInfo(u8 opcode_, u8 ext_ = 0)
			: opcode(opcode_), prefix(Prefix::None), map(Map::M0F), type(Type::Float), ext(ext_)
			, is_commutative(false), is_mov(false), dst_w(false), src_w(false), w_bit(false)
		{
		}

		constexpr SIMDInstructionInfo p66() const { SIMDInstructionInfo copy = *this; copy.prefix = Prefix::P66; return copy; }
		constexpr SIMDInstructionInfo pf3() const { SIMDInstructionInfo copy = *this; copy.prefix = Prefix::PF3; return copy; }
		constexpr SIMDInstructionInfo pf2() const { SIMDInstructionInfo copy = *this; copy.prefix = Prefix::PF2; return copy; }
		constexpr SIMDInstructionInfo m0f38() const { SIMDInstructionInfo copy = *this; copy.map = Map::M0F38; return copy; }
		constexpr SIMDInstructionInfo m0f3a() const { SIMDInstructionInfo copy = *this; copy.map = Map::M0F3A; return copy; }
		constexpr SIMDInstructionInfo f() const { SIMDInstructionInfo copy = *this; copy.type = Type::Float;   return copy; }
		constexpr SIMDInstructionInfo i() const { SIMDInstructionInfo copy = *this; copy.type = Type::Integer; return copy; }
		constexpr SIMDInstructionInfo d() const { SIMDInstructionInfo copy = *this; copy.type = Type::Double;  return copy; }
		constexpr SIMDInstructionInfo w() const { SIMDInstructionInfo copy = *this; copy.w_bit = true; return copy; }
		constexpr SIMDInstructionInfo dstw() const { SIMDInstructionInfo copy = *this; copy.dst_w = true; return copy; }
		constexpr SIMDInstructionInfo srcw() const { SIMDInstructionInfo copy = *this; copy.src_w = true; return copy; }
		constexpr SIMDInstructionInfo commutative() const { SIMDInstructionInfo copy = *this; copy.is_commutative = true; return copy; }
		constexpr SIMDInstructionInfo mov() const { SIMDInstructionInfo copy = *this; copy.is_mov = true; return copy; }
	};

	struct xImplSimd_2Arg
	{
		SIMDInstructionInfo info;

		constexpr xImplSimd_2Arg(SIMDInstructionInfo info_): info(info_.mov()) {}

		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src)  const;
		void operator()(const xRegisterSSE& dst, const xIndirectVoid& src) const;
	};

	struct xImplSimd_2ArgImm
	{
		SIMDInstructionInfo info;

		constexpr xImplSimd_2ArgImm(SIMDInstructionInfo info_): info(info_.mov()) {}

		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src, u8 imm)  const;
		void operator()(const xRegisterSSE& dst, const xIndirectVoid& src, u8 imm) const;
	};

	struct xImplSimd_3Arg
	{
		SIMDInstructionInfo info;

		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src)  const { (*this)(dst, dst, src); }
		void operator()(const xRegisterSSE& dst, const xIndirectVoid& src) const { (*this)(dst, dst, src); }
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE& src2)  const;
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2) const;
	};

	struct xImplSimd_3ArgImm
	{
		SIMDInstructionInfo info;

		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src,  u8 imm) const { (*this)(dst, dst, src, imm); }
		void operator()(const xRegisterSSE& dst, const xIndirectVoid& src, u8 imm) const { (*this)(dst, dst, src, imm); }
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE& src2,  u8 imm) const;
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2, u8 imm) const;
	};

	struct xImplSimd_3ArgCmp
	{
		SIMDInstructionInfo info;

		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src,  SSE2_ComparisonType imm) const { (*this)(dst, dst, src, imm); }
		void operator()(const xRegisterSSE& dst, const xIndirectVoid& src, SSE2_ComparisonType imm) const { (*this)(dst, dst, src, imm); }
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE& src2,  SSE2_ComparisonType imm) const;
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2, SSE2_ComparisonType imm) const;
	};

	struct xImplSimd_4ArgBlend
	{
		SIMDInstructionInfo sse;
		SIMDInstructionInfo avx;

		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src)  const { (*this)(dst, dst, src, xmm0); }
		void operator()(const xRegisterSSE& dst, const xIndirectVoid& src) const { (*this)(dst, dst, src, xmm0); }
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xRegisterSSE& src2,  const xRegisterSSE& src3) const;
		void operator()(const xRegisterSSE& dst, const xRegisterSSE& src1, const xIndirectVoid& src2, const xRegisterSSE& src3) const;
	};
}
