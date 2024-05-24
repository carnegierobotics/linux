// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC inbound uDMA on RC-side
 * Please also check EP_rc_ib.c to see what you should do on EP-side.
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

static int __init rri_init(void)
{
	int ep_index;
	int ret = 0;
	u32 orig_rc_checksum, rc_checksum, ep_checksum;
	u64 rc_dma_addr;
	bool rc_is_src = false;
	struct moemoekyun_rc *moemoekyun_rc;
	struct pci_dev *pdev;
	size_t ep_mem_bar_size;

	for_each_ep(ep_index)
	{
		pdev = endpoints_info->pdev[ep_index];
		moemoekyun_rc = dev_get_drvdata(&pdev->dev);

		rc_dma_addr = moemoekyun_fix_buffer(&moemoekyun_rc->pdev->dev,
						    rc_is_src);
		orig_rc_checksum = crc32_le(~0, buffer, xfer_size);

		ep_mem_bar_size = endpoints_info->ep_mem_bar_size[ep_index];
		if (xfer_size > ep_mem_bar_size)
			return -ENOMEM;

		ret = moemoekyun_pci_rc_ib(
			ep_index, rc_dma_addr,
			endpoints_info->ep_mem_pci_addr[ep_index], xfer_size,
			endpoints_info->msginfo[ep_index]);
		if (ret < 0)
			return ret;

		ep_checksum =
			memokey_fix_expected_crc32(&moemoekyun_rc->pdev->dev);
		rc_checksum = crc32_le(~0, buffer, xfer_size);
		if (orig_rc_checksum == rc_checksum)
			pr_warn("It seems like the transfer didn't start or failed\n");

		dev_info(
			&moemoekyun_rc->pdev->dev,
			"%s: rc_dma crc32 is %x, ep_dma crc32 is %x, is checksum matched? %s, rc_dma_addr is %llx, size is %x, endpoints_info->ep_mem_pci_addr[%d](dma_addr) is %llx\n",
			__func__, rc_checksum, ep_checksum,
			rc_checksum == ep_checksum ? "Yes" : "No", rc_dma_addr,
			xfer_size, ep_index,
			endpoints_info->ep_mem_pci_addr[ep_index]);

		print_hex_dump(KERN_INFO, "rri: rc buffer ", DUMP_PREFIX_OFFSET,
			       16, 1, buffer, xfer_size, false);
	}

	return 0;
}

static void __exit rri_exit(void)
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

module_init(rri_init);
module_exit(rri_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for RC inbound");
MODULE_LICENSE("GPL");
