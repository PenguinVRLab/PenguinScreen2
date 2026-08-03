// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "microVU.h"
#include <array>

struct regCycleInfo
{
	u8 x : 4;
	u8 y : 4;
	u8 z : 4;
	u8 w : 4;
};

union alignas(16) microRegInfo
{
	struct
	{
		union
		{
			struct
			{
				u8 needExactMatch;
				u8 flagInfo;
				u8 q;
				u8 p;
				u8 xgkick;
				u8 viBackUp;
				u8 blockType;
				u8 r;
			};
			u64 quick64[1];
			u32 quick32[2];
		};

		u32 xgkickcycles;
		u8 unused;
		u8 vi15v;
		u16 vi15;

		struct
		{
			u8 VI[16];
			regCycleInfo VF[32];
		};
	};

	u128 full128[96 / sizeof(u128)];
	u64  full64[96 / sizeof(u64)];
	u32  full32[96 / sizeof(u32)];
};

static_assert(sizeof(microRegInfo) == 96, "microRegInfo was not 96 bytes");

struct microProgram;
struct microJumpCache
{
	microJumpCache() : prog(NULL), x86ptrStart(NULL) {}
	microProgram* prog;
	void* x86ptrStart;
};

struct alignas(16) microBlock
{
	microRegInfo    pState;
	microRegInfo    pStateEnd;
	u8*             x86ptrStart;
	microJumpCache* jumpCache;
};

struct microTempRegInfo
{
	regCycleInfo VF[2];
	u8 VFreg[2];
	u8 VI;
	u8 VIreg;
	u8 q;
	u8 p;
	u8 r;
	u8 xgkick;
};

struct microVFreg
{
	u8 reg;
	u8 x;
	u8 y;
	u8 z;
	u8 w;
};

struct microVIreg
{
	u8 reg;
	u8 used;
};

struct microConstInfo
{
	u8  isValid;
	u32 regValue;
};

struct microUpperOp
{
	bool eBit;
	bool iBit;
	bool mBit;
	bool tBit;
	bool dBit;
	microVFreg VF_write;
	microVFreg VF_read[2];
};

struct microLowerOp
{
	microVFreg VF_write;
	microVFreg VF_read[2];
	microVIreg VI_write;
	microVIreg VI_read[2];
	microConstInfo constJump;
	u32  branch;
	u32  kickcycles;
	bool badBranch;
	bool evilBranch;
	bool isNOP;
	bool isFSSET;
	bool noWriteVF;
	bool backupVI;
	bool memReadIs;
	bool memReadIt;
	bool readFlags;
	bool isMemWrite;
	bool isKick;
};

struct microFlagInst
{
	bool doFlag;
	bool doNonSticky;
	u8   write;
	u8   lastWrite;
	u8   read;
};

struct microFlagCycles
{
	int xStatus[4];
	int xMac[4];
	int xClip[4];
	int cycles;
};

struct microOp
{
	u8   stall;
	bool isBadOp;
	bool isEOB;
	bool isBdelay;
	bool swapOps;
	bool backupVF;
	bool doXGKICK;
	u32  XGKICKPC;
	bool doDivFlag;
	int  readQ;
	int  writeQ;
	int  readP;
	int  writeP;
	microFlagInst sFlag;
	microFlagInst mFlag;
	microFlagInst cFlag;
	microUpperOp  uOp;
	microLowerOp  lOp;
};

template <u32 pSize>
struct microIR
{
	microBlock       block;
	microBlock*      pBlock;
	microTempRegInfo regsTemp;
	microOp          info[pSize / 2];
	microConstInfo   constReg[16];
	u8  branch;
	u32 cycles;
	u32 count;
	u32 curPC;
	u32 startPC;
	u32 sFlagHack;
};

#define MVURALOG(...)

struct microMapXMM
{
	int  VFreg;
	int  xyzw;
	int  count;
	bool isNeeded;
	bool isZero;
};

struct microMapGPR
{
	int VIreg;
	int count;
	bool isNeeded;
	bool dirty;
	bool isZeroExtended;
	bool usable;
};

class microRegAlloc
{
protected:
	static const int xmmTotal = iREGCNT_XMM - 1;
	static const int gprTotal = iREGCNT_GPR;

	std::array<microMapXMM, xmmTotal> xmmMap;
	std::array<microMapGPR, gprTotal> gprMap;

	int         counter;
	int         index;

	_xmmregs*   pxmmregs;

	bool        regAllocCOP2;

	VURegs& regs() const { return ::vuRegs[index]; }
	__fi REG_VI& getVI(uint reg) const { return regs().VI[reg]; }
	__fi VECTOR& getVF(uint reg) const { return regs().VF[reg]; }

	__ri void loadIreg(const xmm& reg, int xyzw)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].VIreg == REG_I)
			{
				xMOVDZX(reg, xRegister32(i));
				if (!_XYZWss(xyzw))
					xSHUF.PS(reg, reg, 0);

				return;
			}
		}

		xMOVSSZX(reg, ptr32[&getVI(REG_I)]);
		if (!_XYZWss(xyzw))
			xSHUF.PS(reg, reg, 0);
	}

	int findFreeRegRec(int startIdx)
	{
		for (int i = startIdx; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded)
			{
				int x = findFreeRegRec(i + 1);
				if (x == -1)
					return i;
				return ((xmmMap[i].count < xmmMap[x].count) ? i : x);
			}
		}
		return -1;
	}

	int findFreeReg(int vfreg)
	{
		if (regAllocCOP2)
		{
			return _allocVFtoXMMreg(vfreg, 0);
		}

		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded && (xmmMap[i].VFreg < 0))
			{
				return i;
			}
		}
		int x = findFreeRegRec(0);
		pxAssertMsg(x >= 0, "microVU register allocation failure!");
		return x;
	}

	int findFreeGPRRec(int startIdx)
	{
		for (int i = startIdx; i < gprTotal; i++)
		{
			if (gprMap[i].usable && !gprMap[i].isNeeded)
			{
				int x = findFreeGPRRec(i + 1);
				if (x == -1)
					return i;
				return ((gprMap[i].count < gprMap[x].count) ? i : x);
			}
		}
		return -1;
	}

	int findFreeGPR(int vireg)
	{
		if (regAllocCOP2)
			return _allocX86reg(X86TYPE_VIREG, vireg, MODE_COP2);

		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].usable && !gprMap[i].isNeeded && (gprMap[i].VIreg < 0))
			{
				return i;
			}
		}
		int x = findFreeGPRRec(0);
		pxAssertMsg(x >= 0, "microVU register allocation failure!");
		return x;
	}

	void writeVIBackup(const xRegisterInt& reg);

public:
	microRegAlloc(int _index)
	{
		index = _index;

		gprMap.fill({0, 0, false, false, false, false});
		for (int i = 0; i < gprTotal; i++)
		{
			if (i == gprT1.GetId() || i == gprT2.GetId() ||
				i == gprF0.GetId() || i == gprF1.GetId() || i == gprF2.GetId() || i == gprF3.GetId() ||
				i == rsp.GetId())
			{
				continue;
			}

			gprMap[i].usable = true;
		}

		reset(false);
	}

	void reset(bool cop2mode)
	{
		regAllocCOP2 = false;

		for (int i = 0; i < xmmTotal; i++)
			clearReg(i);
		for (int i = 0; i < gprTotal; i++)
			clearGPR(i);

		counter = 0;
		regAllocCOP2 = cop2mode;
		pxmmregs = cop2mode ? xmmregs : nullptr;

		if (cop2mode)
		{
			for (int i = 0; i < xmmTotal; i++)
			{
				if (!pxmmregs[i].inuse || pxmmregs[i].type != XMMTYPE_VFREG)
					continue;

				if (pxmmregs[i].reg >= 0)
				{
					MVURALOG("Preserving VF reg %d in host reg %d across instruction\n", pxmmregs[i].reg, i);
					pxAssert(pxmmregs[i].reg >= 0);
					pxmmregs[i].needed = false;
					xmmMap[i].isNeeded = false;
					xmmMap[i].VFreg = pxmmregs[i].reg;
					xmmMap[i].xyzw = ((pxmmregs[i].mode & MODE_WRITE) != 0) ? 0xf : 0x0;
				}
			}

			for (int i = 0; i < gprTotal; i++)
			{
				if (!x86regs[i].inuse || x86regs[i].type != X86TYPE_VIREG)
					continue;

				if (x86regs[i].reg >= 0)
				{
					MVURALOG("Preserving VI reg %d in host reg %d across instruction\n", x86regs[i].reg, i);
					x86regs[i].needed = false;
					gprMap[i].isNeeded = false;
					gprMap[i].isZeroExtended = false;
					gprMap[i].VIreg = x86regs[i].reg;
					gprMap[i].dirty = ((x86regs[i].mode & MODE_WRITE) != 0);
				}
			}
		}

		gprMap[RTEXTPTR.GetId()].usable = !xGetTextPtr();
		gprMap[RFASTMEMBASE.GetId()].usable = !cop2mode || !CHECK_FASTMEM;
	}

	int getXmmCount()
	{
		return xmmTotal + 1;
	}

	int getFreeXmmCount()
	{
		int count = 0;

		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded && (xmmMap[i].VFreg < 0))
				count++;
		}

		return count;
	}

	bool hasRegVF(int vfreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (xmmMap[i].VFreg == vfreg)
				return true;
		}

		return false;
	}

	int getRegVF(int i)
	{
		return (i < xmmTotal) ? xmmMap[i].VFreg : -1;
	}

	int getGPRCount()
	{
		return gprTotal;
	}

	int getFreeGPRCount()
	{
		int count = 0;

		for (int i = 0; i < gprTotal; i++)
		{
			if (!gprMap[i].usable && (gprMap[i].VIreg < 0))
				count++;
		}

		return count;
	}

	bool hasRegVI(int vireg)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].VIreg == vireg)
				return true;
		}

		return false;
	}

	int getRegVI(int i)
	{
		return (i < gprTotal) ? gprMap[i].VIreg : -1;
	}

	void flushAll(bool clearState = true)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			writeBackReg(xmm(i));
			if (clearState)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			writeBackReg(xRegister32(i), true);
			if (clearState)
				clearGPR(i);
		}
	}

	void flushCallerSavedRegisters(bool clearNeeded = false)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			writeBackReg(xmm(i));
			if (clearNeeded || !xmmMap[i].isNeeded)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			if (!xRegister32::IsCallerSaved(i))
				continue;

			writeBackReg(xRegister32(i), true);
			if (clearNeeded || !gprMap[i].isNeeded)
				clearGPR(i);
		}
	}

	void flushPartialForCOP2()
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			microMapXMM& clear = xmmMap[i];

			if (pxmmregs[i].inuse && pxmmregs[i].type == XMMTYPE_VFREG)
			{
				if (clear.xyzw != 0 && clear.xyzw != 0xf)
					writeBackReg(xRegisterSSE::GetInstance(i), false);

				if (clear.VFreg <= 0)
				{
					_freeXMMreg(i);
				}
			}

			clear = {-1, 0, 0, false, false};
		}

		for (int i = 0; i < gprTotal; i++)
		{
			microMapGPR& clear = gprMap[i];
			if (clear.VIreg < 0)
				clearGPR(i);
		}
	}

	void TDwritebackAll()
	{

		for (int i = 0; i < xmmTotal; i++)
		{
			microMapXMM& mapX = xmmMap[xmm(i).Id];

			if ((mapX.VFreg > 0) && mapX.xyzw)
			{
				if (mapX.VFreg == 33)
					xMOVSS(ptr32[&getVI(REG_I)], xmm(i));
				else if (mapX.VFreg == 32)
					mVUsaveReg(xmm(i), ptr[&regs().ACC], mapX.xyzw, 1);
				else
					mVUsaveReg(xmm(i), ptr[&getVF(mapX.VFreg)], mapX.xyzw, 1);
			}
		}

		for (int i = 0; i < gprTotal; i++)
			writeBackReg(xRegister32(i), false);
	}

	bool checkVFClamp(int regId)
	{
		if (regId != xmmPQ.Id && ((xmmMap[regId].VFreg == 33 && !EmuConfig.Gamefixes.IbitHack) || xmmMap[regId].isZero))
			return false;
		else
			return true;
	}

	bool checkCachedReg(int regId)
	{
		if (regId < xmmTotal)
			return xmmMap[regId].VFreg >= 0;
		else
			return false;
	}

	bool checkCachedGPR(int regId)
	{
		if (regId < gprTotal)
			return gprMap[regId].VIreg >= 0 || gprMap[regId].isNeeded;
		else
			return false;
	}

	void clearReg(const xmm& reg) { clearReg(reg.Id); }
	void clearReg(int regId)
	{
		microMapXMM& clear = xmmMap[regId];
		if (regAllocCOP2 && (clear.isNeeded || clear.VFreg >= 0))
		{
			pxAssert(pxmmregs[regId].type == XMMTYPE_VFREG);
			pxmmregs[regId].inuse = false;
		}

		clear = {-1, 0, 0, false, false};
	}

	void clearRegVF(int VFreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (xmmMap[i].VFreg == VFreg)
				clearReg(i);
		}
	}

	void clearRegCOP2(int xmmReg)
	{
		if (regAllocCOP2)
			clearReg(xmmReg);
	}

	void updateCOP2AllocState(int rn)
	{
		if (!regAllocCOP2)
			return;

		const bool dirty = (xmmMap[rn].VFreg > 0 && xmmMap[rn].xyzw != 0);
		pxAssert(pxmmregs[rn].type == XMMTYPE_VFREG);
		pxmmregs[rn].reg = xmmMap[rn].VFreg;
		pxmmregs[rn].mode = dirty ? (MODE_READ | MODE_WRITE) : MODE_READ;
		pxmmregs[rn].needed = xmmMap[rn].isNeeded;
	}

	void writeBackReg(const xmm& reg, bool invalidateRegs = true)
	{
		microMapXMM& mapX = xmmMap[reg.Id];

		if ((mapX.VFreg > 0) && mapX.xyzw)
		{
			if (mapX.VFreg == 33)
				xMOVSS(ptr32[&getVI(REG_I)], reg);
			else if (mapX.VFreg == 32)
				mVUsaveReg(reg, ptr[&regs().ACC], mapX.xyzw, true);
			else
				mVUsaveReg(reg, ptr[&getVF(mapX.VFreg)], mapX.xyzw, true);

			if (invalidateRegs)
			{
				for (int i = 0; i < xmmTotal; i++)
				{
					microMapXMM& mapI = xmmMap[i];

					if ((i == reg.Id) || mapI.isNeeded)
						continue;

					if (mapI.VFreg == mapX.VFreg)
					{
						if (mapI.xyzw && mapI.xyzw < 0xf)
							DevCon.Error("microVU Error: writeBackReg() [%d]", mapI.VFreg);
						clearReg(i);
					}
				}
			}
			if (mapX.xyzw == 0xf)
			{
				mapX.count    = counter;
				mapX.xyzw     = 0;
				mapX.isNeeded = false;
				updateCOP2AllocState(reg.Id);
				return;
			}
			clearReg(reg);
		}
		else if (mapX.xyzw)
		{
			clearReg(reg);
		}
	}

	void clearNeeded(const xmm& reg)
	{

		if ((reg.Id < 0) || (reg.Id >= xmmTotal))
			return;

		microMapXMM& clear = xmmMap[reg.Id];
		clear.isNeeded = false;
		if (clear.xyzw)
		{
			if (clear.VFreg > 0)
			{
				int mergeRegs = 0;
				if (clear.xyzw < 0xf)
					mergeRegs = 1;
				for (int i = 0; i < xmmTotal; i++)
				{
					if (i == reg.Id)
						continue;
					microMapXMM& mapI = xmmMap[i];
					if (mapI.VFreg == clear.VFreg)
					{
						if (mapI.xyzw && mapI.xyzw < 0xf)
						{
							DevCon.Error("microVU Error: clearNeeded() [%d]", mapI.VFreg);
						}
						if (mergeRegs == 1)
						{
							mVUmergeRegs(xmm(i), reg, clear.xyzw, true);
							mapI.xyzw  = 0xf;
							mapI.count = counter;
							mergeRegs  = 2;
							updateCOP2AllocState(i);
						}
						else
							clearReg(i);
					}
				}
				if (mergeRegs == 2)
					clearReg(reg);
				else if (mergeRegs == 1)
					writeBackReg(reg);
			}
			else
				clearReg(reg);
		}
		else if (regAllocCOP2 && clear.VFreg < 0)
		{
			pxAssert(pxmmregs[reg.Id].type == XMMTYPE_VFREG);
			pxmmregs[reg.Id].inuse = false;
		}
	}

	const xmm& allocReg(int vfLoadReg = -1, int vfWriteReg = -1, int xyzw = 0, bool cloneWrite = true)
	{
		counter++;
		if (vfLoadReg >= 0)
		{
			for (int i = 0; i < xmmTotal; i++)
			{
				const xmm& xmmI = xmm::GetInstance(i);
				microMapXMM& mapI = xmmMap[i];
				if ((mapI.VFreg == vfLoadReg)
				 && (!mapI.xyzw
				  || (mapI.VFreg && (mapI.xyzw == 0xf))))
				{
					int z = i;
					if (vfWriteReg >= 0)
					{
						if (cloneWrite)
						{
							z = findFreeReg(vfWriteReg);
							const xmm& xmmZ = xmm::GetInstance(z);
							writeBackReg(xmmZ);

							if (xyzw == 4)
								xPSHUF.D(xmmZ, xmmI, 1);
							else if (xyzw == 2)
								xPSHUF.D(xmmZ, xmmI, 2);
							else if (xyzw == 1)
								xPSHUF.D(xmmZ, xmmI, 3);
							else if (z != i)
								xMOVAPS(xmmZ, xmmI);

							mapI.count = counter;
						}
						else
						{
							if ((vfLoadReg != vfWriteReg) || (xyzw != 0xf))
								writeBackReg(xmmI);

							if (xyzw == 4)
								xPSHUF.D(xmmI, xmmI, 1);
							else if (xyzw == 2)
								xPSHUF.D(xmmI, xmmI, 2);
							else if (xyzw == 1)
								xPSHUF.D(xmmI, xmmI, 3);
						}
						xmmMap[z].VFreg = vfWriteReg;
						xmmMap[z].xyzw = xyzw;
						xmmMap[z].isZero = (vfLoadReg == 0);
					}
					xmmMap[z].count = counter;
					xmmMap[z].isNeeded = true;
					updateCOP2AllocState(z);

					return xmm::GetInstance(z);
				}
			}
		}
		int x = findFreeReg((vfWriteReg >= 0) ? vfWriteReg : vfLoadReg);
		const xmm& xmmX = xmm::GetInstance(x);
		writeBackReg(xmmX);

		if (vfWriteReg >= 0)
		{
			if ((vfLoadReg == 0) && !(xyzw & 1))
				xPXOR(xmmX, xmmX);
			else if (vfLoadReg == 33)
				loadIreg(xmmX, xyzw);
			else if (vfLoadReg == 32)
				mVUloadReg(xmmX, ptr[&regs().ACC], xyzw);
			else if (vfLoadReg >= 0)
				mVUloadReg(xmmX, ptr[&getVF(vfLoadReg)], xyzw);

			xmmMap[x].VFreg = vfWriteReg;
			xmmMap[x].xyzw  = xyzw;
		}
		else
		{
			if (vfLoadReg == 33)
				loadIreg(xmmX, 0xf);
			else if (vfLoadReg == 32)
				xMOVAPS (xmmX, ptr128[&regs().ACC]);
			else if (vfLoadReg >= 0)
				xMOVAPS (xmmX, ptr128[&getVF(vfLoadReg)]);

			xmmMap[x].VFreg = vfLoadReg;
			xmmMap[x].xyzw  = 0;
		}
		xmmMap[x].isZero = (vfLoadReg == 0);
		xmmMap[x].count    = counter;
		xmmMap[x].isNeeded = true;
		updateCOP2AllocState(x);
		return xmmX;
	}

	void clearGPR(const xRegisterInt& reg) { clearGPR(reg.GetId()); }

	void clearGPR(int regId)
	{
		microMapGPR& clear = gprMap[regId];

		if (regAllocCOP2)
		{
			if (x86regs[regId].inuse && x86regs[regId].type == X86TYPE_VIREG)
			{
				pxAssert(x86regs[regId].reg == clear.VIreg);
				_freeX86regWithoutWriteback(regId);
			}
		}

		clear.VIreg = -1;
		clear.count = 0;
		clear.isNeeded = 0;
		clear.dirty = false;
		clear.isZeroExtended = false;
	}

	void clearGPRCOP2(int regId)
	{
		if (regAllocCOP2)
			clearGPR(regId);
	}

	void updateCOP2AllocState(const xRegisterInt& reg)
	{
		if (!regAllocCOP2)
			return;

		const u32 rn = reg.GetId();
		const bool dirty = (gprMap[rn].VIreg >= 0 && gprMap[rn].dirty);
		pxAssert(x86regs[rn].type == X86TYPE_VIREG);
		x86regs[rn].reg = gprMap[rn].VIreg;
		x86regs[rn].counter = gprMap[rn].count;
		x86regs[rn].mode = dirty ? (MODE_READ | MODE_WRITE) : MODE_READ;
		x86regs[rn].needed = gprMap[rn].isNeeded;
	}

	void writeBackReg(const xRegisterInt& reg, bool clearDirty)
	{
		microMapGPR& mapX = gprMap[reg.GetId()];
		pxAssert(mapX.usable || !mapX.dirty);
		if (mapX.dirty)
		{
			pxAssert(mapX.VIreg > 0);
			if (mapX.VIreg < 16)
				xMOV(ptr16[&getVI(mapX.VIreg)], xRegister16(reg));
			if (clearDirty)
			{
				mapX.dirty = false;
				updateCOP2AllocState(reg);
			}
		}
	}

	void clearNeeded(const xRegisterInt& reg)
	{
		pxAssert(reg.GetId() < gprTotal);
		microMapGPR& clear = gprMap[reg.GetId()];
		clear.isNeeded = false;
		if (regAllocCOP2)
			x86regs[reg.GetId()].needed = false;
	}

	void unbindAnyVIAllocations(int reg, bool& backup)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			microMapGPR& mapI = gprMap[i];
			if (mapI.VIreg == reg)
			{
				if (backup)
				{
					writeVIBackup(xRegister32(i));
					backup = false;
				}

				if (mapI.isNeeded)
				{
					MVURALOG("  unbind %d to %d for write\n", i, reg);
					if (regAllocCOP2)
					{
						pxAssert(x86regs[i].type == X86TYPE_VIREG && x86regs[i].reg == static_cast<u8>(mapI.VIreg));
						x86regs[i].reg = -1;
					}

					mapI.VIreg = -1;
					mapI.dirty = false;
					mapI.isZeroExtended = false;
				}
				else
				{
					MVURALOG("  clear %d to %d for write\n", i, reg);
					clearGPR(i);
				}

				for (int j = i + 1; j < gprTotal; j++)
				{
					pxAssert(gprMap[j].VIreg != reg);
				}

				break;
			}
		}
	}

	const xRegister32& allocGPR(int viLoadReg = -1, int viWriteReg = -1, bool backup = false, bool zext_if_dirty = false)
	{

		const int this_counter = regAllocCOP2 ? (g_x86AllocCounter++) : (counter++);
		if (viLoadReg == 0 || viWriteReg == 0)
		{
			if (viWriteReg == 0)
			{
				int x = findFreeGPR(-1);
				const xRegister32& gprX = xRegister32::GetInstance(x);
				writeBackReg(gprX, true);
				xXOR(gprX, gprX);
				gprMap[x].VIreg = -1;
				gprMap[x].dirty = false;
				gprMap[x].count = this_counter;
				gprMap[x].isNeeded = true;
				gprMap[x].isZeroExtended = true;
				MVURALOG("  alloc zero to scratch %d\n", x);
				return gprX;
			}
		}

		if (viLoadReg >= 0)
		{
			for (int i = 0; i < gprTotal; i++)
			{
				microMapGPR& mapI = gprMap[i];
				if (mapI.VIreg == viLoadReg)
				{
					gprMap[i].count = this_counter;

					if (viWriteReg >= 0)
					{
						if (viLoadReg != viWriteReg)
						{
							unbindAnyVIAllocations(viWriteReg, backup);

							int x = findFreeGPR(viWriteReg);
							const xRegister32& gprX = xRegister32::GetInstance(x);

							writeBackReg(gprX, true);

							if (backup && gprMap[x].VIreg != viWriteReg)
							{
								xMOVZX(gprX, ptr16[&getVI(viWriteReg)]);
								writeVIBackup(gprX);
								backup = false;
							}

							if (zext_if_dirty)
								xMOVZX(gprX, xRegister16(i));
							else
								xMOV(gprX, xRegister32(i));
							gprMap[x].isZeroExtended = zext_if_dirty;
							MVURALOG("  clone write %d in %d to %d for %d\n", viLoadReg, i, x, viWriteReg);
							std::swap(x, i);
						}
						else
						{
							gprMap[i].isZeroExtended = false;
						}

						gprMap[i].VIreg = viWriteReg;
						gprMap[i].dirty = true;
					}
					else if (zext_if_dirty && !gprMap[i].isZeroExtended)
					{
						xMOVZX(xRegister32(i), xRegister16(i));
						gprMap[i].isZeroExtended = true;
					}

					gprMap[i].isNeeded = true;

					if (backup)
						writeVIBackup(xRegister32(i));

					if (regAllocCOP2)
					{
						pxAssert(x86regs[i].inuse && x86regs[i].type == X86TYPE_VIREG);
						x86regs[i].reg = gprMap[i].VIreg;
						x86regs[i].mode = gprMap[i].dirty ? (MODE_WRITE | MODE_READ) : (MODE_READ);
					}

					MVURALOG("  returning cached in %d\n", i);
					return xRegister32::GetInstance(i);
				}
			}
		}

		if (viWriteReg >= 0)
			unbindAnyVIAllocations(viWriteReg, backup);

		int x = findFreeGPR(viLoadReg);
		const xRegister32& gprX = xRegister32::GetInstance(x);
		writeBackReg(gprX, true);

		if (backup && viLoadReg >= 0 && viWriteReg > 0 && viLoadReg != viWriteReg)
		{
			xMOVZX(gprX, ptr16[&getVI(viWriteReg)]);
			writeVIBackup(gprX);
			backup = false;
		}

		if (viLoadReg > 0)
			xMOVZX(gprX, ptr16[&getVI(viLoadReg)]);
		else if (viLoadReg == 0)
			xXOR(gprX, gprX);

		gprMap[x].VIreg = viLoadReg;
		gprMap[x].isZeroExtended = true;
		if (viWriteReg >= 0)
		{
			gprMap[x].VIreg = viWriteReg;
			gprMap[x].dirty = true;
			gprMap[x].isZeroExtended = false;

			if (backup)
			{
				if (viLoadReg < 0 && viWriteReg > 0)
					xMOVZX(gprX, ptr16[&getVI(viWriteReg)]);

				writeVIBackup(gprX);
			}
		}

		gprMap[x].count = this_counter;
		gprMap[x].isNeeded = true;

		if (regAllocCOP2)
		{
			pxAssert(x86regs[x].inuse && x86regs[x].type == X86TYPE_VIREG);
			x86regs[x].reg = gprMap[x].VIreg;
			x86regs[x].mode = gprMap[x].dirty ? (MODE_WRITE | MODE_READ) : (MODE_READ);
		}

		MVURALOG("  returning new %d\n", x);
		return gprX;
	}

	void moveVIToGPR(const xRegisterInt& reg, int vi, bool signext = false)
	{
		pxAssert(vi >= 0);
		if (vi == 0)
		{
			xXOR(xRegister32(reg), xRegister32(reg));
			return;
		}

		const xRegister32& srcreg = allocGPR(vi);
		if (signext)
			xMOVSX(xRegister32(reg), xRegister16(srcreg));
		else
			xMOVZX(xRegister32(reg), xRegister16(srcreg));
		clearNeeded(srcreg);
	}
};
