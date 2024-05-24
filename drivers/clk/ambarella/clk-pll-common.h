/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 * Copyright (C) 2012-2016, Ambarella, Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#ifndef __AMBARELLA_PLL_COMMON_H
#define __AMBARELLA_PLL_COMMON_H

#include <linux/clk.h>
#include <linux/regmap.h>

#define REF_CLK_FREQ				24000000UL

#define CTRL_WRITE_ENABLE			BIT(0)
#define CTRL_BYPASS				BIT(2)
#define CTRL_FRAC_MODE				BIT(3)
#define CTRL_FORCE_RESET			BIT(4)
#define CTRL_POWER_DOWN				BIT(5)
#define CTRL_HALT_VCO				BIT(6)

#define CTRL2_VCODIV_DIV2			BIT(8)
#define CTRL2_FSDIV_DIV2			BIT(9)
#define CTRL2_FSOUT_DIV2			BIT(11)
#define CTRL2_BYPASS_HSDIV			BIT(12)

#define CTRL3_VCO_RANGE_MASK			(0x6)

/* ==========================================================================*/

struct amb_pll_soc_data {
	u32 fsout_mask;
	u32 fsout_val;
	u32 fsdiv_mask;
	u32 fsdiv_val;
	u32 vcodiv_mask;
	u32 vcodiv_val;
	u32 vco_max_mhz;
	u32 vco_min_mhz;
	u32 vco_range[4];
};

enum {
	CTRL_OFFSET = 0,
	FRAC_OFFSET,
	CTRL2_OFFSET,
	CTRL3_OFFSET,
	PRES_OFFSET,
	POST_OFFSET,
	REG_NUM,
};

struct amb_clk_pll {
	struct clk_hw hw;
	struct regmap *pll_regmap;
	u32 reg_offset[REG_NUM];
	u32 frac_mode : 1;
	u32 ctrl2_val;
	u32 ctrl3_val;
	u32 fix_divider;
	struct amb_pll_soc_data *soc_data;
	struct device_node *np;
};

#define to_amb_clk_pll(_hw) container_of(_hw, struct amb_clk_pll, hw)

#define rct_regmap_en(r, o, v)                             \
	do {                                               \
		regmap_write(r, o, v);                     \
		regmap_write(r, o, v | CTRL_WRITE_ENABLE); \
		regmap_write(r, o, v);                     \
	} while (0)

/* ==========================================================================*/

void __init ambarella_pll_clocks_init(struct device_node *np, const struct clk_ops *pll_ops,
		const struct amb_pll_soc_data *soc_data);
unsigned long ambarella_pll_calc_vco(struct amb_clk_pll *clk_pll, unsigned long parent_rate);
unsigned long ambarella_pll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate);
long ambarella_pll_round_rate(struct clk_hw *hw, unsigned long rate, unsigned long *parent_rate);
void ambarella_pll_set_ctrl2(struct amb_clk_pll *clk_pll, unsigned int ctrl2_val);
void ambarella_pll_set_ctrl3(struct amb_clk_pll *clk_pll, unsigned long parent_rate);
int ambarella_pll_set_from_dts(struct amb_clk_pll *clk_pll,
			char *prop_name, unsigned long rate);

#endif

