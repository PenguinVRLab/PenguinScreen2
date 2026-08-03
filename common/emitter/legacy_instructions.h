// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#if defined(__linux__) && defined(__clang__) && defined(SPAM_DEPRECATION_WARNINGS)
#define ATTR_DEP [[deprecated]]
#else
#define ATTR_DEP
#endif

#ifdef FSCALE
# undef FSCALE
#endif

ATTR_DEP extern void x86SetJ8(u8* j8);
ATTR_DEP extern void x86SetJ8A(u8* j8);
ATTR_DEP extern void x86SetJ16(u16* j16);
ATTR_DEP extern void x86SetJ16A(u16* j16);
ATTR_DEP extern void x86SetJ32(u32* j32);
ATTR_DEP extern void x86SetJ32A(u32* j32);

ATTR_DEP extern u8* JMP8(u8 to);

ATTR_DEP extern u32* JMP32(uptr to);

ATTR_DEP extern u8* JP8(u8 to);
ATTR_DEP extern u8* JNP8(u8 to);
ATTR_DEP extern u8* JE8(u8 to);
ATTR_DEP extern u8* JZ8(u8 to);
ATTR_DEP extern u8* JG8(u8 to);
ATTR_DEP extern u8* JGE8(u8 to);
ATTR_DEP extern u8* JS8(u8 to);
ATTR_DEP extern u8* JNS8(u8 to);
ATTR_DEP extern u8* JL8(u8 to);
ATTR_DEP extern u8* JA8(u8 to);
ATTR_DEP extern u8* JAE8(u8 to);
ATTR_DEP extern u8* JB8(u8 to);
ATTR_DEP extern u8* JBE8(u8 to);
ATTR_DEP extern u8* JLE8(u8 to);
ATTR_DEP extern u8* JNE8(u8 to);
ATTR_DEP extern u8* JNZ8(u8 to);
ATTR_DEP extern u8* JNG8(u8 to);
ATTR_DEP extern u8* JNGE8(u8 to);
ATTR_DEP extern u8* JNL8(u8 to);
ATTR_DEP extern u8* JNLE8(u8 to);
ATTR_DEP extern u8* JO8(u8 to);
ATTR_DEP extern u8* JNO8(u8 to);

ATTR_DEP extern u32* JNS32(u32 to);
ATTR_DEP extern u32* JS32(u32 to);

ATTR_DEP extern u32* JB32(u32 to);
ATTR_DEP extern u32* JE32(u32 to);
ATTR_DEP extern u32* JZ32(u32 to);
ATTR_DEP extern u32* JG32(u32 to);
ATTR_DEP extern u32* JGE32(u32 to);
ATTR_DEP extern u32* JL32(u32 to);
ATTR_DEP extern u32* JLE32(u32 to);
ATTR_DEP extern u32* JAE32(u32 to);
ATTR_DEP extern u32* JNE32(u32 to);
ATTR_DEP extern u32* JNZ32(u32 to);
ATTR_DEP extern u32* JNG32(u32 to);
ATTR_DEP extern u32* JNGE32(u32 to);
ATTR_DEP extern u32* JNL32(u32 to);
ATTR_DEP extern u32* JNLE32(u32 to);
ATTR_DEP extern u32* JO32(u32 to);
ATTR_DEP extern u32* JNO32(u32 to);
ATTR_DEP extern u32* JS32(u32 to);

ATTR_DEP extern void FLD32(u32 from);
ATTR_DEP extern void FLD(int st);
ATTR_DEP extern void FLD1();
ATTR_DEP extern void FLDL2E();
ATTR_DEP extern void FSTP32(u32 to);
ATTR_DEP extern void FSTP(int st);

ATTR_DEP extern void FRNDINT();
ATTR_DEP extern void FXCH(int st);
ATTR_DEP extern void F2XM1();
ATTR_DEP extern void FSCALE();

ATTR_DEP extern void FADD320toR(x86IntRegType src);
ATTR_DEP extern void FSUB32Rto0(x86IntRegType src);

ATTR_DEP extern void FMUL32(u32 from);
ATTR_DEP extern void FDIV32(u32 from);
ATTR_DEP extern void FPATAN(void);
ATTR_DEP extern void FSIN(void);

ATTR_DEP extern void SSE_MAXSS_XMM_to_XMM(x86SSERegType to, x86SSERegType from);
ATTR_DEP extern void SSE_MINSS_XMM_to_XMM(x86SSERegType to, x86SSERegType from);
ATTR_DEP extern void SSE_ADDSS_XMM_to_XMM(x86SSERegType to, x86SSERegType from);
ATTR_DEP extern void SSE_SUBSS_XMM_to_XMM(x86SSERegType to, x86SSERegType from);

ATTR_DEP extern void SSE2_MAXSD_XMM_to_XMM(x86SSERegType to, x86SSERegType from);
ATTR_DEP extern void SSE2_MINSD_XMM_to_XMM(x86SSERegType to, x86SSERegType from);
ATTR_DEP extern void SSE2_ADDSD_XMM_to_XMM(x86SSERegType to, x86SSERegType from);
ATTR_DEP extern void SSE2_SUBSD_XMM_to_XMM(x86SSERegType to, x86SSERegType from);
