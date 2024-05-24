// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC inbound uDMA on RC-side
 * Please also check EP_rc_ib.c to see what you should do on EP-side.
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
#include <linux/kthread.h>
#include <soc/ambarella/epf-core.h>
#include "macros.h"
#include <soc/ambarella/excalibur.h>

struct semaphore sem;

static int kthread_function(void *unused)
{
	int ep_index;
	int ret = 0;
	dma_addr_t rc_dma_dst;
	struct gen_pool *pool;
	void *rc_buffer;
	u32 xfer_size;

	mutex_lock(&endpoints_info->mutex);
	for_each_ep(ep_index)
	{
		ret = excalibur_rri_wait_for_ep_size_and_buffer_ready(
			ep_index, &xfer_size);
		if (ret < 0)
			return ret;

		/* allocate memory for RC buffer */
		pool = excalibur_rc_get_pool(ep_index);
		if (!pool)
			return -ENODEV;

		rc_buffer = gen_pool_dma_alloc(pool, xfer_size, &rc_dma_dst);
		if (!rc_buffer)
			return -ENOMEM;

		excalibur_rc_ib(ep_index, rc_buffer, xfer_size);

		/* free mem allocated from pool, end-user shouldn't forget it */
		gen_pool_free(pool, (unsigned long)rc_buffer, xfer_size);
	}
	up(&sem);
	mutex_unlock(&endpoints_info->mutex);

	return 0;
}

static int __init rri_init(void)
{
	struct task_struct *task1;
	int i;

	sema_init(&sem, 0);
	for (i = 0; i < NUM_KTHREADS; i++) {
		task1 = kthread_run(kthread_function, NULL, "RC_IB thread 2");
		if (IS_ERR(task1)) {
			pr_err("kthread_run fail\n");
			return PTR_ERR(task1);
		}
	}
	for (i = 0; i < NUM_KTHREADS; i++)
		down(&sem);
	return 0;
}
static void __exit rri_exit(void)
{
}

module_init(rri_init);
module_exit(rri_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for RC inbound");
MODULE_LICENSE("GPL");
