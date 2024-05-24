/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * amab-neko.h - PCI neko uapi defines
 *
 * Copyright (C) 2023 Ambarella.Inc
 * Author: Li Chen <lchen@ambarella.com>
 *
 */

#ifndef __UAPI_LINUX_PCIAMBANEKO_H
#define __UAPI_LINUX_PCIAMBANEKO_H

#ifndef __KERNEL__
#include <stdint.h>
#include <stddef.h>
#else
#include <linux/types.h>
#endif

enum DIR
{
    PCI_READ,
    PCI_WRITE,
};

struct xfer_info
{
	uint64_t remote_phy_addr;
	uint64_t local_phy_addr;
	size_t size;
	enum DIR dir;
};

#define PCINEKO_XFER		_IOR('N', 0x1, struct xfer_info)

#endif /* __UAPI_LINUX_PCIAMBANEKO_H */
