/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Ambarella Rainy endpoint function RC|EP driver helpers.
 * Copyright (C) 2023 by Ambarella, Inc.
 */

#ifndef AMBARELLA_RAINY
#define AMBARELLA_RAINY
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

#define RAINY_PCIE_DEVICE_ID 0x0306
#define MAX_PORTS 50

struct rainy_dma_info {
	u32 rc_dma_addr; /* used by EP OB and IB */
	u32 rc_dma_upper_addr;
	u32 rc_dma_region_size;
};

/*
 * Datas stored in EP's bar 0
 *
 * Always use writel/readl because of cadence IP's
 * limitation.
 */
struct rainy_msg {
	struct rainy_dma_info dma_info[MAX_PORTS];
};

#define rainy_readl_poll_timeout_atomic(msg, cond)                                             \
	({                                                                                     \
		u32 msg;                                                                       \
		if (debug_poll)                                                                \
			pr_info("before: %s %d\n", __func__, __LINE__);                        \
		ret = readl_poll_timeout_atomic(&msginfo->msg, msg, cond,                      \
						poll_delay_us, poll_timeout);                  \
		if (debug_poll)                                                                \
			pr_info("after: %s %d\n", __func__, __LINE__);                         \
		if (ret < 0) {                                                                 \
			pr_err("%s: timeout, msginfo->" #msg                                   \
			       " is %x, rc_buffer_checksum is %x, ep_buffer_checksum is %x\n", \
			       __func__, readl(&msginfo->msg),                                 \
			       msginfo->rc_buffer_checksum,                                    \
			       msginfo->ep_buffer_checksum);                                   \
			return ret;                                                            \
		}                                                                              \
	})

// TODO: use debugfs instead
#define rainy_module_parameters                                                \
	static bool enable_checksum = true;                                    \
	module_param(enable_checksum, bool, 0644);                             \
	MODULE_PARM_DESC(                                                      \
		checksum,                                                      \
		"Enable checksum, panic if mismatch, true by default");        \
	static bool debug_poll;                                                \
	module_param(debug_poll, bool, 0644);                                  \
	MODULE_PARM_DESC(debug_poll, "print out before/after poll");           \
	static unsigned long poll_timeout;                                     \
	module_param(poll_timeout, ulong, 0644);                               \
	MODULE_PARM_DESC(poll_timeout, "timeout when poll");                   \
	/* loop too tight(like 0) may cause Async SError */                    \
	static unsigned long poll_delay_us = 1000;                             \
	module_param(poll_delay_us, ulong, 0644);                              \
	MODULE_PARM_DESC(poll_delay_us, "delay_us when poll");                 \
	static bool dump_buffer;                                               \
	module_param(dump_buffer, bool, 0644);                                 \
	MODULE_PARM_DESC(dump_buffer,                                          \
			 "dump buffer when calc or compare checksum");         \
	static bool calc_rate = true;                                          \
	module_param(calc_rate, bool, 0644);                                   \
	MODULE_PARM_DESC(calc_rate, "calc rate for xfer, true by default");    \
	static bool silence_checksum;                                          \
	module_param(silence_checksum, bool, 0644);                            \
	MODULE_PARM_DESC(silence_checksum, "don't output unless mismatch");    \
	static bool panic_if_checksum_mismatch = true;                         \
	module_param(panic_if_checksum_mismatch, bool, 0644);                  \
	MODULE_PARM_DESC(panic_if_checksum_mismatch,                           \
			 "panic if checksum is mismatch");

#endif
