// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ambarella Rainy endpoint function pci EP-side driver.
 *
 * History: 2023/06/29 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2023 by Ambarella, Inc.
 *
 * Abbrev:
 *
 * eeo: ep ob codes run under EP-side kernel
 * eei: ep ib codes run under EP-side kernel
 *
 * reg bar: bar used to store epf's register, like size, addr and etc.
 */

#include <linux/export.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/iopoll.h>
#include <asm-generic/errno-base.h>
#include <linux/printk.h>
#include <soc/ambarella/rainy.h>
#include <soc/ambarella/epf-core.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/types.h>

rainy_module_parameters;

struct rainy_ep {
	struct dma_chan *dma_chan_tx;
	struct dma_chan *dma_chan_rx;
	struct mutex mutex;
	struct mutex cmd_mutex;
	void __iomem *bar[PCI_STD_NUM_BARS];
	enum pci_barno reg_bar;

	struct delayed_work cmd_handler;

	struct pci_epf *epf;
	const struct pci_epc_features *epc_features;
	struct miscdevice miscdev;
	struct device *dev;
};

extern struct rainy_ep *rainy_ep;
extern dma_addr_t rainy_get_rc_dma_region_size(int port);
extern int rainy_pci_ep_ib(dma_addr_t dma_dst, dma_addr_t dma_src,
			   u32 total_len);
extern int rainy_pci_ep_ob(dma_addr_t dma_dst, dma_addr_t dma_src,
			   u32 total_len);
extern dma_addr_t rainy_get_rc_dma_region_size(int port);
extern dma_addr_t rainy_get_rc_dma_addr(int port);

#define RAINY_DRIVER_NAME "pci_epf_rainy"

static DEFINE_IDA(rainy_ida);

static struct pci_epf_header default_epf_header = {
	.vendorid = PCI_ANY_ID,
	.deviceid = PCI_ANY_ID,
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin = PCI_INTERRUPT_INTA,
};

// TODO: remove global variable.
struct rainy_ep *rainy_ep;
EXPORT_SYMBOL(rainy_ep);

static void rainy_eeo_dma_callback(void *param)
{
}

/**
 * rainy_get_rc_dma_addr()) - get rc's buffer's dma address
 *
 * Invoke to get rc buffer's dma addr if ep wants to do ob/ib.
 * Note that final dma_addr can be dma_addr + offset, offset
 * should be <= rainy_get_rc_dma_region_size();
 */
dma_addr_t rainy_get_rc_dma_addr(int port)
{
	struct rainy_msg __iomem *msginfo;
	struct rainy_dma_info __iomem *dma_info;

	msginfo = rainy_ep->bar[rainy_ep->reg_bar];
	dma_info = &msginfo->dma_info[port];

	return readl(&dma_info->rc_dma_addr) |
	       (dma_addr_t)readl(&dma_info->rc_dma_upper_addr) << 32;
}
EXPORT_SYMBOL(rainy_get_rc_dma_addr);

/**
 * rainy_get_rc_dma_region_size() - get rc buffer's dma size
 *
 * Invoke to get rc buffer's dma size if ep wants to do ob/ib.
 * Note that final dma_addr can be dma_addr + offset, offset
 * should be <= rainy_get_rc_dma_region_size();
 */
dma_addr_t rainy_get_rc_dma_region_size(int port)
{
	struct rainy_msg __iomem *msginfo;
	struct rainy_dma_info __iomem *dma_info;

	msginfo = rainy_ep->bar[rainy_ep->reg_bar];
	dma_info = &msginfo->dma_info[port];

	return dma_info->rc_dma_region_size;
}
EXPORT_SYMBOL(rainy_get_rc_dma_region_size);

int rainy_pci_ep_ob(dma_addr_t dma_dst, dma_addr_t dma_src, u32 total_len)
{
	return ambarella_pci_udma_xfer(&rainy_ep->epf->dev, dma_dst, dma_src,
				       total_len, DMA_MEM_TO_DEV,
				       rainy_ep->dma_chan_tx,
				       rainy_eeo_dma_callback,
				       rainy_ep->bar[rainy_ep->reg_bar]);
}
EXPORT_SYMBOL(rainy_pci_ep_ob);

static void rainy_eei_dma_callback(void *param)
{
}

int rainy_pci_ep_ib(dma_addr_t dma_dst, dma_addr_t dma_src, u32 total_len)
{
	return ambarella_pci_udma_xfer(&rainy_ep->epf->dev, dma_dst, dma_src,
				       total_len, DMA_DEV_TO_MEM,
				       rainy_ep->dma_chan_rx,
				       rainy_eei_dma_callback,
				       rainy_ep->bar[rainy_ep->reg_bar]);
}
EXPORT_SYMBOL(rainy_pci_ep_ib);

static int pci_rainy_ep_init_dma(struct rainy_ep *rainy_ep)
{
	struct device *dev = &rainy_ep->epf->epc->dev;

	rainy_ep->dma_chan_tx =
		ambarella_acquire_udma_chan(DMA_MEM_TO_DEV, dev);
	if (!rainy_ep->dma_chan_tx)
		return -ENODEV;
	rainy_ep->dma_chan_rx =
		ambarella_acquire_udma_chan(DMA_DEV_TO_MEM, dev);
	if (!rainy_ep->dma_chan_rx)
		return -ENODEV;

	return 0;
}

static void pci_rainy_cleanup_dma(struct rainy_ep *rainy_ep)
{
	dma_release_channel(rainy_ep->dma_chan_tx);
	rainy_ep->dma_chan_tx = NULL;
	dma_release_channel(rainy_ep->dma_chan_rx);
	rainy_ep->dma_chan_rx = NULL;
}

static int pci_rainy_ep_set_bar(struct pci_epf *epf)
{
	int bar, add, ret;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct rainy_ep *rainy_ep = epf_get_drvdata(epf);
	enum pci_barno reg_bar = rainy_ep->reg_bar;
	const struct pci_epc_features *epc_features;

	epc_features = rainy_ep->epc_features;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar += add) {
		epf_bar = &epf->bar[bar];
		/*
		 * pci_epc_set_bar() sets PCI_BASE_ADDRESS_MEM_TYPE_64
		 * if the specific implementation required a 64-bit BAR,
		 * even if we only requested a 32-bit BAR.
		 */
		add = (epf_bar->flags & PCI_BASE_ADDRESS_MEM_TYPE_64) ? 2 : 1;

		if (!!(epc_features->reserved_bar & (1 << bar)))
			continue;

		ret = pci_epc_set_bar(epc, epf->func_no, epf->vfunc_no,
				      epf_bar);
		if (ret) {
			pci_epf_free_space(epf, rainy_ep->bar[bar], bar,
					   PRIMARY_INTERFACE);
			dev_err(dev, "Failed to set BAR%d", bar);
			if (bar == reg_bar)
				return ret;
		}
	}

	return 0;
}

static int pci_rainy_ep_core_init(struct pci_epf *epf)
{
	struct pci_epf_header *header = epf->header;
	const struct pci_epc_features *epc_features;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;

	int ret;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);

	ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no, header);
	if (ret) {
		dev_err(dev, "Configuration header write failed\n");
		return ret;
	}

	ret = pci_rainy_ep_set_bar(epf);
	if (ret)
		return ret;

	return 0;
}

static int pci_rainy_ep_alloc_space(struct pci_epf *epf)
{
	struct rainy_ep *rainy_ep = epf_get_drvdata(epf);
	struct device *dev = &epf->dev;
	size_t reg_bar_size;
	void __iomem *base;
	enum pci_barno reg_bar = rainy_ep->reg_bar;
	const struct pci_epc_features *epc_features;
	size_t notify_msg_reg_size;

	epc_features = rainy_ep->epc_features;
	reg_bar_size = ALIGN(sizeof(struct rainy_msg), 128);

	notify_msg_reg_size = reg_bar_size;

	if (!epc_features->bar_fixed_size[reg_bar]) {
		dev_err(dev, "%s: failed to get reg bar\n", __func__);
		return -ENODEV;
	}

	if (notify_msg_reg_size >
	    epc_features->bar_fixed_size[rainy_ep->reg_bar])
		return -ENOMEM;

	notify_msg_reg_size = epc_features->bar_fixed_size[rainy_ep->reg_bar];

	/* Init reg bar */
	base = pci_epf_alloc_space(epf, notify_msg_reg_size, reg_bar,
				   epc_features->align, PRIMARY_INTERFACE);

	if (!base) {
		dev_err(dev, "Failed to allocated register space\n");
		return -ENOMEM;
	}
	rainy_ep->bar[reg_bar] = base;

	return 0;
}

static int pci_rainy_ep_drv_bind(struct pci_epf *epf)
{
	int ret;
	struct rainy_ep *rainy_ep = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	enum pci_barno reg_bar = EP_MSG_BAR;
	struct pci_epc *epc = epf->epc;

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	if (WARN_ON_ONCE(!rainy_ep))
		return -EINVAL;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (epc_features)
		ambarella_ep_configure_bar(epf, epc_features);
	else {
		dev_err(&epf->dev, "%s: failed to get epc_features\n",
			__func__);
		return -EINVAL;
	}

	rainy_ep->reg_bar = reg_bar;
	rainy_ep->epc_features = epc_features;

	ret = pci_rainy_ep_alloc_space(epf);
	if (ret)
		return ret;

	ret = pci_rainy_ep_core_init(epf);
	if (ret)
		return ret;

	ret = pci_rainy_ep_init_dma(rainy_ep);
	if (ret)
		return ret;

	return 0;
}

static void pci_rainy_ep_drv_unbind(struct pci_epf *epf)
{
	struct rainy_ep *rainy_ep = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar;
	int bar;

	cancel_delayed_work(&rainy_ep->cmd_handler);
	pci_rainy_cleanup_dma(rainy_ep);
	pci_epc_stop(epc);
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		epf_bar = &epf->bar[bar];

		if (rainy_ep->bar[bar]) {
			pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
					  epf_bar);
			pci_epf_free_space(epf, rainy_ep->bar[bar], bar,
					   PRIMARY_INTERFACE);
		}
	}
}

static const struct pci_epf_device_id pci_rainy_ep_dev_ids[] = {
	{
		.name = RAINY_DRIVER_NAME,
	},
	{},
};

static long rainy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = -EINVAL;
	//void __user *argp = (void __user *)arg;
	//struct xfer_info *info;

	//info = memdup_user(argp, sizeof(*info));
	//if (IS_ERR(info))
	//	return -EFAULT;

	//kfree(info);
	return ret;
}

static const struct file_operations rainy_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = rainy_ioctl,
};

static int pci_rainy_ep_drv_probe(struct pci_epf *epf)
{
	int ret = 0, id;
	struct device *dev = &epf->dev;
	struct miscdevice *misc_device;
	struct device_node *ep_controller_node;
	struct platform_device *parent_pdev;

	rainy_ep = devm_kzalloc(dev, sizeof(*rainy_ep), GFP_KERNEL);
	if (!rainy_ep)
		return -ENOMEM;

	mutex_init(&rainy_ep->mutex);
	mutex_init(&rainy_ep->cmd_mutex);

	epf->header = &default_epf_header;
	rainy_ep->epf = epf;

	epf_set_drvdata(epf, rainy_ep);

	/*set RAINY_DRIVER_NAME coherent_mask to utilize cma*/
	dma_set_coherent_mask(&epf->dev, DMA_BIT_MASK(64));

	misc_device = &rainy_ep->miscdev;
	id = ida_simple_get(&rainy_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		pr_err("Unable to get id\n");
		return ret;
	}

	misc_device->name = kasprintf(GFP_KERNEL, RAINY_DRIVER_NAME ".%d", id);
	if (!misc_device->name) {
		ret = -ENOMEM;
		goto err_ida_remove;
	}

	misc_device = &rainy_ep->miscdev;
	misc_device->minor = MISC_DYNAMIC_MINOR;

	/**
	 *  FIXME: what if multiple endpoint controllers?
	 */
	ep_controller_node = of_find_compatible_node(NULL, "pci-endpoint",
						     "ambarella,cdns-pcie-ep");
	if (!ep_controller_node) {
		pr_err("failed to find pcie EP controller node!\n");
		ret = -ENODEV;
		goto free_miscdev_name;
	}
	parent_pdev = of_find_device_by_node(ep_controller_node);
	if (!parent_pdev) {
		pr_err("failed to find pcie EP controller platform device!\n");
		ret = -ENODEV;
		goto free_miscdev_name;
	}

	misc_device->fops = &rainy_fops;

	misc_device->parent = &parent_pdev->dev;
	ret = misc_register(misc_device);
	if (ret) {
		pr_err("Failed to register device\n");
		ret = -EINVAL;
		goto free_miscdev_name;
	}
	dev = misc_device->this_device;

	rainy_ep->dev = misc_device->this_device;

	return 0;

free_miscdev_name:
	kfree(misc_device->name);
err_ida_remove:
	ida_simple_remove(&rainy_ida, id);
	return ret;
}

static struct pci_epf_ops ops = {
	.unbind = pci_rainy_ep_drv_unbind,
	.bind = pci_rainy_ep_drv_bind,
	.set_bar = pci_rainy_ep_set_bar,
};

static struct pci_epf_driver pci_rainy_ep_driver = {
	.driver.name = RAINY_DRIVER_NAME,
	.probe = pci_rainy_ep_drv_probe,
	.id_table = pci_rainy_ep_dev_ids,
	.ops = &ops,
	.owner = THIS_MODULE,
};

static int __init pci_rainy_ep_drv_init(void)
{
	int ret;

	ret = pci_epf_register_driver(&pci_rainy_ep_driver);
	if (ret) {
		pr_err("Failed to register rainy driver --> %d", ret);
		return ret;
	}

	pr_info("%s: register rainy EP driver successfully\n", __func__);

	return 0;
}
module_init(pci_rainy_ep_drv_init);

static void __exit pci_rainy_ep_drv_exit(void)
{
	struct miscdevice *misc_device;
	int id;

	misc_device = &rainy_ep->miscdev;
	if (sscanf(misc_device->name, RAINY_DRIVER_NAME ".%d", &id) != 1) {
		pr_err("invalid name\n");
		return;
	}
	if (id < 0) {
		pr_err("invalid id\n");
		return;
	}

	kfree(misc_device->name);
	misc_deregister(misc_device);
	ida_simple_remove(&rainy_ida, id);
	pci_epf_unregister_driver(&pci_rainy_ep_driver);
	return;
}
module_exit(pci_rainy_ep_drv_exit);

MODULE_DESCRIPTION("PCI RAINY FUNC DRIVER");
MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_LICENSE("GPL v2");
