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
#include <soc/ambarella/excalibur.h>
#include "macros.h"
#include <linux/kthread.h>

struct semaphore sem;

static int kthread_function(void *unused)
{
	void *ep_buffer;
	dma_addr_t ep_dma_addr;
	int ret = 0;
	struct gen_pool *pool;
	u32 xfer_size;

	/* wait for RC to tell EP size and src addr */
	ret = excalibur_eei_wait_for_rc_size_and_buffer_ready(&xfer_size);
	if (ret < 0) {
		pr_err("%s %d, wait fail\n", __func__, __LINE__);
		return ret;
	}

	/* allocate memory */
	pool = excalibur_ep_get_pool();
	if (!pool)
		return -ENOMEM;
	ep_buffer = gen_pool_dma_alloc(pool, xfer_size, &ep_dma_addr);
	if (!ep_buffer)
		return -ENOMEM;

	ret = excalibur_ep_ib(ep_buffer, xfer_size);
	if (ret < 0)
		return ret;

	gen_pool_free(pool, (unsigned long)ep_buffer, xfer_size);

	up(&sem);
	return ret;
}

static int __init eei_init(void)
{
	struct task_struct *task1;
	int i;

	sema_init(&sem, 0);

	for (i = 0; i < NUM_KTHREADS; i++) {
		task1 = kthread_run(kthread_function, NULL, "EP_IB thread 2");
		if (IS_ERR(task1)) {
			pr_err("kthread_run fail\n");
			return PTR_ERR(task1);
		}
	}
	for (i = 0; i < NUM_KTHREADS; i++)
		down(&sem);
	return 0;
}
static void __exit eei_exit(void)
{
	//pr_info("Cleaning up module.\n");
}

module_init(eei_init);
module_exit(eei_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("EP module for EP inbound");
MODULE_LICENSE("GPL");
