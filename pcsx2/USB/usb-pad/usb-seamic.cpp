// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host.h"
#include "IconsPromptFont.h"
#include "USB/usb-pad/usb-pad.h"
#include "USB/qemu-usb/desc.h"
#include "USB/usb-mic/usb-mic.h"
#include "USB/USB.h"

#include "common/Console.h"

namespace usb_pad
{

	static const USBDescStrings desc_strings = {
		"",
		"ASCII CORPORATION",
		"ASCII Mic/Joy-stick",
	};

	static const uint8_t dev_descriptor[] = {
 0x12,
 0x01,
 WBVAL(0x0110),
 0x00,
 0x00,
 0x00,
 0x08,
 WBVAL(0x0B49),
 WBVAL(0x0644),
 WBVAL(0x0100),
 0x01,
 0x02,
 0x00,
 0x01,
	};

	static const uint8_t hid_report_descriptor[] = {
		0x05, 0x01,
		0x09, 0x04,
		0xA1, 0x01,
		0x09, 0x01,
		0xA1, 0x00,
		0x95, 0x03,
		0x75, 0x08,
		0x15, 0x00,
		0x26, 0xFF, 0x00,
		0x35, 0x00,
		0x46, 0xFF, 0x00,
		0x66, 0x00, 0x00,
		0x05, 0x01,
		0x09, 0x30,
		0x09, 0x31,
		0x09, 0x32,
		0x81, 0x02,
		0x95, 0x01,
		0x75, 0x04,
		0x15, 0x00,
		0x25, 0x07,
		0x35, 0x00,
		0x46, 0x3B, 0x01,
		0x66, 0x14, 0x00,
		0x09, 0x39,
		0x81, 0x42,
		0x95, 0x0A,
		0x75, 0x01,
		0x15, 0x00,
		0x25, 0x01,
		0x35, 0x00,
		0x45, 0x01,
		0x66, 0x00, 0x00,
		0x05, 0x09,
		0x19, 0x01,
		0x29, 0x0A,
		0x81, 0x02,
		0x95, 0x02,
		0x81, 0x01,
		0x95, 0x08,
		0x75, 0x01,
		0x05, 0x08,
		0x19, 0x01,
		0x29, 0x08,
		0x91, 0x02,
		0xC0,
		0xC0,

	};

	static const uint8_t config_descriptor[] = {
		0x09,
		0x02,
		0x86, 0x00,
		0x03,
		0x01,
		0x00,
		0x80,
		0x31,

		0x09,
		0x04,
		0x00,
		0x00,
		0x00,
		0x01,
		0x01,
		0x00,
		0x00,

		0x09,
		0x24,
		0x01,
		0x00, 0x01,
		WBVAL(38),
		0x01,
		0x01,

		0x0C,
		0x24,
		0x02,
		0x01,
		0x01, 0x02,
		0x02,
		0x01,
		0x00, 0x00,
		0x00,
		0x00,

		0x09,
		0x24,
		0x03,
		0x02,
		0x01, 0x01,
		0x01,
		0x03,
		0x00,

		0x08,
		0x24,
		0x06,
		0x03,
		0x01,
		0x01,
		0x03, 0x00,

		0x09,
		0x04,
		0x01,
		0x00,
		0x00,
		0x01,
		0x02,
		0x00,
		0x00,

		0x09,
		0x04,
		0x01,
		0x01,
		0x01,
		0x01,
		0x02,
		0x00,
		0x00,

		0x07,
		0x24,
		0x01,
		0x02,
		0x01,
		0x01, 0x00,

		0x0E,
		0x24,
		0x02,
		0x01,
		0x01,
		0x02,
		0x10,
		0x02,
		B3VAL(8000),
		B3VAL(11025),

		0x07,
		USB_ENDPOINT_DESCRIPTOR_TYPE,
		USB_ENDPOINT_IN(1),
		0x01,
		WBVAL(100),
		0x01,

		0x07,
		0x25,
		0x01,
		0x01,
		0x00,
		0x00, 0x00,

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x02,
		0x00,
		0x01,
		USB_CLASS_HID,
		0x00,
		0x00,
		0x00,

		0x09,
		USB_DT_HID,
		WBVAL(0x0100),
		0x00,
		0x01,
		USB_DT_REPORT,
		WBVAL(98),

		0x07,
		USB_ENDPOINT_DESCRIPTOR_TYPE,
		USB_ENDPOINT_IN(2),
		USB_ENDPOINT_TYPE_INTERRUPT,
		WBVAL(8),
		0x0A,

	};

	struct SeamicState : public PadState
	{
		explicit SeamicState(u32 port);

		USBDevice* mic;
	};

	SeamicState::SeamicState(u32 port) : PadState(port, WT_SEGA_SEAMIC) {}

	static void pad_handle_data(USBDevice* dev, USBPacket* p)
	{
		SeamicState* s = USB_CONTAINER_OF(dev, SeamicState, dev);
		uint8_t data[64];

		uint8_t devep = p->ep->nr;

		switch (p->pid)
		{
			case USB_TOKEN_IN:
				if (devep == 1)
				{
					s->mic->klass.handle_data(s->mic, p);
				}
				else if (devep == 2)
				{
					const int ret = s->TokenIn(data, p->buffer_size);
					if (ret > 0)
						usb_packet_copy(p, data, std::min((size_t)ret, sizeof(data)));
					else
						p->status = ret;
				}
				else
				{
					goto fail;
				}
				break;
			case USB_TOKEN_OUT:
				usb_packet_copy(p, data, p->buffer_size);
				s->TokenOut(data, p->buffer_size);
				break;
			default:
			fail:
				p->status = USB_RET_STALL;
				break;
		}
	}

	static void pad_handle_reset(USBDevice* dev)
	{
		SeamicState* s = USB_CONTAINER_OF(dev, SeamicState, dev);
		s->Reset();
		s->mic->klass.handle_reset(s->mic);
		return;
	}

	static void pad_handle_control(USBDevice* dev, USBPacket* p, int request, int value,
								   int index, int length, uint8_t* data)
	{
		int ret = 0;

		switch (request)
		{
			case DeviceRequest | USB_REQ_GET_DESCRIPTOR:
				ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
				if (ret < 0)
					goto fail;

				break;
			case InterfaceRequest | USB_REQ_GET_DESCRIPTOR:
				switch (value >> 8)
				{
					case USB_DT_REPORT:
						ret = sizeof(hid_report_descriptor);
						std::memcpy(data, hid_report_descriptor, ret);
						p->actual_length = ret;
						break;
					default:
						goto fail;
				}
				break;
			case SET_REPORT:
				if (length > 0)
				{
					p->actual_length = 0;
				}
				break;
			case SET_IDLE:
				break;
			default:
				ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
				if (ret >= 0)
				{
					return;
				}
			fail:
				p->status = USB_RET_STALL;
				break;
		}
	}

	static void pad_handle_destroy(USBDevice* dev)
	{
		SeamicState* s = USB_CONTAINER_OF(dev, SeamicState, dev);
		s->mic->klass.unrealize(s->mic);
		delete s;
	}

	const char* SeamicDevice::Name() const
	{
		return TRANSLATE_NOOP("USB", "Sega Seamic");
	}

	const char* SeamicDevice::TypeName() const
	{
		return "seamic";
	}

	const char* SeamicDevice::IconName() const
	{
		return ICON_PF_SEGA_SEAMIC;
	}

	std::span<const char*> SeamicDevice::SubTypes() const
	{
		return {};
	}

	std::span<const InputBindingInfo> SeamicDevice::Bindings(u32 subtype) const
	{
		static constexpr const InputBindingInfo bindings[] = {
			{"StickLeft", TRANSLATE_NOOP("USB", "Stick Left"), nullptr, InputBindingInfo::Type::HalfAxis, CID_STEERING_L, GenericInputBinding::LeftStickLeft},
			{"StickRight", TRANSLATE_NOOP("USB", "Stick Right"), nullptr, InputBindingInfo::Type::HalfAxis, CID_STEERING_R, GenericInputBinding::LeftStickRight},
			{"StickUp", TRANSLATE_NOOP("USB", "Stick Up"), nullptr, InputBindingInfo::Type::HalfAxis, CID_THROTTLE, GenericInputBinding::LeftStickUp},
			{"StickDown", TRANSLATE_NOOP("USB", "Stick Down"), nullptr, InputBindingInfo::Type::HalfAxis, CID_BRAKE, GenericInputBinding::LeftStickDown},
			{"A", TRANSLATE_NOOP("USB", "A"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON0, GenericInputBinding::Cross},
			{"B", TRANSLATE_NOOP("USB", "B"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON1, GenericInputBinding::Circle},
			{"C", TRANSLATE_NOOP("USB", "C"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON2, GenericInputBinding::R2},
			{"X", TRANSLATE_NOOP("USB", "X"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON3, GenericInputBinding::Square},
			{"Y", TRANSLATE_NOOP("USB", "Y"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON4, GenericInputBinding::Triangle},
			{"Z", TRANSLATE_NOOP("USB", "Z"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON5, GenericInputBinding::L2},
			{"L", TRANSLATE_NOOP("USB", "L"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON6, GenericInputBinding::L1},
			{"R", TRANSLATE_NOOP("USB", "R"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON7, GenericInputBinding::R1},
			{"Select", TRANSLATE_NOOP("USB", "Select"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON8, GenericInputBinding::Select},
			{"Start", TRANSLATE_NOOP("USB", "Start"), nullptr, InputBindingInfo::Type::Button, CID_BUTTON9, GenericInputBinding::Start},
			{"DPadUp", TRANSLATE_NOOP("USB", "D-Pad Up"), nullptr, InputBindingInfo::Type::Button, CID_DPAD_UP, GenericInputBinding::DPadUp},
			{"DPadDown", TRANSLATE_NOOP("USB", "D-Pad Down"), nullptr, InputBindingInfo::Type::Button, CID_DPAD_DOWN, GenericInputBinding::DPadDown},
			{"DPadLeft", TRANSLATE_NOOP("USB", "D-Pad Left"), nullptr, InputBindingInfo::Type::Button, CID_DPAD_LEFT, GenericInputBinding::DPadLeft},
			{"DPadRight", TRANSLATE_NOOP("USB", "D-Pad Right"), nullptr, InputBindingInfo::Type::Button, CID_DPAD_RIGHT, GenericInputBinding::DPadRight},
		};

		return bindings;
	}

	std::span<const SettingInfo> SeamicDevice::Settings(u32 subtype) const
	{
		static constexpr const SettingInfo info[] = {
			{SettingInfo::Type::StringList, "input_device_name", TRANSLATE_NOOP("USB", "Input Device"),
				TRANSLATE_NOOP("USB", "Selects the device to read audio from."), "", nullptr, nullptr, nullptr, nullptr,
				nullptr, &AudioDevice::GetInputDeviceList},
			{SettingInfo::Type::Integer, "input_latency", TRANSLATE_NOOP("USB", "Input Latency"),
				TRANSLATE_NOOP("USB", "Specifies the latency to the host input device."),
				AudioDevice::DEFAULT_LATENCY_STR, "1", "1000", "1", TRANSLATE_NOOP("USB", "%dms"), nullptr, nullptr, 1.0f},
		};
		return info;
	}

	USBDevice* SeamicDevice::CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const
	{
		const usb_mic::MicrophoneDevice* mic_proxy =
			static_cast<usb_mic::MicrophoneDevice*>(RegisterDevice::instance().Device(DEVTYPE_MICROPHONE));
		if (!mic_proxy)
			return nullptr;

		USBDevice* mic = mic_proxy->CreateDevice(si, port, 0, false, 48000, TypeName());
		if (!mic)
			return nullptr;

		SeamicState* s = new SeamicState(port);

		s->mic = mic;
		s->desc.full = &s->desc_dev;
		s->desc.str = desc_strings;

		if (usb_desc_parse_dev(dev_descriptor, sizeof(dev_descriptor), s->desc, s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(config_descriptor, sizeof(config_descriptor), s->desc_dev) < 0)
			goto fail;

		s->dev.speed = USB_SPEED_FULL;
		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_reset = pad_handle_reset;
		s->dev.klass.handle_control = pad_handle_control;
		s->dev.klass.handle_data = pad_handle_data;
		s->dev.klass.unrealize = pad_handle_destroy;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = s->desc.str[2];
		s->port = port;

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);
		pad_handle_reset(&s->dev);

		return &s->dev;

	fail:
		pad_handle_destroy(&s->dev);
		return nullptr;
	}

	bool SeamicDevice::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		Console.Warning("Not implemented!");
		return true;
	}

}
