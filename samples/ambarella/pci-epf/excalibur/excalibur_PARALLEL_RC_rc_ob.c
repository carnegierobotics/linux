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
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/genalloc.h>
#include <soc/ambarella/epf-core.h>
#include "macros.h"
#include <soc/ambarella/excalibur.h>

static struct semaphore sem;

static int kthread_function(void *unused)
{
	int ep_index;
	int ret;
	void *rc_buffer;
	dma_addr_t rc_dma_src;
	struct gen_pool *pool;
	u32 xfer_size;
	struct pci_dev *pdev;
	struct excalibur_rc *excalibur_rc;

	mutex_lock(&endpoints_info->mutex);
	for_each_ep(ep_index)
	{
		pdev = endpoints_info->pdev[ep_index];
		excalibur_rc = dev_get_drvdata(&pdev->dev);

		/* set size, will usd by RC later */
		xfer_size = SZ_1M;

		/* allocate memory for RC buffer */
		pool = excalibur_rc_get_pool(ep_index);
		if (!pool)
			return -ENODEV;
		rc_buffer = gen_pool_dma_alloc(pool, xfer_size, &rc_dma_src);
		if (!rc_buffer)
			return -ENOMEM;

		/* Wait for EP's buffer addr */
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

static int __init rro_init(void)
{
	struct task_struct *task1;
	int i;

	sema_init(&sem, 0);

	for (i = 0; i < NUM_KTHREADS; i++) {
		task1 = kthread_run(kthread_function, NULL, "RC_OB thread 2");
		if (IS_ERR(task1)) {
			pr_err("kthread_run fail\n");
			return PTR_ERR(task1);
		}
	}
	for (i = 0; i < NUM_KTHREADS; i++)
		down(&sem);
	return 0;
}

static void __exit rro_exit(void)
{
}

module_init(rro_init);
module_exit(rro_exit);

MODULE_AUTHOR("Li Chen <lchen@ambarella.com>");
MODULE_DESCRIPTION("RC module for RC outbound");
MODULE_LICENSE("GPL");
