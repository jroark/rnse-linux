/* User space of Linux on the Audi RNS-E 192: one static program, rnse,
 * with a name for each part (as BusyBox has): rnse-radio, rnse-play,
 * rnse-can and rnse-gui.  docs/linux.md. */
#ifndef RNSE_USER_H
#define RNSE_USER_H

#include <stdint.h>
#include <stddef.h>

/* radio.c: the radio processor on /dev/ttyRN1 (sdk/src/radio.c). */
#define RADIO_CD 1
#define RADIO_TUNER 3
#define RADIO_DECODER 4
int radio_open(void);
int radio_command(int cmd, const uint8_t *data, int n, uint8_t *answer, int max);
int radio_start(void);
int radio_source(int source, int volume);
int radio_volume(int volume);
int radio_tune(int frequency);			/* FM in 50 kHz steps: 2042 is 102.1 MHz */
int radio_status(uint8_t *status, int max);	/* 57 bytes: 4-5 frequency, 8-15 text, 33 source, 34 volume */
int radio_main(int argc, char **argv);

/* pcm.c: ALSA by its ioctls, 44.1 kHz stereo, big-endian 16 bits. */
int pcm_open(void);
int pcm_write(int fd, const int16_t *frames, int count);
void pcm_close(int fd);

/* audio.c: a WAV or MP3 file to 44.1 kHz stereo frames. */
struct decoder;
struct decoder *decoder_open(const char *path);
/* Up to max frames into out (interleaved), 0 at the end, < 0 an error. */
int decoder_read(struct decoder *d, int16_t *out, int max);
void decoder_close(struct decoder *d);
int play_main(int argc, char **argv);

/* can.c */
int can_up(void);
int can_socket(void);
int can_main(int argc, char **argv);

/* gui.c */
int gui_main(int argc, char **argv);

/* Little things. */
int read_file(const char *path, char *buf, int max);
int write_file(const char *path, const char *text);
long now_ms(void);

#endif
