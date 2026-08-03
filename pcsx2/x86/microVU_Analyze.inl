// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

__ri void analyzeReg1(mV, int xReg, microVFreg& vfRead) {
	if (xReg) {
		if (_X) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; }
		if (_Y) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; }
		if (_Z) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; }
		if (_W) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; }
	}
}

__ri void analyzeReg2(mV, int xReg, microVFreg& vfWrite, bool isLowOp)
{
	if (xReg)
	{
		#define bReg(x, y) mVUregsTemp.VFreg[y] = x; mVUregsTemp.VF[y]
		if (_X) { bReg(xReg, isLowOp).x = 4; vfWrite.reg = xReg; vfWrite.x = 4; }
		if (_Y) { bReg(xReg, isLowOp).y = 4; vfWrite.reg = xReg; vfWrite.y = 4; }
		if (_Z) { bReg(xReg, isLowOp).z = 4; vfWrite.reg = xReg; vfWrite.z = 4; }
		if (_W) { bReg(xReg, isLowOp).w = 4; vfWrite.reg = xReg; vfWrite.w = 4; }
	}
}

__ri void analyzeReg3(mV, int xReg, microVFreg& vfRead)
{
	if (xReg)
	{
		if (_bc_x)
		{
			mVUstall = std::max(mVUstall, mVUregs.VF[xReg].x);
			vfRead.reg = xReg;
			vfRead.x = 1;
		}
		else if (_bc_y)
		{
			mVUstall = std::max(mVUstall, mVUregs.VF[xReg].y);
			vfRead.reg = xReg;
			vfRead.y = 1;
		}
		else if (_bc_z)
		{
			mVUstall = std::max(mVUstall, mVUregs.VF[xReg].z);
			vfRead.reg = xReg;
			vfRead.z = 1;
		}
		else
		{
			mVUstall = std::max(mVUstall, mVUregs.VF[xReg].w);
			vfRead.reg = xReg;
			vfRead.w = 1;
		}
	}
}

__ri void analyzeReg4(mV, int xReg, microVFreg& vfRead)
{
	if (xReg)
	{
		mVUstall   = std::max(mVUstall, mVUregs.VF[xReg].w);
		vfRead.reg = xReg;
		vfRead.w   = 1;
	}
}

__ri void analyzeReg5(mV, int xReg, int fxf, microVFreg& vfRead)
{
	if (xReg)
	{
		switch (fxf)
		{
			case 0: mVUstall = std::max(mVUstall, mVUregs.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; break;
			case 1: mVUstall = std::max(mVUstall, mVUregs.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; break;
			case 2: mVUstall = std::max(mVUstall, mVUregs.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; break;
			case 3: mVUstall = std::max(mVUstall, mVUregs.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; break;
		}
	}
}

__ri void analyzeReg6(mV, int xReg, microVFreg& vfRead)
{
	if (xReg)
	{
		if (_X) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].y); vfRead.reg = xReg; vfRead.y = 1; }
		if (_Y) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].z); vfRead.reg = xReg; vfRead.z = 1; }
		if (_Z) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].w); vfRead.reg = xReg; vfRead.w = 1; }
		if (_W) { mVUstall = std::max(mVUstall, mVUregs.VF[xReg].x); vfRead.reg = xReg; vfRead.x = 1; }
	}
}

__ri void analyzeVIreg1(mV, int xReg, microVIreg& viRead)
{
	if (xReg)
	{
		mVUstall    = std::max(mVUstall, mVUregs.VI[xReg]);
		viRead.reg  = xReg;
		viRead.used = 1;
	}
}

__ri void analyzeVIreg2(mV, int xReg, microVIreg& viWrite, int aCycles)
{
	if (xReg)
	{
		mVUconstReg[xReg].isValid = 0;
		mVUregsTemp.VIreg = xReg;
		mVUregsTemp.VI    = aCycles;
		viWrite.reg  = xReg;
		viWrite.used = aCycles;
	}
}

#define analyzeQreg(x) \
	{ \
		mVUregsTemp.q = x; \
		mVUstall = std::max(mVUstall, mVUregs.q); \
	}
#define analyzePreg(x) \
	{ \
		mVUregsTemp.p = x; \
		mVUstall = std::max(mVUstall, (u8)((mVUregs.p) ? (mVUregs.p - 1) : 0)); \
	}
#define analyzeRreg() \
	{ \
		mVUregsTemp.r = 1; \
	}
#define analyzeXGkick1() \
	{ \
		mVUstall = std::max(mVUstall, mVUregs.xgkick); \
	}
#define analyzeXGkick2(x) \
	{ \
		mVUregsTemp.xgkick = x; \
	}
#define setConstReg(x, v) \
	{ \
		if (x) \
		{ \
			mVUconstReg[x].isValid = 1; \
			mVUconstReg[x].regValue = v; \
		} \
	}

__fi void mVUanalyzeFMAC1(mV, int Fd, int Fs, int Ft)
{
	sFLAG.doFlag = 1;
	analyzeReg1(mVU, Fs, mVUup.VF_read[0]);
	analyzeReg1(mVU, Ft, mVUup.VF_read[1]);
	analyzeReg2(mVU, Fd, mVUup.VF_write, 0);
}

__fi void mVUanalyzeFMAC2(mV, int Fs, int Ft)
{
	analyzeReg1(mVU, Fs, mVUup.VF_read[0]);
	analyzeReg2(mVU, Ft, mVUup.VF_write, 0);
}

__fi void mVUanalyzeFMAC3(mV, int Fd, int Fs, int Ft)
{
	sFLAG.doFlag = 1;
	analyzeReg1(mVU, Fs, mVUup.VF_read[0]);
	analyzeReg3(mVU, Ft, mVUup.VF_read[1]);
	analyzeReg2(mVU, Fd, mVUup.VF_write, 0);
}

__fi void mVUanalyzeFMAC4(mV, int Fs, int Ft)
{
	cFLAG.doFlag = 1;
	analyzeReg1(mVU, Fs, mVUup.VF_read[0]);
	analyzeReg4(mVU, Ft, mVUup.VF_read[1]);
}

__fi void mVUanalyzeIALU1(mV, int Id, int Is, int It)
{
	if (!Id)
		mVUlow.isNOP = 1;
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeVIreg1(mVU, It, mVUlow.VI_read[1]);
	analyzeVIreg2(mVU, Id, mVUlow.VI_write, 1);
}

__fi void mVUanalyzeIALU2(mV, int Is, int It)
{
	if (!It)
		mVUlow.isNOP = 1;
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
}

__fi void mVUanalyzeIADDI(mV, int Is, int It, s16 imm)
{
	mVUanalyzeIALU2(mVU, Is, It);
	if (!Is && !EmuConfig.Gamefixes.IbitHack)
	{
		setConstReg(It, imm);
	}
}

__fi void mVUanalyzeMR32(mV, int Fs, int Ft)
{
	if (!Ft)
	{
		mVUlow.isNOP = 1;
	}
	analyzeReg6(mVU, Fs, mVUlow.VF_read[0]);
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
}

__fi void mVUanalyzeFDIV(mV, int Fs, int Fsf, int Ft, int Ftf, u8 xCycles)
{
	analyzeReg5(mVU, Fs, Fsf, mVUlow.VF_read[0]);
	analyzeReg5(mVU, Ft, Ftf, mVUlow.VF_read[1]);
	analyzeQreg(xCycles);
}

__fi void mVUanalyzeEFU1(mV, int Fs, int Fsf, u8 xCycles)
{
	analyzeReg5(mVU, Fs, Fsf, mVUlow.VF_read[0]);
	analyzePreg(xCycles);
}

__fi void mVUanalyzeEFU2(mV, int Fs, u8 xCycles)
{
	analyzeReg1(mVU, Fs, mVUlow.VF_read[0]);
	analyzePreg(xCycles);
}

__fi void mVUanalyzeMFP(mV, int Ft)
{
	if (!Ft)
		mVUlow.isNOP = 1;
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
}

__fi void mVUanalyzeMOVE(mV, int Fs, int Ft)
{
	if (!Ft || (Ft == Fs))
		mVUlow.isNOP = 1;
	analyzeReg1(mVU, Fs, mVUlow.VF_read[0]);
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
}

__fi void mVUanalyzeLQ(mV, int Ft, int Is, bool writeIs)
{
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
	if (!Ft)
	{
		if (writeIs && Is)
		{
			mVUlow.noWriteVF = 1;
		}
		else
		{
			mVUlow.isNOP = 1;
		}
	}
	if (writeIs)
	{
		analyzeVIreg2(mVU, Is, mVUlow.VI_write, 1);
	}
}

__fi void mVUanalyzeSQ(mV, int Fs, int It, bool writeIt)
{
	mVUlow.isMemWrite = true;
	analyzeReg1(mVU, Fs, mVUlow.VF_read[0]);
	analyzeVIreg1(mVU, It, mVUlow.VI_read[0]);
	if (writeIt)
	{
		analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
	}
}

__fi void mVUanalyzeR1(mV, int Fs, int Fsf)
{
	analyzeReg5(mVU, Fs, Fsf, mVUlow.VF_read[0]);
	analyzeRreg();
}

__fi void mVUanalyzeR2(mV, int Ft, bool canBeNOP)
{
	if (!Ft)
	{
		if (canBeNOP)
			mVUlow.isNOP = 1;
		else
			mVUlow.noWriteVF = 1;
	}
	analyzeReg2(mVU, Ft, mVUlow.VF_write, 1);
	analyzeRreg();
}

__ri void flagSet(mV, bool setMacFlag)
{
	int curPC = iPC;
	int calcOPS = 0;

	for (int i = mVUcount, j = 0; i > 0; i--, j++)
	{
		j += mVUstall;
		incPC(-2);

		if (calcOPS >= 4 && mVUup.VF_write.reg)
			break;

		if (sFLAG.doFlag && (j >= 3))
		{
			if (setMacFlag)
				mFLAG.doFlag = 1;
			sFLAG.doNonSticky = 1;
			calcOPS++;
		}
	}

	iPC = curPC;
	setCode();
}

__ri void mVUanalyzeSflag(mV, int It)
{
	mVUlow.readFlags = true;
	analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
	if (!It)
	{
		mVUlow.isNOP = 1;
	}
	else
	{
		mVUinfo.swapOps = 1;
		flagSet(mVU, 0);
		if (mVUcount < 4)
		{
			if (!(mVUpBlock->pState.needExactMatch & 1))
				DevCon.WriteLn(Color_Green, "microVU%d: pState's sFlag Info was expected to be set [%04x]", getIndex, xPC);
		}
	}
}

__ri void mVUanalyzeFSSET(mV)
{
	mVUlow.isFSSET = 1;
	mVUlow.readFlags = true;
}

__ri void mVUanalyzeMflag(mV, int Is, int It)
{
	mVUlow.readFlags = true;
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
	if (!It)
	{
		mVUlow.isNOP = 1;
	}
	else
	{
		mVUinfo.swapOps = 1;
		flagSet(mVU, 1);
		if (mVUcount < 4)
		{
			if (!(mVUpBlock->pState.needExactMatch & 2))
				DevCon.WriteLn(Color_Green, "microVU%d: pState's mFlag Info was expected to be set [%04x]", getIndex, xPC);
		}
	}
}

__fi void mVUanalyzeCflag(mV, int It)
{
	mVUinfo.swapOps = 1;
	mVUlow.readFlags = true;
	if (mVUcount < 4)
	{
		if (!(mVUpBlock->pState.needExactMatch & 4))
			DevCon.WriteLn(Color_Green, "microVU%d: pState's cFlag Info was expected to be set [%04x]", getIndex, xPC);
	}
	analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
}

__fi void mVUanalyzeXGkick(mV, int Fs, int xCycles)
{
	mVUlow.isKick = true;
	mVUregs.xgkickcycles = 0;
	mVUlow.kickcycles = 0;
	analyzeVIreg1(mVU, Fs, mVUlow.VI_read[0]);
	if (!CHECK_XGKICKHACK)
	{
		analyzeXGkick1();
		analyzeXGkick2(xCycles);
	}
}

static void analyzeBranchVI(mV, int xReg, bool& infoVar)
{
	if (!xReg)
		return;
	if (mVUstall)
	{
		DevCon.Warning("microVU%d: %d cycle stall on branch instruction [%04x]", getIndex, mVUstall, xPC);
		return;
	}
	int i, j = 0;
	int cyc  = 0;
	int iEnd = 4;
	int bPC  = iPC;
	incPC2(-2);
	for (i = 0; i < iEnd && cyc < iEnd; i++)
	{
		if (i && mVUstall)
		{
			DevCon.Warning("microVU%d: Branch VI-Delay with %d cycle stall (%d) [%04x]", getIndex, mVUstall, i, xPC);
		}
		if (i == (int)mVUcount)
		{
			bool warn = false;

			if (i == 1)
				warn = true;

			if (mVUpBlock->pState.viBackUp == xReg)
			{
				DevCon.WriteLn(Color_Green, "microVU%d: Loading Branch VI value from previous block", getIndex);

				if (i == 0)
					warn = true;

				infoVar = true;
				j = i;
				i++;
			}
			if (warn)
				DevCon.Warning("microVU%d: Branch VI-Delay with small block (%d) [%04x]", getIndex, i, xPC);
			break;
		}
		if ((mVUlow.VI_write.reg == xReg) && mVUlow.VI_write.used)
		{
			if (mVUlow.readFlags)
			{
				if (i)
					DevCon.Warning("microVU%d: Branch VI-Delay with Read Flags Set (%d) [%04x]", getIndex, i, xPC);
				break;
			}
			j = i;
		}
		else if (i == 0)
		{
			break;
		}
		cyc += mVUstall + 1;
		incPC2(-2);
	}

	if (i)
	{
		if (!infoVar)
		{
			iPC = bPC;
			incPC2(-2 * (j + 1));
			mVUlow.backupVI = true;
			infoVar = true;
		}
		iPC = bPC;
		DevCon.WriteLn(Color_Green, "microVU%d: Branch VI-Delay (%d) [%04x][%03d]", getIndex, j + 1, xPC, mVU.prog.cur->idx);
	}
	else
	{
		iPC = bPC;
	}
}

__ri int mVUbranchCheck(mV)
{
	if (!mVUcount && !isEvilBlock)
		return 0;

	if (isEvilBlock)
	{
		mVUlow.evilBranch = true;
		mVUregs.blockType = 2;
		mVUregs.needExactMatch |= 7;
		mVUregs.flagInfo = 0;

		if (mVUlow.branch == 2 || mVUlow.branch == 10)
		{
			Console.Error("microVU%d: %s in branch, branch delay slot requires link [%04x] - If game broken report to PCSX2 Team", mVU.index,
				branchSTR[mVUlow.branch & 0xf], xPC);
		}
		else
		{
			DevCon.Warning("microVU%d: %s in branch, branch delay slot! [%04x] - If game broken report to PCSX2 Team", mVU.index,
				branchSTR[mVUlow.branch & 0xf], xPC);
		}
		return 1;
	}

	incPC(-2);

	if (mVUlow.branch)
	{
		u32 branchType = mVUlow.branch;
		if (doBranchInDelaySlot)
		{
			mVUlow.badBranch = true;
			incPC(2);
			mVUlow.evilBranch = true;

			mVUregs.blockType = 2;

			mVUregs.needExactMatch |= 7;
			mVUregs.flagInfo = 0;
			DevCon.Warning("microVU%d: %s in %s delay slot! [%04x]  - If game broken report to PCSX2 Team", mVU.index,
				branchSTR[mVUlow.branch & 0xf], branchSTR[branchType & 0xf], xPC);
			return 1;
		}
		else
		{
			incPC(2);
			mVUlow.isNOP = true;
			DevCon.Warning("microVU%d: %s in %s delay slot! [%04x]", mVU.index,
				branchSTR[mVUlow.branch & 0xf], branchSTR[branchType & 0xf], xPC);
			return 0;
		}
	}
	incPC(2);
	return 0;
}

__fi void mVUanalyzeCondBranch1(mV, int Is)
{
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	if (!mVUbranchCheck(mVU))
	{
		analyzeBranchVI(mVU, Is, mVUlow.memReadIs);
	}
}

__fi void mVUanalyzeCondBranch2(mV, int Is, int It)
{
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	analyzeVIreg1(mVU, It, mVUlow.VI_read[1]);
	if (!mVUbranchCheck(mVU))
	{
		analyzeBranchVI(mVU, Is, mVUlow.memReadIs);
		analyzeBranchVI(mVU, It, mVUlow.memReadIt);
	}
}

__fi void mVUanalyzeNormBranch(mV, int It, bool isBAL)
{
	mVUbranchCheck(mVU);
	if (isBAL)
	{
		analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
		if(!mVUlow.evilBranch)
			setConstReg(It, bSaveAddr);
	}
}

__ri void mVUanalyzeJump(mV, int Is, int It, bool isJALR)
{
	mVUlow.branch = (isJALR) ? 10 : 9;
	mVUbranchCheck(mVU);
	if (mVUconstReg[Is].isValid && doConstProp)
	{
		mVUlow.constJump.isValid  = 1;
		mVUlow.constJump.regValue = mVUconstReg[Is].regValue;
	}
	analyzeVIreg1(mVU, Is, mVUlow.VI_read[0]);
	if (isJALR)
	{
		analyzeVIreg2(mVU, It, mVUlow.VI_write, 1);
		if (!mVUlow.evilBranch)
			setConstReg(It, bSaveAddr);
	}
}
