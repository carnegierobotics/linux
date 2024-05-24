/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012-2016, Ambarella, Inc.
 */

#ifndef __AMBARELLA_SPI_H__
#define __AMBARELLA_SPI_H__

/* ==========================================================================*/

#define SPI_CTRLR0_OFFSET		0x00
#define SPI_CTRLR1_OFFSET		0x04
#define SPI_SSIENR_OFFSET		0x08
#define SPI_SER_OFFSET			0x10
#define SPI_BAUDR_OFFSET		0x14
#define SPI_TXFTLR_OFFSET		0x18
#define SPI_RXFTLR_OFFSET		0x1c
#define SPI_TXFLR_OFFSET		0x20
#define SPI_RXFLR_OFFSET		0x24
#define SPI_SR_OFFSET			0x28
#define SPI_IMR_OFFSET			0x2c
#define SPI_ISR_OFFSET			0x30
#define SPI_RISR_OFFSET			0x34
#define SPI_TXOICR_OFFSET		0x38
#define SPI_RXOICR_OFFSET		0x3c
#define SPI_RXUICR_OFFSET		0x40
#define SPI_MSTICR_OFFSET		0x44	/* clear @CV2FS */
#define SPI_ICR_OFFSET			0x48
#define SPI_DMAC_OFFSET			0x4c
#define SPI_FAULTINJECT_OFFSET		0x50	/* Introduced @CV2FS */
#define SPI_IDR_OFFSET			0x58
#define SPI_VERSION_ID_OFFSET		0x5c
#define SPI_DR_OFFSET			0x60

#define SPI_SSIENPOLR_OFFSET		0x260
#define SPI_SCLK_OUT_DLY_OFFSET		0x264
#define SPI_START_BIT_OFFSET		0x268

/* ==========================================================================*/

/* SPI rw mode */
#define SPI_WRITE_READ			0
#define SPI_WRITE_ONLY			1
#define SPI_READ_ONLY			2

/* SPI enable register */
#define SPI_SSIENR_DISABLE		0
#define SPI_SSIENR_ENABLE		1

/* SPI interrupt mask */
#define SPI_TXEIS_MASK			0x00000001
#define SPI_TXOIS_MASK 			0x00000002
#define SPI_RXUIS_MASK 			0x00000004
#define SPI_RXOIS_MASK 			0x00000008
#define SPI_RXFIS_MASK 			0x00000010
#define SPI_FCRIS_MASK 			0x00000100

/* SPI status register */
#define SPI_SR_BUSY			0x00000001
#define SPI_SR_TFNF			0x00000002
#define SPI_SR_TFE			0x00000004
#define SPI_SR_RFNE			0x00000008
#define SPI_SR_RFF			0x00000010
#define SPI_SR_TXE			0x00000020
#define SPI_SR_DCOL			0x00000040

/* SPI dma enable register */
#define SPI_DMAC_RX_EN			0x1
#define SPI_DMAC_TX_EN			0x2

/* SPI_FIFO_SIZE */
#define SPI_DATA_FIFO_SIZE_16		0x10
#define SPI_DATA_FIFO_SIZE_32		0x20
#define SPI_DATA_FIFO_SIZE_64		0x40
#define SPI_DATA_FIFO_SIZE_128		0x80

/* ==========================================================================*/

typedef union {
	struct {
		u32		dfs	: 4;	/* [3:0] */
		u32		frf	: 2;	/* [5:4] */
		u32		scph	: 1;	/* [6] */
		u32		scpol	: 1;	/* [7] */
		u32		tmod	: 2;	/* [9:8] */
		u32		slv_oe	: 1;	/* [10] */
		u32		srl	: 1;	/* [11] */
		u32		reserv1	: 5;	/* [16:12] */
		u32		residue	: 1;	/* [17] */
		u32		tx_lsb	: 1;	/* [18] */
		u32		rx_lsb	: 1;	/* [19] */
		u32		reserv2	: 1;	/* [20] */
		u32		fc_en	: 1;	/* [21] */
		u32		rxd_mg	: 4;	/* [25:22] */
		u32		byte_ws	: 1;	/* [26] */
		u32		hold	: 1;	/* [27] */
		u32		reserv3	: 4;	/* [31:28] */
	} s;
	u32	w;
} spi_ctrl0_reg_t;

typedef union {
	struct {
		u32		busy	: 1;	/* [0] */
		u32		tfnf	: 1;	/* [1] */
		u32		tfe	: 1;	/* [2] */
		u32		rfne	: 1;	/* [3] */
		u32		rff	: 1;	/* [4] */
		u32		txe	: 1;	/* [5] */
		u32		dcol	: 1;	/* [6] */
		u32		reserve	: 25;	/* [31:7] */
	} s;
	u32	w;
} spi_status_reg_t;

/* ==========================================================================*/

#endif

