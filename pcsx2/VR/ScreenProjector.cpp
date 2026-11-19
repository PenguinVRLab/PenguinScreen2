// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/ScreenProjector.h"

#ifdef ENABLE_VULKAN

#include "VR/VRInternal.h"
#include "VR/XRSession.h"
#include "VR/ScreenProjectorEyeRects.h"
#include "VR/SwapchainWait.h"
#include "VR/screenproj/vr_screen_projector.h"
#include "VR/screenproj/screen_projector_glsl.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"

#include "common/Console.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace VR::ScreenProjector
{
	namespace
	{
		constexpr int64_t ONE_SECOND_NS = 1000000000;
		constexpr u32 kSlots = 2;
		constexpr VkDeviceSize kUniformBytes = sizeof(vrsp_eye_uniforms);
		constexpr double kTightFovMargin = 0.08;

		constexpr const char kVertexGlsl[] =
			"#version 450\n"
			"void main() {\n"
			"    vec2 p = vec2(float(gl_VertexIndex & 1) * 4.0 - 1.0, float(gl_VertexIndex & 2) * 2.0 - 1.0);\n"
			"    gl_Position = vec4(p, 0.0, 1.0);\n"
			"}\n";

		struct RingImage
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			u32 width = 0;
			u32 height = 0;
			bool has_layout = false;
		};

		struct Slot
		{
			RingImage images[kMaxSources];
			bool present[kMaxSources] = {};
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
			bool fence_submitted = false;
			VkDescriptorSet ds[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
		};

		struct
		{
			bool initialized = false;
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkFormat ring_format = VK_FORMAT_UNDEFINED;
			bool srgb = false;
			float supersample = 1.5f;
			bool depth_requested = false;

			u32 rec_w = 0, rec_h = 0, max_rect_w = 0, max_rect_h = 0;

			XrSwapchain swapchain = XR_NULL_HANDLE;
			std::vector<XrSwapchainImageVulkan2KHR> images;
			std::vector<VkImageView> image_views;
			std::vector<VkFramebuffer> framebuffers;
			u32 sc_w = 0, sc_h = 0;
			bool ever_released = false;
			bool wait_pending = false;
			uint32_t pending_index = 0;
			SwapchainWait::FailureTracker wait_failures;

			VkRenderPass render_pass = VK_NULL_HANDLE;
			VkDescriptorSetLayout ds_layout = VK_NULL_HANDLE;
			VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkSampler sampler = VK_NULL_HANDLE;
			VkDescriptorPool ds_pool = VK_NULL_HANDLE;
			VkBuffer ubo = VK_NULL_HANDLE;
			VkDeviceMemory ubo_memory = VK_NULL_HANDLE;
			u8* ubo_map = nullptr;
			VkDeviceSize ubo_stride = 0;
			VkCommandPool cmd_pool = VK_NULL_HANDLE;

			Slot slots[kSlots];
			u32 current_slot = 0;
			bool recording = false;
			int newest_slot = -1;
			bool published_this_frame = false;

			XrView last_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
			bool have_last_views = false;
			bool views_invalid_episode = false;

			bool warned_swapchain = false;
			bool warned_locate_never = false;
			bool warned_cylinder = false;
			bool warned_fence = false;
			bool warned_wait = false;
			bool warned_acquire = false;
			bool warned_release = false;
			bool warned_ring = false;
			bool warned_begin = false;
			bool logged_tight_fov = false;
			bool failed = false;
		} p;

		bool CheckXR(XrResult res, const char* what)
		{
			if (XR_SUCCEEDED(res))
				return true;
			char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
			if (XRSession::HasInstance())
				xrResultToString(XRSession::GetInstance(), res, buf);
			Console.Error("(VR) Projector: %s failed: %s (%d)", what, buf, static_cast<int>(res));
			return false;
		}

		u32 FindMemoryType(u32 type_bits, VkMemoryPropertyFlags want)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			VkPhysicalDeviceMemoryProperties mp = {};
			vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
			for (u32 i = 0; i < mp.memoryTypeCount; i++)
			{
				if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
					return i;
			}
			return UINT32_MAX;
		}

		void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
			VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
			VkPipelineStageFlags dst_stage)
		{
			VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcAccessMask = src_access;
			barrier.dstAccessMask = dst_access;
			barrier.oldLayout = old_layout;
			barrier.newLayout = new_layout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}

		void DestroyRingImage(RingImage& ri)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid())
			{
				if (ri.view != VK_NULL_HANDLE)
					vkDestroyImageView(h.device, ri.view, nullptr);
				if (ri.image != VK_NULL_HANDLE)
					vkDestroyImage(h.device, ri.image, nullptr);
				if (ri.memory != VK_NULL_HANDLE)
					vkFreeMemory(h.device, ri.memory, nullptr);
			}
			ri = RingImage();
		}

		bool EnsureRingImage(RingImage& ri, u32 w, u32 h)
		{
			if (ri.image != VK_NULL_HANDLE && ri.width == w && ri.height == h)
				return true;
			DestroyRingImage(ri);
			const Internal::VulkanHandles& h_ = Internal::GetVulkanHandles();
			VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ici.imageType = VK_IMAGE_TYPE_2D;
			ici.format = p.ring_format;
			ici.extent = {w, h, 1};
			ici.mipLevels = 1;
			ici.arrayLayers = 1;
			ici.samples = VK_SAMPLE_COUNT_1_BIT;
			ici.tiling = VK_IMAGE_TILING_OPTIMAL;
			ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
			ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			if (vkCreateImage(h_.device, &ici, nullptr, &ri.image) != VK_SUCCESS)
			{
				ri.image = VK_NULL_HANDLE;
				return false;
			}
			VkMemoryRequirements mr = {};
			vkGetImageMemoryRequirements(h_.device, ri.image, &mr);
			const u32 type = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (type == UINT32_MAX)
			{
				DestroyRingImage(ri);
				return false;
			}
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = mr.size;
			mai.memoryTypeIndex = type;
			if (vkAllocateMemory(h_.device, &mai, nullptr, &ri.memory) != VK_SUCCESS ||
				vkBindImageMemory(h_.device, ri.image, ri.memory, 0) != VK_SUCCESS)
			{
				DestroyRingImage(ri);
				return false;
			}
			VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.image = ri.image;
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vci.format = p.ring_format;
			vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			if (vkCreateImageView(h_.device, &vci, nullptr, &ri.view) != VK_SUCCESS)
			{
				ri.view = VK_NULL_HANDLE;
				DestroyRingImage(ri);
				return false;
			}
			ri.width = w;
			ri.height = h;
			ri.has_layout = false;
			return true;
		}

		bool WaitSlotFence(Slot& s)
		{
			if (!s.fence_submitted)
				return true;
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			const VkResult wr = vkWaitForFences(h.device, 1, &s.fence, VK_TRUE, ONE_SECOND_NS);
			if (wr != VK_SUCCESS)
			{
				if (!p.warned_fence)
				{
					p.warned_fence = true;
					Console.Warning("(VR) Projector: fence wait returned %d; skipping this frame.", static_cast<int>(wr));
				}
				return false;
			}
			vkResetFences(h.device, 1, &s.fence);
			s.fence_submitted = false;
			return true;
		}

		bool BeginRecording()
		{
			if (p.recording)
				return true;
			Slot& s = p.slots[p.current_slot];
			if (!WaitSlotFence(s))
				return false;
			if (p.newest_slot == static_cast<int>(p.current_slot))
			{
				if (!WaitSlotFence(p.slots[(p.current_slot + 1) % kSlots]))
					return false;
			}
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if (vkBeginCommandBuffer(s.cmd, &bi) != VK_SUCCESS)
			{
				if (!p.warned_begin)
				{
					p.warned_begin = true;
					Console.Error("(VR) Projector: vkBeginCommandBuffer failed; frames skipped.");
				}
				return false;
			}
			p.recording = true;
			return true;
		}

		bool SubmitRecording()
		{
			if (!p.recording)
				return false;
			p.recording = false;
			Slot& s = p.slots[p.current_slot];
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			bool submitted = false;
			if (vkEndCommandBuffer(s.cmd) == VK_SUCCESS)
			{
				VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
				si.commandBufferCount = 1;
				si.pCommandBuffers = &s.cmd;
				submitted = (vkQueueSubmit(h.queue, 1, &si, s.fence) == VK_SUCCESS);
				s.fence_submitted = submitted;
			}
			if (!submitted)
				Console.Error("(VR) Projector: command buffer submit failed; this frame is dropped.");
			p.current_slot = (p.current_slot + 1) % kSlots;
			p.published_this_frame = false;
			return submitted;
		}

		void WaitAllSlots()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (!h.IsValid())
				return;
			for (Slot& sl : p.slots)
			{
				if (!sl.fence_submitted || sl.fence == VK_NULL_HANDLE)
					continue;
				if (vkWaitForFences(h.device, 1, &sl.fence, VK_TRUE, ONE_SECOND_NS) == VK_SUCCESS)
					sl.fence_submitted = false;
				else
					Console.Warning("(VR) Projector: a pass was still busy after 1s at shutdown; tearing down anyway.");
			}
		}

		void DestroySwapchainResources()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid())
			{
				for (VkFramebuffer fb : p.framebuffers)
					vkDestroyFramebuffer(h.device, fb, nullptr);
				for (VkImageView v : p.image_views)
					vkDestroyImageView(h.device, v, nullptr);
			}
			p.framebuffers.clear();
			p.image_views.clear();
			if (p.swapchain != XR_NULL_HANDLE)
			{
				xrDestroySwapchain(p.swapchain);
				p.swapchain = XR_NULL_HANDLE;
			}
			p.images.clear();
			p.sc_w = p.sc_h = 0;
			p.ever_released = false;
			p.wait_pending = false;
			p.wait_failures = {};
		}

		bool EnsureSwapchain()
		{
			if (p.swapchain != XR_NULL_HANDLE)
				return true;
			if (p.warned_swapchain)
			{
				p.failed = true;
				return false;
			}
			const XRSession::RuntimeLimits& lim = XRSession::GetRuntimeLimits();
			u32 w = std::max(1u, static_cast<u32>(std::lround(static_cast<double>(p.rec_w) * p.supersample)));
			u32 h = std::max(1u, static_cast<u32>(std::lround(static_cast<double>(p.rec_h) * p.supersample)));
			if (p.max_rect_w != 0)
				w = std::min(w, p.max_rect_w);
			if (p.max_rect_h != 0)
				h = std::min(h, p.max_rect_h);
			if (lim.max_swapchain_width != 0)
				w = std::min(w, lim.max_swapchain_width);
			if (lim.max_swapchain_height != 0)
				h = std::min(h, lim.max_swapchain_height);

			XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
			ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			ci.format = static_cast<int64_t>(p.format);
			ci.sampleCount = 1;
			ci.width = w;
			ci.height = h;
			ci.faceCount = 1;
			ci.arraySize = 2;
			ci.mipCount = 1;
			XrResult res;
			{
				res = xrCreateSwapchain(XRSession::GetSession(), &ci, &p.swapchain);
			}
			if (XR_FAILED(res))
			{
				p.swapchain = XR_NULL_HANDLE;
				p.warned_swapchain = true;
				Console.Error("(VR) Projector: xrCreateSwapchain failed (%d) for %ux%u x2; the projector is off this "
							  "session and the compositor falls to the flat quad.",
					static_cast<int>(res), w, h);
				p.failed = true;
				return false;
			}
			uint32_t count = 0;
			if (!CheckXR(xrEnumerateSwapchainImages(p.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages") || count == 0)
			{
				p.warned_swapchain = true;
				p.failed = true;
				DestroySwapchainResources();
				return false;
			}
			p.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
			if (!CheckXR(xrEnumerateSwapchainImages(p.swapchain, count, &count,
							 reinterpret_cast<XrSwapchainImageBaseHeader*>(p.images.data())),
					"xrEnumerateSwapchainImages"))
			{
				p.warned_swapchain = true;
				p.failed = true;
				DestroySwapchainResources();
				return false;
			}
			const Internal::VulkanHandles& vh = Internal::GetVulkanHandles();
			p.image_views.assign(count * 2, VK_NULL_HANDLE);
			p.framebuffers.assign(count * 2, VK_NULL_HANDLE);
			for (uint32_t i = 0; i < count; i++)
			{
				for (u32 eye = 0; eye < 2; eye++)
				{
					VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
					vci.image = p.images[i].image;
					vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
					vci.format = p.format;
					vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
					VkImageView& view = p.image_views[i * 2 + eye];
					if (vkCreateImageView(vh.device, &vci, nullptr, &view) != VK_SUCCESS)
					{
						view = VK_NULL_HANDLE;
						p.warned_swapchain = true;
						p.failed = true;
						Console.Error("(VR) Projector: vkCreateImageView failed for the swapchain; the compositor falls to the flat quad.");
						DestroySwapchainResources();
						return false;
					}
					VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
					fci.renderPass = p.render_pass;
					fci.attachmentCount = 1;
					fci.pAttachments = &view;
					fci.width = w;
					fci.height = h;
					fci.layers = 1;
					VkFramebuffer& fb = p.framebuffers[i * 2 + eye];
					if (vkCreateFramebuffer(vh.device, &fci, nullptr, &fb) != VK_SUCCESS)
					{
						fb = VK_NULL_HANDLE;
						p.warned_swapchain = true;
						p.failed = true;
						Console.Error("(VR) Projector: vkCreateFramebuffer failed for the swapchain; the compositor falls to the flat quad.");
						DestroySwapchainResources();
						return false;
					}
				}
			}
			p.sc_w = w;
			p.sc_h = h;
			Console.WriteLn("(VR) Projector swapchain created: %ux%u x2 (recommended %ux%u x k=%.2f, max rect %ux%u, "
							"runtime max %ux%u), %u images, format %d; created once, never resized for an aspect change.",
				w, h, p.rec_w, p.rec_h, p.supersample, p.max_rect_w, p.max_rect_h, lim.max_swapchain_width,
				lim.max_swapchain_height, count, static_cast<int>(p.format));
			return true;
		}

		bool LocateViews(XrSpace space, XrTime display_time, XrView out[2])
		{
			XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
			vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			vli.displayTime = display_time;
			vli.space = space;
			XrViewState vs = {XR_TYPE_VIEW_STATE};
			XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
			uint32_t count = 0;
			const XrResult res = xrLocateViews(XRSession::GetSession(), &vli, &vs, 2, &count, views);
			const bool valid = XR_SUCCEEDED(res) && count == 2 &&
							   (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
							   (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
			if (valid)
			{
				if (p.views_invalid_episode)
				{
					p.views_invalid_episode = false;
					Console.WriteLn("(VR) Projector: xrLocateViews valid again.");
				}
				p.last_views[0] = views[0];
				p.last_views[1] = views[1];
				p.have_last_views = true;
				out[0] = views[0];
				out[1] = views[1];
				return true;
			}
			if (!p.views_invalid_episode)
			{
				p.views_invalid_episode = true;
				Console.Warning("(VR) Projector: xrLocateViews returned %d, flags 0x%llx; %s.", static_cast<int>(res),
					static_cast<unsigned long long>(vs.viewStateFlags),
					p.have_last_views ? "rendering with the last valid views until they return" : "no views yet, no screen layer");
			}
			if (!p.have_last_views)
				return false;
			out[0] = p.last_views[0];
			out[1] = p.last_views[1];
			return true;
		}

		bool CreatePass()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			VkDevice dev = h.device;

			Vulkan::RenderPassBuilder rpb;
			rpb.AddAttachment(p.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			const u32 sp = rpb.AddSubpass();
			rpb.AddSubpassColorAttachment(sp, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			p.render_pass = rpb.Create(dev);
			if (p.render_pass == VK_NULL_HANDLE)
				return false;

			Vulkan::DescriptorSetLayoutBuilder dslb;
			dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
			dslb.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSources, VK_SHADER_STAGE_FRAGMENT_BIT);
			p.ds_layout = dslb.Create(dev);
			if (p.ds_layout == VK_NULL_HANDLE)
				return false;
			Vulkan::PipelineLayoutBuilder plb;
			plb.AddDescriptorSet(p.ds_layout);
			p.pipeline_layout = plb.Create(dev);
			if (p.pipeline_layout == VK_NULL_HANDLE)
				return false;

			if (!g_vulkan_shader_cache)
			{
				Console.Error("(VR) Projector: no Vulkan shader cache (GS device not up); cannot compile the pass.");
				return false;
			}
			VkShaderModule vs = g_vulkan_shader_cache->GetVertexShader(kVertexGlsl);
			VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(ScreenProjectorShader::kFragmentGlsl);
			if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
			{
				Console.Error("(VR) Projector: the pass's shaders did not compile (vertex %s, fragment %s); see the bad-shader dump in the logs folder.",
					vs ? "ok" : "FAILED", fs ? "ok" : "FAILED");
				return false;
			}
			Vulkan::GraphicsPipelineBuilder gpb;
			gpb.SetVertexShader(vs);
			gpb.SetFragmentShader(fs);
			gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			gpb.SetNoCullRasterizationState();
			gpb.SetNoDepthTestState();
			gpb.SetNoBlendingState();
			gpb.SetDynamicViewportAndScissorState();
			gpb.SetMultisamples(VK_SAMPLE_COUNT_1_BIT);
			gpb.SetPipelineLayout(p.pipeline_layout);
			gpb.SetRenderPass(p.render_pass, 0);
			p.pipeline = gpb.Create(dev, g_vulkan_shader_cache->GetPipelineCache());
			if (p.pipeline == VK_NULL_HANDLE)
				return false;

			Vulkan::SamplerBuilder sb;
			sb.SetLinearSampler(false, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
			p.sampler = sb.Create(dev);
			if (p.sampler == VK_NULL_HANDLE)
				return false;

			VkPhysicalDeviceProperties props = {};
			vkGetPhysicalDeviceProperties(h.physical_device, &props);
			const VkDeviceSize align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
			p.ubo_stride = ((kUniformBytes + align - 1) / align) * align;
			VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = p.ubo_stride * kSlots * 2;
			bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
			bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			if (vkCreateBuffer(dev, &bci, nullptr, &p.ubo) != VK_SUCCESS)
				return false;
			VkMemoryRequirements mr = {};
			vkGetBufferMemoryRequirements(dev, p.ubo, &mr);
			const u32 type = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if (type == UINT32_MAX)
				return false;
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = mr.size;
			mai.memoryTypeIndex = type;
			if (vkAllocateMemory(dev, &mai, nullptr, &p.ubo_memory) != VK_SUCCESS ||
				vkBindBufferMemory(dev, p.ubo, p.ubo_memory, 0) != VK_SUCCESS)
				return false;
			void* map = nullptr;
			if (vkMapMemory(dev, p.ubo_memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
				return false;
			p.ubo_map = static_cast<u8*>(map);

			const VkDescriptorPoolSize sizes[2] = {
				{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots * 2},
				{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlots * 2 * kMaxSources}};
			VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			dpci.maxSets = kSlots * 2;
			dpci.poolSizeCount = 2;
			dpci.pPoolSizes = sizes;
			if (vkCreateDescriptorPool(dev, &dpci, nullptr, &p.ds_pool) != VK_SUCCESS)
				return false;
			for (u32 s = 0; s < kSlots; s++)
			{
				for (u32 eye = 0; eye < 2; eye++)
				{
					VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
					dsai.descriptorPool = p.ds_pool;
					dsai.descriptorSetCount = 1;
					dsai.pSetLayouts = &p.ds_layout;
					if (vkAllocateDescriptorSets(dev, &dsai, &p.slots[s].ds[eye]) != VK_SUCCESS)
						return false;
				}
			}

			VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			pci.queueFamilyIndex = h.queue_family;
			if (vkCreateCommandPool(dev, &pci, nullptr, &p.cmd_pool) != VK_SUCCESS)
				return false;
			for (u32 s = 0; s < kSlots; s++)
			{
				VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				cai.commandPool = p.cmd_pool;
				cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				cai.commandBufferCount = 1;
				if (vkAllocateCommandBuffers(dev, &cai, &p.slots[s].cmd) != VK_SUCCESS)
					return false;
				VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
				if (vkCreateFence(dev, &fci, nullptr, &p.slots[s].fence) != VK_SUCCESS)
					return false;
			}
			return true;
		}

		void DestroyPass()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			const bool alive = h.IsValid();
			VkDevice dev = h.device;
			for (Slot& s : p.slots)
			{
				if (alive && s.fence != VK_NULL_HANDLE)
				{
					if (s.fence_submitted)
						vkWaitForFences(dev, 1, &s.fence, VK_TRUE, ONE_SECOND_NS);
					vkDestroyFence(dev, s.fence, nullptr);
				}
				s.fence = VK_NULL_HANDLE;
				s.fence_submitted = false;
				if (alive && s.cmd != VK_NULL_HANDLE && p.cmd_pool != VK_NULL_HANDLE)
					vkFreeCommandBuffers(dev, p.cmd_pool, 1, &s.cmd);
				s.cmd = VK_NULL_HANDLE;
				for (RingImage& ri : s.images)
					DestroyRingImage(ri);
				for (bool& b : s.present)
					b = false;
				s.ds[0] = s.ds[1] = VK_NULL_HANDLE;
			}
			if (alive)
			{
				if (p.cmd_pool != VK_NULL_HANDLE)
					vkDestroyCommandPool(dev, p.cmd_pool, nullptr);
				if (p.ds_pool != VK_NULL_HANDLE)
					vkDestroyDescriptorPool(dev, p.ds_pool, nullptr);
				if (p.ubo_map)
					vkUnmapMemory(dev, p.ubo_memory);
				if (p.ubo != VK_NULL_HANDLE)
					vkDestroyBuffer(dev, p.ubo, nullptr);
				if (p.ubo_memory != VK_NULL_HANDLE)
					vkFreeMemory(dev, p.ubo_memory, nullptr);
				if (p.sampler != VK_NULL_HANDLE)
					vkDestroySampler(dev, p.sampler, nullptr);
				if (p.pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(dev, p.pipeline, nullptr);
				if (p.pipeline_layout != VK_NULL_HANDLE)
					vkDestroyPipelineLayout(dev, p.pipeline_layout, nullptr);
				if (p.ds_layout != VK_NULL_HANDLE)
					vkDestroyDescriptorSetLayout(dev, p.ds_layout, nullptr);
				if (p.render_pass != VK_NULL_HANDLE)
					vkDestroyRenderPass(dev, p.render_pass, nullptr);
			}
			p.cmd_pool = VK_NULL_HANDLE;
			p.ds_pool = VK_NULL_HANDLE;
			p.ubo_map = nullptr;
			p.ubo = VK_NULL_HANDLE;
			p.ubo_memory = VK_NULL_HANDLE;
			p.sampler = VK_NULL_HANDLE;
			p.pipeline = VK_NULL_HANDLE;
			p.pipeline_layout = VK_NULL_HANDLE;
			p.ds_layout = VK_NULL_HANDLE;
			p.render_pass = VK_NULL_HANDLE;
		}

		void WriteDescriptorSet(u32 slot, u32 eye, const Slot& newest)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			VkImageView fallback = VK_NULL_HANDLE;
			for (u32 k = 0; k < kMaxSources; k++)
			{
				if (newest.present[k] && newest.images[k].view != VK_NULL_HANDLE)
				{
					fallback = newest.images[k].view;
					break;
				}
			}
			VkDescriptorImageInfo images[kMaxSources];
			for (u32 k = 0; k < kMaxSources; k++)
			{
				images[k].sampler = p.sampler;
				images[k].imageView = (newest.present[k] && newest.images[k].view != VK_NULL_HANDLE) ? newest.images[k].view : fallback;
				images[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			}
			VkDescriptorBufferInfo buffer = {};
			buffer.buffer = p.ubo;
			buffer.offset = p.ubo_stride * (slot * 2 + eye);
			buffer.range = kUniformBytes;
			VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
			writes[0].dstSet = p.slots[slot].ds[eye];
			writes[0].dstBinding = 0;
			writes[0].descriptorCount = 1;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			writes[0].pBufferInfo = &buffer;
			writes[1].dstSet = p.slots[slot].ds[eye];
			writes[1].dstBinding = 1;
			writes[1].descriptorCount = kMaxSources;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[1].pImageInfo = images;
			vkUpdateDescriptorSets(h.device, 2, writes, 0, nullptr);
		}
	}

	bool IsInitialized()
	{
		return p.initialized;
	}

	bool Initialize(VkFormat swapchain_format, bool srgb, float supersample, bool depth_requested)
	{
		if (p.initialized)
			return true;
		const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
		if (!h.IsValid() || !XRSession::HasSession())
		{
			Console.Error("(VR) Projector: no Vulkan device or XR session; the curved screen falls back to flat.");
			return false;
		}
		p.format = swapchain_format;
		p.srgb = srgb;
		p.ring_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		p.supersample = std::clamp(supersample, 0.5f, 2.0f);
		p.depth_requested = depth_requested;
		p.warned_swapchain = p.warned_locate_never = p.warned_cylinder = p.warned_fence = false;
		p.warned_wait = p.warned_acquire = p.warned_release = p.warned_ring = p.warned_begin = false;
		p.logged_tight_fov = false;
		p.failed = false;
		p.have_last_views = false;
		p.views_invalid_episode = false;
		p.newest_slot = -1;
		p.current_slot = 0;
		p.recording = false;
		p.published_this_frame = false;

		{
			XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
			uint32_t count = 0;
			if (!CheckXR(xrEnumerateViewConfigurationViews(XRSession::GetInstance(), XRSession::GetSystemId(),
							 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views),
					"xrEnumerateViewConfigurationViews") ||
				count < 2)
			{
				Console.Error("(VR) Projector: the runtime reports no stereo view sizes; falling back to flat.");
				return false;
			}
			p.rec_w = std::max(views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth);
			p.rec_h = std::max(views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight);
			p.max_rect_w = std::min(views[0].maxImageRectWidth, views[1].maxImageRectWidth);
			p.max_rect_h = std::min(views[0].maxImageRectHeight, views[1].maxImageRectHeight);
			if (p.rec_w == 0 || p.rec_h == 0)
			{
				Console.Error("(VR) Projector: recommended view size is zero; falling back to flat.");
				return false;
			}
		}

		if (!CreatePass())
		{
			Console.Error("(VR) Projector: could not create the pass; the curved screen falls back to flat this session.");
			DestroyPass();
			return false;
		}

		p.initialized = true;
		const XRSession::RuntimeLimits& lim = XRSession::GetRuntimeLimits();
		Console.WriteLn("(VR) Curved-screen projector ready: recommended view %ux%u, k=%.2f, fovMutable=%s (tight fov %s), "
						"ring %s, %u slots.",
			p.rec_w, p.rec_h, p.supersample, lim.fov_mutable_known ? (lim.fov_mutable ? "true" : "false") : "unknown",
			lim.fov_mutable ? "on" : "off", srgb ? "sRGB" : "UNORM", kSlots);
		Console.Warning("(VR) Curved-screen projector submits at CONTENT rate from the emulator's frame loop (the staged "
						"exception). This mode is for comparing geometry and colour with the native cylinder; text will "
						"shimmer under head motion until the display-rate presenter lands.");
		if (depth_requested)
			Console.Warning("(VR) Projector: screen.projectorDepth is on, but the depth layer is not built in this "
							"package; rendering without depth.");
		return true;
	}

	void Shutdown()
	{
		if (!p.initialized)
			return;
		WaitAllSlots();
		DestroySwapchainResources();
		DestroyPass();
		p.initialized = false;
		p.failed = false;
		p.recording = false;
		p.published_this_frame = false;
		p.have_last_views = false;
		p.newest_slot = -1;
		Console.WriteLn("(VR) Curved-screen projector shut down.");
	}

	void Publish(const Source& src, u32 index)
	{
		if (!p.initialized || index >= kMaxSources || !src.texture || src.w <= 0 || src.h <= 0)
			return;
		if (!BeginRecording())
			return;
		Slot& s = p.slots[p.current_slot];
		if (!p.published_this_frame)
		{
			for (bool& b : s.present)
				b = false;
			p.published_this_frame = true;
			p.newest_slot = static_cast<int>(p.current_slot);
		}
		RingImage& ri = s.images[index];
		if (!EnsureRingImage(ri, static_cast<u32>(src.w), static_cast<u32>(src.h)))
		{
			if (!p.warned_ring)
			{
				p.warned_ring = true;
				Console.Error("(VR) Projector: could not allocate a %dx%d ring image; that source is skipped.", src.w, src.h);
			}
			return;
		}
		VkCommandBuffer cmd = s.cmd;
		src.texture->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
		ImageBarrier(cmd, ri.image, ri.has_layout ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ri.has_layout ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
			ri.has_layout ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkImageCopy region = {};
		region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src.layer, 1};
		region.srcOffset = {src.x, src.y, 0};
		region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.dstOffset = {0, 0, 0};
		region.extent = {static_cast<u32>(src.w), static_cast<u32>(src.h), 1};
		vkCmdCopyImage(cmd, src.texture->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ri.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		ImageBarrier(cmd, ri.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		ri.has_layout = true;
		s.present[index] = true;
	}

	void FlushPublish()
	{
		if (p.initialized && p.recording)
			SubmitRecording();
	}

	bool Failed()
	{
		return p.initialized && p.failed;
	}

	bool HasSource(u32 index)
	{
		if (!p.initialized || p.newest_slot < 0 || index >= kMaxSources)
			return false;
		const Slot& s = p.slots[p.newest_slot];
		return s.present[index] && s.images[index].view != VK_NULL_HANDLE;
	}

	bool Present(XrSpace space, XrTime display_time, const Cylinder* cyls, u32 cyl_count, Frame& out)
	{
		if (!p.initialized)
			return false;

		vrsp_cylinder_list list;
		vrsp_list_clear(&list);
		for (u32 i = 0; i < cyl_count; i++)
		{
			vrsp_cylinder c;
			c.pose.orientation = {cyls[i].pose.orientation.x, cyls[i].pose.orientation.y, cyls[i].pose.orientation.z,
				cyls[i].pose.orientation.w};
			c.pose.position = {cyls[i].pose.position.x, cyls[i].pose.position.y, cyls[i].pose.position.z};
			c.radius = cyls[i].radius;
			c.central_angle = cyls[i].central_angle;
			c.aspect_ratio = cyls[i].aspect_ratio;
			c.eye_mask = cyls[i].eye_mask;
			c.source = cyls[i].source;
			const int err = vrsp_list_add(&list, &c);
			if (err != VRSP_OK && !p.warned_cylinder)
			{
				p.warned_cylinder = true;
				Console.Warning("(VR) Projector: cylinder %u refused (code %d: radius %.3f, angle %.3f rad, aspect %.3f, mask %u); "
								"it is not drawn.",
					i, err, c.radius, c.central_angle, c.aspect_ratio, c.eye_mask);
			}
		}

		if (list.count == 0 || p.newest_slot < 0 || !EnsureSwapchain())
		{
			SubmitRecording();
			return false;
		}

		XrView views[2];
		if (!LocateViews(space, display_time, views))
		{
			SubmitRecording();
			return false;
		}

		uint32_t index = 0;
		if (p.wait_pending)
		{
			index = p.pending_index;
		}
		else
		{
			XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			const XrResult res = xrAcquireSwapchainImage(p.swapchain, &ai, &index);
			if (XR_FAILED(res))
			{
				if (!p.warned_acquire)
				{
					p.warned_acquire = true;
					Console.Error("(VR) Projector: xrAcquireSwapchainImage failed (%d).", static_cast<int>(res));
				}
				SubmitRecording();
				return false;
			}
			p.pending_index = index;
			p.wait_pending = true;
		}
		{
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = SwapchainWait::Budget(SwapchainWait::kPresentWaitBudgetNs).Next();
			const XrResult res = xrWaitSwapchainImage(p.swapchain, &wi);
			const SwapchainWait::Action act = SwapchainWait::OnWait(p.wait_failures, res);
			if (act == SwapchainWait::Action::Reacquire)
				p.wait_pending = false;
			if (act != SwapchainWait::Action::Use)
			{
				if (!p.warned_wait)
				{
					p.warned_wait = true;
					Console.Warning("(VR) Projector: xrWaitSwapchainImage returned %d; no screen layer this frame.", static_cast<int>(res));
				}
				SubmitRecording();
				return false;
			}
			p.wait_pending = false;
		}

		bool rendered = false;
		if (BeginRecording())
		{
			Slot& s = p.slots[p.current_slot];
			const Slot& newest = p.slots[p.newest_slot];
			const bool tight = XRSession::GetRuntimeLimits().fov_mutable;
			VkCommandBuffer cmd = s.cmd;
			vrsp_pose eye_poses[2];
			vrsp_fov located_fovs[2];
			vrsp_fov declared_fovs[2];
			vrsp_rect rects[2];
			bool draws[2] = {true, true};
			for (u32 eye = 0; eye < 2; eye++)
			{
				eye_poses[eye] = {
					{views[eye].pose.orientation.x, views[eye].pose.orientation.y, views[eye].pose.orientation.z,
						views[eye].pose.orientation.w},
					{views[eye].pose.position.x, views[eye].pose.position.y, views[eye].pose.position.z}};
				located_fovs[eye] = {views[eye].fov.angleLeft, views[eye].fov.angleRight, views[eye].fov.angleUp,
					views[eye].fov.angleDown};
				declared_fovs[eye] = located_fovs[eye];
				rects[eye] = {0, 0, static_cast<int>(p.sc_w), static_cast<int>(p.sc_h)};
				if (tight)
				{
					vrsp_fov box;
					vrsp_rect box_rect;
					int crosses = 0;
					if (vrsp_tight_fov(&list, static_cast<int>(eye), &eye_poses[eye], &located_fovs[eye],
							static_cast<int>(p.sc_w), static_cast<int>(p.sc_h), kTightFovMargin, &box, &box_rect, &crosses))
					{
						declared_fovs[eye] = box;
						rects[eye] = box_rect;
					}
					else
					{
						draws[eye] = false;
						rects[eye] = {0, 0, 0, 0};
					}
				}
			}
			const vrsp_rect alone_rects[2] = {rects[0], rects[1]};
			if (tight)
			{
				MatchEyeRects(located_fovs, static_cast<int>(p.sc_w), static_cast<int>(p.sc_h), rects, declared_fovs);
			}
			for (u32 eye = 0; eye < 2; eye++)
			{
				const vrsp_fov& declared = declared_fovs[eye];
				const vrsp_rect& rect = rects[eye];
				bool draw = draws[eye];
				vrsp_eye_uniforms u;
				if (vrsp_pack_eye(&list, static_cast<int>(eye), &eye_poses[eye], &declared, rect, &u) != VRSP_OK)
					draw = false;
				std::memcpy(p.ubo_map + p.ubo_stride * (p.current_slot * 2 + eye), &u, sizeof(u));
				WriteDescriptorSet(p.current_slot, eye, newest);

				VkClearValue clear = {};
				clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
				VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
				rpbi.renderPass = p.render_pass;
				rpbi.framebuffer = p.framebuffers[index * 2 + eye];
				rpbi.renderArea = {{rect.x, rect.y}, {static_cast<u32>(rect.w), static_cast<u32>(rect.h)}};
				rpbi.clearValueCount = 1;
				rpbi.pClearValues = &clear;
				vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
				if (draw && u.meta[1] > 0)
				{
					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
					const VkViewport vp = {static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.w),
						static_cast<float>(rect.h), 0.0f, 1.0f};
					const VkRect2D sc = {{rect.x, rect.y}, {static_cast<u32>(rect.w), static_cast<u32>(rect.h)}};
					vkCmdSetViewport(cmd, 0, 1, &vp);
					vkCmdSetScissor(cmd, 0, 1, &sc);
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline_layout, 0, 1, &s.ds[eye], 0, nullptr);
					vkCmdDraw(cmd, 3, 1, 0, 0);
				}
				vkCmdEndRenderPass(cmd);

				XrCompositionLayerProjectionView& v = out.views[eye];
				v.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				v.next = nullptr;
				v.pose = views[eye].pose;
				v.fov = {declared.angle_left, declared.angle_right, declared.angle_up, declared.angle_down};
				v.subImage = {p.swapchain, {{rect.x, rect.y}, {rect.w, rect.h}}, eye};
			}
			if (tight && !p.logged_tight_fov)
			{
				p.logged_tight_fov = true;
				Console.WriteLn("(VR) Projector: tight per-eye fov in use (fovMutable=true): left eye rect %dx%d of %ux%u this frame.",
					out.views[0].subImage.imageRect.extent.width, out.views[0].subImage.imageRect.extent.height, p.sc_w, p.sc_h);
				Console.WriteLn("(VR) Projector: both eyes' rects share one size, %dx%d (alone this frame: left %dx%d, right %dx%d).",
					rects[0].w, rects[0].h, alone_rects[0].w, alone_rects[0].h, alone_rects[1].w, alone_rects[1].h);
			}
			rendered = SubmitRecording();
		}
		else
		{
			SubmitRecording();
		}

		{
			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			const XrResult res = xrReleaseSwapchainImage(p.swapchain, &ri);
			if (XR_FAILED(res))
			{
				if (!p.warned_release)
				{
					p.warned_release = true;
					Console.Error("(VR) Projector: xrReleaseSwapchainImage failed (%d).", static_cast<int>(res));
				}
				return false;
			}
		}
		if (!rendered)
			return false;
		p.ever_released = true;

		out.layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		out.layer.next = nullptr;
		out.layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		out.layer.space = space;
		out.layer.viewCount = 2;
		out.layer.views = out.views;
		return true;
	}
}

#endif
