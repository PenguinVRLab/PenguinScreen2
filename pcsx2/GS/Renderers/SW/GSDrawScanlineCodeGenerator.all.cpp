// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSDrawScanlineCodeGenerator.all.h"
#include "GS/Renderers/Common/GSFunctionMap.h"
#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GSVertexSW.h"
#include "common/Perf.h"

#include <cstddef>

MULTI_ISA_UNSHARED_IMPL;
using namespace Xbyak;

#ifdef __clang__
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

#define _rip_local(field) ptr[_m_local + offsetof(GSScanlineLocalData, field)]
#define _rip_local_offset(field, offset) ptr[_m_local + offsetof(GSScanlineLocalData, field) + (offset)]
#define _rip_global(field) ptr[_m_local__gd + offsetof(GSScanlineGlobalData, field)]
#define _rip_global_offset(field, offset) ptr[_m_local__gd + offsetof(GSScanlineGlobalData, field) + (offset)]

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

#define MOVE_IF_64(operation, dst, src64, ...) \
	do \
	{ \
		THREEARG(operation, dst, src64, __VA_ARGS__); \
	} while (0)

#define USING_XMM DRAW_SCANLINE_USING_XMM
#define USING_YMM DRAW_SCANLINE_USING_YMM

#if _M_SSE >= 0x501
	#define BROADCAST_AND_OP(broadcast, op, dst, tmpReg, src) \
		do \
		{ \
			broadcast(tmpReg, src); \
			op(dst, tmpReg); \
		} while (0)
	#define _rip_local_d(x) _rip_local(d8.x)
	#define _rip_local_d_p(x) _rip_local_d(p.x)
#else
	#define BROADCAST_AND_OP(broadcast, op, dst, tmpReg, src) \
		op(dst, src)
	#define _rip_local_d(x) _rip_local(d4.x)
	#define _rip_local_d_p(x) _rip_local_d(x)
#endif

#if USING_YMM
static constexpr const GSScanlineConstantData256B& g_const = g_const_256b;
#else
static constexpr const GSScanlineConstantData128B& g_const = g_const_128b;
#endif

template <typename A, typename B>
static bool IsInRipRelativeRange(A* a, B* b)
{
	uptr ai = reinterpret_cast<uptr>(a);
	uptr bi = reinterpret_cast<uptr>(b);
	sptr diff = static_cast<sptr>(bi - ai);
	return diff == static_cast<s32>(diff);
}

template <typename A, typename B>
static s32 CalcOffset(A* from, B* to)
{
	uptr ai = reinterpret_cast<uptr>(from);
	uptr bi = reinterpret_cast<uptr>(to);
	return static_cast<s32>(bi - ai);
}

GSDrawScanlineCodeGenerator::GSDrawScanlineCodeGenerator(u64 key, void* code, size_t maxsize)
	: GSNewCodeGenerator(code, maxsize)
#ifdef _WIN32
	, a0(rcx), a1(rdx)
	, a2(r8) , a3(r9)
	, t0(rdi), t1(rsi)
	, t2(r8) , t3(r9)
	, _m_local(r10)
#else
	, a0(rdi), a1(rsi)
	, a2(rdx), a3(rcx)
	, t0(r10), t1(r9)
	, t2(rcx), t3(rsi)
	, _m_local(r8)
#endif
	, _m_const(r14)
	, _m_local__gd(r12)
	, _m_local__gd__vm(t3)
	, _m_local__gd__clut(r11)
	, _m_local__gd__tex(r13)
	, _rb(xym5), _ga(xym6), _fm(xym3), _zm(xym4), _fd(xym2), _test(xym15)
	, _z(xym8), _f(xym9), _s(xym10), _t(xym11), _q(xym12), _f_rb(xym13), _f_ga(xym14)
{
	m_sel.key = key;
	use_lod = m_sel.mmin;
	if (isYmm)
		pxAssert(hasAVX2);
}

void GSDrawScanlineCodeGenerator::broadcastf128(const XYm& reg, const Address& mem)
{
#if USING_YMM
	vbroadcastf128(reg, mem);
#else
	movaps(reg, mem);
#endif
}

void GSDrawScanlineCodeGenerator::broadcasti128(const XYm& reg, const Address& mem)
{
#if USING_YMM
	vbroadcasti128(reg, mem);
#else
	movdqa(reg, mem);
#endif
}

void GSDrawScanlineCodeGenerator::broadcastssLocal(const XYm& reg, const Address& mem)
{
#if USING_YMM
	vbroadcastss(reg, mem);
#else
	movaps(reg, mem);
#endif
}

void GSDrawScanlineCodeGenerator::pbroadcastqLocal(const XYm& reg, const Address& mem)
{
#if USING_YMM
	vpbroadcastq(reg, mem);
#else
	movdqa(reg, mem);
#endif
}

void GSDrawScanlineCodeGenerator::pbroadcastdLocal(const XYm& reg, const Address& mem)
{
#if USING_YMM
	vpbroadcastd(reg, mem);
#else
	movdqa(reg, mem);
#endif
}

void GSDrawScanlineCodeGenerator::pbroadcastwLocal(const XYm& reg, const Address& mem)
{
#if USING_YMM
	vpbroadcastw(reg, mem);
#else
	movdqa(reg, mem);
#endif
}

void GSDrawScanlineCodeGenerator::broadcastsd(const XYm& reg, const Address& mem)
{
#if USING_YMM
	vbroadcastsd(reg, mem);
#else
	movddup(reg, mem);
#endif
}

void GSDrawScanlineCodeGenerator::broadcastGPRToVec(const XYm& vec, const Xbyak::Reg32& gpr)
{
	movd(Xmm(vec.getIdx()), gpr);
#if USING_YMM
	vpbroadcastd(vec, Xmm(vec.getIdx()));
#else
	pshufd(vec, vec, _MM_SHUFFLE(0, 0, 0, 0));
#endif
}

void GSDrawScanlineCodeGenerator::modulate16(const XYm& a, const Operand& f, u8 shift)
{
	psllw(a, shift + 1);
	pmulhw(a, f);
}

void GSDrawScanlineCodeGenerator::lerp16(const XYm& a, const XYm& b, const XYm& f, u8 shift)
{
	psubw(a, b);
	modulate16(a, f, shift);
	paddw(a, b);
}

void GSDrawScanlineCodeGenerator::lerp16_4(const XYm& a, const XYm& b, const XYm& f)
{
	psubw(a, b);
	pmullw(a, f);
	psraw(a, 4);
	paddw(a, b);
}

void GSDrawScanlineCodeGenerator::mix16(const XYm& a, const XYm& b, const XYm& temp)
{
	pblendw(a, b, 0xaa);
}

void GSDrawScanlineCodeGenerator::clamp16(const XYm& a, const XYm& temp)
{
	if (isXmm)
	{
		packuswb(a, a);
		pmovzxbw(a, a);
	}
	else
	{
		packuswb(a, a);
		pxor(temp, temp);
		punpcklbw(a, temp);
	}
}

void GSDrawScanlineCodeGenerator::alltrue(const XYm& test)
{
	u32 mask = test.isYMM() ? 0xffffffff : 0xffff;
	pmovmskb(eax, test);
	cmp(eax, mask);
	je("step", Xbyak::CodeGenerator::T_NEAR);
}

void GSDrawScanlineCodeGenerator::blend(const XYm& a, const XYm& b, const XYm& mask)
{
	pand(b, mask);
	pandn(mask, a);
	if (hasAVX)
	{
		vpor(a, b, mask);
	}
	else
	{
		por(b, mask);
		movdqa(a, b);
	}
}

void GSDrawScanlineCodeGenerator::blendr(const XYm& b, const XYm& a, const XYm& mask)
{
	pand(b, mask);
	pandn(mask, a);
	por(b, mask);
}

void GSDrawScanlineCodeGenerator::blend8(const XYm& a, const XYm& b)
{
	pblendvb(a, b );
}

void GSDrawScanlineCodeGenerator::blend8r(const XYm& b, const XYm& a)
{
	if (hasAVX)
	{
		vpblendvb(b, a, b, xym0);
	}
	else
	{
		pblendvb(a, b);
		movdqa(b, a);
	}
}

void GSDrawScanlineCodeGenerator::split16_2x8(const XYm& l, const XYm& h, const XYm& src)
{

	if (hasAVX)
	{
		if (src == h)
		{
			vpsllw(l, src, 8);
			psrlw(h, 8);
		}
		else if (src == l)
		{
			vpsrlw(h, src, 8);
			psllw(l, 8);
		}
		else
		{
			vpsllw(l, src, 8);
			vpsrlw(h, src, 8);
		}
		psrlw(l, 8);
	}
	else
	{
		if (src == h)
		{
			movdqa(l, src);
		}
		else if (src == l)
		{
			movdqa(h, src);
		}
		else
		{
			movdqa(l, src);
			movdqa(h, src);
		}
		psllw(l, 8);
		psrlw(l, 8);
		psrlw(h, 8);
	}
}

void GSDrawScanlineCodeGenerator::Generate()
{
	if (m_sel.breakpoint)
		db(0xCC);

	if (GSDrawScanline::ShouldUseCDrawScanline(m_sel.key))
	{
		auto cds = static_cast<void(*)(int, int, int, const GSVertexSW&, GSScanlineLocalData&)>(&GSDrawScanline::CDrawScanline);
		if (IsInRipRelativeRange(actual.getCode(), cds))
		{
			jmp(reinterpret_cast<void*>(cds));
		}
		else
		{
			mov(rax, reinterpret_cast<uptr>(cds));
			actual.jmp(ptr[rax]);
		}
		return;
	}

	const bool need_tex = m_sel.fb && m_sel.tfx != TFX_NONE;
	const bool need_clut = need_tex && m_sel.tlu;

#ifdef _WIN32
	push(rbx);
	push(rsi);
	push(rdi);
	push(r12);
	push(r13);
	push(r14);

	sub(rsp, _64_win_stack_size);

	for (int i = 0; i < 10; i++)
	{
		movdqa(ptr[rsp + _64_win_xmm_start + 16 * i], Xmm(i + 6));
	}
#else
	mov(ptr[rsp + _64_rz_rbx], rbx);
	mov(ptr[rsp + _64_rz_r12], r12);
	mov(ptr[rsp + _64_rz_r13], r13);
	mov(ptr[rsp + _64_rz_r14], r14);
#endif

#ifdef _WIN32
	mov(_m_local, ptr[rsp + _64_win_stack_size + 88]);
#endif

	const void* code = actual.getCode();
	const char* codeEnd = reinterpret_cast<const char*>(code) + (1024 * 1024);
	if (IsInRipRelativeRange(code, &g_const) && IsInRipRelativeRange(codeEnd, &g_const))
		lea(_m_const, ptr[rip + &g_const]);
	else
		mov(_m_const, reinterpret_cast<uptr>(&g_const));

	mov(_m_local__gd, _rip_local(gd));

	if (need_clut)
		mov(_m_local__gd__clut, _rip_global(clut));

	Init();

	if (!m_sel.edge)
	{
		align(16);
	}

L("loop");

	const bool tme = m_sel.tfx != TFX_NONE;

	TestZ(tme ? xym5 : xym2, tme ? xym6 : xym3);

	if (use_lod)
	{
		SampleTextureLOD();
	}
	else
	{
		SampleTexture();
	}

	AlphaTFX();

	ReadMask();

	TestAlpha();

	ColorTFX();

	Fog();

	ReadFrame();

	TestDestAlpha();

	WriteMask();

	WriteZBuf();

	AlphaBlend();

	WriteFrame();

L("step");

	if (!m_sel.edge)
	{
		test(a0.cvt32(), a0.cvt32());

		jle("exit", CodeGenerator::T_NEAR);

		Step();

		jmp("loop", CodeGenerator::T_NEAR);
	}

L("exit");

#ifdef _WIN32
	for (int i = 0; i < 10; i++)
	{
		movdqa(Xmm(i + 6), ptr[rsp + _64_win_xmm_start + 16 * i]);
	}
	add(rsp, _64_win_stack_size);

	pop(r14);
	pop(r13);
	pop(r12);
	pop(rdi);
	pop(rsi);
	pop(rbx);
#else
	mov(rbx, ptr[rsp + _64_rz_rbx]);
	mov(r12, ptr[rsp + _64_rz_r12]);
	mov(r13, ptr[rsp + _64_rz_r13]);
	mov(r14, ptr[rsp + _64_rz_r14]);
#endif
	if (isYmm)
		vzeroupper();
	ret();

	Perf::any.RegisterKey(actual.getCode(), actual.getSize(), "GSDrawScanline_", m_sel.key);
}

void GSDrawScanlineCodeGenerator::Init()
{
	if (!m_sel.notest)
	{

		mov(ebx, a1.cvt32());
		and_(a1.cvt32(), vecints - 1);

		sub(ebx, a1.cvt32());

		lea(a0.cvt32(), ptr[a0 + a1 - vecints]);

		if (isXmm)
		{
			mov(eax, a0.cvt32());
			sar(eax, 31);
			and_(eax, a0.cvt32());
			shl(eax, 4);
			cdqe();
			shl(a1.cvt32(), 4);
			movdqa(_test, ptr[a1 + _m_const + offsetof(GSScanlineConstantData128B, m_test[0])]);
			por(_test, ptr[rax + _m_const + offsetof(GSScanlineConstantData128B, m_test[7])]);
		}
		else
		{
			mov(eax, a1.cvt32());
			neg(rax);
			pmovsxbd(_test, ptr[rax + _m_const + offsetof(GSScanlineConstantData256B, m_test[16])]);
			xor_(t0.cvt32(), t0.cvt32());
			mov(eax, a0.cvt32());
			neg(eax);
			cmovs(eax, t0.cvt32());
			pmovsxbd(xym0, ptr[rax + _m_const + offsetof(GSScanlineConstantData256B, m_test[0])]);
			por(_test, xym0);
			shl(a1.cvt32(), 5);
		}
	}
	else
	{
		mov(ebx, a1.cvt32());
		xor_(a1.cvt32(), a1.cvt32());
		lea(a0.cvt32(), ptr[a0 - vecints]);
	}

	mov(rax, _rip_global(fzbr));
	lea(t1, ptr[rax + a2 * 8]);

	mov(rax, _rip_global(fzbc));
	lea(t0, ptr[rax + rbx * 2]);

	if ((m_sel.prim != GS_SPRITE_CLASS && ((m_sel.fwrite && m_sel.fge) || m_sel.zb)) || (m_sel.fb && (m_sel.edge || m_sel.tfx != TFX_NONE || m_sel.iip)))
	{
		lea(rax, _rip_local(d));
		lea(a1, ptr[rax + a1 * 8]);
	}

	const XYm& f = _f;

	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		if ((m_sel.fwrite && m_sel.fge) || m_sel.zb)
		{
			if (m_sel.fwrite && m_sel.fge)
			{
				if (isYmm)
					vbroadcastss(f, ptr[a3 + offsetof(GSVertexSW, t.w)]);
				else
					movss(f, ptr[a3 + offsetof(GSVertexSW, t.w)]);

				cvttps2dq(f, f);
				punpcklwd(f, f);
				pshufd(f, f, _MM_SHUFFLE(0, 0, 0, 0));
				paddw(f, ptr[a1 + offsetof(GSScanlineLocalData::skip, f)]);
			}

			if (m_sel.zb && m_sel.zequal)
			{
				Xmm zx(_z.getIdx());
				cvttsd2si(rax, ptr[a3 + offsetof(GSVertexSW, p.z)]);
				movd(zx, eax);
				if (hasAVX2)
					vpbroadcastd(_z, zx);
				else
					pshufd(_z, _z, _MM_SHUFFLE(0, 0, 0, 0));
			}
			else if (m_sel.zb)
			{
				broadcastsd(xym1, ptr[a3 + offsetof(GSVertexSW, p.z)]);
				cvtps2pd(xym7, ptr[a1 + offsetof(GSScanlineLocalData::skip, z.I8[0])]);
				addpd(xym7, xym1);
				movaps(_rip_local(temp.z0), xym7);
				cvtps2pd(_z, ptr[a1 + offsetof(GSScanlineLocalData::skip, z.I8[vecsize/2])]);
				addpd(_z, xym1);
			}
		}
	}
	else
	{
		if (m_sel.fwrite && m_sel.fge)
			pbroadcastwLocal(_f, _rip_local(p.f));
	}

	if (m_sel.fb)
	{
		if (m_sel.edge)
		{

			if (hasAVX2)
			{
				vpbroadcastw(xym3, ptr[a3 + offsetof(GSVertexSW, p.x)]);
			}
			else
			{
				movd(xmm3, ptr[a3 + offsetof(GSVertexSW, p.x)]);
				punpcklwd(xmm3, xmm3);
				pshufd(xmm3, xmm3, _MM_SHUFFLE(0, 0, 0, 0));
			}
			psrlw(xym3, 9);

			movdqa(_rip_local(temp.cov), xym3);
		}

		if (m_sel.tfx != TFX_NONE)
		{
			const XYm& vt = xym4;

			broadcastf128(vt, ptr[a3 + offsetof(GSVertexSW, t)]);

			const XYm& s = _s;
			const XYm& t = _t;

			if (m_sel.fst)
			{

				cvttps2dq(xym6, vt);

				pshufd(s, xym6, _MM_SHUFFLE(0, 0, 0, 0));
				pshufd(t, xym6, _MM_SHUFFLE(1, 1, 1, 1));

				paddd(s, ptr[a1 + offsetof(GSScanlineLocalData::skip, s)]);

				if (m_sel.prim != GS_SPRITE_CLASS || m_sel.mmin)
				{
					paddd(t, ptr[a1 + offsetof(GSScanlineLocalData::skip, t)]);
				}
				else if (m_sel.ltf)
				{
					XYm vf = xym5;
					pshuflw(vf, t, _MM_SHUFFLE(2, 2, 0, 0));
					pshufhw(vf, vf, _MM_SHUFFLE(2, 2, 0, 0));
					psrlw(vf, 12);
					movdqa(_rip_local(temp.vf), vf);
				}
			}
			else
			{
				const XYm& q = _q;

				if (hasAVX)
				{
					vshufps(s, vt, vt, _MM_SHUFFLE(0, 0, 0, 0));
					vshufps(t, vt, vt, _MM_SHUFFLE(1, 1, 1, 1));
					vshufps(q, vt, vt, _MM_SHUFFLE(2, 2, 2, 2));
				}
				else
				{
					movaps(s, vt);
					movaps(t, vt);
					movaps(q, vt);

					shufps(s, s, _MM_SHUFFLE(0, 0, 0, 0));
					shufps(t, t, _MM_SHUFFLE(1, 1, 1, 1));
					shufps(q, q, _MM_SHUFFLE(2, 2, 2, 2));
				}

				addps(s, ptr[a1 + offsetof(GSScanlineLocalData::skip, s)]);
				addps(t, ptr[a1 + offsetof(GSScanlineLocalData::skip, t)]);
				addps(q, ptr[a1 + offsetof(GSScanlineLocalData::skip, q)]);
			}
		}

		if (!(m_sel.tfx == TFX_DECAL && m_sel.tcc))
		{
			const XYm& f_rb = _f_rb;
			const XYm& f_ga = _f_ga;
			if (m_sel.iip)
			{

				if (isXmm)
				{
					cvttps2dq(xym6, ptr[a3 + offsetof(GSVertexSW, c)]);
				}
				else
				{
					vbroadcastf128(ymm6, ptr[a3 + offsetof(GSVertexSW, c)]);
					cvttps2dq(ymm6, ymm6);
				}

				pshufd(xym5, xym6, _MM_SHUFFLE(1, 0, 3, 2));
				punpcklwd(xym6, xym5);

				pshufd(f_rb, xym6, _MM_SHUFFLE(0, 0, 0, 0));
				pshufd(f_ga, xym6, _MM_SHUFFLE(2, 2, 2, 2));

				paddw(f_rb, ptr[a1 + offsetof(GSScanlineLocalData::skip, rb)]);
				paddw(f_ga, ptr[a1 + offsetof(GSScanlineLocalData::skip, ga)]);
			}
			else
			{
				movdqa(f_rb, _rip_local(c.rb));
				movdqa(f_ga, _rip_local(c.ga));
			}

			movdqa(_rb, _f_rb);
			movdqa(_ga, _f_ga);
		}
	}

	if (m_sel.fwrite && m_sel.fpsm == 2 && m_sel.dthe)
	{
		mov(ptr[rsp + _top], a2);
	}

	mov(_m_local__gd__vm, _rip_global(vm));
	if (m_sel.fb && m_sel.tfx != TFX_NONE)
	{
		if (use_lod)
			lea(_m_local__gd__tex, _rip_global(tex));
		else
			mov(_m_local__gd__tex, _rip_global(tex));
	}
}

void GSDrawScanlineCodeGenerator::Step()
{

	sub(a0.cvt32(), vecints);

	add(t0, vecsize / 2);

	const XYm& f = _f;

	if (m_sel.prim != GS_SPRITE_CLASS)
	{

		if (m_sel.zb && !m_sel.zequal)
		{
			broadcastsd(xym7, _rip_local_d_p(z));
			addpd(_z, xym7);
			addpd(xym7, _rip_local(temp.z0));
			movaps(_rip_local(temp.z0), xym7);
		}

		if (m_sel.fwrite && m_sel.fge)
		{
			BROADCAST_AND_OP(vpbroadcastw, paddw, f, xym0, _rip_local_d_p(f));
		}
	}

	if (m_sel.fb)
	{
		if (m_sel.tfx != TFX_NONE)
		{
			if (m_sel.fst)
			{
				const XYm& stq = xym0;

				broadcasti128(stq, _rip_local_d(stq));

				XYm s = xym1;
				pshufd(s, stq, _MM_SHUFFLE(0, 0, 0, 0));
				paddd(_s, s);

				XYm t = xym1;
				if (m_sel.prim != GS_SPRITE_CLASS || m_sel.mmin)
				{
					pshufd(t, stq, _MM_SHUFFLE(1, 1, 1, 1));
					paddd(_t, t);
				}
			}
			else
			{
				const XYm& s = xym2;
				const XYm& t = xym3;
				const XYm& q = xym1;

				if (hasAVX)
				{
					broadcastf128(q, _rip_local_d(stq));

					vshufps(s, q, q, _MM_SHUFFLE(0, 0, 0, 0));
					vshufps(t, q, q, _MM_SHUFFLE(1, 1, 1, 1));
					vshufps(q, q, q, _MM_SHUFFLE(2, 2, 2, 2));
				}
				else
				{
					movaps(q, _rip_local_d(stq));
					movaps(s, q);
					movaps(t, q);

					shufps(s, s, _MM_SHUFFLE(0, 0, 0, 0));
					shufps(t, t, _MM_SHUFFLE(1, 1, 1, 1));
					shufps(q, q, _MM_SHUFFLE(2, 2, 2, 2));
				}

				addps(_s, s);
				addps(_t, t);
				addps(_q, q);
			}
		}

		if (!(m_sel.tfx == TFX_DECAL && m_sel.tcc))
		{
			if (m_sel.iip)
			{
				XYm c = xym0;

				pbroadcastqLocal(c, _rip_local_d(c));

				pshufd(_rb, c, _MM_SHUFFLE(0, 0, 0, 0));
				pshufd(_ga, c, _MM_SHUFFLE(1, 1, 1, 1));

				paddw(_f_rb, _rb);
				paddw(_f_ga, _ga);

				pxor(c, c);
				pmaxsw(_f_rb, c);
				pmaxsw(_f_ga, c);
			}

			movdqa(_rb, _f_rb);
			movdqa(_ga, _f_ga);
		}
	}

	if (!m_sel.notest)
	{
#if USING_XMM

		mov(eax, a0.cvt32());
		sar(eax, 31);
		and_(eax, a0.cvt32());
		shl(eax, 4);
		cdqe();
		movdqa(_test, ptr[rax + _m_const + offsetof(GSScanlineConstantData128B, m_test[7])]);
#else
		xor_(t2.cvt32(), t2.cvt32());
		mov(eax, a0.cvt32());
		neg(eax);
		cmovs(eax, t2.cvt32());
		pmovsxbd(_test, ptr[rax + _m_const + offsetof(GSScanlineConstantData256B, m_test[0])]);
#endif
	}
}

void GSDrawScanlineCodeGenerator::TestZ(const XYm& temp1, const XYm& temp2)
{
	if (!m_sel.zb)
	{
		return;
	}

	mov(t2.cvt32(), dword[t1 + 4]);
	add(t2.cvt32(), dword[t0 + 4]);
	and_(t2.cvt32(), HALF_VM_SIZE - 1);

	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		if (m_sel.zequal)
		{
			movdqa(xym0, _z);
		}
		else if (m_sel.zoverflow)
		{

			broadcastsd(temp1, ptr[_m_const + CalcOffset(&g_const, &GSVector4::m_xc1e00000000fffff)]);

			addpd(xym7, temp1);
			addpd(temp1, _z);
			cvtpd2dq(xmm0, xym7);
			cvtpd2dq(Xmm(temp1.getIdx()), temp1);

#if USING_YMM
			vinserti128(xym0, xym0, Xmm(temp1.getIdx()), 1);
#else
			punpcklqdq(xym0, temp1);
#endif

			pcmpeqd(temp1, temp1);
			pslld(temp1, 31);
			paddd(xym0, temp1);
		}
		else
		{

			cvttpd2dq(xmm0, xym7);
			cvttpd2dq(Xmm(temp1.getIdx()), _z);
#if USING_YMM
			vinserti128(xym0, xym0, Xmm(temp1.getIdx()), 1);
#else
			punpcklqdq(xym0, temp1);
#endif
		}

		if (m_sel.zclamp)
		{
			const u8 amt = (u8)((m_sel.zpsm & 0x3) * 8);
			pcmpeqd(temp1, temp1);
			psrld(temp1, amt);
			pminud(xym0, temp1);
		}

		if (m_sel.zwrite)
		{
			movdqa(_rip_local(temp.zs), xym0);
		}
	}
	else
	{
		pbroadcastdLocal(xym0, _rip_local(p.z));
	}

	if (m_sel.ztest)
	{
		ReadPixel(temp2, temp1, t2);

		if (m_sel.zwrite && m_sel.zpsm < 2)
		{
			movdqa(_rip_local(temp.zd), temp2);
		}

		if (m_sel.zpsm)
		{
			pslld(temp2, static_cast<u8>(m_sel.zpsm * 8));
			psrld(temp2, static_cast<u8>(m_sel.zpsm * 8));
		}

		if (m_sel.zpsm == 0)
		{

			pcmpeqd(temp1, temp1);
			pslld(temp1, 31);

			psubd(xym0, temp1);
			psubd(temp2, temp1);
		}

		switch (m_sel.ztst)
		{
			case ZTST_GEQUAL:
				pcmpgtd(temp2, xym0);
				por(_test, temp2);
				break;

			case ZTST_GREATER:
				pcmpgtd(xym0, temp2);
				pcmpeqd(temp1, temp1);
				pxor(xym0, temp1);
				por(_test, xym0);
				break;
		}

		alltrue(_test);
	}
}

void GSDrawScanlineCodeGenerator::SampleTexture()
{
	if (!m_sel.fb || m_sel.tfx == TFX_NONE)
	{
		return;
	}

	const bool needsMoreRegs = isYmm;

	if (!m_sel.fst)
	{
		MOVE_IF_64(divps, xym2, _s, _q);
		MOVE_IF_64(divps, xym3, _t, _q);

		cvttps2dq(xym2, xym2);
		cvttps2dq(xym3, xym3);

		if (m_sel.ltf)
		{

			mov(eax, 0x8000);
			broadcastGPRToVec(xym1, eax);

			psubd(xym2, xym1);
			psubd(xym3, xym1);
		}
	}
	else
	{
		movdqa(xym2, _s);
		movdqa(xym3, _t);
	}

	if (m_sel.ltf)
	{
		const XYm& vf = xym7;

		pshuflw(xym4, xym2, _MM_SHUFFLE(2, 2, 0, 0));
		pshufhw(xym4, xym4, _MM_SHUFFLE(2, 2, 0, 0));
		psrlw(xym4, 12);

		if (m_sel.prim != GS_SPRITE_CLASS)
		{

			pshuflw(vf, xym3, _MM_SHUFFLE(2, 2, 0, 0));
			pshufhw(vf, vf, _MM_SHUFFLE(2, 2, 0, 0));
			psrlw(vf, 12);
			if (needsMoreRegs)
				movdqa(_rip_local(temp.vf), vf);
		}
		else if (!needsMoreRegs)
		{
			movdqa(vf, _rip_local(temp.vf));
		}
	}

	psrad(xym2, 16);
	psrad(xym3, 16);
	packssdw(xym2, xym3);

	if (m_sel.ltf)
	{

		pcmpeqd(xym0, xym0);
		psrlw(xym0, 15);
		THREEARG(paddw, xym3, xym2, xym0);

		Wrap(xym2, xym3);
	}
	else
	{

		Wrap(xym2);
	}

	SampleTexture_TexelReadHelper(0);

}

void GSDrawScanlineCodeGenerator::SampleTexture_TexelReadHelper(int mip_offset)
{
	const bool needsMoreRegs = isYmm;

	pxor(xym0, xym0);

	THREEARG(punpcklwd, xym5, xym2, xym0);
	punpckhwd(xym2, xym0);
	pslld(xym2, static_cast<u8>(m_sel.tw + 3));

	if (m_sel.ltf)
	{

		THREEARG(punpcklwd, xym1, xym3, xym0);
		punpckhwd(xym3, xym0);
		pslld(xym3, static_cast<u8>(m_sel.tw + 3));

		THREEARG(paddd, xym0, xym3, xym1);
		paddd(xym1, xym2);
		paddd(xym2, xym5);
		paddd(xym3, xym5);

		const XYm& tmp1 = xym7;
		const XYm& tmp2 = xym4;
		ReadTexel4(xym5, xym6, xym0, xym2, xym1, xym3, tmp1, tmp2, mip_offset);

		split16_2x8(xym3, xym6, xym6);

		split16_2x8(xym0, xym1, xym0);

		lerp16_4(xym0, xym3, xym4);
		lerp16_4(xym1, xym6, xym4);

		split16_2x8(xym2, xym3, xym2);

		split16_2x8(xym5, xym6, xym5);

		lerp16_4(xym5, xym2, xym4);
		lerp16_4(xym6, xym3, xym4);

		XYm vf = xym7;
		if (needsMoreRegs)
			movdqa(vf, _rip_local(temp.vf));

		lerp16_4(xym5, xym0, vf);
		lerp16_4(xym6, xym1, vf);
	}
	else
	{

		paddd(xym2, xym5);

		ReadTexel1(xym5, xym2, xym0, xym1, mip_offset);

		split16_2x8(xym5, xym6, xym5);
	}
}

void GSDrawScanlineCodeGenerator::Wrap(const XYm& uv)
{
	const XYm& mask = xym0;
	const XYm& min = xym1;
	const XYm& max = xym5;
	const XYm& tmp = xym6;

	int wms_clamp = ((m_sel.wms + 1) >> 1) & 1;
	int wmt_clamp = ((m_sel.wmt + 1) >> 1) & 1;

	int region = ((m_sel.wms | m_sel.wmt) >> 1) & 1;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				BROADCAST_AND_OP(vbroadcasti128, pmaxsw, uv, min, _rip_global(t.min));
			}
			else
			{
				pxor(tmp, tmp);
				pmaxsw(uv, tmp);
			}

			BROADCAST_AND_OP(vbroadcasti128, pminsw, uv, max, _rip_global(t.max));
		}
		else
		{
			BROADCAST_AND_OP(vbroadcasti128, pand, uv, min, _rip_global(t.min));

			if (region)
			{
				BROADCAST_AND_OP(vbroadcasti128, por, uv, max, _rip_global(t.max));
			}
		}
	}
	else
	{
		broadcasti128(min, _rip_global(t.min));
		broadcasti128(max, _rip_global(t.max));
		broadcasti128(mask, _rip_global(t.mask));

		THREEARG(pand, tmp, uv, min);
		if (region)
			por(tmp, max);
		pmaxsw(uv, min);
		pminsw(uv, max);
		blend8(uv, tmp );
	}
}

void GSDrawScanlineCodeGenerator::Wrap(const XYm& uv0, const XYm& uv1)
{
	const XYm& mask = xym0;
	const XYm& min = xym1;
	const XYm& max = xym5;
	const XYm& tmp = xym6;

	int wms_clamp = ((m_sel.wms + 1) >> 1) & 1;
	int wmt_clamp = ((m_sel.wmt + 1) >> 1) & 1;

	int region = ((m_sel.wms | m_sel.wmt) >> 1) & 1;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				broadcasti128(min, _rip_global(t.min));
				pmaxsw(uv0, min);
				pmaxsw(uv1, min);
			}
			else
			{
				pxor(tmp, tmp);
				pmaxsw(uv0, tmp);
				pmaxsw(uv1, tmp);
			}

			broadcasti128(max, _rip_global(t.max));
			pminsw(uv0, max);
			pminsw(uv1, max);
		}
		else
		{
			broadcasti128(min, _rip_global(t.min));
			pand(uv0, min);
			pand(uv1, min);

			if (region)
			{
				broadcasti128(max, _rip_global(t.max));
				por(uv0, max);
				por(uv1, max);
			}
		}
	}
	else
	{
		broadcasti128(min, _rip_global(t.min));
		broadcasti128(max, _rip_global(t.max));
		broadcasti128(mask, _rip_global(t.mask));

		for (const XYm& uv : {uv0, uv1})
		{
			THREEARG(pand, tmp, uv, min);
			if (region)
				por(tmp, max);
			pmaxsw(uv, min);
			pminsw(uv, max);
			pblendvb(uv, tmp );
		}
	}
}

static s32 log2_coeff_offset(int i)
{
	uptr base = reinterpret_cast<uptr>(&g_const);
	uptr target = reinterpret_cast<uptr>(&g_const.m_log2_coef[i]);
	return static_cast<s32>(target - base);
};

void GSDrawScanlineCodeGenerator::SampleTextureLOD()
{
	if (!m_sel.fb || m_sel.tfx == TFX_NONE)
	{
		return;
	}

	const bool needsMoreRegs = isYmm;

	movdqa(xym4, _q);

	if (!m_sel.fst)
	{
		MOVE_IF_64(divps, xym2, _s, xym4);
		MOVE_IF_64(divps, xym3, _t, xym4);

		cvttps2dq(xym2, xym2);
		cvttps2dq(xym3, xym3);
	}
	else
	{
		movdqa(xym2, _s);
		movdqa(xym3, _t);
	}

	if (!m_sel.lcm)
	{

		pcmpeqd(xym1, xym1);
		psrld(xym1, 25);
		THREEARG(pslld, xym0, xym4, 1);
		psrld(xym0, 24);
		psubd(xym0, xym1);
		cvtdq2ps(xym0, xym0);

		pslld(xym4, 9);
		psrld(xym4, 9);

#if USING_YMM
		auto load_log2_coeff = [this](const XYm& reg, int i)
		{
			vbroadcastss(reg, ptr[_m_const + log2_coeff_offset(i)]);
		};
		auto log2_coeff = [this, &load_log2_coeff](int i)
		{
			load_log2_coeff(xym6, i);
			return xym6;
		};
#else
		auto log2_coeff = [this](int i) -> Address
		{
			return ptr[_m_const + log2_coeff_offset(i)];
		};
		auto load_log2_coeff = [this, &log2_coeff](const XYm& reg, int i)
		{
			movaps(reg, log2_coeff(i));
		};
#endif

		load_log2_coeff(xym1, 3);
		orps(xym4, xym1);

		if (hasFMA)
		{
			load_log2_coeff(xym5, 0);
			vfmadd213ps(xym5, xym4, log2_coeff(1));
			vfmadd213ps(xym5, xym4, log2_coeff(2));
			subps(xym4, xym1);
			vfmadd213ps(xym4, xym5, xym0);
		}
		else
		{
			if (hasAVX)
			{
				vmulps(xym5, xym4, log2_coeff(0));
			}
			else
			{
				load_log2_coeff(xym5, 0);
				mulps(xym5, xym4);
			}
			addps(xym5, log2_coeff(1));
			mulps(xym5, xym4);
			subps(xym4, xym1);
			addps(xym5, log2_coeff(2));
			mulps(xym4, xym5);
			addps(xym4, xym0);
		}

		if (hasFMA)
		{
			movaps(xym5, _rip_global(l));
			vfmadd213ps(xym4, xym5, _rip_global(k));
		}
		else
		{
			mulps(xym4, _rip_global(l));
			addps(xym4, _rip_global(k));
		}

		xorps(xym0, xym0);
		minps(xym4, _rip_global(mxl));
		maxps(xym4, xym0);
		cvtps2dq(xym4, xym4);

		if (m_sel.mmin == 1)
		{
			mov(eax, 0x8000);
			broadcastGPRToVec(xym0, eax);
			paddd(xym4, xym0);
		}

		THREEARG(psrld, xym0, xym4, 16);

		movdqa(_rip_local(temp.lod.i), xym0);
		if (m_sel.mmin == 2)
		{
			pshuflw(xym1, xym4, _MM_SHUFFLE(2, 2, 0, 0));
			pshufhw(xym1, xym1, _MM_SHUFFLE(2, 2, 0, 0));
			movdqa(_rip_local(temp.lod.f), xym1);
		}

		if (hasAVX2)
		{
			vpsravd(xym2, xym2, xym0);
			vpsravd(xym3, xym3, xym0);

			movdqa(_rip_local(temp.uv[0]), xym2);
			movdqa(_rip_local(temp.uv[1]), xym3);

			pxor(xym1, xym1);

			broadcasti128(xym4, _rip_global(t.min));
			vpunpcklwd(xym5, xym4, xym1);
			vpunpckhwd(xym6, xym4, xym1);
			vpsrlvd(xym5, xym5, xym0);
			vpsrlvd(xym6, xym6, xym0);
			packusdw(xym5, xym6);

			broadcasti128(xym4, _rip_global(t.max));
			vpunpcklwd(xym6, xym4, xym1);
			vpunpckhwd(xym4, xym4, xym1);
			vpsrlvd(xym6, xym6, xym0);
			vpsrlvd(xym4, xym4, xym0);
			packusdw(xym6, xym4);

			movdqa(_rip_local(temp.uv_minmax[0]), xym5);
			movdqa(_rip_local(temp.uv_minmax[1]), xym6);
		}
		else
		{
			movq(xym4, _rip_global(t.minmax));

			THREEARG(punpckhdq, xym6, xym2, xym3);
			punpckldq(xym2, xym3);
			movdqa(xym5, xym2);
			movdqa(xym3, xym6);

			movd(xym0, _rip_local(temp.lod.i.U32[0]));
			psrad(xym2, xym0);
			THREEARG(psrlw, xym1, xym4, xym0);
			movq(_rip_local(temp.uv_minmax[0].U32[0]), xym1);

			movd(xym0, _rip_local(temp.lod.i.U32[1]));
			psrad(xym5, xym0);
			THREEARG(psrlw, xym1, xym4, xym0);
			movq(_rip_local(temp.uv_minmax[1].U32[0]), xym1);

			movd(xym0, _rip_local(temp.lod.i.U32[2]));
			psrad(xym3, xym0);
			THREEARG(psrlw, xym1, xym4, xym0);
			movq(_rip_local(temp.uv_minmax[0].U32[2]), xym1);

			movd(xym0, _rip_local(temp.lod.i.U32[3]));
			psrad(xym6, xym0);
			THREEARG(psrlw, xym1, xym4, xym0);
			movq(_rip_local(temp.uv_minmax[1].U32[2]), xym1);

			punpckldq(xym2, xym3);
			punpckhdq(xym5, xym6);
			THREEARG(punpckhdq, xym3, xym2, xym5);
			punpckldq(xym2, xym5);

			movdqa(_rip_local(temp.uv[0]), xym2);
			movdqa(_rip_local(temp.uv[1]), xym3);

			movdqa(xym5, _rip_local(temp.uv_minmax[0]));
			movdqa(xym6, _rip_local(temp.uv_minmax[1]));

			if (hasAVX)
			{
				vpunpcklwd(xym0, xym5, xym6);
				vpunpckhwd(xym1, xym5, xym6);
				vpunpckldq(xym5, xym0, xym1);
				vpunpckhdq(xym6, xym0, xym1);
			}
			else
			{
				movdqa(xym0, xym5);
				punpcklwd(xym5, xym6);
				punpckhwd(xym0, xym6);
				movdqa(xym6, xym5);
				punpckldq(xym5, xym0);
				punpckhdq(xym6, xym0);
			}

			movdqa(_rip_local(temp.uv_minmax[0]), xym5);
			movdqa(_rip_local(temp.uv_minmax[1]), xym6);
		}
	}
	else
	{

		movd(Xmm(xym0.getIdx()), _rip_global(lod.i.U32[0]));

		psrad(xym2, Xmm(xym0.getIdx()));
		psrad(xym3, Xmm(xym0.getIdx()));

		movdqa(_rip_local(temp.uv[0]), xym2);
		movdqa(_rip_local(temp.uv[1]), xym3);

		movdqa(xym5, _rip_local(temp.uv_minmax[0]));
		movdqa(xym6, _rip_local(temp.uv_minmax[1]));
	}

	if (m_sel.ltf)
	{
		const XYm& vf = xym7;

		mov(eax, 0x8000);
		broadcastGPRToVec(xym4, eax);

		psubd(xym2, xym4);
		psubd(xym3, xym4);

		pshuflw(xym4, xym2, _MM_SHUFFLE(2, 2, 0, 0));
		pshufhw(xym4, xym4, _MM_SHUFFLE(2, 2, 0, 0));
		psrlw(xym4, 12);

		pshuflw(vf, xym3, _MM_SHUFFLE(2, 2, 0, 0));
		pshufhw(vf, vf, _MM_SHUFFLE(2, 2, 0, 0));
		psrlw(vf, 12);
		if (needsMoreRegs)
			movdqa(_rip_local(temp.vf), vf);
	}

	psrad(xym2, 16);
	psrad(xym3, 16);
	packssdw(xym2, xym3);

	if (m_sel.ltf)
	{

		pcmpeqd(xym1, xym1);
		psrlw(xym1, 15);
		THREEARG(paddw, xym3, xym2, xym1);

		WrapLOD(xym2, xym3);
	}
	else
	{

		WrapLOD(xym2);
	}

	SampleTexture_TexelReadHelper(0);

	if (m_sel.mmin != 1)
	{
		movdqa(_rip_local(temp.trb), xym5);
		movdqa(_rip_local(temp.tga), xym6);

		movdqa(xym2, _rip_local(temp.uv[0]));
		movdqa(xym3, _rip_local(temp.uv[1]));

		psrad(xym2, 1);
		psrad(xym3, 1);

		movdqa(xym5, _rip_local(temp.uv_minmax[0]));
		movdqa(xym6, _rip_local(temp.uv_minmax[1]));

		psrlw(xym5, 1);
		psrlw(xym6, 1);

		if (m_sel.ltf)
		{
			const XYm& vf = xym7;

			mov(eax, 0x8000);
			broadcastGPRToVec(xym4, eax);

			psubd(xym2, xym4);
			psubd(xym3, xym4);

			pshuflw(xym4, xym2, _MM_SHUFFLE(2, 2, 0, 0));
			pshufhw(xym4, xym4, _MM_SHUFFLE(2, 2, 0, 0));
			psrlw(xym4, 12);

			pshuflw(vf, xym3, _MM_SHUFFLE(2, 2, 0, 0));
			pshufhw(vf, vf, _MM_SHUFFLE(2, 2, 0, 0));
			psrlw(vf, 12);
			if (needsMoreRegs)
				movdqa(_rip_local(temp.vf), vf);
		}

		psrad(xym2, 16);
		psrad(xym3, 16);
		packssdw(xym2, xym3);

		if (m_sel.ltf)
		{

			pcmpeqd(xym1, xym1);
			psrlw(xym1, 15);
			THREEARG(paddw, xym3, xym2, xym1);

			WrapLOD(xym2, xym3);
		}
		else
		{

			WrapLOD(xym2);
		}

		SampleTexture_TexelReadHelper(1);

		movdqa(xym0, m_sel.lcm ? _rip_global(lod.f) : _rip_local(temp.lod.f));
		psrlw(xym0, 1);

		movdqa(xym2, _rip_local(temp.trb));
		movdqa(xym3, _rip_local(temp.tga));

		lerp16(xym5, xym2, xym0, 0);
		lerp16(xym6, xym3, xym0, 0);
	}
}

void GSDrawScanlineCodeGenerator::WrapLOD(const XYm& uv)
{
	const XYm& mask = xym0;
	const XYm& tmp = xym1;
	const XYm& min = xym5;
	const XYm& max = xym6;

	int wms_clamp = ((m_sel.wms + 1) >> 1) & 1;
	int wmt_clamp = ((m_sel.wmt + 1) >> 1) & 1;

	int region = ((m_sel.wms | m_sel.wmt) >> 1) & 1;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				pmaxsw(uv, min);
			}
			else
			{
				pxor(tmp, tmp);
				pmaxsw(uv, tmp);
			}

			pminsw(uv, max);
		}
		else
		{
			pand(uv, min);

			if (region)
			{
				por(uv, max);
			}
		}
	}
	else
	{
		broadcasti128(mask, _rip_global(t.mask));

		THREEARG(pand, tmp, uv, min);
		if (region)
			por(tmp, max);
		pmaxsw(uv, min);
		pminsw(uv, max);
		blend8(uv, tmp );
	}
}

void GSDrawScanlineCodeGenerator::WrapLOD(const XYm& uv0, const XYm& uv1)
{
	const XYm& mask = xym0;
	const XYm& tmp = xym1;
	const XYm& min = xym5;
	const XYm& max = xym6;

	int wms_clamp = ((m_sel.wms + 1) >> 1) & 1;
	int wmt_clamp = ((m_sel.wmt + 1) >> 1) & 1;

	int region = ((m_sel.wms | m_sel.wmt) >> 1) & 1;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				pmaxsw(uv0, min);
				pmaxsw(uv1, min);
			}
			else
			{
				pxor(tmp, tmp);
				pmaxsw(uv0, tmp);
				pmaxsw(uv1, tmp);
			}

			pminsw(uv0, max);
			pminsw(uv1, max);
		}
		else
		{
			pand(uv0, min);
			pand(uv1, min);

			if (region)
			{
				por(uv0, max);
				por(uv1, max);
			}
		}
	}
	else
	{
		broadcasti128(mask, _rip_global(t.mask));

		for (const XYm& uv : {uv0, uv1})
		{
			THREEARG(pand, tmp, uv, min);
			if (region)
				por(tmp, max);
			pmaxsw(uv, min);
			pminsw(uv, max);
			pblendvb(uv, tmp );
		}
	}
}

void GSDrawScanlineCodeGenerator::AlphaTFX()
{
	if (!m_sel.fb)
	{
		return;
	}

	const XYm& f_ga  = _f_ga;
	const XYm& tmpga = xym1;
	const XYm& tmp   = xym0;

	switch (m_sel.tfx)
	{
		case TFX_MODULATE:

			modulate16(_ga, f_ga, 1);

			clamp16(_ga, tmp);

			if (!m_sel.tcc)
			{
				MOVE_IF_64(psrlw, tmpga, f_ga, 7);

				mix16(_ga, tmpga, tmp);
			}

			break;

		case TFX_DECAL:

			if (!m_sel.tcc)
			{

				MOVE_IF_64(psrlw, tmpga, f_ga, 7);

				mix16(_ga, tmpga, tmp);
			}

			break;

		case TFX_HIGHLIGHT:

			MOVE_IF_64(psrlw, tmpga, f_ga, 7);

			if (m_sel.tcc)
			{
				paddusb(tmpga, _ga);
			}

			mix16(_ga, tmpga, tmp);

			break;

		case TFX_HIGHLIGHT2:

			if (!m_sel.tcc)
			{

				MOVE_IF_64(psrlw, tmpga, f_ga, 7);

				mix16(_ga, tmpga, tmp);
			}

			break;

		case TFX_NONE:

			if (m_sel.iip)
			{
				MOVE_IF_64(psrlw, _ga, f_ga, 7);
			}

			break;
	}

	if (m_sel.aa1)
	{

		if (!m_sel.abe)
		{

			if (m_sel.edge)
			{
				movdqa(xym0, _rip_local(temp.cov));
			}
			else
			{
				pcmpeqd(xym0, xym0);
				psllw(xym0, 15);
				psrlw(xym0, 8);
			}

			mix16(_ga, xym0, xym1);
		}
		else
		{

			pcmpeqd(xym0, xym0);
			psllw(xym0, 15);
			psrlw(xym0, 8);

			if (m_sel.edge)
			{
				movdqa(xym1, _rip_local(temp.cov));
			}
			else
			{
				movdqa(xym1, xym0);
			}

			pcmpeqw(xym0, _ga);
			psrld(xym0, 16);
			pslld(xym0, 16);

			blend8(_ga, xym1 );
		}
	}
}

void GSDrawScanlineCodeGenerator::ReadMask()
{
	if (m_sel.fwrite)
	{
		pbroadcastdLocal(_fm, _rip_global(fm));
	}

	if (m_sel.zwrite)
	{
		pbroadcastdLocal(_zm, _rip_global(zm));
	}
}

void GSDrawScanlineCodeGenerator::TestAlpha()
{
	switch (m_sel.atst)
	{
		case ATST_NEVER:
			pcmpeqd(xym1, xym1);
			break;

		case ATST_ALWAYS:
			return;

		case ATST_LESS:
		case ATST_LEQUAL:
			THREEARG(psrld, xym1, _ga, 16);
			BROADCAST_AND_OP(vbroadcasti128, pcmpgtd, xym1, xym0, _rip_global(aref));
			break;

		case ATST_EQUAL:
			THREEARG(psrld, xym1, _ga, 16);
			BROADCAST_AND_OP(vbroadcasti128, pcmpeqd, xym1, xym0, _rip_global(aref));
			pcmpeqd(xym0, xym0);
			pxor(xym1, xym0);
			break;

		case ATST_GEQUAL:
		case ATST_GREATER:
			THREEARG(psrld, xym0, _ga, 16);
			broadcasti128(xym1, _rip_global(aref));
			pcmpgtd(xym1, xym0);
			break;

		case ATST_NOTEQUAL:
			THREEARG(psrld, xym1, _ga, 16);
			BROADCAST_AND_OP(vbroadcasti128, pcmpeqd, xym1, xym0, _rip_global(aref));
			break;
	}

	switch (m_sel.afail)
	{
		case AFAIL_KEEP:
			por(_test, xym1);
			alltrue(_test);
			break;

		case AFAIL_FB_ONLY:
			por(_zm, xym1);
			break;

		case AFAIL_ZB_ONLY:
			por(_fm, xym1);
			break;

		case AFAIL_RGB_ONLY:
			por(_zm, xym1);
			psrld(xym1, 24);
			pslld(xym1, 24);
			por(_fm, xym1);
			break;
	}
}

void GSDrawScanlineCodeGenerator::ColorTFX()
{
	if (!m_sel.fwrite)
	{
		return;
	}

	const XYm& f_ga  = _f_ga;
	const XYm& tmpga = xym2;

	auto modulate16_1_rb = [this]
	{
		modulate16(_rb, _f_rb, 1);
	};

	switch (m_sel.tfx)
	{
		case TFX_MODULATE:

			modulate16_1_rb();

			clamp16(_rb, xym0);

			break;

		case TFX_DECAL:

			break;

		case TFX_HIGHLIGHT:
		case TFX_HIGHLIGHT2:

			movdqa(xym1, _ga);

			modulate16(_ga, f_ga, 1);

			pshuflw(tmpga, f_ga, _MM_SHUFFLE(3, 3, 1, 1));
			pshufhw(tmpga, tmpga, _MM_SHUFFLE(3, 3, 1, 1));
			psrlw(tmpga, 7);

			paddw(_ga, tmpga);

			clamp16(_ga, xym0);

			mix16(_ga, xym1, xym0);

			modulate16_1_rb();

			paddw(_rb, tmpga);

			clamp16(_rb, xym0);

			break;

		case TFX_NONE:

			if (m_sel.iip)
			{
				MOVE_IF_64(psrlw, _rb, _f_rb, 7);
			}

			break;
	}
}

void GSDrawScanlineCodeGenerator::Fog()
{
	if (!m_sel.fwrite || !m_sel.fge)
	{
		return;
	}

	const XYm& f   = _f;
	const XYm& tmp = xym0;

	movdqa(xym1, _ga);

	pbroadcastdLocal(tmp, _rip_global(frb));
	lerp16(_rb, tmp, f, 0);

	pbroadcastdLocal(tmp, _rip_global(fga));
	lerp16(_ga, tmp, f, 0);

	mix16(_ga, xym1, xym0);
}

void GSDrawScanlineCodeGenerator::ReadFrame()
{
	if (!m_sel.fb)
	{
		return;
	}

	mov(ebx, dword[t1]);
	add(ebx, dword[t0]);
	and_(ebx, HALF_VM_SIZE - 1);

	if (!m_sel.rfb)
	{
		return;
	}

	ReadPixel(_fd, xym0, rbx);
}

void GSDrawScanlineCodeGenerator::TestDestAlpha()
{
	if (!m_sel.date || (m_sel.fpsm != 0 && m_sel.fpsm != 2))
	{
		return;
	}

	if (m_sel.datm)
	{
		if (m_sel.fpsm == 2)
		{
			pxor(xym0, xym0);
			THREEARG(pslld, xym1, _fd, 16);
			psrad(xym1, 31);
			pcmpeqd(xym1, xym0);
		}
		else
		{
			pcmpeqd(xym0, xym0);
			THREEARG(pxor, xym1, _fd, xym0);
			psrad(xym1, 31);
		}
	}
	else
	{
		if (m_sel.fpsm == 2)
		{
			THREEARG(pslld, xym1, _fd, 16);
			psrad(xym1, 31);
		}
		else
		{
			THREEARG(psrad, xym1, _fd, 31);
		}
	}

	por(_test, xym1);

	alltrue(_test);
}

void GSDrawScanlineCodeGenerator::WriteMask()
{
	if (m_sel.notest)
	{
		return;
	}

	if (m_sel.fwrite)
	{
		por(_fm, _test);
	}

	if (m_sel.zwrite)
	{
		por(_zm, _test);
	}

	pcmpeqd(xym1, xym1);

	if (m_sel.fwrite && m_sel.zwrite)
	{
		THREEARG(pcmpeqd, xym0, xym1, _zm);
		pcmpeqd(xym1, _fm);
		packssdw(xym1, xym0);
	}
	else if (m_sel.fwrite)
	{
		pcmpeqd(xym1, _fm);
		packssdw(xym1, xym1);
	}
	else if (m_sel.zwrite)
	{
		pcmpeqd(xym1, _zm);
		packssdw(xym1, xym1);
	}

	pmovmskb(edx, xym1);

	not_(edx);
}

void GSDrawScanlineCodeGenerator::WriteZBuf()
{
	if (!m_sel.zwrite)
	{
		return;
	}

	if (m_sel.prim != GS_SPRITE_CLASS)
		movdqa(xym1, _rip_local(temp.zs));
	else
		pbroadcastdLocal(xym1, _rip_local(p.z));

	if (m_sel.ztest && m_sel.zpsm < 2)
	{

		if (hasAVX)
		{
			vpblendvb(xym1, xym1, _rip_local(temp.zd), _zm);
		}
		else
		{
			movdqa(xym0, _zm);
			movdqa(xym7, _rip_local(temp.zd));
			blend8(xym1, xym7 );
		}
	}

	bool fast = m_sel.ztest ? m_sel.zpsm < 2 : m_sel.zpsm == 0 && m_sel.notest;

#if USING_XMM
	WritePixel(xym1, t2, dh, fast, m_sel.zpsm, 1);
#else
	WritePixel(xym1, t2, edx, fast, m_sel.zpsm, 1);
#endif
}

void GSDrawScanlineCodeGenerator::AlphaBlend()
{
	if (!m_sel.fwrite)
	{
		return;
	}

	if (m_sel.abe == 0 && m_sel.aa1 == 0)
	{
		return;
	}

	const XYm& _dst_rb = xym0;
	const XYm& _dst_ga = xym1;
	const XYm& tmp1 = _test;
	const XYm& tmp2 = xym4;

	if (((m_sel.aba != m_sel.abb) && (m_sel.aba == 1 || m_sel.abb == 1 || m_sel.abc == 1)) || m_sel.abd == 1)
	{
		switch (m_sel.fpsm)
		{
			case 0:
			case 1:

				split16_2x8(_dst_rb, _dst_ga, _fd);

				break;

			case 2:

				pcmpeqd(tmp1, tmp1);

				psrld(tmp1, 27);
				THREEARG(pand, _dst_rb, _fd, tmp1);
				pslld(_dst_rb, 3);

				pslld(tmp1, 10);
				THREEARG(pand, tmp2, _fd, tmp1);
				pslld(tmp2, 9);

				por(_dst_rb, tmp2);

				psrld(tmp1, 5);
				THREEARG(pand, _dst_ga, _fd, tmp1);
				psrld(_dst_ga, 2);

				psllw(tmp1, 10);
				THREEARG(pand, tmp2, _fd, tmp1);
				pslld(tmp2, 8);

				por(_dst_ga, tmp2);

				break;
		}
	}

	if (m_sel.pabe || ((m_sel.aba != m_sel.abb) && (m_sel.abb == 0 || m_sel.abd == 0)))
	{
		movdqa(tmp2, _rb);
	}

	if (m_sel.aba != m_sel.abb)
	{

		switch (m_sel.aba)
		{
			case 0:
				break;
			case 1:
				movdqa(_rb, _dst_rb);
				break;
			case 2:
				pxor(_rb, _rb);
				break;
		}

		switch (m_sel.abb)
		{
			case 0:
				psubw(_rb, tmp2);
				break;
			case 1:
				psubw(_rb, _dst_rb);
				break;
			case 2:
				break;
		}

		if (!(m_sel.fpsm == 1 && m_sel.abc == 1))
		{

			switch (m_sel.abc)
			{
				case 0:
				case 1:
					pshuflw(tmp1, m_sel.abc ? _dst_ga : _ga, _MM_SHUFFLE(3, 3, 1, 1));
					pshufhw(tmp1, tmp1, _MM_SHUFFLE(3, 3, 1, 1));
					psllw(tmp1, 7);
					break;
				case 2:
					pbroadcastwLocal(tmp1, _rip_global(afix));
					break;
			}

			modulate16(_rb, tmp1, 1);
		}

		switch (m_sel.abd)
		{
			case 0:
				paddw(_rb, tmp2);
				break;
			case 1:
				paddw(_rb, _dst_rb);
				break;
			case 2:
				break;
		}
	}
	else
	{

		switch (m_sel.abd)
		{
			case 0:
				break;
			case 1:
				movdqa(_rb, _dst_rb);
				break;
			case 2:
				pxor(_rb, _rb);
				break;
		}
	}

	if (m_sel.pabe)
	{

		THREEARG(pslld, xym0, _ga, 8);
		psrad(xym0, 31);

		blend8r(_rb, tmp2 );
	}

	movdqa(tmp2, _ga);

	if (m_sel.aba != m_sel.abb)
	{

		switch (m_sel.aba)
		{
			case 0:
				break;
			case 1:
				movdqa(_ga, _dst_ga);
				break;
			case 2:
				pxor(_ga, _ga);
				break;
		}

		switch (m_sel.abb)
		{
			case 0:
				psubw(_ga, tmp2);
				break;
			case 1:
				psubw(_ga, _dst_ga);
				break;
			case 2:
				break;
		}

		if (!(m_sel.fpsm == 1 && m_sel.abc == 1))
		{

			modulate16(_ga, tmp1, 1);
		}

		switch (m_sel.abd)
		{
			case 0:
				paddw(_ga, tmp2);
				break;
			case 1:
				paddw(_ga, _dst_ga);
				break;
			case 2:
				break;
		}
	}
	else
	{

		switch (m_sel.abd)
		{
			case 0:
				break;
			case 1:
				movdqa(_ga, _dst_ga);
				break;
			case 2:
				pxor(_ga, _ga);
				break;
		}
	}

	if (m_sel.pabe)
	{
		psrld(xym0, 16);

		blend8r(_ga, tmp2 );
	}
	else
	{
		if (m_sel.fpsm != 1)
		{
			mix16(_ga, tmp2, tmp1);
		}
	}
}

void GSDrawScanlineCodeGenerator::WriteFrame()
{
	if (!m_sel.fwrite)
	{
		return;
	}


	const XYm& tmp = xym15;

	if (m_sel.fpsm == 2 && m_sel.dthe)
	{

		mov(eax, ptr[rsp + _top]);
		and_(eax, 3);
		shl(eax, 5);

		add(rax, _rip_global(dimx));

		BROADCAST_AND_OP(vbroadcasti128, paddw, xym5, tmp, ptr[rax + sizeof(GSVector4i) * 0]);
		BROADCAST_AND_OP(vbroadcasti128, paddw, xym6, tmp, ptr[rax + sizeof(GSVector4i) * 1]);
	}

	if (m_sel.colclamp == 0)
	{

		pcmpeqd(tmp, tmp);
		psrlw(tmp, 8);
		pand(xym5, tmp);
		pand(xym6, tmp);
	}

	THREEARG(punpckhwd, tmp, xym5, xym6);
	punpcklwd(xym5, xym6);
	packuswb(xym5, tmp);

	if (m_sel.fba && m_sel.fpsm != 1)
	{

		pcmpeqd(tmp, tmp);
		pslld(tmp, 31);
		por(xym5, tmp);
	}

	if (m_sel.fpsm == 2)
	{

		mov(eax, 0x00f800f8);
		broadcastGPRToVec(xym0, eax);

		mov(eax, 0x8000f800);
		broadcastGPRToVec(xym1, eax);

		pand(xym0, xym5);
		pand(xym1, xym5);

		THREEARG(psrld, xym5, xym0, 9);
		psrld(xym0, 3);
		THREEARG(psrld, xym6, xym1, 16);
		psrld(xym1, 6);

		por(xym0, xym1);
		por(xym5, xym6);
		por(xym5, xym0);
	}

	if (m_sel.rfb)
	{

		blend(xym5, _fd, _fm);
	}

	bool fast = m_sel.rfb ? m_sel.fpsm < 2 : m_sel.fpsm == 0 && m_sel.notest;

#if USING_XMM
	WritePixel(xym5, rbx, dl, fast, m_sel.fpsm, 0);
#else
	WritePixel(xym5, rbx, edx, fast, m_sel.fpsm, 0);
#endif
}

void GSDrawScanlineCodeGenerator::ReadPixel(const XYm& dst, const XYm& tmp, const AddressReg& addr)
{
	RegExp base = _m_local__gd__vm + addr * 2;
#if USING_XMM
	movq(dst, qword[base]);
	movhps(dst, qword[base + 8 * 2]);
#else
	Xmm dstXmm = Xmm(dst.getIdx());
	Xmm tmpXmm = Xmm(tmp.getIdx());
	movq(dstXmm, qword[base]);
	movhps(dstXmm, qword[base + 8 * 2]);
	movq(tmpXmm, qword[base + 16 * 2]);
	movhps(tmpXmm, qword[base + 24 * 2]);
	vinserti128(dst, dst, tmpXmm, 1);
#endif
}

#if USING_XMM
void GSDrawScanlineCodeGenerator::WritePixel(const XYm& src_, const AddressReg& addr, const Reg8& mask, bool fast, int psm, int fz)
#else
void GSDrawScanlineCodeGenerator::WritePixel(const XYm& src_, const AddressReg& addr, const Reg32& mask, bool fast, int psm, int fz)
#endif
{
#if USING_XMM
	const Xmm& src = src_;
	int shift = 0;
#else
	Xmm src = Xmm(src_.getIdx());
	int shift = fz * 8;
#endif
	RegExp base = _m_local__gd__vm + addr * 2;

	if (m_sel.notest)
	{
		if (fast)
		{
			movq(qword[base], src);
			movhps(qword[base + 8 * 2], src);
#if USING_YMM
			vextracti128(src, src_, 1);
			movq(qword[base + 16 * 2], src);
			movhps(qword[base + 24 * 2], src);
#endif
		}
		else
		{
			WritePixel(src, addr, 0, 0, psm);
			WritePixel(src, addr, 1, 1, psm);
			WritePixel(src, addr, 2, 2, psm);
			WritePixel(src, addr, 3, 3, psm);
#if USING_YMM
			vextracti128(src, src_, 1);
			WritePixel(src, addr, 4, 0, psm);
			WritePixel(src, addr, 5, 1, psm);
			WritePixel(src, addr, 6, 2, psm);
			WritePixel(src, addr, 7, 3, psm);
#endif
		}
	}
	else
	{
		if (fast)
		{

			test(mask, 0x0000000f << shift);
			je("@f");
			movq(qword[base], src);
			L("@@");

			test(mask, 0x000000f0 << shift);
			je("@f");
			movhps(qword[base + 8 * 2], src);
			L("@@");

#if USING_YMM
			vextracti128(src, src_, 1);

			test(mask, 0x000f0000 << shift);
			je("@f");
			movq(qword[base + 16 * 2], src);
			L("@@");

			test(mask, 0x00f00000 << shift);
			je("@f");
			movhps(qword[base + 24 * 2], src);
			L("@@");
#endif
		}
		else
		{

			test(mask, 0x00000003 << shift);
			je("@f");
			WritePixel(src, addr, 0, 0, psm);
			L("@@");

			test(mask, 0x0000000c << shift);
			je("@f");
			WritePixel(src, addr, 1, 1, psm);
			L("@@");

			test(mask, 0x00000030 << shift);
			je("@f");
			WritePixel(src, addr, 2, 2, psm);
			L("@@");

			test(mask, 0x000000c0 << shift);
			je("@f");
			WritePixel(src, addr, 3, 3, psm);
			L("@@");

#if USING_YMM
			vextracti128(src, src_, 1);

			test(mask, 0x00030000 << shift);
			je("@f");
			WritePixel(src, addr, 4, 0, psm);
			L("@@");

			test(mask, 0x000c0000 << shift);
			je("@f");
			WritePixel(src, addr, 5, 1, psm);
			L("@@");

			test(mask, 0x00300000 << shift);
			je("@f");
			WritePixel(src, addr, 6, 2, psm);
			L("@@");

			test(mask, 0x00c00000 << shift);
			je("@f");
			WritePixel(src, addr, 7, 3, psm);
			L("@@");
#endif
		}
	}
}

void GSDrawScanlineCodeGenerator::WritePixel(const Xmm& src, const AddressReg& addr, u8 i, u8 j, int psm)
{
	constexpr int s_offsets[8] = {0, 2, 8, 10, 16, 18, 24, 26};

	Address dst = ptr[_m_local__gd__vm + addr * 2 + s_offsets[i] * 2];

	switch (psm)
	{
		case 0:
			if (j == 0)
				movd(dst, src);
			else
				pextrd(dst, src, j);
			break;
		case 1:
			if (j == 0)
				movd(eax, src);
			else
				pextrd(eax, src, j);
			xor_(eax, dst);
			and_(eax, 0xffffff);
			xor_(dst, eax);
			break;
		case 2:
			if (j == 0)
				movd(eax, src);
			else
				pextrw(eax, src, j * 2);
			mov(dst, ax);
			break;
	}
}

void GSDrawScanlineCodeGenerator::ReadTexel1(const XYm& dst, const XYm& src, const XYm& tmp1, const XYm& tmp2, int mip_offset)
{
	const XYm no(-1);
	ReadTexelImpl(dst, tmp1, src, no, no, no, tmp2, no, 1, mip_offset);
}

void GSDrawScanlineCodeGenerator::ReadTexel4(
	const XYm& d0,   const XYm& d1,
	const XYm& d2s0, const XYm& d3s1,
	const XYm& s2,   const XYm& s3,
	const XYm& tmp1, const XYm& tmp2,
	int mip_offset)
{
	ReadTexelImpl(d0, d1, d2s0, d3s1, s2, s3, tmp1, tmp2, 4, mip_offset);
}

void GSDrawScanlineCodeGenerator::ReadTexelImpl(
	const XYm& d0,   const XYm& d1,
	const XYm& d2s0, const XYm& d3s1,
	const XYm& s2,   const XYm& s3,
	const XYm& tmp1, const XYm& tmp2,
	int pixels,      int mip_offset)
{
	mip_offset *= wordsize;
#if USING_XMM
	ReadTexelImplSSE4(d0, d1, d2s0, d3s1, s2, s3, pixels, mip_offset);
#else
	ReadTexelImplYmm(d0, d1, d2s0, d3s1, s2, s3, tmp1, pixels, mip_offset);
#endif
}

void GSDrawScanlineCodeGenerator::ReadTexelImplLoadTexLOD(int lod, int mip_offset)
{
	AddressReg texIn = _m_local__gd__tex;
	Address lod_addr = m_sel.lcm ? _rip_global_offset(lod.i.U32[0], sizeof(u32) * lod) : _rip_local_offset(temp.lod.i.U32[0], sizeof(u32) * lod);
	mov(ebx, lod_addr);
	mov(rbx, ptr[texIn + rbx * wordsize + mip_offset]);
}

void GSDrawScanlineCodeGenerator::ReadTexelImplYmm(
	const Ymm& d0,   const Ymm& d1,
	const Ymm& d2s0, const Ymm& d3s1,
	const Ymm& s2,   const Ymm& s3,
	const Ymm& tmp,
	int pixels,      int mip_offset)
{
	const Ymm dst[] = { d0,   d1,   d2s0, d3s1 };
	const Ymm src[] = { d2s0, d3s1,   s2,   s3 };
	const Ymm t1[]  = { d1,   d2s0, d3s1,   s2 };
	const Ymm t2[]  = { tmp,  tmp,  tmp,  tmp  };

	bool texInRBX = false;
	if (use_lod && m_sel.lcm)
	{
		ReadTexelImplLoadTexLOD(0, mip_offset);
		texInRBX = true;
	}

	for (int i = 0; i < pixels; i++)
	{
		const Xmm xdst{dst[i].getIdx()};
		const Xmm xsrc{src[i].getIdx()};
		const Xmm xt1{t1[i].getIdx()};
		const Xmm xt2{t2[i].getIdx()};

		if (use_lod && !m_sel.lcm)
		{
			texInRBX = true;

			vextracti128(xt1, src[i], 1);

			for (int j = 0; j < 4; j++)
			{
				ReadTexelImplLoadTexLOD(j, mip_offset);

				ReadTexelImpl(xdst, xsrc, j, texInRBX, false);

				ReadTexelImplLoadTexLOD(j + 4, mip_offset);

				ReadTexelImpl(xt2, xt1, j, texInRBX, false);
			}

			vinserti128(dst[i], dst[i], xt2, 1);
		}
		else
		{
			AddressReg tex = texInRBX ? rbx : _m_local__gd__tex;
			if (!m_sel.tlu)
			{
				pcmpeqd(t1[i], t1[i]);
				vpgatherdd(dst[i], ptr[tex + src[i] * 4], t1[i]);
			}
			else
			{
				vextracti128(xt1, src[i], 1);

				for (int j = 0; j < 4; j++)
				{
					ReadTexelImpl(xdst, xsrc, j, texInRBX, false);
					ReadTexelImpl(xt2, xt1, j, texInRBX, false);
				}

				vinserti128(dst[i], dst[i], xt2, 1);

			}
		}
	}
}

void GSDrawScanlineCodeGenerator::ReadTexelImplSSE4(
	const Xmm& d0,   const Xmm& d1,
	const Xmm& d2s0, const Xmm& d3s1,
	const Xmm& s2,   const Xmm& s3,
	int pixels,      int mip_offset)
{
	const bool preserve[] = { false, false, true, true };
	const Xmm dst[]       = { d0,    d1,    d2s0, d3s1 };
	const Xmm src[]       = { d2s0,  d3s1,    s2,   s3 };

	if (use_lod && !m_sel.lcm)
	{
		bool texInRBX = true;
		for (int j = 0; j < 4; j++)
		{
			ReadTexelImplLoadTexLOD(j, mip_offset);

			for (int i = 0; i < pixels; i++)
			{
				ReadTexelImpl(dst[i], src[i], j, texInRBX, preserve[i]);
			}
		}
	}
	else
	{
		bool preserve = false;
		bool texInRBX = false;

		if (use_lod && m_sel.lcm)
		{
			ReadTexelImplLoadTexLOD(0, mip_offset);
			texInRBX = true;
		}

		for (int i = 0; i < pixels; i++)
		{
			for (int j = 0; j < 4; j++)
			{
				ReadTexelImpl(dst[i], src[i], j, texInRBX, preserve);
			}
		}
	}
}

void GSDrawScanlineCodeGenerator::ReadTexelImpl(const Xmm& dst, const Xmm& addr, u8 i, bool texInRBX, bool preserveDst)
{
	pxAssert(i < 4);

	AddressReg clut = _m_local__gd__clut;
	AddressReg tex = texInRBX ? rbx : _m_local__gd__tex;
	Address src = m_sel.tlu ? ptr[clut + rax * 4] : ptr[tex + rax * 4];

	if (i == 0)
		movd(eax, addr);
	else
		pextrd(eax, addr, i);

	if (m_sel.tlu)
		movzx(eax, byte[tex + rax]);

	if (i == 0 && !preserveDst)
		movd(dst, src);
	else
		pinsrd(dst, src, i);
}
