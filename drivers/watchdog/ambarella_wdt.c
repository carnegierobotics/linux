// SPDX-License-Identifier: GPL-2.0+
/*
 * Watchdog driver for Ambarella SoCs.
 *
 * Copyright (C) 2016-2048, Ambarella, Inc.
 *
 */

#include <linux/module.h>
#include <linux/watchdog.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

/* ==========================================================================*/

#define WDOG_STATUS_OFFSET		0x00
#define WDOG_RELOAD_OFFSET		0x04
#define WDOG_RESTART_OFFSET		0x08
#define WDOG_CONTROL_OFFSET		0x0c
#define WDOG_TIMEOUT_OFFSET		0x10
#define WDOG_CLR_TMO_OFFSET		0x14
#define WDOG_RST_WD_OFFSET		0x18

#define WDOG_CTR_EXT_EN			0x00000008
#define WDOG_CTR_INT_EN			0x00000004
#define WDOG_CTR_RST_EN			0x00000002
#define WDOG_CTR_EN			0x00000001

/*
 * WDOG_RESTART_REG only works with magic 0x4755.
 * Set this value would transferred the value in
 * WDOG_RELOAD_REG into WDOG_STATUS_REG and would
 * not trigger the underflow event.
 */
#define WDT_RESTART_VAL			0x4755

/* ==========================================================================*/

#define WDT_RST_L_OFFSET		0x78
#define UNLOCK_WDT_RST_L_OFFSET		0x260

/* ==========================================================================*/

#define AMBARELLA_WDT_MAX_CYCLE		0xffffffff

static int heartbeat; /* get value from FDT */
module_param(heartbeat, int, 0);
MODULE_PARM_DESC(heartbeat, "Initial watchdog heartbeat in seconds");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct ambarella_wdt {
	void __iomem	 		*regbase;
	struct regmap			*rct_reg;
	struct clk			*clk;
	int				irq;
	struct watchdog_device		wdd;
	bool				ext_pin;
	u32				timeout; /* in cycle */
};

static int ambarella_wdt_start(struct watchdog_device *wdd)
{
	struct ambarella_wdt *ambwdt;
	u32 ctrl_val;

	ambwdt = watchdog_get_drvdata(wdd);

	if (ambwdt->ext_pin)
		ctrl_val = WDOG_CTR_EXT_EN;
	else if (ambwdt->irq > 0)
		ctrl_val = WDOG_CTR_INT_EN;
	else
		ctrl_val = WDOG_CTR_RST_EN;

	ctrl_val |= WDOG_CTR_EN;

	if (ctrl_val & WDOG_CTR_INT_EN)
		writel(0x0, ambwdt->regbase + WDOG_RST_WD_OFFSET);
	else
		writel(0xFF, ambwdt->regbase + WDOG_RST_WD_OFFSET);

	writel(ctrl_val, ambwdt->regbase + WDOG_CONTROL_OFFSET);

	return 0;
}

static int ambarella_wdt_stop(struct watchdog_device *wdd)
{
	struct ambarella_wdt *ambwdt = watchdog_get_drvdata(wdd);

	writel(0, ambwdt->regbase + WDOG_CONTROL_OFFSET);

	return 0;
}

static int ambarella_wdt_keepalive(struct watchdog_device *wdd)
{
	struct ambarella_wdt *ambwdt = watchdog_get_drvdata(wdd);

	writel(ambwdt->timeout, ambwdt->regbase + WDOG_RELOAD_OFFSET);
	writel(WDT_RESTART_VAL, ambwdt->regbase + WDOG_RESTART_OFFSET);

	return 0;
}

static int ambarella_wdt_set_timeout(struct watchdog_device *wdd, u32 time)
{
	struct ambarella_wdt *ambwdt = watchdog_get_drvdata(wdd);
	u32 freq;

	wdd->timeout = time;

	freq = clk_get_rate(ambwdt->clk);
	ambwdt->timeout = time * freq;
	writel(ambwdt->timeout, ambwdt->regbase + WDOG_RELOAD_OFFSET);

	return 0;
}

static irqreturn_t ambarella_wdt_irq(int irq, void *devid)
{
	struct ambarella_wdt *ambwdt = devid;

	writel(0x01, ambwdt->regbase + WDOG_CLR_TMO_OFFSET);

	dev_info(ambwdt->wdd.parent, "Watchdog timer expired!\n");

	return IRQ_HANDLED;
}

static const struct watchdog_info ambwdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "Ambarella Watchdog",
};

static const struct watchdog_ops ambwdt_ops = {
	.owner		= THIS_MODULE,
	.start		= ambarella_wdt_start,
	.stop		= ambarella_wdt_stop,
	.ping		= ambarella_wdt_keepalive,
	.set_timeout	= ambarella_wdt_set_timeout,
};

static int ambarella_wdt_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ambarella_wdt *ambwdt;
	u32 bootstatus;
	int rval = 0;

	ambwdt = devm_kzalloc(&pdev->dev, sizeof(*ambwdt), GFP_KERNEL);
	if (ambwdt == NULL) {
		dev_err(&pdev->dev, "Out of memory!\n");
		return -ENOMEM;
	}

	ambwdt->regbase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ambwdt->regbase)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(ambwdt->regbase);
	}

	ambwdt->rct_reg = syscon_regmap_lookup_by_phandle(np, "amb,rct-regmap");
	if (IS_ERR(ambwdt->rct_reg)) {
		dev_err(&pdev->dev, "no rct regmap!\n");
		return -ENODEV;
	}

	/* irq < 0 means to disable interrupt mode */
	ambwdt->irq = platform_get_irq_optional(pdev, 0);
	if (ambwdt->irq > 0) {
		rval = devm_request_irq(&pdev->dev, ambwdt->irq,
					ambarella_wdt_irq, IRQF_TRIGGER_RISING,
					dev_name(&pdev->dev), ambwdt);
		if (rval < 0) {
			dev_err(&pdev->dev, "Request IRQ failed!\n");
			return -ENXIO;
		}
	}

	ambwdt->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(ambwdt->clk)) {
		dev_err(&pdev->dev, "no clk for wdt!\n");
		return -ENODEV;
	}

	ambwdt->wdd.parent = &pdev->dev;
	ambwdt->wdd.info = &ambwdt_info;
	ambwdt->wdd.ops = &ambwdt_ops;
	ambwdt->wdd.min_timeout = 0;
	ambwdt->wdd.max_timeout = AMBARELLA_WDT_MAX_CYCLE / clk_get_rate(ambwdt->clk);
	regmap_read(ambwdt->rct_reg, WDT_RST_L_OFFSET, &bootstatus);
	ambwdt->wdd.bootstatus = bootstatus ? 0 : WDIOF_CARDRESET;

	if (!of_find_property(np, "amb,non-bootstatus", NULL)) {
		/* WDT_RST_L_REG cannot be restored by "reboot" command,
		 * so reset it manually */
		regmap_write(ambwdt->rct_reg, UNLOCK_WDT_RST_L_OFFSET, 0x1);
		regmap_update_bits(ambwdt->rct_reg, WDT_RST_L_OFFSET, 0x1, 0x1);
		regmap_write(ambwdt->rct_reg, UNLOCK_WDT_RST_L_OFFSET, 0x0);
	}

	if (!IS_ERR_OR_NULL(devm_pinctrl_get(&pdev->dev)))
		ambwdt->ext_pin = true;

	watchdog_init_timeout(&ambwdt->wdd, heartbeat, &pdev->dev);
	watchdog_set_nowayout(&ambwdt->wdd, nowayout);
	watchdog_set_drvdata(&ambwdt->wdd, ambwdt);

	ambarella_wdt_set_timeout(&ambwdt->wdd, ambwdt->wdd.timeout);
	ambarella_wdt_stop(&ambwdt->wdd);

	rval = watchdog_register_device(&ambwdt->wdd);
	if (rval < 0) {
		dev_err(&pdev->dev, "failed to register wdt!\n");
		return rval;
	}

	platform_set_drvdata(pdev, ambwdt);

	dev_notice(&pdev->dev, "Ambarella Watchdog Timer Probed.\n");

	return 0;
}

static int ambarella_wdt_remove(struct platform_device *pdev)
{
	struct ambarella_wdt *ambwdt;
	int rval = 0;

	ambwdt = platform_get_drvdata(pdev);

	ambarella_wdt_stop(&ambwdt->wdd);
	watchdog_unregister_device(&ambwdt->wdd);

	dev_notice(&pdev->dev, "Remove Ambarella Watchdog Timer.\n");

	return rval;
}

static void ambarella_wdt_shutdown(struct platform_device *pdev)
{
	struct ambarella_wdt *ambwdt;

	ambwdt = platform_get_drvdata(pdev);
	ambarella_wdt_stop(&ambwdt->wdd);

	dev_info(&pdev->dev, "%s @ %d.\n", __func__, system_state);
}

#ifdef CONFIG_PM

static int ambarella_wdt_suspend(struct platform_device *pdev,
			pm_message_t state)
{
	struct ambarella_wdt *ambwdt;
	int rval = 0;

	ambwdt = platform_get_drvdata(pdev);

	if (watchdog_active(&ambwdt->wdd))
		ambarella_wdt_stop(&ambwdt->wdd);

	dev_dbg(&pdev->dev, "%s exit with %d @ %d\n",
				__func__, rval, state.event);

	return rval;
}

static int ambarella_wdt_resume(struct platform_device *pdev)
{
	struct ambarella_wdt *ambwdt;
	int rval = 0;

	ambwdt = platform_get_drvdata(pdev);

	if (watchdog_active(&ambwdt->wdd)) {
		ambarella_wdt_start(&ambwdt->wdd);
		ambarella_wdt_keepalive(&ambwdt->wdd);
	}

	dev_dbg(&pdev->dev, "%s exit with %d\n", __func__, rval);

	return rval;
}
#endif

static const struct of_device_id ambarella_wdt_dt_ids[] = {
	{.compatible = "ambarella,wdt", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_wdt_dt_ids);

static struct platform_driver ambarella_wdt_driver = {
	.probe		= ambarella_wdt_probe,
	.remove		= ambarella_wdt_remove,
	.shutdown	= ambarella_wdt_shutdown,
#ifdef CONFIG_PM
	.suspend	= ambarella_wdt_suspend,
	.resume		= ambarella_wdt_resume,
#endif
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "ambarella-wdt",
		.of_match_table = ambarella_wdt_dt_ids,
	},
};

module_platform_driver(ambarella_wdt_driver);

MODULE_DESCRIPTION("Ambarella Media Processor Watch Dog Timer");
MODULE_AUTHOR("Anthony Ginger, <hfjiang@ambarella.com>");
MODULE_LICENSE("GPL");

