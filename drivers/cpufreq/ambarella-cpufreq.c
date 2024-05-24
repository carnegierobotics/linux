/*
 * Copyright (C) 2017-2029, Ambarella, Inc.
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

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/clocksource.h>

/*
 * On Ambarella platforms, core clock is recommended to be adjusted on the fly
 * to save the power.
 */
#define CPUFREQ_CORTEX_MASK	(1 << 0)
#define CPUFREQ_CORE_MASK	(1 << 1)
#define CPUFREQ_SUPPORT_MAX	16

struct ambarella_cpufreq_t {
	u32 cpufreq_mask;
	struct clk *clk[2];
	struct cpufreq_frequency_table table[2][CPUFREQ_SUPPORT_MAX];
	struct device *dev;
	struct notifier_block clksrc_nb;
} *amb_cpufreq;

extern int ambarella_clksrc_nb_callback(struct notifier_block *nb,
		unsigned long event, void *data);

static struct cpufreq_frequency_table *ambarella_get_cpufreq_table(void)
{
	struct cpufreq_frequency_table *table;

	if (!amb_cpufreq->cpufreq_mask)
		return NULL;

	if (amb_cpufreq->cpufreq_mask & CPUFREQ_CORTEX_MASK)
		table = amb_cpufreq->table[0];
	else if (amb_cpufreq->cpufreq_mask & CPUFREQ_CORE_MASK)
		table = amb_cpufreq->table[1];
	else
		table = NULL;

	return table;
}

static struct clk *ambarella_get_cpufreq_clk(void)
{
	struct clk *clk;

	if (!amb_cpufreq->cpufreq_mask)
		return NULL;

	if (amb_cpufreq->cpufreq_mask & CPUFREQ_CORTEX_MASK)
		clk = amb_cpufreq->clk[0];
	else if (amb_cpufreq->cpufreq_mask & CPUFREQ_CORE_MASK)
		clk = amb_cpufreq->clk[1];
	else
		clk = NULL;

	return clk;
}

static int ambarella_cpufreq_set_extra_target(struct clk *clk,
		struct cpufreq_frequency_table *table,
		unsigned int index)
{
	int rval;
	unsigned int prev_freq;

	prev_freq = clk_get_rate(clk) / 1000;

	rval = clk_set_rate(clk, table[index].frequency * 1000);
	if (rval < 0) {
		pr_err("set '%s' frequency to %u HZ error.\n",
				__clk_get_name(clk), table[index].frequency);
		return rval;
	}

	pr_info("cpufreq: %s, %u KHZ -> %lu KHZ\n",
			__clk_get_name(clk),
			prev_freq,
			clk_get_rate(clk) / 1000);
	return rval;
}

static int ambarella_cpufreq_set_target(struct cpufreq_policy *policy,
		unsigned int index)
{
	unsigned int old_freq, new_freq;
	struct cpufreq_frequency_table *table;
	struct cpufreq_freqs freqs;
	int rval;

	table = ambarella_get_cpufreq_table();
	if (!table)
		return -EINVAL;

	old_freq = clk_get_rate(policy->clk) / 1000;
	new_freq = table[index].frequency;

	freqs.old = old_freq;
	freqs.new = new_freq;

	rval = clk_set_rate(policy->clk, new_freq * 1000);
	if (rval < 0) {
		pr_err("set %s frequency to %u KHZ error.\n",
				__clk_get_name(policy->clk), new_freq);
		goto err_set_rate;
	}

	pr_info("cpufreq: %s, %u KHZ -> %lu KHZ\n",
			__clk_get_name(policy->clk),
			old_freq,
			clk_get_rate(policy->clk) / 1000);

	if ((amb_cpufreq->cpufreq_mask & CPUFREQ_CORTEX_MASK)
			&& (amb_cpufreq->cpufreq_mask & CPUFREQ_CORE_MASK))
		rval = ambarella_cpufreq_set_extra_target(amb_cpufreq->clk[1],
				amb_cpufreq->table[1],
				index);
err_set_rate:
	return rval;
}

static int ambarella_cpufreq_driver_init(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *table;

	/* only master CPU handler CPU frequency. */
	if (policy->cpu)
		return -EINVAL;

	table = ambarella_get_cpufreq_table();
	if (!table)
		return -EINVAL;

	policy->clk = ambarella_get_cpufreq_clk();
	if (!policy->clk)
		return -ENODEV;

	cpufreq_generic_init(policy, table, CPUFREQ_ETERNAL);

	return 0;
}

static struct freq_attr *ambarella_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver ambarella_cpufreq_driver = {
	.name		= "amb-cpufreq",
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= ambarella_cpufreq_set_target,
	.get		= cpufreq_generic_get,
	.init		= ambarella_cpufreq_driver_init,
	.attr		= ambarella_cpufreq_attr,
};

static const struct of_device_id ambarella_cpufreq_match[] = {
	{
		.compatible = "ambarella,cpufreq",
	},
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_cpufreq_match);

static int ambarella_cpufreq_table_parse(struct device *dev)
{

	struct property *prop;
	const __be32 *reg;
	u32 value, i = 0;

	of_property_for_each_u32(dev->of_node, "clocks-frequency-cortex-core",
			prop, reg, value) {
		amb_cpufreq->table[i % 2][i / 2].driver_data = i / 2;
		amb_cpufreq->table[i % 2][i / 2].frequency = value;
		i++;
	}

	amb_cpufreq->table[0][i / 2].driver_data = i / 2;
	amb_cpufreq->table[1][i / 2].driver_data = i / 2;
	amb_cpufreq->table[0][i / 2].frequency = CPUFREQ_TABLE_END;
	amb_cpufreq->table[1][i / 2].frequency = CPUFREQ_TABLE_END;

	return 0;
}

static int ambarella_cpufreq_probe(struct platform_device *pdev)
{
	struct device_node *np;
	int rval, effective_clk;

	np = pdev->dev.of_node;
	if (!np)
		return -ENODEV;

	amb_cpufreq = devm_kzalloc(&pdev->dev, sizeof(*amb_cpufreq), GFP_KERNEL);
	if (!amb_cpufreq) {
		rval = -ENOMEM;
		goto err_put_node;
	}

	/* For backwards compatibility */
	rval = of_property_read_u32(np, "cpufreq-effective-clock", &effective_clk);
	if (rval || effective_clk < 0 || effective_clk > 2) {
		amb_cpufreq->cpufreq_mask = 0;
	} else {
		if (effective_clk == 0)
			amb_cpufreq->cpufreq_mask = CPUFREQ_CORTEX_MASK;
		else if (effective_clk == 1)
			amb_cpufreq->cpufreq_mask = CPUFREQ_CORE_MASK;
		else if (effective_clk == 2)
			amb_cpufreq->cpufreq_mask = CPUFREQ_CORE_MASK | CPUFREQ_CORTEX_MASK;
	}

	if (!amb_cpufreq->cpufreq_mask) {
		rval = of_property_read_u32(np, "cpufreq-mask", &amb_cpufreq->cpufreq_mask);
		if (rval || !amb_cpufreq->cpufreq_mask) {
			dev_err(&pdev->dev, "Not supported cpufreq-mask value.\n");
			goto err_put_node;
		}
	}

	rval = ambarella_cpufreq_table_parse(&pdev->dev);
	if (rval < 0)
		goto err_put_node;

	amb_cpufreq->clk[0] = of_clk_get(np, 0);
	if (IS_ERR(amb_cpufreq->clk[0])) {
		dev_err(&pdev->dev, "of_clk_get 0 error.\n");
		rval = -ENXIO;
		goto err_put_node;
	}

	amb_cpufreq->clk[1] = of_clk_get(np, 1);
	if (IS_ERR(amb_cpufreq->clk[1])) {
		dev_err(&pdev->dev, "of_clk_get 1 error.\n");
		rval = -ENXIO;
		goto err_put_node;
	}

#if 0
	if (!(amb_cpufreq->cpufreq_mask & CPUFREQ_CORE_MASK))
		ambarella_cpufreq_driver.flags |= CPUFREQ_ASYNC_NOTIFICATION;
#endif

	if (amb_cpufreq->cpufreq_mask & CPUFREQ_CORTEX_MASK) {
		/* notify to adjust the frequency of arm timer */
		if (!of_find_property(np, "amb,timer-freq-adjust-off", NULL)) {
			amb_cpufreq->clksrc_nb.notifier_call = ambarella_clksrc_nb_callback;
			amb_cpufreq->clksrc_nb.next = NULL;
			clk_notifier_register(amb_cpufreq->clk[0], &amb_cpufreq->clksrc_nb);
		}
	}

	rval = cpufreq_register_driver(&ambarella_cpufreq_driver);

err_put_node:
	of_node_put(np);

	return rval;
}

static int ambarella_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&ambarella_cpufreq_driver);

	return 0;
}

static struct platform_driver ambarella_cpufreq_platdrv = {
	.driver = {
		.name	= "ambarella-cpufreq",
		.of_match_table = ambarella_cpufreq_match,
	},
	.probe		= ambarella_cpufreq_probe,
	.remove		= ambarella_cpufreq_remove,
};
module_platform_driver(ambarella_cpufreq_platdrv);

MODULE_AUTHOR("Jorney <qtu@ambarella.com>");
MODULE_DESCRIPTION("Ambarella cpufreq driver");
MODULE_LICENSE("GPL");
