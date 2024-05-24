/*
 *
 * Author: Bingliang <blhu@ambarella.com>
 *
 * Copyright (C) 2023-2026, Ambarella, Inc.
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

static unsigned long calc_fvco(struct hdmi_pll_info *info,
			       unsigned long parent_rate)
{
	unsigned long fvco, frac;
	unsigned long intp = (unsigned long) info->p.intp;
	unsigned long sdiv = (unsigned long) info->p.sdiv;
	unsigned long pre_scaler = (unsigned long) info->p.pre_scaler;
	unsigned long frac_val = (unsigned long) info->p.frac_val;
	unsigned long vcodiv = (unsigned long) info->p.vcodiv;
	unsigned long fsdiv = (unsigned long) info->p.fsdiv;

	fvco = parent_rate / pre_scaler * vcodiv * fsdiv * sdiv * intp;
	frac = parent_rate / pre_scaler * vcodiv * fsdiv * sdiv;
	frac = (frac *  frac_val) >> 32;
	fvco = fvco  + frac;

	return fvco;
}

static void calc_frac(struct hdmi_pll_info *info,
		      unsigned long parent_rate,
		      unsigned long pll_diff)
{
	u32 frac_val;
	unsigned long dividend, divider;
	unsigned long sdiv = (unsigned long) info->p.sdiv;
	unsigned long pre_scaler = (unsigned long) info->p.pre_scaler;
	unsigned long vcodiv = (unsigned long) info->p.vcodiv;
	unsigned long fsdiv = (unsigned long) info->p.fsdiv;
	unsigned long fsout = (unsigned long) info->p.fsout;
	unsigned long sout = (unsigned long) info->p.sout;

	dividend = (pll_diff * pre_scaler * sout * vcodiv * fsout) << 32;
	divider = sdiv * parent_rate * vcodiv * fsdiv;
	frac_val = (u32) DIV_ROUND_CLOSEST_ULL(dividend, divider);

	info->p.frac_val = frac_val;
}

static unsigned long calc_pll_out(struct hdmi_pll_info *info,
				  unsigned long fvco)
{
	unsigned long pllout;
	struct parameters *p = &info->p;
	unsigned long vcodiv = (unsigned long) info->p.vcodiv;
	unsigned long fsout = (unsigned long) info->p.fsout;
	unsigned long sout = (unsigned long) info->p.sout;

	if (p->ctrl2_12 == 0) {
		pllout = fvco / vcodiv / fsout / sout;
	} else {
		pllout = fvco;
	}

	return pllout;
}

#if defined(DEBUG)
static void dump_p(struct hdmi_pll_info *info, unsigned long rate,
		   unsigned long parent_rate, u32 index)
{
	struct parameters *p = &info->p;
	unsigned long fvco, pll_out;

	fvco = calc_fvco(info, parent_rate);
	pll_out = calc_pll_out(info, fvco);

	pr_info("rate:%ld\t:%d pre:%.3d, intp:%.3d, sdiv:%.3d, sout:%.3d "
		"vcodiv:%.3d, fsdiv:%.3d, fsout:%.3d, ctrl2_12:%.3d, frac:0x%.8x "
		"fvco:%ld pllout:%ld\n",
		rate, index, p->pre_scaler, p->intp, p->sdiv, p->sout,
		p->vcodiv, p->fsdiv, p->fsout, p->ctrl2_12, p->frac_val,
		fvco, pll_out);
}
#endif

static int set_reg_ctrl2_v0(struct hdmi_pll_info *info)
{
	CTRL2_REG_U ctrl2_reg = {0};
	u32 vcodiv, fsdiv, fsout;
	struct parameters *p = &info->p;

	ctrl2_reg.val = info->ctrl2_val;

	switch (p->vcodiv) {
	case 1:
		vcodiv = 0;
		break;
	case 2:
		vcodiv = 4;
		break;
	case 3:
		vcodiv = 5;
		break;
	case 5:
		vcodiv = 6;
		break;
	case 7:
		vcodiv = 7;
		break;
	default:
		pr_err("%s invalid vcodiv %d\n", __func__, p->vcodiv);
		return -EINVAL;
		break;
	}

	switch (p->fsdiv) {
	case 1:
		fsdiv = 0;
		break;
	case 2:
		fsdiv = 4;
		break;
	case 3:
		fsdiv = 5;
		break;
	case 5:
		fsdiv = 6;
		break;
	case 7:
		fsdiv = 7;
		break;
	default:
		pr_err("%s invalid fsdiv %d\n", __func__, p->fsdiv);
		return -EINVAL;
	}

	switch (p->fsout) {
	case 1:
		fsout = 0;
		break;
	case 2:
		fsout  = 4;
		break;
	case 3:
		fsout = 5;
		break;
	case 5:
		fsout = 6;
		break;
	case 7:
		fsout = 7;
		break;
	default:
		pr_err("%s invalid fsout  %d\n", __func__, p->fsout );
		return -EINVAL;
	}

	ctrl2_reg.v0.vcodiv = vcodiv;
	ctrl2_reg.v0.fsdiv = fsdiv;
	ctrl2_reg.v0.fsout = fsout;
	pll_reg_wr(info, CTRL2_OFFSET, ctrl2_reg.val);

	return 0;
}

static int set_reg_ctrl2_v1(struct hdmi_pll_info *info)
{
	CTRL2_REG_U ctrl2_reg = {0};
	u32 vcodiv, fsdiv, fsout;
	struct parameters *p = &info->p;

	ctrl2_reg.val = info->ctrl2_val;

	switch (p->vcodiv) {
	case 1:
		vcodiv = 0;
		break;
	case 2:
		vcodiv = 1;
		break;
	default:
		pr_err("%s invalid vcodiv %d\n", __func__, p->vcodiv);
		return -EINVAL;
	}

	switch (p->fsdiv) {
	case 1:
		fsdiv = 0;
		break;
	case 2:
		fsdiv = 1;
		break;
	default:
		pr_err("%s invalid fsdiv %d\n", __func__, p->fsdiv);
		return -EINVAL;
	}

	switch (p->fsout) {
	case 1:
		fsout = 0;
		break;
	case 2:
		fsout  = 1;
		break;
	default:
		pr_err("%s invalid fsout  %d\n", __func__, p->fsout );
		return -EINVAL;
	}

	ctrl2_reg.v1.vcodiv = vcodiv;
	ctrl2_reg.v1.fsdiv = fsdiv;
	ctrl2_reg.v1.fsout = fsout;
	pll_reg_wr(info, CTRL2_OFFSET, ctrl2_reg.val);

	return 0;
}

static int set_reg_ctrl2(struct hdmi_pll_info *info)
{
	int ret = 0;

	if (info->pll_version == HDMI_PLL_10NM)
		set_reg_ctrl2_v0(info);
	else if (info->pll_version == HDMI_PLL_05NM)
		set_reg_ctrl2_v1(info);
	else
		ret = -EINVAL;

	return ret;
}

static void set_reg(struct hdmi_pll_info *info,
		    unsigned long parent_rate,
		    unsigned long rate)
{
	PRES_REG_U pres_reg = {0};
	CTRL_REG_U ctrl_reg = {0};
	CTRL3_REG_U ctrl3_reg = {0};
	u32 fvco_mhz;
	u32 range = 0;
	struct parameters *p = &info->p;

	/* pres scaler register */
	if (info->reg_offset[PRES_OFFSET] != 0) {
		pres_reg.reg.div = p->pre_scaler - 1;
		pres_reg.reg.we = 1;
		pll_reg_wr(info, PRES_OFFSET, pres_reg.val);
		pres_reg.reg.we = 0;
		pll_reg_wr(info, PRES_OFFSET, pres_reg.val);
	}

	/* force reset pll first */
	ctrl_reg.reg.rst_l = 1;
	ctrl_reg.reg.pll_we = 1;
	pll_reg_wr(info, CTRL_OFFSET, ctrl_reg.val);
	ctrl_reg.reg.pll_we = 0;
	pll_reg_wr(info, CTRL_OFFSET, ctrl_reg.val);
	udelay(100);

	/* frac register */
	pll_reg_wr(info, FRAC_OFFSET, p->frac_val);

	/* ctrl2 register */
	set_reg_ctrl2(info);

	/* ctrl3 register */
	fvco_mhz = (u32) (calc_fvco(info, parent_rate) / 1000 / 1000);
	for (range = 0; range < 4; range++) {
		if ((fvco_mhz >= info->vco_range[range]) &&
		    (fvco_mhz < info->vco_range[range + 1]))
			break;
	}
	ctrl3_reg.val = info->ctrl3_val;
	ctrl3_reg.reg.pll_vco_range = range;
	pll_reg_wr(info, CTRL3_OFFSET, ctrl3_reg.val);

	/* ctrl register */
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
}

/*
 * For clkpll-v0 and clkpll-v1
 */
int hdmi_pll_set_rate(struct clk_hw *hw,
		      unsigned long rate,
		      unsigned long parent_rate)
{
	unsigned long fvco, fvco_min, fvco_max, clk_min_rate, pll_out_min;
	unsigned long intp, sout;
	unsigned long pll_out, rate_in, pll_diff;
	unsigned long intp_max, sout_max, sdiv_max;
	struct hdmi_pll_info *info = to_hdmi_pll_info(hw);
	struct parameters *p = &info->p;

	memset(p, 0, sizeof(struct parameters));

	/* use default paremeters first */
	p->vcodiv = 1;
	p->fsdiv = 1;
	p->fsout = 1;
	p->ctrl2_12 = 0;
	p->pre_scaler = 1;
	p->sdiv = 1;

	/* check output rate boundry */
	fvco_min = (unsigned long) info->vco_min_mhz * 1000 * 1000;
	fvco_max = (unsigned long) info->vco_max_mhz * 1000 * 1000;
	if (rate > fvco_max) {
		pr_err("%s rate %ld is too big\n", __func__, rate);
		return -EINVAL;
	}

	/* the minimum rate in theoretically
	 * minimum rate = fvco_min / sout_max / fout_max / vcodiff_max
	 * clkpll_v0's vcodiff_max/fout_max is 7, clkpll_v1's is 2
	 * use vcodiff/fout_max 2 for common value for clkpll_v0/clkpll_v1
	 */
	pll_out_min = fvco_min / 16 / 2 / 2;

	clk_min_rate = (unsigned long) info->clk_min_rate * 1000 * 1000;
	clk_min_rate *= info->fix_divider;
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

	sdiv_max = (1 << 4);
	sout_max = (1 << 4);
	intp_max = (1 << 7);

	if ((rate / parent_rate >= intp_max) &&
	    (rate / parent_rate < (2 * intp_max)))
		p->fsdiv = 2;
	else if (rate / parent_rate >= (2 * intp_max)) {
		p->fsdiv = 2;
		p->sdiv = 2;
	}

	rate_in  = rate / p->fsdiv / p->sdiv;
	rational_best_approximation(rate_in, parent_rate,
				    intp_max - 1, sout_max - 1,
				    &intp, &sout);
	p->intp = (u32) intp;
	p->sout = (u32) sout;
	fvco = calc_fvco(info, parent_rate);
	pll_out = calc_pll_out(info, fvco);

	if (pll_out > rate) {
		while (pll_out > rate) {
			rate_in -= parent_rate / 16 / p->sdiv / p->fsdiv / 4;
			rational_best_approximation(rate_in, parent_rate,
						    intp_max - 1, sout_max - 1,
						    &intp, &sout);
			p->intp = (u32) intp;
			p->sout = (u32) sout;
			p->frac_val = 0;
			fvco = calc_fvco(info, parent_rate);
			pll_out = calc_pll_out(info, fvco);
			pll_diff = rate - pll_out;
			calc_frac(info, parent_rate, pll_diff);
			fvco = calc_fvco(info, parent_rate);
			pll_out = calc_pll_out(info, fvco);
		}
	} else {
		pll_diff = rate - pll_out;
		calc_frac(info, parent_rate, pll_diff);
		fvco = calc_fvco(info, parent_rate);
		pll_out = calc_pll_out(info, fvco);
	}

	while (fvco < fvco_min) {
		if (p->vcodiv == 1) {
			p->vcodiv = 2;
		} else if ((p->fsdiv == 1) && (p->fsout == 1)) {
			p->fsdiv = 2;
			p->fsout = 2;
		} else if (((p->intp * 2) < 128) && ((p->sout * 2) <= 16)) {
			p->intp += p->intp;
			p->sout += p->sout;
			if (p->frac_val >= 0x80000000)
				p->intp += 1;
		} else if (((p->sdiv * 2) <= 16) && ((p->sout * 2) <= 16)) {
			p->sdiv += p->sdiv;
			p->sout += p->sout;
		} else if (((16 / p->sout) < (128 / p->intp)) && (p->sout != 16)) {
			p->intp = 16 * 1000 / p->sout * p->intp / 1000;
			p->sout = 16;

			p->frac_val = 0;
			fvco = calc_fvco(info, parent_rate);
			pll_out = calc_pll_out(info, fvco);

			pll_diff = rate - pll_out;
			calc_frac(info, parent_rate, pll_diff);

			fvco = calc_fvco(info, parent_rate);
			pll_out = calc_pll_out(info, fvco);

			if (pll_out < (rate - 100))
				p->intp += 1;
		}

		p->frac_val = 0;
		fvco = calc_fvco(info, parent_rate);
		pll_out = calc_pll_out(info, fvco);

		pll_diff = rate - pll_out;
		calc_frac(info, parent_rate, pll_diff);

		fvco = calc_fvco(info, parent_rate);
		pll_out = calc_pll_out(info, fvco);
	}

	pll_diff = rate - pll_out;

#ifdef DEBUG
	dump_p(info, rate, parent_rate, 1);
#endif

	BUG_ON(fvco < fvco_min);
	BUG_ON(fvco > fvco_max);
	BUG_ON(p->sdiv > 16);
	BUG_ON(p->sout > 16);
	BUG_ON(p->pre_scaler > 16);
	BUG_ON(p->intp > 128);
	BUG_ON(pll_diff > 100);

	set_reg(info, parent_rate, rate);

	return 0;
}

static u32 get_vcodiv_v0(struct hdmi_pll_info *info)
{
	u32 vcodiv_reg = 0, vcodiv = 0;
	CTRL2_REG_U ctrl2_reg;

	ctrl2_reg.val = pll_reg_rd(info, CTRL2_OFFSET);
	vcodiv_reg = ctrl2_reg.v0.vcodiv;
	if (vcodiv_reg < 4)
		vcodiv = 1;
	else if (vcodiv_reg >= 8)
		vcodiv = 2;
	else if (vcodiv_reg == 4)
		vcodiv = 2;
	else if (vcodiv_reg == 5)
		vcodiv = 3;
	else if (vcodiv_reg == 6)
		vcodiv = 5;
	else if (vcodiv_reg == 7)
		vcodiv = 7;

	return vcodiv;
}

static u32 get_fsdiv_v0(struct hdmi_pll_info *info)
{
	u32 fsdiv_reg = 0, fsdiv = 0;
	CTRL2_REG_U ctrl2_reg;

	ctrl2_reg.val = pll_reg_rd(info, CTRL2_OFFSET);
	fsdiv_reg = ctrl2_reg.v0.fsdiv;
	if (fsdiv_reg < 4)
		fsdiv = 1;
	else if (fsdiv_reg >= 8)
		fsdiv = 2;
	else if (fsdiv_reg == 4)
		fsdiv = 2;
	else if (fsdiv_reg == 5)
		fsdiv = 3;
	else if (fsdiv_reg == 6)
		fsdiv = 5;
	else if (fsdiv_reg == 7)
		fsdiv = 7;

	return fsdiv;
}

static u32 get_fsout_v0(struct hdmi_pll_info *info)
{
	u32 fsout_reg = 0, fsout = 0;
	CTRL2_REG_U ctrl2_reg;

	ctrl2_reg.val = pll_reg_rd(info, CTRL2_OFFSET);
	fsout_reg = ctrl2_reg.v0.fsout;
	if (fsout_reg < 4)
		fsout = 1;
	else if (fsout_reg >= 8)
		fsout = 2;
	else if (fsout_reg == 4)
		fsout = 2;
	else if (fsout_reg == 5)
		fsout = 3;
	else if (fsout_reg == 6)
		fsout = 5;
	else if (fsout_reg == 7)
		fsout = 7;

	return fsout;
}

static u32 get_vcodiv_v1(struct hdmi_pll_info *info)
{
	u32 vcodiv_reg = 0, vcodiv = 0;
	CTRL2_REG_U ctrl2_reg;

	ctrl2_reg.val = pll_reg_rd(info, CTRL2_OFFSET);
	vcodiv_reg = ctrl2_reg.v1.vcodiv;
	if (vcodiv_reg == 0)
		vcodiv = 1;
	else if (vcodiv_reg == 1)
		vcodiv = 2;

	return vcodiv;
}

static u32 get_fsdiv_v1(struct hdmi_pll_info *info)
{
	u32 fsdiv_reg = 0, fsdiv = 0;
	CTRL2_REG_U ctrl2_reg;

	ctrl2_reg.val = pll_reg_rd(info, CTRL2_OFFSET);
	fsdiv_reg = ctrl2_reg.v1.fsdiv;
	if (fsdiv_reg == 0)
		fsdiv = 1;
	else if (fsdiv_reg == 1)
		fsdiv = 2;

	return fsdiv;
}

static u32 get_fsout_v1(struct hdmi_pll_info *info)
{
	u32 fsout_reg = 0, fsout = 0;
	CTRL2_REG_U ctrl2_reg;

	ctrl2_reg.val = pll_reg_rd(info, CTRL2_OFFSET);
	fsout_reg = ctrl2_reg.v1.fsout;
	if (fsout_reg == 0)
		fsout = 1;
	else if (fsout_reg == 1)
		fsout = 2;

	return fsout;
}

static u32 get_vcodiv(struct hdmi_pll_info *info)
{
	u32 vcodiv = 1;

	if (info->pll_version == HDMI_PLL_10NM) {
		vcodiv = get_vcodiv_v0(info);
	} else if (info->pll_version == HDMI_PLL_05NM) {
		vcodiv = get_vcodiv_v1(info);
	}

	return vcodiv;
}

static u32 get_fsdiv(struct hdmi_pll_info *info)
{
	u32 fsdiv = 1;

	if (info->pll_version == HDMI_PLL_10NM) {
		fsdiv = get_fsdiv_v0(info);
	} else if (info->pll_version == HDMI_PLL_05NM) {
		fsdiv = get_fsdiv_v1(info);
	}

	return fsdiv;
}

static u32 get_fsout(struct hdmi_pll_info *info)
{
	u32 fsout = 1;

	if (info->pll_version == HDMI_PLL_10NM) {
		fsout = get_fsout_v0(info);
	} else if (info->pll_version == HDMI_PLL_05NM) {
		fsout = get_fsout_v1(info);
	}

	return fsout;
}

unsigned long hdmi_pll_recalc_rate(struct clk_hw *hw,
				   unsigned long parent_rate)
{
	u32 pre_scaler, intp, sdiv, sout, vcodiv, fsdiv, fsout, frac_reg_val;
	unsigned long fvco, frac, rate;
	struct hdmi_pll_info *info = to_hdmi_pll_info(hw);
	CTRL_REG_U ctrl_reg;
	FRAC_REG_U frac_reg;
	PRES_REG_U pres_reg;

	ctrl_reg.val = pll_reg_rd(info, CTRL_OFFSET);
	if ((ctrl_reg.reg.power_down == 1) || (ctrl_reg.reg.vco_halt == 1))
		return 0;

	intp = ctrl_reg.reg.pll_int + 1;
	sdiv = ctrl_reg.reg.pll_sdiv + 1;
	sout = ctrl_reg.reg.pll_sout + 1;

	vcodiv = get_vcodiv(info);
	fsdiv = get_fsdiv(info);
	fsout = get_fsout(info);

	frac_reg.val = pll_reg_rd(info, FRAC_OFFSET);

	if (info->reg_offset[PRES_OFFSET] != 0) {
		pres_reg.val = pll_reg_rd(info, PRES_OFFSET);
		pre_scaler = pres_reg.reg.div + 1;
	} else
		pre_scaler = 1;

	fvco = parent_rate / ((unsigned long) pre_scaler);
	fvco *= (unsigned long) (vcodiv * fsdiv * sdiv * intp);
	frac_reg_val = pll_reg_rd(info, FRAC_OFFSET);
	frac = parent_rate / ((unsigned long) pre_scaler);
	frac *= (unsigned long) (vcodiv * fsdiv * sdiv);
	frac = (frac *  frac_reg_val) >> 32;
	fvco = fvco  + frac;

	fvco = fvco / info->fix_divider;
	rate = fvco / (unsigned long) (vcodiv * fsout * sout);

	return rate;
}
