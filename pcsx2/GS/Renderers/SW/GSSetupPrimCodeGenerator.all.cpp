// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSSetupPrimCodeGenerator.all.h"
#include "GSVertexSW.h"
#include "common/Perf.h"

#include <cstddef>

MULTI_ISA_UNSHARED_IMPL;
using namespace Xbyak;

#define _rip_local(field) (ptr[_m_local + offsetof(GSScanlineLocalData, field)])
#define _rip_local_di(i, field) (ptr[_m_local + offsetof(GSScanlineLocalData, d[0].field) + (sizeof(GSScanlineLocalData::skip) * (i))])

#define THREEARG(operation, dst, src1, ...) \
	do \
	{ \
		if (hasAVX) \
		{ \
			v##operation(dst, src1, __VA_ARGS__); \
		} \
		else \
		{ \
			movdqa(dst, src1); \
			operation(dst, __VA_ARGS__); \
		} \
	} while (0)

#if _M_SSE >= 0x501
	#define _rip_local_d(x) _rip_local(d8.x)
	#define _rip_local_d_p(x) _rip_local_d(p.x)
#else
	#define _rip_local_d(x) _rip_local(d4.x)
	#define _rip_local_d_p(x) _rip_local_d(x)
#endif

GSSetupPrimCodeGenerator::GSSetupPrimCodeGenerator(u64 key, void* code, size_t maxsize)
	: GSNewCodeGenerator(code, maxsize)
	, many_regs(false)
#ifdef _WIN32
	, _64_vertex(rcx)
	, _index(rdx)
	, _dscan(r8)
	, _m_local(r9), t1(r10)
#else
	, _64_vertex(rdi)
	, _index(rsi)
	, _dscan(rdx)
	, _m_local(rcx), t1(r8)
#endif
{
	m_sel.key = key;

	m_en.z = m_sel.zb ? 1 : 0;
	m_en.f = m_sel.fb && m_sel.fge ? 1 : 0;
	m_en.t = m_sel.fb && m_sel.tfx != TFX_NONE ? 1 : 0;
	m_en.c = m_sel.fb && !(m_sel.tfx == TFX_DECAL && m_sel.tcc) ? 1 : 0;
}

void GSSetupPrimCodeGenerator::broadcastf128(const XYm& reg, const Address& mem)
{
#if SETUP_PRIM_USING_YMM
	vbroadcastf128(reg, mem);
#else
	movaps(reg, mem);
#endif
}

void GSSetupPrimCodeGenerator::broadcastss(const XYm& reg, const Address& mem)
{
	if (hasAVX)
	{
		vbroadcastss(reg, mem);
	}
	else
	{
		movss(reg, mem);
		shufps(reg, reg, _MM_SHUFFLE(0, 0, 0, 0));
	}
}

void GSSetupPrimCodeGenerator::Generate()
{
	bool needs_shift = ((m_en.z || m_en.f) && m_sel.prim != GS_SPRITE_CLASS) || m_en.t || (m_en.c && m_sel.iip);
	many_regs = isYmm && !m_sel.notest && needs_shift;

#ifdef _WIN64
	int needs_saving = many_regs ? 7 : m_sel.notest ? 1 : 3;
	if (needs_saving)
	{
		sub(rsp, 8 + 16 * needs_saving);
		for (int i = 0; i < needs_saving; i++)
		{
			movdqa(ptr[rsp + i * 16], Xmm(i + 6));
		}
	}
#endif

	if (needs_shift)
	{

		if (isXmm)
			mov(rax, (size_t)g_const_128b.m_shift);
		else
			mov(rax, (size_t)g_const_256b.m_shift);

		for (int i = 0; i < (m_sel.notest ? 2 : many_regs ? 9 : 5); i++)
		{
			if (isXmm)
				movaps(XYm(3 + i), ptr[rax + i * vecsize]);
			else if (i == 0)
				vbroadcastss(xym3, ptr[rax]);
			else
				movups(XYm(3 + i), ptr[rax + (9 - i) * sizeof(float)]);
		}
	}

	if (isXmm)
		Depth_XMM();
	else
		Depth_YMM();

	Texture();

	Color();

#ifdef _WIN64
	if (needs_saving)
	{
		for (int i = 0; i < needs_saving; i++)
		{
			movdqa(Xmm(i + 6), ptr[rsp + i * 16]);
		}
		add(rsp, 8 + 16 * needs_saving);
	}
#endif
	if (isYmm)
		vzeroupper();
	ret();

	Perf::any.RegisterKey(actual.getCode(), actual.getSize(), "GSSetupPrim_", m_sel.key);
}

void GSSetupPrimCodeGenerator::Depth_XMM()
{
	if (!m_en.z && !m_en.f)
	{
		return;
	}

	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		if (m_en.f)
		{
			broadcastss(xym1, ptr[_dscan + offsetof(GSVertexSW, t.w)]);

			THREEARG(mulps, xmm2, xmm1, xmm3);
			cvttps2dq(xmm2, xmm2);
			pshuflw(xmm2, xmm2, _MM_SHUFFLE(2, 2, 0, 0));
			pshufhw(xmm2, xmm2, _MM_SHUFFLE(2, 2, 0, 0));
			movdqa(_rip_local_d_p(f), xmm2);

			for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
			{

				THREEARG(mulps, xmm2, xmm1, XYm(4 + i));
				cvttps2dq(xmm2, xmm2);
				pshuflw(xmm2, xmm2, _MM_SHUFFLE(2, 2, 0, 0));
				pshufhw(xmm2, xmm2, _MM_SHUFFLE(2, 2, 0, 0));
				movdqa(_rip_local_di(i, f), xmm2);
			}
		}

		if (m_en.z)
		{
			movddup(xmm0, ptr[_dscan + offsetof(GSVertexSW, p.z)]);

			cvtps2pd(xmm1, xmm3);
			mulpd(xmm1, xmm0);
			movaps(_rip_local_d_p(z), xmm1);

			cvtpd2ps(xmm0, xmm0);
			unpcklpd(xmm0, xmm0);

			for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
			{

				THREEARG(mulps, xmm1, xmm0, XYm(4 + i));
				movdqa(_rip_local_di(i, z), xmm1);
			}
		}
	}
	else
	{

		movzx(eax, word[_index + sizeof(u16) * 1]);
		shl(eax, 6);
		add(rax, _64_vertex);

		if (m_en.f)
		{
			movaps(xmm0, ptr[rax + offsetof(GSVertexSW, p)]);

			cvttps2dq(xmm1, xmm0);
			pshufhw(xmm1, xmm1, _MM_SHUFFLE(2, 2, 2, 2));
			pshufd(xmm1, xmm1, _MM_SHUFFLE(2, 2, 2, 2));
			movdqa(_rip_local(p.f), xmm1);
		}

		if (m_en.z)
		{

			movdqa(xmm0, ptr[rax + offsetof(GSVertexSW, t)]);
			pshufd(xmm0, xmm0, _MM_SHUFFLE(3, 3, 3, 3));
			movdqa(_rip_local(p.z), xmm0);
		}
	}
}

void GSSetupPrimCodeGenerator::Depth_YMM()
{
	if (!m_en.z && !m_en.f)
	{
		return;
	}

	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		if (m_en.f)
		{
			vbroadcastss(ymm1, ptr[_dscan + offsetof(GSVertexSW, t.w)]);

			vmulps(xmm0, xmm1, xmm3);
			cvtps2dq(xmm0, xmm0);
			movd(_rip_local_d_p(f), xmm0);

			for (int i = 0; i < (m_sel.notest ? 1 : dsize); i++)
			{

				if (i < 4 || many_regs)
					vmulps(ymm0, Ymm(4 + i), ymm1);
				else
					vmulps(ymm0, ymm1, ptr[&g_const_256b.m_shift[8 - i]]);
				cvttps2dq(ymm0, ymm0);
				pshuflw(ymm0, ymm0, _MM_SHUFFLE(2, 2, 0, 0));
				pshufhw(ymm0, ymm0, _MM_SHUFFLE(2, 2, 0, 0));
				movdqa(_rip_local_di(i, f), ymm0);
			}
		}

		if (m_en.z)
		{
			movsd(xmm0, ptr[_dscan + offsetof(GSVertexSW, p.z)]);

			vcvtss2sd(xmm1, xmm3, xmm3);
			vmulsd(xmm1, xmm0, xmm1);
			movsd(_rip_local_d_p(z), xmm1);

			cvtsd2ss(xmm0, xmm0);
			vbroadcastss(ymm0, xmm0);

			for (int i = 0; i < (m_sel.notest ? 1 : dsize); i++)
			{

				if (i < 4 || many_regs)
					vmulps(ymm1, Ymm(4 + i), ymm0);
				else
					vmulps(ymm1, ymm0, ptr[&g_const_256b.m_shift[8 - i]]);
				movaps(_rip_local_di(i, z), ymm1);
			}
		}
	}
	else
	{

		movzx(eax, word[_index + sizeof(u16) * 1]);
		shl(eax, 6);
		add(rax, _64_vertex);

		if (m_en.f)
		{

			movaps(xmm0, ptr[rax + offsetof(GSVertexSW, p)]);
			cvttps2dq(xmm0, xmm0);
			pextrd(_rip_local(p.f), xmm0, 3);
		}

		if (m_en.z)
		{

			mov(t1.cvt32(), ptr[rax + offsetof(GSVertexSW, t.w)]);
			mov(_rip_local(p.z), t1.cvt32());
		}
	}
}

void GSSetupPrimCodeGenerator::Texture()
{
	if (!m_en.t)
	{
		return;
	}

	broadcastf128(xym0, ptr[_dscan + offsetof(GSVertexSW, t)]);

	THREEARG(mulps, xmm1, xmm0, xmm3);

	if (m_sel.fst)
	{

		cvttps2dq(xmm1, xmm1);

		movdqa(_rip_local_d(stq), xmm1);
	}
	else
	{

		movaps(_rip_local_d(stq), xmm1);
	}

	for (int j = 0, k = m_sel.fst ? 2 : 3; j < k; j++)
	{

		THREEARG(shufps, xym1, xym0, xym0, _MM_SHUFFLE(j, j, j, j));

		for (int i = 0; i < (m_sel.notest ? 1 : dsize); i++)
		{

			if (i < 4 || many_regs)
				THREEARG(mulps, xym2, XYm(4 + i), xym1);
			else
				vmulps(ymm2, ymm1, ptr[&g_const_256b.m_shift[8 - i]]);

			if (m_sel.fst)
			{

				cvttps2dq(xym2, xym2);

				switch (j)
				{
					case 0: movdqa(_rip_local_di(i, s), xym2); break;
					case 1: movdqa(_rip_local_di(i, t), xym2); break;
				}
			}
			else
			{

				switch (j)
				{
					case 0: movaps(_rip_local_di(i, s), xym2); break;
					case 1: movaps(_rip_local_di(i, t), xym2); break;
					case 2: movaps(_rip_local_di(i, q), xym2); break;
				}
			}
		}
	}
}

void GSSetupPrimCodeGenerator::Color()
{
	if (!m_en.c)
	{
		return;
	}

	if (m_sel.iip)
	{

		broadcastf128(xym0, ptr[_dscan + offsetof(GSVertexSW, c)]);

		XYm mask16 = XYm(many_regs ? 12 : m_sel.notest ? 6 : 8);
		pcmpeqd(mask16, mask16);
		psrld(mask16, 16);

		THREEARG(mulps, xmm1, xmm0, xmm3);
		cvttps2dq(xmm1, xmm1);
		pshufd(xmm1, xmm1, _MM_SHUFFLE(3, 1, 2, 0));
		pand(xym1, mask16);
		packusdw(xmm1, xmm1);
		if (isXmm)
			movdqa(_rip_local_d(c), xmm1);
		else
			movq(_rip_local_d(c), xmm1);

		THREEARG(shufps, xym2, xym0, xym0, _MM_SHUFFLE(0, 0, 0, 0));
		THREEARG(shufps, xym3, xym0, xym0, _MM_SHUFFLE(2, 2, 2, 2));

		for (int i = 0; i < (m_sel.notest ? 1 : dsize); i++)
		{

			if (i < 4 || many_regs)
				THREEARG(mulps, xym0, XYm(4 + i), xym2);
			else
				vmulps(ymm0, ymm2, ptr[&g_const_256b.m_shift[8 - i]]);
			cvttps2dq(xym0, xym0);
			pand(xym0, mask16);
			packusdw(xym0, xym0);

			if (i < 4 || many_regs)
				THREEARG(mulps, xym1, XYm(4 + i), xym3);
			else
				vmulps(ymm1, ymm3, ptr[&g_const_256b.m_shift[8 - i]]);
			cvttps2dq(xym1, xym1);
			pand(xym1, mask16);
			packusdw(xym1, xym1);

			punpcklwd(xym0, xym1);
			movdqa(_rip_local_di(i, rb), xym0);
		}

		broadcastf128(xym0, ptr[_dscan + offsetof(GSVertexSW, c)]);

		THREEARG(shufps, xym2, xym0, xym0, _MM_SHUFFLE(1, 1, 1, 1));
		THREEARG(shufps, xym3, xym0, xym0, _MM_SHUFFLE(3, 3, 3, 3));

		for (int i = 0; i < (m_sel.notest ? 1 : dsize); i++)
		{

			if (i < 4 || many_regs)
				THREEARG(mulps, xym0, XYm(4 + i), xym2);
			else
				vmulps(ymm0, ymm2, ptr[&g_const_256b.m_shift[8 - i]]);
			cvttps2dq(xym0, xym0);
			pand(xym0, mask16);
			packusdw(xym0, xym1);

			if (i < 4 || many_regs)
				THREEARG(mulps, xym1, XYm(4 + i), xym3);
			else
				vmulps(ymm1, ymm3, ptr[&g_const_256b.m_shift[8 - i]]);
			cvttps2dq(xym1, xym1);
			pand(xym1, mask16);
			packusdw(xym1, xym1);

			punpcklwd(xym0, xym1);
			movdqa(_rip_local_di(i, ga), xym0);
		}
	}
	else
	{

		int last = 0;

		switch (m_sel.prim)
		{
			case GS_POINT_CLASS:    last = 0; break;
			case GS_LINE_CLASS:     last = 1; break;
			case GS_TRIANGLE_CLASS: last = 2; break;
			case GS_SPRITE_CLASS:   last = 1; break;
		}

		if (!(m_sel.prim == GS_SPRITE_CLASS && (m_en.z || m_en.f)))
		{
			movzx(eax, word[_index + sizeof(u16) * last]);
			shl(eax, 6);
			add(rax, _64_vertex);
		}

		if (isXmm)
		{
			cvttps2dq(xmm0, ptr[rax + offsetof(GSVertexSW, c)]);
		}
		else
		{
			vbroadcasti128(ymm0, ptr[rax + offsetof(GSVertexSW, c)]);
			cvttps2dq(ymm0, ymm0);
		}

		pshufd(xym1, xym0, _MM_SHUFFLE(1, 0, 3, 2));
		punpcklwd(xym0, xym1);

		if (m_sel.tfx == TFX_NONE)
		{
			psrlw(xym0, 7);
		}

		pshufd(xym1, xym0, _MM_SHUFFLE(0, 0, 0, 0));
		pshufd(xym2, xym0, _MM_SHUFFLE(2, 2, 2, 2));

		movdqa(_rip_local(c.rb), xym1);
		movdqa(_rip_local(c.ga), xym2);
	}
}
