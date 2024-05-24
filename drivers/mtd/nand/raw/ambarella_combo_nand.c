// SPDX-License-Identifier: GPL-2.0
/*
 * Nand driver for Ambarella SoCs
 *
 * History:
 *	2017/05/13 - [Cao Rongrong] created file
 *
 * Copyright (C) 2016-2048, Ambarella, Inc.
 */

#include "ambarella_combo_nand.h"
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/bitrev.h>
#include <linux/bch.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/sys_soc.h>
#include <linux/regmap.h>
#include <linux/pinctrl/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/nand-ecc-sw-hamming.h>
#include <linux/mtd/partitions.h>
#include <linux/pinctrl/pinctrl.h>
#include <asm-generic/errno-base.h>
#include <asm-generic/param.h>
#include <soc/ambarella/misc.h>

#define AMBARELLA_NAND_BUFFER_SIZE		8192

/* ==========================================================================*/
#define NAND_TIMING_RSHIFT24BIT(x)	(((x) & 0xff000000) >> 24)
#define NAND_TIMING_RSHIFT16BIT(x)	(((x) & 0x00ff0000) >> 16)
#define NAND_TIMING_RSHIFT8BIT(x)	(((x) & 0x0000ff00) >> 8)
#define NAND_TIMING_RSHIFT0BIT(x)	(((x) & 0x000000ff) >> 0)

#define NAND_TIMING_LSHIFT24BIT(x)	((x) << 24)
#define NAND_TIMING_LSHIFT16BIT(x)	((x) << 16)
#define NAND_TIMING_LSHIFT8BIT(x)	((x) << 8)
#define NAND_TIMING_LSHIFT0BIT(x)	((x) << 0)

static int nand_timing_calc(u32 clk, int minmax, int val)
{
	u32 x;
	int n, r;

	x = val * clk;
	n = x / 1000;
	r = x % 1000;

	if (r != 0)
		n++;

	if (minmax)
		n--;

	return n < 1 ? 0 : (n-1);
}

static int amb_ecc6_ooblayout_ecc_lp(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section >= chip->ecc.steps)
		return -ERANGE;

	oobregion->offset = (section * 16) + 6;
	oobregion->length = chip->ecc.bytes;

	return 0;
}

static int amb_ecc6_ooblayout_free_lp(struct mtd_info *mtd, int section,
				    struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section >= chip->ecc.steps)
		return -ERANGE;

	oobregion->offset = (section * 16) + 1;
	oobregion->length = 5;

	return 0;
}

static const struct mtd_ooblayout_ops amb_ecc6_lp_ooblayout_ops = {
	.ecc = amb_ecc6_ooblayout_ecc_lp,
	.free = amb_ecc6_ooblayout_free_lp,
};

static int amb_ecc8_ooblayout_ecc_lp(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section >= chip->ecc.steps)
		return -ERANGE;

	oobregion->offset = (section * 32) + 19;
	oobregion->length = chip->ecc.bytes;

	return 0;
}

static int amb_ecc8_ooblayout_free_lp(struct mtd_info *mtd, int section,
				    struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section >= chip->ecc.steps)
		return -ERANGE;

	oobregion->offset = (section * 32) + 2;
	oobregion->length = 17;

	return 0;
}

static const struct mtd_ooblayout_ops amb_ecc8_lp_ooblayout_ops = {
	.ecc = amb_ecc8_ooblayout_ecc_lp,
	.free = amb_ecc8_ooblayout_free_lp,
};

static u32 to_native_cmd(struct ambarella_nand_host *host, u32 cmd)
{
	u32 native_cmd, is_spinand = host->is_spinand;

	switch (cmd) {
	case NAND_CMD_RESET:
		native_cmd = is_spinand ? NAND_AMB_CC_RESET : NAND_AMB_CMD_RESET;
		break;
	case NAND_CMD_READID:
		native_cmd = NAND_AMB_CC_READID;
		break;
	case NAND_CMD_STATUS:
		native_cmd = is_spinand ? NAND_AMB_CC_READSTATUS : NAND_AMB_CMD_READSTATUS;
		break;
	case NAND_CMD_SET_FEATURES:
		native_cmd = NAND_AMB_CC_SETFEATURE;
		break;
	case NAND_CMD_GET_FEATURES:
		native_cmd = NAND_AMB_CC_GETFEATURE;
		break;
	case NAND_CMD_ERASE1:
		native_cmd = NAND_AMB_CC_ERASE;
		break;
	case NAND_CMD_READOOB:
	case NAND_CMD_READ0:
		native_cmd = is_spinand ? NAND_AMB_CC_READ : NAND_AMB_CMD_READ;
		break;
	case NAND_CMD_PAGEPROG:
		native_cmd = is_spinand ? NAND_AMB_CC_PROGRAM : NAND_AMB_CMD_PROGRAM;
		break;
	case NAND_CMD_PARAM:
		native_cmd = NAND_AMB_CC_READ_PARAM;
		break;
	default:
		dev_err(host->dev, "Unknown command: %d\n", cmd);
		BUG();
		break;
	}

	return native_cmd;
}

static int count_zero_bits(u8 *buf, int size, int max_bits)
{
	int i, zero_bits = 0;

	for (i = 0; i < size; i++) {
		zero_bits += hweight8(~buf[i]);
		if (zero_bits > max_bits)
			break;
	}

	return zero_bits;
}

static int nand_bch_check_blank_page(struct ambarella_nand_host *host)
{
	struct nand_chip *chip = &host->chip;
	struct mtd_info	*mtd = nand_to_mtd(chip);
	int eccsteps = chip->ecc.steps, zero_bits = 0, zeroflip = 0, oob_subset;
	u8 *bufpos, *bsp;
	u32 i;

	bufpos = host->dmabuf;
	bsp = host->dmabuf + mtd->writesize;
	oob_subset = mtd->oobsize / eccsteps;

	for (i = 0; i < eccsteps; i++) {
		zero_bits = count_zero_bits(bufpos, chip->ecc.size, chip->ecc.strength);
		if (zero_bits > chip->ecc.strength)
			return -1;

		if (zero_bits)
			zeroflip = 1;

		zero_bits += count_zero_bits(bsp, oob_subset, chip->ecc.strength);
		if (zero_bits > chip->ecc.strength)
			return -1;

		bufpos += chip->ecc.size;
		bsp += oob_subset;
	}

	if (zeroflip)
		memset(host->dmabuf, 0xff, mtd->writesize);

	return 0;
}

static int ambarella_nand_set_spinand_timing(struct ambarella_nand_host *host)
{
	u8 tclh, tcll, tcs, tclqv;
	u8 tchsl, tslch, tchsh, tshch;
	u8 thhqx, twps, twph;
	u32 t, clk, val;

	clk = clk_get_rate(host->clk) / 1000000;

	/* timing 0 */
	t = host->timing[0];
	tclh = NAND_TIMING_RSHIFT24BIT(t);
	tcll = NAND_TIMING_RSHIFT16BIT(t);
	tcs = NAND_TIMING_RSHIFT8BIT(t);
	tclqv = NAND_TIMING_RSHIFT0BIT(t);

	tclh = nand_timing_calc(clk, 0, tclh);
	tcll = nand_timing_calc(clk, 0, tcll);
	tcs = nand_timing_calc(clk, 0, tcs);
	tclqv = nand_timing_calc(clk, 1, tclqv);

	val = NAND_TIMING_LSHIFT24BIT(tclh) |
		NAND_TIMING_LSHIFT16BIT(tcll) |
		NAND_TIMING_LSHIFT8BIT(tcs) |
		NAND_TIMING_LSHIFT0BIT(tclqv);

	writel_relaxed(val, host->regbase + SPINAND_TIMING0_OFFSET);

	/* timing 1 */
	t = host->timing[1];
	tchsl = NAND_TIMING_RSHIFT24BIT(t);
	tslch = NAND_TIMING_RSHIFT16BIT(t);
	tchsh = NAND_TIMING_RSHIFT8BIT(t);
	tshch = NAND_TIMING_RSHIFT0BIT(t);

	tchsl = nand_timing_calc(clk, 0, tchsl);
	tslch = nand_timing_calc(clk, 0, tslch);
	tchsh = nand_timing_calc(clk, 0, tchsh);
	tshch = nand_timing_calc(clk, 0, tshch);

	val = NAND_TIMING_LSHIFT24BIT(tchsl) |
		NAND_TIMING_LSHIFT16BIT(tslch) |
		NAND_TIMING_LSHIFT8BIT(tchsh) |
		NAND_TIMING_LSHIFT0BIT(tshch);

	writel_relaxed(val, host->regbase + SPINAND_TIMING1_OFFSET);

	/* timing 2 */
	t = host->timing[2];
	thhqx = NAND_TIMING_RSHIFT16BIT(t);
	twps = NAND_TIMING_RSHIFT8BIT(t);
	twph = NAND_TIMING_RSHIFT0BIT(t);

	thhqx = nand_timing_calc(clk, 1, thhqx);
	twps = nand_timing_calc(clk, 0, twps);
	twph = nand_timing_calc(clk, 0, twph);

	val = NAND_TIMING_LSHIFT16BIT(thhqx) |
		NAND_TIMING_LSHIFT8BIT(twps) |
		NAND_TIMING_LSHIFT0BIT(twph);

	writel_relaxed(val, host->regbase + SPINAND_TIMING2_OFFSET);

	return 0;
}

static int ambarella_nand_set_timing(struct ambarella_nand_host *host)
{
	u8 tcls, tals, tcs, tds;
	u8 tclh, talh, tch, tdh;
	u8 twp, twh, twb, trr;
	u8 trp, treh, trb, tceh;
	u8 trdelay, tclr, twhr, tir;
	u8 tww, trhz, tar;
	u32 i, t, clk, val;

	for (i = 0; i < ARRAY_SIZE(host->timing); i++) {
		if (host->timing[i] != 0x0)
			break;
	}
	/* if the timing is not setup by Amboot, we leave the timing unchanged */
	if (i == ARRAY_SIZE(host->timing))
		return 0;

	if (host->is_spinand)
		return ambarella_nand_set_spinand_timing(host);

	clk = clk_get_rate(host->clk) / 1000000;

	/* timing 0 */
	t = host->timing[0];
	tcls = NAND_TIMING_RSHIFT24BIT(t);
	tals = NAND_TIMING_RSHIFT16BIT(t);
	tcs = NAND_TIMING_RSHIFT8BIT(t);
	tds = NAND_TIMING_RSHIFT0BIT(t);

	tcls = nand_timing_calc(clk, 0, tcls);
	tals = nand_timing_calc(clk, 0, tals);
	tcs = nand_timing_calc(clk, 0, tcs);
	tds = nand_timing_calc(clk, 0, tds);

	val = NAND_TIMING_LSHIFT24BIT(tcls) |
		NAND_TIMING_LSHIFT16BIT(tals) |
		NAND_TIMING_LSHIFT8BIT(tcs) |
		NAND_TIMING_LSHIFT0BIT(tds);

	writel_relaxed(val, host->regbase + NAND_TIMING0_OFFSET);

	/* timing 1 */
	t = host->timing[1];
	tclh = NAND_TIMING_RSHIFT24BIT(t);
	talh = NAND_TIMING_RSHIFT16BIT(t);
	tch = NAND_TIMING_RSHIFT8BIT(t);
	tdh = NAND_TIMING_RSHIFT0BIT(t);

	tclh = nand_timing_calc(clk, 0, tclh);
	talh = nand_timing_calc(clk, 0, talh);
	tch = nand_timing_calc(clk, 0, tch);
	tdh = nand_timing_calc(clk, 0, tdh);

	val = NAND_TIMING_LSHIFT24BIT(tclh) |
		NAND_TIMING_LSHIFT16BIT(talh) |
		NAND_TIMING_LSHIFT8BIT(tch) |
		NAND_TIMING_LSHIFT0BIT(tdh);

	writel_relaxed(val, host->regbase + NAND_TIMING1_OFFSET);

	/* timing 2 */
	t = host->timing[2];
	twp = NAND_TIMING_RSHIFT24BIT(t);
	twh = NAND_TIMING_RSHIFT16BIT(t);
	twb = NAND_TIMING_RSHIFT8BIT(t);
	trr = NAND_TIMING_RSHIFT0BIT(t);

	twp = nand_timing_calc(clk, 0, twp);
	twh = nand_timing_calc(clk, 0, twh);
	twb = nand_timing_calc(clk, 1, twb);
	trr = nand_timing_calc(clk, 0, trr);

	val = NAND_TIMING_LSHIFT24BIT(twp) |
		NAND_TIMING_LSHIFT16BIT(twh) |
		NAND_TIMING_LSHIFT8BIT(twb) |
		NAND_TIMING_LSHIFT0BIT(trr);

	writel_relaxed(val, host->regbase + NAND_TIMING2_OFFSET);

	/* timing 3 */
	t = host->timing[3];
	trp = NAND_TIMING_RSHIFT24BIT(t);
	treh = NAND_TIMING_RSHIFT16BIT(t);
	trb = NAND_TIMING_RSHIFT8BIT(t);
	tceh = NAND_TIMING_RSHIFT0BIT(t);

	trp = nand_timing_calc(clk, 0, trp);
	treh = nand_timing_calc(clk, 0, treh);
	trb = nand_timing_calc(clk, 1, trb);
	tceh = nand_timing_calc(clk, 1, tceh);

	val = NAND_TIMING_LSHIFT24BIT(trp) |
		NAND_TIMING_LSHIFT16BIT(treh) |
		NAND_TIMING_LSHIFT8BIT(trb) |
		NAND_TIMING_LSHIFT0BIT(tceh);

	writel_relaxed(val, host->regbase + NAND_TIMING3_OFFSET);

	/* timing 4 */
	t = host->timing[4];
	trdelay = NAND_TIMING_RSHIFT24BIT(t);
	tclr = NAND_TIMING_RSHIFT16BIT(t);
	twhr = NAND_TIMING_RSHIFT8BIT(t);
	tir = NAND_TIMING_RSHIFT0BIT(t);

	trdelay = trp + treh;
	tclr = nand_timing_calc(clk, 0, tclr);
	twhr = nand_timing_calc(clk, 0, twhr);
	tir = nand_timing_calc(clk, 0, tir);

	val = NAND_TIMING_LSHIFT24BIT(trdelay) |
		NAND_TIMING_LSHIFT16BIT(tclr) |
		NAND_TIMING_LSHIFT8BIT(twhr) |
		NAND_TIMING_LSHIFT0BIT(tir);

	writel_relaxed(val, host->regbase + NAND_TIMING4_OFFSET);

	/* timing 5 */
	t = host->timing[5];
	tww = NAND_TIMING_RSHIFT16BIT(t);
	trhz = NAND_TIMING_RSHIFT8BIT(t);
	tar = NAND_TIMING_RSHIFT0BIT(t);

	tww = nand_timing_calc(clk, 0, tww);
	trhz = nand_timing_calc(clk, 1, trhz);
	tar = nand_timing_calc(clk, 0, tar);


	val = NAND_TIMING_LSHIFT16BIT(tww) |
		NAND_TIMING_LSHIFT8BIT(trhz) |
		NAND_TIMING_LSHIFT0BIT(tar);

	writel_relaxed(val, host->regbase + NAND_TIMING5_OFFSET);

	return 0;
}

static irqreturn_t ambarella_nand_isr_handler(int irq, void *dev_id)
{
	struct ambarella_nand_host *host = dev_id;
	unsigned long		flags;
	u32 int_sts;

	int_sts = readl_relaxed(host->regbase + FIO_INT_STATUS_OFFSET);
	int_sts &= FIO_INT_OPERATION_DONE | FIO_INT_SND_LOOP_TIMEOUT |
			FIO_INT_ECC_RPT_UNCORR | FIO_INT_ECC_RPT_THRESH;
	if (int_sts) {
		spin_lock_irqsave(&host->lock, flags);
		writel_relaxed(int_sts, host->regbase + FIO_RAW_INT_STATUS_OFFSET);
		host->int_sts = int_sts;
		host->ecc_rpt_sts =
			readl_relaxed(host->regbase + FIO_ECC_RPT_STATUS_OFFSET);
		host->ecc_rpt_sts2 =
			readl_relaxed(host->regbase + FIO_ECC_RPT_STATUS2_OFFSET);
		wake_up(&host->wq);
		spin_unlock_irqrestore(&host->lock, flags);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static void ambarella_nand_setup_dma(struct ambarella_nand_host *host, u32 cmd)
{
	struct mtd_info *mtd = nand_to_mtd(&host->chip);
	u32 dmaaddr, fdma_ctrl;

	/* Setup FDMA engine transfer */
	dmaaddr = host->dmaaddr;
	writel_relaxed(dmaaddr, host->regbase + FDMA_MN_MEM_ADDR_OFFSET);
	dmaaddr = host->dmaaddr + mtd->writesize;
	writel_relaxed(dmaaddr, host->regbase + FDMA_SP_MEM_ADDR_OFFSET);

	fdma_ctrl = (cmd == NAND_AMB_CMD_READ) ? FDMA_CTRL_WRITE_MEM : FDMA_CTRL_READ_MEM;
	fdma_ctrl |= FDMA_CTRL_ENABLE | FDMA_CTRL_BLK_SIZE_512B;
	fdma_ctrl |= mtd->writesize + mtd->oobsize;
	writel(fdma_ctrl, host->regbase + FDMA_MN_CTRL_OFFSET);
}

static void ambarella_nand_readid(struct ambarella_nand_host *host, u32 page_addr)
{
	u32 val;

	/* disable BCH if using soft ecc */
	val = readl_relaxed(host->regbase + FIO_CTRL_OFFSET);
	val &= ~FIO_CTRL_ECC_BCH_ENABLE;
	writel_relaxed(val, host->regbase + FIO_CTRL_OFFSET);

	val = NAND_CC_WORD_CMD1VAL0(NAND_CMD_READID);
	writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

	writel_relaxed(page_addr, host->regbase + NAND_COPY_ADDR_OFFSET);
	writel_relaxed(0x0, host->regbase + NAND_CP_ADDR_H_OFFSET);

	val = NAND_CC_DATA_CYCLE(8) | NAND_CC_RW_READ | NAND_CC_WAIT_TWHR |
			NAND_CC_ADDR_CYCLE(1) | NAND_CC_CMD1(1) |
			NAND_CC_ADDR_SRC(0) | NAND_CC_TERMINATE_CE;
	writel_relaxed(val, host->regbase + NAND_CC_OFFSET);
}

#define ONFI_PARAM_SIZE (256)
static void ambarella_nand_cc_read_param(struct ambarella_nand_host *host, u32 page_addr)
{
	u32 dmaaddr, fdma_ctrl, val;

	/* disable BCH if using soft ecc */
	val = readl_relaxed(host->regbase + FIO_CTRL_OFFSET);
	val &= ~FIO_CTRL_ECC_BCH_ENABLE;
	writel_relaxed(val, host->regbase + FIO_CTRL_OFFSET);

	/* Setup FDMA engine transfer */
	dmaaddr = host->dmaaddr;
	writel_relaxed(dmaaddr, host->regbase + FDMA_MN_MEM_ADDR_OFFSET);

	fdma_ctrl = FDMA_CTRL_ENABLE | FDMA_CTRL_WRITE_MEM |
			FDMA_CTRL_BLK_SIZE_256B | ONFI_PARAM_SIZE;
	writel(fdma_ctrl, host->regbase + FDMA_MN_CTRL_OFFSET);

	val = NAND_CC_WORD_CMD1VAL0(NAND_CMD_PARAM);
	writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

	writel_relaxed(page_addr, host->regbase + NAND_COPY_ADDR_OFFSET);
	writel_relaxed(0x0, host->regbase + NAND_CP_ADDR_H_OFFSET);

	val = NAND_CC_RW_READ | NAND_CC_DATA_SRC_DMA | NAND_CC_WAIT_RB |
			NAND_CC_ADDR_CYCLE(1) | NAND_CC_CMD1(1) | NAND_CC_ADDR_SRC(0);
	writel_relaxed(val, host->regbase + NAND_CC_OFFSET);
}

static void ambarella_nand_cc_reset(struct ambarella_nand_host *host)
{
	u32 val;

	val = NAND_CC_WORD_CMD1VAL0(SPINAND_CMD_RESET);
	writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

	writel_relaxed(0x0, host->regbase + SPINAND_CC2_OFFSET);
	writel_relaxed(0x0, host->regbase + SPINAND_CC1_OFFSET);
}

static void ambarella_nand_cc_readid(struct ambarella_nand_host *host)
{
	u32 val;

	val = NAND_CC_WORD_CMD1VAL0(SPINAND_CMD_READ_ID);
	writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

	writel_relaxed(0x0, host->regbase + NAND_COPY_ADDR_OFFSET);
	writel_relaxed(0x0, host->regbase + NAND_CP_ADDR_H_OFFSET);

	writel_relaxed(0x0, host->regbase + SPINAND_CC2_OFFSET);

	val = SPINAND_CC_DATA_CYCLE(4) | SPINAND_CC_RW_READ |
		SPINAND_CC_ADDR_CYCLE(1);
	writel_relaxed(val, host->regbase + SPINAND_CC1_OFFSET);
}

static void ambarella_nand_cc_readstatus(struct ambarella_nand_host *host)
{
	u32 val;

	val = NAND_CC_WORD_CMD1VAL0(SPINAND_CMD_GET_FEATURE);
	writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

	writel_relaxed(0xC0, host->regbase + NAND_COPY_ADDR_OFFSET);
	writel_relaxed(0x0, host->regbase + NAND_CP_ADDR_H_OFFSET);

	writel_relaxed(0x0, host->regbase + SPINAND_CC2_OFFSET);

	val = SPINAND_CC_DATA_CYCLE(1) | SPINAND_CC_RW_READ | SPINAND_CC_ADDR_CYCLE(1);
	writel_relaxed(val, host->regbase + SPINAND_CC1_OFFSET);
}

static void ambarella_nand_cc_setfeature(struct ambarella_nand_host *host,
					u8 feature_addr, u8 value)
{
	u32 val;

	writel_relaxed(feature_addr, host->regbase + NAND_COPY_ADDR_OFFSET);
	writel_relaxed(0x0, host->regbase + NAND_CP_ADDR_H_OFFSET);

	val = NAND_CC_WORD_CMD1VAL0(SPINAND_CMD_SET_FEATURE);
	writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

	writel_relaxed(SPINAND_ERR_PATTERN, host->regbase + SPINAND_ERR_PATTERN_OFFSET);
	writel_relaxed(SPINAND_DONE_PATTERN, host->regbase + SPINAND_DONE_PATTERN_OFFSET);

	writel_relaxed(value, host->regbase + NAND_CC_DAT0_OFFSET);

	writel_relaxed(0, host->regbase + SPINAND_CC2_OFFSET);

	val = SPINAND_CC1_AUTO_WE | SPINAND_CC_AUTO_STSCHK |
		SPINAND_CC_RW_WRITE | SPINAND_CC_ADDR_CYCLE(1);
	writel_relaxed(val, host->regbase + SPINAND_CC1_OFFSET);
}

static void ambarella_nand_cc_erase(struct ambarella_nand_host *host, u32 page_addr)
{
	u32 val;

	if (host->is_spinand) {
		/* Note: spinand use page number as address for block erase. */
		writel_relaxed(page_addr, host->regbase + NAND_COPY_ADDR_OFFSET);
		writel_relaxed(0x0, host->regbase + NAND_CP_ADDR_H_OFFSET);

		val = NAND_CC_WORD_CMD1VAL0(SPINAND_CMD_BLK_ERASE);
		writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

		val = SPINAND_ERASE_ERR_PATTERN;
		writel_relaxed(val, host->regbase + SPINAND_ERR_PATTERN_OFFSET);
		val = SPINAND_DONE_PATTERN;
		writel_relaxed(val, host->regbase + SPINAND_DONE_PATTERN_OFFSET);

		writel_relaxed(0x0, host->regbase + SPINAND_CC2_OFFSET);

		val = SPINAND_CC1_AUTO_WE | SPINAND_CC_AUTO_STSCHK |
				SPINAND_CC_ADDR_CYCLE(3);
		writel_relaxed(val, host->regbase + SPINAND_CC1_OFFSET);
	} else {
		val = NAND_CC_WORD_CMD1VAL0(0x60) | NAND_CC_WORD_CMD2VAL0(0xD0);
		writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

		val = NAND_CC_DATA_CYCLE(5) | NAND_CC_WAIT_RB |
			NAND_CC_CMD2(1) | NAND_CC_ADDR_CYCLE(3) |
			NAND_CC_CMD1(1) | NAND_CC_ADDR_SRC(1) |
			NAND_CC_TERMINATE_CE;
		writel_relaxed(val, host->regbase + NAND_CC_OFFSET);
	}
}

static void ambarella_nand_cc_read(struct ambarella_nand_host *host, u32 page_addr)
{
	u32 val;

	val = NAND_CC_WORD_CMD1VAL0(SPINAND_CMD_PAGE_READ) |
		NAND_CC_WORD_CMD2VAL0(SPINAND_CMD_READ_CACHE_X4);
	writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

	writel_relaxed(SPINAND_READ_ERR_PATTERN, host->regbase + SPINAND_ERR_PATTERN_OFFSET);
	writel_relaxed(SPINAND_DONE_PATTERN, host->regbase + SPINAND_DONE_PATTERN_OFFSET);

	val = SPINAND_CC2_ENABLE | SPINAND_CC_DATA_SRC_DMA |
		SPINAND_CC_DUMMY_DATA_NUM(1) | SPINAND_CC_ADDR_CYCLE(2) |
		SPINAND_CC_ADDR_SRC(2) | SPINAND_CC_RW_READ | SPINAND_LANE_NUM(4);
	writel_relaxed(val, host->regbase + SPINAND_CC2_OFFSET);

	val = SPINAND_CC_AUTO_STSCHK | SPINAND_CC_DATA_SRC_DMA |
		SPINAND_CC_ADDR_SRC(1) | SPINAND_CC_ADDR_CYCLE(3);
	writel_relaxed(val, host->regbase + SPINAND_CC1_OFFSET);
}

static void ambarella_nand_cc_write(struct ambarella_nand_host *host, u32 page_addr)
{
	u32 val;

	val = NAND_CC_WORD_CMD1VAL0(SPINAND_CMD_PROG_LOAD_X4) |
		NAND_CC_WORD_CMD2VAL0(SPINAND_CMD_PROG_EXEC);
	writel_relaxed(val, host->regbase + NAND_CC_WORD_OFFSET);

	writel_relaxed(SPINAND_PRG_ERR_PATTERN, host->regbase + SPINAND_ERR_PATTERN_OFFSET);
	writel_relaxed(SPINAND_DONE_PATTERN, host->regbase + SPINAND_DONE_PATTERN_OFFSET);

	val = SPINAND_CC_AUTO_STSCHK | SPINAND_CC2_ENABLE | SPINAND_CC_DATA_SRC_DMA |
		SPINAND_CC_ADDR_CYCLE(3) | SPINAND_CC_ADDR_SRC(1);
	writel_relaxed(val, host->regbase + SPINAND_CC2_OFFSET);

	val = SPINAND_CC1_AUTO_WE | SPINAND_CC_DATA_SRC_DMA |
		SPINAND_CC_ADDR_CYCLE(2) | SPINAND_CC_ADDR_SRC(2) |
		SPINAND_CC_RW_WRITE | SPINAND_LANE_NUM(4);
	writel_relaxed(val, host->regbase + SPINAND_CC1_OFFSET);
}

static int ambarella_nand_issue_cmd(struct ambarella_nand_host *host,
		u32 cmd, u32 page_addr)
{
	struct mtd_info *mtd = nand_to_mtd(&host->chip);
	u64 addr64 = (u64)page_addr * mtd->writesize;
	u32 native_cmd = to_native_cmd(host, cmd);
	u32 nand_ctrl, nand_cmd;
	long timeout;
	int rval = 0;

	host->int_sts = 0;

	spin_lock_irq(&host->lock);

	if (NAND_CMD_CMD(native_cmd) == NAND_AMB_CMD_READ ||
			NAND_CMD_CMD(native_cmd) == NAND_AMB_CMD_PROGRAM)
		ambarella_nand_setup_dma(host, NAND_CMD_CMD(native_cmd));

	nand_ctrl = host->control_reg | NAND_CTRL_A33_32(addr64 >> 32);
	nand_cmd = (u32)addr64 | NAND_AMB_CMD(native_cmd);
	writel_relaxed(nand_ctrl, host->regbase + NAND_CTRL_OFFSET);
	writel_relaxed(nand_cmd, host->regbase + NAND_CMD_OFFSET);

	switch (native_cmd) {
	case NAND_AMB_CC_RESET:
		ambarella_nand_cc_reset(host);
		break;
	case NAND_AMB_CC_READID:
		if (host->is_spinand)
			ambarella_nand_cc_readid(host);
		else
			ambarella_nand_readid(host, page_addr);
		break;
	case NAND_AMB_CC_READSTATUS:
		ambarella_nand_cc_readstatus(host);
		break;
	case NAND_AMB_CC_SETFEATURE:
		ambarella_nand_cc_setfeature(host, page_addr, 0x00);
		break;
	case NAND_AMB_CC_GETFEATURE:
		break;
	case NAND_AMB_CC_ERASE:
		ambarella_nand_cc_erase(host, page_addr);
		break;
	case NAND_AMB_CC_READ:
		ambarella_nand_cc_read(host, page_addr);
		break;
	case NAND_AMB_CC_PROGRAM:
		ambarella_nand_cc_write(host, page_addr);
		break;
	case NAND_AMB_CC_READ_PARAM:
		ambarella_nand_cc_read_param(host, page_addr);
		break;
	}

	spin_unlock_irq(&host->lock);

	/* now waiting for command completed */
	timeout = wait_event_timeout(host->wq, host->int_sts, 1 * HZ);
	if (timeout <= 0) {
		rval = -EBUSY;
		dev_err(host->dev, "cmd=0x%x timeout\n", native_cmd);
	}

	/* avoid to flush previous error info */
	if (host->err_code == 0)
		host->err_code = rval;

	return rval;
}


/* ==========================================================================*/
static uint8_t ambarella_nand_read_byte(struct nand_chip *chip)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);
	uint8_t *data;

	data = host->dmabuf + host->dma_bufpos;
	host->dma_bufpos++;

	return *data;
}

static void ambarella_nand_read_buf(struct nand_chip *chip, uint8_t *buf, int len)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);

	BUG_ON((host->dma_bufpos + len) > AMBARELLA_NAND_BUFFER_SIZE);

	memcpy(buf, host->dmabuf + host->dma_bufpos, len);
	host->dma_bufpos += len;
}

static void ambarella_nand_write_buf(struct nand_chip *chip,
	const uint8_t *buf, int len)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);

	BUG_ON((host->dma_bufpos + len) > AMBARELLA_NAND_BUFFER_SIZE);

	memcpy(host->dmabuf + host->dma_bufpos, buf, len);
	host->dma_bufpos += len;
}

static void ambarella_nand_select_chip(struct nand_chip *chip, int cs)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);

	if (cs > 0)
		dev_err(host->dev, "Multi-Chip isn't supported yet.\n");
}

static void ambarella_nand_cmd_ctrl(struct nand_chip *chip, int dat, unsigned int ctrl)
{
}

static int ambarella_nand_dev_ready(struct nand_chip *chip)
{
	chip->legacy.cmdfunc(chip, NAND_CMD_STATUS, -1, -1);

	return (chip->legacy.read_byte(chip) & NAND_STATUS_READY) ? 1 : 0;
}

static int ambarella_nand_waitfunc(struct nand_chip *chip)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);
	int status = 0;

	/*
	 * ambarella nand controller has waited for the command completion,
	 * but still need to check the nand chip's status
	 */
	if (host->err_code)
		status = NAND_STATUS_FAIL;
	else {
		chip->legacy.cmdfunc(chip, NAND_CMD_STATUS, -1, -1);
		status = chip->legacy.read_byte(chip);
	}

	return status;
}

static void ambarella_nand_cmdfunc(struct nand_chip *chip, unsigned int cmd,
	int column, int page_addr)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	u32 val, *id, fio_ctr_bak = 0;

	host->err_code = 0;

	switch (cmd) {
	case NAND_CMD_ERASE2:
		break;

	case NAND_CMD_SEQIN:
		host->dma_bufpos = column;
		host->seqin_page_addr = page_addr;
		break;

	case NAND_CMD_READID:
		fio_ctr_bak = readl_relaxed(host->regbase + FIO_CTRL_OFFSET);
		host->dma_bufpos = 0;
		ambarella_nand_issue_cmd(host, cmd, column);
		break;
	case NAND_CMD_PARAM:
		host->dma_bufpos = 0;
		if (!host->is_spinand) {
			fio_ctr_bak = readl_relaxed(host->regbase + FIO_CTRL_OFFSET);
			ambarella_nand_issue_cmd(host, cmd, column);
		}
		break;
	case NAND_CMD_STATUS:
		host->dma_bufpos = 0;
		ambarella_nand_issue_cmd(host, cmd, 0);
		break;

	case NAND_CMD_RESET:
		host->dma_bufpos = 0;
		ambarella_nand_issue_cmd(host, cmd, 0);
		if (host->is_spinand) {
			usleep_range(2000, 2010);
			/* unlock all blocks */
			ambarella_nand_issue_cmd(host, NAND_CMD_SET_FEATURES, 0xA0);
		}
		break;

	case NAND_CMD_READOOB:
	case NAND_CMD_READ0:
		host->dma_bufpos = (cmd == NAND_CMD_READ0) ? column : mtd->writesize;
		ambarella_nand_issue_cmd(host, cmd, page_addr);
		break;

	case NAND_CMD_PAGEPROG:
		page_addr = host->seqin_page_addr;
		ambarella_nand_issue_cmd(host, cmd, page_addr);
		break;
	case NAND_CMD_ERASE1:
		ambarella_nand_issue_cmd(host, cmd, page_addr);
		break;

	default:
		dev_err(host->dev, "%s: 0x%x, %d, %d\n",
				__func__, cmd, column, page_addr);
		BUG();
		break;
	}

	switch (cmd) {
	case NAND_CMD_READID:
		id = (u32 *)host->dmabuf;

		val = readl_relaxed(host->regbase + NAND_CC_DAT0_OFFSET);
		*id = val;

		val = readl_relaxed(host->regbase + NAND_CC_DAT1_OFFSET);
		host->dmabuf[4] = (unsigned char)(val & 0xff);

		writel_relaxed(fio_ctr_bak, host->regbase + FIO_CTRL_OFFSET);
		break;

	case NAND_CMD_STATUS:
		if (host->is_spinand) {
			/*
			 * no matter the device is Write Enable or not, we can
			 * always send the Write Enable Command automatically
			 * prior to PROGRAM or ERASE command.
			 */
			val = readl_relaxed(host->regbase + NAND_CC_DAT0_OFFSET);
			val &= 0x000000FF;
			host->dmabuf[0] = NAND_STATUS_WP;
			if (!(val & 0x1))
				host->dmabuf[0] |= NAND_STATUS_READY;
			if (val & 0x2c)
				host->dmabuf[0] |= NAND_STATUS_FAIL;
		} else {
			val = readl_relaxed(host->regbase + NAND_STATUS_OFFSET);
			host->dmabuf[0] = (unsigned char)val;
		}
		break;

	case NAND_CMD_READOOB:
	case NAND_CMD_READ0:
		if (host->soft_ecc)
			break;

		if (host->int_sts & FIO_INT_ECC_RPT_UNCORR) {
			if (nand_bch_check_blank_page(host) < 0) {
				mtd->ecc_stats.failed++;
				dev_err(host->dev,
					"BCH corrected failed in block[%d]!\n",
					FIO_ECC_RPT_UNCORR_BLK_ADDR(host->ecc_rpt_sts2));
			}
		} else if (host->int_sts & FIO_INT_ECC_RPT_THRESH) {
			val = FIO_ECC_RPT_MAX_ERR_NUM(host->ecc_rpt_sts);
			mtd->ecc_stats.corrected += val;
			dev_info(host->dev, "BCH correct [%d]bit in block[%d]\n",
				val, FIO_ECC_RPT_BLK_ADDR(host->ecc_rpt_sts));
		}
		break;

	case NAND_CMD_PARAM:
		writel_relaxed(fio_ctr_bak, host->regbase + FIO_CTRL_OFFSET);
		break;
	}
}

static void ambarella_nand_hwctl(struct  nand_chip *chip, int mode)
{
}

static int ambarella_nand_calculate_ecc(struct nand_chip *chip,
		const u_char *buf, u_char *code)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);
	u32 i, amb_eccsize;

	if (!host->soft_ecc) {
		memset(code, 0xff, host->chip.ecc.bytes);
	} else {
		/* make it be compatible with hw bch */
		for (i = 0; i < host->chip.ecc.size; i++)
			host->bch_data[i] = bitrev8(buf[i]);

		memset(code, 0, host->chip.ecc.bytes);

		amb_eccsize = host->chip.ecc.size + host->soft_bch_extra_size;
		bch_encode(host->bch, host->bch_data, amb_eccsize, code);

		/* make it be compatible with hw bch */
		for (i = 0; i < host->chip.ecc.bytes; i++)
			code[i] = bitrev8(code[i]);
	}

	return 0;
}

static int ambarella_nand_correct_data(struct nand_chip *chip, u_char *buf,
		u_char *read_ecc, u_char *calc_ecc)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);
	u32 *errloc = host->errloc;
	int amb_eccsize, i, count;
	int rval = 0;

	/*
	 * if we use hardware ecc, any errors include DMA error and FIO DMA
	 * error, we consider it as a ecc error which will tell the caller the
	 * read is failed. We have distinguish all the errors, but the
	 * nand_read_ecc only check the return value by this function.
	 */
	if (!host->soft_ecc) {
		rval = host->err_code;
	} else {
		for (i = 0; i < chip->ecc.bytes; i++) {
			host->read_ecc_rev[i] = bitrev8(read_ecc[i]);
			host->calc_ecc_rev[i] = bitrev8(calc_ecc[i]);
		}

		amb_eccsize = chip->ecc.size + host->soft_bch_extra_size;
		count = bch_decode(host->bch, NULL,
					amb_eccsize,
					host->read_ecc_rev,
					host->calc_ecc_rev,
					NULL, errloc);
		if (count > 0) {
			for (i = 0; i < count; i++) {
				if (errloc[i] < (amb_eccsize * 8)) {
					/* error is located in data, correct it */
					buf[errloc[i] >> 3] ^= (128 >> (errloc[i] & 7));
				}
				/* else error in ecc, no action needed */

				dev_dbg(host->dev,
					"corrected bitflip %u\n", errloc[i]);
			}
		} else if (count < 0) {
			count = nand_bch_check_blank_page(host);
			if (count < 0)
				dev_err(host->dev, "ecc unrecoverable error\n");
		}

		rval = count;
	}

	return rval;
}

static int ambarella_nand_write_oob_std(struct nand_chip *chip, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct ambarella_nand_host *host = nand_get_controller_data(chip);
	uint8_t *ecc_calc = chip->ecc.calc_buf;
	int i, status, eccsteps;

	/*
	 * Our nand controller will write the generated ECC code into spare
	 * area automatically, so we should mark the ECC code which located
	 * in the eccpos.
	 */
	if (!host->soft_ecc) {
		eccsteps = chip->ecc.steps;
		for (i = 0; eccsteps; eccsteps--, i += chip->ecc.bytes) {
			chip->ecc.calculate(chip, NULL, &ecc_calc[i]);
			status = mtd_ooblayout_set_eccbytes(mtd, ecc_calc,
					chip->oob_poi, 0, chip->ecc.total);
			if (status)
				return status;
		}
	}

	chip->legacy.cmdfunc(chip, NAND_CMD_SEQIN, mtd->writesize, page);
	chip->legacy.write_buf(chip, chip->oob_poi, mtd->oobsize);
	chip->legacy.cmdfunc(chip, NAND_CMD_PAGEPROG, -1, -1);
	status = chip->legacy.waitfunc(chip);

	return status & NAND_STATUS_FAIL ? -EIO : 0;
}

/*
 * The encoding sequence in a byte is "LSB first".
 *
 * For each 2K page, there will be 2048 byte main data (B0 ~ B2047) and 64 byte
 * spare data (B2048 ~ B2111). Thus, each page is divided into 4 BCH blocks.
 * For example, B0~B511 and B2048~B2063 are grouped as the first BCH block.
 * B0 will be encoded first and B2053 will be encoded last.
 *
 * B2054 ~B2063 are used to store 10B parity data (precisely to say, 78 bits)
 * The 2 dummy bits are filled as 0 and located at the msb of B2063.
 */
static int ambarella_nand_init_soft_bch(struct ambarella_nand_host *host)
{
	struct nand_chip *chip = &host->chip;
	u32 amb_eccsize, eccbytes, m, t;

	amb_eccsize = chip->ecc.size + host->soft_bch_extra_size;
	eccbytes = chip->ecc.bytes;

	m = fls(1 + 8 * amb_eccsize);
	t = (eccbytes * 8) / m;

	host->bch = bch_init(m, t, 0, false);
	if (!host->bch)
		return -EINVAL;

	host->errloc = devm_kzalloc(host->dev, t * sizeof(*host->errloc), GFP_KERNEL);
	if (!host->errloc)
		return -ENOMEM;

	host->bch_data = devm_kzalloc(host->dev, amb_eccsize, GFP_KERNEL);
	if (host->bch_data == NULL)
		return -ENOMEM;

	/*
	 * asumming the 6 bytes data in spare area are all 0xff, in other
	 * words, we don't support to write anything except for ECC code
	 * into spare are.
	 */
	memset(host->bch_data + chip->ecc.size, 0xff, host->soft_bch_extra_size);

	return 0;
}

static void ambarella_nand_deinit_soft_bch(struct ambarella_nand_host *host)
{
	devm_kfree(host->dev, host->bch_data);
	devm_kfree(host->dev, host->errloc);
	bch_free(host->bch);
}

static void ambarella_nand_init_hw(struct ambarella_nand_host *host)
{
	u32 val;

	if (!host->is_spinand && host->pins_nand)
		pinctrl_select_state(host->pins, host->pins_nand);

	/* reset FIO by RCT */
	if (!host->is_spinand) {
		BUG_ON(host->rst_regmap == NULL);
		regmap_write(host->rst_regmap, host->rst_offset, 0x8);
		msleep(1);
		regmap_write(host->rst_regmap, host->rst_offset, 0);
		msleep(1);
	}

	/* Reset FIO FIFO and then exit random read mode */
	val = readl_relaxed(host->regbase + FIO_CTRL_OFFSET);
	val |= FIO_CTRL_RANDOM_READ;
	writel_relaxed(val, host->regbase + FIO_CTRL_OFFSET);
	/* wait for some time to make sure FIO FIFO reset is done */
	usleep_range(3000, 30010);
	val &= ~FIO_CTRL_RANDOM_READ;
	writel_relaxed(val, host->regbase + FIO_CTRL_OFFSET);

	/* always use 5 cycles to read ID */
	val = readl_relaxed(host->regbase + NAND_EXT_CTRL_OFFSET);
	val |= NAND_EXT_CTRL_I5;
	if (host->page_4k)
		val |= NAND_EXT_CTRL_4K_PAGE;
	else
		val &= ~NAND_EXT_CTRL_4K_PAGE;
	writel_relaxed(val, host->regbase + NAND_EXT_CTRL_OFFSET);

	/* always enable dual-space mode if BCH is enabled by POC */
	if (host->bch_enabled) {
		if (host->ecc_bits == 6)
			val = FDMA_DSM_MAIN_JP_SIZE_512B | FDMA_DSM_SPARE_JP_SIZE_16B;
		else
			val = FDMA_DSM_MAIN_JP_SIZE_512B | FDMA_DSM_SPARE_JP_SIZE_32B;
	} else {
		if (host->page_4k)
			val = FDMA_DSM_MAIN_JP_SIZE_4KB | FDMA_DSM_SPARE_JP_SIZE_128B;
		else
			val = FDMA_DSM_MAIN_JP_SIZE_2KB | FDMA_DSM_SPARE_JP_SIZE_64B;

		if (host->ecc_bits == 8)
			val += 0x1;
	}
	writel_relaxed(val, host->regbase + FDMA_DSM_CTRL_OFFSET);

	/* disable BCH if using soft ecc */
	val = readl_relaxed(host->regbase + FIO_CTRL_OFFSET);
	val &= ~FIO_CTRL_RDERR_STOP;
	val |= FIO_CTRL_SKIP_BLANK_ECC;
	if (host->soft_ecc || !host->bch_enabled)
		val &= ~FIO_CTRL_ECC_BCH_ENABLE;
	else
		val |= FIO_CTRL_ECC_BCH_ENABLE;
	writel_relaxed(val, host->regbase + FIO_CTRL_OFFSET);

	if (host->is_spinand) {
		val = readl_relaxed(host->regbase + FIO_CTRL2_OFFSET);
		val |= FIO_CTRL2_SPINAND;
		writel_relaxed(val, host->regbase + FIO_CTRL2_OFFSET);

		val = readl_relaxed(host->regbase + SPINAND_CTRL_OFFSET);

		if (host->sck_mode3)
			val |= SPINAND_CTRL_SCKMODE_3;

		val &= ~SPINAND_CTRL_PS_SEL_MASK;
		val |= SPINAND_CTRL_PS_SEL_6;

		writel_relaxed(val, host->regbase + SPINAND_CTRL_OFFSET);
	}

	ambarella_nand_set_timing(host);

	/* setup min number of correctable bits that not trigger irq. */
	val = FIO_ECC_RPT_ERR_NUM_TH(host->ecc_bits);
	writel_relaxed(val, host->regbase + FIO_ECC_RPT_CFG_OFFSET);

	/* clear and enable nand irq */
	val = readl_relaxed(host->regbase + FIO_RAW_INT_STATUS_OFFSET);
	writel_relaxed(val, host->regbase + FIO_RAW_INT_STATUS_OFFSET);
	val = FIO_INT_OPERATION_DONE | FIO_INT_SND_LOOP_TIMEOUT |
			FIO_INT_ECC_RPT_UNCORR | FIO_INT_ECC_RPT_THRESH | FIO_INT_AXI_BUS_ERR;
	writel_relaxed(val, host->regbase + FIO_INT_ENABLE_OFFSET);
}

static void ambarella_nand_init(struct ambarella_nand_host *host)
{
	struct ambarella_nand_soc_data *soc_data = host->soc_data;
	struct device_node *np = host->dev->of_node;
	u32 poc = ambarella_sys_config();
	int rval;

	host->page_4k = (poc & soc_data->poc_mask_pagesize) ? false : true;
	host->sck_mode3 = (poc & soc_data->poc_mask_sckmode) ? true : false;
	host->bch_enabled = (poc & soc_data->poc_mask_bchen) ? true : false;
	host->is_spinand = (poc & soc_data->poc_mask_spinand) ? true : false;

	host->enable_wp = !!of_find_property(np, "amb,enable-wp", NULL);

	rval = of_property_read_u32_array(np, "amb,timing", host->timing, 6);
	if (rval < 0) {
		dev_dbg(host->dev, "No timing defined!\n");
		memset(host->timing, 0x0, sizeof(host->timing));
	}

	rval = of_property_read_u32(np, "amb,soft-ecc", &host->ecc_bits);
	if (rval < 0)
		host->ecc_bits = (poc & soc_data->poc_mask_spare2x) ? 8 : 6;
	else
		host->soft_ecc = true;

	dev_info(host->dev, "in %secc-[%d]bit mode\n",
			host->soft_ecc ? "soft " : "", host->ecc_bits);

	/*
	 * Always use P3 and I5 to support all NAND,
	 * but we will adjust page cycles after read ID from NAND.
	 */
	host->control_reg = NAND_CTRL_P3 | NAND_CTRL_SIZE_8G;
	host->control_reg |= host->enable_wp ? NAND_CTRL_WP : 0;

	ambarella_nand_init_hw(host);
}

static const struct ambarella_nand_soc_data ambarella_nand_soc_data_v0 = {
	.poc_mask_spinand	= 0x00400000,
	.poc_mask_sckmode	= 0x00080000,
	.poc_mask_8kfifo	= 0x00000000, /* not used */
	.poc_mask_pagesize	= 0x00040000,
	.poc_mask_bchen		= 0x00010000,
	.poc_mask_spare2x	= 0x00008000,
};

static const struct ambarella_nand_soc_data ambarella_nand_soc_data_v1 = {
	.poc_mask_spinand	= 0xffffffff, /* not used, spinand only */
	.poc_mask_sckmode	= 0x00080000,
	.poc_mask_8kfifo	= 0x00000000, /* not used */
	.poc_mask_pagesize	= 0x00040000,
	.poc_mask_bchen		= 0x00010000,
	.poc_mask_spare2x	= 0x00008000,
};

static const struct ambarella_nand_soc_data ambarella_nand_soc_data_v2 = {
	.poc_mask_spinand	= 0xffffffff, /* not used, spinand only */
	.poc_mask_sckmode	= 0x00040000,
	.poc_mask_8kfifo	= 0x00100000,
	.poc_mask_pagesize	= 0x00020000,
	.poc_mask_bchen		= 0x00008000,
	.poc_mask_spare2x	= 0x00004000,
};

static const struct soc_device_attribute ambarella_nand_socinfo[] = {
	{ .soc_id = "cv22", .data = &ambarella_nand_soc_data_v0 },
	{ .soc_id = "cv25", .data = &ambarella_nand_soc_data_v0 },
	{ .soc_id = "cv2", .data = &ambarella_nand_soc_data_v0 },
	{ .soc_id = "s6lm", .data = &ambarella_nand_soc_data_v1 },
	{/* sentinel */}
};

static int ambarella_nand_get_resource(struct ambarella_nand_host *host,
		struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const struct soc_device_attribute *soc;
	int rval = 0;

	host->regbase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->regbase)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(host->regbase);
	}

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq < 0) {
		dev_err(&pdev->dev, "no irq found!\n");
		return -ENODEV;
	}

	host->pins = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(host->pins)) {
		dev_err(&pdev->dev, "default pins not configured: %ld\n",
			 PTR_ERR(host->pins));
		return PTR_ERR(host->pins);
	}

	host->pins_nand = pinctrl_lookup_state(host->pins, "nand");
	if (IS_ERR(host->pins_nand))
		host->pins_nand = NULL;

	host->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR_OR_NULL(host->clk)) {
		dev_err(&pdev->dev, "Could not get clock!\n");
		return -ENOENT;
	}

	host->rst_regmap = syscon_regmap_lookup_by_phandle(np, "amb,regmap");
	if (IS_ERR(host->rst_regmap)) {
		host->rst_regmap = NULL;
		host->rst_offset = 0;
	} else {
		if (of_property_read_u32_index(np, "amb,regmap", 1, &host->rst_offset)) {
			dev_err(&pdev->dev, "no regmap offset\n");
			return -EINVAL;
		}
	}

	rval = devm_request_irq(&pdev->dev, host->irq, ambarella_nand_isr_handler,
			IRQF_SHARED | IRQF_TRIGGER_HIGH, "nand_irq", host);
	if (rval < 0) {
		dev_err(&pdev->dev, "Could not register irq %d!\n", host->irq);
		return rval;
	}

	soc = soc_device_match(ambarella_nand_socinfo);
	if (soc)
		host->soc_data = (struct ambarella_nand_soc_data *)soc->data;
	else
		host->soc_data = (struct ambarella_nand_soc_data *)&ambarella_nand_soc_data_v2;

	return 0;
}

static int ambarella_attach_chip(struct nand_chip *chip)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);

	if (chip->bbt_options & NAND_BBT_USE_FLASH)
		chip->bbt_options |= NAND_BBT_NO_OOB;
	else
		chip->options |= NAND_SKIP_BBTSCAN;

	/* sanity check */
	BUG_ON(mtd->writesize != 2048 && mtd->writesize != 4096);
	BUG_ON(host->ecc_bits != 6 && host->ecc_bits != 8);
	BUG_ON(host->ecc_bits == 8 && mtd->oobsize < 128);

	if (host->soft_ecc) {
		if (ambarella_nand_init_soft_bch(host) < 0)
			return -ENOMEM;
	}

	if (host->bch_enabled)
		chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	else
		chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_NONE;

	switch (host->ecc_bits) {
	case 8:
		chip->ecc.size = 512;
		chip->ecc.bytes = 13;
		chip->ecc.strength = 8;
		host->soft_bch_extra_size = 19;
		mtd_set_ooblayout(mtd, &amb_ecc8_lp_ooblayout_ops);
		break;
	case 6:
		chip->ecc.size = 512;
		chip->ecc.bytes = 10;
		chip->ecc.strength = 6;
		host->soft_bch_extra_size = 6;
		mtd_set_ooblayout(mtd, &amb_ecc6_lp_ooblayout_ops);
		break;
	}

	chip->ecc.hwctl = ambarella_nand_hwctl;
	chip->ecc.calculate = ambarella_nand_calculate_ecc;
	chip->ecc.correct = ambarella_nand_correct_data;
	chip->ecc.write_oob = ambarella_nand_write_oob_std;

	/* The nand may be pasrsed as MLC, we set it to SLC mandatory.*/
	nanddev_get_memorg(&chip->base)->bits_per_cell = 1;

	switch (nanddev_target_size(&chip->base)) {
	case 8 * 1024 * 1024:
		host->control_reg = NAND_CTRL_SIZE_64M;
		break;
	case 16 * 1024 * 1024:
		host->control_reg = NAND_CTRL_SIZE_128M;
		break;
	case 32 * 1024 * 1024:
		host->control_reg = NAND_CTRL_SIZE_256M;
		break;
	case 64 * 1024 * 1024:
		host->control_reg = NAND_CTRL_SIZE_512M;
		break;
	case 128 * 1024 * 1024:
		host->control_reg = NAND_CTRL_SIZE_1G;
		break;
	case 256 * 1024 * 1024:
		host->control_reg = NAND_CTRL_SIZE_2G;
		break;
	case 512 * 1024 * 1024:
		host->control_reg = NAND_CTRL_SIZE_4G;
		break;
	case 1024 * 1024 * 1024:
		host->control_reg = NAND_CTRL_SIZE_8G;
		break;
	default:
		dev_err(host->dev, "Unexpected NAND flash chipsize. Aborting\n");
		return -ENXIO;
	}

	if (host->chip.options & NAND_ROW_ADDR_3)
		host->control_reg |= NAND_CTRL_P3;

	return 0;
}

static void ambarella_deattach_chip(struct nand_chip *chip)
{
	struct ambarella_nand_host *host = nand_get_controller_data(chip);

	if (host->soft_ecc)
		ambarella_nand_deinit_soft_bch(host);
}

static const struct nand_controller_ops ambarella_controller_ops = {
	.attach_chip = ambarella_attach_chip,
	.detach_chip = ambarella_deattach_chip,
};

static int ambarella_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ambarella_nand_host *host;
	struct mtd_info *mtd;
	int rval;

	host = devm_kzalloc(dev, sizeof(struct ambarella_nand_host), GFP_KERNEL);
	if (host == NULL)
		return -ENOMEM;

	host->dev = dev;
	dev_set_drvdata(dev, host);

	spin_lock_init(&host->lock);
	init_waitqueue_head(&host->wq);

	host->dmabuf = dmam_alloc_coherent(host->dev, AMBARELLA_NAND_BUFFER_SIZE,
				&host->dmaaddr, GFP_KERNEL);
	if (host->dmabuf == NULL) {
		dev_err(host->dev, "dma_alloc_coherent failed!\n");
		ambarella_nand_deinit_soft_bch(host);
		return -ENOMEM;
	}

	rval = ambarella_nand_get_resource(host, pdev);
	if (rval < 0)
		return rval;

	ambarella_nand_init(host);

	mtd = nand_to_mtd(&host->chip);
	mtd->name = "amba_nand";

	nand_controller_init(&host->controller);
	nand_set_controller_data(&host->chip, host);
	nand_set_flash_node(&host->chip, dev->of_node);

	host->chip.controller = &host->controller;
	host->chip.controller->ops = &ambarella_controller_ops;
	host->chip.legacy.chip_delay = 0;
	host->chip.legacy.read_byte = ambarella_nand_read_byte;
	host->chip.legacy.write_buf = ambarella_nand_write_buf;
	host->chip.legacy.read_buf = ambarella_nand_read_buf;
	host->chip.legacy.select_chip = ambarella_nand_select_chip;
	host->chip.legacy.cmd_ctrl = ambarella_nand_cmd_ctrl;
	host->chip.legacy.dev_ready = ambarella_nand_dev_ready;
	host->chip.legacy.waitfunc = ambarella_nand_waitfunc;
	host->chip.legacy.cmdfunc = ambarella_nand_cmdfunc;
	host->chip.legacy.set_features = nand_get_set_features_notsupp;
	host->chip.legacy.get_features = nand_get_set_features_notsupp;
	host->chip.options |= NAND_NO_SUBPAGE_WRITE | NAND_USES_DMA;

	rval = nand_scan(&host->chip, 1);
	if (rval < 0)
		return rval;

	rval = mtd_device_register(mtd, NULL, 0);
	if (rval < 0)
		nand_cleanup(&host->chip);

	return rval;
}

static int ambarella_nand_remove(struct platform_device *pdev)
{
	struct ambarella_nand_host *host = platform_get_drvdata(pdev);

	WARN_ON(mtd_device_unregister(nand_to_mtd(&host->chip)));
	nand_cleanup(&host->chip);

	return 0;
}

#ifdef CONFIG_PM
static int ambarella_nand_suspend(struct device *dev)
{
	struct ambarella_nand_host *host;
	struct platform_device *pdev = to_platform_device(dev);

	host = platform_get_drvdata(pdev);
	disable_irq(host->irq);

	return 0;
}

static int ambarella_nand_restore(struct device *dev)
{
	struct ambarella_nand_host *host;
	struct platform_device *pdev = to_platform_device(dev);

	host = platform_get_drvdata(pdev);
	ambarella_nand_init_hw(host);
	enable_irq(host->irq);
	nand_reset_op(&host->chip);

	return 0;
}

static int ambarella_nand_resume(struct device *dev)
{
	struct ambarella_nand_host *host;
	struct platform_device *pdev = to_platform_device(dev);

	host = platform_get_drvdata(pdev);
	ambarella_nand_init_hw(host);
	enable_irq(host->irq);
	nand_reset_op(&host->chip);

	return 0;
}

static const struct dev_pm_ops ambarella_nand_pm_ops = {
	/* suspend to memory */
	.suspend = ambarella_nand_suspend,
	.resume = ambarella_nand_resume,

	/* suspend to disk */
	.freeze = ambarella_nand_suspend,
	.thaw = ambarella_nand_resume,

	/* restore from suspend to disk */
	.restore = ambarella_nand_restore,
};
#endif

static const struct of_device_id ambarella_nand_of_match[] = {
	{.compatible = "ambarella,nand", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_nand_of_match);

static struct platform_driver ambarella_nand_driver = {
	.probe		= ambarella_nand_probe,
	.remove		= ambarella_nand_remove,
	.driver = {
		.name	= "ambarella-nand",
		.of_match_table = ambarella_nand_of_match,
#ifdef CONFIG_PM
		.pm	= &ambarella_nand_pm_ops,
#endif
	},
};
module_platform_driver(ambarella_nand_driver);

MODULE_AUTHOR("Cao Rongrong");
MODULE_DESCRIPTION("Ambarella Combo NAND Controller Driver");
MODULE_LICENSE("GPL");
