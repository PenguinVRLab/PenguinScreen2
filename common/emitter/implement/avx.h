// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{
	struct xImplAVX_Move
	{
		u8 Prefix;
		u8 LoadOpcode;
		u8 StoreOpcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from) const;
		void operator()(const xRegisterSSE& to, const xIndirectVoid& from) const;
		void operator()(const xIndirectVoid& to, const xRegisterSSE& from) const;
	};

	struct xImplAVX_ThreeArg
	{
		u8 Prefix;
		u8 Opcode;

		void operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
	};

	struct xImplAVX_ThreeArgYMM : xImplAVX_ThreeArg
	{
		void operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void operator()(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
	};

	struct xImplAVX_ArithFloat
	{
		xImplAVX_ThreeArgYMM PS;
		xImplAVX_ThreeArgYMM PD;
		xImplAVX_ThreeArg SS;
		xImplAVX_ThreeArg SD;
	};

	struct xImplAVX_CmpFloatHelper
	{
		SSE2_ComparisonType CType;

		void PS(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void PS(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
		void PD(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void PD(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;

		void SS(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void SS(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
		void SD(const xRegisterSSE& to, const xRegisterSSE& from1, const xRegisterSSE& from2) const;
		void SD(const xRegisterSSE& to, const xRegisterSSE& from1, const xIndirectVoid& from2) const;
	};

	struct xImplAVX_CmpFloat
	{
		xImplAVX_CmpFloatHelper EQ;
		xImplAVX_CmpFloatHelper LT;
		xImplAVX_CmpFloatHelper LE;
		xImplAVX_CmpFloatHelper UO;
		xImplAVX_CmpFloatHelper NE;
		xImplAVX_CmpFloatHelper GE;
		xImplAVX_CmpFloatHelper GT;
		xImplAVX_CmpFloatHelper OR;
	};

	struct xImplAVX_CmpInt
	{
		const xImplAVX_ThreeArgYMM EQB;

		const xImplAVX_ThreeArgYMM EQW;

		const xImplAVX_ThreeArgYMM EQD;

		const xImplAVX_ThreeArgYMM GTB;

		const xImplAVX_ThreeArgYMM GTW;

		const xImplAVX_ThreeArgYMM GTD;
	};
}
