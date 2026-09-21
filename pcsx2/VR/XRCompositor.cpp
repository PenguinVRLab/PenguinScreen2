// SPDX-FileCopyrightText: 2026 Patrick Carey
// SPDX-License-Identifier: GPL-3.0

#include "VR/XRCompositor.h"
#include "VR/SplitState.h"
#include "VR/ControlQuads.h"
#include "VR/SpatialControls.h"
#include "VR/VRManager.h"
#include "VR/SeatCast.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "VR/SeatSession.h"
#include "VR/HeadPose.h"
#include "VR/VRInternal.h"
#include "VR/VRInput.h"
#include "VR/XRSession.h"

#ifdef ENABLE_VULKAN
#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#endif

#include "common/Console.h"
#include "common/Threading.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef ENABLE_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>
#endif

namespace VR::XRCompositor
{
	namespace
	{
		struct ScreenParams
		{
			float distance = 2.0f;
			float height = 1.4f;
			float arc_deg = 0.0f;
			float voffset = 0.0f;
			bool follow_head = false;
		};
		std::mutex s_screen_mutex;
		ScreenParams s_screen_params;

		std::atomic<bool> s_reanchor_requested{false};

		ScreenAnchor s_anchor_snapshot;

		bool s_xrfault_begin1_fired = false;
	}

#ifdef ENABLE_VULKAN
	namespace
	{
		constexpr int64_t ONE_SECOND_NS = 1000000000;
		constexpr u32 NUM_CMD_BUFFERS = 2;

		struct
		{
			std::thread pacer;
			std::atomic_bool pacer_exit{false};
			std::atomic_bool pacer_done{false};

			std::mutex frame_mutex;
			XrFrameState pending_frame_state{XR_TYPE_FRAME_STATE};
			bool has_pending_frame = false;
			bool initialized = false;

			bool begin_owed = false;
			XrTime begin_owed_display_time = 0;

			struct EyeChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool ever_released = false;
				bool wait_pending = false;
				uint32_t pending_index = 0;
			};
			EyeChain chains[2];
			u32 swapchain_width = 0;
			u32 swapchain_height = 0;

			struct LeverChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool ever_released = false;
				bool wait_pending = false;
				uint32_t pending_index = 0;
				u32 last_key = 0xFFFFFFFFu;
			};
			LeverChain levers[ControlQuads::kSlots];
			VkBuffer lever_staging = VK_NULL_HANDLE;
			VkDeviceMemory lever_staging_memory = VK_NULL_HANDLE;
			void* lever_staging_map = nullptr;
			VkCommandBuffer lever_cmd = VK_NULL_HANDLE;
			VkFence lever_fence = VK_NULL_HANDLE;
			bool lever_fence_submitted = false;
			bool warned_lever = false;
			int lever_layers_logged = -1;
			u32 lever_screen_logged = 0;

			XrSpace view_space = XR_NULL_HANDLE;

			float screen_anchor_x = 0.0f;
			float screen_anchor_y = 0.0f;
			float screen_anchor_z = 0.0f;
			float screen_anchor_yaw = 0.0f;

			VkCommandPool cmd_pool = VK_NULL_HANDLE;
			VkCommandBuffer cmd_buffers[NUM_CMD_BUFFERS] = {};
			VkFence fences[NUM_CMD_BUFFERS] = {};
			bool fence_submitted[NUM_CMD_BUFFERS] = {};
			u32 next_cmd_buffer = 0;

			bool warned_pacer = false;
			bool warned_beginframe = false;
			bool warned_endframe = false;
			bool warned_acquire = false;
			bool warned_wait_timeout = false;
			bool warned_wait_fail = false;
			bool warned_release = false;
			bool warned_fence_timeout = false;
			bool warned_swapchain = false;
			VkFormat swapchain_format = VK_FORMAT_R8G8B8A8_SRGB;
		} s;

		enum class CopyResult
		{
			Copied,
			Skipped,
			TimeoutZeroLayer
		};

		bool CheckXR(XrResult res, const char* what)
		{
			if (XR_SUCCEEDED(res))
				return true;

			char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
			if (XRSession::HasInstance())
				xrResultToString(XRSession::GetInstance(), res, buf);
			Console.Error("(VR) %s failed: %s (%d)", what, buf, static_cast<int>(res));
			return false;
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

		void WaitAllFences()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
			{
				if (!s.fence_submitted[i])
					continue;

				const VkResult res = vkWaitForFences(h.device, 1, &s.fences[i], VK_TRUE, ONE_SECOND_NS);
				if (res == VK_TIMEOUT)
					Console.Warning("(VR) Copy fence %u still busy after 1s during teardown.", i);
				vkResetFences(h.device, 1, &s.fences[i]);
				s.fence_submitted[i] = false;
			}
		}

		void DestroySwapchains()
		{
			WaitAllFences();
			for (auto& chain : s.chains)
			{
				if (chain.swapchain != XR_NULL_HANDLE)
				{
					xrDestroySwapchain(chain.swapchain);
					chain.swapchain = XR_NULL_HANDLE;
				}
				chain.images.clear();
				chain.ever_released = false;
				chain.wait_pending = false;
			}
			s.swapchain_width = 0;
			s.swapchain_height = 0;
		}

		bool CreateChain(u32 eye, u32 w, u32 h)
		{
			auto& chain = s.chains[eye];

			XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
			ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
			ci.format = static_cast<int64_t>(s.swapchain_format);
			ci.sampleCount = 1;
			ci.width = w;
			ci.height = h;
			ci.faceCount = 1;
			ci.arraySize = 1;
			ci.mipCount = 1;

			XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &chain.swapchain);
			if (XR_FAILED(res))
			{
				chain.swapchain = XR_NULL_HANDLE;
				if (!s.warned_swapchain)
				{
					s.warned_swapchain = true;
					Console.Error("(VR) xrCreateSwapchain failed (%d) for %ux%u; frames skipped until it succeeds.",
						static_cast<int>(res), w, h);
				}
				return false;
			}

			uint32_t count = 0;
			if (!CheckXR(xrEnumerateSwapchainImages(chain.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages") ||
				count == 0)
			{
				return false;
			}

			chain.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
			if (!CheckXR(xrEnumerateSwapchainImages(chain.swapchain, count, &count,
							 reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.images.data())),
					"xrEnumerateSwapchainImages"))
			{
				return false;
			}

			Console.WriteLn("(VR) XR swapchain created (eye %u): %ux%u, %u images (%s).",
				eye, w, h, count,
				s.swapchain_format == VK_FORMAT_R8G8B8A8_UNORM ? "VK_FORMAT_R8G8B8A8_UNORM" :
																  "VK_FORMAT_R8G8B8A8_SRGB");
			return true;
		}

		bool EnsureSwapchains(u32 w, u32 h, bool stereo)
		{
			if (s.swapchain_width != 0 && (s.swapchain_width != w || s.swapchain_height != h))
				DestroySwapchains();

			if (s.chains[0].swapchain == XR_NULL_HANDLE)
			{
				if (!CreateChain(0, w, h))
				{
					DestroySwapchains();
					return false;
				}
			}
			if (stereo && s.chains[1].swapchain == XR_NULL_HANDLE)
			{
				if (!CreateChain(1, w, h))
				{
					DestroySwapchains();
					return false;
				}
			}

			s.swapchain_width = w;
			s.swapchain_height = h;
			s.warned_swapchain = false;
			return true;
		}

		bool RecordAndSubmitCopy(GSTextureVK* src, u32 src_layer, VkImage dst)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();

			const u32 i = s.next_cmd_buffer;
			s.next_cmd_buffer = (s.next_cmd_buffer + 1) % NUM_CMD_BUFFERS;

			if (s.fence_submitted[i])
			{
				const VkResult wr = vkWaitForFences(h.device, 1, &s.fences[i], VK_TRUE, ONE_SECOND_NS);
				if (wr != VK_SUCCESS)
				{
					if (!s.warned_fence_timeout)
					{
						s.warned_fence_timeout = true;
						Console.Warning("(VR) Copy fence wait returned %d; skipping this frame's copy.",
							static_cast<int>(wr));
					}
					return false;
				}
				vkResetFences(h.device, 1, &s.fences[i]);
				s.fence_submitted[i] = false;
			}

			VkCommandBuffer cmd = s.cmd_buffers[i];
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VkResult vr = vkBeginCommandBuffer(cmd, &bi);
			if (vr != VK_SUCCESS)
			{
				Console.Error("(VR) vkBeginCommandBuffer failed (%d).", static_cast<int>(vr));
				return false;
			}

			src->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);

			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

			VkImageCopy region = {};
			region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src_layer, 1};
			region.srcOffset = {0, 0, 0};
			region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.dstOffset = {0, 0, 0};
			region.extent = {s.swapchain_width, s.swapchain_height, 1};
			vkCmdCopyImage(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

			vr = vkEndCommandBuffer(cmd);
			if (vr != VK_SUCCESS)
			{
				Console.Error("(VR) vkEndCommandBuffer failed (%d).", static_cast<int>(vr));
				return false;
			}

			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cmd;
			vr = vkQueueSubmit(h.queue, 1, &si, s.fences[i]);
			if (vr != VK_SUCCESS)
			{
				Console.Error("(VR) vkQueueSubmit failed (%d).", static_cast<int>(vr));
				return false;
			}
			s.fence_submitted[i] = true;
			return true;
		}

		CopyResult CopyToSwapchain(GSTextureVK* src, u32 src_layer, decltype(s.chains[0])& chain)
		{
			uint32_t index = 0;
			if (chain.wait_pending)
			{
				index = chain.pending_index;
			}
			else
			{
				XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				XrResult res = xrAcquireSwapchainImage(chain.swapchain, &ai, &index);
				if (XR_FAILED(res))
				{
					if (!s.warned_acquire)
					{
						s.warned_acquire = true;
						Console.Error("(VR) xrAcquireSwapchainImage failed (%d).", static_cast<int>(res));
					}
					return CopyResult::Skipped;
				}
				chain.pending_index = index;
				chain.wait_pending = true;
			}

			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = ONE_SECOND_NS;
			XrResult res = xrWaitSwapchainImage(chain.swapchain, &wi);
			if (res == XR_TIMEOUT_EXPIRED)
			{
				if (!s.warned_wait_timeout)
				{
					s.warned_wait_timeout = true;
					Console.Warning("(VR) xrWaitSwapchainImage timed out; skipping copy this frame.");
				}
				return CopyResult::TimeoutZeroLayer;
			}
			if (XR_FAILED(res))
			{
				if (!s.warned_wait_fail)
				{
					s.warned_wait_fail = true;
					Console.Error("(VR) xrWaitSwapchainImage failed (%d).", static_cast<int>(res));
				}
				return CopyResult::TimeoutZeroLayer;
			}
			chain.wait_pending = false;

			const bool submitted = RecordAndSubmitCopy(src, src_layer, chain.images[index].image);

			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			res = xrReleaseSwapchainImage(chain.swapchain, &ri);
			if (XR_FAILED(res))
			{
				if (!s.warned_release)
				{
					s.warned_release = true;
					Console.Error("(VR) xrReleaseSwapchainImage failed (%d).", static_cast<int>(res));
				}
				return CopyResult::Skipped;
			}

			return submitted ? CopyResult::Copied : CopyResult::Skipped;
		}

		void DestroyLeverChains()
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid() && s.lever_fence != VK_NULL_HANDLE && s.lever_fence_submitted)
			{
				if (vkWaitForFences(h.device, 1, &s.lever_fence, VK_TRUE, ONE_SECOND_NS) == VK_TIMEOUT)
					Console.Warning("(VR) Control quads: lever upload fence still busy after 1s during teardown.");
				vkResetFences(h.device, 1, &s.lever_fence);
				s.lever_fence_submitted = false;
			}
			for (auto& lc : s.levers)
			{
				if (lc.swapchain != XR_NULL_HANDLE)
				{
					xrDestroySwapchain(lc.swapchain);
					lc.swapchain = XR_NULL_HANDLE;
				}
				lc.images.clear();
				lc.ever_released = false;
				lc.wait_pending = false;
				lc.last_key = 0xFFFFFFFFu;
			}
		}

		void DestroyLeverResources()
		{
			DestroyLeverChains();
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid())
			{
				if (s.lever_staging_map && s.lever_staging_memory != VK_NULL_HANDLE)
					vkUnmapMemory(h.device, s.lever_staging_memory);
				if (s.lever_staging != VK_NULL_HANDLE)
					vkDestroyBuffer(h.device, s.lever_staging, nullptr);
				if (s.lever_staging_memory != VK_NULL_HANDLE)
					vkFreeMemory(h.device, s.lever_staging_memory, nullptr);
				if (s.lever_fence != VK_NULL_HANDLE)
					vkDestroyFence(h.device, s.lever_fence, nullptr);
				if (s.lever_cmd != VK_NULL_HANDLE && s.cmd_pool != VK_NULL_HANDLE)
					vkFreeCommandBuffers(h.device, s.cmd_pool, 1, &s.lever_cmd);
			}
			s.lever_staging_map = nullptr;
			s.lever_staging = VK_NULL_HANDLE;
			s.lever_staging_memory = VK_NULL_HANDLE;
			s.lever_fence = VK_NULL_HANDLE;
			s.lever_fence_submitted = false;
			s.lever_cmd = VK_NULL_HANDLE;
		}

		bool EnsureLeverResources()
		{
			if (s.lever_staging != VK_NULL_HANDLE && s.lever_cmd != VK_NULL_HANDLE && s.lever_fence != VK_NULL_HANDLE)
				return true;
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (!h.IsValid() || s.cmd_pool == VK_NULL_HANDLE)
				return false;
			const auto fail = [&](const char* what, int code) {
				if (!s.warned_lever)
				{
					s.warned_lever = true;
					Console.Error("(VR) Control quads: %s failed (%d); the lever cards will not be drawn this session.", what, code);
				}
				DestroyLeverResources();
				return false;
			};

			constexpr VkDeviceSize kBytes =
				static_cast<VkDeviceSize>(ControlQuads::kWidth) * ControlQuads::kHeight * 4u * ControlQuads::kSlots;
			VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = kBytes;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VkResult vr = vkCreateBuffer(h.device, &bci, nullptr, &s.lever_staging);
			if (vr != VK_SUCCESS)
				return fail("vkCreateBuffer", static_cast<int>(vr));

			VkMemoryRequirements mr = {};
			vkGetBufferMemoryRequirements(h.device, s.lever_staging, &mr);
			VkPhysicalDeviceMemoryProperties mp = {};
			vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
			constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			u32 type = UINT32_MAX;
			for (u32 i = 0; i < mp.memoryTypeCount; i++)
			{
				if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & kWant) == kWant)
				{
					type = i;
					break;
				}
			}
			if (type == UINT32_MAX)
				return fail("host-visible memory type lookup", 0);
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = mr.size;
			mai.memoryTypeIndex = type;
			vr = vkAllocateMemory(h.device, &mai, nullptr, &s.lever_staging_memory);
			if (vr != VK_SUCCESS)
				return fail("vkAllocateMemory", static_cast<int>(vr));
			vr = vkBindBufferMemory(h.device, s.lever_staging, s.lever_staging_memory, 0);
			if (vr != VK_SUCCESS)
				return fail("vkBindBufferMemory", static_cast<int>(vr));
			vr = vkMapMemory(h.device, s.lever_staging_memory, 0, VK_WHOLE_SIZE, 0, &s.lever_staging_map);
			if (vr != VK_SUCCESS)
				return fail("vkMapMemory", static_cast<int>(vr));

			VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			cai.commandPool = s.cmd_pool;
			cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cai.commandBufferCount = 1;
			vr = vkAllocateCommandBuffers(h.device, &cai, &s.lever_cmd);
			if (vr != VK_SUCCESS)
				return fail("vkAllocateCommandBuffers", static_cast<int>(vr));
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			vr = vkCreateFence(h.device, &fci, nullptr, &s.lever_fence);
			if (vr != VK_SUCCESS)
				return fail("vkCreateFence", static_cast<int>(vr));
			Console.WriteLn("(VR) Control quads: lever upload resources created (%u x %u x %d cards, %llu-byte staging).",
				ControlQuads::kWidth, ControlQuads::kHeight, ControlQuads::kSlots, static_cast<unsigned long long>(kBytes));
			return true;
		}

		bool EnsureLeverChain(int slot)
		{
			auto& lc = s.levers[slot];
			if (lc.swapchain != XR_NULL_HANDLE)
				return true;
			XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
			ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
			ci.format = static_cast<int64_t>(s.swapchain_format);
			ci.sampleCount = 1;
			ci.width = ControlQuads::kWidth;
			ci.height = ControlQuads::kHeight;
			ci.faceCount = 1;
			ci.arraySize = 1;
			ci.mipCount = 1;
			const XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &lc.swapchain);
			if (XR_FAILED(res))
			{
				lc.swapchain = XR_NULL_HANDLE;
				if (!s.warned_lever)
				{
					s.warned_lever = true;
					Console.Error("(VR) Control quads: xrCreateSwapchain failed (%d) for lever %d; no card.", static_cast<int>(res), slot);
				}
				return false;
			}
			uint32_t count = 0;
			if (!CheckXR(xrEnumerateSwapchainImages(lc.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages (lever)") || count == 0)
				return false;
			lc.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
			if (!CheckXR(xrEnumerateSwapchainImages(lc.swapchain, count, &count,
							 reinterpret_cast<XrSwapchainImageBaseHeader*>(lc.images.data())),
					"xrEnumerateSwapchainImages (lever)"))
			{
				return false;
			}
			Console.WriteLn("(VR) Control quads: lever %d swapchain created (%ux%u, %u images).", slot, ControlQuads::kWidth,
				ControlQuads::kHeight, count);
			return true;
		}

		void RecordLeverUpload(VkCommandBuffer cmd, int slot, VkImage dst, float t, bool grabbed, bool broke_away)
		{
			static std::vector<u32> pixels;
			ControlQuads::RasterLever(pixels, t, grabbed, broke_away, slot);
			const VkDeviceSize bytes = static_cast<VkDeviceSize>(ControlQuads::kWidth) * ControlQuads::kHeight * 4u;
			const VkDeviceSize offset = bytes * static_cast<VkDeviceSize>(slot);
			std::memcpy(static_cast<u8*>(s.lever_staging_map) + offset, pixels.data(), static_cast<size_t>(bytes));

			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy region = {};
			region.bufferOffset = offset;
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;
			region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageOffset = {0, 0, 0};
			region.imageExtent = {ControlQuads::kWidth, ControlQuads::kHeight, 1};
			vkCmdCopyBufferToImage(cmd, s.lever_staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		}

		u32 BuildLeverLayers(XrCompositionLayerQuad* lever_quads, const XrCompositionLayerBaseHeader** layers,
			u32& layer_count, u32 layer_capacity, bool split)
		{
			ControlQuads::Slot slots[ControlQuads::kSlots];
			bool any = false;
			for (int i = 0; i < ControlQuads::kSlots; i++)
			{
				slots[i] = ControlQuads::Get(i);
				any = any || slots[i].active;
			}
			if (!any)
			{
				if (s.levers[0].swapchain != XR_NULL_HANDLE || s.levers[1].swapchain != XR_NULL_HANDLE)
				{
					DestroyLeverChains();
					Console.WriteLn("(VR) Control quads: no active control — lever swapchains destroyed.");
				}
				if (s.lever_layers_logged != 0)
				{
					s.lever_layers_logged = 0;
					s.lever_screen_logged = layer_count;
				}
				return 0;
			}
			if (!EnsureLeverResources())
				return 0;

			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			bool recording = false;
			int pending_release[ControlQuads::kSlots];
			u32 pending_key[ControlQuads::kSlots];
			int pending_count = 0;
			for (int i = 0; i < ControlQuads::kSlots; i++)
			{
				if (!slots[i].active || !EnsureLeverChain(i))
					continue;
				auto& lc = s.levers[i];
				const u32 key = ControlQuads::RasterKey(slots[i].t, slots[i].grabbed, slots[i].broke_away);
				if (lc.ever_released && key == lc.last_key)
					continue;

				uint32_t index = 0;
				if (lc.wait_pending)
				{
					index = lc.pending_index;
				}
				else
				{
					XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
					if (XR_FAILED(xrAcquireSwapchainImage(lc.swapchain, &ai, &index)))
						continue;
					lc.pending_index = index;
					lc.wait_pending = true;
				}
				XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wi.timeout = ONE_SECOND_NS;
				if (XR_FAILED(xrWaitSwapchainImage(lc.swapchain, &wi)))
					continue;
				lc.wait_pending = false;

				if (!recording)
				{
					if (s.lever_fence_submitted)
					{
						if (vkWaitForFences(h.device, 1, &s.lever_fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
						{
							XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
							xrReleaseSwapchainImage(lc.swapchain, &ri);
							break;
						}
						vkResetFences(h.device, 1, &s.lever_fence);
						s.lever_fence_submitted = false;
					}
					VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
					bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
					if (vkBeginCommandBuffer(s.lever_cmd, &bi) != VK_SUCCESS)
					{
						XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
						xrReleaseSwapchainImage(lc.swapchain, &ri);
						break;
					}
					recording = true;
				}
				RecordLeverUpload(s.lever_cmd, i, lc.images[index].image, slots[i].t, slots[i].grabbed, slots[i].broke_away);
				pending_release[pending_count] = i;
				pending_key[pending_count] = key;
				pending_count++;
			}
			if (recording)
			{
				bool submitted = false;
				if (vkEndCommandBuffer(s.lever_cmd) == VK_SUCCESS)
				{
					VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
					si.commandBufferCount = 1;
					si.pCommandBuffers = &s.lever_cmd;
					submitted = (vkQueueSubmit(h.queue, 1, &si, s.lever_fence) == VK_SUCCESS);
					s.lever_fence_submitted = submitted;
				}
				for (int k = 0; k < pending_count; k++)
				{
					auto& lc = s.levers[pending_release[k]];
					XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
					if (XR_SUCCEEDED(xrReleaseSwapchainImage(lc.swapchain, &ri)) && submitted)
					{
						lc.ever_released = true;
						lc.last_key = pending_key[k];
					}
				}
			}

			u32 appended = 0;
			for (int i = 0; i < ControlQuads::kSlots; i++)
			{
				if (!slots[i].active || !s.levers[i].ever_released)
					continue;
				if (layer_count >= layer_capacity)
					break;
				SpatialControls::Anchor a;
				a.position = {s.screen_anchor_x, s.screen_anchor_y, s.screen_anchor_z};
				a.yaw = s.screen_anchor_yaw;
				SpatialControls::Placement pl;
				pl.side = slots[i].side;
				pl.height = slots[i].height;
				pl.forward = slots[i].forward;
				pl.yaw_deg = slots[i].yaw_deg;
				const SpatialControls::Frame f = SpatialControls::PlaceControl(a, pl);
				const float pitch = std::atan2(-slots[i].height, std::max(slots[i].forward, 0.05f));
				const SpatialControls::Quat pitch_q = {std::sin(-pitch * 0.5f), 0.0f, 0.0f, std::cos(-pitch * 0.5f)};
				const SpatialControls::Quat q = f.orientation * pitch_q;

				XrCompositionLayerQuad& quad = lever_quads[i];
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.next = nullptr;
				quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
				quad.space = XRSession::GetSpace();
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage = {s.levers[i].swapchain,
					{{0, 0}, {static_cast<s32>(ControlQuads::kWidth), static_cast<s32>(ControlQuads::kHeight)}}, 0};
				quad.pose.orientation = {q.x, q.y, q.z, q.w};
				quad.pose.position = {f.pivot.x, f.pivot.y, f.pivot.z};
				quad.size = {ControlQuads::kQuadWidthM, ControlQuads::kQuadHeightM};
				layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				appended++;
			}

			const u32 screen_layers = layer_count - appended;
			if (static_cast<int>(appended) != s.lever_layers_logged || screen_layers != s.lever_screen_logged)
			{
				s.lever_layers_logged = static_cast<int>(appended);
				s.lever_screen_logged = screen_layers;
				Console.WriteLn("(VR) Control quads: %u lever layer(s) after %u screen layer(s) [%s branch, capacity %u]%s.",
					appended, screen_layers, split ? "split-screen" : "mirror", layer_capacity,
					(appended == 0 && any) ? " — no card released yet or no slot left" : "");
			}
			return appended;
		}

		float ComputeAspect()
		{
			switch (GSConfig.AspectRatio)
			{
				case AspectRatioType::R4_3:
				case AspectRatioType::RAuto4_3_3_2:
					return 4.0f / 3.0f;
				case AspectRatioType::R16_9:
					return 16.0f / 9.0f;
				default:
					return (s.swapchain_height > 0)
							   ? static_cast<float>(s.swapchain_width) / static_cast<float>(s.swapchain_height)
							   : 4.0f / 3.0f;
			}
		}

		void PacerThreadMain()
		{
			Threading::SetNameOfCurrentThread("VR Pacer");

			const XrSession session = XRSession::GetSession();

			while (!s.pacer_exit.load(std::memory_order_acquire))
			{
				if (!XRSession::IsSessionRunning())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					continue;
				}

				XrFrameState fs = {XR_TYPE_FRAME_STATE};
				const XrResult res = xrWaitFrame(session, nullptr, &fs);
				if (XR_FAILED(res))
				{
					if (res == XR_ERROR_SESSION_NOT_RUNNING)
						continue;

					if (!s.warned_pacer)
					{
						s.warned_pacer = true;
						Console.Error("(VR) xrWaitFrame failed (%d); pacer parked. Session-loss handling "
									  "arrives via events.",
							static_cast<int>(res));
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}

				std::lock_guard<std::mutex> lock(s.frame_mutex);
				s.pending_frame_state = fs;
				s.has_pending_frame = true;
			}

			s.pacer_done.store(true, std::memory_order_release);
		}

		bool SettleOwedBegin(XrSession session)
		{
			if (!s.begin_owed)
				return true;

			XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
			const XrResult res = xrBeginFrame(session, &bi);
			if (res == XR_ERROR_SESSION_NOT_RUNNING)
			{
				s.begin_owed = false;
				return true;
			}
			if (XR_FAILED(res))
				return false;

			XRInput::Update(s.begin_owed_display_time);

			XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
			ei.displayTime = s.begin_owed_display_time;
			ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			ei.layerCount = 0;
			ei.layers = nullptr;
			xrEndFrame(session, &ei);
			s.begin_owed = false;
			return true;
		}

		void DrainOnePacedFrameForShutdown()
		{
			XrFrameState fs;
			{
				std::lock_guard<std::mutex> lock(s.frame_mutex);
				if (!s.has_pending_frame)
					return;
				fs = s.pending_frame_state;
				s.has_pending_frame = false;
			}

			if (!XRSession::HasSession())
				return;

			const XrSession session = XRSession::GetSession();
			XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
			if (XR_FAILED(xrBeginFrame(session, &bi)))
				return;

			XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
			ei.displayTime = fs.predictedDisplayTime;
			ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			ei.layerCount = 0;
			ei.layers = nullptr;
			xrEndFrame(session, &ei);
		}
	}

	bool Initialize()
	{
		if (s.initialized)
			return true;

		s.screen_anchor_x = 0.0f;
		s.screen_anchor_y = 0.0f;
		s.screen_anchor_z = 0.0f;
		s.screen_anchor_yaw = 0.0f;
		s_reanchor_requested.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(s_screen_mutex);
			s_anchor_snapshot.x = s_anchor_snapshot.y = s_anchor_snapshot.z = s_anchor_snapshot.yaw = 0.0f;
			s_anchor_snapshot.generation++;
		}
		s.warned_lever = false;
		s.lever_layers_logged = -1;
		s.lever_screen_logged = 0;

		if (!XRSession::HasSession())
		{
			Console.Error("(VR) Compositor Initialize: no XR session.");
			return false;
		}

		const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
		if (!h.IsValid())
		{
			Console.Error("(VR) Compositor Initialize: Vulkan handles are not valid.");
			return false;
		}

		const XrSession session = XRSession::GetSession();

		uint32_t fmt_count = 0;
		if (!CheckXR(xrEnumerateSwapchainFormats(session, 0, &fmt_count, nullptr), "xrEnumerateSwapchainFormats") ||
			fmt_count == 0)
		{
			return false;
		}
		std::vector<int64_t> formats(fmt_count);
		if (!CheckXR(xrEnumerateSwapchainFormats(session, fmt_count, &fmt_count, formats.data()),
				"xrEnumerateSwapchainFormats"))
		{
			return false;
		}

		static const bool s_unorm_experiment = (std::getenv("PCSX2_VR_SWAPCHAIN_UNORM") != nullptr);
		bool has_unorm = false, has_srgb = false;
		for (int64_t f : formats)
		{
			if (f == static_cast<int64_t>(VK_FORMAT_R8G8B8A8_UNORM))
				has_unorm = true;
			else if (f == static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB))
				has_srgb = true;
		}
		if (s_unorm_experiment && has_unorm)
		{
			s.swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
			Console.Warning("(VR) PCSX2_VR_SWAPCHAIN_UNORM experiment ACTIVE: UNORM swapchain — the runtime "
							"treats the GS's gamma-encoded bytes as linear. Expect a brightness shift; this is "
							"an A/B lane, not a shipping mode.");
		}
		else if (has_srgb)
		{
			s.swapchain_format = VK_FORMAT_R8G8B8A8_SRGB;
			if (s_unorm_experiment)
				Console.Warning("(VR) PCSX2_VR_SWAPCHAIN_UNORM requested, but the runtime offers no "
								"VK_FORMAT_R8G8B8A8_UNORM swapchain — using the sRGB default.");
		}
		else if (has_unorm)
		{
			s.swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
			Console.Warning("(VR) Runtime offers no VK_FORMAT_R8G8B8A8_SRGB swapchain; using UNORM. "
							"Brightness/gamma may be off on this runtime.");
		}
		else
		{
			std::string offered;
			for (int64_t f : formats)
			{
				offered += std::to_string(f);
				offered += ' ';
			}
			Console.Error("(VR) Runtime offers neither VK_FORMAT_R8G8B8A8_SRGB (%d) nor _UNORM (%d). "
						  "Offered VkFormats: %s. Running flat.",
				static_cast<int>(VK_FORMAT_R8G8B8A8_SRGB), static_cast<int>(VK_FORMAT_R8G8B8A8_UNORM),
				offered.c_str());
			return false;
		}

		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = h.queue_family;
		if (vkCreateCommandPool(h.device, &pci, nullptr, &s.cmd_pool) != VK_SUCCESS)
		{
			Console.Error("(VR) vkCreateCommandPool failed.");
			s.cmd_pool = VK_NULL_HANDLE;
			return false;
		}

		VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cbai.commandPool = s.cmd_pool;
		cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = NUM_CMD_BUFFERS;
		if (vkAllocateCommandBuffers(h.device, &cbai, s.cmd_buffers) != VK_SUCCESS)
		{
			Console.Error("(VR) vkAllocateCommandBuffers failed.");
			vkDestroyCommandPool(h.device, s.cmd_pool, nullptr);
			s.cmd_pool = VK_NULL_HANDLE;
			return false;
		}

		for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
		{
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			if (vkCreateFence(h.device, &fci, nullptr, &s.fences[i]) != VK_SUCCESS)
			{
				Console.Error("(VR) vkCreateFence failed.");
				for (u32 j = 0; j < i; j++)
				{
					vkDestroyFence(h.device, s.fences[j], nullptr);
					s.fences[j] = VK_NULL_HANDLE;
				}
				vkFreeCommandBuffers(h.device, s.cmd_pool, NUM_CMD_BUFFERS, s.cmd_buffers);
				vkDestroyCommandPool(h.device, s.cmd_pool, nullptr);
				s.cmd_pool = VK_NULL_HANDLE;
				return false;
			}
			s.fence_submitted[i] = false;
		}

		for (auto& chain : s.chains)
		{
			chain.swapchain = XR_NULL_HANDLE;
			chain.images.clear();
			chain.ever_released = false;
		}
		s.swapchain_width = 0;
		s.swapchain_height = 0;
		s.next_cmd_buffer = 0;
		s.begin_owed = false;
		s.begin_owed_display_time = 0;
		s_xrfault_begin1_fired = false;

		s.warned_pacer = false;
		s.warned_beginframe = false;
		s.warned_endframe = false;
		s.warned_acquire = false;
		s.warned_wait_timeout = false;
		s.warned_wait_fail = false;
		s.warned_release = false;
		s.warned_fence_timeout = false;
		s.warned_swapchain = false;

		s.pacer_exit.store(false, std::memory_order_release);
		s.pacer_done.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lock(s.frame_mutex);
			s.has_pending_frame = false;
		}
		{
			XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
			rsci.poseInReferenceSpace.orientation.w = 1.0f;
			if (XR_FAILED(xrCreateReferenceSpace(session, &rsci, &s.view_space)))
			{
				s.view_space = XR_NULL_HANDLE;
				Console.Warning("(VR) VIEW reference space creation failed; head-tracked camera will stay inert.");
			}
		}

		s.initialized = true;
		s.pacer = std::thread(PacerThreadMain);

		Console.WriteLn("(VR) Compositor initialized (%u copy command buffers; swapchain created on first frame).",
			NUM_CMD_BUFFERS);
		return true;
	}

	void EndOfFrame(GSTexture* current, u32 eye)
	{
		if (!s.initialized)
			return;

		if (s.begin_owed)
		{
			if (XRSession::IsSessionRunning())
				SettleOwedBegin(XRSession::GetSession());
			else
				s.begin_owed = false;
			return;
		}

		XrFrameState fs = {XR_TYPE_FRAME_STATE};
		{
			std::lock_guard<std::mutex> lock(s.frame_mutex);
			if (!s.has_pending_frame)
				return;
			fs = s.pending_frame_state;
			s.has_pending_frame = false;
		}

		if (!XRSession::IsSessionRunning())
			return;

		const XrSession session = XRSession::GetSession();

		XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
		static const bool s_fault_begin1 = [] {
			const char* v = std::getenv("PCSX2_VR_XRFAULT");
			return v && std::strcmp(v, "begin1") == 0;
		}();
		XrResult res;
		if (s_fault_begin1 && !s_xrfault_begin1_fired)
		{
			s_xrfault_begin1_fired = true;
			res = XR_ERROR_RUNTIME_FAILURE;
			Console.Warning("(VR) XRFAULT=begin1: injecting one xrBeginFrame failure to exercise owed-begin recovery.");
		}
		else
		{
			res = xrBeginFrame(session, &bi);
		}
		if (XR_FAILED(res))
		{
			if (res == XR_ERROR_SESSION_NOT_RUNNING)
				return;
			if (!s.warned_beginframe)
			{
				s.warned_beginframe = true;
				Console.Error("(VR) xrBeginFrame failed (%d); will retry the begin/end pairing.", static_cast<int>(res));
			}
			s.begin_owed = true;
			s.begin_owed_display_time = fs.predictedDisplayTime;
			return;
		}

		XRInput::Update(fs.predictedDisplayTime);

		if (s.view_space != XR_NULL_HANDLE)
		{
			XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(xrLocateSpace(s.view_space, XRSession::GetSpace(), fs.predictedDisplayTime, &loc)) &&
				(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			{
				HeadPose::Snapshot p;
				p.orientation_x = loc.pose.orientation.x;
				p.orientation_y = loc.pose.orientation.y;
				p.orientation_z = loc.pose.orientation.z;
				p.orientation_w = loc.pose.orientation.w;
				if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				{
					p.position_x = loc.pose.position.x;
					p.position_y = loc.pose.position.y;
					p.position_z = loc.pose.position.z;
				}
				p.valid = true;
				HeadPose::Publish(p);
			}
			else
			{
				HeadPose::Invalidate();
			}
		}

		if (s_reanchor_requested.load(std::memory_order_acquire) && s.view_space != XR_NULL_HANDLE)
		{
			XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(xrLocateSpace(s.view_space, XRSession::GetSpace(), fs.predictedDisplayTime, &loc)) &&
				(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			{
				const float qx = loc.pose.orientation.x, qy = loc.pose.orientation.y;
				const float qz = loc.pose.orientation.z, qw = loc.pose.orientation.w;
				const float fx = -2.0f * (qw * qy + qz * qx);
				const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
				if ((fx * fx + fz * fz) > 1.0e-4f)
					s.screen_anchor_yaw = std::atan2(-fx, -fz);
				if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				{
					s.screen_anchor_x = loc.pose.position.x;
					s.screen_anchor_y = loc.pose.position.y;
					s.screen_anchor_z = loc.pose.position.z;
				}
				s_reanchor_requested.store(false, std::memory_order_release);
				Console.WriteLn("(VR) Screen re-anchored: pos (%.2f, %.2f, %.2f) yaw %.1f deg.",
					s.screen_anchor_x, s.screen_anchor_y, s.screen_anchor_z,
					s.screen_anchor_yaw * (180.0f / 3.14159265f));
				{
					std::lock_guard<std::mutex> lock(s_screen_mutex);
					s_anchor_snapshot.x = s.screen_anchor_x;
					s_anchor_snapshot.y = s.screen_anchor_y;
					s_anchor_snapshot.z = s.screen_anchor_z;
					s_anchor_snapshot.yaw = s.screen_anchor_yaw;
					s_anchor_snapshot.generation++;
				}
			}
		}

		const bool layered = current && (static_cast<GSTextureVK*>(current)->GetArrayLayers() >= 2);
		const bool stereo = layered || (eye != MonoEye);
		bool force_zero_layers = false;
		static const bool s_chainlog = (std::getenv("PCSX2_VR_CHAINLOG") != nullptr);
		static u64 s_chainlog_visit = 0;
		int cl_copy[2] = {-1, -1};
		if (fs.shouldRender && current)
		{
			GSTextureVK* src = static_cast<GSTextureVK*>(current);
			const u32 w = static_cast<u32>(src->GetWidth());
			const u32 h = static_cast<u32>(src->GetHeight());
			if (w > 0 && h > 0 && EnsureSwapchains(w, h, stereo))
			{
				if (layered)
				{
					for (u32 l = 0; l < 2; l++)
					{
						const CopyResult r = CopyToSwapchain(src, l, s.chains[l]);
						cl_copy[l] = static_cast<int>(r);
						if (r == CopyResult::Copied)
							s.chains[l].ever_released = true;
						else if (r == CopyResult::TimeoutZeroLayer && !s.chains[l].ever_released)
							force_zero_layers = true;
					}
				}
				else
				{
					const u32 cl_ci = (eye != MonoEye) ? eye : 0;
					auto& chain = s.chains[cl_ci];
					const CopyResult r = CopyToSwapchain(src, 0, chain);
					cl_copy[cl_ci] = static_cast<int>(r);
					if (r == CopyResult::Copied)
						chain.ever_released = true;
					else if (r == CopyResult::TimeoutZeroLayer && !chain.ever_released)
						force_zero_layers = true;
				}
			}
		}

		XrCompositionLayerQuad quads[5] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD},
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		XrCompositionLayerCylinderKHR cyls[5] = {
			{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}, {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR},
			{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}, {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR},
			{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}};
		constexpr u32 kLayerCapacity = 5 + ControlQuads::kSlots;
		XrCompositionLayerQuad lever_quads[ControlQuads::kSlots] = {
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		const XrCompositionLayerBaseHeader* layers[kLayerCapacity] = {};
		u32 layer_count = 0;
		bool cl_split = false;
		if (!force_zero_layers)
		{
			ScreenParams sp;
			{
				std::lock_guard<std::mutex> lock(s_screen_mutex);
				sp = s_screen_params;
			}
			const float distance = sp.distance;
			const float height = sp.height;
			const float arc_deg = sp.arc_deg;
			const float voffset = sp.voffset;
			const float aspect = ComputeAspect();
			const bool curved = (arc_deg >= 5.0f) && XRSession::HasCylinderLayer();

			const float ayaw_sin = std::sin(s.screen_anchor_yaw);
			const float ayaw_cos = std::cos(s.screen_anchor_yaw);
			const XrQuaternionf anchor_quat = {
				0.0f, std::sin(s.screen_anchor_yaw * 0.5f), 0.0f, std::cos(s.screen_anchor_yaw * 0.5f)};

			const bool follow = sp.follow_head && (s.view_space != XR_NULL_HANDLE);
			const XrSpace layer_space = follow ? s.view_space : XRSession::GetSpace();
			const XrQuaternionf follow_quat = {0.0f, 0.0f, 0.0f, 1.0f};

			const auto make_layer = [&](u32 chain_idx, XrEyeVisibility vis) {
				const XrSwapchainSubImage sub_image = {
					s.chains[chain_idx].swapchain,
					{{0, 0}, {static_cast<s32>(s.swapchain_width), static_cast<s32>(s.swapchain_height)}},
					0};
				if (curved)
				{
					XrCompositionLayerCylinderKHR& cyl = cyls[layer_count];
					cyl.layerFlags = 0;
					cyl.space = layer_space;
					cyl.eyeVisibility = vis;
					cyl.subImage = sub_image;
					if (follow)
					{
						cyl.pose.orientation = follow_quat;
						cyl.pose.position = {0.0f, voffset, 0.0f};
					}
					else
					{
						cyl.pose.orientation = anchor_quat;
						cyl.pose.position = {s.screen_anchor_x, s.screen_anchor_y + voffset, s.screen_anchor_z};
					}
					cyl.radius = distance;
					cyl.centralAngle = arc_deg * (3.14159265f / 180.0f);
					cyl.aspectRatio = aspect;
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cyl);
				}
				else
				{
					XrCompositionLayerQuad& quad = quads[layer_count];
					quad.layerFlags = 0;
					quad.space = layer_space;
					quad.eyeVisibility = vis;
					quad.subImage = sub_image;
					if (follow)
					{
						quad.pose.orientation = follow_quat;
						quad.pose.position = {0.0f, voffset, -distance};
					}
					else
					{
						quad.pose.orientation = anchor_quat;
						quad.pose.position = {s.screen_anchor_x - distance * ayaw_sin,
							s.screen_anchor_y + voffset,
							s.screen_anchor_z - distance * ayaw_cos};
					}
					quad.size = {height * aspect, height};
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				}
				layer_count++;
			};

			const SplitState::Snapshot split_snap = SplitState::Get();
			cl_split = split_snap.split_active;
			const auto make_split_layer = [&](u32 chain_idx, XrEyeVisibility vis, int viewport) {
				const s32 full_w = static_cast<s32>(s.swapchain_width);
				const s32 full_h = static_cast<s32>(s.swapchain_height);
				s32 rx = static_cast<s32>(split_snap.rect_x[viewport] * full_w);
				s32 ry = static_cast<s32>(split_snap.rect_y[viewport] * full_h);
				s32 rw = static_cast<s32>(split_snap.rect_w[viewport] * full_w);
				s32 rh = static_cast<s32>(split_snap.rect_h[viewport] * full_h);
				rx += 1; ry += 1; rw -= 2; rh -= 2;
				if (rw < 2 || rh < 2)
					return;
				const XrSwapchainSubImage sub_image = {
					s.chains[chain_idx].swapchain, {{rx, ry}, {rw, rh}}, 0};
				const float rect_aspect = static_cast<float>(rw) / static_cast<float>(rh);
				const bool is_local = (viewport == split_snap.local_view);
				const bool focus = (split_snap.mode == 0);
				const float shape_scale = (is_local || !focus) ? 1.0f : split_snap.side_scale;
				const float mirror_width = height * aspect;
				const float local_half_deg = curved ? (0.5f * arc_deg) :
					(std::atan((0.5f * mirror_width) / distance) * (180.0f / 3.14159265f));
				const float side_half_deg = curved ? (0.5f * arc_deg * shape_scale) :
					(std::atan((0.5f * mirror_width * shape_scale) / distance) * (180.0f / 3.14159265f));
				float eff_side_deg = split_snap.side_angle_deg;
				if (!is_local)
				{
					const float min_sep = local_half_deg + side_half_deg + 2.0f;
					if (std::fabs(eff_side_deg) < min_sep)
						eff_side_deg = (eff_side_deg < 0.0f) ? -min_sep : min_sep;
				}
				const float extra_yaw = is_local ? 0.0f :
					eff_side_deg * (3.14159265f / 180.0f);
				const float yaw = (follow ? 0.0f : s.screen_anchor_yaw) + extra_yaw;
				const XrQuaternionf vp_quat = {
					0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
				const float ysin = std::sin(yaw);
				const float ycos = std::cos(yaw);
				const float vp_width = mirror_width * shape_scale;
				const float vp_phys_height = vp_width / rect_aspect;
				if (curved)
				{
					XrCompositionLayerCylinderKHR& cyl = cyls[layer_count];
					cyl.layerFlags = 0;
					cyl.space = layer_space;
					cyl.eyeVisibility = vis;
					cyl.subImage = sub_image;
					cyl.pose.orientation = vp_quat;
					cyl.pose.position = follow ?
						XrVector3f{0.0f, voffset, 0.0f} :
						XrVector3f{s.screen_anchor_x, s.screen_anchor_y + voffset, s.screen_anchor_z};
					cyl.radius = distance;
					cyl.centralAngle = arc_deg * (3.14159265f / 180.0f) * shape_scale;
					cyl.aspectRatio = rect_aspect;
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cyl);
				}
				else
				{
					XrCompositionLayerQuad& quad = quads[layer_count];
					quad.layerFlags = 0;
					quad.space = layer_space;
					quad.eyeVisibility = vis;
					quad.subImage = sub_image;
					quad.pose.orientation = vp_quat;
					quad.pose.position = follow ?
						XrVector3f{-distance * ysin, voffset, -distance * ycos} :
						XrVector3f{s.screen_anchor_x - distance * ysin,
							s.screen_anchor_y + voffset,
							s.screen_anchor_z - distance * ycos};
					quad.size = {vp_width, vp_phys_height};
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				}
				layer_count++;
			};

			const bool l0 = s.chains[0].ever_released;
			const bool l1 = s.chains[1].ever_released;
			if (split_snap.split_active && (l0 || l1))
			{
				if (VR::SeatCastArmed() && current)
				{
					const int ovp = 1 - split_snap.local_view;
					GSTexture* src = current;
					const s32 full_w = static_cast<s32>(src->GetWidth());
					const s32 full_h = static_cast<s32>(src->GetHeight());
					GSVector4i crop(
						static_cast<int>(split_snap.rect_x[ovp] * full_w),
						static_cast<int>(split_snap.rect_y[ovp] * full_h),
						static_cast<int>((split_snap.rect_x[ovp] + split_snap.rect_w[ovp]) * full_w),
						static_cast<int>((split_snap.rect_y[ovp] + split_snap.rect_h[ovp]) * full_h));
					u32 out_w = static_cast<u32>(crop.width());
					u32 out_h = static_cast<u32>(crop.height());
					static GSTexture* s_cast_rt = nullptr;
					if (out_w > SeatCast::kMaxWidth)
					{
						const float scale = static_cast<float>(SeatCast::kMaxWidth) / out_w;
						out_w = SeatCast::kMaxWidth;
						out_h = static_cast<u32>(out_h * scale);
					}
					if (!s_cast_rt || s_cast_rt->GetWidth() != static_cast<int>(out_w) ||
						s_cast_rt->GetHeight() != static_cast<int>(out_h))
					{
						if (s_cast_rt)
							g_gs_device->Recycle(s_cast_rt);
						s_cast_rt = g_gs_device->CreateRenderTarget(out_w, out_h, GSTexture::Format::Color, false);
					}
					static std::unique_ptr<GSDownloadTexture> s_cast_dl;
					if (s_cast_rt)
					{
						const GSVector4 src_uv(
							static_cast<float>(crop.x) / full_w, static_cast<float>(crop.y) / full_h,
							static_cast<float>(crop.z) / full_w, static_cast<float>(crop.w) / full_h);
						g_gs_device->StretchRect(src, src_uv, s_cast_rt,
							GSVector4(0.0f, 0.0f, static_cast<float>(out_w), static_cast<float>(out_h)),
							ShaderConvertSelector(ShaderConvert::COPY), Filter::Biln);
						if (!s_cast_dl || s_cast_dl->GetWidth() < out_w || s_cast_dl->GetHeight() < out_h)
							s_cast_dl = g_gs_device->CreateDownloadTexture(out_w, out_h, GSTexture::Format::Color);
						const GSVector4i rc(0, 0, out_w, out_h);
						if (s_cast_dl)
						{
							s_cast_dl->CopyFromTexture(rc, s_cast_rt, rc, 0);
							s_cast_dl->Flush();
							if (s_cast_dl->Map(rc))
							{
								SeatCast::Publish(s_cast_dl->GetMapPointer(), out_w, out_h, s_cast_dl->GetMapPitch());
								s_cast_dl->Unmap();
							}
						}
					}
				}
				const bool split_stereo = stereo && split_snap.stereo_on && l0 && l1;
				for (int vp = 0; vp < 2; vp++)
				{
					if (split_snap.mode == 2 && vp != split_snap.local_view)
						continue;
					if (split_stereo)
					{
						make_split_layer(0, XR_EYE_VISIBILITY_LEFT, vp);
						make_split_layer(1, XR_EYE_VISIBILITY_RIGHT, vp);
					}
					else
					{
						make_split_layer(l0 ? 0 : 1, XR_EYE_VISIBILITY_BOTH, vp);
					}
				}
			}
			else if (stereo && l0 && l1)
			{
				make_layer(0, XR_EYE_VISIBILITY_LEFT);
				make_layer(1, XR_EYE_VISIBILITY_RIGHT);
			}
			else if (l0 || l1)
			{
				make_layer(l0 ? 0 : 1, XR_EYE_VISIBILITY_BOTH);
			}
		}

		if (s_chainlog)
		{
			const char* branch = (layer_count == 0) ? "Z" :
				cl_split ? "SP" :
				(layer_count == 2) ? "S" :
				(s.chains[0].ever_released ? "B0" : "B1");
			GSTextureVK* cur = static_cast<GSTextureVK*>(current);
			Console.WriteLn("(VR) CHAINLOG v=%llu render=%d cur=%s%ux%u/%uL layered=%d stereo=%d "
							"c0={r=%d er=%d wp=%d} c1={r=%d er=%d wp=%d} branch=%s layers=%u",
				static_cast<unsigned long long>(s_chainlog_visit++), fs.shouldRender ? 1 : 0,
				current ? "" : "NULL:", cur ? static_cast<u32>(cur->GetWidth()) : 0,
				cur ? static_cast<u32>(cur->GetHeight()) : 0, cur ? cur->GetArrayLayers() : 0,
				layered ? 1 : 0, stereo ? 1 : 0,
				cl_copy[0], s.chains[0].ever_released ? 1 : 0, s.chains[0].wait_pending ? 1 : 0,
				cl_copy[1], s.chains[1].ever_released ? 1 : 0, s.chains[1].wait_pending ? 1 : 0,
				branch, layer_count);
		}

		if (!force_zero_layers)
			BuildLeverLayers(lever_quads, layers, layer_count, kLayerCapacity, cl_split);

		XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
		ei.displayTime = fs.predictedDisplayTime;
		ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		ei.layerCount = layer_count;
		ei.layers = layer_count ? layers : nullptr;

		res = xrEndFrame(session, &ei);
		if (XR_FAILED(res) && !s.warned_endframe)
		{
			s.warned_endframe = true;
			Console.Error("(VR) xrEndFrame failed (%d).", static_cast<int>(res));
		}
	}

	void Shutdown()
	{
		if (!s.initialized)
			return;

		s.pacer_exit.store(true, std::memory_order_release);

		if (XRSession::HasSession())
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (!s.pacer_done.load(std::memory_order_acquire))
			{
				SettleOwedBegin(XRSession::GetSession());
				DrainOnePacedFrameForShutdown();
				if (s.pacer_done.load(std::memory_order_acquire))
					break;
				if (std::chrono::steady_clock::now() >= deadline)
				{
					Console.Warning("(VR) Pacer did not exit within 2s during shutdown; joining anyway.");
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}

		if (s.pacer.joinable())
			s.pacer.join();

		HeadPose::Invalidate();
		if (s.view_space != XR_NULL_HANDLE)
		{
			xrDestroySpace(s.view_space);
			s.view_space = XR_NULL_HANDLE;
		}

		const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
		const bool vk_alive = h.IsValid();

		if (vk_alive)
			WaitAllFences();

		DestroyLeverResources();

		for (auto& chain : s.chains)
		{
			if (chain.swapchain != XR_NULL_HANDLE)
			{
				xrDestroySwapchain(chain.swapchain);
				chain.swapchain = XR_NULL_HANDLE;
			}
			chain.images.clear();
			chain.ever_released = false;
		}
		s.swapchain_width = 0;
		s.swapchain_height = 0;

		if (vk_alive)
		{
			for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
			{
				if (s.fences[i] != VK_NULL_HANDLE)
				{
					vkDestroyFence(h.device, s.fences[i], nullptr);
					s.fences[i] = VK_NULL_HANDLE;
				}
			}
			if (s.cmd_pool != VK_NULL_HANDLE)
			{
				vkFreeCommandBuffers(h.device, s.cmd_pool, NUM_CMD_BUFFERS, s.cmd_buffers);
				vkDestroyCommandPool(h.device, s.cmd_pool, nullptr);
			}
		}
		else
		{
			for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
				s.fences[i] = VK_NULL_HANDLE;
		}
		s.cmd_pool = VK_NULL_HANDLE;
		for (u32 i = 0; i < NUM_CMD_BUFFERS; i++)
		{
			s.cmd_buffers[i] = VK_NULL_HANDLE;
			s.fence_submitted[i] = false;
		}
		s.next_cmd_buffer = 0;

		s.initialized = false;
		Console.WriteLn("(VR) Compositor shut down.");
	}
#else
	bool Initialize()
	{
		Console.Error("(VR) Compositor requires the Vulkan renderer; running flat.");
		return false;
	}

	void Shutdown() {}

	void EndOfFrame(GSTexture* , u32 ) {}
#endif

	void UpdateScreenParams(float distance_m, float height_m, float arc_deg, float vertical_offset_m,
		bool follow_head)
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		s_screen_params = {distance_m, height_m, arc_deg, vertical_offset_m, follow_head};
	}

	void RequestScreenReanchor()
	{
		s_reanchor_requested.store(true, std::memory_order_release);
	}

	bool IsReanchorRequested()
	{
		return s_reanchor_requested.load(std::memory_order_acquire);
	}

	void ClearReanchorRequestForTest()
	{
		s_reanchor_requested.store(false, std::memory_order_release);
	}

	ScreenAnchor GetScreenAnchor()
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		return s_anchor_snapshot;
	}
}
