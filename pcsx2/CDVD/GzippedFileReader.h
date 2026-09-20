// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "CDVD/ThreadedFileReader.h"
#include "zlib_indexed.h"

class GzippedFileReader final : public ThreadedFileReader
{
	DeclareNoncopyableObject(GzippedFileReader);

public:
	GzippedFileReader();
	~GzippedFileReader();

	bool Open2(std::string filename, Error* error) override;

	Chunk ChunkForOffset(u64 offset) override;
	int ReadChunk(void* dst, s64 chunkID) override;

	void Close2() override;

	u32 GetBlockCount() const override;

private:
	static constexpr int GZFILE_SPAN_DEFAULT = (1048576 * 4);
	static constexpr int GZFILE_READ_CHUNK_SIZE = (256 * 1024);
	static constexpr int GZFILE_CACHE_SIZE_MB = 200;

	bool LoadOrCreateIndex(Error* error);

	Access* m_index = nullptr;

	std::FILE* m_src = nullptr;

	zstate m_z_state = {};
};
