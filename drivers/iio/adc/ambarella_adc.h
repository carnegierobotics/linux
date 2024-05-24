/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2014-2019, Ambarella, Inc.
 *  Author: Cao Rongrong <rrcao@ambarella.com>
 */

/* ==========================================================================*/

#define ADC_STATUS_OFFSET		0x000
#define ADC_CONTROL_OFFSET		0x004
#define ADC_COUNTER_OFFSET		0x008
#define ADC_SLOT_NUM_OFFSET		0x00c
#define ADC_SLOT_PERIOD_OFFSET		0x010
#define ADC_CTRL_INTR_TABLE_OFFSET	0x044
#define ADC_DATA_INTR_TABLE_OFFSET	0x048
#define ADC_FIFO_INTR_TABLE_OFFSET	0x04c
#define ADC_ERR_STATUS_OFFSET		0x050

#define ADC_SLOT_CTRL_0_OFFSET		0x100
#define ADC_SLOT_CTRL_1_OFFSET		0x104
#define ADC_SLOT_CTRL_2_OFFSET		0x108
#define ADC_SLOT_CTRL_3_OFFSET		0x10c
#define ADC_SLOT_CTRL_4_OFFSET		0x110
#define ADC_SLOT_CTRL_5_OFFSET		0x114
#define ADC_SLOT_CTRL_6_OFFSET		0x118
#define ADC_SLOT_CTRL_7_OFFSET		0x11c

#define ADC_CHAN0_INTR_OFFSET		0x120
#define ADC_CHAN1_INTR_OFFSET		0x124
#define ADC_CHAN2_INTR_OFFSET		0x128
#define ADC_CHAN3_INTR_OFFSET		0x12c
#define ADC_CHAN4_INTR_OFFSET		0x130
#define ADC_CHAN5_INTR_OFFSET		0x134
#define ADC_CHAN6_INTR_OFFSET		0x138
#define ADC_CHAN7_INTR_OFFSET		0x13c

#define ADC_DATA0_OFFSET		0x150
#define ADC_DATA1_OFFSET		0x154
#define ADC_DATA2_OFFSET		0x158
#define ADC_DATA3_OFFSET		0x15c
#define ADC_DATA4_OFFSET		0x160
#define ADC_DATA5_OFFSET		0x164
#define ADC_DATA6_OFFSET		0x168
#define ADC_DATA7_OFFSET		0x16c

#define ADC_FIFO_CTRL_0_OFFSET		0x180
#define ADC_FIFO_CTRL_1_OFFSET		0x184
#define ADC_FIFO_CTRL_2_OFFSET		0x188
#define ADC_FIFO_CTRL_3_OFFSET		0x18C
#define ADC_FIFO_CTRL_OFFSET		0x190

#define ADC_FIFO_STATUS_0_OFFSET	0x1a0
#define ADC_FIFO_STATUS_1_OFFSET	0x1a4
#define ADC_FIFO_STATUS_2_OFFSET	0x1a8
#define ADC_FIFO_STATUS_3_OFFSET	0x1ac

#define ADC_FIFO_DATA0_OFFSET		0x200
#define ADC_FIFO_DATA1_OFFSET		0x280
#define ADC_FIFO_DATA2_OFFSET		0x300
#define ADC_FIFO_DATA3_OFFSET		0x380

#define ADC_SLOT_CTRL_X_OFFSET(n)	(ADC_SLOT_CTRL_0_OFFSET + (n) * 4)
#define ADC_INT_CTRL_X_OFFSET(n)	(ADC_CHAN0_INTR_OFFSET + (n) * 4)
#define ADC_DATA_X_OFFSET(n)		(ADC_DATA0_OFFSET + (n) * 4)
#define ADC_FIFO_CTRL_X_OFFSET(n)	(ADC_FIFO_CTRL_0_OFFSET + (n) * 4)
#define ADC_FIFO_STATUS_X_OFFSET(n)	(ADC_FIFO_STATUS_0_OFFSET + (n) * 4)
#define ADC_FIFO_DATA_X_OFFSET(n)	(ADC_FIFO_DATA0_OFFSET + (n) * 0x80)


/* ==========================================================================*/

#define ADC_CONTROL_CLEAR		0x01
#define ADC_CONTROL_MODE		0x02
#define ADC_CONTROL_ENABLE		0x04
#define ADC_CONTROL_START		0x08

#define ADC_FIFO_OVER_INT_EN		(0x1 << 31)
#define ADC_FIFO_UNDR_INT_EN		(0x1 << 30)
#define ADC_FIFO_DEPTH			0x80
#define ADC_FIFO_TH(n)			((n) << 16)
#define ADC_FIFO_CLEAR			0x1
#define ADC_FIFO_ID(n)			((n) << 12)
#define ADC_FIFO_NUMBER		        0x04

#define ADC_INT_THRESHOLD_EN		(1 << 31)
#define ADC_VAL_HI(x)			(((x) & 0xfff) << 16)
#define ADC_VAL_LO(x)			((x) & 0xfff)

/* ==========================================================================*/

#define ADC_POWER_DOWN			0x2
#define ADC_SCALER_POWER_DOWN		0xf00

/* ==========================================================================*/

#define ADC_MAX_CHANNEL_NUM		8
#define ADC_PERIOD_CYCLE		20
#define ADC_MAX_CLOCK			12000000
#define ADC_MAX_FIFO_DEPTH		1024

/* ==========================================================================*/

#define AMBARELLA_ADC_KEY_DEBOUNCE	100

struct ambadc_keymap {
	u32 key_code;
	u32 channel : 4;
	u32 low_level : 12;
	u32 high_level : 12;
};

enum {
	ADC16_CTRL_OFFSET = 0,
	T2V_CTRL_OFFSET,
	T2V_CALIB_DATA_OFFSET,
	RCT_ADC_REG_NUM,
};

struct ambarella_adc {
	struct device *dev;
	void __iomem *regbase;
	struct regmap *rct_regmap;
	u32 rct_offset[RCT_ADC_REG_NUM];
	int irq;
	struct clk *clk;
	u32 clk_rate;
	struct mutex mtx;
	struct iio_dev *indio_dev;
	struct iio_trigger *trig;
	unsigned long channels_mask;
	unsigned long scalers_mask; /* 1.8v if corresponding bit is set */
	unsigned long fifo_enable_mask;
	u32 channel_num;
	u32 vol_threshold[ADC_MAX_CHANNEL_NUM];
	u32 fifo_threshold;
	int t2v_channel;
	u32 t2v_offset;
	u32 t2v_coeff;

	struct work_struct work;
	u32 data_intr;

	/* following are for adc key if existed */
	struct input_dev *input;
	struct ambadc_keymap *keymap;
	u32 key_num;
	u32 key_pressed[ADC_MAX_CHANNEL_NUM]; /* save the key currently pressed */
};

