/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Ambarella Moemoekyun endpoint function RC|EP driver helpers.
 * Copyright (C) 2022 by Ambarella, Inc.
 */

#ifndef AMBARELLA_MOEMOEKYUN
#define AMBARELLA_MOEMOEKYUN
#include <linux/types.h>
#include <linux/types.h>
#include <linux/genalloc.h>
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

#define MOEMOEKYUN_PCIE_DEVICE_ID 0x0305

/*
 * Datas stored in EP's msg bar
 *
 * Always use datas whose size <= SZ_4 data,
 * because cadence doesn't allow
 * data size(AXI_AR/WLEN) > 4 to be
 * transfered.
 */
struct moemoekyun_msg {
	u32 rc_dma_addr; /* used by EP OB and IB */
	u32 rc_dma_upper_addr;
	u32 rc_dma_region_size;
};

#ifdef CONFIG_AMBARELLA_MOEMOEKYUN_RC

struct moemoekyun_rc {
	struct pci_dev *pdev;
	void __iomem *bar[PCI_STD_NUM_BARS];

	void *rc_buffer; /* will be added to gen_pool with gen_pool_add_virt */
	dma_addr_t rc_dma_addr;
	struct dma_chan *dma_chan_tx;
	struct dma_chan *dma_chan_rx;
};

extern void moemoekyun_rc_tell_ep_dma_range(int index, dma_addr_t rc_dma_addr,
					    u32 size);
extern int moemoekyun_pci_rc_ib(int ep_index, dma_addr_t dma_dst,
				dma_addr_t dma_src, u32 tranlen,
				struct moemoekyun_msg __iomem *msginfo);
extern int moemoekyun_pci_rc_ob(int ep_index, dma_addr_t ep_dma_addr,
				dma_addr_t dma_src, u32 tranlen,
				struct moemoekyun_msg __iomem *msginfo);
#endif

#ifdef CONFIG_AMBARELLA_MOEMOEKYUN_EP
struct moemoekyun_ep {
	struct dma_chan *dma_chan_tx;
	struct dma_chan *dma_chan_rx;
	struct mutex mutex;
	struct mutex cmd_mutex;
	void __iomem *bar[PCI_STD_NUM_BARS];
	enum pci_barno reg_bar;

	struct delayed_work cmd_handler;

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

extern struct moemoekyun_ep *moemoekyun_ep;
extern dma_addr_t moemoekyun_get_rc_dma_region_size(void);
extern int moemoekyun_pci_ep_ib(dma_addr_t dma_dst, dma_addr_t dma_src,
				u32 total_len);
extern int moemoekyun_pci_ep_ob(dma_addr_t dma_dst, dma_addr_t dma_src,
				u32 total_len);
extern dma_addr_t moemoekyun_get_rc_dma_region_size(void);
extern dma_addr_t moemoekyun_get_rc_dma_addr(void);
#endif

#endif
