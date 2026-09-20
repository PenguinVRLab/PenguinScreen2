// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Threading.h"
#include "common/Assertions.h"
#include "common/Pcsx2Defs.h"

static const uint iREGCNT_XMM = 16;
static const uint iREGCNT_GPR = 16;

enum XMMSSEType
{
	XMMT_INT = 0,
	XMMT_FPS = 1,
};

extern thread_local u8* x86Ptr;
extern thread_local XMMSSEType g_xmmtypes[iREGCNT_XMM];

namespace x86Emitter
{
#ifdef _WIN32
	static constexpr int SHADOW_STACK_SIZE = 32;
#else
	static constexpr int SHADOW_STACK_SIZE = 0;
#endif

	extern bool use_avx;

	extern void xWrite8(u8 val);
	extern void xWrite16(u16 val);
	extern void xWrite32(u32 val);
	extern void xWrite64(u64 val);

	extern const char* xGetRegName(int regid, int operandSize);

	template <typename T>
	static __fi bool is_s8(T imm)
	{
		return (s8)imm == (std::make_signed_t<T>)imm;
	}

	template <typename T>
	void xWrite(T val);

#ifdef PCSX2_DEVBUILD
#define __emitinline
#else
#define __emitinline __fi
#endif

	enum ModRm_ModField
	{
		Mod_NoDisp = 0,
		Mod_Disp8,
		Mod_Disp32,
		Mod_Direct,
	};

	enum JccComparisonType
	{
		Jcc_Unknown = -2,
		Jcc_Unconditional = -1,
		Jcc_Overflow = 0x0,
		Jcc_NotOverflow = 0x1,
		Jcc_Below = 0x2,
		Jcc_Carry = 0x2,
		Jcc_AboveOrEqual = 0x3,
		Jcc_NotCarry = 0x3,
		Jcc_Zero = 0x4,
		Jcc_Equal = 0x4,
		Jcc_NotZero = 0x5,
		Jcc_NotEqual = 0x5,
		Jcc_BelowOrEqual = 0x6,
		Jcc_Above = 0x7,
		Jcc_Signed = 0x8,
		Jcc_Unsigned = 0x9,
		Jcc_ParityEven = 0xa,
		Jcc_ParityOdd = 0xb,
		Jcc_Less = 0xc,
		Jcc_GreaterOrEqual = 0xd,
		Jcc_LessOrEqual = 0xe,
		Jcc_Greater = 0xf,
	};

	enum SSE2_ComparisonType
	{
		SSE2_Equal = 0,
		SSE2_Less,
		SSE2_LessOrEqual,
		SSE2_Unordered,
		SSE2_NotEqual,
		SSE2_NotLess,
		SSE2_NotLessOrEqual,
		SSE2_Ordered
	};

	static const int ModRm_UseSib = 4;
	static const int ModRm_UseDisp32 = 5;
	static const int Sib_EIZ = 4;
	static const int Sib_UseDisp32 = 5;

	extern void xSetPtr(void* ptr);
	extern void xSetTextPtr(void* ptr);
	extern void xAlignPtr(uint bytes);
	extern void xAdvancePtr(uint bytes);
	extern void xAlignCallTarget();

	extern u8* xGetPtr();
	extern u8* xGetTextPtr();
	extern u8* xGetAlignedCallTarget();

	extern JccComparisonType xInvertCond(JccComparisonType src);

	class xAddressVoid;

	class OperandSizedObject
	{
	protected:
		uint _operandSize = 0;
		OperandSizedObject() = default;
		OperandSizedObject(uint operandSize)
			: _operandSize(operandSize)
		{
		}

	public:
		uint GetOperandSize() const
		{
			pxAssertMsg(_operandSize != 0, "Attempted to use operand size of uninitialized or void object");
			return _operandSize;
		}

		bool Is8BitOp() const { return GetOperandSize() == 1; }
		u8 GetPrefix16() const { return GetOperandSize() == 2 ? 0x66 : 0; }
		void prefix16() const
		{
			if (GetOperandSize() == 2)
				xWrite8(0x66);
		}

		int GetImmSize() const
		{
			switch (GetOperandSize())
			{
				case 1:
					return 1;
				case 2:
					return 2;
				case 4:
					return 4;
				case 8:
					return 4;
					jNO_DEFAULT
			}
			return 0;
		}

		void xWriteImm(int imm) const
		{
			switch (GetImmSize())
			{
				case 1:
					xWrite8(imm);
					break;
				case 2:
					xWrite16(imm);
					break;
				case 4:
					xWrite32(imm);
					break;

					jNO_DEFAULT
			}
		}
	};

	static const int xRegId_Empty = -1;

	static const int xRegId_Invalid = -2;

	class xRegisterBase : public OperandSizedObject
	{
	protected:
		xRegisterBase(uint operandSize, int regId)
			: OperandSizedObject(operandSize)
			, Id(regId)
		{
			pxAssert((Id >= xRegId_Empty) && (Id < 16));
		}

	public:
		int Id;

		xRegisterBase()
			: OperandSizedObject(0)
			, Id(xRegId_Invalid)
		{
		}

		bool IsEmpty() const { return Id < 0; }
		bool IsInvalid() const { return Id == xRegId_Invalid; }
		bool IsExtended() const { return (Id >= 0 && (Id & 0x0F) > 7); }
		bool IsExtended8Bit() const { return (Is8BitOp() && Id >= 0x10); }
		bool IsMem() const { return false; }
		bool IsReg() const { return true; }

		bool IsAccumulator() const { return Id == 0; }

		bool IsSIMD() const { return GetOperandSize() == 16; }

		bool IsWide() const
		{
			return GetOperandSize() == 8;
		}
		bool IsWideSIMD() const { return GetOperandSize() == 32; }

		const char* GetName();
		int GetId() const { return Id; }

		static inline bool IsCallerSaved(uint id);
	};

	class xRegisterInt : public xRegisterBase
	{
		typedef xRegisterBase _parent;

	protected:
		explicit xRegisterInt(uint operandSize, int regId)
			: _parent(operandSize, regId)
		{
		}

	public:
		xRegisterInt() = default;

		int isIDSameInAllSizes() const
		{
			return Id < 4 || Id >= 8;
		}

		bool canMapIDTo(int otherSize) const
		{
			if ((otherSize == 1) == (GetOperandSize() == 1))
				return true;
			return isIDSameInAllSizes();
		}

		xRegisterInt GetNonWide() const
		{
			return GetOperandSize() == 8 ? xRegisterInt(4, Id) : *this;
		}

		xRegisterInt MatchSizeTo(xRegisterInt other) const;

		bool operator==(const xRegisterInt& src) const { return Id == src.Id && (GetOperandSize() == src.GetOperandSize()); }
		bool operator!=(const xRegisterInt& src) const { return !operator==(src); }
	};

	class xRegister8 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

	public:
		xRegister8() = default;
		explicit xRegister8(int regId)
			: _parent(1, regId)
		{
		}
		explicit xRegister8(const xRegisterInt& other)
			: _parent(1, other.Id)
		{
			if (!other.canMapIDTo(1))
				Id |= 0x10;
		}
		xRegister8(int regId, bool ext8bit)
			: _parent(1, regId)
		{
			if (ext8bit)
				Id |= 0x10;
		}

		bool operator==(const xRegister8& src) const { return Id == src.Id; }
		bool operator!=(const xRegister8& src) const { return Id != src.Id; }
	};

	class xRegister16 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

	public:
		xRegister16() = default;
		explicit xRegister16(int regId)
			: _parent(2, regId)
		{
		}
		explicit xRegister16(const xRegisterInt& other)
			: _parent(2, other.Id)
		{
			pxAssertMsg(other.canMapIDTo(2), "Mapping h registers to higher registers can produce unexpected values");
		}

		bool operator==(const xRegister16& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegister16& src) const { return this->Id != src.Id; }
	};

	class xRegister32 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

	public:
		xRegister32() = default;
		explicit xRegister32(int regId)
			: _parent(4, regId)
		{
		}
		explicit xRegister32(const xRegisterInt& other)
			: _parent(4, other.Id)
		{
			pxAssertMsg(other.canMapIDTo(4), "Mapping h registers to higher registers can produce unexpected values");
		}

		static const inline xRegister32& GetInstance(uint id);

		bool operator==(const xRegister32& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegister32& src) const { return this->Id != src.Id; }
	};

	class xRegister64 : public xRegisterInt
	{
		typedef xRegisterInt _parent;

	public:
		xRegister64() = default;
		explicit xRegister64(int regId)
			: _parent(8, regId)
		{
		}
		explicit xRegister64(const xRegisterInt& other)
			: _parent(8, other.Id)
		{
			pxAssertMsg(other.canMapIDTo(8), "Mapping h registers to higher registers can produce unexpected values");
		}

		static const inline xRegister64& GetInstance(uint id);

		bool operator==(const xRegister64& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegister64& src) const { return this->Id != src.Id; }
	};

	struct xRegisterYMMTag {};

	class xRegisterSSE : public xRegisterBase
	{
		typedef xRegisterBase _parent;

	public:
		xRegisterSSE() = default;
		explicit xRegisterSSE(int regId)
			: _parent(16, regId)
		{
		}
		xRegisterSSE(int regId, xRegisterYMMTag)
			: _parent(32, regId)
		{
		}

		bool operator==(const xRegisterSSE& src) const { return this->Id == src.Id; }
		bool operator!=(const xRegisterSSE& src) const { return this->Id != src.Id; }

		static const inline xRegisterSSE& GetInstance(uint id);
		static const inline xRegisterSSE& GetYMMInstance(uint id);

		static const inline xRegisterSSE& GetArgRegister(uint arg_number, uint sse_number, bool ymm = false);

		static inline bool IsCallerSaved(uint id);
	};

	class xRegisterCL : public xRegister8
	{
	public:
		xRegisterCL()
			: xRegister8(1)
		{
		}
	};

#define xRegisterLong xRegister64
	static const int wordsize = sizeof(sptr);

	class xAddressReg : public xRegisterLong
	{
	public:
		xAddressReg() = default;
		explicit xAddressReg(xRegisterInt other)
			: xRegisterLong(other)
		{
		}
		explicit xAddressReg(int regId)
			: xRegisterLong(regId)
		{
		}

		bool IsStackPointer() const { return Id == 4; }

		static const inline xAddressReg& GetArgRegister(uint arg_number, uint gpr_number);

		xAddressVoid operator+(const xAddressReg& right) const;
		xAddressVoid operator+(sptr right) const;
		xAddressVoid operator+(const void* right) const;
		xAddressVoid operator-(sptr right) const;
		xAddressVoid operator-(const void* right) const;
		xAddressVoid operator*(int factor) const;
		xAddressVoid operator<<(u32 shift) const;
	};

	struct xRegisterEmpty
	{
		operator xRegister8() const
		{
			return xRegister8(xRegId_Empty);
		}

		operator xRegister16() const
		{
			return xRegister16(xRegId_Empty);
		}

		operator xRegister32() const
		{
			return xRegister32(xRegId_Empty);
		}

		operator xRegisterSSE() const
		{
			return xRegisterSSE(xRegId_Empty);
		}

		operator xAddressReg() const
		{
			return xAddressReg(xRegId_Empty);
		}
	};

	class xRegister16or32or64
	{
	protected:
		const xRegisterInt& m_convtype;

	public:
		xRegister16or32or64(const xRegister64& src)
			: m_convtype(src)
		{
		}
		xRegister16or32or64(const xRegister32& src)
			: m_convtype(src)
		{
		}
		xRegister16or32or64(const xRegister16& src)
			: m_convtype(src)
		{
		}

		operator const xRegisterBase&() const { return m_convtype; }

		const xRegisterInt* operator->() const
		{
			return &m_convtype;
		}
	};

	class xRegister32or64
	{
	protected:
		const xRegisterInt& m_convtype;

	public:
		xRegister32or64(const xRegister64& src)
			: m_convtype(src)
		{
		}
		xRegister32or64(const xRegister32& src)
			: m_convtype(src)
		{
		}

		operator const xRegisterBase&() const { return m_convtype; }

		const xRegisterInt* operator->() const
		{
			return &m_convtype;
		}
	};

	extern const xRegisterEmpty xEmptyReg;

	// clang-format off
	extern const xRegisterSSE
    xmm0, xmm1, xmm2, xmm3,
    xmm4, xmm5, xmm6, xmm7,
    xmm8, xmm9, xmm10, xmm11,
    xmm12, xmm13, xmm14, xmm15;

	extern const xRegisterSSE
	  ymm0, ymm1, ymm2, ymm3,
	  ymm4, ymm5, ymm6, ymm7,
	  ymm8, ymm9, ymm10, ymm11,
	  ymm12, ymm13, ymm14, ymm15;

extern const xAddressReg
    rax, rbx, rcx, rdx,
    rsi, rdi, rbp, rsp,
    r8, r9, r10, r11,
    r12, r13, r14, r15;

extern const xRegister32
     eax,  ebx,  ecx,  edx,
     esi,  edi,  ebp,  esp,
     r8d,  r9d, r10d, r11d,
    r12d, r13d, r14d, r15d;

extern const xRegister16
    ax, bx, cx, dx,
    si, di, bp, sp;

extern const xRegister8
    al, dl, bl,
    ah, ch, dh, bh,
    spl, bpl, sil, dil,
    r8b, r9b, r10b, r11b,
    r12b, r13b, r14b, r15b;

extern const xAddressReg
    arg1reg, arg2reg,
    arg3reg, arg4reg,
    calleeSavedReg1,
    calleeSavedReg2;


extern const xRegister32
    arg1regd, arg2regd,
    calleeSavedReg1d,
    calleeSavedReg2d;

static constexpr const xAddressReg& RTEXTPTR = rbx;

	// clang-format on

	extern const xRegisterCL cl;

	bool xRegisterBase::IsCallerSaved(uint id)
	{
#ifdef _WIN32
		return (id <= 2 || (id >= 8 && id <= 11));
#else
		return (id <= 2 || id == 6 || id == 7 || (id >= 8 && id <= 11));
#endif
	}

	const xRegister32& xRegister32::GetInstance(uint id)
	{
		static const xRegister32* const m_tbl_x86Regs[] =
		{
				&eax, &ecx, &edx, &ebx,
				&esp, &ebp, &esi, &edi,
				&r8d, &r9d, &r10d, &r11d,
				&r12d, &r13d, &r14d, &r15d,
		};

		pxAssert(id < iREGCNT_GPR);
		return *m_tbl_x86Regs[id];
	}

	const xRegister64& xRegister64::GetInstance(uint id)
	{
		static const xRegister64* const m_tbl_x86Regs[] =
		{
				&rax, &rcx, &rdx, &rbx,
				&rsp, &rbp, &rsi, &rdi,
				&r8, &r9, &r10, &r11,
				&r12, &r13, &r14, &r15
		};

		pxAssert(id < iREGCNT_GPR);
		return *m_tbl_x86Regs[id];
	}

	bool xRegisterSSE::IsCallerSaved(uint id)
	{
#ifdef _WIN32
		return (id < 6);
#else
		return true;
#endif
	}

	const xRegisterSSE& xRegisterSSE::GetInstance(uint id)
	{
		static const xRegisterSSE* const m_tbl_xmmRegs[] =
			{
				&xmm0, &xmm1, &xmm2, &xmm3,
				&xmm4, &xmm5, &xmm6, &xmm7,
				&xmm8, &xmm9, &xmm10, &xmm11,
				&xmm12, &xmm13, &xmm14, &xmm15};

		pxAssert(id < iREGCNT_XMM);
		return *m_tbl_xmmRegs[id];
	}

	const xRegisterSSE& xRegisterSSE::GetYMMInstance(uint id)
	{
		static const xRegisterSSE* const m_tbl_ymmRegs[] =
			{
				&ymm0, &ymm1, &ymm2, &ymm3,
				&ymm4, &ymm5, &ymm6, &ymm7,
				&ymm8, &ymm9, &ymm10, &ymm11,
				&ymm12, &ymm13, &ymm14, &ymm15};

		pxAssert(id < iREGCNT_XMM);
		return *m_tbl_ymmRegs[id];
	}

	const xRegisterSSE& xRegisterSSE::GetArgRegister(uint arg_number, uint sse_number, bool ymm)
	{
#ifdef _WIN32
		return ymm ? GetYMMInstance(arg_number) : GetInstance(arg_number);
#else
		return ymm ? GetYMMInstance(sse_number) : GetInstance(sse_number);
#endif
	}

	const xAddressReg& xAddressReg::GetArgRegister(uint arg_number, uint gpr_number)
	{
#ifdef _WIN32
		static constexpr const xAddressReg* regs[] = {&rcx, &rdx, &r8, &r9};
		pxAssert(arg_number < std::size(regs));
		return *regs[arg_number];
#else
		static constexpr const xAddressReg* regs[] = {&rdi, &rsi, &rdx, &rcx};
		pxAssert(gpr_number < std::size(regs));
		return *regs[gpr_number];
#endif
	}

	class xAddressVoid
	{
	public:
		xAddressReg Base;
		xAddressReg Index;
		int Factor;
		sptr Displacement;

	public:
		xAddressVoid(const xAddressReg& base, const xAddressReg& index, int factor = 1, sptr displacement = 0);

		xAddressVoid(const xAddressReg& index, sptr displacement = 0);
		explicit xAddressVoid(const void* displacement);
		explicit xAddressVoid(sptr displacement = 0);

	public:
		bool IsByteSizeDisp() const { return is_s8(Displacement); }

		xAddressVoid& Add(sptr imm)
		{
			Displacement += imm;
			return *this;
		}

		xAddressVoid& Add(const xAddressReg& src);
		xAddressVoid& Add(const xAddressVoid& src);

		__fi xAddressVoid operator+(const xAddressReg& right) const { return xAddressVoid(*this).Add(right); }
		__fi xAddressVoid operator+(const xAddressVoid& right) const { return xAddressVoid(*this).Add(right); }
		__fi xAddressVoid operator+(sptr imm) const { return xAddressVoid(*this).Add(imm); }
		__fi xAddressVoid operator-(sptr imm) const { return xAddressVoid(*this).Add(-imm); }
		__fi xAddressVoid operator+(const void* addr) const { return xAddressVoid(*this).Add((uptr)addr); }

		__fi void operator+=(const xAddressReg& right) { Add(right); }
		__fi void operator+=(sptr imm) { Add(imm); }
		__fi void operator-=(sptr imm) { Add(-imm); }
	};

	static __fi xAddressVoid operator+(const void* addr, const xAddressVoid& right)
	{
		return right + addr;
	}

	static __fi xAddressVoid operator+(sptr addr, const xAddressVoid& right)
	{
		return right + addr;
	}

	template <typename xRegType>
	class xImmReg
	{
		xRegType m_reg;
		int m_imm;

	public:
		xImmReg()
			: m_reg()
		{
			m_imm = 0;
		}

		xImmReg(int imm, const xRegType& reg = xEmptyReg)
		{
			m_reg = reg;
			m_imm = imm;
		}

		const xRegType& GetReg() const { return m_reg; }
		int GetImm() const { return m_imm; }
		bool IsReg() const { return !m_reg.IsEmpty(); }
	};

	class xIndirectVoid : public OperandSizedObject
	{
	public:
		xAddressReg Base;
		xAddressReg Index;
		uint Scale;
		sptr Displacement;

	public:
		explicit xIndirectVoid(sptr disp);
		explicit xIndirectVoid(const xAddressVoid& src);
		xIndirectVoid(xAddressReg base, xAddressReg index, int scale = 0, sptr displacement = 0);
		xIndirectVoid& Add(sptr imm);

		bool IsByteSizeDisp() const { return is_s8(Displacement); }
		bool IsMem() const { return true; }
		bool IsReg() const { return false; }
		bool IsExtended() const { return false; }
		bool IsWide() const { return _operandSize == 8; }

		operator xAddressVoid()
		{
			return xAddressVoid(Base, Index, Scale, Displacement);
		}

		__fi xIndirectVoid operator+(const sptr imm) const { return xIndirectVoid(*this).Add(imm); }
		__fi xIndirectVoid operator-(const sptr imm) const { return xIndirectVoid(*this).Add(-imm); }

	protected:
		void Reduce();
	};

	template <typename OperandType>
	class xIndirect : public xIndirectVoid
	{
		typedef xIndirectVoid _parent;

	public:
		explicit xIndirect(sptr disp)
			: _parent(disp)
		{
			_operandSize = sizeof(OperandType);
		}
		xIndirect(xAddressReg base, xAddressReg index, int scale = 0, sptr displacement = 0)
			: _parent(base, index, scale, displacement)
		{
			_operandSize = sizeof(OperandType);
		}
		explicit xIndirect(const xIndirectVoid& other)
			: _parent(other)
		{
		}

		xIndirect<OperandType>& Add(sptr imm)
		{
			Displacement += imm;
			return *this;
		}

		__fi xIndirect<OperandType> operator+(const sptr imm) const { return xIndirect(*this).Add(imm); }
		__fi xIndirect<OperandType> operator-(const sptr imm) const { return xIndirect(*this).Add(-imm); }

		bool operator==(const xIndirect<OperandType>& src) const
		{
			return (Base == src.Base) && (Index == src.Index) &&
				   (Scale == src.Scale) && (Displacement == src.Displacement);
		}

		bool operator!=(const xIndirect<OperandType>& src) const
		{
			return !operator==(src);
		}

	protected:
		void Reduce();
	};

	typedef xIndirect<u128> xIndirect128;
	typedef xIndirect<u64> xIndirect64;
	typedef xIndirect<u32> xIndirect32;
	typedef xIndirect<u16> xIndirect16;
	typedef xIndirect<u8> xIndirect8;
	typedef xIndirect<u64> xIndirectNative;

	class xIndirect64orLess : public xIndirectVoid
	{
		typedef xIndirectVoid _parent;

	public:
		xIndirect64orLess(const xIndirect8& src)
			: _parent(src)
		{
		}
		xIndirect64orLess(const xIndirect16& src)
			: _parent(src)
		{
		}
		xIndirect64orLess(const xIndirect32& src)
			: _parent(src)
		{
		}
		xIndirect64orLess(const xIndirect64& src)
			: _parent(src)
		{
		}
	};

	template <typename xModSibType>
	class xAddressIndexer
	{
	public:
		const xModSibType& operator[](const xModSibType& src) const { return src; }

		xModSibType operator[](const xAddressReg& src) const
		{
			return xModSibType(src, xEmptyReg);
		}

		xModSibType operator[](const xAddressVoid& src) const
		{
			return xModSibType(src.Base, src.Index, src.Factor, src.Displacement);
		}

		xModSibType operator[](const void* src) const
		{
			return xModSibType((uptr)src);
		}
	};

	extern const xAddressIndexer<xIndirectVoid> ptr;
	extern const xAddressIndexer<xIndirectNative> ptrNative;
	extern const xAddressIndexer<xIndirect128> ptr128;
	extern const xAddressIndexer<xIndirect64> ptr64;
	extern const xAddressIndexer<xIndirect32> ptr32;
	extern const xAddressIndexer<xIndirect16> ptr16;
	extern const xAddressIndexer<xIndirect8> ptr8;

	class xForwardJumpBase
	{
	public:
		s8* BasePtr;

	public:
		xForwardJumpBase(uint opsize, JccComparisonType cctype);

	protected:
		void _setTarget(uint opsize) const;
	};

	template <typename OperandType>
	class xForwardJump : public xForwardJumpBase
	{
	public:
		static const uint OperandSize = sizeof(OperandType);

		xForwardJump(JccComparisonType cctype = Jcc_Unconditional)
			: xForwardJumpBase(OperandSize, cctype)
		{
		}

		void SetTarget() const
		{
			_setTarget(OperandSize);
		}
	};

	static __fi xAddressVoid operator+(const void* addr, const xAddressReg& reg)
	{
		return reg + (sptr)addr;
	}

	static __fi xAddressVoid operator+(sptr addr, const xAddressReg& reg)
	{
		return reg + (sptr)addr;
	}
}

#include "implement/helpers.h"

#include "implement/simd_helpers.h"
#include "implement/simd_moremovs.h"
#include "implement/simd_arithmetic.h"
#include "implement/simd_comparisons.h"
#include "implement/simd_shufflepack.h"

#include "implement/group1.h"
#include "implement/group2.h"
#include "implement/group3.h"
#include "implement/movs.h"
#include "implement/dwshift.h"
#include "implement/incdec.h"
#include "implement/test.h"
#include "implement/jmpcall.h"

#include "implement/bmi.h"
#include "implement/avx.h"
