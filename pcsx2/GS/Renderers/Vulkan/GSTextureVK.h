// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSTexture.h"
#include "GS/GS.h"
#include "GS/Renderers/Vulkan/VKLoader.h"

#include <limits>

class GSTextureVK final : public GSTexture
{
public:
	enum class Layout : u32
	{
		Undefined,
		Preinitialized,
		ColorAttachment,
		DepthStencilAttachment,
		ShaderReadOnly,
		ClearDst,
		TransferSrc,
		TransferDst,
		TransferSelf,
		PresentSrc,
		FeedbackLoop,
		ReadWriteImage,
		ComputeReadWriteImage,
		General,
		Count
	};

	~GSTextureVK() override;

	static std::unique_ptr<GSTextureVK> Create(Usage usage, Format format, int width, int height, int levels, int layers = 1);
	static std::unique_ptr<GSTextureVK> Adopt(
		VkImage image, Usage usage, Format format, int width, int height, int levels, VkFormat vk_format);

	void Destroy(bool defer);

	__fi VkImage GetImage() const { return m_image; }
	__fi VkImageView GetView() const { return m_view; }

	VkImageView GetLayerView(u32 layer);

	GSTexture* GetLayerProxyTexture(u32 layer) override;
	__fi bool IsLayerProxy() const { return m_proxy_parent != nullptr; }
	__fi u32 GetBaseArrayLayer() const { return m_proxy_base_layer; }
	__fi VkImageView GetViewForSampling() const
	{
		return (m_array_layers > 1) ? const_cast<GSTextureVK*>(this)->GetLayerView(0) : m_view;
	}

	__fi Layout GetLayout() const { return m_proxy_parent ? m_proxy_parent->m_layout : m_layout; }
	bool IsShaderWriteMode() const override { return GetLayout() == Layout::ReadWriteImage; }

	__fi VkFormat GetVkFormat() const { return m_vk_format; }

	VkImageLayout GetVkLayout() const;

	void* GetNativeHandle() const override;

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = NULL, int layer = 0) override;
	void Unmap() override;
	void GenerateMipmap() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

	void TransitionToLayout(Layout layout);
	void CommitClear();
	void CommitClear(VkCommandBuffer cmdbuf);

	void OverrideImageLayout(Layout new_layout);

	void TransitionToLayout(VkCommandBuffer command_buffer, Layout new_layout);
	void TransitionSubresourcesToLayout(
		VkCommandBuffer command_buffer, int start_level, int num_levels, Layout old_layout, Layout new_layout);

	static VkFramebuffer CreateNullFramebuffer(u32 w, u32 h);

	VkFramebuffer GetFramebuffer(bool feedback_loop);

	VkFramebuffer GetLinkedFramebuffer(GSTextureVK* depth_texture, bool feedback_loop_color, bool feedback_loop_depth);

	__fi void SetUseFenceCounter(u64 counter)
	{
		m_use_fence_counter = counter;
		if (m_proxy_parent)
			m_proxy_parent->m_use_fence_counter = counter;
	}

private:
	GSTextureVK(Usage usage, Format format, int width, int height, int levels, int layers, VkImage image,
		VmaAllocation allocation, VkImageView view, VkFormat vk_format);

	VkCommandBuffer GetCommandBufferForUpdate();
	void CopyTextureDataForUpload(void* dst, const void* src, u32 pitch, u32 upload_pitch, u32 height) const;
	VkBuffer AllocateUploadStagingBuffer(const void* data, u32 pitch, u32 upload_pitch, u32 height) const;
	void UpdateFromBuffer(VkCommandBuffer cmdbuf, int level, u32 x, u32 y, u32 width, u32 height, u32 buffer_height,
		u32 row_length, VkBuffer buffer, u32 buffer_offset);

	VkImage m_image = VK_NULL_HANDLE;
	VmaAllocation m_allocation = VK_NULL_HANDLE;
	VkImageView m_view = VK_NULL_HANDLE;
	VkFormat m_vk_format = VK_FORMAT_UNDEFINED;
	Layout m_layout = Layout::Undefined;

	std::vector<VkImageView> m_layer_views;

	GSTextureVK* m_proxy_parent = nullptr;
	u32 m_proxy_base_layer = 0;
	std::vector<std::unique_ptr<GSTextureVK>> m_layer_proxies;

	u64 m_use_fence_counter = 0;

	int m_map_level = std::numeric_limits<int>::max();
	GSVector4i m_map_area = GSVector4i::zero();

	std::vector<std::tuple<GSTextureVK*, VkFramebuffer, bool, bool>> m_framebuffers;
};

class GSDownloadTextureVK final : public GSDownloadTexture
{
public:
	~GSDownloadTextureVK() override;

	static std::unique_ptr<GSDownloadTextureVK> Create(u32 width, u32 height, GSTexture::Format format);

	void CopyFromTexture(
		const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch) override;

	bool Map(const GSVector4i& read_rc) override;
	void Unmap() override;

	void Flush() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

private:
	GSDownloadTextureVK(u32 width, u32 height, GSTexture::Format format);

	VmaAllocation m_allocation = VK_NULL_HANDLE;
	VkBuffer m_buffer = VK_NULL_HANDLE;

	u64 m_copy_fence_counter = 0;
	u32 m_buffer_size = 0;

	bool m_needs_cache_invalidate = false;
};
