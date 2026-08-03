// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

class Error;

class PageProtectionMode
{
protected:
	bool m_read = false;
	bool m_write = false;
	bool m_exec = false;

public:
	__fi constexpr PageProtectionMode() = default;

	__fi constexpr PageProtectionMode& Read(bool allow = true)
	{
		m_read = allow;
		return *this;
	}

	__fi constexpr PageProtectionMode& Write(bool allow = true)
	{
		m_write = allow;
		return *this;
	}

	__fi constexpr PageProtectionMode& Execute(bool allow = true)
	{
		m_exec = allow;
		return *this;
	}

	__fi constexpr PageProtectionMode& All(bool allow = true)
	{
		m_read = m_write = m_exec = allow;
		return *this;
	}

	__fi constexpr bool CanRead() const { return m_read; }
	__fi constexpr bool CanWrite() const { return m_write; }
	__fi constexpr bool CanExecute() const { return m_exec && m_read; }
	__fi constexpr bool IsNone() const { return !m_read && !m_write; }
};

static __fi PageProtectionMode PageAccess_None()
{
	return PageProtectionMode();
}

static __fi PageProtectionMode PageAccess_ReadOnly()
{
	return PageProtectionMode().Read();
}

static __fi PageProtectionMode PageAccess_WriteOnly()
{
	return PageProtectionMode().Write();
}

static __fi PageProtectionMode PageAccess_ReadWrite()
{
	return PageAccess_ReadOnly().Write();
}

static __fi PageProtectionMode PageAccess_ExecOnly()
{
	return PageAccess_ReadOnly().Execute();
}

static __fi PageProtectionMode PageAccess_Any()
{
	return PageProtectionMode().All();
}

namespace HostSys
{
	extern void MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode);

	extern std::string GetFileMappingName(const char* prefix);
	extern void* CreateSharedMemory(const char* name, size_t size);
	extern void DestroySharedMemory(void* ptr);

#if !defined(__APPLE__) || !defined(ARCH_ARM64)
	[[maybe_unused]] __fi static void BeginCodeWrite() {}
	[[maybe_unused]] __fi static void EndCodeWrite() {}
	// clang-format on
#else
	void BeginCodeWrite();
	void EndCodeWrite();
#endif

#ifdef ARCH_X86
	[[maybe_unused]] __fi static void FlushInstructionCache(void* address, u32 size) {}
#else
	void FlushInstructionCache(void* address, u32 size);
#endif

	size_t GetRuntimePageSize();

	size_t GetRuntimeCacheLineSize();
}

namespace PageFaultHandler
{
	enum class HandlerResult
	{
		ContinueExecution,
		ExecuteNextHandler,
	};

	HandlerResult HandlePageFault(void* exception_pc, void* fault_address, bool is_write);
	bool Install(Error* error = nullptr);
	bool InstallSecondaryThread();
}

class SharedMemoryMappingArea
{
public:
	static std::unique_ptr<SharedMemoryMappingArea> Create(size_t size, bool jit = false);

	~SharedMemoryMappingArea();

	__fi size_t GetSize() const { return m_size; }
	__fi size_t GetNumPages() const { return m_num_pages; }

	__fi u8* BasePointer() const { return m_base_ptr; }
	__fi u8* OffsetPointer(size_t offset) const { return m_base_ptr + offset; }
	__fi u8* PagePointer(size_t page) const { return m_base_ptr + __pagesize * page; }

	u8* Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode);
	bool Unmap(void* map_base, size_t map_size, bool is_file = true);

private:
	SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages);

	u8* m_base_ptr;
	size_t m_size;
	size_t m_num_pages;
	size_t m_num_mappings = 0;

#ifdef _WIN32
	using PlaceholderMap = std::map<size_t, size_t>;

	PlaceholderMap::iterator FindPlaceholder(size_t page);

	PlaceholderMap m_placeholder_ranges;
#endif
};

extern u64 GetTickFrequency();
extern u64 GetCPUTicks();
extern u64 GetPhysicalMemory();
extern u64 GetAvailablePhysicalMemory();
extern u32 ShortSpin();
extern const u32 SPIN_TIME_NS;
[[noreturn]] void AbortWithMessage(const char* msg);

extern std::string GetOSVersionString();

struct CPUInfo {
	std::string name;
	u32 num_big_cores;
	u32 num_small_cores;
	u32 num_threads;
	u32 num_clusters;
};

const CPUInfo& GetCPUInfo();

namespace Common
{
	bool InhibitScreensaver(bool inhibit);

	bool PlaySoundAsync(const char* path);

	void SetMousePosition(int x, int y);
	bool AttachMousePositionCb(std::function<void(int,int)> cb);
	void DetachMousePositionCb();
}
