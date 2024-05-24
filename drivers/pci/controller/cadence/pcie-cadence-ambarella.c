// SPDX-License-Identifier: GPL-2.0
/*
 * pci-ambarella - PCIe controller driver for Ambarella SoCs
 *
 * Copyright (C) 2022 by Ambarella, Inc.
 * Author: Li Chen <lchen@ambarella.com>
 */

#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/pm_runtime.h>
#include <linux/of_device.h>
#include <linux/sys_soc.h>
#include <linux/regmap.h>
#include <linux/of_reserved_mem.h>
#include <linux/irqdomain.h>
#include "pcie-cadence.h"

#define LINK_STATUS_V1 GENMASK(3, 2)
#define LINK_STATUS_V2 GENMASK(1, 0)
#define NIC_PERI_MASK GENMASK_ULL(31, 0)

enum {
	C_STATUS_REG,
	NUM_REG,
};

/**
 * struct ambarella_soc_data - private data for SoC
 * @cpu_to_bus_addr_mask: If CPU accesses PCIe controller through a NIC400-PERI
 *                        bus matrix controller which has 32 bits address, the
 *                        address [63:32] of PCIe address should be 0, and we
 *                        should set cpu_to_bus_addr_mask to do it.
 * @cpu_addr_fixup: Provide this function to set correct PCIe address.
 */
struct ambarella_soc_data {
	u64 cpu_to_bus_addr_mask;
	u64 (*cpu_addr_fixup)(struct cdns_pcie *pcie, u64 cpu_addr);
	u32 pciec_link_status_mask;
} static const *soc_data;

static u64 cdns_ambarella_generic_cpu_addr_fixup(struct cdns_pcie *pcie,
						 u64 cpu_addr)
{
	if (soc_data->cpu_to_bus_addr_mask)
		return cpu_addr & soc_data->cpu_to_bus_addr_mask;
	else
		return cpu_addr;
}

/*
 * PCIe0 connectes to NIC400-PERI(AXI Bus), whose address mapping is 32 bit. So [63:32] should be zero.
 *
 * There are 2 AXI slave ports of NIC400-PERI connecting to 2 PCIE NIC400 controllers, whose
 * address space are 40-bit:
 * PCIe1 connect to NIC400_PCIE_1 controller(AXI Bus).
 * PCIe2 connect to NIC400_PCIE_2 controller(AXI Bus).
 * So their [64:32] should be 40-bit address.
 *
 * For the address 0xFF_8000_0000 ~ 0xFF_BFFF_FFFF,
 * we will remap it to 0xFF_4000_0000 ~ 0xFF_7FFF_FFFF
 * so that we can use the same configuration for two NIC400_PCIE controllers.
 */
static u64 cdns_ambarella_cv3_cpu_addr_fixup(struct cdns_pcie *pcie,
					     u64 cpu_addr)
{
	/* PCIe0 */
	if (cpu_addr >= 0xFF10000000 && cpu_addr <= 0xFF1FFFFFFF)
		return cpu_addr & NIC_PERI_MASK;
	/* PCIe1 */
	else if (cpu_addr >= 0xFF40000000 && cpu_addr <= 0xFF7FFFFFFF)
		return cpu_addr;
	/* PCIe2, software needs to do the translation */
	else if (cpu_addr >= 0xFF80000000 && cpu_addr <= 0xFFBFFFFFFF)
		return cpu_addr - 0x40000000;
	dev_warn(pcie->dev, "bogous address %llx\n", cpu_addr);
	return cpu_addr & NIC_PERI_MASK;
}

static const struct ambarella_soc_data cv3_data = {
	.cpu_addr_fixup = cdns_ambarella_cv3_cpu_addr_fixup,
	.pciec_link_status_mask = LINK_STATUS_V1,
};

/*
 * In cv3ad685, SW team requested larger memory space for PCIe, so HW enhanced
 * the bus structure and the PCIe memory access no longer goes to NIC400-PERI.
 *
 * the transaction from CA78AE within FE.8000.0000~FE.BFFF.FFFF will be remapped to
 * FD.8000.0000~FD.BFFF.FFFF
 * so that we can use the same configuration for two NIC400_PCIE controllers.
 */
static u64 cdns_ambarella_cv3ad685_cpu_addr_fixup(struct cdns_pcie *pcie,
						  u64 cpu_addr)
{
	if (cpu_addr >= 0xFE00000000 && cpu_addr <= 0xFEFFFFFFFF)
		return cpu_addr - 0x100000000; /* PCIe2 */
	else
		return cpu_addr;
}

static const struct ambarella_soc_data cv3ad685_data = {
	.cpu_addr_fixup = cdns_ambarella_cv3ad685_cpu_addr_fixup,
	.pciec_link_status_mask = LINK_STATUS_V2,
};

/*
 * For CV5, AXI slave port go through NIC400-PERI,
 * so it need to set 32 bit mask
 */
static const struct ambarella_soc_data cv5_data = {
	.cpu_to_bus_addr_mask = NIC_PERI_MASK,
	.cpu_addr_fixup = cdns_ambarella_generic_cpu_addr_fixup,
	.pciec_link_status_mask = LINK_STATUS_V1,
};

/*
 * For CV7, AXI slave port doesn't go through NIC400-PERI,
 * so it doesn't need to set 32 bit mask
 */
static const struct ambarella_soc_data cv7_data = {
	.cpu_addr_fixup = cdns_ambarella_generic_cpu_addr_fixup,
	.pciec_link_status_mask = LINK_STATUS_V1,
};

static const struct soc_device_attribute ambarella_soc_info[] = {
	{ .soc_id = "cv3", .data = &cv3_data },
	{ .soc_id = "cv5", .data = &cv5_data },
	{ .soc_id = "cv72", .data = &cv7_data },
	{ .soc_id = "cv3ad685", .data = &cv3ad685_data },
	{ /* sentinel */ }
};

/**
 * struct ambarella_cdns_pcie - private data for this PCIe platform driver
 * @pcie: Cadence PCIe controller
 * @is_rc: Set to 1 indicates the PCIe controller mode is Root Complex,
 *         if 0 it is in Endpoint mode.
 */
struct ambarella_cdns_pcie {
	struct cdns_pcie *pcie;
	bool is_rc;
	struct regmap *regmap;
	u32 offset[NUM_REG];
};

struct ambarella_cdns_pcie_of_data {
	bool is_rc;
};

static const struct of_device_id ambarella_cdns_pcie_of_match[];

static u64 cdns_ambarella_cpu_addr_fixup(struct cdns_pcie *pcie, u64 cpu_addr)
{
	return soc_data->cpu_addr_fixup(pcie, cpu_addr);
}

static bool ambarella_pcie_link_up(struct cdns_pcie *cdns_pcie)
{
	struct ambarella_cdns_pcie *pcie = dev_get_drvdata(cdns_pcie->dev);
	u32 reg;

	regmap_read(pcie->regmap, pcie->offset[C_STATUS_REG], &reg);

	/*
	 * pciec_link_status, Status of the PCI Express link
	 * 0b00 = No receivers detected
	 * 0b01 = Link training in progress
	 * 0b10 = Link up, DL initialization in progress
	 * 0b11 = linkup, DL initialization completed
	 *
	 * Return true if both bit 2 and bit 3 are set.
	 */
	if (regmap_test_bits(pcie->regmap, pcie->offset[C_STATUS_REG],
			     soc_data->pciec_link_status_mask))
		return true;

	dev_dbg(cdns_pcie->dev, "%s not completed", __func__);
	return false;
}

static const struct cdns_pcie_ops ambarella_cdns_ops = {
	.cpu_addr_fixup = cdns_ambarella_cpu_addr_fixup,
	.link_up = ambarella_pcie_link_up,
};

static int ambarella_cdns_pcie_probe(struct platform_device *pdev)
{
	const struct ambarella_cdns_pcie_of_data *data;
	struct ambarella_cdns_pcie *ambarella_cdns_pcie;
	const struct of_device_id *match;
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct cdns_pcie_ep *ep;
	struct cdns_pcie_rc *rc;
	int phy_count;
	bool is_rc;
	int ret;
	const struct soc_device_attribute *soc;

	soc = soc_device_match(ambarella_soc_info);
	if (!soc || !soc->data) {
		dev_err(&pdev->dev, "Unknown SoC!\n");
		return -ENODEV;
	}
	soc_data = soc->data;

	match = of_match_device(ambarella_cdns_pcie_of_match, dev);
	if (!match)
		return -EINVAL;

	data = (struct ambarella_cdns_pcie_of_data *)match->data;
	is_rc = data->is_rc;

	dev_dbg(dev, " Started %s with is_rc: %d\n", __func__, is_rc);
	ambarella_cdns_pcie =
		devm_kzalloc(dev, sizeof(*ambarella_cdns_pcie), GFP_KERNEL);
	if (!ambarella_cdns_pcie)
		return -ENOMEM;

	ambarella_cdns_pcie->regmap = syscon_regmap_lookup_by_phandle_args(
		dev->of_node, "amb,scr-regmap", NUM_REG,
		ambarella_cdns_pcie->offset);
	if (IS_ERR(ambarella_cdns_pcie->regmap)) {
		dev_err(dev, "regmap lookup failed.\n");
		return PTR_ERR(ambarella_cdns_pcie->regmap);
	}

	platform_set_drvdata(pdev, ambarella_cdns_pcie);
	if (is_rc) {
		if (!IS_ENABLED(CONFIG_PCIE_CADENCE_HOST))
			return -ENODEV;

		bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
		if (!bridge)
			return -ENOMEM;

		rc = pci_host_bridge_priv(bridge);
		rc->pcie.dev = dev;
		rc->pcie.ops = &ambarella_cdns_ops;
		ambarella_cdns_pcie->pcie = &rc->pcie;
		ambarella_cdns_pcie->is_rc = is_rc;

		/* XXX: should we init puhy after setup host(cdns_pcie_host_setup)? */
		ret = cdns_pcie_init_phy(dev, ambarella_cdns_pcie->pcie);
		if (ret) {
			dev_err(dev, "failed to init phy, errno: %d\n", ret);
			return ret;
		}
		pm_runtime_enable(dev);
		ret = pm_runtime_get_sync(dev);
		if (ret < 0) {
			dev_err(dev, "pm_runtime_get_sync() failed\n");
			goto err_get_sync;
		}

		ret = cdns_pcie_host_setup(rc);
		if (ret)
			goto err_init;
	} else {
		dev->msi_domain = of_msi_get_domain(dev, dev->of_node,
						    DOMAIN_BUS_PLATFORM_MSI);
		if (!dev->msi_domain)
			dev_dbg(dev, "no MSI domain available\n");

		if (!IS_ENABLED(CONFIG_PCIE_CADENCE_EP))
			return -ENODEV;

		if (of_node_is_type(dev->of_node, "pci")) {
			dev_err(dev,
				"invalid device_type: endpoint controller should use \"pci\" as device_type");
			return -EINVAL;
		}

		ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
		if (!ep)
			return -ENOMEM;

		ep->pcie.dev = dev;
		ep->pcie.ops = &ambarella_cdns_ops;
		ambarella_cdns_pcie->pcie = &ep->pcie;
		ambarella_cdns_pcie->is_rc = is_rc;

		/* XXX: should we init puhy after setup ep(cdns_pcie_ep_setup)? */
		ret = cdns_pcie_init_phy(dev, ambarella_cdns_pcie->pcie);
		if (ret) {
			dev_err(dev, "failed to init phy\n");
			return ret;
		}

		pm_runtime_enable(dev);
		ret = pm_runtime_get_sync(dev);
		if (ret < 0) {
			dev_err(dev, "pm_runtime_get_sync() failed\n");
			goto err_get_sync;
		}

		/*
		 * If we want bar to bind large phy continuous
		 * memory(see pci_epf_alloc_space in pci-epf-core.c)
		 * like 64MB, we need reserve memory at early boot time.
		 * I use "memory-region = <&reservedBar>;" in PCIe
		 * controller's dts to do this, so of_reserved_mem_device_init is needed.
		 */
		if (of_reserved_mem_device_init(dev))
			dev_warn(
				dev,
				"device failed to get specific reserved mem pool, bar allocation may fail\n");

		ret = cdns_pcie_ep_setup(ep);

		if (ret)
			goto err_init;
	}

	dev_info(dev, " Started %s with %s\n", __func__, is_rc ? "RC" : "EP");
	return 0;

err_init:
err_get_sync:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	cdns_pcie_disable_phy(ambarella_cdns_pcie->pcie);
	phy_count = ambarella_cdns_pcie->pcie->phy_count;
	while (phy_count--)
		device_link_del(ambarella_cdns_pcie->pcie->link[phy_count]);

	return 0;
}

static void ambarella_cdns_pcie_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cdns_pcie *pcie = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_put_sync(dev);
	if (ret < 0)
		dev_dbg(dev, "pm_runtime_put_sync failed\n");

	pm_runtime_disable(dev);

	cdns_pcie_disable_phy(pcie);
}

static const struct ambarella_cdns_pcie_of_data
	ambarella_cdns_pcie_host_of_data = {
		.is_rc = true,
	};

static const struct ambarella_cdns_pcie_of_data ambarella_cdns_pcie_ep_of_data = {
	.is_rc = false,
};

static const struct of_device_id ambarella_cdns_pcie_of_match[] = {
	{
		.compatible = "ambarella,cdns-pcie-host",
		.data = &ambarella_cdns_pcie_host_of_data,
	},
	{
		.compatible = "ambarella,cdns-pcie-ep",
		.data = &ambarella_cdns_pcie_ep_of_data,
	},
	{},
};

static struct platform_driver ambarella_cdns_pcie_driver = {
	.driver = {
		.name = "ambarella-cdns-pcie",
		.of_match_table = ambarella_cdns_pcie_of_match,
		.pm	= &cdns_pcie_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ambarella_cdns_pcie_probe,
	.shutdown = ambarella_cdns_pcie_shutdown,
};
builtin_platform_driver(ambarella_cdns_pcie_driver);
