/*
 * Ambarella SoC reset code
 *
 * Author: Jorney < qtu@ambarella.com >
 *
 * Copyright (C) 2016-2029, Ambarella, Inc.
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

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#define SOFT_OR_DLL_RESET_OFFSET	0x68
#define SOFT_RESET			0x1

static struct regmap *reg_rct = NULL;

static int ambarella_restart_handler(struct notifier_block *this,
				unsigned long mode, void *cmd)
{
	local_irq_disable();
	regmap_update_bits(reg_rct, SOFT_OR_DLL_RESET_OFFSET, SOFT_RESET, 0x0);
	regmap_update_bits(reg_rct, SOFT_OR_DLL_RESET_OFFSET, SOFT_RESET, SOFT_RESET);

	return NOTIFY_DONE;
}

static struct notifier_block ambarella_restart_nb = {
	.notifier_call = ambarella_restart_handler,
	.priority = 128,
};

static int ambarella_reboot_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int err;

	reg_rct = syscon_regmap_lookup_by_phandle(np, "amb,rct-regmap");
	if (IS_ERR(reg_rct)) {
		dev_err(&pdev->dev, "no rct regmap!\n");
		return PTR_ERR(reg_rct);
	}

	err = register_restart_handler(&ambarella_restart_nb);
	if (err)
		dev_err(&pdev->dev, "cannot register restart handler (err=%d)\n", err);

	return err;
}

static const struct of_device_id ambarella_reboot_of_match[] = {
	{ .compatible = "ambarella,reboot" },
	{}
};

static struct platform_driver ambarella_reboot_driver = {
	.probe = ambarella_reboot_probe,
	.driver = {
		.name = "ambarella-reboot",
		.of_match_table = ambarella_reboot_of_match,
	},
};
module_platform_driver(ambarella_reboot_driver);
