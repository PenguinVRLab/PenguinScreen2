// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include "common/AlignedMalloc.h"

union nVifBlock
{
	struct
	{
		u8 num;
		u8 upkType;
		u16 length;
		u32 mask;
		u8 mode;
		u8 aligned;
		u8 cl;
		u8 wl;
		uptr startPtr;
	};

	struct
	{
		u16 hash_key;
		u16 _pad0;
		u32 key0;
		u32 key1;
		uptr value;
	};

};

#define hSize 0x10000

class HashBucket
{
protected:
	std::array<nVifBlock*, hSize> m_bucket;

public:
	HashBucket()
	{
		m_bucket.fill(nullptr);
	}

	~HashBucket() { clear(); }

	__fi nVifBlock* find(const nVifBlock& dataPtr)
	{
		nVifBlock* chainpos = m_bucket[dataPtr.hash_key];

		while (true)
		{
			if (chainpos->key0 == dataPtr.key0 && chainpos->key1 == dataPtr.key1)
				return chainpos;

			if (chainpos->startPtr == 0)
				return nullptr;

			chainpos++;
		}
	}

	void add(const nVifBlock& dataPtr)
	{
		u32 b = dataPtr.hash_key;

		u32 size = bucket_size(dataPtr);

		if ((m_bucket[b] = (nVifBlock*)pcsx2_aligned_realloc(m_bucket[b], sizeof(nVifBlock) * (size + 2), 64, sizeof(nVifBlock) * (size + 1))) == NULL)
		{
			pxFailRel("Failed to allocate HashBucket Chain");
		}

		memcpy(&m_bucket[b][size++], &dataPtr, sizeof(nVifBlock));
		memset(&m_bucket[b][size], 0, sizeof(nVifBlock));

		if (size > 3)
			DevCon.Warning("recVifUnpk: Bucket 0x%04x has %d micro-programs", b, size);
	}

	u32 bucket_size(const nVifBlock& dataPtr)
	{
		nVifBlock* chainpos = m_bucket[dataPtr.hash_key];

		u32 size = 0;

		while (chainpos->startPtr != 0)
		{
			size++;
			chainpos++;
		}

		return size;
	}

	void clear()
	{
		for (auto& bucket : m_bucket)
			safe_aligned_free(bucket);
	}

	void reset()
	{
		clear();

		for (auto& bucket : m_bucket)
		{
			if ((bucket = (nVifBlock*)_aligned_malloc(sizeof(nVifBlock), 16)) == nullptr)
			{
				pxFailRel("Failed to allocate HashBucket Chain on reset");
			}

			memset(bucket, 0, sizeof(nVifBlock));
		}
	}
};
