// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/XRSession.h"
#include <cstdlib>
#include "VR/VRInput.h"
#include "VR/VRManager.h"

#include "common/Assertions.h"
#include "common/Console.h"

#include <atomic>
#include <cstring>
#include <iterator>
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
		bool s_cylinder_supported = false;

#ifdef ENABLE_VULKAN
		PFN_xrGetVulkanGraphicsRequirements2KHR s_xrGetVulkanGraphicsRequirements2KHR = nullptr;
		PFN_xrCreateVulkanInstanceKHR s_xrCreateVulkanInstanceKHR = nullptr;
		PFN_xrGetVulkanGraphicsDevice2KHR s_xrGetVulkanGraphicsDevice2KHR = nullptr;
		PFN_xrCreateVulkanDeviceKHR s_xrCreateVulkanDeviceKHR = nullptr;
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
			const std::string dir = VR::ResolveSeatRuntimeDir(seat);
			if (dir.empty())
			{
				Console.Error("(VR) --vr-seat %d has no entry in [VR] XrSeatRuntimeDirs — refusing to bind the default seat instead.", seat);
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
		}
		~ScopedSeatRuntimeDir()
		{
			if (!m_swapped)
				return;
			if (m_had_old)
				setenv("XDG_RUNTIME_DIR", m_saved.c_str(), 1);
			else
				unsetenv("XDG_RUNTIME_DIR");
		}
		bool refused() const { return m_refuse; }

	private:
		std::string m_saved;
		bool m_had_old = false;
		bool m_swapped = false;
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
					}
				}
			}
		}
#ifdef ENABLE_VULKAN
		const char* enabled_extensions[2] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, nullptr};
		u32 enabled_count = 1;
		if (s_cylinder_supported)
			enabled_extensions[enabled_count++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
		ici.enabledExtensionCount = enabled_count;
		ici.enabledExtensionNames = enabled_extensions;
		Console.WriteLn("(VR) Cylinder composition layers: %s.",
			s_cylinder_supported ? "supported (curved screen available)" : "not offered by the runtime");
#endif

		XrResult res = xrCreateInstance(&ici, &s_instance);
		if (XR_FAILED(res))
		{
			Console.Warning("(VR) xrCreateInstance failed (%d) — is an OpenXR runtime installed and active? Running flat.",
				static_cast<int>(res));
			s_instance = XR_NULL_HANDLE;
			return false;
		}

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
		if (CheckXR(xrGetSystemProperties(s_instance, s_system_id, &sp), "xrGetSystemProperties"))
			Console.WriteLn("(VR) HMD: %s", sp.systemName);

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

		LOAD_XR_FN(xrGetVulkanGraphicsRequirements2KHR);
		LOAD_XR_FN(xrCreateVulkanInstanceKHR);
		LOAD_XR_FN(xrGetVulkanGraphicsDevice2KHR);
		LOAD_XR_FN(xrCreateVulkanDeviceKHR);
#undef LOAD_XR_FN
#endif

		s_lost = false;
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
#ifdef ENABLE_VULKAN
		s_xrGetVulkanGraphicsRequirements2KHR = nullptr;
		s_xrCreateVulkanInstanceKHR = nullptr;
		s_xrGetVulkanGraphicsDevice2KHR = nullptr;
		s_xrCreateVulkanDeviceKHR = nullptr;
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
	bool QueryVulkanGraphicsRequirements()
	{
		XrGraphicsRequirementsVulkan2KHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
		if (!CheckXR(s_xrGetVulkanGraphicsRequirements2KHR(s_instance, s_system_id, &reqs),
				"xrGetVulkanGraphicsRequirements2KHR"))
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

		if (!XRInput::Initialize())
			Console.Warning("(VR) Controller input unavailable this session; spatial controls stay inert.");
		return true;
	}
#endif

	void DestroySession()
	{
		if (s_session == XR_NULL_HANDLE)
			return;

		XRInput::Shutdown();

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
					Console.WriteLn("(VR) Reference space recentered.");
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

	bool IsLost()
	{
		return s_lost;
	}

	const char* GetStateName()
	{
		return SessionStateName(s_session_state);
	}
}
