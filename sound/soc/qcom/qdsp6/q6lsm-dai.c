// SPDX-License-Identifier: GPL-2.0-only
/*
 * QDSP6 Listen front end: LSM1, a hostless capture DAI.
 *
 * The aDSP's Listen session (q6lsm, driven from userspace through /dev/q6lsm) connects to an AFE port and hears
 * the microphone there; nothing in the CPU needs its audio. The port, though, runs only while a frontend is routed
 * to it. Opening this DAI's PCM (16 kHz mono, never read: no data ever reaches it) starts the backends it is routed
 * to with the "LSM1 Mixer" controls, as CS-VOICE does for calls (q6voice-dai), so the microphone path and the AFE
 * port run for the DSP alone while the CPU sleeps.
 */
#include <linux/bits.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <dt-bindings/sound/qcom,q6afe.h>

#define DRV_NAME "q6lsm-dai"

struct q6lsm_dai {
	unsigned long routed;	/* bit n: the n-th "LSM1 Mixer" port is on */
};

static struct snd_soc_dai_driver q6lsm_dais[] = {
	{
		.id = 0,
		.name = "LSM1",
		.capture = {
			.stream_name =	"LSM1 Capture",
			.formats =	SNDRV_PCM_FMTBIT_S16_LE,
			.rates =	SNDRV_PCM_RATE_16000,
			.rate_min =	16000,
			.rate_max =	16000,
			.channels_min =	1,
			.channels_max =	1,
		},
	},
};

/* Never transferred: the sizes only have to be valid for the PCM to open */
static const struct snd_pcm_hardware q6lsm_dai_hardware = {
	.info =			SNDRV_PCM_INFO_INTERLEAVED,
	.buffer_bytes_max =	4096 * 2,
	.period_bytes_min =	2048,
	.period_bytes_max =	4096,
	.periods_min =		2,
	.periods_max =		4,
	.fifo_size =		0,
};

static int q6lsm_dai_open(struct snd_soc_component *component, struct snd_pcm_substream *substream)
{
	substream->runtime->hw = q6lsm_dai_hardware;
	return 0;
}

static int q6lsm_get_mixer(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct q6lsm_dai *d = snd_soc_component_get_drvdata(snd_soc_dapm_to_component(dapm));
	struct soc_mixer_control *mc = (struct soc_mixer_control *)kcontrol->private_value;

	ucontrol->value.integer.value[0] = test_bit(mc->shift, &d->routed);
	return 0;
}

static int q6lsm_put_mixer(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct q6lsm_dai *d = snd_soc_component_get_drvdata(snd_soc_dapm_to_component(dapm));
	struct soc_mixer_control *mc = (struct soc_mixer_control *)kcontrol->private_value;
	bool val = !!ucontrol->value.integer.value[0];

	if (test_bit(mc->shift, &d->routed) == val)
		return 0;
	assign_bit(mc->shift, &d->routed, val);
	snd_soc_dapm_mixer_update_power(dapm, kcontrol, val, NULL);
	return 1;
}

/* The ports a Listen session can hear: the WCD codec's (TX_CODEC_DMA_TX_3) and the VA macro's */
static const struct snd_kcontrol_new lsm1_mixer_controls[] = {
	SOC_SINGLE_EXT("TX_CODEC_DMA_TX_3", TX_CODEC_DMA_TX_3, 0, 1, 0, q6lsm_get_mixer, q6lsm_put_mixer),
	SOC_SINGLE_EXT("VA_CODEC_DMA_TX_0", VA_CODEC_DMA_TX_0, 1, 1, 0, q6lsm_get_mixer, q6lsm_put_mixer),
};

static const struct snd_soc_dapm_widget q6lsm_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_OUT("LSM1_UL", "LSM1 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MIXER("LSM1 Mixer", SND_SOC_NOPM, 0, 0,
			   lsm1_mixer_controls, ARRAY_SIZE(lsm1_mixer_controls)),
};

static const struct snd_soc_dapm_route q6lsm_dapm_routes[] = {
	{ "LSM1 Mixer",	"TX_CODEC_DMA_TX_3",	"TX_CODEC_DMA_TX_3" },
	{ "LSM1 Mixer",	"VA_CODEC_DMA_TX_0",	"VA_CODEC_DMA_TX_0" },
	{ "LSM1_UL",	NULL,			"LSM1 Mixer" },
};

static unsigned int q6lsm_reg_read(struct snd_soc_component *component, unsigned int reg)
{
	return 0;
}

static int q6lsm_reg_write(struct snd_soc_component *component, unsigned int reg, unsigned int val)
{
	return 0;
}

static const struct snd_soc_component_driver q6lsm_dai_component = {
	.name = DRV_NAME,
	.open = q6lsm_dai_open,
	.dapm_widgets = q6lsm_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(q6lsm_dapm_widgets),
	.dapm_routes = q6lsm_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(q6lsm_dapm_routes),
	.read = q6lsm_reg_read,
	.write = q6lsm_reg_write,
	/* Its routes name q6afe's port widgets */
	.probe_order = SND_SOC_COMP_ORDER_LATE,
};

static int q6lsm_dai_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct q6lsm_dai *d;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	dev_set_drvdata(dev, d);

	return devm_snd_soc_register_component(dev, &q6lsm_dai_component, q6lsm_dais, ARRAY_SIZE(q6lsm_dais));
}

static const struct of_device_id q6lsm_dai_device_id[] = {
	{ .compatible = "qcom,q6lsm-dais" },
	{},
};
MODULE_DEVICE_TABLE(of, q6lsm_dai_device_id);

static struct platform_driver q6lsm_dai_platform_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = q6lsm_dai_device_id,
	},
	.probe = q6lsm_dai_probe,
};
module_platform_driver(q6lsm_dai_platform_driver);

MODULE_DESCRIPTION("QDSP6 Listen hostless frontend (LSM1)");
MODULE_LICENSE("GPL");
