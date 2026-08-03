// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Host.h"
#include "IopDma.h"
#include "Recording/InputRecording.h"
#include "SIO/Memcard/MemoryCardProtocol.h"
#include "SIO/Multitap/MultitapProtocol.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "SIO/Sio.h"
#include "SIO/Sio2.h"
#include "SIO/SioTypes.h"
#include "StateWrapper.h"

#define SIO2LOG_ENABLE 0
#define Sio2Log if (SIO2LOG_ENABLE) DevCon

std::deque<u8> g_Sio2FifoIn;
std::deque<u8> g_Sio2FifoOut;

Sio2 g_Sio2;

Sio2::Sio2() = default;
Sio2::~Sio2() = default;

bool Sio2::Initialize()
{
	this->SoftReset();

	for (size_t i = 0; i < CmdQueue.size(); i++)
	{
		CmdQueue[i] = 0;
	}

	for (size_t i = 0; i < PortCtrl0.size(); i++)
	{
		PortCtrl0[i] = 0;
		PortCtrl1[i] = 0;
	}

	dataIn = 0;
	dataOut = 0;
	SetCtrl(Sio2Ctrl::SIO2MAN_RESET);
	SetCmdStat(CmdStat::DISCONNECTED);
	PortStat = PortStat::DEFAULT;
	FifoStat = FifoStat::DEFAULT;
	FifoTxPos = 0;
	FifoRxPos = 0;
	iStat = 0;

	port = 0;

	while (!g_Sio2FifoOut.empty())
	{
		g_Sio2FifoOut.pop_front();
	}

	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			mcds[i][j].term = 0x55;
			mcds[i][j].port = i;
			mcds[i][j].slot = j;
			mcds[i][j].FLAG = 0x08;
			mcds[i][j].autoEjectTicks = 0;
		}
	}

	mcd = &mcds[0][0];
	return true;
}

bool Sio2::Shutdown()
{
	return true;
}

void Sio2::SoftReset()
{
	queueRead = false;
	queuePosition = 0;
	commandLength = 0;
	processedLength = 0;
	dmaBlockSize = 0;
	queueComplete = false;

	while (!g_Sio2FifoIn.empty())
	{
		g_Sio2FifoIn.pop_front();
	}

	CmdStat = 0;
}

void Sio2::Interrupt()
{
	if (!iStat)
		iopIntcIrq(17);
	else
		DevCon.Warning("Nearly sent double SIO2 IRQ");

	iStat |= 1;
}

void Sio2::SetCtrl(u32 value)
{
	this->ctrl = value;

	if (this->ctrl & Sio2Ctrl::START_TRANSFER)
	{
		Interrupt();
	}
}

void Sio2::SetCmd(size_t position, u32 value)
{
	this->CmdQueue[position] = value;

	if (position == 0)
	{
		SoftReset();
	}
}

void Sio2::SetCmdStat(u32 value)
{
	this->CmdStat = value;
}

void Sio2::Pad()
{
	MultitapProtocol& mtap = g_MultitapArr.at(port);
	PadBase* pad = Pad::GetPad(port, mtap.GetPadSlot());

	if (this->CmdStat & CmdStat::ONE_PORT_OPEN)
	{
		this->CmdStat &= ~(CmdStat::ONE_PORT_OPEN);
		this->CmdStat |= CmdStat::TWO_PORTS_OPEN;
	}
	else
	{
		this->CmdStat |= CmdStat::ONE_PORT_OPEN;
	}

	this->CmdStat |= CmdStat::NO_DEVICES_MISSING;

	if (pad->GetType() == Pad::ControllerType::NotConnected || pad->ejectTicks)
	{
		if (!port)
		{
			this->CmdStat |= CmdStat::PORT_1_MISSING;
		}
		else
		{
			this->CmdStat |= CmdStat::PORT_2_MISSING;
		}
	}

	g_Sio2FifoOut.push_back(0xff);
	pad->SoftReset();

	while (!g_Sio2FifoIn.empty())
	{
		if (pad->ejectTicks)
		{
			g_Sio2FifoIn.pop_front();
			g_Sio2FifoOut.push_back(0xff);
		}
		else
		{
			const u8 commandByte = g_Sio2FifoIn.front();
			g_Sio2FifoIn.pop_front();
			const u8 responseByte = pad->SendCommandByte(commandByte);
			g_Sio2FifoOut.push_back(responseByte);
		}
	}

	if (pad->ejectTicks)
	{
		pad->ejectTicks -= 1;
	}
}

void Sio2::Multitap()
{
	const bool multitapEnabled = EmuConfig.Pad.IsMultitapPortEnabled(this->port);
	
	if (this->CmdStat & CmdStat::ONE_PORT_OPEN)
	{
		this->CmdStat &= ~(CmdStat::ONE_PORT_OPEN);
		this->CmdStat |= CmdStat::TWO_PORTS_OPEN;
	}
	else
	{
		this->CmdStat |= CmdStat::ONE_PORT_OPEN;
	}

	this->CmdStat |= CmdStat::NO_DEVICES_MISSING;

	if (!multitapEnabled)
	{
		this->CmdStat |= CmdStat::PORT_1_MISSING;
	}

	g_MultitapArr.at(this->port).SendToMultitap();
}

void Sio2::Infrared()
{
	SetCmdStat(CmdStat::DISCONNECTED);

	g_Sio2FifoIn.pop_front();
	const u8 responseByte = 0xff;

	while (g_Sio2FifoOut.size() < commandLength)
	{
		g_Sio2FifoOut.push_back(responseByte);
	}
}

void Sio2::Memcard()
{
	MultitapProtocol& mtap = g_MultitapArr.at(this->port);

	mcd = &mcds[port][mtap.GetMemcardSlot()];

	if (mcd->autoEjectTicks)
	{
		SetCmdStat(CmdStat::DISCONNECTED);
		g_Sio2FifoOut.push_back(0xff);

		while (!g_Sio2FifoIn.empty())
		{
			g_Sio2FifoIn.pop_front();
			g_Sio2FifoOut.push_back(0xff);
		}

		return;
	}

	SetCmdStat(mcd->IsPresent() ? CmdStat::CONNECTED : CmdStat::DISCONNECTED);

	const u8 commandByte = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 responseByte = mcd->IsPresent() ? 0x00 : 0xff;
	g_Sio2FifoOut.push_back(responseByte);
	g_Sio2FifoOut.push_back(responseByte);
	u8 ps1Input = 0;
	u8 ps1Output = 0;

	switch (commandByte)
	{
		case MemcardCommand::PROBE:
			g_MemoryCardProtocol.Probe();
			break;
		case MemcardCommand::UNKNOWN_WRITE_DELETE_END:
			g_MemoryCardProtocol.UnknownWriteDeleteEnd();
			break;
		case MemcardCommand::SET_ERASE_SECTOR:
		case MemcardCommand::SET_WRITE_SECTOR:
		case MemcardCommand::SET_READ_SECTOR:
			g_MemoryCardProtocol.SetSector();
			break;
		case MemcardCommand::GET_SPECS:
			g_MemoryCardProtocol.GetSpecs();
			break;
		case MemcardCommand::SET_TERMINATOR:
			g_MemoryCardProtocol.SetTerminator();
			break;
		case MemcardCommand::GET_TERMINATOR:
			g_MemoryCardProtocol.GetTerminator();
			break;
		case MemcardCommand::WRITE_DATA:
			g_MemoryCardProtocol.WriteData();
			break;
		case MemcardCommand::READ_DATA:
			g_MemoryCardProtocol.ReadData();
			break;
		case MemcardCommand::PS1_READ:
			g_MemoryCardProtocol.ResetPS1State();

			while (!g_Sio2FifoIn.empty())
			{
				ps1Input = g_Sio2FifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Read(ps1Input);
				g_Sio2FifoIn.pop_front();
				g_Sio2FifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_STATE:
			g_MemoryCardProtocol.ResetPS1State();

			while (!g_Sio2FifoIn.empty())
			{
				ps1Input = g_Sio2FifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1State(ps1Input);
				g_Sio2FifoIn.pop_front();
				g_Sio2FifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_WRITE:
			g_MemoryCardProtocol.ResetPS1State();

			while (!g_Sio2FifoIn.empty())
			{
				ps1Input = g_Sio2FifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Write(ps1Input);
				g_Sio2FifoIn.pop_front();
				g_Sio2FifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::PS1_POCKETSTATION:
			g_MemoryCardProtocol.ResetPS1State();

			while (!g_Sio2FifoIn.empty())
			{
				ps1Input = g_Sio2FifoIn.front();
				ps1Output = g_MemoryCardProtocol.PS1Pocketstation(ps1Input);
				g_Sio2FifoIn.pop_front();
				g_Sio2FifoOut.push_back(ps1Output);
			}

			break;
		case MemcardCommand::READ_WRITE_END:
			g_MemoryCardProtocol.ReadWriteEnd();
			break;
		case MemcardCommand::ERASE_BLOCK:
			g_MemoryCardProtocol.EraseBlock();
			break;
		case MemcardCommand::UNKNOWN_BOOT:
			g_MemoryCardProtocol.UnknownBoot();
			break;
		case MemcardCommand::AUTH_XOR:
			g_MemoryCardProtocol.AuthXor();
			break;
		case MemcardCommand::AUTH_F3:
			g_MemoryCardProtocol.AuthF3();
			break;
		case MemcardCommand::AUTH_F7:
			g_MemoryCardProtocol.AuthF7();
			break;
		default:
			Console.Warning("%s() Unhandled memcard command %02X, things are about to break!", __FUNCTION__, commandByte);
			break;
	}
}

void Sio2::Write(u8 data)
{
	Sio2Log.WriteLn("%s(%02X) SIO2 DATA Write", __FUNCTION__, data);

	if (!queueRead)
	{
		if (queuePosition > CmdQueue.size())
		{
			Console.Warning("%s(%02X) Received data after exhausting all queue entries!", __FUNCTION__, data);
			return;
		}

		const u32 currentCmd = CmdQueue[queuePosition];
		port = currentCmd & Sio2Cmd::PORT;
		commandLength = (currentCmd >> 8) & Sio2Cmd::COMMAND_LENGTH_MASK;
		queueRead = true;

		if (commandLength == 0)
		{
			queueComplete = true;
		}

		while (!g_Sio2FifoIn.empty())
		{
			g_Sio2FifoIn.pop_front();
		}
	}

	if (queueComplete)
	{
		return;
	}

	g_Sio2FifoIn.push_back(data);

	if ((g_Sio2FifoIn.size() == g_Sio2.commandLength && g_Sio2.dmaBlockSize == 0) || g_Sio2FifoIn.size() == g_Sio2.dmaBlockSize)
	{
		g_Sio2.queueRead = false;
		g_Sio2.queuePosition++;

		const u8 sioMode = g_Sio2FifoIn.front();
		g_Sio2FifoIn.pop_front();

		switch (sioMode)
		{
			case SioMode::PAD:
				this->Pad();
				break;
			case SioMode::MULTITAP:
				this->Multitap();
				break;
			case SioMode::INFRARED:
				this->Infrared();
				break;
			case SioMode::MEMCARD:
				this->Memcard();
				break;
			default:
				Console.Error("%s(%02X) Unhandled SIO mode %02X", __FUNCTION__, data, sioMode);
				g_Sio2FifoOut.push_back(0xff);
				SetCmdStat(CmdStat::DISCONNECTED);
				break;
		}

		if (g_Sio2.dmaBlockSize > 0)
		{
			const size_t dmaDiff = g_Sio2FifoOut.size() % g_Sio2.dmaBlockSize;

			if (dmaDiff > 0)
			{
				const size_t padding = g_Sio2.dmaBlockSize - dmaDiff;

				for (size_t i = 0; i < padding; i++)
				{
					g_Sio2FifoOut.push_back(0x00);
				}
			}
		}
	}
}

u8 Sio2::Read()
{
	u8 ret = 0xff;

	if (!g_Sio2FifoOut.empty())
	{
		ret = g_Sio2FifoOut.front();
		g_Sio2FifoOut.pop_front();
	}
	else
	{
		Console.Warning("%s() g_Sio2FifoOut underflow! Returning 0xff.", __FUNCTION__);
	}

	Sio2Log.WriteLn("%s() SIO2 DATA Read (%02X)", __FUNCTION__, ret);
	return ret;
}

bool Sio2::DoState(StateWrapper& sw)
{
	if (!sw.DoMarker("Sio2"))
		return false;

	sw.Do(&CmdQueue);
	sw.Do(&PortCtrl0);
	sw.Do(&PortCtrl1);
	sw.Do(&dataIn);
	sw.Do(&dataOut);
	sw.Do(&ctrl);
	sw.Do(&CmdStat);
	sw.Do(&PortStat);
	sw.Do(&FifoStat);
	sw.Do(&FifoTxPos);
	sw.Do(&FifoRxPos);
	sw.Do(&iStat);
	sw.Do(&port);
	sw.Do(&queueRead);
	sw.Do(&queuePosition);
	sw.Do(&commandLength);
	sw.Do(&processedLength);
	sw.Do(&dmaBlockSize);
	sw.Do(&queueComplete);

	sw.Do(&g_Sio2FifoIn);
	sw.Do(&g_Sio2FifoOut);

	u64 mcdCrcs[SIO::PORTS][SIO::SLOTS];
	if (sw.IsWriting())
	{
		for (u32 port = 0; port < SIO::PORTS; port++)
		{
			for (u32 slot = 0; slot < SIO::SLOTS; slot++)
				mcdCrcs[port][slot] = mcds[port][slot].GetChecksum();
		}
	}
	sw.DoBytes(mcdCrcs, sizeof(mcdCrcs));

	if (sw.IsReading())
	{
		bool ejected = false;
		for (u32 port = 0; port < SIO::PORTS && !ejected; port++)
		{
			for (u32 slot = 0; slot < SIO::SLOTS; slot++)
			{
				if (mcdCrcs[port][slot] != mcds[port][slot].GetChecksum())
				{
					AutoEject::SetAll();
					ejected = true;
					break;
				}
			}
		}
	}

	sw.Do(&sioLastFrameMcdBusy);
	return sw.IsGood();
}
