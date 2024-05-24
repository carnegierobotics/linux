// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ambarella Excalibur endpoint function pci RC-side driver.
 *
 * History: 2022/03/10 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by Ambarella, Inc.
 *
 * TODO:
 * 1. use correct lock to handle all ops(EP|RC OB|IB) and more than one Endpoint SoCs
 *    after TW ships new CV5 bub.
 * 2. add size check when rc ob/ib
 *
 * Abbrev:
 *
 * rro: rc ob codes run under RC-side kernel
 * rri: rc ib codes run under RC-side kernel
 * reo: ep ob codes run under RC-side kernel
 * rei: ep ib codes run under RC-side kernel
 * reg bar: bar used to store epf's register, like size, addr and etc.
 * mem bar: bar used for xfer's src/dst buffer.
 */

#include <linux/completion.h>
#include <linux/kernel.h>
#include <soc/ambarella/excalibur.h>
#include <linux/of_platform.h>
#include <soc/ambarella/epf-core.h>
#include <linux/genalloc.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/crc32.h>
#include <linux/moduleparam.h>

excalibur_module_parameters;

#define AMBA_EPF_DRV_MODULE_NAME "excalibur-rc"
#define EXCALIBUR_RC_POOL_SIZE SZ_4M

static int excalibur_rc_init_dma_and_genpool(struct excalibur_rc *excalibur_rc)
{
	struct device *dev = &excalibur_rc->pdev->dev;
	struct gen_pool *pool;
	int ret;

	/* TODO: Currently we only provide single gen_pool for each EP, so let's set name to NULL */
	pool = devm_gen_pool_create(dev, 0, dev_to_node(dev), NULL);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		dev_err(dev, "failed to init pool\n");
	}

	excalibur_rc->dma_chan_tx =
		ambarella_acquire_udma_chan(DMA_MEM_TO_DEV, dev);
	if (!excalibur_rc->dma_chan_tx)
		return -ENODEV;
	excalibur_rc->dma_chan_rx =
		ambarella_acquire_udma_chan(DMA_DEV_TO_MEM, dev);
	if (!excalibur_rc->dma_chan_rx) {
		ret = -ENODEV;
		goto rx_error;
	}

	excalibur_rc->rc_buffer =
		dmam_alloc_coherent(dev, EXCALIBUR_RC_POOL_SIZE,
				    &excalibur_rc->rc_dma_addr, GFP_KERNEL);
	if (!excalibur_rc->rc_buffer) {
		ret = -ENOMEM;
		goto rc_buffer_error;
	}
	ret = gen_pool_add_virt(pool, (unsigned long)excalibur_rc->rc_buffer,
				excalibur_rc->rc_dma_addr,
				EXCALIBUR_RC_POOL_SIZE, dev_to_node(dev));
	if (ret < 0) {
		dev_err(dev, "%s failed to add virt to pool\n", __func__);
		ret = -ENOMEM;
		goto rx_error;
	}

	dev_dbg(dev, "rc pool dma addr ranges from %llx to %llx\n",
		excalibur_rc->rc_dma_addr,
		excalibur_rc->rc_dma_addr + EXCALIBUR_RC_POOL_SIZE);
	return 0;
rc_buffer_error:
	dma_release_channel(excalibur_rc->dma_chan_rx);
rx_error:
	dma_release_channel(excalibur_rc->dma_chan_tx);
	return ret;
}

static void excalibur_rc_cleanup_dma(struct excalibur_rc *excalibur_rc)
{
	dma_release_channel(excalibur_rc->dma_chan_tx);
	dma_release_channel(excalibur_rc->dma_chan_rx);
}

static int excalibur_rc_probe(struct pci_dev *pdev,
			      const struct pci_device_id *ent)
{
	int err, i;
	struct excalibur_msg __iomem *msginfo;
	enum pci_barno bar;
	void __iomem *base;
	struct device *dev = &pdev->dev;
	enum pci_barno test_reg_bar = EP_MSG_BAR;
	struct excalibur_rc *excalibur_rc, *test_node;

	ambarella_rc_helper_init(EXCALIBUR_PCIE_DEVICE_ID);

	dev->parent = ambarella_get_pcie_root_complex(dev);
	dev_info(dev, "parent is %s now\n", dev_name(dev->parent));
	if (pci_is_bridge(pdev))
		return -ENODEV;

	excalibur_rc = devm_kzalloc(dev, sizeof(*excalibur_rc), GFP_KERNEL);
	if (!excalibur_rc)
		return -ENOMEM;

	excalibur_rc->pdev = pdev;

	/* set coherent_dma_mask to allocate from cma */
	if ((dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40)) != 0)) {
		dev_err(dev, "Cannot set DMA mask");
		return -EINVAL;
	}

	err = excalibur_rc_init_dma_and_genpool(excalibur_rc);
	if (err)
		return -err;

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(dev, "Cannot enable PCI device");
		goto err_free_dma;
	}
	mutex_init(&excalibur_rc->rei_mutex);
	mutex_init(&excalibur_rc->reo_mutex);

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
			excalibur_rc->bar[bar] = base;
		}
	}

	msginfo = excalibur_rc->bar[test_reg_bar];
	writel(RC_OB_XFER_NOT_IN_PROGRESS, &msginfo->rc_ob_xfer_in_progress);
	writel(RC_OB_NOT_PREPARE_YET, &msginfo->rc_ob_query_ep_size_enough);
	writel(RC_OB_EP_BUFFER_NOT_READY, &msginfo->rc_ob_ep_buffer_ready);

	writel(RC_IB_QUERY_EP_SIZE_AND_SRC_NOT_PREP_YET,
	       &msginfo->rc_ib_query_ep_size_and_src);
	writel(RC_IB_XFER_NOT_IN_PROGRESS, &msginfo->rc_ib_xfer_in_progress);

	writel(EP_OB_XFER_NOT_IN_PROGRESS, &msginfo->ep_ob_xfer_in_progress);
	writel(EP_OB_NOT_PREPARE_YET, &msginfo->ep_ob_query_rc_size_enough);
	writel(EP_OB_RC_BUFFER_NOT_READY, &msginfo->ep_ob_rc_buffer_ready);

	writel(EP_IB_XFER_NOT_IN_PROGRESS, &msginfo->ep_ib_xfer_in_progress);

	pci_set_drvdata(pdev, excalibur_rc);
	test_node = pci_get_drvdata(pdev);
	pci_info(pdev, "%px binding excalibur_rc to pdev successfully: %px\n",
		 pdev, test_node);

	return 0;

err_free_regions:
	pci_release_regions(pdev);
	for (i = 0; i < bar && i < PCI_STD_NUM_BARS; i++)
		pci_iounmap(pdev, excalibur_rc->bar[i]);
err_disable_pdev:
	pci_disable_device(pdev);

err_free_dma:
	excalibur_rc_cleanup_dma(excalibur_rc);
	return err;
}

static void excalibur_rc_remove(struct pci_dev *pdev)
{
	enum pci_barno bar;
	struct excalibur_rc *excalibur_rc = pci_get_drvdata(pdev);

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (excalibur_rc->bar[bar])
			pci_iounmap(pdev, excalibur_rc->bar[bar]);
	}

	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id excalibur_rc_dev_tbl[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_CDNS, EXCALIBUR_PCIE_DEVICE_ID),
	},
	{}
};
MODULE_DEVICE_TABLE(pci, excalibur_rc_dev_tbl);

static struct pci_driver excalibur_rc_driver = {
	.name = AMBA_EPF_DRV_MODULE_NAME,
	.id_table = excalibur_rc_dev_tbl,
	.probe = excalibur_rc_probe,
	.remove = excalibur_rc_remove,
};
module_pci_driver(excalibur_rc_driver);

MODULE_DESCRIPTION("Excalibur RC DRIVER for EPF");
MODULE_AUTHOR("lchen@ambarella.com");
MODULE_LICENSE("GPL v2");

static void excalibur_rro_dma_callback(void *param)
{
	struct excalibur_msg __iomem *msginfo = param;

	writel(RC_OB_XFER_NOT_IN_PROGRESS, &msginfo->rc_ob_xfer_in_progress);
	writel(RC_OB_EP_BUFFER_NOT_READY, &msginfo->rc_ob_ep_buffer_ready);
}

int excalibur_pci_rc_ob(struct excalibur_rc *excalibur_rc, dma_addr_t dma_dst,
			dma_addr_t dma_src, u32 tranlen,
			struct excalibur_msg __iomem *msginfo)
{
	return ambarella_pci_udma_xfer(&excalibur_rc->pdev->dev, dma_dst,
				      dma_src, tranlen, DMA_MEM_TO_DEV,
				      excalibur_rc->dma_chan_tx,
				      excalibur_rro_dma_callback, msginfo);
}

static void excalibur_rri_dma_callback(void *param)
{
	struct excalibur_msg __iomem *msginfo = param;

	writel(RC_IB_XFER_NOT_IN_PROGRESS, &msginfo->rc_ib_xfer_in_progress);
	writel(RC_IB_QUERY_EP_SIZE_AND_SRC_NOT_PREP_YET,
	       &msginfo->ep_ib_query_rc_size_and_src);
	if (enable_checksum && !silence_checksum)
		pr_info("%s: rc_buffer_checksum is %x, caller is %pS\n",
			__func__, msginfo->rc_buffer_checksum,
			__builtin_return_address(0));
}

static int excalibur_pci_rc_ib(struct excalibur_rc *excalibur_rc,
			       dma_addr_t dma_dst, dma_addr_t dma_src,
			       u32 tranlen,
			       struct excalibur_msg __iomem *msginfo)
{
	pr_info("dst is %llx, src is %llx", dma_dst, dma_src);
	return ambarella_pci_udma_xfer(&excalibur_rc->pdev->dev, dma_dst,
				      dma_src, tranlen, DMA_DEV_TO_MEM,
				      excalibur_rc->dma_chan_rx,
				      excalibur_rri_dma_callback, msginfo);
}

int excalibur_rri_wait_for_ep_size_and_buffer_ready(int index, u32 *size)
{
	struct pci_dev *pdev;
	struct excalibur_rc *excalibur_rc;
	int ret;
	struct excalibur_msg __iomem *msginfo;

	msginfo = endpoints_info->msginfo[index];

	if (unlikely(index >= endpoints_info->ep_num)) {
		pr_err("get wrong ep, ep_id %d is too large, we only have %zu EP\n",
		       index, endpoints_info->ep_num);
		return -1;
	}

	pdev = endpoints_info->pdev[index];
	excalibur_rc = dev_get_drvdata(&pdev->dev);

	excalibur_readl_poll_timeout(
		rc_ib_query_ep_size_and_src,
		rc_ib_query_ep_size_and_src ==
			RC_IB_QUERY_EP_SIZE_AND_SRC_DONE);

	*size = excalibur_rc_get_xfer_size(index);

	return 0;
}
EXPORT_SYMBOL(excalibur_rri_wait_for_ep_size_and_buffer_ready);

struct gen_pool *excalibur_rc_get_pool(int index)
{
	struct pci_dev *pdev;

	if (unlikely(index >= endpoints_info->ep_num)) {
		pr_err("get wrong ep, ep_id %d is too large, we only have %zu EP\n",
		       index, endpoints_info->ep_num);
		return NULL;
	}

	pdev = endpoints_info->pdev[index];
	return gen_pool_get(&pdev->dev, NULL);
}
EXPORT_SYMBOL(excalibur_rc_get_pool);

static void excalibur_rei_calc_checksum(int index, void *rc_buffer, u32 size,
					bool dump)
{
	struct excalibur_msg __iomem *msginfo;
	u32 checksum;
	checksum = crc32_le(~0, rc_buffer, size);

	msginfo = endpoints_info->msginfo[index];
	writel(checksum, &msginfo->rc_buffer_checksum);
	if (dump) {
		pr_info("%s rc checksum is %x, size is %x\n", __func__,
			checksum, size);
		print_hex_dump(KERN_INFO, "rei: rc buffer ", DUMP_PREFIX_OFFSET,
			       16, 1, rc_buffer, size, false);
	}
}

void excalibur_rc_set_xfer_size(int index, u32 size)
{
	struct excalibur_msg __iomem *msginfo;

	msginfo = endpoints_info->msginfo[index];

	if (unlikely(index >= endpoints_info->ep_num)) {
		pr_err("get wrong ep, ep_id %d is too large, we only have %zu EP\n",
		       index, endpoints_info->ep_num);
		return;
	}

	writel(size, &msginfo->trans_size);
}
EXPORT_SYMBOL(excalibur_rc_set_xfer_size);

/*
 * In case user doesn't setup EP correctly, e.g.
 * boot up RC before EPF init so RC cannot find
 * the EP.
 */
int excalibur_rc_check_ep(int index)
{
	struct excalibur_msg __iomem *msginfo;
	struct pci_dev *pdev;

	pdev = endpoints_info->pdev[index];
	msginfo = endpoints_info->msginfo[index];

	if (!pdev || !msginfo) {
		pr_err("%s: failed to get pdev or msginfo,"
		       "please make sure your EP works correctly\n",
		       __func__);
		return -ENODEV;
	}
	return 0;
}
EXPORT_SYMBOL(excalibur_rc_check_ep);

static void excalibur_rc_compare_checksum(int index, void *rc_buffer, u32 size,
					  enum operation operation, bool dump,
					  bool panic_if_checksum_mismatch)
{
	struct excalibur_msg __iomem *msginfo;
	u32 rc_checksum, ep_checksum;
	struct pci_dev *pdev;

	msginfo = endpoints_info->msginfo[index];
	pdev = endpoints_info->pdev[index];
	ep_checksum = readl(&msginfo->ep_buffer_checksum);
	rc_checksum = crc32_le(~0, rc_buffer, size);

	if (!silence_checksum)
		pci_info(
			pdev,
			"ep_checksum %x, rc_checksum %x, checksum is %s, xfer_size is %x, operation is %x\n",
			ep_checksum, rc_checksum,
			rc_checksum == ep_checksum ? "correct" : "incorrect!!!",
			size, operation);
	if (unlikely(rc_checksum != ep_checksum)) {
		if (dump)
			print_hex_dump(KERN_INFO, "RC buffer ",
				       DUMP_PREFIX_OFFSET, 16, 1, rc_buffer,
				       size, false);
		if (panic_if_checksum_mismatch)
			panic("checksum mismatch");
		else
			pci_err(pdev,
				"**************** checksum mismatch ****************\n");
	}
}

u32 excalibur_rc_get_xfer_size(int index)
{
	struct excalibur_msg __iomem *msginfo;

	msginfo = endpoints_info->msginfo[index];
	return readl(&msginfo->trans_size);
}
EXPORT_SYMBOL(excalibur_rc_get_xfer_size);

int excalibur_reo_wait_ep_query_size(int index)
{
	struct pci_dev *pdev;
	struct excalibur_rc *excalibur_rc;
	struct excalibur_msg __iomem *msginfo;
	int ret;
	u32 bytes_needed;
	struct gen_pool *pool;

	msginfo = endpoints_info->msginfo[index];
	pdev = endpoints_info->pdev[index];
	excalibur_rc = dev_get_drvdata(&pdev->dev);
	pool = gen_pool_get(&pdev->dev, NULL);

	mutex_lock(&excalibur_rc->reo_mutex);
	excalibur_readl_poll_timeout(ep_ob_xfer_in_progress,
					    ep_ob_xfer_in_progress ==
						    EP_OB_XFER_NOT_IN_PROGRESS);

	writel(EP_OB_XFER_IN_PROGRESS, &msginfo->ep_ob_xfer_in_progress);
	excalibur_readl_poll_timeout(ep_ob_query_rc_size_enough,
					    ep_ob_query_rc_size_enough ==
						    EP_OB_QUERYING_SIZE);

	bytes_needed = readl(&msginfo->trans_size);
	if (gen_pool_avail(pool) < bytes_needed) {
		writel(EP_OB_RC_SIZE_NOT_BIG_ENOUGH,
		       &msginfo->ep_ob_query_rc_size_enough);
		pr_err("%s %d bytes_needed is %x, but we only have %zx left",
		       __func__, __LINE__, bytes_needed, gen_pool_avail(pool));
		return -ENOMEM;
	}

	writel(EP_OB_RC_SIZE_BIG_ENOUGH, &msginfo->ep_ob_query_rc_size_enough);

	return 0;
}
EXPORT_SYMBOL(excalibur_reo_wait_ep_query_size);

/* EP OB/IB should use RC's pci addr, but RC inbound address translation use 1:1 translate pci addr to axi/cpu addr, so we don't need do any cast*/
static void excalibur_rc_tell_ep_dma_addr(int index, dma_addr_t rc_dma_addr)
{
	struct excalibur_msg __iomem *msginfo;

	pr_debug("%s %d rc_dma_addr is %llx", __func__, __LINE__, rc_dma_addr);
	msginfo = endpoints_info->msginfo[index];
	writel(lower_32_bits(rc_dma_addr), &msginfo->rc_dma_addr);
	writel(upper_32_bits(rc_dma_addr), &msginfo->rc_dma_upper_addr);
}

int excalibur_reo_wait_dma_complete(int index, void *rc_buffer)
{
	int ret;
	struct excalibur_msg __iomem *msginfo;
	struct pci_dev *pdev;
	struct excalibur_rc *excalibur_rc;

	pdev = endpoints_info->pdev[index];
	excalibur_rc = dev_get_drvdata(&pdev->dev);
	msginfo = endpoints_info->msginfo[index];

	excalibur_readl_poll_timeout(ep_ob_xfer_in_progress,
					    ep_ob_xfer_in_progress ==
						    EP_OB_XFER_NOT_IN_PROGRESS);

	if (enable_checksum)
		excalibur_rc_compare_checksum(index, rc_buffer,
					      excalibur_rc_get_xfer_size(index),
					      EP_OB, dump_buffer,
					      panic_if_checksum_mismatch);
	// TODO: add EEO_DONE and wait it ep done?
	writel(EP_OB_NOT_PREPARE_YET, &msginfo->ep_ob_query_rc_size_enough);
	wmb();
	mutex_unlock(&excalibur_rc->reo_mutex);
	return 0;
}
EXPORT_SYMBOL(excalibur_reo_wait_dma_complete);

int excalibur_rei_wait_dma_complete(int index)
{
	int ret;
	struct excalibur_msg __iomem *msginfo;
	struct pci_dev *pdev;
	struct excalibur_rc *excalibur_rc;

	pdev = endpoints_info->pdev[index];
	excalibur_rc = dev_get_drvdata(&pdev->dev);
	msginfo = endpoints_info->msginfo[index];

	/*
	 * RC-side should wait until EP all work get done
	 * then it can unlock mutex.
	 */
	excalibur_readl_poll_timeout(eei_done, eei_done == EEI_DONE);

	mutex_unlock(&excalibur_rc->rei_mutex);
	/* TODO: move it into lock region? */
	writel(EEI_NOT_DONE, &msginfo->eei_done);
	return 0;
}
EXPORT_SYMBOL(excalibur_rei_wait_dma_complete);

/**
 * excalibur_reo_prepare - RC: EP outbound: prepare
 * RC side: EP outbound:
 */
void excalibur_reo_prepare(int index, void *rc_buffer)
{
	struct excalibur_msg __iomem *msginfo;

	msginfo = endpoints_info->msginfo[index];
	if (unlikely(index >= endpoints_info->ep_num)) {
		pr_err("get wrong ep, ep_id %d is too large, we only have %zu EP\n",
		       index, endpoints_info->ep_num);
		return;
	}
	excalibur_rc_tell_ep_dma_addr(
		index, gen_pool_virt_to_phys(excalibur_rc_get_pool(index),
					     (unsigned long)rc_buffer));

	writel(EP_OB_RC_BUFFER_IS_READY, &msginfo->ep_ob_rc_buffer_ready);
}
EXPORT_SYMBOL(excalibur_reo_prepare);

int excalibur_rei_prepare(int index, void *rc_buffer, u32 size)
{
	struct excalibur_msg __iomem *msginfo;
	struct pci_dev *pdev;
	struct excalibur_rc *excalibur_rc;

	pdev = endpoints_info->pdev[index];
	excalibur_rc = dev_get_drvdata(&pdev->dev);

	msginfo = endpoints_info->msginfo[index];

	if (unlikely(index >= endpoints_info->ep_num)) {
		pr_err("get wrong ep, ep_id %d is too large, we only have %zu EP\n",
		       index, endpoints_info->ep_num);
		return -ENODEV;
	}

	mutex_lock(&excalibur_rc->rei_mutex);
	writel(EP_IB_XFER_IN_PROGRESS, &msginfo->ep_ib_xfer_in_progress);

	if (enable_checksum)
		excalibur_rei_calc_checksum(index, rc_buffer, size,
					    dump_buffer);

	excalibur_rc_tell_ep_dma_addr(
		index, gen_pool_virt_to_phys(excalibur_rc_get_pool(index),
					     (unsigned long)rc_buffer));
	excalibur_rc_set_xfer_size(index, size);

	writel(size, &msginfo->trans_size);

	writel(EP_IB_QUERY_RC_SIZE_AND_SRC_DONE,
	       &msginfo->ep_ib_query_rc_size_and_src);
	return 0;
}
EXPORT_SYMBOL(excalibur_rei_prepare);

static void excalibur_rro_calc_checksum(int index, void *rc_buffer,
					u32 trans_size, bool dump)
{
	struct excalibur_msg __iomem *msginfo;

	msginfo = endpoints_info->msginfo[index];

	if (dump) {
		pr_info("%s %d rc_buffer_checksum is %x\n", __func__, __LINE__,
			msginfo->rc_buffer_checksum);
		print_hex_dump(KERN_INFO, "RC outbound uDMA: rc buffer ",
			       DUMP_PREFIX_OFFSET, 16, 1, rc_buffer, trans_size,
			       false);
	}

	writel(crc32_le(~0, rc_buffer, trans_size),
	       &msginfo->rc_buffer_checksum);
}

int excalibur_rro_wait_for_ep_buffer_ready(int index, void *rc_buffer, u32 size)
{
	struct excalibur_msg __iomem *msginfo;
	struct pci_dev *pdev;
	struct excalibur_rc *excalibur_rc;
	int ret;

	if (unlikely(index >= endpoints_info->ep_num)) {
		pr_err("get wrong ep, ep_id %d is too large, we only have %zu EP\n",
		       index, endpoints_info->ep_num);
		return -ENODEV;
	}

	msginfo = endpoints_info->msginfo[index];
	pdev = endpoints_info->pdev[index];

	excalibur_readl_poll_timeout(rc_ob_query_ep_size_enough,
					    rc_ob_query_ep_size_enough ==
						    RC_OB_NOT_PREPARE_YET);
	writel(RC_OB_QUERYING_SIZE, &msginfo->rc_ob_query_ep_size_enough);

	if (enable_checksum)
		excalibur_rro_calc_checksum(index, rc_buffer, size,
					    dump_buffer);

	excalibur_rc_set_xfer_size(index, size);
	excalibur_rc = dev_get_drvdata(&pdev->dev);

	excalibur_readl_poll_timeout(
		rc_ob_query_ep_size_enough,
		rc_ob_query_ep_size_enough == RC_OB_EP_SIZE_BIG_ENOUGH ||
			rc_ob_query_ep_size_enough ==
				RC_OB_EP_SIZE_NOT_BIG_ENOUGH);

	if (readl(&msginfo->rc_ob_query_ep_size_enough) ==
	    RC_OB_EP_SIZE_NOT_BIG_ENOUGH) {
		pr_err("%s %d, EP has no enough space in its pool\n", __func__,
		       __LINE__);
		return -ENOMEM;
	}

	excalibur_readl_poll_timeout(rc_ob_ep_buffer_ready,
					    rc_ob_ep_buffer_ready ==
						    RC_OB_EP_BUFFER_IS_READY);

	return 0;
}
EXPORT_SYMBOL(excalibur_rro_wait_for_ep_buffer_ready);

int excalibur_rc_ob(int ep_id, dma_addr_t src_addr, u32 size)
{
	struct excalibur_msg __iomem *msginfo;
	pci_bus_addr_t ep_pci_addr, offset_addr;
	int ret;
	struct excalibur_rc *excalibur_rc;
	struct timespec64 start, end;

	if (unlikely(ep_id >= endpoints_info->ep_num)) {
		pr_err("get wrong ep, ep_id %d is too large, we only have %zu EP\n",
		       ep_id, endpoints_info->ep_num);
		return -1;
	}

	msginfo = endpoints_info->msginfo[ep_id];

	excalibur_rc = pci_get_drvdata(endpoints_info->pdev[ep_id]);
	offset_addr =
		((pci_bus_addr_t)readl(&msginfo->rc_ob_offset_pci_upper_addr))
			<< 32 |
		readl(&msginfo->rc_ob_offset_pci_addr);
	ep_pci_addr = endpoints_info->ep_mem_pci_addr[ep_id] + offset_addr;
	ktime_get_ts64(&start);
	ret = excalibur_pci_rc_ob(excalibur_rc, ep_pci_addr, src_addr, size,
				  msginfo);
	if (ret < 0) {
		pr_err("%s %d, xfer failed\n", __func__, __LINE__);
		return ret;
	}

	excalibur_readl_poll_timeout(rc_ob_xfer_in_progress,
					    rc_ob_xfer_in_progress ==
						    RC_OB_XFER_NOT_IN_PROGRESS);

	ktime_get_ts64(&end);
	writel(RRO_DONE, &msginfo->rro_done);

	if (calc_rate)
		ambarella_epf_print_rate("RRO", msginfo->trans_size, &start,
					 &end);
	return ret;
}
EXPORT_SYMBOL(excalibur_rc_ob);

int excalibur_rc_ib(int ep_id, void *rc_buffer, u32 size)
{
	struct excalibur_msg __iomem *msginfo;
	pci_bus_addr_t ep_pci_addr, offset_addr;
	int ret;
	struct excalibur_rc *excalibur_rc;
	struct timespec64 start, end;

	if (unlikely(ep_id >= endpoints_info->ep_num)) {
		pr_err("ep_id(%d) is wrong, we only have %zu EP SoCs\n", ep_id,
		       endpoints_info->ep_num);
		return -1;
	}

	msginfo = endpoints_info->msginfo[ep_id];
	excalibur_rc = pci_get_drvdata(endpoints_info->pdev[ep_id]);
	offset_addr =
		((pci_bus_addr_t)readl(&msginfo->rc_ib_offset_pci_upper_addr))
			<< 32 |
		readl(&msginfo->rc_ib_offset_pci_addr);
	ep_pci_addr = endpoints_info->ep_mem_pci_addr[ep_id] + offset_addr;
	writel(RC_IB_XFER_IN_PROGRESS, &msginfo->rc_ib_xfer_in_progress);
	ktime_get_ts64(&start);
	ret = excalibur_pci_rc_ib(
		excalibur_rc,
		gen_pool_virt_to_phys(excalibur_rc_get_pool(ep_id),
				      (unsigned long)rc_buffer),
		ep_pci_addr, size, msginfo);
	if (ret < 0) {
		pr_err("%s %d, xfer failed\n", __func__, __LINE__);
		return ret;
	}
	excalibur_readl_poll_timeout(rc_ib_xfer_in_progress,
					    rc_ib_xfer_in_progress ==
						    RC_IB_XFER_NOT_IN_PROGRESS);
	ktime_get_ts64(&end);

	if (enable_checksum)
		excalibur_rc_compare_checksum(ep_id, rc_buffer, size, RC_IB,
					      dump_buffer,
					      panic_if_checksum_mismatch);
	writel(RRI_DONE, &msginfo->rri_done);
	// TODO: it has been updated in dma callback, let's remove it.
	writel(RC_IB_QUERY_EP_SIZE_AND_SRC_NOT_PREP_YET,
	       &msginfo->rc_ib_query_ep_size_and_src);
	if (calc_rate)
		ambarella_epf_print_rate("RRI", msginfo->trans_size, &start,
					 &end);
	return ret;
}
EXPORT_SYMBOL(excalibur_rc_ib);
