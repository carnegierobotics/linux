/*
 * drivers/pwm/pwm-ambarella.c
 *
 * History:
 *	2014/03/19 - [Cao Rongrong] Created
 *
 * Copyright (C) 2012-2016, Ambarella, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>

/* ==========================================================================*/

#define PWM_B0_ENABLE_OFFSET		0x04
#define PWM_B1_ENABLE_OFFSET		0x0c
#define PWM_C0_ENABLE_OFFSET		0x14
#define PWM_C1_ENABLE_OFFSET		0x1c
#define PWM_B0_DATA1_OFFSET		0x20
#define PWM_B1_DATA1_OFFSET		0x24
#define PWM_C0_DATA1_OFFSET		0x28
#define PWM_C1_DATA1_OFFSET		0x2c

#define PWM_CLK_SRC_BIT			(0x1 << 31)
#define PWM_INV_EN_BIT			(0x1 << 31)
#define PWM_DIVIDER_MASK		(0x7ffffffe)
#define PWM_PWM_EN_BIT			(0x1)

#define PWM_PIN_NUM			4
#define PWM_TICKS_MAX_BITS		16
#define PWM_DEFAULT_FREQUENCY		2000000

/* ==========================================================================*/

struct ambarella_pwm_chip {
	struct pwm_chip chip;
	void __iomem *base;
	struct clk *clk;
	u32 clk_rate;
	enum pwm_polarity polarity[PWM_PIN_NUM];
};

#define to_ambarella_pwm_chip(_chip) container_of(_chip, struct ambarella_pwm_chip, chip)

static u32 ambpwm_enable_offset[] = {
	PWM_B0_ENABLE_OFFSET,
	PWM_B1_ENABLE_OFFSET,
	PWM_C0_ENABLE_OFFSET,
	PWM_C1_ENABLE_OFFSET,
};

static u32 ambpwm_data_offset[] = {
	PWM_B0_DATA1_OFFSET,
	PWM_B1_DATA1_OFFSET,
	PWM_C0_DATA1_OFFSET,
	PWM_C1_DATA1_OFFSET,
};

static bool ambpwm_is_chief[] = {true, false, true, false};

static int ambarella_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			int duty_ns, int period_ns)
{
	struct ambarella_pwm_chip *ambpwm = to_ambarella_pwm_chip(chip);
	void __iomem *reg;
	u32 val, div, xon, xoff;
	u64 on, off, pwm_clock;

	reg = ambpwm->base + ambpwm_enable_offset[pwm->hwpwm];
	val = readl_relaxed(reg);
	if (ambpwm_is_chief[pwm->hwpwm])
		val |= PWM_CLK_SRC_BIT;
	else
		val &= ~PWM_INV_EN_BIT;
	writel_relaxed(val, reg);

	pwm_clock = clk_get_rate(ambpwm->clk);
	for (div = 1; div <= (1 << 30); div++) {
		on = pwm_clock * (u64)duty_ns;
		do_div(on, div);
		do_div(on, 1000000000);

		off = pwm_clock * (u64)(period_ns - duty_ns);
		do_div(off, div);
		do_div(off, 1000000000);

		if ((on <= (1 << 16)) && (off <= (1 << 16)))
			break;
	}

	if (div > (1 << 30))
		return -EINVAL;

	reg = ambpwm->base + ambpwm_enable_offset[pwm->hwpwm];
	val = readl_relaxed(reg);
	val &= ~PWM_DIVIDER_MASK;
	val |= (div - 1) << 1;
	writel_relaxed(val, reg);

	if (on == 0)
		on = 1;
	if (off == 0)
		off = 1;

	xon = (u32)on - 1;
	xoff = (u32)off -1;
	reg = ambpwm->base + ambpwm_data_offset[pwm->hwpwm];
	if (ambpwm->polarity[pwm->hwpwm] == PWM_POLARITY_NORMAL)
		writel_relaxed(xon << PWM_TICKS_MAX_BITS | xoff, reg);
	else
		writel_relaxed(xoff << PWM_TICKS_MAX_BITS | xon, reg);

	return 0;
}

static int ambarella_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ambarella_pwm_chip *ambpwm = to_ambarella_pwm_chip(chip);
	void __iomem *reg = ambpwm->base + ambpwm_enable_offset[pwm->hwpwm];
	u32 val;

	val = readl_relaxed(reg);
	val |= PWM_PWM_EN_BIT;
	writel_relaxed(val, reg);

	return 0;
}

static void ambarella_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ambarella_pwm_chip *ambpwm = to_ambarella_pwm_chip(chip);
	void __iomem *reg = ambpwm->base + ambpwm_enable_offset[pwm->hwpwm];
	u32 val;

	val = readl_relaxed(reg);
	val &= ~PWM_PWM_EN_BIT;
	writel_relaxed(val, reg);
}

static int ambarella_pwm_set_polarity(struct pwm_chip *chip,
			struct pwm_device *pwm,	enum pwm_polarity polarity)
{
	struct ambarella_pwm_chip *ambpwm = to_ambarella_pwm_chip(chip);

	ambpwm->polarity[pwm->hwpwm] = polarity;

	return 0;
}

static const struct pwm_ops ambarella_pwm_ops = {
	.config		= ambarella_pwm_config,
	.enable		= ambarella_pwm_enable,
	.disable	= ambarella_pwm_disable,
	.set_polarity	= ambarella_pwm_set_polarity,
	.owner		= THIS_MODULE,
};

static int ambarella_pwm_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ambarella_pwm_chip *ambpwm;
	struct resource *res;
	u32 clk_rate;
	int ret;

	ambpwm = devm_kzalloc(&pdev->dev, sizeof(*ambpwm), GFP_KERNEL);
	if (!ambpwm)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ambpwm->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ambpwm->base))
		return PTR_ERR(ambpwm->base);

	ambpwm->clk = devm_clk_get(&pdev->dev, "gclk_pwm");
	if (IS_ERR(ambpwm->clk)) {
		dev_err(&pdev->dev, "failed to get clock, err = %ld\n",
			PTR_ERR(ambpwm->clk));
		return PTR_ERR(ambpwm->clk);
	}

	if (of_property_read_u32(np, "clock-frequency", &clk_rate) < 0)
		clk_rate = PWM_DEFAULT_FREQUENCY;

	clk_set_rate(ambpwm->clk, clk_rate);

	ambpwm->chip.dev = &pdev->dev;
	ambpwm->chip.ops = &ambarella_pwm_ops;
	ambpwm->chip.of_xlate = of_pwm_xlate_with_flags;
	ambpwm->chip.of_pwm_n_cells = 3;
	ambpwm->chip.base = -1;
	ambpwm->chip.npwm = PWM_PIN_NUM;

	ret = pwmchip_add(&ambpwm->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add pwm chip %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, ambpwm);

	return 0;
}

static int ambarella_pwm_remove(struct platform_device *pdev)
{
	struct ambarella_pwm_chip *ambpwm = platform_get_drvdata(pdev);

	pwmchip_remove(&ambpwm->chip);

	return 0;
}

#ifdef CONFIG_PM
static int ambarella_pwm_suspend(struct platform_device *pdev,
				 pm_message_t state)
{
	struct ambarella_pwm_chip *ambpwm = platform_get_drvdata(pdev);

	ambpwm->clk_rate = clk_get_rate(ambpwm->clk);
	return 0;
}

static int ambarella_pwm_resume(struct platform_device *pdev)
{
	struct ambarella_pwm_chip *ambpwm = platform_get_drvdata(pdev);

	clk_set_rate(ambpwm->clk, ambpwm->clk_rate);
	return 0;
}
#endif

static const struct of_device_id ambarella_pwm_dt_ids[] = {
	{ .compatible = "ambarella,pwm", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ambarella_pwm_dt_ids);

static struct platform_driver ambarella_pwm_driver = {
	.driver = {
		.name = "ambarella-pwm",
		.of_match_table = of_match_ptr(ambarella_pwm_dt_ids),
	},
	.probe = ambarella_pwm_probe,
	.remove = ambarella_pwm_remove,
#ifdef CONFIG_PM
	.suspend = ambarella_pwm_suspend,
	.resume	 = ambarella_pwm_resume,
#endif
};
module_platform_driver(ambarella_pwm_driver);

MODULE_ALIAS("platform:ambarella-pwm");
MODULE_AUTHOR("Cao Rongrong <rrcao@ambarella.com>");
MODULE_DESCRIPTION("Ambarella PWM Driver");
MODULE_LICENSE("GPL v2");

