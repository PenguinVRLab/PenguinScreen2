// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/RedtapeWindows.h"
#include "common/RedtapeWilCom.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <stdio.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>

#include <Netcfgx.h>
#include <devguid.h>

#include <tchar.h>
#include "tap.h"
#include "DEV9/DEV9.h"
#include <string>

#include <wil/com.h>
#include <wil/resource.h>

#include "DEV9/PacketReader/MAC_Address.h"
#include "DEV9/AdapterUtils.h"

#define TAP_CONTROL_CODE(request, method) \
	CTL_CODE(FILE_DEVICE_UNKNOWN, request, method, FILE_ANY_ACCESS)

// clang-format off
#define TAP_IOCTL_GET_MAC               TAP_CONTROL_CODE(1, METHOD_BUFFERED)
#define TAP_IOCTL_GET_VERSION           TAP_CONTROL_CODE(2, METHOD_BUFFERED)
#define TAP_IOCTL_GET_MTU               TAP_CONTROL_CODE(3, METHOD_BUFFERED)
#define TAP_IOCTL_GET_INFO              TAP_CONTROL_CODE(4, METHOD_BUFFERED)
#define TAP_IOCTL_CONFIG_POINT_TO_POINT TAP_CONTROL_CODE(5, METHOD_BUFFERED)
#define TAP_IOCTL_SET_MEDIA_STATUS      TAP_CONTROL_CODE(6, METHOD_BUFFERED)
#define TAP_IOCTL_CONFIG_DHCP_MASQ      TAP_CONTROL_CODE(7, METHOD_BUFFERED)
#define TAP_IOCTL_GET_LOG_LINE          TAP_CONTROL_CODE(8, METHOD_BUFFERED)
#define TAP_IOCTL_CONFIG_DHCP_SET_OPT   TAP_CONTROL_CODE(9, METHOD_BUFFERED)
// clang-format on

#define ADAPTER_KEY L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}"

#define NETWORK_CONNECTIONS_KEY L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}"

#define USERMODEDEVICEDIR "\\\\.\\Global\\"
#define TAPSUFFIX ".tap"

#define TAP_COMPONENT_ID "tap0901"

bool IsTAPDevice(const TCHAR* guid)
{
	wil::unique_hkey netcard_key;
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, ADAPTER_KEY, 0, KEY_READ, netcard_key.put()) != ERROR_SUCCESS)
		return false;

	int i = 0;
	for (;;)
	{
		TCHAR enum_name[256];
		TCHAR unit_string[256];
		TCHAR component_id_string[] = _T("ComponentId");
		TCHAR component_id[256];
		TCHAR net_cfg_instance_id_string[] = _T("NetCfgInstanceId");
		TCHAR net_cfg_instance_id[256];
		DWORD data_type;

		DWORD len = std::size(enum_name);
		LSTATUS status = RegEnumKeyEx(netcard_key.get(), i, enum_name, &len, nullptr, nullptr, nullptr, nullptr);

		if (status == ERROR_NO_MORE_ITEMS)
			break;
		else if (status != ERROR_SUCCESS)
			return false;

		_stprintf_s(unit_string, _T("%s\\%s"), ADAPTER_KEY, enum_name);

		wil::unique_hkey unit_key;
		status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, unit_string, 0, KEY_READ, unit_key.put());

		if (status != ERROR_SUCCESS)
		{
			return false;
		}
		else
		{
			len = sizeof(component_id);
			status = RegQueryValueEx(unit_key.get(), component_id_string, nullptr, &data_type,
				(LPBYTE)component_id, &len);

			if (!(status != ERROR_SUCCESS || data_type != REG_SZ))
			{
				len = sizeof(net_cfg_instance_id);
				status = RegQueryValueEx(unit_key.get(), net_cfg_instance_id_string, nullptr, &data_type,
					(LPBYTE)net_cfg_instance_id, &len);

				if (status == ERROR_SUCCESS && data_type == REG_SZ)
				{
					if ((!wcsncmp(component_id, L"tap", 3) || !wcsncmp(component_id, L"root\\tap", 8)) && !_tcscmp(net_cfg_instance_id, guid))
					{
						return true;
					}
				}
			}
		}
		++i;
	}

	return false;
}

std::vector<AdapterEntry> TAPAdapter::GetAdapters()
{
	std::vector<AdapterEntry> tap_nic;
	DWORD len;
	DWORD cSubKeys = 0;

	wil::unique_hkey control_net_key;
	LSTATUS status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, NETWORK_CONNECTIONS_KEY, 0, KEY_READ | KEY_QUERY_VALUE,
		control_net_key.put());

	if (status != ERROR_SUCCESS)
		return tap_nic;

	status = RegQueryInfoKey(control_net_key.get(), nullptr, nullptr, nullptr, &cSubKeys, nullptr, nullptr,
		nullptr, nullptr, nullptr, nullptr, nullptr);

	if (status != ERROR_SUCCESS)
		return tap_nic;

	for (DWORD i = 0; i < cSubKeys; i++)
	{
		TCHAR enum_name[256];
		TCHAR connection_string[256];
		TCHAR name_data[256];
		DWORD name_type;
		const TCHAR name_string[] = _T("Name");

		len = std::size(enum_name);
		status = RegEnumKeyEx(control_net_key.get(), i, enum_name, &len, nullptr, nullptr, nullptr, nullptr);

		if (status != ERROR_SUCCESS)
			continue;

		_stprintf_s(connection_string, _T("%s\\%s\\Connection"),
			NETWORK_CONNECTIONS_KEY, enum_name);

		wil::unique_hkey connection_key;
		status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, connection_string, 0, KEY_READ, connection_key.put());

		if (status == ERROR_SUCCESS)
		{
			len = sizeof(name_data);
			status = RegQueryValueEx(connection_key.get(), name_string, nullptr, &name_type, (LPBYTE)name_data,
				&len);

			if (status != ERROR_SUCCESS || name_type != REG_SZ)
			{
				continue;
			}
			else
			{
				if (IsTAPDevice(enum_name))
				{
					AdapterEntry t;
					t.type = Pcsx2Config::DEV9Options::NetApi::TAP;
					t.name = StringUtil::WideStringToUTF8String(std::wstring(name_data));
					t.guid = StringUtil::WideStringToUTF8String(std::wstring(enum_name));
					tap_nic.push_back(t);
				}
			}
		}
	}

	return tap_nic;
}

AdapterOptions TAPAdapter::GetAdapterOptions()
{
	return AdapterOptions::None;
}

static int TAPGetMACAddress(HANDLE handle, PacketReader::MAC_Address* addr)
{
	DWORD len = 0;

	return DeviceIoControl(handle, TAP_IOCTL_GET_MAC,
		addr, 6,
		addr, 6, &len, NULL);
}

static int TAPSetStatus(HANDLE handle, int status)
{
	DWORD len = 0;

	return DeviceIoControl(handle, TAP_IOCTL_SET_MEDIA_STATUS,
		&status, sizeof(status),
		&status, sizeof(status), &len, NULL);
}
HANDLE TAPOpen(const std::string& device_guid)
{
	struct
	{
		unsigned long major;
		unsigned long minor;
		unsigned long debug;
	} version;
	DWORD version_len;

	std::string device_path = USERMODEDEVICEDIR + device_guid + TAPSUFFIX;

	wil::unique_hfile handle(CreateFileA(
		device_path.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0,
		0,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED,
		0));

	if (!handle)
	{
		return INVALID_HANDLE_VALUE;
	}

	const BOOL bret = DeviceIoControl(handle.get(), TAP_IOCTL_GET_VERSION,
		&version, sizeof(version),
		&version, sizeof(version), &version_len, NULL);

	if (bret == FALSE)
	{
		return INVALID_HANDLE_VALUE;
	}

	if (!TAPSetStatus(handle.get(), TRUE))
	{
		return INVALID_HANDLE_VALUE;
	}

	return handle.release();
}

PIP_ADAPTER_ADDRESSES FindAdapterViaIndex(PIP_ADAPTER_ADDRESSES adapterList, int ifIndex)
{
	PIP_ADAPTER_ADDRESSES currentAdapter = adapterList;
	do
	{
		if (currentAdapter->IfIndex == ifIndex)
			break;

		currentAdapter = currentAdapter->Next;
	} while (currentAdapter);
	return currentAdapter;
}

bool TAPGetWin32Adapter(const std::string& name, PIP_ADAPTER_ADDRESSES adapter, AdapterUtils::AdapterBuffer* buffer)
{
	AdapterUtils::AdapterBuffer adapterInfo;
	PIP_ADAPTER_ADDRESSES pAdapterFirst = AdapterUtils::GetAllAdapters(&adapterInfo, true);
	if (pAdapterFirst == nullptr)
		return false;

	PIP_ADAPTER_ADDRESSES pAdapter = pAdapterFirst;
	do
	{
		if (0 == strcmp(pAdapter->AdapterName, name.c_str()))
			break;

		pAdapter = pAdapter->Next;
	} while (pAdapter);

	if (pAdapter == nullptr)
		return false;

	AdapterUtils::AdapterBuffer adapterInfoReduced;
	PIP_ADAPTER_ADDRESSES pAdapterReducedFirst = AdapterUtils::GetAllAdapters(&adapterInfoReduced, false);

	if (FindAdapterViaIndex(pAdapterReducedFirst, pAdapter->IfIndex) != nullptr)
	{
		*adapter = *pAdapter;
		buffer->swap(adapterInfo);
		return true;
	}

	Console.WriteLn("DEV9: Current adapter is probably bridged");
	Console.WriteLn(fmt::format("DEV9: Adapter Display name: {}", StringUtil::WideStringToUTF8String(pAdapter->FriendlyName)));

	std::vector<NET_IFINDEX> potentialBridges;
	std::vector<NET_IFINDEX> searchList;
	searchList.push_back(pAdapter->IfIndex);

	PMIB_IFSTACK_TABLE table;
	GetIfStackTable(&table);
	for (size_t vi = 0; vi < searchList.size(); vi++)
	{
		for (ULONG i = 0; i < table->NumEntries; i++)
		{
			int targetIndex = searchList[vi];
			MIB_IFSTACK_ROW row = table->Table[i];
			if (row.LowerLayerInterfaceIndex == targetIndex)
			{
				const PIP_ADAPTER_ADDRESSES potentialAdapter = FindAdapterViaIndex(pAdapterReducedFirst, row.HigherLayerInterfaceIndex);
				if (potentialAdapter != nullptr)
				{
					Console.WriteLn(fmt::format("DEV9: {} is possible bridge (Check 1 passed)", StringUtil::WideStringToUTF8String(potentialAdapter->Description)));
					potentialBridges.push_back(row.HigherLayerInterfaceIndex);
				}
				else
					searchList.push_back(row.HigherLayerInterfaceIndex);
				break;
			}
		}
	}
	FreeMibTable(table);
	pAdapterReducedFirst = nullptr;
	adapterInfoReduced.reset();

	HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (cohr == RPC_E_CHANGED_MODE)
		cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(cohr))
		return false;

	wil::unique_couninitialize_call uninit;

	PIP_ADAPTER_ADDRESSES bridgeAdapter = nullptr;

	if (auto netcfg = wil::CoCreateInstanceNoThrow<INetCfg>(CLSID_CNetCfg))
	{
		HRESULT hr = netcfg->Initialize(nullptr);
		if (SUCCEEDED(hr))
		{
			wil::com_ptr_nothrow<INetCfgComponent> bridge;
			hr = netcfg->FindComponent(L"ms_bridge", bridge.put());

			if (SUCCEEDED(hr))
			{
				wil::com_ptr_nothrow<IEnumNetCfgComponent> components;
				hr = netcfg->EnumComponents(&GUID_DEVCLASS_NET, components.put());
				if (SUCCEEDED(hr))
				{
					for (const auto& index : potentialBridges)
					{
						PIP_ADAPTER_ADDRESSES cAdapterInfo = FindAdapterViaIndex(pAdapterFirst, index);

						if (cAdapterInfo == nullptr || cAdapterInfo->AdapterName == nullptr)
							continue;

						wchar_t wName[40] = {0};
						mbstowcs(wName, cAdapterInfo->AdapterName, 39);
						GUID nameGuid;
						hr = IIDFromString(wName, &nameGuid);
						if (!SUCCEEDED(hr))
							continue;

						wil::com_ptr_nothrow<INetCfgComponent> component;
						while (true)
						{
							if (components->Next(1, component.put(), nullptr) != S_OK)
								break;

							GUID comInstGuid;
							hr = component->GetInstanceGuid(&comInstGuid);

							if (SUCCEEDED(hr) && IsEqualGUID(nameGuid, comInstGuid))
							{
								wil::unique_cotaskmem_string comId;
								hr = component->GetId(comId.put());
								if (!SUCCEEDED(hr))
									continue;

								if (wcscmp(L"compositebus\\ms_implat_mp", comId.get()) == 0)
								{
									wil::unique_cotaskmem_string dispName;
									hr = component->GetDisplayName(dispName.put());
									if (SUCCEEDED(hr))
										Console.WriteLn(fmt::format("DEV9: {} is possible bridge (Check 2 passed)", StringUtil::WideStringToUTF8String(dispName.get())));

									auto bindings = bridge.try_query<INetCfgComponentBindings>();
									if (!bindings)
										continue;

									hr = bindings->IsBoundTo(component.get());
									if (hr != S_OK)
										continue;

									hr = component->GetDisplayName(dispName.put());
									if (SUCCEEDED(hr))
										Console.WriteLn(fmt::format("DEV9: {} is bridge (Check 3 passed)", StringUtil::WideStringToUTF8String(dispName.get())));

									bridgeAdapter = cAdapterInfo;
									break;
								}
							}
						}
						components->Reset();
						if (bridgeAdapter != nullptr)
							break;
					}
				}
			}
			netcfg->Uninitialize();
		}
	}

	if (bridgeAdapter != nullptr)
	{
		*adapter = *bridgeAdapter;
		buffer->swap(adapterInfo);
		return true;
	}

	return false;
}

TAPAdapter::TAPAdapter()
	: NetAdapter()
{
	if (!EmuConfig.DEV9.EthEnable)
		return;
	htap = TAPOpen(EmuConfig.DEV9.EthDevice);

	read.Offset = 0;
	read.OffsetHigh = 0;
	read.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	write.Offset = 0;
	write.OffsetHigh = 0;
	write.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	cancel = CreateEvent(NULL, TRUE, FALSE, NULL);

	PacketReader::MAC_Address hostMAC;
	PacketReader::MAC_Address newMAC;

	TAPGetMACAddress(htap, &hostMAC);
	newMAC = ps2MAC;

	newMAC.bytes[5] = hostMAC.bytes[4];
	newMAC.bytes[4] = hostMAC.bytes[5];

	SetMACAddress(&newMAC);

	IP_ADAPTER_ADDRESSES adapter;
	AdapterUtils::AdapterBuffer buffer;
	if (TAPGetWin32Adapter(EmuConfig.DEV9.EthDevice, &adapter, &buffer))
		InitInternalServer(&adapter);
	else
	{
		Console.Error("DEV9: Failed to get adapter information");
		InitInternalServer(nullptr);
	}

	isActive = true;
}

bool TAPAdapter::blocks()
{
	return true;
}
bool TAPAdapter::isInitialised()
{
	return (htap != NULL);
}
bool TAPAdapter::recv(NetPacket* pkt)
{
	DWORD read_size;
	BOOL result = ReadFile(htap,
		pkt->buffer,
		sizeof(pkt->buffer),
		&read_size,
		&read);

	if (!result)
	{
		const DWORD dwError = GetLastError();
		if (dwError == ERROR_IO_PENDING)
		{
			HANDLE readHandles[]{read.hEvent, cancel};
			const DWORD waitResult = WaitForMultipleObjects(2, readHandles, FALSE, INFINITE);

			if (waitResult == WAIT_OBJECT_0 + 1)
			{
				CancelIo(htap);
				result = GetOverlappedResult(htap, &read, &read_size, TRUE);
			}
			else
				result = GetOverlappedResult(htap, &read, &read_size, FALSE);
		}
	}

	if (result && VerifyPkt(pkt, read_size))
	{
		InspectRecv(pkt);
		return true;
	}
	else
		return false;
}
bool TAPAdapter::send(NetPacket* pkt)
{
	InspectSend(pkt);
	if (NetAdapter::send(pkt))
		return true;

	DWORD writen;
	BOOL result = WriteFile(htap,
		pkt->buffer,
		pkt->size,
		&writen,
		&write);

	if (!result)
	{
		const DWORD dwError = GetLastError();
		if (dwError == ERROR_IO_PENDING)
		{
			WaitForSingleObject(write.hEvent, INFINITE);
			result = GetOverlappedResult(htap, &write, &writen, FALSE);
		}
	}

	if (result)
	{
		if (writen != pkt->size)
			return false;

		return true;
	}
	else
		return false;
}

void TAPAdapter::reloadSettings()
{
	IP_ADAPTER_ADDRESSES adapter;
	AdapterUtils::AdapterBuffer buffer;
	if (TAPGetWin32Adapter(EmuConfig.DEV9.EthDevice, &adapter, &buffer))
		ReloadInternalServer(&adapter);
	else
		ReloadInternalServer(nullptr);
}

void TAPAdapter::close()
{
	SetEvent(cancel);
}
TAPAdapter::~TAPAdapter()
{
	if (!isActive)
		return;
	CloseHandle(read.hEvent);
	CloseHandle(write.hEvent);
	CloseHandle(cancel);
	TAPSetStatus(htap, FALSE);
	CloseHandle(htap);
	isActive = false;
}