// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016-2048, Ambarella, Inc.
 */

#include <soc/ambarella/misc.h>

/*
 * Copy data from IO memory space to "real" memory space, at most 4 bytes per __raw_writeq.
 */
void __memcpy_fromio_ambarella(void *to, const volatile void __iomem *from, size_t count)
{
	while (count && !IS_ALIGNED((unsigned long)from, 4)) {
		*(u8 *)to = __raw_readb(from);
		from++;
		to++;
		count--;
	}

	while (count >= 4) {
		*(u32 *)to = __raw_readl(from);
		from += 4;
		to += 4;
		count -= 4;
	}

	while (count) {
		*(u8 *)to = __raw_readb(from);
		from++;
		to++;
		count--;
	}
}
EXPORT_SYMBOL(__memcpy_fromio_ambarella);

/*
 * Copy data from "real" memory space to IO memory space, at most 4 bytes per __raw_writeq.
 */
void __memcpy_toio_ambarella(volatile void __iomem *to, const void *from, size_t count)
{
	while (count && !IS_ALIGNED((unsigned long)to, 4)) {
		__raw_writeb(*(u8 *)from, to);
		from++;
		to++;
		count--;
	}

	while (count >= 4) {
		__raw_writel(*(u32 *)from, to);
		from += 4;
		to += 4;
		count -= 4;
	}

	while (count) {
		__raw_writeb(*(u8 *)from, to);
		from++;
		to++;
		count--;
	}
}
EXPORT_SYMBOL(__memcpy_toio_ambarella);
