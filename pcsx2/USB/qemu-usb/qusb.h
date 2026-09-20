// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: MIT

#pragma once
#include "USB/qemu-usb/queue.h"
#include <cstdint>
#include <cstddef>

#define USB_TOKEN_SETUP 0x2d
#define USB_TOKEN_IN 0x69
#define USB_TOKEN_OUT 0xe1

#define USB_MSG_ATTACH 0x100
#define USB_MSG_DETACH 0x101
#define USB_MSG_RESET 0x102

#define USB_RET_SUCCESS (0)
#define USB_RET_NODEV (-1)
#define USB_RET_NAK (-2)
#define USB_RET_STALL (-3)
#define USB_RET_BABBLE (-4)
#define USB_RET_IOERROR (-5)
#define USB_RET_ASYNC (-6)
#define USB_RET_ADD_TO_QUEUE (-7)
#define USB_RET_REMOVE_FROM_QUEUE (-8)

#define USB_SPEED_LOW 0
#define USB_SPEED_FULL 1
#define USB_SPEED_HIGH 2

#define USB_SPEED_MASK_LOW (1 << USB_SPEED_LOW)
#define USB_SPEED_MASK_FULL (1 << USB_SPEED_FULL)

#define USB_STATE_NOTATTACHED 0
#define USB_STATE_ATTACHED 1
#define USB_STATE_DEFAULT 3
#define USB_STATE_SUSPENDED 6

#define USB_CLASS_RESERVED 0
#define USB_CLASS_AUDIO 1
#define USB_CLASS_COMM 2
#define USB_CLASS_HID 3
#define USB_CLASS_PHYSICAL 5
#define USB_CLASS_STILL_IMAGE 6
#define USB_CLASS_PRINTER 7
#define USB_CLASS_MASS_STORAGE 8
#define USB_CLASS_HUB 9
#define USB_CLASS_CDC_DATA 0x0a
#define USB_CLASS_CSCID 0x0b
#define USB_CLASS_CONTENT_SEC 0x0d
#define USB_CLASS_APP_SPEC 0xfe
#define USB_CLASS_VENDOR_SPEC 0xff

#define USB_DIR_OUT 0
#define USB_DIR_IN 0x80

#define USB_TYPE_MASK (0x03 << 5)
#define USB_TYPE_STANDARD (0x00 << 5)
#define USB_TYPE_CLASS (0x01 << 5)
#define USB_TYPE_VENDOR (0x02 << 5)
#define USB_TYPE_RESERVED (0x03 << 5)

#define USB_RECIP_MASK 0x1f
#define USB_RECIP_DEVICE 0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT 0x02
#define USB_RECIP_OTHER 0x03

#define DeviceRequest ((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE) << 8)
#define DeviceOutRequest ((USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE) << 8)
#define VendorDeviceRequest ((USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE) << 8)
#define VendorDeviceOutRequest \
	((USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE) << 8)
#define InterfaceRequest \
	((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) << 8)
#define InterfaceOutRequest \
	((USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE) << 8)
#define EndpointRequest ((USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT) << 8)
#define EndpointOutRequest \
	((USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT) << 8)

#define ClassInterfaceRequest \
	((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)
#define ClassInterfaceOutRequest \
	((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE) << 8)

#define ClassEndpointRequest \
	((USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT) << 8)
#define ClassEndpointOutRequest \
	((USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT) << 8)

#define VendorInterfaceRequest \
	((USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE) << 8)
#define VendorInterfaceOutRequest \
	((USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE) << 8)

#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE 0x0A
#define USB_REQ_SET_INTERFACE 0x0B
#define USB_REQ_SYNCH_FRAME 0x0C

#define USB_DEVICE_SELF_POWERED 0
#define USB_DEVICE_REMOTE_WAKEUP 1

#define USB_DT_DEVICE 0x01
#define USB_DT_CONFIG 0x02
#define USB_DT_STRING 0x03
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT 0x05
#define USB_DT_DEVICE_QUALIFIER 0x06
#define USB_DT_OTHER_SPEED_CONFIG 0x07
#define USB_DT_DEBUG 0x0A
#define USB_DT_INTERFACE_ASSOC 0x0B
#define USB_DT_BOS 0x0F
#define USB_DT_DEVICE_CAPABILITY 0x10
#define USB_DT_HID 0x21
#define USB_DT_REPORT 0x22
#define USB_DT_PHYSICAL 0x23
#define USB_DT_CS_INTERFACE 0x24
#define USB_DT_CS_ENDPOINT 0x25
#define USB_DT_ENDPOINT_COMPANION 0x30

#define USB_DEV_CAP_WIRELESS 0x01
#define USB_DEV_CAP_USB2_EXT 0x02
#define USB_DEV_CAP_SUPERSPEED 0x03

#define USB_CFG_ATT_ONE (1 << 7)
#define USB_CFG_ATT_SELFPOWER (1 << 6)
#define USB_CFG_ATT_WAKEUP (1 << 5)
#define USB_CFG_ATT_BATTERY (1 << 4)

#define USB_ENDPOINT_XFER_CONTROL 0
#define USB_ENDPOINT_XFER_ISOC 1
#define USB_ENDPOINT_XFER_BULK 2
#define USB_ENDPOINT_XFER_INT 3
#define USB_ENDPOINT_XFER_INVALID 255

#define USB_INTERFACE_INVALID 255

#define GET_REPORT 0xa101
#define GET_IDLE 0xa102
#define GET_PROTOCOL 0xa103
#define SET_REPORT 0x2109
#define SET_IDLE 0x210a
#define SET_PROTOCOL 0x210b

#define USB_DEVICE_DESC_SIZE 18
#define USB_CONFIGURATION_DESC_SIZE 9
#define USB_INTERFACE_DESC_SIZE 9
#define USB_ENDPOINT_DESC_SIZE 7

#define WBVAL(x) ((x)&0xFF), (((x) >> 8) & 0xFF)
#define B3VAL(x) ((x)&0xFF), (((x) >> 8) & 0xFF), (((x) >> 16) & 0xFF)

#define REQUEST_HOST_TO_DEVICE 0
#define REQUEST_DEVICE_TO_HOST 1

#define REQUEST_STANDARD 0
#define REQUEST_CLASS 1
#define REQUEST_VENDOR 2
#define REQUEST_RESERVED 3

#define REQUEST_TO_DEVICE 0
#define REQUEST_TO_INTERFACE 1
#define REQUEST_TO_ENDPOINT 2
#define REQUEST_TO_OTHER 3

#define USB_REQUEST_GET_STATUS 0
#define USB_REQUEST_CLEAR_FEATURE 1
#define USB_REQUEST_SET_FEATURE 3
#define USB_REQUEST_SET_ADDRESS 5
#define USB_REQUEST_GET_DESCRIPTOR 6
#define USB_REQUEST_SET_DESCRIPTOR 7
#define USB_REQUEST_GET_CONFIGURATION 8
#define USB_REQUEST_SET_CONFIGURATION 9
#define USB_REQUEST_GET_INTERFACE 10
#define USB_REQUEST_SET_INTERFACE 11
#define USB_REQUEST_SYNC_FRAME 12

#define USB_GETSTATUS_SELF_POWERED 0x01
#define USB_GETSTATUS_REMOTE_WAKEUP 0x02
#define USB_GETSTATUS_ENDPOINT_STALL 0x01

#define USB_FEATURE_ENDPOINT_STALL 0
#define USB_FEATURE_REMOTE_WAKEUP 1

#define USB_DEVICE_DESCRIPTOR_TYPE 1
#define USB_CONFIGURATION_DESCRIPTOR_TYPE 2
#define USB_STRING_DESCRIPTOR_TYPE 3
#define USB_INTERFACE_DESCRIPTOR_TYPE 4
#define USB_ENDPOINT_DESCRIPTOR_TYPE 5
#define USB_DEVICE_QUALIFIER_DESCRIPTOR_TYPE 6
#define USB_OTHER_SPEED_CONFIG_DESCRIPTOR_TYPE 7
#define USB_INTERFACE_POWER_DESCRIPTOR_TYPE 8

#define USB_CONFIG_POWERED_MASK 0xC0
#define USB_CONFIG_BUS_POWERED 0x80
#define USB_CONFIG_SELF_POWERED 0x40
#define USB_CONFIG_REMOTE_WAKEUP 0x20

#define USB_CONFIG_POWER_MA(mA) ((mA) / 2)

#define USB_ENDPOINT_DIRECTION_MASK 0x80
#define USB_ENDPOINT_OUT(addr) ((addr) | 0x00)
#define USB_ENDPOINT_IN(addr) ((addr) | 0x80)

#define USB_ENDPOINT_TYPE_MASK 0x03
#define USB_ENDPOINT_TYPE_CONTROL 0x00
#define USB_ENDPOINT_TYPE_ISOCHRONOUS 0x01
#define USB_ENDPOINT_TYPE_BULK 0x02
#define USB_ENDPOINT_TYPE_INTERRUPT 0x03
#define USB_ENDPOINT_SYNC_MASK 0x0C
#define USB_ENDPOINT_SYNC_NO_SYNCHRONIZATION 0x00
#define USB_ENDPOINT_SYNC_ASYNCHRONOUS 0x04
#define USB_ENDPOINT_SYNC_ADAPTIVE 0x08
#define USB_ENDPOINT_SYNC_SYNCHRONOUS 0x0C
#define USB_ENDPOINT_USAGE_MASK 0x30
#define USB_ENDPOINT_USAGE_DATA 0x00
#define USB_ENDPOINT_USAGE_FEEDBACK 0x10
#define USB_ENDPOINT_USAGE_IMPLICIT_FEEDBACK 0x20
#define USB_ENDPOINT_USAGE_RESERVED 0x30

typedef struct USBBus USBBus;
typedef struct USBBusOps USBBusOps;
typedef struct USBPort USBPort;
typedef struct USBDevice USBDevice;
typedef struct USBPacket USBPacket;
typedef struct USBEndpoint USBEndpoint;

typedef struct USBDesc USBDesc;
typedef struct USBDescID USBDescID;
typedef struct USBDescDevice USBDescDevice;
typedef struct USBDescConfig USBDescConfig;
typedef struct USBDescIfaceAssoc USBDescIfaceAssoc;
typedef struct USBDescIface USBDescIface;
typedef struct USBDescEndpoint USBDescEndpoint;
typedef struct USBDescOther USBDescOther;
typedef struct USBDescString USBDescString;
typedef struct USBDescMSOS USBDescMSOS;

struct USBDescString
{
	uint8_t index;
	char* str;
	QLIST_ENTRY(USBDescString)
	next;
};

#define USB_MAX_ENDPOINTS 15
#define USB_MAX_INTERFACES 16

struct USBEndpoint
{
	uint8_t nr;
	uint8_t pid;
	uint8_t type;
	uint8_t ifnum;
	int max_packet_size;
	int max_streams;
	bool pipeline;
	bool halted;
	USBDevice* dev;
	QTAILQ_HEAD(, USBPacket)
	queue;
};

enum USBDeviceFlags
{
	USB_DEV_FLAG_FULL_PATH,
	USB_DEV_FLAG_IS_HOST,
	USB_DEV_FLAG_MSOS_DESC_ENABLE,
	USB_DEV_FLAG_MSOS_DESC_IN_USE,
};

typedef void (*USBDeviceRealize)(USBDevice* dev );
typedef void (*USBDeviceUnrealize)(USBDevice* dev );

typedef struct USBDeviceClass
{

	USBDeviceRealize realize;
	USBDeviceUnrealize unrealize;

	USBDevice* (*find_device)(USBDevice* dev, uint8_t addr);

	void (*cancel_packet)(USBDevice* dev, USBPacket* p);

	void (*handle_attach)(USBDevice* dev);

	void (*handle_reset)(USBDevice* dev);

	void (*handle_control)(USBDevice* dev, USBPacket* p, int request, int value,
						   int index, int length, uint8_t* data);

	void (*handle_data)(USBDevice* dev, USBPacket* p);

	void (*set_interface)(USBDevice* dev, int intf,
						  int alt_old, int alt_new);

	void (*flush_ep_queue)(USBDevice* dev, USBEndpoint* ep);

	void (*ep_stopped)(USBDevice* dev, USBEndpoint* ep);

	int (*alloc_streams)(USBDevice* dev, USBEndpoint** eps, int nr_eps,
						 int streams);
	void (*free_streams)(USBDevice* dev, USBEndpoint** eps, int nr_eps);

	const char* product_desc;
	const USBDesc* usb_desc;
	bool attached_settable;
} USBDeviceClass;

struct USBDevice
{
	USBDeviceClass klass;
	USBPort* port;
	USBBus* bus;
	void* opaque;
	uint32_t flags;

	int speed;
	int speedmask;
	uint8_t addr;
	char product_desc[32];
	int auto_attach;
	bool attached;

	int32_t state;
	uint8_t setup_buf[8];
	uint8_t data_buf[4096];
	int32_t remote_wakeup;
	int32_t setup_state;
	int32_t setup_len;
	int32_t setup_index;

	USBEndpoint ep_ctl;
	USBEndpoint ep_in[USB_MAX_ENDPOINTS];
	USBEndpoint ep_out[USB_MAX_ENDPOINTS];

	const USBDesc* usb_desc;
	const USBDescDevice* device;

	int configuration;
	int ninterfaces;
	int altsetting[USB_MAX_INTERFACES];
	const USBDescConfig* config;
	const USBDescIface* ifaces[USB_MAX_INTERFACES];
};

typedef struct USBPortOps
{
	void (*attach)(USBPort* port);
	void (*detach)(USBPort* port);
	void (*wakeup)(USBPort* port);
	void (*complete)(USBPort* port, USBPacket* p);
} USBPortOps;

struct USBPort
{
	USBDevice* dev;
	int speedmask;
	USBPortOps* ops;
	void* opaque;
	int index;
};

typedef void USBCallback(USBPacket* packet, void* opaque);

typedef enum USBPacketState
{
	USB_PACKET_UNDEFINED = 0,
	USB_PACKET_SETUP,
	USB_PACKET_QUEUED,
	USB_PACKET_ASYNC,
	USB_PACKET_COMPLETE,
	USB_PACKET_CANCELED,
} USBPacketState;

struct USBPacket
{
	int pid;
	uint64_t id;
	USBEndpoint* ep;
	unsigned int stream;
	unsigned int buffer_size;
	uint8_t* buffer_ptr;
	uint64_t parameter;
	bool short_not_ok;
	bool int_req;
	int status;
	int actual_length;
	USBPacketState state;
	QTAILQ_ENTRY(USBPacket)
	queue;
	QTAILQ_ENTRY(USBPacket)
	combined_entry;
};

void usb_packet_set_state(USBPacket* p, USBPacketState state);
void usb_packet_check_state(USBPacket* p, USBPacketState expected);
void usb_packet_setup(USBPacket* p, int pid,
					  USBEndpoint* ep, unsigned int stream,
					  uint64_t id, bool short_not_ok, bool int_req);
void usb_packet_addbuf(USBPacket* p, void* ptr, size_t len);
void usb_packet_copy(USBPacket* p, void* ptr, size_t bytes);
void usb_packet_skip(USBPacket* p, size_t bytes);
size_t usb_packet_size(USBPacket* p);
void usb_packet_cleanup(USBPacket* p);

static inline bool usb_packet_is_inflight(USBPacket* p)
{
	return (p->state == USB_PACKET_QUEUED ||
			p->state == USB_PACKET_ASYNC);
}

struct USBBus
{
	USBBusOps* ops;
	int busnr;
	int nfree;
	int nused;
	QTAILQ_HEAD(, USBPort)
	free;
	QTAILQ_HEAD(, USBPort)
	used;
	QTAILQ_ENTRY(USBBus)
	next;
};

struct USBBusOps
{
	void (*register_companion)(USBBus* bus, USBPort* ports[],
							   uint32_t portcount, uint32_t firstport
	);
	void (*wakeup_endpoint)(USBBus* bus, USBEndpoint* ep, unsigned int stream);
};

USBDevice* usb_find_device(USBPort* port, uint8_t addr);

void usb_handle_packet(USBDevice* dev, USBPacket* p);
void usb_packet_complete(USBDevice* dev, USBPacket* p);
void usb_packet_complete_one(USBDevice* dev, USBPacket* p);
void usb_cancel_packet(USBPacket* p);

void usb_ep_init(USBDevice* dev);
void usb_ep_reset(USBDevice* dev);
void usb_ep_dump(USBDevice* dev);
struct USBEndpoint* usb_ep_get(USBDevice* dev, int pid, int ep);
uint8_t usb_ep_get_type(USBDevice* dev, int pid, int ep);
void usb_ep_set_type(USBDevice* dev, int pid, int ep, uint8_t type);
void usb_ep_set_ifnum(USBDevice* dev, int pid, int ep, uint8_t ifnum);
void usb_ep_set_max_packet_size(USBDevice* dev, int pid, int ep,
								uint16_t raw);
void usb_ep_set_max_streams(USBDevice* dev, int pid, int ep, uint8_t raw);
void usb_ep_set_halted(USBDevice* dev, int pid, int ep, bool halted);
USBPacket* usb_ep_find_packet_by_id(USBDevice* dev, int pid, int ep,
									uint64_t id);

void usb_pick_speed(USBPort* port);
void usb_attach(USBPort* port);
void usb_detach(USBPort* port);
void usb_port_reset(USBPort* port);
void usb_device_reset(USBDevice* dev);
void usb_wakeup(USBEndpoint* ep, unsigned int stream);
void usb_generic_async_ctrl_complete(USBDevice* s, USBPacket* p);

void usb_reattach(USBPort* port);

USBDevice* usb_device_find_device(USBDevice* dev, uint8_t addr);

void usb_device_cancel_packet(USBDevice* dev, USBPacket* p);

void usb_device_handle_attach(USBDevice* dev);

void usb_device_handle_reset(USBDevice* dev);

void usb_device_handle_control(USBDevice* dev, USBPacket* p, int request,
							   int val, int index, int length, uint8_t* data);

void usb_device_handle_data(USBDevice* dev, USBPacket* p);

void usb_device_set_interface(USBDevice* dev, int intf,
							  int alt_old, int alt_new);

void usb_device_flush_ep_queue(USBDevice* dev, USBEndpoint* ep);

void usb_device_ep_stopped(USBDevice* dev, USBEndpoint* ep);

int usb_device_alloc_streams(USBDevice* dev, USBEndpoint** eps, int nr_eps,
							 int streams);
void usb_device_free_streams(USBDevice* dev, USBEndpoint** eps, int nr_eps);

const USBDesc* usb_device_get_usb_desc(USBDevice* dev);