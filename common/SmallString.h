// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Pcsx2Defs.h"

#include "fmt/base.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

class SmallStringBase
{
public:
	using value_type = char;

	SmallStringBase();
	SmallStringBase(const char* str);
	SmallStringBase(const char* str, u32 length);
	SmallStringBase(const SmallStringBase& copy);
	SmallStringBase(SmallStringBase&& move);
	SmallStringBase(const std::string& str);
	SmallStringBase(const std::string_view sv);

	~SmallStringBase();

	void assign(const char* str);
	void assign(const char* str, u32 length);
	void assign(const std::string& copy);
	void assign(const std::string_view copy);
	void assign(const SmallStringBase& copy);
	void assign(SmallStringBase&& move);

	void make_room_for(u32 space);

	void clear();

	void append(char c);

	void append(const char* appendText);
	void append(const char* str, u32 length);
	void append(const std::string& str);
	void append(const std::string_view str);
	void append(const SmallStringBase& str);

	void append_sprintf(const char* format, ...) ;
	void append_vsprintf(const char* format, va_list ap);

	template <typename... T>
	void append_format(fmt::format_string<T...> fmt, T&&... args);

	void append_hex(const void* data, size_t len);

	void prepend(char c);

	void prepend(const char* str);
	void prepend(const char* str, u32 length);
	void prepend(const std::string& str);
	void prepend(const std::string_view str);
	void prepend(const SmallStringBase& str);

	void prepend_sprintf(const char* format, ...) ;
	void prepend_vsprintf(const char* format, va_list ap);

	template <typename... T>
	void prepend_format(fmt::format_string<T...> fmt, T&&... args);

	void insert(s32 offset, const char* str);
	void insert(s32 offset, const char* str, u32 length);
	void insert(s32 offset, const std::string& str);
	void insert(s32 offset, const std::string_view str);
	void insert(s32 offset, const SmallStringBase& str);

	void sprintf(const char* format, ...) ;
	void vsprintf(const char* format, va_list ap);

	template <typename... T>
	void format(fmt::format_string<T...> fmt, T&&... args);

	void vformat(fmt::string_view fmt, fmt::format_args args);

	bool equals(const char* str) const;
	bool equals(const SmallStringBase& str) const;
	bool equals(const std::string_view str) const;
	bool equals(const std::string& str) const;
	bool iequals(const char* str) const;
	bool iequals(const SmallStringBase& str) const;
	bool iequals(const std::string_view str) const;
	bool iequals(const std::string& str) const;

	int compare(const char* str) const;
	int compare(const SmallStringBase& str) const;
	int compare(const std::string_view str) const;
	int compare(const std::string& str) const;
	int icompare(const char* str) const;
	int icompare(const SmallStringBase& str) const;
	int icompare(const std::string_view str) const;
	int icompare(const std::string& str) const;

	bool starts_with(const char* str, bool case_sensitive = true) const;
	bool starts_with(const SmallStringBase& str, bool case_sensitive = true) const;
	bool starts_with(const std::string_view str, bool case_sensitive = true) const;
	bool starts_with(const std::string& str, bool case_sensitive = true) const;
	bool ends_with(const char* str, bool case_sensitive = true) const;
	bool ends_with(const SmallStringBase& str, bool case_sensitive = true) const;
	bool ends_with(const std::string_view str, bool case_sensitive = true) const;
	bool ends_with(const std::string& str, bool case_sensitive = true) const;

	s32 find(char c, u32 offset = 0) const;
	s32 rfind(char c, u32 offset = 0) const;

	s32 find(const char* str, u32 offset = 0) const;

	u32 count(char ch) const;

	void erase(s32 offset, s32 count = std::numeric_limits<s32>::max());

	void reserve(u32 new_reserve);

	void resize(u32 new_size, char fill = ' ', bool shrink_if_smaller = false);

	void update_size();

	void shrink_to_fit();

	__fi u32 length() const { return m_length; }
	__fi bool empty() const { return (m_length == 0); }

	__fi u32 buffer_size() const { return m_buffer_size; }

	__fi const char* c_str() const { return m_buffer; }

	__fi char* data() { return m_buffer; }

	__fi const char* end_ptr() const { return m_buffer + m_length; }

	__fi void push_back(value_type val) { append(val); }

	std::string_view view() const;

	std::string_view substr(s32 offset, s32 count) const;

	__fi operator const char*() const { return c_str(); }
	__fi operator char*() { return data(); }
	__fi operator std::string_view() const { return view(); }

	__fi bool operator==(const char* str) const { return equals(str); }
	__fi bool operator==(const SmallStringBase& str) const { return equals(str); }
	__fi bool operator==(const std::string_view str) const { return equals(str); }
	__fi bool operator==(const std::string& str) const { return equals(str); }
	__fi bool operator!=(const char* str) const { return !equals(str); }
	__fi bool operator!=(const SmallStringBase& str) const { return !equals(str); }
	__fi bool operator!=(const std::string_view str) const { return !equals(str); }
	__fi bool operator!=(const std::string& str) const { return !equals(str); }
	__fi bool operator<(const char* str) const { return (compare(str) < 0); }
	__fi bool operator<(const SmallStringBase& str) const { return (compare(str) < 0); }
	__fi bool operator<(const std::string_view str) const { return (compare(str) < 0); }
	__fi bool operator<(const std::string& str) const { return (compare(str) < 0); }
	__fi bool operator>(const char* str) const { return (compare(str) > 0); }
	__fi bool operator>(const SmallStringBase& str) const { return (compare(str) > 0); }
	__fi bool operator>(const std::string_view str) const { return (compare(str) > 0); }
	__fi bool operator>(const std::string& str) const { return (compare(str) > 0); }

	SmallStringBase& operator=(const SmallStringBase& copy);
	SmallStringBase& operator=(const char* str);
	SmallStringBase& operator=(const std::string& str);
	SmallStringBase& operator=(const std::string_view str);
	SmallStringBase& operator=(SmallStringBase&& move);

protected:
	char* m_buffer = nullptr;

	u32 m_length = 0;

	u32 m_buffer_size = 0;

	bool m_on_heap = false;
};

template <u32 L>
class SmallStackString : public SmallStringBase
{
public:
	__fi SmallStackString() { init(); }

	__fi SmallStackString(const char* str)
	{
		init();
		assign(str);
	}

	__fi SmallStackString(const char* str, u32 length)
	{
		init();
		assign(str, length);
	}

	__fi SmallStackString(const SmallStringBase& copy)
	{
		init();
		assign(copy);
	}

	__fi SmallStackString(SmallStringBase&& move)
	{
		init();
		assign(move);
	}

	__fi SmallStackString(const SmallStackString& copy)
	{
		init();
		assign(copy);
	}

	__fi SmallStackString(SmallStackString&& move)
	{
		init();
		assign(move);
	}

	__fi SmallStackString(const std::string_view sv)
	{
		init();
		assign(sv);
	}

	__fi SmallStackString& operator=(const SmallStringBase& copy)
	{
		assign(copy);
		return *this;
	}

	__fi SmallStackString& operator=(SmallStringBase&& move)
	{
		assign(move);
		return *this;
	}

	__fi SmallStackString& operator=(const SmallStackString& copy)
	{
		assign(copy);
		return *this;
	}

	__fi SmallStackString& operator=(SmallStackString&& move)
	{
		assign(move);
		return *this;
	}

	__fi SmallStackString& operator=(const std::string_view sv)
	{
		assign(sv);
		return *this;
	}

	__fi SmallStackString& operator=(const char* str)
	{
		assign(str);
		return *this;
	}

	static SmallStackString from_sprintf(const char* format, ...) ;

	template <typename... T>
	static SmallStackString from_format(fmt::format_string<T...> fmt, T&&... args);

	static SmallStackString from_vformat(fmt::string_view fmt, fmt::format_args args);

private:
	char m_stack_buffer[L + 1];

	__fi void init()
	{
		m_buffer = m_stack_buffer;
		m_buffer_size = L + 1;

#ifdef _DEBUG
		std::memset(m_stack_buffer, 0, sizeof(m_stack_buffer));
#else
		m_stack_buffer[0] = '\0';
#endif
	}
};

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4459)
#endif

template <u32 L>
SmallStackString<L> SmallStackString<L>::from_sprintf(const char* format, ...)
{
	std::va_list ap;
	va_start(ap, format);

	SmallStackString ret;
	ret.vsprintf(format, ap);

	va_end(ap);

	return ret;
}

template <u32 L>
template <typename... T>
__fi SmallStackString<L> SmallStackString<L>::from_format(fmt::format_string<T...> fmt, T&&... args)
{
	SmallStackString<L> ret;
	fmt::vformat_to(std::back_inserter(ret), fmt, fmt::make_format_args(args...));
	return ret;
}

template <u32 L>
__fi SmallStackString<L> SmallStackString<L>::from_vformat(fmt::string_view fmt, fmt::format_args args)
{
	SmallStackString<L> ret;
	fmt::vformat_to(std::back_inserter(ret), fmt, args);
	return ret;
}

using TinyString = SmallStackString<64>;
using SmallString = SmallStackString<256>;

template <typename... T>
__fi void SmallStringBase::append_format(fmt::format_string<T...> fmt, T&&... args)
{
	fmt::vformat_to(std::back_inserter(*this), fmt, fmt::make_format_args(args...));
}

template <typename... T>
__fi void SmallStringBase::prepend_format(fmt::format_string<T...> fmt, T&&... args)
{
	TinyString str;
	fmt::vformat_to(std::back_inserter(str), fmt, fmt::make_format_args(args...));
	prepend(str);
}

template <typename... T>
__fi void SmallStringBase::format(fmt::format_string<T...> fmt, T&&... args)
{
	clear();
	fmt::vformat_to(std::back_inserter(*this), fmt, fmt::make_format_args(args...));
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define MAKE_FORMATTER(type) \
	template <> \
	struct fmt::formatter<type> \
	{ \
		template <typename ParseContext> \
		constexpr auto parse(ParseContext& ctx) \
		{ \
			return ctx.begin(); \
		} \
\
		template <typename FormatContext> \
		auto format(const type& str, FormatContext& ctx) const \
		{ \
			return fmt::format_to(ctx.out(), "{}", str.view()); \
		} \
	};

MAKE_FORMATTER(TinyString);
MAKE_FORMATTER(SmallString);

#undef MAKE_FORMATTER