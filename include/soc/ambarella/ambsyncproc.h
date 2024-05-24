/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 * Copyright (C) 2004-2010, Ambarella, Inc.
 */

#ifndef __SOC_AMBARELLA_SYNC_PROC_H
#define __SOC_AMBARELLA_SYNC_PROC_H

#include <linux/fs.h>
#include <linux/poll.h>

/* ==========================================================================*/
#define AMBA_SYNC_PROC_MAX_ID			(31)
#define AMBA_SYNC_PROC_PAGE_SIZE		(PAGE_SIZE - 16)

/* ==========================================================================*/

typedef	int(ambsync_read_proc_t)(char *start, void *data);

struct ambsync_proc_pinfo {
	u32					id;
	u32					mask;
	char					*page;
};

struct ambsync_proc_hinfo {
	u32					maxid;
	u32					tmo;
	wait_queue_head_t			sync_proc_head;
	atomic_t				sync_proc_flag;
	struct idr				sync_proc_idr;
	struct mutex				sync_proc_lock;
	ambsync_read_proc_t			*sync_read_proc;
	void					*sync_read_data;
};

/* ==========================================================================*/

/* ==========================================================================*/
extern int ambsync_proc_hinit(struct ambsync_proc_hinfo *hinfo);
extern int ambsync_proc_open(struct inode *inode, struct file *file);
extern int ambsync_proc_release(struct inode *inode, struct file *file);
extern ssize_t ambsync_proc_read(struct file *file, char __user *buf, size_t size, loff_t *ppos);
extern ssize_t ambsync_proc_write(struct file *file, const char __user *buf, size_t size, loff_t *ppos);
extern unsigned int ambsync_proc_poll(struct file *file, poll_table *wait);

/* ==========================================================================*/

#endif

