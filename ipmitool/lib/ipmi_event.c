/*
 * Copyright (c) 2003 Sun Microsystems, Inc.  All Rights Reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistribution of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * Redistribution in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * Neither the name of Sun Microsystems, Inc. or the names of
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * 
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * SUN MICROSYSTEMS, INC. ("SUN") AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * SUN OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF SUN HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 * 
 * You acknowledge that this software is not designed or intended for use
 * in the design, construction, operation or maintenance of any nuclear
 * facility.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>

#include <ipmitool/ipmi.h>
#include <ipmitool/ipmi_intf.h>
#include <ipmitool/helper.h>
#include <ipmitool/log.h>
#include <ipmitool/ipmi_sel.h>
#include <ipmitool/ipmi_strings.h>
#include <ipmitool/ipmi_channel.h>

static int
ipmi_send_platform_event(struct ipmi_intf * intf, int num)
{
	struct ipmi_rs * rsp;
	struct ipmi_rq req;
	uint8_t rqdata[8];
	uint8_t chmed;
	int p = 0;

	ipmi_intf_session_set_privlvl(intf, IPMI_SESSION_PRIV_ADMIN);

	memset(&req, 0, sizeof(req));
	memset(rqdata, 0, 8);

	req.msg.netfn = IPMI_NETFN_SE;
	req.msg.cmd = 0x02;
	req.msg.data = rqdata;
	req.msg.data_len = 7;

	chmed = ipmi_current_channel_medium(intf);
	if (chmed == IPMI_CHANNEL_MEDIUM_SYSTEM) {
		/* system interface, need extra generator ID */
		req.msg.data_len = 8;
		rqdata[p++] = 0x20;
	}

	/* IPMB/LAN/etc */
	switch (num) {
	case 1:			/* temperature */
		printf("Sending SAMPLE event: Temperature - "
		       "Upper Critical - Going High\n");
		rqdata[p++] = 0x04;	/* EvMRev */
		rqdata[p++] = 0x01;	/* Sensor Type */
		rqdata[p++] = 0x30;	/* Sensor # */
		rqdata[p++] = 0x01;	/* Event Dir / Event Type */
		rqdata[p++] = 0x59;	/* Event Data 1 */
		rqdata[p++] = 0x00;	/* Event Data 2 */
		rqdata[p++] = 0x00;	/* Event Data 3 */
		break;
	case 2:			/* voltage error */
		printf("Sending SAMPLE event: Voltage Threshold - "
		       "Lower Critical - Going Low\n");
		rqdata[p++] = 0x04;	/* EvMRev */
		rqdata[p++] = 0x02;	/* Sensor Type */
		rqdata[p++] = 0x60;	/* Sensor # */
		rqdata[p++] = 0x01;	/* Event Dir / Event Type */
		rqdata[p++] = 0x52;	/* Event Data 1 */
		rqdata[p++] = 0x00;	/* Event Data 2 */
		rqdata[p++] = 0x00;	/* Event Data 3 */
		break;
	case 3:			/* correctable ECC */
		printf("Sending SAMPLE event: Memory - Correctable ECC\n");
		rqdata[p++] = 0x04;	/* EvMRev */
		rqdata[p++] = 0x0c;	/* Sensor Type */
		rqdata[p++] = 0x53;	/* Sensor # */
		rqdata[p++] = 0x6f;	/* Event Dir / Event Type */
		rqdata[p++] = 0x00;	/* Event Data 1 */
		rqdata[p++] = 0x00;	/* Event Data 2 */
		rqdata[p++] = 0x00;	/* Event Data 3 */
		break;
	default:
		lprintf(LOG_ERR, "Invalid event number: %d", num);
		return -1;
	}

	rsp = intf->sendrecv(intf, &req);
	if (rsp == NULL) {
		lprintf(LOG_ERR, "Platform Event Message command failed");
		return -1;
	}
	else if (rsp->ccode > 0) {
		lprintf(LOG_ERR, "Platform Event Message command failed: %s",
			val2str(rsp->ccode, completion_code_vals));
		return -1;
	}

	return 0;
}

static int
ipmi_event_fromfile(struct ipmi_intf * intf, char * file)
{
	FILE * fp;
	struct ipmi_rs * rsp;
	struct ipmi_rq req;
	struct sel_event_record sel_event;
	uint8_t rqdata[8];
	char buf[1024];
	char * ptr, * tok;
	int i, j;
	uint8_t chmed;
	int rc = 0;

	if (file == NULL)
		return -1;

	/* must be admin privilege to do this */
	ipmi_intf_session_set_privlvl(intf, IPMI_SESSION_PRIV_ADMIN);

	memset(rqdata, 0, 8);

	/* setup Platform Event Message command */
	memset(&req, 0, sizeof(req));
	req.msg.netfn = IPMI_NETFN_SE;
	req.msg.cmd = 0x02;
	req.msg.data = rqdata;
	req.msg.data_len = 7;

	chmed = ipmi_current_channel_medium(intf);
	if (chmed == IPMI_CHANNEL_MEDIUM_SYSTEM) {
		/* system interface, need extra generator ID */
		rqdata[0] = 0x20;
		req.msg.data_len = 8;
	}

	fp = ipmi_open_file_read(file);
	if (fp == NULL)
		return -1;

	while (feof(fp) != 0) {
		if (fgets(buf, 1024, fp) == NULL)
			continue;

		/* clip off optional comment tail indicated by # */
		ptr = strchr(buf, '#');
		if (ptr)
			*ptr = '\0';
		else
			ptr = buf + strlen(buf);

		/* clip off trailing and leading whitespace */
		ptr--;
		while (isspace(*ptr) && ptr >= buf)
			*ptr-- = '\0';
		ptr = buf;
		while (isspace(*ptr))
			ptr++;
		if (strlen(ptr) == 0)
			continue;

		/* parse the event, 7 bytes with optional comment */
		/* 0x00 0x00 0x00 0x00 0x00 0x00 0x00 # event */
		i = 0;
		tok = strtok(ptr, " ");
		while (tok) {
			if (i == 7)
				break;
			j = i++;
			if (chmed == IPMI_CHANNEL_MEDIUM_SYSTEM)
				j++;
			rqdata[j] = (uint8_t)strtol(tok, NULL, 0);
			tok = strtok(NULL, " ");
		}
		if (i < 7) {
			lprintf(LOG_ERR, "Invalid Event: %s",
			       buf2str(rqdata, sizeof(rqdata)));
			continue;
		}

		memset(&sel_event, 0, sizeof(struct sel_event_record));
		sel_event.record_id = 0;
		sel_event.gen_id = 2;

		j = (chmed == IPMI_CHANNEL_MEDIUM_SYSTEM) ? 1 : 0;
		sel_event.evm_rev = rqdata[j++];
		sel_event.sensor_type = rqdata[j++];
		sel_event.sensor_num = rqdata[j++];
		sel_event.event_type = rqdata[j] & 0x7f;
		sel_event.event_dir = (rqdata[j++] & 0x80) >> 7;
		sel_event.event_data[0] = rqdata[j++];
		sel_event.event_data[1] = rqdata[j++];
		sel_event.event_data[2] = rqdata[j++];

		ipmi_sel_print_std_entry(&sel_event);
		
		rsp = intf->sendrecv(intf, &req);
		if (rsp == NULL) {
			lprintf(LOG_ERR, "Platform Event Message command failed");
			rc = -1;
		}
		else if (rsp->ccode > 0) {
			lprintf(LOG_ERR, "Platform Event Message command failed: %s",
				val2str(rsp->ccode, completion_code_vals));
			rc = -1;
		}
	}

	fclose(fp);
	return rc;
}

int
ipmi_event_main(struct ipmi_intf * intf, int argc, char ** argv)
{
	uint8_t c;
	int rc = 0;

	if (argc == 0 || strncmp(argv[0], "help", 4) == 0) {
		lprintf(LOG_NOTICE, "usage: event <num>");
		lprintf(LOG_NOTICE, "   1 : Temperature - Upper Critical - Going High");
		lprintf(LOG_NOTICE, "   2 : Voltage Threshold - Lower Critical - Going Low");
		lprintf(LOG_NOTICE, "   3 : Memory - Correctable ECC");
		lprintf(LOG_NOTICE, "usage: event file <filename>");
		lprintf(LOG_NOTICE, "   Will read list of events from file");
		return 0;
	}

	if (strncmp(argv[0], "file", 4) == 0) {
		if (argc < 2) {
			lprintf(LOG_NOTICE, "usage: event file <filename>\n");
			rc = -1;
		} else {
			rc = ipmi_event_fromfile(intf, argv[1]);
		}
	} else {
		c = (uint8_t)strtol(argv[0], NULL, 0);
		rc = ipmi_send_platform_event(intf, c);
	}

	return rc;
}
