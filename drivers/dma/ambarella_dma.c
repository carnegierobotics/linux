/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DMA engine driver for Ambarella SoCs
 *
 * History:
 *	2012/05/10 - Ken He <jianhe@ambarella.com> created file
 *
 * Copyright (C) 2016-2048, Ambarella, Inc.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sys_soc.h>
#include <soc/ambarella/misc.h>
#include "dmaengine.h"
#include "ambarella_dma.h"

#define AMBARELLA_MAX_DESC_TRIALS		10
#define AMBARELLA_MAX_DESC_NUM			64
#define AMBARELLA_DMA_MAX_LENGTH		((1 << 22) - 1)

#define AMBARELLA_DMA_STA_ERR_MASK		( \
		DMA_CHANX_STA_DM		| \
		DMA_CHANX_STA_OE		| \
		DMA_CHANX_STA_DA		| \
		DMA_CHANX_STA_ME		| \
		DMA_CHANX_STA_BE		| \
		DMA_CHANX_STA_RWE		| \
		DMA_CHANX_STA_AE		)

/* The set of bus widths supported by the DMA controller */
#define AMBARELLA_DMA_BUSWIDTHS					\
		BIT(DMA_SLAVE_BUSWIDTH_UNDEFINED)		| \
		BIT(DMA_SLAVE_BUSWIDTH_1_BYTE)			| \
		BIT(DMA_SLAVE_BUSWIDTH_2_BYTES)			| \
		BIT(DMA_SLAVE_BUSWIDTH_4_BYTES)			| \
		BIT(DMA_SLAVE_BUSWIDTH_8_BYTES)

int ambarella_scm_get_dma_chan_id(uint32_t inst, uint32_t devid);

static int ambdma_proc_show(struct seq_file *m, void *v)
{
	struct ambdma_device *amb_dma = m->private;
	struct ambdma_chan *amb_chan;
	const char *sw_status;
	const char *hw_status;
	int i;

	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];

		switch(amb_chan->status) {
		case AMBDMA_STATUS_IDLE:
			sw_status = "idle";
			break;
		case AMBDMA_STATUS_BUSY:
			sw_status = "busy";
			break;
		case AMBDMA_STATUS_STOPPING:
			sw_status = "stopping";
			break;
		default:
			sw_status = "unknown";
			break;
		}

		if (ambdma_chan_is_enabled(amb_chan))
			hw_status = "running";
		else
			hw_status = "stopped";

		seq_printf(m, "channel %d:   %s, %d, %s, %s\n",
			amb_chan->chan.chan_id, dma_chan_name(&amb_chan->chan),
			amb_chan->chan.client_count, sw_status, hw_status);
	}

	seq_printf(m, "\nInput channel ID to stop specific dma channel:\n");
	seq_printf(m, "    example: echo 3 > dma\n\n");

	return 0;
}

static ssize_t ambdma_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	struct ambdma_device *amb_dma = PDE_DATA(file_inode(file));
	int id, ret;

	ret = kstrtouint_from_user(buffer, count, 0, &id);
	if (ret)
		return ret;

	if (id >= amb_dma->nr_channels) {
		pr_err("Invalid channel id\n");
		return -EINVAL;
	}

	dmaengine_terminate_all(&amb_dma->amb_chan[id].chan);

	return count;
}

static int ambdma_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambdma_proc_show, PDE_DATA(inode));
}

static const struct proc_ops proc_ambdma_fops = {
	.proc_open = ambdma_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_write = ambdma_proc_write,
	.proc_release = single_release,
};

static struct ambdma_desc *ambdma_alloc_desc(struct dma_chan *chan, gfp_t gfp_flags)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_device *amb_dma = amb_chan->amb_dma;
	struct ambdma_desc *amb_desc = NULL;
	dma_addr_t phys;

	amb_desc = kzalloc(sizeof(struct ambdma_desc), gfp_flags);
	if (!amb_desc)
		return NULL;

	amb_desc->lli = dma_pool_alloc(amb_dma->lli_pool, gfp_flags, &phys);
	if (!amb_desc->lli) {
		kfree(amb_desc);
		return NULL;
	}

	INIT_LIST_HEAD(&amb_desc->tx_list);
	dma_async_tx_descriptor_init(&amb_desc->txd, chan);
	/* txd.flags will be overwritten in prep functions */
	amb_desc->txd.flags = DMA_CTRL_ACK;
	amb_desc->txd.tx_submit = ambdma_tx_submit;
	amb_desc->txd.phys = phys;
	amb_desc->is_cyclic = 0;

	return amb_desc;
}

static void ambdma_free_desc(struct dma_chan *chan, struct ambdma_desc *amb_desc)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_device *amb_dma = amb_chan->amb_dma;

	dma_pool_free(amb_dma->lli_pool, amb_desc->lli, amb_desc->txd.phys);
	kfree(amb_desc);
}

static struct ambdma_desc *ambdma_get_desc(struct ambdma_chan *amb_chan)
{
	struct ambdma_desc *desc, *_desc;
	struct ambdma_desc *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);
	list_for_each_entry_safe(desc, _desc, &amb_chan->free_list, desc_node) {
		if (async_tx_test_ack(&desc->txd)) {
			list_del_init(&desc->desc_node);
			ret = desc;
			break;
		}
	}
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	/* no more descriptor available in initial pool: create one more */
	if (!ret) {
		ret = ambdma_alloc_desc(&amb_chan->chan, GFP_ATOMIC);
		if (ret) {
			spin_lock_irqsave(&amb_chan->lock, flags);
			amb_chan->descs_allocated++;
			spin_unlock_irqrestore(&amb_chan->lock, flags);
		} else {
			pr_err("%s: no more descriptors available\n", __func__);
		}
	}

	return ret;
}


static void ambdma_put_desc(struct ambdma_chan *amb_chan,
		struct ambdma_desc *amb_desc)
{
	unsigned long flags;

	if (amb_desc) {
		spin_lock_irqsave(&amb_chan->lock, flags);
		list_splice_init(&amb_desc->tx_list, &amb_chan->free_list);
		list_add_tail(&amb_desc->desc_node, &amb_chan->free_list);
		spin_unlock_irqrestore(&amb_chan->lock, flags);
	}
}

static void ambdma_return_desc(struct ambdma_chan *amb_chan,
		struct ambdma_desc *amb_desc)
{
	/* move children to free_list */
	list_splice_init(&amb_desc->tx_list, &amb_chan->free_list);
	/* move myself to free_list */
	list_move_tail(&amb_desc->desc_node, &amb_chan->free_list);
}

/*
 * The descriptor-processing sequency is as below:
 * 1. SW set DMA in descriptor mode and enable it.
 * 2. HW clear STATUS[26:0] to 0.([31:27] are cleared by SW)
 * 3. HW fetch descriptor and fill related registers in this order:
 *    DEST, SRC, DESC(for next descriptor), CTRL.
 * 4. When dma is done, HW writes report to dram, and then set OD bit(STATUS[27])
 *    and interrupt if ID=1, or set DD bit(STATUS[28] and interrupt if EOC=1.
 * 5. HW back to 2 if EOC=0.
 * 6. Done.
 *
 * Note:
 *   all of the channel registers except for DESC, CTRL[31:30] and STATUS[31:27]
 *   are unable to read during descriptor processing. If SW intends to read
 *   these registers, the behavior is as below:
 *
 *   When HW request desc (8B)    -> SW always get STATUS register value, DESC=desc
 *   When HW wait data1           -> SW always get DEST register value
 *   When HW request desc+8 (8B)  -> SW always get SRC register value, DESC=desc+8
 *   When HW wait data2           -> SW always get DEST register value
 *   When HW request desc+16 (8B) -> SW always get CTRL register value, DESC=desc+16
 *   When HW wait data3           -> SW always get SRC register value
 *   When HW update ctrl          -> SW always get CTRL register value
 *   When DMA running             -> normal
 *   When HW update report        -> SW always get STATUS status register value.
 *   PS: the register values above except for status are copied from descriptor.
 */
static struct ambdma_desc *ambdma_current_desc(struct ambdma_chan *amb_chan, u32 *status)
{
	struct ambdma_desc *amb_desc = NULL, *_amb_desc;
	u32 da, sta, ni_addr, ctr, _ctr;

	if (status)
		*status = 0;

	/* check desc reg */
	da = readl(dma_chan_da_reg(amb_chan));

	BUG_ON(list_empty(&amb_chan->work_list));
	list_for_each_entry(_amb_desc, &amb_chan->work_list, work_node) {
		if ((_amb_desc->txd.phys & GENMASK(31, 0)) == da) {
			amb_desc = _amb_desc;
			break;
		}
	}

	if (amb_desc == NULL)
		return NULL;

	if (amb_desc == list_first_entry(&amb_chan->work_list, struct ambdma_desc, work_node))
		amb_desc = list_last_entry(&amb_chan->work_list, struct ambdma_desc, work_node);
	else
		amb_desc = list_prev_entry(amb_desc, work_node);

	/* check status reg */
	sta = readl(dma_chan_sta_reg(amb_chan));
	if ((sta & AMBARELLA_DMA_STA_ERR_MASK)
		|| ((sta & AMBARELLA_DMA_MAX_LENGTH) > amb_desc->lli->xfr_count)
		|| ((sta & GENMASK(26, 0)) == (amb_desc->lli->attr & GENMASK(26, 0)))
		|| ((sta & GENMASK(26, 0)) == (amb_chan->rt_addr & GENMASK(26, 0))))
		return NULL;

	/* if dma between IO and memory, check more */
	if (amb_chan->rt_addr != ~(phys_addr_t)0) {
		/* check src or dest reg */
		if (amb_chan->rt_attr & DMA_DESC_RM)
			ni_addr = readl(dma_chan_dst_reg(amb_chan));
		else
			ni_addr = readl(dma_chan_src_reg(amb_chan));

		if (ni_addr != (amb_chan->rt_addr & GENMASK(31, 0)))
			return NULL;

		/* check control reg */
		_ctr = amb_desc->lli->xfr_count;
		_ctr |= ((amb_chan->rt_attr & 0x00E00000) >> 21) << 27;
		_ctr |= ((amb_chan->rt_attr & 0x00070000) >> 16) << 24;
		_ctr |= ((amb_chan->rt_attr & 0x00180000) >> 19) << 22;

		ctr = readl(dma_chan_ctr_reg(amb_chan));
		if ((ctr & GENMASK(29, 0)) != _ctr)
			return NULL;
	}

	if (status)
		*status = sta;

	return amb_desc;
}

static void ambdma_chain_complete(struct ambdma_chan *amb_chan,
		struct ambdma_desc *amb_desc)
{
	struct dma_async_tx_descriptor *txd = &amb_desc->txd;

	dma_cookie_complete(txd);

	ambdma_return_desc(amb_chan, amb_desc);

	spin_unlock(&amb_chan->lock);
	if (txd->flags & DMA_PREP_INTERRUPT)
		dmaengine_desc_get_callback_invoke(txd, NULL);
	spin_lock(&amb_chan->lock);

	dma_run_dependencies(txd);
}


static void ambdma_complete_all(struct ambdma_chan *amb_chan)
{
	struct ambdma_desc *amb_desc, *_desc;
	LIST_HEAD(list);

	list_splice_init(&amb_chan->active_list, &list);

	/*
	 * submit queued descriptors ASAP, i.e. before we go through the
	 * completed ones.
	 */
	if (!list_empty(&amb_chan->queue_list)) {
		list_splice_init(&amb_chan->queue_list, &amb_chan->active_list);
		ambdma_dostart(amb_chan, ambdma_first_active(amb_chan));
	}

	list_for_each_entry_safe(amb_desc, _desc, &list, desc_node)
		ambdma_chain_complete(amb_chan, amb_desc);
}

static void ambdma_advance_work(struct ambdma_chan *amb_chan)
{
	if (list_empty(&amb_chan->active_list) || list_is_singular(&amb_chan->active_list)) {
		ambdma_complete_all(amb_chan);
	} else {
		ambdma_chain_complete(amb_chan, ambdma_first_active(amb_chan));
		/*
		 * active_list has been updated by ambdma_chain_complete(),
		 * so ambdma_first_active() will get another amb_desc.
		 */
		ambdma_dostart(amb_chan, ambdma_first_active(amb_chan));
	}
}

static void ambdma_handle_error(struct ambdma_chan *amb_chan,
		struct ambdma_desc *bad_desc)
{
	list_del_init(&bad_desc->desc_node);

	/* try to submit queued descriptors to restart dma */
	list_splice_init(&amb_chan->queue_list, amb_chan->active_list.prev);
	if (!list_empty(&amb_chan->active_list))
		ambdma_dostart(amb_chan, ambdma_first_active(amb_chan));

	pr_crit("%s: DMA error on channel %d: 0x%08x\n",
		__func__, amb_chan->chan.chan_id, bad_desc->lli->rpt);

	/* pretend the descriptor completed successfully */
	ambdma_chain_complete(amb_chan, bad_desc);
}

static void ambdma_tasklet(unsigned long data)
{
	struct ambdma_chan *amb_chan = (struct ambdma_chan *)data;
	struct ambdma_desc *first = NULL;
	enum ambdma_status old_status;
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);

	old_status = amb_chan->status;
	if (!ambdma_chan_is_enabled(amb_chan)) {
		amb_chan->status = AMBDMA_STATUS_IDLE;
		if (!list_empty(&amb_chan->stopping_list))
			ambdma_return_desc(amb_chan, ambdma_first_stopping(amb_chan));
	}

	/* someone might have called terminate all */
	if (list_empty(&amb_chan->active_list))
		goto tasklet_out;

	/*
	 * Note: if the DMA channel is stopped manually rather than naturally end,
	 * then ambdma_first_active() will return the next descriptor that need
	 * to be started, but not the descriptor that invoke this tasklet (IRQ).
	 */
	first = ambdma_first_active(amb_chan);

	if (!first->is_cyclic && amb_chan->status != AMBDMA_STATUS_IDLE) {
		pr_err("%s: channel(%d) invalid status\n",
				__func__, amb_chan->chan.chan_id);
		goto tasklet_out;
	}

	if (old_status == AMBDMA_STATUS_BUSY) {
		/* the IRQ is triggered by DMA stopping naturally or by errors.*/
		if (ambdma_desc_is_error(first)) {
			ambdma_handle_error(amb_chan, first);
		} else if (first->is_cyclic) {
			spin_unlock(&amb_chan->lock);
			if (first->txd.flags & DMA_PREP_INTERRUPT)
				dmaengine_desc_get_callback_invoke(&first->txd, NULL);
			spin_lock(&amb_chan->lock);
		} else {
			ambdma_advance_work(amb_chan);
		}
	} else if (old_status == AMBDMA_STATUS_STOPPING) {
		/* the DMA channel is stopped manually.  */
		ambdma_dostart(amb_chan, first);
	}

tasklet_out:
	spin_unlock_irqrestore(&amb_chan->lock, flags);
}

static irqreturn_t ambdma_dma_irq_handler(int irq, void *dev_data)
{
	struct ambdma_device *amb_dma = dev_data;
	struct ambdma_chan *amb_chan;
	u32 i, int_src;
	irqreturn_t ret = IRQ_NONE;

	int_src = readl(amb_dma->regbase + DMA_INT_OFFSET);
	if (int_src == 0)
		return IRQ_HANDLED;

	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];
		spin_lock(&amb_chan->lock);
		if (int_src & (1 << i)) {
			writel(0, dma_chan_sta_reg(amb_chan));
			tasklet_schedule(&amb_chan->tasklet);
			ret = IRQ_HANDLED;
		}
		spin_unlock(&amb_chan->lock);
	}

	return ret;
}

static int ambdma_pause_channel(struct ambdma_chan *amb_chan)
{
	struct ambdma_device *amb_dma = amb_chan->amb_dma;
	u32 val;

	if (amb_dma->non_support_prs)
		return -ENXIO;

	val = readl(amb_dma->regbase + DMA_PAUSE_SET_OFFSET);
	val |= 0x1 << amb_chan->chan.chan_id;
	writel(val, amb_dma->regbase + DMA_PAUSE_SET_OFFSET);

	return 0;
}

static int ambdma_resume_channel(struct ambdma_chan *amb_chan)
{
	struct ambdma_device *amb_dma = amb_chan->amb_dma;
	u32 val;

	if (amb_dma->non_support_prs)
		return -ENXIO;

	val = readl(amb_dma->regbase + DMA_PAUSE_CLR_OFFSET);
	val |= 1 << amb_chan->chan.chan_id;
	writel(val, amb_dma->regbase + DMA_PAUSE_CLR_OFFSET);

	return 0;
}

static int ambdma_stop_channel(struct ambdma_chan *amb_chan)
{
	struct ambdma_device *amb_dma = amb_chan->amb_dma;
	struct ambdma_desc *first, *amb_desc;

	if (amb_chan->status == AMBDMA_STATUS_BUSY) {
		if (!amb_dma->non_support_prs) { /* support early_end */
			ktime_t timeout;
			u32 val;

			ambdma_pause_channel(amb_chan);

			timeout = ktime_add_ms(ktime_get(), 100);
			while (ambdma_current_desc(amb_chan, NULL) == NULL) {
				/* it may needs 20us in worst case, but it only
				 * needs several us in most cases.
				 */
				BUG_ON(ktime_after(ktime_get(), timeout));
			}

			val = readl(amb_dma->regbase + DMA_EARLY_END_OFFSET);
			val |= 0x1 << amb_chan->chan.chan_id;
			writel(val, amb_dma->regbase + DMA_EARLY_END_OFFSET);

			/* clear early end dma interrupt after dma is stopped */
			timeout = ktime_add_ms(ktime_get(), 10);
			while (ambdma_chan_is_enabled(amb_chan)) {
				/* it may needs 3us in worst case, but it only
				 * needs <1us in most cases
				 */
				BUG_ON(ktime_after(ktime_get(), timeout));
			}
			writel(0x0, dma_chan_sta_reg(amb_chan));

			amb_chan->status = AMBDMA_STATUS_IDLE;

			ambdma_resume_channel(amb_chan);
		} else if (amb_chan->force_stop == 0) {
			/* Normally there are still two IRQs will be triggered
			 * untill DMA channel stops.
			 */
			first = ambdma_first_active(amb_chan);
			if (first) {
				first->lli->attr |= DMA_DESC_EOC;
				list_for_each_entry(amb_desc, &first->tx_list, desc_node) {
					amb_desc->lli->attr |= DMA_DESC_EOC;
				}
				amb_chan->status = AMBDMA_STATUS_STOPPING;
				/*
				* active_list is still being used by DMA controller,
				* so move it to stopping_list to avoid being
				* initialized by next transfer.
				*/
				list_move_tail(&first->desc_node, &amb_chan->stopping_list);
			}
		} else {
			/* Disable DMA: this sequence is not mentioned at PRM.*/
			writel(DMA_CHANX_STA_OD, dma_chan_sta_reg(amb_chan));
			writel(amb_dma->dummy_lli_phys, dma_chan_da_reg(amb_chan));
			writel(DMA_CHANX_CTR_WM | DMA_CHANX_CTR_NI, dma_chan_ctr_reg(amb_chan));
			udelay(1);
			/* avoid to trigger dummy IRQ.*/
			writel(0x0, dma_chan_sta_reg(amb_chan));
			if (ambdma_chan_is_enabled(amb_chan)) {
				pr_err("%s: stop dma channel(%d) failed\n",
					__func__, amb_chan->chan.chan_id);
				return -EIO;
			}
			amb_chan->status = AMBDMA_STATUS_IDLE;
		}
	}

	return 0;
}

static void ambdma_dostart(struct ambdma_chan *amb_chan, struct ambdma_desc *first)
{
	struct ambdma_desc *amb_desc;
	ktime_t timeout;

	/*
	 * if DMA channel is not idle right now, the DMA descriptor
	 * will be started at ambdma_tasklet().
	 */
	if (amb_chan->status > AMBDMA_STATUS_IDLE)
		return;

	if (ambdma_chan_is_enabled(amb_chan)) {
		pr_err("%s: channel(%d) should be idle\n",
				__func__, amb_chan->chan.chan_id);
		return;
	}

	INIT_LIST_HEAD(&amb_chan->work_list);

	list_add_tail(&first->work_node, &amb_chan->work_list);

	if (!list_empty(&first->tx_list)) {
		list_for_each_entry(amb_desc, &first->tx_list, desc_node) {
			list_add_tail(&amb_desc->work_node, &amb_chan->work_list);
		}
	}

	writel(0x0, dma_chan_sta_reg(amb_chan));
	writel(first->txd.phys, dma_chan_da_reg(amb_chan));
	writel(DMA_CHANX_CTR_EN | DMA_CHANX_CTR_D, dma_chan_ctr_reg(amb_chan));
	amb_chan->status = AMBDMA_STATUS_BUSY;

	/*
	 * wait for the decriptor-precessing logic write control register
	 * this can gurantee get an stable status
	 */
	timeout = ktime_add_ms(ktime_get(), 100);

	while(ambdma_current_desc(amb_chan, NULL) == NULL) {
		if (ktime_after(ktime_get(), timeout))
			break;

		cpu_relax();
	}
	BUG_ON(ktime_after(ktime_get(), timeout));
}

static dma_cookie_t ambdma_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct ambdma_desc *amb_desc;
	struct ambdma_chan *amb_chan;
	dma_cookie_t cookie;
	unsigned long flags;

	amb_desc = to_ambdma_desc(tx);
	amb_chan = to_ambdma_chan(tx->chan);

	spin_lock_irqsave(&amb_chan->lock, flags);

	cookie = dma_cookie_assign(tx);

	if (list_empty(&amb_chan->active_list)) {
		list_add_tail(&amb_desc->desc_node, &amb_chan->active_list);
		ambdma_dostart(amb_chan, amb_desc);
	} else {
		list_add_tail(&amb_desc->desc_node, &amb_chan->queue_list);
	}

	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return cookie;
}

/* DMA Engine API begin */
static int ambdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct ambdma_chan *amb_chan;
	struct ambdma_desc *amb_desc;
	LIST_HEAD(tmp_list);
	unsigned long flags;
	int i;

	amb_chan = to_ambdma_chan(chan);

	if (amb_chan->status == AMBDMA_STATUS_BUSY) {
		pr_err("%s: channel(%d) not idle!\n", __func__, amb_chan->chan.chan_id);
		return -EIO;
	}

	/* Alloc descriptors for this channel */
	for (i = 0; i < AMBARELLA_MAX_DESC_NUM; i++) {
		amb_desc = ambdma_alloc_desc(chan, GFP_KERNEL);
		if (amb_desc == NULL) {
			struct ambdma_desc *d, *_d;
			list_for_each_entry_safe(d, _d, &tmp_list, desc_node) {
				ambdma_free_desc(chan, d);
			}
			pr_err("%s: failed to allocate descriptor\n", __func__);
			return -ENOMEM;
		}

		list_add_tail(&amb_desc->desc_node, &tmp_list);
	}

	spin_lock_irqsave(&amb_chan->lock, flags);
	amb_chan->descs_allocated = i;
	amb_chan->rt_addr = ~(phys_addr_t)0;
	amb_chan->rt_attr = 0;
	list_splice_init(&tmp_list, &amb_chan->free_list);
	dma_cookie_init(chan);
	if (!amb_chan->chan.private)
		amb_chan->force_stop = 1;
	else
		amb_chan->force_stop = *(u32 *)amb_chan->chan.private;
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return amb_chan->descs_allocated;
}

static void ambdma_free_chan_resources(struct dma_chan *chan)
{
	struct ambdma_chan *amb_chan;
	struct ambdma_desc *amb_desc, *_desc;
	LIST_HEAD(list);
	unsigned long flags;

	amb_chan = to_ambdma_chan(chan);

	spin_lock_irqsave(&amb_chan->lock, flags);
	BUG_ON(!list_empty(&amb_chan->active_list));
	BUG_ON(!list_empty(&amb_chan->queue_list));
	BUG_ON(amb_chan->status == AMBDMA_STATUS_BUSY);

	list_splice_init(&amb_chan->free_list, &list);
	amb_chan->descs_allocated = 0;
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	list_for_each_entry_safe(amb_desc, _desc, &list, desc_node)
		ambdma_free_desc(chan, amb_desc);
}

/*
 * If this function is called when the dma channel is transferring data,
 * you may get inaccuracy result.
 * And, the data count is the count of data read from source, but not the
 * count to destination. Some data may buffer in DMA engine before send out.
 * So it is just a reference if dma is still working or pausing. The internal
 * buffer is up to 40 bytes.
 */
static int ambdma_get_bytes_left(struct dma_chan *chan, dma_cookie_t cookie)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *first = NULL, *amb_desc;
	u32 status, trials;
	size_t count = 0;
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);

	/* Find amb_desc in active_list or stopping_list by cookie. */
	amb_desc = ambdma_first_active(amb_chan);
	if (amb_desc && amb_desc->txd.cookie == cookie)
		first = amb_desc;

	if (!first) {
		amb_desc = ambdma_first_stopping(amb_chan);
		if (amb_desc && amb_desc->txd.cookie == cookie)
			first = amb_desc;
	}

	/*
	 * If it's in active list, we should get the count for non-completed
	 * desc from the dma channel status register, and get the count for
	 * completed desc from the "xfr_count" field in desc.
	 */
	if (likely(first)) {
		ktime_t timeout = ktime_add_ms(ktime_get(), 10);
		/*
		 * The DA and STA registers cannot be read both atomically, hence
		 * a race condition may occur: the first read register may refer
		 * to one child descriptor whereas the second read may refer to
		 * a later child descriptor in the list because of the DMA transfer
		 * progression inbetween the two reads.
		 *
		 * In addition, the descriptor-processing logic is always running
		 * in background, it will lead to get invalid values when reading
		 * from desc and status registers, so that we have to check the
		 * register values to make sure they are valid.
		 */
		for (trials = 0; trials < AMBARELLA_MAX_DESC_TRIALS; trials++) {
			while (1) {
				amb_desc = ambdma_current_desc(amb_chan, &status);
				if (amb_desc)
					break;

				WARN_ONCE(ktime_after(ktime_get(), timeout),
					"timeout: status = 0x%08x\n", status);
			}

			if (amb_desc == ambdma_current_desc(amb_chan, NULL))
				break;
		}
#if 0
		if (trials > 0 || (status & AMBARELLA_DMA_MAX_LENGTH) > first->lli->xfr_count) {
			writel(0, dma_chan_sta_reg(amb_chan));
			//printk(KERN_DEBUG "trials = %d\n", trials);
		}
#endif

		WARN_ONCE(trials >= AMBARELLA_MAX_DESC_TRIALS, "trials = %d\n", trials);

		count = min_t(size_t, amb_desc->index * first->lli->xfr_count, first->total_len);
		count += status & AMBARELLA_DMA_MAX_LENGTH;

	} else if (!list_empty(&amb_chan->queue_list)) {
		/*
		 * If it's in queue_list, all of the desc have not been started,
		 * so the transferred count is always 0.
		 */
		list_for_each_entry(amb_desc, &amb_chan->queue_list, desc_node) {
			if (amb_desc->txd.cookie == cookie) {
				first = amb_desc;
				count = 0;
				break;
			}
		}
	}

	spin_unlock_irqrestore(&amb_chan->lock, flags);

	if (!first)
		return -EINVAL;

	return first->total_len - count;
}

static enum dma_status ambdma_tx_status(struct dma_chan *chan,
		dma_cookie_t cookie, struct dma_tx_state *txstate)
{
	enum dma_status ret;
	int residue;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	residue = ambdma_get_bytes_left(chan, cookie);
	if (unlikely(residue < 0))
		return DMA_ERROR;

	dma_set_residue(txstate, residue);

	return ret;
}

static void ambdma_issue_pending(struct dma_chan *chan)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);

	/* if dma channel is not idle, will active queue_list in tasklet. */
	if (amb_chan->status == AMBDMA_STATUS_IDLE) {
		if (!list_empty(&amb_chan->active_list))
			pr_err("%s: active_list should be empty here\n", __func__);
		else
			ambdma_advance_work(amb_chan);
	}

	spin_unlock_irqrestore(&amb_chan->lock, flags);
}


static int ambdma_pause(struct dma_chan *chan)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&amb_chan->lock, flags);
	ret = ambdma_pause_channel(amb_chan);
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return ret;
}

static int ambdma_resume(struct dma_chan *chan)
{

	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&amb_chan->lock, flags);
	ret = ambdma_resume_channel(amb_chan);
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return ret;
}
static int ambdma_config(struct dma_chan *chan,
			 struct dma_slave_config *dmaengine_cfg)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct dma_slave_config *config = dmaengine_cfg;
	enum dma_slave_buswidth width = 0;
	int maxburst;

	/* We only support mem to dev or dev to mem transfers */
	switch (config->direction) {
	case DMA_MEM_TO_DEV:
		width = config->dst_addr_width;
		maxburst = config->dst_maxburst;
		amb_chan->rt_addr = config->dst_addr;
		amb_chan->rt_attr = DMA_DESC_RM | DMA_DESC_NI |
			DMA_DESC_IE | DMA_DESC_ST;
		break;
	case DMA_DEV_TO_MEM:
		width = config->src_addr_width;
		maxburst = config->src_maxburst;
		amb_chan->rt_addr = config->src_addr;
		amb_chan->rt_attr = DMA_DESC_WM | DMA_DESC_NI |
			DMA_DESC_IE | DMA_DESC_ST;
		break;
	default:
		return -ENXIO;
	}

	/* bus width for descriptor mode control_info [ts fileds] */
	switch (width) {
	case DMA_SLAVE_BUSWIDTH_8_BYTES:
		amb_chan->rt_attr |= DMA_DESC_TS_8B;
		break;
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		amb_chan->rt_attr |= DMA_DESC_TS_4B;
		break;
	case DMA_SLAVE_BUSWIDTH_2_BYTES:
		amb_chan->rt_attr |= DMA_DESC_TS_2B;
		break;
	case DMA_SLAVE_BUSWIDTH_1_BYTE:
		amb_chan->rt_attr |= DMA_DESC_TS_1B;
		break;
	default:
		break;
	}

	/* burst for descriptor mode control_info [blk fileds] */
	switch (maxburst) {
	case 1024:
		amb_chan->rt_attr |= DMA_DESC_BLK_1024B;
		break;
	case 512:
		amb_chan->rt_attr |= DMA_DESC_BLK_512B;
		break;
	case 256:
		amb_chan->rt_attr |= DMA_DESC_BLK_256B;
		break;
	case 128:
		amb_chan->rt_attr |= DMA_DESC_BLK_128B;
		break;
	case 64:
		amb_chan->rt_attr |= DMA_DESC_BLK_64B;
		break;
	case 32:
		amb_chan->rt_attr |= DMA_DESC_BLK_32B;
		break;
	case 16:
		amb_chan->rt_attr |= DMA_DESC_BLK_16B;
		break;
	case 8:
		amb_chan->rt_attr |= DMA_DESC_BLK_8B;
		break;
	default:
		break;
	}

	return 0;


}

static int ambdma_terminate_all(struct dma_chan *chan)
{

	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *amb_desc, *_desc;
	LIST_HEAD(list);
	unsigned long flags;

	spin_lock_irqsave(&amb_chan->lock, flags);
	ambdma_stop_channel(amb_chan);

	/* active_list entries will end up before queued entries */
	list_splice_init(&amb_chan->queue_list, &list);
	list_splice_init(&amb_chan->active_list, &list);

	/* Flush all pending and queued descriptors */
	list_for_each_entry_safe(amb_desc, _desc, &list, desc_node) {
		/* move children to free_list */
		list_splice_init(&amb_desc->tx_list, &amb_chan->free_list);
		/* move myself to free_list */
		list_move_tail(&amb_desc->desc_node, &amb_chan->free_list);
	}
	spin_unlock_irqrestore(&amb_chan->lock, flags);

	return 0;

}

static struct dma_async_tx_descriptor *ambdma_prep_dma_cyclic(
		struct dma_chan *chan, dma_addr_t buf_addr, size_t buf_len,
		size_t period_len, enum dma_transfer_direction direction,
		unsigned long flags)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *amb_desc, *first = NULL, *prev = NULL;
	int left_len = buf_len, index = 0;

	if (buf_len == 0 || period_len == 0) {
		pr_err("%s: buf/period length is zero!\n", __func__);
		return NULL;
	}

	do {
		amb_desc = ambdma_get_desc(amb_chan);
		if (!amb_desc)
			goto dma_cyclic_err;

		amb_desc->is_cyclic = 1;
		amb_desc->index = index++;

		if (period_len > left_len)
			period_len = left_len;

		if (direction == DMA_MEM_TO_DEV) {
			amb_desc->lli->src = buf_addr;
			amb_desc->lli->dst = amb_chan->rt_addr;
		} else if (direction == DMA_DEV_TO_MEM) {
			amb_desc->lli->src = amb_chan->rt_addr;
			amb_desc->lli->dst = buf_addr;
		} else {
			goto dma_cyclic_err;
		}
		/* trigger interrupt after each dma transaction ends. */
		amb_desc->lli->attr = amb_chan->rt_attr;
		amb_desc->lli->attr |= (flags & DMA_PREP_INTERRUPT) ? DMA_DESC_ID : 0;
		amb_desc->lli->xfr_count = period_len;
		/* rpt_addr points to amb_desc->lli->rpt */
		amb_desc->lli->rpt_addr =
			amb_desc->txd.phys + sizeof(struct ambdma_lli) - 4;
		/* here we initialize rpt to 0 */
		amb_desc->lli->rpt = 0;

		if (first == NULL)
			first = amb_desc;
		else {
			prev->lli->next_desc = amb_desc->txd.phys;
			list_add_tail(&amb_desc->desc_node, &first->tx_list);
		}

		prev = amb_desc;

		left_len -= period_len;
		buf_addr += period_len;

		/*
		 * Our dma controller can't transfer data larger than 4M Bytes,
		 * but it seems that no use case will transfer so large data,
		 * so we just trigger a BUG here for reminder.
		 */
		BUG_ON(amb_desc->lli->xfr_count > AMBARELLA_DMA_MAX_LENGTH);
	} while (left_len > 0);

	/* lets make a cyclic list */
	amb_desc->lli->next_desc = first->txd.phys;

	/* First descriptor of the chain embedds additional information */
	first->txd.cookie = -EBUSY;
	first->txd.flags = flags;
	first->total_len = buf_len;

	return &first->txd;

dma_cyclic_err:
	dev_err(&chan->dev->device, "prep_dma_cyclic error: %p\n", amb_desc);
	ambdma_put_desc(amb_chan, first);
	return NULL;
}


static struct dma_async_tx_descriptor *ambdma_prep_slave_sg(
		struct dma_chan *chan, struct scatterlist *sgl,
		unsigned int sg_len, enum dma_transfer_direction direction,
		unsigned long flags, void *context)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *amb_desc, *first = NULL, *prev = NULL;
	struct scatterlist *sgent;
	size_t total_len = 0;
	unsigned i = 0, index = 0;

	if (sg_len == 0) {
		pr_err("%s: sg length is zero!\n", __func__);
		return NULL;
	}

	for_each_sg(sgl, sgent, sg_len, i) {
		amb_desc = ambdma_get_desc(amb_chan);
		if (!amb_desc)
			goto slave_sg_err;

		amb_desc->is_cyclic = 0;
		amb_desc->index = index++;

		if (direction == DMA_MEM_TO_DEV) {
			amb_desc->lli->src = sg_dma_address(sgent);
			amb_desc->lli->dst = amb_chan->rt_addr;
		} else if (direction == DMA_DEV_TO_MEM) {
			amb_desc->lli->src = amb_chan->rt_addr;
			amb_desc->lli->dst = sg_dma_address(sgent);
		} else {
			goto slave_sg_err;
		}
		amb_desc->lli->attr = amb_chan->rt_attr;
		amb_desc->lli->xfr_count = sg_dma_len(sgent);
		/* rpt_addr points to amb_desc->lli->rpt */
		amb_desc->lli->rpt_addr =
			amb_desc->txd.phys + sizeof(struct ambdma_lli) - 4;
		/* here we initialize rpt to 0 */
		amb_desc->lli->rpt = 0;

		if (first == NULL)
			first = amb_desc;
		else {
			prev->lli->next_desc = amb_desc->txd.phys;
			list_add_tail(&amb_desc->desc_node, &first->tx_list);
		}

		prev = amb_desc;
		total_len += amb_desc->lli->xfr_count;

		/*
		 * Our dma controller can't transfer data larger than 4M Bytes,
		 * but it seems that no use case will transfer so large data,
		 * so we just trigger a BUG here for reminder.
		 */
		BUG_ON(amb_desc->lli->xfr_count > AMBARELLA_DMA_MAX_LENGTH);
	}

	/*
	 * Lets make a cyclic list.
	 * Note: it doesn't really take effects on hardware, but only used
	 * by software to identify which descriptor is current running.
	 */
	amb_desc->lli->next_desc = first->txd.phys;

	/* set EOC flag to specify the last descriptor */
	amb_desc->lli->attr |= DMA_DESC_EOC;

	/* First descriptor of the chain embedds additional information */
	first->txd.cookie = -EBUSY;
	first->txd.flags = flags; /* client is in control of this ack */
	first->total_len = total_len;

	return &first->txd;

slave_sg_err:
	dev_err(&chan->dev->device, "prep_slave_sg error: %p\n", amb_desc);
	ambdma_put_desc(amb_chan, first);
	return NULL;
}

static struct dma_async_tx_descriptor *ambdma_prep_dma_memcpy(
		struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
		size_t len, unsigned long flags)
{
	struct ambdma_chan *amb_chan = to_ambdma_chan(chan);
	struct ambdma_desc *amb_desc = NULL, *first = NULL, *prev = NULL;
	size_t left_len = len, xfer_count;
	int index = 0;

	if (unlikely(!len)) {
		pr_info("%s: length is zero!\n", __func__);
		return NULL;
	}

	do{
		amb_desc = ambdma_get_desc(amb_chan);
		if (!amb_desc)
			goto dma_memcpy_err;

		amb_desc->is_cyclic = 0;
		amb_desc->index = index++;

		amb_desc->lli->src = src;
		amb_desc->lli->dst = dst;
		amb_desc->lli->attr = DMA_DESC_RM | DMA_DESC_WM | DMA_DESC_IE |
				DMA_DESC_ST | DMA_DESC_BLK_32B | DMA_DESC_TS_4B;
		xfer_count = min(left_len, (size_t)AMBARELLA_DMA_MAX_LENGTH);
		amb_desc->lli->xfr_count = xfer_count;
		/* rpt_addr points to amb_desc->lli->rpt */
		amb_desc->lli->rpt_addr =
			amb_desc->txd.phys + sizeof(struct ambdma_lli) - 4;
		/* here we initialize rpt to 0 */
		amb_desc->lli->rpt = 0;

		if (first == NULL)
			first = amb_desc;
		else {
			prev->lli->next_desc = amb_desc->txd.phys;
			list_add_tail(&amb_desc->desc_node, &first->tx_list);
		}

		prev = amb_desc;

		src += xfer_count;
		dst += xfer_count;
		left_len -= xfer_count;

	} while (left_len > 0);

	/*
	 * Lets make a cyclic list.
	 * Note: it doesn't really take effects on hardware, but only used
	 * by software to identify which descriptor is current running.
	 */
	amb_desc->lli->next_desc = first->txd.phys;

	/* set EOC flag to specify the last descriptor */
	amb_desc->lli->attr |= DMA_DESC_EOC;

	/* First descriptor of the chain embedds additional information */
	first->txd.cookie = -EBUSY;
	first->txd.flags = flags; /* client is in control of this ack */
	first->total_len = len;

	return &first->txd;

dma_memcpy_err:
	dev_err(&chan->dev->device, "prep_dma_memcpy error: %p\n", amb_desc);
	ambdma_put_desc(amb_chan, first);

	return NULL;
}

static struct dma_chan *ambdma_of_xlate(struct of_phandle_args *dma_spec,
					struct of_dma *ofdma)
{
	struct ambdma_device *amb_dma = ofdma->of_dma_data;
	struct dma_chan *chan;
	unsigned int request = dma_spec->args[0];
	unsigned int reg_idx, sel_channels, chan_id_index;

	if (request >= amb_dma->nr_requests)
		return NULL;

	if (amb_dma->prealloc_chan == 0) {
		chan = dma_get_any_slave_channel(&amb_dma->dma_dev);
		if (!chan) {
			dev_err(amb_dma->dma_dev.dev, "can't get a dma channel\n");
			return NULL;
		}
	} else {
		int i, prealloc_chan_id;
		bool found = false;
		struct ambdma_chan *amb_chan;

		if (amb_dma->id < 0) {
			dev_err(amb_dma->dma_dev.dev, "Invalid DMA instance id\n");
			return NULL;
		}
		prealloc_chan_id = ambarella_scm_get_dma_chan_id(amb_dma->id, request);
		if (prealloc_chan_id < 0) {
			dev_err(amb_dma->dma_dev.dev, "Invalid prealloc channel\n");
			return NULL;
		}

		for (i = 0; i < amb_dma->nr_channels; i++) {
			amb_chan = &amb_dma->amb_chan[i];
			if (amb_chan->chan.chan_id == prealloc_chan_id) {
				chan = &amb_chan->chan;
				chan = dma_get_slave_channel(chan);
				if (chan) {
					found = true;
					break;
				}
			}
		}

		if (!found) {
			return NULL;
		}
	}

	sel_channels = 32 / amb_dma->sel_width;

	if (chan->chan_id < sel_channels) {
		reg_idx = 0;
		chan_id_index = chan->chan_id * amb_dma->sel_width;
	} else {
		reg_idx = 1;
		chan_id_index = (chan->chan_id - sel_channels) * amb_dma->sel_width;
	}

	if (amb_dma->prealloc_chan == 0) {
		regmap_update_bits(amb_dma->scr_regmap, amb_dma->scr_offset[reg_idx],
				   ((1 << amb_dma->sel_width) - 1) << chan_id_index, request << chan_id_index);
	}

	return chan;
}

static const struct soc_device_attribute ambarella_dma_socinfo[] = {
	{ .soc_id = "s5l" },
	{/* sentinel */}
};

static int ambarella_dma_probe(struct platform_device *pdev)
{
	struct ambdma_device *amb_dma;
	struct ambdma_chan *amb_chan;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	int val, i, scr_reg_count, ret = 0;

	/* alloc the amba dma engine struct */
	amb_dma = devm_kzalloc(&pdev->dev, sizeof(*amb_dma), GFP_KERNEL);
	if (amb_dma == NULL) {
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	amb_dma->regbase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(amb_dma->regbase)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		ret = PTR_ERR(amb_dma->regbase);
		goto ambdma_dma_probe_exit;
	}

	amb_dma->id = of_alias_get_id(np, "dma");

	amb_dma->dma_irq = platform_get_irq(pdev, 0);
	if (amb_dma->dma_irq < 0) {
		ret = -EINVAL;
		goto ambdma_dma_probe_exit;
	}

	if (soc_device_match(ambarella_dma_socinfo))
		scr_reg_count = 1;
	else
		scr_reg_count = ARRAY_SIZE(amb_dma->scr_offset);

	amb_dma->scr_regmap = syscon_regmap_lookup_by_phandle_args(np, "amb,scr-regmap",
					scr_reg_count, amb_dma->scr_offset);
	if (IS_ERR(amb_dma->scr_regmap)) {
		dev_err(&pdev->dev, "regmap lookup failed.\n");
		ret = PTR_ERR(amb_dma->scr_regmap);
		goto ambdma_dma_probe_exit;
	}

	if (of_property_read_u32(np, "amb,sel-width", &amb_dma->sel_width) < 0)
		amb_dma->sel_width = AHBSP_DMA_SEL_BIT_SHIFT;

	amb_dma->non_support_prs = !!of_find_property(np, "amb,non-support-prs", NULL);
	amb_dma->prealloc_chan = !!of_find_property(np, "amb,prealloc-channel", NULL);

	ret = of_property_read_u32(np, "dma-channels", &amb_dma->nr_channels);
	if (ret) {
		dev_err(&pdev->dev, "failed to get dma-channels\n");
		goto ambdma_dma_probe_exit;
	}

	ret = of_property_read_u32(np, "dma-requests", &amb_dma->nr_requests);
	if (ret) {
		dev_err(&pdev->dev, "failed to get dma-requests\n");
		goto ambdma_dma_probe_exit;
	}

	/* Explictly set 32-bit capable mask */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "failed to set dma mask\n");
		goto ambdma_dma_probe_exit;
	}

	/* create a pool of consistent memory blocks for hardware descriptors */
	amb_dma->lli_pool = dma_pool_create("ambdma_lli_pool",
			&pdev->dev, sizeof(struct ambdma_lli), 16, 0);
	if (!amb_dma->lli_pool) {
		dev_err(&pdev->dev, "No memory for descriptors dma pool\n");
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit;
	}

	/* alloc dummy_lli and dummy_data for terminate usage. */
	amb_dma->dummy_lli = dma_pool_alloc(amb_dma->lli_pool,
			GFP_KERNEL, &amb_dma->dummy_lli_phys);
	if (amb_dma->dummy_lli == NULL) {
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit1;
	}

	amb_dma->dummy_data = dma_pool_alloc(amb_dma->lli_pool,
			GFP_KERNEL, &amb_dma->dummy_data_phys);
	if (amb_dma->dummy_data == NULL) {
		ret = -ENOMEM;
		goto ambdma_dma_probe_exit2;
	}

	amb_dma->dummy_lli->attr = DMA_DESC_EOC | DMA_DESC_WM |
					DMA_DESC_NI | DMA_DESC_IE |
					DMA_DESC_ST | DMA_DESC_ID;
	amb_dma->dummy_lli->next_desc = amb_dma->dummy_lli_phys;
	amb_dma->dummy_lli->xfr_count = 0;
	amb_dma->dummy_lli->src = 0;
	amb_dma->dummy_lli->dst = amb_dma->dummy_data_phys;
	/* rpt_addr points to ambdma_lli->rpt field */
	amb_dma->dummy_lli->rpt_addr =
		amb_dma->dummy_lli_phys + sizeof(struct ambdma_lli) - 4;

	/* Init dma_device struct */
	dma_cap_zero(amb_dma->dma_dev.cap_mask);
	dma_cap_set(DMA_SLAVE, amb_dma->dma_dev.cap_mask);
	dma_cap_set(DMA_CYCLIC, amb_dma->dma_dev.cap_mask);
	dma_cap_set(DMA_MEMCPY, amb_dma->dma_dev.cap_mask);
	INIT_LIST_HEAD(&amb_dma->dma_dev.channels);
	amb_dma->dma_dev.dev = &pdev->dev;
	amb_dma->dma_dev.device_alloc_chan_resources = ambdma_alloc_chan_resources;
	amb_dma->dma_dev.device_free_chan_resources = ambdma_free_chan_resources;
	amb_dma->dma_dev.device_tx_status = ambdma_tx_status;
	amb_dma->dma_dev.device_issue_pending = ambdma_issue_pending;
	amb_dma->dma_dev.device_prep_dma_cyclic = ambdma_prep_dma_cyclic;
	amb_dma->dma_dev.device_prep_slave_sg = ambdma_prep_slave_sg;
	amb_dma->dma_dev.device_prep_dma_memcpy = ambdma_prep_dma_memcpy;
	amb_dma->dma_dev.device_pause = ambdma_pause;
	amb_dma->dma_dev.device_resume = ambdma_resume;
	amb_dma->dma_dev.device_config = ambdma_config;
	amb_dma->dma_dev.device_terminate_all = ambdma_terminate_all;
	/* DMA capabilities */
	amb_dma->dma_dev.src_addr_widths = AMBARELLA_DMA_BUSWIDTHS;
	amb_dma->dma_dev.dst_addr_widths = AMBARELLA_DMA_BUSWIDTHS;
	amb_dma->dma_dev.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	amb_dma->dma_dev.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;

	/* init dma_chan struct */
	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];
		amb_chan->chan.chan_id = i;

		spin_lock_init(&amb_chan->lock);
		amb_chan->amb_dma = amb_dma;
		amb_chan->status = AMBDMA_STATUS_IDLE;
		amb_chan->chan.device = &amb_dma->dma_dev;
		INIT_LIST_HEAD(&amb_chan->active_list);
		INIT_LIST_HEAD(&amb_chan->queue_list);
		INIT_LIST_HEAD(&amb_chan->free_list);
		INIT_LIST_HEAD(&amb_chan->stopping_list);

		tasklet_init(&amb_chan->tasklet, ambdma_tasklet,
				(unsigned long)amb_chan);
		dma_cookie_init(&amb_chan->chan);
		list_add_tail(&amb_chan->chan.device_node,
				&amb_dma->dma_dev.channels);
	}

	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];

		/*
		 * I2S_RX_DMA_CHAN and I2S_TX_DMA_CHAN may be used
		 * for fastboot in Amboot
		 */
		if ((i == I2S_RX_DMA_CHAN || i == I2S_TX_DMA_CHAN)
			&& ambdma_chan_is_enabled(amb_chan)) {
			amb_chan->status = AMBDMA_STATUS_BUSY;
			amb_chan->force_stop = 1;
			continue;
		}

		writel(0, dma_chan_sta_reg(amb_chan));
		val = DMA_CHANX_CTR_WM | DMA_CHANX_CTR_RM | DMA_CHANX_CTR_NI;
		writel(val, dma_chan_ctr_reg(amb_chan));
	}

	ret = request_irq(amb_dma->dma_irq, ambdma_dma_irq_handler,
			IRQF_SHARED | IRQF_TRIGGER_HIGH,
			dev_name(&pdev->dev), amb_dma);
	if (ret)
		goto ambdma_dma_probe_exit3;

	ret = dma_async_device_register(&amb_dma->dma_dev);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register slave DMA device: %d\n", ret);
		goto ambdma_dma_probe_exit4;
	}

	ret = of_dma_controller_register(np, ambdma_of_xlate, amb_dma);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to register controller\n");
		goto ambdma_dma_probe_exit5;
	}

	proc_create_data(dev_name(&pdev->dev), S_IRUGO|S_IWUSR,
		ambarella_procfs_dir(), &proc_ambdma_fops, amb_dma);

	platform_set_drvdata(pdev, amb_dma);

	dev_info(&pdev->dev, "Ambarella DMA Engine \n");

	return 0;

ambdma_dma_probe_exit5:
	dma_async_device_unregister(&amb_dma->dma_dev);

ambdma_dma_probe_exit4:
	free_irq(amb_dma->dma_irq, amb_dma);

ambdma_dma_probe_exit3:
	dma_pool_free(amb_dma->lli_pool, amb_dma->dummy_data,
			amb_dma->dummy_data_phys);

ambdma_dma_probe_exit2:
	dma_pool_free(amb_dma->lli_pool, amb_dma->dummy_lli,
			amb_dma->dummy_lli_phys);

ambdma_dma_probe_exit1:
	dma_pool_destroy(amb_dma->lli_pool);

ambdma_dma_probe_exit:
	return ret;
}

static int ambarella_dma_remove(struct platform_device *pdev)
{
	struct ambdma_device *amb_dma = platform_get_drvdata(pdev);
	struct ambdma_chan *amb_chan;
	int i;

	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];
		ambdma_stop_channel(amb_chan);

		tasklet_disable(&amb_chan->tasklet);
		tasklet_kill(&amb_chan->tasklet);
	}

	if (pdev->dev.of_node)
		of_dma_controller_free(pdev->dev.of_node);

	dma_async_device_unregister(&amb_dma->dma_dev);

	free_irq(amb_dma->dma_irq, amb_dma);
	dma_pool_free(amb_dma->lli_pool, amb_dma->dummy_lli,
			amb_dma->dummy_lli_phys);
	dma_pool_free(amb_dma->lli_pool, amb_dma->dummy_data,
			amb_dma->dummy_data_phys);
	dma_pool_destroy(amb_dma->lli_pool);

	kfree(amb_dma);

	return 0;
}

#ifdef CONFIG_PM
static int ambarella_dma_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct ambdma_device *amb_dma = platform_get_drvdata(pdev);
	struct ambdma_chan *amb_chan;
	int i;

	/* save dma channel register */
	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];
		amb_chan->ch_ctl = readl(dma_chan_ctr_reg(amb_chan));
		amb_chan->ch_da = readl(dma_chan_da_reg(amb_chan));
		amb_chan->ch_sta = readl(dma_chan_sta_reg(amb_chan));
	}

	regmap_read(amb_dma->scr_regmap, amb_dma->scr_offset[0], &amb_dma->sel_val[0]);
	regmap_read(amb_dma->scr_regmap, amb_dma->scr_offset[1], &amb_dma->sel_val[1]);

	return 0;
}

static int ambarella_dma_resume(struct platform_device *pdev)
{
	struct ambdma_device *amb_dma = platform_get_drvdata(pdev);
	struct ambdma_chan *amb_chan;
	int i;

	regmap_write(amb_dma->scr_regmap, amb_dma->scr_offset[0], amb_dma->sel_val[0]);
	regmap_write(amb_dma->scr_regmap, amb_dma->scr_offset[1], amb_dma->sel_val[1]);

	/* restore dma channel register */
	for (i = 0; i < amb_dma->nr_channels; i++) {
		amb_chan = &amb_dma->amb_chan[i];
		writel(amb_chan->ch_sta, dma_chan_sta_reg(amb_chan));
		writel(amb_chan->ch_da, dma_chan_da_reg(amb_chan));
		writel(amb_chan->ch_ctl, dma_chan_ctr_reg(amb_chan));
	}

	return 0;
}
#endif


static const struct of_device_id ambarella_dma_dt_ids[] = {
	{.compatible = "ambarella,dma", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_dma_dt_ids);

static struct platform_driver ambarella_dma_driver = {
	.probe		= ambarella_dma_probe,
	.remove		= ambarella_dma_remove,
#ifdef CONFIG_PM
	.suspend	= ambarella_dma_suspend,
	.resume		= ambarella_dma_resume,
#endif
	.driver		= {
		.name	= "ambarella-dma",
		.of_match_table = ambarella_dma_dt_ids,
	},
};

static int __init ambarella_dma_init(void)
{
	return platform_driver_register(&ambarella_dma_driver);
}

static void __exit ambarella_dma_exit(void)
{
	platform_driver_unregister(&ambarella_dma_driver);
}

subsys_initcall(ambarella_dma_init);
module_exit(ambarella_dma_exit);

MODULE_DESCRIPTION("Ambarella DMA Engine System Driver");
MODULE_AUTHOR("Jian He <jianhe@ambarella.com>");
MODULE_LICENSE("GPL");

