/*
 *  fs/partitions/ambptb.c
 *
 * Author: Anthony Ginger <hfjiang@ambarella.com>
 *
 * Copyright (C) 2004-2010, Ambarella, Inc.
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

#include "check.h"
#include "ambptb.h"
#include <linux/of.h>

#if 0
#define ambptb_prt printk
#else
#define ambptb_prt(format, arg...) do {} while (0)
#endif

int ambptb_partition(struct parsed_partitions *state)
{
	struct device_node *np = NULL, *pp;
	struct device *dev;
	int sector_size, index = 1, rval = -1;

	dev = disk_to_dev(state->disk);

	while (dev) {
		if (of_device_is_compatible(dev->of_node, "ambarella,sdmmc") ||
			of_device_is_compatible(dev->of_node, "ambarella,sdhci")) {
			np = of_get_child_by_name(dev->of_node, "partitions");
			break;
		}

		dev = dev->parent;
	}

	if (np == NULL)
		return 0;

	sector_size = queue_logical_block_size(state->disk->queue);

	for_each_child_of_node(np,  pp) {
		char str_tmp[1 + BDEVNAME_SIZE + 10 + 1];
		const char *partition_name;
		const __be32 *reg;
		int len, a_cells, s_cells;
		u64 addr, size;

		reg = of_get_property(pp, "reg", &len);
		if (!reg)
			goto exit;

		a_cells = of_n_addr_cells(pp);
		s_cells = of_n_size_cells(pp);
		if (len / 4 != a_cells + s_cells)
			goto exit;

		addr = of_read_number(reg, a_cells);
		do_div(addr, sector_size);
		size = of_read_number(reg + a_cells, s_cells);
		do_div(size, sector_size);

		put_partition(state, index++, addr, size);

		partition_name = of_get_property(pp, "label", &len);
		if (!partition_name)
			goto exit;

		snprintf(str_tmp, sizeof(str_tmp), "(%s)", partition_name);
		strlcat(state->pp_buf, str_tmp, PAGE_SIZE);

		printk(KERN_INFO " %s: 0x%016llx, 0x%016llx\n", str_tmp,
			of_read_number(reg, a_cells),
			of_read_number(reg + a_cells, s_cells));
	}

	strlcat(state->pp_buf, "\n", PAGE_SIZE);
	rval = 1;

exit:
	if (np)
		of_node_put(np);
	return rval;
}

