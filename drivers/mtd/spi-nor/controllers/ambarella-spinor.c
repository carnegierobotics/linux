/*
 * SPI-NOR driver for Ambarella NOR-SPI controller (SPIC)
 *
 * History:
 *	2018/07/02 - [Ken He] created file
 *
 * Copyright (C) 2018-2028, Ambarella, Inc.
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

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include "ambarella-spinor.h"

#define AMBARELLA_SPINOR_DMA_BUFFER_SIZE (8192)
#define SPINOR_BLK_DMA_SIZE	(32)

#define DMA_TIMEOUT		(msecs_to_jiffies(1000))

struct spinor_ctrl {
	u32 len;
	u8 *buf;
	u8 lane;
	u8 is_dtr : 1;
	u8 is_read : 1; /* following are only avaiable for data */
	u8 is_io : 1;
	u8 is_dma : 1;
};

struct ambarella_spinor {
	struct device *dev;
	struct clk *clk;
	u32 clk_rate;
	void __iomem *regbase;
	spinlock_t lock;
	struct spi_nor nor;
	u32 phy_base;

	u8 *dmabuf;
	dma_addr_t dmaaddr;
	/* DMA related objects */
	struct dma_chan *tx_dma_chan;
	struct dma_chan *rx_dma_chan;

	dma_addr_t tx_dma_phys;
	dma_addr_t rx_dma_phys;
	struct completion dma_complete;
	struct completion cmd_complete;
};

static void amba_spinor_release_dma(struct ambarella_spinor *amba_spinor);

static irqreturn_t amba_spinor_irq_handler(int irq, void *dev_id)
{
	struct ambarella_spinor *amba_spinor = dev_id;
	unsigned long	flags;
	unsigned int	irq_status;

	/* Read interrupt status */
	irq_status = readl_relaxed(amba_spinor->regbase + SPINOR_RAWINTR_OFFSET);

	if (irq_status) {
		spin_lock_irqsave(&amba_spinor->lock, flags);
		/* Clear interrupt */
		writel_relaxed(irq_status, amba_spinor->regbase + SPINOR_CLRINTR_OFFSET);

		irq_status &= SPINOR_INTR_ALL;
		if (irq_status & SPINOR_INTR_DATALENREACH){
			complete(&amba_spinor->cmd_complete);
		}

		spin_unlock_irqrestore(&amba_spinor->lock, flags);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}


static int amba_spinor_wait_for_cmd(struct ambarella_spinor *amba_spinor)
{
	int ret;

	ret = wait_for_completion_timeout(&amba_spinor->cmd_complete, DMA_TIMEOUT);
	if (!ret) {
		dev_err(amba_spinor->dev, "command timed out\n");
		ret = -ETIMEDOUT;
	}
	return ret;
}

static void amba_spinor_dma_complete(void *param)
{
	struct ambarella_spinor *amba_spinor = param;

	writel(0x0, amba_spinor->regbase + SPINOR_DMACTRL_OFFSET);
	complete(&amba_spinor->dma_complete);
}


static int amba_spinor_dma_transfer(struct ambarella_spinor *amba_spinor,
		int len, enum dma_data_direction dir)
{
	struct dma_async_tx_descriptor *desc = NULL;
	struct dma_chan *chan;
	struct dma_slave_config config;
	enum dma_transfer_direction tr_dir;
	dma_cookie_t cookie;
	int ret = 0;
	u32 rxtx_enable = 0;

	dev_dbg(amba_spinor->dev, "dma tr(%#.2x): len:%x\n", dir, len);

	memset(&config, 0, sizeof(config));

	if (dir == DMA_FROM_DEVICE){
		chan = amba_spinor->rx_dma_chan;
		tr_dir = DMA_DEV_TO_MEM;
		config.src_addr = amba_spinor->phy_base + SPINOR_RXDATA_OFFSET;
		config.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		config.src_maxburst = 32;
		rxtx_enable = SPINOR_DMACTRL_RXEN;
	} else {
		chan = amba_spinor->tx_dma_chan;
		tr_dir = DMA_MEM_TO_DEV;
		config.dst_addr = amba_spinor->phy_base + SPINOR_TXDATA_OFFSET;
		config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		config.dst_maxburst = 32;
		rxtx_enable = SPINOR_DMACTRL_TXEN;
	}

	config.direction = tr_dir;

	BUG_ON(dmaengine_slave_config(chan, &config) < 0);
	desc = dmaengine_prep_slave_single(chan, amba_spinor->dmaaddr, len, tr_dir,
			DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -ENOMEM;

	desc->callback = amba_spinor_dma_complete;
	desc->callback_param = amba_spinor;
	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		ret = dma_submit_error(cookie);
		dev_warn(amba_spinor->dev, "DMA submit failed \n");
		return ret;
	}

	reinit_completion(&amba_spinor->dma_complete);
	dma_async_issue_pending(chan);

	writel(rxtx_enable, amba_spinor->regbase + SPINOR_DMACTRL_OFFSET);

	return ret;
}

static int amba_spinor_send_cmd(struct ambarella_spinor *amba_spinor,
		struct spinor_ctrl *cmd, struct spinor_ctrl *addr,
		struct spinor_ctrl *data, struct spinor_ctrl *dummy)
{
	u32 reg_length = 0, reg_ctrl = 0, val, i;
	int ret = 0;

	/* setup basic info */
	if (cmd != NULL) {
		reg_length |= SPINOR_LENGTH_CMD(cmd->len);

		reg_ctrl |= cmd->is_dtr ? SPINOR_CTRL_CMDDTR : 0;
		switch(cmd->lane) {
		case 8:
			reg_ctrl |= SPINOR_CTRL_CMD8LANE;
			break;
		case 4:
			reg_ctrl |= SPINOR_CTRL_CMD4LANE;
			break;
		case 2:
			reg_ctrl |= SPINOR_CTRL_CMD2LANE;
			break;
		case 1:
		default:
			reg_ctrl |= SPINOR_CTRL_CMD1LANE;
			break;
		}
	} else {
		return -EINVAL;
	}

	if (addr != NULL) {
		reg_length |= SPINOR_LENGTH_ADDR(addr->len);

		reg_ctrl |= addr->is_dtr ? SPINOR_CTRL_ADDRDTR : 0;
		switch(addr->lane) {
		case 4:
			reg_ctrl |= SPINOR_CTRL_ADDR4LANE;
			break;
		case 2:
			reg_ctrl |= SPINOR_CTRL_ADDR2LANE;
			break;
		case 1:
		default:
			reg_ctrl |= SPINOR_CTRL_ADDR1LANE;
			break;
		}
	}

	if (data != NULL) {
		if (data->len > SPINOR_MAX_DATA_LENGTH) {
			dev_err(amba_spinor->dev, "spinor: data length is too large.\n");
			return -ENOMEM;
		}

		reg_length |= SPINOR_LENGTH_DATA(data->len);

		reg_ctrl |= data->is_dtr ? SPINOR_CTRL_DATADTR : 0;
		switch(data->lane) {
		case 8:
			reg_ctrl |= SPINOR_CTRL_DATA8LANE;
			break;
		case 4:
			reg_ctrl |= SPINOR_CTRL_DATA4LANE;
			break;
		case 2:
			reg_ctrl |= SPINOR_CTRL_DATA2LANE;
			break;
		case 1:
		default:
			reg_ctrl |= SPINOR_CTRL_DATA1LANE;
			break;
		}

		if (data->is_read)
			reg_ctrl |= SPINOR_CTRL_RDEN;
		else
			reg_ctrl |= SPINOR_CTRL_WREN;

		if (!data->is_io)
			reg_ctrl |= SPINOR_CTRL_RXLANE_TXRX;

		if (data->is_dma && data->is_read && data->len < SPINOR_BLK_DMA_SIZE)
			data->is_dma = 0;
	}

	if (dummy != NULL) {
		reg_length |= SPINOR_LENGTH_DUMMY(dummy->len);
		reg_ctrl |= dummy->is_dtr ? SPINOR_CTRL_DUMMYDTR : 0;
	}

	writel_relaxed(reg_length, amba_spinor->regbase + SPINOR_LENGTH_OFFSET);
	writel_relaxed(reg_ctrl, amba_spinor->regbase + SPINOR_CTRL_OFFSET);

	reinit_completion(&amba_spinor->cmd_complete);

	/* setup cmd id */
	val = 0;
	for (i = 0; i < cmd->len; i++)
		val |= (cmd->buf[i] << (i << 3));
	writel_relaxed(val, amba_spinor->regbase + SPINOR_CMD_OFFSET);

	/* setup address */
	if (addr) {
		val = 0;
		for (i = 0; i < addr->len; i++) {
			if (i >= 4)
				break;
			val |= (addr->buf[i] << (i << 3));
		}
		writel_relaxed(val, amba_spinor->regbase + SPINOR_ADDRLO_OFFSET);

		val = 0;
		for (; i < addr->len; i++)
			val |= (addr->buf[i] << ((i - 4) << 3));
		writel_relaxed(val, amba_spinor->regbase + SPINOR_ADDRHI_OFFSET);
	}
	/* set dmactrl usage 0 as default */
	writel(0, amba_spinor->regbase + SPINOR_DMACTRL_OFFSET);

	/* setup dma if data phase is existed and dma is required.
	 * Note: for READ, dma will just transfer the length multiple of
	 * 32Bytes, the residual data in FIFO need to be read manually. */
	if (data != NULL && data->is_dma) {
		if (data->is_read) {
			amba_spinor_dma_transfer(amba_spinor, data->len, DMA_FROM_DEVICE);
		} else {
			amba_spinor_dma_transfer(amba_spinor, data->len, DMA_TO_DEVICE);
		}
	} else if (data != NULL && !data->is_read) {
		if (data->len > 0x100) {
			dev_err(amba_spinor->dev, "spinor: tx length exceeds fifo size.\n");
			return -1;
		}
		for (i = 0; i < data->len; i++) {
			writeb(data->buf[i], amba_spinor->regbase + SPINOR_TXDATA_OFFSET);
		}
	}

	/* start tx/rx transaction */
	writel_relaxed(0x1, amba_spinor->regbase + SPINOR_START_OFFSET);

	ret = amba_spinor_wait_for_cmd(amba_spinor);
	if (ret < 0)
		return ret;

	if (data != NULL && data->is_dma) {
		ret = wait_for_completion_timeout(&amba_spinor->dma_complete, DMA_TIMEOUT);
		if (ret == 0) {
			ret = -ETIMEDOUT;
			dev_err(amba_spinor->dev, "dma wait_for_completion_timeout\n");
			if(data->is_read) {
				dev_err(amba_spinor->dev, "dma read wait_for_completion_timeout\n");
			} else
				dev_err(amba_spinor->dev, "dma write wait_for_completion_timeout\n");

			return ret;
		}
	}

	return 0;
}


static void amba_spinor_release_dma(struct ambarella_spinor *amba_spinor)
{
	if (amba_spinor->tx_dma_chan) {
		dma_release_channel(amba_spinor->tx_dma_chan);
		amba_spinor->tx_dma_chan = NULL;
	}

	if (amba_spinor->rx_dma_chan) {
		dma_release_channel(amba_spinor->rx_dma_chan);
		amba_spinor->rx_dma_chan = NULL;
	}
}

static int amba_spinor_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf, size_t len)
{
	struct ambarella_spinor *amba_spinor = nor->priv;
	struct spinor_ctrl cmd, data;
	int i, rval;

	dev_dbg(amba_spinor->dev, "read_reg(%#.2x): buf:%p len:%lx\n", opcode, buf, len);

	cmd.buf = &opcode;
	cmd.len = 1;
	cmd.lane = 1;
	cmd.is_dtr = 0;

	data.buf = buf;
	data.len = len;
	data.lane = 2;
	data.is_read = 1;
	data.is_io = 0;
	data.is_dtr = 0;
	data.is_dma = 0;

	rval = amba_spinor_send_cmd(amba_spinor, &cmd, NULL, &data, NULL);
	if (rval < 0)
		return rval;

	for (i = 0; i < len; i++)
		buf[i] = readb(amba_spinor->regbase + SPINOR_RXDATA_OFFSET);

	return 0;
}

static int amba_spinor_write_reg(struct spi_nor *nor, u8 opcode, const u8 *buf, size_t len)
{
	struct ambarella_spinor *amba_spinor = nor->priv;
	struct spinor_ctrl cmd, data;
	int rval;

	dev_dbg(amba_spinor->dev, "write_reg(%#.2x): buf:%p len:%lx\n", opcode, buf, len);

	cmd.buf = &opcode;
	cmd.len = 1;
	cmd.lane = 1;
	cmd.is_dtr = 0;

	data.buf = (void *)buf;
	data.len = len;
	data.lane = 1;
	data.is_dtr = 0;
	data.is_read = 0;
	data.is_io = 0;
	data.is_dma = 0;

	rval = amba_spinor_send_cmd(amba_spinor, &cmd, NULL, &data, NULL);
	if (rval < 0)
		return rval;

	return 0;
}

static u8 nor_get_proto(enum spi_nor_protocol proto)
{
	u8 data_lane;

	switch (proto) {
	case SNOR_PROTO_1_1_2:
	case SNOR_PROTO_1_2_2:
		data_lane = 2;
		break;
	case SNOR_PROTO_1_1_4:
	case SNOR_PROTO_1_4_4:
		data_lane = 4;
		break;
	case SNOR_PROTO_1_1_8:
		data_lane = 8;
		break;
	case SNOR_PROTO_1_1_1:
	default:
		data_lane = 1;
		break;
	}

	return data_lane;
}

static ssize_t amba_spinor_read(struct spi_nor *nor, loff_t from, size_t len,
			      u_char *buf)
{
	struct ambarella_spinor *amba_spinor = nor->priv;
	struct spinor_ctrl cmd;
	struct spinor_ctrl addr;
	struct spinor_ctrl data;
	struct spinor_ctrl dummy;
	int ret;
	int i;
	u32 tail, bulk, dma_blk;
	u8 cmd_id, is_dma, data_lane;
	size_t offset = 0;
	size_t remain;
	loff_t trans_addr;

	dev_dbg(amba_spinor->dev, "read(%#.2x): buf:%p from:%#.8x len:%#zx\n",
			nor->read_opcode, buf, (u32)from, len);

	dma_blk = len / AMBARELLA_SPINOR_DMA_BUFFER_SIZE;
	bulk = len / SPINOR_BLK_DMA_SIZE;

	memset(&cmd, 0, sizeof(cmd));
	memset(&addr, 0, sizeof(addr));
	memset(&data, 0, sizeof(data));
	memset(&dummy, 0, sizeof(dummy));

	cmd_id = nor->read_opcode;
	cmd.buf = &cmd_id;
	cmd.len = 1;
	cmd.lane = 1;

	addr.buf = (u8*)&from;
	addr.len = nor->addr_width;
	addr.lane = 1;
	addr.is_dtr = 0;

	is_dma = bulk ? 1 : 0;

	data.buf = buf;
	data_lane = nor_get_proto(nor->read_proto);
	data.lane = max_t(u8, data_lane, 2);
	data.is_dtr = 0;
	data.is_read = 1;
	data.is_io = (data_lane == 1) ? 0 : 1;
	data.is_dma = is_dma;

	dummy.len = nor->read_dummy;
	dummy.is_dtr = 0;

	if (dma_blk) {
		for (i = 0; i < dma_blk; i++) {
			offset = i * AMBARELLA_SPINOR_DMA_BUFFER_SIZE;
			trans_addr = from + offset;
			addr.buf = (u8*)&trans_addr;
			data.len = AMBARELLA_SPINOR_DMA_BUFFER_SIZE;

			ret = amba_spinor_send_cmd(amba_spinor, &cmd, &addr, &data, &dummy);
			if (ret)
				return ret;

			memcpy(buf + offset, amba_spinor->dmabuf, AMBARELLA_SPINOR_DMA_BUFFER_SIZE);
		}
	}

	offset = dma_blk * AMBARELLA_SPINOR_DMA_BUFFER_SIZE;
	remain = len - offset;

	tail = remain % SPINOR_BLK_DMA_SIZE;
	bulk = remain / SPINOR_BLK_DMA_SIZE;

	if (remain) {
		if (bulk) {
			trans_addr = from + offset;
			addr.buf = (u8*)&trans_addr;
			data.len = remain - tail;
			data.is_dma = 1;

			ret = amba_spinor_send_cmd(amba_spinor, &cmd, &addr, &data, &dummy);

			if (ret)
				return ret;

			if (bulk)
				memcpy(buf + offset, amba_spinor->dmabuf, remain-tail);

		}
		offset += bulk * SPINOR_BLK_DMA_SIZE;

		if (tail) {
			trans_addr = from + offset;
			addr.buf = (u8*)&trans_addr;
			data.len = tail;
			data.is_dma = 0;

			ret = amba_spinor_send_cmd(amba_spinor, &cmd, &addr, &data, &dummy);

			if (ret)
				return ret;

			for (i = 0; i < tail; i++){
				*(buf+offset+i) = readb(amba_spinor->regbase + SPINOR_RXDATA_OFFSET);
			}
		}
	}
	return len;
}

/* write function need to consider the page bound issue or not */
/* mtd layer should consider and handle the page bound issue   */
/* So it may could ignore this */
static ssize_t amba_spinor_write(struct spi_nor *nor, loff_t to, size_t len,
			       const u_char *buf)
{
	struct ambarella_spinor *amba_spinor = nor->priv;
	struct spinor_ctrl cmd;
	struct spinor_ctrl addr;
	struct spinor_ctrl data;

	int ret, i;
	u32 tail = 0;
	u8 cmd_id;

	dev_dbg(amba_spinor->dev, "write(%#.2x): buf:%p to:%#.8x len:%#zx\n",
		nor->program_opcode, buf, (u32)to, len);

	i = to % (nor->page_size);
	if ((i + len) > nor->page_size)
		dev_err(amba_spinor->dev, "write data exceed the page bound \n");
	BUG_ON((i + len) > nor->page_size);

	tail = len % SPINOR_BLK_DMA_SIZE;

	memset(&cmd, 0, sizeof(cmd));
	memset(&addr, 0, sizeof(addr));
	memset(&data, 0, sizeof(data));

	cmd_id = nor->program_opcode;
	cmd.buf = &cmd_id;
	cmd.len = 1;
	cmd.lane = 1;

	addr.buf = (u8*)&to;
	addr.len = nor->addr_width;
	addr.lane = 1;
	addr.is_dtr = 0;

	data.buf = (void *)buf;
	data.len = len;
	data.lane = 1;
	data.is_dtr = 0;
	data.is_read = 0;
	data.is_io = 0;
	data.is_dma = tail ? 0 : 1;

	if (tail == 0)
		memcpy(amba_spinor->dmabuf, buf, len);

	ret = amba_spinor_send_cmd(amba_spinor, &cmd, &addr, &data, NULL);
	if (ret)
		return ret;

	return len;
}

static int amba_spinor_erase(struct spi_nor *nor, loff_t offs)
{
	struct ambarella_spinor *amba_spinor = nor->priv;
	struct spinor_ctrl cmd;
	struct spinor_ctrl addr;
	u8 cmd_id;
	int ret;

	dev_dbg(amba_spinor->dev, "erase(%#.2x): to:%#.8x \n",
		nor->erase_opcode, (u32)offs);

	cmd_id = nor->erase_opcode;
	cmd.buf = &cmd_id;
	cmd.len = 1;
	cmd.lane = 1;
	cmd.is_dtr = 0;

	addr.buf = (u8 *)&offs;
	addr.len = nor->addr_width;
	addr.lane = 1;
	addr.is_dtr = 0;

	ret = amba_spinor_send_cmd(amba_spinor, &cmd, &addr, NULL, NULL);
	return ret;
}

static const struct spi_nor_controller_ops amba_controller_ops = {
	.read_reg = amba_spinor_read_reg,
	.write_reg = amba_spinor_write_reg,
	.read = amba_spinor_read,
	.write = amba_spinor_write,
	.erase = amba_spinor_erase,
};

/* We use the 1_1_2 for read as the best match */
/* because some quad spinor could not work normal */
/* on our platform */
static int amba_spinor_setup_flash(struct ambarella_spinor *amba_spinor,
				 struct device_node *np)
{
	struct spi_nor_hwcaps hwcaps = {
		.mask = SNOR_HWCAPS_READ |
			SNOR_HWCAPS_READ_FAST |
			SNOR_HWCAPS_READ_1_1_2 |
			SNOR_HWCAPS_READ_1_1_4 |
			SNOR_HWCAPS_READ_1_1_8 |
			SNOR_HWCAPS_PP,
	};

	int ret;
	struct spi_nor *nor;
	struct mtd_info *mtd;

	nor = &amba_spinor->nor;
	mtd = &nor->mtd;
	nor->dev = amba_spinor->dev;
	nor->priv = amba_spinor;
	nor->controller_ops = &amba_controller_ops;
	spi_nor_set_flash_node(nor, np);

	nor->mtd.name = "amba_nor";

	ret = spi_nor_scan(nor, NULL, &hwcaps);
	if (ret) {
		dev_err(amba_spinor->dev, "device scan failed\n");
		return ret;
	}

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(amba_spinor->dev, "mtd device parse failed\n");
		return ret;
	}

	return 0;
}

static int amba_spinor_setup_dma(struct ambarella_spinor *amba_spinor)
{
	struct device *dev = amba_spinor->dev;
	int ret;

	/* We can only either use DMA for both TX and RX or not use it at all */
	amba_spinor->tx_dma_chan = dma_request_slave_channel(dev, "tx");
	if (!amba_spinor->tx_dma_chan) {
		dev_err(dev, "failed to request tx dma channenl \n");
		ret = -ENODEV;
		goto err_out;
	}

	amba_spinor->rx_dma_chan = dma_request_slave_channel(dev, "rx");
	if (!amba_spinor->rx_dma_chan) {
		dev_err(dev, "failed to request rx dma channenl \n");
		ret = -ENODEV;
		goto err_out;
	}

	amba_spinor->dmabuf = dma_alloc_coherent(dev,
		AMBARELLA_SPINOR_DMA_BUFFER_SIZE,
		&amba_spinor->dmaaddr, GFP_KERNEL);
	if (amba_spinor->dmabuf == NULL) {
		dev_err(dev, "dma_alloc_coherent failed!\n");
		ret = -ENOMEM;
		goto err_out;
	}
	BUG_ON(amba_spinor->dmaaddr & 0x7);

	return 0;
err_out:
	amba_spinor_release_dma(amba_spinor);
	return ret;
}

static void amba_spinor_init(struct ambarella_spinor *amba_spinor)
{
	u32 rval;

	writel_relaxed(SPINOR_INTR_ALL,
					amba_spinor->regbase + SPINOR_INTRMASK_OFFSET);

	writel_relaxed(SPINOR_INTR_ALL,
					amba_spinor->regbase + SPINOR_CLRINTR_OFFSET);

	/* reset the FIFO */
	writel_relaxed(0x1, amba_spinor->regbase + SPINOR_TXFIFORST_OFFSET);
	writel_relaxed(0x1, amba_spinor->regbase + SPINOR_RXFIFORST_OFFSET);

	/* after reset fifo, the 0x28 will become 0x10,
    * so , read REG200 times to clear the 0x28,  this is a bug in hardware
    */
	while (readl_relaxed(amba_spinor->regbase + SPINOR_RXFIFOLV_OFFSET) != 0) {
		rval = readl_relaxed(amba_spinor->regbase + SPINOR_RXDATA_OFFSET);
	}

	writel_relaxed(31, amba_spinor->regbase + SPINOR_TXFIFOTHLV_OFFSET);
	writel_relaxed(31, amba_spinor->regbase + SPINOR_RXFIFOTHLV_OFFSET);
}

static int ambarella_spinor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *flash_np = dev->of_node;
	struct ambarella_spinor *amba_spinor;
	struct resource *res;
	int ret, irq;

	amba_spinor = devm_kzalloc(dev, sizeof(*amba_spinor), GFP_KERNEL);
	if (!amba_spinor)
		return -ENOMEM;

	platform_set_drvdata(pdev, amba_spinor);
	amba_spinor->dev = dev;

	amba_spinor->regbase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(amba_spinor->regbase)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(amba_spinor->regbase);
	}

	amba_spinor->phy_base = res->start;

	amba_spinor->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(amba_spinor->clk)) {
		dev_err(dev, "ambarella spinor controller clock not found\n");
		return PTR_ERR(amba_spinor->clk);
	}

	amba_spinor->clk_rate = clk_get_rate(amba_spinor->clk);
	if (!amba_spinor->clk_rate) {
		dev_err(dev, "can not get clk rate\n");
		return -EINVAL;
	}
	dev_info(dev, "spinor clock is %d \n", amba_spinor->clk_rate);

	/* We set the clock divider from the bootloader */
	/* and do not change the spinor clock setting any more */
#if 0
	ret = clk_prepare_enable(amba_spinor->clk);
	if (ret) {
		dev_err(dev, "can not enable spinor controller clock\n");
		return ret;
	}
#endif
	spin_lock_init(&amba_spinor->lock);
	init_completion(&amba_spinor->cmd_complete);
	init_completion(&amba_spinor->dma_complete);

	/* Initialize controller */
	amba_spinor_init(amba_spinor);
	amba_spinor_setup_dma(amba_spinor);

	/* Get irq for the device */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "failed to get the irq: %d\n", irq);
		ret = -ENODEV;
		goto irq_failed;
	}

	ret = devm_request_irq(dev, irq,
		amba_spinor_irq_handler, IRQF_TRIGGER_HIGH,
		dev_name(dev), amba_spinor);
	if (ret) {
		dev_err(dev, "failed to request irq: %d\n", ret);
		ret = -ENODEV;
		goto irq_failed;
	}

	ret = amba_spinor_setup_flash(amba_spinor, flash_np);
	if (ret) {
		dev_err(dev, "setup flash chip failed 0x%x\n", ret);
		goto setup_failed;
	}

	dev_info(dev, "amba spinor controller \n");
	return 0;

irq_failed:
	dev_info(dev, "amba spinor controller irq failed\n");
setup_failed:
	dev_info(dev, "amba spinor controller set up flash failed\n");
	return ret;
}

static int ambarella_spinor_remove(struct platform_device *pdev)
{
	struct ambarella_spinor *amba_spinor = platform_get_drvdata(pdev);

	amba_spinor_release_dma(amba_spinor);
	dma_free_coherent(amba_spinor->dev,
			AMBARELLA_SPINOR_DMA_BUFFER_SIZE,
			amba_spinor->dmabuf, amba_spinor->dmaaddr);
	mtd_device_unregister(&amba_spinor->nor.mtd);

	return 0;
}

static const struct of_device_id ambarella_spinor_match[] = {
	{.compatible = "ambarella,spinor"},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ambarella_spinor_match);

static struct platform_driver ambarella_spinor_driver = {
	.probe	= ambarella_spinor_probe,
	.remove	= ambarella_spinor_remove,
	.driver	= {
		.name = "ambarella-spinor",
		.of_match_table = ambarella_spinor_match,
	},
};
module_platform_driver(ambarella_spinor_driver);

MODULE_DESCRIPTION("Ambarella SPI NOR Flash Controller driver");
MODULE_AUTHOR("Ken He <jianhe@ambarella.com>");
MODULE_LICENSE("GPL v2");
