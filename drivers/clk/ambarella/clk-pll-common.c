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

#include <linux/init.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include "clk-pll-common.h"

static void ambarella_pll_write_reg(struct amb_clk_pll *clk_pll,
				    u32 *reg_val,
				    int reg_num)
{
	int i = 0;
	struct regmap *map = clk_pll->pll_regmap;
	u32 *reg = clk_pll->reg_offset;

	for (i = CTRL_OFFSET; i < reg_num; i++) {
		switch (i) {
		case PRES_OFFSET:
		case POST_OFFSET:
			if (reg[i] != 0)
				rct_regmap_en(map, reg[i], (reg_val[i] - 1) << 4);
			break;
		case CTRL_OFFSET:
			rct_regmap_en(map, reg[i], reg_val[i]);
			break;
		default:
			regmap_write(map, reg[i], reg_val[i]);
			break;
		}
	}
}


int ambarella_pll_set_from_dts(struct amb_clk_pll *clk_pll,
			char *prop_name, unsigned long rate)
{
	int rval = -1, reg_num = 0, val_num = 0, clk_num = 0;
	u32 i, j;
	struct device_node *np = clk_pll->np;
	u32 reg_val[REG_NUM] = {0};

	/* Check property */
	if (of_find_property(np, prop_name, NULL) == NULL)
		return -1;

	/* Check register number and value number, should be same */
	reg_num = of_property_count_elems_of_size(np, "amb,clk-regmap", sizeof(u32));
	val_num = of_property_count_elems_of_size(np, prop_name, sizeof(u32));
	if (val_num % reg_num) {
		pr_err("%s: please use same elements number in amb,clk-regmap and %s\n",
		       np->name, prop_name);
		rval = -1;
		goto END;
	}

	/* Get clock setting numbert */
	clk_num = of_property_count_elems_of_size(np, prop_name,
						  reg_num * sizeof(u32));
	if (clk_num <= 0) {
		pr_err("%s: failed to get reg value in set-from-dts\n", np->name);
		rval = -1;
		goto END;
	}

	for (i = 0; i < clk_num; i++) {
		/* Read clock setting value */
		for (j = 0; j < reg_num; j++) {
			rval = of_property_read_u32_index(np, prop_name,
							  j + i * reg_num,
							  &reg_val[j]);
			if (rval) {
				pr_err("%s: failed to get clk set val\n",
				       np->name);
				goto END;
			}
		}

		/* Write clock register */
		if (reg_val[0] == rate) {
			ambarella_pll_write_reg(clk_pll, reg_val, reg_num);
			break;
		}

		continue;
	}

	if (i == clk_num)
		rval = -1;

END:
	return rval;
}

void ambarella_pll_set_ctrl2(struct amb_clk_pll *clk_pll, u32 ctrl2_val)
{
	if (clk_pll->ctrl2_val != 0)
		ctrl2_val = clk_pll->ctrl2_val;
	else if (ctrl2_val == 0)
		regmap_read(clk_pll->pll_regmap, clk_pll->reg_offset[CTRL2_OFFSET], &ctrl2_val);

	regmap_write(clk_pll->pll_regmap, clk_pll->reg_offset[CTRL2_OFFSET], ctrl2_val);
}

void ambarella_pll_set_ctrl3(struct amb_clk_pll *clk_pll, unsigned long parent_rate)
{
	struct amb_pll_soc_data *soc_data = clk_pll->soc_data;
	u32 fvco_mhz, range, ctrl3_val = clk_pll->ctrl3_val;

	if (ctrl3_val != 0) {
		regmap_write(clk_pll->pll_regmap, clk_pll->reg_offset[CTRL3_OFFSET], ctrl3_val);
		return;
	}

	fvco_mhz = ambarella_pll_calc_vco(clk_pll, parent_rate) / 1000000UL;

	for (range = 0; range < ARRAY_SIZE(soc_data->vco_range); range++) {
		if (fvco_mhz > soc_data->vco_range[range])
			break;
	}
	range = ARRAY_SIZE(soc_data->vco_range) - range - 1;

	regmap_read(clk_pll->pll_regmap, clk_pll->reg_offset[CTRL3_OFFSET], &ctrl3_val);
	ctrl3_val &= ~CTRL3_VCO_RANGE_MASK;
	ctrl3_val |= range << 1;
	regmap_write(clk_pll->pll_regmap, clk_pll->reg_offset[CTRL3_OFFSET], ctrl3_val);
}

unsigned long ambarella_pll_calc_vco(struct amb_clk_pll *clk_pll, unsigned long parent_rate)
{
	struct amb_pll_soc_data *soc_data = clk_pll->soc_data;
	u32 *reg = clk_pll->reg_offset, pre_scaler = 1, ctrl_val, ctrl2_val, frac_val;
	u32 intp, sdiv, sout, vcodiv, fsdiv, frac = 0UL;

	if (reg[PRES_OFFSET] != 0) {
		regmap_read(clk_pll->pll_regmap, reg[PRES_OFFSET], &pre_scaler);
		pre_scaler >>= 4;
		pre_scaler++;
	}

	regmap_read(clk_pll->pll_regmap, reg[CTRL_OFFSET], &ctrl_val);
	intp = ((ctrl_val >> 24) & 0x7f) + 1;
	sdiv = ((ctrl_val >> 12) & 0xf) + 1;
	sout = ((ctrl_val >> 16) & 0xf) + 1;

	regmap_read(clk_pll->pll_regmap, reg[CTRL2_OFFSET], &ctrl2_val);
	vcodiv = ((ctrl2_val & soc_data->vcodiv_mask) == soc_data->vcodiv_val) ? 2 : 1;
	fsdiv = ((ctrl2_val & soc_data->fsdiv_mask) == soc_data->fsdiv_val) ? 2 : 1;

	if (ctrl_val & CTRL_FRAC_MODE) {
		regmap_read(clk_pll->pll_regmap, reg[FRAC_OFFSET], &frac_val);
		frac = (parent_rate / pre_scaler * vcodiv * sdiv * fsdiv * frac_val) >> 32;
	}

	return parent_rate / pre_scaler * vcodiv * fsdiv * intp * sdiv + frac;
}

long ambarella_pll_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	u32 half_refclk = REF_CLK_FREQ / 2;

	if (to_amb_clk_pll(hw)->frac_mode)
		return rate;
	else
		return roundup(rate, half_refclk);
}

unsigned long ambarella_pll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct amb_clk_pll *clk_pll = to_amb_clk_pll(hw);
	struct amb_pll_soc_data *soc_data = clk_pll->soc_data;
	u32 *reg = clk_pll->reg_offset, ctrl_val, ctrl2_val;
	u32 pre_scaler = 1, post_scaler = 1, vcodiv, fsout, sout;
	u64 fvco, rate;

	regmap_read(clk_pll->pll_regmap, reg[CTRL_OFFSET], &ctrl_val);
	regmap_read(clk_pll->pll_regmap, reg[CTRL2_OFFSET], &ctrl2_val);

	if (ctrl_val & (CTRL_POWER_DOWN | CTRL_HALT_VCO))
		return 0;

	if (reg[PRES_OFFSET] != 0) {
		regmap_read(clk_pll->pll_regmap, reg[PRES_OFFSET], &pre_scaler);
		pre_scaler >>= 4;
		pre_scaler++;
	}

	if (reg[POST_OFFSET] != 0) {
		regmap_read(clk_pll->pll_regmap, reg[POST_OFFSET], &post_scaler);
		post_scaler >>= 4;
		post_scaler++;
	}

	if (ctrl_val & (CTRL_BYPASS | CTRL_FORCE_RESET))
		return parent_rate / pre_scaler / post_scaler;

	vcodiv = ((ctrl2_val & soc_data->vcodiv_mask) == soc_data->vcodiv_val) ? 2 : 1;
	fsout = ((ctrl2_val & soc_data->fsout_mask) == soc_data->fsout_val) ? 2 : 1;
	sout = ((ctrl_val >> 16) & 0xf) + 1;

	fvco = ambarella_pll_calc_vco(clk_pll, parent_rate);

	if (ctrl2_val & CTRL2_BYPASS_HSDIV)
		rate = fvco;
	else
		rate = fvco / vcodiv / fsout / sout;

	return rate / clk_pll->fix_divider / post_scaler;
}

static void ambarella_pll_of_parse(struct amb_clk_pll *clk_pll, struct device_node *np)
{
	u32 vco_limit;

	clk_pll->frac_mode = !!of_find_property(np, "amb,frac-mode", NULL);

	if (of_property_read_u32(np, "amb,ctrl2-val", &clk_pll->ctrl2_val))
		clk_pll->ctrl2_val = 0;

	if (of_property_read_u32(np, "amb,ctrl3-val", &clk_pll->ctrl3_val))
		clk_pll->ctrl3_val = 0;

	if (of_property_read_u32(np, "amb,fix-divider", &clk_pll->fix_divider))
		clk_pll->fix_divider = 1;

	if (of_property_read_u32(np, "amb,vco-max-mhz", &vco_limit) == 0)
		clk_pll->soc_data->vco_max_mhz = vco_limit;

	if (of_property_read_u32(np, "amb,vco-min-mhz", &vco_limit) == 0)
		clk_pll->soc_data->vco_min_mhz = vco_limit;
}

void __init ambarella_pll_clocks_init(struct device_node *np, const struct clk_ops *pll_ops,
		const struct amb_pll_soc_data *soc_data)
{
	struct amb_clk_pll *clk_pll;
	struct clk *clk;
	struct clk_init_data init;
	const char *name, *parent_name;
	int num_parents;

	num_parents = of_clk_get_parent_count(np);
	if (num_parents < 1) {
		pr_err("%s: no parent found\n", np->name);
		return;
	}

	clk_pll = kzalloc(sizeof(struct amb_clk_pll), GFP_KERNEL);
	if (!clk_pll)
		return;

	clk_pll->pll_regmap = syscon_regmap_lookup_by_phandle_args(np, "amb,clk-regmap",
					ARRAY_SIZE(clk_pll->reg_offset), clk_pll->reg_offset);
	if (IS_ERR(clk_pll->pll_regmap)) {
		pr_err("%s: failed to get pll regmap\n", np->name);
		return;
	}

	clk_pll->soc_data = (struct amb_pll_soc_data *)soc_data;

	ambarella_pll_of_parse(clk_pll, np);

	if (of_property_read_string(np, "clock-output-names", &name))
		name = np->name;

	clk_pll->np = np;

	parent_name = of_clk_get_parent_name(np, 0);

	init.name = name;
	init.ops = pll_ops;
	init.flags = CLK_GET_RATE_NOCACHE;
	init.parent_names = &parent_name;
	init.num_parents = num_parents;
	clk_pll->hw.init = &init;

	clk = clk_register(NULL, &clk_pll->hw);
	if (IS_ERR(clk)) {
		pr_err("%s: failed to register %s pll clock (%ld)\n",
		       __func__, name, PTR_ERR(clk));
		kfree(clk_pll);
		return;
	}

	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	clk_register_clkdev(clk, name, NULL);
}

