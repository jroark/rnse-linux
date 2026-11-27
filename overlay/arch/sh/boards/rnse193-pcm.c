// SPDX-License-Identifier: GPL-2.0
/* R8A77730 SSI playback. Each DMA word is a pair of big-endian samples.
 * Channel 3 alternates two register sets; refill the retired set on TE. */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/platform_data/rnse.h>
#include <linux/sh_intc.h>
#include <sound/core.h>
#include <sound/pcm.h>

#define ADMA 0xff801180
#define COMMON 0xff801400
#define SSI 0xff984000
#define SETUP 0x000bc532
#define AUDIO_CLOCK 0xff972000
#define AUDIO_KEY 0xff900864
#define AUDIO_MODULES 0xff9900b8
#define AUDIO_PORT 0xff909000
#define AUDIO_MUTE_DIR 0xff93e004
#define AUDIO_MUTE 0xff93e008

static u32 rd(unsigned long reg)
{
	return __raw_readl((void __iomem *)reg);
}
static void wr(unsigned long reg, u32 value)
{
	__raw_writel(value, (void __iomem *)reg);
}

static struct snd_pcm_substream *playing;
static unsigned int pos, next;
static DEFINE_SPINLOCK(lock);

void rnse_media_quiesce(void)
{
	wr(COMMON + 0x18, rd(COMMON + 0x18) & ~BIT(6));
	wr(ADMA + 0x30, 1);
	wr(ADMA + 0x2c, 0);
	wr(SSI, SETUP);
	wr(COMMON + 0x14, BIT(6));
	playing = NULL;
}

static const struct snd_pcm_hardware hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_BE, .rates = SNDRV_PCM_RATE_44100,
	.rate_min = 44100, .rate_max = 44100, .channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = 65536, .period_bytes_min = 2048, .period_bytes_max = 16384,
	.periods_min = 2, .periods_max = 16,
};

static int pcm_open(struct snd_pcm_substream *sub)
{
	sub->runtime->hw = hw;
	return snd_pcm_hw_constraint_integer(sub->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
}

static int pcm_close(struct snd_pcm_substream *sub)
{
	synchronize_irq(evt2irq(0x980));
	return 0;
}

static int prepare(struct snd_pcm_substream *sub)
{
	if ((rd(AUDIO_MODULES) & 0x60) != 0x60) {
		wr(AUDIO_CLOCK, 0x3c00);
		__raw_writew(0x12, (void __iomem *)0xff973000);
		if (!(rd(AUDIO_KEY) & 1))
			wr(AUDIO_KEY, 0xf0a50f5a);
		wr(AUDIO_MODULES, rd(AUDIO_MODULES) | 0x40);
		wr(0xff986000, 0x0009d533);
		wr(AUDIO_MODULES, rd(AUDIO_MODULES) | 0x20);
		wr(0xff985000, 0x00091100);
		wr(0xff986010, 1);
		wr(0xff986014, 0x20);
		wr(0xff986018, 0x20);
		wr(0xff986008, 0);
		wr(0xff986004, 0);
		wr(AUDIO_MUTE_DIR, rd(AUDIO_MUTE_DIR) | 0x2000);
		wr(AUDIO_MUTE, rd(AUDIO_MUTE) | 0x2000);
	}
	if (!(rd(AUDIO_KEY) & 1))
		wr(AUDIO_KEY, 0xf0a50f5a);
	wr(AUDIO_PORT + 4, rd(AUDIO_PORT + 4) | 4);
	wr(AUDIO_PORT, rd(AUDIO_PORT) & ~BIT(29));
	msleep(1);
	wr(AUDIO_PORT, rd(AUDIO_PORT) | BIT(29));
	msleep(1);
	wr(AUDIO_MODULES, rd(AUDIO_MODULES) | 0x10);
	wr(0xff90301c, 0x1b);
	wr(AUDIO_CLOCK, 0x3dff);
	wr(AUDIO_MUTE, rd(AUDIO_MUTE) & ~0x2000);
	return 0;
}

static void load_period(struct snd_pcm_runtime *rt, unsigned int set)
{
	unsigned long base = ADMA + set * 12;

	wr(base, rt->dma_addr + frames_to_bytes(rt, next));
	wr(base + 4, SSI + 8);
	wr(base + 8, rt->period_size);
	next = (next + rt->period_size) % rt->buffer_size;
}

static int trigger(struct snd_pcm_substream *sub, int cmd)
{
	unsigned long flags;

	spin_lock_irqsave(&lock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		rnse_media_quiesce();
		playing = sub;
		pos = next = 0;
		wr(SSI + 4, 0);
		wr(ADMA + 0x28, 0x05050022);
		load_period(sub->runtime, 0);
		wr(ADMA + 0x2c, 1);
		load_period(sub->runtime, 1);
		wr(ADMA + 0x2c, 3);
		wr(COMMON + 0x18, rd(COMMON + 0x18) | BIT(6));
		wr(SSI, SETUP | 0x10000001);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		rnse_media_quiesce();
		break;
	default:
		spin_unlock_irqrestore(&lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&lock, flags);
	return 0;
}

static irqreturn_t period_irq(int irq, void *data)
{
	struct snd_pcm_substream *sub;
	u32 status;

	spin_lock(&lock);
	if (!(rd(COMMON + 0x10) & BIT(6))) {
		spin_unlock(&lock);
		return IRQ_NONE;
	}
	wr(COMMON + 0x14, BIT(6));
	sub = playing;
	status = rd(ADMA + 0x34);
	if (sub && (status & 1)) {
		struct snd_pcm_runtime *rt = sub->runtime;

		pos = (pos + rt->period_size) % rt->buffer_size;
		load_period(rt, !!(status & 0x40));
		wr(ADMA + 0x2c, 3);
	}
	spin_unlock(&lock);
	if (sub) {
		if (status & 1)
			snd_pcm_period_elapsed(sub);
		else
			snd_pcm_stop_xrun(sub);
	}
	return IRQ_HANDLED;
}

static snd_pcm_uframes_t pointer(struct snd_pcm_substream *sub)
{
	return READ_ONCE(pos);
}

static const struct snd_pcm_ops ops = {
	.open = pcm_open, .close = pcm_close, .prepare = prepare,
	.trigger = trigger, .pointer = pointer,
};

static int __init pcm_init(void)
{
	const struct platform_device_info info = {
		.name = "rnse193-ssi", .id = -1, .dma_mask = DMA_BIT_MASK(32),
	};
	struct platform_device *dev = platform_device_register_full(&info);
	struct snd_card *card;
	struct snd_pcm *pcm;
	int err;

	if (IS_ERR(dev))
		return PTR_ERR(dev);
	err = snd_devm_card_new(&dev->dev, -1, "RNSE", THIS_MODULE, 0, &card);
	if (err)
		goto fail;
	strscpy(card->driver, "rnse193-ssi");
	strscpy(card->shortname, "RNS-E 193");
	strscpy(card->longname, "RNS-E 193 SSI playback");
	err = snd_pcm_new(card, "SSI", 0, 1, 0, &pcm);
	if (err)
		goto fail;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &dev->dev, 65536, 65536);
	err = devm_request_irq(&dev->dev, evt2irq(0x980), period_irq, 0, "rnse193-ssi", card);
	if (!err)
		err = snd_card_register(card);
	if (!err)
		return 0;
fail:
	platform_device_unregister(dev);
	return err;
}
device_initcall(pcm_init);
