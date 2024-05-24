// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do EP inbound uDMA on EP-side
 * Please also check RC_ep_ib.c to see what you should do on RC-side.
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
#include <linux/random.h>
#include <soc/ambarella/epf-core.h>
#include <soc/ambarella/excalibur.h>

static int __init rei_init(void)
{
	int ep_index;
	struct gen_pool *pool;
	u32 xfer_size;
	void *rc_buffer;
	dma_addr_t rc_dma_addr;
	int ret;

	for_each_ep(ep_index)
	{
		ret = excalibur_rc_check_ep(ep_index);
		if (ret)
			return ret;

		get_random_bytes(&xfer_size, sizeof xfer_size);

		/* set size and will tell it to EP later */
		xfer_size = 1u + (xfer_size % SZ_4M);

		/* allocate memory for RC buffer */
		pool = excalibur_rc_get_pool(ep_index);
		if (!pool)
			return -ENODEV;

		rc_buffer = gen_pool_dma_alloc(pool, (size_t)xfer_size,
					       &rc_dma_addr);
		if (!rc_buffer)
			return -ENOMEM;

		/* debug only, end-user don't need do to calc checksum and should use their own data buffer */
		get_random_bytes(rc_buffer, xfer_size);

		/* tell EP RC's addr, size then wait for transformation done*/
		excalibur_rei_prepare(ep_index, rc_buffer,
					       xfer_size);

		excalibur_rei_wait_dma_complete(ep_index);

		/* free mem allocated from pool, end-user shouldn't forget it */
		gen_pool_free(pool, (unsigned long)rc_buffer, xfer_size);
	}

	return 0;
}

static void __exit rei_exit(void)
{
	//pr_info("Cleaning up module.\n");
}

module_init(rei_init);
module_exit(rei_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for EP inbound");
MODULE_LICENSE("GPL");
