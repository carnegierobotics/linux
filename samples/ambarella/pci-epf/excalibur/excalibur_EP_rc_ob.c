// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC outbound uDMA on EP-side
 * Please also check RC_rc_ob.c to see what you should do on RC-side.
 *
 * History: 2022/03/10 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/genalloc.h>
#include <soc/ambarella/excalibur.h>

static int __init ero_init(void)
{
	u32 xfer_size;
	struct gen_pool *pool;
	void *ep_buffer;
	dma_addr_t ep_dma_addr;
	int ret = 0;

	ret = excalibur_ero_wait_rc_query_size();
	if  (ret < 0)
	    return ret;
	/* get size from RC */
	xfer_size = excalibur_ep_get_xfer_size();

	/* alloc buffer for xfer */
	pool = excalibur_ep_get_pool();
	if (!pool)
		return -ENOMEM;
	ep_buffer = gen_pool_dma_alloc(pool, xfer_size, &ep_dma_addr);
	if (!ep_buffer)
		return -ENOMEM;

	/* tell RC EP's addr and do some preparations */
	ret = excalibur_ero_prepare(ep_buffer);
	if (ret < 0)
	    goto fail;

	excalibur_ero_wait_dma_complete(ep_buffer);

fail:
	/* free mem allocated from pool, end-user shouldn't forget it */
	gen_pool_free(pool, (unsigned long)ep_buffer, xfer_size);
	return ret;
}

static void __exit ero_exit(void)
{
	//pr_info("Cleaning up module.\n");
}

module_init(ero_init);
module_exit(ero_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for RC outbound");
MODULE_LICENSE("GPL");
