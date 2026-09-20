// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "rapidjson/document.h"

class JsonValueWrapper
{
public:
	JsonValueWrapper(
		rapidjson::Value& value,
		rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>& allocator)
		: m_value(value)
		, m_allocator(allocator)
	{
	}

	rapidjson::Value& value()
	{
		return m_value;
	}

	const rapidjson::Value& value() const
	{
		return m_value;
	}

	rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>& allocator()
	{
		return m_allocator;
	}

private:
	rapidjson::Value& m_value;
	rapidjson::MemoryPoolAllocator<rapidjson::CrtAllocator>& m_allocator;
};
