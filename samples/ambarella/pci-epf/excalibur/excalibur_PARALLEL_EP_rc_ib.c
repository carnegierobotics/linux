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
#include "macros.h"
#include <soc/ambarella/excalibur.h>

static struct semaphore sem;

static int kthread_function(void *unused)
{
	u32 xfer_size;
	struct gen_pool *pool;
	void *ep_buffer;
	dma_addr_t ep_dma_addr;

	/* set size and will tell it to EP later */
	xfer_size = SZ_1M;

	/* allocate memory */
	pool = excalibur_ep_get_pool();
	if (!pool)
		return -ENOMEM;
	// ok
	mutex_lock(&excalibur_ep->mutex);
	ep_buffer = gen_pool_dma_alloc(pool, xfer_size, &ep_dma_addr);
	// WARNING: RC_IB thread 2/298 still has locks held!
	//mutex_lock(&excalibur_ep->mutex);
	if (!ep_buffer)
		return -ENOMEM;

	// fuck
	//mutex_lock(&excalibur_ep->mutex);
	get_random_bytes(ep_buffer, xfer_size);

	//mutex_lock(&excalibur_ep->mutex);
	/* tell EP RC's addr, size then wait for transformation done*/
	excalibur_eri_prepare(ep_buffer, xfer_size);

	/* free mem allocated from pool, end-user shouldn't forget it */
	gen_pool_free(pool, (unsigned long)ep_buffer, xfer_size);

	up(&sem);
	return 0;
}
static int __init eri_init(void)
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

static void __exit eri_exit(void)
{
}

module_init(eri_init);
module_exit(eri_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for RC inbound");
MODULE_LICENSE("GPL");
