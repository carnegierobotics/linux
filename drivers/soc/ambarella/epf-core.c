// SPDX-License-Identifier: GPL-2.0-only
/*
 * Core file for all Ambarella's endpoint RC/EP-side driver.
 *
 * History: 2022/11/22 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by Ambarella, Inc.
 */
#include <soc/ambarella/epf-core.h>
#include <uapi/linux/pci_regs.h>
#include <linux/pci-epf.h>
#include <linux/pci-epc.h>
#include <linux/platform_device.h>

void ambarella_epf_print_rate(const char *ops, u64 size,
			      struct timespec64 *start, struct timespec64 *end)
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

	pr_info("\n%s => Size: %llu bytes\t DMA: %s\t Time: %llu.%09u seconds\t"
		"Rate: %llu KB/s\n",
		ops, size, "YES", (u64)ts.tv_sec, (u32)ts.tv_nsec, rate / 1024);
}

void ambarella_ep_configure_bar(struct pci_epf *epf,
				const struct pci_epc_features *epc_features)
{
	struct pci_epf_bar *epf_bar;
	bool bar_fixed_64bit;
	bool bar_prefetch;
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		epf_bar = &epf->bar[i];
		bar_fixed_64bit = !!(epc_features->bar_fixed_64bit & (1 << i));
		bar_prefetch = !!(epc_features->bar_prefetch & (1 << i));
		epf_bar->barno = i;
		if (bar_fixed_64bit)
			epf_bar->flags |= PCI_BASE_ADDRESS_MEM_TYPE_64;
		if (bar_prefetch)
			epf_bar->flags |= PCI_BASE_ADDRESS_MEM_PREFETCH;
		if (epc_features->bar_fixed_size[i])
			pr_debug("%s fixed bar %x size is %llx\n", __func__, i,
				 epc_features->bar_fixed_size[i]);
	}
}
EXPORT_SYMBOL(ambarella_ep_configure_bar);

/* Ugly global variables */
struct ambarella_endpoints_info *endpoints_info;
EXPORT_SYMBOL(endpoints_info);

#define PCI_BASE_ADDRESS_OFFSET_INDEX 0x4

static bool is_cadence_ep(struct pci_dev *pdev, int dev_id)
{
	return pdev->vendor == CDNS_VENDOR_ID && pdev->device == dev_id;
}

// TODO: handle 64-bit mem bar.
static int get_endpoints_info(struct pci_dev *pdev, int ep_id)
{
	int i;
	u32 bar_addr;

	endpoints_info->pdev[ep_id] = pdev;
	pci_dbg(pdev,
		"%s %d, pdev is %px, ep_id is %d, endpoints_info is %px, endpoints_info->pdev[%d] is %px, endpoints_info->pdev itself is %px\n",
		__func__, __LINE__, pdev, ep_id, endpoints_info, ep_id,
		endpoints_info->pdev[ep_id], endpoints_info->pdev);
	for (i = 0; i < DEVICE_COUNT_RESOURCE; i++)
		if (resource_size(&pdev->resource[i]))
			pci_dbg(pdev, "%s: bar %x is %llx", __func__, i,
				resource_size(&pdev->resource[i]));
	for (i = 0; i < DEVICE_COUNT_RESOURCE; i++) {
		if (i == EP_MEM_BAR) {
			pci_read_config_dword(
				pdev,
				PCI_BASE_ADDRESS_0 +
					PCI_BASE_ADDRESS_OFFSET_INDEX * i,
				&bar_addr);
			/* TODO: record mem bar size */
			endpoints_info->ep_mem_pci_addr[ep_id] = bar_addr;
			endpoints_info->ep_mem_bar_size[ep_id] =
				resource_size(&pdev->resource[i]);
			pci_info(pdev,
				 "EP SoC(%d) found, ep_mem_pci_addr is %llx,\n",
				 ep_id, endpoints_info->ep_mem_pci_addr[ep_id]);
		}
		if (i == EP_MSG_BAR) {
			endpoints_info->msginfo[ep_id] =
				pci_ioremap_bar(pdev, i);

			if (!endpoints_info->msginfo[ep_id]) {
				pci_err(pdev, "pci_ioremap_bar failed\n");
				return -1;
			}
		}
	}
	return 0;
}

static bool find_all_ep(int dev_id)
{
	size_t ep_id = 0;
	struct pci_dev *pdev = NULL;

	for_each_pci_dev (pdev) {
		pci_dbg(pdev,
			"pdev->vendor is %x, pdev->device %x, dev_driver_string(&pdev->dev) is %s",
			pdev->vendor, pdev->device,
			dev_driver_string(&pdev->dev));
		if (is_cadence_ep(pdev, dev_id)) {
			if (ep_id >= MAX_EP_NUM) {
				pci_err(pdev,
					"Currently, only %d EP are supported\n",
					MAX_EP_NUM);
				break;
			}
			if (get_endpoints_info(pdev, ep_id) < 0)
				continue;

			ep_id++;
		}
	}
	endpoints_info->ep_num = ep_id;

	return ep_id;
}

int ambarella_rc_helper_init(int dev_id)
{
	int ret = 0;

	endpoints_info =
		kzalloc(sizeof(struct ambarella_endpoints_info), GFP_KERNEL);
	if (!endpoints_info) {
		ret = -ENOMEM;
		goto kmalloc_fail;
	}
	if (!find_all_ep(dev_id)) {
		pr_err("no endpoints SoC found\n");
		ret = -ENODEV;
		goto no_ep;
	}
	mutex_init(&endpoints_info->mutex);
	return 0;

no_ep:
	kfree(endpoints_info);

kmalloc_fail:
	return ret;
}
EXPORT_SYMBOL(ambarella_rc_helper_init);

static int pci_epf_assign_msi_space(struct pci_epf *epf, size_t size,
			     enum pci_barno bar, size_t align,
			     enum pci_epc_interface_type type,
			     struct msi_msg *msg)
{
	struct pci_epf_bar *epf_bar;

	if (!msg) {
		pr_err("%s: invalid MSI msg\n", __func__);
		return -EINVAL;
	}

	if (size < 128)
		size = 128;

	if (align)
		size = ALIGN(size, align);
	else
		size = roundup_pow_of_two(size);

	if (type == PRIMARY_INTERFACE)
		epf_bar = epf->bar;
	else
		epf_bar = epf->sec_epc_bar;

	epf_bar[bar].phys_addr =
		msg->address_lo | ((dma_addr_t)(msg->address_hi) << 32);
	pr_debug(
		"bar %d, phys_addr is 0x%llx, msg is %px, msg->address_lo is 0x%x\n",
		bar, epf_bar[bar].phys_addr, msg, msg->address_lo);

	/*
	 * XXX: there is no way to get msi bar vaddr, because it's allocated
	 * on platform MSI driver side.
	 */
	epf_bar[bar].addr = NULL;
	epf_bar[bar].size = size;
	epf_bar[bar].barno = bar;
	epf_bar[bar].flags |= upper_32_bits(size) ?
				      PCI_BASE_ADDRESS_MEM_TYPE_64 :
				      PCI_BASE_ADDRESS_MEM_TYPE_32;
	return 0;
}

int pci_epf_configure_msi_doorbell(
	struct ambarella_msi_doorbell_property *property, struct pci_epf *epf,
	const struct pci_epc_features *epc_features)
{
	struct irq_domain *domain;
	struct pci_epc *epc;
	struct device *dev;
	int ret = 0;
	struct msi_desc *desc;
	int *virq = &property->virq;
	struct msi_msg *msg = &property->msg;
	size_t msi_doorbell_bar_size = property->msi_doorbell_bar_size;
	enum pci_barno msi_doorbell_bar = property->msi_doorbell_bar;

	epc = epf->epc;
	dev = &epc->dev;

	/*
	 * Current only support 1 function.
	 * PCI IMS(interrupt message store) ARM support have not been
	 * ready yet.
	 */
	if (epc->function_num_map != 1)
		return -EOPNOTSUPP;

	domain = dev_get_msi_domain(dev->parent);
	if (!domain) {
		dev_err(dev,
			"Failed to get msi domain from parent, please check AMBARELLA_MSI_DETECTION_DOORBELL and dts\n");
		return -EOPNOTSUPP;
	}
	dev_set_msi_domain(dev, domain);

	dev_info(dev, "dev->of_node was %pOF\n", dev->of_node);
	/* use parent of_node to get device id information */
	dev->of_node = dev->parent->of_node;
	dev_info(dev, "dev->of_node is %pOF now\n", dev->of_node);

	ret = platform_msi_domain_alloc_irqs(dev, 1,
					     property->pci_epf_write_msi_msg);
	if (ret) {
		dev_err(dev, "Can't allocate MSI from system MSI controller\n");
		return -EOPNOTSUPP;
	}

	desc = first_msi_entry(dev);
	if (desc)
		*virq = desc->irq;
	else
		goto err_irq;

	ret = request_threaded_irq(*virq, NULL, property->interrupt_handler,
				   IRQF_ONESHOT, "pci-epf-msi-doorbell", epf);

	if (ret) {
		dev_err(dev, "failed to request msi_doorbell IRQ\n");
		goto err_irq;
	}

	ret = pci_epf_assign_msi_space(epf, msi_doorbell_bar_size,
				       msi_doorbell_bar, epc_features->align,
				       PRIMARY_INTERFACE, msg);
	if (ret)
		goto err_irq;

	return ret;

err_irq:
	platform_msi_domain_free_irqs(dev);

	return ret;
}
EXPORT_SYMBOL(pci_epf_configure_msi_doorbell);

void pci_epf_free_msi_doorbell(struct pci_epf *epf, int virq)
{
	struct pci_epc *epc;

	epc = epf->epc;

	free_irq(virq, epf);

	platform_msi_domain_free_irqs(&epc->dev);
}
EXPORT_SYMBOL(pci_epf_free_msi_doorbell);
