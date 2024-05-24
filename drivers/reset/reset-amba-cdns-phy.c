// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Ambarella Reset driver for Cadence PHY.
 *
 * Copyright (C) 2018-2028, Ambarella, Inc.
 */
#include <linux/of.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/mfd/syscon.h>

/* USB32_PMA_CTRL_REG */
#define USB32_PMA_CMN_REFCLK_DIG_DIV_4		BIT(12)
#define USB32_PMA_CMN_REFCLK_DIG_DIV_MASK	GENMASK(12, 11)

/* USB32C_CTRL_REG */
#define USB32C_SOFT_RESET			BIT(0)

/* USB32P_CTRL_REG */
#define USB32P_PHY_RESET			BIT(0)
#define USB32P_APB_RESET			BIT(1)

/* PCIE_PMA_CTRL_REG */
#define PCIE_PMA_CMN_REFCLK_DIG_DIV_4		BIT(21)
#define PCIE_PMA_CMN_REFCLK_DIG_DIV_MASK	GENMASK(21, 20)

/* PCIEP_CTRL_REG - Stupid register definition for PCIe! */
#define PCIEP_APB_RESET(id)			BIT((id) * 2)
#define PCIEP_PHY_RESET(id)			BIT(((id) * 2) + 1)

/* PCIEC_CTRL1_REG */
#define PCIEC_CONFIG_EN				BIT(25)
#define PCIEC_LINK_TRAIN_EN			BIT(22)
#define PCIEC_MODE_SELECT_RP			BIT(21)
#define PCIEC_APB_CORE_RATIO_4			(4 << 9)
#define PCIEC_APB_CORE_RATIO_MASK		GENMASK(13, 9)
#define PCIEC_MISC_RESET			GENMASK(7, 0)
#define PCIEC_GEN_RESET			GENMASK(18, 17)
#define PCIEC_LANE_RESET			GENMASK(20, 19)
#define LANE_COUNT_X4 2
#define LANE_COUNT_X2 1
#define LANE_COUNT_X1 0

#define GEN3 2
#define GEN2 1
#define GEN1 0

#define PCI_TPVPERL_DELAY_MS    100

enum {
	PMA_CTRL_REG = 0,
	P_CTRL_REG,
	C_CTRL_REG,
	NUM_REG,
};

struct amba_phyrst {
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
	u32 offset[NUM_REG];
	u32 phy_id;
};

struct amba_phyrst_of_data {
	int (*init)(struct amba_phyrst *, struct device_node *);
	const struct reset_control_ops *ops;
};

enum {
	CDNS_PHY_RESET = 0,
	CDNS_PHY_LINK_RESET,	/* Not used, as we are single-link PHY */
	CDNS_PHY_NR_RESETS,
};

static struct amba_phyrst *to_amba_phyrst(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct amba_phyrst, rcdev);
}

static int amba_phyrst_pcie_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct amba_phyrst *phyrst = to_amba_phyrst(rcdev);
	u32 phy_id = phyrst->phy_id;

	switch (id) {
	case CDNS_PHY_LINK_RESET:
		break;
	case CDNS_PHY_RESET:
		regmap_update_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], PCIEC_GEN_RESET, GEN3);
		regmap_update_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], PCIEC_LANE_RESET, LANE_COUNT_X4);
		regmap_clear_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], PCIEC_LINK_TRAIN_EN);
		regmap_set_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], PCIEC_MISC_RESET);
		regmap_set_bits(phyrst->regmap, phyrst->offset[P_CTRL_REG], PCIEP_PHY_RESET(phy_id));
		break;
	}

	return 0;
}

static int amba_phyrst_pcie_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct amba_phyrst *phyrst = to_amba_phyrst(rcdev);
	u32 phy_id = phyrst->phy_id;

	switch (id) {
	case CDNS_PHY_LINK_RESET:
		break;
	case CDNS_PHY_RESET:
		regmap_clear_bits(phyrst->regmap, phyrst->offset[P_CTRL_REG], PCIEP_PHY_RESET(phy_id));
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
		regmap_clear_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], PCIEC_MISC_RESET);
		regmap_set_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], PCIEC_LINK_TRAIN_EN);
		break;
	}

	return 0;
}

static int amba_phyrst_usb32_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct amba_phyrst *phyrst = to_amba_phyrst(rcdev);

	switch(id) {
	case CDNS_PHY_LINK_RESET:
		break;
	case CDNS_PHY_RESET:
		regmap_set_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], USB32C_SOFT_RESET);
		regmap_set_bits(phyrst->regmap, phyrst->offset[P_CTRL_REG], USB32P_PHY_RESET);
		break;
	}

	return 0;
}

static int amba_phyrst_usb32_deassert(struct reset_controller_dev *rcdev,
			unsigned long id)
{
	struct amba_phyrst *phyrst = to_amba_phyrst(rcdev);

	switch(id) {
	case CDNS_PHY_LINK_RESET:
		break;
	case CDNS_PHY_RESET:
		regmap_clear_bits(phyrst->regmap, phyrst->offset[P_CTRL_REG], USB32P_PHY_RESET);
		regmap_clear_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], USB32C_SOFT_RESET);
		break;
	}

	return 0;
}

static const struct reset_control_ops amba_phyrst_pcie_ops = {
	.assert   = amba_phyrst_pcie_assert,
	.deassert = amba_phyrst_pcie_deassert,
};

static const struct reset_control_ops amba_phyrst_usb32_ops = {
	.assert   = amba_phyrst_usb32_assert,
	.deassert = amba_phyrst_usb32_deassert,
};

static int amba_phyrst_pcie_init(struct amba_phyrst *phyrst, struct device_node *np)
{
	struct device_node *ctrlr_np = of_parse_phandle(np, "amb,pcie-controller", 0);
	u32 mask, val, phy_id = phyrst->phy_id;

	mask = PCIEC_MODE_SELECT_RP | PCIEC_CONFIG_EN;

	if (of_device_is_compatible(ctrlr_np, "ambarella,cdns-pcie-ep"))
		val = PCIEC_CONFIG_EN;
	else
		val = PCIEC_MODE_SELECT_RP;

	mask |= PCIEC_APB_CORE_RATIO_MASK;
	val |= PCIEC_APB_CORE_RATIO_4;

	regmap_update_bits(phyrst->regmap, phyrst->offset[C_CTRL_REG], mask, val);

	/* PHY reference clock is fixed at 100Mhz */
	regmap_update_bits(phyrst->regmap, phyrst->offset[PMA_CTRL_REG],
					PCIE_PMA_CMN_REFCLK_DIG_DIV_MASK,
					PCIE_PMA_CMN_REFCLK_DIG_DIV_4);

	/* Release PCIe PHY APB reset to allow access to PCS/PMA registers */
	regmap_clear_bits(phyrst->regmap, phyrst->offset[P_CTRL_REG], PCIEP_APB_RESET(phy_id));

	return 0;
}

static int amba_phyrst_usb32_init(struct amba_phyrst *phyrst, struct device_node *np)
{
	/* PHY reference clock is fixed at 100Mhz */
	regmap_update_bits(phyrst->regmap, phyrst->offset[PMA_CTRL_REG],
					USB32_PMA_CMN_REFCLK_DIG_DIV_MASK,
					USB32_PMA_CMN_REFCLK_DIG_DIV_4);

	/* Release USB32 PHY APB reset to allow access to PCS/PMA registers */
	regmap_clear_bits(phyrst->regmap, phyrst->offset[P_CTRL_REG], USB32P_APB_RESET);

	return 0;
}

static const struct amba_phyrst_of_data amba_phyrst_pcie_of_data = {
	.init = amba_phyrst_pcie_init,
	.ops = &amba_phyrst_pcie_ops,
};

static const struct amba_phyrst_of_data amba_phyrst_usb32_of_data = {
	.init = amba_phyrst_usb32_init,
	.ops = &amba_phyrst_usb32_ops,
};

static const struct of_device_id amba_phyrst_dt_ids[] = {
	{ .compatible = "ambarella,usb32-phyrst", .data = &amba_phyrst_usb32_of_data },
	{ .compatible = "ambarella,pcie-phyrst", .data = &amba_phyrst_pcie_of_data },
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

	match = of_match_device(amba_phyrst_dt_ids, dev);
	if (!match)
		return -EINVAL;

	data = (struct amba_phyrst_of_data *)match->data;

	phyrst = devm_kzalloc(dev, sizeof(*phyrst), GFP_KERNEL);
	if(!phyrst)
		return -ENOMEM;

	phyrst->regmap = syscon_regmap_lookup_by_phandle_args(np,
				"amb,scr-regmap", NUM_REG, phyrst->offset);
	if (IS_ERR(phyrst->regmap)) {
		dev_err(dev, "regmap lookup failed.\n");
		return PTR_ERR(phyrst->regmap);
	}

	of_property_read_u32(np, "amb,usb32-phy-id", &phyrst->phy_id);
	of_property_read_u32(np, "amb,pcie-phy-id", &phyrst->phy_id);

	pwr_gpio = of_get_named_gpio_flags(np, "pwr-gpios", 0, &flags);
	if (gpio_is_valid(pwr_gpio)) {
		u32 gpio_init_flag;
		char label[64];

		if (flags & OF_GPIO_ACTIVE_LOW)
			gpio_init_flag = GPIOF_OUT_INIT_LOW;
		else
			gpio_init_flag = GPIOF_OUT_INIT_HIGH;

		snprintf(label, sizeof(label), "%s.%d", np->name, phyrst->phy_id);

		rval = devm_gpio_request_one(dev, pwr_gpio, gpio_init_flag, label);
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

MODULE_AUTHOR("Xuliang Zhang <xlzhanga@ambarella.com>");
MODULE_AUTHOR("Jian He <jianhe@ambarella.com>");
MODULE_DESCRIPTION("Ambarella Cadence PHY reset driver");
MODULE_LICENSE("GPL v2");
