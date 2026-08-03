// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <algorithm>
#include <bit>
#include <thread>

#ifdef __POSIX__
#define SOCKET_ERROR -1
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#endif

#include "TCP_Session.h"

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::TCP;

namespace Sessions
{
	bool TCP_Session::Send(PacketReader::IP::IP_Payload* payload)
	{
		IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(payload);
		TCP_Packet tcp(ipPayload->data, ipPayload->GetLength());

		if (destPort != 0)
		{
			if (!(tcp.destinationPort == destPort && tcp.sourcePort == srcPort))
			{
				Console.Error("DEV9: TCP: Packet invalid for current session (Duplicate key?)");
				return false;
			}
		}

		if (tcp.GetRST() == true)
		{
			if (client != INVALID_SOCKET)
				CloseSocket();
			else
				Console.Error("DEV9: TCP: Reset closed connection");
			state = TCP_State::CloseCompleted;
			RaiseEventConnectionClosed();
			return true;
		}

		switch (state)
		{
			case TCP_State::None:
				return SendConnect(&tcp);
			case TCP_State::SendingSYN_ACK:
				if (CheckRepeatSYNNumbers(&tcp) == NumCheckResult::Bad)
				{
					Console.Error("DEV9: TCP: Invalid repeated SYN (SendingSYN_ACK)");
					return false;
				}
				return true;
			case TCP_State::SentSYN_ACK:
				return SendConnected(&tcp);
			case TCP_State::Connected:
				if (tcp.GetFIN() == true)
					return CloseByPS2Stage1_2(&tcp);
				else
					return SendData(&tcp);

			case TCP_State::Closing_ClosedByPS2:
				return SendNoData(&tcp);
			case TCP_State::Closing_ClosedByPS2ThenRemote_WaitingForAck:
				return CloseByPS2Stage4(&tcp);

			case TCP_State::Closing_ClosedByRemote:
				if (tcp.GetFIN() == true)
					return CloseByRemoteStage3_4(&tcp);

				return SendData(&tcp);
			case TCP_State::Closing_ClosedByRemoteThenPS2_WaitingForAck:
				return CloseByRemoteStage2_ButAfter4(&tcp);
			case TCP_State::CloseCompleted:
				Console.Error("DEV9: TCP: Attempt to send to a closed TCP connection");
				return false;
			default:
				CloseByRemoteRST();
				Console.Error("DEV9: TCP: Invalid TCP state");
				return true;
		}
	}

	bool TCP_Session::SendConnect(TCP_Packet* tcp)
	{
		destPort = tcp->destinationPort;
		srcPort = tcp->sourcePort;

		if (tcp->GetSYN() == false)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt to send data to a non connected connection");
			return true;
		}
		expectedSeqNumber = tcp->sequenceNumber + 1;
		receivedPS2SeqNumbers.clear();
		for (int i = 0; i < receivedPS2SeqNumberCount; i++)
			receivedPS2SeqNumbers.push_back(tcp->sequenceNumber);

		ResetMyNumbers();

		for (size_t i = 0; i < tcp->options.size(); i++)
		{
			switch (tcp->options[i]->GetCode())
			{
				case 0:
				case 1:
					continue;
				case 2:
					maxSegmentSize = static_cast<TCPopMSS*>(tcp->options[i])->maxSegmentSize;
					break;
				case 3:
					windowScale = static_cast<TCPopWS*>(tcp->options[i])->windowScale;
					if (windowScale > 0)
						Console.Error("DEV9: TCP: Non-zero window scale option");
					break;
				case 8:
					lastRecivedTimeStamp = static_cast<TCPopTS*>(tcp->options[i])->senderTimeStamp;
					sendTimeStamps = true;
					timeStampStart = std::chrono::steady_clock::now();
					break;
				default:
					Console.Error("DEV9: TCP: Got unknown option %d", tcp->options[i]->GetCode());
					break;
			}
		}

		windowSize.store(tcp->windowSize << windowScale);

		CloseSocket();

		client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (client == INVALID_SOCKET)
		{
			Console.Error("DEV9: TCP: Failed to open socket. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif
			RaiseEventConnectionClosed();
			return false;
		}

		int ret;
		if (adapterIP.integer != 0)
		{
			sockaddr_in endpoint{};
			endpoint.sin_family = AF_INET;
			endpoint.sin_addr = std::bit_cast<in_addr>(adapterIP);

			ret = bind(client, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));

			if (ret != 0)
				Console.Error("DEV9: UDP: Failed to bind socket. Error: %d",
#ifdef _WIN32
					WSAGetLastError());
#elif defined(__POSIX__)
					errno);
#endif
		}

#ifdef _WIN32
		u_long blocking = 1;
		ret = ioctlsocket(client, FIONBIO, &blocking);
#elif defined(__POSIX__)
		int blocking = 1;
		ret = ioctl(client, FIONBIO, &blocking);
#endif

		if (ret != 0)
			Console.Error("DEV9: TCP: Failed to set non-blocking. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif


		constexpr int noDelay = true;
		ret = setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

		if (ret != 0)
			Console.Error("DEV9: TCP: Failed to set TCP_NODELAY. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		sockaddr_in endpoint{};
		endpoint.sin_family = AF_INET;
		endpoint.sin_addr = std::bit_cast<in_addr>(destIP);
		endpoint.sin_port = htons(destPort);

		ret = connect(client, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));

		if (ret != 0)
		{
#ifdef _WIN32
			const int err = WSAGetLastError();
			if (err != WSAEWOULDBLOCK)
#elif defined(__POSIX__)
			const int err = errno;
			if (err != EWOULDBLOCK && err != EINPROGRESS)
#endif
			{
				Console.Error("DEV9: TCP: Failed to connect socket. Error: %d", err);
				RaiseEventConnectionClosed();
				return false;
			}
		}

		state = TCP_State::SendingSYN_ACK;
		return true;
	}

	bool TCP_Session::SendConnected(TCP_Packet* tcp)
	{
		if (tcp->GetSYN() == true)
		{
			if (CheckRepeatSYNNumbers(tcp) == NumCheckResult::Bad)
			{
				CloseByRemoteRST();
				Console.Error("DEV9: TCP: Invalid repeated SYN (SentSYN_ACK)");
				return true;
			}
			return true;
		}
		const NumCheckResult Result = CheckNumbers(tcp);
		if (Result == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP numbers received");
			return true;
		}

		for (size_t i = 0; i < tcp->options.size(); i++)
		{
			switch (tcp->options[i]->GetCode())
			{
				case 0:
				case 1:
					continue;
				case 8:
					lastRecivedTimeStamp = static_cast<TCPopTS*>(tcp->options[i])->senderTimeStamp;
					break;
				default:
					Console.Error("DEV9: TCP: Got unknown option %d", tcp->options[i]->GetCode());
					break;
			}
		}
		state = TCP_State::Connected;
		return true;
	}

	bool TCP_Session::SendData(TCP_Packet* tcp)
	{
		if (tcp->GetSYN())
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt to connect to an existing connection");
			return true;
		}
		if (tcp->GetURG())
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Urgent data not supported");
			return true;
		}
		for (size_t i = 0; i < tcp->options.size(); i++)
		{
			switch (tcp->options[i]->GetCode())
			{
				case 0:
				case 1:
					continue;
				case 8:
					lastRecivedTimeStamp = static_cast<TCPopTS*>(tcp->options[i])->senderTimeStamp;
					break;
				default:
					Console.Error("DEV9: TCP: Got unknown option %d", tcp->options[i]->GetCode());
					break;
			}
		}

		windowSize.store(tcp->windowSize << windowScale);

		const NumCheckResult Result = CheckNumbers(tcp);

		if (Result == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP numbers received");
			return true;
		}
		if (tcp->GetPayload()->GetLength() != 0)
		{
			const int delta = GetDelta(expectedSeqNumber, tcp->sequenceNumber);
			pxAssert(delta >= 0);
			if (tcp->GetPayload()->GetLength() - delta > 0)
			{
				DevCon.WriteLn("DEV9: TCP: [PS2] Sending: %d bytes", tcp->GetPayload()->GetLength());

				receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
				receivedPS2SeqNumbers.push_back(expectedSeqNumber);

				int sent = 0;
				PayloadPtr* payload = static_cast<PayloadPtr*>(tcp->GetPayload());
				while (sent != payload->GetLength())
				{
					const int ret = send(client, reinterpret_cast<const char*>(&payload->data[sent]), payload->GetLength() - sent, 0);

					if (ret == SOCKET_ERROR)
					{
#ifdef _WIN32
						const int err = WSAGetLastError();
						if (err == WSAEWOULDBLOCK)
#elif defined(__POSIX__)
						const int err = errno;
						if (err == EWOULDBLOCK)
#endif
							std::this_thread::yield();
						else
						{
							CloseByRemoteRST();
							Console.Error("DEV9: TCP: Send error: %d", err);
							return true;
						}
					}
					else
						sent += ret;
				}

				expectedSeqNumber += tcp->GetPayload()->GetLength() - delta;
			}
			std::unique_ptr<TCP_Packet> ret = CreateBasePacket();
			ret->SetACK(true);

			PushRecvBuff(ReceivedPayload{destIP, std::move(ret)});
		}
		return true;
	}

	bool TCP_Session::SendNoData(TCP_Packet* tcp)
	{
		if (tcp->GetSYN() == true)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt to connect to an existing connection");
			return true;
		}
		for (size_t i = 0; i < tcp->options.size(); i++)
		{
			switch (tcp->options[i]->GetCode())
			{
				case 0:
				case 1:
					continue;
				case 8:
					lastRecivedTimeStamp = static_cast<TCPopTS*>(tcp->options[i])->senderTimeStamp;
					break;
				default:
					Console.Error("DEV9: TCP: Got Unknown Option %d", tcp->options[i]->GetCode());
					break;
			}
		}

		ValidateEmptyPacket(tcp);

		return true;
	}

	TCP_Session::NumCheckResult TCP_Session::CheckRepeatSYNNumbers(TCP_Packet* tcp)
	{

		if (tcp->sequenceNumber != expectedSeqNumber - 1)
		{
			Console.Error("DEV9: TCP: [PS2] Sent unexpected sequence number from repeated SYN packet, got %u expected %u", tcp->sequenceNumber, (expectedSeqNumber - 1));
			return NumCheckResult::Bad;
		}
		return NumCheckResult::OK;
	}

	TCP_Session::NumCheckResult TCP_Session::CheckNumbers(TCP_Packet* tcp, bool rejectOldSeq)
	{
		u32 seqNum;
		std::vector<u32> oldSeqNums;
		std::tie(seqNum, oldSeqNums) = GetAllMyNumbers();

		if (tcp->acknowledgementNumber != seqNum)
		{

			if (std::find(oldSeqNums.begin(), oldSeqNums.end(), tcp->acknowledgementNumber) == oldSeqNums.end())
			{
				Console.Error("DEV9: TCP: [PS2] Sent unexpected acknowledgement number, did not match old numbers, got %u expected %u", tcp->acknowledgementNumber, seqNum);
				return NumCheckResult::Bad;
			}
		}
		else
		{
			myNumberACKed.store(true);
		}

		UpdateReceivedAckNumber(tcp->acknowledgementNumber);

		if (tcp->sequenceNumber != expectedSeqNumber)
		{
			if (rejectOldSeq)
			{
				Console.Error("DEV9: TCP: [PS2] Sent unexpected sequence number, got %u expected %u", tcp->sequenceNumber, expectedSeqNumber);
				return NumCheckResult::Bad;
			} 
			else if (tcp->GetPayload()->GetLength() == 0)
			{
				Console.Error("DEV9: TCP: [PS2] Sent unexpected sequence number in a empty packet, got %u expected %u", tcp->sequenceNumber, expectedSeqNumber);
				return NumCheckResult::OldSeq;
			}
			else
			{
				if (std::find(receivedPS2SeqNumbers.begin(), receivedPS2SeqNumbers.end(), tcp->sequenceNumber) == receivedPS2SeqNumbers.end())
				{
					Console.Error("DEV9: TCP: [PS2] Sent outdated sequence number in an data packet, got %u expected %u", tcp->sequenceNumber, expectedSeqNumber);
					return NumCheckResult::OldSeq;
				}
				else
				{
					Console.Error("DEV9: TCP: [PS2] Sent unexpected sequence number in a data packet, got %u expected %u", tcp->sequenceNumber, expectedSeqNumber);
					return NumCheckResult::Bad;
				}
			}
		}

		return NumCheckResult::OK;
	}
	bool TCP_Session::ValidateEmptyPacket(TCP_Packet* tcp, bool ignoreOld)
	{
		const NumCheckResult ResultFIN = CheckNumbers(tcp, !ignoreOld);
		if (ResultFIN == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP numbers received");
			return true;
		}
		if (tcp->GetPayload()->GetLength() > 0)
		{
			const int delta = GetDelta(expectedSeqNumber, tcp->sequenceNumber);
			pxAssert(delta >= 0);
			if (delta >= tcp->GetPayload()->GetLength())
				return false;

			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Invalid packet, packet has data");
			return true;
		}
		return false;
	}

	bool TCP_Session::CloseByPS2Stage1_2(TCP_Packet* tcp)
	{

		if (ValidateEmptyPacket(tcp, false))
			return true;

		receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
		receivedPS2SeqNumbers.push_back(expectedSeqNumber);
		expectedSeqNumber += 1;

		state = TCP_State::Closing_ClosedByPS2;

		const int result = shutdown(client, SD_SEND);
		if (result == SOCKET_ERROR)
			Console.Error("DEV9: TCP: Shutdown SD_SEND error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		std::unique_ptr<TCP_Packet> ret = CreateBasePacket();

		ret->SetACK(true);
		PushRecvBuff(ReceivedPayload{destIP, std::move(ret)});

		return true;
	}

	bool TCP_Session::CloseByPS2Stage4(TCP_Packet* tcp)
	{

		if (ValidateEmptyPacket(tcp))
			return true;

		if (myNumberACKed.load())
		{
			CloseSocket();
			state = TCP_State::CloseCompleted;
			RaiseEventConnectionClosed();
		}

		return true;
	}

	bool TCP_Session::CloseByRemoteStage2_ButAfter4(TCP_Packet* tcp)
	{

		if (ValidateEmptyPacket(tcp))
			return true;

		if (myNumberACKed.load())
		{
			CloseSocket();
			state = TCP_State::CloseCompletedFlushBuffer;
		}
		return true;
	}

	bool TCP_Session::CloseByRemoteStage3_4(TCP_Packet* tcp)
	{

		if (ValidateEmptyPacket(tcp, false))
			return true;

		receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
		receivedPS2SeqNumbers.push_back(expectedSeqNumber);
		expectedSeqNumber += 1;

		const int result = shutdown(client, SD_SEND);
		if (result == SOCKET_ERROR)
			Console.Error("DEV9: TCP: Shutdown SD_SEND error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		std::unique_ptr<TCP_Packet> ret = CreateBasePacket();

		ret->SetACK(true);

		PushRecvBuff(ReceivedPayload{destIP, std::move(ret)});

		if (myNumberACKed.load())
		{
			CloseSocket();
			state = TCP_State::CloseCompletedFlushBuffer;
		}
		else
			state = TCP_State::Closing_ClosedByRemoteThenPS2_WaitingForAck;

		return true;
	}

	void TCP_Session::CloseByRemoteRST()
	{
		std::unique_ptr<TCP_Packet> reterr = CreateBasePacket();
		reterr->SetRST(true);
		PushRecvBuff(ReceivedPayload{destIP, std::move(reterr)});

		CloseSocket();
		state = TCP_State::CloseCompletedFlushBuffer;
	}
}
