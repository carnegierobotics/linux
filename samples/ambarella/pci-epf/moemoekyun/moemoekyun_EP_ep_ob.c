// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do EP outbound uDMA on EP-side
 * Please also check RC_ep_ob.c to see what you should do on RC-side.
 *
 * History: 2022/11/25 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <soc/ambarella/moemoekyun.h>
#include "moemoekyun_fixed_buf.h"

static int __init eeo_init(void)
{
	dma_addr_t ep_dma_addr, rc_dma_addr;
	int ret = 0, ep_checksum;
	u32 rc_dma_size;
	bool ep_is_src = true; /* ep buffer is used as dst */

	ep_dma_addr =
		moemoekyun_fix_buffer(&moemoekyun_ep->epf->dev, ep_is_src);
	if (!ep_dma_addr)
		return -ENOMEM;

	rc_dma_addr = moemoekyun_get_rc_dma_addr();
	rc_dma_size = moemoekyun_get_rc_dma_region_size();

	ret = moemoekyun_pci_ep_ob(rc_dma_addr, ep_dma_addr, xfer_size);
	if (ret < 0)
		return ret;
	dev_info(&moemoekyun_ep->epf->dev, "%s: rc_dma_addr is %llx\n",
		 __func__, rc_dma_addr);
	ep_checksum = crc32_le(~0, buffer, xfer_size);
	print_hex_dump(KERN_INFO, "eeo: ep buffer ", DUMP_PREFIX_OFFSET, 16, 1,
		       buffer, xfer_size, false);
	return ret;
}

static void __exit eeo_exit(void)
{
	dma_free_coherent(&moemoekyun_ep->epf->dev, xfer_size, buffer,
			  dma_handle);
}

module_init(eeo_init);
module_exit(eeo_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for EP outbound");
MODULE_LICENSE("GPL");
