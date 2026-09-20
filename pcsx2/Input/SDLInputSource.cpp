// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Config.h"
#include "Input/SDLInputSource.h"
#include "Input/InputManager.h"
#include "Host.h"

#include "ImGui/FullscreenUI.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "IconsPromptFont.h"

#include <bit>
#include <cmath>
#include <VMManager.h>

static constexpr const char* CONTROLLER_DB_FILENAME = "game_controller_db.txt";

static constexpr const char* s_sdl_axis_setting_names[] = {
	"LeftX",
	"LeftY",
	"RightX",
	"RightY",
	"LeftTrigger",
	"RightTrigger",
};
static_assert(std::size(s_sdl_axis_setting_names) == SDL_GAMEPAD_AXIS_COUNT);

static constexpr const char* s_sdl_axis_names[] = {
	"Left X",
	"Left Y",
	"Right X",
	"Right Y",
};

static constexpr const char* s_sdl_trigger_names[] = {
	"Left Trigger",
	"Right Trigger",
};
static constexpr const char* s_sdl_trigger_ps_names[] = {
	"L2",
	"R2",
};

static const char* const* s_sdl_trigger_names_list[] = {
	s_sdl_trigger_names,
	s_sdl_trigger_names,
	s_sdl_trigger_names,
	s_sdl_trigger_names,
	s_sdl_trigger_ps_names,
	s_sdl_trigger_ps_names,
	s_sdl_trigger_ps_names,
};

static constexpr const char* s_sdl_ps3_sxs_pressure_names[] = {
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	"Cross (Pressure)",
	"Circle (Pressure)",
	"Square (Pressure)",
	"Triangle (Pressure)",
	"L1 (Pressure)",
	"R1 (Pressure)",
	"D-Pad Up (Pressure)",
	"D-Pad Down (Pressure)",
	"D-Pad Left (Pressure)",
	"D-Pad Right (Pressure)",
};

static constexpr const char* s_sdl_axis_icons[][2] = {
	{ICON_PF_LEFT_ANALOG_LEFT, ICON_PF_LEFT_ANALOG_RIGHT},
	{ICON_PF_LEFT_ANALOG_UP, ICON_PF_LEFT_ANALOG_DOWN},
	{ICON_PF_RIGHT_ANALOG_LEFT, ICON_PF_RIGHT_ANALOG_RIGHT},
	{ICON_PF_RIGHT_ANALOG_UP, ICON_PF_RIGHT_ANALOG_DOWN},
};

static constexpr const char* s_sdl_trigger_icons[] = {
	ICON_PF_LEFT_TRIGGER_PULL,
	ICON_PF_RIGHT_TRIGGER_PULL,
};
static constexpr const char* s_sdl_trigger_ps_icons[] = {
	ICON_PF_LEFT_TRIGGER_L2,
	ICON_PF_RIGHT_TRIGGER_R2,
};
static constexpr const char* s_sdl_trigger_nintendo_icons[] = {
	ICON_PF_LEFT_TRIGGER_ZL,
	ICON_PF_RIGHT_TRIGGER_ZR,
};

static const char* const* s_sdl_trigger_icons_list[] = {
	s_sdl_trigger_icons,
	s_sdl_trigger_icons,
	s_sdl_trigger_icons,
	s_sdl_trigger_icons,
	s_sdl_trigger_ps_icons,
	s_sdl_trigger_ps_icons,
	s_sdl_trigger_ps_icons,
	s_sdl_trigger_nintendo_icons,
};

static constexpr const char* s_sdl_ps3_pressure_icons[] = {
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	"P" ICON_PF_BUTTON_CROSS,
	"P" ICON_PF_BUTTON_CIRCLE,
	"P" ICON_PF_BUTTON_SQUARE,
	"P" ICON_PF_BUTTON_TRIANGLE,
	"P" ICON_PF_LEFT_SHOULDER_L1,
	"P" ICON_PF_RIGHT_SHOULDER_R1,
	"P" ICON_PF_DPAD_UP,
	"P" ICON_PF_DPAD_DOWN,
	"P" ICON_PF_DPAD_LEFT,
	"P" ICON_PF_DPAD_RIGHT,
};

static constexpr const GenericInputBinding s_sdl_generic_binding_axis_mapping[][2] = {
	{GenericInputBinding::LeftStickLeft, GenericInputBinding::LeftStickRight},
	{GenericInputBinding::LeftStickUp, GenericInputBinding::LeftStickDown},
	{GenericInputBinding::RightStickLeft, GenericInputBinding::RightStickRight},
	{GenericInputBinding::RightStickUp, GenericInputBinding::RightStickDown},
	{GenericInputBinding::Unknown, GenericInputBinding::L2},
	{GenericInputBinding::Unknown, GenericInputBinding::R2},
};
static constexpr const GenericInputBinding s_sdl_ps3_binding_pressure_mapping[] = {
	GenericInputBinding::Unknown,
	GenericInputBinding::Unknown,
	GenericInputBinding::Unknown,
	GenericInputBinding::Unknown,
	GenericInputBinding::Unknown,
	GenericInputBinding::Unknown,
	GenericInputBinding::Cross,
	GenericInputBinding::Circle,
	GenericInputBinding::Square,
	GenericInputBinding::Triangle,
	GenericInputBinding::L1,
	GenericInputBinding::R1,
	GenericInputBinding::DPadUp,
	GenericInputBinding::DPadDown,
	GenericInputBinding::DPadLeft,
	GenericInputBinding::DPadRight,
};

static constexpr const char* s_sdl_button_setting_names[] = {
	"FaceSouth",
	"FaceEast",
	"FaceWest",
	"FaceNorth",
	"Back",
	"Guide",
	"Start",
	"LeftStick",
	"RightStick",
	"LeftShoulder",
	"RightShoulder",
	"DPadUp",
	"DPadDown",
	"DPadLeft",
	"DPadRight",
	"Misc1",
	"Paddle1",
	"Paddle2",
	"Paddle3",
	"Paddle4",
	"Touchpad",
	"Misc2",
	"Misc3",
	"Misc4",
	"Misc5",
	"Misc6",
};
static_assert(std::size(s_sdl_button_setting_names) == SDL_GAMEPAD_BUTTON_COUNT);

static constexpr const char* s_sdl_face_button_names[] = {
	nullptr,
	"A",
	"B",
	"X",
	"Y",
	"Cross",
	"Circle",
	"Square",
	"Triangle",
};
static constexpr const char* s_sdl_button_names[] = {
	"Face South",
	"Face East",
	"Face West",
	"Face North",
	"Back",
	"Guide",
	"Start",
	"Left Stick",
	"Right Stick",
	"Left Shoulder",
	"Right Shoulder",
	"D-Pad Up",
	"D-Pad Down",
	"D-Pad Left",
	"D-Pad Right",
	"Misc 1",
	"Paddle 1",
	"Paddle 2",
	"Paddle 3",
	"Paddle 4",
	"Touchpad",
	"Misc 2",
	"Misc 3",
	"Misc 4",
	"Misc 5",
	"Misc 6",
};
static constexpr const char* s_sdl_button_ps3_names[] = {
	"Cross",
	"Circle",
	"Square",
	"Triangle",
	"Select",
	"PS",
	"Start",
	"Left Stick",
	"Right Stick",
	"L1",
	"R1",
};
static constexpr const char* s_sdl_button_ps4_names[] = {
	"Cross",
	"Circle",
	"Square",
	"Triangle",
	"Share",
	"PS",
	"Options",
	"Left Stick",
	"Right Stick",
	"L1",
	"R1",
};
static constexpr const char* s_sdl_button_ps5_names[] = {
	"Cross",
	"Circle",
	"Square",
	"Triangle",
	"Create",
	"PS",
	"Options",
	"Left Stick",
	"Right Stick",
	"L1",
	"R1",
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	"Mute",
};

static constexpr const char* const* s_sdl_button_names_list[] = {
	s_sdl_button_names,
	s_sdl_button_names,
	s_sdl_button_names,
	s_sdl_button_names,
	s_sdl_button_ps3_names,
	s_sdl_button_ps4_names,
	s_sdl_button_ps5_names,
};
static constexpr size_t s_sdl_button_namesize_list[] = {
	std::size(s_sdl_button_names),
	std::size(s_sdl_button_names),
	std::size(s_sdl_button_names),
	std::size(s_sdl_button_names),
	std::size(s_sdl_button_ps3_names),
	std::size(s_sdl_button_ps4_names),
	std::size(s_sdl_button_ps5_names),
};

static constexpr const char* s_sdl_face_button_icons[] = {
	nullptr,
	ICON_PF_BUTTON_A,
	ICON_PF_BUTTON_B,
	ICON_PF_BUTTON_X,
	ICON_PF_BUTTON_Y,
	ICON_PF_BUTTON_CROSS,
	ICON_PF_BUTTON_CIRCLE,
	ICON_PF_BUTTON_SQUARE,
	ICON_PF_BUTTON_TRIANGLE,
};
static constexpr const char* s_sdl_button_icons[] = {
	ICON_PF_BUTTON_DOWN_A,
	ICON_PF_BUTTON_RIGHT_B,
	ICON_PF_BUTTON_LEFT_X,
	ICON_PF_BUTTON_UP_Y,
	ICON_PF_SHARE_CAPTURE,
	ICON_PF_XBOX,
	ICON_PF_BURGER_MENU,
	ICON_PF_LEFT_ANALOG_CLICK,
	ICON_PF_RIGHT_ANALOG_CLICK,
	ICON_PF_LEFT_SHOULDER_LB,
	ICON_PF_RIGHT_SHOULDER_RB,
	ICON_PF_XBOX_DPAD_UP,
	ICON_PF_XBOX_DPAD_DOWN,
	ICON_PF_XBOX_DPAD_LEFT,
	ICON_PF_XBOX_DPAD_RIGHT,
};
static constexpr const char* s_sdl_button_ps3_icons[] = {
	ICON_PF_BUTTON_CROSS,
	ICON_PF_BUTTON_CIRCLE,
	ICON_PF_BUTTON_SQUARE,
	ICON_PF_BUTTON_TRIANGLE,
	ICON_PF_SELECT_SHARE,
	ICON_PF_PLAYSTATION,
	ICON_PF_START,
	ICON_PF_LEFT_ANALOG_CLICK,
	ICON_PF_RIGHT_ANALOG_CLICK,
	ICON_PF_LEFT_SHOULDER_L1,
	ICON_PF_RIGHT_SHOULDER_R1,
	ICON_PF_DPAD_UP,
	ICON_PF_DPAD_DOWN,
	ICON_PF_DPAD_LEFT,
	ICON_PF_DPAD_RIGHT,
};
static constexpr const char* s_sdl_button_ps4_icons[] = {
	ICON_PF_BUTTON_CROSS,
	ICON_PF_BUTTON_CIRCLE,
	ICON_PF_BUTTON_SQUARE,
	ICON_PF_BUTTON_TRIANGLE,
	ICON_PF_DUALSHOCK_SHARE,
	ICON_PF_PLAYSTATION,
	ICON_PF_DUALSHOCK_OPTIONS,
	ICON_PF_LEFT_ANALOG_CLICK,
	ICON_PF_RIGHT_ANALOG_CLICK,
	ICON_PF_LEFT_SHOULDER_L1,
	ICON_PF_RIGHT_SHOULDER_R1,
	ICON_PF_DPAD_UP,
	ICON_PF_DPAD_DOWN,
	ICON_PF_DPAD_LEFT,
	ICON_PF_DPAD_RIGHT,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	ICON_PF_DUALSHOCK_TOUCHPAD,
};
static constexpr const char* s_sdl_button_ps5_icons[] = {
	ICON_PF_BUTTON_CROSS,
	ICON_PF_BUTTON_CIRCLE,
	ICON_PF_BUTTON_SQUARE,
	ICON_PF_BUTTON_TRIANGLE,
	ICON_PF_DUALSENSE_SHARE,
	ICON_PF_PLAYSTATION,
	ICON_PF_DUALSENSE_OPTIONS,
	ICON_PF_LEFT_ANALOG_CLICK,
	ICON_PF_RIGHT_ANALOG_CLICK,
	ICON_PF_LEFT_SHOULDER_L1,
	ICON_PF_RIGHT_SHOULDER_R1,
	ICON_PF_DPAD_UP,
	ICON_PF_DPAD_DOWN,
	ICON_PF_DPAD_LEFT,
	ICON_PF_DPAD_RIGHT,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	ICON_PF_DUALSENSE_TOUCHPAD,
};

static constexpr const char* s_sdl_button_nintendo_icons[] = {
	ICON_PF_BUTTON_B,
	ICON_PF_BUTTON_A,
	ICON_PF_BUTTON_Y,
	ICON_PF_BUTTON_X,
	ICON_PF_MINUS,
	ICON_PF_HOME_MENU,
	ICON_PF_PLUS,
	ICON_PF_LEFT_ANALOG_CLICK,
	ICON_PF_RIGHT_ANALOG_CLICK,
	ICON_PF_LEFT_SHOULDER_L,
	ICON_PF_RIGHT_SHOULDER_R,
	ICON_PF_JOYCON_DPAD_UP,
	ICON_PF_JOYCON_DPAD_DOWN,
	ICON_PF_JOYCON_DPAD_LEFT,
	ICON_PF_JOYCON_DPAD_RIGHT,
};
static constexpr const char* const* s_sdl_button_icons_list[] = {
	s_sdl_button_icons,
	s_sdl_button_icons,
	s_sdl_button_icons,
	s_sdl_button_icons,
	s_sdl_button_ps3_icons,
	s_sdl_button_ps4_icons,
	s_sdl_button_ps5_icons,
	s_sdl_button_nintendo_icons,
};
static constexpr size_t s_sdl_button_iconsize_list[] = {
	std::size(s_sdl_button_icons),
	std::size(s_sdl_button_icons),
	std::size(s_sdl_button_icons),
	std::size(s_sdl_button_icons),
	std::size(s_sdl_button_ps3_icons),
	std::size(s_sdl_button_ps4_icons),
	std::size(s_sdl_button_ps5_icons),
	std::size(s_sdl_button_nintendo_icons),
};

static constexpr const GenericInputBinding s_sdl_generic_binding_button_mapping[] = {
	GenericInputBinding::Cross,
	GenericInputBinding::Circle,
	GenericInputBinding::Square,
	GenericInputBinding::Triangle,
	GenericInputBinding::Select,
	GenericInputBinding::System,
	GenericInputBinding::Start,
	GenericInputBinding::L3,
	GenericInputBinding::R3,
	GenericInputBinding::L1,
	GenericInputBinding::R1,
	GenericInputBinding::DPadUp,
	GenericInputBinding::DPadDown,
	GenericInputBinding::DPadLeft,
	GenericInputBinding::DPadRight,
};
static constexpr const GenericInputBinding s_sdl_ps3_binding_button_mapping[] = {
	GenericInputBinding::Unknown,
	GenericInputBinding::Unknown,
	GenericInputBinding::Unknown,
	GenericInputBinding::Unknown,
	GenericInputBinding::Select,
	GenericInputBinding::System,
	GenericInputBinding::Start,
	GenericInputBinding::L3,
	GenericInputBinding::R3,
};

static constexpr const char* s_sdl_hat_direction_names[] = {
	// clang-format off
	"North",
	"East",
	"South",
	"West",
	// clang-format on
};

static constexpr const char* s_sdl_default_led_colors[] = {
	"000080",
	"800000",
	"008000",
	"808000",
};

static void SetGamepadRGBLED(SDL_Gamepad* pad, u32 color)
{
	SDL_SetGamepadLED(pad, (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

static void SDLLogCallback(void* userdata, int category, SDL_LogPriority priority, const char* message)
{
	if (priority >= SDL_LOG_PRIORITY_INFO)
		Console.WriteLn(fmt::format("SDL: {}", message));
	else
		DevCon.WriteLn(fmt::format("SDL: {}", message));
}

SDLInputSource::SDLInputSource() = default;

SDLInputSource::~SDLInputSource()
{
	pxAssert(m_controllers.empty());
}

bool SDLInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	LoadSettings(si);
	settings_lock.unlock();
	SetHints();
	bool result = InitializeSubsystem();
	settings_lock.lock();
	return result;
}

bool SDLInputSource::IsInitialized()
{
	return m_sdl_subsystem_initialized;
}

void SDLInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	const bool old_enable_enhanced_reports = m_enable_enhanced_reports;
	const bool old_enable_ps5_player_leds = m_enable_ps5_player_leds;
	const bool old_use_raw_input = m_use_raw_input;

#ifdef __APPLE__
	const bool old_enable_iokit_driver = m_enable_iokit_driver;
	const bool old_enable_mfi_driver = m_enable_mfi_driver;
#endif

	LoadSettings(si);

#ifdef __APPLE__
	const bool drivers_changed =
		(m_enable_iokit_driver != old_enable_iokit_driver || m_enable_mfi_driver != old_enable_mfi_driver);
#else
	constexpr bool drivers_changed = false;
#endif

	if (m_enable_enhanced_reports != old_enable_enhanced_reports ||
		m_enable_ps5_player_leds != old_enable_ps5_player_leds ||
		m_use_raw_input != old_use_raw_input ||
		drivers_changed)
	{
		settings_lock.unlock();
		ShutdownSubsystem();
		SetHints();
		InitializeSubsystem();
		settings_lock.lock();
	}
}

bool SDLInputSource::ReloadDevices()
{
	PollEvents();
	return false;
}

void SDLInputSource::Shutdown()
{
	ShutdownSubsystem();
}

void SDLInputSource::LoadSettings(SettingsInterface& si)
{
	for (u32 i = 0; i < MAX_LED_COLORS; i++)
	{
		const u32 color = GetRGBForPlayerId(si, i);
		if (m_led_colors[i] == color)
			continue;

		m_led_colors[i] = color;

		const auto it = GetControllerDataForPlayerId(i);
		if (it == m_controllers.end() || !it->gamepad)
			continue;

		const SDL_PropertiesID props = SDL_GetGamepadProperties(it->gamepad);
		if (props == 0)
		{
			ERROR_LOG("SDLInputSource: SDL_GetGamepadProperties() failed");
			continue;
		}

		if (!SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false))
			continue;

		SetGamepadRGBLED(it->gamepad, color);
	}

	m_sdl_hints = si.GetKeyValueList("SDLHints");

	m_enable_enhanced_reports = si.GetBoolValue("InputSources", "SDLControllerEnhancedMode", true);
	m_enable_ps5_player_leds = si.GetBoolValue("InputSources", "SDLPS5PlayerLED", true);
	m_use_raw_input = si.GetBoolValue("InputSources", "SDLRawInput", false);

#ifdef __APPLE__
	m_enable_iokit_driver = si.GetBoolValue("InputSources", "SDLIOKitDriver", true);
	m_enable_mfi_driver = si.GetBoolValue("InputSources", "SDLMFIDriver", true);
#endif
}

u32 SDLInputSource::GetRGBForPlayerId(SettingsInterface& si, u32 player_id)
{
	return ParseRGBForPlayerId(
		si.GetStringValue("SDLExtra", fmt::format("Player{}LED", player_id).c_str(), s_sdl_default_led_colors[player_id]),
		player_id);
}

u32 SDLInputSource::ParseRGBForPlayerId(const std::string_view str, u32 player_id)
{
	if (player_id >= MAX_LED_COLORS)
		return 0;

	const u32 default_color = StringUtil::FromChars<u32>(s_sdl_default_led_colors[player_id], 16).value_or(0);
	const u32 color = StringUtil::FromChars<u32>(str, 16).value_or(default_color);

	return color;
}

void SDLInputSource::ResetRGBForAllPlayers(SettingsInterface& si)
{
	for (u32 player_id = 0; player_id < MAX_LED_COLORS; player_id++)
	{
		si.DeleteValue("SDLExtra", fmt::format("Player{}LED", player_id).c_str());
	}
}

void SDLInputSource::SetHints()
{
	if (const std::string upath = Path::Combine(EmuFolders::DataRoot, CONTROLLER_DB_FILENAME); FileSystem::FileExists(upath.c_str()))
	{
		Console.WriteLn(Color_StrongGreen, fmt::format("SDLInputSource: Using Controller DB from user directory: '{}'", upath));
		SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, upath.c_str());
	}
	else if (const std::string rpath = EmuFolders::GetOverridableResourcePath(CONTROLLER_DB_FILENAME); FileSystem::FileExists(rpath.c_str()))
	{
		Console.WriteLn(Color_StrongGreen, "SDLInputSource: Using Controller DB from resources.");
		SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, rpath.c_str());
	}
	else
	{
		Console.Error(fmt::format("SDLInputSource: Controller DB not found, it should be named '{}'", CONTROLLER_DB_FILENAME));
	}

	SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, m_use_raw_input ? "1" : "0");
	SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, m_enable_enhanced_reports ? "auto" : "0");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, m_enable_ps5_player_leds ? "1" : "0");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
#ifndef _WIN32
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
#else
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER, "1");
#endif

#ifdef __APPLE__
	Console.WriteLnFmt("IOKit is {}, MFI is {}.", m_enable_iokit_driver ? "enabled" : "disabled", m_enable_mfi_driver ? "enabled" : "disabled");
	SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, m_enable_iokit_driver ? "1" : "0");
	SDL_SetHint(SDL_HINT_JOYSTICK_MFI, m_enable_mfi_driver ? "1" : "0");
#endif

	for (const std::pair<std::string, std::string>& hint : m_sdl_hints)
		SDL_SetHint(hint.first.c_str(), hint.second.c_str());
}

bool SDLInputSource::InitializeSubsystem()
{
	if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
	{
		Console.Error("SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC) failed");
		return false;
	}

	SDL_SetLogOutputFunction(SDLLogCallback, nullptr);
#ifdef PCSX2_DEVBUILD
	SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
#else
	SDL_SetLogPriorities(SDL_LOG_PRIORITY_INFO);
#endif

	m_sdl_subsystem_initialized = true;

	int count;
	char** mappings = SDL_GetGamepadMappings(&count);
	if (mappings != nullptr)
	{
		SDL_free(mappings);
		Console.WriteLnFmt(Color_StrongGreen, "SDLInputSource: {} gamepad mappings are loaded.", count);
	}
	else
		Console.Error("SDL_GetGamepadMappings() failed {}", SDL_GetError());

	return true;
}

void SDLInputSource::ShutdownSubsystem()
{
	while (!m_controllers.empty())
		CloseDevice(m_controllers.begin()->joystick_id);

	if (m_sdl_subsystem_initialized)
	{
		SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC);
		m_sdl_subsystem_initialized = false;
	}
}

void SDLInputSource::PollEvents()
{
	for (;;)
	{
		SDL_Event ev;
		if (SDL_PollEvent(&ev))
			ProcessSDLEvent(&ev);
		else
			break;
	}
}

std::vector<std::pair<std::string, std::string>> SDLInputSource::EnumerateDevices()
{
	std::vector<std::pair<std::string, std::string>> ret;

	for (const ControllerData& cd : m_controllers)
	{
		std::string id(StringUtil::StdStringFromFormat("SDL-%d", cd.player_id));

		const char* name = cd.gamepad ? SDL_GetGamepadName(cd.gamepad) : SDL_GetJoystickName(cd.joystick);
		if (name)
			ret.emplace_back(std::move(id), name);
		else
			ret.emplace_back(std::move(id), "Unknown Device");
	}

	return ret;
}

std::optional<InputBindingKey> SDLInputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
	if (!device.starts_with("SDL-") || binding.empty())
		return std::nullopt;

	const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(4));
	if (!player_id.has_value() || player_id.value() < 0)
		return std::nullopt;

	InputBindingKey key = {};
	key.source_type = InputSourceType::SDL;
	key.source_index = static_cast<u32>(player_id.value());

	static constexpr const char* sdl_button_legacy_names[] = {
		"A",
		"B",
		"X",
		"Y",
	};

	for (u32 i = 0; i < std::size(sdl_button_legacy_names); i++)
	{
		if (binding == sdl_button_legacy_names[i])
		{
			key.source_subtype = InputSubclass::ControllerButton;
			key.data = i;

			static constexpr SDL_GamepadButton face_button_pos[] = {
				SDL_GAMEPAD_BUTTON_SOUTH,
				SDL_GAMEPAD_BUTTON_EAST,
				SDL_GAMEPAD_BUTTON_WEST,
				SDL_GAMEPAD_BUTTON_NORTH,
			};

			{
				std::lock_guard lock(m_controllers_key_mutex);

				auto it = GetControllerDataForPlayerId(key.source_index);
				if (it != m_controllers.end() && it->gamepad)
				{
					static bool shown_prompt = false;
					for (u32 pos = 0; pos < std::size(face_button_pos); pos++)
					{
						const SDL_GamepadButtonLabel label = SDL_GetGamepadButtonLabel(it->gamepad, face_button_pos[pos]);
						if (key.data == (label - 1))
						{
							if (key.data != pos)
							{
								if (!shown_prompt)
								{
									shown_prompt = true;
									Host::ReportInfoAsync(TRANSLATE("SDLInputSource", "SDL3 Migration"),
										TRANSLATE("SDLInputSource", "As part of our upgrade to SDL3, we've had to migrate your binds.\n"
																	"Your controller did not match the Xbox layout and may need rebinding.\n"
																	"Please verify your controller settings and amend if required."));

									Host::RunOnCPUThread([] {
										if (!Host::ContainsBaseSettingValue("UI", "SDL2NintendoLayout"))
										{
											Host::SetBaseStringSettingValue("UI", "SDL2NintendoLayout", "auto");
											Host::CommitBaseSettingChanges();
											if (FullscreenUI::IsInitialized())
												FullscreenUI::GamepadLayoutChanged();
										}
									});
								}
								key.data = pos;
							}
							break;
						}
					}

					key.needs_migration = true;
					return key;
				}
				else if (std::find(m_gamepads_needing_migration.begin(), m_gamepads_needing_migration.end(), key.source_index) ==
						 m_gamepads_needing_migration.end())
				{
					m_gamepads_needing_migration.push_back(key.source_index);
					return std::nullopt;
				}
			}
		}
	}

	if (binding.starts_with("+Axis") || binding.starts_with("-Axis"))
	{
		const std::string_view axis_name(binding.substr(1));

		std::string_view end;
		if (auto value = StringUtil::FromChars<u32>(axis_name.substr(4), 10, &end))
		{
			key.source_subtype = InputSubclass::ControllerAxis;
			key.data = *value - 6 + std::size(s_sdl_axis_setting_names);
			key.modifier = (binding[0] == '-') ? InputModifier::Negate : InputModifier::None;
			key.invert = (end == "~");

			key.needs_migration = true;
			return key;
		}
	}
	else if (binding.starts_with("FullAxis"))
	{
		std::string_view end;
		if (auto value = StringUtil::FromChars<u32>(binding.substr(8), 10, &end))
		{
			key.source_subtype = InputSubclass::ControllerAxis;
			key.data = *value - 6 + std::size(s_sdl_axis_setting_names);
			key.modifier = InputModifier::FullAxis;
			key.invert = (end == "~");

			key.needs_migration = true;
			return key;
		}
	}
	else if (binding.starts_with("Button"))
	{
		if (auto value = StringUtil::FromChars<u32>(binding.substr(6)))
		{
			key.source_subtype = InputSubclass::ControllerButton;
			key.data = *value - 21 + std::size(s_sdl_button_setting_names);

			key.needs_migration = true;
			return key;
		}
	}

	if (binding.ends_with("Motor"))
	{
		key.source_subtype = InputSubclass::ControllerMotor;
		if (binding == "LargeMotor")
		{
			key.data = 0;
			return key;
		}
		else if (binding == "SmallMotor")
		{
			key.data = 1;
			return key;
		}
		else
		{
			return std::nullopt;
		}
	}
	else if (binding.ends_with("Haptic"))
	{
		key.source_subtype = InputSubclass::ControllerHaptic;
		key.data = 0;
		return key;
	}
	else if (binding[0] == '+' || binding[0] == '-' || binding.starts_with("Full"))
	{
		const std::string_view axis_name(binding.substr(binding[0] == 'F' ? 4 : 1));

		if (axis_name.starts_with("JoyAxis"))
		{
			std::string_view end;
			if (auto value = StringUtil::FromChars<u32>(axis_name.substr(7), 10, &end))
			{
				key.source_subtype = InputSubclass::ControllerAxis;
				key.data = *value + std::size(s_sdl_axis_setting_names);
				key.modifier = (binding[0] == 'F') ? InputModifier::FullAxis :
				               (binding[0] == '-') ? InputModifier::Negate :
				                                     InputModifier::None;
				key.invert = (end == "~");
				return key;
			}
		}
		for (u32 i = 0; i < std::size(s_sdl_axis_setting_names); i++)
		{
			if (axis_name == s_sdl_axis_setting_names[i])
			{
				key.source_subtype = InputSubclass::ControllerAxis;
				key.data = i;
				key.modifier = (binding[0] == 'F') ? InputModifier::FullAxis :
				               (binding[0] == '-') ? InputModifier::Negate :
				                                     InputModifier::None;
				return key;
			}
		}
	}
	else if (binding.starts_with("Hat"))
	{
		std::string_view hat_dir;
		if (auto value = StringUtil::FromChars<u32>(binding.substr(3), 10, &hat_dir); value.has_value() && !hat_dir.empty())
		{
			for (u8 dir = 0; dir < static_cast<u8>(std::size(s_sdl_hat_direction_names)); dir++)
			{
				if (hat_dir == s_sdl_hat_direction_names[dir])
				{
					key.source_subtype = InputSubclass::ControllerHat;
					key.data = value.value() * std::size(s_sdl_hat_direction_names) + dir;
					return key;
				}
			}
		}
	}
	else
	{
		if (binding.starts_with("JoyButton"))
		{
			if (auto value = StringUtil::FromChars<u32>(binding.substr(9)))
			{
				key.source_subtype = InputSubclass::ControllerButton;
				key.data = *value + std::size(s_sdl_button_setting_names);
				return key;
			}
		}
		for (u32 i = 0; i < std::size(s_sdl_button_setting_names); i++)
		{
			if (binding == s_sdl_button_setting_names[i])
			{
				key.source_subtype = InputSubclass::ControllerButton;
				key.data = i;
				return key;
			}
		}
	}

	return std::nullopt;
}

TinyString SDLInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	TinyString ret;

	if (key.source_type == InputSourceType::SDL)
	{
		if (key.source_subtype == InputSubclass::ControllerAxis)
		{
			const char* modifier = (key.modifier == InputModifier::FullAxis ? (display ? "Full " : "Full") : (key.modifier == InputModifier::Negate ? "-" : "+"));
			if (display)
			{
				std::lock_guard lock(m_controllers_key_mutex);

				SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
				auto it = GetControllerDataForPlayerId(key.source_index);
				if (it != m_controllers.end())
					type = SDL_GetRealGamepadType(it->gamepad);

				if (key.data < std::size(s_sdl_axis_names))
				{
					ret.format("SDL-{} {}{}", static_cast<u32>(key.source_index), modifier, s_sdl_axis_names[key.data]);
				}
				else if (key.data - std::size(s_sdl_axis_names) < std::size(s_sdl_trigger_names))
				{
					const u32 trigger_index = key.data - std::size(s_sdl_axis_names);

					if (type < std::size(s_sdl_trigger_names_list))
						ret.format("SDL-{} {}", static_cast<u32>(key.source_index), s_sdl_trigger_names_list[type][trigger_index]);
					else
						ret.format("SDL-{} {}", static_cast<u32>(key.source_index), s_sdl_trigger_names[trigger_index]);
				}
				else
				{
					bool is_sixaxis = false;
					if (it != m_controllers.end())
						is_sixaxis = IsControllerSixaxis(*it);

					const size_t joy_axis_Index = key.data - std::size(s_sdl_axis_setting_names);

					if (is_sixaxis && key.modifier == InputModifier::FullAxis && key.invert == false &&
						joy_axis_Index < std::size(s_sdl_ps3_sxs_pressure_names) && s_sdl_ps3_sxs_pressure_names[joy_axis_Index] != nullptr)
					{
						ret.format("SDL-{} {}", static_cast<u32>(key.source_index), s_sdl_ps3_sxs_pressure_names[joy_axis_Index]);
					}
					else
						ret.format("SDL-{} {}Axis {}{}", static_cast<u32>(key.source_index), modifier, joy_axis_Index + 1, key.invert ? "~" : "");
				}
			}
			else
			{
				if (key.data < std::size(s_sdl_axis_setting_names))
					ret.format("SDL-{}/{}{}", static_cast<u32>(key.source_index), modifier, s_sdl_axis_setting_names[key.data]);
				else
					ret.format("SDL-{}/{}JoyAxis{}{}", static_cast<u32>(key.source_index), modifier, key.data - std::size(s_sdl_axis_setting_names), (key.invert && (migration || !ShouldIgnoreInversion())) ? "~" : "");
			}
		}
		else if (key.source_subtype == InputSubclass::ControllerButton)
		{
			if (display)
			{
				std::lock_guard lock(m_controllers_key_mutex);

				SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
				auto it = GetControllerDataForPlayerId(key.source_index);
				if (it != m_controllers.end())
					type = SDL_GetRealGamepadType(it->gamepad);

				if (type > SDL_GAMEPAD_TYPE_STANDARD && type < std::size(s_sdl_button_names_list) &&
					key.data < s_sdl_button_namesize_list[type] && s_sdl_button_names_list[type][key.data] != nullptr)
				{
					ret.format("SDL-{} {}", static_cast<u32>(key.source_index), s_sdl_button_names_list[type][key.data]);
				}
				else if (key.data < 4)
				{
					SDL_GamepadButtonLabel label = SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
					if (it != m_controllers.end() && it->gamepad)
						label = SDL_GetGamepadButtonLabel(it->gamepad, static_cast<SDL_GamepadButton>(key.data));

					if (label > SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN && label < std::size(s_sdl_face_button_names))
						ret.format("SDL-{} {}", static_cast<u32>(key.source_index), s_sdl_face_button_names[label]);
					else
						ret.format("SDL-{} {}", static_cast<u32>(key.source_index), s_sdl_button_names[key.data]);
				}
				else if (key.data < std::size(s_sdl_button_names))
					ret.format("SDL-{} {}", static_cast<u32>(key.source_index), s_sdl_button_names[key.data]);
				else
					ret.format("SDL-{} Button {}", static_cast<u32>(key.source_index), key.data - std::size(s_sdl_button_setting_names) + 1);
			}
			else
			{
				if (key.data < std::size(s_sdl_button_setting_names))
					ret.format("SDL-{}/{}", static_cast<u32>(key.source_index), s_sdl_button_setting_names[key.data]);
				else
					ret.format("SDL-{}/JoyButton{}", static_cast<u32>(key.source_index), key.data - std::size(s_sdl_button_setting_names));
			}
		}
		else if (key.source_subtype == InputSubclass::ControllerHat)
		{
			const u32 hat_index = key.data / static_cast<u32>(std::size(s_sdl_hat_direction_names));
			const u32 hat_direction = key.data % static_cast<u32>(std::size(s_sdl_hat_direction_names));
			if (display)
				ret.format("SDL-{} Hat {} {}", static_cast<u32>(key.source_index), hat_index + 1, s_sdl_hat_direction_names[hat_direction]);
			else
				ret.format("SDL-{}/Hat{}{}", static_cast<u32>(key.source_index), hat_index, s_sdl_hat_direction_names[hat_direction]);
		}
		else if (key.source_subtype == InputSubclass::ControllerMotor)
		{
			if (display)
				ret.format("SDL-{} {} Motor", static_cast<u32>(key.source_index), key.data ? "Small" : "Large");
			else
				ret.format("SDL-{}/{}Motor", static_cast<u32>(key.source_index), key.data ? "Small" : "Large");
		}
		else if (key.source_subtype == InputSubclass::ControllerHaptic)
		{
			if (display)
				ret.format("SDL-{} Haptic", static_cast<u32>(key.source_index));
			else
				ret.format("SDL-{}/Haptic", static_cast<u32>(key.source_index));
		}
	}

	return ret;
}

TinyString SDLInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	TinyString ret;

	if (key.source_type == InputSourceType::SDL)
	{
		std::lock_guard lock(m_controllers_key_mutex);

		SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
		auto it = GetControllerDataForPlayerId(key.source_index);
		if (it != m_controllers.end())
			type = SDL_GetRealGamepadType(it->gamepad);

		const InputLayout glyph_preference = InputManager::GetGamepadIconPreference();
		if (glyph_preference == InputLayout::Xbox)
			type = SDL_GAMEPAD_TYPE_XBOXONE;
		else if (glyph_preference == InputLayout::Playstation)
			type = SDL_GAMEPAD_TYPE_PS5;
		else if (glyph_preference == InputLayout::Nintendo)
			type = SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;

		if (key.source_subtype == InputSubclass::ControllerAxis)
		{
			if (key.modifier != InputModifier::FullAxis)
			{
				if (key.data < std::size(s_sdl_axis_icons))
				{
					ret.format("SDL-{}  {}", static_cast<u32>(key.source_index),
						s_sdl_axis_icons[key.data][key.modifier == InputModifier::None]);
				}
				else if (key.data - std::size(s_sdl_axis_icons) < std::size(s_sdl_trigger_icons))
				{
					const u32 trigger_index = key.data - std::size(s_sdl_axis_icons);

					if (type < std::size(s_sdl_trigger_icons_list))
						ret.format("SDL-{}  {}", static_cast<u32>(key.source_index), s_sdl_trigger_icons_list[type][trigger_index]);
					else
						ret.format("SDL-{}  {}", static_cast<u32>(key.source_index), s_sdl_trigger_icons[trigger_index]);
				}
			}
			else if (it != m_controllers.end() && IsControllerSixaxis(*it) && key.invert == false)
			{
				const size_t joy_axis_Index = key.data - std::size(s_sdl_axis_setting_names);

				if (joy_axis_Index < std::size(s_sdl_ps3_pressure_icons) && s_sdl_ps3_pressure_icons[joy_axis_Index] != nullptr)
					ret.format("SDL-{} {}", static_cast<u32>(key.source_index), s_sdl_ps3_pressure_icons[joy_axis_Index]);
			}
		}
		else if (key.source_subtype == InputSubclass::ControllerButton)
		{
			if (type > SDL_GAMEPAD_TYPE_STANDARD && type < std::size(s_sdl_button_icons_list) &&
				key.data < s_sdl_button_iconsize_list[type] && s_sdl_button_icons_list[type][key.data] != nullptr)
			{
				ret.format("SDL-{}  {}", static_cast<u32>(key.source_index), s_sdl_button_icons_list[type][key.data]);
			}
			else if (key.data < 4)
			{
				SDL_GamepadButtonLabel label = SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
				if (it != m_controllers.end() && it->gamepad)
					label = SDL_GetGamepadButtonLabel(it->gamepad, static_cast<SDL_GamepadButton>(key.data));

				if (label > SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN && label < std::size(s_sdl_face_button_icons))
					ret.format("SDL-{}  {}", static_cast<u32>(key.source_index), s_sdl_face_button_icons[label]);
				else
					ret.format("SDL-{}  {}", static_cast<u32>(key.source_index), s_sdl_button_icons[key.data]);
			}
			else if (key.data < std::size(s_sdl_button_icons))
				ret.format("SDL-{}  {}", static_cast<u32>(key.source_index), s_sdl_button_icons[key.data]);
		}
	}

	return ret;
}

bool SDLInputSource::ProcessSDLEvent(const SDL_Event* event)
{
	switch (event->type)
	{
		case SDL_EVENT_GAMEPAD_ADDED:
		{
			Console.WriteLn("SDLInputSource: Gamepad %d inserted", event->gdevice.which);
			OpenDevice(event->gdevice.which, true);
			return true;
		}

		case SDL_EVENT_GAMEPAD_REMOVED:
		{
			Console.WriteLn("SDLInputSource: Gamepad %d removed", event->gdevice.which);
			CloseDevice(event->gdevice.which);
			return true;
		}

		case SDL_EVENT_JOYSTICK_ADDED:
		{
			if (SDL_IsGamepad(event->jdevice.which))
				return false;

			Console.WriteLn("SDLInputSource: Joystick %d inserted", event->jdevice.which);
			OpenDevice(event->jdevice.which, false);
			return true;
		}
		break;

		case SDL_EVENT_JOYSTICK_REMOVED:
		{
			if (auto it = GetControllerDataForJoystickId(event->jdevice.which); it != m_controllers.end() && it->gamepad)
				return false;

			Console.WriteLn("SDLInputSource: Joystick %d removed", event->jdevice.which);
			CloseDevice(event->jdevice.which);
			return true;
		}

		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			return HandleGamepadAxisEvent(&event->gaxis);

		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
			return HandleGamepadButtonEvent(&event->gbutton);

		case SDL_EVENT_JOYSTICK_AXIS_MOTION:
			return HandleJoystickAxisEvent(&event->jaxis);

		case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
		case SDL_EVENT_JOYSTICK_BUTTON_UP:
			return HandleJoystickButtonEvent(&event->jbutton);

		case SDL_EVENT_JOYSTICK_HAT_MOTION:
			return HandleJoystickHatEvent(&event->jhat);

		default:
			return false;
	}
}

SDL_Joystick* SDLInputSource::GetJoystickForDevice(const std::string_view device)
{
	if (!device.starts_with("SDL-"))
		return nullptr;

	const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(4));
	if (!player_id.has_value() || player_id.value() < 0)
		return nullptr;

	auto it = GetControllerDataForPlayerId(player_id.value());
	if (it == m_controllers.end())
		return nullptr;

	return it->joystick;
}

SDLInputSource::ControllerDataVector::iterator SDLInputSource::GetControllerDataForJoystickId(SDL_JoystickID id)
{
	return std::find_if(m_controllers.begin(), m_controllers.end(), [id](const ControllerData& cd) { return cd.joystick_id == id; });
}

SDLInputSource::ControllerDataVector::iterator SDLInputSource::GetControllerDataForPlayerId(int id)
{
	return std::find_if(m_controllers.begin(), m_controllers.end(), [id](const ControllerData& cd) { return cd.player_id == id; });
}

int SDLInputSource::GetFreePlayerId() const
{
	for (int player_id = 0;; player_id++)
	{
		size_t i;
		for (i = 0; i < m_controllers.size(); i++)
		{
			if (m_controllers[i].player_id == player_id)
				break;
		}
		if (i == m_controllers.size())
			return player_id;
	}

	return 0;
}

bool SDLInputSource::OpenDevice(SDL_JoystickID index, bool is_gamepad)
{
	SDL_Gamepad* gamepad;
	SDL_Joystick* joystick;

	if (is_gamepad)
	{
		gamepad = SDL_OpenGamepad(index);
		joystick = gamepad ? SDL_GetGamepadJoystick(gamepad) : nullptr;
	}
	else
	{
		gamepad = nullptr;
		joystick = SDL_OpenJoystick(index);
	}

	if (!gamepad && !joystick)
	{
		ERROR_LOG("SDLInputSource: Failed to open controller {}", index);
		return false;
	}

	const SDL_JoystickID joystick_id = SDL_GetJoystickID(joystick);
	int player_id = gamepad ? SDL_GetGamepadPlayerIndex(gamepad) : SDL_GetJoystickPlayerIndex(joystick);
	for (auto it = m_controllers.begin(); it != m_controllers.end(); ++it)
	{
		if (it->joystick_id == joystick_id)
		{
			ERROR_LOG("SDLInputSource: Controller {}, instance {}, player {} already connected, ignoring.", index, joystick_id, player_id);
			if (gamepad)
				SDL_CloseGamepad(gamepad);
			else
				SDL_CloseJoystick(joystick);

			return false;
		}
	}

	if (player_id < 0 || GetControllerDataForPlayerId(player_id) != m_controllers.end())
	{
		const int free_player_id = GetFreePlayerId();
		WARNING_LOG("SDLInputSource: Controller {} (joystick {}) returned player ID {}, which is invalid or in "
					"use. Using ID {} instead.",
			index, joystick_id, player_id, free_player_id);
		player_id = free_player_id;
	}

	const char* name = gamepad ? SDL_GetGamepadName(gamepad) : SDL_GetJoystickName(joystick);
	if (!name)
		name = "Unknown Device";

	INFO_LOG("SDLInputSource: Opened {} {} (instance id {}, player id {}): {}", is_gamepad ? "gamepad" : "joystick",
		index, joystick_id, player_id, name);

	ControllerData cd = {};
	cd.player_id = player_id;
	cd.joystick_id = joystick_id;
	cd.haptic_left_right_effect = -1;
	cd.gamepad = gamepad;
	cd.joystick = joystick;

	if (gamepad)
	{
		int binding_count;
		SDL_GamepadBinding** bindings = SDL_GetGamepadBindings(gamepad, &binding_count);
		if (bindings)
		{
			const int num_axes = SDL_GetNumJoystickAxes(joystick);
			const int num_buttons = SDL_GetNumJoystickButtons(joystick);
			cd.joy_axis_used_in_pad.resize(num_axes, false);
			cd.joy_button_used_in_pad.resize(num_buttons, false);
			auto mark_bind = [&](SDL_GamepadBinding* bind) {
				if (bind->input_type == SDL_GAMEPAD_BINDTYPE_AXIS && bind->input.axis.axis < num_axes)
					cd.joy_axis_used_in_pad[bind->input.axis.axis] = true;
				if (bind->input_type == SDL_GAMEPAD_BINDTYPE_BUTTON && bind->input.button < num_buttons)
					cd.joy_button_used_in_pad[bind->input.button] = true;
			};

			for (int i = 0; i < binding_count; i++)
				mark_bind(bindings[i]);

			SDL_free(bindings);

			INFO_LOG("SDLInputSource: Gamepad {} has {} axes and {} buttons", player_id, num_axes, num_buttons);
		}
		else
			ERROR_LOG("SDLInputSource: Failed to get gamepad bindings {}", SDL_GetError());
	}
	else
	{
		const int num_hats = SDL_GetNumJoystickHats(joystick);
		if (num_hats > 0)
			cd.last_hat_state.resize(static_cast<size_t>(num_hats), u8{0});

		INFO_LOG("SDLInputSource: Joystick {} has {} axes, {} buttons and {} hats", player_id,
			SDL_GetNumJoystickAxes(joystick), SDL_GetNumJoystickButtons(joystick), num_hats);
	}

	cd.use_gamepad_rumble = (gamepad && SDL_RumbleGamepad(gamepad, 0, 0, 0));
	if (cd.use_gamepad_rumble)
	{
		INFO_LOG("SDLInputSource: Rumble is supported on '{}' via gamepad", name);
	}
	else
	{
		SDL_Haptic* haptic = SDL_OpenHapticFromJoystick(joystick);
		if (haptic)
		{
			SDL_HapticEffect ef = {};
			ef.leftright.type = SDL_HAPTIC_LEFTRIGHT;
			ef.leftright.length = 1000;

			int ef_id = SDL_CreateHapticEffect(haptic, &ef);
			if (ef_id >= 0)
			{
				cd.haptic = haptic;
				cd.haptic_left_right_effect = ef_id;
			}
			else
			{
				ERROR_LOG("SDLInputSource: Failed to create haptic left/right effect: {}", SDL_GetError());
				if (SDL_HapticRumbleSupported(haptic) && SDL_InitHapticRumble(haptic))
				{
					cd.haptic = haptic;
				}
				else
				{
					ERROR_LOG("SDLInputSource: No haptic rumble supported: {}", SDL_GetError());
					SDL_CloseHaptic(haptic);
				}
			}
		}

		if (cd.haptic)
			INFO_LOG("SDLInputSource: Rumble is supported on '{}' via haptic", name);
	}

	if (!cd.haptic && !cd.use_gamepad_rumble)
		WARNING_LOG("SDLInputSource: Rumble is not supported on '{}'", name);

	if (gamepad)
	{
		const SDL_PropertiesID props = SDL_GetGamepadProperties(gamepad);
		bool hasLED = false;
		if (props == 0)
			ERROR_LOG("SDLInputSource: SDL_GetGamepadProperties() failed");
		else
			hasLED = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);

		if (player_id >= 0 && static_cast<u32>(player_id) < MAX_LED_COLORS && hasLED)
		{
			SetGamepadRGBLED(gamepad, m_led_colors[player_id]);
		}
	}

	{
		std::unique_lock lock(m_controllers_key_mutex);
		m_controllers.push_back(std::move(cd));

		if (gamepad)
		{
			auto idx = std::find(m_gamepads_needing_migration.begin(), m_gamepads_needing_migration.end(), player_id);
			if (idx != m_gamepads_needing_migration.end())
			{
				m_gamepads_needing_migration.erase(idx);

				lock.unlock();
				VMManager::ReloadInputBindings(true);
			}
		}
	}

	InputManager::OnInputDeviceConnected(fmt::format("SDL-{}", player_id), name);
	return true;
}

bool SDLInputSource::CloseDevice(SDL_JoystickID joystick_index)
{
	auto it = GetControllerDataForJoystickId(joystick_index);
	if (it == m_controllers.end())
		return false;

	{
		std::lock_guard lock(m_controllers_key_mutex);
		InputManager::OnInputDeviceDisconnected(
			{InputBindingKey{.source_type = InputSourceType::SDL, .source_index = static_cast<u32>(it->player_id)}},
			fmt::format("SDL-{}", it->player_id));

		if (it->haptic)
			SDL_CloseHaptic(it->haptic);

		if (it->gamepad)
			SDL_CloseGamepad(it->gamepad);
		else
			SDL_CloseJoystick(it->joystick);

		m_controllers.erase(it);
	}

	return true;
}

static float NormalizeS16(s16 value)
{
	return static_cast<float>(value) / (value < 0 ? 32768.0f : 32767.0f);
}

bool SDLInputSource::HandleGamepadAxisEvent(const SDL_GamepadAxisEvent* ev)
{
	auto it = GetControllerDataForJoystickId(ev->which);
	if (it == m_controllers.end())
		return false;

	const InputBindingKey key(MakeGenericControllerAxisKey(InputSourceType::SDL, it->player_id, ev->axis));
	const float value = NormalizeS16(ev->value);

	if (ev->axis < std::size(s_sdl_generic_binding_axis_mapping))
	{
		InputManager::InvokeEvents(key, value, GenericInputBinding::Unknown,
			s_sdl_generic_binding_axis_mapping[ev->axis][0],
			s_sdl_generic_binding_axis_mapping[ev->axis][1]);
	}
	else
	{
		InputManager::InvokeEvents(key, value);
	}

	return true;
}

bool SDLInputSource::HandleGamepadButtonEvent(const SDL_GamepadButtonEvent* ev)
{
	auto it = GetControllerDataForJoystickId(ev->which);
	if (it == m_controllers.end())
		return false;

	const InputBindingKey key(MakeGenericControllerButtonKey(InputSourceType::SDL, it->player_id, ev->button));
	const GenericInputBinding generic_key = (ev->button < std::size(s_sdl_generic_binding_button_mapping)) ?
	                                            s_sdl_generic_binding_button_mapping[ev->button] :
	                                            GenericInputBinding::Unknown;
	InputManager::InvokeEvents(key, static_cast<float>(ev->down), generic_key);
	return true;
}

bool SDLInputSource::HandleJoystickAxisEvent(const SDL_JoyAxisEvent* ev)
{
	auto it = GetControllerDataForJoystickId(ev->which);
	if (it == m_controllers.end())
		return false;
	if (ev->axis < it->joy_axis_used_in_pad.size() && it->joy_axis_used_in_pad[ev->axis])
		return false;
	const u32 axis = ev->axis + std::size(s_sdl_axis_setting_names);
	const InputBindingKey key(MakeGenericControllerAxisKey(InputSourceType::SDL, it->player_id, axis));
	InputManager::InvokeEvents(key, NormalizeS16(ev->value));
	return true;
}

bool SDLInputSource::HandleJoystickButtonEvent(const SDL_JoyButtonEvent* ev)
{
	auto it = GetControllerDataForJoystickId(ev->which);
	if (it == m_controllers.end())
		return false;
	if (ev->button < it->joy_button_used_in_pad.size() && it->joy_button_used_in_pad[ev->button])
		return false;
	const u32 button = ev->button + std::size(s_sdl_button_setting_names);
	const InputBindingKey key(MakeGenericControllerButtonKey(InputSourceType::SDL, it->player_id, button));
	InputManager::InvokeEvents(key, static_cast<float>(ev->down));
	return true;
}

bool SDLInputSource::HandleJoystickHatEvent(const SDL_JoyHatEvent* ev)
{
	auto it = GetControllerDataForJoystickId(ev->which);
	if (it == m_controllers.end() || ev->hat >= it->last_hat_state.size())
		return false;

	const u8 last_direction = it->last_hat_state[ev->hat];
	it->last_hat_state[ev->hat] = ev->value;

	u8 changed_direction = last_direction ^ ev->value;
	while (changed_direction != 0)
	{
		const u8 pos = std::countr_zero(changed_direction);
		const u8 mask = (1u << pos);
		changed_direction &= ~mask;

		const InputBindingKey key(
			MakeGenericControllerHatKey(InputSourceType::SDL, it->player_id, ev->hat, pos, std::size(s_sdl_hat_direction_names)));
		InputManager::InvokeEvents(key, (last_direction & mask) ? 0.0f : 1.0f);
	}

	return true;
}

std::vector<InputBindingKey> SDLInputSource::EnumerateMotors()
{
	std::vector<InputBindingKey> ret;

	InputBindingKey key = {};
	key.source_type = InputSourceType::SDL;

	for (ControllerData& cd : m_controllers)
	{
		key.source_index = cd.player_id;

		if (cd.use_gamepad_rumble || cd.haptic_left_right_effect)
		{
			key.source_subtype = InputSubclass::ControllerMotor;
			key.data = 0;
			ret.push_back(key);
			key.data = 1;
			ret.push_back(key);
		}
		else if (cd.haptic)
		{
			key.source_subtype = InputSubclass::ControllerHaptic;
			key.data = 0;
			ret.push_back(key);
		}
	}

	return ret;
}

bool SDLInputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	if (!device.starts_with("SDL-"))
		return false;

	const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(4));
	if (!player_id.has_value() || player_id.value() < 0)
		return false;

	ControllerDataVector::iterator it = GetControllerDataForPlayerId(player_id.value());
	if (it == m_controllers.end())
		return false;

	if (it->gamepad)
	{
		const s32 pid = player_id.value();
		for (u32 i = 0; i < std::size(s_sdl_generic_binding_axis_mapping); i++)
		{
			const GenericInputBinding negative = s_sdl_generic_binding_axis_mapping[i][0];
			const GenericInputBinding positive = s_sdl_generic_binding_axis_mapping[i][1];
			if (negative != GenericInputBinding::Unknown)
				mapping->emplace_back(negative, fmt::format("SDL-{}/-{}", pid, s_sdl_axis_setting_names[i]));

			if (positive != GenericInputBinding::Unknown)
				mapping->emplace_back(positive, fmt::format("SDL-{}/+{}", pid, s_sdl_axis_setting_names[i]));
		}

		if (IsControllerSixaxis(*it))
		{
			for (u32 i = 0; i < std::size(s_sdl_ps3_binding_pressure_mapping); i++)
			{
				const GenericInputBinding binding = s_sdl_ps3_binding_pressure_mapping[i];
				if (binding != GenericInputBinding::Unknown)
					mapping->emplace_back(binding, fmt::format("SDL-{}/FullJoyAxis{}", pid, i));
			}

			for (u32 i = 0; i < std::size(s_sdl_ps3_binding_button_mapping); i++)
			{
				const GenericInputBinding binding = s_sdl_ps3_binding_button_mapping[i];
				if (binding != GenericInputBinding::Unknown)
					mapping->emplace_back(binding, fmt::format("SDL-{}/{}", pid, s_sdl_button_setting_names[i]));
			}
		}
		else
		{
			for (u32 i = 0; i < std::size(s_sdl_generic_binding_button_mapping); i++)
			{
				const GenericInputBinding binding = s_sdl_generic_binding_button_mapping[i];
				if (binding != GenericInputBinding::Unknown)
					mapping->emplace_back(binding, fmt::format("SDL-{}/{}", pid, s_sdl_button_setting_names[i]));
			}
		}

		if (it->use_gamepad_rumble || it->haptic_left_right_effect)
		{
			mapping->emplace_back(GenericInputBinding::SmallMotor, fmt::format("SDL-{}/SmallMotor", pid));
			mapping->emplace_back(GenericInputBinding::LargeMotor, fmt::format("SDL-{}/LargeMotor", pid));
		}
		else
		{
			mapping->emplace_back(GenericInputBinding::SmallMotor, fmt::format("SDL-{}/Haptic", pid));
			mapping->emplace_back(GenericInputBinding::LargeMotor, fmt::format("SDL-{}/Haptic", pid));
		}

		return true;
	}
	else
	{
		return false;
	}
}

InputLayout SDLInputSource::GetControllerLayout(u32 index)
{
	auto it = GetControllerDataForPlayerId(index);
	if (it == m_controllers.end())
		return InputLayout::Unknown;

	// clang-format off
	switch (SDL_GetGamepadButtonLabel(it->gamepad, SDL_GAMEPAD_BUTTON_EAST))
	{
		case SDL_GAMEPAD_BUTTON_LABEL_B:      return InputLayout::Xbox;
		case SDL_GAMEPAD_BUTTON_LABEL_A:      return InputLayout::Nintendo;
		case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return InputLayout::Playstation;
		default:                              return InputLayout::Unknown;
	}
	// clang-format on
}

void SDLInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
	if (key.source_subtype != InputSubclass::ControllerMotor && key.source_subtype != InputSubclass::ControllerHaptic)
		return;

	auto it = GetControllerDataForPlayerId(key.source_index);
	if (it == m_controllers.end())
		return;

	it->rumble_intensity[key.data] = static_cast<u16>(intensity * 65535.0f);
	SendRumbleUpdate(&(*it));
}

void SDLInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
	if (large_key.source_index != small_key.source_index || large_key.source_subtype != InputSubclass::ControllerMotor ||
		small_key.source_subtype != InputSubclass::ControllerMotor)
	{
		UpdateMotorState(large_key, large_intensity);
		UpdateMotorState(small_key, small_intensity);
		return;
	}

	auto it = GetControllerDataForPlayerId(large_key.source_index);
	if (it == m_controllers.end())
		return;

	it->rumble_intensity[large_key.data] = static_cast<u16>(large_intensity * 65535.0f);
	it->rumble_intensity[small_key.data] = static_cast<u16>(small_intensity * 65535.0f);
	SendRumbleUpdate(&(*it));
}

void SDLInputSource::SendRumbleUpdate(ControllerData* cd)
{
	static constexpr u32 DURATION = 65535;

	if (cd->use_gamepad_rumble)
	{
		SDL_RumbleGamepad(cd->gamepad, cd->rumble_intensity[0], cd->rumble_intensity[1], DURATION);
		return;
	}

	if (cd->haptic_left_right_effect >= 0)
	{
		if ((static_cast<u32>(cd->rumble_intensity[0]) + static_cast<u32>(cd->rumble_intensity[1])) > 0)
		{
			SDL_HapticEffect ef;
			ef.type = SDL_HAPTIC_LEFTRIGHT;
			ef.leftright.large_magnitude = cd->rumble_intensity[0];
			ef.leftright.small_magnitude = cd->rumble_intensity[1];
			ef.leftright.length = DURATION;
			SDL_UpdateHapticEffect(cd->haptic, cd->haptic_left_right_effect, &ef);
			SDL_RunHapticEffect(cd->haptic, cd->haptic_left_right_effect, SDL_HAPTIC_INFINITY);
		}
		else
		{
			SDL_StopHapticEffect(cd->haptic, cd->haptic_left_right_effect);
		}
	}
	else
	{
		const float strength = static_cast<float>(std::max(cd->rumble_intensity[0], cd->rumble_intensity[1])) * (1.0f / 65535.0f);
		if (strength > 0.0f)
			SDL_PlayHapticRumble(cd->haptic, strength, DURATION);
		else
			SDL_StopHapticRumble(cd->haptic);
	}
}

bool SDLInputSource::IsControllerSixaxis(const ControllerData& cd)
{
	const SDL_GamepadType type = SDL_GetRealGamepadType(cd.gamepad);

	return type == SDL_GAMEPAD_TYPE_PS3 &&
	       SDL_GetNumJoystickAxes(cd.joystick) == 16 &&
	       SDL_GetNumJoystickButtons(cd.joystick) == 11;
}
