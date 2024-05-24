// SPDX-License-Identifier: GPL-2.0
/*
 * Test driver to test endpoint functionality
 *
 * Copyright (C) 2017 Texas Instruments
 * Copyright (C) 2023 Ambarella.Inc
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 * Author: Li Chen <lchen@ambarella.com>
 * epf->epc->dev.parent is cdns pcie controller
 * epf->dev is epf device
 */

#include <linux/irqreturn.h>
#include <linux/msi.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci_ids.h>
#include <linux/random.h>
#include <linux/sys_soc.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>
#include <linux/sys_soc.h>
#include <asm-generic/div64.h>
#include <soc/ambarella/misc.h>
#include <soc/ambarella/epf-core.h>

#define MSI_DOORBELL_BAR 4

#define IRQ_TYPE_LEGACY 0
#define IRQ_TYPE_MSI 1
#define IRQ_TYPE_MSIX 2

#define COMMAND_RAISE_LEGACY_IRQ BIT(0)
#define COMMAND_RAISE_MSI_IRQ BIT(1)
#define COMMAND_RAISE_MSIX_IRQ BIT(2)
#define COMMAND_READ BIT(3)
#define COMMAND_WRITE BIT(4)
#define COMMAND_COPY BIT(5)

#define STATUS_READ_SUCCESS BIT(0)
#define STATUS_READ_FAIL BIT(1)
#define STATUS_WRITE_SUCCESS BIT(2)
#define STATUS_WRITE_FAIL BIT(3)
#define STATUS_COPY_SUCCESS BIT(4)
#define STATUS_COPY_FAIL BIT(5)
#define STATUS_IRQ_RAISED BIT(6)
#define STATUS_SRC_ADDR_INVALID BIT(7)
#define STATUS_DST_ADDR_INVALID BIT(8)
#define STATUS_MSI_DOORBELL_SUCCESS BIT(9)

#define FLAG_USE_DMA BIT(0)
#define FLAG_USE_DMA_ALLOC_COHERENT BIT(1)
#define FLAG_USE_VERBOSE_OUTPUT BIT(2)
#define FLAG_USE_MULT_CHAN_FOR_SINGLE_XFER BIT(3)
#define FLAG_SUPPORT_MSI_DOORBELL BIT(5)

static struct workqueue_struct *kpcitest_workqueue;

/* TODO: remove globale_epf!! */
struct pci_epf *global_epf;
enum cdns_dma_mode {
	BULK_MODE = 1,
	SG_MODE = 2,
};

struct pci_epf_amba_test {
	void *reg[PCI_STD_NUM_BARS];
	struct pci_epf *epf;
	enum pci_barno test_reg_bar;
	struct ambarella_msi_doorbell_property msi_doorbell_property;
	size_t msix_table_offset;
	struct delayed_work cmd_handler;
	struct completion transfer_complete;
	const struct pci_epc_features *epc_features;
	int cap;
};

struct pci_epf_amba_test_reg {
	u32 magic;
	u32 command;
	u32 status;
	u64 src_addr;
	u64 dst_addr;
	u32 size;
	u32 checksum;
	u32 irq_type;
	u32 irq_number;
	u32 flags;
	u32 buffer_split_count;
	u32 cdns_dma_mode;
	u32 nr_channels;
	u32 nr_repeated_xfer;
	u32 db_bar;
	u32 db_offset;
	u32 db_data;
} __packed;

static struct pci_epf_header test_header = {
	.vendorid = PCI_ANY_ID,
	.deviceid = PCI_ANY_ID,
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin = PCI_INTERRUPT_INTA,
};

static size_t bar_size[] = { 512, 512, 1024, 16384, 131072, 1048576 };

static void pci_epf_amba_test_raise_irq(struct pci_epf_amba_test *epf_amba_test,
					u8 irq_type, u16 irq);

static enum irqreturn msi_doorbell_interrupt_handler(int irq, void *data)
{
	struct pci_epf *epf = data;
	struct pci_epf_amba_test *epf_amba_test = epf_get_drvdata(epf);
	enum pci_barno test_reg_bar = epf_amba_test->test_reg_bar;
	struct pci_epf_amba_test_reg *reg = epf_amba_test->reg[test_reg_bar];

	reg->status = STATUS_MSI_DOORBELL_SUCCESS;
	/* Tell RC that EP has received doorbell */
	pci_epf_amba_test_raise_irq(epf_amba_test, reg->irq_type,
				    reg->irq_number);

	return IRQ_HANDLED;
}

static void pci_epf_amba_test_dma_callback(void *param)
{
	struct pci_epf_amba_test *epf_amba_test = param;

	complete(&epf_amba_test->transfer_complete);
}

static int m2m_transfer(u32 cdns_dma_mode, enum dma_transfer_direction dir,
			dma_addr_t dma_dst, dma_addr_t dma_src,
			struct dma_async_tx_descriptor *tx,
			struct pci_epf_amba_test *epf_amba_test,
			struct dma_chan *chan, size_t xfer_size,
			enum dma_ctrl_flags flags, struct timespec64 *start,
			struct timespec64 *end, u32 nr_repeated_xfer)

{
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	dma_cookie_t cookie;
	int ret;
	int i;

	ktime_get_ts64(start);
	for (i = 0; i < nr_repeated_xfer; i++) {
		tx = dmaengine_prep_dma_memcpy(chan, dma_dst, dma_src,
					       xfer_size, flags);
		if (!tx) {
			dev_err(dev, "Failed to prepare DMA memcpy\n");
			return -EINVAL;
		}
		/* completing multiple times is still well-behavior, linux completion has ref counter */
		reinit_completion(&epf_amba_test->transfer_complete);
		tx->callback = pci_epf_amba_test_dma_callback;
		tx->callback_param = epf_amba_test;
		cookie = tx->tx_submit(tx);

		ret = dma_submit_error(cookie);
		if (ret) {
			dev_err(dev, "Failed to do DMA tx_submit %d\n", cookie);
			return ret;
		}

		dma_async_issue_pending(chan);
		ret = wait_for_completion_interruptible(
			&epf_amba_test->transfer_complete);
		if (ret) {
			dmaengine_terminate_sync(chan);
			dev_err(dev, "DMA failed: get signaled\n");
			return ret;
		}
	}
	ktime_get_ts64(end);
	return 0;
}

static int cdns_udma_bulk_transfer(
	enum dma_transfer_direction dir, dma_addr_t dma_remote,
	dma_addr_t dma_local, struct pci_epf_amba_test *epf_amba_test,
	u32 buffer_split_count, struct dma_async_tx_descriptor **tx,
	size_t xfer_size, int nr_chans, struct dma_chan **chans,
	enum dma_ctrl_flags flags, struct timespec64 *start,
	struct timespec64 *end, u32 nr_repeated_xfer)
{
	int ret = 0, bulk_trunk_size, i, j, k;
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	dma_cookie_t cookie;
	struct scatterlist *local_sg, *remote_sg;
	size_t per_chan_xfer_size = xfer_size;

	/*
	 * bulk mode
	 */
	if (xfer_size % nr_chans != 0) {
		dev_err(dev, "invalid length\n");
		return -EINVAL;
	}

	do_div(per_chan_xfer_size, (uint32_t)nr_chans);

	local_sg = kmalloc_array(buffer_split_count, sizeof(*local_sg),
				 GFP_KERNEL);
	if (!local_sg)
		return -ENOMEM;
	remote_sg = kmalloc_array(buffer_split_count, sizeof(*remote_sg),
				  GFP_KERNEL);
	if (!remote_sg)
		goto free_remote;

	ktime_get_ts64(start);
	for (k = 0; k < nr_repeated_xfer; k++) {
		for (i = 0; i < nr_chans; i++) {
			bulk_trunk_size =
				per_chan_xfer_size / buffer_split_count;

			sg_init_table(local_sg, buffer_split_count);
			sg_init_table(remote_sg, buffer_split_count);

			for (j = 0; j < buffer_split_count; j++) {
				sg_dma_len(&local_sg[j]) = bulk_trunk_size;
				sg_dma_len(&remote_sg[j]) = bulk_trunk_size;
				sg_dma_address(&local_sg[j]) =
					dma_local + per_chan_xfer_size * i +
					bulk_trunk_size * j;
				sg_dma_address(&remote_sg[j]) =
					dma_remote + per_chan_xfer_size * i +
					bulk_trunk_size * j;
			}

			if (!chans[i] || !chans[i]->device ||
			    !chans[i]->device->device_prep_slave_sg)
				goto free_local;

			tx[i] = chans[i]->device->device_prep_slave_sg(
				chans[i], local_sg, buffer_split_count, dir,
				flags, remote_sg);
			if (!tx[i]) {
				dev_err(dev,
					"Failed to prepare cdns uDMA slave sg tx\n");
				goto free_local;
			}
		}
		/* completing multiple times is still well-behavior, linux completion has ref counter */
		reinit_completion(&epf_amba_test->transfer_complete);
		for (i = 0; i < nr_chans; i++) {
			tx[i]->callback = pci_epf_amba_test_dma_callback;
			tx[i]->callback_param = epf_amba_test;
			cookie = tx[i]->tx_submit(tx[i]);

			ret = dma_submit_error(cookie);
			if (ret) {
				dev_err(dev, "Failed to do DMA tx_submit %d\n",
					cookie);
				goto free_local;
			}

			dma_async_issue_pending(chans[i]);
		}
		/*
	 * FIXME: The best way is to use nr completion for nr channels, but cdns udma
	 * may miss some channels mask in common_udma_int somehow.
	 */
		ret = wait_for_completion_interruptible(
			&epf_amba_test->transfer_complete);
		if (ret) {
			dmaengine_terminate_sync(chans[i]);
			dev_err(dev, "DMA failed: get signaled\n");
			goto free_local;
		}
	}
	ktime_get_ts64(end);

free_local:
	/* non-dma doesn't use local nor remote sg */
	if (local_sg)
		kfree(local_sg);
free_remote:
	/*
	 * scatter/gather doesn't need remote_sg because it doesn't support
	 * multiple slave address
	 */
	if (remote_sg)
		kfree(remote_sg);

	return ret;
}

static int cdns_udma_sg_transfer(
	struct dma_chan *chan, enum dma_transfer_direction dir,
	dma_addr_t dma_remote, dma_addr_t dma_local,
	struct pci_epf_amba_test *epf_amba_test, u32 buffer_split_count,
	struct dma_async_tx_descriptor *tx, size_t xfer_size, int flags,
	struct timespec64 *start, struct timespec64 *end, u32 nr_repeated_xfer)
{
	int ret = -EINVAL, i, j;
	struct dma_slave_config sconf = {};
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	dma_cookie_t cookie;
	struct scatterlist *local_sg;

	sconf.direction = dir;
	sconf.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	sconf.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;

	if (dir == DMA_MEM_TO_DEV)
		sconf.dst_addr = dma_remote;
	else if (dir == DMA_DEV_TO_MEM)
		sconf.src_addr = dma_remote;
	else {
		dev_err(dev, "Invalid DMA direction\n");
		return -EINVAL;
	}

	if (dmaengine_slave_config(chan, &sconf)) {
		dev_err(dev, "DMA slave config fail\n");
		return -EIO;
	}

	local_sg = kmalloc_array(buffer_split_count, sizeof(*local_sg),
				 GFP_KERNEL);
	if (!local_sg)
		return -ENOMEM;
	sg_init_table(local_sg, buffer_split_count);
	for (i = 0; i < buffer_split_count; i++) {
		sg_dma_address(&local_sg[i]) =
			dma_local + xfer_size / buffer_split_count * i;
		sg_dma_len(&local_sg[i]) = xfer_size / buffer_split_count;
	}
	ktime_get_ts64(start);
	for (j = 0; j < nr_repeated_xfer; j++) {
		tx = dmaengine_prep_slave_sg(chan, local_sg, buffer_split_count,
					     dir, flags);
		if (!tx) {
			dev_err(dev, "Failed to prepare cdns uDMA slave sg\n");
			goto free_local;
		}
		/* completing multiple times is still well-behavior, linux completion has ref counter */
		reinit_completion(&epf_amba_test->transfer_complete);
		tx->callback = pci_epf_amba_test_dma_callback;
		tx->callback_param = epf_amba_test;
		cookie = tx->tx_submit(tx);

		ret = dma_submit_error(cookie);
		if (ret) {
			dev_err(dev, "Failed to do DMA tx_submit %d\n", cookie);
			goto free_local;
		}

		dma_async_issue_pending(chan);
		ret = wait_for_completion_interruptible(
			&epf_amba_test->transfer_complete);
		if (ret) {
			dmaengine_terminate_sync(chan);
			dev_err(dev, "DMA failed: get signaled\n");
			goto free_local;
		}
	}
	ktime_get_ts64(end);

free_local:
	/* non-dma doesn't use local nor remote sg */
	if (local_sg)
		kfree(local_sg);

	return ret;
}

static int slave_cdns_udma_transfer(
	u32 cdns_dma_mode, u32 buffer_split_count, size_t xfer_size,
	dma_addr_t dma_local, dma_addr_t dma_remote, struct dma_chan **chans,
	enum dma_transfer_direction dir, enum dma_ctrl_flags flags,
	struct dma_async_tx_descriptor **tx, int nr_chans,
	struct pci_epf_amba_test *epf_amba_test, struct timespec64 *start,
	struct timespec64 *end, u32 nr_repeated_xfer)
{
	int ret;
	struct dma_chan *first_chan = chans[0];
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;

	/*
	 * scatter/gather mode
	 */
	if (cdns_dma_mode == 2) {
		ret = cdns_udma_sg_transfer(first_chan, dir, dma_remote,
					    dma_local, epf_amba_test,
					    buffer_split_count, tx[0],
					    xfer_size, flags, start, end,
					    nr_repeated_xfer);
	} else if (cdns_dma_mode == 1) {
		ret = cdns_udma_bulk_transfer(dir, dma_remote, dma_local,
					      epf_amba_test, buffer_split_count,
					      tx, xfer_size, nr_chans, chans,
					      flags, start, end,
					      nr_repeated_xfer);
	} else {
		dev_err(dev, "invalid cdns dma mode\n");
		ret = -EINVAL;
	}

	return ret;
}

static int slave_generic_transfer(struct dma_chan *chan, dma_addr_t dma_local,
				  dma_addr_t dma_remote, size_t xfer_size,
				  struct pci_epf_amba_test *epf_amba_test,
				  enum dma_ctrl_flags flags,
				  enum dma_transfer_direction dir,
				  struct timespec64 *start,
				  struct timespec64 *end, u32 nr_repeated_xfer)

{
	struct dma_async_tx_descriptor *tx;
	int ret = 0, j;
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	dma_cookie_t cookie;
	struct dma_slave_config sconf = {};

	if (dmaengine_slave_config(chan, &sconf)) {
		dev_err(dev, "DMA slave config fail\n");
		return -EIO;
	}

	sconf.direction = dir;

	if (dir == DMA_MEM_TO_DEV)
		sconf.dst_addr = dma_remote;
	else if (dir == DMA_DEV_TO_MEM)
		sconf.src_addr = dma_remote;
	else {
		dev_err(dev, "Invalid DMA direction\n");
		return -EINVAL;
	}

	ktime_get_ts64(start);
	for (j = 0; j < nr_repeated_xfer; j++) {
		tx = dmaengine_prep_slave_single(chan, dma_local, xfer_size,
						 dir, flags);

		if (!tx) {
			dev_err(dev, "Failed to prepare DMA slave tx\n");
			return -EINVAL;
		}
		reinit_completion(&epf_amba_test->transfer_complete);
		tx->callback = pci_epf_amba_test_dma_callback;
		tx->callback_param = epf_amba_test;

		cookie = tx->tx_submit(tx);

		ret = dma_submit_error(cookie);
		if (ret) {
			dev_err(dev, "Failed to do DMA tx_submit %d\n", cookie);
			return -EINVAL;
		}

		dma_async_issue_pending(chan);
		ret = wait_for_completion_interruptible(
			&epf_amba_test->transfer_complete);
		if (ret) {
			dmaengine_terminate_sync(chan);
			dev_err(dev, "DMA failed: get signaled\n");
			return ret;
		}
	}
	ktime_get_ts64(end);
	return ret;
}

static int slave_transfer(u32 cdns_dma_mode, enum dma_transfer_direction dir,
			  dma_addr_t dma_remote,
			  struct pci_epf_amba_test *epf_amba_test,
			  struct dma_chan **chans, u32 buffer_split_count,
			  dma_addr_t dma_local, size_t xfer_size,
			  struct dma_async_tx_descriptor **tx,
			  enum dma_ctrl_flags flags, int nr_chans,
			  struct timespec64 *start, struct timespec64 *end,
			  u32 nr_repeated_xfer)
{
	struct dma_chan *first_chan = chans[0];

	if (cdns_dma_mode)
		return slave_cdns_udma_transfer(
			cdns_dma_mode, buffer_split_count, xfer_size, dma_local,
			dma_remote, chans, dir, flags, tx, nr_chans,
			epf_amba_test, start, end, nr_repeated_xfer);

	return slave_generic_transfer(first_chan, dma_local, dma_remote,
				      xfer_size, epf_amba_test, flags, dir,
				      start, end, nr_repeated_xfer);
}

/**
 * pci_epf_amba_test_data_transfer() - Function that uses dmaengine API to transfer
 *				  data between PCIe EP and remote PCIe RC
 * @chans: the channel used for transfer.
 * @nr_chans: chans used for this transfer.
 * @epf_amba_test: the EPF test device that performs the data transfer operation
 * @dma_dst: The destination address of the data transfer. It can be a physical
 *	     address given by pci_epc_mem_alloc_addr or DMA mapping APIs.
 * @dma_src: The source address of the data transfer. It can be a physical
 *	     address given by pci_epc_mem_alloc_addr or DMA mapping APIs.
 * @len: The size of the data transfer
 * @dma_remote: remote RC physical address
 * @dir: DMA transfer direction
 * @cdns_dma_mode: dma controller mode, either bulk or s/g, only used for CDNS uDMA
 *                  0 represents not used(poor man's std::optional),
 *                  1 represents bulk mode,
 *                  2 represetns s/g.
 *
 *                  non-cdns DMA controller should just pass 0 to this argument.
 *
 * Function that uses dmaengine API to transfer data between PCIe EP and remote
 * PCIe RC. The source and destination address can be a physical address given
 * by pci_epc_mem_alloc_addr or the one obtained using DMA mapping APIs.
 *
 * The function returns '0' on success and negative value on failure.
 */
static int pci_epf_amba_test_data_transfer(
	struct dma_chan **chans, int nr_chans,
	struct pci_epf_amba_test *epf_amba_test, dma_addr_t dma_dst,
	dma_addr_t dma_src, size_t len, dma_addr_t dma_remote,
	enum dma_transfer_direction dir, u32 buffer_split_count,
	u32 cdns_dma_mode, struct timespec64 *start, struct timespec64 *end,
	u32 nr_repeated_xfer)
{
	dma_addr_t dma_local = (dir == DMA_MEM_TO_DEV) ? dma_src : dma_dst;
	enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	struct pci_epf *epf = epf_amba_test->epf;
	struct dma_async_tx_descriptor **tx;
	struct device *dev = &epf->dev;
	int ret = 0;
	int i;
	struct dma_chan *first_chan;
	size_t xfer_size = len;

	if (nr_chans != 1 && cdns_dma_mode != 1) {
		dev_err(dev,
			"Multi channels only support cdns bulk mode now\n");
		return -EINVAL;
	}
	if (nr_chans != 1 && dir == DMA_MEM_TO_MEM) {
		dev_err(dev, "Multi channels doesn't support m2m now\n");
		return -EINVAL;
	}
	for (i = 0; i < nr_chans; i++) {
		if (IS_ERR_OR_NULL(chans[i])) {
			dev_err(dev, "Invalid DMA channel\n");
			return -EINVAL;
		}
	}

	tx = kmalloc_array(nr_chans, sizeof(struct dma_async_tx_descriptor *),
			   GFP_KERNEL);

	if (!tx)
		return -ENOMEM;

	first_chan = chans[0];

	if (dir == DMA_MEM_TO_DEV || dir == DMA_DEV_TO_MEM) {
		ret = slave_transfer(cdns_dma_mode, dir, dma_remote,
				     epf_amba_test, chans, buffer_split_count,
				     dma_local, xfer_size, tx, flags, nr_chans,
				     start, end, nr_repeated_xfer);
	} else if (dir == DMA_MEM_TO_MEM) {
		ret = m2m_transfer(cdns_dma_mode, dir, dma_dst, dma_src, tx[0],
				   epf_amba_test, first_chan, xfer_size, flags,
				   start, end, nr_repeated_xfer);
	}

	kfree(tx);
	return ret;
}

struct epf_dma_filter {
	struct device *dev;
	u32 dma_mask;
};

static bool epf_dma_filter_fn(struct dma_chan *chan, void *node)
{
	struct epf_dma_filter *filter = node;
	struct dma_slave_caps caps;

	memset(&caps, 0, sizeof(caps));
	dma_get_slave_caps(chan, &caps);

	if (IS_ENABLED(CONFIG_ARCH_AMBARELLA) &&
	    ambarella_is_cdns_udma(chan, filter->dev) &&
	    filter->dma_mask & caps.directions)
		return true;

	return chan->device->dev == filter->dev &&
	       (filter->dma_mask & caps.directions);
}

/**
 * pci_epf_amba_test_init_dma_chan() - Function to initialize EPF test DMA channel
 * @epf_amba_test: the EPF test device that performs data transfer operation
 *
 * Function to initialize EPF test DMA channel.
 */
static struct dma_chan **
pci_epf_amba_test_init_dma_chan(struct pci_epf_amba_test *epf_amba_test,
				u32 dma_mask, u32 nr_channels)
{
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	struct epf_dma_filter filter;
	struct dma_chan **dma_chan;
	dma_cap_mask_t mask;
	int i, requested = 0;

	filter.dev = epf->epc->dev.parent;
	filter.dma_mask = dma_mask;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	dma_chan = kvcalloc(nr_channels, sizeof(struct dma_chan *), GFP_KERNEL);
	for (i = 0; i < nr_channels; i++) {
		dma_chan[i] =
			dma_request_channel(mask, epf_dma_filter_fn, &filter);
		if (!dma_chan[i]) {
			dev_err(dev,
				"Failed to get enough DMA channel, tests aborted.\n");
			goto free_channels;
		}
		requested++;
	}

	init_completion(&epf_amba_test->transfer_complete);

	return dma_chan;

free_channels:
	for (i = 0; i < requested; i++)
		dma_release_channel(dma_chan[i]);
	kvfree(dma_chan);
	return NULL;
}

static void
pci_epf_amba_test_print_rate(const char *ops, u64 size,
			     struct timespec64 *start, struct timespec64 *end,
			     bool dma, int buffer_split_count, bool stream_dma,
			     enum cdns_dma_mode mode, int nr_channels)
{
	struct timespec64 ts;
	u64 rate, ns;

	ts = timespec64_sub(*end, *start);

	/* convert both size (stored in 'rate') and time in terms of 'ns' */
	ns = timespec64_to_ns(&ts);
	rate = size * NSEC_PER_SEC;

	/* Divide both size (stored in 'rate') and ns by a common factor */
	while (ns > UINT_MAX) {
		rate >>= 1;
		ns >>= 1;
	}

	if (!ns)
		return;

	/* calculate the rate */
	do_div(rate, (uint32_t)ns);

	if (dma)
		pr_info("\n%s => Size: %llu bytes\t DMA: %s\t Time: %llu.%09u seconds\t"
			"Rate: %llu MB/s\t Split to %d chunks, use %d channel(s)\n",
			ops, size,
			stream_dma ? "dma_map_*" : "dma_alloc_coherent",
			(u64)ts.tv_sec, (u32)ts.tv_nsec, rate / 1024 / 1024,
			buffer_split_count, nr_channels);
	else
		pr_info("\n%s => Size: %llu bytes\t DMA: NO\t Time: %llu.%09u seconds\t"
			"Rate: %llu MB/s\t\n",
			ops, size, (u64)ts.tv_sec, (u32)ts.tv_nsec,
			rate / 1024 / 1024);
}

static int pci_epf_amba_test_copy(struct pci_epf_amba_test *epf_amba_test)
{
	int ret;
	bool use_dma, use_dma_alloc_coherent = false;
	void __iomem *src_addr;
	void __iomem *dst_addr;
	phys_addr_t src_phys_addr;
	phys_addr_t dst_phys_addr;
	struct timespec64 start, end;
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	struct pci_epc *epc = epf->epc;
	enum pci_barno test_reg_bar = epf_amba_test->test_reg_bar;
	struct pci_epf_amba_test_reg *reg = epf_amba_test->reg[test_reg_bar];
	struct dma_chan **chan;
	u32 nr_channels = reg->nr_channels;
	int i;

	src_addr = pci_epc_mem_alloc_addr(epc, &src_phys_addr, reg->size);
	if (!src_addr) {
		dev_err(dev, "Failed to allocate source address\n");
		reg->status = STATUS_SRC_ADDR_INVALID;
		ret = -ENOMEM;
		goto err;
	}

	ret = pci_epc_map_addr(epc, epf->func_no, epf->vfunc_no, src_phys_addr,
			       reg->src_addr, reg->size);
	if (ret) {
		dev_err(dev, "Failed to map source address\n");
		reg->status = STATUS_SRC_ADDR_INVALID;
		goto err_src_addr;
	}

	dst_addr = pci_epc_mem_alloc_addr(epc, &dst_phys_addr, reg->size);
	if (!dst_addr) {
		dev_err(dev, "Failed to allocate destination address\n");
		reg->status = STATUS_DST_ADDR_INVALID;
		ret = -ENOMEM;
		goto err_src_map_addr;
	}

	ret = pci_epc_map_addr(epc, epf->func_no, epf->vfunc_no, dst_phys_addr,
			       reg->dst_addr, reg->size);
	if (ret) {
		dev_err(dev, "Failed to map destination address\n");
		reg->status = STATUS_DST_ADDR_INVALID;
		goto err_dst_addr;
	}

	use_dma = !!(reg->flags & FLAG_USE_DMA);
	if (use_dma) {
		chan = pci_epf_amba_test_init_dma_chan(
			epf_amba_test, BIT(DMA_MEM_TO_MEM), nr_channels);
		if (!chan) {
			dev_err(dev, "%d: No enough channels available\n",
				__LINE__);
			ret = -EINVAL;
			goto err_map_addr;
		}

		for (i = 0; i < nr_channels; i++) {
			use_dma_alloc_coherent =
				!!(reg->flags & FLAG_USE_DMA_ALLOC_COHERENT);

			ret = pci_epf_amba_test_data_transfer(
				chan, 1, epf_amba_test, dst_phys_addr,
				src_phys_addr, reg->size, 0, DMA_MEM_TO_MEM,
				reg->buffer_split_count, reg->cdns_dma_mode,
				&start, &end, reg->nr_repeated_xfer);
			if (ret)
				dev_err(dev, "Data transfer failed\n");

			dma_release_channel(chan[i]);
		}
	} else {
		void *buf;

		buf = kzalloc(reg->size, GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto err_map_addr;
		}

		ktime_get_ts64(&start);
		if (IS_ENABLED(CONFIG_ARCH_AMBARELLA)) {
			memcpy_fromio_ambarella(buf, src_addr, reg->size);
			memcpy_toio_ambarella(dst_addr, buf, reg->size);
		} else {
			memcpy_fromio(buf, src_addr, reg->size);
			memcpy_toio(dst_addr, buf, reg->size);
		}
		ktime_get_ts64(&end);
		kfree(buf);
	}
	pci_epf_amba_test_print_rate("COPY ", reg->size, &start, &end, use_dma,
				     reg->buffer_split_count,
				     !use_dma_alloc_coherent,
				     reg->cdns_dma_mode, 1);

err_map_addr:
	pci_epc_unmap_addr(epc, epf->func_no, epf->vfunc_no, dst_phys_addr);

err_dst_addr:
	pci_epc_mem_free_addr(epc, dst_phys_addr, dst_addr, reg->size);

err_src_map_addr:
	pci_epc_unmap_addr(epc, epf->func_no, epf->vfunc_no, src_phys_addr);

err_src_addr:
	pci_epc_mem_free_addr(epc, src_phys_addr, src_addr, reg->size);

err:
	return ret;
}

static int pci_epf_amba_test_read(struct pci_epf_amba_test *epf_amba_test)
{
	int ret;
	void __iomem *src_addr;
	void *buf;
	u32 crc32;
	bool use_dma, use_dma_alloc_coherent = false, verbose_output = false,
		      mult_chan_for_single_xfer = false;
	phys_addr_t phys_addr;
	phys_addr_t dst_phys_addr;
	struct timespec64 start, end;
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	struct pci_epc *epc = epf->epc;
	enum pci_barno test_reg_bar = epf_amba_test->test_reg_bar;
	struct pci_epf_amba_test_reg *reg = epf_amba_test->reg[test_reg_bar];
	struct dma_chan **chan = NULL;
	u32 nr_channels = reg->nr_channels;
	int i;

	src_addr = pci_epc_mem_alloc_addr(epc, &phys_addr, reg->size);
	if (!src_addr) {
		dev_err(dev, "Failed to allocate address\n");
		reg->status = STATUS_SRC_ADDR_INVALID;
		ret = -ENOMEM;
		goto err;
	}

	ret = pci_epc_map_addr(epc, epf->func_no, epf->vfunc_no, phys_addr,
			       reg->src_addr, reg->size);
	if (ret) {
		dev_err(dev, "Failed to map address\n");
		reg->status = STATUS_SRC_ADDR_INVALID;
		goto err_addr;
	}

	use_dma = !!(reg->flags & FLAG_USE_DMA);
	verbose_output = !!(reg->flags & FLAG_USE_VERBOSE_OUTPUT);
	mult_chan_for_single_xfer =
		!!(reg->flags & FLAG_USE_MULT_CHAN_FOR_SINGLE_XFER);
	if (use_dma) {
		chan = pci_epf_amba_test_init_dma_chan(
			epf_amba_test, BIT(DMA_DEV_TO_MEM), nr_channels);
		if (!chan) {
			dev_err(dev, "%d: No enough channels available\n",
				__LINE__);
			ret = -EINVAL;
			goto err_map_addr;
		}

		use_dma_alloc_coherent =
			!!(reg->flags & FLAG_USE_DMA_ALLOC_COHERENT);

		if (!use_dma_alloc_coherent) {
			buf = kzalloc(reg->size, GFP_KERNEL);
			if (!buf) {
				ret = -ENOMEM;
				goto err_channels;
			}
		} else {
			buf = dma_alloc_coherent(chan[0]->device->dev,
						 reg->size, &dst_phys_addr,
						 GFP_KERNEL);

			if (!buf) {
				ret = -ENOMEM;
				goto err_channels;
			}
		}

		if (!mult_chan_for_single_xfer) {
			for (i = 0; i < nr_channels; i++) {
				dev_dbg(chan[0]->device->dev, "%s %d: use %s\n",
					__func__, __LINE__,
					use_dma_alloc_coherent ?
						"dma_alloc_coherent" :
						"dma_map_");

				if (!use_dma_alloc_coherent) {
					dst_phys_addr = dma_map_single(
						chan[0]->device->dev, buf,
						reg->size, DMA_FROM_DEVICE);
					if (dma_mapping_error(
						    chan[0]->device->dev,
						    dst_phys_addr)) {
						dev_err(dev,
							"Failed to map destination buffer addr\n");
						ret = -ENOMEM;
						goto err_dma_map;
					}
				}

				ret = pci_epf_amba_test_data_transfer(
					chan + i, 1, epf_amba_test,
					dst_phys_addr, phys_addr, reg->size,
					reg->src_addr, DMA_DEV_TO_MEM,
					reg->buffer_split_count,
					reg->cdns_dma_mode, &start, &end,
					reg->nr_repeated_xfer);
				if (ret)
					dev_err(dev, "Data transfer failed\n");

				if (!use_dma_alloc_coherent)
					dma_unmap_single(chan[0]->device->dev,
							 dst_phys_addr,
							 reg->size,
							 DMA_FROM_DEVICE);

				pci_epf_amba_test_print_rate(
					"READ ",
					reg->size * (u64)reg->nr_repeated_xfer,
					&start, &end, use_dma,
					reg->buffer_split_count,
					!use_dma_alloc_coherent,
					reg->cdns_dma_mode, 1);

				crc32 = crc32_le(~0, buf, reg->size);
				if (verbose_output)
					print_hex_dump(KERN_INFO,
						       "EP read buffer ",
						       DUMP_PREFIX_OFFSET, 16,
						       1, buf, reg->size,
						       false);

				if (crc32 != reg->checksum)
					ret = -EIO;
			}
		} else {
			dev_dbg(chan[0]->device->dev, "%s %d: use %s\n",
				__func__, __LINE__,
				use_dma_alloc_coherent ? "dma_alloc_coherent" :
							 "dma_map_");

			if (!use_dma_alloc_coherent) {
				dst_phys_addr =
					dma_map_single(chan[0]->device->dev,
						       buf, reg->size,
						       DMA_FROM_DEVICE);
				if (dma_mapping_error(chan[0]->device->dev,
						      dst_phys_addr)) {
					dev_err(dev,
						"Failed to map destination buffer addr\n");
					ret = -ENOMEM;
					goto err_dma_map;
				}
			}

			ret = pci_epf_amba_test_data_transfer(
				chan, nr_channels, epf_amba_test, dst_phys_addr,
				phys_addr, reg->size, reg->src_addr,
				DMA_DEV_TO_MEM, reg->buffer_split_count,
				reg->cdns_dma_mode, &start, &end,
				reg->nr_repeated_xfer);
			if (ret)
				dev_err(dev, "Data transfer failed\n");

			if (!use_dma_alloc_coherent)
				dma_unmap_single(chan[0]->device->dev,
						 dst_phys_addr, reg->size,
						 DMA_FROM_DEVICE);

			pci_epf_amba_test_print_rate(
				"READ ", reg->size * (u64)reg->nr_repeated_xfer,
				&start, &end, use_dma, reg->buffer_split_count,
				!use_dma_alloc_coherent, reg->cdns_dma_mode,
				nr_channels);

			crc32 = crc32_le(~0, buf, reg->size);
			if (verbose_output)
				print_hex_dump(KERN_INFO, "EP read buffer ",
					       DUMP_PREFIX_OFFSET, 16, 1, buf,
					       reg->size, false);

			if (crc32 != reg->checksum)
				ret = -EIO;
		}
	} else {
		buf = kzalloc(reg->size, GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto err_map_addr;
		}

		ktime_get_ts64(&start);
		if (IS_ENABLED(CONFIG_ARCH_AMBARELLA))
			memcpy_fromio_ambarella(buf, src_addr, reg->size);
		else
			memcpy_fromio(buf, src_addr, reg->size);
		ktime_get_ts64(&end);

		pci_epf_amba_test_print_rate("READ ", reg->size, &start, &end,
					     use_dma, reg->buffer_split_count,
					     !use_dma_alloc_coherent,
					     reg->cdns_dma_mode, 1);

		crc32 = crc32_le(~0, buf, reg->size);
		if (verbose_output)
			print_hex_dump(KERN_INFO, "EP read buffer ",
				       DUMP_PREFIX_OFFSET, 16, 1, buf,
				       reg->size, false);

		if (crc32 != reg->checksum)
			ret = -EIO;
	}
err_dma_map:
	if (!use_dma_alloc_coherent)
		kfree(buf);
	else
		dma_free_coherent(chan[0]->device->dev, reg->size, buf,
				  dst_phys_addr);
err_channels:
	if (use_dma) {
		for (i = 0; i < nr_channels; i++)
			dma_release_channel(chan[i]);
		kvfree(chan);
	}
err_map_addr:
	pci_epc_unmap_addr(epc, epf->func_no, epf->vfunc_no, phys_addr);

err_addr:
	pci_epc_mem_free_addr(epc, phys_addr, src_addr, reg->size);
err:
	return ret;
}

static int pci_epf_amba_test_write(struct pci_epf_amba_test *epf_amba_test)
{
	int ret;
	void __iomem *dst_addr;
	void *buf;
	bool use_dma, use_dma_alloc_coherent = false, verbose_output = false,
		      mult_chan_for_single_xfer = false;
	phys_addr_t phys_addr;
	phys_addr_t src_phys_addr;
	struct timespec64 start, end;
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	struct pci_epc *epc = epf->epc;
	enum pci_barno test_reg_bar = epf_amba_test->test_reg_bar;
	struct pci_epf_amba_test_reg *reg = epf_amba_test->reg[test_reg_bar];
	struct dma_chan **chan;
	u32 nr_channels = reg->nr_channels;
	int i;

	dst_addr = pci_epc_mem_alloc_addr(epc, &phys_addr, reg->size);
	if (!dst_addr) {
		dev_err(dev, "Failed to allocate address\n");
		reg->status = STATUS_DST_ADDR_INVALID;
		ret = -ENOMEM;
		goto err;
	}

	ret = pci_epc_map_addr(epc, epf->func_no, epf->vfunc_no, phys_addr,
			       reg->dst_addr, reg->size);
	if (ret) {
		dev_err(dev, "Failed to map address\n");
		reg->status = STATUS_DST_ADDR_INVALID;
		goto err_addr;
	}

	use_dma = !!(reg->flags & FLAG_USE_DMA);
	verbose_output = !!(reg->flags & FLAG_USE_VERBOSE_OUTPUT);
	mult_chan_for_single_xfer =
		!!(reg->flags & FLAG_USE_MULT_CHAN_FOR_SINGLE_XFER);
	if (use_dma) {
		chan = pci_epf_amba_test_init_dma_chan(
			epf_amba_test, BIT(DMA_MEM_TO_DEV), nr_channels);
		if (!chan) {
			dev_err(dev, "%d: No enough channels available\n",
				__LINE__);
			ret = -EINVAL;
			goto err_map_addr;
		}

		use_dma_alloc_coherent =
			!!(reg->flags & FLAG_USE_DMA_ALLOC_COHERENT);
		if (!use_dma_alloc_coherent) {
			buf = kzalloc(reg->size, GFP_KERNEL);
			if (!buf) {
				ret = -ENOMEM;
				goto err_channels;
			}
		} else {
			buf = dma_alloc_coherent(chan[0]->device->dev,
						 reg->size, &src_phys_addr,
						 GFP_KERNEL);

			if (!buf) {
				ret = -ENOMEM;
				goto err_channels;
			}
		}
		if (!mult_chan_for_single_xfer) {
			for (i = 0; i < nr_channels; i++) {
				dev_dbg(chan[0]->device->dev, "%s %d: use %s\n",
					__func__, __LINE__,
					use_dma_alloc_coherent ?
						"dma_alloc_coherent" :
						"dma_map_");

				if (!use_dma_alloc_coherent) {
					/*
				 * NOTE:
				 * Always get random bytes before mapping, otherwise RC always
				 * get zero data.
				 */
					get_random_bytes(buf, reg->size);
					reg->checksum =
						crc32_le(~0, buf, reg->size);

					src_phys_addr = dma_map_single(
						chan[0]->device->dev, buf,
						reg->size, DMA_TO_DEVICE);
					if (dma_mapping_error(
						    chan[0]->device->dev,
						    src_phys_addr)) {
						dev_err(dev,
							"Failed to map source buffer addr\n");
						ret = -ENOMEM;
						goto err_dma_map;
					}
				} else {
					get_random_bytes(buf, reg->size);
					reg->checksum =
						crc32_le(~0, buf, reg->size);
				}

				ret = pci_epf_amba_test_data_transfer(
					chan + i, 1, epf_amba_test, phys_addr,
					src_phys_addr, reg->size, reg->dst_addr,
					DMA_MEM_TO_DEV, reg->buffer_split_count,
					reg->cdns_dma_mode, &start, &end,
					reg->nr_repeated_xfer);
				if (ret)
					dev_err(dev, "Data transfer failed\n");

				if (!use_dma_alloc_coherent)
					dma_unmap_single(chan[0]->device->dev,
							 src_phys_addr,
							 reg->size,
							 DMA_TO_DEVICE);

				if (verbose_output)
					print_hex_dump(KERN_INFO,
						       "EP write buffer ",
						       DUMP_PREFIX_OFFSET, 16,
						       1, buf, reg->size,
						       false);

				pci_epf_amba_test_print_rate(
					"WRITE",
					reg->size * (u64)reg->nr_repeated_xfer,
					&start, &end, use_dma,
					reg->buffer_split_count,
					!use_dma_alloc_coherent,
					reg->cdns_dma_mode, 1);
			}
		} else {
			dev_dbg(chan[0]->device->dev, "%s %d: use %s\n",
				__func__, __LINE__,
				use_dma_alloc_coherent ? "dma_alloc_coherent" :
							 "dma_map_");

			if (!use_dma_alloc_coherent) {
				/*
				 * NOTE:
				 * Always get random bytes before mapping, otherwise RC always
				 * get zero data.
				 */
				get_random_bytes(buf, reg->size);
				reg->checksum = crc32_le(~0, buf, reg->size);

				src_phys_addr =
					dma_map_single(chan[0]->device->dev,
						       buf, reg->size,
						       DMA_TO_DEVICE);
				if (dma_mapping_error(chan[0]->device->dev,
						      src_phys_addr)) {
					dev_err(dev,
						"Failed to map source buffer addr\n");
					ret = -ENOMEM;
					goto err_dma_map;
				}
			} else {
				get_random_bytes(buf, reg->size);
				reg->checksum = crc32_le(~0, buf, reg->size);
			}

			ret = pci_epf_amba_test_data_transfer(
				chan, nr_channels, epf_amba_test, phys_addr,
				src_phys_addr, reg->size, reg->dst_addr,
				DMA_MEM_TO_DEV, reg->buffer_split_count,
				reg->cdns_dma_mode, &start, &end,
				reg->nr_repeated_xfer);
			if (ret)
				dev_err(dev, "Data transfer failed\n");

			if (!use_dma_alloc_coherent)
				dma_unmap_single(chan[0]->device->dev,
						 src_phys_addr, reg->size,
						 DMA_TO_DEVICE);

			if (verbose_output)
				print_hex_dump(KERN_INFO, "EP write buffer ",
					       DUMP_PREFIX_OFFSET, 16, 1, buf,
					       reg->size, false);

			pci_epf_amba_test_print_rate(
				"WRITE", reg->size * (u64)reg->nr_repeated_xfer,
				&start, &end, use_dma, reg->buffer_split_count,
				!use_dma_alloc_coherent, reg->cdns_dma_mode,
				nr_channels);
		}
	} else {
		buf = kzalloc(reg->size, GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto err_map_addr;
		}

		get_random_bytes(buf, reg->size);
		reg->checksum = crc32_le(~0, buf, reg->size);

		ktime_get_ts64(&start);
		if (IS_ENABLED(CONFIG_ARCH_AMBARELLA))
			memcpy_toio_ambarella(dst_addr, buf, reg->size);
		else
			memcpy_toio(dst_addr, buf, reg->size);
		ktime_get_ts64(&end);

		if (verbose_output)
			print_hex_dump(KERN_INFO, "EP write buffer ",
				       DUMP_PREFIX_OFFSET, 16, 1, buf,
				       reg->size, false);

		pci_epf_amba_test_print_rate("WRITE", reg->size, &start, &end,
					     use_dma, reg->buffer_split_count,
					     !use_dma_alloc_coherent,
					     reg->cdns_dma_mode, nr_channels);
	}

	/*
	 * wait 1ms inorder for the write to complete. Without this delay L3
	 * error in observed in the host system.
	 */
	usleep_range(1000, 2000);

err_dma_map:
	if (!use_dma_alloc_coherent)
		kfree(buf);
	else
		dma_free_coherent(chan[0]->device->dev, reg->size, buf,
				  src_phys_addr);

err_channels:
	if (use_dma) {
		for (i = 0; i < nr_channels; i++)
			dma_release_channel(chan[i]);
		kvfree(chan);
	}
err_map_addr:
	pci_epc_unmap_addr(epc, epf->func_no, epf->vfunc_no, phys_addr);

err_addr:
	pci_epc_mem_free_addr(epc, phys_addr, dst_addr, reg->size);

err:
	return ret;
}

static void pci_epf_amba_test_raise_irq(struct pci_epf_amba_test *epf_amba_test,
					u8 irq_type, u16 irq)
{
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	struct pci_epc *epc = epf->epc;
	enum pci_barno test_reg_bar = epf_amba_test->test_reg_bar;
	struct pci_epf_amba_test_reg *reg = epf_amba_test->reg[test_reg_bar];
	u32 status = reg->status | STATUS_IRQ_RAISED;

	/*
	* Set the status before raising the IRQ to ensure that the host sees
	* the updated value when it gets the IRQ.
	*/
	WRITE_ONCE(reg->status, status);
	switch (irq_type) {
	case IRQ_TYPE_LEGACY:
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  PCI_EPC_IRQ_LEGACY, 0);
		break;
	case IRQ_TYPE_MSI:
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  PCI_EPC_IRQ_MSI, irq);
		break;
	case IRQ_TYPE_MSIX:
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  PCI_EPC_IRQ_MSIX, irq);
		break;
	default:
		dev_err(dev, "Failed to raise IRQ, unknown type\n");
		break;
	}
}

static void pci_epf_amba_test_cmd_handler(struct work_struct *work)
{
	int ret;
	int count;
	u32 command;
	struct pci_epf_amba_test *epf_amba_test =
		container_of(work, struct pci_epf_amba_test, cmd_handler.work);
	struct pci_epf *epf = epf_amba_test->epf;
	struct device *dev = &epf->dev;
	struct pci_epc *epc = epf->epc;
	enum pci_barno test_reg_bar = epf_amba_test->test_reg_bar;
	struct pci_epf_amba_test_reg *reg = epf_amba_test->reg[test_reg_bar];

	command = READ_ONCE(reg->command);
	if (!command)
		goto reset_handler;

	WRITE_ONCE(reg->command, 0);
	WRITE_ONCE(reg->status, 0);

	if (reg->irq_type > IRQ_TYPE_MSIX) {
		dev_err(dev, "Failed to detect IRQ type\n");
		goto reset_handler;
	}

	if (command & COMMAND_RAISE_LEGACY_IRQ) {
		reg->status = STATUS_IRQ_RAISED;
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  PCI_EPC_IRQ_LEGACY, 0);
		goto reset_handler;
	}

	if (command & COMMAND_WRITE) {
		ret = pci_epf_amba_test_write(epf_amba_test);
		if (ret)
			reg->status |= STATUS_WRITE_FAIL;
		else
			reg->status |= STATUS_WRITE_SUCCESS;
		pci_epf_amba_test_raise_irq(epf_amba_test, reg->irq_type,
					    reg->irq_number);
		goto reset_handler;
	}

	if (command & COMMAND_READ) {
		ret = pci_epf_amba_test_read(epf_amba_test);
		if (!ret)
			reg->status |= STATUS_READ_SUCCESS;
		else
			reg->status |= STATUS_READ_FAIL;
		pci_epf_amba_test_raise_irq(epf_amba_test, reg->irq_type,
					    reg->irq_number);
		goto reset_handler;
	}

	if (command & COMMAND_COPY) {
		ret = pci_epf_amba_test_copy(epf_amba_test);
		if (!ret)
			reg->status |= STATUS_COPY_SUCCESS;
		else
			reg->status |= STATUS_COPY_FAIL;
		pci_epf_amba_test_raise_irq(epf_amba_test, reg->irq_type,
					    reg->irq_number);
		goto reset_handler;
	}

	if (command & COMMAND_RAISE_MSI_IRQ) {
		count = pci_epc_get_msi(epc, epf->func_no, epf->vfunc_no);
		if (reg->irq_number > count || count <= 0)
			goto reset_handler;
		reg->status = STATUS_IRQ_RAISED;
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  PCI_EPC_IRQ_MSI, reg->irq_number);
		goto reset_handler;
	}

	if (command & COMMAND_RAISE_MSIX_IRQ) {
		count = pci_epc_get_msix(epc, epf->func_no, epf->vfunc_no);
		if (reg->irq_number > count || count <= 0)
			goto reset_handler;
		reg->status = STATUS_IRQ_RAISED;
		pci_epc_raise_irq(epc, epf->func_no, epf->vfunc_no,
				  PCI_EPC_IRQ_MSIX, reg->irq_number);
		goto reset_handler;
	}

reset_handler:
	queue_delayed_work(kpcitest_workqueue, &epf_amba_test->cmd_handler,
			   msecs_to_jiffies(1));
}

static void pci_epf_amba_test_unbind(struct pci_epf *epf)
{
	struct pci_epf_amba_test *epf_amba_test = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar;
	int bar;

	cancel_delayed_work(&epf_amba_test->cmd_handler);
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		epf_bar = &epf->bar[bar];

		if (epf_amba_test->reg[bar]) {
			pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
					  epf_bar);
			pci_epf_free_space(epf, epf_amba_test->reg[bar], bar,
					   PRIMARY_INTERFACE);
		}
	}

	pci_epf_free_msi_doorbell(epf,
				  epf_amba_test->msi_doorbell_property.virq);
}

static int pci_epf_amba_test_set_bar(struct pci_epf *epf)
{
	int bar, add;
	int ret;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct pci_epf_amba_test *epf_amba_test = epf_get_drvdata(epf);
	enum pci_barno test_reg_bar = epf_amba_test->test_reg_bar;
	const struct pci_epc_features *epc_features;

	epc_features = epf_amba_test->epc_features;

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
			pci_epf_free_space(epf, epf_amba_test->reg[bar], bar,
					   PRIMARY_INTERFACE);
			dev_err(dev, "Failed to set BAR%d\n", bar);
			if (bar == test_reg_bar)
				return ret;
		}
	}

	return 0;
}

static int pci_epf_amba_test_core_init(struct pci_epf *epf)
{
	struct pci_epf_amba_test *epf_amba_test = epf_get_drvdata(epf);
	struct pci_epf_header *header = epf->header;
	const struct pci_epc_features *epc_features;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	bool msix_capable = false;
	bool msi_capable = true;
	int ret;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (epc_features) {
		msix_capable = epc_features->msix_capable;
		msi_capable = epc_features->msi_capable;
	}

	if (epf->vfunc_no <= 1) {
		ret = pci_epc_write_header(epc, epf->func_no, epf->vfunc_no,
					   header);
		if (ret) {
			dev_err(dev, "Configuration header write failed\n");
			return ret;
		}
	}

	ret = pci_epf_amba_test_set_bar(epf);
	if (ret)
		return ret;

	if (msi_capable) {
		ret = pci_epc_set_msi(epc, epf->func_no, epf->vfunc_no,
				      epf->msi_interrupts);
		if (ret) {
			dev_err(dev, "MSI configuration failed\n");
			return ret;
		}
	}

	if (msix_capable) {
		ret = pci_epc_set_msix(epc, epf->func_no, epf->vfunc_no,
				       epf->msix_interrupts,
				       epf_amba_test->test_reg_bar,
				       epf_amba_test->msix_table_offset);
		if (ret) {
			dev_err(dev, "MSI-X configuration failed\n");
			return ret;
		}
	}

	return 0;
}

static int pci_epf_amba_test_alloc_space(struct pci_epf *epf)
{
	struct pci_epf_amba_test *epf_amba_test = epf_get_drvdata(epf);
	struct device *dev = &epf->dev;
	struct pci_epf_bar *epf_bar;
	size_t msix_table_size = 0;
	size_t test_reg_bar_size;
	size_t pba_size = 0;
	bool msix_capable;
	void *base;
	int bar, add;
	enum pci_barno test_reg_bar = epf_amba_test->test_reg_bar;
	const struct pci_epc_features *epc_features;
	size_t test_reg_size;

	epc_features = epf_amba_test->epc_features;

	test_reg_bar_size = ALIGN(sizeof(struct pci_epf_amba_test_reg), 128);

	msix_capable = epc_features->msix_capable;
	if (msix_capable) {
		msix_table_size = PCI_MSIX_ENTRY_SIZE * epf->msix_interrupts;
		epf_amba_test->msix_table_offset = test_reg_bar_size;
		/* Align to QWORD or 8 Bytes */
		pba_size = ALIGN(DIV_ROUND_UP(epf->msix_interrupts, 8), 8);
	}
	test_reg_size = test_reg_bar_size + msix_table_size + pba_size;

	if (epc_features->bar_fixed_size[test_reg_bar]) {
		if (test_reg_size > bar_size[test_reg_bar])
			return -ENOMEM;
		test_reg_size = bar_size[test_reg_bar];
	}

	base = pci_epf_alloc_space(epf, test_reg_size, test_reg_bar,
				   epc_features->align, PRIMARY_INTERFACE);
	if (!base) {
		dev_err(dev, "Failed to allocated register space\n");
		return -ENOMEM;
	}
	epf_amba_test->reg[test_reg_bar] = base;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar += add) {
		epf_bar = &epf->bar[bar];
		add = (epf_bar->flags & PCI_BASE_ADDRESS_MEM_TYPE_64) ? 2 : 1;

		if (bar == test_reg_bar ||
		    bar == epf_amba_test->msi_doorbell_property.msi_doorbell_bar)
			continue;

		if (!!(epc_features->reserved_bar & (1 << bar)))
			continue;

		base = pci_epf_alloc_space(epf, bar_size[bar], bar,
					   epc_features->align,
					   PRIMARY_INTERFACE);
		if (!base)
			dev_err(dev, "Failed to allocate space for BAR%d\n",
				bar);
		epf_amba_test->reg[bar] = base;
	}

	return 0;
}

static void pci_epf_configure_bar(struct pci_epf *epf,
				  const struct pci_epc_features *epc_features)
{
	struct pci_epf_bar *epf_bar;
	bool bar_fixed_64bit;
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		epf_bar = &epf->bar[i];
		bar_fixed_64bit = !!(epc_features->bar_fixed_64bit & (1 << i));
		if (bar_fixed_64bit)
			epf_bar->flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;
		if (epc_features->bar_fixed_size[i])
			bar_size[i] = epc_features->bar_fixed_size[i];
		if (epc_features->bar_prefetch & (1 << i))
			epf_bar->flags |= PCI_BASE_ADDRESS_MEM_PREFETCH;
	}
}

static void pci_epf_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	/*
	 * TODO: check if we can get epc from desc->dev like
	 * struct pci_epc *epc = container_of(desc->dev, struct pci_epc, dev);
	 */
	struct pci_epf *epf = global_epf;
	struct pci_epf_amba_test *epf_amba_test = epf_get_drvdata(epf);

	epf_amba_test->msi_doorbell_property.msg = *msg;
}

static int pci_epf_amba_test_bind(struct pci_epf *epf)
{
	int ret;
	struct pci_epf_amba_test *epf_amba_test = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	enum pci_barno test_reg_bar = BAR_0;
	struct pci_epc *epc = epf->epc;
	enum pci_barno msi_doorbell_bar = MSI_DOORBELL_BAR;
	struct ambarella_msi_doorbell_property *msi_doorbell_property =
		&epf_amba_test->msi_doorbell_property;
	struct pci_epf_amba_test_reg *test_reg;

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (!epc_features) {
		dev_err(&epf->dev, "epc_features not implemented\n");
		return -EOPNOTSUPP;
	}

	pci_epf_configure_bar(epf, epc_features);

	epf_amba_test->test_reg_bar = test_reg_bar;
	epf_amba_test->epc_features = epc_features;

	if (epf_amba_test->cap & FLAG_SUPPORT_MSI_DOORBELL) {
		msi_doorbell_property->interrupt_handler =
			msi_doorbell_interrupt_handler;
		msi_doorbell_property->pci_epf_write_msi_msg =
			pci_epf_write_msi_msg;
		msi_doorbell_property->msi_doorbell_bar = msi_doorbell_bar;
		msi_doorbell_property->msi_doorbell_bar_size =
			bar_size[msi_doorbell_property->msi_doorbell_bar];

		ret = pci_epf_configure_msi_doorbell(msi_doorbell_property, epf,
						     epc_features);
		if (ret)
			return ret;
		/*
		 * XXX: there is no way to get msi bar vaddr, because it's
		 * allocated on platform MSI driver side.
		 */
		epf_amba_test->reg[msi_doorbell_bar] = NULL;
	}

	ret = pci_epf_amba_test_alloc_space(epf);
	if (ret)
		return ret;

	if (epf_amba_test->cap & FLAG_SUPPORT_MSI_DOORBELL) {
		test_reg = epf_amba_test->reg[test_reg_bar];
		WRITE_ONCE(
			test_reg->db_bar,
			epf_amba_test->msi_doorbell_property.msi_doorbell_bar);
		WRITE_ONCE(test_reg->db_offset, 0);
		WRITE_ONCE(test_reg->flags, epf_amba_test->cap);
		WRITE_ONCE(test_reg->db_data, 0xdb);
	}

	ret = pci_epf_amba_test_core_init(epf);
	if (ret)
		return ret;

	queue_work(kpcitest_workqueue, &epf_amba_test->cmd_handler.work);
	return 0;
}

static const struct pci_epf_device_id pci_epf_amba_test_ids[] = {
	{
		.name = "pci_epf_amba_test",
	},
	{},
};

struct amba_test_driverdata {
	int cap;
};

static const struct amba_test_driverdata cv72_data = {
	.cap = FLAG_SUPPORT_MSI_DOORBELL,
};

static const struct soc_device_attribute amba_test_soc_info[] = {
	{ .soc_id = "cv72", .data = (void *)&cv72_data },
	{ /* sentinel */ },
};

static int pci_epf_amba_test_probe(struct pci_epf *epf)
{
	struct pci_epf_amba_test *epf_amba_test;
	struct device *dev = &epf->dev;
	const struct soc_device_attribute *soc;
	const struct amba_test_driverdata *soc_data = NULL;

	global_epf = epf;
	epf_amba_test = devm_kzalloc(dev, sizeof(*epf_amba_test), GFP_KERNEL);
	if (!epf_amba_test)
		return -ENOMEM;

	epf->header = &test_header;
	epf_amba_test->epf = epf;

	soc = soc_device_match(amba_test_soc_info);
	if (soc) {
		soc_data = soc->data;
		epf_amba_test->cap = soc_data->cap;
	}

	INIT_DELAYED_WORK(&epf_amba_test->cmd_handler,
			  pci_epf_amba_test_cmd_handler);

	epf_set_drvdata(epf, epf_amba_test);
	return 0;
}

static struct pci_epf_ops ops = {
	.unbind = pci_epf_amba_test_unbind,
	.bind = pci_epf_amba_test_bind,
	.set_bar = pci_epf_amba_test_set_bar,
};

static struct pci_epf_driver test_driver = {
	.driver.name = "pci_epf_amba_test",
	.probe = pci_epf_amba_test_probe,
	.id_table = pci_epf_amba_test_ids,
	.ops = &ops,
	.owner = THIS_MODULE,
};

static int __init pci_epf_amba_test_init(void)
{
	int ret;

	kpcitest_workqueue =
		alloc_workqueue("kpcitest", WQ_MEM_RECLAIM | WQ_HIGHPRI, 0);
	if (!kpcitest_workqueue) {
		pr_err("Failed to allocate the kpcitest work queue\n");
		return -ENOMEM;
	}

	ret = pci_epf_register_driver(&test_driver);
	if (ret) {
		destroy_workqueue(kpcitest_workqueue);
		pr_err("Failed to register pci epf test driver --> %d\n", ret);
		return ret;
	}

	return 0;
}
module_init(pci_epf_amba_test_init);

static void __exit pci_epf_amba_test_exit(void)
{
	if (kpcitest_workqueue)
		destroy_workqueue(kpcitest_workqueue);
	pci_epf_unregister_driver(&test_driver);
}
module_exit(pci_epf_amba_test_exit);

MODULE_DESCRIPTION("PCI EPF AMBA TEST DRIVER");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_LICENSE("GPL v2");
