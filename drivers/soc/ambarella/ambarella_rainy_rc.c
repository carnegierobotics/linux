// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ambarella Rainy endpoint function pci RC-side driver.
 *
 * History: 2023/6/28 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2023 by Ambarella, Inc.
 *
 * Abbrev:
 *
 * reo: ep ob codes run under RC-side kernel
 * rei: ep ib codes run under RC-side kernel
 *
 * reg bar: bar used to store epf's register, like size, addr and etc.
 */

#include <linux/completion.h>
#include <linux/kernel.h>
#include <soc/ambarella/rainy.h>
#include <linux/of_platform.h>
#include <soc/ambarella/epf-core.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/moduleparam.h>

#define STATUS_IRQ_RAISED BIT(6)

#define PCI_ENDPOINT_RAINY_STATUS 0x8

#define EPF_DRV_MODULE_NAME "rainy-rc"
#define IRQ_NUM 1

struct rainy_rc {
	struct pci_dev *pdev;
	struct device *dev;
	void __iomem *bar[PCI_STD_NUM_BARS];
	struct miscdevice misc_device;
	struct completion irq_raised;
	const char *name;
};

static inline u32 pci_endpoint_rainy_readl(struct rainy_rc *rainy_rc,
					   u32 offset)
{
	return readl(rainy_rc->bar[EP_MSG_BAR] + offset);
}

static inline void pci_endpoint_rainy_writel(struct rainy_rc *rainy_rc,
					     u32 offset, u32 value)
{
	writel(value, rainy_rc->bar[EP_MSG_BAR] + offset);
}

/* TODO: use INTx + message, see cdns_pcie_set_outbound_region_for_normal_msg and cdns_pcie_ep_assert_intx */
static irqreturn_t pci_endpoint_rainy_irqhandler(int irq, void *dev_id)
{
	struct rainy_rc *rainy_rc = dev_id;
	u32 reg;

	reg = pci_endpoint_rainy_readl(rainy_rc, PCI_ENDPOINT_RAINY_STATUS);
	if (reg & STATUS_IRQ_RAISED) {
		complete(&rainy_rc->irq_raised);
		reg &= ~STATUS_IRQ_RAISED;
	}
	pci_endpoint_rainy_writel(rainy_rc, PCI_ENDPOINT_RAINY_STATUS, reg);

	return IRQ_HANDLED;
}

static DEFINE_IDA(rainy_ida);

static long rainy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = -EINVAL;
	//void __user *argp = (void __user *)arg;
	//struct xfer_info *info;

	//info = memdup_user(argp, sizeof(*info));
	//if (IS_ERR(info))
	//	return -EFAULT;

	//kfree(info);
	return ret;
}

static const struct file_operations rainy_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = rainy_ioctl,
};

static int rainy_rc_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int err, i, id, irq_num;
	enum pci_barno bar;
	void __iomem *base;
	struct device *dev = &pdev->dev;
	enum pci_barno test_reg_bar = EP_MSG_BAR;
	struct rainy_rc *rainy_rc, *test_node;
	struct miscdevice *misc_device;

	ambarella_rc_helper_init(RAINY_PCIE_DEVICE_ID);

	dev->parent = ambarella_get_pcie_root_complex(dev);
	dev_info(dev, "parent is %s now\n", dev_name(dev->parent));
	if (pci_is_bridge(pdev))
		return -ENODEV;

	rainy_rc = devm_kzalloc(dev, sizeof(*rainy_rc), GFP_KERNEL);
	if (!rainy_rc)
		return -ENOMEM;

	rainy_rc->pdev = pdev;

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
				WARN_ON(bar == test_reg_bar);
				for (i = 0; i < bar; i++)
					pci_iounmap(pdev, rainy_rc->bar[i]);
				err = -ENOMEM;
				goto free_irq_vectors;
			}
			pr_info("%s %d, write to %p, pci_resource_flags(pdev, bar %d) is %lx\n",
				__func__, __LINE__, base, bar,
				pci_resource_flags(pdev, bar));
			rainy_rc->bar[bar] = base;
		}
	}

	pci_set_drvdata(pdev, rainy_rc);
	test_node = pci_get_drvdata(pdev);
	pci_info(pdev, "%px binding rainy_rc to pdev successfully: %px\n", pdev,
		 test_node);

	misc_device = &rainy_rc->misc_device;
	id = ida_simple_get(&rainy_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		err = id;
		pr_err("Unable to get id\n");
		goto err_pci_iounmap;
	}

	misc_device->name =
		kasprintf(GFP_KERNEL, EPF_DRV_MODULE_NAME ".%d", id);
	if (!misc_device->name) {
		err = -ENOMEM;
		goto err_ida_remove;
	}
	rainy_rc->name = kstrdup(misc_device->name, GFP_KERNEL);
	if (!rainy_rc->name) {
		err = -ENOMEM;
		goto free_miscdev_name;
	}

	misc_device = &rainy_rc->misc_device;
	misc_device->minor = MISC_DYNAMIC_MINOR;

	misc_device->fops = &rainy_fops;

	misc_device->parent = &pdev->dev;
	err = misc_register(misc_device);
	if (err) {
		pr_err("Failed to register device\n");
		err = -EINVAL;
		goto free_rainy_name;
	}
	dev = misc_device->this_device;

	rainy_rc->dev = misc_device->this_device;

	err = devm_request_irq(dev, pci_irq_vector(pdev, 0),
			       pci_endpoint_rainy_irqhandler, IRQF_SHARED,
			       rainy_rc->name, rainy_rc);
	if (err)
		goto deregister_misc;

	pr_info("register rainy EPF driver successfully\n");

	return 0;

deregister_misc:
	misc_deregister(&rainy_rc->misc_device);
free_rainy_name:
	kfree(rainy_rc->name);
free_miscdev_name:
	kfree(misc_device->name);
err_ida_remove:
	ida_simple_remove(&rainy_ida, id);
err_pci_iounmap:
	for (i = 0; i < bar; i++)
		pci_iounmap(pdev, rainy_rc->bar[i]);
free_irq_vectors:
	pci_free_irq_vectors(pdev);
err_release_regions:
	pci_release_regions(pdev);
err_disable_pdev:
	pci_disable_device(pdev);
	return err;
}

static void rainy_rc_remove(struct pci_dev *pdev)
{
	enum pci_barno bar;
	struct rainy_rc *rainy_rc = pci_get_drvdata(pdev);
	int id;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (rainy_rc->bar[bar])
			pci_iounmap(pdev, rainy_rc->bar[bar]);
	}

	if (sscanf(rainy_rc->misc_device.name, EPF_DRV_MODULE_NAME ".%d",
		   &id) != 1)
		return;
	if (id < 0) {
		pr_err("invalid id\n");
		return;
	}

	kfree(rainy_rc->name);

	devm_free_irq(&pdev->dev, pci_irq_vector(pdev, IRQ_NUM), rainy_rc);
	pci_free_irq_vectors(pdev);

	misc_deregister(&rainy_rc->misc_device);
	ida_simple_remove(&rainy_ida, id);

	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

static const struct pci_device_id rainy_rc_dev_tbl[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_CDNS, RAINY_PCIE_DEVICE_ID),
	},
	{}
};
MODULE_DEVICE_TABLE(pci, rainy_rc_dev_tbl);

static struct pci_driver rainy_rc_driver = {
	.name = EPF_DRV_MODULE_NAME,
	.id_table = rainy_rc_dev_tbl,
	.probe = rainy_rc_probe,
	.remove = rainy_rc_remove,
};
module_pci_driver(rainy_rc_driver);

MODULE_DESCRIPTION("Rainy RC DRIVER for EPF");
MODULE_AUTHOR("lchen@ambarella.com");
MODULE_LICENSE("GPL v2");

/*
 * Used by EP ob/ib
 */
static void rainy_rc_tell_ep_dma_info(int index, dma_addr_t rc_dma_addr,
				      u32 size, int port)
{
	struct rainy_msg __iomem *msginfo;
	struct rainy_dma_info __iomem *dma_info;

	pr_debug("%s %d rc_dma_addr is %llx", __func__, __LINE__, rc_dma_addr);
	msginfo = endpoints_info->msginfo[index];
	dma_info = &msginfo->dma_info[port];
	writel(lower_32_bits(rc_dma_addr), &dma_info->rc_dma_addr);
	writel(upper_32_bits(rc_dma_addr), &dma_info->rc_dma_upper_addr);
	writel(upper_32_bits(size), &dma_info->rc_dma_region_size);
}
