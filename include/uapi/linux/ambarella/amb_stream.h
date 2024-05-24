/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __AMB_STREAM_H__
#define __AMB_STREAM_H__

/**
 *
 * (C) Copyright Ambarella, Inc. 2022
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/types.h>

/* TOKEN */
#define AMB_COMMAND_TOKEN 0x55434D44
#define AMB_STATUS_TOKEN 0x55525350

#define SIMPLE_CMD_SIZE 32

#define USB_CMD_TRY_TO_CONN 0
#define USB_CMD_RDY_TO_RCV 1
#define USB_CMD_RCV_DATA 2
#define USB_CMD_RDY_TO_SND 3
#define USB_CMD_SND_DATA 4
#define USB_CMD_SET_MODE 5
#define USB_CMD_RECV_REQUEST 6

#define AMB_RSP_SUCCESS 0
#define AMB_RSP_FAILED 1

#define AMB_CMD_PARA_UP 0
#define AMB_CMD_PARA_DOWN 1
#define AMB_CMD_PARA_MEASURE_SPD 2

#define AMB_RSP_NO_CONN 0
#define AMB_RSP_CONNECT 1

#define NR_PORT 32
#define ALL_PORT 0xffff

#define PORT_STATUS_CHANGE 0x55
#define PORT_NOTIFY_IDLE 0xff
#define PORT_NO_CONNECT 0
#define PORT_CONNECT 1
#define PORT_FREE_ALL 2

#define REQUEST_HOST_CONNECT 0xaa
#define HOST_NO_CONNECT 0
#define HOST_CONNECT 1

#define FLAG_LAST_TRANS 0x01
#define FLAG_FORCE_FINISH 0x10

#define AMB_DATA_STREAM_MAGIC 'u'
#define AMB_DATA_STREAM_WR_RSP _IOW(AMB_DATA_STREAM_MAGIC, 1, struct amb_rsp *)
#define AMB_DATA_STREAM_RD_CMD _IOR(AMB_DATA_STREAM_MAGIC, 1, struct amb_cmd *)
#define AMB_DATA_STREAM_STATUS_CHANGE                                          \
	_IOW(AMB_DATA_STREAM_MAGIC, 2, struct amb_notify *)

struct amb_ack {
	__u32 signature;
	__u32 acknowledge;
	__u32 parameter0;
	__u32 parameter1;
};

struct amb_cmd {
	__u32 signature;
	__u32 command;
	__u32 parameter[(SIMPLE_CMD_SIZE / sizeof(__u32)) - 2];
};

struct amb_rsp {
	__u32 signature;
	__u32 response;
	__u32 parameter0;
	__u32 parameter1;
};

struct amb_notify {
	__u16 bNotifyType;
	__u16 port_id;
	__u16 value;
	__u16 status;
};

struct amb_usb_head {
	__u32 port_id;
	__u32 size;
	__u32 flag1;
	__u32 flag2;
};

#define USB_PORT_IDLE 0x0
#define USB_PORT_OPEN 0x1
#define USB_PORT_CLOSED 0x2

#define USB_HEAD_SIZE sizeof(struct amb_usb_head)

#endif
