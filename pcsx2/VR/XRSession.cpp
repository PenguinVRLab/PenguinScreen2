// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/XRSession.h"
#include "VR/SpaceChange.h"
#include <cstdlib>
#include "VR/HandTracking.h"
#include "VR/VRInput.h"
#include "VR/VRManager.h"
#include "VR/XRExtensions.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <atomic>
#include <cstring>
#include <iterator>
#include <string_view>
#include <vector>

#ifdef ENABLE_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>
#endif

namespace VR::XRSession
{
	namespace
	{
		XrInstance s_instance = XR_NULL_HANDLE;
		XrSystemId s_system_id = XR_NULL_SYSTEM_ID;
		XrSession s_session = XR_NULL_HANDLE;
		XrSpace s_space = XR_NULL_HANDLE;
		XrSessionState s_session_state = XR_SESSION_STATE_UNKNOWN;
		std::atomic_bool s_session_running{false};
		bool s_lost = false;
		SpaceChangeFollow s_local_space_change;
		bool s_cylinder_supported = false;
		std::vector<std::string> s_enabled_extensions;
		RuntimeLimits s_limits;

#ifdef ENABLE_VULKAN
		PFN_xrGetVulkanGraphicsRequirements2KHR s_xrGetVulkanGraphicsRequirements2KHR = nullptr;
		PFN_xrCreateVulkanInstanceKHR s_xrCreateVulkanInstanceKHR = nullptr;
		PFN_xrGetVulkanGraphicsDevice2KHR s_xrGetVulkanGraphicsDevice2KHR = nullptr;
		PFN_xrCreateVulkanDeviceKHR s_xrCreateVulkanDeviceKHR = nullptr;

		bool s_vulkan_enable1 = false;
		PFN_xrGetVulkanGraphicsRequirementsKHR s_xrGetVulkanGraphicsRequirementsKHR = nullptr;
		PFN_xrGetVulkanInstanceExtensionsKHR s_xrGetVulkanInstanceExtensionsKHR = nullptr;
		PFN_xrGetVulkanDeviceExtensionsKHR s_xrGetVulkanDeviceExtensionsKHR = nullptr;
		PFN_xrGetVulkanGraphicsDeviceKHR s_xrGetVulkanGraphicsDeviceKHR = nullptr;
#endif

		bool CheckXR(XrResult res, const char* what)
		{
			if (XR_SUCCEEDED(res))
				return true;

			char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
			if (s_instance != XR_NULL_HANDLE)
				xrResultToString(s_instance, res, buf);
			Console.Error("(VR) %s failed: %s (%d)", what, buf, static_cast<int>(res));
			return false;
		}

		const char* SessionStateName(XrSessionState state)
		{
			switch (state)
			{
				case XR_SESSION_STATE_IDLE:         return "IDLE";
				case XR_SESSION_STATE_READY:        return "READY";
				case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
				case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
				case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
				case XR_SESSION_STATE_STOPPING:     return "STOPPING";
				case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
				case XR_SESSION_STATE_EXITING:      return "EXITING";
				default:                            return "UNKNOWN";
			}
		}

		void HandleSessionStateChange(XrSessionState new_state)
		{
			Console.WriteLn("(VR) Session state: %s -> %s", SessionStateName(s_session_state), SessionStateName(new_state));
			s_session_state = new_state;

			switch (new_state)
			{
				case XR_SESSION_STATE_READY:
				{
					XrSessionBeginInfo begin_info = {XR_TYPE_SESSION_BEGIN_INFO};
					begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (CheckXR(xrBeginSession(s_session, &begin_info), "xrBeginSession"))
					{
						Console.WriteLn("(VR) Session began.");
						s_session_running.store(true, std::memory_order_release);
					}
					else
					{
						s_lost = true;
					}
					break;
				}

				case XR_SESSION_STATE_STOPPING:
				{
					s_session_running.store(false, std::memory_order_release);
					CheckXR(xrEndSession(s_session), "xrEndSession");
					Console.WriteLn("(VR) Session ended (runtime request). It may begin again when READY.");
					break;
				}

				case XR_SESSION_STATE_LOSS_PENDING:
				case XR_SESSION_STATE_EXITING:
				{
					s_session_running.store(false, std::memory_order_release);
					s_lost = true;
					break;
				}

				default:
					break;
			}
		}
	}

	class ScopedSeatRuntimeDir
	{
	public:
		ScopedSeatRuntimeDir()
		{
			const int seat = VR::GetLaunchSeat();
			if (seat <= 1)
				return;
#ifndef ENABLE_VR_SEATS
			Console.Error("(VR) --vr-seat %d: seats are not available on this platform — refusing to bind.", seat);
			m_refuse = true;
			return;
#else
			const std::string dir = VR::ResolveSeatRuntimeDir(seat);
			if (dir.empty())
			{
				Console.Error("(VR) --vr-seat %d has no runtime dir (--vr-seat-dirs or [VR] XrSeatRuntimeDirs) — refusing to bind the default seat instead.", seat);
				m_refuse = true;
				return;
			}
			const char* old = std::getenv("XDG_RUNTIME_DIR");
			if (old)
				m_saved = old;
			m_had_old = (old != nullptr);
			setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
			m_swapped = true;
			Console.WriteLn("(VR) XR seat %d: binding runtime socket %s/wivrn/comp_ipc", seat, dir.c_str());
#endif
		}
		~ScopedSeatRuntimeDir()
		{
#ifdef ENABLE_VR_SEATS
			if (!m_swapped)
				return;
			if (m_had_old)
				setenv("XDG_RUNTIME_DIR", m_saved.c_str(), 1);
			else
				unsetenv("XDG_RUNTIME_DIR");
#endif
		}
		bool refused() const { return m_refuse; }

	private:
#ifdef ENABLE_VR_SEATS
		std::string m_saved;
		bool m_had_old = false;
		bool m_swapped = false;
#endif
		bool m_refuse = false;
	};

	bool CreateInstanceAndSystem()
	{
		pxAssertRel(s_instance == XR_NULL_HANDLE, "XR instance created twice");

		const ScopedSeatRuntimeDir seat_binding;
		if (seat_binding.refused())
			return false;

		XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
		std::strncpy(ici.applicationInfo.applicationName, "PenguinScreen2", XR_MAX_APPLICATION_NAME_SIZE - 1);
		std::strncpy(ici.applicationInfo.engineName, "PenguinScreen2", XR_MAX_ENGINE_NAME_SIZE - 1);
		ici.applicationInfo.applicationVersion = 1;
		ici.applicationInfo.engineVersion = 1;
		ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;

		s_cylinder_supported = false;
		bool has_vulkan_enable2 = false;
		bool has_vulkan_enable1 = false;
		bool has_hand_tracking = false;
		bool has_frame_controller = false;
		bool has_hand_interaction = false;
		XRInput::SetFrameControllerExtensionEnabled(false);
		XRInput::SetHandInteractionExtensionEnabled(false);
		{
			u32 ext_count = 0;
			if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr)) && ext_count > 0)
			{
				std::vector<XrExtensionProperties> props(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
				if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, props.data())))
				{
					for (const XrExtensionProperties& p : props)
					{
						if (std::strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) == 0)
							s_cylinder_supported = true;
						else if (std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
							has_vulkan_enable2 = true;
						else if (std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0)
							has_vulkan_enable1 = true;
						else if (std::strcmp(p.extensionName, HandTracking::kExtensionName) == 0)
							has_hand_tracking = true;
						else if (std::strcmp(p.extensionName, PadXRActions::kFrameExtensionName) == 0)
							has_frame_controller = true;
						else if (std::strcmp(p.extensionName, XR_EXT_HAND_INTERACTION_EXTENSION_NAME) == 0)
							has_hand_interaction = true;
					}
				}
			}
		}
#ifdef ENABLE_VULKAN
		if (!has_vulkan_enable2 && !has_vulkan_enable1)
		{
			Console.Error("(VR) The OpenXR runtime offers neither XR_KHR_vulkan_enable2 nor XR_KHR_vulkan_enable, "
						  "so no Vulkan graphics binding is possible (PenguinScreen2's VR path is Vulkan-only). "
						  "Running flat.");
			return false;
		}
		s_vulkan_enable1 = !has_vulkan_enable2;
		static_assert(std::string_view(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == XRExtensions::kVulkanEnable2);
		static_assert(std::string_view(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == XRExtensions::kVulkanEnable);
		static_assert(std::string_view(PadXRActions::kFrameExtensionName) == XRExtensions::kFrameController);
		XRExtensions::Offered offered;
		offered.vulkan_enable2 = has_vulkan_enable2;
		offered.cylinder = s_cylinder_supported;
		offered.hand_tracking = has_hand_tracking;
		offered.frame_controller = has_frame_controller;
		offered.hand_interaction = has_hand_interaction;
		const std::vector<const char*> enabled_extensions = XRExtensions::ToEnable(offered);
		ici.enabledExtensionCount = static_cast<u32>(enabled_extensions.size());
		ici.enabledExtensionNames = enabled_extensions.data();
		Console.WriteLn("(VR) Vulkan binding: %s%s.", enabled_extensions[0],
			s_vulkan_enable1 ? " (legacy; the runtime does not offer XR_KHR_vulkan_enable2 — we create the "
							   "VkInstance/VkDevice ourselves with the extensions it names)" :
							   "");
		Console.WriteLn("(VR) Cylinder composition layers: %s.",
			s_cylinder_supported ? "supported (curved screen available)" : "not offered by the runtime");
#endif

		XrResult res = xrCreateInstance(&ici, &s_instance);
		s_enabled_extensions.clear();
		if (XR_SUCCEEDED(res))
		{
			for (u32 i = 0; i < ici.enabledExtensionCount; i++)
				s_enabled_extensions.emplace_back(ici.enabledExtensionNames[i]);
		}
		if (XR_FAILED(res))
		{
			Console.Warning("(VR) xrCreateInstance failed (%d) — is an OpenXR runtime installed and active? Running flat.",
				static_cast<int>(res));
			s_instance = XR_NULL_HANDLE;
			return false;
		}

#ifdef ENABLE_VULKAN
		HandTracking::SetExtensionEnabled(s_instance, has_hand_tracking);
		XRInput::SetFrameControllerExtensionEnabled(has_frame_controller);
		XRInput::SetHandInteractionExtensionEnabled(has_hand_interaction);
		Console.WriteLn("(VR) Hand interaction profile: %s.",
			has_hand_interaction ? "XR_EXT_hand_interaction enabled (pinch value + aim)" :
								   "not offered by the runtime");
		Console.WriteLn("(VR) Steam Frame controller profile: %s.",
			has_frame_controller ? "XR_VALVE_frame_controller_interaction enabled (provisional mapping)" :
								   "not offered by the runtime");
#endif

		XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
		if (CheckXR(xrGetInstanceProperties(s_instance, &ip), "xrGetInstanceProperties"))
		{
			Console.WriteLn("(VR) OpenXR runtime: %s %u.%u.%u", ip.runtimeName,
				XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
				XR_VERSION_PATCH(ip.runtimeVersion));
		}

		XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		res = xrGetSystem(s_instance, &sgi, &s_system_id);
		if (XR_FAILED(res))
		{
			Console.Warning("(VR) No HMD system available (%d) — headset connected and awake? Running flat.",
				static_cast<int>(res));
			DestroyInstance();
			return false;
		}

		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		s_limits = RuntimeLimits{};
		if (CheckXR(xrGetSystemProperties(s_instance, s_system_id, &sp), "xrGetSystemProperties"))
		{
			Console.WriteLn("(VR) HMD: %s", sp.systemName);
			s_limits.max_swapchain_width = sp.graphicsProperties.maxSwapchainImageWidth;
			s_limits.max_swapchain_height = sp.graphicsProperties.maxSwapchainImageHeight;
			s_limits.max_layer_count = sp.graphicsProperties.maxLayerCount;
		}
		{
			XrViewConfigurationProperties vcp = {XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
			if (XR_SUCCEEDED(xrGetViewConfigurationProperties(s_instance, s_system_id,
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &vcp)))
			{
				s_limits.fov_mutable = (vcp.fovMutable == XR_TRUE);
				s_limits.fov_mutable_known = true;
			}
			else
			{
				Console.Warning("(VR) xrGetViewConfigurationProperties(PRIMARY_STEREO) failed; fovMutable unknown.");
			}
		}

#ifdef ENABLE_VULKAN
#define LOAD_XR_FN(name) \
	do \
	{ \
		if (XR_FAILED(xrGetInstanceProcAddr(s_instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&s_##name)))) \
		{ \
			Console.Error("(VR) Failed to load " #name " — running flat."); \
			DestroyInstance(); \
			return false; \
		} \
	} while (0)

		if (s_vulkan_enable1)
		{
			LOAD_XR_FN(xrGetVulkanGraphicsRequirementsKHR);
			LOAD_XR_FN(xrGetVulkanInstanceExtensionsKHR);
			LOAD_XR_FN(xrGetVulkanDeviceExtensionsKHR);
			LOAD_XR_FN(xrGetVulkanGraphicsDeviceKHR);
		}
		else
		{
			LOAD_XR_FN(xrGetVulkanGraphicsRequirements2KHR);
			LOAD_XR_FN(xrCreateVulkanInstanceKHR);
			LOAD_XR_FN(xrGetVulkanGraphicsDevice2KHR);
			LOAD_XR_FN(xrCreateVulkanDeviceKHR);
		}
#undef LOAD_XR_FN
#endif

		s_lost = false;
		s_local_space_change = {};
		return true;
	}

	void DestroyInstance()
	{
		DestroySession();

		if (s_instance != XR_NULL_HANDLE)
		{
			try
			{
				xrDestroyInstance(s_instance);
			}
			catch (...)
			{
				Console.Warning("(VR) xrDestroyInstance threw during teardown — continuing.");
			}
			s_instance = XR_NULL_HANDLE;
		}

		s_system_id = XR_NULL_SYSTEM_ID;
		s_lost = false;
		s_local_space_change = {};
		HandTracking::SetExtensionEnabled(XR_NULL_HANDLE, false);
		XRInput::SetFrameControllerExtensionEnabled(false);
		XRInput::SetHandInteractionExtensionEnabled(false);
#ifdef ENABLE_VULKAN
		s_xrGetVulkanGraphicsRequirements2KHR = nullptr;
		s_xrCreateVulkanInstanceKHR = nullptr;
		s_xrGetVulkanGraphicsDevice2KHR = nullptr;
		s_xrCreateVulkanDeviceKHR = nullptr;
		s_vulkan_enable1 = false;
		s_xrGetVulkanGraphicsRequirementsKHR = nullptr;
		s_xrGetVulkanInstanceExtensionsKHR = nullptr;
		s_xrGetVulkanDeviceExtensionsKHR = nullptr;
		s_xrGetVulkanGraphicsDeviceKHR = nullptr;
#endif
	}

	bool UsesVulkanEnable1()
	{
#ifdef ENABLE_VULKAN
		return s_vulkan_enable1;
#else
		return false;
#endif
	}

	bool HasInstance()
	{
		return s_instance != XR_NULL_HANDLE;
	}

	XrInstance GetInstance()
	{
		return s_instance;
	}

	XrSystemId GetSystemId()
	{
		return s_system_id;
	}

#ifdef ENABLE_VULKAN
	namespace
	{
		using PFN_v1ExtensionQuery = XrResult(XRAPI_PTR*)(XrInstance, XrSystemId, uint32_t, uint32_t*, char*);
		bool QueryV1ExtensionString(PFN_v1ExtensionQuery fn, const char* what, std::string& out)
		{
			uint32_t len = 0;
			if (!CheckXR(fn(s_instance, s_system_id, 0, &len, nullptr), what))
				return false;
			out.assign(len, '\0');
			if (len == 0)
				return true;
			if (!CheckXR(fn(s_instance, s_system_id, len, &len, out.data()), what))
				return false;
			out.resize(std::strlen(out.c_str()));
			return true;
		}

		void MergeExtensionNames(const char* const* existing, uint32_t existing_count, const std::string& runtime_list,
			std::vector<std::string>& storage, std::vector<const char*>& out)
		{
			out.assign(existing, existing + existing_count);
			size_t pos = 0;
			while (pos < runtime_list.size())
			{
				size_t end = runtime_list.find(' ', pos);
				if (end == std::string::npos)
					end = runtime_list.size();
				if (end > pos)
				{
					const std::string name = runtime_list.substr(pos, end - pos);
					bool present = false;
					for (const char* e : out)
					{
						if (name == e)
						{
							present = true;
							break;
						}
					}
					bool queued = false;
					for (const std::string& q : storage)
					{
						if (q == name)
						{
							queued = true;
							break;
						}
					}
					if (!present && !queued)
						storage.push_back(name);
				}
				pos = end + 1;
			}
			for (const std::string& name : storage)
				out.push_back(name.c_str());
		}
	}

	bool QueryVulkanGraphicsRequirements()
	{
		XrGraphicsRequirementsVulkan2KHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
		const XrResult res = s_vulkan_enable1 ? s_xrGetVulkanGraphicsRequirementsKHR(s_instance, s_system_id, &reqs) :
												s_xrGetVulkanGraphicsRequirements2KHR(s_instance, s_system_id, &reqs);
		if (!CheckXR(res, s_vulkan_enable1 ? "xrGetVulkanGraphicsRequirementsKHR" : "xrGetVulkanGraphicsRequirements2KHR"))
		{
			return false;
		}

		Console.WriteLn("(VR) Runtime supports Vulkan %u.%u - %u.%u",
			XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported),
			XR_VERSION_MAJOR(reqs.maxApiVersionSupported), XR_VERSION_MINOR(reqs.maxApiVersionSupported));

		return true;
	}

	bool CreateVulkanInstanceThroughXR(const VkInstanceCreateInfo* ci, VkInstance* out)
	{
		if (s_vulkan_enable1)
		{
			std::string list;
			if (!QueryV1ExtensionString(s_xrGetVulkanInstanceExtensionsKHR, "xrGetVulkanInstanceExtensionsKHR", list))
				return false;
			std::vector<std::string> storage;
			std::vector<const char*> names;
			MergeExtensionNames(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, list, storage, names);
			VkInstanceCreateInfo v1ci = *ci;
			v1ci.enabledExtensionCount = static_cast<uint32_t>(names.size());
			v1ci.ppEnabledExtensionNames = names.data();
			const VkResult vk_result = vkCreateInstance(&v1ci, nullptr, out);
			if (vk_result != VK_SUCCESS)
			{
				Console.Error("(VR) XR_KHR_vulkan_enable: vkCreateInstance failed (%d) with the runtime's required "
							  "instance extensions [%s] merged in.",
					static_cast<int>(vk_result), list.c_str());
				return false;
			}
			Console.WriteLn("(VR) XR_KHR_vulkan_enable: VkInstance created by us; runtime-required instance "
							"extensions: [%s] (%zu new).",
				list.c_str(), storage.size());
			return true;
		}

		XrVulkanInstanceCreateInfoKHR xci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
		xci.systemId = s_system_id;
		xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
		xci.vulkanCreateInfo = ci;

		VkResult vk_result = VK_SUCCESS;
		if (!CheckXR(s_xrCreateVulkanInstanceKHR(s_instance, &xci, out, &vk_result), "xrCreateVulkanInstanceKHR"))
			return false;
		if (vk_result != VK_SUCCESS)
		{
			Console.Error("(VR) xrCreateVulkanInstanceKHR: vkCreateInstance failed (%d)", static_cast<int>(vk_result));
			return false;
		}

		return true;
	}

	VkPhysicalDevice GetVulkanGraphicsDevice(VkInstance instance)
	{
		if (s_vulkan_enable1)
		{
			VkPhysicalDevice pd = VK_NULL_HANDLE;
			if (!CheckXR(s_xrGetVulkanGraphicsDeviceKHR(s_instance, s_system_id, instance, &pd), "xrGetVulkanGraphicsDeviceKHR"))
				return VK_NULL_HANDLE;
			return pd;
		}

		XrVulkanGraphicsDeviceGetInfoKHR info = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
		info.systemId = s_system_id;
		info.vulkanInstance = instance;

		VkPhysicalDevice pd = VK_NULL_HANDLE;
		if (!CheckXR(s_xrGetVulkanGraphicsDevice2KHR(s_instance, &info, &pd), "xrGetVulkanGraphicsDevice2KHR"))
			return VK_NULL_HANDLE;

		return pd;
	}

	bool CreateVulkanDeviceThroughXR(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* ci, VkDevice* out)
	{
		if (s_vulkan_enable1)
		{
			std::string list;
			if (!QueryV1ExtensionString(s_xrGetVulkanDeviceExtensionsKHR, "xrGetVulkanDeviceExtensionsKHR", list))
				return false;
			std::vector<std::string> storage;
			std::vector<const char*> names;
			MergeExtensionNames(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, list, storage, names);
			VkDeviceCreateInfo v1ci = *ci;
			v1ci.enabledExtensionCount = static_cast<uint32_t>(names.size());
			v1ci.ppEnabledExtensionNames = names.data();
			const VkResult vk_result = vkCreateDevice(physical_device, &v1ci, nullptr, out);
			if (vk_result != VK_SUCCESS)
			{
				Console.Error("(VR) XR_KHR_vulkan_enable: vkCreateDevice failed (%d) with the runtime's required "
							  "device extensions [%s] merged in.",
					static_cast<int>(vk_result), list.c_str());
				return false;
			}
			Console.WriteLn("(VR) XR_KHR_vulkan_enable: VkDevice created by us; runtime-required device "
							"extensions: [%s] (%zu new).",
				list.c_str(), storage.size());
			return true;
		}

		XrVulkanDeviceCreateInfoKHR xci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
		xci.systemId = s_system_id;
		xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
		xci.vulkanPhysicalDevice = physical_device;
		xci.vulkanCreateInfo = ci;

		VkResult vk_result = VK_SUCCESS;
		if (!CheckXR(s_xrCreateVulkanDeviceKHR(s_instance, &xci, out, &vk_result), "xrCreateVulkanDeviceKHR"))
			return false;
		if (vk_result != VK_SUCCESS)
		{
			Console.Error("(VR) xrCreateVulkanDeviceKHR: vkCreateDevice failed (%d)", static_cast<int>(vk_result));
			return false;
		}

		return true;
	}

	bool CreateSessionVK(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
		u32 queue_family, u32 queue_index)
	{
		pxAssertRel(s_session == XR_NULL_HANDLE, "XR session created twice");

		XrGraphicsBindingVulkan2KHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
		binding.instance = instance;
		binding.physicalDevice = physical_device;
		binding.device = device;
		binding.queueFamilyIndex = queue_family;
		binding.queueIndex = queue_index;

		XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
		sci.next = &binding;
		sci.systemId = s_system_id;

		if (!CheckXR(xrCreateSession(s_instance, &sci, &s_session), "xrCreateSession"))
		{
			s_session = XR_NULL_HANDLE;
			return false;
		}

		XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		rsci.poseInReferenceSpace.orientation.w = 1.0f;
		if (!CheckXR(xrCreateReferenceSpace(s_session, &rsci, &s_space), "xrCreateReferenceSpace"))
		{
			DestroySession();
			return false;
		}

		s_session_state = XR_SESSION_STATE_UNKNOWN;
		Console.WriteLn("(VR) XR session created (Vulkan, queue family %u).", queue_family);
		Console.WriteLn("(VR) Runtime limits: swapchain max %ux%u, composition layers max %u, fovMutable=%s.",
			s_limits.max_swapchain_width, s_limits.max_swapchain_height, s_limits.max_layer_count,
			s_limits.fov_mutable_known ? (s_limits.fov_mutable ? "true" : "false") : "unknown");

		if (!XRInput::Initialize())
			Console.Warning("(VR) Controller input unavailable this session; spatial controls stay inert.");
		HandTracking::CreateTrackers(s_session);
		return true;
	}
#endif

	void DestroySession()
	{
		if (s_session == XR_NULL_HANDLE)
			return;

		XRInput::Shutdown();
		HandTracking::DestroyTrackers();

		if (s_session_running.load(std::memory_order_acquire))
		{
			s_session_running.store(false, std::memory_order_release);
			try
			{
				xrEndSession(s_session);
			}
			catch (...)
			{
				Console.Warning("(VR) xrEndSession threw (connection already dead) — continuing teardown.");
			}
		}

		if (s_space != XR_NULL_HANDLE)
		{
			try
			{
				xrDestroySpace(s_space);
			}
			catch (...)
			{
				Console.Warning("(VR) xrDestroySpace threw during teardown — continuing.");
			}
			s_space = XR_NULL_HANDLE;
		}

		try
		{
			xrDestroySession(s_session);
		}
		catch (...)
		{
			Console.Warning("(VR) xrDestroySession threw during teardown — continuing.");
		}
		s_session = XR_NULL_HANDLE;
		s_session_state = XR_SESSION_STATE_UNKNOWN;
		Console.WriteLn("(VR) XR session destroyed.");
	}

	bool HasSession()
	{
		return s_session != XR_NULL_HANDLE;
	}

	XrSession GetSession()
	{
		return s_session;
	}

	XrSpace GetSpace()
	{
		return s_space;
	}

	void PumpEvents()
	{
		if (s_instance == XR_NULL_HANDLE)
			return;

		for (;;)
		{
			XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
			const XrResult res = xrPollEvent(s_instance, &event);
			if (res == XR_EVENT_UNAVAILABLE)
				break;
			if (XR_FAILED(res))
			{
				CheckXR(res, "xrPollEvent");
				s_lost = true;
				break;
			}

			switch (event.type)
			{
				case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
				{
					const auto& e = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
					if (e.session == s_session)
						HandleSessionStateChange(e.state);
					break;
				}

				case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				{
					Console.Error("(VR) OpenXR instance loss pending (runtime shutting down/updating).");
					s_session_running.store(false, std::memory_order_release);
					s_lost = true;
					break;
				}

				case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
				{
					const auto* ev = reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
					if (ev->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
					{
						s_local_space_change.OnChangePending(ev->changeTime);
						Console.WriteLn("(VR) Reference space recentered by the runtime; head-look re-baselines at its change time.");
					}
					else
					{
						Console.WriteLn("(VR) Reference space %d changed (not ours; nothing to follow).",
							static_cast<int>(ev->referenceSpaceType));
					}
					break;
				}

				case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
				{
					const auto* ev = reinterpret_cast<const XrEventDataInteractionProfileChanged*>(&event);
					if (ev->session == s_session)
						XRInput::NoteInteractionProfileChanged();
					break;
				}

				case XR_TYPE_EVENT_DATA_EVENTS_LOST:
				{
					Console.Warning("(VR) OpenXR event queue overflowed; some events were lost.");
					break;
				}

				default:
					break;
			}
		}
	}

	bool IsSessionRunning()
	{
		return s_session_running.load(std::memory_order_acquire);
	}

	bool HasCylinderLayer()
	{
		return s_cylinder_supported;
	}

	const RuntimeLimits& GetRuntimeLimits()
	{
		return s_limits;
	}

	bool ConsumeLocalSpaceChange(XrTime display_time)
	{
		return s_local_space_change.Due(display_time);
	}

	bool IsLost()
	{
		return s_lost;
	}

	void MarkLost()
	{
		s_lost = true;
	}

	void ParkHandlesForAbandonedPacer()
	{
		Console.Warning("(VR) Parking XR handles (session=%p instance=%p) — abandoned with a stuck pacer.",
			reinterpret_cast<void*>(s_session), reinterpret_cast<void*>(s_instance));
		HandTracking::ParkHandles();
		XRInput::SetFrameControllerExtensionEnabled(false);
		XRInput::SetHandInteractionExtensionEnabled(false);
		s_session = XR_NULL_HANDLE;
		s_space = XR_NULL_HANDLE;
		s_instance = XR_NULL_HANDLE;
		s_system_id = XR_NULL_SYSTEM_ID;
		s_session_state = XR_SESSION_STATE_UNKNOWN;
		s_session_running.store(false, std::memory_order_release);
		s_lost = false;
#ifdef ENABLE_VULKAN
		s_xrGetVulkanGraphicsRequirements2KHR = nullptr;
		s_xrCreateVulkanInstanceKHR = nullptr;
		s_xrGetVulkanGraphicsDevice2KHR = nullptr;
		s_xrCreateVulkanDeviceKHR = nullptr;
#endif
	}

	const char* GetStateName()
	{
		return SessionStateName(s_session_state);
	}

	const std::vector<std::string>& EnabledExtensions()
	{
		return s_enabled_extensions;
	}
}
