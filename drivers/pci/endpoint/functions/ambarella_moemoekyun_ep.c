// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ambarella Moemoekyun endpoint function pci EP-side driver.
 *
 * History: 2022/11/24 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by Ambarella, Inc.
 *
 * Abbrev:
 *
 * ero: rc ob codes run under EP-side kernel
 * eri: rc ib codes run under EP-side kernel
 * eeo: ep ob codes run under EP-side kernel
 * eei: ep ib codes run under EP-side kernel
 * reg bar: bar used to store epf's register, like size, addr and etc.
 * mem bar: bar used for xfer's src/dst buffer.
 */

#include <linux/export.h>
#include <linux/slab.h>
#include <linux/iopoll.h>
#include <asm-generic/errno-base.h>
#include <linux/printk.h>
#include <soc/ambarella/moemoekyun.h>
#include <soc/ambarella/epf-core.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/types.h>

#define MOEMOEKYUN_DRIVER_NAME "pci_epf_moemoekyun"

static struct pci_epf_header default_epf_header = {
	.vendorid = PCI_ANY_ID,
	.deviceid = PCI_ANY_ID,
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin = PCI_INTERRUPT_INTA,
};

// TODO: remove global variable.
struct moemoekyun_ep *moemoekyun_ep;
EXPORT_SYMBOL(moemoekyun_ep);

static void moemoekyun_eeo_dma_callback(void *param)
{
}

/**
 * moemoekyun_get_rc_dma_addr()) - get rc's buffer's dma address
 *
 * Invoke to get rc buffer's dma addr if ep wants to do ob/ib.
 * Note that final dma_addr can be dma_addr + offset, offset
 * should be <= moemoekyun_get_rc_dma_region_size();
 */
dma_addr_t moemoekyun_get_rc_dma_addr(void)
{
	struct moemoekyun_msg __iomem *msginfo;

	msginfo = moemoekyun_ep->bar[moemoekyun_ep->reg_bar];

	return readl(&msginfo->rc_dma_addr) |
	       (dma_addr_t)readl(&msginfo->rc_dma_upper_addr) << 32;
}
EXPORT_SYMBOL(moemoekyun_get_rc_dma_addr);

/**
 * moemoekyun_get_rc_dma_region_size() - get rc's buffer's dma size
 *
 * Invoke to get rc buffer's dma size if ep wants to do ob/ib.
 * Note that final dma_addr can be dma_addr + offset, offset
 * should be <= moemoekyun_get_rc_dma_region_size();
 */
dma_addr_t moemoekyun_get_rc_dma_region_size(void)
{
	struct moemoekyun_msg __iomem *msginfo;

	msginfo = moemoekyun_ep->bar[moemoekyun_ep->reg_bar];

	return msginfo->rc_dma_region_size;
}
EXPORT_SYMBOL(moemoekyun_get_rc_dma_region_size);

int moemoekyun_pci_ep_ob(dma_addr_t dma_dst, dma_addr_t dma_src, u32 total_len)
{
	return ambarella_pci_udma_xfer(
		&moemoekyun_ep->epf->dev, dma_dst, dma_src, total_len,
		DMA_MEM_TO_DEV, moemoekyun_ep->dma_chan_tx,
		moemoekyun_eeo_dma_callback,
		moemoekyun_ep->bar[moemoekyun_ep->reg_bar]);
}
EXPORT_SYMBOL(moemoekyun_pci_ep_ob);

static void moemoekyun_eei_dma_callback(void *param)
{
}

int moemoekyun_pci_ep_ib(dma_addr_t dma_dst, dma_addr_t dma_src, u32 total_len)
{
	return ambarella_pci_udma_xfer(
		&moemoekyun_ep->epf->dev, dma_dst, dma_src, total_len,
		DMA_DEV_TO_MEM, moemoekyun_ep->dma_chan_rx,
		moemoekyun_eei_dma_callback,
		moemoekyun_ep->bar[moemoekyun_ep->reg_bar]);
}
EXPORT_SYMBOL(moemoekyun_pci_ep_ib);

static int pci_moemoekyun_ep_init_dma(struct moemoekyun_ep *moemoekyun_ep)
{
	struct device *dev = &moemoekyun_ep->epf->epc->dev;

	moemoekyun_ep->dma_chan_tx =
		ambarella_acquire_udma_chan(DMA_MEM_TO_DEV, dev);
	if (!moemoekyun_ep->dma_chan_tx)
		return -ENODEV;
	moemoekyun_ep->dma_chan_rx =
		ambarella_acquire_udma_chan(DMA_DEV_TO_MEM, dev);
	if (!moemoekyun_ep->dma_chan_rx)
		return -ENODEV;

	return 0;
}

static void pci_moemoekyun_ep_cleanup_dma(struct moemoekyun_ep *moemoekyun_ep)
{
	// TODO: cleanup
}

static int pci_moemoekyun_ep_set_bar(struct pci_epf *epf)
{
	int bar, add, ret;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct moemoekyun_ep *moemoekyun_ep = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;

	epc_features = moemoekyun_ep->epc_features;

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
			pci_epf_free_space(epf, moemoekyun_ep->bar[bar], bar,
					   PRIMARY_INTERFACE);
			dev_err(dev, "Failed to set BAR%d", bar);
		}
	}

	return 0;
}

static int pci_moemoekyun_ep_core_init(struct pci_epf *epf)
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

	ret = pci_moemoekyun_ep_set_bar(epf);
	if (ret)
		return ret;

	return 0;
}

static int pci_moemoekyun_ep_alloc_space(struct pci_epf *epf)
{
	struct moemoekyun_ep *moemoekyun_ep = epf_get_drvdata(epf);
	struct device *dev = &epf->dev;
	struct pci_epf_bar *epf_mem_bar;
	size_t reg_bar_size;
	void __iomem *base;
	enum pci_barno reg_bar = moemoekyun_ep->reg_bar;
	const struct pci_epc_features *epc_features;
	size_t notify_msg_reg_size;

	epc_features = moemoekyun_ep->epc_features;
	reg_bar_size = ALIGN(sizeof(struct moemoekyun_msg), 128);

	notify_msg_reg_size = reg_bar_size;

	if (!epc_features->bar_fixed_size[reg_bar]) {
		dev_err(dev, "%s: failed to get reg bar\n", __func__);
		return -ENODEV;
	}

	if (!epc_features->bar_fixed_size[moemoekyun_ep->mem_bar]) {
		dev_err(dev, "%s: failed to get mem bar\n", __func__);
		return -ENODEV;
	}

	if (notify_msg_reg_size >
	    epc_features->bar_fixed_size[moemoekyun_ep->reg_bar])
		return -ENOMEM;

	notify_msg_reg_size =
		epc_features->bar_fixed_size[moemoekyun_ep->reg_bar];

	/* Init reg bar */
	base = pci_epf_alloc_space(epf, notify_msg_reg_size, reg_bar,
				   epc_features->align, PRIMARY_INTERFACE);

	if (!base) {
		dev_err(dev, "Failed to allocated register space\n");
		return -ENOMEM;
	}
	moemoekyun_ep->bar[reg_bar] = base;

	/* Init mem bar */
	epf_mem_bar = &epf->bar[moemoekyun_ep->mem_bar];
	epf_mem_bar->size =
		epc_features->bar_fixed_size[moemoekyun_ep->mem_bar];
	base = pci_epf_alloc_space(epf, epf_mem_bar->size,
				   moemoekyun_ep->mem_bar, epc_features->align,
				   PRIMARY_INTERFACE);
	if (!base)
		dev_err(dev, "Failed to allocate space for mem BAR %d\n",
			moemoekyun_ep->mem_bar);

	moemoekyun_ep->bar[moemoekyun_ep->mem_bar] = base;
	moemoekyun_ep->mem_bar_base = epf_mem_bar->addr;
	moemoekyun_ep->mem_bar_size = epf_mem_bar->size;
	moemoekyun_ep->mem_bar_dma_addr = epf_mem_bar->phys_addr;
	dev_info(
		dev,
		"%s %d: mem_bar_base is %px, epf_mem_bar->phys_addr is %llx, size is %zx",
		__func__, __LINE__, moemoekyun_ep->mem_bar_base,
		epf_mem_bar->phys_addr, epf_mem_bar->size);

	return 0;
}

static int pci_moemoekyun_ep_drv_bind(struct pci_epf *epf)
{
	int ret;
	struct moemoekyun_ep *moemoekyun_ep = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	enum pci_barno reg_bar = EP_MSG_BAR;
	struct pci_epc *epc = epf->epc;
	struct moemoekyun_msg __iomem *msginfo = NULL;

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (epc_features)
		ambarella_ep_configure_bar(epf, epc_features);
	else {
		dev_err(&epf->dev, "%s: failed to get epc_features\n",
			 __func__);
		return -EINVAL;
	}

	moemoekyun_ep->reg_bar = reg_bar;
	moemoekyun_ep->epc_features = epc_features;

	ret = pci_moemoekyun_ep_alloc_space(epf);
	if (ret)
		return ret;

	ret = pci_moemoekyun_ep_core_init(epf);
	if (ret)
		return ret;

	ret = pci_moemoekyun_ep_init_dma(moemoekyun_ep);
	if (ret)
		return ret;

	/* start background work handler */
	msginfo = moemoekyun_ep->bar[moemoekyun_ep->reg_bar];
	return 0;
}

static void pci_moemoekyun_ep_drv_unbind(struct pci_epf *epf)
{
	struct moemoekyun_ep *moemoekyun_ep = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar;
	int bar;

	cancel_delayed_work(&moemoekyun_ep->cmd_handler);
	pci_moemoekyun_ep_cleanup_dma(moemoekyun_ep);
	pci_epc_stop(epc);
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		epf_bar = &epf->bar[bar];

		if (moemoekyun_ep->bar[bar]) {
			pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
					  epf_bar);
			pci_epf_free_space(epf, moemoekyun_ep->bar[bar], bar,
					   PRIMARY_INTERFACE);
		}
	}
}

static const struct pci_epf_device_id pci_moemoekyun_ep_dev_ids[] = {
	{
		.name = MOEMOEKYUN_DRIVER_NAME,
	},
	{},
};

static int pci_moemoekyun_ep_drv_probe(struct pci_epf *epf)
{
	int ret = 0;
	struct device *dev = &epf->dev;

	moemoekyun_ep = devm_kzalloc(dev, sizeof(*moemoekyun_ep), GFP_KERNEL);
	if (!moemoekyun_ep) {
		return -ENOMEM;
	}

	mutex_init(&moemoekyun_ep->mutex);
	mutex_init(&moemoekyun_ep->cmd_mutex);

	epf->header = &default_epf_header;
	moemoekyun_ep->epf = epf;
	moemoekyun_ep->mem_bar = EP_MEM_BAR;

	epf_set_drvdata(epf, moemoekyun_ep);

	/*set MOEMOEKYUN_DRIVER_NAME coherent_mask to utilize cma*/
	dma_set_coherent_mask(&epf->dev, DMA_BIT_MASK(64));

	return ret;
}

static struct pci_epf_ops ops = {
	.unbind = pci_moemoekyun_ep_drv_unbind,
	.bind = pci_moemoekyun_ep_drv_bind,
	.set_bar = pci_moemoekyun_ep_set_bar,
};

static struct pci_epf_driver pci_moemoekyun_ep_driver = {
	.driver.name = MOEMOEKYUN_DRIVER_NAME,
	.probe = pci_moemoekyun_ep_drv_probe,
	.id_table = pci_moemoekyun_ep_dev_ids,
	.ops = &ops,
	.owner = THIS_MODULE,
};

static int __init pci_moemoekyun_ep_drv_init(void)
{
	int ret;

	ret = pci_epf_register_driver(&pci_moemoekyun_ep_driver);
	if (ret) {
		pr_err("Failed to register moemoekyun moemoekyun driver --> %d",
		       ret);
		return ret;
	}

	pr_info("%s: register moemoekyun EP driver successfully\n", __func__);

	return 0;
}
module_init(pci_moemoekyun_ep_drv_init);

static void __exit pci_moemoekyun_ep_drv_exit(void)
{
	pci_epf_unregister_driver(&pci_moemoekyun_ep_driver);
}
module_exit(pci_moemoekyun_ep_drv_exit);

MODULE_DESCRIPTION("PCI MOEMOEKYUN FUNC DRIVER");
MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_LICENSE("GPL v2");
