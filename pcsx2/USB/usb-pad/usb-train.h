// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "USB/deviceproxy.h"
#include "USB/qemu-usb/qusb.h"
#include "USB/qemu-usb/desc.h"

namespace usb_pad
{
	enum TrainDeviceTypes
	{
		TRAIN_TYPE2,
		TRAIN_SHINKANSEN,
		TRAIN_RYOJOUHEN,
		TRAIN_MASCON,
		MASTER_CONTROLLER,
	};

	class TrainDevice final : public DeviceProxy
	{
	public:
		USBDevice* CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const override;
		const char* Name() const override;
		const char* TypeName() const override;
		const char* IconName() const override;
		std::span<const char*> SubTypes() const override;
		void UpdateSettings(USBDevice* dev, SettingsInterface& si) const override;
		std::span<const SettingInfo> Settings(u32 subtype) const override;
		float GetBindingValue(const USBDevice* dev, u32 bind_index) const override;
		void SetBindingValue(USBDevice* dev, u32 bind_index, float value) const override;
		std::span<const InputBindingInfo> Bindings(u32 subtype) const override;
		bool Freeze(USBDevice* dev, StateWrapper& sw) const override;
	};

#pragma pack(push, 1)
	struct TrainConData_Type2
	{
		u8 control;
		u8 brake;
		u8 power;
		u8 horn;
		u8 hat;
		u8 buttons;
	};
	static_assert(sizeof(TrainConData_Type2) == 6);

	struct TrainConData_Shinkansen
	{
		u8 brake;
		u8 power;
		u8 horn;
		u8 hat;
		u8 buttons;
		u8 pad;
	};
	static_assert(sizeof(TrainConData_Shinkansen) == 6);

	struct TrainConData_Ryojouhen
	{
		u8 brake;
		u8 power;
		u8 horn;
		u8 hat;
		u8 buttons;
		u8 pad[3];
	};
	static_assert(sizeof(TrainConData_Ryojouhen) == 8);

	struct TrainConData_TrainMascon
	{
		u8 one;

		u8 handle : 4;
		u8 reverser : 4;

		u8 ats : 1;
		u8 close : 1;
		u8 button_a_soft : 1;
		u8 button_a_hard : 1;
		u8 button_b : 1;
		u8 button_c : 1;
		u8 : 2;

		u8 start : 1;
		u8 select : 1;
		u8 dpad_up : 1;
		u8 dpad_down : 1;
		u8 dpad_left : 1;
		u8 dpad_right : 1;
		u8 : 2;
	};
	static_assert(sizeof(TrainConData_TrainMascon) == 4);
#pragma pack(pop)

	struct TrainDeviceState
	{
		TrainDeviceState(u32 port_, TrainDeviceTypes type_);
		~TrainDeviceState();

		void Reset();
		void UpdateHatSwitch() noexcept;
		void UpdateHandles(u8 max_power, u8 max_brake);

		USBDevice dev{};
		USBDesc desc{};
		USBDescDevice desc_dev{};

		u32 port = 0;
		TrainDeviceTypes type = TRAIN_TYPE2;
		bool passthrough = false;

		struct
		{
			bool hat_left : 1;
			bool hat_right : 1;
			bool hat_up : 1;
			bool hat_down : 1;

			u8 power;
			u8 brake;
			u8 hatswitch;
			u16 buttons;
		} data = {};

		const char* mc_handle[16] = {"TSB20", "TSB30", "TSB40", "TSE99", "TSA05", "TSA15", "TSA25", "TSA35", "TSA45", "TSA50", "TSA55", "TSA65", "TSA75", "TSA85", "TSA95", "TSB60"};
		const char* mc_reverser[3] = {"TSG00", "TSG50", "TSG99"};
		const char* mc_button_pressed[4] = {"TSY99", "TSX99", "TSZ99", "TSK99"};
		const char* mc_button_released[4] = {"TSY00", "TSX00", "TSZ00", "TSK00"};
		u8 power_notches;
		u8 brake_notches;

		u16 prev_buttons;
		s8 last_handle = -1, handle = 0;
		s8 last_reverser = -1, reverser = 1;
	};

#define DEFINE_DCT_DEV_DESCRIPTOR(prefix, subclass, product) \
	static const uint8_t prefix##_dev_descriptor[] = { \
 USB_DEVICE_DESC_SIZE, \
 USB_DEVICE_DESCRIPTOR_TYPE, \
 WBVAL(0x0110), \
 0xFF, \
 subclass, \
 0x00, \
 0x08, \
 WBVAL(0x0ae4), \
 WBVAL(product), \
 WBVAL(0x0102), \
 0x01, \
 0x02, \
 0x03, \
 0x01, \
	}

	static const uint8_t taito_denshacon_config_descriptor[] = {
		USB_CONFIGURATION_DESC_SIZE,
		USB_CONFIGURATION_DESCRIPTOR_TYPE,
		WBVAL(25),
		0x01,
		0x01,
		0x00,
		0xA0,
		0xFA,

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x00,
		0x00,
		0x01,
		USB_CLASS_HID,
		0x00,
		0x00,
		0x00,

		USB_ENDPOINT_DESC_SIZE,
		USB_ENDPOINT_DESCRIPTOR_TYPE,
		USB_ENDPOINT_IN(1),
		USB_ENDPOINT_TYPE_INTERRUPT,
		WBVAL(8),
		0x14,
	};

	static const USBDescStrings dct01_desc_strings = {
		"",
		"TAITO",
		"TAITO_DENSYA_CON_T01",
		"TCPP20009",
	};

	DEFINE_DCT_DEV_DESCRIPTOR(dct01, 0x04, 0x0004);

	static const USBDescStrings dct02_desc_strings = {
		"",
		"TAITO",
		"TAITO_DENSYA_CON_T02",
		"TCPP20011",
	};

	DEFINE_DCT_DEV_DESCRIPTOR(dct02, 0x05, 0x0005);

	static const USBDescStrings dct03_desc_strings = {
		"",
		"TAITO",
		"TAITO_DENSYA_CON_T03",
		"TCPP20014",
	};

	DEFINE_DCT_DEV_DESCRIPTOR(dct03, 0xFF, 0x0007);

	static const uint8_t train_mascon_dev_descriptor[] = {
		0x12,
		0x01,
		0x10, 0x01,
		0x00,
		0x00,
		0x00,
		0x08,
		0x06, 0x1C,
		0xA7, 0x77,
		0x02, 0x02,
		0x01,
		0x02,
		0x03,
		0x01,
	};

	static const uint8_t train_mascon_config_descriptor[] = {
		0x09,
		0x02,
		0x19, 0x00,
		0x01,
		0x01,
		0x04,
		0xA0,
		0x32,

		0x09,
		0x04,
		0x00,
		0x00,
		0x01,
		0x00,
		0x00,
		0x00,
		0x00,

		0x07,
		0x05,
		0x81,
		0x03,
		0x08, 0x00,
		0x14,
	};

	static const uint8_t master_controller_dev_descriptor[] = {
		0x12,
		0x01,
		0x10, 0x01,
		0x00,
		0x00,
		0x00,
		0x40,
		0x7B, 0x06,
		0x03, 0x23,
		0x00, 0x03,
		0x01,
		0x02,
		0x00,
		0x01,
	};

	static const uint8_t master_controller_config_descriptor[] = {
		0x09,
		0x02,
		0x27, 0x00,
		0x01,
		0x01,
		0x00,
		0x80,
		0x32,

		0x09,
		0x04,
		0x00,
		0x00,
		0x03,
		0xFF,
		0x00,
		0x00,
		0x00,

		0x07,
		0x05,
		0x81,
		0x03,
		0x0A, 0x00,
		0x01,

		0x07,
		0x05,
		0x02,
		0x02,
		0x40, 0x00,
		0x00,

		0x07,
		0x05,
		0x83,
		0x02,
		0x40, 0x00,
		0x00,
	};

}
