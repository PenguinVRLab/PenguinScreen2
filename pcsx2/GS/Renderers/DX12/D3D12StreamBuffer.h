// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/RedtapeWindows.h"
#include "common/RedtapeWilCom.h"
#include "common/Assertions.h"

#include <directx/d3d12.h>
#include <deque>
#include <utility>

namespace D3D12MA
{
	class Allocation;
}

class D3D12StreamBuffer
{
public:
	D3D12StreamBuffer();
	~D3D12StreamBuffer();

	bool Create(u32 size, bool gpu_backed_buffer = false);

	__fi bool IsValid() const { return static_cast<bool>(m_buffer_upload); }
	__fi ID3D12Resource* GetBuffer() const
	{
		pxAssert(m_buffer_default == nullptr);
		return m_buffer_upload.get();
	}
	__fi D3D12_GPU_VIRTUAL_ADDRESS GetGPUPointer() const { return m_gpu_pointer; }
	__fi void* GetHostPointer() const { return m_host_pointer; }
	__fi void* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
	__fi D3D12_GPU_VIRTUAL_ADDRESS GetCurrentGPUPointer() const { return m_gpu_pointer + m_current_offset; }
	__fi u32 GetSize() const { return m_size; }
	__fi u32 GetCurrentOffset() const { return m_current_offset; }
	__fi u32 GetCurrentSpace() const { return m_current_space; }

	bool ReserveMemory(u32 num_bytes, u32 alignment);
	void CommitMemory(u32 final_num_bytes);
	void FlushMemory();

	void Destroy(bool defer = true);

private:
	void UpdateCurrentFencePosition();
	void UpdateGPUPosition();

	bool WaitForClearSpace(u32 num_bytes);

	u32 m_size = 0;
	u32 m_current_offset = 0;
	u32 m_current_copy_offset = 0;
	u32 m_current_space = 0;
	u32 m_current_gpu_position = 0;

	wil::com_ptr_nothrow<ID3D12Resource> m_buffer_upload;
	wil::com_ptr_nothrow<ID3D12Resource> m_buffer_default;
	wil::com_ptr_nothrow<D3D12MA::Allocation> m_allocation_upload;
	wil::com_ptr_nothrow<D3D12MA::Allocation> m_allocation_default;
	D3D12_GPU_VIRTUAL_ADDRESS m_gpu_pointer = {};
	u8* m_host_pointer = nullptr;

	std::deque<std::pair<u64, u32>> m_tracked_fences;
};
