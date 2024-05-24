/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Ambarella Excalibur endpoint function RC|EP driver helpers.
 * Copyright (C) 2022 by Ambarella, Inc.
 */

#ifndef AMBARELLA_EXCALIBUR
#define AMBARELLA_EXCALIBUR
#include <linux/types.h>
#include <linux/types.h>
#include <linux/genalloc.h>
#include <linux/pci.h>
#include <linux/time64.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/pci.h>
#include <linux/dmaengine.h>
#include <linux/miscdevice.h>
#include <linux/io.h>
#include <linux/pci_ids.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci_regs.h>

#define EXCALIBUR_PCIE_DEVICE_ID 0x0300

enum rc_ob_query_ep_size_enough {
	RC_OB_NOT_PREPARE_YET,
	RC_OB_QUERYING_SIZE,
	RC_OB_EP_SIZE_BIG_ENOUGH,
	RC_OB_EP_SIZE_NOT_BIG_ENOUGH,
};

enum rro_done {
	RRO_NOT_DONE,
	RRO_DONE,
};

enum rc_ob_xfer_in_progress {
	RC_OB_XFER_NOT_IN_PROGRESS,
	RC_OB_XFER_IN_PROGRESS,
};

enum rc_ob_ep_buffer_ready {
	RC_OB_EP_BUFFER_NOT_READY,
	RC_OB_EP_BUFFER_IS_READY,
};

enum rc_ib_query_ep_size_and_src {
	RC_IB_QUERY_EP_SIZE_AND_SRC_NOT_PREP_YET,
	RC_IB_QUERY_EP_SIZE_AND_SRC_DONE,
};

enum rc_ib_xfer_in_progress {
	RC_IB_XFER_NOT_IN_PROGRESS,
	RC_IB_XFER_IN_PROGRESS,
};

enum rri_done {
	RRI_NOT_DONE,
	RRI_DONE,
};

enum ep_ob_query_rc_size_enough {
	EP_OB_NOT_PREPARE_YET,
	EP_OB_QUERYING_SIZE,
	EP_OB_RC_SIZE_BIG_ENOUGH,
	EP_OB_RC_SIZE_NOT_BIG_ENOUGH,
};

enum ep_ob_xfer_in_progress {
	EP_OB_XFER_NOT_IN_PROGRESS,
	EP_OB_XFER_IN_PROGRESS,
};

enum ep_ob_rc_buffer_ready {
	EP_OB_RC_BUFFER_NOT_READY,
	EP_OB_RC_BUFFER_IS_READY,
};

enum ep_ib_query_rc_size_and_src {
	EP_IB_QUERY_RC_SIZE_AND_SRC_NOT_PREP_YET,
	EP_IB_QUERY_RC_SIZE_AND_SRC_DONE,
};

enum ep_ib_xfer_in_progress {
	EP_IB_XFER_NOT_IN_PROGRESS,
	EP_IB_XFER_IN_PROGRESS,
};

enum eei_done {
	EEI_NOT_DONE,
	EEI_DONE,
};
/*
 * Datas stored in EP's msg bar
 *
 * Always use datas whose size <= SZ_4 data,
 * because cadence doesn't allow
 * data size(AXI_AR/WLEN) > 4 to be
 * transfered.
 *
 * TODO:
 * use bit field for size < 4 variables.
 */
struct excalibur_msg {
	u32 rc_ob_xfer_in_progress;
	u32 rc_ob_query_ep_size_enough;
	u32 rc_ob_ep_buffer_ready;
	u32 rro_done;

	u32 rc_ib_query_ep_size_and_src;
	u32 rc_ib_xfer_in_progress;
	u32 rri_done;

	u32 ep_ob_rc_buffer_ready;
	u32 ep_ob_query_rc_size_enough;
	u32 ep_ob_xfer_in_progress;

	u32 ep_ib_xfer_in_progress;
	u32 ep_ib_query_rc_size_and_src;
	u32 eei_done;

	/* FIXME: use individual variable for different xfer types */
	u32 trans_size; /* total transfer size must be multi of 4. Pad if need */

	u32 rc_dma_addr; /* used by EP OB and IB */
	u32 ep_buffer_checksum;
	u32 rc_buffer_checksum;
	u32 rc_dma_upper_addr;

	u32 rc_ob_offset_pci_upper_addr;
	u32 rc_ob_offset_pci_addr;

	u32 rc_ib_offset_pci_upper_addr;
	u32 rc_ib_offset_pci_addr;
};

#ifdef CONFIG_AMBARELLA_EXCALIBUR_RC

struct excalibur_rc {
	struct pci_dev *pdev;
	void __iomem *bar[PCI_STD_NUM_BARS];

	void *rc_buffer; /* will be added to gen_pool with gen_pool_add_virt */
	dma_addr_t rc_dma_addr;
	struct dma_chan *dma_chan_tx;
	struct dma_chan *dma_chan_rx;
	struct mutex rei_mutex;
	struct mutex reo_mutex;
};

extern struct gen_pool *excalibur_rc_get_pool(int index);
extern int excalibur_rro_wait_for_ep_buffer_ready(int index, void *rc_buffer,
						  u32 size);
extern void excalibur_reo_prepare(int index, void *rc_buffer);
extern int excalibur_reo_wait_dma_complete(int index, void *rc_buffer);
extern int excalibur_rei_wait_dma_complete(int index);
extern int excalibur_rei_prepare(int index, void *rc_buffer, u32 size);
extern u32 excalibur_rc_get_xfer_size(int index);
extern int excalibur_reo_wait_ep_query_size(int index);
extern void excalibur_rc_set_xfer_size(int index, u32 size);
extern int excalibur_rc_check_ep(int index);
extern int excalibur_rc_ob(int index, dma_addr_t src_addr, u32 size);
extern int excalibur_rri_wait_for_ep_size_and_buffer_ready(int index,
							   u32 *size);
extern int excalibur_rc_ib(int index, void *rc_buffer, u32 size);
#endif

#ifdef CONFIG_AMBARELLA_EXCALIBUR_EP
struct excalibur_ep {
	struct dma_chan *dma_chan_tx;
	struct dma_chan *dma_chan_rx;
	struct mutex mutex;
	struct mutex eri_mutex;
	struct mutex ero_mutex;
	void __iomem *bar[PCI_STD_NUM_BARS];
	enum pci_barno reg_bar;

	struct pci_epf *epf;
	const struct pci_epc_features *epc_features;

	/* data transfer window: inbound transfer */
	u32 mem_bar;
	void *mem_bar_base;
	size_t mem_bar_size;
	dma_addr_t
		mem_bar_dma_addr; /* mem bar's phy/dma addr on RC, allocated by dma_alloc_coherent */
	struct gen_pool *pool;
};

extern struct excalibur_ep *excalibur_ep;
extern struct gen_pool *excalibur_ep_get_pool(void);
extern int excalibur_eeo_wait_for_rc_buffer_ready(void *ep_buffer, u32 size);
extern int excalibur_eei_wait_for_rc_size_and_buffer_ready(u32 *size);
extern u32 excalibur_ep_get_xfer_size(void);
extern int excalibur_ero_wait_rc_query_size(void);
extern int excalibur_ep_ob(dma_addr_t dma_src, u32 size);
extern int excalibur_ep_ib(void *ep_buffer, u32 size);
extern int excalibur_ero_prepare(void *ep_buffer);
extern int excalibur_ero_wait_dma_complete(void *ep_buffer);
extern int excalibur_eri_prepare(void *ep_buffer, u32 size);
extern int excalibur_eri_wait_dma_complete(void);
#endif

#define excalibur_readl_poll_timeout(msg, cond)                                                \
	({                                                                                     \
		u32 msg;                                                                       \
		if (debug_poll)                                                                \
			pr_info("before: %s %d\n", __func__, __LINE__);                        \
		ret = readl_poll_timeout(&msginfo->msg, msg, cond,                             \
					 poll_delay_us, poll_timeout);                         \
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

#define excalibur_module_parameters                                            \
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
