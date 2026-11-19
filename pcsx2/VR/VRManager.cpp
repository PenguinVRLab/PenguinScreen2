// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0

#include "VR/VRManager.h"
#include "VR/StereoState.h"
#include "VR/VRProfileDB.h"
#include "VR/SplitState.h"
#include "VR/SeatSession.h"
#include "VR/LagMatch.h"
#include "VR/XRCompositor.h"
#include "VR/XRSession.h"

#include "BuildVersion.h"
#include "Config.h"
#include "Host.h"
#include "MTGS.h"
#include "Memory.h"
#include "VMManager.h"
#include "Input/InputManager.h"
#include "Input/VRInputSource.h"
#include "VR/NearField.h"
#include "VR/HandCapture.h"
#include "USB/USB.h"

#include "pcsx2/GS.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <openxr/openxr.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace VR
{
	namespace
	{
		const char* ResultToString(XrResult result)
		{
			switch (result)
			{
				case XR_SUCCESS:                          return "XR_SUCCESS";
				case XR_ERROR_RUNTIME_UNAVAILABLE:        return "XR_ERROR_RUNTIME_UNAVAILABLE";
				case XR_ERROR_RUNTIME_FAILURE:            return "XR_ERROR_RUNTIME_FAILURE";
				case XR_ERROR_INSTANCE_LOST:              return "XR_ERROR_INSTANCE_LOST";
				case XR_ERROR_INITIALIZATION_FAILED:      return "XR_ERROR_INITIALIZATION_FAILED";
				case XR_ERROR_API_VERSION_UNSUPPORTED:    return "XR_ERROR_API_VERSION_UNSUPPORTED";
				case XR_ERROR_FORM_FACTOR_UNAVAILABLE:    return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
				case XR_ERROR_FORM_FACTOR_UNSUPPORTED:    return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
				case XR_ERROR_OUT_OF_MEMORY:              return "XR_ERROR_OUT_OF_MEMORY";
				case XR_ERROR_LIMIT_REACHED:              return "XR_ERROR_LIMIT_REACHED";
				case XR_ERROR_EXTENSION_NOT_PRESENT:      return "XR_ERROR_EXTENSION_NOT_PRESENT";
				case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
				default:                                  return nullptr;
			}
		}

		std::string FormatResult(XrResult result)
		{
			if (const char* name = ResultToString(result))
				return name;
			return fmt::format("XrResult({})", static_cast<int>(result));
		}
	}

	std::string GetRuntimeInfoReport()
	{
		std::string report;
		const auto append = [&report](std::string_view line) {
			report.append(line);
			report.push_back('\n');
		};

		uint32_t count = 0;
		XrResult res = xrEnumerateApiLayerProperties(0, &count, nullptr);
		std::vector<XrApiLayerProperties> layers;
		if (XR_SUCCEEDED(res) && count > 0)
		{
			layers.resize(count, {XR_TYPE_API_LAYER_PROPERTIES});
			res = xrEnumerateApiLayerProperties(count, &count, layers.data());
		}

		count = 0;
		res = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE || res == XR_ERROR_INITIALIZATION_FAILED)
		{
			append(fmt::format("No OpenXR runtime is available ({}).", FormatResult(res)));
			append("Install/start one: SteamVR, Meta Quest Link, Virtual Desktop (VDXR),");
			append("or Monado/WiVRn on Linux, and make sure it is set as the active");
			append("OpenXR runtime in its settings.");
			return report;
		}
		if (XR_FAILED(res))
		{
			append(fmt::format("xrEnumerateInstanceExtensionProperties failed: {}", FormatResult(res)));
			return report;
		}

		std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
		if (count > 0)
			xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());

		XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
		std::strncpy(ici.applicationInfo.applicationName, "PenguinScreen2", XR_MAX_APPLICATION_NAME_SIZE - 1);
		std::strncpy(ici.applicationInfo.engineName, "PenguinScreen2", XR_MAX_ENGINE_NAME_SIZE - 1);
		ici.applicationInfo.applicationVersion = 1;
		ici.applicationInfo.engineVersion = 1;
		ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;

		XrInstance instance = XR_NULL_HANDLE;
		res = xrCreateInstance(&ici, &instance);
		if (XR_FAILED(res))
		{
			append(fmt::format("xrCreateInstance failed: {}", FormatResult(res)));
			append("An OpenXR runtime appears to be installed but could not be initialized.");
			return report;
		}

		XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
		if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &ip)))
		{
			append(fmt::format("Runtime: {} {}.{}.{}", ip.runtimeName,
				XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
				XR_VERSION_PATCH(ip.runtimeVersion)));
		}

		XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XrSystemId system = XR_NULL_SYSTEM_ID;
		res = xrGetSystem(instance, &sgi, &system);
		if (res == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
		{
			append("Headset: none ready (runtime is active, but no HMD is connected/awake).");
			append("If using a Quest: start Link/Air Link or Virtual Desktop first.");
		}
		else if (XR_FAILED(res))
		{
			append(fmt::format("xrGetSystem failed: {}", FormatResult(res)));
		}
		else
		{
			XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
			if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &sp)))
			{
				append(fmt::format("Headset: {} (vendor 0x{:04X})", sp.systemName, sp.vendorId));
				append(fmt::format("  Max swapchain: {}x{}, max composition layers: {}",
					sp.graphicsProperties.maxSwapchainImageWidth,
					sp.graphicsProperties.maxSwapchainImageHeight,
					sp.graphicsProperties.maxLayerCount));
				append(fmt::format("  Tracking: orientation={} position={}",
					sp.trackingProperties.orientationTracking ? "yes" : "no",
					sp.trackingProperties.positionTracking ? "yes" : "no"));
			}

			count = 0;
			if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance, system,
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)) && count > 0)
			{
				std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
				if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(instance, system,
						XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data())))
				{
					for (uint32_t i = 0; i < count; i++)
					{
						append(fmt::format("  Eye {}: recommended {}x{} ({}x MSAA), max {}x{}",
							i, views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight,
							views[i].recommendedSwapchainSampleCount,
							views[i].maxImageRectWidth, views[i].maxImageRectHeight));
					}
				}
			}
		}

		append(fmt::format("API layers ({}):", layers.size()));
		for (const XrApiLayerProperties& layer : layers)
			append(fmt::format("  {} (spec {}.{}.{})", layer.layerName,
				XR_VERSION_MAJOR(layer.specVersion), XR_VERSION_MINOR(layer.specVersion),
				XR_VERSION_PATCH(layer.specVersion)));

		append(fmt::format("Instance extensions ({}):", extensions.size()));
		for (const XrExtensionProperties& ext : extensions)
			append(fmt::format("  {} (v{})", ext.extensionName, ext.extensionVersion));

		xrDestroyInstance(instance);
		return report;
	}

	namespace
	{
		std::mutex s_settings_mutex;
		Pcsx2Config::VROptions s_settings;

		u64 s_watchdog_last_consumed = 0;
		u32 s_watchdog_starved_vsyncs = 0;

		int s_scene_published = -1;
		bool s_scene_memo_valid = false;
		int s_scene_pending = -1;
		u32 s_scene_pending_count = 0;
		constexpr u32 SCENE_DEBOUNCE_VSYNCS = 3;

		void CopyResolvedMap(StereoState::Params& dst, const ProfileDB::StereoResolvedMap& src)
		{
			static_assert(static_cast<u32>(ProfileDB::StereoMap::Linear) == static_cast<u32>(StereoState::Params::Map::Linear) &&
							  static_cast<u32>(ProfileDB::StereoMap::Bands) == static_cast<u32>(StereoState::Params::Map::Bands) &&
							  static_cast<u32>(ProfileDB::StereoMap::Log) == static_cast<u32>(StereoState::Params::Map::Log),
				"StereoMap and StereoState::Params::Map must agree numerically — the copy below casts through u32");
			dst.map = static_cast<StereoState::Params::Map>(src.map);
			dst.band_count = src.band_count;
			for (u32 i = 0; i < 3; i++)
				dst.split_q[i] = src.split_q[i];
			for (u32 i = 0; i < 4; i++)
			{
				dst.conv[i] = src.conv[i];
				dst.sep[i] = src.sep[i];
				dst.bias[i] = src.bias[i];
			}
			dst.log_w0 = src.log_w0;
			dst.log_w1 = src.log_w1;
			dst.log_dfar = src.log_dfar;
			dst.crossed_limit = src.crossed_limit;
			dst.crossed_gain = src.crossed_gain;
		}

		void CopyCollimate(StereoState::Params& dst, const ProfileDB::StereoParams& src)
		{
			static_assert(ProfileDB::kMaxCollimateRules == StereoState::Params::MAX_COLLIMATE_RULES,
				"The authored rule cap and the GS snapshot's fixed array must agree — otherwise a "
				"profile's last rules are parsed and then silently dropped on the way to the shader");
			dst.collimate_disparity = 0.0f;
			dst.collimate_rule_count = 0;
			if (!src.hud_collimate.has_value())
				return;

			const ProfileDB::HudCollimate& hc = src.hud_collimate.value();
			dst.collimate_disparity = hc.disparity;
			for (const ProfileDB::CollimateRule& r : hc.rules)
			{
				if (dst.collimate_rule_count >= StereoState::Params::MAX_COLLIMATE_RULES)
					break;
				StereoState::Params::CollimateRule& o = dst.collimate_rules[dst.collimate_rule_count++];
				o.prim = r.prim;
				o.tme = r.tme;
				o.abe = r.abe;
				o.min_w = r.min_w;
				o.max_w = r.max_w;
				o.min_h = r.min_h;
				o.max_h = r.max_h;
				o.rx0 = r.rx0;
				o.ry0 = r.ry0;
				o.rx1 = r.rx1;
				o.ry1 = r.ry1;
				o.tu0 = r.tu0;
				o.tv0 = r.tv0;
				o.tu1 = r.tu1;
				o.tv1 = r.tv1;
				StringUtil::Strlcpy(o.label, r.label, sizeof(o.label));
			}
		}

		std::string FormatStereoMap(const ProfileDB::StereoResolvedMap& map, float separation, float convergence)
		{
			const float d_inf = ProfileDB::EvalDisparity(map, separation, convergence, 0.0f);
			if (map.map == ProfileDB::StereoMap::Log)
			{
				return fmt::format("log w {:.4g}->{:.4g} dfar {:.4g} | d(inf) {:.4f}",
					map.log_w0, map.log_w1, map.log_dfar, d_inf);
			}

			std::string out = fmt::format("bands({})", map.band_count);
			for (u32 i = 0; i + 1 < map.band_count && i < 3; i++)
			{
				out += fmt::format("{}{:.4g}", (i == 0) ? " splits w[" : ", ",
					(map.split_q[i] != 0.0f) ? (1.0f / map.split_q[i]) : 0.0f);
			}
			if (map.band_count > 1)
				out += "]";
			for (u32 i = 0; i < map.band_count && i < 4; i++)
				out += fmt::format(" | c{:.4g}/s{:.4g}", map.conv[i], map.sep[i]);
			out += fmt::format(" | d(inf) {:.4f}", d_inf);
			return out;
		}
	}

	static void ApplySpatialControls(const ProfileDB::Profile* profile)
	{
		const ProfileDB::Profile* vr_profile = (profile && WantsVR()) ? profile : nullptr;
		const bool game_recipe = vr_profile && vr_profile->game_hands.has_value();
		HandCapture::Global().Configure(game_recipe ? &*vr_profile->game_hands : nullptr);
		if (game_recipe)
			HandCapture::Global().SetEnabled(Host::GetBoolSettingValue("VR", "GameHands", vr_profile->hands == NearField::Hands::Game));
		NearField::SetProfileHands((game_recipe && HandCapture::Global().Enabled()) ? NearField::Hands::Game :
								   vr_profile ? vr_profile->hands : NearField::Hands::Off);
		std::vector<ProfileDB::SpatialControlSpec> specs;
		if (profile && !profile->controls.empty() && WantsVR())
			specs = profile->controls;

		VRInputSource::SetActiveControls(specs);

		std::vector<InputManager::VRBindingOverlayEntry> overlay = VRInputSource::BuildBindingOverlay(specs,
			[](u32 usb_port) { return Host::GetStringSettingValue(USB::GetConfigSection(usb_port).c_str(), "Type", "None"); });
		const size_t count = overlay.size();
		if (InputManager::SetVRBindingOverlay(std::move(overlay)))
		{
			if (count > 0)
				Console.WriteLn("(VR) Spatial controls: %zu control(s), %zu binding(s) overlaid from the profile; reloading input bindings.",
					specs.size(), count);
			else
				Console.WriteLn("(VR) Spatial controls: profile bindings restored; reloading input bindings.");
			VMManager::ReloadInputBindings(true);
		}
	}

	void ClearSpatialControls()
	{
		ApplySpatialControls(nullptr);
	}

	static void OfferProfileRequirements(const ProfileDB::Profile* profile)
	{
		if (!profile || !profile->requirements.Any())
			return;
		const u32 crc = VMManager::GetDiscCRC();
		if (crc == 0)
			return;
		std::string path = VMManager::GetGameSettingsPath(VMManager::GetSerialForGameSettings(), crc);
		if (!FileSystem::FileExists(path.c_str()))
		{
			std::string legacy = VMManager::GetGameSettingsPath({}, crc);
			if (FileSystem::FileExists(legacy.c_str()))
				path = std::move(legacy);
		}
		ProfileDB::OfferRequirements(profile, crc, EmuConfig.Cpu.ExtraMemory, path);
	}

	void UpdateSettings()
	{
		const Pcsx2Config::VROptions& new_settings = EmuConfig.VR;

		bool enable_changed;
		{
			std::lock_guard lock(s_settings_mutex);
			enable_changed = (s_settings.Enable != new_settings.Enable);
			s_settings = new_settings;
		}

		ProfileDB::ReloadIfChanged();
		const std::string serial = VMManager::GetDiscSerial();
		const ProfileDB::Profile* profile =
			serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());

		OfferProfileRequirements(profile);

		ApplySpatialControls(profile);

		float screen_distance = new_settings.ScreenDistance;
		float screen_height = new_settings.ScreenHeight;
		float screen_arc = new_settings.ScreenArcDeg;
		bool screen_follow_head = false;
		if (profile)
		{
			screen_distance = profile->screen_distance.value_or(screen_distance);
			screen_height = profile->screen_height.value_or(screen_height);
			screen_arc = profile->screen_arc_deg.value_or(screen_arc);
			screen_follow_head = profile->screen_follow_head.value_or(false);
		}
		XRCompositor::UpdateScreenParams(screen_distance, screen_height, screen_arc,
			new_settings.ScreenVerticalOffset, screen_follow_head, new_settings.DisplayFitFill);
		XRCompositor::UpdateScreenAspectRule(new_settings.ScreenFollowsPatchAspect);

		{
			LagMatch::Lag lag;
			u8 axes = 0;
			if (profile && profile->camera.has_value())
			{
				lag = LagMatch::ResolveLag(*profile, GetVerticalFrequency());
				axes = LagMatch::DrivenAxes(profile->camera.value());
			}
			if (lag.ns > 0 && axes == 0)
			{
				Console.Warning("(VR) screen.lagMs: the camera block steers no head axis, so there is nothing to match; off.");
				lag.ns = 0;
			}
			static s64 s_logged_lag_ns = 0;
			if (lag.ns != s_logged_lag_ns)
			{
				s_logged_lag_ns = lag.ns;
				if (lag.ns > 0)
				{
					if (lag.from_frame_age)
						Console.WriteLn("(VR) screen.lagMs: %.1f ms, from the measured frameAge (%u vsyncs; it wins over the profile's number).",
							lag.ms, lag.frame_age);
					else
						Console.WriteLn("(VR) screen.lagMs: %.1f ms (the profile's number; no measured frameAge).", lag.ms);
				}
			}
			XRCompositor::UpdateScreenLag(lag.ns, axes);
		}

		{
			ScreenCurve::Fallback fallback = ScreenCurve::Fallback::Flat;
			ScreenCurve::Force force = ScreenCurve::Force::None;
			const std::optional<ScreenCurve::Fallback> fb = ScreenCurve::ParseFallback(new_settings.ScreenCurveFallback);
			if (fb.has_value())
				fallback = fb.value();
			else
				Console.Warning("(VR) VR/ScreenCurveFallback '%s' is not flat|facets|projector; using flat.",
					new_settings.ScreenCurveFallback.c_str());
			const std::optional<ScreenCurve::Force> fc = ScreenCurve::ParseForce(new_settings.ScreenForceFallback);
			if (fc.has_value())
				force = fc.value();
			else
				Console.Warning("(VR) VR/ScreenForceFallback '%s' is not none|facets|projector; ignoring it.",
					new_settings.ScreenForceFallback.c_str());
			float supersample = new_settings.ScreenProjectorSupersample;
			bool depth = new_settings.ScreenProjectorDepth;
			if (profile)
			{
				fallback = profile->screen_curve_fallback.value_or(fallback);
				force = profile->screen_force_fallback.value_or(force);
				supersample = profile->screen_projector_supersample.value_or(supersample);
				depth = profile->screen_projector_depth.value_or(depth);
			}
			XRCompositor::UpdateScreenCurve(fallback, force, supersample, depth);
		}

		StereoState::Params stereo;
		stereo.separation = new_settings.StereoSeparation;
		stereo.convergence = new_settings.StereoConvergence;
		stereo.uv_policy = StereoState::Params::UvPolicy::Screen;

		bool from_profile = false;
		if (new_settings.StereoUseProfile && profile && profile->stereo)
		{
			stereo.separation = profile->stereo->separation;
			stereo.convergence = profile->stereo->convergence;
			stereo.uv_policy = (profile->stereo->uv_draws == ProfileDB::UvDrawPolicy::World)
									? StereoState::Params::UvPolicy::World
									: StereoState::Params::UvPolicy::Screen;
			stereo.pin_uniform_q = profile->stereo->pin_uniform_q;
			stereo.z_driven_depth = profile->stereo->z_driven_depth;
			CopyCollimate(stereo, *profile->stereo);
			CopyResolvedMap(stereo, profile->stereo->resolved);
			from_profile = true;
		}

		stereo.enabled = StereoGateOpen(new_settings.Enable, LaunchRequestedVR(), StereoRenderArmed(),
			new_settings.StereoMode, from_profile || !new_settings.StereoUseProfile);

		if (MTGS::IsOpen())
			MTGS::RunOnGSThread([stereo]() { StereoState::Publish(stereo); });
		else
			StereoState::Publish(stereo);

		ProfileDB::StereoResolvedMap osd_map;
		if (from_profile)
			osd_map = profile->stereo->resolved;

		static bool s_osd_enabled = false;
		static std::string s_osd_text;
		std::string osd_text;
		if (stereo.enabled)
		{
			const char* provenance = from_profile ? " (game profile)" : "";
			osd_text = (osd_map.map == ProfileDB::StereoMap::Linear)
			               ? fmt::format("Stereo: separation {:.3f}, convergence {:.4g}{}", stereo.separation,
			                     stereo.convergence, provenance)
			               : fmt::format("Stereo: {}{}",
			                     FormatStereoMap(osd_map, stereo.separation, stereo.convergence), provenance);
		}
		if (stereo.enabled != s_osd_enabled || (stereo.enabled && osd_text != s_osd_text))
		{
			if (stereo.enabled)
				Host::AddKeyedOSDMessage("VRStereo", osd_text, 5.0f);
			else if (s_osd_enabled)
				Host::AddKeyedOSDMessage("VRStereo", "Stereo: off", 3.0f);
			s_osd_enabled = stereo.enabled;
			s_osd_text = std::move(osd_text);
		}

		if (enable_changed)
		{
			Console.WriteLn("(VR) VR %s in config; this takes effect when the renderer restarts (Vulkan only).",
				new_settings.Enable ? "enabled" : "disabled");
			if (new_settings.Enable)
				Console.WriteLn("(VR) PenguinScreen2 build %s", BuildVersion::GitRev);
		}

		s_scene_published = -1;
		s_scene_memo_valid = false;
		SplitState::InvalidateMemo();
		s_scene_pending = -1;
		s_scene_pending_count = 0;
	}

	void ApplySceneStereo()
	{
		const Pcsx2Config::VROptions& cfg = EmuConfig.VR;
		if (!StereoGateOpen(cfg.Enable, LaunchRequestedVR(), StereoRenderArmed(), cfg.StereoMode,
				cfg.StereoUseProfile))
		{
			s_scene_published = -1;
			s_scene_memo_valid = false;
			s_scene_pending = -1;
			s_scene_pending_count = 0;
			return;
		}

		ProfileDB::EnsureLoaded();
		const std::string serial = VMManager::GetDiscSerial();
		const ProfileDB::Profile* profile =
			serial.empty() ? nullptr : ProfileDB::Lookup(serial, VMManager::GetDiscCRC());
		if (!profile || !profile->stereo.has_value() || profile->stereo->scenes.empty())
		{
			s_scene_published = -1;
			s_scene_memo_valid = false;
			s_scene_pending = -1;
			s_scene_pending_count = 0;
			return;
		}

		const ProfileDB::StereoParams& base = profile->stereo.value();

		int match = -1;
		for (size_t i = 0; i < base.scenes.size(); i++)
		{
			const ProfileDB::StereoSceneRule& rule = base.scenes[i];
			u32 value = 0;
			switch (rule.width)
			{
				case 1: value = memRead8(rule.ee_address); break;
				case 2: value = memRead16(rule.ee_address); break;
				default: value = memRead32(rule.ee_address); break;
			}
			if (value == rule.equals)
			{
				match = static_cast<int>(i);
				break;
			}
		}

		if (s_scene_memo_valid)
		{
			if (match == s_scene_published)
			{
				s_scene_pending = match;
				s_scene_pending_count = 0;
				return;
			}
			if (match != s_scene_pending)
			{
				s_scene_pending = match;
				s_scene_pending_count = 1;
				return;
			}
			if (++s_scene_pending_count < SCENE_DEBOUNCE_VSYNCS)
				return;
		}

		StereoState::Params stereo;
		stereo.enabled = true;
		stereo.separation = base.separation;
		stereo.convergence = base.convergence;
		stereo.uv_policy = (base.uv_draws == ProfileDB::UvDrawPolicy::World) ?
		                       StereoState::Params::UvPolicy::World :
		                       StereoState::Params::UvPolicy::Screen;
		stereo.pin_uniform_q = base.pin_uniform_q;
		stereo.z_driven_depth = base.z_driven_depth;
		CopyCollimate(stereo, base);
		CopyResolvedMap(stereo, base.resolved);
		if (match >= 0)
		{
			const ProfileDB::StereoSceneRule& rule = base.scenes[static_cast<size_t>(match)];
			stereo.separation = rule.separation.value_or(stereo.separation);
			stereo.convergence = rule.convergence.value_or(stereo.convergence);
			if (rule.map_override.has_value())
				CopyResolvedMap(stereo, *rule.map_override);
		}

		MTGS::RunOnGSThread([stereo]() { StereoState::Publish(stereo); });

		if (match >= 0)
		{
			const ProfileDB::StereoSceneRule& matched = base.scenes[static_cast<size_t>(match)];
			const std::string& label = matched.label;
			const bool scene_map = matched.map_override.has_value();
			const ProfileDB::StereoResolvedMap& eff = scene_map ? *matched.map_override : base.resolved;
			const std::string body =
				(eff.map == ProfileDB::StereoMap::Linear)
					? fmt::format("sep {:.3f}, conv {:.4g}", stereo.separation, stereo.convergence)
					: fmt::format("{} - {}", FormatStereoMap(eff, stereo.separation, stereo.convergence),
						  scene_map ? "scene map" : "base map");
			Host::AddKeyedOSDMessage("VRStereoScene",
				fmt::format("Stereo scene: {} ({})", label.empty() ? "override" : label, body), 3.0f);
		}
		else if (s_scene_memo_valid)
		{
			Host::AddKeyedOSDMessage("VRStereoScene", "Stereo scene: base", 3.0f);
		}

		s_scene_published = match;
		s_scene_memo_valid = true;
		s_scene_pending = match;
		s_scene_pending_count = 0;
	}

	bool EffectiveVREnabled(bool cfg_enable)
	{
		return cfg_enable || LaunchRequestedVR();
	}

	static bool s_launch_requested_vr = false;

	static bool s_stereo_render_armed = false;

	void SetLaunchRequestedVR(bool requested)
	{
		s_launch_requested_vr = requested;
	}

	static int s_launch_seat = 1;

	void SetLaunchSeat(int seat)
	{
		s_launch_seat = seat;
	}

	int GetLaunchSeat()
	{
		return s_launch_seat;
	}

	static int s_seatcast_target = 0;

	void SetSeatCastTarget(int seat)
	{
		s_seatcast_target = seat;
	}

	int SeatCastTarget()
	{
		return s_seatcast_target;
	}

	bool SeatCastArmed()
	{
		return s_seatcast_target > 0;
	}

	static std::string s_launch_seat_runtime_dirs;

	void SetLaunchSeatRuntimeDirs(std::string dirs)
	{
		s_launch_seat_runtime_dirs = std::move(dirs);
	}

	static std::string NthColonEntry(const std::string& reg, int idx)
	{
		size_t pos = 0;
		while (idx > 0 && pos != std::string::npos)
		{
			pos = reg.find(':', pos);
			if (pos != std::string::npos)
				pos++;
			idx--;
		}
		if (pos == std::string::npos)
			return {};
		const size_t end = reg.find(':', pos);
		return reg.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
	}

	static std::string s_launch_seat_runtime_jsons;

	void SetLaunchSeatRuntimeJsons(std::string jsons)
	{
		s_launch_seat_runtime_jsons = std::move(jsons);
	}

	std::string ResolveSeatRuntimeJson(int seat)
	{
		const char* env = std::getenv("XR_RUNTIME_JSON");
		const std::string fallback = env ? env : "";
		if (seat < 2)
			return fallback;
		const bool from_launch = !s_launch_seat_runtime_jsons.empty();
		const std::string json =
			NthColonEntry(from_launch ? s_launch_seat_runtime_jsons : EmuConfig.VR.XrSeatRuntimeJsons, seat - 2);
		if (json.empty())
			return fallback;
		if (FileSystem::FileExists(json.c_str()))
			return json;
		Console.Error("(VR) Seat %d runtime manifest '%s' (from %s) does not exist: seat %d is not started. "
					  "Fix that entry (or drop it: with no entry the seat uses XR_RUNTIME_JSON, '%s').",
			seat, json.c_str(), from_launch ? "--vr-seat-jsons" : "[VR] XrSeatRuntimeJsons", seat,
			fallback.empty() ? "(unset)" : fallback.c_str());
		return {};
	}

	std::string ResolveSeatRuntimeDir(int seat)
	{
		if (seat <= 1)
			return {};
		const std::string& reg = s_launch_seat_runtime_dirs.empty() ?
									 EmuConfig.VR.XrSeatRuntimeDirs :
									 s_launch_seat_runtime_dirs;
		return NthColonEntry(reg, seat - 2);
	}

	bool LaunchRequestedVR()
	{
		return s_launch_requested_vr;
	}

	void SetStereoRenderArmed(bool armed)
	{
		s_stereo_render_armed = armed;
	}

	bool StereoRenderArmed()
	{
		return s_launch_requested_vr || s_stereo_render_armed;
	}

	bool WantsVR()
	{
		if (!LaunchRequestedVR())
			return false;
		std::lock_guard lock(s_settings_mutex);
		return EffectiveVREnabled(s_settings.Enable);
	}

	bool IsSessionActive()
	{
		return XRSession::HasSession();
	}

	SessionStatus GetSessionStatus()
	{
		bool enabled;
		{
			std::lock_guard lock(s_settings_mutex);
			enabled = EffectiveVREnabled(s_settings.Enable);
		}
		if (!enabled)
			return SessionStatus::DisabledInConfig;
		if (!LaunchRequestedVR())
			return SessionStatus::NotLaunchedForVR;
		if (IsSessionActive())
			return SessionStatus::Active;
		return SessionStatus::Inactive;
	}

	void EnsureFrameSubmitted()
	{
		if (!IsSessionActive())
			return;
		GSDeviceVK::GetInstance()->ExecuteCommandBuffer(false);
	}

	void NoteGameVsync(bool new_frame)
	{
		if (XRSession::HasSession())
			XRCompositor::NoteGameVsync(new_frame);
	}

	void EndOfFrame(GSTexture* current)
	{
		if (!XRSession::HasSession())
			return;

		if (SeatCastArmed() && !SeatSession::Running())
			SeatSession::Start(SeatCastTarget());

		XRSession::PumpEvents();

		if (!XRSession::IsLost())
		{
			if (XRCompositor::RuntimeLossDetected())
			{
				Console.Error("(VR) Frame pacer reports the runtime is gone — treating the session as lost.");
				XRSession::MarkLost();
			}
			else if (XRSession::IsSessionRunning())
			{
				const u64 consumed = XRCompositor::GetPacedFramesConsumed();
				if (consumed != s_watchdog_last_consumed)
				{
					s_watchdog_last_consumed = consumed;
					s_watchdog_starved_vsyncs = 0;
				}
				else if (++s_watchdog_starved_vsyncs >= 300)
				{
					Console.Error("(VR) No paced frames for ~5 s while the session is running — treating the runtime as lost.");
					XRSession::MarkLost();
					s_watchdog_starved_vsyncs = 0;
				}
			}
			else
			{
				s_watchdog_starved_vsyncs = 0;
			}
		}

		if (XRSession::IsLost())
		{
			Console.Error("(VR) Session/runtime lost — shutting VR down; flat rendering continues.");
			XRCompositor::Shutdown();
			XRSession::DestroySession();
			XRSession::DestroyInstance();
			return;
		}

		{
			XRCompositor::EndOfFrame(current, XRCompositor::MonoEye);
		}
	}
}
