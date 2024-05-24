/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * amab-bsb.h - PCI BSB uapi defines
 *
 * Copyright (C) 2023 Ambarella.Inc
 * Author: Li Chen <lchen@ambarella.com>
 *
 */

#ifndef __UAPI_LINUX_PCIAMBABSB_H
#define __UAPI_LINUX_PCIAMBABSB_H

#ifndef __KERNEL__
#include <stdint.h>
#include <stddef.h>
#else
#include <linux/types.h>
#endif

struct msg_info {
#ifdef __KERNEL__
	void __user *base;
#else
	void *base;
#endif
	size_t size;
};

struct subdevice_rmem {
	uint64_t start_addr;
	uint32_t size;
};

struct subdevices_info {
	uint32_t nr_subdevices;
#define MAX_NR_SUBDEVICES 16
	struct subdevice_rmem subdevice_rmem[MAX_NR_SUBDEVICES];
};

/* For RC */
struct rc_msg2ep_and_msg2rc {
	struct msg_info msg2ep;
	struct msg_info msg2rc;
	int subdevice_idx;
};

#define BSB_RC_MSG2EP_THEN_WAIT_FOR_MSG2RC                                     \
	_IOR('B', 1, struct rc_msg2ep_and_msg2rc)
#define BSB_RC_GET_RC_SUBDEVICES_INFO _IOR('B', 3, struct subdevices_info)

/* For EP */
struct ep_msg2rc {
	struct msg_info msg2rc;
	int subdevice_idx;
};

enum DIR {
	PCI_READ,
	PCI_WRITE,
};

struct xfer_info {
	uint64_t remote_phy_addr;
	uint64_t local_phy_addr;
	size_t size;
	enum DIR dir;
	int subdevice_idx;
};

struct ep_msg2ep {
	struct msg_info msg2ep;
	int subdevice_idx;
};

#define BSB_EP_GET_RC_SUBDEVICES_INFO _IOR('B', 3, struct subdevices_info)
#define BSB_EP_WAIT_RC_MSG2EP _IOR('B', 2, struct ep_msg2ep)
#define BSB_EP_XFER _IOR('B', 1, struct xfer_info)
#define BSB_EP_SEND_MSG2RC_INTERRUPT_RC _IOR('B', 1, struct ep_msg2rc)

#endif /* __UAPI_LINUX_PCIAMBABSB_H */
