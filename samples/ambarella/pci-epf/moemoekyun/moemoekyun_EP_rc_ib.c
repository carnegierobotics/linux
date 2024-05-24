// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC inbound uDMA on EP-side
 * Please also check RC_rc_ib.c to see what you should do on RC-side.
 *
 * History: 2022/11/25 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <soc/ambarella/moemoekyun.h>
#include "moemoekyun_fixed_buf.h"

static int __init eri_init(void)
{
	dma_addr_t ep_dma_addr;
	int ret = 0;
	bool ep_is_src = true;

	ep_dma_addr =
		moemoekyun_fix_buffer(&moemoekyun_ep->epf->dev, ep_is_src);
	if (xfer_size > moemoekyun_ep->mem_bar_size) {
		dev_info(
			&moemoekyun_ep->epf->dev,
			"%s: failed to xfer: xfer size %x is large than mem bar size %zx\n",
			__func__, xfer_size, moemoekyun_ep->mem_bar_size);
		return -ENOMEM;
	}
	if (!ep_dma_addr)
		return -ENOMEM;
	/* You can also write to mem_bar_base directly, just like what EP_rc_ob.c does */
	memcpy(moemoekyun_ep->mem_bar_base, buffer, xfer_size);

	pr_info("ep dma addr is %llx, size is %x, ep buffer crc32 is %x\n",
		dma_handle, xfer_size, crc32_le(~0, buffer, xfer_size));
	print_hex_dump(KERN_INFO, "ep buffer ", DUMP_PREFIX_OFFSET, 16, 1,
		       buffer, xfer_size, false);
	return ret;
}

static void __exit eri_exit(void)
{
	dma_free_coherent(&moemoekyun_ep->epf->dev, xfer_size, buffer,
			  dma_handle);
	/* zero out bar */
	memset(moemoekyun_ep->mem_bar_base, 0, xfer_size);
}

module_init(eri_init);
module_exit(eri_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for RC inbound");
MODULE_LICENSE("GPL");
