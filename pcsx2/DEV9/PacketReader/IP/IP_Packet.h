// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "DEV9/PacketReader/Payload.h"
#include "IP_Address.h"
#include "IP_Options.h"
#include "IP_Payload.h"

#include "common/Pcsx2Defs.h"

#include <vector>

namespace PacketReader::IP
{
	enum struct IP_Type : u8
	{
		ICMP = 0x01,
		IGMP = 0x02,
		TCP = 0x06,
		UDP = 0x11
	};

	class IP_Packet : public Payload
	{
	private:
		const u8 _verHi = 4 << 4;
		int headerLength = 20;

		u8 dscp = 0;

		u16 id = 0;
	private:
		u8 fragmentFlags1 = 0;
		u8 fragmentFlags2 = 0;
	public:
		u8 timeToLive = 0;
		u8 protocol;

	private:
		u16 checksum;

	public:
		IP_Address sourceIP{};
		IP_Address destinationIP{};
		std::vector<IPOption*> options;

	private:
		std::unique_ptr<IP_Payload> payload;

	public:
		int GetHeaderLength() const;

		u8 GetDscpValue() const;
		void SetDscpValue(u8 value);

		u8 GetDscpECN() const;
		void SetDscpECN(u8 value);

		bool GetDoNotFragment() const;
		void SetDoNotFragment(bool value);

		bool GetMoreFragments() const;
		void SetMoreFragments(bool value);

		u16 GetFragmentOffset() const;

		IP_Packet(IP_Payload* data);
		IP_Packet(const u8* buffer, int bufferSize, bool fromICMP = false);
		IP_Packet(const IP_Packet&);

		IP_Payload* GetPayload() const;

		virtual int GetLength();
		virtual void WriteBytes(u8* buffer, int* offset);
		virtual IP_Packet* Clone() const;

		bool VerifyChecksum();
		static u16 InternetChecksum(const u8* buffer, int length);

		~IP_Packet();

	private:
		void ReComputeHeaderLen();
		void CalculateChecksum();
	};
}
