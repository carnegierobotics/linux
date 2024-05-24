// SPDX-License-Identifier: GPL-2.0
/*
* History:
*	2008/06/12 - [Cao Rongrong] created file
*
* Copyright (C) 2008 by Ambarella, Inc.
* http://www.ambarella.com
 */

#ifndef __AMBARELLA_UDC_H__
#define __AMBARELLA_UDC_H__

/* ==========================================================================*/

//-------------------------------------
// USB RxFIFO and TxFIFO depth (single or multiple)
//-------------------------------------
#define USB_RXFIFO_DEPTH_CTRLOUT	(256 << 16)	// shared
#define USB_RXFIFO_DEPTH_BULKOUT	(256 << 16)	// shared
#define USB_RXFIFO_DEPTH_INTROUT	(256 << 16)	// shared
#define USB_TXFIFO_DEPTH_CTRLIN		(64 / 4)	// 16 32-bit
#define USB_TXFIFO_DEPTH_BULKIN		(1024 / 4)	// 256 32-bit
#define USB_TXFIFO_DEPTH_INTRIN		(512 / 4)	// 128 32-bit
#define USB_TXFIFO_DEPTH_ISOIN		((512 * 2) / 4)	// 128 32-bit

#define USB_TXFIFO_DEPTH		(64 / 4 + 4 * 512 / 4)	// 528 32-bit
#define USB_RXFIFO_DEPTH		(256)			// 256 32-bit

//-------------------------------------
// USB register address
//-------------------------------------
#define	USB_EP_IN_CTRL_REG(n)		(0x0000 + 0x0020 * (n))
#define	USB_EP_IN_STS_REG(n)		(0x0004 + 0x0020 * (n))
#define	USB_EP_IN_BUF_SZ_REG(n)		(0x0008 + 0x0020 * (n))
#define	USB_EP_IN_MAX_PKT_SZ_REG(n)	(0x000c + 0x0020 * (n))
#define	USB_EP_IN_DAT_DESC_PTR_REG(n)	(0x0014 + 0x0020 * (n))
#define USB_EP_IN_WR_CFM_REG		(0x001c + 0x0020 * (n))	// for slave-only mode

#define	USB_EP_OUT_CTRL_REG(n)		(0x0200 + 0x0020 * (n))
#define	USB_EP_OUT_STS_REG(n)		(0x0204 + 0x0020 * (n))
#define	USB_EP_OUT_PKT_FRM_NUM_REG(n)	(0x0208 + 0x0020 * (n))
#define	USB_EP_OUT_MAX_PKT_SZ_REG(n) 	(0x020c + 0x0020 * (n))
#define	USB_EP_OUT_SETUP_BUF_PTR_REG(n) (0x0210 + 0x0020 * (n))
#define	USB_EP_OUT_DAT_DESC_PTR_REG(n)	(0x0214 + 0x0020 * (n))
#define USB_EP_OUT_RD_CFM_ZO_REG	(0x021c + 0x0020 * (n))	// for slave-only mode

#define USB_DEV_CFG_REG			0x0400
#define USB_DEV_CTRL_REG		0x0404
#define USB_DEV_STS_REG			0x0408
#define USB_DEV_INTR_REG		0x040c
#define USB_DEV_INTR_MSK_REG		0x0410
#define USB_DEV_EP_INTR_REG		0x0414
#define USB_DEV_EP_INTR_MSK_REG		0x0418
#define USB_DEV_TEST_MODE_REG		0x041c

#define USB_UDC_REG(n)			(0x0504 + 0x0004 * (n))	// EP0 is reserved for control endpoint

//-------------------------------------
// USB register fields
//-------------------------------------
//-----------------
// for endpoint specific registers
//-----------------
// for USB_EP_IN_CTRL_REG(n) and USB_EP_OUT_CTRL_REG(n)	// default
#define USB_EP_STALL			0x00000001		// 0 (RW)
#define USB_EP_FLUSH			0x00000002		// 0 (RW)
#define USB_EP_SNOOP			0x00000004		// 0 (RW) - for OUT EP only
#define USB_EP_POLL_DEMAND		0x00000008		// 0 (RW) - for IN EP only
#define USB_EP_TYPE_CTRL		0x00000000		// 00 (RW)
#define USB_EP_TYPE_ISO			0x00000010		// 00 (RW)
#define USB_EP_TYPE_BULK		0x00000020		// 00 (RW)
#define USB_EP_TYPE_INTR		0x00000030		// 00 (RW)
#define USB_EP_NAK_STS			0x00000040		// 0 (RO) - set by UDC after SETUP and STALL
#define USB_EP_SET_NAK			0x00000080		// 0 (WO)
#define USB_EP_CLR_NAK			0x00000100		// 0 (WO)
#define USB_EP_RCV_RDY			0x00000200		// 0 (RW)(D) - set by APP when APP is ready for DMA
														//			   clear by UDC when end of each packet if USB_DEV_DESC_UPD is set
														//			   clear by UDC when end of payload if USB_DEV_DESC_UPD is clear

// for USB_EP_IN_STS_REG(n) and USB_EP_OUT_STS_REG(n)
#define USB_EP_OUT_PKT_MSK		0x00000030		// 00 (R/WC) - 01 for OUT and 10 for SETUP - for OUT EP only
#define USB_EP_OUT_PKT			0x00000010
#define USB_EP_SETUP_PKT		0x00000020
#define USB_EP_IN_PKT			0x00000040		// 0 (R/WC) - for IN EP only
#define USB_EP_BUF_NOT_AVAIL		0x00000080		// 0 (R/WC)(D) - set by UDC when descriptor is not available
														//	         	 clear by APP when interrupt is serviced
#define USB_EP_HOST_ERR			0x00000200		// 0 (R/WC) - set by DMA when DMA is error
#define USB_EP_TRN_DMA_CMPL		0x00000400		// 0 (R/WC)(D) - set by DMA when DMA is completed
#define USB_EP_RCV_CLR_STALL            0x02000000              // 0 (R/WC) - Received Clear Stall indication
														//				 clear by APP when interrupt is serviced
#define USB_EP_RCV_SET_STALL            0x04000000		// 0 (R/WC) - Received Set Stall indication
#define USB_EP_RX_PKT_SZ		0x007ff800		// bit mask (RW) - receive packet size in RxFIFO (e.g. SETUP=64, BULK=512)

#define USB_EP_TXFIFO_EMPTY		0x08000000		// 0 (R/WC) - TxFIFO is empty after DMA transfer done

// for USB_EP_IN_BUF_SZ_REG(n) and USB_EP_OUT_PKT_FRM_NUM_REG(n)
#define USB_EP_TXFIFO_DEPTH		0x0000ffff		// bit mask (RW) - for IN EP only
#define USB_EP_FRM_NUM			0x0000ffff		// bit mask (RW) - for OUT EP only

// for USB_EP_IN_MAX_PKT_SZ_REG(n) and USB_EP_OUT_MAX_PKT_SZ_REG(n)
#define USB_EP_RXFIFO_DEPTH		0xffff0000		// bit mask (RW) - for OUT EP only
#define USB_EP_MAX_PKT_SZ		0x0000ffff		// bit mask (RW)

//-----------------
// for device specific registers
//-----------------
// for USB_DEV_CFG_REG
#define USB_DEV_SPD_HI			0x00000000		// 00 (RW) - PHY CLK = 30 or 60 MHz
#define USB_DEV_SPD_FU			0x00000001		// 00 (RW) - PHY CLK = 30 or 60 MHz
#define USB_DEV_SPD_LO			0x00000002		// 00 (RW) - PHY CLK = 6 MHz
#define USB_DEV_SPD_FU48		0x00000003		// 00 (RW) - PHY CLK = 48 MHz

#define USB_DEV_REMOTE_WAKEUP_EN	0x00000004		// 0 (RW)

#define USB_DEV_BUS_POWER		0x00000000		// 0 (RW)
#define USB_DEV_SELF_POWER		0x00000008		// 0 (RW)

#define USB_DEV_SYNC_FRM_EN		0x00000010		// 0 (RW)

#define USB_DEV_PHY_16BIT		0x00000000		// 0 (RW)
#define USB_DEV_PHY_8BIT		0x00000020		// 0 (RW)

#define USB_DEV_UTMI_DIR_UNI		0x00000000		// 0 (RW) - UDC20 reserved to 0
#define USB_DEV_UTMI_DIR_BI		0x00000040		// 0 (RW)

#define USB_DEV_STS_OUT_NONZERO		0x00000180		// bit mask (RW)

#define USB_DEV_PHY_ERR			0x00000200		// 0 (RW)

#define USB_DEV_SPD_FU_TIMEOUT		0x00001c00		// bit mask (RW)
#define USB_DEV_SPD_HI_TIMEOUT		0x0000e000		// bit mask (RW)

#define USB_DEV_HALT_ACK		0x00000000		// 0 (RW) - ACK Clear_Feature (ENDPOINT_HALT) of EP0
#define USB_DEV_HALT_STALL		0x00010000		// 1 (RW) - STALL Clear_Feature (ENDPOINT_HALT) of EP0

#define USB_DEV_CSR_PRG_EN		0x00020000		// 1 (RW) - enable CSR_DONE of USB_DEV_CFG_REG

#define USB_DEV_SET_DESC_STALL		0x00000000		// 0 (RW) - STALL Set_Descriptor
#define USB_DEV_SET_DESC_ACK		0x00040000		// 1 (RW) - ACK Set_Descriptor

#define USB_DEV_SDR			0x00000000		// 0 (RW)
#define USB_DEV_DDR			0x00080000		// 1 (RW)

// for USB_DEV_CTRL_REG
#define USB_DEV_REMOTE_WAKEUP		0x00000001		// 0 (RW)
#define USB_DEV_RCV_DMA_EN		0x00000004		// 0 (RW)(D)
#define USB_DEV_TRN_DMA_EN		0x00000008		// 0 (RW)(D)

#define USB_DEV_DESC_UPD_PYL		0x00000000		// 0 (RW)(D)
#define USB_DEV_DESC_UPD_PKT		0x00000010		// 0 (RW)(D)

#define USB_DEV_LITTLE_ENDN		0x00000000		// 0 (RW)(D)
#define USB_DEV_BIG_ENDN		0x00000020		// 0 (RW)(D)

#define USB_DEV_PKT_PER_BUF_MD		0x00000000		// 0 (RW)(D) - packet-per-buffer mode
#define USB_DEV_BUF_FIL_MD		0x00000040		// 0 (RW)(D) - buffer fill mode

#define USB_DEV_THRESH_EN		0x00000080		// 0 (RW)(D) - for OUT EP only

#define USB_DEV_BURST_EN		0x00000100		// 0 (RW)(D)

#define USB_DEV_SLAVE_ONLY_MD		0x00000000		// 0 (RW)(D)
#define USB_DEV_DMA_MD			0x00000200		// 0 (RW)(D)

#define USB_DEV_SOFT_DISCON		0x00000400		// 0 (RW) - signal UDC20 to disconnect
#define USB_DEV_TIMER_SCALE_DOWN	0x00000800		// 0 (RW) - for gate-level simulation only
#define USB_DEV_NAK			0x00001000		// 0 (RW) - device NAK (applied to all EPs)
#define USB_DEV_CSR_DONE		0x00002000		// 0 (RW) - set to ACK Set_Configuration or Set_Interface if USB_DEV_CSR_PRG_EN
#define USB_DEV_FLUSH_RXFIFO		0x00004000
														//		    clear to NAK
#define USB_DEV_BURST_LEN		0x00070000		// bit mask (RW)
#define USB_DEV_THRESH_LEN		0x0f000000		// bit mask (RW)

// for USB_DEV_STS_REG
#define USB_DEV_CFG_NUM			0x0000000f		// bit mask (RO)
#define USB_DEV_INTF_NUM		0x000000f0		// bit mask (RO)
#define USB_DEV_ALT_SET			0x00000f00		// bit mask (RO)
#define USB_DEV_SUSP_STS		0x00001000		// bit mask (RO)

#define USB_DEV_ENUM_SPD		0x00006000		// bit mask (RO)
#define USB_DEV_ENUM_SPD_HI		0x00000000		// 00 (RO)
#define USB_DEV_ENUM_SPD_FU		0x00002000		// 00 (RO)
#define USB_DEV_ENUM_SPD_LO		0x00004000		// 00 (RO)
#define USB_DEV_ENUM_SPD_FU48		0x00006000		// 00 (RO)

#define USB_DEV_RXFIFO_EMPTY_STS	0x00008000		// bit mask (RO)
#define USB_DEV_PHY_ERR_STS		0x00010000		// bit mask (RO)
#define USB_DEV_FRM_NUM			0xfffc0000		// bit mask (RO)

// for USB_DEV_INTR_REG
#define USB_DEV_SET_CFG			0x00000001		// 0 (R/WC)
#define USB_DEV_SET_INTF		0x00000002		// 0 (R/WC)
#define USB_DEV_IDLE_3MS		0x00000004		// 0 (R/WC)
#define USB_DEV_RESET			0x00000008		// 0 (R/WC)
#define USB_DEV_SUSP			0x00000010		// 0 (R/WC)
#define USB_DEV_SOF			0x00000020		// 0 (R/WC)
#define USB_DEV_ENUM_CMPL		0x00000040		// 0 (R/WC)

// for USB_DEV_INTR_MSK_REG
#define USB_DEV_MSK_SET_CFG		0x00000001		// 0 (R/WC)
#define USB_DEV_MSK_SET_INTF		0x00000002		// 0 (R/WC)
#define USB_DEV_MSK_IDLE_3MS		0x00000004		// 0 (R/WC)
#define USB_DEV_MSK_RESET		0x00000008		// 0 (R/WC)
#define USB_DEV_MSK_SUSP		0x00000010		// 0 (R/WC)
#define USB_DEV_MSK_SOF			0x00000020		// 0 (R/WC)
#define USB_DEV_MSK_SPD_ENUM_CMPL	0x00000040		// 0 (R/WC)

// for USB_DEV_EP_INTR_REG
#define USB_DEV_EP_INTR(n)		(1 << (n))

// for USB_DEV_EP_INTR_MSK_REG
#define USB_DEV_EP_INTR_MSK(n)		(1 << (n))

#define USB_EP_CTRL_MAX_PKT_SZ		64	// max packet size of control in/out endpoint
#define USB_EP_BULK_MAX_PKT_SZ_HI	512	// max packet size of bulk in/out endpoint (high speed)
#define USB_EP_BULK_MAX_PKT_SZ_FU	64	// max packet size of bulk in/out endpoint (full speed)
#define USB_EP_INTR_MAX_PKT_SZ		64	// max packet size of interrupt in/out endpoint
#define USB_EP_ISO_MAX_PKT_SZ		512	// max packet size of isochronous in/out endpoint


//-------------------------------------
// DMA status quadlet fields
//-------------------------------------
// IN / OUT descriptor specific
#define USB_DMA_RXTX_BYTES		0x0000ffff		// bit mask

// SETUP descriptor specific
#define USB_DMA_CFG_STS			0x0fff0000		// bit mask
#define USB_DMA_CFG_NUM			0x0f000000		// bit mask
#define USB_DMA_INTF_NUM		0x00f00000		// bit mask
#define USB_DMA_ALT_SET			0x000f0000		// bitmask
// ISO OUT descriptor only
#define USB_DMA_FRM_NUM			0x07ff0000		// bit mask
// IN / OUT descriptor specific
#define USB_DMA_LAST			0x08000000		// bit mask

#define USB_DMA_RXTX_STS		0x30000000		// bit mask
#define USB_DMA_RXTX_SUCC		0x00000000		// 00
#define USB_DMA_RXTX_DES_ERR		0x10000000		// 01
#define USB_DMA_RXTX_BUF_ERR		0x30000000		// 11

#define USB_DMA_BUF_STS 		0xc0000000		// bit mask
#define USB_DMA_BUF_HOST_RDY		0x00000000		// 00
#define USB_DMA_BUF_DMA_BUSY		0x40000000		// 01
#define USB_DMA_BUF_DMA_DONE		0x80000000		// 10
#define	USB_DMA_BUF_HOST_BUSY		0xc0000000		// 11

/* ==========================================================================*/

#define CTRL_IN			0
#define CTRL_OUT		16

#define EP_IN_NUM		16
#define EP_NUM_MAX		32

#define CTRL_OUT_UDC_IDX	11

#define ISO_MAX_PACKET		3

#define IS_EP0(ep)		(ep->id == CTRL_IN || ep->id == CTRL_OUT)
#define IS_ISO_IN_EP(ep)	(!IS_EP0(ep) && usb_endpoint_is_isoc_in(ep->ep.desc))

#define UDC_DMA_MAXPACKET	65536

#define VBUS_POLL_TIMEOUT	msecs_to_jiffies(500)

#define UDC_DMA_RETRY_MAX	10

#define setbitsl(a,v)   (writel(((v) | readl(a)), (a)))
#define clrbitsl(a,v)   (writel(((~(v)) & readl(a)), (a)))

//-------------------------------------
// Structure definition
//-------------------------------------
// SETUP buffer descriptor
struct ambarella_setup_desc {
	u32 status;
	u32 reserved;
	u32 data0;
	u32 data1;
	u32 rsvd1;
	u32 rsvd2;
	u32 rsvd3;
	u32 rsvd4;
};

// IN/OUT data descriptor
struct ambarella_data_desc {
	u32 status;
	u32 reserved;
	u32 data_ptr;
	u32 next_desc_ptr;
	u32 rsvd1;
	u32 last_aux;		/* dma enginee may disturb the L bit in status
				 * field, so we use this field as auxiliary to
				 * mark the last descriptor */
	dma_addr_t cur_desc_addr;	/* dma address for this descriptor */
	struct ambarella_data_desc *next_desc_virt;
};


struct ambarella_ep_reg {
	u32 ctrl_reg;
	u32 sts_reg;
	u32 buf_sz_reg;		// IN_EP: buf_sz_reg,     OUT_EP: pkt_frm_num_reg
	u32 max_pkt_sz_reg;	// IN_EP: max_pkt_sz_reg,   OUT EP: buffer_size_max_pkt_sz_reg
	u32 setup_buf_ptr_reg;	// Just for ep0
	u32 dat_desc_ptr_reg;
	//u32 rw_confirm;	// For slave-only mode
};


struct ambarella_request {
	struct list_head	queue;		/* ep's requests */
	struct usb_request	req;

	int			desc_count;
	int			active_desc_count;
	dma_addr_t		data_desc_addr; /* data_desc Physical Address */
	struct ambarella_data_desc 	*data_desc;

	dma_addr_t		dma_aux;
	void			*buf_aux;	/* If the original buffer of
						 * usb_req is not 8-bytes
						 * aligned, we use this buffer
						 * instead */
	unsigned		use_aux_buf : 1,
				mapped : 1;
};


struct ambarella_ep {

	struct list_head		queue;
	struct ambarella_udc		*udc;
	const struct usb_endpoint_descriptor *desc;
	struct usb_ep			ep;
	u8 				id;
	u8 				dir;

	struct ambarella_ep_reg		ep_reg;

	struct ambarella_data_desc 	*data_desc;
	struct ambarella_data_desc 	*last_data_desc;
	dma_addr_t			data_desc_addr; /* data_desc Physical Address */

	unsigned			halted : 1,
					cancel_transfer : 1,
					need_cnak : 1,
					ctrl_sts_phase : 1,
					dma_going : 1;

	unsigned int frame_offset;  /* iso frame num offset */
	unsigned int frame_interval;  /* iso frame num interval */

	dma_addr_t			dummy_desc_addr;
	struct ambarella_data_desc	*dummy_desc;
};

struct ambarella_udc {
	spinlock_t			lock;
	struct device			*dev;
	void __iomem			*base_reg;
	struct regmap			*rct_reg;
	struct regmap			*scr_reg;
	int				irq;
	struct usb_phy			*phy;

	struct proc_dir_entry		*proc_file;
	struct work_struct		uevent_work;
#if 0
	struct timer_list		vbus_timer;
#endif
	enum usb_device_state		pre_state;

	struct usb_gadget		gadget;
	struct usb_gadget_driver	*driver;

	struct dma_pool			*desc_dma_pool;

	struct ambarella_ep		ep[EP_NUM_MAX];
	u32				setup[2];
	dma_addr_t			setup_addr;
	struct ambarella_setup_desc	*setup_buf;

	u16				cur_config;
	u16				cur_intf;
	u16				cur_alt;

	unsigned 			auto_ack_0_pkt : 1,
					remote_wakeup_en  : 1,
					host_suspended : 1,
					sys_suspended : 1,
					reset_by_host : 1,
					vbus_status : 1,
					udc_is_enabled : 1;

	struct tasklet_struct		disconnect_tasklet;
	int				tx_fifosize;
	int				bulk_fifo_factor;
	u32				connect_status;
	u32				status_offset;
};

/* Function Declaration  */
static void ambarella_udc_done(struct ambarella_ep *ep,
		struct ambarella_request *req, int status);

static void ambarella_set_tx_dma(struct ambarella_ep *ep,
	struct ambarella_request * req, int fix);

static void ambarella_set_rx_dma(struct ambarella_ep *ep,
	struct ambarella_request * req);

static void ambarella_udc_reinit(struct ambarella_udc *udc);

static int ambarella_udc_set_halt(struct usb_ep *_ep, int value);

static void ambarella_ep_nuke(struct ambarella_ep *ep, int status);

static void ambarella_stop_activity(struct ambarella_udc *udc);

static void ambarella_udc_enable(struct ambarella_udc *udc);

static void ambarella_udc_disable(struct ambarella_udc *udc);

static int ambarella_udc_vbus_session(struct usb_gadget *gadget, int is_active);

static int ambarella_udc_set_pullup(struct ambarella_udc *udc, int is_on);

static int ambarella_udc_get_frame(struct usb_gadget *_gadget);

#endif

