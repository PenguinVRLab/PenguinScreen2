// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

static const int FIFO_SIF_W = 128;

struct sifData
{
	s32 data;
	s32 words;

	tDMA_TAG	tag_lo;
	tDMA_TAG	tag_hi;
};

struct sifFifo
{
	u32 data[FIFO_SIF_W];
	u32 junk[4];
	s32 readPos;
	s32 writePos;
	s32 size;

	s32 sif_free()
	{
		return FIFO_SIF_W - size;
	}

	void write(u32 *from, int words)
	{
		if (words > 0)
		{
			if ((FIFO_SIF_W - size) < words)
				DevCon.Warning("Not enough space in SIF0 FIFO!\n");

			const int wP0 = std::min((FIFO_SIF_W - writePos), words);
			const int wP1 = words - wP0;

			memcpy(&data[writePos], from, wP0 << 2);
			memcpy(&data[0], &from[wP0], wP1 << 2);

			writePos = (writePos + words) & (FIFO_SIF_W - 1);
			size += words;
		}
		SIF_LOG("  SIF + %d = %d (pos=%d)", words, size, writePos);
	}

	void writeJunk(int words)
	{
		if (words > 0)
		{
			const int transferredWords = 4 - words;
			const int prevQWPos = (writePos - (4 + transferredWords)) & (FIFO_SIF_W - 1);

			const int rP0 = std::min((FIFO_SIF_W - prevQWPos), 4);
			const int rP1 = 4 - rP0;
			memcpy(&junk[0], &data[prevQWPos], rP0 << 2);
			memcpy(&junk[rP0], &data[0], rP1 << 2);

			const int wP0 = std::min((FIFO_SIF_W - writePos), words);
			const int wP1 = words - wP0;
			memcpy(&data[writePos], &junk[4- wP0], wP0 << 2);
			memcpy(&data[0], &junk[wP0], wP1 << 2);

			writePos = (writePos + words) & (FIFO_SIF_W - 1);
			size += words;

			SIF_LOG("  SIF + %d = %d Junk (pos=%d)", words, size, writePos);
		}
	}

	void read(u32 *to, int words)
	{
		if (words > 0)
		{
			const int wP0 = std::min((FIFO_SIF_W - readPos), words);
			const int wP1 = words - wP0;

			memcpy(to, &data[readPos], wP0 << 2);
			memcpy(&to[wP0], &data[0], wP1 << 2);

			readPos = (readPos + words) & (FIFO_SIF_W - 1);
			size -= words;
		}
		SIF_LOG("  SIF - %d = %d (pos=%d)", words, size, readPos);
	}
	void clear()
	{
		std::memset(data, 0, sizeof(data));
		readPos = 0;
		writePos = 0;
		size = 0;
	}
};

struct old_sif_structure
{
	sifFifo fifo;
	s32 chain;
	s32 end;
	s32 tagMode;
	s32 counter;
	struct sifData data;
};

struct sif_ee
{
	bool end;
	bool busy;

	s32 cycles;
};

struct sif_iop
{
	bool end;
	bool busy;

	s32 cycles;
	s32 writeJunk;

	s32 counter;
	struct sifData data;
};

struct _sif
{
	sifFifo fifo;
	sif_ee ee;
	sif_iop iop;
};

extern _sif sif0, sif1, sif2;

extern void sifReset();

extern void SIF0Dma();
extern void SIF1Dma();
extern void SIF2Dma();

extern void dmaSIF0();
extern void dmaSIF1();
extern void dmaSIF2();

extern void EEsif0Interrupt();
extern void EEsif1Interrupt();
extern void EEsif2Interrupt();

extern void sif0Interrupt();
extern void sif1Interrupt();
extern void sif2Interrupt();

extern bool ReadFifoSingleWord();
extern bool WriteFifoSingleWord();

#define sif0data sif0.iop.data.data
#define sif1data sif1.iop.data.data
#define sif2data sif2.iop.data.data

#define sif0words sif0.iop.data.words
#define sif1words sif1.iop.data.words
#define sif2words sif2.iop.data.words

#define sif0tag DMA_TAG(sif0data)
#define sif1tag DMA_TAG(sif1data)
#define sif2tag DMA_TAG(sif2data)
