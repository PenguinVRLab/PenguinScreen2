// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Pcsx2Defs.h"
#include <cstring>
#include <cstdlib>
#include <new>
#include <memory>
#include <type_traits>
#include <utility>

#ifdef _MSC_VER
#include <malloc.h>
#endif

#define safe_aligned_free(ptr) \
	((void)(_aligned_free(ptr), (ptr) = NULL))

#if !defined(_MSC_VER)
extern void* _aligned_malloc(size_t size, size_t align);
extern void* pcsx2_aligned_realloc(void* handle, size_t new_size, size_t align, size_t old_size);
extern void _aligned_free(void* pmem);
#else
#define pcsx2_aligned_realloc(handle, new_size, align, old_size) \
	_aligned_realloc(handle, new_size, align)
#endif
