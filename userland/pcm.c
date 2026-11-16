/* Sound through ALSA without its library: the ioctls of
 * /dev/snd/pcmC0D0p, as tinyalsa makes them.  The card (rnse-media in
 * the kernel) takes one format: 44.1 kHz, two channels, 16 bits
 * big-endian, the CPU's own. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <sound/asound.h>
#include "rnse.h"

#define PERIOD 2048
#define PERIODS 4

static void mask_set(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
	struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(m->bits, 0, sizeof m->bits);
	m->bits[bit >> 5] |= 1u << (bit & 31);
}

static void interval_set(struct snd_pcm_hw_params *p, int n, unsigned v)
{
	struct snd_interval *i = &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	i->min = i->max = v;
	i->integer = 1;
}

int pcm_open(void)
{
	struct snd_pcm_hw_params hw;
	struct snd_pcm_sw_params sw;
	unsigned long boundary;
	int fd, n;

	fd = open("/dev/snd/pcmC0D0p", O_RDWR);
	if (fd < 0)
		return -1;
	memset(&hw, 0, sizeof hw);
	for (n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
		memset(hw.masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits, 0xff,
		       sizeof hw.masks[0].bits);
	for (n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
		hw.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 0;
		hw.intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = ~0u;
	}
	hw.rmask = ~0u;
	hw.info = ~0u;
	mask_set(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	mask_set(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_BE);
	mask_set(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	interval_set(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	interval_set(&hw, SNDRV_PCM_HW_PARAM_RATE, 44100);
	interval_set(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
	interval_set(&hw, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw))
		goto fail;

	memset(&sw, 0, sizeof sw);
	sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
	sw.period_step = 1;
	sw.avail_min = PERIOD;
	sw.start_threshold = PERIOD * 2;
	sw.stop_threshold = PERIOD * PERIODS;
	boundary = PERIOD * PERIODS;
	while (boundary * 2 <= LONG_MAX - PERIOD * PERIODS)
		boundary *= 2;
	sw.boundary = boundary;
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw))
		goto fail;
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE))
		goto fail;
	return fd;
fail:
	n = errno;
	close(fd);
	errno = n;
	return -1;
}

int pcm_write(int fd, const int16_t *frames, int count)
{
	while (count > 0) {
		struct snd_xferi x = { .buf = (void *)frames, .frames = count };

		if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x)) {
			if (errno == EPIPE) {	/* ran dry: start again */
				ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
				continue;
			}
			if (errno == EINTR)
				continue;
			return -1;
		}
		frames += 2 * x.result;
		count -= x.result;
	}
	return 0;
}

void pcm_close(int fd)
{
	if (fd < 0)
		return;
	ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
	close(fd);
}
