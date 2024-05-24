// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do EP outbound uDMA on RC-side
 * Please also check EP_ep_ob.c to see what you should do on EP-side.
 *
 * History: 2022/11/25 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include "asm-generic/errno-base.h"
#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <soc/ambarella/moemoekyun.h>
#include "moemoekyun_fixed_buf.h"

static int __init reo_init(void)
{
	int ep_index;
	dma_addr_t rc_dma_addr;
	bool rc_is_src = false; /* rc buffer is used as src */
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
			rc_dma_addr, xfer_size,
			crc32_le(~0, buffer, xfer_size));
		moemoekyun_rc_tell_ep_dma_range(ep_index, rc_dma_addr,
						xfer_size);
		/*
		 * This sample module must be loaded before EP_ep_ob because RC needs
		 * to tell EP dma addr and size firstly.
		 *
		 * But moemoekyun doesn't provide mechanism to let RC know when ep ob done.
		 *
		 * So there is no way to compare checksum.
		 */
		dev_info(
			&moemoekyun_rc->pdev->dev,
			"%s: please manually use devmem %llx to check after ep ob done\n",
			__func__, rc_dma_addr);
	}

	return 0;
}

static void __exit reo_exit(void)
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

module_init(reo_init);
module_exit(reo_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for EP outbound");
MODULE_LICENSE("GPL");
