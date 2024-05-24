// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 ambarella, Inc.
 *
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/of.h>

/* ==========================================================================*/

#define IDCS_ENABLE			0x00
#define IDCS_CONTROL			0x04
#define IDCS_DATA			0x08
#define IDCS_STATUS			0x0C
#define IDCS_FIFO_CNT			0x10
#define IDCS_RX_FIFO_TH			0x14
#define IDCS_TX_FIFO_TH			0x18
#define IDCS_T_HD			0x1C
#define IDCS_SLAVE_ADDR			0x20
#define IDCS_SCL_TIMER			0x24
#define IDCS_TIMEOUT_STATUS		0x28
#define IDCS_DEBUG			0x2C
#define IDCS_ACK_CONTROL		0x30
#define IDCS_IRQ			0x34
#define IDCS_FAULT_INJECT		0x38

#define IDCS_ENABLE_BIT			BIT(0)

#define IDCS_CONTROL_RESET		BIT(7)
#define IDCS_CONTROL_IRQ_CLR		BIT(6)
#define IDCS_CONTROL_FIFO_CLR		BIT(5)
#define IDCS_CONTROL_TIMEOUT		BIT(4)
#define IDCS_CONTROL_IRQ_FIFO_TH	BIT(3)
#define IDCS_CONTROL_IRQ_P_SR		BIT(2)
#define IDCS_CONTROL_IRQ_ENABLE		BIT(1)
#define IDCS_CONTROL_ACK		BIT(0)
#define IDCS_CONTROL_RESET_ALL (IDCS_CONTROL_RESET \
				| IDCS_CONTROL_IRQ_CLR \
				| IDCS_CONTROL_FIFO_CLR)

#define IDCS_DATA_FST_BYTE		BIT(8)

#define IDCS_STATUS_TIMEOUT		BIT(8)
#define IDCS_STATUS_STOP		BIT(7)
#define IDCS_STATUS_RSTART		BIT(6)
#define IDCS_STATUS_FIFO_TH_VLD		BIT(5)
#define IDCS_STATUS_SEL			BIT(4)
#define IDCS_STATUS_GENERAL_CALL	BIT(3)
#define IDCS_STATUS_FIFO_FULL		BIT(2)
#define IDCS_STATUS_FIFO_EMPTY		BIT(1)
#define IDCS_STATUS_RX_TX_STATE		BIT(0)

#define IDCS_DEBUG_TIMEOUT_NACK		BIT(12)
#define IDCS_DEBUG_INT_BUSY		BIT(11)
#define IDCS_DEBUG_RX_FIFO_BUSY		BIT(10)
#define IDCS_DEBUG_RX_FIFO_FULL		BIT(9)
#define IDCS_DEBUG_TX_FIFO_EMPTY	BIT(8)
#define IDCS_DEBUG_SCL_HOLD_LOW		BIT(7)
#define IDCS_DEBUG_BIT_CNT_MASK		GENMASK(6, 3)
#define IDCS_DEBUG_STATE_MASK		GENMASK(2, 0)

#define AMBARELLA_IDCS_RX_TH		(0)
#define AMBARELLA_IDCS_TX_TH		(0)

/* ==========================================================================*/

struct ambarella_i2cs_dev {
	unsigned char __iomem *base;
	struct i2c_adapter adapter;
	struct device *dev;

	struct i2c_client *slave;
	int irq;
	int after_start;
};

static inline void ambarella_i2cs_hw_init(struct ambarella_i2cs_dev *i2cs_dev)
{
	u32 val;

	dev_dbg(i2cs_dev->dev, "%s\n", __func__);

	i2cs_dev->after_start = 1;
	/* reset all */
	writel_relaxed(IDCS_CONTROL_RESET_ALL, i2cs_dev->base + IDCS_CONTROL);
	/* set strech timer */
	writel_relaxed(0xFFFFFFFF, i2cs_dev->base + IDCS_SCL_TIMER);
	/* set slave addr */
	writel_relaxed(i2cs_dev->slave->addr, i2cs_dev->base + IDCS_SLAVE_ADDR);
	/* set rx/tx threshold */
	writel_relaxed(AMBARELLA_IDCS_RX_TH, i2cs_dev->base + IDCS_RX_FIFO_TH);
	writel_relaxed(AMBARELLA_IDCS_TX_TH, i2cs_dev->base + IDCS_TX_FIFO_TH);
	/* enable irq */
	val = IDCS_CONTROL_IRQ_FIFO_TH | IDCS_CONTROL_IRQ_P_SR | IDCS_CONTROL_IRQ_ENABLE;
	writel_relaxed(val, i2cs_dev->base + IDCS_CONTROL);
	/* enable controller */
	writel_relaxed(IDCS_ENABLE_BIT, i2cs_dev->base + IDCS_ENABLE);
}

static irqreturn_t ambarella_i2cs_irq(int irqno, void *dev_id)
{
	struct ambarella_i2cs_dev *i2cs_dev = dev_id;
	struct i2c_client *slave = i2cs_dev->slave;
	u32 control, status, cnt, reg, debug;
	u8 value;

	control = readl_relaxed(i2cs_dev->base + IDCS_CONTROL);
	status = readl_relaxed(i2cs_dev->base + IDCS_STATUS);
	debug = readl_relaxed(i2cs_dev->base + IDCS_DEBUG);
	cnt = readl_relaxed(i2cs_dev->base + IDCS_FIFO_CNT);

	dev_dbg(i2cs_dev->dev, "control=0x%x status=0x%x debug=0x%x cnt=0x%x\n",
		control, status, debug,cnt);
	if(status & IDCS_STATUS_RX_TX_STATE) {
		/* slave-receiver */
		for(; cnt > 0; cnt--)
		{
			reg = readl_relaxed(i2cs_dev->base + IDCS_DATA);
			value = reg & 0xFF;
			if(reg & IDCS_DATA_FST_BYTE)
				i2c_slave_event(slave, I2C_SLAVE_WRITE_REQUESTED, &value);
			i2c_slave_event(slave, I2C_SLAVE_WRITE_RECEIVED, &value);
		}
	} else {
		/* slave-transmitter */
		/* whether the tx fifo is empty */
		if (debug & IDCS_DEBUG_TX_FIFO_EMPTY) {
			if(i2cs_dev->after_start == 1) {
				if(!i2c_slave_event(slave, I2C_SLAVE_READ_REQUESTED, &value))
					writeb_relaxed(value, i2cs_dev->base + IDCS_DATA);
				i2cs_dev->after_start = 0;
				dev_dbg(i2cs_dev->dev, "svalue=0x%x", value);
			} else if (!(control & IDCS_CONTROL_ACK)) {
				if(!i2c_slave_event(slave, I2C_SLAVE_READ_PROCESSED, &value))
					writeb_relaxed(value, i2cs_dev->base + IDCS_DATA);
				dev_dbg(i2cs_dev->dev, "value=0x%x", value);
			}
		}
	}

	/* repeated start */
	if(status & IDCS_STATUS_RSTART) {
		writel_relaxed(status & ~IDCS_STATUS_RSTART, i2cs_dev->base + IDCS_STATUS);
		i2cs_dev->after_start = 1;
	}

	/* clear interrupt */
	writel(control | IDCS_CONTROL_IRQ_CLR, i2cs_dev->base + IDCS_CONTROL);

	/* stop condition */
	if(status & IDCS_STATUS_STOP) {
		i2c_slave_event(slave, I2C_SLAVE_STOP, &value);
		ambarella_i2cs_hw_init(i2cs_dev);
	}

	return IRQ_HANDLED;
}

static int ambarella_i2cs_reg_slave(struct i2c_client *slave)
{
	struct ambarella_i2cs_dev *i2cs_dev = i2c_get_adapdata(slave->adapter);

	if (slave->flags & I2C_CLIENT_TEN)
		return -EAFNOSUPPORT;

	i2cs_dev->slave = slave;
	//writel_relaxed(slave->addr ,i2cs_dev->base + IDCS_SLAVE_ADDR);
	ambarella_i2cs_hw_init(i2cs_dev);

	return 0;
}

static int ambarella_i2cs_unreg_slave(struct i2c_client *slave)
{
	struct ambarella_i2cs_dev *i2cs_dev = i2c_get_adapdata(slave->adapter);

	WARN_ON(!i2cs_dev->slave);
	i2cs_dev->slave = NULL;

	writel_relaxed(IDCS_CONTROL_RESET_ALL, i2cs_dev->base + IDCS_CONTROL);

	return 0;
}

static u32 ambarella_i2cs_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_SLAVE;
}

static const struct i2c_algorithm ambarella_i2cs_algo = {
	.functionality = ambarella_i2cs_func,
	.reg_slave = ambarella_i2cs_reg_slave,
	.unreg_slave = ambarella_i2cs_unreg_slave,
};

static int ambarella_i2cs_probe(struct platform_device *pdev)
{
	struct ambarella_i2cs_dev *i2cs_dev;
	struct resource *res;
	int ret;

	i2cs_dev = devm_kzalloc(&pdev->dev, sizeof(*i2cs_dev), GFP_KERNEL);
	if (!i2cs_dev) {
		dev_err(&pdev->dev, "Out of memory!\n");
		return -ENOMEM;
	}

	i2cs_dev->dev = &pdev->dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	i2cs_dev->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2cs_dev->base)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(i2cs_dev->base);
	}

	i2cs_dev->irq = platform_get_irq(pdev, 0);
	if (i2cs_dev->irq <= 0) {
		dev_err(&pdev->dev, "cannot find IRQ\n");
		return i2cs_dev->irq;
	}

	ret = devm_request_irq(&pdev->dev, i2cs_dev->irq, ambarella_i2cs_irq,
				   IRQF_TRIGGER_HIGH, dev_name(&pdev->dev), i2cs_dev);
	if (ret != 0) {
		dev_err(&pdev->dev, "cannot claim IRQ %d\n", i2cs_dev->irq);
		return ret;
	}

	/* Setup i2cs_dev driver structure */
	strlcpy(i2cs_dev->adapter.name, pdev->name, sizeof(i2cs_dev->adapter.name));
	i2cs_dev->adapter.owner	= THIS_MODULE;
	i2cs_dev->adapter.algo = &ambarella_i2cs_algo;
	i2cs_dev->adapter.dev.parent = &pdev->dev;
	i2cs_dev->adapter.dev.of_node = pdev->dev.of_node;
	i2c_set_adapdata(&i2cs_dev->adapter, i2cs_dev);
	platform_set_drvdata(pdev, i2cs_dev);

	ret = i2c_add_adapter(&i2cs_dev->adapter);
	if (ret) {
		dev_err(&pdev->dev, "Adding I2CS adapter failed!\n");
		return ret;
	}

	return 0;
}

static int ambarella_i2cs_remove(struct platform_device *pdev)
{
	struct ambarella_i2cs_dev *i2cs_dev = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2cs_dev->adapter);

	return 0;
}

static const struct of_device_id ambarella_i2cs_match[] = {
	{ .compatible = "ambarella,i2cs", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_i2cs_match);

static struct platform_driver ambarella_i2cs_driver = {
	.driver = {
		.name = "ambarella-i2cs",
		.of_match_table = ambarella_i2cs_match,
	},
	.probe = ambarella_i2cs_probe,
	.remove = ambarella_i2cs_remove,
};

module_platform_driver(ambarella_i2cs_driver);

MODULE_DESCRIPTION("Ambarella I2C slave driver");
MODULE_LICENSE("GPL v2");

