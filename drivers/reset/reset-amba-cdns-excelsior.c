// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ambarella Reset driver for cadence excelsior PHY driver.
 *
 * Copyright 2023 Ambarella, Inc.
 * Author: Li Chen <lchen@ambarella.com>
 */

#include <linux/of.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/mfd/syscon.h>

/* TODO: confirm if the reference is 100MHz */
/* PCIE_PMA_CTRL_REG */
#define PCIE_PMA_CMN_REFCLK_DIG_DIV_4 BIT(4)
#define PCIE_PMA_CMN_REFCLK_DIG_DIV_MASK GENMASK(4, 3)
#define PCIE_PMA_CMN_REFCLK0_TERM_EN_MASK BIT(0)

/* PCIEP_CTRL_REG - Stupid register definition for PCIe! */
#define PCIEP0_APB_PRESET_SOFT_RESET BIT(0)
#define PCIEP0_SOFT_RESET BIT(1)
#define PCIEP1_APB_PRESET_SOFT_RESET BIT(2)
#define PCIEP1_SOFT_RESET BIT(3)
#define PCIEP0_UC_SOFT_RESET BIT(4)
#define PCIEP1_UC_SOFT_RESET BIT(5)
#define PCIEP0_PIPE_l00_SOFT_RESET BIT(6)
#define PCIEP0_PIPE_l01_SOFT_RESET BIT(7)
#define PCIEP0_PIPE_l02_SOFT_RESET BIT(8)
#define PCIEP0_PIPE_l03_SOFT_RESET BIT(9)
#define PCIEP1_PIPE_l00_SOFT_RESET BIT(10)
#define PCIEP1_PIPE_l01_SOFT_RESET BIT(11)

/* PCIEC_CTRL1_REG */
#define PCIEC_RESETN_SOFT_RESET BIT(0)
#define PCIEC_APB_SOFT_RESET BIT(1)
#define PCIEC_AXI_SOFT_RESET BIT(2)
#define PCIEC_PM_SOFT_RESET BIT(3)
#define PCIEC_REG_STICKY_SOFT_RESET BIT(4)
#define PCIEC_REG_SOFT_RESET BIT(5)
#define PCIEC_REG_AXI_SOFT_RESET BIT(6)
#define PCIEC_PIPE_SOFT_RESET BIT(7)
#define PCIEC_PERST_SOFT_RESET BIT(8)
#define PCIEC_MISC_RESET GENMASK(8, 0)
#define PCIEC_MODE_SELECT_RP BIT(10)
#define PCIEC_CONFIG_EN BIT(15)
#define PCIEC_LANE_RESET GENMASK(21, 19)
#define PCIEC_STRAP_PCIE_RATE_MAX GENMASK(24, 22)
#define PCIEC_LINK_TRAIN_EN BIT(29)
#define PCIEC_ASF_PAR_PASSTHRU_ENABLE BIT(30)

static DEFINE_SPINLOCK(register_lock);

/* PCIEC_CTRL2_REG */
#define PCIEC_STRAP_DC_MAX_EVAL_ITERATION GENMASK(17, 11)

enum {
	PCIE0_PHY_RESET,
	PCIE0_APB_RESET,
	PCIE0_LNK_RESET,
	PCIE1_PHY_RESET,
	PCIE1_APB_RESET,
	PCIE1_LN0_LNK_RESET,
	PCIE1_LN1_LNK_RESET,
	CDNS_PHY_NR_RESETS,
};

enum {
	LANE_COUNT_X1,
	LANE_COUNT_X2,
	LANE_COUNT_X4,
	LANE_COUNT_X8,
	LANE_COUNT_X16,
};

enum {
	PCIE_RATE_2_5GT,
	PCIE_RATE_5_0GT,
	PCIE_RATE_8_0GT,
	PCIE_RATE_16_0GT,
	PCIE_RATE_32_0GT,
};

enum pcie_mode {
	EP_MODE,
	RC_MODE,
};

#define PCI_TPVPERL_DELAY_MS 100

enum {
	PMA_CTRL_REG = 0,
	P_CTRL_REG,
	PCIEP_NUM_REG,
};

enum {
	C_CTRL1_REG,
	C_CTRL2_REG,
	PCIEC_NUM_REG,
};

struct amba_phyrst {
	struct reset_controller_dev rcdev;
	struct regmap *ns_sp_regmap;
	struct regmap *pciec_sp_regmap;
	u32 pciec_offset[PCIEC_NUM_REG];
	u32 pciep_offset[PCIEP_NUM_REG];
	u32 phy_id;
	u32 gen;
	enum pcie_mode pcie_mode;
};

struct amba_phyrst_of_data {
	int (*init)(struct amba_phyrst *phyrst, struct device_node *np);
	const struct reset_control_ops *ops;
};

static struct amba_phyrst *to_amba_phyrst(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct amba_phyrst, rcdev);
}

static int amba_phyrst_pcie_assert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	struct amba_phyrst *phyrst = to_amba_phyrst(rcdev);

	spin_lock(&register_lock);
	switch (id) {
	case PCIE0_LNK_RESET:
	case PCIE1_LN0_LNK_RESET:
	case PCIE1_LN1_LNK_RESET:
		break;
	case PCIE0_PHY_RESET:
		regmap_update_bits(phyrst->pciec_sp_regmap,
				   phyrst->pciec_offset[C_CTRL1_REG],
				   PCIEC_STRAP_PCIE_RATE_MAX, PCIE_RATE_32_0GT);
		regmap_update_bits(phyrst->pciec_sp_regmap,
				   phyrst->pciec_offset[C_CTRL1_REG],
				   PCIEC_LANE_RESET, LANE_COUNT_X4);
		regmap_clear_bits(phyrst->pciec_sp_regmap,
				  phyrst->pciec_offset[C_CTRL1_REG],
				  PCIEC_LINK_TRAIN_EN);
		regmap_set_bits(phyrst->pciec_sp_regmap,
				phyrst->pciec_offset[C_CTRL1_REG],
				PCIEC_MISC_RESET);
		regmap_set_bits(phyrst->ns_sp_regmap,
				phyrst->pciep_offset[P_CTRL_REG],
				PCIE0_PHY_RESET);
		break;
	case PCIE1_PHY_RESET:
		regmap_update_bits(phyrst->pciec_sp_regmap,
				   phyrst->pciec_offset[C_CTRL1_REG],
				   PCIEC_STRAP_PCIE_RATE_MAX, PCIE_RATE_32_0GT);
		regmap_update_bits(phyrst->pciec_sp_regmap,
				   phyrst->pciec_offset[C_CTRL1_REG],
				   PCIEC_LANE_RESET, LANE_COUNT_X4);
		regmap_clear_bits(phyrst->pciec_sp_regmap,
				  phyrst->pciec_offset[C_CTRL1_REG],
				  PCIEC_LINK_TRAIN_EN);
		regmap_set_bits(phyrst->pciec_sp_regmap,
				phyrst->pciec_offset[C_CTRL1_REG],
				PCIEC_MISC_RESET);
		regmap_set_bits(phyrst->ns_sp_regmap,
				phyrst->pciep_offset[P_CTRL_REG],
				PCIE1_PHY_RESET);
		break;
	}
	spin_unlock(&register_lock);

	return 0;
}

static void amba_phyrst_lnk_pciec_deassert(struct amba_phyrst *phyrst)
{
	enum pcie_mode pcie_mode = phyrst->pcie_mode;

	/* Some dely for UC */
	msleep(20);
	/* Wait PHY status to be de-asserted */
	msleep(20);
	regmap_clear_bits(phyrst->pciec_sp_regmap,
			  phyrst->pciec_offset[C_CTRL1_REG],
			  PCIEC_AXI_SOFT_RESET);
	regmap_clear_bits(phyrst->pciec_sp_regmap,
			  phyrst->pciec_offset[C_CTRL1_REG],
			  PCIEC_RESETN_SOFT_RESET);
	regmap_clear_bits(phyrst->pciec_sp_regmap,
			  phyrst->pciec_offset[C_CTRL1_REG],
			  PCIEC_REG_AXI_SOFT_RESET);
	regmap_clear_bits(phyrst->pciec_sp_regmap,
			  phyrst->pciec_offset[C_CTRL1_REG],
			  PCIEC_REG_STICKY_SOFT_RESET);
	regmap_clear_bits(phyrst->pciec_sp_regmap,
			  phyrst->pciec_offset[C_CTRL1_REG],
			  PCIEC_REG_SOFT_RESET);
	regmap_clear_bits(phyrst->pciec_sp_regmap,
			  phyrst->pciec_offset[C_CTRL1_REG],
			  PCIEC_PM_SOFT_RESET);
	/**
	 * "Power Sequencing and Reset Signal Timings" table in
	 * PCI EXPRESS CARD ELECTROMECHANICAL SPECIFICATION, REV. 3.0
	 * indicates PERST# should be deasserted after minimum of 100us
	 * once REFCLK is stable. The REFCLK to the connector in RC
	 * mode is selected while enabling the PHY. So deassert PERST#
	 * after 100 us.
	 * PCI EXPRESS CARD ELECTROMECHANICAL SPECIFICATION, REV. 3.0
	 * indicates PERST# should be deasserted after minimum of 100ms
	 * after power rails achieve specified operating limits and
	 * within this period reference clock should also become stable.
	 */
	msleep(PCI_TPVPERL_DELAY_MS);
	regmap_clear_bits(phyrst->pciec_sp_regmap,
			  phyrst->pciec_offset[C_CTRL1_REG],
			  PCIEC_PERST_SOFT_RESET);

	/* XXX: in romcode, this is configured after PCIe controller finishes
	 * its programming
	 */
	regmap_set_bits(phyrst->pciec_sp_regmap,
			phyrst->pciec_offset[C_CTRL1_REG], PCIEC_LINK_TRAIN_EN);
	if (pcie_mode == EP_MODE)
		regmap_set_bits(phyrst->pciec_sp_regmap,
				phyrst->pciec_offset[C_CTRL1_REG],
				PCIEC_CONFIG_EN);
}
static int amba_phyrst_pcie_deassert(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	struct amba_phyrst *phyrst = to_amba_phyrst(rcdev);

	spin_lock(&register_lock);
	switch (id) {
	case PCIE0_LNK_RESET:
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP0_PIPE_l00_SOFT_RESET);
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP0_PIPE_l01_SOFT_RESET);
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP0_PIPE_l02_SOFT_RESET);
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP0_PIPE_l03_SOFT_RESET);

		amba_phyrst_lnk_pciec_deassert(phyrst);
		break;
	case PCIE1_LN0_LNK_RESET:
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP1_PIPE_l00_SOFT_RESET);

		amba_phyrst_lnk_pciec_deassert(phyrst);
		break;
	case PCIE1_LN1_LNK_RESET:
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP1_PIPE_l01_SOFT_RESET);

		amba_phyrst_lnk_pciec_deassert(phyrst);
		break;
	case PCIE0_PHY_RESET:
		/* Some delay for UC */
		msleep(20);
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP0_SOFT_RESET);

		regmap_clear_bits(phyrst->pciec_sp_regmap,
				  phyrst->pciec_offset[C_CTRL1_REG],
				  PCIEC_PIPE_SOFT_RESET);
		break;
	/*
	 * PCIE1_PHY_RESET should only be called once since two phyrsts share
	 * the same single cdns-pcie1-phy, so we need already_configured here.
	 */
	case PCIE1_PHY_RESET:
		/* Some delay for UC */
		msleep(20);
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP1_SOFT_RESET);

		regmap_clear_bits(phyrst->pciec_sp_regmap,
				  phyrst->pciec_offset[C_CTRL1_REG],
				  PCIEC_PIPE_SOFT_RESET);
		break;
	}
	spin_unlock(&register_lock);

	return 0;
}

static const struct reset_control_ops amba_phyrst_pcie_ops = {
	.assert = amba_phyrst_pcie_assert,
	.deassert = amba_phyrst_pcie_deassert,
};

static int amba_phyrst_pcie_init(struct amba_phyrst *phyrst,
				 struct device_node *np)
{
	/*
	 * we have pcie2_phyrst and pcie1_phyrst, but they share
	 * the same PHY controller: "cdns-pcie1-phy"
	 *
	 * So to avoid re-configure, let's introduce pciep1_already_configured
	 */
	static bool pciep1_already_configured;
	u32 phy_id = phyrst->phy_id, gen = phyrst->gen;
	enum pcie_mode pcie_mode = phyrst->pcie_mode;
	int pcie_pipe_rate_max;

	spin_lock(&register_lock);
	if (pcie_mode == RC_MODE)
		regmap_set_bits(phyrst->pciec_sp_regmap,
				phyrst->pciec_offset[C_CTRL1_REG],
				PCIEC_MODE_SELECT_RP);
	else
		regmap_clear_bits(phyrst->pciec_sp_regmap,
				  phyrst->pciec_offset[C_CTRL1_REG],
				  PCIEC_MODE_SELECT_RP);

	pcie_pipe_rate_max = gen - 1;

	regmap_update_bits(phyrst->pciec_sp_regmap,
			   phyrst->pciec_offset[C_CTRL1_REG],
			   PCIEC_STRAP_PCIE_RATE_MAX, pcie_pipe_rate_max << 22);

	regmap_set_bits(phyrst->pciec_sp_regmap,
			phyrst->pciec_offset[C_CTRL1_REG],
			PCIEC_ASF_PAR_PASSTHRU_ENABLE);

	regmap_update_bits(phyrst->pciec_sp_regmap,
			   phyrst->pciec_offset[C_CTRL2_REG],
			   PCIEC_STRAP_DC_MAX_EVAL_ITERATION, BIT(14));

	if ((!pciep1_already_configured && phy_id == 1) || phy_id == 0) {
		/* TODO: check if PHY reference clock is fixed at 100Mhz */
		regmap_update_bits(phyrst->ns_sp_regmap,
				   phyrst->pciep_offset[PMA_CTRL_REG],
				   PCIE_PMA_CMN_REFCLK_DIG_DIV_MASK,
				   PCIE_PMA_CMN_REFCLK_DIG_DIV_4);

		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[PMA_CTRL_REG],
				  PCIE_PMA_CMN_REFCLK0_TERM_EN_MASK);
	}

	regmap_clear_bits(phyrst->pciec_sp_regmap,
			  phyrst->pciec_offset[C_CTRL1_REG],
			  PCIEC_APB_SOFT_RESET);

	if (phy_id == 0) {
		/* TODO: do we still need this early access if we have clear pciec's apb reset? */
		/* Release PCIe PHY APB reset to allow access to PCS/PMA registers */
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP0_APB_PRESET_SOFT_RESET);

		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP0_UC_SOFT_RESET);
	}

	if (!pciep1_already_configured && phy_id == 1) {
		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP1_APB_PRESET_SOFT_RESET);

		regmap_clear_bits(phyrst->ns_sp_regmap,
				  phyrst->pciep_offset[P_CTRL_REG],
				  PCIEP1_UC_SOFT_RESET);
	}

	/* pciep1 finishes configuration */
	if (phy_id == 1)
		pciep1_already_configured = true;

	spin_unlock(&register_lock);

	return 0;
}

static const struct amba_phyrst_of_data amba_phyrst_pcie_of_data = {
	.init = amba_phyrst_pcie_init,
	.ops = &amba_phyrst_pcie_ops,
};

static const struct of_device_id amba_phyrst_dt_ids[] = {
	{ .compatible = "ambarella,excelsior-phyrst",
	  .data = &amba_phyrst_pcie_of_data },
	{ /* sentinel */ }
};

static int amba_phyrst_probe(struct platform_device *pdev)
{
	const struct amba_phyrst_of_data *data;
	const struct of_device_id *match;
	struct amba_phyrst *phyrst;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	enum of_gpio_flags flags;
	int pwr_gpio, rval;
	struct device_node *ctrlr_np =
		of_parse_phandle(np, "amb,pcie-controller", 0);
	int ret;

	match = of_match_device(amba_phyrst_dt_ids, dev);
	if (!match)
		return -EINVAL;

	data = (struct amba_phyrst_of_data *)match->data;

	phyrst = devm_kzalloc(dev, sizeof(*phyrst), GFP_KERNEL);
	if (!phyrst)
		return -ENOMEM;

	if (of_device_is_compatible(ctrlr_np, "ambarella,cdns-pcie-ep"))
		phyrst->pcie_mode = EP_MODE;
	else
		phyrst->pcie_mode = RC_MODE;

	phyrst->ns_sp_regmap = syscon_regmap_lookup_by_phandle_args(
		np, "amb,scr-regmap", PCIEP_NUM_REG, phyrst->pciep_offset);
	if (IS_ERR(phyrst->ns_sp_regmap)) {
		dev_err(dev, "amb,scr-regmap lookup failed.\n");
		return PTR_ERR(phyrst->ns_sp_regmap);
	}

	phyrst->pciec_sp_regmap = syscon_regmap_lookup_by_phandle_args(
		np, "amb,pcie-scr-regmap", PCIEC_NUM_REG, phyrst->pciec_offset);
	if (IS_ERR(phyrst->pciec_sp_regmap)) {
		dev_err(dev, "amb,pcie-scr-regmap lookup failed.\n");
		return PTR_ERR(phyrst->pciec_sp_regmap);
	}

	ret = of_property_read_u32(np, "amb,pcie-phy-id", &phyrst->phy_id);
	if (ret) {
		dev_err(dev, "amb,pcie-phy-id lookup failed.\n");
		return ret;
	}

	ret = of_property_read_u32(np, "amb,pcie-gen", &phyrst->gen);
	if (ret) {
		dev_err(dev, "amb,pcie-gen lookup failed.\n");
		return ret;
	}

	pwr_gpio = of_get_named_gpio_flags(np, "pwr-gpios", 0, &flags);
	if (gpio_is_valid(pwr_gpio)) {
		u32 gpio_init_flag;

		if (flags & OF_GPIO_ACTIVE_LOW)
			gpio_init_flag = GPIOF_OUT_INIT_LOW;
		else
			gpio_init_flag = GPIOF_OUT_INIT_HIGH;

		rval = devm_gpio_request_one(dev, pwr_gpio, gpio_init_flag,
					     dev_name(&pdev->dev));
		if (rval < 0) {
			dev_err(dev, "Failed to request pwr-gpios!\n");
			return rval;
		}
	}

	data->init(phyrst, np);

	phyrst->rcdev.owner = THIS_MODULE;
	phyrst->rcdev.ops = data->ops;
	phyrst->rcdev.of_node = dev->of_node;
	phyrst->rcdev.nr_resets = CDNS_PHY_NR_RESETS;

	rval = devm_reset_controller_register(dev, &phyrst->rcdev);
	if (rval < 0) {
		dev_err(dev, "failed to registers!\n");
		return rval;
	}

	return 0;
}

static struct platform_driver amba_phyrst_driver = {
	.probe = amba_phyrst_probe,
	.driver = {
		.name = "amba-phyrst",
		.of_match_table = amba_phyrst_dt_ids,
	},
};

static int __init amba_phyrst_init(void)
{
	return platform_driver_register(&amba_phyrst_driver);
}

postcore_initcall(amba_phyrst_init);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("Ambarella Cadence PHY reset driver");
MODULE_LICENSE("GPL v2");
