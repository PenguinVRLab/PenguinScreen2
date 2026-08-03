// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/DX12/D3D12StreamBuffer.h"
#include "GS/Renderers/DX12/GSDevice12.h"

#include "common/BitUtils.h"
#include "common/Assertions.h"
#include "common/Console.h"

#include "D3D12MemAlloc.h"

#include <algorithm>
#include <functional>

D3D12StreamBuffer::D3D12StreamBuffer() = default;

D3D12StreamBuffer::~D3D12StreamBuffer()
{
	Destroy();
}

bool D3D12StreamBuffer::Create(u32 size, bool gpu_backed_buffer)
{
	const GSDevice12::D3D12_RESOURCE_DESCU resource_desc = {{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
		{1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE}};

	D3D12MA::ALLOCATION_DESC allocationDesc = {};
	allocationDesc.Flags = D3D12MA::ALLOCATION_FLAG_COMMITTED;
	allocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

	wil::com_ptr_nothrow<ID3D12Resource> buffer;
	wil::com_ptr_nothrow<D3D12MA::Allocation> allocation;
	HRESULT hr;
	if (GSDevice12::GetInstance()->UseEnhancedBarriers())
		hr = GSDevice12::GetInstance()->GetAllocator()->CreateResource3(&allocationDesc, &resource_desc.desc1,
			D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr, allocation.put(), IID_PPV_ARGS(buffer.put()));
	else
		hr = GSDevice12::GetInstance()->GetAllocator()->CreateResource(&allocationDesc, &resource_desc.desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, allocation.put(), IID_PPV_ARGS(buffer.put()));
	pxAssertMsg(SUCCEEDED(hr), "Allocate buffer");
	if (FAILED(hr))
		return false;

	static const D3D12_RANGE read_range = {};
	u8* host_pointer;
	hr = buffer->Map(0, &read_range, reinterpret_cast<void**>(&host_pointer));
	pxAssertMsg(SUCCEEDED(hr), "Map buffer");
	if (FAILED(hr))
		return false;

	Destroy(true);

	m_buffer_upload = std::move(buffer);
	m_allocation_upload = std::move(allocation);
	m_host_pointer = host_pointer;
	m_size = size;

	if (gpu_backed_buffer)
	{
		allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

		if (GSDevice12::GetInstance()->UseEnhancedBarriers())
			hr = GSDevice12::GetInstance()->GetAllocator()->CreateResource3(&allocationDesc, &resource_desc.desc1,
				D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, 0, nullptr, allocation.put(), IID_PPV_ARGS(buffer.put()));
		else
			hr = GSDevice12::GetInstance()->GetAllocator()->CreateResource(&allocationDesc, &resource_desc.desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, allocation.put(), IID_PPV_ARGS(buffer.put()));
		pxAssertMsg(SUCCEEDED(hr), "Allocate buffer");
		if (FAILED(hr))
			return false;

		m_buffer_default = std::move(buffer);
		m_allocation_default = std::move(allocation);
		m_gpu_pointer = m_buffer_default->GetGPUVirtualAddress();
	}
	else
		m_gpu_pointer = m_buffer_upload->GetGPUVirtualAddress();

	return true;
}

bool D3D12StreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
{
	const u32 required_bytes = num_bytes + alignment;

	if (num_bytes > m_size)
	{
		Console.Error("Attempting to allocate %u bytes from a %u byte stream buffer", static_cast<u32>(num_bytes),
			static_cast<u32>(m_size));
		pxFailRel("Stream buffer overflow");
		return false;
	}

	UpdateCurrentFencePosition();
	if (m_current_offset >= m_current_gpu_position)
	{
		const u32 aligned_required_bytes = (m_current_offset > 0) ? required_bytes : num_bytes;
		const u32 remaining_bytes = m_size - m_current_offset;
		if (aligned_required_bytes <= remaining_bytes)
		{
			m_current_offset = Common::AlignUp(m_current_offset, alignment);
			m_current_space = m_size - m_current_offset;
			return true;
		}

		if (required_bytes < m_current_gpu_position)
		{
			m_current_offset = 0;
			m_current_space = m_current_gpu_position;
			return true;
		}
	}

	if (m_current_offset < m_current_gpu_position)
	{
		const u32 remaining_bytes = m_current_gpu_position - m_current_offset;
		if (required_bytes < remaining_bytes)
		{
			m_current_offset = Common::AlignUp(m_current_offset, alignment);
			m_current_space = m_current_gpu_position - m_current_offset;
			return true;
		}
	}

	if (WaitForClearSpace(required_bytes))
	{
		const u32 align_diff = Common::AlignUp(m_current_offset, alignment) - m_current_offset;
		m_current_offset += align_diff;
		m_current_space -= align_diff;
		return true;
	}

	return false;
}

void D3D12StreamBuffer::CommitMemory(u32 final_num_bytes)
{
	pxAssert((m_current_offset + final_num_bytes) <= m_size);
	pxAssert(final_num_bytes <= m_current_space);
	m_current_offset += final_num_bytes;
	m_current_space -= final_num_bytes;
}

void D3D12StreamBuffer::FlushMemory()
{
	if (m_buffer_default &&
		m_current_space != m_size)
	{
		if (m_current_copy_offset < m_current_offset)
		{
			const u32 size = m_current_offset - m_current_copy_offset;
			GSDevice12::GetInstance()->GetInitCommandList().list4->CopyBufferRegion(m_buffer_default.get(), m_current_copy_offset,
				m_buffer_upload.get(), m_current_copy_offset, size);
		}
		else
		{
			const u32 size = m_size - m_current_copy_offset;
			GSDevice12::GetInstance()->GetInitCommandList().list4->CopyBufferRegion(m_buffer_default.get(), m_current_copy_offset,
				m_buffer_upload.get(), m_current_copy_offset, size);
			GSDevice12::GetInstance()->GetInitCommandList().list4->CopyBufferRegion(m_buffer_default.get(), 0,
				m_buffer_upload.get(), 0, m_current_offset);
		}
		m_current_copy_offset = m_current_offset;
	}
}

void D3D12StreamBuffer::Destroy(bool defer)
{
	if (m_host_pointer)
	{
		const D3D12_RANGE written_range = {0, m_size};
		m_buffer_upload->Unmap(0, &written_range);
		m_host_pointer = nullptr;
	}

	if (m_buffer_upload && defer)
		GSDevice12::GetInstance()->DeferResourceDestruction(m_allocation_upload.get(), m_buffer_upload.get());
	m_buffer_upload.reset();
	m_allocation_upload.reset();

	if (m_buffer_default && defer)
		GSDevice12::GetInstance()->DeferResourceDestruction(m_allocation_default.get(), m_buffer_default.get());
	m_buffer_default.reset();
	m_allocation_default.reset();

	m_current_offset = 0;
	m_current_copy_offset = 0;
	m_current_space = 0;
	m_current_gpu_position = 0;
	m_tracked_fences.clear();
}

void D3D12StreamBuffer::UpdateCurrentFencePosition()
{
	if (m_current_offset == m_current_gpu_position)
		return;

	const u64 fence = GSDevice12::GetInstance()->GetCurrentFenceValue();
	if (!m_tracked_fences.empty() && m_tracked_fences.back().first == fence)
	{
		m_tracked_fences.back().second = m_current_offset;
		return;
	}

	UpdateGPUPosition();
	m_tracked_fences.emplace_back(fence, m_current_offset);
}

void D3D12StreamBuffer::UpdateGPUPosition()
{
	auto start = m_tracked_fences.begin();
	auto end = start;

	const u64 completed_counter = GSDevice12::GetInstance()->GetCompletedFenceValue();
	while (end != m_tracked_fences.end() && completed_counter >= end->first)
	{
		m_current_gpu_position = end->second;
		++end;
	}

	if (start != end)
		m_tracked_fences.erase(start, end);
}

bool D3D12StreamBuffer::WaitForClearSpace(u32 num_bytes)
{
	u32 new_offset = 0;
	u32 new_space = 0;
	u32 new_gpu_position = 0;

	auto iter = std::find_if(m_tracked_fences.begin(), m_tracked_fences.end(), [&, this](const auto& iter) {
		u32 gpu_position = iter.second;
		if (m_current_offset == gpu_position)
		{
			new_offset = 0;
			new_space = m_size;
			new_gpu_position = 0;
			return true;
		}

		if (m_current_offset > gpu_position)
		{
			const u32 remaining_space_after_offset = m_size - m_current_offset;
			if (remaining_space_after_offset >= num_bytes)
			{
				new_offset = m_current_offset;
				new_space = m_size - m_current_offset;
				new_gpu_position = gpu_position;
				return true;
			}

			if (gpu_position > num_bytes)
			{
				new_offset = 0;
				new_space = gpu_position;
				new_gpu_position = gpu_position;
				return true;
			}
		}
		else
		{
			u32 available_space_inbetween = gpu_position - m_current_offset;
			if (available_space_inbetween > num_bytes)
			{
				new_offset = m_current_offset;
				new_space = gpu_position - m_current_offset;
				new_gpu_position = gpu_position;
				return true;
			}
		}
		return false;
	});

	if (iter == m_tracked_fences.end() || iter->first == GSDevice12::GetInstance()->GetCurrentFenceValue())
		return false;

	GSDevice12::GetInstance()->WaitForFence(iter->first, false);
	m_tracked_fences.erase(
		m_tracked_fences.begin(), m_current_offset == iter->second ? m_tracked_fences.end() : ++iter);
	m_current_offset = new_offset;
	m_current_space = new_space;
	m_current_gpu_position = new_gpu_position;
	return true;
}
