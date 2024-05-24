// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pinctrl driver for Ambarella SoCs
 *
 * History:
 *	2013/12/18 - [Cao Rongrong] created file
 *
 * Copyright (C) 2012-2048, Ambarella, Inc.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/gpio.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include "core.h"

/* ==========================================================================*/

#define GPIO_DATA_OFFSET		0x00
#define GPIO_DIR_OFFSET			0x04
#define GPIO_IS_OFFSET			0x08
#define GPIO_IBE_OFFSET			0x0c
#define GPIO_IEV_OFFSET			0x10
#define GPIO_IE_OFFSET			0x14
#define GPIO_AFSEL_OFFSET		0x18
#define GPIO_RIS_OFFSET			0x1c
#define GPIO_MIS_OFFSET			0x20
#define GPIO_IC_OFFSET			0x24
#define GPIO_MASK_OFFSET		0x28
#define GPIO_ENABLE_OFFSET		0x2c

#define IOMUX_OFFSET(bank, n)		(((bank) * 0xc) + ((n) * 4))
#define IOMUX_CTRL_SET_OFFSET		0xf0

/* pull and drive strength */
#define DS0_OFFSET(bank)		((bank) >= 4 ? \
					(0x438 + (((bank) - 4) * 8)) : \
					(0x314 + ((bank) * 8)))
#define DS1_OFFSET(bank)		(DS0_OFFSET(bank) + 4)
#define DS_OFFSET(offset, bank)		((offset) + ((bank) * 8))

#define PULL_EN_OFFSET(bank)		((bank) == 6 ? 0x100 : (0x60 + ((bank) * 4)))
#define PULL_DIR_OFFSET(bank)		((bank) == 6 ? 0x108 : (0x7C + ((bank) * 4)))
#define PULL_OFFSET(offset, bank)	((offset) + ((bank) * 4))

/* ==========================================================================*/

#define MAX_BANK_NUM			8
#define MAX_PIN_NUM			(MAX_BANK_NUM * 32)

#define PINID_TO_BANK(p)		((p) >> 5)
#define PINID_TO_OFFSET(p)		((p) & 0x1f)


#define MUXIDS_TO_PINID(m)		((m) & 0xfff)
#define MUXIDS_TO_ALT(m)		(((m) >> 12) & 0xf)

#define CONFIDS_TO_PINID(c)		((c) & 0xfff)
#define CONFIDS_TO_CONF(c)		(((c) >> 16) & 0xffff)


/*
 * bit1~0: 00: pull down, 01: pull up, 1x: clear pull up/down
 * bit2:   reserved
 * bit3:   1: config pull up/down, 0: leave pull up/down as default value
 * bit5~4: drive strength value
 * bit6:   reserved
 * bit7:   1: config drive strength, 0: leave drive strength as default value
 */
#define CONF_TO_PULL_VAL(c)		(((c) >> 0) & 0x1)
#define CONF_TO_PULL_CLR(c)		(((c) >> 1) & 0x1)
#define CFG_PULL_PRESENT(c)		(((c) >> 3) & 0x1)
#define CONF_TO_DS_VAL(c)		(((c) >> 4) & 0x3)
#define CFG_DS_PRESENT(c)		(((c) >> 7) & 0x1)

struct ambpin_group {
	const char		*name;
	unsigned int		*pins;
	unsigned int		num_pins;
	u8			*alt;
	unsigned int		*conf_pins;
	unsigned int		num_conf_pins;
	unsigned long		*conf;
};

struct ambpin_function {
	const char		*name;
	const char		**groups;
	unsigned int		num_groups;
};

struct amb_pinctrl_pm_state {
	u32 iomux[3];
	u32 pull[2];
	u32 ds[2];
	u32 data;
	u32 dir;
	u32 is;
	u32 ibe;
	u32 iev;
	u32 ie;
	u32 afsel;
	u32 mask;
};

struct amb_pinctrl_soc_data {
	struct device			*dev;
	void __iomem			*gpio_base[MAX_BANK_NUM];
	void __iomem			*iomux_base;
	struct regmap			*ds_regmap;
	struct regmap			*pull_regmap;
	unsigned int			ds0[MAX_BANK_NUM];
	unsigned int			ds1[MAX_BANK_NUM];
	unsigned int			pull_en[MAX_BANK_NUM];
	unsigned int			pull_dir[MAX_BANK_NUM];
	unsigned int			bank_num;
	int				irq[MAX_BANK_NUM];
	unsigned int			irq_wake_mask[MAX_BANK_NUM];
	unsigned long			used[BITS_TO_LONGS(MAX_PIN_NUM)];
	raw_spinlock_t lock;

	struct pinctrl_dev		*pctl;
	struct gpio_chip		*gc;
	struct irq_domain		*domain;

	struct ambpin_function		*functions;
	unsigned int			nr_functions;
	struct ambpin_group		*groups;
	unsigned int			nr_groups;

	struct amb_pinctrl_pm_state	pm[MAX_BANK_NUM];

	uint32_t			hsm_domain_id; /* domain id, used in HSM boot */
};

static struct amb_pinctrl_soc_data *amb_pinctrl_soc;

static __iomem void *amb_get_gpio_base(const struct amb_pinctrl_soc_data *soc,
									   struct irq_data *data)
{
	return soc->gpio_base[PINID_TO_BANK(irqd_to_hwirq(data))];
}

/* check if the selector is a valid pin group selector */
static int amb_get_group_count(struct pinctrl_dev *pctldev)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	return soc->nr_groups;
}

/* return the name of the group selected by the group selector */
static const char *amb_get_group_name(struct pinctrl_dev *pctldev,
						unsigned int selector)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	return soc->groups[selector].name;
}

/* return the pin numbers associated with the specified group */
static int amb_get_group_pins(struct pinctrl_dev *pctldev,
		unsigned int selector, const unsigned int **pins, unsigned int *num_pins)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	*pins = soc->groups[selector].pins;
	*num_pins = soc->groups[selector].num_pins;

	return 0;
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static void amb_pin_dbg_show(struct pinctrl_dev *pctldev,
			struct seq_file *s, unsigned int pin)
{
	struct pin_desc *desc;

	seq_printf(s, " %s", dev_name(pctldev->dev));

	desc = pin_desc_get(pctldev, pin);
	if (desc) {
		seq_printf(s, " owner: %s%s%s%s",
			desc->mux_owner ? desc->mux_owner : "",
			desc->mux_owner && desc->gpio_owner ? " " : "",
			desc->gpio_owner ? desc->gpio_owner : "",
			!desc->mux_owner && !desc->gpio_owner ? "NULL" : "");
	} else {
		seq_puts(s, " not registered");
	}
}
#endif

#if IS_ENABLED(CONFIG_OF)
static int amb_dt_node_to_map(struct pinctrl_dev *pctldev,
			struct device_node *np,
			struct pinctrl_map **map, unsigned int *num_maps)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	struct ambpin_group *grp = NULL;
	struct pinctrl_map *new_map;
	char *grp_name;
	u32 i, reg, new_num;

	if (of_property_read_u32(np, "reg", &reg))
		return -EINVAL;

	/* Compose group name */
	grp_name = devm_kasprintf(soc->dev, GFP_KERNEL, "%s.%d", np->name, reg);
	if (!grp_name)
		return -ENOMEM;

	/* find the group of this node by name */
	for (i = 0; i < soc->nr_groups; i++) {
		if (!strcmp(soc->groups[i].name, grp_name)) {
			grp = &soc->groups[i];
			break;
		}
	}
	if (!grp) {
		dev_err(soc->dev, "unable to find group for node %s\n", np->name);
		return -EINVAL;
	}

	new_num = !!grp->num_pins + grp->num_conf_pins;
	new_map = devm_kcalloc(soc->dev, new_num,
				sizeof(struct pinctrl_map), GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	*map = new_map;
	*num_maps = new_num;

	/* create mux map */
	if (grp->num_pins) {
		new_map[0].type = PIN_MAP_TYPE_MUX_GROUP;
		new_map[0].data.mux.group = grp_name;
		new_map[0].data.mux.function = np->name;
		new_map++;
	}

	/* create config map */
	for (i = 0; i < grp->num_conf_pins; i++) {
		new_map[i].type = PIN_MAP_TYPE_CONFIGS_PIN;
		new_map[i].data.configs.group_or_pin =
				pin_get_name(pctldev, grp->conf_pins[i]);
		new_map[i].data.configs.configs = &grp->conf[i];
		new_map[i].data.configs.num_configs = 1;
	}

	return 0;
}

static void amb_dt_free_map(struct pinctrl_dev *pctldev,
			struct pinctrl_map *map, unsigned int num_maps)
{
}
#endif

/* list of pinctrl callbacks for the pinctrl core */
static const struct pinctrl_ops amb_pctrl_ops = {
	.get_groups_count	= amb_get_group_count,
	.get_group_name		= amb_get_group_name,
	.get_group_pins		= amb_get_group_pins,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.pin_dbg_show		= amb_pin_dbg_show,
#endif
#if IS_ENABLED(CONFIG_OF)
	.dt_node_to_map		= amb_dt_node_to_map,
	.dt_free_map		= amb_dt_free_map,
#endif
};

/* check if the selector is a valid pin function selector */
static int amb_pinmux_request(struct pinctrl_dev *pctldev, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	if (test_and_set_bit(pin, soc->used))
		return -EBUSY;

	return 0;
}

/* check if the selector is a valid pin function selector */
static int amb_pinmux_free(struct pinctrl_dev *pctldev, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	clear_bit(pin, soc->used);

	return 0;
}

/* check if the selector is a valid pin function selector */
static int amb_pinmux_get_fcount(struct pinctrl_dev *pctldev)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	return soc->nr_functions;
}

/* return the name of the pin function specified */
static const char *amb_pinmux_get_fname(struct pinctrl_dev *pctldev,
			unsigned int selector)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	return soc->functions[selector].name;
}

/* return the groups associated for the specified function selector */
static int amb_pinmux_get_groups(struct pinctrl_dev *pctldev,
			unsigned int selector, const char * const **groups,
			unsigned int * const num_groups)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	*groups = soc->functions[selector].groups;
	*num_groups = soc->functions[selector].num_groups;

	return 0;
}

static void amb_pinmux_set_altfunc(struct amb_pinctrl_soc_data *soc,
			u32 bank, u32 offset, u32 altfunc)
{
	u32 i, data;

	/* On CV3 platform, only ARM cluster0 (safety domain) can access pinctrl registers */
	if (soc->hsm_domain_id != 0)
		return;

	for (i = 0; i < 3; i++) {
		data = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(bank, i));
		data &= (~(0x1 << offset));
		data |= (((altfunc >> i) & 0x1) << offset);
		writel_relaxed(data, soc->iomux_base + IOMUX_OFFSET(bank, i));
	}

	writel_relaxed(0x1, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
	writel_relaxed(0x0, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
}

/* enable a specified pinmux by writing to registers */
static int amb_pinmux_set_mux(struct pinctrl_dev *pctldev,
			unsigned int selector, unsigned int group)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	const struct ambpin_group *grp;
	u32 i, bank, offset;
	unsigned long flags;

	grp = &soc->groups[group];

	raw_spin_lock_irqsave(&soc->lock, flags);
	for (i = 0; i < grp->num_pins; i++) {
		bank = PINID_TO_BANK(grp->pins[i]);
		offset = PINID_TO_OFFSET(grp->pins[i]);
		amb_pinmux_set_altfunc(soc, bank, offset,  grp->alt[i]);
	}
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

static int amb_pinmux_gpio_request_enable(struct pinctrl_dev *pctldev,
			struct pinctrl_gpio_range *range, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	u32 bank, offset;
	unsigned long flags;

	if (!range || !range->gc) {
		dev_err(soc->dev, "invalid range: %p\n", range);
		return -EINVAL;
	}

	if (test_and_set_bit(pin, soc->used))
		return -EBUSY;

	bank = PINID_TO_BANK(pin);
	offset = PINID_TO_OFFSET(pin);

	raw_spin_lock_irqsave(&soc->lock, flags);
	amb_pinmux_set_altfunc(soc, bank, offset, 0);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

static void amb_pinmux_gpio_disable_free(struct pinctrl_dev *pctldev,
			struct pinctrl_gpio_range *range,
			unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);

	dev_dbg(soc->dev, "disable pin %u as GPIO\n", pin);
	/* Set the pin to some default state, GPIO is usually default */

	clear_bit(pin, soc->used);
}

/* list of pinmux callbacks for the pinmux vertical in pinctrl core */
static const struct pinmux_ops amb_pinmux_ops = {
	.request		= amb_pinmux_request,
	.free			= amb_pinmux_free,
	.get_functions_count	= amb_pinmux_get_fcount,
	.get_function_name	= amb_pinmux_get_fname,
	.get_function_groups	= amb_pinmux_get_groups,
	.set_mux                = amb_pinmux_set_mux,
	.gpio_request_enable	= amb_pinmux_gpio_request_enable,
	.gpio_disable_free	= amb_pinmux_gpio_disable_free,
};

/* set the pin config settings for a specified pin */
static int amb_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
			unsigned long *configs, unsigned int num_configs)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	u32 i, bank, offset;
	unsigned long config, flags;

	bank = PINID_TO_BANK(pin);
	offset = PINID_TO_OFFSET(pin);

	raw_spin_lock_irqsave(&soc->lock, flags);
	for (i = 0; i < num_configs; i++) {
		config = configs[i];

		if (CFG_PULL_PRESENT(config)) {
			regmap_update_bits(soc->pull_regmap,
				soc->pull_dir[bank], 0x1 << offset,
				CONF_TO_PULL_VAL(config) << offset);

			regmap_update_bits(soc->pull_regmap,
				soc->pull_en[bank], 0x1 << offset,
				(CONF_TO_PULL_CLR(config) ? 0x0 : 0x1) << offset);
		}

		if (CFG_DS_PRESENT(config)) {
			/* set bit1 of DS value to DS0 reg, and set bit0 of DS value to DS1 reg */
			regmap_update_bits(soc->ds_regmap,
				soc->ds0[bank], 0x1 << offset,
				((CONF_TO_DS_VAL(config) >> 1) & 0x1) << offset);
			regmap_update_bits(soc->ds_regmap,
				soc->ds1[bank], 0x1 << offset,
				(CONF_TO_DS_VAL(config) & 0x1) << offset);
		}
	}
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

/* get the pin config settings for a specified pin */
static int amb_pinconf_get(struct pinctrl_dev *pctldev,
			unsigned int pin, unsigned long *config)
{
	dev_WARN_ONCE(pctldev->dev, true, "NOT Implemented.\n");
	return -EOPNOTSUPP;
}


#if IS_ENABLED(CONFIG_DEBUG_FS)
static void amb_pinconf_dbg_show(struct pinctrl_dev *pctldev,
			struct seq_file *s, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = pinctrl_dev_get_drvdata(pctldev);
	u32 pull_en, pull_dir, ds0, ds1, drv_strength;
	u32 bank, offset;

	bank = PINID_TO_BANK(pin);
	offset = PINID_TO_OFFSET(pin);

	regmap_read(soc->pull_regmap, soc->pull_en[bank], &pull_en);
	pull_en = (pull_en >> offset) & 0x1;
	regmap_read(soc->pull_regmap, soc->pull_dir[bank], &pull_dir);
	pull_dir = (pull_dir >> offset) & 0x1;
	seq_printf(s, " pull: %s, ", pull_en ? pull_dir ? "up" : "down" : "disable");

	regmap_read(soc->ds_regmap, soc->ds0[bank], &ds0);
	ds0 = (ds0 >> offset) & 0x1;
	regmap_read(soc->ds_regmap, soc->ds1[bank], &ds1);
	ds1 = (ds1 >> offset) & 0x1;
	drv_strength = (ds0 << 1) | ds1;
	seq_printf(s, "drive-strength: %s",
		drv_strength == 3 ? "12mA" : drv_strength == 2 ? "8mA" :
		drv_strength == 1 ? "4mA" : "2mA");
}
#endif

/* list of pinconfig callbacks for pinconfig vertical in the pinctrl code */
static const struct pinconf_ops amb_pinconf_ops = {
	.pin_config_get		= amb_pinconf_get,
	.pin_config_set		= amb_pinconf_set,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.pin_config_dbg_show	= amb_pinconf_dbg_show,
#endif
};

static struct pinctrl_desc amb_pinctrl_desc = {
	.pctlops = &amb_pctrl_ops,
	.pmxops = &amb_pinmux_ops,
	.confops = &amb_pinconf_ops,
	.owner = THIS_MODULE,
};

static int amb_pinctrl_parse_group(struct amb_pinctrl_soc_data *soc,
			struct device_node *np, int idx, const char **out_name)
{
	struct ambpin_group *grp = &soc->groups[idx];
	struct property *prop;
	const char *prop_name;
	int length;
	u32 reg, i;

	if (of_property_read_u32(np, "reg", &reg))
		return -EINVAL;

	grp->name = devm_kasprintf(soc->dev, GFP_KERNEL, "%s.%d", np->name, reg);
	if (!grp->name)
		return -ENOMEM;

	prop_name = "amb,pinmux-ids";
	prop = of_find_property(np, prop_name, &length);
	if (prop) {
		grp->num_pins = length / sizeof(u32);

		grp->pins = devm_kcalloc(soc->dev,
					grp->num_pins, sizeof(u32), GFP_KERNEL);
		if (!grp->pins)
			return -ENOMEM;

		grp->alt = devm_kcalloc(soc->dev,
					grp->num_pins, sizeof(u8), GFP_KERNEL);
		if (!grp->alt)
			return -ENOMEM;

		of_property_read_u32_array(np, prop_name, grp->pins, grp->num_pins);

		for (i = 0; i < grp->num_pins; i++) {
			grp->alt[i] = MUXIDS_TO_ALT(grp->pins[i]);
			grp->pins[i] = MUXIDS_TO_PINID(grp->pins[i]);
		}
	}

	/* parse pinconf */
	prop_name = "amb,pinconf-ids";
	prop = of_find_property(np, prop_name, &length);
	if (prop) {
		grp->num_conf_pins = length / sizeof(u32);

		grp->conf_pins = devm_kcalloc(soc->dev,
					grp->num_conf_pins, sizeof(u32), GFP_KERNEL);
		if (!grp->conf_pins)
			return -ENOMEM;

		grp->conf = devm_kcalloc(soc->dev,
				grp->num_conf_pins, sizeof(unsigned long), GFP_KERNEL);
		if (!grp->conf)
			return -ENOMEM;

		of_property_read_u32_array(np, prop_name, grp->conf_pins, grp->num_conf_pins);

		for (i = 0; i < grp->num_conf_pins; i++) {
			grp->conf[i] = CONFIDS_TO_CONF(grp->conf_pins[i]);
			grp->conf_pins[i] = CONFIDS_TO_PINID(grp->conf_pins[i]);
		}
	}

	if (out_name)
		*out_name = grp->name;

	return 0;
}

static int amb_pinctrl_parse_dt(struct amb_pinctrl_soc_data *soc)
{
	struct device_node *np = soc->dev->of_node;
	struct device_node *child;
	struct ambpin_function *f;
	const char *fn;
	int i = 0, idxf = 0, idxg = 0;
	int ret;

	child = of_get_next_child(np, NULL);
	if (!child) {
		dev_err(soc->dev, "no group is defined\n");
		return -ENOENT;
	}

	/* Count total functions and groups */
	fn = "";
	for_each_child_of_node(np, child) {
		if (of_find_property(child, "gpio-controller", NULL))
			continue;
		soc->nr_groups++;
		if (strcmp(fn, child->name)) {
			fn = child->name;
			soc->nr_functions++;
		}
	}

	soc->functions = devm_kcalloc(soc->dev, soc->nr_functions,
				sizeof(struct ambpin_function), GFP_KERNEL);
	if (!soc->functions)
		return -ENOMEM;

	soc->groups = devm_kcalloc(soc->dev, soc->nr_groups,
				sizeof(struct ambpin_group), GFP_KERNEL);
	if (!soc->groups)
		return -ENOMEM;

	/* Count groups for each function */
	fn = "";
	f = &soc->functions[idxf];
	for_each_child_of_node(np, child) {
		if (of_find_property(child, "gpio-controller", NULL))
			continue;
		if (strcmp(fn, child->name)) {
			f = &soc->functions[idxf++];
			f->name = fn = child->name;
		}
		f->num_groups++;
	};

	/* Get groups for each function */
	fn = "";
	idxf = 0;
	for_each_child_of_node(np, child) {
		if (of_find_property(child, "gpio-controller", NULL))
			continue;

		if (strcmp(fn, child->name)) {
			f = &soc->functions[idxf++];
			f->groups = devm_kcalloc(soc->dev,
					f->num_groups, sizeof(*f->groups),
					GFP_KERNEL);
			if (!f->groups) {
				of_node_put(child);
				return -ENOMEM;
			}
			fn = child->name;
			i = 0;
		}

		ret = amb_pinctrl_parse_group(soc, child,
					idxg++, &f->groups[i++]);
		if (ret) {
			of_node_put(child);
			return ret;
		}
	}

	return 0;
}

/* register the pinctrl interface with the pinctrl subsystem */
static int amb_pinctrl_register(struct amb_pinctrl_soc_data *soc)
{
	struct pinctrl_pin_desc *pindesc;
	int pin, pin_num, rval;

	pin_num = soc->bank_num * 32;

	/* dynamically populate the pin number and pin name for pindesc */
	pindesc = devm_kcalloc(soc->dev, pin_num, sizeof(*pindesc), GFP_KERNEL);
	if (!pindesc)
		return -ENOMEM;

	for (pin = 0; pin < pin_num; pin++) {
		pindesc[pin].number = pin;
		pindesc[pin].name = devm_kasprintf(soc->dev, GFP_KERNEL, "io%d", pin);
		if (!pindesc[pin].name)
			return -ENOMEM;
	}

	amb_pinctrl_desc.name = dev_name(soc->dev);
	amb_pinctrl_desc.pins = pindesc;
	amb_pinctrl_desc.npins = pin_num;

	rval = amb_pinctrl_parse_dt(soc);
	if (rval)
		return rval;

	soc->pctl = devm_pinctrl_register(soc->dev, &amb_pinctrl_desc, soc);
	if (IS_ERR(soc->pctl)) {
		dev_err(soc->dev, "could not register pinctrl driver\n");
		return PTR_ERR(soc->pctl);
	}

	return 0;
}

/* gpiolib gpio_free callback function */
static void amb_gpio_set(struct gpio_chip *gc, unsigned int pin, int value)
{
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	u32 bank, offset, data;
	unsigned long flags;

	bank = PINID_TO_BANK(pin);
	offset = PINID_TO_OFFSET(pin);

	raw_spin_lock_irqsave(&soc->lock, flags);
	writel_relaxed(0x1 << offset, soc->gpio_base[bank] + GPIO_MASK_OFFSET);
	data = (value == 0) ? 0 : 0x1 << offset;
	writel_relaxed(data, soc->gpio_base[bank] + GPIO_DATA_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

/* gpiolib gpio_get callback function */
static int amb_gpio_get(struct gpio_chip *gc, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	u32 bank, offset, data;
	unsigned long flags;

	bank = PINID_TO_BANK(pin);
	offset = PINID_TO_OFFSET(pin);

	raw_spin_lock_irqsave(&soc->lock, flags);
	writel_relaxed(0x1 << offset, soc->gpio_base[bank] + GPIO_MASK_OFFSET);
	data = readl_relaxed(soc->gpio_base[bank] + GPIO_DATA_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return (data >> offset) & 0x1;
}

static int amb_gpio_get_direction(struct gpio_chip *gc, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	u32 bank, offset, data;
	unsigned long flags;

	bank = PINID_TO_BANK(pin);
	offset = PINID_TO_OFFSET(pin);

	raw_spin_lock_irqsave(&soc->lock, flags);
	data = readl_relaxed(soc->gpio_base[bank] + GPIO_DIR_OFFSET);
	data = (data >> offset) & 0x1;
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return data ? GPIOF_DIR_OUT : GPIOF_DIR_IN;
}

static int amb_gpio_set_direction(struct gpio_chip *gc, unsigned int pin, bool input)
{
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	u32 bank, offset, data;
	unsigned long flags;

	bank = PINID_TO_BANK(pin);
	offset = PINID_TO_OFFSET(pin);

	raw_spin_lock_irqsave(&soc->lock, flags);
	data = readl_relaxed(soc->gpio_base[bank] + GPIO_DIR_OFFSET);
	if (input)
		writel_relaxed(data & ~(0x1 << offset), soc->gpio_base[bank] + GPIO_DIR_OFFSET);
	else
		writel_relaxed(data | (0x1 << offset), soc->gpio_base[bank] + GPIO_DIR_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

/* gpiolib gpio_direction_input callback function */
static int amb_gpio_direction_input(struct gpio_chip *gc, unsigned int pin)
{
	return amb_gpio_set_direction(gc, pin, true);
}

/* gpiolib gpio_direction_output callback function */
static int amb_gpio_direction_output(struct gpio_chip *gc, unsigned int pin, int value)
{
	amb_gpio_set_direction(gc, pin, false);
	amb_gpio_set(gc, pin, value);
	return 0;
}

/* gpiolib gpio_to_irq callback function */
static int amb_gpio_to_irq(struct gpio_chip *gc, unsigned int pin)
{
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);

	return irq_create_mapping(soc->domain, pin);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static void amb_gpio_dbg_show(struct seq_file *s, struct gpio_chip *gc)
{
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	u32 afsel, data, dir, mask, iomux0, iomux1, iomux2, alt;
	u32 i, bank, offset;

	for (i = 0; i < gc->ngpio; i++) {
		offset = PINID_TO_OFFSET(i);
		if (!offset) {
			bank = PINID_TO_BANK(i);

			afsel = readl_relaxed(soc->gpio_base[bank] + GPIO_AFSEL_OFFSET);
			dir = readl_relaxed(soc->gpio_base[bank] + GPIO_DIR_OFFSET);
			mask = readl_relaxed(soc->gpio_base[bank] + GPIO_MASK_OFFSET);
			writel_relaxed(0xffffffff, soc->gpio_base[bank] + GPIO_MASK_OFFSET);
			data = readl_relaxed(soc->gpio_base[bank] + GPIO_DATA_OFFSET);
			writel_relaxed(mask, soc->gpio_base[bank] + GPIO_MASK_OFFSET);

			seq_printf(s, "\nGPIO[%d]:\t[%d - %d]\n", bank, i, i + 32 - 1);
			seq_printf(s, "GPIO_AFSEL:\t0x%08X\n", afsel);
			seq_printf(s, "GPIO_DIR:\t0x%08X\n", dir);
			seq_printf(s, "GPIO_MASK:\t0x%08X\n", mask);
			seq_printf(s, "GPIO_DATA:\t0x%08X\n", data);

			iomux0 = readl_relaxed(soc->iomux_base + bank * 12);
			iomux1 = readl_relaxed(soc->iomux_base + bank * 12 + 4);
			iomux2 = readl_relaxed(soc->iomux_base + bank * 12 + 8);
			seq_printf(s, "IOMUX_REG%d_0:\t0x%08X\n", bank, iomux0);
			seq_printf(s, "IOMUX_REG%d_1:\t0x%08X\n", bank, iomux1);
			seq_printf(s, "IOMUX_REG%d_2:\t0x%08X\n", bank, iomux2);
		}

		seq_printf(s, " gpio-%-3d", gc->base + i);

		alt = ((iomux2 >> offset) & 1) << 2;
		alt |= ((iomux1 >> offset) & 1) << 1;
		alt |= ((iomux0 >> offset) & 1) << 0;
		if (alt) {
			seq_printf(s, " [HW  ] (alt%d)\n", alt);
		} else {
			const char *label = gpiochip_is_requested(gc, i);

			label = label ? : "";
			seq_printf(s, " [GPIO] (%-20.20s) %s %s\n", label,
				(dir & (1 << offset)) ? "out" : "in ",
				(data & (1 << offset)) ? "hi" : "lo");
		}
	}
}
#endif

static struct gpio_chip amb_gc = {
	.label			= "ambarella-gpio",
	.base			= 0,
	.ngpio			= 0, /* assigned in probe */
	.request		= gpiochip_generic_request,
	.free			= gpiochip_generic_free,
	.direction_input	= amb_gpio_direction_input,
	.direction_output	= amb_gpio_direction_output,
	.get_direction		= amb_gpio_get_direction,
	.get			= amb_gpio_get,
	.set			= amb_gpio_set,
	.to_irq			= amb_gpio_to_irq,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.dbg_show		= amb_gpio_dbg_show,
#endif
	.owner			= THIS_MODULE,
};

static void amb_gpio_irq_enable(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	void __iomem *gpio_base = amb_get_gpio_base(soc, data);
	void __iomem *iomux_base = soc->iomux_base;
	u32 i, val, bank, offset;
	unsigned long flags;

	bank = PINID_TO_BANK(irqd_to_hwirq(data));
	offset = PINID_TO_OFFSET(irqd_to_hwirq(data));

	raw_spin_lock_irqsave(&soc->lock, flags);

	/* make sure the pin is in gpio mode */
	if (!gpiochip_is_requested(gc, irqd_to_hwirq(data))) {
		val = readl_relaxed(gpio_base + GPIO_DIR_OFFSET);
		val &= ~(0x1 << offset);
		writel_relaxed(val, gpio_base + GPIO_DIR_OFFSET);

		for (i = 0; i < 3; i++) {
			val = readl_relaxed(iomux_base + IOMUX_OFFSET(bank, i));
			val &= ~(0x1 << offset);
			writel_relaxed(val, iomux_base + IOMUX_OFFSET(bank, i));
		}
		writel_relaxed(0x1, iomux_base + IOMUX_CTRL_SET_OFFSET);
		writel_relaxed(0x0, iomux_base + IOMUX_CTRL_SET_OFFSET);
	}

	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);

	val = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	val |= 0x1 << offset;
	writel_relaxed(val, gpio_base + GPIO_IE_OFFSET);

	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_disable(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	void __iomem *gpio_base = amb_get_gpio_base(soc, data);
	u32 offset, ie;
	unsigned long flags;

	offset = PINID_TO_OFFSET(irqd_to_hwirq(data));

	raw_spin_lock_irqsave(&soc->lock, flags);
	ie = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(ie & ~(0x1 << offset), gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_ack(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	void __iomem *gpio_base = amb_get_gpio_base(soc, data);
	u32 offset = PINID_TO_OFFSET(irqd_to_hwirq(data));
	unsigned long flags;

	raw_spin_lock_irqsave(&soc->lock, flags);
	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_mask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	void __iomem *gpio_base = amb_get_gpio_base(soc, data);
	u32 offset, ie;
	unsigned long flags;

	offset = PINID_TO_OFFSET(irqd_to_hwirq(data));

	raw_spin_lock_irqsave(&soc->lock, flags);
	ie = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(ie & ~(0x1 << offset), gpio_base + GPIO_IE_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_mask_ack(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	void __iomem *gpio_base = amb_get_gpio_base(soc, data);
	u32 offset, ie;
	unsigned long flags;

	offset = PINID_TO_OFFSET(irqd_to_hwirq(data));

	raw_spin_lock_irqsave(&soc->lock, flags);
	ie = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(ie & ~(0x1 << offset), gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static void amb_gpio_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	void __iomem *gpio_base = amb_get_gpio_base(soc, data);
	u32 offset, ie;
	unsigned long flags;

	offset = PINID_TO_OFFSET(irqd_to_hwirq(data));

	raw_spin_lock_irqsave(&soc->lock, flags);
	ie = readl_relaxed(gpio_base + GPIO_IE_OFFSET);
	writel_relaxed(ie | (0x1 << offset), gpio_base + GPIO_IE_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);
}

static int amb_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
	void __iomem *gpio_base = amb_get_gpio_base(soc, data);
	struct irq_desc *desc = irq_to_desc(data->irq);
	u32 offset = PINID_TO_OFFSET(irqd_to_hwirq(data));
	u32 mask, bit, sense, bothedges, event;
	unsigned long flags;

	mask = ~(0x1 << offset);
	bit = (0x1 << offset);
	sense = readl_relaxed(gpio_base + GPIO_IS_OFFSET);
	bothedges = readl_relaxed(gpio_base + GPIO_IBE_OFFSET);
	event = readl_relaxed(gpio_base + GPIO_IEV_OFFSET);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		sense &= mask;
		bothedges &= mask;
		event |= bit;
		desc->handle_irq = handle_edge_irq;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		sense &= mask;
		bothedges &= mask;
		event &= mask;
		desc->handle_irq = handle_edge_irq;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		sense &= mask;
		bothedges |= bit;
		event &= mask;
		desc->handle_irq = handle_edge_irq;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		sense |= bit;
		bothedges &= mask;
		event |= bit;
		desc->handle_irq = handle_level_irq;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		sense |= bit;
		bothedges &= mask;
		event &= mask;
		desc->handle_irq = handle_level_irq;
		break;
	default:
		pr_err("%s: irq[%d] type[%d] fail!\n",
			__func__, data->irq, type);
		return -EINVAL;
	}

	raw_spin_lock_irqsave(&soc->lock, flags);
	writel_relaxed(sense, gpio_base + GPIO_IS_OFFSET);
	writel_relaxed(bothedges, gpio_base + GPIO_IBE_OFFSET);
	writel_relaxed(event, gpio_base + GPIO_IEV_OFFSET);
	/* clear obsolete irq */
	writel_relaxed(0x1 << offset, gpio_base + GPIO_IC_OFFSET);
	raw_spin_unlock_irqrestore(&soc->lock, flags);

	return 0;
}

static int amb_gpio_irq_set_wake(struct irq_data *data, unsigned int on)
{
	if (IS_ENABLED(CONFIG_PM)) {
		struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
		struct amb_pinctrl_soc_data *soc = gpiochip_get_data(gc);
		u32 bank = PINID_TO_BANK(irqd_to_hwirq(data));
		u32 offset = PINID_TO_OFFSET(irqd_to_hwirq(data));
		unsigned long flags;

		raw_spin_lock_irqsave(&soc->lock, flags);
		if (on)
			soc->irq_wake_mask[bank] |= (1 << offset);
		else
			soc->irq_wake_mask[bank] &= ~(1 << offset);
		raw_spin_unlock_irqrestore(&soc->lock, flags);
	}
	return 0;
}

static struct irq_chip amb_gpio_irqchip = {
	.name		= "GPIO",
	.irq_enable	= amb_gpio_irq_enable,
	.irq_disable	= amb_gpio_irq_disable,
	.irq_ack	= amb_gpio_irq_ack,
	.irq_mask	= amb_gpio_irq_mask,
	.irq_mask_ack	= amb_gpio_irq_mask_ack,
	.irq_unmask	= amb_gpio_irq_unmask,
	.irq_set_type	= amb_gpio_irq_set_type,
	.irq_set_wake	= amb_gpio_irq_set_wake,
	.flags		= IRQCHIP_SET_TYPE_MASKED | IRQCHIP_MASK_ON_SUSPEND,
};

static int amb_gpio_irqdomain_map(struct irq_domain *d,
			unsigned int irq, irq_hw_number_t hwirq)
{
	struct amb_pinctrl_soc_data *soc = d->host_data;

	irq_set_chip_data(irq, soc->gc);
	irq_set_chip_and_handler(irq, &amb_gpio_irqchip, handle_level_irq);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops amb_gpio_irq_domain_ops = {
	.map = amb_gpio_irqdomain_map,
	.xlate = irq_domain_xlate_twocell,
};

static void amb_gpio_handle_irq(struct irq_desc *desc)
{
	struct amb_pinctrl_soc_data *soc;
	struct irq_chip *irqchip;
	u32 i, gpio_mis, gpio_hwirq, gpio_irq;

	irqchip = irq_desc_get_chip(desc);
	chained_irq_enter(irqchip, desc);

	soc = irq_desc_get_handler_data(desc);

	/* find the GPIO bank generating this irq */
	for (i = 0; i < soc->bank_num; i++) {
		if (soc->irq[i] == irq_desc_get_irq(desc))
			break;
	}

	if (i == soc->bank_num)
		return;

	gpio_mis = readl_relaxed(soc->gpio_base[i] + GPIO_MIS_OFFSET);
	if (gpio_mis) {
		gpio_hwirq = i * 32 + ffs(gpio_mis) - 1;
		gpio_irq = irq_find_mapping(soc->domain, gpio_hwirq);
		generic_handle_irq(gpio_irq);
	}

	chained_irq_exit(irqchip, desc);
}

/* register the pinctrl interface with the pinctrl subsystem */
static int amb_gpio_register(struct amb_pinctrl_soc_data *soc)
{
	struct device_node *np;
	int i, rval = -ENODEV;

	for_each_child_of_node(soc->dev->of_node, np) {
		if (of_find_property(np, "gpio-controller", NULL)) {
			rval = 0;
			break;
		}
	}
	if (rval < 0) {
		dev_err(soc->dev, "no gpio-controller child node\n");
		return rval;
	}

	soc->gc = &amb_gc;
	soc->gc->parent = soc->dev;
	soc->gc->of_node = np;
	soc->gc->ngpio = soc->bank_num * 32;

	rval = devm_gpiochip_add_data(soc->dev, soc->gc, soc);
	if (rval)
		return rval;

	for (i = 0; i < soc->bank_num; i++) {
		writel_relaxed(0xffffffff, soc->gpio_base[i] + GPIO_ENABLE_OFFSET);
		writel_relaxed(0x00000000, soc->gpio_base[i] + GPIO_AFSEL_OFFSET);
		writel_relaxed(0x00000000, soc->gpio_base[i] + GPIO_MASK_OFFSET);
	}

	/* Initialize GPIO irq */
	soc->domain = irq_domain_add_linear(np, soc->gc->ngpio,
					&amb_gpio_irq_domain_ops, soc);
	if (!soc->domain) {
		dev_err(soc->dev, "Failed to create irqdomain\n");
		return -ENODEV;
	}

	for (i = 0; i < soc->bank_num; i++) {
		irq_set_irq_type(soc->irq[i], IRQ_TYPE_LEVEL_HIGH);
		irq_set_chained_handler_and_data(soc->irq[i], amb_gpio_handle_irq, soc);
	}

	return 0;
}

static int amb_pinctrl_probe(struct platform_device *pdev)
{
	struct amb_pinctrl_soc_data *soc;
	struct device_node *np;
	int i, rval;

	soc = devm_kzalloc(&pdev->dev, sizeof(*soc), GFP_KERNEL);
	if (!soc)
		return -ENOMEM;

	soc->dev = &pdev->dev;
	np = pdev->dev.of_node;
	amb_pinctrl_soc = soc;

	soc->bank_num = of_irq_count(np);
	if (!soc->bank_num || soc->bank_num > MAX_BANK_NUM)
		return dev_err_probe(&pdev->dev, -EINVAL, "Invalid gpio bank(irq)");

	for (i = 0; i < soc->bank_num; i++) {
		soc->gpio_base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(soc->gpio_base[i]))
			return dev_err_probe(&pdev->dev, PTR_ERR(soc->gpio_base[i]),
								 "couldn't get gpio[%d] reg", i);

		soc->irq[i] = platform_get_irq(pdev, i);
		if (soc->irq[i] < 0)
			return dev_err_probe(&pdev->dev, -ENODEV, "couldn't get gpio[%d] irq", i);
	}

	soc->iomux_base = devm_platform_ioremap_resource(pdev, i);
	if (IS_ERR(soc->iomux_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->iomux_base),
							 "couldn't get iomux reg");

	soc->ds_regmap = syscon_regmap_lookup_by_phandle(np, "amb,ds-regmap");
	if (IS_ERR(soc->ds_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->ds_regmap), "no ds regmap!");

	if (of_property_read_u32_index(np, "amb,ds-regmap", 1, &soc->ds0[0]) == 0) {
		for (i = 0; i < soc->bank_num; i++) {
			soc->ds0[i] = DS_OFFSET(soc->ds0[0], i);
			soc->ds1[i] = soc->ds0[i] + 4;
		}
	} else {
		for (i = 0; i < soc->bank_num; i++) {
			soc->ds0[i] = DS0_OFFSET(i);
			soc->ds1[i] = DS1_OFFSET(i);
		}
	}

	soc->pull_regmap = syscon_regmap_lookup_by_phandle(np, "amb,pull-regmap");
	if (IS_ERR(soc->pull_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(soc->pull_regmap), "no pull regmap!");

	if (of_property_read_u32_index(np, "amb,pull-regmap", 1, &soc->pull_en[0]) == 0) {
		for (i = 0; i < soc->bank_num; i++)
			soc->pull_en[i] = PULL_OFFSET(soc->pull_en[0], i);
	} else {
		for (i = 0; i < soc->bank_num; i++)
			soc->pull_en[i] = PULL_EN_OFFSET(i);
	}

	if (of_property_read_u32_index(np, "amb,pull-regmap", 2, &soc->pull_dir[0]) == 0) {
		for (i = 0; i < soc->bank_num; i++)
			soc->pull_dir[i] = PULL_OFFSET(soc->pull_dir[0], i);
	} else {
		for (i = 0; i < soc->bank_num; i++)
			soc->pull_dir[i] = PULL_DIR_OFFSET(i);
	}

	if (of_property_read_u32(np, "amb,hsm-domain", &soc->hsm_domain_id))
		soc->hsm_domain_id = 0;

	raw_spin_lock_init(&soc->lock);

	/* not allowed to use for non-existed pins */
	for (i = soc->bank_num * 32; i < MAX_BANK_NUM * 32; i++)
		set_bit(i, soc->used);

	rval = amb_pinctrl_register(soc);
	if (rval)
		return dev_err_probe(&pdev->dev, rval, "pinctrl register failed!");

	rval = amb_gpio_register(soc);
	if (rval)
		return dev_err_probe(&pdev->dev, rval, "gpio register failed!");

	platform_set_drvdata(pdev, soc);
	dev_info(&pdev->dev, "Ambarella pinctrl driver registered");

	return 0;
}

#if defined(CONFIG_PM)

static int amb_pinctrl_suspend(void)
{
	struct amb_pinctrl_soc_data *soc = amb_pinctrl_soc;
	u32 i;

	for (i = 0; i < soc->bank_num; i++) {
		regmap_read(soc->pull_regmap, soc->pull_en[i], &soc->pm[i].pull[0]);
		regmap_read(soc->pull_regmap, soc->pull_dir[i], &soc->pm[i].pull[1]);

		regmap_read(soc->ds_regmap, soc->ds0[i], &soc->pm[i].ds[0]);
		regmap_read(soc->ds_regmap, soc->ds1[i], &soc->pm[i].ds[1]);

		soc->pm[i].iomux[0] = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(i, 0));
		soc->pm[i].iomux[1] = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(i, 1));
		soc->pm[i].iomux[2] = readl_relaxed(soc->iomux_base + IOMUX_OFFSET(i, 2));

		soc->pm[i].afsel = readl_relaxed(soc->gpio_base[i] + GPIO_AFSEL_OFFSET);
		soc->pm[i].dir = readl_relaxed(soc->gpio_base[i] + GPIO_DIR_OFFSET);
		soc->pm[i].is = readl_relaxed(soc->gpio_base[i] + GPIO_IS_OFFSET);
		soc->pm[i].ibe = readl_relaxed(soc->gpio_base[i] + GPIO_IBE_OFFSET);
		soc->pm[i].iev = readl_relaxed(soc->gpio_base[i] + GPIO_IEV_OFFSET);
		soc->pm[i].ie = readl_relaxed(soc->gpio_base[i] + GPIO_IE_OFFSET);
		soc->pm[i].mask = readl_relaxed(soc->gpio_base[i] + GPIO_MASK_OFFSET);
		writel_relaxed(0xffffffff, soc->gpio_base[i] + GPIO_MASK_OFFSET);
		soc->pm[i].data = readl_relaxed(soc->gpio_base[i] + GPIO_DATA_OFFSET);

		if (soc->irq_wake_mask[i])
			writel_relaxed(soc->irq_wake_mask[i], soc->gpio_base[i] + GPIO_IE_OFFSET);
	}

	return 0;
}

static void amb_pinctrl_resume(void)
{
	struct amb_pinctrl_soc_data *soc = amb_pinctrl_soc;
	u32 i;

	for (i = 0; i < soc->bank_num; i++) {
		regmap_write(soc->pull_regmap, soc->pull_en[i], soc->pm[i].pull[0]);
		regmap_write(soc->pull_regmap, soc->pull_dir[i], soc->pm[i].pull[1]);

		regmap_write(soc->ds_regmap, soc->ds0[i], soc->pm[i].ds[0]);
		regmap_write(soc->ds_regmap, soc->ds1[i], soc->pm[i].ds[1]);

		writel_relaxed(soc->pm[i].iomux[0], soc->iomux_base + IOMUX_OFFSET(i, 0));
		writel_relaxed(soc->pm[i].iomux[1], soc->iomux_base + IOMUX_OFFSET(i, 1));
		writel_relaxed(soc->pm[i].iomux[2], soc->iomux_base + IOMUX_OFFSET(i, 2));

		writel_relaxed(soc->pm[i].afsel, soc->gpio_base[i] + GPIO_AFSEL_OFFSET);
		writel_relaxed(soc->pm[i].dir, soc->gpio_base[i] + GPIO_DIR_OFFSET);
		writel_relaxed(soc->pm[i].mask, soc->gpio_base[i] + GPIO_MASK_OFFSET);
		writel_relaxed(soc->pm[i].data, soc->gpio_base[i] + GPIO_DATA_OFFSET);
		writel_relaxed(soc->pm[i].is, soc->gpio_base[i] + GPIO_IS_OFFSET);
		writel_relaxed(soc->pm[i].ibe, soc->gpio_base[i] + GPIO_IBE_OFFSET);
		writel_relaxed(soc->pm[i].iev, soc->gpio_base[i] + GPIO_IEV_OFFSET);
		writel_relaxed(soc->pm[i].ie, soc->gpio_base[i] + GPIO_IE_OFFSET);
		writel_relaxed(0xffffffff, soc->gpio_base[i] + GPIO_ENABLE_OFFSET);
	}

	writel_relaxed(0x1, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
	writel_relaxed(0x0, soc->iomux_base + IOMUX_CTRL_SET_OFFSET);
}

static struct syscore_ops amb_pinctrl_syscore_ops = {
	.suspend	= amb_pinctrl_suspend,
	.resume		= amb_pinctrl_resume,
};

#endif /* CONFIG_PM */

static const struct of_device_id amb_pinctrl_dt_match[] = {
	{ .compatible = "ambarella,pinctrl" },
	{},
};
MODULE_DEVICE_TABLE(of, amb_pinctrl_dt_match);

static struct platform_driver amb_pinctrl_driver = {
	.probe	= amb_pinctrl_probe,
	.driver	= {
		.name	= "ambarella-pinctrl",
		.of_match_table = of_match_ptr(amb_pinctrl_dt_match),
	},
};

static int __init amb_pinctrl_drv_register(void)
{
#ifdef CONFIG_PM
	register_syscore_ops(&amb_pinctrl_syscore_ops);
#endif
	return platform_driver_register(&amb_pinctrl_driver);
}
arch_initcall(amb_pinctrl_drv_register);

MODULE_AUTHOR("Cao Rongrong <rrcao@ambarella.com>");
MODULE_DESCRIPTION("Ambarella SoC pinctrl driver");
MODULE_LICENSE("GPL");
