// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ps2/Iop/IopHw_Internal.h"
#include "ps2/HwInternal.h"
#include "ps2/pgif.h"
#include "IopHw.h"
#include "IopDma.h"
#include "Common.h"

#define LOG_REG 0

#if LOG_REG
	#define REG_LOG pgifConLog
#else
	#define REG_LOG(...) do {} while(0)
#endif

#define LOG_PGPU_DMA 1

#if LOG_PGPU_DMA
	#define PGPU_DMA_LOG pgifConLog
#else
	#define PGPU_DMA_LOG(...) do {} while(0)
#endif

u32 old_gp0_value = 0;
void fillFifoOnDrain(void);
void drainPgpuDmaLl(void);
void drainPgpuDmaNrToGpu(void);
void drainPgpuDmaNrToIop(void);

void ringBufPut(struct ringBuf_t* rb, u32* data)
{
	if (rb->count < rb->size)
	{
		*(rb->buf + rb->head) = *data;
		if ((++(rb->head)) >= rb->size)
			rb->head = 0;
		rb->count++;
	}
	else
	{
		Console.Error("PGIF FIFO overflow! sz= %X", rb->size);
	}
}

void ringBufGet(struct ringBuf_t* rb, u32* data)
{
	if (rb->count > 0)
	{
		*data = *(rb->buf + rb->tail);
		if ((++(rb->tail)) >= rb->size)
			rb->tail = 0;
		rb->count--;
	}
	else
	{
		Console.Error("PGIF FIFO underflow! sz= %X", rb->size);
	}
}

void ringBufferClear(struct ringBuf_t* rb)
{
	rb->head = 0;
	rb->tail = 0;
	rb->count = 0;
	return;
}


#define PGIF_CMD_RB_SIZE 0x8
struct ringBuf_t rb_gp1;
u32 pgif_gp1_buffer[PGIF_CMD_RB_SIZE] = {0};

#define PGIF_DAT_RB_SIZE 0x20000
struct ringBuf_t rb_gp0;
u32 pgif_gp0_buffer[PGIF_DAT_RB_SIZE] = {0};
dma_t dma;


void pgifInit()
{
	rb_gp1.buf = pgif_gp1_buffer;
	rb_gp1.size = PGIF_CMD_RB_SIZE;
	ringBufferClear(&rb_gp1);

	rb_gp0.buf = pgif_gp0_buffer;
	rb_gp0.size = PGIF_DAT_RB_SIZE;
	ringBufferClear(&rb_gp0);

	pgpu.stat.write(0);
	pgif.ctrl.write(0);
	old_gp0_value = 0;


	dmaRegs.madr.address = 0;
	dmaRegs.bcr.write(0);
	dmaRegs.chcr.write(0);

	dma.state.ll_active = 0;
	dma.state.to_gpu_active = 0;
	dma.state.to_iop_active = 0;

	dma.ll_dma.data_read_address = 0;
	dma.ll_dma.current_word = 0;
	dma.ll_dma.total_words = 0;
	dma.ll_dma.next_address = 0;

	dma.normal.total_words = 0;
	dma.normal.current_word = 0;
	dma.normal.address = 0;
}

void triggerPgifInt(int subCause)
{
	hwIntcIrq(15);
	cpuSetEvent();
}

void getIrqCmd(u32 data)
{
	{
		if ((data & 0xFF000000) == 0x1F000000)
		{
			pgpu.stat.bits.IRQ1 = 1;
			iopIntcIrq(1);
		}
	}
}

void ackGpuIrq1()
{
	pgpu.stat.bits.IRQ1 = 0;
}

void pgpuDmaIntr(int trigDma)
{

#if PREVENT_IRQ_ON_NORM_DMA_TO_GPU == 1
	if (trigDma != 1)
#endif
		psxDmaInterrupt(2);
}


u32 immRespHndl(u32 cmd, u32 data)
{
	switch ((cmd & 0x7))
	{
		case 0:
		case 1:
		case 6:
		case 7:
			break;
		case 2:
			data = pgif.imm_response.reg.e2 & 0x000FFFFF;
			break;
		case 3:
			data = pgif.imm_response.reg.e3 & 0x0007FFFF;
			break;
		case 4:
			data = pgif.imm_response.reg.e4 & 0x0007FFFF;
			break;
		case 5:
			data = pgif.imm_response.reg.e5 & 0x003FFFFF;
			break;
	}
	return data;
}

void handleGp1Command(u32 cmd)
{
	const u32 cmdNr = ((cmd >> 24) & 0xFF) & 0x3F;
	switch (cmdNr)
	{
		case 2:
			ackGpuIrq1();
			break;
		case 4:
			pgpu.stat.bits.DDIR = cmd & 0x3;
			switch (pgpu.stat.bits.DDIR)
				{
					case 0x00:
						pgpu.stat.bits.DREQ = 0;
						break;
					case 0x01:
						if (rb_gp0.count < (rb_gp0.size - PGIF_DAT_RB_LEAVE_FREE))
						{
							pgpu.stat.bits.DREQ = 1;
						}
						else
						{
							pgpu.stat.bits.DREQ = 0;
						}
						break;
					case 0x02:
						pgpu.stat.bits.DREQ = pgpu.stat.bits.RDMA;
						drainPgpuDmaLl();
						break;
					case 0x03:
						pgpu.stat.bits.DREQ = pgpu.stat.bits.RSEND;
						break;
				}
			break;
		default:
			break;
	}
}

u32 getUpdPgpuStatReg()
{

	pgpu.stat.bits.RSEND = pgif.ctrl.bits.data_from_gpu_ready;
	return pgpu.stat.get();
}

u8 getGP0RbC_Count()
{
	return std::min(rb_gp0.count, 0x1F);
}

u32 getUpdPgifCtrlReg()
{
	pgif.ctrl.bits.GP0_fifo_count = getGP0RbC_Count();
	pgif.ctrl.bits.GP1_fifo_count = rb_gp1.count;
	return pgif.ctrl.get();
}


void rb_gp1_Get(u32* data)
{
	ringBufGet(&rb_gp1, data);
	handleGp1Command(*data);
}

void rb_gp0_Get(u32* data)
{
	if (rb_gp0.count > 0)
	{
		ringBufGet(&rb_gp0, data);
		getIrqCmd(*data);
	}
	else
	{
		*data = old_gp0_value;
	}
}

void psxGPUw(int addr, u32 data)
{
	REG_LOG("PGPU write 0x%08X = 0x%08X", addr, data);
	if (addr == HW_PS1_GPU_DATA)
	{
		ringBufPut(&rb_gp0, &data);
	}
	else if (addr == HW_PS1_GPU_STATUS)
	{
		u8 imm_check = (data >> 28);
		imm_check &= 0x3;
		if (imm_check == 1)
		{
			old_gp0_value = immRespHndl(data, old_gp0_value);
		}
		else
		{
			triggerPgifInt(0);
			ringBufPut(&rb_gp1, &data);
		}
	}
}

u32 psxGPUr(int addr)
{
	u32 data = 0;
	if (addr == HW_PS1_GPU_DATA)
	{
		rb_gp0_Get(&data);
	}
	else if (addr == HW_PS1_GPU_STATUS)
	{
		data = getUpdPgpuStatReg();
	}
	if (addr != HW_PS1_GPU_STATUS)
		REG_LOG("PGPU read  0x%08X = 0x%08X", addr, data);

	return data;
}

void PGIFw(int addr, u32 data)
{
		REG_LOG("PGIF write 0x%08X = 0x%08X  0x%08X  EEpc= %08X  IOPpc= %08X ", addr, data, getUpdPgifCtrlReg(), cpuRegs.pc, psxRegs.pc);

	switch (addr)
	{
		case PGPU_STAT:
			pgpu.stat.write(data);
			break;
		case PGIF_CTRL:
			pgif.ctrl.write(data);
			fillFifoOnDrain();
			break;
		case IMM_E2:
			pgif.imm_response.reg.e2 = data;
			break;
		case IMM_E3:
			pgif.imm_response.reg.e3 = data;
			break;
		case IMM_E4:
			pgif.imm_response.reg.e4 = data;
			break;
		case IMM_E5:
			pgif.imm_response.reg.e5 = data;
			break;
		case PGPU_CMD_FIFO:
			Console.Error("PGIF CMD FIFO write by EE (SHOULDN'T HAPPEN) 0x%08X = 0x%08X", addr, data);
			break;
		case PGPU_DAT_FIFO:
			ringBufPut(&rb_gp0, &data);
			drainPgpuDmaNrToIop();
			break;
		default:
			DevCon.Error("PGIF write to unknown location 0xx% , data: %x", addr, data);
			break;
	}
}

u32 PGIFr(int addr)
{
	u32 data = 0;
	switch (addr)
	{
		case PGPU_STAT:
			data = pgpu.stat.get();
			break;
		case PGIF_CTRL:
			data = getUpdPgifCtrlReg();
			break;
		case IMM_E2:
			data = pgif.imm_response.reg.e2;
			break;
		case IMM_E3:
			data = pgif.imm_response.reg.e3;
			break;
		case IMM_E4:
			data = pgif.imm_response.reg.e4;
			break;
		case IMM_E5:
			data = pgif.imm_response.reg.e5;
			break;
		case PGPU_CMD_FIFO:
			rb_gp1_Get(&data);
			break;
		case PGPU_DAT_FIFO:
			fillFifoOnDrain();
			rb_gp0_Get(&data);
			break;
		default:
			DevCon.Error("PGIF read from unknown location 0xx%", addr);
			break;
	}

	return data;
}

void PGIFrQword(u32 addr, void* dat)
{
	u32* data = (u32*)dat;

	if (addr == PGPU_CMD_FIFO)
	{
		Console.Error("PGIF QW CMD read =ERR!");
	}
	else if (addr == PGPU_DAT_FIFO)
	{
		fillFifoOnDrain();
		rb_gp0_Get(data + 0);
		rb_gp0_Get(data + 1);
		rb_gp0_Get(data + 2);
		rb_gp0_Get(data + 3);

		fillFifoOnDrain();
	}
	else
	{
		Console.WriteLn("PGIF QWord Read from address %08X  ERR - shouldnt happen!", addr);
		Console.WriteLn("Data = %08X %08X %08X %08X ", *(u32*)(data + 0), *(u32*)(data + 1), *(u32*)(data + 2), *(u32*)(data + 3));
	}
}

void PGIFwQword(u32 addr, void* dat)
{
	u32* data = (u32*)dat;
	DevCon.Warning("WARNING PGIF WRITE BY PS1DRV ! - NOT KNOWN TO EVER BE DONE!");
	Console.WriteLn("PGIF QW write  0x%08X = 0x%08X %08X %08X %08X ", addr, *(u32*)(data + 0), *(u32*)(data + 1), *(u32*)(data + 2), *(u32*)(data + 3));

	if (addr == PGPU_CMD_FIFO)
	{
		Console.Error("PGIF QW CMD write!");
	}
	else if (addr == PGPU_DAT_FIFO)
	{
		ringBufPut(&rb_gp0, (u32*)(data + 0));
		ringBufPut(&rb_gp0, (u32*)(data + 1));
		ringBufPut(&rb_gp0, (u32*)(data + 2));
		ringBufPut(&rb_gp0, (u32*)(data + 3));
		drainPgpuDmaNrToIop();
	}
}

void fillFifoOnDrain()
{
	if (!pgif.ctrl.bits.fifo_GP0_ready_for_data)
		return;


	while ((rb_gp0.count < ((rb_gp0.size) - PGIF_DAT_RB_LEAVE_FREE)) && ((dma.state.to_gpu_active) || (dma.state.ll_active)))
	{
		drainPgpuDmaLl();
		drainPgpuDmaNrToGpu();
	}
	if (((dma.state.ll_active) || (dma.state.to_gpu_active)) && (!dma.state.to_iop_active))
	{
		pgif.ctrl.bits.fifo_GP0_ready_for_data = 0;
	}
}

void drainPgpuDmaLl()
{
	if (!dma.state.ll_active)
		return;

	if (rb_gp0.count >= ((rb_gp0.size) - PGIF_DAT_RB_LEAVE_FREE))
		return;

	if (dmaRegs.chcr.bits.MAS)
		DevCon.Error("Unimplemented backward memory step on PGPU DMA Linked List");

	if (dma.ll_dma.current_word >= dma.ll_dma.total_words)
	{
		if (dma.ll_dma.next_address == DMA_LL_END_CODE)
		{
			dma.state.ll_active = 0;
			dmaRegs.madr.address = 0x00FFFFFF;
			dmaRegs.chcr.bits.BUSY = 0;
			pgpuDmaIntr(3);
			PGPU_DMA_LOG("PGPU DMA Linked List Finished");
		}
		else
		{
			u32 data = iopMemRead32(dma.ll_dma.next_address);
			PGPU_DMA_LOG( "Next PGPU LL DMA header= %08X  ", data);
			dmaRegs.madr.address = data & 0x00FFFFFF;
			dma.ll_dma.data_read_address = dma.ll_dma.next_address + 4;
			dma.ll_dma.current_word = 0;
			dma.ll_dma.total_words = (data >> 24) & 0xFF;
			dma.ll_dma.next_address = dmaRegs.madr.address;
		}
	}
	else
	{
		u32 data = iopMemRead32(dma.ll_dma.data_read_address);
		PGPU_DMA_LOG( "PGPU LL DMA data= %08X  addr %08X ", data, dma.ll_dma.data_read_address);
		ringBufPut(&rb_gp0, &data);
		dma.ll_dma.data_read_address += 4;
		dma.ll_dma.current_word++;
	}
}

void drainPgpuDmaNrToGpu()
{
	if (!dma.state.to_gpu_active)
		return;

	if (rb_gp0.count >= ((rb_gp0.size) - PGIF_DAT_RB_LEAVE_FREE))
		return;

	if (dma.normal.current_word < dma.normal.total_words)
	{
		u32 data = iopMemRead32(dma.normal.address);
		PGPU_DMA_LOG( "To GPU Normal DMA data= %08X  addr %08X ", data, dma.ll_dma.data_read_address);

		ringBufPut(&rb_gp0, &data);
		if (dmaRegs.chcr.bits.MAS)
		{
			DevCon.Error("Unimplemented backward memory step on TO GPU DMA");
		}
		dmaRegs.madr.address += 4;
		dma.normal.address += 4;
		dma.normal.current_word++;

		if ((dma.normal.current_word % dmaRegs.bcr.bit.block_size) == 0)
			dmaRegs.bcr.bit.block_amount -= 1;
	}
	if (dma.normal.current_word >= dma.normal.total_words)
	{
		dma.state.to_gpu_active = 0;
		dmaRegs.chcr.bits.BUSY = 0;
		pgpuDmaIntr(1);
		PGPU_DMA_LOG("To GPU DMA Normal FINISHED");
	}
}

void drainPgpuDmaNrToIop()
{
	if (!dma.state.to_iop_active || rb_gp0.count <= 0)
		return;

	if (dma.normal.current_word < dma.normal.total_words)
	{
		u32 data = 0;
		ringBufGet(&rb_gp0, &data);
		iopMemWrite32(dma.normal.address, data);
		if (dmaRegs.chcr.bits.MAS)
		{
			DevCon.Error("Unimplemented backward memory step on FROM GPU DMA");
		}
		dmaRegs.madr.address += 4;
		dma.normal.address += 4;
		dma.normal.current_word++;
		if ((dma.normal.current_word % dmaRegs.bcr.bit.block_size) == 0)
		{
			dmaRegs.bcr.bit.block_amount -= 1;
		}
		PGPU_DMA_LOG("GPU->IOP ba: %x , cw: %x , tw: %x" ,dmaRegs.bcr.bit.block_amount, dma.normal.current_word, dma.normal.total_words);
	}
	if (dma.normal.current_word >= dma.normal.total_words)
	{
		dma.state.to_iop_active = 0;
		dmaRegs.chcr.bits.BUSY = 0;
		pgpuDmaIntr(2);
	}

	if (rb_gp0.count > 0)
		drainPgpuDmaNrToIop();
}


void processPgpuDma()
{
	if (!dmaRegs.chcr.bits.TSM)
	{
		Console.Error("SyncMode 0 on GPU DMA!");
	}
	if (dmaRegs.chcr.bits.TSM == 3)
	{
		Console.Warning("SyncMode 3! Assuming SyncMode 1");
		dmaRegs.chcr.bits.TSM = 1;
	}
	PGPU_DMA_LOG("Starting GPU DMA! CHCR %08X  BCR %08X  MADR %08X ", dmaRegs.chcr.get(), dmaRegs.bcr.get(), dmaRegs.madr.address);

	if (dmaRegs.chcr.bits.TSM == 2)
	{
		if (dmaRegs.chcr.bits.DIR)
		{
			dma.state.ll_active = 1;
			dma.ll_dma.next_address = (dmaRegs.madr.address & 0x00FFFFFF);
			dma.ll_dma.current_word = 0;
			dma.ll_dma.total_words = 0;
			PGPU_DMA_LOG("LL DMA FILL");

			fillFifoOnDrain();
			return;
		}
		else
		{
			Console.Error("Error: Linked list from GPU DMA!");
			return;
		}
	}
	dma.normal.current_word = 0;
	dma.normal.address = dmaRegs.madr.address & 0x1FFFFFFF;
	dma.normal.total_words = (dmaRegs.bcr.bit.block_size * dmaRegs.bcr.get_block_amount());

	if (dmaRegs.chcr.bits.DIR)
	{
		PGPU_DMA_LOG("NORMAL DMA TO GPU");
		dma.state.to_gpu_active = 1;
		fillFifoOnDrain();
	}
	else
	{
		PGPU_DMA_LOG("NORMAL DMA FROM GPU");
		dma.state.to_iop_active = 1;
		drainPgpuDmaNrToIop();
	}
}

u32 psxDma2GpuR(u32 addr)
{
	u32 data = 0;
	addr &= 0x1FFFFFFF;
	switch (addr)
	{
		case PGPU_DMA_MADR:
			data = dmaRegs.madr.address;
			break;
		case PGPU_DMA_BCR:
			data = dmaRegs.bcr.get();
			break;
		case PGPU_DMA_CHCR:
			data = dmaRegs.chcr.get();
			break;
		case PGPU_DMA_TADR:
			data = pgpuDmaTadr;
			Console.Error("PGPU DMA read TADR!");
			break;
		default:
			Console.Error("Unknown PGPU DMA read 0x%08X", addr);
			break;
	}
	if (addr != PGPU_DMA_CHCR)
		PGPU_DMA_LOG("PGPU DMA read  0x%08X = 0x%08X", addr, data);
	return data;
}

void psxDma2GpuW(u32 addr, u32 data)
{
	PGPU_DMA_LOG("PGPU DMA write 0x%08X = 0x%08X", addr, data);
	addr &= 0x1FFFFFFF;
	switch (addr)
	{
		case PGPU_DMA_MADR:
			dmaRegs.madr.address = (data & 0x00FFFFFF);
			break;
		case PGPU_DMA_BCR:
			dmaRegs.bcr.write(data);
			break;
		case PGPU_DMA_CHCR:
			dmaRegs.chcr.write(data);
			if (dmaRegs.chcr.bits.BUSY)
			{
				processPgpuDma();
			}
			break;
		case PGPU_DMA_TADR:
			pgpuDmaTadr = data;
			Console.Error("PGPU DMA write TADR! ");
			break;
		default:
			Console.Error("Unknown PGPU DMA write 0x%08X = 0x%08X", addr, data);
			break;
	}
}