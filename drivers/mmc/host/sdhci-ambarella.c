/*
 * /drivers/mmc/host/sdhci-ambarella.c
 *
 * Copyright (C) 2004-2099, Ambarella, Inc.
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
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/gpio/consumer.h>

#include "sdhci-pltfm.h"

#define BOUNDARY_OK(addr, len) \
	((addr | (SZ_128M - 1)) == ((addr + len - 1) | (SZ_128M - 1)))

struct sdhci_ambarella_host {
	struct gpio_desc *power_gpio;
	struct gpio_desc *v18_gpio;
};

static void sdhci_ambarella_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host;
	u32 sd_clk;
	u16 clk;

	pltfm_host = sdhci_priv(host);
	host->mmc->actual_clock = 0;

	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (clock == 0)
		return;

	sd_clk = min_t(u32, clock, host->mmc->f_max);
	clk_set_rate(pltfm_host->clk, sd_clk);

	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	sdhci_enable_clk(host, clk);
}

static void sdhci_ambarella_set_power(struct sdhci_host *host, unsigned char mode,
		     unsigned short vdd)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_ambarella_host *ambarella_host = sdhci_pltfm_priv(pltfm_host);

	switch (mode) {
	case MMC_POWER_ON:
		break;
	case MMC_POWER_OFF:
		if(ambarella_host->power_gpio)
			gpiod_set_value_cansleep(ambarella_host->power_gpio, 0);
		break;
	case MMC_POWER_UP:
		if(ambarella_host->power_gpio)
			gpiod_set_value_cansleep(ambarella_host->power_gpio, 1);
		break;
	}

	sdhci_set_power_noreg(host, mode, vdd);
}

static int sdhci_ambarella_voltage_switch(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_ambarella_host *ambarella_host = sdhci_pltfm_priv(pltfm_host);

	switch (ios->signal_voltage) {
	case MMC_SIGNAL_VOLTAGE_180:
		if(ambarella_host->v18_gpio)
			gpiod_set_value_cansleep(ambarella_host->v18_gpio, 1);
		break;

	default:
		/* fall-through */
	case MMC_SIGNAL_VOLTAGE_330:
		if(ambarella_host->v18_gpio)
			gpiod_set_value_cansleep(ambarella_host->v18_gpio, 0);
		break;
	}

	return sdhci_start_signal_voltage_switch(mmc, ios);
}

static unsigned int sdhci_ambarella_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);

	return clk_get_rate(clk_get_parent(pltfm_host->clk));
}

unsigned int sdhci_ambarella_get_min_clock(struct sdhci_host *host)
{
	return 400000;
}

/*
 * If DMA addr spans 128MB boundary, we split the DMA transfer into two
 * so that each DMA transfer doesn't exceed the boundary.
 */
static void sdhci_ambarella_adma_write_desc(struct sdhci_host *host, void **desc,
				    dma_addr_t addr, int len, unsigned int cmd)
{
	int tmplen, offset;

	if (likely(!len || BOUNDARY_OK(addr, len))) {
		sdhci_adma_write_desc(host, desc, addr, len, cmd);
		return;
	}

	offset = addr & (SZ_128M - 1);
	tmplen = SZ_128M - offset;
	sdhci_adma_write_desc(host, desc, addr, tmplen, cmd);

	addr += tmplen;
	len -= tmplen;
	sdhci_adma_write_desc(host, desc, addr, len, cmd);
}

static struct sdhci_ops sdhci_ambarella_ops = {
	.set_clock = sdhci_ambarella_set_clock,
	.set_power = sdhci_ambarella_set_power,
	.get_max_clock = sdhci_ambarella_get_max_clock,
	.get_min_clock = sdhci_ambarella_get_min_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = sdhci_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
	.adma_write_desc	= sdhci_ambarella_adma_write_desc,
};

static const struct sdhci_pltfm_data sdhci_ambarella_pdata = {
	.quirks = SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN |
		  SDHCI_QUIRK_INVERTED_WRITE_PROTECT,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN | SDHCI_QUIRK2_BROKEN_DDR50,
	.ops = &sdhci_ambarella_ops,
};

static int sdhci_ambarella_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_ambarella_host *ambarella_host;
	int ret;

	host = sdhci_pltfm_init(pdev, &sdhci_ambarella_pdata, sizeof(*ambarella_host));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	ambarella_host = sdhci_pltfm_priv(pltfm_host);
	pltfm_host->clk = devm_clk_get(&pdev->dev, NULL);

	if (IS_ERR(pltfm_host->clk)) {
		dev_err(&pdev->dev, "Get PLL failed!\n");
		ret = PTR_ERR(pltfm_host->clk);
		goto free_pltfm;
	}

	host->mmc_host_ops.start_signal_voltage_switch =
		sdhci_ambarella_voltage_switch;

	ambarella_host->power_gpio = devm_gpiod_get_optional(&pdev->dev,
							 "pwr",
							 GPIOD_OUT_HIGH);
	if (IS_ERR(ambarella_host->power_gpio)) {
		dev_err(&pdev->dev, "Invalid power GPIO\n");
		ret = PTR_ERR(ambarella_host->power_gpio);
		goto free_pltfm;
	}

	ambarella_host->v18_gpio = devm_gpiod_get_optional(&pdev->dev,
							 "v18",
							 GPIOD_OUT_LOW);
	if (IS_ERR(ambarella_host->v18_gpio)) {
		dev_err(&pdev->dev, "Invalid v18 GPIO\n");
		ret = PTR_ERR(ambarella_host->v18_gpio);
		goto free_pltfm;
	}

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto free_pltfm;

	sdhci_get_of_property(pdev);

	sdhci_read_caps(host);
	/* if there is no 1.8v switch, clear UHS-I modes */
	if(ambarella_host->v18_gpio == NULL)
		host->caps1 &= ~(SDHCI_SUPPORT_SDR104 | SDHCI_SUPPORT_SDR50 |
			 SDHCI_SUPPORT_DDR50);

	sdhci_enable_v4_mode(host);

	ret = sdhci_add_host(host);
	if (ret)
		goto free_pltfm;

	return 0;

free_pltfm:
	sdhci_pltfm_free(pdev);
	return ret;
}

static int sdhci_ambarella_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);

	sdhci_remove_host(host, 0);
	sdhci_pltfm_free(pdev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int sdhci_ambarella_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	int ret;

	if (host->tuning_mode != SDHCI_TUNING_MODE_3)
		mmc_retune_needed(host->mmc);

	ret = sdhci_suspend_host(host);

	return ret;
}

static int sdhci_ambarella_resume(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	int ret;

	ret = sdhci_resume_host(host);

	return ret;
}

static const struct dev_pm_ops sdhci_ambarella_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sdhci_ambarella_suspend, sdhci_ambarella_resume)
};
#endif

static const struct of_device_id sdhci_ambarella_dt_match[] = {
	{ .compatible = "ambarella,sdhci"},
	{}
};
MODULE_DEVICE_TABLE(of, sdhci_ambarella_dt_match);

static struct platform_driver sdhci_ambarella_driver = {
	.probe = sdhci_ambarella_probe,
	.remove = sdhci_ambarella_remove,
	.driver = {
		   .name = "sdhci_ambarella",
		   .pm = &sdhci_ambarella_pm_ops,
		   .of_match_table = sdhci_ambarella_dt_match,
	},
};

module_platform_driver(sdhci_ambarella_driver);

MODULE_DESCRIPTION("Ambarella Secure Digital Host Controller Interface driver");
MODULE_LICENSE("GPL v2");

