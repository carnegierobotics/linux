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
#include <linux/kthread.h>
#include <linux/init.h>
#include "macros.h"
#include <soc/ambarella/excalibur.h>
#include "macros.h"

static struct semaphore sem;

static int kthread_function(void *unused)
{
	void *ep_buffer;
	dma_addr_t ep_dma_addr;
	int ret = 0;
	struct gen_pool *pool;
	u32 xfer_size;

	mutex_lock(&excalibur_ep->mutex);
	/* set size, will usd by RC later */
	xfer_size = SZ_1M;

	/* alloc buffer for xfer */
	pool = excalibur_ep_get_pool();
	ep_buffer = gen_pool_dma_alloc(pool, xfer_size, &ep_dma_addr);
	if (!ep_buffer)
		return -ENOMEM;

	/* FIXME: waiting */
	/* wait for RC buffer */
	ret = excalibur_eeo_wait_for_rc_buffer_ready(ep_buffer, xfer_size);
	if (ret < 0)
		goto fail;

	ret = excalibur_ep_ob(ep_dma_addr, xfer_size);
fail:
	gen_pool_free(pool, (unsigned long)ep_buffer, xfer_size);
	mutex_unlock(&excalibur_ep->mutex);
	up(&sem);
	return ret;
}

static int __init eeo_init(void)
{
	struct task_struct *task1;
	int i;

	sema_init(&sem, 0);

	for (i = 0; i < NUM_KTHREADS; i++) {
		task1 = kthread_run(kthread_function, NULL, "EP_OB thread 2");
		if (IS_ERR(task1)) {
			pr_err("kthread_run fail\n");
			return PTR_ERR(task1);
		}
	}
	for (i = 0; i < NUM_KTHREADS; i++)
		down(&sem);
	return 0;
}
static void __exit eeo_exit(void)
{
	//pr_info("Cleaning up module.\n");
}

module_init(eeo_init);
module_exit(eeo_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for EP outbound");
MODULE_LICENSE("GPL");
