// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC outbound uDMA on RC-side
 * Please also check EP_rc_ob.c to see what you should do on EP-side.
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
#include <soc/ambarella/epf-core.h>
#include <soc/ambarella/excalibur.h>

static int __init rro_init(void)
{
	int ep_index;
	int ret;
	void *rc_buffer;
	dma_addr_t rc_dma_src;
	struct gen_pool *pool;
	u32 xfer_size;

	for_each_ep(ep_index)
	{
		ret = excalibur_rc_check_ep(ep_index);
		if (ret)
			return ret;

		/* set size, will usd by EP later */
		get_random_bytes(&xfer_size, sizeof xfer_size);
		xfer_size = 1u + (xfer_size % SZ_4M);

		excalibur_rc_set_xfer_size(ep_index, xfer_size);

		/* allocate memory for RC buffer */
		pool = excalibur_rc_get_pool(ep_index);
		if (!pool)
			return -ENODEV;
		rc_buffer = gen_pool_dma_alloc(pool, xfer_size, &rc_dma_src);
		if (!rc_buffer)
			return -ENOMEM;

		get_random_bytes(rc_buffer, xfer_size);
		/* Wait for EP's buffer addr */
		ret = excalibur_rro_wait_for_ep_buffer_ready(ep_index, rc_buffer,
							     xfer_size);
		if (ret < 0)
			goto fail;

		ret = excalibur_rc_ob(ep_index, rc_dma_src, xfer_size);
		if (ret < 0)
			pr_err("rc_ob fail\n");
	fail:
		gen_pool_free(pool, (unsigned long)rc_buffer, xfer_size);
	}

	return 0;
}

static void __exit rro_exit(void)
{
	//pr_info("Cleaning up module.\n");
}

module_init(rro_init);
module_exit(rro_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for RC outbound");
MODULE_LICENSE("GPL");
