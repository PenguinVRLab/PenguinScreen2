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
#include "USB/usb-pad/usb-realplay.h"
#include "USB/usb-pad/usb-pad.h"
#include <common/Console.h>

namespace usb_pad
{
	static const USBDescStrings desc_strings = {
		"",
		"In2Games",
		"Real Play"};

	static const uint8_t realplay_racing_dev_descriptor[] = {
		0x12,
		0x01,
		0x00, 0x02,
		0x00,
		0x00,
		0x00,
		0x40,
		0xB7, 0x14,
		0xB2, 0x09,
		0x00, 0x01,
		0x01,
		0x02,
		0x00,
		0x01,
	};

	static const uint8_t realplay_sphere_dev_descriptor[] = {
		0x12,
		0x01,
		0x00, 0x02,
		0x00,
		0x00,
		0x00,
		0x40,
		0xB7, 0x14,
		0xB3, 0x09,
		0x00, 0x01,
		0x01,
		0x02,
		0x00,
		0x01,
	};

	static const uint8_t realplay_golf_dev_descriptor[] = {
		0x12,
		0x01,
		0x00, 0x02,
		0x00,
		0x00,
		0x00,
		0x40,
		0xB7, 0x14,
		0xB5, 0x09,
		0x00, 0x01,
		0x01,
		0x02,
		0x00,
		0x01,
	};

	static const uint8_t realplay_pool_dev_descriptor[] = {
		0x12,
		0x01,
		0x00, 0x02,
		0x00,
		0x00,
		0x00,
		0x40,
		0xB7, 0x14,
		0xB6, 0x09,
		0x00, 0x01,
		0x01,
		0x02,
		0x00,
		0x01,
	};

	static const uint8_t config_descriptor[] = {
		0x09,
		0x02,
		0x29, 0x00,
		0x01,
		0x01,
		0x00,
		0x80,
		0x32,

		0x09,
		0x04,
		0x00,
		0x00,
		0x02,
		0x03,
		0x00,
		0x00,
		0x00,

		0x09,
		0x21,
		0x11, 0x01,
		0x00,
		0x01,
		0x22,
		0x85, 0x00,

		0x07,
		0x05,
		0x02,
		0x03,
		0x40, 0x00,
		0x0A,

		0x07,
		0x05,
		0x81,
		0x03,
		0x40, 0x00,
		0x0A,
	};

	static const uint8_t hid_report_descriptor[] = {
		0x05, 0x01,
		0x09, 0x05,
		0xA1, 0x01,
		0x15, 0x00,
		0x26, 0xFF, 0x0F,
		0x35, 0x00,
		0x46, 0xFF, 0x0F,
		0x09, 0x30,
		0x75, 0x0C,
		0x95, 0x01,
		0x81, 0x02,
		0x75, 0x01,
		0x95, 0x04,
		0x81, 0x03,
		0x09, 0x31,
		0x75, 0x0C,
		0x95, 0x01,
		0x81, 0x02,
		0x75, 0x01,
		0x95, 0x04,
		0x81, 0x03,
		0x09, 0x32,
		0x75, 0x0C,
		0x95, 0x01,
		0x81, 0x02,
		0x75, 0x01,
		0x95, 0x04,
		0x81, 0x03,
		0x06, 0x00, 0xFF,
		0x09, 0x20,
		0x09, 0x21,
		0x09, 0x22,
		0x09, 0x23,
		0x09, 0x24,
		0x09, 0x25,
		0x09, 0x26,
		0x09, 0x27,
		0x26, 0xFF, 0x00,
		0x46, 0xFF, 0x00,
		0x75, 0x08,
		0x95, 0x08,
		0x81, 0x02,
		0x15, 0x00,
		0x25, 0x01,
		0x35, 0x00,
		0x45, 0x01,
		0x75, 0x01,
		0x95, 0x08,
		0x05, 0x09,
		0x19, 0x01,
		0x29, 0x08,
		0x81, 0x02,
		0x06, 0x00, 0xFF,
		0x09, 0x28,
		0x09, 0x29,
		0x09, 0x2A,
		0x09, 0x2B,
		0x26, 0xFF, 0x00,
		0x46, 0xFF, 0x00,
		0x75, 0x08,
		0x95, 0x04,
		0x81, 0x02,
		0xC0,
	};

	RealPlayState::RealPlayState(u32 port_, u32 type_)
		: port(port_)
		, type(type_)
	{
	}

	RealPlayState::~RealPlayState() = default;

	static void realplay_handle_control(USBDevice* dev, USBPacket* p,
		int request, int value, int index, int length, uint8_t* data)
	{
		int ret = 0;

		switch (request)
		{
			case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
				switch (value >> 8)
				{
					case USB_DT_REPORT:
						ret = sizeof(hid_report_descriptor);
						std::memcpy(data, hid_report_descriptor, ret);
						p->actual_length = ret;
						break;
				}
				break;
			case SET_REPORT:
				break;
			case SET_IDLE:
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

	static void realplay_handle_data(USBDevice* dev, USBPacket* p)
	{
		RealPlayState* s = USB_CONTAINER_OF(dev, RealPlayState, dev);

		switch (p->pid)
		{
			case USB_TOKEN_IN:
				if (p->ep->nr == 1)
				{
					pxAssert(p->buffer_size >= sizeof(s->data));

					std::memcpy(p->buffer_ptr, &s->data, sizeof(s->data));

					p->buffer_ptr[0] ^= s->state;
					s->state ^= 1;

					p->actual_length += sizeof(s->data);
				}
				else
				{
					goto fail;
				}
				break;
			case USB_TOKEN_OUT:
				break;
			default:
			fail:
				p->status = USB_RET_STALL;
				break;
		}
	}

	static void realplay_unrealize(USBDevice* dev)
	{
		RealPlayState* s = USB_CONTAINER_OF(dev, RealPlayState, dev);
		delete s;
	}

	const char* RealPlayDevice::Name() const
	{
		return TRANSLATE_NOOP("USB", "RealPlay Device");
	}

	const char* RealPlayDevice::TypeName() const
	{
		return "RealPlay";
	}

	const char* RealPlayDevice::IconName() const
	{
		return ICON_PF_REALPLAY_BOWLING;
	}

	USBDevice* RealPlayDevice::CreateDevice(SettingsInterface& si, u32 port, u32 type) const
	{
		RealPlayState* s = new RealPlayState(port, type);

		s->desc.full = &s->desc_dev;
		s->desc.str = desc_strings;

		const uint8_t* dev_desc = nullptr;
		int dev_desc_len = 0;

		switch (s->type)
		{
			case REALPLAY_RACING:
				dev_desc = realplay_racing_dev_descriptor;
				dev_desc_len = sizeof(realplay_racing_dev_descriptor);
				break;
			case REALPLAY_SPHERE:
				dev_desc = realplay_sphere_dev_descriptor;
				dev_desc_len = sizeof(realplay_sphere_dev_descriptor);
				break;
			case REALPLAY_GOLF:
				dev_desc = realplay_golf_dev_descriptor;
				dev_desc_len = sizeof(realplay_golf_dev_descriptor);
				break;
			case REALPLAY_POOL:
				dev_desc = realplay_pool_dev_descriptor;
				dev_desc_len = sizeof(realplay_pool_dev_descriptor);
				break;
			default:
				pxAssertMsg(false, "Unhandled type");
				break;
		}

		if (usb_desc_parse_dev(dev_desc, dev_desc_len, s->desc, s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(config_descriptor, sizeof(config_descriptor), s->desc_dev) < 0)
			goto fail;

		s->dev.speed = USB_SPEED_FULL;
		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_reset = nullptr;
		s->dev.klass.handle_control = realplay_handle_control;
		s->dev.klass.handle_data = realplay_handle_data;
		s->dev.klass.unrealize = realplay_unrealize;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = nullptr;

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);

		return &s->dev;

	fail:
		realplay_unrealize(&s->dev);
		return nullptr;
	}

	bool RealPlayDevice::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		RealPlayState* s = USB_CONTAINER_OF(dev, RealPlayState, dev);

		if (!sw.DoMarker("RealPlayDevice"))
			return false;

		sw.Do(&s->type);
		sw.Do(&s->state);
		return !sw.HasError();
	}

	float RealPlayDevice::GetBindingValue(const USBDevice* dev, u32 bind_index) const
	{
		RealPlayState* s = USB_CONTAINER_OF(dev, RealPlayState, dev);
		switch (bind_index)
		{
			case CID_RP_DPAD_UP:    return s->data.dpad_up;
			case CID_RP_DPAD_DOWN:  return s->data.dpad_down;
			case CID_RP_DPAD_LEFT:  return s->data.dpad_left;
			case CID_RP_DPAD_RIGHT: return s->data.dpad_right;
			case CID_RP_RED:        return s->data.btn_red;
			case CID_RP_GREEN:      return s->data.btn_green;
			case CID_RP_YELLOW:     return s->data.btn_yellow;
			case CID_RP_BLUE:       return s->data.btn_blue;
			default: 				return 0.0f;
		}
	}

	void RealPlayDevice::SetBindingValue(USBDevice* dev, u32 bind_index, float value) const
	{
		RealPlayState* s = USB_CONTAINER_OF(dev, RealPlayState, dev);
		switch (bind_index)
		{
			case CID_RP_DPAD_UP:
				s->data.dpad_up = (value >= 0.5f);
				break;
			case CID_RP_DPAD_DOWN:
				s->data.dpad_down = (value >= 0.5f);
				break;
			case CID_RP_DPAD_LEFT:
				s->data.dpad_left = (value >= 0.5f);
				break;
			case CID_RP_DPAD_RIGHT:
				s->data.dpad_right = (value >= 0.5f);
				break;
			case CID_RP_RED:
				s->data.btn_red = (value >= 0.5f);
				break;
			case CID_RP_GREEN:
				s->data.btn_green = (value >= 0.5f);
				break;
			case CID_RP_YELLOW:
				s->data.btn_yellow = (value >= 0.5f);
				break;
			case CID_RP_BLUE:
				s->data.btn_blue = (value >= 0.5f);
				break;
			case CID_RP_ACC_X:
				s->data.acc_x = static_cast<u16>(std::clamp<long>(std::lroundf(value * 4095.f), 0, 4095));
				s->data.acc_x = s->invert_x_axis ? 4095 - s->data.acc_x : s->data.acc_x;
				break;
			case CID_RP_ACC_Y:
				s->data.acc_y = static_cast<u16>(std::clamp<long>(std::lroundf(value * 4095.f), 0, 4095));
				s->data.acc_y = s->invert_x_axis ? 4095 - s->data.acc_y : s->data.acc_y;
				break;
			case CID_RP_ACC_Z:
				s->data.acc_z = static_cast<u16>(std::clamp<long>(std::lroundf(value * 4095.f), 0, 4095));
				s->data.acc_z = s->invert_x_axis ? 4095 - s->data.acc_z : s->data.acc_z;
				break;
			default:
				break;
		}
	}

	std::span<const char*> RealPlayDevice::SubTypes() const
	{
		static const char* subtypes[] = {
			TRANSLATE_NOOP("USB", "RealPlay Racing"),
			TRANSLATE_NOOP("USB", "RealPlay Sphere"),
			TRANSLATE_NOOP("USB", "RealPlay Golf"),
			TRANSLATE_NOOP("USB", "RealPlay Pool"),
		};
		return subtypes;
	}

	void RealPlayDevice::UpdateSettings(USBDevice* dev, SettingsInterface& si) const
	{
		RealPlayState* s = USB_CONTAINER_OF(dev, RealPlayState, dev);
		s->invert_x_axis = USB::GetConfigBool(si, s->port, TypeName(), "invert_x_axis", false);
		s->invert_y_axis = USB::GetConfigBool(si, s->port, TypeName(), "invert_y_axis", false);
		s->invert_z_axis = USB::GetConfigBool(si, s->port, TypeName(), "invert_z_axis", false);
	}

	std::span<const InputBindingInfo> RealPlayDevice::Bindings(u32 subtype) const
	{
		static constexpr const InputBindingInfo bindings[] = {
			{"DPadUp", TRANSLATE_NOOP("USB", "D-Pad Up"), nullptr, InputBindingInfo::Type::Button, CID_RP_DPAD_UP, GenericInputBinding::DPadUp},
			{"DPadDown", TRANSLATE_NOOP("USB", "D-Pad Down"), nullptr, InputBindingInfo::Type::Button, CID_RP_DPAD_DOWN, GenericInputBinding::DPadDown},
			{"DPadLeft", TRANSLATE_NOOP("USB", "D-Pad Left"), nullptr, InputBindingInfo::Type::Button, CID_RP_DPAD_LEFT, GenericInputBinding::DPadLeft},
			{"DPadRight", TRANSLATE_NOOP("USB", "D-Pad Right"), nullptr, InputBindingInfo::Type::Button, CID_RP_DPAD_RIGHT, GenericInputBinding::DPadRight},
			{"Red", TRANSLATE_NOOP("USB", "Red"), nullptr, InputBindingInfo::Type::Button, CID_RP_RED, GenericInputBinding::Triangle},
			{"Green", TRANSLATE_NOOP("USB", "Green"), nullptr, InputBindingInfo::Type::Button, CID_RP_GREEN, GenericInputBinding::Cross},
			{"Yellow", TRANSLATE_NOOP("USB", "Yellow"), nullptr, InputBindingInfo::Type::Button, CID_RP_YELLOW, GenericInputBinding::Square},
			{"Blue", TRANSLATE_NOOP("USB", "Blue"), nullptr, InputBindingInfo::Type::Button, CID_RP_BLUE, GenericInputBinding::Circle},
			{"AccelX", TRANSLATE_NOOP("USB", "Accel X"), nullptr, InputBindingInfo::Type::Axis, CID_RP_ACC_X, GenericInputBinding::Unknown},
			{"AccelY", TRANSLATE_NOOP("USB", "Accel Y"), nullptr, InputBindingInfo::Type::Axis, CID_RP_ACC_Y, GenericInputBinding::Unknown},
			{"AccelZ", TRANSLATE_NOOP("USB", "Accel Z"), nullptr, InputBindingInfo::Type::Axis, CID_RP_ACC_Z, GenericInputBinding::Unknown},
		};

		return bindings;
	}

	std::span<const SettingInfo> RealPlayDevice::Settings(u32 subtype) const
	{
		static constexpr const SettingInfo info[] = {
			{SettingInfo::Type::Boolean, "invert_x_axis", TRANSLATE_NOOP("USB", "Invert X axis"), TRANSLATE_NOOP("USB", "Invert X axis"), "false"},
			{SettingInfo::Type::Boolean, "invert_y_axis", TRANSLATE_NOOP("USB", "Invert Y axis"), TRANSLATE_NOOP("USB", "Invert Y axis"), "false"},
			{SettingInfo::Type::Boolean, "invert_z_axis", TRANSLATE_NOOP("USB", "Invert Z axis"), TRANSLATE_NOOP("USB", "Invert Z axis"), "false"},
		};
		return info;
	}
}
