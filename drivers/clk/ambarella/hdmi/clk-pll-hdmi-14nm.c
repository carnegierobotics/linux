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

static unsigned long calc_fvco_14nm(struct hdmi_pll_info *info,
				    unsigned long parent_rate)
{
	unsigned long fvco, frac;
	unsigned long intp = (unsigned long) info->p_14nm.intp;
	unsigned long sdiv = (unsigned long) info->p_14nm.sdiv;
	unsigned long ctrl2_8 = (unsigned long) info->p_14nm.ctrl2_8;
	unsigned long ctrl2_9 = (unsigned long) info->p_14nm.ctrl2_9;
	unsigned long pre_scaler = (unsigned long) info->p_14nm.pre_scaler;
	unsigned long frac_val = (unsigned long) info->p_14nm.frac_val;
	u32 frac_nega = info->p_14nm.frac_nega;

	fvco = parent_rate / pre_scaler * (ctrl2_8 +1) * (ctrl2_9 +1) * sdiv * intp;
	frac = parent_rate / pre_scaler * (ctrl2_8 +1) * (ctrl2_9 +1) * sdiv;

	if (frac_nega) {
		frac = (frac * (0x80000000 - frac_val)) >> 32;
		fvco = fvco  - frac;
	} else {
		frac = (frac *  frac_val) >> 32;
		fvco = fvco  + frac;
	}

	return fvco;
}

static void calc_frac_14nm(struct hdmi_pll_info *info,
			   unsigned long parent_rate,
			   unsigned long pll_diff)
{
	u32 frac_val;
	unsigned long dividend, divider;

	unsigned long pre_scaler = (unsigned long) info->p_14nm.pre_scaler;
	unsigned long ctrl2_8 = (unsigned long) info->p_14nm.ctrl2_8;
	unsigned long ctrl2_9 = (unsigned long) info->p_14nm.ctrl2_9;
	unsigned long ctrl2_10 = (unsigned long) info->p_14nm.ctrl2_10;
	unsigned long sdiv = (unsigned long) info->p_14nm.sdiv;
	unsigned long sout = (unsigned long) info->p_14nm.sout;
	u32 frac_nega = info->p_14nm.frac_nega;

	dividend = (pll_diff * pre_scaler * sout * (ctrl2_10 + 1)) << 32;
	divider = sdiv * parent_rate * (ctrl2_8 + 1) * (ctrl2_9 + 1);
	frac_val = (u32) DIV_ROUND_CLOSEST_ULL(dividend, divider);

	if (frac_nega == 0) {
		frac_val = frac_val;
	} else {
	        frac_val = 0x80000000 - frac_val;
	}

	info->p_14nm.frac_val = frac_val;
}

static unsigned long calc_pllout_14nm(struct hdmi_pll_info *info,
				      unsigned long fvco)
{
	unsigned long sout = (unsigned long) info->p_14nm.sout;
	unsigned long ctrl2_10 = (unsigned long) info->p_14nm.ctrl2_10;

	return fvco / (ctrl2_10 + 1) / sout;
}

static u32 calc_sdiv_14nm(unsigned long rate, unsigned long pll_in,
			  unsigned long intp_max, unsigned long sdiv_max)
{
	unsigned long i = 1;
	unsigned long val;
	unsigned long ret = 0;

	for (i = 1; i < sdiv_max + 2; i++) {
		val = DIV_ROUND_CLOSEST_ULL(rate / i, pll_in);
		if ((val < (intp_max + 1)) && (ret == 0))
			ret = i;
		if (!((rate / i) % pll_in) && (val < (intp_max + 1)))
			return i;
	}

	return (ret == 0) ? 1 : (u32) ret;
}

#if defined(DEBUG)
static void dump_p14(struct hdmi_pll_info *info, unsigned long rate,
		     unsigned long parent_rate, u32 index)
{
	struct parameters_14nm *p = &info->p_14nm;
	unsigned long fvco, pll_out;

	fvco = calc_fvco_14nm(info, parent_rate);
	pll_out = calc_pllout_14nm(info, fvco);

	pr_info("rate:%ld\t:%d pre:%.3d, intp:%.3d, sdiv:%.3d, sout:%.3d "
		"ctrl2_8:%.3d, ctrl2_9:%.3d, ctrl2_10:%.3d, nega:%.3d, frac:0x%.8x "
		"fvco:%ld pllout:%ld\n",
		rate, index, p->pre_scaler, p->intp, p->sdiv, p->sout,
		p->ctrl2_8, p->ctrl2_9 , p->ctrl2_10, p->frac_nega, p->frac_val,
		fvco, pll_out);
}
#endif

static void set_reg_14nm(struct hdmi_pll_info *info,
			 unsigned long parent_rate,
			 unsigned long rate)
{
	PRES_REG_U pres_reg = {0};
	CTRL_REG_U ctrl_reg = {0};
	CTRL2_REG_U ctrl2_reg = {0};
	CTRL3_REG_U ctrl3_reg = {0};
	FRAC_REG_U frac_reg = {0};
	HDMI_CLK_CTRL_REG_U hdmi_clk_ctrl_reg = {0};
	u32 fvco_mhz;
	u32 range = 0;
	struct parameters_14nm *p = &info->p_14nm;

	/* pres scaler register */
	pres_reg.reg.div = p->pre_scaler - 1;
	pres_reg.reg.we = 1;
	pll_reg_wr(info, PRES_OFFSET, pres_reg.val);
	pres_reg.reg.we = 0;
	pll_reg_wr(info, PRES_OFFSET, pres_reg.val);

	/* frac register */
	frac_reg.nm_14.frac = p->frac_val;
	frac_reg.nm_14.nega = p->frac_nega;
	pll_reg_wr(info, FRAC_OFFSET, frac_reg.val);

	/* ctrl2 register */
	ctrl2_reg.val = info->ctrl2_val;
	ctrl2_reg.nm_14.ctrl2_8 = p->ctrl2_8;
	ctrl2_reg.nm_14.ctrl2_9 = p->ctrl2_9;
	ctrl2_reg.nm_14.ctrl2_10 = p->ctrl2_10;
	ctrl2_reg.nm_14.dutycyle_tune_lv = 0;
	ctrl2_reg.nm_14.bypass_vco_out = 0;
	pll_reg_wr(info, CTRL2_OFFSET, ctrl2_reg.val);

	/* ctrl3 register */
	fvco_mhz = (u32) (calc_fvco_14nm(info, parent_rate) / 1000 / 1000);
	for (range = 0; range < 4; range++) {
		if ((fvco_mhz >= info->vco_range[range]) &&
		    (fvco_mhz < info->vco_range[range + 1]))
			break;
	}
	ctrl3_reg.val = info->ctrl3_val;
	ctrl3_reg.reg.pll_vco_range = range;
	pll_reg_wr(info, CTRL3_OFFSET, ctrl3_reg.val);

	/* ctrl register */
	ctrl_reg.reg.rst_l = 1;
	ctrl_reg.reg.pll_we = 1;
	pll_reg_wr(info, CTRL_OFFSET, ctrl_reg.val);
	ctrl_reg.reg.pll_we = 0;
	pll_reg_wr(info, CTRL_OFFSET, ctrl_reg.val);
	udelay(100);
	ctrl_reg.reg.pll_int = p->intp - 1;
	ctrl_reg.reg.pll_sdiv = p->sdiv - 1;
	ctrl_reg.reg.pll_sout = p->sout - 1;
	ctrl_reg.reg.pll_bypass = 0;
	ctrl_reg.reg.frac_mode = 1;
	ctrl_reg.reg.rst_l = 0;
	ctrl_reg.reg.power_down = 0;
	ctrl_reg.reg.vco_halt = 0;
	ctrl_reg.reg.pfd_lpf_float = 0;
	ctrl_reg.reg.force_lock = 1;
	ctrl_reg.reg.force_bypass = 0;
	ctrl_reg.reg.pll_we = 1;
	pll_reg_wr(info, CTRL_OFFSET, ctrl_reg.val);
	ctrl_reg.reg.pll_we = 0;
	pll_reg_wr(info, CTRL_OFFSET, ctrl_reg.val);

	/* hdmi clock ctrl register */
	hdmi_clk_ctrl_reg.reg.refclk_sel = 1;
	hdmi_clk_ctrl_reg.reg.use_hdmi_phy_clk_vo_for_gclk_vo = 1;
	hdmi_clk_ctrl_reg.reg.sel_ring = 1;
	hdmi_clk_ctrl_reg.reg.pdb_hdmi = 1;
	if (rate < 5940000000) {
		hdmi_clk_ctrl_reg.reg.clksel = 0;
	} else {
		hdmi_clk_ctrl_reg.reg.clksel = 1;
	}
	pll_reg_wr(info, CLK_CTRL_OFFSET, hdmi_clk_ctrl_reg.val);
}

/*
 * Fix ctrl2[8], ctrl2[9], ctrl2[10] to 0, if need to set them to 1, please use dts
 */
int hdmi_pll_set_rate_14nm(struct clk_hw *hw,
			   unsigned long rate,
			   unsigned long parent_rate)
{
	unsigned long fvco, fvco_min, fvco_max, clk_min_rate, pll_out_min;
	unsigned long intp, sout;
	unsigned long pll_out, pll_in, rate_in, pll_diff;
	unsigned long intp_max, sout_max, sdiv_max;
	struct hdmi_pll_info *info = to_hdmi_pll_info(hw);
	struct parameters_14nm *p = &info->p_14nm;

	memset(p, 0, sizeof(struct parameters_14nm));

	/* check output rate boundry */
	fvco_min = (unsigned long) info->vco_min_mhz * 1000 * 1000;
	fvco_max = (unsigned long) info->vco_max_mhz * 1000 * 1000;
	if (rate > fvco_max) {
		pr_err("%s rate %lu is too big\n", __func__, rate);
		return -EINVAL;
	}

	clk_min_rate = (unsigned long) info->clk_min_rate * 1000 * 1000;
	clk_min_rate *= info->fix_divider;
	pll_out_min = fvco_min / 16;
	if (clk_min_rate != 0)
		pll_out_min = min(clk_min_rate, pll_out_min);
	else
		pll_out_min = pll_out_min;
	if (rate < pll_out_min) {
		pr_err("%s rate %ld is too small\n", __func__, rate);
		return -EINVAL;
	}

	if (info->frac_mode == 0) {
		pr_err("%s hdmi pll need frac mode\n", __func__);
		return -EINVAL;
	}

	sdiv_max = (1 << 4) - 1;
	sout_max = (1 << 4) - 1;
	intp_max = (1 << 7) - 1;

	p->pre_scaler = 1;
	pll_in = parent_rate / p->pre_scaler;
	p->sdiv = calc_sdiv_14nm(rate, pll_in, intp_max, sdiv_max);
	rate_in = rate / ((unsigned long) p->sdiv);
	rational_best_approximation(rate_in, pll_in,
				    intp_max, sout_max,
				    &intp, &sout);
	p->intp = (u32) intp;
	p->sout = (u32) sout;

	fvco = calc_fvco_14nm(info, parent_rate);
	pll_out = calc_pllout_14nm(info, fvco);

	while (fvco <= fvco_min) {
		if (((p->intp * 2) < 128) && ((p->sout * 2) <= 16)) {
			p->intp += p->intp;
			p->sout += p->sout;
		} else if (((p->sdiv * 2) <= 16) && ((p->sout * 2) <= 16)) {
			p->sdiv += p->sdiv;
			p->sout += p->sout;
		} else if ((16 / p->sout) > (16 / p->sdiv) && (p->sdiv != 16)) {
			p->sout = DIV_ROUND_UP(p->sout * 16, p->sdiv);
			p->sdiv = 16;
			rate_in = rate / ((unsigned long) p->sdiv);
			rational_best_approximation(rate_in, pll_in,
						    intp_max, sout_max,
						    &intp, &sout);
			p->intp = (u32) intp;
			p->sout = (u32) sout;
		} else if ((16 / p->sout) < (16 / p->sdiv) && (p->sout != 16)) {
			p->sdiv = DIV_ROUND_UP(p->sdiv * 16, p->sout);
			p->sout = 16;
			rate_in = rate / ((unsigned long) p->sdiv);
			rational_best_approximation(rate_in, pll_in,
						    intp_max, sout_max,
						    &intp, &sout);
			p->intp = (u32) intp;
			p->sout = (u32) sout;
		} else if ((16 / p->sout) > (128 / p->intp) && (p->intp != 128)){
			p->sout = DIV_ROUND_UP(128 * p->sout, p->intp);
			p->intp = 128;
		} else if ((16 / p->sout) < (128 / p->intp) && (p->sout != 16)) {
			p->intp = DIV_ROUND_UP(p->intp * 16, p->sout);
			p->sout = 16;
		}

		fvco = calc_fvco_14nm(info, parent_rate);
		pll_out = calc_pllout_14nm(info, fvco);
	}

	if (rate >= pll_out) {
		pll_diff = rate - pll_out;
		p->frac_nega = 0;
		calc_frac_14nm(info, parent_rate, pll_diff);
	} else {
		pll_diff = pll_out - rate;
		p->frac_nega = 1;
		calc_frac_14nm(info, parent_rate, pll_diff);
	}

	fvco = calc_fvco_14nm(info, parent_rate);
	pll_out = calc_pllout_14nm(info, fvco);

	if (rate >= pll_out) {
		pll_diff = rate - pll_out;
	} else {
		pll_diff = pll_out - rate;
	}

#ifdef DEBUG
	dump_p14(info, rate, parent_rate, 4);
#endif

	BUG_ON(fvco < fvco_min);
	BUG_ON(fvco > fvco_max);
	BUG_ON(p->sdiv > 16);
	BUG_ON(p->sout > 16);
	BUG_ON(p->pre_scaler > 16);
	BUG_ON(p->intp > 128);
	BUG_ON(pll_diff > 100);

	set_reg_14nm(info, parent_rate, rate);

	return 0;
}

unsigned long hdmi_pll_recalc_rate_14nm(struct clk_hw *hw,
					unsigned long parent_rate)
{
	u32 pre_scaler, intp, sdiv, sout, ctrl2_8, ctrl2_9, ctrl2_10;
	unsigned long fvco, frac;
	struct hdmi_pll_info *info = to_hdmi_pll_info(hw);
	CTRL_REG_U ctrl_reg;
	CTRL2_REG_U ctrl2_reg;
	FRAC_REG_U frac_reg;
	PRES_REG_U pres_reg;

	ctrl_reg.val = pll_reg_rd(info, CTRL_OFFSET);
	if ((ctrl_reg.reg.power_down == 1) || (ctrl_reg.reg.vco_halt == 1))
		return 0;

	intp = ctrl_reg.reg.pll_int + 1;
	sdiv = ctrl_reg.reg.pll_sdiv + 1;
	sout = ctrl_reg.reg.pll_sout + 1;

	ctrl2_reg.val = pll_reg_rd(info, CTRL2_OFFSET);
	ctrl2_8 = ctrl2_reg.nm_14.ctrl2_8;
	ctrl2_9 = ctrl2_reg.nm_14.ctrl2_9;
	ctrl2_10 = ctrl2_reg.nm_14.ctrl2_10;

	frac_reg.val = pll_reg_rd(info, FRAC_OFFSET);

	if (info->reg_offset[PRES_OFFSET] != 0) {
		pres_reg.val = pll_reg_rd(info, PRES_OFFSET);
		pre_scaler = pres_reg.reg.div + 1;
	} else
		pre_scaler = 1;

	fvco = parent_rate / ((unsigned long) pre_scaler);
	fvco *= (unsigned long) ((ctrl2_8 +1) * (ctrl2_9 +1) * sdiv * intp);

	frac = parent_rate / ((unsigned long) pre_scaler);
	frac *= (unsigned long) ((ctrl2_8 +1) * (ctrl2_9 +1) * sdiv);
	frac_reg.val = pll_reg_rd(info, FRAC_OFFSET);
	if (frac_reg.nm_14.nega) {
		frac = (frac * (0x80000000 - frac_reg.nm_14.frac)) >> 32;
		fvco = fvco  - frac;
	} else {
		frac = (frac *  frac_reg.nm_14.frac) >> 32;
		fvco = fvco  + frac;
	}

	fvco = fvco / info->fix_divider;

	return fvco / ((unsigned long) (ctrl2_10 + 1)) / (unsigned long)sout;
}
