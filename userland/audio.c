/* A WAV or MP3 file to 44.1 kHz stereo, and rnse-play, which plays one.
 *
 * MP3 is decoded on the SH-4 by minimp3 (CC0; fetched and checked by
 * build.sh, not kept here).  Other rates than 44.1 kHz are made
 * 44.1 kHz by straight-line interpolation, which is enough for a car's
 * speakers and cheap for a 235 MHz processor. */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "rnse.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define OUT_RATE 44100

struct decoder {
	int fd, mp3;
	int rate, channels, bits, big_endian;
	long data_left;			/* WAV: bytes of samples left */
	/* MP3 */
	mp3dec_t mp3dec;
	uint8_t in[16384];
	int in_len, eof;
	/* What the decoder gave and has not gone out yet, stereo frames
	 * at the file's rate. */
	int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	int pcm_frames, pcm_at;
	/* Resampling: the position between the last two input frames,
	 * in 1/65536. */
	uint32_t step, frac;
	int16_t prev[2], next[2];
	int primed, ended;
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

static int fill(struct decoder *d)
{
	int n;

	if (d->eof || d->in_len == sizeof d->in)
		return 0;
	n = read(d->fd, d->in + d->in_len, sizeof d->in - d->in_len);
	if (n <= 0)
		d->eof = 1;
	else
		d->in_len += n;
	return n;
}

static int wav_header(struct decoder *d)
{
	uint8_t h[12], c[8], fmt[16];

	if (read(d->fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		return -1;
	for (;;) {
		uint32_t len;

		if (read(d->fd, c, 8) != 8)
			return -1;
		len = le32(c + 4);
		if (!memcmp(c, "fmt ", 4)) {
			if (len < 16 || read(d->fd, fmt, 16) != 16)
				return -1;
			if (le16(fmt) != 1)
				return -1;	/* PCM only */
			d->channels = le16(fmt + 2);
			d->rate = le32(fmt + 4);
			d->bits = le16(fmt + 14);
			if (len > 16)
				lseek(d->fd, (len - 16 + 1) & ~1u, SEEK_CUR);
		} else if (!memcmp(c, "data", 4)) {
			d->data_left = len;
			break;
		} else {
			lseek(d->fd, (len + 1) & ~1u, SEEK_CUR);
		}
	}
	if (d->bits != 16 || d->channels < 1 || d->channels > 2 || d->rate < 8000 || d->rate > 96000)
		return -1;
	return 0;
}

/* Skip an ID3v2 tag at the start of an MP3 file. */
static void id3_skip(struct decoder *d)
{
	uint8_t h[10];

	if (read(d->fd, h, 10) == 10 && !memcmp(h, "ID3", 3)) {
		long len = (h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f);

		lseek(d->fd, 10 + len + (h[5] & 0x10 ? 10 : 0), SEEK_SET);
	} else {
		lseek(d->fd, 0, SEEK_SET);
	}
}

struct decoder *decoder_open(const char *path)
{
	struct decoder *d = calloc(1, sizeof *d);
	const char *dot = strrchr(path, '.');

	if (!d)
		return NULL;
	d->fd = open(path, O_RDONLY);
	if (d->fd < 0)
		goto fail;
	if (dot && !strcasecmp(dot, ".mp3")) {
		d->mp3 = 1;
		mp3dec_init(&d->mp3dec);
		id3_skip(d);
		d->rate = 0;	/* from the first frame */
	} else if (wav_header(d)) {
		goto fail;
	}
	return d;
fail:
	if (d->fd >= 0)
		close(d->fd);
	free(d);
	return NULL;
}

/* The next stereo frame at the file's rate, 0 at the end. */
static int next_frame(struct decoder *d, int16_t lr[2])
{
	if (d->pcm_at >= d->pcm_frames) {
		d->pcm_at = d->pcm_frames = 0;
		if (d->mp3) {
			mp3dec_frame_info_t info;
			int16_t raw[MINIMP3_MAX_SAMPLES_PER_FRAME];
			int n, i;

			do {
				fill(d);
				if (!d->in_len)
					return 0;
				n = mp3dec_decode_frame(&d->mp3dec, d->in, d->in_len, raw, &info);
				if (info.frame_bytes) {
					d->in_len -= info.frame_bytes;
					memmove(d->in, d->in + info.frame_bytes, d->in_len);
				} else if (d->eof) {
					return 0;
				} else if (d->in_len == sizeof d->in) {
					d->in_len = 0;	/* nothing in it: drop it */
				}
			} while (!n);
			if (!d->rate) {
				d->rate = info.hz;
				d->channels = info.channels;
				d->step = (uint32_t)(((uint64_t)d->rate << 16) / OUT_RATE);
			}
			for (i = 0; i < n; i++) {
				d->pcm[2 * i] = raw[info.channels * i];
				d->pcm[2 * i + 1] = raw[info.channels * i + (info.channels > 1)];
			}
			d->pcm_frames = n;
		} else {
			uint8_t raw[2048];	/* at most 1024 frames: they fit pcm[] */
			int per = 2 * d->channels, want, got, i;

			want = sizeof raw / per * per;
			if (want > d->data_left)
				want = d->data_left / per * per;
			if (want <= 0)
				return 0;
			got = read(d->fd, raw, want);
			if (got < per)
				return 0;
			got = got / per;
			d->data_left -= got * per;
			for (i = 0; i < got; i++) {
				d->pcm[2 * i] = (int16_t)le16(raw + per * i);
				d->pcm[2 * i + 1] = (int16_t)le16(raw + per * i + (d->channels > 1 ? 2 : 0));
			}
			d->pcm_frames = got;
		}
	}
	lr[0] = d->pcm[2 * d->pcm_at];
	lr[1] = d->pcm[2 * d->pcm_at + 1];
	d->pcm_at++;
	return 1;
}

int decoder_read(struct decoder *d, int16_t *out, int max)
{
	int n = 0;

	if (d->ended)
		return 0;
	if (!d->step && d->rate)
		d->step = (uint32_t)(((uint64_t)d->rate << 16) / OUT_RATE);
	if (!d->primed) {
		if (!next_frame(d, d->prev) || !next_frame(d, d->next)) {
			d->ended = 1;
			return 0;
		}
		if (!d->step)
			d->step = (uint32_t)(((uint64_t)d->rate << 16) / OUT_RATE);
		d->primed = 1;
	}
	while (n < max) {
		int c;

		for (c = 0; c < 2; c++)
			out[2 * n + c] = d->prev[c] + (int)(((int32_t)(d->next[c] - d->prev[c]) * (int32_t)d->frac) >> 16);
		n++;
		d->frac += d->step;
		while (d->frac >= 0x10000) {
			d->frac -= 0x10000;
			d->prev[0] = d->next[0];
			d->prev[1] = d->next[1];
			if (!next_frame(d, d->next)) {
				d->ended = 1;
				return n;
			}
		}
	}
	return n;
}

void decoder_close(struct decoder *d)
{
	if (!d)
		return;
	close(d->fd);
	free(d);
}

static volatile sig_atomic_t stop;

static void on_signal(int s)
{
	stop = 1;
}

/* rnse-play FILE...: each file in turn through the card processor's
 * converter.  The amplifier's source is the radio processor's to set
 * (rnse-radio source decoder). */
int play_main(int argc, char **argv)
{
	static int16_t buf[2 * 2048];
	int fd, i, n, status = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: rnse-play FILE.wav|FILE.mp3 ...\n");
		return 2;
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	fd = pcm_open();
	if (fd < 0) {
		perror("rnse-play: /dev/snd/pcmC0D0p");
		return 1;
	}
	for (i = 1; i < argc && !stop; i++) {
		struct decoder *d = decoder_open(argv[i]);
		long t0 = now_ms(), frames = 0;

		if (!d) {
			fprintf(stderr, "rnse-play: %s: not a file to play\n", argv[i]);
			status = 1;
			continue;
		}
		while (!stop && (n = decoder_read(d, buf, 2048)) > 0) {
			if (pcm_write(fd, buf, n))
				break;
			frames += n;
		}
		fprintf(stderr, "rnse-play: %s: %ld frames in %ld ms\n", argv[i], frames, now_ms() - t0);
		decoder_close(d);
	}
	if (stop)
		close(fd);
	else
		pcm_close(fd);
	return status;
}
