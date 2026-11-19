// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/XRCompositor.h"
#include "VR/WorldProjection.h"
#include "VR/SplitState.h"
#include "VR/ControlQuads.h"
#include "VR/NearField.h"
#include "VR/VrUiPanel.h"
#include "VR/SpatialControls.h"
#include "VR/VRManager.h"
#include "VR/SeatCast.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "VR/SeatSession.h"
#include "VR/HeadPose.h"
#include "VR/CameraDriver.h"
#include "VR/DisplayFit.h"
#include "VR/LagMatch.h"
#include "VR/ScreenAspect.h"
#include "VR/VRInternal.h"
#include "VR/VRInput.h"
#include "VR/XRSession.h"
#include "VR/XRSwapchainFormat.h"
#include "VR/ScreenProjector.h"
#include "VR/SwapchainWait.h"
#include "VR/XRFrameHealth.h"
#include "VR/UiPanel.h"
#include "VMManager.h"
#include "Host.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "Config.h"
#include "VR/PadXRActions.h"

#ifdef ENABLE_VULKAN
#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/VKBuilders.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"

static_assert(VR::XRSwapchainFormat::kR8G8B8A8_UNORM == static_cast<int64_t>(VK_FORMAT_R8G8B8A8_UNORM));
static_assert(VR::XRSwapchainFormat::kR8G8B8A8_SRGB == static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB));
static_assert(VR::XRSwapchainFormat::kB8G8R8A8_UNORM == static_cast<int64_t>(VK_FORMAT_B8G8R8A8_UNORM));
static_assert(VR::XRSwapchainFormat::kB8G8R8A8_SRGB == static_cast<int64_t>(VK_FORMAT_B8G8R8A8_SRGB));
static_assert(VR::XRSwapchainFormat::kA8B8G8R8_UNORM_PACK32 == static_cast<int64_t>(VK_FORMAT_A8B8G8R8_UNORM_PACK32));
static_assert(VR::XRSwapchainFormat::kA8B8G8R8_SRGB_PACK32 == static_cast<int64_t>(VK_FORMAT_A8B8G8R8_SRGB_PACK32));
#endif

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Image.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "fmt/format.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
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
		std::atomic<float> s_presented_hfov_deg{std::numeric_limits<float>::quiet_NaN()};

		struct ScreenParams
		{
			float distance = 2.0f;
			float height = 1.4f;
			float arc_deg = 0.0f;
			float voffset = 0.0f;
			bool follow_head = false;
			ScreenCurve::Fallback curve_fallback = ScreenCurve::Fallback::Flat;
			ScreenCurve::Force force_fallback = ScreenCurve::Force::None;
			float projector_supersample = 1.5f;
			bool projector_depth = false;
			float fit_fill = DisplayFit::kDefaultFill;
			bool follow_patch_aspect = false;
			s64 lag_ns = 0;
			u8 lag_axes = 0;
		};
		std::mutex s_screen_mutex;
		ScreenParams s_screen_params;

		std::atomic<bool> s_reanchor_requested{false};
		std::atomic<bool> s_ui_recentre_requested{false};
		static_assert(kUiPanelWidth == UiPanel::kTextureWidth && kUiPanelHeight == UiPanel::kTextureHeight,
			"the panel target the present draws is the panel texture");

		ScreenAnchor s_anchor_snapshot;

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

			std::atomic_bool pacer_lost{false};
			std::atomic_bool frame_lost{false};
			XRFrameHealth::Streak frame_streak;
			std::atomic<u64> paced_frames_consumed{0};

			std::atomic<u64> pacer_generation{0};

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
				bool waited = false;
				SwapchainWait::FailureTracker wait_failures;
			};
			EyeChain chains[2];
			u32 swapchain_width = 0;
			u32 swapchain_height = 0;
			u32 source_width = 0;
			u32 source_height = 0;
			GSTexture* clamp_rt = nullptr;
			bool warned_clamp = false;
			u32 clamp_warned_width = 0;
			u32 clamp_warned_height = 0;
			u32 clamp_failed_width = 0;
			u32 clamp_failed_height = 0;

			struct LeverChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool ever_released = false;
				bool wait_pending = false;
				SwapchainWait::FailureTracker wait_failures;
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

			struct NearFieldChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool wait_pending = false;
				SwapchainWait::FailureTracker wait_failures;
				uint32_t pending_index = 0;
				std::vector<VkImageView> views;
				std::vector<VkFramebuffer> fbs;
				VkImage depth = VK_NULL_HANDLE;
				VkDeviceMemory depth_memory = VK_NULL_HANDLE;
				VkImageView depth_view = VK_NULL_HANDLE;
			};
			NearFieldChain nf[2];
			bool nf_gpu_failed = false;
			bool nf_gpu_logged = false;
			VkRenderPass nf_rp = VK_NULL_HANDLE;
			VkPipelineLayout nf_pl = VK_NULL_HANDLE;
			VkPipeline nf_pipe = VK_NULL_HANDLE;
			VkFormat nf_depth_format = VK_FORMAT_UNDEFINED;
			VkBuffer nf_vb = VK_NULL_HANDLE, nf_ib = VK_NULL_HANDLE;
			VkDeviceMemory nf_vb_memory = VK_NULL_HANDLE, nf_ib_memory = VK_NULL_HANDLE;
			float* nf_vb_map = nullptr;
			u32 nf_hand_vertices[2] = {}, nf_hand_indices[2] = {};
			VkDescriptorSetLayout nf_dsl = VK_NULL_HANDLE;
			VkDescriptorPool nf_dpool = VK_NULL_HANDLE;
			VkDescriptorSet nf_dset[2] = {};
			VkImage nf_tex[2] = {};
			VkDeviceMemory nf_tex_memory[2] = {};
			VkImageView nf_tex_view[2] = {};
			VkSampler nf_tex_sampler[2] = {};
			bool nf_tex_pending = false;
			double nf_wait_sum = 0.0, nf_skin_sum = 0.0, nf_draw_sum = 0.0, nf_upload_sum = 0.0, nf_submit_sum = 0.0;
			double nf_work_max = 0.0;
			u32 nf_calls = 0, nf_skip_fits = 0, nf_skip_views = 0, nf_skip_images = 0, nf_skip_fence = 0;
			std::chrono::steady_clock::time_point nf_window_start{};
			NearField::HandsModel nf_model;
			NearField::Hands nf_load_failed = NearField::Hands::Off;
			VkBuffer nf_staging = VK_NULL_HANDLE;
			VkDeviceMemory nf_staging_memory = VK_NULL_HANDLE;
			void* nf_staging_map = nullptr;
			VkCommandBuffer nf_cmd = VK_NULL_HANDLE;
			VkFence nf_fence = VK_NULL_HANDLE;
			bool nf_fence_submitted = false;
			bool nf_failed = false;
			int nf_logged = -1;
			u32 nf_layers_logged = 0;
			u32 nf_frames = 0;
			double nf_ms_sum = 0.0, nf_ms_max = 0.0;
			std::vector<u32> nf_pixels;
			std::vector<float> nf_depth;

			struct UiChain
			{
				XrSwapchain swapchain = XR_NULL_HANDLE;
				std::vector<XrSwapchainImageVulkan2KHR> images;
				bool ever_released = false;
				bool wait_pending = false;
				bool failed = false;
				SwapchainWait::FailureTracker wait_failures;
				uint32_t pending_index = 0;
				bool premultiplied = false;
				VkCommandBuffer cmd = VK_NULL_HANDLE;
				VkFence fence = VK_NULL_HANDLE;
				bool fence_submitted = false;
				bool warned_copy = false;
			} ui;
			UiPanel::State ui_state;
			GSTexture* ui_source = nullptr;
			bool ui_wants_pixels = false;
			bool ui_imgui = false;
			UiPanel::NavState ui_nav;
			bool ui_open_logged = false;
			XrTime ui_last_time = 0;
			int ui_layers_logged = -1;
			int ui_panel_logged = -1;
			int ui_strip_logged = -1;

			XrSpace view_space = XR_NULL_HANDLE;

			float screen_anchor_x = 0.0f;
			float screen_anchor_y = 0.0f;
			float screen_anchor_z = 0.0f;
			float screen_anchor_yaw = 0.0f;
			float screen_anchor_pitch = 0.0f;

			float display_vfov_deg = 0.0f;
			float display_hfov_deg = 0.0f;
			float display_hfov_union_deg = 0.0f;
			LagMatch::Follower lag_follower;
			bool lag_logged_on = false;
			u32 wp_declared = 0, wp_flat = 0;
			VR::WorldProjection::HeadTally wp_head;
			float wp_eye_offset[2][3] = {};
			float wp_eye_cant_deg = 0.0f;
			u32 wp_eye_frames = 0;
			u32 wp_pairs = 0;
			int wp_st_eyes = 1;
			float wp_st_shift = 0.0f, wp_st_ipd_mm = 0.0f, wp_st_dphys = 0.0f;
			float wp_st_fov[4] = {};
			float wp_eye_fov[2][4] = {};
			bool wp_views_logged = false, wp_declared_logged = false;
			u32 display_fov_attempts = 0;
			DisplayFit::Fit fit_logged;

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
			bool warned_lever_release = false;
			bool warned_release = false;
			bool warned_fence_timeout = false;
			bool warned_swapchain = false;
			VkFormat swapchain_format = VK_FORMAT_R8G8B8A8_SRGB;
			XRSwapchainFormat::Rung swapchain_rung = XRSwapchainFormat::Rung::RawCopy;
			bool swapchain_srgb = true;
			ScreenProjector::Frame proj_frame;

			bool mode_logged = false;
			ScreenCurve::Mode mode_last = ScreenCurve::Mode::Flat;
			bool facets_logged = false;
			bool projector_failed = false;
			u32 proj_frame_w = 0;
			u32 proj_frame_h = 0;

			GSTexture* cast_rt = nullptr;
			std::unique_ptr<GSDownloadTexture> cast_dl;
		} s;

		const char* SwapchainFormatName(char* buf, size_t n)
		{
			return XRSwapchainFormat::FormatName(static_cast<int64_t>(s.swapchain_format), buf, n);
		}

		enum class CopyResult
		{
			Copied,
			Skipped,
			TimeoutZeroLayer
		};


		void SeatCastReadback(GSTexture* src, const GSVector4i& crop, bool publish)
		{
			const s32 full_w = static_cast<s32>(src->GetWidth());
			const s32 full_h = static_cast<s32>(src->GetHeight());
			u32 out_w = static_cast<u32>(crop.width());
			u32 out_h = static_cast<u32>(crop.height());
			if (out_w > SeatCast::kMaxWidth)
			{
				const float scale = static_cast<float>(SeatCast::kMaxWidth) / out_w;
				out_w = SeatCast::kMaxWidth;
				out_h = static_cast<u32>(out_h * scale);
			}
			if (!s.cast_rt || s.cast_rt->GetWidth() != static_cast<int>(out_w) ||
				s.cast_rt->GetHeight() != static_cast<int>(out_h))
			{
				if (s.cast_rt)
					g_gs_device->Recycle(s.cast_rt);
				s.cast_rt = g_gs_device->CreateRenderTarget(out_w, out_h, GSTexture::Format::Color, false);
			}
			if (s.cast_rt)
			{
				const GSVector4 src_uv(
					static_cast<float>(crop.x) / full_w, static_cast<float>(crop.y) / full_h,
					static_cast<float>(crop.z) / full_w, static_cast<float>(crop.w) / full_h);
				g_gs_device->StretchRect(src, src_uv, s.cast_rt,
					GSVector4(0.0f, 0.0f, static_cast<float>(out_w), static_cast<float>(out_h)),
					ShaderConvertSelector(ShaderConvert::COPY), Filter::Biln);
				if (!s.cast_dl || s.cast_dl->GetWidth() < out_w || s.cast_dl->GetHeight() < out_h)
					s.cast_dl = g_gs_device->CreateDownloadTexture(out_w, out_h, GSTexture::Format::Color);
				const GSVector4i rc(0, 0, out_w, out_h);
				if (s.cast_dl)
				{
					s.cast_dl->CopyFromTexture(rc, s.cast_rt, rc, 0);
					s.cast_dl->Flush();
					if (publish && s.cast_dl->Map(rc))
					{
						SeatCast::Publish(s.cast_dl->GetMapPointer(), out_w, out_h, s.cast_dl->GetMapPitch());
						s.cast_dl->Unmap();
					}
				}
			}
		}

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

		void DestroyChains()
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
				chain.waited = false;
				chain.wait_failures = {};
			}
			s.swapchain_width = 0;
			s.swapchain_height = 0;
			s.source_width = 0;
			s.source_height = 0;
		}

		void DestroySwapchains()
		{
			DestroyChains();
			delete s.clamp_rt;
			s.clamp_rt = nullptr;
		}

		void ClampToRuntimeMaxima(u32 w, u32 h, u32& out_w, u32& out_h)
		{
			XRSession::RuntimeLimits lim = XRSession::GetRuntimeLimits();
			float scale = 1.0f;
			if (lim.max_swapchain_width != 0 && w > lim.max_swapchain_width)
				scale = std::min(scale, static_cast<float>(lim.max_swapchain_width) / static_cast<float>(w));
			if (lim.max_swapchain_height != 0 && h > lim.max_swapchain_height)
				scale = std::min(scale, static_cast<float>(lim.max_swapchain_height) / static_cast<float>(h));
			if (scale >= 1.0f)
			{
				out_w = w;
				out_h = h;
				return;
			}
			out_w = std::max(1u, static_cast<u32>(static_cast<float>(w) * scale));
			out_h = std::max(1u, static_cast<u32>(static_cast<float>(h) * scale));
			if (lim.max_swapchain_width != 0)
				out_w = std::min(out_w, lim.max_swapchain_width);
			if (lim.max_swapchain_height != 0)
				out_h = std::min(out_h, lim.max_swapchain_height);
		}

		bool EnsureClampTarget(u32 w, u32 h)
		{
			if (s.clamp_rt && s.clamp_rt->GetWidth() == static_cast<int>(w) &&
				s.clamp_rt->GetHeight() == static_cast<int>(h))
			{
				return true;
			}
			if (s.clamp_rt)
			{
				WaitAllFences();
				delete s.clamp_rt;
				s.clamp_rt = nullptr;
			}
			if (!g_gs_device)
				return false;
			if (s.clamp_failed_width == w && s.clamp_failed_height == h)
				return false;
			{
				s.clamp_rt = g_gs_device->CreateRenderTarget(static_cast<int>(w), static_cast<int>(h),
					GSTexture::Format::Color, false , false );
			}
			if (!s.clamp_rt)
			{
				s.clamp_failed_width = w;
				s.clamp_failed_height = h;
				Console.Error("(VR) Could not allocate the %ux%u downscale target for the clamped swapchain; "
							  "the headset gets no frame at this size. Lower the internal resolution.",
					w, h);
				return false;
			}
			s.clamp_failed_width = 0;
			s.clamp_failed_height = 0;
			return true;
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

			char fmt_name[32];
			Console.WriteLn("(VR) XR swapchain created (eye %u): %ux%u, %u images (%s via %s).",
				eye, w, h, count, SwapchainFormatName(fmt_name, sizeof(fmt_name)),
				XRSwapchainFormat::RungName(s.swapchain_rung));
			return true;
		}

		bool EnsureSwapchains(u32 w, u32 h, bool stereo)
		{
			if (s.source_width != 0 && (s.source_width != w || s.source_height != h))
				DestroySwapchains();

			u32 cw = 0, ch = 0;
			ClampToRuntimeMaxima(w, h, cw, ch);
			const bool clamped = (cw != w || ch != h);
			if (clamped && (s.clamp_warned_width != w || s.clamp_warned_height != h))
			{
				s.clamp_warned_width = w;
				s.clamp_warned_height = h;
				const XRSession::RuntimeLimits& lim = XRSession::GetRuntimeLimits();
				Console.Warning("(VR) Source frame %ux%u exceeds the runtime's swapchain maximum %ux%u; presenting "
								"downscaled to %ux%u (lower the internal resolution to avoid the resample).",
					w, h, lim.max_swapchain_width, lim.max_swapchain_height, cw, ch);
			}

			if (clamped && !EnsureClampTarget(cw, ch))
				return false;

			if (s.chains[0].swapchain == XR_NULL_HANDLE)
			{
				if (!CreateChain(0, cw, ch))
				{
					DestroyChains();
					return false;
				}
			}
			if (stereo && s.chains[1].swapchain == XR_NULL_HANDLE)
			{
				if (!CreateChain(1, cw, ch))
				{
					DestroyChains();
					return false;
				}
			}

			s.source_width = w;
			s.source_height = h;
			s.swapchain_width = cw;
			s.swapchain_height = ch;
			s.warned_swapchain = false;
			return true;
		}

		bool RecordAndSubmitCopy(GSTextureVK* src, u32 src_layer, VkImage dst)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();

			const bool clamped = (s.swapchain_width != s.source_width || s.swapchain_height != s.source_height);
			if (clamped && (!s.clamp_rt || s.clamp_rt->GetWidth() != static_cast<int>(s.swapchain_width) ||
								s.clamp_rt->GetHeight() != static_cast<int>(s.swapchain_height)))
			{
				if (!s.warned_clamp)
				{
					s.warned_clamp = true;
					Console.Error("(VR) Clamped swapchain has no downscale target; frame skipped.");
				}
				return false;
			}


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

			GSTextureVK* copy_src = src;
			u32 copy_layer = src_layer;
			if (clamped)
			{
				GSTextureVK* rt = static_cast<GSTextureVK*>(s.clamp_rt);
				rt->TransitionToLayout(cmd, GSTextureVK::Layout::TransferDst);
				VkImageBlit down = {};
				down.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, src_layer, 1};
				down.srcOffsets[0] = {0, 0, 0};
				down.srcOffsets[1] = {static_cast<s32>(s.source_width), static_cast<s32>(s.source_height), 1};
				down.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				down.dstOffsets[0] = {0, 0, 0};
				down.dstOffsets[1] = {static_cast<s32>(s.swapchain_width), static_cast<s32>(s.swapchain_height), 1};
				vkCmdBlitImage(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rt->GetImage(),
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down, VK_FILTER_LINEAR);
				rt->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
				copy_src = rt;
				copy_layer = 0;
			}

			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

			if (s.swapchain_rung == XRSwapchainFormat::Rung::BlitSwizzle)
			{
				VkImageBlit blit = {};
				blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, copy_layer, 1};
				blit.srcOffsets[0] = {0, 0, 0};
				blit.srcOffsets[1] = {static_cast<s32>(s.swapchain_width), static_cast<s32>(s.swapchain_height), 1};
				blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				blit.dstOffsets[0] = {0, 0, 0};
				blit.dstOffsets[1] = blit.srcOffsets[1];
				vkCmdBlitImage(cmd, copy_src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
			}
			else
			{
				VkImageCopy region = {};
				region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, copy_layer, 1};
				region.srcOffset = {0, 0, 0};
				region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.dstOffset = {0, 0, 0};
				region.extent = {s.swapchain_width, s.swapchain_height, 1};
				vkCmdCopyImage(cmd, copy_src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			}

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

		void RecordFrameCall(XrResult res, const char* call);

		XrResult BudgetedWait(XrSwapchain swapchain, SwapchainWait::Budget& budget)
		{
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = budget.Next();
			const auto t0 = std::chrono::steady_clock::now();
			const XrResult res = xrWaitSwapchainImage(swapchain, &wi);
			budget.Spend(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
			return res;
		}

		enum class EyeWait
		{
			Ready,
			NotReady,
			Skipped,
		};

		EyeWait AcquireAndWait(decltype(s.chains[0])& chain, SwapchainWait::Budget& budget)
		{
			if (chain.waited)
				return EyeWait::Ready;

			if (!chain.wait_pending)
			{
				uint32_t index = 0;
				XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				XrResult res = xrAcquireSwapchainImage(chain.swapchain, &ai, &index);
				if (XR_FAILED(res))
				{
					if (!s.warned_acquire)
					{
						s.warned_acquire = true;
						Console.Error("(VR) xrAcquireSwapchainImage failed (%d).", static_cast<int>(res));
					}
					if (XRFrameHealth::IsTerminal(res))
						RecordFrameCall(res, "xrAcquireSwapchainImage");
					return EyeWait::Skipped;
				}
				chain.pending_index = index;
				chain.wait_pending = true;
			}

			const XrResult res = BudgetedWait(chain.swapchain, budget);
			switch (SwapchainWait::OnWait(chain.wait_failures, res))
			{
				case SwapchainWait::Action::Use:
					chain.wait_pending = false;
					chain.waited = true;
					return EyeWait::Ready;
				case SwapchainWait::Action::Hold:
					if (res == XR_TIMEOUT_EXPIRED)
					{
						if (!s.warned_wait_timeout)
						{
							s.warned_wait_timeout = true;
							Console.Warning("(VR) xrWaitSwapchainImage not ready within the %.1f ms present budget; "
											"re-presenting the last image.", SwapchainWait::kPresentWaitBudgetNs / 1e6);
						}
					}
					else if (!s.warned_wait_fail)
					{
						s.warned_wait_fail = true;
						Console.Error("(VR) xrWaitSwapchainImage failed (%d); retrying it next frame.", static_cast<int>(res));
					}
					return EyeWait::NotReady;
				case SwapchainWait::Action::Reacquire:
					Console.Error("(VR) xrWaitSwapchainImage failed (%d); dropping that acquire and acquiring afresh.",
						static_cast<int>(res));
					chain.wait_pending = false;
					return EyeWait::NotReady;
				case SwapchainWait::Action::Lost:
				default:
					RecordFrameCall(res, "xrWaitSwapchainImage");
					return EyeWait::NotReady;
			}
		}

		CopyResult CopyAndRelease(GSTextureVK* src, u32 src_layer, decltype(s.chains[0])& chain)
		{
			chain.waited = false;
			const bool submitted = RecordAndSubmitCopy(src, src_layer, chain.images[chain.pending_index].image);

			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			const XrResult res = xrReleaseSwapchainImage(chain.swapchain, &ri);
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

		CopyResult CopyToSwapchain(GSTextureVK* src, u32 src_layer, decltype(s.chains[0])& chain,
			SwapchainWait::Budget& budget)
		{
			switch (AcquireAndWait(chain, budget))
			{
				case EyeWait::Ready:
					return CopyAndRelease(src, src_layer, chain);
				case EyeWait::NotReady:
					return CopyResult::TimeoutZeroLayer;
				case EyeWait::Skipped:
				default:
					return CopyResult::Skipped;
			}
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
				lc.wait_failures = {};
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
			bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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
			if (s.swapchain_rung == XRSwapchainFormat::Rung::BlitSwizzle)
			{
				for (u32& p : pixels)
					p = XRSwapchainFormat::SwapRedBlue(p);
			}
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
			u32& layer_count, u32 layer_capacity, bool split, SwapchainWait::Budget& budget)
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
				XrResult wres;
				{
					wres = BudgetedWait(lc.swapchain, budget);
				}
				const SwapchainWait::Action act = SwapchainWait::OnWait(lc.wait_failures, wres);
				if (act == SwapchainWait::Action::Reacquire)
					lc.wait_pending = false;
				else if (act == SwapchainWait::Action::Lost)
					RecordFrameCall(wres, "xrWaitSwapchainImage (lever)");
				if (act != SwapchainWait::Action::Use)
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
					const XrResult rres = xrReleaseSwapchainImage(lc.swapchain, &ri);
					if (XR_FAILED(rres) && !s.warned_lever_release)
					{
						s.warned_lever_release = true;
						Console.Error("(VR) Control quads: lever release failed (%d).", static_cast<int>(rres));
					}
					if (XR_SUCCEEDED(rres) && submitted)
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

		static constexpr u32 kNearFieldEdge = 1024;

		void DestroyNearFieldGpu(VkDevice dev)
		{
			for (auto& c : s.nf)
			{
				if (dev != VK_NULL_HANDLE)
				{
					for (VkFramebuffer fb : c.fbs)
						if (fb != VK_NULL_HANDLE)
							vkDestroyFramebuffer(dev, fb, nullptr);
					for (VkImageView v : c.views)
						if (v != VK_NULL_HANDLE)
							vkDestroyImageView(dev, v, nullptr);
					if (c.depth_view != VK_NULL_HANDLE)
						vkDestroyImageView(dev, c.depth_view, nullptr);
					if (c.depth != VK_NULL_HANDLE)
						vkDestroyImage(dev, c.depth, nullptr);
					if (c.depth_memory != VK_NULL_HANDLE)
						vkFreeMemory(dev, c.depth_memory, nullptr);
				}
				c.fbs.clear();
				c.views.clear();
				c.depth_view = VK_NULL_HANDLE;
				c.depth = VK_NULL_HANDLE;
				c.depth_memory = VK_NULL_HANDLE;
			}
			if (dev != VK_NULL_HANDLE)
			{
				if (s.nf_pipe != VK_NULL_HANDLE)
					vkDestroyPipeline(dev, s.nf_pipe, nullptr);
				if (s.nf_pl != VK_NULL_HANDLE)
					vkDestroyPipelineLayout(dev, s.nf_pl, nullptr);
				for (int i = 0; i < 2; i++)
				{
					if (s.nf_tex_sampler[i] != VK_NULL_HANDLE)
						vkDestroySampler(dev, s.nf_tex_sampler[i], nullptr);
					if (s.nf_tex_view[i] != VK_NULL_HANDLE)
						vkDestroyImageView(dev, s.nf_tex_view[i], nullptr);
					if (s.nf_tex[i] != VK_NULL_HANDLE)
						vkDestroyImage(dev, s.nf_tex[i], nullptr);
					if (s.nf_tex_memory[i] != VK_NULL_HANDLE)
						vkFreeMemory(dev, s.nf_tex_memory[i], nullptr);
				}
				if (s.nf_dpool != VK_NULL_HANDLE)
					vkDestroyDescriptorPool(dev, s.nf_dpool, nullptr);
				if (s.nf_dsl != VK_NULL_HANDLE)
					vkDestroyDescriptorSetLayout(dev, s.nf_dsl, nullptr);
				if (s.nf_rp != VK_NULL_HANDLE)
					vkDestroyRenderPass(dev, s.nf_rp, nullptr);
				if (s.nf_vb_map && s.nf_vb_memory != VK_NULL_HANDLE)
					vkUnmapMemory(dev, s.nf_vb_memory);
				if (s.nf_vb != VK_NULL_HANDLE)
					vkDestroyBuffer(dev, s.nf_vb, nullptr);
				if (s.nf_vb_memory != VK_NULL_HANDLE)
					vkFreeMemory(dev, s.nf_vb_memory, nullptr);
				if (s.nf_ib != VK_NULL_HANDLE)
					vkDestroyBuffer(dev, s.nf_ib, nullptr);
				if (s.nf_ib_memory != VK_NULL_HANDLE)
					vkFreeMemory(dev, s.nf_ib_memory, nullptr);
			}
			s.nf_pipe = VK_NULL_HANDLE;
			s.nf_pl = VK_NULL_HANDLE;
			s.nf_rp = VK_NULL_HANDLE;
			for (int i = 0; i < 2; i++)
			{
				s.nf_tex_sampler[i] = VK_NULL_HANDLE;
				s.nf_tex_view[i] = VK_NULL_HANDLE;
				s.nf_tex[i] = VK_NULL_HANDLE;
				s.nf_tex_memory[i] = VK_NULL_HANDLE;
				s.nf_dset[i] = VK_NULL_HANDLE;
			}
			s.nf_dpool = VK_NULL_HANDLE;
			s.nf_dsl = VK_NULL_HANDLE;
			s.nf_tex_pending = false;
			s.nf_vb = s.nf_ib = VK_NULL_HANDLE;
			s.nf_vb_memory = s.nf_ib_memory = VK_NULL_HANDLE;
			s.nf_vb_map = nullptr;
			s.nf_hand_vertices[0] = s.nf_hand_vertices[1] = 0;
			s.nf_hand_indices[0] = s.nf_hand_indices[1] = 0;
		}

		static u32 NearFieldMemoryType(u32 bits, VkMemoryPropertyFlags want)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			VkPhysicalDeviceMemoryProperties mp = {};
			vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
			for (u32 i = 0; i < mp.memoryTypeCount; i++)
				if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
					return i;
			return UINT32_MAX;
		}

		static bool NearFieldTexture(VkDevice dev, int i, const HandsGltf::HandTexture& want)
		{
			static const HandsGltf::HandTexture s_white = {1, 1, false, false, {0xFFFFFFFFu}};
			const HandsGltf::HandTexture& t = want.Valid() ? want : s_white;
			VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ici.imageType = VK_IMAGE_TYPE_2D;
			ici.format = VK_FORMAT_R8G8B8A8_SRGB;
			ici.extent = {t.width, t.height, 1};
			ici.mipLevels = 1;
			ici.arrayLayers = 1;
			ici.samples = VK_SAMPLE_COUNT_1_BIT;
			ici.tiling = VK_IMAGE_TILING_LINEAR;
			ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
			ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
			if (vkCreateImage(dev, &ici, nullptr, &s.nf_tex[i]) != VK_SUCCESS)
				return false;
			VkMemoryRequirements mr = {};
			vkGetImageMemoryRequirements(dev, s.nf_tex[i], &mr);
			const u32 type = NearFieldMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if (type == UINT32_MAX)
				return false;
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = mr.size;
			mai.memoryTypeIndex = type;
			if (vkAllocateMemory(dev, &mai, nullptr, &s.nf_tex_memory[i]) != VK_SUCCESS ||
				vkBindImageMemory(dev, s.nf_tex[i], s.nf_tex_memory[i], 0) != VK_SUCCESS)
				return false;
			VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
			VkSubresourceLayout lay = {};
			vkGetImageSubresourceLayout(dev, s.nf_tex[i], &sub, &lay);
			void* map = nullptr;
			if (vkMapMemory(dev, s.nf_tex_memory[i], 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
				return false;
			for (u32 y = 0; y < t.height; y++)
				std::memcpy(static_cast<u8*>(map) + lay.offset + lay.rowPitch * y, t.rgba.data() + static_cast<size_t>(y) * t.width,
					static_cast<size_t>(t.width) * 4u);
			vkUnmapMemory(dev, s.nf_tex_memory[i]);
			VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.image = s.nf_tex[i];
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vci.format = VK_FORMAT_R8G8B8A8_SRGB;
			vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			if (vkCreateImageView(dev, &vci, nullptr, &s.nf_tex_view[i]) != VK_SUCCESS)
				return false;
			Vulkan::SamplerBuilder sb;
			sb.SetFilter(VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST);
			sb.SetAddressMode(t.clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
				t.clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);
			s.nf_tex_sampler[i] = sb.Create(dev);
			if (s.nf_tex_sampler[i] == VK_NULL_HANDLE)
				return false;
			Vulkan::DescriptorSetUpdateBuilder dsub;
			dsub.AddCombinedImageSamplerDescriptorWrite(s.nf_dset[i], 0, s.nf_tex_view[i], s.nf_tex_sampler[i]);
			dsub.Update(dev);
			return true;
		}

		static bool NearFieldBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** map)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = bytes;
			bci.usage = usage;
			bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			if (vkCreateBuffer(h.device, &bci, nullptr, buf) != VK_SUCCESS)
				return false;
			VkMemoryRequirements mr = {};
			vkGetBufferMemoryRequirements(h.device, *buf, &mr);
			const u32 type = NearFieldMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if (type == UINT32_MAX)
				return false;
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = mr.size;
			mai.memoryTypeIndex = type;
			if (vkAllocateMemory(h.device, &mai, nullptr, mem) != VK_SUCCESS)
				return false;
			if (vkBindBufferMemory(h.device, *buf, *mem, 0) != VK_SUCCESS)
				return false;
			return vkMapMemory(h.device, *mem, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS;
		}


		bool EnsureNearFieldGpu()
		{
			if (s.nf_gpu_failed)
				return false;
			if (s.nf_pipe != VK_NULL_HANDLE)
				return true;
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			VkDevice dev = h.device;
			auto fail = [dev](const char* what) {
				s.nf_gpu_failed = true;
				Console.Warning("(VR) NearField: the GPU route did not initialise (%s); the CPU raster draws the hands.", what);
				DestroyNearFieldGpu(dev);
				return false;
			};
			if (!g_vulkan_shader_cache)
				return fail("no Vulkan shader cache");
			const VkFormat color = s.swapchain_format;
			for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM})
			{
				VkFormatProperties fp = {};
				vkGetPhysicalDeviceFormatProperties(h.physical_device, f, &fp);
				if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
				{
					s.nf_depth_format = f;
					break;
				}
			}
			if (s.nf_depth_format == VK_FORMAT_UNDEFINED)
				return fail("no depth format");

			Vulkan::RenderPassBuilder rpb;
			rpb.AddAttachment(color, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			rpb.AddAttachment(s.nf_depth_format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			const u32 sp = rpb.AddSubpass();
			rpb.AddSubpassColorAttachment(sp, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			rpb.AddSubpassDepthAttachment(sp, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			s.nf_rp = rpb.Create(dev);
			if (s.nf_rp == VK_NULL_HANDLE)
				return fail("vkCreateRenderPass");
			Vulkan::DescriptorSetLayoutBuilder dslb;
			dslb.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
			s.nf_dsl = dslb.Create(dev);
			if (s.nf_dsl == VK_NULL_HANDLE)
				return fail("vkCreateDescriptorSetLayout");
			Vulkan::PipelineLayoutBuilder plb;
			plb.AddPushConstants(VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(NearField::GpuEyeConstants));
			plb.AddDescriptorSet(s.nf_dsl);
			s.nf_pl = plb.Create(dev);
			if (s.nf_pl == VK_NULL_HANDLE)
				return fail("vkCreatePipelineLayout");
			const VkShaderModule vs = g_vulkan_shader_cache->GetVertexShader(NearField::GpuVertexGlsl());
			const VkShaderModule fs = g_vulkan_shader_cache->GetFragmentShader(NearField::GpuFragmentGlsl(!s.swapchain_srgb));
			if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE)
			{
				if (vs != VK_NULL_HANDLE)
					vkDestroyShaderModule(dev, vs, nullptr);
				if (fs != VK_NULL_HANDLE)
					vkDestroyShaderModule(dev, fs, nullptr);
				return fail("shader compile (see the bad-shader dump in the logs folder)");
			}
			Vulkan::GraphicsPipelineBuilder gpb;
			gpb.SetVertexShader(vs);
			gpb.SetFragmentShader(fs);
			gpb.AddVertexBuffer(0, NearField::kGpuVertexFloats * sizeof(float));
			gpb.AddVertexAttribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);
			gpb.AddVertexAttribute(1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float));
			gpb.AddVertexAttribute(2, 0, VK_FORMAT_R32G32B32_SFLOAT, 6 * sizeof(float));
			gpb.AddVertexAttribute(3, 0, VK_FORMAT_R32G32_SFLOAT, 9 * sizeof(float));
			gpb.SetPrimitiveTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
			gpb.SetNoCullRasterizationState();
			gpb.SetDepthState(true, true, VK_COMPARE_OP_GREATER);
			gpb.SetNoBlendingState();
			gpb.SetDynamicViewportAndScissorState();
			gpb.SetMultisamples(VK_SAMPLE_COUNT_1_BIT);
			gpb.SetPipelineLayout(s.nf_pl);
			gpb.SetRenderPass(s.nf_rp, 0);
			s.nf_pipe = gpb.Create(dev, g_vulkan_shader_cache->GetPipelineCache());
			vkDestroyShaderModule(dev, vs, nullptr);
			vkDestroyShaderModule(dev, fs, nullptr);
			if (s.nf_pipe == VK_NULL_HANDLE)
				return fail("vkCreateGraphicsPipelines");

			for (int e = 0; e < 2; e++)
			{
				auto& c = s.nf[e];
				VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
				ici.imageType = VK_IMAGE_TYPE_2D;
				ici.format = s.nf_depth_format;
				ici.extent = {kNearFieldEdge, kNearFieldEdge, 1};
				ici.mipLevels = 1;
				ici.arrayLayers = 1;
				ici.samples = VK_SAMPLE_COUNT_1_BIT;
				ici.tiling = VK_IMAGE_TILING_OPTIMAL;
				ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
				ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				if (vkCreateImage(dev, &ici, nullptr, &c.depth) != VK_SUCCESS)
					return fail("vkCreateImage (depth)");
				VkMemoryRequirements mr = {};
				vkGetImageMemoryRequirements(dev, c.depth, &mr);
				const u32 type = NearFieldMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				if (type == UINT32_MAX)
					return fail("device-local memory type lookup");
				VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
				mai.allocationSize = mr.size;
				mai.memoryTypeIndex = type;
				if (vkAllocateMemory(dev, &mai, nullptr, &c.depth_memory) != VK_SUCCESS ||
					vkBindImageMemory(dev, c.depth, c.depth_memory, 0) != VK_SUCCESS)
					return fail("depth memory");
				VkImageViewCreateInfo dvi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				dvi.image = c.depth;
				dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
				dvi.format = s.nf_depth_format;
				dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
				if (vkCreateImageView(dev, &dvi, nullptr, &c.depth_view) != VK_SUCCESS)
					return fail("vkCreateImageView (depth)");
				c.views.assign(c.images.size(), VK_NULL_HANDLE);
				c.fbs.assign(c.images.size(), VK_NULL_HANDLE);
				for (size_t i = 0; i < c.images.size(); i++)
				{
					VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
					vci.image = c.images[i].image;
					vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
					vci.format = color;
					vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
					if (vkCreateImageView(dev, &vci, nullptr, &c.views[i]) != VK_SUCCESS)
						return fail("vkCreateImageView (swapchain)");
					const VkImageView atts[2] = {c.views[i], c.depth_view};
					VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
					fci.renderPass = s.nf_rp;
					fci.attachmentCount = 2;
					fci.pAttachments = atts;
					fci.width = kNearFieldEdge;
					fci.height = kNearFieldEdge;
					fci.layers = 1;
					if (vkCreateFramebuffer(dev, &fci, nullptr, &c.fbs[i]) != VK_SUCCESS)
						return fail("vkCreateFramebuffer");
				}
			}

			u32 total_v = 0, total_i = 0;
			for (int i = 0; i < 2; i++)
			{
				const HandsGltf::HandAsset& a = s.nf_model.Asset(i);
				s.nf_hand_vertices[i] = static_cast<u32>(a.positions.size() / 3);
				s.nf_hand_indices[i] = static_cast<u32>(a.indices.size());
				total_v += s.nf_hand_vertices[i];
				total_i += s.nf_hand_indices[i];
			}
			void* vb_map = nullptr;
			void* ib_map = nullptr;
			if (total_v == 0 || total_i == 0 ||
				!NearFieldBuffer(static_cast<VkDeviceSize>(total_v) * NearField::kGpuVertexFloats * sizeof(float),
					VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &s.nf_vb, &s.nf_vb_memory, &vb_map) ||
				!NearFieldBuffer(static_cast<VkDeviceSize>(total_i) * sizeof(u32), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &s.nf_ib,
					&s.nf_ib_memory, &ib_map))
				return fail("the vertex and index buffers");
			s.nf_vb_map = static_cast<float*>(vb_map);
			std::memcpy(ib_map, s.nf_model.Asset(0).indices.data(), s.nf_hand_indices[0] * sizeof(u32));
			std::memcpy(static_cast<u32*>(ib_map) + s.nf_hand_indices[0], s.nf_model.Asset(1).indices.data(),
				s.nf_hand_indices[1] * sizeof(u32));
			vkUnmapMemory(dev, s.nf_ib_memory);
			{
				VkFormatProperties tfp = {};
				vkGetPhysicalDeviceFormatProperties(h.physical_device, VK_FORMAT_R8G8B8A8_SRGB, &tfp);
				if (!(tfp.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
					return fail("no linear-tiled sRGB sampling for the hand textures");
				const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
				VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
				dpci.maxSets = 2;
				dpci.poolSizeCount = 1;
				dpci.pPoolSizes = &ps;
				if (vkCreateDescriptorPool(dev, &dpci, nullptr, &s.nf_dpool) != VK_SUCCESS)
					return fail("vkCreateDescriptorPool");
				const VkDescriptorSetLayout layouts[2] = {s.nf_dsl, s.nf_dsl};
				VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
				dsai.descriptorPool = s.nf_dpool;
				dsai.descriptorSetCount = 2;
				dsai.pSetLayouts = layouts;
				if (vkAllocateDescriptorSets(dev, &dsai, s.nf_dset) != VK_SUCCESS)
					return fail("vkAllocateDescriptorSets");
				for (int i = 0; i < 2; i++)
				{
					const HandsGltf::HandAsset& a = s.nf_model.Asset(i);
					if (!NearFieldTexture(dev, i, a.Textured() ? a.texture : HandsGltf::HandTexture{}))
						return fail("the hand textures");
				}
				s.nf_tex_pending = true;
			}
			Console.WriteLn("(VR) NearField: the GPU route is up (format %d %s, depth %d, %u + %u vertices, %u + %u triangles).",
				static_cast<int>(color), s.swapchain_srgb ? "sRGB in hardware" : "sRGB in the shader",
				static_cast<int>(s.nf_depth_format), s.nf_hand_vertices[0], s.nf_hand_vertices[1], s.nf_hand_indices[0] / 3,
				s.nf_hand_indices[1] / 3);
			return true;
		}

		void DestroyNearField(bool xr_calls)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid())
			{
				if (s.nf_fence != VK_NULL_HANDLE && s.nf_fence_submitted &&
					vkWaitForFences(h.device, 1, &s.nf_fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
					vkQueueWaitIdle(h.queue);
				if (s.nf_staging_map && s.nf_staging_memory != VK_NULL_HANDLE)
					vkUnmapMemory(h.device, s.nf_staging_memory);
				if (s.nf_staging != VK_NULL_HANDLE)
					vkDestroyBuffer(h.device, s.nf_staging, nullptr);
				if (s.nf_staging_memory != VK_NULL_HANDLE)
					vkFreeMemory(h.device, s.nf_staging_memory, nullptr);
				if (s.nf_fence != VK_NULL_HANDLE)
					vkDestroyFence(h.device, s.nf_fence, nullptr);
				if (s.nf_cmd != VK_NULL_HANDLE && s.cmd_pool != VK_NULL_HANDLE)
					vkFreeCommandBuffers(h.device, s.cmd_pool, 1, &s.nf_cmd);
				DestroyNearFieldGpu(h.device);
			}
			else
			{
				DestroyNearFieldGpu(VK_NULL_HANDLE);
			}
			for (auto& c : s.nf)
			{
				if (c.swapchain != XR_NULL_HANDLE && xr_calls)
					xrDestroySwapchain(c.swapchain);
				c = {};
			}
			s.nf_staging = VK_NULL_HANDLE;
			s.nf_staging_memory = VK_NULL_HANDLE;
			s.nf_staging_map = nullptr;
			s.nf_cmd = VK_NULL_HANDLE;
			s.nf_fence = VK_NULL_HANDLE;
			s.nf_fence_submitted = false;
		}

		bool EnsureNearFieldResources()
		{
			if (s.nf_failed)
				return false;
			if (s.nf[0].swapchain != XR_NULL_HANDLE && s.nf[1].swapchain != XR_NULL_HANDLE && s.nf_staging != VK_NULL_HANDLE)
				return true;
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			auto fail = [](const char* what, int code) {
				s.nf_failed = true;
				Console.Error("(VR) NearField: %s failed (%d); no near-field layer this session.", what, code);
				DestroyNearField(true);
				return false;
			};
			if (!h.IsValid() || s.cmd_pool == VK_NULL_HANDLE)
				return fail("the Vulkan handles", 0);
			for (int e = 0; e < 2; e++)
			{
				auto& c = s.nf[e];
				XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
				ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
				ci.format = static_cast<int64_t>(s.swapchain_format);
				ci.sampleCount = 1;
				ci.width = kNearFieldEdge;
				ci.height = kNearFieldEdge;
				ci.faceCount = 1;
				ci.arraySize = 1;
				ci.mipCount = 1;
				const XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &c.swapchain);
				if (XR_FAILED(res))
				{
					c.swapchain = XR_NULL_HANDLE;
					return fail("xrCreateSwapchain", static_cast<int>(res));
				}
				uint32_t count = 0;
				if (!CheckXR(xrEnumerateSwapchainImages(c.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages (near-field)") ||
					count == 0)
					return fail("xrEnumerateSwapchainImages", 0);
				c.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
				if (!CheckXR(xrEnumerateSwapchainImages(c.swapchain, count, &count,
								 reinterpret_cast<XrSwapchainImageBaseHeader*>(c.images.data())),
						"xrEnumerateSwapchainImages (near-field)"))
					return fail("xrEnumerateSwapchainImages", 0);
			}
			constexpr VkDeviceSize kBytes = static_cast<VkDeviceSize>(kNearFieldEdge) * kNearFieldEdge * 4u * 2u;
			VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = kBytes;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VkResult vr = vkCreateBuffer(h.device, &bci, nullptr, &s.nf_staging);
			if (vr != VK_SUCCESS)
				return fail("vkCreateBuffer", static_cast<int>(vr));
			VkMemoryRequirements mr = {};
			vkGetBufferMemoryRequirements(h.device, s.nf_staging, &mr);
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
			vr = vkAllocateMemory(h.device, &mai, nullptr, &s.nf_staging_memory);
			if (vr != VK_SUCCESS)
				return fail("vkAllocateMemory", static_cast<int>(vr));
			vr = vkBindBufferMemory(h.device, s.nf_staging, s.nf_staging_memory, 0);
			if (vr != VK_SUCCESS)
				return fail("vkBindBufferMemory", static_cast<int>(vr));
			vr = vkMapMemory(h.device, s.nf_staging_memory, 0, VK_WHOLE_SIZE, 0, &s.nf_staging_map);
			if (vr != VK_SUCCESS)
				return fail("vkMapMemory", static_cast<int>(vr));
			VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			cai.commandPool = s.cmd_pool;
			cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cai.commandBufferCount = 1;
			vr = vkAllocateCommandBuffers(h.device, &cai, &s.nf_cmd);
			if (vr != VK_SUCCESS)
				return fail("vkAllocateCommandBuffers", static_cast<int>(vr));
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			vr = vkCreateFence(h.device, &fci, nullptr, &s.nf_fence);
			if (vr != VK_SUCCESS)
				return fail("vkCreateFence", static_cast<int>(vr));
			Console.WriteLn("(VR) NearField: two %ux%u eye swapchains (%u + %u images) and a %llu-byte staging buffer created.",
				kNearFieldEdge, kNearFieldEdge, static_cast<unsigned>(s.nf[0].images.size()),
				static_cast<unsigned>(s.nf[1].images.size()), static_cast<unsigned long long>(kBytes));
			return true;
		}


		void LogNearFieldCost();
		bool BuildNearFieldLayer(XrCompositionLayerProjection* layer, XrCompositionLayerProjectionView* views,
			const XrCompositionLayerBaseHeader** layers, u32& layer_count, u32 capacity, XrTime display_time,
			SwapchainWait::Budget& budget)
		{
			const NearField::Hands want = NearField::ProfileHands();
			if (want == NearField::Hands::Off)
			{
				if (s.nf[0].swapchain != XR_NULL_HANDLE || s.nf_staging != VK_NULL_HANDLE)
				{
					DestroyNearField(true);
					Console.WriteLn("(VR) NearField: hands off: the near-field swapchains destroyed.");
				}
				return false;
			}
			if (!s.nf_model.Loaded() || s.nf_model.Which() != want)
			{
				if (s.nf_load_failed == want)
					return false;
				std::string err;
				if (!s.nf_model.Load(EmuFolders::Resources, want, &err))
				{
					s.nf_load_failed = want;
					Console.Error("(VR) NearField: hands '%s' did not load (%s); no hands are drawn.", NearField::HandsName(want),
						err.c_str());
					return false;
				}
				Console.WriteLn("(VR) NearField: hands '%s' loaded (%u + %u triangles).", NearField::HandsName(want),
					s.nf_model.Asset(0).TriangleCount(), s.nf_model.Asset(1).TriangleCount());
				if (s.nf_pipe != VK_NULL_HANDLE)
				{
					const Internal::VulkanHandles& vh = Internal::GetVulkanHandles();
					if (s.nf_fence_submitted && vkWaitForFences(vh.device, 1, &s.nf_fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
						vkQueueWaitIdle(vh.queue);
					DestroyNearFieldGpu(vh.device);
				}
			}
			if (s.nf_model.RefreshGameHands())
			{
				Console.WriteLn("(VR) NearField: hands now left %s, right %s (%u + %u triangles).",
					s.nf_model.UsingGameHand(0) ? "game" : "glove", s.nf_model.UsingGameHand(1) ? "game" : "glove",
					s.nf_model.Asset(0).TriangleCount(), s.nf_model.Asset(1).TriangleCount());
				if (s.nf_pipe != VK_NULL_HANDLE)
				{
					const Internal::VulkanHandles& vh = Internal::GetVulkanHandles();
					if (s.nf_fence_submitted && vkWaitForFences(vh.device, 1, &s.nf_fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
						vkQueueWaitIdle(vh.queue);
					DestroyNearFieldGpu(vh.device);
				}
			}
			if (s.nf_calls++ == 0)
				s.nf_window_start = std::chrono::steady_clock::now();
			if (!NearField::Fits(layer_count, capacity, XRSession::GetRuntimeLimits().max_layer_count))
			{
				s.nf_skip_fits++;
				return false;
			}

			XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
			vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			vli.displayTime = display_time;
			vli.space = XRSession::GetSpace();
			XrViewState vs = {XR_TYPE_VIEW_STATE};
			XrView xv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
			u32 vcount = 0;
			if (XR_FAILED(xrLocateViews(XRSession::GetSession(), &vli, &vs, 2, &vcount, xv)) || vcount != 2 ||
				!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) || !(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
			{
				s.nf_skip_views++;
				return false;
			}

			const VRInputSnapshot snap = GetInputSnapshot();
			std::array<NearField::LeverSlot, 2> levers{};
			for (int i = 0; i < 2; i++)
			{
				const ControlQuads::Slot q = ControlQuads::Get(i);
				levers[i] = {q.active, q.grabbed, q.t, q.side, q.height, q.forward, q.yaw_deg};
			}
			const NearField::Anchor anchor{s.screen_anchor_x, s.screen_anchor_y, s.screen_anchor_z, s.screen_anchor_yaw};
			const std::array<NearField::HandDraw, 2> draw =
				NearField::PlaceHands(snap.hands, snap.actions_active && !snap.stale, levers, anchor, NearField::JointsForModel(s.nf_model.Which(), HandTracking::JointsForDraw()), {HandsGltf::WeightedJoints(s.nf_model.Asset(0)), HandsGltf::WeightedJoints(s.nf_model.Asset(1))}, HandTracking::ControllersInHands(snap, HandTracking::CurrentProfiles()));
			std::array<NearField::Mesh, 2> meshes{};
			using Clock = std::chrono::steady_clock;
			auto ms_between = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
			const auto t0 = Clock::now();
			const size_t mesh_count = s.nf_model.Update(draw, &meshes);
			const auto t_skin = Clock::now();
			if (mesh_count == 0)
			{
				if (s.nf_logged != 0)
				{
					LogNearFieldCost();
					s.nf_logged = 0;
					Console.WriteLn("(VR) NearField: no hand to draw (no tracked controller, no grabbed lever): no layer.");
				}
				s.nf_calls = s.nf_skip_fits = s.nf_skip_views = 0;
				return false;
			}
			const bool setup_frame = (s.nf_staging == VK_NULL_HANDLE) || (s.nf_pipe == VK_NULL_HANDLE && !s.nf_gpu_failed);
			if (!EnsureNearFieldResources())
				return false;
			const bool gpu = EnsureNearFieldGpu();
			const double setup_ms = setup_frame ? ms_between(t_skin, Clock::now()) : 0.0;
			if (!s.nf_gpu_logged || setup_frame)
			{
				s.nf_gpu_logged = true;
				Console.WriteLn("(VR) NearField: the hands are drawn by the %s (setup %.1f ms, once: not in the cost line).",
					gpu ? "GPU pipeline" : "CPU raster (the fallback)", setup_ms);
			}

			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			bool ready[2] = {false, false};
			for (int e = 0; e < 2; e++)
			{
				auto& c = s.nf[e];
				if (!c.wait_pending)
				{
					XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
					uint32_t index = 0;
					if (XR_FAILED(xrAcquireSwapchainImage(c.swapchain, &ai, &index)))
						continue;
					c.pending_index = index;
					c.wait_pending = true;
				}
				const XrResult wres = BudgetedWait(c.swapchain, budget);
				const SwapchainWait::Action act = SwapchainWait::OnWait(c.wait_failures, wres);
				if (act == SwapchainWait::Action::Reacquire)
					c.wait_pending = false;
				else if (act == SwapchainWait::Action::Lost)
					RecordFrameCall(wres, "xrWaitSwapchainImage (near-field)");
				if (act != SwapchainWait::Action::Use)
					continue;
				c.wait_pending = false;
				ready[e] = true;
			}
			auto release_ready = [&]() {
				for (int e = 0; e < 2; e++)
				{
					if (!ready[e])
						continue;
					XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
					xrReleaseSwapchainImage(s.nf[e].swapchain, &ri);
				}
			};
			if (!ready[0] || !ready[1])
			{
				s.nf_skip_images++;
				release_ready();
				return false;
			}
			if (s.nf_fence_submitted)
			{
				if (vkWaitForFences(h.device, 1, &s.nf_fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
				{
					s.nf_skip_fence++;
					release_ready();
					return false;
				}
				vkResetFences(h.device, 1, &s.nf_fence);
				s.nf_fence_submitted = false;
			}
			const auto t_waited = Clock::now();
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if (vkBeginCommandBuffer(s.nf_cmd, &bi) != VK_SUCCESS)
			{
				release_ready();
				return false;
			}
			constexpr VkDeviceSize kEyeBytes = static_cast<VkDeviceSize>(kNearFieldEdge) * kNearFieldEdge * 4u;
			NearField::EyeView evs[2];
			for (int e = 0; e < 2; e++)
			{
				NearField::EyeView& ev = evs[e];
				ev.orientation = {xv[e].pose.orientation.x, xv[e].pose.orientation.y, xv[e].pose.orientation.z, xv[e].pose.orientation.w};
				ev.position = {xv[e].pose.position.x, xv[e].pose.position.y, xv[e].pose.position.z};
				ev.angle_left = xv[e].fov.angleLeft;
				ev.angle_right = xv[e].fov.angleRight;
				ev.angle_up = xv[e].fov.angleUp;
				ev.angle_down = xv[e].fov.angleDown;
			}
			bool dump_due = false;
			(void)dump_due;
			double draw_ms = 0.0, upload_ms = 0.0;
			if (gpu)
			{
				const auto ta = Clock::now();
				for (size_t k = 0; k < mesh_count; k++)
				{
					const bool right = (meshes[k].indices == &s.nf_model.Asset(1).indices);
					NearField::PackGpuVertices(meshes[k], s.nf_vb_map + static_cast<size_t>(right ? s.nf_hand_vertices[0] : 0) * NearField::kGpuVertexFloats);
				}
				const auto tb = Clock::now();
				upload_ms = ms_between(ta, tb);
				if (s.nf_tex_pending)
				{
					for (int i = 0; i < 2; i++)
						ImageBarrier(s.nf_cmd, s.nf_tex[i], VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
							VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT,
							VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
					s.nf_tex_pending = false;
				}
				for (int e = 0; e < 2; e++)
				{
					NearField::GpuEyeConstants pc;
					if (!NearField::MakeGpuEyeConstants(evs[e], &pc))
						pc = {};
					VkClearValue cv[2] = {};
					cv[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
					cv[1].depthStencil = {0.0f, 0};
					VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
					rpbi.renderPass = s.nf_rp;
					rpbi.framebuffer = s.nf[e].fbs[s.nf[e].pending_index];
					rpbi.renderArea = {{0, 0}, {kNearFieldEdge, kNearFieldEdge}};
					rpbi.clearValueCount = 2;
					rpbi.pClearValues = cv;
					vkCmdBeginRenderPass(s.nf_cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
					vkCmdBindPipeline(s.nf_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.nf_pipe);
					const VkViewport vp = {0.0f, 0.0f, static_cast<float>(kNearFieldEdge), static_cast<float>(kNearFieldEdge), 0.0f, 1.0f};
					const VkRect2D sc = {{0, 0}, {kNearFieldEdge, kNearFieldEdge}};
					vkCmdSetViewport(s.nf_cmd, 0, 1, &vp);
					vkCmdSetScissor(s.nf_cmd, 0, 1, &sc);
					const VkDeviceSize vb_offset = 0;
					vkCmdBindVertexBuffers(s.nf_cmd, 0, 1, &s.nf_vb, &vb_offset);
					vkCmdBindIndexBuffer(s.nf_cmd, s.nf_ib, 0, VK_INDEX_TYPE_UINT32);
					vkCmdPushConstants(s.nf_cmd, s.nf_pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
					for (size_t k = 0; k < mesh_count; k++)
					{
						const bool right = (meshes[k].indices == &s.nf_model.Asset(1).indices);
						vkCmdBindDescriptorSets(s.nf_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.nf_pl, 0, 1, &s.nf_dset[right ? 1 : 0], 0, nullptr);
						vkCmdDrawIndexed(s.nf_cmd, s.nf_hand_indices[right ? 1 : 0], 1, right ? s.nf_hand_indices[0] : 0,
							static_cast<s32>(right ? s.nf_hand_vertices[0] : 0), 0);
					}
					vkCmdEndRenderPass(s.nf_cmd);
				}
				draw_ms = ms_between(tb, Clock::now());
			}
			else
			{
				for (int e = 0; e < 2; e++)
				{
					const NearField::RasterStats st =
						NearField::Raster(evs[e], kNearFieldEdge, kNearFieldEdge, meshes.data(), mesh_count, &s.nf_pixels, &s.nf_depth);
					draw_ms += st.clear_ms + st.draw_ms;
					const auto tu = Clock::now();
					if (s.swapchain_rung == XRSwapchainFormat::Rung::BlitSwizzle)
					{
						for (u32& p : s.nf_pixels)
							p = XRSwapchainFormat::SwapRedBlue(p);
					}
					const VkDeviceSize offset = kEyeBytes * static_cast<VkDeviceSize>(e);
					std::memcpy(static_cast<u8*>(s.nf_staging_map) + offset, s.nf_pixels.data(), static_cast<size_t>(kEyeBytes));
					const VkImage dst = s.nf[e].images[s.nf[e].pending_index].image;
					ImageBarrier(s.nf_cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
					VkBufferImageCopy region = {};
					region.bufferOffset = offset;
					region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
					region.imageExtent = {kNearFieldEdge, kNearFieldEdge, 1};
					vkCmdCopyBufferToImage(s.nf_cmd, s.nf_staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
					ImageBarrier(s.nf_cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
						VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
					upload_ms += ms_between(tu, Clock::now());
				}
			}
			for (int e = 0; e < 2; e++)
			{
				XrCompositionLayerProjectionView& pv = views[e];
				pv.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				pv.next = nullptr;
				pv.pose = xv[e].pose;
				pv.fov = xv[e].fov;
				pv.subImage.swapchain = s.nf[e].swapchain;
				pv.subImage.imageRect = {{0, 0}, {static_cast<s32>(kNearFieldEdge), static_cast<s32>(kNearFieldEdge)}};
				pv.subImage.imageArrayIndex = 0;
			}
			const auto t_submit = Clock::now();
			bool submitted = false;
			if (vkEndCommandBuffer(s.nf_cmd) == VK_SUCCESS)
			{
				VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
				si.commandBufferCount = 1;
				si.pCommandBuffers = &s.nf_cmd;
				submitted = (vkQueueSubmit(h.queue, 1, &si, s.nf_fence) == VK_SUCCESS);
				s.nf_fence_submitted = submitted;
			}
			release_ready();
			const auto t_end = Clock::now();
			const double ms = ms_between(t0, t_end);
			if (!submitted)
				return false;

			layer->type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
			layer->next = nullptr;
			layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			layer->space = XRSession::GetSpace();
			layer->viewCount = 2;
			layer->views = views;
			layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(layer);

			if (s.nf_logged != 1 || s.nf_layers_logged != layer_count)
			{
				s.nf_logged = 1;
				s.nf_layers_logged = layer_count;
				Console.WriteLn("(VR) NearField: layers=%u, the near-field layer last (hands %s%s / %s%s; capacity %u, runtime max %u).",
					layer_count, draw[0].draw ? "L" : "-", draw[0].on_lever ? " on lever" : "", draw[1].draw ? "R" : "-",
					draw[1].on_lever ? " on lever" : "", capacity, XRSession::GetRuntimeLimits().max_layer_count);
			}
			const double wait_ms = ms_between(t_skin, t_waited) - setup_ms;
			const double frame_ms = ms - setup_ms;
			s.nf_frames++;
			s.nf_ms_sum += frame_ms;
			s.nf_ms_max = std::max(s.nf_ms_max, frame_ms);
			s.nf_work_max = std::max(s.nf_work_max, frame_ms - wait_ms);
			s.nf_wait_sum += wait_ms;
			s.nf_skin_sum += ms_between(t0, t_skin);
			s.nf_draw_sum += draw_ms;
			s.nf_upload_sum += upload_ms;
			s.nf_submit_sum += ms_between(t_submit, t_end);
			if (s.nf_frames == 300)
				LogNearFieldCost();
			return true;
		}

		void LogNearFieldCost()
		{
			if (s.nf_frames > 0)
			{
				const double n = s.nf_frames;
				const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.nf_window_start).count();
				Console.WriteLn("(VR) NearField cost (%s): %.2f ms mean, %.2f ms max per frame over %u frames (pose, skin, two %u px eyes, upload); "
								"waits %.2f, pose+skin %.2f, draw %.2f, upload %.2f, submit %.2f ms mean, %.2f ms max without the waits; "
								"drawn %u of %u XR frames in %.2f s (%.1f Hz drawn, %.1f Hz XR); skipped: no slot %u, views %u, images %u, fence %u.",
					s.nf_pipe != VK_NULL_HANDLE ? "GPU" : "CPU", s.nf_ms_sum / n, s.nf_ms_max, s.nf_frames, kNearFieldEdge,
					s.nf_wait_sum / n, s.nf_skin_sum / n, s.nf_draw_sum / n, s.nf_upload_sum / n, s.nf_submit_sum / n, s.nf_work_max,
					s.nf_frames, s.nf_calls, secs, secs > 0 ? n / secs : 0.0, secs > 0 ? s.nf_calls / secs : 0.0, s.nf_skip_fits,
					s.nf_skip_views, s.nf_skip_images, s.nf_skip_fence);
			}
			s.nf_frames = 0;
			s.nf_ms_sum = s.nf_ms_max = s.nf_work_max = 0.0;
			s.nf_wait_sum = s.nf_skin_sum = s.nf_draw_sum = s.nf_upload_sum = s.nf_submit_sum = 0.0;
			s.nf_calls = s.nf_skip_fits = s.nf_skip_views = s.nf_skip_images = s.nf_skip_fence = 0;
		}

		void DestroyUiPanel(bool xr_calls)
		{
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (h.IsValid())
			{
				if (s.ui.fence != VK_NULL_HANDLE && s.ui.fence_submitted &&
					vkWaitForFences(h.device, 1, &s.ui.fence, VK_TRUE, ONE_SECOND_NS) != VK_SUCCESS)
					vkQueueWaitIdle(h.queue);
				if (s.ui.fence != VK_NULL_HANDLE)
					vkDestroyFence(h.device, s.ui.fence, nullptr);
				if (s.ui.cmd != VK_NULL_HANDLE && s.cmd_pool != VK_NULL_HANDLE)
					vkFreeCommandBuffers(h.device, s.cmd_pool, 1, &s.ui.cmd);
			}
			if (s.ui.swapchain != XR_NULL_HANDLE && xr_calls)
				xrDestroySwapchain(s.ui.swapchain);
			s.ui = {};
			s.ui_state = {};
			s.ui_source = nullptr;
			s.ui_wants_pixels = false;
			s.ui_nav = {};
			s.ui_open_logged = false;
			UiPanel::SetOwnsInput(false);
			XRInput::NoteUiOpen(false);
			ImGuiManager::SetVRPanelLayout(false);
			s.ui_last_time = 0;
			s.ui_layers_logged = s.ui_panel_logged = s.ui_strip_logged = -1;
			UiPanel::ResetOsd();
		}

		bool UploadUiPattern(const Internal::VulkanHandles& h, VkImage dst)
		{
			std::vector<u32> px;
			UiPanel::TestPattern(px);
			if (s.swapchain_rung == XRSwapchainFormat::Rung::BlitSwizzle)
			{
				for (u32& p : px)
					p = XRSwapchainFormat::SwapRedBlue(p);
			}
			const VkDeviceSize bytes = static_cast<VkDeviceSize>(px.size()) * 4u;
			VkBuffer buf = VK_NULL_HANDLE;
			VkDeviceMemory mem = VK_NULL_HANDLE;
			VkCommandBuffer cmd = VK_NULL_HANDLE;
			VkFence fence = VK_NULL_HANDLE;
			bool submitted = false, done = false;
			do
			{
				VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
				bci.size = bytes;
				bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
				bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
				if (vkCreateBuffer(h.device, &bci, nullptr, &buf) != VK_SUCCESS)
					break;
				VkMemoryRequirements mr = {};
				vkGetBufferMemoryRequirements(h.device, buf, &mr);
				VkPhysicalDeviceMemoryProperties mp = {};
				vkGetPhysicalDeviceMemoryProperties(h.physical_device, &mp);
				constexpr VkMemoryPropertyFlags kWant = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				u32 type = UINT32_MAX;
				for (u32 i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
				{
					if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & kWant) == kWant)
						type = i;
				}
				if (type == UINT32_MAX)
					break;
				VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
				mai.allocationSize = mr.size;
				mai.memoryTypeIndex = type;
				if (vkAllocateMemory(h.device, &mai, nullptr, &mem) != VK_SUCCESS ||
					vkBindBufferMemory(h.device, buf, mem, 0) != VK_SUCCESS)
					break;
				void* map = nullptr;
				if (vkMapMemory(h.device, mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
					break;
				std::memcpy(map, px.data(), static_cast<size_t>(bytes));
				vkUnmapMemory(h.device, mem);

				VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				cai.commandPool = s.cmd_pool;
				cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				cai.commandBufferCount = 1;
				if (vkAllocateCommandBuffers(h.device, &cai, &cmd) != VK_SUCCESS)
				{
					cmd = VK_NULL_HANDLE;
					break;
				}
				VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
				bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
					break;
				ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
				VkBufferImageCopy region = {};
				region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.imageExtent = {UiPanel::kTextureWidth, UiPanel::kTextureHeight, 1};
				vkCmdCopyBufferToImage(cmd, buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
				ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
				if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
					break;
				VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
				if (vkCreateFence(h.device, &fci, nullptr, &fence) != VK_SUCCESS)
				{
					fence = VK_NULL_HANDLE;
					break;
				}
				VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
				si.commandBufferCount = 1;
				si.pCommandBuffers = &cmd;
				submitted = (vkQueueSubmit(h.queue, 1, &si, fence) == VK_SUCCESS);
				if (submitted)
					done = (vkWaitForFences(h.device, 1, &fence, VK_TRUE, ONE_SECOND_NS) == VK_SUCCESS);
			} while (false);
			if (submitted && !done)
				vkQueueWaitIdle(h.queue);
			if (fence != VK_NULL_HANDLE)
				vkDestroyFence(h.device, fence, nullptr);
			if (cmd != VK_NULL_HANDLE)
				vkFreeCommandBuffers(h.device, s.cmd_pool, 1, &cmd);
			if (buf != VK_NULL_HANDLE)
				vkDestroyBuffer(h.device, buf, nullptr);
			if (mem != VK_NULL_HANDLE)
				vkFreeMemory(h.device, mem, nullptr);
			return done;
		}

		bool CopyUiSource(const Internal::VulkanHandles& h, GSTextureVK* src, VkImage dst)
		{
			const auto warn = [](const char* what, int code) {
				if (!s.ui.warned_copy)
				{
					s.ui.warned_copy = true;
					Console.Warning("(VR) UiPanel: %s (%d); the panel keeps its last image.", what, code);
				}
				return false;
			};
			if (!src || src->GetWidth() != static_cast<int>(UiPanel::kTextureWidth) ||
				src->GetHeight() != static_cast<int>(UiPanel::kTextureHeight))
				return warn("panel source missing or the wrong size", 0);
			if (s.ui.cmd == VK_NULL_HANDLE)
			{
				VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
				cai.commandPool = s.cmd_pool;
				cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				cai.commandBufferCount = 1;
				if (const VkResult r = vkAllocateCommandBuffers(h.device, &cai, &s.ui.cmd); r != VK_SUCCESS)
				{
					s.ui.cmd = VK_NULL_HANDLE;
					return warn("vkAllocateCommandBuffers", static_cast<int>(r));
				}
			}
			if (s.ui.fence == VK_NULL_HANDLE)
			{
				VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
				if (const VkResult r = vkCreateFence(h.device, &fci, nullptr, &s.ui.fence); r != VK_SUCCESS)
				{
					s.ui.fence = VK_NULL_HANDLE;
					return warn("vkCreateFence", static_cast<int>(r));
				}
			}
			if (s.ui.fence_submitted)
			{
				if (const VkResult r = vkWaitForFences(h.device, 1, &s.ui.fence, VK_TRUE, ONE_SECOND_NS); r != VK_SUCCESS)
					return warn("the previous panel copy did not finish", static_cast<int>(r));
				vkResetFences(h.device, 1, &s.ui.fence);
				s.ui.fence_submitted = false;
			}
			VkCommandBuffer cmd = s.ui.cmd;
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if (const VkResult r = vkBeginCommandBuffer(cmd, &bi); r != VK_SUCCESS)
				return warn("vkBeginCommandBuffer", static_cast<int>(r));
			src->TransitionToLayout(cmd, GSTextureVK::Layout::TransferSrc);
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			const s32 w = static_cast<s32>(UiPanel::kTextureWidth), hgt = static_cast<s32>(UiPanel::kTextureHeight);
			if (s.swapchain_rung == XRSwapchainFormat::Rung::BlitSwizzle)
			{
				VkImageBlit blit = {};
				blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				blit.srcOffsets[1] = {w, hgt, 1};
				blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				blit.dstOffsets[1] = {w, hgt, 1};
				vkCmdBlitImage(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					1, &blit, VK_FILTER_NEAREST);
			}
			else
			{
				VkImageCopy region = {};
				region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				region.extent = {UiPanel::kTextureWidth, UiPanel::kTextureHeight, 1};
				vkCmdCopyImage(cmd, src->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					1, &region);
			}
			ImageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
			if (const VkResult r = vkEndCommandBuffer(cmd); r != VK_SUCCESS)
				return warn("vkEndCommandBuffer", static_cast<int>(r));
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cmd;
			if (const VkResult r = vkQueueSubmit(h.queue, 1, &si, s.ui.fence); r != VK_SUCCESS)
				return warn("vkQueueSubmit", static_cast<int>(r));
			s.ui.fence_submitted = true;
			return true;
		}

		bool EnsureUiPanel(SwapchainWait::Budget& budget)
		{
			if (s.ui.failed)
				return false;
			if (!s.ui_imgui && s.ui.ever_released)
				return true;
			if (s.ui_imgui && s.ui_source == nullptr)
				return s.ui.ever_released;
			const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
			if (!h.IsValid() || s.cmd_pool == VK_NULL_HANDLE)
				return false;
			const auto fail = [](const char* what, int code) {
				s.ui.failed = true;
				Console.Error("(VR) UiPanel: %s failed (%d); no UI panel this session.", what, code);
				return false;
			};
			if (s.ui.swapchain == XR_NULL_HANDLE)
			{
				XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
				ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
				ci.format = static_cast<int64_t>(s.swapchain_format);
				ci.sampleCount = 1;
				ci.width = UiPanel::kTextureWidth;
				ci.height = UiPanel::kTextureHeight;
				ci.faceCount = 1;
				ci.arraySize = 1;
				ci.mipCount = 1;
				const XrResult res = xrCreateSwapchain(XRSession::GetSession(), &ci, &s.ui.swapchain);
				if (XR_FAILED(res))
				{
					s.ui.swapchain = XR_NULL_HANDLE;
					return fail("xrCreateSwapchain", static_cast<int>(res));
				}
				uint32_t count = 0;
				if (!CheckXR(xrEnumerateSwapchainImages(s.ui.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages (ui)") ||
					count == 0)
					return fail("xrEnumerateSwapchainImages", 0);
				s.ui.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
				if (!CheckXR(xrEnumerateSwapchainImages(s.ui.swapchain, count, &count,
								 reinterpret_cast<XrSwapchainImageBaseHeader*>(s.ui.images.data())),
						"xrEnumerateSwapchainImages (ui)"))
					return fail("xrEnumerateSwapchainImages", 0);
				Console.WriteLn("(VR) UiPanel: swapchain created (%ux%u, %u images, source %s).", UiPanel::kTextureWidth,
					UiPanel::kTextureHeight, count, s.ui_imgui ? "the emulator's ImGui" : "the test pattern");
			}
			uint32_t index = 0;
			if (s.ui.wait_pending)
			{
				index = s.ui.pending_index;
			}
			else
			{
				XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				if (XR_FAILED(xrAcquireSwapchainImage(s.ui.swapchain, &ai, &index)))
					return s.ui.ever_released;
				s.ui.pending_index = index;
				s.ui.wait_pending = true;
			}
			const XrResult wres = BudgetedWait(s.ui.swapchain, budget);
			const SwapchainWait::Action act = SwapchainWait::OnWait(s.ui.wait_failures, wres);
			if (act == SwapchainWait::Action::Reacquire)
				s.ui.wait_pending = false;
			else if (act == SwapchainWait::Action::Lost)
				RecordFrameCall(wres, "xrWaitSwapchainImage (ui panel)");
			if (act != SwapchainWait::Action::Use)
				return s.ui.ever_released;
			s.ui.wait_pending = false;
			const bool written = s.ui_imgui ? CopyUiSource(h, static_cast<GSTextureVK*>(s.ui_source), s.ui.images[index].image) :
											  UploadUiPattern(h, s.ui.images[index].image);
			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			const XrResult rres = xrReleaseSwapchainImage(s.ui.swapchain, &ri);
			if (XR_FAILED(rres))
				return fail("xrReleaseSwapchainImage", static_cast<int>(rres));
			if (!written)
			{
				if (!s.ui_imgui)
					return fail("test pattern upload", 0);
				return s.ui.ever_released;
			}
			s.ui.premultiplied = s.ui_imgui;
			if (!s.ui.ever_released)
				Console.WriteLn("(VR) UiPanel: %s.", s.ui_imgui ? "first ImGui frame copied" : "test pattern uploaded");
			s.ui.ever_released = true;
			return true;
		}


		void UiPanelInput()
		{
			ImGuiManager::SetVRPanelLayout(true);
			const bool was_open = FullscreenUI::HasActiveWindow();
			UiPanel::SystemRoute route = UiPanel::RouteSystemEvents(XRInput::TakePadXREvents(), was_open);
			if (s_ui_recentre_requested.exchange(false, std::memory_order_acq_rel))
			{
				route.recentre = true;
				route.replace_panel = was_open;
			}
			if (route.recentre)
			{
				RequestScreenReanchor();
#ifndef VR_UIPANEL_PLANTED_RECENTRE_SCREEN_ONLY
				CameraDriver::RequestRecenter();
				Host::AddKeyedOSDMessage("VRRecenter", "VR recentered (head + screen).", 2.0f);
#endif
				Console.WriteLn("(VR) UiPanel: recentre (head + screen)%s.",
					route.replace_panel ? "; the menu stays open and re-places" : "");
			}
			if (route.replace_panel)
				UiPanel::RequestReplace(s.ui_state);
			if (route.close_ui)
				FullscreenUI::CloseVRUi();
			else if (route.open_quick_menu)
				FullscreenUI::OpenVRQuickMenu();
			const bool open = FullscreenUI::HasActiveWindow();
			XRInput::NoteUiOpen(open);
			UiPanel::SetOwnsInput(open);
			if (open != s.ui_open_logged)
			{
				s.ui_open_logged = open;
				Console.WriteLn("(VR) UiPanel: the UI %s the pad.", open ? "owns" : "releases");
			}

			std::vector<UiPanel::NavEdge> edges;
			const PadXRActions::ActionState a = VR::GetPadXRActionState();
			if (open && a.generation != 0 && a.active && !a.stale)
			{
				UiPanel::NavInput in;
				in.select = a.hands[0].menu_select || a.hands[1].menu_select;
				in.back = a.hands[0].menu_back || a.hands[1].menu_back;
				const int hand = (std::fabs(a.hands[1].navigate_x) + std::fabs(a.hands[1].navigate_y) >
									 std::fabs(a.hands[0].navigate_x) + std::fabs(a.hands[0].navigate_y)) ? 1 : 0;
				in.x = a.hands[hand].navigate_x;
				in.y = a.hands[hand].navigate_y;
				UiPanel::NavTick(s.ui_nav, in, edges);
			}
			else
			{
				UiPanel::NavRelease(s.ui_nav, edges);
			}
			static constexpr GenericInputBinding kKey[] = {GenericInputBinding::DPadUp, GenericInputBinding::DPadDown,
				GenericInputBinding::DPadLeft, GenericInputBinding::DPadRight, GenericInputBinding::Cross,
				GenericInputBinding::Circle};
			static_assert(std::size(kKey) == static_cast<size_t>(UiPanel::NavKey::Count));
			for (const UiPanel::NavEdge& e : edges)
				ImGuiManager::ProcessXrNavEvent(kKey[static_cast<size_t>(e.key)], e.down);
		}

		UiPanel::LayerRequest UiPanelTick(const UiPanel::Head& head, XrTime now, SwapchainWait::Budget& budget)
		{
			static const UiPanel::Settings kSettings{};
			const bool want_open = s.ui_imgui ? FullscreenUI::HasActiveWindow() : (VMManager::GetState() == VMState::Paused);
			const float dt = (s.ui_last_time != 0 && now > s.ui_last_time) ?
								 std::min(static_cast<float>(now - s.ui_last_time) * 1.0e-9f, 0.25f) :
								 0.0f;
			s.ui_last_time = now;
			const UiPanel::Event ev = UiPanel::Update(s.ui_state, want_open, head, dt, kSettings);
			if (ev == UiPanel::Event::Opened || ev == UiPanel::Event::Replaced)
			{
				const UiPanel::Pose& p = s.ui_state.panel.pose;
				Console.WriteLn("(VR) UiPanel: %s at (%.2f, %.2f, %.2f) yaw %.1f deg.",
					ev == UiPanel::Event::Opened ? "open" : "re-placed (come-back or recentre)", p.x, p.y, p.z,
					s.ui_state.panel.yaw_rad * (180.0f / 3.14159265f));
				if (ev == UiPanel::Event::Opened && !s.ui_imgui)
					UiPanel::PostOsdMessage("UiPanelTest", 3.0f);
			}
			else if (ev == UiPanel::Event::Closed)
			{
				Console.WriteLn("(VR) UiPanel: close.");
			}
			const bool strip = !s.ui_imgui && UiPanel::StripVisible(true, UiPanel::OsdLiveNow()) && (s.view_space != XR_NULL_HANDLE);
			const bool panel = UiPanel::Visible(s.ui_state);
			if (ev == UiPanel::Event::Closed && s.ui_imgui)
				s.ui.ever_released = false;
			s.ui_wants_pixels = s.ui_imgui && s.ui_state.open;
			UiPanel::LayerRequest rq;
			if ((panel || strip) && EnsureUiPanel(budget))
			{
				rq.panel = panel;
				rq.strip = strip;
			}
			s.ui_source = nullptr;
			return rq;
		}

		u32 BuildUiLayers(XrCompositionLayerQuad* ui_quads, const XrCompositionLayerBaseHeader** layers, u32& layer_count,
			u32 layer_capacity, const UiPanel::LayerPlan& plan)
		{
			u32 n = 0;
			const auto add = [&](const UiPanel::Panel& p, XrSpace space, const UiPanel::Rect& r) {
				if (layer_count >= layer_capacity || n >= UiPanel::kUiLayers)
					return;
				XrCompositionLayerQuad& q = ui_quads[n];
				q.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				q.next = nullptr;
				q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
							   (s.ui.premultiplied ? 0 : XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT);
				q.space = space;
				q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				q.subImage = {s.ui.swapchain, {{r.x, r.y}, {r.w, r.h}}, 0};
				q.pose.orientation = {p.pose.qx, p.pose.qy, p.pose.qz, p.pose.qw};
				q.pose.position = {p.pose.x, p.pose.y, p.pose.z};
				q.size = {p.width_m, p.height_m};
				layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
				n++;
			};
			if (!s.ui.ever_released)
				return 0;
			if (plan.panel && UiPanel::Visible(s.ui_state))
				add(s.ui_state.panel, XRSession::GetSpace(), UiPanel::PanelImageRect());
			if (plan.strip && s.view_space != XR_NULL_HANDLE)
				add(UiPanel::PlaceStrip(UiPanel::Settings{}), s.view_space, UiPanel::StripImageRect());
			return n;
		}

		float ComputeAspect(u32 w, u32 h, bool follow_patch_aspect)
		{
			return ScreenAspect::Compute(GSConfig.AspectRatio, w, h, EmuConfig.CurrentCustomAspectRatio, follow_patch_aspect);
		}

		void PacerThreadMain()
		{
			Threading::SetNameOfCurrentThread("VR Pacer");

			const XrSession session = XRSession::GetSession();
			const u64 my_gen = s.pacer_generation.load(std::memory_order_acquire);
			const auto current = [&]() { return s.pacer_generation.load(std::memory_order_acquire) == my_gen; };

			while (!s.pacer_exit.load(std::memory_order_acquire) && current())
			{
				if (!XRSession::IsSessionRunning())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					continue;
				}

				if (s.pacer_lost.load(std::memory_order_acquire))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}

				XrFrameState fs = {XR_TYPE_FRAME_STATE};
				const XrResult res = xrWaitFrame(session, nullptr, &fs);
				if (!current())
					break;

				if (XR_FAILED(res))
				{
					if (res == XR_ERROR_SESSION_NOT_RUNNING)
						continue;

					if (res == XR_ERROR_INSTANCE_LOST || res == XR_ERROR_SESSION_LOST ||
						res == XR_ERROR_RUNTIME_FAILURE || res == XR_ERROR_HANDLE_INVALID)
					{
						Console.Error("(VR) xrWaitFrame failed terminally (%d) — flagging runtime loss "
									  "(no event will arrive for this).",
							static_cast<int>(res));
						s.pacer_lost.store(true, std::memory_order_release);
						continue;
					}

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
				if (!current())
					break;
				s.pending_frame_state = fs;
				s.has_pending_frame = true;
			}

			if (current())
				s.pacer_done.store(true, std::memory_order_release);
		}

		void RecordFrameCall(XrResult res, const char* call)
		{
			if (!s.frame_streak.Record(res) || s.frame_lost.load(std::memory_order_relaxed))
				return;
			Console.Error("(VR) %s failed (%d)%s: treating the session as lost.", call, static_cast<int>(res),
				XRFrameHealth::IsTerminal(res) ? "" : " for too many frames in a row");
			s.frame_lost.store(true, std::memory_order_release);
		}

		bool SettleOwedBegin(XrSession session)
		{
			if (!s.begin_owed)
				return true;

			XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
			const XrResult res = xrBeginFrame(session, &bi);
			RecordFrameCall(res, "xrBeginFrame (owed)");
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
		s.screen_anchor_pitch = 0.0f;
		s_reanchor_requested.store(false, std::memory_order_release);
		s.display_vfov_deg = 0.0f;
		s.display_hfov_deg = 0.0f;
		s.display_hfov_union_deg = 0.0f;
		s.display_fov_attempts = 0;
		for (auto& eye : s.wp_eye_offset)
			eye[0] = eye[1] = eye[2] = 0.0f;
		s.wp_eye_cant_deg = 0.0f;
		s.wp_eye_frames = 0;
		s.wp_st_eyes = 1;
		s.wp_st_shift = s.wp_st_ipd_mm = s.wp_st_dphys = 0.0f;
		for (float& f : s.wp_st_fov)
			f = 0.0f;
		s.wp_views_logged = s.wp_declared_logged = false;
		s.fit_logged = DisplayFit::Fit{};
		DisplayFit::Reset();
		{
			std::lock_guard<std::mutex> lock(s_screen_mutex);
			s_anchor_snapshot.x = s_anchor_snapshot.y = s_anchor_snapshot.z = s_anchor_snapshot.yaw = 0.0f;
			s_anchor_snapshot.generation++;
		}
		s.warned_lever = false;
		s.lever_layers_logged = -1;
		s.lever_screen_logged = 0;
		s.mode_logged = false;
		s.mode_last = ScreenCurve::Mode::Flat;
		s.facets_logged = false;
		s.projector_failed = false;
		s.proj_frame_w = 0;
		s.proj_frame_h = 0;

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

		std::string offered;
		{
			char name[32];
			for (int64_t f : formats)
			{
				offered += XRSwapchainFormat::FormatName(f, name, sizeof(name));
				offered += ' ';
			}
		}
		const XRSwapchainFormat::Choice pick = XRSwapchainFormat::Choose(formats.data(), formats.size(), false
		);
		if (pick.rung == XRSwapchainFormat::Rung::Unsupported)
		{
			Console.Error("(VR) No offered swapchain format is reachable by a built conversion rung. Offered: %s. "
						  "Reachable: VK_FORMAT_R8G8B8A8_{SRGB,UNORM} (RawCopy), VK_FORMAT_B8G8R8A8_UNORM (BlitSwizzle). %s"
						  "Running flat.",
				offered.c_str(),
				pick.shader_encode_would_help ? "VK_FORMAT_B8G8R8A8_SRGB is offered but needs the ShaderEncode rung, "
												 "which SP0 reserves and does not build. " :
												 "");
			return false;
		}
		s.swapchain_format = static_cast<VkFormat>(pick.format);
		s.swapchain_rung = pick.rung;
		s.swapchain_srgb = pick.srgb;
		{
			char name[32];
			Console.WriteLn("(VR) XR swapchain format: %s via %s (%s; offered: %s).",
				SwapchainFormatName(name, sizeof(name)), XRSwapchainFormat::RungName(pick.rung),
				pick.srgb ? "sRGB, gamma correct" : "UNORM, brightness may be off", offered.c_str());
		}
		if (!pick.srgb)
		{
			char name[32];
			Console.Warning("(VR) Runtime offers no sRGB swapchain we can reach; using %s. "
							"Brightness/gamma may be off on this runtime.%s",
				SwapchainFormatName(name, sizeof(name)),
				pick.shader_encode_would_help ? " (VK_FORMAT_B8G8R8A8_SRGB is offered; the reserved ShaderEncode "
												 "rung would make it gamma-correct.)" :
												 "");
		}
		if (pick.rung == XRSwapchainFormat::Rung::BlitSwizzle)
			Console.WriteLn("(VR) Channel order differs from the GS target: the frame reaches the swapchain by "
							"vkCmdBlitImage (red/blue swapped in flight); the lever cards are swizzled on the CPU.");

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
		s.source_width = 0;
		s.source_height = 0;
		s.clamp_rt = nullptr;
		s.warned_clamp = false;
		s.clamp_warned_width = 0;
		s.clamp_warned_height = 0;
		s.clamp_failed_width = 0;
		s.clamp_failed_height = 0;
		s.next_cmd_buffer = 0;
		s.begin_owed = false;
		s.begin_owed_display_time = 0;

		s.warned_pacer = false;
		s.warned_beginframe = false;
		s.warned_endframe = false;
		s.warned_acquire = false;
		s.warned_wait_timeout = false;
		s.warned_wait_fail = false;
		s.warned_release = false;
		s.warned_lever_release = false;
		s.warned_fence_timeout = false;
		s.warned_swapchain = false;

		s.pacer_generation.fetch_add(1, std::memory_order_acq_rel);
		s.pacer_exit.store(false, std::memory_order_release);
		s.pacer_done.store(false, std::memory_order_release);
		s.pacer_lost.store(false, std::memory_order_release);
		s.frame_lost.store(false, std::memory_order_release);
		s.frame_streak = {};
		s.paced_frames_consumed.store(0, std::memory_order_release);
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

		s.lag_follower = LagMatch::Follower{};
		s.lag_logged_on = false;
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
		struct UiSourceDrop
		{
			~UiSourceDrop() { s.ui_source = nullptr; }
		} ui_source_drop;

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
		s.paced_frames_consumed.fetch_add(1, std::memory_order_relaxed);

		if (!XRSession::IsSessionRunning())
			return;

		const XrSession session = XRSession::GetSession();

		XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
		XrResult res;
		{
			res = xrBeginFrame(session, &bi);
		}
		RecordFrameCall(res, "xrBeginFrame");
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
		UiPanelInput();

		UiPanel::Head ui_head;
		if (s.view_space != XR_NULL_HANDLE)
		{
			XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(xrLocateSpace(s.view_space, XRSession::GetSpace(), fs.predictedDisplayTime, &loc)) &&
				(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			{
				ui_head.pose = {loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z,
					loc.pose.orientation.w, 0.0f, 0.0f, 0.0f};
				if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				{
					ui_head.pose.x = loc.pose.position.x;
					ui_head.pose.y = loc.pose.position.y;
					ui_head.pose.z = loc.pose.position.z;
				}
				ui_head.valid = true;
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
				s.lag_follower.OnHead(fs.predictedDisplayTime,
					LagMatch::Quat{loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w});
			}
			else
			{
				HeadPose::Invalidate();
				s.lag_follower.OnHeadLost();
			}
		}

		if (XRSession::ConsumeLocalSpaceChange(fs.predictedDisplayTime))
		{
			Console.WriteLn("(VR) Runtime recenter took effect: head-look re-baselined.");
			CameraDriver::RequestRecenter();
		}

		if (s.display_vfov_deg <= 0.0f && s.view_space != XR_NULL_HANDLE && s.display_fov_attempts < 120)
		{
			s.display_fov_attempts++;
			XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
			vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			vli.displayTime = fs.predictedDisplayTime;
			vli.space = s.view_space;
			XrViewState vs = {XR_TYPE_VIEW_STATE};
			XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
			u32 count = 0;
			if (XR_SUCCEEDED(xrLocateViews(XRSession::GetSession(), &vli, &vs, 2, &count, views)) && count == 2)
			{
				float v = 0.0f, h = 0.0f, left = 0.0f, right = 0.0f;
				for (const XrView& view : views)
				{
					v = std::max(v, (view.fov.angleUp - view.fov.angleDown) * (180.0f / 3.14159265358979323846f));
					h = std::max(h, (view.fov.angleRight - view.fov.angleLeft) * (180.0f / 3.14159265358979323846f));
					left = std::min(left, view.fov.angleLeft);
					right = std::max(right, view.fov.angleRight);
				}
				if (v > 0.0f && std::isfinite(v))
				{
					s.display_vfov_deg = v;
					s.display_hfov_deg = h;
					s.display_hfov_union_deg = (right - left) * (180.0f / 3.14159265358979323846f);
					Console.WriteLn("(VR) Display: %.1f x %.1f deg per eye (%s).", h, v,
						v < DisplayFit::kTightVerticalFovDeg ? "TIGHT: the screen and head angles are fitted to it" :
															  "headset: no display fit");
				}
			}
			if (s.display_vfov_deg <= 0.0f && s.display_fov_attempts == 120)
				Console.Warning("(VR) Display: the runtime gave no field of view in 120 frames; no display fit.");
		}

		if (VR::WorldProjection::GsActive() && s.view_space != XR_NULL_HANDLE && (s.wp_eye_frames++ % 120) == 0)
		{
			XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
			vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			vli.displayTime = fs.predictedDisplayTime;
			vli.space = s.view_space;
			XrViewState vs = {XR_TYPE_VIEW_STATE};
			XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
			u32 count = 0;
			if (XR_SUCCEEDED(xrLocateViews(XRSession::GetSession(), &vli, &vs, 2, &count, views)) && count == 2 &&
				(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
			{
				constexpr float kDeg = 180.0f / 3.14159265358979323846f;
				float rot[2] = {};
				for (u32 e = 0; e < 2; e++)
				{
					s.wp_eye_offset[e][0] = views[e].pose.position.x;
					s.wp_eye_offset[e][1] = views[e].pose.position.y;
					s.wp_eye_offset[e][2] = views[e].pose.position.z;
					const XrFovf& f = views[e].fov;
					s.wp_eye_fov[e][0] = f.angleLeft * kDeg;
					s.wp_eye_fov[e][1] = f.angleRight * kDeg;
					s.wp_eye_fov[e][2] = f.angleUp * kDeg;
					s.wp_eye_fov[e][3] = f.angleDown * kDeg;
					const XrQuaternionf& q = views[e].pose.orientation;
					rot[e] = VR::WorldProjection::QuatAngleDeg(VR::WorldProjection::Quat{}, VR::WorldProjection::Quat{q.x, q.y, q.z, q.w});
				}
				const XrQuaternionf& a = views[0].pose.orientation;
				const XrQuaternionf& b = views[1].pose.orientation;
				s.wp_eye_cant_deg = VR::WorldProjection::QuatAngleDeg(
					VR::WorldProjection::Quat{a.x, a.y, a.z, a.w}, VR::WorldProjection::Quat{b.x, b.y, b.z, b.w});
				if (!s.wp_views_logged)
				{
					s.wp_views_logged = true;
					Console.WriteLn("(VR) wp views (E2): L pos=(%.4f,%.4f,%.4f) rot=%.3f fov=[%.2f,%.2f,%.2f,%.2f] "
									"R pos=(%.4f,%.4f,%.4f) rot=%.3f fov=[%.2f,%.2f,%.2f,%.2f] cant=%.3f deg",
						s.wp_eye_offset[0][0], s.wp_eye_offset[0][1], s.wp_eye_offset[0][2], rot[0], s.wp_eye_fov[0][0],
						s.wp_eye_fov[0][1], s.wp_eye_fov[0][2], s.wp_eye_fov[0][3], s.wp_eye_offset[1][0],
						s.wp_eye_offset[1][1], s.wp_eye_offset[1][2], rot[1], s.wp_eye_fov[1][0], s.wp_eye_fov[1][1],
						s.wp_eye_fov[1][2], s.wp_eye_fov[1][3], s.wp_eye_cant_deg);
				}
			}
		}

		if (s_reanchor_requested.load(std::memory_order_acquire) && s.view_space != XR_NULL_HANDLE)
		{
			XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
			const bool located = XR_SUCCEEDED(xrLocateSpace(s.view_space, XRSession::GetSpace(), fs.predictedDisplayTime, &loc));
			HeadPose::Snapshot head;
			head.orientation_x = loc.pose.orientation.x;
			head.orientation_y = loc.pose.orientation.y;
			head.orientation_z = loc.pose.orientation.z;
			head.orientation_w = loc.pose.orientation.w;
			head.position_x = loc.pose.position.x;
			head.position_y = loc.pose.position.y;
			head.position_z = loc.pose.position.z;
			head.valid = located && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
			bool position_valid = located && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
			ReanchorHead(&head, &position_valid);
			if (head.valid)
			{
				const float qx = head.orientation_x, qy = head.orientation_y;
				const float qz = head.orientation_z, qw = head.orientation_w;
				const float fx = -2.0f * (qw * qy + qz * qx);
				const float fz = -(1.0f - 2.0f * (qx * qx + qy * qy));
				if ((fx * fx + fz * fz) > 1.0e-4f)
					s.screen_anchor_yaw = std::atan2(-fx, -fz);
				s.screen_anchor_pitch = DisplayFit::RecenterPitch(DisplayFit::ForwardY(qx, qy, qz, qw), DisplayFit::Get());
				if (position_valid)
				{
					s.screen_anchor_x = head.position_x;
					s.screen_anchor_y = head.position_y;
					s.screen_anchor_z = head.position_z;
				}
				s_reanchor_requested.store(false, std::memory_order_release);
				Console.WriteLn("(VR) Screen re-anchored: pos (%.2f, %.2f, %.2f) yaw %.1f deg pitch %.1f deg.",
					s.screen_anchor_x, s.screen_anchor_y, s.screen_anchor_z,
					s.screen_anchor_yaw * (180.0f / 3.14159265f), s.screen_anchor_pitch * (180.0f / 3.14159265f));
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

		ScreenParams sp;
		{
			std::lock_guard<std::mutex> lock(s_screen_mutex);
			sp = s_screen_params;
		}
		const ScreenCurve::Decision decision =
			ScreenCurve::Decide(sp.arc_deg, XRSession::HasCylinderLayer(), sp.curve_fallback, sp.force_fallback);
		ScreenCurve::Mode mode = decision.mode;
		if (mode == ScreenCurve::Mode::Projector && ScreenProjector::Failed())
		{
			ScreenProjector::Shutdown();
			s.projector_failed = true;
			Console.Warning("(VR) Curved screen: the projector gave up (no swapchain); falling to the flat quad for this session.");
		}
		if (mode == ScreenCurve::Mode::Projector && s.projector_failed)
			mode = ScreenCurve::Mode::Flat;
		if (mode == ScreenCurve::Mode::Projector && !ScreenProjector::IsInitialized())
		{
			DestroySwapchains();
			if (!ScreenProjector::Initialize(s.swapchain_format, s.swapchain_srgb, sp.projector_supersample, sp.projector_depth))
			{
				s.projector_failed = true;
				mode = ScreenCurve::Mode::Flat;
				Console.Warning("(VR) Curved screen: the projector could not start; falling to the flat quad for this session.");
			}
		}
		else if (mode != ScreenCurve::Mode::Projector && ScreenProjector::IsInitialized())
		{
			ScreenProjector::Shutdown();
		}
		if (!s.mode_logged || mode != s.mode_last)
		{
			s.mode_logged = true;
			s.mode_last = mode;
			const XRSession::RuntimeLimits& lim = XRSession::GetRuntimeLimits();
			Console.WriteLn("(VR) Curved screen: %s (arc %.0f deg, cylinder layer %s, curveFallback=%s, forceFallback=%s, "
							"fovMutable=%s, maxLayerCount=%u).",
				ScreenCurve::ModeName(mode), sp.arc_deg, XRSession::HasCylinderLayer() ? "advertised" : "not advertised",
				ScreenCurve::FallbackName(sp.curve_fallback), ScreenCurve::ForceName(sp.force_fallback),
				lim.fov_mutable_known ? (lim.fov_mutable ? "true" : "false") : "unknown", lim.max_layer_count);
			if (decision.facets_unavailable && !s.facets_logged)
			{
				s.facets_logged = true;
				Console.Warning("(VR) Curved screen: facets are not available in this core; taking the next rung (%s).",
					ScreenCurve::ModeName(mode));
			}
		}
		const bool projector = (mode == ScreenCurve::Mode::Projector);
		const bool curved = (mode == ScreenCurve::Mode::NativeCylinder);
		const SplitState::Snapshot split_snap = SplitState::Get();

		const bool layered = current && (static_cast<GSTextureVK*>(current)->GetArrayLayers() >= 2);
		const bool stereo = layered || (eye != MonoEye);
		bool force_zero_layers = false;
		SwapchainWait::Budget wait_budget(SwapchainWait::kPresentWaitBudgetNs);
		int cl_copy[2] = {-1, -1};
		if (projector)
		{
			if (fs.shouldRender && current)
			{
				GSTextureVK* src = static_cast<GSTextureVK*>(current);
				const u32 w = static_cast<u32>(src->GetWidth());
				const u32 h = static_cast<u32>(src->GetHeight());
				if (w > 0 && h > 0)
				{
					s.proj_frame_w = w;
					s.proj_frame_h = h;
					const auto publish = [&](u32 index, u32 layer, s32 x, s32 y, s32 cw, s32 ch) {
						ScreenProjector::Source ps;
						ps.texture = src;
						ps.layer = layer;
						ps.x = x;
						ps.y = y;
						ps.w = cw;
						ps.h = ch;
						ScreenProjector::Publish(ps, index);
					};
					if (split_snap.split_active)
					{
						for (int vp = 0; vp < 2; vp++)
						{
							if (split_snap.mode == 2 && vp != split_snap.local_view)
								continue;
							s32 rx = static_cast<s32>(split_snap.rect_x[vp] * static_cast<s32>(w));
							s32 ry = static_cast<s32>(split_snap.rect_y[vp] * static_cast<s32>(h));
							s32 rw = static_cast<s32>(split_snap.rect_w[vp] * static_cast<s32>(w));
							s32 rh = static_cast<s32>(split_snap.rect_h[vp] * static_cast<s32>(h));
							rx += 1; ry += 1; rw -= 2; rh -= 2;
							if (rw < 2 || rh < 2)
								continue;
							if (layered)
							{
								publish(static_cast<u32>(2 * vp), 0, rx, ry, rw, rh);
								publish(static_cast<u32>(2 * vp + 1), 1, rx, ry, rw, rh);
							}
							else
							{
								publish(static_cast<u32>(2 * vp), 0, rx, ry, rw, rh);
							}
						}
					}
					else if (layered)
					{
						publish(0, 0, 0, 0, static_cast<s32>(w), static_cast<s32>(h));
						publish(1, 1, 0, 0, static_cast<s32>(w), static_cast<s32>(h));
					}
					else
					{
						publish(0, 0, 0, 0, static_cast<s32>(w), static_cast<s32>(h));
					}
					ScreenProjector::FlushPublish();
				}
			}
		}
		else if (fs.shouldRender && current)
		{
			GSTextureVK* src = static_cast<GSTextureVK*>(current);
			const u32 w = static_cast<u32>(src->GetWidth());
			const u32 h = static_cast<u32>(src->GetHeight());
			if (w > 0 && h > 0 && EnsureSwapchains(w, h, stereo))
			{
				if (layered)
				{
					const EyeWait waits[2] = {AcquireAndWait(s.chains[0], wait_budget), AcquireAndWait(s.chains[1], wait_budget)};
					const SwapchainWait::PairPlan plan =
						SwapchainWait::PlanEyePair(waits[0] == EyeWait::Ready, waits[1] == EyeWait::Ready);
					for (u32 l = 0; l < 2; l++)
					{
						CopyResult r;
						if (plan.copy[l] && waits[l] == EyeWait::Ready)
							r = CopyAndRelease(src, l, s.chains[l]);
						else
							r = (waits[l] == EyeWait::Skipped) ? CopyResult::Skipped : CopyResult::TimeoutZeroLayer;
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
					const CopyResult r = CopyToSwapchain(src, 0, chain, wait_budget);
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
		constexpr u32 kBaseCapacity = 5 + ControlQuads::kSlots + UiPanel::kUiLayers;
		static_assert(kBaseCapacity == 9, "the UI panel's budget assumes 5 screen + 2 lever + 2 UI slots");
		constexpr u32 kPlanCapacity = VrUiPanel::LayerCapacity(5, ControlQuads::kSlots, UiPanel::kUiLayers);
		constexpr u32 kLayerCapacity =
			NearField::LayerCapacity(5, ControlQuads::kSlots, UiPanel::kUiLayers + VrUiPanel::kMaxLayers);
		static_assert(kLayerCapacity <= XR_MIN_COMPOSITION_LAYERS_SUPPORTED,
			"a spec runtime guarantees XR_MIN_COMPOSITION_LAYERS_SUPPORTED (16) and no more");
		XrCompositionLayerQuad vrui_quads[VrUiPanel::kMaxLayers] = {};
		XrCompositionLayerProjection nf_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		XrCompositionLayerProjectionView nf_views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		XrCompositionLayerQuad ui_quads[UiPanel::kUiLayers] = {
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		XrCompositionLayerQuad lever_quads[ControlQuads::kSlots] = {
			{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
		const XrCompositionLayerBaseHeader* layers[kLayerCapacity] = {};
		u32 layer_count = 0;
		XrCompositionLayerProjection wp_layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		XrCompositionLayerProjectionView wp_views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		bool cl_split = false;
		ScreenProjector::Cylinder proj_cyls[5];
		u32 proj_count = 0;
		bool wp_layer_declared = false;
		if (!force_zero_layers)
		{
			const float distance = sp.distance;
			const float voffset = sp.voffset;
			const float aspect = ComputeAspect(projector ? s.proj_frame_w : s.swapchain_width,
				projector ? s.proj_frame_h : s.swapchain_height, sp.follow_patch_aspect);
			const bool curved_geometry = curved || projector;
			DisplayFit::Screen ref_screen;
			ref_screen.curved = curved_geometry;
			ref_screen.arc_deg = sp.arc_deg;
			ref_screen.height_m = sp.height;
			ref_screen.distance_m = distance;
			ref_screen.aspect = aspect;
			const DisplayFit::Fit fit = DisplayFit::Compute(s.display_vfov_deg, ref_screen, sp.fit_fill);
			const DisplayFit::Screen fitted = DisplayFit::Apply(ref_screen, fit);
			const float height = fitted.height_m;
			const float arc_deg = fitted.arc_deg;
			DisplayFit::Publish(fit);
			if (fit.yaw_scale != s.fit_logged.yaw_scale || fit.pitch_scale != s.fit_logged.pitch_scale)
			{
				if (fit.IsIdentity())
					Console.WriteLn("(VR) Display fit: none (the screen fits this display as set).");
				else
					Console.WriteLn("(VR) Display fit: screen %.1f x %.1f deg -> %.1f x %.1f deg (%.0f%% of a %.1f deg tall "
									"display); head angles x%.2f yaw, x%.2f pitch into the profile.",
						DisplayFit::HorizontalDeg(ref_screen), DisplayFit::VerticalDeg(ref_screen),
						DisplayFit::HorizontalDeg(fitted), DisplayFit::VerticalDeg(fitted), sp.fit_fill * 100.0f,
						s.display_vfov_deg, 1.0f / fit.yaw_scale, 1.0f / fit.pitch_scale);
				s.fit_logged = fit;
			}
			const auto to_projector = [&](const XrCompositionLayerCylinderKHR& cyl, XrEyeVisibility vis, int source) {
				if (proj_count >= 5)
					return;
				ScreenProjector::Cylinder& pc = proj_cyls[proj_count++];
				pc.pose = cyl.pose;
				pc.radius = cyl.radius;
				pc.central_angle = cyl.centralAngle;
				pc.aspect_ratio = cyl.aspectRatio;
				pc.eye_mask = (vis == XR_EYE_VISIBILITY_LEFT) ? 1u : (vis == XR_EYE_VISIBILITY_RIGHT) ? 2u : 3u;
				pc.source = source;
			};

			const float ayaw_sin = std::sin(s.screen_anchor_yaw);
			const float ayaw_cos = std::cos(s.screen_anchor_yaw);
			const XrQuaternionf anchor_quat = {
				0.0f, std::sin(s.screen_anchor_yaw * 0.5f), 0.0f, std::cos(s.screen_anchor_yaw * 0.5f)};

			const bool follow = sp.follow_head && (s.view_space != XR_NULL_HANDLE);
			const XrSpace layer_space = follow ? s.view_space : XRSession::GetSpace();
			s.lag_follower.Configure(follow ? sp.lag_ns : 0, sp.lag_axes);
			const std::optional<LagMatch::Quat> lag_q =
				follow ? s.lag_follower.LayerOrientation(fs.predictedDisplayTime) : std::nullopt;
			if (s.lag_follower.On() != s.lag_logged_on)
			{
				s.lag_logged_on = s.lag_follower.On();
				if (s.lag_logged_on)
					Console.WriteLn("(VR) Follow-head screen: held at the head pose %.0f ms back, per game frame, on axes%s%s%s.",
						static_cast<double>(s.lag_follower.LagNs()) / 1.0e6, (sp.lag_axes & LagMatch::AxisYaw) ? " yaw" : "",
						(sp.lag_axes & LagMatch::AxisPitch) ? " pitch" : "", (sp.lag_axes & LagMatch::AxisRoll) ? " roll" : "");
				else
					Console.WriteLn("(VR) Follow-head screen: lag matching off (head-locked).");
			}
			const XrQuaternionf follow_quat = lag_q.has_value() ? XrQuaternionf{lag_q->x, lag_q->y, lag_q->z, lag_q->w} :
			                                                     XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};

			const auto make_layer = [&](u32 chain_idx, XrEyeVisibility vis) {
				const XrSwapchainSubImage sub_image = {
					s.chains[chain_idx].swapchain,
					{{0, 0}, {static_cast<s32>(s.swapchain_width), static_cast<s32>(s.swapchain_height)}},
					0};
				if (curved_geometry)
				{
					XrCompositionLayerCylinderKHR& cyl = cyls[projector ? proj_count : layer_count];
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
						if (s.screen_anchor_pitch != 0.0f)
						{
							const DisplayFit::Placement pl = DisplayFit::PlaceAhead(s.screen_anchor_yaw, s.screen_anchor_pitch, distance);
							cyl.pose.orientation = {pl.qx, pl.qy, pl.qz, pl.qw};
						}
					}
					cyl.radius = distance;
					cyl.centralAngle = arc_deg * (3.14159265f / 180.0f);
					cyl.aspectRatio = aspect;
					if (projector)
					{
						to_projector(cyl, vis, static_cast<int>(chain_idx));
						return;
					}
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
						float fx = 0.0f, fy = 0.0f, fz = -distance;
						if (lag_q.has_value())
							LagMatch::Rotate(lag_q.value(), fx, fy, fz);
						quad.pose.position = {fx, voffset + fy, fz};
					}
					else
					{
						quad.pose.orientation = anchor_quat;
						quad.pose.position = {s.screen_anchor_x - distance * ayaw_sin,
							s.screen_anchor_y + voffset,
							s.screen_anchor_z - distance * ayaw_cos};
						if (s.screen_anchor_pitch != 0.0f)
						{
							const DisplayFit::Placement pl = DisplayFit::PlaceAhead(s.screen_anchor_yaw, s.screen_anchor_pitch, distance);
							quad.pose.orientation = {pl.qx, pl.qy, pl.qz, pl.qw};
							quad.pose.position = {s.screen_anchor_x + pl.dx, s.screen_anchor_y + voffset + pl.dy, s.screen_anchor_z + pl.dz};
						}
					}
					quad.size = {height * aspect, height};
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				}
				layer_count++;
			};

			cl_split = split_snap.split_active;

			const bool wp_mode = VR::WorldProjection::GsActive();
			VR::WorldProjection::Presented wp_present;
			bool wp_declare = false;
			int wp_eyes = 1;
			if (wp_mode)
			{
				wp_present = VR::WorldProjection::LastPresent();
				const u32 wci = s.chains[0].ever_released ? 0u : 1u;
				VR::WorldProjection::LayerInputs wpin;
				wpin.present_declares = wp_present.declares;
				wpin.split = cl_split;
				wpin.projector = projector;
				wpin.chain_released = s.chains[wci].ever_released;
				wpin.copied_this_visit = cl_copy[wci] == static_cast<int>(CopyResult::Copied);
				wpin.display_vfov_deg = s.display_vfov_deg;
				wpin.fit = fit;
				wpin.far_shift_ndc = wp_present.far_shift_ndc;
				wpin.layered = layered;
				wpin.right_copied_this_visit = cl_copy[1] == static_cast<int>(CopyResult::Copied);
				wp_eyes = VR::WorldProjection::LayerEyes(wpin);
				wp_declare = VR::WorldProjection::LayerDeclaresStereo(wpin);
			}
			wp_layer_declared = wp_declare;

			s_presented_hfov_deg.store(
				cl_split ? std::numeric_limits<float>::quiet_NaN() :
				(wp_mode && s.display_hfov_union_deg > 0.0f) ? s.display_hfov_union_deg :
															   DisplayFit::HorizontalDeg(ref_screen),
				std::memory_order_relaxed);
			const auto make_split_layer = [&](u32 chain_idx, XrEyeVisibility vis, int viewport) {
				const s32 full_w = static_cast<s32>(projector ? s.proj_frame_w : s.swapchain_width);
				const s32 full_h = static_cast<s32>(projector ? s.proj_frame_h : s.swapchain_height);
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
				const float local_half_deg = curved_geometry ? (0.5f * arc_deg) :
					(std::atan((0.5f * mirror_width) / distance) * (180.0f / 3.14159265f));
				const float side_half_deg = curved_geometry ? (0.5f * arc_deg * shape_scale) :
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
				if (curved_geometry)
				{
					XrCompositionLayerCylinderKHR& cyl = cyls[projector ? proj_count : layer_count];
					cyl.layerFlags = 0;
					cyl.space = layer_space;
					cyl.eyeVisibility = vis;
					cyl.subImage = sub_image;
					cyl.pose.orientation = vp_quat;
					cyl.pose.position = follow ?
						XrVector3f{0.0f, voffset, 0.0f} :
						XrVector3f{s.screen_anchor_x, s.screen_anchor_y + voffset, s.screen_anchor_z};
					if (!follow && s.screen_anchor_pitch != 0.0f)
					{
						const DisplayFit::Placement pl = DisplayFit::PlaceAhead(yaw, s.screen_anchor_pitch, distance);
						cyl.pose.orientation = {pl.qx, pl.qy, pl.qz, pl.qw};
					}
					cyl.radius = distance;
					cyl.centralAngle = arc_deg * (3.14159265f / 180.0f) * shape_scale;
					cyl.aspectRatio = rect_aspect;
					if (projector)
					{
						to_projector(cyl, vis, 2 * viewport + static_cast<int>(chain_idx));
						return;
					}
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
					if (!follow && s.screen_anchor_pitch != 0.0f)
					{
						const DisplayFit::Placement pl = DisplayFit::PlaceAhead(yaw, s.screen_anchor_pitch, distance);
						quad.pose.orientation = {pl.qx, pl.qy, pl.qz, pl.qw};
						quad.pose.position = {s.screen_anchor_x + pl.dx, s.screen_anchor_y + voffset + pl.dy, s.screen_anchor_z + pl.dz};
					}
					quad.size = {vp_width, vp_phys_height};
					layers[layer_count] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
				}
				layer_count++;
			};

			const bool l0 = projector ? (ScreenProjector::HasSource(0) || ScreenProjector::HasSource(2)) : s.chains[0].ever_released;
			const bool l1 = projector ? (ScreenProjector::HasSource(1) || ScreenProjector::HasSource(3)) : s.chains[1].ever_released;
			if (wp_declare)
			{
				const u32 ci = s.chains[0].ever_released ? 0u : 1u;
				const VR::WorldProjection::Pose& wpp = wp_present.decision.stamp.pose;
				VR::WorldProjection::EyeView eyes[2];
				const bool per_eye = (wp_eyes == 2) && VR::WorldProjection::DeclareEyes(wpp.view, wpp.position,
														   wp_present.far_shift_ndc, s.wp_eye_offset, eyes);
				for (u32 v = 0; v < 2; v++)
				{
					XrCompositionLayerProjectionView& pv = wp_views[v];
					const VR::WorldProjection::DeclaredView& dv = per_eye ? eyes[v].view : wpp.view;
					const float* pos = per_eye ? eyes[v].position : wpp.position;
					const u32 chain = per_eye ? VR::WorldProjection::EyeChain(v) : ci;
					pv.pose.orientation = {dv.orientation.x, dv.orientation.y, dv.orientation.z, dv.orientation.w};
					pv.pose.position = {pos[0], pos[1], pos[2]};
					pv.fov = {std::atan(dv.tan_left), std::atan(dv.tan_right), std::atan(dv.tan_up), std::atan(dv.tan_down)};
					pv.subImage = {s.chains[chain].swapchain,
						{{0, 0}, {static_cast<s32>(s.swapchain_width), static_cast<s32>(s.swapchain_height)}}, 0};
					s.wp_st_fov[v * 2 + 0] = pv.fov.angleLeft * (180.0f / 3.14159265358979323846f);
					s.wp_st_fov[v * 2 + 1] = pv.fov.angleRight * (180.0f / 3.14159265358979323846f);
				}
				const float half = 0.5f * (wpp.view.tan_right - wpp.view.tan_left);
				const float dx = s.wp_eye_offset[1][0] - s.wp_eye_offset[0][0];
				const float dy = s.wp_eye_offset[1][1] - s.wp_eye_offset[0][1];
				const float dz = s.wp_eye_offset[1][2] - s.wp_eye_offset[0][2];
				const float ipd_m = std::sqrt(dx * dx + dy * dy + dz * dz);
				s.wp_st_eyes = per_eye ? 2 : 1;
				s.wp_st_shift = per_eye ? wp_present.far_shift_ndc * half : 0.0f;
				s.wp_st_ipd_mm = ipd_m * 1000.0f;
				s.wp_st_dphys = per_eye ? VR::WorldProjection::ConvergenceDistanceM(ipd_m, wp_present.far_shift_ndc, half) : 0.0f;
				if (per_eye)
					s.wp_pairs++;
				if (per_eye && !s.wp_declared_logged)
				{
					s.wp_declared_logged = true;
					constexpr float kDeg = 180.0f / 3.14159265358979323846f;
					Console.WriteLn("(VR) wp views (E2): declared fovL=[%.2f,%.2f,%.2f,%.2f] vs located [%.2f,%.2f,%.2f,%.2f]; "
									"declared fovR=[%.2f,%.2f,%.2f,%.2f] vs located [%.2f,%.2f,%.2f,%.2f]; the runtime returns no "
									"composited image: whether it kept the declared fov is judged by eye (E1).",
						wp_views[0].fov.angleLeft * kDeg, wp_views[0].fov.angleRight * kDeg, wp_views[0].fov.angleUp * kDeg,
						wp_views[0].fov.angleDown * kDeg, s.wp_eye_fov[0][0], s.wp_eye_fov[0][1], s.wp_eye_fov[0][2],
						s.wp_eye_fov[0][3], wp_views[1].fov.angleLeft * kDeg, wp_views[1].fov.angleRight * kDeg,
						wp_views[1].fov.angleUp * kDeg, wp_views[1].fov.angleDown * kDeg, s.wp_eye_fov[1][0],
						s.wp_eye_fov[1][1], s.wp_eye_fov[1][2], s.wp_eye_fov[1][3]);
				}
				wp_layer.layerFlags = 0;
				wp_layer.space = XRSession::GetSpace();
				wp_layer.viewCount = 2;
				wp_layer.views = wp_views;
				layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&wp_layer);
			}
			else if (split_snap.split_active && (l0 || l1))
			{
				if (VR::SeatCastArmed() && current)
				{
					const int ovp = 1 - split_snap.local_view;
					GSTexture* src = current;
					const s32 full_w = static_cast<s32>(src->GetWidth());
					const s32 full_h = static_cast<s32>(src->GetHeight());
					const GSVector4i crop(
						static_cast<int>(split_snap.rect_x[ovp] * full_w),
						static_cast<int>(split_snap.rect_y[ovp] * full_h),
						static_cast<int>((split_snap.rect_x[ovp] + split_snap.rect_w[ovp]) * full_w),
						static_cast<int>((split_snap.rect_y[ovp] + split_snap.rect_h[ovp]) * full_h));
					SeatCastReadback(src, crop, true);
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

			if (projector && !wp_declare)
			{
				if (ScreenProjector::Present(layer_space, fs.predictedDisplayTime, proj_cyls, proj_count, s.proj_frame) &&
					layer_count < kPlanCapacity)
				{
					layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&s.proj_frame.layer);
				}
			}
		}

		if (VR::WorldProjection::GsActive())
		{
			const VR::WorldProjection::Presented wpl = VR::WorldProjection::LastPresent();
			const bool declared = wp_layer_declared;
			(declared ? s.wp_declared : s.wp_flat)++;
			if (declared)
				s.wp_head.Add(wpl.decision.stamp.pose);
			if (s.wp_declared + s.wp_flat >= 300)
			{
				Console.WriteLn("(VR) world projection: %u of the last %u presents declared (projection layer), %u on "
								"the screen; last: src=%d age=%llu declares=%d. head check: %u of %u declared within "
								"%.2f deg, worst %.4f deg.",
					s.wp_declared, s.wp_declared + s.wp_flat, s.wp_flat, static_cast<int>(wpl.decision.source),
					static_cast<unsigned long long>(wpl.decision.age), wpl.declares ? 1 : 0, s.wp_head.within,
					s.wp_head.checked, VR::WorldProjection::kHeadCheckToleranceDeg, s.wp_head.worst);
				Console.WriteLn("(VR) wp stereo: eyes=%d shift=%.5f fovL=[%.2f,%.2f] fovR=[%.2f,%.2f] ipd=%.1f dphys=%.3f "
								"pairs=%u of %u cant=%.3f",
					s.wp_st_eyes, s.wp_st_shift, s.wp_st_fov[0], s.wp_st_fov[1], s.wp_st_fov[2], s.wp_st_fov[3],
					s.wp_st_ipd_mm, s.wp_st_dphys, s.wp_pairs, s.wp_declared, s.wp_eye_cant_deg);
				s.wp_declared = s.wp_flat = 0;
				s.wp_pairs = 0;
				s.wp_head.Reset();
			}
		}


		bool ui_test_switch = false;
		s.ui_imgui = !ui_test_switch;
		const bool ui_on = !force_zero_layers;
		u32 lever_capacity = kPlanCapacity;
		UiPanel::LayerPlan ui_plan;
		u32 ui_capacity = kPlanCapacity;
		if (ui_on)
		{
			UiPanel::LayerRequest rq = UiPanelTick(ui_head, fs.predictedDisplayTime, wait_budget);
			rq.screens = layer_count;
			for (int i = 0; i < ControlQuads::kSlots; i++)
				rq.levers += ControlQuads::Get(i).active ? 1u : 0u;
			rq.member_panels = VrUiPanel::Runtime::Wanted();
			ui_capacity = UiPanel::EffectiveCapacity(kPlanCapacity, XRSession::GetRuntimeLimits().max_layer_count);
			ui_plan = UiPanel::PlanLayers(rq, ui_capacity);
			lever_capacity = std::min(ui_capacity, layer_count + ui_plan.levers);
		}
		if (!force_zero_layers)
			BuildLeverLayers(lever_quads, layers, layer_count, lever_capacity, cl_split, wait_budget);
		u32 vrui_n = 0;
		if (ui_on && ui_plan.member_panels > 0)
		{
			VrUiPanel::Runtime::Frame vf;
			vf.swapchain_format = static_cast<int64_t>(s.swapchain_format);
			vf.swap_red_blue = (s.swapchain_rung == XRSwapchainFormat::Rung::BlitSwizzle);
			vf.anchor = {s.screen_anchor_x, s.screen_anchor_y, s.screen_anchor_z, s.screen_anchor_yaw};
			vf.session = session;
			vf.space = XRSession::GetSpace();
			vrui_n = VrUiPanel::Runtime::Build(vf, wait_budget, vrui_quads, layers, layer_count,
				std::min(ui_capacity, layer_count + ui_plan.member_panels));
		}
		if (ui_on)
		{
			const u32 ui_n = BuildUiLayers(ui_quads, layers, layer_count, ui_capacity, ui_plan);
			const int panel_on = (ui_plan.panel && UiPanel::Visible(s.ui_state) && ui_n > 0) ? 1 : 0;
			const int strip_on = (static_cast<int>(ui_n) > panel_on) ? 1 : 0;
			if (static_cast<int>(layer_count) != s.ui_layers_logged || panel_on != s.ui_panel_logged ||
				strip_on != s.ui_strip_logged)
			{
				s.ui_layers_logged = static_cast<int>(layer_count);
				s.ui_panel_logged = panel_on;
				s.ui_strip_logged = strip_on;
				Console.WriteLn("(VR) UiPanel: layers=%u panel=%d strip=%d capacity=%u (runtime max %u).", layer_count,
					panel_on, strip_on, ui_capacity, XRSession::GetRuntimeLimits().max_layer_count);
			}
		}
		(void)vrui_n;

		if (!force_zero_layers)
			BuildNearFieldLayer(&nf_layer, nf_views, layers, layer_count, kLayerCapacity, fs.predictedDisplayTime, wait_budget);

		XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
		ei.displayTime = fs.predictedDisplayTime;
		ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		ei.layerCount = layer_count;
		ei.layers = layer_count ? layers : nullptr;

		res = xrEndFrame(session, &ei);
		RecordFrameCall(res, "xrEndFrame");
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

		bool pacer_abandoned = false;
		if (s.pacer.joinable())
		{
			if (s.pacer_done.load(std::memory_order_acquire))
			{
				s.pacer.join();
			}
			else
			{
				Console.Error("(VR) Pacer is stuck inside the dead runtime — detaching it and parking the "
							  "XR handles (bounded leak; VR goes flat, the next VR start creates a fresh instance).");
				s.pacer.detach();
				pacer_abandoned = true;
			}
		}

		HeadPose::Invalidate();
		if (s.view_space != XR_NULL_HANDLE)
		{
			if (!pacer_abandoned)
				xrDestroySpace(s.view_space);
			s.view_space = XR_NULL_HANDLE;
		}

		const Internal::VulkanHandles& h = Internal::GetVulkanHandles();
		const bool vk_alive = h.IsValid();

		ScreenProjector::Shutdown();

		if (vk_alive)
			WaitAllFences();

		DestroyLeverResources();
		DestroyUiPanel(!pacer_abandoned);
		VrUiPanel::Runtime::Shutdown(!pacer_abandoned);
		DestroyNearField(!pacer_abandoned);

		for (auto& chain : s.chains)
		{
			if (chain.swapchain != XR_NULL_HANDLE)
			{
				if (!pacer_abandoned)
					xrDestroySwapchain(chain.swapchain);
				chain.swapchain = XR_NULL_HANDLE;
			}
			chain.images.clear();
			chain.ever_released = false;
		}
		s.swapchain_width = 0;
		s.swapchain_height = 0;
		s.source_width = 0;
		s.source_height = 0;
		if (vk_alive)
			delete s.clamp_rt;
		s.clamp_rt = nullptr;
		if (g_gs_device)
		{
			delete s.cast_rt;
			s.cast_dl.reset();
		}
		else
		{
			(void)s.cast_dl.release();
		}
		s.cast_rt = nullptr;

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

		if (pacer_abandoned)
			XRSession::ParkHandlesForAbandonedPacer();

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
		bool follow_head, float fit_fill)
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		s_screen_params.fit_fill = fit_fill;
		s_screen_params.distance = distance_m;
		s_screen_params.height = height_m;
		s_screen_params.arc_deg = arc_deg;
		s_screen_params.voffset = vertical_offset_m;
		s_screen_params.follow_head = follow_head;
	}

	void UpdateScreenLag(s64 lag_ns, u8 axes)
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		s_screen_params.lag_ns = lag_ns;
		s_screen_params.lag_axes = axes;
	}

	void NoteGameVsync(bool new_frame)
	{
#ifdef ENABLE_VULKAN
		if (s.initialized)
			s.lag_follower.OnGameVsync(new_frame);
#else
		(void)new_frame;
#endif
	}

	void UpdateScreenAspectRule(bool follow_patch_aspect)
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		s_screen_params.follow_patch_aspect = follow_patch_aspect;
	}

	void UpdateScreenCurve(ScreenCurve::Fallback fallback, ScreenCurve::Force force, float projector_supersample,
		bool projector_depth)
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		s_screen_params.curve_fallback = fallback;
		s_screen_params.force_fallback = force;
		s_screen_params.projector_supersample = projector_supersample;
		s_screen_params.projector_depth = projector_depth;
	}

	void RequestScreenReanchor()
	{
		s_reanchor_requested.store(true, std::memory_order_release);
	}

	bool UiPanelWantsPixels()
	{
		return s.ui_wants_pixels && XRSession::HasSession();
	}

	void SetUiPanelSource(GSTexture* panel)
	{
		s.ui_source = panel;
	}

	void RequestUiRecentre()
	{
		s_ui_recentre_requested.store(true, std::memory_order_release);
	}

	bool IsReanchorRequested()
	{
		return s_reanchor_requested.load(std::memory_order_acquire);
	}

	void ClearReanchorRequestForTest()
	{
		s_reanchor_requested.store(false, std::memory_order_release);
	}

	void ReanchorHead(HeadPose::Snapshot* head, bool* position_valid)
	{
		(void)head;
		(void)position_valid;
	}

	ScreenAnchor GetScreenAnchor()
	{
		std::lock_guard<std::mutex> lock(s_screen_mutex);
		return s_anchor_snapshot;
	}

	std::optional<float> PresentedScreenHfovDeg()
	{
		const float v = s_presented_hfov_deg.load(std::memory_order_relaxed);
		return std::isfinite(v) ? std::optional<float>(v) : std::nullopt;
	}

	bool RuntimeLossDetected()
	{
#ifdef ENABLE_VULKAN
		return s.pacer_lost.load(std::memory_order_acquire) || s.frame_lost.load(std::memory_order_acquire);
#else
		return false;
#endif
	}

	u64 GetPacedFramesConsumed()
	{
#ifdef ENABLE_VULKAN
		return s.paced_frames_consumed.load(std::memory_order_relaxed);
#else
		return 0;
#endif
	}
}
