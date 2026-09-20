// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/emitter/x86emitter.h"
#include "VUmicro.h"

#define RALOG(...)

#define MODE_READ        1
#define MODE_WRITE       2
#define MODE_CALLEESAVED  0x20
#define MODE_COP2 0x40

#define PROCESS_EE_XMM 0x02

#define PROCESS_EE_S 0x04
#define PROCESS_EE_T 0x08
#define PROCESS_EE_D 0x10

#define PROCESS_EE_LO         0x40
#define PROCESS_EE_HI         0x80
#define PROCESS_EE_ACC        0x40

#define EEREC_S    (((info) >>  8) & 0xf)
#define EEREC_T    (((info) >> 12) & 0xf)
#define EEREC_D    (((info) >> 16) & 0xf)
#define EEREC_LO   (((info) >> 20) & 0xf)
#define EEREC_HI   (((info) >> 24) & 0xf)
#define EEREC_ACC  (((info) >> 20) & 0xf)

#define PROCESS_EE_SET_S(reg)   (((reg) <<  8) | PROCESS_EE_S)
#define PROCESS_EE_SET_T(reg)   (((reg) << 12) | PROCESS_EE_T)
#define PROCESS_EE_SET_D(reg)   (((reg) << 16) | PROCESS_EE_D)
#define PROCESS_EE_SET_LO(reg)  (((reg) << 20) | PROCESS_EE_LO)
#define PROCESS_EE_SET_HI(reg)  (((reg) << 24) | PROCESS_EE_HI)
#define PROCESS_EE_SET_ACC(reg) (((reg) << 20) | PROCESS_EE_ACC)

#define PROCESS_CONSTS 1
#define PROCESS_CONSTT 2

enum xmminfo : u16 
{
	XMMINFO_READLO = 0x001,
	XMMINFO_READHI = 0x002,
	XMMINFO_WRITELO = 0x004,
	XMMINFO_WRITEHI = 0x008,
	XMMINFO_WRITED = 0x010,
	XMMINFO_READD = 0x020,
	XMMINFO_READS = 0x040,
	XMMINFO_READT = 0x080,
	XMMINFO_READACC = 0x200,
	XMMINFO_WRITEACC = 0x400,
	XMMINFO_WRITET = 0x800,

	XMMINFO_64BITOP = 0x1000,
	XMMINFO_FORCEREGS = 0x2000,
	XMMINFO_FORCEREGT = 0x4000,
	XMMINFO_NORENAME = 0x8000
};

enum x86type : u8 
{
	X86TYPE_TEMP = 0,
	X86TYPE_GPR = 1,
	X86TYPE_FPRC = 2,
	X86TYPE_VIREG = 3,
	X86TYPE_PCWRITEBACK = 4,
	X86TYPE_PSX = 5,
	X86TYPE_PSX_PCWRITEBACK = 6
};

struct _x86regs
{
	u8 inuse;
	s8 reg;
	u8 mode;
	u8 needed;
	u8 type;
	u16 counter;
	u32 extra;
};

extern _x86regs x86regs[iREGCNT_GPR], s_saveX86regs[iREGCNT_GPR];

bool _isAllocatableX86reg(int x86reg);
void _initX86regs();
int _getFreeX86reg(int mode);
int _allocX86reg(int type, int reg, int mode);
int _checkX86reg(int type, int reg, int mode);
bool _hasX86reg(int type, int reg, int required_mode = 0);
void _addNeededX86reg(int type, int reg);
void _clearNeededX86regs();
void _freeX86reg(const x86Emitter::xRegister32& x86reg);
void _freeX86reg(int x86reg);
void _freeX86regWithoutWriteback(int x86reg);
void _freeX86regs();
void _flushX86regs();
void _flushConstRegs(bool delete_const);
void _flushConstReg(int reg);
void _validateRegs();
void _writebackX86Reg(int x86reg);

void mVUFreeCOP2GPR(int hostreg);
bool mVUIsReservedCOP2(int hostreg);

#define XMMTYPE_TEMP   0
#define XMMTYPE_GPRREG X86TYPE_GPR
#define XMMTYPE_FPREG  6
#define XMMTYPE_FPACC  7
#define XMMTYPE_VFREG  8

#define XMMGPR_LO  33
#define XMMGPR_HI  32
#define XMMFPU_ACC 32

enum : int
{
	DELETE_REG_FREE = 0,
	DELETE_REG_FLUSH = 1,
	DELETE_REG_FLUSH_AND_FREE = 2,
	DELETE_REG_FREE_NO_WRITEBACK = 3
};

struct _xmmregs
{
	u8 inuse;
	s8 reg;
	u8 type;
	u8 mode;
	u8 needed;
	u16 counter;
};

void _initXMMregs();
int _getFreeXMMreg(u32 maxreg = iREGCNT_XMM);
int _allocTempXMMreg(XMMSSEType type);
int _allocFPtoXMMreg(int fpreg, int mode);
int _allocGPRtoXMMreg(int gprreg, int mode);
int _allocFPACCtoXMMreg(int mode);
void _reallocateXMMreg(int xmmreg, int newtype, int newreg, int newmode, bool writeback = true);
int _checkXMMreg(int type, int reg, int mode);
bool _hasXMMreg(int type, int reg, int required_mode = 0);
void _addNeededFPtoXMMreg(int fpreg);
void _addNeededFPACCtoXMMreg();
void _addNeededGPRtoX86reg(int gprreg);
void _addNeededPSXtoX86reg(int gprreg);
void _addNeededGPRtoXMMreg(int gprreg);
void _clearNeededXMMregs();
void _deleteGPRtoX86reg(int reg, int flush);
void _deletePSXtoX86reg(int reg, int flush);
void _deleteGPRtoXMMreg(int reg, int flush);
void _deleteFPtoXMMreg(int reg, int flush);
void _freeXMMreg(int xmmreg);
void _freeXMMregWithoutWriteback(int xmmreg);
void _writebackXMMreg(int xmmreg);
int _allocVFtoXMMreg(int vfreg, int mode);
void mVUFreeCOP2XMMreg(int hostreg);
void _flushCOP2regs();
void _flushXMMreg(int xmmreg);
void _flushXMMregs();

#define EEINST_LIVE     1
#define EEINST_LASTUSE   8
#define EEINST_XMM    0x20
#define EEINST_USED   0x40

#define EEINST_COP2_DENORMALIZE_STATUS_FLAG 0x100
#define EEINST_COP2_NORMALIZE_STATUS_FLAG 0x200
#define EEINST_COP2_STATUS_FLAG 0x400
#define EEINST_COP2_MAC_FLAG 0x800
#define EEINST_COP2_CLIP_FLAG 0x1000
#define EEINST_COP2_SYNC_VU0 0x2000
#define EEINST_COP2_FINISH_VU0 0x4000
#define EEINST_COP2_FLUSH_VU0_REGISTERS 0x8000

struct EEINST
{
	u16 info;
	u8 regs[34];
	u8 fpuregs[33];
	u8 vfregs[34];
	u8 viregs[16];

	u8 writeType[3], writeReg[3];
	u8 readType[4], readReg[4];
};

extern EEINST* g_pCurInstInfo;
extern void _recClearInst(EEINST* pinst);

extern u32 _recIsRegReadOrWritten(EEINST* pinst, int size, u8 xmmtype, u8 reg);

extern void _recFillRegister(EEINST& pinst, int type, int reg, int write);

#define EE_WRITE_DEAD_VALUES 1

static __fi bool EEINST_USEDTEST(u32 reg)
{
	return (g_pCurInstInfo->regs[reg] & (EEINST_USED | EEINST_LASTUSE)) == EEINST_USED;
}

static __fi bool EEINST_XMMUSEDTEST(u32 reg)
{
	return (g_pCurInstInfo->regs[reg] & (EEINST_USED | EEINST_XMM | EEINST_LASTUSE)) == (EEINST_USED | EEINST_XMM);
}

static __fi bool EEINST_VFUSEDTEST(u32 reg)
{
	return (g_pCurInstInfo->vfregs[reg] & (EEINST_USED | EEINST_LASTUSE)) == EEINST_USED;
}

static __fi bool EEINST_VIUSEDTEST(u32 reg)
{
	return (g_pCurInstInfo->viregs[reg] & (EEINST_USED | EEINST_LASTUSE)) == EEINST_USED;
}

static __fi bool EEINST_LIVETEST(u32 reg)
{
	return EE_WRITE_DEAD_VALUES || ((g_pCurInstInfo->regs[reg] & EEINST_LIVE) != 0);
}

static __fi bool EEINST_RENAMETEST(u32 reg)
{
	return (reg == 0 || !EEINST_USEDTEST(reg) || !EEINST_LIVETEST(reg));
}

static __fi bool FPUINST_ISLIVE(u32 reg)   { return !!(g_pCurInstInfo->fpuregs[reg] & EEINST_LIVE); }
static __fi bool FPUINST_LASTUSE(u32 reg)  { return !!(g_pCurInstInfo->fpuregs[reg] & EEINST_LASTUSE); }

static __fi bool FPUINST_USEDTEST(u32 reg)
{
	return (g_pCurInstInfo->fpuregs[reg] & (EEINST_USED | EEINST_LASTUSE)) == EEINST_USED;
}

static __fi bool FPUINST_LIVETEST(u32 reg)
{
	return EE_WRITE_DEAD_VALUES || FPUINST_ISLIVE(reg);
}

static __fi bool FPUINST_RENAMETEST(u32 reg)
{
	return (!EEINST_USEDTEST(reg) || !EEINST_LIVETEST(reg));
}

extern _xmmregs xmmregs[iREGCNT_XMM], s_saveXMMregs[iREGCNT_XMM];

extern thread_local u8* j8Ptr[32];
extern thread_local u32* j32Ptr[32];

extern u16 g_x86AllocCounter;
extern u16 g_xmmAllocCounter;

int _allocIfUsedGPRtoX86(int gprreg, int mode);
int _allocIfUsedVItoX86(int vireg, int mode);
int _allocIfUsedGPRtoXMM(int gprreg, int mode);
int _allocIfUsedFPUtoXMM(int fpureg, int mode);

#define FLUSH_NONE             0x000
#define FLUSH_CONSTANT_REGS    0x001
#define FLUSH_FLUSH_XMM        0x002
#define FLUSH_FREE_XMM         0x004
#define FLUSH_ALL_X86          0x020
#define FLUSH_FREE_TEMP_X86    0x040
#define FLUSH_FREE_NONTEMP_X86 0x080
#define FLUSH_FREE_VU0         0x100
#define FLUSH_PC               0x200
#define FLUSH_CODE             0x800

#define FLUSH_EVERYTHING   0x1ff
#define FLUSH_INTERPRETER  0xfff
#define FLUSH_FULLVTLB 0x000

#define FLUSH_NODESTROY (FLUSH_CONSTANT_REGS | FLUSH_FLUSH_XMM | FLUSH_ALL_X86)

