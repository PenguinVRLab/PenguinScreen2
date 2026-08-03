// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

#if 0

template< typename xImpl, typename T >
void _DoI_helpermess( const xImpl& helpme, const xDirectOrIndirect& to, const xImmReg<T>& immOrReg )
{
	if( to.IsDirect() )
	{
		if( immOrReg.IsReg() )
			helpme( to.GetReg(), immOrReg.GetReg() );
		else
			helpme( to.GetReg(), immOrReg.GetImm() );
	}
	else
	{
		if( immOrReg.IsReg() )
			helpme( to.GetMem(), immOrReg.GetReg() );
		else
			helpme( to.GetMem(), immOrReg.GetImm() );
	}
}

template< typename xImpl, typename T >
void _DoI_helpermess( const xImpl& helpme, const ModSibBase& to, const xImmReg<T>& immOrReg )
{
	if( immOrReg.IsReg() )
		helpme( to, immOrReg.GetReg() );
	else
		helpme( (ModSibStrict)to, immOrReg.GetImm() );
}

template< typename xImpl, typename T >
void _DoI_helpermess( const xImpl& helpme, const xDirectOrIndirect<T>& to, int imm )
{
	if( to.IsDirect() )
		helpme( to.GetReg(), imm );
	else
		helpme( to.GetMem(), imm );
}

template< typename xImpl, typename T >
void _DoI_helpermess( const xImpl& helpme, const xDirectOrIndirect<T>& parm )
{
	if( parm.IsDirect() )
		helpme( parm.GetReg() );
	else
		helpme( parm.GetMem() );
}

template< typename xImpl, typename T >
void _DoI_helpermess( const xImpl& helpme, const xDirectOrIndirect<T>& to, const xDirectOrIndirect<T>& from )
{
	if( to.IsDirect() && from.IsDirect() )
		helpme( to.GetReg(), from.GetReg() );

	else if( to.IsDirect() )
		helpme( to.GetReg(), from.GetMem() );

	else if( from.IsDirect() )
		helpme( to.GetMem(), from.GetReg() );

	else

		pxFailDev( "Invalid asm instruction: Both operands are indirect memory addresses." );
}
#endif

}
