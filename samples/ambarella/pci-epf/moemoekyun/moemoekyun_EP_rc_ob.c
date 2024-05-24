// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC outbound uDMA on EP-side
 * Please also check RC_rc_ob.c to see what you should do on RC-side.
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

static int __init ero_init(void)
{
	int ret = 0;
	u32 rc_checksum, ep_checksum;

	ep_checksum = crc32_le(~0, moemoekyun_ep->mem_bar_base, xfer_size);
	pr_info("ep buffer crc32 is %x\n", ep_checksum);
	rc_checksum = memokey_fix_expected_crc32(&moemoekyun_ep->epf->dev);
	pr_info("%s: rc_dma crc32 is %x, ep_dma crc32 is %x, is checksum matched? %s, size is %x\n",
		__func__, rc_checksum, ep_checksum,
		rc_checksum == ep_checksum ? "Yes" : "No", xfer_size);

	print_hex_dump(KERN_INFO, "ep buffer ", DUMP_PREFIX_OFFSET, 16, 1,
		       moemoekyun_ep->mem_bar_base, xfer_size, false);
	return ret;
}

static void __exit ero_exit(void)
{
	/* EP doesn't allocate addition buffer, so there is no need to free */

	/* zero out bar */
	memset(moemoekyun_ep->mem_bar_base, 0, xfer_size);
}

module_init(ero_init);
module_exit(ero_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for RC outbound");
MODULE_LICENSE("GPL");
