// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 * History:
 *	2014/07/21 - [Cao Rongrong] Create
 *
 * Copyright (C) 2014-2019, Ambarella, Inc.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/iio/iio.h>
#include <linux/iio/machine.h>
#include <linux/iio/driver.h>
#include <linux/iio/buffer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/events.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/triggered_event.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/input.h>
#include "ambarella_adc.h"

/*
 * T = V * coeff - offset
 * T is Celsius degree
 * V is adc value read out
 * coeff varies from process to process, it should be 0.07 for 14nm
 * 0.06757 for 5nm, T = 105 + coeff * (V - CODEcal)
 * offset varies from chip to chip, it needs calibration
 * Scale use *10^6
 */

/* 105000 is a rough value, it veries from chip to chip */
#define AMBARELLA_ADC_T2V_OFFSET	105000
/* Typical sensor slope coefficient at all temperatures */
#define AMBARELLA_ADC_T2V_COEFF		67570

/* Set 105C as a calibrate temperature base when using otp calibration */
#define AMBARELLA_CALIB_T2V_OFFSET	105000000

/* New calibrate method use 2 temperature base */
#define CALIB_T2V_HIGH				105000000
#define CALIB_T2V_LOW				50000000
#define CALIB_T2V_HIGH_BIT_OFFSET	(10)
#define CALIB_T2V_LOW_BIT_OFFSET	(20)
#define CALIB_T2V_HIGH_BITMASK		(0x003FF000)
#define CALIB_T2V_LOW_BITMASK		(0xFFC00000)
#define CALIB_T2V_CHECK_BITMASK		(0xFFFFF000)
/* ==========================================================================*/

static void ambarella_adc_set_threshold(struct ambarella_adc *ambadc,
		u32 ch, u32 high, u32 low)
{
	ambadc->vol_threshold[ch] &= ~(ADC_VAL_HI(0xfff));
	ambadc->vol_threshold[ch] &= ~(ADC_VAL_LO(0xfff));
	ambadc->vol_threshold[ch] |= ADC_VAL_HI(high);
	ambadc->vol_threshold[ch] |= ADC_VAL_LO(low);

	writel_relaxed(ambadc->vol_threshold[ch],
			ambadc->regbase + ADC_INT_CTRL_X_OFFSET(ch));
}

static void ambarella_adc_enable_threshold(struct ambarella_adc *ambadc, u32 ch)
{
	ambadc->vol_threshold[ch] |= ADC_INT_THRESHOLD_EN;
	writel_relaxed(ambadc->vol_threshold[ch],
			ambadc->regbase + ADC_INT_CTRL_X_OFFSET(ch));
}

static void ambarella_adc_disable_threshold(struct ambarella_adc *ambadc, u32 ch)
{

	ambadc->vol_threshold[ch] &= ~ADC_INT_THRESHOLD_EN;
	writel_relaxed(ambadc->vol_threshold[ch],
			ambadc->regbase + ADC_INT_CTRL_X_OFFSET(ch));
}

static int ambarella_adc_key_handler(struct ambarella_adc *ambadc, u32 ch)
{
	struct ambadc_keymap *keymap = ambadc->keymap;
	struct input_dev *input = ambadc->input;
	unsigned long expire_time;
	u32 i, level, data;

	level = readl_relaxed(ambadc->regbase + ADC_DATA_X_OFFSET(ch));

	expire_time = jiffies + msecs_to_jiffies(AMBARELLA_ADC_KEY_DEBOUNCE);

	while (time_before(jiffies, expire_time)) {
		data = readl_relaxed(ambadc->regbase + ADC_DATA_X_OFFSET(ch));
		usleep_range(10, 50);
		if (ambadc->key_pressed[ch] == KEY_RESERVED)
			level = min(level, data);
		else
			level = max(level, data);
	}

	for (i = 0; i < ambadc->key_num; i++, keymap++) {
		if (ch != keymap->channel
			|| level < keymap->low_level
			|| level > keymap->high_level)
			continue;

		if (ambadc->key_pressed[ch] == KEY_RESERVED
			&& keymap->key_code != KEY_RESERVED) {
			input_report_key(input, keymap->key_code, 1);
			input_sync(input);
			ambadc->key_pressed[ch] = keymap->key_code;
			ambarella_adc_set_threshold(ambadc,
				ch, keymap->high_level, keymap->low_level);
			dev_dbg(&input->dev, "key[%d:%d] pressed %d\n",
				ch, keymap->key_code, level);
			break;
		} else if (ambadc->key_pressed[ch] != KEY_RESERVED
			&& keymap->key_code == KEY_RESERVED) {
			input_report_key(input, ambadc->key_pressed[ch], 0);
			input_sync(input);
			ambadc->key_pressed[ch] = KEY_RESERVED;
			ambarella_adc_set_threshold(ambadc,
				ch, keymap->high_level, keymap->low_level);
			dev_dbg(&input->dev, "key[%d:%d] released %d\n",
				ch, keymap->key_code, level);
			break;
		}
	}

	return 0;
}

static int ambarella_adc_key_of_parse(struct ambarella_adc *ambadc)
{
	struct device_node *np = ambadc->dev->of_node;
	struct ambadc_keymap *keymap;
	const __be32 *prop;
	u32 propval, i, size;

	prop = of_get_property(np, "amb,keymap", &size);
	if (!prop || size % (sizeof(__be32) * 2)) {
		dev_err(ambadc->dev, "Invalid keymap!\n");
		return -ENOENT;
	}

	/* cells is 2 for each keymap */
	size /= sizeof(__be32) * 2;
	ambadc->key_num = size;

	ambadc->keymap = devm_kzalloc(ambadc->dev,
			sizeof(struct ambadc_keymap) * size, GFP_KERNEL);
	if (ambadc->keymap == NULL){
		dev_err(ambadc->dev, "No memory for keymap!\n");
		return -ENOMEM;
	}

	keymap = ambadc->keymap;

	for (i = 0; i < ambadc->key_num; i++, keymap++) {
		propval = be32_to_cpup(prop + i * 2);
		keymap->low_level = propval & 0xfff;
		keymap->high_level = (propval >> 16) & 0xfff;
		keymap->channel = propval >> 28;
		if (keymap->channel >= ambadc->channel_num) {
			dev_err(ambadc->dev, "Invalid channel: %d\n", keymap->channel);
			return -EINVAL;
		}

		keymap->key_code = be32_to_cpup(prop + i * 2 + 1);

		if (keymap->key_code == KEY_RESERVED) {
			ambadc->key_pressed[keymap->channel] = KEY_RESERVED;
			ambarella_adc_set_threshold(ambadc,
				keymap->channel, keymap->high_level, keymap->low_level);
			ambarella_adc_enable_threshold(ambadc, keymap->channel);
		}

		input_set_capability(ambadc->input, EV_KEY, keymap->key_code);

		set_bit(keymap->channel, &ambadc->channels_mask);

	}

	return 0;
}

static int ambarella_adc_key_init(struct ambarella_adc *ambadc)
{
	struct device_node *np = ambadc->dev->of_node;
	struct input_dev *input;
	int rval = 0;

	if (!of_find_property(np, "amb,keymap", NULL))
		return 0;

	input = devm_input_allocate_device(ambadc->dev);
	if (!input) {
		dev_err(ambadc->dev, "input_allocate_device fail!\n");
		return -ENOMEM;
	}

	input->name = "AmbADCkey";
	input->phys = "ambadckey/input0";
	input->id.bustype = BUS_HOST;
	input->dev.parent = ambadc->dev;

	rval = input_register_device(input);
	if (rval < 0) {
		dev_err(ambadc->dev, "Register input_dev failed!\n");
		return rval;
	}

	ambadc->input = input;

	rval = ambarella_adc_key_of_parse(ambadc);
	if (rval < 0) {
		input_unregister_device(ambadc->input);
		return rval;
	}

	dev_info(ambadc->dev, "ADC key input driver probed!\n");

	return 0;
}


/*****************************************************************************/

static void ambarella_adc_set_fifo(struct ambarella_adc *ambadc)
{
	u32 tmp, bit, chan_num, fifo_depth, fifo_threshold;

	if (bitmap_empty(&ambadc->fifo_enable_mask, ambadc->channel_num))
		return;

	chan_num = bitmap_weight(&ambadc->fifo_enable_mask, ambadc->channel_num);
	fifo_depth = (ADC_MAX_FIFO_DEPTH / chan_num) & ~0x3;
	fifo_threshold = (fifo_depth >> 2) * 3;

	for_each_set_bit(bit, &ambadc->fifo_enable_mask, ambadc->channel_num) {
		tmp = ADC_FIFO_ID(bit) | ADC_FIFO_TH(fifo_threshold) | fifo_depth;
		writel_relaxed(tmp, ambadc->regbase + ADC_FIFO_CTRL_X_OFFSET(bit));
	}

	ambadc->fifo_threshold = fifo_threshold;

	/* clear fifo first */
	writel_relaxed(ADC_FIFO_CLEAR, ambadc->regbase + ADC_FIFO_CTRL_OFFSET);
}

static void ambarella_adc_power_up(struct ambarella_adc *ambadc)
{
	/* soft reset adc, all registers except for scaler reg will be cleared */
	writel_relaxed(ADC_CONTROL_CLEAR, ambadc->regbase + ADC_CONTROL_OFFSET);

	/* power up adc and all needed scaler */
	regmap_update_bits(ambadc->rct_regmap, ambadc->rct_offset[ADC16_CTRL_OFFSET],
		ADC_POWER_DOWN | ADC_SCALER_POWER_DOWN, ambadc->scalers_mask << 8);

	if (ambadc->t2v_channel >= 0)
		regmap_write(ambadc->rct_regmap, ambadc->rct_offset[T2V_CTRL_OFFSET], 0x0);
}

static void ambarella_adc_power_down(struct ambarella_adc *ambadc)
{
	u32 tmp;

	/* soft reset adc, all registers except for scaler reg will be cleared */
	writel_relaxed(ADC_CONTROL_CLEAR, ambadc->regbase + ADC_CONTROL_OFFSET);

	if (ambadc->t2v_channel >= 0)
		regmap_write(ambadc->rct_regmap, ambadc->rct_offset[T2V_CTRL_OFFSET], 0x1);

	/* power down adc and all scalers */
	tmp = ((1 << ambadc->channel_num) - 1) << 8;
	regmap_update_bits(ambadc->rct_regmap, ambadc->rct_offset[ADC16_CTRL_OFFSET],
		ADC_POWER_DOWN | ADC_SCALER_POWER_DOWN, tmp | ADC_POWER_DOWN);
}

static void ambarella_adc_start(struct ambarella_adc *ambadc)
{
	u32 i, tmp;

	/* soft reset adc, all registers except for scaler reg will be cleared */
	writel_relaxed(ADC_CONTROL_CLEAR, ambadc->regbase + ADC_CONTROL_OFFSET);

	/* return directly if no channel need to be enabled */
	if (bitmap_empty(&ambadc->channels_mask, ambadc->channel_num))
		return;

	/* enable adc, it must be done before parameters setting */
	tmp = ADC_CONTROL_MODE | ADC_CONTROL_ENABLE;
	writel_relaxed(tmp, ambadc->regbase + ADC_CONTROL_OFFSET);
	usleep_range(3, 10);

	/* we just use one slot */
	writel_relaxed(0x0, ambadc->regbase + ADC_SLOT_NUM_OFFSET);

	/* enable in used channels in each slot period */
	writel_relaxed(ambadc->channels_mask, ambadc->regbase + ADC_SLOT_CTRL_0_OFFSET);

	/* setup the period cycle for one slot */
	tmp = bitmap_weight(&ambadc->channels_mask, ambadc->channel_num);
	tmp *= ADC_PERIOD_CYCLE;
	writel_relaxed(tmp - 1, ambadc->regbase + ADC_SLOT_PERIOD_OFFSET);

	/* setup threshold for each channels */
	for (i = 0; i < ambadc->channel_num; i++) {
		tmp = ambadc->vol_threshold[i];
		writel_relaxed(tmp, ambadc->regbase + ADC_INT_CTRL_X_OFFSET(i));
	}

	ambarella_adc_set_fifo(ambadc);

	/* start adc */
	tmp = ADC_CONTROL_MODE | ADC_CONTROL_ENABLE | ADC_CONTROL_START;
	writel_relaxed(tmp, ambadc->regbase + ADC_CONTROL_OFFSET);
}

static void ambarella_adc_work(struct work_struct *data)
{
	struct ambarella_adc *ambadc;
	struct iio_dev *indio_dev;
	u32 ch, data_intr;

	ambadc = container_of(data, struct ambarella_adc, work);
	indio_dev = ambadc->indio_dev;

	local_irq_disable();
	data_intr = ambadc->data_intr;
	ambadc->data_intr = 0;
	local_irq_enable();

	for_each_set_bit(ch, (unsigned long *)&data_intr, ambadc->channel_num) {
		if (ambadc->input) {
			ambarella_adc_key_handler(ambadc, ch);
		} else {
			/*
			 * we don't know whether it is a upper or lower threshold
			 * event. Userspace will have to check the channel value
			 * if it wants to know.
			 */
			iio_push_event(indio_dev,
				IIO_UNMOD_EVENT_CODE(IIO_VOLTAGE, ch,
					IIO_EV_TYPE_THRESH, IIO_EV_DIR_EITHER),
				iio_get_time_ns(indio_dev));
		}

		ambarella_adc_enable_threshold(ambadc, ch);
	}
}

static irqreturn_t ambarella_adc_irq(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct ambarella_adc *ambadc = iio_priv(indio_dev);
	u32 ch, ctrl_intr, data_intr;

	ctrl_intr = readl_relaxed(ambadc->regbase + ADC_CTRL_INTR_TABLE_OFFSET);
	data_intr = readl_relaxed(ambadc->regbase + ADC_DATA_INTR_TABLE_OFFSET);

	if (readl_relaxed(ambadc->regbase + ADC_FIFO_INTR_TABLE_OFFSET)) {
		BUG_ON(!iio_buffer_enabled(indio_dev));
		disable_irq_nosync(irq);
		iio_trigger_poll(indio_dev->trig);
	}

	for_each_set_bit(ch, (unsigned long *)&data_intr, ambadc->channel_num) {
		BUG_ON(!(ambadc->vol_threshold[ch] & ADC_INT_THRESHOLD_EN));
		/* disable irq to avoid dummy irq, it will be enabled in WORK function */
		ambarella_adc_disable_threshold(ambadc, ch);
	}

	ambadc->data_intr = data_intr;
	schedule_work(&ambadc->work);

	/* clear irq source */
	writel_relaxed(ctrl_intr, ambadc->regbase + ADC_CTRL_INTR_TABLE_OFFSET);
	writel_relaxed(data_intr, ambadc->regbase + ADC_DATA_INTR_TABLE_OFFSET);

	return IRQ_HANDLED;
}

static int ambarella_adc_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct ambarella_adc *ambadc = iio_priv(indio_dev);
	struct iio_chan_spec const *chan;
	int bit;

	for_each_set_bit(bit, indio_dev->active_scan_mask, indio_dev->masklength) {
		chan = indio_dev->channels + bit;
		if (state)
			set_bit(chan->channel, &ambadc->fifo_enable_mask);
		else
			clear_bit(chan->channel, &ambadc->fifo_enable_mask);
	}

	ambarella_adc_start(ambadc);

	return 0;
}

static irqreturn_t ambarella_adc_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = (struct iio_poll_func *)private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ambarella_adc *ambadc = iio_priv(indio_dev);
	u32 i, j, fifo_int, fifo_count;
	u16 data[ADC_FIFO_NUMBER];

	/* wait for all channels fifo to reach at threshold */
	while (1) {
		fifo_int = readl_relaxed(ambadc->regbase + ADC_FIFO_INTR_TABLE_OFFSET);
		if (fifo_int == ambadc->fifo_enable_mask)
			break;
	}

	fifo_count = ambadc->fifo_threshold;

	while (fifo_count) {
		j = 0;
		for_each_set_bit(i, (unsigned long *)&fifo_int, ADC_FIFO_NUMBER) {
			data[j] = readl_relaxed(ambadc->regbase + ADC_FIFO_DATA_X_OFFSET(i));
			j++;
		}
		iio_push_to_buffers(indio_dev, data);
		fifo_count--;
	}

	iio_trigger_notify_done(indio_dev->trig);

	/* clear intr source at last to avoid dummy irq */
	writel_relaxed(fifo_int, ambadc->regbase + ADC_FIFO_INTR_TABLE_OFFSET);

	enable_irq(ambadc->irq);

	return IRQ_HANDLED;
}

static const struct iio_trigger_ops ambarella_adc_trigger_ops = {
	.set_trigger_state = &ambarella_adc_set_trigger_state,
};

static int ambarella_adc_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct ambarella_adc *ambadc = iio_priv(indio_dev);
	u32 tmp;

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		/* only for temperature */
		if (!test_bit(chan->channel, &ambadc->channels_mask))
			return -EIO;

		if (chan->channel != ambadc->t2v_channel)
			return -EINVAL;

		mutex_lock(&ambadc->mtx);
		tmp = readl_relaxed(ambadc->regbase + ADC_DATA_X_OFFSET(chan->channel));
		if (ambadc->rct_offset[T2V_CALIB_DATA_OFFSET]) {
			if (ambadc->t2v_offset)
				*val = CALIB_T2V_HIGH +
					tmp * ambadc->t2v_coeff - ambadc->t2v_coeff * ambadc->t2v_offset;
			else
				*val = 0;
		} else
			*val = tmp * ambadc->t2v_coeff - ambadc->t2v_offset;
		*val2 = 1000000;
		mutex_unlock(&ambadc->mtx);
		return IIO_VAL_FRACTIONAL;

	case IIO_CHAN_INFO_RAW:
		if (!test_bit(chan->channel, &ambadc->channels_mask))
			return -EIO;

		mutex_lock(&ambadc->mtx);
		*val = readl_relaxed(ambadc->regbase + ADC_DATA_X_OFFSET(chan->channel));
		*val2 = 0;
		mutex_unlock(&ambadc->mtx);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = test_bit(chan->channel, &ambadc->scalers_mask) ? 1800 : 3300;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;

	case IIO_CHAN_INFO_SAMP_FREQ:
		mutex_lock(&ambadc->mtx);
		*val = clk_get_rate(ambadc->clk) / ADC_PERIOD_CYCLE;
		*val /= bitmap_weight(&ambadc->channels_mask, ambadc->channel_num);
		*val2 = 0;
		mutex_unlock(&ambadc->mtx);
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int ambarella_adc_write_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int val, int val2, long mask)
{
	struct ambarella_adc *ambadc = iio_priv(indio_dev);

	if (mask != IIO_CHAN_INFO_SAMP_FREQ || val <= 0)
		return -EINVAL;

	mutex_lock(&ambadc->mtx);

	val *= ADC_PERIOD_CYCLE;
	val *= bitmap_weight(&ambadc->channels_mask, ambadc->channel_num);

	if (val > ADC_MAX_CLOCK)
		val = ADC_MAX_CLOCK;

	clk_set_rate(ambadc->clk, val);

	mutex_unlock(&ambadc->mtx);

	return 0;
}

static int ambarella_adc_read_event_config(struct iio_dev *idev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir)
{
	struct ambarella_adc *ambadc = iio_priv(idev);
	u32 tmp;

	tmp = readl_relaxed(ambadc->regbase + ADC_INT_CTRL_X_OFFSET(chan->channel));
	return !!(tmp & ADC_INT_THRESHOLD_EN);
}

static int ambarella_adc_write_event_config(struct iio_dev *idev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir, int state)
{
	struct ambarella_adc *ambadc = iio_priv(idev);
	u32 tmp, low, high;

	mutex_lock(&ambadc->mtx);

	tmp = readl_relaxed(ambadc->regbase + ADC_INT_CTRL_X_OFFSET(chan->channel));

	/* do NOT enable threshold if the threshold values are invalid */
	low = tmp & 0xfff;
	high = (tmp >> 16) & 0xfff;
	if (low >= high) {
		mutex_unlock(&ambadc->mtx);
		return -EPERM;
	}

	if (state)
		tmp |= ADC_INT_THRESHOLD_EN;
	else
		tmp &= ~ADC_INT_THRESHOLD_EN;

	writel_relaxed(tmp, ambadc->regbase + ADC_INT_CTRL_X_OFFSET(chan->channel));

	mutex_unlock(&ambadc->mtx);

	return 0;
}

static int ambarella_adc_read_event_value(struct iio_dev *idev,
				   const struct iio_chan_spec *chan,
				   enum iio_event_type type,
				   enum iio_event_direction dir,
				   enum iio_event_info info,
				   int *val, int *val2)
{
	struct ambarella_adc *ambadc = iio_priv(idev);
	u32 tmp;
	int rval = IIO_VAL_INT;

	mutex_lock(&ambadc->mtx);

	tmp = readl_relaxed(ambadc->regbase + ADC_INT_CTRL_X_OFFSET(chan->channel));

	switch (dir) {
	case IIO_EV_DIR_RISING:
		tmp >>= 16;
		*val = tmp & 0xfff;
		break;
	case IIO_EV_DIR_FALLING:
		*val = tmp & 0xfff;
		break;
	default:
		rval = -EINVAL;
		break;
	}

	mutex_unlock(&ambadc->mtx);

	return rval;
}

static int ambarella_adc_write_event_value(struct iio_dev *idev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info,
				    int val, int val2)
{
	struct ambarella_adc *ambadc = iio_priv(idev);
	u32 tmp;

	if (val > 0xfff)
		return -EINVAL;

	mutex_lock(&ambadc->mtx);

	tmp = readl_relaxed(ambadc->regbase + ADC_INT_CTRL_X_OFFSET(chan->channel));

	switch (dir) {
	case IIO_EV_DIR_RISING:
		tmp &= ~(ADC_VAL_HI(0xfff));
		tmp |= ADC_VAL_HI(val);
		ambadc->vol_threshold[chan->channel] = tmp;
		break;
	case IIO_EV_DIR_FALLING:
		tmp &= ~(ADC_VAL_LO(0xfff));
		tmp |= ADC_VAL_LO(val);
		ambadc->vol_threshold[chan->channel] = tmp;
		break;
	default:
		mutex_unlock(&ambadc->mtx);
		return -EINVAL;
	}

	writel_relaxed(tmp, ambadc->regbase + ADC_INT_CTRL_X_OFFSET(chan->channel));

	mutex_unlock(&ambadc->mtx);

	return 0;
}

static int ambarella_adc_validate_trigger(struct iio_dev *indio_dev,
					struct iio_trigger *trig)
{
	struct ambarella_adc *ambadc = iio_priv(indio_dev);

	/*
	 * only our own trigger is supported, other triggers provided by
	 * kernel should be able to support also, but need to update the
	 * drivers.
	 */
	return trig != ambadc->trig ? -EINVAL : 0;
}

static const struct iio_info ambarella_adc_iio_info = {
	.read_raw = &ambarella_adc_read_raw,
	.write_raw = &ambarella_adc_write_raw,
	.read_event_config = &ambarella_adc_read_event_config,
	.write_event_config = ambarella_adc_write_event_config,
	.read_event_value = &ambarella_adc_read_event_value,
	.write_event_value = &ambarella_adc_write_event_value,
	.validate_trigger = ambarella_adc_validate_trigger,
};

static ssize_t ambarella_adc_read_enable(struct iio_dev *indio_dev,
					  uintptr_t private,
					  const struct iio_chan_spec *chan,
					  char *buf)
{
	struct ambarella_adc *ambadc = iio_priv(indio_dev);

	return sprintf(buf, "%d\n", test_bit(chan->channel, &ambadc->channels_mask));
}

static ssize_t ambarella_adc_write_enable(struct iio_dev *indio_dev,
					   uintptr_t private,
					   const struct iio_chan_spec *chan,
					   const char *buf, size_t len)
{
	struct ambarella_adc *ambadc = iio_priv(indio_dev);
	bool powerdown;
	int rval;

	if (iio_buffer_enabled(indio_dev))
		return -EBUSY;

	rval = strtobool(buf, &powerdown);
	if (rval)
		return rval;

	mutex_lock(&ambadc->mtx);

	if (test_bit(chan->channel, &ambadc->channels_mask) != powerdown) {
		change_bit(chan->channel, &ambadc->channels_mask);
		ambarella_adc_start(ambadc);
	}

	mutex_unlock(&ambadc->mtx);

	return len;
}

static const struct iio_chan_spec_ext_info ambarella_adc_ext_info[] = {
	{
		.name = "enable",
		.read = ambarella_adc_read_enable,
		.write = ambarella_adc_write_enable,
		.shared = IIO_SEPARATE,
	},
	{ },
};

static const struct iio_event_spec ambarella_adc_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

static int ambarella_adc_channel_init(struct iio_dev *indio_dev)
{
	struct ambarella_adc *ambadc = iio_priv(indio_dev);
	struct iio_chan_spec *chan_array, *chan;
	u32 num_channels, idx, bit;

	num_channels = bitmap_weight(&ambadc->channels_mask, 	ambadc->channel_num);

	chan_array = devm_kcalloc(ambadc->dev, num_channels,
				sizeof(struct iio_chan_spec), GFP_KERNEL);
	if (chan_array == NULL)
		return -ENOMEM;

	chan = chan_array;
	idx = 0;

	for_each_set_bit(bit, &ambadc->channels_mask, ambadc->channel_num) {
		if (bit == ambadc->t2v_channel) {
			chan->type = IIO_TEMP;
			chan->info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
							BIT(IIO_CHAN_INFO_PROCESSED);
			chan->scan_index = -1;
		} else {
			chan->type = IIO_VOLTAGE;
			chan->info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
							BIT(IIO_CHAN_INFO_SCALE);
			chan->scan_index = idx++;
		}
		chan->indexed = 1;
		chan->channel = bit;
		chan->info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ);
		chan->datasheet_name = devm_kasprintf(ambadc->dev, GFP_KERNEL,
							"adc%d", bit);
		chan->ext_info = ambarella_adc_ext_info;
		chan->scan_type.sign = 'u';
		chan->scan_type.realbits = 12;
		chan->scan_type.storagebits = 16;

		chan->event_spec = ambarella_adc_events;
		chan->num_event_specs = ARRAY_SIZE(ambarella_adc_events);

		chan++;
	}

	indio_dev->channels = chan_array;
	indio_dev->num_channels = num_channels;

	return 0;
}

static int ambarella_adc_parse_dt(struct ambarella_adc *ambadc)
{
	struct device_node *np = ambadc->dev->of_node;
	unsigned long channels_all;
	u32 channels_used, scalers_1v8, clk_rate;
	u32 t2v_calib, calib_high, calib_low;

	if (of_property_read_u32(np, "amb,channel-num", &ambadc->channel_num) < 0
		|| ambadc->channel_num > ADC_MAX_CHANNEL_NUM) {
		dev_err(ambadc->dev, "invalid channel num: %d\n", ambadc->channel_num);
		return -EINVAL;
	}

	channels_all = (1 << ambadc->channel_num) - 1;

	if (of_property_read_u32(np, "amb,channels-used", &channels_used) < 0)
		ambadc->channels_mask = channels_all;
	else
		ambadc->channels_mask = channels_used;

	/* make sure the channels for using are all supported */
	if (!bitmap_subset(&ambadc->channels_mask, &channels_all, ambadc->channel_num)) {
		dev_err(ambadc->dev, "invalid channel used: %lx\n", ambadc->channels_mask);
		return -EINVAL;
	}

	if (of_property_read_u32(np, "amb,scaler-1v8", &scalers_1v8) < 0)
		scalers_1v8 = 0;

	/* no scaler for channel 0, so channel 0 range is always 1.8v */
	ambadc->scalers_mask = scalers_1v8 | 0x1;

	if (of_property_read_u32(np, "clock-frequency", &clk_rate) < 0)
		clk_rate = 6000000;

	clk_set_rate(ambadc->clk, clk_rate);

	/* dt properties for t2v channel */
	if (of_property_read_u32(np, "amb,t2v-channel", &ambadc->t2v_channel) < 0
		|| ambadc->t2v_channel >= ambadc->channel_num) {
		ambadc->t2v_channel = -1;
	}

	if (ambadc->t2v_channel >= 0) {
		if (ambadc->rct_offset[T2V_CTRL_OFFSET] == 0) {
			dev_err(ambadc->dev, "no t2v ctrl reg!\n");
			return -ENXIO;
		}

		set_bit(ambadc->t2v_channel, &ambadc->channels_mask);

		if (of_property_read_u32(np, "amb,t2v-coeff", &ambadc->t2v_coeff) < 0)
			ambadc->t2v_coeff = AMBARELLA_ADC_T2V_COEFF;

		if (of_property_read_u32(np, "amb,t2v-offset", &ambadc->t2v_offset) < 0)
			ambadc->t2v_offset = AMBARELLA_ADC_T2V_OFFSET;

		if (ambadc->rct_offset[T2V_CALIB_DATA_OFFSET]) {
			regmap_read(ambadc->rct_regmap,
				ambadc->rct_offset[T2V_CALIB_DATA_OFFSET],
				&t2v_calib);

			if (t2v_calib & CALIB_T2V_CHECK_BITMASK) {
				calib_high = t2v_calib & CALIB_T2V_HIGH_BITMASK;
				calib_high >>= CALIB_T2V_HIGH_BIT_OFFSET;

				calib_low = t2v_calib & CALIB_T2V_LOW_BITMASK;
				calib_low >>= CALIB_T2V_LOW_BIT_OFFSET;

				ambadc->t2v_coeff = (CALIB_T2V_HIGH - CALIB_T2V_LOW)/(calib_high - calib_low);
				ambadc->t2v_offset = calib_high;
			} else {
				ambadc->t2v_offset = t2v_calib;
			}
		}
	}

	return 0;
}

static int ambarella_adc_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ambarella_adc *ambadc;
	struct iio_dev *indio_dev;
	int rval;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(struct ambarella_adc));
	if (!indio_dev) {
		dev_err(&pdev->dev, "failed to allocate iio device\n");
		return -ENOMEM;
	}

	ambadc = iio_priv(indio_dev);
	ambadc->dev = &pdev->dev;
	ambadc->indio_dev = indio_dev;
	mutex_init(&ambadc->mtx);

	ambadc->regbase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ambadc->regbase)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(ambadc->regbase);
	}

	ambadc->irq = platform_get_irq(pdev, 0);
	if (ambadc->irq < 0) {
		dev_err(&pdev->dev, "Can not get irq!\n");
		return -ENXIO;
	}

	ambadc->clk = devm_clk_get(&pdev->dev, "gclk_adc");
	if (IS_ERR(ambadc->clk)) {
		dev_err(&pdev->dev, "failed to get clock: %ld\n", PTR_ERR(ambadc->clk));
		return PTR_ERR(ambadc->clk);
	}

	ambadc->rct_regmap = syscon_regmap_lookup_by_phandle_args(np, "amb,rct-regmap",
					ARRAY_SIZE(ambadc->rct_offset), ambadc->rct_offset);
	if (IS_ERR(ambadc->rct_regmap)) {
		dev_err(&pdev->dev, "no rct regmap!\n");
		return PTR_ERR(ambadc->rct_regmap);
	}

	rval = ambarella_adc_parse_dt(ambadc);
	if (rval < 0)
		return rval;

	platform_set_drvdata(pdev, indio_dev);

	ambarella_adc_key_init(ambadc);

	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->dev.of_node = pdev->dev.of_node;
	indio_dev->info = &ambarella_adc_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	rval = ambarella_adc_channel_init(indio_dev);
	if (rval < 0)
		return rval;

	rval = iio_triggered_buffer_setup(indio_dev, NULL,
				&ambarella_adc_trigger_handler, NULL);
	if (rval < 0) {
		dev_err(&pdev->dev, "Failed to setup buffer\n");
		return rval;
	}

	ambadc->trig = devm_iio_trigger_alloc(&pdev->dev, "%s-dev%d",
				      indio_dev->name, iio_device_id(indio_dev));
	if (ambadc->trig == NULL) {
		dev_err(&pdev->dev, "Failed to allocate iio trigger\n");
		return -ENOMEM;
	}

	ambadc->trig->ops = &ambarella_adc_trigger_ops;
	ambadc->trig->dev.parent = &pdev->dev;
	iio_trigger_set_drvdata(ambadc->trig, indio_dev);
	iio_trigger_register(ambadc->trig);
	/* select default trigger */
	indio_dev->trig = iio_trigger_get(ambadc->trig);

	rval = iio_device_register(indio_dev);
	if (rval < 0) {
		dev_err(&pdev->dev, "failed to register iio device %d!\n", rval);
		return -ENXIO;
	}

	INIT_WORK(&ambadc->work, ambarella_adc_work);
	rval = devm_request_irq(&pdev->dev, ambadc->irq, ambarella_adc_irq,
				0, dev_name(&pdev->dev), indio_dev);
	if (rval < 0) {
		dev_err(&pdev->dev, "failed to request irq %d!\n", ambadc->irq);
		return -ENXIO;
	}

	ambarella_adc_power_up(ambadc);
	ambarella_adc_start(ambadc);

	dev_info(&pdev->dev, "%d channels\n", indio_dev->num_channels);

	return 0;
}

static int ambarella_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct ambarella_adc *ambadc = iio_priv(indio_dev);

	cancel_work_sync(&ambadc->work);

	if (ambadc->input)
		input_unregister_device(ambadc->input);
	iio_trigger_unregister(ambadc->trig);
	iio_device_unregister(indio_dev);
	ambarella_adc_power_down(ambadc);

	return 0;
}

#ifdef CONFIG_PM
static int ambarella_adc_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ambarella_adc *ambadc = iio_priv(indio_dev);

	ambadc->clk_rate = clk_get_rate(ambadc->clk);
	ambarella_adc_power_down(ambadc);

	return 0;
}

static int ambarella_adc_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ambarella_adc *ambadc = iio_priv(indio_dev);

	clk_set_rate(ambadc->clk, ambadc->clk_rate);

	ambarella_adc_power_up(ambadc);
	ambarella_adc_start(ambadc);

	return 0;
}
static struct dev_pm_ops ambarella_adc_pm_ops = {
	/* suspend to memory */
	.suspend	= ambarella_adc_suspend,
	.resume		= ambarella_adc_resume,

	/* suspend to disk */
	.freeze		= ambarella_adc_suspend,
	.thaw		= ambarella_adc_resume,

	/* restore from suspend to disk */
	.restore	= ambarella_adc_resume,
};

#endif

static const struct of_device_id ambarella_adc_match[] = {
	{ .compatible = "ambarella,adc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ambarella_adc_match);

static struct platform_driver ambarella_adc_driver = {
	.probe		= ambarella_adc_probe,
	.remove		= ambarella_adc_remove,
	.driver		= {
		.name	= "ambarella-adc",
		.of_match_table = ambarella_adc_match,
#ifdef CONFIG_PM
		.pm	= &ambarella_adc_pm_ops,
#endif
	},
};

module_platform_driver(ambarella_adc_driver);

MODULE_AUTHOR("Cao Rongrong <rrcao@ambarella.com>");
MODULE_DESCRIPTION("Ambarella ADC driver");
MODULE_LICENSE("GPL v2");
