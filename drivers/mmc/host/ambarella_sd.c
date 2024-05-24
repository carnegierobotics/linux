/*
 * drivers/mmc/host/ambarella_sd.c
 *
 * Author: Anthony Ginger <hfjiang@ambarella.com>
 * Copyright (C) 2004-2009, Ambarella, Inc.
 *
 * History:
 *	2016/03/03 - [Cao Rongrong] Re-write the driver
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
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/sys_soc.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/debugfs.h>
#include <linux/leds.h>
#include <soc/ambarella/misc.h>
#include "ambarella_sd.h"

void ambarella_detect_sd_slot(int id, int fixed_cd)
{
	struct ambarella_mmc_host *host;
	struct platform_device *pdev;
	struct device_node *np;
	char tmp[4];

	snprintf(tmp, sizeof(tmp), "sd%d", id);

	np = of_find_node_by_path(tmp);
	if (!np) {
		pr_err("%s: No np found for %s\n", __func__, tmp);
		return;
	}

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		pr_err("%s: No device found for %s\n", __func__, tmp);
		return;
	}

	host = platform_get_drvdata(pdev);
	host->fixed_cd = fixed_cd;
	mmc_detect_change(host->mmc, 0);
}
EXPORT_SYMBOL(ambarella_detect_sd_slot);

static ssize_t ambarella_fixed_cd_get(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct ambarella_mmc_host *host = file->private_data;
	char tmp[4];

	snprintf(tmp, sizeof(tmp), "%d\n", host->fixed_cd);
	tmp[3] = '\n';

	return simple_read_from_buffer(buf, count, ppos, tmp, sizeof(tmp));
}

static ssize_t ambarella_fixed_cd_set(struct file *file, const char __user *buf,
		size_t size, loff_t *ppos)
{
	struct ambarella_mmc_host *host = file->private_data;
	char tmp[20];
	ssize_t len;

	len = simple_write_to_buffer(tmp, sizeof(tmp) - 1, ppos, buf, size);
	if (len >= 0) {
		tmp[len] = '\0';
		host->fixed_cd = !!simple_strtoul(tmp, NULL, 0);
	}

	mmc_detect_change(host->mmc, 0);

	return len;
}

static const struct file_operations fixed_cd_fops = {
	.read	= ambarella_fixed_cd_get,
	.write	= ambarella_fixed_cd_set,
	.open	= simple_open,
	.llseek	= default_llseek,
};

static void ambarella_sd_add_debugfs(struct ambarella_mmc_host *host)
{
	struct mmc_host *mmc = host->mmc;
	struct dentry *root, *fixed_cd;

	if (!mmc->debugfs_root)
		return;

	root = debugfs_create_dir("ambhost", mmc->debugfs_root);
	if (IS_ERR_OR_NULL(root))
		return;

	host->debugfs = root;

	fixed_cd = debugfs_create_file("fixed_cd", S_IWUSR | S_IRUGO,
			host->debugfs, host, &fixed_cd_fops);
	if (IS_ERR_OR_NULL(fixed_cd)) {
		debugfs_remove_recursive(root);
		host->debugfs = NULL;
		dev_err(host->dev, "failed to add debugfs\n");
	}
}

static void ambarella_sd_remove_debugfs(struct ambarella_mmc_host *host)
{
	if (host->debugfs) {
		debugfs_remove_recursive(host->debugfs);
		host->debugfs = NULL;
	}
}

/* ==========================================================================*/

static void ambarella_sd_enable_irq(struct ambarella_mmc_host *host, u32 mask)
{
	u32 tmp;

	tmp = readl_relaxed(host->regbase + SD_NISEN_OFFSET);
	tmp |= mask;
	writel_relaxed(tmp, host->regbase + SD_NISEN_OFFSET);

	tmp = readl_relaxed(host->regbase + SD_NIXEN_OFFSET);
	tmp |= mask;
	writel_relaxed(tmp, host->regbase + SD_NIXEN_OFFSET);

}

static void ambarella_sd_disable_irq(struct ambarella_mmc_host *host, u32 mask)
{
	u32 tmp;

	tmp = readl_relaxed(host->regbase + SD_NISEN_OFFSET);
	tmp &= ~mask;
	writel_relaxed(tmp, host->regbase + SD_NISEN_OFFSET);

	tmp = readl_relaxed(host->regbase + SD_NIXEN_OFFSET);
	tmp &= ~mask;
	writel_relaxed(tmp, host->regbase + SD_NIXEN_OFFSET);
}

static void ambarella_sd_switch_clk(struct ambarella_mmc_host *host, bool enable)
{
	u32 clk_reg;

	clk_reg = readw_relaxed(host->regbase + SD_CLK_OFFSET);
	if (enable)
		clk_reg |= SD_CLK_EN;
	else
		clk_reg &= ~SD_CLK_EN;
	writew_relaxed(clk_reg, host->regbase + SD_CLK_OFFSET);

	/* delay to wait for SD_CLK output stable, this is somewhat useful. */
	udelay(10);
}

static u32 ambarella_sd_set_dll(struct ambarella_mmc_host *host, u32 sbc)
{
	regmap_field_write(host->phy_sbc, sbc);

	regmap_field_update_bits(host->phy_ctrl, SD_PHY_RESET, SD_PHY_RESET);
	/* wait time for DLL reset */
	if (in_interrupt())
		udelay(5);
	else
		usleep_range(5, 10);

	regmap_field_update_bits(host->phy_ctrl, SD_PHY_RESET, 0);
	/* wait time for DLL lock */
	if (in_interrupt())
		udelay(5);
	else
		usleep_range(5, 10);

	return SD_PHY_MAX_DLL_VFINE;
}

static void ambarella_sd_reset_all(struct ambarella_mmc_host *host)
{
	u32 nis, eis;
	ktime_t timeout;

	ambarella_sd_disable_irq(host, 0xffffffff);

	/* reset sd phy timing if exist */
	if (host->reg_rct) {
		regmap_field_write(host->phy_sbc, 0x0);
		regmap_field_write(host->phy_sel, 0x0);
		regmap_field_write(host->phy_ctrl, 0x0407);
	}

	writeb_relaxed(SD_RESET_ALL, host->regbase + SD_RESET_OFFSET);
	timeout = ktime_add_ms(ktime_get(), 10);
	while (readb_relaxed(host->regbase + SD_RESET_OFFSET) & SD_RESET_ALL) {
		if(ktime_after(ktime_get(), timeout)) {
			dev_info(host->dev, "reset all timeout\n");
			break;
		}
		cpu_relax();
	}

	/* need 3 SD clock cycles to let bit0 of SD 0x74 go low */
	udelay(1);
	/* for emmc boot controller, check boot status register if ready to boot */
	if(host->emmc_boot_controller) {
		timeout = ktime_add_ms(ktime_get(), 200);
		while ((readb_relaxed(host->regbase + SD_BOOT_STA_OFFSET) & 0x1) == 0x0) {
			if(ktime_after(ktime_get(), timeout)) {
				dev_info(host->dev, "wait boot rdy timeout\n");
				break;
			}
			cpu_relax();
		}
	}

	if(host->latctrl)
		writel_relaxed(host->latctrl, host->regbase + SD_LAT_CTRL_OFFSET);

	if(host->timing_reg_num == 4) {
		writel_relaxed(host->phy_timing[0], host->regbase + SD_LAT_CTRL_OFFSET);
		regmap_field_write(host->phy_ctrl, host->phy_timing[1]);
		regmap_field_write(host->phy_sel, host->phy_timing[2]);
		ambarella_sd_set_dll(host, host->phy_timing[3]);
	} else if(host->timing_reg_num == 3) {
		writel_relaxed(host->phy_timing[0], host->regbase + SD_LAT_CTRL_OFFSET);
		writel_relaxed(host->phy_timing[1], host->regbase + SD_DELAY_SEL_L);
		writel_relaxed(host->phy_timing[2], host->regbase + SD_DELAY_SEL_H);
	}

	if((host->mode == MMC_TIMING_MMC_HS200) ||
	    (host->mode == MMC_TIMING_UHS_SDR104) ||
	    (host->mode == MMC_TIMING_UHS_SDR50)) {
		writel_relaxed(host->sd_delay_mux[0], host->regbase + SD_DELAY_SEL_L);
		writel_relaxed(host->sd_delay_mux[1], host->regbase + SD_DELAY_SEL_H);
	}

	nis = SD_NISEN_REMOVAL | SD_NISEN_INSERT |
		SD_NISEN_DMA |	SD_NISEN_BLOCK_GAP |
		SD_NISEN_XFR_DONE | SD_NISEN_CMD_DONE |
		SD_NISEN_CARD;

	eis = SD_EISEN_ADMA_ERR | SD_EISEN_ACMD12_ERR |
		SD_EISEN_CURRENT_ERR |	SD_EISEN_DATA_BIT_ERR |
		SD_EISEN_DATA_CRC_ERR | SD_EISEN_DATA_TMOUT_ERR |
		SD_EISEN_CMD_IDX_ERR |	SD_EISEN_CMD_BIT_ERR |
		SD_EISEN_CMD_CRC_ERR |	SD_EISEN_CMD_TMOUT_ERR;

	ambarella_sd_enable_irq(host, (eis << 16) | nis);
}

static void ambarella_sd_set_clk(struct mmc_host *mmc, u32 clock)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);
	u32 sd_clk, clk_reg;
	ktime_t timeout;

	host->clock = 0;
	writew_relaxed(0, host->regbase + SD_CLK_OFFSET);

	if (clock != 0) {
		if (clock <= 400000 && host->fixed_init_clk > 0)
			clock = host->fixed_init_clk;

		/* FIXME: When resume from self refresh, if `clock` is equal to `host->clk->rate`,
		 * clk_set_rate() will return immediately without accessing the hardware register.
		 * Calling clk_get_rate() before clk_set_rate() is order to set the `host->clk->rate`
		 * as the actual value.*/
		clk_get_rate(host->clk);

		sd_clk = min_t(u32, clock, mmc->f_max);
		clk_set_rate(host->clk, sd_clk);

		sd_clk = clk_get_rate(host->clk);

		host->clock = sd_clk;
		host->ns_in_one_cycle = DIV_ROUND_UP(1000000000, sd_clk);
		mmc->max_busy_timeout = (1 << 27) / (sd_clk / 1000);

		/* after changing clk, some delay is useful */
		udelay(20);
		timeout = ktime_add_ms(ktime_get(), 100);
		writew_relaxed(SD_CLK_ICLK_EN, host->regbase + SD_CLK_OFFSET);
		while (!(readw_relaxed(host->regbase + SD_CLK_OFFSET) & SD_CLK_ICLK_STABLE)) {
			if(ktime_after(ktime_get(), timeout)) {
				dev_info(host->dev, "%s wait clk stable timeout\n", __func__);
				break;
			}
			cpu_relax();
		}
		udelay(10);
		clk_reg = readw_relaxed(host->regbase + SD_CLK_OFFSET);
		writew_relaxed(clk_reg | SD_CLK_EN, host->regbase + SD_CLK_OFFSET);

		udelay(100);
	}
}

static void ambarella_sd_set_pwr(struct ambarella_mmc_host *host, u8 power_mode)
{
	if (power_mode == MMC_POWER_OFF) {
		ambarella_sd_reset_all(host);
		writeb_relaxed(SD_PWR_OFF, host->regbase + SD_PWR_OFFSET);

		if (gpio_is_valid(host->pwr_gpio)) {
			gpio_set_value_cansleep(host->pwr_gpio,
						!host->pwr_gpio_active);
			msleep(host->pwr_up_dly);
		}

		if (gpio_is_valid(host->v18_gpio)) {
			gpio_set_value_cansleep(host->v18_gpio,
						!host->v18_gpio_active);
			msleep(10);
		}
	} else if (power_mode == MMC_POWER_UP) {
		if (gpio_is_valid(host->v18_gpio)) {
			gpio_set_value_cansleep(host->v18_gpio,
						!host->v18_gpio_active);
			msleep(10);
		}

		if (gpio_is_valid(host->pwr_gpio)) {
			gpio_set_value_cansleep(host->pwr_gpio,
						host->pwr_gpio_active);
			msleep(host->pwr_up_dly);
		}

		writeb_relaxed(SD_PWR_ON | SD_PWR_3_3V, host->regbase + SD_PWR_OFFSET);
	}

	host->power_mode = power_mode;
}

static void ambarella_sd_set_bus(struct ambarella_mmc_host *host, u8 bus_width, u8 mode)
{
	u32 hostr = 0, tmp;

	hostr = readb_relaxed(host->regbase + SD_HOST_OFFSET);
	hostr |= SD_HOST_ADMA;

	switch (bus_width) {
	case MMC_BUS_WIDTH_8:
		hostr |= SD_HOST_8BIT;
		hostr &= ~(SD_HOST_4BIT);
		break;
	case MMC_BUS_WIDTH_4:
		hostr &= ~(SD_HOST_8BIT);
		hostr |= SD_HOST_4BIT;
		break;
	case MMC_BUS_WIDTH_1:
	default:
		hostr &= ~(SD_HOST_8BIT);
		hostr &= ~(SD_HOST_4BIT);
		break;
	}

	if(host->set_hs_bit)
		hostr |= SD_HOST_HIGH_SPEED;
	else
		hostr &= ~SD_HOST_HIGH_SPEED;

	writeb_relaxed(hostr, host->regbase + SD_HOST_OFFSET);

	tmp = readl_relaxed(host->regbase + SD_XC_CTR_OFFSET);
	tmp &= ~SD_XC_CTR_DDR_EN;
	writel_relaxed(tmp, host->regbase + SD_XC_CTR_OFFSET);

	tmp = readw_relaxed(host->regbase + SD_HOST2_OFFSET);
	tmp &= ~0x0004;
	writew_relaxed(tmp, host->regbase + SD_HOST2_OFFSET);

	host->bus_width = bus_width;
	host->mode = mode;

	if((host->mode == MMC_TIMING_MMC_HS200) ||
	    (host->mode == MMC_TIMING_UHS_SDR104) ||
	    (host->mode == MMC_TIMING_UHS_SDR50)) {
		writel_relaxed(host->sd_delay_mux[0], host->regbase + SD_DELAY_SEL_L);
		writel_relaxed(host->sd_delay_mux[1], host->regbase + SD_DELAY_SEL_H);
	}
}

static void ambarella_sd_recovery(struct ambarella_mmc_host *host)
{
	u32 latency, ctrl_reg = 0, sel_reg = 0, sbc_reg = 0, pwr_reg, clk_reg;
	ktime_t timeout;

	latency = readl_relaxed(host->regbase + SD_LAT_CTRL_OFFSET);
	if (host->reg_rct) {
		regmap_field_read(host->phy_ctrl, &ctrl_reg);
		regmap_field_read(host->phy_sel, &sel_reg);
		regmap_field_read(host->phy_sbc, &sbc_reg);
	}

	pwr_reg = readb_relaxed(host->regbase + SD_PWR_OFFSET);

	ambarella_sd_reset_all(host);

	/* restore power reg setting*/
	writeb_relaxed(pwr_reg, host->regbase + SD_PWR_OFFSET);

	/* restore the clock*/
	writew_relaxed(SD_CLK_ICLK_EN, host->regbase + SD_CLK_OFFSET);
	timeout = ktime_add_ms(ktime_get(), 10);
	while (!(readw_relaxed(host->regbase + SD_CLK_OFFSET) & SD_CLK_ICLK_STABLE)) {
		if(ktime_after(ktime_get(), timeout)) {
			dev_info(host->dev, "wait clk stable timeout\n");
			break;
		}
		cpu_relax();
	}
	udelay(10);
	clk_reg = readw_relaxed(host->regbase + SD_CLK_OFFSET);
	writew_relaxed(clk_reg | SD_CLK_EN, host->regbase + SD_CLK_OFFSET);
	udelay(10);

	/* host reg will be precess in this func */
	ambarella_sd_set_bus(host, host->bus_width, host->mode);

	/* restore the latency register */
	writel_relaxed(latency, host->regbase + SD_LAT_CTRL_OFFSET);
	if (host->reg_rct) {
		regmap_field_write(host->phy_ctrl, ctrl_reg);
		regmap_field_write(host->phy_sel, sel_reg);
		ambarella_sd_set_dll(host, sbc_reg);
	}
}

static void ambarella_sd_timer_timeout(struct timer_list *t)
{
	struct ambarella_mmc_host *host = from_timer(host, t, timer);
	struct mmc_request *mrq = host->mrq;
	u32 dir;

	dev_err(host->dev, "pending mrq: %s[%u],arg=0x%x\n",
			mrq ? mrq->data ? "data" : "cmd" : "",
			mrq ? mrq->cmd->opcode : 9999, mrq->cmd->arg);

	if (mrq) {
		if (mrq->data) {
			if (mrq->data->flags & MMC_DATA_WRITE)
				dir = DMA_TO_DEVICE;
			else
				dir = DMA_FROM_DEVICE;
			dma_unmap_sg(host->dev, mrq->data->sg, mrq->data->sg_len, dir);
		}
		mrq->cmd->error = -ETIMEDOUT;
		host->mrq = NULL;
		host->cmd = NULL;
		mmc_request_done(host->mmc, mrq);
	}

	ambarella_sd_recovery(host);
}

static void ambarella_sd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);

	if (host->power_mode != ios->power_mode)
		ambarella_sd_set_pwr(host, ios->power_mode);

	if (host->clock != ios->clock)
		ambarella_sd_set_clk(mmc, ios->clock);

	if ((host->bus_width != ios->bus_width) || (host->mode != ios->timing))
		ambarella_sd_set_bus(host, ios->bus_width, ios->timing);

	/* delay to wait for set bus stable, support MICRON EMMC. */
	mdelay(10);

}

static int ambarella_sd_get_cd(struct mmc_host *mmc)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);
	int cd_pin = host->fixed_cd;

	if (cd_pin < 0)
		cd_pin = mmc_gpio_get_cd(mmc);

	if (cd_pin < 0) {
		cd_pin = readl_relaxed(host->regbase + SD_STA_OFFSET);
		cd_pin &= SD_STA_CARD_INSERTED;
	}

	return !!cd_pin;
}

static int ambarella_sd_get_ro(struct mmc_host *mmc)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);
	int wp_pin = host->fixed_wp;

	if (wp_pin < 0)
		wp_pin = mmc_gpio_get_ro(mmc);

	if (wp_pin < 0) {
		wp_pin = readl_relaxed(host->regbase + SD_STA_OFFSET);
		wp_pin &= SD_STA_WPS_PL;
	}

	return !!wp_pin;
}

static int ambarella_sd_check_ready(struct ambarella_mmc_host *host)
{
	u32 sta_reg = 0, check = SD_STA_CMD_INHIBIT_CMD;
	unsigned long timeout;

	if (host->cmd->data || (host->cmd->flags & MMC_RSP_BUSY))
		check |= SD_STA_CMD_INHIBIT_DAT;

	/* We shouldn't wait for data inhibit for stop commands, even
	   though they might use busy signaling */
	if (host->cmd == host->mrq->stop)
		check &= ~SD_STA_CMD_INHIBIT_DAT;

	timeout = jiffies + HZ;

	while ((readl_relaxed(host->regbase + SD_STA_OFFSET) & check)
		&& time_before(jiffies, timeout))
		cpu_relax();

	if (time_after(jiffies, timeout)) {
		dev_err(host->dev, "Wait bus idle timeout [0x%08x]\n", sta_reg);
		ambarella_sd_recovery(host);
		return -ETIMEDOUT;
	}

	return 0;
}

static void ambarella_sd_set_timeout(struct ambarella_mmc_host *host)
{
	u32 cycle_ns = host->ns_in_one_cycle, clks, tmo, try_num = 0;
	struct mmc_command *cmd = host->cmd;
	struct mmc_data *data = cmd->data;

	if (data)
		clks = data->timeout_clks + DIV_ROUND_UP(data->timeout_ns, cycle_ns);
	else
		clks = cmd->busy_timeout * 1000000 / host->ns_in_one_cycle;

	if (clks == 0)
		clks = 1 << 27;

	tmo = clamp(order_base_2(clks), 13, 27) - 13;
	writeb_relaxed(tmo, host->regbase + SD_TMO_OFFSET);

	/*
	 * change timeout setting may trigger unwanted "data timeout error"
	 * interrupt immediately, so here we clear it manually.
	 */
	while (readw_relaxed(host->regbase + SD_EIS_OFFSET) & SD_EIS_DATA_TMOUT_ERR) {
		writew_relaxed(SD_EIS_DATA_TMOUT_ERR, host->regbase + SD_EIS_OFFSET);
		if (try_num++ > 100)
			break;
	}

	BUG_ON(try_num > 100);
}

static void ambarella_sd_setup_dma(struct ambarella_mmc_host *host,
		struct mmc_data *data)
{
	struct ambarella_sd_dma_desc *desc;
	struct scatterlist *sgent;
	u32 i, dir, sg_cnt, dma_len, dma_addr, word_num, byte_num;

	if (data->flags & MMC_DATA_WRITE)
		dir = DMA_TO_DEVICE;
	else
		dir = DMA_FROM_DEVICE;

	sg_cnt = dma_map_sg(host->dev, data->sg, data->sg_len, dir);
	BUG_ON(sg_cnt == 0);

	desc = host->desc_virt;
	for_each_sg(data->sg, sgent, sg_cnt, i) {
		dma_addr = sg_dma_address(sgent);
		dma_len = sg_dma_len(sgent);

		BUG_ON(dma_len > SD_ADMA_TBL_LINE_MAX_LEN);

		word_num = dma_len / 4;
		byte_num = dma_len % 4;

		desc->attr = SD_ADMA_TBL_ATTR_TRAN | SD_ADMA_TBL_ATTR_VALID;

		if (word_num > 0) {
			desc->attr |= SD_ADMA_TBL_ATTR_WORD;
			desc->len = word_num % 0x10000; /* 0 means 65536 words */
			desc->addr = dma_addr;

			if (byte_num > 0) {
				dma_addr += word_num << 2;
				desc++;
			}
		}

		if (byte_num > 0) {
			desc->attr = SD_ADMA_TBL_ATTR_TRAN | SD_ADMA_TBL_ATTR_VALID;
			desc->len = byte_num % 0x10000; /* 0 means 65536 bytes */
			desc->addr = dma_addr;
		}

		if(unlikely(i == sg_cnt - 1))
			desc->attr |= SD_ADMA_TBL_ATTR_END;
		else
			desc++;
	}
}

static int ambarella_sd_send_cmd(struct ambarella_mmc_host *host, struct mmc_command *cmd)
{
	u32 cmd_reg, xfr_reg = 0, arg_reg = cmd->arg, sd_host2 = 0;
	int rval;

	host->cmd = cmd;

	if (cmd && cmd->opcode != MMC_SEND_TUNING_BLOCK &&
			cmd->opcode != MMC_SEND_TUNING_BLOCK_HS200) {
		dev_dbg(host->dev, "Start %s[%u], arg = 0x%08x\n",
			cmd->data ? "data" : "cmd", cmd->opcode, cmd->arg);
	}

	/* use MMC_SWITCH cmd and arg to identify switch to and leave RPMB partition */
	if(unlikely((cmd->opcode == MMC_SWITCH) && ((cmd->arg & 0xFFFF0700) == 0x03b30300)))
		host->visited_rpmb = 1;
	if(unlikely(host->visited_rpmb && (cmd->opcode == MMC_SWITCH) && ((cmd->arg & 0xFFFF0700) != 0x03b30300)))
		host->visited_rpmb = 0;

	rval = ambarella_sd_check_ready(host);
	if (rval < 0) {
		struct mmc_request *mrq = host->mrq;
		cmd->error = rval;
		host->mrq = NULL;
		host->cmd = NULL;
		mmc_request_done(host->mmc, mrq);
		return -EIO;
	}

	cmd_reg = SD_CMD_IDX(cmd->opcode);

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_NONE:
		cmd_reg |= SD_CMD_RSP_NONE;
		break;
	case MMC_RSP_R1B:
		cmd_reg |= SD_CMD_RSP_48BUSY;
		break;
	case MMC_RSP_R2:
		cmd_reg |= SD_CMD_RSP_136;
		break;
	default:
		cmd_reg |= SD_CMD_RSP_48;
		break;
	}

	if (mmc_resp_type(cmd) & MMC_RSP_CRC)
		cmd_reg |= SD_CMD_CHKCRC;

	if (mmc_resp_type(cmd) & MMC_RSP_OPCODE)
		cmd_reg |= SD_CMD_CHKIDX;

	if (cmd->data) {
		cmd_reg |= SD_CMD_DATA;
		xfr_reg = SD_XFR_DMA_EN;

		/* if stream, we should clear SD_XFR_BLKCNT_EN to enable
		 * infinite transfer, but this is NOT supported by ADMA.
		 * however, fortunately, it seems no one use MMC_DATA_STREAM
		 * in mmc core. */

		if (mmc_op_multi(cmd->opcode) || cmd->data->blocks > 1)
			xfr_reg |= SD_XFR_MUL_SEL | SD_XFR_BLKCNT_EN;

		if (cmd->data->flags & MMC_DATA_READ)
			xfr_reg |= SD_XFR_CTH_SEL;

		/* if CMD23 is used, we should not send CMD12, unless any
		 * errors happened in read/write operation. So we disable
		 * auto_cmd12, but send CMD12 manually when necessary. */
		if (host->auto_cmd12 && !cmd->mrq->sbc && cmd->data->stop)
			xfr_reg |= SD_XFR_AC12_EN;

		/*if cmd23 is used, amba host need enable cmd23 sequences*/
		if(cmd->mrq->sbc) {
			sd_host2 = readw_relaxed(host->regbase + SD_HOST2_OFFSET);
			sd_host2 |= (1 << 13);
			writew_relaxed(sd_host2, host->regbase + SD_HOST2_OFFSET);
		} else {
			sd_host2 = readw_relaxed(host->regbase + SD_HOST2_OFFSET);
			sd_host2 &= ~(1 << 13);
			writew_relaxed(sd_host2, host->regbase + SD_HOST2_OFFSET);
		}

		writel(host->desc_phys, host->regbase + SD_ADMA_ADDR_OFFSET);
		writew_relaxed(cmd->data->blksz, host->regbase + SD_BLK_SZ_OFFSET);
		writew_relaxed(cmd->data->blocks, host->regbase + SD_BLK_CNT_OFFSET);
	}

	ambarella_sd_set_timeout(host);

	writew_relaxed(xfr_reg, host->regbase + SD_XFR_OFFSET);
	writel_relaxed(arg_reg, host->regbase + SD_ARG_OFFSET);
	writew_relaxed(cmd_reg, host->regbase + SD_CMD_OFFSET);

	mod_timer(&host->timer, jiffies + 5 * HZ);

	return 0;
}

static void ambarella_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);
	unsigned long flags;
	int present;

	present = ambarella_sd_get_cd(host->mmc);

	spin_lock_irqsave(&host->lock, flags);
	WARN_ON(host->mrq);
	host->mrq = mrq;

	if(likely(present)) {
		if (mrq->data)
			ambarella_sd_setup_dma(host, mrq->data);

		ambarella_sd_send_cmd(host, mrq->sbc ? mrq->sbc : mrq->cmd);
	} else {
		mrq->cmd->error = -ENOMEDIUM;
		host->mrq = NULL;
		host->cmd = NULL;
		mmc_request_done(host->mmc, mrq);
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

static void __ambarella_sd_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);

	if (enable)
		ambarella_sd_enable_irq(host, SD_NISEN_CARD);
	else
		ambarella_sd_disable_irq(host, SD_NISEN_CARD);
}

static void ambarella_sd_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);

	if (enable)
		pm_runtime_get_noresume(host->dev);

	__ambarella_sd_enable_sdio_irq(mmc, enable);

	if (!enable)
		pm_runtime_put_noidle(host->dev);
}

static void ambarella_sd_ack_sdio_irq(struct mmc_host *mmc)
{
	__ambarella_sd_enable_sdio_irq(mmc, 1);
}

static int ambarella_sd_switch_voltage(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_330) {
		if (host->force_v18)
			return -EINVAL;
		writeb_relaxed(SD_PWR_ON | SD_PWR_3_3V, host->regbase + SD_PWR_OFFSET);

		if (gpio_is_valid(host->v18_gpio)) {
			gpio_set_value_cansleep(host->v18_gpio,
						!host->v18_gpio_active);
			msleep(host->switch_1v8_dly);
		}
	} else if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180) {
		ambarella_sd_switch_clk(host, false);
		writeb_relaxed(SD_PWR_ON | SD_PWR_1_8V, host->regbase + SD_PWR_OFFSET);

		if (gpio_is_valid(host->v18_gpio)) {
			gpio_set_value_cansleep(host->v18_gpio,
						host->v18_gpio_active);
			msleep(host->switch_1v8_dly);
		}
		ambarella_sd_switch_clk(host, true);
	}

	return 0;
}

static int ambarella_sd_card_busy(struct mmc_host *mmc)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);

	/* only check if data0 is low */
	return !(readl_relaxed(host->regbase + SD_STA_OFFSET) & 0x100000);
}

static int ambarella_sd_execute_tuning(struct mmc_host *mmc, u32 opcode)
{
	struct ambarella_mmc_host *host = mmc_priv(mmc);
	u32 clock, doing_retune = mmc->doing_retune;
	u32 best_misc = 0, best_s = 0, best_e = 0, longest_range = 0;
	u32 tmp, dly, sel, vfine, misc, s, e, range;

	if (!host->reg_rct)
		return 0;

#if 0
	tmp = readb_relaxed(host->regbase + SD_HOST_OFFSET);
	tmp |= SD_HOST_HIGH_SPEED;
	writeb_relaxed(tmp, host->regbase + SD_HOST_OFFSET);
#endif

	if(host->timing_reg_num != 0) {
		/* the fixed timing will be set in ambarella_sd_recovery */
		ambarella_sd_recovery(host);
		return 0;
	}

retry:
	if (doing_retune && !host->tuning_fixed_clk_freq) {
		tmp = clk_get_rate(clk_get_parent(host->clk));
		clock = tmp / (tmp / host->clock + 1);
		if (clock <= 48000000)
			return -ECANCELED;

		ambarella_sd_set_clk(mmc, clock);
		dev_dbg(host->dev, "doing retune, clock = %d\n", host->clock);
	}

	misc = 0;
	regmap_field_write(host->phy_ctrl, 0x0000);

retry_latency:
	if (host->fixed_latency == 1)
		misc |= BIT(0);
	tmp = (misc & BIT(0)) + 1;
	tmp = (tmp << 12) | (tmp << 8) | (tmp << 4) | (tmp << 0);
	writel_relaxed(tmp, host->regbase + SD_LAT_CTRL_OFFSET);
	misc &= ~BIT(1);

retry_tx_clk:
	if (host->bypass_tx_clk == 1)
		misc |= BIT(1);
	tmp = (misc & BIT(1)) ? SD_PHY_CLKOUT_BYPASS : 0;
	regmap_field_update_bits(host->phy_ctrl, SD_PHY_CLKOUT_BYPASS, tmp);
	misc &= ~BIT(2);

retry_rx_clk:
	if (host->invert_rx_clk == 1)
		misc |= BIT(2);
	tmp = (misc & BIT(2)) ? SD_PHY_DIN_CLK_POL : 0;
	regmap_field_update_bits(host->phy_ctrl, SD_PHY_DIN_CLK_POL, tmp);
	misc &= ~BIT(3);

retry_dll_clk:
	if (host->invert_dll_clk == 1)
		misc |= BIT(3);
	tmp = (misc & BIT(3)) ? SD_PHY_DLL_CLK_POL : 0;
	regmap_field_update_bits(host->phy_ctrl, SD_PHY_DLL_CLK_POL, tmp);

	vfine = SD_PHY_MAX_DLL_VFINE;
	s = -1;
	e = range = 0;

	for (dly = 0; dly < 128; dly++) {
		sel = dly % 32;
		if (sel == 0) {
			/*
			 * DLL has been saturated with last SC, so that
			 * no need to try larger SC any more.
			 */
			if (vfine < SD_PHY_MAX_DLL_VFINE)
				break;

			/* update coarse delay, i.e., SC */
			tmp = ((dly >> 5) << 1) | SD_PHY_SBC_DEFAULT_VALUE;
			vfine = ambarella_sd_set_dll(host, tmp);
		}

		if (sel < vfine)
			sel = (vfine - sel) | 0x20;
		else
			sel = sel - vfine;

		/* some field is reserved which does not have to set  */
		tmp = (sel << 24) | (sel << 16) | (sel << 8) | (sel << 0);
		regmap_field_write(host->phy_sel, tmp);

		if (mmc_send_tuning(mmc, opcode, NULL) == 0) {
			/* Tuning is successful at this tuning point */
			if (s == -1)
				s = dly;
			e = dly;
			range++;
		} else {
			/* if range is 1 or 2, it seems nonsense */
			if (range > 2) {
				dev_dbg(host->dev, "tuning: lat[%ld], %s, %s, %s, count[%d](%d-%d)\n",
					(misc & BIT(0)) + 1,
					(misc & BIT(1)) ? "tx_clk_bypass" : "tx_clk_delays",
					(misc & BIT(2)) ? "rx_clk_invert" : "rx_clk_normal",
					(misc & BIT(3)) ? "dll_clk_invert" : "dll_clk_normal",
					e - s + 1, s, e);
			}
			if (range > longest_range) {
				best_misc = misc;
				best_s = s;
				best_e = e;
				longest_range = range;
			}
			s = -1;
			e = range = 0;
		}
	}

	if (range > 2) {
		dev_dbg(host->dev, "tuning: lat[%ld], %s, %s, %s, count[%d](%d-%d)\n",
			(misc & BIT(0)) + 1,
			(misc & BIT(1)) ? "tx_clk_bypass" : "tx_clk_delays",
			(misc & BIT(2)) ? "rx_clk_invert" : "rx_clk_normal",
			(misc & BIT(3)) ? "dll_clk_invert" : "dll_clk_normal",
			e - s + 1, s, e);
	}

	/* in case the last timings are all working */
	if (range > longest_range) {
		best_misc = misc;
		best_s = s;
		best_e = e;
		longest_range = range;
	}

	if (host->invert_dll_clk == -1 && (misc & BIT(3)) == 0) {
		misc |= BIT(3);
		goto retry_dll_clk;
	}

	if (host->invert_rx_clk == -1 && (misc & BIT(2)) == 0) {
		misc |= BIT(2);
		goto retry_rx_clk;
	}

	if (host->bypass_tx_clk == -1 && (misc & BIT(1)) == 0) {
		misc |= BIT(1);
		goto retry_tx_clk;
	}

	if (host->fixed_latency == -1 && (misc & BIT(0)) == 0) {
		misc |= BIT(0);
		goto retry_latency;
	}

	/* we set threshhold to 16 according to experiences */
	if (longest_range < 16) {
		if (host->tuning_fixed_clk_freq)
			return -ECANCELED;

		doing_retune = 1;
		goto retry;
	}

	tmp = (best_misc & BIT(0)) + 1;
	tmp = (tmp << 12) | (tmp << 8) | (tmp << 4) | (tmp << 0);
	writel_relaxed(tmp, host->regbase + SD_LAT_CTRL_OFFSET);

	tmp = (best_misc & BIT(1)) ? SD_PHY_CLKOUT_BYPASS : 0;
	regmap_field_update_bits(host->phy_ctrl, SD_PHY_CLKOUT_BYPASS, tmp);

	tmp = (best_misc & BIT(2)) ? SD_PHY_DIN_CLK_POL : 0;
	regmap_field_update_bits(host->phy_ctrl, SD_PHY_DIN_CLK_POL, tmp);

	tmp = (best_misc & BIT(3)) ? SD_PHY_DLL_CLK_POL : 0;
	regmap_field_update_bits(host->phy_ctrl, SD_PHY_DLL_CLK_POL, tmp);

	tmp = ((((best_s + best_e) / 2) >> 5) << 1) | SD_PHY_SBC_DEFAULT_VALUE;
	vfine = ambarella_sd_set_dll(host, tmp);

	if((best_s >> 5) + 2 <= (best_e >> 5)){
		/* start and end cross at least one full 32 length range */
		best_s = 0;
		best_e = 31;
	} else if ((best_s >> 5) + 1 == (best_e >> 5)) {
		/* start and end cross one 32 length range, but not full */
		if((32 - best_s % 32) > (best_e % 32 + 1)) {
			/* compare [best_s % 32 , 31] and [0, best_e % 32] length */
			best_s = best_s % 32;
			best_e = 31;
		} else {
			best_s = 0;
			best_e = best_e % 32;
		}
	} else {
		/* start and end within one 32 length range */
		best_s = best_s % 32;
		best_e = best_e % 32;
	}

	sel = (best_s + best_e) / 2;
	if (sel < vfine)
		sel = (vfine - sel) | 0x20;
	else
		sel = sel - vfine;

	tmp = (sel << 24) | (sel << 16) | (sel << 8) | (sel << 0);
	regmap_field_write(host->phy_sel, tmp);

	/* after tuning do reset all is useful */
	ambarella_sd_recovery(host);

	return 0;
}

static const struct mmc_host_ops ambarella_sd_host_ops = {
	.request = ambarella_sd_request,
	.set_ios = ambarella_sd_set_ios,
	.get_ro = ambarella_sd_get_ro,
	.get_cd = ambarella_sd_get_cd,
	.enable_sdio_irq = ambarella_sd_enable_sdio_irq,
	.ack_sdio_irq = ambarella_sd_ack_sdio_irq,
	.start_signal_voltage_switch = ambarella_sd_switch_voltage,
	.card_busy = ambarella_sd_card_busy,
	.execute_tuning = ambarella_sd_execute_tuning,
};

static void ambarella_sd_handle_irq(struct ambarella_mmc_host *host)
{
	struct mmc_command *cmd = host->cmd;
	u32 resp0, resp1, resp2, resp3;
	u16 nis, eis, ac12es, adma_err_sta;

	if (cmd == NULL)
		return;

	nis = host->irq_status & 0xffff;
	eis = host->irq_status >> 16;
	ac12es = host->ac12es;
	adma_err_sta = host->adma_err_status;

	if (nis & SD_NIS_ERROR) {
		if (eis & (SD_EIS_CMD_BIT_ERR | SD_EIS_CMD_CRC_ERR))
			cmd->error = -EILSEQ;
		else if (eis & SD_EIS_CMD_TMOUT_ERR)
			cmd->error = -ETIMEDOUT;
		else if ((eis & SD_EIS_DATA_TMOUT_ERR) && !cmd->data)
			/* for cmd without data, but needs to check busy */
			cmd->error = -ETIMEDOUT;
		else if (eis & SD_EIS_CMD_IDX_ERR)
			cmd->error = -EIO;

		if (cmd->data) {
			if (eis & (SD_EIS_DATA_BIT_ERR | SD_EIS_DATA_CRC_ERR))
				cmd->data->error = -EILSEQ;
			else if (eis & SD_EIS_DATA_TMOUT_ERR)
				cmd->data->error = -ETIMEDOUT;
			else if (eis & SD_EIS_ADMA_ERR) {
				/* when vistit rpmb partition, ignore ADMA error */
				if (host->visited_rpmb) {
					mdelay(1);
					goto finish;
				} else
					cmd->data->error = -EIO;
			}
		}

		if (cmd->data && cmd->data->stop && (eis & SD_EIS_ACMD12_ERR)) {
			if (ac12es & (SD_AC12ES_CRC_ERROR | SD_AC12ES_END_BIT))
				cmd->data->stop->error = -EILSEQ;
			else if (ac12es & SD_AC12ES_TMOUT_ERROR)
				cmd->data->stop->error = -ETIMEDOUT;
			else if (ac12es & (SD_AC12ES_NOT_EXECED | SD_AC12ES_INDEX))
				cmd->data->stop->error = -EIO;
		}

		if (!cmd->error && (!cmd->data || !cmd->data->error)
			&& (!cmd->data || !cmd->data->stop || !cmd->data->stop->error)) {
			dev_warn(host->dev, "Miss error: 0x%04x, 0x%04x!\n", nis, eis);
			cmd->error = -EIO;
		}

		if (cmd->opcode != MMC_SEND_TUNING_BLOCK &&
			cmd->opcode != MMC_SEND_TUNING_BLOCK_HS200) {
			dev_dbg(host->dev, "%s[%u] error: "
				"nis[0x%04x], eis[0x%04x], ac12es[0x%08x], adam_err_status[0x%08x]!\n",
				cmd->data ? "data" : "cmd", cmd->opcode,
				nis, eis, ac12es, adma_err_sta);
		}

		goto finish;
	}

	if (nis & SD_NIS_CMD_DONE) {
		if (cmd->flags & MMC_RSP_136) {
			resp0 = readl_relaxed(host->regbase + SD_RSP0_OFFSET);
			resp1 = readl_relaxed(host->regbase + SD_RSP1_OFFSET);
			resp2 = readl_relaxed(host->regbase + SD_RSP2_OFFSET);
			resp3 = readl_relaxed(host->regbase + SD_RSP3_OFFSET);
			cmd->resp[0] = (resp3 << 8) | (resp2 >> 24);
			cmd->resp[1] = (resp2 << 8) | (resp1 >> 24);
			cmd->resp[2] = (resp1 << 8) | (resp0 >> 24);
			cmd->resp[3] = (resp0 << 8);
		} else {
			cmd->resp[0] = readl_relaxed(host->regbase + SD_RSP0_OFFSET);
		}

		/* if cmd without data needs to check busy, we have to
		 * wait for transfer_complete IRQ */
		if (!cmd->data && !(cmd->flags & MMC_RSP_BUSY))
			goto finish;
	}

	if (nis & SD_NIS_XFR_DONE) {
		if (cmd->data)
			cmd->data->bytes_xfered = cmd->data->blksz * cmd->data->blocks;
		goto finish;
	}

	return;

finish:
	tasklet_schedule(&host->finish_tasklet);

	return;

}

static void ambarella_sd_tasklet_finish(unsigned long param)
{
	struct ambarella_mmc_host *host = (struct ambarella_mmc_host *)param;
	struct mmc_request *mrq = host->mrq;
	struct mmc_command *cmd = host->cmd;
	unsigned long flags;
	u16 dir;

	if (cmd == NULL || mrq == NULL)
		return;

	spin_lock_irqsave(&host->lock, flags);

	if (cmd && cmd->opcode != MMC_SEND_TUNING_BLOCK &&
			cmd->opcode != MMC_SEND_TUNING_BLOCK_HS200) {
		dev_dbg(host->dev, "End %s[%u], arg = 0x%08x\n",
			cmd->data ? "data" : "cmd", cmd->opcode, cmd->arg);
	}

	del_timer(&host->timer);

	if(cmd->error || (cmd->data && (cmd->data->error ||
		(cmd->data->stop && cmd->data->stop->error))))
		ambarella_sd_recovery(host);

	/* now we send READ/WRITE cmd if current cmd is CMD23 */
	if (cmd == mrq->sbc) {
		ambarella_sd_send_cmd(host, mrq->cmd);
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	if (cmd->data) {
		u32 dma_size;

		dma_size = cmd->data->blksz * cmd->data->blocks;
		dir = cmd->data->flags & MMC_DATA_WRITE ? DMA_TO_DEVICE : DMA_FROM_DEVICE;

		dma_unmap_sg(host->dev, cmd->data->sg, cmd->data->sg_len, dir);

		/* send the STOP cmd manually if auto_cmd12 is disabled and
		 * there is no preceded CMD23 for multi-read/write cmd */
		if (!host->auto_cmd12 && !mrq->sbc && cmd->data->stop) {
			ambarella_sd_send_cmd(host, cmd->data->stop);
			spin_unlock_irqrestore(&host->lock, flags);
			return;
		}
	}

	/* auto_cmd12 is disabled if mrq->sbc existed, which means the SD
	 * controller will not send CMD12 automatically. However, if any
	 * error happened in CMD18/CMD25 (read/write), we need to send
	 * CMD12 manually. */
	if ((cmd == mrq->cmd) && mrq->sbc && mrq->data->error && mrq->stop) {
		dev_dbg(host->dev, "SBC|DATA data error, send STOP manually!\n");
		ambarella_sd_send_cmd(host, mrq->stop);
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	host->cmd = NULL;
	host->mrq = NULL;

	spin_unlock_irqrestore(&host->lock, flags);
	mmc_request_done(host->mmc, mrq);
}

static irqreturn_t ambarella_sd_irq(int irq, void *devid)
{
	struct ambarella_mmc_host *host = devid;
	struct mmc_command *cmd = host->cmd;

	/* Read and clear the interrupt registers. Note: ac12es has to be
	 * read here to clear auto_cmd12 error irq. */
	spin_lock(&host->lock);
	host->adma_err_status = readb_relaxed(host->regbase + SD_ADMA_STA_OFFSET);
	host->ac12es = readl_relaxed(host->regbase + SD_AC12ES_OFFSET);
	host->irq_status = readl_relaxed(host->regbase + SD_NIS_OFFSET);
	writel_relaxed(host->irq_status, host->regbase + SD_NIS_OFFSET);

	if (cmd && cmd->opcode != MMC_SEND_TUNING_BLOCK &&
			cmd->opcode != MMC_SEND_TUNING_BLOCK_HS200) {
		dev_dbg(host->dev, "irq_status = 0x%08x, ac12es = 0x%08x, adma_err_status = 0x%08x\n",
			host->irq_status, host->ac12es, host->adma_err_status);
	}

	if (host->irq_status & SD_NIS_CARD) {
		__ambarella_sd_enable_sdio_irq(host->mmc, 0);
		sdio_signal_irq(host->mmc);
	}

	if ((host->fixed_cd == -1) &&
		(host->irq_status & (SD_NIS_REMOVAL | SD_NIS_INSERT))) {
		dev_dbg(host->dev, "0x%08x, card %s\n", host->irq_status,
			(host->irq_status & SD_NIS_INSERT) ? "Insert" : "Removed");
		mmc_detect_change(host->mmc, msecs_to_jiffies(500));
	}

	ambarella_sd_handle_irq(host);
	spin_unlock(&host->lock);

	return IRQ_HANDLED;
}

static int ambarella_sd_init_hw(struct ambarella_mmc_host *host)
{
	u32 gpio_init_flag;
	int rval;

	if (gpio_is_valid(host->pwr_gpio)) {
		if (host->pwr_gpio_active)
			gpio_init_flag = GPIOF_OUT_INIT_LOW;
		else
			gpio_init_flag = GPIOF_OUT_INIT_HIGH;

		rval = devm_gpio_request_one(host->dev, host->pwr_gpio,
					gpio_init_flag, "sd ext power");
		if (rval < 0) {
			dev_err(host->dev, "Failed to request pwr-gpios!\n");
			return 0;
		}
	}

	if (gpio_is_valid(host->v18_gpio)) {
		if (host->v18_gpio_active)
			gpio_init_flag = GPIOF_OUT_INIT_LOW;
		else
			gpio_init_flag = GPIOF_OUT_INIT_HIGH;

		rval = devm_gpio_request_one(host->dev, host->v18_gpio,
				gpio_init_flag, "sd 1v8 switch");
		if (rval < 0) {
			dev_err(host->dev, "Failed to request v18-gpios!\n");
			return 0;
		}
	}

	ambarella_sd_reset_all(host);
	host->bus_width = -1;

	return 0;
}

static int ambarella_sd_of_parse(struct ambarella_mmc_host *host)
{
	struct device_node *np = host->dev->of_node, *child;
	struct mmc_host *mmc = host->mmc;
	enum of_gpio_flags flags;
	int rval;

	rval = mmc_of_parse(mmc);
	if (rval < 0)
		return rval;

	if (!of_property_read_bool(np, "amb,no-cap-cmd23"))
		mmc->caps |= MMC_CAP_CMD23;

	if (of_property_read_u32(np, "amb,switch-1v8-dly", &host->switch_1v8_dly) < 0)
		host->switch_1v8_dly = 100;

	if (of_property_read_u32(np, "amb,pwr-up-dly", &host->pwr_up_dly) < 0)
		host->pwr_up_dly = 300;

	if (of_property_read_u32(np, "amb,fixed-init-clk", &host->fixed_init_clk) < 0)
		host->fixed_init_clk = 0;

	if (of_property_read_u32(np, "amb,fixed-wp", &host->fixed_wp) < 0)
		host->fixed_wp = -1;

	if (of_property_read_u32(np, "amb,fixed-cd", &host->fixed_cd) < 0)
		host->fixed_cd = -1;

	if (of_property_read_u32(np, "amb,tuning-latency", &host->fixed_latency) < 0)
		host->fixed_latency = -1;

	if (of_property_read_u32(np, "amb,tuning-tx-clk", &host->bypass_tx_clk) < 0)
		host->bypass_tx_clk = -1;

	if (of_property_read_u32(np, "amb,tuning-rx-clk", &host->invert_rx_clk) < 0)
		host->invert_rx_clk = -1;

	if (of_property_read_u32(np, "amb,tuning-dll-clk", &host->invert_dll_clk) < 0)
		host->invert_dll_clk = -1;

	if (of_property_read_bool(np, "amb,tuning-fixed-clk-freq"))
		host->tuning_fixed_clk_freq = true;
	else
		host->tuning_fixed_clk_freq = false;

	host->timing_reg_num = of_property_count_u32_elems(np, "amb,phy-timing");
	if(host->timing_reg_num == 4)
		of_property_read_u32_array(np, "amb,phy-timing", host->phy_timing, 4);
	else if(host->timing_reg_num == 3)
		of_property_read_u32_array(np, "amb,phy-timing", host->phy_timing, 3);
	else
		host->timing_reg_num = 0;

	if (of_property_read_u32(np, "amb,latctrl", &host->latctrl) < 0)
		host->latctrl = 0;

	if(of_property_read_u32_array(np, "amb,sd-delay-mux", host->sd_delay_mux, 2) < 0)
		host->sd_delay_mux[0] = host->sd_delay_mux[1] = 0;

	host->have_led = of_property_read_bool(np, "amb,have-led");
	host->auto_cmd12 = of_property_read_bool(np, "amb,auto-cmd12");
	host->force_v18 = of_property_read_bool(np, "amb,sd-force-1_8v");
	host->set_hs_bit = of_property_read_bool(np, "amb,sd-hs-bit");

	/* old style of sd device tree defines slot subnode, these codes are
	 * just for compatibility. Note: we should remove this workaroud when
	 * none use slot subnode in future. */
	child = of_get_child_by_name(np, "slot");
	if (child)
		np = child;

	/* gpio for external power control */
	host->pwr_gpio = of_get_named_gpio_flags(np, "pwr-gpios", 0, &flags);
	host->pwr_gpio_active = !!(flags & OF_GPIO_ACTIVE_LOW);

	/* gpio for 3.3v/1.8v switch, except for force 1.8v enabled */
	if (!host->force_v18) {
		host->v18_gpio = of_get_named_gpio_flags(np, "v18-gpios", 0, &flags);
		host->v18_gpio_active = !!(flags & OF_GPIO_ACTIVE_LOW);
	} else {
		host->v18_gpio = -1;
	}

	mmc->ops = &ambarella_sd_host_ops;
	mmc->max_blk_size = 1024; /* please check SD_CAP_OFFSET */
	mmc->max_blk_count = 65535;
	mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;
	mmc->max_segs = PAGE_SIZE / 8;
	mmc->max_seg_size = SD_ADMA_TBL_LINE_MAX_LEN;

	clk_set_rate(host->clk, mmc->f_max);
	mmc->f_max = clk_get_rate(host->clk);
	mmc->f_min = 24000000 / 256;
	mmc->max_busy_timeout = (1 << 27) / (mmc->f_max / 1000);

	if (!host->force_v18) {
		mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	}

	mmc->caps |= MMC_CAP_4_BIT_DATA | MMC_CAP_BUS_WIDTH_TEST;

	if (gpio_is_valid(host->v18_gpio) || host->force_v18) {
		mmc->ocr_avail |= MMC_VDD_165_195;

		mmc->caps |= MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25 |
			MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR104;
		mmc->caps2 |= MMC_CAP2_HS200_1_8V_SDR;
	}

	/* Process SDIO IRQs through the sdio_irq_work. */
	if (mmc->caps & MMC_CAP_SDIO_IRQ)
		mmc->caps2 |= MMC_CAP2_SDIO_IRQ_NOTHREAD;

	return 0;
}

static const struct soc_device_attribute ambarella_mmc_socinfo[] = {
	{ .soc_id = "cv5" },
	{/* sentinel */}
};

static int ambarella_sd_get_resource(struct platform_device *pdev,
			struct ambarella_mmc_host *host)
{
	struct device_node *np = host->dev->of_node;
	struct device *dev = host->dev;
	struct reg_field field;
	u32 regs[6], emmc_slot, poc = ambarella_sys_config();
	int rval;

	host->regbase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->regbase)) {
		dev_err(dev, "devm_ioremap() failed\n");
		return PTR_ERR(host->regbase);
	}

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq < 0) {
		dev_err(dev, "Get SD/MMC irq resource failed!\n");
		return -ENXIO;
	}

	host->clk = devm_clk_get(host->dev, NULL);
	if (IS_ERR(host->clk)) {
		dev_err(host->dev, "Get PLL failed!\n");
		return PTR_ERR(host->clk);
	}

	host->reg_rct = syscon_regmap_lookup_by_phandle(np, "amb,rct-regmap");
	if (IS_ERR(host->reg_rct)) {
		host->reg_rct = NULL;
		return 0;
	}

	rval = of_property_read_u32_array(np, "amb,rct-regmap", regs, ARRAY_SIZE(regs));
	if (rval < 0) {
		dev_err(dev, "Can't get phy regs offset (%d)\n", rval);
		return rval;
	}

	field.reg = regs[1];
	field.lsb = 16;
	field.msb = 31;
	host->phy_ctrl = devm_regmap_field_alloc(dev, host->reg_rct, field);
	if (IS_ERR(host->phy_ctrl)) {
		dev_err(dev, "Can't alloc phy ctrl reg field.\n");
		return PTR_ERR(host->phy_ctrl);
	}

	field.reg = regs[2];
	field.lsb = 0;
	field.msb = 24;
	host->phy_sel = devm_regmap_field_alloc(dev, host->reg_rct, field);
	if (IS_ERR(host->phy_sel)) {
		dev_err(dev, "Can't alloc phy sel reg field.\n");
		return PTR_ERR(host->phy_sel);
	}

	field.reg = regs[3];
	field.lsb = 0;
	field.msb = 15;
	host->phy_sbc = devm_regmap_field_alloc(dev, host->reg_rct, field);
	if (IS_ERR(host->phy_sbc)) {
		dev_err(dev, "Can't alloc phy sbc reg field.\n");
		return PTR_ERR(host->phy_sbc);
	}

	field.reg = regs[4];
	field.lsb = regs[5];
	field.msb = regs[5] + 4;
	host->phy_obsv = devm_regmap_field_alloc(dev, host->reg_rct, field);
	if (IS_ERR(host->phy_obsv)) {
		dev_err(dev, "Can't alloc phy obsv reg field.\n");
		return PTR_ERR(host->phy_obsv);
	}

	if (soc_device_match(ambarella_mmc_socinfo))
		emmc_slot = 2;
	else
		emmc_slot = 0;

	if(((poc & BOOT_MODE_MASK) == BOOT_MODE_EMMC) && (of_alias_get_id(np, "sd") == emmc_slot))
		host->emmc_boot_controller = true;
	else
		host->emmc_boot_controller = false;

	return 0;
}

#ifdef CONFIG_LEDS_CLASS
static void ambarella_sd_led_set_brightness(struct led_classdev *led_cdev, enum led_brightness value)
{
	struct ambarella_mmc_host *host = container_of(led_cdev, struct ambarella_mmc_host, led);

	if (gpio_is_valid(host->led_gpio)) {
		if(value)
			gpio_set_value_cansleep(host->led_gpio, !host->led_gpio_active);
		else
			gpio_set_value_cansleep(host->led_gpio, host->led_gpio_active);
	}
}

static int ambarella_sd_led_init(struct ambarella_mmc_host *host)
{
	int gpio_init_flag;
	int rval;
	enum of_gpio_flags flags;

	host->led_gpio = of_get_named_gpio_flags(host->dev->of_node, "led-gpios", 0, &flags);
	host->led_gpio_active = !!(flags & OF_GPIO_ACTIVE_LOW);
	if (gpio_is_valid(host->led_gpio)) {
		if (host->led_gpio_active)
			gpio_init_flag = GPIOF_OUT_INIT_LOW;
		else
			gpio_init_flag = GPIOF_OUT_INIT_HIGH;
	} else {
		return -ENODEV;
	}
	rval = devm_gpio_request_one(host->dev, host->led_gpio, gpio_init_flag, "sd led");
	if (rval < 0) {
		dev_err(host->dev, "Failed to request sd led-gpios!\n");
		return -ENODEV;
	}

	host->led.name = mmc_hostname(host->mmc);
	host->led.brightness = LED_OFF;
	host->led.default_trigger = mmc_hostname(host->mmc);
	host->led.brightness_set = ambarella_sd_led_set_brightness;
	rval = led_classdev_register(mmc_dev(host->mmc), &host->led);

	return rval;
}
#endif

static int ambarella_sd_probe(struct platform_device *pdev)
{
	struct ambarella_mmc_host *host;
	struct mmc_host *mmc;
	int rval;

	mmc = mmc_alloc_host(sizeof(struct ambarella_mmc_host), &pdev->dev);
	if (!mmc) {
		dev_err(&pdev->dev, "Failed to alloc mmc host!\n");
		return -ENOMEM;
	}

	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->dev = &pdev->dev;
	spin_lock_init(&host->lock);

	tasklet_init(&host->finish_tasklet,
		ambarella_sd_tasklet_finish, (unsigned long)host);

	host->desc_virt = dma_alloc_coherent(&pdev->dev, PAGE_SIZE,
				&host->desc_phys, GFP_KERNEL);
	if (!host->desc_virt) {
		dev_err(&pdev->dev, "Can't alloc DMA memory");
		rval = -ENOMEM;
		goto out0;
	}

	rval = ambarella_sd_get_resource(pdev, host);
	if (rval < 0)
		goto out1;

	rval = ambarella_sd_of_parse(host);
	if (rval < 0)
		goto out1;

	rval = ambarella_sd_init_hw(host);
	if (rval < 0)
		goto out1;

	timer_setup(&host->timer, ambarella_sd_timer_timeout, 0);

	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, 50);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	rval = mmc_add_host(mmc);
	if (rval < 0) {
		dev_err(&pdev->dev, "Can't add mmc host!\n");
		goto out2;
	}

	rval = devm_request_irq(&pdev->dev, host->irq, ambarella_sd_irq,
				IRQF_SHARED | IRQF_TRIGGER_HIGH,
				dev_name(&pdev->dev), host);
	if (rval < 0) {
		dev_err(&pdev->dev, "Can't Request IRQ%u!\n", host->irq);
		goto out3;
	}

	ambarella_sd_add_debugfs(host);

	platform_set_drvdata(pdev, host);
	dev_info(&pdev->dev, "Max frequency is %uHz\n", mmc->f_max);

	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_autosuspend(&pdev->dev);

#ifdef CONFIG_LEDS_CLASS
	if (host->have_led && ambarella_sd_led_init(host))
		goto out3;
#endif
	return 0;

out3:
	mmc_remove_host(mmc);
out2:
	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	del_timer_sync(&host->timer);
out1:
	dma_free_coherent(&pdev->dev, PAGE_SIZE, host->desc_virt, host->desc_phys);
out0:
	mmc_free_host(mmc);
	return rval;
}

static int ambarella_sd_remove(struct platform_device *pdev)
{
	struct ambarella_mmc_host *host = platform_get_drvdata(pdev);

#ifdef CONFIG_LEDS_CLASS
	if(host->have_led)
		led_classdev_unregister(&host->led);
#endif

	pm_runtime_get_sync(&pdev->dev);

	ambarella_sd_remove_debugfs(host);

	tasklet_kill(&host->finish_tasklet);

	del_timer_sync(&host->timer);

	mmc_remove_host(host->mmc);

	dma_free_coherent(&pdev->dev, PAGE_SIZE, host->desc_virt, host->desc_phys);

	mmc_free_host(host->mmc);

	pm_runtime_disable(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	dev_info(&pdev->dev, "Remove Ambarella SD/MMC Host Controller.\n");

	return 0;
}

#ifdef CONFIG_PM
static int ambarella_sd_suspend(struct device *dev)
{
	struct ambarella_mmc_host *host = dev_get_drvdata(dev);

	if(host->mmc->pm_caps & MMC_PM_KEEP_POWER) {
		ambarella_sd_disable_irq(host, SD_NISEN_CARD);
		host->sd_nisen = readw_relaxed(host->regbase + SD_NISEN_OFFSET);
		host->sd_eisen = readw_relaxed(host->regbase + SD_EISEN_OFFSET);
		host->sd_nixen = readw_relaxed(host->regbase + SD_NIXEN_OFFSET);
		host->sd_eixen = readw_relaxed(host->regbase + SD_EIXEN_OFFSET);
	}

	disable_irq(host->irq);

	return 0;

}

static int ambarella_sd_resume(struct device *dev)
{
	struct ambarella_mmc_host *host = dev_get_drvdata(dev);

	if (gpio_is_valid(host->pwr_gpio))
		gpio_direction_output(host->pwr_gpio, host->pwr_gpio_active);

	if(host->mmc->pm_caps & MMC_PM_KEEP_POWER) {
		writew_relaxed(host->sd_nisen, host->regbase + SD_NISEN_OFFSET);
		writew_relaxed(host->sd_eisen, host->regbase + SD_EISEN_OFFSET);
		writew_relaxed(host->sd_nixen, host->regbase + SD_NIXEN_OFFSET);
		writew_relaxed(host->sd_eixen, host->regbase + SD_EIXEN_OFFSET);
		host->mmc->caps |= MMC_CAP_NONREMOVABLE;
		mdelay(10);
		ambarella_sd_set_clk(host->mmc, host->clock);
		ambarella_sd_set_bus(host, host->bus_width, host->mode);
		mdelay(10);
		ambarella_sd_enable_irq(host, SD_NISEN_CARD);
	} else {
		clk_set_rate(host->clk, host->mmc->f_max);
		ambarella_sd_reset_all(host);
		ambarella_sd_set_bus(host, host->bus_width, host->mode);
	}

	enable_irq(host->irq);

	return 0;
}

static int ambarella_sd_runtime_suspend(struct device *dev)
{
	struct ambarella_mmc_host *host =  dev_get_drvdata(dev);

	ambarella_sd_switch_clk(host, false);

	return 0;
}

static int ambarella_sd_runtime_resume(struct device *dev)
{
	struct ambarella_mmc_host *host =  dev_get_drvdata(dev);

	ambarella_sd_switch_clk(host, true);

	return 0;
}
#endif

static const struct of_device_id ambarella_mmc_dt_ids[] = {
	{ .compatible = "ambarella,sdmmc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ambarella_mmc_dt_ids);

static const struct dev_pm_ops ambarella_mmc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ambarella_sd_suspend, ambarella_sd_resume)
	SET_RUNTIME_PM_OPS(ambarella_sd_runtime_suspend,
			   ambarella_sd_runtime_resume,
			   NULL)
};

static struct platform_driver ambarella_sd_driver = {
	.probe		= ambarella_sd_probe,
	.remove		= ambarella_sd_remove,
	.driver		= {
		.name	= "ambarella-sd",
		.of_match_table = ambarella_mmc_dt_ids,
		.pm = &ambarella_mmc_pm_ops,
	},
};

static int __init ambarella_sd_init(void)
{
	int rval = 0;

	rval = platform_driver_register(&ambarella_sd_driver);
	if (rval)
		printk(KERN_ERR "%s: Register failed %d!\n", __func__, rval);

	return rval;
}

static void __exit ambarella_sd_exit(void)
{
	platform_driver_unregister(&ambarella_sd_driver);
}

module_init(ambarella_sd_init);
module_exit(ambarella_sd_exit);

MODULE_DESCRIPTION("Ambarella Media Processor SD/MMC Host Controller");
MODULE_AUTHOR("Anthony Ginger, <hfjiang@ambarella.com>");
MODULE_LICENSE("GPL");

