// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

class Error;
class ProgressCallback;

class ThreadedFileReader
{
	ThreadedFileReader(ThreadedFileReader&&) = delete;
protected:
	std::string m_filename;

	u32 m_dataoffset = 0;
	u32 m_blocksize = 2048;

	struct Chunk
	{
		s64 chunkID;
		u64 offset;
		u32 length;
	};

	int m_internalBlockSize = 0;

	virtual Chunk ChunkForOffset(u64 offset) = 0;
	virtual int ReadChunk(void* dst, s64 chunkID) = 0;
	virtual bool Open2(std::string filename, Error* error) = 0;
	virtual bool Precache2(ProgressCallback* progress, Error* error);
	virtual void Close2() = 0;
	bool CheckAvailableMemoryForPrecaching(u64 required_size, Error* error);

	ThreadedFileReader();

private:
	int m_amtRead;
	std::atomic<void*> m_requestPtr{nullptr};
	u64 m_requestOffset = 0;
	u32 m_requestSize = 0;
	std::atomic<bool> m_requestCancelled{false};
	struct Buffer
	{
		void* ptr = nullptr;
		u64 offset = 0;
		std::atomic<u32> size{0};
		u32 cap = 0;
	};
	Buffer m_buffer[2];
	u32 m_nextBuffer = 0;

	std::thread m_readThread;
	std::mutex m_mtx;
	std::condition_variable m_condition;
	bool m_quit = false;
	bool m_running = false;

	u32 InternalBlockSize() const { return m_internalBlockSize ? m_internalBlockSize : m_blocksize; }
	size_t CopyBlocks(void* dst, const void* src, size_t size) const;

	void Loop();

	Buffer* GetBlockPtr(const Chunk& block);
	bool Decompress(void* ptr, u64 offset, u32 size);
	void CancelAndWaitUntilStopped(void);
	bool TryCachedRead(void*& buffer, u64& offset, u32& size, const std::lock_guard<std::mutex>&);

public:
	virtual ~ThreadedFileReader();

	const std::string& GetFilename() const { return m_filename; }
	u32 GetBlockSize() const { return m_blocksize; }

	virtual u32 GetBlockCount() const = 0;


	bool Open(std::string filename, Error* error);
	bool Precache(ProgressCallback* progress, Error* error);
	int ReadSync(void* pBuffer, u32 sector, u32 count);
	void BeginRead(void* pBuffer, u32 sector, u32 count);
	int FinishRead();
	void CancelRead();
	void Close();
	void SetBlockSize(u32 bytes);
	void SetDataOffset(u32 bytes);
};
