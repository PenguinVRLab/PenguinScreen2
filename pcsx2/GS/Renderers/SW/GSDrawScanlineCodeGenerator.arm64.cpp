// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0

#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/GSState.h"

#include "common/StringUtil.h"
#include "common/Perf.h"

#include <cstdint>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

MULTI_ISA_UNSHARED_IMPL;

using namespace vixl::aarch64;

static const auto& _steps = w0;
static const auto& _left = w1;
static const auto& _skip = w1;
static const auto& _top = w2;
static const auto& _v = x3;
static const auto& _locals = x4;

static const auto& _globals = x10;
static const auto& _vm = x11;
static const auto& _vm_high = x12;
static const auto& _global_tex0 = x13;
static const auto& _global_clut = x14;

static const auto& _wscratch = w15;
static const auto& _xscratch = x15;
static const auto& _wscratch2 = w16;
static const auto& _xscratch2 = x16;
static const auto& _scratchaddr = x17;

static const auto& _global_dimx = x19;
static const auto& _local_aref = w20;
static const auto& _global_frb = w21;
static const auto& _global_fga = w22;
static const auto& _global_l = w23;
static const auto& _global_k = w24;
static const auto& _global_mxl = w25;

static const auto& _vscratch = v31;
static const auto& _vscratch2 = v30;
static const auto& _vscratch3 = v29;
static const auto& _temp_s = v28;
static const auto& _temp_t = v27;
static const auto& _temp_q = v26;
static const auto& _temp_z0 = v25;
static const auto& _temp_z1 = v24;
static const auto& _temp_rb = v23;
static const auto& _temp_ga = v22;
static const auto& _temp_zs = v21;
static const auto& _temp_zd = v20;
static const auto& _temp_vf = v19;
static const auto& _d4_z = v18;
static const auto& _d4_stq = v17;
static const auto& _d4_c = v16;
static const auto& _global_tmin = v15;
static const auto& _global_tmax = v14;
static const auto& _global_tmask = v13;
static const auto& _const_movemskw_mask = v12;
static const auto& _const_log2_coef = v11;
static const auto& _temp_f = v10;
static const auto& _d4_f = v9;

static const auto& _test = v8;
static const auto& _fd = v2;

#define OFFSETOF(base, field) (reinterpret_cast<uptr>(&reinterpret_cast<base*>(0)->field))
#define _local(field) MemOperand(_locals, OFFSETOF(GSScanlineLocalData, field))
#define _global(field) MemOperand(_globals, OFFSETOF(GSScanlineGlobalData, field))
#define armAsm (&m_emitter)

GSDrawScanlineCodeGenerator::GSDrawScanlineCodeGenerator(u64 key, void* code, size_t maxsize)
	: m_emitter(static_cast<vixl::byte*>(code), maxsize, vixl::aarch64::PositionDependentCode)
	, m_sel(key)
{
	m_emitter.GetScratchRegisterList()->Remove(_xscratch.GetCode());
	m_emitter.GetScratchRegisterList()->Remove(_xscratch2.GetCode());
}

void GSDrawScanlineCodeGenerator::Generate()
{
	if (m_sel.breakpoint)
		armAsm->Brk(1);

	if (GSDrawScanline::ShouldUseCDrawScanline(m_sel.key))
	{
		armAsm->Mov(vixl::aarch64::x15, reinterpret_cast<uintptr_t>(
											static_cast<void (*)(int, int, int, const GSVertexSW&, GSScanlineLocalData&)>(
												&GSDrawScanline::CDrawScanline)));
		armAsm->Br(vixl::aarch64::x15);
		armAsm->FinalizeCode();
		return;
	}

	armAsm->Sub(sp, sp, 128);
	armAsm->Stp(x19, x20, MemOperand(sp, 0));
	armAsm->Stp(x21, x22, MemOperand(sp, 16));
	armAsm->Stp(x23, x24, MemOperand(sp, 32));
	armAsm->Stp(x25, x26, MemOperand(sp, 48));
	armAsm->Stp(d8, d9, MemOperand(sp, 64));
	armAsm->Stp(d10, d11, MemOperand(sp, 80));
	armAsm->Stp(d12, d13, MemOperand(sp, 96));
	armAsm->Stp(d14, d15, MemOperand(sp, 112));

	armAsm->Ldr(_globals, _local(gd));
	armAsm->Ldr(_vm, _global(vm));
	armAsm->Add(_vm_high, _vm, 8 * 2);

	Init();

	Label loop;
	armAsm->Bind(&loop);

	bool tme = m_sel.tfx != TFX_NONE;

	TestZ(tme ? v5 : v2, tme ? v6 : v3);

	if (m_sel.mmin)
		SampleTextureLOD();
	else
		SampleTexture();

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

	Label exit;
	armAsm->Bind(&m_step_label);

	if (!m_sel.edge)
	{
		armAsm->Cmp(_steps, 0);
		armAsm->B(le, &exit);

		Step();

		armAsm->B(&loop);
	}

	armAsm->Bind(&exit);

	armAsm->Ldp(d14, d15, MemOperand(sp, 112));
	armAsm->Ldp(d12, d13, MemOperand(sp, 96));
	armAsm->Ldp(d10, d11, MemOperand(sp, 80));
	armAsm->Ldp(d8, d9, MemOperand(sp, 64));
	armAsm->Ldp(x25, x26, MemOperand(sp, 48));
	armAsm->Ldp(x23, x24, MemOperand(sp, 32));
	armAsm->Ldp(x21, x22, MemOperand(sp, 16));
	armAsm->Ldp(x19, x20, MemOperand(sp, 0));
	armAsm->Add(sp, sp, 128);

	armAsm->Ret();

	armAsm->FinalizeCode();

	Perf::any.RegisterKey(GetCode(), GetSize(), "GSDrawScanline_", m_sel.key);
}

void GSDrawScanlineCodeGenerator::Init()
{
	if (!m_sel.notest)
	{

		armAsm->Mov(w6, _left);
		armAsm->And(_left, _left, 3);

		armAsm->Add(_steps, _steps, _left);
		armAsm->Sub(_steps, _steps, 4);

		armAsm->Sub(w6, w6, _left);

		armAsm->Lsl(_left, _left, 4);

		armAsm->Add(_scratchaddr, _globals, offsetof(GSScanlineGlobalData, const_test_128b[0]));
		armAsm->Ldr(_test, MemOperand(_scratchaddr, x1));

		armAsm->Asr(w5, _steps, 31);
		armAsm->And(w5, w5, _steps);
		armAsm->Lsl(w5, w5, 4);

		armAsm->Add(_scratchaddr, _globals, offsetof(GSScanlineGlobalData, const_test_128b[7]));
		armAsm->Ldr(_vscratch, MemOperand(_scratchaddr, w5, SXTW));
		armAsm->Orr(_test.V16B(), _test.V16B(), _vscratch.V16B());
	}
	else
	{
		armAsm->Mov(w6, _left);
		armAsm->Mov(_skip, wzr);
		armAsm->Sub(_steps, _steps, 4);
	}

	armAsm->Ldr(_scratchaddr, _global(fzbr));
	armAsm->Lsl(w7, _top, 3);
	armAsm->Add(x7, _scratchaddr, x7);

	armAsm->Ldr(_scratchaddr, _global(fzbc));
	armAsm->Lsl(w8, w6, 1);
	armAsm->Add(x8, _scratchaddr, x8);

	if ((m_sel.prim != GS_SPRITE_CLASS && ((m_sel.fwrite && m_sel.fge) || m_sel.zb)) || (m_sel.fb && (m_sel.edge || m_sel.tfx != TFX_NONE || m_sel.iip)))
	{

		armAsm->Lsl(w1, w1, 3);
		armAsm->Add(x1, x1, _locals);
		static_assert(offsetof(GSScanlineLocalData, d) == 0);
	}

	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		if ((m_sel.fwrite && m_sel.fge) || m_sel.zb)
		{
			armAsm->Ldr(_d4_z, _local(d4.z));

			if (m_sel.fwrite && m_sel.fge)
			{
				armAsm->Ldr(_temp_f.S(), MemOperand(_v, offsetof(GSVertexSW, t.w)));
				armAsm->Ldr(_vscratch, MemOperand(x1, offsetof(GSScanlineLocalData::skip, f)));
				armAsm->Ldr(_d4_f, _local(d4.f));

				armAsm->Fcvtzs(_temp_f.S(), _temp_f.S());
				armAsm->Dup(_temp_f.V8H(), _temp_f.V8H(), 0);
				armAsm->Add(_temp_f.V8H(), _temp_f.V8H(), _vscratch.V8H());
			}

			if (m_sel.zb && m_sel.zequal)
			{
				armAsm->Ldr(_temp_z0.D(), MemOperand(_v, offsetof(GSVertexSW, p.z)));
				armAsm->Fcvtzs(_temp_z0.D(), _temp_z0.D());
				armAsm->Dup(_temp_z0.V4S(), _temp_z0.V4S(), 0);
			}
			else if (m_sel.zb)
			{
				armAsm->Add(_scratchaddr, _v, offsetof(GSVertexSW, p.z));
				armAsm->Ldr(_vscratch, MemOperand(x1, offsetof(GSScanlineLocalData::skip, z)));
				armAsm->Ld1r(_vscratch2.V2D(), MemOperand(_scratchaddr));

				armAsm->Fcvtl(_temp_z0.V2D(), _vscratch.V2S());
				armAsm->Fadd(_temp_z0.V2D(), _temp_z0.V2D(), _vscratch2.V2D());

				armAsm->Fcvtl2(_temp_z1.V2D(), _vscratch.V4S());
				armAsm->Fadd(_temp_z1.V2D(), _temp_z1.V2D(), _vscratch2.V2D());
			}
		}
	}
	else
	{
		if (m_sel.ztest || m_sel.zwrite)
		{
			armAsm->Ldr(_temp_z0, _local(p.z));
		}

		if (m_sel.fwrite && m_sel.fge)
		{
			armAsm->Ldr(_temp_f, _local(p.f));
		}
	}

	if (m_sel.fb)
	{
		if (m_sel.edge)
		{
			armAsm->Add(_scratchaddr, _v, offsetof(GSVertexSW, p.x));
			armAsm->Ld1r(v3.V8H(), MemOperand(_scratchaddr));
		}

		if (m_sel.tfx != TFX_NONE)
		{
			armAsm->Ldr(v4, MemOperand(_v, offsetof(GSVertexSW, t)));
		}

		if (m_sel.edge)
		{
			armAsm->Ushr(v3.V8H(), v3.V8H(), 9);
			armAsm->Str(v3, _local(temp.cov));
		}

		if (m_sel.tfx != TFX_NONE)
		{
			if (m_sel.fst)
			{

				armAsm->Fcvtzs(v6.V4S(), v4.V4S());

				armAsm->Dup(_temp_s.V4S(), v6.V4S(), 0);
				armAsm->Dup(_temp_t.V4S(), v6.V4S(), 1);

				armAsm->Ldr(_vscratch, MemOperand(x1, offsetof(GSScanlineLocalData::skip, s)));
				armAsm->Add(_temp_s.V4S(), _temp_s.V4S(), _vscratch.V4S());

				if (m_sel.prim != GS_SPRITE_CLASS || m_sel.mmin)
				{
					armAsm->Ldr(_vscratch, MemOperand(x1, offsetof(GSScanlineLocalData::skip, t)));
					armAsm->Add(_temp_t.V4S(), _temp_t.V4S(), _vscratch.V4S());
				}
				else
				{
					if (m_sel.ltf)
					{
						armAsm->Trn1(_temp_vf.V8H(), _temp_t.V8H(), _temp_t.V8H());
						armAsm->Ushr(_temp_vf.V8H(), _temp_vf.V8H(), 12);
					}
				}
			}
			else
			{

				armAsm->Ldr(_temp_s, MemOperand(x1, offsetof(GSScanlineLocalData::skip, s)));
				armAsm->Ldr(_temp_t, MemOperand(x1, offsetof(GSScanlineLocalData::skip, t)));
				armAsm->Ldr(_temp_q, MemOperand(x1, offsetof(GSScanlineLocalData::skip, q)));

				armAsm->Dup(v2.V4S(), v4.V4S(), 0);
				armAsm->Dup(v3.V4S(), v4.V4S(), 1);
				armAsm->Dup(v4.V4S(), v4.V4S(), 2);

				armAsm->Fadd(_temp_s.V4S(), v2.V4S(), _temp_s.V4S());
				armAsm->Fadd(_temp_t.V4S(), v3.V4S(), _temp_t.V4S());
				armAsm->Fadd(_temp_q.V4S(), v4.V4S(), _temp_q.V4S());
			}

			armAsm->Ldr(_d4_stq, _local(d4.stq));
			armAsm->Ldr(_global_tmin, _global(t.min));
			armAsm->Ldr(_global_tmax, _global(t.max));
			armAsm->Ldr(_global_tmask, _global(t.mask));

			if (!m_sel.mmin)
				armAsm->Ldr(_global_tex0, _global(tex[0]));
			else
				armAsm->Add(_global_tex0, _globals, offsetof(GSScanlineGlobalData, tex));

			if (m_sel.tlu)
				armAsm->Ldr(_global_clut, _global(clut));
		}

		if (!(m_sel.tfx == TFX_DECAL && m_sel.tcc))
		{
			if (m_sel.iip)
			{

				armAsm->Ldr(v6, MemOperand(_v, offsetof(GSVertexSW, c)));
				armAsm->Ldr(v1, MemOperand(x1, offsetof(GSScanlineLocalData::skip, rb)));
				armAsm->Ldr(_vscratch, MemOperand(x1, offsetof(GSScanlineLocalData::skip, ga)));
				armAsm->Fcvtzs(v6.V4S(), v6.V4S());

				armAsm->Ext(v5.V16B(), v6.V16B(), v6.V16B(), 8);
				armAsm->Zip1(v6.V8H(), v6.V8H(), v5.V8H());

				armAsm->Dup(_temp_rb.V4S(), v6.V4S(), 0);
				armAsm->Dup(_temp_ga.V4S(), v6.V4S(), 2);

				armAsm->Add(_temp_rb.V8H(), _temp_rb.V8H(), v1.V8H());
				armAsm->Add(_temp_ga.V8H(), _temp_ga.V8H(), _vscratch.V8H());

				armAsm->Ldr(_d4_c, _local(d4.c));
			}
			else
			{
				armAsm->Ldr(_temp_rb, _local(c.rb));
				armAsm->Ldr(_temp_ga, _local(c.ga));
			}
		}
	}

	if (m_sel.atst != ATST_ALWAYS && m_sel.atst != ATST_NEVER)
	{
		armAsm->Ldr(_local_aref, _global(aref));
	}

	if (m_sel.fwrite && m_sel.fge)
	{
		armAsm->Ldr(_global_frb, _global(frb));
		armAsm->Ldr(_global_fga, _global(fga));
	}

	if (!m_sel.notest)
		armAsm->Ldr(_const_movemskw_mask, _global(const_movemaskw_mask));

	if (m_sel.mmin && !m_sel.lcm)
	{
		armAsm->Ldr(_const_log2_coef, _global(const_log2_coef));
		armAsm->Ldr(_global_l, _global(l));
		armAsm->Ldr(_global_k, _global(k));
		armAsm->Ldr(_global_mxl, _global(mxl));
	}

	if (m_sel.fpsm == 2 && m_sel.dthe)
		armAsm->Ldr(_global_dimx, _global(dimx));
}

void GSDrawScanlineCodeGenerator::Step()
{

	armAsm->Sub(_steps, _steps, 4);

	armAsm->Add(x8, x8, 8);

	if (m_sel.prim != GS_SPRITE_CLASS)
	{

		if (m_sel.zb && !m_sel.zequal)
		{
			armAsm->Fadd(_temp_z1.V2D(), _temp_z1.V2D(), _d4_z.V2D());
			armAsm->Fadd(_temp_z0.V2D(), _temp_z0.V2D(), _d4_z.V2D());
		}

		if (m_sel.fwrite && m_sel.fge)
		{
			armAsm->Add(_temp_f.V8H(), _temp_f.V8H(), _d4_f.V8H());
		}
	}

	if (m_sel.fb)
	{
		if (m_sel.tfx != TFX_NONE)
		{
			if (m_sel.fst)
			{

				armAsm->Dup(_vscratch.V4S(), _d4_stq.V4S(), 0);
				if (m_sel.prim != GS_SPRITE_CLASS || m_sel.mmin)
					armAsm->Dup(_vscratch2.V4S(), _d4_stq.V4S(), 1);

				armAsm->Add(_temp_s.V4S(), _temp_s.V4S(), _vscratch.V4S());

				if (m_sel.prim != GS_SPRITE_CLASS || m_sel.mmin)
					armAsm->Add(_temp_t.V4S(), _temp_t.V4S(), _vscratch2.V4S());
			}
			else
			{

				armAsm->Dup(_vscratch.V4S(), _d4_stq.V4S(), 0);
				armAsm->Dup(_vscratch2.V4S(), _d4_stq.V4S(), 1);
				armAsm->Dup(v1.V4S(), _d4_stq.V4S(), 2);

				armAsm->Fadd(_temp_s.V4S(), _temp_s.V4S(), _vscratch.V4S());
				armAsm->Fadd(_temp_t.V4S(), _temp_t.V4S(), _vscratch2.V4S());
				armAsm->Fadd(_temp_q.V4S(), _temp_q.V4S(), v1.V4S());
			}
		}

		if (!(m_sel.tfx == TFX_DECAL && m_sel.tcc))
		{
			if (m_sel.iip)
			{

				armAsm->Dup(_vscratch.V4S(), _d4_c.V4S(), 0);
				armAsm->Dup(_vscratch2.V4S(), _d4_c.V4S(), 1);
				armAsm->Movi(v1.V8H(), 0);

				armAsm->Add(_temp_rb.V8H(), _temp_rb.V8H(), _vscratch.V8H());
				armAsm->Add(_temp_ga.V8H(), _temp_ga.V8H(), _vscratch2.V8H());

				armAsm->Smax(_temp_rb.V8H(), _temp_rb.V8H(), v1.V8H());
				armAsm->Smax(_temp_ga.V8H(), _temp_ga.V8H(), v1.V8H());
			}
		}
	}

	if (!m_sel.notest)
	{

		armAsm->Asr(w1, _steps, 31);
		armAsm->And(w1, w1, _steps);
		armAsm->Lsl(w1, w1, 4);

		armAsm->Add(_scratchaddr, _globals, offsetof(GSScanlineGlobalData, const_test_128b[7]));
		armAsm->Ldr(_test, MemOperand(_scratchaddr, x1, SXTW));
	}
}

void GSDrawScanlineCodeGenerator::TestZ(const VRegister& temp1, const VRegister& temp2)
{
	if (!m_sel.zb)
	{
		return;
	}

	armAsm->Ldr(w9, MemOperand(x7, 4));
	armAsm->Ldr(_wscratch, MemOperand(x8, 4));
	armAsm->Add(w9, w9, _wscratch);
	armAsm->And(w9, w9, HALF_VM_SIZE - 1);

	VRegister zs;
	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		if (m_sel.zequal)
		{
			zs = _temp_z0;
		}
		else if (m_sel.zoverflow)
		{

			armAsm->Movi(temp1.V2D(), GSVector4::m_xc1e00000000fffff.U64[0]);
			armAsm->Fadd(_temp_z0.V2D(), _temp_z0.V2D(), temp1.V2D());
			armAsm->Fadd(_temp_z1.V2D(), _temp_z1.V2D(), temp1.V2D());

			armAsm->Fcvtzs(v0.V2D(), _temp_z0.V2D());
			armAsm->Fcvtzs(temp1.V2D(), _temp_z1.V2D());
			armAsm->Movi(_vscratch.V4S(), 0x80000000);
			armAsm->Uzp1(v0.V4S(), v0.V4S(), temp1.V4S());

			armAsm->Add(v0.V4S(), v0.V4S(), _vscratch.V4S());
			zs = v0;
		}
		else
		{

			armAsm->Fcvtzs(v0.V2D(), _temp_z0.V2D());
			armAsm->Fcvtzs(temp1.V2D(), _temp_z1.V2D());
			armAsm->Uzp1(v0.V4S(), v0.V4S(), temp1.V4S());
			zs = v0;
		}


		if (m_sel.zclamp)
		{
			armAsm->Movi(temp1.V4S(), 0xFFFFFFFFu >> ((m_sel.zpsm & 0x3) * 8));
			armAsm->Umin(v0.V4S(), zs.V4S(), temp1.V4S());
			zs = v0;
		}

		if (m_sel.zwrite)
			armAsm->Mov(_temp_zs, zs);
	}
	else
	{
		zs = _temp_z0;
	}

	if (m_sel.ztest)
	{
		VRegister zd(_temp_zd);
		ReadPixel(zd, w9);

		if (m_sel.zpsm)
		{
			armAsm->Shl(v1.V4S(), zd.V4S(), m_sel.zpsm * 8);
			armAsm->Ushr(v1.V4S(), v1.V4S(), m_sel.zpsm * 8);
			zd = v1;
		}

		if (m_sel.zpsm == 0)
		{
			armAsm->Movi(temp1.V4S(), 0x80000000u);

			armAsm->Sub(v0.V4S(), zs.V4S(), temp1.V4S());
			armAsm->Sub(v1.V4S(), zd.V4S(), temp1.V4S());
			zs = v0;
			zd = v1;
		}

		switch (m_sel.ztst)
		{
			case ZTST_GEQUAL:
				armAsm->Cmgt(v1.V4S(), zd.V4S(), zs.V4S());
				armAsm->Orr(_test.V16B(), _test.V16B(), v1.V16B());
				break;

			case ZTST_GREATER:
				armAsm->Cmgt(v0.V4S(), zs.V4S(), zd.V4S());
				armAsm->Mvn(v0.V16B(), v0.V16B());
				armAsm->Orr(_test.V16B(), _test.V16B(), v0.V16B());
				break;
		}

		alltrue(_test, temp1);
	}
}

void GSDrawScanlineCodeGenerator::SampleTexture()
{
	if (!m_sel.fb || m_sel.tfx == TFX_NONE)
	{
		return;
	}

	const auto& uf = v4;
	const auto& vf = v7;

	VRegister ureg = _temp_s;
	VRegister vreg = _temp_t;

	if (!m_sel.fst)
	{
		armAsm->Fdiv(v2.V4S(), _temp_s.V4S(), _temp_q.V4S());
		armAsm->Fdiv(v3.V4S(), _temp_t.V4S(), _temp_q.V4S());
		ureg = v2;
		vreg = v3;

		armAsm->Fcvtzs(v2.V4S(), v2.V4S());
		armAsm->Fcvtzs(v3.V4S(), v3.V4S());

		if (m_sel.ltf)
		{

			armAsm->Movi(v1.V4S(), 0x8000);
			armAsm->Sub(v2.V4S(), v2.V4S(), v1.V4S());
			armAsm->Sub(v3.V4S(), v3.V4S(), v1.V4S());
		}
	}

	if (m_sel.ltf)
	{

		armAsm->Trn1(uf.V8H(), ureg.V8H(), ureg.V8H());
		armAsm->Ushr(uf.V8H(), uf.V8H(), 12);

		if (m_sel.prim != GS_SPRITE_CLASS)
		{

			armAsm->Trn1(vf.V8H(), vreg.V8H(), vreg.V8H());
			armAsm->Ushr(vf.V8H(), vf.V8H(), 12);
		}
	}

	armAsm->Sshr(v2.V4S(), ureg.V4S(), 16);
	armAsm->Sshr(v3.V4S(), vreg.V4S(), 16);
	armAsm->Sqxtn(v2.V4H(), v2.V4S());
	armAsm->Sqxtn2(v2.V8H(), v3.V4S());

	if (m_sel.ltf)
	{

		armAsm->Movi(v1.V8H(), 1);
		armAsm->Add(v3.V8H(), v2.V8H(), v1.V8H());

		Wrap(v2, v3);
	}
	else
	{

		Wrap(v2);
	}

	SampleTexture_TexelReadHelper(0);
}

void GSDrawScanlineCodeGenerator::SampleTexture_TexelReadHelper(int mip_offset)
{
	const auto& uf = v4;
	const auto& vf = (m_sel.prim != GS_SPRITE_CLASS || m_sel.mmin) ? v7 : _temp_vf;

	armAsm->Movi(v0.V8H(), 0);

	armAsm->Zip1(v5.V8H(), v2.V8H(), v0.V8H());
	armAsm->Zip2(v2.V8H(), v2.V8H(), v0.V8H());
	armAsm->Shl(v2.V4S(), v2.V4S(), m_sel.tw + 3);

	if (m_sel.ltf)
	{

		armAsm->Zip1(v1.V8H(), v3.V8H(), v0.V8H());
		armAsm->Zip2(v3.V8H(), v3.V8H(), v0.V8H());
		armAsm->Shl(v3.V4S(), v3.V4S(), m_sel.tw + 3);

		armAsm->Add(v0.V4S(), v3.V4S(), v1.V4S());
		armAsm->Add(v1.V4S(), v1.V4S(), v2.V4S());
		armAsm->Add(v2.V4S(), v2.V4S(), v5.V4S());
		armAsm->Add(v3.V4S(), v3.V4S(), v5.V4S());

		ReadTexel4(v5, v6, v0, v2, v1, v3, mip_offset);

		split16_2x8(v3, v6, v6);

		split16_2x8(v0, v1, v0);

		lerp16_4(v0, v3, uf);
		lerp16_4(v1, v6, uf);

		split16_2x8(v2, v3, v2);

		split16_2x8(v5, v6, v5);

		lerp16_4(v5, v2, uf);
		lerp16_4(v6, v3, uf);

		lerp16_4(v5, v0, vf);
		lerp16_4(v6, v1, vf);
	}
	else
	{

		armAsm->Add(v2.V4S(), v2.V4S(), v5.V4S());

		ReadTexel1(v5, v2, v0, mip_offset);

		split16_2x8(v5, v6, v5);
	}
}

void GSDrawScanlineCodeGenerator::Wrap(const VRegister& uv)
{

	int wms_clamp = ((m_sel.wms + 1) >> 1) & 1;
	int wmt_clamp = ((m_sel.wmt + 1) >> 1) & 1;

	int region = ((m_sel.wms | m_sel.wmt) >> 1) & 1;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				armAsm->Smax(uv.V8H(), uv.V8H(), _global_tmin.V8H());
			}
			else
			{
				armAsm->Movi(v0.V8H(), 0);
				armAsm->Smax(uv.V8H(), uv.V8H(), v0.V8H());
			}

			armAsm->Smin(uv.V8H(), uv.V8H(), _global_tmax.V8H());
		}
		else
		{
			armAsm->And(uv.V16B(), uv.V16B(), _global_tmin.V16B());

			if (region)
				armAsm->Orr(uv.V16B(), uv.V16B(), _global_tmax.V16B());
		}
	}
	else
	{

		armAsm->And(v1.V16B(), uv.V16B(), _global_tmin.V16B());

		if (region)
			armAsm->Orr(v1.V16B(), v1.V16B(), _global_tmax.V16B());

		armAsm->Smax(uv.V8H(), uv.V8H(), _global_tmin.V8H());
		armAsm->Smin(_vscratch.V8H(), uv.V8H(), _global_tmax.V8H());

		armAsm->Sshr(uv.V16B(), _global_tmask.V16B(), 7);
		armAsm->Bsl(uv.V16B(), v1.V16B(), _vscratch.V16B());
	}
}

void GSDrawScanlineCodeGenerator::Wrap(const VRegister& uv0, const VRegister& uv1)
{

	int wms_clamp = ((m_sel.wms + 1) >> 1) & 1;
	int wmt_clamp = ((m_sel.wmt + 1) >> 1) & 1;

	int region = ((m_sel.wms | m_sel.wmt) >> 1) & 1;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				armAsm->Smax(uv0.V8H(), uv0.V8H(), _global_tmin.V8H());
				armAsm->Smax(uv1.V8H(), uv1.V8H(), _global_tmin.V8H());
			}
			else
			{
				armAsm->Movi(v0.V16B(), 0);
				armAsm->Smax(uv0.V8H(), uv0.V8H(), v0.V8H());
				armAsm->Smax(uv1.V8H(), uv1.V8H(), v0.V8H());
			}

			armAsm->Smin(uv0.V8H(), uv0.V8H(), _global_tmax.V8H());
			armAsm->Smin(uv1.V8H(), uv1.V8H(), _global_tmax.V8H());
		}
		else
		{
			armAsm->And(uv0.V16B(), uv0.V16B(), _global_tmin.V16B());
			armAsm->And(uv1.V16B(), uv1.V16B(), _global_tmin.V16B());

			if (region)
			{
				armAsm->Orr(uv0.V16B(), uv0.V16B(), _global_tmax.V16B());
				armAsm->Orr(uv1.V16B(), uv1.V16B(), _global_tmax.V16B());
			}
		}
	}
	else
	{

		armAsm->And(v1.V16B(), uv0.V16B(), _global_tmin.V16B());

		if (region)
			armAsm->Orr(v1.V16B(), v1.V16B(), _global_tmax.V16B());

		armAsm->Smax(uv0.V8H(), uv0.V8H(), _global_tmin.V8H());
		armAsm->Smin(_vscratch.V8H(), uv0.V8H(), _global_tmax.V8H());

		armAsm->Sshr(uv0.V16B(), _global_tmask.V16B(), 7);
		armAsm->Bsl(uv0.V16B(), v1.V16B(), _vscratch.V16B());

		armAsm->And(v1.V16B(), uv1.V16B(), _global_tmin.V16B());

		if (region)
			armAsm->Orr(v1.V16B(), v1.V16B(), _global_tmax.V16B());

		armAsm->Smax(uv1.V8H(), uv1.V8H(), _global_tmin.V8H());
		armAsm->Smin(_vscratch.V8H(), uv1.V8H(), _global_tmax.V8H());

		armAsm->Sshr(uv1.V16B(), _global_tmask.V16B(), 7);
		armAsm->Bsl(uv1.V16B(), v1.V16B(), _vscratch.V16B());
	}
}

void GSDrawScanlineCodeGenerator::SampleTextureLOD()
{
	if (!m_sel.fb || m_sel.tfx == TFX_NONE)
	{
		return;
	}

	const auto& uf = v4;
	const auto& vf = v7;
	const auto& local0 = _vscratch;
	const auto& local1 = _vscratch2;
	const auto& local2 = _vscratch3;

	VRegister uv0(_temp_s);
	VRegister uv1(_temp_t);

	if (!m_sel.fst)
	{
		armAsm->Fdiv(local0.V4S(), _temp_s.V4S(), _temp_q.V4S());
		armAsm->Fdiv(local1.V4S(), _temp_t.V4S(), _temp_q.V4S());

		armAsm->Fcvtzs(local0.V4S(), local0.V4S());
		armAsm->Fcvtzs(local1.V4S(), local1.V4S());

		uv0 = local0;
		uv1 = local1;
	}

	if (!m_sel.lcm)
	{

		armAsm->Movi(v1.V4S(), 127);
		armAsm->Shl(v0.V4S(), _temp_q.V4S(), 1);
		armAsm->Ushr(v0.V4S(), v0.V4S(), 24);
		armAsm->Sub(v0.V4S(), v0.V4S(), v1.V4S());
		armAsm->Scvtf(v0.V4S(), v0.V4S());

		armAsm->Shl(v4.V4S(), _temp_q.V4S(), 9);
		armAsm->Ushr(v4.V4S(), v4.V4S(), 9);

		armAsm->Dup(v1.V4S(), _const_log2_coef.V4S(), 3);
		armAsm->Orr(v4.V16B(), v4.V16B(), v1.V16B());

#if 0
		armAsm->Dup(v1.V4S(), _const_log2_coef.V4S(), 0);
		armAsm->Fmul(v5.V4S(), v4.V4S(), v1.V4S());

		armAsm->Dup(v1.V4S(), _const_log2_coef.V4S(), 1);
		armAsm->Fadd(v5.V4S(), v5.V4S(), v1.V4S());

		armAsm->Fmul(v5.V4S(), v5.V4S(), v4.V4S());

		armAsm->Dup(v1.V4S(), _const_log2_coef.V4S(), 3);
		armAsm->Fsub(v4.V4S(), v4.V4S(), v1.V4S());
			
		armAsm->Dup(v1.V4S(), _const_log2_coef.V4S(), 2);
		armAsm->Fadd(v5.V4S(), v5.V4S(), v1.V4S());

		armAsm->Fmul(v4.V4S(), v4.V4S(), v5.V4S());
		armAsm->Fadd(v4.V4S(), v4.V4S(), v0.V4S());

		armAsm->Dup(v0.V4S(), _global_l);
		armAsm->Dup(v1.V4S(), _global_k);

		armAsm->Fmul(v4.V4S(), v4.V4S(), v0.V4S());
		armAsm->Fadd(v4.V4S(), v4.V4S(), v1.V4S());
#else
		armAsm->Dup(v1.V4S(), _const_log2_coef.V4S(), 0);
		armAsm->Dup(local2.V4S(), _const_log2_coef.V4S(), 1);
		armAsm->Fmla(local2.V4S(), v4.V4S(), v1.V4S());
		armAsm->Dup(v1.V4S(), _const_log2_coef.V4S(), 2);
		armAsm->Fmla(v1.V4S(), local2.V4S(), v4.V4S());
		armAsm->Dup(local2.V4S(), _const_log2_coef.V4S(), 3);
		armAsm->Fsub(v4.V4S(), v4.V4S(), local2.V4S());
		armAsm->Fmla(v0.V4S(), v4.V4S(), v1.V4S());

		armAsm->Dup(v1.V4S(), _global_l);
		armAsm->Dup(v4.V4S(), _global_k);
		armAsm->Fmla(v4.V4S(), v0.V4S(), v1.V4S());
#endif

		armAsm->Dup(v0.V4S(), _global_mxl);
		armAsm->Movi(v1.V4S(), 0);
		armAsm->Fminnm(v4.V4S(), v4.V4S(), v0.V4S());
		armAsm->Fmaxnm(v4.V4S(), v4.V4S(), v1.V4S());
		armAsm->Fcvtzs(v4.V4S(), v4.V4S());

		if (m_sel.mmin == 1)
		{
			armAsm->Movi(v0.V4S(), 0x8000);
			armAsm->Add(v4.V4S(), v4.V4S(), v0.V4S());
		}

		armAsm->Ushr(v0.V4S(), v4.V4S(), 16);

		armAsm->Str(v0, _local(temp.lod.i));

		if (m_sel.mmin == 2)
		{
			armAsm->Trn1(v1.V8H(), v4.V8H(), v4.V8H());
			armAsm->Str(v1.V4S(), _local(temp.lod.f));
		}

		armAsm->Neg(v0.V4S(), v0.V4S());
		armAsm->Sshl(local0.V4S(), uv0.V4S(), v0.V4S());
		armAsm->Sshl(local1.V4S(), uv1.V4S(), v0.V4S());
		uv0 = local0;
		uv1 = local1;

		armAsm->Movi(v1.V4S(), 0);
		armAsm->Zip1(v5.V8H(), _global_tmin.V8H(), v1.V8H());
		armAsm->Zip2(v6.V8H(), _global_tmin.V8H(), v1.V8H());
		armAsm->Ushl(v5.V4S(), v5.V4S(), v0.V4S());
		armAsm->Ushl(v6.V4S(), v6.V4S(), v0.V4S());
		armAsm->Sqxtun(v5.V4H(), v5.V4S());
		armAsm->Sqxtun2(v5.V8H(), v6.V4S());

		armAsm->Zip1(v6.V8H(), _global_tmax.V8H(), v1.V8H());
		armAsm->Zip2(v4.V8H(), _global_tmax.V8H(), v1.V8H());
		armAsm->Ushl(v6.V4S(), v6.V4S(), v0.V4S());
		armAsm->Ushl(v4.V4S(), v4.V4S(), v0.V4S());
		armAsm->Sqxtun(v6.V4H(), v6.V4S());
		armAsm->Sqxtun2(v6.V8H(), v4.V4S());

		if (m_sel.mmin != 1)
		{
			armAsm->Str(v5, _local(temp.uv_minmax[0]));
			armAsm->Str(v6, _local(temp.uv_minmax[1]));
		}
	}
	else
	{

		armAsm->Add(_scratchaddr, _globals, offsetof(GSScanlineGlobalData, lod));
		armAsm->Ld1r(v0.V4S(), MemOperand(_scratchaddr));
		armAsm->Neg(v0.V4S(), v0.V4S());

		armAsm->Sshl(local0.V4S(), uv0.V4S(), v0.V4S());
		armAsm->Sshl(local1.V4S(), uv1.V4S(), v0.V4S());
		uv0 = local0;
		uv1 = local1;

		armAsm->Ldr(v5, _local(temp.uv_minmax[0]));
		armAsm->Ldr(v6, _local(temp.uv_minmax[1]));
	}

	if (m_sel.ltf)
	{

		armAsm->Movi(v4.V4S(), 0x8000);
		armAsm->Sub(v2.V4S(), uv0.V4S(), v4.V4S());
		armAsm->Sub(v3.V4S(), uv1.V4S(), v4.V4S());

		armAsm->Trn1(uf.V8H(), v2.V8H(), v2.V8H());
		armAsm->Ushr(uf.V8H(), uf.V8H(), 12);

		armAsm->Trn1(vf.V8H(), v3.V8H(), v3.V8H());
		armAsm->Ushr(vf.V8H(), vf.V8H(), 12);
	}

	armAsm->Sshr(v2.V4S(), m_sel.ltf ? v2.V4S() : uv0.V4S(), 16);
	armAsm->Sshr(v3.V4S(), m_sel.ltf ? v3.V4S() : uv1.V4S(), 16);
	armAsm->Sqxtn(v2.V4H(), v2.V4S());
	armAsm->Sqxtn2(v2.V8H(), v3.V4S());

	if (m_sel.ltf)
	{

		armAsm->Movi(v1.V8H(), 1);
		armAsm->Add(v3.V8H(), v2.V8H(), v1.V8H());

		WrapLOD(v2, v3, v0, v1, v5, v6);
	}
	else
	{

		WrapLOD(v2, v0, v1, v5, v6);
	}

	SampleTexture_TexelReadHelper(0);

	if (m_sel.mmin != 1)
	{
		armAsm->Sshr(v2.V4S(), uv0.V4S(), 1);
		armAsm->Sshr(v3.V4S(), uv1.V4S(), 1);

		armAsm->Mov(local0, v5);
		armAsm->Mov(local1, v6);

		armAsm->Ldr(v5, _local(temp.uv_minmax[0]));
		armAsm->Ldr(v6, _local(temp.uv_minmax[1]));

		armAsm->Ushr(v5.V8H(), v5.V8H(), 1);
		armAsm->Ushr(v6.V8H(), v6.V8H(), 1);

		if (m_sel.ltf)
		{

			armAsm->Movi(v4.V4S(), 0x8000);
			armAsm->Sub(v2.V4S(), v2.V4S(), v4.V4S());
			armAsm->Sub(v3.V4S(), v3.V4S(), v4.V4S());

			armAsm->Trn1(uf.V8H(), v2.V8H(), v2.V8H());
			armAsm->Ushr(uf.V8H(), uf.V8H(), 12);

			armAsm->Trn1(vf.V8H(), v3.V8H(), v3.V8H());
			armAsm->Ushr(vf.V8H(), vf.V8H(), 12);
		}

		armAsm->Sshr(v2.V4S(), v2.V4S(), 16);
		armAsm->Sshr(v3.V4S(), v3.V4S(), 16);
		armAsm->Sqxtn(v2.V4H(), v2.V4S());
		armAsm->Sqxtn2(v2.V8H(), v3.V4S());

		if (m_sel.ltf)
		{

			armAsm->Movi(v1.V8H(), 1);
			armAsm->Add(v3.V8H(), v2.V8H(), v1.V8H());

			WrapLOD(v2, v3, v0, v1, v5, v6);
		}
		else
		{

			WrapLOD(v2, v0, v1, v5, v6);
		}

		armAsm->Ldr(local2, m_sel.lcm ? _global(lod.f) : _local(temp.lod.f));

		SampleTexture_TexelReadHelper(1);

		armAsm->Ushr(v0.V8H(), local2.V8H(), 1);

		lerp16(v5, local0, v0, 0);
		lerp16(v6, local1, v0, 0);
	}
}

void GSDrawScanlineCodeGenerator::WrapLOD(const VRegister& uv,
	const VRegister& tmp, const VRegister& tmp2,
	const VRegister& min, const VRegister& max)
{
	const int wms_clamp = ((m_sel.wms + 1) >> 1) & 1;
	const int wmt_clamp = ((m_sel.wmt + 1) >> 1) & 1;
	const int region = ((m_sel.wms | m_sel.wmt) >> 1) & 1;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				armAsm->Smax(uv.V8H(), uv.V8H(), min.V8H());
			}
			else
			{
				armAsm->Movi(tmp.V8H(), 0);
				armAsm->Smax(uv.V8H(), uv.V8H(), tmp.V8H());
			}

			armAsm->Smin(uv.V8H(), uv.V8H(), max.V8H());
		}
		else
		{
			armAsm->And(uv.V16B(), uv.V16B(), min.V16B());

			if (region)
				armAsm->Orr(uv.V16B(), uv.V16B(), max.V16B());
		}
	}
	else
	{

		armAsm->And(tmp.V16B(), uv.V16B(), min.V16B());
		if (region)
			armAsm->Orr(tmp.V16B(), tmp.V16B(), max.V16B());

		armAsm->Smax(uv.V8H(), uv.V8H(), min.V8H());
		armAsm->Smin(tmp2.V8H(), uv.V8H(), max.V8H());

		armAsm->Sshr(uv.V16B(), _global_tmask.V16B(), 7);
		armAsm->Bsl(uv.V16B(), tmp.V16B(), tmp2.V16B());
	}
}

void GSDrawScanlineCodeGenerator::WrapLOD(
	const VRegister& uv0, const VRegister& uv1,
	const VRegister& tmp, const VRegister& tmp2,
	const VRegister& min, const VRegister& max)
{
	const int wms_clamp = ((m_sel.wms + 1) >> 1) & 1;
	const int wmt_clamp = ((m_sel.wmt + 1) >> 1) & 1;
	const int region = ((m_sel.wms | m_sel.wmt) >> 1) & 1;

	if (wms_clamp == wmt_clamp)
	{
		if (wms_clamp)
		{
			if (region)
			{
				armAsm->Smax(uv0.V8H(), uv0.V8H(), min.V8H());
				armAsm->Smax(uv1.V8H(), uv1.V8H(), min.V8H());
			}
			else
			{
				armAsm->Movi(tmp.V8H(), 0);
				armAsm->Smax(uv0.V8H(), uv0.V8H(), tmp.V8H());
				armAsm->Smax(uv1.V8H(), uv1.V8H(), tmp.V8H());
			}

			armAsm->Smin(uv0.V8H(), uv0.V8H(), max.V8H());
			armAsm->Smin(uv1.V8H(), uv1.V8H(), max.V8H());
		}
		else
		{
			armAsm->And(uv0.V16B(), uv0.V16B(), min.V16B());
			armAsm->And(uv1.V16B(), uv1.V16B(), min.V16B());

			if (region)
			{
				armAsm->Orr(uv0.V16B(), uv0.V16B(), max.V16B());
				armAsm->Orr(uv1.V16B(), uv1.V16B(), max.V16B());
			}
		}
	}
	else
	{
		for (const VRegister& uv : {uv0, uv1})
		{
			armAsm->And(tmp.V16B(), uv.V16B(), min.V16B());
			if (region)
				armAsm->Orr(tmp.V16B(), tmp.V16B(), max.V16B());

			armAsm->Smax(uv.V8H(), uv.V8H(), min.V8H());
			armAsm->Smin(tmp2.V8H(), uv.V8H(), max.V8H());

			armAsm->Sshr(uv.V16B(), _global_tmask.V16B(), 7);
			armAsm->Bsl(uv.V16B(), tmp.V16B(), tmp2.V16B());
		}
	}
}

void GSDrawScanlineCodeGenerator::AlphaTFX()
{
	if (!m_sel.fb)
	{
		return;
	}

	switch (m_sel.tfx)
	{
		case TFX_MODULATE:

			modulate16(v6, _temp_ga, 1);
			clamp16(v6, v3);

			if (!m_sel.tcc)
			{
				armAsm->Ushr(v4.V8H(), _temp_ga.V8H(), 7);

				mix16(v6, v4, v3);
			}

			break;

		case TFX_DECAL:

			if (!m_sel.tcc)
			{

				armAsm->Ushr(v4.V8H(), _temp_ga.V8H(), 7);
				mix16(v6, v4, v3);
			}

			break;

		case TFX_HIGHLIGHT:

			armAsm->Ushr(v4.V8H(), _temp_ga.V8H(), 7);

			if (m_sel.tcc)
				armAsm->Uqadd(v4.V16B(), v4.V16B(), v6.V16B());

			mix16(v6, v4, v3);

			break;

		case TFX_HIGHLIGHT2:

			if (!m_sel.tcc)
			{
				armAsm->Ushr(v4.V8H(), _temp_ga.V8H(), 7);

				mix16(v6, v4, v3);
			}

			break;

		case TFX_NONE:

			if (m_sel.iip)
				armAsm->Ushr(v6.V8H(), _temp_ga.V8H(), 7);
			else
				armAsm->Mov(v6, _temp_ga);

			break;
	}

	if (m_sel.aa1)
	{

		if (!m_sel.abe)
		{

			if (m_sel.edge)
				armAsm->Ldr(v0, _local(temp.cov));
			else
				armAsm->Movi(v0.V8H(), 0x0080);

			mix16(v6, v0, v1);
		}
		else
		{

			armAsm->Movi(v0.V8H(), 0x0080);

			if (m_sel.edge)
				armAsm->Ldr(v1, _local(temp.cov));
			else
				armAsm->Mov(v1, v0);

			armAsm->Cmeq(v0.V8H(), v0.V8H(), v6.V8H());
			armAsm->Ushr(v0.V4S(), v0.V4S(), 16);
			armAsm->Shl(v0.V4S(), v0.V4S(), 16);

			blend8(v6, v1, v0, _vscratch);
		}
	}
}

void GSDrawScanlineCodeGenerator::ReadMask()
{
	if (m_sel.fwrite)
		armAsm->Ldr(v3, _global(fm));

	if (m_sel.zwrite)
		armAsm->Ldr(v4, _global(zm));
}

void GSDrawScanlineCodeGenerator::TestAlpha()
{
	switch (m_sel.atst)
	{
		case ATST_NEVER:
			armAsm->Movi(v1.V2D(), 0xFFFFFFFFFFFFFFFFULL);
			break;

		case ATST_ALWAYS:
			return;

		case ATST_LESS:
		case ATST_LEQUAL:
			armAsm->Dup(_vscratch.V4S(), _local_aref);
			armAsm->Ushr(v1.V4S(), v6.V4S(), 16);
			armAsm->Cmgt(v1.V4S(), v1.V4S(), _vscratch.V4S());
			break;

		case ATST_EQUAL:
			armAsm->Dup(_vscratch.V4S(), _local_aref);
			armAsm->Ushr(v1.V4S(), v6.V4S(), 16);
			armAsm->Cmeq(v1.V4S(), v1.V4S(), _vscratch.V4S());
			armAsm->Mvn(v1.V16B(), v1.V16B());
			break;

		case ATST_GEQUAL:
		case ATST_GREATER:
			armAsm->Dup(_vscratch.V4S(), _local_aref);
			armAsm->Ushr(v1.V4S(), v6.V4S(), 16);
			armAsm->Cmgt(v1.V4S(), _vscratch.V4S(), v1.V4S());
			break;

		case ATST_NOTEQUAL:
			armAsm->Dup(_vscratch.V4S(), _local_aref);
			armAsm->Ushr(v1.V4S(), v6.V4S(), 16);
			armAsm->Cmeq(v1.V4S(), v1.V4S(), _vscratch.V4S());
			break;
	}

	switch (m_sel.afail)
	{
		case AFAIL_KEEP:
			armAsm->Orr(_test.V16B(), _test.V16B(), v1.V16B());
			alltrue(_test, _vscratch);
			break;

		case AFAIL_FB_ONLY:
			armAsm->Orr(v4.V16B(), v4.V16B(), v1.V16B());
			break;

		case AFAIL_ZB_ONLY:
			armAsm->Orr(v3.V16B(), v3.V16B(), v1.V16B());
			break;

		case AFAIL_RGB_ONLY:
			armAsm->Orr(v4.V16B(), v4.V16B(), v1.V16B());

			armAsm->Ushr(v1.V4S(), v1.V4S(), 24);
			armAsm->Shl(v1.V4S(), v1.V4S(), 24);
			armAsm->Orr(v3.V16B(), v3.V16B(), v1.V16B());
			break;
	}
}

void GSDrawScanlineCodeGenerator::ColorTFX()
{
	if (!m_sel.fwrite)
	{
		return;
	}

	switch (m_sel.tfx)
	{
		case TFX_MODULATE:

			modulate16(v5, _temp_rb, 1);
			clamp16(v5, v1);

			break;

		case TFX_DECAL:

			break;

		case TFX_HIGHLIGHT:
		case TFX_HIGHLIGHT2:

			armAsm->Mov(v1, v6);
			modulate16(v6, _temp_ga, 1);

			armAsm->Trn2(v2.V8H(), _temp_ga.V8H(), _temp_ga.V8H());
			armAsm->Ushr(v2.V8H(), v2.V8H(), 7);
			armAsm->Add(v6.V8H(), v6.V8H(), v2.V8H());

			clamp16(v6, v0);

			mix16(v6, v1, v0);

			modulate16(v5, _temp_rb, 1);
			armAsm->Add(v5.V8H(), v5.V8H(), v2.V8H());

			clamp16(v5, v0);

			break;

		case TFX_NONE:

			if (m_sel.iip)
				armAsm->Ushr(v5.V8H(), _temp_rb.V8H(), 7);
			else
				armAsm->Mov(v5, _temp_rb);

			break;
	}
}

void GSDrawScanlineCodeGenerator::Fog()
{
	if (!m_sel.fwrite || !m_sel.fge)
	{
		return;
	}

	armAsm->Dup(_vscratch.V4S(), _global_frb);
	armAsm->Dup(_vscratch2.V4S(), _global_fga);
	armAsm->Mov(v1, v6);

	lerp16(v5, _vscratch, _temp_f, 0);

	lerp16(v6, _vscratch2, _temp_f, 0);
	mix16(v6, v1, v0);
}

void GSDrawScanlineCodeGenerator::ReadFrame()
{
	if (!m_sel.fb)
	{
		return;
	}

	armAsm->Ldr(w6, MemOperand(x7));
	armAsm->Ldr(_wscratch, MemOperand(x8));
	armAsm->Add(w6, w6, _wscratch);
	armAsm->And(w6, w6, HALF_VM_SIZE - 1);

	if (!m_sel.rfb)
	{
		return;
	}

	ReadPixel(v2, w6);
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
			armAsm->Movi(v0.V4S(), 0);
			armAsm->Shl(v1.V4S(), _fd.V4S(), 16);
			armAsm->Ushr(v1.V4S(), v1.V4S(), 31);
			armAsm->Cmeq(v1.V4S(), v0.V4S(), 0);
		}
		else
		{
			armAsm->Mvn(v1.V16B(), _fd.V16B());
			armAsm->Sshr(v1.V4S(), v1.V4S(), 31);
		}
	}
	else
	{
		if (m_sel.fpsm == 2)
		{
			armAsm->Shl(v1.V4S(), _fd.V4S(), 16);
			armAsm->Sshr(v1.V4S(), v1.V4S(), 31);
		}
		else
		{
			armAsm->Sshr(v1.V4S(), _fd.V4S(), 31);
		}
	}

	armAsm->Orr(_test.V16B(), _test.V16B(), v1.V16B());

	alltrue(_test, _vscratch);
}

void GSDrawScanlineCodeGenerator::WriteMask()
{
	if (m_sel.notest)
	{
		return;
	}

	if (m_sel.fwrite)
		armAsm->Orr(v3.V16B(), v3.V16B(), _test.V16B());

	if (m_sel.zwrite)
		armAsm->Orr(v4.V16B(), v4.V16B(), _test.V16B());

	armAsm->Movi(v1.V4S(), 0xFFFFFFFFu);

	if (m_sel.fwrite && m_sel.zwrite)
	{
		armAsm->Cmeq(v0.V4S(), v1.V4S(), v4.V4S());
		armAsm->Cmeq(v1.V4S(), v1.V4S(), v3.V4S());
		armAsm->Sqxtn(v1.V4H(), v1.V4S());
		armAsm->Sqxtn2(v1.V8H(), v0.V4S());
	}
	else if (m_sel.fwrite)
	{
		armAsm->Cmeq(_vscratch.V4S(), v1.V4S(), v3.V4S());
		armAsm->Sqxtn(v1.V4H(), _vscratch.V4S());
		armAsm->Sqxtn2(v1.V8H(), _vscratch.V4S());
	}
	else if (m_sel.zwrite)
	{
		armAsm->Cmeq(v1.V4S(), v1.V4S(), v4.V4S());
		armAsm->Sqxtn(v1.V4H(), _vscratch.V4S());
		armAsm->Sqxtn2(v1.V8H(), _vscratch.V4S());
	}

	armAsm->And(v1.V16B(), v1.V16B(), _const_movemskw_mask.V16B());
	armAsm->Addv(v1.H(), v1.V8H());
	armAsm->Umov(w1, v1.V8H(), 0);
	armAsm->Mvn(w1, w1);
}

void GSDrawScanlineCodeGenerator::WriteZBuf()
{
	if (!m_sel.zwrite)
	{
		return;
	}

	armAsm->Mov(v1, m_sel.prim != GS_SPRITE_CLASS ? _temp_zs : _temp_z0);

	if (m_sel.ztest && m_sel.zpsm < 2)
	{

		blend8(v1, _temp_zd, v4, _vscratch);
	}

	if (m_sel.zclamp)
	{
		armAsm->Movi(v7.V4S(), 0xFFFFFFFFu >> (u8)((m_sel.zpsm & 0x3) * 8));
		armAsm->Smin(v1.V4S(), v1.V4S(), v7.V4S());
	}

	bool fast = m_sel.ztest ? m_sel.zpsm < 2 : m_sel.zpsm == 0 && m_sel.notest;

	WritePixel(v1, w9, w1, true, fast, m_sel.zpsm, 1);
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

	if (((m_sel.aba != m_sel.abb) && (m_sel.aba == 1 || m_sel.abb == 1 || m_sel.abc == 1)) || m_sel.abd == 1)
	{
		switch (m_sel.fpsm)
		{
			case 0:
			case 1:

				split16_2x8(v0, v1, v2);

				break;

			case 2:

				armAsm->Movi(v7.V4S(), 0x1F);
				armAsm->And(v0.V16B(), v2.V16B(), v7.V16B());
				armAsm->Shl(v0.V4S(), v0.V4S(), 3);

				armAsm->Movi(v7.V4S(), 0x7C00);
				armAsm->And(v4.V16B(), v2.V16B(), v7.V16B());
				armAsm->Movi(v7.V4S(), 0x3E0);
				armAsm->Shl(v4.V4S(), v4.V4S(), 9);

				armAsm->Orr(v0.V16B(), v0.V16B(), v4.V16B());

				armAsm->And(v1.V16B(), v2.V16B(), v7.V16B());
				armAsm->Ushr(v1.V4S(), v1.V4S(), 2);

				armAsm->Movi(v7.V4S(), 0x8000);
				armAsm->And(v4.V16B(), v2.V16B(), v7.V16B());
				armAsm->Shl(v4.V4S(), v4.V4S(), 8);

				armAsm->Orr(v1.V16B(), v1.V16B(), v4.V16B());
				break;
		}
	}

	if (m_sel.pabe || ((m_sel.aba != m_sel.abb) && (m_sel.abb == 0 || m_sel.abd == 0)))
	{
		armAsm->Mov(v4, v5);
	}

	if (m_sel.aba != m_sel.abb)
	{

		switch (m_sel.aba)
		{
			case 0:
				break;
			case 1:
				armAsm->Mov(v5, v0);
				break;
			case 2:
				armAsm->Movi(v5.V16B(), 0);
				break;
		}

		switch (m_sel.abb)
		{
			case 0:
				armAsm->Sub(v5.V8H(), v5.V8H(), v4.V8H());
				break;
			case 1:
				armAsm->Sub(v5.V8H(), v5.V8H(), v0.V8H());
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
					armAsm->Trn2(v7.V8H(), m_sel.abc ? v1.V8H() : v6.V8H(), m_sel.abc ? v1.V8H() : v6.V8H());
					armAsm->Shl(v7.V8H(), v7.V8H(), 7);
					break;
				case 2:
					armAsm->Ldr(v7, _global(afix));
					break;
			}

			modulate16(v5, v7, 1);
		}

		switch (m_sel.abd)
		{
			case 0:
				armAsm->Add(v5.V8H(), v5.V8H(), v4.V8H());
				break;
			case 1:
				armAsm->Add(v5.V8H(), v5.V8H(), v0.V8H());
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
				armAsm->Mov(v5, v0);
				break;
			case 2:
				armAsm->Movi(v5.V16B(), 0);
				break;
		}
	}

	if (m_sel.pabe)
	{

		armAsm->Shl(v0.V4S(), v6.V4S(), 8);
		armAsm->Sshr(v0.V4S(), v0.V4S(), 31);

		blend8r(v5, v4, v0, _vscratch);
	}

	armAsm->Mov(v4, v6);

	if (m_sel.aba != m_sel.abb)
	{

		switch (m_sel.aba)
		{
			case 0:
				break;
			case 1:
				armAsm->Mov(v6, v1);
				break;
			case 2:
				armAsm->Movi(v6.V16B(), 0);
				break;
		}

		switch (m_sel.abb)
		{
			case 0:
				armAsm->Sub(v6.V8H(), v6.V8H(), v4.V8H());
				break;
			case 1:
				armAsm->Sub(v6.V8H(), v6.V8H(), v1.V8H());
				break;
			case 2:
				break;
		}

		if (!(m_sel.fpsm == 1 && m_sel.abc == 1))
		{

			modulate16(v6, v7, 1);
		}

		switch (m_sel.abd)
		{
			case 0:
				armAsm->Add(v6.V8H(), v6.V8H(), v4.V8H());
				break;
			case 1:
				armAsm->Add(v6.V8H(), v6.V8H(), v1.V8H());
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
				armAsm->Mov(v6, v1);
				break;
			case 2:
				armAsm->Movi(v6.V16B(), 0);
				break;
		}
	}

	if (m_sel.pabe)
	{
		armAsm->Ushr(v0.V4S(), v0.V4S(), 16);

		blend8r(v6, v4, v0, _vscratch);
	}
	else
	{
		if (m_sel.fpsm != 1)
		{
			mix16(v6, v4, v7);
		}
	}
}

void GSDrawScanlineCodeGenerator::WriteFrame()
{
	if (!m_sel.fwrite)
	{
		return;
	}

	if (m_sel.fpsm == 2 && m_sel.dthe)
	{
		armAsm->And(w5, _top, 3);
		armAsm->Lsl(w5, w5, 5);
		armAsm->Ldr(_vscratch, MemOperand(_global_dimx, x5));
		armAsm->Add(x5, x5, sizeof(GSVector4i));
		armAsm->Ldr(_vscratch2, MemOperand(_global_dimx, x5));
		armAsm->Add(v5.V8H(), v5.V8H(), _vscratch.V8H());
		armAsm->Add(v6.V8H(), v6.V8H(), _vscratch2.V8H());
	}

	if (m_sel.colclamp == 0)
	{

		armAsm->Movi(v7.V8H(), 0xFF);

		armAsm->And(v5.V16B(), v5.V16B(), v7.V16B());
		armAsm->And(v6.V16B(), v6.V16B(), v7.V16B());
	}

	armAsm->Zip2(v7.V8H(), v5.V8H(), v6.V8H());
	armAsm->Zip1(v5.V8H(), v5.V8H(), v6.V8H());
	armAsm->Sqxtun(v5.V8B(), v5.V8H());
	armAsm->Sqxtun2(v5.V16B(), v7.V8H());

	if (m_sel.fba && m_sel.fpsm != 1)
	{

		armAsm->Movi(v7.V4S(), 0x80000000);
		armAsm->Orr(v5.V16B(), v5.V16B(), v7.V16B());
	}

	if (m_sel.fpsm == 2)
	{

		armAsm->Movi(v6.V4S(), 0x00f800f8);

		armAsm->Movi(v7.V4S(), 0x8000f800);

		armAsm->And(v4.V16B(), v5.V16B(), v6.V16B());
		armAsm->And(v5.V16B(), v5.V16B(), v7.V16B());

		armAsm->Ushr(v6.V4S(), v4.V4S(), 9);
		armAsm->Ushr(v7.V4S(), v5.V4S(), 16);
		armAsm->Ushr(v4.V4S(), v4.V4S(), 3);
		armAsm->Ushr(v5.V4S(), v5.V4S(), 6);

		armAsm->Orr(v5.V16B(), v5.V16B(), v4.V16B());
		armAsm->Orr(v7.V16B(), v7.V16B(), v6.V16B());
		armAsm->Orr(v5.V16B(), v5.V16B(), v7.V16B());
	}

	const VRegister& pixel = m_sel.rfb ? v3 : v5;
	if (m_sel.rfb)
	{

		armAsm->Bsl(v3.V16B(), v2.V16B(), v5.V16B());
	}

	const bool fast = m_sel.rfb ? m_sel.fpsm < 2 : m_sel.fpsm == 0 && m_sel.notest;

	WritePixel(pixel, w6, w1, false, fast, m_sel.fpsm, 0);
}

void GSDrawScanlineCodeGenerator::ReadPixel(const VRegister& dst, const Register& addr)
{
	pxAssert(addr.IsW());
	armAsm->Lsl(_wscratch, addr, 1);
	armAsm->Ldr(dst.D(), MemOperand(_vm, _xscratch));
	armAsm->Add(_scratchaddr, _vm_high, _xscratch);
	armAsm->Ld1(dst.V2D(), 1, MemOperand(_scratchaddr));
}

void GSDrawScanlineCodeGenerator::WritePixel(const VRegister& src, const Register& addr, const Register& mask, bool high, bool fast, int psm, int fz)
{
	pxAssert(addr.IsW() && mask.IsW());
	if (m_sel.notest)
	{
		if (fast)
		{
			armAsm->Lsl(_wscratch, addr, 1);
			armAsm->Str(src.D(), MemOperand(_vm, _xscratch));
			armAsm->Add(_scratchaddr, _vm_high, _xscratch);
			armAsm->St1(src.V2D(), 1, MemOperand(_scratchaddr));
		}
		else
		{
			WritePixel(src, addr, 0, psm);
			WritePixel(src, addr, 1, psm);
			WritePixel(src, addr, 2, psm);
			WritePixel(src, addr, 3, psm);
		}
	}
	else
	{
		if (fast)
		{

			Label skip_low, skip_high;
			armAsm->Lsl(_wscratch, addr, 1);

			armAsm->Tst(mask, high ? 0x0F00 : 0x0F);
			armAsm->B(eq, &skip_low);
			armAsm->Str(src.D(), MemOperand(_vm, _xscratch));
			armAsm->Bind(&skip_low);

			armAsm->Tst(mask, high ? 0xF000 : 0xF0);
			armAsm->B(eq, &skip_high);
			armAsm->Add(_scratchaddr, _vm_high, _xscratch);
			armAsm->St1(src.V2D(), 1, MemOperand(_scratchaddr));
			armAsm->Bind(&skip_high);
		}
		else
		{

			Label skip_0, skip_1, skip_2, skip_3;

			armAsm->Tst(mask, high ? 0x0300 : 0x03);
			armAsm->B(eq, &skip_0);
			WritePixel(src, addr, 0, psm);
			armAsm->Bind(&skip_0);

			armAsm->Tst(mask, high ? 0x0c00 : 0x0c);
			armAsm->B(eq, &skip_1);
			WritePixel(src, addr, 1, psm);
			armAsm->Bind(&skip_1);

			armAsm->Tst(mask, high ? 0x3000 : 0x30);
			armAsm->B(eq, &skip_2);
			WritePixel(src, addr, 2, psm);
			armAsm->Bind(&skip_2);

			armAsm->Tst(mask, high ? 0xc000 : 0xc0);
			armAsm->B(eq, &skip_3);
			WritePixel(src, addr, 3, psm);
			armAsm->Bind(&skip_3);
		}
	}
}

static const int s_offsets[4] = {0, 2, 8, 10};

void GSDrawScanlineCodeGenerator::WritePixel(const VRegister& src, const Register& addr, u8 i, int psm)
{
	pxAssert(addr.IsW());
	armAsm->Lsl(_wscratch, addr, 1);
	armAsm->Add(_scratchaddr, _vm, s_offsets[i] * 2);

	switch (psm)
	{
		case 0:
			if (i == 0)
			{
				armAsm->Str(src.S(), MemOperand(_scratchaddr, _xscratch));
			}
			else
			{
				armAsm->Add(_scratchaddr, _scratchaddr, _xscratch);
				armAsm->St1(src.V4S(), i, MemOperand(_scratchaddr));
			}
			break;
		case 1:
			armAsm->Ldr(_wscratch2, MemOperand(_scratchaddr, _xscratch));

			armAsm->Mov(w5, src.V4S(), i);

			armAsm->Eor(w5, w5, _wscratch2);
			armAsm->And(w5, w5, 0xffffff);
			armAsm->Eor(_wscratch2, _wscratch2, w5);
			armAsm->Str(_wscratch2, MemOperand(_scratchaddr, _xscratch));

			break;
		case 2:
			armAsm->Umov(w5, src.V8H(), i * 2);
			armAsm->Strh(w5, MemOperand(_scratchaddr, _xscratch));
			break;
	}
}

void GSDrawScanlineCodeGenerator::ReadTexel1(const VRegister& dst, const VRegister& src, const VRegister& tmp1, int mip_offset)
{
	const VRegister no;
	ReadTexelImpl(dst, tmp1, src, no, no, no, 1, mip_offset);
}

void GSDrawScanlineCodeGenerator::ReadTexel4(
	const VRegister& d0, const VRegister& d1,
	const VRegister& d2s0, const VRegister& d3s1,
	const VRegister& s2, const VRegister& s3,
	int mip_offset)
{
	ReadTexelImpl(d0, d1, d2s0, d3s1, s2, s3, 4, mip_offset);
}

void GSDrawScanlineCodeGenerator::ReadTexelImplLoadTexLOD(const Register& addr, int lod, int mip_offset)
{
	pxAssert(addr.IsX());
	pxAssert(m_sel.mmin);
	armAsm->Ldr(addr.W(), m_sel.lcm ? _global(lod.i.U32[lod]) : _local(temp.lod.i.U32[lod]));
	if (mip_offset != 0)
		armAsm->Add(addr.W(), addr.W(), mip_offset);
	armAsm->Ldr(addr.X(), MemOperand(_global_tex0, addr, LSL, 3));
}

void GSDrawScanlineCodeGenerator::ReadTexelImpl(
	const VRegister& d0, const VRegister& d1,
	const VRegister& d2s0, const VRegister& d3s1,
	const VRegister& s2, const VRegister& s3,
	int pixels, int mip_offset)
{

	const bool preserve[] = {false, false, true, true};
	const VRegister dst[] = {d0, d1, d2s0, d3s1};
	const VRegister src[] = {d2s0, d3s1, s2, s3};

	if (m_sel.mmin && !m_sel.lcm)
	{
		for (int j = 0; j < 4; j++)
		{
			ReadTexelImplLoadTexLOD(_xscratch, j, mip_offset);

			for (int i = 0; i < pixels; i++)
			{
				ReadTexelImpl(dst[i], src[i], j, _xscratch, preserve[i]);
			}
		}
	}
	else
	{
		Register base_register(_global_tex0);

		if (m_sel.mmin && m_sel.lcm)
		{
			ReadTexelImplLoadTexLOD(_xscratch, 0, mip_offset);
			base_register = _xscratch;
		}

		for (int i = 0; i < pixels; i++)
		{
			for (int j = 0; j < 4; j++)
			{
				ReadTexelImpl(dst[i], src[i], j, base_register, false);
			}
		}
	}
}

void GSDrawScanlineCodeGenerator::ReadTexelImpl(const VRegister& dst,
	const VRegister& addr, u8 i, const Register& baseRegister, bool preserveDst)
{
	pxAssert(baseRegister.GetCode() != _scratchaddr.GetCode());
	pxAssert(baseRegister.IsX());
	armAsm->Mov(_scratchaddr.W(), addr.V4S(), i);

	if (m_sel.tlu)
	{
		armAsm->Ldrb(_scratchaddr.W(), MemOperand(baseRegister, _scratchaddr));

		armAsm->Add(_scratchaddr, _global_clut, Operand(_scratchaddr, UXTW, 2));
		if (i == 0 && !preserveDst)
			armAsm->Ldr(dst.S(), MemOperand(_scratchaddr));
		else
			armAsm->Ld1(dst.V4S(), i, MemOperand(_scratchaddr));
	}
	else
	{
		armAsm->Add(_scratchaddr, baseRegister, Operand(_scratchaddr, UXTW, 2));
		if (i == 0 && !preserveDst)
			armAsm->Ldr(dst.S(), MemOperand(_scratchaddr));
		else
			armAsm->Ld1(dst.V4S(), i, MemOperand(_scratchaddr));
	}
}


void GSDrawScanlineCodeGenerator::modulate16(const VRegister& a, const VRegister& f, u8 shift)
{
	modulate16(a, a, f, shift);
}

void GSDrawScanlineCodeGenerator::modulate16(const VRegister& d, const VRegister& a, const VRegister& f, u8 shift)
{
	if (shift)
	{
		armAsm->Shl(d.V8H(), a.V8H(), shift);
		armAsm->Sqdmulh(d.V8H(), d.V8H(), f.V8H());
	}
	else
	{
		armAsm->Sqdmulh(a.V8H(), d.V8H(), f.V8H());
	}
}

void GSDrawScanlineCodeGenerator::lerp16(const VRegister& a, const VRegister& b, const VRegister& f, u8 shift)
{
	armAsm->Sub(a.V8H(), a.V8H(), b.V8H());
	modulate16(a, f, shift);
	armAsm->Add(a.V8H(), a.V8H(), b.V8H());
}

void GSDrawScanlineCodeGenerator::lerp16_4(const VRegister& a, const VRegister& b, const VRegister& f)
{
	armAsm->Sub(a.V8H(), a.V8H(), b.V8H());
	armAsm->Mul(a.V8H(), a.V8H(), f.V8H());
	armAsm->Sshr(a.V8H(), a.V8H(), 4);
	armAsm->Add(a.V8H(), a.V8H(), b.V8H());
}

void GSDrawScanlineCodeGenerator::mix16(const VRegister& a, const VRegister& b, const VRegister& temp)
{
	pxAssert(a.GetCode() != temp.GetCode() && b.GetCode() != temp.GetCode());

	armAsm->Mov(temp, a);
	armAsm->Movi(a.V4S(), 0xFFFF0000);
	armAsm->Bsl(a.V16B(), b.V16B(), temp.V16B());
}

void GSDrawScanlineCodeGenerator::clamp16(const VRegister& a, const VRegister& temp)
{
	armAsm->Sqxtun(a.V8B(), a.V8H());
	armAsm->Ushll(a.V8H(), a.V8B(), 0);
}

void GSDrawScanlineCodeGenerator::alltrue(const VRegister& test, const VRegister& temp)
{
	armAsm->Uminv(temp.S(), test.V4S());
	armAsm->Fmov(_wscratch, temp.S());
	armAsm->Cmn(_wscratch, 1);
	armAsm->B(eq, &m_step_label);
}

void GSDrawScanlineCodeGenerator::blend8(const VRegister& a, const VRegister& b, const VRegister& mask, const VRegister& temp)
{
	armAsm->Sshr(temp.V16B(), mask.V16B(), 7);
	armAsm->Bsl(temp.V16B(), b.V16B(), a.V16B());
	armAsm->Mov(a, temp);
}

void GSDrawScanlineCodeGenerator::blend8r(const VRegister& b, const VRegister& a, const VRegister& mask, const VRegister& temp)
{
	armAsm->Sshr(temp.V16B(), mask.V16B(), 7);
	armAsm->Bsl(temp.V16B(), b.V16B(), a.V16B());
	armAsm->Mov(b, temp);
}

void GSDrawScanlineCodeGenerator::split16_2x8(const VRegister& l, const VRegister& h, const VRegister& src)
{

	if (src.GetCode() == h.GetCode())
	{
		armAsm->Mov(l, src);
		armAsm->Ushr(h.V8H(), src.V8H(), 8);
		armAsm->Bic(l.V8H(), 0xFF, 8);
	}
	else if (src.GetCode() == l.GetCode())
	{
		armAsm->Ushr(h.V8H(), src.V8H(), 8);
		armAsm->Bic(l.V8H(), 0xFF, 8);
	}
	else
	{
		armAsm->Mov(l, src);
		armAsm->Ushr(h.V8H(), src.V8H(), 8);
		armAsm->Bic(l.V8H(), 0xFF, 8);
	}
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
