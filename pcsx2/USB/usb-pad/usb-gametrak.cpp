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
#include "USB/usb-pad/usb-gametrak.h"
#include "USB/usb-pad/usb-pad.h"
#include <common/Console.h>

namespace usb_pad
{
	static const USBDescStrings desc_strings = {
		"",
		"In2Games Ltd.",
		"Game-Trak V1.3"};

	static uint8_t dev_descriptor[] = {
		0x12,
		0x01,
		0x10, 0x01,
		0x00,
		0x00,
		0x00,
		0x08,
		0xB7, 0x14,
		0x82, 0x09,
		0x01, 0x00,
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
		0x0A,

		0x09,
		0x04,
		0x00,
		0x00,
		0x01,
		0x03,
		0x00,
		0x00,
		0x00,

		0x09,
		0x21,
		0x01, 0x01,
		0x00,
		0x01,
		0x22,
		0x7A, 0x00,

		0x07,
		0x05,
		0x81,
		0x03,
		0x10, 0x00,
		0x0A,
	};

	static const uint8_t hid_report_descriptor[] = {
		0x05, 0x01,
		0x09, 0x04,
		0xA1, 0x01,
		0x09, 0x01,
		0xA1, 0x00,
		0x09, 0x30,
		0x09, 0x31,
		0x09, 0x32,
		0x09, 0x33,
		0x09, 0x34,
		0x09, 0x35,
		0x16, 0x00, 0x00,
		0x26, 0xFF, 0x0F,
		0x36, 0x00, 0x00,
		0x46, 0xFF, 0x0F,
		0x66, 0x00, 0x00,
		0x75, 0x10,
		0x95, 0x06,
		0x81, 0x02,
		0xC0,
		0x09, 0x39,
		0x15, 0x01,
		0x25, 0x08,
		0x35, 0x00,
		0x46, 0x3B, 0x01,
		0x65, 0x14,
		0x75, 0x04,
		0x95, 0x01,
		0x81, 0x02,
		0x05, 0x09,
		0x19, 0x01,
		0x29, 0x0C,
		0x15, 0x00,
		0x25, 0x01,
		0x75, 0x01,
		0x95, 0x0C,
		0x55, 0x00,
		0x65, 0x00,
		0x81, 0x02,
		0x75, 0x08,
		0x95, 0x02,
		0x81, 0x01,
		0x05, 0x08,
		0x09, 0x43,
		0x15, 0x00,
		0x26, 0xFF, 0x00,
		0x35, 0x00,
		0x46, 0xFF, 0x00,
		0x75, 0x08,
		0x95, 0x01,
		0x91, 0x82,
		0x09, 0x44,
		0x91, 0x82,
		0x09, 0x45,
		0x91, 0x82,
		0x09, 0x46,
		0x91, 0x82,
		0xC0,
	};

	GametrakState::GametrakState(u32 port_)
		: port(port_)
	{
	}

	GametrakState::~GametrakState() = default;

	static u32 gametrak_compute_key(u32* key)
	{
		u32 ret = 0;
		ret  = *key <<  2 & 0xFC0000;
		ret |= *key << 17 & 0x020000;
		ret ^= *key << 16 & 0xFE0000;
		ret |= *key       & 0x010000;
		ret |= *key >>  9 & 0x007F7F;
		ret |= *key <<  7 & 0x008080;
		*key = ret;
		return ret >> 16;
	};

	static void gametrak_handle_control(USBDevice* dev, USBPacket* p,
		int request, int value, int index, int length, uint8_t* data)
	{
		GametrakState* s = USB_CONTAINER_OF(dev, GametrakState, dev);
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
			{
				constexpr u8 secret[] = "Gametrak";
				if (length == 8 && std::memcmp(data, secret, sizeof(secret)) == 0)
				{
					s->state = 0;
					s->key = 0;
				}
				else if (length == 2)
				{
					if (data[0] == 0x45)
					{
						s->key = data[1] << 16;
					}

					if ((s->key >> 16) == data[1])
					{
						gametrak_compute_key(&s->key);
					}
					else
					{
						ERROR_LOG("gametrak error : own key = {}, recv key = {}", s->key >> 16, data[1]);
					}
				}
				break;
			}
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

	static void gametrak_handle_data(USBDevice* dev, USBPacket* p)
	{
		GametrakState* s = USB_CONTAINER_OF(dev, GametrakState, dev);

		switch (p->pid)
		{
			case USB_TOKEN_IN:
				if (p->ep->nr == 1)
				{
					pxAssert(p->buffer_size >= sizeof(s->data));

					if (s->state == 0)
					{
						s->state = 1;
						constexpr u8 secret[] = "Gametrak\0\0\0\0\0\0\0\0";
						std::memcpy(p->buffer_ptr, secret, sizeof(secret));
					}
					else
					{
						s->data.k1 = s->key >> 16 & 1;
						s->data.k2 = s->key >> 17 & 1;
						s->data.k3 = s->key >> 18 & 1;
						s->data.k4 = s->key >> 19 & 1;
						s->data.k5 = s->key >> 20 & 1;
						s->data.k6 = s->key >> 21 & 1;

						auto time = std::chrono::steady_clock::now();
						if (std::chrono::duration_cast<std::chrono::milliseconds>(time - s->last_log).count() > 500)
						{
							Log::Write(LOGLEVEL_INFO, Color_Green, "{} LX={} LY={} LZ={} RX={} RY={} RZ={} Btn={}",
								__FUNCTION__,
								(u16)s->data.left_x, (u16)s->data.left_y, (u16)s->data.left_z,
								(u16)s->data.right_x, (u16)s->data.right_y, (u16)s->data.right_z,
								(u8)s->data.button);
							s->last_log = time;
						}

						std::memcpy(p->buffer_ptr, &s->data, sizeof(s->data));
					}

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

	static void gametrak_unrealize(USBDevice* dev)
	{
		GametrakState* s = USB_CONTAINER_OF(dev, GametrakState, dev);
		delete s;
	}

	const char* GametrakDevice::Name() const
	{
		return TRANSLATE_NOOP("USB", "Gametrak Device");
	}

	const char* GametrakDevice::TypeName() const
	{
		return "Gametrak";
	}

	const char* GametrakDevice::IconName() const
	{
		return ICON_PF_GAMETRAK_DEVICE;
	}

	USBDevice* GametrakDevice::CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const
	{
		GametrakState* s = new GametrakState(port);

		s->desc.full = &s->desc_dev;
		s->desc.str = desc_strings;

		if (usb_desc_parse_dev(dev_descriptor, sizeof(dev_descriptor), s->desc, s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(config_descriptor, sizeof(config_descriptor), s->desc_dev) < 0)
			goto fail;

		s->dev.speed = USB_SPEED_FULL;
		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_reset = nullptr;
		s->dev.klass.handle_control = gametrak_handle_control;
		s->dev.klass.handle_data = gametrak_handle_data;
		s->dev.klass.unrealize = gametrak_unrealize;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = nullptr;

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);

		return &s->dev;

	fail:
		gametrak_unrealize(&s->dev);
		return nullptr;
	}

	bool GametrakDevice::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		GametrakState* s = USB_CONTAINER_OF(dev, GametrakState, dev);

		if (!sw.DoMarker("GametrakDevice"))
			return false;

		sw.Do(&s->state);
		sw.Do(&s->key);
		return !sw.HasError();
	}

	float GametrakDevice::GetBindingValue(const USBDevice* dev, u32 bind_index) const
	{
		GametrakState* s = USB_CONTAINER_OF(dev, GametrakState, dev);
		if (bind_index == CID_GT_BUTTON)
			return s->data.button;
		return 0.0f;
	}

	void GametrakDevice::SetBindingValue(USBDevice* dev, u32 bind_index, float value) const
	{
		GametrakState* s = USB_CONTAINER_OF(dev, GametrakState, dev);
		switch (bind_index)
		{
			case CID_GT_BUTTON:
				s->data.button = (value >= 0.5f);
				break;
			case CID_GT_LEFT_X:
				s->data.left_x = static_cast<u16>(std::clamp<long>(std::lroundf(value * 2047.f), 0, 2047));
				s->data.left_x = s->invert_x_axis ? 2047 - s->data.left_x : s->data.left_x;
				break;
			case CID_GT_LEFT_Y:
				s->data.left_y = static_cast<u16>(std::clamp<long>(std::lroundf(value * 2047.f), 0, 2047));
				s->data.left_y = s->invert_y_axis ? 2047 - s->data.left_y : s->data.left_y;
				break;
			case CID_GT_LEFT_Z:
				s->data.left_z = static_cast<u32>(std::clamp<long>(std::lroundf(value * s->limit_z_axis), 0, s->limit_z_axis));
				s->data.left_z = s->invert_z_axis ? s->limit_z_axis - s->data.left_z : s->data.left_z;
				break;
			case CID_GT_RIGHT_X:
				s->data.right_x = static_cast<u16>(std::clamp<long>(std::lroundf(value * 2047.f), 0, 2047));
				s->data.right_x = s->invert_x_axis ? 2047 - s->data.right_x : s->data.right_x;
				break;
			case CID_GT_RIGHT_Y:
				s->data.right_y = static_cast<u16>(std::clamp<long>(std::lroundf(value * 2047.f), 0, 2047));
				s->data.right_y = s->invert_y_axis ? 2047 - s->data.right_y : s->data.right_y;
				break;
			case CID_GT_RIGHT_Z:
				s->data.right_z = static_cast<u32>(std::clamp<long>(std::lroundf(value * s->limit_z_axis), 0, s->limit_z_axis));
				s->data.right_z = s->invert_z_axis ? s->limit_z_axis - s->data.right_z : s->data.right_z;
				break;
			default:
				break;
		}
	}

	void GametrakDevice::UpdateSettings(USBDevice* dev, SettingsInterface& si) const
	{
		GametrakState* s = USB_CONTAINER_OF(dev, GametrakState, dev);
		s->invert_x_axis = USB::GetConfigBool(si, s->port, TypeName(), "invert_x_axis", false);
		s->invert_y_axis = USB::GetConfigBool(si, s->port, TypeName(), "invert_y_axis", false);
		s->invert_z_axis = USB::GetConfigBool(si, s->port, TypeName(), "invert_z_axis", false);
		s->limit_z_axis = USB::GetConfigInt(si, s->port, TypeName(), "limit_z_axis", 4095);
	}

	std::span<const InputBindingInfo> GametrakDevice::Bindings(u32 subtype) const
	{
		static constexpr const InputBindingInfo bindings[] = {
			{"FootPedal", TRANSLATE_NOOP("USB", "Foot Pedal"), nullptr, InputBindingInfo::Type::Button, CID_GT_BUTTON, GenericInputBinding::Cross},
			{"LeftX", TRANSLATE_NOOP("USB", "Left X"), nullptr, InputBindingInfo::Type::Axis, CID_GT_LEFT_X, GenericInputBinding::Unknown},
			{"LeftY", TRANSLATE_NOOP("USB", "Left Y"), nullptr, InputBindingInfo::Type::Axis, CID_GT_LEFT_Y, GenericInputBinding::Unknown},
			{"LeftZ", TRANSLATE_NOOP("USB", "Left Z"), nullptr, InputBindingInfo::Type::Axis, CID_GT_LEFT_Z, GenericInputBinding::Unknown},
			{"RightX", TRANSLATE_NOOP("USB", "Right X"), nullptr, InputBindingInfo::Type::Axis, CID_GT_RIGHT_X, GenericInputBinding::Unknown},
			{"RightY", TRANSLATE_NOOP("USB", "Right Y"), nullptr, InputBindingInfo::Type::Axis, CID_GT_RIGHT_Y, GenericInputBinding::Unknown},
			{"RightZ", TRANSLATE_NOOP("USB", "Right Z"), nullptr, InputBindingInfo::Type::Axis, CID_GT_RIGHT_Z, GenericInputBinding::Unknown},
		};

		return bindings;
	}

	std::span<const SettingInfo> GametrakDevice::Settings(u32 subtype) const
	{
		static constexpr const SettingInfo info[] = {
			{SettingInfo::Type::Boolean, "invert_x_axis", TRANSLATE_NOOP("USB", "Invert X axis"), TRANSLATE_NOOP("USB", "Invert X axis"), "false"},
			{SettingInfo::Type::Boolean, "invert_y_axis", TRANSLATE_NOOP("USB", "Invert Y axis"), TRANSLATE_NOOP("USB", "Invert Y axis"), "false"},
			{SettingInfo::Type::Boolean, "invert_z_axis", TRANSLATE_NOOP("USB", "Invert Z axis"), TRANSLATE_NOOP("USB", "Invert Z axis"), "false"},
			{SettingInfo::Type::Integer, "limit_z_axis", TRANSLATE_NOOP("USB", "Limit Z axis [100-4095]"),
				TRANSLATE_NOOP("USB", "- 4095 for original Gametrak controllers\n- 1790 for standard gamepads"),
				"4095", "100", "4095", "1", TRANSLATE_NOOP("USB", "%d"), nullptr, nullptr, 1.0f},
		};
		return info;
	}
}
