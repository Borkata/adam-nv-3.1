/*
 * tegra_alc5623.c - Tegra machine ASoC driver for boards using ALC5623 codec.
 *
 * Author: Jason Stern
 *
 * Based on code copyright/by:
 *
 * Author: Stephen Warren <swarren@nvidia.com>
 * Copyright (C) 2010-2011 - NVIDIA, Inc.
 *
 * (c) 2009, 2010 Nvidia Graphics Pvt. Ltd.
 *
 * Copyright 2007 Wolfson Microelectronics PLC.
 * Author: Graeme Gregory
 *         graeme.gregory@wolfsonmicro.com or linux@wolfsonmicro.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <asm/mach-types.h>

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif

#include <mach/tegra_alc5623_pdata.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/alc5623.h"

#include "tegra_pcm.h"
#include "tegra_asoc_utils.h"

#define DRV_NAME "tegra-snd-alc5623"

#define GPIO_SPKR_EN    BIT(0)
#define GPIO_HP_MUTE    BIT(1)
#define GPIO_INT_MIC_EN BIT(2)
#define GPIO_EXT_MIC_EN BIT(3)
#define GPIO_HP_DET     BIT(4)

struct tegra_alc5623 {
	struct tegra_asoc_utils_data util_data;
	struct tegra_alc5623_platform_data *pdata;
	struct regulator *spk_reg;
	struct regulator *dmic_reg;
	int gpio_requested;
	bool swap_channels;
#ifdef CONFIG_SWITCH
	int jack_status;
#endif
};

static int tegra_alc5623_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_alc5623 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_alc5623_platform_data *pdata = machine->pdata;
	int srate, mclk;
	int err;

	srate = params_rate(params);
	switch (srate) {
	case 64000:
	case 88200:
	case 96000:
		mclk = 128 * srate;
		break;
	default:
		mclk = 256 * srate;
		break;
	}
	/* FIXME: Codec only requires >= 3MHz if OSR==0 */
	while (mclk < 6000000)
		mclk *= 2;

	err = tegra_asoc_utils_set_rate(&machine->util_data, srate, mclk);
	if (err < 0) {
		dev_err(card->dev, "Can't configure clocks\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(codec_dai,
				SND_SOC_DAIFMT_I2S |
		(machine->swap_channels? SND_SOC_DAIFMT_NB_IF : SND_SOC_DAIFMT_NB_NF) |
				SND_SOC_DAIFMT_CBS_CFS);
	if (err < 0) {
		dev_err(card->dev, "codec_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_fmt(cpu_dai,
					SND_SOC_DAIFMT_I2S |
					SND_SOC_DAIFMT_NB_NF |
					SND_SOC_DAIFMT_CBS_CFS);
	if (err < 0) {
		dev_err(card->dev, "cpu_dai fmt not set\n");
		return err;
	}

	err = snd_soc_dai_set_sysclk(codec_dai, 0, mclk,
					SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev, "codec_dai clock not set\n");
		return err;
	}

	return 0;
}

static struct snd_soc_ops tegra_alc5623_ops = {
	.hw_params = tegra_alc5623_hw_params,
};

static struct snd_soc_ops tegra_voice_ops = {
	.hw_params = tegra_alc5623_hw_params,
};

static int tegra_spdif_hw_params(struct snd_pcm_substream *substream,
                                        struct snd_pcm_hw_params *params)
{
        return 0;
}

static int tegra_voice_hw_params(struct snd_pcm_substream *substream,
                                        struct snd_pcm_hw_params *params)
{
        return 0;
}

static struct snd_soc_ops tegra_spdif_ops = {
	.hw_params = tegra_spdif_hw_params,
};

//static struct snd_soc_ops tegra_voice_ops = {
//	.hw_params = tegra_spdif_hw_params,
//};

static struct snd_soc_jack tegra_alc5623_hp_jack;

static struct snd_soc_jack_gpio tegra_alc5623_hp_jack_gpio = {
	.name = "headphone detect",
	.report = SND_JACK_HEADPHONE,
	.debounce_time = 150,
	.invert = 1,
};

#ifdef CONFIG_SWITCH
/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET = (1 << 0),
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static struct switch_dev tegra_alc5623_headset_switch = {
	.name = "h2w",
};

static int tegra_alc5623_jack_notifier(struct notifier_block *self,
			      unsigned long action, void *dev)
{
          
	struct snd_soc_jack *jack = dev;
	struct snd_soc_codec *codec = jack->codec;
	struct snd_soc_card *card = codec->card;
	struct tegra_alc5623 *machine = snd_soc_card_get_drvdata(card);
	enum headset_state state = BIT_NO_HEADSET;

	if (jack == &tegra_alc5623_hp_jack) {
		machine->jack_status &= ~SND_JACK_HEADPHONE;
		machine->jack_status |= (action & SND_JACK_HEADPHONE);
	} else {
		machine->jack_status &= ~SND_JACK_MICROPHONE;
		machine->jack_status |= (action & SND_JACK_MICROPHONE);
	}

	switch (machine->jack_status) {
	case SND_JACK_HEADPHONE:
		state = BIT_HEADSET_NO_MIC;
		break;
	case SND_JACK_HEADSET:
		state = BIT_HEADSET;
		break;
	case SND_JACK_MICROPHONE:
		/* mic: would not report */
	default:
		state = BIT_NO_HEADSET;
	}

	switch_set_state(&tegra_alc5623_headset_switch, state);

	return NOTIFY_OK;
}

static struct notifier_block tegra_alc5623_jack_detect_nb = {
	.notifier_call = tegra_alc5623_jack_notifier,
};
#else
static struct snd_soc_jack_pin tegra_alc5623_hp_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

#endif

static int tegra_alc5623_event_pre_channel(struct snd_soc_dapm_widget *w,
                                        struct snd_kcontrol *k, int event)
{
        struct snd_soc_dapm_context *dapm = w->dapm;
        struct snd_soc_card *card = dapm->card;
        struct snd_soc_codec *codec = dapm->codec;
        struct tegra_alc5623 *machine = snd_soc_card_get_drvdata(card);
        struct tegra_alc5623_platform_data *pdata = machine->pdata;

#ifdef CONFIG_SWITCH
	machine->swap_channels = (machine->jack_status == SND_JACK_HEADPHONE) ||
				 (machine->jack_status == SND_JACK_HEADSET);
#else
	machine->swap_channels = (bool) snd_soc_dapm_get_pin_status(dapm,"Headphone Jack");

#endif
	return 0;
}
static int tegra_alc5623_event_int_spk(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct snd_soc_codec *codec = dapm->codec;
	struct tegra_alc5623 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_alc5623_platform_data *pdata = machine->pdata;
	static bool firstEnable = true;
	if (machine->spk_reg) {
		if (SND_SOC_DAPM_EVENT_ON(event))
			regulator_enable(machine->spk_reg);
		else
			regulator_disable(machine->spk_reg);
	}


	/*
	*  Manage both amps in the Adam board.
	*  Don't turn them on the first enable of the internal speaker.
	*  This is to prevent the whine that results before the
	*  userspace lib can set up the audio pathing.
	*/
	if (firstEnable == true && !!SND_SOC_DAPM_EVENT_ON(event)) {
		firstEnable = false;
	}
	else {

		// External Amp GPIO
        	snd_soc_update_bits(codec, ALC5623_GPIO_OUTPUT_PIN_CTRL,
                	        ALC5623_GPIO_OUTPUT_GPIO_OUT_STATUS,
                        	(!!SND_SOC_DAPM_EVENT_ON(event))*ALC5623_GPIO_OUTPUT_GPIO_OUT_STATUS);

		// ALC5623 Internal Auxout Amp
		snd_soc_update_bits(codec, ALC5623_PWR_MANAG_ADD1,
        	                ALC5623_PWR_ADD1_AUX_OUT_AMP,
                	        (!!SND_SOC_DAPM_EVENT_ON(event))*ALC5623_PWR_ADD1_AUX_OUT_AMP);
	}

	if (!(machine->gpio_requested & GPIO_SPKR_EN)) {
		return 0;
	}
	gpio_set_value_cansleep(pdata->gpio_spkr_en,
				SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

#if 0
static int tegra_alc5623_event_hp(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *k, int event)
{
        pr_info("%s++", __func__); 
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct snd_soc_codec *codec = dapm->codec;
	struct tegra_alc5623 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_alc5623_platform_data *pdata = machine->pdata;

	snd_soc_update_bits(codec, ALC5623_DAI_CONTROL,
                        ALC5623_DAI_DAC_DATA_L_R_SWAP,
                        (!!SND_SOC_DAPM_EVENT_ON(event))*ALC5623_DAI_DAC_DATA_L_R_SWAP);

	return 0;
}
#endif 

static int tegra_alc5623_event_int_mic(struct snd_soc_dapm_widget *w,
                                        struct snd_kcontrol *k, int event)
{
        struct snd_soc_dapm_context *dapm = w->dapm;
        struct snd_soc_card *card = dapm->card;
        struct tegra_alc5623 *machine = snd_soc_card_get_drvdata(card);
        struct tegra_alc5623_platform_data *pdata = machine->pdata;
	struct snd_soc_codec *codec = dapm->codec;

        if (machine->dmic_reg) {
                if (SND_SOC_DAPM_EVENT_ON(event))
                        regulator_enable(machine->dmic_reg);
                else
                        regulator_disable(machine->dmic_reg);
        }

        if (!(machine->gpio_requested & GPIO_INT_MIC_EN))
                return 0;

 	// Enables the mic differntial control
        snd_soc_update_bits(codec, ALC5623_MIC_ROUTING_CTRL,
                        (1 << 12),
                        (!!SND_SOC_DAPM_EVENT_ON(event))*(1<<12));
	// Mic Bias
//	snd_soc_update_bits(codec, ALC5623_PWR_MANAG_ADD1,
//			(1 << 11),
//		    (!!SND_SOC_DAPM_EVENT_ON(event))*(1<<11));


        gpio_set_value_cansleep(pdata->gpio_int_mic_en,
                                SND_SOC_DAPM_EVENT_ON(event));
	printk("%s: Changing mic gpio to: %d\n", __func__, SND_SOC_DAPM_EVENT_ON(event));
        return 0;
}



#ifdef CONFIG_MACH_ADAM
static const struct snd_soc_dapm_widget dapm_widgets[] = {
	SND_SOC_DAPM_PRE("Channel Swap Detect", tegra_alc5623_event_pre_channel),
	SND_SOC_DAPM_SPK("Int Spk", tegra_alc5623_event_int_spk),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Int Mic", tegra_alc5623_event_int_mic),
	SND_SOC_DAPM_LINE("FM Radio", NULL),
};

static const struct snd_soc_dapm_route audio_map[] = {
	{"Headphone Jack", NULL, "HPR"},
	{"Headphone Jack", NULL, "HPL"},
	{"Int Spk", NULL, "AUXOUTR"},
	{"Int Spk", NULL, "AUXOUTL"},
        {"Mic Bias1", NULL, "Int Mic"},
        {"MIC1", NULL, "Mic Bias1"},
	{"AUXINR", NULL, "FM Radio"},
	{"AUXINL", NULL, "FM Radio"},
};


static const struct snd_kcontrol_new controls[] = {
	SOC_DAPM_PIN_SWITCH("Int Spk"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
	SOC_DAPM_PIN_SWITCH("FM"),
	SOC_DAPM_PIN_SWITCH("Mic Bias1"),
};

static const char* nc_pins[] = {
	"SPKOUT",
	"SPKOUTN",
	"LINEINL",
	"LINEINR",
	"MIC2",
};
#endif

static int tegra_alc5623_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->card;
	struct tegra_alc5623 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_alc5623_platform_data *pdata = machine->pdata;
	int ret, i;

	if (gpio_is_valid(pdata->gpio_spkr_en)) {
		ret = gpio_request(pdata->gpio_spkr_en, "spkr_en");
		if (ret) {
			dev_err(card->dev, "cannot get spkr_en gpio\n");
			return ret;
		}
		machine->gpio_requested |= GPIO_SPKR_EN;

		gpio_direction_output(pdata->gpio_spkr_en, 0);
	} else if(pdata->gpio_spkr_en == -2) {
          	snd_soc_update_bits(codec, ALC5623_GPIO_PIN_CONFIG,
                	ALC5623_GPIO_PIN_CONFIG_GPIO_CONF,
                        0);
	}

        if (gpio_is_valid(pdata->gpio_int_mic_en)) {
                ret = gpio_request(pdata->gpio_int_mic_en, "int_mic_en");
                if (ret) {
                        dev_err(card->dev, "cannot get int_mic_en gpio\n");
                        return ret;
                }
                machine->gpio_requested |= GPIO_INT_MIC_EN;

                /* Disable int mic; enable signal is active-high */
                gpio_direction_output(pdata->gpio_int_mic_en, 0);
        }

	ret = snd_soc_add_controls(codec, controls,
			ARRAY_SIZE(controls));

	if (ret < 0)
		return ret;

	snd_soc_dapm_new_controls(dapm, dapm_widgets,
			ARRAY_SIZE(dapm_widgets));

	snd_soc_dapm_add_routes(dapm, audio_map,
				ARRAY_SIZE(audio_map));


	if (gpio_is_valid(pdata->gpio_hp_det)) {
		tegra_alc5623_hp_jack_gpio.gpio = pdata->gpio_hp_det;
		snd_soc_jack_new(codec, "Headphone Jack", SND_JACK_HEADPHONE,
				&tegra_alc5623_hp_jack);
#ifndef CONFIG_SWITCH
		snd_soc_jack_add_pins(&tegra_alc5623_hp_jack,
					ARRAY_SIZE(tegra_alc5623_hp_jack_pins),
					tegra_alc5623_hp_jack_pins);
#else
		snd_soc_jack_notifier_register(&tegra_alc5623_hp_jack,
					&tegra_alc5623_jack_detect_nb);
#endif
		snd_soc_jack_add_gpios(&tegra_alc5623_hp_jack,
					1,
					&tegra_alc5623_hp_jack_gpio);
		machine->gpio_requested |= GPIO_HP_DET;
	}

	snd_soc_dapm_force_enable_pin(dapm, "Mic Bias1");

	for(i = 0; i < ARRAY_SIZE(nc_pins); i++) {
		snd_soc_dapm_nc_pin(dapm, nc_pins[i]);
	}

	snd_soc_dapm_sync(dapm);

	return 0;
}
static int alc5623_pre_resume(struct snd_soc_card *card) {

	/* Delay on resume because otherwise a paging error 
	 * occurs. There is probably a deeper problem here,
	 * but this works.
	 */

	msleep(100);
	return 0;
}


static struct snd_soc_dai_link tegra_alc5623_dai[] = {
	{
		.name = "ALC5623",
		.stream_name = "ALC5623 PCM",
		.codec_name = "alc562x-codec.0-001a",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-i2s.0",
		.codec_dai_name = "alc5623-hifi",
		.init = tegra_alc5623_init,
		.ops = &tegra_alc5623_ops,
	},
	{
		.name = "VOICE",
		.stream_name = "Tegra Generic Voice",
		.codec_name = "tegra-generic-codec",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-i2s.1",
		.codec_dai_name = "tegra_generic_voice_codec",
		.ops = &tegra_voice_ops,
	},
	{
		.name = "SPDIF",
		.stream_name = "SPDIF PCM",
		.codec_name = "spdif-dit.0",
		.platform_name = "tegra-pcm-audio",
		.cpu_dai_name = "tegra20-spdif",
		.codec_dai_name = "dit-hifi",
		.ops = &tegra_spdif_ops,
	}
};


static struct snd_soc_card snd_soc_tegra_alc5623 = {
	.name = "tegra-alc5623",
	.dai_link = tegra_alc5623_dai,
	.num_links = ARRAY_SIZE(tegra_alc5623_dai),
	.resume_pre = alc5623_pre_resume,
};

static __devinit int tegra_alc5623_driver_probe(struct platform_device *pdev)
{
          
	struct snd_soc_card *card = &snd_soc_tegra_alc5623;
	struct tegra_alc5623 *machine;
	struct tegra_alc5623_platform_data *pdata;
	int ret;

	pdata = pdev->dev.platform_data;
	if (!pdata) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}

	machine = kzalloc(sizeof(struct tegra_alc5623), GFP_KERNEL);
	if (!machine) {
		dev_err(&pdev->dev, "Can't allocate tegra_alc5623 struct\n");
		return -ENOMEM;
	}

	machine->pdata = pdata;

	ret = tegra_asoc_utils_init(&machine->util_data, &pdev->dev);
	if (ret)
		goto err_free_machine;

//	machine->spk_reg = regulator_get(&pdev->dev, "vdd_spk_amp");
//	if (IS_ERR(machine->spk_reg)) {
//		dev_info(&pdev->dev, "No speaker regulator found\n");
		machine->spk_reg = 0;
//	}
//
//	machine->dmic_reg = regulator_get(&pdev->dev, "vdd_dmic");
//	if (IS_ERR(machine->dmic_reg)) {
//		dev_info(&pdev->dev, "No digital mic regulator found\n");
		machine->dmic_reg = 0;
//	}

	machine->swap_channels = false;
#ifdef CONFIG_SWITCH
	/* Addd h2w swith class support */
	ret = switch_dev_register(&tegra_alc5623_headset_switch);
	if (ret < 0)
		goto err_fini_utils;
#endif

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, machine);

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n",
			ret);
		goto err_unregister_switch;
	}

	return 0;

err_unregister_switch:
#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_alc5623_headset_switch);
#endif
err_fini_utils:
	tegra_asoc_utils_fini(&machine->util_data);
err_free_machine:
	kfree(machine);
	return ret;
}

static int __devexit tegra_alc5623_driver_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct tegra_alc5623 *machine = snd_soc_card_get_drvdata(card);
	struct tegra_alc5623_platform_data *pdata = machine->pdata;

	if (machine->gpio_requested & GPIO_HP_DET)
		snd_soc_jack_free_gpios(&tegra_alc5623_hp_jack,
					1,
					&tegra_alc5623_hp_jack_gpio);
	if (machine->gpio_requested & GPIO_SPKR_EN)
		gpio_free(pdata->gpio_spkr_en);
        if (machine->gpio_requested & GPIO_INT_MIC_EN)
                gpio_free(pdata->gpio_int_mic_en);
	machine->gpio_requested = 0;

	if (machine->spk_reg)
		regulator_put(machine->spk_reg);
	if (machine->dmic_reg)
		regulator_put(machine->dmic_reg);

	snd_soc_unregister_card(card);

	tegra_asoc_utils_fini(&machine->util_data);

#ifdef CONFIG_SWITCH
	switch_dev_unregister(&tegra_alc5623_headset_switch);
#endif
	kfree(machine);

	return 0;
}

static struct platform_driver tegra_alc5623_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
	},
	.probe = tegra_alc5623_driver_probe,
	.remove = __devexit_p(tegra_alc5623_driver_remove),
};

static int __init tegra_alc5623_modinit(void)
{
          
	return platform_driver_register(&tegra_alc5623_driver);
}
module_init(tegra_alc5623_modinit);

static void __exit tegra_alc5623_modexit(void)
{
	platform_driver_unregister(&tegra_alc5623_driver);
}
module_exit(tegra_alc5623_modexit);

MODULE_AUTHOR("Jason Stern");
MODULE_DESCRIPTION("Tegra+ALC5623 machine ASoC driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
