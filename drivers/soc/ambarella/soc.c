// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023, Ambarella, Inc.
 *
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include <linux/sys_soc.h>
#include <linux/regmap.h>
#include <asm-generic/errno-base.h>
#include <linux/mfd/syscon.h>
#include <soc/ambarella/misc.h>

#define AHB_CPUID_OFFSET		0x00

#define SYS_CONFIG_OFFSET		0x34
#define SOFT_OR_DLL_RESET_OFFSET	0x68

static struct ambarella_soc_id {
	unsigned int id;
	const char *name;
	const char *family;
} soc_ids[] = {
	{ 0x00483253, "s5l",   "Ambarella 14nm", },
	{ 0x00435632, "cv2",   "Ambarella 10nm", },
	{ 0x43563241, "cv2fs", "Ambarella 10nm", },
	{ 0x43563253, "cv22",  "Ambarella 10nm", },
	{ 0x43563245, "cv25",  "Ambarella 10nm", },
	{ 0x00483245, "s6lm",  "Ambarella 10nm", },
	{ 0x4356324C, "cv28m", "Ambarella 10nm", },
	{ 0x00435635, "cv5",   "Ambarella 5nm",  },
	{ 0x00435636, "cv3",   "Ambarella 5nm",  },
	{ 0x00435637, "cv72",  "Ambarella 5nm",  },
	{ 0x43563641, "cv3ad685", "Ambarella 5nm",  },
};

static const char * __init ambarella_socinfo_soc_id(unsigned int soc_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(soc_ids); i++)
		if (soc_id == soc_ids[i].id)
			return soc_ids[i].name;
	return NULL;
}

static const char * __init ambarella_socinfo_family(unsigned int soc_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(soc_ids); i++)
		if (soc_id == soc_ids[i].id)
			return soc_ids[i].family;
	return NULL;
}

static int __init ambarella_socinfo_init(void)
{
	struct soc_device_attribute *soc_dev_attr;
	struct soc_device *soc_dev;
	struct device_node *np;
	struct regmap *cpuid_regmap;
	unsigned int soc_id;

	cpuid_regmap = syscon_regmap_lookup_by_compatible("ambarella,cpuid");
	if (IS_ERR(cpuid_regmap))
		return PTR_ERR(cpuid_regmap);

	regmap_read(cpuid_regmap, AHB_CPUID_OFFSET, &soc_id);

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENODEV;

	np = of_find_node_by_path("/");
	of_property_read_string(np, "model", &soc_dev_attr->machine);
	of_node_put(np);

	soc_dev_attr->soc_id = ambarella_socinfo_soc_id(soc_id);
	if (!soc_dev_attr->soc_id) {
		pr_err("Unknown SoC ID\n");
		kfree(soc_dev_attr);
		return -ENODEV;
	}

	soc_dev_attr->family = ambarella_socinfo_family(soc_id);
	if (!soc_dev_attr->family) {
		pr_err("Unknown SoC Family\n");
		kfree(soc_dev_attr);
		return -ENODEV;
	}

	/* please note that the actual registration will be deferred */
	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR(soc_dev)) {
		kfree(soc_dev_attr);
		return PTR_ERR(soc_dev);
	}

	return 0;
}

static unsigned int ambsys_config;

unsigned int ambarella_sys_config(void)
{
	return ambsys_config;
}
EXPORT_SYMBOL(ambarella_sys_config);

static struct proc_dir_entry *ambroot_procfs_dir;

struct proc_dir_entry *ambarella_procfs_dir(void)
{
	return ambroot_procfs_dir;
}
EXPORT_SYMBOL(ambarella_procfs_dir);

static struct dentry *ambroot_debugfs_dir;

struct dentry *ambarella_debugfs_dir(void)
{
	return ambroot_debugfs_dir;
}
EXPORT_SYMBOL(ambarella_debugfs_dir);

static int __init ambarella_soc_init(void)
{
	struct regmap *rct_regmap;
	int ret;

	ambroot_procfs_dir = proc_mkdir("ambarella", NULL);
	if (IS_ERR_OR_NULL(ambroot_procfs_dir)) {
		pr_err("failed to create ambarella root proc dir\n");
		return -ENOMEM;
	}

	ambroot_debugfs_dir = debugfs_create_dir("ambarella", NULL);

	rct_regmap = syscon_regmap_lookup_by_compatible("ambarella,rct");
	if (IS_ERR(rct_regmap)) {
		pr_err("failed to get ambarella rct regmap\n");
		return PTR_ERR(rct_regmap);
	}

	regmap_read(rct_regmap, SYS_CONFIG_OFFSET, &ambsys_config);

	/* make sure software reboot bit is low, otherwise WDT cannot reset the chip */
	regmap_update_bits(rct_regmap, SOFT_OR_DLL_RESET_OFFSET, 0x1, 0x0);

	ret = ambarella_socinfo_init();
	if (ret < 0)
		return ret;

	return 0;
}

arch_initcall(ambarella_soc_init);
