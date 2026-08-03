// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "iR5900.h"
#include "R5900OpcodeTables.h"

using namespace x86Emitter;

namespace R5900 {
namespace Dynarec {

void recDoBranchImm(u32 branchTo, u32* jmpSkip, bool isLikely, bool swappedDelaySlot)
{

	if (!swappedDelaySlot)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

	x86SetJ32(jmpSkip);

	if (!swappedDelaySlot)
	{
		LoadBranchState();
		if (!isLikely)
		{
			pc -= 4;
			recompileNextInstruction(true, false);
		}
	}

	SetBranchImm(pc);
}

namespace OpcodeImpl {

void recPREF()
{
}

void recSYNC()
{
}

void recMFSA()
{
	if (!_Rd_)
		return;

	if (const int mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_WRITE); mmreg >= 0)
	{
		const int temp = _allocTempXMMreg(XMMT_INT);
		xMOVSSZX(xRegisterSSE(temp), ptr32[&cpuRegs.sa]);
		xBLEND.PD(xRegisterSSE(mmreg), xRegisterSSE(temp), 1);
		_freeXMMreg(temp);
	}
	else if (const int gprreg = _allocIfUsedGPRtoX86(_Rd_, MODE_WRITE); gprreg >= 0)
	{
		xMOV(xRegister32(gprreg), ptr32[&cpuRegs.sa]);
	}
	else
	{
		_deleteEEreg(_Rd_, 0);
		xMOV(eax, ptr32[&cpuRegs.sa]);
		xMOV(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], rax);
	}
}

void recMTSA()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], g_cpuConstRegs[_Rs_].UL[0] & 0xf);
	}
	else
	{
		int mmreg;

		if ((mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0)
		{
			xMOVSS(ptr[&cpuRegs.sa], xRegisterSSE(mmreg));
		}
		else if ((mmreg = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ)) >= 0)
		{
			xMOV(ptr[&cpuRegs.sa], xRegister32(mmreg));
		}
		else
		{
			xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[0]]);
			xMOV(ptr[&cpuRegs.sa], eax);
		}
		xAND(ptr32[&cpuRegs.sa], 0xf);
	}
}

void recMTSAB()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], ((g_cpuConstRegs[_Rs_].UL[0] & 0xF) ^ (_Imm_ & 0xF)));
	}
	else
	{
		_eeMoveGPRtoR(eax, _Rs_);
		xAND(eax, 0xF);
		xXOR(eax, _Imm_ & 0xf);
		xMOV(ptr[&cpuRegs.sa], eax);
	}
}

void recMTSAH()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], ((g_cpuConstRegs[_Rs_].UL[0] & 0x7) ^ (_Imm_ & 0x7)) << 1);
	}
	else
	{
		_eeMoveGPRtoR(eax, _Rs_);
		xAND(eax, 0x7);
		xXOR(eax, _Imm_ & 0x7);
		xSHL(eax, 1);
		xMOV(ptr[&cpuRegs.sa], eax);
	}
}

void recNULL()
{
	Console.Error("EE: Unimplemented op %x", cpuRegs.code);
}

void recUnknown()
{
	Console.Error("EE: Unrecognized op %x", cpuRegs.code);
}

void recMMI_Unknown()
{
	Console.Error("EE: Unrecognized MMI op %x", cpuRegs.code);
}

void recCOP0_Unknown()
{
	Console.Error("EE: Unrecognized COP0 op %x", cpuRegs.code);
}

void recCOP1_Unknown()
{
	Console.Error("EE: Unrecognized FPU/COP1 op %x", cpuRegs.code);
}

void recCACHE()
{
}

void recTGE()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TGE);
}

void recTGEU()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TGEU);
}

void recTLT()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TLT);
}

void recTLTU()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TLTU);
}

void recTEQ()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TEQ);
}

void recTNE()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TNE);
}

void recTGEI()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TGEI);
}

void recTGEIU()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TGEIU);
}

void recTLTI()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TLTI);
}

void recTLTIU()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TLTIU);
}

void recTEQI()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TEQI);
}

void recTNEI()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TNEI);
}

}
}
}
