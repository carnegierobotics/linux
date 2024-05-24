// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ambarella CV72 MSI detection doorbell support
 *
 * An MSI detection logic detects the message signaled
 * interrupt from the external PCIe device.
 *
 * In CV72, MSI is a normal AXI write transaction from
 * the AXI master port of the PCIe controller.
 *
 * The MSI detection logic monitors AXI write transfers
 * from the PCIe AXI master port, and if the write
 * transfer is an MSI, the logic stores the MSI data and sends
 * an interrupt to the generic interrupt controller (GIC).
 *
 * This driver use MSI detection logic as doorbell to
 * allow RC interrupt EP.
 *
 * TODO:
 *  1) Convert platform to the new MSI parent model
 *  2) Utilize PCI/IMS which is giving you exactly what you need with
 *   proper PCI semantics
 *
 * Copyright (C) 2023 Ambarella.Inc.
 *
 * Author: Li Chen <lchen@ambarella.com>
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/spinlock.h>
#include <linux/dma-iommu.h>
#include <linux/sys_soc.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/dma-mapping.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define MSI_ADDR_LO(i) (0x0 + (0x8 * (i)))
#define MSI_ADDR_HI(i) (0x4 + (0x8 * (i)))
#define FIFO_CTRL(i) (0x20 + (0x4 * (i)))
#define MSI_DETECT_CTRL 0x30
#define FIFO_CNT GENMASK(9, 4)
#define MSI_ENABLE BIT(0x0)
#define FIFO_ACCESS_START(i) (0x100 + ((i) * (0x80)))
#define FIFO_ACCESS_END(i) (0x17c + ((i) * (0x80)))

static const struct regmap_config msi_detection_doorbell_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.use_raw_spinlock = true,
};

struct ambarella_msi_detection_doorbell {
	int spi_base;
	dma_addr_t *dma_addrs;
	unsigned int spi_cnt;
	unsigned long *spi_bitmap;
	spinlock_t spi_lock;
	struct device *dev;
	struct device_node *gic_node;
	struct regmap *regmap;
};

static void msi_detection_doorbell_compose_msi_msg(struct irq_data *data,
						   struct msi_msg *msg)
{
	struct ambarella_msi_detection_doorbell *msi_detection_doorbell =
		data->chip_data;
	int virq = data->irq;
	struct msi_desc *msi_desc;
	struct device *dev;

	msi_desc = irq_data_get_msi_desc(data);
	dev = msi_desc_to_dev(msi_desc);

	regmap_write(
		msi_detection_doorbell->regmap, MSI_ADDR_LO(data->hwirq),
		lower_32_bits(msi_detection_doorbell->dma_addrs[data->hwirq]));
	regmap_write(
		msi_detection_doorbell->regmap, MSI_ADDR_HI(data->hwirq),
		upper_32_bits(msi_detection_doorbell->dma_addrs[data->hwirq]));

	if (!msi_detection_doorbell->dma_addrs[data->hwirq])
		dev_err(dev, "invalid msi message addr, hwirq is %ld\n",
			data->hwirq);
	msg->address_lo =
		lower_32_bits(msi_detection_doorbell->dma_addrs[data->hwirq]);
	msg->address_hi =
		upper_32_bits(msi_detection_doorbell->dma_addrs[data->hwirq]);

	/*
	 * FIXME: Although most MSI controller drivers also write the hardware IRQ to message data,
	 * there may be some exceptions, e.g. when deisgn a epf driver which wants to raise
	 * multiple different irq to RC and trigger RC-side handlers, but there is only single spi
	 * underlying. See cdns_pcie_ep_send_msi_irq for example.
	 *
	 * This design cannot be supported via shared IRQ + legacy IRQ because the multiple irq should
	 * be triggered by single device. But MSI can encode its own IRQ number
	 * (not SPI nor any other IRQ domain hardware IRQ) in its message data, so RC-side driver can
	 * use it to invoke correct handle.
	 *
	 * In such cases, the message data should be not overridden by the MSI controller driver.
	 */
	msg->data = data->hwirq;

	dev_dbg(
		dev,
		"address low is %x, address high is %x, hwirq is %ld, virq is %d, msi_detection_doorbell->dma_addrs[%ld] is %llx), msg is 0x%px\n",
		msg->address_lo, msg->address_hi, data->hwirq, virq,
		data->hwirq, msi_detection_doorbell->dma_addrs[data->hwirq],
		msg);
}

static int msi_detection_doorbell_domain_ops_init(struct irq_domain *domain,
						  struct msi_domain_info *info,
						  unsigned int virq,
						  irq_hw_number_t hwirq,
						  msi_alloc_info_t *arg)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	int ret;
	struct ambarella_msi_detection_doorbell *msi_detection_doorbell =
		info->chip_data;

	irq_domain_set_hwirq_and_chip(domain, virq, hwirq, info->chip,
				      info->chip_data);
	if (info->handler && info->handler_name) {
		__irq_set_handler(virq, info->handler, 0, info->handler_name);
		if (info->handler_data)
			irq_set_handler_data(virq, info->handler_data);
	}

	ret = d->chip->irq_set_type(d, IRQ_TYPE_LEVEL_HIGH);
	if (ret) {
		dev_err(msi_detection_doorbell->dev,
			"failed to set irq type to IRQ_TYPE_LEVEL_HIGH, errno %d\n",
			ret);
		return ret;
	}
	dev_dbg(msi_detection_doorbell->dev,
		"%s %d, irq set chip %s, type done, virq is %d, hwirq is %ld, d->hwirq is %ld, d->irq is %d\n",
		__func__, __LINE__, d->chip->name, virq, hwirq, d->hwirq,
		d->irq);
	return 0;
}

static irq_hw_number_t
msi_detection_doorbell_get_hwirq(struct msi_domain_info *info,
				 msi_alloc_info_t *arg,
				 struct irq_fwspec *fwspec)
{
	struct ambarella_msi_detection_doorbell *msi_detection_doorbell =
		info->chip_data;
	irq_hw_number_t hwirq;

	fwspec->fwnode = of_node_to_fwnode(msi_detection_doorbell->gic_node);
	// TODO: use of function to get param
	fwspec->param_count = 3;
	fwspec->param[0] = GIC_SPI;
	fwspec->param[2] = IRQ_TYPE_LEVEL_HIGH;

	spin_lock(&msi_detection_doorbell->spi_lock);
	hwirq = find_first_zero_bit(msi_detection_doorbell->spi_bitmap,
				    msi_detection_doorbell->spi_cnt);
	dev_dbg(msi_detection_doorbell->dev, "%s %d, get hwirq %ld\n", __func__,
		__LINE__, hwirq);
	if (hwirq >= msi_detection_doorbell->spi_cnt) {
		spin_unlock(&msi_detection_doorbell->spi_lock);
		return -ENOSPC;
	}
	__set_bit(hwirq, msi_detection_doorbell->spi_bitmap);
	spin_unlock(&msi_detection_doorbell->spi_lock);

	fwspec->param[1] = msi_detection_doorbell->spi_base + hwirq;
	return hwirq;
}

static void msi_detection_doorbell_free(struct irq_domain *domain,
					struct msi_domain_info *info,
					unsigned int virq)
{
	struct ambarella_msi_detection_doorbell *msi_detection_doorbell =
		info->chip_data;
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);

	if (d->hwirq >= msi_detection_doorbell->spi_cnt) {
		dev_err(msi_detection_doorbell->dev, "Invalid hwirq %lu\n",
			d->hwirq);
		return;
	}

	spin_lock(&msi_detection_doorbell->spi_lock);
	__clear_bit(d->hwirq, msi_detection_doorbell->spi_bitmap);
	spin_unlock(&msi_detection_doorbell->spi_lock);
}

static struct msi_domain_ops msi_detection_doorbell_msi_ops = {
	.get_hwirq = msi_detection_doorbell_get_hwirq,
	.msi_free = msi_detection_doorbell_free,
	.msi_init = msi_detection_doorbell_domain_ops_init,
};

static void msi_detection_doorbell_mask_msi_irq(struct irq_data *d)
{
	irq_chip_mask_parent(d);
}

static void msi_detection_doorbell_unmask_msi_irq(struct irq_data *d)
{
	irq_chip_unmask_parent(d);
}

void msi_detection_doorbell_eoi(struct irq_data *data)
{
	struct ambarella_msi_detection_doorbell *msi_detection_doorbell =
		data->chip_data;
	int cnt, msi_data, ctrl;

	/* Read from FIFO */
	regmap_read(msi_detection_doorbell->regmap, FIFO_CTRL(data->hwirq),
		    &ctrl);
	cnt = FIELD_GET(FIFO_CNT, ctrl);
	dev_dbg(msi_detection_doorbell->dev,
		"%s %d, hwirq is %ld, cnt is 0x%x\n", __func__, __LINE__,
		data->hwirq, cnt);
	regmap_read(msi_detection_doorbell->regmap,
		    FIFO_ACCESS_START(data->hwirq), &msi_data);
	data = data->parent_data;
	data->chip->irq_eoi(data);
}

static int msi_detection_doorbell_irq_set_type(struct irq_data *d,
					       unsigned int type)
{
	irqd_set_trigger_type(d, IRQ_TYPE_LEVEL_HIGH);
	return irq_chip_set_type_parent(d, type);
}

static struct irq_chip msi_detection_doorbell_msi_irq_chip = {
	.name = "MSI_DETECTION_DOORBELL",
	.irq_set_type = msi_detection_doorbell_irq_set_type,
	.irq_compose_msi_msg = msi_detection_doorbell_compose_msi_msg,
	.irq_mask = msi_detection_doorbell_mask_msi_irq,
	.irq_unmask = msi_detection_doorbell_unmask_msi_irq,
	.irq_set_affinity = irq_chip_set_affinity_parent,
	.irq_eoi = msi_detection_doorbell_eoi,
};

static struct msi_domain_info msi_detection_doorbell_msi_domain_info = {
	.flags = (MSI_FLAG_USE_DEF_DOM_OPS | /* For set_desc and msi_check */
		  MSI_FLAG_USE_DEF_CHIP_OPS /* For  platform_msi_write_msg*/
		  ),
	.ops = &msi_detection_doorbell_msi_ops,
	.chip = &msi_detection_doorbell_msi_irq_chip,
};

static int ambarella_msi_detection_doorbell_probe(struct platform_device *pdev)
{
	struct ambarella_msi_detection_doorbell *msi_detection_doorbell;
	struct irq_domain *msi_domain, *parent_domain;
	struct device_node *node = pdev->dev.of_node;
	struct device_node *irq_parent_dn;
	int nr_irqs, i, ret;
	void *base;
	struct device *dev = &pdev->dev;
	void *msg_vaddr, *dummy_vaddr;
	dma_addr_t dummy_dma_addr;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	msi_detection_doorbell =
		devm_kzalloc(dev, sizeof(*msi_detection_doorbell), GFP_KERNEL);
	if (!msi_detection_doorbell)
		return -ENOMEM;

	msi_detection_doorbell->dev = dev;
	spin_lock_init(&msi_detection_doorbell->spi_lock);

	msi_detection_doorbell->regmap = devm_regmap_init_mmio(
		dev, base, &msi_detection_doorbell_regmap_config);
	if (IS_ERR(msi_detection_doorbell->regmap)) {
		dev_err(dev, "Failed to create regmap\n");
		return PTR_ERR(msi_detection_doorbell->regmap);
	}

	/* use "msi-detection,spi-ranges instead of "interrupts" because the latter will map hwirq to virq here, but we want the mapping to be done when get_hwirq/irq_alloc */
	ret = of_property_read_u32_index(node, "msi-detection,spi-range", 1,
					 &msi_detection_doorbell->spi_cnt);
	if (ret)
		return ret;

	ret = of_property_read_u32_index(node, "msi-detection,spi-range", 0,
					 &msi_detection_doorbell->spi_base);
	if (ret)
		return ret;

	dev_info(dev, "spi_cnt is %d, spi_base is %d\n",
		 msi_detection_doorbell->spi_cnt,
		 msi_detection_doorbell->spi_base);

	nr_irqs = msi_detection_doorbell->spi_cnt;

	msi_detection_doorbell->spi_bitmap = devm_bitmap_zalloc(
		dev, msi_detection_doorbell->spi_cnt, GFP_KERNEL);
	if (!msi_detection_doorbell->spi_bitmap)
		return -ENOMEM;

	msi_detection_doorbell->dma_addrs =
		devm_kcalloc(dev, nr_irqs,
			     sizeof(msi_detection_doorbell->dma_addrs),
			     GFP_KERNEL);
	if (!msi_detection_doorbell->dma_addrs)
		return -ENOMEM;

	for (i = 0; i < nr_irqs; i++) {
		regmap_set_bits(msi_detection_doorbell->regmap, FIFO_CTRL(i),
				BIT(2));
		regmap_set_bits(msi_detection_doorbell->regmap, FIFO_CTRL(i),
				BIT(3));

		msg_vaddr = dmam_alloc_coherent(
			msi_detection_doorbell->dev, PAGE_SIZE,
			&msi_detection_doorbell->dma_addrs[i], GFP_KERNEL);
		if (!msg_vaddr) {
			dev_err(msi_detection_doorbell->dev,
				"%s: alloc memory for msg failed\n", __func__);
			return -ENOMEM;
		}
		memset(msg_vaddr, -1, PAGE_SIZE);

		dummy_vaddr = dmam_alloc_coherent(msi_detection_doorbell->dev,
						  PAGE_SIZE, &dummy_dma_addr,
						  GFP_KERNEL);
		if (!dummy_vaddr) {
			dev_err(msi_detection_doorbell->dev,
				"%s: alloc dummy memory failed\n", __func__);
			return -ENOMEM;
		}

		dev_info(
			dev,
			"%s %d, dma_addr is %llx, i is %d, MSI_ADDR_LO is %x, MSI_ADDR_HI is %x\n",
			__func__, __LINE__,
			msi_detection_doorbell->dma_addrs[i], i, MSI_ADDR_LO(i),
			MSI_ADDR_HI(i));
	}

	irq_parent_dn = of_irq_find_parent(node);
	if (!irq_parent_dn) {
		dev_err(dev, "failed to find GIC node\n");
		return -ENODEV;
	}

	parent_domain = irq_find_host(irq_parent_dn);
	msi_detection_doorbell->gic_node = irq_parent_dn;
	if (!parent_domain) {
		dev_err(dev, "failed to find GIC domain\n");
		return -ENODEV;
	}

	msi_domain = platform_msi_create_irq_domain(
		of_node_to_fwnode(node),
		&msi_detection_doorbell_msi_domain_info, parent_domain);
	if (!msi_domain) {
		dev_err(dev, "Failed to create MSI domain\n");
		return -ENOMEM;
	}

	msi_detection_doorbell_msi_domain_info.chip_data =
		msi_detection_doorbell;
	regmap_write(msi_detection_doorbell->regmap, MSI_DETECT_CTRL,
		     MSI_ENABLE);

	return 0;
}

static const struct of_device_id ambarella_msi_detection_doorbell_of_match[] = {
	{
		.compatible = "ambarella,msi-doorbell",
	},
	{},
};

static struct platform_driver ambarella_msi_detection_doorbell_driver = {
	.probe  = ambarella_msi_detection_doorbell_probe,
	.driver = {
		.name = "ambarella-msi_detection_doorbell",
		.of_match_table = ambarella_msi_detection_doorbell_of_match,
	},
};
builtin_platform_driver(ambarella_msi_detection_doorbell_driver);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Ambarella MSI Detection Doorbell driver");
