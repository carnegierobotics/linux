/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * pciambatest.h - PCI test uapi defines
 *
 * Copyright (C) 2017 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 * Copyright (C) 2023 Ambarella.Inc
 * Author: Li Chen <lchen@ambarella.com>
 *
 */

#ifndef __UAPI_LINUX_PCIAMBATEST_H
#define __UAPI_LINUX_PCIAMBATEST_H

#define PCITEST_BAR _IO('P', 0x1)
#define PCITEST_LEGACY_IRQ _IO('P', 0x2)
#define PCITEST_MSI _IOW('P', 0x3, int)
#define PCITEST_WRITE _IOW('P', 0x4, unsigned long)
#define PCITEST_READ _IOW('P', 0x5, unsigned long)
#define PCITEST_COPY _IOW('P', 0x6, unsigned long)
#define PCITEST_MSIX _IOW('P', 0x7, int)
#define PCITEST_SET_IRQTYPE _IOW('P', 0x8, int)
#define PCITEST_GET_IRQTYPE _IO('P', 0x9)
#define PCITEST_CLEAR_IRQ _IO('P', 0x10)
#define PCITEST_MSI_DOORBELL _IO('P', 0x11)

#define PCITEST_FLAGS_USE_DMA 0x00000001
#define PCITEST_FLAGS_DMA_ALLOC_COHERENT 0x00000002
#define PCITEST_FLAGS_VERBOSE_OUTPUT 0x00000004
#define PCITEST_FLAGS_MULT_CHAN_FOR_SINGLE_XFER 0x00000008

/**
 * @size: size of xfer
 * @cdns_dma_mdoe: dma controller mode, either bulk or s/g, only used for CDNS uDMA
 *            0 represents not used(poor man's std::optional),
 *            1 represents bulk mode,
 *            2 represetns s/g.
 *
 */
struct pci_endpoint_amba_test_xfer_param {
	unsigned long size;
	unsigned long cdns_dma_mode;
	unsigned long buffer_split_count;
	unsigned long nr_channels;
	unsigned char flags;
	unsigned nr_repeated_xfer;
};

#endif /* __UAPI_LINUX_PCIAMBATEST_H */
