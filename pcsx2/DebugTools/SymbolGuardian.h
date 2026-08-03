// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/MemoryInterface.h"
#include "common/Pcsx2Types.h"

#include <ccc/ast.h>
#include <ccc/symbol_database.h>
#include <ccc/symbol_file.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>

struct SymbolInfo
{
	std::optional<ccc::SymbolDescriptor> descriptor;
	u32 handle = (u32)-1;
	std::string name;
	ccc::Address address;
	u32 size = 0;
};

struct FunctionInfo
{
	ccc::FunctionHandle handle;
	std::string name;
	ccc::Address address;
	u32 size = 0;
	bool is_no_return = false;
};

class SymbolGuardian
{
public:
	SymbolGuardian() = default;
	SymbolGuardian(const SymbolGuardian& rhs) = delete;
	SymbolGuardian(SymbolGuardian&& rhs) = delete;
	~SymbolGuardian() = default;
	SymbolGuardian& operator=(const SymbolGuardian& rhs) = delete;
	SymbolGuardian& operator=(SymbolGuardian&& rhs) = delete;

	using ReadCallback = std::function<void(const ccc::SymbolDatabase&)>;
	using ReadWriteCallback = std::function<void(ccc::SymbolDatabase&)>;

	void Read(ReadCallback callback) const noexcept;

	void ReadWrite(ReadWriteCallback callback) noexcept;

	SymbolInfo SymbolStartingAtAddress(
		u32 address, u32 descriptors = ccc::ALL_SYMBOL_TYPES) const;
	SymbolInfo SymbolAfterAddress(
		u32 address, u32 descriptors = ccc::ALL_SYMBOL_TYPES) const;
	SymbolInfo SymbolOverlappingAddress(
		u32 address, u32 descriptors = ccc::ALL_SYMBOL_TYPES) const;
	SymbolInfo SymbolWithName(
		const std::string& name, u32 descriptors = ccc::ALL_SYMBOL_TYPES) const;

	bool FunctionExistsWithStartingAddress(u32 address) const;
	bool FunctionExistsThatOverlapsAddress(u32 address) const;

	FunctionInfo FunctionStartingAtAddress(u32 address) const;
	FunctionInfo FunctionOverlappingAddress(u32 address) const;

	static void GenerateFunctionHashes(ccc::SymbolDatabase& database, MemoryInterface& reader);

	static void UpdateFunctionHashes(ccc::SymbolDatabase& database, MemoryInterface& reader);

	static std::optional<ccc::FunctionHash> HashFunction(const ccc::Function& function, MemoryInterface& reader);

	void ClearIrxModules();

	static const char* TranslateSymbolSourceName(const char* name);

protected:
	ccc::SymbolDatabase m_database;
	mutable std::shared_mutex m_big_symbol_lock;
};

extern SymbolGuardian R5900SymbolGuardian;
extern SymbolGuardian R3000SymbolGuardian;
