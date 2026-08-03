// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKStreamBuffer.h"

#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"

VKStreamBuffer::VKStreamBuffer() = default;

VKStreamBuffer::VKStreamBuffer(VKStreamBuffer&& move)
	: m_size(move.m_size)
	, m_current_offset(move.m_current_offset)
	, m_current_space(move.m_current_space)
	, m_current_gpu_position(move.m_current_gpu_position)
	, m_allocation(move.m_allocation)
	, m_buffer(move.m_buffer)
	, m_host_pointer(move.m_host_pointer)
	, m_tracked_fences(std::move(move.m_tracked_fences))
{
	move.m_size = 0;
	move.m_current_offset = 0;
	move.m_current_space = 0;
	move.m_current_gpu_position = 0;
	move.m_allocation = VK_NULL_HANDLE;
	move.m_buffer = VK_NULL_HANDLE;
	move.m_host_pointer = nullptr;
}

VKStreamBuffer::~VKStreamBuffer()
{
	if (IsValid())
		Destroy(true);
}

VKStreamBuffer& VKStreamBuffer::operator=(VKStreamBuffer&& move)
{
	if (IsValid())
		Destroy(true);

	std::swap(m_size, move.m_size);
	std::swap(m_current_offset, move.m_current_offset);
	std::swap(m_current_space, move.m_current_space);
	std::swap(m_current_gpu_position, move.m_current_gpu_position);
	std::swap(m_buffer, move.m_buffer);
	std::swap(m_host_pointer, move.m_host_pointer);
	std::swap(m_tracked_fences, move.m_tracked_fences);

	return *this;
}

bool VKStreamBuffer::Create(VkBufferUsageFlags usage, u32 size)
{
	const VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, static_cast<VkDeviceSize>(size),
		usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};

	VmaAllocationCreateInfo aci = {};
	aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	aci.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VmaAllocationInfo ai = {};
	VkBuffer new_buffer = VK_NULL_HANDLE;
	VmaAllocation new_allocation = VK_NULL_HANDLE;
	VkResult res =
		vmaCreateBuffer(GSDeviceVK::GetInstance()->GetAllocator(), &bci, &aci, &new_buffer, &new_allocation, &ai);
	if (res != VK_SUCCESS)
	{
		LOG_VULKAN_ERROR(res, "vkCreateBuffer failed: ");
		return false;
	}

	if (IsValid())
		Destroy(true);

	m_size = size;
	m_current_offset = 0;
	m_current_gpu_position = 0;
	m_tracked_fences.clear();
	m_allocation = new_allocation;
	m_buffer = new_buffer;
	m_host_pointer = static_cast<u8*>(ai.pMappedData);
	return true;
}

void VKStreamBuffer::Destroy(bool defer)
{
	if (m_buffer != VK_NULL_HANDLE)
	{
		if (defer)
			GSDeviceVK::GetInstance()->DeferBufferDestruction(m_buffer, m_allocation);
		else
			vmaDestroyBuffer(GSDeviceVK::GetInstance()->GetAllocator(), m_buffer, m_allocation);
	}

	m_size = 0;
	m_current_offset = 0;
	m_current_gpu_position = 0;
	m_tracked_fences.clear();
	m_buffer = VK_NULL_HANDLE;
	m_allocation = VK_NULL_HANDLE;
	m_host_pointer = nullptr;
}

bool VKStreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
{
	const u32 required_bytes = num_bytes + alignment;

	if (required_bytes > m_size)
	{
		Console.Error("Attempting to allocate %u bytes from a %u byte stream buffer", static_cast<u32>(num_bytes),
			static_cast<u32>(m_size));
		pxFailRel("Stream buffer overflow");
		return false;
	}

	UpdateGPUPosition();

	if (m_current_offset >= m_current_gpu_position)
	{
		const u32 remaining_bytes = m_size - m_current_offset;
		if (required_bytes <= remaining_bytes)
		{
			m_current_offset = Common::AlignUp(m_current_offset, alignment);
			m_current_space = m_size - m_current_offset;
			return true;
		}

		if (required_bytes < m_current_gpu_position)
		{
			m_current_offset = 0;
			m_current_space = m_current_gpu_position - 1;
			return true;
		}
	}

	if (m_current_offset < m_current_gpu_position)
	{
		const u32 remaining_bytes = m_current_gpu_position - m_current_offset;
		if (required_bytes < remaining_bytes)
		{
			m_current_offset = Common::AlignUp(m_current_offset, alignment);
			m_current_space = m_current_gpu_position - m_current_offset - 1;
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

void VKStreamBuffer::CommitMemory(u32 final_num_bytes)
{
	pxAssert((m_current_offset + final_num_bytes) <= m_size);
	pxAssert(final_num_bytes <= m_current_space);

	vmaFlushAllocation(GSDeviceVK::GetInstance()->GetAllocator(), m_allocation, m_current_offset, final_num_bytes);

	m_current_offset += final_num_bytes;
	m_current_space -= final_num_bytes;
	UpdateCurrentFencePosition();
}

void VKStreamBuffer::UpdateCurrentFencePosition()
{
	const u64 counter = GSDeviceVK::GetInstance()->GetCurrentFenceCounter();
	if (!m_tracked_fences.empty() && m_tracked_fences.back().first == counter)
	{
		m_tracked_fences.back().second = m_current_offset;
		return;
	}

	m_tracked_fences.emplace_back(counter, m_current_offset);
}

void VKStreamBuffer::UpdateGPUPosition()
{
	auto start = m_tracked_fences.begin();
	auto end = start;

	const u64 completed_counter = GSDeviceVK::GetInstance()->GetCompletedFenceCounter();
	while (end != m_tracked_fences.end() && completed_counter >= end->first)
	{
		m_current_gpu_position = end->second;
		++end;
	}

	if (start != end)
	{
		m_tracked_fences.erase(start, end);
		if (m_current_offset == m_current_gpu_position)
		{
			m_current_offset = 0;
			m_current_gpu_position = 0;
			m_current_space = m_size;
		}
	}
}

bool VKStreamBuffer::WaitForClearSpace(u32 num_bytes)
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
				new_space = gpu_position - 1;
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
				new_space = available_space_inbetween - 1;
				new_gpu_position = gpu_position;
				return true;
			}
		}
		return false;
	});

	if (iter == m_tracked_fences.end() || iter->first == GSDeviceVK::GetInstance()->GetCurrentFenceCounter())
		return false;

	GSDeviceVK::GetInstance()->WaitForFenceCounter(iter->first);
	m_tracked_fences.erase(
		m_tracked_fences.begin(), m_current_offset == iter->second ? m_tracked_fences.end() : ++iter);
	m_current_offset = new_offset;
	m_current_space = new_space;
	m_current_gpu_position = new_gpu_position;
	return true;
}
