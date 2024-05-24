// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sample file to demonstrate how to do RC outbound uDMA on RC-side
 * Please also check EP_rc_ob.c to see what you should do on EP-side.
 *
 * History: 2022/03/10 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/semaphore.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/genalloc.h>
#include <soc/ambarella/epf-core.h>
#include "macros.h"
#include <soc/ambarella/excalibur.h>
#include <linux/kthread.h>

struct semaphore sem;

static int rro_function(void *unused)
{
	int ep_index;
	int ret;
	void *rc_buffer;
	dma_addr_t rc_dma_src;
	struct gen_pool *pool;
	u32 xfer_size;
	struct pci_dev *pdev;

	mutex_lock(&endpoints_info->mutex);
	for_each_ep(ep_index)
	{
		pdev = endpoints_info->pdev[ep_index];

		/* set size, will usd by RC later */
		xfer_size = PARALLEL_XFER_SIZE;

		/* allocate memory for RC buffer */
		pool = excalibur_rc_get_pool(ep_index);
		if (!pool)
			return -ENODEV;
		rc_buffer = gen_pool_dma_alloc(pool, xfer_size, &rc_dma_src);
		if (!rc_buffer)
			return -ENOMEM;

		// halting
		ret = excalibur_rro_wait_for_ep_buffer_ready(
			ep_index, rc_buffer, xfer_size);
		if (ret < 0)
			goto fail;

		ret = excalibur_rc_ob(ep_index, rc_dma_src, xfer_size);
		if (ret < 0)
			pr_err("rc_ob fail\n");
	fail:
		gen_pool_free(pool, (unsigned long)rc_buffer, xfer_size);
	}
	mutex_unlock(&endpoints_info->mutex);
	up(&sem);

	return 0;
}
static int rri_function(void *unused)
{
	int ep_index;
	int ret = 0;
	dma_addr_t rc_dma_dst;
	struct gen_pool *pool;
	void *rc_buffer;
	u32 xfer_size;

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

	return 0;
}

static int reo_function(void *unused)
{
	int ep_index;
	dma_addr_t rc_dma_addr;
	void *rc_buffer;
	struct gen_pool *pool;
	u32 xfer_size;

	mutex_lock(&endpoints_info->mutex);
	/* Presuite: EPs are waiting for RC buffer. */
	for_each_ep(ep_index)
	{
		//    pr_info("%s %d waiting ep start\n", __func__, __LINE__);
		excalibur_reo_wait_ep_query_size(ep_index);
		//   pr_info("%s %d start!\n", __func__, __LINE__);
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
	mutex_unlock(&endpoints_info->mutex);
	up(&sem);

	return 0;
}

static int __init rc_mix_init(void)
{
	struct task_struct *task1;
	int i;

	sema_init(&sem, 0);

	for (i = 0; i < NUM_MIX_KTHREADS; i++) {
		//pr_info("%s %d i is %d\n", __func__, __LINE__, i);
		task1 = kthread_run(rro_function, NULL, "RC_OB thread");
		task1 = kthread_run(reo_function, NULL, "EP_OB thread");
	}
	//pr_info("%s %d sem count is %d\n", __func__, __LINE__, sem.count);
	for (i = 0; i < NUM_MIX_KTHREADS; i++) {
		down(&sem);
		down(&sem);
	}
	return 0;
}

static void __exit rc_mix_exit(void)
{
}

module_init(rc_mix_init);
module_exit(rc_mix_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for RC outbound");
MODULE_LICENSE("GPL");
