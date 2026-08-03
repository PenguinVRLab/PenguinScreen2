// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

union tPGIF_CTRL
{

	struct pgifCtrl_t
	{
		u32 UNK1					: 2;
		u32 fifo_GP1_ready_for_data : 1;
		u32 fifo_GP0_ready_for_data : 1;
		u32 data_from_gpu_ready		: 1;
		u32 UNK2					: 1;
		u32 UNK3					: 2;
		u32 GP0_fifo_count			: 5;
		u32 UNK4					: 3;
		u32 GP1_fifo_count			: 3;
		u32 UNK5					: 1;
		u32 GP0_fifo_empty			: 1;
		u32 UNK6					: 1;
		u32 UNK7					: 1;
		u32 UNK8					: 8;
		u32 BUSY					: 1;
	}bits;

	u32 _u32;
	tPGIF_CTRL( u32 val ) { _u32 = val; }
	void write(u32 value) { _u32 = value; }
	u32 get() { return _u32; }
};

union tPGIF_IMM
{
	struct imm_t
	{

		u32 e2;
		u32 dummy1[3];
		u32 e3;
		u32 dummy2[3];
		u32 e4;
		u32 dummy3[3];
		u32 e5;
		u32 dummy4[3];

	}reg;
	void reset() { reg.e2 = reg.e3 = reg.e4 = reg.e5 = 0; }
};

struct PGIFregisters
{
	tPGIF_IMM	imm_response;
	u128 		dummy1[2];
	tPGIF_CTRL	ctrl;
};
static PGIFregisters& pgif = (PGIFregisters&)eeHw[0xf310];

union tPGPU_REGS
{
	struct Bits_t
	{
		u32 TPXB	: 4;
		u32 TPYB	: 1;
		u32 ST		: 2;
		u32 TPC		: 2;
		u32 DITH	: 1;
		u32 DRAW	: 1;
		u32 DMSK	: 1;
		u32 DPIX	: 1;
		u32 ILAC	: 1;
		u32 RFLG	: 1;
		u32 TDIS	: 1;
		u32 HR2		: 1;
		u32 HR1		: 2;
		u32 VRES	: 1;
		u32 VMOD	: 1;
		u32 COLD	: 1;
		u32 VILAC	: 1;
		u32 DE		: 1;
		u32 IRQ1	: 1;
		u32 DREQ	: 1;
		u32 RCMD	: 1;
		u32 RSEND	: 1;
		u32 RDMA	: 1;
		u32 DDIR	: 2;
		u32 DEO		: 1;
	}bits;

	u32 _u32;
	tPGPU_REGS( u32 val ) { _u32 = val; }
	void write(u32 value) { _u32 = value; }
	u32 get() { return _u32; }
};

struct PGPUregisters
{
	tPGPU_REGS	stat;
};
static PGPUregisters& pgpu = (PGPUregisters&)eeHw[0xf300];

struct dma_t
{
	struct dmaState_t
	{
		bool ll_active;
		bool to_gpu_active;
		bool to_iop_active;
	} state;

	struct ll_dma_t
	{
		u32 data_read_address;
		u32 total_words;
		u32 current_word;
		u32 next_address;
	} ll_dma;

	struct normalDma_t
	{
		u32 total_words;
		u32 current_word;
		u32 address;
	} normal;
};

union tCHCR_DMA
{
	struct chcrDma_t
	{
		u32 DIR		: 1;
		u32 MAS		: 1;
		u32 resv0	: 6;
		u32 CHE		: 1;
		u32 TSM		: 2;
		u32 resv1	: 5;
		u32 CDWS	: 3;
		u32 resv2	: 1;
		u32 CCWS	: 3;
		u32 resv3	: 1;
		u32 BUSY	: 1;
		u32 resv4	: 3;
		u32 TRIG	: 1;
		u32 UKN1	: 1;
		u32 UNK2	: 1;
		u32 resv5	: 1;
	}bits;
	u32 _u32;
	tCHCR_DMA( u32 val ) { _u32 = val; }
	void write(u32 value) { _u32 = value; }
	u32 get() { return _u32; }
};

union tBCR_DMA
{
	struct bcrDma_t
	{
		u32 block_size : 16;
		u32 block_amount : 16;
	}bit;

	u32 _u32;
	tBCR_DMA( u32 val ) { _u32 = val; }
	u32 get_block_amount()  { return bit.block_amount ? bit.block_amount : 0x10000; }
	u32 get_block_size()  { return bit.block_size; }
	void write(u32 value) { _u32 = value; }
	u32 get() { return _u32; }
};

union tMADR_DMA
{
	u32 address;

	tMADR_DMA( u32 val ) { address = val; }
	void write(u32 value) { address = value; }
	u32 get() { return address; }
};

struct DMAregisters
{
	tMADR_DMA	madr;
	tBCR_DMA	bcr;
	tCHCR_DMA	chcr;
};
static DMAregisters& dmaRegs = (DMAregisters&)iopHw[0x10a0];

struct ringBuf_t
{
	u32* buf;
	int size;
	int count;
	int head;
	int tail;
};

#define PGPU_STAT 0x1000F300

#define IMM_E2 0x1000F310
#define IMM_E3 0x1000F320
#define IMM_E4 0x1000F330
#define IMM_E5 0x1000F340

#define PGIF_CTRL 0x1000F380

#define PGPU_CMD_FIFO 0x1000F3C0
#define PGPU_DAT_FIFO 0x1000F3E0

#define DMA_LL_END_CODE 0x00FFFFFF

#define PGPU_DMA_MADR 0x1F8010A0
#define PGPU_DMA_BCR 0x1F8010A4
#define PGPU_DMA_CHCR 0x1F8010A8
#define PGPU_DMA_TADR 0x1F8010AC

#define pgpuDmaTadr HW_DMA2_TADR

void pgifInit(void);

extern void psxGPUw(int, u32);
extern u32 psxGPUr(int);

extern void PGIFw(int, u32);
extern u32 PGIFr(int);

extern void PGIFwQword(u32 addr, void *);
extern void PGIFrQword(u32 addr, void *);

extern u32 psxDma2GpuR(u32 addr);
extern void psxDma2GpuW(u32 addr, u32 data);

#define PREVENT_IRQ_ON_NORM_DMA_TO_GPU 1

#define PGIF_DAT_RB_LEAVE_FREE 1
