/*
 * arch/arm/plat-ambarella/include/plat/can.h
 *
 * Author: Ken He  <jianhe@ambarella.com>
 *
 * Copyright (c) 2018 Ambarella, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#ifndef __AMBARELLA_CAN_H__
#define __AMBARELLA_CAN_H__

#define	CAN_CTRL_OFFSET			0x00
#define	CAN_CFG_TTCAN_OFFSET		0x04
#define	CAN_RST_OFFSET			0x08
#define	CAN_WAKEUP_OFFSET		0x0C
#define	CAN_TQ_OFFSET			0x10
#define	CAN_TQ_FD_OFFSET		0x14
#define	CAN_TT_TIMER_OFFSET		0x18
#define	CAN_TT_TIMER_EN_OFFSET		0x1C
#define	CAN_ERR_STATUS_OFFSET		0x20
#define	CAN_RBUF_OVFLOW_OFFSET		0x24
#define	CAN_RX_CNT_OFFSET		0x28
#define	CAN_GLOBAL_OP_ITR_OFFSET	0x2C
#define	CAN_GLOBAL_OP_RITR_OFFSET	0x30
#define	CAN_GLOBAL_OP_ITR_MSK_OFFSET	0x34
#define	CAN_CANC_EN_OFFSET		0x38
#define	CAN_MBUF_TX_BUSY_OFFSET		0x3C
#define	CAN_MBUF_RX_BUSY_OFFSET		0x40

#define	CAN_MBUF_CFG_DONE_OFFSET	0x80
#define	CAN_MSG_REQ_REG1_OFFSET		0x84
#define	CAN_MSG_REQ_REG2_OFFSET		0x88
#define	CAN_TX_MBUF_DONE_OFFSET		0x8C
#define	CAN_RX_MBUF_DONE_OFFSET		0x90
#define	CAN_MBUF_RP_FAIL_OFFSET		0x94
#define	CAN_MBUF_TIME_OUT_OFFSET	0x98
#define	CAN_MBUF_TX_OFFSET		0x9C

/* ID filter 0~31 */
#define	CAN_ID_FLTR_REG0_OFFSET		0x100
#define	CAN_ID_FLTR_MASK_REG0_OFFSET	0x104

/* mbuf 0~31 */
#define CAN_MBUF0_ID_OFFSET		0x200
#define CAN_MBUF0_CTL_OFFSET		0x204
#define CAN_MBUF0_TIME_TRIG_OFFSET	0x208
#define CAN_MBUF0_TIME_OUT_TH_OFFSET	0x20C

/* msg priority 0~31 */
#define CAN_MSG_PRIORITY0_OFFSET	0x400
#define CAN_MSG_PRIORITY1_OFFSET	0x404
#define CAN_MSG_PRIORITY2_OFFSET	0x408
#define CAN_MSG_PRIORITY3_OFFSET	0x40C
#define CAN_MSG_PRIORITY4_OFFSET	0x410
#define CAN_MSG_PRIORITY5_OFFSET	0x414
#define CAN_MSG_PRIORITY6_OFFSET	0x418
#define CAN_MSG_PRIORITY7_OFFSET	0x41C
#define CAN_MSG_PRIORITY_OFFSET(x)	((0x400 + (x) << 2))

#define CAN_TX_DONE_TH_OFFSET		0x420
#define CAN_TX_DONE_TIMER_OFFSET	0x424
#define CAN_TX_DONE_TIMER_TH_OFFSET	0x428
#define CAN_RX_DONE_TH_OFFSET		0x42C
#define CAN_RX_DONE_TIMER_OFFSET	0x430
#define CAN_RX_DONE_TIMER_TH_OFFSET	0x434

#define CAN_DEBUG_SIGNALS_OFFSET	0x438
#define CAN_DEBUG_SIGNAL_PHY_STS_OFFSET	0x43C

#define CAN_DMA_CTRL_OFFSET		0x500
#define CAN_RX_DMA_DSC_PTR_OFFSET	0x504
#define CAN_RX_DMA_NUM_OFFSET		0x508
#define CAN_RX_DSC_TIMER_OFFSET		0x50C
#define CAN_RX_DSC_TIMER_TH_OFFSET	0x510
#define CAN_RX_SPECIAL_ID_OFFSET	0x514

/* mbuf 0~31 data 0~15 */
#define CAN_MBUF0_DATA0_OFFSET		0x800
#define CAN_MBUF0_DATA15_OFFSET		0x83C
#define CAN_MBUF_DATA0_OFFSET(i)	((CAN_MBUF0_DATA0_OFFSET + (i << 6)))

/* reg interrupt 0x2c 0x30 0x34 */
#define CAN_INT_ALL			0x003FFFFF
#define CAN_INT_ENTER_BUS_OFF		0x00000001
#define CAN_INT_ENTER_ERR_PSV		0x00000002
#define CAN_INT_ACK_ERR			0x00000004
#define CAN_INT_FORM_ERR		0x00000008
#define CAN_INT_CRC_ERR			0x00000010
#define CAN_INT_STUFF_ERR		0x00000020
#define CAN_INT_BIT_ERR			0x00000040
#define CAN_INT_TIMEOUT			0x00000080
#define CAN_INT_RBUF_OVERFLOW		0x00000100
#define CAN_INT_RX_DONE			0x00000200
#define CAN_INT_TX_DONE			0x00000400
#define CAN_INT_TIME_WRAP		0x00000800
#define CAN_INT_WAKE_UP			0x00001000
#define CAN_INT_RP_FAIL			0x00002000
#define CAN_INT_RX_DONE_TIMEOUT		0x00004000
#define CAN_INT_TX_DONE_TIMEOUT		0x00008000
#define CAN_INT_RX_DSC_DONE		0x00010000
#define CAN_INT_RX_DMA_DONE		0x00020000
#define CAN_INT_RX_DMA_TIMEOUT		0x00040000
#define CAN_INT_RX_DMA_ID		0x00080000
#define CAN_INT_RX_DMA_RTR		0x00100000
#define CAN_INT_NEW_FD_ERR		0x00200000

/* REG CAN_CTRL */
#define	CAN_NORMAL_MODE			0x00000000
#define	CAN_LOOPBACK_MODE_IN		0x00000001
#define	CAN_LOOPBACK_MODE_OUT		0x00000002
#define	CAN_LISTEN_MODE			0x00000004
#define	CAN_AUTO_RESP_MODE		0x00000008
#define	CAN_OLD_FD_FORMAT		0x00000010
#define	CAN_FD				0x80000000

/* REG RST */
#define CAN_RST_CANC			0x00000002
#define CAN_RST_CANC_DMA		0x00000004

#define CAN_ECR_TEC_MASK		0x000001FF /* Transmit error counter */
#define CAN_ECR_ERR_STA_MASK		0x00000003 /* Transmit error counter */

#define CAN_ERR_STA_SHIFT		25	/* Error State */
#define CAN_ERR_REC_SHIFT		16	/* Rx Error Count */
#define CAN_BASEID_SHIFT		18	/* Base format ID*/

#define CAN_IDR_RTR_MASK		0x00000080
#define CAN_MSG_DLC_MASK		0x0000000F
#define CAN_MSG_EFF_ID_MASK		0x20000000
#define CAN_MSG_EDL_MASK        BIT(6)  /* FDF Flag */
#define CAN_MSG_BRS_MASK        BIT(5)  /* BRS Flag */
#define CAN_MSG_ESI_MASK        BIT(4)	/* ESI Flag */


#define CAN_TX_BUF_NUM			0	/* We use the buf0 for tx for the moment */

#endif /* __AMBARELLA_CAN_H__ */

