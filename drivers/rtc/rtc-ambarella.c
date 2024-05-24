/*
 * drivers/rtc/ambarella_rtc.c
 *
 * History:
 *	2008/04/01 - [Cao Rongrong] Support pause and resume
 *	2009/01/22 - [Anthony Ginger] Port to 2.6.28
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
#include <linux/io.h>
#include <linux/of.h>
#include <linux/sys_soc.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/rtc.h>
#include <linux/bcd.h>

/* ==========================================================================*/

#define RTC_CURT_WRITE_OFFSET		0x30
#define RTC_CURT_READ_OFFSET		0x34
#define RTC_PCRST_V0_OFFSET		0x40
#define RTC_PCRST_V1_OFFSET		0x38
#define RTC_CTRL_V0_OFFSET		0xFC
#define RTC_CTRL_V1_OFFSET		0x48

/* ==========================================================================*/
struct ambarella_rtc_pdata {
	u32 bc_enable;
	u32 pcrst_offset;
	u32 rtc_ctl_offset;
};

struct ambarella_rtc {
	struct rtc_device *rtc;
	void __iomem *base;
	struct device *dev;
	u32 bc_enable;
	u32 pcrst_offset;
	u32 rtc_ctl_offset;
};

static int ambrtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct ambarella_rtc *ambrtc;
	u32 time_sec;

	ambrtc = dev_get_drvdata(dev);
	time_sec = readl(ambrtc->base + RTC_CURT_READ_OFFSET);

	rtc_time64_to_tm(time_sec, tm);

	return 0;
}

static int ambrtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct ambarella_rtc *ambrtc = dev_get_drvdata(dev);
	time64_t secs = tm ? rtc_tm_to_time64(tm) : 0;

	writel(secs, ambrtc->base + RTC_CURT_WRITE_OFFSET);

	if (ambrtc->bc_enable)
		writel(0x1, ambrtc->base + ambrtc->rtc_ctl_offset);

	writel(0x1, ambrtc->base + ambrtc->pcrst_offset);
	msleep(5);
	writel(0x0, ambrtc->base + ambrtc->pcrst_offset);

	if (ambrtc->bc_enable)
		writel(0x0, ambrtc->base + ambrtc->rtc_ctl_offset);

	return 0;
}

static const struct rtc_class_ops ambarella_rtc_ops = {
	.read_time	= ambrtc_read_time,
	.set_time	= ambrtc_set_time,
};

/* chip before CV22 */
static const struct ambarella_rtc_pdata amba_rtc_rev0_pdata = {
	.bc_enable = 1,
	.pcrst_offset = RTC_PCRST_V0_OFFSET,
	.rtc_ctl_offset = RTC_CTRL_V0_OFFSET,
};

/* CV22 */
static const struct ambarella_rtc_pdata amba_rtc_rev1_pdata = {
	.bc_enable = 1,
	.pcrst_offset = RTC_PCRST_V0_OFFSET,
	.rtc_ctl_offset = RTC_CTRL_V1_OFFSET,
};

/* CV25 CV28 S6LM CV5 */
static const struct ambarella_rtc_pdata amba_rtc_rev2_pdata = {
	.bc_enable = 0,
	.pcrst_offset = RTC_PCRST_V0_OFFSET,
};

/* CV3 CV72 */
static const struct ambarella_rtc_pdata amba_rtc_rev3_pdata = {
	.bc_enable = 0,
	.pcrst_offset = RTC_PCRST_V1_OFFSET,
};

static const struct soc_device_attribute ambarella_rtc_socinfo[] = {
	{ .family = "Ambarella 14nm", .data = &amba_rtc_rev0_pdata },
	{ .soc_id = "cv2",		.data = &amba_rtc_rev0_pdata },
	{ .soc_id = "cv22",		.data = &amba_rtc_rev1_pdata },
	{ .soc_id = "cv25",		.data = &amba_rtc_rev2_pdata },
	{ .soc_id = "s6lm",		.data = &amba_rtc_rev2_pdata },
	{ .soc_id = "cv28m",		.data = &amba_rtc_rev2_pdata },
	{ .soc_id = "cv5",		.data = &amba_rtc_rev2_pdata },
	{ .soc_id = "cv3",		.data = &amba_rtc_rev3_pdata },
	{ .soc_id = "cv72",		.data = &amba_rtc_rev3_pdata },
	{ .soc_id = "cv3ad685",		.data = &amba_rtc_rev3_pdata },
	{/* sentinel */}
};

static int ambrtc_probe(struct platform_device *pdev)
{
	const struct soc_device_attribute *soc;
	const struct ambarella_rtc_pdata *socdata;
	struct ambarella_rtc *ambrtc;

	ambrtc = devm_kzalloc(&pdev->dev, sizeof(*ambrtc), GFP_KERNEL);
	if (!ambrtc)
		return -ENOMEM;

	ambrtc->dev = &pdev->dev;
	platform_set_drvdata(pdev, ambrtc);

	ambrtc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ambrtc->base)) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return PTR_ERR(ambrtc->base);
	}

	soc = soc_device_match(ambarella_rtc_socinfo);
	if (!soc || !soc->data) {
		dev_err(&pdev->dev, "Unknown SoC!\n");
		return -ENODEV;
	}

	socdata = soc->data;
	ambrtc->pcrst_offset = socdata->pcrst_offset;
	ambrtc->bc_enable = socdata->bc_enable;
	if (ambrtc->bc_enable)
		ambrtc->rtc_ctl_offset = socdata->rtc_ctl_offset;

	ambrtc->rtc = devm_rtc_device_register(&pdev->dev, pdev->name,
			&ambarella_rtc_ops, THIS_MODULE);
	if (IS_ERR(ambrtc->rtc)) {
		dev_err(&pdev->dev, "devm_rtc_device_register fail.\n");
		return PTR_ERR(ambrtc->rtc);
	}

	ambrtc->rtc->uie_unsupported = 1;

	return 0;
}

static int ambrtc_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct of_device_id ambarella_rtc_dt_ids[] = {
	{.compatible = "ambarella,rtc", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_rtc_dt_ids);

static struct platform_driver ambarella_rtc_driver = {
	.probe		= ambrtc_probe,
	.remove		= ambrtc_remove,
	.driver		= {
		.name	= "ambarella-rtc",
		.owner	= THIS_MODULE,
		.of_match_table = ambarella_rtc_dt_ids,
	},
};

module_platform_driver(ambarella_rtc_driver);

MODULE_DESCRIPTION("Ambarella Onchip RTC Driver.v200");
MODULE_AUTHOR("Cao Rongrong <rrcao@ambarella.com>");
MODULE_LICENSE("GPL");

