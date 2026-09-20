// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "IopGte.h"
#include "IopMem.h"

#include "common/Console.h"

void psxBGEZ();
void psxBGEZAL();
void psxBGTZ();
void psxBLEZ();
void psxBLTZ();
void psxBLTZAL();

void psxBEQ();
void psxBNE();
void psxJ();
void psxJAL();

void psxJR();
void psxJALR();

void psxADDI() 	{ if (!_Rt_) return; _rRt_ = _u32(_rRs_) + _Imm_ ; }
void psxADDIU() {
	if (!_Rt_)
		return;
	_rRt_ = _u32(_rRs_) + _Imm_ ;
}
void psxANDI() 	{ if (!_Rt_) return; _rRt_ = _u32(_rRs_) & _ImmU_; }
void psxORI() 	{ if (!_Rt_) return; _rRt_ = _u32(_rRs_) | _ImmU_; }
void psxXORI() 	{ if (!_Rt_) return; _rRt_ = _u32(_rRs_) ^ _ImmU_; }
void psxSLTI() 	{ if (!_Rt_) return; _rRt_ = _i32(_rRs_) < _Imm_ ; }
void psxSLTIU() { if (!_Rt_) return; _rRt_ = _u32(_rRs_) < (u32)_Imm_; }

void psxADD()	{ if (!_Rd_) return; _rRd_ = _u32(_rRs_) + _u32(_rRt_); }
void psxADDU() 	{ if (!_Rd_) return; _rRd_ = _u32(_rRs_) + _u32(_rRt_); }
void psxSUB() 	{ if (!_Rd_) return; _rRd_ = _u32(_rRs_) - _u32(_rRt_); }
void psxSUBU() 	{ if (!_Rd_) return; _rRd_ = _u32(_rRs_) - _u32(_rRt_); }
void psxAND() 	{ if (!_Rd_) return; _rRd_ = _u32(_rRs_) & _u32(_rRt_); }
void psxOR() 	{ if (!_Rd_) return; _rRd_ = _u32(_rRs_) | _u32(_rRt_); }
void psxXOR() 	{ if (!_Rd_) return; _rRd_ = _u32(_rRs_) ^ _u32(_rRt_); }
void psxNOR() 	{ if (!_Rd_) return; _rRd_ =~(_u32(_rRs_) | _u32(_rRt_)); }
void psxSLT() 	{ if (!_Rd_) return; _rRd_ = _i32(_rRs_) < _i32(_rRt_); }
void psxSLTU() 	{ if (!_Rd_) return; _rRd_ = _u32(_rRs_) < _u32(_rRt_); }

void psxDIV() {
	if (_rRt_ == 0) {
		_rLo_ = _i32(_rRs_) < 0 ? 1 : 0xFFFFFFFFu;
		_rHi_ = _rRs_;

	} else if (_rRs_ == 0x80000000u && _rRt_ == 0xFFFFFFFFu) {
		_rLo_ = 0x80000000u;
		_rHi_ = 0;

	} else {
		_rLo_ = _i32(_rRs_) / _i32(_rRt_);
		_rHi_ = _i32(_rRs_) % _i32(_rRt_);
	}
}

void psxDIVU() {
	if (_rRt_ == 0) {
		_rLo_ = 0xFFFFFFFFu;
		_rHi_ = _rRs_;

	} else {
		_rLo_ = _rRs_ / _rRt_;
		_rHi_ = _rRs_ % _rRt_;
	}
}

void psxMULT() {
	u64 res = (s64)((s64)_i32(_rRs_) * (s64)_i32(_rRt_));

	psxRegs.GPR.n.lo = (u32)(res & 0xffffffff);
	psxRegs.GPR.n.hi = (u32)((res >> 32) & 0xffffffff);
}

void psxMULTU() {
	u64 res = (u64)((u64)_u32(_rRs_) * (u64)_u32(_rRt_));

	psxRegs.GPR.n.lo = (u32)(res & 0xffffffff);
	psxRegs.GPR.n.hi = (u32)((res >> 32) & 0xffffffff);
}

void psxSLL() { if (!_Rd_) return; _rRd_ = _u32(_rRt_) << _Sa_; }
void psxSRA() { if (!_Rd_) return; _rRd_ = _i32(_rRt_) >> _Sa_; }
void psxSRL() { if (!_Rd_) return; _rRd_ = _u32(_rRt_) >> _Sa_; }

void psxSLLV() { if (!_Rd_) return; _rRd_ = _u32(_rRt_) << _u32(_rRs_); }
void psxSRAV() { if (!_Rd_) return; _rRd_ = _i32(_rRt_) >> _u32(_rRs_); }
void psxSRLV() { if (!_Rd_) return; _rRd_ = _u32(_rRt_) >> _u32(_rRs_); }

void psxLUI() { if (!_Rt_) return; _rRt_ = psxRegs.code << 16; }

void psxMFHI() { if (!_Rd_) return; _rRd_ = _rHi_; }
void psxMFLO() { if (!_Rd_) return; _rRd_ = _rLo_; }

void psxMTHI() { _rHi_ = _rRs_; }
void psxMTLO() { _rLo_ = _rRs_; }

void psxBREAK() {
	psxRegs.pc -= 4;
	psxException(0x24, iopIsDelaySlot);
}

void psxSYSCALL() {
	psxRegs.pc -= 4;
	psxException(0x20, iopIsDelaySlot);

}

void psxRFE() {
	psxRegs.CP0.n.Status = (psxRegs.CP0.n.Status & 0xfffffff0) |
						  ((psxRegs.CP0.n.Status & 0x3c) >> 2);
}

#define _oB_ (_u32(_rRs_) + _Imm_)

void psxLB() {
	if (_Rt_) {
		_rRt_ = (s8 )iopMemRead8(_oB_);
	} else {
		iopMemRead8(_oB_);
	}
}

void psxLBU() {
	if (_Rt_) {
		_rRt_ = iopMemRead8(_oB_);
	} else {
		iopMemRead8(_oB_);
	}
}

void psxLH() {
	if (_Rt_) {
		_rRt_ = (s16)iopMemRead16(_oB_);
	} else {
		iopMemRead16(_oB_);
	}
}

void psxLHU() {
	if (_Rt_) {
		_rRt_ = iopMemRead16(_oB_);
	} else {
		iopMemRead16(_oB_);
	}
}

void psxLW() {
	if (_Rt_) {
		_rRt_ = iopMemRead32(_oB_);
	} else {
		iopMemRead32(_oB_);
	}
}

void psxLWL() {
	u32 shift = (_oB_ & 3) << 3;
	u32 mem = iopMemRead32(_oB_ & 0xfffffffc);

	if (!_Rt_) return;
	_rRt_ =	( _u32(_rRt_) & (0x00ffffff >> shift) ) |
				( mem << (24 - shift) );

}

void psxLWR() {
	u32 shift = (_oB_ & 3) << 3;
	u32 mem = iopMemRead32(_oB_ & 0xfffffffc);

	if (!_Rt_) return;
	_rRt_ =	( _u32(_rRt_) & (0xffffff00 << (24 - shift)) ) |
			( mem  >> shift );

}

void psxSB() { iopMemWrite8 (_oB_, _u8 (_rRt_)); }
void psxSH() { iopMemWrite16(_oB_, _u16(_rRt_)); }
void psxSW() { iopMemWrite32(_oB_, _u32(_rRt_)); }

void psxSWL() {
	u32 shift = (_oB_ & 3) << 3;
	u32 mem = iopMemRead32(_oB_ & 0xfffffffc);

	iopMemWrite32((_oB_ & 0xfffffffc),  ( ( _u32(_rRt_) >>  (24 - shift) ) ) |
			     (  mem & (0xffffff00 << shift) ));
}

void psxSWR() {
	u32 shift = (_oB_ & 3) << 3;
	u32 mem = iopMemRead32(_oB_ & 0xfffffffc);

	iopMemWrite32((_oB_ & 0xfffffffc), ( ( _u32(_rRt_) << shift ) |
			     (mem  & (0x00ffffff >> (24 - shift)) ) ) );
}

void psxMFC0() { if (!_Rt_) return; _rRt_ = (int)_rFs_; }
void psxCFC0() { if (!_Rt_) return; _rRt_ = (int)_rFs_; }

void psxMTC0() { _rFs_ = _u32(_rRt_); }
void psxCTC0() { _rFs_ = _u32(_rRt_); }

void psxCTC2() { _c2dRd_ = _u32(_rRt_); };
void psxNULL() {
Console.Warning("psx: Unimplemented op %x", psxRegs.code);
}

void psxSPECIAL() {
	psxSPC[_Funct_]();
}

void psxREGIMM() {
	psxREG[_Rt_]();
}

void psxCOP0() {
	psxCP0[_Rs_]();
}

void psxCOP2() {
	psxCP2[_Funct_]();
}

void psxBASIC() {
	psxCP2BSC[_Rs_]();
}

void(*psxBSC[64])() = {
	psxSPECIAL, psxREGIMM, psxJ   , psxJAL  , psxBEQ , psxBNE , psxBLEZ, psxBGTZ,
	psxADDI   , psxADDIU , psxSLTI, psxSLTIU, psxANDI, psxORI , psxXORI, psxLUI ,
	psxCOP0   , psxNULL  , psxCOP2, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL   , psxNULL  , psxNULL, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL,
	psxLB     , psxLH    , psxLWL , psxLW   , psxLBU , psxLHU , psxLWR , psxNULL,
	psxSB     , psxSH    , psxSWL , psxSW   , psxNULL, psxNULL, psxSWR , psxNULL,
	psxNULL   , psxNULL  , gteLWC2, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL   , psxNULL  , gteSWC2, psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL
};


void(*psxSPC[64])() = {
	psxSLL , psxNULL , psxSRL , psxSRA , psxSLLV   , psxNULL , psxSRLV, psxSRAV,
	psxJR  , psxJALR , psxNULL, psxNULL, psxSYSCALL, psxBREAK, psxNULL, psxNULL,
	psxMFHI, psxMTHI , psxMFLO, psxMTLO, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxMULT, psxMULTU, psxDIV , psxDIVU, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxADD , psxADDU , psxSUB , psxSUBU, psxAND    , psxOR   , psxXOR , psxNOR ,
	psxNULL, psxNULL , psxSLT , psxSLTU, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxNULL, psxNULL , psxNULL, psxNULL, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxNULL, psxNULL , psxNULL, psxNULL, psxNULL   , psxNULL , psxNULL, psxNULL
};

void(*psxREG[32])() = {
	psxBLTZ  , psxBGEZ  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL  , psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxBLTZAL, psxBGEZAL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL  , psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};

void(*psxCP0[32])() = {
	psxMFC0, psxNULL, psxCFC0, psxNULL, psxMTC0, psxNULL, psxCTC0, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxRFE , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};

void (*psxCP2[64])() = {
	psxBASIC, gteRTPS , psxNULL , psxNULL, psxNULL, psxNULL , gteNCLIP, psxNULL,
	psxNULL , psxNULL , psxNULL , psxNULL, gteOP  , psxNULL , psxNULL , psxNULL,
	gteDPCS , gteINTPL, gteMVMVA, gteNCDS, gteCDP , psxNULL , gteNCDT , psxNULL,
	psxNULL , psxNULL , psxNULL , gteNCCS, gteCC  , psxNULL , gteNCS  , psxNULL,
	gteNCT  , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL,
	gteSQR  , gteDCPL , gteDPCT , psxNULL, psxNULL, gteAVSZ3, gteAVSZ4, psxNULL,
	gteRTPT , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL,
	psxNULL , psxNULL , psxNULL , psxNULL, psxNULL, gteGPF  , gteGPL  , gteNCCT
};

void(*psxCP2BSC[32])() = {
	gteMFC2, psxNULL, gteCFC2, psxNULL, gteMTC2, psxNULL, gteCTC2, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};
