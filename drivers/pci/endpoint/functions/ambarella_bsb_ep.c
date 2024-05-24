// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ambarella BSB endpoint function pci EP-side driver.
 *
 * History: 2023/08/10 - Li Chen <lchen@ambarella.com> created file
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
#include <linux/sys_soc.h>
#include <asm-generic/errno-base.h>
#include <linux/printk.h>
#include <uapi/linux/amba-bsb.h>
#include <soc/ambarella/epf-core.h>
#include <soc/ambarella/bsb.h>
#include <soc/ambarella/pci-util.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/nospec.h>

bsb_module_parameters;

/* TODO: remove globale_epf!! */
struct pci_epf *global_epf;

/**
 *
 * @wait_ep_msg2rc_mutex: per-subdevice mutex, APP should
 *                        enter BSB_RC_MSG2EP_THEN_WAIT_FOR_MSG2RC one by one.
 */
struct bsb_sub_device {
	bool subdevice_received;
	struct mutex ep_wait_rc_mutex;
};

struct bsb_ep {
	struct dma_chan *dma_chan_tx;
	struct dma_chan *dma_chan_rx;
	struct mutex send_msg2rc_interrupt_rc_mutex;
	void *bar[PCI_STD_NUM_BARS];
	enum pci_barno reg_bar;
	enum pci_barno msg_bar;
	size_t msg_bar_size;
	void *msg2ep_base;
	size_t msg2ep_total_size;
	void *msg2rc_base;
	size_t msg2rc_total_size;
	struct delayed_work cmd_handler;
	int cap;
	struct pci_epf *epf;
	const struct pci_epc_features *epc_features;
	struct miscdevice miscdev;
	struct device *dev;
	struct ambarella_msi_doorbell_property msi_doorbell_property;
	struct bsb_sub_device subdevices[];
};

static struct bsb_ep *bsb_ep;

#define BSB_DRIVER_NAME "pci_epf_bsb"
static DEFINE_IDA(bsb_ida);
static DECLARE_WAIT_QUEUE_HEAD(subdevice_wq);

static struct pci_epf_header default_epf_header = {
	.vendorid = PCI_ANY_ID,
	.deviceid = PCI_ANY_ID,
	.baseclass_code = PCI_CLASS_OTHERS,
	.interrupt_pin = PCI_INTERRUPT_INTA,
};

static void bsb_eeo_dma_callback(void *param)
{
}

static int bsb_pci_ep_ob(dma_addr_t dma_dst, dma_addr_t dma_src, u32 total_len)
{
	return ambarella_pci_udma_xfer(&bsb_ep->epf->dev, dma_dst, dma_src,
				       total_len, DMA_MEM_TO_DEV,
				       bsb_ep->dma_chan_tx,
				       bsb_eeo_dma_callback,
				       bsb_ep->bar[bsb_ep->reg_bar]);
}

static void bsb_eei_dma_callback(void *param)
{
}

static int bsb_pci_ep_ib(dma_addr_t dma_dst, dma_addr_t dma_src, u32 total_len)
{
	return ambarella_pci_udma_xfer(&bsb_ep->epf->dev, dma_dst, dma_src,
				       total_len, DMA_DEV_TO_MEM,
				       bsb_ep->dma_chan_rx,
				       bsb_eei_dma_callback,
				       bsb_ep->bar[bsb_ep->reg_bar]);
}

static long bsb_ep_xfer(unsigned long arg)
{
	int ret = -EINVAL, rc_subdevice_idx;
	void __user *argp = (void __user *)arg;
	struct xfer_info *info;
	struct bsb_reg *bsb_reg = bsb_ep->bar[bsb_ep->reg_bar];
	struct reg_subdevice_rmem *rmem;
	dma_addr_t remote_start_addr;
	u32 remote_size;
	int nr_subdevices = READ_ONCE(bsb_reg->nr_subdevices);

	info = memdup_user(argp, sizeof(*info));
	if (IS_ERR(info))
		return PTR_ERR(info);

	rc_subdevice_idx = info->subdevice_idx;
	rc_subdevice_idx = array_index_nospec(rc_subdevice_idx, nr_subdevices);
	rmem = &bsb_reg->subdevice_rmem[rc_subdevice_idx];
	remote_start_addr =
		READ_ONCE(rmem->lower_start_addr) |
		((dma_addr_t)READ_ONCE(rmem->upper_start_addr) << 32);
	remote_size = READ_ONCE(rmem->size);

	if (info->remote_phy_addr < remote_start_addr ||
	    info->remote_phy_addr >= remote_start_addr + remote_size) {
		ret = -EINVAL;
		goto free_xfer_info;
	}

	if (info->dir == PCI_READ)
		ret = bsb_pci_ep_ib(info->local_phy_addr, info->remote_phy_addr,
				    info->size);
	else
		ret = bsb_pci_ep_ob(info->remote_phy_addr, info->local_phy_addr,
				    info->size);

free_xfer_info:
	kfree(info);

	return ret;
}

static int bsb_ep_raise_irq(struct bsb_ep *bsb_ep)
{
	return pci_epc_raise_irq(bsb_ep->epf->epc, bsb_ep->epf->func_no,
				 bsb_ep->epf->vfunc_no, PCI_EPC_IRQ_LEGACY, 0);
}
static long bsb_ep_send_msg2rc_interrupt_rc(unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int ret;
	struct ep_msg2rc *ep_msg2rc = vmemdup_user(argp, sizeof(*ep_msg2rc));
	struct bsb_reg *bsb_reg = bsb_ep->bar[bsb_ep->reg_bar];
	struct mutex *mutex;
	int rc_subdevice_idx;
	int nr_subdevices = READ_ONCE(bsb_reg->nr_subdevices);
	struct msg_info *msg2rc;
	struct device *dev = bsb_ep->dev;
	size_t msg2rc_total_size_per_subdevice =
		bsb_ep->msg2ep_total_size / nr_subdevices;

	if (IS_ERR(ep_msg2rc))
		return PTR_ERR(ep_msg2rc);

	msg2rc = &ep_msg2rc->msg2rc;

	if (msg2rc->size > msg2rc_total_size_per_subdevice) {
		dev_err(dev,
			"msg2rc->size(0x%lx) is too large, we only have 0x%lx  for each device, bsb_ep->msg2ep_total_size is 0x%lx\n",
			msg2rc->size, msg2rc_total_size_per_subdevice,
			bsb_ep->msg2ep_total_size);
		ret = -EINVAL;
		goto free_memdup_user;
	}

	rc_subdevice_idx = ep_msg2rc->subdevice_idx;
	rc_subdevice_idx = array_index_nospec(rc_subdevice_idx, nr_subdevices);
	mutex = &bsb_ep->send_msg2rc_interrupt_rc_mutex;

	if (!READ_ONCE(bsb_reg->waiting_ep[rc_subdevice_idx])) {
		ret = -ESRCH;
		goto free_memdup_user;
	}
	/*
	 * TODO: allow concurrency.
	 */
	mutex_lock(mutex);
	ret = copy_from_user(bsb_ep->msg2rc_base +
				     msg2rc_total_size_per_subdevice *
					     rc_subdevice_idx,
			     msg2rc->base, msg2rc->size);
	if (ret) {
		ret = -EFAULT;
		goto unlock;
	}

	WRITE_ONCE(bsb_reg->wakeup_rc[rc_subdevice_idx], 1);
	WRITE_ONCE(bsb_reg->sz_msg2rc[rc_subdevice_idx], msg2rc->size);
	ret = bsb_ep_raise_irq(bsb_ep);

unlock:
	mutex_unlock(mutex);
free_memdup_user:
	kvfree(ep_msg2rc);

	return ret;
}

static long bsb_ep_get_rc_subdevices_info(unsigned long arg)
{
	int i, ret;
	struct subdevices_info __user *argp = (void __user *)arg;
	struct subdevices_info *subdevices_info;
	struct bsb_reg *bsb_reg = bsb_ep->bar[bsb_ep->reg_bar];
	struct reg_subdevice_rmem *rmem;
	struct device *dev = bsb_ep->dev;

	subdevices_info = vmalloc(sizeof(struct subdevices_info));
	if (!subdevices_info)
		return -ENOMEM;

	subdevices_info->nr_subdevices = READ_ONCE(bsb_reg->nr_subdevices);
	for (i = 0; i < subdevices_info->nr_subdevices; i++) {
		rmem = &bsb_reg->subdevice_rmem[i];
		subdevices_info->subdevice_rmem[i].start_addr =
			READ_ONCE(rmem->lower_start_addr) |
			((dma_addr_t)(READ_ONCE(rmem->upper_start_addr)) << 32);
		subdevices_info->subdevice_rmem[i].size = READ_ONCE(rmem->size);
	}
	ret = copy_to_user(argp, subdevices_info, sizeof(*subdevices_info));
	vfree(subdevices_info);
	if (ret) {
		dev_dbg(dev, "%s: failed to copy_to_user\n", __func__);
		ret = -EFAULT;
	}

	return ret;
}

static long bsb_ep_wait_rc_msg2ep(unsigned long arg)
{
	int subdevice_idx, ret = 0;
	struct ep_msg2ep __user *argp = (struct ep_msg2ep __user *)arg;
	struct bsb_sub_device *subdevice;
	struct mutex *mutex;
	struct bsb_reg *bsb_reg = bsb_ep->bar[bsb_ep->reg_bar];
	int nr_subdevices = READ_ONCE(bsb_reg->nr_subdevices);
	struct ep_msg2ep *ep_msg2ep = vmemdup_user(argp, sizeof(*ep_msg2ep));
	struct msg_info *msg2ep;
	struct device *dev = bsb_ep->dev;
	size_t msg2ep_total_size_per_subdevice =
		bsb_ep->msg2ep_total_size / nr_subdevices;

	if (IS_ERR(ep_msg2ep))
		return PTR_ERR(ep_msg2ep);

	msg2ep = &ep_msg2ep->msg2ep;

	subdevice_idx = ep_msg2ep->subdevice_idx;
	subdevice_idx = array_index_nospec(subdevice_idx, nr_subdevices);

	subdevice = &bsb_ep->subdevices[subdevice_idx];
	mutex = &subdevice->ep_wait_rc_mutex;

	mutex_lock(mutex);
	WRITE_ONCE(bsb_reg->ep_waiting_rc[subdevice_idx], true);
	wait_event(subdevice_wq, subdevice->subdevice_received == true);

	msg2ep->size = READ_ONCE(bsb_reg->sz_msg2ep[subdevice_idx]);
	WARN_ON(!msg2ep->size);
	if (!msg2ep->size) {
		dev_err(dev,
			"invalid msg2ep->size(0x0), subdevice->subdevice_received is %d\n",
			subdevice->subdevice_received);
		ret = -EINVAL;
		goto unlock;
	}
	if (msg2ep->size > msg2ep_total_size_per_subdevice) {
		dev_dbg(dev, "msg2ep->size(%lx) is too large\n", msg2ep->size);
		ret = -EINVAL;
		goto unlock;
	}
	WRITE_ONCE(bsb_reg->sz_msg2ep[subdevice_idx], 0);

	/* TODO: check if copy correctly */
	ret = copy_to_user(msg2ep->base,
			   bsb_ep->msg2ep_base +
				   msg2ep_total_size_per_subdevice *
					   subdevice_idx,
			   msg2ep->size);
	if (ret) {
		ret = -EFAULT;
		goto unlock;
	}

	/* Let userspace know msg size */
	ret = put_user(msg2ep->size, &argp->msg2ep.size);
	if (ret)
		goto unlock;

	subdevice->subdevice_received = false;
	WRITE_ONCE(bsb_reg->ep_waiting_rc[subdevice_idx], false);

unlock:
	mutex_unlock(mutex);

	kvfree(ep_msg2ep);

	return ret;
}

static long bsb_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = -ENOTTY;

	switch (cmd) {
	case BSB_EP_GET_RC_SUBDEVICES_INFO:
		return bsb_ep_get_rc_subdevices_info(arg);
	case BSB_EP_XFER:
		return bsb_ep_xfer(arg);
	case BSB_EP_SEND_MSG2RC_INTERRUPT_RC:
		return bsb_ep_send_msg2rc_interrupt_rc(arg);
	case BSB_EP_WAIT_RC_MSG2EP:
		return bsb_ep_wait_rc_msg2ep(arg);
	}

	return ret;
}

static const struct file_operations bsb_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = bsb_ioctl,
};

static int pci_bsb_ep_init_dma(struct bsb_ep *bsb_ep)
{
	struct device *dev = &bsb_ep->epf->epc->dev;

	bsb_ep->dma_chan_tx = ambarella_acquire_udma_chan(DMA_MEM_TO_DEV, dev);
	if (!bsb_ep->dma_chan_tx)
		return -ENODEV;
	bsb_ep->dma_chan_rx = ambarella_acquire_udma_chan(DMA_DEV_TO_MEM, dev);
	if (!bsb_ep->dma_chan_rx)
		return -ENODEV;

	return 0;
}

static void pci_bsb_cleanup_dma(struct bsb_ep *bsb_ep)
{
	dma_release_channel(bsb_ep->dma_chan_tx);
	bsb_ep->dma_chan_tx = NULL;
	dma_release_channel(bsb_ep->dma_chan_rx);
	bsb_ep->dma_chan_rx = NULL;
}

static int pci_bsb_ep_set_bar(struct pci_epf *epf)
{
	int bar, add, ret;
	struct pci_epf_bar *epf_bar;
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct bsb_ep *bsb_ep = epf_get_drvdata(epf);
	enum pci_barno reg_bar = bsb_ep->reg_bar;
	const struct pci_epc_features *epc_features;

	epc_features = bsb_ep->epc_features;

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
			pci_epf_free_space(epf, bsb_ep->bar[bar], bar,
					   PRIMARY_INTERFACE);
			dev_err(dev, "Failed to set BAR%d", bar);
			if (bar == reg_bar)
				return ret;
		}
	}

	return 0;
}

static int pci_bsb_ep_core_init(struct pci_epf *epf)
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

	ret = pci_bsb_ep_set_bar(epf);
	if (ret)
		return ret;

	return 0;
}

static int pci_bsb_ep_alloc_space(struct pci_epf *epf)
{
	struct bsb_ep *bsb_ep = epf_get_drvdata(epf);
	struct device *dev = &epf->dev;
	size_t reg_bar_size;
	void *base;
	enum pci_barno reg_bar = bsb_ep->reg_bar;
	enum pci_barno msg_bar = bsb_ep->msg_bar;
	const struct pci_epc_features *epc_features;
	size_t notify_msg_reg_size, mem_size;

	epc_features = bsb_ep->epc_features;
	reg_bar_size = ALIGN(sizeof(struct bsb_reg), 128);

	notify_msg_reg_size = reg_bar_size;

	if (!epc_features->bar_fixed_size[reg_bar]) {
		dev_err(dev, "%s: failed to get reg bar\n", __func__);
		return -ENODEV;
	}
	if (!epc_features->bar_fixed_size[msg_bar]) {
		dev_err(dev, "%s: failed to get mem bar\n", __func__);
		return -ENODEV;
	}

	if (notify_msg_reg_size > epc_features->bar_fixed_size[bsb_ep->reg_bar])
		return -ENOMEM;

	notify_msg_reg_size = epc_features->bar_fixed_size[bsb_ep->reg_bar];
	mem_size = epc_features->bar_fixed_size[bsb_ep->msg_bar];

	/* Init reg bar */
	base = pci_epf_alloc_space(epf, notify_msg_reg_size, reg_bar,
				   epc_features->align, PRIMARY_INTERFACE);

	if (!base) {
		dev_err(dev, "Failed to allocated register space(reg)\n");
		return -ENOMEM;
	}
	bsb_ep->bar[reg_bar] = base;

	/* Init mem bar */
	base = pci_epf_alloc_space(epf, mem_size, msg_bar, epc_features->align,
				   PRIMARY_INTERFACE);

	if (!base) {
		dev_err(dev, "Failed to allocated register space(mem)\n");
		pci_epf_free_space(epf, bsb_ep->bar[reg_bar], reg_bar,
				   PRIMARY_INTERFACE);
		return -ENOMEM;
	}
	bsb_ep->bar[msg_bar] = base;
	bsb_ep->msg_bar_size = mem_size;

	bsb_ep->msg2ep_base = base;
	bsb_ep->msg2ep_total_size = mem_size / 2;
	bsb_ep->msg2rc_base = base + bsb_ep->msg2ep_total_size;
	bsb_ep->msg2rc_total_size = mem_size / 2;

	return 0;
}

static void pci_epf_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	/*
	 * TODO: check if we can get epc from desc->dev like
	 * struct pci_epc *epc = container_of(desc->dev, struct pci_epc, dev);
	 */
	struct pci_epf *epf = global_epf;
	struct bsb_ep *bsb_ep = epf_get_drvdata(epf);

	bsb_ep->msi_doorbell_property.msg = *msg;
}

static irqreturn_t bsb_interrupt_handler(int irq, void *dev_id)
{
	struct bsb_reg *bsb_reg = bsb_ep->bar[bsb_ep->reg_bar];
	int i;
	int nr_subdevices = READ_ONCE(bsb_reg->nr_subdevices);
	struct bsb_sub_device *subdevice;

	for (i = 0; i < nr_subdevices; i++)
		if (READ_ONCE(bsb_reg->wakeup_ep[i])) {
			subdevice = &bsb_ep->subdevices[i];
			subdevice->subdevice_received = true;
			WRITE_ONCE(bsb_reg->wakeup_ep[i], 0);
		}

	wake_up(&subdevice_wq);

	return IRQ_HANDLED;
}

static int pci_bsb_ep_drv_bind(struct pci_epf *epf)
{
	int ret;
	struct bsb_ep *bsb_ep = epf_get_drvdata(epf);
	const struct pci_epc_features *epc_features;
	enum pci_barno reg_bar = BSB_REG_BAR;
	enum pci_barno msg_bar = BSB_MSG_BAR;
	enum pci_barno msi_doorbell_bar = BSB_MSI_DOORBELL_BAR;
	struct pci_epc *epc = epf->epc;
	struct ambarella_msi_doorbell_property *msi_doorbell_property =
		&bsb_ep->msi_doorbell_property;
	struct bsb_reg *bsb_reg = bsb_ep->bar[bsb_ep->reg_bar];

	if (WARN_ON_ONCE(!epc))
		return -EINVAL;

	if (WARN_ON_ONCE(!bsb_ep))
		return -EINVAL;

	epc_features = pci_epc_get_features(epc, epf->func_no, epf->vfunc_no);
	if (epc_features)
		ambarella_ep_configure_bar(epf, epc_features);
	else {
		dev_err(&epf->dev, "%s: failed to get epc_features\n",
			__func__);
		return -EINVAL;
	}

	bsb_ep->reg_bar = reg_bar;
	bsb_ep->msg_bar = msg_bar;
	bsb_ep->epc_features = epc_features;

	if (bsb_ep->cap & FLAG_SUPPORT_MSI_DOORBELL) {
		msi_doorbell_property->interrupt_handler =
			bsb_interrupt_handler;
		msi_doorbell_property->pci_epf_write_msi_msg =
			pci_epf_write_msi_msg;
		msi_doorbell_property->msi_doorbell_bar = msi_doorbell_bar;
		msi_doorbell_property->msi_doorbell_bar_size = PAGE_SIZE;

		ret = pci_epf_configure_msi_doorbell(msi_doorbell_property, epf,
						     epc_features);
		if (ret)
			return ret;
		/*
		 * XXX: there is no way to get msi bar vaddr, because it's
		 * allocated on platform MSI driver side.
		 */
		bsb_ep->bar[msi_doorbell_bar] = NULL;
	}

	ret = pci_bsb_ep_alloc_space(epf);
	if (ret)
		return ret;

	ret = pci_bsb_ep_core_init(epf);
	if (ret)
		return ret;

	ret = pci_bsb_ep_init_dma(bsb_ep);
	if (ret)
		return ret;

	if (bsb_ep->cap & FLAG_SUPPORT_MSI_DOORBELL) {
		bsb_reg = bsb_ep->bar[bsb_ep->reg_bar];
		WRITE_ONCE(bsb_reg->db_bar,
			   bsb_ep->msi_doorbell_property.msi_doorbell_bar);
		WRITE_ONCE(bsb_reg->db_offset, 0);
		WRITE_ONCE(bsb_reg->flags, bsb_ep->cap);
		WRITE_ONCE(bsb_reg->db_data, 0xdb);
	}

	return 0;
}

static void pci_bsb_ep_drv_unbind(struct pci_epf *epf)
{
	struct bsb_ep *bsb_ep = epf_get_drvdata(epf);
	struct pci_epc *epc = epf->epc;
	struct pci_epf_bar *epf_bar;
	int bar;

	cancel_delayed_work(&bsb_ep->cmd_handler);
	pci_bsb_cleanup_dma(bsb_ep);
	pci_epc_stop(epc);
	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		epf_bar = &epf->bar[bar];

		if (bsb_ep->bar[bar]) {
			pci_epc_clear_bar(epc, epf->func_no, epf->vfunc_no,
					  epf_bar);
			pci_epf_free_space(epf, bsb_ep->bar[bar], bar,
					   PRIMARY_INTERFACE);
		}
	}

	pci_epf_free_msi_doorbell(epf, bsb_ep->msi_doorbell_property.virq);
}

static const struct pci_epf_device_id pci_bsb_ep_dev_ids[] = {
	{
		.name = BSB_DRIVER_NAME,
	},
	{},
};

struct bsb_ep_driverdata {
	int cap;
};

static const struct bsb_ep_driverdata cv72_data = {
	.cap = FLAG_SUPPORT_MSI_DOORBELL,
};

static const struct soc_device_attribute bsb_ep_soc_info[] = {
	{ .soc_id = "cv72", .data = (void *)&cv72_data },
	{ /* sentinel */ },
};

static int pci_bsb_ep_drv_probe(struct pci_epf *epf)
{
	struct device *dev = &epf->dev;
	struct device_node *ep_controller_node;
	struct platform_device *parent_pdev;
	struct miscdevice *misc_device;
	int ret, id, i;
	const struct soc_device_attribute *soc;
	const struct bsb_ep_driverdata *soc_data = NULL;

	global_epf = epf;

	/*
	 * We cannot know nr_subdevice on EP side when probe. And it's
	 * too ugly to alloc it when doing ioctl, so let's pre-allocate here.
	 * XXX: let's realloc(reduce size) in ioctl?
	 */
	bsb_ep =
		devm_kzalloc(dev,
			     struct_size(bsb_ep, subdevices, MAX_NR_SUBDEVICES),
			     GFP_KERNEL);
	if (!bsb_ep)
		return -ENOMEM;

	soc = soc_device_match(bsb_ep_soc_info);
	if (soc) {
		soc_data = soc->data;
		bsb_ep->cap = soc_data->cap;
		if ((!IS_ENABLED(CONFIG_AMBARELLA_MSI_DETECTION_DOORBELL) ||
		     !(bsb_ep->cap & FLAG_SUPPORT_MSI_DOORBELL)) &&
		    doorbell_method == DOORBELL_VIA_MSI) {
			dev_err(dev,
				"invalid doorbell_method, cap is %d, please make"
				"sure CONFIG_AMBARELLA_MSI_DETECTION_DOORBELL is on, and dts is correct\n",
				bsb_ep->cap);
			return -EINVAL;
		}
	}

	mutex_init(&bsb_ep->send_msg2rc_interrupt_rc_mutex);

	for (i = 0; i < MAX_NR_SUBDEVICES; i++)
		mutex_init(&bsb_ep->subdevices[i].ep_wait_rc_mutex);

	epf->header = &default_epf_header;
	bsb_ep->epf = epf;

	epf_set_drvdata(epf, bsb_ep);

	/*set BSB_DRIVER_NAME coherent_mask to utilize cma*/
	dma_set_coherent_mask(&epf->dev, DMA_BIT_MASK(64));

	misc_device = &bsb_ep->miscdev;
	id = ida_simple_get(&bsb_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		pr_err("Unable to get id\n");
		return ret;
	}

	misc_device->name = kasprintf(GFP_KERNEL, BSB_DRIVER_NAME ".%d", id);
	if (!misc_device->name) {
		ret = -ENOMEM;
		goto err_ida_remove;
	}

	misc_device = &bsb_ep->miscdev;
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

	misc_device->fops = &bsb_fops;

	misc_device->parent = &parent_pdev->dev;
	ret = misc_register(misc_device);
	if (ret) {
		pr_err("Failed to register device\n");
		ret = -EINVAL;
		goto free_miscdev_name;
	}
	dev = misc_device->this_device;

	bsb_ep->dev = misc_device->this_device;

	return 0;

free_miscdev_name:
	kfree(misc_device->name);
err_ida_remove:
	ida_simple_remove(&bsb_ida, id);
	return ret;
}

static struct pci_epf_ops ops = {
	.unbind = pci_bsb_ep_drv_unbind,
	.bind = pci_bsb_ep_drv_bind,
	.set_bar = pci_bsb_ep_set_bar,
};

static struct pci_epf_driver pci_bsb_ep_driver = {
	.driver.name = BSB_DRIVER_NAME,
	.probe = pci_bsb_ep_drv_probe,
	.id_table = pci_bsb_ep_dev_ids,
	.ops = &ops,
	.owner = THIS_MODULE,
};

static const struct of_device_id bsb_match[] = {
	{
		.compatible = "ambarella,bsb_ep",
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, bsb_match);

static int bsb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret, irq;

	if (doorbell_method != DOORBELL_VIA_GPIO)
		return 0;

	irq = ret = platform_get_irq(pdev, 0);
	if (ret < 0) {
		dev_err(dev, "failed to get irq, ret %d\n", ret);
		return -EINVAL;
	}
	dev_info(dev, "%s probed successfully\n", __func__);

	ret = devm_request_irq(dev, irq, bsb_interrupt_handler,
			       IRQ_TYPE_EDGE_RISING, "bsb", NULL);
	if (ret) {
		dev_err(dev, "failed to request IRQ %d\n", irq);
		return ret;
	}
	return 0;
}

static struct platform_driver bsb_platform_driver = {
	.driver = {
		.name = "ambarella,bsb_ep",
		.of_match_table = bsb_match,
	},
	.probe = bsb_probe,
};

static int __init pci_bsb_ep_drv_init(void)
{
	int ret;

	ret = platform_driver_register(&bsb_platform_driver);
	if (ret) {
		pr_err("failed to register bsb platform driver");
		return ret;
	}

	ret = pci_epf_register_driver(&pci_bsb_ep_driver);
	if (ret) {
		pr_err("Failed to register bsb driver --> %d", ret);
		return ret;
	}

	pr_info("%s: register bsb EP driver successfully\n", __func__);

	return 0;
}
module_init(pci_bsb_ep_drv_init);

static void __exit pci_bsb_ep_drv_exit(void)
{
	struct miscdevice *misc_device;
	int id;

	misc_device = &bsb_ep->miscdev;
	if (sscanf(misc_device->name, BSB_DRIVER_NAME ".%d", &id) != 1) {
		pr_err("invalid name\n");
		return;
	}
	if (id < 0) {
		pr_err("invalid id\n");
		return;
	}

	kfree(misc_device->name);
	misc_deregister(misc_device);
	ida_simple_remove(&bsb_ida, id);
	pci_epf_unregister_driver(&pci_bsb_ep_driver);
}
module_exit(pci_bsb_ep_drv_exit);

MODULE_DESCRIPTION("PCI BSB FUNC DRIVER");
MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_LICENSE("GPL v2");
