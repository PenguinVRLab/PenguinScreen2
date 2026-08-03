// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <deque>
#include "Gif.h"
#include "Vif.h"
#include "GS.h"
#include "GS/GSRegs.h"
#include "MTGS.h"

#include "common/boost_spsc_queue.hpp"

struct GS_Packet;
extern void Gif_MTGS_Wait(bool isMTVU);
extern void Gif_FinishIRQ();
extern bool Gif_HandlerAD(u8* pMem);
extern void Gif_HandlerAD_MTVU(u8* pMem);
extern bool Gif_HandlerAD_Debug(u8* pMem);
extern void Gif_AddBlankGSPacket(u32 size, GIF_PATH path);
extern void Gif_AddGSPacketMTVU(GS_Packet& gsPack, GIF_PATH path);
extern void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path);
extern void Gif_ParsePacket(u8* data, u32 size, GIF_PATH path);
extern void Gif_ParsePacket(GS_Packet& gsPack, GIF_PATH path);

struct Gif_Tag
{
	struct HW_Gif_Tag
	{
		u16 NLOOP : 15;
		u16 EOP : 1;
		u16 _dummy0 : 16;
		u32 _dummy1 : 14;
		u32 PRE : 1;
		u32 PRIM : 11;
		u32 FLG : 2;
		u32 NREG : 4;
		u32 REGS[2];
	} tag;

	u32 nLoop;
	u32 nRegs;
	u32 nRegIdx;
	u32 len;
	u32 cycles;
	u8 regs[16];
	bool hasAD;
	bool isValid;

	__ri Gif_Tag() { Reset(); }
	__ri Gif_Tag(u8* pMem, bool analyze = false)
	{
		setTag(pMem, analyze);
	}

	__ri void Reset() { std::memset(this, 0, sizeof(*this)); }
	__ri u8 curReg() { return regs[nRegIdx & 0xf]; }

	__ri void packedStep()
	{
		if (nLoop > 0)
		{
			nRegIdx++;
			if (nRegIdx >= nRegs)
			{
				nRegIdx = 0;
				nLoop--;
			}
		}
	}

	__ri void setTag(u8* pMem, bool analyze = false)
	{
		tag = *(HW_Gif_Tag*)pMem;
		nLoop = tag.NLOOP;
		hasAD = false;
		nRegIdx = 0;
		isValid = 1;
		len = 0;
		switch (tag.FLG)
		{
			case GIF_FLG_PACKED:
				nRegs = ((tag.NREG - 1) & 0xf) + 1;
				len = (nRegs * tag.NLOOP) * 16;
				cycles = len << 1;
				if (analyze)
					analyzeTag();
				break;
			case GIF_FLG_REGLIST:
				nRegs = ((tag.NREG - 1) & 0xf) + 1;
				len = ((nRegs * tag.NLOOP + 1) >> 1) * 16;
				cycles = len << 2;
				break;
			case GIF_FLG_IMAGE:
			case GIF_FLG_IMAGE2:
				nRegs = 0;
				len = tag.NLOOP * 16;
				cycles = len << 2;
				tag.FLG = GIF_FLG_IMAGE;
				break;
				jNO_DEFAULT;
		}
	}

	__ri void analyzeTag()
	{
#ifdef ARCH_X86
		__m128i vregs = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(tag.REGS));
		vregs = _mm_and_si128(vregs, _mm_srli_epi64(_mm_set1_epi32(0xFFFFFFFFu), (64 - nRegs * 4)));

		vregs = _mm_and_si128(_mm_unpacklo_epi8(vregs, _mm_srli_epi32(vregs, 4)), _mm_set1_epi8(0x0F));

		hasAD = (_mm_movemask_epi8(_mm_cmpeq_epi8(vregs, _mm_set1_epi8(GIF_REG_A_D))) != 0);

		_mm_storeu_si128(reinterpret_cast<__m128i*>(regs), vregs);
#elif defined(ARCH_ARM64)
		u64 REGS64;
		std::memcpy(&REGS64, tag.REGS, sizeof(u64));
		REGS64 &= (0xFFFFFFFFFFFFFFFFULL >> (64 - nRegs * 4));
		uint8x16_t vregs = vreinterpretq_u8_u64(vsetq_lane_u64(REGS64, vdupq_n_u64(0), 0));

		vregs = vandq_u8(vzip1q_u8(vregs, vshrq_n_u8(vregs, 4)), vdupq_n_u8(0x0F));

		const uint8x16_t comp = vceqq_u8(vregs, vdupq_n_u8(GIF_REG_A_D));
		hasAD = vmaxvq_u8(comp) & 1;

		vst1q_u8(regs, vregs);
#else
		hasAD = false;
		u32 t = tag.REGS[0];
		u32 i = 0;
		u32 j = std::min<u32>(nRegs, 8);
		for (; i < j; i++)
		{
			regs[i] = t & 0xf;
			hasAD |= (regs[i] == GIF_REG_A_D);
			t >>= 4;
		}
		t = tag.REGS[1];
		j = nRegs;
		for (; i < j; i++)
		{
			regs[i] = t & 0xf;
			hasAD |= (regs[i] == GIF_REG_A_D);
			t >>= 4;
		}
#endif
	}
};

struct GS_Packet
{

	u32 offset;
	u32 size;
	s32 cycles;
	s32 readAmount;
	GS_Packet() { Reset(); }
	void Reset() { std::memset(this, 0, sizeof(*this)); }
};

struct GS_SIGNAL
{
	u32 data[2];
	bool queued;
	void Reset() { std::memset(this, 0, sizeof(*this)); }
};

struct GS_FINISH
{
	bool gsFINISHFired;
	bool gsFINISHPending;

	void Reset() { std::memset(this, 0, sizeof(*this)); }
};

static __fi void incTag(u32& offset, u32& size, u32 incAmount)
{
	size += incAmount;
	offset += incAmount;
}

struct Gif_Path_MTVU
{
	u32 fakePackets;
	GS_Packet fakePacket;
	ringbuffer_base<GS_Packet, MTGS::RingBufferSize / 2> gsPackQueue;
	Gif_Path_MTVU() { Reset(); }
	void Reset()
	{
		fakePackets = 0;
		gsPackQueue.reset();
		fakePacket.Reset();
		fakePacket.size = ~0u;
	}
};

struct Gif_Path
{
	std::atomic<int> readAmount;
	u8* buffer;
	u32 buffSize;
	u32 buffLimit;
	u32 curSize;
	u32 curOffset;
	u32 dmaRewind;
	Gif_Tag gifTag;
	GS_Packet gsPack;
	GIF_PATH idx;
	GIF_PATH_STATE state;
	Gif_Path_MTVU mtvu;

	Gif_Path() { Reset(); }
	~Gif_Path() { _aligned_free(buffer); }

	void Init(GIF_PATH _idx, u32 _buffSize, u32 _buffSafeZone)
	{
		idx = _idx;
		buffSize = _buffSize;
		buffLimit = _buffSize - _buffSafeZone;
		buffer = (u8*)_aligned_malloc(buffSize, 16);
		Reset();
	}

	void Reset(bool softReset = false)
	{
		state = GIF_PATH_IDLE;
		if (softReset)
		{
			if (!isMTVU())
			{
				GUNIT_WARN("Gif Path %d - Soft Reset", idx + 1);
				gifTag.Reset();
				gsPack.Reset();
				curSize = curOffset;
				gsPack.offset = curOffset;
			}
			return;
		}
		mtvu.Reset();
		curSize = 0;
		curOffset = 0;
		readAmount = 0;
		gifTag.Reset();
		gsPack.Reset();
	}

	bool isMTVU() const { return !idx && THREAD_VU1; }
	s32 getReadAmount() { return readAmount.load(std::memory_order_acquire) + gsPack.readAmount; }
	bool hasDataRemaining() const { return curOffset < curSize; }
	bool isDone() const { return isMTVU() ? !mtvu.fakePackets : (!hasDataRemaining() && (state == GIF_PATH_IDLE || state == GIF_PATH_WAIT)); }

	void mtgsReadWait()
	{
		if (IsDevBuild)
		{
			DevCon.WriteLn(Color_Red, "Gif Path[%d] - MTGS Wait! [r=0x%x]", idx + 1, getReadAmount());
			Gif_MTGS_Wait(isMTVU());
			DevCon.WriteLn(Color_Green, "Gif Path[%d] - MTGS Wait! [r=0x%x]", idx + 1, getReadAmount());
			return;
		}
		Gif_MTGS_Wait(isMTVU());
	}

	void RealignPacket()
	{
		GUNIT_LOG("Path Buffer: Realigning packet!");
		s32 offset = curOffset - gsPack.size;
		s32 sizeToAdd = curSize - offset;
		s32 intersect = sizeToAdd - offset;
		if (intersect < 0)
			intersect = 0;
		for (;;)
		{
			s32 frontFree = offset - getReadAmount();
			if (frontFree >= sizeToAdd - intersect)
				break;
			mtgsReadWait();
		}
		if (offset < (s32)buffLimit)
		{
			if (isMTVU())
				gsPack.readAmount += buffLimit - offset;
			else
				Gif_AddBlankGSPacket(buffLimit - offset, idx);
		}
		if (intersect)
			memmove(buffer, &buffer[offset], curSize - offset);
		else
			memcpy(buffer, &buffer[offset], curSize - offset);
		curSize -= offset;
		curOffset = gsPack.size;
		gsPack.offset = 0;
	}

	void CopyGSPacketData(u8* pMem, u32 size, bool aligned = false)
	{
		if (curSize + size > buffSize)
		{
			GUNIT_LOG("CopyGSPacketData: Realigning packet!");
			RealignPacket();
		}
		for (;;)
		{
			s32 offset = curOffset - gsPack.size;
			s32 readPos = offset - getReadAmount();
			if (readPos >= 0)
				break;
			if ((s32)buffLimit + readPos > (s32)curSize + (s32)size)
				break;
			mtgsReadWait();
		}
		pxAssertMsg(curSize + size <= buffSize, "Gif Path Buffer Overflow!");
		memcpy(&buffer[curSize], pMem, size);
		curSize += size;
	}

	GS_Packet ExecuteGSPacket(bool& done)
	{
		if (mtvu.fakePackets)
		{
			mtvu.fakePackets--;
			done = true;
			return mtvu.fakePacket;
		}
		pxAssert(!isMTVU());
		for (;;)
		{
			if (!gifTag.isValid)
			{
				if (curOffset + 16 > curSize)
				{
					GUNIT_WARN("PATH %d not enough data pre tag, available %d wanted %d", gifRegs.stat.APATH, curSize - curOffset, 16);
					return gsPack;
				}

				if (curOffset > buffLimit)
				{
					RealignPacket();
				}

				gifTag.setTag(&buffer[curOffset], 1);

				state = (GIF_PATH_STATE)(gifTag.tag.FLG + 1);
				GUNIT_WARN("PATH %d New tag State %d FLG %d EOP %d NLOOP %d", gifRegs.stat.APATH, gifRegs.stat.APATH, state, gifTag.tag.FLG, gifTag.tag.EOP, gifTag.tag.NLOOP);
				if (!gifTag.hasAD && curOffset + 16 + gifTag.len > curSize)
				{
					gifTag.isValid = false;
					GUNIT_WARN("PATH %d not enough data, available %d wanted %d", gifRegs.stat.APATH, curSize - curOffset, 16 + gifTag.len);
					return gsPack;
				}

				incTag(curOffset, gsPack.size, 16);
				gsPack.cycles += 2 + gifTag.cycles;
			}

			if (gifTag.hasAD)
			{
				bool dblSIGNAL = false;
				while (gifTag.nLoop && !dblSIGNAL)
				{
					if (curOffset + 16 > curSize)
					{
						GUNIT_WARN("PATH %d not enough data AD, available %d wanted %d", gifRegs.stat.APATH, curSize - curOffset, 16);
						return gsPack;
					}
					if (gifTag.curReg() == GIF_REG_A_D)
					{
						if (!isMTVU())
							dblSIGNAL = Gif_HandlerAD(&buffer[curOffset]);
					}
					incTag(curOffset, gsPack.size, 16);
					gifTag.packedStep();
				}
				if (dblSIGNAL && !(gifTag.tag.EOP && !gifTag.nLoop))
				{
					GUNIT_WARN("PATH %d early exit (double signal)", gifRegs.stat.APATH);
					return gsPack;
				}
			}
			else
				incTag(curOffset, gsPack.size, gifTag.len);

			gifTag.isValid = false;

			if (gifTag.tag.EOP)
			{
				GS_Packet t = gsPack;
				done = true;

				dmaRewind = 0;

				gsPack.Reset();
				gsPack.offset = curOffset;
				GUNIT_WARN("EOP PATH %d", gifRegs.stat.APATH);

				if ((gifRegs.stat.APATH - 1) == GIF_PATH_3)
				{
					state = GIF_PATH_WAIT;

					if (curSize - curOffset > 0 && (gifRegs.stat.M3R || gifRegs.stat.M3P))
					{
						dmaRewind = curSize - curOffset;
						curSize = curOffset;
					}
				}
				else
					state = GIF_PATH_IDLE;

				return t;
			}
		}
	}

	void ExecuteGSPacketMTVU()
	{
		if (curOffset > buffLimit)
		{
			RealignPacket();
		}
		for (;;)
		{
			if (curOffset + 16 > curSize)
				break;
			gifTag.setTag(&buffer[curOffset], 1);

			if (!gifTag.hasAD && curOffset + 16 + gifTag.len > curSize)
				break;
			incTag(curOffset, gsPack.size, 16);

			if (gifTag.hasAD)
			{
				while (gifTag.nLoop)
				{
					if (curOffset + 16 > curSize)
						break;
					if (gifTag.curReg() == GIF_REG_A_D)
					{
						Gif_HandlerAD_MTVU(&buffer[curOffset]);
					}
					incTag(curOffset, gsPack.size, 16);
					gifTag.packedStep();
				}
			}
			else
				incTag(curOffset, gsPack.size, gifTag.len);
			if (curOffset >= curSize)
				break;
			if (gifTag.tag.EOP)
				break;
		}
		pxAssert(curOffset == curSize);
		gifTag.isValid = false;
	}

	void FinishGSPacketMTVU()
	{
		readAmount.fetch_add(gsPack.size + gsPack.readAmount, std::memory_order_acq_rel);
		while (!mtvu.gsPackQueue.push(gsPack))
			;

		gsPack.Reset();
		gsPack.offset = curOffset;
	}

	GS_Packet GetGSPacketMTVU()
	{
		if (!mtvu.gsPackQueue.empty())
		{
			return mtvu.gsPackQueue.front();
		}

		Console.Error("MTVU: Expected gsPackQueue to have elements!");
		pxAssert(0);
		return GS_Packet();
	}

	void PopGSPacketMTVU()
	{
		mtvu.gsPackQueue.pop();
	}

	u32 GetPendingGSPackets()
	{
		return (u32)mtvu.gsPackQueue.size();
	}
};

struct Gif_Unit
{
	Gif_Path gifPath[3];
	GS_SIGNAL gsSIGNAL;
	GS_FINISH gsFINISH;
	tGIF_STAT& stat;
	GIF_TRANSFER_TYPE lastTranType;

	Gif_Unit()
		: gsSIGNAL()
		, gsFINISH()
		, stat(gifRegs.stat)
		, lastTranType(GIF_TRANS_INVALID)
	{
		gifPath[0].Init(GIF_PATH_1, _1mb * 9, _1mb + _1kb);
		gifPath[1].Init(GIF_PATH_2, _1mb * 9, _1mb + _1kb);
		gifPath[2].Init(GIF_PATH_3, _1mb * 9, _1mb + _1kb);
	}

	void Reset(bool softReset = false)
	{
		GUNIT_WARN(Color_Red, "Gif Unit Reset!!! [soft=%d]", softReset);
		ResetRegs();
		gsSIGNAL.Reset();
		gsFINISH.Reset();
		gifPath[0].Reset(softReset);
		gifPath[1].Reset(softReset);
		gifPath[2].Reset(softReset);
		if (!softReset)
		{
			lastTranType = GIF_TRANS_INVALID;
		}
		if (vif1Regs.stat.VGW)
		{
			if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
				CPU_INT(DMAC_VIF1, 1);
		}
	}

	void ResetRegs()
	{
		gifRegs.stat.reset();
		gifRegs.ctrl.reset();
		gifRegs.mode.reset();
		CSRreg.FIFO = CSR_FIFO_EMPTY;
	}

	__fi void AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path)
	{
		if (gsPack.size == ~0u)
			Gif_AddGSPacketMTVU(gsPack, path);
		else
			Gif_AddCompletedGSPacket(gsPack, path);
		if (PRINT_GIF_PACKET)
			Gif_ParsePacket(gsPack, path);
	}

	u32 GetGSPacketSize(GIF_PATH pathIdx, u8* pMem, u32 offset = 0, u32 size = ~0u, bool flush = false)
	{
		u32 memMask = pathIdx ? ~0u : 0x3fffu;
		u32 curSize = 0;
		for (;;)
		{
			Gif_Tag gifTag(&pMem[offset & memMask]);
			incTag(offset, curSize, 16 + gifTag.len);
			if (pathIdx == GIF_PATH_1 && curSize >= 0x4000)
			{
				DevCon.Warning("Gif Unit - GS packet size exceeded VU memory size!");
				return 0;
			}
			if (curSize >= size)
				return size;
			if(((flush && gifTag.tag.EOP) || !flush) && (CHECK_XGKICKHACK || !REC_VU1))
			{
				return curSize | ((u32)gifTag.tag.EOP << 31);
			}
			if (gifTag.tag.EOP )
			{
				return curSize;
			}
		}
	}

	u32 TransferGSPacketData(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size, bool aligned = false)
	{

		if (THREAD_VU1)
		{
			Gif_Path& path1 = gifPath[GIF_PATH_1];
			if (tranType == GIF_TRANS_XGKICK)
			{
				path1.CopyGSPacketData(pMem, size, aligned);
				path1.ExecuteGSPacketMTVU();
				return size;
			}
			if (tranType == GIF_TRANS_MTVU)
			{
				path1.mtvu.fakePackets++;
				if (CanDoGif())
					Execute(false, true);
				return 0;
			}
		}

		GUNIT_LOG("%s - [path=%d][size=%d]", Gif_TransferStr[(tranType >> 8) & 0xf], (tranType & 3) + 1, size);
		if (size == 0)
		{
			GUNIT_WARN("Gif Unit - Size == 0");
			return 0;
		}
		if (!CanDoGif())
		{
			GUNIT_WARN("Gif Unit - Signal or PSE Set or Dir = GS to EE");
		}
		lastTranType = tranType;

		if (tranType == GIF_TRANS_FIFO)
		{
			if (!CanDoPath3())
				DevCon.Warning("Gif Unit - Path 3 FIFO transfer while !CanDoPath3()");
		}
		if (tranType == GIF_TRANS_DMA)
		{
			if (!CanDoPath3())
			{
				if (!Path3Masked())
					stat.P3Q = 1;
				return 0;
			}
		}
		if (tranType == GIF_TRANS_XGKICK)
		{
			if (!CanDoPath1())
			{
				stat.P1Q = 1;
			}
		}
		if (tranType == GIF_TRANS_DIRECT)
		{
			if (!CanDoPath2())
			{
				stat.P2Q = 1;
				return 0;
			}
		}
		if (tranType == GIF_TRANS_DIRECTHL)
		{
			if (!CanDoPath2HL())
			{
				stat.P2Q = 1;
				return 0;
			}
		}

		gifPath[tranType & 3].CopyGSPacketData(pMem, size, aligned);
		size -= Execute(tranType == GIF_TRANS_DMA, false);
		return size;
	}

	__fi int checkPaths(bool p1, bool p2, bool p3, bool checkQ = false)
	{
		int ret = 0;
		ret |= (p1 && !gifPath[GIF_PATH_1].isDone()) << 0;
		ret |= (p2 && !gifPath[GIF_PATH_2].isDone()) << 1;
		ret |= (p3 && !gifPath[GIF_PATH_3].isDone()) << 2;
		return ret | (checkQ ? checkQueued(p1, p2, p3) : 0);
	}

	__fi int checkQueued(bool p1, bool p2, bool p3)
	{
		int ret = 0;
		ret |= (p1 && stat.P1Q) << 0;
		ret |= (p2 && stat.P2Q) << 1;
		ret |= (p3 && stat.P3Q) << 2;
		return ret;
	}

	void FlushToMTGS()
	{
		if (!stat.APATH)
			return;
		Gif_Path& path = gifPath[stat.APATH - 1];
		if (path.gsPack.size && !path.gifTag.isValid)
		{
			AddCompletedGSPacket(path.gsPack, (GIF_PATH)(stat.APATH - 1));
			path.gsPack.offset = path.curOffset;
			path.gsPack.size = 0;
		}
	}

	int Execute(bool isPath3, bool isResume)
	{
		if (!CanDoGif())
		{
			DevCon.Error("Gif Unit - Signal or PSE Set or Dir = GS to EE");
			return 0;
		}
		bool didPath3 = false;
		bool path3Check = isPath3;
		int curPath = stat.APATH > 0 ? stat.APATH - 1 : 0;
		gifPath[2].dmaRewind = 0;
		stat.OPH = 1;

		for (;;)
		{
			if (stat.APATH)
			{
				Gif_Path& path = gifPath[stat.APATH - 1];
				bool done = false;
				GS_Packet gsPack = path.ExecuteGSPacket(done);
				if (!done)
				{
					if (stat.APATH == 3 && CanDoP3Slice() && !gsSIGNAL.queued)
					{
						if (!didPath3 && checkPaths(1, 1, 0))
						{
							didPath3 = true;
							stat.APATH = 0;
							stat.IP3 = 1;
							GUNIT_LOG(Color_Magenta, "Gif Unit - Path 3 slicing arbitration");
							if (gsPack.size > 16)
							{
								u32 subOffset = path.gifTag.isValid ? 16 : 0;
								gsPack.size -= subOffset;
								AddCompletedGSPacket(gsPack, GIF_PATH_3);
								path.gsPack.Reset();
								path.curOffset -= subOffset;
								path.gsPack.offset = path.curOffset;
								path.gifTag.isValid = false;
								pxAssert((s32)path.curOffset >= 0);
								pxAssert(path.state == GIF_PATH_IMAGE);
								GUNIT_LOG(Color_Magenta, "Gif Unit - Sending path 3 sliced gs packet!");
							}
							continue;
						}
					}
					break;
				}
				if (gifPath[curPath].state == GIF_PATH_WAIT || gifPath[curPath].state == GIF_PATH_IDLE)
				{
					AddCompletedGSPacket(gsPack, (GIF_PATH)(stat.APATH - 1));
				}
			}
			if (!gsSIGNAL.queued && !gifPath[0].isDone())
			{
				GUNIT_WARN("Swapping to PATH 1");
				stat.APATH = 1;
				stat.P1Q = 0;
				curPath = 0;
			}
			else if (!gsSIGNAL.queued && !gifPath[1].isDone())
			{
				GUNIT_WARN("Swapping to PATH 2");
				stat.APATH = 2;
				stat.P2Q = 0;
				curPath = 1;
			}
			else if (!gsSIGNAL.queued && !gifPath[2].isDone() && !Path3Masked())
			{
				GUNIT_WARN("Swapping to PATH 3");
				stat.APATH = 3;
				stat.P3Q = 0;
				stat.IP3 = 0;
				curPath = 2;
				path3Check = true;
			}
			else
			{
				GUNIT_WARN("Finished Processing");
				if (stat.APATH == 3 || path3Check)
					gifCheckPathStatus(true);
				else
				{
					if (vif1Regs.stat.VGW)
					{
						if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
							CPU_INT(DMAC_VIF1, 1);
					}

					stat.APATH = 0;
					stat.OPH = 0;
				}

				break;
			}
		}
		if (gifPath[curPath].curOffset == gifPath[curPath].curSize)
		{
			FlushToMTGS();
		}

		if(!checkPaths(stat.APATH != 1, stat.APATH != 2, stat.APATH != 3, true))
			Gif_FinishIRQ();

		if (isPath3)
			return gifPath[2].dmaRewind;
		else
			return 0;
	}

	bool CanDoPath1() const
	{
		return (stat.APATH == 0 || stat.APATH == 1 || (stat.APATH == 3 && CanDoP3Slice())) && CanDoGif();
	}
	bool CanDoPath2() const
	{
		return (stat.APATH == 0 || stat.APATH == 2 || (stat.APATH == 3 && CanDoP3Slice())) && CanDoGif();
	}
	bool CanDoPath2HL() const
	{
		return (stat.APATH == 0 || stat.APATH == 2) && CanDoGif();
	}
	bool CanDoPath3() const
	{
		return ((stat.APATH == 0 && !Path3Masked()) || stat.APATH == 3) && CanDoGif();
	}

	bool CanDoP3Slice() const { return stat.IMT == 1 && gifPath[GIF_PATH_3].state == GIF_PATH_IMAGE; }
	bool CanDoGif() const { return stat.PSE == 0 && stat.DIR == 0 && gsSIGNAL.queued == 0; }
	bool Path3Masked() const { return ((stat.M3R || stat.M3P) && (gifPath[GIF_PATH_3].state == GIF_PATH_IDLE || gifPath[GIF_PATH_3].state == GIF_PATH_WAIT)); }

	void PrintInfo(bool printP1 = 1, bool printP2 = 1, bool printP3 = 1)
	{
		u32 a = checkPaths(1, 1, 1), b = checkQueued(1, 1, 1);
		(void)a;
		(void)b;
		GUNIT_LOG("Gif Unit - LastTransfer = %s, Paths = [%d,%d,%d], Queued = [%d,%d,%d]",
				  Gif_TransferStr[(lastTranType >> 8) & 0xf],
				  !!(a & 1), !!(a & 2), !!(a & 4), !!(b & 1), !!(b & 2), !!(b & 4));
		GUNIT_LOG("Gif Unit - [APATH = %d][Signal = %d][PSE = %d][DIR = %d]",
				  stat.APATH, gsSIGNAL.queued, stat.PSE, stat.DIR);
		GUNIT_LOG("Gif Unit - [CanDoGif = %d][CanDoPath3 = %d][CanDoP3Slice = %d]",
				  CanDoGif(), CanDoPath3(), CanDoP3Slice());
		if (printP1)
			PrintPathInfo(GIF_PATH_1);
		if (printP2)
			PrintPathInfo(GIF_PATH_2);
		if (printP3)
			PrintPathInfo(GIF_PATH_3);
	}

	void PrintPathInfo(GIF_PATH path)
	{
		GUNIT_LOG("Gif Path %d - [hasData = %d][state = %d]", path,
				  gifPath[path].hasDataRemaining(), gifPath[path].state);
	}
};

extern Gif_Unit gifUnit;
