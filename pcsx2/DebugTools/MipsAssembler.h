// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "MipsAssemblerTables.h"
#include "DebugInterface.h"

enum MipsImmediateType { MIPS_NOIMMEDIATE, MIPS_IMMEDIATE5,
	MIPS_IMMEDIATE16, MIPS_IMMEDIATE20, MIPS_IMMEDIATE26 };

enum MipsArchType { MARCH_PSX = 0, MARCH_N64, MARCH_PS2, MARCH_PSP, MARCH_INVALID };

typedef struct {
	const char* name;
	short num;
} tMipsRegister;

typedef struct {
	char name[5];
	short num;
} MipsRegisterInfo;

struct MipsImmediate
{
	int value;
	int originalValue;
};

struct MipsOpcodeRegisters {
	MipsRegisterInfo grs;
	MipsRegisterInfo grt;
	MipsRegisterInfo grd;

	MipsRegisterInfo frs;
	MipsRegisterInfo frt;
	MipsRegisterInfo frd;

	MipsRegisterInfo ps2vrs;
	MipsRegisterInfo ps2vrt;
	MipsRegisterInfo ps2vrd;

	void reset()
	{
		grs.num = grt.num = grd.num = -1;
		frs.num = frt.num = frd.num = -1;
		ps2vrs.num = ps2vrt.num = ps2vrd.num = -1;
	}
};


class CMipsInstruction
{
public:
	CMipsInstruction(DebugInterface* cpu);
	bool Load(const char* Name, const char* Params, int RamPos);
	virtual bool Validate();
	virtual void Encode();
	u32 getEncoding() { return encoding; };
	std::string getErrorMessage() { return error; };
private:
	void encodeNormal();
	bool parseOpcode(const tMipsOpcode& SourceOpcode, const char* Line);
	bool LoadEncoding(const tMipsOpcode& SourceOpcode, const char* Line);
	void setOmittedRegisters();

	tMipsOpcode Opcode;
	bool NoCheckError;
	bool Loaded;
	int RamPos;

	MipsOpcodeRegisters registers;
	MipsImmediateType immediateType;
	MipsImmediate immediate;
	int vfpuSize;

	DebugInterface* cpu;
	u32 encoding;
	std::string error;
};

bool MipsAssembleOpcode(const char* line, DebugInterface* cpu, u32 address, u32& dest, std::string& errorText);
