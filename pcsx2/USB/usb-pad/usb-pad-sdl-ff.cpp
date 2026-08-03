// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Input/InputManager.h"
#include "USB/usb-pad/usb-pad-sdl-ff.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <algorithm>

namespace usb_pad
{
	SDLFFDevice::SDLFFDevice(SDL_Haptic* haptic)
		: m_haptic(haptic)
	{
		std::memset(&m_constant_effect, 0, sizeof(m_constant_effect));
		std::memset(&m_spring_effect, 0, sizeof(m_spring_effect));
		std::memset(&m_damper_effect, 0, sizeof(m_damper_effect));
		std::memset(&m_friction_effect, 0, sizeof(m_friction_effect));
	}

	SDLFFDevice::~SDLFFDevice()
	{
		if (m_haptic)
		{
			DestroyEffects();

			SDL_CloseHaptic(m_haptic);
			m_haptic = nullptr;
		}
	}

	std::unique_ptr<SDLFFDevice> SDLFFDevice::Create(const std::string_view device)
	{
		SDLInputSource* source = static_cast<SDLInputSource*>(InputManager::GetInputSourceInterface(InputSourceType::SDL));
		if (!source)
			return nullptr;

		SDL_Joystick* joystick = source->GetJoystickForDevice(device);
		if (!joystick)
		{
			Console.Error(fmt::format("No SDL_Joystick for {}. Cannot use FF.", device));
			return nullptr;
		}

		SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(joystick);
		if (!haptic)
		{
			Console.Error(fmt::format("Haptic is not supported on {}.", device));
			return nullptr;
		}

		std::unique_ptr<SDLFFDevice> ret(new SDLFFDevice(haptic));
		ret->CreateEffects(device);
		return ret;
	}

	void SDLFFDevice::CreateEffects(const std::string_view device)
	{
		constexpr u32 length = SDL_HAPTIC_INFINITY;

		const unsigned int supported = SDL_GetHapticFeatures(m_haptic);
		if (supported & SDL_HAPTIC_CONSTANT)
		{
			m_constant_effect.type = SDL_HAPTIC_CONSTANT;
			m_constant_effect.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
			m_constant_effect.constant.length = length;

			m_constant_effect_id = SDL_CreateHapticEffect(m_haptic, &m_constant_effect);
			if (m_constant_effect_id < 0)
				Console.Error("SDL_CreateHapticEffect() for constant failed: %s", SDL_GetError());
		}
		else
		{
			Console.Warning(fmt::format("(SDLFFDevice) Constant effect is not supported on '{}'", device));
		}

		if (supported & SDL_HAPTIC_SPRING)
		{
			m_spring_effect.type = SDL_HAPTIC_SPRING;
			m_spring_effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
			m_spring_effect.condition.length = length;

			m_spring_effect_id = SDL_CreateHapticEffect(m_haptic, &m_spring_effect);
			if (m_spring_effect_id < 0)
				Console.Error("SDL_CreateHapticEffect() for spring failed: %s", SDL_GetError());
		}
		else
		{
			Console.Warning(fmt::format("(SDLFFDevice) Spring effect is not supported on '{}'", device));
		}

		if (supported & SDL_HAPTIC_DAMPER)
		{
			m_damper_effect.type = SDL_HAPTIC_DAMPER;
			m_damper_effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
			m_damper_effect.condition.length = length;

			m_damper_effect_id = SDL_CreateHapticEffect(m_haptic, &m_damper_effect);
			if (m_damper_effect_id < 0)
				Console.Error("SDL_CreateHapticEffect() for damper failed: %s", SDL_GetError());
		}
		else
		{
			Console.Warning(fmt::format("(SDLFFDevice) Damper effect is not supported on '{}'", device));
		}

		if (supported & SDL_HAPTIC_FRICTION)
		{
			m_friction_effect.type = SDL_HAPTIC_FRICTION;
			m_friction_effect.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
			m_friction_effect.condition.length = length;

			m_friction_effect_id = SDL_CreateHapticEffect(m_haptic, &m_friction_effect);
			if (m_friction_effect_id < 0)
				Console.Error("SDL_CreateHapticEffect() for friction failed: %s", SDL_GetError());
		}
		else
		{
			Console.Warning(fmt::format("(SDLFFDevice) Friction effect is not supported on '{}'", device));
		}

		m_autocenter_supported = (supported & SDL_HAPTIC_AUTOCENTER) != 0;
		if (!m_autocenter_supported)
			Console.Warning(fmt::format("(SDLFFDevice) Autocenter effect is not supported on '{}'", device));
	}

	void SDLFFDevice::DestroyEffects()
	{
		if (m_friction_effect_id >= 0)
		{
			if (m_friction_effect_running)
			{
				SDL_StopHapticEffect(m_haptic, m_friction_effect_id);
				m_friction_effect_running = false;
			}
			SDL_DestroyHapticEffect(m_haptic, m_friction_effect_id);
			m_friction_effect_id = -1;
		}

		if (m_damper_effect_id >= 0)
		{
			if (m_damper_effect_running)
			{
				SDL_StopHapticEffect(m_haptic, m_damper_effect_id);
				m_damper_effect_running = false;
			}
			SDL_DestroyHapticEffect(m_haptic, m_damper_effect_id);
			m_damper_effect_id = -1;
		}

		if (m_spring_effect_id >= 0)
		{
			if (m_spring_effect_running)
			{
				SDL_StopHapticEffect(m_haptic, m_spring_effect_id);
				m_spring_effect_running = false;
			}
			SDL_DestroyHapticEffect(m_haptic, m_spring_effect_id);
			m_spring_effect_id = -1;
		}

		if (m_constant_effect_id >= 0)
		{
			if (m_constant_effect_running)
			{
				SDL_StopHapticEffect(m_haptic, m_constant_effect_id);
				m_constant_effect_running = false;
			}
			SDL_DestroyHapticEffect(m_haptic, m_constant_effect_id);
			m_constant_effect_id = -1;
		}
	}

	void SDLFFDevice::SetConstantForce(int level)
	{
		if (m_constant_effect_id < 0)
			return;

		const s16 new_level = static_cast<s16>(std::clamp(level, -32768, 32767));
		if (m_constant_effect.constant.level != new_level)
		{
			m_constant_effect.constant.level = new_level;
			if (!SDL_UpdateHapticEffect(m_haptic, m_constant_effect_id, &m_constant_effect))
				Console.Warning("SDL_UpdateHapticEffect() for constant failed: %s", SDL_GetError());
		}

		if (!m_constant_effect_running || use_ffb_dropout_workaround)
		{
			if (SDL_RunHapticEffect(m_haptic, m_constant_effect_id, SDL_HAPTIC_INFINITY))
				m_constant_effect_running = true;
			else
				Console.Error("SDL_RunHapticEffect() for constant failed: %s", SDL_GetError());
		}
	}

	template <typename T>
	static u16 ClampU16(T val)
	{
		return static_cast<u16>(std::clamp<T>(val, 0, 65535));
	}

	template <typename T>
	static u16 ClampS16(T val)
	{
		return static_cast<s16>(std::clamp<T>(val, -32768, 32767));
	}

	void SDLFFDevice::SetSpringForce(const parsed_ff_data& ff)
	{
		if (m_spring_effect_id < 0)
			return;

		m_spring_effect.condition.left_sat[0] = ClampU16(ff.u.condition.left_saturation);
		m_spring_effect.condition.left_coeff[0] = ClampS16(ff.u.condition.left_coeff);
		m_spring_effect.condition.right_sat[0] = ClampU16(ff.u.condition.right_saturation);
		m_spring_effect.condition.right_coeff[0] = ClampS16(ff.u.condition.right_coeff);
		m_spring_effect.condition.deadband[0] = ClampU16(ff.u.condition.deadband);
		m_spring_effect.condition.center[0] = ClampS16(ff.u.condition.center);

		if (!SDL_UpdateHapticEffect(m_haptic, m_spring_effect_id, &m_spring_effect))
			Console.Warning("SDL_UpdateHapticEffect() for spring failed: %s", SDL_GetError());

		if (!m_spring_effect_running)
		{
			if (SDL_RunHapticEffect(m_haptic, m_spring_effect_id, SDL_HAPTIC_INFINITY))
				m_spring_effect_running = true;
			else
				Console.Error("SDL_RunHapticEffect() for spring failed: %s", SDL_GetError());
		}
	}

	void SDLFFDevice::SetDamperForce(const parsed_ff_data& ff)
	{
		if (m_damper_effect_id < 0)
			return;

		m_damper_effect.condition.left_sat[0] = ClampU16(ff.u.condition.left_saturation);
		m_damper_effect.condition.left_coeff[0] = ClampS16(ff.u.condition.left_coeff);
		m_damper_effect.condition.right_sat[0] = ClampU16(ff.u.condition.right_saturation);
		m_damper_effect.condition.right_coeff[0] = ClampS16(ff.u.condition.right_coeff);
		m_damper_effect.condition.deadband[0] = ClampU16(ff.u.condition.deadband);
		m_damper_effect.condition.center[0] = ClampS16(ff.u.condition.center);

		if (!SDL_UpdateHapticEffect(m_haptic, m_damper_effect_id, &m_damper_effect))
			Console.Warning("SDL_UpdateHapticEffect() for damper failed: %s", SDL_GetError());

		if (!m_damper_effect_running)
		{
			if (SDL_RunHapticEffect(m_haptic, m_damper_effect_id, SDL_HAPTIC_INFINITY))
				m_damper_effect_running = true;
			else
				Console.Error("SDL_RunHapticEffect() for damper failed: %s", SDL_GetError());
		}
	}

	void SDLFFDevice::SetFrictionForce(const parsed_ff_data& ff)
	{
		if (m_friction_effect_id < 0)
			return;

		m_friction_effect.condition.left_sat[0] = ClampU16(ff.u.condition.left_saturation);
		m_friction_effect.condition.left_coeff[0] = ClampS16(ff.u.condition.left_coeff);
		m_friction_effect.condition.right_sat[0] = ClampU16(ff.u.condition.right_saturation);
		m_friction_effect.condition.right_coeff[0] = ClampS16(ff.u.condition.right_coeff);
		m_friction_effect.condition.deadband[0] = ClampU16(ff.u.condition.deadband);
		m_friction_effect.condition.center[0] = ClampS16(ff.u.condition.center);

		if (!SDL_UpdateHapticEffect(m_haptic, m_friction_effect_id, &m_friction_effect))
		{
			if (!m_friction_effect_running && SDL_RunHapticEffect(m_haptic, m_friction_effect_id, SDL_HAPTIC_INFINITY))
				m_friction_effect_running = true;
			else
				Console.Error("SDL_UpdateHapticEffect() for friction failed: %s", SDL_GetError());
		}
	}

	void SDLFFDevice::SetAutoCenter(int value)
	{
		if (m_autocenter_supported)
		{
			if (!SDL_SetHapticAutocenter(m_haptic, value))
				Console.Warning("SDL_SetHapticAutocenter() failed: %s", SDL_GetError());
		}
	}

	void SDLFFDevice::DisableForce(EffectID force)
	{
		switch (force)
		{
			case EFF_CONSTANT:
			{
				if (m_constant_effect_running)
				{
					SDL_StopHapticEffect(m_haptic, m_constant_effect_id);
					m_constant_effect_running = false;
				}
			}
			break;

			case EFF_SPRING:
			{
				if (m_spring_effect_running)
				{
					SDL_StopHapticEffect(m_haptic, m_spring_effect_id);
					m_spring_effect_running = false;
				}
			}
			break;

			case EFF_DAMPER:
			{
				if (m_damper_effect_running)
				{
					SDL_StopHapticEffect(m_haptic, m_damper_effect_id);
					m_damper_effect_running = false;
				}
			}
			break;

			case EFF_FRICTION:
			{
				if (m_friction_effect_running)
				{
					SDL_StopHapticEffect(m_haptic, m_friction_effect_id);
					m_friction_effect_running = false;
				}
			}
			break;

			case EFF_RUMBLE:
				break;

			default:
				break;
		}
	}

}
