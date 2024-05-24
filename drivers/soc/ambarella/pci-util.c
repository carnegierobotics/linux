// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PCIe utility functions
 *
 * History: 2023/06/15 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2023 by Ambarella, Inc.
 */

#include <soc/ambarella/pci-util.h>
#include <soc/ambarella/misc.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct udma_filter {
	struct device *dev;
	u32 dma_mask;
};

/*
 * Used by udma, epf-test, excalibur_rc, excalibur_ep and etc.
 *
 * This function filters all dma channels to find udma channel.
 */
bool ambarella_is_cdns_udma(struct dma_chan *chan, void *data)
{
	struct device *filter_dev = data;

	WARN_ON_ONCE(!filter_dev);
	dev_dbg(chan->device->dev, "Hi 1\n");
	if (chan->device->dev->parent)
		dev_dbg(chan->device->dev->parent, "Hi 2\n");
	dev_dbg(filter_dev, "Hi 3\n");
	if (filter_dev->parent)
		dev_dbg(filter_dev->parent, "Hi 4\n");
	return filter_dev == chan->device->dev->parent;
}
EXPORT_SYMBOL(ambarella_is_cdns_udma);

/*
 * ugly way to get pcie controller device
 */
struct device *ambarella_get_pcie_root_complex(struct device *dev)
{
	return dev_is_platform(dev) ?
		       dev :
		       ambarella_get_pcie_root_complex(dev->parent);
}
EXPORT_SYMBOL(ambarella_get_pcie_root_complex);

struct dma_chan *ambarella_acquire_udma_chan(enum dma_transfer_direction dir,
					     struct device *dev)
{
	dma_cap_mask_t mask;
	struct udma_filter filter;
	struct dma_chan *dma_chan;

	if (dir == DMA_MEM_TO_DEV)
		filter.dma_mask = BIT(DMA_DEV_TO_MEM);
	else if (dir == DMA_DEV_TO_MEM)
		filter.dma_mask = BIT(DMA_MEM_TO_DEV);
	else
		return NULL;

	if (!dev) {
		pr_err("%s: invalid dev, pls check your EPC, which may still not be registered\n",
		       __func__);
		return NULL;
	}
	filter.dev = dev->parent;
	if (!dev->parent) {
		dev_err(dev, "%s: invalid parent\n", __func__);
		return NULL;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	dma_chan =
		dma_request_channel(mask, ambarella_is_cdns_udma, dev->parent);

	if (!dma_chan) {
		dev_info(dev, "Failed to get uDMA Channel\n");
		return NULL;
	}
	return dma_chan;
}
EXPORT_SYMBOL(ambarella_acquire_udma_chan);

int ambarella_pci_udma_xfer(struct device *dev, dma_addr_t dma_dst,
			    dma_addr_t dma_src, u32 total_len,
			    enum dma_transfer_direction dir,
			    struct dma_chan *chan, dma_callback_t callback,
			    void __iomem *msginfo)
{
	int ret = 0;
	enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	dma_addr_t dma_local = (dir == DMA_MEM_TO_DEV) ? dma_src : dma_dst;
	dma_addr_t dma_remote = (dir == DMA_MEM_TO_DEV) ? dma_dst : dma_src;
	struct scatterlist *local_sg, *remote_sg;
	int nr_chunk = 1;

	if (!chan || !chan->device || !chan->device->device_prep_slave_sg)
		return -EINVAL;

	/* TODO: provide sg support in ambarella/epf-core.c */
	local_sg = kcalloc(nr_chunk, sizeof(*local_sg), GFP_KERNEL);
	if (!local_sg)
		return -ENOMEM;

	remote_sg = kcalloc(nr_chunk, sizeof(*remote_sg), GFP_KERNEL);
	if (!remote_sg) {
		ret = -ENOMEM;
		goto free_local_sg;
	}
	sg_init_table(local_sg, nr_chunk);
	sg_init_table(remote_sg, nr_chunk);
	sg_dma_address(&local_sg[0]) = dma_local;
	sg_dma_address(&remote_sg[0]) = dma_remote;
	sg_dma_len(&local_sg[0]) = total_len;
	sg_dma_len(&remote_sg[0]) = total_len;

	tx = chan->device->device_prep_slave_sg(chan, local_sg, nr_chunk, dir,
						flags, remote_sg);

	if (!tx) {
		ret = -EIO;
		goto free_remote_sg;
	}

	cookie = tx->tx_submit(tx);
	tx->callback = callback;
	tx->callback_param = msginfo;
	ret = dma_submit_error(cookie);
	if (ret) {
		dev_err(dev, "Failed to do DMA tx_submit %d\n", cookie);
		ret = -EIO;
		goto free_remote_sg;
	}

	dma_async_issue_pending(chan);

free_remote_sg:
	kfree(remote_sg);
free_local_sg:
	kfree(local_sg);

	return 0;
}
EXPORT_SYMBOL(ambarella_pci_udma_xfer);

/**
 * copy_to_user_fromio - copy data from mmio-space to user-space
 * @dst: the destination pointer on user-space
 * @src: the source pointer on mmio
 * @count: the data size to copy in bytes
 *
 * Copies the data from mmio-space to user-space.
 *
 * Return: Zero if successful, or non-zero on failure.
 */
int ambarella_copy_to_user_fromio(void __user *dst,
				  const volatile void __iomem *src,
				  size_t count)
{
	char buf[256];
	while (count) {
		size_t c = count;
		if (c > sizeof(buf))
			c = sizeof(buf);
		memcpy_fromio(buf, (void __iomem *)src, c);
		if (copy_to_user(dst, buf, c))
			return -EFAULT;
		count -= c;
		dst += c;
		src += c;
	}
	return 0;
}
EXPORT_SYMBOL(ambarella_copy_to_user_fromio);

/**
 * copy_from_user_toio - copy data from user-space to mmio-space
 * @dst: the destination pointer on mmio-space
 * @src: the source pointer on user-space
 * @count: the data size to copy in bytes
 *
 * Copies the data from user-space to mmio-space.
 *
 * Return: Zero if successful, or non-zero on failure.
 */
int ambarella_copy_from_user_toio(volatile void __iomem *dst,
				  const void __user *src, size_t count)
{
	char buf[256];
	while (count) {
		size_t c = count;
		if (c > sizeof(buf))
			c = sizeof(buf);
		if (copy_from_user(buf, src, c))
			return -EFAULT;
		memcpy_toio(dst, buf, c);
		count -= c;
		dst += c;
		src += c;
	}
	return 0;
}
EXPORT_SYMBOL(ambarella_copy_from_user_toio);

/**
 * copy_to_user_fromio - copy data from mmio-space to user-space
 * @dst: the destination pointer on user-space
 * @src: the source pointer on mmio
 * @count: the data size to copy in bytes
 *
 * Copies the data from mmio-space to user-space.
 *
 * Return: Zero if successful, or non-zero on failure.
 */
int ambarella_copy_to_user_fromio_l(void __user *dst,
				    const volatile void __iomem *src,
				    size_t count)
{
	char buf[256];
	while (count) {
		size_t c = count;
		if (c > sizeof(buf))
			c = sizeof(buf);
		memcpy_fromio_ambarella(buf, (void __iomem *)src, c);
		if (copy_to_user(dst, buf, c))
			return -EFAULT;
		count -= c;
		dst += c;
		src += c;
	}
	return 0;
}
EXPORT_SYMBOL(ambarella_copy_to_user_fromio_l);

/**
 * copy_from_user_toio - copy data from user-space to mmio-space
 * @dst: the destination pointer on mmio-space
 * @src: the source pointer on user-space
 * @count: the data size to copy in bytes
 *
 * Copies the data from user-space to mmio-space.
 *
 * Return: Zero if successful, or non-zero on failure.
 */
int ambarella_copy_from_user_toio_l(volatile void __iomem *dst,
				    const void __user *src, size_t count)
{
	char buf[256];
	while (count) {
		size_t c = count;
		if (c > sizeof(buf))
			c = sizeof(buf);
		if (copy_from_user(buf, src, c))
			return -EFAULT;
		memcpy_toio_ambarella(dst, buf, c);
		count -= c;
		dst += c;
		src += c;
	}
	return 0;
}
EXPORT_SYMBOL(ambarella_copy_from_user_toio_l);
