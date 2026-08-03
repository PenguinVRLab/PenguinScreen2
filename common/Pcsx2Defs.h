// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Pcsx2Types.h"

#include <bit>
#include <cstddef>

#ifdef PCSX2_DEVBUILD
static constexpr bool IsDevBuild = true;
#else
static constexpr bool IsDevBuild = false;
#endif

#ifdef PCSX2_DEBUG
static constexpr bool IsDebugBuild = true;
#else
static constexpr bool IsDebugBuild = false;
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
	#define ARCH_ARM64
#elif defined(_M_X86) || defined(__x86_64__) || defined(__i386__)
	#define ARCH_X86
#else
	#error Unsupported Platform
#endif

#if defined(OVERRIDE_HOST_PAGE_SIZE)
	static constexpr unsigned int __pagesize = OVERRIDE_HOST_PAGE_SIZE;
	static constexpr unsigned int __pagemask = __pagesize - 1;
	static constexpr unsigned int __pageshift = std::bit_width(__pagemask);
#elif defined(ARCH_ARM64)
	static constexpr unsigned int __pagesize = 0x4000;
	static constexpr unsigned int __pageshift = 14;
	static constexpr unsigned int __pagemask = __pagesize - 1;
#else
	static constexpr unsigned int __pagesize = 0x1000;
	static constexpr unsigned int __pageshift = 12;
	static constexpr unsigned int __pagemask = __pagesize - 1;
#endif
#if defined(OVERRIDE_HOST_CACHE_LINE_SIZE)
	static constexpr unsigned int __cachelinesize = OVERRIDE_HOST_CACHE_LINE_SIZE;
#elif defined(ARCH_ARM64)
	static constexpr unsigned int __cachelinesize = 128;
#else
	static constexpr unsigned int __cachelinesize = 64;
#endif

static constexpr unsigned int __pagealignsize = 0x1000;

#ifdef _MSC_VER

#define __forceinline_odr __forceinline
#define __noinline __declspec(noinline)
#define __noreturn __declspec(noreturn)

#define RESTRICT __restrict
#define ASSUME(x) __assume(x)

#else

#ifndef _WIN32
#define __vectorcall
#endif

#define __forceinline __attribute__((always_inline, unused))
#define __forceinline_odr __forceinline inline
#define __noinline __attribute__((noinline))
#define __noreturn __attribute__((noreturn))

#define RESTRICT __restrict__

#define ASSUME(x) \
	do \
	{ \
		if (!(x)) \
			__builtin_unreachable(); \
	} while (0)

#endif

#define __fi __forceinline
#ifdef PCSX2_DEVBUILD
#define __ri
#else
#define __ri __fi
#endif

#define safe_delete(ptr) (delete (ptr), (ptr) = nullptr)
#define safe_delete_array(ptr) (delete[] (ptr), (ptr) = nullptr)
#define safe_free(ptr) (std::free(ptr), (ptr) = nullptr)

#ifndef DeclareNoncopyableObject
#define DeclareNoncopyableObject(classname) \
public: \
	classname(const classname&) = delete; \
	classname& operator=(const classname&) = delete
#endif

static constexpr sptr _1kb = 1024 * 1;
static constexpr sptr _4kb = _1kb * 4;
static constexpr sptr _16kb = _1kb * 16;
static constexpr sptr _32kb = _1kb * 32;
static constexpr sptr _64kb = _1kb * 64;
static constexpr sptr _128kb = _1kb * 128;
static constexpr sptr _256kb = _1kb * 256;

static constexpr s64 _1mb = 1024 * 1024;
static constexpr s64 _8mb = _1mb * 8;
static constexpr s64 _16mb = _1mb * 16;
static constexpr s64 _32mb = _1mb * 32;
static constexpr s64 _64mb = _1mb * 64;
static constexpr s64 _256mb = _1mb * 256;
static constexpr s64 _1gb = _1mb * 1024;
static constexpr s64 _4gb = _1gb * 4;

#ifdef _MSC_VER
#pragma warning(disable : 4244)
#pragma warning(disable : 4267)
#endif
