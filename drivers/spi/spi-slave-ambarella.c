/*
 * History:
 *	2012/02/21 - [Zhenwu Xue]  created file
 *	2019/10/25 - [xuliang Zhang] modify and fixup bugs
 *	2021/03/07 - [Cao Rongrong] re-write
 * Copyright (C) 2004-2012, Ambarella, Inc.
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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/spi/spidev.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include "spi-ambarella.h"

#define SSIS_DEVNAME			"slavespi"

#define SSIS_DIR_RX			(true)
#define SSIS_DIR_TX			(false)

#define SSIS_MAX_SPEED_HZ		(8000000)
#define SSIS_MIN_PLL_HZ			(150000000UL)

#define SSIS_DMA_BUF_SIZE		(2048)
#define SSIS_DMA_BURST_SIZE		(8)

#define SSIS_FIFO_ENTRY_NUM		(32)

/* slave spi controller */
struct ambarella_ssis {
	struct miscdevice miscdev;
	struct device *dev;
	struct clk *clk;
	void __iomem *regbase;
	u32 phys_base;
	int irq;

	int notify_gpio;
	u8 notify_gpio_active;

	bool opened;
	struct mutex mtx;
	atomic_t dma_flag;
	wait_queue_head_t wq;

	u32 bpw;
	u32 spi_mode;
	u32 max_speed_hz;

	struct dma_chan *tx_dma_chan;
	struct dma_chan *rx_dma_chan;
	dma_addr_t tx_dma_phys;
	dma_addr_t rx_dma_phys;
	u8 *tx_dma_buf;
	u8 *rx_dma_buf;
	u32 dma_buf_size;
};

#define miscdev_to_ssis(d)	container_of((d), struct ambarella_ssis, miscdev)

static irqreturn_t ambarella_ssis_isr(int irq, void *dev_data)
{
	struct ambarella_ssis *ssis = dev_data;
	u32 isr, imr;

	isr = readl(ssis->regbase + SPI_ISR_OFFSET);
	imr = readl(ssis->regbase + SPI_IMR_OFFSET);
	writel(imr & ~isr, ssis->regbase + SPI_IMR_OFFSET);

	/* clear irq, seems useless */
	readl(ssis->regbase + SPI_ICR_OFFSET);
	readl(ssis->regbase + SPI_RISR_OFFSET);

	if (isr & SPI_TXEIS_MASK) {
		dev_err(ssis->dev, "Tx Fifo Empty\n");
	}

	if (isr & SPI_TXOIS_MASK) {
		dev_err(ssis->dev, "Tx Fifo Overflow\n");
	}

	if (isr & SPI_RXUIS_MASK) {
		dev_err(ssis->dev, "Rx Fifo Underflow\n");
	}

	if (isr & SPI_RXOIS_MASK) {
		dev_err(ssis->dev, "Rx Fifo Overflow\n");
	}

	if (isr & SPI_RXFIS_MASK) {
		if (gpio_is_valid(ssis->notify_gpio))
			gpio_set_value(ssis->notify_gpio, !ssis->notify_gpio_active);

		atomic_and(~0x2, &ssis->dma_flag);
		wake_up_interruptible(&ssis->wq);
	}

	return IRQ_HANDLED;
}

static void ambarella_ssis_dma_rx_callback(void *dma_param)
{
	struct ambarella_ssis *ssis = dma_param;

	if (gpio_is_valid(ssis->notify_gpio))
		gpio_set_value(ssis->notify_gpio, !ssis->notify_gpio_active);

	atomic_and(~0x2, &ssis->dma_flag);
	wake_up_interruptible(&ssis->wq);
}

static void ambarella_ssis_dma_tx_callback(void *dma_param)
{
	struct ambarella_ssis *ssis = dma_param;

	atomic_and(~0x1, &ssis->dma_flag);
	wake_up_interruptible(&ssis->wq);
}

static int ambarella_ssis_dma_start(struct ambarella_ssis *ssis, bool is_rx, u32 size)
{
	struct dma_chan *dma_chan;
	struct dma_slave_config dma_cfg;
	struct dma_async_tx_descriptor *txd;
	enum dma_transfer_direction direction;
	enum dma_slave_buswidth buswidth;
	dma_addr_t dma_phys;
	int rval;

	if (size == 0)
		return 0;

	direction = is_rx ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV;

	if (ssis->bpw == 8)
		buswidth = DMA_SLAVE_BUSWIDTH_1_BYTE;
	else
		buswidth = DMA_SLAVE_BUSWIDTH_2_BYTES;

	if (is_rx) {
		dma_cfg.direction = direction;
		dma_cfg.src_addr_width = buswidth;
		dma_cfg.src_addr = ssis->phys_base + SPI_DR_OFFSET;
		dma_cfg.src_maxburst = SSIS_DMA_BURST_SIZE;

		dma_chan = ssis->rx_dma_chan;
		dma_phys = ssis->rx_dma_phys;
	} else {
		dma_cfg.direction = direction;
		dma_cfg.dst_addr_width = buswidth;
		dma_cfg.dst_addr = ssis->phys_base + SPI_DR_OFFSET;
		dma_cfg.dst_maxburst = SSIS_DMA_BURST_SIZE;

		dma_chan = ssis->tx_dma_chan;
		dma_phys = ssis->tx_dma_phys;
	}

	rval = dmaengine_slave_config(dma_chan, &dma_cfg);
	if (rval < 0)
		return rval;

	txd = dmaengine_prep_slave_single(dma_chan,   dma_phys, size,
			direction, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!txd)
		return -EIO;

	txd->callback = is_rx ? ambarella_ssis_dma_rx_callback : ambarella_ssis_dma_tx_callback;
	txd->callback_param = ssis;
	dmaengine_submit(txd);
	dma_async_issue_pending(dma_chan);

	return 0;
}

static int ambarella_ssis_dma_allocate(struct ambarella_ssis *ssis, bool is_rx)
{
	struct dma_chan *dma_chan;
	dma_addr_t dma_phys;
	u8 *dma_buf;

	dma_chan = dma_request_slave_channel(ssis->dev, is_rx ? "rx" : "tx");
	if (IS_ERR_OR_NULL(dma_chan))
		return -ENODEV;

	dma_buf = dma_alloc_coherent(ssis->dev, ssis->dma_buf_size, &dma_phys, GFP_KERNEL);
	if (!dma_buf) {
		dev_err(ssis->dev, "Not able to allocate the dma buffer\n");
		dma_release_channel(dma_chan);
		return -ENOMEM;
	}

	if (is_rx) {
		ssis->rx_dma_chan = dma_chan;
		ssis->rx_dma_buf = dma_buf;
		ssis->rx_dma_phys = dma_phys;
	} else {
		ssis->tx_dma_chan = dma_chan;
		ssis->tx_dma_buf = dma_buf;
		ssis->tx_dma_phys = dma_phys;
	}

	return 0;
}

static void ambarella_ssis_dma_free(struct ambarella_ssis *ssis, bool is_rx)
{
	struct dma_chan *dma_chan;
	dma_addr_t dma_phys;
	u8 *dma_buf;

	if (is_rx) {
		dma_chan = ssis->rx_dma_chan;
		dma_buf = ssis->rx_dma_buf;
		dma_phys = ssis->rx_dma_phys;
	} else {
		dma_chan = ssis->tx_dma_chan;
		dma_buf = ssis->tx_dma_buf;
		dma_phys = ssis->tx_dma_phys;
	}

	dma_release_channel(dma_chan);
	dma_free_coherent(ssis->dev, ssis->dma_buf_size, dma_buf, dma_phys);
}

static void ambarella_ssis_enable(struct ambarella_ssis *ssis)
{
	spi_ctrl0_reg_t ctrl0_reg;
	ktime_t timeout;

	ctrl0_reg.w = 0;
	ctrl0_reg.s.dfs = ssis->bpw - 1;
	ctrl0_reg.s.scph = !!(ssis->spi_mode & SPI_CPHA);
	ctrl0_reg.s.scpol = !!(ssis->spi_mode & SPI_CPOL);
	ctrl0_reg.s.tmod = SPI_WRITE_READ;
	ctrl0_reg.s.residue = 1;
	ctrl0_reg.s.tx_lsb = 0;
	ctrl0_reg.s.rx_lsb = 0;
	ctrl0_reg.s.byte_ws = 0; /* always use 32 entries */
	writel(ctrl0_reg.w, ssis->regbase + SPI_CTRLR0_OFFSET);

	/* enable dma-request */
	writel(SPI_DMAC_TX_EN | SPI_DMAC_RX_EN, ssis->regbase + SPI_DMAC_OFFSET);

	timeout = ktime_add_ms(ktime_get(), 300);
	while ((readl_relaxed(ssis->regbase + SPI_SR_OFFSET) & SPI_SR_TFE)) {
		if (ktime_after(ktime_get(), timeout))
			break;
		cpu_relax();
	}
	BUG_ON(ktime_after(ktime_get(), timeout));

	writel(SPI_SSIENR_ENABLE, ssis->regbase + SPI_SSIENR_OFFSET);

	if (gpio_is_valid(ssis->notify_gpio))
		gpio_set_value(ssis->notify_gpio, ssis->notify_gpio_active);
}

static void ambarella_ssis_disable(struct ambarella_ssis *ssis)
{
	if (gpio_is_valid(ssis->notify_gpio))
		gpio_set_value(ssis->notify_gpio, !ssis->notify_gpio_active);

	dmaengine_terminate_sync(ssis->tx_dma_chan);
	dmaengine_terminate_sync(ssis->rx_dma_chan);

	/* fifo will be reset when enble goes from 1 to 0 */
	writel(SPI_SSIENR_DISABLE, ssis->regbase + SPI_SSIENR_OFFSET);
	writel(0, ssis->regbase + SPI_IMR_OFFSET);
	writel(0, ssis->regbase + SPI_DMAC_OFFSET);
}

static int ambarella_ssis_transfer(struct ambarella_ssis *ssis, struct spi_ioc_transfer *xfer)
{
	u32 tx_dma_len, rx_dma_len, rx_fifo_lvl, rx_fifo_data;
	ktime_t timeout;
	int rval;

	if (xfer->len == 0 || xfer->len > ssis->dma_buf_size || (xfer->len % (ssis->bpw / 8))) {
		dev_err(ssis->dev, "Invalid transfer len: %d!\n", xfer->len);
		return -EINVAL;
	}

	if (xfer->speed_hz && xfer->speed_hz > ssis->max_speed_hz) {
		dev_err(ssis->dev, "invalid speed_hz: %d!\n", ssis->max_speed_hz);
		return -EINVAL;
	}

	if (xfer->bits_per_word && xfer->bits_per_word != ssis->bpw) {
		dev_err(ssis->dev, "invalid bits_per_word: %d!\n", ssis->bpw);
		return -EINVAL;
	}

	if (xfer->tx_buf) {
		if (copy_from_user(ssis->tx_dma_buf, (u8 __user *)xfer->tx_buf, xfer->len))
			return -EFAULT;
	}

	if (xfer->len < SSIS_DMA_BURST_SIZE) {
		tx_dma_len = SSIS_DMA_BURST_SIZE;
		rx_dma_len = 0;
		writel(0, ssis->regbase + SPI_RXFTLR_OFFSET);
		writel(SPI_RXFIS_MASK, ssis->regbase + SPI_IMR_OFFSET);
	} else {
		tx_dma_len = xfer->len;
		rx_dma_len = round_down(xfer->len, SSIS_DMA_BURST_SIZE);
		writel(SSIS_DMA_BURST_SIZE / (ssis->bpw / 8) - 1, ssis->regbase + SPI_RXFTLR_OFFSET);
		writel(0, ssis->regbase + SPI_IMR_OFFSET);
	}

	atomic_set(&ssis->dma_flag, 0x3);

	rval = ambarella_ssis_dma_start(ssis, SSIS_DIR_RX, rx_dma_len);
	if (rval < 0) {
		dev_err(ssis->dev, "failed to start rx dma: %d!\n", rval);
		return rval;
	}

	rval = ambarella_ssis_dma_start(ssis, SSIS_DIR_TX, tx_dma_len);
	if (rval < 0) {
		dev_err(ssis->dev, "failed to start tx dma: %d!\n", rval);
		return rval;
	}

	ambarella_ssis_enable(ssis);

	wait_event_interruptible_timeout(ssis->wq, atomic_read(&ssis->dma_flag) == 0x0,
		msecs_to_jiffies(xfer->delay_usecs) ? : MAX_SCHEDULE_TIMEOUT);

	/* drain rx fifo by CPU */
	timeout = ktime_add_ms(ktime_get(), 300);
	while (rx_dma_len < xfer->len && ktime_before(ktime_get(), timeout)) {
		rx_fifo_lvl = readl(ssis->regbase + SPI_RXFLR_OFFSET);
		while (rx_fifo_lvl--) {
			BUG_ON(rx_dma_len >= ssis->dma_buf_size);
			rx_fifo_data = readl(ssis->regbase + SPI_DR_OFFSET);
			ssis->rx_dma_buf[rx_dma_len++] = rx_fifo_data;
			if (ssis->bpw == 16)
				ssis->rx_dma_buf[rx_dma_len++] = rx_fifo_data >> 8;
		}

		cpu_relax();
	}

	ambarella_ssis_disable(ssis);

	if (xfer->rx_buf) {
		if (copy_to_user((u8 __user *)xfer->rx_buf, ssis->rx_dma_buf, xfer->len))
			rval = -EFAULT;
	}

	return rval;
}

static long slavespi_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ambarella_ssis *ssis = miscdev_to_ssis(filp->private_data);
	struct spi_ioc_transfer xfer;
	u32 bpw, spi_mode, max_speed_hz;
	int rval = 0;

	mutex_lock(&ssis->mtx);

	bpw = ssis->bpw;
	spi_mode = ssis->spi_mode;
	max_speed_hz = ssis->max_speed_hz;

	switch (cmd) {
	/* read requests */
	case SPI_IOC_RD_MODE:
		rval = put_user(ssis->spi_mode, (__u8 __user *)arg);
		break;
	case SPI_IOC_RD_BITS_PER_WORD:
		rval = put_user(ssis->bpw, (__u8 __user *)arg);
		break;
	case SPI_IOC_RD_MAX_SPEED_HZ:
		rval = put_user(ssis->max_speed_hz, (__u32 __user *)arg);
		break;

	/* write requests */
	case SPI_IOC_WR_MODE:
		rval = get_user(spi_mode, (u8 __user *)arg);
		break;
	case SPI_IOC_WR_BITS_PER_WORD:
		rval = get_user(bpw, (__u8 __user *)arg);
		break;
	case SPI_IOC_WR_MAX_SPEED_HZ:
		rval = get_user(max_speed_hz, (__u32 __user *)arg);
		break;

	/* not supported */
	case SPI_IOC_RD_MODE32:
	case SPI_IOC_WR_MODE32:
	case SPI_IOC_RD_LSB_FIRST:
	case SPI_IOC_WR_LSB_FIRST:
		rval = -ENOTSUPP;
		break;

	case SPI_IOC_MESSAGE(1): /* we just support cmd with 1 message */
		if (copy_from_user(&xfer, (void __user *)arg, sizeof(xfer))) {
			rval = -EFAULT;
			break;
		}

		rval = ambarella_ssis_transfer(ssis, &xfer);
		break;

	default:
		dev_err(ssis->dev, "Unknown cmd: %d\n", cmd);
		break;
	}

	if ((bpw != 8 && bpw != 16) || !(spi_mode & SPI_CPHA) 	|| max_speed_hz > SSIS_MAX_SPEED_HZ) {
		dev_err(ssis->dev, "Invalid parameters: %d, %d, %d\n", bpw, spi_mode, max_speed_hz);
		rval = -EINVAL;
	} else {
		ssis->bpw = bpw;
		ssis->spi_mode = spi_mode;
		ssis->max_speed_hz = max_speed_hz;
	}

	mutex_unlock(&ssis->mtx);

	return rval;
}

static ssize_t slavespi_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	return -EPERM;
}

static ssize_t slavespi_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	return -EPERM;
}

static int slavespi_open(struct inode *inode, struct file *filp)
{
	struct ambarella_ssis *ssis = miscdev_to_ssis(filp->private_data);
	int rval = 0;

	mutex_lock(&ssis->mtx);
	if (ssis->opened)
		rval = -EBUSY;
	else
		ssis->opened = true;
	mutex_unlock(&ssis->mtx);

	return rval;
}

static int slavespi_release(struct inode *inode, struct file *filp)
{
	struct ambarella_ssis *ssis = miscdev_to_ssis(filp->private_data);

	mutex_lock(&ssis->mtx);
	ssis->opened = false;
	mutex_unlock(&ssis->mtx);

	return 0;
}

static struct file_operations slavespi_fops = {
	.open		= slavespi_open,
	.read		= slavespi_read,
	.write		= slavespi_write,
	.unlocked_ioctl	= slavespi_ioctl,
	.release	= slavespi_release,
};

static void ambarella_ssis_init(struct ambarella_ssis *ssis)
{
	unsigned long clk_rate, div = 1;

	clk_rate = clk_get_rate(clk_get_parent(ssis->clk));

	while ((clk_rate / (div + 1)) > SSIS_MIN_PLL_HZ)
		div++;

	clk_set_rate(ssis->clk, clk_rate / div);

	/* disable slave spi explicitly */
	writel(SPI_SSIENR_DISABLE, ssis->regbase + SPI_SSIENR_OFFSET);

	/* it's reserved register for chips without CEHU, so harmless */
	writel(2, ssis->regbase + SPI_FAULTINJECT_OFFSET);

	/* disable dma & all IRQs */
	writel(0, ssis->regbase + SPI_DMAC_OFFSET);
	writel(0, ssis->regbase + SPI_IMR_OFFSET);

	/* clear any error */
	readl(ssis->regbase + SPI_TXOICR_OFFSET);
	readl(ssis->regbase + SPI_RXOICR_OFFSET);
	readl(ssis->regbase + SPI_RXUICR_OFFSET);
	readl(ssis->regbase + SPI_ICR_OFFSET);

	writel(SSIS_FIFO_ENTRY_NUM / 2 - 1, ssis->regbase + SPI_TXFTLR_OFFSET);
	writel(SSIS_DMA_BURST_SIZE - 1, ssis->regbase + SPI_RXFTLR_OFFSET);
}

static int ambarella_ssis_of_parse(struct ambarella_ssis *ssis)
{
	struct device_node *np = ssis->dev->of_node;
	enum of_gpio_flags flags;
	u32 gpio_init_flag;
	int rval;

	if (of_property_read_u32(np, "amb,dma-buf-size", &ssis->dma_buf_size) < 0)
		ssis->dma_buf_size = SSIS_DMA_BUF_SIZE;

	ssis->notify_gpio = of_get_named_gpio_flags(np, "notify-gpio", 0, &flags);
	if (!gpio_is_valid(ssis->notify_gpio))
		return 0;

	ssis->notify_gpio_active = !!(flags & OF_GPIO_ACTIVE_LOW);

	if (ssis->notify_gpio_active)
		gpio_init_flag = GPIOF_OUT_INIT_LOW;
	else
		gpio_init_flag = GPIOF_OUT_INIT_HIGH;

	rval = devm_gpio_request_one(ssis->dev, ssis->notify_gpio,
			gpio_init_flag, "slavespi notify gpio");
	if (rval < 0) {
		dev_err(ssis->dev, "Failed to request notify-gpios!\n");
		return 0;
	}

	return 0;
}

static int ambarella_ssis_probe(struct platform_device *pdev)
{
	struct ambarella_ssis *ssis;
	struct resource *res;
	int rval;

	ssis = devm_kzalloc(&pdev->dev, sizeof(*ssis), GFP_KERNEL);
	if (!ssis) {
		dev_err(&pdev->dev, "no memory\n");
		return -ENOMEM;
	}

	ssis->regbase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(ssis->regbase)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(ssis->regbase);
	}

	ssis->irq = platform_get_irq(pdev, 0);
	if (ssis->irq < 0) {
		dev_err(&pdev->dev, "get irq failed\n");
		return -ENOENT;
	}

	rval = devm_request_irq(&pdev->dev, ssis->irq,
			ambarella_ssis_isr, IRQF_TRIGGER_HIGH,
			dev_name(&pdev->dev), ssis);
	if (rval < 0) {
		dev_err(&pdev->dev, "failed to request irq: %d\n", rval);
		return rval;
	}

	ssis->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR_OR_NULL(ssis->clk)) {
		dev_err(&pdev->dev, "no clk found\n");
		return PTR_ERR(ssis->clk);
	}

	ssis->phys_base = res->start;
	ssis->dev = &pdev->dev;
	ssis->max_speed_hz = SSIS_MAX_SPEED_HZ;
	ssis->spi_mode = SPI_MODE_3;	/* [only support mode 3!!!], force!!! */
	ssis->bpw = 8;
	ssis->opened = false;

	mutex_init(&ssis->mtx);
	init_waitqueue_head(&ssis->wq);

	rval = ambarella_ssis_of_parse(ssis);
	if (rval < 0)
		return rval;

	ambarella_ssis_init(ssis);

	rval = ambarella_ssis_dma_allocate(ssis, SSIS_DIR_RX);
	if (rval < 0) {
		dev_err(ssis->dev, "failed to allocate rx dma\n");
		goto err_exit0;
	}

	rval = ambarella_ssis_dma_allocate(ssis, SSIS_DIR_TX);
	if (rval < 0) {
		dev_err(ssis->dev, "failed to allocate tx dma\n");
		goto err_exit1;
	}

	/* init misc device */
	ssis->miscdev.minor = MISC_DYNAMIC_MINOR;
	ssis->miscdev.name = SSIS_DEVNAME; // KBUILD_MODNAME
	ssis->miscdev.parent = ssis->dev;
	ssis->miscdev.fops = &slavespi_fops;
	rval = misc_register(&ssis->miscdev);
	if (rval < 0) {
		dev_err(&pdev->dev, "unable to register misc device\n");
		goto err_exit2;
	}

	dev_info(&pdev->dev, "probed\n");

	platform_set_drvdata(pdev, ssis);

	return 0;

err_exit2:
	ambarella_ssis_dma_free(ssis, SSIS_DIR_TX);
err_exit1:
	ambarella_ssis_dma_free(ssis, SSIS_DIR_RX);
err_exit0:
	return rval;
}

static int ambarella_ssis_remove(struct platform_device *pdev)
{
	struct ambarella_ssis *ssis = platform_get_drvdata(pdev);

	misc_deregister(&ssis->miscdev);

	ambarella_ssis_dma_free(ssis, SSIS_DIR_TX);
	ambarella_ssis_dma_free(ssis, SSIS_DIR_RX);

	return 0;
}

static const struct of_device_id ambarella_ssis_dt_ids[] = {
	{.compatible = "ambarella,spi-slave", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_ssis_dt_ids);

static struct platform_driver ambarella_ssis_driver = {
	.probe		= ambarella_ssis_probe,
	.remove		= ambarella_ssis_remove,
	.driver		= {
		.name	= "ambarella-spi-slave",
		.owner	= THIS_MODULE,
		.of_match_table = ambarella_ssis_dt_ids,
	},
};

module_platform_driver(ambarella_ssis_driver);

MODULE_DESCRIPTION("Ambarella SPI Slave Controller");
MODULE_AUTHOR("Zhang Xuliang");
MODULE_LICENSE("GPL v2");

