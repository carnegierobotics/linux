// SPDX-License-Identifier: GPL-2.0+
/*
 * Author: Anthony Ginger <hfjiang@ambarella.com>
 *
 * Copyright (C) 2004-2010, Ambarella, Inc.
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
#include <linux/list.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/sys_soc.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/uaccess.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <soc/ambarella/misc.h>
#include "clk-pll-common.h"

#define RCT_CLOCK_OBSV_REG		0x1e0

static u32 rct_clock_obsv_enable;

static const char * const gclk_names[] = {
	"pll_out_core", "pll_out_sd", "pll_out_hdmi", "pll_out_vo2", "pll_out_enet",
	"pll_out_video_a", "pll_out_video_b", "gclk_cortex", "gclk_cortex0", "gclk_cortex1",
	"gclk_axi", "gclk_dsu", "smp_twd", "gclk_ddr", "gclk_ddr0", "gclk_ddr1", "gclk_core",
	"gclk_ahb", "gclk_apb", "gclk_idsp", "gclk_idspv", "gclk_so", "gclk_so2", "gclk_vo2",
	"gclk_vo", "gclk_vo_a", "gclk_vo_b", "gclk_nand", "gclk_sdxc", "gclk_sdio", "gclk_sd",
	"gclk_sd0", "gclk_sd1", "gclk_sd2", "gclk_uart", "gclk_uart0", "gclk_uart1", "gclk_uart2",
	"gclk_uart3", "gclk_uart4", "gclk_audio", "gclk_audio2", "gclk_audio3", "gclk_audio_aux",
	"gclk_ir", "gclk_adc", "gclk_ssi", "gclk_ssi2", "gclk_ssi3", "gclk_pwm", "gclk_stereo",
	"gclk_vision", "gclk_nvp", "gclk_gvp", "gclk_fex", "pll_out_slvsec",
	"gclk_fma", "gclk_hsm", "gclk_core_dsp", "gclk_gpu", "gclk_vdsp",
};

static int ambarella_clock_proc_show(struct seq_file *m, void *v)
{
	struct clk *gclk;
	int i;

	seq_puts(m, "\nClock Information:\n");
	for (i = 0; i < ARRAY_SIZE(gclk_names); i++) {
		gclk = clk_get_sys(NULL, gclk_names[i]);
		if (IS_ERR(gclk))
			continue;

		seq_printf(m, "\t%s:\t%ld Hz\n",
			__clk_get_name(gclk), clk_get_rate(gclk));

		clk_put(gclk);
	}

	return 0;
}

static ssize_t ambarella_clock_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	struct clk *gclk;
	char *buf, clk_name[32];
	unsigned long freq;
	int rval = count;

	pr_warn("!!!DANGEROUS!!! You must know what you are doning!\n");

	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, buffer, count)) {
		rval = -EFAULT;
		goto exit;
	}

	sscanf(buf, "%s %ld", clk_name, &freq);

	gclk = clk_get_sys(NULL, clk_name);
	if (IS_ERR(gclk)) {
		pr_err("Invalid clk name\n");
		rval = -EINVAL;
		goto exit;
	}

	clk_set_rate(gclk, freq);
	clk_put(gclk);
exit:
	kfree(buf);
	return rval;
}

static int ambarella_clock_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambarella_clock_proc_show, PDE_DATA(inode));
}

static const struct proc_ops proc_clock_fops = {
	.proc_open = ambarella_clock_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_write = ambarella_clock_proc_write,
	.proc_release = single_release,
};

/* ==========================================================================*/

static int ambarella_clock_debug_show(struct seq_file *s, void *p)
{
	seq_puts(s, "Usage:\n");
	seq_puts(s, "    a. Change clock rate: echo CLOCK RATE_IN_HZ > clock\n");
	seq_puts(s, "    b. Observe clock:     echo CLOCK obsv_on/obsv_off > clock\n");
	seq_puts(s, "    c. Show clock info:   echo CLOCK info > clock\n");
	seq_puts(s, "\n");

	seq_puts(s, "Note:\n");
	seq_puts(s, "    a. Change clock rate by debugfs is DANGEROUS, it's for DEBUG purpose only\n");
	seq_puts(s, "    b. Clock is observed on pin CLK_SI at 1/16th or 1/32th of real frequency\n");
	seq_puts(s, "    b. Clock info includes VCO frequency and related register offset/value\n");
	seq_puts(s, "\n");

	return 0;
}

static int ambarella_clock_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambarella_clock_debug_show, inode->i_private);
}

static ssize_t ambarella_clock_debug_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	struct clk *gclk;
	struct regmap *map;
	char *buf, clk_name[32];
	unsigned long freq = -1UL;
	u32 show_info = 0, obsv = -1U, obsv_id;
	int rval = count;

	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, buffer, count)) {
		rval = -EFAULT;
		goto exit;
	}

	if (sscanf(buf, "%s %ld", clk_name, &freq) != 2) {
		if (strnstr(buf, "info", count))
			show_info = 1;
		else if (strnstr(buf, "obsv_on", count))
			obsv = 1;
		else if (strnstr(buf, "obsv_off", count))
			obsv = 0;
		else
			pr_err("Invalid argument\n");
	}

	gclk = clk_get_sys(NULL, clk_name);
	if (IS_ERR(gclk)) {
		pr_err("Invalid clk name\n");
		goto exit;
	}

	if (show_info) {
		struct amb_clk_pll *clk_pll = to_amb_clk_pll(__clk_get_hw(gclk));
		unsigned long rate, fvco;
		u32 *reg, val[REG_NUM], i;

		map = clk_pll->pll_regmap;
		if (!map) {
			pr_err("info command only supports pll\n");
			return -EINVAL;
		}
		reg = clk_pll->reg_offset;

		for (i = 0; i < ARRAY_SIZE(val); i++) {
			if (i == PRES_OFFSET && reg[PRES_OFFSET] == 0)
				continue;
			if (i == POST_OFFSET && reg[POST_OFFSET] == 0)
				continue;
			regmap_read(map, reg[i], &val[i]);
		}

		rate = clk_get_rate(gclk);
		fvco = ambarella_pll_calc_vco(clk_pll, REF_CLK_FREQ);

		pr_info("%s: rate = %ld.%ldMhz, fvco = %ld.%ldMhz\n", __clk_get_name(gclk),
			rate / 1000000UL, rate % 1000000UL, fvco / 1000000UL, fvco % 1000000UL);

		pr_info("    CTRL:  0x%03x, 0x%08x\n", reg[CTRL_OFFSET], val[CTRL_OFFSET]);
		pr_info("    FRAC:  0x%03x, 0x%08x\n", reg[FRAC_OFFSET], val[FRAC_OFFSET]);
		pr_info("    CTRL2: 0x%03x, 0x%08x\n", reg[CTRL2_OFFSET], val[CTRL2_OFFSET]);
		pr_info("    CTRL3: 0x%03x, 0x%08x\n", reg[CTRL3_OFFSET], val[CTRL3_OFFSET]);

		if (reg[PRES_OFFSET] != 0)
			pr_info("    PRES:  0x%03x, 0x%08x\n", reg[PRES_OFFSET], val[PRES_OFFSET]);

		if (reg[POST_OFFSET] != 0)
			pr_info("    POST:  0x%03x, 0x%08x\n", reg[POST_OFFSET], val[POST_OFFSET]);

	} else if (obsv != -1U) {
		struct device_node *np = NULL;
		const char *_clk_name;

		map = syscon_regmap_lookup_by_compatible("ambarella,rct");
		if (IS_ERR(map)) {
			pr_err("No rct syscon regmap\n");
			goto exit;
		}

		for_each_compatible_node(np, NULL, "ambarella,pll-clock") {
			if (of_property_read_string(np, "clock-output-names", &_clk_name))
				continue;

			if (strcmp(_clk_name, clk_name) == 0)
				break;
		}

		if (np == NULL || of_property_read_u32(np, "amb,obsv-id", &obsv_id) < 0) {
			pr_err("No such observable pll\n");
			goto exit;
		}

		pr_info("%s %s observation on clk_si pin\n", obsv ? "Enable" : "Disable", clk_name);

		if (obsv)
			regmap_write(map, RCT_CLOCK_OBSV_REG, rct_clock_obsv_enable | obsv_id);
		else
			regmap_write(map, RCT_CLOCK_OBSV_REG, 0x0);

	} else if (freq != -1UL) {
		pr_warn("!!!DANGEROUS!!! You must know what you are doning!\n");
		clk_set_rate(gclk, freq);
	}

exit:
	if (!IS_ERR(gclk))
		clk_put(gclk);
	kfree(buf);

	return rval;
}

static const struct file_operations debug_clock_fops = {
	.open = ambarella_clock_debug_open,
	.write = ambarella_clock_debug_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.owner = THIS_MODULE,
};

/* ==========================================================================*/

static const struct soc_device_attribute ambarella_clk_socinfo[] = {
	{ .family = "Ambarella 10nm" },
	{ .soc_id = "cv5" },
	{/* sentinel */}
};

static int __init ambarella_init_clk(void)
{
	proc_create_data("clock", 0444,
			ambarella_procfs_dir(), &proc_clock_fops, NULL);

	debugfs_create_file("clock", 0644,
			ambarella_debugfs_dir(), NULL, &debug_clock_fops);

	if (soc_device_match(ambarella_clk_socinfo))
		rct_clock_obsv_enable = BIT(5);
	else
		rct_clock_obsv_enable = BIT(16);

	return 0;
}

late_initcall(ambarella_init_clk);

