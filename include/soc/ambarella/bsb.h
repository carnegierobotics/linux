/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Ambarella Bsb endpoint function RC|EP driver helpers.
 * Copyright (C) 2023 by Ambarella, Inc.
 */

#ifndef AMBARELLA_BSB
#define AMBARELLA_BSB
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/time64.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/pci.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/pci_ids.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>
#include <soc/ambarella/epf-core.h>
#include <linux/miscdevice.h>

enum {
	DOORBELL_VIA_MSI = 0,
	DOORBELL_VIA_GPIO,
};

#define bsb_module_parameters                                                  \
	static unsigned long doorbell_method = DOORBELL_VIA_MSI;               \
	module_param(doorbell_method, ulong, 0644);                            \
	MODULE_PARM_DESC(doorbell_method,                                      \
			 "specify how to send doorbell to EP from RC");

#define FLAG_SUPPORT_MSI_DOORBELL BIT(1)

#define BSB_REG_BAR 2
#define BSB_MSG_BAR 0
#define BSB_MSI_DOORBELL_BAR 4

#define BSB_PCIE_DEVICE_ID 0x0307
#define MAX_NR_SUBDEVICES 16

struct reg_subdevice_rmem {
	u32 lower_start_addr;
	u32 upper_start_addr;
	u32 size;
};

/**
 * Datas stored in EP's reg bar
 * Always use writel/readl because of cadence IP's
 * limitation.
 *
 * @wakeup_ep: tell EP to wakeup which APP/APPs
 *             don't use bitmep because there is no
 *             way to allow RC and EP access the bitmap
 *             variable automatically.
 *             Say that if there are multiple subdevice wake_up
 *             ioctl come from RC,
 * @wakeup_rc: tell RC to wakeup which APP/APPs
 */
struct bsb_reg {
	u32 nr_subdevices;
	u32 flags;
	u32 db_bar;
	u32 db_offset;
	u32 db_data;
	struct reg_subdevice_rmem subdevice_rmem[MAX_NR_SUBDEVICES];
	u32 wakeup_ep[MAX_NR_SUBDEVICES];
	u32 wakeup_rc[MAX_NR_SUBDEVICES];
	u32 waiting_ep[MAX_NR_SUBDEVICES];
	u32 ep_waiting_rc[MAX_NR_SUBDEVICES];
	u32 sz_msg2ep[MAX_NR_SUBDEVICES];
	u32 sz_msg2rc[MAX_NR_SUBDEVICES];
};

#endif
