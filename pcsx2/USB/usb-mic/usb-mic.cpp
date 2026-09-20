/*
 * QEMU USB HID devices
 *
 * Copyright (c) 2005 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Most stuff is based on Qemu 1.7 USB soundcard passthrough code.

#include "IconsPromptFont.h"
#include "USB/qemu-usb/qusb.h"
#include "USB/qemu-usb/desc.h"
#include "USB/qemu-usb/USBinternal.h"
#include "USB/usb-mic/usb-mic.h"
#include "USB/usb-mic/audiodev.h"
#include "USB/usb-mic/audiodev-noop.h"
#include "USB/usb-mic/audiodev-cubeb.h"
#include "USB/usb-mic/audio.h"
#include "USB/USB.h"
#include "Host.h"
#include "StateWrapper.h"

#include "common/Console.h"

#include "fmt/format.h"

static FILE* file = NULL;

#define BUFFER_FRAMES 200

#define USBAUDIO_PACKET_SIZE 200
#define USBAUDIO_SAMPLE_RATE 48000
#define USBAUDIO_PACKET_INTERVAL 1

namespace usb_mic
{

	enum usb_audio_altset : int8_t
	{
		ALTSET_OFF = 0x00,
		ALTSET_ON = 0x01,
	};

	struct SINGSTARMICState
	{
		USBDevice dev;

		USBDesc desc;
		USBDescDevice desc_dev;

		std::unique_ptr<AudioDevice> audsrc[2];

		struct freeze
		{
			int intf;
			MicMode mode;

			enum usb_audio_altset altset;
			bool mute;
			uint8_t vol[2];
			uint32_t srate[2];
		} f;

		std::vector<int16_t> buffer[2];
	};

	static const USBDescStrings singstar_desc_strings = {
		"",
		"Nam Tai E&E Products Ltd.",
		"USBMIC",
		"310420811",
	};

	static const USBDescStrings logitech_desc_strings = {
		"",
		"Logitech",
		"USBMIC",
	};

	static const USBDescStrings ak5370_desc_strings = {
		"",
		"AKM",
		"AK5370"
	};

	static const uint8_t singstar_dev_descriptor[] = {
 0x12,
 0x01,
 WBVAL(0x0110),
 0x00,
 0x00,
 0x00,
 0x08,
 WBVAL(0x1415),
 WBVAL(0x0000),
 WBVAL(0x0001),
 0x01,
 0x02,
 0x00,
 0x01,
	};

	static const uint8_t singstar_config_descriptor[] = {
		0x09,
		USB_CONFIGURATION_DESCRIPTOR_TYPE,
		WBVAL(0x00b1),
		0x02,
		0x01,
		0x00,
		USB_CONFIG_BUS_POWERED,
		USB_CONFIG_POWER_MA(90),

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x00,
		0x00,
		0x00,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOCONTROL,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		AUDIO_CONTROL_INTERFACE_DESC_SZ(1),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_HEADER,
		WBVAL(0x0100),
		WBVAL(0x0028),
		0x01,
		0x01,

		AUDIO_INPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_INPUT_TERMINAL,
		0x01,
		WBVAL(AUDIO_TERMINAL_MICROPHONE),
		0x02,
		0x02,
		WBVAL(AUDIO_CHANNEL_L | AUDIO_CHANNEL_R),
		0x00,
		0x00,

		AUDIO_OUTPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_OUTPUT_TERMINAL,
		0x02,
		WBVAL(AUDIO_TERMINAL_USB_STREAMING),
		0x01,
		0x03,
		0x00,

		AUDIO_FEATURE_UNIT_DESC_SZ(2, 1),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_FEATURE_UNIT,
		0x03,
		0x01,
		0x01,
		0x01,
		0x02,
		0x02,
		0x00,

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x01,
		0x00,
		0x00,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOSTREAMING,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x01,
		0x01,
		0x01,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOSTREAMING,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		AUDIO_STREAMING_INTERFACE_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_GENERAL,
		0x02,
		0x01,
		WBVAL(AUDIO_FORMAT_PCM),

		AUDIO_FORMAT_TYPE_I_DESC_SZ(5),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_FORMAT_TYPE,
		AUDIO_FORMAT_TYPE_I,
		0x01,
		0x02,
		0x10,
		0x05,
		B3VAL(8000),
		B3VAL(11025),
		B3VAL(22050),
		B3VAL(44100),
		B3VAL(48000),

		AUDIO_STANDARD_ENDPOINT_DESC_SIZE,
		USB_ENDPOINT_DESCRIPTOR_TYPE,
		USB_ENDPOINT_IN(1),
		USB_ENDPOINT_TYPE_ISOCHRONOUS | USB_ENDPOINT_SYNC_ASYNCHRONOUS,
		WBVAL(0x0064),
		0x01,
		0x00,
		0x00,

		AUDIO_STREAMING_ENDPOINT_DESC_SIZE,
		AUDIO_ENDPOINT_DESCRIPTOR_TYPE,
		AUDIO_ENDPOINT_GENERAL,
		0x01,
		0x00,
		WBVAL(0x0000),

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x01,
		0x02,
		0x01,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOSTREAMING,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		AUDIO_STREAMING_INTERFACE_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_GENERAL,
		0x02,
		0x01,
		WBVAL(AUDIO_FORMAT_PCM),

		AUDIO_FORMAT_TYPE_I_DESC_SZ(5),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_FORMAT_TYPE,
		AUDIO_FORMAT_TYPE_I,
		0x02,
		0x02,
		0x10,
		0x05,
		B3VAL(8000),
		B3VAL(11025),
		B3VAL(22050),
		B3VAL(44100),
		B3VAL(48000),

		AUDIO_STANDARD_ENDPOINT_DESC_SIZE,
		USB_ENDPOINT_DESCRIPTOR_TYPE,
		USB_ENDPOINT_IN(1),
		USB_ENDPOINT_TYPE_ISOCHRONOUS | USB_ENDPOINT_SYNC_ASYNCHRONOUS,
		WBVAL(0x00c8),
		0x01,
		0x00,
		0x00,

		AUDIO_STREAMING_ENDPOINT_DESC_SIZE,
		AUDIO_ENDPOINT_DESCRIPTOR_TYPE,
		AUDIO_ENDPOINT_GENERAL,
		0x01,
		0x00,
		WBVAL(0x0000),

		0
	};

	static const uint8_t logitech_dev_descriptor[] = {
 0x12,
 0x01,
 WBVAL(0x0110),
 0x00,
 0x00,
 0x00,
 0x08,
 WBVAL(0x046D),
 WBVAL(0x0000),
 WBVAL(0x0001),
 0x01,
 0x02,
 0x00,
 0x01,
	};

	static const uint8_t logitech_config_descriptor[] = {
		0x09,
		USB_CONFIGURATION_DESCRIPTOR_TYPE,
		WBVAL(0x00b1),
		0x02,
		0x01,
		0x00,
		USB_CONFIG_BUS_POWERED,
		USB_CONFIG_POWER_MA(90),

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x00,
		0x00,
		0x00,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOCONTROL,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		AUDIO_CONTROL_INTERFACE_DESC_SZ(1),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_HEADER,
		WBVAL(0x0100),
		WBVAL(0x0028),
		0x01,
		0x01,

		AUDIO_INPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_INPUT_TERMINAL,
		0x01,
		WBVAL(AUDIO_TERMINAL_MICROPHONE),
		0x02,
		0x02,
		WBVAL(AUDIO_CHANNEL_L | AUDIO_CHANNEL_R),
		0x00,
		0x00,

		AUDIO_OUTPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_OUTPUT_TERMINAL,
		0x02,
		WBVAL(AUDIO_TERMINAL_USB_STREAMING),
		0x01,
		0x03,
		0x00,

		AUDIO_FEATURE_UNIT_DESC_SZ(2, 1),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_FEATURE_UNIT,
		0x03,
		0x01,
		0x01,
		0x01,
		0x02,
		0x02,
		0x00,

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x01,
		0x00,
		0x00,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOSTREAMING,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x01,
		0x01,
		0x01,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOSTREAMING,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		AUDIO_STREAMING_INTERFACE_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_GENERAL,
		0x02,
		0x01,
		WBVAL(AUDIO_FORMAT_PCM),

		AUDIO_FORMAT_TYPE_I_DESC_SZ(5),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_FORMAT_TYPE,
		AUDIO_FORMAT_TYPE_I,
		0x01,
		0x02,
		0x10,
		0x05,
		B3VAL(8000),
		B3VAL(11025),
		B3VAL(22050),
		B3VAL(44100),
		B3VAL(48000),

		AUDIO_STANDARD_ENDPOINT_DESC_SIZE,
		USB_ENDPOINT_DESCRIPTOR_TYPE,
		USB_ENDPOINT_IN(1),
		USB_ENDPOINT_TYPE_ISOCHRONOUS | USB_ENDPOINT_SYNC_ASYNCHRONOUS,
		WBVAL(0x0064),
		0x01,
		0x00,
		0x00,

		AUDIO_STREAMING_ENDPOINT_DESC_SIZE,
		AUDIO_ENDPOINT_DESCRIPTOR_TYPE,
		AUDIO_ENDPOINT_GENERAL,
		0x01,
		0x00,
		WBVAL(0x0000),

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x01,
		0x02,
		0x01,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOSTREAMING,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		AUDIO_STREAMING_INTERFACE_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_GENERAL,
		0x02,
		0x01,
		WBVAL(AUDIO_FORMAT_PCM),

		AUDIO_FORMAT_TYPE_I_DESC_SZ(5),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_FORMAT_TYPE,
		AUDIO_FORMAT_TYPE_I,
		0x02,
		0x02,
		0x10,
		0x05,
		B3VAL(8000),
		B3VAL(11025),
		B3VAL(22050),
		B3VAL(44100),
		B3VAL(48000),

		AUDIO_STANDARD_ENDPOINT_DESC_SIZE,
		USB_ENDPOINT_DESCRIPTOR_TYPE,
		USB_ENDPOINT_IN(1),
		USB_ENDPOINT_TYPE_ISOCHRONOUS | USB_ENDPOINT_SYNC_ASYNCHRONOUS,
		WBVAL(0x00c8),
		0x01,
		0x00,
		0x00,

		AUDIO_STREAMING_ENDPOINT_DESC_SIZE,
		AUDIO_ENDPOINT_DESCRIPTOR_TYPE,
		AUDIO_ENDPOINT_GENERAL,
		0x01,
		0x00,
		WBVAL(0x0000),

		0
	};

	static const uint8_t ak5370_dev_descriptor[] = {
		0x12,
		0x01,
		0x10, 0x01,
		0x00,
		0x00,
		0x00,
		0x08,
		0x56, 0x05,
		0x01, 0x00,
		0x01, 0x00,
		0x01,
		0x02,
		0x00,
		0x01,
	};

	static const uint8_t ak5370_config_descriptor[] = {
		0x09,
		0x02,
		0x76, 0x00,
		0x02,
		0x01,
		0x00,
		0x80,
		0x2D,

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
		0x26, 0x00,
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
		0x43, 0x00,

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

		0x17,
		0x24,
		0x02,
		0x01,
		0x01,
		0x02,
		0x10,
		0x05,
		0x40, 0x1F, 0x00,
		0x11, 0x2B, 0x00,
		0x22, 0x56, 0x00,
		0x44, 0xAC, 0x00,
		0x80, 0xBB, 0x00,

		0x07,
		0x05,
		0x81,
		0x01,
		0x64, 0x00,
		0x01,

		0x07,
		0x25,
		0x01,
		0x01,
		0x00,
		0x00, 0x00,
	};

	static void usb_mic_handle_reset(USBDevice* dev)
	{
		return;
	}

#define ATTRIB_ID(cs, attrib, idif) (((cs) << 24) | ((attrib) << 16) | (idif))


	static int usb_audio_get_control(SINGSTARMICState* s, uint8_t attrib, uint16_t cscn, uint16_t idif, int length, uint8_t* data)
	{
		const uint8_t cs = cscn >> 8;
		const uint8_t cn = cscn - 1;
		const uint32_t aid = ATTRIB_ID(cs, attrib, idif);
		int ret = USB_RET_STALL;

		switch (aid)
		{
			case ATTRIB_ID(AUDIO_MUTE_CONTROL, AUDIO_REQUEST_GET_CUR, 0x0300):
				data[0] = s->f.mute;
				ret = 1;
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_CUR, 0x0300):
				if (cn < 2 || cn == 0xff)
				{
					const uint16_t vol = (s->f.vol[cn == 1 ? 1 : 0] * 0x8800 + 127) / 255 + 0x8000;
					data[0] = (uint8_t)(vol & 0xFF);
					data[1] = vol >> 8;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_MIN, 0x0300):
				if (cn < 2 || cn == 0xff)
				{
					data[0] = 0x01;
					data[1] = 0x80;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_MAX, 0x0300):
				if (cn < 2 || cn == 0xff)
				{
					data[0] = 0x00;
					data[1] = 0x08;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_RES, 0x0300):
				if (cn < 2 || cn == 0xff)
				{
					data[0] = 0x88;
					data[1] = 0x00;
					ret = 2;
				}
				break;
		}

		return ret;
	}

	static int usb_audio_set_control(SINGSTARMICState* s, uint8_t attrib, uint16_t cscn, uint16_t idif, int length, uint8_t* data)
	{
		uint8_t cs = cscn >> 8;
		uint8_t cn = cscn - 1;
		uint32_t aid = ATTRIB_ID(cs, attrib, idif);
		int ret = USB_RET_STALL;

		switch (aid)
		{
			case ATTRIB_ID(AUDIO_MUTE_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0300):
				s->f.mute = data[0] & 1;
				ret = 0;
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0300):
				if (cn < 2 || cn == 0xff)
				{
					uint16_t vol = data[0] + (data[1] << 8);

					vol -= 0x8000;
					vol = (vol * 255 + 0x4400) / 0x8800;
					if (vol > 255)
						vol = 255;

					if (cn == 0xff)
					{
						if (s->f.vol[0] != vol)
							s->f.vol[0] = (uint8_t)vol;
						if (s->f.vol[1] != vol)
							s->f.vol[1] = (uint8_t)vol;
					}
					else
					{
						if (s->f.vol[cn] != vol)
							s->f.vol[cn] = (uint8_t)vol;
					}

					ret = 0;
				}
				break;
			case ATTRIB_ID(AUDIO_AUTOMATIC_GAIN_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0300):
				ret = 0;
				break;
		}

		return ret;
	}

	static int usb_audio_ep_control(SINGSTARMICState* s, uint8_t attrib, uint16_t cscn, uint16_t ep, int length, uint8_t* data)
	{
		uint8_t cs = cscn >> 8;
		uint8_t cn = cscn - 1;
		uint32_t aid = ATTRIB_ID(cs, attrib, ep);
		int ret = USB_RET_STALL;

		Console.Warning("usb_mic: ep control: cs=0x%x, cn=0x%X, attrib=0x%X, ep=0x%X", cs, cn, attrib, ep);

		switch (aid)
		{
			case ATTRIB_ID(AUDIO_SAMPLING_FREQ_CONTROL, AUDIO_REQUEST_SET_CUR, 0x81):
				if (cn == 0xFF)
				{
					const uint32_t sr = data[0] | (data[1] << 8) | (data[2] << 16);
					if (s->f.srate[0] != sr)
					{
						s->f.srate[0] = sr;
						if (s->audsrc[0])
							s->audsrc[0]->SetResampling(s->f.srate[0]);

					}
					if (s->f.srate[1] != sr)
					{
						s->f.srate[1] = sr;
						if (s->audsrc[1])
							s->audsrc[1]->SetResampling(s->f.srate[1]);
						
					}
				}
				else if (cn < 2)
				{
					const uint32_t sr = data[0] | (data[1] << 8) | (data[2] << 16);
					if (s->f.srate[cn] != sr)
					{
						s->f.srate[cn] = sr;
						if (s->audsrc[cn])
							s->audsrc[cn]->SetResampling(s->f.srate[cn]);
					}
				}
				ret = 0;
				break;
			case ATTRIB_ID(AUDIO_SAMPLING_FREQ_CONTROL, AUDIO_REQUEST_GET_CUR, 0x81):
				data[0] = s->f.srate[0] & 0xFF;
				data[1] = (s->f.srate[0] >> 8) & 0xFF;
				data[2] = (s->f.srate[0] >> 16) & 0xFF;
				ret = 3;
				break;
		}

		return ret;
	}

	static void usb_mic_set_interface(USBDevice* dev, int intf, int alt_old, int alt_new)
	{
		SINGSTARMICState* s = USB_CONTAINER_OF(dev, SINGSTARMICState, dev);
		s->f.intf = alt_new;
#if defined(_DEBUG)
		if (file && intf > 0 && alt_old != alt_new)
		{
			fclose(file);
			file = nullptr;
		}
#endif
	}

	static void usb_mic_handle_control(USBDevice* dev, USBPacket* p, int request, int value, int index, int length, uint8_t* data)
	{
		SINGSTARMICState* s = USB_CONTAINER_OF(dev, SINGSTARMICState, dev);
		int ret = 0;


		ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
		if (ret >= 0)
		{
			return;
		}

		switch (request)
		{
			case ClassInterfaceRequest | AUDIO_REQUEST_GET_CUR:
			case ClassInterfaceRequest | AUDIO_REQUEST_GET_MIN:
			case ClassInterfaceRequest | AUDIO_REQUEST_GET_MAX:
			case ClassInterfaceRequest | AUDIO_REQUEST_GET_RES:
				ret = usb_audio_get_control(s, request & 0xff, value, index, length, data);
				if (ret < 0)
				{
					Console.Warning("usb_mic: fail: get control, req=%02x, val=%02x, idx=%02x, ret=%d", request, value, index, ret);
					goto fail;
				}
				p->actual_length = ret;
				break;

			case ClassInterfaceOutRequest | AUDIO_REQUEST_SET_CUR:
			case ClassInterfaceOutRequest | AUDIO_REQUEST_SET_MIN:
			case ClassInterfaceOutRequest | AUDIO_REQUEST_SET_MAX:
			case ClassInterfaceOutRequest | AUDIO_REQUEST_SET_RES:
				ret = usb_audio_set_control(s, request & 0xff, value, index, length, data);
				if (ret < 0)
				{
					Console.Warning("usb_mic: fail: set control, req=%02x, val=%02x, idx=%02x", request, value, index);
					goto fail;
				}
				break;

			case ClassEndpointRequest | AUDIO_REQUEST_GET_CUR:
			case ClassEndpointRequest | AUDIO_REQUEST_GET_MIN:
			case ClassEndpointRequest | AUDIO_REQUEST_GET_MAX:
			case ClassEndpointRequest | AUDIO_REQUEST_GET_RES:
			case ClassEndpointOutRequest | AUDIO_REQUEST_SET_CUR:
			case ClassEndpointOutRequest | AUDIO_REQUEST_SET_MIN:
			case ClassEndpointOutRequest | AUDIO_REQUEST_SET_MAX:
			case ClassEndpointOutRequest | AUDIO_REQUEST_SET_RES:
				ret = usb_audio_ep_control(s, request & 0xff, value, index, length, data);
				if (ret < 0)
					goto fail;
				break;
			default:
			fail:
				p->status = USB_RET_STALL;
				break;
		}
	}

	inline static int16_t SetVolume(int16_t sample, int vol)
	{
		return (int16_t)((int32_t)sample * vol / 0xFF);
	}

	static void usb_mic_handle_data(USBDevice* dev, USBPacket* p)
	{
		SINGSTARMICState* s = USB_CONTAINER_OF(dev, SINGSTARMICState, dev);
		int ret = 0;

		switch (p->pid)
		{
			case USB_TOKEN_IN:
				{
					int outChns = s->f.intf == 2 ? 2 : 1;
					uint32_t frames, out_frames[2] = {0}, chn;
					int16_t *src1, *src2;
					int16_t* dst = (int16_t*)p->buffer_ptr;
					size_t len = p->buffer_size;

					if (s->f.srate[0] == 48000 || s->f.srate[0] == 8000 || s->f.srate[0] == 16000)
						len = std::min<u32>(p->buffer_size, outChns * sizeof(int16_t) * s->f.srate[0] / 1000);

					uint32_t max_frames = len / (outChns * sizeof(uint16_t));

					memset(dst, 0, len);

					for (int i = 0; i < 2; i++)
					{
						frames = max_frames;
						if (s->audsrc[i] && s->audsrc[i]->GetFrames(&frames))
						{
							frames = std::min(max_frames, frames);
							out_frames[i] = s->audsrc[i]->GetBuffer(s->buffer[i].data(), frames);
						}
					}

					if (!frames)
					{
						p->status = USB_RET_NAK;
						return;
					}


					switch (s->f.mode)
					{
						case MIC_MODE_SINGLE:
						{
							int k = s->audsrc[0] ? 0 : 1;
							int off = s->f.intf == 1 ? 0 : k;
							chn = s->audsrc[k]->GetChannels();
							frames = out_frames[k];

							uint32_t i = 0;
							for (; i < frames && i < max_frames; i++)
							{
								dst[i * outChns + off] = SetVolume(s->buffer[k][i * chn], s->f.vol[0]);
							}

							ret = i;
						}
						break;
						case MIC_MODE_SHARED:
						{
							chn = s->audsrc[0]->GetChannels();
							frames = out_frames[0];
							src1 = s->buffer[0].data();

							uint32_t i = 0;
							for (; i < frames && i < max_frames; i++)
							{
								dst[i * outChns] = SetVolume(src1[i * chn], s->f.vol[0]);
								if (outChns > 1)
								{
									if (chn == 1)
										dst[i * 2 + 1] = dst[i * 2];
									else
										dst[i * 2 + 1] = SetVolume(src1[i * chn + 1], s->f.vol[0]);
								}
							}

							ret = i;
						}
						break;
						case MIC_MODE_SEPARATE:
						{
							uint32_t cn1 = s->audsrc[0]->GetChannels();
							uint32_t cn2 = s->audsrc[1]->GetChannels();
							uint32_t minLen = std::min(out_frames[0], out_frames[1]);

							src1 = s->buffer[0].data();
							src2 = s->buffer[1].data();

							uint32_t i = 0;
							for (; i < minLen && i < max_frames; i++)
							{
								dst[i * outChns] = SetVolume(src1[i * cn1], s->f.vol[0]);
								if (outChns > 1)
									dst[i * 2 + 1] = SetVolume(src2[i * cn2], s->f.vol[1]);
							}

							ret = i;
						}
						break;
						default:
							break;
					}

					ret = ret * outChns * sizeof(int16_t);
					p->actual_length = ret;

#if 0
					if (!file)
					{
						char name[1024] = {0};
						snprintf(name, sizeof(name), "usb_mic_%dch_%uHz.raw", outChns, s->f.srate[0]);
						file = fopen(name, "wb");
					}

					if (file)
						fwrite(dst, 1, ret, file);
#endif
				}
				break;
			case USB_TOKEN_OUT:
				break;
			default:
				p->status = USB_RET_STALL;
				break;
		}
	}


	static void usb_mic_handle_destroy(USBDevice* dev)
	{
		SINGSTARMICState* s = USB_CONTAINER_OF(dev, SINGSTARMICState, dev);
		if (file)
			fclose(file);
		file = NULL;

		if (!s)
			return;
		for (int i = 0; i < 2; i++)
		{
			if (s->audsrc[i])
			{
				s->audsrc[i]->Stop();
				s->audsrc[i].reset();
				s->buffer[i].clear();
			}
		}

		delete s;
	}

	USBDevice* MicrophoneDevice::CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const
	{
		if (subtype >= MIC_COUNT)
			return nullptr;

		static const bool dual_mic = subtype == MIC_SINGSTAR;
		return CreateDevice(si, port, subtype, dual_mic, 48000, MicrophoneDevice::TypeName());
	}

	USBDevice* MicrophoneDevice::CreateDevice(SettingsInterface& si, u32 port, u32 subtype, bool dual_mic, const int samplerate, const char* devtype) const
	{
		if (subtype >= MIC_COUNT)
			return nullptr;

		SINGSTARMICState* s = new SINGSTARMICState();

		if (dual_mic)
		{
			std::string dev0(USB::GetConfigString(si, port, devtype, "player1_device_name"));
			std::string dev1(USB::GetConfigString(si, port, devtype, "player2_device_name"));
			const s32 latency = USB::GetConfigInt(si, port, devtype, "input_latency", AudioDevice::DEFAULT_LATENCY);

			if (!dev0.empty() && dev0 == dev1)
			{
				Console.WriteLn("USB-Mic: Trying to open stereo single source dual mic: '%s'", dev0.c_str());
				s->audsrc[0] = AudioDevice::CreateDevice(AUDIODIR_SOURCE, 2, dev0, latency);
				if (!s->audsrc[0])
				{
					Console.Error("USB-Mic: Failed to get stereo source, mic '%s' might only be mono", dev0.c_str());
					s->audsrc[0] = AudioDevice::CreateDevice(AUDIODIR_SOURCE, 1, std::move(dev0), latency);
				}

				s->f.mode = MIC_MODE_SHARED;
			}
			else
			{
				if (!dev0.empty())
					s->audsrc[0] = AudioDevice::CreateDevice(AUDIODIR_SOURCE, 1, std::move(dev0), latency);
				if (!dev1.empty())
					s->audsrc[1] = AudioDevice::CreateDevice(AUDIODIR_SOURCE, 1, std::move(dev1), latency);

				s->f.mode = (s->audsrc[0] && s->audsrc[1]) ? MIC_MODE_SEPARATE : MIC_MODE_SINGLE;
			}
		}
		else
		{
			std::string dev0(USB::GetConfigString(si, port, devtype, "input_device_name"));
			const s32 latency0 = USB::GetConfigInt(si, port, devtype, "input_latency", AudioDevice::DEFAULT_LATENCY);
			if (!dev0.empty())
				s->audsrc[0] = AudioDevice::CreateDevice(AUDIODIR_SOURCE, 1, std::move(dev0), latency0);

			s->f.mode = MIC_MODE_SINGLE;
		}

		if (!s->audsrc[0] && !s->audsrc[1])
		{
			Host::AddOSDMessage(
				TRANSLATE_STR("USB", "USB-Mic: Neither player 1 nor 2 is connected."), Host::OSD_ERROR_DURATION);
			goto fail;
		}

		Console.WriteLn("USB-Mic Mode: %s",
			(s->f.mode == MIC_MODE_SHARED ? "shared" : (s->f.mode == MIC_MODE_SEPARATE ? "separate" : "single")));
		Console.WriteLn("USB-Mic Source 0: %s", s->audsrc[0] ? "opened" : "not opened");
		Console.WriteLn("USB-Mic Source 1: %s", s->audsrc[1] ? "opened" : "not opened");

		for (int i = 0; i < 2; i++)
		{
			if (s->audsrc[i])
			{
				s->buffer[i].resize(BUFFER_FRAMES * s->audsrc[i]->GetChannels());
				if (!s->audsrc[i]->Start())
				{
					Host::AddOSDMessage(
						fmt::format(TRANSLATE_FS("USB", "USB-Mic: Failed to start player {} audio stream."), i + 1),
						Host::OSD_ERROR_DURATION);
					goto fail;
				}
				s->audsrc[i]->SetResampling(samplerate);
			}
		}

		s->desc.full = &s->desc_dev;
		switch (subtype)
		{
			case MIC_SINGSTAR:
				s->desc.str = singstar_desc_strings;
				if (usb_desc_parse_dev(singstar_dev_descriptor, sizeof(singstar_dev_descriptor), s->desc, s->desc_dev) < 0)
					goto fail;
				if (usb_desc_parse_config(singstar_config_descriptor, sizeof(singstar_config_descriptor), s->desc_dev) < 0)
					goto fail;
				break;
			case MIC_LOGITECH:
				s->desc.str = logitech_desc_strings;
				if (usb_desc_parse_dev(logitech_dev_descriptor, sizeof(logitech_dev_descriptor), s->desc, s->desc_dev) < 0)
					goto fail;
				if (usb_desc_parse_config(logitech_config_descriptor, sizeof(logitech_config_descriptor), s->desc_dev) < 0)
					goto fail;
				break;
			case MIC_KONAMI:
				s->desc.str = ak5370_desc_strings;
				if (usb_desc_parse_dev(ak5370_dev_descriptor, sizeof(ak5370_dev_descriptor), s->desc, s->desc_dev) < 0)
					goto fail;
				if (usb_desc_parse_config(ak5370_config_descriptor, sizeof(ak5370_config_descriptor), s->desc_dev) < 0)
					goto fail;
				break;
		}

		s->dev.speed = USB_SPEED_FULL;
		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_reset = usb_mic_handle_reset;
		s->dev.klass.handle_control = usb_mic_handle_control;
		s->dev.klass.handle_data = usb_mic_handle_data;
		s->dev.klass.set_interface = usb_mic_set_interface;
		s->dev.klass.unrealize = usb_mic_handle_destroy;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = singstar_desc_strings[2];

		s->f.vol[0] = 240;
		s->f.vol[1] = 240;
		s->f.srate[0] = samplerate;
		s->f.srate[1] = samplerate;

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);
		usb_mic_handle_reset(&s->dev);

		return &s->dev;

	fail:
		usb_mic_handle_destroy(&s->dev);
		return nullptr;
	}

	const char* MicrophoneDevice::Name() const
	{
		return TRANSLATE_NOOP("USB", "Microphone");
	}

	const char* MicrophoneDevice::TypeName() const
	{
		return "singstar";
	}

	const char* MicrophoneDevice::IconName() const
	{
		return ICON_PF_SINGSTAR_MIC;
	}

	bool MicrophoneDevice::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		SINGSTARMICState* s = USB_CONTAINER_OF(dev, SINGSTARMICState, dev);
		if (!sw.DoMarker("SINGSTARMICState"))
			return false;

		sw.Do(&s->f.intf);
		sw.Do(&s->f.mode);
		sw.Do(&s->f.altset);
		sw.Do(&s->f.mute);
		sw.DoPODArray(&s->f.vol, std::size(s->f.vol));
		sw.DoPODArray(s->f.srate, std::size(s->f.srate));

		if (sw.IsReading() && !sw.HasError())
		{
			for (u32 i = 0; i < 2; i++)
			{
				if (s->audsrc[i])
					s->audsrc[i]->SetResampling(s->f.srate[i]);
			}
		}

		return !sw.HasError();
	}

	void MicrophoneDevice::UpdateSettings(USBDevice* dev, SettingsInterface& si) const
	{
	}

	std::span<const char*> MicrophoneDevice::SubTypes() const
	{
		static const char* subtypes[] = {
			TRANSLATE_NOOP("USB", "Singstar"),
			TRANSLATE_NOOP("USB", "Logitech"),
			TRANSLATE_NOOP("USB", "Konami"),
		};
		return subtypes;
	}

	std::span<const SettingInfo> MicrophoneDevice::Settings(u32 subtype) const
	{
		switch (subtype)
		{
			case MIC_SINGSTAR:
			{
				static constexpr const SettingInfo info[] = {
					{SettingInfo::Type::StringList, "player1_device_name", TRANSLATE_NOOP("USB", "Player 1 Device"),
						TRANSLATE_NOOP("USB", "Selects the input for the first player."), "", nullptr, nullptr, nullptr,
						nullptr, nullptr, &AudioDevice::GetInputDeviceList},
					{SettingInfo::Type::StringList, "player2_device_name", TRANSLATE_NOOP("USB", "Player 2 Device"),
						TRANSLATE_NOOP("USB", "Selects the input for the second player."), "", nullptr, nullptr, nullptr,
						nullptr, nullptr, &AudioDevice::GetInputDeviceList},
					{SettingInfo::Type::Integer, "input_latency", TRANSLATE_NOOP("USB", "Input Latency"),
						TRANSLATE_NOOP("USB", "Specifies the latency to the host input device."),
						AudioDevice::DEFAULT_LATENCY_STR, "1", "1000", "1", TRANSLATE_NOOP("USB", "%dms"), nullptr, nullptr, 1.0f},
				};
				return info;
			}
			case MIC_LOGITECH:
			case MIC_KONAMI:
			default:
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
		}
	}
}

std::unique_ptr<AudioDevice> AudioDevice::CreateNoopDevice(AudioDir dir, u32 channels)
{
	return std::make_unique<usb_mic::audiodev_noop::NoopAudioDevice>(dir, channels);
}

std::unique_ptr<AudioDevice> AudioDevice::CreateDevice(AudioDir dir, u32 channels, std::string devname, s32 latency)
{
	return std::make_unique<usb_mic::audiodev_cubeb::CubebAudioDevice>(dir, channels, std::move(devname), latency);
}

std::vector<std::pair<std::string, std::string>> AudioDevice::GetInputDeviceList()
{
	return usb_mic::audiodev_cubeb::CubebAudioDevice::GetDeviceList(true);
}

std::vector<std::pair<std::string, std::string>> AudioDevice::GetOutputDeviceList()
{
	return usb_mic::audiodev_cubeb::CubebAudioDevice::GetDeviceList(false);
}
