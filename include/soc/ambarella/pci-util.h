/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PCIe utility header, used by various Ambarella PCIe drivers.
 * Copyright (C) 2023 by Ambarella, Inc.
 */

#include <linux/dmaengine.h>

#ifndef AMBARELLA_PCI_UTIL_H
#define AMBARELLA_PCI_UTIL_H

typedef void (*dma_callback_t)(void *param);

extern bool ambarella_is_cdns_udma(struct dma_chan *chan, void *data);

extern struct device *ambarella_get_pcie_root_complex(struct device *dev);

extern struct dma_chan *
ambarella_acquire_udma_chan(enum dma_transfer_direction dir,
			    struct device *dev);
extern int ambarella_pci_udma_xfer(struct device *dev, dma_addr_t dma_dst,
				   dma_addr_t dma_src, u32 total_len,
				   enum dma_transfer_direction dir,
				   struct dma_chan *chan,
				   dma_callback_t callback,
				   void __iomem *msginfo);

extern int ambarella_copy_from_user_toio(volatile void __iomem *dst,
			       const void __user *src, size_t count);
extern int ambarella_copy_to_user_fromio(void __user *dst,
			       const volatile void __iomem *src, size_t count);
extern int ambarella_copy_from_user_toio_l(volatile void __iomem *dst,
				 const void __user *src, size_t count);
extern int ambarella_copy_to_user_fromio_l(void __user *dst,
				 const volatile void __iomem *src,
				 size_t count);
#endif
