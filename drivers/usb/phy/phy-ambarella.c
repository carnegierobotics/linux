/*
* linux/drivers/usb/phy/phy-ambarella.c
*
* History:
*	2014/01/28 - [Cao Rongrong] created file
*
* Copyright (C) 2012 by Ambarella, Inc.
* http://www.ambarella.com
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
* along with this program; if not, write to the
* Free Software Foundation, Inc., 59 Temple Place - Suite 330,
* Boston, MA  02111-1307, USA.
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/usb/otg.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/io.h>
#include <linux/of_gpio.h>
#include <linux/uaccess.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <soc/ambarella/misc.h>

/* ==========================================================================*/

#define USB0_IDDIG0_MASK		0x00000000
#define USB0_IS_HOST_MASK		0x00000001

/* ==========================================================================*/

#define DRIVER_NAME "ambarella_phy"

/*
 * PORT is termed as usb slot which is outside the chip, and
 * PHY is termed as usb interface which is inside the chip
 */

#define PHY_TO_DEVICE_PORT	0 /* rotue D+/D- signal to device port */
#define PHY_TO_HOST_PORT	1 /* rotue D+/D- signal to host port */

/* PORT_TYPE_XXX is only for the device port */
#define PORT_TYPE_DEVICE	0 /* we should work as device */
#define PORT_TYPE_OTG		1 /* we should work as host */

#define setbitsl(a,v)   (writel_relaxed(((v) | readl_relaxed(a)), (a)))
#define clrbitsl(a,v)   (writel_relaxed(((~(v)) & readl_relaxed(a)), (a)))

struct ambarella_phy {
	struct usb_phy phy;
	void __iomem *pol_reg;
	void __iomem *ana_reg;
	void __iomem *own_reg;
	void __iomem *ctrl_reg;
	void __iomem *ctrl2_reg;
	struct regmap *ana_regmap;
	struct regmap *own_regmap;
	struct regmap *phy_regmap;
	struct regmap *pol_regmap;
	u32 ana_offset;
	u32 own_offset;
	u32 phy_offset;
	u32 pol_offset;

	u32 host_phy_num;
	u32 ovrcur_pol_inv;
	bool usbp_ctrl_set;
	u32 ctrl_device[2];
	u32 ctrl_host[2];

	int gpio_id;
	bool id_is_otg;
	int gpio_md;
	bool md_host_active;
	int gpio_hub;
	bool hub_active;
	u8 port_type;	/* the behavior of the device port working */
	u8 phy_route;	/* route D+/D- signal to device or host port */

#ifdef CONFIG_PM
	u32 pol_val;
	u32 own_val;
#endif
};

#define to_ambarella_phy(p) container_of((p), struct ambarella_phy, phy)

static inline bool ambarella_usb0_is_host(struct ambarella_phy *amb_phy)
{
	u32 val;

	if (amb_phy->own_regmap)
		regmap_read(amb_phy->own_regmap, amb_phy->own_offset, &val);
	else
		val = readl_relaxed(amb_phy->own_reg);

	return !(val & USB0_IS_HOST_MASK);
}

static inline void ambarella_switch_to_host(struct ambarella_phy *amb_phy)
{
	if (amb_phy->own_regmap) {
		regmap_update_bits(amb_phy->own_regmap, amb_phy->own_offset,
			USB0_IS_HOST_MASK, 0);

		if (amb_phy->usbp_ctrl_set) {
			regmap_write(amb_phy->phy_regmap, amb_phy->phy_offset + 0,
				amb_phy->ctrl_host[0]);
			regmap_write(amb_phy->phy_regmap, amb_phy->phy_offset + 4,
				amb_phy->ctrl_host[1]);
		}
	} else {
		clrbitsl(amb_phy->own_reg, USB0_IS_HOST_MASK);

		if (amb_phy->usbp_ctrl_set) {
			writel_relaxed(amb_phy->ctrl_host[0], amb_phy->ctrl_reg);
			writel_relaxed(amb_phy->ctrl_host[1], amb_phy->ctrl2_reg);
		}
	}
}

static inline void ambarella_switch_to_device(struct ambarella_phy *amb_phy)
{
	if (amb_phy->own_regmap) {
		regmap_update_bits(amb_phy->own_regmap, amb_phy->own_offset,
			USB0_IS_HOST_MASK, USB0_IS_HOST_MASK);

		if (amb_phy->usbp_ctrl_set) {
			regmap_write(amb_phy->phy_regmap, amb_phy->phy_offset + 0,
				amb_phy->ctrl_device[0]);
			regmap_write(amb_phy->phy_regmap, amb_phy->phy_offset + 4,
				amb_phy->ctrl_device[1]);
		}
	} else {
		setbitsl(amb_phy->own_reg, USB0_IS_HOST_MASK);

		if (amb_phy->usbp_ctrl_set) {
			writel_relaxed(amb_phy->ctrl_device[0], amb_phy->ctrl_reg);
			writel_relaxed(amb_phy->ctrl_device[1], amb_phy->ctrl2_reg);
		}
	}
}

static inline void ambarella_check_otg(struct ambarella_phy *amb_phy)
{
	/*
	 * if D+/D- is routed to device port which is working
	 * as otg, we need to switch the phy to host mode.
	 */
	if (amb_phy->phy_route == PHY_TO_DEVICE_PORT) {
		if (amb_phy->port_type == PORT_TYPE_OTG)
			ambarella_switch_to_host(amb_phy);
		else
			ambarella_switch_to_device(amb_phy);
	}
}

static int ambarella_phy_proc_show(struct seq_file *m, void *v)
{
	struct ambarella_phy *amb_phy = m->private;
	const char *port_status;
	const char *phy_status;

	if (amb_phy->phy_route == PHY_TO_HOST_PORT)
		port_status = "HOST";
	else
		port_status = "DEVICE";

	if (ambarella_usb0_is_host(amb_phy))
		phy_status = "HOST";
	else
		phy_status = "DEVICE";

	seq_printf(m, "Possible parameter: host device\n");
	seq_printf(m, "Current status:\n");
	if (gpio_is_valid(amb_phy->gpio_md))
		seq_printf(m, "\tport is %s\n", port_status);
	seq_printf(m, "\tphy is %s\n\n", phy_status);

	return 0;
}

static ssize_t ambarella_phy_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *ppos)
{
	struct ambarella_phy *amb_phy = PDE_DATA(file_inode(file));
	char n, str[32];

	n = (count < 32) ? count : 32;

	if (copy_from_user(str, buffer, n))
		return -EFAULT;

	str[n - 1] = '\0';

	if (!strcasecmp(str, "host")) {
		if (gpio_is_valid(amb_phy->gpio_md)) {
			amb_phy->phy_route = PHY_TO_HOST_PORT;
			gpio_direction_output(amb_phy->gpio_md,
						amb_phy->md_host_active);
		}

		ambarella_switch_to_host(amb_phy);
	} else if (!strcasecmp(str, "device")) {
		if (gpio_is_valid(amb_phy->gpio_md)) {
			amb_phy->phy_route = PHY_TO_DEVICE_PORT;
			gpio_direction_output(amb_phy->gpio_md,
						!amb_phy->md_host_active);
		}

		ambarella_check_otg(amb_phy);
	} else {
		pr_err("Invalid argument!\n");
	}

	return count;
}

static int ambarella_phy_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, ambarella_phy_proc_show, PDE_DATA(inode));
}

static const struct proc_ops proc_phy_switcher_fops = {
	.proc_open = ambarella_phy_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_write = ambarella_phy_proc_write,
	.proc_release = single_release,
};

static irqreturn_t ambarella_otg_detect_irq(int irq, void *dev_id)
{
	struct ambarella_phy *amb_phy = dev_id;

	if (gpio_is_valid(amb_phy->gpio_id)) {
		if (gpio_get_value_cansleep(amb_phy->gpio_id) == amb_phy->id_is_otg)
			amb_phy->port_type = PORT_TYPE_OTG;
		else
			amb_phy->port_type = PORT_TYPE_DEVICE;
	} else {
		amb_phy->port_type = PORT_TYPE_DEVICE;
	}

	ambarella_check_otg(amb_phy);

	return IRQ_HANDLED;
}

static int ambarella_init_phy_switcher(struct ambarella_phy *amb_phy)
{
	struct usb_phy *phy = &amb_phy->phy;
	int irq, rval = 0;

	/* only usb0 support to switch between host and device */
	/* This phy is used for platforms before CV5 */
	proc_create_data("usbphy0", S_IRUGO|S_IWUSR,
		ambarella_procfs_dir(), &proc_phy_switcher_fops, amb_phy);

	/* setup over-current polarity */
	if (amb_phy->pol_regmap) {
		regmap_update_bits(amb_phy->pol_regmap, amb_phy->pol_offset,
			BIT(13), amb_phy->ovrcur_pol_inv);
		regmap_update_bits(amb_phy->own_regmap, amb_phy->own_offset,
			USB0_IDDIG0_MASK, USB0_IDDIG0_MASK);
	} else {
		clrbitsl(amb_phy->pol_reg, 0x1 << 13);
		setbitsl(amb_phy->pol_reg, amb_phy->ovrcur_pol_inv);
		setbitsl(amb_phy->own_reg, USB0_IDDIG0_MASK);
	}

	/* request gpio for PHY HUB reset */
	if (gpio_is_valid(amb_phy->gpio_hub)) {
		rval = devm_gpio_request(phy->dev, amb_phy->gpio_hub, "hub reset");
		if (rval < 0) {
			dev_err(phy->dev, "Failed to request hub reset pin %d\n", rval);
			return rval;
		}
		gpio_direction_output(amb_phy->gpio_hub, !amb_phy->hub_active);
	}

	if (gpio_is_valid(amb_phy->gpio_id)) {
		rval = devm_gpio_request_one(phy->dev,
			amb_phy->gpio_id, GPIOF_DIR_IN, "otg_id");
		if (rval < 0){
			dev_err(phy->dev, "Failed to request id pin %d\n", rval);
			return rval;
		}

		if (gpio_get_value_cansleep(amb_phy->gpio_id) == amb_phy->id_is_otg)
			amb_phy->port_type = PORT_TYPE_OTG;
		else
			amb_phy->port_type = PORT_TYPE_DEVICE;

		irq = gpio_to_irq(amb_phy->gpio_id);

		rval = devm_request_threaded_irq(phy->dev, irq, NULL,
			ambarella_otg_detect_irq,
			IRQ_TYPE_EDGE_BOTH | IRQF_ONESHOT,
			"usb_otg_id", amb_phy);
		if (rval) {
			dev_err(phy->dev, "request usb_otg_id irq failed: %d\n", rval);
			return rval;
		}

	} else {
		amb_phy->port_type = PORT_TYPE_DEVICE;
	}

	/* rotue D+/D- signal to host or device port if control gpio existed */
	if (gpio_is_valid(amb_phy->gpio_md)) {
		rval = devm_gpio_request(phy->dev,
				amb_phy->gpio_md, "phy_switcher");
		if (rval) {
			dev_err(phy->dev, "request phy_switcher gpio failed\n");
			return rval;
		}

		/*
		 * if usb0 is configured as host, route D+/D- signal to
		 * host port, otherwise route them to device port.
		 */
		if (ambarella_usb0_is_host(amb_phy)) {
			amb_phy->phy_route = PHY_TO_HOST_PORT;
			gpio_direction_output(amb_phy->gpio_md,
						amb_phy->md_host_active);
		} else {
			amb_phy->phy_route = PHY_TO_DEVICE_PORT;
			gpio_direction_output(amb_phy->gpio_md,
						!amb_phy->md_host_active);
		}
	} else {
		amb_phy->phy_route = PHY_TO_DEVICE_PORT;
	}

	ambarella_check_otg(amb_phy);

	return 0;
}

static void ambarella_phy_of_parse(struct platform_device *pdev,
			struct ambarella_phy *amb_phy)
{
	struct device_node *np = pdev->dev.of_node;
	enum of_gpio_flags flags;
	int rval;

	rval = of_property_read_u32(np, "amb,host-phy-num", &amb_phy->host_phy_num);
	if (rval < 0)
		amb_phy->host_phy_num = 1;

	rval = of_property_read_u32(np, "amb,ocp-polarity", &amb_phy->ovrcur_pol_inv);
	if (rval < 0)
		amb_phy->ovrcur_pol_inv = 0;

	amb_phy->ovrcur_pol_inv = (!!amb_phy->ovrcur_pol_inv) << 13;

	rval = of_property_read_u32_array(np, "amb,ctrl-device", amb_phy->ctrl_device, 2);
	if (rval == 0)
		amb_phy->usbp_ctrl_set = true;

	rval = of_property_read_u32_array(np, "amb,ctrl-host", amb_phy->ctrl_host, 2);
	if (rval == 0)
		amb_phy->usbp_ctrl_set = true;

	amb_phy->gpio_id = of_get_named_gpio_flags(np, "id-gpios", 0, &flags);
	amb_phy->id_is_otg = !!(flags & OF_GPIO_ACTIVE_LOW);

	amb_phy->gpio_md = of_get_named_gpio_flags(np, "md-gpios", 0, &flags);
	amb_phy->md_host_active = !!(flags & OF_GPIO_ACTIVE_LOW);

	amb_phy->gpio_hub = of_get_named_gpio_flags(np, "hub-gpios", 0, &flags);
	amb_phy->hub_active = !!(flags & OF_GPIO_ACTIVE_LOW);
}

static int ambarella_phy_get_resource(struct platform_device *pdev,
			struct ambarella_phy *amb_phy)
{
	struct resource *mem;

	/* get register for usb phy power on */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	amb_phy->ana_reg = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(amb_phy->ana_reg)) {
		dev_err(&pdev->dev, "ana devm_ioremap() failed\n");
		return PTR_ERR(amb_phy->ana_reg);
	}

	/* get register for overcurrent polarity */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	amb_phy->pol_reg = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(amb_phy->pol_reg)) {
		dev_err(&pdev->dev, "pol devm_ioremap() failed\n");
		return PTR_ERR(amb_phy->pol_reg);
	}

	/* get register for usb phy owner configure */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	amb_phy->own_reg = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(amb_phy->own_reg)) {
		dev_err(&pdev->dev, "own devm_ioremap() failed\n");
		return PTR_ERR(amb_phy->own_reg);
	}

	/* get register for usb phy ctrl configure */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	if (mem) {
		amb_phy->ctrl_reg = devm_ioremap_resource(&pdev->dev, mem);
		if (IS_ERR(amb_phy->ctrl_reg)) {
			dev_err(&pdev->dev, "ctrl devm_ioremap() failed\n");
			return PTR_ERR(amb_phy->ctrl_reg);
		}
	}

	/* get register for usb phy ctrl2 configure */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 4);
	if (mem) {
		amb_phy->ctrl2_reg = devm_ioremap_resource(&pdev->dev, mem);
		if (IS_ERR(amb_phy->ctrl2_reg)) {
			dev_err(&pdev->dev, "ctrl2 devm_ioremap() failed\n");
			return PTR_ERR(amb_phy->ctrl2_reg);
		}
	}

	if (amb_phy->ctrl_reg && amb_phy->ctrl2_reg) {
		amb_phy->ctrl_device[0] = readl_relaxed(amb_phy->ctrl_reg);
		amb_phy->ctrl_device[1] = readl_relaxed(amb_phy->ctrl2_reg);
		amb_phy->ctrl_host[0] = amb_phy->ctrl_device[0];
		amb_phy->ctrl_host[1] = amb_phy->ctrl_device[1];
	}

	return 0;
}

static int ambarella_phy_get_regmap(struct platform_device *pdev,
			struct ambarella_phy *amb_phy)
{
	struct device_node *np = pdev->dev.of_node;

	/* get register for usb phy owner configure */
	amb_phy->own_regmap = syscon_regmap_lookup_by_phandle(np, "amb,own-regmap");
	if (IS_ERR(amb_phy->own_regmap)) {
		dev_err(&pdev->dev, "no own regmap!\n");
		return PTR_ERR(amb_phy->own_regmap);
	}

	if (of_property_read_u32_index(np, "amb,own-regmap", 1, &amb_phy->own_offset)) {
		dev_err(&pdev->dev, "couldn't get own offset\n");
		return -EINVAL;
	}

	/* get register for usb phy power on */
	amb_phy->ana_regmap = syscon_regmap_lookup_by_phandle(np, "amb,ana-regmap");
	if (IS_ERR(amb_phy->ana_regmap)) {
		dev_err(&pdev->dev, "no ana regmap!\n");
		return PTR_ERR(amb_phy->ana_regmap);
	}

	if (of_property_read_u32_index(np, "amb,ana-regmap", 1, &amb_phy->ana_offset)) {
		dev_err(&pdev->dev, "couldn't get ana offset\n");
		return -EINVAL;
	}

	/* get register for overcurrent polarity */
	amb_phy->pol_regmap = syscon_regmap_lookup_by_phandle(np, "amb,pol-regmap");
	if (IS_ERR(amb_phy->pol_regmap)) {
		dev_err(&pdev->dev, "no pol regmap!\n");
		return PTR_ERR(amb_phy->pol_regmap);
	}

	if (of_property_read_u32_index(np, "amb,pol-regmap", 1, &amb_phy->pol_offset)) {
		dev_err(&pdev->dev, "couldn't get pol offset\n");
		return -EINVAL;
	}

	/* get register for usb phy ctrl configure */
	amb_phy->phy_regmap = syscon_regmap_lookup_by_phandle(np, "amb,phy-regmap");
	if (IS_ERR(amb_phy->phy_regmap)) {
		dev_err(&pdev->dev, "no phy regmap!\n");
		return PTR_ERR(amb_phy->phy_regmap);
	}

	if (of_property_read_u32_index(np, "amb,phy-regmap", 1, &amb_phy->phy_offset)) {
		dev_err(&pdev->dev, "couldn't get phy offset\n");
		return -EINVAL;
	}

	regmap_read(amb_phy->phy_regmap, amb_phy->phy_offset + 0, &amb_phy->ctrl_device[0]);
	regmap_read(amb_phy->phy_regmap, amb_phy->phy_offset + 4, &amb_phy->ctrl_device[1]);
	amb_phy->ctrl_host[0] = amb_phy->ctrl_device[0];
	amb_phy->ctrl_host[1] = amb_phy->ctrl_device[1];

	return 0;
}

static int ambarella_phy_init(struct usb_phy *phy)
{
	struct ambarella_phy *amb_phy = to_ambarella_phy(phy);
	u32 ana_val = 0x3006;

	/*
	 * If there are 2 PHYs, no matter which PHY need to be initialized,
	 * we initialize all of them at the same time.
	 */
	if (amb_phy->ana_regmap) {
		regmap_update_bits(amb_phy->ana_regmap, amb_phy->ana_offset,
			ana_val, ana_val);
	} else {
		if (!(readl_relaxed(amb_phy->ana_reg) & ana_val))
			setbitsl(amb_phy->ana_reg, ana_val);
	}

	mdelay(1);

	return 0;
}

static int ambarella_phy_probe(struct platform_device *pdev)
{
	struct ambarella_phy *amb_phy;
	int rval;

	amb_phy = devm_kzalloc(&pdev->dev, sizeof(*amb_phy), GFP_KERNEL);
	if (!amb_phy) {
		dev_err(&pdev->dev, "Failed to allocate memory!\n");
		return -ENOMEM;
	}

	amb_phy->phy.otg = devm_kzalloc(&pdev->dev, sizeof(struct usb_otg),
					GFP_KERNEL);
	if (!amb_phy->phy.otg) {
		dev_err(&pdev->dev, "Failed to allocate memory!\n");
		return -ENOMEM;
	}

	amb_phy->phy.dev = &pdev->dev;
	amb_phy->phy.label = DRIVER_NAME;
	amb_phy->phy.init = ambarella_phy_init;

	rval = ambarella_phy_get_resource(pdev, amb_phy);
	if (rval < 0) {
		rval = ambarella_phy_get_regmap(pdev, amb_phy);
		if (rval < 0)
			return rval;
	}

	ambarella_phy_of_parse(pdev, amb_phy);

	ambarella_init_phy_switcher(amb_phy);

	platform_set_drvdata(pdev, &amb_phy->phy);

	rval = usb_add_phy_dev(&amb_phy->phy);
	if (rval < 0)
		return rval;

	return 0;
}

static int ambarella_phy_remove(struct platform_device *pdev)
{
	struct ambarella_phy *amb_phy = platform_get_drvdata(pdev);

	usb_remove_phy(&amb_phy->phy);

	return 0;
}

static void ambarella_phy_shutdown(struct platform_device *pdev)
{
	struct ambarella_phy *amb_phy = platform_get_drvdata(pdev);

	if (amb_phy->host_phy_num && gpio_is_valid(amb_phy->gpio_md)) {
		gpio_direction_output(amb_phy->gpio_md,
					!amb_phy->md_host_active);
	}

}

#ifdef CONFIG_PM
static int ambarella_phy_suspend(struct device *dev)
{
	struct platform_device	*pdev = to_platform_device(dev);
	struct ambarella_phy *amb_phy = platform_get_drvdata(pdev);

	if (amb_phy->pol_regmap) {
		regmap_read(amb_phy->pol_regmap, amb_phy->pol_offset, &amb_phy->pol_val);
		regmap_read(amb_phy->own_regmap, amb_phy->own_offset, &amb_phy->own_val);
	} else {
		amb_phy->pol_val = readl_relaxed(amb_phy->pol_reg);
		amb_phy->own_val = readl_relaxed(amb_phy->own_reg);
	}

	return 0;
}

static int ambarella_phy_resume(struct device *dev)
{
	struct platform_device	*pdev = to_platform_device(dev);
	struct ambarella_phy *amb_phy = platform_get_drvdata(pdev);

	if (amb_phy->pol_regmap) {
		regmap_write(amb_phy->pol_regmap, amb_phy->pol_offset, amb_phy->pol_val);
		regmap_write(amb_phy->own_regmap, amb_phy->own_offset, amb_phy->own_val);
	} else {
		writel_relaxed(amb_phy->pol_val, amb_phy->pol_reg);
		writel_relaxed(amb_phy->own_val, amb_phy->own_reg);
	}

	return 0;
}

static const struct dev_pm_ops ambarella_usbphy_dev_pm_ops = {

	/* suspend to mem */
	.suspend_late = ambarella_phy_suspend,
	.resume_early = ambarella_phy_resume,

	/* suspend to disk */
	.freeze = ambarella_phy_suspend,
	.thaw = ambarella_phy_resume,

	/* restore from suspend to disk */
	.restore = ambarella_phy_resume,
};
#endif

static const struct of_device_id ambarella_phy_dt_ids[] = {
	{ .compatible = "ambarella,usbphy", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ambarella_phy_dt_ids);

static struct platform_driver ambarella_phy_driver = {
	.probe = ambarella_phy_probe,
	.remove = ambarella_phy_remove,
	.shutdown = ambarella_phy_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = ambarella_phy_dt_ids,
#ifdef CONFIG_PM
		.pm = &ambarella_usbphy_dev_pm_ops,
#endif
	 },
};

/*
 * We have to call ambarella_phy_module_init() before the drives using USB PHY
 * like EHCI/OHCI/UDC, and after GPIO drivers including external GPIO chip, so
 * we use subsys_initcall_sync here.
 */
static int __init ambarella_phy_module_init(void)
{
	return platform_driver_register(&ambarella_phy_driver);
}
subsys_initcall_sync(ambarella_phy_module_init);

static void __exit ambarella_phy_module_exit(void)
{
	platform_driver_unregister(&ambarella_phy_driver);
}
module_exit(ambarella_phy_module_exit);


MODULE_AUTHOR("Cao Rongrong <rrcao@ambarella.com>");
MODULE_DESCRIPTION("Ambarella USB PHY driver");
MODULE_LICENSE("GPL");

