// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#ifdef ENABLE_VULKAN

#include "VR/VRVulkanBridge.h"
#include "VR/SeatSession.h"
#include "VR/VRInternal.h"
#include "VR/VRManager.h"
#include "VR/XRCompositor.h"
#include "VR/XRSession.h"

#include "common/Assertions.h"
#include "common/Console.h"

namespace VR
{
	namespace
	{
		bool s_bootstrap_active = false;

		void AbortBootstrap()
		{
			SeatSession::Stop();
			XRCompositor::Shutdown();
			XRSession::DestroySession();
			XRSession::DestroyInstance();
			Internal::GetVulkanHandles().Clear();
			s_bootstrap_active = false;
		}
	}

	namespace Internal
	{
		VulkanHandles& GetVulkanHandles()
		{
			static VulkanHandles s_handles;
			return s_handles;
		}
	}

	bool BeginVulkanBootstrap()
	{
		pxAssertRel(!s_bootstrap_active, "VR Vulkan bootstrap started twice");

		if (!WantsVR())
			return false;

		Console.WriteLn("(VR) VR is enabled — attempting OpenXR bootstrap for Vulkan device creation.");

		if (!XRSession::CreateInstanceAndSystem())
			return false;

		if (!XRSession::QueryVulkanGraphicsRequirements())
		{
			XRSession::DestroyInstance();
			return false;
		}

		s_bootstrap_active = true;
		return true;
	}

	bool VulkanBootstrapActive()
	{
		return s_bootstrap_active && XRSession::HasInstance();
	}

	bool CreateVulkanInstanceThroughXR(const VkInstanceCreateInfo* ci, VkInstance* out)
	{
		if (!XRSession::CreateVulkanInstanceThroughXR(ci, out))
		{
			Console.Error("(VR) Vulkan instance creation through OpenXR failed — running flat.");
			AbortBootstrap();
			return false;
		}

		return true;
	}

	VkPhysicalDevice GetXrVulkanPhysicalDevice(VkInstance instance)
	{
		const VkPhysicalDevice pd = XRSession::GetVulkanGraphicsDevice(instance);
		if (pd == VK_NULL_HANDLE)
		{
			Console.Error("(VR) Runtime did not provide a Vulkan physical device — running flat.");
			AbortBootstrap();
		}

		return pd;
	}

	bool CreateVulkanDeviceThroughXR(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* ci, VkDevice* out)
	{
		if (!XRSession::CreateVulkanDeviceThroughXR(physical_device, ci, out))
		{
			Console.Error("(VR) Vulkan device creation through OpenXR failed — running flat.");
			AbortBootstrap();
			return false;
		}

		return true;
	}

	bool OnVulkanDeviceCreated(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
		u32 queue_family, VkQueue queue)
	{
		if (!VulkanBootstrapActive())
			return false;

		Internal::VulkanHandles& handles = Internal::GetVulkanHandles();
		handles.instance = instance;
		handles.physical_device = physical_device;
		handles.device = device;
		handles.queue = queue;
		handles.queue_family = queue_family;

		if (!XRSession::CreateSessionVK(instance, physical_device, device, queue_family, 0))
		{
			AbortBootstrap();
			return false;
		}

		if (!XRCompositor::Initialize())
		{
			Console.Error("(VR) Compositor initialization failed — running flat.");
			AbortBootstrap();
			return false;
		}

		Console.WriteLn("(VR) VR session active. Put on the headset.");
		return true;
	}

	void OnGSDeviceDestroyed()
	{
		if (!s_bootstrap_active && !XRSession::HasInstance())
			return;

		Console.WriteLn("(VR) GS device destroyed — tearing down VR.");
		AbortBootstrap();
	}
}

#endif
