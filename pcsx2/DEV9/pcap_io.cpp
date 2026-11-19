// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include <algorithm>
#include <memory>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"
#include <WinSock2.h>
#include <iphlpapi.h>
#endif

#include <stdio.h>
#include "pcap_io.h"
#include "DEV9.h"
#include "AdapterUtils.h"
#include "net.h"
#include "PacketReader/EthernetFrame.h"
#include "PacketReader/EthernetFrameEditor.h"
#include "PacketReader/ARP/ARP_PacketEditor.h"

#ifdef ENABLE_VR
#include "common/Console.h"
#include "fmt/format.h"
#include <chrono>
#endif
#ifndef PCAP_NETMASK_UNKNOWN
#define PCAP_NETMASK_UNKNOWN 0xffffffff
#endif

#ifdef _WIN32
#define PCAPPREFIX "\\Device\\NPF_"
#endif

using namespace PacketReader;
using namespace PacketReader::ARP;
using namespace PacketReader::IP;

PCAPAdapter::PCAPAdapter()
	: NetAdapter()
{
	if (!EmuConfig.DEV9.EthEnable)
		return;
#ifdef _WIN32
	if (!load_pcap())
		return;
#endif

#ifdef _WIN32
	std::string pcapAdapter = PCAPPREFIX + EmuConfig.DEV9.EthDevice;
#else
	std::string pcapAdapter = EmuConfig.DEV9.EthDevice;
#endif

	switched = EmuConfig.DEV9.EthApi == Pcsx2Config::DEV9Options::NetApi::PCAP_Switched;

	if (!InitPCAP(pcapAdapter, switched))
	{
		Console.Error("DEV9: Can't open Device '%s'", EmuConfig.DEV9.EthDevice.c_str());
		return;
	}

	AdapterUtils::Adapter adapter;
	AdapterUtils::AdapterBuffer buffer;
	std::optional<MAC_Address> adMAC = std::nullopt;
	const bool foundAdapter = AdapterUtils::GetAdapter(EmuConfig.DEV9.EthDevice, &adapter, &buffer);
	if (foundAdapter)
		adMAC = AdapterUtils::GetAdapterMAC(&adapter);
	else
		Console.Error("DEV9: Failed to get adapter information");

	if (adMAC.has_value())
	{
		hostMAC = adMAC.value();
		MAC_Address newMAC = ps2MAC;

		newMAC.bytes[5] = hostMAC.bytes[4];
		newMAC.bytes[4] = hostMAC.bytes[5];

		SetMACAddress(&newMAC);
	}
	else
	{
		Console.Error("DEV9: Failed to get MAC address for adapter");
		pcap_close(hpcap);
		hpcap = nullptr;
		return;
	}

#ifdef ENABLE_VR
	if (!switched)
		Console.WriteLn("DEV9: (Netplay) the mod-list exchange runs in PCAP Switched mode only: off in this mode.");
#endif

	if (switched && !SetMACSwitchedFilter(ps2MAC))
	{
		pcap_close(hpcap);
		hpcap = nullptr;
		Console.Error("DEV9: Can't open Device '%s'", EmuConfig.DEV9.EthDevice.c_str());
		return;
	}

	if (foundAdapter)
		InitInternalServer(&adapter);
	else
		InitInternalServer(nullptr);
}
AdapterOptions PCAPAdapter::GetAdapterOptions()
{
	return AdapterOptions::None;
}
bool PCAPAdapter::blocks()
{
	pxAssert(hpcap);
	return blocking;
}
bool PCAPAdapter::isInitialised()
{
	return hpcap != nullptr;
}
bool PCAPAdapter::recv(NetPacket* pkt)
{
	pxAssert(hpcap);

	if (!blocking && NetAdapter::recv(pkt))
		return true;

	while (RecvPCAPPacket(pkt))
	{
		if (!switched)
			SetMACBridgedRecv(pkt);

		if (VerifyPkt(pkt, pkt->size))
		{
			HandleFrameCheckSequence(pkt);

			if (pkt->size > 1514)
			{
				Console.Error("DEV9: Dropped jumbo frame of size: %u", pkt->size);
				continue;
			}

#ifdef ENABLE_VR
			if (switched && VR::Netplay::IsRecordFrame(reinterpret_cast<const u8*>(pkt->buffer), static_cast<size_t>(pkt->size)))
			{
				NetplayRecv(pkt);
				continue;
			}
			if (switched && pkt->size >= 14)
			{
				VR::Netplay::MAC src;
				std::copy(pkt->buffer + 6, pkt->buffer + 12, src.begin());
				const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count());
				netplayPeers.OnHeard(src, now);
			}
#endif

			InspectRecv(pkt);
			return true;
		}
	}

	return false;
}
bool PCAPAdapter::send(NetPacket* pkt)
{
	pxAssert(hpcap);

	InspectSend(pkt);
	if (NetAdapter::send(pkt))
		return true;

#ifdef ENABLE_VR
	if (switched)
		NetplaySend(pkt);
#endif

	if (!switched)
		SetMACBridgedSend(pkt);

	return SendPCAPPacket(pkt);
}

bool PCAPAdapter::RecvPCAPPacket(NetPacket* pkt)
{
	pcap_pkthdr* header;
	const u_char* pkt_data;

	while (pcap_next_ex(hpcap, &header, &pkt_data) > 0)
	{
		if (header->len > 1518)
		{
			Console.Error("DEV9: Dropped jumbo frame of size: %u", header->len);
			continue;
		}

		pxAssert(header->len == header->caplen);

		memcpy(pkt->buffer, pkt_data, header->len);
		pkt->size = static_cast<int>(header->len);
		return true;
	}

	return false;
}

bool PCAPAdapter::SendPCAPPacket(const NetPacket* pkt)
{
	return pcap_sendpacket(hpcap, reinterpret_cast<const u_char*>(pkt->buffer), pkt->size) == 0;
}

#ifdef ENABLE_VR
namespace
{
	VR::Netplay::MAC ToMAC(const MAC_Address& m)
	{
		VR::Netplay::MAC out;
		std::copy(std::begin(m.bytes), std::end(m.bytes), out.begin());
		return out;
	}

	u64 NowMs()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	bool IsPublicIPv4(const u8* ip)
	{
		if (ip[0] == 0 || ip[0] == 10 || ip[0] == 127 || ip[0] >= 224)
			return false;
		if (ip[0] == 172 && (ip[1] & 0xF0) == 16)
			return false;
		if (ip[0] == 192 && ip[1] == 168)
			return false;
		if (ip[0] == 169 && ip[1] == 254)
			return false;
		if (ip[0] == 100 && (ip[1] & 0xC0) == 64)
			return false;
		return true;
	}

	std::string ModsMissing(const VR::Netplay::Record& r, std::string_view mode)
	{
		std::string out;
		for (const VR::Netplay::ModLine& m : r.mods)
		{
			if (m.runs_in.find(mode) == std::string::npos)
				out += out.empty() ? m.id : fmt::format(", {}", m.id);
		}
		return out;
	}
}

void PCAPAdapter::NetplaySendRecord(const VR::Netplay::MAC& to)
{
	VR::Netplay::Record ours = VR::Netplay::GetLocalRecord();
	if (ours.serial.empty())
		ours.serial = "-";
	ours.session = netplaySession;
	ours.seen = netplayPeers.SeenSession(to);
	const std::vector<u8> frame = VR::Netplay::BuildFrame(to, ToMAC(ps2MAC), ours);
	NetPacket out;
	out.size = static_cast<int>(frame.size());
	std::memcpy(out.buffer, frame.data(), frame.size());
	const bool sent = SendPCAPPacket(&out);
	netplayPeers.MarkSent(to, sent, NowMs());
	Console.WriteLnFmt("DEV9: (Netplay) mod list {} to {}: {} {:08X}, {} mod(s), session {}.", sent ? "sent" : "NOT sent (pcap error; tried again on the next frame)",
		VR::Netplay::FormatMAC(to), ours.serial, ours.crc, ours.mods.size(), netplaySession);
}

void PCAPAdapter::NetplaySend(const NetPacket* pkt)
{
	if (pkt->size < 34)
		return;
	const u8* f = reinterpret_cast<const u8*>(pkt->buffer);
	VR::Netplay::MAC dst;
	std::copy(f, f + 6, dst.begin());

	if (f[12] == 0x08 && f[13] == 0x00 && IsPublicIPv4(f + 30) && !netplayOnlineSaid.exchange(true))
	{
		const std::string missing = ModsMissing(VR::Netplay::GetLocalRecord(), "online");
		Console.WriteLnFmt("DEV9: (Netplay) the game sent to a public address ({}.{}.{}.{}): online play (or a public DNS "
						   "server in the network settings). {}",
			f[30], f[31], f[32], f[33],
			missing.empty() ? std::string("Every applied mod lists online.") :
							  fmt::format("Applied mods whose runs_in does not list online: {}.", missing));
	}

	for (const VR::Netplay::MAC& to : netplayPeers.OnGuestSend(dst, ToMAC(ps2MAC), ToMAC(internalMAC), ToMAC(hostMAC), NowMs()))
		NetplaySendRecord(to);
	for (const VR::Netplay::MAC& m : netplayPeers.Overdue(NowMs()))
	{
		Console.WriteLnFmt("DEV9: (Netplay) no mod list from {} in {} s (sent twice): stock PCSX2, a real PS2, a router or "
						   "server, or not PenguinScreen2.",
			VR::Netplay::FormatMAC(m), VR::Netplay::kAnswerWaitMs / 1000);
		for (const VR::Netplay::ModLine& l : VR::Netplay::GetLocalRecord().mods)
		{
			if (l.runs_in.find("both-must-run") != std::string::npos)
				Console.WarningFmt("DEV9: (Netplay) MUST RUN ON BOTH MACHINES: {} is on here and {} sent no mod list.", l.id,
					VR::Netplay::FormatMAC(m));
		}
	}
}

void PCAPAdapter::NetplayRecv(const NetPacket* pkt)
{
	VR::Netplay::MAC src;
	VR::Netplay::Record theirs;
	const u8* f = reinterpret_cast<const u8*>(pkt->buffer);
	if (!VR::Netplay::ParseFrame(f, static_cast<size_t>(pkt->size), &src, &theirs))
	{
		std::copy(f + 6, f + 12, src.begin());
		if (netplayPeers.OnUnreadable(src, NowMs()))
			Console.WriteLnFmt("DEV9: (Netplay) a mod-list record from {} that this build cannot read (another format?); "
							   "dropped. It counts as PenguinScreen2.",
				VR::Netplay::FormatMAC(src));
		return;
	}
	const VR::Netplay::PeerTable::Heard h = netplayPeers.OnRecord(src, theirs.session, theirs.seen, netplaySession, NowMs());
	if (!h.log_full)
		return;
	VR::Netplay::Record ours = VR::Netplay::GetLocalRecord();
	if (ours.serial.empty())
		ours.serial = "-";
	ours.build = ours.build.empty() ? std::string("-") : ours.build;
	Console.WriteLnFmt("DEV9: (Netplay) mod list from {} (session {}): {} {:08X}, build {}, {} mod(s).", VR::Netplay::FormatMAC(src),
		theirs.session, theirs.serial, theirs.crc, theirs.build, theirs.mods.size());
	for (const VR::Netplay::ModLine& m : theirs.mods)
		Console.WriteLnFmt("DEV9: (Netplay)   theirs: {} [{}]", m.id, m.runs_in);
	for (const VR::Netplay::ModLine& m : ours.mods)
		Console.WriteLnFmt("DEV9: (Netplay)   ours:   {} [{}]", m.id, m.runs_in);
	const std::vector<std::string> diff = VR::Netplay::Compare(ours, theirs);
	if (diff.empty())
		Console.WriteLnFmt("DEV9: (Netplay)   the two lists agree.");
	for (const std::string& d : diff)
		Console.WriteLnFmt("DEV9: (Netplay)   differs: {}", d);
	const std::string missing = ModsMissing(ours, "lan");
	if (!missing.empty())
		Console.WriteLnFmt("DEV9: (Netplay)   a LAN peer is talking to this game; applied mods whose runs_in does not "
						   "list lan: {}.",
			missing);
}
#endif

void PCAPAdapter::reloadSettings()
{
	AdapterUtils::Adapter adapter;
	AdapterUtils::AdapterBuffer buffer;
	if (AdapterUtils::GetAdapter(EmuConfig.DEV9.EthDevice, &adapter, &buffer))
		ReloadInternalServer(&adapter);
	else
		ReloadInternalServer(nullptr);
}

PCAPAdapter::~PCAPAdapter()
{
	if (hpcap)
	{
		pcap_close(hpcap);
		hpcap = nullptr;
	}
}

std::vector<AdapterEntry> PCAPAdapter::GetAdapters()
{
	std::vector<AdapterEntry> nic;

#ifdef _WIN32
	if (!load_pcap())
		return nic;
#endif

	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_if_t* alldevs;
	pcap_if_t* d;

	if (pcap_findalldevs(&alldevs, errbuf) == -1)
		return nic;

	d = alldevs;
	while (d != NULL)
	{
		AdapterEntry entry;
		entry.type = Pcsx2Config::DEV9Options::NetApi::PCAP_Switched;
#ifdef _WIN32
		if (!std::string_view(d->name).starts_with(PCAPPREFIX))
		{
			Console.Error("PCAP: Unexpected Device: ", d->name);
			d = d->next;
			continue;
		}

		entry.guid = std::string(&d->name[strlen(PCAPPREFIX)]);

		IP_ADAPTER_ADDRESSES adapterInfo;
		AdapterUtils::AdapterBuffer buffer;

		if (AdapterUtils::GetAdapter(entry.guid, &adapterInfo, &buffer))
			entry.name = StringUtil::WideStringToUTF8String(std::wstring(adapterInfo.FriendlyName));
		else
		{
			const int len_desc = strlen(d->description) + 1;
			const int len_buf = MultiByteToWideChar(CP_ACP, 0, d->description, len_desc, nullptr, 0);

			std::unique_ptr<wchar_t[]> buf = std::make_unique<wchar_t[]>(len_buf);
			MultiByteToWideChar(CP_ACP, 0, d->description, len_desc, buf.get(), len_buf);

			entry.name = StringUtil::WideStringToUTF8String(std::wstring(buf.get()));
		}
#else
		entry.name = std::string(d->name);
		entry.guid = std::string(d->name);
#endif

		nic.push_back(entry);
		entry.type = Pcsx2Config::DEV9Options::NetApi::PCAP_Bridged;
		nic.push_back(entry);
		d = d->next;
	}

	return nic;
}

bool PCAPAdapter::InitPCAP(const std::string& adapter, bool promiscuous)
{
	char errbuf[PCAP_ERRBUF_SIZE];
	Console.WriteLn("DEV9: Opening adapter '%s'...", adapter.c_str());

	if ((hpcap = pcap_open_live(adapter.c_str(),
			 65536,
			 promiscuous ? 1 : 0,
			 1,
			 errbuf
			 )) == nullptr)
	{
		Console.Error("DEV9: %s", errbuf);
#if defined(__linux__)
		Console.Error("DEV9: PCAP on Linux requires CAP_NET_RAW and CAP_NET_ADMIN capabilities.");
		Console.Error("DEV9: Flatpak: raw packet capture is not supported; use the Sockets backend instead.");
		Console.Error("DEV9: AppImage: extract the image first (--appimage-extract), then run setcap on the extracted binary.");
		Console.Error("DEV9: Note: applying file capabilities to the main PCSX2 executable breaks Steam overlay/Input.");
#endif
		Console.Error("DEV9: Unable to open the adapter. %s is not supported by pcap", adapter.c_str());
		return false;
	}

	if (pcap_setnonblock(hpcap, 1, errbuf) == -1)
	{
		Console.Error("DEV9: Error setting non-blocking: %s", pcap_geterr(hpcap));
		Console.Error("DEV9: Continuing in blocking mode");
		blocking = true;
	}
	else
		blocking = false;

	const int dlt = pcap_datalink(hpcap);
	const char* dlt_name = pcap_datalink_val_to_name(dlt);

	Console.WriteLn("DEV9: Device uses DLT %d: %s", dlt, dlt_name);
	switch (dlt)
	{
		case DLT_EN10MB:
			break;
		default:
			Console.Error("ERROR: Unsupported DataLink Type (%d): %s", dlt, dlt_name);
			pcap_close(hpcap);
			hpcap = nullptr;
			return false;
	}

	Console.WriteLn("DEV9: Adapter Ok.");
	return true;
}

bool PCAPAdapter::SetMACSwitchedFilter(MAC_Address mac)
{
	bpf_program fp;

	char filter[128];
	std::snprintf(filter, std::size(filter), "ether broadcast or ether dst %.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
		mac.bytes[0], mac.bytes[1], mac.bytes[2], mac.bytes[3], mac.bytes[4], mac.bytes[5]);

	if (pcap_compile(hpcap, &fp, filter, 1, PCAP_NETMASK_UNKNOWN) == -1)
	{
		Console.Error("DEV9: Error calling pcap_compile: %s", pcap_geterr(hpcap));
		return false;
	}

	int setFilterRet;
	if ((setFilterRet = pcap_setfilter(hpcap, &fp)) == -1)
		Console.Error("DEV9: Error setting filter: %s", pcap_geterr(hpcap));

	pcap_freecode(&fp);
	return setFilterRet != -1;
}

void PCAPAdapter::SetMACBridgedRecv(NetPacket* pkt)
{
	EthernetFrameEditor frame(pkt);
	if (frame.GetProtocol() == static_cast<u16>(EtherType::IPv4))
	{
		PayloadPtrEditor* payload = frame.GetPayload();
		IP_Packet ippkt(payload->data, payload->GetLength());
		if (ippkt.destinationIP == ps2IP)
			frame.SetDestinationMAC(ps2MAC);
	}
	if (frame.GetProtocol() == static_cast<u16>(EtherType::ARP))
	{
		ARP_PacketEditor arpPkt(frame.GetPayload());
		if (*(IP_Address*)arpPkt.TargetProtocolAddress() == ps2IP)
		{
			frame.SetDestinationMAC(ps2MAC);
			*(MAC_Address*)arpPkt.TargetHardwareAddress() = ps2MAC;
		}
	}
}

void PCAPAdapter::SetMACBridgedSend(NetPacket* pkt)
{
	EthernetFrameEditor frame(pkt);
	if (frame.GetProtocol() == static_cast<u16>(EtherType::IPv4))
	{
		PayloadPtrEditor* payload = frame.GetPayload();
		IP_Packet ippkt(payload->data, payload->GetLength());
		ps2IP = ippkt.sourceIP;
	}
	if (frame.GetProtocol() == static_cast<u16>(EtherType::ARP))
	{
		ARP_PacketEditor arpPkt(frame.GetPayload());
		ps2IP = *(IP_Address*)arpPkt.SenderProtocolAddress();
		*(MAC_Address*)arpPkt.SenderHardwareAddress() = hostMAC;
	}
	frame.SetSourceMAC(hostMAC);
}

void PCAPAdapter::HandleFrameCheckSequence(NetPacket* pkt)
{
	EthernetFrameEditor frame(pkt);
	if (frame.GetSourceMAC() == hostMAC)
		return;

	int payloadSize = -1;
	if (frame.GetProtocol() == static_cast<u16>(EtherType::IPv4))
	{
		PayloadPtrEditor* payload = frame.GetPayload();
		IP_Packet ippkt(payload->data, payload->GetLength());
		payloadSize = ippkt.GetLength();
	}
	if (frame.GetProtocol() == static_cast<u16>(EtherType::ARP))
	{
		ARP_PacketEditor arpPkt(frame.GetPayload());
		payloadSize = arpPkt.GetLength();
	}

	if (payloadSize != -1)
	{
		payloadSize = std::min(std::max(payloadSize, 60 - frame.headerLength), pkt->size);

		pkt->size = payloadSize + frame.headerLength;
		return;
	}

	if (ValidateEtherFrame(pkt))
		pkt->size -= 4;
}

bool PCAPAdapter::ValidateEtherFrame(NetPacket* pkt)
{
	u32 crc = 0xFFFFFFFF;

	for (int i = 0; i < pkt->size; i++)
	{
		crc = crc ^ static_cast<u8>(pkt->buffer[i]);
		for (int bit = 0; bit < 8; bit++)
		{
			if ((crc & 1) != 0)
				crc = (crc >> 1) ^ 0xEDB88320;
			else
				crc = (crc >> 1);
		}
	}

	crc = ~crc;

	return crc == 0x2144DF1C;
}
