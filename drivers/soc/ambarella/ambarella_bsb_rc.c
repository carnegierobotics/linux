// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ambarella BSB endpoint function pci RC-side driver.
 *
 * History: 2023/8/10 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2023 by Ambarella, Inc.
 *
 * Abbrev:
 *
 * reo: ep ob codes run under RC-side kernel
 * rei: ep ib codes run under RC-side kernel
 *
 * reg bar: bar used to store epf's register, like size, addr and etc.
 *
 */

#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/resource.h>
#include <linux/of_platform.h>
#include <soc/ambarella/epf-core.h>
#include <soc/ambarella/bsb.h>
#include <soc/ambarella/pci-util.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/of_reserved_mem.h>
#include <linux/moduleparam.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/nospec.h>
#include <uapi/linux/amba-bsb.h>

bsb_module_parameters;

static unsigned long nr_subdevices = 4;
static DECLARE_WAIT_QUEUE_HEAD(ioctl_wq);
module_param(nr_subdevices, ulong, 0644);
MODULE_PARM_DESC(nr_subdevices, "timeout when poll");
static unsigned int gpio;
struct rc_subdevice_rmem {
	phys_addr_t start_addr;
	phys_addr_t size;
};
static struct rc_subdevice_rmem *resources;

#define STATUS_IRQ_RAISED BIT(6)

#define PCI_ENDPOINT_BSB_STATUS 0x8

#define EPF_DRV_MODULE_NAME "bsb-rc"
#define IRQ_NUM 1

static DEFINE_IDA(ida);

struct bsb_sub_device {
	struct resource res;
	struct miscdevice misc_device;
	struct mutex mutex;
	bool irq_received;
	int id;
};

struct bsb_rc {
	unsigned int gpio;
	int cap;
	struct pci_dev *pdev;
	struct device *dev;
	enum pci_barno reg_bar;
	void __iomem *msg2ep_base;
	size_t msg2ep_total_size;
	size_t msg2ep_total_size_per_subdevice;
	void __iomem *msg2rc_base;
	size_t msg2rc_total_size;
	size_t msg2rc_total_size_per_subdevice;
	void __iomem *reg_base;
	void __iomem *bar[PCI_STD_NUM_BARS];
	struct bsb_sub_device subdevices[];
};
static struct bsb_rc *bsb_rc;

/*
 * TODO: use INTx + message, see
 * cdns_pcie_set_outbound_region_for_normal_msg
 * and cdns_pcie_ep_assert_intx
 */
static irqreturn_t pci_endpoint_bsb_irqhandler(int irq, void *dev_id)
{
	int i;
	struct bsb_reg __iomem *bsb_reg = bsb_rc->bar[bsb_rc->reg_bar];

	for (i = 0; i < nr_subdevices; i++)
		if (readl(&bsb_reg->wakeup_rc[i])) {
			bsb_rc->subdevices[i].irq_received = true;
			writel(0, &bsb_reg->wakeup_rc[i]);
		}

	wake_up(&ioctl_wq);

	return IRQ_HANDLED;
}

static int doorbell_gpio(struct bsb_rc *bsb_rc)
{
	int gpio = bsb_rc->gpio;

	if (!gpio_is_valid(gpio))
		return gpio;

	gpio_set_value(gpio, 1);
	gpio_set_value(gpio, 0);

	return 0;
}

static int doorbell_msi(struct bsb_rc *bsb_rc)
{
	enum pci_barno db_bar;
	u32 data;
	u32 offset;
	struct bsb_reg __iomem *bsb_reg = bsb_rc->reg_base;
	void __iomem *db_msi_base;

	if (!(bsb_rc->cap & FLAG_SUPPORT_MSI_DOORBELL))
		return -EINVAL;

	db_bar = readl(&bsb_reg->db_bar);
	if (db_bar == NO_BAR)
		return -EINVAL;

	db_msi_base = bsb_rc->bar[db_bar];

	data = readl(&bsb_reg->db_data);
	offset = readl(&bsb_reg->db_offset);

	writel(data, db_msi_base + offset);
	return 0;
}

static long bsb_rc_msg2ep_then_wait_for_msg2rc(unsigned long arg)
{
	struct rc_msg2ep_and_msg2rc __user *argp = (void __user *)arg;
	struct rc_msg2ep_and_msg2rc *msg2ep_and_msg2rc;
	int ret = 0, subdevice_idx;
	struct bsb_reg __iomem *bsb_reg = bsb_rc->bar[bsb_rc->reg_bar];
	struct mutex *mutex;
	struct msg_info *msg2ep, *msg2rc;
	struct device *dev = bsb_rc->dev;
	size_t msg2rc_total_size_per_subdevice =
		bsb_rc->msg2rc_total_size_per_subdevice;
	size_t msg2ep_total_size_per_subdevice =
		bsb_rc->msg2ep_total_size_per_subdevice;

	msg2ep_and_msg2rc = vmemdup_user(argp, sizeof(*msg2ep_and_msg2rc));

	if (IS_ERR(msg2ep_and_msg2rc))
		return PTR_ERR(msg2ep_and_msg2rc);

	msg2ep = &msg2ep_and_msg2rc->msg2ep;
	msg2rc = &msg2ep_and_msg2rc->msg2rc;

	if (!msg2ep->size) {
		dev_dbg(dev, "invalid msg2ep->size(0x0)\n");
		ret = -EINVAL;
		goto free_vmemdup_user;
	}
	if (msg2ep->size > bsb_rc->msg2ep_total_size) {
		dev_dbg(dev, "msg2ep->size(%lx) is too large\n", msg2ep->size);
		ret = -EINVAL;
		goto free_vmemdup_user;
	}

	subdevice_idx = msg2ep_and_msg2rc->subdevice_idx;
	subdevice_idx = array_index_nospec(subdevice_idx, nr_subdevices);
	if (!readl(&bsb_reg->ep_waiting_rc[subdevice_idx])) {
		ret = -ESRCH;
		goto free_vmemdup_user;
	}

	mutex = &bsb_rc->subdevices[subdevice_idx].mutex;

	/*
	 *  TODO: allow concurrency
	 */
	mutex_lock(mutex);

	ret = ambarella_copy_from_user_toio_l(
		bsb_rc->msg2ep_base +
			msg2ep_total_size_per_subdevice * subdevice_idx,
		msg2ep->base, msg2ep->size);
	if (ret)
		goto unlock;

	writel(1, &bsb_reg->wakeup_ep[subdevice_idx]);
	writel(msg2ep->size, &bsb_reg->sz_msg2ep[subdevice_idx]);

	if (doorbell_method == DOORBELL_VIA_GPIO)
		ret = doorbell_gpio(bsb_rc);
	else
		ret = doorbell_msi(bsb_rc);
	if (ret)
		goto unlock;

	writel(true, &bsb_reg->waiting_ep[subdevice_idx]);

	/* Wait for EP msg */
	wait_event(ioctl_wq,
		   bsb_rc->subdevices[subdevice_idx].irq_received == true);

	/* let's copy msg2rc to userspace */
	msg2rc->size = readl(&bsb_reg->sz_msg2rc[subdevice_idx]);
	if (!msg2rc->size || msg2rc->size > bsb_rc->msg2rc_total_size) {
		dev_err(dev, "invalid msg2rc->size: 0x%lx, subdevice_idx is %d",
			msg2rc->size, subdevice_idx);
		ret = -EINVAL;
		goto unlock;
	}

	writel(0, &bsb_reg->sz_msg2rc[subdevice_idx]);

	ret = ambarella_copy_to_user_fromio_l(
		msg2rc->base,
		bsb_rc->msg2rc_base +
			msg2rc_total_size_per_subdevice * subdevice_idx,
		msg2rc->size);
	if (ret)
		goto unlock;

	/* Let userspace know msg size */
	ret = put_user(msg2rc->size, &argp->msg2rc.size);
	if (ret)
		goto unlock;

	bsb_rc->subdevices[subdevice_idx].irq_received = false;
	writel(false, &bsb_reg->waiting_ep[subdevice_idx]);

unlock:
	mutex_unlock(mutex);
free_vmemdup_user:
	kvfree(msg2ep_and_msg2rc);

	return ret;
}

static long bsb_rc_get_rc_subdevices_info(unsigned long arg)
{
	int i, ret;
	struct subdevices_info __user *argp = (void __user *)arg;
	struct subdevices_info *subdevices_info;
	struct bsb_reg *bsb_reg = bsb_rc->bar[bsb_rc->reg_bar];
	struct reg_subdevice_rmem *rmem;
	struct device *dev = bsb_rc->dev;

	subdevices_info = vmalloc(sizeof(struct subdevices_info));
	if (!subdevices_info)
		return -ENOMEM;

	subdevices_info->nr_subdevices = readl(&bsb_reg->nr_subdevices);
	for (i = 0; i < subdevices_info->nr_subdevices; i++) {
		rmem = &bsb_reg->subdevice_rmem[i];
		subdevices_info->subdevice_rmem[i].start_addr =
			readl(&rmem->lower_start_addr) |
			((dma_addr_t)(readl(&rmem->upper_start_addr)) << 32);
		subdevices_info->subdevice_rmem[i].size = readl(&rmem->size);
	}
	ret = copy_to_user(argp, subdevices_info, sizeof(*subdevices_info));
	vfree(subdevices_info);
	if (ret) {
		dev_dbg(dev, "%s: failed to copy_to_user\n", __func__);
		ret = -EFAULT;
	}

	return ret;
}

static long bsb_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = -ENOTTY;

	switch (cmd) {
	case BSB_RC_MSG2EP_THEN_WAIT_FOR_MSG2RC:
		return bsb_rc_msg2ep_then_wait_for_msg2rc(arg);
	case BSB_RC_GET_RC_SUBDEVICES_INFO:
		return bsb_rc_get_rc_subdevices_info(arg);
	}

	return ret;
}

static const struct file_operations bsb_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = bsb_ioctl,
};

static int bsb_init_subdevices(struct bsb_rc *bsb_rc, struct device *parent_dev)
{
	struct miscdevice *misc_device;
	int i, err, j;

	for (i = 0; i < nr_subdevices; i++) {
		misc_device = &bsb_rc->subdevices[i].misc_device;
		bsb_rc->subdevices[i].id =
			ida_simple_get(&ida, 0, 0, GFP_KERNEL);
		if (bsb_rc->subdevices[i].id < 0) {
			err = bsb_rc->subdevices[i].id;
			pr_err("%s: Unable to get id\n", __func__);
			goto err_ida_remove;
		}

		misc_device->name = devm_kasprintf(bsb_rc->dev, GFP_KERNEL,
						   EPF_DRV_MODULE_NAME ".%d",
						   bsb_rc->subdevices[i].id);
		if (!misc_device->name) {
			err = -ENOMEM;
			goto err_ida_remove;
		}

		misc_device->minor = MISC_DYNAMIC_MINOR;

		misc_device->fops = &bsb_fops;

		misc_device->parent = parent_dev;
		err = misc_register(misc_device);
		if (err) {
			pr_err("%s: Failed to register device\n", __func__);
			err = -EINVAL;
			goto deregister_misc;
		}
		mutex_init(&bsb_rc->subdevices[i].mutex);
	}

	return 0;

deregister_misc:
	for (j = 0; j < i; j++)
		misc_deregister(&bsb_rc->subdevices[j].misc_device);
err_ida_remove:
	for (j = 0; j < i; j++)
		ida_simple_remove(&ida, bsb_rc->subdevices[j].id);
	return err;
}

static void bsb_rc_configure_subdevice(struct bsb_rc *bsb_rc)
{
	int i;
	struct bsb_reg __iomem *bsb_reg = bsb_rc->bar[bsb_rc->reg_bar];

	writel(nr_subdevices, &bsb_reg->nr_subdevices);
	for (i = 0; i < nr_subdevices; i++) {
		writel(lower_32_bits(resources[i].start_addr),
		       &bsb_reg->subdevice_rmem[i].lower_start_addr);
		writel(upper_32_bits(resources[i].start_addr),
		       &bsb_reg->subdevice_rmem[i].upper_start_addr);
		/* XXX: Assume size is never over 4GB */
		writel(lower_32_bits(resources[i].size),
		       &bsb_reg->subdevice_rmem[i].size);
	}
}

static int bsb_rc_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int err = 0, i, irq_num;
	enum pci_barno bar;
	void __iomem *base = NULL;
	struct device *dev = &pdev->dev;
	enum pci_barno bsb_reg_bar = BSB_REG_BAR;
	enum pci_barno bsb_msg_bar = BSB_MSG_BAR;
	struct bsb_rc *bsb_test;
	resource_size_t msg_bar_size = 0;
	struct bsb_reg __iomem *bsb_reg;

	if (!resources)
		return -EPROBE_DEFER;

	ambarella_rc_helper_init(BSB_PCIE_DEVICE_ID);

	dev->parent = ambarella_get_pcie_root_complex(dev);
	dev_info(dev, "parent is %s now\n", dev_name(dev->parent));
	if (pci_is_bridge(pdev))
		return -ENODEV;

	if (nr_subdevices > MAX_NR_SUBDEVICES) {
		dev_err(dev,
			"Invalid nr_subdevices(%ld), we can at mostly support %d misdevices\n",
			nr_subdevices, MAX_NR_SUBDEVICES);
		return -EINVAL;
	}
	bsb_rc = devm_kzalloc(dev,
			      struct_size(bsb_rc, subdevices, nr_subdevices),
			      GFP_KERNEL);
	if (!bsb_rc)
		return -ENOMEM;

	bsb_rc->gpio = gpio;
	bsb_rc->pdev = pdev;
	bsb_rc->reg_bar = bsb_reg_bar;
	bsb_rc->dev = dev;

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(dev, "Cannot enable PCI device");
		return err;
	}

	err = pci_request_regions(pdev, EPF_DRV_MODULE_NAME);
	if (err) {
		dev_err(dev, "Cannot obtain PCI resources");
		goto err_disable_pdev;
	}

	pci_set_master(pdev);

	irq_num = pci_alloc_irq_vectors(pdev, IRQ_NUM, IRQ_NUM, PCI_IRQ_LEGACY);
	if (irq_num < 0) {
		dev_err(dev, "Failed to get Legacy interrupt\n");
		err = irq_num;
		goto err_release_regions;
	}

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM) {
			base = pci_ioremap_bar(pdev, bar);
			if (!base) {
				dev_err(dev, "Failed to remap BAR%d", bar);
				WARN_ON(bar == bsb_reg_bar ||
					bar == bsb_msg_bar);
				for (i = 0; i < bar; i++)
					pci_iounmap(pdev, bsb_rc->bar[i]);
				err = -ENOMEM;
				goto err_pci_iounmap;
			}
			pr_info("%s %d, write to %p, pci_resource_flags(pdev, bar %d) is %lx\n",
				__func__, __LINE__, base, bar,
				pci_resource_flags(pdev, bar));
			bsb_rc->bar[bar] = base;
			/*
			 * XXX: RC needs know EP's cap from reg bar, so don't zero out the bar
			 * memset_io(base, 0, resource_size(&pdev->resource[bar]));
			 */
			if (bar == BSB_MSG_BAR)
				msg_bar_size =
					resource_size(&pdev->resource[bar]);
		}
	}

	if (!msg_bar_size) {
		dev_err(dev, "msg bar is missing!\n");
		err = -EINVAL;
		goto err_pci_iounmap;
	}
	dev_dbg(dev, "msg bar size is 0x%llx", msg_bar_size);

	bsb_rc->msg2ep_base = bsb_rc->bar[bsb_msg_bar];
	bsb_rc->msg2ep_total_size = msg_bar_size / 2;
	bsb_rc->msg2ep_total_size_per_subdevice =
		bsb_rc->msg2ep_total_size / nr_subdevices;

	bsb_rc->msg2rc_base =
		bsb_rc->bar[bsb_msg_bar] + bsb_rc->msg2ep_total_size;
	bsb_rc->msg2rc_total_size = msg_bar_size / 2;
	bsb_rc->msg2rc_total_size_per_subdevice =
		bsb_rc->msg2rc_total_size / nr_subdevices;

	bsb_rc->reg_base = bsb_rc->bar[bsb_reg_bar];
	bsb_reg = bsb_rc->reg_base;

	bsb_rc_configure_subdevice(bsb_rc);

	pci_set_drvdata(pdev, bsb_rc);
	bsb_test = pci_get_drvdata(pdev);
	pci_info(pdev, "%px binding bsb_rc to pdev successfully: %px\n", pdev,
		 bsb_test);

	err = devm_request_irq(&pdev->dev, pci_irq_vector(pdev, 0),
			       pci_endpoint_bsb_irqhandler, IRQF_SHARED,
			       EPF_DRV_MODULE_NAME, bsb_rc);
	if (err)
		goto err_pci_iounmap;

	err = bsb_init_subdevices(bsb_rc, &pdev->dev);
	if (err) {
		dev_err(dev, "failed to init misc devices\n");
		goto err_free_irq;
	}

	bsb_rc->cap = readl(&bsb_reg->flags);
	if (!(bsb_rc->cap & FLAG_SUPPORT_MSI_DOORBELL) &&
	    doorbell_method == DOORBELL_VIA_MSI) {
		err = -EINVAL;
		dev_err(dev, "invalid doorbell_method, cap is %d\n",
			bsb_rc->cap);
		goto err_free_misc;
	}

	pr_info("register bsb EPF driver successfully\n");

	return 0;

err_free_misc:
	for (i = 0; i < nr_subdevices; i++) {
		misc_deregister(&bsb_rc->subdevices[i].misc_device);
		ida_simple_remove(&ida, bsb_rc->subdevices[i].id);
	}
/* Free irq at early stage, otherwise the irq handler may get invoked */
err_free_irq:
	devm_free_irq(&pdev->dev, pci_irq_vector(pdev, 0), bsb_rc);
err_pci_iounmap:
	for (i = 0; i < bar && i < PCI_STD_NUM_BARS; i++)
		pci_iounmap(pdev, bsb_rc->bar[i]);
	pci_free_irq_vectors(pdev);
err_release_regions:
	pci_release_regions(pdev);
err_disable_pdev:
	pci_disable_device(pdev);

	return err;
}

static void bsb_rc_remove(struct pci_dev *pdev)
{
	enum pci_barno bar;
	struct bsb_rc *bsb_rc = pci_get_drvdata(pdev);
	int id, i;
	struct miscdevice *misc_device;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (bsb_rc->bar[bar])
			pci_iounmap(pdev, bsb_rc->bar[bar]);
	}

	devm_free_irq(&pdev->dev, pci_irq_vector(pdev, 0), bsb_rc);
	pci_free_irq_vectors(pdev);

	for (i = 0; i < nr_subdevices; i++) {
		misc_device = &bsb_rc->subdevices[i].misc_device;
		if (sscanf(misc_device->name, EPF_DRV_MODULE_NAME ".%d", &id) !=
		    1)
			return;
		if (id < 0) {
			pr_err("invalid id\n");
			continue;
		}
		misc_deregister(&bsb_rc->subdevices[i].misc_device);
		ida_simple_remove(&ida, id);
	}

	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id bsb_rc_dev_tbl[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_CDNS, BSB_PCIE_DEVICE_ID),
	},
	{}
};
MODULE_DEVICE_TABLE(pci, bsb_rc_dev_tbl);

static struct pci_driver bsb_rc_driver = {
	.name = EPF_DRV_MODULE_NAME,
	.id_table = bsb_rc_dev_tbl,
	.probe = bsb_rc_probe,
	.remove = bsb_rc_remove,
};
module_pci_driver(bsb_rc_driver);

MODULE_DESCRIPTION("BSB RC DRIVER for EPF");
MODULE_AUTHOR("lchen@ambarella.com");
MODULE_LICENSE("GPL v2");

static int bsb_rc_configure_gpio_irq(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	enum of_gpio_flags flags;
	int ret;

	gpio = of_get_gpio_flags(dev->of_node, 0, &flags);

	if (!gpio_is_valid(gpio))
		return gpio;

	ret = gpio_request(gpio, "bsb-rc");
	if (ret) {
		dev_err(dev, "failed to request gpio\n");
		return ret;
	}
	dev_info(dev, "requested GPIO %d\n", gpio);
	ret = gpio_direction_output(gpio,
				    //(flags & OF_GPIO_ACTIVE_LOW) ? (0) : (1));
				    (flags & OF_GPIO_ACTIVE_LOW) ? (1) : (0));
	if (ret) {
		dev_err(dev, "failed to set gpio dir\n");
		return ret;
	}

	return 0;
}

static int bsb_platform_probe(struct platform_device *pdev)
{
	int ret, i, nr_rmem;
	struct device_node *mem_node;
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem;

	if (doorbell_method == DOORBELL_VIA_GPIO) {
		ret = bsb_rc_configure_gpio_irq(pdev);
		if (ret)
			return ret;
	}

	resources = devm_kcalloc(dev, nr_subdevices, sizeof(*resources),
				 GFP_KERNEL);
	if (!resources)
		return -ENOMEM;

	nr_rmem =
		of_count_phandle_with_args(dev->of_node, "memory-region", NULL);
	if (nr_rmem != nr_subdevices) {
		dev_err(dev,
			"nr_rmem(0x%x) doesn't match with nr_subdevices(0x%lx)\n",
			nr_rmem, nr_subdevices);
		return -EINVAL;
	}

	for (i = 0; i < nr_subdevices; i++) {
		mem_node = of_parse_phandle(dev->of_node, "memory-region", i);
		if (!mem_node) {
			dev_err(dev, "no memory-region %d specified\n", i);
			return -EINVAL;
		}

		rmem = of_reserved_mem_lookup(mem_node);
		of_node_put(mem_node);

		if (!rmem) {
			dev_err(dev,
				"of_reserved_mem_lookup() returned NULL\n");
			return -ENODEV;
		}
		resources[i].start_addr = rmem->base;
		resources[i].size = rmem->size;
	}
	return 0;
}

static const struct of_device_id bsb_rc_match[] = {
	{
		.compatible = "ambarella,bsb_rc",
	},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, bsb_rc_match);

static struct platform_driver bsb_rc_platform_driver = {
	.driver = {
		.name = "bsb_rc",
		.of_match_table = bsb_rc_match,
	},
	.probe = bsb_platform_probe,
};
module_platform_driver(bsb_rc_platform_driver);
