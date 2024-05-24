// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * TEST EP/RC OB/IB in parallel
 *
 * History: 2022/04/01 - Li Chen <lchen@ambarella.com> created file
 *
 * Copyright (C) 2022 by AMBARELLA, Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/genalloc.h>
#include <linux/kthread.h>
#include <soc/ambarella/excalibur.h>
#include "macros.h"

static struct semaphore sem;

static int ero_function(void *unused)
{
	u32 xfer_size;
	struct gen_pool *pool;
	void *ep_buffer;
	dma_addr_t ep_dma_addr;
	int ret = 0;

	mutex_lock(&excalibur_ep->mutex);
	ret = excalibur_ero_wait_rc_query_size();
	if (ret < 0)
		return ret;
	/* get size from RC */
	xfer_size = excalibur_ep_get_xfer_size();

	/* alloc buffer for xfer */
	pool = excalibur_ep_get_pool();
	ep_buffer = gen_pool_dma_alloc(pool, xfer_size, &ep_dma_addr);
	if (!ep_buffer)
		return -ENOMEM;

	/* tell RC EP's addr and do some preparations */
	ret = excalibur_ero_prepare(ep_buffer);
	if (ret < 0)
		goto fail;

	/* Debug only, end-user don't need do this */

fail:
	/* free mem allocated from pool, end-user shouldn't forget it */
	gen_pool_free(pool, (unsigned long)ep_buffer, xfer_size);
	mutex_unlock(&excalibur_ep->mutex);
	up(&sem);
	//	pr_info("=============== %s %d rc ob done! ==============\n", __func__,
	//		__LINE__);
	return ret;
}

static int eeo_function(void *unused)
{
	void *ep_buffer;
	dma_addr_t ep_dma_addr;
	int ret = 0;
	struct gen_pool *pool;
	u32 xfer_size;
	static int count;

	count++;

	mutex_lock(&excalibur_ep->mutex);
	/* set size, will usd by RC later */
	xfer_size = PARALLEL_XFER_SIZE;

	/* alloc buffer for xfer */
	pool = excalibur_ep_get_pool();
	if (!pool)
		return -ENOMEM;
	ep_buffer = gen_pool_dma_alloc(pool, xfer_size, &ep_dma_addr);
	if (!ep_buffer)
		return -ENOMEM;

	/* FIXME: waiting */
	/* wait for RC buffer */
	// TODO: combine excalibur_eeo_wait_for_rc_buffer_ready and excalibur_ep_ob
	ret = excalibur_eeo_wait_for_rc_buffer_ready(ep_buffer, xfer_size);
	if (ret < 0)
		goto fail;

	ret = excalibur_ep_ob(ep_dma_addr, xfer_size);
fail:
	gen_pool_free(pool, (unsigned long)ep_buffer, xfer_size);
	mutex_unlock(&excalibur_ep->mutex);
	up(&sem);
	//	pr_info("=============== %s %d ep ob done! ==============\n", __func__,
	//		__LINE__);
	return ret;
}

static int __init ep_mix_init(void)
{
	struct task_struct *task1;
	int i;

	sema_init(&sem, 0);
	for (i = 0; i < NUM_MIX_KTHREADS; i++) {
		//pr_info("%s %d i is %d\n", __func__, __LINE__, i);
		task1 = kthread_run(ero_function, NULL, "RC_OB thread 2");
		task1 = kthread_run(eeo_function, NULL, "EP_OB thread 2");
	}
	//pr_info("%s %d sem count is %d\n", __func__, __LINE__, sem.count);
	for (i = 0; i < NUM_MIX_KTHREADS; i++) {
		down(&sem);
		down(&sem);
	}
	return 0;
}

static void __exit ep_mix_exit(void)
{
}

module_init(ep_mix_init);
module_exit(ep_mix_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for RC outbound");
MODULE_LICENSE("GPL");
