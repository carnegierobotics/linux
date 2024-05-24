// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cadence Excelsior PHY driver.
 *
 * Copyright 2023 Cadence Design Systems, Inc.
 * Author: Swapnil Jakhade <sjakhade@cadence.com>
 */

#include <dt-bindings/phy/phy.h>
#include <dt-bindings/phy/phy-cadence.h>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/regmap.h>

#define EXCR_PMA_COMMON_CDB_OFFSET		0x0

#define EXCR_PMA_TX_LANE_CDB_OFFSET(ln)		(0x10000 + ((ln) << 11))

#define EXCR_PMA_RX_LANE_CDB_OFFSET(ln)		(0x20000 + ((ln) << 11))

#define EXCR_PHY_PCS_COMMON_CDB_OFFSET		0x30000

#define EXCR_PHY_PCS_LANE_CDB_OFFSET(ln)	(0x34000 + ((ln) << 10))

#define EXCR_PHY_PMA_COMMON_CDB_OFFSET		0x38000

#define EXCR_PHY_PMA_LANE_CDB_OFFSET(ln)	(0x3C000 + ((ln) << 10))

/********** Register offsets **********/

/* PHY PCS common registers */
#define PHY_CTRL_STS			0x0014U
#define UC_TOP_CTRL			0x0040U
#define UC_TOP_SRAM_ADDR		0x0044U
#define UC_TOP_SCRPAD0			0x0050U
#define UC_TOP_SCRPAD1			0x0054U
#define UC_TOP_SCRPAD2			0x0058U
#define UC_TOP_SRAM_ACCESS		0x0080U

/* PHY PMA common registers */
#define PHY_UC_CMN_INT_STS		0x0040U
#define PHY_UC_CMN_INT_STS_SET		0x0044U
#define PHY_SOC_INT_STS			0x0060U
#define PHY_SOC_INT_STS_SET		0x0064U

/* PHY PMA lane registers */
#define UC_LN_CTRL			0x0040U
#define UC_LN_SRAM_ADDR			0x0044U
#define UC_LN_SCRPAD0			0x0048U
#define UC_LN_SCRPAD1			0x004CU
#define UC_LN_SCRPAD2			0x0050U
#define UC_LN_SCRPAD3			0x0054U
#define UC_LN_SRAM_ACCESS		0x0080U

/* Firmwares */
#define TOP_FW			"cadence/excelsior_top_main.bin"
#define LANE_FW			"cadence/excelsior_lane_main.bin"

/* Reference clock */
#define REF_CLK_100MHZ		100000000
#define REF_CLK_19_2MHZ		 19200000
#define REF_CLK_20MHZ		 20000000
#define REF_CLK_24MHZ		 24000000
#define REF_CLK_25MHZ		 25000000
#define REF_CLK_26MHZ		 26000000
#define REF_CLK_27MHZ		 27000000
#define REF_CLK_156_25MHZ	156250000

/* PHY operating mode */
#define PHY_MODE_PCIE_SINGLE	0x0
#define PHY_MODE_PCIE_MULTI	0x1

/* Gen */
#define MAX_NUM_LANES		8

#define SSC_CONFIG_3K_PPM	0
#define SSC_CONFIG_5K_PPM	1

#define PCIE_EP			0
#define PCIE_RC(ln)		(1 << ((ln) * 4))

#define DISABLE_LN_FW_COPY	BIT(28)

#define POLL_TIMEOUT_US		50000

#ifdef EXCR_DEBUG

/* API Command type encoding */
#define API_CMD_READ_MEMORY				0x00
#define API_CMD_WRITE_MEMORY				0x01
#define API_CMD_START_TRACE_TRIGGER			0x02
#define API_CMD_START_TRACE_STEP			0x03
#define API_CMD_STOP_TRACE				0x04
#define API_CMD_TRACE_COMPLETE				0x05
#define API_CMD_READ_TRACE				0x06
#define API_CMD_STOP_RECEIVER_ADAPTATION		0x10
#define API_CMD_RECEIVER_ADAPTATION_STOPPED		0x11
#define API_CMD_RESUME_RECEIVER_ADAPTATION		0x12
#define API_CMD_RX_EYE_INITIALIZE			0x13
#define API_CMD_RX_EYE_MEASURE				0x14
#define API_CMD_RX_EYE_RESTORE				0x15
#define API_CMD_RUN_RECEIVER_ADAPTATION_FUNCTION	0x16
#define API_CMD_PING					0x83
#define API_CMD_CONFIGURE_RATE				0x84
#define API_CMD_CONFIGURE_PHY				0x85
#define API_CMD_PHY_ERROR_DETECTED			0xC0

#define CMD_MASK		0xFF
#define API_CMD_RESP_ERR	0xFFFFFFFF

#define API_SYSTEM_TO_TOP_INT	BIT(30)

#endif

enum cdns_excelsior_ref_clk {
	CLK_100_MHZ,
	CLK_19_2_MHZ,
	CLK_20_MHZ,
	CLK_24_MHZ,
	CLK_25_MHZ,
	CLK_26_MHZ,
	CLK_27_MHZ,
	CLK_156_25_MHZ
};

enum cdns_excelsior_ssc_mode {
	NO_SSC,
	INTERNAL_SSC,
	EXTERNAL_SSC
};

struct cdns_excelsior_init_data {
	u8 internal_ssc_ppm;
	bool disable_ln_fw_copy;
};

struct cdns_excelsior_inst {
	struct phy *phy;
	u32 mlane;
	u32 num_lanes;
	u32 pcie_mode;
	struct reset_control *lnk_rst;
};

struct cdns_excelsior_phy {
	void __iomem *sd_base; /* SD2000 registers base */
	struct reset_control *apb_rst;
	struct reset_control *phy_rst;
	struct device *dev;
	struct clk *clk;
	struct mutex api_cmd_mutex;
	struct cdns_excelsior_inst phys[MAX_NUM_LANES];
	const struct cdns_excelsior_init_data *init_data;
	int nsubnodes;
	enum cdns_excelsior_ref_clk ref_clk_rate;
	enum cdns_excelsior_ssc_mode ssc_mode;
	struct regmap *regmap_phy_pcs_common_cdb;
	struct regmap *regmap_phy_pma_common_cdb;
	struct regmap *regmap_phy_pma_lane_cdb[MAX_NUM_LANES];
	struct regmap_field *uc_top_ctrl_stall_run;
	struct regmap_field *phy_ctrl_sts_uc_init_cmpl;
	struct regmap_field *phy_ctrl_sts_pipe_pll_ok;
};

/* Regmap */

static const struct reg_field uc_top_ctrl_stall_run =
				REG_FIELD(UC_TOP_CTRL, 0, 0);

static const struct reg_field phy_ctrl_sts_uc_init_cmpl =
				REG_FIELD(PHY_CTRL_STS, 0, 0);

static const struct reg_field phy_ctrl_sts_pipe_pll_ok =
				REG_FIELD(PHY_CTRL_STS, 5, 5);

struct cdns_regmap_cdb_context {
	struct device *dev;
	void __iomem *base;
	u8 reg_offset_shift;
};

static int cdns_regmap_write(void *context, unsigned int reg, unsigned int val)
{
	struct cdns_regmap_cdb_context *ctx = context;
	u32 offset = reg << ctx->reg_offset_shift;

	writel(val, ctx->base + offset);

	return 0;
}

static int cdns_regmap_read(void *context, unsigned int reg, unsigned int *val)
{
	struct cdns_regmap_cdb_context *ctx = context;
	u32 offset = reg << ctx->reg_offset_shift;

	*val = readl(ctx->base + offset);
	return 0;
}

static const struct regmap_config cdns_excr_phy_pcs_cmn_cdb_config = {
	.name = "excr_phy_pcs_cmn_cdb",
	.reg_stride = 1,
	.fast_io = true,
	.reg_write = cdns_regmap_write,
	.reg_read = cdns_regmap_read,
};

static const struct regmap_config cdns_excr_phy_pma_cmn_cdb_config = {
	.name = "excr_phy_pma_cmn_cdb",
	.reg_stride = 1,
	.fast_io = true,
	.reg_write = cdns_regmap_write,
	.reg_read = cdns_regmap_read,
};

#define EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF(n) \
{ \
	.name = "excr_phy_pma_lane" n "_cdb", \
	.reg_stride = 1, \
	.fast_io = true, \
	.reg_write = cdns_regmap_write, \
	.reg_read = cdns_regmap_read, \
}

static const struct regmap_config cdns_excr_phy_pma_lane_cdb_config[] = {
	EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF("0"),
	EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF("1"),
	EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF("2"),
	EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF("3"),
	EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF("4"),
	EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF("5"),
	EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF("6"),
	EXCR_PHY_PMA_LANE_CDB_REGMAP_CONF("7"),
};

static struct regmap *cdns_regmap_init(struct device *dev, void __iomem *base,
				       u32 block_offset, u8 reg_offset_shift,
				       const struct regmap_config *config)
{
	struct cdns_regmap_cdb_context *ctx;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->dev = dev;
	ctx->base = base + block_offset;
	ctx->reg_offset_shift = reg_offset_shift;

	return devm_regmap_init(dev, NULL, ctx, config);
}

static int cdns_excelsior_regmap_init(struct cdns_excelsior_phy *cdns_phy)
{
	void __iomem *sd_base = cdns_phy->sd_base;
	struct device *dev = cdns_phy->dev;
	u8 reg_offset_shift = 0;
	struct regmap *regmap;
	u32 block_offset;
	int i;

	/* PHY PCS common */
	block_offset = EXCR_PHY_PCS_COMMON_CDB_OFFSET;
	regmap = cdns_regmap_init(dev, sd_base, block_offset, reg_offset_shift,
				  &cdns_excr_phy_pcs_cmn_cdb_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "Failed to init PHY PCS common CDB regmap\n");
		return PTR_ERR(regmap);
	}
	cdns_phy->regmap_phy_pcs_common_cdb = regmap;

	/* PHY PMA common */
	block_offset = EXCR_PHY_PMA_COMMON_CDB_OFFSET;
	regmap = cdns_regmap_init(dev, sd_base, block_offset, reg_offset_shift,
				  &cdns_excr_phy_pma_cmn_cdb_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "Failed to init PHY PMA common CDB regmap\n");
		return PTR_ERR(regmap);
	}
	cdns_phy->regmap_phy_pma_common_cdb = regmap;

	for (i = 0; i < MAX_NUM_LANES; i++) {
		/* PHY PMA lane */
		block_offset = EXCR_PHY_PMA_LANE_CDB_OFFSET(i);
		regmap = cdns_regmap_init(dev, sd_base, block_offset, reg_offset_shift,
					  &cdns_excr_phy_pma_lane_cdb_config[i]);
		if (IS_ERR(regmap)) {
			dev_err(dev, "Failed to init PHY PMA lane CDB regmap\n");
			return PTR_ERR(regmap);
		}
		cdns_phy->regmap_phy_pma_lane_cdb[i] = regmap;
	}

	return 0;
}

static int cdns_excelsior_regfield_init(struct cdns_excelsior_phy *cdns_phy)
{
	struct device *dev = cdns_phy->dev;
	struct regmap_field *field;
	struct regmap *regmap;

	regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	field = devm_regmap_field_alloc(dev, regmap, uc_top_ctrl_stall_run);
	if (IS_ERR(field)) {
		dev_err(dev, "uc_top_ctrl_stall_run reg field init failed\n");
		return PTR_ERR(field);
	}
	cdns_phy->uc_top_ctrl_stall_run = field;

	regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	field = devm_regmap_field_alloc(dev, regmap, phy_ctrl_sts_uc_init_cmpl);
	if (IS_ERR(field)) {
		dev_err(dev, "phy_ctrl_sts_uc_init_cmpl reg field init failed\n");
		return PTR_ERR(field);
	}
	cdns_phy->phy_ctrl_sts_uc_init_cmpl = field;

	regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	field = devm_regmap_field_alloc(dev, regmap, phy_ctrl_sts_pipe_pll_ok);
	if (IS_ERR(field)) {
		dev_err(dev, "phy_ctrl_sts_pipe_pll_ok reg field init failed\n");
		return PTR_ERR(field);
	}
	cdns_phy->phy_ctrl_sts_pipe_pll_ok = field;

	return 0;
}

static void cdns_excelsior_reg_write(struct regmap *regmap, u32 offset, u32 val)
{
	regmap_write(regmap, offset, val);
}

static u32 cdns_excelsior_reg_read(struct regmap *regmap, u32 offset)
{
	unsigned int val;

	regmap_read(regmap, offset, &val);
	return val;
}

#ifdef EXCR_DEBUG

/* Mailbox */

static int excr_api_wait_for_response(struct cdns_excelsior_phy *cdns_phy)
{
	struct regmap *regmap = cdns_phy->regmap_phy_pma_common_cdb;
	int ret;
	u32 reg;

	cdns_excelsior_reg_write(regmap, PHY_UC_CMN_INT_STS_SET, API_SYSTEM_TO_TOP_INT);

	ret = regmap_read_poll_timeout(regmap, PHY_UC_CMN_INT_STS, reg,
				       !(reg & API_SYSTEM_TO_TOP_INT), 0, POLL_TIMEOUT_US);
	if (ret == -ETIMEDOUT) {
		dev_err(cdns_phy->dev, "timeout waiting for response from top uC\n");
		return ret;
	}

	return 0;
}

static int excr_api_read_memory(struct cdns_excelsior_phy *cdns_phy, u16 addr, u32 *resp_data)
{
	struct regmap *regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	u32 cmd, resp;
	int ret;

	mutex_lock(&cdns_phy->api_cmd_mutex);

	// write command
	cmd = API_CMD_READ_MEMORY & CMD_MASK;
	cmd |= (addr << 16);

	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD0, cmd);

	ret = excr_api_wait_for_response(cdns_phy);
	if (ret)
		goto err;

	// read response
	resp = cdns_excelsior_reg_read(regmap, UC_TOP_SCRPAD0);
	if (resp == API_CMD_RESP_ERR) {
		ret = -EINVAL;
		goto err;
	}

	ret = excr_api_wait_for_response(cdns_phy);
	if (ret)
		goto err;

	// read response data
	*resp_data = cdns_excelsior_reg_read(regmap, UC_TOP_SCRPAD0);

	mutex_unlock(&cdns_phy->api_cmd_mutex);
	return 0;

err:
	mutex_unlock(&cdns_phy->api_cmd_mutex);
	return ret;
}

static int excr_api_write_memory(struct cdns_excelsior_phy *cdns_phy, u16 addr, u32 data)
{
	struct regmap *regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	u32 cmd, resp;
	int ret;

	mutex_lock(&cdns_phy->api_cmd_mutex);

	// write command
	cmd = API_CMD_WRITE_MEMORY & CMD_MASK;
	cmd |= (addr << 16);

	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD0, cmd);

	ret = excr_api_wait_for_response(cdns_phy);
	if (ret)
		goto err;

	// write data
	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD0, data);

	ret = excr_api_wait_for_response(cdns_phy);
	if (ret)
		goto err;

	// write mask
	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD0, 0xFFFFFFFF);

	ret = excr_api_wait_for_response(cdns_phy);
	if (ret)
		goto err;

	// read response
	resp = cdns_excelsior_reg_read(regmap, UC_TOP_SCRPAD0);
	if (resp == API_CMD_RESP_ERR) {
		ret = -EINVAL;
		goto err;
	}

	mutex_unlock(&cdns_phy->api_cmd_mutex);
	return 0;

err:
	mutex_unlock(&cdns_phy->api_cmd_mutex);
	return ret;
}

static int excr_api_ping(struct cdns_excelsior_phy *cdns_phy)
{
	struct regmap *regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	u32 cmd, resp, state, resp_num;
	u32 random_num = 0x34;
	int ret;

	mutex_lock(&cdns_phy->api_cmd_mutex);

	// write command
	cmd = API_CMD_PING & CMD_MASK;
	cmd |= ((random_num & 0xFFFFFF) << 8);

	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD0, cmd);

	ret = excr_api_wait_for_response(cdns_phy);
	if (ret)
		goto err;

	// read response
	resp = cdns_excelsior_reg_read(regmap, UC_TOP_SCRPAD0);

	resp_num = resp & 0xFFFFFF;
	state = (resp >> 24) & 0xFF;

	if ((resp_num != random_num) || (state > 4)) {
		dev_err(cdns_phy->dev, "Ping command failed\n");
		ret = -EIO;
		goto err;
	}

	dev_dbg(cdns_phy->dev, "Ping success: state = %u\n", state);

	mutex_unlock(&cdns_phy->api_cmd_mutex);
	return 0;

err:
	mutex_unlock(&cdns_phy->api_cmd_mutex);
	return ret;
}

static int excr_api_configure_phy(struct cdns_excelsior_phy *cdns_phy, bool port_enable,
				  u16 port_mask)
{
	struct regmap *regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	u32 cmd, resp;
	int ret;

	mutex_lock(&cdns_phy->api_cmd_mutex);

	// write command
	cmd = API_CMD_CONFIGURE_PHY & CMD_MASK;
	if (!port_enable)
		cmd |= (0x01 << 8);
	cmd |= (port_mask << 16);

	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD0, cmd);

	ret = excr_api_wait_for_response(cdns_phy);
	if (ret)
		goto err;

	// read response
	resp = cdns_excelsior_reg_read(regmap, UC_TOP_SCRPAD0);
	if (resp == API_CMD_RESP_ERR) {
		ret = -EINVAL;
		goto err;
	}

	mutex_unlock(&cdns_phy->api_cmd_mutex);
	return 0;

err:
	mutex_unlock(&cdns_phy->api_cmd_mutex);
	return ret;
}

static int excr_enable_port(struct cdns_excelsior_phy *cdns_phy, u16 port_mask)
{
	return excr_api_configure_phy(cdns_phy, true, port_mask);
}

static int excr_disable_port(struct cdns_excelsior_phy *cdns_phy, u16 port_mask)
{
	return excr_api_configure_phy(cdns_phy, false, port_mask);
}

#endif

static int cdns_excelsior_phy_on(struct phy *phy)
{
	struct cdns_excelsior_phy *cdns_phy = dev_get_drvdata(phy->dev.parent);
	struct cdns_excelsior_inst *inst = phy_get_drvdata(phy);
	u32 read_val;
	int ret;

	/* Enable link */
	reset_control_deassert(inst->lnk_rst);

	/* Wait for PLL ready */
	ret = regmap_field_read_poll_timeout(cdns_phy->phy_ctrl_sts_pipe_pll_ok,
					     read_val, read_val, 0, POLL_TIMEOUT_US);
	if (ret)
		dev_err(cdns_phy->dev, "Timeout waiting for PIPE PLL ready\n");

	return ret;
}

static int cdns_excelsior_phy_off(struct phy *phy)
{
	struct cdns_excelsior_inst *inst = phy_get_drvdata(phy);

	/* Disable link */
	return reset_control_assert(inst->lnk_rst);
}

static const struct phy_ops cdns_excelsior_phy_ops = {
	.power_on	= cdns_excelsior_phy_on,
	.power_off	= cdns_excelsior_phy_off,
	.owner		= THIS_MODULE,
};

static void cdns_excelsior_start_of_day_config(struct cdns_excelsior_phy *cdns_phy)
{
	struct regmap *regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	u32 uc_top_scrpad0, uc_top_scrpad1, uc_top_scrpad2;
	u32 phy_mode = PHY_MODE_PCIE_SINGLE, i, j;
	u32 lane_mask = 0;
	u32 mlane, num_lanes;

	if (cdns_phy->nsubnodes > 1)
		phy_mode = PHY_MODE_PCIE_MULTI;

	uc_top_scrpad0 = phy_mode                       |
			 (cdns_phy->ref_clk_rate << 8)  |
			 (cdns_phy->ref_clk_rate << 16) |
			 (cdns_phy->ssc_mode << 20);

	if (cdns_phy->ssc_mode == INTERNAL_SSC) {
		uc_top_scrpad0 &= ~(3 << 24);
		uc_top_scrpad0 |= (cdns_phy->init_data->internal_ssc_ppm << 24);
	}

	if (cdns_phy->init_data->disable_ln_fw_copy)
		uc_top_scrpad0 |= DISABLE_LN_FW_COPY;

	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD0, uc_top_scrpad0);

	for (i = 0; i < cdns_phy->nsubnodes; i++) {
		mlane = cdns_phy->phys[i].mlane;
		num_lanes = cdns_phy->phys[i].num_lanes;
		lane_mask |= (((1 << num_lanes) - 1) << mlane);
		if (cdns_phy->phys[i].pcie_mode == CDNS_EXCR_PCIE_RC) {
			for (j = 0; j < num_lanes; j++)
				uc_top_scrpad2 |= PCIE_RC(mlane + j);
		}
	}

	uc_top_scrpad1 = 0xFFFF & ~lane_mask;
	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD1, uc_top_scrpad1);

	cdns_excelsior_reg_write(regmap, UC_TOP_SCRPAD2, uc_top_scrpad2);
}

static int cdns_excelsior_reset(struct cdns_excelsior_phy *cdns_phy)
{
	struct device *dev = cdns_phy->dev;

	cdns_phy->phy_rst = devm_reset_control_get_exclusive_by_index(dev, 0);
	if (IS_ERR(cdns_phy->phy_rst)) {
		dev_err(dev, "%s: failed to get PHY reset\n", dev->of_node->full_name);
		return PTR_ERR(cdns_phy->phy_rst);
	}

	cdns_phy->apb_rst = devm_reset_control_get_optional_exclusive(dev, "apb_reset");
	if (IS_ERR(cdns_phy->apb_rst)) {
		dev_err(dev, "%s: failed to get apb reset\n", dev->of_node->full_name);
		return PTR_ERR(cdns_phy->apb_rst);
	}

	return 0;
}

static int cdns_excelsior_clk(struct cdns_excelsior_phy *cdns_phy)
{
	struct device *dev = cdns_phy->dev;
	unsigned long ref_clk_rate;
	int ret;

	cdns_phy->clk = devm_clk_get(dev, "refclk");
	if (IS_ERR(cdns_phy->clk)) {
		dev_err(dev, "phy ref clock not found\n");
		return PTR_ERR(cdns_phy->clk);
	}

	ret = clk_prepare_enable(cdns_phy->clk);
	if (ret) {
		dev_err(cdns_phy->dev, "Failed to prepare ref clock\n");
		return ret;
	}

	ref_clk_rate = clk_get_rate(cdns_phy->clk);
	if (!ref_clk_rate) {
		dev_err(cdns_phy->dev, "Failed to get ref clock rate\n");
		clk_disable_unprepare(cdns_phy->clk);
		return -EINVAL;
	}

	switch (ref_clk_rate) {
	case REF_CLK_19_2MHZ:
		cdns_phy->ref_clk_rate = CLK_19_2_MHZ;
		break;
	case REF_CLK_20MHZ:
		cdns_phy->ref_clk_rate = CLK_20_MHZ;
		break;
	case REF_CLK_24MHZ:
		cdns_phy->ref_clk_rate = CLK_24_MHZ;
		break;
	case REF_CLK_25MHZ:
		cdns_phy->ref_clk_rate = CLK_25_MHZ;
		break;
	case REF_CLK_26MHZ:
		cdns_phy->ref_clk_rate = CLK_26_MHZ;
		break;
	case REF_CLK_27MHZ:
		cdns_phy->ref_clk_rate = CLK_27_MHZ;
		break;
	case REF_CLK_100MHZ:
		cdns_phy->ref_clk_rate = CLK_100_MHZ;
		break;
	case REF_CLK_156_25MHZ:
		cdns_phy->ref_clk_rate = CLK_156_25_MHZ;
		break;
	default:
		dev_err(cdns_phy->dev, "Invalid Ref Clock Rate\n");
		clk_disable_unprepare(cdns_phy->clk);
		return -EINVAL;
	}

	return 0;
}

static void cdns_excelsior_load_top_fw(struct cdns_excelsior_phy *cdns_phy,
				       const u32 *fw, u32 size)
{
	struct regmap *regmap = cdns_phy->regmap_phy_pcs_common_cdb;
	int i;

	cdns_excelsior_reg_write(regmap, UC_TOP_SRAM_ADDR, 0x00000000);

	for (i = 0; i < size; i += 4)
		cdns_excelsior_reg_write(regmap, UC_TOP_SRAM_ACCESS, *fw++);
}

static void cdns_excelsior_load_lane_fw(struct cdns_excelsior_phy *cdns_phy,
					const u32 *fw, u32 size)
{
	// Lane 0
	struct regmap *regmap = cdns_phy->regmap_phy_pma_lane_cdb[0];
	int i;

	cdns_excelsior_reg_write(regmap, UC_LN_SRAM_ADDR, 0x00000000);

	for (i = 0; i < size; i += 4)
		cdns_excelsior_reg_write(regmap, UC_LN_SRAM_ACCESS, *fw++);
}

static int cdns_excelsior_load_firmware(struct cdns_excelsior_phy *cdns_phy)
{
	const struct firmware *fw;
	const u32 *top_fw, *ln_fw;
	const char *fw_name;
	int ret;

	/* Top FW */
	fw_name = TOP_FW;
	dev_info(cdns_phy->dev, "Loading top UC firmware \"%s\"\n", fw_name);

	ret = request_firmware(&fw, fw_name, cdns_phy->dev);
	if (ret < 0) {
		dev_err(cdns_phy->dev, "failed to get firmware %s, ret: %d\n", fw_name, ret);
		return ret;
	}

	top_fw = (const u32 *)fw->data;
	cdns_excelsior_load_top_fw(cdns_phy, top_fw, fw->size);

	release_firmware(fw);

	/* Lane FW */
	fw_name = LANE_FW;
	dev_info(cdns_phy->dev, "Loading lane UC firmware \"%s\"\n", fw_name);

	ret = request_firmware(&fw, fw_name, cdns_phy->dev);
	if (ret < 0) {
		dev_err(cdns_phy->dev, "failed to get firmware %s, ret: %d\n", fw_name, ret);
		return ret;
	}

	ln_fw = (const u32 *)fw->data;
	cdns_excelsior_load_lane_fw(cdns_phy, ln_fw, fw->size);

	release_firmware(fw);

	return 0;
}

static int cdns_excelsior_uc_startup(struct cdns_excelsior_phy *cdns_phy)
{
	u32 read_val;
	int ret;

	/* Enable top uc */
	regmap_field_write(cdns_phy->uc_top_ctrl_stall_run, 0);

	/* Wait for PHY uC initialization complete */
	ret = regmap_field_read_poll_timeout(cdns_phy->phy_ctrl_sts_uc_init_cmpl,
					     read_val, read_val, 0, POLL_TIMEOUT_US);
	if (ret) {
		dev_err(cdns_phy->dev, "Timeout waiting for uC init complete\n");
		return ret;
	}

	/* Take the PHY out of reset */
	return reset_control_deassert(cdns_phy->phy_rst);
}

static int cdns_excelsior_phy_probe(struct platform_device *pdev)
{
	const struct cdns_excelsior_init_data *data;
	struct cdns_excelsior_phy *cdns_phy;
	enum cdns_excelsior_ssc_mode ssc_mode;
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct device_node *child;
	int i, ret, node = 0;

	/* Get init data for this PHY */
	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	cdns_phy = devm_kzalloc(dev, sizeof(*cdns_phy), GFP_KERNEL);
	if (!cdns_phy)
		return -ENOMEM;

	dev_set_drvdata(dev, cdns_phy);
	cdns_phy->dev = dev;
	cdns_phy->init_data = data;

	cdns_phy->sd_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cdns_phy->sd_base))
		return PTR_ERR(cdns_phy->sd_base);

	if (of_get_available_child_count(dev->of_node) == 0) {
		dev_err(dev, "No available phy subnodes found\n");
		return -EINVAL;
	}

	mutex_init(&cdns_phy->api_cmd_mutex);

	ret = cdns_excelsior_regmap_init(cdns_phy);
	if (ret)
		return ret;

	ret = cdns_excelsior_regfield_init(cdns_phy);
	if (ret)
		return ret;

	ret = cdns_excelsior_reset(cdns_phy);
	if (ret)
		return ret;

	ret = cdns_excelsior_clk(cdns_phy);
	if (ret)
		return ret;

	/* Get SSC mode */
	of_property_read_u32(dev->of_node, "cdns,ssc-mode", &ssc_mode);
	switch (ssc_mode) {
	case CDNS_SERDES_EXTERNAL_SSC:
		cdns_phy->ssc_mode = EXTERNAL_SSC;
		break;
	case CDNS_SERDES_INTERNAL_SSC:
		cdns_phy->ssc_mode = INTERNAL_SSC;
		break;
	default:
		cdns_phy->ssc_mode = NO_SSC;
	}

	/* Enable APB */
	reset_control_deassert(cdns_phy->apb_rst);

	for_each_available_child_of_node(dev->of_node, child) {
		struct phy *gphy;

		cdns_phy->phys[node].lnk_rst = of_reset_control_array_get_exclusive(child);
		if (IS_ERR(cdns_phy->phys[node].lnk_rst)) {
			dev_err(dev, "%s: failed to get reset\n", child->full_name);
			ret = PTR_ERR(cdns_phy->phys[node].lnk_rst);
			of_node_put(child);
			goto err;
		}

		if (of_property_read_u32(child, "reg", &cdns_phy->phys[node].mlane)) {
			dev_err(dev, "%s: No \"reg\"-property.\n", child->full_name);
			ret = -EINVAL;
			of_node_put(child);
			goto put_child;
		}

		if (of_property_read_u32(child, "cdns,num-lanes",
					 &cdns_phy->phys[node].num_lanes)) {
			dev_err(dev, "%s: No \"cdns,num-lanes\"-property.\n", child->full_name);
			ret = -EINVAL;
			of_node_put(child);
			goto put_child;
		}

		if (of_property_read_u32(child, "cdns,pcie-mode",
					 &cdns_phy->phys[node].pcie_mode)) {
			dev_err(dev, "%s: No \"cdns,pcie-mode\"-property.\n", child->full_name);
			ret = -EINVAL;
			of_node_put(child);
			goto put_child;
		}

		gphy = devm_phy_create(dev, child, &cdns_excelsior_phy_ops);
		if (IS_ERR(gphy)) {
			ret = PTR_ERR(gphy);
			of_node_put(child);
			goto put_child;
		}

		cdns_phy->phys[node].phy = gphy;
		phy_set_drvdata(gphy, &cdns_phy->phys[node]);

		node++;
	}

	cdns_phy->nsubnodes = node;

	/* Start of Day PHY configuration */
	cdns_excelsior_start_of_day_config(cdns_phy);

	/* Load top and lane firmwares */
	ret = cdns_excelsior_load_firmware(cdns_phy);
	if (ret)
		goto err;

	ret = cdns_excelsior_uc_startup(cdns_phy);
	if (ret)
		goto err;

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {
		ret = PTR_ERR(phy_provider);
		goto err;
	}

	return 0;

put_child:
	node++;
err:
	for (i = 0; i < node; i++)
		reset_control_put(cdns_phy->phys[i].lnk_rst);
	reset_control_assert(cdns_phy->apb_rst);
	clk_disable_unprepare(cdns_phy->clk);

	return ret;
}

static void cdns_excelsior_phy_remove(struct platform_device *pdev)
{
	struct cdns_excelsior_phy *cdns_phy = platform_get_drvdata(pdev);
	int i;

	reset_control_assert(cdns_phy->phy_rst);
	reset_control_assert(cdns_phy->apb_rst);
	for (i = 0; i < cdns_phy->nsubnodes; i++) {
		reset_control_assert(cdns_phy->phys[i].lnk_rst);
		reset_control_put(cdns_phy->phys[i].lnk_rst);
	}

	clk_disable_unprepare(cdns_phy->clk);
}

static const struct cdns_excelsior_init_data cdns_sd2000_excelsior_pciephy_cfg = {
	.internal_ssc_ppm = SSC_CONFIG_3K_PPM,
	.disable_ln_fw_copy = false,
};

static const struct of_device_id cdns_excelsior_phy_of_match[] = {
	{
		.compatible = "cdns,sd2000-excelsior-pcie-phy",
		.data = &cdns_sd2000_excelsior_pciephy_cfg,
	},
	{}
};
MODULE_DEVICE_TABLE(of, cdns_excelsior_phy_of_match);

static struct platform_driver cdns_excelsior_phy_driver = {
	.probe	= cdns_excelsior_phy_probe,
	.remove_new = cdns_excelsior_phy_remove,
	.driver = {
		.name	= "cdns-excelsior-phy",
		.of_match_table	= cdns_excelsior_phy_of_match,
	}
};
module_platform_driver(cdns_excelsior_phy_driver);

MODULE_AUTHOR("Swapnil Jakhade <sjakhade@cadence.com>");
MODULE_DESCRIPTION("Cadence Excelsior PHY driver");
MODULE_LICENSE("GPL v2");
