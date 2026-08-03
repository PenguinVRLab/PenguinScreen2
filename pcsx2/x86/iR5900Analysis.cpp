// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "iR5900Analysis.h"
#include "Memory.h"
#include "DebugTools/Debug.h"

using namespace R5900;

extern int cop2flags(u32 code);

AnalysisPass::AnalysisPass() = default;

AnalysisPass::~AnalysisPass() = default;

void AnalysisPass::Run(u32 start, u32 end, EEINST* inst_cache)
{
}

template <class F>
void __fi AnalysisPass::ForEachInstruction(u32 start, u32 end, EEINST* inst_cache, const F& func)
{
	EEINST* eeinst = inst_cache;
	for (u32 apc = start; apc < end; apc += 4, eeinst++)
	{
		cpuRegs.code = memRead32(apc);
		if (!func(apc, eeinst))
			break;
	}
}

template <class F>
void __fi R5900::AnalysisPass::DumpAnnotatedBlock(u32 start, u32 end, EEINST* inst_cache, const F& func)
{
	std::string d;
	EEINST* eeinst = inst_cache;
	for (u32 apc = start; apc < end; apc += 4, eeinst++)
	{
		const u32 code = memRead32(apc);
		d.clear();
		disR5900Fasm(d, code, apc, false);
		func(apc, eeinst, d);
		Console.WriteLn("  %08X %08X %s", apc, code, d.c_str());
	}
}

COP2FlagHackPass::COP2FlagHackPass()
	: AnalysisPass()
{
}

COP2FlagHackPass::~COP2FlagHackPass() = default;

void COP2FlagHackPass::Run(u32 start, u32 end, EEINST* inst_cache)
{
	m_status_denormalized = false;
	m_last_status_write = nullptr;
	m_last_mac_write = nullptr;
	m_last_clip_write = nullptr;
	m_cfc2_pc = start;

	ForEachInstruction(start, end, inst_cache, [this, end](u32 apc, EEINST* inst) {
		if (_Opcode_ == 050 || _Opcode_ == 051 || _Opcode_ == 053)
		{
			CommitAllFlags();
			return true;
		}
		else if (_Opcode_ != 022)
		{
			return true;
		}

		if (_Rs_ == 6 && _Rd_ == REG_STATUS_FLAG)
		{
			m_cfc2_pc = apc;
			ForEachInstruction(apc, end, inst, [this](u32 capc, EEINST*) {
				if (_Opcode_ == 022 && _Rs_ == 2 && _Rd_ == REG_STATUS_FLAG)
				{
					m_cfc2_pc = capc;
					return false;
				}
				return true;
			});
#ifdef PCSX2_DEVBUILD
			if (m_cfc2_pc != apc)
				DevCon.WriteLn("CTC2 at %08X paired with CFC2 %08X", apc, m_cfc2_pc);
#endif
		}

		if (_Rs_ == 6 || _Rs_ == 2)
		{
			switch (_Rd_)
			{
				case REG_STATUS_FLAG:
					CommitStatusFlag();
					break;
				case REG_MAC_FLAG:
					CommitMACFlag();
					break;
				case REG_CLIP_FLAG:
					CommitClipFlag();
					break;
				case REG_FBRST:
				{
					if (_Rs_ == 2)
						CommitAllFlags();
				}
				break;
			}
		}

		if (((cpuRegs.code >> 25 & 1) == 1) && ((cpuRegs.code >> 2 & 15) == 14))
		{
			CommitAllFlags();
		}

		const int flags = cop2flags(cpuRegs.code);
		if (flags == 0)
			return true;

		if (flags & 1)
		{
			if (!m_status_denormalized)
			{
				inst->info |= EEINST_COP2_DENORMALIZE_STATUS_FLAG;
				m_status_denormalized = true;
			}

			const u32 sub_opcode = (cpuRegs.code & 3) | ((cpuRegs.code >> 4) & 0x7c);
			if (apc < m_cfc2_pc || (_Rs_ >= 020 && _Funct_ >= 074 && sub_opcode >= 070 && sub_opcode <= 072))
				inst->info |= EEINST_COP2_STATUS_FLAG;

			m_last_status_write = inst;
		}

		if (flags & 2)
		{
			m_last_mac_write = inst;
		}

		if (flags & 4)
		{
			inst->info |= EEINST_COP2_CLIP_FLAG;
			m_last_clip_write = inst;
		}

		return true;
	});

	CommitAllFlags();

#if 0
	if (m_cfc2_pc != start)
		DumpAnnotatedBlock(start, end, inst_cache);
#endif
}

void COP2FlagHackPass::DumpAnnotatedBlock(u32 start, u32 end, EEINST* inst_cache)
{
	AnalysisPass::DumpAnnotatedBlock(start, end, inst_cache, [](u32, EEINST* eeinst, std::string& d) {
		if (eeinst->info & EEINST_COP2_DENORMALIZE_STATUS_FLAG)
			d.append(" COP2_DENORMALIZE_STATUS_FLAG");
		if (eeinst->info & EEINST_COP2_NORMALIZE_STATUS_FLAG)
			d.append(" COP2_NORMALIZE_STATUS_FLAG");
		if (eeinst->info & EEINST_COP2_STATUS_FLAG)
			d.append(" COP2_STATUS_FLAG");
		if (eeinst->info & EEINST_COP2_MAC_FLAG)
			d.append(" COP2_MAC_FLAG");
		if (eeinst->info & EEINST_COP2_CLIP_FLAG)
			d.append(" COP2_CLIP_FLAG");
	});
}

void COP2FlagHackPass::CommitStatusFlag()
{
	if (m_last_status_write)
	{
		m_last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
		m_status_denormalized = false;
	}
}

void COP2FlagHackPass::CommitMACFlag()
{
	if (m_last_mac_write)
		m_last_mac_write->info |= EEINST_COP2_MAC_FLAG;
}

void COP2FlagHackPass::CommitClipFlag()
{
	if (m_last_clip_write)
		m_last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
}

void COP2FlagHackPass::CommitAllFlags()
{
	CommitStatusFlag();
	CommitMACFlag();
	CommitClipFlag();
}

COP2MicroFinishPass::COP2MicroFinishPass() = default;

COP2MicroFinishPass::~COP2MicroFinishPass() = default;

void COP2MicroFinishPass::Run(u32 start, u32 end, EEINST* inst_cache)
{
	bool needs_vu0_sync = true;
	bool needs_vu0_finish = true;
	bool block_interlocked = CHECK_FULLVU0SYNCHACK;

	ForEachInstruction(start, end, inst_cache, [&block_interlocked](u32 apc, EEINST* inst) {
		if (_Opcode_ == 022 && (_Rs_ == 001 || _Rs_ == 002 || _Rs_ == 005 || _Rs_ == 006) && cpuRegs.code & 1)
		{
			block_interlocked = true;
			return false;
		}
		return true;
	});

	ForEachInstruction(start, end, inst_cache, [this, end, inst_cache, &needs_vu0_sync, &needs_vu0_finish, block_interlocked](u32 apc, EEINST* inst) {
		if (_Opcode_ == 050 || _Opcode_ == 051 || _Opcode_ == 053 || _Opcode_ == 077 || (_Opcode_ == 022 && _Rs_ >= 020 && (_Funct_ == 070 || _Funct_ == 071)))
		{
			needs_vu0_sync = true;
			needs_vu0_finish = true;
			inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS;
			return true;
		}

		const bool is_lqc_sqc = (_Opcode_ == 066 || _Opcode_ == 076);
		const bool is_non_interlocked_move = (_Opcode_ == 022 && _Rs_ < 020 && ((cpuRegs.code & 1) == 0));
		const bool likely_clear = _Opcode_ == 022 && _Rs_ < 020 && _Rs_ > 004 && _Rt_ == 000;
		if ((needs_vu0_sync && (is_lqc_sqc || is_non_interlocked_move)) || likely_clear)
		{
			bool following_needs_finish = false;
			ForEachInstruction(apc + 4, end, inst_cache + 1, [&following_needs_finish](u32 apc2, EEINST* inst2) {
				if (_Opcode_ == 022)
				{
					if (_Rs_ >= 020 && (_Funct_ == 070 || _Funct_ == 071))
						return false;

					following_needs_finish = _Rs_ >= 020;
					if (following_needs_finish)
						return false;
				}

				return true;
			});
			if (following_needs_finish && !block_interlocked)
			{
				inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_FINISH_VU0;
				needs_vu0_sync = false;
				needs_vu0_finish = false;
			}
			else
			{
				inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_SYNC_VU0;
				needs_vu0_sync = block_interlocked || (is_non_interlocked_move && likely_clear);
				needs_vu0_finish = true;
			}

			return true;
		}

		if (_Opcode_ != 022)
			return true;

		if (_Rs_ >= 020 && needs_vu0_finish)
		{
			inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_FINISH_VU0;
			needs_vu0_finish = false;
			needs_vu0_sync = false;
		}
		else if (needs_vu0_sync)
		{
			inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_SYNC_VU0;
			needs_vu0_sync = block_interlocked;
		}

		return true;
	});

#if 0
	if (!block_interlocked)
		return;
#endif

#if 0
	Console.WriteLn("-- Beginning of COP2 block at %08X - %08X%s", start, end, block_interlocked ? " [BLOCK IS INTERLOCKED]" : "");
	AnalysisPass::DumpAnnotatedBlock(start, end, inst_cache, [](u32, EEINST* eeinst, std::string& d) {
		if (eeinst->info & EEINST_COP2_DENORMALIZE_STATUS_FLAG)
			d.append(" COP2_DENORMALIZE_STATUS_FLAG");
		if (eeinst->info & EEINST_COP2_NORMALIZE_STATUS_FLAG)
			d.append(" COP2_NORMALIZE_STATUS_FLAG");
		if (eeinst->info & EEINST_COP2_STATUS_FLAG)
			d.append(" COP2_STATUS_FLAG");
		if (eeinst->info & EEINST_COP2_MAC_FLAG)
			d.append(" COP2_MAC_FLAG");
		if (eeinst->info & EEINST_COP2_CLIP_FLAG)
			d.append(" COP2_CLIP_FLAG");
		if (eeinst->info & EEINST_COP2_SYNC_VU0)
			d.append(" COP2_SYNC_VU0");
		if (eeinst->info & EEINST_COP2_FINISH_VU0)
			d.append(" COP2_FINISH_VU0");
		if (eeinst->info & EEINST_COP2_FLUSH_VU0_REGISTERS)
			d.append(" COP2_FLUSH_VU0_REGISTERS");
	});
	Console.WriteLn("-- End of COP2 block at %08X - %08X", start, end);
#endif
}

#define recBackpropSetGPRRead(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			prev->regs[reg] = (EEINST_LIVE | EEINST_USED); \
			pinst->regs[reg] = (pinst->regs[reg] & ~EEINST_XMM) | EEINST_USED; \
			_recFillRegister(*pinst, XMMTYPE_GPRREG, reg, 0); \
		} \
	} while (0)

#define recBackpropSetGPRWrite(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			prev->regs[reg] &= ~(EEINST_XMM | EEINST_LIVE | EEINST_USED); \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_USED; \
			_recFillRegister(*pinst, XMMTYPE_GPRREG, reg, 1); \
		} \
	} while (0)

#define recBackpropSetGPRRead128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			prev->regs[reg] |= EEINST_LIVE | EEINST_USED | EEINST_XMM; \
			pinst->regs[reg] |= EEINST_USED | EEINST_XMM; \
			_recFillRegister(*pinst, XMMTYPE_GPRREG, reg, 0); \
		} \
	} while (0)

#define recBackpropSetGPRPartialWrite128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_LIVE | EEINST_USED | EEINST_XMM; \
			prev->regs[reg] |= EEINST_USED | EEINST_XMM; \
			_recFillRegister(*pinst, XMMTYPE_GPRREG, reg, 1); \
		} \
	} while (0)

#define recBackpropSetGPRWrite128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			prev->regs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_USED | EEINST_XMM; \
			_recFillRegister(*pinst, XMMTYPE_GPRREG, reg, 1); \
		} \
	} while (0)

#define recBackpropSetFPURead(reg) \
	do \
	{ \
		if (!(pinst->fpuregs[reg] & EEINST_USED)) \
			pinst->fpuregs[reg] |= EEINST_LASTUSE; \
		prev->fpuregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->fpuregs[reg] |= EEINST_USED; \
		_recFillRegister(*pinst, XMMTYPE_FPREG, reg, 0); \
	} while (0)

#define recBackpropSetFPUWrite(reg) \
	do \
	{ \
		prev->fpuregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->fpuregs[reg] & EEINST_USED)) \
			pinst->fpuregs[reg] |= EEINST_LASTUSE; \
		pinst->fpuregs[reg] |= EEINST_USED; \
		_recFillRegister(*pinst, XMMTYPE_FPREG, reg, 1); \
	} while (0)

#define recBackpropSetVFRead(reg) \
	do \
	{ \
		if (!(pinst->vfregs[reg] & EEINST_USED)) \
			pinst->vfregs[reg] |= EEINST_LASTUSE; \
		prev->vfregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->vfregs[reg] |= EEINST_USED; \
		_recFillRegister(*pinst, XMMTYPE_VFREG, reg, 0); \
	} while (0)

#define recBackpropSetVFWrite(reg) \
	do \
	{ \
		prev->vfregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->vfregs[reg] & EEINST_USED)) \
			pinst->vfregs[reg] |= EEINST_LASTUSE; \
		pinst->vfregs[reg] |= EEINST_USED; \
		_recFillRegister(*pinst, XMMTYPE_VFREG, reg, 1); \
	} while (0)

#define recBackpropSetVIRead(reg) \
	if ((reg) < 16) \
	{ \
		if (!(pinst->viregs[reg] & EEINST_USED)) \
			pinst->viregs[reg] |= EEINST_LASTUSE; \
		prev->viregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->viregs[reg] |= EEINST_USED; \
		_recFillRegister(*pinst, X86TYPE_VIREG, reg, 0); \
	}

#define recBackpropSetVIWrite(reg) \
	if ((reg) < 16) \
	{ \
		prev->viregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->viregs[reg] & EEINST_USED)) \
			pinst->viregs[reg] |= EEINST_LASTUSE; \
		pinst->viregs[reg] |= EEINST_USED; \
		_recFillRegister(*pinst, X86TYPE_VIREG, reg, 1); \
	}

static void recBackpropSPECIAL(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropREGIMM(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP0(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP1(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP2(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropMMI(u32 code, EEINST* prev, EEINST* pinst);

void recBackpropBSC(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (code >> 26)
	{
		case 0:
			recBackpropSPECIAL(code, prev, pinst);
			break;
		case 1:
			recBackpropREGIMM(code, prev, pinst);
			break;
		case 2:
			break;
		case 3:
			recBackpropSetGPRWrite(31);
			break;
		case 4:
		case 5:
		case 20:
		case 21:
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 6:
		case 7:
		case 22:
		case 23:
			recBackpropSetGPRRead(rs);
			break;

		case 15:
			recBackpropSetGPRWrite(rt);
			break;

		case 8:
		case 9:
		case 10:
		case 11:
		case 12:
		case 13:
		case 14:
		case 24:
		case 25:
			recBackpropSetGPRWrite(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 32:
		case 33:
		case 35:
		case 36:
		case 37:
		case 39:
		case 55:
			recBackpropSetGPRWrite(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 30:
			recBackpropSetGPRWrite128(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 26:
		case 27:
		case 34:
		case 38:
			recBackpropSetGPRWrite(rt);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 40:
		case 41:
		case 42:
		case 43:
		case 44:
		case 45:
		case 46:
		case 63:
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 31:
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead128(rs);
			break;

		case 16:
			recBackpropCOP0(code, prev, pinst);
			break;

		case 17:
			recBackpropCOP1(code, prev, pinst);
			break;

		case 18:
			recBackpropCOP2(code, prev, pinst);
			break;

		case 28:
			recBackpropMMI(code, prev, pinst);
			break;

		case 49:
			recBackpropSetGPRRead(rs);
			recBackpropSetFPURead(rt);
			break;

		case 57:
			recBackpropSetGPRRead(rs);
			recBackpropSetFPURead(rt);
			break;

		case 54:
			recBackpropSetVFWrite(rt);
			recBackpropSetGPRRead128(rs);
			break;

		case 62:
			recBackpropSetGPRRead128(rs);
			recBackpropSetVFRead(rt);
			break;

		case 47:
			recBackpropSetGPRRead(rs);
			break;

		case 51:
			break;

		default:
			Console.Warning("Unknown R5900 Standard: %08X", code);
			break;
	}
}

void recBackpropSPECIAL(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 rd = ((code >> 11) & 0x1F);
	const u32 funct = (code & 0x3F);

	switch (funct)
	{
		case 0:
		case 2:
		case 3:
		case 56:
		case 58:
		case 59:
		case 60:
		case 62:
		case 63:
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rt);
			break;

		case 4:
		case 6:
		case 7:
		case 10:
		case 11:
		case 20:
		case 22:
		case 23:
		case 32:
		case 33:
		case 34:
		case 35:
		case 36:
		case 37:
		case 38:
		case 39:
		case 42:
		case 43:
		case 44:
		case 45:
		case 46:
		case 47:
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 8:
			recBackpropSetGPRRead(rs);
			break;

		case 9:
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rs);
			break;

		case 24:
		case 25:
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 26:
		case 27:
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 16:
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(XMMGPR_HI);
			break;

		case 17:
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			break;

		case 18:
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(XMMGPR_LO);
			break;

		case 19:
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRRead(rs);
			break;

		case 40:
			recBackpropSetGPRWrite(rd);
			break;

		case 41:
			recBackpropSetGPRRead(rs);
			break;

		case 48:
		case 49:
		case 50:
		case 51:
		case 52:
		case 54:
			recBackpropSetGPRRead(rs);
			break;

		case 15:
			break;

		case 12:
		case 13:
			_recClearInst(prev);
			prev->info = 0;
			break;

		default:
			Console.Warning("Unknown R5900 SPECIAL: %08X", code);
			break;
	}
}

void recBackpropREGIMM(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (rt)
	{
		case 0:
		case 1:
		case 2:
		case 3:
		case 9:
		case 10:
		case 11:
		case 12:
		case 13:
		case 15:
		case 24:
		case 25:
			recBackpropSetGPRRead(rs);
			break;

		case 16:
		case 17:
		case 18:
		case 19:
			recBackpropSetGPRRead(rs);
			break;

		default:
			Console.Warning("Unknown R5900 REGIMM: %08X", code);
			break;
	}
}

void recBackpropCOP0(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (rs)
	{
		case 0:
		case 2:
			recBackpropSetGPRWrite(rt);
			break;

		case 4:
		case 6:
			recBackpropSetGPRRead(rt);
			break;

		case 8:
		case 16:
			break;

		default:
			Console.Warning("Unknown R5900 COP0: %08X", code);
			break;
	}
}

void recBackpropCOP1(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 fmt = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 fs = ((code >> 11) & 0x1F);
	const u32 ft = ((code >> 16) & 0x1F);
	const u32 fd = ((code >> 6) & 0x1F);
	const u32 funct = (code & 0x3F);

	switch (fmt)
	{
		case 0:
			recBackpropSetGPRWrite(rt);
			recBackpropSetFPURead(fs);
			break;

		case 2:
			recBackpropSetGPRWrite(rt);
			break;

		case 4:
			recBackpropSetFPUWrite(fs);
			recBackpropSetGPRRead(rt);
			break;

		case 6:
			recBackpropSetGPRRead(rt);
			break;

		case 8:
			break;

		case 16:
		{
			switch (funct)
			{
				case 0:
				case 1:
				case 2:
				case 3:
				case 40:
				case 41:
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					break;

				case 5:
				case 6:
				case 7:
				case 36:
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					break;

				case 24:
				case 25:
				case 26:
					recBackpropSetFPUWrite(XMMFPU_ACC);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					break;

				case 28:
				case 29:
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					recBackpropSetFPURead(XMMFPU_ACC);
					break;

				case 30:
				case 31:
					recBackpropSetFPUWrite(XMMFPU_ACC);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					recBackpropSetFPURead(XMMFPU_ACC);
					break;

				case 4:
				case 22:
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(ft);
					break;

				case 48:
					break;

				case 50:
				case 52:
				case 54:
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					break;

				default:
					Console.Warning("Unknown R5900 COP1: %08X", code);
					break;
			}
		}
		break;

		case 20:
		{
			switch (funct)
			{
				case 32:
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					break;

				default:
					Console.Warning("Unknown R5900 COP1: %08X", code);
					break;
			}
		}
		break;

		default:
			Console.Warning("Unknown R5900 COP1: %08X", code);
			break;
	}
}

void recBackpropCOP2(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 fmt = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 fs = ((code >> 11) & 0x1F);
	const u32 ft = ((code >> 16) & 0x1F);
	const u32 fd = ((code >> 6) & 0x1F);
	const u32 funct = (code & 0x3F);

	constexpr u32 VF_ACC = 32;
	constexpr u32 VF_I = 33;

	switch (fmt)
	{
		case 1:
			recBackpropSetGPRWrite128(rt);
			recBackpropSetVFRead(fs);
			break;

		case 2:
			recBackpropSetGPRWrite(rt);
			recBackpropSetVIRead(fs);
			break;

		case 5:
			recBackpropSetVFWrite(fs);
			recBackpropSetGPRRead128(rt);
			break;

		case 6:
			recBackpropSetVIWrite(fs);
			recBackpropSetGPRRead(rt);
			break;

		case 8:
			break;

		case 16:
		case 17:
		case 18:
		case 19:
		case 20:
		case 21:
		case 22:
		case 23:
		case 24:
		case 25:
		case 26:
		case 27:
		case 28:
		case 29:
		case 30:
		case 31:
		{
			switch (funct)
			{
				case 0:
				case 1:
				case 2:
				case 3:
				case 4:
				case 5:
				case 6:
				case 7:
				case 16:
				case 17:
				case 18:
				case 19:
				case 20:
				case 21:
				case 22:
				case 23:
				case 24:
				case 25:
				case 26:
				case 27:
				case 40:
				case 42:
				case 43:
				case 44:
				case 47:
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(ft);
					recBackpropSetVFRead(fd);
					break;

				case 8:
				case 9:
				case 10:
				case 11:
				case 12:
				case 13:
				case 14:
				case 15:
				case 41:
				case 45:
				case 46:
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(ft);
					recBackpropSetVFRead(VF_ACC);
					recBackpropSetVFRead(fd);
					break;

				case 29:
				case 30:
				case 31:
				case 34:
				case 38:
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_I);
					break;

				case 35:
				case 39:
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_ACC);
					recBackpropSetVFRead(VF_I);
					break;

				case 28:
				case 32:
				case 36:
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					break;

				case 33:
				case 37:
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_ACC);
					break;

				case 48:
				case 49:
				case 50:
				case 52:
				case 53:
				{
					const u32 is = fs & 0xFu;
					const u32 it = ft & 0xFu;
					const u32 id = fd & 0xFu;
					recBackpropSetVIWrite(id);
					recBackpropSetVIRead(is);
					recBackpropSetVIRead(it);
					recBackpropSetVIRead(id);
				}
				break;


				case 56:
				case 57:
					break;

				case 60:
				case 61:
				case 62:
				case 63:
				{
					const u32 idx = (code & 3u) | ((code >> 4) & 0x7cu);
					switch (idx)
					{
						case 0:
						case 1:
						case 2:
						case 3:
						case 4:
						case 5:
						case 6:
						case 7:
						case 24:
						case 25:
						case 26:
						case 27:
						case 40:
						case 42:
						case 44:
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 8:
						case 9:
						case 10:
						case 11:
						case 12:
						case 13:
						case 14:
						case 15:
						case 41:
						case 45:
						case 46:
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 16:
						case 17:
						case 18:
						case 19:
						case 20:
						case 21:
						case 22:
						case 23:
						case 29:
						case 48:
						case 49:
							recBackpropSetVFWrite(ft);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							break;

						case 31:
							recBackpropSetVFRead(fs);
							break;

						case 30:
						case 34:
						case 38:
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(VF_I);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 35:
						case 39:
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(VF_I);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 32:
						case 36:
						case 28:
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 33:
						case 37:
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 52:
						case 54:
							recBackpropSetVFWrite(ft);
							recBackpropSetVIWrite(fs & 0xFu);
							recBackpropSetVIRead(fs & 0xFu);
							recBackpropSetVFRead(ft);
							break;

						case 53:
						case 55:
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVIRead(ft & 0xFu);
							recBackpropSetVFRead(fs);
							break;

						case 56:
						case 58:
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							break;

						case 57:
							recBackpropSetVFRead(ft);
							break;


						case 60:
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVFRead(fs);
							break;

						case 61:
							recBackpropSetVFWrite(ft);
							recBackpropSetVIRead(fs & 0xFu);
							break;

						case 62:
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVIRead(fs & 0xFu);
							break;

						case 63:
							recBackpropSetVIRead(fs & 0xFu);
							recBackpropSetVIRead(ft & 0xFu);
							break;

						case 64:
						case 65:
							recBackpropSetVFWrite(ft);
							break;

						case 66:
						case 67:
							recBackpropSetVFRead(fs);
							break;

						case 47:
						case 59:
							break;

						default:
							Console.Warning("Unknown R5900 COP2 SPEC2: %08X", code);
							break;
					}
				}
				break;

				default:
					Console.Warning("Unknown R5900 COP2 SPEC1: %08X", code);
					break;
			}
		}
		break;

		default:
			break;
	}
}

void recBackpropMMI(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 funct = (code & 0x3F);
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 rd = ((code >> 11) & 0x1F);

	switch (funct)
	{
		case 0:
		case 1:
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead(XMMGPR_LO);
			recBackpropSetGPRRead(XMMGPR_HI);
			break;

		case 32:
		case 33:
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRRead128(XMMGPR_HI);
			break;

		case 24:
		case 25:
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRWrite(rd);
			break;

		case 26:
		case 27:
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 16:
			recBackpropSetGPRRead128(XMMGPR_HI);
			recBackpropSetGPRWrite(rd);
			break;

		case 17:
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			break;

		case 18:
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRWrite(rd);
			break;

		case 19:
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRRead(rs);
			break;

		case 4:
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRWrite(rd);
			break;

		case 48:
			recBackpropSetGPRPartialWrite128(rd);
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRRead128(XMMGPR_HI);
			break;

		case 49:
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead128(rs);
			break;

		case 52:
		case 54:
		case 55:
		case 60:
		case 62:
		case 63:
			recBackpropSetGPRWrite128(rd);
			recBackpropSetGPRRead128(rt);
			break;

		case 8:
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0:
				case 1:
				case 2:
				case 3:
				case 4:
				case 5:
				case 6:
				case 7:
				case 8:
				case 9:
				case 10:
				case 16:
				case 17:
				case 18:
				case 19:
				case 20:
				case 21:
				case 22:
				case 23:
				case 24:
				case 25:
				case 26:
				case 27:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 30:
				case 31:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					Console.Warning("Unknown R5900 MMI0: %08X", code);
					break;
			}
		}
		break;

		case 40:
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 2:
				case 3:
				case 4:
				case 6:
				case 7:
				case 10:
				case 16:
				case 17:
				case 18:
				case 20:
				case 21:
				case 22:
				case 24:
				case 25:
				case 26:
				case 27:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 1:
				case 5:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				case 0:
				default:
					Console.Warning("Unknown R5900 MMI1: %08X", code);
					break;
			}
		}
		break;

		case 9:
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0:
				case 4:
				case 16:
				case 17:
				case 20:
				case 21:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					recBackpropSetGPRRead128(XMMGPR_LO);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 12:
				case 28:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 13:
				case 29:
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 2:
				case 3:
				case 10:
				case 14:
				case 18:
				case 19:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 8:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(XMMGPR_LO);
					break;

				case 9:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 26:
				case 27:
				case 30:
				case 31:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					Console.Warning("Unknown R5900 MMI2: %08X", code);
					break;
			}
		}
		break;

		case 41:
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					recBackpropSetGPRRead128(XMMGPR_LO);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 3:
				case 10:
				case 18:
				case 19:
				case 14:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 26:
				case 27:
				case 30:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				case 8:
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					break;

				case 9:
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRRead128(rs);
					break;

				case 12:
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 13:
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					Console.Warning("Unknown R5900 MMI3: %08X", code);
					break;
			}
		}
		break;

		default:
		{
			Console.Warning("Unknown R5900 MMI: %08X", code);
		}
		break;
	}
}
