// SPDX-License-Identifier: GPL-2.0
/**
 * cdns3-ambarella.c - AMBARELLA Specific Glue layer for Cadence USB Controller
 *
 * Copyright (C) 2021 by Ambarella, Inc.
 * http://www.ambarella.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/io.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
#include <linux/sys_soc.h>
#include "core.h"


#define USB32C_CTRL_OFFSET			0x16c
#define USB32C_RESET_MASK			(0x1)
#define USB32C_MODE_STRAP_MASK 		(0x6)
#define USB32C_MODE_STRAP_SHIFT		(1)

#define USBC_CTRL_OFFSET			0x12c
#define USBC_HOST_OCP_MASK			BIT(1)
#define USBC_HOST_OCP_SHIFT			(1)

/* Modestrap modes */
enum modestrap_mode {
	MODE_STRAP_MODE_NONE,
	MODE_STRAP_MODE_HOST,
	MODE_STRAP_MODE_PERIPHERAL
};

static unsigned int mode_strap = MODE_STRAP_MODE_PERIPHERAL;
module_param(mode_strap, uint, 0644);
MODULE_PARM_DESC(mode_strap, "mode_strap.");

struct cdns_ambarella {
	struct device *dev;
	//void __iomem *usbss;
	struct regmap	*scr_reg;
	struct platform_device *cdns3_pdev;
	int hub_rst_pin;
	int rst_active;
	int hub_pwr_pin;
	int pwr_active;
	u32 ovrcur_pol_inv;
};

static const struct soc_device_attribute ambarella_cdnsp_socinfo[] = {
	{ .soc_id = "cv72" },
	{ .soc_id = "cv3ad685" },
	{/* sentinel */}
};

static int cdns_ambarella_platform_suspend(struct device *dev,
	bool suspend, bool wakeup)
{
	/* TODO */
	return 0;
}

static int cdns_ambarella_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct of_dev_auxdata *cdns_ambarella_auxdata;
	struct cdns3_platform_data *cdns_ambarella_pdata;
	struct cdns_ambarella *data;
	enum of_gpio_flags flags;
	u32 ovrcur_pol;
	int ret;

	if (!node)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);
	data->dev = dev;

#if 0
	data->usbss = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->usbss)) {
		dev_err(dev, "can't map IOMEM resource\n");
		return PTR_ERR(data->usbss);
	}
#endif
	data->scr_reg = syscon_regmap_lookup_by_phandle(node, "amb,scr-regmap");
	if (IS_ERR(data->scr_reg)) {
		dev_err(dev, "no scr regmap!\n");
		return PTR_ERR(data->scr_reg);
	}

	/* Set default mode (mode_strap) to be actived after power on reset */
	regmap_update_bits(data->scr_reg,
			USB32C_CTRL_OFFSET,
			USB32C_MODE_STRAP_MASK, mode_strap << USB32C_MODE_STRAP_SHIFT);

	if (soc_device_match(ambarella_cdnsp_socinfo)) {
		/* Default OCP is low with bit1 = 1; while bit1 = 0, ocp high */
		ret = of_property_read_u32(node, "amb,ocp-polarity", &ovrcur_pol);
		if (ret < 0)
			ovrcur_pol = 0;
		data->ovrcur_pol_inv = !ovrcur_pol;
		regmap_update_bits(data->scr_reg,
			USBC_CTRL_OFFSET,
			USBC_HOST_OCP_MASK, data->ovrcur_pol_inv << USBC_HOST_OCP_SHIFT);
	}

	data->hub_pwr_pin = of_get_named_gpio_flags(node, "hub-pwr-gpios", 0, &flags);
	data->pwr_active = !!(flags & OF_GPIO_ACTIVE_LOW);

	data->hub_rst_pin = of_get_named_gpio_flags(node, "hub-rst-gpios", 0, &flags);
	data->rst_active = !!(flags & OF_GPIO_ACTIVE_LOW);

	/* request gpio for HUB power */
	if (gpio_is_valid(data->hub_pwr_pin)) {
		ret = devm_gpio_request(dev, data->hub_pwr_pin, "usb3 hub power");
		if (ret < 0) {
			dev_err(dev, "Failed to request hub power pin %d\n", ret);
			return ret;
		}
		gpio_direction_output(data->hub_pwr_pin, data->pwr_active);
	}

	/* request gpio for HUB reset */
	if (gpio_is_valid(data->hub_rst_pin)) {
		ret = devm_gpio_request(dev, data->hub_rst_pin, "usb3 hub reset");
		if (ret < 0) {
			dev_err(dev, "Failed to request hub reset pin %d\n", ret);
			return ret;
		}
		gpio_direction_output(data->hub_rst_pin, data->rst_active);
		msleep(10);
		gpio_direction_output(data->hub_rst_pin, !data->rst_active);
	}

	/* Zero out all data because of_dev_lookup will iterate over the array */
	cdns_ambarella_auxdata =
		devm_kmalloc_array(dev, 2, sizeof(*cdns_ambarella_auxdata), GFP_KERNEL | __GFP_ZERO);
	if (!cdns_ambarella_auxdata)
		return -ENOMEM;

	cdns_ambarella_auxdata[0].compatible = "cdns,usb3";

	/* pdata would be kfree when depopulate, so don't use resource management family */
	cdns_ambarella_pdata =
		kzalloc(sizeof(*cdns_ambarella_pdata), GFP_KERNEL);
	if (!cdns_ambarella_pdata)
		return -ENOMEM;

	cdns_ambarella_pdata->platform_suspend =
		cdns_ambarella_platform_suspend;
	cdns_ambarella_pdata->quirks = CDNS3_DEFAULT_PM_RUNTIME_ALLOW;
	cdns_ambarella_auxdata[0].platform_data = cdns_ambarella_pdata;
	cdns_ambarella_auxdata[0].name = "cdns3";

	ret = of_platform_populate(node, NULL, cdns_ambarella_auxdata, dev);
	if (ret) {
		dev_err(dev, "failed to create children: %d\n", ret);
		goto err;
	}

err:
	return ret;
}

static int cdns_ambarella_remove(struct platform_device *pdev)
{
	struct cdns_ambarella *data = platform_get_drvdata(pdev);

	of_platform_depopulate(&pdev->dev);

	regmap_set_bits(data->scr_reg, USB32C_CTRL_OFFSET, USB32C_RESET_MASK);
	regmap_clear_bits(data->scr_reg, USB32C_CTRL_OFFSET, USB32C_RESET_MASK);

	return 0;
}

static const struct of_device_id cdns_ambarella_of_match[] = {
	{ .compatible = "ambarella,cdns-usb3", },
	{},
};
MODULE_DEVICE_TABLE(of, cdns_ambarella_of_match);

static struct platform_driver cdns_ambarella_driver = {
	.probe		= cdns_ambarella_probe,
	.remove		= cdns_ambarella_remove,
	.driver		= {
		.name	= "cdns3-ambarella",
		.of_match_table	= cdns_ambarella_of_match,
	},
};
module_platform_driver(cdns_ambarella_driver);

MODULE_ALIAS("platform:cdns3-ambarella");
MODULE_AUTHOR("Ken He <jianhe@ambarella.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Cadence USB3 Ambarella Glue Layer");
