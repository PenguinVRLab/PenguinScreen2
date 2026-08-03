// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <deque>

class StateWrapper;

class Sio2
{
public:
	std::array<u32, 16> CmdQueue;
	std::array<u32, 4> PortCtrl0;
	std::array<u32, 4> PortCtrl1;
	u32 dataIn;
	u32 dataOut;
	u32 ctrl;
	u32 CmdStat;
	u32 PortStat;
	u32 FifoStat;
	u32 FifoTxPos;
	u32 FifoRxPos;
	u32 iStat;

	u8 port = 0;

	bool queueRead = false;
	size_t queuePosition = 0;
	size_t commandLength = 0;
	size_t processedLength = 0;
	size_t dmaBlockSize = 0;
	bool queueComplete = false;

	Sio2();
	~Sio2();

	bool Initialize();
	bool Shutdown();

	void SoftReset();
	bool DoState(StateWrapper& sw);

	void Interrupt();

	void SetCtrl(u32 value);
	void SetCmd(size_t position, u32 value);
	void SetCmdStat(u32 value);

	void Pad();
	void Multitap();
	void Infrared();
	void Memcard();

	void Write(u8 data);
	u8 Read();
};

extern std::deque<u8> g_Sio2FifoIn;
extern std::deque<u8> g_Sio2FifoOut;
extern Sio2 g_Sio2;
