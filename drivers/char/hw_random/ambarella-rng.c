/*
 * Copyright (c) 2017 Ambarella, Inc.
 *
 * This program is free software; you can distribute it and/or modify it
 * under the terms of the GNU General Public License (Version 2) as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

/* ==========================================================================*/

#define RNG_CTRL_OFFSET			0x1A4
#define RNG_CTRL_PD			(1 << 0)

#define	AMBA_RNG_CONTROL		0x00
#define	AMBA_RNG_DATA			0x04

#define AMBA_RNG_DELAY_MS		200
#define AMBA_RNG_HW_MAX_SIZE		16

/* ==========================================================================*/

struct ambarella_rng_private {
	struct hwrng rng;
	void __iomem *base;
	struct regmap *rct_reg;
};

static int ambarella_rng_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	struct ambarella_rng_private *priv;
	u32 val, start_time, timeout, *data = buf;
	size_t read = 0;
	size_t maxsize;

	priv = container_of(rng, struct ambarella_rng_private, rng);

	/* calculate max size bytes can be transfered back */
	maxsize = min_t(size_t, AMBA_RNG_HW_MAX_SIZE, max);

	/* start rng */
	val = readl(priv->base + AMBA_RNG_CONTROL);
	val |= BIT(1);
	writel(val, priv->base + AMBA_RNG_CONTROL);

	start_time = jiffies_to_msecs(jiffies);

	while ((readl(priv->base + AMBA_RNG_CONTROL) & BIT(1)) && wait) {
		timeout = jiffies_to_msecs(jiffies) - start_time;
		if (timeout < AMBA_RNG_DELAY_MS) {
			cpu_relax();
		} else {
			pr_info("ambarella hwrng timeout\n");
			read = -ETIMEDOUT;
			goto out;
		}
	}

	while (maxsize >= sizeof(u32)) {
		*data++ = readl(priv->base + AMBA_RNG_DATA + read);
		read += sizeof(u32);
		maxsize -= sizeof(u32);
	}

out:
	return read;
}

static int ambarella_rng_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ambarella_rng_private *priv;
	u32 val, rate = 32000;

	dev_info(&pdev->dev, "rng probed\r\n");

	priv = devm_kzalloc(&pdev->dev, sizeof(struct ambarella_rng_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(priv->base);
	}

	priv->rct_reg = syscon_regmap_lookup_by_phandle(np, "amb,rct-regmap");
	if (IS_ERR(priv->rct_reg)) {
		dev_err(&pdev->dev, "no rct regmap!\n");
		return -ENODEV;
	}

	priv->rng.name = dev_driver_string(&pdev->dev);
	priv->rng.read = ambarella_rng_read;
	priv->rng.priv = (unsigned long)&pdev->dev;

	platform_set_drvdata(pdev, priv);

	/* set sample rate, if not set in dts, use 32K as default */
	if (of_property_read_u32(np, "amb,sample-rate", &rate) < 0)
		rate = 32000;

	val = readl(priv->base + AMBA_RNG_CONTROL);
	val &= ~(0x3 << 4);

	switch (rate) {
	case 256000:
		val |= (0x3 << 4);
		break;
	case 128000:
		val |= (0x2 << 4);
		break;
	case 64000:
		val |= (0x1 << 4);
		break;
	case 32000:
		val |= (0x0 << 4);
		break;
	default:
		pr_info("set wrong sample rate, use default 32K.\n");
		break;
	}

	writel(val, priv->base + AMBA_RNG_CONTROL);

	/* power up rng */
	regmap_update_bits(priv->rct_reg, RNG_CTRL_OFFSET, RNG_CTRL_PD, 0x0);

	return devm_hwrng_register(&pdev->dev, &priv->rng);
}

static const struct of_device_id ambarella_rng_match[] = {
	{
		.compatible = "ambarella,hw-rng",
	},
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_rng_match);

static struct platform_driver ambarella_rng_driver = {
	.driver = {
		.name = "ambarella-hwrng",
		.of_match_table = ambarella_rng_match,
	},
	.probe = ambarella_rng_probe,
};

module_platform_driver(ambarella_rng_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kai Qiang <kqiang@ambarella.com>");
MODULE_DESCRIPTION("Ambarella RNG device driver");
