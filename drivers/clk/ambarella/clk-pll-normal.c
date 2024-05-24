/*
 *
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

#include <linux/clkdev.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/of_address.h>
#include <linux/rational.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include "clk-pll-common.h"

static const struct amb_pll_soc_data pll_soc_data_v0 = {
	.fsout_mask	= 0x00000f00,
	.fsout_val	= 0x00000400,
	.fsdiv_mask	= 0x000000f0,
	.fsdiv_val	= 0x00000040,
	.vcodiv_mask	= 0x0000000f,
	.vcodiv_val	= 0x00000004,
	.vco_max_mhz	= 1600UL, /* RCT doc said 1.8GHz, but we use 1.6GHz for margin */
	.vco_min_mhz	= 700UL,
	.vco_range	= {980UL, 700UL, 530UL, 0UL},
};

static const struct amb_pll_soc_data pll_soc_data_v1 = {
	.fsout_mask	= CTRL2_FSOUT_DIV2,
	.fsout_val	= CTRL2_FSOUT_DIV2,
	.fsdiv_mask	= CTRL2_FSDIV_DIV2,
	.fsdiv_val	= CTRL2_FSDIV_DIV2,
	.vcodiv_mask	= CTRL2_VCODIV_DIV2,
	.vcodiv_val	= CTRL2_VCODIV_DIV2,
	.vco_max_mhz	= 2600UL, /* RCT doc said 2.8GHz, but we use 2.6GHz for margin */
	.vco_min_mhz	= 850UL,
	.vco_range	= {1800UL, 1400UL, 1100UL, 0UL},
};

static int ambarella_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate)
{
	struct amb_clk_pll *clk_pll = to_amb_clk_pll(hw);
	struct amb_pll_soc_data *soc_data = clk_pll->soc_data;
	struct regmap *map = clk_pll->pll_regmap;
	u32 *reg = clk_pll->reg_offset, ctrl_val, ctrl2_val, frac_val, vcodiv, fsdiv, fsout;
	unsigned long max_numerator, max_denominator;
	unsigned long rate_tmp, rate_resolution, pre_scaler = 1, post_scaler = 1;
	unsigned long intp, sdiv = 1, sout = 1, intp_tmp, sout_tmp;
	u64 dividend, divider, diff;

	if (rate == 0) {
		regmap_read(map, reg[CTRL_OFFSET], &ctrl_val);
		ctrl_val |= CTRL_POWER_DOWN | CTRL_HALT_VCO;
		rct_regmap_en(map, reg[CTRL_OFFSET], ctrl_val);
		return 0;
	}

	if (ambarella_pll_set_from_dts(clk_pll, "amb,val-regmap", rate) == 0)
		return 0;

	rate *= clk_pll->fix_divider;

	if (rate < parent_rate && reg[POST_OFFSET] != 0) {
		rate *= 16;
		post_scaler = 16;
	}

	if (rate < parent_rate) {
		pr_err("%s: Error: target rate is too slow: %ld!\n",
				clk_hw_get_name(hw), rate);
		return -EINVAL;
	}

retry:
	clk_pll->ctrl2_val &= ~soc_data->fsdiv_mask;

	if (rate >= 3000000000UL) {
		clk_pll->ctrl2_val |= soc_data->fsdiv_val;
		rate_tmp = rate / 2;
	} else {
		rate_tmp = rate;
	}

	if (clk_pll->ctrl2_val != 0)
		ctrl2_val = clk_pll->ctrl2_val;
	else
		regmap_read(map, reg[CTRL2_OFFSET], &ctrl2_val);

	vcodiv = ((ctrl2_val & soc_data->vcodiv_mask) == soc_data->vcodiv_val) ? 2 : 1;
	fsdiv = ((ctrl2_val & soc_data->fsdiv_mask) == soc_data->fsdiv_val) ? 2 : 1;
	fsout = ((ctrl2_val & soc_data->fsout_mask) == soc_data->fsout_val) ? 2 : 1;

	max_numerator = soc_data->vco_max_mhz / (REF_CLK_FREQ / 1000000UL) / vcodiv / fsdiv;
	max_numerator = min(128UL, max_numerator);
	max_denominator = 16;
	rational_best_approximation(rate_tmp, parent_rate,
			max_numerator, max_denominator, &intp, &sout);

	rate_resolution = parent_rate / post_scaler / 16;

	/*
	 * 10nm chips don't have negative fraction mode, so the calculated
	 * rate must be less than the required rate.
	 */
	while (parent_rate * fsdiv * intp * sdiv / fsout / sout > rate) {
		rate_tmp -= rate_resolution;
		rational_best_approximation(rate_tmp, parent_rate,
				max_numerator, max_denominator, &intp, &sout);
	}

	intp_tmp = intp;
	sout_tmp = sout;

	while (parent_rate / 1000000 * vcodiv * fsdiv * intp * sdiv /
		       pre_scaler < soc_data->vco_min_mhz) {
		if (sout > 8 || intp > 64) {
			if (reg[POST_OFFSET] != 0 && post_scaler == 1) {
				rate *= 16;
				post_scaler = 16;
				goto retry;
			}
			break;
		}
		intp += intp_tmp;
		sout += sout_tmp;
	}

	BUG_ON(intp > max_numerator || sout > max_denominator || sdiv > 16);
	BUG_ON(pre_scaler > 16 || post_scaler > 16);

	if (reg[PRES_OFFSET] != 0)
		rct_regmap_en(map, reg[PRES_OFFSET], (pre_scaler - 1) << 4);

	if (reg[POST_OFFSET] != 0)
		rct_regmap_en(map, reg[POST_OFFSET], (post_scaler - 1) << 4);

	ctrl_val = ((intp - 1) & 0x7f) << 24;
	ctrl_val |= ((sdiv - 1) & 0xf) << 12;
	ctrl_val |= ((sout - 1) & 0xf) << 16;
	regmap_write(map, reg[CTRL_OFFSET], ctrl_val);

	regmap_write(map, reg[FRAC_OFFSET], 0x0);

	ambarella_pll_set_ctrl2(clk_pll, 0x0);

	if (clk_pll->frac_mode) {
		rate_tmp = ambarella_pll_recalc_rate(hw, parent_rate);
		rate_tmp *= clk_pll->fix_divider * post_scaler;
		BUG_ON(rate_tmp > rate);

		diff = rate - rate_tmp;
		if (diff) {
			dividend = (diff * pre_scaler * sout * fsout) << 32;
			divider = (u64)sdiv * (u64)fsdiv * parent_rate;
			frac_val = DIV_ROUND_CLOSEST_ULL(dividend, divider);
			regmap_write(map, reg[FRAC_OFFSET], frac_val);

			ctrl_val |= CTRL_FRAC_MODE;
		}
	}

	ambarella_pll_set_ctrl3(clk_pll, parent_rate);

	/* critical PLL like cortex cannot be stopped when system is running */
	if (clk_pll->frac_mode) {
		ctrl_val |= CTRL_FORCE_RESET;
		rct_regmap_en(map, reg[CTRL_OFFSET], ctrl_val);
	}

	ctrl_val &= ~CTRL_FORCE_RESET;
	rct_regmap_en(map, reg[CTRL_OFFSET], ctrl_val);

	/* check if result rate is precise or not */
	rate_tmp = ambarella_pll_recalc_rate(hw, parent_rate);
	if (abs(rate_tmp - rate / clk_pll->fix_divider / post_scaler) > 10) {
		pr_warn("[Warning] %s: request %ld, but got %ld\n",
			clk_hw_get_name(hw),
			rate / clk_pll->fix_divider / post_scaler, rate_tmp);
	}

	return 0;
}

static const struct clk_ops pll_ops = {
	.recalc_rate = ambarella_pll_recalc_rate,
	.round_rate = ambarella_pll_round_rate,
	.set_rate = ambarella_pll_set_rate,
};

static void __init ambarella_pll_normal_clocks_init(struct device_node *np)
{
	struct device_node *parent_np = of_get_parent(np);
	const struct amb_pll_soc_data *soc_data = kzalloc(sizeof(struct amb_pll_soc_data), GFP_KERNEL);;

	if (of_device_is_compatible(parent_np, "ambarella,clkpll-v0"))
		memcpy((void *)soc_data, &pll_soc_data_v0, sizeof(struct amb_pll_soc_data));
	else
		memcpy((void *)soc_data, &pll_soc_data_v1, sizeof(struct amb_pll_soc_data));

	of_node_put(parent_np);

	ambarella_pll_clocks_init(np, &pll_ops, soc_data);
}
CLK_OF_DECLARE(ambarella_clk_pll, "ambarella,pll-clock", ambarella_pll_normal_clocks_init);

