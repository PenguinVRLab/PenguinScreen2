// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace x86Emitter
{

	struct xImpl_IncDec
	{
		bool isDec;

		void operator()(const xRegisterInt& to) const;
		void operator()(const xIndirect64orLess& to) const;
	};

}
