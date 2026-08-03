// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include <cmath>

#define PosInfinity 0x7f800000
#define NegInfinity 0xff800000
#define posFmax 0x7F7FFFFF
#define negFmax 0xFF7FFFFF


#define _Ft_         ( ( cpuRegs.code >> 16 ) & 0x1F )
#define _Fs_         ( ( cpuRegs.code >> 11 ) & 0x1F )
#define _Fd_         ( ( cpuRegs.code >>  6 ) & 0x1F )

#define _FtValf_     fpuRegs.fpr[ _Ft_ ].f
#define _FsValf_     fpuRegs.fpr[ _Fs_ ].f
#define _FdValf_     fpuRegs.fpr[ _Fd_ ].f
#define _FAValf_     fpuRegs.ACC.f

#define _FtValUl_    fpuRegs.fpr[ _Ft_ ].UL
#define _FsValUl_    fpuRegs.fpr[ _Fs_ ].UL
#define _FdValUl_    fpuRegs.fpr[ _Fd_ ].UL
#define _FAValUl_    fpuRegs.ACC.UL

#define _FtValSl_    fpuRegs.fpr[ _Ft_ ].SL
#define _FsValSl_    fpuRegs.fpr[ _Fs_ ].SL
#define _FdValSl_    fpuRegs.fpr[ _Fd_ ].SL
#define _FAValSl_    fpuRegs.ACC.SL

#define _ContVal_    fpuRegs.fprc[ 31 ]

#define FPUflagC	0X00800000
#define FPUflagI	0X00020000
#define FPUflagD	0X00010000
#define FPUflagO	0X00008000
#define FPUflagU	0X00004000
#define FPUflagSI	0X00000040
#define FPUflagSD	0X00000020
#define FPUflagSO	0X00000010
#define FPUflagSU	0X00000008

bool checkOverflow(u32& xReg, u32 cFlagsToSet)
{
	if ((xReg & ~0x80000000) == PosInfinity) {
		xReg = (xReg & 0x80000000) | posFmax;
		_ContVal_ |= (cFlagsToSet);
		return true;
	}
	else if (cFlagsToSet & FPUflagO)
		_ContVal_ &= ~FPUflagO;

	return false;
}

bool checkUnderflow(u32& xReg, u32 cFlagsToSet) {
	if ( ( (xReg & 0x7F800000) == 0 ) && ( (xReg & 0x007FFFFF) != 0 ) ) {
		xReg &= 0x80000000;
		_ContVal_ |= (cFlagsToSet);
		return true;
	}
	else if (cFlagsToSet & FPUflagU)
		_ContVal_ &= ~FPUflagU;

	return false;
}

__fi u32 fp_max(u32 a, u32 b)
{
	return ((s32)a < 0 && (s32)b < 0) ? std::min<s32>(a, b) : std::max<s32>(a, b);
}

__fi u32 fp_min(u32 a, u32 b)
{
	return ((s32)a < 0 && (s32)b < 0) ? std::max<s32>(a, b) : std::min<s32>(a, b);
}

bool checkDivideByZero(u32& xReg, u32 yDivisorReg, u32 zDividendReg, u32 cFlagsToSet1, u32 cFlagsToSet2) {

	if ( (yDivisorReg & 0x7F800000) == 0 ) {
		_ContVal_ |= ( (zDividendReg & 0x7F800000) == 0 ) ? cFlagsToSet2 : cFlagsToSet1;
		xReg = ( (yDivisorReg ^ zDividendReg) & 0x80000000 ) | posFmax;
		return true;
	}

	return false;
}

#define clearFPUFlags(cFlags) {  \
	_ContVal_ &= ~( cFlags ) ;  \
}

#ifdef comparePrecision
	#define C_cond_S(cond) {  \
		FPRreg tempA, tempB;  \
		tempA.UL = _FsValUl_ & comparePrecision;  \
		tempB.UL = _FtValUl_ & comparePrecision;  \
		_ContVal_ = ( ( tempA.f ) cond ( tempB.f ) ) ?  \
					( _ContVal_ | FPUflagC ) :  \
					( _ContVal_ & ~FPUflagC );  \
	}
#else
	#define C_cond_S(cond) {  \
	   _ContVal_ = ( fpuDouble(_FsValUl_) cond fpuDouble(_FtValUl_) ) ?  \
				   ( _ContVal_ | FPUflagC ) :  \
				   ( _ContVal_ & ~FPUflagC );  \
	}
#endif

#define BC1(cond)                               \
   if ( ( _ContVal_ & FPUflagC ) cond 0 ) {   \
      intDoBranch( _BranchTarget_ );            \
   }

#define BC1L(cond)                              \
   if ( ( _ContVal_ & FPUflagC ) cond 0 ) {   \
      intDoBranch( _BranchTarget_ );            \
   } else cpuRegs.pc += 4;

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {
namespace COP1 {

float fpuDouble(u32 f)
{
	switch(f & 0x7f800000){
		case 0x0:
			f &= 0x80000000;
			return *(float*)&f;
			break;
		case 0x7f800000:
			f = (f & 0x80000000)|0x7f7fffff;
			return *(float*)&f;
			break;
		default:
			return *(float*)&f;
			break;
	}
}

void ABS_S() {
	_FdValUl_ = _FsValUl_ & 0x7fffffff;
	clearFPUFlags( FPUflagO | FPUflagU );
}

void ADD_S() {
	_FdValf_  = fpuDouble( _FsValUl_ ) + fpuDouble( _FtValUl_ );
	if (checkOverflow( _FdValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FdValUl_, FPUflagU | FPUflagSU);
}

void ADDA_S() {
	_FAValf_  = fpuDouble( _FsValUl_ ) + fpuDouble( _FtValUl_ );
	if (checkOverflow( _FAValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FAValUl_, FPUflagU | FPUflagSU);
}

void BC1F() {
	BC1(==);
}

void BC1FL() {
	BC1L(==);
}

void BC1T() {
	BC1(!=);
}

void BC1TL() {
	BC1L(!=);
}

void C_EQ() {
	C_cond_S(==);
}

void C_F() {
	clearFPUFlags( FPUflagC );
}

void C_LE() {
	C_cond_S(<=);
}

void C_LT() {
	C_cond_S(<);
}

void CFC1() {
	if (!_Rt_) return;

	if (_Fs_ == 31)
		cpuRegs.GPR.r[_Rt_].SD[0] = (s32)fpuRegs.fprc[31];
	else if (_Fs_ == 0)
		cpuRegs.GPR.r[_Rt_].SD[0] = 0x2E00;
	else
		cpuRegs.GPR.r[_Rt_].SD[0] = 0;
}

void CTC1() {
	if ( _Fs_ != 31 ) return;
	fpuRegs.fprc[_Fs_] = cpuRegs.GPR.r[_Rt_].UL[0];
}

void CVT_S() {
	_FdValf_ = (float)_FsValSl_;
}

void CVT_W() {
	if ( ( _FsValUl_ & 0x7F800000 ) <= 0x4E800000 ) { _FdValSl_ = (s32)_FsValf_; }
	else if ( ( _FsValUl_ & 0x80000000 ) == 0 ) { _FdValUl_ = 0x7fffffff; }
	else { _FdValUl_ = 0x80000000; }
}

void DIV_S() {
	if (checkDivideByZero( _FdValUl_, _FtValUl_, _FsValUl_, FPUflagD | FPUflagSD, FPUflagI | FPUflagSI)) return;
	_FdValf_ = fpuDouble( _FsValUl_ ) / fpuDouble( _FtValUl_ );
	if (checkOverflow( _FdValUl_, 0)) return;
	checkUnderflow( _FdValUl_, 0);
}

void MADD_S() {
	FPRreg temp;
	temp.f = fpuDouble( _FsValUl_ ) * fpuDouble( _FtValUl_ );
	_FdValf_  = fpuDouble( _FAValUl_ ) + fpuDouble( temp.UL );
	if (checkOverflow( _FdValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FdValUl_, FPUflagU | FPUflagSU);
}

void MADDA_S() {
	_FAValf_ += fpuDouble( _FsValUl_ ) * fpuDouble( _FtValUl_ );
	if (checkOverflow( _FAValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FAValUl_, FPUflagU | FPUflagSU);
}

void MAX_S() {
	_FdValUl_  = fp_max( _FsValUl_, _FtValUl_ );
	clearFPUFlags( FPUflagO | FPUflagU );
}

void MFC1() {
	if ( !_Rt_ ) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = _FsValSl_;
}

void MIN_S() {
	_FdValUl_ = fp_min(_FsValUl_, _FtValUl_);
	clearFPUFlags( FPUflagO | FPUflagU );
}

void MOV_S() {
	_FdValUl_ = _FsValUl_;
}

void MSUB_S() {
	FPRreg temp;
	temp.f = fpuDouble( _FsValUl_ ) * fpuDouble( _FtValUl_ );
	_FdValf_  = fpuDouble( _FAValUl_ ) - fpuDouble( temp.UL );
	if (checkOverflow( _FdValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FdValUl_, FPUflagU | FPUflagSU);
}

void MSUBA_S() {
	_FAValf_ -= fpuDouble( _FsValUl_ ) * fpuDouble( _FtValUl_ );
	if (checkOverflow( _FAValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FAValUl_, FPUflagU | FPUflagSU);
}

void MTC1() {
	_FsValUl_ = cpuRegs.GPR.r[_Rt_].UL[0];
}

void MUL_S() {
	_FdValf_  = fpuDouble( _FsValUl_ ) * fpuDouble( _FtValUl_ );
	if (checkOverflow( _FdValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FdValUl_, FPUflagU | FPUflagSU);
}

void MULA_S() {
	_FAValf_  = fpuDouble( _FsValUl_ ) * fpuDouble( _FtValUl_ );
	if (checkOverflow( _FAValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FAValUl_, FPUflagU | FPUflagSU);
}

void NEG_S() {
	_FdValUl_  = (_FsValUl_ ^ 0x80000000);
	clearFPUFlags( FPUflagO | FPUflagU );
}

void RSQRT_S() {
	FPRreg temp;
	clearFPUFlags(FPUflagD | FPUflagI);

	if ( ( _FtValUl_ & 0x7F800000 ) == 0 ) {
		_ContVal_ |= FPUflagD | FPUflagSD;
		_FdValUl_ = ( _FtValUl_ & 0x80000000 ) | posFmax;
		return;
	}
	else if ( _FtValUl_ & 0x80000000 ) {
		_ContVal_ |= FPUflagI | FPUflagSI;
		temp.f = sqrt( fabs( fpuDouble( _FtValUl_ ) ) );
		_FdValf_ = fpuDouble( _FsValUl_ ) / fpuDouble( temp.UL );
	}
	else { _FdValf_ = fpuDouble( _FsValUl_ ) / sqrt( fpuDouble( _FtValUl_ ) ); }

	if (checkOverflow( _FdValUl_, 0)) return;
	checkUnderflow( _FdValUl_, 0);
}

void SQRT_S() {
	clearFPUFlags(FPUflagI | FPUflagD);

	if ( ( _FtValUl_ & 0x7F800000 ) == 0 )
		_FdValUl_ = _FtValUl_ & 0x80000000;
	else if ( _FtValUl_ & 0x80000000 ) {
		_ContVal_ |= FPUflagI | FPUflagSI;
		_FdValf_ = sqrt( fabs( fpuDouble( _FtValUl_ ) ) );
	} else
		_FdValf_ = sqrt( fpuDouble( _FtValUl_ ) );
}

void SUB_S() {
	_FdValf_  = fpuDouble( _FsValUl_ ) - fpuDouble( _FtValUl_ );
	if (checkOverflow( _FdValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FdValUl_, FPUflagU | FPUflagSU);
}

void SUBA_S() {
	_FAValf_  = fpuDouble( _FsValUl_ ) - fpuDouble( _FtValUl_ );
	if (checkOverflow( _FAValUl_, FPUflagO | FPUflagSO)) return;
	checkUnderflow( _FAValUl_, FPUflagU | FPUflagSU);
}

}

void LWC1() {
	u32 addr;
	addr = cpuRegs.GPR.r[_Rs_].UL[0] + (s16)(cpuRegs.code & 0xffff);
	if (addr & 0x00000003) { Console.Error( "FPU (LWC1 Opcode): Invalid Unaligned Memory Address" ); return; }
	fpuRegs.fpr[_Rt_].UL = memRead32(addr);
}

void SWC1() {
	u32 addr;
	addr = cpuRegs.GPR.r[_Rs_].UL[0] + (s16)(cpuRegs.code & 0xffff);
	if (addr & 0x00000003) { Console.Error( "FPU (SWC1 Opcode): Invalid Unaligned Memory Address" ); return; }
	memWrite32(addr, fpuRegs.fpr[_Rt_].UL);
}

} } }
