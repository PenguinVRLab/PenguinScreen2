// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "deci2.h"

struct DECI2_NETMP_HEADER{
	DECI2_HEADER	h;
	u8				code,
					result;
};

struct DECI2_NETMP_CONNECT{
	u8				priority,
					_pad;
	u16				protocol;
};

char				d2_message[100];
int					d2_count=1;
DECI2_NETMP_CONNECT	d2_connect[50]={0xFF, 0, 0x400};

void D2_NETMP(const u8 *inbuffer, u8 *outbuffer, char *message){
	DECI2_NETMP_HEADER	*in=(DECI2_NETMP_HEADER*)inbuffer,
						*out=(DECI2_NETMP_HEADER*)outbuffer;
	u8	*data=(u8*)in+sizeof(DECI2_NETMP_HEADER);
	DECI2_NETMP_CONNECT	*connect=(DECI2_NETMP_CONNECT*)data;
	int					i, n;
	static char			p[100], line[1024];
	u64	EEboot, IOPboot;
	u16	node;

	memcpy(outbuffer, inbuffer, 128*1024);
	out->h.length=sizeof(DECI2_NETMP_HEADER);
	out->code++;
	out->result=0;
	switch(in->code){
		case 0:
			n=(in->h.length-sizeof(DECI2_NETMP_HEADER)) / sizeof(DECI2_NETMP_CONNECT);
			sprintf(line, "code=CONNECT");
			for (i=0; i<n; i++){
				sprintf(p, " %04X/%d", connect[i].protocol, connect[i].priority);
				strcat(line, p);
			}
			memcpy(&d2_connect[n], connect, n*sizeof(DECI2_NETMP_CONNECT));
			d2_count+=n;
			writeData(outbuffer);
			break;
		case 2:
									data+=2;
			EEboot =*(u64*)data;	data+=8;
			IOPboot=*(u64*)data;
			sprintf(line, "code=RESET EE=0x%I64X IOP=0x%I64X", EEboot, IOPboot);
			writeData(outbuffer);

			node=(u16)'I';
			sendDCMP(PROTO_DCMP, 'H', 'H', 2, 0, (char*)&node, sizeof(node));

			node=(u16)'E';
			sendDCMP(PROTO_DCMP, 'H', 'H', 2, 0, (char*)&node, sizeof(node));

			node=PROTO_ILOADP;
			sendDCMP(PROTO_DCMP, 'I', 'H', 2, 1, (char*)&node, sizeof(node));

			for (i=0; i<10; i++){
				node=PROTO_ETTYP+i;
				sendDCMP(PROTO_DCMP, 'E', 'H', 2, 1, (char*)&node, sizeof(node));

				node=PROTO_ITTYP+i;
				sendDCMP(PROTO_DCMP, 'E', 'H', 2, 1, (char*)&node, sizeof(node));
			}
			node=PROTO_ETTYP+0xF;
			sendDCMP(PROTO_DCMP, 'E', 'H', 2, 1, (char*)&node, sizeof(node));

			node=PROTO_ITTYP+0xF;
			sendDCMP(PROTO_DCMP, 'E', 'H', 2, 1, (char*)&node, sizeof(node));
			break;
		case 4:
			sprintf(line, "code=MESSAGE %s", data);
			strcpy(d2_message, (char*)data);
			writeData(outbuffer);
			break;
		case 6:
			sprintf(line, "code=STATUS");
			data=(u8*)out+sizeof(DECI2_NETMP_HEADER)+2;

			out->h.length=data-(u8*)out;
			writeData(outbuffer);
			break;
		case 8:
			sprintf(line, "code=KILL protocol=0x%04X", *(u16*)data);
			writeData(outbuffer);
			break;
		case 10:
			sprintf(line, "code=VERSION %s", data);
			data=(u8*)out+sizeof(DECI2_NETMP_HEADER);
			strcpy((char*)data, "0.2.0");data+=strlen("0.2.0");
			out->h.length=data-(u8*)out;
			writeData(outbuffer);
			break;
		default:
			sprintf(line, "code=%d[unknown] result=%d", in->code, in->result);
			writeData(outbuffer);
	}
	sprintf(message, "[NETMP %c->%c/%04X] %s", in->h.source, in->h.destination, in->h.length, line);
}
