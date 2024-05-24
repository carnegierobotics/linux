// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do EP outbound uDMA on EP-side
 * Please also check RC_ep_ob.c to see what you should do on RC-side.
 *
 * History: 2022/03/10 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <soc/ambarella/excalibur.h>

static int __init eeo_init(void)
{
	void *ep_buffer;
	dma_addr_t ep_dma_addr;
	int ret = 0;
	struct gen_pool *pool;
	u32 xfer_size;

	/* set size, will usd by RC later */
	get_random_bytes(&xfer_size, sizeof xfer_size);
	xfer_size = 1u + (xfer_size % SZ_4M);

	/* alloc buffer for xfer */
	pool = excalibur_ep_get_pool();
	if (!pool)
		return -ENOMEM;
	ep_buffer = gen_pool_dma_alloc(pool, xfer_size, &ep_dma_addr);
	if (!ep_buffer)
		return -ENOMEM;

	get_random_bytes(ep_buffer, xfer_size);

	ret = excalibur_eeo_wait_for_rc_buffer_ready(ep_buffer, xfer_size);
	if (ret < 0)
		goto fail;

	ret = excalibur_ep_ob(ep_dma_addr, xfer_size);
fail:
	gen_pool_free(pool, (unsigned long)ep_buffer, xfer_size);
	return ret;
}

static void __exit eeo_exit(void)
{
}

module_init(eeo_init);
module_exit(eeo_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for EP outbound");
MODULE_LICENSE("GPL");
