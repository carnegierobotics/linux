// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do EP inbound uDMA on EP-side
 * Please also check RC_ep_ib.c to see what you should do on RC-side.
 *
 * History: 2022/11/25 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <soc/ambarella/moemoekyun.h>
#include "moemoekyun_fixed_buf.h"

static int __init rei_init(void)
{
	int ep_index;
	dma_addr_t rc_dma_addr;
	bool rc_is_src = true; /* rc buffer is used as src */
	struct pci_dev *pdev;
	struct moemoekyun_rc *moemoekyun_rc;

	for_each_ep(ep_index)
	{
		pdev = endpoints_info->pdev[ep_index];
		moemoekyun_rc = dev_get_drvdata(&pdev->dev);
		rc_dma_addr = moemoekyun_fix_buffer(&moemoekyun_rc->pdev->dev,
						    rc_is_src);
		if (!rc_dma_addr)
			return -ENOMEM;

		pr_info("rc dma addr is %llx, size is %x, rc buffer crc32 is %x",
			dma_handle, xfer_size, crc32_le(~0, buffer, xfer_size));
		moemoekyun_rc_tell_ep_dma_range(ep_index, dma_handle,
						xfer_size);
		print_hex_dump(KERN_INFO, "rc buffer ", DUMP_PREFIX_OFFSET, 16,
			       1, buffer, xfer_size, false);
	}

	return 0;
}

static void __exit rei_exit(void)
{
	struct pci_dev *pdev;
	int ep_index;
	struct moemoekyun_rc *moemoekyun_rc;
	for_each_ep(ep_index)
	{
		pdev = endpoints_info->pdev[ep_index];
		moemoekyun_rc = dev_get_drvdata(&pdev->dev);
		dma_free_coherent(&moemoekyun_rc->pdev->dev, xfer_size, buffer,
				  dma_handle);
	}
}

module_init(rei_init);
module_exit(rei_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for EP inbound");
MODULE_LICENSE("GPL");
