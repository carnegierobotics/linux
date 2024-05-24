/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2004-2010, Ambarella, Inc.
 */

#ifndef __AMBARELLA_UART_H__
#define __AMBARELLA_UART_H__

/* ==========================================================================*/
#define UART_RB_OFFSET			0x00
#define UART_TH_OFFSET			0x00
#define UART_DLL_OFFSET			0x00
#define UART_IE_OFFSET			0x04
#define UART_DLH_OFFSET			0x04
#define UART_II_OFFSET			0x08
#define UART_FC_OFFSET			0x08
#define UART_LC_OFFSET			0x0c
#define UART_MC_OFFSET			0x10
#define UART_LS_OFFSET			0x14
#define UART_MS_OFFSET			0x18
#define UART_SC_OFFSET			0x1c	/* Byte */
#define UART_DMAE_OFFSET		0x28
#define UART_DMAF_OFFSET		0x40	/* DMA fifo */
#define UART_US_OFFSET			0x7c
#define UART_TFL_OFFSET			0x80
#define UART_RFL_OFFSET			0x84
#define UART_SRR_OFFSET			0x88
#define UART_RTR_OFFSET			0xac
#define UART_TTR_OFFSET			0xb0

/* UART[x]_IE_REG */
#define UART_IE_PTIME			0x80
#define UART_IE_ETOI			0x20
#define UART_IE_EBDI			0x10
#define UART_IE_EDSSI			0x08
#define UART_IE_ELSI			0x04
#define UART_IE_ETBEI			0x02
#define UART_IE_ERBFI			0x01

/* UART[x]_II_REG */
#define UART_II_MODEM_STATUS_CHANGED	0x00
#define UART_II_NO_INT_PENDING		0x01
#define UART_II_THR_EMPTY		0x02
#define UART_II_RCV_DATA_AVAIL		0x04
#define UART_II_RCV_STATUS		0x06
#define UART_II_CHAR_TIMEOUT		0x0c

/* UART[x]_FC_REG */
#define UART_FC_RX_ONECHAR		0x00
#define UART_FC_RX_QUARTER_FULL		0x40
#define UART_FC_RX_HALF_FULL		0x80
#define UART_FC_RX_2_TO_FULL		0xc0
#define UART_FC_TX_EMPTY		0x00
#define UART_FC_TX_2_IN_FIFO		0x10
#define UART_FC_TX_QUATER_IN_FIFO	0x20
#define UART_FC_TX_HALF_IN_FIFO		0x30
#define UART_FC_DMA_SELECT		0x08
#define UART_FC_XMITR			0x04
#define UART_FC_RCVRR			0x02
#define UART_FC_FIFOE			0x01

/* UART[x]_LC_REG */
#define UART_LC_DLAB			0x80
#define UART_LC_BRK			0x40
#define UART_LC_EVEN_PARITY		0x10
#define UART_LC_ODD_PARITY		0x00
#define UART_LC_PEN			0x08
#define UART_LC_STOP_2BIT		0x04
#define UART_LC_STOP_1BIT		0x00
#define UART_LC_CLS_8_BITS		0x03
#define UART_LC_CLS_7_BITS		0x02
#define UART_LC_CLS_6_BITS		0x01
#define UART_LC_CLS_5_BITS		0x00
/*	quick defs */
#define	UART_LC_8N1			0x03
#define	UART_LC_7E1			0x0a

/* UART[x]_MC_REG */
#define UART_MC_SIRE			0x40
#define UART_MC_AFCE			0x20
#define UART_MC_LB			0x10
#define UART_MC_OUT2			0x08
#define UART_MC_OUT1			0x04
#define UART_MC_RTS			0x02
#define UART_MC_DTR			0x01

/* UART[x]_LS_REG */
#define UART_LS_FERR			0x80
#define UART_LS_TEMT			0x40
#define UART_LS_THRE			0x20
#define UART_LS_BI			0x10
#define UART_LS_FE			0x08
#define UART_LS_PE			0x04
#define UART_LS_OE			0x02
#define UART_LS_DR			0x01

/* UART[x]_MS_REG */
#define UART_MS_DCD			0x80
#define UART_MS_RI			0x40
#define UART_MS_DSR			0x20
#define UART_MS_CTS			0x10
#define UART_MS_DDCD			0x08
#define UART_MS_TERI			0x04
#define UART_MS_DDSR			0x02
#define UART_MS_DCTS			0x01

/* UART[x]_US_REG */
#define UART_US_RFF			0x10
#define UART_US_RFNE			0x08
#define UART_US_TFE			0x04
#define UART_US_TFNF			0x02
#define UART_US_BUSY			0x01

/* ==========================================================================*/
#define UART_FIFO_SIZE			(16)

#define DEFAULT_AMBARELLA_UART_MCR	(0)
#define DEFAULT_AMBARELLA_UART_IER	(UART_IE_ELSI | UART_IE_ERBFI | UART_IE_ETOI)

#endif /* __PLAT_AMBARELLA_UART_H__ */

