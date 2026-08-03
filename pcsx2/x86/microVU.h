// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <deque>
#include <algorithm>
#include <memory>
#include "Common.h"
#include "VU.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "iR5900.h"
#include "R5900OpcodeTables.h"
#include "common/emitter/x86emitter.h"
#include "microVU_Misc.h"
#include "microVU_IR.h"
#include "microVU_Profiler.h"
#include "common/Perf.h"

class microBlockManager;

struct microBlockLink
{
	microBlock block;
	microBlockLink* next;
};

struct microBlockLinkRef
{
	microBlock* pBlock;
	u64 quick;
};

struct microRange
{
	s32 start;
	s32 end;
};

#define mProgSize (0x4000 / 4)
struct microProgram
{
	u32                data [mProgSize];
	microBlockManager* block[mProgSize / 2];
	std::deque<microRange>* ranges;
	u32 startPC;
	int idx;
};

typedef std::deque<microProgram*> microProgramList;

struct microProgramQuick
{
	microBlockManager* block;
	microProgram*      prog;
};

struct microProgManager
{
	microIR<mProgSize> IRinfo;
	microProgramList*  prog [mProgSize/2];
	microProgramQuick  quick[mProgSize/2];
	microProgram*      cur;
	int                total;
	int                isSame;
	int                cleared;
	u32                curFrame;
	u8*                x86ptr;
	u8*                x86start;
	u8*                x86end;
	microRegInfo       lpState;
};

static const uint mVUcacheSafeZone =  3;

struct microVU
{

	alignas(16) u32 statFlag[4];
	alignas(16) u32 macFlag [4];
	alignas(16) u32 clipFlag[4];
	alignas(16) u32 xmmCTemp[4];
	alignas(16) u32 xmmBackup[16][4];

	u32 index;
	u32 cop2;
	u32 vuMemSize;
	u32 microMemSize;
	u32 progSize;
	u32 progMemMask;
	u32 cacheSize;

	microProgManager               prog;
	microProfiler                  profiler;
	std::unique_ptr<microRegAlloc> regAlloc;
	std::FILE*                     logFile;

	u8* cache;
	u8* startFunct;
	u8* exitFunct;
	u8* startFunctXG;
	u8* exitFunctXG;
	u8* compareStateF;
	u8* waitMTVU;
	u8* copyPLState;
	u8* resumePtrXG;
	u32 code;
	u32 divFlag;
	u32 VIbackup;
	u32 VIxgkick;
	u32 branch;
	u32 badBranch;
	u32 evilBranch;
	u32 evilevilBranch;
	u32 p;
	u32 q;
	u32 totalCycles;
	s32 cycles;

	VURegs& regs() const { return ::vuRegs[index]; }
	void* textPtr() const { return (index && THREAD_VU1) ? (void*)&regs().VF[9] : (void*)R5900_TEXTPTR; }

	__fi REG_VI& getVI(uint reg) const { return regs().VI[reg]; }
	__fi VECTOR& getVF(uint reg) const { return regs().VF[reg]; }
	__fi VIFregisters& getVifRegs() const
	{
		return (index && THREAD_VU1) ? vu1Thread.vifRegs : regs().GetVifRegs();
	}

	__fi u32 compareState(microRegInfo* lhs, microRegInfo* rhs) const {
		return reinterpret_cast<u32(*)(void*, void*)>(compareStateF)(lhs, rhs);
	}
};

class microBlockManager
{
private:
	microBlockLink *qBlockList, *qBlockEnd;
	microBlockLink *fBlockList, *fBlockEnd;
	std::vector<microBlockLinkRef> quickLookup;
	int qListI, fListI;

public:
	inline int getFullListCount() const { return fListI; }
	microBlockManager()
	{
		qListI = fListI = 0;
		qBlockEnd = qBlockList = nullptr;
		fBlockEnd = fBlockList = nullptr;
	}
	~microBlockManager() { reset(); }
	void reset()
	{
		for (microBlockLink* linkI = qBlockList; linkI != nullptr;)
		{
			microBlockLink* freeI = linkI;
			safe_delete_array(linkI->block.jumpCache);
			linkI = linkI->next;
			_aligned_free(freeI);
		}
		for (microBlockLink* linkI = fBlockList; linkI != nullptr;)
		{
			microBlockLink* freeI = linkI;
			safe_delete_array(linkI->block.jumpCache);
			linkI = linkI->next;
			_aligned_free(freeI);
		}
		qListI = fListI = 0;
		qBlockEnd = qBlockList = nullptr;
		fBlockEnd = fBlockList = nullptr;
		quickLookup.clear();
	};
	microBlock* add(microVU& mVU, microBlock* pBlock)
	{
		microBlock* thisBlock = search(mVU, &pBlock->pState);
		if (!thisBlock)
		{
			u8 fullCmp = pBlock->pState.needExactMatch;
			if (fullCmp)
				fListI++;
			else
				qListI++;

			microBlockLink*& blockList = fullCmp ? fBlockList : qBlockList;
			microBlockLink*& blockEnd  = fullCmp ? fBlockEnd  : qBlockEnd;
			microBlockLink*  newBlock  = (microBlockLink*)_aligned_malloc(sizeof(microBlockLink), 32);
			newBlock->block.jumpCache  = nullptr;
			newBlock->next             = nullptr;

			if (blockEnd)
			{
				blockEnd->next = newBlock;
				blockEnd       = newBlock;
			}
			else
			{
				blockEnd = blockList = newBlock;
			}

			std::memcpy(&newBlock->block, pBlock, sizeof(microBlock));
			thisBlock = &newBlock->block;

			quickLookup.push_back({&newBlock->block, pBlock->pState.quick64[0]});
		}
		return thisBlock;
	}
	__ri microBlock* search(microVU& mVU, microRegInfo* pState)
	{
		if (pState->needExactMatch)
		{
			microBlockLink* prevI = nullptr;
			for (microBlockLink* linkI = fBlockList; linkI != nullptr; prevI = linkI, linkI = linkI->next)
			{
				if (mVU.compareState(pState, &linkI->block.pState) == 0)
				{
					if (linkI != fBlockList)
					{
						prevI->next = linkI->next;
						linkI->next = fBlockList;
						fBlockList = linkI;
					}

					return &linkI->block;
				}
			}
		}
		else
		{
			const u64 quick64 = pState->quick64[0];
			for (const microBlockLinkRef& ref : quickLookup)
			{
				if (mVUsFlagHack)
				{
					if ((ref.quick & ~0x0C04) != (quick64 & ~0x0C04)) continue;
				}
				else if (ref.quick != quick64) continue;

				if (doConstProp && (ref.pBlock->pState.vi15 != pState->vi15))  continue;
				if (doConstProp && (ref.pBlock->pState.vi15v != pState->vi15v)) continue;
				return ref.pBlock;
			}
		}
		return nullptr;
	}
	void printInfo(int pc, bool printQuick)
	{
		int listI = printQuick ? qListI : fListI;
		if (listI < 7)
			return;
		microBlockLink* linkI = printQuick ? qBlockList : fBlockList;
		for (int i = 0; i <= listI; i++)
		{
			u32 viCRC = 0, vfCRC = 0, crc = 0, z = sizeof(microRegInfo) / 4;
			for (u32 j = 0; j < 4;  j++) viCRC -= ((u32*)linkI->block.pState.VI)[j];
			for (u32 j = 0; j < 32; j++) vfCRC -= linkI->block.pState.VF[j].x + (linkI->block.pState.VF[j].y << 8) + (linkI->block.pState.VF[j].z << 16) + (linkI->block.pState.VF[j].w << 24);
			for (u32 j = 0; j < z;  j++) crc   -= ((u32*)&linkI->block.pState)[j];
			DevCon.WriteLn(Color_Green,
				"[%04x][Block #%d][crc=%08x][q=%02d][p=%02d][xgkick=%d][vi15=%04x][vi15v=%d][viBackup=%02d]"
				"[flags=%02x][exactMatch=%x][blockType=%d][viCRC=%08x][vfCRC=%08x]",
				pc, i, crc, linkI->block.pState.q,
				linkI->block.pState.p, linkI->block.pState.xgkick, linkI->block.pState.vi15, linkI->block.pState.vi15v,
				linkI->block.pState.viBackUp, linkI->block.pState.flagInfo, linkI->block.pState.needExactMatch,
				linkI->block.pState.blockType, viCRC, vfCRC);
			linkI = linkI->next;
		}
	}
};

alignas(16) microVU microVU0;
alignas(16) microVU microVU1;

int mVUdebugNow = 0;
extern void DumpVUState(u32 n, u32 pc);

extern void mVUclear(mV, u32, u32);
extern void mVUreset(microVU& mVU, bool resetReserve);
extern void* mVUblockFetch(microVU& mVU, u32 startPC, uptr pState);
_mVUt extern void* mVUcompileJIT(u32 startPC, uptr ptr);

extern void mVUcleanUpVU0();
extern void mVUcleanUpVU1();
mVUop(mVUopU);
mVUop(mVUopL);

extern void mVUcacheProg(microVU& mVU, microProgram& prog);
extern void mVUdeleteProg(microVU& mVU, microProgram*& prog);
_mVUt extern void* mVUsearchProg(u32 startPC, uptr pState);
extern void* mVUexecuteVU0(u32 startPC, u32 cycles);
extern void* mVUexecuteVU1(u32 startPC, u32 cycles);

typedef void (*mVUrecCall)(u32, u32);
typedef void (*mVUrecCallXG)(void);

template <typename T>
void makeUnique(T& v)
{
	v.erase(unique(v.begin(), v.end()), v.end());
}

template <typename T>
void sortVector(T& v)
{
	sort(v.begin(), v.end());
}

#include "microVU_Clamp.inl"
#include "microVU_Misc.inl"
#include "microVU_Log.inl"
#include "microVU_Analyze.inl"
#include "microVU_Alloc.inl"
#include "microVU_Upper.inl"
#include "microVU_Lower.inl"
#include "microVU_Tables.inl"
#include "microVU_Flags.inl"
#include "microVU_Branch.inl"
#include "microVU_Compile.inl"
#include "microVU_Execute.inl"
#include "microVU_Macro.inl"
