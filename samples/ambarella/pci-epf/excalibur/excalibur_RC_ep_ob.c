// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do EP outbound uDMA on RC-side
 * Please also check EP_ep_ob.c to see what you should do on EP-side.
 *
 * History: 2022/03/10 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include "asm-generic/errno-base.h"
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/genalloc.h>
#include <soc/ambarella/epf-core.h>
#include <soc/ambarella/excalibur.h>

static int __init reo_init(void)
{
	int ep_index;
	dma_addr_t rc_dma_addr;
	void *rc_buffer;
	struct gen_pool *pool;
	u32 xfer_size;
	int ret;

	/* Presuite: EPs are waiting for RC buffer. */
	for_each_ep(ep_index)
	{
		ret = excalibur_rc_check_ep(ep_index);
		if (ret)
			return ret;

		ret = excalibur_reo_wait_ep_query_size(ep_index);
		if (ret < 0)
			return ret;

		/* get size from EP */
		xfer_size = excalibur_rc_get_xfer_size(ep_index);

		/* allocate memory for RC buffer */
		pool = excalibur_rc_get_pool(ep_index);
		if (!pool)
			return -ENODEV;

		rc_buffer = gen_pool_dma_alloc(pool, xfer_size, &rc_dma_addr);
		if (!rc_buffer)
			return -ENOMEM;

		/* tell EP RC's addr and do some preparations */
		excalibur_reo_prepare(ep_index, rc_buffer);

		excalibur_reo_wait_dma_complete(ep_index, rc_buffer);

		/* free mem allocated from pool, end-user shouldn't forget it */
		gen_pool_free(pool, (unsigned long)rc_buffer, xfer_size);
	}

	return 0;
}

static void __exit reo_exit(void)
{
	//pr_info("Cleaning up module.");
}

module_init(reo_init);
module_exit(reo_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for EP outbound");
MODULE_LICENSE("GPL");
