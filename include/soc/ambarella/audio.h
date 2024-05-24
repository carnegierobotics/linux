/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Author: Cao Rongrong <rrcao@ambarella.com>
 *
 * Copyright (C) 2004-2010, Ambarella, Inc.
 */

#ifndef __SOC_AMBARELLA_AUDIO_H__
#define __SOC_AMBARELLA_AUDIO_H__

struct notifier_block;

struct ambarella_i2s_interface {
	u32 state;
	u32 mode;
	u32 clksrc;
	u32 mclk;
	u32 sfreq;
	u32 channels;
	u32 word_len;
	u32 word_pos;
	u32 slots;
	u32 rx_ctrl;
	u32 tx_ctrl;
	u32 rx_fifo_len;
	u32 tx_fifo_len;
	u32 multi24;
	u32 ws_set;
};

enum Audio_Notify_Type
{
	AUDIO_NOTIFY_UNKNOWN,
	AUDIO_NOTIFY_INIT,
	AUDIO_NOTIFY_SETHWPARAMS,
	AUDIO_NOTIFY_REMOVE
};

extern int ambarella_audio_register_notifier(struct notifier_block *nb);
extern int ambarella_audio_unregister_notifier(struct notifier_block *nb);

extern struct ambarella_i2s_interface get_audio_i2s_interface(void);

#endif /* __SOC_AMBARELLA_AUDIO_H__ */

