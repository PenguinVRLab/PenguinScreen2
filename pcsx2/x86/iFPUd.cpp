// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "common/emitter/x86emitter.h"
#include "iR5900.h"
#include "iFPU.h"

using namespace x86Emitter;

#define FPU_FLAGS_OVERFLOW 1
#define FPU_FLAGS_UNDERFLOW 1

#define FPU_RESULT 1

#define FPU_FLAGS_ID 1

#define FPU_CORRECT_ADD_SUB 1

#ifdef FPU_RECOMPILE

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
namespace COP1 {

namespace DOUBLE {

#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

#define FPUflagC  0x00800000
#define FPUflagI  0x00020000
#define FPUflagD  0x00010000
#define FPUflagO  0x00008000
#define FPUflagU  0x00004000
#define FPUflagSI 0x00000040
#define FPUflagSD 0x00000020
#define FPUflagSO 0x00000010
#define FPUflagSU 0x00000008

#define SINGLE(sign, exp, mant) (((u32)(sign) << 31) | ((u32)(exp) << 23) | (u32)(mant))
#define DOUBLE(sign, exp, mant) (((sign##ULL) << 63) | ((exp##ULL) << 52) | (mant##ULL))

struct FPUd_Globals
{
	u32 neg[4], pos[4];

	u32 pos_inf[4], neg_inf[4],
	    one_exp[4];

	u64 dbl_one_exp[2];

	u64 dbl_cvt_overflow,
	    dbl_ps2_overflow,
	    dbl_underflow;

	u64 padding;

	u64 dbl_s_pos[2];
};

alignas(32) static const FPUd_Globals s_const =
{
	{0x80000000, 0xffffffff, 0xffffffff, 0xffffffff},
	{0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff},

	{SINGLE(0, 0xff, 0), 0, 0, 0},
	{SINGLE(1, 0xff, 0), 0, 0, 0},
	{SINGLE(0,    1, 0), 0, 0, 0},

	{DOUBLE(0, 1, 0), 0},

	DOUBLE(0, 1151, 0),
	DOUBLE(0, 1152, 0),
	DOUBLE(0,  897, 0),

	0,

	{0x7fffffffffffffffULL, 0},
};


void ToDouble(int reg)
{
	xUCOMI.SS(xRegisterSSE(reg), ptr[s_const.pos_inf]);
	u8* to_complex = JE8(0);
	xUCOMI.SS(xRegisterSSE(reg), ptr[s_const.neg_inf]);
	u8* to_complex2 = JE8(0);

	xCVTSS2SD(xRegisterSSE(reg), xRegisterSSE(reg));
	u8* end = JMP8(0);

	x86SetJ8(to_complex);
	x86SetJ8(to_complex2);

	xPSUB.D(xRegisterSSE(reg), ptr[s_const.one_exp]);
	xCVTSS2SD(xRegisterSSE(reg), xRegisterSSE(reg));
	xPADD.Q(xRegisterSSE(reg), ptr[s_const.dbl_one_exp]);

	x86SetJ8(end);
}

void ToPS2FPU_Full(int reg, bool flags, int absreg, bool acc, bool addsub)
{
	if (flags)
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO | FPUflagU));
	if (flags && acc)
		xAND(ptr32[&fpuRegs.ACCflag], ~1);

	xAND.PD(xRegisterSSE(absreg), xRegisterSSE(reg), ptr[&s_const.dbl_s_pos]);

	xUCOMI.SD(xRegisterSSE(absreg), ptr[&s_const.dbl_cvt_overflow]);
	u8* to_complex = JAE8(0);

	xUCOMI.SD(xRegisterSSE(absreg), ptr[&s_const.dbl_underflow]);
	u8* to_underflow = JB8(0);

	xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg));

	u32* end = JMP32(0);

	x86SetJ8(to_complex);
	xUCOMI.SD(xRegisterSSE(absreg), ptr[&s_const.dbl_ps2_overflow]);
	u8* to_overflow = JAE8(0);

	xPSUB.Q(xRegisterSSE(reg), ptr[&s_const.dbl_one_exp]);
	xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg));
	xPADD.D(xRegisterSSE(reg), ptr[s_const.one_exp]);

	u32* end2 = JMP32(0);

	x86SetJ8(to_overflow);
	xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg));
	xOR.PS(xRegisterSSE(reg), ptr[&s_const.pos]);
	if (flags && FPU_FLAGS_OVERFLOW)
		xOR(ptr32[&fpuRegs.fprc[31]], (FPUflagO | FPUflagSO));
	if (flags && FPU_FLAGS_OVERFLOW && acc)
		xOR(ptr32[&fpuRegs.ACCflag], 1);
	u8* end3 = JMP8(0);

	x86SetJ8(to_underflow);
	u8* end4 = nullptr;
	if (flags && FPU_FLAGS_UNDERFLOW)
	{
		xXOR.PD(xRegisterSSE(absreg), xRegisterSSE(absreg));
		xUCOMI.SD(xRegisterSSE(reg), xRegisterSSE(absreg));
		u8* is_zero = JE8(0);

		xOR(ptr32[&fpuRegs.fprc[31]], (FPUflagU | FPUflagSU));
		if (addsub)
		{
			xMOVAPS(xRegisterSSE(absreg), xRegisterSSE(reg));
			xPSLL.Q(xRegisterSSE(reg), 12);
			xPSRL.Q(xRegisterSSE(reg), 41);
			xPSRL.Q(xRegisterSSE(absreg), 63);
			xPSLL.Q(xRegisterSSE(absreg), 31);
			xPOR(xRegisterSSE(reg), xRegisterSSE(absreg));
			end4 = JMP8(0);
		}

		x86SetJ8(is_zero);
	}
	xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg));
	xAND.PS(xRegisterSSE(reg), ptr[s_const.neg]);

	x86SetJ32(end);
	x86SetJ32(end2);

	x86SetJ8(end3);
	if (flags && FPU_FLAGS_UNDERFLOW && addsub)
		x86SetJ8(end4);
}

void ToPS2FPU(int reg, bool flags, int absreg, bool acc, bool addsub = false)
{
	if (FPU_RESULT)
		ToPS2FPU_Full(reg, flags, absreg, acc, addsub);
	else
	{
		xCVTSD2SS(xRegisterSSE(reg), xRegisterSSE(reg));
		xMIN.SS(xRegisterSSE(reg), ptr[&g_maxvals[0]]);
		xMAX.SS(xRegisterSSE(reg), ptr[&g_minvals[0]]);
	}
}

void SetMaxValue(int regd)
{
	if (FPU_RESULT)
		xOR.PS(xRegisterSSE(regd), ptr[&s_const.pos[0]]);
	else
	{
		xAND.PS(xRegisterSSE(regd), ptr[&s_const.neg[0]]);
		xOR.PS(xRegisterSSE(regd), ptr[&g_maxvals[0]]);
	}
}

#define GET_S(sreg) \
	do { \
		if (info & PROCESS_EE_S) \
			xMOVSS(xRegisterSSE(sreg), xRegisterSSE(EEREC_S)); \
		else \
			xMOVSSZX(xRegisterSSE(sreg), ptr[&fpuRegs.fpr[_Fs_]]); \
	} while (0)

#define ALLOC_S(sreg) \
	do { \
		(sreg) = _allocTempXMMreg(XMMT_FPS); \
		GET_S(sreg); \
	} while (0)

#define GET_T(treg) \
	do { \
		if (info & PROCESS_EE_T) \
			xMOVSS(xRegisterSSE(treg), xRegisterSSE(EEREC_T)); \
		else \
			xMOVSSZX(xRegisterSSE(treg), ptr[&fpuRegs.fpr[_Ft_]]); \
	} while (0)

#define ALLOC_T(treg) \
	do { \
		(treg) = _allocTempXMMreg(XMMT_FPS); \
		GET_T(treg); \
	} while (0)

#define GET_ACC(areg) \
	do { \
		if (info & PROCESS_EE_ACC) \
			xMOVSS(xRegisterSSE(areg), xRegisterSSE(EEREC_ACC)); \
		else \
			xMOVSSZX(xRegisterSSE(areg), ptr[&fpuRegs.ACC]); \
	} while (0)

#define ALLOC_ACC(areg) \
	do { \
		(areg) = _allocTempXMMreg(XMMT_FPS); \
		GET_ACC(areg); \
	} while (0)

#define CLEAR_OU_FLAGS \
	do { \
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO | FPUflagU)); \
	} while (0)


void recABS_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::ABS_F);
	GET_S(EEREC_D);

	CLEAR_OU_FLAGS;

	xAND.PS(xRegisterSSE(EEREC_D), ptr[s_const.pos]);
}

FPURECOMPILE_CONSTCODE(ABS_S, XMMINFO_WRITED | XMMINFO_READS);


void FPU_ADD_SUB(int tempd, int tempt)
{
	const int xmmtemp = _allocTempXMMreg(XMMT_FPS);
	xMOVD(ecx, xRegisterSSE(tempd));
	xMOVD(eax, xRegisterSSE(tempt));

	xSHR(ecx, 23);
	xSHR(eax, 23);
	xAND(ecx, 0xff);
	xAND(eax, 0xff);

	xSUB(ecx, eax);
	xCMP(ecx, 25);
	j8Ptr[0] = JGE8(0);
	xCMP(ecx, 0);
	j8Ptr[1] = JG8(0);
	j8Ptr[2] = JE8(0);
	xCMP(ecx, -25);
	j8Ptr[3] = JLE8(0);

	xNEG(ecx);
	xDEC(ecx);
	xMOV(eax, 0xffffffff);
	xSHL(eax, cl);
	xMOVDZX(xRegisterSSE(xmmtemp), eax);
	xAND.PS(xRegisterSSE(tempd), xRegisterSSE(xmmtemp));
	j8Ptr[4] = JMP8(0);

	x86SetJ8(j8Ptr[0]);
	xAND.PS(xRegisterSSE(tempt), ptr[s_const.neg]);
	j8Ptr[5] = JMP8(0);

	x86SetJ8(j8Ptr[1]);
	xDEC(ecx);
	xMOV(eax, 0xffffffff);
	xSHL(eax, cl);
	xMOVDZX(xRegisterSSE(xmmtemp), eax);
	xAND.PS(xRegisterSSE(tempt), xRegisterSSE(xmmtemp));
	j8Ptr[6] = JMP8(0);

	x86SetJ8(j8Ptr[3]);
	xAND.PS(xRegisterSSE(tempd), ptr[s_const.neg]);

	x86SetJ8(j8Ptr[2]);

	x86SetJ8(j8Ptr[4]);
	x86SetJ8(j8Ptr[5]);
	x86SetJ8(j8Ptr[6]);

	_freeXMMreg(xmmtemp);
}

void FPU_MUL(int info, int regd, int sreg, int treg, bool acc)
{
	u32* endMul = nullptr;

	if (CHECK_FPUMULHACK)
	{

		alignas(16) static constexpr const u32 result[4] = { 0x3f490fda };

		xMOVD(ecx, xRegisterSSE(sreg));
		xMOVD(edx, xRegisterSSE(treg));

		xXOR(ecx, 0x3e800000);
		xXOR(edx, 0x40490fdb);
		xOR(edx, ecx);

		u8* noHack = JNZ8(0);
			xMOVAPS(xRegisterSSE(regd), ptr128[result]);
			endMul = JMP32(0);
		x86SetJ8(noHack);
	}

	ToDouble(sreg); ToDouble(treg);
	xMUL.SD(xRegisterSSE(sreg), xRegisterSSE(treg));
	ToPS2FPU(sreg, true, treg, acc);
	xMOVSS(xRegisterSSE(regd), xRegisterSSE(sreg));

	if (CHECK_FPUMULHACK)
		x86SetJ32(endMul);
}

static void (*recFPUOpXMM_to_XMM[])(x86SSERegType, x86SSERegType) = {
	SSE2_ADDSD_XMM_to_XMM, SSE2_SUBSD_XMM_to_XMM};

void recFPUOp(int info, int regd, int op, bool acc)
{
	int sreg, treg;
	ALLOC_S(sreg); ALLOC_T(treg);

	if (FPU_CORRECT_ADD_SUB)
		FPU_ADD_SUB(sreg, treg);

	ToDouble(sreg); ToDouble(treg);

	recFPUOpXMM_to_XMM[op](sreg, treg);

	ToPS2FPU(sreg, true, treg, acc, true);
	xMOVSS(xRegisterSSE(regd), xRegisterSSE(sreg));

	_freeXMMreg(sreg); _freeXMMreg(treg);
}


void recADD_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::ADD_F);
	recFPUOp(info, EEREC_D, 0, false);
}

FPURECOMPILE_CONSTCODE(ADD_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recADDA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::ADDA_F);
	recFPUOp(info, EEREC_ACC, 0, true);
}

FPURECOMPILE_CONSTCODE(ADDA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);

void recCMP(int info)
{
	int sreg, treg;
	ALLOC_S(sreg); ALLOC_T(treg);
	ToDouble(sreg); ToDouble(treg);

	xUCOMI.SD(xRegisterSSE(sreg), xRegisterSSE(treg));

	_freeXMMreg(sreg); _freeXMMreg(treg);
}

void recC_EQ_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::CEQ_F);
	recCMP(info);

	j8Ptr[0] = JZ8(0);
		xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
		j8Ptr[1] = JMP8(0);
	x86SetJ8(j8Ptr[0]);
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	x86SetJ8(j8Ptr[1]);
}

FPURECOMPILE_CONSTCODE(C_EQ, XMMINFO_READS | XMMINFO_READT);

void recC_LE_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::CLE_F);
	recCMP(info);

	j8Ptr[0] = JBE8(0);
		xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
		j8Ptr[1] = JMP8(0);
	x86SetJ8(j8Ptr[0]);
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	x86SetJ8(j8Ptr[1]);
}

FPURECOMPILE_CONSTCODE(C_LE, XMMINFO_READS | XMMINFO_READT);

void recC_LT_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::CLT_F);
	recCMP(info);

	j8Ptr[0] = JB8(0);
		xAND(ptr32[&fpuRegs.fprc[31]], ~FPUflagC);
		j8Ptr[1] = JMP8(0);
	x86SetJ8(j8Ptr[0]);
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagC);
	x86SetJ8(j8Ptr[1]);
}

FPURECOMPILE_CONSTCODE(C_LT, XMMINFO_READS | XMMINFO_READT);


void recDIVhelper1(int regd, int regt)
{
	u8 *pjmp1, *pjmp2;
	u32 *ajmp32, *bjmp32;
	const int t1reg = _allocTempXMMreg(XMMT_FPS);

	xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD));

	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regt));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1);
	ajmp32 = JZ32(0);

		xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
		xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regd));
		xMOVMSKPS(eax, xRegisterSSE(t1reg));
		xAND(eax, 1);
		pjmp1 = JZ8(0);
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI);
			pjmp2 = JMP8(0);
		x86SetJ8(pjmp1);
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD);
		x86SetJ8(pjmp2);

		xXOR.PS(xRegisterSSE(regd), xRegisterSSE(regt));
		SetMaxValue(regd);
		bjmp32 = JMP32(0);

	x86SetJ32(ajmp32);

	ToDouble(regd); ToDouble(regt);

	xDIV.SD(xRegisterSSE(regd), xRegisterSSE(regt));

	ToPS2FPU(regd, false, regt, false);

	x86SetJ32(bjmp32);

	_freeXMMreg(t1reg);
}

void recDIVhelper2(int regd, int regt)
{
	ToDouble(regd); ToDouble(regt);

	xDIV.SD(xRegisterSSE(regd), xRegisterSSE(regt));

	ToPS2FPU(regd, false, regt, false);
}

alignas(16) static FPControlRegister roundmode_nearest;

void recDIV_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::DIV_F);

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUDivFPCR.bitmask]);

	int sreg, treg;

	ALLOC_S(sreg); ALLOC_T(treg);

	if (FPU_FLAGS_ID)
		recDIVhelper1(sreg, treg);
	else
		recDIVhelper2(sreg, treg);

	xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(sreg));

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);

	_freeXMMreg(sreg); _freeXMMreg(treg);
}

FPURECOMPILE_CONSTCODE(DIV_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);


void recMaddsub(int info, int regd, int op, bool acc)
{
	int sreg, treg;
	ALLOC_S(sreg); ALLOC_T(treg);

	FPU_MUL(info, sreg, sreg, treg, false);

	GET_ACC(treg);

	if (FPU_CORRECT_ADD_SUB)
		FPU_ADD_SUB(treg, sreg);

	xTEST(ptr32[&fpuRegs.fprc[31]], FPUflagO);
	u8* mulovf = JNZ8(0);
	ToDouble(sreg);

	xTEST(ptr32[&fpuRegs.ACCflag], 1);
	u8* accovf = JNZ8(0);
	ToDouble(treg);
	u8* operation = JMP8(0);

	x86SetJ8(mulovf);
	if (op == 1)
		xXOR.PS(xRegisterSSE(sreg), ptr[s_const.neg]);
	xMOVAPS(xRegisterSSE(treg), xRegisterSSE(sreg));

	x86SetJ8(accovf);
	SetMaxValue(treg);
	CLEAR_OU_FLAGS;
	if (FPU_FLAGS_OVERFLOW)
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagO | FPUflagSO);
	if (FPU_FLAGS_OVERFLOW && acc)
		xOR(ptr32[&fpuRegs.ACCflag], 1);
	u32* skipall = JMP32(0);

	x86SetJ8(operation);
	if (op == 1)
		xSUB.SD(xRegisterSSE(treg), xRegisterSSE(sreg));
	else
		xADD.SD(xRegisterSSE(treg), xRegisterSSE(sreg));

	ToPS2FPU(treg, true, sreg, acc, true);
	x86SetJ32(skipall);

	xMOVSS(xRegisterSSE(regd), xRegisterSSE(treg));

	_freeXMMreg(sreg); _freeXMMreg(treg);
}

void recMADD_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MADD_F);
	recMaddsub(info, EEREC_D, 0, false);
}

FPURECOMPILE_CONSTCODE(MADD_S, XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMADDA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MADDA_F);
	recMaddsub(info, EEREC_ACC, 0, true);
}

FPURECOMPILE_CONSTCODE(MADDA_S, XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);


alignas(16) static const u32 minmax_mask[8] =
{
	0xffffffff, 0x80000000, 0, 0,
	0,          0x40000000, 0, 0,
};
void recMINMAX(int info, bool ismin)
{
	int sreg, treg;
	ALLOC_S(sreg); ALLOC_T(treg);

	CLEAR_OU_FLAGS;

	xPSHUF.D(xRegisterSSE(sreg), xRegisterSSE(sreg), 0x00);
	xPAND(xRegisterSSE(sreg), ptr[minmax_mask]);
	xPOR(xRegisterSSE(sreg), ptr[&minmax_mask[4]]);
	xPSHUF.D(xRegisterSSE(treg), xRegisterSSE(treg), 0x00);
	xPAND(xRegisterSSE(treg), ptr[minmax_mask]);
	xPOR(xRegisterSSE(treg), ptr[&minmax_mask[4]]);
	if (ismin)
		xMIN.SD(xRegisterSSE(sreg), xRegisterSSE(treg));
	else
		xMAX.SD(xRegisterSSE(sreg), xRegisterSSE(treg));

	xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(sreg));

	_freeXMMreg(sreg); _freeXMMreg(treg);
}

void recMAX_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MAX_F);
	recMINMAX(info, false);
}

FPURECOMPILE_CONSTCODE(MAX_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMIN_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MIN_F);
	recMINMAX(info, true);
}

FPURECOMPILE_CONSTCODE(MIN_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);


void recMOV_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MOV_F);
	GET_S(EEREC_D);
}

FPURECOMPILE_CONSTCODE(MOV_S, XMMINFO_WRITED | XMMINFO_READS);


void recMSUB_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MSUB_F);
	recMaddsub(info, EEREC_D, 1, false);
}

FPURECOMPILE_CONSTCODE(MSUB_S, XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMSUBA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MSUBA_F);
	recMaddsub(info, EEREC_ACC, 1, true);
}

FPURECOMPILE_CONSTCODE(MSUBA_S, XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMUL_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MUL_F);
	int sreg, treg;
	ALLOC_S(sreg); ALLOC_T(treg);

	FPU_MUL(info, EEREC_D, sreg, treg, false);
	_freeXMMreg(sreg); _freeXMMreg(treg);
}

FPURECOMPILE_CONSTCODE(MUL_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMULA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::MULA_F);
	int sreg, treg;
	ALLOC_S(sreg); ALLOC_T(treg);

	FPU_MUL(info, EEREC_ACC, sreg, treg, true);
	_freeXMMreg(sreg); _freeXMMreg(treg);
}

FPURECOMPILE_CONSTCODE(MULA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);


void recNEG_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::NEG_F);
	GET_S(EEREC_D);

	CLEAR_OU_FLAGS;

	xXOR.PS(xRegisterSSE(EEREC_D), ptr[&s_const.neg[0]]);
}

FPURECOMPILE_CONSTCODE(NEG_S, XMMINFO_WRITED | XMMINFO_READS);


void recSUB_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::SUB_F);
	recFPUOp(info, EEREC_D, 1, false);
}

FPURECOMPILE_CONSTCODE(SUB_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);


void recSUBA_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::SUBA_F);
	recFPUOp(info, EEREC_ACC, 1, true);
}

FPURECOMPILE_CONSTCODE(SUBA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);


void recSQRT_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::SQRT_F);
	int roundmodeFlag = 0;
	const int t1reg = _allocTempXMMreg(XMMT_FPS);

	if (EmuConfig.Cpu.FPUFPCR.GetRoundMode() != FPRoundMode::Nearest)
	{
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest.SetRoundMode(FPRoundMode::Nearest);
		xLDMXCSR(ptr32[&roundmode_nearest.bitmask]);
		roundmodeFlag = 1;
	}

	GET_T(EEREC_D);

	if (FPU_FLAGS_ID)
	{
		xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD));

		xMOVMSKPS(eax, xRegisterSSE(EEREC_D));
		xAND(eax, 1);
		u8* pjmp = JZ8(0);
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI);
			xAND.PS(xRegisterSSE(EEREC_D), ptr[&s_const.pos[0]]);
		x86SetJ8(pjmp);
	}
	else
	{
		xAND.PS(xRegisterSSE(EEREC_D), ptr[&s_const.pos[0]]);
	}


	ToDouble(EEREC_D);

	xSQRT.SD(xRegisterSSE(EEREC_D), xRegisterSSE(EEREC_D));

	ToPS2FPU(EEREC_D, false, t1reg, false);

	if (roundmodeFlag == 1)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);

	_freeXMMreg(t1reg);
}

FPURECOMPILE_CONSTCODE(SQRT_S, XMMINFO_WRITED | XMMINFO_READT);


void recRSQRThelper1(int regd, int regt)
{
	u8 *pjmp1, *pjmp2;
	u8 *qjmp1, *qjmp2;
	u32* pjmp32;
	int t1reg = _allocTempXMMreg(XMMT_FPS);

	xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagI | FPUflagD));

	xMOVMSKPS(eax, xRegisterSSE(regt));
	xAND(eax, 1);
	pjmp2 = JZ8(0);
		xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI);
		xAND.PS(xRegisterSSE(regt), ptr[&s_const.pos[0]]);
	x86SetJ8(pjmp2);

	xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
	xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regt));
	xMOVMSKPS(eax, xRegisterSSE(t1reg));
	xAND(eax, 1);
	pjmp1 = JZ8(0);

		xXOR.PS(xRegisterSSE(t1reg), xRegisterSSE(t1reg));
		xCMPEQ.SS(xRegisterSSE(t1reg), xRegisterSSE(regd));
		xMOVMSKPS(eax, xRegisterSSE(t1reg));
		xAND(eax, 1);
		qjmp1 = JZ8(0);
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagI | FPUflagSI);
			qjmp2 = JMP8(0);
		x86SetJ8(qjmp1);
			xOR(ptr32[&fpuRegs.fprc[31]], FPUflagD | FPUflagSD);
		x86SetJ8(qjmp2);

		SetMaxValue(regd);
		pjmp32 = JMP32(0);
	x86SetJ8(pjmp1);

	ToDouble(regt); ToDouble(regd);

	xSQRT.SD(xRegisterSSE(regt), xRegisterSSE(regt));
	xDIV.SD(xRegisterSSE(regd), xRegisterSSE(regt));

	ToPS2FPU(regd, false, regt, false);
	x86SetJ32(pjmp32);

	_freeXMMreg(t1reg);
}

void recRSQRThelper2(int regd, int regt)
{
	xAND.PS(xRegisterSSE(regt), ptr[&s_const.pos[0]]);

	ToDouble(regt); ToDouble(regd);

	xSQRT.SD(xRegisterSSE(regt), xRegisterSSE(regt));
	xDIV.SD(xRegisterSSE(regd), xRegisterSSE(regt));

	ToPS2FPU(regd, false, regt, false);
}

void recRSQRT_S_xmm(int info)
{
	EE::Profiler.EmitOp(eeOpcode::RSQRT_F);
	int sreg, treg;

	bool roundmodeFlag = false;
	if (EmuConfig.Cpu.FPUFPCR.GetRoundMode() != FPRoundMode::Nearest)
	{
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest.SetRoundMode(FPRoundMode::Nearest);
		xLDMXCSR(ptr32[&roundmode_nearest.bitmask]);
		roundmodeFlag = true;
	}

	ALLOC_S(sreg); ALLOC_T(treg);

	if (FPU_FLAGS_ID)
		recRSQRThelper1(sreg, treg);
	else
		recRSQRThelper2(sreg, treg);

	xMOVSS(xRegisterSSE(EEREC_D), xRegisterSSE(sreg));

	_freeXMMreg(treg); _freeXMMreg(sreg);

	if (roundmodeFlag)
		xLDMXCSR(ptr32[&EmuConfig.Cpu.FPUFPCR.bitmask]);
}

FPURECOMPILE_CONSTCODE(RSQRT_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);


}
}
}
}
}
#endif
