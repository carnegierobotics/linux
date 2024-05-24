// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2023 Ambarella International LP
 */

#ifndef __RPMSG_SLAVE_H
#define __RPMSG_SLAVE_H

#include <linux/interrupt.h>
#include <linux/virtio.h>

#define VIRTIO_ID_BLZNET		0x00beebee

struct virtio_slave {
	phys_addr_t start_pa;
	phys_addr_t end_pa;
	void *__iomem buf_va;
	const char *rpmsg_user;
};

int rpmsg_slave_ns_register_device(struct rpmsg_device *rpdev);

u64 virtqueue_slave_pick_avaid_buffer(struct virtqueue *_vq,
				      unsigned int *len);
int virtqueue_slave_recycle_used_buffer(struct virtqueue *vq,
				   unsigned int num,
				   unsigned int len,
				   void *data,
				   gfp_t gfp);

irqreturn_t vring_slave_interrupt(int irq, void *_vq);

u32 ambarella_rpmsg_buffer_size(void);
#endif /*__RPMSG_SLAVE_H */
