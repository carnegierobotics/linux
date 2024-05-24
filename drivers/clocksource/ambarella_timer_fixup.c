// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Ambarella International LP
 *
 */
#include <linux/clk.h>

extern int ambarella_aarch64_cntfrq_update(void);
static struct clocksource csfake;

static void ambarella_update_cntfrq_freq(void *unused)
{
	ambarella_aarch64_cntfrq_update();
	clockevents_update_freq(raw_cpu_ptr(arch_timer_evt),
			arch_timer_get_cntfrq());
}

int ambarella_clksrc_nb_callback(struct notifier_block *nb,
		unsigned long event, void *data)
{
	int rval;
	switch (event) {
	case PRE_RATE_CHANGE:
		/*
		 * Create and register a fake clocksource.
		 */
		memcpy(&csfake, &clocksource_counter, sizeof(struct clocksource));
		csfake.name = "arm_fake_timer";
		rval = clocksource_register_hz(&csfake, arch_timer_get_cntfrq());
		if (rval)
			break;
		rval = clocksource_unregister(&clocksource_counter);
		break;

	case POST_RATE_CHANGE:
		on_each_cpu(ambarella_update_cntfrq_freq, NULL, 1);
		rval = clocksource_register_hz(&clocksource_counter, arch_timer_get_cntfrq());
		if (rval)
			break;

		/* unregister */
		rval = clocksource_unregister(&csfake);
		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}
