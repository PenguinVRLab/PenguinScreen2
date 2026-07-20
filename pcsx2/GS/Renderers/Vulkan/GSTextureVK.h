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

	// PCSX2-VR (M4.3-pre): an optional array-layer count enables stereo/multiview render
	// targets. Defaults to 1 layer, keeping every existing caller and single-layer behavior
	// byte-identical. When layers > 1 the primary view (GetView()) is a 2D_ARRAY covering
	// all layers; per-layer single-view accessors are created lazily via GetLayerView().
	static std::unique_ptr<GSTextureVK> Create(Usage usage, Format format, int width, int height, int levels, int layers = 1);
	static std::unique_ptr<GSTextureVK> Adopt(
		VkImage image, Usage usage, Format format, int width, int height, int levels, VkFormat vk_format);

	void Destroy(bool defer);

	__fi VkImage GetImage() const { return m_image; }
	__fi VkImageView GetView() const { return m_view; }

	// PCSX2-VR (M4.3-pre): lazily-created single-layer 2D view of one array layer, for the
	// per-eye merge/compositor path. Returns m_view's layer for single-layer textures too.
	VkImageView GetLayerView(u32 layer);

	// PCSX2-VR (M4.3): layer-proxy support. A proxy is a non-owning GSTextureVK aliasing one
	// layer of its parent array texture: same VkImage, view = the parent's layer view, its
	// own lazily-built framebuffers. Layout/fence state routes to the parent (whose barriers
	// span all layers), so parent and proxies share one coherent layout. Update/Map are
	// forbidden on proxies. Owned by (and destroyed just before) the parent.
	GSTexture* GetLayerProxyTexture(u32 layer) override;
	__fi bool IsLayerProxy() const { return m_proxy_parent != nullptr; }
	/// First layer this texture addresses in copies: the aliased layer for proxies, 0 otherwise.
	__fi u32 GetBaseArrayLayer() const { return m_proxy_base_layer; }
	/// View to bind when SAMPLING this texture through a plain 2D sampler: layer 0 for array
	/// textures (Phase-A rule — mono consumers read the left eye), m_view otherwise. Const
	/// because lazily creating the cached layer view is logically const (callers hold
	/// const pointers at descriptor-build time).
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

	// Used when the render pass is changing the image layout, or to force it to
	// VK_IMAGE_LAYOUT_UNDEFINED, if the existing contents of the image is
	// irrelevant and will not be loaded.
	void OverrideImageLayout(Layout new_layout);

	void TransitionToLayout(VkCommandBuffer command_buffer, Layout new_layout);
	void TransitionSubresourcesToLayout(
		VkCommandBuffer command_buffer, int start_level, int num_levels, Layout old_layout, Layout new_layout);

	static VkFramebuffer CreateNullFramebuffer(u32 w, u32 h);

	/// Framebuffers are lazily allocated.
	VkFramebuffer GetFramebuffer(bool feedback_loop);

	VkFramebuffer GetLinkedFramebuffer(GSTextureVK* depth_texture, bool feedback_loop_color, bool feedback_loop_depth);

	// Call when the texture is bound to the pipeline, or read from in a copy.
	__fi void SetUseFenceCounter(u64 counter)
	{
		m_use_fence_counter = counter;
		// Keep the parent alive/undeleted for as long as any proxy is in flight.
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

	// PCSX2-VR (M4.3-pre): m_layer_views holds the lazily-created single-layer views for
	// array textures (the layer count itself lives in GSTexture::m_array_layers).
	std::vector<VkImageView> m_layer_views;

	// PCSX2-VR (M4.3): layer-proxy plumbing (see GetLayerProxyTexture).
	GSTextureVK* m_proxy_parent = nullptr;
	u32 m_proxy_base_layer = 0;
	std::vector<std::unique_ptr<GSTextureVK>> m_layer_proxies;

	// Contains the fence counter when the texture was last used.
	// When this matches the current fence counter, the texture was used this command buffer.
	u64 m_use_fence_counter = 0;

	int m_map_level = std::numeric_limits<int>::max();
	GSVector4i m_map_area = GSVector4i::zero();

	// linked framebuffer is combined with depth texture
	// list of color textures this depth texture is linked to or vice versa
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
