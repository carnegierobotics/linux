// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ambarella Excalibur endpoint function pci EP-side driver.
 *
 * History: 2022/03/10 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by Ambarella, Inc.
 *
 * TODO:
 * 1. use correct lock to handle all ops(EP|RC OB|IB) and more than one Endpoint SoCs
 *    after TW ships new CV5 bub.
 * 2. add size check when ep ob/ib
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
#include <soc/ambarella/excalibur.h>
#include <soc/ambarella/epf-core.h>
#include <linux/genalloc.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/crc32.h>

excalibur_module_parameters;

#define EXCALIBUR_DRIVER_NAME "pci_epf_excalibur"
#define MSG_ACK_FLAG 0x5555AAAA

struct gen_pool *excalibur_ep_get_pool(void)
{
	struct device *dev;

	dev = &excalibur_ep->epf->epc->dev;

	if (!excalibur_ep) {
		dev_err(dev,
			"%s, failed to get correct excalibur_ep, did your forget to init excalibur endpoint function?\n",
			__func__);
		return NULL;
	}
	return gen_pool_get(dev, NULL);
}
EXPORT_SYMBOL(excalibur_ep_get_pool);

static void excalibur_eeo_calc_checksum(void *ep_buffer, u32 trans_size,
					bool dump)
{
	struct excalibur_msg __iomem *msginfo =
		excalibur_ep->bar[excalibur_ep->reg_bar];

	if (dump)
		print_hex_dump(KERN_INFO, "EP outbound uDMA: ep buffer ",
			       DUMP_PREFIX_OFFSET, 16, 1, ep_buffer, trans_size,
			       false);

	writel(crc32_le(~0, ep_buffer, trans_size),
	       &msginfo->ep_buffer_checksum);
	if (!silence_checksum)
		pr_info("%s: ep checksum is %x\n", __func__,
			readl(&msginfo->ep_buffer_checksum));
}

static void excalibur_ep_set_xfer_size(u32 size);
int excalibur_eeo_wait_for_rc_buffer_ready(void *ep_buffer, u32 size)
{
	struct excalibur_msg __iomem *msginfo;
	int ret;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];

	if (enable_checksum)
		excalibur_eeo_calc_checksum(ep_buffer, size, dump_buffer);

	excalibur_ep_set_xfer_size(size);

	/* MOVE poll to front of this function */
	excalibur_readl_poll_timeout(ep_ob_query_rc_size_enough,
					    ep_ob_query_rc_size_enough ==
						    EP_OB_NOT_PREPARE_YET);
	writel(EP_OB_QUERYING_SIZE, &msginfo->ep_ob_query_rc_size_enough);
	wmb();

	excalibur_readl_poll_timeout(
		ep_ob_query_rc_size_enough,
		ep_ob_query_rc_size_enough == EP_OB_RC_SIZE_BIG_ENOUGH ||
			ep_ob_query_rc_size_enough ==
				EP_OB_RC_SIZE_NOT_BIG_ENOUGH);

	if (readl(&msginfo->ep_ob_query_rc_size_enough) ==
	    EP_OB_RC_SIZE_NOT_BIG_ENOUGH) {
		pr_err("%s %d, RC has no enough space in its pool\n", __func__,
		       __LINE__);
		return -ENOMEM;
	}

	excalibur_readl_poll_timeout(ep_ob_rc_buffer_ready,
					    ep_ob_rc_buffer_ready ==
						    EP_OB_RC_BUFFER_IS_READY);

	return 0;
}
EXPORT_SYMBOL(excalibur_eeo_wait_for_rc_buffer_ready);

int excalibur_eei_wait_for_rc_size_and_buffer_ready(u32 *size)
{
	struct excalibur_msg __iomem *msginfo;
	int ret;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];

	excalibur_readl_poll_timeout(
		ep_ib_query_rc_size_and_src,
		ep_ib_query_rc_size_and_src ==
			EP_IB_QUERY_RC_SIZE_AND_SRC_DONE);

	*size = excalibur_ep_get_xfer_size();
	return 0;
}
EXPORT_SYMBOL(excalibur_eei_wait_for_rc_size_and_buffer_ready);

static void excalibur_ep_set_xfer_size(u32 size)
{
	struct excalibur_msg __iomem *msginfo;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];
	writel(size, &msginfo->trans_size);
}

u32 excalibur_ep_get_xfer_size(void)
{
	struct excalibur_msg __iomem *msginfo;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];
	return readl(&msginfo->trans_size);
}
EXPORT_SYMBOL(excalibur_ep_get_xfer_size);

int excalibur_ero_wait_rc_query_size(void)
{
	struct excalibur_msg __iomem *msginfo;
	int ret;
	u32 bytes_needed;
	struct gen_pool *pool = excalibur_ep_get_pool();

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];
	if (!msginfo) {
		pr_err("%s msginfo(reg) bar is empty, pls check your reg bar\n",
		       __func__);
		return -1;
	}
	mutex_lock(&excalibur_ep->ero_mutex);

	excalibur_readl_poll_timeout(rc_ob_xfer_in_progress,
					    rc_ob_xfer_in_progress ==
						    RC_OB_XFER_NOT_IN_PROGRESS);

	writel(RC_OB_XFER_IN_PROGRESS, &msginfo->rc_ob_xfer_in_progress);

	excalibur_readl_poll_timeout(rc_ob_query_ep_size_enough,
					    rc_ob_query_ep_size_enough ==
						    RC_OB_QUERYING_SIZE);

	bytes_needed = readl(&msginfo->trans_size);
	if (gen_pool_avail(pool) < bytes_needed) {
		writel(RC_OB_EP_SIZE_NOT_BIG_ENOUGH,
		       &msginfo->rc_ob_query_ep_size_enough);
		pr_err("%s %d bytes_needed is %x, but we only have %zx left",
		       __func__, __LINE__, bytes_needed, gen_pool_avail(pool));
		return -ENOMEM;
	}

	writel(RC_OB_EP_SIZE_BIG_ENOUGH, &msginfo->rc_ob_query_ep_size_enough);
	return 0;
}
EXPORT_SYMBOL(excalibur_ero_wait_rc_query_size);

static int excalibur_pci_ep_ob(dma_addr_t dma_dst, dma_addr_t dma_src,
			       u32 total_len);

int excalibur_ep_ob(dma_addr_t src_addr, u32 size)
{
	struct excalibur_msg __iomem *msginfo;
	dma_addr_t rc_addr;
	int ret;
	struct timespec64 start, end;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];

	rc_addr = (dma_addr_t)(readl(&msginfo->rc_dma_upper_addr)) << 32 |
		  readl(&msginfo->rc_dma_addr);
	pr_debug("%s: rc addr ranges from %llx to %llx\n", __func__, rc_addr,
		 rc_addr + size - 1);
	ktime_get_ts64(&start);
	ret = excalibur_pci_ep_ob(rc_addr, src_addr, size);
	if (ret) {
		pr_err("%s %d failed, ret is %x", __func__, __LINE__, ret);
		return ret;
	}
	excalibur_readl_poll_timeout(ep_ob_xfer_in_progress,
					    ep_ob_xfer_in_progress ==
						    EP_OB_XFER_NOT_IN_PROGRESS);
	ktime_get_ts64(&end);
	if (calc_rate)
		ambarella_epf_print_rate("EEO", msginfo->trans_size, &start,
					 &end);
	return ret;
}
EXPORT_SYMBOL(excalibur_ep_ob);

static int excalibur_pci_ep_ib(dma_addr_t dma_dst, dma_addr_t dma_src,
			       u32 total_len);
static void excalibur_eei_compare_checksum(void *ep_buffer, u32 xfer_size,
					   bool dump,
					   bool panic_if_checksum_mismatch);

int excalibur_ep_ib(void *ep_buffer, u32 size)
{
	struct excalibur_msg __iomem *msginfo;
	dma_addr_t rc_addr;
	int ret;
	struct timespec64 start, end;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];
	rc_addr = (dma_addr_t)readl(&msginfo->rc_dma_upper_addr) << 32 |
		  readl(&msginfo->rc_dma_addr);
	writel(EP_IB_XFER_IN_PROGRESS, &msginfo->ep_ib_xfer_in_progress);
	wmb();
	ktime_get_ts64(&start);
	ret = excalibur_pci_ep_ib(
		gen_pool_virt_to_phys(excalibur_ep->pool,
				      (unsigned long)ep_buffer),
		rc_addr, size);
	if (ret) {
		pr_err("%s %d failed, ret is %x", __func__, __LINE__, ret);
		return ret;
	}

	excalibur_readl_poll_timeout(ep_ib_xfer_in_progress,
					    ep_ib_xfer_in_progress ==
						    EP_IB_XFER_NOT_IN_PROGRESS);
	ktime_get_ts64(&end);

	writel(EP_IB_QUERY_RC_SIZE_AND_SRC_NOT_PREP_YET,
	       &msginfo->ep_ib_query_rc_size_and_src);
	if (enable_checksum)
		excalibur_eei_compare_checksum(ep_buffer,
					       excalibur_ep_get_xfer_size(),
					       dump_buffer,
					       panic_if_checksum_mismatch);
	writel(EEI_DONE, &msginfo->eei_done);
	if (calc_rate)
		ambarella_epf_print_rate("EEI", msginfo->trans_size, &start,
					 &end);
	return ret;
}
EXPORT_SYMBOL(excalibur_ep_ib);

/* RC OB/IB should use EP's pci addr from RC view, so we need pci addr from bar, instead of dma addr from EP view */
static void excalibur_ep_tell_rc_dma_addr(dma_addr_t ep_dma_addr,
					  const enum operation operation)
{
	struct excalibur_msg __iomem *msginfo;
	pci_bus_addr_t offset_addr;

	offset_addr = ep_dma_addr - excalibur_ep->mem_bar_dma_addr;
	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];
	if (operation == RC_OB) {
		writel(lower_32_bits(offset_addr),
		       &msginfo->rc_ob_offset_pci_addr);
		writel(upper_32_bits(offset_addr),
		       &msginfo->rc_ob_offset_pci_upper_addr);
		pr_debug(
			"%s %d, offset_addr is %llx, rc_ob_offset_pci_addr is %x, rc_ob_offset_pci_upper_addr is %x\n",
			__func__, __LINE__, offset_addr,
			msginfo->rc_ob_offset_pci_addr,
			msginfo->rc_ob_offset_pci_upper_addr);
	} else if (operation == RC_IB) {
		writel(lower_32_bits(offset_addr),
		       &msginfo->rc_ib_offset_pci_addr);
		writel(upper_32_bits(offset_addr),
		       &msginfo->rc_ib_offset_pci_upper_addr);
	} else
		pr_err("%s %d, you don't need to tell rc ep's dma addr if operations are not RC_OB or RC_IB\n",
		       __func__, __LINE__);
}

static void excalibur_ero_compare_checksum(void *ep_buffer, u32 xfer_size,
					   bool dump,
					   bool panic_if_checksum_mismatch);

int excalibur_ero_wait_dma_complete(void *ep_buffer)
{
	int ret;

	struct excalibur_msg __iomem *msginfo;
	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];

	/*
	 * EP-side should wait until RC all work get done
	 * then it can unlock mutex.
	 */
	excalibur_readl_poll_timeout(rro_done, rro_done == RRO_DONE);

	if (enable_checksum)
		excalibur_ero_compare_checksum(ep_buffer,
					       excalibur_ep_get_xfer_size(),
					       dump_buffer,
					       panic_if_checksum_mismatch);

	/* Clear msginfo for next xfer */
	writel(RC_OB_NOT_PREPARE_YET, &msginfo->rc_ob_query_ep_size_enough);
	writel(RRO_NOT_DONE, &msginfo->rro_done);
	wmb();
	mutex_unlock(&excalibur_ep->ero_mutex);
	return 0;
}
EXPORT_SYMBOL(excalibur_ero_wait_dma_complete);

int excalibur_ero_prepare(void *ep_buffer)
{
	struct excalibur_msg __iomem *msginfo;
	struct gen_pool *pool = excalibur_ep_get_pool();

	if (!pool)
		return -ENOMEM;
	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];
	excalibur_ep_tell_rc_dma_addr(
		gen_pool_virt_to_phys(pool, (unsigned long)ep_buffer), RC_OB);
	writel(RC_OB_EP_BUFFER_IS_READY, &msginfo->rc_ob_ep_buffer_ready);

	return 0;
}
EXPORT_SYMBOL(excalibur_ero_prepare);

static void excalibur_ero_compare_checksum(void *ep_buffer, u32 xfer_size,
					   bool dump,
					   bool panic_if_checksum_mismatch)
{
	u32 ep_checksum, rc_checksum;
	struct excalibur_msg __iomem *msginfo;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];

	ep_checksum = crc32_le(~0, ep_buffer, xfer_size);
	rc_checksum = readl(&msginfo->rc_buffer_checksum);

	if (!silence_checksum)
		pr_info("ero: ep_checksum %u, rc_checksum %u, checksum is %s, xfer_size is %x\n",
			ep_checksum, rc_checksum,
			rc_checksum == ep_checksum ? "correct" : "incorrect!!!",
			xfer_size);

	if (unlikely(rc_checksum != ep_checksum)) {
		pr_err("***************** %s %d checksum mismatch! rc_checksum is %x, ep_checksum is %x ************************\n",
		       __func__, __LINE__, rc_checksum, ep_checksum);
		if (dump)
			print_hex_dump(KERN_INFO, "EP buffer ",
				       DUMP_PREFIX_OFFSET, 16, 1, ep_buffer,
				       xfer_size, false);
		if (panic_if_checksum_mismatch)
			panic("checksum mismatch");
	}
}

static void excalibur_eri_calc_checksum(void *ep_buffer, u32 size, bool dump)
{
	struct excalibur_msg __iomem *msginfo;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];

	writel(crc32_le(~0, ep_buffer, size), &msginfo->ep_buffer_checksum);
	if (dump)
		print_hex_dump(KERN_INFO, "RC inbound uDMA: ep buffer ",
			       DUMP_PREFIX_OFFSET, 16, 1, ep_buffer, size,
			       false);
	if (!silence_checksum)
		pr_info("%s: ep checksum is %x\n", __func__,
			readl(&msginfo->ep_buffer_checksum));
}

int excalibur_eri_wait_dma_complete(void)
{
	int ret;

	struct excalibur_msg __iomem *msginfo;
	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];

	/*
	 * EP-side should wait until RC all work get done
	 * then it can unlock mutex.
	 */
	excalibur_readl_poll_timeout(rri_done, rri_done == RRI_DONE);

	writel(RRI_NOT_DONE, &msginfo->rri_done);
	mutex_unlock(&excalibur_ep->eri_mutex);
	return 0;
}
EXPORT_SYMBOL(excalibur_eri_wait_dma_complete);

int excalibur_eri_prepare(void *ep_buffer, u32 size)
{
	struct excalibur_msg __iomem *msginfo;
	struct gen_pool *pool = excalibur_ep_get_pool();

	if (!pool)
		return -ENOMEM;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];

	mutex_lock(&excalibur_ep->eri_mutex);
	writel(RC_IB_XFER_IN_PROGRESS, &msginfo->rc_ib_xfer_in_progress);
	if (enable_checksum)
		excalibur_eri_calc_checksum(ep_buffer, size, dump_buffer);
	excalibur_ep_tell_rc_dma_addr(
		gen_pool_virt_to_phys(pool, (unsigned long)ep_buffer), RC_IB);

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];
	writel(size, &msginfo->trans_size);

	writel(RC_IB_QUERY_EP_SIZE_AND_SRC_DONE,
	       &msginfo->rc_ib_query_ep_size_and_src);
	return 0;
}
EXPORT_SYMBOL(excalibur_eri_prepare);

static struct pci_epf_header default_epf_header = {
	.vendorid = PCI_ANY_ID,
	.deviceid = PCI_ANY_ID,
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin = PCI_INTERRUPT_INTA,
};

// TODO: remove global variable.
struct excalibur_ep *excalibur_ep;
EXPORT_SYMBOL(excalibur_ep);

static void excalibur_eeo_dma_callback(void *param)
{
	struct excalibur_msg __iomem *msginfo = param;

	writel(EP_OB_XFER_NOT_IN_PROGRESS, &msginfo->ep_ob_xfer_in_progress);
	writel(EP_OB_RC_BUFFER_NOT_READY, &msginfo->ep_ob_rc_buffer_ready);
	/*
	 * don't set ep_ob_xfer_in_progress to
	 * EP_OB_XFER_NOT_IN_PROGRESS here because
	 * RC is wait for EP_OB_XFER_DONE.
	 */
}

static int excalibur_pci_ep_ob(dma_addr_t dma_dst, dma_addr_t dma_src,
			       u32 total_len)
{
	return ambarella_pci_udma_xfer(&excalibur_ep->epf->dev, dma_dst, dma_src,
				      total_len, DMA_MEM_TO_DEV,
				      excalibur_ep->dma_chan_tx,
				      excalibur_eeo_dma_callback,
				      excalibur_ep->bar[excalibur_ep->reg_bar]);
}

static void excalibur_eei_dma_callback(void *param)
{
	struct excalibur_msg __iomem *msginfo = param;

	writel(EP_IB_XFER_NOT_IN_PROGRESS, &msginfo->ep_ib_xfer_in_progress);
	writel(EP_IB_QUERY_RC_SIZE_AND_SRC_NOT_PREP_YET,
	       &msginfo->ep_ib_query_rc_size_and_src);
	if (!silence_checksum)
		pr_debug("%s: rc_buffer_checksum is %x, caller is %pS\n",
			 __func__, msginfo->rc_buffer_checksum,
			 __builtin_return_address(0));
}

static int excalibur_pci_ep_ib(dma_addr_t dma_dst, dma_addr_t dma_src,
			       u32 total_len)
{
	return ambarella_pci_udma_xfer(&excalibur_ep->epf->dev, dma_dst, dma_src,
				      total_len, DMA_DEV_TO_MEM,
				      excalibur_ep->dma_chan_rx,
				      excalibur_eei_dma_callback,
				      excalibur_ep->bar[excalibur_ep->reg_bar]);
}

static void excalibur_eei_compare_checksum(void *ep_buffer, u32 xfer_size,
					   bool dump,
					   bool panic_if_checksum_mismatch)
{
	u32 ep_checksum, rc_checksum;
	struct excalibur_msg __iomem *msginfo =
		excalibur_ep->bar[excalibur_ep->reg_bar];

	ep_checksum = crc32_le(~0, ep_buffer, xfer_size);
	rc_checksum = readl(&msginfo->rc_buffer_checksum);
	if (!silence_checksum)
		pr_info("eei: ep_checksum %x, rc_checksum %x, checksum is %s, xfer_size is %x\n",
			ep_checksum, rc_checksum,
			rc_checksum == ep_checksum ?
				"correct" :
				"incorrect!!! What's your EP's SoC version? A1 has bug, pls update it to A2",
			xfer_size);
	if (unlikely(rc_checksum != ep_checksum)) {
		if (dump)
			print_hex_dump(KERN_INFO, "EP buffer ",
				       DUMP_PREFIX_OFFSET, 16, 1, ep_buffer,
				       xfer_size, false);
		if (panic_if_checksum_mismatch)
			panic("checksum mismatch");
	}
}

static int pci_excalibur_ep_init_dma(struct excalibur_ep *excalibur_ep)
{
	struct device *dev = &excalibur_ep->epf->epc->dev;
	struct gen_pool *pool;
	int ret;

	pool = devm_gen_pool_create(dev, 0, dev_to_node(dev), NULL);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		dev_err(dev, "failed to init pool\n");
	}
	/*  add mem bar to pool.  */
	ret = gen_pool_add_virt(pool, (unsigned long)excalibur_ep->mem_bar_base,
				excalibur_ep->mem_bar_dma_addr,
				excalibur_ep->mem_bar_size, dev_to_node(dev));
	if (ret < 0) {
		dev_err(dev, "%s %d failed to add virt to pool\n", __func__,
			__LINE__);
		return -ENOMEM;
	}

	excalibur_ep->dma_chan_tx =
		ambarella_acquire_udma_chan(DMA_MEM_TO_DEV, dev);
	if (!excalibur_ep->dma_chan_tx)
		return -ENODEV;
	excalibur_ep->dma_chan_rx =
		ambarella_acquire_udma_chan(DMA_DEV_TO_MEM, dev);
	if (!excalibur_ep->dma_chan_rx)
		return -ENODEV;

	dev_dbg(dev, "successfully acquire rx and tx uDMA Channels\n");
	excalibur_ep->pool = pool;
	return 0;
}

static void pci_excalibur_ep_cleanup_dma(struct excalibur_ep *excalibur_ep)
{
	// TODO: cleanup
}

static int pci_excalibur_ep_set_bar(struct pci_epf *epf)
{
	int bar, add, ret;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct excalibur_ep *excalibur_ep = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;

	epc_features = excalibur_ep->epc_features;

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
			pci_epf_free_space(epf, excalibur_ep->bar[bar], bar,
					   PRIMARY_INTERFACE);
			dev_err(dev, "Failed to set BAR%d", bar);
		}
	}

	return 0;
}

static int pci_excalibur_ep_core_init(struct pci_epf *epf)
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

	ret = pci_excalibur_ep_set_bar(epf);
	if (ret)
		return ret;

	return 0;
}

static int pci_excalibur_ep_alloc_space(struct pci_epf *epf)
{
	struct excalibur_ep *excalibur_ep = epf_get_drvdata(epf);
	struct device *dev = &epf->dev;
	struct pci_epf_bar *epf_mem_bar;
	size_t reg_bar_size;
	void __iomem *base;
	enum pci_barno reg_bar = excalibur_ep->reg_bar;
	const struct pci_epc_features *epc_features;
	size_t notify_msg_reg_size;
	struct excalibur_msg __iomem *msginfo;

	epc_features = excalibur_ep->epc_features;
	reg_bar_size = ALIGN(sizeof(struct excalibur_msg), 128);

	notify_msg_reg_size = reg_bar_size;

	if (!epc_features->bar_fixed_size[reg_bar]) {
		dev_err(dev, "%s: failed to get reg bar\n", __func__);
		return -ENODEV;
	}

	if (!epc_features->bar_fixed_size[excalibur_ep->mem_bar]) {
		dev_err(dev, "%s: failed to get mem bar\n", __func__);
		return -ENODEV;
	}

	if (notify_msg_reg_size >
	    epc_features->bar_fixed_size[excalibur_ep->reg_bar])
		return -ENOMEM;

	if (notify_msg_reg_size < sizeof(struct excalibur_msg)) {
		pr_err("%s: need more space for excalibur_msg\n", __func__);
		return -ENOMEM;
	}

	notify_msg_reg_size =
		epc_features->bar_fixed_size[excalibur_ep->reg_bar];

	/* Init reg bar */
	base = pci_epf_alloc_space(epf, notify_msg_reg_size, reg_bar,
				   epc_features->align, PRIMARY_INTERFACE);

	if (!base) {
		dev_err(dev, "Failed to allocated register space\n");
		return -ENOMEM;
	}
	excalibur_ep->bar[reg_bar] = base;

	/* Init mem bar */
	epf_mem_bar = &epf->bar[excalibur_ep->mem_bar];
	epf_mem_bar->size = epc_features->bar_fixed_size[excalibur_ep->mem_bar];
	base = pci_epf_alloc_space(epf, epf_mem_bar->size,
				   excalibur_ep->mem_bar, epc_features->align,
				   PRIMARY_INTERFACE);
	if (!base)
		dev_err(dev, "Failed to allocate space for mem BAR %d\n",
			excalibur_ep->mem_bar);

	excalibur_ep->bar[excalibur_ep->mem_bar] = base;
	excalibur_ep->mem_bar_base = epf_mem_bar->addr;
	excalibur_ep->mem_bar_size = epf_mem_bar->size;
	excalibur_ep->mem_bar_dma_addr = epf_mem_bar->phys_addr;
	dev_info(
		dev,
		"%s %d: mem_bar_base is %px, epf_mem_bar->phys_addr is %llx, size is %zx",
		__func__, __LINE__, excalibur_ep->mem_bar_base,
		epf_mem_bar->phys_addr, epf_mem_bar->size);

	// TODO: init other poll variable here.
	msginfo = excalibur_ep->bar[reg_bar];
	writel(EP_OB_NOT_PREPARE_YET, &msginfo->ep_ob_query_rc_size_enough);
	writel(EP_OB_RC_BUFFER_NOT_READY, &msginfo->ep_ob_rc_buffer_ready);
	writel(EP_IB_QUERY_RC_SIZE_AND_SRC_NOT_PREP_YET,
	       &msginfo->ep_ib_query_rc_size_and_src);
	return 0;
}

static void excalibur_reset_message_bar(struct excalibur_msg __iomem *msginfo)
{
	/* should not use memset or pcie-bus corrupted */
	writel(0, &msginfo->trans_size);
	writel(0, &msginfo->rc_buffer_checksum);
	writel(0, &msginfo->ep_buffer_checksum);
}

static int pci_excalibur_ep_drv_bind(struct pci_epf *epf)
{
	int ret;
	struct excalibur_ep *excalibur_ep = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	enum pci_barno reg_bar = EP_MSG_BAR;
	struct pci_epc *epc = epf->epc;
	struct excalibur_msg __iomem *msginfo = NULL;

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

	excalibur_ep->reg_bar = reg_bar;
	excalibur_ep->epc_features = epc_features;

	ret = pci_excalibur_ep_alloc_space(epf);
	if (ret)
		return ret;

	ret = pci_excalibur_ep_core_init(epf);
	if (ret)
		return ret;

	ret = pci_excalibur_ep_init_dma(excalibur_ep);
	if (ret < 0)
		return ret;

	msginfo = excalibur_ep->bar[excalibur_ep->reg_bar];
	excalibur_reset_message_bar(msginfo);
	return 0;
}

static void pci_excalibur_ep_drv_unbind(struct pci_epf *epf)
{
	struct excalibur_ep *excalibur_ep = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar;
	int bar;

	pci_excalibur_ep_cleanup_dma(excalibur_ep);
	pci_epc_stop(epc);
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		epf_bar = &epf->bar[bar];

		if (excalibur_ep->bar[bar]) {
			pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
					  epf_bar);
			pci_epf_free_space(epf, excalibur_ep->bar[bar], bar,
					   PRIMARY_INTERFACE);
		}
	}
}

static const struct pci_epf_device_id pci_excalibur_ep_dev_ids[] = {
	{
		.name = EXCALIBUR_DRIVER_NAME,
	},
	{},
};

static int pci_excalibur_ep_drv_probe(struct pci_epf *epf)
{
	int ret = 0;
	struct device *dev = &epf->dev;

	excalibur_ep = devm_kzalloc(dev, sizeof(*excalibur_ep), GFP_KERNEL);
	if (!excalibur_ep) {
		ret = -ENOMEM;
		goto err_out;
	}

	mutex_init(&excalibur_ep->mutex);
	mutex_init(&excalibur_ep->eri_mutex);
	mutex_init(&excalibur_ep->ero_mutex);

	epf->header = &default_epf_header;
	excalibur_ep->epf = epf;
	excalibur_ep->mem_bar = EP_MEM_BAR;

	epf_set_drvdata(epf, excalibur_ep);

	return ret;

err_out:
	return ret;
}

static struct pci_epf_ops ops = {
	.unbind = pci_excalibur_ep_drv_unbind,
	.bind = pci_excalibur_ep_drv_bind,
	.set_bar = pci_excalibur_ep_set_bar,
};

static struct pci_epf_driver pci_excalibur_ep_driver = {
	.driver.name = EXCALIBUR_DRIVER_NAME,
	.probe = pci_excalibur_ep_drv_probe,
	.id_table = pci_excalibur_ep_dev_ids,
	.ops = &ops,
	.owner = THIS_MODULE,
};

static int __init pci_excalibur_ep_drv_init(void)
{
	int ret;

	ret = pci_epf_register_driver(&pci_excalibur_ep_driver);
	if (ret) {
		pr_err("Failed to register excalibur excalibur driver --> %d",
		       ret);
		return ret;
	}

	pr_info("%s: register excalibur EP driver successfully\n", __func__);

	return 0;
}
module_init(pci_excalibur_ep_drv_init);

static void __exit pci_excalibur_ep_drv_exit(void)
{
	pci_epf_unregister_driver(&pci_excalibur_ep_driver);
}
module_exit(pci_excalibur_ep_drv_exit);

MODULE_DESCRIPTION("PCI EXCALIBUR FUNC DRIVER");
MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_LICENSE("GPL v2");
