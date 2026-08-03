// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <tuple>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#elif defined(__POSIX__)
#define INVALID_SOCKET -1
#endif

#include "DEV9/SimpleQueue.h"
#include "DEV9/Sessions/BaseSession.h"
#include "DEV9/PacketReader/IP/TCP/TCP_Packet.h"

namespace Sessions
{
	class TCP_Session : public BaseSession
	{
	private:
		enum struct TCP_State
		{
			None,
			SendingSYN_ACK,
			SentSYN_ACK,
			Connected,
			Closing_ClosedByPS2,
			Closing_ClosedByPS2ThenRemote_WaitingForAck,
			Closing_ClosedByRemote,
			Closing_ClosedByRemoteThenPS2_WaitingForAck,
			CloseCompletedFlushBuffer,
			CloseCompleted,
		};
		enum struct NumCheckResult
		{
			OK,
			OldSeq,
			Bad
		};

		SimpleQueue<ReceivedPayload> _recvBuff;

#ifdef _WIN32
		SOCKET client = INVALID_SOCKET;
#elif defined(__POSIX__)
		int client = INVALID_SOCKET;
#endif

		TCP_State state = TCP_State::None;

		u16 srcPort = 0;
		u16 destPort = 0;

		u16 maxSegmentSize = 1460;
		int windowScale = 0;
		std::atomic<int> windowSize{1460};

		u32 lastRecivedTimeStamp;
		std::chrono::steady_clock::time_point timeStampStart;
		bool sendTimeStamps = false;

		const int receivedPS2SeqNumberCount = 5;
		u32 expectedSeqNumber;
		std::vector<u32> receivedPS2SeqNumbers;

		std::mutex myNumberSentry;
		const int oldMyNumCount = 64;
		u32 _MySequenceNumber = 1;
		std::vector<u32> _OldMyNumbers;
		u32 _ReceivedAckNumber = 1;
		std::atomic<bool> myNumberACKed{true};

	public:
		TCP_Session(ConnectionKey parKey, PacketReader::IP::IP_Address parAdapterIP);

		virtual std::optional<ReceivedPayload> Recv();
		virtual bool Send(PacketReader::IP::IP_Payload* payload);
		virtual void Reset();

		virtual ~TCP_Session();

	private:
		void PushRecvBuff(ReceivedPayload tcp);
		std::optional<ReceivedPayload> PopRecvBuff();

		void IncrementMyNumber(u32 amount);
		void UpdateReceivedAckNumber(u32 ack);
		u32 GetMyNumber();
		u32 GetOutstandingSequenceLength();
		bool ShouldWaitForAck();
		std::tuple<u32, std::vector<u32>> GetAllMyNumbers();
		void ResetMyNumbers();

		NumCheckResult CheckRepeatSYNNumbers(PacketReader::IP::TCP::TCP_Packet* tcp);
		NumCheckResult CheckNumbers(PacketReader::IP::TCP::TCP_Packet* tcp, bool rejectOldSeq = false);
		s32 GetDelta(u32 a, u32 b);
		bool ValidateEmptyPacket(PacketReader::IP::TCP::TCP_Packet* tcp, bool ignoreOld = true);

		std::optional<ReceivedPayload> ConnectTCPComplete(bool success);
		bool SendConnect(PacketReader::IP::TCP::TCP_Packet* tcp);
		bool SendConnected(PacketReader::IP::TCP::TCP_Packet* tcp);

		bool SendData(PacketReader::IP::TCP::TCP_Packet* tcp);
		bool SendNoData(PacketReader::IP::TCP::TCP_Packet* tcp);

		bool CloseByPS2Stage1_2(PacketReader::IP::TCP::TCP_Packet* tcp);
		ReceivedPayload CloseByPS2Stage3();
		bool CloseByPS2Stage4(PacketReader::IP::TCP::TCP_Packet* tcp);

		ReceivedPayload CloseByRemoteStage1();
		bool CloseByRemoteStage2_ButAfter4(PacketReader::IP::TCP::TCP_Packet* tcp);
		bool CloseByRemoteStage3_4(PacketReader::IP::TCP::TCP_Packet* tcp);

		void CloseByRemoteRST();

		std::unique_ptr<PacketReader::IP::TCP::TCP_Packet> CreateBasePacket(PacketReader::PayloadData* data = nullptr);

		void CloseSocket();
	};
}
