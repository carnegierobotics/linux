// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ambarella Neko driver.
 *
 * History: 2023/6/15 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2023 by Ambarella, Inc.
 *
 */
#define NEKO_DRIVER_NAME "neko"
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/export.h>
#include <linux/slab.h>
#include <linux/iopoll.h>
#include <asm-generic/errno-base.h>
#include <linux/printk.h>
#include <uapi/linux/amba-neko.h>
#include <soc/ambarella/epf-core.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/miscdevice.h>
#include <linux/of_platform.h>

#define to_neko(priv) container_of((priv), struct neko, miscdev)

static DEFINE_IDA(neko_ida);

struct neko {
	struct dma_chan *dma_chan_tx;
	struct dma_chan *dma_chan_rx;
	struct device *dev;
	struct miscdevice miscdev;
} * neko;

int neko_pci_ep_ob(dma_addr_t dma_dst, dma_addr_t dma_src, u32 total_len)
{
	return ambarella_pci_udma_xfer(neko->dev, dma_dst, dma_src, total_len,
				       DMA_MEM_TO_DEV, neko->dma_chan_tx, NULL,
				       NULL);
}

int neko_pci_ep_ib(dma_addr_t dma_dst, dma_addr_t dma_src, u32 total_len)
{
	return ambarella_pci_udma_xfer(neko->dev, dma_dst, dma_src, total_len,
				       DMA_DEV_TO_MEM, neko->dma_chan_rx, NULL,
				       NULL);
}

static int pci_neko_init_dma(struct neko *neko)
{
	struct device *dev = neko->dev;

	neko->dma_chan_tx = ambarella_acquire_udma_chan(DMA_MEM_TO_DEV, dev);
	if (!neko->dma_chan_tx)
		return -ENODEV;
	neko->dma_chan_rx = ambarella_acquire_udma_chan(DMA_DEV_TO_MEM, dev);
	if (!neko->dma_chan_rx)
		return -ENODEV;

	return 0;
}

static long neko_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = -EINVAL;
	void __user *argp = (void __user *)arg;
	struct xfer_info *info;

	info = memdup_user(argp, sizeof(*info));
	if (IS_ERR(info))
		return -EFAULT;

	pr_debug("local addr: %llx, remote addr: %llx, size: %lx\n",
		 info->local_phy_addr, info->remote_phy_addr, info->size);
	switch (cmd) {
	case PCINEKO_XFER:
		if (info->dir == PCI_READ)
			ret = neko_pci_ep_ib(info->local_phy_addr,
					     info->remote_phy_addr, info->size);
		else
			ret = neko_pci_ep_ob(info->remote_phy_addr,
					     info->local_phy_addr, info->size);
		break;
	}

	kfree(info);
	return ret;
}

static const struct file_operations neko_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = neko_ioctl,
};

static void pci_neko_cleanup_dma(struct neko *neko)
{
	dma_release_channel(neko->dma_chan_tx);
	neko->dma_chan_tx = NULL;
	dma_release_channel(neko->dma_chan_rx);
	neko->dma_chan_rx = NULL;
}

static int __init pci_neko_drv_init(void)
{
	struct miscdevice *misc_device;
	int id, err;
	struct device_node *ep_controller_node;
	struct device *dev;
	struct platform_device *parent_pdev;

	neko = vzalloc(sizeof(struct neko));
	misc_device = &neko->miscdev;

	id = ida_simple_get(&neko_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		err = id;
		pr_err("Unable to get id\n");
		goto free_neko;
	}

	misc_device->name = kasprintf(GFP_KERNEL, NEKO_DRIVER_NAME ".%d", id);
	if (!misc_device->name) {
		err = -ENOMEM;
		goto err_ida_remove;
	}

	misc_device = &neko->miscdev;
	misc_device->minor = MISC_DYNAMIC_MINOR;

	/**
	 *  FIXME: what if multiple endpoint controllers?
	 */
	ep_controller_node = of_find_compatible_node(NULL, "pci-endpoint",
						     "ambarella,cdns-pcie-ep");
	if (!ep_controller_node) {
		pr_err("failed to find pcie EP controller node!\n");
		err = -ENODEV;
		goto free_miscdev_name;
	}
	parent_pdev = of_find_device_by_node(ep_controller_node);
	if (!parent_pdev) {
		pr_err("failed to find pcie EP controller platform device!\n");
		err = -ENODEV;
		goto free_miscdev_name;
	}

	misc_device->fops = &neko_fops;

	misc_device->parent = &parent_pdev->dev;
	err = misc_register(misc_device);
	if (err) {
		pr_err("Failed to register device\n");
		err = -EINVAL;
		goto free_miscdev_name;
	}
	dev = misc_device->this_device;

	neko->dev = misc_device->this_device;
	err = pci_neko_init_dma(neko);
	if (err)
		goto deregister__miscdev;
	pr_info("register neko EP driver successfully\n");
	return 0;


deregister__miscdev:
	misc_deregister(misc_device);
free_miscdev_name:
	kfree(misc_device->name);
err_ida_remove:
	ida_simple_remove(&neko_ida, id);
free_neko:
	vfree(neko);
	return err;
}

static void __exit pci_neko_drv_exit(void)
{
	struct miscdevice *misc_device;
	int id;

	pci_neko_cleanup_dma(neko);

	misc_device = &neko->miscdev;
	if (sscanf(misc_device->name, NEKO_DRIVER_NAME ".%d", &id) != 1) {
		pr_err("invalid name\n");
		return;
	}
	if (id < 0) {
		pr_err("invalid id\n");
		return;
	}

	kfree(misc_device->name);
	misc_deregister(misc_device);
	ida_simple_remove(&neko_ida, id);
	vfree(neko);
	return;
}

module_init(pci_neko_drv_init);
module_exit(pci_neko_drv_exit);

MODULE_DESCRIPTION("PCI NEKO DRIVER");
MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_LICENSE("GPL v2");
