/*
 * drivers/i2c/busses/ambarella_i2c.c
 *
 * Anthony Ginger, <hfjiang@ambarella.com>
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <asm/dma.h>
#include <linux/cpufreq.h>

/* ==========================================================================*/

#define IDC_ENR_OFFSET			0x00
#define IDC_CTRL_OFFSET			0x04
#define IDC_DATA_OFFSET			0x08
#define IDC_STS_OFFSET			0x0c
#define IDC_PSLL_OFFSET			0x10
#define IDC_PSLH_OFFSET			0x14
#define IDC_FMCTRL_OFFSET		0x18
#define IDC_FMDATA_OFFSET		0x1c
#define IDC_PSHS_OFFSET			0x20
#define IDC_DUTYCYCLE_OFFSET		0x24
#define IDC_STRETCHSCL_OFFSET		0x28

#define IDC_ENR_REG_ENABLE		(0x01)
#define IDC_ENR_REG_DISABLE		(0x00)

#define IDC_CTRL_HSMODE			(0x10)
#define IDC_CTRL_STOP			(0x08)
#define IDC_CTRL_START			(0x04)
#define IDC_CTRL_IF			(0x02)
#define IDC_CTRL_ACK			(0x01)
#define IDC_CTRL_CLS			(0x00)

#define IDC_STS_FIFO_EMP		(0x04)
#define IDC_STS_FIFO_FUL		(0x02)

#define IDC_FIFO_BUF_SIZE		(63)

#define IDC_FMCTRL_HSMODE		(0x10)
#define IDC_FMCTRL_STOP			(0x08)
#define IDC_FMCTRL_START		(0x04)
#define IDC_FMCTRL_IF			(0x02)

/* ==========================================================================*/

#ifndef CONFIG_I2C_AMBARELLA_RETRIES
#define CONFIG_I2C_AMBARELLA_RETRIES		(3)
#endif

/* must keep consistent with the name in device tree source, and the name
 * of corresponding i2c client will be overwritten when it's in used. */
#define AMBARELLA_I2C_VIN_FDT_NAME		"ambvin"
#define AMBARELLA_I2C_VIN_MAX_NUM		8
#define AMBARELLA_I2C_STOP_WAIT_INTERVAL_US (10)
#define AMBARELLA_I2C_STOP_WAIT_TIME_US (5 * AMBARELLA_I2C_STOP_WAIT_INTERVAL_US)

enum ambarella_i2c_state {
	AMBA_I2C_STATE_IDLE,
	AMBA_I2C_STATE_START,
	AMBA_I2C_STATE_START_TEN,
	AMBA_I2C_STATE_START_NEW,
	AMBA_I2C_STATE_READ,
	AMBA_I2C_STATE_READ_STOP,
	AMBA_I2C_STATE_WRITE,
	AMBA_I2C_STATE_WRITE_WAIT_ACK,
	AMBA_I2C_STATE_BULK_WRITE,
	AMBA_I2C_STATE_NO_ACK,
	AMBA_I2C_STATE_ERROR,
	AMBA_I2C_STATE_HS_MODE
};

enum ambarella_i2c_hw_state {
	AMBA_I2C_HW_STATE_IDLE,
	AMBA_I2C_HW_STATE_START_COMMAND,
	AMBA_I2C_HW_STATE_TX_DATA,
	AMBA_I2C_HW_STATE_RX_ACK,
	AMBA_I2C_HW_STATE_COMMAND_FINISH,
	AMBA_I2C_HW_STATE_STOP_COMMAND,
	AMBA_I2C_HW_STATE_RX_DATA,
	AMBA_I2C_HW_STATE_TX_ACK
};

struct ambarella_i2c_dev_info {
	unsigned char __iomem 			*regbase;

	struct device				*dev;
	int				        irq;
	struct i2c_adapter			adap;
	enum ambarella_i2c_state		state;

	u32					clk_limit;
	u32					hs_clk_limit;
	u32					bulk_num;
	u32					duty_cycle;
	u32					stretch_scl;
	u32					turbo_mode;
	u32					master_code;
	bool					hs_mode;
	bool					hsmode_enter;

	struct i2c_msg				*msgs;
	__u16					msg_num;
	__u16					msg_addr;
	wait_queue_head_t			msg_wait;
	unsigned int				msg_index;

#ifdef CONFIG_CPU_FREQ
	struct notifier_block			cpufreq_nb;
#endif
	struct clk				*clk;
};

int ambpriv_i2c_update_addr(const char *name, int bus, int addr)
{
	struct device_node *np;
	struct i2c_adapter *adap;
	struct i2c_client *client;
	char buf[32];
	int i;

	adap = i2c_get_adapter(bus);
	if (!adap) {
		pr_err("No such i2c controller: %d\n", bus);
		return -ENODEV;
	}

	for (i = 0; i < AMBARELLA_I2C_VIN_MAX_NUM; i++) {
		snprintf(buf, 32, "%s%d", AMBARELLA_I2C_VIN_FDT_NAME, i);
		np = of_get_child_by_name(adap->dev.of_node, buf);
		if (!np)
			continue;

		client = of_find_i2c_device_by_node(np);
		if (!client) {
			pr_err("failed to find i2c client\n");
			return -ENODEV;
		}

		if (!strcmp(client->name, AMBARELLA_I2C_VIN_FDT_NAME) ||
			!strcmp(client->name, name))
			break;
	}

	if (i >= AMBARELLA_I2C_VIN_MAX_NUM) {
		pr_err("Can't find VIN FDT node on i2c bus %d!\n", bus);
		return -ENODEV;
	}

	client->addr = addr;
	strlcpy(client->name, name, I2C_NAME_SIZE);

	return 0;
}
EXPORT_SYMBOL(ambpriv_i2c_update_addr);

static enum ambarella_i2c_hw_state ambarella_i2c_get_hw_state(struct ambarella_i2c_dev_info *pinfo)
{
	__u32 status_reg;

	status_reg = readl_relaxed(pinfo->regbase + IDC_STS_OFFSET);

	return (status_reg >> 4) & 0xF;
}

static inline void ambarella_i2c_set_clk(struct ambarella_i2c_dev_info *pinfo)
{
	unsigned int				apb_clk;
	__u32					idc_prescale;

	apb_clk = clk_get_rate(pinfo->clk);

	writel_relaxed(IDC_ENR_REG_DISABLE, pinfo->regbase + IDC_ENR_OFFSET);

	idc_prescale =( ((apb_clk / pinfo->clk_limit) - 2)/(4 + pinfo->duty_cycle)) - 1;

	dev_dbg(pinfo->dev, "apb_clk[%dHz]\n", apb_clk);
	dev_dbg(pinfo->dev, "idc_prescale[%d]\n", idc_prescale);
	dev_dbg(pinfo->dev, "duty_cycle[%d]\n", pinfo->duty_cycle);
	dev_dbg(pinfo->dev, "clk[%dHz]\n",
		(apb_clk / ((idc_prescale + 1) << 2)));

	writeb_relaxed(idc_prescale, pinfo->regbase + IDC_PSLL_OFFSET);
	writeb_relaxed(idc_prescale >> 8, pinfo->regbase + IDC_PSLH_OFFSET);

	writeb_relaxed(pinfo->duty_cycle, pinfo->regbase + IDC_DUTYCYCLE_OFFSET);
	writeb_relaxed(pinfo->stretch_scl, pinfo->regbase + IDC_STRETCHSCL_OFFSET);

	writel_relaxed(IDC_ENR_REG_ENABLE, pinfo->regbase + IDC_ENR_OFFSET);
}

static inline void ambarella_i2c_set_hs_clk(struct ambarella_i2c_dev_info *pinfo)
{
	unsigned int				apb_clk;
	__u32					idc_prescale;

	apb_clk = clk_get_rate(pinfo->clk);

	writel_relaxed(IDC_ENR_REG_DISABLE, pinfo->regbase + IDC_ENR_OFFSET);

	idc_prescale = (apb_clk / (6 * pinfo->hs_clk_limit)) - (4 / 3);

	dev_dbg(pinfo->dev, "apb_clk[%dHz]\n", apb_clk);
	dev_dbg(pinfo->dev, "idc_prescale[%d]\n", idc_prescale);

	writeb_relaxed(idc_prescale, pinfo->regbase + IDC_PSHS_OFFSET);

	writel_relaxed(IDC_ENR_REG_ENABLE, pinfo->regbase + IDC_ENR_OFFSET);
}

#ifdef CONFIG_CPU_FREQ
static int ambarella_i2c_cpufreq_callback(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct ambarella_i2c_dev_info *pinfo;
	bool clk_changed = false;

	pinfo = container_of(nb, struct ambarella_i2c_dev_info, cpufreq_nb);

	if (val == CPUFREQ_PRECHANGE) {
		i2c_lock_bus(&pinfo->adap, I2C_LOCK_ROOT_ADAPTER);
		dev_dbg(pinfo->dev, "%s: CPUFREQ_PRECHANGE \n", __func__);
	} else if (val == CPUFREQ_POSTCHANGE) {
		dev_dbg(pinfo->dev, "%s: CPUFREQ_POSTCHANGE \n", __func__);

		if (pinfo->clk) {
			struct clk *parent = clk_get_parent(pinfo->clk);
			const char *clk_name = __clk_get_name(pinfo->clk);
			const char *parent_clk_name;

			if (parent)
				parent_clk_name = __clk_get_name(parent);

			if (!strcmp(clk_name, "gclk_core")
					|| !strcmp(clk_name,"gclk_ahb")
					|| !strcmp(clk_name,"gclk_apb")) {
				clk_changed = true;

			} else if (parent && (!strcmp(parent_clk_name, "gclk_core")
						|| !strcmp(parent_clk_name,"gclk_ahb")
						|| !strcmp(parent_clk_name,"gclk_apb"))) {
				clk_changed = true;
			}

		}

		if (clk_changed)
			ambarella_i2c_set_clk(pinfo);

		i2c_unlock_bus(&pinfo->adap, I2C_LOCK_ROOT_ADAPTER);
	}

	return 0;
}
#endif

static inline void ambarella_i2c_hw_init(struct ambarella_i2c_dev_info *pinfo)
{
	ambarella_i2c_set_clk(pinfo);

	if (pinfo->hs_mode)
		ambarella_i2c_set_hs_clk(pinfo);

	pinfo->msgs = NULL;
	pinfo->msg_num = 0;
	pinfo->state = AMBA_I2C_STATE_IDLE;
}

static inline void ambarella_i2c_send_master_code(
	struct ambarella_i2c_dev_info *pinfo)
{
	writeb_relaxed(pinfo->master_code, pinfo->regbase + IDC_DATA_OFFSET);
	writel_relaxed(IDC_CTRL_START, pinfo->regbase + IDC_CTRL_OFFSET);
}

static inline void ambarella_i2c_start_single_msg(
	struct ambarella_i2c_dev_info *pinfo)
{
	__u32 hs_mode;

	if (pinfo->msgs->flags & I2C_M_TEN) {
		pinfo->state = AMBA_I2C_STATE_START_TEN;
		writeb_relaxed((0xf0 | ((pinfo->msg_addr >> 8) & 0x07)),
					pinfo->regbase + IDC_DATA_OFFSET);
	} else {
		pinfo->state = AMBA_I2C_STATE_START;
		writeb_relaxed(pinfo->msg_addr, pinfo->regbase + IDC_DATA_OFFSET);
	}

	hs_mode = (pinfo->hs_mode) ? (IDC_CTRL_HSMODE) : (0U);
	writel_relaxed(IDC_CTRL_START | hs_mode, pinfo->regbase + IDC_CTRL_OFFSET);
}

static inline void ambarella_i2c_bulk_write(
	struct ambarella_i2c_dev_info *pinfo,
	__u32 fifosize)
{
	while (fifosize--) {
		writeb_relaxed(pinfo->msgs->buf[pinfo->msg_index++],
				pinfo->regbase + IDC_FMDATA_OFFSET);
		if (pinfo->msg_index >= pinfo->msgs->len)
			break;
	};

	/* the last fifo data MUST be STOP+IF */
	writel_relaxed(IDC_FMCTRL_IF | IDC_FMCTRL_STOP,
		pinfo->regbase + IDC_FMCTRL_OFFSET);
}

static inline void ambarella_i2c_start_bulk_msg_write(
	struct ambarella_i2c_dev_info *pinfo)
{
	__u32				fifosize = IDC_FIFO_BUF_SIZE;
	__u32				hs_mode;

	pinfo->state = AMBA_I2C_STATE_BULK_WRITE;

	writel_relaxed(0, pinfo->regbase + IDC_CTRL_OFFSET);

	hs_mode = (pinfo->hs_mode) ? (IDC_FMCTRL_HSMODE) : (0U);
	writel_relaxed(IDC_FMCTRL_START | hs_mode, pinfo->regbase + IDC_FMCTRL_OFFSET);

	if (pinfo->msgs->flags & I2C_M_TEN) {
		writeb_relaxed((0xf0 | ((pinfo->msg_addr >> 8) & 0x07)),
				pinfo->regbase + IDC_FMDATA_OFFSET);
		fifosize--;
	}
	writeb_relaxed(pinfo->msg_addr, pinfo->regbase + IDC_FMDATA_OFFSET);
	fifosize -= 2;

	ambarella_i2c_bulk_write(pinfo, fifosize);
}

static inline void ambarella_i2c_start_current_msg(
	struct ambarella_i2c_dev_info *pinfo)
{
	pinfo->msg_index = 0;
	pinfo->msg_addr = (pinfo->msgs->addr << 1);

	if (pinfo->msgs->flags & I2C_M_RD)
		pinfo->msg_addr |= 1;

	if (pinfo->msgs->flags & I2C_M_REV_DIR_ADDR)
		pinfo->msg_addr ^= 1;

	if (pinfo->msgs->flags & I2C_M_RD) {
		ambarella_i2c_start_single_msg(pinfo);
	} else if (pinfo->turbo_mode) {
		ambarella_i2c_start_bulk_msg_write(pinfo);
	} else {
		ambarella_i2c_start_single_msg(pinfo);
	}

	dev_dbg(pinfo->dev, "msg_addr[0x%x], len[0x%x]",
		pinfo->msg_addr, pinfo->msgs->len);
}

static inline void ambarella_i2c_stop(
	struct ambarella_i2c_dev_info *pinfo,
	enum ambarella_i2c_state state,
	__u32 *pack_control)
{
	if(state != AMBA_I2C_STATE_IDLE) {
		*pack_control |= IDC_CTRL_ACK;
	}

	pinfo->state = state;
	pinfo->msgs = NULL;
	pinfo->msg_num = 0;

	*pack_control |= IDC_CTRL_STOP;

	if (pinfo->hs_mode)
		*pack_control &= ~IDC_CTRL_HSMODE;

	if (pinfo->state == AMBA_I2C_STATE_IDLE ||
		pinfo->state == AMBA_I2C_STATE_NO_ACK)
		wake_up(&pinfo->msg_wait);
}

static inline __u32 ambarella_i2c_check_ack(
	struct ambarella_i2c_dev_info *pinfo,
	__u32 *pack_control,
	__u32 retry_counter)
{
	__u32				retVal = IDC_CTRL_ACK;

ambarella_i2c_check_ack_enter:
	if (unlikely((*pack_control) & IDC_CTRL_ACK)) {
		if (pinfo->msgs->flags & I2C_M_IGNORE_NAK)
			goto ambarella_i2c_check_ack_exit;

		if ((pinfo->msgs->flags & I2C_M_RD) &&
			(pinfo->msgs->flags & I2C_M_NO_RD_ACK))
			goto ambarella_i2c_check_ack_exit;

		if (retry_counter--) {
			udelay(100);
			*pack_control = readl_relaxed(pinfo->regbase + IDC_CTRL_OFFSET);
			goto ambarella_i2c_check_ack_enter;
		}
		retVal = 0;
		*pack_control = 0;
		ambarella_i2c_stop(pinfo,
			AMBA_I2C_STATE_NO_ACK, pack_control);
	}

ambarella_i2c_check_ack_exit:
	return retVal;
}

static irqreturn_t ambarella_i2c_irq(int irqno, void *dev_id)
{
	struct ambarella_i2c_dev_info	*pinfo;
	__u32				status_reg;
	__u32				control_reg;
	__u32				ack_control = IDC_CTRL_CLS;

	pinfo = (struct ambarella_i2c_dev_info *)dev_id;

	status_reg = readl_relaxed(pinfo->regbase + IDC_STS_OFFSET);
	control_reg = readl_relaxed(pinfo->regbase + IDC_CTRL_OFFSET);

	if (pinfo->hs_mode)
		ack_control |= IDC_CTRL_HSMODE;

	dev_dbg(pinfo->dev, "state[0x%x]\n", pinfo->state);
	dev_dbg(pinfo->dev, "status_reg[0x%x]\n", status_reg);
	dev_dbg(pinfo->dev, "control_reg[0x%x]\n", control_reg);

	switch (pinfo->state) {
	case AMBA_I2C_STATE_START:
		if (ambarella_i2c_check_ack(pinfo, &control_reg,
			1) == IDC_CTRL_ACK) {
			if (pinfo->msgs->flags & I2C_M_RD) {
				if (pinfo->msgs->len == 1)
					ack_control |= IDC_CTRL_ACK;
				pinfo->state = AMBA_I2C_STATE_READ;
			} else {
				pinfo->state = AMBA_I2C_STATE_WRITE;
				goto amba_i2c_irq_write;
			}
		} else {
			ack_control = control_reg;
		}
		break;
	case AMBA_I2C_STATE_START_TEN:
		pinfo->state = AMBA_I2C_STATE_START;
		writeb_relaxed(pinfo->msg_addr, pinfo->regbase + IDC_DATA_OFFSET);
		break;
	case AMBA_I2C_STATE_START_NEW:
amba_i2c_irq_start_new:
		ambarella_i2c_start_current_msg(pinfo);
		goto amba_i2c_irq_exit;
		break;
	case AMBA_I2C_STATE_READ_STOP:
		pinfo->msgs->buf[pinfo->msg_index] =
			readb_relaxed(pinfo->regbase + IDC_DATA_OFFSET);
		pinfo->msg_index++;
amba_i2c_irq_read_stop:
		ambarella_i2c_stop(pinfo, AMBA_I2C_STATE_IDLE, &ack_control);
		break;
	case AMBA_I2C_STATE_READ:
		pinfo->msgs->buf[pinfo->msg_index] =
			readb_relaxed(pinfo->regbase + IDC_DATA_OFFSET);
		pinfo->msg_index++;

		if (pinfo->msg_index >= pinfo->msgs->len - 1) {
			if (pinfo->msg_num > 1) {
				pinfo->msgs++;
				pinfo->state = AMBA_I2C_STATE_START_NEW;
				pinfo->msg_num--;
			} else {
				if (pinfo->msg_index > pinfo->msgs->len - 1) {
					goto amba_i2c_irq_read_stop;
				} else {
					pinfo->state = AMBA_I2C_STATE_READ_STOP;
					ack_control |= IDC_CTRL_ACK;
				}
			}
		}
		break;
	case AMBA_I2C_STATE_WRITE:
amba_i2c_irq_write:
		pinfo->state = AMBA_I2C_STATE_WRITE_WAIT_ACK;
		writeb_relaxed(pinfo->msgs->buf[pinfo->msg_index],
				pinfo->regbase + IDC_DATA_OFFSET);
		break;
	case AMBA_I2C_STATE_WRITE_WAIT_ACK:
		if (ambarella_i2c_check_ack(pinfo, &control_reg,
			1) == IDC_CTRL_ACK) {
			pinfo->state = AMBA_I2C_STATE_WRITE;
			pinfo->msg_index++;

			if (pinfo->msg_index >= pinfo->msgs->len) {
				if (pinfo->msg_num > 1) {
					pinfo->msgs++;
					pinfo->state = AMBA_I2C_STATE_START_NEW;
					pinfo->msg_num--;
					goto amba_i2c_irq_start_new;
				}
				ambarella_i2c_stop(pinfo,
					AMBA_I2C_STATE_IDLE, &ack_control);
			} else {
				goto amba_i2c_irq_write;
			}
		} else {
			ack_control = control_reg;
		}
		break;
	case AMBA_I2C_STATE_BULK_WRITE:
		while (((status_reg & 0xF0) != 0x50) && ((status_reg & 0xF0) != 0x00)) {
			cpu_relax();
			status_reg = readl_relaxed(pinfo->regbase + IDC_STS_OFFSET);
		};
		if (pinfo->msg_num > 1) {
			pinfo->msgs++;
			pinfo->state = AMBA_I2C_STATE_START_NEW;
			pinfo->msg_num--;
			goto amba_i2c_irq_start_new;
		}
		ambarella_i2c_stop(pinfo, AMBA_I2C_STATE_IDLE, &ack_control);
		goto amba_i2c_irq_exit;
	case AMBA_I2C_STATE_HS_MODE:
		pinfo->hsmode_enter = false;
		wake_up(&pinfo->msg_wait);
		goto amba_i2c_irq_exit;
	default:
		dev_err(pinfo->dev, "ambarella_i2c_irq in wrong state[0x%x]\n",
			pinfo->state);
		dev_err(pinfo->dev, "status_reg[0x%x]\n", status_reg);
		dev_err(pinfo->dev, "control_reg[0x%x]\n", control_reg);
		ack_control = IDC_CTRL_STOP | IDC_CTRL_ACK;
		pinfo->state = AMBA_I2C_STATE_ERROR;
		break;
	}

	writel_relaxed(ack_control, pinfo->regbase + IDC_CTRL_OFFSET);

amba_i2c_irq_exit:
	return IRQ_HANDLED;
}

static int ambarella_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
	int num)
{
	struct ambarella_i2c_dev_info	*pinfo;
	int				errorCode = -EPERM;
	int				retryCount;
	long				timeout;
	int				hw_state;
	int				wait_stop_retry;
	int				i;

	pinfo = (struct ambarella_i2c_dev_info *)i2c_get_adapdata(adap);

	/* check data length for FIFO mode */
	if (unlikely(pinfo->turbo_mode)) {
		pinfo->msgs = msgs;
		pinfo->msg_num = num;
		for (i = 0 ; i < pinfo->msg_num; i++) {
			if ((!(pinfo->msgs->flags & I2C_M_RD)) &&
				(pinfo->msgs->len > IDC_FIFO_BUF_SIZE - 2)) {
				dev_err(pinfo->dev,
					"Turbo(FIFO) mode can only support <= "
					"%d bytes writing, but message[%d]: %d bytes applied!\n",
					IDC_FIFO_BUF_SIZE - 2, i, pinfo->msgs->len);

				return -EPERM;
			}
			pinfo->msgs++;
		}
	}

	for (retryCount = 0; retryCount <= adap->retries; retryCount++) {
		errorCode = 0;

		if (pinfo->hs_mode) {
			pinfo->state = AMBA_I2C_STATE_HS_MODE;
			pinfo->hsmode_enter = true;
			ambarella_i2c_send_master_code(pinfo);

			timeout = wait_event_timeout(pinfo->msg_wait,
				pinfo->hsmode_enter == false, HZ * 1);
			if (timeout <= 0) {
				pinfo->state = AMBA_I2C_STATE_NO_ACK;
			}
			dev_dbg(pinfo->dev, "enter hs mode %ld jiffies left.\n", timeout);

			pinfo->hsmode_enter = false;
		}

		if (pinfo->state != AMBA_I2C_STATE_IDLE)
			ambarella_i2c_hw_init(pinfo);
		pinfo->msgs = msgs;
		pinfo->msg_num = num;

		ambarella_i2c_start_current_msg(pinfo);
		timeout = wait_event_timeout(pinfo->msg_wait,
			pinfo->msg_num == 0, adap->timeout);

		/* We need to check HW state to ensure the controller has already entered idle indeed, or if slave device
		 * holds the SCL due to busy, i2c gpio muxer might turn off bus before the STOP is really sent out. */
		wait_stop_retry = AMBARELLA_I2C_STOP_WAIT_TIME_US / AMBARELLA_I2C_STOP_WAIT_INTERVAL_US;
		while (wait_stop_retry--) {
			hw_state = ambarella_i2c_get_hw_state(pinfo);
			if (hw_state == AMBA_I2C_HW_STATE_IDLE)
				break;

			udelay(AMBARELLA_I2C_STOP_WAIT_INTERVAL_US);
		}

		if (hw_state != AMBA_I2C_HW_STATE_IDLE)
			dev_dbg(pinfo->dev, "Xfer exits with non-idle hw state %d.\n", hw_state);

		if (timeout <= 0) {
			pinfo->state = AMBA_I2C_STATE_NO_ACK;
		}
		dev_dbg(pinfo->dev, "%ld jiffies left.\n", timeout);

		if (pinfo->state != AMBA_I2C_STATE_IDLE) {
			errorCode = -EBUSY;
		} else {
			break;
		}
	}


	if (errorCode) {
		if (pinfo->state == AMBA_I2C_STATE_NO_ACK) {
			dev_err(pinfo->dev,
				"No ACK from address 0x%x, %d:%d!\n",
				pinfo->msg_addr, pinfo->msg_num,
				pinfo->msg_index);
		}
		return errorCode;
	}

	return num;
}

static u32 ambarella_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ambarella_i2c_algo = {
	.master_xfer	= ambarella_i2c_xfer,
	.functionality	= ambarella_i2c_func,
};

static int ambarella_i2c_probe(struct platform_device *pdev)
{
	struct device_node 			*np = pdev->dev.of_node;
	int					errorCode;
	struct ambarella_i2c_dev_info		*pinfo;
	struct i2c_adapter			*adap;
	struct resource				*mem;
	u32					i2c_class = 0;

	pinfo = devm_kzalloc(&pdev->dev, sizeof(*pinfo), GFP_KERNEL);
	if (pinfo == NULL) {
		dev_err(&pdev->dev, "Out of memory!\n");
		return -ENOMEM;
	}
	pinfo->dev = &pdev->dev;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (mem == NULL) {
		dev_err(&pdev->dev, "Get I2C mem resource failed!\n");
		return -ENXIO;
	}

	pinfo->regbase = devm_ioremap(&pdev->dev, mem->start, resource_size(mem));
	if (!pinfo->regbase) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return -ENOMEM;
	}

	pinfo->irq = platform_get_irq(pdev, 0);
	if (pinfo->irq < 0) {
		dev_err(&pdev->dev, "no irq for i2c%d!\n", pdev->id);
		return -ENODEV;
	}

	errorCode = of_property_read_u32(np, "amb,i2c-class", &i2c_class);
	if (errorCode < 0) {
		dev_err(&pdev->dev, "Get i2c_class failed!\n");
		return -ENODEV;
	}

	i2c_class &= (I2C_CLASS_HWMON | I2C_CLASS_DDC | I2C_CLASS_SPD);
	if (i2c_class == 0){
		dev_err(&pdev->dev, "Invalid i2c_class (0x%x)!\n", i2c_class);
		return -EINVAL;
	}

	errorCode = of_property_read_u32(np, "clock-frequency", &pinfo->clk_limit);
	if (errorCode < 0) {
		dev_err(&pdev->dev, "Get clock-frequenc failed!\n");
		return -ENODEV;
	}

	/* S/F/F+ mode */
	if (pinfo->clk_limit < 1000 || pinfo->clk_limit > 1000000)
		pinfo->clk_limit = 100000;

	errorCode = of_property_read_u32(np, "amb,duty-cycle", &pinfo->duty_cycle);
	if (errorCode < 0) {
		dev_dbg(&pdev->dev, "Missing duty-cycle, assuming 1:1!\n");
		pinfo->duty_cycle = 0;
	}

	if (pinfo->duty_cycle > 2)
		pinfo->duty_cycle = 2;

	errorCode = of_property_read_u32(np, "amb,stretch-scl", &pinfo->stretch_scl);
	if (errorCode < 0) {
		dev_dbg(&pdev->dev, "Missing stretch-scl, assuming 1!\n");
		pinfo->stretch_scl = 1;
	}

	/* check if using turbo mode */
	if (of_find_property(pdev->dev.of_node, "amb,turbo-mode", NULL)) {
		pinfo->turbo_mode = 1;
		dev_info(&pdev->dev, "Turbo(FIFO) mode is used(ignore device ACK)!\n");
	} else {
		pinfo->turbo_mode = 0;
	}

	/* check if using high-speed mode */
	if (of_find_property(pdev->dev.of_node, "amb,hs-mode", NULL)) {
		pinfo->hs_mode = true;

		errorCode = of_property_read_u32(np, "hs-master-code", &pinfo->master_code);
		if (errorCode < 0) {
			dev_dbg(&pdev->dev, "Missing hs-master-code, assuming 0x0E!\n");
			pinfo->master_code = 0x0E;
		}

		errorCode = of_property_read_u32(np, "hs-clock-frequency", &pinfo->hs_clk_limit);
		if (errorCode < 0) {
			dev_dbg(&pdev->dev, "Missing hs-clock-frequenc, assuming 1Mbps!\n");
			pinfo->hs_clk_limit = 1000000;
		}

		/* high speed mode */
		if (pinfo->hs_clk_limit < 1000 || pinfo->hs_clk_limit > 3400000)
			pinfo->hs_clk_limit = 1000000;

		/* for hs-mode, master code can only be sent by F/S mode */
		if (pinfo->clk_limit > 400000)
			pinfo->clk_limit = 400000;

		dev_info(&pdev->dev, "High speed mode is used, hs-clock:%d!\n", pinfo->hs_clk_limit);
	} else {
		pinfo->hs_mode = false;
	}

	init_waitqueue_head(&pinfo->msg_wait);

	pinfo->clk = clk_get(pinfo->dev, NULL);

	ambarella_i2c_hw_init(pinfo);

	platform_set_drvdata(pdev, pinfo);

	errorCode = devm_request_irq(&pdev->dev, pinfo->irq, ambarella_i2c_irq,
				IRQF_TRIGGER_RISING, dev_name(&pdev->dev), pinfo);
	if (errorCode) {
		dev_err(&pdev->dev, "Request IRQ failed!\n");
		return errorCode;
	}

	adap = &pinfo->adap;
	i2c_set_adapdata(adap, pinfo);
	adap->owner = THIS_MODULE;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = np;
	adap->class = i2c_class;
	strlcpy(adap->name, pdev->name, sizeof(adap->name));
	adap->algo = &ambarella_i2c_algo;
	adap->nr = of_alias_get_id(np, "i2c");
	adap->retries = CONFIG_I2C_AMBARELLA_RETRIES;

	errorCode = i2c_add_numbered_adapter(adap);
	if (errorCode) {
		dev_err(&pdev->dev, "Adding I2C adapter failed!\n");
		return errorCode;
	}

#ifdef CONFIG_CPU_FREQ
	pinfo->cpufreq_nb.notifier_call = ambarella_i2c_cpufreq_callback;
	errorCode = cpufreq_register_notifier(&pinfo->cpufreq_nb, CPUFREQ_TRANSITION_NOTIFIER);
	if (errorCode < 0) {
		pr_err("%s: Failed to register cpufreq notifier %d\n", __func__, errorCode);
		return errorCode;
	}
#endif

	dev_info(&pdev->dev, "Ambarella I2C adapter[%d] probed, clock:%d!\n", adap->nr, pinfo->clk_limit);

	return 0;
}

static int ambarella_i2c_remove(struct platform_device *pdev)
{
	struct ambarella_i2c_dev_info *pinfo;
	int errorCode = 0;

	pinfo = platform_get_drvdata(pdev);
	if (pinfo) {
#ifdef CONFIG_CPU_FREQ
		cpufreq_unregister_notifier(&pinfo->cpufreq_nb,
				CPUFREQ_TRANSITION_NOTIFIER);
#endif
		i2c_del_adapter(&pinfo->adap);
	}

	dev_notice(&pdev->dev,
		"Remove Ambarella Media Processor I2C adapter[%s] [%d].\n",
		dev_name(&pinfo->adap.dev), errorCode);

	return errorCode;
}

#ifdef CONFIG_PM
static int ambarella_i2c_suspend(struct device *dev)
{
	int				errorCode = 0;
	struct platform_device		*pdev;
	struct ambarella_i2c_dev_info	*pinfo;

	pdev = to_platform_device(dev);
	pinfo = platform_get_drvdata(pdev);
	if (pinfo) {
		disable_irq(pinfo->irq);
	}

	dev_dbg(&pdev->dev, "%s\n", __func__);

	return errorCode;
}

static int ambarella_i2c_resume(struct device *dev)
{
	int				errorCode = 0;
	struct platform_device		*pdev;
	struct ambarella_i2c_dev_info	*pinfo;

	pdev = to_platform_device(dev);
	pinfo = platform_get_drvdata(pdev);
	if (pinfo) {
		ambarella_i2c_hw_init(pinfo);
		enable_irq(pinfo->irq);
	}

	dev_dbg(&pdev->dev, "%s\n", __func__);

	return errorCode;
}

static const struct dev_pm_ops ambarella_i2c_dev_pm_ops = {

	/* suspend to mem */
	.suspend_noirq = ambarella_i2c_suspend,
	.resume_noirq = ambarella_i2c_resume,

	/* suspend to disk */
	.freeze = ambarella_i2c_suspend,
	.thaw = ambarella_i2c_resume,

	/* restore from suspend to disk */
	.restore = ambarella_i2c_resume,
};
#endif

static const struct of_device_id ambarella_i2c_of_match[] = {
	{.compatible = "ambarella,i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_i2c_of_match);

static struct platform_driver ambarella_i2c_driver = {
	.probe		= ambarella_i2c_probe,
	.remove		= ambarella_i2c_remove,
	.driver		= {
		.name	= "ambarella-i2c",
		.of_match_table = ambarella_i2c_of_match,
#ifdef CONFIG_PM
		.pm	= &ambarella_i2c_dev_pm_ops,
#endif
	},
};

static int __init ambarella_i2c_init(void)
{
	return platform_driver_register(&ambarella_i2c_driver);
}

static void __exit ambarella_i2c_exit(void)
{
	platform_driver_unregister(&ambarella_i2c_driver);
}

subsys_initcall(ambarella_i2c_init);
module_exit(ambarella_i2c_exit);

MODULE_DESCRIPTION("Ambarella Media Processor I2C Bus Controller");
MODULE_AUTHOR("Anthony Ginger, <hfjiang@ambarella.com>");
MODULE_LICENSE("GPL");

