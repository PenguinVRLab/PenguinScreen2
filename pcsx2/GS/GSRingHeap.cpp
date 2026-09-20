// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSRingHeap.h"
#include "GS.h"
#include "GSExtra.h"

#include "common/AlignedMalloc.h"

namespace
{
	template <size_t align>
	size_t alignTo(size_t value)
	{
		return ((value + (align - 1)) / align) * align;
	}

	size_t alignUsingMask(size_t align_mask, size_t value)
	{
		return (value + align_mask) & ~align_mask;
	}
}

struct GSRingHeap::Buffer
{
	friend class GSRingHeap;

	static const size_t BEGINNING_OFFSET;

	static constexpr size_t USAGE_ARR_SIZE = sizeof(uint64_t) / sizeof(size_t);
	static constexpr size_t USAGE_ARR_ELEMS_PER_ENTRY = sizeof(size_t) / sizeof(uint16_t);

	std::atomic<size_t> m_amt_allocated;
	std::atomic<size_t> m_usage[USAGE_ARR_SIZE];
	size_t m_size;
	size_t m_write_loc;
	int m_quadrant_shift;

	void beginUse(uint64_t usage)
	{
		for (size_t i = 0; i < USAGE_ARR_SIZE; i++)
		{
			size_t piece = static_cast<size_t>(usage >> (i * (64 / USAGE_ARR_SIZE)));
			size_t prev = m_usage[i].fetch_add(piece, std::memory_order_relaxed);
			for (size_t j = 0; j < USAGE_ARR_ELEMS_PER_ENTRY; j++)
			{
				[[maybe_unused]] uint16_t section = prev >> (j * 16);
				pxAssert(section != UINT16_MAX && "Usage count overflow");
			}
		}
	}

	void endUse(uint64_t usage)
	{
		for (size_t i = 0; i < USAGE_ARR_SIZE; i++)
		{
			size_t piece = static_cast<size_t>(usage >> (i * (64 / USAGE_ARR_SIZE)));
			m_usage[i].fetch_sub(piece, std::memory_order_release);
		}
	}

	bool isStillInUse(uint32_t quadrant)
	{
		int arridx = (quadrant / USAGE_ARR_ELEMS_PER_ENTRY) % USAGE_ARR_SIZE;
		int shift = (quadrant % USAGE_ARR_ELEMS_PER_ENTRY) * 16;
		return ((m_usage[arridx].load(std::memory_order_acquire) >> shift) & 0xFFFF) != 0;
	}

	uint32_t quadrant(size_t off)
	{
		return static_cast<uint32_t>(off >> m_quadrant_shift);
	}

	uint64_t usageMask(size_t begin_off, size_t size)
	{
		uint64_t mask = 0;
		mask |= 1ull << (quadrant(begin_off) * 16);
		size_t mid_off = begin_off + size / 2;
		mask |= 1ull << (quadrant(mid_off) * 16);
		size_t end_off = begin_off + size - 1;
		mask |= 1ull << (quadrant(end_off) * 16);
		return mask;
	}

	void decref(size_t amt)
	{
		if (m_amt_allocated.fetch_sub(amt, std::memory_order_release) == amt) [[unlikely]]
		{
			std::atomic_thread_fence(std::memory_order_acquire);
			_aligned_free(this);
		}
	}

	void free(void* allocation, size_t size)
	{
		const char* base = reinterpret_cast<const char*>(this);
		size_t begin_off = static_cast<const char*>(allocation) - base;
		endUse(usageMask(begin_off, size));
		decref(size);
	}

	void* alloc(size_t size, size_t align_mask, size_t prefix_size)
	{
		uint32_t prev_quadrant = quadrant(m_write_loc - 1);
		size_t base_off = alignUsingMask(align_mask, m_write_loc + prefix_size);
		uint64_t usage_mask = 1ull << (quadrant(base_off - prefix_size) * 16);
		uint32_t new_quadrant = quadrant(base_off + size - 1);
		if (prev_quadrant != new_quadrant)
		{
			uint32_t cur_quadrant = prev_quadrant + 1;
			if (new_quadrant >= 4)
			{
				cur_quadrant = 0;
				usage_mask = 0;
				base_off = alignUsingMask(align_mask, BEGINNING_OFFSET + prefix_size);
				new_quadrant = quadrant(base_off + size - 1);
			}
			do
			{
				usage_mask |= 1ull << (cur_quadrant * 16);
				if (isStillInUse(cur_quadrant)) [[unlikely]]
					return nullptr;
			} while (++cur_quadrant <= new_quadrant);
		}

		m_write_loc = base_off + size;
		beginUse(usage_mask);
		m_amt_allocated.fetch_add(size + prefix_size, std::memory_order_relaxed);
		return reinterpret_cast<char*>(this) + base_off - prefix_size;
	}

	static Buffer* make(int quadrant_shift)
	{
		size_t size = 4ull << quadrant_shift;
		Buffer* buffer = reinterpret_cast<Buffer*>(_aligned_malloc(size, 32));
		buffer->m_size = size;
		buffer->m_quadrant_shift = quadrant_shift;
		buffer->m_amt_allocated.store(1, std::memory_order_relaxed);
		for (std::atomic<size_t>& usage : buffer->m_usage)
			usage.store(0, std::memory_order_relaxed);
		buffer->m_write_loc = BEGINNING_OFFSET;
		return buffer;
	}
};

const size_t GSRingHeap::Buffer::BEGINNING_OFFSET = alignTo<64>(sizeof(Buffer));
constexpr size_t GSRingHeap::MIN_ALIGN;

GSRingHeap::GSRingHeap()
{
	m_current_buffer = Buffer::make(14);
}

GSRingHeap::~GSRingHeap() noexcept
{
	orphanBuffer();
}

void GSRingHeap::orphanBuffer() noexcept
{
	m_current_buffer->decref(1);
}

void* GSRingHeap::alloc_internal(size_t size, size_t align_mask, size_t prefix_size)
{
	prefix_size += sizeof(Buffer*);
	size_t total_size = size + prefix_size;

	if (total_size <= (m_current_buffer->m_size / 2)) [[likely]]
	{
		if (void* ptr = m_current_buffer->alloc(size, align_mask, prefix_size))
		{
			Buffer** bptr = static_cast<Buffer**>(ptr);
			*bptr = m_current_buffer;
			return bptr + 1;
		}
		else if (IsDevBuild)
		{
			size_t total = m_current_buffer->m_size;
			size_t mb = 1024 * 1024;
			if (total >= mb)
			{
				size_t used = m_current_buffer->m_amt_allocated.load(std::memory_order_relaxed) - 1;
				if (used * 4 < total)
				{
					fprintf(stderr, "GSRingHeap: Orphaning %zumb buffer with low usage of %d%%, check that allocations are actually being deallocated approximately in order\n", total / mb, static_cast<int>((used * 100) / total));
				}
			}
		}
	}

	int shift = m_current_buffer->m_quadrant_shift;
	do
	{
		shift++;
	} while (total_size > (2ull << shift));

	if (shift > 24 && total_size <= (2ull << (shift - 1)))
	{
		fprintf(stderr, "GSRingHeap: Refusing to grow to %umb\n", 4u << (shift - 20));
		shift--;
	}
	Buffer* new_buffer = Buffer::make(shift);
	orphanBuffer();
	m_current_buffer = new_buffer;
	void* ptr = m_current_buffer->alloc(size, align_mask, prefix_size);
	pxAssert(ptr && "Fresh buffer failed to allocate!");

	Buffer** bptr = static_cast<Buffer**>(ptr);
	*bptr = m_current_buffer;
	return bptr + 1;
}

void GSRingHeap::free_internal(void* ptr, size_t size) noexcept
{
	size += sizeof(Buffer*);
	Buffer** bptr = static_cast<Buffer**>(ptr) - 1;
	(*bptr)->free(bptr, size);
}
