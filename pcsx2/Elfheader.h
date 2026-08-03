// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

class Error;
class IsoReader;

struct ELF_HEADER {
	u8	e_ident[16];
	u16	e_type;
	u16	e_machine;
	u32	e_version;
	u32	e_entry;
	u32	e_phoff;
	u32	e_shoff;
	u32	e_flags;
	u16	e_ehsize;
	u16	e_phentsize;
	u16	e_phnum;
	u16	e_shentsize;
	u16	e_shnum;
	u16	e_shstrndx;
};

struct ELF_PHR {
	u32 p_type;
	u32 p_offset;
	u32 p_vaddr;
	u32 p_paddr;
	u32 p_filesz;
	u32 p_memsz;
	u32 p_flags;
	u32 p_align;
};

struct ELF_SHR {
	u32	sh_name;
	u32	sh_type;
	u32	sh_flags;
	u32	sh_addr;
	u32	sh_offset;
	u32	sh_size;
	u32	sh_link;
	u32	sh_info;
	u32	sh_addralign;
	u32	sh_entsize;
};

struct Elf32_Sym {
	u32	st_name;
	u32	st_value;
	u32	st_size;
	u8	st_info;
	u8	st_other;
	u16	st_shndx;
};

#define ELF32_ST_TYPE(i) ((i)&0xf)

struct Elf32_Rel {
	u32	r_offset;
	u32	r_info;
};

class ElfObject final
{
public:
	ElfObject();
	ElfObject(const ElfObject&) = delete;
	~ElfObject();

	__fi const std::vector<u8>& GetData() const { return data; }
	__fi std::vector<u8> ReleaseData() const { return std::move(data); }
	__fi const ELF_HEADER& GetHeader() const { return *reinterpret_cast<const ELF_HEADER*>(data.data()); }
	__fi u32 GetSize() const { return static_cast<u32>(data.size()); }

	bool OpenFile(std::string srcfile, bool isPSXElf_, Error* error);
	bool OpenIsoFile(std::string srcfile, IsoReader& isor, bool isPSXElf_, Error* error);

	void LoadHeaders();

	bool HasProgramHeaders() const;
	bool HasSectionHeaders() const;
	bool HasHeaders() const;

	std::pair<u32, u32> GetTextRange() const;
	u32 GetEntryPoint() const;
	u32 GetCRC() const;

private:
	std::vector<u8> data;
	ELF_PHR* proghead = nullptr;
	ELF_SHR* secthead = nullptr;
	std::string filename;
	bool isPSXElf;

	bool CheckElfSize(s64 size, Error* error);

	void InitElfHeaders();
	void LoadProgramHeaders();
	void LoadSectionHeaders();

	bool HasValidPSXHeader() const;
};

