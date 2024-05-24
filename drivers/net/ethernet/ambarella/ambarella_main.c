/*
 * /drivers/net/ethernet/ambarella/ambarella_main.c
 *
 * Author: Anthony Ginger <hfjiang@ambarella.com>
 * Copyright (C) 2004-2011, Ambarella, Inc.
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
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/of_mdio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mii.h>
#include <linux/if_vlan.h>
#include <linux/phy.h>
#include <linux/ethtool.h>
#include <linux/phylink.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/sys_soc.h>
#include <soc/ambarella/misc.h>
#include "ambarella_eth.h"
#include "ambarella_ptp.h"

/* ==========================================================================*/

#define le32_to_ahbmdio(id, val)	((id) ? (val) << 16 : val & 0xFFFF)
#define ahbmdio_to_le32(id, val)	((id) ? (val) >> 16 : val & 0xFFFF)

#define ENET_SEL			(1 << 0)
#define ENET_PHY_INTF_SEL_RMII		(1 << 1)
#define	POC_ETH_IS_ENABLED		0x00000001
#define	POC_ETH_RMII_MODE		0x00100000
#define SYS_CONFIG_OFFSET		0x34
/*--------------------------------------------------------------------------*/
static int msg_level = -1;
module_param (msg_level, int, 0);
MODULE_PARM_DESC (msg_level, "Override default message level");

static DEFINE_SPINLOCK(ahbmdio_lock);

static void ambhw_dump(struct ambeth_info *lp)
{
	u32 i;
	unsigned int dirty_diff;
	u32 entry;

	dirty_diff = (lp->rx.cur_rx - lp->rx.dirty_rx);
	entry = (lp->rx.cur_rx % lp->rx_count);
	dev_info(&lp->ndev->dev, "RX Info: cur_rx[%llu], dirty_rx[%llu],"
		" diff[%u], entry[%u].\n", lp->rx.cur_rx, lp->rx.dirty_rx,
		dirty_diff, entry);
	for (i = 0; i < lp->rx_count; i++) {
		dev_info(&lp->ndev->dev, "RX Info: RX descriptor[%u] "
			"0x%08x 0x%08x 0x%08x 0x%08x.\n", i,
			lp->rx.desc_rx[i].status, lp->rx.desc_rx[i].length,
			lp->rx.desc_rx[i].buffer1, lp->rx.desc_rx[i].buffer2);
	}
	dirty_diff = (lp->tx.cur_tx - lp->tx.dirty_tx);
	entry = (lp->tx.cur_tx % lp->tx_count);
	dev_info(&lp->ndev->dev, "TX Info: cur_tx[%llu], dirty_tx[%llu],"
		" diff[%u], entry[%u].\n", lp->tx.cur_tx, lp->tx.dirty_tx,
		dirty_diff, entry);
	for (i = 0; i < lp->tx_count; i++) {
		dev_info(&lp->ndev->dev, "TX Info: TX descriptor[%u] "
			"0x%08x 0x%08x 0x%08x 0x%08x.\n", i,
			lp->tx.desc_tx[i].status, lp->tx.desc_tx[i].length,
			lp->tx.desc_tx[i].buffer1, lp->tx.desc_tx[i].buffer2);
	}

	/* Dump MAC register */
	dev_info(&lp->ndev->dev, "MAC registers:\n");
	for (i = 0; i < 0x58; i += 4)
		dev_info(&lp->ndev->dev, "%04x:%08x\n", i, readl(lp->regbase +
					ETH_MAC_CFG_OFFSET + i));

	/* Dump DMA register */
	dev_info(&lp->ndev->dev, "DMA registers:\n");
	for (i = 0; i < 0x58; i += 4)
		dev_info(&lp->ndev->dev, "%04x:%08x\n", i, readl(lp->regbase +
					ETH_DMA_BUS_MODE_OFFSET + i));
}

static inline int ambhw_dma_reset(struct ambeth_info *lp)
{

	u32 value;
	int rval;

	setbitsl(ETH_DMA_BUS_MODE_SWR, lp->regbase + ETH_DMA_BUS_MODE_OFFSET);
	rval = readl_poll_timeout_atomic(lp->regbase + ETH_DMA_BUS_MODE_OFFSET,
				value, !(value & ETH_DMA_BUS_MODE_SWR),
				100, 10000);

	if (rval && netif_msg_drv(lp))
		dev_err(&lp->ndev->dev, "DMA Error: Check PHY.\n");

	return rval;
}

static inline void ambhw_dma_int_enable(struct ambeth_info *lp)
{
	writel(AMBETH_DMA_INTEN, lp->regbase + ETH_DMA_INTEN_OFFSET);
}

static inline void ambhw_dma_int_disable(struct ambeth_info *lp)
{
	writel(0, lp->regbase + ETH_DMA_INTEN_OFFSET);
}

static inline void ambhw_dma_rx_start(struct ambeth_info *lp)
{
	u32 val;

	val = readl(lp->regbase + ETH_DMA_OPMODE_OFFSET);
	val |= ETH_DMA_OPMODE_SR;
	writel(val, lp->regbase + ETH_DMA_OPMODE_OFFSET);
}

static inline void ambhw_dma_rx_stop(struct ambeth_info *lp)
{
	u32 i = 1300, irq_status, val;

	val = readl(lp->regbase + ETH_DMA_OPMODE_OFFSET);
	val &= ~ETH_DMA_OPMODE_SR;
	writel(val, lp->regbase + ETH_DMA_OPMODE_OFFSET);

	do {
		udelay(1);
		irq_status = readl(lp->regbase + ETH_DMA_STATUS_OFFSET);
	} while ((irq_status & ETH_DMA_STATUS_RS_MASK) && --i);

	if ((i <= 0) && netif_msg_drv(lp)) {
		dev_err(&lp->ndev->dev,
			"DMA Error: Stop RX status=0x%x, opmode=0x%x.\n",
			readl(lp->regbase + ETH_DMA_STATUS_OFFSET),
			readl(lp->regbase + ETH_DMA_OPMODE_OFFSET));
	}
}

static inline void ambhw_dma_tx_start(struct ambeth_info *lp)
{
	u32 val;

	val = readl(lp->regbase + ETH_DMA_OPMODE_OFFSET);
	val |= ETH_DMA_OPMODE_ST;
	writel(val, lp->regbase + ETH_DMA_OPMODE_OFFSET);
}

static inline void ambhw_dma_tx_stop(struct ambeth_info *lp)
{
	u32 i = 1300, irq_status, val;

	val = readl(lp->regbase + ETH_DMA_OPMODE_OFFSET);
	val &= ~ETH_DMA_OPMODE_ST;
	writel(val, lp->regbase + ETH_DMA_OPMODE_OFFSET);

	do {
		udelay(1);
		irq_status = readl(lp->regbase + ETH_DMA_STATUS_OFFSET);
	} while ((irq_status & ETH_DMA_STATUS_TS_MASK) && --i);
	if ((i == 0) && netif_msg_drv(lp)) {
		dev_err(&lp->ndev->dev,
			"DMA Error: Stop TX status=0x%x, opmode=0x%x.\n",
			readl(lp->regbase + ETH_DMA_STATUS_OFFSET),
			readl(lp->regbase + ETH_DMA_OPMODE_OFFSET));
	}

	val = readl(lp->regbase + ETH_DMA_OPMODE_OFFSET);
	val |= ETH_DMA_OPMODE_FTF;
	writel(val, lp->regbase + ETH_DMA_OPMODE_OFFSET);
}

static inline void ambhw_dma_tx_restart(struct ambeth_info *lp, u32 entry)
{
	lp->tx.desc_tx[entry].status = ETH_TDES0_OWN | ETH_TDES0_IC;
	dma_wmb();
	writel((u32)lp->tx_dma_desc + (entry * sizeof(struct ambeth_desc)),
		lp->regbase + ETH_DMA_TX_DESC_LIST_OFFSET);
	if (netif_msg_tx_err(lp)) {
		dev_err(&lp->ndev->dev, "TX Error: restart %u.\n", entry);
		ambhw_dump(lp);
	}
	ambhw_dma_tx_start(lp);
}

static inline void ambhw_dma_tx_poll(struct ambeth_info *lp)
{
	writel(0x01, lp->regbase + ETH_DMA_TX_POLL_DMD_OFFSET);
}

static inline void ambhw_start_tx_rx(struct ambeth_info *priv)
{
	setbitsl(ETH_MAC_CFG_RE, priv->regbase + ETH_MAC_CFG_OFFSET);
	setbitsl(ETH_MAC_CFG_TE, priv->regbase + ETH_MAC_CFG_OFFSET);
}

static inline void ambhw_stop_tx_rx(struct ambeth_info *lp)
{
	u32 i = 1300, irq_status, val;

	val = readl(lp->regbase + ETH_MAC_CFG_OFFSET);
	val &= ~ETH_MAC_CFG_RE;
	writel(val, lp->regbase + ETH_MAC_CFG_OFFSET);

	val = readl(lp->regbase + ETH_DMA_OPMODE_OFFSET);
	val &= ~(ETH_DMA_OPMODE_SR | ETH_DMA_OPMODE_ST);
	writel(val, lp->regbase + ETH_DMA_OPMODE_OFFSET);

	do {
		udelay(1);
		irq_status = readl(lp->regbase + ETH_DMA_STATUS_OFFSET);
	} while ((irq_status & (ETH_DMA_STATUS_TS_MASK |
		ETH_DMA_STATUS_RS_MASK)) && --i);
	if ((i == 0) && netif_msg_drv(lp)) {
		dev_err(&lp->ndev->dev,
			"DMA Error: Stop TX/RX status=0x%x, opmode=0x%x.\n",
			readl(lp->regbase + ETH_DMA_STATUS_OFFSET),
			readl(lp->regbase + ETH_DMA_OPMODE_OFFSET));
	}

	val = readl(lp->regbase + ETH_MAC_CFG_OFFSET);
	val &= ~ETH_MAC_CFG_TE;
	writel(val, lp->regbase + ETH_MAC_CFG_OFFSET);
}

static inline void ambhw_set_dma_desc(struct ambeth_info *lp)
{
	writel(lp->rx_dma_desc, lp->regbase + ETH_DMA_RX_DESC_LIST_OFFSET);
	writel(lp->tx_dma_desc, lp->regbase + ETH_DMA_TX_DESC_LIST_OFFSET);
}

static inline void ambhw_set_hwaddr(struct ambeth_info *lp, u8 *hwaddr)
{
	u32 val;

	val = (hwaddr[5] << 8) | hwaddr[4];
	writel(val, lp->regbase + ETH_MAC_MAC0_HI_OFFSET);
	udelay(4);
	val = (hwaddr[3] << 24) | (hwaddr[2] << 16) |
		(hwaddr[1] << 8) | hwaddr[0];
	writel(val, lp->regbase + ETH_MAC_MAC0_LO_OFFSET);
}

static inline void ambhw_get_hwaddr(struct ambeth_info *lp, u8 *hwaddr)
{
	u32 hval;
	u32 lval;

	hval = readl(lp->regbase + ETH_MAC_MAC0_HI_OFFSET);
	lval = readl(lp->regbase + ETH_MAC_MAC0_LO_OFFSET);
	hwaddr[5] = ((hval >> 8) & 0xff);
	hwaddr[4] = ((hval >> 0) & 0xff);
	hwaddr[3] = ((lval >> 24) & 0xff);
	hwaddr[2] = ((lval >> 16) & 0xff);
	hwaddr[1] = ((lval >> 8) & 0xff);
	hwaddr[0] = ((lval >> 0) & 0xff);
}

static void ambeth_fc_resolve(struct ambeth_info *lp);

static inline void ambhw_set_link_mode_speed(struct ambeth_info *lp)
{
	u32 val;

	val = readl(lp->regbase + ETH_MAC_CFG_OFFSET);
	switch (lp->oldspeed) {
	case SPEED_1000:
		val &= ~(ETH_MAC_CFG_PS);
		break;
	case SPEED_100:
		val |= ETH_MAC_CFG_PS;
		val |= ETH_MAC_CFG_FES;
		break;
	case SPEED_10:
		val |= ETH_MAC_CFG_PS;
		val &= ~(ETH_MAC_CFG_FES);
		break;
	default:
		break;
	}
	if (lp->oldduplex) {
		val &= ~(ETH_MAC_CFG_DO);
		val |= ETH_MAC_CFG_DM;
	} else {
		val &= ~(ETH_MAC_CFG_DM);
		val |= ETH_MAC_CFG_DO;
	}
	writel(val, lp->regbase + ETH_MAC_CFG_OFFSET);
	ambeth_fc_resolve(lp);
}

static inline int ambhw_enable(struct ambeth_info *lp)
{
	int ret_val = 0;
	u32 val;

	ret_val = ambhw_dma_reset(lp);
	if (ret_val)
		goto ambhw_init_exit;

	ambhw_set_hwaddr(lp, lp->ndev->dev_addr);

	val = ETH_DMA_BUS_MODE_FB | ETH_DMA_BUS_MODE_PBL_32 |
		ETH_DMA_BUS_MODE_DA_RX | ETH_DMA_BUS_MODE_ATDS;
	writel(val, lp->regbase + ETH_DMA_BUS_MODE_OFFSET);
	val = readl(lp->regbase + ETH_DMA_BUS_MODE_OFFSET);
	if (!val) {
		ret_val = -EIO;
		dev_err(lp->dev, "No enet_ext_osc_clk, Mac doesn't work \n");
		goto ambhw_init_exit;
	}
	writel(0, lp->regbase + ETH_MAC_FRAME_FILTER_OFFSET);

	val = ETH_DMA_OPMODE_TTC_256 | ETH_DMA_OPMODE_RTC_64 |
		ETH_DMA_OPMODE_FUF ;

	val |= ETH_DMA_OPMODE_TSF;

	/* bugged JUMBO frame: if MTU is bigger than 1500, should disable Tx COE and
	* RSF. please refer to stmmac GMAC. */
	if (lp->bfsize == AMBETH_DEFAULT_BFSIZE)
		val |= ETH_DMA_OPMODE_RSF;
	else
		; /* disable RSF */

	writel(val, lp->regbase + ETH_DMA_OPMODE_OFFSET);

	writel(ETH_MAC_CFG_TE | ETH_MAC_CFG_RE | ETH_MAC_CFG_JD,
			lp->regbase + ETH_MAC_CFG_OFFSET);

	if (lp->ndev->mtu > 1500)
		setbitsl(ETH_MAC_CFG_2K, lp->regbase + ETH_MAC_CFG_OFFSET);
	if (lp->ndev->mtu > 2000)
		setbitsl(ETH_MAC_CFG_JE, lp->regbase + ETH_MAC_CFG_OFFSET);

	val = (0x3f << 1) | (0xff << 16);
	writel(val, lp->regbase + ETH_DMA_AXI_BUS_MODE_OFFSET);
	/*
	 * (512 bits / N) * pause_time = actual pause time
	 * ex:
	 *     512 bits / 1 Gbps * 1954 = ~0.0010 sec = 1 ms
	 *     512 bits / 100 Mbps * 1954 = ~0.010 sec = 10 ms
	 */
	val = ETH_MAC_FLOW_CTR_PLT_256 | ETH_MAC_FLOW_CTR_PT(AMBETH_FC_PAUSE_TIME);
	writel(val, lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);

	if (lp->ipc_rx) {
		val = readl(lp->regbase + ETH_MAC_CFG_OFFSET);
		val |= ETH_MAC_CFG_IPC;
		writel(val, lp->regbase + ETH_MAC_CFG_OFFSET);
	}

	if (lp->dump_rx_all) {
		val = readl(lp->regbase + ETH_MAC_FRAME_FILTER_OFFSET);
		val |= ETH_MAC_FRAME_FILTER_RA;
		writel(val, lp->regbase + ETH_MAC_FRAME_FILTER_OFFSET);
	}

	writel(0xFFFFFFFF, lp->regbase + ETH_MAC_INTERRUPT_MASK_OFFSET);

	val = readl(lp->regbase + ETH_DMA_STATUS_OFFSET);
	writel(val, lp->regbase + ETH_DMA_STATUS_OFFSET);

ambhw_init_exit:
	return ret_val;
}

static inline void ambhw_disable(struct ambeth_info *lp)
{
	ambhw_stop_tx_rx(lp);
	ambhw_dma_int_disable(lp);
}

static void ambhw_dump_buffer(const char *msg,
	unsigned char *data, unsigned int length)
{

	char *p, *q;
	unsigned int i;
	int dlen = length > 48 ? 48 : length;

	q = p = kmalloc(strlen(msg) + dlen * 3 + 1, GFP_ATOMIC);
	if (!q) {
		pr_err("%s: no enough memory\n", __func__);
		return;
	}
	sprintf(q, "%s:", msg);
	q += (strlen(msg) + 1);

	for (i = 0; i < dlen; i++, q+=3)
		sprintf(q, "%02x:", data[i]);

	printk("%s\n", p);

	kfree(p);
}

/* ==========================================================================*/

static int ambahb_mdio_poll_status(struct mii_bus *bus)
{
	struct ambeth_info *lp = bus->priv;
	unsigned int value;
	int i;

	for (i = 0; i < 1000; i++) {
		regmap_read(lp->reg_scr, lp->soc_offsets->ahbsp_gmii_addr, &value);
		value = ahbmdio_to_le32(lp->id, value);

		if (!(value & BIT(0)))
			return 0;

		spin_unlock(&ahbmdio_lock);
		schedule_timeout(HZ / 10);
		spin_lock(&ahbmdio_lock);
	}

	pr_err("%s: timeout to wait for AHB MDIO ready.\n", bus->name);

	return -ETIMEDOUT;
}

static int ambahb_mdio_read(struct mii_bus *bus, int mii_id, int regnum)
{
	struct ambeth_info *lp = bus->priv;
	unsigned int regval, value, mask;

	regval = ETH_MAC_GMII_ADDR_PA(mii_id) | ETH_MAC_GMII_ADDR_GR(regnum);
	regval |= ((lp->ahb_mdio_clk_div - 1 ) << 2);
	regval |= (0 << 1);			/* Read enable */
	regval |= (1 << 0);

	regval = le32_to_ahbmdio(lp->id, regval);
	mask = le32_to_ahbmdio(lp->id, 0xFFFF);

	spin_lock(&ahbmdio_lock);
	regmap_update_bits(lp->reg_scr, lp->soc_offsets->ahbsp_gmii_addr, mask, regval);

	if (ambahb_mdio_poll_status(bus)) {
		spin_unlock(&ahbmdio_lock);
		return 0;
	}

	regmap_read(lp->reg_scr, lp->soc_offsets->ahbsp_gmii_data, &value);
	spin_unlock(&ahbmdio_lock);

	value = ahbmdio_to_le32(lp->id, value);
	dev_dbg(lp->dev, "R: id.%02x reg.%02x val.%02x\n", mii_id, regnum, value);

	return value;
}

static int ambahb_mdio_write(struct mii_bus *bus, int mii_id,
		int regnum, u16 value)
{
	struct ambeth_info *lp = bus->priv;
	unsigned int regval, opval, mask;
	int rval = 0;

	dev_dbg(lp->dev, "W: id.%02x reg.%02x val.%02x\n", mii_id, regnum, value);

	regval = ETH_MAC_GMII_ADDR_PA(mii_id) | ETH_MAC_GMII_ADDR_GR(regnum);
	regval |= ((lp->ahb_mdio_clk_div - 1) << 2);
	regval |= (1 << 1);			/* Write enable */
	regval |= (1 << 0);

	regval = le32_to_ahbmdio(lp->id, regval);
	opval = le32_to_ahbmdio(lp->id, value);
	mask = le32_to_ahbmdio(lp->id, 0xFFFF);

	spin_lock(&ahbmdio_lock);
	regmap_update_bits(lp->reg_scr, lp->soc_offsets->ahbsp_gmii_data, mask, opval);
	regmap_update_bits(lp->reg_scr, lp->soc_offsets->ahbsp_gmii_addr, mask, regval);

	if (ambahb_mdio_poll_status(bus))
		rval = -EIO;
	spin_unlock(&ahbmdio_lock);

	return rval;
}

static int ambhw_mdio_read(struct mii_bus *bus,
	int mii_id, int regnum)
{
	struct ambeth_info *lp = bus->priv;
	int val, cnt;

	for (cnt = AMBETH_MII_RETRY_CNT; cnt > 0; cnt--) {
		val = readl(lp->regbase + ETH_MAC_GMII_ADDR_OFFSET);
		if (!(val & ETH_MAC_GMII_ADDR_GB))
			break;
		udelay(10);
	}
	if ((cnt <= 0) && netif_msg_hw(lp)) {
		dev_err(&lp->ndev->dev, "MII Error: Preread tmo!\n");
		val = 0xFFFFFFFF;
		goto ambhw_mdio_read_exit;
	}

	val = ETH_MAC_GMII_ADDR_PA(mii_id) | ETH_MAC_GMII_ADDR_GR(regnum);
	val |= ETH_MAC_GMII_ADDR_CR_250_300MHZ | ETH_MAC_GMII_ADDR_GB;
	writel(val, lp->regbase + ETH_MAC_GMII_ADDR_OFFSET);

	for (cnt = AMBETH_MII_RETRY_CNT; cnt > 0; cnt--) {
		val = readl(lp->regbase + ETH_MAC_GMII_ADDR_OFFSET);
		if (!(val & ETH_MAC_GMII_ADDR_GB))
			break;
		udelay(10);
	}
	if ((cnt <= 0) && netif_msg_hw(lp)) {
		dev_err(&lp->ndev->dev, "MII Error: Postread tmo!\n");
		val = 0xFFFFFFFF;
		goto ambhw_mdio_read_exit;
	}

	val = readl(lp->regbase + ETH_MAC_GMII_DATA_OFFSET);

ambhw_mdio_read_exit:
	if (netif_msg_hw(lp))
		dev_info(&lp->ndev->dev,
			"MII Read: addr[0x%02x], reg[0x%02x], val[0x%04x].\n",
			mii_id, regnum, val);

	return val;
}

static int ambhw_mdio_write(struct mii_bus *bus,
	int mii_id, int regnum, u16 value)
{
	int ret_val = 0;
	struct ambeth_info *lp;
	int val;
	int cnt = 0;

	lp = (struct ambeth_info *)bus->priv;

	if (netif_msg_hw(lp))
		dev_info(&lp->ndev->dev,
			"MII Write: id[0x%02x], add[0x%02x], val[0x%04x].\n",
			mii_id, regnum, value);

	for (cnt = AMBETH_MII_RETRY_CNT; cnt > 0; cnt--) {
		val = readl(lp->regbase + ETH_MAC_GMII_ADDR_OFFSET);
		if (!(val & ETH_MAC_GMII_ADDR_GB))
			break;
		udelay(10);
	}
	if ((cnt <= 0) && netif_msg_hw(lp)) {
		dev_err(&lp->ndev->dev, "MII Error: Prewrite tmo!\n");
		ret_val = -EIO;
		goto ambhw_mdio_write_exit;
	}

	val = value;
	writel(val, lp->regbase + ETH_MAC_GMII_DATA_OFFSET);
	val = ETH_MAC_GMII_ADDR_PA(mii_id) | ETH_MAC_GMII_ADDR_GR(regnum);
	val |= ETH_MAC_GMII_ADDR_CR_250_300MHZ | ETH_MAC_GMII_ADDR_GW |
		ETH_MAC_GMII_ADDR_GB;
	writel(val, lp->regbase + ETH_MAC_GMII_ADDR_OFFSET);

	for (cnt = AMBETH_MII_RETRY_CNT; cnt > 0; cnt--) {
		val = readl(lp->regbase + ETH_MAC_GMII_ADDR_OFFSET);
		if (!(val & ETH_MAC_GMII_ADDR_GB))
			break;
		udelay(10);
	}
	if ((cnt <= 0) && netif_msg_hw(lp)) {
		dev_err(&lp->ndev->dev, "MII Error: Postwrite tmo!\n");
		ret_val = -EIO;
		goto ambhw_mdio_write_exit;
	}

ambhw_mdio_write_exit:
	return ret_val;
}

static void ambeth_phy_init(struct ambeth_info *priv)
{
	if (gpio_is_valid(priv->pwr_gpio)) {
		gpio_direction_output(priv->pwr_gpio, !priv->pwr_gpio_active);
		msleep(20);
		gpio_direction_output(priv->pwr_gpio, priv->pwr_gpio_active);
		msleep(20);
	}

	if (gpio_is_valid(priv->rst_gpio)) {
		gpio_direction_output(priv->rst_gpio, priv->rst_gpio_active);
		msleep(priv->rst_gpio_delay);
		gpio_direction_output(priv->rst_gpio, !priv->rst_gpio_active);
		msleep(priv->rst_gpio_delay);
	}
}

static void ambeth_phy_deinit(struct ambeth_info *priv)
{

	if (gpio_is_valid(priv->rst_gpio)) {
		gpio_direction_output(priv->rst_gpio, priv->rst_gpio_active);
		msleep(20);
	}

	if (gpio_is_valid(priv->pwr_gpio)) {
		gpio_direction_output(priv->pwr_gpio, !priv->pwr_gpio_active);
		msleep(20);
	}

}

static int ambhw_mdio_reset(struct mii_bus *bus)
{
	ambeth_phy_init(bus->priv);

	return 0;
}

/* ==========================================================================*/
static void ambeth_adjust_link(struct net_device *ndev)
{
	struct ambeth_info *lp = netdev_priv(ndev);
	struct phy_device *phydev = lp->phydev;
	int need_update = 0;
	unsigned long flags;

	spin_lock_irqsave(&lp->lock, flags);

	if (phydev->link) {
		if (phydev->duplex != lp->oldduplex) {
			need_update = 1;
			lp->oldduplex = phydev->duplex;
		}
		if (phydev->speed != lp->oldspeed) {
			switch (phydev->speed) {
			case SPEED_1000:
			case SPEED_100:
			case SPEED_10:
				need_update = 1;
				lp->oldspeed = phydev->speed;
				break;
			default:
				if (netif_msg_link(lp))
					dev_warn(&lp->ndev->dev,
						"Unknown Speed(%d).\n",
						phydev->speed);
				break;
			}
		}
		if (phydev->pause != lp->oldpause ||
		    phydev->asym_pause != lp->oldasym_pause) {
			lp->oldpause = phydev->pause;
			lp->oldasym_pause = phydev->asym_pause;
			need_update = 1;
		}
		if (lp->oldlink != phydev->link) {
			need_update = 1;
			lp->oldlink = phydev->link;
		}
	} else if (lp->oldlink) {
		need_update = 1;
		lp->oldlink = PHY_DOWN;
		lp->oldspeed = 0;
		lp->oldduplex = -1;
	}

	if (need_update) {
		ambhw_set_link_mode_speed(lp);
		if (netif_msg_link(lp)) {
			if (phydev->link) {
				dev_info(&lp->ndev->dev, "Link is up - Speed is %s\n",
					phydev->speed == SPEED_1000 ? "1000M" :
					phydev->speed == SPEED_100 ? "100M" :
					phydev->speed == SPEED_10 ? "10M" : "Unknown");
			} else {
				dev_info(&lp->ndev->dev, "Link is Down\n");
			}

		}
	}
	spin_unlock_irqrestore(&lp->lock, flags);
}

static void ambeth_fc_config(struct ambeth_info *lp)
{
	u32 sup, adv, flow_ctr;

	ethtool_convert_link_mode_to_legacy_u32(&adv, lp->phydev->advertising);
	ethtool_convert_link_mode_to_legacy_u32(&sup, lp->phydev->supported);
	flow_ctr = lp->flow_ctr;

	sup |= (SUPPORTED_Pause | SUPPORTED_Asym_Pause);
	adv &= ~(ADVERTISED_Pause | ADVERTISED_Asym_Pause);

	sup &= lp->phy_supported;

	if (!(sup & (SUPPORTED_Pause | SUPPORTED_Asym_Pause)))
		goto unsupported;

        if (lp->fixed_speed != SPEED_UNKNOWN)
		goto autoneg_unsupported;

	if (!(sup & SUPPORTED_Autoneg) ||
	    !(flow_ctr & AMBETH_FC_AUTONEG))
		goto autoneg_unsupported;

	if (flow_ctr & AMBETH_FC_RX) {
		/*
		 * Being able to decode pause frames is sufficently to
		 * advertise that we support both sym. and asym. pause.
		 * It doesn't matter if send pause frame or not.
                 */
		adv |= (ADVERTISED_Pause | ADVERTISED_Asym_Pause);
		goto done;
	}

	if (flow_ctr & AMBETH_FC_TX) {
		/* Tell the link parter that we do send the pause frame. */
		adv |= ADVERTISED_Asym_Pause;
		goto done;
	}
	goto done;

autoneg_unsupported:
	/* Sanitize the config value */
	lp->flow_ctr &= ~AMBETH_FC_AUTONEG;

	/* Advertise nothing about pause frame */
	adv &= ~(ADVERTISED_Pause | ADVERTISED_Asym_Pause);
	goto done;

unsupported:
	/* Sanitize the config value */
	lp->flow_ctr &= ~(AMBETH_FC_AUTONEG | AMBETH_FC_RX | AMBETH_FC_TX);
done:
	ethtool_convert_legacy_u32_to_link_mode(lp->phydev->advertising, adv);
	ethtool_convert_legacy_u32_to_link_mode(lp->phydev->supported, sup);

	dev_info(&lp->ndev->dev, "adv: sym %d, asym: %d\n",
		 !!(adv & ADVERTISED_Pause),
		 !!(adv & ADVERTISED_Asym_Pause));
}

static void ambeth_fc_resolve(struct ambeth_info *lp)
{
	u32 flow_ctr, fc, old_fc;

	flow_ctr = lp->flow_ctr;

	if (!(flow_ctr & AMBETH_FC_AUTONEG))
		goto force_setting;

	fc = old_fc = readl(lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);

	dev_info(&lp->ndev->dev, "lp: sym: %d, asym: %d\n",
		 lp->phydev->pause, lp->phydev->asym_pause);
	/*
	 * Decode pause frames only if user specified, and the link
	 * partner could send them on the same time.
	 */
	if ((flow_ctr & AMBETH_FC_RX) &&
	    (lp->phydev->pause || lp->phydev->asym_pause))
		fc |= ETH_MAC_FLOW_CTR_RFE;
	else
		fc &= ~ETH_MAC_FLOW_CTR_RFE;

	/*
	 * Send pause frames only if user specified, and the link
	 * partner can resopnds to them on the same time.
	 */
	if ((flow_ctr & AMBETH_FC_TX) && lp->phydev->pause)
		fc |= ETH_MAC_FLOW_CTR_TFE;
	else
		fc &= ~ETH_MAC_FLOW_CTR_TFE;

	if (fc != old_fc)
		writel(fc, lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);

	return;

force_setting:

	if (flow_ctr & AMBETH_FC_TX) {
		fc = readl(lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);
		fc |= ETH_MAC_FLOW_CTR_TFE;
		writel(fc, lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);
	}

	if (flow_ctr & AMBETH_FC_RX) {
		fc = readl(lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);
		fc |= ETH_MAC_FLOW_CTR_RFE;
		writel(fc, lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);
	}
}

static int ambeth_phy_start(struct ambeth_info *lp)
{
	struct net_device *ndev = lp->ndev;
	struct phy_device *phydev = lp->phydev;
	int ret_val = 0;
	unsigned long flags;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };

	if (lp->phy_enabled)
		return 0;

	lp->oldlink = PHY_DOWN;
	lp->oldspeed = 0;
	lp->oldduplex = -1;

	/* Fixed Link mode: we allow all valid fixed_speed,
	   even HW can not support the speed. */
	switch (lp->fixed_speed) {
	case SPEED_1000:
	case SPEED_100:
	case SPEED_10:
		lp->oldlink = PHY_RUNNING;
		lp->oldspeed = lp->fixed_speed;
		lp->oldduplex = DUPLEX_FULL;
		ambhw_set_link_mode_speed(lp);
		dev_notice(&lp->ndev->dev, "Fixed Link - %d/%s\n", lp->oldspeed,
			((lp->oldduplex == DUPLEX_FULL) ? "Full" : "Half"));
		netif_carrier_on(ndev);
		goto ambeth_init_phy_exit;
		break;
	default:
		break;
	}

	ret_val = phy_connect_direct(ndev, phydev, &ambeth_adjust_link, lp->intf_type);
	if (ret_val) {
		dev_err(&lp->ndev->dev, "Could not attach to PHY!\n");
		goto ambeth_init_phy_exit;
	}

	ethtool_convert_legacy_u32_to_link_mode(mask, lp->phy_supported);
	bitmap_and(phydev->supported, phydev->supported, mask, 32);
	bitmap_copy(phydev->advertising, phydev->supported, __ETHTOOL_LINK_MODE_MASK_NBITS);

	spin_lock_irqsave(&lp->lock, flags);
	lp->phy_enabled = 1;
	spin_unlock_irqrestore(&lp->lock, flags);

	ambeth_fc_config(lp);
	ambeth_fc_resolve(lp);

ambeth_init_phy_exit:
	return ret_val;
}

static void ambeth_phy_stop(struct ambeth_info *lp)
{
	unsigned long flags;

	spin_lock_irqsave(&lp->lock, flags);
	lp->phy_enabled = 0;
	lp->oldlink = PHY_DOWN;
	spin_unlock_irqrestore(&lp->lock, flags);
}

static inline int ambeth_rx_rngmng_check_skb(struct ambeth_info *lp, u32 entry)
{
	int ret_val = 0;
	dma_addr_t mapping;
	struct sk_buff *skb;

	if (lp->rx.rng_rx[entry].skb == NULL) {
		skb = netdev_alloc_skb(lp->ndev, lp->bfsize);
		if (skb == NULL) {
			if (netif_msg_drv(lp))
				dev_err(&lp->ndev->dev,
				"RX Error: netdev_alloc_skb.\n");
			ret_val = -ENOMEM;
			goto ambeth_rx_rngmng_skb_exit;
		}
		mapping = dma_map_single(lp->ndev->dev.parent, skb->data,
			lp->bfsize, DMA_FROM_DEVICE);
		lp->rx.rng_rx[entry].skb = skb;
		lp->rx.rng_rx[entry].mapping = mapping;
		lp->rx.desc_rx[entry].buffer1 = mapping;
	}

ambeth_rx_rngmng_skb_exit:
	return ret_val;
}

static inline void ambeth_rx_rngmng_init(struct ambeth_info *lp)
{
	int i;

	lp->rx.cur_rx = 0;
	lp->rx.dirty_rx = 0;
	for (i = 0; i < lp->rx_count; i++) {
		if (ambeth_rx_rngmng_check_skb(lp, i))
			break;

		lp->rx.desc_rx[i].status = ETH_RDES0_OWN;

		lp->rx.desc_rx[i].length = ETH_RDES1_RCH | ETH_RDES1_RBS1(lp->bfsize);

		lp->rx.desc_rx[i].buffer2 = (u32)lp->rx_dma_desc +
			((i + 1) * sizeof(struct ambeth_desc));
	}
	dma_wmb();

	lp->rx.desc_rx[lp->rx_count - 1].buffer2 = (u32)lp->rx_dma_desc;
}

static inline void ambeth_rx_rngmng_refill(struct ambeth_info *lp)
{
	u32 i;
	unsigned int dirty_diff;
	u32 entry;

	dirty_diff = (lp->rx.cur_rx - lp->rx.dirty_rx);
	for (i = 0; i < dirty_diff; i++) {
		entry = lp->rx.dirty_rx % lp->rx_count;
		if (ambeth_rx_rngmng_check_skb(lp, entry))
			break;

		lp->rx.desc_rx[entry].status = ETH_RDES0_OWN;
		dma_wmb();
		lp->rx.dirty_rx++;
	}
}

static inline void ambeth_rx_rngmng_del(struct ambeth_info *lp)
{
	int i;
	dma_addr_t mapping;
	struct sk_buff *skb;

	for (i = 0; i < lp->rx_count; i++) {
		if (lp->rx.rng_rx) {
			skb = lp->rx.rng_rx[i].skb;
			mapping = lp->rx.rng_rx[i].mapping;
			lp->rx.rng_rx[i].skb = NULL;
			lp->rx.rng_rx[i].mapping = 0;
			if (mapping) {
				dma_unmap_single(lp->ndev->dev.parent, mapping,
						lp->bfsize,
						DMA_FROM_DEVICE);
			}
			if (skb) {
				dev_kfree_skb(skb);
			}
		}
		if (lp->rx.desc_rx) {
			lp->rx.desc_rx[i].status = 0;
			lp->rx.desc_rx[i].length = 0;
			lp->rx.desc_rx[i].buffer1 = 0xBADF00D0;
			lp->rx.desc_rx[i].buffer2 = 0xBADF00D0;
		}
	}
}

static inline void ambeth_tx_rngmng_init(struct ambeth_info *lp)
{
	u32 i;

	lp->tx.cur_tx = 0;
	lp->tx.dirty_tx = 0;
	for (i = 0; i < lp->tx_count; i++) {
		lp->tx.rng_tx[i].mapping = 0 ;

		lp->tx.desc_tx[i].status = ETH_TDES0_LS | ETH_TDES0_FS | ETH_TDES0_TCH;
		lp->tx.desc_tx[i].length = 0;
		lp->tx.desc_tx[i].buffer1 = 0;
		lp->tx.desc_tx[i].buffer2 = (u32)lp->tx_dma_desc +
			((i + 1) * sizeof(struct ambeth_desc));
	}
	lp->tx.desc_tx[lp->tx_count - 1].buffer2 = (u32)lp->tx_dma_desc;
}

static inline void ambeth_tx_rngmng_del(struct ambeth_info *lp)
{
	u32 i;
	dma_addr_t mapping;
	struct sk_buff *skb;

	for (i = 0; i < lp->tx_count; i++) {
		if (lp->tx.rng_tx) {
			skb = lp->tx.rng_tx[i].skb;
			mapping = lp->tx.rng_tx[i].mapping;
			lp->tx.rng_tx[i].skb = NULL;
			lp->tx.rng_tx[i].mapping = 0;
			if (skb) {
				dma_unmap_single(lp->ndev->dev.parent, mapping,
					skb->len, DMA_TO_DEVICE);
				dev_kfree_skb(skb);
			}
		}
		if (lp->tx.desc_tx) {
			lp->tx.desc_tx[i].status = 0;
			lp->tx.desc_tx[i].length = 0;
			lp->tx.desc_tx[i].buffer1 = 0xBADF00D0;
			lp->tx.desc_tx[i].buffer2 = 0xBADF00D0;
		}
	}
}

static inline void ambeth_check_dma_error(struct ambeth_info *lp,
	u32 irq_status)
{
	u32 miss_ov = 0;

	if (unlikely(irq_status & ETH_DMA_STATUS_AIS)) {
		if (irq_status & (ETH_DMA_STATUS_RU | ETH_DMA_STATUS_OVF))
			miss_ov = readl(lp->regbase +
				ETH_DMA_MISS_FRAME_BOCNT_OFFSET);

		if (irq_status & ETH_DMA_STATUS_FBI) {
			if (netif_msg_drv(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Fatal Bus Error 0x%x.\n",
				(irq_status & ETH_DMA_STATUS_EB_MASK));
		}
		if (irq_status & ETH_DMA_STATUS_ETI) {
			if (netif_msg_tx_err(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Early Transmit.\n");
		}
		if (irq_status & ETH_DMA_STATUS_RWT) {
			if (netif_msg_rx_err(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Receive Watchdog Timeout.\n");
		}
		if (irq_status & ETH_DMA_STATUS_RPS) {
			if (netif_msg_rx_err(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Receive Process Stopped.\n");
		}
		if (irq_status & ETH_DMA_STATUS_RU) {
			if (miss_ov & ETH_DMA_MISS_FRAME_BOCNT_FRAME) {
				lp->stats.rx_dropped +=
					ETH_DMA_MISS_FRAME_BOCNT_HOST(miss_ov);
			}
			if (netif_msg_rx_err(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Receive Buffer Unavailable, %u.\n",
				ETH_DMA_MISS_FRAME_BOCNT_HOST(miss_ov));
		}
		if (irq_status & ETH_DMA_STATUS_UNF) {
			if (netif_msg_tx_err(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Transmit Underflow.\n");
		}
		if (irq_status & ETH_DMA_STATUS_OVF) {
			if (miss_ov & ETH_DMA_MISS_FRAME_BOCNT_FIFO) {
				lp->stats.rx_fifo_errors +=
					ETH_DMA_MISS_FRAME_BOCNT_APP(miss_ov);
			}
			if (netif_msg_rx_err(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Receive FIFO Overflow, %u.\n",
				ETH_DMA_MISS_FRAME_BOCNT_APP(miss_ov));
		}
		if (irq_status & ETH_DMA_STATUS_TJT) {
			lp->stats.tx_errors++;
			if (netif_msg_drv(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Transmit Jabber Timeout.\n");
		}
		if (irq_status & ETH_DMA_STATUS_TPS) {
			if (netif_msg_tx_err(lp))
				dev_err(&lp->ndev->dev,
				"DMA Error: Transmit Process Stopped.\n");
		}
		if (netif_msg_tx_err(lp) || netif_msg_rx_err(lp)) {
			dev_err(&lp->ndev->dev, "DMA Error: Abnormal: 0x%x.\n",
				irq_status);
			ambhw_dump(lp);
		}
	}
}

static inline void ambeth_pause_frame(struct ambeth_info *lp)
{
	u32					fc;

	fc = readl(lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);
	if (!(fc & ETH_MAC_FLOW_CTR_TFE))
		return;

	fc |= ETH_MAC_FLOW_CTR_FCBBPA;

	writel(fc, lp->regbase + ETH_MAC_FLOW_CTR_OFFSET);
}

static inline void ambeth_interrupt_rx(struct ambeth_info *lp, u32 irq_status)
{
	if (irq_status & AMBETH_RXDMA_STATUS) {
		u32 val;
		val = readl(lp->regbase + ETH_DMA_INTEN_OFFSET);
		val &= ~AMBETH_RXDMA_INTEN;
		writel(val, lp->regbase + ETH_DMA_INTEN_OFFSET);
		napi_schedule(&lp->napi);
	}
}
static inline void ambeth_interrupt_gmac(struct ambeth_info *lp, u32 irq_status)
{
	u32 tmp_reg;

	if (irq_status & ETH_DMA_STATUS_GPI) {
		dev_vdbg(&lp->ndev->dev, "ETH_DMA_STATUS_GPI\n");
	}
	if (irq_status & ETH_DMA_STATUS_GMI) {
		dev_vdbg(&lp->ndev->dev, "ETH_DMA_STATUS_GMI\n");
	}
	if (irq_status & ETH_DMA_STATUS_GLI) {
		dev_vdbg(&lp->ndev->dev, "ETH_DMA_STATUS_GLI\n");
		tmp_reg = readl(lp->regbase +
			ETH_MAC_INTERRUPT_STATUS_OFFSET);
		dev_vdbg(&lp->ndev->dev,
			"ETH_MAC_INTERRUPT_STATUS_OFFSET = 0x%08X\n",tmp_reg);
		tmp_reg = readl(lp->regbase +
			ETH_MAC_INTERRUPT_MASK_OFFSET);
		dev_vdbg(&lp->ndev->dev,
			"ETH_MAC_INTERRUPT_MASK_OFFSET = 0x%08X\n",tmp_reg);
		tmp_reg = readl(lp->regbase +
			ETH_MAC_AN_STATUS_OFFSET);
		dev_vdbg(&lp->ndev->dev,
			"ETH_MAC_AN_STATUS_OFFSET = 0x%08X\n",tmp_reg);
		tmp_reg = readl(lp->regbase +
			ETH_MAC_RGMII_CS_OFFSET);
		dev_vdbg(&lp->ndev->dev,
			"ETH_MAC_RGMII_CS_OFFSET = 0x%08X\n",tmp_reg);
		tmp_reg = readl(lp->regbase +
			ETH_MAC_GPIO_OFFSET);
		dev_vdbg(&lp->ndev->dev,
			"ETH_MAC_GPIO_OFFSET = 0x%08X\n",tmp_reg);
	}
}

static inline u32 ambeth_check_tdes0_status(struct ambeth_info *lp,
	unsigned int status)
{
	u32 tx_retry = 0;

	if (status & ETH_TDES0_JT) {
		lp->stats.tx_heartbeat_errors++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "TX Error: Jabber Timeout.\n");
	}
	if (status & ETH_TDES0_FF) {
		lp->stats.tx_dropped++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "TX Error: Frame Flushed.\n");
	}
	if (status & ETH_TDES0_IPE) {
		lp->stats.tx_fifo_errors++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"TX Error: Payload Checksum Error.\n");
	}
	if (status & ETH_TDES0_LCA) {
		lp->stats.tx_carrier_errors++;
		dev_err(&lp->ndev->dev, "TX Error: Loss of Carrier.\n");
	}
	if (status & ETH_TDES0_NC) {
		lp->stats.tx_carrier_errors++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "TX Error: No Carrier.\n");
	}
	if (status & ETH_TDES0_LCO) {
		lp->stats.tx_aborted_errors++;
		lp->stats.collisions++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "TX Error: Late Collision.\n");
	}
	if (status & ETH_TDES0_EC) {
		lp->stats.tx_aborted_errors++;
		lp->stats.collisions += ETH_TDES0_CC(status);
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"TX Error: Excessive Collision %u.\n",
			ETH_TDES0_CC(status));
	}
	if (status & ETH_TDES0_VF) {
		if (netif_msg_drv(lp))
			dev_info(&lp->ndev->dev, "TX Info: VLAN Frame.\n");
	}
	if (status & ETH_TDES0_ED) {
		lp->stats.tx_fifo_errors++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"TX Error: Excessive Deferral.\n");
	}
	if (status & ETH_TDES0_UF) {
		tx_retry = 1;
		if (netif_msg_tx_err(lp)) {
			dev_err(&lp->ndev->dev, "TX Error: Underflow Error.\n");
			ambhw_dump(lp);
		}
	}
	if (status & ETH_TDES0_DB) {
		lp->stats.tx_fifo_errors++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "TX Error: Deferred Bit.\n");
	}

	return tx_retry;
}

static inline void ambeth_interrupt_tx(struct ambeth_info *lp, u32 irq_status)
{
	u32 i;
	unsigned int dirty_diff;
	u32 entry;
	u32 status;

	if (irq_status & AMBETH_TXDMA_STATUS) {
		dev_vdbg(&lp->ndev->dev, "cur_tx[%llu], dirty_tx[%llu], 0x%x.\n",
			lp->tx.cur_tx, lp->tx.dirty_tx, irq_status);
		dirty_diff = (lp->tx.cur_tx - lp->tx.dirty_tx);
		for (i = 0; i < dirty_diff; i++) {
			entry = (lp->tx.dirty_tx % lp->tx_count);
			status = lp->tx.desc_tx[entry].status;

			if (status & ETH_TDES0_OWN) {
				break;
			}

			dma_rmb();

			if (unlikely(status & ETH_TDES0_ES)) {
				if ((status & ETH_TDES0_ES_MASK) ==
					ETH_TDES0_ES) {
					break;
				}
				if (ambeth_check_tdes0_status(lp, status)) {
					ambhw_dma_tx_stop(lp);
					ambhw_dma_tx_restart(lp, entry);
					ambhw_dma_tx_poll(lp);
					break;
				} else {
					lp->stats.tx_errors++;
				}
			} else {
				if (unlikely(status & ETH_TDES0_IHE)) {
					if (netif_msg_drv(lp))
						dev_err(&lp->ndev->dev,
						"TX Error: IP Header Error.\n");
				}
				lp->stats.tx_bytes +=
					lp->tx.rng_tx[entry].skb->len;
				lp->stats.tx_packets++;
			}

			ambeth_get_tx_hwtstamp(lp, lp->tx.rng_tx[entry].skb,
					&lp->tx.desc_tx[entry]);

			dma_unmap_single(lp->ndev->dev.parent,
				lp->tx.rng_tx[entry].mapping,
				lp->tx.rng_tx[entry].skb->len,
				DMA_TO_DEVICE);

			dev_kfree_skb_irq(lp->tx.rng_tx[entry].skb);
			lp->tx.rng_tx[entry].skb = NULL;
			lp->tx.rng_tx[entry].mapping = 0;
			lp->tx.dirty_tx++;

			if (netif_queue_stopped(lp->ndev))
				netif_wake_queue(lp->ndev);
		}
		dirty_diff = (lp->tx.cur_tx - lp->tx.dirty_tx);
		if (dirty_diff && (irq_status & ETH_DMA_STATUS_TU)) {
			ambhw_dma_tx_poll(lp);
		}

		dev_vdbg(&lp->ndev->dev, "cur_tx[%llu], dirty_tx[%llu], 0x%x.\n",
			lp->tx.cur_tx, lp->tx.dirty_tx, irq_status);
	}
}

static irqreturn_t ambeth_interrupt(int irq, void *dev_id)
{
	struct net_device *ndev;
	struct ambeth_info *lp;
	u32 irq_status;
	unsigned long flags;

	ndev = (struct net_device *)dev_id;
	lp = netdev_priv(ndev);

	spin_lock_irqsave(&lp->lock, flags);
	irq_status = readl(lp->regbase + ETH_DMA_STATUS_OFFSET);
	ambeth_check_dma_error(lp, irq_status);
	ambeth_interrupt_gmac(lp, irq_status);
	ambeth_interrupt_rx(lp, irq_status);
	ambeth_interrupt_tx(lp, irq_status);
	writel(irq_status, lp->regbase + ETH_DMA_STATUS_OFFSET);
	spin_unlock_irqrestore(&lp->lock, flags);

	ambeth_interrupt_statis(lp, irq_status);

	return IRQ_HANDLED;
}

static int ambeth_start_hw(struct net_device *ndev)
{
	int ret_val = 0;
	struct ambeth_info *lp;
	unsigned long flags;

	lp = (struct ambeth_info *)netdev_priv(ndev);

	ambeth_phy_init(lp);

	spin_lock_irqsave(&lp->lock, flags);
	ret_val = ambhw_enable(lp);
	spin_unlock_irqrestore(&lp->lock, flags);
	if (ret_val)
		goto ambeth_start_hw_exit;

	lp->rx.rng_rx = kmalloc((sizeof(struct ambeth_rng_info) *
		lp->rx_count), GFP_KERNEL);
	if (lp->rx.rng_rx == NULL) {
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "alloc rng_rx fail.\n");
		ret_val = -ENOMEM;
		goto ambeth_start_hw_exit;
	}
	lp->rx.desc_rx = dma_alloc_coherent(lp->ndev->dev.parent,
		(sizeof(struct ambeth_desc) * lp->rx_count),
		&lp->rx_dma_desc, GFP_KERNEL);
	if (lp->rx.desc_rx == NULL) {
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"dma_alloc_coherent desc_rx fail.\n");
		ret_val = -ENOMEM;
		goto ambeth_start_hw_exit;
	}
	memset(lp->rx.rng_rx, 0,
		(sizeof(struct ambeth_rng_info) * lp->rx_count));
	memset(lp->rx.desc_rx, 0,
		(sizeof(struct ambeth_desc) * lp->rx_count));
	ambeth_rx_rngmng_init(lp);

	lp->tx.rng_tx = kmalloc((sizeof(struct ambeth_rng_info) *
		lp->tx_count), GFP_KERNEL);
	if (lp->tx.rng_tx == NULL) {
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "alloc rng_tx fail.\n");
		ret_val = -ENOMEM;
		goto ambeth_start_hw_exit;
	}
	lp->tx.desc_tx = dma_alloc_coherent(lp->ndev->dev.parent,
		(sizeof(struct ambeth_desc) * lp->tx_count),
		&lp->tx_dma_desc, GFP_KERNEL);
	if (lp->tx.desc_tx == NULL) {
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"dma_alloc_coherent desc_tx fail.\n");
		ret_val = -ENOMEM;
		goto ambeth_start_hw_exit;
	}
	memset(lp->tx.rng_tx, 0,
		(sizeof(struct ambeth_rng_info) * lp->tx_count));
	memset(lp->tx.desc_tx, 0,
		(sizeof(struct ambeth_desc) * lp->tx_count));
	ambeth_tx_rngmng_init(lp);

	spin_lock_irqsave(&lp->lock, flags);
	ambhw_set_dma_desc(lp);
	ambhw_dma_rx_start(lp);
	ambhw_dma_tx_start(lp);
	spin_unlock_irqrestore(&lp->lock, flags);

ambeth_start_hw_exit:
	return ret_val;
}

static void ambeth_stop_hw(struct net_device *ndev)
{
	struct ambeth_info *lp;
	unsigned long flags;

	lp = (struct ambeth_info *)netdev_priv(ndev);

	spin_lock_irqsave(&lp->lock, flags);
	ambhw_disable(lp);
	spin_unlock_irqrestore(&lp->lock, flags);

	ambeth_tx_rngmng_del(lp);
	if (lp->tx.desc_tx) {
		dma_free_coherent(lp->ndev->dev.parent,
			(sizeof(struct ambeth_desc) * lp->tx_count),
			lp->tx.desc_tx, lp->tx_dma_desc);
		lp->tx.desc_tx = NULL;
	}
	if (lp->tx.rng_tx) {
		kfree(lp->tx.rng_tx);
		lp->tx.rng_tx = NULL;
	}

	ambeth_rx_rngmng_del(lp);
	if (lp->rx.desc_rx) {
		dma_free_coherent(lp->ndev->dev.parent,
			(sizeof(struct ambeth_desc) * lp->rx_count),
			lp->rx.desc_rx, lp->rx_dma_desc);
		lp->rx.desc_rx = NULL;
	}
	if (lp->rx.rng_rx) {
		kfree(lp->rx.rng_rx);
		lp->rx.rng_rx = NULL;
	}

	ambeth_phy_deinit(lp);
}

static int ambeth_open(struct net_device *ndev)
{
	int ret_val = 0;
	struct ambeth_info *lp;

	lp = (struct ambeth_info *)netdev_priv(ndev);

	ret_val = ambeth_start_hw(ndev);
	if (ret_val)
		goto ambeth_open_exit;

	ret_val = request_irq(ndev->irq, ambeth_interrupt,
		IRQF_SHARED | IRQF_TRIGGER_HIGH, ndev->name, ndev);
	if (ret_val) {
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"Request_irq[%d] fail.\n", ndev->irq);
		goto ambeth_open_exit;
	}

	napi_enable(&lp->napi);
	netif_start_queue(ndev);
	ambhw_dma_int_enable(lp);

	netif_carrier_off(ndev);
	ret_val = ambeth_phy_start(lp);
	if (ret_val) {
		netif_stop_queue(ndev);
		napi_disable(&lp->napi);
		free_irq(ndev->irq, ndev);
	}

	if (lp->phydev)
		phy_start(lp->phydev);

ambeth_open_exit:
	if (ret_val) {
		ambeth_stop_hw(ndev);
	}

	return ret_val;
}

static int ambeth_stop(struct net_device *ndev)
{
	struct ambeth_info *lp = netdev_priv(ndev);
	int ret_val = 0;

	if (lp->phydev) {
		phy_stop(lp->phydev);
		phy_disconnect(lp->phydev);
	}

	netif_tx_disable(ndev);
	napi_disable(&lp->napi);
	free_irq(ndev->irq, ndev);
	ambeth_phy_stop(lp);
	netif_carrier_off(ndev);
	ambeth_stop_hw(ndev);

	return ret_val;
}

static int ambeth_hard_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	int ret_val = 0;
	struct ambeth_info *lp;
	dma_addr_t mapping;
	u32 entry;
	unsigned int dirty_diff;
	u32 tx_flag;
	unsigned long flags;
	struct netdev_queue *txq;

	txq = netdev_get_tx_queue(ndev, skb_get_queue_mapping(skb));
	lp = (struct ambeth_info *)netdev_priv(ndev);
	tx_flag = ETH_TDES0_LS | ETH_TDES0_FS | ETH_TDES0_TCH;

	if (unlikely((skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)))
		ambeth_tx_hwtstamp_enable(lp, &tx_flag);

	spin_lock_irqsave(&lp->lock, flags);
	dirty_diff = (lp->tx.cur_tx - lp->tx.dirty_tx);
	entry = (lp->tx.cur_tx % lp->tx_count);
	if (dirty_diff == lp->tx_irq_high) {
		tx_flag |= ETH_TDES0_IC;
	} else if (dirty_diff == (lp->tx_count - 1)) {
		if (!netif_queue_stopped(ndev))
			netif_stop_queue(ndev);
		tx_flag |= ETH_TDES0_IC;
	} else if (dirty_diff >= lp->tx_count) {
		ret_val = NETDEV_TX_BUSY;
		spin_unlock_irqrestore(&lp->lock, flags);

		if (!netif_queue_stopped(ndev))
			netif_stop_queue(ndev);
		printk("WARNING %s:TX Ring buffer is Overflow.\n", __func__);

		goto ambeth_hard_start_xmit_exit;
	}
	if (unlikely(lp->dump_tx))
		ambhw_dump_buffer(__func__, skb->data, skb->len);

	if (lp->ipc_tx && (skb->ip_summed == CHECKSUM_PARTIAL)) {
		ret_val = readl(lp->regbase + ETH_DMA_OPMODE_OFFSET);
		if(ret_val & ETH_DMA_OPMODE_TSF) {
			tx_flag |= ETH_TDES0_CIC_V2;
		} else {
			skb_set_transport_header(skb, skb_checksum_start_offset(skb));
			if(skb_checksum_help(skb))
				goto drop;
		}
	}

	mapping = dma_map_single(lp->ndev->dev.parent,
		skb->data, skb->len, DMA_TO_DEVICE);

	ret_val = dma_mapping_error(lp->ndev->dev.parent, mapping);
	if(ret_val) {
		dev_err(&lp->ndev->dev, "Tx DMA map failed\n");
		dev_kfree_skb(skb);
		spin_unlock_irqrestore(&lp->lock, flags);
		return ret_val;
	}

	skb_tx_timestamp(skb);
	lp->tx.rng_tx[entry].skb = skb;
	lp->tx.rng_tx[entry].mapping = mapping;
	lp->tx.desc_tx[entry].buffer1 = mapping;
	lp->tx.desc_tx[entry].length = ETH_TDES1_TBS1(skb->len);
	lp->tx.desc_tx[entry].status = ETH_TDES0_OWN | tx_flag;
	dma_wmb();

	lp->tx.cur_tx++;
	ambhw_dma_tx_poll(lp);
	spin_unlock_irqrestore(&lp->lock, flags);

#if 0 /* linux-4.8 use netdev_queue->trans_start instead */
	ndev->trans_start = jiffies;
#endif
	dev_vdbg(&lp->ndev->dev, "TX Info: cur_tx[%llu], dirty_tx[%llu], "
		"entry[%u], len[%u], data_len[%u], ip_summed[%u], "
		"csum_start[%u], csum_offset[%u].\n",
		lp->tx.cur_tx, lp->tx.dirty_tx, entry, skb->len, skb->data_len,
		skb->ip_summed, skb->csum_start, skb->csum_offset);

ambeth_hard_start_xmit_exit:
	return ret_val;
drop:
	dev_kfree_skb_any(skb);
	lp->stats.tx_errors++;
	spin_unlock_irqrestore(&lp->lock, flags);
	return 0;
}

static int ambeth_change_mtu(struct net_device *dev, int new_mtu)
{
	struct ambeth_info *priv = netdev_priv(dev);
	u32 bfsize;

	if (new_mtu > 4096)
		bfsize = 8100;
	else if (new_mtu > 2048)
		bfsize = 4096;
	else if (new_mtu > AMBETH_DEFAULT_BFSIZE)
		bfsize = 2048;
	else
		bfsize = AMBETH_DEFAULT_BFSIZE;

	if (!netif_running(dev)) {
		priv->bfsize = bfsize;
		dev->mtu = new_mtu;
		netdev_update_features(dev);
	} else {
		unsigned long flags;

		netif_trans_update(dev);	/* prevent tx timeout */
		napi_disable(&priv->napi);
		netif_tx_disable(dev);

		disable_irq(dev->irq);
		spin_lock_irqsave(&priv->lock, flags);
		ambhw_disable(priv);

		ambeth_tx_rngmng_del(priv);
		ambeth_rx_rngmng_del(priv);

		priv->bfsize = bfsize;
		dev->mtu = new_mtu;
		netdev_update_features(dev);

		if (priv->bfsize == AMBETH_DEFAULT_BFSIZE)
			setbitsl(ETH_DMA_OPMODE_RSF, priv->regbase + ETH_DMA_OPMODE_OFFSET);
		else
			clrbitsl(ETH_DMA_OPMODE_RSF, priv->regbase + ETH_DMA_OPMODE_OFFSET);

		if (dev->mtu > 1500)
			setbitsl(ETH_MAC_CFG_2K, priv->regbase + ETH_MAC_CFG_OFFSET);
		else
			clrbitsl(ETH_MAC_CFG_2K, priv->regbase + ETH_MAC_CFG_OFFSET);

		if (dev->mtu > 2000)
			setbitsl(ETH_MAC_CFG_JE, priv->regbase + ETH_MAC_CFG_OFFSET);
		else
			clrbitsl(ETH_MAC_CFG_JE, priv->regbase + ETH_MAC_CFG_OFFSET);

		ambeth_rx_rngmng_init(priv);
		ambeth_tx_rngmng_init(priv);

		ambhw_set_dma_desc(priv);
		ambhw_dma_rx_start(priv);
		ambhw_dma_tx_start(priv);
		ambhw_dma_int_enable(priv);
		ambhw_start_tx_rx(priv);

		spin_unlock_irqrestore(&priv->lock, flags);

		enable_irq(dev->irq);
		napi_enable(&priv->napi);
		netif_wake_queue(dev);
	}

	return 0;
}

static void ambeth_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct ambeth_info *lp;
	unsigned long flags;
	u32 irq_status;

	lp = (struct ambeth_info *)netdev_priv(ndev);

	dev_info(&lp->ndev->dev, "OOM Info:...\n");
	spin_lock_irqsave(&lp->lock, flags);
	irq_status = readl(lp->regbase + ETH_DMA_STATUS_OFFSET);
	ambeth_interrupt_tx(lp, irq_status | AMBETH_TXDMA_STATUS);
	ambhw_dump(lp);
	spin_unlock_irqrestore(&lp->lock, flags);

	if (netif_queue_stopped(ndev)) {
		printk("WARNING %s: restart TX process ...\n", __func__);
		netif_wake_queue(ndev);
	}
}

static struct net_device_stats *ambeth_get_stats(struct net_device *ndev)
{
	struct ambeth_info *lp = netdev_priv(ndev);

	return &lp->stats;
}

static void ambhw_dump_rx(struct ambeth_info *lp, u32 status, u32 entry)
{
	short pkt_len;
	struct sk_buff *skb;
	dma_addr_t mapping;

	pkt_len = ETH_RDES0_FL(status) - 4;

	if (unlikely(pkt_len > lp->bfsize)) {
		dev_warn(&lp->ndev->dev, "Bogus packet size %u.\n", pkt_len);
		pkt_len = lp->bfsize;
	}

	skb = lp->rx.rng_rx[entry].skb;
	mapping = lp->rx.rng_rx[entry].mapping;
	if (likely(skb && mapping)) {
		dma_unmap_single(lp->ndev->dev.parent, mapping,
			lp->bfsize, DMA_FROM_DEVICE);
		skb_put(skb, pkt_len);
		lp->rx.rng_rx[entry].skb = NULL;
		lp->rx.rng_rx[entry].mapping = 0;
		ambhw_dump_buffer(__func__, skb->data, skb->len);
		dev_kfree_skb(skb);
	}
}

static inline void ambeth_check_rdes0_status(struct ambeth_info *lp,
	u32 status, u32 entry)
{
	if (status & ETH_RDES0_DE) {
		lp->stats.rx_frame_errors++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"RX Error: Descriptor Error.\n");
	}
	if (status & ETH_RDES0_SAF) {
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"RX Error: Source Address Filter Fail.\n");
	}
	if (status & ETH_RDES0_LE) {
		lp->stats.rx_length_errors++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "RX Error: Length Error.\n");
	}
	if (status & ETH_RDES0_OE) {
		lp->stats.rx_over_errors++;
		if (netif_msg_rx_err(lp))
			dev_err(&lp->ndev->dev, "RX Error: Overflow Error.\n");
	}
	if (status & ETH_RDES0_VLAN) {
		if (netif_msg_drv(lp))
			dev_info(&lp->ndev->dev, "RX Info: VLAN.\n");
	}
	if (status & ETH_RDES0_IPC) {
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"RX Error: IPC Checksum/Giant Frame.\n");
	}
	if (status & ETH_RDES0_LC) {
		lp->stats.collisions++;
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev, "RX Error: Late Collision.\n");
	}
	if (status & ETH_RDES0_FT) {
		if (netif_msg_rx_err(lp))
			dev_info(&lp->ndev->dev,
			"RX Info: Ethernet-type frame.\n");
	}
	if (status & ETH_RDES0_RWT) {
		if (netif_msg_drv(lp))
			dev_err(&lp->ndev->dev,
			"RX Error: Watchdog Timeout.\n");
	}
	if (status & ETH_RDES0_RE) {
		lp->stats.rx_errors++;
		if (netif_msg_rx_err(lp))
			dev_err(&lp->ndev->dev, "RX Error: Receive.\n");
	}
	if (status & ETH_RDES0_DBE) {
		u32 val = readl(lp->regbase + ETH_MAC_CFG_OFFSET);
		if (val & ETH_MAC_CFG_PS) {
			lp->stats.rx_length_errors++;
			if (netif_msg_drv(lp))
				dev_err(&lp->ndev->dev,
				"RX Error: Dribble Bit.\n");
		}
	}
	if (status & ETH_RDES0_CE) {
		lp->stats.rx_crc_errors++;
		if (netif_msg_rx_err(lp)) {
			dev_err(&lp->ndev->dev, "RX Error: CRC.\n");
		}
	}
	if (status & ETH_RDES0_RX) {
		if (netif_msg_drv(lp)) {
			dev_err(&lp->ndev->dev,
			"RX Error: Rx MAC Address/Payload Checksum.\n");
			if (lp->dump_rx)
				ambhw_dump_rx(lp, status, entry);
		}
	}
}

static void ambeth_rx_vlan(struct ambeth_info *priv, struct sk_buff *skb)
{
	struct vlan_ethhdr *vlanhdr = (struct vlan_ethhdr *)skb->data;
	unsigned short vlanid, protocol;

	if (likely(vlanhdr->h_vlan_proto != htons(ETH_P_8021Q)))
		return ;

	vlanid = ntohs(vlanhdr->h_vlan_TCI);
	protocol = vlanhdr->h_vlan_proto;
	memmove(skb->data + VLAN_HLEN, vlanhdr, ETH_ALEN * 2);
	skb_pull(skb, VLAN_HLEN);
	__vlan_hwaccel_put_tag(skb, protocol, vlanid);
}

static inline void ambeth_napi_rx(struct ambeth_info *lp, u32 status, u32 entry, bool fragment)
{
	short pkt_len;
	struct sk_buff *skb;
	dma_addr_t mapping;

	pkt_len = ETH_RDES0_FL(status) - 4;

	if (unlikely(pkt_len > lp->bfsize)) {
		pkt_len = lp->bfsize;
		lp->ndev->stats.rx_length_errors++;
	}

	skb = lp->rx.rng_rx[entry].skb;
	ambeth_get_rx_hwtstamp(lp, skb, &lp->rx.desc_rx[entry]);

	mapping = lp->rx.rng_rx[entry].mapping;
	if (likely(skb && mapping)) {
		dma_unmap_single(lp->ndev->dev.parent, mapping,
			lp->bfsize, DMA_FROM_DEVICE);
		skb_put(skb, pkt_len);

		if (unlikely(lp->dump_rx))
			ambhw_dump_buffer(__func__, skb->data,  skb->len);

		ambeth_rx_vlan(lp, skb);

		/* skb_pull 14 bytes in eth_type_trans() */
		skb->protocol = eth_type_trans(skb, lp->ndev);
#if 0
		if (lp->ipc_rx) {
			if ((status & ETH_RDES0_COE_MASK) ==
				ETH_RDES0_COE_NOCHKERROR) {
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			} else {
				skb->ip_summed = CHECKSUM_NONE;
				if (netif_msg_rx_err(lp)) {
					dev_err(&lp->ndev->dev,
					"RX Error: RDES0_COE[0x%x].\n", status);
				}
			}
		}
#endif

		if (unlikely(lp->loopback && lp->selftest_callback)) {
			lp->selftest_callback(skb, lp->ndev);
			kfree_skb(skb);
		} else {
			/* Fragment packet is not implemented, drop it */
			if (unlikely(fragment) || unlikely(lp->dump_rx_free))
				kfree_skb(skb);
			else
				netif_receive_skb(skb);
		}

		lp->rx.rng_rx[entry].skb = NULL;
		lp->rx.rng_rx[entry].mapping = 0;
		lp->stats.rx_packets++;
		lp->stats.rx_bytes += pkt_len;
		lp->rx.cur_rx++;
	} else {
		if (netif_msg_drv(lp)) {
			dev_err(&lp->ndev->dev,
			"RX Error: %u skb[%p], map[0x%08X].\n",
			entry, skb, (unsigned int)mapping);
		}
	}
}

int ambeth_napi(struct napi_struct *napi, int budget)
{
	int rx_budget = budget;
	struct ambeth_info *lp;
	u32 entry;
	u32 status;
	unsigned long flags;
	unsigned int dirty_diff;
	bool fragment = false;

	lp = container_of(napi, struct ambeth_info, napi);
	dev_vdbg(&lp->ndev->dev, "cur_rx[%llu], dirty_rx[%llu]\n",
		lp->rx.cur_rx, lp->rx.dirty_rx);

	/*
	 * Continue, even though carrier is off if loopback is enabled.
	 */
	if (unlikely(!netif_carrier_ok(lp->ndev) && !lp->loopback))
		goto ambeth_poll_complete;

	while (rx_budget > 0) {
		entry = lp->rx.cur_rx % lp->rx_count;
		status = lp->rx.desc_rx[entry].status;
		if (status & ETH_RDES0_OWN)
			break;
		if (unlikely((status & (ETH_RDES0_FS | ETH_RDES0_LS)) !=
			(ETH_RDES0_FS | ETH_RDES0_LS))) {
			fragment = true;
		}

		if (likely((status & ETH_RDES0_ES) != ETH_RDES0_ES)) {
			ambeth_napi_rx(lp, status, entry, fragment);
		} else {
			spin_lock_irqsave(&lp->lock, flags);
			ambhw_dma_rx_stop(lp);
			spin_unlock_irqrestore(&lp->lock, flags);
			ambeth_check_rdes0_status(lp, status, entry);
			rx_budget += lp->rx_count;
			lp->rx.cur_rx++;
		}
		rx_budget--;

		dirty_diff = (lp->rx.cur_rx - lp->rx.dirty_rx);
		if (dirty_diff > (lp->rx_count / 4)) {
			ambeth_rx_rngmng_refill(lp);
		}
	}

ambeth_poll_complete:
	if (rx_budget > 0) {
		u32 val;

		ambeth_rx_rngmng_refill(lp);
		spin_lock_irqsave(&lp->lock, flags);
		napi_complete(&lp->napi);

		val = readl(lp->regbase + ETH_DMA_INTEN_OFFSET);
		val |= AMBETH_RXDMA_INTEN;
		writel(val, lp->regbase + ETH_DMA_INTEN_OFFSET);
		ambhw_dma_rx_start(lp);
		spin_unlock_irqrestore(&lp->lock, flags);
	}

	dev_vdbg(&lp->ndev->dev, "cur_rx[%llu], dirty_rx[%llu], rx_budget[%u]\n",
		lp->rx.cur_rx, lp->rx.dirty_rx, rx_budget);
	return (budget - rx_budget);
}

static inline u32 ambhw_hashtable_crc(unsigned char *mac)
{
	unsigned char tmpbuf[ETH_ALEN];
	int i;
	u32 crc;

	for (i = 0; i < ETH_ALEN; i++)
		tmpbuf[i] = bitrev8(mac[i]);
	crc = crc32_be(~0, tmpbuf, ETH_ALEN);

	return (crc ^ ~0);
}

static inline void ambhw_hashtable_get(struct net_device *ndev, u32 *hat)
{
	struct netdev_hw_addr *ha;
	unsigned int bitnr;
#if 0
	unsigned char test1[] = {0x1F,0x52,0x41,0x9C,0xB6,0xAF};
	unsigned char test2[] = {0xA0,0x0A,0x98,0x00,0x00,0x45};
	dev_info(&ndev->dev,
		"Test1: 0x%08X.\n", ambhw_hashtable_crc(test1));
	dev_info(&ndev->dev,
		"Test2: 0x%08X.\n", ambhw_hashtable_crc(test2));
#endif

	hat[0] = hat[1] = 0;
	netdev_for_each_mc_addr(ha, ndev) {
		if (!(ha->addr[0] & 1))
			continue;
		bitnr = ambhw_hashtable_crc(ha->addr);
		bitnr >>= 26;
		bitnr &= 0x3F;
		hat[bitnr >> 5] |= 1 << (bitnr & 31);
	}
}

static void ambeth_set_multicast_list(struct net_device *ndev)
{
	struct ambeth_info *lp;
	unsigned int mac_filter;
	u32 hat[2];
	unsigned long flags;

	lp = (struct ambeth_info *)netdev_priv(ndev);
	spin_lock_irqsave(&lp->lock, flags);

	mac_filter = readl(lp->regbase + ETH_MAC_FRAME_FILTER_OFFSET);
	hat[0] = 0;
	hat[1] = 0;

	if (ndev->flags & IFF_PROMISC) {
		mac_filter |= ETH_MAC_FRAME_FILTER_PR;
	} else if (ndev->flags & (~IFF_PROMISC)) {
		mac_filter &= ~ETH_MAC_FRAME_FILTER_PR;
	}

	if (ndev->flags & IFF_ALLMULTI) {
		hat[0] = 0xFFFFFFFF;
		hat[1] = 0xFFFFFFFF;
		mac_filter |= ETH_MAC_FRAME_FILTER_PM;
	} else if (!netdev_mc_empty(ndev)) {
		ambhw_hashtable_get(ndev, hat);
		mac_filter &= ~ETH_MAC_FRAME_FILTER_PM;
		mac_filter |= ETH_MAC_FRAME_FILTER_HMC;
	} else if (ndev->flags & (~IFF_ALLMULTI)) {
		mac_filter &= ~ETH_MAC_FRAME_FILTER_PM;
		mac_filter |= ETH_MAC_FRAME_FILTER_HMC;
	}

	if (netif_msg_hw(lp)) {
		dev_info(&lp->ndev->dev, "MC Info: flags 0x%x.\n", ndev->flags);
		dev_info(&lp->ndev->dev, "MC Info: mc_count 0x%x.\n",
			netdev_mc_count(ndev));
		dev_info(&lp->ndev->dev, "MC Info: mac_filter 0x%x.\n",
			mac_filter);
		dev_info(&lp->ndev->dev, "MC Info: hat[0x%x:0x%x].\n",
			hat[1], hat[0]);
	}

	writel(hat[1], lp->regbase + ETH_MAC_HASH_HI_OFFSET);
	writel(hat[0], lp->regbase + ETH_MAC_HASH_LO_OFFSET);
	writel(mac_filter, lp->regbase + ETH_MAC_FRAME_FILTER_OFFSET);

	spin_unlock_irqrestore(&lp->lock, flags);
}

static int ambeth_set_mac_address(struct net_device *ndev, void *addr)
{
	struct ambeth_info *lp = (struct ambeth_info *)netdev_priv(ndev);
	struct sockaddr *saddr = addr;
	unsigned long flags;

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	spin_lock_irqsave(&lp->lock, flags);

	if (netif_running(ndev)) {
		spin_unlock_irqrestore(&lp->lock, flags);
		return -EBUSY;
	}

	dev_dbg(&lp->ndev->dev, "MAC address[%pM].\n", saddr->sa_data);

	memcpy(ndev->dev_addr, saddr->sa_data, ndev->addr_len);
	ambhw_set_hwaddr(lp, ndev->dev_addr);
	ambhw_get_hwaddr(lp, ndev->dev_addr);

	spin_unlock_irqrestore(&lp->lock, flags);

	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void ambeth_poll_controller(struct net_device *ndev)
{
	ambeth_interrupt(ndev->irq, ndev);
}
#endif

static int ambeth_ioctl(struct net_device *ndev, struct ifreq *ifr, int cmd)
{
	int rval = 0;
	struct ambeth_info *lp = netdev_priv(ndev);

	if (!netif_running(ndev))
		return -EINVAL;

	switch(cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		if (!lp->phydev)
			return -ENODEV;
		rval = phy_mii_ioctl(lp->phydev, ifr, cmd);
		break;
	case SIOCSHWTSTAMP:
		rval = ambeth_set_hwtstamp(ndev, ifr);
		break;
	default:
		rval = -EOPNOTSUPP;
	}

	return rval;
}

static netdev_features_t ambeth_fix_features(struct net_device *dev,
					     netdev_features_t features)
{
	/* bugged JUMBO frame: disable Tx COE. */
	if (dev->mtu > ETH_DATA_LEN)
		features &= ~NETIF_F_CSUM_MASK;

	return features;
}

static int ambeth_set_features(struct net_device *netdev,
			       netdev_features_t features)
{
	return 0;
}

static const struct net_device_ops ambeth_netdev_ops = {
	.ndo_open		= ambeth_open,
	.ndo_stop		= ambeth_stop,
	.ndo_start_xmit		= ambeth_hard_start_xmit,
	.ndo_set_rx_mode	= ambeth_set_multicast_list,
	.ndo_set_mac_address 	= ambeth_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= ambeth_ioctl,
	.ndo_change_mtu		= ambeth_change_mtu,
	.ndo_fix_features	= ambeth_fix_features,
	.ndo_set_features	= ambeth_set_features,
	.ndo_tx_timeout		= ambeth_timeout,
	.ndo_get_stats		= ambeth_get_stats,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= ambeth_poll_controller,
#endif
};

/* ==========================================================================*/
static int ambeth_get_dump_flag(struct net_device *ndev,
	struct ethtool_dump *ed)
{
	ed->len = (AMBETH_PHY_REG_SIZE * sizeof(u16));
	ed->flag = 0;
	pr_debug("%s: cmd[0x%08X], version[0x%08X], "
		"flag[0x%08X], len[0x%08X]\n",
		__func__, ed->cmd, ed->version,
		ed->flag, ed->len);

	return 0;
}

static int ambeth_get_dump_data(struct net_device *ndev,
	struct ethtool_dump *ed, void *pdata)
{
	int i;
	u16 *regbuf;
	struct ambeth_info *lp = netdev_priv(ndev);
	struct phy_device	*phydev = lp->phydev;

	pr_debug("%s: cmd[0x%08X], version[0x%08X], "
		"flag[0x%08X], len[0x%08X]\n",
		__func__, ed->cmd, ed->version,
		ed->flag, ed->len);

	if (!lp->phy_enabled) {
		return -EINVAL;
	}
	regbuf = (u16 *)pdata;
	for (i = 0; i < (ed->len / 2); i++) {
		regbuf[i] = mdiobus_read(phydev->mdio.bus,
				phydev->mdio.addr, i);
	}

	return 0;
}

static int ambeth_set_dump(struct net_device *ndev, struct ethtool_dump *ed)
{
	u16 dbg_address, dbg_value;
	struct ambeth_info *lp = netdev_priv(ndev);
	struct phy_device	*phydev = lp->phydev;

	pr_debug("%s: cmd[0x%08X], version[0x%08X], "
		"flag[0x%08X], len[0x%08X]\n",
		__func__, ed->cmd, ed->version,
		ed->flag, ed->len);

	if (!lp->phy_enabled) {
		return -EINVAL;
	}
	dbg_address = ((ed->flag & 0xFFFF0000) >> 16);
	dbg_value = (ed->flag & 0x0000FFFF);
	mdiobus_write(phydev->mdio.bus, phydev->mdio.addr,
		dbg_address, dbg_value);

	return 0;
}

static int ambeth_ethtools_get_regs_len(struct net_device *ndev)
{
	return sizeof(u16) * AMBETH_PHY_REG_SIZE;
}

static void ambeth_ethtools_get_regs(struct net_device *ndev,
		struct ethtool_regs *regs, void *rdata)
{
	u16 *data = (u16 *) rdata;
	size_t len = sizeof(u16) * AMBETH_PHY_REG_SIZE;
	struct ambeth_info *lp = netdev_priv(ndev);
	struct phy_device *phydev = lp->phydev;
	int i;

	regs->version = 0;
	regs->len = len;

	memset(data, 0, len);
	for (i = 0;  i < AMBETH_PHY_REG_SIZE; i++)
		data[i] = phy_read(phydev, i);

}

static u32 ambeth_get_msglevel(struct net_device *ndev)
{
	struct ambeth_info *lp;

	lp = (struct ambeth_info *)netdev_priv(ndev);

	return lp->msg_enable;
}

static void ambeth_set_msglevel(struct net_device *ndev, u32 value)
{
	struct ambeth_info *lp;

	lp = (struct ambeth_info *)netdev_priv(ndev);

	lp->msg_enable = value;
}

static void ambeth_get_pauseparam(struct net_device *ndev,
				  struct ethtool_pauseparam *pause)
{
	struct ambeth_info *lp;
	u32 flow_ctr;

	lp = (struct ambeth_info *)netdev_priv(ndev);
	flow_ctr = lp->flow_ctr;

	pause->autoneg = (flow_ctr & AMBETH_FC_AUTONEG) ?
				AUTONEG_ENABLE : AUTONEG_DISABLE;

	pause->rx_pause = (flow_ctr & AMBETH_FC_RX) ? 1 : 0;
	pause->tx_pause = (flow_ctr & AMBETH_FC_TX) ? 1 : 0;
}

static int ambeth_set_pauseparam(struct net_device *ndev,
				 struct ethtool_pauseparam *pause)
{
	struct ambeth_info *lp = netdev_priv(ndev);
	u32 flow_ctr;
	int ret_val = 0;

	/*
	 * Symmeteric pause can respond to recieved pause frames, and
	 * send pause frames to the link partner.
	 *
	 * Asymmetric pause can send pause frames, but can't respond to
	 * pause frames from the link partner.
	 *
	 * Autoneg only advertises and reports the 'cap (or will)' of
	 * the link partner. The final resolution still has to be done in
	 * MAC / Driver.
	 *
	 * Since our MAC can support both directions independently, we
	 * advertise our 'cap' to the link partner based on the
	 * pauseparam specified by the user (ethtool). And take the
	 * 'cap' of the link partner reported into consideration for
	 * makeing the final resolution.
	 */

	flow_ctr = lp->flow_ctr;

	if (pause->autoneg)
		flow_ctr |= AMBETH_FC_AUTONEG;
	else
		flow_ctr &= ~AMBETH_FC_AUTONEG;

	if (pause->rx_pause)
		flow_ctr |= AMBETH_FC_RX;
	else
		flow_ctr &= ~AMBETH_FC_RX;

	if (pause->tx_pause)
		flow_ctr |= AMBETH_FC_TX;
	else
		flow_ctr &= ~AMBETH_FC_TX;

	lp->flow_ctr = flow_ctr;

	if(lp->flow_ctr & (AMBETH_FC_TX | AMBETH_FC_RX))
		lp->phy_supported |= SUPPORTED_Pause;
	else
		lp->phy_supported &= ~SUPPORTED_Pause;

	ambeth_fc_config(lp);

	if (pause->autoneg && lp->phydev->autoneg) {

		ret_val = phy_start_aneg(lp->phydev);
		if (ret_val)
			goto done;
	}
	else {
		ambeth_fc_resolve(lp);
	}
done:
	return ret_val;
}

static const struct ethtool_ops ambeth_ethtool_ops = {
	.get_link		= ethtool_op_get_link,
	.get_dump_flag		= ambeth_get_dump_flag,
	.get_dump_data		= ambeth_get_dump_data,
	.set_dump		= ambeth_set_dump,
	.get_regs_len		= ambeth_ethtools_get_regs_len,
	.get_regs		= ambeth_ethtools_get_regs,
	.get_msglevel		= ambeth_get_msglevel,
	.set_msglevel		= ambeth_set_msglevel,
	.get_pauseparam		= ambeth_get_pauseparam,
	.set_pauseparam		= ambeth_set_pauseparam,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.get_ts_info		= ambeth_get_ts_info,
	.self_test		= ambeth_self_test,
	.get_sset_count		= ambeth_get_sset_count,
	.get_strings		= ambeth_get_strings,
	.get_ethtool_stats	= ambeth_get_ethtool_stats,
};

/* ==========================================================================*/

static const struct ambeth_soc_data ambeth_soc_data_v0 = {
	.intf_type_mask		= BIT(0) | BIT(20),
	.intf_type_rmii		= BIT(0) | BIT(20),
	.intf_type_rgmii	= BIT(0),
};

static const struct ambeth_soc_data ambeth_soc_data_v1 = {
	.intf_type_mask		= ENET_SEL | ENET_PHY_INTF_SEL_RMII,
	.intf_type_rmii		= ENET_SEL | ENET_PHY_INTF_SEL_RMII,
	.intf_type_rgmii	= ENET_SEL,
};

static int gmac_set_clock(void *priv)
{
	struct ambeth_info *gmac_priv = priv;

	if (gmac_priv->tx_clk_invert) {
		if (gmac_priv->id == 0)
			regmap_set_bits(gmac_priv->reg_scr,
					gmac_priv->soc_offsets->ahbsp_ctrl,
					BIT(31));
		else
			regmap_set_bits(gmac_priv->reg_scr,
					gmac_priv->soc_offsets->ahbsp_ctrl,
					BIT(28));
	}

	if (gmac_priv->rx_clk_invert) {
		if (gmac_priv->id == 0)
			regmap_set_bits(gmac_priv->reg_scr,
					gmac_priv->soc_offsets->ahbsp_ctrl,
					BIT(0));
		else
			regmap_set_bits(gmac_priv->reg_scr,
					gmac_priv->soc_offsets->ahbsp_ctrl,
					BIT(11));
	}

	if (gmac_priv->second_ref_clk_50mhz)
		regmap_set_bits(gmac_priv->reg_scr,
				gmac_priv->soc_offsets->ahbsp_ctrl,
				BIT(23));
	return 0;
}

/* for: s5l cv2 cv22 cv25 */
static int gmac_set_mode_v0(void *priv)
{
	unsigned int value, hw_intf;
	struct ambeth_info *gmac_priv = priv;

	regmap_read(gmac_priv->reg_rct, SYS_CONFIG_OFFSET, &value);
	if (!(value & POC_ETH_IS_ENABLED)) {
		dev_warn(gmac_priv->dev, "Ethernet is not enabled by POC, force enable it!\n");
		regmap_update_bits(gmac_priv->reg_rct, SYS_CONFIG_OFFSET, POC_ETH_IS_ENABLED, POC_ETH_IS_ENABLED);
	}

	switch(gmac_priv->intf_type) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		/* Mode matched */
		if (!(value & POC_ETH_RMII_MODE))
			break;
		dev_warn(gmac_priv->dev, "Mismatch mode by POC, force switch to RGMII.\n");
		regmap_update_bits(gmac_priv->reg_rct, SYS_CONFIG_OFFSET, POC_ETH_RMII_MODE, 0);

		/* reset MAC if hardware mode is changed */
		if (ambhw_dma_reset(gmac_priv))
			return -EIO;

		/* sanity check */
		hw_intf = readl(gmac_priv->regbase + ETH_DMA_HWFEA_OFFSET);
		hw_intf &= ETH_DMA_HWFEA_ACTPHYIF_MASK;
		if (hw_intf != ETH_DMA_HWFEA_ACTPHYIF_RGMII) {
			dev_err(gmac_priv->dev, "Sanity check error: 0x%x:0x%x\n", hw_intf, gmac_priv->intf_type);
			return -EIO;
		}

		break;
	case PHY_INTERFACE_MODE_RMII:
		if (!(value & POC_ETH_RMII_MODE)) {
			dev_warn(gmac_priv->dev, "Mismatched mode by POC, force switch to RMII.\n");
			regmap_update_bits(gmac_priv->reg_rct, SYS_CONFIG_OFFSET, POC_ETH_RMII_MODE, POC_ETH_RMII_MODE);

			/* reset MAC if hardware mode is changed */
			if (ambhw_dma_reset(gmac_priv))
				return -EIO;

			/* sanity check */
			hw_intf = readl(gmac_priv->regbase + ETH_DMA_HWFEA_OFFSET);
			hw_intf &= ETH_DMA_HWFEA_ACTPHYIF_MASK;
			if (hw_intf != ETH_DMA_HWFEA_ACTPHYIF_RMII) {
				dev_err(gmac_priv->dev, "Sanity check error: 0x%x:0x%x\n", hw_intf, gmac_priv->intf_type);
				return -EIO;
			}
		}
		break;
	/* MII interface is not supported on S5 and later SoCs */
	default:
		dev_err(gmac_priv->dev, "Not supported PHY mode %d\n", gmac_priv->intf_type);
		return -ENOTSUPP;
	}

	return 0;
}

/* For: s6lm cv2fs cv28m */
static int gmac_set_mode_v1(void *priv)
{
	struct ambeth_info *gmac_priv = priv;
	unsigned int value, mask;

	/* Enable ENET and set clock Source as clk_rx */
	value = ENET_SEL;
	mask = ENET_SEL | ENET_PHY_INTF_SEL_RMII;

	switch (gmac_priv->intf_type) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		dev_info(gmac_priv->dev, "select RGMII mode\n");
		break;
	case PHY_INTERFACE_MODE_RMII:
		dev_info(gmac_priv->dev, "select RMII mode\n");
		value |= ENET_PHY_INTF_SEL_RMII;
		break;
	default:
		dev_info(gmac_priv->dev, "Unsupported mode\n");
		break;
	};

	/* Enable ENET and select PHY interface */
	mask = mask << (gmac_priv->id * 4);
	value = value << (gmac_priv->id * 4);
	regmap_update_bits(gmac_priv->reg_rct, 0x79C, mask, value);

	return 0;
}

/* for cv5x */
static int gmac_set_mode_v2(void *priv)
{
	struct ambeth_info *gmac_priv = priv;
	unsigned int value, mask = 0xF;

	/* Enable ENET and set clock Source as clk_rx */
	value = BIT(0);

	switch (gmac_priv->intf_type) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		dev_info(gmac_priv->dev, "select RGMII mode\n");
		break;
	case PHY_INTERFACE_MODE_RMII:
		dev_info(gmac_priv->dev, "select RMII mode\n");
		value |= BIT(1);
		break;
	default:
		dev_info(gmac_priv->dev, "Unsupported mode\n");
		break;
	};

	/* Enable ENET and select PHY interface */
	mask = mask << (gmac_priv->id * 4);
	value = value << (gmac_priv->id * 4);
	regmap_update_bits(gmac_priv->reg_scr, 0x10C, mask, value);

	return 0;
}

static const struct ambeth_gmac_ops gmac_ops_v0 = {
	.set_mode = gmac_set_mode_v0,
	.set_clock = gmac_set_clock,
};

static const struct ambeth_gmac_ops gmac_ops_v1 = {
	.set_mode = gmac_set_mode_v1,
	.set_clock = gmac_set_clock,
};

static const struct ambeth_gmac_ops gmac_ops_v2 = {
	.set_mode = gmac_set_mode_v2,
	.set_clock = gmac_set_clock,
};

// TODO: s5l doesn't have ahbsp_gmii_data and ahbsp_gmii_addr
static const struct ambeth_gmac_offsets gmac_offsets_v0 = {
	.ahbsp_ctrl = 0x0c,
	.ahbsp_gmii_data = 0x6c,
	.ahbsp_gmii_addr = 0x70,
};

static const struct ambeth_gmac_offsets gmac_offsets_v1 = {
	.ahbsp_ctrl = 0x60,
	.ahbsp_gmii_data = 0xa0,
	.ahbsp_gmii_addr = 0xa4,
};

static const struct ambeth_gmac_soc_data gmac_soc_data_v0 = {
	.ops = &gmac_ops_v0,
	.offsets = &gmac_offsets_v0,
};

static const struct ambeth_gmac_soc_data gmac_soc_data_v1 = {
	.ops = &gmac_ops_v0,
	.offsets = &gmac_offsets_v1,
};

static const struct ambeth_gmac_soc_data gmac_soc_data_v2 = {
	.ops = &gmac_ops_v1,
	.offsets = &gmac_offsets_v1,
};

static const struct ambeth_gmac_soc_data gmac_soc_data_v3 = {
	.ops = &gmac_ops_v2,
	.offsets = &gmac_offsets_v1,
};

static const struct soc_device_attribute ambeth_socinfo[] = {
	{ .soc_id = "s5l",   .data = &gmac_soc_data_v0 },
	{ .soc_id = "cv2",   .data = &gmac_soc_data_v0 },
	{ .soc_id = "cv22",  .data = &gmac_soc_data_v1 },
	{ .soc_id = "cv25",  .data = &gmac_soc_data_v1 },
	{ .soc_id = "s6lm",  .data = &gmac_soc_data_v2 },
	{ .soc_id = "cv28m", .data = &gmac_soc_data_v2 },
	{ .soc_id = "cv2fs", .data = &gmac_soc_data_v2 },
	{ .soc_id = "cv5",   .data = &gmac_soc_data_v3 },
	{ /* sentinel */ }
};

static int ambeth_of_parse(struct device_node *np, struct ambeth_info *lp)
{
	struct device *dev = lp->dev;
	struct device_node *phy_np;
	const struct soc_device_attribute *soc;
	enum of_gpio_flags flags;
	int ret_val, val;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(phy_supported) = { 0, };
	const struct ambeth_gmac_soc_data *soc_data;

	lp->id = of_alias_get_id(np, "ethernet");

	for_each_child_of_node(np, phy_np) {
		if (!phy_np->name || of_node_cmp(phy_np->name, "phy"))
			continue;

		lp->pwr_gpio = of_get_named_gpio_flags(phy_np, "pwr-gpios", 0, &flags);
		lp->pwr_gpio_active = !!(flags & OF_GPIO_ACTIVE_LOW);

		lp->rst_gpio = of_get_named_gpio_flags(phy_np, "rst-gpios", 0, &flags);
		lp->rst_gpio_active = !!(flags & OF_GPIO_ACTIVE_LOW);

		ret_val = of_property_read_u32(phy_np, "rst-gpio-delay", &lp->rst_gpio_delay);
		if (ret_val < 0)
			lp->rst_gpio_delay = 20;

		if (gpio_is_valid(lp->pwr_gpio)) {
			ret_val = devm_gpio_request(dev, lp->pwr_gpio, "phy power");
			if (ret_val < 0) {
				dev_err(dev, "Failed to request pwr-gpios!\n");
				return -EBUSY;
			}
		}

		if (gpio_is_valid(lp->rst_gpio)) {
			ret_val = devm_gpio_request(dev, lp->rst_gpio, "phy reset");
			if (ret_val < 0) {
				dev_err(dev, "Failed to request rst-gpios!\n");
				return -EBUSY;
			}
		}

		/* request gpio for PHY power control */
		ambeth_phy_init(lp);
	}

	ret_val = of_property_read_u32(np, "amb,fixed-speed", &lp->fixed_speed);
	if (ret_val < 0)
		lp->fixed_speed = SPEED_UNKNOWN;

	ret_val = of_get_phy_mode(np, &lp->intf_type);
	if (ret_val < 0) {
		dev_err(dev, "get phy interface type failed!\n");
		return -ENODEV;
	}

	soc = soc_device_match(ambeth_socinfo);
	if (!soc) {
		dev_err(dev, "Unknow SOC !!!\n");
		return -ENODEV;
	}
	soc_data = soc->data;
	lp->soc_ops = soc_data->ops;
	lp->soc_offsets = soc_data->offsets;

	phylink_set(phy_supported, 10baseT_Half);
	phylink_set(phy_supported, 10baseT_Full);
	phylink_set(phy_supported, 100baseT_Half);
	phylink_set(phy_supported, 100baseT_Full);
	phylink_set(phy_supported, Autoneg);
	phylink_set(phy_supported, Pause);
	phylink_set(phy_supported, Asym_Pause);

	if (lp->intf_type >= PHY_INTERFACE_MODE_RGMII &&
		lp->intf_type <= PHY_INTERFACE_MODE_RGMII_TXID) {
		phylink_set(phy_supported, 1000baseT_Half);
		phylink_set(phy_supported, 1000baseT_Full);
	}

	phylink_set(phy_supported, TP);
	phylink_set(phy_supported, MII);

	ethtool_convert_link_mode_to_legacy_u32(&lp->phy_supported, phy_supported);

	ret_val = of_property_read_u32(np, "amb,tx-ring-size", &lp->tx_count);
	if (ret_val < 0 || lp->tx_count < AMBETH_TX_RNG_MIN)
		lp->tx_count = AMBETH_TX_RNG_MIN;

	ret_val = of_property_read_u32(np, "amb,rx-ring-size", &lp->rx_count);
	if (ret_val < 0 || lp->rx_count < AMBETH_RX_RNG_MIN)
		lp->rx_count = AMBETH_RX_RNG_MIN;

	ret_val = of_property_read_u32(np, "amb,ahb-12mhz-div", &val);
	if (ret_val < 0 || val > 16)
		lp->ahb_mdio_clk_div = 0;
	else
		lp->ahb_mdio_clk_div = val;

	lp->tx_irq_low = ((lp->tx_count * 1) / 4);
	lp->tx_irq_high = ((lp->tx_count * 3) / 4);

	lp->ipc_tx = of_property_read_bool(np, "amb,ipc-tx");
	lp->ipc_rx = of_property_read_bool(np, "amb,ipc-rx");
	lp->dump_tx = of_property_read_bool(np, "amb,dump-tx");
	lp->dump_rx = of_property_read_bool(np, "amb,dump-rx");
	lp->dump_rx_free = of_property_read_bool(np, "amb,dump-rx-free");
	lp->dump_rx_all = of_property_read_bool(np, "amb,dump-rx-all");
	lp->mdio_gpio = !!of_find_property(np, "amb,mdio-gpio", NULL);

	lp->tx_clk_invert = !!of_find_property(np, "amb,tx-clk-invert", NULL);
	lp->rx_clk_invert = !!of_find_property(np, "amb,rx-clk-invert", NULL);
	lp->second_ref_clk_50mhz = !!of_find_property(np, "amb,2nd-ref-clk-50mhz", NULL);

	return 0;
}

static int ambeth_drv_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node, *mdio_np = NULL;
	struct net_device *ndev;
	struct mii_bus *bus;
	struct ambeth_info *lp;
	struct resource *res;
	int ret_val = 0;

	ndev = alloc_etherdev(sizeof(struct ambeth_info));
	if (ndev == NULL) {
		dev_err(&pdev->dev, "alloc_etherdev fail.\n");
		return -ENOMEM;
	}
	lp = netdev_priv(ndev);
	lp->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "No mem resource for fio_reg!\n");
		ret_val = -ENXIO;
		goto ambeth_drv_probe_free_netdev;
	}

	lp->regbase = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!lp->regbase) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		ret_val = -ENOMEM;
		goto ambeth_drv_probe_free_netdev;
	}


	ndev->irq = platform_get_irq(pdev, 0);
	if (ndev->irq < 0) {
		dev_err(&pdev->dev, "no irq for ethernet!\n");
		ret_val = -ENODEV;
		goto ambeth_drv_probe_free_netdev;
	}

	lp->reg_rct = syscon_regmap_lookup_by_phandle(np, "amb,rct-regmap");
	if (IS_ERR(lp->reg_rct)) {
		dev_err(&pdev->dev, "no rct regmap!\n");
		ret_val = PTR_ERR(lp->reg_rct);
		goto ambeth_drv_probe_free_netdev;
	}

	lp->reg_scr = syscon_regmap_lookup_by_phandle(np, "amb,scr-regmap");
	if (IS_ERR(lp->reg_scr)) {
		dev_err(&pdev->dev, "no scr regmap!\n");
		ret_val = PTR_ERR(lp->reg_scr);
		goto ambeth_drv_probe_free_netdev;
	}

	SET_NETDEV_DEV(ndev, &pdev->dev);
	ndev->dev.dma_mask = pdev->dev.dma_mask;
	ndev->dev.coherent_dma_mask = pdev->dev.coherent_dma_mask;

	init_completion(&lp->comp);
	spin_lock_init(&lp->lock);
	lp->ndev = ndev;
	lp->msg_enable = netif_msg_init(msg_level, NETIF_MSG_DRV);
	lp->bfsize = AMBETH_DEFAULT_BFSIZE;

	ret_val = ambeth_of_parse(np, lp);
	if (ret_val < 0)
		goto ambeth_drv_probe_free_netdev;

	if (lp->soc_ops->set_mode)
		lp->soc_ops->set_mode(lp);

	if (lp->soc_ops->set_clock)
		lp->soc_ops->set_clock(lp);

	ndev->hw_features = NETIF_F_SG | NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM |
			    NETIF_F_RXCSUM;

	if(lp->mdio_gpio){
		mdio_np = of_find_compatible_node(NULL, NULL, "virtual,mdio-gpio");

		if(mdio_np == NULL) {
			dev_err(&pdev->dev, "Failed to get mdio_gpio device node\n");
			goto ambeth_drv_probe_free_netdev;
		}

		lp->phydev = of_phy_find_device(mdio_np->child);

		if(!lp->phydev) {
			dev_err(&pdev->dev, "Failed to get phydev from mdio_gpio device node\n");
			goto ambeth_drv_probe_free_netdev;
		}

		lp->new_bus = *lp->phydev->mdio.bus;
	} else {
		bus = devm_mdiobus_alloc_size(&pdev->dev, sizeof(struct ambeth_info));
		if (!bus) {
			ret_val = -ENOMEM;
			goto ambeth_drv_probe_free_netdev;
		}
		memcpy(&lp->new_bus, bus, sizeof(struct mii_bus));

		lp->new_bus.name = "Ambarella MDIO Bus";
		lp->new_bus.read = &ambhw_mdio_read;
		lp->new_bus.write = &ambhw_mdio_write;
		lp->new_bus.reset = &ambhw_mdio_reset;
		snprintf(lp->new_bus.id, MII_BUS_ID_SIZE, "%s", pdev->name);
		lp->new_bus.priv = lp;
		lp->new_bus.parent = &pdev->dev;
		lp->new_bus.state = MDIOBUS_ALLOCATED;


		if (lp->ahb_mdio_clk_div) {
			lp->new_bus.read = &ambahb_mdio_read;
			lp->new_bus.write = &ambahb_mdio_write;
		}

		ret_val = of_mdiobus_register(&lp->new_bus, pdev->dev.of_node);
		if (ret_val < 0) {
			dev_err(&pdev->dev, "of_mdiobus_register fail%d.\n", ret_val);
			goto ambeth_drv_probe_free_netdev;
		}

		lp->phydev = phy_find_first(&lp->new_bus);
		if (lp->phydev == NULL) {
			dev_err(&pdev->dev, "No PHY device.\n");
			ret_val = -ENODEV;
			goto ambeth_drv_probe_remove_mdio;
		}
	}
	lp->phydev->mac_managed_pm = 1;

	if (netif_msg_drv(lp)) {
		dev_info(&pdev->dev, "Ethernet PHY[%d]: 0x%08x, %s!\n",
				lp->phydev->mdio.addr,
				lp->phydev->phy_id,
				lp->phydev->drv ? lp->phydev->drv->name : "Fixed");
	}
	ether_setup(ndev);
	ndev->netdev_ops = &ambeth_netdev_ops;
	ndev->watchdog_timeo = AMBETH_TX_WATCHDOG;
	netif_napi_add(ndev, &lp->napi, ambeth_napi, AMBETH_NAPI_WEIGHT);

	ret_val = of_get_mac_address(pdev->dev.of_node, ndev->dev_addr);
	if (ret_val) {
		dev_warn(&pdev->dev, "using random Ethernet MAC\n");
		eth_hw_addr_random(ndev);
	}

	ambhw_disable(lp);

	ndev->ethtool_ops = &ambeth_ethtool_ops;

	/* In theory the MAC hardware can support frame size which can up to 9018,
	 * however, the software will be more complex based on the previous
	 * framwork. So MTU is set as 8000 which is not bigger than buffer size. */
	ndev->max_mtu = 8000;

	ret_val = register_netdev(ndev);
	if (ret_val) {
		dev_err(&pdev->dev, " register_netdev fail%d.\n", ret_val);
		goto ambeth_drv_probe_netif_napi_del;
	}

	platform_set_drvdata(pdev, ndev);
	ambeth_ptp_init(pdev);

	dev_notice(&pdev->dev, "MAC Address[%pM].\n", ndev->dev_addr);
	return 0;

ambeth_drv_probe_netif_napi_del:
	netif_napi_del(&lp->napi);
ambeth_drv_probe_remove_mdio:
	mdiobus_unregister(&lp->new_bus);

ambeth_drv_probe_free_netdev:
	free_netdev(ndev);
	return ret_val;
}

static int ambeth_drv_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct ambeth_info *lp = netdev_priv(ndev);

	ambeth_ptp_exit(lp);
	unregister_netdev(ndev);
	netif_napi_del(&lp->napi);
	mdiobus_unregister(&lp->new_bus);
	kfree(lp->new_bus.irq);
	platform_set_drvdata(pdev, NULL);
	free_netdev(ndev);
	dev_notice(&pdev->dev, "Removed.\n");

	return 0;
}

#ifdef CONFIG_PM
static int ambeth_drv_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct ambeth_info *lp = netdev_priv(ndev);
	int ret_val = 0;
	unsigned long flags;

	if (!netif_running(ndev))
		goto ambeth_drv_suspend_exit;

	if (lp->phydev) {
		phy_stop(lp->phydev);
		phy_disconnect(lp->phydev);
	}

	napi_disable(&lp->napi);
	netif_device_detach(ndev);
	disable_irq(ndev->irq);

	ambeth_phy_stop(lp);

	spin_lock_irqsave(&lp->lock, flags);
	ambhw_disable(lp);
	spin_unlock_irqrestore(&lp->lock, flags);

	ambeth_phy_deinit(lp);
ambeth_drv_suspend_exit:
	dev_dbg(&pdev->dev, "%s exit with %d @ %d\n",
		__func__, ret_val, state.event);

	return ret_val;
}

static int ambeth_drv_resume(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct ambeth_info *lp = netdev_priv(ndev);
	int ret_val = 0;
	unsigned long flags;

	if (!netif_running(ndev))
		goto ambeth_drv_resume_exit;

	if (lp->soc_ops->set_mode)
		lp->soc_ops->set_mode(lp);

	if (lp->soc_ops->set_clock)
		lp->soc_ops->set_clock(lp);

	ambeth_phy_init(lp);

	spin_lock_irqsave(&lp->lock, flags);
	ret_val = ambhw_enable(lp);
	ambhw_set_link_mode_speed(lp);
	ambeth_rx_rngmng_init(lp);
	ambeth_tx_rngmng_init(lp);
	ambhw_set_dma_desc(lp);
	ambhw_dma_rx_start(lp);
	ambhw_dma_tx_start(lp);
	ambhw_dma_int_enable(lp);
	spin_unlock_irqrestore(&lp->lock, flags);

	if (ret_val) {
		dev_err(&pdev->dev, "ambhw_enable.\n");
	} else {
		ambeth_set_multicast_list(ndev);
		netif_carrier_off(ndev);
		ret_val = ambeth_phy_start(lp);
		if (ret_val)
			goto ambeth_drv_resume_exit;
		enable_irq(ndev->irq);
		netif_device_attach(ndev);
		napi_enable(&lp->napi);

		if (lp->phydev)
			phy_start(lp->phydev);
	}


ambeth_drv_resume_exit:
	dev_dbg(&pdev->dev, "%s exit with %d\n", __func__, ret_val);
	return ret_val;
}

static int ambarella_eth_suspend(struct device *dev)
{
	int retval;
	struct platform_device *pdev = to_platform_device(dev);

	retval = ambeth_drv_suspend(pdev, PMSG_SUSPEND);
	return retval;
}

static int ambarella_eth_resume(struct device *dev)
{
	int retval;
	struct platform_device *pdev = to_platform_device(dev);

	retval = ambeth_drv_resume(pdev);
	return retval;
}

static const struct dev_pm_ops ambarella_ethdev_pm_ops = {
	.suspend = ambarella_eth_suspend,
	.resume = ambarella_eth_resume,
};

#endif

static const struct of_device_id ambarella_eth_dt_ids[] = {
	{ .compatible = "ambarella,eth" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ambarella_eth_dt_ids);

static struct platform_driver ambeth_driver = {
	.probe		= ambeth_drv_probe,
	.remove		= ambeth_drv_remove,
	.driver = {
		.name	= "ambarella-eth",
		.owner	= THIS_MODULE,
		.of_match_table	= ambarella_eth_dt_ids,
#ifdef CONFIG_PM
		.pm	= &ambarella_ethdev_pm_ops,
#endif
	},
};

module_platform_driver(ambeth_driver);

MODULE_DESCRIPTION("Ambarella Media Processor Ethernet Driver");
MODULE_AUTHOR("Anthony Ginger, <hfjiang@ambarella.com>");
MODULE_LICENSE("GPL");

