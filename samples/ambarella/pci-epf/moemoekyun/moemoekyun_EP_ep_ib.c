// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do EP inbound uDMA on EP-side
 * Please also check RC_ep_ib.c to see what you should do on RC-side.
 * You should modprobe RC_ep_ib on RC-side before modprobe this module.
 *
 * History: 2022/11/25 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/genalloc.h>
#include <soc/ambarella/moemoekyun.h>
#include <linux/kthread.h>
#include "moemoekyun_fixed_buf.h"

static int __init eei_init(void)
{
	dma_addr_t ep_dma_addr, rc_dma_addr;
	int ret = 0, rc_checksum, ep_checksum, orig_ep_checksum;
	u32 rc_dma_size;
	bool ep_is_src = false; /* ep buffer is used as dst */

	ep_dma_addr =
		moemoekyun_fix_buffer(&moemoekyun_ep->epf->dev, ep_is_src);
	orig_ep_checksum = crc32_le(~0, buffer, xfer_size);
	if (!ep_dma_addr)
		return -ENOMEM;

	rc_dma_addr = moemoekyun_get_rc_dma_addr();
	rc_dma_size = moemoekyun_get_rc_dma_region_size();

	ret = moemoekyun_pci_ep_ib(ep_dma_addr, rc_dma_addr, xfer_size);
	if (ret < 0)
		return ret;
	rc_checksum = memokey_fix_expected_crc32(&moemoekyun_ep->epf->dev);
	ep_checksum = crc32_le(~0, buffer, xfer_size);
	if (orig_ep_checksum == ep_checksum)
		pr_warn("It seems like the transfer didn't start or failed\n");
	pr_info("%s: rc_dma crc32 is %x, ep_dma crc32 is %x, is checksum matched? %s, rc_dma_addr is %llx, ep_dma_addr is %llx, size is %x\n",
		__func__, rc_checksum, ep_checksum,
		rc_checksum == ep_checksum ? "Yes" : "No", rc_dma_addr,
		ep_dma_addr, xfer_size);

	print_hex_dump(KERN_INFO, "rei: ep buffer ", DUMP_PREFIX_OFFSET, 16, 1,
		       buffer, xfer_size, false);
	return ret;
}

static void __exit eei_exit(void)
{
	dma_free_coherent(&moemoekyun_ep->epf->dev, xfer_size, buffer,
			  dma_handle);
}

module_init(eei_init);
module_exit(eei_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for EP inbound");
MODULE_LICENSE("GPL");
