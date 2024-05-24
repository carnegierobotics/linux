// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC outbound uDMA on RC-side
 * Please also check EP_rc_ob.c to see what you should do on EP-side.
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

static int __init rro_init(void)
{
	int ep_index;
	int ret = 0;
	u32 rc_checksum;
	u64 rc_dma_addr;
	bool rc_is_src = true;
	struct moemoekyun_rc *moemoekyun_rc;
	struct pci_dev *pdev;
	size_t ep_mem_bar_size;

	for_each_ep(ep_index)
	{
		pdev = endpoints_info->pdev[ep_index];
		moemoekyun_rc = dev_get_drvdata(&pdev->dev);

		rc_dma_addr = moemoekyun_fix_buffer(&moemoekyun_rc->pdev->dev,
						    rc_is_src);

		ep_mem_bar_size = endpoints_info->ep_mem_bar_size[ep_index];
		if (xfer_size > ep_mem_bar_size)
			return -ENOMEM;

		ret = moemoekyun_pci_rc_ob(
			ep_index, endpoints_info->ep_mem_pci_addr[ep_index],
			rc_dma_addr, xfer_size,
			endpoints_info->msginfo[ep_index]);
		if (ret < 0)
			return ret;

		rc_checksum = crc32_le(~0, buffer, xfer_size);

		print_hex_dump(KERN_INFO, "rro: rc buffer ", DUMP_PREFIX_OFFSET,
			       16, 1, buffer, xfer_size, false);
	}

	return 0;
}

static void __exit rro_exit(void)
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

module_init(rro_init);
module_exit(rro_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for RC outbound");
MODULE_LICENSE("GPL");
