// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0

#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include "common/StringUtil.h"
#include "common/Perf.h"

#include <cstdint>

MULTI_ISA_UNSHARED_IMPL;

using namespace vixl::aarch64;

static const auto& _vertex = x0;
static const auto& _index = x1;
static const auto& _dscan = x2;
static const auto& _locals = x3;
static const auto& _scratchaddr = x7;
static const auto& _vscratch = v31;

static constexpr const GSScanlineConstantData128B& g_const = g_const_128b;

#define OFFSETOF(base, field) (reinterpret_cast<uptr>(&reinterpret_cast<base*>(0)->field))
#define _local(field) MemOperand(_locals, OFFSETOF(GSScanlineLocalData, field))
#define armAsm (&m_emitter)

GSSetupPrimCodeGenerator::GSSetupPrimCodeGenerator(u64 key, void* code, size_t maxsize)
	: m_emitter(static_cast<vixl::byte*>(code), maxsize, vixl::aarch64::PositionDependentCode)
	, m_sel(key)
{
	m_en.z = m_sel.zb ? 1 : 0;
	m_en.f = m_sel.fb && m_sel.fge ? 1 : 0;
	m_en.t = m_sel.fb && m_sel.tfx != TFX_NONE ? 1 : 0;
	m_en.c = m_sel.fb && !(m_sel.tfx == TFX_DECAL && m_sel.tcc) ? 1 : 0;
}

void GSSetupPrimCodeGenerator::Generate()
{
	const bool needs_shift = ((m_en.z || m_en.f) && m_sel.prim != GS_SPRITE_CLASS) || m_en.t || (m_en.c && m_sel.iip);
	if (needs_shift)
	{
		armAsm->Mov(x4, reinterpret_cast<intptr_t>(g_const.m_shift));
		for (int i = 0; i < (m_sel.notest ? 2 : 5); i++)
		{
			armAsm->Ldr(VRegister(3 + i, kFormat16B), MemOperand(x4, i * sizeof(g_const.m_shift[0])));
		}
	}

	Depth();

	Texture();

	Color();

	armAsm->Ret();

	armAsm->FinalizeCode();

	Perf::any.RegisterKey(GetCode(), GetSize(), "GSSetupPrim_", m_sel.key);
}

void GSSetupPrimCodeGenerator::Depth()
{
	if (!m_en.z && !m_en.f)
	{
		return;
	}

	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		if (m_en.f)
		{
			armAsm->Add(_scratchaddr, _dscan, offsetof(GSVertexSW, t.w));
			armAsm->Ld1r(v1.V4S(), MemOperand(_scratchaddr));

			armAsm->Fmul(v2.V4S(), v1.V4S(), v3.V4S());
			armAsm->Fcvtzs(v2.V4S(), v2.V4S());
			armAsm->Trn1(v2.V8H(), v2.V8H(), v2.V8H());

			armAsm->Str(v2.V4S(), _local(d4.f));

			for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
			{

				armAsm->Fmul(v2.V4S(), v1.V4S(), VRegister(4 + i, kFormat4S));
				armAsm->Fcvtzs(v2.V4S(), v2.V4S());
				armAsm->Trn1(v2.V8H(), v2.V8H(), v2.V8H());

				armAsm->Str(v2.V4S(), _local(d[i].f));
			}
		}

		if (m_en.z)
		{
			armAsm->Add(_scratchaddr, _dscan, offsetof(GSVertexSW, p.z));
			armAsm->Ld1r(_vscratch.V2D(), MemOperand(_scratchaddr));

			armAsm->Fcvtl(v1.V2D(), v3.V2S());
			armAsm->Fmul(v1.V2D(), v1.V2D(), _vscratch.V2D());
			armAsm->Str(v1.V2D(), _local(d4.z));

			armAsm->Fcvtn(v0.V2S(), _vscratch.V2D());
			armAsm->Fcvtn2(v0.V4S(), _vscratch.V2D());

			for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
			{

				armAsm->Fmul(v1.V4S(), v0.V4S(), VRegister(4 + i, kFormat4S));
				armAsm->Str(v1.V4S(), _local(d[i].z));
			}
		}
	}
	else
	{

		armAsm->Ldrh(w4, MemOperand(_index, sizeof(u16)));
		armAsm->Lsl(w4, w4, 6);
		armAsm->Add(x4, _vertex, x4);

		if (m_en.f)
		{

			armAsm->Ldr(v0, MemOperand(x4, offsetof(GSVertexSW, p)));

			armAsm->Fcvtzs(v1.V4S(), v0.V4S());
			armAsm->Dup(v1.V8H(), v1.V8H(), 6);

			armAsm->Str(v1, MemOperand(_locals, offsetof(GSScanlineLocalData, p.f)));
		}

		if (m_en.z)
		{

			armAsm->Add(_scratchaddr, x4, offsetof(GSVertexSW, t.w));
			armAsm->Ld1r(v0.V4S(), MemOperand(_scratchaddr));
			armAsm->Str(v0, MemOperand(_locals, offsetof(GSScanlineLocalData, p.z)));
		}
	}
}

void GSSetupPrimCodeGenerator::Texture()
{
	if (!m_en.t)
	{
		return;
	}

	armAsm->Ldr(v0, MemOperand(_dscan, offsetof(GSVertexSW, t)));
	armAsm->Fmul(v1.V4S(), v0.V4S(), v3.V4S());

	if (m_sel.fst)
	{
		armAsm->Fcvtzs(v1.V4S(), v1.V4S());
		armAsm->Str(v1, MemOperand(_locals, offsetof(GSScanlineLocalData, d4.stq)));
	}
	else
	{
		armAsm->Str(v1, MemOperand(_locals, offsetof(GSScanlineLocalData, d4.stq)));
	}

	for (int j = 0, k = m_sel.fst ? 2 : 3; j < k; j++)
	{

		armAsm->Dup(v1.V4S(), v0.V4S(), j);

		for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
		{

			armAsm->Fmul(v2.V4S(), v1.V4S(), VRegister(4 + i, 128, 4));

			if (m_sel.fst)
			{

				armAsm->Fcvtzs(v2.V4S(), v2.V4S());

				switch (j)
				{
					case 0: armAsm->Str(v2, _local(d[i].s)); break;
					case 1: armAsm->Str(v2, _local(d[i].t)); break;
				}
			}
			else
			{

				switch (j)
				{
					case 0: armAsm->Str(v2, _local(d[i].s)); break;
					case 1: armAsm->Str(v2, _local(d[i].t)); break;
					case 2: armAsm->Str(v2, _local(d[i].q)); break;
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
		armAsm->Ldr(v16, MemOperand(_dscan, offsetof(GSVertexSW, c)));

		armAsm->Fmul(v2.V4S(), v16.V4S(), v3.V4S());
		armAsm->Fcvtzs(v2.V4S(), v2.V4S());
		armAsm->Rev64(_vscratch.V4S(), v2.V4S());
		armAsm->Uzp1(v2.V4S(), v2.V4S(), _vscratch.V4S());
		armAsm->Uzp1(v2.V8H(), v2.V8H(), v2.V8H());
		armAsm->Str(v2, MemOperand(_locals, offsetof(GSScanlineLocalData, d4.c)));

		armAsm->Dup(v0.V4S(), v16.V4S(), 0);
		armAsm->Dup(v1.V4S(), v16.V4S(), 2);

		for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
		{

			armAsm->Fmul(v2.V4S(), v0.V4S(), VRegister(4 + i, kFormat4S));
			armAsm->Fcvtzs(v2.V4S(), v2.V4S());

			armAsm->Fmul(v3.V4S(), v1.V4S(), VRegister(4 + i, kFormat4S));
			armAsm->Fcvtzs(v3.V4S(), v3.V4S());

			armAsm->Trn1(v2.V8H(), v2.V8H(), v3.V8H());
			armAsm->Str(v2, _local(d[i].rb));
		}

		armAsm->Dup(v0.V4S(), v16.V4S(), 1);
		armAsm->Dup(v1.V4S(), v16.V4S(), 3);

		for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
		{

			armAsm->Fmul(v2.V4S(), v0.V4S(), VRegister(4 + i, kFormat4S));
			armAsm->Fcvtzs(v2.V4S(), v2.V4S());

			armAsm->Fmul(v3.V4S(), v1.V4S(), VRegister(4 + i, kFormat4S));
			armAsm->Fcvtzs(v3.V4S(), v3.V4S());

			armAsm->Trn1(v2.V8H(), v2.V8H(), v3.V8H());
			armAsm->Str(v2, _local(d[i].ga));
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
			armAsm->Ldrh(w4, MemOperand(_index, sizeof(u16) * last));
			armAsm->Lsl(w4, w4, 6);
			armAsm->Add(x4, _vertex, x4);
		}

		armAsm->Ldr(v0, MemOperand(x4, offsetof(GSVertexSW, c)));
		armAsm->Fcvtzs(v0.V4S(), v0.V4S());

		armAsm->Ext(v1.V16B(), v0.V16B(), v0.V16B(), 8);
		armAsm->Zip1(v0.V8H(), v0.V8H(), v1.V8H());

		if (m_sel.tfx == TFX_NONE)
			armAsm->Ushr(v0.V8H(), v0.V8H(), 7);

		armAsm->Dup(v1.V4S(), v0.V4S(), 0);
		armAsm->Dup(v2.V4S(), v0.V4S(), 2);

		armAsm->Str(v1, _local(c.rb));
		armAsm->Str(v2, _local(c.ga));
	}
}
