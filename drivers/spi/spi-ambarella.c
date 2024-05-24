// SPDX-License-Identifier: GPL-2.0+
/*
 * SPI master driver for Ambarella SoCs.
 *
 * Copyright (C) 2016-2048, Ambarella, Inc.
 *
 */

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/delay.h>
#include <soc/ambarella/spi.h>
#include "spi-ambarella.h"

#define SPI_DUMMY_DATA			0xffff
#define SPI_MAX_QUERY_TIMES		10

#define	SPI_BUF_MAX_LEN			4096
#define	SPI_MAX_XFER_PER_MSG		32
#define	SPI_MAX_CS_NUM			8

struct ambarella_spi {
	struct device		*dev;
	struct dma_chan		*tx_dma_chan;
	struct dma_chan		*rx_dma_chan;
	struct spi_message	*msg;
	struct spi_transfer	*transfers[SPI_MAX_XFER_PER_MSG];
	struct clk		*clk;
	struct tasklet_struct	tasklet;
	void __iomem		*virt;

	u8			*tx_dma_buf;
	u8			*rx_dma_buf;
	dma_addr_t		tx_dma_phys;
	dma_addr_t		rx_dma_phys;

	u32			dma_buf_size;
	u32			phys;
	u32			clk_freq;
	u32			dma_used:1;
	u32			msb_first_only:1;
	u32			ridx, widx;
	u32			cspol;
	u32			ext_rx_dly;
	int			cs_pins[SPI_MAX_CS_NUM];
	int			xfer_id, n_xfer;
	int			cs_active;
	int			rw;
	int			irq;
};

static void ambarella_spi_next_transfer(void *args);

static int ambarella_spi_of_parse(struct platform_device *pdev,
			struct spi_master *master)
{
	struct device_node *np = pdev->dev.of_node;
	struct ambarella_spi *ambspi = spi_master_get_devdata(master);

	ambspi->clk = of_clk_get(np, 0);
	if (IS_ERR(ambspi->clk)) {
		dev_err(&pdev->dev, "Get PLL failed!\n");
		return PTR_ERR(ambspi->clk);
	}

	ambspi->clk_freq = clk_get_rate(ambspi->clk);

	return 0;
}

static void ambarella_spi_setup(struct ambarella_spi *bus,
	struct spi_device *spi, struct spi_transfer *xfer)
{
	spi_ctrl0_reg_t cr0;
	u32 ssi_clk, sckdv;

	cr0.w = 0;
	cr0.s.dfs = spi->bits_per_word - 1;
	cr0.s.scph = (spi->mode & SPI_CPHA) ? 1 : 0;
	cr0.s.scpol = (spi->mode & SPI_CPOL) ? 1 : 0;
	cr0.s.srl = (spi->mode & SPI_LOOP) ? 1 : 0;
	cr0.s.tx_lsb = (spi->mode & SPI_LSB_FIRST) ? 1 : 0;
	cr0.s.rx_lsb = (spi->mode & SPI_LSB_FIRST) ? 1 : 0;
	cr0.s.hold = 0;
	cr0.s.byte_ws = 0;
	cr0.s.fc_en = 0;
	cr0.s.residue = 1;
	cr0.s.rxd_mg = bus->ext_rx_dly;
	writel_relaxed(cr0.w, bus->virt + SPI_CTRLR0_OFFSET);

	bus->cspol = (spi->mode & SPI_CS_HIGH) ? 1 : 0;
	writel_relaxed(bus->cspol << spi->chip_select,
		bus->virt + SPI_SSIENPOLR_OFFSET);

	ssi_clk = bus->clk_freq;
	if(xfer->speed_hz > ssi_clk / 2)
		xfer->speed_hz = ssi_clk / 2;
	sckdv = (ssi_clk / xfer->speed_hz + 1) & 0xfffe;
	writel_relaxed(sckdv, bus->virt + SPI_BAUDR_OFFSET);
}

static void ambarella_spi_stop(struct ambarella_spi *bus)
{
	readl_relaxed(bus->virt + SPI_ICR_OFFSET);
	readl_relaxed(bus->virt + SPI_ISR_OFFSET);

	writel_relaxed(0, bus->virt + SPI_SSIENR_OFFSET);
	writel_relaxed(0, bus->virt + SPI_SER_OFFSET);

	if (bus->dma_used)
		writel_relaxed(0, bus->virt + SPI_DMAC_OFFSET);
}

static void ambarella_spi_prepare_transfer(struct ambarella_spi *bus)
{
	struct spi_device *spi;
	struct spi_message *msg;
	struct spi_transfer *xfer;
	const void *wbuf, *rbuf;
	spi_ctrl0_reg_t cr0;

	bus->widx = 0;
	bus->ridx = 0;

	msg = bus->msg;
	xfer = bus->transfers[bus->xfer_id];
	spi = bus->msg->spi;

	wbuf = xfer->tx_buf;
	rbuf = xfer->rx_buf;
	if (wbuf && !rbuf) {
		bus->rw	= SPI_WRITE_ONLY;
	}
	if (!wbuf && rbuf) {
		bus->rw	= SPI_READ_ONLY;
	}
	if (wbuf && rbuf) {
		bus->rw	= SPI_WRITE_READ;
	}
	msg->actual_length += xfer->len;

	ambarella_spi_setup(bus, spi, xfer);

	cr0.w = readl_relaxed(bus->virt + SPI_CTRLR0_OFFSET);
	cr0.s.tmod = SPI_WRITE_READ;
	writel_relaxed(cr0.w, bus->virt + SPI_CTRLR0_OFFSET);

	if (!bus->cs_active) {
		if (gpio_is_valid(msg->spi->cs_gpio))
			gpio_set_value(msg->spi->cs_gpio, bus->cspol);
		bus->cs_active = 1;
	}

	writel_relaxed(0, bus->virt + SPI_SSIENR_OFFSET);
	writel_relaxed(0, bus->virt + SPI_SER_OFFSET);
	if (bus->dma_used) {
		if (bus->msg->spi->bits_per_word <= 8) {
			writel_relaxed(8 - 1, bus->virt + SPI_RXFTLR_OFFSET);
		} else {
			writel_relaxed(4 - 1, bus->virt + SPI_RXFTLR_OFFSET);
		}
		writel_relaxed(0x3, bus->virt + SPI_DMAC_OFFSET);
	} else {
		disable_irq_nosync(bus->irq);
		writel_relaxed(SPI_TXEIS_MASK, bus->virt + SPI_IMR_OFFSET);
		writel_relaxed(0, bus->virt + SPI_TXFTLR_OFFSET);
		writel_relaxed(1, bus->virt + SPI_RXFTLR_OFFSET);
	}
	writel_relaxed(1 << spi->chip_select, bus->virt + SPI_SER_OFFSET);
	writel_relaxed(1, bus->virt + SPI_SSIENR_OFFSET);
}

static void ambarella_spi_start_transfer(struct ambarella_spi *bus)
{
	struct spi_device *spi;
	struct spi_transfer *xfer;
	struct dma_slave_config tx_cfg, rx_cfg;
	struct dma_async_tx_descriptor *txd, *rxd;
	u32 len, widx, ridx, xfer_len, i;
	void *wbuf, *rbuf;
	u16 tmp;

	xfer = bus->transfers[bus->xfer_id];
	spi = bus->msg->spi;

	wbuf = (void *)xfer->tx_buf;
	rbuf = (void *)xfer->rx_buf;
	widx = bus->widx;
	ridx = bus->ridx;

	len = xfer->len;
	if (!bus->dma_used) {
		if (bus->msg->spi->bits_per_word > 8)
			len >>= 1;
	}

	switch (bus->rw) {
	case SPI_WRITE_ONLY:
	case SPI_WRITE_READ:
		if (!bus->dma_used) {
			xfer_len = min_t(int, len - widx, SPI_DATA_FIFO_SIZE_16);
			for(i = 0; i < xfer_len; i++) {
				if (bus->msg->spi->bits_per_word <= 8)
					tmp = ((u8 *)wbuf)[widx++];
				else
					tmp = ((u16 *)wbuf)[widx++];
				writel_relaxed(tmp, bus->virt + SPI_DR_OFFSET);
			}
		} else {
			memcpy(bus->tx_dma_buf, xfer->tx_buf, len);
		}
		break;
	case SPI_READ_ONLY:
		if (!bus->dma_used) {
			xfer_len = min_t(int, len - ridx, SPI_DATA_FIFO_SIZE_16);
			for(i = 0; i < xfer_len; i++)
				writel_relaxed(SPI_DUMMY_DATA, bus->virt + SPI_DR_OFFSET);
		} else {
			memset(bus->tx_dma_buf, 0xFF, len);
		}

		break;
	default:
		break;
	}

	if (bus->dma_used) {
		/* RX DMA */
		rx_cfg.src_addr			= bus->phys + SPI_DR_OFFSET;
		if (spi->bits_per_word <= 8) {
			rx_cfg.src_addr_width	= DMA_SLAVE_BUSWIDTH_1_BYTE;
		} else {
			rx_cfg.src_addr_width	= DMA_SLAVE_BUSWIDTH_2_BYTES;
		}
		rx_cfg.src_maxburst		= 8;
		rx_cfg.direction		= DMA_DEV_TO_MEM;

		BUG_ON(dmaengine_slave_config(bus->rx_dma_chan, &rx_cfg) < 0);

		rxd = dmaengine_prep_slave_single(bus->rx_dma_chan, bus->rx_dma_phys, len,
			DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		BUG_ON(!rxd);

		rxd->callback		= ambarella_spi_next_transfer;
		rxd->callback_param	= bus;

		dmaengine_submit(rxd);
		dma_async_issue_pending(bus->rx_dma_chan);

		/* TX DMA */
		tx_cfg.dst_addr			= bus->phys + SPI_DR_OFFSET;
		if (spi->bits_per_word <= 8) {
			tx_cfg.dst_addr_width	= DMA_SLAVE_BUSWIDTH_1_BYTE;
		} else {
			tx_cfg.dst_addr_width	= DMA_SLAVE_BUSWIDTH_2_BYTES;
		}
		tx_cfg.dst_maxburst		= 8;
		tx_cfg.direction		= DMA_MEM_TO_DEV;

		BUG_ON(dmaengine_slave_config(bus->tx_dma_chan, &tx_cfg) < 0);

		txd = dmaengine_prep_slave_single(bus->tx_dma_chan, bus->tx_dma_phys, len,
			DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		BUG_ON (!txd);

		txd->callback		= NULL;
		txd->callback_param	= NULL;

		dmaengine_submit(txd);
		dma_async_issue_pending(bus->tx_dma_chan);
	} else {
		bus->widx = widx;
		enable_irq(bus->irq);
	}
}

static void ambarella_spi_next_transfer(void *args)
{
	struct ambarella_spi	*bus = args;
	struct spi_transfer	*xfer;

	xfer = bus->transfers[bus->xfer_id++];

	if (bus->dma_used) {
		switch (bus->rw) {
		case SPI_WRITE_READ:
		case SPI_READ_ONLY:
			memcpy(xfer->rx_buf, bus->rx_dma_buf, xfer->len);
			break;
		default:
			break;
		}
	}

	if (bus->xfer_id >= bus->n_xfer) {
		/* If cs_change=1, keep CS asserted between messages */
		if (!xfer->cs_change) {
			if (gpio_is_valid(bus->msg->spi->cs_gpio))
				gpio_set_value(bus->msg->spi->cs_gpio, bus->cspol^1);
			bus->cs_active = 0;
		}
		ambarella_spi_stop(bus);
		spi_finalize_current_message(bus->msg->spi->master);
	} else {
		/* If cs_change=1, briefly toggle CS between transfers */
		if (xfer->cs_change) {
			if (gpio_is_valid(bus->msg->spi->cs_gpio))
				gpio_set_value(bus->msg->spi->cs_gpio, bus->cspol^1);
			bus->cs_active = 0;
			udelay(10);
		}
		ambarella_spi_prepare_transfer(bus);
		ambarella_spi_start_transfer(bus);
	}
}

static void ambarella_spi_tasklet(unsigned long data)
{
	struct ambarella_spi *bus = (struct ambarella_spi *)data;
	struct spi_transfer *xfer;
	void *rbuf;
	u32 widx, ridx, len, rxflr, xfer_len;
	u32 status, finish_transfer = 0;
	u16 i, tmp;

	xfer = bus->transfers[bus->xfer_id];

	/* Wait until SPI idle */
	status = readl_relaxed(bus->virt + SPI_SR_OFFSET);
	if (status & 0x1) {
		/* Transfer is still in progress */
		for (i = 0; i < SPI_MAX_QUERY_TIMES; i++) {
			status = readl_relaxed(bus->virt + SPI_SR_OFFSET);
			if (!(status & 0x1))
				break;
		}
		if (status & 0x1) {
			tasklet_schedule(&bus->tasklet);
			return;
		}
	}

	rbuf	= (void *)xfer->rx_buf;
	widx	= bus->widx;
	ridx	= bus->ridx;
	len	= xfer->len;

	if (bus->msg->spi->bits_per_word > 8)
		len >>= 1;

	/* Fetch data from FIFO */
	switch (bus->rw) {
	case SPI_READ_ONLY:
	case SPI_WRITE_READ:
		xfer_len = len - ridx;
		rxflr = readl_relaxed(bus->virt + SPI_RXFLR_OFFSET);
		if (xfer_len > rxflr)
			xfer_len = rxflr;
		for(i = 0; i < xfer_len; i++) {
			tmp	= readl_relaxed(bus->virt + SPI_DR_OFFSET);
			if (bus->msg->spi->bits_per_word <= 8)
				((u8 *)rbuf)[ridx++] = tmp & 0xff;
			else
				((u16 *)rbuf)[ridx++] = tmp;
		}
		bus->ridx = ridx;
		break;
	default:
		break;
	}

	/* Check whether the current transfer ends */
	switch (bus->rw) {
	case SPI_WRITE_ONLY:
		if (widx == len)
			finish_transfer = 1;
		break;
	case SPI_READ_ONLY:
		if (ridx == len)
			finish_transfer = 1;
		break;
	case SPI_WRITE_READ:
		if (ridx == len && widx == len)
			finish_transfer = 1;
		break;
	default:
		break;
	}

	/* End transfer or continue filling FIFO */
	if (finish_transfer) {
		ambarella_spi_next_transfer((void *) bus);
		enable_irq(bus->irq);
	} else {
		ambarella_spi_start_transfer(bus);
	}
}

static int ambarella_spi_one_message(struct spi_master *master, struct spi_message *msg)
{
	int				err = 0;
	struct spi_device		*spi;
	struct ambarella_spi		*bus;
	struct spi_transfer		*xfer;

	spi = msg->spi;
	bus = spi_master_get_devdata(master);

	msg->status		= 0;
	msg->actual_length	= 0;
	bus->msg		= msg;

	if (list_empty(&msg->transfers)) {
		spi_finalize_current_message(master);
		goto ambarella_spi_transfer_exit;
	}

	if (spi->bits_per_word < 4 || spi->bits_per_word > 16) {
		err = -EINVAL;
		goto ambarella_spi_transfer_exit;
	}

	if (!spi->max_speed_hz) {
		err = -EINVAL;
		goto ambarella_spi_transfer_exit;
	}

	if (spi->bits_per_word > 8) {
		list_for_each_entry(xfer, &msg->transfers, transfer_list) {
			if (xfer->len & 0x1) {
				err = -EINVAL;
				goto ambarella_spi_transfer_exit;
			}
		}
	}

	if ((spi->mode & SPI_LSB_FIRST) && bus->msb_first_only) {
		dev_err(&master->dev, "SPI[%d] only supports msb first tx-rx", master->bus_num);
		err = -EINVAL;
		goto ambarella_spi_transfer_exit;
	}

	bus->n_xfer	= 0;
	bus->xfer_id	= 0;
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (bus->dma_used && xfer->len > bus->dma_buf_size) {
			dev_err(&master->dev, "DMA: transfer len %d > buf size %d!\n",
				xfer->len, bus->dma_buf_size);
			err = -EINVAL;
			goto ambarella_spi_transfer_exit;
		}
		bus->transfers[bus->n_xfer++] = xfer;
	}

	ambarella_spi_prepare_transfer(bus);
	ambarella_spi_start_transfer(bus);

ambarella_spi_transfer_exit:
	return err;
}

static irqreturn_t ambarella_spi_isr(int irq, void *dev_data)
{
	struct ambarella_spi *bus = dev_data;

	if (readl_relaxed(bus->virt + SPI_ISR_OFFSET)) {
		disable_irq_nosync(bus->irq);
		writel_relaxed(0, bus->virt + SPI_ISR_OFFSET);

		ambarella_spi_tasklet((unsigned long)bus);
	}

	return IRQ_HANDLED;
}

static int ambarella_spi_dma_channel_allocate(struct ambarella_spi *bus,
			bool dma_to_memory)
{
	struct dma_chan *dma_chan;
	dma_addr_t dma_phys;
	u8 *dma_buf;
	int ret = 0;

	dma_chan = dma_request_slave_channel(bus->dev,
					dma_to_memory ? "rx" : "tx");
	if (IS_ERR(dma_chan)) {
		ret = PTR_ERR(dma_chan);
		if (ret != -EPROBE_DEFER)
			dev_err(bus->dev,
				"Dma channel is not available: %d\n", ret);
		return ret;
	}

	dma_buf = dma_alloc_coherent(bus->dev, bus->dma_buf_size,
				&dma_phys, GFP_KERNEL);
	if (!dma_buf) {
		dev_err(bus->dev, " Not able to allocate the dma buffer\n");
		dma_release_channel(dma_chan);
		return -ENOMEM;
	}

	if (dma_to_memory) {
		bus->rx_dma_chan = dma_chan;
		bus->rx_dma_buf = dma_buf;
		bus->rx_dma_phys = dma_phys;
	} else {
		bus->tx_dma_chan = dma_chan;
		bus->tx_dma_buf = dma_buf;
		bus->tx_dma_phys = dma_phys;
	}

	return ret;
}

static void ambarella_spi_dma_channel_free(struct ambarella_spi *bus,
	bool dma_to_memory)
{
	u8 *dma_buf;
	dma_addr_t dma_phys;
	struct dma_chan *dma_chan;

	if (dma_to_memory) {
		dma_buf = bus->rx_dma_buf;
		dma_chan = bus->rx_dma_chan;
		dma_phys = bus->rx_dma_phys;
		bus->rx_dma_chan = NULL;
		bus->rx_dma_buf = NULL;
	} else {
		dma_buf = bus->tx_dma_buf;
		dma_chan = bus->tx_dma_chan;
		dma_phys = bus->tx_dma_phys;
		bus->tx_dma_buf = NULL;
		bus->tx_dma_chan = NULL;
	}
	if (!dma_chan)
		return;

	dma_free_coherent(bus->dev, bus->dma_buf_size, dma_buf, dma_phys);
	dma_release_channel(dma_chan);
}

static int ambarella_spi_hw_setup(struct spi_device *spi)
{
	struct ambarella_spi *bus = spi_master_get_devdata(spi->master);
	int err = 0;

	if (gpio_is_valid(spi->cs_gpio) &&
		(bus->cs_pins[spi->chip_select] != spi->cs_gpio)) {
		err = gpio_request(spi->cs_gpio, dev_name(&spi->dev));
		if (err < 0) {
			dev_err(&spi->dev, "can't get CS: %d\n", err);
			return -EINVAL;
		}
		bus->cspol = (spi->mode & SPI_CS_HIGH) ? 1 : 0;
		bus->cs_pins[spi->chip_select] = spi->cs_gpio;
		gpio_direction_output(spi->cs_gpio, bus->cspol^1);
		bus->cs_active = 0;
	}

	return 0;
}

static int ambarella_spi_probe(struct platform_device *pdev)
{
	struct resource				*res;
	void __iomem				*reg;
	struct ambarella_spi			*bus;
	struct spi_master			*master;
	int					i;
	int					err = 0;
	int					irq;

	reg = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(reg)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(reg);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq resource!\n");
		err = -ENODEV;
		goto exit_spi_probe;
	}

	master = spi_alloc_master(&pdev->dev, sizeof(*bus));
	if (!master) {
		err = -ENOMEM;
		goto exit_spi_probe;
	}

	bus = spi_master_get_devdata(master);
	bus->phys = res->start;
	bus->virt = reg;
	bus->irq = irq;
	bus->dev = &pdev->dev;

	for (i = 0; i < SPI_MAX_CS_NUM; i++)
		bus->cs_pins[i] = -1;

	err = ambarella_spi_of_parse(pdev, master);
	if (err < 0) {
		goto exit_free_master;
	}

	writel_relaxed(0, reg + SPI_SSIENR_OFFSET);
	writel_relaxed(0, reg + SPI_IMR_OFFSET);

	master->dev.of_node		= pdev->dev.of_node;
	master->mode_bits		= SPI_CPHA | SPI_CPOL | SPI_CS_HIGH;
	master->transfer_one_message	= ambarella_spi_one_message;
	master->setup = ambarella_spi_hw_setup;
	platform_set_drvdata(pdev, master);

	err = of_property_read_u32(pdev->dev.of_node, "amb,ext_rx_dly",
		&bus->ext_rx_dly);
	if (err < 0) {
		dev_dbg(&pdev->dev, "Missing extra-rx-delay, assuming 0!\n");
		bus->ext_rx_dly = 0;
	} else if (bus->ext_rx_dly > 0xF){
		bus->ext_rx_dly = 0xF;
	}

	/* check if hw only supports msb first tx/rx */
	if (of_find_property(pdev->dev.of_node, "amb,msb-first-only", NULL)) {
		bus->msb_first_only = 1;
		dev_info(&pdev->dev,"Only supports msb first tx-rx\n");
	} else {
		bus->msb_first_only = 0;
		master->mode_bits |= SPI_LSB_FIRST;
	}

	/* check if using dma */
	if (of_find_property(pdev->dev.of_node, "amb,dma-used", NULL)) {
		bus->dma_used = 1;
		dev_info(&pdev->dev,"DMA is used\n");

		bus->dma_buf_size = SPI_BUF_MAX_LEN;
		err = ambarella_spi_dma_channel_allocate(bus, false);
		if (err < 0)
			goto exit_free_master;
		err = ambarella_spi_dma_channel_allocate(bus, true);
		if (err < 0)
			goto exit_tx_dma_irq_free;
	} else {
		bus->dma_used = 0;
		/* request IRQ */
		err = devm_request_irq(&pdev->dev, irq, ambarella_spi_isr,
				IRQF_TRIGGER_HIGH, dev_name(&pdev->dev), bus);
		if (err)
			goto exit_free_master;

		tasklet_init(&bus->tasklet, ambarella_spi_tasklet, (unsigned long)bus);
	}

	err = spi_register_master(master);
	if (err)
		goto exit_rx_dma_free;

	dev_info(&pdev->dev, "Ambarella spi controller %d created.\r\n", master->bus_num);

	return err;

exit_rx_dma_free:
	if (bus->dma_used)
		ambarella_spi_dma_channel_free(bus, true);
exit_tx_dma_irq_free:
	if (bus->dma_used)
		ambarella_spi_dma_channel_free(bus, false);
	else
		free_irq(irq, bus);
exit_free_master:
	spi_master_put(master);
exit_spi_probe:
	return err;
}

static int ambarella_spi_remove(struct platform_device *pdev)
{
	struct spi_master		*master;
	struct ambarella_spi		*bus;
	int i;

	master	= platform_get_drvdata(pdev);
	bus	= spi_master_get_devdata(master);

	if (!bus->dma_used)
		tasklet_kill(&bus->tasklet);
	else {
		if (bus->tx_dma_chan)
			ambarella_spi_dma_channel_free(bus, false);

		if (bus->rx_dma_chan)
			ambarella_spi_dma_channel_free(bus, true);
	}

	ambarella_spi_stop(bus);

	for (i = 0; i < SPI_MAX_CS_NUM; i++) {
		if (gpio_is_valid(bus->cs_pins[i]))
			gpio_free(bus->cs_pins[i]);
	}

	spi_unregister_master(master);

	return 0;
}

#ifdef CONFIG_PM
static int ambarella_spi_suspend(struct platform_device *pdev,
	pm_message_t state)
{
	struct spi_master		*master;
	struct ambarella_spi		*bus;

	master	= platform_get_drvdata(pdev);
	bus	= spi_master_get_devdata(master);

	clk_set_rate(bus->clk, 0);

	dev_dbg(&pdev->dev, "%s exit with 0 @ %d\n", __func__, state.event);

	return 0;
}

static int ambarella_spi_resume(struct platform_device *pdev)
{
	struct spi_master		*master;
	struct ambarella_spi		*bus;

	master	= platform_get_drvdata(pdev);
	bus	= spi_master_get_devdata(master);

	clk_set_rate(bus->clk, bus->clk_freq);

	dev_dbg(&pdev->dev, "%s exit with 0\n", __func__);

	return 0;
}
#endif

int ambarella_spi_write(amba_spi_cfg_t *spi_cfg, amba_spi_write_t *spi_w)
{
	int					err = 0;
	struct spi_master			*master;
	struct spi_device			*spi;
	struct ambarella_spi			*bus;

	spi = kvzalloc(sizeof(struct spi_device), GFP_KERNEL);
	if (!spi)
		return -ENOMEM;

	master = spi_busnum_to_master(spi_w->bus_id);
	if (!master) {
		err = -EINVAL;
		goto ambarella_spi_write_exit;
	}

	bus = spi_master_get_devdata(master);

	spi->master = spi->controller = master;
	spi->bits_per_word	= spi_cfg->cfs_dfs;
	spi->mode		= spi_cfg->spi_mode;
	spi->max_speed_hz	= spi_cfg->baud_rate;
	spi->chip_select		= spi_w->cs_id;
	spi->cs_gpio		= bus->cs_pins[spi->chip_select];

	spin_lock_init(&spi->statistics.lock);

	err = spi_write_then_read(spi, spi_w->buffer, spi_w->n_size, NULL, 0);

ambarella_spi_write_exit:
	kvfree(spi);
	return err;
}
EXPORT_SYMBOL(ambarella_spi_write);

int ambarella_spi_read(amba_spi_cfg_t *spi_cfg, amba_spi_read_t *spi_r)
{
	int					err = 0;
	struct spi_master			*master;
	struct spi_device			*spi;
	struct ambarella_spi			*bus;

	spi = kvzalloc(sizeof(struct spi_device), GFP_KERNEL);
	if (!spi)
		return -ENOMEM;

	master = spi_busnum_to_master(spi_r->bus_id);
	if (!master) {
		err = -EINVAL;
		goto ambarella_spi_read_exit;
	}

	bus = spi_master_get_devdata(master);

	spi->master = spi->controller = master;
	spi->bits_per_word	= spi_cfg->cfs_dfs;
	spi->mode		= spi_cfg->spi_mode;
	spi->max_speed_hz	= spi_cfg->baud_rate;
	spi->chip_select		= spi_r->cs_id;
	spi->cs_gpio		= bus->cs_pins[spi->chip_select];

	spin_lock_init(&spi->statistics.lock);

	err = spi_write_then_read(spi, NULL, 0, spi_r->buffer, spi_r->n_size);

ambarella_spi_read_exit:
	kvfree(spi);
	return err;
}
EXPORT_SYMBOL(ambarella_spi_read);

int ambarella_spi_write_then_read(amba_spi_cfg_t *spi_cfg,
	amba_spi_write_then_read_t *spi_wtr)
{
	int					err = 0;
	struct spi_master			*master;
	struct spi_device			*spi;
	struct ambarella_spi			*bus;

	spi = kvzalloc(sizeof(struct spi_device), GFP_KERNEL);
	if (!spi)
		return -ENOMEM;

	master = spi_busnum_to_master(spi_wtr->bus_id);
	if (!master) {
		err = -EINVAL;
		goto ambarella_spi_write_then_read_exit;
	}

	bus = spi_master_get_devdata(master);

	spi->master = spi->controller = master;
	spi->bits_per_word	= spi_cfg->cfs_dfs;
	spi->mode		= spi_cfg->spi_mode;
	spi->max_speed_hz	= spi_cfg->baud_rate;
	spi->chip_select		= spi_wtr->cs_id;
	spi->cs_gpio		= bus->cs_pins[spi->chip_select];

	spin_lock_init(&spi->statistics.lock);

	err = spi_write_then_read(spi,
		spi_wtr->w_buffer, spi_wtr->w_size,
		spi_wtr->r_buffer, spi_wtr->r_size);

ambarella_spi_write_then_read_exit:
	kvfree(spi);
	return err;
}
EXPORT_SYMBOL(ambarella_spi_write_then_read);

int ambarella_spi_write_and_read(amba_spi_cfg_t *spi_cfg,
	amba_spi_write_and_read_t *spi_war)
{
	int					err = 0;
	struct spi_master			*master;
	struct spi_device			*spi;
	struct ambarella_spi			*bus;
	struct spi_message			msg;
	struct spi_transfer			x[1];

	spi = kvzalloc(sizeof(struct spi_device), GFP_KERNEL);
	if (!spi)
		return -ENOMEM;

	master = spi_busnum_to_master(spi_war->bus_id);
	if (!master) {
		err = -EINVAL;
		goto ambarella_spi_write_and_read_exit;
	}

	bus = spi_master_get_devdata(master);

	spi->master = spi->controller = master;
	spi->bits_per_word	= spi_cfg->cfs_dfs;
	spi->mode		= spi_cfg->spi_mode;
	spi->max_speed_hz	= spi_cfg->baud_rate;
	spi->chip_select		= spi_war->cs_id;
	spi->cs_gpio		= bus->cs_pins[spi->chip_select];

	spin_lock_init(&spi->statistics.lock);

	spi_message_init(&msg);
	memset(x, 0, sizeof x);
	x->tx_buf	= spi_war->w_buffer;
	x->rx_buf	= spi_war->r_buffer;
	x->len		= spi_war->n_size;
	spi_message_add_tail(x, &msg);

	err = spi_sync(spi, &msg);

ambarella_spi_write_and_read_exit:
	kvfree(spi);
	return err;
}
EXPORT_SYMBOL(ambarella_spi_write_and_read);

static const struct of_device_id ambarella_spi_dt_ids[] = {
	{.compatible = "ambarella,spi", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_spi_dt_ids);

static struct platform_driver ambarella_spi_driver = {
	.probe		= ambarella_spi_probe,
	.remove		= ambarella_spi_remove,
#ifdef CONFIG_PM
	.suspend	= ambarella_spi_suspend,
	.resume		= ambarella_spi_resume,
#endif
	.driver		= {
		.name	= "ambarella-spi",
		.of_match_table = ambarella_spi_dt_ids,
	},
};

static int ambarella_spi_init(void)
{
	return platform_driver_register(&ambarella_spi_driver);
}

static void ambarella_spi_exit(void)
{
	platform_driver_unregister(&ambarella_spi_driver);
}

module_init(ambarella_spi_init);
module_exit(ambarella_spi_exit);

MODULE_DESCRIPTION("Ambarella Media Processor SPI Bus Controller");
MODULE_AUTHOR("Zhenwu Xue, <zwxue@ambarella.com>");
MODULE_LICENSE("GPL");
