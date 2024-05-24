/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Core file header for all Ambarella's endpoint RC/EP-side driver.
 * Copyright (C) 2022 by Ambarella, Inc.
 */

#ifndef AMBARELLA_EPF_CORE_H
#define AMBARELLA_EPF_CORE_H
#include <linux/types.h>
#include <linux/pci-epf.h>
#include <linux/pci-epc.h>
#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include <linux/msi.h>
#include "pci-util.h"

enum operation { NOOP, EP_OB, EP_IB, RC_OB, RC_IB };

struct ambarella_msi_doorbell_property {
	enum pci_barno msi_doorbell_bar;
	size_t msi_doorbell_bar_size;
	int virq;
	struct msi_msg msg;
	enum irqreturn (*interrupt_handler)(int irq, void *data);
	void (*pci_epf_write_msi_msg)(struct msi_desc *desc,
				      struct msi_msg *msg);
};

extern void ambarella_epf_print_rate(const char *ops, u64 size,
				     struct timespec64 *start,
				     struct timespec64 *end);
extern void
ambarella_ep_configure_bar(struct pci_epf *epf,
			   const struct pci_epc_features *epc_features);

extern int
pci_epf_configure_msi_doorbell(struct ambarella_msi_doorbell_property *property,
			       struct pci_epf *epf,
			       const struct pci_epc_features *epc_features);
extern void pci_epf_free_msi_doorbell(struct pci_epf *epf, int virq);

#define MAX_EP_NUM 6
#define CDNS_VENDOR_ID 0x17cd

/* always use bar 2 as EP's mem buffer for RC inbound/outbound */
#define EP_MEM_BAR 0
/* always use bar 0 as EP's message(irq, ack, command, checksum and etc) buffer for RC inbound/outbound */
#define EP_MSG_BAR 2

#define for_each_ep(index)                                                     \
	for ((index) = 0; (index) < endpoints_info->ep_num; index++)

struct ambarella_endpoints_info {
	pci_bus_addr_t ep_mem_pci_addr[MAX_EP_NUM];
	size_t ep_mem_bar_size[MAX_EP_NUM];
	void __iomem *msginfo[MAX_EP_NUM];
	size_t ep_num;
	struct pci_dev *pdev[MAX_EP_NUM];
	// TODO: use mutex per ep
	struct mutex mutex;
};

extern struct ambarella_endpoints_info *endpoints_info;

extern int ambarella_rc_helper_init(int dev_id);
#endif
