/*
 * /drivers/net/ethernet/ambarella/ambarella_diag.c
 *
 * Copyright (C) 2004-2021, Ambarella, Inc.
 *	Jorney <qtu@ambarella.com>
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/bits.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/crc32.h>
#include <linux/time.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/ethtool.h>
#include <linux/iopoll.h>
#include "ambarella_eth.h"
#include "ambarella_ptp.h"

static int selftest_loop = 1;
module_param (selftest_loop, int, 0);
MODULE_PARM_DESC (selftest_loop, "Specific the loop count for selftest");

static int selftest_speed = 0;
module_param (selftest_speed, int, 0);
MODULE_PARM_DESC (selftest_speed, "Specific the speed for selftest");

static const char ambeth_gstrings_test[][ETH_GSTRING_LEN] = {
	"Mac loopback",
};

static const char ambeth_gstrings_stat[][ETH_GSTRING_LEN] = {
	"Tx Complete", "Rx Complete", "Tx buffer unavailable", "Tx stopped",
	"Tx jabber timeout", "Rx overflow", "Tx underflow", "Rx buffer unavailable",
	"Rx stopped", "Rx watchdog timeout", "Tx early", "Fatal error"
};


static void ambeth_selftest_callback(struct sk_buff *skb, struct net_device *netdev)
{
	struct ambeth_info *priv;
	char *data = skb->data - 14;

	priv = (struct ambeth_info *)netdev_priv(netdev);
	priv->selftest_rx_len =
		skb->len > ETH_DATA_LEN ? ETH_DATA_LEN : skb->len;

	if (priv->selftest_rx_data)
		memcpy(priv->selftest_rx_data, data, priv->selftest_rx_len);

	complete(&priv->comp);
}

void ambeth_self_test(struct net_device *netdev,
		struct ethtool_test *etest, u64 *buf)
{
	char magic;
	int i, rval = 0, carrier, loop;
	struct sk_buff *skb;
	struct ambeth_info *priv;
	unsigned int cfgreg_save, tmp;

	priv = (struct ambeth_info *)netdev_priv(netdev);

	carrier = netif_carrier_ok(netdev);
	if (carrier)
		netif_carrier_off(netdev);

	/* wait for Q drain */
	msleep(500);

	cfgreg_save = readl(priv->regbase + ETH_MAC_CFG_OFFSET);
	tmp = cfgreg_save;

	/* Speed for selftest if carrier is off depends on PHY's speed */
	if (!carrier) {
		dev_info(&netdev->dev, "No carrier, setup speed: %d\n", selftest_speed);
		switch (selftest_speed) {
		case 10:
			tmp |= ETH_MAC_CFG_PS;
			tmp &= ~ETH_MAC_CFG_FES;
			break;
		case 100:
			tmp |= ETH_MAC_CFG_PS;
			tmp |= ETH_MAC_CFG_FES;
			break;
		case 1000:
			tmp &= ~ETH_MAC_CFG_PS;
			tmp &= ~ETH_MAC_CFG_FES;
			break;
		default:
			pr_err("Speed %d for selftest is invalid.\n", selftest_speed);
			rval = -EINVAL;
			goto __err_self_test;
		}

		tmp |= ETH_MAC_CFG_DM;
	}

	/* set MAC loopback */
	tmp |= ETH_MAC_CFG_LM;
	writel(tmp, priv->regbase + ETH_MAC_CFG_OFFSET);

	/* Enable receive all */
	setbitsl(ETH_MAC_FRAME_FILTER_RA, priv->regbase + ETH_MAC_FRAME_FILTER_OFFSET);

	/* sync */
	msleep(100);

	priv->selftest_rx_data = kmalloc(ETH_DATA_LEN, GFP_KERNEL);
	if (!priv->selftest_rx_data) {
		rval = -ENOMEM;
		goto __err_self_test;
	}

	for (loop = 0; loop < selftest_loop; loop++) {

		magic = (loop + 0x5A) & 0xff;
		skb = netdev_alloc_skb(netdev, ETH_DATA_LEN);
		if (!skb) {
			rval = -ENOMEM;
			break;
		}
		skb_put(skb, ETH_DATA_LEN);
		memset(skb->data, magic, ETH_DATA_LEN);

		priv->loopback = 1;
		priv->selftest_callback = ambeth_selftest_callback;
		netdev->netdev_ops->ndo_start_xmit(skb, netdev);
		rval = wait_for_completion_timeout(&priv->comp, 1000);
		if (!rval) {
			rval = -ETIMEDOUT;
			break;
		} else {
			char *data = priv->selftest_rx_data;

			rval = 0;

			for (i = 0 ; i < priv->selftest_rx_len; i++) {
				if (data[i] == magic)
					continue;

				pr_err("Data corrputed: [%d][%d] %x (expected) VS %x (actual)\n",
						loop, i, magic, data[i]);
				rval = -EIO;
				break;
			}
		}

		if (rval)
			break;

		//pr_info("%d: Selftest Pass.\n", loop);
	};

	kfree(priv->selftest_rx_data);
	priv->loopback = 0;
	priv->selftest_callback = NULL;

__err_self_test:
	buf[0] = rval;
	etest->flags |= rval ? ETH_TEST_FL_FAILED : 0;
	writel(cfgreg_save, priv->regbase + ETH_MAC_CFG_OFFSET);
	clrbitsl(ETH_MAC_FRAME_FILTER_RA, priv->regbase + ETH_MAC_FRAME_FILTER_OFFSET);

	if (carrier)
		netif_carrier_on(netdev);
}

int ambeth_get_sset_count(struct net_device *netdev, int sset)
{

	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(ambeth_gstrings_stat);
	case ETH_SS_TEST:
		return ARRAY_SIZE(ambeth_gstrings_test);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}
void ambeth_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	switch (stringset) {
	case ETH_SS_TEST:
		memcpy(data, *ambeth_gstrings_test, sizeof(ambeth_gstrings_test));
		break;
	case ETH_SS_STATS:
		memcpy(data, *ambeth_gstrings_stat, sizeof(ambeth_gstrings_stat));
		break;
	}
}
void ambeth_get_ethtool_stats(struct net_device *netdev,
	struct ethtool_stats *stats, u64 *data)
{
	struct ambeth_info *priv = (struct ambeth_info *)netdev_priv(netdev);

	data[0] = priv->nis_tx + priv->nis_tx_buf_unavail;
	data[1] = priv->nis_rx;

	data[2] = priv->nis_tx_buf_unavail;
	data[3] = priv->ais_tx_stopped;
	data[4] = priv->ais_tx_jabber_timeout;
	data[5] = priv->ais_rx_overflow;
	data[6] = priv->ais_tx_underflow;
	data[7] = priv->ais_rx_buf_unavail;
	data[8] = priv->ais_rx_stopped;
	data[9] = priv->ais_rx_wdt_timeout;
	data[10] = priv->ais_early_tx;
	data[11] = priv->ais_fatal_error;
}

void ambeth_interrupt_statis(struct ambeth_info *priv, u32 irq_status)
{
	if (irq_status & BIT(0))
		priv->nis_tx ++;

	if (irq_status & BIT(6) || irq_status & BIT(14))
		priv->nis_rx ++;

	if (irq_status & BIT(2))
		priv->nis_tx_buf_unavail ++;

	if (!(irq_status & BIT(15)))
		return ;

	if (irq_status & BIT(1))
		priv->ais_tx_stopped ++;

	if (irq_status & BIT(3))
		priv->ais_tx_jabber_timeout ++;

	if (irq_status & BIT(4))
		priv->ais_rx_overflow ++;

	if (irq_status & BIT(5))
		priv->ais_tx_underflow ++;

	if (irq_status & BIT(7))
		priv->ais_rx_buf_unavail ++;

	if (irq_status & BIT(8))
		priv->ais_rx_stopped ++;

	if (irq_status & BIT(9))
		priv->ais_rx_wdt_timeout ++;

	if (irq_status & BIT(10))
		priv->ais_early_tx ++;

	if (irq_status & BIT(13))
		priv->ais_fatal_error ++;
}
