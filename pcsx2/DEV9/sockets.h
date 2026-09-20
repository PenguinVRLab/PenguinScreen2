// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <mutex>
#include <vector>

#include "net.h"

#include "PacketReader/IP/IP_Packet.h"
#include "PacketReader/EthernetFrame.h"
#include "Sessions/BaseSession.h"
#include "SimpleQueue.h"
#include "ThreadSafeMap.h"

class SocketAdapter : public NetAdapter
{
	SimpleQueue<PacketReader::EthernetFrame*> vRecBuffer;

#ifdef _WIN32
	bool wsa_init = false;
#endif
	bool initialized = false;
	PacketReader::IP::IP_Address adapterIP;

	ThreadSafeMap<Sessions::ConnectionKey, Sessions::BaseSession*> connections;
	ThreadSafeMap<u16, Sessions::BaseSession*> fixedUDPPorts;

	std::thread::id sendThreadId;
	std::vector<Sessions::BaseSession*> deleteQueueSendThread;
	std::vector<Sessions::BaseSession*> deleteQueueRecvThread;
	std::mutex deleteSendSentry;
	std::mutex deleteRecvSentry;

public:
	SocketAdapter();
	virtual bool blocks();
	virtual bool isInitialised();
	virtual bool recv(NetPacket* pkt);
	virtual bool send(NetPacket* pkt);
	virtual void reset();
	virtual void reloadSettings();
	virtual void close();
	virtual ~SocketAdapter();
	static std::vector<AdapterEntry> GetAdapters();
	static AdapterOptions GetAdapterOptions();

private:
	bool SendIP(PacketReader::IP::IP_Packet* ipPkt);
	bool SendICMP(Sessions::ConnectionKey Key, PacketReader::IP::IP_Packet* ipPkt);
	bool SendIGMP(Sessions::ConnectionKey Key, PacketReader::IP::IP_Packet* ipPkt);
	bool SendTCP(Sessions::ConnectionKey Key, PacketReader::IP::IP_Packet* ipPkt);
	bool SendUDP(Sessions::ConnectionKey Key, PacketReader::IP::IP_Packet* ipPkt);

	int SendFromConnection(Sessions::ConnectionKey Key, PacketReader::IP::IP_Packet* ipPkt);

	void HandleConnectionClosed(Sessions::BaseSession* sender);
	void HandleFixedPortClosed(Sessions::BaseSession* sender);
};
