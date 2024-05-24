/*
* linux/drivers/usb/gadget/ambarella_udc.c
* driver for High/Full speed USB device controller on Ambarella processors
*
* History:
*	2008/06/12 - [Cao Rongrong] created file
*	2009/03/16 - [Cao Rongrong] Change DMA descriptor allocate mode
*
* Copyright (C) 2008 by Ambarella, Inc.
* http://www.ambarella.com
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc., 59 Temple Place - Suite 330,
* Boston, MA  02111-1307, USA.
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/bits.h>
#include <linux/platform_device.h>
#include <linux/sys_soc.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/usb.h>
#include <linux/usb/gadget.h>
#include <linux/usb/otg.h>
#include <linux/proc_fs.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <soc/ambarella/misc.h>
#include "ambarella_udc.h"

/* ==========================================================================*/

//#define AHBSP_USB_SIDEBAND_OFFSET	0x94

#define UDC_SOFT_RESET_OFFSET		0x2cc

/* ==========================================================================*/

#define DRIVER_DESC	"Ambarella USB Device Controller Gadget"
#define DRIVER_VERSION	"21 Jan 2016"
#define DRIVER_AUTHOR	"Cao Rongrong <rrcao@ambarella.com>"

#define	DMA_ADDR_INVALID	(~(dma_addr_t)0)

static const char		gadget_name[] = "ambarella_udc";
static const char		driver_desc[] = DRIVER_DESC;
static const char 		ep0name [] = "ep0";

static const char *amb_ep_string[] = {
	"ep0in", "ep1in", "ep2in", "ep3in",
	"ep4in", "ep5in", "ep6in", "ep7in",
	"ep8in", "ep9in", "ep10in", "ep11in",
	"ep12in", "ep13in", "ep14in", "ep15in",

	"ep0out", "ep1out", "ep2out", "ep3out",
	"ep4out", "ep5out", "ep6out", "ep7out",
	"ep8out", "ep9out", "ep10out", "ep11out",
	"ep12out", "ep13out", "ep14out", "ep15out"
};

/*************************** DEBUG FUNCTION ***************************/
#define DEBUG_NORMAL		0
#define DEBUG_VERBOSE		1

#define AMBARELLA_USB_DEBUG 0
#if AMBARELLA_USB_DEBUG

#define dprintk(level,format,args...) \
	do {	\
		if(level > 0) \
			printk(KERN_DEBUG "%s: " format, __FUNCTION__,## args);	\
	} while(0)

#else
#define dprintk(args...) do { } while(0)
#endif


static inline struct ambarella_ep *to_ambarella_ep(struct usb_ep *ep)
{
	return container_of(ep, struct ambarella_ep, ep);
}

static inline struct ambarella_udc *to_ambarella_udc(struct usb_gadget *gadget)
{
	return container_of(gadget, struct ambarella_udc, gadget);
}

static inline struct ambarella_request *to_ambarella_req(struct usb_request *req)
{
	return container_of(req, struct ambarella_request, req);
}


/**************  PROC FILESYSTEM BEGIN *****************/

static void ambarella_uevent_work(struct work_struct *data)
{
	struct ambarella_udc *udc;
	char buf_status[64];
	char buf_vbus[64];
	char buf_driver[64];
	char *envp[4];

	udc = container_of(data, struct ambarella_udc, uevent_work);
	buf_status[0] = '\0';
	buf_vbus[0] = '\0';
	buf_driver[0] = '\0';

	spin_lock_irq(&udc->lock);
	if (udc->pre_state == udc->gadget.state) {
		spin_unlock_irq(&udc->lock);
		return;
	}

	udc->pre_state = udc->gadget.state;
	snprintf(buf_status, sizeof(buf_status),
		"AMBUDC_STATUS=%s", usb_state_string(udc->gadget.state));
	snprintf(buf_vbus, sizeof(buf_vbus), "AMBUDC_VBUS=%s",
		udc->vbus_status ? "Connected" : "Disconnected");
	snprintf(buf_driver, sizeof(buf_driver), "AMBUDC_DRIVER=%s",
		udc->driver ? udc->driver->driver.name : "NULL");
	spin_unlock_irq(&udc->lock);

	envp[0] = buf_status;
	envp[1] = buf_vbus;
	envp[2] = buf_driver;
	envp[3] = NULL;
	kobject_uevent_env(&udc->dev->kobj, KOBJ_CHANGE, envp);
}

/* ========================================================================== */
static int ambarella_udc_proc_show(struct seq_file *m, void *v)
{
	struct ambarella_udc *udc;

	udc = (struct ambarella_udc *)m->private;

	seq_printf(m, "AMBUDC_STATUS=%s",
			usb_state_string(udc->gadget.state));
	seq_printf(m, " (%s: %s)\n", (udc->driver ?
			udc->driver->driver.name : "NULL"),
			udc->vbus_status ? "Connected" : "Disconnected");

	return 0;
}

static int ambarella_udc_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambarella_udc_proc_show, PDE_DATA(inode));
}

static const struct proc_ops ambarella_udc_fops = {
	.proc_open = ambarella_udc_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

#ifdef CONFIG_USB_GADGET_DEBUG_FILES

static const char proc_node_name[] = "driver/udc";

static int ambarella_debugfs_udc_read(char *page, char **start,
	off_t off, int count, int *eof, void *_dev)
{
	char *buf = page;
	struct ambarella_udc *udc = _dev;
	char *next = buf;
	unsigned size = count;
	int t;
	struct usb_ctrlrequest *crq = (struct usb_ctrlrequest *)&udc->setup[0];

	if (off != 0)
		return 0;

	/* basic device status */
	t = scnprintf(next, size,
		DRIVER_DESC "\n"
		"Name: %s\n"
		"Version: %s\n"
		"Author: %s\n\n"
		"Gadget driver: %s\n"
		"Host: %s\n\n",
		gadget_name,
		DRIVER_VERSION,
		DRIVER_AUTHOR,
		udc->driver ? udc->driver->driver.name : "(none)",
		udc->vbus_status ? (udc->gadget.speed == USB_SPEED_HIGH ?
			"high speed" : "full speed") : "disconnected");
	size -= t;
	next += t;

	t = scnprintf(next, size, "the last setup packet is: \n"
		"bRequestType = 0x%02x, bRequest = 0x%02x,\n"
		"wValue = 0x%04x, wIndex = 0x%04x, wLength = 0x%04x\n\n",
		crq->bRequestType, crq->bRequest, crq->wValue, crq->wIndex,
		crq->wLength);
	size -= t;
	next += t;

/*
	t = scnprintf(next, size, "max_cmd_num = %d\tmax_ep0_cmd_num = %d\n\n",
		udc->max_cmd_num, udc->max_ep0_cmd_num);
	size -= t;
	next += t;
*/
	*eof = 1;
	return count - size;
}

#define create_debugfs_files() 	create_proc_read_entry(proc_node_name, 0, NULL, ambarella_debugfs_udc_read, udc)
#define remove_debugfs_files() 	remove_proc_entry(proc_node_name, NULL)

#else	/* !CONFIG_USB_GADGET_DEBUG_FILES */

#define create_debugfs_files() do {} while (0)
#define remove_debugfs_files() do {} while (0)

#endif	/* CONFIG_USB_GADGET_DEBUG_FILES */

/**************  PROC FILESYSTEM  END*****************/

static void ambarella_regaddr_map(struct ambarella_udc *udc)
{
	u32 i;

	for(i = 0; i < EP_NUM_MAX; i++) {
		struct ambarella_ep *ep;
		ep = &udc->ep[i];
		if(ep->dir == USB_DIR_IN){
			ep->ep_reg.ctrl_reg = USB_EP_IN_CTRL_REG(ep->id);
			ep->ep_reg.sts_reg = USB_EP_IN_STS_REG(ep->id);
			ep->ep_reg.buf_sz_reg = USB_EP_IN_BUF_SZ_REG(ep->id);
			ep->ep_reg.max_pkt_sz_reg =
				USB_EP_IN_MAX_PKT_SZ_REG(ep->id);
			ep->ep_reg.dat_desc_ptr_reg =
				USB_EP_IN_DAT_DESC_PTR_REG(ep->id);
		} else {
			ep->ep_reg.ctrl_reg = USB_EP_OUT_CTRL_REG(ep->id - 16);
			ep->ep_reg.sts_reg = USB_EP_OUT_STS_REG(ep->id - 16);
			ep->ep_reg.buf_sz_reg =
				USB_EP_OUT_PKT_FRM_NUM_REG(ep->id - 16);
			ep->ep_reg.max_pkt_sz_reg =
				USB_EP_OUT_MAX_PKT_SZ_REG(ep->id - 16);
			ep->ep_reg.dat_desc_ptr_reg =
				USB_EP_OUT_DAT_DESC_PTR_REG(ep->id - 16);
		}

		ep->ep_reg.setup_buf_ptr_reg = (i == CTRL_OUT) ?
			USB_EP_OUT_SETUP_BUF_PTR_REG(ep->id - 16) : 0;
	}
}


static void ambarella_disable_all_intr(struct ambarella_udc *udc)
{
	/* device interrupt mask register */
	writel(0x0000007f, udc->base_reg + USB_DEV_INTR_MSK_REG);
	writel(0xffffffff, udc->base_reg + USB_DEV_EP_INTR_MSK_REG);
	writel(0x0000007f, udc->base_reg + USB_DEV_INTR_REG);
	writel(0xffffffff, udc->base_reg + USB_DEV_EP_INTR_REG);
}

static void ambarella_ep_fifo_flush(struct ambarella_ep *ep)
{
	struct ambarella_udc *udc = ep->udc;
	if(ep->dir == USB_DIR_IN)  /* Empty Tx FIFO */
		setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_FLUSH);
	else { 			  /* Empty RX FIFO */
		if (!(readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_RXFIFO_EMPTY_STS)) {
			int retry_count = 1000;

			setbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_NAK);
			setbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_FLUSH_RXFIFO);
			while(!(readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_RXFIFO_EMPTY_STS)) {
				if (retry_count-- < 0) {
					printk (KERN_ERR "%s: failed", __func__);
					break;
				}
				udelay(5);
			}
			clrbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_NAK);
		}
	}
}


/*
 * Name: ambarella_flush_fifo
 * Description:
 *	 Empty Tx/Rx FIFO of DMA
 */
static void ambarella_udc_fifo_flush(struct ambarella_udc *udc)
{
	struct ambarella_ep *ep;
	u32 ep_id;

	for (ep_id = 0; ep_id < EP_NUM_MAX; ep_id++) {
		ep = &udc->ep[ep_id];
		if(ep->ep.desc == NULL && !IS_EP0(ep))
			continue;
		ambarella_ep_fifo_flush(ep);
	}
}

static void ambarella_udc_reset(struct ambarella_udc *udc, struct device_node *np)
{
	if (udc->rct_reg) {
		regmap_update_bits(udc->rct_reg, UDC_SOFT_RESET_OFFSET, 0x2, 0x2);
		msleep(1);
		regmap_update_bits(udc->rct_reg, UDC_SOFT_RESET_OFFSET, 0x2, 0x0);
	}
};

static void ambarella_init_usb(struct ambarella_udc *udc)
{
	u32 value;

	/* disconnect to host */
	setbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_SOFT_DISCON);
	/* disable all interrupts */
	ambarella_disable_all_intr(udc);
	/* disable Tx and Rx DMA */
	clrbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_RCV_DMA_EN | USB_DEV_TRN_DMA_EN);
	/* flush dma fifo, may used in AMboot */
	ambarella_udc_fifo_flush(udc);

	/* device config register */
	value = USB_DEV_SPD_HI |
		USB_DEV_SELF_POWER |
		USB_DEV_PHY_8BIT |
		USB_DEV_UTMI_DIR_BI |
		USB_DEV_HALT_ACK |
		USB_DEV_SET_DESC_STALL |
		USB_DEV_DDR |
		USB_DEV_CSR_PRG_EN |
		USB_DEV_REMOTE_WAKEUP_EN;

	setbitsl(udc->base_reg + USB_DEV_CFG_REG, value);

	/* device control register */
	value = USB_DEV_DESC_UPD_PYL |
		USB_DEV_LITTLE_ENDN |
		USB_DEV_DMA_MD |
		USB_DEV_BURST_EN | (0xf << 16); /* enable maximum 16 bursts */

	setbitsl(udc->base_reg + USB_DEV_CTRL_REG, value);

	// udelay(200); // FIXME: how long to wait is the best?
}


/*
 * Name: init_setup_descriptor
 * Description:
 *  Config the setup packet to specific endpoint register
 */
static void init_setup_descriptor(struct ambarella_udc *udc)
{
	struct ambarella_ep *ep = &udc->ep[CTRL_OUT];

	udc->setup_buf->status 	= USB_DMA_BUF_HOST_RDY;
	udc->setup_buf->reserved = 0xffffffff;
	udc->setup_buf->data0	= 0xffffffff;
	udc->setup_buf->data1	= 0xffffffff;

	writel(udc->setup_addr, udc->base_reg + ep->ep_reg.setup_buf_ptr_reg);
}


static int ambarella_ep_dummy_init(struct ambarella_udc *udc)
{
	int i;
	struct ambarella_ep *ep;

	for (i = 0; i < EP_NUM_MAX; i++) {
		ep = &udc->ep[i];
		ep->dummy_desc = dma_pool_alloc(udc->desc_dma_pool, GFP_KERNEL,
				&ep->dummy_desc_addr);
		if(ep->dummy_desc == NULL){
			printk(KERN_ERR "No memory to dummy DMA\n");
			return -ENOMEM;
		}

		ep->dummy_desc->data_ptr = ep->dummy_desc_addr;
		ep->dummy_desc->reserved = 0xffffffff;
		ep->dummy_desc->next_desc_ptr = ep->dummy_desc_addr;
		ep->dummy_desc->status = USB_DMA_BUF_HOST_RDY | USB_DMA_LAST;
	}

	return 0;
}

static void ambarella_ep_dummy_exit(struct ambarella_udc *udc)
{
	int i;
	struct ambarella_ep *ep;

	for (i = 0; i < EP_NUM_MAX; i++) {
		ep = &udc->ep[i];
		dma_pool_free(udc->desc_dma_pool, ep->dummy_desc, ep->dummy_desc_addr);
	}
}

static void ambarella_ep_dummy_patch(struct ambarella_ep *ep, struct ambarella_data_desc *desc)
{
	if (desc->next_desc_ptr)
		return;

	if (ep->ep.desc && !IS_ISO_IN_EP(ep))
		return;

	ep->dummy_desc->status = USB_DMA_BUF_HOST_RDY | USB_DMA_LAST;
	desc->next_desc_ptr = ep->dummy_desc_addr;
}

static void init_ep0(struct ambarella_udc *udc)
{
	struct ambarella_ep_reg *ep_reg;

	ep_reg = &udc->ep[CTRL_IN].ep_reg;
	writel(USB_EP_TYPE_CTRL, udc->base_reg + ep_reg->ctrl_reg);
	writel(USB_TXFIFO_DEPTH_CTRLIN, udc->base_reg + ep_reg->buf_sz_reg);
	writel(USB_EP_CTRL_MAX_PKT_SZ, udc->base_reg + ep_reg->max_pkt_sz_reg);
	udc->ep[CTRL_IN].ctrl_sts_phase = 0;

	ep_reg = &udc->ep[CTRL_OUT].ep_reg;
	writel(USB_EP_TYPE_CTRL, udc->base_reg + ep_reg->ctrl_reg);
	writel(USB_EP_CTRL_MAX_PKT_SZ, udc->base_reg + ep_reg->max_pkt_sz_reg);
	init_setup_descriptor(udc);
	udc->ep[CTRL_OUT].ctrl_sts_phase = 0;

	/* This should be set after gadget->bind */
	udc->ep[CTRL_OUT].ep.driver_data = udc->ep[CTRL_IN].ep.driver_data;

	/* FIXME: For A5S, this bit must be set,
	  * or USB_UDC_REG can't be read or write */
	setbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_REMOTE_WAKEUP);

	/* setup ep0 CSR. Note: ep0-in and ep0-out share the same CSR reg */
	clrbitsl(udc->base_reg + USB_UDC_REG(CTRL_IN), 0x7ff << 19);
	setbitsl(udc->base_reg + USB_UDC_REG(CTRL_IN), USB_EP_CTRL_MAX_PKT_SZ << 19);
	/* the direction bit should not take effect for ep0 */
	setbitsl(udc->base_reg + USB_UDC_REG(CTRL_IN), 0x1 << 4);
}

static int ambarella_map_dma_buffer(struct ambarella_ep *ep,
		struct ambarella_request *req)
{
	struct ambarella_udc *udc = ep->udc;
	enum dma_data_direction dmadir;
	int is_in, ret = 0;

	dmadir = (ep->dir & USB_DIR_IN) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	if (likely(!req->use_aux_buf)) {
		is_in = (ep->dir & USB_DIR_IN) ? 1 : 0;
		ret = usb_gadget_map_request(&udc->gadget, &req->req, is_in);
	} else {
		if (req->dma_aux == DMA_ADDR_INVALID) {
			req->dma_aux = dma_map_single(udc->dev,
				req->buf_aux, req->req.length, dmadir);
			/* dma address isn't 8-byte align */
			if(req->dma_aux & 0x7){
				printk("dma address isn't 8-byte align\n");
				BUG();
			}
			req->mapped = 1;
		} else {
			dma_sync_single_for_device(udc->dev,
				req->dma_aux, req->req.length, dmadir);
			req->mapped = 0;
		}
	}

	return ret;
}


static int ambarella_unmap_dma_buffer(struct ambarella_ep *ep,
		struct ambarella_request *req)
{
	struct ambarella_udc *udc = ep->udc;
	enum dma_data_direction dmadir;
	int is_in;

	dmadir = (ep->dir & USB_DIR_IN) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;

	if (likely(!req->use_aux_buf)) {
		is_in = (ep->dir & USB_DIR_IN) ? 1 : 0;
		usb_gadget_unmap_request(&udc->gadget, &req->req, is_in);
	} else {
		if (req->mapped) {
			dma_unmap_single(udc->dev,
				req->dma_aux, req->req.length, dmadir);
			req->dma_aux = DMA_ADDR_INVALID;
			req->mapped = 0;
		} else {
			dma_sync_single_for_cpu(udc->dev,
				req->dma_aux, req->req.length, dmadir);
		}
	}

	return 0;
}

static struct ambarella_data_desc *ambarella_get_last_desc(struct ambarella_ep *ep)
{
	struct ambarella_data_desc *desc = ep->data_desc;
	int retry_count = 1000;

	while(desc && (desc->status & USB_DMA_LAST) == 0) {
		if (retry_count-- < 0) {
			printk(KERN_ERR "Can't find the last descriptor\n");
			break;
		}

		if (desc->last_aux == 1)
			break;

		desc = desc->next_desc_virt;
	};

	return desc;
}

static u32 ambarella_check_bna_error (struct ambarella_ep *ep, u32 ep_status)
{
	u32 retval = 0;
	struct ambarella_udc *udc = ep->udc;

	/* Error: Buffer Not Available */
	if (ep_status & USB_EP_BUF_NOT_AVAIL) {
		printk(KERN_DEBUG "[USB]:BNA error in %s\n", ep->ep.name);

		/* recover the system if BNA error occured. */
		ep->dummy_desc->status = USB_DMA_BUF_HOST_RDY | USB_DMA_LAST;
		writel(USB_EP_BUF_NOT_AVAIL, udc->base_reg + ep->ep_reg.sts_reg);
		retval = 1;
	}

	return retval;
}

static u32 ambarella_check_he_error (struct ambarella_ep *ep, u32 ep_status)
{
	u32 retval = 0;
	struct ambarella_udc *udc = ep->udc;

	/* Error: Host Error */
	if (ep_status & USB_EP_HOST_ERR) {
		printk(KERN_ERR "[USB]:HE error in %s\n", ep->ep.name);
		writel(USB_EP_HOST_ERR, udc->base_reg + ep->ep_reg.sts_reg);
		retval = 1;
	}

	return retval;
}

static u32 ambarella_check_dma_error (struct ambarella_ep *ep)
{
	u32	retval = 0, sts_tmp1, sts_tmp2;

	if(ep->last_data_desc){
		sts_tmp1 = ep->last_data_desc->status & USB_DMA_BUF_STS;
		sts_tmp2 = ep->last_data_desc->status & USB_DMA_RXTX_STS;
		if ((sts_tmp1 != USB_DMA_BUF_DMA_DONE) || (sts_tmp2 != USB_DMA_RXTX_SUCC)){
			printk(KERN_DEBUG "%s: DMA failed\n", ep->ep.name);
			retval = 1;
		}
	}


	return retval;
}

static int ambarella_check_dma_desc(struct ambarella_ep *ep,
				    struct ambarella_request *req)
{
	struct ambarella_udc *udc = ep->udc;

	/* only check USB_DIR_IN */
	if (ep->dir != USB_DIR_IN)
		return 0;

	/* do nothing while transfer DMA is disabled */
	if (!(readl(udc->base_reg + USB_DEV_CTRL_REG) & USB_DEV_TRN_DMA_EN))
		return 0;

	/* only ckeck the DMA descriptor which is submitted to current EP */
	if (req->data_desc_addr != readl(udc->base_reg + ep->ep_reg.dat_desc_ptr_reg))
		return 0;

	return 1;
}

static void ambarella_terminate_dma_desc(struct ambarella_ep *ep,
					 struct ambarella_data_desc *desc)
{
	int retry = UDC_DMA_RETRY_MAX;
	struct ambarella_udc *udc = ep->udc;
	int dma_terminate_status = USB_DMA_BUF_DMA_DONE | USB_DMA_BUF_DMA_BUSY;

	for (;;) {

		if (desc->status & dma_terminate_status)
			break;

		/* Poll DMA and flush TXFIFO */
		setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg,
			 USB_EP_POLL_DEMAND | USB_EP_FLUSH);
		mdelay(1);

		if (!--retry) {
			pr_warn("ep%u: Terminate DMA descriptor %x timeout\n",
				ep->id, desc->status);
			break;
		}
	}
}

static void ambarella_free_descriptor(struct ambarella_ep *ep,
				      struct ambarella_request *req)
{
	struct ambarella_data_desc *data_desc, *next_data_desc;
	struct dma_pool *desc_dma_pool = ep->udc->desc_dma_pool;
	struct ambarella_udc *udc = ep->udc;
	int i, dma_desc_terminate;

	dma_desc_terminate = ambarella_check_dma_desc(ep, req);

	next_data_desc = req->data_desc;
	for(i = 0; i < req->desc_count; i++){
		data_desc = next_data_desc;

		if (dma_desc_terminate)
			ambarella_terminate_dma_desc(ep, data_desc);

		data_desc->status = USB_DMA_BUF_HOST_BUSY | USB_DMA_LAST;
		data_desc->data_ptr = 0xffffffff;
		data_desc->next_desc_ptr = 0;
		data_desc->last_aux = 1;
		next_data_desc = data_desc->next_desc_virt;
		ambarella_ep_dummy_patch(ep, data_desc);
		dma_pool_free(desc_dma_pool, data_desc, data_desc->cur_desc_addr);
	}

	if (dma_desc_terminate)
		setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_FLUSH);

	req->desc_count = 0;
	req->data_desc = NULL;
	req->data_desc_addr = 0;
}

static int ambarella_reuse_descriptor(struct ambarella_ep *ep,
	struct ambarella_request *req, u32 desc_count, dma_addr_t start_address)
{
	struct ambarella_data_desc *data_desc, *next_data_desc;
	u32 data_transmit, rest_bytes, i;
	dma_addr_t buf_dma_address;

	next_data_desc = req->data_desc;
	for(i = 0; i < desc_count; i++){
		rest_bytes = req->req.length - i * ep->ep.maxpacket;
		if(ep->dir == USB_DIR_IN)
			data_transmit = rest_bytes < ep->ep.maxpacket ?
				rest_bytes : ep->ep.maxpacket;
		else
			data_transmit = 0;

		data_desc = next_data_desc;
		data_desc->status = USB_DMA_BUF_HOST_RDY | data_transmit;
		data_desc->reserved = 0xffffffff;
		buf_dma_address = start_address + i * ep->ep.maxpacket;
		data_desc->data_ptr = buf_dma_address;
		data_desc->last_aux = 0;

		next_data_desc = data_desc->next_desc_virt;
	}

	/* Patch last one. */
	data_desc->status |= USB_DMA_LAST;
	data_desc->last_aux = 1;

	return 0;
}

static int ambarella_prepare_descriptor(struct ambarella_ep *ep,
	struct ambarella_request *req, gfp_t gfp)
{
	struct ambarella_udc *udc = ep->udc;
	struct ambarella_data_desc *data_desc = NULL;
	struct ambarella_data_desc *prev_data_desc = NULL;
	dma_addr_t desc_phys, start_address, buf_dma_address;
	u32 desc_count, data_transmit, rest_bytes, i;

	if (req->req.length > UDC_DMA_MAXPACKET) {
		/* Max support length 64KB */
		pr_debug("Invalid request length (0x%08x)\n", req->req.length);
		return -EINVAL;
	}

	if (likely(!req->use_aux_buf))
		start_address = req->req.dma;
	else
		start_address = req->dma_aux;

	desc_count = (req->req.length + ep->ep.maxpacket - 1) / ep->ep.maxpacket;
	if(req->req.zero && (req->req.length % ep->ep.maxpacket == 0))
		desc_count++;
	if(desc_count == 0)
		desc_count = 1;

	req->active_desc_count = desc_count;

	if (desc_count <= req->desc_count) {
		ambarella_reuse_descriptor(ep, req, desc_count, start_address);
		return 0;
	}

	if(req->desc_count > 0)
		ambarella_free_descriptor(ep, req);

	req->desc_count = desc_count;

	for(i = 0; i < desc_count; i++){
		rest_bytes = req->req.length - i * ep->ep.maxpacket;
		if(ep->dir == USB_DIR_IN)
			data_transmit = rest_bytes < ep->ep.maxpacket ?
				rest_bytes : ep->ep.maxpacket;
		else
			data_transmit = 0;

		data_desc = dma_pool_alloc(udc->desc_dma_pool, gfp, &desc_phys);
		if (!data_desc) {
			req->desc_count = i;
			if(req->desc_count > 0)
				ambarella_free_descriptor(ep, req);
			return -ENOMEM;
		}

		data_desc->status = USB_DMA_BUF_HOST_RDY | data_transmit;
		data_desc->reserved = 0xffffffff;
		buf_dma_address = start_address + i * ep->ep.maxpacket;
		data_desc->data_ptr = buf_dma_address;
		data_desc->next_desc_ptr = 0;
		data_desc->rsvd1 = 0xffffffff;
		data_desc->last_aux = 0;
		data_desc->cur_desc_addr = desc_phys;

		ambarella_ep_dummy_patch(ep, data_desc);

		if(prev_data_desc){
			prev_data_desc->next_desc_ptr = desc_phys;
			prev_data_desc->next_desc_virt = data_desc;
		}

		prev_data_desc = data_desc;

		if(i == 0){
			req->data_desc = data_desc;
			req->data_desc_addr = desc_phys;
		}
	}

	/* Patch last one */
	data_desc->status |= USB_DMA_LAST;
	data_desc->next_desc_ptr = 0;
	ambarella_ep_dummy_patch(ep, data_desc);
	data_desc->next_desc_virt = NULL;
	data_desc->last_aux = 1;

	return 0;
}

static void ambarella_clr_ep_nak(struct ambarella_ep *ep)
{
	struct ambarella_ep_reg *ep_reg = &ep->ep_reg;
	struct ambarella_udc *udc = ep->udc;

	setbitsl(udc->base_reg + ep_reg->ctrl_reg, USB_EP_CLR_NAK);
	if (readl(udc->base_reg + ep_reg->ctrl_reg) & USB_EP_NAK_STS) {
		/* can't clear NAK, let somebody clear it after Rx DMA is done. */
		ep->need_cnak = 1;
	}else{
		ep->need_cnak = 0;
	}
}

static void ambarella_set_ep_nak(struct ambarella_ep *ep)
{
	struct ambarella_udc *udc = ep->udc;
	setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_SET_NAK);
}

static void ambarella_enable_rx_dma(struct ambarella_ep *ep)
{
	struct ambarella_udc *udc = ep->udc;
	ep->dma_going = 1;
	setbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_RCV_DMA_EN);
}

static void ambarella_patch_iso_desc(struct ambarella_udc *udc,
	struct ambarella_request * req, struct ambarella_ep *ep, int frame_fix)
{
	struct ambarella_data_desc *data_desc;
	u32 current_frame, max_packet_num, i, j;

	/* If it's requested by the IN token,
	 * we shoud get the real frame number offset from the udc.
	 * If it's requested by the TX completion,
	 * we should calculate the frame number offset directly.
	 * The wrong and discontinuous frame number will
	 * cause the isoc transmission to fail.
	 */
	current_frame = ambarella_udc_get_frame(&udc->gadget);
	if (likely(frame_fix)) {
		if (unlikely((ep->frame_offset == 0) &&
			((current_frame < ep->frame_interval) || (current_frame > (0x800 - ep->frame_interval))))) {
			frame_fix = ep->frame_interval;
		} else {
			frame_fix = (ep->frame_offset + ep->frame_interval) & 0x7ff;
			if (unlikely((frame_fix <= current_frame) &&
				!((frame_fix == 0) && (current_frame > (0x800 - 2 * ep->frame_interval))))) {
				frame_fix = (current_frame - current_frame % ep->frame_interval + ep->frame_interval) & 0x7ff;
				printk(KERN_DEBUG "isoc is delayed: cur=0x%x, offset=0x%x\n", current_frame, ep->frame_offset);
			}
		}
	} else {
		frame_fix = (current_frame + ep->frame_interval) & 0x7ff;
	}
	ep->frame_offset = frame_fix;

	/* according to USB2.0 spec, each microframe can send 3 packets at most */
	data_desc = req->data_desc;
	for (i = 0; i < req->active_desc_count; i += j) {
		if ((req->active_desc_count - i) >= ISO_MAX_PACKET)
			max_packet_num = ISO_MAX_PACKET;
		else
			max_packet_num = req->active_desc_count - i;

		for (j = 0; j < max_packet_num; j++) {
			data_desc->status |= (frame_fix << 16);
			data_desc->status |= (max_packet_num << 14);
			data_desc->status &= ~(0xf0000000);
			data_desc->status |= USB_DMA_BUF_HOST_RDY | USB_DMA_LAST;
			data_desc = data_desc->next_desc_virt;
		}
	}
}

static void ambarella_set_tx_dma(struct ambarella_ep *ep,
	struct ambarella_request * req, int frame_fix)
{
	struct ambarella_udc *udc = ep->udc;
	struct ambarella_ep_reg *ep_reg = &ep->ep_reg;

	/* Sometimes, DMA completion and IN_PKT INT come at the same time.
	   They both will trigger next TX DMA. If one has triggered, the other
	   should skip doing so. Because it is not permitted to trigger DMA again
	   when it is on-going, which might cause potential H/W issue.
	   So we'll do nothing if DMA is already on-going;
	*/
	if (ep->dma_going) {
		/* pr_debug("%s: tx dma is on-going, skip triggering again.\n", ep->ep.name); */
		return;
	}

	if (IS_ISO_IN_EP(ep))
		ambarella_patch_iso_desc(udc, req, ep, frame_fix);

	ep->data_desc = req->data_desc;
	writel(req->data_desc_addr, udc->base_reg + ep_reg->dat_desc_ptr_reg);
	/* set Poll command to transfer data to Tx FIFO */
	setbitsl(udc->base_reg + ep_reg->ctrl_reg, USB_EP_POLL_DEMAND);
	ep->dma_going = 1;
}


static void ambarella_set_rx_dma(struct ambarella_ep *ep,
	struct ambarella_request * req)
{
	struct ambarella_ep_reg *ep_reg = &ep->ep_reg;
	struct ambarella_udc *udc = ep->udc;
	u32 i;

	if(req){
		ep->data_desc = req->data_desc;
		writel(req->data_desc_addr, udc->base_reg + ep_reg->dat_desc_ptr_reg);
	} else {
		/* receive zero-length-packet */
		ep->dummy_desc->status = USB_DMA_BUF_HOST_RDY | USB_DMA_LAST;
		writel(ep->dummy_desc_addr, udc->base_reg + ep_reg->dat_desc_ptr_reg);
	}

	/* enable dma completion interrupt for next RX data */
	clrbitsl(udc->base_reg + USB_DEV_EP_INTR_MSK_REG, 1 << ep->id);
	/* re-enable DMA read */
	ambarella_enable_rx_dma(ep);

	if (readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_RXFIFO_EMPTY_STS) {
		/* clear NAK for TX dma */
		for (i = 0; i < EP_NUM_MAX; i++) {
			struct ambarella_ep *_ep = &udc->ep[i];
			if (_ep->need_cnak == 1)
				ambarella_clr_ep_nak(_ep);
		}
	}

	/* clear NAK */
	ambarella_clr_ep_nak(ep);
}

static int ambarella_handle_ep_stall(struct ambarella_ep *ep, u32 ep_status)
{
	int ret = 0;
	struct ambarella_udc *udc = ep->udc;

	if (ep_status & USB_EP_RCV_CLR_STALL) {
		setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_CLR_NAK);
		clrbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_STALL);
		setbitsl(udc->base_reg + ep->ep_reg.sts_reg, USB_EP_RCV_CLR_STALL);
		ret = 1;
	}

	if (ep_status & USB_EP_RCV_SET_STALL) {
		setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_STALL);
		if (ep->dir == USB_DIR_IN)
			setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_FLUSH);
		setbitsl(udc->base_reg + ep->ep_reg.sts_reg, USB_EP_RCV_SET_STALL);
		ret = 1;
	}

	return ret;
}

static void ambarella_handle_request_packet(struct ambarella_udc *udc)
{
	struct usb_ctrlrequest *crq;
	int ret;

	ambarella_ep_nuke(&udc->ep[CTRL_OUT], -EPROTO);  // Todo

	/* read out setup packet */
	udc->setup[0] = udc->setup_buf->data0;
	udc->setup[1] = udc->setup_buf->data1;
	/* reinitialize setup packet */
	init_setup_descriptor(udc);

	crq = (struct usb_ctrlrequest *) &udc->setup[0];

	dprintk(DEBUG_NORMAL, "bRequestType = 0x%02x, bRequest = 0x%02x, "
		"wValue = 0x%04x, wIndex = 0x%04x, wLength = 0x%04x\n",
		crq->bRequestType, crq->bRequest, crq->wValue, crq->wIndex,
		crq->wLength);

	if((crq->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD){
		switch(crq->bRequest)
		{
		case USB_REQ_GET_STATUS:
		case USB_REQ_SET_ADDRESS:
		case USB_REQ_CLEAR_FEATURE:
		case USB_REQ_SET_FEATURE:
			pr_err("%s: This bRequest is not implemented!\n"
				"\tbRequestType = 0x%02x, bRequest = 0x%02x,\n"
				"\twValue = 0x%04x, wIndex = 0x%04x, wLength = 0x%04x\n",
				__func__,
				crq->bRequestType, crq->bRequest, crq->wValue, crq->wIndex,
				crq->wLength);
		}
	}

	if (crq->bRequestType & USB_DIR_IN)
		udc->gadget.ep0 = &udc->ep[CTRL_IN].ep;

	else
		udc->gadget.ep0 = &udc->ep[CTRL_OUT].ep;

	spin_unlock(&udc->lock);
	ret = udc->driver->setup(&udc->gadget, crq);
	spin_lock(&udc->lock);
	if (ret < 0) {
		pr_err("%s: SETUP request failed (%d)\n", __func__, ret);
		setbitsl(udc->base_reg + udc->ep[CTRL_IN].ep_reg.ctrl_reg, USB_EP_STALL | USB_EP_FLUSH);
		/* Re-enable Rx DMA to receive next setup packet */
		ambarella_enable_rx_dma(&udc->ep[CTRL_OUT]);
		ambarella_clr_ep_nak(&udc->ep[CTRL_OUT]);
	}

	return;

}

static void ambarella_handle_data_in(struct ambarella_ep *ep)
{
	struct ambarella_request	*req = NULL;
	struct ambarella_udc *udc = ep->udc;

	/* get request */
	if (list_empty(&ep->queue)) {
		printk(KERN_DEBUG "%s: req NULL\n", __func__);
		return ;
	}

	req = list_first_entry(&ep->queue, struct ambarella_request,queue);

	/* If error happened, issue the request again */
	if (ambarella_check_dma_error(ep))
		req->req.status = -EPROTO;
	else {
		/* No error happened, so all the data has been sent to host */
		req->req.actual = req->req.length;
	}

	ambarella_udc_done(ep, req, 0);

	if(ep->id == CTRL_IN){
		/* For STATUS-OUT stage */
		udc->ep[CTRL_OUT].ctrl_sts_phase = 1;
		ambarella_set_rx_dma(&udc->ep[CTRL_OUT], NULL);
	}
}

static int ambarella_handle_data_out(struct ambarella_ep *ep)
{
	struct ambarella_request	*req = NULL;
	struct ambarella_udc *udc = ep->udc;
	u32 recv_size = 0, req_size;

	/* get request */
	if (list_empty(&ep->queue)) {
		printk(KERN_DEBUG "%s: req NULL\n", __func__);
		return -EINVAL;
	}

	req = list_first_entry(&ep->queue, struct ambarella_request,queue);

	/* If error happened, issue the request again */
	if (ambarella_check_dma_error(ep))
		req->req.status = -EPROTO;

	recv_size = ep->last_data_desc->status & USB_DMA_RXTX_BYTES;
	if (!recv_size && req->req.length == UDC_DMA_MAXPACKET) {
		/* on 64k packets the RXBYTES field is zero */
		recv_size = UDC_DMA_MAXPACKET;
	}

	req_size = req->req.length - req->req.actual;
	if (recv_size > req_size) {
		if ((req_size % ep->ep.maxpacket) != 0)
			req->req.status = -EOVERFLOW;
		recv_size = req_size;
	}

	req->req.actual += recv_size;

	ambarella_udc_done(ep, req, 0);

	if(ep->id == CTRL_OUT) {
		/* For STATUS-IN stage */
		ambarella_clr_ep_nak(&udc->ep[CTRL_IN]);
		/* Re-enable Rx DMA to receive next setup packet */
		ambarella_enable_rx_dma(ep);
		ep->dma_going = 0;
	}

	return 0;
}


static void ambarella_udc_done(struct ambarella_ep *ep,
		struct ambarella_request *req, int status)
{
	unsigned halted_tmp, need_queue = 0;
	struct ambarella_request *next_req;
	struct ambarella_udc *udc = ep->udc;

	halted_tmp = ep->halted;

	list_del_init(&req->queue);

	if(!list_empty(&ep->queue) && !ep->halted && !ep->cancel_transfer){
		need_queue = 1;
	} else if (!IS_EP0(ep) && (ep->dir == USB_DIR_IN) && !ep->cancel_transfer) {
		/* ep->ep.desc = NULL when ep disabled */
		if (!ep->ep.desc || IS_ISO_IN_EP(ep))
			ambarella_set_ep_nak(ep);
		setbitsl(udc->base_reg + USB_DEV_EP_INTR_MSK_REG, 1 << ep->id);
	}

	if (likely (req->req.status == -EINPROGRESS))
		req->req.status = status;
	else
		status = req->req.status;

	ambarella_unmap_dma_buffer(ep, req);

	if (req->use_aux_buf && ep->dir != USB_DIR_IN)
		memcpy(req->req.buf, req->buf_aux, req->req.actual);

	ep->data_desc = NULL;
	ep->last_data_desc = NULL;

	ep->halted = 1;
	spin_unlock(&ep->udc->lock);
	req->req.complete(&ep->ep, &req->req);
	spin_lock(&ep->udc->lock);
	ep->halted = halted_tmp;

	if(need_queue){
		next_req = list_first_entry (&ep->queue,
			 struct ambarella_request, queue);

		switch(ep->dir) {
		case USB_DIR_IN:
			/* no need to wait for IN-token for ISO transfer */
			if (IS_ISO_IN_EP(ep)) {
				writel(USB_EP_IN_PKT, udc->base_reg + ep->ep_reg.sts_reg);
				ambarella_set_tx_dma(ep, next_req, 1);
			}
			ambarella_clr_ep_nak(ep);
			break;
		case USB_DIR_OUT:
			ambarella_set_rx_dma(ep, next_req);
			break;
		default:
			return;
		}
	}
}

static void ambarella_ep_nuke(struct ambarella_ep *ep, int status)
{
	while (!list_empty (&ep->queue)) {
		struct ambarella_request *req;
		req = list_first_entry(&ep->queue,
			struct ambarella_request, queue);
		ambarella_udc_done(ep, req, status);
	}
}

/*
 * Name: device_interrupt
 * Description:
 *	Process related device interrupt
 */
static void udc_device_interrupt(struct ambarella_udc *udc, u32 int_value)
{
	u32 regval, connect;

	/* case 1. Get set_config or set_interface request from host */
	if (int_value & (USB_DEV_SET_CFG | USB_DEV_SET_INTF)) {
		struct usb_ctrlrequest crq;
		u32 i, csr_config;
		int ret;

		if(int_value & USB_DEV_SET_CFG) {
			/* Ack the SC interrupt */
			writel(USB_DEV_SET_CFG, udc->base_reg + USB_DEV_INTR_REG);
			udc->cur_config = (u16)(readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_CFG_NUM);

			crq.bRequestType = 0x00;
			crq.bRequest = USB_REQ_SET_CONFIGURATION;
			crq.wValue = cpu_to_le16(udc->cur_config);
			crq.wIndex = 0x0000;
			crq.wLength = 0x0000;
		} else if(int_value & USB_DEV_SET_INTF){
			/* Ack the SI interrupt */
			writel(USB_DEV_SET_INTF, udc->base_reg + USB_DEV_INTR_REG);
			udc->cur_intf = (readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_INTF_NUM) >> 4;
			udc->cur_alt = (readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_ALT_SET) >> 8;

			crq.bRequestType = 0x01;
			crq.bRequest = USB_REQ_SET_INTERFACE;
			crq.wValue = cpu_to_le16(udc->cur_alt);
			crq.wIndex = cpu_to_le16(udc->cur_intf);
			crq.wLength = 0x0000;
		}

		for (i = 0; i < EP_NUM_MAX; i++){
			udc->ep[i].halted = 0;
			clrbitsl(udc->base_reg + udc->ep[i].ep_reg.ctrl_reg, USB_EP_STALL);
		}

		/* setup ep0 CSR. Note: ep0-in and ep0-out share the same CSR reg */
		csr_config = (udc->cur_config << 7) | (udc->cur_intf << 11) |
			(udc->cur_alt << 15);
		clrbitsl(udc->base_reg + USB_UDC_REG(CTRL_IN), 0xfff << 7);
		setbitsl(udc->base_reg + USB_UDC_REG(CTRL_IN), csr_config);

		udc->auto_ack_0_pkt = 1;
		ambarella_ep_nuke(&udc->ep[CTRL_OUT], -EPROTO);
		spin_unlock(&udc->lock);
		ret = udc->driver->setup(&udc->gadget, &crq);
		spin_lock(&udc->lock);
		if(ret < 0)
			printk(KERN_ERR "set config failed. (%d)\n", ret);

		udc->cur_intf = 0;
		udc->cur_alt = 0;

		/* told UDC the configuration is done, and to ack HOST
		 * UDC has to ack the host quickly, or Host will think failed,
		 * do don't add much debug message when receive SC/SI irq.*/
		setbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_CSR_DONE);
		udelay(150);
		usb_gadget_set_state(&udc->gadget, USB_STATE_CONFIGURED);
		schedule_work(&udc->uevent_work);

	}

	/* case 2. Get reset Interrupt */
	else if (int_value & USB_DEV_RESET) {

		dprintk(DEBUG_NORMAL, "USB reset IRQ\n");

		writel(USB_DEV_RESET, udc->base_reg + USB_DEV_INTR_REG);

		ambarella_disable_all_intr(udc);

		if (udc->host_suspended && udc->driver && udc->driver->resume){
			spin_unlock(&udc->lock);
			udc->driver->resume(&udc->gadget);
			spin_lock(&udc->lock);
			udc->host_suspended = 0;
		}

		ambarella_stop_activity(udc);

		udc->gadget.speed = USB_SPEED_UNKNOWN;
		udc->auto_ack_0_pkt = 0;
		udc->remote_wakeup_en = 0;

		regmap_read(udc->scr_reg, udc->status_offset, &regval);
		connect = !!(regval & udc->connect_status);

		udc->vbus_status = connect;
		udc->udc_is_enabled = 0;
		ambarella_udc_enable(udc);
		if (connect)  {
			dev_info(udc->dev, "Connected.\n");
			usb_gadget_set_state(&udc->gadget, USB_STATE_ATTACHED);
		} else {
			dev_info(udc->dev, "Disconnected.\n");
			usb_gadget_set_state(&udc->gadget, USB_STATE_NOTATTACHED);
		}
		schedule_work(&udc->uevent_work);
#if 0
		/* enable suspend interrupt */
		clrbitsl(udc->base_reg + USB_DEV_INTR_MSK_REG, USB_DEV_MSK_SUSP);
#endif
	}

	/* case 3. Get suspend Interrupt */
	else if (int_value & USB_DEV_SUSP) {

		pr_err("%s: USB suspend IRQ\n", __func__);

		writel(USB_DEV_SUSP, udc->base_reg + USB_DEV_INTR_REG);

		if (udc->driver->suspend) {
			udc->host_suspended = 1;
			spin_unlock(&udc->lock);
			udc->driver->suspend(&udc->gadget);
			spin_lock(&udc->lock);
		}
	}

	/* case 4. enumeration complete */
	else if(int_value & USB_DEV_ENUM_CMPL) {
		u32 	value = 0;

		/* Ack the CMPL interrupt */
		writel(USB_DEV_ENUM_CMPL, udc->base_reg + USB_DEV_INTR_REG);

		value = readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_ENUM_SPD;

		if(value == USB_DEV_ENUM_SPD_HI) {  /* high speed */

			dprintk(DEBUG_NORMAL,"enumeration IRQ - "
					"High-speed bus detected\n");
			udc->gadget.speed = USB_SPEED_HIGH;
		} else if (value == USB_DEV_ENUM_SPD_FU) { /* full speed */

			dprintk(DEBUG_NORMAL,"enumeration IRQ - "
					"Full-speed bus detected\n");
			udc->gadget.speed = USB_SPEED_FULL;
		} else {
			printk(KERN_ERR "Not supported speed!"
					"USB_DEV_STS_REG = 0x%x\n", value);
			udc->gadget.speed = USB_SPEED_UNKNOWN;

		}
	} /* ENUM COMPLETE */
	else {
		printk(KERN_ERR "Unknown Interrupt:0x%08x\n", int_value);
		/* Ack the Unknown interrupt */
		writel(int_value, udc->base_reg + USB_DEV_INTR_REG);
	}
}


/*
 * Name: udc_epin_interrupt
 * Description:
 *	Process IN(CTRL or BULK) endpoint interrupt
 */
static void udc_epin_interrupt(struct ambarella_udc *udc, u32 ep_id)
{
	u32 ep_status = 0;
	u32 hd_status = USB_EP_TXFIFO_EMPTY | USB_EP_TRN_DMA_CMPL | USB_EP_IN_PKT;
	struct ambarella_ep *ep = &udc->ep[ep_id];
	struct ambarella_request *req = NULL;

	ep_status = readl(udc->base_reg + ep->ep_reg.sts_reg);
	dprintk(DEBUG_NORMAL, "%s: ep_status = 0x%08x\n", ep->ep.name, ep_status);

	if (ambarella_handle_ep_stall(ep, ep_status))
		return;

	if (ambarella_check_bna_error(ep, ep_status)
		|| ambarella_check_he_error(ep, ep_status)) {
		ep->dma_going = 0;
		ep->cancel_transfer = 0;
		ep->need_cnak = 0;
		if (!list_empty(&ep->queue)) {
			req = list_first_entry(&ep->queue, struct ambarella_request,queue);
			req->req.status = -EPROTO;
			ambarella_udc_done(ep, req, 0);
		}
		return;
	}

	if (ep_status & hd_status) {
		/* TxFIFO is empty, but we've not used this bit, so just ignored simply. */
		if (ep_status & USB_EP_TXFIFO_EMPTY) {
			writel(USB_EP_TXFIFO_EMPTY, udc->base_reg + ep->ep_reg.sts_reg);
		}

		if (ep_status & USB_EP_TRN_DMA_CMPL) {
			/*
			 * USB_EP_TRN_DMA_CMPL may be re-triggered when handling the previous one,
			 * so we should clean it at the beginning.
			 */
			writel(USB_EP_TRN_DMA_CMPL, udc->base_reg + ep->ep_reg.sts_reg);

			/* write dummy desc to try to avoid BNA error */
			ep->dummy_desc->status = USB_DMA_BUF_HOST_RDY | USB_DMA_LAST;
			writel(ep->dummy_desc_addr, udc->base_reg + ep->ep_reg.dat_desc_ptr_reg);

			if (ep->halted || ep->dma_going == 0 || ep->cancel_transfer == 1
				|| list_empty(&ep->queue)) {
				ep->dma_going = 0;
				ep->cancel_transfer = 0;
			} else {
				ep->dma_going = 0;
				ep->cancel_transfer = 0;
				ep->need_cnak = 0;

				ep->last_data_desc = ambarella_get_last_desc(ep);
				if (ep->last_data_desc == NULL) {
					pr_err("%s: last_data_desc is NULL\n", ep->ep.name);
					BUG();
				} else {
					ambarella_handle_data_in(&udc->ep[ep_id]);
				}
			}

			if (IS_EP0(ep)) {
				return;
			}
		} else if (ep_status & USB_EP_IN_PKT) {
			/*
			 * When USB_EP_TRN_DMA_CMPL and USB_EP_IN_PKT come at the same time, USB_EP_IN_PKT belongs
			 * to the transaction just completed. In this case, should not send NAK to host.
			 */
			if (!ep->halted && !ep->cancel_transfer && !list_empty(&ep->queue)) {
				req = list_first_entry(&ep->queue,
					struct ambarella_request, queue);
				ambarella_set_tx_dma(ep, req, 0);
			} else if (ep->dma_going == 0 || ep->halted || ep->cancel_transfer) {
				ambarella_set_ep_nak(ep);
			}
			ep->cancel_transfer = 0;
		}

		if (ep_status & USB_EP_IN_PKT) {
			/*
			 * Cleaning USB_EP_IN_PKT at the beginning makes usb rndis not work properly,
			 * so we should clean it at the end.
			 */
			writel(USB_EP_IN_PKT, udc->base_reg + ep->ep_reg.sts_reg);
		}
	} else if (ep_status) {
		pr_debug("%s: %s, Unknown int source(0x%08x)\n", __func__,
			ep->ep.name, ep_status);
		writel(ep_status, udc->base_reg + ep->ep_reg.sts_reg);
	}
}


/*
 * Name: udc_epout_interrupt
 * Description:
 *	Process OUT endpoint interrupt
 */
static void udc_epout_interrupt(struct ambarella_udc *udc, u32 ep_id)
{
	struct ambarella_ep *ep = &udc->ep[ep_id];
	u32 desc_status, ep_status, i;

	/* check the status bits for what kind of packets in */
	ep_status = readl(udc->base_reg + ep->ep_reg.sts_reg);

	if (ambarella_handle_ep_stall(ep, ep_status))
		return;


	if(ep_id == CTRL_OUT) {
		/* Cope with setup-data packet  */
		if((ep_status & USB_EP_OUT_PKT_MSK) == USB_EP_SETUP_PKT){
			writel(USB_EP_SETUP_PKT, udc->base_reg + ep->ep_reg.sts_reg);
			ep->ctrl_sts_phase = 0;
			ep->dma_going = 0;
			ambarella_handle_request_packet(udc);
		}
	}

	/* Cope with normal data packet  */
	if((ep_status & USB_EP_OUT_PKT_MSK) == USB_EP_OUT_PKT) {

		writel(USB_EP_OUT_PKT, udc->base_reg + ep->ep_reg.sts_reg);
		ep->dma_going = 0;

		/* Just cope with the zero-length packet */
		if(ep->ctrl_sts_phase == 1) {
			ep->ctrl_sts_phase = 0;
			ambarella_enable_rx_dma(ep);
			ep->dma_going = 0;
			return;
		}

		if(ep->halted || ep->cancel_transfer || list_empty(&ep->queue)) {
			writel(ep_status, udc->base_reg + ep->ep_reg.sts_reg);
			return;
		}

		if (ambarella_check_bna_error(ep, ep_status))
			return;

		if(ambarella_check_he_error(ep, ep_status) && !list_empty(&ep->queue)) {
			struct ambarella_request *req = NULL;
			req = list_first_entry(&ep->queue,
				struct ambarella_request, queue);
			req->req.status = -EPROTO;
			ambarella_udc_done(ep, req, 0);
			return;
		}

		ep->last_data_desc = ambarella_get_last_desc(ep);
		if(ep->last_data_desc == NULL){
			pr_err("%s: %s, last_data_desc is NULL\n", __func__, ep->ep.name);
			BUG();
			return;
		}

		if(ep_id != CTRL_OUT){
			desc_status = ep->last_data_desc->status;
			/* received data */
			if((desc_status & USB_DMA_BUF_STS) == USB_DMA_BUF_DMA_DONE) {
				setbitsl(udc->base_reg + USB_DEV_EP_INTR_MSK_REG, 1 << ep_id);
				ambarella_set_ep_nak(ep);
			}
		}

		ambarella_handle_data_out(ep);

		/* clear NAK for TX dma */
		if (readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_RXFIFO_EMPTY_STS) {
			for (i = 0; i < EP_NUM_MAX; i++) {
				struct ambarella_ep *_ep = &udc->ep[i];
				if (_ep->need_cnak == 1)
					ambarella_clr_ep_nak(_ep);
			}
		}
	}

	return;
}

static irqreturn_t ambarella_udc_irq(int irq, void *_dev)
{
	struct ambarella_udc *udc = _dev;
	u32 value, handled = 0, i, ep_irq;

	spin_lock(&udc->lock);
	/* If gadget driver is not connected, do not handle the interrupt  */
	if (!udc->driver) {
		writel(readl(udc->base_reg + USB_DEV_INTR_REG),
				udc->base_reg + USB_DEV_INTR_REG);
		writel(readl(udc->base_reg + USB_DEV_EP_INTR_REG),
				udc->base_reg + USB_DEV_EP_INTR_REG);
		ambarella_udc_set_pullup(udc, 0);
	}


	/* 1. check if device interrupt */
	value = readl(udc->base_reg + USB_DEV_INTR_REG);
	if (value) {

		//printk("device int value = 0x%x\n", value);

		handled = 1;
		udc_device_interrupt(udc, value);

	}
	/* 2. check if endpoint interrupt */
	value = readl(udc->base_reg + USB_DEV_EP_INTR_REG);
	if(value) {
		handled = 1;

		for(i = 0; i < EP_NUM_MAX; i++){
			ep_irq = 1 << i;
			if (!(value & ep_irq))
				continue;

			/* ack the endpoint interrupt */
			writel(ep_irq, udc->base_reg + USB_DEV_EP_INTR_REG);

			/* irq for out ep ? */
			if (i >= EP_IN_NUM)
				udc_epout_interrupt(udc, i);
			else
				udc_epin_interrupt(udc, i);

			value &= ~ep_irq;
			if(value == 0)
				break;
		}
	}

	spin_unlock(&udc->lock);

	return IRQ_RETVAL(handled);
}

#if 0
static void ambarella_vbus_timer(struct timer_list *t)
{
	struct ambarella_udc *udc = from_timer(udc, t, vbus_timer);
	enum usb_device_state state;
	u32 raw_status, connected, suspended;

	suspended = readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_SUSP_STS;
	regmap_read(udc->scr_reg, udc->status_offset, &raw_status);
	connected = !!(raw_status & udc->connect_status);

	if (suspended)
		usb_gadget_set_state(&udc->gadget, USB_STATE_SUSPENDED);

	if (udc->vbus_status != connected) {
		state = connected ? USB_STATE_ATTACHED : USB_STATE_NOTATTACHED;
		usb_gadget_set_state(&udc->gadget, state);
		udc->vbus_status = connected;
		schedule_work(&udc->uevent_work);
		if (udc->driver != NULL)
			ambarella_udc_vbus_session(&udc->gadget, udc->vbus_status);
	}

	mod_timer(&udc->vbus_timer, jiffies + VBUS_POLL_TIMEOUT);
}
#endif

static void ambarella_stop_activity(struct ambarella_udc *udc)
{
	struct usb_gadget_driver *driver = udc->driver;
	struct ambarella_ep *ep;
	u32  i;

	/* Disable Tx and Rx DMA */
	clrbitsl(udc->base_reg + USB_DEV_CTRL_REG,
			USB_DEV_RCV_DMA_EN | USB_DEV_TRN_DMA_EN);

	if (udc->gadget.speed == USB_SPEED_UNKNOWN)
		driver = NULL;
	udc->gadget.speed = USB_SPEED_UNKNOWN;

	for (i = 0; i < EP_NUM_MAX; i++) {
		ep = &udc->ep[i];

		if(ep->ep.desc == NULL && !IS_EP0(ep))
			continue;

		ambarella_set_ep_nak(ep);

		ep->halted = 1;
		ambarella_ep_nuke(ep, -ESHUTDOWN);
	}

	tasklet_schedule(&udc->disconnect_tasklet);

	ambarella_udc_reinit(udc);
}

static int ambarella_udc_ep_enable(struct usb_ep *_ep,
				 const struct usb_endpoint_descriptor *desc)
{
	struct ambarella_udc	*udc;
	struct ambarella_ep	*ep = to_ambarella_ep(_ep);
	u32			max_packet, tmp, type, idx = 0;
	unsigned long		flags;
	int rval = 0;

	/* Sanity check  */
	if (!_ep || !desc || IS_EP0(ep)
		|| desc->bDescriptorType != USB_DT_ENDPOINT) {
		printk("%s ep %d is inval \n",__func__,ep->id);
		return -EINVAL;
	}
	udc = ep->udc;
	if (!udc->driver || udc->gadget.speed == USB_SPEED_UNKNOWN)
		return -ESHUTDOWN;

	max_packet = usb_endpoint_maxp(desc) & 0x7ff;

	spin_lock_irqsave(&udc->lock, flags);
	ep->ep.maxpacket = max_packet;
	ep->ep.desc = desc;
	ep->halted = 0;
	ep->data_desc = NULL;
	ep->last_data_desc = NULL;
	ep->ctrl_sts_phase = 0;
	ep->dma_going = 0;
	ep->cancel_transfer = 0;
	ep->frame_offset = 0;
	ep->frame_interval = desc->bInterval ? (1 << (desc->bInterval - 1)) : 0;

	if(ep->dir == USB_DIR_IN){
		idx = ep->id;
	} else {
		idx = ep->id - CTRL_OUT_UDC_IDX;
	}

	/* setup CSR */
	clrbitsl(udc->base_reg + USB_UDC_REG(idx), 0x3fffffff);
	tmp = (desc->bEndpointAddress & 0xf) << 0;
	tmp |= (desc->bEndpointAddress >> 7) << 4;
	tmp |= (desc->bmAttributes & 0x3) << 5;
	tmp |= udc->cur_config << 7;
	tmp |= udc->cur_intf << 11;
	tmp |= udc->cur_alt << 15;
	tmp |= max_packet << 19;
	setbitsl(udc->base_reg + USB_UDC_REG(idx), tmp);

	type = (desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) << 4;
	writel(type | USB_EP_SET_NAK, udc->base_reg + ep->ep_reg.ctrl_reg);

	if(ep->dir == USB_DIR_IN) {
		/* NOTE: total IN fifo size must be less than 528 * 4B */
		tmp = DIV_ROUND_UP(max_packet, 4);

		/* Double up the FIFO if isochronous mode */
		if (IS_ISO_IN_EP(ep))
			tmp *= 2;
		else if (usb_endpoint_is_bulk_in(ep->ep.desc))
			tmp *= udc->bulk_fifo_factor;

		udc->tx_fifosize -= tmp;
		if (udc->tx_fifosize < 0) {
			dev_err(udc->dev, "Tx FIFO overflow: request %d.\n", tmp);
			rval = -ENOMEM;
		} else {
			dev_dbg(udc->dev, "Tx FIFO: %s use %d, reserved %d\n",
					ep->ep.name, tmp, udc->tx_fifosize);
		}

		writel(tmp, udc->base_reg + ep->ep_reg.buf_sz_reg);

	}

	writel(max_packet, udc->base_reg + ep->ep_reg.max_pkt_sz_reg);
	spin_unlock_irqrestore(&udc->lock, flags);


	return rval;
}

static int ambarella_udc_ep_disable(struct usb_ep *_ep)
{
	struct ambarella_ep *ep = to_ambarella_ep(_ep);
	struct ambarella_udc *udc = ep->udc;
	unsigned long flags;

	if (!_ep || !ep->ep.desc) {
		dprintk(DEBUG_NORMAL, "%s not enabled\n",
			_ep ? ep->ep.name : NULL);
		return -EINVAL;
	}

	spin_lock_irqsave(&ep->udc->lock, flags);

	ep->ep.desc = NULL;
	ep->halted = 1;
	ambarella_ep_nuke(ep, -ESHUTDOWN);

	ambarella_set_ep_nak(ep);

	if(ep->dir == USB_DIR_IN){
		/* clear DMA poll demand bit */
		clrbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_POLL_DEMAND);
		/* clear status register */
		setbitsl(udc->base_reg + ep->ep_reg.sts_reg, USB_EP_IN_PKT);
		/* flush the fifo */
		setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_FLUSH);
	}

	/* disable irqs */
	setbitsl(udc->base_reg + USB_DEV_EP_INTR_MSK_REG, 1 << ep->id);

	udc->tx_fifosize += readl(udc->base_reg + ep->ep_reg.buf_sz_reg);

	spin_unlock_irqrestore(&ep->udc->lock, flags);

	return 0;
}


static struct usb_request *
ambarella_udc_alloc_request(struct usb_ep *_ep, gfp_t mem_flags)
{
	struct ambarella_request *req;

	if (!_ep)
		return NULL;

	req = kzalloc (sizeof(struct ambarella_request), mem_flags);
	if (!req)
		return NULL;

	req->req.dma = DMA_ADDR_INVALID;
	INIT_LIST_HEAD (&req->queue);
	req->desc_count = 0;

	req->buf_aux = NULL;
	req->dma_aux = DMA_ADDR_INVALID;
	req->use_aux_buf = 0;

	return &req->req;
}


static void
ambarella_udc_free_request(struct usb_ep *_ep, struct usb_request *_req)
{
	struct ambarella_ep	*ep = to_ambarella_ep(_ep);
	struct ambarella_request	*req = to_ambarella_req(_req);

	if (!ep || !_req)
		return;

	if(req->desc_count > 0)
		ambarella_free_descriptor(ep, req);

	if (req->buf_aux) {
		kfree(req->buf_aux);
		req->buf_aux = NULL;
	}

	WARN_ON (!list_empty (&req->queue));
	kfree(req);
}


static int ambarella_udc_queue(struct usb_ep *_ep, struct usb_request *_req,
		gfp_t gfp_flags)
{
	struct ambarella_request	*req = NULL;
	struct ambarella_ep	*ep = NULL;
	struct ambarella_udc	*udc;
	unsigned long flags;
	int i, ret;

	if (unlikely (!_ep)) {
		pr_err("%s: _ep is NULL\n", __func__);
		return -EINVAL;
	}


	ep = to_ambarella_ep(_ep);
	udc = ep->udc;

	/* check whether USB is suspended */
	if((readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_SUSP_STS) && udc->sys_suspended){
		pr_err("%s: UDC is suspended!\n", __func__);
		return -ESHUTDOWN;
	}

	for(i = 0; i < EP_NUM_MAX; i++) {
		struct ambarella_ep *endp;
		endp = &udc->ep[i];

		if (endp != NULL && endp->dma_going) //Check for any ongoing DMA
			break;

		// If no other onging DMA and the current req is for ISO, enable the DMA
		if((i == EP_NUM_MAX - 1) && IS_ISO_IN_EP(ep)) {
			setbitsl(udc->base_reg + USB_DEV_CTRL_REG,
					USB_DEV_RCV_DMA_EN | USB_DEV_TRN_DMA_EN);
		}
	}

	if (unlikely (!ep->ep.desc && !IS_EP0(ep))) {
		pr_err("%s: %s, invalid args\n", __func__, _ep->name);
		return -EINVAL;
	}

	if( unlikely( !udc->driver || udc->gadget.speed == USB_SPEED_UNKNOWN)){
		dprintk(DEBUG_NORMAL, "%s: %01d %01d\n", _ep->name,
			!udc->driver, udc->gadget.speed==USB_SPEED_UNKNOWN);
		return -ESHUTDOWN;
	}

	if (unlikely(!_req )) {
		pr_err("%s: %s, _req is NULL\n", __func__, _ep->name);
		return -EINVAL;
	}

	req = to_ambarella_req(_req);
	if (unlikely(!req->req.complete || !req->req.buf
				|| !list_empty(&req->queue))) {
		pr_err("%s: %s, %01d %01d %01d\n", __func__, _ep->name,
			!_req->complete, !_req->buf, !list_empty(&req->queue));

		return -EINVAL;
	}

	if(IS_EP0(ep) && (udc->auto_ack_0_pkt == 1)){
		/* It's status stage in setup packet. And A2/A3 will
		  * automatically send the zero-length packet to Host */
		udc->auto_ack_0_pkt = 0;
		req->req.actual = 0;
		req->req.complete(&ep->ep, &req->req);
		return 0;
	}

	if (unlikely((unsigned long)req->req.buf & 0x7)) {
		req->use_aux_buf = 1;

		if (req->buf_aux == NULL) {
			req->buf_aux = kmalloc(UDC_DMA_MAXPACKET, GFP_ATOMIC);
			if (req->buf_aux == NULL)
				return -ENOMEM;
		}

		if (ep->dir == USB_DIR_IN)
			memcpy(req->buf_aux, req->req.buf, req->req.length);
	} else {
		req->use_aux_buf = 0;
	}

	ret = ambarella_map_dma_buffer(ep, req);
	if (ret) {
		pr_err("%s failed to map dma memory 0x%x\n", __func__, ret);
		return ret;
	}
	_req->status = -EINPROGRESS;
	_req->actual = 0;

	ret = ambarella_prepare_descriptor(ep, req, gfp_flags);
	if (ret) {
		pr_err("%s: could not create DMA desc chain:%d\n", __func__, ret);
		return ret;
	}

	/* disable IRQ handler's bottom-half  */
	spin_lock_irqsave(&udc->lock, flags);

	/* kickstart this i/o queue? */
	//if (list_empty(&ep->queue) && !ep->halted) {
	if (list_empty(&ep->queue)) {
		/* when the data length in ctrl-out transfer is zero, we just
		  * need to implement the STATUS-IN stage. But we don't
		  * implement the case that the data length in ctrl-in transfer
		  * is zero. */
		if(req->req.length == 0) {
			if(ep->id == CTRL_OUT) {
				ambarella_udc_done(ep, req, 0);
				/* told UDC the configuration is done, and to ack HOST */
				//setbitsl(USB_DEV_CTRL_REG, USB_DEV_CSR_DONE);
				//udelay(150);
				/* For STATUS-IN stage */
				ambarella_clr_ep_nak(&udc->ep[CTRL_IN]);
				/* Re-enable Rx DMA to receive next setup packet */
				ambarella_set_rx_dma(ep, NULL);
				ep->dma_going = 0;
				goto finish;
			} else if (ep->id == CTRL_IN) {
				printk(KERN_INFO "%s: send zero-length packet\n", ep->ep.name);
				ambarella_udc_done(ep, req, 0);
				goto finish;
			}
		}

		if (ep->dir == USB_DIR_IN) {
			/* no need to wait for IN-token for ISO transfer */
#if 0
			if (IS_ISO_IN_EP(ep)) {
				writel(udc->base_reg + ep->ep_reg.sts_reg, USB_EP_IN_PKT);
				ambarella_set_tx_dma(ep, req);
			}
#endif
			/* enable dma completion interrupt for current TX data */
			clrbitsl(udc->base_reg + USB_DEV_EP_INTR_MSK_REG, 1 << ep->id);
			ambarella_clr_ep_nak(ep);
		} else {
			ambarella_set_rx_dma(ep, req);
		}
	}

	list_add_tail(&req->queue, &ep->queue);

finish:
	/* enable IRQ handler's bottom-half  */
	spin_unlock_irqrestore(&udc->lock, flags);

	return 0;
}

static int ambarella_udc_dequeue(struct usb_ep *_ep, struct usb_request *_req)
{
	struct ambarella_ep *ep = to_ambarella_ep(_ep);
	struct ambarella_udc *udc = ep->udc;
	struct ambarella_request *req;
	unsigned long flags;
	unsigned halted;

	if (!ep->udc->driver)
		return -ESHUTDOWN;

	if (!_ep || !_req)
		return -EINVAL;

	spin_lock_irqsave(&ep->udc->lock, flags);
	/* make sure the request is actually queued on this endpoint */
	list_for_each_entry (req, &ep->queue, queue) {
		if (&req->req == _req)
			break;
	}
	if (&req->req != _req) {
		spin_unlock_irqrestore(&ep->udc->lock, flags);
		return -EINVAL;
	}

	halted = ep->halted;
	ep->halted = 1;

	/* request in processing */
	if((ep->data_desc == req->data_desc) && (ep->dma_going == 1)) {
		if (ep->dir == USB_DIR_IN)
			ep->cancel_transfer = 1;
		else {
			u32 tmp, desc_status;
			/* stop potential receive DMA */
			tmp = readl(udc->base_reg + USB_DEV_CTRL_REG);
			clrbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_RCV_DMA_EN);

			/* cancel transfer later in ISR if descriptor was touched. */
			desc_status = req->data_desc->status;
			if (desc_status != USB_DMA_BUF_HOST_RDY)
				ep->cancel_transfer = 1;

			writel(tmp, udc->base_reg + USB_DEV_CTRL_REG);
		}
	}

	ambarella_udc_done(ep, req, -ECONNRESET);

	ep->halted = halted;
	spin_unlock_irqrestore(&ep->udc->lock, flags);

	return 0;
}

/*
 * ambarella_udc_set_halt
 */
static int ambarella_udc_set_halt(struct usb_ep *_ep, int value)
{
	struct ambarella_ep *ep = to_ambarella_ep(_ep);
	struct ambarella_udc *udc = ep->udc;
	unsigned long flags;

	if (unlikely (!_ep)) {
		pr_err("%s: invalid _ep\n", __func__);
		return -EINVAL;
	}
	if (unlikely (!ep->ep.desc && !IS_EP0(ep))) {
		pr_err("%s: %s, -EINVAL 1\n", __func__,_ep->name);
		return -EINVAL;
	}
	if (!ep->udc->driver || ep->udc->gadget.speed == USB_SPEED_UNKNOWN){
		pr_err("%s: %s, -ESHUTDOWN\n", __func__, _ep->name);
		return -ESHUTDOWN;
	}
	/* isochronous transfer never halts because there is no handshake
	 * to report a halt condition */
	if (ep->ep.desc /* not ep0 */ && IS_ISO_IN_EP(ep)) {
		pr_err("%s: %s, -EINVAL 2\n", __func__, _ep->name);
		return -EINVAL;
	}

	spin_lock_irqsave(&ep->udc->lock, flags);

	/* set/clear, then synch memory views with the device */
	if (value) { /* stall endpoint */
		if (ep->dir == USB_DIR_IN) {
			setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_STALL | USB_EP_FLUSH);
		} else {
			int retry_count = 10000;
			/* Wait Rx-FIFO to be empty */
			while(!(readl(udc->base_reg + USB_DEV_STS_REG) & USB_DEV_RXFIFO_EMPTY_STS)){
				if (retry_count-- < 0) {
					printk(KERN_ERR"[USB] stall_endpoint:failed");
					break;
				}
			}
			setbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_STALL);
		}
	} else { /* clear stall endpoint */
		clrbitsl(udc->base_reg + ep->ep_reg.ctrl_reg, USB_EP_STALL);
	}

	ep->halted = !!value;

	spin_unlock_irqrestore(&ep->udc->lock, flags);

	return 0;
}


static const struct usb_ep_ops ambarella_ep_ops = {
	.enable		= ambarella_udc_ep_enable,
	.disable	= ambarella_udc_ep_disable,

	.alloc_request	= ambarella_udc_alloc_request,
	.free_request	= ambarella_udc_free_request,

	.queue		= ambarella_udc_queue,
	.dequeue	= ambarella_udc_dequeue,

	.set_halt	= ambarella_udc_set_halt,
	/* fifo ops not implemented */
};

/*------------------------- usb_gadget_ops ----------------------------------*/


static int ambarella_udc_get_frame(struct usb_gadget *_gadget)
{
	struct ambarella_udc *udc = to_ambarella_udc(_gadget);
	return (readl(udc->base_reg + USB_DEV_STS_REG) >> 18) & 0x7ff;
}


static int ambarella_udc_wakeup(struct usb_gadget *_gadget)
{
	struct ambarella_udc *udc = to_ambarella_udc(_gadget);
	u32 tmp;

	tmp = readl(udc->base_reg + USB_DEV_CFG_REG);
	/* Remote wakeup feature not enabled by host */
	if ((!udc->remote_wakeup_en) || (!(tmp & USB_DEV_REMOTE_WAKEUP_EN)))
		return -ENOTSUPP;

	tmp = readl(udc->base_reg + USB_DEV_STS_REG);
	/* not suspended? */
	if (!(tmp & USB_DEV_SUSP_STS))
		return 0;

	/* trigger force resume */
	setbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_REMOTE_WAKEUP);

	return 0;
}

static int ambarella_udc_set_pullup(struct ambarella_udc *udc, int is_on)
{
	if (is_on) {
		ambarella_udc_enable(udc);
		/* reconnect to host */
		clrbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_SOFT_DISCON);
	} else {
		if (udc->gadget.speed != USB_SPEED_UNKNOWN)
			ambarella_stop_activity(udc);
		/* disconnect to host */
		setbitsl(udc->base_reg + USB_DEV_CTRL_REG, USB_DEV_SOFT_DISCON);
		ambarella_udc_disable(udc);
	}

	return 0;
}

static int ambarella_udc_vbus_session(struct usb_gadget *gadget, int is_active)
{
	unsigned long flags;
	struct ambarella_udc *udc = to_ambarella_udc(gadget);

	spin_lock_irqsave(&udc->lock, flags);
	if (udc->driver)
		ambarella_udc_set_pullup(udc, is_active);
	else
		ambarella_udc_set_pullup(udc, 0);
	spin_unlock_irqrestore(&udc->lock, flags);

	return 0;
}

static int ambarella_udc_pullup(struct usb_gadget *gadget, int is_on)
{
	unsigned long	flags;
	struct ambarella_udc *udc = to_ambarella_udc(gadget);

	spin_lock_irqsave(&udc->lock, flags);
	ambarella_udc_set_pullup(udc, is_on);
	spin_unlock_irqrestore(&udc->lock, flags);

	return 0;
}

static int ambarella_udc_start(struct usb_gadget *gadget,
			struct usb_gadget_driver *driver)
{
	struct ambarella_udc *udc = to_ambarella_udc(gadget);
	unsigned long flags;

	spin_lock_irqsave(&udc->lock, flags);

	/* Hook the driver */
	driver->driver.bus = NULL;
	udc->driver = driver;
	/* Enable udc */
	ambarella_udc_enable(udc);

	spin_unlock_irqrestore(&udc->lock, flags);

	return 0;
}

static int ambarella_udc_stop(struct usb_gadget *gadget)
{
	struct ambarella_udc *udc = to_ambarella_udc(gadget);
	unsigned long flags;

	spin_lock_irqsave(&udc->lock, flags);

	ambarella_stop_activity(udc);
	ambarella_udc_disable(udc);
	udc->driver = NULL;

	spin_unlock_irqrestore(&udc->lock, flags);

	return 0;
}

static const struct usb_gadget_ops ambarella_ops = {
	.get_frame		= ambarella_udc_get_frame,
	.wakeup			= ambarella_udc_wakeup,
	.pullup			= ambarella_udc_pullup,
	.vbus_session		= ambarella_udc_vbus_session,
	/*.set_selfpowered: Always selfpowered */
	.udc_start		= ambarella_udc_start,
	.udc_stop		= ambarella_udc_stop,
};

/*------------------------- gadget driver handling---------------------------*/

/* Tears down device */
static void ambarella_gadget_release(struct device *dev)
{
	struct ambarella_udc *udc = dev_get_drvdata(dev);
	kfree(udc);
}

static void ambarella_init_gadget(struct ambarella_udc *udc,
	struct platform_device *pdev)
{
	struct ambarella_ep *ep;
	u32 i;

	spin_lock_init (&udc->lock);

	udc->gadget.ops = &ambarella_ops;
	udc->gadget.name = gadget_name;
	udc->gadget.max_speed = USB_SPEED_HIGH;
	udc->gadget.ep0 = &udc->ep[CTRL_IN].ep;
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);

	/* set basic ep parameters */
	for (i = 0; i < EP_NUM_MAX; i++) {
		ep = &udc->ep[i];
		ep->ep.name = amb_ep_string[i];
		ep->id = i;
		ep->ep.ops = &ambarella_ep_ops;
		ep->ep.maxpacket = (unsigned short) ~0;
		ep->ep.maxpacket_limit = (unsigned short) ~0;

		if (i < EP_IN_NUM) {
			ep->ep.caps.dir_in = true;
			ep->dir = USB_DIR_IN;
		} else {
			ep->ep.caps.dir_out = true;
			ep->dir = USB_DIR_OUT;
		}

		if (i == CTRL_IN || i == CTRL_OUT){
			ep->ep.caps.type_control = true;
		}else{
			ep->ep.caps.type_iso = true;
			ep->ep.caps.type_bulk = true;
			ep->ep.caps.type_int = true;
		}

	}

	udc->ep[CTRL_IN].ep.maxpacket = USB_EP_CTRL_MAX_PKT_SZ;
	udc->ep[CTRL_OUT].ep.maxpacket = USB_EP_CTRL_MAX_PKT_SZ;

	return;
}

static void ambarella_udc_enable(struct ambarella_udc *udc)
{
	if (udc->udc_is_enabled)
		return;

	udc->udc_is_enabled = 1;

	/* Disable Tx and Rx DMA */
	clrbitsl(udc->base_reg + USB_DEV_CTRL_REG,
		USB_DEV_RCV_DMA_EN | USB_DEV_TRN_DMA_EN);


	/* flush all of dma fifo */
	ambarella_udc_fifo_flush(udc);

	/* initialize ep0 register */
	init_ep0(udc);

	/* enable ep0 interrupt. */
	clrbitsl(udc->base_reg + USB_DEV_EP_INTR_MSK_REG,
		USB_DEV_EP_INTR_MSK(CTRL_IN) | USB_DEV_EP_INTR_MSK(CTRL_OUT));

	/* enable Tx and Rx DMA */
	setbitsl(udc->base_reg + USB_DEV_CTRL_REG,
		USB_DEV_RCV_DMA_EN | USB_DEV_TRN_DMA_EN);

	/* enable device interrupt:
	 * Set_Configure, Set_Interface, Speed Enumerate Complete, Reset */
	clrbitsl(udc->base_reg + USB_DEV_INTR_MSK_REG,
			USB_DEV_MSK_SET_CFG |
			USB_DEV_MSK_SET_INTF |
			USB_DEV_MSK_SPD_ENUM_CMPL |
			USB_DEV_MSK_RESET);
}

static void ambarella_udc_disable(struct ambarella_udc *udc)
{
	/* Disable all interrupts and Clear the interrupt registers */
	ambarella_disable_all_intr(udc);

	/* Disable Tx and Rx DMA */
	clrbitsl(udc->base_reg + USB_DEV_CTRL_REG,
			USB_DEV_RCV_DMA_EN | USB_DEV_TRN_DMA_EN);

	udc->gadget.speed = USB_SPEED_UNKNOWN;
	udc->udc_is_enabled = 0;
}


/*
 * ambarella_udc_reinit
 */
static void ambarella_udc_reinit(struct ambarella_udc *udc)
{
	u32 i;

	/* device/ep0 records init */
	INIT_LIST_HEAD (&udc->gadget.ep_list);
	INIT_LIST_HEAD (&udc->gadget.ep0->ep_list);
	udc->auto_ack_0_pkt = 0;
	udc->remote_wakeup_en = 0;

	for (i = 0; i < EP_NUM_MAX; i++) {
		struct ambarella_ep *ep = &udc->ep[i];

		if (!IS_EP0(ep)){
			list_add_tail (&ep->ep.ep_list, &udc->gadget.ep_list);
		}

		ep->udc = udc;
		ep->halted = 0;
		ep->data_desc = NULL;
		ep->last_data_desc = NULL;
		INIT_LIST_HEAD (&ep->queue);
	}
}

static void ambarella_udc_disconnect_tasklet(unsigned long data)
{
	struct ambarella_udc *udc = (void *)data;

	if (udc->driver && udc->driver->disconnect)
		udc->driver->disconnect(&udc->gadget);
}

static const struct soc_device_attribute ambarella_udc_socinfo[] = {
	{ .family = "Ambarella 10nm" },
	{ .soc_id = "s5l" },
	{ .soc_id = "cv5" },
	{/* sentinel */}
};

static int ambarella_udc_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ambarella_udc *udc;
	struct usb_phy *phy;
	int retval;

	dev_info(&pdev->dev, "%s: version %s\n", gadget_name, DRIVER_VERSION);

	udc = devm_kzalloc(&pdev->dev, sizeof(struct ambarella_udc), GFP_KERNEL);
	if (!udc) {
		dev_err(&pdev->dev, "failed to allocate memory\n");
		return -ENOMEM;
	}
	udc->dev = &pdev->dev;

	udc->base_reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(udc->base_reg)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(udc->base_reg);
	}

	udc->rct_reg = syscon_regmap_lookup_by_phandle(np, "amb,rct-regmap");
	if (IS_ERR(udc->rct_reg))
		udc->rct_reg = NULL;

	udc->scr_reg = syscon_regmap_lookup_by_phandle_args(np, "amb,scr-regmap",
					1, &udc->status_offset);
	if (IS_ERR(udc->scr_reg)) {
		dev_err(&pdev->dev, "no scr regmap!\n");
		return -ENODEV;
	}

	retval = of_property_read_u32(np, "amb,ep-bulk-fifo-factor", &udc->bulk_fifo_factor);
	if (retval < 0)
		udc->bulk_fifo_factor = 1;

	udc->irq = platform_get_irq(pdev, 0);
	if (udc->irq < 0) {
		dev_err(&pdev->dev, "no irq!\n");
		return -ENODEV;
	}

	/* get the PHY device */
	phy = devm_usb_get_phy_by_phandle(&pdev->dev, "amb,usbphy", 0);
	if (IS_ERR(phy)) {
		dev_err(&pdev->dev, "Can't get USB PHY %ld\n", PTR_ERR(phy));
		return -ENXIO;
	}

	if (soc_device_match(ambarella_udc_socinfo))
		udc->connect_status = BIT(26);
	else
		udc->connect_status = BIT(25);

	usb_phy_init(phy);
	udc->phy = phy;

	ambarella_udc_reset(udc, np);

	udc->udc_is_enabled = 0;
	udc->vbus_status = 0;
	/* "total tx_fifosize" - "ep0 tx_fifosize" */
	udc->tx_fifosize = USB_TXFIFO_DEPTH - USB_TXFIFO_DEPTH_CTRLIN;

	ambarella_init_gadget(udc, pdev);
	ambarella_udc_reinit(udc);
	ambarella_regaddr_map(udc);

	/*initial usb hardware, and set soft disconnect */
	ambarella_init_usb(udc);

	/* DMA pool create */
	udc->desc_dma_pool = dma_pool_create("desc_dma_pool", &pdev->dev,
		sizeof(struct ambarella_data_desc), 16, 0);
	if (!udc->desc_dma_pool) {
		pr_err("%s: can't get descriptor dma pool\n", __func__);
		return -ENOMEM;
	}

	udc->setup_buf = dma_pool_alloc(udc->desc_dma_pool, GFP_KERNEL,
		&udc->setup_addr);
	if(udc->setup_buf == NULL) {
		printk(KERN_ERR "No memory to DMA\n");
		retval = -ENOMEM;
		goto err_out1;
	}

	retval = ambarella_ep_dummy_init(udc);
	if (retval)
		goto err_out2;


	tasklet_init(&udc->disconnect_tasklet, ambarella_udc_disconnect_tasklet,
			(unsigned long)udc);

	/* irq setup after old hardware state is cleaned up */
	retval = devm_request_irq(&pdev->dev, udc->irq, ambarella_udc_irq,
				IRQF_TRIGGER_HIGH, dev_name(&pdev->dev), udc);
	if (retval != 0) {
		pr_err("%s: cannot get irq %d\n", __func__, udc->irq);
		goto err_out3;
	}

#if 0
	timer_setup(&udc->vbus_timer, ambarella_vbus_timer, 0);
	mod_timer(&udc->vbus_timer, jiffies + VBUS_POLL_TIMEOUT);
#endif

	udc->pre_state = USB_STATE_NOTATTACHED;
	INIT_WORK(&udc->uevent_work, ambarella_uevent_work);

	udc->proc_file = proc_create_data("udc", S_IRUGO,
		ambarella_procfs_dir(), &ambarella_udc_fops, udc);
	if (udc->proc_file == NULL) {
		retval = -ENOMEM;
		goto err_out3;
	}

	create_debugfs_files();

	/* Register gadget driver */
	retval = usb_add_gadget_udc_release(&pdev->dev, &udc->gadget,
					ambarella_gadget_release);
	if (retval) {
		goto err_out4;
	}

	platform_set_drvdata(pdev, udc);

	return 0;

err_out4:
	remove_debugfs_files();
err_out3:
	ambarella_ep_dummy_exit(udc);
err_out2:
	dma_pool_free(udc->desc_dma_pool, udc->setup_buf, udc->setup_addr);
err_out1:
	dma_pool_destroy(udc->desc_dma_pool);
	return retval;
}


/*
 * Name: ambarella_udc_remove
 * Description:
 *	Remove udc driver.
 */
static int ambarella_udc_remove(struct platform_device *pdev)
{
	struct ambarella_udc *udc = platform_get_drvdata(pdev);

	usb_del_gadget_udc(&udc->gadget);
	if (udc->driver)
		return -EBUSY;

#if 0
	del_timer_sync(&udc->vbus_timer);
#endif

	remove_proc_entry("udc", ambarella_procfs_dir());
	remove_debugfs_files();

	tasklet_kill(&udc->disconnect_tasklet);

	ambarella_ep_dummy_exit(udc);
	dma_pool_free(udc->desc_dma_pool, udc->setup_buf, udc->setup_addr);
	dma_pool_destroy(udc->desc_dma_pool);

	return 0;
}


#ifdef CONFIG_PM
static int ambarella_udc_suspend(struct platform_device *pdev, pm_message_t message)
{
	unsigned long flags;
	int retval = 0;
	struct ambarella_udc *udc;

	udc = platform_get_drvdata(pdev);
	udc->sys_suspended = 1;
	disable_irq(udc->irq);

#if 0
	del_timer_sync(&udc->vbus_timer);
#endif

	spin_lock_irqsave(&udc->lock, flags);
	ambarella_udc_set_pullup(udc, 0);
	spin_unlock_irqrestore(&udc->lock, flags);

	udc->udc_is_enabled = 0;
	udc->vbus_status = 0;

	dev_dbg(&pdev->dev, "%s exit with %d @ %d\n",
		__func__, retval, message.event);

	return retval;
}

static int ambarella_udc_resume(struct platform_device *pdev)
{
	unsigned long flags;
	int retval = 0;
	struct ambarella_udc *udc;

	udc = platform_get_drvdata(pdev);
	udc->sys_suspended = 0;

	/* Initial USB PLL */
	usb_phy_init(udc->phy);
	/* Reset USB */
	ambarella_udc_reset(udc, pdev->dev.of_node);
	/*initial usb hardware */
	ambarella_init_usb(udc);

	enable_irq(udc->irq);

	spin_lock_irqsave(&udc->lock, flags);
	ambarella_udc_set_pullup(udc, 1);
	spin_unlock_irqrestore(&udc->lock, flags);

#if 0
	timer_setup(&udc->vbus_timer, ambarella_vbus_timer, 0);
	mod_timer(&udc->vbus_timer, jiffies + VBUS_POLL_TIMEOUT);
#endif

	dev_dbg(&pdev->dev, "%s exit with %d\n", __func__, retval);

	return retval;
}
#endif

static const struct of_device_id ambarella_udc_dt_ids[] = {
	{ .compatible = "ambarella,udc" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, ambarella_udc_dt_ids);

static struct platform_driver ambarella_udc_driver = {
	.driver		= {
		.name	= "ambarella-udc",
		.owner	= THIS_MODULE,
		.of_match_table	= ambarella_udc_dt_ids,
	},
	.remove		= ambarella_udc_remove,
#ifdef CONFIG_PM
	.suspend	= ambarella_udc_suspend,
	.resume		= ambarella_udc_resume,
#endif
};

module_platform_driver_probe(ambarella_udc_driver, ambarella_udc_probe);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ambarella-usbgadget");

