// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2012-2023 Ambarella International LP
 *
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/rpmsg.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_ring.h>
#include <linux/remoteproc.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/jiffies.h>
#include <linux/wait.h>

#include "rpmsg_slave.h"

#define VRING_DESC_NUM			32
#define VRING_ALIGN			64
#define RPMSG_BUFFER_SIZE		512
#define VIRTIO_VQ_NUM			2
#define to_ambarella_rpmsg(d)		container_of(d, struct ambarella_rpmsg_dev, vdev)

enum virtio_role {
	VIRTIO_ROLE_MASTER,
	VIRTIO_ROLE_SLAVE,
};

struct shared_resource_table {
	/* rpmsg vdev entry */
	struct fw_rsc_hdr hdr;
	struct fw_rsc_vdev rsc;
};

struct ambarella_rpmsg_dev {
	struct virtio_device vdev;
	struct device *dev;
	struct virtqueue *vq[VIRTIO_VQ_NUM];
	struct regmap *reg_scr;
	struct virtio_slave slave;
	void *vring_mem;
	struct shared_resource_table *rsrc_tbl;
	void *vq_mem[VIRTIO_VQ_NUM];
#if !defined(RPMSG_ENABLE_TASKLET)
	struct work_struct work;
#else
	struct tasklet_struct tasklet;		/* CPU usage: + 10%*/
#endif
	u32 irq_bit_map[2];
	u32 role;		/* 0 - master, 1 - slave */
	u32 irq_set_reg;
	u32 irq_clr_reg;
	u32 vr_num;
	u32 master_timeout;	/* ms */
	struct completion master_online;
};

static u32 txq_idx;
static u32 rxq_idx;
static u32 rpmsg_buffer_size = RPMSG_BUFFER_SIZE;
/* ------------------------------------------------------------------------- */
static bool ambarella_rpmsg_role_master(struct ambarella_rpmsg_dev *rpdev)
{
	return rpdev->role == VIRTIO_ROLE_MASTER;
}

static void ambarella_rpmsg_wait_master_online(struct ambarella_rpmsg_dev *rpdev)
{
	u32 err;
	u32 timeout;

	if (rpdev->master_timeout)
		timeout = rpdev->master_timeout;
	else
		timeout = 5000;

	err = wait_for_completion_timeout(&rpdev->master_online,
					  msecs_to_jiffies(timeout));

	if (err == 0) {
		dev_warn(rpdev->dev, "Timeout waiting for master online\n");
	} else {
		/* delay secs to ensure RPMSG in master is completely ready */
		msleep(500);
	}

}

#if !defined(RPMSG_ENABLE_TASKLET)
static void ambarella_rpmsg_work_handler(struct work_struct *work)
{
	struct ambarella_rpmsg_dev *rpmsg_dev =
		container_of(work, struct ambarella_rpmsg_dev, work);

	if (rpmsg_dev->vq[0]) {
		if (ambarella_rpmsg_role_master(rpmsg_dev))
			vring_interrupt(0, rpmsg_dev->vq[0]);
		else
			vring_slave_interrupt(0, rpmsg_dev->vq[0]);
	}
}
#else
static void ambarella_rpmsg_tasklet(unsigned long arg)
{
	struct ambarella_rpmsg_dev *rpmsg_dev =
		(struct ambarella_rpmsg_dev *)arg;

	if (rpmsg_dev->vq[0]) {
		if (ambarella_rpmsg_role_master(rpmsg_dev))
			vring_interrupt(0, rpmsg_dev->vq[0]);
		else
			vring_slave_interrupt(0, rpmsg_dev->vq[0]);
	}
}
#endif

static irqreturn_t ambarella_rpmsg_isr(int irq, void *data)
{
	struct ambarella_rpmsg_dev *rpmsg_dev = (struct ambarella_rpmsg_dev *)data;

	/* ACK AXI software IRQ */
	if (rpmsg_dev && rpmsg_dev->reg_scr) {
		regmap_write(rpmsg_dev->reg_scr, rpmsg_dev->irq_clr_reg,
			     BIT(rpmsg_dev->irq_bit_map[rxq_idx]));

#if !defined(RPMSG_ENABLE_TASKLET)
		schedule_work(&rpmsg_dev->work);
#else
		tasklet_schedule(&rpmsg_dev->tasklet);
#endif
		if (!ambarella_rpmsg_role_master(rpmsg_dev)) {
			complete(&rpmsg_dev->master_online);
		}
	}


	return IRQ_HANDLED;
}

static bool ambarella_rpmsg_notify(struct virtqueue *vq)
{
	struct ambarella_rpmsg_dev *rpmsg_dev = vq->priv;

	/* Send AXI software IRQ to slave */
	regmap_write(rpmsg_dev->reg_scr, rpmsg_dev->irq_set_reg,
		     BIT(rpmsg_dev->irq_bit_map[txq_idx]));

	return true;
}

static void ambarella_rpmsg_del_vqs(struct virtio_device *vdev)
{
	struct ambarella_rpmsg_dev *rpmsg_dev = to_ambarella_rpmsg(vdev);
	int i;

	for (i = 0; i < ARRAY_SIZE(rpmsg_dev->vq); i++) {
		if (rpmsg_dev->vq[i]) {
			if (ambarella_rpmsg_role_master(rpmsg_dev))
				vring_del_virtqueue(rpmsg_dev->vq[i]);
			rpmsg_dev->vq[i] = NULL;
		}
	}
}

static int ambarella_rpmsg_find_vqs(struct virtio_device *vdev,
				    unsigned nvqs,
				    struct virtqueue *vqs[],
				    vq_callback_t *callbacks[],
				    const char * const names[],
				    const bool *ctx,
				    struct irq_affinity *desc)
{
	u32 i;
	struct virtqueue *vq;
	struct ambarella_rpmsg_dev *rpmsg_dev = to_ambarella_rpmsg(vdev);

	for (i = 0; i < nvqs; i++) {
		if (!names[i] || i >= ARRAY_SIZE(rpmsg_dev->vq)) {
			vqs[i] = NULL;
			continue;
		}

		vq = vring_new_virtqueue(i,
					 rpmsg_dev->vr_num,	/* num */
					 VRING_ALIGN,		/* align */
					 vdev, false, false,
					 rpmsg_dev->vq_mem[i],
					 ambarella_rpmsg_notify,
					 callbacks[i],
					 names[i]);
		if (!vq) {
			ambarella_rpmsg_del_vqs(vdev);
			return -ENOMEM;
		}

		vq->priv = rpmsg_dev;
		vqs[i] = rpmsg_dev->vq[i] = vq;
	}

	return 0;
}

static void ambarella_rpmsg_reset(struct virtio_device *vdev)
{
	struct ambarella_rpmsg_dev *rpmsg_dev = to_ambarella_rpmsg(vdev);

	rpmsg_dev->rsrc_tbl->rsc.status = 0;
}

static u8 ambarella_rpmsg_get_status(struct virtio_device *vdev)
{
	struct ambarella_rpmsg_dev *rpmsg_dev = to_ambarella_rpmsg(vdev);

	return rpmsg_dev->rsrc_tbl->rsc.status;
}

static void ambarella_rpmsg_set_status(struct virtio_device *vdev, u8 status)
{
	struct ambarella_rpmsg_dev *rpmsg_dev = to_ambarella_rpmsg(vdev);

	rpmsg_dev->rsrc_tbl->rsc.status = status;
}

static u64 ambarella_rpmsg_get_features(struct virtio_device *vdev)
{
	/* return feature: */
	return BIT_ULL(0);
}

static int ambarella_rpmsg_finalize_features(struct virtio_device *vdev)
{
	struct ambarella_rpmsg_dev *rpmsg_dev = to_ambarella_rpmsg(vdev);

	/* Give virtio_ring a chance to accept features. */
	vring_transport_features(vdev);

	/*
	 * Remember the finalized features of our vdev, and provide it
	 * to the remote processor once it is powered on.
	 */
	rpmsg_dev->rsrc_tbl->rsc.gfeatures = vdev->features;

	return 0;
}

static void ambarella_rpmsg_dev_release(struct device *dev)
{
}

static void ambarella_rpmsg_get(struct virtio_device *vdev,
				unsigned int offset,
				void *buf,
				unsigned int len)
{
}

static void ambarella_rpmsg_set(struct virtio_device *vdev,
				unsigned int offset,
				const void *buf,
				unsigned int len)
{
}

static struct virtio_config_ops ambarella_rpmsg_config = {
	.get_features		= ambarella_rpmsg_get_features,
	.finalize_features	= ambarella_rpmsg_finalize_features,
	.find_vqs		= ambarella_rpmsg_find_vqs,
	.del_vqs		= ambarella_rpmsg_del_vqs,
	.reset			= ambarella_rpmsg_reset,
	.set_status		= ambarella_rpmsg_set_status,
	.get_status		= ambarella_rpmsg_get_status,
	.get			= ambarella_rpmsg_get,
	.set			= ambarella_rpmsg_set,
};

u32 ambarella_rpmsg_buffer_size(void)
{
	return rpmsg_buffer_size;
}
EXPORT_SYMBOL_GPL(ambarella_rpmsg_buffer_size);

static int ambarella_rpmsg_of_parser(struct ambarella_rpmsg_dev *rpmsg_dev,
				     struct device_node *np)
{
	if (of_property_read_bool(np, "amb,role-slave")) {

		rpmsg_dev->role = VIRTIO_ROLE_SLAVE;
		if (of_property_read_string(np, "amb,rpmsg-user",
					    &rpmsg_dev->slave.rpmsg_user)) {
			dev_warn(rpmsg_dev->dev, "No rpmsg user is specified\n");
		} else {
			dev_dbg(rpmsg_dev->dev, "rpmsg user: %s\n",
				rpmsg_dev->slave.rpmsg_user);
		}

	} else {
		rpmsg_dev->role = VIRTIO_ROLE_MASTER;
	}

	rpmsg_dev->reg_scr =
		syscon_regmap_lookup_by_phandle(np, "amb,scr-regmap");
	if (IS_ERR(rpmsg_dev->reg_scr)) {
		return -1;
	}

	if (of_property_read_u32_array(np, "amb,axi-irq-bmap",
				       rpmsg_dev->irq_bit_map, 2)) {
		return -1;
	}

	if (of_property_read_u32(np, "amb,axi-irq-set-reg",
				 &rpmsg_dev->irq_set_reg)) {
		return -1;
	}

	if (of_property_read_u32(np, "amb,axi-irq-clr-reg",
				 &rpmsg_dev->irq_clr_reg)) {
		return -1;
	}

	if (of_property_read_u32(np, "amb,vring-desc-num",
				 &rpmsg_dev->vr_num)) {
		rpmsg_dev->vr_num = VRING_DESC_NUM;
	}

	if (of_property_read_u32(np, "amb,rpmsg-buffer-size",
				 &rpmsg_buffer_size)) {
		rpmsg_buffer_size = RPMSG_BUFFER_SIZE;
	}

	if (of_property_read_u32(np, "amb,wait-master-timeout",
				 &rpmsg_dev->master_timeout)) {
		rpmsg_dev->master_timeout = 0;
	}

	rpmsg_buffer_size = ALIGN(rpmsg_buffer_size, 512);

	return 0;
}

static int ambarella_rpmsg_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ambarella_rpmsg_dev *rpmsg_dev;
	struct device *dev = &pdev->dev;
	struct resource *res;
	size_t size;
	const char *irq_name[2] = {"txq", "rxq"};
	int irq, rval;

	rpmsg_dev = devm_kzalloc(dev, sizeof(*rpmsg_dev), GFP_KERNEL);
	if (!rpmsg_dev)
		return -ENOMEM;

	rpmsg_dev->dev = dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		return -ENODEV;
	}

	rpmsg_dev->vring_mem = devm_ioremap_wc(dev, res->start, resource_size(res));
	if (!rpmsg_dev->vring_mem) {
		dev_err(dev, "ioremap failed\n");
		return -ENOMEM;
	}

	rval = ambarella_rpmsg_of_parser(rpmsg_dev, np);
	if (rval) {
		dev_err(dev, "of_parse failed\n");
		return rval;
	}

	size = ALIGN(vring_size(rpmsg_dev->vr_num, VRING_ALIGN), 512);
	if (sizeof(struct shared_resource_table) + size * VIRTIO_VQ_NUM >
	    resource_size(res)) {
		dev_err(dev, "vring size is not enough\n");
		return -EINVAL;
	}

	txq_idx = rpmsg_dev->role == VIRTIO_ROLE_MASTER ? 0 : 1;
	rxq_idx = rpmsg_dev->role == VIRTIO_ROLE_MASTER ? 1 : 0;

	if (rpmsg_dev->role == VIRTIO_ROLE_MASTER) {
		rpmsg_dev->vq_mem[0] = rpmsg_dev->vring_mem;
		rpmsg_dev->vq_mem[1] = rpmsg_dev->vring_mem + size;
		memset_io(rpmsg_dev->vq_mem[0], 0, size);
		memset_io(rpmsg_dev->vq_mem[1], 0, size);
	} else {
		rpmsg_dev->vq_mem[1] = rpmsg_dev->vring_mem;
		rpmsg_dev->vq_mem[0] = rpmsg_dev->vring_mem + size;
	}

	rpmsg_dev->rsrc_tbl = rpmsg_dev->vring_mem + size * VIRTIO_VQ_NUM;

	if (rpmsg_dev->role == VIRTIO_ROLE_MASTER)
		memset_io(rpmsg_dev->rsrc_tbl, 0, sizeof(struct shared_resource_table));

#if !defined(RPMSG_ENABLE_TASKLET)
	INIT_WORK(&rpmsg_dev->work, ambarella_rpmsg_work_handler);
#else
	tasklet_init(&rpmsg_dev->tasklet, ambarella_rpmsg_tasklet,
		     (unsigned long)rpmsg_dev);
#endif

	/* register RX irq */
	irq = platform_get_irq_byname(pdev, irq_name[rxq_idx]);
	if (irq < 0) {
		dev_err(dev, "get irq '%s' failed\n", irq_name[rxq_idx]);
		return -ENXIO;
	}

	rval = devm_request_irq(dev, irq, ambarella_rpmsg_isr, 0,
				dev_name(dev), rpmsg_dev);
	if (rval) {
		dev_err(dev, "failed to request irq\n");
		return rval;
	}

	if (rpmsg_dev->role == VIRTIO_ROLE_MASTER) {

		rval = of_reserved_mem_device_init_by_idx(dev, dev->of_node, 0);
		if (rval) {
			dev_err(dev, "Can't associate reserved memory\n");
			return rval;
		}

	} else {
		struct device_node *node;
		const __be32 *reg;
		phys_addr_t base, size;

		node = of_parse_phandle(np, "memory-region", 0);
		if (!node) {
			dev_err(dev, "node 'memory-region' is not found\n");
			return -ENODEV;
		}

		reg = of_get_property(node, "reg", NULL);
		if (!reg) {
			dev_err(dev, "property 'reg' is not found\n");
			of_node_put(node);
			return -ENODEV;
		}

		base = of_read_number(reg, of_n_addr_cells(node));
		reg += of_n_addr_cells(node);
		size = of_read_number(reg, of_n_size_cells(node));
		of_node_put(node);

		rpmsg_dev->slave.start_pa = base;
		rpmsg_dev->slave.end_pa = base + size;
		rpmsg_dev->slave.buf_va = devm_ioremap_wc(dev, base, size);
		if (!rpmsg_dev->slave.buf_va)
			return -ENOMEM;

		dev_set_drvdata(dev, &rpmsg_dev->slave);
	}

	if (!ambarella_rpmsg_role_master(rpmsg_dev)) {
		init_completion(&rpmsg_dev->master_online);
		ambarella_rpmsg_wait_master_online(rpmsg_dev);
	}

	rpmsg_dev->vdev.id.device = rpmsg_dev->role == VIRTIO_ROLE_MASTER ?
		VIRTIO_ID_RPMSG : VIRTIO_ID_BLZNET;

	rpmsg_dev->vdev.config = &ambarella_rpmsg_config;
	rpmsg_dev->vdev.dev.parent = dev;
	rpmsg_dev->vdev.dev.release = ambarella_rpmsg_dev_release;

	rval = register_virtio_device(&rpmsg_dev->vdev);
	if (rval) {
		pr_err("register_virtio_device error %d\n", rval);
	} else {
		dev_info(dev, "virtio register as %s\n",
			 rpmsg_dev->role == VIRTIO_ROLE_MASTER ? "master" : "slave");
	}

	return rval;
}

static const struct of_device_id ambarella_rpmsg_dt_ids[] = {
	{ .compatible = "ambarella,rpmsg",},
	{ /* sentinel */ }
};

static struct platform_driver ambarella_rpmsg_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "ambarella-rpmsg",
		.of_match_table = ambarella_rpmsg_dt_ids,
	},
	.probe = ambarella_rpmsg_probe,
};

static int __init ambarella_rpmsg_init(void)
{
	int rval;

	rval = platform_driver_register(&ambarella_rpmsg_driver);
	if (rval) {
		pr_err("Unable to initialize rpmsg driver\n");
		return rval;
	}

	return 0;
}
subsys_initcall(ambarella_rpmsg_init);

static void __exit ambarella_rpmsg_exit(void)
{
	platform_driver_unregister(&ambarella_rpmsg_driver);
}
module_exit(ambarella_rpmsg_exit);

MODULE_AUTHOR("Jorney <qtu@ambarella.com>");
MODULE_DESCRIPTION("Ambarella RPMSG Driver");
MODULE_LICENSE("GPL v2");
