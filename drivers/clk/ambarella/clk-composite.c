// SPDX-License-Identifier: GPL-2.0+
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
#include <linux/err.h>
#include <linux/init.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#define to_clk_mux_regmap(_hw) container_of(_hw, struct clk_mux_regmap, hw)
#define to_clk_div_regmap(_hw) container_of(_hw, struct clk_div_regmap, hw)

struct clk_mux_regmap {
	struct clk_hw hw;
	struct regmap *map;
	u32 offset;
	u32 mask;
	u32 shift;
	u32 flags;
};

struct clk_div_regmap {
	struct clk_hw hw;
	struct regmap *map;
	u32 offset;
	u32 shift;
	u32 width;
	u32 flags;
	u32 fix_divider;
};

static u8 clk_regmap_mux_get_parent(struct clk_hw *hw)
{
	struct clk_mux_regmap *mux = to_clk_mux_regmap(hw);
	u32 val;

	regmap_read(mux->map, mux->offset, &val);

	val >>= mux->shift;
	val &= mux->mask;

	return clk_mux_val_to_index(hw, NULL, mux->flags, val);
}

static int clk_regmap_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_mux_regmap *mux = to_clk_mux_regmap(hw);
	u32 val = clk_mux_index_to_val(NULL, mux->flags, index);

	return regmap_update_bits(mux->map, mux->offset,
				  mux->mask << mux->shift,
				  val << mux->shift);
}

static int clk_regmap_mux_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	struct clk_mux_regmap *mux = to_clk_mux_regmap(hw);

	return clk_mux_determine_rate_flags(hw, req, mux->flags);
}

static const struct clk_ops clk_regmap_mux_ops = {
	.get_parent = clk_regmap_mux_get_parent,
	.set_parent = clk_regmap_mux_set_parent,
	.determine_rate = clk_regmap_mux_determine_rate,
};

static unsigned long clk_regmap_div_recalc_rate(struct clk_hw *hw,
						unsigned long prate)
{
	struct clk_div_regmap *div = to_clk_div_regmap(hw);
	unsigned long rate;
	u32 val;

	regmap_read(div->map, div->offset, &val);

	val >>= div->shift;
	val &= clk_div_mask(div->width);

	rate = divider_recalc_rate(hw, prate, val, NULL, div->flags, div->width);
	do_div(rate, div->fix_divider);

	return rate;
}

static long clk_regmap_div_round_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long *prate)
{
	struct clk_div_regmap *div = to_clk_div_regmap(hw);
	long round_rate;
	u32 val;

	/* if read only, just return current value */
	if (div->flags & CLK_DIVIDER_READ_ONLY) {
		regmap_read(div->map, div->offset, &val);

		val >>= div->shift;
		val &= clk_div_mask(div->width);

		round_rate = divider_ro_round_rate(hw, rate, prate, NULL,
						div->width, div->flags, val);
	} else {
		round_rate = divider_round_rate(hw, rate, prate, NULL, div->width, div->flags);
	}

	if (round_rate > 0)
		do_div(round_rate, div->fix_divider);

	return round_rate;
}

static int clk_regmap_div_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long prate)
{
	struct clk_div_regmap *div = to_clk_div_regmap(hw);
	int val;

	rate *= div->fix_divider;

	val = divider_get_val(rate, prate, NULL, div->width, div->flags);
	if (val < 0)
		return val;

	regmap_update_bits(div->map, div->offset,
		clk_div_mask(div->width) << div->shift, val << div->shift);

	/* write enable */
	if (!(div->flags & CLK_DIVIDER_ONE_BASED)) {
		regmap_update_bits(div->map, div->offset, 0x1, 0x1);
		regmap_update_bits(div->map, div->offset, 0x1, 0x0);
	}

	return 0;
};

static const struct clk_ops clk_regmap_div_ops = {
	.recalc_rate = clk_regmap_div_recalc_rate,
	.round_rate = clk_regmap_div_round_rate,
	.set_rate = clk_regmap_div_set_rate,
};

static struct clk_mux_regmap * __init ambarella_mux_regmap_clock_init(struct device_node *np)
{
	struct clk_mux_regmap *mux;

	if (of_device_is_compatible(np, "ambarella,div-clock"))
		return NULL;

	/* sanity check */
	if (of_clk_get_parent_count(np) < 2) {
		pr_err("%s: mux clock needs 2 more parents\n", np->name);
		return NULL;
	}

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return NULL;

	mux->map = syscon_regmap_lookup_by_phandle(np, "amb,mux-regmap");
	if (IS_ERR(mux->map)) {
		pr_err("%s: no regmap!\n", np->name);
		goto err_exit;
	}

	if (of_property_read_u32_index(np, "amb,mux-regmap", 1, &mux->offset)) {
		pr_err("%s: no regmap offset!\n", np->name);
		goto err_exit;
	}

	if (of_property_read_u32(np, "amb,mux-mask", &mux->mask))
		mux->mask = 0x3;

	if (of_property_read_u32(np, "amb,mux-shift", &mux->shift))
		mux->shift = 0;

	return mux;

err_exit:
	kfree(mux);
	return NULL;
}

static struct clk_div_regmap * __init ambarella_div_regmap_clock_init(struct device_node *np)
{
	struct clk_div_regmap *div;

	if (of_device_is_compatible(np, "ambarella,mux-clock"))
		return NULL;

	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		return NULL;

	div->map = syscon_regmap_lookup_by_phandle(np, "amb,div-regmap");
	if (IS_ERR(div->map)) {
		pr_err("%s: no regmap!\n", np->name);
		goto err_exit;
	}

	if (of_property_read_u32_index(np, "amb,div-regmap", 1, &div->offset)) {
		pr_err("%s: no regmap offset!\n", np->name);
		goto err_exit;
	}

	if (of_property_read_u32(np, "amb,div-width", &div->width))
		div->width = 24;

	if (of_property_read_u32(np, "amb,div-shift", &div->shift))
		div->shift = 0;

	if (of_find_property(np, "amb,div-plus", NULL))
		div->flags = 0;
	else
		div->flags = CLK_DIVIDER_ONE_BASED;

	if (of_property_read_u32(np, "amb,fix-divider", &div->fix_divider))
		div->fix_divider = 1;

	return div;

err_exit:
	kfree(div);
	return NULL;
}

static void __init ambarella_composite_clocks_init(struct device_node *np)
{
	struct clk *clk;
	struct clk_mux_regmap *mux_regmap = NULL;
	struct clk_div_regmap *div_regmap = NULL;
	const char *name, **parent_names;
	u32 num_parents;

	num_parents = of_clk_get_parent_count(np);
	if (num_parents < 1) {
		pr_err("%s: no parent found\n", np->name);
		return;
	}

	parent_names = kzalloc(sizeof(char *) * num_parents, GFP_KERNEL);
	if (!parent_names)
		return;

	of_clk_parent_fill(np, parent_names, num_parents);

	if (of_property_read_string(np, "clock-output-names", &name))
		name = np->name;

	div_regmap = ambarella_div_regmap_clock_init(np);
	mux_regmap = ambarella_mux_regmap_clock_init(np);

	clk = clk_register_composite(NULL, name, parent_names, num_parents,
			mux_regmap ? &mux_regmap->hw : NULL,
			mux_regmap ? &clk_regmap_mux_ops : NULL,
			div_regmap ? &div_regmap->hw : NULL,
			div_regmap ? &clk_regmap_div_ops : NULL,
			NULL, NULL,
			CLK_GET_RATE_NOCACHE | CLK_SET_RATE_NO_REPARENT);
	if (IS_ERR(clk)) {
		pr_err("%s: failed to register %s composite clock (%ld)\n",
		       __func__, name, PTR_ERR(clk));
		return;
	}

	of_clk_add_provider(np, of_clk_src_simple_get, clk);
	clk_register_clkdev(clk, name, NULL);

	kfree(parent_names);
}

CLK_OF_DECLARE(ambarella_clk_composite,
		"ambarella,composite-clock", ambarella_composite_clocks_init);
CLK_OF_DECLARE(ambarella_clk_div,
		"ambarella,div-clock", ambarella_composite_clocks_init);
CLK_OF_DECLARE(ambarella_clk_mux,
		"ambarella,mux-clock", ambarella_composite_clocks_init);

