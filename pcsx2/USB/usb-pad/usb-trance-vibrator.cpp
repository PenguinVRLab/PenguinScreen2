// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host.h"
#include "IconsPromptFont.h"
#include "Input/InputManager.h"
#include "StateWrapper.h"
#include "USB/USB.h"
#include "USB/deviceproxy.h"
#include "USB/qemu-usb/USBinternal.h"
#include "USB/qemu-usb/desc.h"
#include "USB/usb-pad/usb-trance-vibrator.h"
#include "USB/usb-pad/usb-pad.h"
#include <common/Console.h>

namespace usb_pad
{
	static const USBDescStrings desc_strings = {
		"",
		"ASCII CORPORATION",
		"ASCII Vib"};

	static uint8_t dev_descriptor[] = {
		0x12,
		0x01,
		0x00, 0x01,
		0x00,
		0x00,
		0x00,
		0x08,
		0x49, 0x0B,
		0x4F, 0x06,
		0x00, 0x01,
		0x01,
		0x02,
		0x00,
		0x01,
	};

	static const uint8_t config_descriptor[] = {
		0x09,
		0x02,
		0x22, 0x00,
		0x01,
		0x01,
		0x00,
		0x80,
		0x31,

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
		0x0A,
	};

	TranceVibratorState::TranceVibratorState(u32 port_)
		: port(port_)
	{
	}

	TranceVibratorState::~TranceVibratorState() = default;

	static void trancevibrator_handle_control(USBDevice* dev, USBPacket* p,
		int request, int value, int index, int length, uint8_t* data)
	{
		TranceVibratorState* s = USB_CONTAINER_OF(dev, TranceVibratorState, dev);
		int ret = 0;

		switch (request)
		{
			case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
				break;
			case SET_IDLE:
				break;
			case VendorDeviceOutRequest:
				InputManager::SetUSBVibrationIntensity(s->port, value & 0xff, 0);
				break;
			default:
				ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
				if (ret >= 0)
				{
					return;
				}
				p->status = USB_RET_STALL;
				break;
		}
	}

	static void trancevibrator_handle_data(USBDevice* dev, USBPacket* p)
	{
		switch (p->pid)
		{
			case USB_TOKEN_IN:
				break;
			case USB_TOKEN_OUT:
				break;
			default:
				p->status = USB_RET_STALL;
				break;
		}
	}

	static void trancevibrator_unrealize(USBDevice* dev)
	{
		TranceVibratorState* s = USB_CONTAINER_OF(dev, TranceVibratorState, dev);
		delete s;
	}

	const char* TranceVibratorDevice::Name() const
	{
		return TRANSLATE_NOOP("USB", "Trance Vibrator (Rez)");
	}

	const char* TranceVibratorDevice::TypeName() const
	{
		return "TranceVibrator";
	}

	const char* TranceVibratorDevice::IconName() const
	{
		return ICON_PF_REZ_VIBRATOR;
	}

	bool TranceVibratorDevice::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		return true;
	}

	USBDevice* TranceVibratorDevice::CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const
	{
		TranceVibratorState* s = new TranceVibratorState(port);

		s->desc.full = &s->desc_dev;
		s->desc.str = desc_strings;

		if (usb_desc_parse_dev(dev_descriptor, sizeof(dev_descriptor), s->desc, s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(config_descriptor, sizeof(config_descriptor), s->desc_dev) < 0)
			goto fail;

		s->dev.speed = USB_SPEED_FULL;
		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_reset = nullptr;
		s->dev.klass.handle_control = trancevibrator_handle_control;
		s->dev.klass.handle_data = trancevibrator_handle_data;
		s->dev.klass.unrealize = trancevibrator_unrealize;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = nullptr;

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);

		return &s->dev;

	fail:
		trancevibrator_unrealize(&s->dev);
		return nullptr;
	}

	float TranceVibratorDevice::GetBindingValue(const USBDevice* dev, u32 bind_index) const
	{
		return 0.0f;
	}

	void TranceVibratorDevice::SetBindingValue(USBDevice* dev, u32 bind_index, float value) const
	{
	}

	std::span<const InputBindingInfo> TranceVibratorDevice::Bindings(u32 subtype) const
	{
		static constexpr const InputBindingInfo bindings[] = {
			{"Motor", TRANSLATE_NOOP("Pad", "Motor"), nullptr, InputBindingInfo::Type::Motor, 0, GenericInputBinding::LargeMotor},
		};

		return bindings;
	}

	std::span<const SettingInfo> TranceVibratorDevice::Settings(u32 subtype) const
	{
		return {};
	}
}
