// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "deci2.h"

typedef struct tag_DECI2_DCMP_HEADER{
	DECI2_HEADER	h;
	u8				type,
					code;
	u16				_pad;
} DECI2_DCMP_HEADER;

extern char d2_message[100];

void D2_DCMP(char *inbuffer, char *outbuffer, char *message){
	DECI2_DCMP_HEADER	*in=(DECI2_DCMP_HEADER*)inbuffer,
				*out=(DECI2_DCMP_HEADER*)outbuffer;
	u8	*data=(u8*)in+sizeof(DECI2_DCMP_HEADER);

	memcpy(outbuffer, inbuffer, 128*1024);
	out->h.length=sizeof(DECI2_DCMP_HEADER);
	switch(in->type){
		case 4:
			sprintf(message, "  [DCMP] code=MESSAGE %s", data);
			strcpy(d2_message, data);
			break;
		default:
			sprintf(message, "  [DCMP] code=%d[unknown] result=%d", netmp->code, netmp->result);
	}
	result->code++;
	result->result=0;
}
