#include <linux/stmmac.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/phy.h>
#include <linux/of_net.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>
#include "stmmac_platform.h"

#define GMAC_SPEED_10M      (10)
#define GMAC_SPEED_100M     (100)
#define GMAC_SPEED_1000M    (1000)
#define ETH_MAC_GMII_ADDR_PA(x)		(((x) & 0x1f) << 21)
#define ETH_MAC_GMII_ADDR_GR(x)		(((x) & 0x1f) << 16)
#define ETH_MAC_GMII_CLKDIV(x) 		(((x) & 0xf) << 8)
#define ETH_MAC_GMII_CMD_READ		(3 << 2)
#define ETH_MAC_GMII_CMD_WRITE		(1 << 2)
#define ETH_MAC_GMII_CMD_BUSY		(1 << 0)
#define ahbmdio_to_le32(id, val)	((id & 1) ? (val) >> 16 : val & 0xFFFF)
#define le32_to_ahbmdio(id, val)	((id & 1) ? (val) << 16 : val & 0xFFFF)
#define AHBSP_GMII0_ADDR_OFFSET  0xA4
#define AHBSP_GMII1_ADDR_OFFSET  0xA8
#define AHBSP_GMII2_ADDR_OFFSET  0xB0
#define AHBSP_GMII3_ADDR_OFFSET  0xB4
#define AHBSP_GMII01_DATA_OFFSET 0xA0
#define AHBSP_GMII23_DATA_OFFSET 0xAC

static u32 mdio_addr_reg[] = {
		AHBSP_GMII0_ADDR_OFFSET,
		AHBSP_GMII1_ADDR_OFFSET,
		AHBSP_GMII2_ADDR_OFFSET,
		AHBSP_GMII3_ADDR_OFFSET,
};
static u32 mdio_data_reg[] = {
	AHBSP_GMII01_DATA_OFFSET,
	AHBSP_GMII01_DATA_OFFSET,
	AHBSP_GMII23_DATA_OFFSET,
	AHBSP_GMII23_DATA_OFFSET,
};
static DEFINE_MUTEX(mdio_lock);

struct ambeth_gmac_ops {
	void (*set_mode)(void *priv);
	void (*set_clock)(void *priv);
};

struct amba_bsp_priv
{
	u32		id;
	u32		phy_iface;
	u32		second_ref_clk_50mhz;
	u32		tx_clk_invert;
	u32		rx_clk_invert;
	u32		ahb_mdio_clk_div;

	int		pwr_gpio;
	int		pwr_gpio_active;

	struct regmap	*reg_scr;
	struct regmap	*reg_rct;
	struct platform_device *pdev;
	const struct ambeth_gmac_ops	*ops;
};

static void amba_gmac_set_clock(void *priv)
{
	struct amba_bsp_priv *bsp_priv = priv;

	if (bsp_priv->tx_clk_invert) {
		if (bsp_priv->id == 0)
			regmap_update_bits(bsp_priv->reg_scr, 0x60, (1<<31), (1<<31));
		else if (bsp_priv->id == 1)
			regmap_update_bits(bsp_priv->reg_scr, 0x60, (1<<28), (1<<28));
		else if (bsp_priv->id == 2)
			regmap_update_bits(bsp_priv->reg_scr, 0x270, (1<<13), (1<<13));
		else if (bsp_priv->id == 3)
			regmap_update_bits(bsp_priv->reg_scr, 0x270, (1<<6), (1<<6));
		else
			dev_err(&bsp_priv->pdev->dev, "Unsupport ethernt%d \n", bsp_priv->id);
	}

	if (bsp_priv->rx_clk_invert) {
		if (bsp_priv->id == 0)
			regmap_update_bits(bsp_priv->reg_scr, 0x60, (1<<0), (1<<0));
		else if (bsp_priv->id == 1)
			regmap_update_bits(bsp_priv->reg_scr, 0x60, (1<<11), (1<<11));
		else if (bsp_priv->id == 2)
			regmap_update_bits(bsp_priv->reg_scr, 0x270, (1<<8), (1<<8));
		else if (bsp_priv->id == 3)
			regmap_update_bits(bsp_priv->reg_scr, 0x270, (1<<1), (1<<1));
		else
			dev_err(&bsp_priv->pdev->dev, "Unsupport ethernt%d \n",bsp_priv->id);
	}

	/*
	 * snd_ref_clk_50mhz: rmii refclk for both mac and phy
	 * snd_ref_clk_25mhz: ->phy to generate rmii(100Mbps)_ref_clk_50Mhz
	 * or rgmii_125M(1000Mbps) or rgmii_25M(100Mbps)
	 * */
	if (bsp_priv->second_ref_clk_50mhz)
		regmap_update_bits(bsp_priv->reg_scr, 0x60, (1<<23), (1<<23));

	/* attention: gclk_mac_csr = gclk_core or gclk_core/2  ref: amboot: soc_fixup()
	 * gclk_mac_csr_max limit <=800MHZ
	 * mdio_clk < 2.5M <--->gclk_mac_csr. use scr.0xa0 scr.0xa4;
	 * if gclk_mac_csr > 800Mhz then can only use scr.0xa0 scr.0xa4;
	*/
}

static void amba_gmac_set_mode(void *priv)
{
	struct amba_bsp_priv *bsp_priv = priv;
	unsigned int value;
	unsigned int mask;

	/* Enable ENET and set clock Source as clk_rx */
	value = BIT(0);

	switch (bsp_priv->phy_iface) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		dev_info(&bsp_priv->pdev->dev, "select RGMII mode\n");
		break;
	case PHY_INTERFACE_MODE_RMII:
		dev_info(&bsp_priv->pdev->dev, "select RMII mode\n");
		value |= BIT(1);
		break;
	default:
		dev_info(&bsp_priv->pdev->dev, "Unsupported mode\n");
		break;
	};

	/* Enable ENET and select PHY interface */
	value = (value << (bsp_priv->id * 4));
	mask = (0xf << (bsp_priv->id * 4));
	regmap_update_bits(bsp_priv->reg_scr, 0x10c, mask, value);
}

static const struct ambeth_gmac_ops ambarella_gmac_ops = {
	.set_mode = amba_gmac_set_mode,
	.set_clock = amba_gmac_set_clock,
};

static int ambarella_gmac_plat_init(struct amba_bsp_priv *bsp_priv)
{
	/* power-on-phy && let stmmac-mdio-reset phy */
	if (gpio_is_valid(bsp_priv->pwr_gpio)) {
		gpio_direction_output(bsp_priv->pwr_gpio, !bsp_priv->pwr_gpio_active);
		msleep(20);
		gpio_direction_output(bsp_priv->pwr_gpio, bsp_priv->pwr_gpio_active);
		msleep(20);
	}

	if (bsp_priv->ops && bsp_priv->ops->set_mode)
			bsp_priv->ops->set_mode(bsp_priv);

	if (bsp_priv->ops && bsp_priv->ops->set_clock)
			bsp_priv->ops->set_clock(bsp_priv);

	return 0;
}

static void ambarella_gmac_suspend_exit(struct platform_device *pdev, void *priv)
{
	struct amba_bsp_priv *bsp_priv = priv;

	/* power-off phy when suspend */
	if (gpio_is_valid(bsp_priv->pwr_gpio))
		gpio_direction_output(bsp_priv->pwr_gpio, !bsp_priv->pwr_gpio_active);
}

static int ambarella_gmac_resume_init(struct platform_device *pdev, void *priv)
{
	struct amba_bsp_priv *bsp_priv = priv;

	return ambarella_gmac_plat_init(bsp_priv);
}

static int ambahb_mdio_poll_status(struct regmap *regmap, u32 regoff)
{
	int err;
	u32 value;

	err = regmap_read_poll_timeout(regmap, regoff, value, !(value & 1),
					1000, 1000000);
	if (err)
		pr_err("timeout to wait for AHB MDIO ready.\n");
	return err;
}

static int ambahb_mdio_read(struct mii_bus *bus, int mii_id, int phyreg)
{
	int rval;
	unsigned int regval, phydata;
	struct net_device *ndev;
	struct stmmac_priv *stmpriv;
	struct amba_bsp_priv *bsp_priv;
	struct plat_stmmacenet_data *plat_dat;

	ndev = bus->priv;  			/* ref stmmac_mdio_register */
	stmpriv = netdev_priv(ndev);
	plat_dat = stmpriv->plat;
	bsp_priv = plat_dat->bsp_priv;

	regval = ETH_MAC_GMII_ADDR_PA(mii_id) | ETH_MAC_GMII_ADDR_GR(phyreg);
	regval |= ETH_MAC_GMII_CLKDIV((bsp_priv->ahb_mdio_clk_div - 1));
	regval |= ETH_MAC_GMII_CMD_READ;	/* Read enable */
	regval |= ETH_MAC_GMII_CMD_BUSY;	/* busy */

	mutex_lock(&mdio_lock);
	regmap_write(bsp_priv->reg_scr, mdio_addr_reg[bsp_priv->id], regval);
	rval = ambahb_mdio_poll_status(bsp_priv->reg_scr, mdio_addr_reg[bsp_priv->id]);
	if (rval < 0) {
		mutex_unlock(&mdio_lock);
		return 0;
	}
	regmap_read(bsp_priv->reg_scr, mdio_data_reg[bsp_priv->id], &phydata);
	mutex_unlock(&mdio_lock);
	phydata = ahbmdio_to_le32(bsp_priv->id, phydata);

	return phydata;
}

static int ambahb_mdio_write(struct mii_bus *bus, int mii_id, int phyreg, u16 phydata)
{
	int rval = 0;
	unsigned int regval, opval, mask;
	struct net_device *ndev;
	struct stmmac_priv *stmpriv;
	struct amba_bsp_priv *bsp_priv;
	struct plat_stmmacenet_data *plat_dat;

	ndev = bus->priv;
	stmpriv = netdev_priv(ndev);
	plat_dat = stmpriv->plat;
	bsp_priv = plat_dat->bsp_priv;

	regval = ETH_MAC_GMII_ADDR_PA(mii_id) | ETH_MAC_GMII_ADDR_GR(phyreg);
	regval |= ETH_MAC_GMII_CLKDIV((bsp_priv->ahb_mdio_clk_div - 1));
	regval |= ETH_MAC_GMII_CMD_WRITE;
	regval |= ETH_MAC_GMII_CMD_BUSY;
	opval = le32_to_ahbmdio(bsp_priv->id, phydata);
	mask = le32_to_ahbmdio(bsp_priv->id, 0xFFFF);

	mutex_lock(&mdio_lock);
	regmap_update_bits(bsp_priv->reg_scr, mdio_data_reg[bsp_priv->id], mask, opval);
	regmap_write(bsp_priv->reg_scr, mdio_addr_reg[bsp_priv->id], regval);
	rval = ambahb_mdio_poll_status(bsp_priv->reg_scr, mdio_addr_reg[bsp_priv->id]);
	mutex_unlock(&mdio_lock);

	return rval;
}

static struct amba_bsp_priv *amba_gmac_parse(struct platform_device *pdev,
					struct plat_stmmacenet_data *plat,
					const struct ambeth_gmac_ops *ops)
{
	int ret;
	u32 value;
	struct amba_bsp_priv *bsp_priv;
	struct device *dev = &pdev->dev;
	enum of_gpio_flags flags;

	bsp_priv = devm_kzalloc(dev, sizeof(*bsp_priv), GFP_KERNEL);
	if (!bsp_priv)
		return ERR_PTR(-ENOMEM);

	of_get_phy_mode(dev->of_node, &bsp_priv->phy_iface);
	bsp_priv->ops = ops;

	ret = of_property_read_u32(dev->of_node, "index", &value);
	if (ret)
		bsp_priv->id = plat->bus_id;
	else
		bsp_priv->id = value;

	plat->addr64 = 0; /* default 0: 32bits */
	if (of_find_property(dev->of_node, "amb,dma-eame", NULL))
		plat->addr64 = 40; /* ENABLED(CONFIG_ARCH_DMA_ADDR_T_64BIT) to use 40bits dma */

	dev_info(dev, "EthernetMac_index = %u", bsp_priv->id);
	dev_info(dev, "EthernetMac_dma_cap = %u bits \n", plat->addr64);

	ret = of_property_read_u32(dev->of_node, "amb,ahb-12mhz-div", &value);
	if (ret < 0 || value > 16)
		bsp_priv->ahb_mdio_clk_div = 0;
	else
		bsp_priv->ahb_mdio_clk_div = value;

	bsp_priv->pwr_gpio = of_get_named_gpio_flags(dev->of_node, "pwr-gpios", 0, &flags);
	bsp_priv->pwr_gpio_active = !!(flags & OF_GPIO_ACTIVE_LOW);
	bsp_priv->tx_clk_invert = !!of_find_property(dev->of_node, "amb,tx-clk-invert", NULL);
	bsp_priv->rx_clk_invert = !!of_find_property(dev->of_node, "amb,rx-clk-invert", NULL);
	bsp_priv->second_ref_clk_50mhz = !!of_find_property(dev->of_node, "amb,2nd-ref-clk-50mhz", NULL);

	bsp_priv->reg_scr = syscon_regmap_lookup_by_phandle(dev->of_node, "amb,scr-regmap");
	if (IS_ERR(bsp_priv->reg_scr)) {
		dev_err(dev, "no scr regmap!\n");
		bsp_priv->reg_scr = NULL;
	}
	bsp_priv->reg_rct = syscon_regmap_lookup_by_phandle(dev->of_node, "amb,rct-regmap");
	if (IS_ERR(bsp_priv->reg_rct)) {
		dev_err(dev, "no rct regmap!\n");
		bsp_priv->reg_rct = NULL;
	}

	if (bsp_priv->ahb_mdio_clk_div) {
		plat->mdio_read = &ambahb_mdio_read;
		plat->mdio_write = &ambahb_mdio_write;
	}

	/* callback for suspend && resume */
	plat->init = ambarella_gmac_resume_init;
	plat->exit = ambarella_gmac_suspend_exit;
	plat->has_ambarella = 1;

	bsp_priv->pdev = pdev;
	return bsp_priv;
}

static int ambarella_gmac_probe(struct platform_device *pdev)
{
	int ret;

	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	const struct ambeth_gmac_ops *data;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat)) {
		dev_err(&pdev->dev, "stmmac_probe_config_dt error \n");
		return PTR_ERR(plat_dat);
	}

	data = of_device_get_match_data(&pdev->dev);
	if (!data) {
		dev_err(&pdev->dev, "no of match data provided\n");
		ret = -ENOMEM;
		goto err_out;
	}

	/* platform specific */
	plat_dat->bsp_priv = amba_gmac_parse(pdev, plat_dat, data);
	if (IS_ERR(plat_dat->bsp_priv)) {
		dev_err(&pdev->dev, "stmmac_probe_config_dt error \n");
		ret = PTR_ERR(plat_dat->bsp_priv);
		goto err_out;
	}
	ambarella_gmac_plat_init(plat_dat->bsp_priv);

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret) {
		dev_err(&pdev->dev, "stmmac_dvr_probe error \n");
		goto err_out;
	} else {
		dev_info(&pdev->dev, "stmmac_dvr_probe OK \n");
	}

	return 0;

err_out:
	stmmac_remove_config_dt(pdev, plat_dat);
	return ret;
}

static const struct of_device_id ambarella_gmac_match[] = {
	{ .compatible = "ambarella-dwmac-eqos", .data = &ambarella_gmac_ops },
	{ }
};
MODULE_DEVICE_TABLE(of, ambarella_gmac_match);

static struct platform_driver ambarella_gmac_driver = {
	.probe  = ambarella_gmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "ambarella-dwmac-eqos",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = ambarella_gmac_match,
	},
};
module_platform_driver(ambarella_gmac_driver);
MODULE_AUTHOR("Zhang Xuliang @ambarella.com");
MODULE_DESCRIPTION("Ambarella CV3 Gmac glue driver ");
MODULE_LICENSE("GPL");
