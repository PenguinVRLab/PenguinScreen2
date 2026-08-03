// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "Pcsx2Defs.h"
#include <optional>
#include <utility>

template <typename T>
class ScopedGuard final
{
public:
	__fi ScopedGuard(T&& func)
		: m_func(std::forward<T>(func))
	{
	}
	__fi ScopedGuard(ScopedGuard&& other)
		: m_func(std::move(other.m_func))
	{
		other.m_func = nullptr;
	}

	__fi ~ScopedGuard()
	{
		Run();
	}

	ScopedGuard(const ScopedGuard&) = delete;
	void operator=(const ScopedGuard&) = delete;

	__fi void Run()
	{
		if (!m_func.has_value())
			return;

		m_func.value()();
		m_func.reset();
	}

	__fi void Cancel()
	{
		m_func.reset();
	}

private:
	std::optional<T> m_func;
};
