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

//#define DEBUG

#define NR_VCO 5
enum {
	CTRL_OFFSET = 0,
	FRAC_OFFSET,
	CTRL2_OFFSET,
	CTRL3_OFFSET,
	PRES_OFFSET,
	POST_OFFSET,
	CLK_CTRL_OFFSET,
	REG_MAX_NUM,
};

struct hdmi_pll_info {
	struct clk_hw hw;
	struct device_node *np;
	struct regmap *pll_regmap;
	const char *name;
	const char *parent_name;
	u32 reg_offset[REG_MAX_NUM];
#define HDMI_PLL_14NM 0 /* For 14nm chip */
#define HDMI_PLL_10NM 1 /* For 10nm chip */
#define HDMI_PLL_05NM 2 /* For 5nm chip */
	u32 pll_version;
	u32 frac_mode;
	u32 fix_divider;
	u32 vco_max_mhz;
	u32 vco_min_mhz;
	u32 clk_min_rate;
	u32 ctrl2_val;
	u32 ctrl3_val;
	u32 vco_range[NR_VCO];
	struct parameters_14nm {	// for 14nm(S5L)
		u32 pre_scaler : 5;     /* BIT[4:0]*/
		u32 intp : 8;	        /* BIT[12:5]*/
		u32 sdiv : 5;		/* BIT[17:13]*/
		u32 sout : 5;		/* BIT[22:18]*/
		u32 frac_nega :1;	/* BIT[23]*/
		u32 ctrl2_8 :1;		/* BIT[24]*/
		u32 ctrl2_9 :1;		/* BIT[25]*/
		u32 ctrl2_10 :1;	/* BIT[26]*/
		u32 reserved :5;	/* BIT[31:27]*/
		u32 frac_val;
	} p_14nm;
	struct parameters {		// for clkpll-v0 and clkpll-v1
		u32 pre_scaler : 5;     /* BIT[4:0]*/
		u32 intp : 8;	        /* BIT[12:5]*/
		u32 sdiv : 5;		/* BIT[17:13]*/
		u32 sout : 5;		/* BIT[22:18]*/
		u32 reserved1 :9;	/* BIT[31:23]*/
		u32 vcodiv :5;		/* BIT[4:0]*/
		u32 fsdiv :5;		/* BIT[9:5]*/
		u32 fsout :5;		/* BIT[14:10]*/
		u32 ctrl2_12 :1;	/* BIT[15]*/
		u32 reserved2 :16;	/* BIT[31:16]*/
		u32 frac_val;
	} p;
};

#define to_hdmi_pll_info(_hw) container_of(_hw, struct hdmi_pll_info, hw)

u32 pll_reg_rd(struct hdmi_pll_info *info, u32 reg_offset);
void pll_reg_wr(struct hdmi_pll_info *info, u32 reg_offset, u32 val);
int hdmi_pll_set_rate_14nm(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate);
int hdmi_pll_set_rate(struct clk_hw *hw,
		      unsigned long rate,
		      unsigned long parent_rate);
unsigned long hdmi_pll_recalc_rate_14nm(struct clk_hw *hw,
					unsigned long parent_rate);
unsigned long hdmi_pll_recalc_rate(struct clk_hw *hw,
				   unsigned long parent_rate);

/*
 * Ctrl/Ctrl3 reg for S5L(14nm), 10nm, 5nm chip is same
 */
typedef struct {
	u32 pll_we		: 1;	//BIT[0]
	u32 reserved1		: 1;	//BIT[1]
	u32 pll_bypass		: 1;	//BIT[2]
	u32 frac_mode		: 1;	//BIT[3]
	u32 rst_l		: 1;	//BIT[4]
	u32 power_down		: 1;	//BIT[5]
	u32 vco_halt		: 1;	//BIT[6]
	u32 pfd_lpf_float	: 1;	//BIT[7]
	u32 pll_tout_sel	: 4;	//BIT[11:8]
	u32 pll_sdiv		: 4;	//BIT[15:12]
	u32 pll_sout		: 4;	//BIT[19:16]
	u32 force_lock		: 1;	//BIT[20]
	u32 force_bypass	: 1;	//BIT[21]
	u32 pll_dsm_rst_l	: 1;	//BIT[22]
	u32 reserved3		: 1;	//BIT[23]
	u32 pll_int		: 7;	//BIT[30:24]
	u32 reserved4		: 1;	//BIT[31]
} ctrl_reg_s;

typedef struct {
	u32 reserved1		: 1;	//BIT[0]
	u32 pll_vco_range	: 2;	//BIT[2:1]
	u32 pll_vco_clamp	: 2;	//BIT[4:3]
	u32 reserved2		: 2;	//BIT[6:5]
	u32 pll_dsm_dith_gain	: 2;	//BIT[8:7]
	u32 reserved3		: 4;	//BIT[12:9]
	u32 pll_lpf_rz		: 4;	//BIT[16:13]
	u32 pll_iref_ctrl	: 3;	//BIT[19:17]
	u32 pll_byp_jdiv	: 1;	//BIT[20]
	u32 pll_byp_jout	: 1;	//BIT[21]
	u32 reserved4		: 10;	//BIT[31:22]
} ctrl3_reg_s;

typedef struct {
	u32 reserved1		: 8;	//BIT[7:0]
	u32 ctrl2_8		: 1;	//BIT[8]
	u32 ctrl2_9		: 1;	//BIT[9]
	u32 ctrl2_10		: 1;	//BIT[10]
	u32 dutycyle_tune_lv	: 1;	//BIT[11]
	u32 bypass_vco_out	: 1;	//BIT[12]
	u32 reserved2		: 3;	//BIT[15:13]
	u32 pll_icp_ctrl	: 8;	//BIT[23:16]
	u32 pll_dsm_type_sel	: 2;	//BIT[25:24]
	u32 reserved3		: 6;	//BIT[31:26]
} ctrl2_reg_14nm_s;

typedef struct {
	u32 vcodiv		: 4;	//BIT[3:0]
	u32 fsdiv		: 4;	//BIT[7:4]
	u32 fsout		: 4;	//BIT[11:8]
	u32 pass_pll_div_hs	: 1;	//BIT[12]
	u32 bypass_mdiv		: 1;	//BIT[13]
	u32 diffvco_en		: 1;	//BIT[14]
	u32 dutycyle_tune_lv	: 1;	//BIT[15]
	u32 pll_icp_ctrl	: 8;	//BIT[23:16]
	u32 pll_dsm_type_sel	: 2;	//BIT[25:24]
	u32 reserved1		: 2;	//BIT[27:26]
	u32 lctrl		: 2;	//BIT[29:28]
	u32 reserved2		: 2;	//BIT[31:30]
} ctrl2_reg_v0_s; /* for clkpll-v0 and clkpll-v1 */

typedef struct {
	u32 reserved1		: 8;	//BIT[7:0]
	u32 vcodiv		: 1;	//BIT[8]
	u32 fsdiv		: 1;	//BIT[9]
	u32 reserved2		: 1;	//BIT[10]
	u32 fsout		: 1;	//BIT[11]
	u32 pass_pll_div_hs	: 1;	//BIT[12]
	u32 bypass_mdiv		: 1;	//BIT[13]
	u32 reserved3		: 2;	//BIT[15:14]
	u32 pll_icp_ctrl	: 8;	//BIT[23:16]
	u32 pll_dsm_type_sel	: 2;	//BIT[25:24]
	u32 reserved4		: 2;	//BIT[27:26]
	u32 lctrl		: 2;	//BIT[29:28]
	u32 reserved5		: 2;	//BIT[31:30]
} ctrl2_reg_v1_s; /* for clkpll-v0 and clkpll-v1 */

typedef struct {
	u32	frac	        : 31;	/* [30:0] */
	u32	nega		: 1;	/* [31] */
} frac_reg_14nm_s;

typedef struct {
	u32	we	        : 1;	/* [0] */
	u32	reserved1	: 3;	/* [3:1] */
	u32	div		: 4;	/* [7:4] */
	u32	reserved2	: 24;	/* [31:8] */
} pre_reg_s;

typedef union {
	struct {
		u32 refclk_sel : 1;				/* [0] */
		u32 hdmi_phy_test_mode : 1;			/* [1] */
		u32 use_hdmi_phy_clk_vo_for_gclk_vo : 1;	/* [2] */
		u32 sel_ring : 1;				/* [3] */
		u32 pdb_hdmi : 1;				/* [4] */
		u32 reserved1 : 3;				/* [7:5] */
		u32 clksel : 2;					/* [9:8] */
		u32 reserved2 : 5;				/* [14:10] */
		u32 pll_cascade_setting : 1;			/* [15] */
		u32 reserved3 : 16;				/* [31:16] */
	} reg;
	u32 val;
} HDMI_CLK_CTRL_REG_U;

typedef union {
	ctrl_reg_s reg;
	u32 val;
} CTRL_REG_U;

typedef union {
	ctrl2_reg_v0_s v0;
	ctrl2_reg_v1_s v1;
	ctrl2_reg_14nm_s nm_14;
	u32 val;
} CTRL2_REG_U;

typedef union {
	ctrl3_reg_s reg;
	u32 val;
} CTRL3_REG_U;

typedef union {
	frac_reg_14nm_s nm_14;
	u32 val;
} FRAC_REG_U;

typedef union {
	pre_reg_s reg;
	u32 val;
} PRES_REG_U;
#endif

