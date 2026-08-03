// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SIO/SioTypes.h"

class StateWrapper;

class Sio0
{
private:
	u32 txData;
	u32 rxData;
	u32 stat;
	u16 mode;
	u16 ctrl;
	u16 baud;

	void ClearStatAcknowledge();

public:
	u8 flag = 0;

	SioStage sioStage = SioStage::IDLE;
	u8 sioMode = SioMode::NOT_SET;
	u8 sioCommand = 0;
	bool padStarted = false;
	bool rxDataSet = false;

	u8 port = 0;
	u8 slot = 0;

	Sio0();
	~Sio0();

	bool Initialize();
	bool Shutdown();

	void SoftReset();
	bool DoState(StateWrapper& sw);

	void SetAcknowledge(bool ack);
	void Interrupt(Sio0Interrupt sio0Interrupt);

	u8 GetTxData();
	u8 GetRxData();
	u32 GetStat();
	u16 GetMode();
	u16 GetCtrl();
	u16 GetBaud();

	void SetTxData(u8 value);
	void SetRxData(u8 value);
	void SetStat(u32 value);
	void SetMode(u16 value);
	void SetCtrl(u16 value);
	void SetBaud(u16 value);

	bool IsPadCommand(u8 command);
	bool IsMemcardCommand(u8 command);
	bool IsPocketstationCommand(u8 command);

	u8 Memcard(u8 value);
};

extern Sio0 g_Sio0;
