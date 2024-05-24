// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC inbound uDMA on EP-side
 * Please also check RC_rc_ib.c to see what you should do on RC-side.
 *
 * History: 2022/03/10 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/genalloc.h>
#include <soc/ambarella/excalibur.h>

static int __init eri_init(void)
{
	u32 xfer_size;
	struct gen_pool *pool;
	void *ep_buffer;
	dma_addr_t ep_dma_addr;

	/* set size and will tell it to EP later */
	get_random_bytes(&xfer_size, sizeof xfer_size);
	xfer_size = 1u + (xfer_size % SZ_4M);

	//pr_info("%s %d\n", __func__, __LINE__);

	/* allocate memory */
	pool = excalibur_ep_get_pool();
	if (!pool) {
		pr_err("%s failed to get pool\n", __func__);
		return -ENOMEM;
	}
	ep_buffer = gen_pool_dma_alloc(pool, xfer_size, &ep_dma_addr);
	if (!ep_buffer)
		return -ENOMEM;

	//pr_info("%s %d\n", __func__, __LINE__);
	get_random_bytes(ep_buffer, xfer_size);

	/* tell EP RC's addr, size then wait for transformation done*/
	excalibur_eri_prepare(ep_buffer, xfer_size);
	excalibur_eri_wait_dma_complete();
	/* free mem allocated from pool, end-user shouldn't forget it */
	gen_pool_free(pool, (unsigned long)ep_buffer, xfer_size);
	return 0;
}

static void __exit eri_exit(void)
{
	//pr_info("Cleaning up module.\n");
}

module_init(eri_init);
module_exit(eri_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for RC inbound");
MODULE_LICENSE("GPL");
