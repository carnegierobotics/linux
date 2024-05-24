/*
 * arch/arm/plat-ambarella/include/plat/nand.h
 *
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 * Copyright (C) 2004-2009, Ambarella, Inc.
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#ifndef __PLAT_AMBARELLA_NAND_LEGACY_H__
#define __PLAT_AMBARELLA_NAND_LEGACY_H__

/* ==========================================================================*/

#define FIO_FIFO_OFFSET			0x0000
#define FIO_OFFSET			0x1000
#define FIO_4K_OFFSET			0x30000

#define FIO_FIFO_BASE			(AHB_BASE + FIO_FIFO_OFFSET)
#define FIO_BASE			(AHB_BASE + FIO_OFFSET)
#define FIO_4K_BASE			(AHB_BASE + FIO_4K_OFFSET)

#define FIO_REG(x)			(FIO_BASE + (x))
#define FIO_4K_REG(x)			(FIO_4K_BASE + (x))

/* ==========================================================================*/

#define FIO_SUPPORT_SKIP_BLANK_ECC	1

#define NAND_ECC_RPT_NUM_SUPPORT	1

#define NAND_CUSTOM_CMD_SUPPORT	1

/* ==========================================================================*/

#define FIO_CTR_OFFSET			0x000
#define FIO_STA_OFFSET			0x004
#define FIO_DMACTR_OFFSET		0x080
#define FIO_DMAADR_OFFSET		0x084
#define FIO_DMASTA_OFFSET		0x08c
#define FIO_DSM_CTR_OFFSET              0x0a0
#define FIO_ECC_RPT_STA_OFFSET          0x0a4

#define FLASH_CTR_OFFSET		0x120
#define FLASH_CMD_OFFSET		0x124
#define FLASH_TIM0_OFFSET		0x128
#define FLASH_TIM1_OFFSET		0x12c
#define FLASH_TIM2_OFFSET		0x130
#define FLASH_TIM3_OFFSET		0x134
#define FLASH_TIM4_OFFSET		0x138
#define FLASH_TIM5_OFFSET		0x13c
#define FLASH_STA_OFFSET		0x140
#define FLASH_ID_OFFSET			0x144
#define FLASH_CFI_OFFSET		0x148
#define FLASH_LEN_OFFSET		0x14c
#define FLASH_INT_OFFSET		0x150
#define FLASH_EX_CTR_OFFSET		0x15c
#define FLASH_EX_ID_OFFSET		0x160

/* followings are for customer command, start from S3L */
#define FLASH_TIM6_OFFSET		0x164
#define FLASH_CC_OFFSET			0x170
#define FLASH_CC_WORD_OFFSET		0x174
#define FLASH_CC_DATA0_OFFSET		0x180
#define FLASH_CC_DATA1_OFFSET		0x184
#define FLASH_CC_DATA2_OFFSET		0x188
#define FLASH_CC_DATA3_OFFSET		0x18c
#define FLASH_CC_DATA4_OFFSET		0x190
#define FLASH_CC_DATA5_OFFSET		0x194
#define FLASH_CC_DATA6_OFFSET		0x198
#define FLASH_CC_DATA7_OFFSET		0x19c

#define FIO_CTR_REG			FIO_REG(0x000)
#define FIO_STA_REG			FIO_REG(0x004)
#define FIO_DMACTR_REG			FIO_REG(0x080)
#define FIO_DMAADR_REG			FIO_REG(0x084)
#define FIO_DMASTA_REG			FIO_REG(0x08c)
#define FIO_DSM_CTR_REG			FIO_REG(0x0a0)
#define FIO_ECC_RPT_STA_REG		FIO_REG(0x0a4)

#define NAND_CTR_REG			FIO_REG(0x120)
#define NAND_CMD_REG			FIO_REG(0x124)
#define NAND_TIM0_REG			FIO_REG(0x128)
#define NAND_TIM1_REG			FIO_REG(0x12c)
#define NAND_TIM2_REG			FIO_REG(0x130)
#define NAND_TIM3_REG			FIO_REG(0x134)
#define NAND_TIM4_REG			FIO_REG(0x138)
#define NAND_TIM5_REG			FIO_REG(0x13c)
#define NAND_STA_REG			FIO_REG(0x140)
#define NAND_ID_REG			FIO_REG(0x144)
#define NAND_COPY_ADDR_REG		FIO_REG(0x148)
#define NAND_LEN_REG			FIO_REG(0x14c)
#define NAND_INT_REG			FIO_REG(0x150)
#define NAND_EXT_CTR_REG		FIO_REG(0x15c)
#define NAND_EXT_ID5_REG		FIO_REG(0x160)

/* followings are for customer command, start from S3L */
#define NAND_TIM6_REG			FIO_REG(0x164)
#define NAND_CC_REG			FIO_REG(0x170)
#define NAND_CC_WORD_REG		FIO_REG(0x174)
#define NAND_CC_DATA0_REG		FIO_REG(0x180)
#define NAND_CC_DATA1_REG		FIO_REG(0x184)
#define NAND_CC_DATA2_REG		FIO_REG(0x188)
#define NAND_CC_DATA3_REG		FIO_REG(0x18c)
#define NAND_CC_DATA4_REG		FIO_REG(0x190)
#define NAND_CC_DATA5_REG		FIO_REG(0x194)
#define NAND_CC_DATA6_REG		FIO_REG(0x198)
#define NAND_CC_DATA7_REG		FIO_REG(0x19c)

#define NAND_READ_CMDWORD_REG		FIO_REG(0x154)
#define NAND_PROG_CMDWORD_REG		FIO_REG(0x158)
#define NAND_CMDWORD1(x)		((x) << 16)
#define NAND_CMDWORD2(x)		((x) & 0xff)

#define NAND_CPS1_ADDR_REG		FIO_REG(0x154)
#define NAND_CPD1_ADDR_REG		FIO_REG(0x158)
#define NAND_CPS2_ADDR_REG		FIO_REG(0x15c)
#define NAND_CPD2_ADDR_REG		FIO_REG(0x160)
#define NAND_CPS3_ADDR_REG		FIO_REG(0x164)
#define NAND_CPD3_ADDR_REG		FIO_REG(0x168)
#define NAND_NBR_SPA_REG		FIO_REG(0x16c)


/* ==========================================================================*/

/* FIO_CTR_REG */
#define FIO_CTR_DA			0x00020000
#define FIO_CTR_DR			0x00010000
#define FIO_CTR_SX			0x00000100
#define FIO_CTR_SKIP_BLANK		0x00000080
#define FIO_CTR_ECC_8BIT		0x00000060
#define FIO_CTR_ECC_6BIT		0x00000040
#define FIO_CTR_RS			0x00000010
#define FIO_CTR_SE			0x00000008
#define FIO_CTR_CO			0x00000004
#define FIO_CTR_RR			0x00000002
#define FIO_CTR_XD			0x00000001

/* FIO_STA_REG */
#define FIO_STA_SI			0x00000008
#define FIO_STA_CI			0x00000004
#define FIO_STA_XI			0x00000002
#define FIO_STA_FI			0x00000001

/* FIO_DMACTR_REG */
#define FIO_DMACTR_EN			0x80000000
#define FIO_DMACTR_RM			0x40000000
#define FIO_DMACTR_SD			0x30000000
#define FIO_DMACTR_CF			0x20000000
#define FIO_DMACTR_XD			0x10000000
#define FIO_DMACTR_FL			0x00000000
#define FIO_DMACTR_BLK_32768B		0x0c000000
#define FIO_DMACTR_BLK_16384B		0x0b000000
#define FIO_DMACTR_BLK_8192B		0x0a000000
#define FIO_DMACTR_BLK_4096B		0x09000000
#define FIO_DMACTR_BLK_2048B		0x08000000
#define FIO_DMACTR_BLK_1024B		0x07000000
#define FIO_DMACTR_BLK_256B		0x05000000
#define FIO_DMACTR_BLK_512B		0x06000000
#define FIO_DMACTR_BLK_128B		0x04000000
#define FIO_DMACTR_BLK_64B		0x03000000
#define FIO_DMACTR_BLK_32B		0x02000000
#define FIO_DMACTR_BLK_16B		0x01000000
#define FIO_DMACTR_BLK_8B		0x00000000
#define FIO_DMACTR_TS8B			0x00c00000
#define FIO_DMACTR_TS4B			0x00800000
#define FIO_DMACTR_TS2B			0x00400000
#define FIO_DMACTR_TS1B			0x00000000

/* FIO_DMASTA_REG */
#define FIO_DMASTA_RE			0x04000000
#define FIO_DMASTA_AE			0x02000000
#define FIO_DMASTA_DN			0x01000000

/* FIO_DSM_CTR_REG */
#define FIO_DSM_EN			0x80000000
#define FIO_DSM_MAJP_2KB		0x00000090
#define FIO_DSM_SPJP_64B		0x00000004
#define FIO_DSM_SPJP_128B		0x00000005

/* FIO_ECC_RPT_REG */
#define FIO_ECC_RPT_ERR			0x80000000
#define FIO_ECC_RPT_FAIL		0x40000000

/* NAND_CTR_REG */
#define NAND_CTR_A(x)			((x) << 28)
#define NAND_CTR_SA			0x08000000
#define NAND_CTR_SE			0x04000000
#define NAND_CTR_C2			0x02000000
#define NAND_CTR_P3			0x01000000
#define NAND_CTR_I4			0x00800000
#define NAND_CTR_RC			0x00400000
#define NAND_CTR_CC			0x00200000
#define NAND_CTR_CE			0x00100000
#define NAND_CTR_EC_MAIN		0x00080000
#define NAND_CTR_EC_SPARE		0x00040000
#define NAND_CTR_EG_MAIN		0x00020000
#define NAND_CTR_EG_SPARE		0x00010000
#define NAND_CTR_WP			0x00000200
#define NAND_CTR_IE			0x00000100
#define NAND_CTR_XS			0x00000080
#define NAND_CTR_SZ_8G			0x00000070
#define NAND_CTR_SZ_4G			0x00000060
#define NAND_CTR_SZ_2G			0x00000050
#define NAND_CTR_SZ_1G			0x00000040
#define NAND_CTR_SZ_512M		0x00000030
#define NAND_CTR_SZ_256M		0x00000020
#define NAND_CTR_SZ_128M		0x00000010
#define NAND_CTR_SZ_64M			0x00000000
#define NAND_CTR_4BANK			0x00000008
#define NAND_CTR_2BANK			0x00000004
#define NAND_CTR_1BANK			0x00000000
#define NAND_CTR_WD_64BIT		0x00000003
#define NAND_CTR_WD_32BIT		0x00000002
#define NAND_CTR_WD_16BIT		0x00000001
#define NAND_CTR_WD_8BIT		0x00000000

#define NAND_CTR_INTLVE			0x80000000
#define NAND_CTR_SPRBURST		0x40000000
#define NAND_CFG_STAT_ENB		0x00002000
#define NAND_CTR_2INTL			0x00001000
#define NAND_CTR_K9			0x00000800

#define NAND_CTR_WAS			0x00000400

/* NAND_CMD_REG */
#define NAND_AMB_CMD_ADDR(x)		((x) << 4)
#define NAND_AMB_CMD_NOP		0x0
#define NAND_AMB_CMD_DMA		0x1
#define NAND_AMB_CMD_RESET		0x2
#define NAND_AMB_CMD_NOP2		0x3
#define NAND_AMB_CMD_NOP3		0x4
#define NAND_AMB_CMD_NOP4		0x5
#define NAND_AMB_CMD_NOP5		0x6
#define NAND_AMB_CMD_COPYBACK		0x7
#define NAND_AMB_CMD_NOP6		0x8
#define NAND_AMB_CMD_ERASE		0x9
#define NAND_AMB_CMD_READID		0xa
#define NAND_AMB_CMD_NOP7		0xb
#define NAND_AMB_CMD_READSTATUS		0xc
#define NAND_AMB_CMD_NOP8		0xd
#define NAND_AMB_CMD_READ		0xe
#define NAND_AMB_CMD_PROGRAM		0xf

/* NAND_CC_REG */
#define NAND_CC_TERM_CE_HIGH	0x80000000
#define NAND_CC_CMD1_VAL0		0x00004000
#define NAND_CC_CMD1_VAL1		0x00008000
#define NAND_CC_CMD2_VAL0		0x00000200
#define NAND_CC_CMD2_VAL1		0x00000400
#define NAND_CC_RW_WE			0x00000080
#define NAND_CC_RW_RE			0x00000100
#define NAND_CC_WAIT_RB			0x00000020
#define NAND_CC_WAIT_TWHR		0x00000040

/* NAND_TIM0_REG (NAND mode) */
#define NAND_TIM0_TCLS(x)		((x) << 24)
#define NAND_TIM0_TALS(x)		((x) << 16)
#define NAND_TIM0_TCS(x)		((x) << 8)
#define NAND_TIM0_TDS(x)		(x)

/* NAND_TIM1_REG (NAND mode) */
#define NAND_TIM1_TCLH(x)		((x) << 24)
#define NAND_TIM1_TALH(x)		((x) << 16)
#define NAND_TIM1_TCH(x)		((x) << 8)
#define NAND_TIM1_TDH(x)		(x)

/* NAND_TIM2_REG (NAND mode) */
#define NAND_TIM2_TWP(x)		((x) << 24)
#define NAND_TIM2_TWH(x)		((x) << 16)
#define NAND_TIM2_TWB(x)		((x) << 8)
#define NAND_TIM2_TRR(x)		(x)

/* NAND_TIM3_REG (NAND mode) */
#define NAND_TIM3_TRP(x)		((x) << 24)
#define NAND_TIM3_TREH(x)		((x) << 16)
#define NAND_TIM3_TRB(x)		((x) << 8)
#define NAND_TIM3_TCEH(x)		(x)

/* NAND_TIM4_REG (NAND mode) */
#define NAND_TIM4_TRDELAY(x)		((x) << 24)
#define NAND_TIM4_TCLR(x)		((x) << 16)
#define NAND_TIM4_TWHR(x)		((x) << 8)
#define NAND_TIM4_TIR(x)		(x)

/* NAND_TIM5_REG (NAND mode) */
#define NAND_TIM5_TWW(x)		((x) << 16)
#define NAND_TIM5_TRHZ(x)		((x) << 8)
#define NAND_TIM5_TAR(x)		(x)

/* NAND_TIM6_REG (NAND mode) */
#define NAND_TIM6_TRHW(x)		((x) << 16)
#define NAND_TIM6_TADL(x)		((x) << 8)
#define NAND_TIM6_TCRL(x)		(x)

/* NAND_INT_REG (NAND mode) */
#define NAND_INT_DI			0x1

/* NAND_EXT_CTR_REG */
#define NAND_EXT_CTR_I5			0x00800000
#define NAND_EXT_CTR_SP_2X		0x00000001

/* ==========================================================================*/

#define FDMA_OFFSET			0x12000
#define FDMA_BASE			(AHB_N_BASE + FDMA_OFFSET)
#define FDMA_REG(x)			(FDMA_BASE + (x))

#define FDMA_CTR_OFFSET			0x300
#define FDMA_SRC_OFFSET			0x304
#define FDMA_DST_OFFSET			0x308
#define FDMA_STA_OFFSET			0x30c
#define FDMA_DA_OFFSET			0x380
#define FDMA_DSM_CTR_OFFSET		0x3a0
#define FDMA_INT_OFFSET			0x3f0

#define FDMA_SPR_CNT_OFFSET		0x200
#define FDMA_SPR_SRC_OFFSET		0x204
#define FDMA_SPR_DST_OFFSET		0x208
#define FDMA_SPR_STA_OFFSET		0x20c
#define FDMA_SPR_DA_OFFSET		0x280

#define FDMA_CHAN_CTR_REG		FDMA_REG(FDMA_CTR_OFFSET)
#define FDMA_CHAN_SRC_REG		FDMA_REG(FDMA_SRC_OFFSET)
#define FDMA_CHAN_DST_REG		FDMA_REG(FDMA_DST_OFFSET)
#define FDMA_CHAN_STA_REG		FDMA_REG(FDMA_STA_OFFSET)
#define FDMA_CHAN_DA_REG		FDMA_REG(FDMA_DA_OFFSET)
#define FDMA_CHAN_DSM_CTR_REG		FDMA_REG(FDMA_DSM_CTR_OFFSET)

#define FDMA_CHAN_SPR_CNT_REG		FDMA_REG(FDMA_SPR_CNT_OFFSET)
#define FDMA_CHAN_SPR_SRC_REG		FDMA_REG(FDMA_SPR_SRC_OFFSET)
#define FDMA_CHAN_SPR_DST_REG		FDMA_REG(FDMA_SPR_DST_OFFSET)
#define FDMA_CHAN_SPR_STA_REG		FDMA_REG(FDMA_SPR_STA_OFFSET)
#define FDMA_CHAN_SPR_DA_REG		FDMA_REG(FDMA_SPR_DA_OFFSET)


/* FDMA_CTR_REG */
#define FDMA_CHAN_CTR_EN		0x80000000
#define FDMA_CHAN_CTR_D			0x40000000
#define FDMA_CHAN_CTR_WM		0x20000000
#define FDMA_CHAN_CTR_RM		0x10000000
#define FDMA_CHAN_CTR_NI		0x08000000
#define FDMA_CHAN_CTR_BLK_1024B		0x07000000
#define FDMA_CHAN_CTR_BLK_512B		0x06000000
#define FDMA_CHAN_CTR_BLK_256B		0x05000000
#define FDMA_CHAN_CTR_BLK_128B		0x04000000
#define FDMA_CHAN_CTR_BLK_64B		0x03000000
#define FDMA_CHAN_CTR_BLK_32B		0x02000000
#define FDMA_CHAN_CTR_BLK_16B		0x01000000
#define FDMA_CHAN_CTR_BLK_8B		0x00000000
#define FDMA_CHAN_CTR_TS_8B		0x00C00000
#define FDMA_CHAN_CTR_TS_4B		0x00800000
#define FDMA_CHAN_CTR_TS_2B		0x00400000
#define FDMA_CHAN_CTR_TS_1B		0x00000000

/* FDMA descriptor bit fields */
#define FDMA_DESC_EOC			0x01000000
#define FDMA_DESC_WM			0x00800000
#define FDMA_DESC_RM			0x00400000
#define FDMA_DESC_NI			0x00200000
#define FDMA_DESC_TS_8B			0x00180000
#define FDMA_DESC_TS_4B			0x00100000
#define FDMA_DESC_TS_2B			0x00080000
#define FDMA_DESC_TS_1B			0x00000000
#define FDMA_DESC_BLK_1024B		0x00070000
#define FDMA_DESC_BLK_512B		0x00060000
#define FDMA_DESC_BLK_256B		0x00050000
#define FDMA_DESC_BLK_128B		0x00040000
#define FDMA_DESC_BLK_64B		0x00030000
#define FDMA_DESC_BLK_32B		0x00020000
#define FDMA_DESC_BLK_16B		0x00010000
#define FDMA_DESC_BLK_8B		0x00000000
#define FDMA_DESC_ID			0x00000004
#define FDMA_DESC_IE			0x00000002
#define FDMA_DESC_ST			0x00000001

/* FDMA_STA_REG */
#define FDMA_CHAN_STA_DM		0x80000000
#define FDMA_CHAN_STA_OE		0x40000000
#define FDMA_CHAN_STA_DA		0x20000000
#define FDMA_CHAN_STA_DD		0x10000000
#define FDMA_CHAN_STA_OD		0x08000000
#define FDMA_CHAN_STA_ME		0x04000000
#define FDMA_CHAN_STA_BE		0x02000000
#define FDMA_CHAN_STA_RWE		0x01000000
#define FDMA_CHAN_STA_AE		0x00800000
#define FDMA_CHAN_STA_DN		0x00400000

/* FDMA_CHAN_DSM_CTR_REG */
#define FDMA_DSM_EN			0x80000000
#define FDMA_DSM_MAJP_2KB		0x00000090
#define FDMA_DSM_SPJP_64B		0x00000004
#define FDMA_DSM_SPJP_128B		0x00000005

/* ==========================================================================*/

#define EC_MDSD			0 /* check main disable and spare disable */
#define EC_MDSE			1 /* check main disable and spare enable */
#define EC_MESD			2 /* check main enable and spare disable */
#define EC_MESE			3 /* check main enable and spare enable */

#define EG_MDSD			0 /* generate main disable and spare disable */
#define EG_MDSE			1 /* generate main disable and spare enable */
#define EG_MESD			2 /* generate main enable and spare disable */
#define EG_MESE			3 /* generate main enable and spare enable */

#define MAIN_ONLY		0
#define SPARE_ONLY		1
#define MAIN_ECC		2
#define SPARE_ECC		3

#define FDMA_NODC_MN_BURST_SIZE	(FDMA_CHAN_CTR_BLK_512B | FDMA_CHAN_CTR_TS_8B)
#define FDMA_NODC_SP_BURST_SIZE	(FDMA_CHAN_CTR_BLK_16B | FDMA_CHAN_CTR_TS_8B)
#define FDMA_DESC_MN_BURST_SIZE	(FDMA_DESC_BLK_512B | FDMA_DESC_TS_8B)
#define FDMA_DESC_SP_BURST_SIZE	(FDMA_DESC_BLK_16B | FDMA_DESC_TS_8B)

#endif
