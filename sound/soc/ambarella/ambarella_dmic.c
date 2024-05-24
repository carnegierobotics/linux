/*
 * sound/soc/ambarella_i2s.c
 *
 * History:
 *	2016/07/13 - [XianqingZheng] created file
 *
 * Copyright (C) 2004-2016, Ambarella, Inc.
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
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>
#include "ambarella_pcm.h"
#include "ambarella_dmic.h"

static const struct snd_soc_dapm_widget dmic_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_OUT("DMIC AIF", "Capture", 0,
			     SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_INPUT("DMic"),
};

static const struct snd_soc_dapm_route intercon[] = {
	{"DMIC AIF", NULL, "DMic"},
};

static void ambarella_dmic_enable(struct amb_dmic_priv *priv_data)
{
	writel_relaxed(1, priv_data->regbase + DMIC_ENABLE_OFFSET);
}

static void ambarella_dmic_disbale(struct amb_dmic_priv *priv_data)
{
	writel_relaxed(0, priv_data->regbase + DMIC_ENABLE_OFFSET);
}

static void ambarella_dmic_clock(struct amb_dmic_priv *priv_data, u32 mclk, u32 rate)
{
	u32 dec_factor0 = 31, dec_factor1 = 1, dec_fs, fdiv;
	u32 val;
	u32 left_phase, right_phase;

	dec_fs = priv_data->mclk/rate -1;
	fdiv = (dec_fs + 1) / ((dec_factor0 + 1) * (dec_factor1 + 1));
	dec_fs = (dec_fs << 16) | (dec_factor0 | (dec_factor1 << 8));

	writel_relaxed(dec_fs, priv_data->regbase + DECIMATION_FACTOR_OFFSET);
	writel_relaxed(fdiv, priv_data->regbase + DMIC_CLK_DIV_OFFSET);

	left_phase = fdiv/4;
	right_phase = fdiv - fdiv/4;
	val = (left_phase << 16) | right_phase;

	/* set the phase for left and right channel, according to the record parameter */
	writel_relaxed(val, priv_data->regbase + DMIC_DATA_PHASE_OFFSET);
}

static void ambarella_dmic_set_df(struct amb_dmic_priv *priv_data)
{
	u32 i;
	static u32 DroopFilterTable[8] = {
		0xFF85C000, 0xFF0C9000, 0x01CAB000, 0x04A9E000,
		0xF9A26000, 0xEF8AE000, 0x0F0B6000, 0x4381A000
	};

	for (i = 0; i < 8; i++)
		writel_relaxed(DroopFilterTable[i], priv_data->regbase + 0x200 + 4*i);
}

static void ambarella_dmic_set_hbf(struct amb_dmic_priv *priv_data)
{
	u32 i;
	static u32 HalfBandFilterTable[32] = {
		0xFFC87000, 0xFFFD2000, 0x00601000, 0x00056000,
		0xFF23F000, 0xFFF61000, 0x01CCF000, 0x000FC000,
		0xFC93E000, 0xFFEA3000, 0x06462000, 0x001B2000,
		0xF39CC000, 0xFFE14000, 0x28558000, 0x40200000,
		0x28558000, 0xFFE14000, 0xF39CC000, 0x001B2000,
		0x06462000, 0xFFEA3000, 0xFC93E000, 0x000FC000,
		0x01CCF000, 0xFFF61000, 0xFF23F000, 0x00056000,
		0x00601000, 0xFFFD2000, 0xFFC87000, 0x00000000
	};

	for (i = 0; i < 32; i++)
		writel_relaxed(HalfBandFilterTable[i], priv_data->regbase + 0x300 + 4*i);
}

static void ambarella_dmic_set_custom_iir(struct amb_dmic_priv *priv_data)
{
	u32 i;
	static u32 DcBlockTable[17] = {
		0x40000000, 0x00000000, 0x00000000, 0xC0000000,
		0x00000000, 0xC0107000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000, 0x00000000, 0x00000000, 0x00000000,
		0x00000000
	};

	for (i = 0; i < 17; i++)
		writel_relaxed(DcBlockTable[i], priv_data->regbase + 0x6d4 + 4*i);
}

static int ambarella_dmic_init(struct amb_dmic_priv *priv_data)
{
	/* Droop Setting */
	ambarella_dmic_set_df(priv_data);

	/* HBF Setting */
	ambarella_dmic_set_hbf(priv_data);

	if (priv_data->custom_iir_support)
		ambarella_dmic_set_custom_iir(priv_data);

	return 0;
}

static int ambarella_dmic_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct amb_dmic_priv *priv_data = snd_soc_dai_get_drvdata(dai);
	u32 rate;

	/*Reset the ADC Data Path*/
	writel_relaxed(0, priv_data->regbase + AUDIO_CODEC_DP_RESET_OFFSET);
	writel_relaxed(1, priv_data->regbase + AUDIO_CODEC_DP_RESET_OFFSET);
	writel_relaxed(0, priv_data->regbase + AUDIO_CODEC_DP_RESET_OFFSET);

	rate = params_rate(params);
	ambarella_dmic_clock(priv_data, priv_data->mclk, rate);
	ambarella_dmic_init(priv_data);

	return 0;
}

static int ambarella_dmic_trigger(struct snd_pcm_substream *substream, int cmd,
		struct snd_soc_dai *dai)
{
	struct amb_dmic_priv *priv_data = snd_soc_dai_get_drvdata(dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		regmap_update_bits(priv_data->reg_scr, DMIC_I2S_SEL_OFFSET, DMIC_I2S_SEL_MASK, DMIC_I2S_SEL_MASK);
		ambarella_dmic_enable(priv_data);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		regmap_update_bits(priv_data->reg_scr, DMIC_I2S_SEL_OFFSET, DMIC_I2S_SEL_MASK, 0x00000000);
		ambarella_dmic_disbale(priv_data);
		break;
	default:
		break;
	}

	return 0;
}

/*
 * Set Ambarella I2S DAI format
 */
static int ambarella_dmic_set_fmt(struct snd_soc_dai *dai,
		unsigned int fmt)
{
	return 0;
}

static int ambarella_dmic_set_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	struct amb_dmic_priv *priv_data = snd_soc_dai_get_drvdata(dai);

	priv_data->mclk = freq;
	return 0;
}

static const struct snd_soc_dai_ops ambarella_dmic_dai_ops = {
	.hw_params = ambarella_dmic_hw_params,
	.trigger = ambarella_dmic_trigger,
	.set_fmt = ambarella_dmic_set_fmt,
	.set_sysclk = ambarella_dmic_set_sysclk,
};

static struct snd_soc_dai_driver ambarella_dmic_dai = {
	.name = "dmic-hifi",
	.capture = {
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = (SNDRV_PCM_FMTBIT_S16_LE
			| SNDRV_PCM_FMTBIT_S24_LE
			| SNDRV_PCM_FMTBIT_S32_LE),
	},
	.ops = &ambarella_dmic_dai_ops,
	.symmetric_rate = 1,
};

static const struct snd_soc_component_driver ambarella_dmic_component = {
		.dapm_widgets = dmic_dapm_widgets,
		.num_dapm_widgets = ARRAY_SIZE(dmic_dapm_widgets),
		.dapm_routes = intercon,
		.num_dapm_routes = ARRAY_SIZE(intercon),
};

static int ambarella_dmic_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct amb_dmic_priv *priv_data;
	struct resource *res;

	priv_data = devm_kzalloc(&pdev->dev, sizeof(*priv_data), GFP_KERNEL);
	if (priv_data == NULL)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "No mem resource for DICI!\n");
		return -ENXIO;
	}

	priv_data->regbase = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!priv_data->regbase) {
		dev_err(&pdev->dev, "devm_ioremap() failed\n");
		return -ENOMEM;
	}

	priv_data->reg_scr = syscon_regmap_lookup_by_phandle(np, "amb,scr-regmap");
	if (IS_ERR(priv_data->reg_scr)) {
		dev_err(&pdev->dev, "no scr regmap!\n");
		return -ENXIO;
	}

	priv_data->custom_iir_support = !!of_find_property(np, "amb,custom-iir", NULL);

	dev_set_drvdata(&pdev->dev, priv_data);

	return devm_snd_soc_register_component(&pdev->dev,
			&ambarella_dmic_component, &ambarella_dmic_dai, 1);
}

static const struct of_device_id ambarella_dmic_dt_ids[] = {
	{ .compatible = "ambarella,dmic", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ambarella_dmic_dt_ids);

static struct platform_driver ambarella_dmic_driver = {
	.probe = ambarella_dmic_probe,

	.driver = {
		.name = "ambarella-dmic",
		.of_match_table = ambarella_dmic_dt_ids,
	},
};

module_platform_driver(ambarella_dmic_driver);

MODULE_AUTHOR("XianqingZheng <xqzheng@ambarella.com>");
MODULE_DESCRIPTION("Ambarella Soc DMIC Interface");

MODULE_LICENSE("GPL");

