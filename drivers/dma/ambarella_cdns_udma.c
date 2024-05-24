// SPDX-License-Identifier: GPL-2.0
/*
 * Platform driver for the Cadence uDMA Controller
 *
 * Copyright (C) 2022 Ambarella.Inc
 * Author: Li Chen <lchen@ambarella.com>
 */

#include <linux/debugfs.h>
#include <linux/dmaengine.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/bits.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/overflow.h>
#include <linux/dmapool.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <uapi/linux/types.h>
#include <linux/sys_soc.h>
#include <linux/types.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include "virt-dma.h"

DEFINE_SPINLOCK(int_ena_lock);
DEFINE_SPINLOCK(int_dis_lock);
DEFINE_SPINLOCK(int_lock);

static bool enable_ib_mrrs_quirk = true;
module_param(enable_ib_mrrs_quirk, bool, 0644);
MODULE_PARM_DESC(
	enable_ib_mrrs_quirk,
	"Enable quirk for data corrupt issue if ib size <= mrrs (default: enable)");
static bool enable_ob_mrrs_quirk = true;
module_param(enable_ob_mrrs_quirk, bool, 0644);
MODULE_PARM_DESC(
	enable_ob_mrrs_quirk,
	"Enable quirk for data corrupt issue if ob size <= mrrs (default: enable)");

/*
 * uDMA supports three different types of transfer:
 * 1. Bulk transfers
 * 2. Scatter transfers
 * 3. Gather transfers.
 *
 * Bulk transfers supports non-contiguous system memory
 * and non-contiguous external memory xfer because, via
 * breaking down non-contiguous data areas into individual
 * contiguous transfers.
 *
 * This xfer mode is not traditional and not support by
 * Linux DMA engine framework. So it is not supported by
 * this driver.
 */

/*
 *
 *  ********************  Bulk mode **************************
 *  In Bulk mode, it transfer of a large amount of data between
 *  system memory and external memory. Firmware needs to
 *  understand how the data areas in the system memory and
 *  external memory map to each other.
 *  The data is less than or equal to the maximum definable in
 *  a descriptor (16 MB).
 *
 *  Read/Write desc      Read/Write desc      Read/Write desc
 *      ▲                     ▲                     ▲
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      │                     │                     │
 *      ▼                     ▼                     ▼
 *
 *  ********************  Scatter mode **************************
 *  In Scatter mode, the data is transferred from a single area in
 *  source memory to a number of areas in target memory.
 *
 *  Scattering is to take data from your local Device and send it
 *  to different address in Host memory.
 *
 *  The first descriptor(Pre-fetch) is used to fetchthe entire data
 *  and subsequent descriptors are used to write each set of data
 *  to target memory. The overall data size is restricted to RAM
 *  Partition size.
 *
 *                         local(DRAM)
 *  Post-write desc      Post-write desc         Post-write desc
 *       ▲                     ▲                       ▲
 *       │                     │                       │
 *       │                     │                       │
 *       │                     │                       │
 *       │                     │                       │
 *       │                     │                       │
 *       └─────────────────────┼───────────────────────┘
 *                             │
 *                             │
 *                             │
 *                             │
 *                             │
 *                             ▲
 *                       Pre-fetch desc
 *
 *                       External(PCIe)
 *
 ************************  Gather mode ***************************
 * In Gather mode, data is transferred from a number of areas in
 * source memory to a single area in target memory.
 *
 * Gathering is to get data from different address in Host memory
 * to your Local device.
 *
 * The initial descriptors in the linked list are used to fetch the
 * separate sets of data from source memory: the last descriptor
 * is used write all the data to target memory. The overall data
 * size is restricted to RAM Partition size.
 *
 *                         local(DRAM)
 *  Pre-fetch desc        Pre-fetch desc           Pre-fetch desc
 *         ▼                     ▼                       ▼
 *         │                     │                       │
 *         │                     │                       │
 *         │                     │                       │
 *         │                     │                       │
 *         └─────────────────────┼───────────────────────┘
 *                               │
 *                               │
 *                               │
 *                               │
 *                               │
 *                               ▼
 *                         Post-write desc
 *                            External
 */

/*
 * TODO:
 * - cdns_udma ff38600000.pcie2-udma: WARN: Device release
 *   is not defined so it is not safe to unbind this driver while in use
 */

#define CONTINUE_TO_EXECUTE_LINKED_LIST 1
#define DONT_CONTINUE_TO_EXECUTE_LINKED_LIST 0

/*
 * This may be larger than COMMON_UDMA_CONFIG_NUM_CHANNELS_MASK
 */
#define CDNS_UDMA_MAX_CHANNELS 8
#define COMMON_UDMA_INT_BITS 16

#define DMA_OB_CMD 0x3
#define DMA_IB_CMD 0x1

/* Length of xfer in bytes(0 indicates maximum length xfer 2 ^ 24 bytes) */
#define REG_FIELD_LENGTH_MASK GENMASK(23, 0)
#define REG_FIELD_CONTROL_MASK GENMASK(31, 24) /* Control Byte */

#define CHANNEL_CTRL_OFFSET(id) (0x0 + 0x14 * id)
#define CHANNEL_SP_L_OFFSET(id) (0x4 + 0x14 * id)
#define CHANNEL_SP_U_OFFSET(id) (0x8 + 0x14 * id)
#define CHANNEL_ATTR_L_OFFSET(id) (0xc + 0x14 * id)
#define CHANNEL_ATTR_U_OFFSET(id) (0x10 + 0x14 * id)
#define COMMON_UDMA_INT_OFFSET 0xa0
#define COMMON_UDMA_INT_ENA_OFFSET 0xa4
#define COMMON_UDMA_INT_DIS_OFFSET 0xa8

#define COMMON_UDMA_IB_ECC_UNCORRECTABLE_ERRORS_OFFSET 0xac
#define COMMON_UDMA_IB_ECC_CORRECTABLE_ERRORS_OFFSET 0xb0
#define COMMON_UDMA_IB_ECC_UNCORRECTABLE_ERRORS_MASK GENMASK(15, 0)
#define COMMON_UDMA_IB_ECC_CORRECTABLE_ERRORS_MASK GENMASK(15, 0)

#define COMMON_UDMA_OB_ECC_UNCORRECTABLE_ERRORS_OFFSET 0xb4
#define COMMON_UDMA_OB_ECC_CORRECTABLE_ERRORS_OFFSET 0xb8
#define COMMON_UDMA_OB_ECC_UNCORRECTABLE_ERRORS_MASK GENMASK(15, 0)
#define COMMON_UDMA_OB_ECC_CORRECTABLE_ERRORS_MASK GENMASK(15, 0)

#define CTRL_BYTE_INT_MASK BIT(0)
#define CTRL_BYTE_CONTINUITY_MASK GENMASK(2, 1) /* R/W or Prefetch or Write */
#define CTRL_BYTE_CONTINUE_MASK BIT(5)

#define COMMON_UDMA_CONFIG_OFFSET 0xfc
#define COMMON_UDMA_CAP_VER_MIN_VER_MASK GENMASK(7, 0)
#define COMMON_UDMA_CAP_VER_MAJ_VER_MASK GENMASK(15, 8)

#define COMMON_UDMA_CAP_VER_OFFSET 0xf8
#define COMMON_UDMA_CONFIG_NUM_CHANNELS_MASK GENMASK(3, 0)
#define COMMON_UDMA_CONFIG_NUM_PARTITIONS_MASK GENMASK(7, 4)
#define COMMON_UDMA_CONFIG_PARTITIONS_SIZE_MASK GENMASK(11, 8)
#define COMMON_UDMA_CONFIG_SYS_AW_GT_32_MASK BIT(12)
#define COMMON_UDMA_CONFIG_SYS_TW_GT_32_MASK BIT(13)
#define COMMON_UDMA_CONFIG_EXT_AW_GT_32_MASK BIT(14)
#define COMMON_UDMA_CONFIG_EXT_TW_GT_32_MASK BIT(15)

#define BULK_XFER_MAX_SIZE_PER_DESC SZ_16M

enum cdns_udma_dir {
	OUTBOUND,
	INBOUND,
};

enum interrupt_val {
	DONT_INTERRUPT,
	INTERRUPT,
};

struct pcie_status {
	u8 sys_status; /* System (local) bus status */
	u8 ext_status; /* External (remote) bus status */
	u8 chnl_status; /* uDMA channel status */
	u8 reserved_0; /** Reserved */
};

/*
 * The pcie_master_AXI_AR/WSIZE variation from its max value of (4)
 * is not allowed when pcie_master_AXI_ARLEN is not zero in a request.
 *
 * So use 32 instead of 64 bytes variable here.
 *
 */
struct cdns_udma_lli {
	__le32 sys_lo_addr; /* local-axi-addr */
	__le32 sys_hi_addr;
	__le32 sys_attr;

	__le32 ext_lo_addr; /* ext-pci-bus-addr */
	__le32 ext_hi_addr;
	__le32 ext_attr;

	__le32 size_and_ctrl_bits;
	struct pcie_status status;
	__le32 next;
	__le32 next_hi_addr;
};

struct cdns_udma_desc_node {
	struct cdns_udma_lli *lli;
	dma_addr_t lli_dma_addr;
};

struct cdns_desc {
	struct virt_dma_desc vd;
	unsigned int count;
	enum cdns_udma_dir dir;
	enum dma_status status;
	struct cdns_udma_desc_node node[];
};

/**
 * struct cdns_udma_chan - This is the struct for udma channel
 * @refcount:              reference counter to check if common_udma_int
 *                         has correct masks.
 * @idx:                   index of udma channel.
 */
struct cdns_udma_chan {
	struct virt_dma_chan vc;
	struct cdns_desc *desc;
	struct dma_pool *desc_pool;
	struct cdns_udma_dev *udma_dev;
	u8 idx;
	bool in_use;
	atomic_t refcount;
	struct dma_slave_config config;
	dma_addr_t local_addr;
	dma_addr_t external_addr;
	struct tasklet_struct irqtask;
};

/**
 * cdns_udma_driverdata - driver specific data
 * @external_alignment: SoCs like Ambarella cv3 has external(PCIe) address alignment restriction
 * @local_alignment: SoCs like Ambarella cv3 has local(dram) address alignment restriction
 * @quirks: Optional platform quirks.
 *         - CDNS_UDMA_OB_QUIRK_BULK_MRRS: Ambarella cv3/cv5 roled as EP may get corrupt when do
 *                                      outbound DMA if RC is PC.
 *         - CDNS_UDMA_IB_QUIRK_BULK_MRRS: Ambarella cv3/cv5 roled as EP may get corrupt when do
 *                                      inbound DMA if RC is PC.
 */
struct cdns_udma_driverdata {
	u32 external_alignment;
	u32 local_alignment;
#define CDNS_UDMA_OB_QUIRK_BULK_MRRS BIT(0)
#define CDNS_UDMA_IB_QUIRK_BULK_MRRS BIT(1)
	u32 quirks;
	int (*get_mrrs)(struct cdns_udma_dev *udma_dev);
};

struct cdns_udma_dev {
	struct device *dev;
	struct dma_device dma_dev;
	u32 chan_num;
	u32 partition_size;
	int irq;
	void __iomem *dma_base;
	const struct cdns_udma_driverdata *data;
	bool is_rc;
	struct cdns_udma_chan chan[];
};

static inline struct cdns_udma_chan *to_cdns_udma_chan(struct dma_chan *c)
{
	return container_of(c, struct cdns_udma_chan, vc.chan);
}

static struct cdns_desc *to_cdns_udma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct cdns_desc, vd);
}

static inline struct cdns_desc *to_cdns_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct cdns_desc, vd);
}

static int cdns_udma_mask_irq(struct cdns_udma_chan *chan)
{
	unsigned long stat = 0;

	if (chan->idx > chan->udma_dev->chan_num) {
		dev_err(chan->udma_dev->dev, "invalid chan idx %d\n",
			chan->idx);
		return -EINVAL;
	}

	spin_lock(&int_dis_lock);
	stat = readl(chan->udma_dev->dma_base + COMMON_UDMA_INT_DIS_OFFSET);
	set_bit(chan->idx, &stat);
	set_bit(chan->idx + CDNS_UDMA_MAX_CHANNELS, &stat);
	writel(stat, chan->udma_dev->dma_base + COMMON_UDMA_INT_DIS_OFFSET);
	spin_unlock(&int_dis_lock);
	return 0;
}

static int cdns_udma_unmask_irq(struct cdns_udma_chan *chan)
{
	unsigned long stat = 0;

	if (chan->idx > chan->udma_dev->chan_num) {
		dev_err(chan->udma_dev->dev, "invalid chan idx %d\n",
			chan->idx);
		return -EINVAL;
	}

	spin_lock(&int_ena_lock);
	stat = readl(chan->udma_dev->dma_base + COMMON_UDMA_INT_ENA_OFFSET);
	set_bit(chan->idx, &stat);
	set_bit(chan->idx + CDNS_UDMA_MAX_CHANNELS, &stat);
	writel(stat, chan->udma_dev->dma_base + COMMON_UDMA_INT_ENA_OFFSET);
	spin_unlock(&int_ena_lock);
	return 0;
}

/**
 * enum udma_desc_type - udma descriptor type
 * @PCIE_READ_WRITE: Only for Bulk mode
 * @PCIE_PREFETCH: For Scatter and Gather mode
 * @PCIE_POSTWRITE: For Scatter and Gather mode
 */
enum udma_desc_type {
	PCIE_READ_WRITE,
	PCIE_PREFETCH,
	PCIE_POSTWRITE,
};

static u8 set_contro_byte(enum udma_desc_type desc_type, bool interrupt_value,
			  bool continue_on_value)
{
	return FIELD_PREP(CTRL_BYTE_INT_MASK, interrupt_value) |
	       FIELD_PREP(CTRL_BYTE_CONTINUITY_MASK, desc_type) |
	       FIELD_PREP(CTRL_BYTE_CONTINUE_MASK, continue_on_value);
}

static bool is_buswidth_valid(u8 buswidth)
{
	if (buswidth != DMA_SLAVE_BUSWIDTH_4_BYTES &&
	    buswidth != DMA_SLAVE_BUSWIDTH_8_BYTES)
		return false;
	else
		return true;
}

static int cdns_udma_device_config(struct dma_chan *dma_chan,
				   struct dma_slave_config *config)
{
	struct cdns_udma_chan *chan = to_cdns_udma_chan(dma_chan);
	struct cdns_udma_dev *udma_dev = chan->udma_dev;
	struct cdns_udma_driverdata const *data = udma_dev->data;

	if (data) {
		if (config->direction == DMA_DEV_TO_MEM &&
		    data->external_alignment &&
		    !IS_ALIGNED(config->src_addr, data->external_alignment)) {
			dev_err(udma_dev->dev,
				"%s: mis-aligned external addr\n", __func__);
			return -EINVAL;
		}
		if (config->direction == DMA_MEM_TO_DEV &&
		    data->local_alignment &&
		    !IS_ALIGNED(config->dst_addr, data->local_alignment)) {
			dev_err(udma_dev->dev,
				"%s: mis-aligned external addr\n", __func__);
			return -EINVAL;
		}
	}

	/* Reject definitely invalid configurations */
	if (!is_buswidth_valid(config->src_addr_width) ||
	    !is_buswidth_valid(config->dst_addr_width))
		return -EINVAL;

	memcpy(&chan->config, config, sizeof(*config));
	return 0;
}

static void cdns_udma_free_desc(struct cdns_udma_chan *chan,
				struct cdns_desc *desc)
{
	int i = 0;

	pr_debug("%s: chan idx %d, caller is %pS\n", __func__, chan->idx,
		 __builtin_return_address(0));
	for (i = 0; i < desc->count; i++)
		dma_pool_free(chan->desc_pool, desc->node[i].lli,
			      desc->node[i].lli_dma_addr);

	kfree(desc);
}

static void cdns_udma_vchan_free_desc(struct virt_dma_desc *vd)
{
	cdns_udma_free_desc(to_cdns_udma_chan(vd->tx.chan),
			    to_cdns_udma_desc(vd));
}

static struct cdns_desc *cdns_udma_alloc_desc(struct cdns_udma_chan *chan,
					      u32 count)
{
	struct cdns_desc *desc;
	int i;
	struct device *dev = chan->udma_dev->dev;
	gfp_t mem_flags;

	dev_dbg(dev, "%s: alloc 0x%x descs\n", __func__, count);
	desc = kzalloc(struct_size(desc, node, count), GFP_NOWAIT);
	if (!desc)
		return NULL;

	if (count >= 0x1000)
		mem_flags = GFP_KERNEL;
	else
		mem_flags = GFP_NOWAIT;

	for (i = 0; i < count; i++) {
		desc->node[i].lli =
			dma_pool_zalloc(chan->desc_pool, mem_flags,
					&desc->node[i].lli_dma_addr);
		if (!desc->node[i].lli)
			goto err;

		/* Clear attr */
		/* If sys_attr is not cleared, OB will get corrupt data */
		writel(0, &desc->node[i].lli->sys_attr);
		writel(0, &desc->node[i].lli->ext_attr);

		/* Clear status */
		writeb(0, &desc->node[i].lli->status.sys_status);
		writeb(0, &desc->node[i].lli->status.ext_status);
		writeb(0, &desc->node[i].lli->status.chnl_status);
	}

	desc->count = count;

	return desc;

err:
	dev_err(dev, "Failed to allocate descriptor\n");
	while (--i >= 0)
		dma_pool_free(chan->desc_pool, desc->node[i].lli,
			      desc->node[i].lli_dma_addr);
	kfree(desc);
	return NULL;
}

static void cdns_udma_setup_lli(struct cdns_udma_chan *chan,
				struct cdns_desc *desc, u32 index,
				dma_addr_t external_addr, dma_addr_t local_addr,
				u32 len, bool is_last,
				enum udma_desc_type desc_type,
				unsigned long flags)

{
	struct cdns_udma_lli *lli;
	u32 next = index + 1;
	u8 ctrl_bits;

	lli = desc->node[index].lli;

	lli->sys_lo_addr = lower_32_bits(local_addr);
	lli->sys_hi_addr = upper_32_bits(local_addr);

	lli->ext_lo_addr = lower_32_bits(external_addr);
	lli->ext_hi_addr = upper_32_bits(external_addr);

	if (is_last) {
		if (flags & DMA_PREP_INTERRUPT)
			ctrl_bits = set_contro_byte(
				desc_type, INTERRUPT,
				DONT_CONTINUE_TO_EXECUTE_LINKED_LIST);
		else
			ctrl_bits = set_contro_byte(
				desc_type, DONT_INTERRUPT,
				DONT_CONTINUE_TO_EXECUTE_LINKED_LIST);
		lli->next = 0;
		lli->next_hi_addr = 0;
	} else {
		ctrl_bits = set_contro_byte(desc_type, DONT_INTERRUPT,
					    CONTINUE_TO_EXECUTE_LINKED_LIST);
		lli->next = lower_32_bits(desc->node[next].lli_dma_addr);
		lli->next_hi_addr =
			upper_32_bits(desc->node[next].lli_dma_addr);
	}
	lli->size_and_ctrl_bits = FIELD_PREP(REG_FIELD_LENGTH_MASK, len) |
				  FIELD_PREP(REG_FIELD_CONTROL_MASK, ctrl_bits);
}

static int ambarella_get_mrrs(struct cdns_udma_dev *udma_dev)
{
	struct regmap *regmap;
	struct device *dev = udma_dev->dev;
	int status_offset, status, mrrs;

	/*
	 * XXX: show use more generic name like
	 * regmap instead of amb,scr-regmap
	 *
	 * This is PCIe controller status, so get it
	 * from PCIe controller node.
	 */
	regmap = syscon_regmap_lookup_by_phandle_args(
		dev->parent->of_node, "amb,scr-regmap", 1, &status_offset);
	if (!regmap) {
		dev_err(dev->parent, "%s: missing regmap for status reg\n",
			__func__);
		return -EINVAL;
	}

	regmap_read(regmap, status_offset, &status);
	/* TODO: don't hardcode, use of data or soc data instead */
	mrrs = FIELD_GET(GENMASK(14, 12), status);
	return 128 * (1 << mrrs);
}

static int div_ceil(int numerator, int denominator)
{
	int rem = do_div(numerator, denominator);

	return numerator + !!rem;
}

static struct dma_async_tx_descriptor *
cdns_udma_setup_bulk_mode(struct cdns_udma_chan *chan,
			  struct scatterlist *local_sgl, unsigned int sg_len,
			  enum dma_transfer_direction direction,
			  void *bulk_context, unsigned long flags)
{
	int ret, i, desc_index = 0;
	struct cdns_desc *desc = chan->desc;
	dma_addr_t local_addr, external_addr;
	struct cdns_udma_dev *udma_dev = chan->udma_dev;
	struct device *dev = udma_dev->dev;
	int local_sg_nents = sg_len;
	struct scatterlist *local_sg, *external_sg,
		*external_sgl = bulk_context;
	int external_sg_nents = sg_nents(external_sgl);
	struct cdns_udma_driverdata const *data = udma_dev->data;
	u32 local_alignment = data ? data->local_alignment : 0;
	u32 external_alignment = data ? data->external_alignment : 0;
	int quirks = 0;
	int chunk_size, mrrs = -1, subchunk_size;
	int rest_size, quirk_mrrs_nr_desc = 0;
	bool is_last_desc;
	bool ob_quirk_bulk_mrrs;
	bool ib_quirk_bulk_mrrs;
	bool need_quirk_bulk_mrrs;

	if (udma_dev->data)
		quirks = udma_dev->data->quirks;
	ob_quirk_bulk_mrrs =
		(direction == DMA_MEM_TO_DEV &&
		 quirks & CDNS_UDMA_OB_QUIRK_BULK_MRRS && enable_ob_mrrs_quirk);
	ib_quirk_bulk_mrrs =
		(direction == DMA_DEV_TO_MEM &&
		 quirks & CDNS_UDMA_IB_QUIRK_BULK_MRRS && enable_ib_mrrs_quirk);
	need_quirk_bulk_mrrs = ob_quirk_bulk_mrrs || ib_quirk_bulk_mrrs;

	if (need_quirk_bulk_mrrs) {
		if (!udma_dev->data) {
			dev_err(dev, "%s: invalid udma_dev->data\n", __func__);
			return NULL;
		}
		mrrs = udma_dev->data->get_mrrs(udma_dev);
		if (mrrs < 0)
			return NULL;
		dev_dbg(dev, "mrrs is %d Byte\n", mrrs);
	}

	if (sg_len != sg_nents(local_sgl)) {
		dev_err(dev, "%s: why sg nents mismatch(%x vs %x)?", __func__,
			sg_len, sg_nents(local_sgl));
		return NULL;
	}

	if (external_sg_nents != local_sg_nents) {
		dev_err(dev,
			"%s: why local(%d) and externel(%d) buffer have different number of entries!!",
			__func__, local_sg_nents, external_sg_nents);
		return NULL;
	}

	if (!bulk_context) {
		dev_err(dev,
			"You should use context/scatterlist to provide"
			"scatterlist external/peripherals address for bulk mode\n");
		return NULL;
	}

	if (need_quirk_bulk_mrrs)
		for_each_sg (local_sgl, local_sg, sg_len, i) {
			dev_dbg(dev, "%s: sg len is 0x%x, div_ceil is %x\n",
				__func__, sg_dma_len(local_sg),
				div_ceil(sg_dma_len(local_sg), mrrs));
			quirk_mrrs_nr_desc +=
				div_ceil(sg_dma_len(local_sg), mrrs);
		}

	if (need_quirk_bulk_mrrs)
		desc = cdns_udma_alloc_desc(chan, quirk_mrrs_nr_desc);
	else
		desc = cdns_udma_alloc_desc(chan, sg_len);

	if (!desc)
		return NULL;

	chan->desc = desc;

	desc->dir = direction == DMA_DEV_TO_MEM ? INBOUND : OUTBOUND;

	for (i = 0, local_sg = local_sgl, external_sg = external_sgl;
	     i < sg_len; i++, local_sg = sg_next(local_sg),
	    external_sg = sg_next(external_sg)) {
		chunk_size = sg_dma_len(local_sg);
		if (chunk_size != sg_dma_len(external_sg)) {
			dev_err(dev,
				"%s: invalid local(0x%x) or external(0x%x) buffer length",
				__func__, chunk_size, sg_dma_len(external_sg));
			return NULL;
		}

		external_addr = sg_dma_address(external_sg);
		local_addr = sg_dma_address(local_sg);
		if (need_quirk_bulk_mrrs) {
			rest_size = chunk_size;
			do {
				if (local_alignment &&
				    !IS_ALIGNED(local_addr, local_alignment)) {
					dev_err(dev,
						"%s: mis-aligned local addr\n",
						__func__);
					return NULL;
				}

				if (external_alignment &&
				    !IS_ALIGNED(external_addr,
						external_alignment)) {
					dev_err(dev,
						"%s: mis-aligned external addr\n",
						__func__);
					return NULL;
				}

				if ((i == sg_len - 1) && rest_size <= mrrs)
					is_last_desc = true;
				else
					is_last_desc = false;

				subchunk_size =
					min(rest_size, min(chunk_size, mrrs));

				if (subchunk_size == 0)
					dev_warn(
						chan->udma_dev->dev,
						"Note: len is 0, uDMA will xfer max size: %x",
						BULK_XFER_MAX_SIZE_PER_DESC);

				cdns_udma_setup_lli(chan, desc, desc_index++,
						    external_addr, local_addr,
						    subchunk_size, is_last_desc,
						    PCIE_READ_WRITE, flags);
				dev_dbg(dev,
					"%s: sg idx: %d, desc idx: %d, desc xfer size: 0x%x, sg chunk size is 0x%x, sg chunk idx: 0x%x, external_addr is %llx, local_addr is %llx\n",
					__func__, i, desc_index - 1,
					subchunk_size, chunk_size, i,
					external_addr, local_addr);
				rest_size -= subchunk_size;
				local_addr += subchunk_size;
				external_addr += subchunk_size;
			} while (rest_size > 0);
		} else {
			if (local_alignment &&
			    !IS_ALIGNED(local_addr, local_alignment)) {
				dev_err(dev, "%s: mis-aligned local addr\n",
					__func__);
				return NULL;
			}

			if (external_alignment &&
			    !IS_ALIGNED(external_addr, external_alignment)) {
				dev_err(dev, "%s: mis-aligned external addr\n",
					__func__);
				return NULL;
			}

			if (chunk_size > BULK_XFER_MAX_SIZE_PER_DESC) {
				dev_err(chan->udma_dev->dev,
					"%s: invalid xfer size %x for bulk mode\n",
					__func__, chunk_size);
				return NULL;
			}

			if (chunk_size == 0)
				dev_warn(
					chan->udma_dev->dev,
					"Note: sg entry %d, len is 0, uDMA will xfer max size: %x",
					i, BULK_XFER_MAX_SIZE_PER_DESC);

			cdns_udma_setup_lli(chan, desc, desc_index++,
					    external_addr, local_addr,
					    chunk_size, i == sg_len - 1,
					    PCIE_READ_WRITE, flags);
		}
	}

	dev_dbg(dev, "%s: 0x%x descriptors are used\n", __func__, desc_index);

	ret = cdns_udma_unmask_irq(chan);
	if (ret) {
		cdns_udma_free_desc(chan, desc);
		return NULL;
	}
	chan->in_use = true;
	return vchan_tx_prep(&chan->vc, &desc->vd, flags);
}

static struct dma_async_tx_descriptor *cdns_udma_setup_scatter_or_gather_mode(
	struct cdns_udma_chan *chan, struct scatterlist *sgl,
	unsigned int sg_len, enum dma_transfer_direction direction,
	unsigned long flags)
{
	int ret, desc_index = 0, i;
	unsigned long sg_total_length = 0;
	bool is_last_desc;
	struct scatterlist *sg;
	struct cdns_desc *desc = chan->desc;
	dma_addr_t local_addr, external_addr;
	struct cdns_udma_dev *udma_dev = chan->udma_dev;
	struct device *dev = udma_dev->dev;
	struct cdns_udma_driverdata const *data = udma_dev->data;
	u32 local_alignment = data ? data->local_alignment : 0;
	int quirks = 0;
	int chunk_size, mrrs = -1, subchunk_size;
	int rest_size, quirk_mrrs_nr_desc = 0;
	bool ob_quirk_bulk_mrrs;
	bool ib_quirk_bulk_mrrs;
	bool need_quirk_bulk_mrrs;

	if (udma_dev->data)
		quirks = udma_dev->data->quirks;
	ob_quirk_bulk_mrrs =
		(direction == DMA_MEM_TO_DEV &&
		 quirks & CDNS_UDMA_OB_QUIRK_BULK_MRRS && enable_ob_mrrs_quirk);
	ib_quirk_bulk_mrrs =
		(direction == DMA_DEV_TO_MEM &&
		 quirks & CDNS_UDMA_IB_QUIRK_BULK_MRRS && enable_ib_mrrs_quirk);
	need_quirk_bulk_mrrs = ob_quirk_bulk_mrrs || ib_quirk_bulk_mrrs;

	if (need_quirk_bulk_mrrs) {
		if (!udma_dev->data) {
			dev_err(dev, "%s: invalid udma_dev->data\n", __func__);
			return NULL;
		}
		mrrs = udma_dev->data->get_mrrs(udma_dev);
		if (mrrs < 0)
			return NULL;
		dev_dbg(dev, "mrrs is %d Byte\n", mrrs);
	}

	external_addr = direction == DMA_DEV_TO_MEM ? chan->config.src_addr :
						      chan->config.dst_addr;

	if (chan->config.direction != direction)
		dev_dbg(dev, "%s: mismatch with sconf dir\n", __func__);

	for_each_sg (sgl, sg, sg_len, i) {
		if (local_alignment &&
		    !IS_ALIGNED(sg_dma_address(sg), local_alignment)) {
			dev_err(dev, "%s: %d: mis-aligned local addr\n",
				__func__, __LINE__);
			return NULL;
		}
		sg_total_length += sg_dma_len(sg);
		if (need_quirk_bulk_mrrs)
			quirk_mrrs_nr_desc += div_ceil(sg_dma_len(sg), mrrs);
	}

	/* both scatter and gather need sg_len + 1 descriptors */
	if (need_quirk_bulk_mrrs)
		desc = cdns_udma_alloc_desc(chan, quirk_mrrs_nr_desc + 1);
	else
		desc = cdns_udma_alloc_desc(chan, sg_len + 1);

	if (!desc)
		return NULL;

	desc->dir = direction == DMA_DEV_TO_MEM ? INBOUND : OUTBOUND;
	chan->desc = desc;

	if (sg_total_length > udma_dev->partition_size) {
		dev_err(dev,
			"for scatter/gather mode, total xfer size(0x%lx)"
			"shouldn't be over partition_size(0x%x)"
			"you could use udma's bulk mode instead\n",
			sg_total_length, udma_dev->partition_size);
		return NULL;
	}

	if (direction == DMA_DEV_TO_MEM) {
		cdns_udma_setup_lli(chan, desc, desc_index++, external_addr, 0,
				    sg_total_length, false, PCIE_PREFETCH,
				    flags);
		dev_dbg(dev, "%s: total_length is %lx\n", __func__,
			sg_total_length);
	}

	for_each_sg (sgl, sg, sg_len, i) {
		local_addr = sg_dma_address(sg);
		chunk_size = sg_dma_len(sg);

		if (need_quirk_bulk_mrrs) {
			rest_size = chunk_size;
			do {
				if ((i == sg_len - 1) && rest_size <= mrrs &&
				    direction == DMA_DEV_TO_MEM)
					is_last_desc = true;
				else
					is_last_desc = false;

				subchunk_size =
					min(rest_size, min(chunk_size, mrrs));

				if (local_alignment &&
				    !IS_ALIGNED(local_addr, local_alignment)) {
					dev_err(dev,
						"%s: %d: mis-aligned local addr\n",
						__func__, __LINE__);
					return NULL;
				}

				chan->in_use = true;
				cdns_udma_setup_lli(
					chan, desc, desc_index++, 0, local_addr,
					subchunk_size, is_last_desc,
					direction == DMA_DEV_TO_MEM ?
						PCIE_POSTWRITE :
						PCIE_PREFETCH,
					flags);
				dev_dbg(dev,
					"%s: sg idx: %d, desc idx: %d, desc xfer size: 0x%x, sg chunk size is 0x%x\n",
					__func__, i, desc_index - 1,
					subchunk_size, chunk_size);
				rest_size -= subchunk_size;
				local_addr += subchunk_size;
			} while (rest_size > 0);
		} else {
			if (direction == DMA_DEV_TO_MEM && (i == sg_len - 1))
				is_last_desc = true;
			else
				is_last_desc = false;

			cdns_udma_setup_lli(chan, desc, desc_index++, 0,
					    local_addr, sg_dma_len(sg),
					    is_last_desc,
					    direction == DMA_DEV_TO_MEM ?
						    PCIE_POSTWRITE :
						    PCIE_PREFETCH,
					    flags);
		}

		dev_dbg(dev, "%s: this sg length is %x\n", __func__,
			sg_dma_len(sg));
	}

	if (direction == DMA_MEM_TO_DEV) {
		cdns_udma_setup_lli(chan, desc, desc_index++, external_addr, 0,
				    sg_total_length, true, PCIE_POSTWRITE,
				    flags);
		pr_debug("%s: total_length is %lx\n", __func__,
			 sg_total_length);
	}

	ret = cdns_udma_unmask_irq(chan);
	if (ret) {
		cdns_udma_free_desc(chan, desc);
		return NULL;
	}
	chan->in_use = true;
	return vchan_tx_prep(&chan->vc, &desc->vd, flags);
}

/**
 * cdns_udma_prep_slave_sg - prepare for memory2dev/dev2mem
 * @bulk_context: scatterlist for slave addresses.
 *
 * uDMA supports three different xfer modes:
 * - Scatter mode
 * - Gather mode
 * - Bulk mode
 *
 * By default, Scatter/Gather modes are used, which only
 * servers for very small buffer.
 *
 * If your buffer is/are large, please pass sg to bulk_context
 */
static struct dma_async_tx_descriptor *
cdns_udma_prep_slave_sg(struct dma_chan *dma_chan, struct scatterlist *sgl,
			unsigned int sg_len,
			enum dma_transfer_direction direction,
			unsigned long flags, void *bulk_context)
{
	struct cdns_udma_chan *chan = to_cdns_udma_chan(dma_chan);

	if (bulk_context)
		return cdns_udma_setup_bulk_mode(chan, sgl, sg_len, direction,
						 bulk_context, flags);
	else
		return cdns_udma_setup_scatter_or_gather_mode(chan, sgl, sg_len,
							      direction, flags);
}

static void cdns_udma_start_transfer(struct cdns_udma_chan *chan)
{
	struct cdns_udma_dev *udma_dev = chan->udma_dev;
	struct cdns_desc *desc;
	struct virt_dma_desc *vd = vchan_next_desc(&chan->vc);
	struct cdns_udma_lli *lli;

	if (!vd) {
		dev_err(udma_dev->dev,
			"invalid virt_dma_desc: chan is no.%x!\n", chan->idx);
		return;
	}

	list_del(&vd->node);
	desc = to_cdns_desc(vd);

	lli = desc->node[0].lli;

	dev_dbg(udma_dev->dev, "%s %d, chan->idx is %x\n", __func__, __LINE__,
		chan->idx);

	if (chan->idx > udma_dev->chan_num) {
		dev_err(udma_dev->dev, "invalid channel index: %x!\n",
			chan->idx);
		return;
	}

	/* Set up starting descriptor */
	writel((u32)(lower_32_bits(desc->node[0].lli_dma_addr)),
	       udma_dev->dma_base + CHANNEL_SP_L_OFFSET(chan->idx));
	writel((u32)(upper_32_bits(desc->node[0].lli_dma_addr)),
	       udma_dev->dma_base + CHANNEL_SP_U_OFFSET(chan->idx));

	/* Clear channel attr */
	writel(0, udma_dev->dma_base + CHANNEL_ATTR_L_OFFSET(chan->idx));
	writel(0, udma_dev->dma_base + CHANNEL_ATTR_U_OFFSET(chan->idx));

	/* let's go */
	if (desc->dir == OUTBOUND)
		writel(DMA_OB_CMD,
		       udma_dev->dma_base + CHANNEL_CTRL_OFFSET(chan->idx));
	else
		writel(DMA_IB_CMD,
		       udma_dev->dma_base + CHANNEL_CTRL_OFFSET(chan->idx));
	atomic_inc(&chan->refcount);
}

/*
 * WOCLR: write bit id to clear interrupt status,
 * otherwise will always get triggered
 */

static void cdns_udma_clear_irq(int id, void *base)
{
	unsigned long val;

	val = readl(base + COMMON_UDMA_INT_OFFSET);
	set_bit(id, &val);
	writel(val, base + COMMON_UDMA_INT_OFFSET);
}

static void cdns_udma_issue_pending(struct dma_chan *c)
{
	struct cdns_udma_chan *chan = to_cdns_udma_chan(c);
	unsigned long flags;

	spin_lock_irqsave(&chan->vc.lock, flags);

	if (vchan_issue_pending(&chan->vc))
		cdns_udma_start_transfer(chan);

	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static int cdns_udma_dump_error(struct cdns_desc *desc,
				struct cdns_udma_dev *udma_dev,
				struct cdns_udma_chan *chan, int id)
{
	int i;
	struct cdns_udma_lli *lli;
	bool has_error = false;

	pr_debug(
		"%s: chan id is %d, desc->count is %d, uc ib: %x, c ib: %x, uc ob: %x, c ob: %x\n",
		__func__, id, desc->count,
		readl(udma_dev->dma_base +
		      COMMON_UDMA_IB_ECC_UNCORRECTABLE_ERRORS_OFFSET),
		readl(udma_dev->dma_base +
		      COMMON_UDMA_IB_ECC_CORRECTABLE_ERRORS_OFFSET),
		readl(udma_dev->dma_base +
		      COMMON_UDMA_OB_ECC_UNCORRECTABLE_ERRORS_OFFSET),
		readl(udma_dev->dma_base +
		      COMMON_UDMA_OB_ECC_CORRECTABLE_ERRORS_OFFSET));
	for (i = 0; i < desc->count; i++) {
		lli = chan->desc->node[i].lli;
		dev_dbg(udma_dev->dev,
			"%s: desc %d, axi attr: %x, pci attr: %x, chnl stat %x, sys stat %x, ext stat %x\n",
			__func__, i, lli->sys_attr, lli->ext_attr,
			lli->status.chnl_status, lli->status.sys_status,
			lli->status.ext_status);
		if (lli->status.chnl_status != 1) {
			if (lli->status.chnl_status == 0) {
				dev_dbg(udma_dev->dev,
					"Descriptor action is not completed, but why?\n");
				continue;
			}
			dev_err(udma_dev->dev,
				"channel %x lli %x channel status error: %x\n",
				id, i, lli->status.chnl_status);
			has_error = true;
		}

		if (lli->status.ext_status != 0) {
			dev_err(udma_dev->dev,
				"channel %x lli %x PCIe Bus Status error: %x\n",
				id, i, lli->status.ext_status);
			has_error = true;
		}
		if (lli->status.sys_status != 0) {
			dev_err(udma_dev->dev,
				"channel %x lli %x Local Bus Status error: %x\n",
				id, i, lli->status.sys_status);
			has_error = true;
		}
	}
	if (has_error)
		return -1;
	return 0;
}

static irqreturn_t cdns_udma_irq(int irq, void *data)
{
	u32 id;
	struct cdns_udma_dev *udma_dev = data;
	struct cdns_udma_chan *chan;
	struct cdns_desc *desc;
	const unsigned long common_udma_int =
		readl(udma_dev->dma_base + COMMON_UDMA_INT_OFFSET);
	int i;
	struct cdns_udma_lli *last_lli;

	/*
	 * FIXME:
	 * if multiple channels were used for a single transfer and
	 * issued in parallel, the 'common_udma_int' in the ISR might
	 * miss some channel masks. For example, if four channels were
	 * used for the transfer, only one IRQ would get triggered,
	 * and 'common_udma_int' would only have channel 0 or 0-1,
	 * or 0-2. Consequently, some channels wouldn't get freed by
	 * vchan_cookie_complete, and errors like
	 *
	 * "cdns_udma ff30600000.pcie-udma: dma_pool_destroy
	 * ff30600000.pcie-udma, 00000000128c68d1 busy"
	 *
	 * would occur.
	 *
	 * it seems to be a hardware race condition to me.
	 */
	for_each_set_bit (id, &common_udma_int, COMMON_UDMA_INT_BITS) {
		/* 0..7 are done interrupts, 7-13 are error interrupts */
		if (id >= CDNS_UDMA_MAX_CHANNELS) {
			/* Get Error interrupt */
			chan = &udma_dev->chan[id - CDNS_UDMA_MAX_CHANNELS];
			desc = chan->desc;

			dev_err(udma_dev->dev, "%s: get error interrupt!\n",
				__func__);
			cdns_udma_clear_irq(id, udma_dev->dma_base);
			cdns_udma_dump_error(desc, udma_dev, chan,
					     id - CDNS_UDMA_MAX_CHANNELS);
			goto error;
		} else {
			chan = &udma_dev->chan[id];

			cdns_udma_clear_irq(id, udma_dev->dma_base);

			if (!chan) {
				dev_err(udma_dev->dev,
					"uDMA channel not initialized\n");
				goto error;
			}
			desc = chan->desc;
			if (!desc) {
				dev_err(udma_dev->dev,
					"uDMA channel desc not initialized\n");
				goto error;
			}
			if (chan->in_use) {
				atomic_dec(&chan->refcount);
				chan->in_use = false;

				dev_dbg(udma_dev->dev,
					"%s %d, let's free chan %d\n", __func__,
					__LINE__, chan->idx);
				tasklet_schedule(&chan->irqtask);
				cdns_udma_dump_error(desc, udma_dev, chan, id);
			}
		}
	}

	/*
	 * Let's use chnl_status as workaround to free channel
	 */
	for (i = 0; i < udma_dev->chan_num; i++) {
		chan = &udma_dev->chan[i];
		desc = chan->desc;
		if (!desc)
			continue;
		last_lli = desc->node[desc->count - 1].lli;

		/* 1 means "Descriptor action completed". */
		if (chan->in_use && last_lli->status.chnl_status == 1) {
			dev_dbg(udma_dev->dev, "%s %d, let's free chan %d\n",
				__func__, __LINE__, chan->idx);
			chan->in_use = false;
			atomic_dec(&chan->refcount);
			tasklet_schedule(&chan->irqtask);
			cdns_udma_dump_error(desc, udma_dev, chan, i);
		}
	}

	return IRQ_HANDLED;
error:
	if (desc)
		desc->status = DMA_ERROR;
	return IRQ_NONE;
}

static void cdns_udma_task(struct tasklet_struct *task)
{
	struct cdns_udma_chan *chan = from_tasklet(chan, task, irqtask);
	struct cdns_desc *desc;
	unsigned long flags;

	desc = chan->desc;

	spin_lock_irqsave(&chan->vc.lock, flags);
	desc->status = DMA_COMPLETE;
	pr_debug("complete vchan: chan id is %d\n", chan->idx);
	/* TODO: invoke callback for epf completion, may sleep, so not suitable for tasklet */
	vchan_cookie_complete(&chan->desc->vd);
	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

/* This function enables all hw channels in a device */
static int cdns_udma_enable_hw_channels(struct cdns_udma_dev *udma_dev)
{
	int ret;
	int i;

	ret = devm_request_irq(udma_dev->dev, udma_dev->irq, cdns_udma_irq, 0,
			       "cdns,udma", udma_dev);
	if (ret) {
		dev_err(udma_dev->dev,
			"fail to request irq for udma channel!\n");
		return ret;
	}

	for (i = 0; i < udma_dev->chan_num; i++) {
		udma_dev->chan[i].idx = i;
		udma_dev->chan[i].udma_dev = udma_dev;
		udma_dev->chan[i].vc.desc_free = cdns_udma_vchan_free_desc;
		vchan_init(&udma_dev->chan[i].vc, &udma_dev->dma_dev);
	}

	return 0;
}

static int cdns_udma_alloc_chan_resources(struct dma_chan *dma_chan)
{
	struct cdns_udma_chan *chan = to_cdns_udma_chan(dma_chan);
	int ret = 0;

	chan->desc_pool =
		dmam_pool_create(dev_name(chan->udma_dev->dev),
				 chan->udma_dev->dev,
				 sizeof(struct cdns_udma_lli),
				 __alignof__(struct cdns_udma_lli), 0);
	if (!chan->desc_pool) {
		dev_err(chan->udma_dev->dev,
			"failed to allocate descriptor pool\n");
		return -ENOMEM;
	}

	atomic_set(&chan->refcount, 0);
	tasklet_setup(&chan->irqtask, cdns_udma_task);

	/*  TODO: disable irq here
	 * ret = cdns_udma_disable_chan(chan);
	 */

	return ret;
}

static void cdns_udma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct cdns_udma_chan *chan = to_cdns_udma_chan(dma_chan);
	struct device *dev = &dma_chan->dev->device;

	chan->in_use = false;
	cdns_udma_mask_irq(chan);
	dev_dbg(dev, "Freeing channel %d\n", chan->idx);

	/*
	 * TODO: if busy, impelement cdns_udma_stop(chan)
	 * to disable chan and clear interrupt,
	 * see stm32_mdma_free_chan_resources
	 */

	WARN(atomic_read(&chan->refcount), "chan %d: unbalanced count\n",
	     chan->idx);
	vchan_free_chan_resources(to_virt_chan(dma_chan));
	dmam_pool_destroy(chan->desc_pool);
	chan->desc_pool = NULL;
	chan->desc = NULL;
	tasklet_kill(&chan->irqtask);
}

static const struct cdns_udma_driverdata cv5_data = {
	.external_alignment = SZ_16,
	.local_alignment = SZ_16,
	.quirks = CDNS_UDMA_IB_QUIRK_BULK_MRRS | CDNS_UDMA_OB_QUIRK_BULK_MRRS,
	.get_mrrs = ambarella_get_mrrs,
};

static const struct cdns_udma_driverdata cv3_data = {
	.external_alignment = SZ_16,
	.local_alignment = SZ_16,
	.quirks = CDNS_UDMA_IB_QUIRK_BULK_MRRS | CDNS_UDMA_OB_QUIRK_BULK_MRRS,
	.get_mrrs = ambarella_get_mrrs,
};

#ifdef CONFIG_DEBUG_FS
static int counter_udma_debugfs_show(struct seq_file *s, void *data)
{
	struct cdns_udma_dev *udma_dev = s->private;
	int i;

	for (i = 0; i < udma_dev->chan_num; i++)
		seq_printf(s, "chan %d ref counter: %d\n", i,
			   atomic_read(&udma_dev->chan[i].refcount));
	return 0;
}

static int errors_udma_debugfs_show(struct seq_file *s, void *data)
{
	struct cdns_udma_dev *udma_dev = s->private;

	seq_printf(
		s,
		"ib ecc uncorrectable errors: %x\nib ecc correctable errors: %x\nob ecc uncorrectable errors: %x\nob ecc correctable errors: %x\n",
		readl(udma_dev->dma_base +
		      COMMON_UDMA_IB_ECC_UNCORRECTABLE_ERRORS_OFFSET),
		readl(udma_dev->dma_base +
		      COMMON_UDMA_IB_ECC_CORRECTABLE_ERRORS_OFFSET),
		readl(udma_dev->dma_base +
		      COMMON_UDMA_OB_ECC_UNCORRECTABLE_ERRORS_OFFSET),
		readl(udma_dev->dma_base +
		      COMMON_UDMA_OB_ECC_CORRECTABLE_ERRORS_OFFSET));
	return 0;
}

static int common_udma_debugfs_show(struct seq_file *s, void *data)
{
	struct cdns_udma_dev *udma_dev = s->private;
	u32 common_udma_config, common_udma_cap_ver;

	common_udma_config =
		readl(udma_dev->dma_base + COMMON_UDMA_CONFIG_OFFSET);
	common_udma_cap_ver =
		readl(udma_dev->dma_base + COMMON_UDMA_CAP_VER_OFFSET);

	seq_printf(
		s,
		"dma channel number is 0x%lx\npartition size is 0x%x\n"
		"partition number is 0x%lx\nsys addr width %s 32-bits\n"
		"sys attr width %s 32-bits\next addr width %s 32-bits\n"
		"ext attr width %s 32-bits\ncommon_udma_cap_ver is v%ld.%ld\n",
		FIELD_GET(COMMON_UDMA_CONFIG_NUM_CHANNELS_MASK,
			  common_udma_config),
		udma_dev->partition_size,
		FIELD_GET(COMMON_UDMA_CONFIG_NUM_PARTITIONS_MASK,
			  common_udma_config),
		FIELD_GET(COMMON_UDMA_CONFIG_SYS_AW_GT_32_MASK,
			  common_udma_config) ?
			">" :
			"<",
		FIELD_GET(COMMON_UDMA_CONFIG_SYS_TW_GT_32_MASK,
			  common_udma_config) ?
			">" :
			"<",
		FIELD_GET(COMMON_UDMA_CONFIG_EXT_AW_GT_32_MASK,
			  common_udma_config) ?
			">" :
			"<",
		FIELD_GET(COMMON_UDMA_CONFIG_EXT_TW_GT_32_MASK,
			  common_udma_config) ?
			">" :
			"<",
		FIELD_GET(COMMON_UDMA_CAP_VER_MIN_VER_MASK,
			  common_udma_cap_ver),
		FIELD_GET(COMMON_UDMA_CAP_VER_MAJ_VER_MASK, common_udma_cap_ver)

	);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(common_udma_debugfs);
DEFINE_SHOW_ATTRIBUTE(errors_udma_debugfs);
DEFINE_SHOW_ATTRIBUTE(counter_udma_debugfs);
static inline void init_udma_debugfs(struct cdns_udma_dev *udma_dev)
{
	struct dentry *debugfs;

	debugfs = debugfs_create_dir(dev_name(udma_dev->dev), NULL);
	debugfs_create_file("common_udma", 0400, debugfs, udma_dev,
			    &common_udma_debugfs_fops);
	debugfs_create_file("errors", 0400, debugfs, udma_dev,
			    &errors_udma_debugfs_fops);
	debugfs_create_file("refcounter", 0400, debugfs, udma_dev,
			    &counter_udma_debugfs_fops);
}
#else
static inline void init_udma_debugfs(struct cdns_udma_dev *udma_dev)
{
}
#endif
static const struct of_device_id cdns_udma_match[] = {
	{
		.compatible = "cdns,udma",
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, cdns_udma_match);

static const struct soc_device_attribute cdns_udma_soc_info[] = {
	{
		.soc_id = "cv5",
		.data = (void *)&cv3_data,
	},
	{
		.soc_id = "cv3",
		.data = (void *)&cv5_data,
	},
	{ /* sentinel */ },
};

static int cdns_udma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cdns_udma_dev *udma_dev;
	struct dma_device *dma_dev;
	int ret;
	u32 common_udma_config, common_udma_cap_ver;
	void __iomem *dma_base;
	int nr_channels;
	struct device_node *pcie_np;
	struct platform_device *parent_pdev;
	const struct soc_device_attribute *soc;
	const struct cdns_udma_driverdata *soc_data = NULL;

	spin_lock_init(&int_lock);
	spin_lock_init(&int_dis_lock);
	spin_lock_init(&int_ena_lock);

	pcie_np = of_parse_phandle(dev->of_node, "pcie-controller", 0);
	if (!pcie_np) {
		dev_err(dev, "pcie-controller is not specified\n");
		return -ENODEV;
	}
	parent_pdev = of_find_device_by_node(pcie_np);
	if (!parent_pdev) {
		dev_err(dev,
			"%s: failed to find pcie controller platform device!\n",
			__func__);
		return -ENODEV;
	}

	/*
	 * 1. set pcie controller as udma parent device, and use it to filter dma chan
	 * 2. udma cannot work as child of pcie controller node
	 *    because it sets interrupt/address/size cells for itself only and
	 *    don't consider its child
	 */
	dev->parent = &parent_pdev->dev;
	dev_dbg(dev, "%s %d: set parent to %s\n", __func__, __LINE__,
		dev_name(dev->parent));

	dma_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dma_base)) {
		dev_err(dev, "missing \"reg\"\n");
		return PTR_ERR(dma_base);
	}

	common_udma_cap_ver = readl(dma_base + COMMON_UDMA_CAP_VER_OFFSET);

	common_udma_config = readl(dma_base + COMMON_UDMA_CONFIG_OFFSET);

	if (FIELD_GET(COMMON_UDMA_CAP_VER_MIN_VER_MASK, common_udma_cap_ver) !=
		    1 ||
	    FIELD_GET(COMMON_UDMA_CAP_VER_MAJ_VER_MASK, common_udma_cap_ver) !=
		    0) {
		dev_err(dev,
			"Current version of driver only supports uDMA v1.0\n");
		return -ENXIO;
	}

	nr_channels = FIELD_GET(COMMON_UDMA_CONFIG_NUM_CHANNELS_MASK,
				common_udma_config);

	udma_dev = devm_kzalloc(dev, struct_size(udma_dev, chan, nr_channels),
				GFP_KERNEL);
	if (!udma_dev)
		return -EINVAL;

	/* If device_type is pci, the controller must role as RC instead of EP */
	if (of_node_is_type(dev->of_node, "pci"))
		udma_dev->is_rc = 1;

	soc = soc_device_match(cdns_udma_soc_info);
	if (soc) {
		soc_data = soc->data;
		udma_dev->data = soc_data;
		if (soc_data &&
		    soc_data->quirks & (CDNS_UDMA_OB_QUIRK_BULK_MRRS |
					CDNS_UDMA_IB_QUIRK_BULK_MRRS) &&
		    !soc_data->get_mrrs) {
			dev_err(dev,
				"CDNS_UDMA_OB_QUIRK_BULK_MRRS/CDNS_UDMA_IB_QUIRK_BULK_MRRS is specified,, but missing get_mrrs, please provide one to get mrrs");
			return -EINVAL;
		}
	}

	udma_dev->irq = platform_get_irq(pdev, 0);
	if (udma_dev->irq < 0)
		return udma_dev->irq;

	udma_dev->dev = &pdev->dev;

	udma_dev->chan_num = nr_channels;
	udma_dev->dma_base = dma_base;

	/*
	 * XXX: Doc has some something wrong, it should be:
	 * 128 * (2 ^ CONFIG.PZ) / 2
	 */
	udma_dev->partition_size =
		128 * 1 << (FIELD_GET(COMMON_UDMA_CONFIG_PARTITIONS_SIZE_MASK,
				      common_udma_config) -
			    1);

	platform_set_drvdata(pdev, udma_dev);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	dma_dev = &udma_dev->dma_dev;
	dma_cap_zero(dma_dev->cap_mask);
	dma_cap_set(DMA_SLAVE, dma_dev->cap_mask);
	dma_cap_set(DMA_PRIVATE, dma_dev->cap_mask);
	// TODO: impelment device_terminate_all
	dma_dev->device_prep_slave_sg = cdns_udma_prep_slave_sg;
	dma_dev->device_issue_pending = cdns_udma_issue_pending;
	dma_dev->device_config = cdns_udma_device_config;
	dma_dev->device_tx_status = dma_cookie_status;
	dma_dev->device_alloc_chan_resources = cdns_udma_alloc_chan_resources;
	dma_dev->device_free_chan_resources = cdns_udma_free_chan_resources;
	dma_dev->directions = BIT(DMA_MEM_TO_DEV) | BIT(DMA_DEV_TO_MEM);
	dma_dev->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES) |
				   BIT(DMA_SLAVE_BUSWIDTH_8_BYTES);
	dma_dev->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES) |
				   BIT(DMA_SLAVE_BUSWIDTH_8_BYTES);
	/*
	 * TODO: Let's mark residue as DMA_RESIDUE_GRANULARITY_DESCRIPTOR now,
	 * but need test for DMA_RESIDUE_GRANULARITY_SEGMENT support later.
	 */
	dma_dev->residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;

	dma_dev->dev = dev;
	INIT_LIST_HEAD(&dma_dev->channels);

	init_udma_debugfs(udma_dev);

	ret = cdns_udma_enable_hw_channels(udma_dev);
	if (ret < 0) {
		dev_err(dev, "failed to enable hw channel!\n");
		return ret;
	}

	if (ret)
		return ret;

	ret = dmaenginem_async_device_register(dma_dev);
	if (ret < 0)
		dev_err(dev, "failed to register device!\n");

	dev_info(dev, "Register successfully\n");
	return ret;
}

static int cdns_udma_remove(struct platform_device *pdev)
{
	int i;
	struct cdns_udma_dev *udma_dev = platform_get_drvdata(pdev);

	dma_async_device_unregister(&udma_dev->dma_dev);

	/* Mask all interrupts for this execution environment */
	for (i = 0; i < udma_dev->chan_num; i++)
		cdns_udma_mask_irq(&udma_dev->chan[i]);

	/* Make sure we won't have any further interrupts */
	devm_free_irq(udma_dev->dev, udma_dev->irq, udma_dev);

	return 0;
}

/*
 * Use platform_driver instead of pci_driver because if udma works for EP-mode
 * controller, the controller itself is also platform_driver, and there may be
 * no RC controller to scan pci device.
 *
 * XXX: but why dw-edma is a pci_driver? Is it because dw-edma works as PCI device?
 *
 */
static struct platform_driver cdns_udma_platform_driver = {
	.driver = {
		.name = "cdns_udma",
		.of_match_table = cdns_udma_match,
	},
	.probe = cdns_udma_probe,
	.remove = cdns_udma_remove,
};
module_platform_driver(cdns_udma_platform_driver);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("Candence uDMA controller driver");
MODULE_LICENSE("GPL v2");
