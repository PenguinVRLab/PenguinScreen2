// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

static __fi void testZero(const xmm& xmmReg, const xmm& xmmTemp, const x32& gprTemp)
{
	xXOR.PS(xmmTemp, xmmTemp);
	xCMPEQ.SS(xmmTemp, xmmReg);
	xPTEST(xmmTemp, xmmTemp);
}

static __fi void testNeg(mV, const xmm& xmmReg, const x32& gprTemp)
{
	xMOVMSKPS(gprTemp, xmmReg);
	xTEST(gprTemp, 1);
	xForwardJZ8 skip;
		xMOV(ptr32[&mVU.divFlag], divI);
		xAND.PS(xmmReg, ptr128[mVUglob.absclip]);
	skip.SetTarget();
}

mVUop(mVU_DIV)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 7); }
	pass2
	{
		xmm Ft;
		if (_Ftf_) Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		else       Ft = mVU.regAlloc->allocReg(_Ft_);
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();

		testZero(Ft, t1, gprT1);
		xForwardJZ8 cjmp;

			testZero(Fs, t1, gprT1);
			xForwardJZ8 ajmp;
				xMOV(ptr32[&mVU.divFlag], divI);
				xForwardJump8 bjmp;
			ajmp.SetTarget();
				xMOV(ptr32[&mVU.divFlag], divD);
			bjmp.SetTarget();

			xXOR.PS(Fs, Ft);
			xAND.PS(Fs, ptr128[mVUglob.signbit]);
			xOR.PS (Fs, ptr128[mVUglob.maxvals]);

			xForwardJump8 djmp;
		cjmp.SetTarget();
			xMOV(ptr32[&mVU.divFlag], 0);
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		djmp.SetTarget();

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
		mVU.profiler.EmitOp(opDIV);
	}
	pass3 { mVUlog("DIV Q, vf%02d%s, vf%02d%s", _Fs_, _Fsf_String, _Ft_, _Ftf_String); }
}

mVUop(mVU_SQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, 0, 0, _Ft_, _Ftf_, 7); }
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));

		xMOV(ptr32[&mVU.divFlag], 0);
		testNeg(mVU, Ft, gprT1);

		if (CHECK_VU_OVERFLOW(mVU.index))
			xMIN.SS(Ft, ptr32[mVUglob.maxvals]);
		xSQRT.SS(Ft, Ft);
		writeQreg(Ft, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opSQRT);
	}
	pass3 { mVUlog("SQRT Q, vf%02d%s", _Ft_, _Ftf_String); }
}

mVUop(mVU_RSQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 13); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& Ft = mVU.regAlloc->allocReg(_Ft_, 0, (1 << (3 - _Ftf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();

		xMOV(ptr32[&mVU.divFlag], 0);
		testNeg(mVU, Ft, gprT1);

		xSQRT.SS(Ft, Ft);
		testZero(Ft, t1, gprT1);
		xForwardJZ8 ajmp;

			testZero(Fs, t1, gprT1);
			xForwardJZ8 bjmp;
				xMOV(ptr32[&mVU.divFlag], divI);
				xForwardJump8 cjmp;
			bjmp.SetTarget();
				xMOV(ptr32[&mVU.divFlag], divD);
			cjmp.SetTarget();

			xAND.PS(Fs, ptr128[mVUglob.signbit]);
			xOR.PS(Fs, ptr128[mVUglob.maxvals]);

			xForwardJump8 djmp;
		ajmp.SetTarget();
			SSE_DIVSS(mVU, Fs, Ft);
			mVUclamp1(mVU, Fs, t1, 8, true);
		djmp.SetTarget();

		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU.cop2)
		{
			xAND(gprF0, ~0xc0000);
			xOR(gprF0, ptr32[&mVU.divFlag]);
		}

		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(t1);
		mVU.profiler.EmitOp(opRSQRT);
	}
	pass3 { mVUlog("RSQRT Q, vf%02d%s, vf%02d%s", _Fs_, _Fsf_String, _Ft_, _Ftf_String); }
}

#define EATANhelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		SSE_MULSS(mVU, t2, Fs); \
		xMOVAPS(t1, t2); \
		xMUL.SS(t1, ptr32[addr]); \
		SSE_ADDSS(mVU, PQ, t1); \
	}

static __fi void mVU_EATAN_(mV, const xmm& PQ, const xmm& Fs, const xmm& t1, const xmm& t2)
{
	xMOVSS(PQ, Fs);
	xMUL.SS(PQ, ptr32[mVUglob.T1]);
	xMOVAPS(t2, Fs);
	EATANhelper(mVUglob.T2);
	EATANhelper(mVUglob.T3);
	EATANhelper(mVUglob.T4);
	EATANhelper(mVUglob.T5);
	EATANhelper(mVUglob.T6);
	EATANhelper(mVUglob.T7);
	EATANhelper(mVUglob.T8);
	xADD.SS(PQ, ptr32[mVUglob.Pi4]);
	xPSHUF.D(PQ, PQ, mVUinfo.writeP ? 0x27 : 0xC6);
}

mVUop(mVU_EATAN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 54);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xMOVSS (xmmPQ, Fs);
		xSUB.SS(Fs,    ptr32[mVUglob.one]);
		xADD.SS(xmmPQ, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opEATAN);
	}
	pass3 { mVUlog("EATAN P"); }
}

mVUop(mVU_EATANxy)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const xmm& t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(Fs, t1, 0x01);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xMOVSS  (xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1);
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opEATANxy);
	}
	pass3 { mVUlog("EATANxy P"); }
}

mVUop(mVU_EATANxz)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const xmm& t1 = mVU.regAlloc->allocReg(_Fs_, 0, 0xf);
		const xmm& Fs = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(Fs, t1, 0x02);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xMOVSS  (xmmPQ, Fs);
		SSE_SUBSS (mVU, Fs, t1);
		SSE_ADDSS (mVU, t1, xmmPQ);
		SSE_DIVSS (mVU, Fs, t1);
		mVU_EATAN_(mVU, xmmPQ, Fs, t1, t2);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opEATANxz);
	}
	pass3 { mVUlog("EATANxz P"); }
}

#define eexpHelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs); \
		xMOVAPS(t1, t2); \
		xMUL.SS(t1, ptr32[addr]); \
		SSE_ADDSS(mVU, xmmPQ, t1); \
	}

mVUop(mVU_EEXP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 44);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xMOVSS  (xmmPQ, Fs);
		xMUL.SS (xmmPQ, ptr32[mVUglob.E1]);
		xADD.SS (xmmPQ, ptr32[mVUglob.one]);
		xMOVAPS(t1, Fs);
		SSE_MULSS(mVU, t1, Fs);
		xMOVAPS(t2, t1);
		xMUL.SS(t1, ptr32[mVUglob.E2]);
		SSE_ADDSS(mVU, xmmPQ, t1);
		eexpHelper(&mVUglob.E3);
		eexpHelper(&mVUglob.E4);
		eexpHelper(&mVUglob.E5);
		SSE_MULSS(mVU, t2, Fs);
		xMUL.SS(t2, ptr32[mVUglob.E6]);
		SSE_ADDSS(mVU, xmmPQ, t2);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		SSE_MULSS(mVU, xmmPQ, xmmPQ);
		xMOVSSZX(t2, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, t2, xmmPQ);
		xMOVSS(xmmPQ, t2);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opEEXP);
	}
	pass3 { mVUlog("EEXP P"); }
}

static __fi void mVU_sumXYZ(mV, const xmm& PQ, const xmm& Fs)
{
	xDP.PS(Fs, Fs, 0x71);
	xMOVSS(PQ, Fs);
}

mVUop(mVU_ELENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xSQRT.SS       (xmmPQ, xmmPQ);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opELENG);
	}
	pass3 { mVUlog("ELENG P"); }
}

mVUop(mVU_ERCPR)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xMOVSS        (xmmPQ, Fs);
		xMOVSSZX      (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		xMOVSS        (xmmPQ, Fs);
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opERCPR);
	}
	pass3 { mVUlog("ERCPR P"); }
}

mVUop(mVU_ERLENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 24);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xSQRT.SS       (xmmPQ, xmmPQ);
		xMOVSSZX       (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xMOVSS         (xmmPQ, Fs);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opERLENG);
	}
	pass3 { mVUlog("ERLENG P"); }
}

mVUop(mVU_ERSADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xMOVSSZX       (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS (mVU, Fs, xmmPQ);
		xMOVSS         (xmmPQ, Fs);
		xPSHUF.D       (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opERSADD);
	}
	pass3 { mVUlog("ERSADD P"); }
}

mVUop(mVU_ERSQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 18);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xAND.PS       (Fs, ptr128[mVUglob.absclip]);
		xSQRT.SS      (xmmPQ, Fs);
		xMOVSSZX      (Fs, ptr32[mVUglob.one]);
		SSE_DIVSS(mVU, Fs, xmmPQ);
		xMOVSS        (xmmPQ, Fs);
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opERSQRT);
	}
	pass3 { mVUlog("ERSQRT P"); }
}

mVUop(mVU_ESADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 11);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU_sumXYZ(mVU, xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opESADD);
	}
	pass3 { mVUlog("ESADD P"); }
}

mVUop(mVU_ESIN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 29);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xmm& t1 = mVU.regAlloc->allocReg();
		const xmm& t2 = mVU.regAlloc->allocReg();
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xMOVSS        (xmmPQ, Fs);
		SSE_MULSS(mVU, Fs, Fs);
		xMOVAPS       (t1, Fs);
		SSE_MULSS(mVU, Fs, xmmPQ);
		xMOVAPS       (t2, Fs);
		xMUL.SS       (Fs, ptr32[mVUglob.S2]);
		SSE_ADDSS(mVU, xmmPQ, Fs);

		SSE_MULSS(mVU, t2, t1);
		xMUL.SS       (Fs, t2, ptr32[mVUglob.S3]);
		SSE_ADDSS(mVU, xmmPQ, Fs);

		SSE_MULSS(mVU, t2, t1);
		xMUL.SS       (Fs, t2, ptr32[mVUglob.S4]);
		SSE_ADDSS(mVU, xmmPQ, Fs);

		SSE_MULSS(mVU, t2, t1);
		xMUL.SS       (t2, ptr32[mVUglob.S5]);
		SSE_ADDSS(mVU, xmmPQ, t2);
		xPSHUF.D      (xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.regAlloc->clearNeeded(t2);
		mVU.profiler.EmitOp(opESIN);
	}
	pass3 { mVUlog("ESIN P"); }
}

mVUop(mVU_ESQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xAND.PS (Fs, ptr128[mVUglob.absclip]);
		xSQRT.SS(xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opESQRT);
	}
	pass3 { mVUlog("ESQRT P"); }
}

mVUop(mVU_ESUM)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 12);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, _X_Y_Z_W);
		const xmm& t1 = mVU.regAlloc->allocReg();
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		xPSHUF.D(t1, Fs, 0x1b);
		SSE_ADDPS(mVU, Fs, t1);
		xPSHUF.D(t1, Fs, 0x01);
		SSE_ADDSS(mVU, Fs, t1);
		xMOVSS(xmmPQ, Fs);
		xPSHUF.D(xmmPQ, xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.regAlloc->clearNeeded(t1);
		mVU.profiler.EmitOp(opESUM);
	}
	pass3 { mVUlog("ESUM P"); }
}

mVUop(mVU_FCAND)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xAND(dst, _Imm24_);
		xADD(dst, 0xffffff);
		xSHR(dst, 24);
		mVU.regAlloc->clearNeeded(dst);
		mVU.profiler.EmitOp(opFCAND);
	}
	pass3 { mVUlog("FCAND vi01, $%x", _Imm24_); }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCEQ)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xXOR(dst, _Imm24_);
		xSUB(dst, 1);
		xSHR(dst, 31);
		mVU.regAlloc->clearNeeded(dst);
		mVU.profiler.EmitOp(opFCEQ);
	}
	pass3 { mVUlog("FCEQ vi01, $%x", _Imm24_); }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCGET)
{
	pass1 { mVUanalyzeCflag(mVU, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, regT, cFLAG.read);
		xAND(regT, 0xfff);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opFCGET);
	}
	pass3 { mVUlog("FCGET vi%02d", _Ft_); }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCOR)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const xRegister32& dst = mVU.regAlloc->allocGPR(-1, 1, mVUlow.backupVI);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		xOR(dst, _Imm24_);
		xADD(dst, 1);
		xSHR(dst, 24);
		mVU.regAlloc->clearNeeded(dst);
		mVU.profiler.EmitOp(opFCOR);
	}
	pass3 { mVUlog("FCOR vi01, $%x", _Imm24_); }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCSET)
{
	pass1 { cFLAG.doFlag = true; }
	pass2
	{
		xMOV(gprT1, _Imm24_);
		mVUallocCFLAGb(mVU, gprT1, cFLAG.write);
		mVU.profiler.EmitOp(opFCSET);
	}
	pass3 { mVUlog("FCSET $%x", _Imm24_); }
}

mVUop(mVU_FMAND)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xAND(regT, gprT1);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opFMAND);
	}
	pass3 { mVUlog("FMAND vi%02d, vi%02d", _Ft_, _Fs_); }
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMEQ)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xXOR(regT, gprT1);
		xSUB(regT, 1);
		xSHR(regT, 31);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opFMEQ);
	}
	pass3 { mVUlog("FMEQ vi%02d, vi%02d", _Ft_, _Fs_); }
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMOR)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		const xRegister32& regT = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		xOR(regT, gprT1);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opFMOR);
	}
	pass3 { mVUlog("FMOR vi%02d, vi%02d", _Ft_, _Fs_); }
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FSAND)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		if (_Imm12_ & 0x0c30) DevCon.WriteLn(Color_Green, "mVU_FSAND: Checking I/D/IS/DS Flags");
		if (_Imm12_ & 0x030c) DevCon.WriteLn(Color_Green, "mVU_FSAND: Checking U/O/US/OS Flags");
		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT1, sFLAG.read);
		xAND(reg, _Imm12_);
		mVU.regAlloc->clearNeeded(reg);
		mVU.profiler.EmitOp(opFSAND);
	}
	pass3 { mVUlog("FSAND vi%02d, $%x", _Ft_, _Imm12_); }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSOR)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGc(reg, gprT2, sFLAG.read);
		xOR(reg, _Imm12_);
		mVU.regAlloc->clearNeeded(reg);
		mVU.profiler.EmitOp(opFSOR);
	}
	pass3 { mVUlog("FSOR vi%02d, $%x", _Ft_, _Imm12_); }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSEQ)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0c30) DevCon.WriteLn(Color_Green, "mVU_FSEQ: Checking I/D/IS/DS Flags");
		if (_Imm12_ & 0x030c) DevCon.WriteLn(Color_Green, "mVU_FSEQ: Checking U/O/US/OS Flags");
		if (_Imm12_ & 0x0001) imm |= 0x0000f00;
		if (_Imm12_ & 0x0002) imm |= 0x000f000;
		if (_Imm12_ & 0x0004) imm |= 0x0010000;
		if (_Imm12_ & 0x0008) imm |= 0x0020000;
		if (_Imm12_ & 0x0010) imm |= 0x0040000;
		if (_Imm12_ & 0x0020) imm |= 0x0080000;
		if (_Imm12_ & 0x0040) imm |= 0x000000f;
		if (_Imm12_ & 0x0080) imm |= 0x00000f0;
		if (_Imm12_ & 0x0100) imm |= 0x0400000;
		if (_Imm12_ & 0x0200) imm |= 0x0800000;
		if (_Imm12_ & 0x0400) imm |= 0x1000000;
		if (_Imm12_ & 0x0800) imm |= 0x2000000;

		const xRegister32& reg = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		mVUallocSFLAGa(reg, sFLAG.read);
		setBitFSEQ(reg, 0x0f00);
		setBitFSEQ(reg, 0xf000);
		setBitFSEQ(reg, 0x000f);
		setBitFSEQ(reg, 0x00f0);
		xXOR(reg, imm);
		xSUB(reg, 1);
		xSHR(reg, 31);
		mVU.regAlloc->clearNeeded(reg);
		mVU.profiler.EmitOp(opFSEQ);
	}
	pass3 { mVUlog("FSEQ vi%02d, $%x", _Ft_, _Imm12_); }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSSET)
{
	pass1 { mVUanalyzeFSSET(mVU); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0040) imm |= 0x000000f;
		if (_Imm12_ & 0x0080) imm |= 0x00000f0;
		if (_Imm12_ & 0x0100) imm |= 0x0400000;
		if (_Imm12_ & 0x0200) imm |= 0x0800000;
		if (_Imm12_ & 0x0400) imm |= 0x1000000;
		if (_Imm12_ & 0x0800) imm |= 0x2000000;
		if (!(sFLAG.doFlag || mVUinfo.doDivFlag))
		{
			mVUallocSFLAGa(getFlagReg(sFLAG.write), sFLAG.lastWrite);
		}
		xAND(getFlagReg(sFLAG.write), 0xfff00);
		if (imm)
			xOR(getFlagReg(sFLAG.write), imm);
		mVU.profiler.EmitOp(opFSSET);
	}
	pass3 { mVUlog("FSSET $%x", _Imm12_); }
}

mVUop(mVU_IADD)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_Is_ == 0 || _It_ == 0)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_ ? _Is_ : _It_, -1);
			const xRegister32& regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xMOV(regD, regS);
			mVU.regAlloc->clearNeeded(regD);
			mVU.regAlloc->clearNeeded(regS);
		}
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xADD(regS, regT);
			mVU.regAlloc->clearNeeded(regS);
			mVU.regAlloc->clearNeeded(regT);
		}
		mVU.profiler.EmitOp(opIADD);
	}
	pass3 { mVUlog("IADD vi%02d, vi%02d, vi%02d", _Fd_, _Fs_, _Ft_); }
}

mVUop(mVU_IADDI)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm5_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm5_ != 0)
					xMOV(regT, _Imm5_);
				else
					xXOR(regT, regT);
			}
			else
			{
				xMOV(regT, ptr32[&curI]);
				xSHL(regT, 21);
				xSAR(regT, 27);
			}
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm5_ != 0)
					xADD(regS, _Imm5_);
			}
			else
			{
				xMOV(gprT1, ptr32[&curI]);
				xSHL(gprT1, 21);
				xSAR(gprT1, 27);

				xADD(regS, gprT1);
			}
			mVU.regAlloc->clearNeeded(regS);
		}
		mVU.profiler.EmitOp(opIADDI);
	}
	pass3 { mVUlog("IADDI vi%02d, vi%02d, %d", _Ft_, _Fs_, _Imm5_); }
}

mVUop(mVU_IADDIU)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm15_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm15_ != 0)
					xMOV(regT, _Imm15_);
				else
					xXOR(regT, regT);
			}
			else
			{
				xMOV(regT, ptr32[&curI]);
				xMOV(gprT1, regT);
				xSHR(gprT1, 10);
				xAND(gprT1, 0x7800);
				xAND(regT, 0x7FF);
				xOR(regT, gprT1);
			}
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm15_ != 0)
					xADD(regS, _Imm15_);
			}
			else
			{
				xMOV(gprT1, ptr32[&curI]);
				xMOV(gprT2, gprT1);
				xSHR(gprT2, 10);
				xAND(gprT2, 0x7800);
				xAND(gprT1, 0x7FF);
				xOR(gprT1, gprT2);

				xADD(regS, gprT1);
			}
			mVU.regAlloc->clearNeeded(regS);
		}
		mVU.profiler.EmitOp(opIADDIU);
	}
	pass3 { mVUlog("IADDIU vi%02d, vi%02d, %d", _Ft_, _Fs_, _Imm15_); }
}

mVUop(mVU_IAND)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xAND(regS, regT);
		mVU.regAlloc->clearNeeded(regS);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opIAND);
	}
	pass3 { mVUlog("IAND vi%02d, vi%02d, vi%02d", _Fd_, _Fs_, _Ft_); }
}

mVUop(mVU_IOR)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
		if (_It_ != _Is_)
			xOR(regS, regT);
		mVU.regAlloc->clearNeeded(regS);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opIOR);
	}
	pass3 { mVUlog("IOR vi%02d, vi%02d, vi%02d", _Fd_, _Fs_, _Ft_); }
}

mVUop(mVU_ISUB)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_It_ != _Is_)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1);
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Id_, mVUlow.backupVI);
			xSUB(regS, regT);
			mVU.regAlloc->clearNeeded(regS);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regD = mVU.regAlloc->allocGPR(-1, _Id_, mVUlow.backupVI);
			xXOR(regD, regD);
			mVU.regAlloc->clearNeeded(regD);
		}
		mVU.profiler.EmitOp(opISUB);
	}
	pass3 { mVUlog("ISUB vi%02d, vi%02d, vi%02d", _Fd_, _Fs_, _Ft_); }
}

mVUop(mVU_ISUBIU)
{
	pass1 { mVUanalyzeIALU2(mVU, _Is_, _It_); }
	pass2
	{
		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _It_, mVUlow.backupVI);
		if (!EmuConfig.Gamefixes.IbitHack)
		{
			if (_Imm15_ != 0)
				xSUB(regS, _Imm15_);
		}
		else
		{
			xMOV(gprT1, ptr32[&curI]);
			xMOV(gprT2, gprT1);
			xSHR(gprT2, 10);
			xAND(gprT2, 0x7800);
			xAND(gprT1, 0x7FF);
			xOR(gprT1, gprT2);

			xSUB(regS, gprT1);
		}
		mVU.regAlloc->clearNeeded(regS);
		mVU.profiler.EmitOp(opISUBIU);
	}
	pass3 { mVUlog("ISUBIU vi%02d, vi%02d, %d", _Ft_, _Fs_, _Imm15_); }
}

mVUop(mVU_MFIR)
{
	pass1
	{
		if (!_Ft_)
		{
			mVUlow.isNOP = true;
		}
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeReg2  (mVU, _Ft_, mVUlow.VF_write, 1);
	}
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_Is_ != 0)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, -1);
			xMOVSX(xRegister32(regS), xRegister16(regS));
			xMOVDZX(Ft, regS);
			if (!_XYZW_SS)
				mVUunpack_xyzw(Ft, Ft, 0);
			mVU.regAlloc->clearNeeded(regS);
		}
		else
		{
			xPXOR(Ft, Ft);
		}
		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opMFIR);
	}
	pass3 { mVUlog("MFIR.%s vf%02d, vi%02d", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_MFP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeMFP(mVU, _Ft_);
	}
	pass2
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		getPreg(mVU, Ft);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opMFP);
	}
	pass3 { mVUlog("MFP.%s vf%02d, P", _XYZW_String, _Ft_); }
}

mVUop(mVU_MOVE)
{
	pass1 { mVUanalyzeMOVE(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _Ft_, _X_Y_Z_W);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opMOVE);
	}
	pass3 { mVUlog("MOVE.%s vf%02d, vf%02d", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_MR32)
{
	pass1 { mVUanalyzeMR32(mVU, _Fs_, _Ft_); }
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_);
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		if (_XYZW_SS)
			mVUunpack_xyzw(Ft, Fs, (_X ? 1 : (_Y ? 2 : (_Z ? 3 : 0))));
		else
			xPSHUF.D(Ft, Fs, 0x39);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opMR32);
	}
	pass3 { mVUlog("MR32.%s vf%02d, vf%02d", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_MTIR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeReg5(mVU, _Fs_, _Fsf_, mVUlow.VF_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVD(regT, Fs);
		mVU.regAlloc->clearNeeded(regT);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opMTIR);
	}
	pass3 { mVUlog("MTIR vi%02d, vf%02d%s", _Ft_, _Fs_, _Fsf_String); }
}

mVUop(mVU_ILW)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = mVU.regs().Mem + offsetSS;
		std::optional<xAddressVoid> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, offsetSS));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xADD(gprT1, _Imm11_);
			}
			else
			{
				xMOV(gprT2, ptr32[&curI]);
				xSHL(gprT2, 21);
				xSAR(gprT2, 21);

				xADD(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVZX(regT, ptr16[optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, ptr, gprT1q)]);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opILW);
	}
	pass3 { mVUlog("ILW.%s vi%02d, vi%02d + %d", _XYZW_String, _Ft_, _Fs_, _Imm11_); }
}

mVUop(mVU_ILWR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = mVU.regs().Mem + offsetSS;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix (mVU, gprT1q, gprT2q);

			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOVZX(regT, ptr16[xComplexAddress(gprT2q, ptr, gprT1q)]);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOVZX(regT, ptr16[ptr]);
			mVU.regAlloc->clearNeeded(regT);
		}
		mVU.profiler.EmitOp(opILWR);
	}
	pass3 { mVUlog("ILWR.%s vi%02d, vi%02d", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_ISW)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, mVUlow.VI_read[1]);
	}
	pass2
	{
		std::optional<xAddressVoid> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xADD(gprT1, _Imm11_);
			}
			else
			{
				xMOV(gprT2, ptr32[&curI]);
				xSHL(gprT2, 21);
				xSAR(gprT2, 21);

				xADD(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		const xAddressVoid ptr(optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, mVU.regs().Mem, gprT1q));
		if (_X) xMOV(ptr32[ptr], regT);
		if (_Y) xMOV(ptr32[ptr + 4], regT);
		if (_Z) xMOV(ptr32[ptr + 8], regT);
		if (_W) xMOV(ptr32[ptr + 12], regT);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opISW);
	}
	pass3 { mVUlog("ISW.%s vi%02d, vi%02d + %d", _XYZW_String, _Ft_, _Fs_, _Imm11_); }
}

mVUop(mVU_ISWR)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, mVUlow.VI_read[1]);
	}
	pass2
	{
		void* base = mVU.regs().Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_)
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			is = gprT1q;
		}
		const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, -1, false, true);
		if (!is.IsEmpty() && (sptr)base != (s32)(sptr)base)
		{
			int register_offset = -1;
			auto writeBackAt = [&](int offset) {
				if (register_offset == -1)
				{
					xLEA(gprT2q, ptr[(void*)((sptr)base + offset)]);
					register_offset = offset;
				}
				xMOV(ptr32[gprT2q + is + (offset - register_offset)], regT);
			};
			if (_X) writeBackAt(0);
			if (_Y) writeBackAt(4);
			if (_Z) writeBackAt(8);
			if (_W) writeBackAt(12);
		}
		else if (is.IsEmpty())
		{
			if (_X) xMOV(ptr32[(void*)((uptr)base)], regT);
			if (_Y) xMOV(ptr32[(void*)((uptr)base + 4)], regT);
			if (_Z) xMOV(ptr32[(void*)((uptr)base + 8)], regT);
			if (_W) xMOV(ptr32[(void*)((uptr)base + 12)], regT);
		}
		else
		{
			if (_X) xMOV(ptr32[base + is], regT);
			if (_Y) xMOV(ptr32[base + is + 4], regT);
			if (_Z) xMOV(ptr32[base + is + 8], regT);
			if (_W) xMOV(ptr32[base + is + 12], regT);
		}
		mVU.regAlloc->clearNeeded(regT);

		mVU.profiler.EmitOp(opISWR);
	}
	pass3 { mVUlog("ISWR.%s vi%02d, vi%02d", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_LQ)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, false); }
	pass2
	{
		const std::optional<xAddressVoid> optaddr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0));
		if (!optaddr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xADD(gprT1, _Imm11_);
			}
			else
			{
				xMOV(gprT2, ptr32[&curI]);
				xSHL(gprT2, 21);
				xSAR(gprT2, 21);

				xADD(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		mVUloadReg(Ft, optaddr.has_value() ? optaddr.value() : xComplexAddress(gprT2q, mVU.regs().Mem, gprT1q), _X_Y_Z_W);
		mVU.regAlloc->clearNeeded(Ft);
		mVU.profiler.EmitOp(opLQ);
	}
	pass3 { mVUlog("LQ.%s vf%02d, vi%02d + %d", _XYZW_String, _Ft_, _Fs_, _Imm11_); }
}

mVUop(mVU_LQD)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = mVU.regs().Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_ || isVU0)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xDEC(regS);
			xMOVSX(gprT1, xRegister16(regS));
			mVU.regAlloc->clearNeeded(regS);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			is = gprT1q;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU.microMemSize - 8)));
		}
		if (!mVUlow.noWriteVF)
		{
			const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			if (is.IsEmpty())
			{
				mVUloadReg(Ft, xAddressVoid(ptr), _X_Y_Z_W);
			}
			else
			{
				mVUloadReg(Ft, xComplexAddress(gprT2q, ptr, is), _X_Y_Z_W);
			}
			mVU.regAlloc->clearNeeded(Ft);
		}
		mVU.profiler.EmitOp(opLQD);
	}
	pass3 { mVUlog("LQD.%s vf%02d, --vi%02d", _XYZW_String, _Ft_, _Is_); }
}

mVUop(mVU_LQI)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		void* ptr = mVU.regs().Mem;
		xAddressReg is = xEmptyReg;
		if (_Is_)
		{
			const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, _Is_, mVUlow.backupVI);
			xMOVSX(gprT1, xRegister16(regS));
			xINC(regS);
			mVU.regAlloc->clearNeeded(regS);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			is = gprT1q;
		}
		if (!mVUlow.noWriteVF)
		{
			const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
			if (is.IsEmpty())
				mVUloadReg(Ft, xAddressVoid(ptr), _X_Y_Z_W);
			else
				mVUloadReg(Ft, xComplexAddress(gprT2q, ptr, is), _X_Y_Z_W);
			mVU.regAlloc->clearNeeded(Ft);
		}
		mVU.profiler.EmitOp(opLQI);
	}
	pass3 { mVUlog("LQI.%s vf%02d, vi%02d++", _XYZW_String, _Ft_, _Fs_); }
}

mVUop(mVU_SQ)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, false); }
	pass2
	{
		const std::optional<xAddressVoid> optptr(EmuConfig.Gamefixes.IbitHack ? std::nullopt : mVUoptimizeConstantAddr(mVU, _It_, _Imm11_, 0));
		if (!optptr.has_value())
		{
			mVU.regAlloc->moveVIToGPR(gprT1, _It_);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					xADD(gprT1, _Imm11_);
			}
			else
			{
				xMOV(gprT2, ptr32[&curI]);
				xSHL(gprT2, 21);
				xSAR(gprT2, 21);

				xADD(gprT1, gprT2);
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		mVUsaveReg(Fs, optptr.has_value() ? optptr.value() : xComplexAddress(gprT2q, mVU.regs().Mem, gprT1q), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opSQ);
	}
	pass3 { mVUlog("SQ.%s vf%02d, vi%02d + %d", _XYZW_String, _Fs_, _Ft_, _Imm11_); }
}

mVUop(mVU_SQD)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		void* ptr = mVU.regs().Mem;
		xAddressReg it = xEmptyReg;
		if (_It_ || isVU0)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xDEC(regT);
			xMOVZX(gprT1, xRegister16(regT));
			mVU.regAlloc->clearNeeded(regT);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			it = gprT1q;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU.microMemSize - 8)));
		}
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		if (it.IsEmpty())
			mVUsaveReg(Fs, xAddressVoid(ptr), _X_Y_Z_W, 1);
		else
			mVUsaveReg(Fs, xComplexAddress(gprT2q, ptr, it), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opSQD);
	}
	pass3 { mVUlog("SQD.%s vf%02d, --vi%02d", _XYZW_String, _Fs_, _Ft_); }
}

mVUop(mVU_SQI)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		void* ptr = mVU.regs().Mem;
		if (_It_)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_, _It_, mVUlow.backupVI);
			xMOVZX(gprT1, xRegister16(regT));
			xINC(regT);
			mVU.regAlloc->clearNeeded(regT);
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}
		const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W);
		if (_It_)
			mVUsaveReg(Fs, xComplexAddress(gprT2q, ptr, gprT1q), _X_Y_Z_W, 1);
		else
			mVUsaveReg(Fs, xAddressVoid(ptr), _X_Y_Z_W, 1);
		mVU.regAlloc->clearNeeded(Fs);
		mVU.profiler.EmitOp(opSQI);
	}
	pass3 { mVUlog("SQI.%s vf%02d, vi%02d++", _XYZW_String, _Fs_, _Ft_); }
}

mVUop(mVU_RINIT)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xMOVD(gprT1, Fs);
			xAND(gprT1, 0x007fffff);
			xOR (gprT1, 0x3f800000);
			xMOV(ptr32[Rmem], gprT1);
			mVU.regAlloc->clearNeeded(Fs);
		}
		else
			xMOV(ptr32[Rmem], 0x3f800000);
		mVU.profiler.EmitOp(opRINIT);
	}
	pass3 { mVUlog("RINIT R, vf%02d%s", _Fs_, _Fsf_String); }
}

static __fi void mVU_RGET_(mV, const x32& Rreg)
{
	if (!mVUlow.noWriteVF)
	{
		const xmm& Ft = mVU.regAlloc->allocReg(-1, _Ft_, _X_Y_Z_W);
		xMOVDZX(Ft, Rreg);
		if (!_XYZW_SS)
			mVUunpack_xyzw(Ft, Ft, 0);
		mVU.regAlloc->clearNeeded(Ft);
	}
}

mVUop(mVU_RGET)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, true); }
	pass2
	{
		xMOV(gprT1, ptr32[Rmem]);
		mVU_RGET_(mVU, gprT1);
		mVU.profiler.EmitOp(opRGET);
	}
	pass3 { mVUlog("RGET.%s vf%02d, R", _XYZW_String, _Ft_); }
}

mVUop(mVU_RNEXT)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, false); }
	pass2
	{
		const xRegister32& temp3 = mVU.regAlloc->allocGPR();
		xMOV(temp3, ptr32[Rmem]);
		xMOV(gprT1, temp3);
		xSHR(gprT1, 4);
		xAND(gprT1, 1);

		xMOV(gprT2, temp3);
		xSHR(gprT2, 22);
		xAND(gprT2, 1);

		xSHL(temp3, 1);
		xXOR(gprT1, gprT2);
		xXOR(temp3, gprT1);
		xAND(temp3, 0x007fffff);
		xOR (temp3, 0x3f800000);
		xMOV(ptr32[Rmem], temp3);
		mVU_RGET_(mVU, temp3);
		mVU.regAlloc->clearNeeded(temp3);
		mVU.profiler.EmitOp(opRNEXT);
	}
	pass3 { mVUlog("RNEXT.%s vf%02d, R", _XYZW_String, _Ft_); }
}

mVUop(mVU_RXOR)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const xmm& Fs = mVU.regAlloc->allocReg(_Fs_, 0, (1 << (3 - _Fsf_)));
			xMOVD(gprT1, Fs);
			xAND(gprT1, 0x7fffff);
			xXOR(ptr32[Rmem], gprT1);
			mVU.regAlloc->clearNeeded(Fs);
		}
		mVU.profiler.EmitOp(opRXOR);
	}
	pass3 { mVUlog("RXOR R, vf%02d%s", _Fs_, _Fsf_String); }
}

mVUop(mVU_WAITP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUstall = std::max(mVUstall, (u8)((mVUregs.p) ? (mVUregs.p - 1) : 0));
	}
	pass2 { mVU.profiler.EmitOp(opWAITP); }
	pass3 { mVUlog("WAITP"); }
}

mVUop(mVU_WAITQ)
{
	pass1 { mVUstall = std::max(mVUstall, mVUregs.q); }
	pass2 { mVU.profiler.EmitOp(opWAITQ); }
	pass3 { mVUlog("WAITQ"); }
}

mVUop(mVU_XTOP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}

		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVZX(regT, ptr16[&mVU.getVifRegs().top]);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opXTOP);
	}
	pass3 { mVUlog("XTOP vi%02d", _Ft_); }
}

mVUop(mVU_XITOP)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, mVUlow.VI_write, 1);
	}
	pass2
	{
		const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
		xMOVZX(regT, ptr16[&mVU.getVifRegs().itop]);
		xAND(regT, isVU1 ? 0x3ff : 0xff);
		mVU.regAlloc->clearNeeded(regT);
		mVU.profiler.EmitOp(opXITOP);
	}
	pass3 { mVUlog("XITOP vi%02d", _Ft_); }
}

void mVU_XGKICK_(u32 addr)
{
	addr = (addr & 0x3ff) * 16;
	u32 diff = 0x4000 - addr;
	u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, addr, ~0u, true);

	if (size > diff)
	{
		gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&vuRegs[1].Mem[addr], diff, true);
		gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[0], size - diff, true);
	}
	else
	{
		gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[addr], size, true);
	}
}

void _vuXGKICKTransfermVU(bool flush)
{
	while (VU1.xgkickenable && (flush || VU1.xgkickcyclecount >= 2))
	{
		u32 transfersize = 0;

		if (VU1.xgkicksizeremaining == 0)
		{
			u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, VU1.xgkickaddr, ~0u, flush);
			VU1.xgkicksizeremaining = size & 0xFFFF;
			VU1.xgkickendpacket = size >> 31;
			VU1.xgkickdiff = 0x4000 - VU1.xgkickaddr;

			if (VU1.xgkicksizeremaining == 0)
			{
				VU1.xgkickenable = false;
				break;
			}
		}

		if (!flush)
		{
			transfersize = std::min(VU1.xgkicksizeremaining, VU1.xgkickcyclecount * 8);
			transfersize = std::min(transfersize, VU1.xgkickdiff);
		}
		else
		{
			transfersize = VU1.xgkicksizeremaining;
			transfersize = std::min(transfersize, VU1.xgkickdiff);
		}

		if (THREAD_VU1)
		{
			if (transfersize < VU1.xgkicksizeremaining)
				gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&VU1.Mem[VU1.xgkickaddr], transfersize, true);
			else
				gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[VU1.xgkickaddr], transfersize, true);
		}
		else
		{
			gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[VU1.xgkickaddr], transfersize, true);
		}

		if (flush)
			VU1.cycle += transfersize / 8;

		VU1.xgkickcyclecount -= transfersize / 8;

		VU1.xgkickaddr = (VU1.xgkickaddr + transfersize) & 0x3FFF;
		VU1.xgkicksizeremaining -= transfersize;
		VU1.xgkickdiff = 0x4000 - VU1.xgkickaddr;

		if (VU1.xgkickendpacket && !VU1.xgkicksizeremaining)
		{
			VU1.xgkickenable = false;
		}
	}
}

static __fi void mVU_XGKICK_SYNC(mV, bool flush)
{
	mVU.regAlloc->flushCallerSavedRegisters();

	xTEST(ptr32[&VU1.xgkickenable], 0x1);
	xForwardJZ32 skipxgkick;
	xADD(ptr32[&VU1.xgkickcyclecount], mVUlow.kickcycles-1);
	xCMP(ptr32[&VU1.xgkickcyclecount], 2);
	xForwardJL32 needcycles;
	mVUbackupRegs(mVU, true, true);
	xFastCall(_vuXGKICKTransfermVU, flush);
	mVUrestoreRegs(mVU, true, true);
	needcycles.SetTarget();
	xADD(ptr32[&VU1.xgkickcyclecount], 1);
	skipxgkick.SetTarget();
}

static __fi void mVU_XGKICK_DELAY(mV)
{
	mVU.regAlloc->flushCallerSavedRegisters();

	mVUbackupRegs(mVU, true, true);
#if 0
	xTEST (ptr32[&SomeGifPathValue], 1);
	xMOV  (ptr32[&mVU.resumePtrXG], (uptr)xGetPtr() + 10 + 6);
	xJcc32(Jcc_NotZero, (uptr)mVU.exitFunctXG - ((uptr)xGetPtr()+6));
#endif
	xFastCall(mVU_XGKICK_, ptr32[&mVU.VIxgkick]);
	mVUrestoreRegs(mVU, true, true);
}

mVUop(mVU_XGKICK)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeXGkick(mVU, _Is_, 1);
	}
		pass2
	{
		if (CHECK_XGKICKHACK)
		{
			mVUlow.kickcycles = 99;
			mVU_XGKICK_SYNC(mVU, true);
			mVUlow.kickcycles = 0;
		}
		if (mVUinfo.doXGKICK)
		{
			mVU_XGKICK_DELAY(mVU);
			mVUinfo.doXGKICK = false;
		}

		const xRegister32& regS = mVU.regAlloc->allocGPR(_Is_, -1);
		if (!CHECK_XGKICKHACK)
		{
			xMOV(ptr32[&mVU.VIxgkick], regS);
		}
		else
		{
			xMOV(ptr32[&VU1.xgkickenable], 1);
			xMOV(ptr32[&VU1.xgkickendpacket], 0);
			xMOV(ptr32[&VU1.xgkicksizeremaining], 0);
			xMOV(ptr32[&VU1.xgkickcyclecount], 0);
			xMOV(gprT2, ptr32[&mVU.totalCycles]);
			xSUB(gprT2, ptr32[&mVU.cycles]);
			xADD(gprT2, ptr64[&VU1.cycle]);
			xMOV(ptr32[&VU1.xgkicklastcycle], gprT2);
			xMOV(gprT1, regS);
			xAND(gprT1, 0x3FF);
			xSHL(gprT1, 4);
			xMOV(ptr32[&VU1.xgkickaddr], gprT1);
		}
		mVU.regAlloc->clearNeeded(regS);
		mVU.profiler.EmitOp(opXGKICK);
	}
	pass3 { mVUlog("XGKICK vi%02d", _Fs_); }
}

void setBranchA(mP, int x, int _x_)
{
	bool isBranchDelaySlot = false;

	incPC(-2);
	if (mVUlow.branch)
		isBranchDelaySlot = true;
	incPC(2);

	pass1
	{
		if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot)
		{
			DevCon.WriteLn(Color_Green, "microVU%d: Branch Optimization", mVU.index);
			mVUlow.isNOP = true;
			return;
		}
		mVUbranch     = x;
		mVUlow.branch = x;
	}
	pass2 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
	pass3 { mVUbranch = x; }
	pass4 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
}

void condEvilBranch(mV, int JMPcc)
{
	if (mVUlow.badBranch)
	{
		xMOV(ptr32[&mVU.branch], gprT1);
		xMOV(ptr32[&mVU.badBranch], branchAddr(mVU));

		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
			incPC(4);
			xMOV(ptr32[&mVU.badBranch], xPC);
			incPC(-4);
		cJMP.SetTarget();
		return;
	}
	if (isEvilBlock)
	{
		xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU));
		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
		xMOV(gprT1, ptr32[&mVU.evilBranch]);
		xADD(gprT1, 8);
		xMOV(ptr32[&mVU.evilevilBranch], gprT1);
		cJMP.SetTarget();
	}
	else
	{
		xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU));
		xCMP(gprT1b, 0);
		xForwardJump8 cJMP((JccComparisonType)JMPcc);
		xMOV(gprT1, ptr32[&mVU.badBranch]);
		xADD(gprT1, 8);
		xMOV(ptr32[&mVU.evilBranch], gprT1);
		cJMP.SetTarget();
		incPC(-2);
		if (mVUlow.branch >= 9)
			DevCon.Warning("Conditional in JALR/JR delay slot - If game broken report to PCSX2 Team");
		incPC(2);
	}
}

mVUop(mVU_B)
{
	setBranchA(mX, 1, 0);
	pass1 { mVUanalyzeNormBranch(mVU, 0, false); }
	pass2
	{
		if (mVUlow.badBranch)  { xMOV(ptr32[&mVU.badBranch],  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if (isEvilBlock) xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU)); else xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU)); }
		mVU.profiler.EmitOp(opB);
	}
	pass3 { mVUlog("B [<a href=\"#addr%04x\">%04x</a>]", branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_BAL)
{
	setBranchA(mX, 2, _It_);
	pass1 { mVUanalyzeNormBranch(mVU, _It_, true); }
	pass2
	{
		if (!mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOV(regT, bSaveAddr);
			mVU.regAlloc->clearNeeded(regT);
		}
		else
		{
			incPC(-2);
			DevCon.Warning("Linking BAL from %s branch taken/not taken target! - If game broken report to PCSX2 Team", branchSTR[mVUlow.branch & 0xf]);
			incPC(2);

			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
				xMOV(regT, ptr32[&mVU.evilBranch]);
			else
				xMOV(regT, ptr32[&mVU.badBranch]);

			xADD(regT, 8);
			xSHR(regT, 3);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (mVUlow.badBranch)  { xMOV(ptr32[&mVU.badBranch],  branchAddr(mVU)); }
		if (mVUlow.evilBranch) { if (isEvilBlock) xMOV(ptr32[&mVU.evilevilBranch], branchAddr(mVU)); else xMOV(ptr32[&mVU.evilBranch], branchAddr(mVU)); }
		mVU.profiler.EmitOp(opBAL);
	}
	pass3 { mVUlog("BAL vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Ft_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBEQ)
{
	setBranchA(mX, 3, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xXOR(gprT1, ptr32[&mVU.VIbackup]);
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_);
			xXOR(gprT1, regT);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Equal);
		mVU.profiler.EmitOp(opIBEQ);
	}
	pass3 { mVUlog("IBEQ vi%02d, vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Ft_, _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBGEZ)
{
	setBranchA(mX, 4, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_GreaterOrEqual);
		mVU.profiler.EmitOp(opIBGEZ);
	}
	pass3 { mVUlog("IBGEZ vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBGTZ)
{
	setBranchA(mX, 5, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Greater);
		mVU.profiler.EmitOp(opIBGTZ);
	}
	pass3 { mVUlog("IBGTZ vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBLEZ)
{
	setBranchA(mX, 6, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_LessOrEqual);
		mVU.profiler.EmitOp(opIBLEZ);
	}
	pass3 { mVUlog("IBLEZ vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBLTZ)
{
	setBranchA(mX, 7, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_Less);
		mVU.profiler.EmitOp(opIBLTZ);
	}
	pass3 { mVUlog("IBLTZ vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

mVUop(mVU_IBNE)
{
	setBranchA(mX, 8, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			xMOV(gprT1, ptr32[&mVU.VIbackup]);
		else
			mVU.regAlloc->moveVIToGPR(gprT1, _Is_);

		if (mVUlow.memReadIt)
			xXOR(gprT1, ptr32[&mVU.VIbackup]);
		else
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(_It_);
			xXOR(gprT1, regT);
			mVU.regAlloc->clearNeeded(regT);
		}

		if (!(isBadOrEvil))
			xMOV(ptr32[&mVU.branch], gprT1);
		else
			condEvilBranch(mVU, Jcc_NotEqual);
		mVU.profiler.EmitOp(opIBNE);
	}
	pass3 { mVUlog("IBNE vi%02d, vi%02d [<a href=\"#addr%04x\">%04x</a>]", _Ft_, _Fs_, branchAddr(mVU), branchAddr(mVU)); }
}

void normJumpPass2(mV)
{
	if (!mVUlow.constJump.isValid || mVUlow.evilBranch)
	{
		mVU.regAlloc->moveVIToGPR(gprT1, _Is_);
		xSHL(gprT1, 3);
		xAND(gprT1, mVU.microMemSize - 8);

		if (!mVUlow.evilBranch)
		{
			xMOV(ptr32[&mVU.branch], gprT1);
		}
		else
		{
			if(isEvilBlock)
				xMOV(ptr32[&mVU.evilevilBranch], gprT1);
			else
				xMOV(ptr32[&mVU.evilBranch], gprT1);
		}
		if (mVUlow.badBranch)
		{
			xMOV(ptr32[&mVU.badBranch], gprT1);
		}
	}
}

mVUop(mVU_JR)
{
	mVUbranch = 9;
	pass1 { mVUanalyzeJump(mVU, _Is_, 0, false); }
	pass2
	{
		normJumpPass2(mVU);
		mVU.profiler.EmitOp(opJR);
	}
	pass3 { mVUlog("JR [vi%02d]", _Fs_); }
}

mVUop(mVU_JALR)
{
	mVUbranch = 10;
	pass1 { mVUanalyzeJump(mVU, _Is_, _It_, 1); }
	pass2
	{
		normJumpPass2(mVU);
		if (!mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			xMOV(regT, bSaveAddr);
			mVU.regAlloc->clearNeeded(regT);
		}
		if (mVUlow.evilBranch)
		{
			const xRegister32& regT = mVU.regAlloc->allocGPR(-1, _It_, mVUlow.backupVI);
			if (isEvilBlock)
			{
				xMOV(regT, ptr32[&mVU.evilBranch]);
				xADD(regT, 8);
				xSHR(regT, 3);
			}
			else
			{
				incPC(-2);
				DevCon.Warning("Linking JALR from %s branch taken/not taken target! - If game broken report to PCSX2 Team", branchSTR[mVUlow.branch & 0xf]);
				incPC(2);

				xMOV(regT, ptr32[&mVU.badBranch]);
				xADD(regT, 8);
				xSHR(regT, 3);
			}
			mVU.regAlloc->clearNeeded(regT);
		}

		mVU.profiler.EmitOp(opJALR);
	}
	pass3 { mVUlog("JALR vi%02d, [vi%02d]", _Ft_, _Fs_); }
}
