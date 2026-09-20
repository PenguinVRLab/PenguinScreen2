// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "MemoryTypes.h"
#include "R5900.h"

#include "common/StringUtil.h"

enum vif0_stat_flags
{
	VIF0_STAT_VPS_W 	= (1),
	VIF0_STAT_VPS_D 	= (2),
	VIF0_STAT_VPS_T		= (3),
	VIF0_STAT_VPS 		= (3),
	VIF0_STAT_VEW		= (1<<2),
	VIF0_STAT_MRK		= (1<<6),
	VIF0_STAT_DBF		= (1<<7),
	VIF0_STAT_VSS		= (1<<8),
	VIF0_STAT_VFS		= (1<<9),
	VIF0_STAT_VIS		= (1<<10),
	VIF0_STAT_INT		= (1<<11),
	VIF0_STAT_ER0		= (1<<12),
	VIF0_STAT_ER1		= (1<<13),
	VIF0_STAT_FQC		= (15<<24)
};

enum vif1_stat_flags
{
	VIF1_STAT_VPS_W 	= (1),
	VIF1_STAT_VPS_D 	= (2),
	VIF1_STAT_VPS_T		= (3),
	VIF1_STAT_VPS 		= (3),
	VIF1_STAT_VEW		= (1<<2),
	VIF1_STAT_VGW		= (1<<3),
	VIF1_STAT_MRK		= (1<<6),
	VIF1_STAT_DBF		= (1<<7),
	VIF1_STAT_VSS		= (1<<8),
	VIF1_STAT_VFS		= (1<<9),
	VIF1_STAT_VIS		= (1<<10),
	VIF1_STAT_INT		= (1<<11),
	VIF1_STAT_ER0		= (1<<12),
	VIF1_STAT_ER1		= (1<<13),
	VIF1_STAT_FDR 		= (1<<23),
	VIF1_STAT_FQC		= (31<<24)
};

enum vif_stat_flags
{
	VIF_STAT_VPS_W		= (1),
	VIF_STAT_VPS_D		= (2),
	VIF_STAT_VPS_T		= (3),
	VIF_STAT_VPS 		= (3),
	VIF_STAT_VEW		= (1<<2),
	VIF_STAT_MRK		= (1<<6),
	VIF_STAT_DBF		= (1<<7),
	VIF_STAT_VSS		= (1<<8),
	VIF_STAT_VFS		= (1<<9),
	VIF_STAT_VIS		= (1<<10),
	VIF_STAT_INT		= (1<<11),
	VIF_STAT_ER0		= (1<<12),
	VIF_STAT_ER1		= (1<<13)
};

enum vif_status
{
    VPS_IDLE		 = 0,
    VPS_WAITING		 = 1,
    VPS_DECODING	 = 2,
    VPS_TRANSFERRING = 3
};

enum vif_stallreasons
{
    VIF_TIMING_BREAK  = 1,
    VIF_IRQ_STALL	 = 2
};

union tVIF_STAT {
	struct {
		u32 VPS : 2;
		u32 VEW : 1;
		u32 VGW : 1;
		u32 _reserved : 2;
		u32 MRK : 1;
		u32 DBF : 1;
		u32 VSS : 1;
		u32 VFS : 1;
		u32 VIS : 1;
		u32 INT : 1;
		u32 ER0 : 1;
		u32 ER1 : 1;
		u32 _reserved2 : 9;
		u32 FDR : 1;
		u32 FQC : 5;
	};
	u32 _u32;

	tVIF_STAT() = default;
	tVIF_STAT(u32 val)			{ _u32 = val; }
	bool test(u32 flags) const	{ return !!(_u32 & flags); }
	void set_flags	(u32 flags)	{ _u32 |=  flags; }
	void clear_flags(u32 flags) { _u32 &= ~flags; }
	void reset()				{ _u32 = 0; }
	std::string desc() const		{ return StringUtil::StdStringFromFormat("Stat: 0x%x", _u32); }
};

#define VIF_STAT(value) ((tVIF_STAT)(value))

union tVIF_FBRST {
	struct {
		u32 RST : 1;
		u32 FBK : 1;
		u32 STP : 1;
		u32 STC : 1;
		u32 _reserved : 28;
	};
	u32 _u32;

	tVIF_FBRST() = default;
	tVIF_FBRST(u32 val)					{ _u32 = val; }
	bool test		(u32 flags) const	{ return !!(_u32 & flags); }
	void set_flags	(u32 flags)			{ _u32 |=  flags; }
	void clear_flags(u32 flags)			{ _u32 &= ~flags; }
	void reset()						{ _u32 = 0; }
	std::string desc() const				{ return StringUtil::StdStringFromFormat("Fbrst: 0x%x", _u32); }
};

#define FBRST(value) ((tVIF_FBRST)(value))

union tVIF_ERR {
	struct {
		u32 MII : 1;
		u32 ME0 : 1;
		u32 ME1 : 1;
		u32 _reserved : 29;
	};
	u32 _u32;

	tVIF_ERR() = default;
	tVIF_ERR  (u32 val)					{ _u32 = val; }
	void write(u32 val)					{ _u32 = val; }
	bool test		(u32 flags) const	{ return !!(_u32 & flags); }
	void set_flags	(u32 flags)			{ _u32 |=  flags; }
	void clear_flags(u32 flags)			{ _u32 &= ~flags; }
	void reset()						{ _u32 = 0; }
	std::string desc() const				{ return StringUtil::StdStringFromFormat("Err: 0x%x", _u32); }
};

struct vifCycle
{
	u8 cl, wl;
	u8 pad[2];
};

struct VIFregisters {
	tVIF_STAT stat;
	u32 _pad0[3];
	u32 fbrst;
	u32 _pad1[3];
	tVIF_ERR err;
	u32 _pad2[3];
	u32 mark;
	u32 _pad3[3];
	vifCycle cycle;
	u32 _pad4[3];
	u32 mode;
	u32 _pad5[3];
	u32 num;
	u32 _pad6[3];
	u32 mask;
	u32 _pad7[3];
	u32 code;
	u32 _pad8[3];
	u32 itops;
	u32 _pad9[3];
	u32 base;
	u32 _pad10[3];
	u32 ofst;
	u32 _pad11[3];
	u32 tops;
	u32 _pad12[3];
	u32 itop;
	u32 _pad13[3];
	u32 top;
	u32 _pad14[3];
	u32 mskpath3;
	u32 _pad15[3];
	u32 r0;
	u32 _pad16[3];
	u32 r1;
	u32 _pad17[3];
	u32 r2;
	u32 _pad18[3];
	u32 r3;
	u32 _pad19[3];
	u32 c0;
	u32 _pad20[3];
	u32 c1;
	u32 _pad21[3];
	u32 c2;
	u32 _pad22[3];
	u32 c3;
	u32 _pad23[3];
	u32 offset;
	u32 addr;
};

struct VIFregistersMTVU {
	vifCycle cycle;
	u32 mode;
	u32 num;
	u32 mask;
	u32 itop;
	u32 top;
};

static VIFregisters& vif0Regs = (VIFregisters&)eeHw[0x3800];
static VIFregisters& vif1Regs = (VIFregisters&)eeHw[0x3C00];

#define _vifT		template <int idx>
#define  GetVifX	(idx ? (vif1)     : (vif0))
#define  vifXch		(idx ? (vif1ch)   : (vif0ch))
#define  vifXRegs	(idx ? (vif1Regs) : (vif0Regs))

#define  MTVU_VifX     (idx ? ((THREAD_VU1) ? vu1Thread.vif     : vif1)     : (vif0))
#define  MTVU_VifXRegs (idx ? ((THREAD_VU1) ? vu1Thread.vifRegs : vif1Regs) : (vif0Regs))

#define VifStallEnable(vif) (vif.chcr.STR);

extern void dmaVIF0();
extern void dmaVIF1();
extern void mfifoVIF1transfer();
extern bool VIF0transfer(u32 *data, int size, bool TTE=0);
extern bool VIF1transfer(u32 *data, int size, bool TTE=0);
extern void vifMFIFOInterrupt();
