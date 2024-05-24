// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ambarella Moemoekyun endpoint function pci RC-side driver.
 *
 * History: 2022/11/24 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by Ambarella, Inc.
 *
 * Abbrev:
 *
 * rro: rc ob codes run under RC-side kernel
 * rri: rc ib codes run under RC-side kernel
 * reo: ep ob codes run under RC-side kernel
 * rei: ep ib codes run under RC-side kernel
 */

#include <linux/completion.h>
#include <linux/kernel.h>
#include <soc/ambarella/moemoekyun.h>
#include <linux/of_platform.h>
#include <soc/ambarella/epf-core.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/moduleparam.h>

#define AMBA_EPF_DRV_MODULE_NAME "moemoekyun-rc"
#define MOEMOEKYUN_RC_POLL_SIZE SZ_16M

static int moemoekyun_rc_init_dma(struct moemoekyun_rc *moemoekyun_rc)
{
	struct device *dev = &moemoekyun_rc->pdev->dev;
	int ret;

	moemoekyun_rc->dma_chan_tx =
		ambarella_acquire_udma_chan(DMA_MEM_TO_DEV, dev);
	if (!moemoekyun_rc->dma_chan_tx)
		return -ENODEV;
	moemoekyun_rc->dma_chan_rx =
		ambarella_acquire_udma_chan(DMA_DEV_TO_MEM, dev);
	if (!moemoekyun_rc->dma_chan_rx) {
		ret = -ENODEV;
		goto rx_error;
	}

	moemoekyun_rc->rc_buffer =
		dmam_alloc_coherent(dev, MOEMOEKYUN_RC_POLL_SIZE,
				    &moemoekyun_rc->rc_dma_addr, GFP_KERNEL);
	if (!moemoekyun_rc->rc_buffer) {
		ret = -ENOMEM;
		goto rc_buffer_error;
	}

	return 0;
rc_buffer_error:
	dma_release_channel(moemoekyun_rc->dma_chan_rx);
rx_error:
	dma_release_channel(moemoekyun_rc->dma_chan_tx);
	return ret;
}

static void moemoekyun_rc_cleanup_dma(struct moemoekyun_rc *moemoekyun_rc)
{
	dma_release_channel(moemoekyun_rc->dma_chan_tx);
	dma_release_channel(moemoekyun_rc->dma_chan_rx);
}

static int moemoekyun_rc_probe(struct pci_dev *pdev,
			       const struct pci_device_id *ent)
{
	int err, i;
	struct moemoekyun_msg __iomem *msginfo;
	enum pci_barno bar;
	void __iomem *base;
	struct device *dev = &pdev->dev;
	enum pci_barno test_reg_bar = EP_MSG_BAR;
	struct moemoekyun_rc *moemoekyun_rc, *test_node;

	ambarella_rc_helper_init(MOEMOEKYUN_PCIE_DEVICE_ID);

	dev->parent = ambarella_get_pcie_root_complex(dev);
	dev_info(dev, "parent is %s now\n", dev_name(dev->parent));
	if (pci_is_bridge(pdev))
		return -ENODEV;

	moemoekyun_rc = devm_kzalloc(dev, sizeof(*moemoekyun_rc), GFP_KERNEL);
	if (!moemoekyun_rc)
		return -ENOMEM;

	moemoekyun_rc->pdev = pdev;

	/* set coherent_dma_mask to allocate from cma */
	if ((dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40)) != 0)) {
		dev_err(dev, "Cannot set DMA mask");
		return -EINVAL;
	}

	err = moemoekyun_rc_init_dma(moemoekyun_rc);
	if (err)
		return -err;

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(dev, "Cannot enable PCI device");
		goto err_free_dma;
	}

	err = pci_request_regions(pdev, AMBA_EPF_DRV_MODULE_NAME);
	if (err) {
		dev_err(dev, "Cannot obtain PCI resources");
		goto err_disable_pdev;
	}

	pci_set_master(pdev);

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM) {
			base = pci_ioremap_bar(pdev, bar);
			if (!base) {
				dev_err(dev, "Failed to remap BAR%d", bar);
				WARN_ON(bar == test_reg_bar);
				err = -ENOMEM;
				goto err_free_regions;
			}
			pr_info("%s %d, write to %p, pci_resource_flags(pdev, bar %d) is %lx\n",
				__func__, __LINE__, base, bar,
				pci_resource_flags(pdev, bar));
			moemoekyun_rc->bar[bar] = base;
		}
	}

	msginfo = moemoekyun_rc->bar[test_reg_bar];

	pci_set_drvdata(pdev, moemoekyun_rc);
	test_node = pci_get_drvdata(pdev);
	pci_info(pdev, "%px binding moemoekyun_rc to pdev successfully: %px\n",
		 pdev, test_node);

	return 0;

err_free_regions:
	pci_release_regions(pdev);
	for (i = 0; i < bar && i < PCI_STD_NUM_BARS; i++)
		pci_iounmap(pdev, moemoekyun_rc->bar[i]);
err_disable_pdev:
	pci_release_regions(pdev);
	pci_disable_device(pdev);
err_free_dma:
	moemoekyun_rc_cleanup_dma(moemoekyun_rc);
	return err;
}

static void moemoekyun_rc_remove(struct pci_dev *pdev)
{
	enum pci_barno bar;
	struct moemoekyun_rc *moemoekyun_rc = pci_get_drvdata(pdev);

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (moemoekyun_rc->bar[bar])
			pci_iounmap(pdev, moemoekyun_rc->bar[bar]);
	}

	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id moemoekyun_rc_dev_tbl[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_CDNS, MOEMOEKYUN_PCIE_DEVICE_ID),
	},
	{}
};
MODULE_DEVICE_TABLE(pci, moemoekyun_rc_dev_tbl);

static struct pci_driver moemoekyun_rc_driver = {
	.name = AMBA_EPF_DRV_MODULE_NAME,
	.id_table = moemoekyun_rc_dev_tbl,
	.probe = moemoekyun_rc_probe,
	.remove = moemoekyun_rc_remove,
};
module_pci_driver(moemoekyun_rc_driver);

MODULE_DESCRIPTION("Moemoekyun RC DRIVER for EPF");
MODULE_AUTHOR("lchen@ambarella.com");
MODULE_LICENSE("GPL v2");

static void moemoekyun_rro_dma_callback(void *param)
{
	// TODO
}

static void moemoekyun_rri_dma_callback(void *param)
{
	// TODO
}

int moemoekyun_pci_rc_ib(int ep_index, dma_addr_t dma_dst,
			 dma_addr_t ep_dma_addr, u32 tranlen,
			 struct moemoekyun_msg __iomem *msginfo)
{
	struct pci_dev *pdev;
	struct moemoekyun_rc *moemoekyun_rc;

	pdev = endpoints_info->pdev[ep_index];
	moemoekyun_rc = dev_get_drvdata(&pdev->dev);
	msginfo = endpoints_info->msginfo[ep_index];

	dev_dbg(&pdev->dev, "%s: ep_dma_addr is %llx\n", __func__, ep_dma_addr);

	return ambarella_pci_udma_xfer(&moemoekyun_rc->pdev->dev, dma_dst,
				      ep_dma_addr, tranlen, DMA_DEV_TO_MEM,
				      moemoekyun_rc->dma_chan_rx,
				      moemoekyun_rri_dma_callback, msginfo);
}
EXPORT_SYMBOL(moemoekyun_pci_rc_ib);

int moemoekyun_pci_rc_ob(int ep_index, dma_addr_t ep_dma_addr,
			 dma_addr_t dma_src, u32 tranlen,
			 struct moemoekyun_msg __iomem *msginfo)
{
	struct pci_dev *pdev;
	struct moemoekyun_rc *moemoekyun_rc;

	pdev = endpoints_info->pdev[ep_index];
	moemoekyun_rc = dev_get_drvdata(&pdev->dev);
	msginfo = endpoints_info->msginfo[ep_index];

	dev_dbg(&pdev->dev, "%s: ep_dma_addr is %llx\n", __func__, ep_dma_addr);

	return ambarella_pci_udma_xfer(&moemoekyun_rc->pdev->dev, ep_dma_addr,
				      dma_src, tranlen, DMA_MEM_TO_DEV,
				      moemoekyun_rc->dma_chan_tx,
				      moemoekyun_rro_dma_callback, msginfo);
}
EXPORT_SYMBOL(moemoekyun_pci_rc_ob);

/*
 * Used by EP ob/ib
 */
void moemoekyun_rc_tell_ep_dma_range(int index, dma_addr_t rc_dma_addr,
				     u32 size)
{
	struct moemoekyun_msg __iomem *msginfo;

	pr_debug("%s %d rc_dma_addr is %llx", __func__, __LINE__, rc_dma_addr);
	msginfo = endpoints_info->msginfo[index];
	writel(lower_32_bits(rc_dma_addr), &msginfo->rc_dma_addr);
	writel(upper_32_bits(rc_dma_addr), &msginfo->rc_dma_upper_addr);
	writel(upper_32_bits(size), &msginfo->rc_dma_region_size);
}
EXPORT_SYMBOL(moemoekyun_rc_tell_ep_dma_range);
