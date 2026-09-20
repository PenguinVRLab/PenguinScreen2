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

#include "Host.h"
#include "USB/qemu-usb/qusb.h"
#include "USB/qemu-usb/desc.h"
#include "USB/qemu-usb/USBinternal.h"
#include "USB/usb-mic/audio.h"
#include "USB/usb-mic/usb-headset.h"
#include "USB/usb-mic/audiodev.h"
#include "USB/USB.h"
#include "StateWrapper.h"

#include "IconsPromptFont.h"
#include "common/Console.h"

#define BUFFER_FRAMES 200

#define USBAUDIO_PACKET_SIZE 200
#define USBAUDIO_SAMPLE_RATE 48000
#define USBAUDIO_PACKET_INTERVAL 1

namespace usb_mic
{

	static FILE* file = NULL;

	typedef struct HeadsetState
	{
		USBDevice dev;
		std::unique_ptr<AudioDevice> audsrc;
		std::unique_ptr<AudioDevice> audsink;

		struct freeze
		{
			int intf;
			MicMode mode;

			struct
			{
				bool mute;
				uint8_t vol[2];
				uint32_t srate;
			} out;

			struct
			{
				bool mute;
				uint8_t vol;
				uint32_t srate;
			} in;

			struct
			{
				bool mute;
				uint8_t vol[2];
			} mixer;
		} f;

		std::vector<int16_t> in_buffer;
		std::vector<int16_t> out_buffer;

		USBDesc desc;
		USBDescDevice desc_dev;
	} HeadsetState;

	static const uint8_t headset_dev_descriptor[] = {
 0x12,
 0x01,
 WBVAL(0x0110),
 0x00,
 0x00,
 0x00,
 0x40,
 WBVAL(0x046d),
 WBVAL(0x0a01),
 WBVAL(0x1012),
 0x01,
 0x02,
 0x00,
 0x01,
	};

	static const uint8_t headset_config_descriptor[] = {

		USB_CONFIGURATION_DESC_SIZE,
		USB_CONFIGURATION_DESCRIPTOR_TYPE,
		WBVAL(318),
		0x03,
		0x01,
		0x00,
		USB_CONFIG_BUS_POWERED,
		USB_CONFIG_POWER_MA(100),

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x00,
		0x00,
		0x00,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOCONTROL,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		AUDIO_CONTROL_INTERFACE_DESC_SZ(2),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_HEADER,
		WBVAL(0x0100),
		WBVAL(0x0075),
		0x02,
		0x01,
		0x02,

		AUDIO_INPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_INPUT_TERMINAL,
		0x0d,
		WBVAL(AUDIO_TERMINAL_MICROPHONE),
		0x00,
		0x01,
		WBVAL(AUDIO_CHANNEL_L),
		0x00,
		0x00,

		AUDIO_FEATURE_UNIT_DESC_SZ(1, 1),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_FEATURE_UNIT,
		0x06,
		0x0d,
		0x01,
		0x03,
		0x00,
		0x00,

		AUDIO_INPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_INPUT_TERMINAL,
		0x0c,
		WBVAL(AUDIO_TERMINAL_USB_STREAMING),
		0x00,
		0x02,
		WBVAL((AUDIO_CHANNEL_L | AUDIO_CHANNEL_R)),
		0x00,
		0x00,

		AUDIO_MIXER_UNIT_DESC_SZ(2, 1),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_MIXER_UNIT, 0x09,
		0x02,
		0x0c,
		0x06,
		0x02,
		WBVAL((AUDIO_CHANNEL_L | AUDIO_CHANNEL_R)),
		0,
		0x00,
		0,

		AUDIO_FEATURE_UNIT_DESC_SZ(2, 1),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_FEATURE_UNIT,
		0x01,
		0x09,
		0x01,
		0x01,
		0x02,
		0x02,
		0x00,

		AUDIO_OUTPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_OUTPUT_TERMINAL,
		0x0e,
		WBVAL(AUDIO_TERMINAL_SPEAKER),
		0x00,
		0x01,
		0x00,

		AUDIO_INPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_INPUT_TERMINAL,
		0x0b,
		WBVAL(AUDIO_TERMINAL_MICROPHONE),
		0x00,
		0x01,
		WBVAL(AUDIO_CHANNEL_L),
		0x00,
		0x00,

		AUDIO_FEATURE_UNIT_DESC_SZ(1, 1),
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_FEATURE_UNIT,
		0x02,
		0x0b,
		0x01,
		0x03,
		0x00,
		0x00,

		AUDIO_MIXER_UNIT_DESC_SZ(1, 1), AUDIO_INTERFACE_DESCRIPTOR_TYPE, AUDIO_CONTROL_MIXER_UNIT, 0x07,
		0x01,
		0x02,
		0x01,
		WBVAL(AUDIO_CHANNEL_L),
		0,
		0x00,
		0,

		AUDIO_OUTPUT_TERMINAL_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_CONTROL_OUTPUT_TERMINAL,
		0x0a,
		WBVAL(AUDIO_TERMINAL_USB_STREAMING),
		0x00,
		0x07,
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
		0x0c,
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
		USB_ENDPOINT_OUT(1),
		USB_ENDPOINT_TYPE_ISOCHRONOUS | USB_ENDPOINT_SYNC_ADAPTIVE,
		WBVAL(0x00c0),
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
		0x0c,
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
		USB_ENDPOINT_OUT(1),
		USB_ENDPOINT_TYPE_ISOCHRONOUS | USB_ENDPOINT_SYNC_ADAPTIVE,
		WBVAL(0x0060),
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
		0x02,
		0x00,
		0x00,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOSTREAMING,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		USB_INTERFACE_DESC_SIZE,
		USB_INTERFACE_DESCRIPTOR_TYPE,
		0x02,
		0x01,
		0x01,
		USB_CLASS_AUDIO,
		AUDIO_SUBCLASS_AUDIOSTREAMING,
		AUDIO_PROTOCOL_UNDEFINED,
		0x00,

		AUDIO_STREAMING_INTERFACE_DESC_SIZE,
		AUDIO_INTERFACE_DESCRIPTOR_TYPE,
		AUDIO_STREAMING_GENERAL,
		0x0a,
		0x00,
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
		USB_ENDPOINT_IN(4),
		USB_ENDPOINT_TYPE_ISOCHRONOUS | USB_ENDPOINT_SYNC_ADAPTIVE,
		WBVAL(0x0060),
		0x01,
		0x00,
		0x00,

		AUDIO_STREAMING_ENDPOINT_DESC_SIZE,
		AUDIO_ENDPOINT_DESCRIPTOR_TYPE,
		AUDIO_ENDPOINT_GENERAL,
		0x01,
		0x02,
		WBVAL(0x0001),

		0
	};

	static const USBDescStrings desc_strings = {"",
		"Logitech",
		"Logitech USB Headset", "00000000"};

	static void headset_handle_reset(USBDevice* dev)
	{
		return;
	}

#define ATTRIB_ID(cs, attrib, idif) (((cs) << 24) | ((attrib) << 16) | (idif))


	static int usb_audio_get_control(HeadsetState* s, uint8_t attrib, uint16_t cscn, uint16_t idif, int length, uint8_t* data)
	{
		uint8_t cs = cscn >> 8;
		const uint8_t cn = cscn - 1;
		uint32_t aid = ATTRIB_ID(cs, attrib, idif);
		int ret = USB_RET_STALL;

		switch (aid)
		{
			case ATTRIB_ID(AUDIO_MUTE_CONTROL, AUDIO_REQUEST_GET_CUR, 0x0600):
			case ATTRIB_ID(AUDIO_MUTE_CONTROL, AUDIO_REQUEST_GET_CUR, 0x0200):
				data[0] = s->f.in.mute;
				ret = 1;
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_CUR, 0x0600):
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_CUR, 0x0200):
				{
					uint16_t vol = (s->f.in.vol * 0x8800 + 127) / 255 + 0x8000;
					data[0] = (uint8_t)(vol & 0xFF);
					data[1] = vol >> 8;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_MIN, 0x0600):
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_MIN, 0x0200):
				{
					data[0] = 0x01;
					data[1] = 0x80;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_MAX, 0x0600):
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_MAX, 0x0200):
				{
					data[0] = 0x00;
					data[1] = 0x08;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_RES, 0x0600):
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_RES, 0x0200):
				{
					data[0] = 0x88;
					data[1] = 0x00;
					ret = 2;
				}
				break;

			case ATTRIB_ID(AUDIO_MUTE_CONTROL, AUDIO_REQUEST_GET_CUR, 0x0100):
				data[0] = s->f.out.mute;
				ret = 1;
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_CUR, 0x0100):
				if (cn < 2)
				{
					uint16_t vol = (s->f.out.vol[cn] * 0x8800 + 127) / 255 + 0x8000;
					data[0] = (uint8_t)(vol & 0xFF);
					data[1] = vol >> 8;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_MIN, 0x0100):
				{
					data[0] = 0x01;
					data[1] = 0x80;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_MAX, 0x0100):
				{
					data[0] = 0x00;
					data[1] = 0x08;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_GET_RES, 0x0100):
				{
					data[0] = 0x88;
					data[1] = 0x00;
					ret = 2;
				}
				break;
			case ATTRIB_ID(AUDIO_BASS_BOOST_CONTROL, AUDIO_REQUEST_GET_CUR,
				0x0100):
				data[0] = 0;
				ret = 1;
				break;
		}

		return ret;
	}

	static int usb_audio_set_control(HeadsetState* s, uint8_t attrib, uint16_t cscn, uint16_t idif, int length, uint8_t* data)
	{
		uint8_t cs = cscn >> 8;
		const uint8_t cn = cscn - 1;
		uint32_t aid = ATTRIB_ID(cs, attrib, idif);
		uint16_t vol;
		int ret = USB_RET_STALL;
		bool set_vol = false;

		switch (aid)
		{
			case ATTRIB_ID(AUDIO_MUTE_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0600):
			case ATTRIB_ID(AUDIO_MUTE_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0200):
				s->f.in.mute = data[0] & 1;
				set_vol = true;
				ret = 0;
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0600):
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0200):
				vol = data[0] + (data[1] << 8);
				vol -= 0x8000;
				vol = (vol * 255 + 0x4400) / 0x8800;
				if (vol > 255)
				{
					vol = 255;
				}

				if (s->f.in.vol != vol)
				{
					s->f.in.vol = (uint8_t)vol;
					set_vol = true;
				}
				ret = 0;
				break;
			case ATTRIB_ID(AUDIO_MUTE_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0100):
				s->f.out.mute = data[0] & 1;
				set_vol = true;
				ret = 0;
				break;
			case ATTRIB_ID(AUDIO_VOLUME_CONTROL, AUDIO_REQUEST_SET_CUR, 0x0100):
				vol = data[0] + (data[1] << 8);
				if (cn < 2)
				{

					vol -= 0x8000;
					vol = (vol * 255 + 0x4400) / 0x8800;
					if (vol > 255)
					{
						vol = 255;
					}

					if (s->f.out.vol[cn] != vol)
					{
						s->f.out.vol[cn] = (uint8_t)vol;
						set_vol = true;
					}
					ret = 0;
				}
				break;
		}

		if (set_vol)
		{
		}

		return ret;
	}

	static int usb_audio_ep_control(HeadsetState* s, uint8_t attrib, uint16_t cscn, uint16_t ep, int length, uint8_t* data)
	{
		uint8_t cs = cscn >> 8;
		[[maybe_unused]] const uint8_t cn = cscn - 1;
		uint32_t aid = ATTRIB_ID(cs, attrib, ep);
		int ret = USB_RET_STALL;

		switch (aid)
		{
			case ATTRIB_ID(AUDIO_SAMPLING_FREQ_CONTROL, AUDIO_REQUEST_SET_CUR, 0x84):
				s->f.in.srate = data[0] | (data[1] << 8) | (data[2] << 16);
				if (s->audsrc)
					s->audsrc->SetResampling(s->f.in.srate);
				ret = 0;
				break;
			case ATTRIB_ID(AUDIO_SAMPLING_FREQ_CONTROL, AUDIO_REQUEST_GET_CUR, 0x84):
				data[0] = s->f.in.srate & 0xFF;
				data[1] = (s->f.in.srate >> 8) & 0xFF;
				data[2] = (s->f.in.srate >> 16) & 0xFF;
				ret = 3;
				break;

			case ATTRIB_ID(AUDIO_SAMPLING_FREQ_CONTROL, AUDIO_REQUEST_SET_CUR, 0x01):
				s->f.out.srate = data[0] | (data[1] << 8) | (data[2] << 16);
				if (s->audsink)
					s->audsink->SetResampling(s->f.out.srate);
				ret = 0;
				break;
			case ATTRIB_ID(AUDIO_SAMPLING_FREQ_CONTROL, AUDIO_REQUEST_GET_CUR, 0x01):
				data[0] = s->f.out.srate & 0xFF;
				data[1] = (s->f.out.srate >> 8) & 0xFF;
				data[2] = (s->f.out.srate >> 16) & 0xFF;
				ret = 3;
				break;
		}

		return ret;
	}

	static void headset_handle_control(USBDevice* dev, USBPacket* p, int request, int value, int index, int length, uint8_t* data)
	{
		HeadsetState* s = USB_CONTAINER_OF(dev, HeadsetState, dev);
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
					Console.Warning("headset: fail: get control\n");
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
					Console.Warning("headset: fail: set control\n data:");
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

	static void headset_handle_data(USBDevice* dev, USBPacket* p)
	{
		HeadsetState* s = USB_CONTAINER_OF(dev, HeadsetState, dev);
		int ret = USB_RET_STALL;
		uint8_t devep = p->ep->nr;

		switch (p->pid)
		{
			case USB_TOKEN_IN:
				if (devep == 4 && s->dev.altsetting[2] && s->audsrc)
				{

					uint32_t outChns = 1;
					uint32_t inChns = s->audsrc->GetChannels();
					int16_t* dst = (int16_t*)p->buffer_ptr;
					size_t len = p->buffer_size;
					uint32_t maxFrames = len / (outChns * sizeof(int16_t)), frames = 0;

					if (s->audsrc->GetFrames(&frames))
					{
						frames = std::min(frames, maxFrames);
						s->in_buffer.resize(frames * inChns);
						frames = s->audsrc->GetBuffer(s->in_buffer.data(), frames);
					}

					uint32_t i = 0;
					for (; i < frames; i++)
					{
						dst[i * outChns] = SetVolume(s->in_buffer[i * inChns], s->f.in.vol);
					}

					ret = i;

#if 0
            if (!file)
            {
                char name[1024] = { 0 };
                snprintf(name, sizeof(name), "headset_s16le_%dch_%dHz.raw", outChns, s->f.in.srate);
                file = fopen(name, "wb");
            }

            if (file)
                fwrite(data, sizeof(short), ret * outChns, file);
#endif
					ret = ret * outChns * sizeof(int16_t);
					p->actual_length = ret;
				}
				break;
			case USB_TOKEN_OUT:

				if (!s->audsink)
					return;

				if (devep == 1 && s->dev.altsetting[1])
				{
					uint32_t inChns = s->dev.altsetting[1] == 1 ? 2 : 1;
					uint32_t outChns = s->audsink->GetChannels();
					int16_t* src = (int16_t*)p->buffer_ptr;
					size_t len = p->buffer_size;
					uint32_t frames = len / (inChns * sizeof(int16_t));

					s->out_buffer.resize(frames * outChns);

					uint32_t i = 0;
					for (; i < frames; i++)
					{
						if (inChns == outChns)
						{
							for (uint32_t cn = 0; cn < outChns; cn++)
								s->out_buffer[i * outChns + cn] = SetVolume(src[i * inChns + cn], s->f.out.vol[cn]);
						}
						else if (inChns < outChns)
						{
							for (uint32_t cn = 0; cn < outChns; cn++)
								s->out_buffer[i * outChns + cn] = SetVolume(src[i * inChns], s->f.out.vol[cn]);
						}
					}

#if 0
            if (!file)
            {
                char name[1024] = { 0 };
                snprintf(name, sizeof(name), "headset_s16le_%dch_%dHz.raw", inChns, s->f.out.srate);
                file = fopen(name, "wb");
            }

            if (file)
                fwrite(data, sizeof(short), frames * inChns, file);
#endif

					frames = s->audsink->SetBuffer(s->out_buffer.data(), frames);

					p->actual_length = frames * inChns * sizeof(int16_t);
				}
				break;
			default:
				p->status = USB_RET_STALL;
				break;
		}
	}


	static void headset_handle_destroy(USBDevice* dev)
	{
		HeadsetState* s = USB_CONTAINER_OF(dev, HeadsetState, dev);
		if (file)
			fclose(file);
		file = NULL;

		if (!s)
			return;
		if (s->audsrc)
		{
			s->audsrc->Stop();
			s->audsrc.reset();
			s->in_buffer.clear();
		}

		if (s->audsink)
		{
			s->audsink->Stop();
			s->audsink.reset();
			s->out_buffer.clear();
		}

		delete s;
	}

	USBDevice* HeadsetDevice::CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const
	{
		HeadsetState* s = new HeadsetState();

		std::string input_devname(USB::GetConfigString(si, port, TypeName(), "input_device_name"));
		std::string output_devname(USB::GetConfigString(si, port, TypeName(), "output_device_name"));
		const s32 input_latency = USB::GetConfigInt(si, port, TypeName(), "input_latency", AudioDevice::DEFAULT_LATENCY);
		const s32 output_latency = USB::GetConfigInt(si, port, TypeName(), "output_latency", AudioDevice::DEFAULT_LATENCY);

		if (!input_devname.empty())
			s->audsrc = AudioDevice::CreateDevice(AUDIODIR_SOURCE, 1, std::move(input_devname), input_latency);
		else
			s->audsrc = AudioDevice::CreateNoopDevice(AUDIODIR_SOURCE, 1);

		if (!output_devname.empty())
			s->audsink = AudioDevice::CreateDevice(AUDIODIR_SINK, 2, std::move(output_devname), output_latency);
		else
			s->audsink = AudioDevice::CreateNoopDevice(AUDIODIR_SINK, 2);

		s->f.mode = MIC_MODE_SINGLE;

		if (!s->audsrc || !s->audsink)
			goto fail;

		s->in_buffer.reserve(BUFFER_FRAMES * s->audsrc->GetChannels());
		s->out_buffer.reserve(BUFFER_FRAMES * s->audsink->GetChannels());

		s->audsrc->Start();
		s->audsink->Start();

		s->desc.full = &s->desc_dev;
		s->desc.str = desc_strings;
		if (usb_desc_parse_dev(headset_dev_descriptor, sizeof(headset_dev_descriptor), s->desc, s->desc_dev) < 0)
			goto fail;
		if (usb_desc_parse_config(headset_config_descriptor, sizeof(headset_config_descriptor), s->desc_dev) < 0)
			goto fail;

		s->dev.speed = USB_SPEED_FULL;
		s->dev.klass.handle_attach = usb_desc_attach;
		s->dev.klass.handle_reset = headset_handle_reset;
		s->dev.klass.handle_control = headset_handle_control;
		s->dev.klass.handle_data = headset_handle_data;
		s->dev.klass.unrealize = headset_handle_destroy;
		s->dev.klass.usb_desc = &s->desc;
		s->dev.klass.product_desc = desc_strings[2];

		s->f.out.vol[0] = 240;
		s->f.out.vol[1] = 240;
		s->f.in.vol = 240;
		s->f.out.srate = 48000;
		s->f.in.srate = 48000;

		usb_desc_init(&s->dev);
		usb_ep_init(&s->dev);
		headset_handle_reset(&s->dev);

		return &s->dev;

	fail:
		headset_handle_destroy(&s->dev);
		return nullptr;
	}

	const char* HeadsetDevice::TypeName() const
	{
		return "headset";
	}

	const char* HeadsetDevice::Name() const
	{
		return TRANSLATE_NOOP("USB", "Logitech USB Headset");
	}

	const char* HeadsetDevice::IconName() const
	{
		return ICON_PF_HEADSET;
	}

	bool HeadsetDevice::Freeze(USBDevice* dev, StateWrapper& sw) const
	{
		HeadsetState* s = USB_CONTAINER_OF(dev, HeadsetState, dev);
		if (!sw.DoMarker("HeadsetDevice"))
			return false;

		sw.Do(&s->f.intf);
		sw.Do(&s->f.mode);
		sw.Do(&s->f.out.mute);
		sw.DoPODArray(s->f.out.vol, std::size(s->f.out.vol));
		sw.Do(&s->f.out.srate);
		sw.Do(&s->f.in.mute);
		sw.Do(&s->f.in.vol);
		sw.Do(&s->f.in.srate);
		sw.Do(&s->f.mixer.mute);
		sw.DoPODArray(s->f.mixer.vol, std::size(s->f.mixer.vol));

		if (sw.IsReading() && !sw.HasError())
		{
			if (s->audsrc)
				s->audsrc->SetResampling(s->f.in.srate);
			if (s->audsink)
				s->audsink->SetResampling(s->f.out.srate);
		}

		return !sw.HasError();
	}

	void HeadsetDevice::UpdateSettings(USBDevice* dev, SettingsInterface& si) const
	{
	}

	std::span<const SettingInfo> HeadsetDevice::Settings(u32 subtype) const
	{
		static constexpr const SettingInfo info[] = {
			{SettingInfo::Type::StringList, "input_device_name", TRANSLATE_NOOP("USB", "Input Device"),
				TRANSLATE_NOOP("USB", "Selects the device to read audio from."), "", nullptr, nullptr, nullptr, nullptr,
				nullptr, &AudioDevice::GetInputDeviceList},
			{SettingInfo::Type::StringList, "output_device_name", TRANSLATE_NOOP("USB", "Output Device"),
				TRANSLATE_NOOP("USB", "Selects the device to output audio to."), "", nullptr, nullptr, nullptr, nullptr,
				nullptr, &AudioDevice::GetOutputDeviceList},
			{SettingInfo::Type::Integer, "input_latency", TRANSLATE_NOOP("USB", "Input Latency"),
				TRANSLATE_NOOP("USB", "Specifies the latency to the host input device."),
				AudioDevice::DEFAULT_LATENCY_STR, "1", "1000", "1", TRANSLATE_NOOP("USB", "%dms"), nullptr, nullptr, 1.0f},
			{SettingInfo::Type::Integer, "output_latency", TRANSLATE_NOOP("USB", "Output Latency"),
				TRANSLATE_NOOP("USB", "Specifies the latency to the host output device."),
				AudioDevice::DEFAULT_LATENCY_STR, "1", "1000", "1", TRANSLATE_NOOP("USB", "%dms"), nullptr, nullptr, 1.0f},
		};
		return info;
	}
}
