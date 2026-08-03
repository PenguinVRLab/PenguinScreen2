// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-2.0+

#include "USB/qemu-usb/qusb.h"
#include "USB/qemu-usb/queue.h"
#include "USB/qemu-usb/USBinternal.h"
#include "IopMem.h"

#include "common/Console.h"

#include <cstring>

#define DMA_DIRECTION_TO_DEVICE 0
#define DMA_DIRECTION_FROM_DEVICE 1
#define ED_LINK_LIMIT 32

extern s64 g_usb_last_cycle;
#define MIN_IRQ_INTERVAL 64

extern s64 usb_get_clock();
extern int usb_get_ticks_per_second();
extern void usbIrq(int);

static void ohci_async_cancel_device(OHCIState* ohci, USBDevice* dev);

static u64 muldiv64(u64 a, u32 b, u32 c)
{
	union
	{
		u64 ll;
		struct
		{
			u32 low, high;
		} l;
	} u, res;
	u64 rl, rh;

	u.ll = a;
	rl = static_cast<u64>(u.l.low) * static_cast<u64>(b);
	rh = static_cast<u64>(u.l.high) * static_cast<u64>(b);
	rh += (rl >> 32);
	res.l.high = rh / c;
	res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
	return res.ll;
}

static inline void ohci_intr_update(OHCIState* ohci)
{
	int level = 0;

	if ((ohci->intr & OHCI_INTR_MIE) &&
		(ohci->intr_status & ohci->intr))
		level = 1;

	if (level)
	{

		if ((usb_get_clock() - g_usb_last_cycle) > MIN_IRQ_INTERVAL)
		{
			usbIrq(1);
			g_usb_last_cycle = usb_get_clock();
		}
	}
}

static inline void ohci_set_interrupt(OHCIState* ohci, u32 intr)
{
	ohci->intr_status |= intr;
	ohci_intr_update(ohci);
}

static void ohci_die(OHCIState* ohci)
{

	Console.Warning("ohci_die: DMA error\n");

	ohci_set_interrupt(ohci, OHCI_INTR_UE);
	ohci_bus_stop(ohci);
}

static void ohci_attach(USBPort* port1)
{
	OHCIState* s = (OHCIState*)port1->opaque;
	OHCIPort* port = &s->rhport[port1->index];
	const u32 old_state = port->ctrl;

	port1->dev->port = port1;

	port->ctrl |= OHCI_PORT_CCS | OHCI_PORT_CSC;

	if (port->port.dev->speed == USB_SPEED_LOW)
	{
		port->ctrl |= OHCI_PORT_LSDA;
	}
	else
	{
		port->ctrl &= ~OHCI_PORT_LSDA;
	}

	if ((s->ctl & OHCI_CTL_HCFS) == OHCI_USB_SUSPEND)
	{
		ohci_set_interrupt(s, OHCI_INTR_RD);
	}

	if (old_state != port->ctrl)
	{
		ohci_set_interrupt(s, OHCI_INTR_RHSC);
	}
}

static void ohci_detach(USBPort* port1)
{
	OHCIState* s = (OHCIState*)port1->opaque;
	OHCIPort* port = &s->rhport[port1->index];
	const u32 old_state = port->ctrl;

	if (port1->dev)
		ohci_async_cancel_device(s, port1->dev);

	if (port->ctrl & OHCI_PORT_CCS)
	{
		port->ctrl &= ~OHCI_PORT_CCS;
		port->ctrl |= OHCI_PORT_CSC;
	}
	if (port->ctrl & OHCI_PORT_PES)
	{
		port->ctrl &= ~OHCI_PORT_PES;
		port->ctrl |= OHCI_PORT_PESC;
	}

	if (old_state != port->ctrl)
	{
		ohci_set_interrupt(s, OHCI_INTR_RHSC);
	}
}

static void ohci_wakeup(USBPort* port1)
{
	OHCIState* s = (OHCIState*)port1->opaque;
	OHCIPort* port = (OHCIPort*)&s->rhport[port1->index];
	u32 intr = 0;
	if (port->ctrl & OHCI_PORT_PSS)
	{
		port->ctrl |= OHCI_PORT_PSSC;
		port->ctrl &= ~OHCI_PORT_PSS;
		intr = OHCI_INTR_RHSC;
	}
	if ((s->ctl & OHCI_CTL_HCFS) == OHCI_USB_SUSPEND)
	{
		s->ctl &= ~OHCI_CTL_HCFS;
		s->ctl |= OHCI_USB_RESUME;
		intr = OHCI_INTR_RD;
	}
	ohci_set_interrupt(s, intr);
}

static USBDevice* ohci_find_device(OHCIState* ohci, uint8_t addr)
{
	USBDevice* dev;

	for (unsigned int i = 0; i < ohci->num_ports; i++)
	{
		if ((ohci->rhport[i].ctrl & OHCI_PORT_PES) == 0)
		{
			continue;
		}
		dev = usb_find_device(&ohci->rhport[i].port, addr);
		if (dev != nullptr)
		{
			return dev;
		}
	}
	return nullptr;
}

static void ohci_stop_endpoints(OHCIState* ohci)
{
	USBDevice* dev;

	for (unsigned int i = 0; i < ohci->num_ports; i++)
	{
		dev = ohci->rhport[i].port.dev;
		if (dev && dev->attached)
		{
			usb_device_ep_stopped(dev, &dev->ep_ctl);
			for (int j = 0; j < USB_MAX_ENDPOINTS; j++)
			{
				usb_device_ep_stopped(dev, &dev->ep_in[j]);
				usb_device_ep_stopped(dev, &dev->ep_out[j]);
			}
		}
	}
}

static void ohci_roothub_reset(OHCIState* ohci)
{
	OHCIPort* port;

	ohci_bus_stop(ohci);
	ohci->rhdesc_a = OHCI_RHA_NPS | ohci->num_ports;
	ohci->rhdesc_b = 0x0;
	ohci->rhstatus = 0;

	for (u32 i = 0; i < ohci->num_ports; i++)
	{
		port = &ohci->rhport[i];
		port->ctrl = 0;
		if (port->port.dev && port->port.dev->attached)
		{
			usb_port_reset(&port->port);
		}
	}
	if (ohci->async_td)
	{
		usb_cancel_packet(&ohci->usb_packet);
		ohci->async_td = 0;
	}
	ohci_stop_endpoints(ohci);
}

void ohci_soft_reset(OHCIState* ohci)
{
	ohci_bus_stop(ohci);
	ohci->ctl = (ohci->ctl & OHCI_CTL_IR) | OHCI_USB_SUSPEND;
	ohci->old_ctl = 0;
	ohci->status = 0;
	ohci->intr_status = 0;
	ohci->intr = OHCI_INTR_MIE;

	ohci->hcca = 0;
	ohci->ctrl_head = ohci->ctrl_cur = 0;
	ohci->bulk_head = ohci->bulk_cur = 0;
	ohci->per_cur = 0;
	ohci->done = 0;
	ohci->done_count = 7;

	ohci->fsmps = 0x2778;
	ohci->fi = 0x2edf;
	ohci->fit = 0;
	ohci->frt = 0;
	ohci->frame_number = 0;
	ohci->pstart = 0;
	ohci->lst = OHCI_LS_THRESH;
}

void ohci_hard_reset(OHCIState* ohci)
{
	ohci_soft_reset(ohci);
	ohci->ctl = 0;
	ohci_roothub_reset(ohci);
}

__fi static int get_dwords(u32 addr, u32* buf, u32 num)
{
	if ((addr + (num * sizeof(u32))) > Ps2MemSize::ExposedIopRam)
		return 0;

	std::memcpy(buf, iopMem->Main + addr, num * sizeof(u32));
	return 1;
}

__fi static int get_words(u32 addr, u16* buf, u32 num)
{
	if ((addr + (num * sizeof(u16))) > Ps2MemSize::ExposedIopRam)
		return 0;

	std::memcpy(buf, iopMem->Main + addr, num * sizeof(u16));
	return 1;
}

__fi static int put_dwords(u32 addr, u32* buf, u32 num)
{
	if ((addr + (num * sizeof(u32))) > Ps2MemSize::ExposedIopRam)
		return 0;

	std::memcpy(iopMem->Main + addr, buf, num * sizeof(u32));
	return 1;
}

__fi static int put_words(u32 addr, u16* buf, u32 num)
{
	if ((addr + (num * sizeof(u16))) > Ps2MemSize::ExposedIopRam)
		return 0;

	std::memcpy(iopMem->Main + addr, buf, num * sizeof(u16));
	return 1;
}

static inline int ohci_read_ed(OHCIState* ohci, u32 addr, struct ohci_ed* ed)
{
	return get_dwords(addr, (u32*)ed, sizeof(*ed) >> 2);
}

static inline int ohci_read_td(OHCIState* ohci, u32 addr, struct ohci_td* td)
{
	return get_dwords(addr, (u32*)td, sizeof(*td) >> 2);
}

static inline int ohci_read_iso_td(OHCIState* ohci, u32 addr, struct ohci_iso_td* td)
{
	return get_dwords(addr, (u32*)td, 4) &&
		   get_words(addr + 16, td->offset, 8);
}

static inline int ohci_put_ed(OHCIState* ohci, u32 addr, struct ohci_ed* ed)
{
	return put_dwords(addr + ED_WBACK_OFFSET,
					  (u32*)((char*)ed + ED_WBACK_OFFSET),
					  ED_WBACK_SIZE >> 2);
}

static inline int ohci_put_td(OHCIState* ohci, u32 addr, struct ohci_td* td)
{
	return put_dwords(addr, (u32*)td, sizeof(*td) >> 2);
}

static inline int ohci_put_iso_td(OHCIState* ohci, u32 addr, struct ohci_iso_td* td)
{
	return put_dwords(addr, (u32*)td, 4) &&
		   put_words(addr + 16, td->offset, 8);
}

static int ohci_copy_td(OHCIState* ohci, struct ohci_td* td, uint8_t* buf, u32 len, int write)
{
	u32 ptr = td->cbp;
	const u32 n = std::min<u32>(0x1000 - (ptr & 0xfff), len);

	if ((ptr + n) > Ps2MemSize::ExposedIopRam)
		return 1;

	if (write)
		std::memcpy(iopMem->Main + ptr, buf, len);
	else
		std::memcpy(buf, iopMem->Main + ptr, len);

	if (n == len)
		return 0;
	ptr = td->be & ~0xfffu;
	buf += n;
	len -= n;

	if ((ptr + n) > Ps2MemSize::ExposedIopRam)
		return 1;

	if (write)
		std::memcpy(iopMem->Main + ptr, buf, len);
	else
		std::memcpy(buf, iopMem->Main + ptr, len);

	return 0;
}

static int ohci_copy_iso_td(OHCIState* ohci, u32 start_addr, u32 end_addr,
							uint8_t* buf, u32 len, int write)
{
	u32 ptr = start_addr;
	const u32 n = std::min<u32>(0x1000 - (ptr & 0xfff), len);

	if ((ptr + n) > Ps2MemSize::ExposedIopRam)
		return 1;

	if (write)
		std::memcpy(iopMem->Main + ptr, buf, len);
	else
		std::memcpy(buf, iopMem->Main + ptr, len);

	if (n == len)
		return 0;
	ptr = end_addr & ~0xfffu;
	buf += n;
	len -= n;

	if ((ptr + n) > Ps2MemSize::ExposedIopRam)
		return 1;

	if (write)
		std::memcpy(iopMem->Main + ptr, buf, len);
	else
		std::memcpy(buf, iopMem->Main + ptr, len);

	return 0;
}

static void ohci_process_lists(OHCIState* ohci, int completion);

static void ohci_async_complete_packet(USBPort* port, USBPacket* packet)
{
	OHCIState* ohci = USB_CONTAINER_OF(packet, OHCIState, usb_packet);

	ohci->async_complete = true;
	ohci_process_lists(ohci, 1);
}

#define USUB(a, b) ((int16_t)((u16)(a) - (u16)(b)))

static int ohci_service_iso_td(OHCIState* ohci, struct ohci_ed* ed,
							   int completion)
{
	u32 len = 0;
	[[maybe_unused]] const char* str = nullptr;
	int pid;
	int ret;
	int i;
	struct ohci_iso_td iso_td;
	u32 next_offset;
	u32 start_addr, end_addr;

	const u32 addr = ed->head & OHCI_DPTR_MASK;

	if (!ohci_read_iso_td(ohci, addr, &iso_td))
	{
		ohci_die(ohci);
		return 1;
	}

	const u16 starting_frame = OHCI_BM(iso_td.flags, TD_SF);
	const int frame_count = OHCI_BM(iso_td.flags, TD_FC);
	const s16 relative_frame_number = USUB(ohci->frame_number, starting_frame);

	if (relative_frame_number < 0)
	{
		return 1;
	}
	else if (relative_frame_number > frame_count)
	{
		if (OHCI_CC_DATAOVERRUN == OHCI_BM(iso_td.flags, TD_CC))
		{
			return 1;
		}
		OHCI_SET_BM(iso_td.flags, TD_CC, OHCI_CC_DATAOVERRUN);
		ed->head &= ~OHCI_DPTR_MASK;
		ed->head |= (iso_td.next & OHCI_DPTR_MASK);
		iso_td.next = ohci->done;
		ohci->done = addr;
		i = OHCI_BM(iso_td.flags, TD_DI);
		if (i < ohci->done_count)
			ohci->done_count = i;
		if (!ohci_put_iso_td(ohci, addr, &iso_td))
		{
			ohci_die(ohci);
			return 1;
		}
		return 0;
	}

	const int dir = OHCI_BM(ed->flags, ED_D);
	switch (dir)
	{
		case OHCI_TD_DIR_IN:
			str = "in";
			pid = USB_TOKEN_IN;
			break;
		case OHCI_TD_DIR_OUT:
			str = "out";
			pid = USB_TOKEN_OUT;
			break;
		case OHCI_TD_DIR_SETUP:
			str = "setup";
			pid = USB_TOKEN_SETUP;
			break;
		default:
			return 1;
	}

	if (!iso_td.bp || !iso_td.be)
	{
		return 1;
	}

	const u32 start_offset = iso_td.offset[relative_frame_number];
	if (relative_frame_number < frame_count)
	{
		next_offset = iso_td.offset[relative_frame_number + 1];
	}
	else
	{
		next_offset = iso_td.be;
	}

	if (!(OHCI_BM(start_offset, TD_PSW_CC) & 0xe) ||
		((relative_frame_number < frame_count) &&
		 !(OHCI_BM(next_offset, TD_PSW_CC) & 0xe)))
	{
		return 1;
	}

	if ((relative_frame_number < frame_count) && (start_offset > next_offset))
	{
		return 1;
	}

	if ((start_offset & 0x1000) == 0)
	{
		start_addr = (iso_td.bp & OHCI_PAGE_MASK) |
					 (start_offset & OHCI_OFFSET_MASK);
	}
	else
	{
		start_addr = (iso_td.be & OHCI_PAGE_MASK) |
					 (start_offset & OHCI_OFFSET_MASK);
	}

	if (relative_frame_number < frame_count)
	{
		const u32 end_offset = next_offset - 1;
		if ((end_offset & 0x1000) == 0)
		{
			end_addr = (iso_td.bp & OHCI_PAGE_MASK) |
					   (end_offset & OHCI_OFFSET_MASK);
		}
		else
		{
			end_addr = (iso_td.be & OHCI_PAGE_MASK) |
					   (end_offset & OHCI_OFFSET_MASK);
		}
	}
	else
	{
		end_addr = next_offset;
	}

	if (start_addr > end_addr)
	{
		return 1;
	}

	if ((start_addr & OHCI_PAGE_MASK) != (end_addr & OHCI_PAGE_MASK))
	{
		len = (end_addr & OHCI_OFFSET_MASK) + 0x1001 - (start_addr & OHCI_OFFSET_MASK);
	}
	else
	{
		len = end_addr - start_addr + 1;
	}
	if (len > sizeof(ohci->usb_buf))
	{
		len = sizeof(ohci->usb_buf);
	}

	if (len && dir != OHCI_TD_DIR_IN)
	{
		if (ohci_copy_iso_td(ohci, start_addr, end_addr, ohci->usb_buf, len,
							 DMA_DIRECTION_TO_DEVICE))
		{
			ohci_die(ohci);
			return 1;
		}
	}

	if (!completion)
	{
		const bool int_req = relative_frame_number == frame_count &&
					   OHCI_BM(iso_td.flags, TD_DI) == 0;
		USBDevice* dev = ohci_find_device(ohci, OHCI_BM(ed->flags, ED_FA));
		if (dev == nullptr)
		{
			return 1;
		}
		USBEndpoint* ep = usb_ep_get(dev, pid, OHCI_BM(ed->flags, ED_EN));
		usb_packet_setup(&ohci->usb_packet, pid, ep, 0, addr, false, int_req);
		usb_packet_addbuf(&ohci->usb_packet, ohci->usb_buf, len);
		usb_handle_packet(dev, &ohci->usb_packet);
		if (ohci->usb_packet.status == USB_RET_ASYNC)
		{
			usb_device_flush_ep_queue(dev, ep);
			return 1;
		}
	}
	if (ohci->usb_packet.status == USB_RET_SUCCESS)
	{
		ret = ohci->usb_packet.actual_length;
	}
	else
	{
		ret = ohci->usb_packet.status;
	}

	if (dir == OHCI_TD_DIR_IN && ret >= 0 && ret <= (int)len)
	{
		if (ohci_copy_iso_td(ohci, start_addr, end_addr, ohci->usb_buf, ret,
							 DMA_DIRECTION_FROM_DEVICE))
		{
			ohci_die(ohci);
			return 1;
		}
		OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
					OHCI_CC_NOERROR);
		OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE, ret);
	}
	else if (dir == OHCI_TD_DIR_OUT && (ret == (int)len))
	{
		OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
					OHCI_CC_NOERROR);
		OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE, 0);
	}
	else
	{
		if (ret > static_cast<s32>(len))
		{
			OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
						OHCI_CC_DATAOVERRUN);
			OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE,
						len);
		}
		else if (ret >= 0)
		{
			OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
						OHCI_CC_DATAUNDERRUN);
		}
		else
		{
			switch (ret)
			{
				case USB_RET_IOERROR:
				case USB_RET_NODEV:
					OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
								OHCI_CC_DEVICENOTRESPONDING);
					OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE,
								0);
					break;
				case USB_RET_NAK:
				case USB_RET_STALL:
					OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
								OHCI_CC_STALL);
					OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_SIZE,
								0);
					break;
				default:
					OHCI_SET_BM(iso_td.offset[relative_frame_number], TD_PSW_CC,
								OHCI_CC_UNDEXPETEDPID);
					break;
			}
		}
	}

	if (relative_frame_number == frame_count)
	{
		OHCI_SET_BM(iso_td.flags, TD_CC, OHCI_CC_NOERROR);
		ed->head &= ~OHCI_DPTR_MASK;
		ed->head |= (iso_td.next & OHCI_DPTR_MASK);
		iso_td.next = ohci->done;
		ohci->done = addr;
		i = OHCI_BM(iso_td.flags, TD_DI);
		if (i < ohci->done_count)
			ohci->done_count = i;
	}
	if (!ohci_put_iso_td(ohci, addr, &iso_td))
	{
		ohci_die(ohci);
	}
	return 1;
}

static int ohci_service_td(OHCIState* ohci, struct ohci_ed* ed)
{
	u32 len = 0, pktlen = 0;
	[[maybe_unused]]const char* str = nullptr;
	int pid;
	int ret;
	int i;
	USBDevice* dev;
	USBEndpoint* ep;
	struct ohci_td td;

	const u32 addr = ed->head & OHCI_DPTR_MASK;
	const int completion = (addr == ohci->async_td);
	if (completion && !ohci->async_complete)
	{
		return 1;
	}
	if (!ohci_read_td(ohci, addr, &td))
	{
		ohci_die(ohci);
		return 1;
	}

	int dir = OHCI_BM(ed->flags, ED_D);
	switch (dir)
	{
		case OHCI_TD_DIR_OUT:
		case OHCI_TD_DIR_IN:
			break;
		default:
			dir = OHCI_BM(td.flags, TD_DP);
			break;
	}

	switch (dir)
	{
		case OHCI_TD_DIR_IN:
			str = "in";
			pid = USB_TOKEN_IN;
			break;
		case OHCI_TD_DIR_OUT:
			str = "out";
			pid = USB_TOKEN_OUT;
			break;
		case OHCI_TD_DIR_SETUP:
			str = "setup";
			pid = USB_TOKEN_SETUP;
			break;
		default:
			return 1;
	}
	if (td.cbp && td.be)
	{
		if ((td.cbp & 0xfffff000) != (td.be & 0xfffff000))
		{
			len = (td.be & 0xfff) + 0x1001 - (td.cbp & 0xfff);
		}
		else
		{
			if (td.cbp > td.be)
			{
				ohci_die(ohci);
				return 1;
			}
			len = (td.be - td.cbp) + 1;
		}
		if (len > sizeof(ohci->usb_buf))
		{
			len = sizeof(ohci->usb_buf);
		}

		pktlen = len;
		if (len && dir != OHCI_TD_DIR_IN)
		{
			pktlen = (ed->flags & OHCI_ED_MPS_MASK) >> OHCI_ED_MPS_SHIFT;
			if (pktlen > len)
			{
				pktlen = len;
			}
			if (!completion)
			{
				if (ohci_copy_td(ohci, &td, ohci->usb_buf, pktlen,
								 DMA_DIRECTION_TO_DEVICE))
				{
					ohci_die(ohci);
				}
			}
		}
	}

	const int flag_r = (td.flags & OHCI_TD_R) != 0;

	if (completion)
	{
		ohci->async_td = 0;
		ohci->async_complete = false;
	}
	else
	{
		if (ohci->async_td)
		{
			return 1;
		}
		dev = ohci_find_device(ohci, OHCI_BM(ed->flags, ED_FA));
		if (dev == nullptr)
		{
			return 1;
		}
		ep = usb_ep_get(dev, pid, OHCI_BM(ed->flags, ED_EN));
		usb_packet_setup(&ohci->usb_packet, pid, ep, 0, addr, !flag_r,
						 OHCI_BM(td.flags, TD_DI) == 0);
		usb_packet_addbuf(&ohci->usb_packet, ohci->usb_buf, pktlen);
		usb_handle_packet(dev, &ohci->usb_packet);

		if (ohci->usb_packet.status == USB_RET_ASYNC)
		{
			usb_device_flush_ep_queue(dev, ep);
			ohci->async_td = addr;
			return 1;
		}
	}
	if (ohci->usb_packet.status == USB_RET_SUCCESS)
	{
		ret = ohci->usb_packet.actual_length;
	}
	else
	{
		ret = ohci->usb_packet.status;
	}

	if (ret >= 0)
	{
		if (dir == OHCI_TD_DIR_IN)
		{
			if (ohci_copy_td(ohci, &td, ohci->usb_buf, ret,
							 DMA_DIRECTION_FROM_DEVICE))
			{
				ohci_die(ohci);
			}
		}
		else
		{
			ret = pktlen;
		}
	}

	if (ret == (int)pktlen || (dir == OHCI_TD_DIR_IN && ret >= 0 && flag_r))
	{
		if (ret == (int)len)
		{
			td.cbp = 0;
		}
		else
		{
			if ((td.cbp & 0xfff) + ret > 0xfff)
			{
				td.cbp = (td.be & ~0xfff) + ((td.cbp + ret) & 0xfff);
			}
			else
			{
				td.cbp += ret;
			}
		}
		td.flags |= OHCI_TD_T1;
		td.flags ^= OHCI_TD_T0;
		OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_NOERROR);
		OHCI_SET_BM(td.flags, TD_EC, 0);

		if ((dir != OHCI_TD_DIR_IN) && (ret != (int)len))
		{
			goto exit_no_retire;
		}

		ed->head &= ~OHCI_ED_C;
		if (td.flags & OHCI_TD_T0)
			ed->head |= OHCI_ED_C;
	}
	else
	{
		if (ret >= 0)
		{
			OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_DATAUNDERRUN);
		}
		else
		{
			switch (ret)
			{
				case USB_RET_IOERROR:
				case USB_RET_NODEV:
					OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_DEVICENOTRESPONDING);
					break;
				case USB_RET_NAK:
					return 1;
				case USB_RET_STALL:
					OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_STALL);
					break;
				case USB_RET_BABBLE:
					OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_DATAOVERRUN);
					break;
				default:
					OHCI_SET_BM(td.flags, TD_CC, OHCI_CC_UNDEXPETEDPID);
					OHCI_SET_BM(td.flags, TD_EC, 3);
					break;
			}
			ohci->done_count = 0;
		}
		ed->head |= OHCI_ED_H;
	}

	ed->head &= ~OHCI_DPTR_MASK;
	ed->head |= td.next & OHCI_DPTR_MASK;
	td.next = ohci->done;
	ohci->done = addr;
	i = OHCI_BM(td.flags, TD_DI);
	if (i < ohci->done_count)
		ohci->done_count = i;
exit_no_retire:
	if (!ohci_put_td(ohci, addr, &td))
	{
		ohci_die(ohci);
		return 1;
	}
	return OHCI_BM(td.flags, TD_CC) != OHCI_CC_NOERROR;
}

static int ohci_service_ed_list(OHCIState* ohci, u32 head, int completion)
{
	struct ohci_ed ed;
	u32 next_ed = 0;
	u32 cur;
	int active;
	u32 link_cnt = 0;
	active = 0;

	if (head == 0)
		return 0;

	for (cur = head; cur && link_cnt++ < ED_LINK_LIMIT; cur = next_ed)
	{
		if (!ohci_read_ed(ohci, cur, &ed))
		{
			ohci_die(ohci);
			return 0;
		}

		next_ed = ed.next & OHCI_DPTR_MASK;

		if ((ed.head & OHCI_ED_H) || (ed.flags & OHCI_ED_K))
		{
			u32 addr;
			addr = ed.head & OHCI_DPTR_MASK;
			if (ohci->async_td && addr == ohci->async_td)
			{
				usb_cancel_packet(&ohci->usb_packet);
				ohci->async_td = 0;
				usb_device_ep_stopped(ohci->usb_packet.ep->dev,
									  ohci->usb_packet.ep);
			}
			continue;
		}

		while ((ed.head & OHCI_DPTR_MASK) != ed.tail)
		{

			active = 1;

			if ((ed.flags & OHCI_ED_F) == 0)
			{
				if (ohci_service_td(ohci, &ed))
					break;
			}
			else
			{
				if (ohci_service_iso_td(ohci, &ed, completion))
					break;
			}
		}

		if (!ohci_put_ed(ohci, cur, &ed))
		{
			ohci_die(ohci);
			return 0;
		}
	}

	return active;
}

static void ohci_sof(OHCIState* ohci)
{
	ohci->sof_time = usb_get_clock();
	ohci->eof_timer = g_usb_frame_time;
	ohci_set_interrupt(ohci, OHCI_INTR_SF);
}

static void ohci_process_lists(OHCIState* ohci, int completion)
{
	if ((ohci->ctl & OHCI_CTL_CLE) && (ohci->status & OHCI_STATUS_CLF))
	{
		if (ohci->ctrl_cur && ohci->ctrl_cur != ohci->ctrl_head)
		{
		}
		if (!ohci_service_ed_list(ohci, ohci->ctrl_head, completion))
		{
			ohci->ctrl_cur = 0;
			ohci->status &= ~OHCI_STATUS_CLF;
		}
	}

	if ((ohci->ctl & OHCI_CTL_BLE) && (ohci->status & OHCI_STATUS_BLF))
	{
		if (!ohci_service_ed_list(ohci, ohci->bulk_head, completion))
		{
			ohci->bulk_cur = 0;
			ohci->status &= ~OHCI_STATUS_BLF;
		}
	}
}

void ohci_frame_boundary(void* opaque)
{
	OHCIState* ohci = (OHCIState*)opaque;

	if (ohci->hcca + sizeof(ohci_hcca) > Ps2MemSize::ExposedIopRam)
	{
		Console.Error("ohci->hcca pointer is out of range.");
		return;
	}

	ohci_hcca* hcca = reinterpret_cast<ohci_hcca*>(iopMem->Main + ohci->hcca);

	if (ohci->ctl & OHCI_CTL_PLE)
	{
		const int n = ohci->frame_number & 0x1f;
		ohci_service_ed_list(ohci, hcca->intr[n], 0);
	}

	if (ohci->old_ctl & (~ohci->ctl) & (OHCI_CTL_BLE | OHCI_CTL_CLE))
	{
		if (ohci->async_td)
		{
			usb_cancel_packet(&ohci->usb_packet);
			ohci->async_td = 0;
		}
		ohci_stop_endpoints(ohci);
	}
	ohci->old_ctl = ohci->ctl;
	ohci_process_lists(ohci, 0);

	if (ohci->intr_status & OHCI_INTR_UE)
	{
		return;
	}

	ohci->frt = ohci->fit;

	ohci->frame_number = (ohci->frame_number + 1) & 0xffff;
	hcca->frame = ohci->frame_number;

	if (ohci->done_count == 0 && !(ohci->intr_status & OHCI_INTR_WD))
	{
		if (!ohci->done)
			abort();
		if (ohci->intr & ohci->intr_status)
			ohci->done |= 1;
		hcca->done = ohci->done;
		ohci->done = 0;
		ohci->done_count = 7;
		ohci_set_interrupt(ohci, OHCI_INTR_WD);
	}

	if (ohci->done_count != 7 && ohci->done_count != 0)
		ohci->done_count--;

	ohci_sof(ohci);
}

int ohci_bus_start(OHCIState* ohci)
{
	ohci->eof_timer = 0;


	ohci_sof(ohci);

	return 1;
}

void ohci_bus_stop(OHCIState* ohci)
{
	if (ohci->eof_timer)
		ohci->eof_timer = 0;
}

static int ohci_port_set_if_connected(OHCIState* ohci, int i, u32 val)
{
	int ret = 1;

	if (val == 0)
		return 0;

	if (!(ohci->rhport[i].ctrl & OHCI_PORT_CCS))
	{
		ohci->rhport[i].ctrl |= OHCI_PORT_CSC;
		if (ohci->rhstatus & OHCI_RHS_DRWE)
		{
		}
		return 0;
	}

	if (ohci->rhport[i].ctrl & val)
		ret = 0;

	ohci->rhport[i].ctrl |= val;

	return ret;
}

static void ohci_set_frame_interval(OHCIState* ohci, u16 val)
{
	val &= OHCI_FMI_FI;

	if (val != ohci->fi)
	{
	}

	ohci->fi = val;
}

static void ohci_port_power(OHCIState* ohci, int i, int p)
{
	if (p)
	{
		ohci->rhport[i].ctrl |= OHCI_PORT_PPS;
	}
	else
	{
		ohci->rhport[i].ctrl &= ~(OHCI_PORT_PPS |
								  OHCI_PORT_CCS |
								  OHCI_PORT_PSS |
								  OHCI_PORT_PRS);
	}
}

static void ohci_set_ctl(OHCIState* ohci, u32 val)
{
	const u32 old_state = ohci->ctl & OHCI_CTL_HCFS;
	ohci->ctl = val;
	const u32 new_state = ohci->ctl & OHCI_CTL_HCFS;

	if (old_state == new_state)
		return;

	switch (new_state)
	{
		case OHCI_USB_OPERATIONAL:
			ohci_bus_start(ohci);
			break;
		case OHCI_USB_SUSPEND:
			ohci_bus_stop(ohci);
			ohci->intr_status &= ~OHCI_INTR_SF;
			ohci_intr_update(ohci);
			break;
		case OHCI_USB_RESUME:
			break;
		case OHCI_USB_RESET:
			ohci_roothub_reset(ohci);
			break;
	}
}

static u32 ohci_get_frame_remaining(OHCIState* ohci)
{
	u16 fr;
	s64 tks;

	if ((ohci->ctl & OHCI_CTL_HCFS) != OHCI_USB_OPERATIONAL)
		return (ohci->frt << 31);

	tks = usb_get_clock() - ohci->sof_time;

	if (tks >= g_usb_frame_time)
		return (ohci->frt << 31);

	tks = muldiv64(1, tks, g_usb_bit_time);
	fr = (u16)(ohci->fi - tks);

	return (ohci->frt << 31) | fr;
}


static void ohci_set_hub_status(OHCIState* ohci, u32 val)
{
	const u32 old_state = ohci->rhstatus;

	if (val & OHCI_RHS_OCIC)
		ohci->rhstatus &= ~OHCI_RHS_OCIC;

	if (val & OHCI_RHS_LPS)
	{
		for (unsigned int i = 0; i < ohci->num_ports; i++)
			ohci_port_power(ohci, i, 0);
	}

	if (val & OHCI_RHS_LPSC)
	{
		for (unsigned int i = 0; i < ohci->num_ports; i++)
			ohci_port_power(ohci, i, 1);
	}

	if (val & OHCI_RHS_DRWE)
		ohci->rhstatus |= OHCI_RHS_DRWE;

	if (val & OHCI_RHS_CRWE)
		ohci->rhstatus &= ~OHCI_RHS_DRWE;

	if (old_state != ohci->rhstatus)
		ohci_set_interrupt(ohci, OHCI_INTR_RHSC);
}

static void ohci_port_set_status(OHCIState* ohci, int portnum, u32 val)
{
	OHCIPort* port = &ohci->rhport[portnum];
	const u32 old_state = port->ctrl;

	if (val & OHCI_PORT_WTC)
		port->ctrl &= ~(val & OHCI_PORT_WTC);

	if (val & OHCI_PORT_CCS)
		port->ctrl &= ~OHCI_PORT_PES;

	ohci_port_set_if_connected(ohci, portnum, val & OHCI_PORT_PES);

	if (ohci_port_set_if_connected(ohci, portnum, val & OHCI_PORT_PSS))
	{
	}

	if (ohci_port_set_if_connected(ohci, portnum, val & OHCI_PORT_PRS))
	{
		usb_device_reset(port->port.dev);
		port->ctrl &= ~OHCI_PORT_PRS;
		port->ctrl |= OHCI_PORT_PES | OHCI_PORT_PRSC;
	}

	if (val & OHCI_PORT_LSDA)
		ohci_port_power(ohci, portnum, 0);
	if (val & OHCI_PORT_PPS)
		ohci_port_power(ohci, portnum, 1);

	if (old_state != port->ctrl)
		ohci_set_interrupt(ohci, OHCI_INTR_RHSC);
}

#ifdef DEBUG_OHCI
static const char* reg_names[] = {
	"HcRevision",
	"HcControl",
	"HcCommandStatus",
	"HcInterruptStatus",
	"HcInterruptEnable",
	"HcInterruptDisable",
	"HcHCCA",
	"HcPeriodCurrentED",
	"HcControlHeadED",
	"HcControlCurrentED",
	"HcBulkHeadED",
	"HcBulkCurrentED",
	"HcDoneHead",
	"HcFmInterval",
	"HcFmRemaining",
	"HcFmNumber",
	"HcPeriodicStart",
	"HcLSThreshold",
	"HcRhDescriptorA",
	"HcRhDescriptorB",
	"HcRhStatus",
};

u32 ohci_mem_read_impl(OHCIState* ptr, u32 addr);
u32 ohci_mem_read(OHCIState* ptr, u32 addr)
{
	auto val = ohci_mem_read_impl(ptr, addr);
	int idx = (addr - ptr->mem_base) >> 2;
	if (idx < countof(reg_names))
	{
		Console.Warning("ohci_mem_read %s(%d): %08x\n", reg_names[idx], idx, val);
	}
	return val;
}

u32 ohci_mem_read_impl(OHCIState* ptr, u32 addr)
#else
u32 ohci_mem_read(OHCIState* ptr, u32 addr)
#endif
{
	OHCIState* ohci = ptr;

	addr -= ohci->mem_base;

	if (addr & 3)
	{
		return 0xffffffff;
	}

	if (addr >= 0x54 && addr < 0x54 + ohci->num_ports * 4)
	{
		return ohci->rhport[(addr - 0x54) >> 2].ctrl | OHCI_PORT_PPS;
	}
	switch (addr >> 2)
	{
		case 0:
			return 0x10;

		case 1:
			return ohci->ctl;

		case 2:
			return ohci->status;

		case 3:
			return ohci->intr_status;

		case 4:
		case 5:
			return ohci->intr;

		case 6:
			return ohci->hcca;

		case 7:
			return ohci->per_cur;

		case 8:
			return ohci->ctrl_head;

		case 9:
			return ohci->ctrl_cur;

		case 10:
			return ohci->bulk_head;

		case 11:
			return ohci->bulk_cur;

		case 12:
			return ohci->done;

		case 13:
			return (ohci->fit << 31) | (ohci->fsmps << 16) | (ohci->fi);

		case 14:
			return ohci_get_frame_remaining(ohci);

		case 15:
			return ohci->frame_number;

		case 16:
			return ohci->pstart;

		case 17:
			return ohci->lst;

		case 18:
			return ohci->rhdesc_a;

		case 19:
			return ohci->rhdesc_b;

		case 20:
			return ohci->rhstatus;

		default:
			return 0xffffffff;
	}
}

#ifdef DEBUG_OHCI
void ohci_mem_write_impl(OHCIState* ptr, u32 addr, u32 val);
void ohci_mem_write(OHCIState* ptr, u32 addr, u32 val)
{
	int idx = (addr - ptr->mem_base) >> 2;
	if (idx < countof(reg_names))
	{
		Console.Warning("ohci_mem_write %s(%d): %08x\n", reg_names[idx], idx, val);
	}
	ohci_mem_write_impl(ptr, addr, val);
}

void ohci_mem_write_impl(OHCIState* ptr, u32 addr, u32 val)
#else
void ohci_mem_write(OHCIState* ptr, u32 addr, u32 val)
#endif
{
	OHCIState* ohci = ptr;

	addr -= ohci->mem_base;

	if (addr & 3)
	{
		Console.Warning("usb-ohci: Mis-aligned write\n");
		return;
	}

	if ((addr >= 0x54) && (addr < (0x54 + ohci->num_ports * 4)))
	{
		ohci_port_set_status(ohci, (addr - 0x54) >> 2, val);
		return;
	}
	switch (addr >> 2)
	{
		case 1:
			ohci_set_ctl(ohci, val);
			break;

		case 2:
			val = (val & ~OHCI_STATUS_SOC);

			ohci->status |= val;

			if (ohci->status & OHCI_STATUS_HCR)
				ohci_soft_reset(ohci);
			break;

		case 3:
			ohci->intr_status &= ~val;
			ohci_intr_update(ohci);
			break;

		case 4:
			ohci->intr |= val;
			ohci_intr_update(ohci);
			break;

		case 5:
			ohci->intr &= ~val;
			ohci_intr_update(ohci);
			break;

		case 6:
			ohci->hcca = val & OHCI_HCCA_MASK;
			break;

		case 8:
			ohci->ctrl_head = val & OHCI_EDPTR_MASK;
			break;

		case 9:
			ohci->ctrl_cur = val & OHCI_EDPTR_MASK;
			break;

		case 10:
			ohci->bulk_head = val & OHCI_EDPTR_MASK;
			break;

		case 11:
			ohci->bulk_cur = val & OHCI_EDPTR_MASK;
			break;

		case 13:
			ohci->fsmps = (val & OHCI_FMI_FSMPS) >> 16;
			ohci->fit = (val & OHCI_FMI_FIT) >> 31;
			ohci_set_frame_interval(ohci, val);
			break;

		case 16:
			ohci->pstart = val & 0xffff;
			break;

		case 17:
			ohci->lst = val & 0xffff;
			break;

		case 18:
			ohci->rhdesc_a &= ~OHCI_RHA_RW_MASK;
			ohci->rhdesc_a |= val & OHCI_RHA_RW_MASK;
			break;

		case 19:
			break;

		case 20:
			ohci_set_hub_status(ohci, val);
			break;

		default:
			break;
	}
}

static void ohci_async_cancel_device(OHCIState* ohci, USBDevice* dev)
{
	if (ohci->async_td &&
		usb_packet_is_inflight(&ohci->usb_packet) &&
		ohci->usb_packet.ep->dev == dev)
	{
		usb_cancel_packet(&ohci->usb_packet);
		ohci->async_td = 0;
	}
}

static USBPortOps ohci_port_ops = {
ohci_attach,
ohci_detach,
ohci_wakeup,
ohci_async_complete_packet,
};

OHCIState* ohci_create(u32 base, int ports)
{
	OHCIState* ohci = (OHCIState*)malloc(sizeof(OHCIState));
	if (!ohci)
		return nullptr;
	int i;

	const int ticks_per_sec = usb_get_ticks_per_second();

	std::memset(ohci, 0, sizeof(OHCIState));

	ohci->mem_base = base;

	if (g_usb_frame_time == 0)
	{
#if OHCI_TIME_WARP
		g_usb_frame_time = ticks_per_sec;
		g_usb_bit_time = muldiv64(1, ticks_per_sec, USB_HZ / 1000);
#else
		g_usb_frame_time = muldiv64(1, ticks_per_sec, 1000);
		if (ticks_per_sec >= USB_HZ)
		{
			g_usb_bit_time = muldiv64(1, ticks_per_sec, USB_HZ);
		}
		else
		{
			g_usb_bit_time = 1;
		}
#endif
	}

	ohci->num_ports = ports;
	for (i = 0; i < ports; i++)
	{
		std::memset(&(ohci->rhport[i].port), 0, sizeof(USBPort));
		ohci->rhport[i].port.opaque = ohci;
		ohci->rhport[i].port.index = i;
		ohci->rhport[i].port.speedmask = USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL;
		ohci->rhport[i].port.ops = &ohci_port_ops;
	}

	ohci_hard_reset(ohci);
	return ohci;
}
