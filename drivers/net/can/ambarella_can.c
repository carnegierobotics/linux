/*
 * ambarella_can.c - CAN network driver for Ambarella SoC CAN controller
 *
 * History:
 *	2018/07/04 - [Ken He] created file
 *
 * Copyright (C) 2004-2009, Ambarella, Inc.
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

#include <linux/clk.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/can/dev.h>
#include <linux/can/error.h>
#include <linux/can/led.h>
#include <linux/pm_runtime.h>
#include "ambarella_can.h"

#define DRIVER_NAME	"ambarella_can"

/* We use the mbuf[0] for TX BUF */
#define AMBA_CAN_TX_BUF_SIZE	(1)
#define AMBA_CAN_RX_DMA_SIZE	(8)
#define AMBA_CAN_MSG_BUF_NUM	(32)
#define	AMBA_CAN_MSG_DSIZE	(64)


#define	AMBA_CAN_IRQ_DEFAULT	(CAN_INT_ENTER_BUS_OFF | CAN_INT_RX_DONE \
				| CAN_INT_TX_DONE | CAN_INT_RP_FAIL)

#define	AMBA_CAN_IRQ_DMA	(CAN_INT_ENTER_BUS_OFF | CAN_INT_RX_DONE \
				| CAN_INT_TX_DONE | CAN_INT_RP_FAIL \
				| CAN_INT_RX_DSC_DONE | CAN_INT_RX_DMA_DONE)

#define AMBA_CAN_IRQ_BUS_ERR	(CAN_INT_ACK_ERR | CAN_INT_FORM_ERR \
				| CAN_INT_CRC_ERR | CAN_INT_STUFF_ERR | CAN_INT_BIT_ERR)

static u32 mbuf_cfg_done;
/**
 * struct ambarella_can_priv - This definition define CAN driver instance
 * @can:			CAN private data structure.
 * @tx_head:			Tx CAN packets ready to send on the queue
 * @tx_tail:			Tx CAN packets successfully sended on the queue
 * @tx_max:			Maximum number packets the driver can send
 * @napi:			NAPI structure
 * @dev:			Network device data structure
 * @reg_base:			Ioremapped address to registers
 * @irq_flags:			For request_irq()
 * @bus_clk:			Pointer to struct clk
 * @can_clk:			Pointer to struct clk
 */
struct ambacan_priv {
	struct can_priv can;
	unsigned int tx_head;
	unsigned int tx_tail;
	unsigned int tx_max;
	struct napi_struct napi;
	struct device *dev;
	void __iomem *reg_base;
	unsigned long irq_flags;
	struct clk *can_clk;
	spinlock_t lock;
};

/* CANFD Nomimal Bittiming constants */
static const struct can_bittiming_const ambarella_bittiming_const_canfd = {
	.name = DRIVER_NAME,
	.tseg1_min = 1,
	.tseg1_max = 64,
	.tseg2_min = 1,
	.tseg2_max = 16,
	.sjw_max = 16,
	.brp_min = 1,
	.brp_max = 256,
	.brp_inc = 1,
};
/* CANFD Data Bittiming constants */
static const struct can_bittiming_const ambarella_data_bittiming_const_canfd = {
	.name = DRIVER_NAME,
	.tseg1_min = 1,
	.tseg1_max = 32,
	.tseg2_min = 1,
	.tseg2_max = 16,
	.sjw_max = 16,
	.brp_min = 1,
	.brp_max = 256,
	.brp_inc = 1,
};

/**
 * set_reset_mode - Resets the CAN device mode
 * @ndev:	Pointer to net_device structure
 *
 * This is the driver reset mode routine.The driver
 * enters into configuration mode.
 *
 * Return: 0 on success and failure value on error
 */
static int set_reset_mode(struct net_device *ndev)
{
	struct ambacan_priv *priv = netdev_priv(ndev);

	writel_relaxed((CAN_RST_CANC | CAN_RST_CANC_DMA), priv->reg_base + CAN_RST_OFFSET);
	writel_relaxed(0, priv->reg_base + CAN_RST_OFFSET);

	return 0;
}

static int ambarella_can_set_bittiming(struct net_device *ndev)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	struct can_bittiming *bt = &priv->can.bittiming;
	struct can_bittiming *dbt = &priv->can.data_bittiming;
	u32 cfg_tq, cfg_fd_tq;

	netdev_dbg(ndev, "set bit timing brp=[0x%x] sjw=[0x%x] prop_seg=[0x%x] seg1=[0x%x] seg2=[0x%x] \n",
				bt->brp, bt->sjw, bt->prop_seg, bt->phase_seg1, bt->phase_seg2);
	cfg_tq = (((bt->brp - 1) & 0xFF) << 14 )|
			(((bt->sjw - 1) & 0xF) << 10) |
			(((bt->prop_seg + bt->phase_seg1 - 1) & 0x3F) << 4) |
			((bt->phase_seg2 - 1) & 0xF);

	netdev_dbg(ndev, "setting BITTIMING=0x%08x\n", cfg_tq);

	if (priv->can.ctrlmode & CAN_CTRLMODE_FD) {
		netdev_dbg(ndev, "set data bit timing brp=[0x%x] sjw=[0x%x] prop_seg=[0x%x] seg1=[0x%x] seg2=[0x%x] \n",
				dbt->brp, dbt->sjw, dbt->prop_seg, dbt->phase_seg1, dbt->phase_seg2);
		cfg_fd_tq = (((dbt->brp - 1) & 0xFF) << 13 )|
			(((dbt->sjw - 1) & 0xF) << 9) |
			(((dbt->prop_seg + dbt->phase_seg1 - 1) & 0x1F) << 4) |
			((dbt->phase_seg2 - 1) & 0xF);

		netdev_dbg(ndev, "setting data BITTIMING=0x%08x\n", cfg_fd_tq);
		writel_relaxed(cfg_fd_tq, priv->reg_base + CAN_TQ_FD_OFFSET);
	} else {
		if (priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES)
			cfg_tq |= (1 << 31);
	}

	writel_relaxed(cfg_tq, priv->reg_base + CAN_TQ_OFFSET);

	return 0;
}

static int ambarella_can_start(struct net_device *ndev)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	int err;
	u32 val_ctrl;
	u32 mbuf_tx;

	/* Check if it is in reset mode */
	err = set_reset_mode(ndev);
	if (err < 0)
		return err;

	/* disable interrupts */
	writel_relaxed(CAN_INT_ALL, priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);

	/* disable chip */
	writel_relaxed(0, priv->reg_base + CAN_CANC_EN_OFFSET);

	err = ambarella_can_set_bittiming(ndev);
	if (err < 0)
		return err;

	/* mbuf0 as tx */
	mbuf_cfg_done = 0xFFFFFFFE;
	mbuf_tx = (1 << CAN_TX_BUF_NUM);

	/* Enable interrupts */
	writel_relaxed((AMBA_CAN_IRQ_DEFAULT | CAN_INT_RX_DONE_TIMEOUT |
			CAN_INT_ENTER_ERR_PSV | AMBA_CAN_IRQ_BUS_ERR),
				priv->reg_base + CAN_GLOBAL_OP_ITR_MSK_OFFSET);

	netdev_dbg(ndev, "priv ctrlmode is 0x%x \n", priv->can.ctrlmode);
	/* enable chip */
	val_ctrl = readl_relaxed(priv->reg_base + CAN_CTRL_OFFSET);
	val_ctrl &= 0xFFFFFFF0;
	/* Check whether it is loopback mode or normal mode  */
	if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY) {
		val_ctrl |= CAN_LISTEN_MODE;
		mbuf_cfg_done = 0xFFFFFFFF;
		mbuf_tx = 0;
	}

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK)
		val_ctrl |= CAN_LOOPBACK_MODE_OUT;

	if (priv->can.ctrlmode & CAN_CTRLMODE_FD) {
		val_ctrl |= CAN_FD;
		val_ctrl &= ~CAN_OLD_FD_FORMAT;
		if (priv->can.ctrlmode & CAN_CTRLMODE_FD_NON_ISO)
			val_ctrl |= CAN_OLD_FD_FORMAT;
	} else {
		val_ctrl &= ~CAN_FD;
	}
	writel_relaxed(val_ctrl, priv->reg_base + CAN_CTRL_OFFSET);
	writel_relaxed(mbuf_tx, priv->reg_base + CAN_MBUF_TX_OFFSET);
	writel_relaxed(mbuf_cfg_done, priv->reg_base + CAN_MBUF_CFG_DONE_OFFSET);

	//writel_relaxed(0, priv->reg_base + CAN_TX_DONE_TH_OFFSET);
	//writel_relaxed(0, priv->reg_base + CAN_TX_DONE_TIMER_OFFSET);
	writel_relaxed(1, priv->reg_base + CAN_RX_DONE_TH_OFFSET);
	writel_relaxed(0, priv->reg_base + CAN_RX_DONE_TIMER_OFFSET);

	writel_relaxed(1, priv->reg_base + CAN_CANC_EN_OFFSET);

	priv->can.state = CAN_STATE_ERROR_ACTIVE;
	return 0;
}

static int ambarella_can_do_set_mode(struct net_device *ndev, enum can_mode mode)
{
	int ret;

	switch (mode) {
	case CAN_MODE_START:
		ret = ambarella_can_start(ndev);
		if (ret < 0) {
			netdev_err(ndev, "starting CAN failed!\n");
			return ret;
		}
		netif_wake_queue(ndev);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static int ambarella_can_request_msgbuf(struct net_device *ndev, u32 num)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	u32 request;
	u32 requestmask = 1 << num;
	int i;

	request = readl_relaxed(priv->reg_base + CAN_MSG_REQ_REG1_OFFSET);
	request |= requestmask;
	writel_relaxed(request, priv->reg_base + CAN_MSG_REQ_REG1_OFFSET);
	for (i= 0; i < 3; i++){
		request = readl_relaxed(priv->reg_base + CAN_MSG_REQ_REG2_OFFSET);
		if (request & requestmask) {
			return 1;
		}
	}

	return 0;

}
static netdev_tx_t ambarella_can_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	//struct net_device_stats *stats = &ndev->stats;
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	u32 id, cfg_done, buf_ctrl = 0;
	u32 i, lwords;

	if (can_dropped_invalid_skb(ndev, skb))
		return NETDEV_TX_OK;

	/* Watch carefully on the bit sequence */
	if (cf->can_id & CAN_EFF_FLAG) {
		/* Extended CAN ID format */
		id = cf->can_id & CAN_EFF_MASK;
		id |= CAN_MSG_EFF_ID_MASK;
	} else {
		/* Standard CAN ID format */
		id = (cf->can_id & CAN_SFF_MASK) << CAN_BASEID_SHIFT;
	}

	if (can_is_canfd_skb(skb)) {
		/* CAN FD frame format */
		buf_ctrl = can_fd_len2dlc(cf->len);		/* DLC */
		buf_ctrl |= CAN_MSG_EDL_MASK;
		if (cf->flags & CANFD_BRS)
			buf_ctrl |= CAN_MSG_BRS_MASK;

		if (cf->flags & CANFD_ESI)
			buf_ctrl |= CAN_MSG_ESI_MASK;
	} else {
		buf_ctrl = cf->len;		/* DLC */

		/* frames remote TX request */
		if (cf->can_id & CAN_RTR_FLAG)
			buf_ctrl |= CAN_IDR_RTR_MASK;
	}

	/* Write the Frame to CAN TX FIFO MBUF0 */
	writel_relaxed(id, priv->reg_base + CAN_MBUF0_ID_OFFSET);
	/* If the CAN frame is RTR frame this write triggers tranmission */
	writel_relaxed(buf_ctrl, priv->reg_base + CAN_MBUF0_CTL_OFFSET);

	if (!(cf->can_id & CAN_RTR_FLAG)) {
		if (ambarella_can_request_msgbuf(ndev, 0) > 0) {
			lwords = DIV_ROUND_UP(cf->len, sizeof(u32));
			for (i = 0; i < lwords; i++)
				writel(*((u32 *)cf->data + i),
					priv->reg_base + (CAN_MBUF0_DATA0_OFFSET + i * sizeof(u32)));

			/* If the CAN frame is Standard/Extended frame this
			 * write triggers tranmission
			 */

			//stats->tx_bytes += cf->len;
		} else
			return NETDEV_TX_BUSY;
	}

	can_put_echo_skb(skb, ndev, priv->tx_head % priv->tx_max, 0);
	priv->tx_head++;

	cfg_done = readl_relaxed(priv->reg_base + CAN_MBUF_CFG_DONE_OFFSET);
	cfg_done |= (1 << CAN_TX_BUF_NUM);
	writel_relaxed(cfg_done, priv->reg_base + CAN_MBUF_CFG_DONE_OFFSET);

	return NETDEV_TX_OK;
}

static void ambarella_can_state(struct net_device *ndev, u32 itr)
{
	struct ambacan_priv *priv = netdev_priv(ndev);

	/* Check for Wake up interrupt if set put CAN device in Active state */
	if (itr & CAN_INT_WAKE_UP)
		priv->can.state = CAN_STATE_ERROR_ACTIVE;
}

static int ambarella_can_err(struct net_device *ndev, u32 isr)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct can_frame *cf;
	struct sk_buff *skb;
	u32 rxerr, txerr, err;
	u32 err_state;

	/* create zero'ed CAN frame buffer */
	skb = alloc_can_err_skb(ndev, &cf);
	if (unlikely(!skb))
		return -ENOMEM;

	err = readl_relaxed(priv->reg_base + CAN_ERR_STATUS_OFFSET);
	err_state = (err >> CAN_ERR_STA_SHIFT) & CAN_ECR_ERR_STA_MASK;
	rxerr = (err >> CAN_ERR_REC_SHIFT) & CAN_ECR_TEC_MASK;
	txerr = err & CAN_ECR_TEC_MASK;

	/* Handle the err interrupt */
	/* Check for bus-off */
	if (isr & CAN_INT_ENTER_BUS_OFF) {
		priv->can.state = CAN_STATE_BUS_OFF;
		priv->can.can_stats.bus_off++;
		writel_relaxed(CAN_INT_ENTER_BUS_OFF,
				priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);
		can_bus_off(ndev);
		cf->can_id |= CAN_ERR_BUSOFF;
		writel_relaxed(0, priv->reg_base + CAN_WAKEUP_OFFSET);
	} else {
		cf->data[6] = txerr;
		cf->data[7] = rxerr;
	}

	if (isr & CAN_INT_ENTER_ERR_PSV) {
		priv->can.state = CAN_STATE_ERROR_PASSIVE;
		priv->can.can_stats.error_passive++;
		writel_relaxed(CAN_INT_ENTER_ERR_PSV,
				priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);
		cf->can_id |= CAN_ERR_CRTL;
		cf->data[1] = (rxerr > 127) ?
					CAN_ERR_CRTL_RX_PASSIVE :
					CAN_ERR_CRTL_TX_PASSIVE;
	}

	if (isr & CAN_INT_RBUF_OVERFLOW) {
		writel_relaxed(CAN_INT_RBUF_OVERFLOW,
				priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);
		cf->can_id |= CAN_ERR_CRTL;
		cf->data[1] = CAN_ERR_CRTL_RX_OVERFLOW;
		stats->rx_over_errors++;
		stats->rx_errors++;
	}

	if (isr & AMBA_CAN_IRQ_BUS_ERR) {
		cf->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR;
		priv->can.can_stats.bus_error++;
		writel_relaxed(AMBA_CAN_IRQ_BUS_ERR,
				priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);

		if (isr & CAN_INT_ACK_ERR) {
			stats->tx_errors++;
			cf->can_id |= CAN_ERR_ACK;
			cf->data[3] = CAN_ERR_PROT_LOC_ACK;
		}

		if (isr & CAN_INT_FORM_ERR) {
			stats->rx_errors++;
			cf->can_id |= CAN_ERR_PROT;
			cf->data[2] = CAN_ERR_PROT_FORM;
		}

		if (isr & CAN_INT_CRC_ERR) {
			stats->rx_errors++;
			cf->can_id |= CAN_ERR_PROT;
			cf->data[3] = CAN_ERR_PROT_LOC_CRC_SEQ;
		}

		if (isr & CAN_INT_STUFF_ERR) {
			stats->rx_errors++;
			cf->can_id |= CAN_ERR_PROT;
			cf->data[2] = CAN_ERR_PROT_STUFF;
		}

		if (isr & CAN_INT_BIT_ERR) {
			stats->tx_errors++;
			cf->can_id |= CAN_ERR_PROT;
			cf->data[2] = CAN_ERR_PROT_BIT;
		}
	}
	/* Handle the err interrupt end */

	stats->rx_packets++;
	stats->rx_bytes += cf->len;
	netif_rx(skb);

	return 0;
}

static void ambarella_can_tx(struct net_device *ndev, u32 isr)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	u32 tx_done_sts, i = 0;
	u32 len;

	tx_done_sts = readl_relaxed(priv->reg_base + CAN_TX_MBUF_DONE_OFFSET);

	while ((priv->tx_head - priv->tx_tail > 0) && (i < AMBA_CAN_MSG_BUF_NUM)){
		if (tx_done_sts & 1) {
			len = can_get_echo_skb(ndev, priv->tx_tail %
						priv->tx_max, 0);
			stats->tx_bytes += len;
			priv->tx_tail++;
			stats->tx_packets++;
		}
		tx_done_sts >>= 1;
		i++;
	}
	can_led_event(ndev, CAN_LED_EVENT_TX);
	netif_wake_queue(ndev);
}

static void ambarella_can_rx(struct net_device *ndev, int num)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	struct net_device_stats *stats = &ndev->stats;
	struct canfd_frame *cf;
	struct sk_buff *skb;
	u32 buf_id, buf_ctrl, cfg_done;
	u32 dlc;
	u32 i, lwords;

	buf_id = readl_relaxed(priv->reg_base + CAN_MBUF0_ID_OFFSET + num * 16);
	buf_ctrl = readl_relaxed(priv->reg_base + CAN_MBUF0_CTL_OFFSET + num * 16);
	dlc = buf_ctrl & CAN_MSG_DLC_MASK;

	/* create zero'ed CAN frame buffer */
	if (priv->can.ctrlmode & CAN_CTRLMODE_FD) {
		if (buf_ctrl & CAN_MSG_EDL_MASK)
			skb = alloc_canfd_skb(ndev, &cf);
		else
			skb = alloc_can_skb(ndev, (struct can_frame **)&cf);
	} else
		skb = alloc_can_skb(ndev, (struct can_frame **)&cf);

	if (unlikely(!skb)) {
		stats->rx_dropped++;
		return;
	}

	/* Change CAN data length format to socketCAN data format */
	if (priv->can.ctrlmode & CAN_CTRLMODE_FD) {
		if (buf_ctrl & CAN_MSG_EDL_MASK) {
			cf->len = can_fd_dlc2len(dlc);
			if (buf_ctrl & CAN_MSG_BRS_MASK)
				cf->flags |= CANFD_BRS;
			if (buf_ctrl & CAN_MSG_ESI_MASK)
				cf->flags |= CANFD_ESI;
		} else
			cf->len = can_cc_dlc2len(dlc);
	} else
		cf->len = can_cc_dlc2len(dlc);

	/* Change CAN ID format to socketCAN ID format */
	if (buf_id & CAN_MSG_EFF_ID_MASK) {
		/* The received frame is an Extended format frame */
		cf->can_id = buf_id & CAN_EFF_MASK;
		cf->can_id |= CAN_EFF_FLAG;
	} else {
		/* The received frame is a standard format frame */
		cf->can_id = (buf_id & CAN_EFF_MASK) >>
				CAN_BASEID_SHIFT;
	}

	if (buf_ctrl & CAN_IDR_RTR_MASK)
			cf->can_id |= CAN_RTR_FLAG;
	else {
		lwords = DIV_ROUND_UP(cf->len, sizeof(u32));
		for (i = 0; i < lwords; i++)
		*((u32 *)cf->data + i) =
			readl(priv->reg_base + CAN_MBUF_DATA0_OFFSET(num) + (i * sizeof(u32)));
	}

	cfg_done = readl_relaxed(priv->reg_base + CAN_MBUF_CFG_DONE_OFFSET);
	cfg_done |= (1 << num);
	writel_relaxed(cfg_done, priv->reg_base + CAN_MBUF_CFG_DONE_OFFSET);

	stats->rx_packets++;
	stats->rx_bytes += cf->len;
	netif_rx(skb);

	can_led_event(ndev, CAN_LED_EVENT_RX);
}

static irqreturn_t ambarella_can_irq(int irq, void *dev_id)
{
	struct net_device *ndev = (struct net_device *)dev_id;
	struct ambacan_priv *priv = netdev_priv(ndev);
	u32 int_src, int_msk, rx_done_sts;
	int i;

	/* Get the interrupt status from CAN */
	int_src = readl_relaxed(priv->reg_base + CAN_GLOBAL_OP_ITR_OFFSET);
	int_msk = readl_relaxed(priv->reg_base + CAN_GLOBAL_OP_ITR_MSK_OFFSET);

	int_src &= int_msk;
	if (!int_src)
		return IRQ_NONE;

	/* Check for rx timeout */
	if (int_src & CAN_INT_RX_DONE_TIMEOUT) {
		writel_relaxed(0, priv->reg_base + CAN_RX_DONE_TIMER_OFFSET);
		writel_relaxed(CAN_INT_RX_DONE_TIMEOUT,
				priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);
	}

	/* Check wakeup interrupt */
	if (int_src & CAN_INT_WAKE_UP) {
		writel_relaxed(CAN_INT_WAKE_UP,
				priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);
		ambarella_can_state(ndev, int_src);
	}

	/* Check for rfail */
	if (int_src & CAN_INT_RP_FAIL) {
		netdev_dbg(ndev, "%s: error status register:0x%x\n",
			__func__, readl_relaxed(priv->reg_base +  CAN_GLOBAL_OP_ITR_OFFSET));
	}

	/* Check for Tx interrupt and Processing it */
	if (int_src & CAN_INT_TX_DONE) {
		ambarella_can_tx(ndev, int_src);
		writel_relaxed(CAN_INT_TX_DONE,
				priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);
	}

	/* Check for the type of receive interrupt and Processing it */
	if (int_src & CAN_INT_RX_DONE) {
		rx_done_sts = readl_relaxed(priv->reg_base + CAN_RX_MBUF_DONE_OFFSET);
		for(i = 0; i < AMBA_CAN_MSG_BUF_NUM; i++) {
			if (rx_done_sts & 1)
				ambarella_can_rx(ndev, i);
			rx_done_sts >>= 1;
		}
		writel_relaxed(CAN_INT_RX_DONE, priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);
		writel_relaxed(mbuf_cfg_done, priv->reg_base + CAN_MBUF_CFG_DONE_OFFSET);
	}

	if (int_src & (AMBA_CAN_IRQ_BUS_ERR | CAN_INT_RBUF_OVERFLOW |
			CAN_INT_ENTER_ERR_PSV | CAN_INT_ENTER_BUS_OFF)) {
			/* error interrupt */
			if (ambarella_can_err(ndev, int_src))
				netdev_err(ndev, "can't allocate buffer\n");
	}

	return IRQ_HANDLED;
}

static void ambarella_can_stop(struct net_device *ndev)
{
	struct ambacan_priv *priv = netdev_priv(ndev);

	/* Disable interrupts and leave the can in reset mode */
	set_reset_mode(ndev);

	/* disable all interrupts */
	writel_relaxed(CAN_GLOBAL_OP_RITR_OFFSET,
			priv->reg_base + CAN_GLOBAL_OP_RITR_OFFSET);
	priv->can.state = CAN_STATE_STOPPED;
}

static int ambarella_can_open(struct net_device *ndev)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	int ret;

	ret = pm_runtime_get_sync(priv->dev);
	if (ret < 0) {
		netdev_err(ndev, "%s: pm_runtime_get failed(%d)\n",
				__func__, ret);
		return ret;
	}

	ret = request_irq(ndev->irq, ambarella_can_irq, priv->irq_flags,
			ndev->name, ndev);
	if (ret < 0) {
		netdev_err(ndev, "irq allocation for CAN failed\n");
		goto err;
	}

	/* Set chip into reset mode */
	ret = set_reset_mode(ndev);
	if (ret < 0) {
		netdev_err(ndev, "mode resetting failed!\n");
		goto err_irq;
	}

	/* Common open */
	ret = open_candev(ndev);
	if (ret)
		goto err_irq;

	ret = ambarella_can_start(ndev);
	if (ret < 0) {
		netdev_err(ndev, "Ambarella can start failed!\n");
		goto err_candev;
	}

	can_led_event(ndev, CAN_LED_EVENT_OPEN);

	netif_start_queue(ndev);

	return 0;

err_candev:
	close_candev(ndev);
err_irq:
	free_irq(ndev->irq, ndev);
err:
	pm_runtime_put(priv->dev);

	return ret;
}

static int ambarella_can_close(struct net_device *ndev)
{
	struct ambacan_priv *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	ambarella_can_stop(ndev);
	free_irq(ndev->irq, ndev);
	close_candev(ndev);

	can_led_event(ndev, CAN_LED_EVENT_STOP);
	pm_runtime_put(priv->dev);

	return 0;
}

/**
 * ambarella_can_get_berr_counter - error counter routine
 * @ndev:	Pointer to net_device structure
 * @bec:	Pointer to can_berr_counter structure
 *
 * This is the driver error counter routine.
 * Return: 0 on success and failure value on error
 */
static int ambarella_can_get_berr_counter(const struct net_device *ndev,
					struct can_berr_counter *bec)
{
	struct ambacan_priv *priv = netdev_priv(ndev);
	int ret;
	u32 err;

	ret = pm_runtime_get_sync(priv->dev);
	if (ret < 0) {
		netdev_err(ndev, "%s: pm_runtime_get failed(%d)\n",
				__func__, ret);
		return ret;
	}

	err = readl_relaxed(priv->reg_base + CAN_ERR_STATUS_OFFSET);
	bec->rxerr = (err >> CAN_ERR_REC_SHIFT) & CAN_ECR_TEC_MASK;
	bec->txerr = err & CAN_ECR_TEC_MASK;

	pm_runtime_put(priv->dev);

	return 0;
}


static const struct net_device_ops ambarella_can_netdev_ops = {
	.ndo_open	= ambarella_can_open,
	.ndo_stop	= ambarella_can_close,
	.ndo_start_xmit	= ambarella_can_start_xmit,
	.ndo_change_mtu	= can_change_mtu,
};

static int __maybe_unused ambarella_can_suspend(struct device *dev)
{
	if (!device_may_wakeup(dev))
		return pm_runtime_force_suspend(dev);

	return 0;
}

static int __maybe_unused ambarella_can_resume(struct device *dev)
{
	if (!device_may_wakeup(dev))
		return pm_runtime_force_resume(dev);

	return 0;

}


static const struct dev_pm_ops ambarella_can_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ambarella_can_suspend, ambarella_can_resume)
};

static int ambarella_can_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct net_device *ndev;
	struct ambacan_priv *priv;
	struct clk *clk;
	void __iomem *addr;
	int irq;
	int ret, rx_max, tx_max;

	/* 1. check can clk */
	clk = devm_clk_get(&pdev->dev, "gclk_can");
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "Device clock not found.\n");
		ret = -ENODEV;
		goto err;
	}

	/* 2. Get IRQ for the device */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "could not get a valid irq \n");
		ret = -ENODEV;
		goto err;
	}

	/* 3. Get the virtual base address for the device */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err;
	}
	addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(addr)) {
		ret = PTR_ERR(addr);
		goto err;
	}
#if 0
	ret = of_property_read_u32(pdev->dev.of_node, "tx-fifo-depth", &tx_max);
	if (ret < 0)
		goto err;

	ret = of_property_read_u32(pdev->dev.of_node, "rx-fifo-depth", &rx_max);
	if (ret < 0)
		goto err;
#endif
	tx_max = 1; /* use 1 for tx_max as default */
	rx_max = 31;
	/* 4. Create a CAN device instance */
	ndev = alloc_candev(sizeof(struct ambacan_priv), tx_max);
	if (!ndev) {
		dev_err(&pdev->dev, "could not allocate memory for CAN device\n");
		ret = -ENOMEM;
		goto err;
	}

	ndev->netdev_ops = &ambarella_can_netdev_ops;
	ndev->irq = irq;
	ndev->flags |= IFF_ECHO;	/* We support local echo */

	priv = netdev_priv(ndev);
	priv->dev = &pdev->dev;
	priv->can.clock.freq = clk_get_rate(clk);
	priv->can.bittiming_const = &ambarella_bittiming_const_canfd;
	priv->can.data_bittiming_const	= &ambarella_data_bittiming_const_canfd;
	priv->can.do_set_mode = ambarella_can_do_set_mode;
	priv->can.do_get_berr_counter = ambarella_can_get_berr_counter;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_LOOPBACK |
					CAN_CTRLMODE_LISTENONLY |
					CAN_CTRLMODE_FD |
					CAN_CTRLMODE_FD_NON_ISO |
					CAN_CTRLMODE_BERR_REPORTING;
	priv->reg_base = addr;
	priv->tx_max = tx_max;
	priv->can_clk = clk;

	spin_lock_init(&priv->lock);

	platform_set_drvdata(pdev, ndev);
	SET_NETDEV_DEV(ndev, &pdev->dev);

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		netdev_err(ndev, "%s: pm_runtime_get failed(%d)\n",
			__func__, ret);
		goto err_pmdisable;
	}

	ret = register_candev(ndev);
	if (ret) {
		dev_err(&pdev->dev, "fail to register failed (err=%d)\n", ret);
		goto err_disableclks;
	}

	devm_can_led_init(ndev);

	pm_runtime_put(&pdev->dev);

	dev_info(&pdev->dev, "Ambarella canfd driver \n");

	return 0;

err_disableclks:
	pm_runtime_put(priv->dev);
err_pmdisable:
	pm_runtime_disable(&pdev->dev);

	free_candev(ndev);
err:
	return ret;
}

static int ambarella_can_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);

	unregister_candev(ndev);
	pm_runtime_disable(&pdev->dev);

	free_candev(ndev);

	return 0;
}

/* Match table for OF platform binding */
static const struct of_device_id ambarella_can_of_match[] = {
	{ .compatible = "ambarella,can", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, ambarella_can_of_match);

static struct platform_driver ambarella_can_driver = {
	.probe = ambarella_can_probe,
	.remove	= ambarella_can_remove,
	.driver	= {
		.name = DRIVER_NAME,
		.pm = &ambarella_can_dev_pm_ops,
		.of_match_table	= ambarella_can_of_match,
	},
};

module_platform_driver(ambarella_can_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ken He");
MODULE_DESCRIPTION("Ambarella CAN netdevice driver");
