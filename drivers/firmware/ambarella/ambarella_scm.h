/*
 * Ambarella SCM driver
 *
 * Copyright (C) 2017 Ambarella Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __AMBARELLA_SMC__
#define __AMBARELLA_SMC__

/* cpufreq svc ID 0x1 */
#define AMBA_SCM_SVC_FREQ			0x1
#define AMBA_SCM_CNTFRQ_SETUP_CMD		0x1

/* SYSTEM power manager 0x2 */
#define AMBA_SCM_SVC_PM				0x2	/* Deprecated */
#define AMBA_SCM_PM_GPIO_SETUP			0x1	/* Deprecated */

/* Register Access 0x3 */
#define AMBA_SIP_ACCESS_REG			0x3	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_READ		0x1	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_WRITE		0x2	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_SETBIT		0x3	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_CLRBIT		0x4	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_READ8		0x5	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_READ16		0x6	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_READ32		0x7	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_READ64		0x8	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_WRITE8		0x9	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_WRITE16		0xa	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_WRITE32		0xb	/* Deprecated */
#define AMBA_SIP_ACCESS_REG_WRITE64		0xc	/* Deprecated */


/* Switch to AARCH32 0x4 */
#define AMBA_SIP_SWITCH_TO_AARCH32		0x4
#define AMBA_SIP_AARCH32_KERNEL			0x1

/* OTP operation 0x5 */
#define AMBA_SIP_ACCESS_OTP			0x5
#define AMBA_SIP_GET_AMBA_UNIQUE_ID		0x1

/* Authentication 0x6 */
#define AMBA_SIP_SVC_AUTH			0x6	/* deprecated */
#define AMBA_SIP_AUTH_INIT			0x1
#define AMBA_SIP_AUTH_PK			0x2
#define AMBA_SIP_AUTH_SIG			0x3
#define AMBA_SIP_AUTH_DATA			0x4
#define AMBA_SIP_AUTH_VERIFY			0x5
#define AMBA_SIP_AUTH_EXIT			0x6

/* Stage2 translation SIP 0x7 */
#define AMBA_SIP_SVC_EL2_FAULT			0x7
#define AMBA_SIP_EL2_DATA_ABORT			0x1

/* Memory monitor 0x8 */
#define AMBA_SIP_MEMORY_MONITOR			0x8
#define AMBA_SIP_MONITOR_CONFIG			0x1
#define AMBA_SIP_MONITOR_ENABLE			0x2
#define AMBA_SIP_MONITOR_DISABLE		0x3

/* VP Software reset 0x9 */
#define AMBA_SIP_VP_CONFIG			0x9
#define AMBA_SIP_VP_CONFIG_RESET		0x1

#define AMBA_SIP_LP5_ADJUST 			0xb
#define AMBA_SIP_LP5_ADJUST_ISLP5		0x1
#define AMBA_SIP_LP5_ADJUST_INIT		0x2
#define AMBA_SIP_LP5_ADJUST_RUN 		0x3
#define AMBA_SIP_LP5_ADJUST_SET_PVAL 		0x4
#define AMBA_SIP_LP5_ADJUST_GET_PVAL 		0x5
#define AMBA_SIP_LP5_ADJUST_SET_NVAL 		0x6
#define AMBA_SIP_LP5_ADJUST_GET_NVAL 		0x7
#define AMBA_SIP_LP5_ADJUST_SHOW_SWITCH 	0x8
#define AMBA_SIP_LP5_ADJUST_SET_WCK2DQI_TIMER	0x9

/* HSM smc call */
#define AMBA_SIP_HSM_CALL                       0xc
#define AMBA_SIP_HSM_INIT_QUEUE                 0x1
#define AMBA_SIP_HSM_BOOT_DECRYPTAUTH           0x2
#define AMBA_SIP_HSM_BOOT_DOMAIN		0x3
#define AMBA_SIP_HSM_DMA_CHAN_ID		0x4
#define AMBA_SIP_HSM_DOMALLOC			0x5

/* XXX svc ID 0xff */
#define AMBA_SCM_SVC_QUERY			0xff
#define AMBA_SCM_QUERY_COUNT			0x00
#define AMBA_SCM_QUERY_UID			0x01
#define AMBA_SCM_QUERY_VERSION			0x03

#define	SVC_SCM_FN(s, f)			((((s) & 0xff) << 8) | ((f) & 0xff))

#define SVC_OF_SMC(c)				(((c) >> 8) & 0xff)
#define FNID_OF_SMC(c)				((c) & 0xff)
#endif
