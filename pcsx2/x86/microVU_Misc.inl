// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <bitset>
#include <optional>

void mVUunpack_xyzw(const xmm& dstreg, const xmm& srcreg, int xyzw)
{
	switch (xyzw)
	{
		case 0: xPSHUF.D(dstreg, srcreg, 0x00); break;
		case 1: xPSHUF.D(dstreg, srcreg, 0x55); break;
		case 2: xPSHUF.D(dstreg, srcreg, 0xaa); break;
		case 3: xPSHUF.D(dstreg, srcreg, 0xff); break;
	}
}

void mVUloadReg(const xmm& reg, xAddressVoid ptr, int xyzw)
{
	switch (xyzw)
	{
		case 8:  xMOVSSZX(reg, ptr32[ptr     ]); break;
		case 4:  xMOVSSZX(reg, ptr32[ptr +  4]); break;
		case 2:  xMOVSSZX(reg, ptr32[ptr +  8]); break;
		case 1:  xMOVSSZX(reg, ptr32[ptr + 12]); break;
		default: xMOVAPS (reg, ptr128[ptr]);     break;
	}
}

void mVUloadIreg(const xmm& reg, int xyzw, VURegs* vuRegs)
{
	xMOVSSZX(reg, ptr32[&vuRegs->VI[REG_I].UL]);
	if (!_XYZWss(xyzw))
		xSHUF.PS(reg, reg, 0);
}

void mVUsaveReg(const xmm& reg, xAddressVoid ptr, int xyzw, bool modXYZW)
{
	switch (xyzw)
	{
		case 5:
			xEXTRACTPS(ptr32[ptr + 4], reg, 1);
			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
			break;
		case 6:
			xPSHUF.D(reg, reg, 0xc9);
			xMOVL.PS(ptr64[ptr + 4], reg);
			break;
		case 7:
			xMOVH.PS(ptr64[ptr + 8], reg);
			xEXTRACTPS(ptr32[ptr + 4], reg, 1);
			break;
		case 9:
			xMOVSS(ptr32[ptr], reg);
			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
			break;
		case 10:
			xMOVSS(ptr32[ptr], reg);
			xEXTRACTPS(ptr32[ptr + 8], reg, 2);
			break;
		case 11:
			xMOVSS(ptr32[ptr], reg);
			xMOVH.PS(ptr64[ptr + 8], reg);
			break;
		case 13:
			xMOVL.PS(ptr64[ptr], reg);
			xEXTRACTPS(ptr32[ptr + 12], reg, 3);
			break;
		case 14:
			xMOVL.PS(ptr64[ptr], reg);
			xEXTRACTPS(ptr32[ptr + 8], reg, 2);
			break;
		case 4:
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 1);
			xMOVSS(ptr32[ptr + 4], reg);
			break;
		case 2:
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 2);
			xMOVSS(ptr32[ptr + 8], reg);
			break;
		case 1:
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 3);
			xMOVSS(ptr32[ptr + 12], reg);
			break;
		case 8:
			xMOVSS(ptr32[ptr], reg);
			break;
		case 12:
			xMOVL.PS(ptr64[ptr], reg);
			break;
		case 3:
			xMOVH.PS(ptr64[ptr + 8], reg);
			break;
		default:
			xMOVAPS(ptr128[ptr], reg);
			break;
	}
}

void mVUmergeRegs(const xmm& dest, const xmm& src, int xyzw, bool modXYZW)
{
	xyzw &= 0xf;
	if ((dest != src) && (xyzw != 0))
	{
		if (xyzw == 0x8)
			xMOVSS(dest, src);
		else if (xyzw == 0xf)
			xMOVAPS(dest, src);
		else
		{
			if (modXYZW)
			{
				if      (xyzw == 1) { xINSERTPS(dest, src, _MM_MK_INSERTPS_NDX(0, 3, 0)); return; }
				else if (xyzw == 2) { xINSERTPS(dest, src, _MM_MK_INSERTPS_NDX(0, 2, 0)); return; }
				else if (xyzw == 4) { xINSERTPS(dest, src, _MM_MK_INSERTPS_NDX(0, 1, 0)); return; }
			}
			xyzw = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);
			xBLEND.PS(dest, src, xyzw);
		}
	}
}

__fi void mVUbackupRegs(microVU& mVU, bool toMemory = false, bool onlyNeeded = false)
{
	if (toMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
		{
			if (!xRegister32::IsCallerSaved(i) || i == rsp.GetId())
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedGPR(i))
			{
				num_gprs++;
				xPUSH(xRegister64(i));
			}
		}

		std::bitset<iREGCNT_XMM> save_xmms;
		for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedReg(i) || xmmPQ.Id == i)
			{
				save_xmms[i] = true;
				num_xmms++;
			}
		}

#ifdef _WIN32
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64)) + 32;
		int stack_offset = 32;
#else
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64));
		int stack_offset = 0;
#endif
		if (stack_size > 0)
		{
			xSUB(rsp, stack_size);
			for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
			{
				if (save_xmms[i])
				{
					xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
					stack_offset += sizeof(u128);
				}
			}
		}
	}
	else
	{
		mVU.regAlloc->flushAll();
		xMOVAPS(ptr128[&mVU.xmmBackup[xmmPQ.Id][0]], xmmPQ);
	}
}

__fi void mVUrestoreRegs(microVU& mVU, bool fromMemory = false, bool onlyNeeded = false)
{
	if (fromMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		std::bitset<iREGCNT_GPR> save_gprs;
		for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
		{
			if (!xRegister32::IsCallerSaved(i) || i == rsp.GetId())
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedGPR(i))
			{
				save_gprs[i] = true;
				num_gprs++;
			}
		}

		std::bitset<iREGCNT_XMM> save_xmms;
		for (int i = 0; i < static_cast<int>(iREGCNT_XMM); i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			if (!onlyNeeded || mVU.regAlloc->checkCachedReg(i) || xmmPQ.Id == i)
			{
				save_xmms[i] = true;
				num_xmms++;
			}
		}

#ifdef _WIN32
		const int stack_extra = 32;
#else
		const int stack_extra = 0;
#endif
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64)) + stack_extra;
		if (num_xmms > 0)
		{
			int stack_offset = (num_xmms - 1) * sizeof(u128) + stack_extra;
			for (int i = static_cast<int>(iREGCNT_XMM - 1); i >= 0; i--)
			{
				if (!save_xmms[i])
					continue;

				xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
				stack_offset -= sizeof(u128);
			}
		}
		if (stack_size > 0)
			xADD(rsp, stack_size);

		for (int i = static_cast<int>(iREGCNT_GPR - 1); i >= 0; i--)
		{
			if (save_gprs[i])
				xPOP(xRegister64(i));
		}
	}
	else
	{
		xMOVAPS(xmmPQ, ptr128[&mVU.xmmBackup[xmmPQ.Id][0]]);
	}
}

#if 0
static void mVUwarningRegAccess(u32 prog, u32 pc)
{
	Console.Error("microVU0 Warning: Accessing VU1 Regs! [%04x] [%x]", pc, prog);
}
#endif

static void mVUTBit()
{
	u32 old = vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUTBit, std::memory_order_release);
	if (old & VU_Thread::InterruptFlagVUTBit)
		DevCon.Warning("Old TBit not registered");
}

static void mVUEBit()
{
	vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUEBit, std::memory_order_release);
}

static inline u32 branchAddr(const mV)
{
	pxAssumeMsg(islowerOP, "MicroVU: Expected Lower OP code for valid branch addr.");
	return ((((iPC + 2) + (_Imm11_ * 2)) & mVU.progMemMask) * 4);
}

static void mVUwaitMTVU()
{
	if (IsDevBuild)
		DevCon.WriteLn("microVU0: Waiting on VU1 thread to access VU1 regs!");
	vu1Thread.WaitVU();
}

__fi void mVUaddrFix(mV, const xAddressReg& gprReg, const xAddressReg& tmpReg)
{
	if (isVU1)
	{
		xAND(xRegister32(gprReg.Id), 0x3ff);
		xSHL(xRegister32(gprReg.Id), 4);
	}
	else
	{
		xTEST(xRegister32(gprReg.Id), 0x400);
		xForwardJNZ8 jmpA;
			xAND(xRegister32(gprReg.Id), 0xff);
			xForwardJump32 jmpB;
		jmpA.SetTarget();
			if (THREAD_VU1)
			{
#if 0
					if (IsDevBuild && !isCOP2)
					{
						xMOV(gprT1, mVU.prog.cur->idx);
						xMOV(gprT2, xPC);
						mVUbackupRegs(mVU, true, false);
						xFastCall((void*)mVUwarningRegAccess, arg1regd, arg2regd);
						mVUrestoreRegs(mVU, true, false);
					}
#endif
				xFastCall((void*)mVU.waitMTVU);
			}
			xAND(xRegister32(gprReg.Id), 0x3f);
			sptr offset = (u128*)VU1.VF - (u128*)VU0.Mem;
			if (offset == (s32)offset)
			{
				xADD(gprReg, (s32)offset);
			}
			else
			{
				xMOV64(tmpReg, offset);
				xADD(gprReg, tmpReg);
			}
		jmpB.SetTarget();
		xSHL(gprReg, 4);
	}
}

__fi std::optional<xAddressVoid> mVUoptimizeConstantAddr(mV, u32 srcreg, s32 offset, s32 offsetSS_)
{
	if (srcreg != 0)
		return std::nullopt;

	const s32 addr = 0 + offset;
	if (isVU1)
	{
		return ptr[mVU.regs().Mem + ((addr & 0x3FFu) << 4) + offsetSS_];
	}
	else
	{
		if (addr & 0x400)
			return std::nullopt;

		return ptr[mVU.regs().Mem + ((addr & 0xFFu) << 4) + offsetSS_];
	}
}

struct SSEMasks
{
	u32 MIN_MAX_1[4], MIN_MAX_2[4], ADD_SS[4];
};

alignas(16) static const SSEMasks sseMasks =
{
	{0xffffffff, 0x80000000, 0xffffffff, 0x80000000},
	{0x00000000, 0x40000000, 0x00000000, 0x40000000},
	{0x80000000, 0xffffffff, 0xffffffff, 0xffffffff},
};


void MIN_MAX_PS(microVU& mVU, const xmm& to, const xmm& from, const xmm& t1in, const xmm& t2in, bool min)
{
	const xmm& t1 = t1in.IsEmpty() ? mVU.regAlloc->allocReg() : t1in;
	const xmm& t2 = t2in.IsEmpty() ? mVU.regAlloc->allocReg() : t2in;

	if (0)
	{
		xPSHUF.D(t1, to, 0xfa);
		xPAND   (t1, ptr128[sseMasks.MIN_MAX_1]);
		xPOR    (t1, ptr128[sseMasks.MIN_MAX_2]);
		xPSHUF.D(t2, from, 0xfa);
		xPAND   (t2, ptr128[sseMasks.MIN_MAX_1]);
		xPOR    (t2, ptr128[sseMasks.MIN_MAX_2]);
		if (min) xMIN.PD(t1, t2);
		else     xMAX.PD(t1, t2);

		xPSHUF.D(t2, from, 0x50);
		xPAND   (t2, ptr128[sseMasks.MIN_MAX_1]);
		xPOR    (t2, ptr128[sseMasks.MIN_MAX_2]);
		xPSHUF.D(to, to, 0x50);
		xPAND   (to, ptr128[sseMasks.MIN_MAX_1]);
		xPOR    (to, ptr128[sseMasks.MIN_MAX_2]);
		if (min) xMIN.PD(to, t2);
		else     xMAX.PD(to, t2);

		xSHUF.PS(to, t1, 0x88);
	}
	else
	{
		const xmm& c1 = min ? t2 : t1;
		const xmm& c2 = min ? t1 : t2;

		xMOVAPS  (t1, to);
		xPSRA.D  (t1, 31);
		xPSRL.D  (t1,  1);
		xPXOR    (t1, to);

		xMOVAPS  (t2, from);
		xPSRA.D  (t2, 31);
		xPSRL.D  (t2,  1);
		xPXOR    (t2, from);

		xPCMP.GTD(c1, c2);
		xPAND    (to, c1);
		xPANDN   (c1, from);
		xPOR     (to, c1);
	}

	if (t1 != t1in) mVU.regAlloc->clearNeeded(t1);
	if (t2 != t2in) mVU.regAlloc->clearNeeded(t2);
}

void MIN_MAX_SS(mV, const xmm& to, const xmm& from, const xmm& t1in, bool min)
{
	const xmm& t1 = t1in.IsEmpty() ? mVU.regAlloc->allocReg() : t1in;
	xSHUF.PS(to, from, 0);
	xPAND   (to, ptr128[sseMasks.MIN_MAX_1]);
	xPOR    (to, ptr128[sseMasks.MIN_MAX_2]);
	xPSHUF.D(t1, to, 0xee);
	if (min) xMIN.PD(to, t1);
	else	 xMAX.PD(to, t1);
	if (t1 != t1in)
		mVU.regAlloc->clearNeeded(t1);
}

void ADD_SS_Single_Guard_Bit(microVU& mVU, const xmm& to, const xmm& from, const xmm& t1in)
{
	const xmm& t1 = t1in.IsEmpty() ? mVU.regAlloc->allocReg() : t1in;

	xMOVD(eax, to);
	xMOVD(ecx, from);
	xSHR (eax, 23);
	xSHR (ecx, 23);
	xAND (eax, 0xff);
	xAND (ecx, 0xff);
	xSUB (ecx, eax);

	xForwardJL8 case_neg;
	xForwardJE8 case_end1;

	xCMP (ecx, 24);
	xForwardJLE8 case_pos_small;

	xPAND(to, ptr128[sseMasks.ADD_SS]);
	xForwardJump8 case_end2;

	case_pos_small.SetTarget();
	xDEC   (ecx);
	xMOV   (eax, 0xffffffff);
	xSHL   (eax, cl);
	xMOVDZX(t1, eax);
	xPAND  (to, t1);
	xForwardJump8 case_end3;

	case_neg.SetTarget();
	xCMP (ecx, -24);
	xForwardJGE8 case_neg_small;

	xPAND(from, ptr128[sseMasks.ADD_SS]);
	xForwardJump8 case_end4;

	case_neg_small.SetTarget();
	xNOT   (ecx);
	xMOV   (eax, 0xffffffff);
	xSHL   (eax, cl);
	xMOVDZX(t1, eax);
	xPAND  (from, t1);

	case_end1.SetTarget();
	case_end2.SetTarget();
	case_end3.SetTarget();
	case_end4.SetTarget();

	xADD.SS(to, from);
	if (t1 != t1in)
		mVU.regAlloc->clearNeeded(t1);
}

void ADD_SS_TriAceHack(microVU& mVU, const xmm& to, const xmm& from)
{
	xMOVD(eax, to);
	xMOVD(ecx, from);
	xSHR (eax, 23);
	xSHR (ecx, 23);
	xAND (eax, 0xff);
	xAND (ecx, 0xff);
	xSUB (ecx, eax);

	xCMP (ecx, -25);
	xForwardJLE8 case_neg_big;
	xCMP (ecx,  25);
	xForwardJL8  case_end1;

	xPAND(to, ptr128[sseMasks.ADD_SS]);
	xForwardJump8 case_end2;

	case_neg_big.SetTarget();
	xPAND(from, ptr128[sseMasks.ADD_SS]);

	case_end1.SetTarget();
	case_end2.SetTarget();

	xADD.SS(to, from);
}

#define clampOp(opX, isPS) \
	do { \
		mVUclamp3(mVU, to, t1, (isPS) ? 0xf : 0x8); \
		mVUclamp3(mVU, from, t1, (isPS) ? 0xf : 0x8); \
		opX(to, from); \
		mVUclamp4(mVU, to, t1, (isPS) ? 0xf : 0x8); \
	} while (0)

void SSE_MAXPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, false);
}
void SSE_MINPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, true);
}
void SSE_MAXSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	MIN_MAX_SS(mVU, to, from, t1, false);
}
void SSE_MINSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	MIN_MAX_SS(mVU, to, from, t1, true);
}
void SSE_ADD2SS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	if (!CHECK_VUADDSUBHACK)
		clampOp(xADD.SS, false);
	else
		ADD_SS_TriAceHack(mVU, to, from);
}

void SSE_ADD2PS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xADD.PS, true);
}
void SSE_ADDPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xADD.PS, true);
}
void SSE_ADDSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xADD.SS, false);
}
void SSE_SUBPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xSUB.PS, true);
}
void SSE_SUBSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xSUB.SS, false);
}
void SSE_MULPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xMUL.PS, true);
}
void SSE_MULSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xMUL.SS, false);
}
void SSE_DIVPS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xDIV.PS, true);
}
void SSE_DIVSS(mV, const xmm& to, const xmm& from, const xmm& t1 = xEmptyReg, const xmm& t2 = xEmptyReg)
{
	clampOp(xDIV.SS, false);
}
