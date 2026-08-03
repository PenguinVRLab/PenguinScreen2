// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include <float.h>

#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "GS.h"
#include "ps2/BiosTools.h"
#include "DebugTools/DebugInterface.h"
#include "DebugTools/Breakpoints.h"
#include "Host.h"
#include "VMManager.h"

#include "fmt/format.h"

GS_VideoMode gsVideoMode = GS_VideoMode::Uninitialized;
bool gsIsInterlaced = false;

static __fi bool _add64_Overflow( s64 x, s64 y, s64 &ret )
{
	const s64 result = x + y;

	if( ((~(x^y))&(x^result)) < 0 ) {
		cpuException(0x30, cpuRegs.branch);
		return true;
	}

	ret = result;
	return false;
}

static __fi bool _add32_Overflow( s32 x, s32 y, s64 &ret )
{
	GPR_reg64 result;  result.SD[0] = (s64)x + y;

	if( (result.UL[0]>>31) != (result.UL[1] & 1) ) {
		cpuException(0x30, cpuRegs.branch);
		return true;
	}

	ret = result.SD[0];

	return false;
}


const R5900::OPCODE& R5900::GetCurrentInstruction()
{
	const OPCODE* opcode = &R5900::OpcodeTables::tbl_Standard[_Opcode_];

	while( opcode->getsubclass != NULL )
		opcode = &opcode->getsubclass(cpuRegs.code);

	return *opcode;
}

const R5900::OPCODE& R5900::GetInstruction(u32 op)
{
	const OPCODE* opcode = &R5900::OpcodeTables::tbl_Standard[op >> 26];

	while( opcode->getsubclass != NULL )
		opcode = &opcode->getsubclass(op);

	return *opcode;
}

const char * const R5900::bios[256]=
{
	"RFU000_FullReset", "ResetEE",				"SetGsCrt",				"RFU003",
	"Exit",				"RFU005",				"LoadExecPS2",			"ExecPS2",
	"RFU008",			"RFU009",				"AddSbusIntcHandler",	"RemoveSbusIntcHandler",
	"Interrupt2Iop",	"SetVTLBRefillHandler", "SetVCommonHandler",	"SetVInterruptHandler",
	"AddIntcHandler",	"RemoveIntcHandler",	"AddDmacHandler",		"RemoveDmacHandler",
	"_EnableIntc",		"_DisableIntc",			"_EnableDmac",			"_DisableDmac",
	"_SetAlarm",		"_ReleaseAlarm",		"_iEnableIntc",			"_iDisableIntc",
	"_iEnableDmac",		"_iDisableDmac",		"_iSetAlarm",			"_iReleaseAlarm",
	"CreateThread",			"DeleteThread",		"StartThread",			"ExitThread",
	"ExitDeleteThread",		"TerminateThread",	"iTerminateThread",		"DisableDispatchThread",
	"EnableDispatchThread",		"ChangeThreadPriority", "iChangeThreadPriority",	"RotateThreadReadyQueue",
	"iRotateThreadReadyQueue",	"ReleaseWaitThread",	"iReleaseWaitThread",		"GetThreadId",
	"ReferThreadStatus","iReferThreadStatus",	"SleepThread",		"WakeupThread",
	"_iWakeupThread",   "CancelWakeupThread",	"iCancelWakeupThread",	"SuspendThread",
	"iSuspendThread",   "ResumeThread",		"iResumeThread",	"JoinThread",
	"RFU060",	    "RFU061",			"EndOfHeap",		 "RFU063",
	"CreateSema",	    "DeleteSema",	"SignalSema",		"iSignalSema",
	"WaitSema",	    "PollSema",		"iPollSema",		"ReferSemaStatus",
	"iReferSemaStatus", "RFU073",		"SetOsdConfigParam", 	"GetOsdConfigParam",
	"GetGsHParam",	    "GetGsVParam",	"SetGsHParam",		"SetGsVParam",
	"RFU080_CreateEventFlag",	"RFU081_DeleteEventFlag",
	"RFU082_SetEventFlag",		"RFU083_iSetEventFlag",
	"RFU084_ClearEventFlag",	"RFU085_iClearEventFlag",
	"RFU086_WaitEventFlag",		"RFU087_PollEventFlag",
	"RFU088_iPollEventFlag",	"RFU089_ReferEventFlagStatus",
	"RFU090_iReferEventFlagStatus", "RFU091_GetEntryAddress",
	"EnableIntcHandler_iEnableIntcHandler",
	"DisableIntcHandler_iDisableIntcHandler",
	"EnableDmacHandler_iEnableDmacHandler",
	"DisableDmacHandler_iDisableDmacHandler",
	"KSeg0",				"EnableCache",	"DisableCache",			"GetCop0",
	"FlushCache",			"RFU101",		"CpuConfig",			"iGetCop0",
	"iFlushCache",			"RFU105",		"iCpuConfig", 			"sceSifStopDma",
	"SetCPUTimerHandler",	"SetCPUTimer",	"SetOsdConfigParam2",	"GetOsdConfigParam2",
	"GsGetIMR_iGsGetIMR",				"GsGetIMR_iGsPutIMR",	"SetPgifHandler", 				"SetVSyncFlag",
	"RFU116",							"print", 				"sceSifDmaStat_isceSifDmaStat", "sceSifSetDma_isceSifSetDma",
	"sceSifSetDChain_isceSifSetDChain", "sceSifSetReg",			"sceSifGetReg",					"ExecOSD",
	"Deci2Call",						"PSMode",				"MachineType",					"GetMemorySize",
};

static u32 deci2addr = 0;
static u32 deci2handler = 0;
static char deci2buffer[256];

void Deci2Reset()
{
	deci2handler	= 0;
	deci2addr		= 0;
	std::memset(deci2buffer, 0, sizeof(deci2buffer));
}

bool SaveStateBase::deci2Freeze()
{
	if (!FreezeTag("deci2"))
		return false;

	Freeze( deci2addr );
	Freeze( deci2handler );
	Freeze( deci2buffer );

	return IsOkay();
}

static int __Deci2Call(int call, u32 *addr)
{
	if (call > 0x10)
		return -1;

	switch (call)
	{
		case 1:
			if( addr != NULL )
			{
				deci2addr = addr[1];
				BIOS_LOG("deci2open: %x,%x,%x,%x",
						 addr[3], addr[2], addr[1], addr[0]);
				deci2handler = addr[2];
			}
			else
			{
				deci2handler = 0;
				DevCon.Warning( "Deci2Call.Open > NULL address ignored." );
			}
			return 1;

		case 2:
			deci2addr = 0;
			deci2handler = 0;
			return 1;

		case 3:
		{
			char reqaddr[128];
			if( addr != NULL )
				std::snprintf(reqaddr, std::size(reqaddr), "%x %x %x %x", addr[3], addr[2], addr[1], addr[0]);

			if (!deci2addr) return 1;

			const u32* d2ptr = (u32*)PSM(deci2addr);

			BIOS_LOG("deci2reqsend: %s: deci2addr: %x,%x,%x,buf=%x %x,%x,len=%x,%x",
				(( addr == NULL ) ? "NULL" : reqaddr),
				d2ptr[7], d2ptr[6], d2ptr[5], d2ptr[4],
				d2ptr[3], d2ptr[2], d2ptr[1], d2ptr[0]);

			if (d2ptr[1]>0xc){
				u8* pdeciaddr = (u8*)dmaGetAddr(d2ptr[4]+0xc, false);
				if( pdeciaddr == NULL )
					pdeciaddr = (u8*)PSM(d2ptr[4]+0xc);
				else
					pdeciaddr += (d2ptr[4]+0xc) % 16;

				const int copylen = std::min<uint>(255, d2ptr[1]-0xc);
				memcpy(deci2buffer, pdeciaddr, copylen );
				deci2buffer[copylen] = '\0';

				eeConLog( ShiftJIS_ConvertString(deci2buffer) );
			}
			((u32*)PSM(deci2addr))[3] = 0;
			return 1;
		}

		case 4:
			if( addr != NULL )
				BIOS_LOG("deci2poll: %x,%x,%x,%x\n", addr[3], addr[2], addr[1], addr[0]);
			return 1;

		case 5:
			return 1;

		case 6:
			return 1;

		case 0x10:
			if( addr != NULL )
			{
				eeDeci2Log( ShiftJIS_ConvertString((char*)PSM(*addr)) );
			}
			return 1;
	}

	return 0;
}

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {

void COP2()
{

	Int_COP2PrintTable[_Rs_]();
}

void Unknown() {
	CPU_LOG("%8.8lx: Unknown opcode called", cpuRegs.pc);
}

void MMI_Unknown() { Console.Warning("Unknown MMI opcode called"); }
void COP0_Unknown() { Console.Warning("Unknown COP0 opcode called"); }
void COP1_Unknown() { Console.Warning("Unknown FPU/COP1 opcode called"); }



void ADDI()
{
	s64 result;
	bool overflow = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], _Imm_, result );
	if (overflow || !_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = result;
}

void ADDIU()
{
	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = u64(s64(s32(cpuRegs.GPR.r[_Rs_].UL[0] + u32(s32(_Imm_)))));
}

void DADDI()
{
	s64 result;
	bool overflow = _add64_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], _Imm_, result );
	if (overflow || !_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = result;
}

void DADDIU()
{
	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] + u64(s64(_Imm_));
}
void ANDI() 	{ if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] & (u64)_ImmU_; }
void ORI() 	    { if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] | (u64)_ImmU_; }
void XORI() 	{ if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] ^ (u64)_ImmU_; }
void SLTI()     { if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = (cpuRegs.GPR.r[_Rs_].SD[0] < (s64)(_Imm_)) ? 1 : 0; }
void SLTIU()    { if (!_Rt_) return; cpuRegs.GPR.r[_Rt_].UD[0] = (cpuRegs.GPR.r[_Rs_].UD[0] < (u64)(_Imm_)) ? 1 : 0; }

void ADD()
{
	s64 result;
	bool overflow = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], cpuRegs.GPR.r[_Rt_].SD[0], result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

void DADD()
{
	s64 result;
	bool overflow = _add64_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], cpuRegs.GPR.r[_Rt_].SD[0], result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

void SUB()
{
	s64 result;
	bool overflow = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], -cpuRegs.GPR.r[_Rt_].SD[0], result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

void DSUB()
{
	s64 result;
	bool overflow = _add64_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], -cpuRegs.GPR.r[_Rt_].SD[0], result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

void ADDU() 	{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = u64(s64(s32(cpuRegs.GPR.r[_Rs_].UL[0]  + cpuRegs.GPR.r[_Rt_].UL[0]))); }
void DADDU()    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  + cpuRegs.GPR.r[_Rt_].UD[0]; }
void SUBU() 	{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = u64(s64(s32(cpuRegs.GPR.r[_Rs_].UL[0]  - cpuRegs.GPR.r[_Rt_].UL[0]))); }
void DSUBU() 	{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  - cpuRegs.GPR.r[_Rt_].UD[0]; }
void AND() 	    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  & cpuRegs.GPR.r[_Rt_].UD[0]; }
void OR() 	    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  | cpuRegs.GPR.r[_Rt_].UD[0]; }
void XOR() 	    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  ^ cpuRegs.GPR.r[_Rt_].UD[0]; }
void NOR() 	    { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] =~(cpuRegs.GPR.r[_Rs_].UD[0] | cpuRegs.GPR.r[_Rt_].UD[0]); }
void SLT()		{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (cpuRegs.GPR.r[_Rs_].SD[0] < cpuRegs.GPR.r[_Rt_].SD[0]) ? 1 : 0; }
void SLTU()		{ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (cpuRegs.GPR.r[_Rs_].UD[0] < cpuRegs.GPR.r[_Rt_].UD[0]) ? 1 : 0; }

void DIV()
{
	if (cpuRegs.GPR.r[_Rs_].UL[0] == 0x80000000 && cpuRegs.GPR.r[_Rt_].UL[0] == 0xffffffff)
	{
		cpuRegs.LO.SD[0] = (s32)0x80000000;
		cpuRegs.HI.SD[0] = (s32)0x0;
	}
    else if (cpuRegs.GPR.r[_Rt_].SL[0] != 0)
    {
        cpuRegs.LO.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0] / cpuRegs.GPR.r[_Rt_].SL[0];
        cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0] % cpuRegs.GPR.r[_Rt_].SL[0];
    }
	else
	{
		cpuRegs.LO.SD[0] = (cpuRegs.GPR.r[_Rs_].SL[0] < 0) ? 1 : -1;
		cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0];
	}
}

void DIVU()
{
	if (cpuRegs.GPR.r[_Rt_].UL[0] != 0)
	{
		cpuRegs.LO.SD[0] = (s32)(cpuRegs.GPR.r[_Rs_].UL[0] / cpuRegs.GPR.r[_Rt_].UL[0]);
		cpuRegs.HI.SD[0] = (s32)(cpuRegs.GPR.r[_Rs_].UL[0] % cpuRegs.GPR.r[_Rt_].UL[0]);
	}
	else
	{
		cpuRegs.LO.SD[0] = -1;
		cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0];
	}
}

void MULT()
{
	s64 res = (s64)cpuRegs.GPR.r[_Rs_].SL[0] * cpuRegs.GPR.r[_Rt_].SL[0];

	cpuRegs.LO.SD[0] = (s32)(res & 0xffffffff);
	cpuRegs.HI.SD[0] = (s32)(res >> 32);

	if( _Rd_ ) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0];
}

void MULTU()
{
	u64 res = (u64)cpuRegs.GPR.r[_Rs_].UL[0] * cpuRegs.GPR.r[_Rt_].UL[0];

	cpuRegs.LO.SD[0] = (s32)(res & 0xffffffff);
	cpuRegs.HI.SD[0] = (s32)(res >> 32);

	if( _Rd_ ) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0];
}

void LUI() {
	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = (s32)(cpuRegs.code << 16);
}

void MFHI() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.HI.UD[0]; }
void MFLO() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0]; }

void MTHI() { cpuRegs.HI.UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]; }
void MTLO() { cpuRegs.LO.UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]; }


void SRA()   { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].SL[0] >> _Sa_); }
void SRL()   { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].UL[0] >> _Sa_); }
void SLL()   { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].UL[0] << _Sa_); }
void DSLL()  { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (u64)(cpuRegs.GPR.r[_Rt_].UD[0] << _Sa_); }
void DSLL32(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (u64)(cpuRegs.GPR.r[_Rt_].UD[0] << (_Sa_+32));}
void DSRA()  { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = cpuRegs.GPR.r[_Rt_].SD[0] >> _Sa_; }
void DSRA32(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = cpuRegs.GPR.r[_Rt_].SD[0] >> (_Sa_+32);}
void DSRL()  { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rt_].UD[0] >> _Sa_; }
void DSRL32(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rt_].UD[0] >> (_Sa_+32);}

void SLLV() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].UL[0] << (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));}
void SRAV() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].SL[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));}
void SRLV() { if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s32)(cpuRegs.GPR.r[_Rt_].UL[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));}
void DSLLV(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (u64)(cpuRegs.GPR.r[_Rt_].UD[0] << (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}
void DSRAV(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].SD[0] = (s64)(cpuRegs.GPR.r[_Rt_].SD[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}
void DSRLV(){ if (!_Rd_) return; cpuRegs.GPR.r[_Rd_].UD[0] = (u64)(cpuRegs.GPR.r[_Rt_].UD[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}

__noinline static void RaiseAddressError(u32 addr, bool store)
{
	const std::string message(fmt::format("Address Error, addr=0x{:x} [{}]", addr, store ? "store" : "load"));

	Console.Error(message);

	Cpu->CancelInstruction();
}

void LB()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	s8 temp = memRead8(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = temp;

	if ((addr & 0xFFFFE000) == 0x10000000)
	{
		intUpdateCPUCycles();
		intEventTest();
	}
}

void LBU()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u8 temp = memRead8(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = temp;

	if ((addr & 0xFFFFE000) == 0x10000000)
	{
		intUpdateCPUCycles();
		intEventTest();
	}
}

void LH()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 1) [[unlikely]]
		RaiseAddressError(addr, false);

	s16 temp = memRead16(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = temp;

	if ((addr & 0xFFFFE000) == 0x10000000)
	{
		intUpdateCPUCycles();
		intEventTest();
	}
}

void LHU()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 1) [[unlikely]]
		RaiseAddressError(addr, false);

	u16 temp = memRead16(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = temp;

	if ((addr & 0xFFFFE000) == 0x10000000)
	{
		intUpdateCPUCycles();
		intEventTest();
	}
}

void LW()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 3) [[unlikely]]
		RaiseAddressError(addr, false);

	u32 temp = memRead32(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = (s32)temp;

	if ((addr & 0xFFFFE000) == 0x10000000)
	{
		intUpdateCPUCycles();
		intEventTest();
	}
}

void LWU()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 3) [[unlikely]]
		RaiseAddressError(addr, false);

	u32 temp = memRead32(addr);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] = temp;
}

static const u32 LWL_MASK[4] = { 0xffffff, 0x0000ffff, 0x000000ff, 0x00000000 };
static const u32 LWR_MASK[4] = { 0x000000, 0xff000000, 0xffff0000, 0xffffff00 };
static const u8 LWL_SHIFT[4] = { 24, 16, 8, 0 };
static const u8 LWR_SHIFT[4] = { 0, 8, 16, 24 };

void LWL()
{
	s32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;

	u32 mem = memRead32(addr & ~3);

	if (!_Rt_) return;

	cpuRegs.GPR.r[_Rt_].SD[0] =	(s32)((cpuRegs.GPR.r[_Rt_].UL[0] & LWL_MASK[shift]) |
								(mem << LWL_SHIFT[shift]));

}

void LWR()
{
	s32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;

	u32 mem = memRead32(addr & ~3);

	if (!_Rt_) return;

	mem = (cpuRegs.GPR.r[_Rt_].UL[0] & LWR_MASK[shift]) | (mem >> LWR_SHIFT[shift]);

	if( shift == 0 )
	{
		cpuRegs.GPR.r[_Rt_].SD[0] =	(s32)mem;
	}
	else
	{
		cpuRegs.GPR.r[_Rt_].UL[0] =	mem;
	}

}

alignas(16) static GPR_reg m_dummy_gpr_zero;

static GPR_reg* gpr_GetWritePtr( uint gpr )
{
	return (( gpr == 0 ) ? &m_dummy_gpr_zero : &cpuRegs.GPR.r[gpr]);
}

void LD()
{
    s32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 7) [[unlikely]]
		RaiseAddressError(addr, false);

	cpuRegs.GPR.r[_Rt_].UD[0] = memRead64(addr);
}

static const u64 LDL_MASK[8] =
{	0x00ffffffffffffffULL, 0x0000ffffffffffffULL, 0x000000ffffffffffULL, 0x00000000ffffffffULL,
	0x0000000000ffffffULL, 0x000000000000ffffULL, 0x00000000000000ffULL, 0x0000000000000000ULL
};
static const u64 LDR_MASK[8] =
{	0x0000000000000000ULL, 0xff00000000000000ULL, 0xffff000000000000ULL, 0xffffff0000000000ULL,
	0xffffffff00000000ULL, 0xffffffffff000000ULL, 0xffffffffffff0000ULL, 0xffffffffffffff00ULL
};

static const u8 LDR_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
static const u8 LDL_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };


void LDL()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;

	u64 mem = memRead64(addr & ~7);

	if( !_Rt_ ) return;
	cpuRegs.GPR.r[_Rt_].UD[0] =	(cpuRegs.GPR.r[_Rt_].UD[0] & LDL_MASK[shift]) |
								(mem << LDL_SHIFT[shift]);
}

void LDR()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;

	u64 mem = memRead64(addr & ~7);

	if (!_Rt_) return;
	cpuRegs.GPR.r[_Rt_].UD[0] =	(cpuRegs.GPR.r[_Rt_].UD[0] & LDR_MASK[shift]) |
								(mem >> LDR_SHIFT[shift]);
}

void LQ()
{

	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	memRead128(addr & ~0xf, (u128*)gpr_GetWritePtr(_Rt_));
}

void SB()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	memWrite8(addr, cpuRegs.GPR.r[_Rt_].UC[0]);
}

void SH()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 1) [[unlikely]]
		RaiseAddressError(addr, true);

	memWrite16(addr, cpuRegs.GPR.r[_Rt_].US[0]);
}

void SW()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 3) [[unlikely]]
		RaiseAddressError(addr, true);

  memWrite32(addr, cpuRegs.GPR.r[_Rt_].UL[0]);
}

static const u32 SWL_MASK[4] = { 0xffffff00, 0xffff0000, 0xff000000, 0x00000000 };
static const u32 SWR_MASK[4] = { 0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff };

static const u8 SWR_SHIFT[4] = { 0, 8, 16, 24 };
static const u8 SWL_SHIFT[4] = { 24, 16, 8, 0 };

void SWL()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;
	u32 mem = memRead32( addr & ~3 );

	memWrite32( addr & ~3,
		(cpuRegs.GPR.r[_Rt_].UL[0] >> SWL_SHIFT[shift]) |
		(mem & SWL_MASK[shift])
	);

}

void SWR() {
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;
	u32 mem = memRead32(addr & ~3);

	memWrite32( addr & ~3,
		(cpuRegs.GPR.r[_Rt_].UL[0] << SWR_SHIFT[shift]) |
		(mem & SWR_MASK[shift])
	);

}

void SD()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (addr & 7) [[unlikely]]
		RaiseAddressError(addr, true);

    memWrite64(addr,cpuRegs.GPR.r[_Rt_].UD[0]);
}

static const u64 SDL_MASK[8] =
{	0xffffffffffffff00ULL, 0xffffffffffff0000ULL, 0xffffffffff000000ULL, 0xffffffff00000000ULL,
	0xffffff0000000000ULL, 0xffff000000000000ULL, 0xff00000000000000ULL, 0x0000000000000000ULL
};
static const u64 SDR_MASK[8] =
{	0x0000000000000000ULL, 0x00000000000000ffULL, 0x000000000000ffffULL, 0x0000000000ffffffULL,
	0x00000000ffffffffULL, 0x000000ffffffffffULL, 0x0000ffffffffffffULL, 0x00ffffffffffffffULL
};

static const u8 SDL_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
static const u8 SDR_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };

void SDL()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;
	u64 mem = memRead64(addr & ~7);
	mem = (cpuRegs.GPR.r[_Rt_].UD[0] >> SDL_SHIFT[shift]) |
		  (mem & SDL_MASK[shift]);
	memWrite64(addr & ~7, mem);
}


void SDR()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;
	u64 mem = memRead64(addr & ~7);
	mem = (cpuRegs.GPR.r[_Rt_].UD[0] << SDR_SHIFT[shift]) |
		  (mem & SDR_MASK[shift]);
	memWrite64(addr & ~7, mem );
}

void SQ()
{

	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	memWrite128(addr & ~0xf, cpuRegs.GPR.r[_Rt_].UQ);
}

void MOVZ() {
	if (!_Rd_) return;
	if (cpuRegs.GPR.r[_Rt_].UD[0] == 0) {
		cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0];
	}
}
void MOVN() {
	if (!_Rd_) return;
	if (cpuRegs.GPR.r[_Rt_].UD[0] != 0) {
		cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0];
	}
}

#include "Sifcmd.h"

void SYSCALL()
{
	u8 call;

	if (cpuRegs.GPR.n.v1.SL[0] < 0)
		call = (u8)(-cpuRegs.GPR.n.v1.SL[0]);
	else
		call = cpuRegs.GPR.n.v1.UC[0];

	BIOS_LOG("Bios call: %s (%x)", R5900::bios[call], call);


	switch (static_cast<Syscall>(call))
	{
		case Syscall::SetGsCrt:
		{

			gsIsInterlaced = cpuRegs.GPR.n.a0.UL[0] & 1;
			bool gsIsFrameMode = cpuRegs.GPR.n.a2.UL[0] & 1;
			const char* inter = (gsIsInterlaced) ? "Interlaced" : "Progressive";
			const char* field = (gsIsFrameMode) ? "FRAME" : "FIELD";
			std::string mode;
			switch (cpuRegs.GPR.n.a1.UC[0])
			{
				case 0x0:
				case 0x2:
					mode = "NTSC 640x448 @ 59.940 (59.82)"; gsSetVideoMode(GS_VideoMode::NTSC); break;

				case 0x1:
				case 0x3:
					mode = "PAL  640x512 @ 50.000 (49.76)"; gsSetVideoMode(GS_VideoMode::PAL); break;

				case 0x1A: mode = "VESA 640x480 @ 59.940"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x1B: mode = "VESA 640x480 @ 72.809"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x1C: mode = "VESA 640x480 @ 75.000"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x1D: mode = "VESA 640x480 @ 85.008"; gsSetVideoMode(GS_VideoMode::VESA); break;

				case 0x2A: mode = "VESA 800x600 @ 56.250"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x2B: mode = "VESA 800x600 @ 60.317"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x2C: mode = "VESA 800x600 @ 72.188"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x2D: mode = "VESA 800x600 @ 75.000"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x2E: mode = "VESA 800x600 @ 85.061"; gsSetVideoMode(GS_VideoMode::VESA); break;

				case 0x3B: mode = "VESA 1024x768 @ 60.004"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x3C: mode = "VESA 1024x768 @ 70.069"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x3D: mode = "VESA 1024x768 @ 75.029"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x3E: mode = "VESA 1024x768 @ 84.997"; gsSetVideoMode(GS_VideoMode::VESA); break;

				case 0x4A: mode = "VESA 1280x1024 @ 63.981"; gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x4B: mode = "VESA 1280x1024 @ 79.976"; gsSetVideoMode(GS_VideoMode::VESA); break;

				case 0x50: mode = "SDTV   720x480 @ 59.94"; gsSetVideoMode(GS_VideoMode::SDTV_480P); break;
				case 0x51: mode = "HDTV 1920x1080 @ 60.00"; gsSetVideoMode(GS_VideoMode::HDTV_1080I); break;
				case 0x52: mode = "HDTV  1280x720 @ ??.???"; gsSetVideoMode(GS_VideoMode::HDTV_720P); break;
				case 0x53: mode = "SDTV   768x576 @ ??.???"; gsSetVideoMode(GS_VideoMode::SDTV_576P); break;
				case 0x54: mode = "HDTV 1920x1080 @ ??.???"; gsSetVideoMode(GS_VideoMode::HDTV_1080P); break;

				case 0x72:
				case 0x82:
					mode = "DVD NTSC 640x448 @ ??.???"; gsSetVideoMode(GS_VideoMode::DVD_NTSC); break;
				case 0x73:
				case 0x83:
					mode = "DVD PAL 720x480 @ ??.???"; gsSetVideoMode(GS_VideoMode::DVD_PAL); break;

				default:
					DevCon.Error("Mode %x is not supported. Report me upstream", cpuRegs.GPR.n.a1.UC[0]);
					gsSetVideoMode(GS_VideoMode::Unknown);
			}
			DevCon.Warning("Set GS CRTC configuration. %s %s (%s)",mode.c_str(), inter, field);
		}
		break;
		case Syscall::ExecPS2:
		{
			if (DebugInterface::getPauseOnEntry())
			{
				CBreakPoints::AddBreakPoint(BREAKPOINT_EE, cpuRegs.GPR.n.a0.UL[0], true);
				DebugInterface::setPauseOnEntry(false);
			}
		}
		break;
		case Syscall::RFU060:
			if (CHECK_EXTRAMEM && cpuRegs.GPR.n.a1.UL[0] == 0xFFFFFFFF)
			{
				cpuRegs.GPR.n.a1.UL[0] = Ps2MemSize::ExposedRam - cpuRegs.GPR.n.a2.SL[0];
			}
			break;
		case Syscall::SetOsdConfigParam:
			AllowParams1 = true;
			break;
		case Syscall::GetOsdConfigParam:
			if (!NoOSD && !AllowParams1)
			{
				ReadOSDConfigParames();

				u32 memaddr = cpuRegs.GPR.n.a0.UL[0];

				memWrite32(memaddr, configParams1.UL[0]);
				
				if (cpuRegs.GPR.n.v1.SL[0] < 0)
					cpuRegs.GPR.n.v1.SL[0] = -Syscall::SetOsdConfigParam;
				else
					cpuRegs.GPR.n.v1.UC[0] = Syscall::SetOsdConfigParam;

				AllowParams1 = true;
			}
			break;
		case Syscall::SetOsdConfigParam2:
			if (!AllowParams2)
			{
				ReadOSDConfigParames();

				u32 memaddr = cpuRegs.GPR.n.a0.UL[0];
				u32 size = cpuRegs.GPR.n.a1.UL[0];
				u32 offset = cpuRegs.GPR.n.a2.UL[0];

				if (offset == 0 && size >= 4)
					AllowParams2 = true;

				for (u32 i = 0; i < size; i++)
				{
					if (offset >= 4)
						break;

					configParams2.UC[offset++] = memRead8(memaddr++);
				}
			}
			break;
		case Syscall::GetOsdConfigParam2:
			if (!NoOSD && !AllowParams2)
			{
				ReadOSDConfigParames();

				u32 memaddr = cpuRegs.GPR.n.a0.UL[0];
				u32 size = cpuRegs.GPR.n.a1.UL[0];
				u32 offset = cpuRegs.GPR.n.a2.UL[0];

				if (offset + size > 2)
					Console.Warning("Warning: GetOsdConfigParam2 Reading extended language/version configs, may be incorrect!");

				for (u32 i = 0; i < size; i++)
				{
					if (offset >= 4)
						memWrite8(memaddr++, 0);
					else
						memWrite8(memaddr++, configParams2.UC[offset++]);
				}
				return;
			}
			break;
		case Syscall::SetVTLBRefillHandler:
			DevCon.Warning("A tlb refill handler is set. New handler %x", (u32*)PSM(cpuRegs.GPR.n.a1.UL[0]));
			break;
		case Syscall::StartThread:
		case Syscall::ChangeThreadPriority:
		{
			if (CurrentBiosInformation.eeThreadListAddr == 0)
			{
				u32 offset = 0x0;
				while (offset < 0x5000)
				{
					u32 addr = 0x80000000 + offset;
					const u32 inst1 = memRead32(addr);
					const u32 inst2 = memRead32(addr += 4);
					const u32 inst3 = memRead32(addr += 4);

					if (ThreadListInstructions[0] == inst1 &&
						ThreadListInstructions[1] == inst2 &&
						ThreadListInstructions[2] == inst3)
					{
						const u32 op = memRead32(0x80000000 + offset + (sizeof(u32) * 6));
						CurrentBiosInformation.eeThreadListAddr = 0x80010000 + static_cast<u16>(op) - 8;
						DevCon.WriteLn("BIOS: Successfully found the instruction pattern. Assuming the thread list is here: %0x", CurrentBiosInformation.eeThreadListAddr);
						break;
					}
					offset += 4;
				}
				if (!CurrentBiosInformation.eeThreadListAddr)
				{
					CurrentBiosInformation.eeThreadListAddr = -1;
					Console.Warning("BIOS Warning: Unable to get a thread list offset. The debugger thread and stack frame views will not be functional.");
				}
			}
		}
		break;
		case Syscall::sceSifSetDma:
			if (TraceActive(EE.Bios))
			{
				int n_transfer;
				u32 addr;

				n_transfer = cpuRegs.GPR.n.a1.UL[0] - 1;
				if (n_transfer >= 0)
				{
					addr = cpuRegs.GPR.n.a0.UL[0] + n_transfer * sizeof(t_sif_dma_transfer);
					t_sif_dma_transfer* dmat = (t_sif_dma_transfer*)PSM(addr);

					BIOS_LOG("bios_%s: n_transfer=%d, size=%x, attr=%x, dest=%x, src=%x",
							R5900::bios[cpuRegs.GPR.n.v1.UC[0]], n_transfer,
							dmat->size, dmat->attr,
							dmat->dest, dmat->src);
				}
			}
			break;

		case Syscall::Deci2Call:
		{
			if (cpuRegs.GPR.n.a0.UL[0] == 0x10)
			{
				eeConLog(ShiftJIS_ConvertString((char*)PSM(memRead32(cpuRegs.GPR.n.a1.UL[0]))));
			}
			else
				__Deci2Call(cpuRegs.GPR.n.a0.UL[0], (u32*)PSM(cpuRegs.GPR.n.a1.UL[0]));

			break;
		}
		case Syscall::sysPrintOut:
		{
			if (cpuRegs.GPR.n.a0.UL[0] != 0)
			{
				char* fmt = (char*)PSM(cpuRegs.GPR.n.a0.UL[0]);

				u64 regs[7] = {
					cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.a2.UL[0],
					cpuRegs.GPR.n.a3.UL[0],
					cpuRegs.GPR.n.t0.UL[0],
					cpuRegs.GPR.n.t1.UL[0],
					cpuRegs.GPR.n.t2.UL[0],
					cpuRegs.GPR.n.t3.UL[0],
				};

				int curRegArg = 0;
				for (int i = 0; 1; i++)
				{
					if (fmt[i] == '\0')
						break;

					if (fmt[i] == '%')
					{
						if (i == 0 || fmt[i - 1] != '%') {
							if (fmt[i + 1] == 's') {
								regs[curRegArg] = (u64)PSM(regs[curRegArg]);
							}
							curRegArg++;
						}
					}
				}
				char buf[2048];
				snprintf(buf, sizeof(buf), fmt,
					regs[0],
					regs[1],
					regs[2],
					regs[3],
					regs[4],
					regs[5],
					regs[6]
				);

				eeConLog(buf);
			}
			break;
		}
		case Syscall::GetMemorySize:
			if (CHECK_EXTRAMEM)
			{
				cpuRegs.GPR.n.v0.UL[0] = Ps2MemSize::ExposedRam;
				return;
			}
			break;


		default:
			break;
	}

	cpuRegs.pc -= 4;
	cpuException(0x20, cpuRegs.branch);
}

void BREAK() {
	cpuRegs.pc -= 4;
	cpuException(0x24, cpuRegs.branch);
}

void MFSA() {
	if (!_Rd_) return;
	cpuRegs.GPR.r[_Rd_].UD[0] = (u64)cpuRegs.sa;
}

void MTSA() {
	cpuRegs.sa = (u32)cpuRegs.GPR.r[_Rs_].UD[0];
}

void SYNC()
{
}

void PREF()
{
}

static void trap(u16 code=0)
{
	cpuRegs.pc -= 4;
	Console.Warning("Trap exception at 0x%08x", cpuRegs.pc);
	cpuException(0x34, cpuRegs.branch);
}

void TGE()  { if (cpuRegs.GPR.r[_Rs_].SD[0] >= cpuRegs.GPR.r[_Rt_].SD[0]) trap(_TrapCode_); }
void TGEU() { if (cpuRegs.GPR.r[_Rs_].UD[0] >= cpuRegs.GPR.r[_Rt_].UD[0]) trap(_TrapCode_); }
void TLT()  { if (cpuRegs.GPR.r[_Rs_].SD[0] <  cpuRegs.GPR.r[_Rt_].SD[0]) trap(_TrapCode_); }
void TLTU() { if (cpuRegs.GPR.r[_Rs_].UD[0] <  cpuRegs.GPR.r[_Rt_].UD[0]) trap(_TrapCode_); }
void TEQ()  { if (cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0]) trap(_TrapCode_); }
void TNE()  { if (cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0]) trap(_TrapCode_); }

void TGEI()  { if (cpuRegs.GPR.r[_Rs_].SD[0] >= _Imm_) trap(); }
void TLTI()  { if (cpuRegs.GPR.r[_Rs_].SD[0] <  _Imm_) trap(); }
void TEQI()  { if (cpuRegs.GPR.r[_Rs_].SD[0] == _Imm_) trap(); }
void TNEI()  { if (cpuRegs.GPR.r[_Rs_].SD[0] != _Imm_) trap(); }
void TGEIU() { if (cpuRegs.GPR.r[_Rs_].UD[0] >= (u64)_Imm_) trap(); }
void TLTIU() { if (cpuRegs.GPR.r[_Rs_].UD[0] <  (u64)_Imm_) trap(); }

void MTSAB() {
	cpuRegs.sa = ((cpuRegs.GPR.r[_Rs_].UL[0] & 0xF) ^ (_Imm_ & 0xF));
}

void MTSAH() {
	cpuRegs.sa = ((cpuRegs.GPR.r[_Rs_].UL[0] & 0x7) ^ (_Imm_ & 0x7)) << 1;
}

} }	}
