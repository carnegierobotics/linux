/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 * Copyright (C) 2004-2010, Ambarella, Inc.
 */

#ifndef __SOC_AMBARELLA_SPI_H__
#define __SOC_AMBARELLA_SPI_H__

struct ambarella_spi_cfg_info {
	u8	spi_mode;
	u8	cfs_dfs;
	u8	cs_change;
	u32	baud_rate;
};
typedef struct ambarella_spi_cfg_info amba_spi_cfg_t;

typedef struct {
	u8	bus_id;
	u8	cs_id;
	u8	*buffer;
	u32	n_size;	// u16	n_size;
} amba_spi_write_t;

typedef struct {
	u8	bus_id;
	u8	cs_id;
	u8	*buffer;
	u16	n_size;
} amba_spi_read_t;

typedef struct {
	u8	bus_id;
	u8	cs_id;
	u8	*w_buffer;
	u8	*r_buffer;
	u16	w_size;
	u16	r_size;
} amba_spi_write_then_read_t;

typedef struct {
	u8	bus_id;
	u8	cs_id;
	u8	*w_buffer;
	u8	*r_buffer;
	u16	n_size;
} amba_spi_write_and_read_t;

extern int ambarella_spi_write(amba_spi_cfg_t *spi_cfg,
	amba_spi_write_t *spi_write);
extern int ambarella_spi_read(amba_spi_cfg_t *spi_cfg,
	amba_spi_read_t *spi_read);
extern int ambarella_spi_write_then_read(amba_spi_cfg_t *spi_cfg,
	amba_spi_write_then_read_t *spi_write_then_read);
extern int ambarella_spi_write_and_read(amba_spi_cfg_t *spi_cfg,
	amba_spi_write_and_read_t *spi_write_and_read);


#endif /* __PLAT_AMBARELLA_SPI_H__ */

