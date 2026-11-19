// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/SeatSession.h"
#include "VR/SeatCast.h"
#include "VR/VRManager.h"
#include "VR/XRSwapchainFormat.h"
#include "VR/SwapchainWait.h"
#include "VR/XRFrameHealth.h"

#include "common/Console.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <fstream>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace VR::SeatSession
{
	namespace
	{
		struct XrNegotiateLoaderInfo_
		{
			u32 structType;
			u32 structVersion;
			size_t structSize;
			u32 minInterfaceVersion;
			u32 maxInterfaceVersion;
			XrVersion minApiVersion;
			XrVersion maxApiVersion;
		};
		struct XrNegotiateRuntimeRequest_
		{
			u32 structType;
			u32 structVersion;
			size_t structSize;
			u32 runtimeInterfaceVersion;
			XrVersion runtimeApiVersion;
			PFN_xrGetInstanceProcAddr getInstanceProcAddr;
		};
		using PFN_negotiate = XrResult(XRAPI_PTR*)(const XrNegotiateLoaderInfo_*, XrNegotiateRuntimeRequest_*);

		struct XrFns
		{
			void* lib = nullptr;
			PFN_xrGetInstanceProcAddr gipa = nullptr;
#define XFN(name) PFN_xr##name name = nullptr
			XFN(CreateInstance);
			XFN(EnumerateInstanceExtensionProperties);
			XFN(DestroyInstance);
			XFN(GetSystem);
			XFN(CreateSession);
			XFN(DestroySession);
			XFN(CreateReferenceSpace);
			XFN(DestroySpace);
			XFN(PollEvent);
			XFN(BeginSession);
			XFN(EndSession);
			XFN(WaitFrame);
			XFN(BeginFrame);
			XFN(EndFrame);
			XFN(CreateSwapchain);
			XFN(DestroySwapchain);
			XFN(EnumerateSwapchainFormats);
			XFN(EnumerateSwapchainImages);
			XFN(AcquireSwapchainImage);
			XFN(WaitSwapchainImage);
			XFN(ReleaseSwapchainImage);
#undef XFN

			bool Negotiate(const std::string& manifest)
			{
				const std::string libpath = RuntimeLibFromManifest(manifest);
				if (libpath.empty())
				{
					Console.Error("(VR) SeatSession: no library_path in runtime manifest '%s'", manifest.c_str());
					return false;
				}
				lib = dlopen(libpath.c_str(), RTLD_NOW | RTLD_LOCAL);
				if (!lib)
				{
					Console.Error("(VR) SeatSession: dlopen('%s') failed: %s", libpath.c_str(), dlerror());
					return false;
				}
				auto negotiate = reinterpret_cast<PFN_negotiate>(dlsym(lib, "xrNegotiateLoaderRuntimeInterface"));
				if (!negotiate)
				{
					Console.Error("(VR) SeatSession: runtime lacks xrNegotiateLoaderRuntimeInterface");
					return false;
				}
				XrNegotiateLoaderInfo_ li = {};
				li.structType = 1;
				li.structVersion = 1;
				li.structSize = sizeof(li);
				li.minInterfaceVersion = 1;
				li.maxInterfaceVersion = 1;
				li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
				li.maxApiVersion = XR_MAKE_VERSION(1, 0, 0x3ffu);
				XrNegotiateRuntimeRequest_ rr = {};
				rr.structType = 3;
				rr.structVersion = 1;
				rr.structSize = sizeof(rr);
				if (XR_FAILED(negotiate(&li, &rr)) || !rr.getInstanceProcAddr)
				{
					Console.Error("(VR) SeatSession: runtime negotiation failed");
					return false;
				}
				gipa = rr.getInstanceProcAddr;
#define LOADN(name) 	if (XR_FAILED(gipa(XR_NULL_HANDLE, "xr" #name, reinterpret_cast<PFN_xrVoidFunction*>(&name))) || !(name)) 	{ 		name = nullptr; 	}
				LOADN(CreateInstance);
				LOADN(EnumerateInstanceExtensionProperties);
#undef LOADN
				return CreateInstance != nullptr;
			}

			bool LoadForInstance(XrInstance inst)
			{
#define LOADN(name) 	if (XR_FAILED(gipa(inst, "xr" #name, reinterpret_cast<PFN_xrVoidFunction*>(&name))) || !(name)) 		return false
				LOADN(DestroyInstance);
				LOADN(GetSystem);
				LOADN(CreateSession);
				LOADN(DestroySession);
				LOADN(CreateReferenceSpace);
				LOADN(DestroySpace);
				LOADN(PollEvent);
				LOADN(BeginSession);
				LOADN(EndSession);
				LOADN(WaitFrame);
				LOADN(BeginFrame);
				LOADN(EndFrame);
				LOADN(CreateSwapchain);
				LOADN(DestroySwapchain);
				LOADN(EnumerateSwapchainFormats);
				LOADN(EnumerateSwapchainImages);
				LOADN(AcquireSwapchainImage);
				LOADN(WaitSwapchainImage);
				LOADN(ReleaseSwapchainImage);
#undef LOADN
				return true;
			}

			void Unload()
			{
				if (lib)
					dlclose(lib);
				*this = {};
			}
		};

		std::thread s_thread;
		std::atomic<bool> s_run{false};
		std::atomic<bool> s_running{false};
		std::atomic<bool> s_exited{true};
		bool s_abandoned = false;
		std::atomic<int> s_state{static_cast<int>(State::Idle)};
		void SetState(State s) { s_state.store(static_cast<int>(s), std::memory_order_release); }
		constexpr XrDuration kSeatImageWaitNs = 100'000'000;

		struct VkFns
		{
			PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
#define FN(name) PFN_vk##name name = nullptr
			FN(GetPhysicalDeviceQueueFamilyProperties);
			FN(GetPhysicalDeviceMemoryProperties);
			FN(GetDeviceQueue);
			FN(CreateCommandPool);
			FN(DestroyCommandPool);
			FN(AllocateCommandBuffers);
			FN(CreateFence);
			FN(DestroyFence);
			FN(ResetFences);
			FN(WaitForFences);
			FN(CreateBuffer);
			FN(DestroyBuffer);
			FN(GetBufferMemoryRequirements);
			FN(AllocateMemory);
			FN(FreeMemory);
			FN(BindBufferMemory);
			FN(MapMemory);
			FN(BeginCommandBuffer);
			FN(EndCommandBuffer);
			FN(CmdPipelineBarrier);
			FN(CmdCopyBufferToImage);
			FN(QueueSubmit);
			FN(DeviceWaitIdle);
			FN(DestroyDevice);
			FN(DestroyInstance);
#undef FN

			void* lib = nullptr;

			bool LoadBase()
			{
				lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
				if (!lib)
					lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
				if (!lib)
					return false;
				GetInstanceProcAddr =
					reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
				return GetInstanceProcAddr != nullptr;
			}

			bool LoadForInstance(VkInstance inst)
			{
#define LOADI(name) \
	name = reinterpret_cast<PFN_vk##name>(GetInstanceProcAddr(inst, "vk" #name)); \
	if (!(name)) \
		return false
				LOADI(GetPhysicalDeviceQueueFamilyProperties);
				LOADI(GetPhysicalDeviceMemoryProperties);
				LOADI(GetDeviceQueue);
				LOADI(CreateCommandPool);
				LOADI(DestroyCommandPool);
				LOADI(AllocateCommandBuffers);
				LOADI(CreateFence);
				LOADI(DestroyFence);
				LOADI(ResetFences);
				LOADI(WaitForFences);
				LOADI(CreateBuffer);
				LOADI(DestroyBuffer);
				LOADI(GetBufferMemoryRequirements);
				LOADI(AllocateMemory);
				LOADI(FreeMemory);
				LOADI(BindBufferMemory);
				LOADI(MapMemory);
				LOADI(BeginCommandBuffer);
				LOADI(EndCommandBuffer);
				LOADI(CmdPipelineBarrier);
				LOADI(CmdCopyBufferToImage);
				LOADI(QueueSubmit);
				LOADI(DeviceWaitIdle);
				LOADI(DestroyDevice);
				LOADI(DestroyInstance);
#undef LOADI
				return true;
			}

			void Unload()
			{
				if (lib)
					dlclose(lib);
				*this = {};
			}
		};

		struct Stack
		{
			VkFns fn;
			XrFns xr;
			XrInstance instance = XR_NULL_HANDLE;
			XrSystemId system = XR_NULL_SYSTEM_ID;
			XrSession session = XR_NULL_HANDLE;
			XrSpace space = XR_NULL_HANDLE;
			XrSwapchain swapchain = XR_NULL_HANDLE;
			u32 chain_w = 0, chain_h = 0;
			u32 chain_generation = 0;
			int64_t format = 0;
			bool swap_rb = false;
			std::vector<XrSwapchainImageVulkanKHR> images;

			VkInstance vk_instance = VK_NULL_HANDLE;
			VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
			VkDevice vk_device = VK_NULL_HANDLE;
			VkQueue vk_queue = VK_NULL_HANDLE;
			u32 vk_queue_family = 0;
			VkCommandPool vk_pool = VK_NULL_HANDLE;
			VkCommandBuffer vk_cmd = VK_NULL_HANDLE;
			VkFence vk_fence = VK_NULL_HANDLE;
			VkBuffer staging = VK_NULL_HANDLE;
			VkDeviceMemory staging_mem = VK_NULL_HANDLE;
			void* staging_map = nullptr;
			size_t staging_size = 0;

			bool session_running = false;
			std::string dir;
			std::string sock;
			const char* fail_reason = nullptr;
		};

		bool XrOk(XrResult r, const char* what)
		{
			if (XR_SUCCEEDED(r))
				return true;
			Console.Error("(VR) SeatSession: %s failed (%d)", what, static_cast<int>(r));
			return false;
		}

		bool VkOk(VkResult r, const char* what)
		{
			if (r == VK_SUCCESS)
				return true;
			Console.Error("(VR) SeatSession: %s failed (%d)", what, static_cast<int>(r));
			return false;
		}

		class ScopedSeatDir
		{
		public:
			explicit ScopedSeatDir(const std::string& dir)
			{
				if (dir.empty())
					return;
				const char* old = std::getenv("XDG_RUNTIME_DIR");
				m_had = (old != nullptr);
				if (old)
					m_saved = old;
				setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
				m_swapped = true;
			}
			~ScopedSeatDir()
			{
				if (!m_swapped)
					return;
				if (m_had)
					setenv("XDG_RUNTIME_DIR", m_saved.c_str(), 1);
				else
					unsetenv("XDG_RUNTIME_DIR");
			}

		private:
			std::string m_saved;
			bool m_had = false;
			bool m_swapped = false;
		};

		std::string FindRuntimeSocket(const std::string& dir)
		{
			if (dir.empty())
				return {};
			for (const char* rel : {"/wivrn/comp_ipc", "/monado_comp_ipc", "/penguinscreen2-monado/comp_ipc"})
			{
				const std::string p = dir + rel;
				struct stat sb;
				if (stat(p.c_str(), &sb) == 0 && S_ISSOCK(sb.st_mode))
					return p;
			}
			return {};
		}

		bool SocketPresent(const std::string& path)
		{
			struct stat sb;
			return !path.empty() && stat(path.c_str(), &sb) == 0;
		}

		bool CreateStack(Stack& st, int seat)
		{
			const std::string dir = VR::ResolveSeatRuntimeDir(seat);
			st.dir = dir;
			if (seat > 1 && dir.empty())
			{
				Console.Error("(VR) SeatSession: seat %d has no runtime dir (--vr-seat-dirs or [VR] XrSeatRuntimeDirs) — not starting.", seat);
				return false;
			}
			if (!st.fn.LoadBase())
			{
				Console.Error("(VR) SeatSession: could not dlopen libvulkan.");
				return false;
			}
			const std::string manifest = VR::ResolveSeatRuntimeJson(seat);
			if (manifest.empty())
			{
				Console.Error("(VR) SeatSession: no usable runtime manifest for seat %d: not starting (a named one that is missing is reported above; with none named, set XR_RUNTIME_JSON or [VR] XrSeatRuntimeJsons).", seat);
				return false;
			}
			if (!st.xr.Negotiate(manifest))
				return false;

			{
				ScopedSeatDir bind(dir);
				XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
				std::strncpy(ici.applicationInfo.applicationName, "PenguinScreen2-Seat", XR_MAX_APPLICATION_NAME_SIZE - 1);
				std::strncpy(ici.applicationInfo.engineName, "PenguinScreen2", XR_MAX_ENGINE_NAME_SIZE - 1);
				ici.applicationInfo.applicationVersion = 1;
				ici.applicationInfo.engineVersion = 1;
				ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
				std::vector<XrExtensionProperties> props;
				if (st.xr.EnumerateInstanceExtensionProperties)
				{
					u32 n = 0;
					if (XR_SUCCEEDED(st.xr.EnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)) && n > 0)
					{
						props.assign(n, {XR_TYPE_EXTENSION_PROPERTIES});
						if (XR_SUCCEEDED(st.xr.EnumerateInstanceExtensionProperties(nullptr, n, &n, props.data())))
							props.resize(n);
						else
							props.clear();
					}
				}
				std::vector<const char*> offered;
				for (const XrExtensionProperties& ep : props)
					offered.push_back(ep.extensionName);
				const char* binding = ChooseVulkanBinding(offered.data(), offered.size());
				if (!binding)
				{
					Console.Error("(VR) SeatSession: seat %d's runtime does not offer XR_KHR_vulkan_enable2 (%zu "
								  "extension(s) offered); the seat has no other Vulkan binding — not starting.",
						seat, offered.size());
					return false;
				}
				Console.WriteLn("(VR) SeatSession: seat %d Vulkan binding: %s.", seat, binding);
				const char* exts[1] = {binding};
				ici.enabledExtensionCount = 1;
				ici.enabledExtensionNames = exts;
				if (!XrOk(st.xr.CreateInstance(&ici, &st.instance), "xrCreateInstance(seat)"))
					return false;
				st.sock = FindRuntimeSocket(dir);
				Console.WriteLn("(VR) SeatSession: seat %d instance up (runtime dir %s; socket %s)", seat,
					dir.empty() ? "<default>" : dir.c_str(), st.sock.empty() ? "<none found>" : st.sock.c_str());

			}
			if (!st.xr.LoadForInstance(st.instance))
			{
				Console.Error("(VR) SeatSession: XR function table incomplete.");
				return false;
			}
			{
				XrSystemGetInfo sgi2 = {XR_TYPE_SYSTEM_GET_INFO};
				sgi2.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
				XrResult sr = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
				SetState(State::Waiting);
				while (s_run.load(std::memory_order_acquire))
				{
					sr = st.xr.GetSystem(st.instance, &sgi2, &st.system);
					if (XR_SUCCEEDED(sr))
						break;
					Observation o;
					o.get_system = sr;
					o.socket_seen = !st.sock.empty();
					o.socket_now = SocketPresent(st.sock);
					if ((st.fail_reason = RuntimeFailure(o)) != nullptr)
						return false;
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
				if (!XR_SUCCEEDED(sr))
					return false;
			}

			PFN_xrGetVulkanGraphicsRequirements2KHR pGetReq = nullptr;
			PFN_xrCreateVulkanInstanceKHR pCreateInst = nullptr;
			PFN_xrGetVulkanGraphicsDevice2KHR pGetDev = nullptr;
			PFN_xrCreateVulkanDeviceKHR pCreateDev = nullptr;
#define LOADX(fnname, var) \
	if (!XrOk(st.xr.gipa(st.instance, fnname, reinterpret_cast<PFN_xrVoidFunction*>(&var)), fnname)) \
		return false
			LOADX("xrGetVulkanGraphicsRequirements2KHR", pGetReq);
			LOADX("xrCreateVulkanInstanceKHR", pCreateInst);
			LOADX("xrGetVulkanGraphicsDevice2KHR", pGetDev);
			LOADX("xrCreateVulkanDeviceKHR", pCreateDev);
#undef LOADX

			XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
			if (!XrOk(pGetReq(st.instance, st.system, &req), "xrGetVulkanGraphicsRequirements2KHR"))
				return false;

			VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
			ai.pApplicationName = "PenguinScreen2-Seat";
			ai.apiVersion = VK_API_VERSION_1_1;
			VkInstanceCreateInfo vici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
			vici.pApplicationInfo = &ai;
			XrVulkanInstanceCreateInfoKHR xici = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
			xici.systemId = st.system;
			xici.pfnGetInstanceProcAddr = st.fn.GetInstanceProcAddr;
			xici.vulkanCreateInfo = &vici;
			VkResult vkr = VK_SUCCESS;
			if (!XrOk(pCreateInst(st.instance, &xici, &st.vk_instance, &vkr), "xrCreateVulkanInstanceKHR") ||
				!VkOk(vkr, "vkCreateInstance(seat)"))
				return false;
			if (!st.fn.LoadForInstance(st.vk_instance))
			{
				Console.Error("(VR) SeatSession: vulkan function table incomplete.");
				return false;
			}

			XrVulkanGraphicsDeviceGetInfoKHR gdi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
			gdi.systemId = st.system;
			gdi.vulkanInstance = st.vk_instance;
			if (!XrOk(pGetDev(st.instance, &gdi, &st.vk_phys), "xrGetVulkanGraphicsDevice2KHR"))
				return false;

			u32 qf_count = 0;
			st.fn.GetPhysicalDeviceQueueFamilyProperties(st.vk_phys, &qf_count, nullptr);
			std::vector<VkQueueFamilyProperties> qf(qf_count);
			st.fn.GetPhysicalDeviceQueueFamilyProperties(st.vk_phys, &qf_count, qf.data());
			st.vk_queue_family = 0;
			for (u32 i = 0; i < qf_count; i++)
			{
				if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				{
					st.vk_queue_family = i;
					break;
				}
			}
			const float prio = 1.0f;
			VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
			qci.queueFamilyIndex = st.vk_queue_family;
			qci.queueCount = 1;
			qci.pQueuePriorities = &prio;
			VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
			dci.queueCreateInfoCount = 1;
			dci.pQueueCreateInfos = &qci;
			XrVulkanDeviceCreateInfoKHR xdci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
			xdci.systemId = st.system;
			xdci.pfnGetInstanceProcAddr = st.fn.GetInstanceProcAddr;
			xdci.vulkanPhysicalDevice = st.vk_phys;
			xdci.vulkanCreateInfo = &dci;
			if (!XrOk(pCreateDev(st.instance, &xdci, &st.vk_device, &vkr), "xrCreateVulkanDeviceKHR") ||
				!VkOk(vkr, "vkCreateDevice(seat)"))
				return false;
			st.fn.GetDeviceQueue(st.vk_device, st.vk_queue_family, 0, &st.vk_queue);

			XrGraphicsBindingVulkanKHR gb = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
			gb.instance = st.vk_instance;
			gb.physicalDevice = st.vk_phys;
			gb.device = st.vk_device;
			gb.queueFamilyIndex = st.vk_queue_family;
			gb.queueIndex = 0;
			XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
			sci.next = &gb;
			sci.systemId = st.system;
			if (!XrOk(st.xr.CreateSession(st.instance, &sci, &st.session), "xrCreateSession(seat)"))
				return false;

			XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			rci.poseInReferenceSpace.orientation.w = 1.0f;
			if (!XrOk(st.xr.CreateReferenceSpace(st.session, &rci, &st.space), "xrCreateReferenceSpace(seat)"))
				return false;

			{
				u32 fn = 0;
				std::vector<int64_t> formats;
				if (XR_SUCCEEDED(st.xr.EnumerateSwapchainFormats(st.session, 0, &fn, nullptr)) && fn > 0)
				{
					formats.resize(fn);
					if (!XrOk(st.xr.EnumerateSwapchainFormats(st.session, fn, &fn, formats.data()),
							"xrEnumerateSwapchainFormats(seat)"))
						return false;
					formats.resize(fn);
				}
				std::string offered;
				char name[32];
				for (int64_t f : formats)
				{
					offered += XRSwapchainFormat::FormatName(f, name, sizeof(name));
					offered += ' ';
				}
				const XRSwapchainFormat::Choice pick = ChooseSwapchainFormat(formats.data(), formats.size(), false
				);
				if (pick.rung == XRSwapchainFormat::Rung::Unsupported)
				{
					Console.Error("(VR) SeatSession: seat %d: no offered swapchain format is reachable by a built "
								  "conversion rung. Offered: %s. Not starting.",
						seat, offered.c_str());
					return false;
				}
				st.format = pick.format;
				st.swap_rb = (pick.rung == XRSwapchainFormat::Rung::BlitSwizzle);
				Console.WriteLn("(VR) SeatSession: seat %d swapchain format: %s via %s%s (%s; offered: %s).", seat,
					XRSwapchainFormat::FormatName(pick.format, name, sizeof(name)), XRSwapchainFormat::RungName(pick.rung),
					st.swap_rb ? " (red/blue swapped on the CPU)" : "",
					pick.srgb ? "sRGB, gamma correct" : "UNORM, brightness may be off", offered.c_str());
			}

			VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			pci.queueFamilyIndex = st.vk_queue_family;
			if (!VkOk(st.fn.CreateCommandPool(st.vk_device, &pci, nullptr, &st.vk_pool), "vkCreateCommandPool"))
				return false;
			VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			cai.commandPool = st.vk_pool;
			cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cai.commandBufferCount = 1;
			if (!VkOk(st.fn.AllocateCommandBuffers(st.vk_device, &cai, &st.vk_cmd), "vkAllocateCommandBuffers"))
				return false;
			VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			if (!VkOk(st.fn.CreateFence(st.vk_device, &fci, nullptr, &st.vk_fence), "vkCreateFence"))
				return false;
			return true;
		}

		bool EnsureSwapchain(Stack& st, u32 w, u32 h)
		{
			if (st.swapchain != XR_NULL_HANDLE && st.chain_w == w && st.chain_h == h)
				return true;
			if (st.swapchain != XR_NULL_HANDLE)
			{
				st.xr.DestroySwapchain(st.swapchain);
				st.swapchain = XR_NULL_HANDLE;
				st.images.clear();
			}
			st.chain_generation++;
			XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
			ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			ci.format = st.format;
			ci.sampleCount = 1;
			ci.width = w;
			ci.height = h;
			ci.faceCount = 1;
			ci.arraySize = 1;
			ci.mipCount = 1;
			if (!XrOk(st.xr.CreateSwapchain(st.session, &ci, &st.swapchain), "xrCreateSwapchain(seat)"))
				return false;
			u32 n = 0;
			st.xr.EnumerateSwapchainImages(st.swapchain, 0, &n, nullptr);
			st.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
			if (!XrOk(st.xr.EnumerateSwapchainImages(st.swapchain, n, &n,
					reinterpret_cast<XrSwapchainImageBaseHeader*>(st.images.data())),
					"xrEnumerateSwapchainImages(seat)"))
				return false;
			st.chain_w = w;
			st.chain_h = h;
			Console.WriteLn("(VR) SeatSession: swapchain %ux%u (%zu images)", w, h, st.images.size());
			return true;
		}

		bool EnsureStaging(Stack& st, size_t bytes)
		{
			if (st.staging != VK_NULL_HANDLE && st.staging_size >= bytes)
				return true;
			if (st.staging != VK_NULL_HANDLE)
			{
				st.fn.DestroyBuffer(st.vk_device, st.staging, nullptr);
				st.fn.FreeMemory(st.vk_device, st.staging_mem, nullptr);
				st.staging = VK_NULL_HANDLE;
				st.staging_map = nullptr;
			}
			VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = bytes;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			if (!VkOk(st.fn.CreateBuffer(st.vk_device, &bci, nullptr, &st.staging), "vkCreateBuffer(staging)"))
				return false;
			VkMemoryRequirements mr;
			st.fn.GetBufferMemoryRequirements(st.vk_device, st.staging, &mr);
			VkPhysicalDeviceMemoryProperties mp;
			st.fn.GetPhysicalDeviceMemoryProperties(st.vk_phys, &mp);
			u32 type = UINT32_MAX;
			for (u32 i = 0; i < mp.memoryTypeCount; i++)
			{
				const VkMemoryPropertyFlags want =
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
				{
					type = i;
					break;
				}
			}
			if (type == UINT32_MAX)
				return false;
			VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.allocationSize = mr.size;
			mai.memoryTypeIndex = type;
			if (!VkOk(st.fn.AllocateMemory(st.vk_device, &mai, nullptr, &st.staging_mem), "vkAllocateMemory(staging)") ||
				!VkOk(st.fn.BindBufferMemory(st.vk_device, st.staging, st.staging_mem, 0), "vkBindBufferMemory") ||
				!VkOk(st.fn.MapMemory(st.vk_device, st.staging_mem, 0, VK_WHOLE_SIZE, 0, &st.staging_map), "vkMapMemory"))
				return false;
			st.staging_size = bytes;
			return true;
		}

		void DestroyStack(Stack& st)
		{
			if (st.vk_device != VK_NULL_HANDLE && st.fn.DeviceWaitIdle)
				st.fn.DeviceWaitIdle(st.vk_device);
			if (st.swapchain != XR_NULL_HANDLE && st.xr.DestroySwapchain)
				st.xr.DestroySwapchain(st.swapchain);
			if (st.space != XR_NULL_HANDLE && st.xr.DestroySpace)
				st.xr.DestroySpace(st.space);
			if (st.session != XR_NULL_HANDLE && st.xr.DestroySession)
				st.xr.DestroySession(st.session);
			if (st.staging != VK_NULL_HANDLE)
			{
				st.fn.DestroyBuffer(st.vk_device, st.staging, nullptr);
				st.fn.FreeMemory(st.vk_device, st.staging_mem, nullptr);
			}
			if (st.vk_fence != VK_NULL_HANDLE)
				st.fn.DestroyFence(st.vk_device, st.vk_fence, nullptr);
			if (st.vk_pool != VK_NULL_HANDLE)
				st.fn.DestroyCommandPool(st.vk_device, st.vk_pool, nullptr);
			if (st.vk_device != VK_NULL_HANDLE)
				st.fn.DestroyDevice(st.vk_device, nullptr);
			if (st.vk_instance != VK_NULL_HANDLE && st.fn.DestroyInstance)
				st.fn.DestroyInstance(st.vk_instance, nullptr);
			if (st.instance != XR_NULL_HANDLE && st.xr.DestroyInstance)
				st.xr.DestroyInstance(st.instance);
			st.fn.Unload();
			st.xr.Unload();
			st = {};
		}

		const char* PumpEvents(Stack& st)
		{
			XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
			XrResult pr;
			while ((pr = st.xr.PollEvent(st.instance, &ev)) == XR_SUCCESS)
			{
				Observation o;
				o.instance_loss = (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING);
				if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
					o.session_state = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev)->state;
				if (const char* why = RuntimeFailure(o))
					return why;
				if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				{
					const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
					if (sc->state == XR_SESSION_STATE_READY && !st.session_running)
					{
						XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
						bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						if (XrOk(st.xr.BeginSession(st.session, &bi), "xrBeginSession(seat)"))
						{
							st.session_running = true;
							Console.WriteLn("(VR) SeatSession: session RUNNING — seat screen live.");
						}
					}
					else if (sc->state == XR_SESSION_STATE_STOPPING && st.session_running)
					{
						st.xr.EndSession(st.session);
						st.session_running = false;
						Console.WriteLn("(VR) SeatSession: session stopped by runtime.");
					}
				}
				ev = {XR_TYPE_EVENT_DATA_BUFFER};
			}
			Observation o;
			o.poll = pr;
			return RuntimeFailure(o);
		}

		void Fail(int seat, const std::string& dir, const char* why)
		{
#ifdef VR_SEATFAIL_PLANTED
			SetState(State::Idle);
#else
			SetState(State::Failed);
#endif
			Console.Error("(VR) SeatSession: seat %d FAILED (%s); runtime dir %s. Not restarted. "
						  "The game and the primary headset continue.",
				seat, why, dir.empty() ? "<default>" : dir.c_str());
		}

		void ThreadBody(int seat);

		void ThreadMain(int seat)
		{
			try
			{
				ThreadBody(seat);
			}
			catch (const std::exception& e)
			{
				Console.Error("(VR) SeatSession: runtime exception — seat screen down (%s). "
							  "The game and the primary headset continue.", e.what());
				if (s_run.load(std::memory_order_acquire))
					Fail(seat, VR::ResolveSeatRuntimeDir(seat), "its runtime threw (the line above)");
			}
			catch (...)
			{
				Console.Error("(VR) SeatSession: unknown exception — seat screen down. "
							  "The game and the primary headset continue.");
				if (s_run.load(std::memory_order_acquire))
					Fail(seat, VR::ResolveSeatRuntimeDir(seat), "its runtime threw (the line above)");
			}
			s_running.store(false);
			s_exited.store(true, std::memory_order_release);
		}

		void ThreadBody(int seat)
		{
			Stack st;
			if (!CreateStack(st, seat))
			{
				const std::string dir = st.dir;
				const char* why = st.fail_reason;
				DestroyStack(st);
				s_running.store(false);
				if (s_run.load(std::memory_order_acquire))
					Fail(seat, dir, why ? why : "its runtime could not be set up; the error is above");
				else
					SetState(State::Idle);
				return;
			}
			s_running.store(true);
			const std::string seat_dir = st.dir;
			auto next_sock_check = std::chrono::steady_clock::now();
			const char* failed = nullptr;

			std::vector<u8> frame_buf(static_cast<size_t>(SeatCast::kMaxWidth) * 4 * 2048);
			u64 last_seq = 0;
			SeatCast::Frame fr = {};
			XRFrameHealth::Streak streak;
			SwapchainWait::FailureTracker wait_failures;
			bool wait_pending = false;
			u32 pending_index = 0;
			u32 pending_generation = 0;
			u32 released_generation = 0;
			bool lost = false;
			const auto loss = [&](XrResult res, const char* call) {
				Console.Error("(VR) SeatSession: %s failed (%d) — seat session lost; seat screen down. "
							  "The game and the primary headset continue.", call, static_cast<int>(res));
				lost = true;
				failed = "its frame calls kept failing: the session is lost";
			};

			while (s_run.load(std::memory_order_acquire))
			{
				if ((failed = PumpEvents(st)) != nullptr)
					break;
				if (!st.sock.empty() && std::chrono::steady_clock::now() >= next_sock_check)
				{
					next_sock_check = std::chrono::steady_clock::now() + std::chrono::seconds(1);
					Observation o;
					o.socket_seen = true;
					o.socket_now = SocketPresent(st.sock);
					if ((failed = RuntimeFailure(o)) != nullptr)
						break;
				}
				SetState(st.session_running ? State::Running : State::Waiting);
				if (!st.session_running)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					continue;
				}

				XrFrameState fs = {XR_TYPE_FRAME_STATE};
				XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
				const XrResult wr = st.xr.WaitFrame(st.session, &fwi, &fs);
				if (streak.Record(wr))
				{
					loss(wr, "xrWaitFrame");
					break;
				}
				if (!XR_SUCCEEDED(wr))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					continue;
				}
				XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
				XrResult br = st.xr.BeginFrame(st.session, &fbi);
				while (XR_FAILED(br) && br != XR_ERROR_SESSION_NOT_RUNNING && s_run.load(std::memory_order_acquire))
				{
					if (streak.Record(br))
					{
						loss(br, "xrBeginFrame");
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
					br = st.xr.BeginFrame(st.session, &fbi);
				}
				if (lost)
					break;
				streak.Record(br);
				if (!XR_SUCCEEDED(br))
					continue;

				const bool got = SeatCast::ConsumeLatest(last_seq, frame_buf.data(), frame_buf.size(), &fr);
				bool have_image = (st.swapchain != XR_NULL_HANDLE && released_generation == st.chain_generation);
				if (got)
				{
					last_seq = fr.seq;
					if (EnsureSwapchain(st, fr.width, fr.height) &&
						EnsureStaging(st, static_cast<size_t>(fr.stride) * fr.height))
					{
						if (!st.swap_rb)
						{
							std::memcpy(st.staging_map, fr.pixels, static_cast<size_t>(fr.stride) * fr.height);
						}
						else
						{
							const size_t words = static_cast<size_t>(fr.stride) / 4 * fr.height;
							const u32* in = reinterpret_cast<const u32*>(fr.pixels);
							u32* out = static_cast<u32*>(st.staging_map);
							for (size_t i = 0; i < words; i++)
								out[i] = XRSwapchainFormat::SwapRedBlue(in[i]);
						}
						if (wait_pending && pending_generation != st.chain_generation)
							wait_pending = false;
						if (!wait_pending)
						{
							u32 idx = 0;
							XrSwapchainImageAcquireInfo ac = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
							if (XR_SUCCEEDED(st.xr.AcquireSwapchainImage(st.swapchain, &ac, &idx)))
							{
								pending_index = idx;
								pending_generation = st.chain_generation;
								wait_pending = true;
							}
						}
						bool ready = false;
						if (wait_pending)
						{
							XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
							wi.timeout = kSeatImageWaitNs;
							const XrResult ir = st.xr.WaitSwapchainImage(st.swapchain, &wi);
							switch (SwapchainWait::OnWait(wait_failures, ir))
							{
								case SwapchainWait::Action::Use:
									wait_pending = false;
									ready = true;
									break;
								case SwapchainWait::Action::Reacquire:
									wait_pending = false;
									break;
								case SwapchainWait::Action::Lost:
									loss(ir, "xrWaitSwapchainImage");
									break;
								case SwapchainWait::Action::Hold:
								default:
									break;
							}
						}
						const u32 img_index = pending_index;
						if (ready)
						{
							VkCommandBufferBeginInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
							cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
							st.fn.BeginCommandBuffer(st.vk_cmd, &cb);
							VkImageMemoryBarrier to_dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
							to_dst.srcAccessMask = 0;
							to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
							to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
							to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
							to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
							to_dst.image = st.images[img_index].image;
							to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
							st.fn.CmdPipelineBarrier(st.vk_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
								VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);
							VkBufferImageCopy region = {};
							region.bufferRowLength = fr.stride / 4;
							region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
							region.imageExtent = {fr.width, fr.height, 1};
							st.fn.CmdCopyBufferToImage(st.vk_cmd, st.staging, st.images[img_index].image,
								VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
							VkImageMemoryBarrier to_ro = to_dst;
							to_ro.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
							to_ro.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
							to_ro.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
							to_ro.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
							st.fn.CmdPipelineBarrier(st.vk_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
								VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_ro);
							st.fn.EndCommandBuffer(st.vk_cmd);
							VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
							si.commandBufferCount = 1;
							si.pCommandBuffers = &st.vk_cmd;
							st.fn.ResetFences(st.vk_device, 1, &st.vk_fence);
							st.fn.QueueSubmit(st.vk_queue, 1, &si, st.vk_fence);
							st.fn.WaitForFences(st.vk_device, 1, &st.vk_fence, VK_TRUE, UINT64_MAX);
							XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
							if (XR_SUCCEEDED(st.xr.ReleaseSwapchainImage(st.swapchain, &rel)))
								released_generation = st.chain_generation;
							have_image = (released_generation == st.chain_generation);
						}
					}
				}

				XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
				const XrCompositionLayerBaseHeader* layers[1] = {};
				u32 layer_count = 0;
				if (have_image)
				{
					quad.layerFlags = 0;
					quad.space = st.space;
					quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
					quad.subImage = {st.swapchain,
						{{0, 0}, {static_cast<s32>(st.chain_w), static_cast<s32>(st.chain_h)}}, 0};
					quad.pose.orientation.w = 1.0f;
					quad.pose.position = {0.0f, 0.0f, -2.0f};
					const float h = 1.2f;
					const float aspect = (st.chain_h > 0) ?
						static_cast<float>(st.chain_w) / static_cast<float>(st.chain_h) : 1.0f;
					quad.size = {h * aspect, h};
					layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
					layer_count = 1;
				}
				XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
				fei.displayTime = fs.predictedDisplayTime;
				fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
				fei.layerCount = layer_count;
				fei.layers = layer_count ? layers : nullptr;
				const XrResult er = st.xr.EndFrame(st.session, &fei);
				if (lost || streak.Record(er))
				{
					if (!lost)
						loss(er, "xrEndFrame");
					break;
				}
			}

			DestroyStack(st);
			s_running.store(false);
			if (failed)
				Fail(seat, seat_dir, failed);
			else
				SetState(State::Idle);
			Console.WriteLn("(VR) SeatSession: thread exited.");
		}
	}

	const char* StateName(State s)
	{
		switch (s)
		{
			case State::Idle: return "IDLE";
			case State::Connecting: return "CONNECTING";
			case State::Waiting: return "WAITING";
			case State::Running: return "RUNNING";
			case State::Failed: return "FAILED";
		}
		return "?";
	}

	State GetState()
	{
		return static_cast<State>(s_state.load(std::memory_order_acquire));
	}

	const char* RuntimeFailure(const Observation& o)
	{
#ifdef VR_SEATFAIL_PLANTED
		(void)o;
		return nullptr;
#else
		if (o.poll < 0)
			return "its runtime's event queue failed (xrPollEvent): the server or its IPC is gone";
		if (o.instance_loss)
			return "its runtime announced instance loss";
		if (o.session_state == XR_SESSION_STATE_LOSS_PENDING)
			return "its session is being lost (LOSS_PENDING)";
		if (o.session_state == XR_SESSION_STATE_EXITING)
			return "its runtime ended the session (EXITING)";
		if (o.get_system == XR_ERROR_INSTANCE_LOST || o.get_system == XR_ERROR_RUNTIME_FAILURE ||
			o.get_system == XR_ERROR_RUNTIME_UNAVAILABLE)
			return "its runtime went away while the seat waited for its headset (xrGetSystem)";
		if (o.socket_seen && !o.socket_now)
			return "its runtime socket vanished from the seat dir: the server stopped";
		return nullptr;
#endif
	}

	std::string RuntimeLibFromManifest(const std::string& json_path)
	{
		std::ifstream f(json_path);
		if (!f.good())
			return {};
		std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		const size_t key = text.find("\"library_path\"");
		if (key == std::string::npos)
			return {};
		const size_t colon = text.find(':', key);
		const size_t q1 = text.find('"', colon + 1);
		const size_t q2 = text.find('"', q1 + 1);
		if (q1 == std::string::npos || q2 == std::string::npos)
			return {};
		std::string lib = text.substr(q1 + 1, q2 - q1 - 1);
		if (lib.empty() || lib.find_first_of("/\\") == std::string::npos || lib[0] == '/')
			return lib;
		std::string dir = json_path;
		if (char* real = ::realpath(json_path.c_str(), nullptr))
		{
			dir = real;
			std::free(real);
		}
		const size_t slash = dir.rfind('/');
		if (slash == std::string::npos)
			return lib;
		return dir.substr(0, slash + 1) + lib;
	}

	const char* ChooseVulkanBinding(const char* const* offered, size_t count)
	{
		for (size_t i = 0; i < count; i++)
		{
			if (offered[i] && std::strcmp(offered[i], XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
				return XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
		}
		return nullptr;
	}

	XRSwapchainFormat::Choice ChooseSwapchainFormat(const int64_t* offered, size_t count, bool prefer_unorm)
	{
		return XRSwapchainFormat::Choose(offered, count, prefer_unorm);
	}

	bool JoinOrDetach(std::thread& t, const std::atomic<bool>& exited, std::chrono::milliseconds timeout)
	{
		if (!t.joinable())
			return true;
#ifdef VR_FORKBUGS_PLANTED_SEATJOIN
		t.join();
		return true;
#else
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!exited.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		if (exited.load(std::memory_order_acquire))
		{
			t.join();
			return true;
		}
		t.detach();
		return false;
#endif
	}

	void Start(int seat)
	{
		if (s_thread.joinable())
			return;
		if (s_abandoned)
		{
			if (!s_exited.load(std::memory_order_acquire))
			{
				Console.Error("(VR) SeatSession: a previous seat thread is still stuck in its runtime; "
							  "not starting another beside it.");
				return;
			}
			s_abandoned = false;
		}
		s_run.store(true);
		s_exited.store(false, std::memory_order_release);
		SetState(State::Connecting);
		s_thread = std::thread(ThreadMain, seat);
	}

	void Stop()
	{
		if (!s_thread.joinable())
			return;
		s_run.store(false);
		if (!JoinOrDetach(s_thread, s_exited, kStopJoinTimeout))
		{
			s_abandoned = true;
			Console.Error("(VR) SeatSession: the seat thread did not exit within %lld ms; detached it "
						  "(its runtime is stuck). VM shutdown continues.",
				static_cast<long long>(kStopJoinTimeout.count()));
		}
		SeatCast::Shutdown();
	}

	bool Running()
	{
		return s_running.load();
	}
}
