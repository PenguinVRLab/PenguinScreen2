// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/Assertions.h"

class Error;

enum class FreezeAction
{
	Load,
	Save,
	Size,
};

static const u32 g_SaveVersion = (0x9A59 << 16) | 0x0000;


struct freezeData
{
	int size;
	u8* data;
};

struct SaveStateScreenshotData
{
	u32 width;
	u32 height;
	std::vector<u32> pixels;
};

class ArchiveEntryList;

extern std::unique_ptr<ArchiveEntryList> SaveState_DownloadState(Error* error);
extern std::unique_ptr<SaveStateScreenshotData> SaveState_SaveScreenshot();
extern bool SaveState_ZipToDisk(
	std::unique_ptr<ArchiveEntryList> srclist, std::unique_ptr<SaveStateScreenshotData> screenshot,
	const char* filename, Error* error);
extern bool SaveState_ReadScreenshot(const std::string& filename, u32* out_width, u32* out_height, std::vector<u32>* out_pixels);
extern bool SaveState_UnzipFromDisk(const std::string& filename, Error* error);

class SaveStateBase
{
public:
	using VmStateBuffer = std::vector<u8>;

protected:
	VmStateBuffer& m_memory;

	u32 m_version = 0;

	int m_idx = 0;

	bool m_error = false;

public:
	SaveStateBase(VmStateBuffer& memblock);
	virtual ~SaveStateBase() = default;

	__fi bool HasError() const { return m_error; }
	__fi bool IsOkay() const { return !m_error; }

	u32 GetVersion() const
	{
		return (m_version & 0xffff);
	}

	bool FreezeBios();
	bool FreezeInternals(Error* error);

	template<typename T>
	void Freeze( T& data )
	{
		FreezeMem( const_cast<void*>((void*)&data), sizeof( T ) );
	}

	template<typename T>
	void FreezeLegacy( T& data, int sizeOfNewStuff )
	{
		FreezeMem( &data, sizeof( T ) - sizeOfNewStuff );
	}

	void PrepBlock( int size );

	template <typename T>
	void FreezeDeque(std::deque<T>& q)
	{
		u32 count = static_cast<u32>(q.size());
		Freeze(count);

		std::unique_ptr<T[]> temp;
		if (count > 0)
		{
			temp = std::make_unique<T[]>(count);
			if (IsSaving())
			{
				u32 pos = 0;
				for (const T& it : q)
					temp[pos++] = it;
			}

			FreezeMem(temp.get(), static_cast<int>(sizeof(T) * count));
		}

		if (IsLoading())
		{
			q.clear();
			for (u32 i = 0; i < count; i++)
				q.push_back(temp[i]);
		}
	}

	void FreezeString(std::string& s)
	{
		u32 length = static_cast<u32>(s.length());
		Freeze(length);

		if (IsLoading())
			s.resize(length);

		FreezeMem(s.data(), length);
	}

	uint GetCurrentPos() const
	{
		return m_idx;
	}

	u8* GetBlockPtr()
	{
		return &m_memory[m_idx];
	}

	void CommitBlock( int size )
	{
		m_idx += size;
	}

	bool FreezeTag( const char* src );

	bool IsLoading() const { return !IsSaving(); }

	virtual void FreezeMem( void* data, int size )=0;

	virtual bool IsSaving() const=0;

public:
	bool gsFreeze();

protected:
	bool vmFreeze();
	bool mtvuFreeze();
	bool rcntFreeze();
	bool memFreeze(Error* error);
	bool vuMicroFreeze();
	bool vuJITFreeze();
	bool vif0Freeze();
	bool vif1Freeze();
	bool sifFreeze();
	bool ipuFreeze();
	bool ipuDmaFreeze();
	bool gifFreeze();
	bool gifDmaFreeze();
	bool gifPathFreeze(u32 path);

	bool sprFreeze();

	bool sioFreeze();
	bool cdrFreeze();
	bool cdvdFreeze();
	bool psxRcntFreeze();
	bool deci2Freeze();
	bool handleFreeze();

	bool InputRecordingFreeze();
};

class ArchiveEntry final
{
protected:
	std::string	m_filename;
	uptr		m_dataidx;
	size_t		m_datasize;

public:
	ArchiveEntry(std::string filename)
		: m_filename(std::move(filename))
	{
		m_dataidx = 0;
		m_datasize = 0;
	}

	~ArchiveEntry() = default;

	ArchiveEntry& SetDataIndex(uptr idx)
	{
		m_dataidx = idx;
		return *this;
	}

	ArchiveEntry& SetDataSize(size_t size)
	{
		m_datasize = size;
		return *this;
	}

	const std::string& GetFilename() const
	{
		return m_filename;
	}

	uptr GetDataIndex() const
	{
		return m_dataidx;
	}

	uint GetDataSize() const
	{
		return m_datasize;
	}
};

class ArchiveEntryList final
{
public:
	using VmStateBuffer = std::vector<u8>;
	DeclareNoncopyableObject(ArchiveEntryList);

protected:
	std::vector<ArchiveEntry> m_list;
	VmStateBuffer m_data;

public:
	ArchiveEntryList() = default;
	~ArchiveEntryList() = default;

	const VmStateBuffer& GetBuffer() const
	{
		return m_data;
	}

	VmStateBuffer& GetBuffer()
	{
		return m_data;
	}

	u8* GetPtr(uint idx)
	{
		return &m_data[idx];
	}

	const u8* GetPtr(uint idx) const
	{
		return &m_data[idx];
	}

	ArchiveEntryList& Add(const ArchiveEntry& src)
	{
		m_list.push_back(src);
		return *this;
	}

	size_t GetLength() const
	{
		return m_list.size();
	}

	ArchiveEntry& operator[](uint idx)
	{
		return m_list[idx];
	}

	const ArchiveEntry& operator[](uint idx) const
	{
		return m_list[idx];
	}
};

class memSavingState final : public SaveStateBase
{
	typedef SaveStateBase _parent;

public:
	memSavingState(VmStateBuffer& save_to);
	~memSavingState() override = default;

	void FreezeMem(void* data, int size) override;
	bool IsSaving() const override { return true; }
};

class memLoadingState final : public SaveStateBase
{
public:
	memLoadingState(const VmStateBuffer& load_from);
	~memLoadingState() override = default;

	void FreezeMem(void* data, int size) override;
	bool IsSaving() const override { return false; }
};

void SaveState_ReportLoadErrorOSD(const std::string& message, std::optional<s32> slot, bool backup);
void SaveState_ReportSaveErrorOSD(const std::string& message, std::optional<s32> slot);
