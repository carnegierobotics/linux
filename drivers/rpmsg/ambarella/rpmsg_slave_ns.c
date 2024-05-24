// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2012-2023 Ambarella International LP
 */
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/rpmsg/ns.h>
#include <linux/slab.h>

#include "../rpmsg_internal.h"

int rpmsg_slave_ns_register_device(struct rpmsg_device *rpdev)
{
	strcpy(rpdev->id.name, "rpmsg_slave_ns");
	rpdev->driver_override = "rpmsg_slave_ns";
	rpdev->src = RPMSG_NS_ADDR;
	rpdev->dst = RPMSG_NS_ADDR;

	return rpmsg_register_device(rpdev);
}
EXPORT_SYMBOL(rpmsg_slave_ns_register_device);

static int rpmsg_slave_ns_lookup(int id, void *p, void *data)
{
	struct rpmsg_ns_msg *msg = data;
	struct rpmsg_endpoint *ept = p;

	if (!strncmp(msg->name, ept->rpdev->id.name,
		     strlen(ept->rpdev->id.name))) {

		pr_info("%s: Bind Destination -> %u\n", msg->name, msg->addr);
		ept->rpdev->dst = msg->addr;
		return 0;
	}

	return 0;
}

static int rpmsg_slave_ns_cb(struct rpmsg_device *rpdev, void *data, int len,
		       void *priv, u32 src)
{
	struct idr *endp = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_ns_msg *msg = data;

	pr_debug("%s, %u\n", msg->name, msg->addr);
	idr_for_each(endp, rpmsg_slave_ns_lookup, data);

	return 0;
}

static int rpmsg_slave_ns_probe(struct rpmsg_device *rpdev)
{
	struct rpmsg_endpoint *ept;
	struct rpmsg_channel_info ns_chinfo = {
		.src = RPMSG_NS_ADDR,
		.dst = RPMSG_NS_ADDR,
		.name = "name_service",
	};

	ept = rpmsg_create_ept(rpdev, rpmsg_slave_ns_cb, NULL, ns_chinfo);
	if (!ept) {
		dev_err(&rpdev->dev, "failed to create the ns ept\n");
		return -ENOMEM;
	}

	rpdev->ept = ept;

	return 0;
}

static struct rpmsg_driver rpmsg_slave_ns_driver = {
	.drv.name = "rpmsg_slave_ns",
	.probe = rpmsg_slave_ns_probe,
};

static int rpmsg_slave_ns_init(void)
{
	int ret;

	ret = register_rpmsg_driver(&rpmsg_slave_ns_driver);
	if (ret < 0)
		pr_err("%s: Failed to register rpmsg driver\n", __func__);

	return ret;
}
postcore_initcall(rpmsg_slave_ns_init);

static void rpmsg_slave_ns_exit(void)
{
	unregister_rpmsg_driver(&rpmsg_slave_ns_driver);
}
module_exit(rpmsg_slave_ns_exit);

MODULE_DESCRIPTION("Name service announcement rpmsg slave driver");
MODULE_ALIAS("rpmsg:" KBUILD_MODNAME);
MODULE_LICENSE("GPL v2");
