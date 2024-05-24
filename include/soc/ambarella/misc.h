/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2016-2048, Ambarella, Inc.
 *
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 */


#ifndef __SOC_AMBARELLA_MISC_H__
#define __SOC_AMBARELLA_MISC_H__
#include <linux/io.h>

#define memcpy_fromio_ambarella(a, c, l)	__memcpy_fromio_ambarella((a), (c), (l))
#define memcpy_toio_ambarella(c, a, l)	__memcpy_toio_ambarella((c), (a), (l))

extern void __memcpy_fromio_ambarella(void *to, const volatile void __iomem *from, size_t count);
extern void __memcpy_toio_ambarella(volatile void __iomem *to, const void *from, size_t count);
extern unsigned int ambarella_sys_config(void);
extern struct proc_dir_entry *ambarella_procfs_dir(void);
extern struct dentry *ambarella_debugfs_dir(void);

#endif

