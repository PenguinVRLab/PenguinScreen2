// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "deci2.h"

struct DECI2_DCMP_HEADER{
	DECI2_HEADER	h;
	u8				type,
					code;
	u16				_pad;
};

struct DECI2_DCMP_CONNECT{
	u8				result,
					_pad[3];
	u64				EEboot,
					IOPboot;
};

struct DECI2_DCMP_ECHO{
	u16				identifier,
					sequence;
	u8				data[32];
};

void D2_DCMP(const u8 *inbuffer, u8 *outbuffer, char *message){
	DECI2_DCMP_HEADER	*in=(DECI2_DCMP_HEADER*)inbuffer,
						*out=(DECI2_DCMP_HEADER*)outbuffer;
	u8	*data=(u8*)in+sizeof(DECI2_DCMP_HEADER);

	memcpy(outbuffer, inbuffer, 128*1024);
	out->h.length=sizeof(DECI2_DCMP_HEADER);
	out->code++;
}

void sendDCMP(u16 protocol, u8 source, u8 destination, u8 type, u8 code, char *data, int size){
	static u8 tmp[100];
	((DECI2_DCMP_HEADER*)tmp)->h.length		=sizeof(DECI2_DCMP_HEADER)+size;
	((DECI2_DCMP_HEADER*)tmp)->h._pad		=0;
	((DECI2_DCMP_HEADER*)tmp)->h.protocol	=protocol;
	((DECI2_DCMP_HEADER*)tmp)->h.source		=source;
	((DECI2_DCMP_HEADER*)tmp)->h.destination=destination;
	((DECI2_DCMP_HEADER*)tmp)->type			=type;
	((DECI2_DCMP_HEADER*)tmp)->code			=code;
	((DECI2_DCMP_HEADER*)tmp)->_pad			=0;
	memcpy(&tmp[sizeof(DECI2_DCMP_HEADER)], data, size);
	writeData(tmp);
}
