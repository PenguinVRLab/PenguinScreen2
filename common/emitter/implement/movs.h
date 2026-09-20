// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct xImpl_Mov
	{
		xImpl_Mov() {}

		void operator()(const xRegisterInt& to, const xRegisterInt& from) const;
		void operator()(const xIndirectVoid& dest, const xRegisterInt& from) const;
		void operator()(const xRegisterInt& to, const xIndirectVoid& src) const;
		void operator()(const xIndirect64orLess& dest, sptr imm) const;
		void operator()(const xRegisterInt& to, sptr imm, bool preserve_flags = false) const;

#if 0
	template< typename T > __noinline void operator()( const ModSibBase& to, const xImmReg<T>& immOrReg ) const
	{
		_DoI_helpermess( *this, to, immOrReg );
	}

	template< typename T > __noinline void operator()( const xDirectOrIndirect<T>& to, const xImmReg<T>& immOrReg ) const
	{
		_DoI_helpermess( *this, to, immOrReg );
	}

	template< typename T > __noinline void operator()( const xDirectOrIndirect<T>& to, int imm ) const
	{
		_DoI_helpermess( *this, to, imm );
	}

	template< typename T > __noinline void operator()( const xDirectOrIndirect<T>& to, const xDirectOrIndirect<T>& from ) const
	{
		if( to == from ) return;
		_DoI_helpermess( *this, to, from );
	}

#endif
	};

	struct xImpl_MovImm64
	{
		xImpl_MovImm64() {}

		void operator()(const xRegister64& to, s64 imm, bool preserve_flags = false) const;
	};

	struct xImpl_CMov
	{
		JccComparisonType ccType;
		void operator()(const xRegister16or32or64& to, const xRegister16or32or64& from) const;
		void operator()(const xRegister16or32or64& to, const xIndirectVoid& sibsrc) const;

	};

	struct xImpl_Set
	{
		JccComparisonType ccType;

		void operator()(const xRegister8& to) const;
		void operator()(const xIndirect8& dest) const;

	};


	struct xImpl_MovExtend
	{
		bool SignExtend;

		void operator()(const xRegister16or32or64& to, const xRegister8& from) const;
		void operator()(const xRegister16or32or64& to, const xIndirect8& sibsrc) const;
		void operator()(const xRegister32or64& to, const xRegister16& from) const;
		void operator()(const xRegister32or64& to, const xIndirect16& sibsrc) const;
		void operator()(const xRegister64& to, const xRegister32& from) const;
		void operator()(const xRegister64& to, const xIndirect32& sibsrc) const;

	};

}
