// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winsock2.h>
#include <iphlpapi.h>
#elif defined(__POSIX__)
#include <sys/types.h>
#include <ifaddrs.h>
#endif

#include <string>
#include <memory>
#include <optional>
#include <vector>

#include "DEV9/PacketReader/MAC_Address.h"
#include "DEV9/PacketReader/IP/IP_Address.h"

namespace AdapterUtils
{
#ifdef _WIN32
	typedef IP_ADAPTER_ADDRESSES Adapter;
	typedef std::unique_ptr<std::byte[]> AdapterBuffer;
#elif defined(__POSIX__)
	typedef ifaddrs Adapter;
	struct IfAdaptersDeleter
	{
		void operator()(ifaddrs* buffer) const { freeifaddrs(buffer); }
	};
	typedef std::unique_ptr<ifaddrs, IfAdaptersDeleter> AdapterBuffer;
#endif

	u16 ReadAddressFamily(const sockaddr* unknownAddr);

#ifdef _WIN32
	Adapter* GetAllAdapters(AdapterBuffer* buffer, bool includeHidden = false);
#elif defined(__POSIX__)
	Adapter* GetAllAdapters(AdapterBuffer* buffer);
#endif
	bool GetAdapter(const std::string& name, Adapter* adapter, AdapterBuffer* buffer);
	bool GetAdapterAuto(Adapter* adapter, AdapterBuffer* buffer);

	std::optional<PacketReader::MAC_Address> GetAdapterMAC(const Adapter* adapter);
	std::optional<PacketReader::IP::IP_Address> GetAdapterIP(const Adapter* adapter);
	std::vector<PacketReader::IP::IP_Address> GetGateways(const Adapter* adapter);
	std::vector<PacketReader::IP::IP_Address> GetDNS(const Adapter* adapter);
};
