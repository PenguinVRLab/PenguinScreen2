// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>

class Error;

class DynamicLibrary final
{
public:
	DynamicLibrary();

	DynamicLibrary(const char* filename);

	DynamicLibrary(DynamicLibrary&& move);

	~DynamicLibrary();

	static std::string GetUnprefixedFilename(const char* filename);

	static std::string GetVersionedFilename(const char* libname, int major = -1, int minor = -1);

	bool IsOpen() const { return m_handle != nullptr; }

	bool Open(const char* filename, Error* error);

	void Adopt(void* handle);

	void Close();

	void* GetSymbolAddress(const char* name) const;

	template <typename T>
	bool GetSymbol(const char* name, T* ptr) const
	{
		*ptr = reinterpret_cast<T>(GetSymbolAddress(name));
		return *ptr != nullptr;
	}

	void* GetHandle() const { return m_handle; }

	DynamicLibrary& operator=(DynamicLibrary&& move);

private:
	DynamicLibrary(const DynamicLibrary&) = delete;
	DynamicLibrary& operator=(const DynamicLibrary&) = delete;

	void* m_handle = nullptr;
};
