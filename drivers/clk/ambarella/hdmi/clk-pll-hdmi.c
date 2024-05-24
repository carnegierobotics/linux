/*
 *
 * Author: Bingliang <blhu@ambarella.com>
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
#include "clk-pll-hdmi.h"

u32 pll_reg_rd(struct hdmi_pll_info *info, u32 reg_offset)
{
	u32 val;
	regmap_read(info->pll_regmap, info->reg_offset[reg_offset], &val);
	return val;
}

void pll_reg_wr(struct hdmi_pll_info *info, u32 reg_offset, u32 val)
{
	regmap_write(info->pll_regmap, info->reg_offset[reg_offset], val);
}

/*
 * same for clkpll-v0/v1 and s5l
 */
static int hdmi_pll_shutdown(struct hdmi_pll_info *info)
{
	CTRL_REG_U ctrl_reg;

	ctrl_reg.val = pll_reg_rd(info, CTRL_OFFSET);
	ctrl_reg.reg.power_down = 1;
	ctrl_reg.reg.vco_halt = 1;
	ctrl_reg.reg.pll_we = 1;
	pll_reg_wr(info, CTRL_OFFSET, ctrl_reg.val);
	ctrl_reg.reg.pll_we = 0;
	pll_reg_wr(info, CTRL_OFFSET, ctrl_reg.val);

	return 0;
}

static void hdmi_pll_version(struct device_node *np,
			     struct hdmi_pll_info *info)
{
	struct device_node *parent_np = of_get_parent(np);

	if (of_device_is_compatible(parent_np, "ambarella,clkpll-v0")) {
		if (!!of_find_property(np, "amb,pll-14nm", NULL)) {
			info->pll_version = HDMI_PLL_14NM;
		} else {
			info->pll_version = HDMI_PLL_10NM;
		}
	} else if (of_device_is_compatible(parent_np, "ambarella,clkpll-v1")) {
		info->pll_version = HDMI_PLL_05NM;
	} else {
		pr_info("invalid hdmi pll version, use defalut 05nm\n");
		info->pll_version = HDMI_PLL_05NM;
	}

	of_node_put(parent_np);

	pr_info("hdmi pll_version %d\n", info->pll_version);
}

static int hdmi_pll_of_parse(struct hdmi_pll_info *info)
{
	u32 num = 0;
	u32 buf[REG_MAX_NUM + 1];
	u32 assigned_rate = 0;
	struct device_node *np = info->np;

	hdmi_pll_version(np, info);

	info->frac_mode = !!of_find_property(np, "amb,frac-mode", NULL);
	if (info->frac_mode != 1) {
		pr_err("hdmi pll need frac mode\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "amb,vco-min-mhz", &info->vco_min_mhz)) {
		pr_err("hdmi pll need vco-min-mhz in dts\n");
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "amb,vco-range", info->vco_range, NR_VCO)) {
		pr_err("hdmi pll need %d vco range in dts\n", NR_VCO);
		return -EINVAL;
	}
	info->vco_max_mhz = info->vco_range[4];

	if (of_property_read_u32(np, "amb,clk-min-rate", &info->clk_min_rate)) {
		info->clk_min_rate = 0;
	}

	if (of_property_read_u32(np, "amb,ctrl2-val", &info->ctrl2_val)) {
		pr_err("hdmi pll need ctrl2 val in dts\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "amb,ctrl3-val", &info->ctrl3_val)) {
		pr_err("hdmi pll need ctrl3 val in dts\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "amb,fix-divider", &info->fix_divider))
		info->fix_divider = 1;

	info->pll_regmap = syscon_regmap_lookup_by_phandle(np, "amb,clk-regmap");
	if (IS_ERR(info->pll_regmap)) {
		pr_err("%s: failed to get pll regmap\n", np->name);
		return -EINVAL;
	}

	num = of_property_count_elems_of_size(np, "amb,clk-regmap", sizeof(u32));
	if ((num - 1) > REG_MAX_NUM) {
		pr_err("%s: clk-regmap elements number %d is wrong\n", np->name, num);
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "amb,clk-regmap", buf, num)) {
		pr_err("%s: failed to get pll reg offset\n", np->name);
		return -EINVAL;
	}

	memcpy(info->reg_offset, &buf[1], 4 * (num - 1));

	info->parent_name = of_clk_get_parent_name(np, 0);

	if (of_property_read_string(np, "clock-output-names", &info->name))
		info->name = np->name;

	/* shut down pll if assigned-clock-rate is 0 */
	if (!of_property_read_u32(np, "assigned-clock-rates", &assigned_rate)) {
		if (assigned_rate == 0)
			hdmi_pll_shutdown(info);
	}

	return 0;
}

static void hdmi_pll_dts_set_reg(struct hdmi_pll_info *info, u32 *reg_val, int num)
{
	int i = 0;
	u32 *reg = info->reg_offset;

	for (i = 0; i < num; i++) {
		switch (i) {
		case PRES_OFFSET:
		case POST_OFFSET:
			if (reg[i] != 0) {
				pll_reg_wr(info, i, reg_val[i] << 4);
				pll_reg_wr(info, i, 1 | (reg_val[i] << 4));
				pll_reg_wr(info, i, reg_val[i] << 4);
			}
			break;
		case CTRL_OFFSET:
			pll_reg_wr(info, i, reg_val[i]);
			pll_reg_wr(info, i, 1 | reg_val[i]);
			pll_reg_wr(info, i, reg_val[i]);
			break;
		default:
			pll_reg_wr(info, i, reg_val[i]);
			break;
		}
	}
}

static int hdmi_pll_set_from_dts(struct hdmi_pll_info *info,
				 char *reg_name, char *val_name,
				 unsigned long rate)
{
	int rval = 0, reg_num = 0, val_num = 0, clk_num = 0;
	u32 i, j;
	struct device_node *np = info->np;
	u32 reg_val[REG_MAX_NUM + 1] = {0};

	/* Check property */
	if (of_find_property(np, val_name, NULL) == NULL) {
		return -1;
	}

	/* check register number and value number, should be same */
	reg_num = of_property_count_elems_of_size(np, reg_name, sizeof(u32));
	val_num = of_property_count_elems_of_size(np, val_name, sizeof(u32));
	/* get clock setting numbert */
	clk_num = of_property_count_elems_of_size(np, val_name,
						  reg_num * sizeof(u32));
	if (clk_num <= 0) {
		pr_err("wrong clk num\n");
		return -1;
	}

	if ((val_num / clk_num) != reg_num) {
		pr_err("wrong elements number in %s and %s\n", reg_name, val_name);
		return -1;
	}

	for (i = 0; i < clk_num; i++) {
		/* read clock setting value */
		for (j = 0; j < reg_num; j++) {
			rval = of_property_read_u32_index(np, val_name,
							  j + i * reg_num,
							  &reg_val[j]);
			if (rval) {
				pr_err("failed to get clk reg val\n");
				return -1;
			}
		}

		/* write clock register */
		if (reg_val[0] == rate) {
			hdmi_pll_dts_set_reg(info, &reg_val[1], reg_num - 1);
			break;
		}
	}

	if (i == clk_num) {
		rval = -1;
	}

	return rval;
}

static unsigned long ambarella_hdmi_pll_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{

	if ((to_hdmi_pll_info(hw)->pll_version == HDMI_PLL_10NM) ||
	    (to_hdmi_pll_info(hw)->pll_version == HDMI_PLL_05NM))
		return hdmi_pll_recalc_rate(hw, parent_rate);
	else if (to_hdmi_pll_info(hw)->pll_version == HDMI_PLL_14NM)
		return hdmi_pll_recalc_rate_14nm(hw, parent_rate);
	return 0;
}

static long ambarella_hdmi_pll_round_rate(struct clk_hw *hw, unsigned long rate,
					  unsigned long *parent_rate)
{
	return rate;
}

static int ambarella_hdmi_pll_set_rate(struct clk_hw *hw,
				       unsigned long rate,
				       unsigned long parent_rate)
{
	int ret = 0;
	struct hdmi_pll_info *info = to_hdmi_pll_info(hw);

	if (rate == 0) {
		hdmi_pll_shutdown(info);
		return 0;
	}

	ret = hdmi_pll_set_from_dts(info, "amb,clk-regmap", "amb,val-regmap", rate);
	if (ret == 0)
		return ret;

	rate *= info->fix_divider;

	if ((to_hdmi_pll_info(hw)->pll_version == HDMI_PLL_10NM) ||
	    (to_hdmi_pll_info(hw)->pll_version == HDMI_PLL_05NM)) {
		ret = hdmi_pll_set_rate(hw, rate, parent_rate);
	} else if (info->pll_version == HDMI_PLL_14NM) {
		ret = hdmi_pll_set_rate_14nm(hw, rate, parent_rate);
	}

	return ret;
}

void __init ambarella_hdmi_pll_init(struct device_node *np,
				    const struct clk_ops *pll_ops)
{
	struct hdmi_pll_info *info;
	struct clk_init_data init;
	struct clk *clk;
	int num_parents = of_clk_get_parent_count(np);

	if (num_parents < 1) {
		pr_err("%s: no parent found\n", np->name);
		return;
	}

	info = kzalloc(sizeof(struct hdmi_pll_info), GFP_KERNEL);
	if (info == NULL) {
		pr_err("%s no memory\n", np->name);
		return;
	}
	info->np = np;

	if (hdmi_pll_of_parse(info))
		return;

	init.name = info->name;
	init.ops = pll_ops;
	init.flags = CLK_GET_RATE_NOCACHE;
	init.parent_names = &info->parent_name;
	init.num_parents = num_parents;
	info->hw.init = &init;
	clk = clk_register(NULL, &info->hw);
	if (IS_ERR(clk)) {
		pr_err("%s: failed to register %s pll clock (%ld)\n",
		       np->name, info->name, PTR_ERR(clk));
		kfree(info);
		return;
	}

	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	clk_register_clkdev(clk, info->name, NULL);

}

static const struct clk_ops ambarella_hdmi_pll_ops = {
	.recalc_rate = ambarella_hdmi_pll_recalc_rate,
	.round_rate = ambarella_hdmi_pll_round_rate,
	.set_rate = ambarella_hdmi_pll_set_rate,
};

static void __init ambarella_pll_hdmi_clocks_init(struct device_node *np)
{
	ambarella_hdmi_pll_init(np, &ambarella_hdmi_pll_ops);
}

CLK_OF_DECLARE(ambarella_clk_pll_hdmi,
	       "ambarella,pll-hdmi-clock",
	       ambarella_pll_hdmi_clocks_init);
