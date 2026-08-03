// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/CDVDdiscReader.h"
#include "CDVD/CDVD.h"

#include "common/Console.h"
#include "common/Error.h"

#include <winioctl.h>
#include <ntddcdvd.h>
#include <ntddcdrm.h>
#include <errno.h>
#pragma warning(push)
#pragma warning(disable : 4091)
#include <ntddscsi.h>
#pragma warning(pop)

#include <cstddef>
#include <cstdlib>
#include <stdexcept>

#include "fmt/format.h"

IOCtlSrc::IOCtlSrc(std::string filename)
	: m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc()
{
	if (m_device != INVALID_HANDLE_VALUE)
	{
		SetSpindleSpeed(true);
		CloseHandle(m_device);
	}
}

bool IOCtlSrc::Reopen(Error* error)
{
	if (m_device != INVALID_HANDLE_VALUE)
		CloseHandle(m_device);

	m_device = CreateFileA(m_filename.c_str(), GENERIC_READ | GENERIC_WRITE,
						  FILE_SHARE_READ, nullptr, OPEN_EXISTING,
						  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (m_device == INVALID_HANDLE_VALUE)
	{
		Error::SetWin32(error, GetLastError());
		return false;
	}

	DWORD unused;
	DeviceIoControl(m_device, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr,
					0, &unused, nullptr);

	if (ReadDVDInfo() || ReadCDInfo())
		SetSpindleSpeed(false);

	return true;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
	const USHORT speed = restore_defaults ? 0xFFFF : GetMediaType() >= 0 ? 5540 : 3600;
	CDROM_SET_SPEED s{CdromSetSpeed, speed, speed, CdromDefaultRotation};

	DWORD unused;
	if (DeviceIoControl(m_device, IOCTL_CDROM_SET_SPEED, &s, sizeof(s),
						nullptr, 0, &unused, nullptr))
	{
		if (!restore_defaults)
			printf(" * CDVD: setSpindleSpeed success (%uKB/s)\n", speed);
	}
	else
	{
		printf(" * CDVD: setSpindleSpeed failed!\n");
	}
}

u32 IOCtlSrc::GetSectorCount() const
{
	return m_sectors;
}

u32 IOCtlSrc::GetLayerBreakAddress() const
{
	return m_layer_break;
}

s32 IOCtlSrc::GetMediaType() const
{
	return m_media_type;
}

const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
	return m_toc;
}

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
	std::lock_guard<std::mutex> guard(m_lock);
	LARGE_INTEGER offset;
	offset.QuadPart = sector * 2048ULL;

	if (!SetFilePointerEx(m_device, offset, nullptr, FILE_BEGIN))
	{
		Console.Error(fmt::format(" * CDVD SetFilePointerEx failed: sector {}: error {}",
				sector, GetLastError()));
		return false;
	}

	const DWORD bytes_to_read = 2048 * count;
	DWORD bytes_read;
	if (ReadFile(m_device, buffer, bytes_to_read, &bytes_read, nullptr))
	{
		if (bytes_read == bytes_to_read)
			return true;
		Console.Error(fmt::format(" * CDVD ReadFile: sectors {}-{}: {} bytes read, {} bytes expected",
				sector, sector + count - 1, bytes_read, bytes_to_read));
	}
	else
	{
		Console.Error(fmt::format(" * CDVD ReadFile failed: sectors {}-{}: error {}",
				sector, sector + count - 1, GetLastError()));
	}

	return false;
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
	struct sptdinfo
	{
		SCSI_PASS_THROUGH_DIRECT info;
		char sense_buffer[20];
	} sptd{};

	sptd.info.Cdb[0] = 0xBE;
	sptd.info.Cdb[1] = 0;
	sptd.info.Cdb[6] = 0;
	sptd.info.Cdb[7] = 0;
	sptd.info.Cdb[8] = 1;
	sptd.info.Cdb[9] = 0xF8;
	sptd.info.Cdb[10] = 0;
	sptd.info.Cdb[11] = 0;

	sptd.info.CdbLength = 12;
	sptd.info.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
	sptd.info.DataIn = SCSI_IOCTL_DATA_IN;
	sptd.info.SenseInfoOffset = offsetof(sptdinfo, sense_buffer);
	sptd.info.TimeOutValue = 5;

	for (u32 n = 0; n < count; ++n)
	{
		u32 current_sector = sector + n;
		sptd.info.Cdb[2] = (current_sector >> 24) & 0xFF;
		sptd.info.Cdb[3] = (current_sector >> 16) & 0xFF;
		sptd.info.Cdb[4] = (current_sector >> 8) & 0xFF;
		sptd.info.Cdb[5] = current_sector & 0xFF;
		sptd.info.DataTransferLength = 2352;
		sptd.info.DataBuffer = buffer + 2352 * n;
		sptd.info.SenseInfoLength = sizeof(sptd.sense_buffer);

		DWORD unused;
		if (DeviceIoControl(m_device, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
							sizeof(sptd), &sptd, sizeof(sptd), &unused, nullptr))
		{
			if (sptd.info.DataTransferLength == 2352)
				continue;
		}
		printf(" * CDVD: SPTI failed reading sector %u; SENSE %u -", current_sector, sptd.info.SenseInfoLength);
		for (const auto& c : sptd.sense_buffer)
			printf(" %02X", c);
		putchar('\n');
		return false;
	}

	return true;
}

bool IOCtlSrc::ReadDVDInfo()
{
	DWORD unused;

	std::array<u8, 32> buffer;
	DVD_READ_STRUCTURE dvdrs{{}, DvdPhysicalDescriptor, 0, 0};

	if (!DeviceIoControl(m_device, IOCTL_DVD_READ_STRUCTURE, &dvdrs, sizeof(dvdrs),
						 buffer.data(), buffer.size(), &unused, nullptr))
	{
		if ((GetLastError() == ERROR_INVALID_FUNCTION) || (GetLastError() == ERROR_NOT_SUPPORTED))
		{
			Console.Warning("IOCTL_DVD_READ_STRUCTURE not supported");
		}
		else if (GetLastError() != ERROR_UNRECOGNIZED_MEDIA)
		{
			Console.Warning("IOCTL Unknown Error %d", GetLastError());
		}
		return false;
	}

	auto& layer = *reinterpret_cast<DVD_LAYER_DESCRIPTOR*>(
		reinterpret_cast<DVD_DESCRIPTOR_HEADER*>(buffer.data())->Data);

	u32 start_sector = _byteswap_ulong(layer.StartingDataSector);
	u32 end_sector = _byteswap_ulong(layer.EndDataSector);

	if (layer.NumberOfLayers == 0)
	{
		m_media_type = 0;
		m_layer_break = 0;
		m_sectors = end_sector - start_sector + 1;
	}
	else if (layer.TrackPath == 0)
	{
		dvdrs.LayerNumber = 1;
		if (!DeviceIoControl(m_device, IOCTL_DVD_READ_STRUCTURE, &dvdrs, sizeof(dvdrs),
							 buffer.data(), buffer.size(), &unused, nullptr))
			return false;
		u32 layer1_start_sector = _byteswap_ulong(layer.StartingDataSector);
		u32 layer1_end_sector = _byteswap_ulong(layer.EndDataSector);

		m_media_type = 1;
		m_layer_break = end_sector - start_sector;
		m_sectors = end_sector - start_sector + 1 + layer1_end_sector - layer1_start_sector + 1;
	}
	else
	{
		u32 end_sector_layer0 = _byteswap_ulong(layer.EndLayerZeroSector);
		m_media_type = 2;
		m_layer_break = end_sector_layer0 - start_sector;
		m_sectors = end_sector_layer0 - start_sector + 1 + end_sector - (~end_sector_layer0 & 0xFFFFFFU) + 1;
	}

	return true;
}

bool IOCtlSrc::ReadCDInfo()
{
	DWORD unused;
	CDROM_READ_TOC_EX toc_ex{};
	toc_ex.Format = CDROM_READ_TOC_EX_FORMAT_TOC;
	toc_ex.Msf = 0;
	toc_ex.SessionTrack = 1;

	CDROM_TOC toc;
	if (!DeviceIoControl(m_device, IOCTL_CDROM_READ_TOC_EX, &toc_ex,
						 sizeof(toc_ex), &toc, sizeof(toc), &unused, nullptr))
		return false;

	m_toc.clear();
	size_t track_count = ((toc.Length[0] << 8) + toc.Length[1] - 2) / sizeof(TRACK_DATA);
	for (size_t n = 0; n < track_count; ++n)
	{
		TRACK_DATA& track = toc.TrackData[n];
		if (track.TrackNumber == 0xAA)
			continue;
		u32 lba = (track.Address[1] << 16) + (track.Address[2] << 8) + track.Address[3];
		m_toc.push_back({lba, track.TrackNumber, track.Adr, track.Control});
	}

	GET_LENGTH_INFORMATION info;
	if (!DeviceIoControl(m_device, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &info,
						 sizeof(info), &unused, nullptr))
		return false;

	m_sectors = static_cast<u32>(info.Length.QuadPart / 2048);
	m_media_type = -1;

	return true;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subQ) const
{
	CDROM_SUB_Q_DATA_FORMAT format;
	SUB_Q_CHANNEL_DATA osSubQ{};
	DWORD unused;

	format.Format = IOCTL_CDROM_CURRENT_POSITION;

	if (!DeviceIoControl(m_device, IOCTL_CDROM_READ_Q_CHANNEL, &format, sizeof(format), &osSubQ, sizeof(osSubQ), &unused, nullptr))
	{
		Console.Error("SUB CHANNEL READ ERROR: %d\n", errno);
		return false;
	}
	else
	{
		subQ->adr = osSubQ.CurrentPosition.ADR;
		subQ->trackNum = osSubQ.CurrentPosition.TrackNumber;
		subQ->trackIndex = osSubQ.CurrentPosition.IndexNumber;
	}

	return true;
}

bool IOCtlSrc::DiscReady()
{
	if (m_device == INVALID_HANDLE_VALUE)
		return false;

	DWORD unused;
	if (DeviceIoControl(m_device, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0,
						nullptr, 0, &unused, nullptr))
	{
		if (!m_sectors)
			Reopen(nullptr);
	}
	else
	{
		m_sectors = 0;
		m_layer_break = 0;
		m_media_type = 0;
	}

	return !!m_sectors;
}
