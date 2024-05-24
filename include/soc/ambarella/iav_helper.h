/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 * Copyright (C) 2004-2010, Ambarella, Inc.
 */

#ifndef __SOC_AMBARELLA_SERVICE_H
#define __SOC_AMBARELLA_SERVICE_H

/*===========================================================================*/

#define AMBFB_IS_PRIVATE_EVENT(evt)		(((evt) >> 16) == 0x5252)
#define AMBFB_EVENT_OPEN			0x52520001
#define AMBFB_EVENT_RELEASE			0x52520002
#define AMBFB_EVENT_CHECK_PAR			0x52520003
#define AMBFB_EVENT_SET_PAR			0x52520004
#define AMBFB_EVENT_PAN_DISPLAY			0x52520005
#define AMBFB_EVENT_SET_CMAP			0x52520006
#define AMBFB_EVENT_REQUEST_FB			0x52520007
#define AMBFB_EVENT_RELEASE_FB			0x52520008

/*===========================================================================*/

extern struct proc_dir_entry *ambarella_procfs_dir(void);
static inline struct proc_dir_entry *get_ambarella_proc_dir(void)
{
	return ambarella_procfs_dir();
}

/*===========================================================================*/

extern void ambcache_clean_range(void *addr, unsigned int size);
extern void ambcache_inv_range(void *addr, unsigned int size);

/*===========================================================================*/

unsigned long get_ambarella_iavmem_phys(void);
unsigned int get_ambarella_iavmem_size(void);

/*===========================================================================*/

extern int ambpriv_i2c_update_addr(const char *name, int bus, int addr);

/*===========================================================================*/

extern void ambarella_detect_sd_slot(int slotid, int fixed_cd);

/*===========================================================================*/

#endif

