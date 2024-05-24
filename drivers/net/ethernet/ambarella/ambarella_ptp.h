/*
 * /drivers/net/ethernet/ambarella/ambarella_eth.h
 *
 * Copyright (C) 2004-2099, Ambarella, Inc.
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

#ifndef __AMBARELLA_ETH_PTP_H__
#define __AMBARELLA_ETH_PTP_H__

/* ==========================================================================*/

/* IEEE 1588 PTP registers */
#define MAC_PTP_CTRL_OFFSET			0x0700
#define MAC_PTP_SSINC_OFFSET			0x0704
#define MAC_PTP_STSEC_OFFSET			0x0708
#define MAC_PTP_STNSEC_OFFSET			0x070C
#define MAC_PTP_STSEC_UPDATE_OFFSET		0x0710
#define MAC_PTP_STNSEC_UPDATE_OFFSET		0x0714
#define MAC_PTP_ADDEND_OFFSET			0x0718
#define MAC_PPS_TARGET_TIME_SEC_OFFSET		0x071C
#define MAC_PPS_TARGET_TIME_NSEC_OFFSET		0x0720
#define MAC_PPS_CONTROL_OFFSET			0x072C
#define MAC_PPS_INTERVAL_OFFSET			0x0760
#define MAC_PPS_WIDTH_OFFSET			0x0764


/* MAC_PPS_CONTROL_OFFSET bitmap */
#define PPSEN0					BIT(4)	/* enable flexiable output */
#define TRGTBUSY0				BIT(31)
#define PPSCMD(val)				(val)
#define TRGTMODSEL(val)				((val) << 5)
#define TRGTMODSEL_MASK				(3 << 5)

/* MAC_PTP_CTRL_OFFSET bitmap */
#define PTP_CTRL_TSENA				BIT(0)
#define PTP_CTRL_TSCFUPDT			BIT(1)
#define PTP_CTRL_TSINIT				BIT(2)
#define PTP_CTRL_TSUPDT				BIT(3)
#define PTP_CTRL_TSTRIG				BIT(4)
#define PTP_CTRL_TSADDREG			BIT(5)
#define PTP_CTRL_TSENALL			BIT(8)
#define PTP_CTRL_TSCTRLSSR			BIT(9)
#define PTP_CTRL_TSVER2ENA			BIT(10)
#define PTP_CTRL_TSIPENA			BIT(11)
#define PTP_CTRL_TSIPV6ENA			BIT(12)
#define PTP_CTRL_TSIPV4ENA			BIT(13)
#define PTP_CTRL_TSEVNTENA			BIT(14)
#define PTP_CTRL_TSMSTRENA			BIT(15)
#define PTP_CTRL_SNAPTYPSEL			BIT(16)
#define PTP_CTRL_TSENMACADDR			BIT(18)

#define	PTP_SSIR_SSINC_MASK			0xff

/* ==========================================================================*/

struct ambeth_mac_pps_cfg {
	bool available;
	struct timespec64 start;
	struct timespec64 period;
};

int ambeth_set_hwtstamp(struct net_device *dev, struct ifreq *ifr);
int ambeth_get_ts_info(struct net_device *dev, struct ethtool_ts_info *info);
int ambeth_ptp_init(struct platform_device *pdev);
void ambeth_ptp_exit(struct ambeth_info *priv);
void ambeth_get_rx_hwtstamp(struct ambeth_info *lp, struct sk_buff *skb,
		struct ambeth_desc *desc);
void ambeth_get_tx_hwtstamp(struct ambeth_info *lp, struct sk_buff *skb,
		struct ambeth_desc *desc);
void ambeth_tx_hwtstamp_enable(struct ambeth_info *lp, u32 *flags);

#endif

