// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

enum class SioStage
{
	IDLE,
	WAITING_COMMAND,
	WORKING
};

namespace SioMode
{
	static constexpr u8 NOT_SET = 0x00;
	static constexpr u8 PAD = 0x01;
	static constexpr u8 MULTITAP = 0x21;
	static constexpr u8 INFRARED = 0x61;
	static constexpr u8 MEMCARD = 0x81;
}

namespace MemcardCommand
{
	static constexpr u8 NOT_SET = 0x00;
	static constexpr u8 PROBE = 0x11;
	static constexpr u8 UNKNOWN_WRITE_DELETE_END = 0x12;
	static constexpr u8 SET_ERASE_SECTOR = 0x21;
	static constexpr u8 SET_WRITE_SECTOR = 0x22;
	static constexpr u8 SET_READ_SECTOR = 0x23;
	static constexpr u8 GET_SPECS = 0x26;
	static constexpr u8 SET_TERMINATOR = 0x27;
	static constexpr u8 GET_TERMINATOR = 0x28;
	static constexpr u8 WRITE_DATA = 0x42;
	static constexpr u8 READ_DATA = 0x43;
	static constexpr u8 PS1_READ = 0x52;
	static constexpr u8 PS1_STATE = 0x53;
	static constexpr u8 PS1_WRITE = 0x57;
	static constexpr u8 PS1_POCKETSTATION = 0x58;
	static constexpr u8 READ_WRITE_END = 0x81;
	static constexpr u8 ERASE_BLOCK = 0x82;
	static constexpr u8 UNKNOWN_BOOT = 0xbf;
	static constexpr u8 AUTH_XOR = 0xf0;
	static constexpr u8 AUTH_F3 = 0xf3;
	static constexpr u8 AUTH_F7 = 0xf7;
}

enum class Sio0Interrupt
{
	TEST_EVENT,
	STAT_READ,
	TX_DATA_WRITE
};

namespace SIO
{
	static constexpr u8 PORTS = 2;
	static constexpr u8 SLOTS = 4;
}

namespace SIO0_STAT
{
	static constexpr u32 TX_READY = 0x01;
	static constexpr u32 RX_FIFO_NOT_EMPTY = 0x02;
	static constexpr u32 TX_EMPTY = 0x04;
	static constexpr u32 RX_PARITY_ERROR = 0x08;
	static constexpr u32 ACK = 0x80;
	static constexpr u32 IRQ = 0x0200;
}

namespace SIO0_CTRL
{
	static constexpr u16 TX_ENABLE = 0x01;
	static constexpr u16 RX_ENABLE = 0x04;
	static constexpr u16 ACK = 0x10;
	static constexpr u16 RESET = 0x40;
	static constexpr u16 RX_INT_MODE_LSB = 0x0100;
	static constexpr u16 RX_INT_MODE_MSB = 0x0200;
	static constexpr u16 TX_INT_ENABLE = 0x0400;
	static constexpr u16 RX_INT_ENABLE = 0x0800;
	static constexpr u16 ACK_INT_ENABLE = 0x1000;
	static constexpr u16 PORT = 0x2000;
}

namespace Sio2Cmd
{
	static constexpr u32 PORT = 0x01;
	static constexpr u16 COMMAND_LENGTH_MASK = 0x3ff;
}

namespace Sio2Ctrl
{
	static constexpr u32 START_TRANSFER = 0x1;
	static constexpr u32 RESET = 0xc;
	static constexpr u32 PORT = 0x2000;
	static constexpr u32 SIO2MAN_RESET = 0x000003bc;
}

namespace CmdStat
{
	static constexpr u32 DISCONNECTED = 0x1d100;
	static constexpr u32 CONNECTED = 0x1100;

	static constexpr u32 NO_DEVICES_MISSING = 0x1000;
	static constexpr u32 PORT_1_MISSING = 0x1D000;
	static constexpr u32 PORT_2_MISSING = 0x2D000;
	static constexpr u32 BOTH_PORTS_MISSING = 0x3D000;
	static constexpr u32 ONE_PORT_OPEN = 0x100;
	static constexpr u32 TWO_PORTS_OPEN = 0x200;

}

namespace PortStat
{
	static constexpr u32 DEFAULT = 0xf;
}

namespace FifoStat
{
	static constexpr u32 DEFAULT = 0x0;
	static constexpr u32 SPECS = 0x83;
	static constexpr u32 TERMINATOR = 0x8b;
	static constexpr u32 READ_WRITE_END = 0x8c;
}

namespace Terminator
{
	static constexpr u32 NOT_READY = 0x66;
	static constexpr u32 READY = 0x55;
}
