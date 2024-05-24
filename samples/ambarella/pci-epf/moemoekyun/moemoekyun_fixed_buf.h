/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * macros for samples
 *
 * History: 2022/03/31 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <soc/ambarella/moemoekyun.h>
#include <soc/ambarella/epf-core.h>

static void *buffer;
static dma_addr_t dma_handle;
static const int xfer_size = SZ_256;

// memset only for src buffer
static inline dma_addr_t moemoekyun_fix_buffer(struct device *dev, bool is_src)
{
	if (!buffer) {
		buffer = dma_alloc_coherent(dev, xfer_size, &dma_handle,
					    GFP_KERNEL);
		if (!buffer) {
			pr_info("%s: dma_alloc_coherent failed\n", __func__);
			return 0;
		}
		if (is_src) {
			dev_info(dev, "%s: init src buffer to 0x050505...\n",
				 __func__);
			memset(buffer, 5, xfer_size);
		}
	}
	return dma_handle;
}

static inline int memokey_fix_expected_crc32(struct device *dev)
{
	void *buffer;
	dma_addr_t dma_handle;
	static int expect_crc32;

	if (expect_crc32)
		return expect_crc32;

	buffer = dma_alloc_coherent(dev, xfer_size, &dma_handle, GFP_KERNEL);
	memset(buffer, 5, xfer_size);
	if (!buffer)
		return -ENOMEM;
	expect_crc32 = crc32_le(~0, buffer, xfer_size);
	dma_free_coherent(dev, xfer_size, buffer, dma_handle);
	return expect_crc32;
}
