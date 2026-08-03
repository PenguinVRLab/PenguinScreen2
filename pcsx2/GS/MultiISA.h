// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#ifdef MULTI_ISA_UNSHARED_COMPILATION
	#define CURRENT_ISA MULTI_ISA_UNSHARED_COMPILATION
#else
	#define CURRENT_ISA isa_native
#endif

#if defined(MULTI_ISA_UNSHARED_COMPILATION)
	#define MULTI_ISA_COMPILE_ONCE MULTI_ISA_IS_FIRST
#elif !defined(MULTI_ISA_SHARED_COMPILATION)
	#define MULTI_ISA_COMPILE_ONCE 1
#else
	#define MULTI_ISA_COMPILE_ONCE static_assert(0, "You don't need MULTI_ISA_COMPILE_ONCE in a non-multi-isa file!");
#endif

#ifndef MULTI_ISA_SHARED_COMPILATION
	#define MULTI_ISA_UNSHARED_START namespace CURRENT_ISA {
	#define MULTI_ISA_UNSHARED_END }
	#define MULTI_ISA_UNSHARED_IMPL using namespace CURRENT_ISA
#else
	#define MULTI_ISA_UNSHARED_START static_assert(0, "This file should not be included by multi-isa shared compilation!");
	#define MULTI_ISA_UNSHARED_IMPL static_assert(0, "This file should be compiled unshared in multi-isa mode!");
	#define MULTI_ISA_UNSHARED_END
#endif

struct ProcessorFeatures
{
#ifdef _M_X86
	enum class VectorISA { SSE4, AVX, AVX2, AVX512F };
	VectorISA vectorISA;
	bool hasFMA;
	bool hasBMI2;
	bool hasSlowGather;
#endif
};

extern const ProcessorFeatures g_cpu;

#if defined(MULTI_ISA_UNSHARED_COMPILATION) || defined(MULTI_ISA_SHARED_COMPILATION)
	#define MULTI_ISA_DEF(...) \
		namespace isa_sse4 { __VA_ARGS__ } \
		namespace isa_avx  { __VA_ARGS__ } \
		namespace isa_avx2 { __VA_ARGS__ }

	#define MULTI_ISA_FRIEND(klass) \
		friend class isa_sse4::klass; \
		friend class isa_avx ::klass; \
		friend class isa_avx2::klass;

	#define MULTI_ISA_SELECT(fn) (\
		::g_cpu.vectorISA == ProcessorFeatures::VectorISA::AVX2 ? isa_avx2::fn : \
		::g_cpu.vectorISA == ProcessorFeatures::VectorISA::AVX  ? isa_avx ::fn : \
		                                                          isa_sse4::fn)
#else
	#define MULTI_ISA_DEF(...) namespace isa_native { __VA_ARGS__ }
	#define MULTI_ISA_FRIEND(klass) friend class isa_native::klass;
	#define MULTI_ISA_SELECT(fn) (isa_native::fn)
#endif

class GSRenderer;
MULTI_ISA_DEF(GSRenderer* makeGSRendererSW(int threads);)

namespace MultiISAFunctions
{
	extern u64 (&GSXXH3_64_Long)(const void* data, size_t len);
	extern u32 (&GSXXH3_64_Update)(void* state, const void* data, size_t len);
	extern u64 (&GSXXH3_64_Digest)(void* state);
}
